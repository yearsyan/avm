/*
 * Driver interaction with QEMU virtio wifi
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */
#include "includes.h"

#include "common.h"
#include "driver.h"
#include "driver_virtio_wifi.h"
#include "eloop.h"
#include "ap/hostapd.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"
#include "common/wpa_common.h"

#include "string.h"

#define IEEE80211_MAX_FRAME_LEN		2352

// AEMU mutex
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef _WIN32
#define RWLOCK_TYPE SRWLOCK
static void rwlock_init(void *lock) {
  InitializeSRWLock(lock);
}
static void rwlock_write_lock(void *lock) {
  AcquireSRWLockExclusive(lock);
}
static void rwlock_write_unlock(void *lock) {
  ReleaseSRWLockExclusive(lock);
}
static void rwlock_read_lock(void *lock) {
  AcquireSRWLockShared(lock);
}
static void rwlock_read_unlock(void *lock) {
  ReleaseSRWLockShared(lock);
}
static void rwlock_destroy(void *lock) {
}
#else
#define RWLOCK_TYPE pthread_rwlock_t
static void rwlock_init(void *lock) {
  pthread_rwlock_init(lock, NULL);
}
static void rwlock_write_lock(void *lock) {
  pthread_rwlock_wrlock(lock);
}
static void rwlock_write_unlock(void *lock) {
  pthread_rwlock_unlock(lock);
}
static void rwlock_read_lock(void *lock) {
  pthread_rwlock_rdlock(lock);
}
static void rwlock_read_unlock(void *lock) {
  pthread_rwlock_unlock(lock);
}
static void rwlock_destroy(void *lock) {
  pthread_rwlock_destroy(lock);
}
#endif
// end AEMU mutex

// AEMU sockets
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#else /* !_WIN32 */
#include <sys/socket.h>
#endif /* !_WIN32 */

/* QSOCKET_CALL is used to deal with the fact that EINTR happens pretty
 * easily in QEMU since we use SIGALRM to implement periodic timers
 */
#ifdef _WIN32
#define QSOCKET_CALL(_ret, _cmd) \
    do {                         \
        _ret = (_cmd);           \
    } while (_ret < 0 && WSAGetLastError() == WSAEINTR)
#else
#define HANDLE_EINTR(x)                                       \
    __extension__({                                           \
        __typeof__(x) eintr_wrapper_result;                   \
        do {                                                  \
            eintr_wrapper_result = (x);                       \
        } while (eintr_wrapper_result < 0 && errno == EINTR); \
        eintr_wrapper_result;                                 \
    })
#define QSOCKET_CALL(_ret, _cmd)   \
    do {                           \
        errno = 0;                 \
        _ret = HANDLE_EINTR(_cmd); \
    } while (0);
#endif

#define SOCKET_CALL(cmd)             \
    int ret;                         \
    QSOCKET_CALL(ret, (cmd));        \
    return ret;

static int socket_recv(int fd, void* buf, int len) {
    SOCKET_CALL(recv(fd, buf, len, 0))
}

static int socket_send(int fd, const void* buf, int buflen) {
    SOCKET_CALL(send(fd, buf, buflen, 0))
}
// end AEMU sockets

struct virtio_wifi_data {
	struct hostapd_data *hapd;
	int sock; /* raw packet socket */
	int ctrl_sock; /* control cmds socket */
	u8 perm_addr[ETH_ALEN];
	struct virtio_wifi_key_data PTK; /* Pairwise Temporal Key */
	struct virtio_wifi_key_data GTK; /* Group Temporal Key */
	RWLOCK_TYPE *lock;
};

static const unsigned char s_bssid[] = { 0x00, 0x13, 0x10, 0x85, 0xfe, 0x01 };

static const unsigned char rfc1042_header[] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

static struct virtio_wifi_data * priv_drv = NULL;

static void handle_eapol(struct virtio_wifi_data *drv, u8 *buf, size_t len,
			u16 stype)
{
	struct ieee80211_hdr *hdr;
	u8 *sa;
	size_t skipped_len;
	hdr = (struct ieee80211_hdr *) buf;
	sa = hdr->addr2;
	skipped_len = IEEE80211_HDRLEN + sizeof(rfc1042_header) + 2;
	if (len > skipped_len) {
		drv_event_eapol_rx(drv->hapd, sa, buf + skipped_len, len - skipped_len);
	} else {
		wpa_printf(MSG_DEBUG, "virtio_wifi: eapol frame is too short, length: %zu \n",
			   len);
	}
}

