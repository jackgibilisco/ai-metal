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
validation error in the console output.

## Architecture

The whole program is one memory arena (`Arena`, `src/arena.h`/`.cpp`): a flat
bump allocator over a single block that is `calloc`'d once in `main` and
never freed — the OS reclaims it on process exit. `ArenaPush`/`ArenaPushStruct`/
`ArenaPushArray` are the only ways memory is claimed, and they are only
called during `Init`. `FrameUpdate` and `FrameRender` never allocate.

The public API the platform layer drives is exactly three functions
(`src/app.h`):

```
Init(arena, device, colorFormat, depthFormat, aspectRatio)   // once
FrameUpdate(arena, deltaTime, cameraInput)                   // every frame
FrameRender(arena, RenderTarget)                             // every frame
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
  touches AppKit/UIKit, only the Metal API. Owns `RendererState` (device,
  pipeline, depth state, vertex/index/uniform buffers, orbit-camera state),
  cube mesh data, the embedded shader source, `RendererUpdateCamera` (applies
  a frame's `CameraInput` — trackpad pan/pinch-zoom/shift-orbit deltas — to
  the camera), and `RendererRender`, which encodes one frame from a
  `RenderTarget` (command buffer + pass descriptor + drawable) handed in by
  the platform layer.
- **`src/platform_macos.mm`** — the only file allowed to touch AppKit. Owns
  the `NSWindow`, the `MTKView` (+ its delegate, which drives `FrameUpdate`
  then `FrameRender` on every `drawInMTKView:`), the arena allocation, and
  reading trackpad `NSEvent`s (`scrollWheel:`/`magnifyWithEvent:` on an
  `AppMetalView` subclass) into the `CameraInput` passed to `FrameUpdate`. A
  future second platform (e.g. iOS) would add a new file at this layer only;
  `game.*` and `renderer_metal.*` are unchanged.

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
