#pragma once

// The exact API the platform layer calls: Init once, then FrameUpdate and
// FrameRender every frame. Everything the program needs lives in the arena
// passed to each call; nothing else is allocated after Init returns.

#import <Metal/Metal.h>

#include "arena.h"
#include "renderer_metal.h"

void Init(Arena *arena, id<MTLDevice> device, MTLPixelFormat colorFormat,
          MTLPixelFormat depthFormat, float drawableWidth, float drawableHeight);
void FrameUpdate(Arena *arena, float deltaTime, CameraInput cameraInput);
void FrameRender(Arena *arena, RenderTarget target);
void FrameResize(Arena *arena, float drawableWidth, float drawableHeight);

// Called by the platform layer's File > Import File... menu action.
bool ImportBlendFile(Arena *arena, const char *filepath);
