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
AudioState *AudioInit(Arena *arena, int deviceSampleRate);
ClipId      AudioLoadClip(AudioState*, const char *wavPath);                // Dan's drop calls this
void        AudioUpdate(AudioState*, const SceneState*, const TimelineState*, double transportTime);
// AudioSourceParams { ClipId; gain; minDist; maxDist; rolloff; loop; spatial; mute; solo; }

// timeline.h  (Dan)
TimelineState *TimelineInit(Arena *arena);
bool           TimelineIsPlaying(const TimelineState*);   double TimelineTime(const TimelineState*);
void           TimelineUpdate(TimelineState*, FrameInput, SceneState*);     // scrub, transport, edits→commands

// ui: Dan adds the timeline panel + toolbar to the existing UiBuildFrame pass.
```

`FrameUpdate` gate (manager): advance sim/animation time only while
`TimelineIsPlaying`; always run camera + UI so the view redraws when paused.

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
