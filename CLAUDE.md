# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Metal renderer for macOS (plus an OpenGL 4.1 build at pixel parity, which is
also what the Windows build runs) in C-style C++ / Objective-C++, being grown into
a 3D scene editor for spatial audio. It opens a window on a default test
scene (ground plane, two occluder cubes, two audio sources, one active
listener) with a deferred renderer, an orbit camera, a tool toolbar, and the
scene / audio / timeline modules wired in. See `docs/scene-editor-plan.md`
for what has landed and what is still pending (timeline panel UI, gizmo
viewport interaction + draw, wav drag-drop).

## Build and run

macOS:

```
make              # build build/Renderer (Metal)
make run          # build and launch
make opengl       # build build/Renderer_gl (same app on OpenGL 4.1)
make run-opengl   # build and launch the OpenGL build
make parity-test  # render scripted cases through both backends, diff the images
make clean        # remove build/
```

Windows (the OpenGL backend on a Win32 platform layer):

```
build.bat         # build build\Renderer_gl.exe
build.bat run     # build and launch
build.bat test    # unit tests, then parity against tests\golden
build.bat clean   # remove build\
```

No Xcode project, no CMake, no `xcrun metal` step — the Metal shader source is a
string embedded in `src/renderer_metal.mm` and compiled at runtime via
`newLibraryWithSource:` (the GLSL likewise lives in `src/renderer_gl.cpp`).
`clang++` alone (via the Xcode command line tools) is enough on macOS;
`build.bat` finds `vcvars64.bat` through `vswhere` and runs one `cl.exe` per
target on Windows. Every third-party dependency is vendored in
`src/third_party/` (miniaudio, stb_truetype, the Argentum Sans TTF, Khronos'
`glcorearb.h`, and zstd's single-file decompressor), so neither platform needs
a package manager.

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
headless unit tests and the backend parity harness: `make parity-test` must
pass after any renderer or UI change (Metal is the reference; GL must match
within 1/255). Windows cannot run Metal, so `build.bat test` diffs the same
scripted cases against the Metal images committed in `tests/golden/`. Those are
rendered with the AO pass disabled (`--ao-off`, which also drops the two
AO-view cases), because SSAO is the one pass two GPU vendors do not agree on;
`--cross-gpu` then allows any per-pixel delta but only 1 pixel in 5000, which
covers triangle-edge tie-breaking without hiding a real regression. Re-bless
from a Mac with `make bless-goldens` and commit the result whenever a change is
meant to move pixels. In fullscreen an in-app menu strip appears at the top
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
pointers. The Metal backend defines them in `src/gpu_metal.h`, the OpenGL
backend in `src/gpu_gl.h`. The Windows build reuses the OpenGL backend
unchanged and adds only a platform layer; a D3D12 backend would add
`renderer_d3d12.cpp` + `ui_render_d3d12.cpp` against `renderer.h` /
`ui_render.h` with no edits to the cores.

`FrameInput.keyCode` is a portable `Key_*` value from `frame_input.h`, not a
native virtual key code. Each platform layer translates its own codes
(`NormalizedKeyCode` in `platform_macos.mm` and `platform_windows.cpp`);
`app.cpp` and `ui.cpp` only ever see the enum.

Layers by portability contract:

| file | contract | owns |
|------|----------|------|
| `src/game.h`/`.cpp` | pure C++, no platform headers | `GameState` (3 cubes); extend here for simulation/gameplay |
| `src/ui.h`/`.cpp` | pure C++, no Metal/AppKit | immediate-mode dockable panels, fills a flat `UiVertex` list; view state only |
| `src/menu.h`/`.cpp` | pure C++ | one `Command` table + `MenuBar` layout; NSMenu, in-app strip, keybindings all resolve through it |
| `src/app.h`/`.cpp`, `src/gpu.h` | pure C++, no graphics API | wires game/renderer/ui/menu together; opaque `GpuContext`/`RenderTarget` |
| `src/renderer.h`, `src/ui_render.h` | pure C++ contract | backend-agnostic renderer + UI-renderer signatures |
| `src/renderer_common.h`/`.cpp`, `src/font_atlas.*` | pure C++ | what both backends share: mesh data, orbit camera, AO samples, overlay geometry, SDF glyph atlas bake (stb_truetype, so both platforms bake byte-identical atlases) |
| `src/renderer_metal.*` + `src/gpu_metal.h` | Metal, no AppKit | `RendererState`, 4 pipelines (geometry/AO/lighting/FXAA), deferred pass chain; does not present/commit |
| `src/ui_render_metal.mm` | Metal only | UI pipeline, glyph atlas, per-frame vertex buffer |
| `src/renderer_gl.cpp`, `src/ui_render_gl.cpp`, `src/gpu_gl.h`, `src/gl_shader.*` | OpenGL 4.1 core, no AppKit/Win32 | GL backend, same passes as Metal; no GPU timings (Apple GL timer queries read 0) |
| `src/gl_loader.h`/`.cpp`, `src/third_party/glcorearb.h` | Windows only | one X-macro list of the 58 GL entry points, resolved through `wglGetProcAddress` with a fallback to `opengl32.dll` |
| `src/platform_macos.mm` | AppKit, no graphics API | `NSWindow`, content view, arena alloc, NSEvent -> `FrameInput`, native menu, HUD, fullscreen |
| `src/platform_macos_present.h`, `src/platform_macos_{metal,gl}.mm` | AppKit + one graphics API | presenter: backing layer / GL context, display link, per-frame begin + present |
| `src/platform_windows.cpp` | Win32, no graphics API | window, arena alloc, messages -> `FrameInput`, `HMENU` from the command table, `IDropTarget`, `IFileOpenDialog`, borderless fullscreen, frame loop |
| `src/platform_windows_present.h`, `src/platform_windows_gl.cpp` | Win32 + WGL | presenter: WGL 4.1 core context, swap interval 1, `DwmFlush` for idle frames |
| `src/platform_windows_hud.h`/`.cpp` | Win32 GDI | the `DebugHudView` counterpart: a click-through layered overlay window |
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
  fullscreen (that throttles the display link).
- **No `rand()` in the renderer**: `BuildAoSamples` runs its own LCG. The C
  library's sequence differs between platforms, which silently put macOS and
  Windows on different AO kernels and broke image parity by several percent.
- **COM after audio (Windows)**: `platform_windows.cpp` calls `OleInitialize`
  only after `Init` returns. Joining an apartment before `AudioInit` makes
  miniaudio's WASAPI backend fault during device creation. Drag-and-drop and
  the file dialog are the only things that need COM, and both come later.
- **Windows fullscreen covers the monitor exactly** — deliberately *not* the
  macOS 1px overhang below. That overhang dodges a WindowServer
  direct-scanout path; DWM has no equivalent penalty, so copying it would just
  clip a pixel off every edge.
- **Ctrl is Cmd on Windows**: `ModifierBits` sets `ShortcutMod_Cmd` *and*
  `ShortcutMod_Ctrl` from the Ctrl key, so every `ShortcutMod_Cmd` binding in
  `menu.cpp` and `app.cpp` fires without a second table.
- **GL Y flip**: `renderer_gl.cpp` draws into its own textures with
  clip-space y negated (`flipY = -1`) so they share Metal's top-row-first
  layout and every shader samples with Metal's uv; only the final composite
  into the `RenderTarget` is upright. `make parity-test` catches a wrong flip.

## Working here

- `renderer_metal.mm`, `renderer_gl.cpp`, and `platform_macos.mm` are large:
  grep for the symbol, then Read a window with `offset`/`limit`. Do not Read
  either whole.
- `PLAN.md` is 45 KB — Read the section you need, not the file.
