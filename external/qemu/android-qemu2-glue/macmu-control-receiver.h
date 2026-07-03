// SPDX-License-Identifier: Apache-2.0
//
// MacMu control plane receiver: serves MMCP requests (display add/remove/list,
// hello, ping) arriving on the inherited control fd (MACMU_CONTROL_FD) and
// emits display events back to the shell. See
// shell/protocol/macmu_control_protocol.h and
// docs/FRAME_CHANNEL_V2_CONTROL_PLANE.md.

#pragma once

void macmu_control_receiver_start();
void macmu_control_receiver_stop();
