#include "renderer.h"

#include "gpu_metal.h"
#include "renderer_common.h"
#include "theme.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

// One 256-byte-aligned slot per object in the uniform buffer (256 is
// Metal's minimum constant buffer offset alignment on macOS).
constexpr size_t kUniformStride = 256;

struct GeoUniforms {
    Mat4 modelViewProjection;
    Mat4 modelView;
    float baseColor[4]; // per-draw albedo: white unselected, blue selected
};

struct AoParams {
    Mat4 projection;
    float sampleOffsets[kAoKernelSize][4];
    float params0[4]; // radius, bias, power, unused
    float params1[4]; // screenWidth, screenHeight, unused, unused
};

struct LightParams {
    float lightDirectionView[4];
    float misc[4]; // x: debug mode, yz: 1/aoWidth, 1/aoHeight
};

const char *kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

constant int kKernelSize = 32;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct GeoUniforms {
    float4x4 modelViewProjection;
    float4x4 modelView;
    float4 baseColor;
};

struct GeoVertexOut {
    float4 position [[position]];
    float viewZ;
    float3 viewNormal;
    float3 baseColor;
};

vertex GeoVertexOut geometry_vertex(VertexIn in [[stage_in]],
                                    constant GeoUniforms &uniforms [[buffer(1)]]) {
    GeoVertexOut out;
    out.position = uniforms.modelViewProjection * float4(in.position, 1.0);
    out.viewZ = (uniforms.modelView * float4(in.position, 1.0)).z;
    out.viewNormal = (uniforms.modelView * float4(in.normal, 0.0)).xyz;
    out.baseColor = uniforms.baseColor.rgb;
    return out;
}

// G-buffer: color(0) xyz = view-space normal, w = view-space Z (always
// negative for real geometry; the pass clears w to 1.0, so w >= 0.5 means
// "background"). color(1) rgb = per-object albedo the lighting pass shades.
struct GeoOut {
    float4 gbuffer [[color(0)]];
    float4 albedo [[color(1)]];
};

fragment GeoOut geometry_fragment(GeoVertexOut in [[stage_in]]) {
    GeoOut out;
    out.gbuffer = float4(normalize(in.viewNormal), in.viewZ);
    out.albedo = float4(in.baseColor, 1.0);
    return out;
}

float3 ReconstructViewPosition(float2 uv, float viewZ, float4x4 projection) {
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = 1.0 - uv.y * 2.0;
    float viewX = ndcX * (-viewZ) / projection[0][0];
    float viewY = ndcY * (-viewZ) / projection[1][1];
    return float3(viewX, viewY, viewZ);
}

struct FullscreenOut {
    float4 position [[position]];
    float2 uv;
};

vertex FullscreenOut fullscreen_vertex(uint vertexID [[vertex_id]]) {
    float2 corners[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    float2 ndc = corners[vertexID];
    FullscreenOut out;
    out.position = float4(ndc, 0.0, 1.0);
    out.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return out;
}

struct AoParams {
    float4x4 projection;
    float4 sampleOffsets[kKernelSize];
    float4 params0; // radius, bias, power, unused
    float4 params1; // screenWidth, screenHeight, unused, unused
};

fragment float ao_fragment(FullscreenOut in [[stage_in]],
                           texture2d<float> normalTexture [[texture(1)]],
                           texture2d<float> noiseTexture [[texture(2)]],
                           constant AoParams &params [[buffer(0)]]) {
    constexpr sampler pointSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler noiseSampler(address::repeat, filter::nearest);

    float4 normalSample = normalTexture.sample(pointSampler, in.uv);
    if (normalSample.w >= 0.5) {
        return 1.0;
    }
    float3 fragPosition = ReconstructViewPosition(in.uv, normalSample.w, params.projection);
    float3 normal = normalize(normalSample.xyz);

    float2 noiseScale = params.params1.xy / float(4.0);
    float4 noiseSample = noiseTexture.sample(noiseSampler, in.uv * noiseScale);
    float3 randomVec = normalize(noiseSample.xyz);
    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tangentToView = float3x3(tangent, bitangent, normal);

    // Jitters the whole kernel's radius per pixel so a flat, uniformly-angled
    // surface (e.g. the ground plane) doesn't self-occlude at the kernel's
    // fixed sample distances identically everywhere, which reads as
    // concentric banding instead of noise. The 4x4 noise tile repeats this
    // jitter at a fine period; the lighting pass's box blur smooths it out.
    float radius = params.params0.x * (0.75 + 0.5 * noiseSample.w);
    float bias = params.params0.y;
    float occlusion = 0.0;
    for (int i = 0; i < kKernelSize; ++i) {
        float3 samplePosition = fragPosition + (tangentToView * params.sampleOffsets[i].xyz) * radius;
        float4 clip = params.projection * float4(samplePosition, 1.0);
        clip.xyz /= clip.w;
        float2 sampleUV = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);

        float4 occluderSample = normalTexture.sample(pointSampler, sampleUV);
        if (occluderSample.w >= 0.5) {
            continue;
        }
        float occluderZ = occluderSample.w;
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPosition.z - occluderZ));
        occlusion += (occluderZ >= samplePosition.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(kKernelSize);
    return pow(max(ao, 0.0), params.params0.z);
}

