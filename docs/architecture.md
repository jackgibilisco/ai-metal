# Architecture (detail)

Compact map is in `CLAUDE.md`. This file is the deep version — read it only
when a task needs internals it does not cover.

## Memory

The whole program is one memory arena (`Arena`, `src/arena.h`/`.cpp`): a flat
bump allocator over a single block that is `calloc`'d once in `main` and
never freed — the OS reclaims it on process exit. `ArenaPush`/`ArenaPushStruct`/
`ArenaPushArray` are the only ways *arena* memory is claimed, and they are
only called during `Init`. `FrameUpdate` and `FrameRender` never touch the
arena. The one thing (re)allocated after `Init` is the renderer's set of
screen-sized Metal textures (the g-buffer, depth, half-res AO, and lit-color
targets), rebuilt by `RendererSetContentRect` when the content region (the
drawable minus the docked UI panels and menu strip) changes — those are
Metal allocations, not arena pushes.

## Public API the platform layer drives (`src/app.h`)

```
Init(arena, GpuContext*, drawableW, drawableH, menuHooks)                      // once
FrameUpdate(arena, deltaTime, FrameInput) -> bool needsRender                  // every tick
FrameRender(arena, RenderTarget*)                                             // only when needsRender
FrameResize(arena, drawableW, drawableH)                                      // on drawable resize
AppRequestRender(arena)                                                       // force the next few renders
AppInvokeCommand(arena, CommandId) / AppCommandContext(arena)                  // native menu bar
```

`GpuContext` (device + swapchain formats) and `RenderTarget` (command buffer
+ drawable) are forward-declared in `src/gpu.h` and opaque to `app.cpp`; the
Metal backend defines them in `src/gpu_metal.h`. `platform_macos.mm` builds
a `GpuContext` at startup and a `RenderTarget` each frame, and does the
`presentDrawable:` + `commit` after `FrameRender` returns.

`FrameUpdate` returns whether anything the renderer would draw differently
changed this frame (scene animated, camera moved, a render toggle fired, the
UI is being interacted with, or `AppRequestRender` was called); when it
returns false the platform layer skips `FrameRender` and leaves the last
presented drawable on screen. The `CAMetalDisplayLink` is paused outright
when the window can't be seen (see the platform layer).

`Init` pushes a small `AppState { SceneState*, AudioState*, TimelineState*,
RendererState*, UiState*, UiRenderState*, UiDemoState, MenuState,
PlatformMenuHooks, bool snapEnabled, int forcedRenderFrames }` as the very
first thing in the arena (`src/app.cpp`). The per-frame order is
`TimelineUpdate` → gate the sim delta on `TimelineIsPlaying` → `SceneUpdate`
→ `AudioUpdate`. `FrameUpdate`/`FrameRender` recover it by
reinterpreting `arena->base` — there are no global/static pointers holding
program state. `FrameUpdate` builds the immediate-mode UI, then routes any
in-app menu click and any matched keyboard shortcut through the same
`CommandInvoke` the native menu bar uses; `FrameRender` encodes the scene
into the content region, then the UI overlay pass. The platform layer
presents and commits.

The portable input/menu structs live in their own dependency-free headers
(`src/frame_input.h`, `src/menu.h`) so the pure-C++ layers pull in no Metal.

## Layers, each with a different portability contract

- **`src/scene.h`/`.cpp`** — pure, platform-agnostic C++. The editor's scene
  representation: fixed-capacity entity + component pools in the arena
  (meshes, audio sources, listeners), a shared entity/clip selection set,
  ray/AABB picking, and a unified command/undo stack (`SceneCommand`). The
  renderer and audio read it through iteration helpers (`SceneMeshRenderers`,
  `SceneAudioSources`, `SceneActiveListener`); the gizmo and timeline push
  edits through `SceneSubmitCommand`.
- **`src/game.h`/`.cpp`** — pure C++, now just `GameLoadDefaultScene(SceneState*)`,
  which direct-inserts the default spatial-audio test scene (undo stack stays
  empty).
- **`src/audio.h`/`.cpp`** — pure C++ + miniaudio (vendored). Decodes wavs
  into a 64 MiB PCM pool, runs a parametric-HRTF + distance + occlusion mixer
  on the device thread. `AudioUpdate(audio, scene, timeline, transportTime)`
  once per frame.
- **`src/timeline.h`/`.cpp`** — pure C++. The transport clock (the editor's
  global sim clock) plus tracks/clips bound to scene audio sources; every
  structural edit submits one `SceneCommand`.
- **`src/gizmo.h`/`.cpp`** — pure C++ transform-gizmo hit-test + drag math
  (`GizmoHitTest` → `TransformDelta`), mesh/icon builders, and the
  `ToolAddAudioSource` / `ToolAddListener` placement helpers.
