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

RendererState *RendererInit(Arena *arena, id<MTLDevice> device,
                             MTLPixelFormat colorFormat, MTLPixelFormat depthFormat,
                             float aspectRatio);
void RendererRender(RendererState *renderer, const GameState *game, RenderTarget target);
