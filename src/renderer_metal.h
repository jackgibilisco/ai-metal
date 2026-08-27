#pragma once

// Metal-specific rendering. This file may use the Metal API freely, but must
// not touch AppKit/UIKit or any other OS windowing API — the platform layer
// owns the window and hands rendering targets in through RenderTarget.

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "arena.h"
#include "game.h"

struct RenderTarget {
    id<MTLCommandBuffer> commandBuffer;
    id<CAMetalDrawable> drawable;
};

struct RendererState;

// Raw, per-frame accumulated pointer deltas. Plain floats so platform_macos.mm
// (which reads the NSEvents) and app.mm (which just forwards this struct) don't
// need to know anything about how the renderer interprets them.
struct CameraInput {
    float panX;       // two-finger drag or right-drag, points
    float panY;       // two-finger drag or right-drag, points
    float zoomDelta;  // pinch magnification or mouse wheel
    float orbitYaw;   // shift + two-finger drag, shift-right-drag, or middle-drag, points
    float orbitPitch; // shift + two-finger drag, shift-right-drag, or middle-drag, points
    bool cycleDebugView; // one keypress: advance the AO debug view mode
    bool toggleFxaa;     // one keypress: enable/disable the FXAA post pass
};

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
void RendererResize(RendererState *renderer, float drawableWidth, float drawableHeight);
void RendererUpdateCamera(RendererState *renderer, CameraInput input);
void RendererRender(RendererState *renderer, const GameState *game, RenderTarget target);
RendererPassTimings RendererLastFrameTimings(const RendererState *renderer);