struct LightParams {
    float4 lightDirectionView;
    float4 misc; // x: debug mode, yz: 1/aoWidth, 1/aoHeight
};

fragment float4 lighting_fragment(FullscreenOut in [[stage_in]],
                                  texture2d<float> normalTexture [[texture(1)]],
                                  texture2d<float> aoTexture [[texture(2)]],
                                  texture2d<float> albedoTexture [[texture(3)]],
                                  constant LightParams &params [[buffer(0)]]) {
    constexpr sampler pointSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);

    float4 normalSample = normalTexture.sample(pointSampler, in.uv);
    int debugMode = int(params.misc.x);

    // 4x4 box blur of the half-res AO, folded in from the old separate pass.
    float2 aoTexel = params.misc.yz;
    float ao = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            ao += aoTexture.sample(linearSampler, in.uv + float2(x, y) * aoTexel).r;
        }
    }
    ao /= 16.0;

    if (normalSample.w >= 0.5) {
        return float4(kThemeViewportBackground, 1.0);
    }
    if (debugMode == 1) {
        return float4(ao, ao, ao, 1.0);
    }

    float3 normal = normalize(normalSample.xyz);
    float3 lightDirection = normalize(params.lightDirectionView.xyz);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float ambientOcclusion = (debugMode == 2) ? 1.0 : ao;
    constexpr sampler albedoSampler(address::clamp_to_edge, filter::nearest);
    float3 baseColor = albedoTexture.sample(albedoSampler, in.uv).rgb;
    float3 color = baseColor * (0.35 * ambientOcclusion + 0.65 * diffuse);
    return float4(color, 1.0);
}

constant float kFxaaSpanMax = 8.0;
constant float kFxaaReduceMul = 1.0 / 8.0;
constant float kFxaaReduceMin = 1.0 / 128.0;
constant float3 kLumaWeights = float3(0.299, 0.587, 0.114);

