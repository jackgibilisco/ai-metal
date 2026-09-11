// Headless UI-layout inspector. Builds one UiBuildFrame at a fixed size,
// rebuilds rectangles from the flat vertex list, writes build/ui_inspect.png
// and build/ui_inspect.txt, then asserts no controls overlap and side-panel
// children share a left margin. Exit nonzero on any failure.
//
// Build + run:
//   clang++ -std=c++17 -Wall -Wextra -I src tools/ui_inspect.cpp \
//     src/ui.cpp src/scene.cpp src/timeline.cpp src/menu.cpp src/arena.cpp src/undo_stack.cpp \
//     -framework CoreText -framework CoreGraphics -framework CoreFoundation \
//     -o build/ui_inspect && build/ui_inspect
// Or: make ui-inspect

#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "arena.h"
#include "audio.h"
#include "menu.h"
#include "scene.h"
#include "theme.h"
#include "timeline.h"
#include "ui.h"

#include "third_party/argentum/argentum_sans_regular.h"

// ui.cpp references these only inside its wav-drop branch, which never runs here
// (UiEditorState.audio is null).
struct AudioState;
WavId AudioLoadWav(AudioState *, const char *) { return kInvalidWavId; }
double AudioWavDuration(const AudioState *, WavId) { return 0.0; }

// Must match the file-local constants in src/ui.cpp.
constexpr float kHeaderWidth = 184.0f; // kTimelineHeaderWidth
constexpr float kPanelPad = 22.0f;     // kPadding

// ---------------------------------------------------------------------------
// Font metrics: the metrics half of BuildFontAtlas (src/ui_render_metal.mm),
// via CoreText only. No Metal, no SDF raster.
// ---------------------------------------------------------------------------
static void BuildFontMetricsCoreText(UiFontMetrics *out) {
    const CGFloat size = 28.0; // kFontPixelSize
    const int pad = 4;         // ceil(kSdfRange)

    CTFontRef font = nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, argentum_sans_regular_ttf, argentum_sans_regular_ttf_len, nullptr);
    if (provider != nullptr) {
        CGFontRef cgFont = CGFontCreateWithDataProvider(provider);
        CGDataProviderRelease(provider);
        if (cgFont != nullptr) {
            font = CTFontCreateWithGraphicsFont(cgFont, size, nullptr, nullptr);
            CGFontRelease(cgFont);
        }
    }
    if (font == nullptr) {
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr);
    }

    *out = {};
    out->pixelSize = (float)size;
    out->ascent = (float)CTFontGetAscent(font);
    out->descent = (float)CTFontGetDescent(font);
    out->lineHeight =
        (float)ceil(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));

    for (int i = 0; i < 96; ++i) {
        UniChar ch = (UniChar)(32 + i);
        CGGlyph glyph = 0;
        CGRect bbox = CGRectNull;
        CGSize advance = CGSizeZero;
        CTFontGetGlyphsForCharacters(font, &ch, &glyph, 1);
        CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &bbox, 1);
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &advance, 1);

        UiGlyphMetric &g = out->glyphs[i];
        g = {};
        g.advance = (float)advance.width;
        bool empty = glyph == 0 || CGRectIsNull(bbox) || bbox.size.width <= 0.0;
        if (empty) {
            continue;
        }
        int inkLeft = (int)floor(bbox.origin.x);
        int inkBottom = (int)floor(bbox.origin.y);
        int inkRight = (int)ceil(bbox.origin.x + bbox.size.width);
        int inkTop = (int)ceil(bbox.origin.y + bbox.size.height);
        g.width = (float)((inkRight - inkLeft) + 2 * pad);
        g.height = (float)((inkTop - inkBottom) + 2 * pad);
        g.offsetX = (float)(inkLeft - pad);
        g.offsetY = (float)(inkTop + pad);
    }

    CGGlyph h = 0;
    CGRect hbox = CGRectNull;
    UniChar hc = 'H';
    CTFontGetGlyphsForCharacters(font, &hc, &h, 1);
    CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &h, &hbox, 1);
    out->capHeight = (float)hbox.size.height;

    CFRelease(font);
}

// ---------------------------------------------------------------------------
// Box reconstruction from the vertex soup
// ---------------------------------------------------------------------------
struct Box {
    float x, y, w, h;
};
struct Fill {
    Box r;
    unsigned char rgba[4];
    bool hairline;
};
struct Run {
    Box r;
    unsigned char rgba[4];
    int glyphs;
};

