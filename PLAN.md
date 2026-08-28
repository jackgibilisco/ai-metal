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

## Feature: File > Import File... (.blend cubes/planes)

### Problem
Load a Blender `.blend` file containing only cube and plane mesh objects and
display that scene, via a File menu item.

### Scope decision
Transform-only import: read each Object's position/rotation/scale and
classify its linked Mesh as a cube (8 verts) or plane (4 verts), then render
with the engine's existing built-in unit-cube mesh plus a new built-in plane
mesh — not a generic arbitrary-geometry importer. Real per-vertex mesh data
in the file is never read.

### .blend format (Blender 5.x, verified against blender/blender source,
not memorized/guessed)
- Whole file is a single zstd stream; decompress up front (streaming API,
  doesn't rely on embedded content size).
- 17-byte header: `BLENDER` + `17` (header size) + `-` + `01` (format
  version) + `v` + 4-digit Blender version. Only this new-format header is
  supported (pre-5.0 12-byte-header files are out of scope).
- Format version 1 always uses `LargeBHead8` block headers (32 bytes:
  `int32 code, int32 sdnaIndex, uint64 oldPointer, int64 length, int64 count`),
  followed immediately by `length` bytes of block data, until code `ENDB`.
- ID blocks (Object, Mesh, ...) have `code` <= 0xFFFF: the low 16 bits are
  the 2-letter ID code ("OB", "ME").
- The `DNA1` block holds the classic SDNA struct-layout table (magic
  `SDNA`/`NAME`/`TYPE`/`TLEN`/`STRC` chunks, 4-byte-padded string tables) —
  unchanged from historical Blender versions. `blend_file.cpp` parses it and
  uses it to compute each named struct member's byte offset (member sizes
  sum in declaration order; DNA structs are defined never to need internal
  padding) so field reads are correct regardless of exact struct layout for
  the file's Blender version.
