# SDF text rendering

## Problem

`BuildFontAtlas` bakes Argentum Sans at a fixed 20 logical px into an R8
coverage atlas. The UI then draws it at `kTextScale` (2.0) times `uiScale`
(up to 2.5) = up to 5x magnification, so the linear sampler just blurs the
20px bitmap. Text looks soft at native scale and mushy when zoomed.

## Approach

Single-channel signed distance field, reconstructed in the fragment shader
with a screen-space `fwidth` / `smoothstep` edge. Bake once at startup, no
new dependency, no atlas rebuild on zoom. Corners round off only at extreme
magnification, which the UI never reaches.

All text is 2D UI (panels, toolbar, timeline, menus); no world-space labels,
so the change is contained to `src/ui_render_metal.mm` plus the atlas-cell
padding baked into `UiFontMetrics`. `src/ui.cpp` (`PushText`, `TextWidth`,
layout) needs no change: the glyph quad is inflated and its uv widened
inside the atlas builder, so callers keep reading `width/height/offset/uv`
as before.

## Constants (ui_render_metal.mm)

| name | value | meaning |
|------|-------|---------|
| `kFontPixelSize` | 20.0 | unchanged: logical px the metrics describe |
| `kSdfSupersample` | 4 | CoreText raster scale before the distance transform |
| `kSdfRange` | 4.0 | logical px the signed distance spans each side of the edge |

`kSdfRange` sets both the atlas padding and the shader's distance decode.
4 px = crisp up to ~8x before the field clips.

## Steps

### 1. High-res coverage raster
- Per glyph, rasterize the CoreText glyph into its own tight CG bitmap at
  `kFontPixelSize * kSdfSupersample`, with `kSdfRange * kSdfSupersample` px
  of transparent margin on every side.
- Keep the existing glyph enumeration (ASCII 32..127), advances, and
  bounding rects; only the raster target changes from one shared row to a
  per-glyph scratch buffer.

### 2. Euclidean distance transform
- Add `namespace`-local `EuclideanDistanceTransform(const float *mask,
  int w, int h, float *out)` — Felzenszwalb two-pass 1D EDT (squared),
  ~35 lines. Runs on the scratch buffer, once per glyph at startup.
- Threshold coverage at 0.5 to get inside/outside. Run the EDT on the mask
  and its complement; `signed = outsideDist - insideDist` in hi-res texels.
- Encode to R8: `v = clamp(0.5 + signed / (2 * kSdfRange * kSdfSupersample),
  0, 1)`. 0.5 is the edge.

### 3. Downsample into the atlas
- Box-average `kSdfSupersample x kSdfSupersample` blocks of the encoded
  hi-res field into the glyph's cell in the shared R8 atlas.
- Atlas cell width/height = glyph box (logical px) + `2 * kSdfRange`, in
  atlas texels. Pack cells left to right as today, `kGlyphPad` gutter.

### 4. Bake padding into metrics
- `g.offsetX -= kSdfRange; g.offsetY += kSdfRange;`
- `g.width += 2 * kSdfRange; g.height += 2 * kSdfRange;`
- uv rect covers the full padded cell.
- Whitespace (width 0) stays width 0, no quad, as now.

### 5. Shader (`ui_fragment`, `mode > 0.5` branch)
```
float sd = fontAtlas.sample(glyphSampler, in.uv).r;
float w  = fwidth(sd);
color.a *= smoothstep(0.5 - w, 0.5 + w, sd);
```
Sampler switches to `filter::linear` (already is). No new uniforms.

## Verification

```
make run
```
- Default scene + panels render, no Metal validation error in console.
- Panel / toolbar / timeline text is crisp at native scale.
- Cmd-+ to max UI scale (2.5x): glyph edges stay sharp, no blur, no
  visible atlas-cell clipping on descenders/round caps.
