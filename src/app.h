#pragma once

// The exact API the platform layer calls: Init once, then FrameUpdate and
// FrameRender every frame. Everything the program needs lives in the arena
// passed to each call; nothing else is allocated after Init returns.

#import <Metal/Metal.h>

#include "arena.h"
#include "frame_input.h"
#include "menu.h"
#include "renderer_metal.h"

void Init(Arena *arena, id<MTLDevice> device, MTLPixelFormat colorFormat,
          MTLPixelFormat depthFormat, float drawableWidth, float drawableHeight,
          PlatformMenuHooks menuHooks);
// Returns true if the frame changed something the renderer would draw
// differently (scene animated, camera moved, a render toggle fired, the UI is
// being interacted with, or a render was explicitly requested). When it
// returns false the platform layer may skip FrameRender and leave the last
// presented frame on screen.
bool FrameUpdate(Arena *arena, float deltaTime, FrameInput input);
void FrameRender(Arena *arena, RenderTarget target);
void FrameResize(Arena *arena, float drawableWidth, float drawableHeight);

// Force the next few FrameUpdate calls to report "needs render" — used after
// the frame loop resumes or the HUD is toggled, so multi-buffered targets
// flush to the screen.
void AppRequestRender(Arena *arena);

// Per-pass GPU time of the last completed frame, for the F3 HUD.
RendererPassTimings FrameGpuTimings(Arena *arena);

// Runs a menu action (from the native menu bar or the in-app strip) through
// the shared dispatch.
void AppDispatchMenuAction(Arena *arena, MenuAction action);

// Current menu state, for the native menu bar to reflect (checkmark / enable).
MenuState AppMenuState(Arena *arena);

// Called by the platform layer's File > Import File... menu action.
bool ImportBlendFile(Arena *arena, const char *filepath);
