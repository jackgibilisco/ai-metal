# Gizmo + viewport-icon plan (the renderer)

## Problem

The scene editor needs on-viewport manipulation: translate/rotate/scale
gizmos over the current selection, click + box picking, and visible,
clickable icons for empty entities (audio sources, listeners). All
transform edits must land as single undoable commands through the scene module. Gizmo hit-testing and drag->delta math must be pure C++ and
headless-testable; only the Metal draw calls live in `renderer_metal.mm`.

## Ownership (from scene-editor-plan.md)

- `src/gizmo.h` / `src/gizmo.cpp` (new) - pure C++: screen-constant sizing,
  handle hit-testing, drag->`GizmoDelta`, gizmo/icon geometry builders,
  ground-plane placement, the "add audio source"/"add listener" actions.
- Additions in `src/renderer_metal.mm`: one unlit overlay pipeline + pass,
  drawn last onto the content viewport; a per-frame overlay vertex buffer.
- `src/renderer.h`: `RendererRender` signature change (announce to manager).

## Dependencies / blockers

- the scene module `scene.h`: `SceneState`, `EntityId`, `ToolMode` enum, `Selection`,
  `Ray`, `PickResult`, `ScenePickRay`, `SceneCreateEntity`, `Command` +
  `SceneSubmitCommand`, iteration helpers (`SceneEntities`,
  `SceneAudioSources`, `SceneActiveListener`), selection pivot.
- the scene module `math3d.h`: quaternion (`Quat`, `QuatFromAxisAngle`, `QuatMul`,
  `QuatToMat4`) and `Mat4RotationAxis`. Gizmo output avoids needing these by
  emitting axis-angle; the scene module's transform command consumes it.
- the audio module `audio.h`: `AudioSourceParams { minDist, maxDist, ... }` for the
  selected-source distance spheres.

Until these land: code `gizmo.cpp` against the signatures below, stub the
scene/audio calls in the headless test, keep the Metal code behind
`EncodeOverlayPass` taking plain vertex arrays.

## Design decisions (updated after scene.h / audio.h / theme.h landed)