static void handle_tx_callback(struct virtio_wifi_data *drv, const u8 *buf,
			       size_t len, int ok)
{
	struct ieee80211_hdr *hdr;
	u16 fc;
	union wpa_event_data event;

	hdr = (struct ieee80211_hdr *) buf;
	fc = le_to_host16(hdr->frame_control);

	os_memset(&event, 0, sizeof(event));
	event.tx_status.type = WLAN_FC_GET_TYPE(fc);
	event.tx_status.stype = WLAN_FC_GET_STYPE(fc);
	event.tx_status.dst = hdr->addr1;
	event.tx_status.data = buf;
	event.tx_status.data_len = len;
	event.tx_status.ack = ok;
	wpa_supplicant_event(drv->hapd, EVENT_TX_STATUS, &event);
}

static void handle_tx_eapol_callback(struct virtio_wifi_data *drv, const u8 *buf,
			       size_t len, int ok)
{
	struct ieee80211_hdr *hdr;
	union wpa_event_data event;

	hdr = (struct ieee80211_hdr *) buf;

	os_memset(&event, 0, sizeof(event));
	event.eapol_tx_status.dst = hdr->addr1;
	event.eapol_tx_status.data = buf;
	event.eapol_tx_status.data_len = len;
	event.eapol_tx_status.ack = ok;

	wpa_supplicant_event(drv->hapd, EVENT_EAPOL_TX_STATUS, &event);
}

static void handle_frame(struct virtio_wifi_data *drv, u8 *buf, size_t len)
{
	struct ieee80211_hdr *hdr;
	u16 fc, type, stype;
	union wpa_event_data event;

	hdr = (struct ieee80211_hdr *) buf;
	fc = le_to_host16(hdr->frame_control);
	type = WLAN_FC_GET_TYPE(fc);
	stype = WLAN_FC_GET_STYPE(fc);

	switch (type) {
	case WLAN_FC_TYPE_MGMT:
		os_memset(&event, 0, sizeof(event));
		event.rx_mgmt.frame = buf;
		event.rx_mgmt.frame_len = len;
		wpa_supplicant_event(drv->hapd, EVENT_RX_MGMT, &event);
		break;
	case WLAN_FC_TYPE_DATA:
		handle_eapol(drv, buf, len, stype);
		break;
	case WLAN_FC_TYPE_CTRL:
		break;
	default:
		wpa_printf(MSG_ERROR, "virtio_wifi: unknown frame type %d \n",
			   type);
		break;
	}
}

static void handle_read(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct virtio_wifi_data *drv = eloop_ctx;
	int len;
	unsigned char buf[IEEE80211_MAX_FRAME_LEN];
	len = socket_recv(sock, buf, IEEE80211_MAX_FRAME_LEN);

	if (len < IEEE80211_HDRLEN) {
		if (len < 0) {
			wpa_printf(MSG_ERROR, "virtio_wifi: recv error: %s", strerror(errno));
		} else if (len > 0) {
			wpa_printf(MSG_DEBUG, "virtio_wifi: received too short frame: %d",
				   len);
		}
		return;
	}
	handle_frame(drv, buf, len);
}

int set_virtio_sock(int sock)
{
	if (priv_drv) {
		priv_drv->sock = sock;
		if (priv_drv->sock > 0 &&
			!eloop_register_read_sock(priv_drv->sock, handle_read, priv_drv, NULL)) {
			return 0;
		}
		wpa_printf(MSG_ERROR, "virtio_wifi: Could not register read socket for eloop");
	}
	return -1;
}

static void handle_ctrl_cmds(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct virtio_wifi_data *drv = eloop_ctx;
	int len;
	char buf[IEEE80211_MAX_FRAME_LEN + 1];
	len = socket_recv(sock, buf, IEEE80211_MAX_FRAME_LEN);
	if (len < 0 || len > IEEE80211_MAX_FRAME_LEN) {
		wpa_printf(MSG_ERROR, "virtio_wifi: handle_ctrl_cmds recv: %s", strerror(errno));
		return;
	}
	if (len == (sizeof(VIRTIO_WIFI_CTRL_CMD_TERMINATE) - 1) &&
			!strncmp(buf, VIRTIO_WIFI_CTRL_CMD_TERMINATE, len)) {
		eloop_terminate();
	} else if (len == (sizeof(VIRTIO_WIFI_CTRL_CMD_RELOAD_CONFIG) - 1) &&
			!strncmp(buf, VIRTIO_WIFI_CTRL_CMD_RELOAD_CONFIG, len)) {
		if (hostapd_reload_config(drv->hapd->iface) < 0) {
			wpa_printf(MSG_ERROR, "virtio_wifi: failed to read new configuration "
			   "file - continuing with old.");
		}
	} else {
		buf[len] = '\0';
		wpa_printf(MSG_ERROR, "virtio_wifi: unknown ctrl cmds %s", buf);
	}
}

