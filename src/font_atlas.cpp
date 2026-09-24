#include "font_atlas.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "third_party/argentum/argentum_sans_regular.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
// stb_truetype offers far more than this bake uses; its unused half is not a
// finding worth reporting on every build.
#ifdef _MSC_VER
#pragma warning(disable : 4505)
#else
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_truetype.h"

namespace {

// Felzenszwalb & Huttenlocher exact squared Euclidean distance transform,
// one row (or column) of `n` samples. `seed[q]` is 0 where the feature is
// present and a large value elsewhere; `out[q]` receives the squared
// distance to the nearest feature. `hull`/`edge` are scratch of size n / n+1.
void SquaredDistanceTransform1d(const float *seed, float *out, int n, int *hull, float *edge) {
    int k = 0;
    hull[0] = 0;
    edge[0] = -1e20f;
    edge[1] = 1e20f;
    for (int q = 1; q < n; ++q) {
        float intersection =
            ((seed[q] + q * q) - (seed[hull[k]] + hull[k] * hull[k])) / (2 * q - 2 * hull[k]);
        while (intersection <= edge[k]) {
            --k;
            intersection =
                ((seed[q] + q * q) - (seed[hull[k]] + hull[k] * hull[k])) / (2 * q - 2 * hull[k]);
        }
        ++k;
        hull[k] = q;
        edge[k] = intersection;
        edge[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (edge[k + 1] < q) {
            ++k;
        }
        int delta = q - hull[k];
        out[q] = (float)(delta * delta) + seed[hull[k]];
    }
}

void SquaredDistanceTransform2d(float *grid, int width, int height) {
    int longest = width > height ? width : height;
    float *column = (float *)malloc(sizeof(float) * longest);
    float *result = (float *)malloc(sizeof(float) * longest);
    int *hull = (int *)malloc(sizeof(int) * longest);
    float *edge = (float *)malloc(sizeof(float) * (longest + 1));
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            column[y] = grid[y * width + x];
        }
        SquaredDistanceTransform1d(column, result, height, hull, edge);
        for (int y = 0; y < height; ++y) {
            grid[y * width + x] = result[y];
        }
    }
    for (int y = 0; y < height; ++y) {
        SquaredDistanceTransform1d(&grid[y * width], result, width, hull, edge);
        memcpy(&grid[y * width], result, sizeof(float) * width);
    }
    free(column);
    free(result);
    free(hull);
    free(edge);
}

// Turns an 8-bit coverage buffer into a normalized signed distance field:
// 0.5 on the glyph outline, rising inward, falling outward, clamped so that
// `range` texels map to the full 0..1 span.
void CoverageToSignedField(const uint8_t *coverage, int width, int height, float range,
                           uint8_t *out) {
    int count = width * height;
    float *distToInk = (float *)malloc(sizeof(float) * count);
    float *distToVoid = (float *)malloc(sizeof(float) * count);
    for (int i = 0; i < count; ++i) {
        bool solid = coverage[i] >= 128;
        distToInk[i] = solid ? 0.0f : 1e20f;
        distToVoid[i] = solid ? 1e20f : 0.0f;
    }
    SquaredDistanceTransform2d(distToInk, width, height);
    SquaredDistanceTransform2d(distToVoid, width, height);
    for (int i = 0; i < count; ++i) {
        float inside = sqrtf(distToVoid[i]);
        float outside = sqrtf(distToInk[i]);
        float normalized = 0.5f + (inside - outside) / (2.0f * range);
        normalized = normalized < 0.0f ? 0.0f : (normalized > 1.0f ? 1.0f : normalized);
        out[i] = (uint8_t)(normalized * 255.0f + 0.5f);
    }
    free(distToInk);
    free(distToVoid);
}

} // namespace

