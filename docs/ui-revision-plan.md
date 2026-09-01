# UI revision plan (branch `ui-panel`)

## Problem

The scene editor works but the UI is a first draft: a demo right-panel,
an overloaded toolbar, a timeline that does not span the window, a bitmap
font, and a few layout/render bugs. `notes.md` lists 11 changes that turn
it into a usable editor. This plan groups them into batches that each
build, run, and can be verified before the next starts.

## Locked decisions (from Q&A)

| Topic | Decision |
|-------|----------|
| Font (item 3) | macOS CoreText rasterizes a variable-width glyph atlas in `ui_render_metal.mm`. `ArgentumSans-Regular.ttf` (OFL, from `argentum-sans.zip`) is vendored to `src/third_party/argentum/` and embedded as a byte-array header (`xxd -i`), matching the codebase's no-asset-pipeline style; CoreText loads it via `CTFontManagerCreateFontDescriptorsFromData`. System UI font is the fallback if the load fails. |
| Rename (item 6) | New reusable `UiTextInput` widget in `ui.cpp`; key events routed while focused; `UiWantsKeyboard` true during edit. |
| Rename / delete undo (items 6, 11) | Both go through `SceneSubmitCommand`. Rename payload = `{EntityId, char before[64], char after[64]}`. Delete = **one combined command** over the whole selection, snapshotting each entity's kind/name/transform/component data and re-adding on undo. |
| Marquee (item 5) | Drag on empty viewport space box-selects in **any** tool mode. No dedicated Select tool button. |
| Tool shortcuts (item 11) | Handled inline in `app.cpp`'s key loop first (compression approach). A later optional pass extracts a `src/shortcuts.{h,cpp}` registration table once the set is stable. |
| Delivery | One plan, then implement batch by batch; `make && make run` clean between each. |

## Open items / accepted v1 limitations

1. **Undoing a delete re-ids the entity**: a re-added entity gets a fresh
   `EntityId`, so a timeline track bound to a deleted audio source is not
   auto-relinked on undo. Accepted for v1; revisit if it bites.
2. Argentum ships many weights; only Regular is vendored now. SemiBold can
   be added later for panel/menu titles.

## Batches

### Batch 0 — shared input plumbing (no visible change)

- `frame_input.h`: add drag-hover fields valid every frame while a Finder
  drag is over the window: `bool dragHovering; float dragHoverX, dragHoverY;
  const char *const *dragHoverFiles; int dragHoverFileCount`. Keep the
  existing one-frame `droppedFiles`/`dropX`/`dropY` for the actual drop.
- `platform_macos.mm`: implement `draggingUpdated:` / `draggingExited:` to
  latch the hover point + filtered `.wav` names each frame; `performDragOperation:`
  unchanged.
- Verify scroll-wheel modifiers: `input.shift/ctrl/alt` already reflect the
  frame the scroll arrived on — good enough for item 9, no change.
- **Test**: build + run, no behavior change (temporary log of the hover
  point to confirm the callback fires, removed before commit).

### Batch 1 — layout + render-bug fixes (items 2, 4, 8)

- **Item 8**: `ResolvePanelLayout` resolves docks by fixed priority
  (Bottom, Top, then Left, Right) instead of panel-slot order, so the
  bottom Timeline spans the full width and the Left/Right panels shorten
  to sit above it.
- **Item 4**: per-panel minimum size so text never spills. Add a
  `minDockSize` / `minFloat{W,H}` to `UiBeginPanel` (or a small table keyed
  by panel id); clamp dock and float sizes up to it. Timeline and the
  outliner get a larger minimum than the current `kPanelMinDockSize = 150`.
- **Item 2**: diagnose the 1px edge seam on the viewport panel. Suspects:
  `floorf` on `contentRect` vs. un-floored panel rects leaving a sub-pixel
  gap; the `MTLViewport` for the content blit rounding differently from the
  UI panel rects; or the 1px panel border. Fix by snapping panel rects and
  the content rect to the same integer grid and matching the viewport to
  it. Confirm with a before/after screenshot.
- **Test**: run, resize the window, tear/redock each panel, screenshot the
  panel/viewport edges.

### Batch 2 — toolbar + shortcuts + tooltips (items 5, 11)

- `kToolButtons` -> `Move` (relabel of Translate), `Rotate`, `Scale` only.
  Drop Select / Add Source / Add Listener / Snap / Frame Selected from the
  strip.
- Default tool mode becomes `ToolMode_Translate` (set in
  `GameLoadDefaultScene` or `SceneInit`).
- `ToolButtonDef` gains `shortcutLabel`; `ToolButtonWidget` draws a tooltip
  box (label + key) under the bar while `hotId == id`.
- `app.cpp` key loop (only when `!UiWantsKeyboard`): `1` Move, `2` Rotate,
  `3` Scale, `Delete`/`Backspace` delete every selected entity, `n` (Batch 3)
  raises an "open add menu" intent the outliner reads next frame.