- **`src/ui.h`/`.cpp`** — pure C++ immediate-mode UI, no Metal/AppKit. Each
  frame `UiBuildFrame` reads one `FrameInput` plus an app-owned
  `UiDemoState*` (the values the demo sliders edit), builds the panels
  (`UiBeginPanel`/`UiPanelButton`/`UiPanelSlider`/`UiPanelText`/`UiEndPanel`)
  plus a top menu strip from `MenuBarDefault()`, and fills a flat
  `UiVertex` triangle list (`x, y, u, v, mode, rgba` — `mode` 0 solid, 1
  glyph) in screen-pixel coordinates. A **panel** is a titled, dockable,
  tear-off container (buttons/sliders/labels are **controls**); each has a
  stable id, a dock edge or a float rect, and a size, kept in `UiState`.
  `ResolvePanelLayout` carves docked panels off the drawable (minus the menu
  strip) and the leftover is the 3D viewport (`UiContentOriginX/Y`,
  `UiContentWidth/Height`). `UpdatePanelInteraction` handles title-bar
  tear-off (while a panel is dragged only a 2px outline is drawn — its
  controls are internally suppressed — and it snaps to a screen edge to
  re-dock) and dock-edge resize grips. `UiState` is view state only: panel
  placement/size, drag state, hot/active control id, open menu, previous
  mouse — it renders app/game state passed in and returns intents, it does
  not own domain data. Reports any clicked `CommandId` (`UiTakeCommand`) and
  whether it owns the pointer/keyboard this frame (`UiWantsMouse` /
  `UiWantsKeyboard`, true over any panel or during a drag).
  `PushVertex` asserts on overflow of the 65536-vertex buffer. Text is one
  textured quad per character (`mode` 1, uv into the glyph atlas); the
  monospace 8x8 cell means `TextWidth` is just `strlen * 8 * scale`, no
  measurement pass. The atlas is built once in `ui_render_metal.mm` from the
  vendored public-domain `src/third_party/font8x8_basic.h` (ASCII 32..126);
  `ui.cpp` mirrors `kFontFirstChar`/`kFontCharCount` to place the cells.
- **`src/menu.h`/`.cpp`** — pure C++ command table: a static array of
  `Command { id, label, Shortcut, isEnabled(ctx), isChecked(ctx), invoke(ctx) }`
  (`CommandTable` / `CommandById` / `CommandForShortcut` / `CommandInvoke`),
  plus a `MenuBar` layout of command-id lists per title (`MenuBarDefault`,
  `kMenuSeparator`). `invoke` mutates `MenuState` for app-level commands
  (Toggle Menu Bar) or calls a `PlatformMenuHooks` function pointer for the
  platform-only ones (import file, toggle fullscreen, quit); everything a
  predicate needs is in `CommandContext { MenuState*, PlatformMenuHooks,
  fullscreen }`. The native `NSMenu`, the in-app strip, and the keybinding
  matcher all resolve through this one table, so shortcuts work with no
  native menu (a Windows port).
- **`src/ui_render.h` + `src/ui_render_metal.mm`** — `ui_render.h` is the
  graphics-API-free contract (`UiRenderInit(arena, GpuContext*)`,
  `UiRenderEncode(state, RenderTarget*, ...)`); `ui_render_metal.mm` is the
  Metal backend for the UI: one pipeline
  (fragment branches on `mode` — solid color, or color with alpha from the
  R8 glyph atlas), the glyph atlas texture, a per-frame vertex buffer, and
  one alpha-blended Load-action pass drawn on top of the drawable after the
  scene. A Windows port adds a sibling `ui_render_d3d12.cpp` and reuses
  `ui.cpp` and `ui_render.h` unchanged.
