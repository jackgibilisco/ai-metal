#include "ui_render_metal.h"

#include <cstddef>
#include <cstring>

namespace {

const char *kUiShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct UiVertexIn {
    float2 position [[attribute(0)]];
    uchar4 color [[attribute(1)]];
};

struct UiVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex UiVertexOut ui_vertex(UiVertexIn in [[stage_in]],
                             constant float2 &inverseScreenSize [[buffer(1)]]) {
    UiVertexOut out;
    float2 ndc = float2(in.position.x * inverseScreenSize.x * 2.0 - 1.0,
                        1.0 - in.position.y * inverseScreenSize.y * 2.0);
    out.position = float4(ndc, 0.0, 1.0);
    out.color = float4(in.color) / 255.0;
    return out;
}

fragment float4 ui_fragment(UiVertexOut in [[stage_in]]) {
    return in.color;
}
)";

constexpr int kMaxUiVertices = 16384;

} // namespace

struct UiRenderState {
    id<MTLRenderPipelineState> pipeline;
    id<MTLBuffer> vertexBuffer;
};

UiRenderState *UiRenderInit(Arena *arena, id<MTLDevice> device, MTLPixelFormat colorFormat) {
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
    return state;
}

void UiRenderEncode(UiRenderState *uiRender, RenderTarget target, const UiVertex *vertices,
                    int vertexCount, float drawableWidth, float drawableHeight) {
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
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertexCount];
    [encoder endEncoding];
}
