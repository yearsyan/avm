// Copyright (C) 2017 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/console_internal.h"

#include "aemu/base/sockets/SocketUtils.h"
#include "android/avd/info.h"
#include "android/console.h"
#include "android/telephony/modem_driver.h"

#include <gtest/gtest.h>

#include "android/emulation/testing/MockAndroidEmulatorWindowAgent.h"

#ifdef __APPLE__
// Mac linker has a lot of problems determining that modem_driver.o is
// a dependency, even though we use |android_modem| throughout console.c.
// To make sure we get this dependency, tell the compiler to resolve one of
// the functions defined in modem_driver.c.
static const auto pointer = &android_modem_init;
#endif

namespace android {

using namespace base;

namespace console {

// Android studio has a read buffer size of 1024 bytes.
// Older versions of studio use the 'help' command to ping the
// emulator, while newer versions will just use the ping command.
// So for now, we'll be careful to not pass the 1024-byte limit on
// the help printout.
static const int STUDIO_BUFF_SIZE = 1024;

TEST(Console, postauth_help_size_max) {
#ifdef __APPLE__
    // The compiler may still optimize the modem_driver.o dependency out, so
    // force it to locate that symbol.
    ASSERT_TRUE(pointer != nullptr);
#endif

    const int BUFF_SIZE = STUDIO_BUFF_SIZE + 1;
    char buff[BUFF_SIZE];
    int sock[2];

    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    // Create a fake client that will send the output through this socket
    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != NULL);

    send_test_string(opaque, "help");

    // Read back the output of 'help'. Should be <= 1024 bytes. If we have
    // received every byte we asked for, then it means we went past the limit.
    memset(buff, 0, BUFF_SIZE);
    bool hasRecvAll = socketRecvAll(sock[0], buff, BUFF_SIZE);
    EXPECT_FALSE(hasRecvAll);

    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
}

TEST(Console, NetworkSpeedNoArgsFails) {
    const char kExpected[] =
            "KO: missing <speed> argument, see 'help network speed'\r\n";
    const int BUFF_SIZE = 1024;
    char buff[BUFF_SIZE] = {};
    int sock[2];

    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    // Create a fake client that will send the output through this socket
    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != NULL);

    send_test_string(opaque, "network speed");

    // Read back the output.
    socketRecvAll(sock[0], buff, sizeof(kExpected) - 1);
    EXPECT_STREQ(kExpected, buff);

    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
}

TEST(Console, multidisplay_add_xr_fails) {
    const int BUFF_SIZE = STUDIO_BUFF_SIZE + 1;
    char buff[BUFF_SIZE];
    int sock[2];

    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    // Create a fake client that will send the output through this socket
    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != NULL);

    // Create a mock AVD info for an XR device.
    AvdInfo* avdInfo = avdInfo_new_for_testing(AVD_XR);
    getConsoleAgents()->settings->inject_AvdInfo(avdInfo);

    send_test_string(opaque, "multidisplay add 1 800 600 240 0");

    // Read back the output.
    memset(buff, 0, BUFF_SIZE);
    socketRecvAll(sock[0], buff, BUFF_SIZE);
    EXPECT_STREQ(buff, "KO: multidisplay is not supported on XR devices\r\n");

    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
    getConsoleAgents()->settings->inject_AvdInfo(NULL);
    avdInfo_free(avdInfo);
}

TEST(Console, rotate_xr_fails) {
    const int BUFF_SIZE = STUDIO_BUFF_SIZE + 1;
    char buff[BUFF_SIZE];
    int sock[2];

    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    // Create a fake client that will send the output through this socket
    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != NULL);

    // Create a mock AVD info for an XR device.
    AvdInfo* avdInfo = avdInfo_new_for_testing(AVD_XR);
    getConsoleAgents()->settings->inject_AvdInfo(avdInfo);

    send_test_string(opaque, "rotate");

    // Read back the output.
    memset(buff, 0, BUFF_SIZE);
    socketRecvAll(sock[0], buff, BUFF_SIZE);
    EXPECT_STREQ(buff, "KO: rotation is not supported for XR devices.\r\n");

    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
    getConsoleAgents()->settings->inject_AvdInfo(NULL);
    avdInfo_free(avdInfo);
}

