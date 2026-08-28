#include "app.h"

#include "game.h"
#include "ui.h"
#include "ui_render_metal.h"

namespace {

// The very first thing placed in the arena, so FrameUpdate/FrameRender can
// always recover it from arena->base with no globals and no bookkeeping.
struct AppState {
    GameState *game;
    RendererState *renderer;
    UiState *ui;
    UiRenderState *uiRender;
    UiDemoState demo;
    MenuState menu;
    PlatformMenuHooks menuHooks;
};

} // namespace

void Init(Arena *arena, id<MTLDevice> device, MTLPixelFormat colorFormat,
          MTLPixelFormat depthFormat, float drawableWidth, float drawableHeight,
          PlatformMenuHooks menuHooks) {
    AppState *appState = ArenaPushStruct(arena, AppState);
    appState->game = GameInit(arena);
    appState->renderer =
        RendererInit(arena, device, colorFormat, depthFormat, drawableWidth, drawableHeight);
    appState->ui = UiInit(arena, drawableWidth, drawableHeight);
    appState->uiRender = UiRenderInit(arena, device, colorFormat);
    appState->demo = {0.5f, 0.35f};
    appState->menuHooks = menuHooks;
}

void FrameUpdate(Arena *arena, float deltaTime, FrameInput input) {
    AppState *appState = (AppState *)arena->base;
    GameUpdate(appState->game, deltaTime);
    UiBuildFrame(appState->ui, input, appState->menu.showMenuBar, &appState->demo);

    MenuAction menuAction = UiTakeMenuAction(appState->ui);
    if (menuAction != MenuAction_None) {
        MenuInvoke(menuAction, &appState->menu, appState->menuHooks);
    }

    FrameInput cameraInput = input;
    if (UiWantsMouse(appState->ui)) {
        cameraInput.panX = 0.0f;
        cameraInput.panY = 0.0f;
        cameraInput.zoomDelta = 0.0f;
        cameraInput.orbitYaw = 0.0f;
        cameraInput.orbitPitch = 0.0f;
    }
    RendererUpdateCamera(appState->renderer, cameraInput);
}

void FrameRender(Arena *arena, RenderTarget target) {
    AppState *appState = (AppState *)arena->base;
    RendererSetContentRect(appState->renderer, UiContentOriginX(appState->ui),
                           UiContentOriginY(appState->ui), UiContentWidth(appState->ui),
                           UiContentHeight(appState->ui));
    RendererRender(appState->renderer, appState->game, target);
    UiRenderEncode(appState->uiRender, target, UiVertices(appState->ui),
                   UiVertexCount(appState->ui), UiDrawableWidth(appState->ui),
                   UiDrawableHeight(appState->ui));
    [target.commandBuffer presentDrawable:target.drawable];
    [target.commandBuffer commit];
}

void FrameResize(Arena *arena, float drawableWidth, float drawableHeight) {
    AppState *appState = (AppState *)arena->base;
    UiHandleResize(appState->ui, drawableWidth, drawableHeight);
    RendererSetContentRect(appState->renderer, UiContentOriginX(appState->ui),
                           UiContentOriginY(appState->ui), UiContentWidth(appState->ui),
                           UiContentHeight(appState->ui));
}

RendererPassTimings FrameGpuTimings(Arena *arena) {
    AppState *appState = (AppState *)arena->base;
    return RendererLastFrameTimings(appState->renderer);
}

void AppDispatchMenuAction(Arena *arena, MenuAction action) {
    AppState *appState = (AppState *)arena->base;
    MenuInvoke(action, &appState->menu, appState->menuHooks);
}

MenuState AppMenuState(Arena *arena) {
    AppState *appState = (AppState *)arena->base;
    return appState->menu;
}

bool ImportBlendFile(Arena *arena, const char *filepath) {
    AppState *appState = (AppState *)arena->base;
    return GameImportBlendFile(appState->game, filepath);
}