fragment float4 fxaa_fragment(FullscreenOut in [[stage_in]],
                              texture2d<float> litTexture [[texture(0)]],
                              constant float2 &inverseScreenSize [[buffer(0)]]) {
    constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);
    float2 uv = in.uv;

    float lumaNW = dot(litTexture.sample(linearSampler, uv + float2(-1.0, -1.0) * inverseScreenSize).rgb, kLumaWeights);
    float lumaNE = dot(litTexture.sample(linearSampler, uv + float2( 1.0, -1.0) * inverseScreenSize).rgb, kLumaWeights);
    float lumaSW = dot(litTexture.sample(linearSampler, uv + float2(-1.0,  1.0) * inverseScreenSize).rgb, kLumaWeights);
    float lumaSE = dot(litTexture.sample(linearSampler, uv + float2( 1.0,  1.0) * inverseScreenSize).rgb, kLumaWeights);
    float3 rgbM = litTexture.sample(linearSampler, uv).rgb;
    float lumaM = dot(rgbM, kLumaWeights);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < lumaMax * 0.125) {
        return float4(rgbM, 1.0);
    }

    float2 direction = float2(
        -((lumaNW + lumaNE) - (lumaSW + lumaSE)),
         ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
    float directionReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kFxaaReduceMul), kFxaaReduceMin);
    float inverseDirectionMin = 1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirectionMin,
                      float2(-kFxaaSpanMax), float2(kFxaaSpanMax)) * inverseScreenSize;

    float3 rgbInner = 0.5 * (
        litTexture.sample(linearSampler, uv + direction * (1.0 / 3.0 - 0.5)).rgb +
        litTexture.sample(linearSampler, uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgbOuter = rgbInner * 0.5 + 0.25 * (
        litTexture.sample(linearSampler, uv + direction * -0.5).rgb +
        litTexture.sample(linearSampler, uv + direction *  0.5).rgb);

    float lumaOuter = dot(rgbOuter, kLumaWeights);
    if (lumaOuter < lumaMin || lumaOuter > lumaMax) {
        return float4(rgbInner, 1.0);
    }
    return float4(rgbOuter, 1.0);
}

struct OverlayIn {
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct OverlayOut {
    float4 position [[position]];
    float4 color;
};

vertex OverlayOut overlay_vertex(OverlayIn in [[stage_in]],
                                 constant float4x4 &viewProjection [[buffer(1)]]) {
    OverlayOut out;
    out.position = viewProjection * float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 overlay_fragment(OverlayOut in [[stage_in]]) {
    return in.color;
}
)";

// theme.h owns every colour; the two the shader needs are injected as
// `constant` definitions ahead of the source above rather than duplicated
// as literals inside it.
NSString *BuildShaderSource() {
    theme::Rgba background = theme::ToFloat(theme::ViewportBackground);
    theme::Rgba meshBase = theme::ToFloat(theme::MeshBase);
    return [NSString stringWithFormat:@"#include <metal_stdlib>\n"
                                       "using namespace metal;\n"
                                       "constant float3 kThemeViewportBackground = float3(%f, %f, %f);\n"
                                       "constant float3 kThemeMeshBase = float3(%f, %f, %f);\n"
                                       "%s",
                                      background.r, background.g, background.b, meshBase.r,
                                      meshBase.g, meshBase.b, kShaderSource];
}

id<MTLRenderPipelineState> MakeFullscreenPipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                   NSString *fragmentName, MTLPixelFormat colorFormat) {
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"fullscreen_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
    descriptor.colorAttachments[0].pixelFormat = colorFormat;

    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) {
        NSLog(@"Failed to create pipeline %@: %@", fragmentName, error);
        abort();
    }
    return pipeline;
}

} // namespace

struct RendererState {
    id<MTLDevice> device;
    MTLPixelFormat colorFormat;
    MTLPixelFormat depthFormat;

    id<MTLRenderPipelineState> geometryPipeline;
    id<MTLRenderPipelineState> aoPipeline;
    id<MTLRenderPipelineState> lightingPipeline;
    id<MTLRenderPipelineState> fxaaPipeline;
    id<MTLRenderPipelineState> overlayPipeline;
    id<MTLDepthStencilState> depthState;

    id<MTLBuffer> overlayLineBuffer;
    id<MTLBuffer> overlayTriBuffer;

    id<MTLBuffer> cubeVertexBuffer;
    id<MTLBuffer> cubeIndexBuffer;
    id<MTLBuffer> planeVertexBuffer;
    id<MTLBuffer> planeIndexBuffer;
    id<MTLBuffer> uniformBuffer;

    id<MTLTexture> gNormalTexture;
    id<MTLTexture> gAlbedoTexture;
    id<MTLTexture> sceneDepthTexture;
    id<MTLTexture> aoRawTexture;
    id<MTLTexture> litColorTexture;
    id<MTLTexture> noiseTexture;
    uint32_t screenWidth;
    uint32_t screenHeight;
    uint32_t aoWidth;
    uint32_t aoHeight;

    id<MTLCounterSampleBuffer> timestampSampleBuffer;
    bool timingSupported;
    float passMs[5];

    float aoKernel[kAoKernelSize][4];

    RendererCamera camera;
    int debugMode;
    bool fxaaEnabled;

};

