# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A barebones, macOS-only Metal renderer in C-style C++ / Objective-C++. It
opens a window and draws 3 cubes that spin in place. There is no engine, no
asset pipeline, no scene graph — just enough to prove the platform/game/
renderer split works.

## Build and run

```
make        # build build/ai_metal
make run    # build and launch
make clean  # remove build/
```

No Xcode project, no `xcrun metal` step — the Metal shader source is a
string embedded in `src/renderer_metal.mm` and compiled at runtime via
`newLibraryWithSource:`. `clang++` alone (via the Xcode command line tools)
is enough to build.

There is no test suite; verification is running the binary and confirming 3
distinct, independently-rotating cubes render without a crash or Metal
validation error in the console output. The `o` key cycles the ambient-
occlusion debug view (normal / raw AO buffer / AO disabled); the `f` key
toggles the FXAA post pass; `F3` toggles the frame-timing debug HUD (which
also shows per-pass GPU time); ⌃⌘F (View menu) toggles borderless
fullscreen.

## Architecture

The whole program is one memory arena (`Arena`, `src/arena.h`/`.cpp`): a flat
bump allocator over a single block that is `calloc`'d once in `main` and
never freed — the OS reclaims it on process exit. `ArenaPush`/`ArenaPushStruct`/
`ArenaPushArray` are the only ways *arena* memory is claimed, and they are
only called during `Init`. `FrameUpdate` and `FrameRender` never touch the
arena. The one thing (re)allocated after `Init` is the renderer's set of
screen-sized Metal textures (the g-buffer, depth, half-res AO, and lit-color
targets), rebuilt by `FrameResize` -> `RendererResize` when the drawable
size changes — those are Metal allocations, not arena pushes.

The public API the platform layer drives is exactly three functions
(`src/app.h`):

```
Init(arena, device, colorFormat, depthFormat, drawableW, drawableH)  // once
FrameUpdate(arena, deltaTime, cameraInput)                           // every frame
FrameRender(arena, RenderTarget)                                     // every frame
FrameResize(arena, drawableW, drawableH)                             // on drawable resize
```

`Init` pushes a small `AppState { GameState*, RendererState* }` as the very
first thing in the arena (`src/app.mm`). `FrameUpdate`/`FrameRender` recover
it by reinterpreting `arena->base` — there are no global/static pointers
holding program state.

Three layers, each with a different portability contract:

- **`src/game.h`/`.cpp`** — pure, platform-agnostic C++. No Metal, no AppKit,
  no platform headers of any kind. Owns `GameState` (currently 3 `Cube`s:
  position + rotation) and `GameUpdate`, which just advances each cube's
  rotation by `deltaTime`. This is the file to extend for anything that is
  simulation/gameplay rather than rendering.
- **`src/renderer_metal.h`/`.mm`** — Metal-specific but OS-agnostic: it never
  touches AppKit/UIKit, only the Metal API. Owns `RendererState` (device, the
  four pipelines — geometry, AO, lighting, FXAA — depth state,
  vertex/index/uniform buffers, the screen-sized targets + lit-color target,
  the AO sample kernel + noise texture, per-pass GPU timestamp sample buffer,
  orbit-camera state, the AO debug mode, the FXAA on/off flag), cube/plane
  mesh data, the embedded shader source, `RendererResize` (rebuilds the
  projection and the screen targets for a new drawable size),
  `RendererUpdateCamera` (applies a frame's `CameraInput` — trackpad/mouse
  pan/zoom/orbit deltas, plus the `o`-key debug-view cycle and `f`-key FXAA
  toggle — to the camera), and `RendererRender`, which encodes one frame from
  a `RenderTarget` (command buffer + drawable) handed in by the platform
  layer. `RendererRender` runs a small deferred pipeline: geometry pass ->
  a single g-buffer (RGBA16F: xyz = view-space normal, w = view-space Z, from
  which view-space X/Y are reconstructed), then a full-screen half-res SSAO
  pass, then a lighting pass that folds in the 4x4 AO box blur, then an
  optional FXAA pass; the last pass run writes the drawable.
  `RendererLastFrameTimings` exposes the per-pass GPU time for the F3 HUD.
  See PLAN.md for the SSAO and FXAA detail.
