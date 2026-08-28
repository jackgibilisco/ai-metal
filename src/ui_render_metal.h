#pragma once

// Metal backend for the UI core. Turns the UiVertex triangle list into one
// alpha-blended render pass drawn on top of the drawable. A Windows port
// would add a parallel ui_render_d3d.* and leave ui.cpp untouched.

#import <Metal/Metal.h>

#include "arena.h"
#include "renderer_metal.h"
#include "ui.h"

struct UiRenderState;

UiRenderState *UiRenderInit(Arena *arena, id<MTLDevice> device, MTLPixelFormat colorFormat);
void UiRenderEncode(UiRenderState *uiRender, RenderTarget target, const UiVertex *vertices,
                    int vertexCount, float drawableWidth, float drawableHeight);