namespace {

// The g-buffer, AO, and lit-color targets are the only textures whose size
// depends on the drawable, so they are (re)created here whenever it
// changes. Everything else the renderer owns is allocated once in
// RendererInit.
void AllocateScreenTargets(RendererState *state, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    if (state->gNormalTexture != nil && state->screenWidth == width &&
        state->screenHeight == height) {
        return;
    }
    state->screenWidth = width;
    state->screenHeight = height;

    MTLTextureDescriptor *floatTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    floatTarget.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    floatTarget.storageMode = MTLStorageModePrivate;
    state->gNormalTexture = [state->device newTextureWithDescriptor:floatTarget];
    state->gAlbedoTexture = [state->device newTextureWithDescriptor:floatTarget];

    MTLTextureDescriptor *depthTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:state->depthFormat
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    depthTarget.usage = MTLTextureUsageRenderTarget;
    depthTarget.storageMode = MTLStorageModePrivate;
    state->sceneDepthTexture = [state->device newTextureWithDescriptor:depthTarget];

    state->aoWidth = (width + 1) / 2;
    state->aoHeight = (height + 1) / 2;
    MTLTextureDescriptor *aoTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                          width:state->aoWidth
                                                         height:state->aoHeight
                                                      mipmapped:NO];
    aoTarget.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    aoTarget.storageMode = MTLStorageModePrivate;
    state->aoRawTexture = [state->device newTextureWithDescriptor:aoTarget];

    MTLTextureDescriptor *litTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:state->colorFormat
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    litTarget.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    litTarget.storageMode = MTLStorageModePrivate;
    state->litColorTexture = [state->device newTextureWithDescriptor:litTarget];
}

id<MTLTexture> BuildNoiseTexture(id<MTLDevice> device, const float *texels) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                          width:kAoNoiseSize
                                                         height:kAoNoiseSize
                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    [texture replaceRegion:MTLRegionMake2D(0, 0, kAoNoiseSize, kAoNoiseSize)
               mipmapLevel:0
                 withBytes:texels
               bytesPerRow:kAoNoiseSize * 4 * sizeof(float)];
    return texture;
}

// Two timestamp samples (start/end) per pass for geometry/ao/lighting/fxaa.
constexpr NSUInteger kTimestampSampleCount = 8;

id<MTLCounterSampleBuffer> MakeTimestampSampleBuffer(id<MTLDevice> device) {
    if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        return nil;
    }
    id<MTLCounterSet> timestampSet = nil;
    for (id<MTLCounterSet> set in device.counterSets) {
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
            timestampSet = set;
        }
    }
    if (timestampSet == nil) {
        return nil;
    }

    MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
    descriptor.counterSet = timestampSet;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.sampleCount = kTimestampSampleCount;

    NSError *error = nil;
    id<MTLCounterSampleBuffer> buffer =
        [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
    if (buffer == nil) {
        NSLog(@"Timestamp sample buffer unavailable: %@", error);
    }
    return buffer;
}

void AttachPassTiming(RendererState *renderer, MTLRenderPassDescriptor *pass, int slot) {
    if (!renderer->timingSupported) {
        return;
    }
    pass.sampleBufferAttachments[0].sampleBuffer = renderer->timestampSampleBuffer;
    pass.sampleBufferAttachments[0].startOfVertexSampleIndex = slot * 2;
    pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = slot * 2 + 1;
}

} // namespace

