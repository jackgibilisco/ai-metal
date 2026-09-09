# Design notes: macOS Metal renderer → spatial-audio scene editor

Design decisions behind the current code, one section per subsystem. History,
step-by-step logs, and superseded intermediate designs are omitted — only what
the code does now. Editor-specific detail is in `docs/scene-editor-plan.md`;
per-file contracts are in `docs/architecture.md`.

## Core contract

- **One arena.** Flat bump allocator over a single `calloc`'d block. `ArenaPush`
  is only ever called during `Init`; nothing is freed (the OS reclaims on exit).
- **Three entry points**, called by the platform layer: `Init` once, then
  `FrameUpdate(dt, FrameInput) -> needsRender` every tick and
  `FrameRender(RenderTarget*)` only when that returns true. `FrameUpdate` is
  pure portable C++; `FrameRender` touches only the graphics API, never AppKit.
- **No globals.** `Init` pushes `AppState` (pointers to the module states) as
  the first thing in the arena; every call recovers it from `arena->base`.
- **Shader source is an embedded string**, compiled at runtime with
  `newLibraryWithSource:` — no `xcrun metal` build step, `clang++` alone builds.
- **ARC-in-arena gotcha.** State structs hold ARC `id<MTL...>` fields but live
  in raw arena memory. The arena must be `calloc`'d so the first assignment to
  each `id` sees `nil`, not garbage, when `objc_storeStrong` releases the
  previous value.
- The original demo `GameState` (3 spinning cubes) was retired for
  `SceneState` / `Audio` / `Timeline`; `game.cpp` now only loads the default
  scene.

## Blender `.blend` import (File > Import File...)

Transform-only: read each Object's position/rotation/scale, classify its Mesh
as a plane (4 verts) or cube (else), and render with the built-in unit meshes.
Per-vertex data is never read. Loads into `SceneState`.

- `src/blend_file.h/.cpp` — portable C++ (parallel to `math3d.h`), depends on
  Homebrew libzstd. Owns zstd decompression, block/SDNA parsing, and a by-name
  field-read API. The whole file is one zstd stream.
- Blender 5.0+ only (17-byte new-format header; `LargeBHead8` 32-byte block
  headers). The `DNA1` block's SDNA table gives each struct member's byte
  offset, so field reads survive layout changes across Blender versions. Fields
  read: `Object.type/data/loc/rotmode/quat/rot/scale`, `Mesh.verts_num`.
- **Coordinate conversion** (done once at import; the renderer never knows
  "Blender" exists). Blender Z-up → renderer Y-up:
  - Position `(x,y,z) → (x, z, -y)`; scale same axis swap, magnitude only.
  - Rotation: build `R` in Blender's convention (`Rz·Ry·Rx`, or quat→matrix),
    then `C·R·Cᵀ` with the fixed change-of-basis `C`.
  - Blender's default cube is 2×2×2 vs the engine's unit cube, so imported cube
    scale is doubled; the built-in plane is authored at 2×2 to need no fixup.
- Limitations: Euler-XYZ and quaternion rotation modes only (others fall back to
  reading `rot[3]` as XYZ); classification is vertex-count-based; object count
  clamped to the scene capacity.

## Orbit camera

State and update live in the renderer (a camera is a rendering concern):
`cameraTarget` + `distance` + `yaw` + `pitch`, `eye = target + distance ·
(cos p·sin y, sin p, cos p·cos y)`, looking at `target` with world-up.

- Gestures are `NSEvent` input, so only `platform_macos.mm` reads them
  (`scrollWheel:` for two-finger drag, routed to orbit when Shift is held;
  `magnifyWithEvent:` for pinch). It accumulates per-frame deltas into
  `FrameInput` (plain floats); `FrameUpdate` forwards them to
  `RendererUpdateCamera`, which holds all sensitivity/clamp constants.
- Pan moves `cameraTarget` along screen-space right/up (derived algebraically
  from yaw/pitch), scaled by `distance` so speed tracks zoom. Distance clamped
  `[2.5, 60]`; pitch clamped to ±~86° to not flip through the poles.
- The projection matrix is rebuilt only on a drawable-size change; a camera
  move rebuilds only the view matrix.

## Resizable window