static Fill g_fills[8192];
static int g_fillCount;
static Run g_runs[4096];
static int g_runCount;

static bool RgbaEq(const unsigned char *p, theme::Color c) {
    return p[0] == c.r && p[1] == c.g && p[2] == c.b && p[3] == c.a;
}

static void Reconstruct(const UiVertex *v, int n) {
    g_fillCount = 0;
    g_runCount = 0;
    bool haveRun = false;
    Run cur = {};

    for (int i = 0; i + 6 <= n; i += 6) {
        float minX = v[i].x, maxX = v[i].x, minY = v[i].y, maxY = v[i].y;
        for (int k = 1; k < 6; ++k) {
            minX = fminf(minX, v[i + k].x);
            maxX = fmaxf(maxX, v[i + k].x);
            minY = fminf(minY, v[i + k].y);
            maxY = fmaxf(maxY, v[i + k].y);
        }
        Box r = {minX, minY, maxX - minX, maxY - minY};
        const unsigned char *rgba = v[i].rgba;

        // Any non-text primitive ends the current text run: two adjacent
        // TimelineHeaderToggle labels ("M", "S") are separated by the second
        // toggle's background fill, so they must not coalesce.
        if (v[i].mode < 0.5f) {
            if (haveRun && g_runCount < (int)(sizeof g_runs / sizeof *g_runs)) {
                g_runs[g_runCount++] = cur;
            }
            haveRun = false;
            if (g_fillCount < (int)(sizeof g_fills / sizeof *g_fills)) {
                Fill &f = g_fills[g_fillCount++];
                f.r = r;
                memcpy(f.rgba, rgba, 4);
                f.hairline = r.w <= 2.0f || r.h <= 2.0f;
            }
            continue;
        }

        // Glyphs of one PushText call arrive back to back with no gap wider than
        // a space; a different call on the same line (a right-aligned readout)
        // leaves a much larger gap.
        bool sameColor = haveRun && memcmp(cur.rgba, rgba, 4) == 0;
        bool sameLine = sameColor && fabsf(r.y - cur.r.y) < 3.0f;
        bool adjacent = sameLine && r.x >= cur.r.x - 1.0f &&
                        r.x - (cur.r.x + cur.r.w) < 6.0f;

        if (haveRun && adjacent) {
            float x2 = fmaxf(cur.r.x + cur.r.w, r.x + r.w);
            float y1 = fminf(cur.r.y, r.y);
            float y2 = fmaxf(cur.r.y + cur.r.h, r.y + r.h);
            cur.r.x = fminf(cur.r.x, r.x);
            cur.r.w = x2 - cur.r.x;
            cur.r.y = y1;
            cur.r.h = y2 - y1;
            cur.glyphs++;
        } else {
            if (haveRun && g_runCount < (int)(sizeof g_runs / sizeof *g_runs)) {
                g_runs[g_runCount++] = cur;
            }
            cur = {};
            cur.r = r;
            memcpy(cur.rgba, rgba, 4);
            cur.glyphs = 1;
            haveRun = true;
        }
    }
    if (haveRun && g_runCount < (int)(sizeof g_runs / sizeof *g_runs)) {
        g_runs[g_runCount++] = cur;
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static float Area(Box r) { return r.w * r.h; }

static float OverlapArea(Box a, Box b) {
    float x = fminf(a.x + a.w, b.x + b.w) - fmaxf(a.x, b.x);
    float y = fminf(a.y + a.h, b.y + b.h) - fmaxf(a.y, b.y);
    if (x <= 0.0f || y <= 0.0f) {
        return 0.0f;
    }
    return x * y;
}

static bool Contains(Box a, Box b, float eps) {
    return a.x - eps <= b.x && a.y - eps <= b.y && a.x + a.w + eps >= b.x + b.w &&
           a.y + a.h + eps >= b.y + b.h && Area(a) > Area(b);
}

static bool RectInside(Box outer, Box inner, float eps) {
    return inner.x >= outer.x - eps && inner.y >= outer.y - eps &&
           inner.x + inner.w <= outer.x + outer.w + eps &&
           inner.y + inner.h <= outer.y + outer.h + eps;
}

static bool CenterIn(Box outer, Box inner) {
    float cx = inner.x + inner.w * 0.5f;
    float cy = inner.y + inner.h * 0.5f;
    return cx >= outer.x && cx <= outer.x + outer.w && cy >= outer.y && cy <= outer.y + outer.h;
}

// ---------------------------------------------------------------------------
// Minimal PNG writer: 8-bit RGB, stored (uncompressed) zlib.
// ---------------------------------------------------------------------------
static uint32_t g_crc[256];

static void CrcInit() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        g_crc[i] = c;
    }
}