RendererState *RendererInit(Arena *arena, GpuContext *gpu, float drawableWidth,
                            float drawableHeight) {
    id<MTLDevice> device = gpu->device;
    MTLPixelFormat colorFormat = gpu->colorFormat;
    MTLPixelFormat depthFormat = gpu->depthFormat;

    RendererState *state = ArenaPushStruct(arena, RendererState);
    state->device = device;
    state->colorFormat = colorFormat;
    state->depthFormat = depthFormat;

    state->cubeVertexBuffer = [device newBufferWithBytes:kCubeVertices
                                                    length:sizeof(kCubeVertices)
                                                   options:MTLResourceStorageModeShared];
    state->cubeIndexBuffer = [device newBufferWithBytes:kCubeIndices
                                                   length:sizeof(kCubeIndices)
                                                  options:MTLResourceStorageModeShared];
    state->planeVertexBuffer = [device newBufferWithBytes:kPlaneVertices
                                                     length:sizeof(kPlaneVertices)
                                                    options:MTLResourceStorageModeShared];
    state->planeIndexBuffer = [device newBufferWithBytes:kPlaneIndices
                                                    length:sizeof(kPlaneIndices)
                                                   options:MTLResourceStorageModeShared];
    state->uniformBuffer = [device newBufferWithLength:kUniformStride * kMaxMeshRenderers
                                                options:MTLResourceStorageModeShared];

    state->overlayLineBuffer =
        [device newBufferWithLength:sizeof(GizmoVertex) * kMaxOverlayLineVertices
                            options:MTLResourceStorageModeShared];
    state->overlayTriBuffer =
        [device newBufferWithLength:sizeof(GizmoVertex) * kMaxOverlayTriVertices
                            options:MTLResourceStorageModeShared];

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:BuildShaderSource()
                                                   options:nil
                                                     error:&error];
    if (library == nil) {
        NSLog(@"Failed to compile shader library: %@", error);
        abort();
    }

    MTLVertexDescriptor *vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    vertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
    vertexDescriptor.attributes[0].offset = offsetof(MeshVertex, position);
    vertexDescriptor.attributes[0].bufferIndex = 0;
    vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
    vertexDescriptor.attributes[1].offset = offsetof(MeshVertex, normal);
    vertexDescriptor.attributes[1].bufferIndex = 0;
    vertexDescriptor.layouts[0].stride = sizeof(MeshVertex);

    MTLRenderPipelineDescriptor *geometryDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    geometryDescriptor.vertexFunction = [library newFunctionWithName:@"geometry_vertex"];
    geometryDescriptor.fragmentFunction = [library newFunctionWithName:@"geometry_fragment"];
    geometryDescriptor.vertexDescriptor = vertexDescriptor;
    geometryDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    geometryDescriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float;
    geometryDescriptor.depthAttachmentPixelFormat = depthFormat;

    NSError *geometryError = nil;
    state->geometryPipeline =
        [device newRenderPipelineStateWithDescriptor:geometryDescriptor error:&geometryError];
    if (state->geometryPipeline == nil) {
        NSLog(@"Failed to create geometry pipeline: %@", geometryError);
        abort();
    }

    state->aoPipeline = MakeFullscreenPipeline(device, library, @"ao_fragment", MTLPixelFormatR8Unorm);
    state->lightingPipeline = MakeFullscreenPipeline(device, library, @"lighting_fragment", colorFormat);
    state->fxaaPipeline = MakeFullscreenPipeline(device, library, @"fxaa_fragment", colorFormat);

    MTLVertexDescriptor *overlayVertexDescriptor = [[MTLVertexDescriptor alloc] init];
    overlayVertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
    overlayVertexDescriptor.attributes[0].offset = offsetof(GizmoVertex, pos);
    overlayVertexDescriptor.attributes[0].bufferIndex = 0;
    overlayVertexDescriptor.attributes[1].format = MTLVertexFormatFloat4;
    overlayVertexDescriptor.attributes[1].offset = offsetof(GizmoVertex, rgba);
    overlayVertexDescriptor.attributes[1].bufferIndex = 0;
    overlayVertexDescriptor.layouts[0].stride = sizeof(GizmoVertex);

    MTLRenderPipelineDescriptor *overlayDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    overlayDescriptor.vertexFunction = [library newFunctionWithName:@"overlay_vertex"];
    overlayDescriptor.fragmentFunction = [library newFunctionWithName:@"overlay_fragment"];
    overlayDescriptor.vertexDescriptor = overlayVertexDescriptor;
    overlayDescriptor.colorAttachments[0].pixelFormat = colorFormat;
    overlayDescriptor.colorAttachments[0].blendingEnabled = YES;
    overlayDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    overlayDescriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    overlayDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    overlayDescriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;

    NSError *overlayError = nil;
    state->overlayPipeline =
        [device newRenderPipelineStateWithDescriptor:overlayDescriptor error:&overlayError];
    if (state->overlayPipeline == nil) {
        NSLog(@"Failed to create overlay pipeline: %@", overlayError);
        abort();
    }

    MTLDepthStencilDescriptor *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
    depthDescriptor.depthWriteEnabled = YES;
    state->depthState = [device newDepthStencilStateWithDescriptor:depthDescriptor];

    float noiseTexels[kAoNoiseSize * kAoNoiseSize * 4];
    BuildAoSamples(state->aoKernel, noiseTexels);
    state->noiseTexture = BuildNoiseTexture(device, noiseTexels);
    state->timestampSampleBuffer = MakeTimestampSampleBuffer(device);
    state->timingSupported = state->timestampSampleBuffer != nil;

    state->camera = CameraCreate();
    state->debugMode = 0;
    state->fxaaEnabled = true;

    RendererSetContentRect(state, 0.0f, 0.0f, drawableWidth, drawableHeight);

    return state;
}

