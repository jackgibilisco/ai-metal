#pragma once

// Metal-specific rendering. This file may use the Metal API freely, but must
// not touch AppKit/UIKit or any other OS windowing API — the platform layer
// owns the window and hands rendering targets in through RenderTarget.

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "arena.h"
#include "frame_input.h"
#include "game.h"

struct RenderTarget {
    id<MTLCommandBuffer> commandBuffer;
    id<CAMetalDrawable> drawable;
};

struct RendererState;

// GPU time in milliseconds for each pass of the last completed frame, for
// the F3 HUD. A pass that didn't run that frame reads 0. Per-pass values
// assume Apple-silicon nanosecond timestamps; total is measured
// independently from the command buffer's GPU start/end.
struct RendererPassTimings {
    float geometryMs;
    float aoMs;
    float lightingMs;
    float fxaaMs;
    float totalMs;
};

RendererState *RendererInit(Arena *arena, id<MTLDevice> device,
                             MTLPixelFormat colorFormat, MTLPixelFormat depthFormat,
                             float drawableWidth, float drawableHeight);

// The scene renders into an `(originX, originY, width, height)` region of the
// drawable (the part the UI panel and menu strip don't take). Sets the
// projection aspect and rebuilds the screen-sized targets when the size
// changes; the origin only shifts where the final pass writes the drawable.
void RendererSetContentRect(RendererState *renderer, float originX, float originY, float width,
                            float height);
void RendererUpdateCamera(RendererState *renderer, FrameInput input);
void RendererRender(RendererState *renderer, const GameState *game, RenderTarget target);
RendererPassTimings RendererLastFrameTimings(const RendererState *renderer);