static uint32_t Crc32(const unsigned char *p, size_t n, uint32_t c) {
    for (size_t i = 0; i < n; ++i) {
        c = g_crc[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

static uint32_t Adler32(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void PutBE32(FILE *f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 8), (unsigned char)v};
    fwrite(b, 1, 4, f);
}

static void Chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
    PutBE32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n != 0) {
        fwrite(data, 1, n, f);
    }
    uint32_t c = 0xFFFFFFFFu;
    c = Crc32((const unsigned char *)type, 4, c);
    if (n != 0) {
        c = Crc32(data, n, c);
    }
    PutBE32(f, c ^ 0xFFFFFFFFu);
}

static void WritePng(const char *path, const unsigned char *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "ui-inspect: cannot write %s\n", path);
        return;
    }
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[13] = {
        (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8),
        (unsigned char)w,         (unsigned char)(h >> 24), (unsigned char)(h >> 16),
        (unsigned char)(h >> 8),  (unsigned char)h,         8,
        2,                        0,                        0,
        0};
    Chunk(f, "IHDR", ihdr, 13);

    size_t rowBytes = 1 + (size_t)w * 3;
    size_t rawLen = rowBytes * h;
    unsigned char *raw = (unsigned char *)malloc(rawLen);
    for (int y = 0; y < h; ++y) {
        unsigned char *row = raw + (size_t)y * rowBytes;
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }

    size_t cap = 2 + rawLen + (rawLen / 65535 + 1) * 5 + 4;
    unsigned char *z = (unsigned char *)malloc(cap);
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    for (size_t off = 0; off < rawLen;) {
        size_t block = rawLen - off;
        if (block > 65535) {
            block = 65535;
        }
        int final = (off + block >= rawLen) ? 1 : 0;
        z[zn++] = (unsigned char)final;
        z[zn++] = (unsigned char)(block & 0xFF);
        z[zn++] = (unsigned char)(block >> 8);
        z[zn++] = (unsigned char)(~block & 0xFF);
        z[zn++] = (unsigned char)((~block >> 8) & 0xFF);
        memcpy(z + zn, raw + off, block);
        zn += block;
        off += block;
    }
    uint32_t adler = Adler32(raw, rawLen);
    z[zn++] = (unsigned char)(adler >> 24);
    z[zn++] = (unsigned char)(adler >> 16);
    z[zn++] = (unsigned char)(adler >> 8);
    z[zn++] = (unsigned char)adler;

    Chunk(f, "IDAT", z, zn);
    Chunk(f, "IEND", nullptr, 0);
    free(z);
    free(raw);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Raster
