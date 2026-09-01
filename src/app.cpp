#include "app.h"

#include "audio.h"
#include "game.h"
#include "gizmo.h"
#include "scene.h"
#include "scene_import.h"
#include "timeline.h"
#include "ui.h"
#include "ui_render.h"

namespace {

// The very first thing placed in the arena, so FrameUpdate/FrameRender can
// always recover it from arena->base with no globals and no bookkeeping.
struct AppState {
    SceneState *scene;
    AudioState *audio;
    TimelineState *timeline;
    RendererState *renderer;
    UiState *ui;
    UiRenderState *uiRender;
    MenuState menu;
    PlatformMenuHooks menuHooks;
    bool fullscreen; // tracked from the last FrameInput, for CommandContext
    int forcedRenderFrames; // FrameUpdate reports "needs render" while this is > 0
    bool addMenuRequested;  // the 'n' key, handed to the outliner for one frame

    // Viewport interaction. FrameInput carries the mouse button as a level, so
    // the press/release edges are recovered from the previous frame's state.
    bool mouseLeftWasDown;
    GizmoHandle hoveredHandle;
    GizmoHandle activeHandle;
    GizmoDrag gizmoDrag;
    bool gizmoVisible;
    Vec3 gizmoPivot;
    bool boxSelecting;
    float boxAnchorX;
    float boxAnchorY;

    // Camera-drag latch: once an orbit/pan drag starts in the viewport, keep
    // feeding it even as the pointer crosses a panel. Plus a stale-hover guard
    // so panel/toolbar highlights repaint once the pointer leaves them.
    bool cameraButtonWasDown;
    bool cameraDragActive;
    bool uiHoverLastFrame;
    float lastMouseX;
    float lastMouseY;
};

// A drag shorter than this is a click on empty space, not a box select.
constexpr float kBoxSelectMinPixels = 3.0f;

CommandContext AppStateContext(AppState *appState) {
    return CommandContext{&appState->menu, appState->menuHooks, appState->fullscreen};
}

void InvokeCommand(AppState *appState, CommandId id) {
    CommandInvoke(id, AppStateContext(appState));
    appState->forcedRenderFrames = 3;
}

// Drawable pixels (top-left origin) -> the NDC rect SceneBoxSelect wants. The
// content rect comes from the UI so this matches the renderer's viewport.
NdcRect ScreenRectToNdc(AppState *appState, float x0, float y0, float x1, float y1) {
    float originX = UiContentOriginX(appState->ui);
    float originY = UiContentOriginY(appState->ui);
    float width = UiContentWidth(appState->ui);
    float height = UiContentHeight(appState->ui);

    float ndcX0 = 2.0f * (x0 - originX) / width - 1.0f;
    float ndcX1 = 2.0f * (x1 - originX) / width - 1.0f;
    float ndcY0 = 1.0f - 2.0f * (y0 - originY) / height;
    float ndcY1 = 1.0f - 2.0f * (y1 - originY) / height;

    NdcRect rect;
    rect.minX = ndcX0 < ndcX1 ? ndcX0 : ndcX1;
    rect.maxX = ndcX0 < ndcX1 ? ndcX1 : ndcX0;
    rect.minY = ndcY0 < ndcY1 ? ndcY0 : ndcY1;
    rect.maxY = ndcY0 < ndcY1 ? ndcY1 : ndcY0;
    return rect;
}

// Returns true when an entity was under the ray.
bool SelectEntityAt(AppState *appState, Ray ray, bool additive) {
    PickResult hit;
    if (ScenePickRay(appState->scene, ray, &hit)) {
        SelectionItem item = {SelectionKind_Entity, (uint32_t)hit.entity};
        if (additive) {
            SceneSelectionToggle(appState->scene, item);
        } else {
            SceneSelectionSet(appState->scene, &item, 1);
        }
        return true;
    }
    if (!additive) {
        SceneSelectionClear(appState->scene);
    }
    return false;
}

// Click-select, box-select and gizmo drags. Runs after RendererUpdateCamera so
// the picking ray uses this frame's camera.
void UpdateViewportInteraction(AppState *appState, FrameInput input) {
    SceneState *scene = appState->scene;
    ToolMode toolMode = SceneToolMode(scene);

    bool leftDown = input.mouseLeftDown;
    bool pressed = leftDown && !appState->mouseLeftWasDown;
    appState->mouseLeftWasDown = leftDown;

    Ray ray = RendererScreenPointToRay(appState->renderer, input.mouseX, input.mouseY);

    // An in-flight drag keeps running even if the pointer crosses a panel, so
    // releasing outside the viewport still commits one undo step.
    if (appState->gizmoDrag.active) {
        if (leftDown) {
            TransformDelta delta = GizmoUpdateDrag(&appState->gizmoDrag, ray);
            ScenePreviewTransformDrag(scene, delta);
            // The gizmo rides along with the entities it is moving; rotate and
            // scale leave the pivot where the drag started.
            appState->gizmoPivot = Vec3Add(appState->gizmoDrag.pivot, delta.translate);
        } else {
            SceneEndTransformDrag(scene, GizmoEndDrag(&appState->gizmoDrag, ray));
            appState->activeHandle = GizmoHandle::None;
        }
        appState->forcedRenderFrames = 3;
        return;
    }

    if (appState->boxSelecting) {
        if (!leftDown) {
            appState->boxSelecting = false;
            float dragX = input.mouseX - appState->boxAnchorX;
            float dragY = input.mouseY - appState->boxAnchorY;
            bool isBox = dragX * dragX + dragY * dragY >=
                         kBoxSelectMinPixels * kBoxSelectMinPixels;
            if (isBox) {
                NdcRect rect = ScreenRectToNdc(appState, appState->boxAnchorX,
                                               appState->boxAnchorY, input.mouseX, input.mouseY);
                SceneBoxSelect(scene, RendererViewProjection(appState->renderer), rect, input.shift);
            }
        }
        appState->forcedRenderFrames = 3;
        return;
    }

    Selection selection = SceneSelection(scene);
    appState->gizmoVisible =
        ToolModeHasGizmo(toolMode) && EntityIdValid(selection.activeEntity);
    appState->gizmoPivot = SceneSelectionCentroid(scene);

    if (UiWantsMouse(appState->ui)) {
        appState->hoveredHandle = GizmoHandle::None;
        return;
    }

    float scale = RendererGizmoScale(appState->renderer, appState->gizmoPivot);
    GizmoHandle handle = appState->gizmoVisible
                             ? GizmoHitTest(toolMode, appState->gizmoPivot, scale, ray)
                             : GizmoHandle::None;

    if (!leftDown && handle != appState->hoveredHandle) {
        appState->hoveredHandle = handle;
        appState->forcedRenderFrames = 3;
    }

    if (!pressed) {
        return;
    }

    if (handle != GizmoHandle::None) {
        SceneBeginTransformDrag(scene);
        appState->gizmoDrag = GizmoBeginDrag(toolMode, handle, appState->gizmoPivot, scale, ray);
        appState->activeHandle = handle;
        appState->forcedRenderFrames = 3;
        return;
    }

    // Clicking off the gizmo selects, in every tool mode — otherwise a
    // transform tool with an empty selection would be a dead end. A drag on
    // empty space marquee-selects, also in every mode.
    bool hitEntity = SelectEntityAt(appState, ray, input.shift);
    if (!hitEntity) {
        appState->boxSelecting = true;
        appState->boxAnchorX = input.mouseX;
        appState->boxAnchorY = input.mouseY;
    }
    appState->forcedRenderFrames = 3;
}

void PublishEditorState(AppState *appState) {
    UiEditorState editor = {};
    editor.toolMode = SceneToolModePtr(appState->scene);
    editor.requestAddMenu = appState->addMenuRequested;
    editor.timeline = appState->timeline;
    editor.scene = appState->scene;
    editor.audio = appState->audio;
    UiSetEditorState(appState->ui, editor);
    appState->addMenuRequested = false; // handed off; the outliner opened it this frame
}

} // namespace

