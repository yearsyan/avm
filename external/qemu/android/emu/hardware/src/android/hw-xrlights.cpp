/* Copyright (C) 2026 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

#include "android/hw-xrlights.h"

#include "aemu/base/EventNotificationSupport.h"
#include "aemu/base/utils/stream.h"
#include "android/console.h"
#include "android/emulation/android_qemud.h"
#include "android/utils/debug.h"
#include "android/utils/misc.h"
#include "android/utils/system.h"


#include "lights_conn.pb.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <optional>

#define  E(...)    derror(__VA_ARGS__)
#define  W(...)    dwarning(__VA_ARGS__)
const bool DEBUG = false;
#define  D(...)  if (DEBUG) { dwarning(__VA_ARGS__); }
#define  V(...)  VERBOSE_PRINT(init,__VA_ARGS__)

using lights_conn_proto::LightStatus;

std::optional<HwXrLights> sHwXrLights;
std::once_flag sHwXrLightsOnceFlag;

HwXrLights& getHwXrLights() {
    std::call_once(sHwXrLightsOnceFlag, []() {
        android_hw_xrlights_init();
    });
    return *sHwXrLights;
}

// Called when a qemud client connects to the service
static QemudClient* _hwXrLights_connect(void*  opaque,
                                        QemudService*  service,
                                        int  channel,
                                        const char* client_param) {
    QemudClient* qemudClient;
    HwXrLightsClient* hwXrLightsClient = new HwXrLightsClient(&getHwXrLights());
    qemudClient = qemud_client_new(service, channel, client_param, hwXrLightsClient,
                                   HwXrLightsClient::recv,
                                   HwXrLightsClient::close,
                                   NULL, /* no save */
                                   NULL /* no load */ );
    hwXrLightsClient->setQemudClient(qemudClient);
    qemud_client_set_framing(qemudClient, 1);
    D("%s: connect lights qemud is called", __func__);
    return qemudClient;
}

//static
void HwXrLightsClient::recv(void* opaque, uint8_t* msg, int msglen,
                                   QemudClient*  client) {
    auto* hwXrLightsClient = static_cast<HwXrLightsClient*>(opaque);
    hwXrLightsClient->mHwXrLights->qemudClientRecv(msg, msglen);
}

//static
void HwXrLightsClient::close(void* opaque) {
    D("HwXrLights client closed.");
    auto* client = static_cast<HwXrLightsClient*>(opaque);
    delete client;
}

HwXrLights::HwXrLights()
    : mClientCallbacksLock(std::make_unique<std::mutex>()){
    if (mService == nullptr) {
        mService = qemud_service_register("xrlightsservice", 0, this,
                                             _hwXrLights_connect,
                                             NULL, /* no save */
                                             NULL /* no load */);
        D("%s: xr lights qemud listen service initialized", __func__);
    } else {
        D("%s: xr lights qemud listen service already initialized", __func__);
    }
}

void HwXrLights::setLightStatus(uint32_t id, uint32_t color) {
        if(id == WORLD) {
            mHwXrLightStatus.world_color = color;
        } else if (id == USER) {
            mHwXrLightStatus.user_color = color;
        } else {
            D("Unknown led light id %d received", id);
        }
}

void HwXrLights::qemudClientRecv(uint8_t* msg, int msglen) {
    // The current message format is pure string, only for log and test purpose.
    // TODO(b/504670848): refine the message format.
    // The color is in ARGB with the alpha channel expected to be 255, but ignored.
    LightStatus lightStatus;
    int id = -1;
    int color = -1;

    // SerializeAsString on the sender side still writes standard Protobuf wire-format
    // binary data, the receiving side can continue using ParseFromArray. This method
    // naturally accepts a raw pointer and a size without needing a string conversion wrapper.
    if (lightStatus.ParseFromArray(msg, msglen)) {
        // Success! You can now read the fields
        id = lightStatus.light_id();
        color = lightStatus.light_color();
        D("Received LightStatus: id=%d, color=0x%x", id, color);
        setLightStatus(id, color);
    } else {
        // Failure
        D("Failed to parse LightStatus from received array.");
    }

    std::lock_guard<std::mutex> guard(*mClientCallbacksLock);
    AndroidHwXrLedEvent event = {
        .lightid = id,
        .lightcolor = color
    };
    for (const auto& [client, funcs] : mClientCallbacks) {
        if (funcs.led_forwarder != nullptr) {
            funcs.led_forwarder(client, &event);
        }
    }
}

void HwXrLights::setClient(void* client, AndroidHwXrLedFuncs clientFuncs) {
    std::lock_guard<std::mutex> guard(*mClientCallbacksLock);
    mClientCallbacks[client] = clientFuncs;
}

void HwXrLights::removeClient(void* client) {
    std::lock_guard<std::mutex> guard(*mClientCallbacksLock);
    mClientCallbacks.erase(client);
}

uint32_t HwXrLights::getLightStatus(uint32_t id) {
    if(id == WORLD) {
        return mHwXrLightStatus.world_color;
    }
    if(id == USER) {
        return mHwXrLightStatus.user_color;
    }
    D("%s: invalid led light id", __func__);
    return 0;
}

void android_hw_xrlights_init() {
    sHwXrLights = std::make_optional<HwXrLights>();
    D("%s: HwXrLights qemud handler initialized", __func__);
}

void android_hw_xrlights_set(void* opaque, const AndroidHwXrLedFuncs* funcs) {
    getHwXrLights().setClient(opaque, *funcs);
}

void android_hw_xrlights_unset(void* opaque) {
    if (sHwXrLights) {
        getHwXrLights().removeClient(opaque);
    }
}
