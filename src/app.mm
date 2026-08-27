#include "app.h"

#include "game.h"

namespace {

// The very first thing placed in the arena, so FrameUpdate/FrameRender can
// always recover it from arena->base with no globals and no bookkeeping.
struct AppState {
    GameState *game;
    RendererState *renderer;
};

} // namespace

void Init(Arena *arena, id<MTLDevice> device, MTLPixelFormat colorFormat,
          MTLPixelFormat depthFormat, float aspectRatio) {
    AppState *appState = ArenaPushStruct(arena, AppState);
    appState->game = GameInit(arena);
    appState->renderer = RendererInit(arena, device, colorFormat, depthFormat, aspectRatio);
}

void FrameUpdate(Arena *arena, float deltaTime, CameraInput cameraInput) {
    AppState *appState = (AppState *)arena->base;
    GameUpdate(appState->game, deltaTime);
    RendererUpdateCamera(appState->renderer, cameraInput);
}

void FrameRender(Arena *arena, RenderTarget target) {
    AppState *appState = (AppState *)arena->base;
    RendererRender(appState->renderer, appState->game, target);
}

bool ImportBlendFile(Arena *arena, const char *filepath) {
    AppState *appState = (AppState *)arena->base;
    return GameImportBlendFile(appState->game, filepath);
}