- **`src/platform_macos.mm`** — the only file allowed to touch AppKit. Owns
  the `NSWindow`, the `MTKView` (+ its delegate, whose `renderIntoDrawable:`
  drives `FrameUpdate` then `FrameRender` once per `CAMetalDisplayLink`
  callback), the arena allocation, and
  reading trackpad/mouse `NSEvent`s (`scrollWheel:`/`magnifyWithEvent:`/
  `rightMouseDragged:`/`otherMouseDragged:`) and the `o`/`f`/`F3` keys
  (`keyDown:`) on an `AppMetalView` subclass into the `CameraInput`
  passed to `FrameUpdate`, and forwarding `MTKView`'s
  `drawableSizeWillChange:` to `FrameResize`. It also owns `DebugHudView`,
  a pass-through `NSView` overlay that renders the `F3` frame-timing HUD
  from a `FrameStats` (`src/frame_stats.h`, header-only pure C++) fed one
  `deltaTime` sample per frame. A future second platform (e.g. iOS) would
  add a new file at this layer only; `game.*` and `renderer_metal.*` are
  unchanged. `MTKView`'s built-in draw loop is left paused
  (`paused = YES`, `enableSetNeedsDisplay = NO`) — it caps at 120 Hz on
  macOS, and so does an `NSView` `CADisplayLink`. Frames are driven instead
  by a `CAMetalDisplayLink` on the view's `CAMetalLayer`, whose
  `metalDisplayLink:needsUpdate:` delegate callback hands a fresh drawable
  to `AppViewDelegate.renderIntoDrawable:` (which does the `FrameUpdate` +
  `FrameRender` the old `drawInMTKView:` used to). Its
  `preferredFrameRateRange` is pinned to `NSScreen.maximumFramesPerSecond`
  (re-applied from `matchDisplayRefreshRate` on `windowDidChangeScreen:`),
  so a 240 Hz display renders at 240 Hz. The `MTKView` is retained only as
  a preconfigured `CAMetalLayer` host and for its
  `drawableSizeWillChange:` -> `FrameResize` hook. Fullscreen is *not*
  AppKit's Spaces fullscreen (it throttles the `CAMetalDisplayLink` back to
  120 Hz); the window disables it (`NSWindowCollectionBehaviorFullScreenNone`)
  and fullscreen is instead a borderless screen-sized window
  (`AppDelegate.toggleBorderlessFullscreen`), which keeps the uncapped
  windowed compositor path. It is reached three ways: the View menu's
  "Toggle Full Screen" (Cmd-F) and any `-toggleFullScreen:` (routed through
  `AppMetalView`'s override), the green zoom button (`windowShouldZoom:`
  returns NO after toggling), and Escape (only while already fullscreen,
  since there is no title bar to click). `AppWindow` overrides
  `canBecomeKeyWindow`/`canBecomeMainWindow` so the borderless window still
  takes keyboard input. Because the paused `MTKView` no longer syncs its
  `CAMetalLayer.drawableSize`, `AppViewDelegate.resizeToDrawableSize:` sets
  it explicitly (from `mtkView:drawableSizeWillChange:` and after the
  fullscreen toggle) so fullscreen renders at true backing resolution, not
  an upscaled stale size. The borderless window is sized to *overhang* the
  screen by 1px (`NSInsetRect(screen.frame, -1, -1)`): covering the display
  exactly triggers macOS's fullscreen bypass / direct scanout, which
  double-buffers and pins the frame rate to half the refresh (120 Hz on a
  240 Hz panel); the 1px overhang keeps the normal compositor path and is
  clipped off-screen. With the trimmed pipeline, fullscreen at a 5K backing
  runs ~2 ms GPU / 240 fps on an M2 Pro.

`src/math3d.h` is header-only, pure-C++ `Vec3`/`Mat4` math (column-major,
matching Metal Shading Language's `float4x4` layout byte-for-byte so CPU
matrices can be `memcpy`'d straight into a uniform buffer). It's shared by
both the game layer and the renderer, and has no platform dependencies, so
it's safe to include from either.

## The ARC-in-arena gotcha

`RendererState` holds ARC-managed `id<MTL...>` fields, but it is placed in
raw arena memory rather than allocated the normal ARC way. The first write
to any such field compiles to `objc_storeStrong`, which retains the new
value and releases whatever was previously there — if that memory is
uninitialized garbage instead of `nil`, this crashes. That's why the arena's
backing memory is obtained with `calloc`, not `malloc`
(`src/platform_macos.mm`): zeroing guarantees every `id` field starts as
`nil` before its first assignment. If you ever add a second Metal-object-
holding struct pushed into the arena, this same requirement applies to it.

The SSAO screen targets are assigned more than once (every drawable
resize). That is safe for the same reason: each field holds either `nil` or
a valid texture when `objc_storeStrong` releases it, never garbage.