int set_virtio_ctrl_sock(int sock)
{
	if (priv_drv) {
		priv_drv->ctrl_sock = sock;
		if (priv_drv->ctrl_sock > 0 &&
			!eloop_register_read_sock(priv_drv->ctrl_sock, handle_ctrl_cmds,
				priv_drv, NULL)) {
			return 0;
		}
		wpa_printf(MSG_ERROR, "virtio_wifi: Could not register control socket for eloop.");
	}
	return -1;
}

struct virtio_wifi_key_data get_active_ptk() {
	struct virtio_wifi_key_data ret;
	os_memset(&ret, 0, sizeof(ret));
	if (priv_drv) {
	rwlock_read_lock(priv_drv->lock);
		ret = priv_drv->PTK;
	rwlock_read_unlock(priv_drv->lock);
	}
	return ret;
}

struct virtio_wifi_key_data get_active_gtk() {
	struct virtio_wifi_key_data ret;
	os_memset(&ret, 0, sizeof(ret));
	if (priv_drv) {
	rwlock_read_lock(priv_drv->lock);
		ret = priv_drv->GTK;
	rwlock_read_unlock(priv_drv->lock);
	}
	return ret;
}

static void *virtio_wifi_init(struct hostapd_data *hapd,
		       struct wpa_init_params *params)
{
	struct virtio_wifi_data *drv;
	drv = os_zalloc(sizeof(*drv));
	drv->hapd = hapd;
	// Set customized bssid if set in hostapd conf
	if (is_zero_ether_addr(hapd->conf->bssid)) {
		os_memcpy(drv->perm_addr, s_bssid, ETH_ALEN);
		os_memcpy(hapd->own_addr, s_bssid, ETH_ALEN);
	} else {
		os_memcpy(drv->perm_addr, hapd->conf->bssid, ETH_ALEN);
		os_memcpy(hapd->own_addr, hapd->conf->bssid, ETH_ALEN);
	}
	os_memset(&drv->GTK, 0, sizeof(drv->GTK));
	os_memset(&drv->PTK, 0, sizeof(drv->PTK));
	drv->lock = os_zalloc(sizeof(*drv->lock));
	rwlock_init(drv->lock);
	priv_drv = drv;
	return drv;
}

static void virtio_wifi_deinit(void *priv) {
	struct virtio_wifi_data *drv = priv;
	priv_drv = NULL;
	rwlock_destroy(drv->lock);
	os_free(drv->lock);
	eloop_unregister_read_sock(drv->sock);
	eloop_unregister_read_sock(drv->ctrl_sock);
	os_free(drv);
}

static int virtio_wifi_send_mlme(void *priv, const u8 *msg, size_t len, int noack,
			    unsigned int freq,
			    const u16 *csa_offs, size_t csa_offs_len)
{
	struct virtio_wifi_data *drv = priv;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) msg;
	struct ieee80211_mgmt* mgmt = (struct ieee80211_mgmt *) msg;
	u16 fc = le_to_host16(mgmt->frame_control);
	int res;
	memcpy(hdr->IEEE80211_BSSID_FROMDS, priv_drv->hapd->own_addr, ETH_ALEN);
	memcpy(hdr->IEEE80211_SA_FROMDS, priv_drv->hapd->own_addr, ETH_ALEN);
	res = socket_send(drv->sock, msg, len);
	/* Request TX callback, assume that they have always been received */
	handle_tx_callback(drv, msg, len, 1);
	return res;
}

