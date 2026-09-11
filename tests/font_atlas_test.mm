// Headless test for the SDF glyph atlas bake. It guards the invariant whose
// violation made text render at ragged per-character heights: the gutter
// between a quad's top edge and the glyph's first inked row must be the same
// for every glyph, so that one snapped baseline puts all of them on one line.
//
// Includes the backend source directly because BakeFontAtlas lives in an
// anonymous namespace; no MTLDevice is ever created.
//
// Build + run:
//   clang++ -std=c++17 -fobjc-arc -Wall -Wextra -I src tests/font_atlas_test.mm \
//     -framework Metal -framework QuartzCore -framework CoreText \
//     -framework CoreGraphics -framework Foundation \
//     -o build/font_atlas_test && build/font_atlas_test

#include <cmath>
#include <cstdio>

#include "../src/ui_render_metal.mm"

static int g_failures = 0;

static void Check(bool condition, const char *label) {
    if (!condition) {
        printf("FAIL: %s\n", label);
        g_failures++;
    }
}

// Row within the glyph's cell holding the topmost texel that reads as inside
// the outline (the SDF stores 0.5 on the edge, so >= 128 is ink).
static int FirstInkedRow(const uint8_t *pixels, int atlasWidth, int cellX, int cellW, int cellH) {
    for (int row = 0; row < cellH; ++row) {
        for (int col = 0; col < cellW; ++col) {
            if (pixels[row * atlasWidth + cellX + col] >= 128) {
                return row;
            }
        }
    }
    return -1;
}

int main() {
    UiFontMetrics metrics = {};
    int atlasWidth = 0;
    int atlasHeight = 0;
    uint8_t *pixels = BakeFontAtlas(&metrics, &atlasWidth, &atlasHeight);

    Check(atlasWidth > 0 && atlasHeight > 0, "atlas has a size");
    Check(metrics.pixelSize == kFontPixelSize, "pixelSize reports the bake size");
    Check(metrics.capHeight > 0.0f, "capHeight was measured");

    int pad = (int)ceil(kSdfRange);

    // The cell is packed left to right in bake order, so walking the same order
    // recovers each glyph's column without storing it in UiFontMetrics.
    int cellX = kGlyphPad;
    int referenceGutter = -1;
    char referenceChar = 0;

    for (int i = 0; i < kFontCharCount; ++i) {
        const UiGlyphMetric &g = metrics.glyphs[i];
        char label[64];
        char ch = (char)(kFontFirstChar + i);

        if (g.width <= 0.0f) {
            snprintf(label, sizeof(label), "whitespace '%c' still advances the pen", ch);
            Check(g.advance >= 0.0f, label);
            cellX += kGlyphPad;
            continue;
        }

        snprintf(label, sizeof(label), "'%c' offsetX is a whole pixel", ch);
        Check(floorf(g.offsetX) == g.offsetX, label);
        snprintf(label, sizeof(label), "'%c' offsetY is a whole pixel", ch);
        Check(floorf(g.offsetY) == g.offsetY, label);

        int cellW = (int)g.width;
        int cellH = (int)g.height;
        int inkedRow = FirstInkedRow(pixels, atlasWidth, cellX, cellW, cellH);
        snprintf(label, sizeof(label), "'%c' has ink in its cell", ch);
        Check(inkedRow >= 0, label);

        if (inkedRow >= 0) {
            // The ink box was snapped outward before the gutter was added, so
            // the first inked row sits `pad` or (for a glyph whose top rounded
            // up) `pad + 1` below the cell top -- never further.
            snprintf(label, sizeof(label), "'%c' top gutter is pad, not %d", ch, inkedRow);
            Check(inkedRow >= pad && inkedRow <= pad + 1, label);

            // The property that actually broke: quad top to ink top must not
            // drift from glyph to glyph, or a shared baseline scatters them.
            if (referenceGutter < 0) {
                referenceGutter = inkedRow;
                referenceChar = ch;
            }
            snprintf(label, sizeof(label), "'%c' gutter %d matches '%c' gutter %d", ch, inkedRow,
                     referenceChar, referenceGutter);
            Check(abs(inkedRow - referenceGutter) <= 1, label);
        }

        cellX += cellW + kGlyphPad;
    }

    // Cells must tile the atlas exactly, or the column walk above was wrong and
    // every gutter measurement read the neighbouring glyph.
    Check(cellX == atlasWidth, "cells tile the atlas width exactly");

    free(pixels);

    if (g_failures == 0) {
        printf("font_atlas_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