- Needed fields (current DNA field names, read generically by name/size via
  SDNA so exact byte widths aren't hardcoded): `Object.type` (OB_MESH=1),
  `Object.data` (pointer to Mesh), `Object.loc[3]`, `Object.rotmode`
  (0=quaternion via `Object.quat[4]`, else Euler XYZ via `Object.rot[3]`),
  `Object.scale[3]`, `Mesh.verts_num`.

### Coordinate conversion
Blender is Z-up; this renderer is Y-up (existing camera/scene convention).
Imported transforms are converted once, at import time, into engine space
so the renderer stays completely unaware that "Blender" exists:
- Position: `(x, y, z)_blender -> (x, z, -y)_render`.
- Rotation: build the rotation matrix in Blender's own axis convention
  (`Rz*Ry*Rx` for Euler XYZ, or quaternion-to-matrix), then conjugate by the
  fixed change-of-basis matrix C (`C * R * C^T`) to get the render-space
  rotation matrix — computed directly via matrix multiplication, not
  hand-derived per-axis angle substitution.
- Scale: same axis permutation as position (Y/Z swap), magnitude only.
- Cube-only correction: Blender's default cube is 2x2x2 but the engine's
  built-in cube mesh is a 1x1x1 unit cube, so imported cube scale is
  doubled. The new plane mesh is authored at 2x2 (half-extent 1) to match
  Blender's default plane exactly, so no correction is needed there.

### Scene representation
`GameState` holds a fixed-capacity `SceneObject objects[kMaxSceneObjects]`
(`kMaxSceneObjects = 128`, pre-allocated at `Init` so `ArenaPush` is still
only ever called during `Init`, matching the existing arena invariant) plus
`objectCount`. Each `SceneObject` has position/scale/a precomputed rotation
matrix, plus `rotationEuler`/`rotationSpeed` used only by objects that spin
(the 3 demo cubes; imported objects get `rotationSpeed = 0` and are static).
`GameUpdate` advances `rotationEuler` and rebuilds the rotation matrix only
for objects with nonzero `rotationSpeed`. Importing a file replaces the
scene entirely (sets `objectCount` to the imported count).

### New file: `src/blend_file.h/.cpp`
Pure, portable C++ (no platform/Metal headers), parallel to `math3d.h` as a
shared low-level utility. Owns zstd decompression, header/block/SDNA
parsing, and a small by-name field-read API (`BlendFileNextBlock`,
`BlendFileReadFloatArray`, `BlendFileReadInt`, `BlendFileFollowPointer`).
Depends on Homebrew's libzstd.

### Known limitations (documented, not silently wrong)
- Only Blender 5.0+ (new-format) `.blend` files.
- Only Euler-XYZ and quaternion rotation modes (axis-angle and non-XYZ
  Euler orders fall back to reading `rot[3]` as XYZ).
- Mesh classification is vertex-count-based (4 verts = plane, anything else
  = cube); edited/non-primitive meshes render as whichever built-in shape
  their vertex count implies.
- A blend file with more than 128 cube/plane objects is clamped.

## Feature: Trackpad camera controls (pan / pinch-zoom / shift-orbit)

### Problem
The camera is currently a fixed `Mat4LookAt` computed once in
`RendererInit` and never touched again. Add trackpad gestures to move it:
two-finger drag pans, pinch zooms, shift + two-finger drag orbits.

### Scope decision
The camera is an orbit camera (target point + distance + yaw + pitch), not
a free-fly camera — matches the existing single-target-at-origin scene and
keeps the math small (no quaternions/free rotation needed). Camera state
and update logic live in `RendererState`/`renderer_metal.mm`, since a
camera is a rendering concern, not gameplay — `GameState` is untouched.

### Data flow
Gestures are AppKit (`NSEvent`) input, so only `platform_macos.mm` may read
them, per the existing "only platform_macos.mm touches AppKit" rule. It
accumulates raw per-frame deltas (trackpad points for pan/orbit, pinch
magnification for zoom) on a small `AppMetalView : MTKView` subclass by
overriding `scrollWheel:` (two-finger drag; routed to orbit instead of pan
when `NSEvent.modifierFlags` has Shift) and `magnifyWithEvent:` (pinch).
Each frame, `drawInMTKView:` reads and resets those accumulators into a
`CameraInput{panX, panY, zoomDelta, orbitYaw, orbitPitch}` struct (defined
in `renderer_metal.h`, plain floats, no AppKit/Metal types) and passes it
through the existing `FrameUpdate` call — the 3-function `app.h` API is
unchanged in shape, just gains this one parameter alongside `deltaTime`.
`FrameUpdate` forwards it to a new `RendererUpdateCamera`, which is where
all sensitivity/clamping constants live.

### Camera math
Spherical orbit around `cameraTarget`:
`eye = target + distance * (cos(pitch)sin(yaw), sin(pitch), cos(pitch)cos(yaw))`,
looking at `target` with world-up `(0,1,0)` via the existing `Mat4LookAt`.
- Pan moves `cameraTarget` along the camera's screen-space right/up axes
  (derived algebraically from yaw/pitch, not a second `Mat4LookAt` call),
  scaled by `cameraDistance` so pan speed matches the current zoom level.
- Pinch scales `cameraDistance`, clamped to `[2.5, 60]` so it can't cross
  the near plane or zoom out past the far plane.
- Shift-drag adds to yaw/pitch; pitch clamped to +-~86 degrees to avoid
  the view flipping through the poles.
- The projection matrix is recomputed only when the drawable size
  changes (see the resizable-window feature); on a camera change only the
  view matrix is rebuilt.

### Known limitations
- Gesture sign/sensitivity constants are a best-effort default tuned by
  feel, not measured against a specific trackpad — adjust the constants at
  the top of `renderer_metal.mm` if a gesture feels inverted or too
  fast/slow.

## Feature: Resizable window without image distortion

### Problem
The window is fixed-size. Making it resizable naively would stretch the
cubes, because the perspective projection's aspect ratio is baked in once
at `RendererInit` and never updated.

### Change
- `platform_macos.mm`: add `NSWindowStyleMaskResizable` to the window
  style mask.
- Route drawable-size changes to the renderer. `MTKView`'s delegate
  already gets `mtkView:drawableSizeWillChange:` (fired once at startup
  and on every resize); implement it to call a new `FrameResize(arena,
  aspectRatio)` on the `app.h` API, which forwards to a new
  `RendererResize(renderer, aspectRatio)`.
- `RendererResize` rebuilds `projection` from the new aspect ratio (same
  fov / near / far as `RendererInit`) and recomposes `viewProjection`
  from the current camera. The camera code path is unchanged.

### Notes
- Aspect ratio is `drawableSize.width / drawableSize.height` (pixels), not
  point size; for a plain perspective matrix the ratio is what matters and
  the two agree, but using the drawable size keeps it correct if a
  content-scale change ever fires this callback on its own.

## Feature: Screen-space ambient occlusion

### Problem
The scene is lit by a single directional light plus a flat ambient term,
so surfaces in contact (a cube meeting the floor, two cubes touching) have
no darkening between them and the scene reads as flat. Add SSAO so
crevices and contact points are occluded.

### Approach
Screen-space, so it works for the demo cubes and any imported `.blend`
scene without per-object work. This turns the single forward pass into a
small deferred pipeline, all encoded on the platform's one command buffer
in `RendererRender`:

1. **Geometry pass** -> two `RGBA16Float` render targets: view-space
   position (`.w = 1` marks a written texel) and view-space normal, plus a
   private depth target. Same vertex/index buffers and cull state as the
   old forward pass; the per-object uniform is now `{modelViewProjection,
   modelView}`.
2. **AO pass** (full-screen triangle) -> `R8Unorm`. Classic hemisphere
   kernel: 32 sample offsets (generated once at init, biased toward the
   origin) rotated per-pixel by a tiled 4x4 noise texture, each projected
   back to screen space and depth-compared against the position target. A
   `smoothstep` range check rejects occluders across large depth gaps.
3. **Blur pass** (full-screen) -> `R8Unorm`. 4x4 box blur, matching the
   noise tile size, to remove the per-pixel noise the rotation adds.
4. **Lighting pass** (full-screen) -> the drawable. Reads position,
   normal, and blurred AO; applies `base * (0.35 * ao + 0.65 * diffuse)`
   with the light direction transformed into view space on the CPU.
   Background texels (position `.w == 0`) get the old clear color.

### G-buffer / AO / blur targets
Owned by `RendererState`, sized to the drawable, and the one set of
resources the renderer (re)creates outside `Init` — `AllocateScreenTargets`
runs from `RendererResize` whenever the size actually changes. The arena
invariant is unaffected: these are Metal allocations, not `ArenaPush`.

### Tuning / debug
`kAoRadius` / `kAoBias` / `kAoPower` are constants at the top of
`renderer_metal.mm`. The `o` key cycles a debug view
(`CameraInput.cycleDebugView` -> `RendererState.debugMode`): 0 = normal,
1 = raw AO buffer, 2 = AO disabled.

### Known limitations
- Radius is a single world-space constant tuned for ~1-unit geometry; very
  large or very small imported scenes may need it adjusted.
- Normals are `modelView * normal` with no inverse-transpose, so a
  non-uniformly scaled imported object has slightly skewed AO normals
  (same simplification the original forward shader made).
- No half-resolution AO or temporal accumulation; the blur is the only
  denoise.

## Feature: FXAA

### Problem
Cube/plane edges alias badly — the renderer has no multisampling and the
deferred pipeline rules out MSAA on the lighting output anyway. Add FXAA, a
single full-screen post pass that finds luma edges in the final image and
blends across them.

### Change
- The lighting pass stops writing the drawable directly. It writes a new
  screen-sized `litColorTexture` (drawable's `colorFormat`, one more entry
  in `AllocateScreenTargets`). A new `fxaa_fragment` full-screen pass reads
  that texture and writes the drawable.
- `fxaa_fragment` is the classic compact FXAA (luma from RGB dot 0.299/
  0.587/0.114, 3x3 luma corners -> edge direction, up to 4 taps along it).
  Its only uniform is `1 / screenSize`.
- Toggle: the `f` key flips `RendererState.fxaaEnabled`
  (`CameraInput.toggleFxaa`, read in `platform_macos.mm` next to the `o`
  key). When off, the lighting pass targets the drawable again and the FXAA
  pass is skipped — `EncodeLightingPass` takes the destination texture as a
  parameter so both wirings share one code path.

### Known limitations
- Compact FXAA only (no FXAA 3.11 quality presets, no subpixel-aliasing
  tuning knobs). Good enough for hard-surface cube edges.
- Runs on the LDR lit image with no separate luma/alpha channel, so very
  dark or very bright edges antialias slightly less well.

## Feature: Debug HUD (frame timing)

### Problem
There is no way to see how the renderer performs. Add an overlay showing
current framerate, a scrolling frame-time graph, average frame time, and
the 1% low.

### Scope decision
The HUD is an AppKit overlay, not a Metal pass. Frame time is already known
platform-side (`drawInMTKView:` computes `deltaTime`), and text/graph
drawing is trivial with `NSString drawAtPoint:` / `NSRectFill` versus
building a glyph atlas and quad batcher in the renderer. The renderer and
`app.h` API are untouched.

### Pieces
- `src/frame_stats.h` — header-only pure C++, in the spirit of `math3d.h`:
  a fixed 240-sample ring buffer of millisecond frame times plus
  `FrameStatsPush`, `FrameStatsMeanMs(lastN)`, and
  `FrameStatsOnePercentLowMs` (99th-percentile frame time, i.e. worst frame
  after setting the slowest ~1% aside).
- `DebugHudView : NSView` in `platform_macos.mm` — owns a `FrameStats`,
  `pushFrameTime:` is called once per frame from `drawInMTKView:` and marks
  the view dirty at ~15 Hz. `drawRect:` draws, in the top-left corner: the
  text block (`FPS` from the mean of the last 20 frames plus the display's
  refresh rate; `avg` over the full buffer; `1% low` shown as both fps and
  ms) over a translucent panel, then the graph — one 1px column per sample,
  most recent at the right, vertical scale 0 to 3x the display's frame time,
  green/yellow/red at 1x/2x that time, with guide lines at 1x and 2x.
  `hitTest:` returns nil so camera drags pass through.
- The view is a full-size subview of the `MTKView`, `hidden = YES` at
  startup, toggled by the `F3` key (`AppMetalView.keyDown:` matches virtual
  keyCode 99 -> `pendingToggleHud`, flipped in `drawInMTKView:`).

### Refresh rate
Both `MTKView`'s built-in draw loop and an `NSView` `CADisplayLink` cap at
120 Hz on macOS regardless of `preferredFramesPerSecond` /
`preferredFrameRateRange` — verified on a 240 Hz display where
`CVDisplayLink` and `NSScreen.maximumFramesPerSecond` both report 240 but
the `NSView` `CADisplayLink` still fires only 120x/sec, even in fullscreen.
So `AppDelegate` pauses the `MTKView` (`paused = YES`,
`enableSetNeedsDisplay = NO`) and drives frames from a `CAMetalDisplayLink`
built on the view's `CAMetalLayer`. Its `metalDisplayLink:needsUpdate:`
delegate callback (on the main run loop) passes `update.drawable` to
`AppViewDelegate.renderIntoDrawable:`, which runs `FrameUpdate` +
`FrameRender`. `preferredFrameRateRange` is pinned to
`NSScreen.maximumFramesPerSecond` for the window's current display
(`matchDisplayRefreshRate`, re-applied from `windowDidChangeScreen:`); the
same rate feeds the HUD its per-frame millisecond target. Measured ~240
callbacks/sec on the 240 Hz display.

Two macOS fullscreen quirks each halve the frame rate on a 240 Hz display,
and both are worked around:

1. AppKit's own fullscreen (green button / Spaces) throttles the
   `CAMetalDisplayLink` to 120 Hz with no override, so it is disabled
   (`NSWindowCollectionBehaviorFullScreenNone`). "Fullscreen" is instead a
   borderless window (`AppDelegate.toggleBorderlessFullscreen`): swap to
   `NSWindowStyleMaskBorderless`, dock/menu bar hidden, no shadow, and back.
   Reached via the View menu's "Toggle Full Screen" (Cmd-F) and any
   `-toggleFullScreen:` (caught by `AppMetalView`'s override), the green
   zoom button (`windowShouldZoom:` toggles and returns NO), or Escape
   (only while fullscreen). `AppWindow` overrides
   `canBecomeKeyWindow`/`canBecomeMainWindow` (a borderless `NSWindow`
   refuses key status by default, which would break keyboard input).
2. A borderless window that covers the display *exactly* triggers macOS's
   fullscreen bypass / direct scanout, which double-buffers and again pins
   to 120 Hz. The window is therefore sized to overhang the screen by 1px
   (`NSInsetRect(screen.frame, -1, -1)`), keeping the normal compositor
   path; the 1px is clipped off-screen.

The paused `MTKView` defers `CAMetalLayer.drawableSize` updates to a
`-draw` that never happens, so `AppViewDelegate.resizeToDrawableSize:` sets
the layer size explicitly — from `mtkView:drawableSizeWillChange:` and
after the toggle — otherwise fullscreen renders at the stale windowed size
and is upscaled (blurry). With both quirks handled and the trimmed
pipeline, fullscreen at a 5124x2884 backing runs ~2 ms GPU / 240 fps on an
M2 Pro.

### Known limitations
- `F3` only reaches the app when the system keyboard setting "Use F1, F2,
  etc. keys as standard function keys" is on, or when pressed as `fn`+`F3`;
  otherwise macOS eats it for Mission Control.
- HUD frame-time samples are wall-clock deltas between `renderIntoDrawable:`
  calls (CPU-side present cadence). The `F3` HUD also shows per-pass GPU
  time from a `MTLCounterSampleBuffer` (see "SSAO performance pass").

## Feature: SSAO performance pass

### Problem
In fullscreen, loading `scene1.blend` gives much worse frame times than the
3-cube test scene at the same resolution. Cause: `ao_fragment` early-outs
(`positionSample.w < 0.5`) on background pixels, so with 3 small cubes most
of the screen skips the 32-sample kernel; the blend scene's ground plane
fills the frame, so every pixel runs the full kernel (32 iterations, each a
`projection * float4` plus two RGBA16F texture fetches) over the whole
drawable. Object count is not the issue (10 objects vs 3). Target: 4.16 ms
(240 Hz).

### Changes
1. Per-pass GPU timing on the F3 HUD (do first, to measure).
   - `RendererState` gets a `MTLCounterSampleBuffer` (timestamp counter set,
     `sampleCount` 10) and `float passMs[6]` (geometry, ao, blur, lighting,
     fxaa, total).
   - Each render pass descriptor gets
     `sampleBufferAttachments[0]` set to `{startOfVertexSampleIndex: slot*2,
     endOfFragmentSampleIndex: slot*2+1}` with fixed slots
     geometry 0 / ao 1 / blur 2 / lighting 3 / fxaa 4.
   - A `commandBuffer` completion handler resolves the range and fills
     `passMs`; per-pass values assume Apple-silicon nanosecond timestamps,
     total comes from `GPUEndTime - GPUStartTime` (unit-independent). Passes
     not encoded this frame are forced to 0. The handler runs on a
     background thread and writes plain floats the HUD reads on the main
     thread — a benign race, acceptable for a debug readout.
   - New `RendererPassTimings RendererLastFrameTimings(const RendererState*)`
     and `FrameGpuTimings(Arena*)`; `DebugHudView` draws two extra lines.
2. Half-resolution SSAO. `aoRawTexture` / `aoBlurTexture` allocated at
   `(width+1)/2 x (height+1)/2`. `RendererState.aoWidth/aoHeight` track it.
   The AO and blur passes render at that size (render pass viewport defaults
   to the attachment size); the lighting pass already samples the AO result
   with a linear sampler, so upscale is free. `ao_fragment`'s noise tiling
   and `blur_fragment`'s texel size switch from screen size to AO size.
3. AO kernel 32 -> 16 samples (`kAoKernelSize` in `renderer_metal.mm`,
   `kKernelSize` in the shader, `AoParams.sampleOffsets` length). The blur
   hides the reduced sample count.
4. Skip the AO and blur passes entirely when AO is disabled (the `o`-key
   debug mode 2); the lighting shader already forces occlusion to 1 there.

### Pipeline trim (follow-up, after the fullscreen work)
Once fullscreen ran at true 5K, the deferred passes were the cost. Two
cuts, no visible quality change:
5. Drop the position g-buffer. The geometry pass now writes one RGBA16F
   target: `xyz` = view-space normal, `w` = view-space Z (negative for real
   geometry; the pass clears `w` to 1.0 so `w >= 0.5` means background).
   `ReconstructViewPosition(uv, viewZ, projection)` in `ao_fragment` rebuilds
   view-space X/Y from the fullscreen-triangle uv and the projection's
   `[0][0]`/`[1][1]` terms — no matrix inverse, no dependence on the depth
   buffer's encoding. `lighting_fragment` only needed the background test, so
   it just reads `normalSample.w`.
6. Fold the 4x4 AO box blur into `lighting_fragment` (it already sampled the
   AO texture). Removes the blur pipeline, `blur_fragment`, `aoBlurTexture`,
   and a full-screen pass. `LightParams.misc.yz` carries `1/aoWidth`,
   `1/aoHeight`. Timer slots renumber to geometry 0 / ao 1 / lighting 2 /
   fxaa 3.

Result: geometry writes 8 bytes/texel instead of 16, the AO loop reads the
normal target (which it needs anyway) instead of a separate position
target, and one full-screen pass is gone. Fullscreen 5124x2884 on an M2
Pro: ~8.6 ms -> ~2 ms GPU. (The earlier 8.6 ms was mostly the
fullscreen-bypass stall, not compute; see the refresh-rate section.)

### Not doing (would need sign-off)
- Moving SSAO to a compute shader with threadgroup-shared samples.
- Per-frame ring of counter sample buffers (the current single buffer can
  under-report per-pass times when several frames are in flight; the total
  from `GPUStartTime`/`GPUEndTime` stays accurate).

## Feature: in-app UI panel (the renderer as one pane)

### Problem
The renderer should be one pane in a larger tool (like Unity's Game view),
with room for a developer to build a toolbar / timeline / inspector around
it. Constraints: no Electron, no Dear ImGui dependency, must stay portable
to a future Windows backend, and the UI is authored in code by a developer
(no visual editor needed).

### Decisions
- **Immediate-mode core with a small retained blob.** `ui.h`/`ui.cpp` is
  pure C++ (no Metal, no AppKit). Each frame it reads one `FrameInput`,
  runs the widget calls, and fills a flat vertex list. The only state that
  survives frames is the splitter position, the hot/active widget id, the
  previous mouse state, and the demo widget values. No widget tree, no
  event routing.
- **The panel shrinks the 3D viewport.** The scene renders into the
  rectangle left of the panel, not behind it. The renderer's screen-sized
  targets are sized to that content rect; the final pass writes a
  `(0,0,contentW,contentH)` viewport of the drawable; the UI pass fills the
  rest.
- **Text is solid-color quads via vendored `stb_easy_font.h`** (public
  domain, no texture, no atlas). The UI Metal shader is transform + vertex
  color only. Crisp glyph atlas text is a later swap if wanted.
- **`CameraInput` becomes `FrameInput`** and gains `mouseX`, `mouseY`
  (backing pixels, top-left origin), `mouseDown`. Moved to a pure header
  `frame_input.h` shared by `renderer_metal.h` and `ui.h` so the UI core
  pulls in no Metal.
- **New layer split.** `ui.h`/`.cpp` (pure C++, layout + interaction +
  draw-list) and `ui_render_metal.h`/`.mm` (Metal backend: pipeline, vertex
  upload, one Load-action pass onto the drawable). A Windows port adds
  `ui_render_d3d.*` and reuses `ui.cpp` unchanged.
- `RendererRender` stops calling `presentDrawable`/`commit`; `FrameRender`
  encodes scene, then UI, then presents once.

### Not doing yet (would be follow-ups)
- Left / top / bottom panels and a real dock layout (only a right panel now;
  content origin is hardcoded to 0,0).
- Suppressing camera zoom/pan while the cursor is over the panel (camera
  uses right/middle/scroll, the UI uses left-drag, so they don't collide).
- Reallocation throttling while dragging the splitter (a few screen-target
  reallocs per drag is acceptable for now).
- Glyph-atlas text, widget theming, keyboard focus/text entry.

### Steps (done)
1. Branch `ui-panel`.
2. `frame_input.h`: `FrameInput`. Rewire `renderer_metal.*`, `app.*`,
   `platform_macos.mm` off `CameraInput`.
3. Vendor `src/third_party/stb_easy_font.h`.
4. `ui.h`/`ui.cpp`: `UiVertex`, `UiState`, `UiInit`, `UiHandleResize`,
   `UiBuildFrame`, `UiContentWidth/Height`, `UiDrawableWidth/Height`,
   `UiVertices`/`UiVertexCount`. Immediate-mode button / slider / vertical
   splitter + a vertical layout cursor + demo values.
5. `ui_render_metal.h`/`.mm`: `UiRenderInit`, `UiRenderEncode`. Embedded
   solid-color shader, per-frame vertex buffer, quad index buffer,
   alpha-blended Load-action pass to `target.drawable.texture`.
6. `renderer_metal.*`: `RendererResize` -> `RendererSetContentSize`
   (projection + targets from content size); `setViewport` clamp on the
   final drawable-writing pass; drop present/commit from `RendererRender`.
7. `app.*`: `AppState` gains `UiState*` + `UiRenderState*`; `FrameUpdate`
   runs `UiBuildFrame`; `FrameRender` runs `RendererSetContentSize` ->
   `RendererRender` -> `UiRenderEncode` -> present + commit; `FrameResize`
   runs `UiHandleResize`.
8. `platform_macos.mm`: tracking area + left `mouseDown:`/`mouseUp:`/
   `mouseMoved:`/`mouseDragged:` -> pending mouse pos (backing px, top-left)
   + down flag; fill `FrameInput`.
9. `Makefile`: add `src/ui.cpp`, `src/ui_render_metal.mm`.
10. `make run`; visually verify: right-side dark panel; 3 buttons highlight
    on hover and depress on click; 2 sliders track the cursor with live
    handle + readout; dragging the splitter resizes the panel, the widgets
    reflow to the new width, and the 3D viewport grows/shrinks to fill the
    rest; no Metal validation errors.
11. Update this section and `CLAUDE.md`.

## Feature: portable menu model + in-app menu strip

### Problem
Borderless fullscreen hides the macOS menu bar, so File > Import File and
View > Toggle Full Screen become unreachable. The menu structure and
dispatch also lived entirely in `platform_macos.mm` (`InstallMainMenu` plus
three distinct AppKit selectors), which a Windows port would have to
reinvent from scratch.

### Decisions
- **The menu is portable data.** `menu.h`/`menu.cpp`: a `MenuAction` enum,
  `MenuItem`/`Menu`/`MenuBar` structs, and `MenuBarDefault()` building the
  App / File / View layout. Both the native `NSMenu` and the in-app strip
  are generated from this one model.
- **One dispatch path.** `MenuInvoke(action, MenuState*, PlatformMenuHooks)`
  in portable code. App-state actions (toggle menu bar) mutate `MenuState`;
  platform-only actions (import file, toggle fullscreen, quit) call through
  a `PlatformMenuHooks` function-pointer struct that `platform_macos.mm`
  fills with C trampolines and hands to `Init`. Native menu-bar clicks
  (`-dispatchMenuAction:` reading `NSMenuItem.tag`) and in-app menu clicks
  (`UiTakeMenuAction`) both funnel through `AppDispatchMenuAction(arena,
  action)`.
- **Strip visibility = `fullscreen || MenuState.showMenuBar`.** View >
  Toggle Menu Bar flips the preference. While fullscreen the strip cannot
  be hidden: visibility ORs in `fullscreen`, the native item is disabled by
  `-validateMenuItem:`, and the in-app item renders greyed and ignores
  clicks.
- **The strip shrinks the viewport.** `RendererSetContentSize` becomes
  `RendererSetContentRect(x, y, w, h)`. The scene targets stay `w x h`; the
  final drawable-writing pass takes an `(x, y, w, h)` `MTLViewport` so the
  strip's band at the top of the drawable is left for the UI pass to fill.
  `y` = strip height when visible, else 0. Intermediate passes still use
  `(0, 0, w, h)`.
- `FrameInput` gains `bool fullscreen` — platform state the app needs each
  frame to compute strip visibility.
- In-app text is ASCII-only (`stb_easy_font` indexes a 32..126 table), so
  labels use "Import File..." not "Import File…".

### Not doing yet
- Checkmark glyph for the in-app toggle item (greyed text + native
  `NSControlStateValue` only).
- Hover-to-switch between open menus (click to open / switch / close only).
- Submenus, separators, off-screen dropdown clamping.
- Removing `AppMetalView -toggleFullScreen:` (now only a defensive catch
  for a stray responder-chain message; the menu no longer uses it).

### Steps
1. `menu.h`/`menu.cpp`: model, `MenuBarDefault`, `MenuInvoke`,
   `MenuState`, `PlatformMenuHooks`.
2. `frame_input.h`: `+bool fullscreen`.
3. `renderer_metal.*`: `RendererSetContentSize` -> `RendererSetContentRect`;
   origin `MTLViewport` on the final pass, `(0,0,w,h)` on intermediates.
4. `ui.*`: strip + dropdown render/interaction, `menuHasPointer` input
   swallow; `UiBuildFrame(ui, input, showMenuBarPref)`; `UiTakeMenuAction`;
   `UiContentOriginX/Y`; content height minus strip; layout cursor starts
   below the strip.
5. `app.*`: `AppState{+MenuState, +PlatformMenuHooks}`; `Init(..., hooks)`;
   `AppDispatchMenuAction`; `AppMenuState`; `FrameUpdate` dispatches
   `UiTakeMenuAction`; `FrameRender` passes the content rect.
6. `platform_macos.mm`: `InstallMainMenu` from `MenuBarDefault()` with a
   unified `-dispatchMenuAction:` + tag; hook trampolines; `-validateMenuItem:`
   for the toggle item; `frameInput.fullscreen`.
7. `Makefile`: `+src/menu.cpp`.
8. `make run`; verify (below).
9. Update this section and `CLAUDE.md`.

### Verify
- Windowed: native menu bar still works (Import, Toggle Full Screen, Quit)
  through the new dispatch; no in-app strip until View > Toggle Menu Bar.
- Toggle Menu Bar windowed: strip shows/hides at the top and the viewport
  grows/shrinks to match.
- Fullscreen (Cmd-F): strip forced on at the top, viewport sits below it, 3
  cubes + right panel still render; File > Import File... opens the panel;
  View > Toggle Full Screen exits; Toggle Menu Bar is greyed and inert.
- No Metal validation errors.

## Feature: UI foundation refactor (for text atlas / keybindings / docking)

### Problem
The immediate-mode UI shipped enough to prototype, but the next agents (glyph
atlas, keybinding layer, dockable panels) need a stable vertex ABI, a real
input snapshot, and a clean state boundary. Do those now while cheap.

### Changes
- `UiVertex` gains `float u, v` and `float mode` (0 = solid, 1 reserved for
  textured glyphs). `ui_render_metal` declares attributes 2/3 in the vertex
  descriptor and the shader; the fragment still returns vertex color. All
  push paths set `u = v = mode = 0`.
- `FrameInput` becomes a per-frame input snapshot: `mouseLeftDown` /
  `mouseRightDown` / `mouseMiddleDown` level bits, `scrollX` / `scrollY`
  (UI scroll, separate from the camera pan/zoom deltas), `shift` / `ctrl` /
  `alt` / `cmd`, and a fixed `KeyEvent keyEvents[16]` queue with
  `keyEventCount` (overflow dropped). `o` / `f` / `F3` keep their one-shot
  bools; those keys also enqueue a `KeyEvent`.
- `platform_macos.mm`: `AppMetalView` gains `rightMouseDown/Up`,
  `otherMouseDown/Up`, `flagsChanged:`, `keyUp:`, an `enqueueKey:pressed:` /
  `drainKeyEventsInto:max:` pair over a `KeyEvent[kMaxKeyEvents]` ivar, and
  a UI-scroll accumulator in `scrollWheel:` alongside the untouched camera
  path. `renderIntoDrawable:` assembles the fuller `FrameInput` and resets
  the per-frame accumulators (deltas, scroll, key queue) but not level
  state.
- `UiWantsMouse` / `UiWantsKeyboard`: true when the cursor is over the panel
  / splitter / menu strip / open dropdown, or a UI drag is active. `app.mm`
  zeroes the camera pan/zoom/orbit deltas on frames where `UiWantsMouse` is
  true, so dragging a slider no longer also moves the camera.
- Demo slider values move out of `UiState` into an app-owned `UiDemoState`
  passed into `UiBuildFrame` by pointer. UiState = view state only; the UI
  renders app/game state and returns intents.
- `PushVertex` overflow is now `assert(vertexCount < kMaxVertices)` instead
  of a silent drop; `kMaxVertices` (and `ui_render_metal`'s
  `kMaxUiVertices`) raised to 65536.

### Verify
- `make` clean, no new warnings; `make run` with Metal API + GPU + shader
  validation: 3 cubes + right panel render, no validation errors.
- Buttons highlight on hover / depress on click; both sliders drag and
  update their `%.2f` readouts.
- `o` / `f` / `F3` unchanged; camera orbit/pan/zoom works over the viewport
  but is suppressed while the cursor is over the panel.

## Feature: throttle scene rendering when unseen or idle

### Problem
The frame loop rendered the scene every display refresh unconditionally,
even when the window was hidden or nothing on screen was changing. Two
levels of waste: (a) rendering while the result cannot be seen at all (app
inactive, window minimized or fully occluded), (b) rendering identical
frames while the scene is static and no UI interaction is happening.

### Design
- **Not visible -> pause the frame loop.** `AppDelegate.updateFrameLoopRunning`
  computes `NSApp.active && windowVisible && !window.miniaturized`
  (`windowVisible` from `NSWindowOcclusionStateVisible`) and sets
  `CAMetalDisplayLink.paused` accordingly. Driven by
  `windowDidChangeOcclusionState:`, `applicationDidBecomeActive:` /
  `applicationDidResignActive:`, `windowDidMiniaturize:` /
  `windowDidDeminiaturize:`. On resume it zeroes `AppViewDelegate.lastTime`
  (so the first frame back does not integrate the whole gap) and calls
  `AppRequestRender`.
- **Idle -> skip the render.** `FrameUpdate` returns `bool` — true when the
  scene animated, the camera moved, a render toggle fired, the UI is being
  interacted with (`UiWantsMouse || UiWantsKeyboard`, or a menu action
  fired), or a render was explicitly requested. `renderIntoDrawable:` skips
  `FrameRender` (and the HUD frame-time push) when it returns false, leaving
  the last presented drawable on screen. `FrameUpdate` still runs every tick.
- **Spin pause.** `GameState.spinPaused` (default off) plus `GameToggleSpin`,
  bound to the `p` key via `FrameInput.toggleSpin`. `GameUpdate` returns
  whether any rotation actually advanced, which is what makes idle detection
  possible for the demo scene.
- **`AppRequestRender(arena)`** sets a small `forcedRenderFrames` countdown
  in `AppState` so a resumed loop, a resize, an import, a HUD toggle, or a
  menu action flushes through multi-buffered targets.

### Measured (M-series, 240 Hz, MTL_DEBUG_LAYER + shader validation, no errors)
- Active + spinning: ~35% CPU.
- Spin paused (`p`), camera still (idle-skip): ~3% CPU (FrameUpdate only).
- App inactive / window minimized: <1% CPU (loop paused); resumes clean.

### Not doing
- Pausing the display link on idle (not just skipping the render) — would
  need every input path to wake it. Left running per the "FrameUpdate every
  tick" contract.

## Feature: glyph-atlas text (replaces stb_easy_font)

### Problem
`stb_easy_font` renders text as blocky untextured vector quads, is
ASCII-only with an out-of-bounds trap on any codepoint outside 32..126, and
has no path to sizes/atlas without a rewrite. Recommendation #5 from the
architecture review.

### Approach
- Vendor `src/third_party/font8x8_basic.h` — Daniel Hepper's public-domain
  8x8 bitmap font (derived from the public-domain IBM VGA fonts), covering
  U+0000..U+007F. Vendored change: the array is made
  `static const unsigned char` so the header can be `#included` without a
  multiple-definition / signedness issue.
- `ui_render_metal.mm` unpacks ASCII 32..127 into one `MTLPixelFormatR8Unorm`
  atlas texture at init: a `kFontCharCount`-wide, one-row grid of 8x8 cells,
  bit set -> 255. Stored as `UiRenderState.fontAtlas` (arena-resident; the
  calloc-zeroed arena makes the first `id` assignment safe, same as the
  pipeline/vertexBuffer fields).
- The UI shader gains a texture binding; `mode > 0.5` multiplies the
  fragment alpha by the nearest-sampled atlas red channel, `mode == 0` is
  the unchanged solid path.
- `ui.cpp`: `PushText` emits one `mode = 1` quad per character with uv into
  that glyph's cell, advancing `8 * kTextScale` px each. `TextWidth` is
  `strlen * 8 * kTextScale` (monospace). Codepoints outside the atlas
  advance but draw nothing. `stb_easy_font.h` and its `#pragma` wrapper are
  deleted.
- `kGlyphHeight` becomes 8 (the cell height) so button / menu-row vertical
  centering stays correct.

### Trade
Crisp nearest-sampled bitmap text (classic 8x8 VGA look), wider advance
than the old font so labels take more width — everything still fits the
260px panel and the menu strip. A proportional / SDF font is a later swap;
the `UiVertex` uv+mode channels and the shader branch already support it.

### Verify
- `make` clean, no new warnings; run with `MTL_DEBUG_LAYER=1
  MTL_SHADER_VALIDATION=1`, no validation errors.
- Windowed: "Button A/B", "Reset", "Speed"/"Zoom" + `0.50`/`0.35` readouts
  render as clean readable glyphs, centered/positioned as before.
- Fullscreen: "Renderer"/"File"/"View" strip titles render in the new font;
  3 cubes + panel unaffected.