TEST(Console, fold_fails_on_unsupported) {
    const int BUFF_SIZE = STUDIO_BUFF_SIZE + 1;
    char buff[BUFF_SIZE];
    int sock[2];

    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != NULL);

    MockAndroidEmulatorWindowAgent mockWindowAgent;
    MockAndroidEmulatorWindowAgent::mock = &mockWindowAgent;
    EXPECT_CALL(mockWindowAgent, fold(true)).WillOnce(testing::Return(false));

    send_test_string(opaque, "fold");

    memset(buff, 0, BUFF_SIZE);
    socketRecvAll(sock[0], buff, BUFF_SIZE);
    EXPECT_STREQ(buff, "KO: Device is not foldable\r\n");

    MockAndroidEmulatorWindowAgent::mock = nullptr;
    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
}

TEST(Console, unfold_fails_on_unsupported) {
    const int BUFF_SIZE = STUDIO_BUFF_SIZE + 1;
    char buff[BUFF_SIZE];
    int sock[2];

    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != NULL);

    MockAndroidEmulatorWindowAgent mockWindowAgent;
    MockAndroidEmulatorWindowAgent::mock = &mockWindowAgent;
    EXPECT_CALL(mockWindowAgent, fold(false)).WillOnce(testing::Return(false));

    send_test_string(opaque, "unfold");

    memset(buff, 0, BUFF_SIZE);
    socketRecvAll(sock[0], buff, BUFF_SIZE);
    EXPECT_STREQ(buff, "KO: Device is not foldable\r\n");

    MockAndroidEmulatorWindowAgent::mock = nullptr;
    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
}

static void ping_test();

// Bug: 171479818
// TEST(Console, preauth_ping) {
//     ping_test();
// }

// Bug: 171479818
// TEST(Console, postauth_ping) {
//     ping_test();
// }

static void ping_test() {
    // Verify the 'ping' response.
    // This may be used by automated clients that expect
    // an exact response, so we verify the exact response.
    const char ALIVE_WRITE[] = "I am alive!\r\n";
    const char OK_WRITE[]= "OK\r\n";
    auto PING_SIZE_ALIVE = strlen(ALIVE_WRITE);
    auto PING_SIZE_OK = strlen(OK_WRITE);
    const int BUFF_SIZE = STUDIO_BUFF_SIZE + 1;
    char buff[BUFF_SIZE];
    int sock[2];
    errno = 0;

    EXPECT_GT(BUFF_SIZE, PING_SIZE_ALIVE + PING_SIZE_OK);
    ASSERT_EQ(0, socketCreatePair(&sock[0], &sock[1]));

    // Create a fake client that will send the output through this socket
    void* opaque = test_control_client_create(sock[1]);
    ASSERT_TRUE(opaque != nullptr);

    errno = 0;
    send_test_string(opaque, "ping");
    EXPECT_EQ(0, errno);

    // First check we can read the alive message.
    errno = 0;
    memset(buff, 0, BUFF_SIZE);
    bool hasRecvAll = socketRecvAll(sock[0], buff, PING_SIZE_ALIVE);

    // All the ping bytes should have been received.
    EXPECT_TRUE(hasRecvAll);

    // There should not be an errors. errno will contain a unix error code
    // even when running on windows.
    EXPECT_EQ(0, errno);

    // We should have received the alive message
    EXPECT_EQ(PING_SIZE_ALIVE, strlen(buff));
    EXPECT_STREQ(buff, ALIVE_WRITE);

    // Next we receive the OK message.
    errno = 0;
    memset(buff, 0, BUFF_SIZE);
    hasRecvAll = socketRecvAll(sock[0], buff, PING_SIZE_OK);

    // All the ping bytes should have been received.
    EXPECT_TRUE(hasRecvAll);

    // There should not be an errors. errno will contain a unix error code
    // even when running on windows.
    EXPECT_EQ(0, errno);

    // We should have received the OK message
    EXPECT_EQ(PING_SIZE_OK, strlen(buff));
    EXPECT_STREQ(buff, OK_WRITE);

    test_control_client_close(opaque);
    socketClose(sock[1]);
    socketClose(sock[0]);
}
}  // namespace console
}  // namespace android
