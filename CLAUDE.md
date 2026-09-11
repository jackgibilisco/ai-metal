# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A macOS-only Metal renderer in C-style C++ / Objective-C++, being grown into
a 3D scene editor for spatial audio. It opens a window on a default test
scene (ground plane, two occluder cubes, two audio sources, one active
listener) with a deferred renderer, an orbit camera, a tool toolbar, and the
scene / audio / timeline modules wired in. See `docs/scene-editor-plan.md`
for what has landed and what is still pending (timeline panel UI, gizmo
viewport interaction + draw, wav drag-drop).

## Build and run

```
make        # build build/Renderer
make run    # build and launch
make clean  # remove build/
```

No Xcode project, no `xcrun metal` step — the Metal shader source is a
string embedded in `src/renderer_metal.mm` and compiled at runtime via
`newLibraryWithSource:`. `clang++` alone (via the Xcode command line tools)
is enough to build.

Verification is running the binary and confirming the default scene (ground
plane + two occluder cubes) renders without a crash or Metal validation
error in the console output, alongside two dockable UI panels ("Controls"
right, "Scene" left) with title bars that shrink the 3D viewport; drag a
title bar to tear a panel off and re-dock or float it. A tool toolbar
overlays the top-left of the viewport (Select / Translate / Rotate / Scale /
Add Source / Add Listener / Snap). Debug toggles are F3 chords (hold F3,
press the key), all listed in the Debug menu: F3+O cycles the
ambient-occlusion debug view (normal / raw AO buffer / AO disabled), F3+F
toggles the FXAA post pass, F3+B toggles the layout-bounds overlay (magenta
outlines on every UI container, for auditing inset symmetry), and tapping F3
alone toggles the frame-timing HUD (which also shows per-pass GPU time).
Window > Reset Panel Layout restores every panel's startup dock and size. The
spacebar toggles the timeline transport (the editor's global sim clock — while
paused, sim/audio time holds but the camera still orbits); pinch over the
timeline zooms it. ⌃⌘F (View menu) toggles borderless fullscreen. `tests/` holds
headless unit tests (not in the Makefile; build them standalone). In fullscreen an in-app menu strip appears at the top
(mirroring the hidden macOS menu bar); View > Toggle Menu Bar also shows it
while windowed. The frame loop pauses entirely when the app is inactive or
its window is minimized/occluded.

## Architecture (summary)

One `calloc`'d memory arena (`src/arena.h`/`.cpp`), bump-allocated only
during `Init`, never freed. `Init` pushes `AppState { GameState*,
RendererState*, UiState*, UiRenderState*, ... }` first; `FrameUpdate` /
`FrameRender` recover it from `arena->base` — no global state pointers. The
platform layer calls `Init` once, then `FrameUpdate(deltaTime, FrameInput)
-> needsRender` every tick and `FrameRender(RenderTarget*)` only when that is
true (`src/app.h`).

`src/app.h` and `src/app.cpp` name no graphics-API type: `GpuContext` and
`RenderTarget` are forward-declared in `src/gpu.h` and passed as opaque
pointers. The Metal backend defines them in `src/gpu_metal.h`. A Windows port
adds `platform_windows.cpp` + `renderer_d3d12.cpp` + `ui_render_d3d12.cpp`
against `app.h` / `renderer.h` / `ui_render.h` with no edits to the cores.

Layers by portability contract:

| file | contract | owns |
|------|----------|------|
| `src/game.h`/`.cpp` | pure C++, no platform headers | `GameState` (3 cubes); extend here for simulation/gameplay |
| `src/ui.h`/`.cpp` | pure C++, no Metal/AppKit | immediate-mode dockable panels, fills a flat `UiVertex` list; view state only |
| `src/menu.h`/`.cpp` | pure C++ | one `Command` table + `MenuBar` layout; NSMenu, in-app strip, keybindings all resolve through it |
| `src/app.h`/`.cpp`, `src/gpu.h` | pure C++, no graphics API | wires game/renderer/ui/menu together; opaque `GpuContext`/`RenderTarget` |
| `src/renderer.h`, `src/ui_render.h` | pure C++ contract | backend-agnostic renderer + UI-renderer signatures |
| `src/renderer_metal.*` + `src/gpu_metal.h` | Metal, no AppKit | `RendererState`, 4 pipelines (geometry/AO/lighting/FXAA), deferred pass chain, orbit camera; does not present/commit |
| `src/ui_render_metal.mm` | Metal only | UI pipeline, glyph atlas, per-frame vertex buffer |
| `src/platform_macos.mm` | the only AppKit file | `NSWindow`, `MTKView`+`CAMetalDisplayLink`, arena alloc, NSEvent -> `FrameInput`, native menu, per-frame present + commit |
| `src/math3d.h` | header-only pure C++ | column-major `Vec3`/`Mat4`, layout matches MSL `float4x4` |
| `src/undo_stack.h`/`.cpp` | pure C++, owned by no module | fixed-capacity undo/redo ring; stores each command's payload by value; scene and timeline both submit to one shared instance |
| `src/frame_input.h`, `frame_stats.h` | dependency-free headers | portable input/timing structs |

Full detail (pass chain, frame-loop pausing, menu routing, fullscreen path)
is in `docs/architecture.md`. SSAO/FXAA math is in `PLAN.md`.

## Gotchas

- **ARC-in-arena**: `RendererState` / `UiRenderState` hold ARC `id<MTL...>`
  fields but live in raw arena memory. First assignment compiles to
  `objc_storeStrong`, which releases the previous value — garbage instead of
  `nil` crashes. The arena is `calloc`'d (not `malloc`'d) so every `id`
  starts `nil`. Any new Metal-object-holding arena struct must rely on the
  same zeroing. Reassigning the SSAO targets on resize is safe: the field is
  always `nil` or a valid texture, never garbage.
- **Fullscreen 1px overhang**: borderless fullscreen sizes the window to
  `NSInsetRect(screen.frame, -1, -1)`. Covering the display *exactly*
  triggers macOS direct-scanout, which halves the frame rate (120 Hz on a
  240 Hz panel). The 1px overhang keeps the normal compositor path. Do not
  "fix" it to exact bounds. It is also deliberately not AppKit Spaces
  fullscreen (that throttles the `CAMetalDisplayLink`).

## Working here

- `renderer_metal.mm` (880 lines) and `platform_macos.mm` (792) are large:
  grep for the symbol, then Read a window with `offset`/`limit`. Do not Read
  either whole.
- `PLAN.md` is 45 KB — Read the section you need, not the file.