- **Cmd+Z / Cmd+Shift+Z** wired to `SceneUndo` / `SceneRedo` in the same
  loop. The undo stack existed but nothing invoked it; the combined-delete
  and transform commands are only meaningful once undo has a key.
- **Test**: run, press 1/2/3, hover the tools, select + Delete + Cmd+Z.

### Batch 3 — scene outliner panel (item 6)

- **`UiTextInput(ui, id, rect, char *buf, int cap)`**: click to focus,
  blinking caret, `Backspace`, printable chars from `input.keyEvents`,
  `Enter` commits, `Esc` cancels. `UiState` gains `focusedInputId`, an edit
  buffer, and caret position; `UiWantsKeyboard` returns true while focused.
- `scene.h`/`scene.cpp`: `int SceneEntities(const SceneState*, SceneEntityRow*, int max)`
  returning `{ EntityId id; const char *name; EntityKind kind; }` in a
  stable order; `void SceneRenameEntity(SceneState*, EntityId, const char*)`.
- Panel id 1 body (was "Controls"): one row per entity — kind glyph +
  name. Click selects (writes `SceneSelection`, round-trips with the
  viewport). Double-click swaps the name for a `UiTextInput`.
- `+` button opens a dropdown (reuse the menu-dropdown draw) with Cube /
  Plane / Source / Listener -> `SceneCreateEntityAt` at
  `RendererCameraFocus` (one undo step). The `n` shortcut opens the same
  dropdown.
- Delete the demo controls (Button A/B/Reset, Speed/Zoom sliders) and the
  now-unused `UiDemoState` wiring; `UiBuildFrame` loses its `demo` param.
- `scene.cpp`: `SceneCreateMeshEntityAt` (create + choose Cube/Plane in one
  undo step), `SceneRenameEntity` (one undoable command, `SceneCmdTag_Rename`).
- **Test**: run, add each kind, rename one, confirm selection round-trips
  both directions.

**Also folded into this batch (user-reported):**
- **Marquee has no visual**: `UiSetMarquee` (app feeds its box-select drag
  rect each frame) → `UiBuildFrame` draws a `theme::MarqueeFill` rectangle +
  outline.
- **Camera stutters on panel borders**: `app.cpp` latches
  `cameraDragActive` when a right/middle drag starts over the viewport and
  keeps feeding camera input until the button releases, even as the pointer
  crosses a panel.
- **Panel/toolbar hover highlight freezes on-screen after the pointer
  leaves**: the frame loop had stopped re-rendering. `app.cpp` now forces
  two more frames whenever the pointer moves on or just off UI chrome.

### Batch 4 — timeline scroll / zoom + drop preview (items 1, 9)

- Timeline view state moves onto `UiState`: `float timelineScrollSeconds`
  (pan) and `float timelinePixelsPerSecond` (zoom, was the
  `kTimelinePixelsPerSecond` constant). `TimelineTimeToX` / `XToTime` take
  both.
- `BuildTimelineBody`, when the cursor is over the panel and `scrollY != 0`:
  plain scroll pans lanes vertically; `Shift` pans time horizontally;
  `Alt`/`Option` zooms `pixelsPerSecond` about the cursor's time.
- **Item 1**: when `input.dragHovering` and the point is over the lanes,
  draw a ghost clip rect at the snapped drop time + highlight the target
  lane, using the Batch 0 hover fields. No model change.
- **Test**: run, scroll / Shift-scroll / Alt-scroll over the timeline;
  drag a `.wav` in and watch the ghost before releasing.

**Also folded into this batch (user-reported):**
- **UI zoom (Cmd + / Cmd - / Cmd 0)**: `ui.cpp` now lays out in logical
  units where `logical = realPixels / uiScale`; vertices scale up on push,
  incoming pointer/drop coords scale down, and the content-rect / drawable
  accessors report real pixels for the renderer. `UiSetUiScale` /
  `UiAdjustUiScale` (clamped 0.6–2.5); `app.cpp` binds the keys.
- **Panels wobble during window resize**: `platform_macos.mm`
  `resizeToDrawableSize:` now renders one frame synchronously against the
  new size (`[layer nextDrawable]` → `renderIntoDrawable:`) so the panels
  don't trail the window edge by a display-link frame.

### Batch 5 — selection color + semantic icons (items 7, 10)

- **Item 7**: the deferred g-buffer is a single target (normal.xyz,
  viewZ). Add a second color attachment to the geometry pass carrying base
  color; the geometry draw loop sets a per-entity `baseColor` uniform
  (white, or `theme::MeshSelected` blue when
  `SceneSelectionContains`). The lighting pass samples attachment 1 instead
  of the `kThemeMeshBase` constant. `theme.h`: `MeshBase` -> white, add
  `MeshSelected`. Fallback if MRT proves invasive: tint selected meshes in
  the existing unlit overlay pass — noted, not preferred.
