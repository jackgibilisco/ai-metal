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
    MTLRenderPassDescriptor *passDescriptor;
    id<CAMetalDrawable> drawable;
};

struct RendererState;

// Raw, per-frame accumulated trackpad deltas. Plain floats so platform_macos.mm
// (which reads the NSEvents) and app.mm (which just forwards this struct) don't
// need to know anything about how the renderer interprets them.
struct CameraInput {
    float panX;       // two-finger drag, points
    float panY;       // two-finger drag, points
    float zoomDelta;  // pinch magnification
    float orbitYaw;   // shift + two-finger drag, points
    float orbitPitch; // shift + two-finger drag, points
};

RendererState *RendererInit(Arena *arena, id<MTLDevice> device,
                             MTLPixelFormat colorFormat, MTLPixelFormat depthFormat,
                             float aspectRatio);
void RendererUpdateCamera(RendererState *renderer, CameraInput input);
void RendererRender(RendererState *renderer, const GameState *game, RenderTarget target);