// Rasterizes ASCII 32..127 of Argentum Sans (embedded) into a one-row R8 atlas
// of signed distance fields, and fills `out` with per-glyph placement (quads
// carry a kSdfRange gutter so the shader can reconstruct the edge). Returns the
// malloc'd R8 pixels; the caller uploads and frees them.
//
// Every cell is aligned to whole pixels: the ink box is snapped outward to
// integers before the gutter is added, so `offsetX` / `offsetY` come out exact
// integers and a snapped pen puts every glyph on the same pixel grid. The
// glyph is still rasterized at the supersampled scale, so the sub-pixel phase
// of its outline survives in the distance field.
uint8_t *BakeFontAtlas(UiFontMetrics *out, int *outAtlasWidth, int *outAtlasHeight) {
    stbtt_fontinfo font;
    stbtt_InitFont(&font, argentum_sans_regular_ttf, 0);

    // CoreText and DirectWrite both treat a font size as the em size, so map
    // the em square to kFontPixelSize rather than the ascent-to-descent span.
    float scale = stbtt_ScaleForMappingEmToPixels(&font, kFontPixelSize);
    float hiScale = scale * kSdfSupersample;

    int rawAscent = 0;
    int rawDescent = 0;
    int rawLineGap = 0;
    stbtt_GetFontVMetrics(&font, &rawAscent, &rawDescent, &rawLineGap);
    out->ascent = (float)rawAscent * scale;
    out->descent = (float)-rawDescent * scale;
    out->lineHeight = ceilf(out->ascent + out->descent + (float)rawLineGap * scale);
    out->pixelSize = kFontPixelSize;

    int capX0 = 0, capY0 = 0, capX1 = 0, capY1 = 0;
    stbtt_GetCodepointBox(&font, 'H', &capX0, &capY0, &capX1, &capY1);
    out->capHeight = (float)(capY1 - capY0) * scale;

    int cellX[kFontCharCount] = {};
    int cellW[kFontCharCount] = {};
    int cellH[kFontCharCount] = {};
    int inkLeft[kFontCharCount] = {};
    int inkTop[kFontCharCount] = {};
    float advance[kFontCharCount] = {};

    int pad = (int)ceilf(kSdfRange);
    int atlasWidth = kGlyphPad;
    int atlasHeight = 0;
    for (int i = 0; i < kFontCharCount; ++i) {
        int codepoint = kFontFirstChar + i;
        int rawAdvance = 0;
        int leftSideBearing = 0;
        stbtt_GetCodepointHMetrics(&font, codepoint, &rawAdvance, &leftSideBearing);
        advance[i] = (float)rawAdvance * scale;

        // Box in screen space: y grows down, both edges relative to the pen.
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&font, codepoint, scale, scale, &x0, &y0, &x1, &y1);
        bool empty = stbtt_FindGlyphIndex(&font, codepoint) == 0 || x1 <= x0 || y1 <= y0;
        inkLeft[i] = x0;
        inkTop[i] = -y0;
        cellW[i] = empty ? 0 : (x1 - x0) + 2 * pad;
        cellH[i] = empty ? 0 : (y1 - y0) + 2 * pad;
        cellX[i] = atlasWidth;
        atlasWidth += cellW[i] + kGlyphPad;
        if (cellH[i] > atlasHeight) {
            atlasHeight = cellH[i];
        }
    }

    size_t bytesPerRow = (size_t)atlasWidth;
    uint8_t *pixels = (uint8_t *)calloc(bytesPerRow * atlasHeight, 1);

    int supersampledPad = pad * kSdfSupersample;
    for (int i = 0; i < kFontCharCount; ++i) {
        UiGlyphMetric &g = out->glyphs[i];
        g = {};
        g.advance = advance[i];
        if (cellW[i] == 0) {
            continue;
        }
        int codepoint = kFontFirstChar + i;
        int hiWidth = cellW[i] * kSdfSupersample;
        int hiHeight = cellH[i] * kSdfSupersample;

        // The pen sits `pad` cell-pixels in from the cell's top-left corner, so
        // the supersampled box lands inside the gutter on every side.
        int penX = (pad - inkLeft[i]) * kSdfSupersample;
        int penY = (pad + inkTop[i]) * kSdfSupersample;
        int hiX0 = 0, hiY0 = 0, hiX1 = 0, hiY1 = 0;
        stbtt_GetCodepointBitmapBox(&font, codepoint, hiScale, hiScale, &hiX0, &hiY0, &hiX1, &hiY1);

        uint8_t *coverage = (uint8_t *)calloc((size_t)hiWidth * hiHeight, 1);
        uint8_t *glyphOrigin = coverage + (penY + hiY0) * hiWidth + (penX + hiX0);
        stbtt_MakeCodepointBitmap(&font, glyphOrigin, hiX1 - hiX0, hiY1 - hiY0, hiWidth, hiScale,
                                  hiScale, codepoint);

        uint8_t *hiField = (uint8_t *)malloc((size_t)hiWidth * hiHeight);
        CoverageToSignedField(coverage, hiWidth, hiHeight, (float)supersampledPad, hiField);
        free(coverage);

        for (int cy = 0; cy < cellH[i]; ++cy) {
            for (int cx = 0; cx < cellW[i]; ++cx) {
                int sum = 0;
                for (int sy = 0; sy < kSdfSupersample; ++sy) {
                    for (int sx = 0; sx < kSdfSupersample; ++sx) {
                        int hx = cx * kSdfSupersample + sx;
                        int hy = cy * kSdfSupersample + sy;
                        sum += hiField[hy * hiWidth + hx];
                    }
                }
                pixels[cy * bytesPerRow + cellX[i] + cx] =
                    (uint8_t)(sum / (kSdfSupersample * kSdfSupersample));
            }
        }
        free(hiField);

        // The atlas cell is the pixel-aligned ink box grown by `pad` on every
        // side; the quad carries that gutter so the shader's smoothstep has
        // field to read. Row 0 of the coverage buffer is the cell top, so
        // v0 = 0 is the top of the cell.
        g.u0 = (float)cellX[i] / (float)atlasWidth;
        g.u1 = ((float)cellX[i] + cellW[i]) / (float)atlasWidth;
        g.v0 = 0.0f;
        g.v1 = (float)cellH[i] / (float)atlasHeight;
        g.offsetX = (float)(inkLeft[i] - pad);
        g.offsetY = (float)(inkTop[i] + pad); // baseline -> top of cell, +up
        g.width = (float)cellW[i];
        g.height = (float)cellH[i];
    }

    *outAtlasWidth = atlasWidth;
    *outAtlasHeight = atlasHeight;
    return pixels;
}
