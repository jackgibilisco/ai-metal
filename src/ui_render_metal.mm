#include "ui_render.h"

#include "gpu_metal.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "third_party/font8x8_basic.h"

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
    constexpr sampler glyphSampler(filter::nearest, address::clamp_to_edge);
    float4 color = in.color;
    if (in.mode > 0.5) {
        color.a *= fontAtlas.sample(glyphSampler, in.uv).r;
    }
    return color;
}
)";

constexpr int kMaxUiVertices = 65536;

// Glyph atlas layout. ui.cpp mirrors kFontFirstChar / kFontCharCount to place
// text quads; a glyph's cell is column (codepoint - kFontFirstChar) of an
// kFontCharCount-wide, one-row grid of 8x8 cells.
constexpr int kFontFirstChar = 32;
constexpr int kFontCharCount = 96;
constexpr int kGlyphSize = 8;

} // namespace

struct UiRenderState {
    id<MTLRenderPipelineState> pipeline;
    id<MTLBuffer> vertexBuffer;
    id<MTLTexture> fontAtlas;
};

namespace {

id<MTLTexture> BuildFontAtlas(id<MTLDevice> device) {
    int atlasWidth = kFontCharCount * kGlyphSize;
    int atlasHeight = kGlyphSize;
    uint8_t *pixels = (uint8_t *)calloc((size_t)atlasWidth * atlasHeight, 1);
    for (int glyph = 0; glyph < kFontCharCount; ++glyph) {
        const unsigned char *rows = font8x8_basic[kFontFirstChar + glyph];
        for (int row = 0; row < kGlyphSize; ++row) {
            for (int col = 0; col < kGlyphSize; ++col) {
                if (rows[row] & (1 << col)) {
                    pixels[row * atlasWidth + glyph * kGlyphSize + col] = 255;
                }
            }
        }
    }

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
             bytesPerRow:atlasWidth];
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
    state->fontAtlas = BuildFontAtlas(device);
    return state;
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
