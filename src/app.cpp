#include "app.h"

#include "game.h"
#include "ui.h"
#include "ui_render.h"

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
    bool fullscreen; // tracked from the last FrameInput, for CommandContext
    int forcedRenderFrames; // FrameUpdate reports "needs render" while this is > 0
};

CommandContext AppStateContext(AppState *appState) {
    return CommandContext{&appState->menu, appState->menuHooks, appState->fullscreen};
}

void InvokeCommand(AppState *appState, CommandId id) {
    CommandInvoke(id, AppStateContext(appState));
    appState->forcedRenderFrames = 3;
}

} // namespace

void Init(Arena *arena, GpuContext *gpu, float drawableWidth, float drawableHeight,
          PlatformMenuHooks menuHooks) {
    AppState *appState = ArenaPushStruct(arena, AppState);
    appState->game = GameInit(arena);
    appState->renderer = RendererInit(arena, gpu, drawableWidth, drawableHeight);
    appState->ui = UiInit(arena, drawableWidth, drawableHeight);
    appState->uiRender = UiRenderInit(arena, gpu);
    appState->demo = {0.5f, 0.35f};
    appState->menuHooks = menuHooks;
}

bool FrameUpdate(Arena *arena, float deltaTime, FrameInput input) {
    AppState *appState = (AppState *)arena->base;
    appState->fullscreen = input.fullscreen;

    if (input.toggleSpin) {
        GameToggleSpin(appState->game);
        appState->forcedRenderFrames = 1;
    }

    bool sceneAnimated = GameUpdate(appState->game, deltaTime);

    UiBuildFrame(appState->ui, input, AppStateContext(appState), &appState->demo);

    CommandId clicked = UiTakeCommand(appState->ui);
    if (clicked != Command_None) {
        InvokeCommand(appState, clicked);
    }

    for (int i = 0; i < input.keyEventCount; ++i) {
        if (!input.keyEvents[i].pressed) {
            continue;
        }
        unsigned int key = input.keyEvents[i].codepoint;
        if (key >= 'A' && key <= 'Z') {
            key += 32;
        }
        const Command *command = CommandForShortcut(key, input.keyEvents[i].mods);
        if (command != nullptr) {
            InvokeCommand(appState, command->id);
        }
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

    bool cameraMoved = cameraInput.panX != 0.0f || cameraInput.panY != 0.0f ||
                       cameraInput.zoomDelta != 0.0f || cameraInput.orbitYaw != 0.0f ||
                       cameraInput.orbitPitch != 0.0f;
    bool renderToggled = input.cycleDebugView || input.toggleFxaa;
    bool uiInteracting = UiWantsMouse(appState->ui) || UiWantsKeyboard(appState->ui);

    bool needsRender = sceneAnimated || cameraMoved || renderToggled || uiInteracting ||
                       appState->forcedRenderFrames > 0;

    if (appState->forcedRenderFrames > 0) {
        appState->forcedRenderFrames--;
    }
    return needsRender;
}

void FrameRender(Arena *arena, RenderTarget *target) {
    AppState *appState = (AppState *)arena->base;
    RendererSetContentRect(appState->renderer, UiContentOriginX(appState->ui),
                           UiContentOriginY(appState->ui), UiContentWidth(appState->ui),
                           UiContentHeight(appState->ui));
    RendererRender(appState->renderer, appState->game, target);
    UiRenderEncode(appState->uiRender, target, UiVertices(appState->ui),
                   UiVertexCount(appState->ui), UiDrawableWidth(appState->ui),
                   UiDrawableHeight(appState->ui));
}

void FrameResize(Arena *arena, float drawableWidth, float drawableHeight) {
    AppState *appState = (AppState *)arena->base;
    UiHandleResize(appState->ui, drawableWidth, drawableHeight);
    RendererSetContentRect(appState->renderer, UiContentOriginX(appState->ui),
                           UiContentOriginY(appState->ui), UiContentWidth(appState->ui),
                           UiContentHeight(appState->ui));
    appState->forcedRenderFrames = 3;
}

void AppRequestRender(Arena *arena) {
    AppState *appState = (AppState *)arena->base;
    appState->forcedRenderFrames = 3;
}

RendererPassTimings FrameGpuTimings(Arena *arena) {
    AppState *appState = (AppState *)arena->base;
    return RendererLastFrameTimings(appState->renderer);
}

void AppInvokeCommand(Arena *arena, CommandId id) {
    InvokeCommand((AppState *)arena->base, id);
}

CommandContext AppCommandContext(Arena *arena) {
    return AppStateContext((AppState *)arena->base);
}

bool ImportBlendFile(Arena *arena, const char *filepath) {
    AppState *appState = (AppState *)arena->base;
    bool imported = GameImportBlendFile(appState->game, filepath);
    if (imported) {
        appState->forcedRenderFrames = 3;
    }
    return imported;
}
