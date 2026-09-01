# Scene editor: audio, timeline, multi-select, undo — coordination plan

Single source of truth for the four agents building the 3D scene editor.
Read this, `CLAUDE.md`, and `docs/architecture.md` before writing code.
Report to the manager (main session) when blocked, when you need another
agent to land something first, or when a design question isn't answered here.

## Vision

Turn the renderer into a small 3D scene editor for spatial audio: place
audio sources and listeners in the scene, arrange `.wav` clips on a
timeline bound to those sources, scrub a playhead, and hear HRTF-spatialized
audio relative to the active listener. Multi-select, a unified undo/redo
stack, a viewport toolbar, and a centralized color palette round it out.

## Locked decisions

| # | Decision |
|---|----------|
| 1 | Four agents: Audio Andy, Render Expert Remus, Designer Dan, Engine Eric. |
| 2 | Scene storage = fixed-capacity pools in the arena with free lists. No heap allocator. Hard caps (below). |
| 3 | Audio backend = **miniaudio** (single-header, vendored to `src/third_party/miniaudio.h`). Andy writes the spatial mixer on top. No per-platform audio code. |
| 4 | Timeline tracks are **bound to scene audio sources**. A clip on a track = "this source emits this wav starting at time T". Playhead drives 3D-spatialized playback. |
| 5 | Spatial audio v1 = distance attenuation + panning **+ HRTF + geometry occlusion** (lowpass from listener→source raycast). No reverb/reflections v1. |
| 6 | Manipulation = viewport translate/rotate/scale **gizmos + box-select + shift-click**. Remus renders gizmos in the scene pass; Eric owns selection state and multi-select transform math. |
| 7 | Undo/redo = **one unified command stack** in Eric's scene module: entity add/delete/transform AND timeline clip/track edits. Dan and Remus push commands through Eric's API. Selection changes are **not** on the stack. |
| 8 | Persistence = **session-only** for v1. No save/load, no file format. Undo history is session-only. |
| 9 | Transport: the `p` key is **removed**. The default scene changes freely to fit testing needs. Timeline play/pause **is** the global sim clock — when paused, sim/animation time and audio playback freeze. Camera orbit/pan/zoom is an editor control and must still redraw the view while the timeline is paused. |
| 10 | Selection is **shared** across the viewport and the timeline: selecting a source highlights its track/clips and vice versa. One selection set in Eric's module spanning entities and clips. |
| 11 | Listeners: **multiple allowed, exactly one active** (marked like a DoP camera). Default scene spawns one active listener at the origin. |
| 12 | Color palette: **single code edit point**. Dan adds `src/theme.h` with a named palette table; every draw site reads from it. No runtime color-picker UI in v1. |

## Pool capacities (Eric owns; change only with manager sign-off)

```
entities        4096
mesh renderers  4096
audio sources    256
audio listeners   16
undo commands   1024   (ring buffer; oldest dropped)
wav clips        128   (Andy's AudioClip registry)
timeline tracks   64
timeline clips   1024
```

## Module ownership

| Owner | New / changed files | Responsibility |
|-------|--------------------|----------------|
| **Eric** | `src/scene.h`/`.cpp` (new), `src/math3d.h` (add quaternion), `src/game.*` (absorb/retire the 3-cube demo) | Entity + component pools, transforms, the shared selection set, box-select math, multi-select transform math, the unified command/undo stack, tool-mode enum, ray/AABB picking helpers used by Remus and Andy. |
| **Andy** | `src/audio.h`/`.cpp` (new), `src/third_party/miniaudio.h` (vendored) | miniaudio device init (from portable code, called in `Init`), `.wav` decode + resample to device rate, the `AudioClip` registry, `AudioSourceParams` struct, HRTF + distance + occlusion spatial mixer, a per-frame update that reads listener + source transforms from the scene and plays the clips the timeline says are active. |
| **Remus** | `src/renderer_metal.mm` / `src/renderer.h` (gizmo pass), a portable `src/gizmo.h`/`.cpp` for input→delta math | Translate/rotate/scale gizmo rendering in the scene pass; gizmo drag → transform delta → Eric's transform command; the "add audio source" / "add listener" toolbar actions (placement via Eric's pick ray); source/listener icons in the viewport. |
| **Dan** | `src/timeline.h`/`.cpp` (new), `src/ui.*` (timeline panel + toolbar), `src/theme.h` (new), platform drop-event plumbing in `src/platform_macos.mm` (thin: deliver dropped file paths + drop position into `FrameInput`) | Bottom timeline panel: tracks, drag-drop `.wav` onto tracks, move/trim clips, playhead scrub, transport (the global clock, decision 9). The 3D-editor toolbar shell + generic tool-button API. `theme.h` and routing every existing draw site through it. |
| **Manager** | `src/app.cpp`, `src/app.h`, `AppState` wiring | Integration: owns the `AppState` struct, `Init` order, and the `FrameUpdate`/`FrameRender` orchestration so `app.cpp` isn't a four-way merge. Agents hand the manager their `XInit`/`XUpdate` signatures. |

