/* Driver interaction with QEMU virtio wifi
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
*/

#ifndef DRIVER_VIRTIO_WIFI_H
#define DRIVER_VIRTIO_WIFI_H

#include <stdint.h>
extern int set_virtio_sock(int sock);
extern int set_virtio_ctrl_sock(int sock);

#define MAX_KEY_MATERIAL_LEN 32 /* max key length is 32 bytes */

struct virtio_wifi_key_data {
	uint8_t key_material[MAX_KEY_MATERIAL_LEN];
	int key_len;
	int key_idx;
};

#define VIRTIO_WIFI_CTRL_CMD_TERMINATE "CTRL_CMD_TERMINATE"
#define VIRTIO_WIFI_CTRL_CMD_RELOAD_CONFIG "CTRL_CMD_RELOAD_CONFIG"

// There is at most one active key in use.
extern struct virtio_wifi_key_data get_active_ptk();
extern struct virtio_wifi_key_data get_active_gtk();

#endif
