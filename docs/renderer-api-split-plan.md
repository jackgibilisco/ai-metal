# Renderer API split (prep for a Windows port)

## Problem

The platform -> app contract (`src/app.h`) is Metal-typed: `Init` takes
`id<MTLDevice>` + `MTLPixelFormat`s, `FrameRender` takes a `RenderTarget`
holding `id<MTLCommandBuffer>` / `id<CAMetalDrawable>`. `renderer_metal.h`
and `ui_render_metal.h` expose Metal in their init signatures too. A second
platform layer (`platform_windows.cpp`) cannot call this contract, and a
second renderer backend cannot implement it, without editing the same
headers the engine programmer lives in. Every parallel workstream collides.

## Goal

`app.h` and a new neutral `renderer.h` / `ui_render.h` name no graphics-API
type. The Metal backend keeps working unchanged in behaviour. After this,
a Windows dev adds `renderer_d3d12.cpp` + `platform_windows.cpp` + a build
file with no edits to `game.*`, `ui.*`, `app.cpp`, or the neutral headers.

## Design

- `src/gpu.h` (new, neutral): forward-declares `struct GpuContext;` and
  `struct RenderTarget;` only. Included by `app.h`, `renderer.h`,
  `ui_render.h`. The app and UI cores pass these as opaque pointers.
- `src/gpu_metal.h` (new, Metal): defines both structs with Metal types.
  `GpuContext { device; colorFormat; depthFormat; }`,
  `RenderTarget { commandBuffer; drawable; }`. Included only by the three
  Metal backend files and `platform_macos.mm`.
- `src/renderer.h` (new, replaces `renderer_metal.h`): neutral renderer
  contract. `RendererInit(Arena*, GpuContext*, float w, float h)`;
  `RendererRender(RendererState*, const GameState*, RenderTarget*)`.
  `RendererPassTimings` moves here (already pure).
- `src/ui_render.h` (new, replaces `ui_render_metal.h`): neutral.
  `UiRenderInit(Arena*, GpuContext*)`;
  `UiRenderEncode(UiRenderState*, RenderTarget*, ...)`.
- `src/app.h`: drop `#import <Metal/Metal.h>`; include `gpu.h` + `renderer.h`.
  `Init(Arena*, GpuContext*, float w, float h, PlatformMenuHooks)`;
  `FrameRender(Arena*, RenderTarget*)`.
- `src/app.mm` -> `src/app.cpp`: now free of ObjC/Metal. The
  `presentDrawable:` + `commit` that ended `FrameRender` moves to
  `platform_macos.mm` (it owns the command queue and drawable).
- `renderer_metal.mm` / `ui_render_metal.mm`: unpack `device` / formats from
  `GpuContext` at the top of their init funcs; copy `*target` to a local at
  the top of the render funcs. Bodies otherwise untouched.
- `platform_macos.mm`: build a stack `GpuContext`, pass `&gpu` to `Init`;
  `FrameRender(arena, &target)` then present + commit.
- `Makefile`: move `src/app.cpp` to `SRC_CPP`; drop the two deleted headers'
  `.mm` names stay in `SRC_MM` unchanged.

## Verify

`make` clean, then `make run`: 3 spinning cubes, both panels, `o` / `f` /
`p` / `F3` all still work, no Metal validation errors.