- **`src/renderer.h` + `src/renderer_metal.mm` + `src/gpu_metal.h`** —
  `renderer.h` is the graphics-API-free contract (`RendererInit(arena,
  GpuContext*, w, h)`, `RendererRender(state, game, RenderTarget*)`, and the
  pure `RendererPassTimings` struct); `gpu_metal.h` gives `GpuContext` /
  `RenderTarget` their Metal fields. `renderer_metal.mm` is Metal-specific but
  OS-agnostic: it never touches AppKit/UIKit, only the Metal API. Owns
  `RendererState` (device, the
  four pipelines — geometry, AO, lighting, FXAA — depth state,
  vertex/index/uniform buffers, the screen-sized targets + lit-color target,
  the AO sample kernel + noise texture, per-pass GPU timestamp sample buffer,
  orbit-camera state, the AO debug mode, the FXAA on/off flag), cube/plane
  mesh data, the embedded shader source, `RendererSetContentRect` (rebuilds
  the projection and the screen targets for a new content-region size, and
  stores the origin the final pass writes the drawable at),
  `RendererUpdateCamera` (applies a frame's `FrameInput` — trackpad/mouse
  pan/zoom/orbit deltas, plus the `o`-key debug-view cycle and `f`-key FXAA
  toggle — to the camera), and `RendererRender`, which encodes one frame from
  a `RenderTarget` (command buffer + drawable) handed in by the platform
  layer. The screen targets are content-region-sized; the last pass takes an
  `(originX, originY, w, h)` `MTLViewport` so the scene lands below the menu
  strip and left of the panel, and the UI pass fills the rest.
  `RendererRender` no longer presents or commits — `FrameRender` does, after
  the UI pass. It runs a small deferred pipeline: geometry pass ->
  a single g-buffer (RGBA16F: xyz = view-space normal, w = view-space Z, from
  which view-space X/Y are reconstructed), then a full-screen half-res SSAO
  pass, then a lighting pass that folds in the 4x4 AO box blur, then an
  optional FXAA pass; the last pass run writes the drawable.
  `RendererLastFrameTimings` exposes the per-pass GPU time for the F3 HUD.
  See PLAN.md for the SSAO and FXAA detail.
- **`src/platform_macos.mm`** — the only file allowed to touch AppKit. Owns
  the `NSWindow`, the `MTKView` (+ its delegate, whose `renderIntoDrawable:`
  drives `FrameUpdate` every `CAMetalDisplayLink` callback and `FrameRender`
  only when `FrameUpdate` reports a change), the arena allocation, and
  reading trackpad/mouse `NSEvent`s (`scrollWheel:`/`magnifyWithEvent:`/
  `rightMouseDragged:`/`otherMouseDragged:` for the camera; all three mouse
  buttons' down/up, `mouseDragged:`/`mouseMoved:` + a tracking area,
  `flagsChanged:` for modifiers, and a `keyDown:`/`keyUp:` `KeyEvent` queue
  for the UI cursor) plus the `o`/`f`/`p`/`F3` one-shot keys, all assembled
  in `renderIntoDrawable:` into the per-frame `FrameInput` snapshot (which
  also carries `fullscreen`) passed to `FrameUpdate`, and forwarding
  `MTKView`'s `drawableSizeWillChange:` to `FrameResize`. `app.cpp` zeroes
  the camera deltas on frames where `UiWantsMouse` is true so panel drags
  don't move the camera. `AppDelegate.updateFrameLoopRunning` pauses the
  `CAMetalDisplayLink` outright when the render can't be seen (`!NSApp.active`,
  window minimized, or not `NSWindowOcclusionStateVisible`) and resumes on
  the matching `windowDidChangeOcclusionState:` /
  `applicationDid{Become,Resign}Active:` / `windowDid{Miniaturize,Deminiaturize}:`
  notifications, re-priming `AppViewDelegate.lastTime` and calling
  `AppRequestRender`. `InstallMainMenu` builds the `NSMenu` bar from the
  `MenuBarDefault()` layout + command table; every item carries its
  `CommandId` in its `tag`, has no `keyEquivalent` (the app's own keybinding
  matcher in `FrameUpdate` is the sole shortcut handler, so shortcuts also
  work with no `NSMenu`), and routes through one `-dispatchMenuAction:` ->
  `AppInvokeCommand`; `-validateMenuItem:` reads the command's `isChecked` /
  `isEnabled` via `AppCommandContext`. `keyDown:`/`keyUp:` record each
  `KeyEvent` with its modifier bits captured at event time (a synthetic
  Cmd-up can beat the frame, so sampling the modifier level-state at frame
  assembly is unreliable). The platform-only actions are `PlatformMenuHooks`
  C trampolines handed to `Init`. It also owns `DebugHudView`,
  a pass-through `NSView` overlay that renders the `F3` frame-timing HUD
  from a `FrameStats` (`src/frame_stats.h`, header-only pure C++) fed one
  `deltaTime` sample per frame. A future second platform (e.g. iOS or
  Windows) would add a new file at this layer plus a `ui_render_*` backend,
  and fill in `PlatformMenuHooks`; `game.*`, `ui.*`, `menu.*`, and
  `renderer_metal.mm` is unchanged. `MTKView`'s built-in draw loop is left paused
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
`nil` before its first assignment. `UiRenderState` (pipeline + vertex
buffer + glyph atlas texture) is a second such struct pushed into the arena
and relies on the same zeroing; any further Metal-object-holding arena
struct must too.

The SSAO screen targets are assigned more than once (every drawable
resize). That is safe for the same reason: each field holds either `nil` or
a valid texture when `objc_storeStrong` releases it, never garbage.
