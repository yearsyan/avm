// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0.

#include "android/console.h"

int android_console_start(int control_port, const AndroidConsoleAgents* agents) {
    (void)control_port;
    (void)agents;
    return 0;
}

const char* android_console_auth_banner_get() {
    return "Android Console disabled in AEMU core-only build\nOK";
}

const char* android_console_help_banner_get() {
    return "Android Console disabled in AEMU core-only build\nOK";
}

void* test_control_client_create(int socket) {
    (void)socket;
    return nullptr;
}

void test_control_client_close(void* opaque) {
    (void)opaque;
}

void send_test_string(void* opaque, const char* the_string) {
    (void)opaque;
    (void)the_string;
}

extern "C" bool qemu_wav_audio_rewind_input_wave() {
    return false;
}
