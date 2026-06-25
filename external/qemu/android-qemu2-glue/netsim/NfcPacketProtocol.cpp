// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "android-qemu2-glue/netsim/NfcPacketProtocol.h"
#include "aemu/base/logging/Log.h"
#include "netsim.h"
#include "netsim/common.pb.h"
#include "netsim/packet_streamer.pb.h"
#include "netsim/startup.pb.h"
#include <utility>
#include <algorithm>

namespace android {
namespace qemu2 {

class NfcPacketProtocol : public PacketProtocol {
public:
    NfcPacketProtocol(std::shared_ptr<DeviceInfo> device_info)
        : mDeviceInfo(device_info) {}

    std::unique_ptr<ChipInfo> chip_info() override {
        auto info = std::make_unique<ChipInfo>();
        info->set_name(mDeviceInfo->name());
        info->mutable_chip()->set_kind(netsim::common::ChipKind::NFC);
        info->mutable_device_info()->CopyFrom(*mDeviceInfo);
        return info;
    }

    void forwardToQemu(const PacketResponse* received,
                       byte_writer toStream) override {
        if (!received->has_packet()) {
            return;
        }
        const auto& packet = received->packet();
        toStream((uint8_t*)packet.data(), packet.size());
    }

    std::vector<PacketRequest> forwardToPacketStreamer(
            const uint8_t* buffer,
            size_t len) override {
        size_t left = len;

        while (left > 0) {
            size_t send = std::min<size_t>(left, mNfcParser.BytesRequested());
            if (send == 0) {
                break;
            }
            mNfcParser.Consume(buffer, send);
            buffer += send;
            left -= send;
        }
        return mNfcParser.getPacketQueue();
    }

    std::vector<PacketRequest> connectionStateChange(
            ConnectionStateChange state,
            byte_writer toQemu) override {
        std::vector<PacketRequest> response;

        switch (state) {
            case ConnectionStateChange::REMOTE_CONNECTED: {
                // Registration packet.
                PacketRequest registration;
                registration.set_allocated_initial_info(chip_info().release());
                response.push_back(std::move(registration));
                break;
            }
            case ConnectionStateChange::QEMU_DISCONNECTED:
            case ConnectionStateChange::REMOTE_DISCONNECTED:
                mNfcParser.Reset();
                break;
            default:
                break;
        }
        return response;
    }

private:
    std::shared_ptr<DeviceInfo> mDeviceInfo;

    class NfcParser {
    public:
        NfcParser() {}
        std::vector<PacketRequest> getPacketQueue() {
            return std::exchange(mPacketQueue, {});
        }

        void Reset() {
            mPacket.clear();
            mBytesWanted = NCI_HEADER_SIZE;
            state_ = NCI_HEADER;
            mPacketQueue.clear();
        }

        bool Consume(const uint8_t* buffer, size_t bytes_read) {
            if (bytes_read == 0) {
                LOG(INFO) << "remote disconnected, or unhandled error?";
                return false;
            }
            size_t bytes_to_read = BytesRequested();
            if (bytes_read > bytes_to_read) {
                LOG(FATAL) << "Nfc: More bytes read than expected";
            }

            mPacket.insert(mPacket.end(), buffer, buffer + bytes_read);
            mBytesWanted -= bytes_read;

            if (mBytesWanted == 0) {
                switch (state_) {
                    case NCI_HEADER:
                        mBytesWanted = mPacket.at(NCI_PAYLOAD_LENGTH_FIELD);
                        state_ = NCI_PAYLOAD;
                        if (mBytesWanted > 0) {
                            break;
                        }
                        [[fallthrough]];
                    case NCI_PAYLOAD: {
                        PacketRequest request;
                        request.set_packet(mPacket.data(), mPacket.size());
                        mPacketQueue.push_back(std::move(request));
                        mPacket.clear();
                        mBytesWanted = NCI_HEADER_SIZE;
                        state_ = NCI_HEADER;
                        break;
                    }
                }
            }
            return true;
        }

        size_t BytesRequested() {
            return mBytesWanted;
        }

    private:
        static constexpr size_t NCI_HEADER_SIZE = 3;
        static constexpr size_t NCI_PAYLOAD_LENGTH_FIELD = 2;
        enum State { NCI_HEADER, NCI_PAYLOAD };

        State state_{NCI_HEADER};
        std::vector<uint8_t> mPacket;
        size_t mBytesWanted{NCI_HEADER_SIZE};
        std::vector<PacketRequest> mPacketQueue;
    };

    NfcParser mNfcParser;
};

std::unique_ptr<PacketProtocol> getNfcPacketProtocol(
        std::string /*deviceType*/,
        std::shared_ptr<DeviceInfo> deviceInfo) {
    return std::make_unique<NfcPacketProtocol>(deviceInfo);
}

}  // namespace qemu2
}  // namespace android
