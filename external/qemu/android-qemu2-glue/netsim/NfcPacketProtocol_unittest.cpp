// Copyright (C) 2026 The Android Open Source Project
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
#include "netsim/common.pb.h"
#include "netsim/packet_streamer.pb.h"
#include "netsim/startup.pb.h"
#include "gtest/gtest.h"

namespace android {
namespace qemu2 {

class NfcPacketProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        mDeviceInfo = std::make_shared<DeviceInfo>();
        mDeviceInfo->set_name("test-device");
        mDeviceInfo->set_avd_path("/path/to/avd");
        mProtocol = getNfcPacketProtocol("nfc", mDeviceInfo);
        ASSERT_NE(mProtocol, nullptr);
    }

    std::shared_ptr<DeviceInfo> mDeviceInfo;
    std::unique_ptr<PacketProtocol> mProtocol;
};

TEST_F(NfcPacketProtocolTest, ChipInfo) {
    auto info = mProtocol->chip_info();
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->name(), "test-device");
    EXPECT_EQ(info->chip().kind(), netsim::common::ChipKind::NFC);
    EXPECT_EQ(info->device_info().name(), "test-device");
    EXPECT_EQ(info->device_info().avd_path(), "/path/to/avd");
}

TEST_F(NfcPacketProtocolTest, ConnectionStateChangeRemoteConnected) {
    std::vector<uint8_t> writtenBytes;
    auto writer = [&writtenBytes](uint8_t* buffer, size_t len) {
        writtenBytes.insert(writtenBytes.end(), buffer, buffer + len);
    };

    auto requests = mProtocol->connectionStateChange(
            PacketProtocol::ConnectionStateChange::REMOTE_CONNECTED, writer);

    ASSERT_EQ(requests.size(), 1);
    EXPECT_TRUE(requests[0].has_initial_info());
    EXPECT_EQ(requests[0].initial_info().chip().kind(), netsim::common::ChipKind::NFC);
    EXPECT_TRUE(writtenBytes.empty()); // No bytes should be written to QEMU on remote connect
}

TEST_F(NfcPacketProtocolTest, ForwardToQemu) {
    std::vector<uint8_t> writtenBytes;
    auto writer = [&writtenBytes](uint8_t* buffer, size_t len) {
        writtenBytes.insert(writtenBytes.end(), buffer, buffer + len);
    };

    PacketResponse response;
    response.set_packet("hello nfc");

    mProtocol->forwardToQemu(&response, writer);

    std::string result(writtenBytes.begin(), writtenBytes.end());
    EXPECT_EQ(result, "hello nfc");
}

TEST_F(NfcPacketProtocolTest, ForwardToPacketStreamerIncompleteHeader) {
    // NCI header is 3 bytes. Send 2 bytes.
    uint8_t data[] = {0x00, 0x01};
    auto requests = mProtocol->forwardToPacketStreamer(data, sizeof(data));
    EXPECT_TRUE(requests.empty());
}

TEST_F(NfcPacketProtocolTest, ForwardToPacketStreamerEmptyPayload) {
    // NCI header: MT=0 (Data), GID=0, OID=0, Payload Length = 0
    // Header bytes: 0x00, 0x00, 0x00
    uint8_t data[] = {0x00, 0x00, 0x00};
    auto requests = mProtocol->forwardToPacketStreamer(data, sizeof(data));
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0].packet(), std::string("\x00\x00\x00", 3));
}

TEST_F(NfcPacketProtocolTest, ForwardToPacketStreamerWithPayload) {
    // NCI header: Payload Length = 5
    // Header: 0x00, 0x00, 0x05
    // Payload: 'h', 'e', 'l', 'l', 'o' (0x68, 0x65, 0x6c, 0x6c, 0x6f)
    uint8_t data[] = {0x00, 0x00, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f};
    auto requests = mProtocol->forwardToPacketStreamer(data, sizeof(data));
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0].packet(), std::string("\x00\x00\x05hello", 8));
}

TEST_F(NfcPacketProtocolTest, ForwardToPacketStreamerIncremental) {
    // Send packet in parts
    // Part 1: Header (3 bytes: len=2)
    uint8_t part1[] = {0x00, 0x00, 0x02};
    auto requests = mProtocol->forwardToPacketStreamer(part1, sizeof(part1));
    EXPECT_TRUE(requests.empty());

    // Part 2: Payload byte 1
    uint8_t part2[] = {0xaa};
    requests = mProtocol->forwardToPacketStreamer(part2, sizeof(part2));
    EXPECT_TRUE(requests.empty());

    // Part 3: Payload byte 2
    uint8_t part3[] = {0xbb};
    requests = mProtocol->forwardToPacketStreamer(part3, sizeof(part3));
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0].packet(), std::string("\x00\x00\x02\xaa\xbb", 5));
}

TEST_F(NfcPacketProtocolTest, ForwardToPacketStreamerMultiplePackets) {
    // Packet 1: len=1, payload=0xaa
    // Packet 2: len=2, payload=0xbb, 0xcc
    uint8_t data[] = {
        0x00, 0x00, 0x01, 0xaa,
        0x00, 0x00, 0x02, 0xbb, 0xcc
    };
    auto requests = mProtocol->forwardToPacketStreamer(data, sizeof(data));
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[0].packet(), std::string("\x00\x00\x01\xaa", 4));
    EXPECT_EQ(requests[1].packet(), std::string("\x00\x00\x02\xbb\xcc", 5));
}

TEST_F(NfcPacketProtocolTest, ResetOnDisconnect) {
    // 1. Send incomplete packet (header only, expecting 5 bytes payload)
    uint8_t header[] = {0x00, 0x00, 0x05};
    auto requests = mProtocol->forwardToPacketStreamer(header, sizeof(header));
    EXPECT_TRUE(requests.empty());

    // 2. Trigger disconnect (should reset parser)
    auto writer = [](uint8_t* /*buffer*/, size_t /*len*/) {};
    mProtocol->connectionStateChange(
            PacketProtocol::ConnectionStateChange::REMOTE_DISCONNECTED, writer);

    // 3. Send a new complete packet (len=1, payload=0xaa)
    uint8_t new_packet[] = {0x00, 0x00, 0x01, 0xaa};
    requests = mProtocol->forwardToPacketStreamer(new_packet, sizeof(new_packet));
    
    // 4. Verify it parsed correctly as a new packet (not merged with old header)
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0].packet(), std::string("\x00\x00\x01\xaa", 4));
}

}  // namespace qemu2
}  // namespace android