static int virtio_wifi_send_eapol(void *priv, const u8 *addr, const u8 *data,
				 size_t data_len, int encrypt, const u8 *own_addr,
				 u32 flags)
{
	struct virtio_wifi_data *drv = priv;
	struct ieee80211_hdr *hdr;
	size_t len;
	u8 *pos;
	int res;
	int qos;
	qos = flags & WPA_STA_WMM;
	len = sizeof(*hdr) + (qos ? 2 : 0) +
			sizeof(rfc1042_header) + 2 + data_len;
	hdr = os_zalloc(len);
	if (!hdr) {
		return -1;
	}

	hdr->frame_control =
		IEEE80211_FC(WLAN_FC_TYPE_DATA, WLAN_FC_STYPE_DATA);
	hdr->frame_control |= host_to_le16(WLAN_FC_FROMDS);
	if (encrypt)
		hdr->frame_control |= host_to_le16(WLAN_FC_ISWEP);
	if (qos) {
		hdr->frame_control |=
			host_to_le16(WLAN_FC_STYPE_QOS_DATA << 4);
	}
	memcpy(hdr->IEEE80211_DA_FROMDS, addr, ETH_ALEN);
	memcpy(hdr->IEEE80211_BSSID_FROMDS, own_addr, ETH_ALEN);
	memcpy(hdr->IEEE80211_SA_FROMDS, own_addr, ETH_ALEN);

	pos = (u8 *) (hdr + 1);
	if (qos) {
		/* Set highest priority in QoS header */
		pos[0] = 7;
		pos[1] = 0;
		pos += 2;
	}
	memcpy(pos, rfc1042_header, sizeof(rfc1042_header));
	pos += sizeof(rfc1042_header);
	WPA_PUT_BE16(pos, ETH_P_PAE);
	pos += 2;
	memcpy(pos, data, data_len);

	res = socket_send(drv->sock, (u8 *) hdr, len);
	if (res < 0) {
		wpa_printf(MSG_ERROR, "virtio_wifi: virtio_wifi_send_eapol -"
			   " packet len: %lu - failed: %d (%s)",
			   (unsigned long) len, errno, strerror(errno));
	}
	handle_tx_eapol_callback(drv, (u8 *)hdr, len, 1);
	os_free(hdr);
	return res;
}

static int virtio_wifi_if_add(void *priv, enum wpa_driver_if_type type,
				     const char *ifname, const u8 *addr,
				     void *bss_ctx, void **drv_priv,
				     char *force_ifname, u8 *if_addr,
				     const char *bridge, int use_existing,
				     int setup_ap)
{
	if (addr) {
		os_memcpy(if_addr, addr, ETH_ALEN);
	}
	return 0;
}

static struct hostapd_hw_modes * virtio_wifi_get_hw_feature_data(void *priv,
							    u16 *num_modes,
							    u16 *flags, u8 *dfs)
{
	struct hostapd_hw_modes *mode;
	int i, clen, rlen;
	const short chan2freq[14] = {
		2412, 2417, 2422, 2427, 2432, 2437, 2442,
		2447, 2452, 2457, 2462, 2467, 2472, 2484
	};

	mode = os_zalloc(sizeof(struct hostapd_hw_modes));
	if (mode == NULL)
		return NULL;

	*num_modes = 1;
	*flags = 0;
	*dfs = 0;

	mode->mode = HOSTAPD_MODE_IEEE80211G;
	mode->num_channels = 14;
	mode->num_rates = 4;

	clen = mode->num_channels * sizeof(struct hostapd_channel_data);
	rlen = mode->num_rates * sizeof(int);

	mode->channels = os_zalloc(clen);
	mode->rates = os_zalloc(rlen);
	if (mode->channels == NULL || mode->rates == NULL) {
		os_free(mode->channels);
		os_free(mode->rates);
		os_free(mode);
		return NULL;
	}

	for (i = 0; i < 14; i++) {
		mode->channels[i].chan = i + 1;
		mode->channels[i].freq = chan2freq[i];
		mode->channels[i].allowed_bw = HOSTAPD_CHAN_WIDTH_20;
		// TODO: Get allowed channel list from the driver 
		if (i >= 11)
			mode->channels[i].flag = HOSTAPD_CHAN_DISABLED;
	}

	mode->rates[0] = 10;
	mode->rates[1] = 20;
	mode->rates[2] = 55;
	mode->rates[3] = 110;

	return mode;
}