Window has `NSWindowStyleMaskResizable`. `mtkView:drawableSizeWillChange:`
routes the new size to `RendererSetContentRect`, which rebuilds `projection`
from `drawableWidth / drawableHeight` (pixels — the right ratio for a plain
perspective matrix) and recomposes `viewProjection`.

## Deferred renderer + SSAO + FXAA

One command buffer per frame, encoded in `RendererRender`:

1. **Geometry** → one `RGBA16Float` target: `xyz` = view-space normal,
   `w` = view-space Z (negative for real geometry; the pass clears `w` to 1.0,
   so `w ≥ 0.5` means background). Plus a private depth target. Per-object
   uniform `{modelViewProjection, modelView}`.
2. **AO** (full-screen triangle) → `R8Unorm` at half resolution
   (`(w+1)/2 × (h+1)/2`; the lighting pass samples it with a linear sampler so
   upscale is free). Hemisphere kernel: 32 sample offsets generated once at
   init (biased toward the origin), rotated per-pixel by a tiled 4×4 noise
   texture. `ReconstructViewPosition(uv, viewZ, projection)` rebuilds view X/Y
   from the fullscreen-triangle uv and `projection[0][0]/[1][1]` — no matrix
   inverse. Each sample projects back to screen space and depth-compares
   against the normal target's `w`; `rangeCheck = smoothstep(0, 1, radius /
   |fragZ − occluderZ|)` rejects occluders across large depth gaps.
   `ao = pow(max(1 − occlusion/N, 0), kAoPower)`.
   - **Per-pixel radius jitter:** each pixel scales its kernel radius by the
     noise tile's `w` channel (`0.75..1.25×`), so a flat uniformly-angled
     surface doesn't self-occlude identically everywhere and band into
     concentric rings. `BuildNoiseTexture` fills `w` with `RandomUnit()`.
3. **Lighting** (full-screen) → the drawable, or `litColorTexture` when FXAA is
   on. Reads the normal target and AO; folds in a 4×4 box blur of the half-res
   AO (`LightParams.misc.yz` = `1/aoWidth, 1/aoHeight`). Shades
   `base · (0.35·ao + 0.65·diffuse)`, light direction transformed to view space
   on the CPU. Background texels get the viewport clear color.
4. **FXAA** (full-screen, only when enabled) → the drawable. Compact FXAA: luma
   = `dot(rgb, (0.299, 0.587, 0.114))`, 3×3 luma corners → edge direction, up
   to 4 taps along it. Only uniform is `1/screenSize`.

- Screen-sized targets (`gNormalTexture`, `aoRawTexture`, `litColorTexture`,
  depth) are the one resource set recreated outside `Init` —
  `AllocateScreenTargets` runs on an actual size change. Metal allocations, not
  `ArenaPush`, so the arena invariant holds.
- Tuning constants (`kAoRadius/Bias/Power`) are at the top of
  `renderer_metal.mm`. `o` cycles a debug view (0 normal / 1 raw AO / 2 AO
  off); `f` toggles FXAA. When AO is off the AO pass is skipped entirely.
- Known limits: single world-space AO radius tuned for ~1-unit geometry;
  normals use `modelView · normal` with no inverse-transpose (skewed AO under
  non-uniform scale); no temporal denoise.

## Frame timing HUD

`F3` toggles a top-left AppKit overlay (`DebugHudView : NSView`, `hitTest:`
returns nil so drags pass through). `src/frame_stats.h` (header-only) is a
240-sample ring of ms frame times with mean and 1%-low (99th-percentile)
helpers. Frame times are wall-clock deltas between present callbacks. The HUD
also shows per-pass GPU time: `RendererState` holds a `MTLCounterSampleBuffer`;
each pass descriptor samples at stage boundaries into fixed slots
(geometry 0 / ao 1 / lighting 2 / fxaa 3); a completion handler resolves them
into `passMs` (a benign cross-thread race, fine for a debug readout). Total
comes from `GPUEndTime − GPUStartTime`.

## Display link and fullscreen

The `MTKView` draw loop and an `NSView` `CADisplayLink` both cap at 120 Hz on
macOS regardless of the frame-rate hints. So the `MTKView` is paused and frames
are driven by a `CAMetalDisplayLink` on its `CAMetalLayer`, whose
`needsUpdate:` callback runs `FrameUpdate` + `FrameRender`.
`preferredFrameRateRange` is pinned to `NSScreen.maximumFramesPerSecond` for
the current display.

Fullscreen is **borderless-window**, not AppKit/Spaces fullscreen (which
throttles the display link to 120 Hz with no override): swap to
`NSWindowStyleMaskBorderless`, hide the dock/menu bar. `AppWindow` overrides
`canBecomeKey/MainWindow` (borderless windows refuse key status by default).
The window is sized to **overhang the screen by 1px** (`NSInsetRect(frame, -1,
-1)`) — covering the display exactly triggers macOS direct scanout, which again
pins to 120 Hz. The paused `MTKView` won't update `CAMetalLayer.drawableSize`
on its own, so the layer size is set explicitly on resize and after the toggle.

## Idle / visibility throttling

- **Not visible → pause the frame loop.** `CAMetalDisplayLink.paused` tracks
  `NSApp.active && windowVisible && !miniaturized`, driven by the occlusion /
  activation / miniaturize notifications. On resume, `lastTime` is zeroed so
  the first frame doesn't integrate the whole gap.
- **Nothing changed → skip the render.** `FrameUpdate` returns false when the
  scene didn't animate, the camera didn't move, no toggle fired, and the UI
  isn't being interacted with (`UiWantsMouse || UiWantsKeyboard`). The last
  drawable stays on screen. `FrameUpdate` still runs every tick.
- `AppRequestRender` sets a small `forcedRenderFrames` countdown so a resume,
  resize, import, or menu action flushes through multi-buffered targets.

## Immediate-mode UI

`ui.h`/`ui.cpp` is pure C++ (no Metal, no AppKit). Each frame it reads one
`FrameInput`, runs the control calls, and fills a flat `UiVertex` list
(`x, y, u, v, mode, rgba`; `mode > 0.5` = textured glyph, else solid). The
Metal backend (`ui_render_metal.mm`) uploads that list and runs one
Load-action pass onto the drawable. A Windows port swaps only the backend.
`FrameRender` encodes scene → UI → present, so `RendererRender` no longer
presents or commits.

- **`FrameInput`** is the per-frame input snapshot: mouse button level bits,
  `scrollX/scrollY` (UI scroll, separate from camera deltas), `shift/ctrl/alt/
  cmd`, a fixed `KeyEvent keyEvents[16]` queue (each with captured modifier
  bits), and `bool fullscreen`. Assembled in `platform_macos.mm`; per-frame
  accumulators reset each frame, level state persists.
- **`UiWantsMouse` / `UiWantsKeyboard`** are true over any panel / menu strip /
  open dropdown or during a UI drag. `app` zeroes the camera pan/zoom/orbit
  deltas on those frames so a slider drag doesn't also move the camera.
- **Text** is a baked glyph-atlas font: `ui_render_metal` rasterizes a
  proportional atlas (`R8Unorm`) and owns `UiFontMetrics` (per-glyph uv +
  advance + offsets), handed to `ui.cpp` via `UiSetFontMetrics`. `mode > 0.5`
  multiplies fragment alpha by the sampled atlas coverage.
- **`UiSetUiScale`** (Cmd + / Cmd −) scales every panel/control/glyph without
  changing layout. `kMaxVertices` is 65536; overflow asserts.

## Dockable tear-off panels

A **panel** is a titled, dockable, tear-off container; its **title bar** is the
drag handle; buttons/sliders/labels are **controls** ("widget" is not used).

```
UiBeginPanel(ui, id, "Title", initialDock, initialSize)   // id stable + nonzero
  UiPanelButton / UiPanelSlider / UiPanelText