## Integration contracts (code against these; manager wires `app.cpp`)

Each module exposes an arena-allocated state struct and free functions,
matching the existing `RendererInit` / `UiInit` style. No globals, no
statics holding program state.

```c
// scene.h  (Eric)
SceneState *SceneInit(Arena *arena);
EntityId    SceneCreateEntity(SceneState*, EntityKind, const char *name);   // via command
void        SceneSubmitCommand(SceneState*, Command);                       // do + push to undo
void        SceneUndo(SceneState*);  void SceneRedo(SceneState*);
Selection   SceneSelection(const SceneState*);                              // shared entity+clip set
bool        ScenePickRay(const SceneState*, Ray, PickResult *out);          // Remus/Andy reuse
// iteration helpers for renderer + audio: SceneEntities(), SceneAudioSources(), SceneActiveListener()

// audio.h  (Andy)
AudioState *AudioInit(Arena *arena, size_t pcmPoolBytes);   // miniaudio owns the device; rate queried
int         AudioDeviceSampleRate(const AudioState*);
ClipId      AudioLoadClip(AudioState*, const char *wavPath);                // Dan's drop calls this
void        AudioUpdate(AudioState*, const SceneState*, const TimelineState*, double transportTime);
// AudioSourceParams { ClipId; gain; minDist; maxDist; rolloff; loop; spatial; mute; solo; }

// timeline.h  (Dan)
TimelineState *TimelineInit(Arena *arena);
bool           TimelineIsPlaying(const TimelineState*);   double TimelineTime(const TimelineState*);
void           TimelineUpdate(TimelineState*, FrameInput, SceneState*, double deltaTime); // scrub, transport, edits→commands

// ui: Dan adds the timeline panel + toolbar to the existing UiBuildFrame pass.
```

`FrameUpdate` gate (manager): advance sim/animation time only while
`TimelineIsPlaying`; always run camera + UI so the view redraws when paused.

## Resolved cross-agent decisions (manager, round 1)

- **`Command` is a generic variant** (Eric): `{ tag, void *payload (arena), void (*redo)(SceneState*, void*), void (*undo)(SceneState*, void*) }` alongside any typed helpers. Dan's `TimelineApply*` and Remus's gizmo edits are wrapped as redo/undo bodies — Eric does not extend an enum per request.
- **Audio PCM pool = 64 MiB**, reserved once in `AudioInit`; the `main` arena grows by +64 MiB (manager sizes it). Clips bump-allocate from the pool, no per-clip free, session-only. Overflow = drop + log.
- **All audio is spatial + mono in v1.** Stereo files downmixed to mono on load (all clips, not just spatial). Stereo music beds are out of scope (consistent with rejecting the hybrid-timeline option).
- **HRTF = parametric** (ITD + frequency-dependent ILD + pinna-notch biquads), no vendored HRIR dataset, no voice cap. Measured-HRIR upgrade path kept behind the internal voice-param struct.
- **Overlapping clips per source are allowed.** `TimelineActiveClips` returns N rows per source; Andy mixes N voices per source. `localOffset` is in seconds.
- **Clips play once in v1.** `AudioSourceParams.loop` is reserved but not wired to timeline playback. Track mute/solo is mirrored into `AudioSourceParams` by Dan each frame; Andy applies it.
- **`frame_input.h` drop fields** (Dan lands, platform fills, valid one frame):
  `const char *const *droppedFiles; int droppedFileCount; float dropX, dropY;`
