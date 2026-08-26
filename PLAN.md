# Plan: barebones Metal cube renderer

## Problem
A minimal C-style C++ / Objective-C++ program that opens a macOS window and
draws 3 rotating cubes with Metal. Platform layer calls exactly three
functions: `Init`, `FrameUpdate`, `FrameRender`, all operating on a single
memory arena allocated once at startup and never freed (OS reclaims it on
exit). `FrameUpdate` must be pure, platform-agnostic C++. `FrameRender` must
not touch AppKit/windowing, only the Metal API.

## Design
- `Arena`: flat bump allocator over one `calloc`'d block. No frees, no
  allocation after `Init` finishes.
- `game.h/.cpp` (pure C++, no platform/Metal includes): `GameState` (3 cubes,
  position + rotation), `GameInit`, `GameUpdate` (spins each cube).
- `math3d.h` (pure C++): `Vec3`/`Mat4` and the handful of matrix ops needed
  (identity, multiply, translate, rotateX/Y, perspective, lookAt).
- `renderer_metal.h/.mm`: `RendererState` (Metal objects), `RendererInit`
  (builds cube mesh, pipeline, depth state, uniform buffer, view-projection),
  `RendererRender` (encodes + presents one frame). Only Metal API calls, no
  AppKit.
- `app.h/.mm`: the public 3-function API (`Init`/`FrameUpdate`/`FrameRender`)
  the platform layer calls. Places a small `AppState{GameState*, RendererState*}`
  as the very first thing in the arena so every call can recover it from
  `arena->base` — no hidden globals.
- `platform_macos.mm`: the only AppKit-aware file. Creates the window, an
  `MTKView`, allocates+zeroes the arena, calls `Init` once, then drives
  `FrameUpdate`/`FrameRender` from the view's per-frame callback.
- Shader source is embedded as a string and compiled at runtime
  (`newLibraryWithSource:`) — avoids a separate `xcrun metal` build step.

## Gotcha worth flagging
`RendererState` holds ARC-managed `id<MTL...>` fields but lives in raw
arena memory, not a normal ARC-tracked allocation. The arena must be
zero-initialized (`calloc`, not `malloc`) so the first field assignment
doesn't try to `objc_release` garbage bytes.

## Steps
1. `arena.h/.cpp`
2. `math3d.h`
3. `game.h/.cpp`
4. `renderer_metal.h/.mm` (cube mesh, pipeline, draw)
5. `app.h/.mm`
6. `platform_macos.mm`
7. `Makefile`
8. Build, run, visually confirm 3 spinning cubes
9. `CLAUDE.md`