- **Item 10**: `GizmoBuildIcons` swaps the generic billboard quad for
  line-geometry glyphs — headphones (headband arc + two ear cups) for
  listeners, speaker body + cone + two wave arcs for sources — keyed by
  the existing `kind`. New billboard helpers `BillboardPoint` / `IconQuad`
  / `IconArc` in `gizmo.cpp`.
- **Item 7 landed as**: a second `RGBA16Float` g-buffer attachment
  (`gAlbedoTexture`) carrying per-object albedo. `GeoUniforms` gains
  `baseColor`; `geometry_fragment` returns `GeoOut { gbuffer, albedo }`;
  `lighting_fragment` samples the albedo texture instead of the
  `kThemeMeshBase` constant. `theme.h`: `MeshBase` -> white,
  `MeshSelected` -> blue. `EncodeGeometryPass` sets the per-draw colour
  from `SceneSelectionContains`.
- **Test**: run, select meshes (turn blue, others white), confirm sources
  show a speaker and listeners headphones, active listener still tinted.

**Also folded into this batch (user-reported):**
- **Trackpad horizontal scroll pans the timeline** with no modifier
  (`input.scrollX`); vertical scroll pans time too when the lanes don't
  overflow, so a plain swipe always does something.

### Batch 6 — CoreText font (item 3)

- Vendor `ArgentumSans-Regular.ttf` + `OFL.txt` to
  `src/third_party/argentum/`; generate `argentum_sans_regular.h`
  (`xxd -i`) with the font bytes.
- `ui_render_metal.mm`: replace `BuildFontAtlas` with a CoreText path —
  `CGDataProvider` over the embedded bytes -> `CGFont` ->
  `CTFontCreateWithGraphicsFont` (fall back to the system UI font on
  failure), rasterize ASCII 32..126 into an R8 atlas via
  `CTFontDrawGlyphs` into a `CGBitmapContext`, record per-glyph
  `{u0,v0,u1,v1,advance,bearing,size}`.
- `ui.h`/`ui.cpp`: `PushText` / `TextWidth` consume the per-glyph metrics
  (variable advance) instead of a fixed cell; the metrics table is shared
  from the UI renderer to `ui.cpp` (a `UiFontMetrics` struct passed at
  init). Menu / panel / timeline layout math that assumes `kGlyphPixels`
  advance is updated.
- **Test**: run, every panel / menu / timeline label renders in Argentum
  (or the system font) with no clipping or overlap.

**Landed**: `ui_render_metal.mm` bakes the atlas via CoreText from the
embedded `.ttf`; `UiFontMetrics` (in `ui.h`) is handed to `ui.cpp` once at
startup (`UiSetFontMetrics`, file-static pointer). `PushText` / `TextWidth`
/ new `TextWidthN` do proportional layout; `PushGlyphQuad` replaces the
fixed-cell `PushGlyph`; every `kGlyphHeight * kTextScale` became
`TextLineHeight()`. Sampler switched to `filter::linear`. `CGBitmapContextCreate` draws bottom-up but stores its bytes top-down, so
glyph UVs are `v = (atlasHeight - cgY) / atlasHeight` (NOT `cgY/atlasHeight`
— that lined up for flat-bottomed capitals but chopped the overshoot on
round letters and the descenders on g/y/p/j). Vertical placement centres
the **cap height** (reference glyph 'H') in the `TextLineHeight()` box —
the font's raw ascent is loose. `UiFontMetrics` carries
`ascent`/`descent`/`lineHeight`/`pixelSize`; `TextCapHeight()` reads 'H'.
All verified by screenshot.
Makefile gains `-framework CoreText -framework CoreGraphics`.

**Also folded in (user-reported):**
- **1px flicker between viewport and the timeline panel**: the renderer's
  content viewport now floors its origin and overdraws its extent by 4px
  (`renderer_metal.mm` `RendererRender`); `RendererSetContentRect` rounds
  the target size instead of truncating. The final composite can no longer
  leave a sub-pixel sliver a panel doesn't cover.
- **Trackpad scroll axis**: vertical scroll only scrolls lanes now (never
  falls through to time-pan); horizontal scroll pans time; Shift+vertical
  pans time; Alt zooms.

## Verification (every batch)

`make && make run`: default scene (ground + 2 cubes) renders, one source +
active listener present, timeline docked full-width at the bottom, toolbar
in the viewport, transport drives sim + audio, camera still moves while
paused, no Metal validation errors in the console.

Standalone tests to add under `tests/` (not in the Makefile):
`ui_textinput_test` (caret / backspace / commit), `timeline_view_test`
(scroll + zoom coordinate transforms), `scene_outliner_test`
(`SceneEntities` ordering + rename).