| # | Decision |
|---|----------|
| 1 | Use the scene module's `ToolMode` directly (`ToolMode_Select/Translate/Rotate/Scale`); no local mode enum. `ToolModeHasGizmo()` helper filters out Select. |
| 2 | `Ray` lives in `scene.h` (the scene module). `gizmo.h` includes `scene.h` (pure C++: it only pulls `arena.h` + `audio.h`, no Metal). The math/geometry functions still touch no `SceneState`, so the headless test compiles with just the headers + a tiny scene stub for the two tool actions. |
| 3 | One gizmo at the selection pivot = `SceneSelectionCentroid` (also `TransformDelta.useCentroidPivot`). Multi-select: one `TransformDelta` applied to every selected entity by the scene module's command. |
| 4 | Screen-constant size: `GizmoWorldScale(pivot, cameraEye, fovY, viewportHeightPx, desiredPx=90)`. Same fn renderer-side (draw) and manager-side (hit-test). |
| 5 | Drag model: cumulative-from-anchor. `GizmoBeginDrag` captures the anchor; `GizmoUpdateDrag` returns the total `TransformDelta` since drag start (pivot carried in it). the scene module snapshots selected transforms on begin, previews each frame (not undoable), pushes ONE command on end. Manager wires begin/preview/end; the renderer supplies the delta. |
| 6 | `GizmoUpdateDrag` returns the scene module's `TransformDelta` (`translate`, `rotate` Quat, `scale` multiplier, `pivot`, `useCentroidPivot`). Rotate accumulates an unwrapped angle in `GizmoDrag` and emits `QuatFromAxisAngle(axis, angle)`. |
| 7 | Handles v1: 3 axis handles (line + arrowhead / ring / line + box) + centre box = uniform scale. `PlaneXY/YZ/ZX` are in the enum, implemented as a stretch goal. |
| 8 | Overlay rendering: one unlit pipeline, vertex `{ float pos[3]; float rgba[4]; }`, alpha-blended, drawn last on the content viewport. Lines for axes/rings/wire-spheres + triangle fans for arrowheads and billboards. No depth test (painter's order, axes sorted back-to-front). Hovered handle in theme.h `GizmoActive`. |
| 9 | Icons: camera-facing billboard quads (renderer supplies camera right/up). Colours from theme.h `AudioSourceIcon` / `ListenerIcon` / `ListenerActive`; selected source outline `SelectionOutline`; two wireframe distance spheres need a new theme name (request to the timeline module). Icons must be pick targets - see open question 3. |
| 10 | "Add" actions place on `y = 0` under the cursor via `RayGroundIntersect`; fallback `cameraFallbackPoint` (manager passes camera eye + 6*forward). New listener activated only if it is the first (no existing listeners). Through the scene module's create command so it's undoable. |
| 11 | `ToolAddAudioSource` / `ToolAddListener` live in `gizmo.h`/`.cpp`. Manager may split to `tool_actions.*`. |
| 12 | Colours: `gizmo.cpp` is theme-agnostic - callers fill `GizmoColors` / `IconColors` from theme.h. No colour literal in `gizmo.cpp` or the new `renderer_metal.mm` code. |

## Renderer integration (proposal to manager)

Replace `const GameState *game` in `RendererRender` with a per-frame view
struct the manager builds in `FrameRender` from `SceneState` + `AudioState`
+ the hover/drag result computed in `FrameUpdate`:

```c
// renderer.h
struct RendererSceneView {
    const SceneState *scene;      // iterate entities / audio sources / listener
    const AudioState *audio;      // selected-source min/max distance
    GizmoMode   gizmoMode;
    bool        gizmoVisible;     // selection non-empty and mode != Select
    Vec3        gizmoPivot;
    GizmoHandle hoveredHandle;    // highlight
    GizmoHandle activeHandle;     // mid-drag
};
void RendererRender(RendererState*, const RendererSceneView*, RenderTarget*);
```

Plus two accessors so the manager can build a world ray / gizmo scale in
`FrameUpdate` without duplicating camera state (camera lives in
`RendererState` today):

```c
Ray   RendererScreenPointToRay(const RendererState*, float mouseX, float mouseY);
float RendererGizmoScale(const RendererState*, Vec3 pivot);
```

Open question for manager/the scene module: keep camera in the renderer (these
accessors), or hoist camera to a shared `CameraState` in `scene.h` that
both `ScenePickRay` and the renderer read. Either works for the renderer.

## Steps

1. [done] `docs/gizmo-plan.md` + `src/gizmo.h`; renderer proposal approved
   (round-2 decisions in `docs/scene-editor-plan.md`).
2. [done] `src/gizmo.cpp`: sizing, hit-test (axis/ring/uniform), drag
   lifecycle (`GizmoBeginDrag` / `GizmoUpdateDrag` -> `ScenePreviewTransformDrag`,
   `GizmoEndDrag` -> `SceneEndTransformDrag`), `RayGroundIntersect`, the two
   tool actions. Geometry builders `GizmoBuild` / `GizmoBuildIcons`.
3. [done] `tests/gizmo_test.cpp` headless (17 groups, hand-rolled asserts,
   one-off `clang++`, not in the Makefile). Fake `SceneState` surface inline.
   Command: `clang++ -std=c++17 -Wall -Wextra -I src tests/gizmo_test.cpp
   src/gizmo.cpp -o build/gizmo_test && build/gizmo_test`.
4. [blocked on cutover] `renderer_metal.mm`: overlay pipeline +
   `EncodeOverlayPass` + per-frame buffer; call it after lighting/FXAA on
   the content viewport. `renderer.h`: `RendererSceneView`,
   `RendererScreenPointToRay`, `RendererGizmoScale`. Geometry pass switches
   to `SceneMeshRenderers()`. Lands only in the manager's coordinated
   GameState -> SceneState cutover.
5. Manager wires `FrameUpdate` (hover via `GizmoHitTest`, drag lifecycle)
   and `FrameRender` (`RendererSceneView`) and the toolbar `add*` callbacks
   to `ToolAddAudioSource` / `ToolAddListener`.

## Known follow-ups / notes for the manager

- `ToolAddAudioSource` / `ToolAddListener` are TWO undo steps today
  (`SceneCreateEntity` then `SceneSetEntityTransform`). One-step would need
  a create-with-transform in `scene.h`. Not blocking; flag if it matters.
- `GizmoColors` gained a `uniform[4]` field (centre scale box when not
  hovered) beyond the approved set - renderer fills it from theme.h
  (`SelectionOutline` or a dimmed `GizmoActive`); no new theme name needed.
- Hit-test tolerances (`kAxisPickRadius` etc.) are fractions of the
  screen-constant `scale`, so they track on-screen size (~11 px at the
  default 90 px gizmo). Tune in `gizmo.cpp` after seeing it live.
- Plane-translate handles (`PlaneXY/YZ/ZX`) are declared but not built or
  hit-tested yet - stretch goal after the cutover.

## Test plan

- Headless (`tests/gizmo_test.cpp`): sizing monotonic in distance;
  hit-test picks the right axis for rays aimed down each handle and `None`
  for a ray into empty space; translate-along-axis delta equals the
  projected pointer displacement; rotate delta sign/magnitude for a known
  ray sweep; uniform-scale factor = distance ratio; `RayGroundIntersect`
  hit + parallel-miss fallback.
- In-app (`make run`, after integration): gizmo tracks selection at
  constant pixel size while orbiting/zooming; hovered axis highlights; drag
  moves all multi-selected entities as one undo step; source/listener icons
  visible + clickable; active listener visually distinct; selected source
  shows min/max spheres.