void Init(Arena *arena, GpuContext *gpu, float drawableWidth, float drawableHeight,
          PlatformMenuHooks menuHooks) {
    AppState *appState = ArenaPushStruct(arena, AppState);
    appState->audio = AudioInit(arena, kAudioPcmPoolBytes);
    appState->scene = SceneInit(arena);
    appState->timeline = TimelineInit(arena);
    GameLoadDefaultScene(appState->scene);
    appState->renderer = RendererInit(arena, gpu, drawableWidth, drawableHeight);
    appState->ui = UiInit(arena, drawableWidth, drawableHeight);
    appState->uiRender = UiRenderInit(arena, gpu);
    UiSetFontMetrics(appState->ui, UiRenderFontMetrics(appState->uiRender));
    appState->menuHooks = menuHooks;
}

bool FrameUpdate(Arena *arena, float deltaTime, FrameInput input) {
    AppState *appState = (AppState *)arena->base;
    appState->fullscreen = input.fullscreen;

    TimelineUpdate(appState->timeline, input, appState->scene, deltaTime);
    bool playing = TimelineIsPlaying(appState->timeline);
    float simDeltaTime = playing ? deltaTime : 0.0f;
    bool sceneChanged = SceneUpdate(appState->scene, simDeltaTime);
    AudioUpdate(appState->audio, appState->scene, appState->timeline,
                TimelineTime(appState->timeline));

    PublishEditorState(appState);
    UiSetMarquee(appState->ui, appState->boxSelecting, appState->boxAnchorX, appState->boxAnchorY,
                 input.mouseX, input.mouseY);
    UiBuildFrame(appState->ui, input, AppStateContext(appState));

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
        if (key == ' ') {
            TimelineTogglePlay(appState->timeline);
            appState->forcedRenderFrames = 3;
            continue;
        }

        // Editor tool shortcuts. Handled here, not through the menu command
        // table, because they act on scene state the table's CommandContext
        // does not carry. Suppressed while a UI text field has the keyboard.
        if (!UiWantsKeyboard(appState->ui)) {
            int keyCode = input.keyEvents[i].keyCode;
            bool handled = true;
            if (key == '1') {
                SceneSetToolMode(appState->scene, ToolMode_Translate);
            } else if (key == '2') {
                SceneSetToolMode(appState->scene, ToolMode_Rotate);
            } else if (key == '3') {
                SceneSetToolMode(appState->scene, ToolMode_Scale);
            } else if (key == 127 || keyCode == 51 || keyCode == 117) { // Backspace / Fwd-Delete
                SceneDeleteSelection(appState->scene);
            } else if (key == 'n') {
                appState->addMenuRequested = true; // outliner opens its add-kind dropdown
            } else if ((key == '=' || key == '+') && (input.keyEvents[i].mods & ShortcutMod_Cmd)) {
                UiAdjustUiScale(appState->ui, 0.1f);
            } else if (key == '-' && (input.keyEvents[i].mods & ShortcutMod_Cmd)) {
                UiAdjustUiScale(appState->ui, -0.1f);
            } else if (key == '0' && (input.keyEvents[i].mods & ShortcutMod_Cmd)) {
                UiSetUiScale(appState->ui, 1.0f);
            } else if (key == 'z' && (input.keyEvents[i].mods & ShortcutMod_Cmd)) {
                if (input.keyEvents[i].mods & ShortcutMod_Shift) {
                    SceneRedo(appState->scene);
                } else {
                    SceneUndo(appState->scene);
                }
            } else {
                handled = false;
            }
            if (handled) {
                appState->forcedRenderFrames = 3;
                continue;
            }
        }

        const Command *command = CommandForShortcut(key, input.keyEvents[i].mods);
        if (command != nullptr) {
            InvokeCommand(appState, command->id);
        }
    }

    // Right/middle button drives orbit/pan. Once such a drag begins over the
    // viewport, keep feeding the camera even as the pointer sweeps across a
    // panel edge — otherwise the camera stutters on the border.
    bool cameraButtonDown = input.mouseRightDown || input.mouseMiddleDown;
    if (cameraButtonDown && !appState->cameraButtonWasDown && !UiWantsMouse(appState->ui)) {
        appState->cameraDragActive = true;
    }
    if (!cameraButtonDown) {
        appState->cameraDragActive = false;
    }
    appState->cameraButtonWasDown = cameraButtonDown;

    FrameInput cameraInput = input;
    if (UiWantsMouse(appState->ui) && !appState->cameraDragActive) {
        cameraInput.panX = 0.0f;
        cameraInput.panY = 0.0f;
        cameraInput.zoomDelta = 0.0f;
        cameraInput.orbitYaw = 0.0f;
        cameraInput.orbitPitch = 0.0f;
    }
    RendererUpdateCamera(appState->renderer, cameraInput);
    UpdateViewportInteraction(appState, input);

    // A pointer move on or just off UI chrome needs one more frame so the
    // panel/toolbar hover highlight repaints instead of freezing on-screen.
    bool mouseMoved =
        input.mouseX != appState->lastMouseX || input.mouseY != appState->lastMouseY;
    appState->lastMouseX = input.mouseX;
    appState->lastMouseY = input.mouseY;
    bool uiHover = UiWantsMouse(appState->ui);
    if (mouseMoved && (uiHover || appState->uiHoverLastFrame)) {
        appState->forcedRenderFrames = 2;
    }
    appState->uiHoverLastFrame = uiHover;

    bool cameraMoved = cameraInput.panX != 0.0f || cameraInput.panY != 0.0f ||
                       cameraInput.zoomDelta != 0.0f || cameraInput.orbitYaw != 0.0f ||
                       cameraInput.orbitPitch != 0.0f;
    bool renderToggled = input.cycleDebugView || input.toggleFxaa;
    bool uiInteracting = UiWantsMouse(appState->ui) || UiWantsKeyboard(appState->ui) ||
                         input.dragHovering;

    bool needsRender = sceneChanged || cameraMoved || renderToggled || uiInteracting || playing ||
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
    RendererSceneView view = {};
    view.scene = appState->scene;
    view.toolMode = SceneToolMode(appState->scene);
    view.gizmoVisible = appState->gizmoVisible;
    view.gizmoPivot = appState->gizmoPivot;
    view.hoveredHandle = appState->hoveredHandle;
    view.activeHandle = appState->activeHandle;
    RendererRender(appState->renderer, &view, target);
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
    bool imported = SceneImportBlendFile(appState->scene, filepath);
    if (imported) {
        appState->forcedRenderFrames = 3;
    }
    return imported;
}
