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
  text block (`FPS` from the mean of the last 20 frames; `avg` over the
  full buffer; `1% low` shown as both fps and ms) over a translucent panel,
  then the graph — one 1px column per sample, most recent at the right,
  fixed 0–50 ms scale, green/yellow/red under 60/30 fps, with 60 and 30 fps
  guide lines. `hitTest:` returns nil so camera drags pass through.
- The view is a full-size subview of the `MTKView`, `hidden = YES` at
  startup, toggled by the `F3` key (`AppMetalView.keyDown:` matches virtual
  keyCode 99 -> `pendingToggleHud`, flipped in `drawInMTKView:`).

### Known limitations
- `F3` only reaches the app when the system keyboard setting "Use F1, F2,
  etc. keys as standard function keys" is on, or when pressed as `fn`+`F3`;
  otherwise macOS eats it for Mission Control.
- Samples are wall-clock deltas between `drawInMTKView:` calls (CPU-side
  present cadence), not GPU timestamp spans.
