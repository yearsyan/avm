// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_INPUT_VIEW_H
#define MACMU_SHELL_INPUT_VIEW_H

#import <MetalKit/MetalKit.h>

#include "macmu_surface_renderer.h"

class InputSender;
class GuestInputSender;

MTKView* macmu_input_view_create(NSRect frame,
                                 id<MTLDevice> device,
                                 InputSender* input_sender,
                                 GuestInputSender* guest_input_sender);

void macmu_input_view_set_renderer(MTKView* view, MacMuSurfaceRendererRef renderer);

#endif  // MACMU_SHELL_INPUT_VIEW_H