void RendererSetContentRect(RendererState *renderer, float originX, float originY, float width,
                            float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    CameraSetContentRect(&renderer->camera, originX, originY, width, height);
    AllocateScreenTargets(renderer, (uint32_t)(width + 0.5f), (uint32_t)(height + 0.5f));
}

void RendererSetDebugView(RendererState *renderer, int mode) { renderer->debugMode = mode; }

void RendererSetFxaaEnabled(RendererState *renderer, bool enabled) {
    renderer->fxaaEnabled = enabled;
}

void RendererUpdateCamera(RendererState *renderer, FrameInput input) {
    CameraUpdate(&renderer->camera, input);
}

Vec3 RendererCameraFocus(const RendererState *renderer) { return renderer->camera.target; }

Mat4 RendererViewProjection(const RendererState *renderer) {
    return renderer->camera.viewProjection;
}

float RendererGizmoScale(const RendererState *renderer, Vec3 pivot) {
    return CameraGizmoScale(&renderer->camera, pivot);
}

Ray RendererScreenPointToRay(const RendererState *renderer, float screenX, float screenY) {
    return CameraScreenPointToRay(&renderer->camera, screenX, screenY);
}

namespace {

void EncodeGeometryPass(RendererState *renderer, const RendererSceneView *view,
                        id<MTLCommandBuffer> commandBuffer) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = renderer->gNormalTexture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    // w = 1.0 marks "background"; real geometry writes its negative view-space Z.
    theme::Rgba gBufferClear = theme::ToFloat(theme::GBufferClear);
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(gBufferClear.r, gBufferClear.g, gBufferClear.b, 1.0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[1].texture = renderer->gAlbedoTexture;
    pass.colorAttachments[1].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = renderer->sceneDepthTexture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.clearDepth = 1.0;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    AttachPassTiming(renderer, pass, 0);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->geometryPipeline];
    [encoder setDepthStencilState:renderer->depthState];
    [encoder setCullMode:MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];

    static SceneMeshView meshViews[kMaxMeshRenderers];
    int meshCount = SceneMeshRenderers(view->scene, meshViews, kMaxMeshRenderers);

    theme::Rgba meshBaseColor = theme::ToFloat(theme::MeshBase);
    theme::Rgba meshSelectedColor = theme::ToFloat(theme::MeshSelected);

    uint8_t *uniformContents = (uint8_t *)[renderer->uniformBuffer contents];
    for (int i = 0; i < meshCount; ++i) {
        const SceneMeshView &object = meshViews[i];

        Mat4 model = object.model;

        GeoUniforms uniforms;
        uniforms.modelViewProjection = Mat4Multiply(renderer->camera.viewProjection, model);
        uniforms.modelView = Mat4Multiply(renderer->camera.view, model);
        bool selected = SceneSelectionContains(
            view->scene, SelectionItem{SelectionKind_Entity, (uint32_t)object.entity});
        const theme::Rgba &albedo = selected ? meshSelectedColor : meshBaseColor;
        uniforms.baseColor[0] = albedo.r;
        uniforms.baseColor[1] = albedo.g;
        uniforms.baseColor[2] = albedo.b;
        uniforms.baseColor[3] = 1.0f;

        size_t offset = i * kUniformStride;
        memcpy(uniformContents + offset, &uniforms, sizeof(GeoUniforms));

        bool isCube = object.mesh == MeshId_Cube;
        id<MTLBuffer> vertexBuffer = isCube ? renderer->cubeVertexBuffer : renderer->planeVertexBuffer;
        id<MTLBuffer> indexBuffer = isCube ? renderer->cubeIndexBuffer : renderer->planeIndexBuffer;
        uint32_t indexCount = isCube ? kCubeIndexCount : kPlaneIndexCount;

        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [encoder setVertexBuffer:renderer->uniformBuffer offset:offset atIndex:1];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:indexCount
                              indexType:MTLIndexTypeUInt16
                            indexBuffer:indexBuffer
                      indexBufferOffset:0];
    }
    [encoder endEncoding];
}

