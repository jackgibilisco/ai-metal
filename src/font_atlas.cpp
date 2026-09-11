#include "font_atlas.h"

#include <CoreText/CoreText.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "third_party/argentum/argentum_sans_regular.h"

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
// of signed distance fields via CoreText, and fills `out` with per-glyph
// placement (quads carry a kSdfRange gutter so the shader can reconstruct the
// edge). Falls back to the system UI font if the embedded face fails to load.
// Returns the malloc'd R8 pixels; the caller uploads and frees them.
//
// Every cell is aligned to whole pixels: the ink box is snapped outward to
// integers before the gutter is added, so `offsetX` / `offsetY` come out exact
// integers and a snapped pen puts every glyph on the same pixel grid. The
// glyph is still drawn at its true fractional position inside the cell, so the
// sub-pixel phase survives in the distance field.
uint8_t *BakeFontAtlas(UiFontMetrics *out, int *outAtlasWidth, int *outAtlasHeight) {
    CTFontRef font = nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, argentum_sans_regular_ttf, argentum_sans_regular_ttf_len, nullptr);
    if (provider != nullptr) {
        CGFontRef cgFont = CGFontCreateWithDataProvider(provider);
        CGDataProviderRelease(provider);
        if (cgFont != nullptr) {
            font = CTFontCreateWithGraphicsFont(cgFont, kFontPixelSize, nullptr, nullptr);
            CGFontRelease(cgFont);
        }
    }
    if (font == nullptr) {
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, kFontPixelSize, nullptr);
    }

    CGFloat ascent = CTFontGetAscent(font);
    CGFloat descent = CTFontGetDescent(font);
    CGFloat leading = CTFontGetLeading(font);
    out->ascent = (float)ascent;
    out->descent = (float)descent;
    out->lineHeight = (float)ceil(ascent + descent + leading);
    out->pixelSize = kFontPixelSize;

    CGGlyph glyphs[kFontCharCount] = {};
    CGRect boundingRects[kFontCharCount] = {};
    CGSize advances[kFontCharCount] = {};
    int cellX[kFontCharCount] = {};
    int cellW[kFontCharCount] = {};
    int cellH[kFontCharCount] = {};
    int inkLeft[kFontCharCount] = {};
    int inkBottom[kFontCharCount] = {};
    int inkTop[kFontCharCount] = {};

    int pad = (int)ceil(kSdfRange);
    int atlasWidth = kGlyphPad;
    int atlasHeight = 0;
    for (int i = 0; i < kFontCharCount; ++i) {
        UniChar ch = (UniChar)(kFontFirstChar + i);
        CTFontGetGlyphsForCharacters(font, &ch, &glyphs[i], 1);
        CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyphs[i],
                                        &boundingRects[i], 1);
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyphs[i], &advances[i], 1);
        bool empty = glyphs[i] == 0 || CGRectIsNull(boundingRects[i]) ||
                     boundingRects[i].size.width <= 0.0;
        CGRect bbox = boundingRects[i];
        inkLeft[i] = (int)floor(bbox.origin.x);
        inkBottom[i] = (int)floor(bbox.origin.y);
        inkTop[i] = (int)ceil(bbox.origin.y + bbox.size.height);
        int inkRight = (int)ceil(bbox.origin.x + bbox.size.width);
        cellW[i] = empty ? 0 : (inkRight - inkLeft[i]) + 2 * pad;
        cellH[i] = empty ? 0 : (inkTop[i] - inkBottom[i]) + 2 * pad;
        cellX[i] = atlasWidth;
        atlasWidth += cellW[i] + kGlyphPad;
        if (cellH[i] > atlasHeight) {
            atlasHeight = cellH[i];
        }
    }

    int capIndex = 'H' - kFontFirstChar;
    out->capHeight = (float)boundingRects[capIndex].size.height;

    size_t bytesPerRow = (size_t)atlasWidth;
    uint8_t *pixels = (uint8_t *)calloc(bytesPerRow * atlasHeight, 1);

    int supersampledPad = pad * kSdfSupersample;
    for (int i = 0; i < kFontCharCount; ++i) {
        UiGlyphMetric &g = out->glyphs[i];
        g = {};
        g.advance = (float)advances[i].width;
        if (cellW[i] == 0) {
            continue;
        }
        int hiWidth = cellW[i] * kSdfSupersample;
        int hiHeight = cellH[i] * kSdfSupersample;

        uint8_t *coverage = (uint8_t *)calloc((size_t)hiWidth * hiHeight, 1);
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        CGContextRef ctx = CGBitmapContextCreate(coverage, hiWidth, hiHeight, 8, hiWidth, gray,
                                                 kCGImageAlphaNone);
        CGColorSpaceRelease(gray);
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetGrayFillColor(ctx, 1.0, 1.0);
        CGContextScaleCTM(ctx, kSdfSupersample, kSdfSupersample);
        CGPoint pos = CGPointMake((CGFloat)(pad - inkLeft[i]), (CGFloat)(pad - inkBottom[i]));
        CTFontDrawGlyphs(font, &glyphs[i], &pos, 1, ctx);
        CGContextRelease(ctx);

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
        // field to read. Row 0 of a CGBitmapContext is the image top, and the
        // copy above preserved that, so v0 = 0 is the top of the cell.
        g.u0 = (float)cellX[i] / (float)atlasWidth;
        g.u1 = ((float)cellX[i] + cellW[i]) / (float)atlasWidth;
        g.v0 = 0.0f;
        g.v1 = (float)cellH[i] / (float)atlasHeight;
        g.offsetX = (float)(inkLeft[i] - pad);
        g.offsetY = (float)(inkTop[i] + pad); // baseline -> top of cell, +up
        g.width = (float)cellW[i];
        g.height = (float)cellH[i];
    }
    CFRelease(font);

    *outAtlasWidth = atlasWidth;
    *outAtlasHeight = atlasHeight;
    return pixels;
}