UiEndPanel(ui)
```

`initialDock` (`UiDock_Float|Left|Right|Top|Bottom`) / `initialSize` apply only
the first time an id is seen. Calls are unconditional: while a title bar is
dragged the UI suppresses the body internally, so the app never branches on
drag state.

- Per-panel retained state (`PanelState`, keyed by id): `dock`, `dockSize`,
  `floatRect`. `ResolvePanelLayout` carves each docked panel off its edge from
  a `free` rect (drawable minus menu strip); the leftover is the 3D viewport,
  exposed via `UiContentOriginX/Y` + `UiContentWidth/Height`. Floating panels
  draw on top and don't shrink the viewport.
- Run order per frame: resolve → interact (hit-test title bars / resize grips)
  → resolve again, so a drag starting or ending this frame reflows the same
  frame.
- Tear-off: a title-bar press pushes only a 2px outline (no chrome/content/
  layout) that follows the cursor and snaps to an edge-dock rect within
  `kDockSnapMargin` of a screen edge; release docks or floats. The dragged
  panel is skipped by layout so the viewport expands into its slot immediately.
- Not done: real OS-window tear-off; split trees / tabbed docks.

## Portable menu + command table

- **One `Command` table** (`menu.h`/`menu.cpp`): `{ CommandId id, label,
  Shortcut{key,mods}, bool(*isEnabled)(ctx), bool(*isChecked)(ctx),
  void(*invoke)(ctx) }` in a static array, with `CommandById`,
  `CommandForShortcut`, `CommandInvoke`. `CommandContext { MenuState*,
  PlatformMenuHooks, bool fullscreen }` is all a predicate/action sees;
  platform-only actions (import, fullscreen, quit) go through the hook struct
  `platform_macos.mm` fills with C trampolines.
- **Menu layout is data**: `MenuBar` of `{ title, CommandId entries[] }` with a
  separator sentinel. The native `NSMenu` and the in-app strip are both
  generated from it.
- **Single dispatch.** Native menu clicks (`NSMenuItem.tag` = `CommandId`), the
  in-app strip (`UiTakeCommand`), and the keybinding matcher in `FrameUpdate`
  (scans `FrameInput.keyEvents` → `CommandForShortcut`) all funnel through
  `CommandInvoke`. Native items carry an empty `keyEquivalent`, so the matcher
  is the only shortcut handler and chords fire exactly once.
- The in-app strip renders label / checkmark / greyed-disabled straight from
  the command predicates. Borderless fullscreen hides the macOS bar, so strip
  visibility is `fullscreen || MenuState.showMenuBar`; while fullscreen the
  Toggle Menu Bar command's `isEnabled` returns `!fullscreen`, locking it on.
  The strip's band at the top shrinks the viewport via `RendererSetContentRect`.

## Scene-editor module refactor

Structural only, no behavior change. The scene / audio / timeline modules were
grown in parallel and had diverged.

- **The undo history is its own module.** `src/undo_stack.h`/`.cpp`: a
  fixed-capacity redo/undo ring owned by no feature module, storing each
  command's payload **by value** in the ring slot (cap
  `kUndoStackPayloadBytes`) so no module keeps a parallel payload ring. A
  command is a `redo`/`undo` callback pair over an opaque `context` (the
  editor passes its `SceneState*`; a handler needing more, like the timeline's,
  puts a pointer in the payload). Replaces `SceneCommand` /
  `SceneSubmitCommand` / `SceneCmdTag_*`. `scene.cpp` owns the one instance and
  exposes `SceneUndoStack`; `timeline.cpp` submits to it directly. This is what
  lets entity edits and clip edits share one history without scene.cpp and
  timeline.cpp co-owning a ring.
- **One verb per operation class.** Undoable edits carry no `Submit` infix
  (`TimelineAddTrack`, `TimelineAddClip`, `TimelineMoveClip`, ...);
  `SceneTransformSelection` (was `SceneMakeTransformSelection`). Id lookups
  that return null/false when missing use `Find` (`SceneFindEntity`,
  `TimelineFindTrack`, ...). Entity creation stays `SceneCreateEntity*`.
- **Collection iteration is always fill-a-buffer:** `int Xs(state, Row *out,
  int maxOut)` returning the count written, like `SceneMeshRenderers`.
  Timeline's `Count()`+`At(i)` became `TimelineTracks` / `TimelineClips` with
  `TimelineTrackRow` / `TimelineClipRow`.
- **Enums are unscoped `enum Name { Name_Value }`** throughout; `GizmoHandle`
  was converted from `enum class`.
- **`WavId`** (was audio `ClipId`) names a decoded-wav handle, distinct from
  `TimelineClipId`.
- **Tool mode has no `int*` alias.** The toolbar goes through `SceneToolMode` /
  `SceneSetToolMode` on the `SceneState*` it holds; `SceneToolModePtr` and the
  mirrored `UiTool_*` enum are gone.
- Tests: `tests/scene_undo_test.cpp`, `tests/timeline_undo_test.cpp` (headless,
  standalone) cover undo/redo across the shared history.