void EncodeAoPass(RendererState *renderer, id<MTLCommandBuffer> commandBuffer) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = renderer->aoRawTexture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    AoParams params;
    params.projection = renderer->camera.projection;
    memcpy(params.sampleOffsets, renderer->aoKernel, sizeof(params.sampleOffsets));
    params.params0[0] = kAoRadius;
    params.params0[1] = kAoBias;
    params.params0[2] = kAoPower;
    params.params0[3] = 0.0f;
    params.params1[0] = (float)renderer->aoWidth;
    params.params1[1] = (float)renderer->aoHeight;
    params.params1[2] = 0.0f;
    params.params1[3] = 0.0f;
    AttachPassTiming(renderer, pass, 1);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->aoPipeline];
    [encoder setFragmentTexture:renderer->gNormalTexture atIndex:1];
    [encoder setFragmentTexture:renderer->noiseTexture atIndex:2];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void EncodeLightingPass(RendererState *renderer, id<MTLCommandBuffer> commandBuffer,
                        id<MTLTexture> destination, MTLViewport viewport) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    Vec3 lightView = Mat4TransformDirection(renderer->camera.view, kLightDirectionWorld);
    LightParams params;
    params.lightDirectionView[0] = lightView.x;
    params.lightDirectionView[1] = lightView.y;
    params.lightDirectionView[2] = lightView.z;
    params.lightDirectionView[3] = 0.0f;
    params.misc[0] = (float)renderer->debugMode;
    params.misc[1] = 1.0f / (float)renderer->aoWidth;
    params.misc[2] = 1.0f / (float)renderer->aoHeight;
    params.misc[3] = 0.0f;
    AttachPassTiming(renderer, pass, 2);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->lightingPipeline];
    // `destination` may be the full drawable while the scene only fills the
    // content region; the caller passes the region to write.
    [encoder setViewport:viewport];
    [encoder setFragmentTexture:renderer->gNormalTexture atIndex:1];
    [encoder setFragmentTexture:renderer->aoRawTexture atIndex:2];
    [encoder setFragmentTexture:renderer->gAlbedoTexture atIndex:3];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void EncodeFxaaPass(RendererState *renderer, id<MTLCommandBuffer> commandBuffer,
                    id<MTLTexture> destination, MTLViewport viewport) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    float inverseScreenSize[2] = {1.0f / (float)renderer->screenWidth,
                                  1.0f / (float)renderer->screenHeight};
    AttachPassTiming(renderer, pass, 3);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->fxaaPipeline];
    [encoder setViewport:viewport];
    [encoder setFragmentTexture:renderer->litColorTexture atIndex:0];
    [encoder setFragmentBytes:inverseScreenSize length:sizeof(inverseScreenSize) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

// Unlit, alpha-blended, no depth test: the gizmo and icons always read on top
// of the lit scene, which is what an editor overlay wants.
void EncodeOverlayPass(RendererState *renderer, const RendererSceneView *view,
                       id<MTLCommandBuffer> commandBuffer, id<MTLTexture> destination,
                       MTLViewport viewport) {
    int lineCount = 0;
    int triCount = 0;
    BuildOverlayGeometry(&renderer->camera, view,
                         (GizmoVertex *)[renderer->overlayLineBuffer contents],
                         (GizmoVertex *)[renderer->overlayTriBuffer contents], &lineCount,
                         &triCount);
    if (lineCount == 0 && triCount == 0) {
        return;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    Mat4 viewProjection = renderer->camera.viewProjection;

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->overlayPipeline];
    [encoder setViewport:viewport];
    [encoder setVertexBytes:&viewProjection length:sizeof(viewProjection) atIndex:1];

    if (triCount > 0) {
        [encoder setVertexBuffer:renderer->overlayTriBuffer offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:triCount];
    }
    if (lineCount > 0) {
        [encoder setVertexBuffer:renderer->overlayLineBuffer offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:lineCount];
    }
    [encoder endEncoding];
}

} // namespace