static u32 wpa_alg_to_cipher_suite(enum wpa_alg alg, size_t key_len)
{
	switch (alg) {
	case WPA_ALG_WEP:
		if (key_len == 5)
			return RSN_CIPHER_SUITE_WEP40;
		return RSN_CIPHER_SUITE_WEP104;
	case WPA_ALG_TKIP:
		return RSN_CIPHER_SUITE_TKIP;
	case WPA_ALG_CCMP:
		return RSN_CIPHER_SUITE_CCMP;
	case WPA_ALG_GCMP:
		return RSN_CIPHER_SUITE_GCMP;
	case WPA_ALG_CCMP_256:
		return RSN_CIPHER_SUITE_CCMP_256;
	case WPA_ALG_GCMP_256:
		return RSN_CIPHER_SUITE_GCMP_256;
	case WPA_ALG_IGTK:
		return RSN_CIPHER_SUITE_AES_128_CMAC;
	case WPA_ALG_BIP_GMAC_128:
		return RSN_CIPHER_SUITE_BIP_GMAC_128;
	case WPA_ALG_BIP_GMAC_256:
		return RSN_CIPHER_SUITE_BIP_GMAC_256;
	case WPA_ALG_BIP_CMAC_256:
		return RSN_CIPHER_SUITE_BIP_CMAC_256;
	case WPA_ALG_SMS4:
		return RSN_CIPHER_SUITE_SMS4;
	case WPA_ALG_KRK:
		return RSN_CIPHER_SUITE_KRK;
	case WPA_ALG_NONE:
	case WPA_ALG_PMK:
		wpa_printf(MSG_ERROR, "virtio_wifi: Unexpected encryption algorithm %d",
			   alg);
		return 0;
	}
	return 0;
}

static int virtio_wifi_set_key(const char *ifname, void *priv,
				  enum wpa_alg alg, const u8 *addr,
				  int key_idx, int set_tx,
				  const u8 *seq, size_t seq_len,
				  const u8 *key, size_t key_len) {
	u32 suite;
	int ret = 0;
	struct virtio_wifi_data *drv = priv;
	rwlock_write_lock(drv->lock);
	if (alg == WPA_ALG_NONE) {
		if (key_idx == drv->PTK.key_idx)
			drv->PTK.key_len = 0;
		if (key_idx == drv->GTK.key_idx)
			drv->GTK.key_len = 0;
		goto out;
	}
	suite = wpa_alg_to_cipher_suite(alg, key_len);
	if (suite != RSN_CIPHER_SUITE_CCMP) {
		wpa_printf(MSG_ERROR, "virtio_wifi: Unsupported encryption algorithm %d",
				alg);
		ret = -1;
		goto out;
	}
	if (key_len > MAX_KEY_MATERIAL_LEN) {
		wpa_printf(MSG_ERROR, "virtio_wifi: key_len %zu is larger than max key len %d",
				key_len, MAX_KEY_MATERIAL_LEN);
		ret = -1;
		goto out;
	}
	if (addr && is_broadcast_ether_addr(addr)) {
		os_memcpy(drv->GTK.key_material, key, key_len);
		drv->GTK.key_len = key_len;
		drv->GTK.key_idx = key_idx;
	} else if (addr && !is_broadcast_ether_addr(addr)) {
		os_memcpy(drv->PTK.key_material, key, key_len);
		drv->PTK.key_len = key_len;
		drv->PTK.key_idx = key_idx;
	}
out:
	rwlock_write_unlock(drv->lock);
	return ret;
}

static const u8 * virtio_wifi_get_macaddr(void *priv)
{
	struct virtio_wifi_data *drv = priv;

	return drv->perm_addr;
}

static int virtio_wifi_set_ap(void *priv, struct wpa_driver_ap_params *params) {
	struct virtio_wifi_data *drv = priv;
	struct ieee80211_hdr *hdr;
	size_t total_len = params->head_len + params->tail_len;
	hdr = os_zalloc(total_len);
	os_memcpy((u8 *)hdr, params->head, params->head_len);
	os_memcpy(((u8 *)hdr) + params->head_len, params->tail, params->tail_len);
	socket_send(drv->sock, hdr, total_len);
	/* Request TX callback, assume that they have always been received */
	handle_tx_callback(drv, (u8 *)hdr, total_len, 1);
	os_free(hdr);
	return 0;
}

const struct wpa_driver_ops wpa_driver_virtio_wifi_ops = {
	.name = "virtio_wifi",
	.desc = "Qemu virtio WiFi",
	.get_mac_addr = virtio_wifi_get_macaddr,
	.if_add = virtio_wifi_if_add,
	.get_hw_feature_data = virtio_wifi_get_hw_feature_data,
	.hapd_send_eapol = virtio_wifi_send_eapol,
	.send_mlme = virtio_wifi_send_mlme,
	.set_ap = virtio_wifi_set_ap,
	.set_key = virtio_wifi_set_key,
	.hapd_init = virtio_wifi_init,
	.hapd_deinit = virtio_wifi_deinit,
};
