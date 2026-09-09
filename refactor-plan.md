# Refactor plan

## Problem

The scene-editor headers carry heavy prose comments. Most explain real design
contracts, but a large fraction explain around API seams that could just be
tighter, or are stale process notes ("the manager", "decision 9", "plan round
1", "before scene.h lands"). Verb/noun naming diverged between the modules that
were built in parallel. Nothing is broken; this is structure only.

## Verification (behavior must not change)

- `make` clean, no new warnings.
- `clang++ -std=c++17 -Wall -Wextra -I src tests/gizmo_test.cpp src/gizmo.cpp -o build/gizmo_test && build/gizmo_test`
- `clang++ -std=c++17 -Wall -Wextra -I src tests/math3d_test.cpp -o build/math3d_test && build/math3d_test`
- `make run`, default scene renders, orbit + toolbar + timeline + gizmo drag work, no Metal validation errors.

Run all four after every step. Weak net (2 unit tests + manual), so steps are
ordered small and each compiles + passes on its own.

## Gray areas (resolved)

1. **scene <-> timeline undo coupling.** SPLIT: new `undo_stack.h`/`.cpp` owned
   by neither. `scene.cpp` and `timeline.cpp` both include it.
2. **`ClipId` rename.** YES: audio `ClipId` -> `WavId` (18 sites).
3. **Iteration convention.** UNIFY on fill-buffer: timeline `Count()`+`At()` ->
   `int TimelineTracks(out, maxOut)` / `int TimelineClips(out, maxOut)`.

Baseline: `tests/gizmo_test.cpp` `TestBuildIcons` was stale (pre-`e7d2e98` icon
geometry); repinned to current counts. Now green.

Nothing committed; incremental `make` + tests after each step is the safety net.

## Steps

### A. Strip stale process comments (no code change, safe)
References to remove or rewrite to the actual invariant:
- "the manager" -> name the caller (platform layer / app.cpp frame loop) or drop
- "decision 9 / 11", "plan round 1/2", "manager sign-off", "before scene.h lands"
- "still to be reserved with the timeline module"
Files: scene.h, audio.h, timeline.h, gizmo.h, ui.h, theme.h, and matching .cpp
top-of-file blocks.
Serves: readability. Verify: build + tests.

### B. One verb for an undoable edit
- Drop the `Submit` infix: `TimelineSubmitAddTrack` -> `TimelineAddTrack`,
  `TimelineSubmitCreateClip` -> `TimelineAddClip`, `...DeleteClip/MoveClip/TrimClip`
  -> `TimelineDeleteClip` etc. (undo-ness stays in the section header).
- `Add` vs `Create`: use `Add` for tracks and clips; keep scene's
  `SceneCreateEntity*` (an entity is created, not added to a list).
- `SceneMakeTransformSelection` -> `SceneTransformSelection`.
- Keep `SceneSubmitCommand` (that one literally submits a generic command).
Serves: consistent verbs. Verify: `make` catches every missed call site.

### C. One verb for id lookup
`SceneGetEntity/GetEntityTransform/GetAudioSourceParams` -> `SceneFindEntity/...`
to match `TimelineFindTrack/FindClip` ("by id, null/false if missing"). 15 sites.
Serves: consistent verbs.

### D. Remove the `int* toolMode` alias
Delete `SceneToolModePtr`, `UiEditorState.toolMode`, and the `UiTool_*` enum
that mirrors `ToolMode_*`. The toolbar already holds `editor.scene`; it calls
`SceneToolMode` / `SceneSetToolMode` directly. Deletes 3 comments and one
representation leak. ~27 sites. Verify: toolbar still switches modes in `make run`.

### E. Enum style consistency
`enum class GizmoHandle` -> unscoped `enum GizmoHandle { GizmoHandle_None, ... }`
to match every other enum in the tree. 56 sites, but gizmo_test covers the math.

### F. (optional, pending gray area 2) `ClipId` -> `WavId`

## Status — all steps landed, build + 4 tests green

| step | what changed |
|------|--------------|
| A | stripped "the manager" / "decision N" / "plan round" / "before X lands" process comments; "the manager" -> app.cpp; undid the clunky "the X module" phrasing from the persona edit |
| B | dropped the `Submit` infix: `TimelineSubmitAddTrack`->`TimelineAddTrack`, `...CreateClip`->`TimelineAddClip`, etc; `SceneMakeTransformSelection`->`SceneTransformSelection` |
| C | `SceneGetEntity*` -> `SceneFindEntity*` to match `TimelineFindTrack/FindClip` |
| D | deleted `SceneToolModePtr`, `UiEditorState.toolMode` (int*), and the `UiTool_*` enum; toolbar now calls `SceneToolMode` / `SceneSetToolMode` through `editor.scene`; `SceneState::toolMode` is a real `ToolMode`, not an aliased int |
| E | `enum class GizmoHandle` -> unscoped `enum GizmoHandle { GizmoHandle_None, ... }` to match every other enum |
| F | audio `ClipId` -> `WavId` (+ `kInvalidWavId`, `WavIdValid`, `AudioLoadWav`, `AudioWavDuration`, `kWavRegistryCapacity`, `AudioSourceParams.wav`) so it no longer collides with `TimelineClipId` |
| G | new `src/undo_stack.h`/`.cpp`: a neutral fixed-capacity undo ring, owned by neither scene nor timeline, storing each payload by value. `SceneCommand` / `SceneSubmitCommand` / `SceneCmdTag_*` removed. `scene.cpp` keeps one `UndoStack*` and exposes `SceneUndoStack`; `timeline.cpp` submits straight to it and dropped its parallel `payloads[]` ring + `NextPayload` + `MakeCommand`. Handlers take `(void *context, void *payload)`. |
| H | timeline `Count()`+`At()` -> `int TimelineTracks(out,maxOut)` / `int TimelineClips(out,maxOut)` with `TimelineTrackRow` / `TimelineClipRow`, matching `SceneMeshRenderers` etc; removed the O(n^2) panel loop |

New characterization tests (standalone, not in the Makefile):
- `tests/scene_undo_test.cpp` — scene create / transform / delete / redo-branch, undo+redo
- `tests/timeline_undo_test.cpp` — clip+track edits share the scene history; move-clip undo

Not done: `docs/*.md` still name the old APIs (they record the round-1/2 plan as
executed; left as history).