namespace {

void ResolvePassTimings(RendererState *renderer, uint32_t encodedSlotMask,
                        id<MTLCommandBuffer> commandBuffer) {
    if (renderer->timingSupported) {
        NSData *resolved = [renderer->timestampSampleBuffer
            resolveCounterRange:NSMakeRange(0, kTimestampSampleCount)];
        const MTLCounterResultTimestamp *samples =
            (const MTLCounterResultTimestamp *)resolved.bytes;
        for (int slot = 0; slot < 4; ++slot) {
            MTLTimestamp start = samples[slot * 2].timestamp;
            MTLTimestamp end = samples[slot * 2 + 1].timestamp;
            bool valid = (encodedSlotMask & (1u << slot)) && start != MTLCounterErrorValue &&
                         end != MTLCounterErrorValue && end >= start;
            renderer->passMs[slot] = valid ? (float)(end - start) * 1e-6f : 0.0f;
        }
    }
    renderer->passMs[4] =
        (float)((commandBuffer.GPUEndTime - commandBuffer.GPUStartTime) * 1000.0);
}

} // namespace

void RendererRender(RendererState *renderer, const RendererSceneView *view,
                    RenderTarget *targetPtr) {
    RenderTarget target = *targetPtr;
    bool aoEnabled = renderer->debugMode != 2;
    bool fxaaEnabled = renderer->fxaaEnabled;

    MTLViewport fullViewport = {0.0, 0.0, (double)renderer->screenWidth,
                               (double)renderer->screenHeight, 0.0, 1.0};
    // Pixel-align the origin and overdraw the extent by a few pixels: the final
    // composite must never leave a sub-pixel sliver between the viewport and a
    // docked panel. The fullscreen triangle covers any extent; the extra rows
    // land under the panels (or are clipped past the drawable edge).
    MTLViewport contentViewport = {floor((double)renderer->camera.contentOriginX),
                                   floor((double)renderer->camera.contentOriginY),
                                   (double)renderer->screenWidth + 4.0,
                                   (double)renderer->screenHeight + 4.0, 0.0, 1.0};

    EncodeGeometryPass(renderer, view, target.commandBuffer);
    if (aoEnabled) {
        EncodeAoPass(renderer, target.commandBuffer);
    }
    // With FXAA on, the gizmo and icons composite into the lit image before the
    // FXAA resolve, so their edges are antialiased along with the geometry.
    if (fxaaEnabled) {
        EncodeLightingPass(renderer, target.commandBuffer, renderer->litColorTexture, fullViewport);
        EncodeOverlayPass(renderer, view, target.commandBuffer, renderer->litColorTexture,
                          fullViewport);
        EncodeFxaaPass(renderer, target.commandBuffer, target.colorTexture, contentViewport);
    } else {
        EncodeLightingPass(renderer, target.commandBuffer, target.colorTexture, contentViewport);
        EncodeOverlayPass(renderer, view, target.commandBuffer, target.colorTexture,
                          contentViewport);
    }

    uint32_t encodedSlotMask =
        (1u << 0) | (1u << 2) | (aoEnabled ? (1u << 1) : 0u) | (fxaaEnabled ? (1u << 3) : 0u);
    [target.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer) {
        ResolvePassTimings(renderer, encodedSlotMask, commandBuffer);
    }];
}

RendererPassTimings RendererLastFrameTimings(const RendererState *renderer) {
    return RendererPassTimings{
        renderer->passMs[0], renderer->passMs[1], renderer->passMs[2],
        renderer->passMs[3], renderer->passMs[4],
    };
}