// ---------------------------------------------------------------------------
static void Blend(unsigned char *img, int w, int h, Box r, const unsigned char *c, float a) {
    int x0 = (int)floorf(r.x), y0 = (int)floorf(r.y);
    int x1 = (int)ceilf(r.x + r.w), y1 = (int)ceilf(r.y + r.h);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > w ? w : x1;
    y1 = y1 > h ? h : y1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            unsigned char *p = img + ((size_t)y * w + x) * 3;
            for (int k = 0; k < 3; ++k) {
                p[k] = (unsigned char)(p[k] * (1.0f - a) + c[k] * a);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Labels for the text dump
// ---------------------------------------------------------------------------
static const char *RgbaLabel(const unsigned char *p) {
    struct Named {
        theme::Color c;
        const char *name;
    };
    static const Named table[] = {
        {theme::PanelBg, "panel-bg"},          {theme::PanelBorder, "border"},
        {theme::TitleBar, "title/lane-header"}, {theme::Button, "button"},
        {theme::ButtonHot, "button-hot"},       {theme::ButtonActive, "button-active"},
        {theme::SliderTrack, "slider-track"},   {theme::SliderHandle, "slider-handle"},
        {theme::SliderHandleHot, "slider-hot"}, {theme::Text, "text"},
        {theme::TextDisabled, "text-disabled"}, {theme::TextShortcut, "text-shortcut"},
        {theme::TimelineRuler, "ruler"},        {theme::TimelineLaneEven, "lane-even"},
        {theme::TimelineLaneOdd, "lane-odd"},   {theme::TimelineClip, "clip"},
        {theme::MenuBarBg, "menu-strip"},       {theme::Playhead, "playhead"},
        {theme::ToolbarBg, "toolbar-bg"},
    };
    for (const Named &n : table) {
        if (RgbaEq(p, n.c)) {
            return n.name;
        }
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------
static int g_failures;

#define FAILR(cat, ...)                                                                             \
    do {                                                                                            \
        printf("FAIL %-24s ", cat);                                                                 \
        printf(__VA_ARGS__);                                                                        \
        printf("\n");                                                                               \
        ++g_failures;                                                                               \
    } while (0)

static const char *RS(Box r) {
    static char bufs[4][64];
    static int turn;
    char *buf = bufs[turn++ & 3];
    snprintf(buf, 64, "{%.0f,%.0f %.0fx%.0f}", r.x, r.y, r.w, r.h);
    return buf;
}

// A: inside each timeline track header, the name text and the row-2 controls
// (volume track, mute bg, solo bg) must not overlap each other. The slider
// handle legitimately sits on its track, so it is excluded.
static void AssertHeaderNoOverlap(Box timeline, float rulerBottom) {
    (void)timeline;
    for (int i = 0; i < g_fillCount; ++i) {
        Fill &H = g_fills[i];
        // A track header: lane-header colour, ~kHeaderWidth wide, below the ruler
        // (the panel title bars share the colour but span the whole panel).
        if (!RgbaEq(H.rgba, theme::TimelineLaneHeader) || H.hairline) {
            continue;
        }
        if (fabsf(H.r.w - kHeaderWidth) > 4.0f || H.r.y < rulerBottom - 1.0f) {
            continue;
        }

        // Row-2 control fills (volume track, mute bg, solo bg). The slider
        // handle sits on its track by design.
        Box controls[6];
        int nc = 0;
        for (int j = 0; j < g_fillCount && nc < 6; ++j) {
            Fill &c = g_fills[j];
            if (j == i || c.hairline || !Contains(H.r, c.r, 1.0f)) {
                continue;
            }
            if (RgbaEq(c.rgba, theme::SliderHandle) || RgbaEq(c.rgba, theme::SliderHandleHot)) {
                continue;
            }
            controls[nc++] = c.r;
        }
        for (int a = 0; a < nc; ++a) {
            for (int b = a + 1; b < nc; ++b) {
                if (OverlapArea(controls[a], controls[b]) > 0.5f) {
                    FAILR("timeline-header-overlap", "controls %s vs %s in header %s",
                          RS(controls[a]), RS(controls[b]), RS(H.r));
                }
            }
        }

        // The name row (upper half of the header) must clear the control row.
        float mid = H.r.y + H.r.h * 0.5f;
        for (int j = 0; j < g_runCount; ++j) {
            Box run = g_runs[j].r;
            if (!CenterIn(H.r, run) || run.y + run.h * 0.5f >= mid) {
                continue;
            }
            for (int a = 0; a < nc; ++a) {
                if (OverlapArea(run, controls[a]) > 0.5f) {
                    FAILR("timeline-header-name-overlap", "name %s over control %s in header %s",
                          RS(run), RS(controls[a]), RS(H.r));
                }
            }
        }
    }
}

// B: every text run outside the timeline header column sits within its nearest
// enclosing control box (button / field), not spilling it.
static void AssertTextFits(Box panels[3], Box timeline, float rulerBottom) {
    for (int i = 0; i < g_runCount; ++i) {
        Box run = g_runs[i].r;
        bool inHeaderCol = run.x >= timeline.x - 1.0f &&
                           run.x <= timeline.x + kHeaderWidth + 2.0f && run.y >= rulerBottom - 1.0f;
        if (inHeaderCol) {
            continue;
        }

        Box enclosingPanel = {};
        bool havePanel = false;
        for (int p = 0; p < 3; ++p) {
            if (CenterIn(panels[p], run)) {
                enclosingPanel = panels[p];
                havePanel = true;
            }
        }

        Box best = {};
        bool have = false;
        for (int j = 0; j < g_fillCount; ++j) {
            Fill &c = g_fills[j];
            if (c.hairline || RgbaEq(c.rgba, theme::PanelBg)) {
                continue;
            }
            if (havePanel && c.r.w >= enclosingPanel.w - 2.0f) {
                continue; // full-panel-width chrome (title bar)
            }
            if (!Contains(c.r, run, 1.0f)) {
                continue;
            }
            if (!have || Area(c.r) < Area(best)) {
                best = c.r;
                have = true;
            }
        }
        if (!have) {
            continue;
        }
        if (!RectInside(best, run, 1.5f)) {
            FAILR("text-spill", "text %s spills box %s", RS(run), RS(best));
        }
    }
}

// C: timeline lane rows (and lane headers) tile without vertical overlap.
static void AssertLanesTile() {
    for (int pass = 0; pass < 2; ++pass) {
        Box rows[64];
        int nr = 0;
        for (int i = 0; i < g_fillCount && nr < 64; ++i) {
            Fill &f = g_fills[i];
            bool match = pass == 0 ? (RgbaEq(f.rgba, theme::TimelineLaneEven) ||
                                      RgbaEq(f.rgba, theme::TimelineLaneOdd))
                                   : RgbaEq(f.rgba, theme::TimelineLaneHeader);
            if (match && !f.hairline) {
                rows[nr++] = f.r;
            }
        }
        for (int a = 0; a < nr; ++a) {
            for (int b = 0; b < nr; ++b) {
                if (a == b) {
                    continue;
                }
                if (rows[a].y < rows[b].y && rows[a].y + rows[a].h > rows[b].y + 1.0f &&
                    fabsf(rows[a].x - rows[b].x) < 2.0f) {
                    FAILR("lane-row-overlap", "%s over %s", RS(rows[a]), RS(rows[b]));
                }
            }
        }
    }
}

// D: in the two side panels, every top-level child fill starts at panel.x +
// kPanelPad.
static void AssertLeftMargin(Box panels[3], int objectsIdx, int sceneIdx) {
    int idxs[2] = {objectsIdx, sceneIdx};
    for (int t = 0; t < 2; ++t) {
        Box p = panels[idxs[t]];
        float expected = p.x + kPanelPad;
        for (int i = 0; i < g_fillCount; ++i) {
            Fill &c = g_fills[i];
            if (c.hairline || RgbaEq(c.rgba, theme::PanelBg) || RgbaEq(c.rgba, theme::TitleBar) ||
                RgbaEq(c.rgba, theme::ResizeGrip) || RgbaEq(c.rgba, theme::ResizeGripHot)) {
                continue; // panel chrome, not content
            }
            if (!CenterIn(p, c.r) || c.r.w >= p.w - 2.0f) {
                continue;
            }
            bool nested = false;
            for (int j = 0; j < g_fillCount; ++j) {
                if (j == i || g_fills[j].hairline || RgbaEq(g_fills[j].rgba, theme::PanelBg)) {
                    continue;
                }
                if (Contains(g_fills[j].r, c.r, 1.0f)) {
                    nested = true;
                    break;
                }
            }
            if (nested) {
                continue;
            }
            if (fabsf(c.r.x - expected) > 1.5f) {
                FAILR("left-margin", "child %s x=%.0f expected %.0f (panel %s)", RS(c.r), c.r.x,
                      expected, RS(p));
            }
        }
    }
}

// ---------------------------------------------------------------------------
static void WriteTxt(const char *path, Box panels[3], const char *names[3]) {
    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        return;
    }
    for (int p = 0; p < 3; ++p) {
        fprintf(f, "PANEL %-10s %s\n", names[p], RS(panels[p]));
        for (int i = 0; i < g_fillCount; ++i) {
            if (!g_fills[i].hairline && CenterIn(panels[p], g_fills[i].r)) {
                fprintf(f, "  fill %-22s %s\n", RS(g_fills[i].r), RgbaLabel(g_fills[i].rgba));
            }
        }
        for (int i = 0; i < g_runCount; ++i) {
            if (CenterIn(panels[p], g_runs[i].r)) {
                fprintf(f, "  text %-22s glyphs=%d\n", RS(g_runs[i].r), g_runs[i].glyphs);
            }
        }
    }
    fprintf(f, "OTHER\n");
    for (int i = 0; i < g_fillCount; ++i) {
        Fill &c = g_fills[i];
        if (c.hairline) {
            continue;
        }
        bool inPanel = false;
        for (int p = 0; p < 3; ++p) {
            inPanel = inPanel || CenterIn(panels[p], c.r);
        }
        if (!inPanel) {
            fprintf(f, "  fill %-22s %s\n", RS(c.r), RgbaLabel(c.rgba));
        }
    }
    fclose(f);
}

int main() {
    CrcInit();

    size_t bytes = 128u << 20;
    Arena arena = ArenaCreate(malloc(bytes), bytes);

    SceneState *scene = SceneInit(&arena);
    TimelineState *timeline = TimelineInit(&arena);
    for (int i = 0; i < 4; ++i) {
        char name[16];
        snprintf(name, sizeof name, "Source %d", i + 1);
        SceneCreateEntity(scene, EntityKind_AudioSource, name);
    }
    SceneCreateMeshEntityAt(scene, "Cube", MeshId_Cube, TransformIdentity());
    SceneCreateMeshEntityAt(scene, "Plane", MeshId_Plane, TransformIdentity());

    const int w = 1440;
    const int h = 900;
    UiState *ui = UiInit(&arena, (float)w, (float)h);
    static UiFontMetrics fm;
    BuildFontMetricsCoreText(&fm);
    UiSetFontMetrics(ui, &fm);

    UiEditorState ed = {};
    ed.timeline = timeline;
    ed.scene = scene;
    ed.audio = nullptr;
    UiSetEditorState(ui, ed);

    MenuState menu = {};
    menu.showMenuBar = true;
    EditorFlags flags = {};
    CommandContext ctx = {};
    ctx.menuState = &menu;
    ctx.flags = &flags;

    FrameInput in = {};
    in.mouseX = -1.0f;
    in.mouseY = -1.0f;

    // Panels are created lazily inside the first UiBuildFrame, after that
    // frame's layout pass, so they have zero-size rects and draw nothing until
    // the next frame. Build twice; inspect the second.
    UiBuildFrame(ui, in, ctx);
    UiBuildFrame(ui, in, ctx);
    Reconstruct(UiVertices(ui), UiVertexCount(ui));
    printf("ui-inspect: %d vertices, %d fills, %d text runs\n", UiVertexCount(ui), g_fillCount,
           g_runCount);

    // Panels by geometry: timeline spans the full width; of the other two the
    // right-docked one has the larger x.
    int panelFills[8];
    int np = 0;
    for (int i = 0; i < g_fillCount && np < 8; ++i) {
        if (RgbaEq(g_fills[i].rgba, theme::PanelBg)) {
            panelFills[np++] = i;
        }
    }
    if (np != 3) {
        printf("FAIL panels: expected 3 PanelBg rects, found %d\n", np);
        return 1;
    }
    int timelineIdx = panelFills[0];
    for (int k = 1; k < 3; ++k) {
        if (g_fills[panelFills[k]].r.w > g_fills[timelineIdx].r.w) {
            timelineIdx = panelFills[k];
        }
    }
    int sideA = -1, sideB = -1;
    for (int k = 0; k < 3; ++k) {
        if (panelFills[k] == timelineIdx) {
            continue;
        }
        (sideA < 0 ? sideA : sideB) = panelFills[k];
    }
    int objectsIdx = g_fills[sideA].r.x > g_fills[sideB].r.x ? sideA : sideB;
    int sceneIdx = objectsIdx == sideA ? sideB : sideA;

    Box panels[3] = {g_fills[objectsIdx].r, g_fills[sceneIdx].r, g_fills[timelineIdx].r};
    const char *names[3] = {"Objects", "Scene", "Timeline"};

    float rulerBottom = panels[2].y;
    for (int i = 0; i < g_fillCount; ++i) {
        if (RgbaEq(g_fills[i].rgba, theme::TimelineRuler)) {
            rulerBottom = g_fills[i].r.y + g_fills[i].r.h;
        }
    }

    unsigned char *img = (unsigned char *)malloc((size_t)w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        img[i * 3 + 0] = theme::ViewportBackground.r;
        img[i * 3 + 1] = theme::ViewportBackground.g;
        img[i * 3 + 2] = theme::ViewportBackground.b;
    }
    for (int i = 0; i < g_fillCount; ++i) {
        Blend(img, w, h, g_fills[i].r, g_fills[i].rgba, g_fills[i].rgba[3] / 255.0f);
    }
    for (int i = 0; i < g_runCount; ++i) {
        Blend(img, w, h, g_runs[i].r, g_runs[i].rgba, 0.55f);
    }
    WritePng("build/ui_inspect.png", img, w, h);
    free(img);

    WriteTxt("build/ui_inspect.txt", panels, names);

    AssertHeaderNoOverlap(panels[2], rulerBottom);
    AssertTextFits(panels, panels[2], rulerBottom);
    AssertLanesTile();
    AssertLeftMargin(panels, 0, 1);

    printf("ui-inspect: %d failure(s); wrote build/ui_inspect.png build/ui_inspect.txt\n",
           g_failures);
    return g_failures != 0 ? 1 : 0;
}