- **theme.h**: Dan owns the header and every name. The 3 existing renderer colors (viewport bg, g-buffer clear, mesh base) are routed through it **by the manager** in `renderer_metal.mm` (shader-source `#define` prelude built from `theme::ToFloat`). Remus reads `theme.h` names directly in his gizmo/icon code. Dan does not edit `renderer_metal.mm`.
- **miniaudio pinned at 0.11.22** (verify it's the latest stable single-file tag at vendor time), `MINIAUDIO_IMPLEMENTATION` only in `audio.cpp`. Manager adds `src/audio.cpp` + `src/timeline.cpp` + `src/scene.cpp` + `src/gizmo.cpp` to the Makefile.
- **math3d.h**: declare `Mat4` (and `Mat4Identity`) before any `Quat`→`Mat4` helper — current WIP has an ordering error (`Mat4` unknown at line 70).

## Resolved cross-agent decisions (manager, round 2)

- **Gizmo drags use the purpose-built `SceneMakeTransformSelection`** (multi-select + pivot + single undo range), not the generic Command variant. The generic variant is for Dan's timeline ops only.
- **Eric adds a transform-drag lifecycle** so gizmo drags get live preview + one undo entry:
  `SceneBeginTransformDrag(SceneState*)` snapshots the selected transforms;
  `ScenePreviewTransformDrag(SceneState*, TransformDelta)` re-applies from the snapshot every frame, no undo push;
  `SceneEndTransformDrag(SceneState*, TransformDelta)` pushes exactly one command.
- **Eric gives non-mesh entities (audio source, listener) a synthetic pick AABB** (~0.3 world units or screen-constant) so `ScenePickRayExcluding` is the single pick path for clicks and icon selection.
- **`RendererRender` 2nd param becomes `const RendererSceneView*`** (replaces `const GameState*`): `{ const SceneState *scene; ToolMode toolMode; bool gizmoVisible; Vec3 gizmoPivot; GizmoHandle hoveredHandle, activeHandle; }`. No `AudioState*` field — selected-source min/max distance comes from the `AudioSourceParams` embedded in the scene's `AudioSource` component. `renderer.h` may include `scene.h` + `gizmo.h` (all pure C++). Manager builds the view each frame in `FrameRender`.
- **New renderer accessors** (camera lives in `RendererState`): `Ray RendererScreenPointToRay(const RendererState*, float x, float y)` and `float RendererGizmoScale(const RendererState*, Vec3 pivot)` — the manager calls these in `FrameUpdate` to feed `ScenePickRay` / `GizmoHitTest`.
- **`GameState` retirement is one atomic cutover**: Eric removes `GameState`, Remus switches the geometry pass to iterate `SceneMeshRenderers()` (mapping `MeshId_Cube`/`MeshId_Plane` onto the existing cube/plane buffers), manager re-wires `app.cpp` — landed together, coordinated through the manager, so the build is red for minutes not hours.
- **theme.h** gains `AudioDistanceSphere` for the selected-source min/max wireframe spheres. Remus's other names map to reserved entries (`GizmoAxisX/Y/Z`, `GizmoActive`, `AudioSourceIcon`, `ListenerIcon`, `ListenerActive`, `SelectionOutline`).

## Landed so far

- **`theme.h`** — final, all names grouped, `ui.cpp` routed through it (1:1 alias block, no draw site churned), `make` clean. The 3 renderer colours (`ViewportBackground`, `GBufferClear`, `MeshBase`) are still hard-coded in `renderer_metal.mm`; the manager routes them via a shader `#define` prelude **as part of the GameState→SceneState cutover landing**, not before (keeps concurrent `renderer_metal.mm` edits to one).
- **Toolbar shell** — `ui.h`: `UiTool_*` enum (matches `ToolMode_*`), `struct UiEditorState { int *toolMode; bool *snapEnabled; void(*addSource/addListener/frameSelected)(void*); void *context; }`, `UiSetEditorState(UiState*, UiEditorState)`. `ui.cpp`: data-driven `kToolButtonDef[]`, `BuildToolbar()` draws the strip, null callbacks render inert. `UiBuildFrame` signature unchanged.
- **Manager app.cpp wiring owed**: call `UiSetEditorState(ui, { SceneToolModePtr(scene), &appState->snapEnabled, <Remus add fns>, ctx })` once per frame before `UiBuildFrame`. Grow the arena +64 MiB for Andy's PCM pool. Add `src/audio.cpp`, `src/timeline.cpp`, `src/gizmo.cpp` to the Makefile (Eric already added `src/scene.cpp`).
- **Timeline→audio query** (Dan, final): `struct TimelineVoice { EntityId source; ClipId wav; double localOffset; float gain; TimelineClipId clip; }`; `int TimelineActiveVoices(const TimelineState*, double time, TimelineVoice*, int max)` + `…ForSource(…, EntityId, …)`. `localOffset = clip.trimIn + (time - clip.startTime)`, `gain` = clip gain only, past-end rows omitted.
- **`scene.h` + `math3d.h` FINAL, `make` green** (Eric). `math3d.h` adds `Quat` + full Vec3/Quat helper set, `Mat4` declared first. `scene.h`: `EntityId` = packed generation+slot int32, `Transform { Vec3 pos; Quat rot; Vec3 scale }`, generic `Command { int tag; void *payload; redo; undo }` (external tags ≥ `SceneCmdTag_UserBase = 1000`), shared `Selection` (entity + clip items, `activeEntity`), `SceneBoxSelect(viewProj, NdcRect, additive)`, `ScenePickRayExcluding`, `kNonMeshPickHalfExtent = 0.3f`, `SceneMeshRenderers/SceneAudioSources/SceneActiveListener` views, `SceneToolModePtr` for Dan's toolbar, drag lifecycle `SceneBeginTransformDrag/ScenePreviewTransformDrag/SceneEndTransformDrag`. `SceneInit` leaves the scene empty; `game.cpp` will expose `GameLoadDefaultScene(SceneState*)` (direct inserts, undo stack starts empty) at the cutover.

## Progress (manager tracking)

### Cutover LANDED (manager, solo — agents were rate-limited)

`GameState` is retired. The app runs on `SceneState` + `AudioState` + `TimelineState`. `make` clean, runs with no Metal validation errors.

- **Makefile**: `src/audio.cpp src/timeline.cpp src/gizmo.cpp` added to `SRC_CPP`; `-framework CoreAudio -framework AudioToolbox -framework AudioUnit` added.
- **`game.h`/`game.cpp`**: reduced to `GameLoadDefaultScene(SceneState*)` only. `GameState`, `GameInit`, `GameUpdate`, `GameToggleSpin`, `GameImportBlendFile`, `SceneObject`, `Primitive`, `kMaxSceneObjects` all deleted.
- **`scene_import.*`**: `SceneImportBlendFile(SceneState*, const char*)` — `SceneClear` then `SceneAddEntity` + `SceneSetMesh` per object, cube/plane-only, Blender-cube ×2 fixup kept, rotation via `QuatNormalize(QuatFromMat4(ConvertRotation(...)))`. Non-undoable replacement.
- **`renderer.h`/`renderer_metal.mm`**: `RendererRender` + `EncodeGeometryPass` take `const SceneState*`; geometry loop iterates `SceneMeshRenderers()` into a `static SceneMeshView[kMaxMeshRenderers]`, maps `MeshId_Cube`/`MeshId_Plane` onto the existing buffers. Added `Vec3 RendererCameraFocus(const RendererState*)` (orbit target — the drop point for `ToolAdd*`).
- **`frame_input.h`/`platform_macos.mm`**: `toggleSpin` field + `pendingToggleSpin` plumbing + the `p`-key handler removed. Arena grew 64 → **192 MiB** (Andy's 64 MiB PCM pool + scene/timeline pools + headroom).
- **`app.cpp`**: `AppState { SceneState*, AudioState*, TimelineState*, RendererState*, UiState*, UiRenderState*, ..., bool snapEnabled }`. Init order `AudioInit(arena, kAudioPcmPoolBytes)` → `SceneInit` → `TimelineInit` → `GameLoadDefaultScene`. Frame order `TimelineUpdate` → `simDt = TimelineIsPlaying ? dt : 0` → `SceneUpdate(scene, simDt)` → `AudioUpdate(audio, scene, timeline, TimelineTime())`. `PublishEditorState` calls `UiSetEditorState` every frame before `UiBuildFrame` with `SceneToolModePtr(scene)`, `&snapEnabled`, `AppAddSource`/`AppAddListener` (→ `ToolAddAudioSource`/`ToolAddListener` with a null ray + `RendererCameraFocus` fallback), `frameSelected = nullptr`, and `timeline`/`scene`/`audio`. **Spacebar** → `TimelineTogglePlay` (replaces the `p` key). `ImportBlendFile` → `SceneImportBlendFile`.
- **`scene.h`/`scene.cpp`/`timeline.cpp`**: the undo-command struct renamed `Command` → **`SceneCommand`** (collided with `menu.h`'s `Command` once both landed in `app.cpp`'s TU). `SceneSubmitCommand` name unchanged.

### Round 2 LANDED (agents Remus + Dan, worktrees merged onto `ui-panel`)

Combined `make` clean (`-Wall -Wextra`, no warnings); app runs under `MTL_DEBUG_LAYER=1` with no validation errors; `tests/gizmo_test.cpp` + `tests/math3d_test.cpp` both pass (standalone, not in the Makefile).

- **Gizmo interaction + draw** (commit `14e5194`): `math3d.h` gains `Mat4Inverse`. `renderer.h` gains `RendererScreenPointToRay`, `RendererViewProjection`, `RendererGizmoScale`, and `struct RendererSceneView { const SceneState*; ToolMode; bool gizmoVisible; Vec3 gizmoPivot; GizmoHandle hoveredHandle, activeHandle; }` — `RendererRender`'s 2nd param is now `const RendererSceneView*`. **`renderer.h` now `#include`s `gizmo.h`** (both pure C++). `renderer_metal.mm` adds an unlit overlay pipeline + per-frame vertex buffer, drawing `GizmoBuild` (T/R/S tool + selection) and `GizmoBuildIcons` (all sources + listeners) on top of the lit image (no depth test — scene depth is `DontCare` + content-sized). The three renderer colours now come from `theme.h` via a `#define` prelude on the shader source. `app.cpp` `UpdateViewportInteraction`: click-select (all tool modes), box-select (Select tool only), gizmo hover highlight, and gizmo drag (`SceneBeginTransformDrag`+`GizmoBeginDrag` → per-frame `ScenePreviewTransformDrag` → `SceneEndTransformDrag`), with mouse press/release edges recovered from the previous frame. `scene.h`/`scene.cpp` gain `SceneAudioListeners`/`SceneAudioListenerView` (read-only, for icon draw) and `SceneCreateEntityAt` (create-with-transform) — **`ToolAddAudioSource`/`ToolAddListener` are now one undo step each.**
- **Timeline panel + wav drop** (commit `d7cf448`): `frame_input.h` gains `droppedFiles`/`droppedFileCount`/`dropX`/`dropY` (appended; platform sets them by assignment so field order is safe). `platform_macos.mm` registers `NSPasteboardTypeFileURL`, filters `.wav`, latches paths + drop point for one frame. `ui.cpp` draws a bottom `UiDock_Bottom` "Timeline" panel: transport row (play/pause/stop + `mm:ss.cs`), per-second ruler with click-to-seek, grabbable playhead (`TimelineScrub`/`TimelineScrubEnd`), one lane per track (name + M/S indicators), clip rects with trim handles + selection tint. Clip body drag → `TimelineSubmitMoveClip` (cross-lane) on release; edge drag → `TimelineSubmitTrimClip` on release. `.wav` on a lane → `AudioLoadClip` + `TimelineSubmitCreateClip`; `.wav` below the lanes → `TimelineSubmitAddTrack` (bound to the selected source) + clip. Initial dock height 240.

**Behaviour changes to note**
- **`TimelineSubmitTrimClip`** (`timeline.cpp`) now slides `clipAfter.startTime` by `newTrimIn - oldTrimIn` so a left-edge trim is one undo step with `localOffset` held constant. Right-edge trims (unchanged `trimIn`) are unaffected. Only caller is `ui.cpp`.
- Gizmo overlay has **no depth test** — handles always draw over geometry (standard editor behaviour).
- **Click-select fires in every tool mode**, not just Select (box-select stays Select-only).

**Still deferred / cosmetic**
- Timeline mute/solo are drawn as indicators, not clickable — no `TimelineSubmitSetMute`-style API yet (`TimelineUpdate` mirrors the flags into `AudioSourceParams`).
- Gizmo screen size `kGizmoPixelSize = 80` drawable px reads slightly small; one constant to bump.
- `hoveredHandle` not refreshed mid non-gizmo drag (stale until release).
- Overlay + uniform buffers are CPU-written each frame with no ring/triple-buffering (matches the pre-existing `uniformBuffer` pattern).
- **wav-drop path is untested** — can't drive a real Finder drag headlessly. Needs a manual check.

### Tooling issue observed

Both round-2 worktrees were provisioned off `main` (README history), not `ui-panel` — none of the scene-editor files were present. Both agents reset their branch to `ui-panel` (`72b2e1e`) before starting; their branches had no commits of their own and their trees were clean, so nothing was lost and both merged cleanly. Flagging the worktree base-commit selection for a look.

**What works now**: default scene renders (ground + 2 cubes) with source/listener icons; orbit camera; toolbar switches tool mode; click to select, drag-box to multi-select, gizmo to translate/rotate/scale (one undo step per drag); Add Source / Add Listener (one undo step); spacebar drives the transport; bottom Timeline panel with working transport + ruler + playhead + lanes/clips; `AudioUpdate` every frame (audible once a clip is dropped and the transport plays).

## Resolved cross-agent decisions (manager, round 3)

- Transform op name stays **`SceneMakeTransformSelection`** (plan wins over the earlier `SceneTransformSelection` note).
- **Default scene is not undoable** — built via direct pool inserts, undo stack empty at startup. Confirmed.
- **`p` key / `GameToggleSpin`**: Eric drops `GameToggleSpin` from `game.h` at the cutover; manager removes `input.toggleSpin` handling from `app.cpp`; the `frame_input.h` field + platform plumbing are cleaned in the same cutover landing (Dan is already in both files for drop events).
- **Generic-command payload lifetime**: submitter arena-allocates; a ring-evicted command's payload leaks for the session. Payloads are tens of bytes and human-paced — acceptable for v1, no pooling required.
- **Blend import — KEEP, reroute onto `SceneState`, cube/plane-only** (resolved). The existing `scene_import.cpp` already classifies every Blender mesh object as `Plane` (4 verts) or `Cube` (else) and maps it onto the built-in unit meshes — it never supported arbitrary geometry, so there is no dynamic-mesh work. Eric owns the reroute (lands in the cutover):
  - `scene_import.h`: `bool SceneImportBlendFile(SceneState*, const char *filepath)`.
  - `scene_import.cpp`: keep the `.blend` parsing + vert-count classification + Z-up→Y-up conversion; replace the `state->objects[i]` writes with `SceneCreateEntity(EntityKind_Mesh, name)` + set `Transform` + set the `MeshRenderer.mesh` to `MeshId_Cube`/`MeshId_Plane`. Keep the Blender-default-cube ×2 scale fixup.
  - Import = **non-undoable scene replacement** (clear entities + clear undo stack, then load), same model as `GameLoadDefaultScene`.
  - `math3d.h`: add `QuatFromMat4` (the importer builds rotation as a `Mat4`; `Transform.rotation` is a `Quat`).
  - `app.cpp` `ImportBlendFile` routes here; the File > Import menu command stays enabled.

## Dependency order

1. **Eric** publishes `scene.h` + `math3d.h` quaternion additions ASAP — everyone else codes against them. **Andy** starts in parallel (miniaudio vendor, decode, mixer, `audio.h`).
2. **Remus** and **Dan** code against the contracts above, stubbing Eric's/Andy's calls locally until the real headers land, then integrate.
3. Manager wires `app.cpp` as each module delivers; runs the integrated build.

## Coordination rules

- One owner per file (table above). Need an edit in someone else's file? Ask the manager to route it; don't edit it directly.
- Shared headers (`math3d.h`, `frame_input.h`, `app.h`, `renderer.h`): announce the change to the manager before landing it.
- Every scene/timeline mutation goes through `SceneSubmitCommand` so it's undoable as one step. Multi-select transforms = one command covering all selected.
- Portability: keep `platform_macos.mm` a thin shim (deliver events/paths, nothing more). Audio, timeline, scene, gizmo math are pure C++.
- All colors come from `theme.h`. No literal `MTLClearColor` / `UiColor` hex anywhere else after Dan's pass.
- Fixed-capacity pools only. On overflow: drop + log, never grow.
- Build must stay green (`make`) after each integration. Verify with `make run`: default scene loads, a source + active listener present, timeline panel at the bottom, toolbar in the viewport, transport play/pause drives sim + audio, camera still moves while paused, no Metal validation errors.
