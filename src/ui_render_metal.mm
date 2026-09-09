#include "ui_render.h"

#include "gpu_metal.h"

#import <CoreText/CoreText.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "third_party/argentum/argentum_sans_regular.h"

namespace {

const char *kUiShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct UiVertexIn {
    float2 position [[attribute(0)]];
    uchar4 color [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float mode [[attribute(3)]];
};

struct UiVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float mode;
};

vertex UiVertexOut ui_vertex(UiVertexIn in [[stage_in]],
                             constant float2 &inverseScreenSize [[buffer(1)]]) {
    UiVertexOut out;
    float2 ndc = float2(in.position.x * inverseScreenSize.x * 2.0 - 1.0,
                        1.0 - in.position.y * inverseScreenSize.y * 2.0);
    out.position = float4(ndc, 0.0, 1.0);
    out.color = float4(in.color) / 255.0;
    out.uv = in.uv;
    out.mode = in.mode;
    return out;
}

fragment float4 ui_fragment(UiVertexOut in [[stage_in]],
                            texture2d<float> fontAtlas [[texture(0)]]) {
    constexpr sampler glyphSampler(filter::linear, address::clamp_to_edge);
    float4 color = in.color;
    if (in.mode > 0.5) {
        float signedDistance = fontAtlas.sample(glyphSampler, in.uv).r;
        float edgeWidth = max(fwidth(signedDistance), 0.0001);
        color.a *= smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, signedDistance);
    }
    return color;
}
)";

constexpr int kMaxUiVertices = 65536;

constexpr int kFontFirstChar = 32;
constexpr int kFontCharCount = 96;
constexpr float kFontPixelSize = 20.0f; // logical UI px the atlas is baked at
constexpr int kGlyphPad = 2;            // transparent gutter between atlas cells
constexpr int kSdfSupersample = 4;     // CoreText raster scale before the distance transform
constexpr float kSdfRange = 4.0f;      // logical px the signed distance spans each side of the edge

} // namespace

struct UiRenderState {
    id<MTLRenderPipelineState> pipeline;
    id<MTLBuffer> vertexBuffer;
    id<MTLTexture> fontAtlas;
    UiFontMetrics fontMetrics;
};

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

// Rasterizes ASCII 32..127 of Argentum Sans (embedded) into a one-row R8 atlas
// of signed distance fields via CoreText, and fills `out` with per-glyph
// placement (quads carry a kSdfRange gutter so the shader can reconstruct the
// edge). Falls back to the system UI font if the embedded face fails to load.
id<MTLTexture> BuildFontAtlas(id<MTLDevice> device, UiFontMetrics *out) {
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
        cellW[i] = empty ? 0 : (int)ceil(boundingRects[i].size.width) + 1 + 2 * pad;
        cellH[i] = empty ? 0 : (int)ceil(boundingRects[i].size.height) + 1 + 2 * pad;
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
        CGRect bbox = boundingRects[i];
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
        CGPoint pos = CGPointMake((CGFloat)pad - bbox.origin.x, (CGFloat)pad - bbox.origin.y);
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

        // The atlas cell is the glyph ink box grown by `pad` on every side; the
        // quad carries that gutter so the shader's smoothstep has field to read.
        g.u0 = (float)cellX[i] / (float)atlasWidth;
        g.u1 = ((float)cellX[i] + cellW[i]) / (float)atlasWidth;
        g.v0 = 0.0f;
        g.v1 = (float)cellH[i] / (float)atlasHeight;
        g.offsetX = (float)bbox.origin.x - pad;
        g.offsetY = (float)(bbox.origin.y + bbox.size.height) + pad; // baseline -> top of cell, +up
        g.width = (float)cellW[i];
        g.height = (float)cellH[i];
    }
    CFRelease(font);

    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                          width:atlasWidth
                                                         height:atlasHeight
                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> atlas = [device newTextureWithDescriptor:descriptor];
    [atlas replaceRegion:MTLRegionMake2D(0, 0, atlasWidth, atlasHeight)
             mipmapLevel:0
               withBytes:pixels
             bytesPerRow:bytesPerRow];
    free(pixels);
    return atlas;
}

} // namespace

UiRenderState *UiRenderInit(Arena *arena, GpuContext *gpu) {
    id<MTLDevice> device = gpu->device;
    MTLPixelFormat colorFormat = gpu->colorFormat;

    UiRenderState *state = ArenaPushStruct(arena, UiRenderState);

    NSError *error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithSource:[NSString stringWithUTF8String:kUiShaderSource]
                             options:nil
                               error:&error];
    if (library == nil) {
        NSLog(@"Failed to compile UI shader library: %@", error);
        abort();
    }

    MTLVertexDescriptor *vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
    vertexDescriptor.attributes[0].offset = offsetof(UiVertex, x);
    vertexDescriptor.attributes[0].bufferIndex = 0;
    vertexDescriptor.attributes[1].format = MTLVertexFormatUChar4;
    vertexDescriptor.attributes[1].offset = offsetof(UiVertex, rgba);
    vertexDescriptor.attributes[1].bufferIndex = 0;
    vertexDescriptor.attributes[2].format = MTLVertexFormatFloat2;
    vertexDescriptor.attributes[2].offset = offsetof(UiVertex, u);
    vertexDescriptor.attributes[2].bufferIndex = 0;
    vertexDescriptor.attributes[3].format = MTLVertexFormatFloat;
    vertexDescriptor.attributes[3].offset = offsetof(UiVertex, mode);
    vertexDescriptor.attributes[3].bufferIndex = 0;
    vertexDescriptor.layouts[0].stride = sizeof(UiVertex);

    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"ui_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"ui_fragment"];
    descriptor.vertexDescriptor = vertexDescriptor;
    descriptor.colorAttachments[0].pixelFormat = colorFormat;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    NSError *pipelineError = nil;
    state->pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&pipelineError];
    if (state->pipeline == nil) {
        NSLog(@"Failed to create UI pipeline: %@", pipelineError);
        abort();
    }

    state->vertexBuffer = [device newBufferWithLength:kMaxUiVertices * sizeof(UiVertex)
                                             options:MTLResourceStorageModeShared];
    state->fontAtlas = BuildFontAtlas(device, &state->fontMetrics);
    return state;
}

const UiFontMetrics *UiRenderFontMetrics(const UiRenderState *uiRender) {
    return &uiRender->fontMetrics;
}

void UiRenderEncode(UiRenderState *uiRender, RenderTarget *targetPtr, const UiVertex *vertices,
                    int vertexCount, float drawableWidth, float drawableHeight) {
    RenderTarget target = *targetPtr;
    if (vertexCount <= 0) {
        return;
    }
    if (vertexCount > kMaxUiVertices) {
        vertexCount = kMaxUiVertices;
    }
    memcpy([uiRender->vertexBuffer contents], vertices, vertexCount * sizeof(UiVertex));

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target.drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    float inverseScreenSize[2] = {1.0f / drawableWidth, 1.0f / drawableHeight};

    id<MTLRenderCommandEncoder> encoder =
        [target.commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:uiRender->pipeline];
    [encoder setVertexBuffer:uiRender->vertexBuffer offset:0 atIndex:0];
    [encoder setVertexBytes:inverseScreenSize length:sizeof(inverseScreenSize) atIndex:1];
    [encoder setFragmentTexture:uiRender->fontAtlas atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertexCount];
    [encoder endEncoding];
}
