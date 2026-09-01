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
        color.a *= fontAtlas.sample(glyphSampler, in.uv).r;
    }
    return color;
}
)";

constexpr int kMaxUiVertices = 65536;

constexpr int kFontFirstChar = 32;
constexpr int kFontCharCount = 96;
constexpr float kFontPixelSize = 20.0f; // logical UI px the atlas is baked at
constexpr int kGlyphPad = 2;            // transparent gutter between atlas cells

} // namespace

struct UiRenderState {
    id<MTLRenderPipelineState> pipeline;
    id<MTLBuffer> vertexBuffer;
    id<MTLTexture> fontAtlas;
    UiFontMetrics fontMetrics;
};

namespace {

// Rasterizes ASCII 32..127 of Argentum Sans (embedded) into a one-row R8 atlas
// via CoreText and fills `out` with per-glyph placement. Falls back to the
// system UI font if the embedded face fails to load.
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

    int atlasHeight = (int)ceil(ascent + descent) + kGlyphPad * 2;
    CGGlyph glyphs[kFontCharCount] = {};
    CGRect boundingRects[kFontCharCount] = {};
    CGSize advances[kFontCharCount] = {};
    int cellX[kFontCharCount] = {};
    int cellW[kFontCharCount] = {};

    int atlasWidth = kGlyphPad;
    for (int i = 0; i < kFontCharCount; ++i) {
        UniChar ch = (UniChar)(kFontFirstChar + i);
        CTFontGetGlyphsForCharacters(font, &ch, &glyphs[i], 1);
        CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyphs[i],
                                        &boundingRects[i], 1);
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyphs[i], &advances[i], 1);
        int w = (glyphs[i] == 0 || CGRectIsNull(boundingRects[i]) ||
                 boundingRects[i].size.width <= 0.0)
                    ? 0
                    : (int)ceil(boundingRects[i].size.width) + 1;
        cellW[i] = w;
        cellX[i] = atlasWidth;
        atlasWidth += w + kGlyphPad;
    }

    size_t bytesPerRow = (size_t)atlasWidth;
    uint8_t *pixels = (uint8_t *)calloc(bytesPerRow * atlasHeight, 1);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(pixels, atlasWidth, atlasHeight, 8, bytesPerRow, gray,
                                             kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);

    // Baseline sits kGlyphPad above the descent gutter. The bitmap context
    // rasterizes top-down here, so atlas row 0 is the top of the glyph cell.
    CGFloat baseline = (CGFloat)kGlyphPad + descent;
    for (int i = 0; i < kFontCharCount; ++i) {
        UiGlyphMetric &g = out->glyphs[i];
        g = {};
        g.advance = (float)advances[i].width;
        if (cellW[i] == 0) {
            continue;
        }
        CGRect bbox = boundingRects[i];
        CGPoint pos = CGPointMake((CGFloat)cellX[i] - bbox.origin.x, baseline);
        CTFontDrawGlyphs(font, &glyphs[i], &pos, 1, ctx);

        float w = (float)bbox.size.width;
        float h = (float)bbox.size.height;
        // CGBitmapContext draws bottom-up; the byte buffer is row 0 = top. So a
        // CG y of `cy` lands in buffer row (atlasHeight - cy), which is the v
        // texel row Metal samples.
        float glyphBottomCG = (float)(baseline + bbox.origin.y);
        float glyphTopCG = glyphBottomCG + h;
        g.u0 = (float)cellX[i] / (float)atlasWidth;
        g.u1 = ((float)cellX[i] + w) / (float)atlasWidth;
        g.v0 = ((float)atlasHeight - glyphTopCG) / (float)atlasHeight;    // screen-top corner
        g.v1 = ((float)atlasHeight - glyphBottomCG) / (float)atlasHeight; // screen-bottom corner
        g.offsetX = (float)bbox.origin.x;
        g.offsetY = (float)(bbox.origin.y + bbox.size.height); // baseline -> top of glyph
        g.width = w;
        g.height = h;
    }
    CGContextRelease(ctx);
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
