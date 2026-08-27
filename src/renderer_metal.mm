#include "renderer_metal.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

struct MeshVertex {
    float position[3];
    float normal[3];
};

// One 256-byte-aligned slot per object in the uniform buffer (256 is
// Metal's minimum constant buffer offset alignment on macOS).
constexpr size_t kUniformStride = 256;

// Orbit-camera feel. Tuned by hand, not measured against a specific
// trackpad — adjust here if a gesture feels inverted or too fast/slow.
constexpr float kPanSensitivity = 0.0025f;  // world units per point, per unit of distance
constexpr float kOrbitSensitivity = 0.006f; // radians per point
constexpr float kMinCameraDistance = 2.5f;
constexpr float kMaxCameraDistance = 60.0f;
constexpr float kMaxCameraPitch = 1.5f; // radians; keeps the view short of the poles

// Screen-space ambient occlusion. Radius is in world units — the demo cubes
// are 1 unit across, so this is roughly "darken where surfaces are within
// half a cube of each other". Bias fights depth-precision self-occlusion;
// power sharpens the falloff. Tune with the 'o' key's debug views.
constexpr float kAoRadius = 0.6f;
constexpr float kAoBias = 0.025f;
constexpr float kAoPower = 1.6f;
constexpr int kAoKernelSize = 32;
constexpr int kAoNoiseSize = 4;

const Vec3 kLightDirectionWorld = {0.4f, 1.0f, 0.6f};

struct GeoUniforms {
    Mat4 modelViewProjection;
    Mat4 modelView;
};

struct AoParams {
    Mat4 projection;
    float sampleOffsets[kAoKernelSize][4];
    float params0[4]; // radius, bias, power, unused
    float params1[4]; // screenWidth, screenHeight, unused, unused
};

struct LightParams {
    float lightDirectionView[4];
    float misc[4]; // x: debug mode
};

// clang-format off
const MeshVertex kCubeVertices[] = {
    // +X
    {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
    // -X
    {{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
    // +Y
    {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f, 0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    // -Y
    {{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}},
    // +Z
    {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{ 0.5f,  0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f,  0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    // -Z
    {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
};

const uint16_t kCubeIndices[] = {
    0,  1,  2,  0,  2,  3,
    4,  5,  6,  4,  6,  7,
    8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15,
    16, 17, 18, 16, 18, 19,
    20, 21, 22, 20, 22, 23,
};

// A flat quad in the XZ plane (normal +Y), half-extent 1 to match Blender's
// default plane size exactly, so no import-time scale correction is needed
// for planes the way there is for cubes.
const MeshVertex kPlaneVertices[] = {
    {{-1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-1.0f, 0.0f,  1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 1.0f, 0.0f,  1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
};

const uint16_t kPlaneIndices[] = {
    0, 1, 2, 0, 2, 3,
};
// clang-format on

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
};

struct GeoVertexOut {
    float4 position [[position]];
    float3 viewPosition;
    float3 viewNormal;
};

struct GBufferOut {
    float4 position [[color(0)]];
    float4 normal [[color(1)]];
};

vertex GeoVertexOut geometry_vertex(VertexIn in [[stage_in]],
                                    constant GeoUniforms &uniforms [[buffer(1)]]) {
    GeoVertexOut out;
    out.position = uniforms.modelViewProjection * float4(in.position, 1.0);
    out.viewPosition = (uniforms.modelView * float4(in.position, 1.0)).xyz;
    out.viewNormal = (uniforms.modelView * float4(in.normal, 0.0)).xyz;
    return out;
}

fragment GBufferOut geometry_fragment(GeoVertexOut in [[stage_in]]) {
    GBufferOut out;
    out.position = float4(in.viewPosition, 1.0);
    out.normal = float4(normalize(in.viewNormal), 0.0);
    return out;
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
                           texture2d<float> positionTexture [[texture(0)]],
                           texture2d<float> normalTexture [[texture(1)]],
                           texture2d<float> noiseTexture [[texture(2)]],
                           constant AoParams &params [[buffer(0)]]) {
    constexpr sampler pointSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler noiseSampler(address::repeat, filter::nearest);

    float4 positionSample = positionTexture.sample(pointSampler, in.uv);
    if (positionSample.w < 0.5) {
        return 1.0;
    }
    float3 fragPosition = positionSample.xyz;
    float3 normal = normalize(normalTexture.sample(pointSampler, in.uv).xyz);

    float2 noiseScale = params.params1.xy / float(4.0);
    float3 randomVec = normalize(noiseTexture.sample(noiseSampler, in.uv * noiseScale).xyz);
    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tangentToView = float3x3(tangent, bitangent, normal);

    float radius = params.params0.x;
    float bias = params.params0.y;
    float occlusion = 0.0;
    for (int i = 0; i < kKernelSize; ++i) {
        float3 samplePosition = fragPosition + (tangentToView * params.sampleOffsets[i].xyz) * radius;
        float4 clip = params.projection * float4(samplePosition, 1.0);
        clip.xyz /= clip.w;
        float2 sampleUV = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);

        float4 occluderSample = positionTexture.sample(pointSampler, sampleUV);
        if (occluderSample.w < 0.5) {
            continue;
        }
        float occluderZ = occluderSample.z;
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPosition.z - occluderZ));
        occlusion += (occluderZ >= samplePosition.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(kKernelSize);
    return pow(max(ao, 0.0), params.params0.z);
}

fragment float blur_fragment(FullscreenOut in [[stage_in]],
                             texture2d<float> aoTexture [[texture(0)]],
                             constant float2 &texelSize [[buffer(0)]]) {
    constexpr sampler pointSampler(address::clamp_to_edge, filter::nearest);
    float result = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            float2 offset = float2(float(x), float(y)) * texelSize;
            result += aoTexture.sample(pointSampler, in.uv + offset).r;
        }
    }
    return result / 16.0;
}

struct LightParams {
    float4 lightDirectionView;
    float4 misc; // x: debug mode
};

fragment float4 lighting_fragment(FullscreenOut in [[stage_in]],
                                  texture2d<float> positionTexture [[texture(0)]],
                                  texture2d<float> normalTexture [[texture(1)]],
                                  texture2d<float> aoTexture [[texture(2)]],
                                  constant LightParams &params [[buffer(0)]]) {
    constexpr sampler pointSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);

    float4 positionSample = positionTexture.sample(pointSampler, in.uv);
    int debugMode = int(params.misc.x);
    float ao = aoTexture.sample(linearSampler, in.uv).r;

    if (positionSample.w < 0.5) {
        return float4(0.05, 0.05, 0.08, 1.0);
    }
    if (debugMode == 1) {
        return float4(ao, ao, ao, 1.0);
    }

    float3 normal = normalize(normalTexture.sample(pointSampler, in.uv).xyz);
    float3 lightDirection = normalize(params.lightDirectionView.xyz);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float ambientOcclusion = (debugMode == 2) ? 1.0 : ao;
    float3 baseColor = float3(0.25, 0.55, 0.95);
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
)";

float Clamp(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float RandomUnit() {
    return (float)rand() / (float)RAND_MAX;
}

// eye = target + distance * sphericalDirection(yaw, pitch), looking at
// target with world-up (0, 1, 0). yaw is measured from +Z toward +X.
Mat4 OrbitCameraViewMatrix(Vec3 target, float distance, float yaw, float pitch) {
    Vec3 direction = {
        cosf(pitch) * sinf(yaw),
        sinf(pitch),
        cosf(pitch) * cosf(yaw),
    };
    Vec3 eye = {
        target.x + distance * direction.x,
        target.y + distance * direction.y,
        target.z + distance * direction.z,
    };
    Vec3 up = {0.0f, 1.0f, 0.0f};
    return Mat4LookAt(eye, target, up);
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
    id<MTLRenderPipelineState> blurPipeline;
    id<MTLRenderPipelineState> lightingPipeline;
    id<MTLRenderPipelineState> fxaaPipeline;
    id<MTLDepthStencilState> depthState;

    id<MTLBuffer> cubeVertexBuffer;
    id<MTLBuffer> cubeIndexBuffer;
    id<MTLBuffer> planeVertexBuffer;
    id<MTLBuffer> planeIndexBuffer;
    id<MTLBuffer> uniformBuffer;

    id<MTLTexture> gPositionTexture;
    id<MTLTexture> gNormalTexture;
    id<MTLTexture> sceneDepthTexture;
    id<MTLTexture> aoRawTexture;
    id<MTLTexture> aoBlurTexture;
    id<MTLTexture> litColorTexture;
    id<MTLTexture> noiseTexture;
    uint32_t screenWidth;
    uint32_t screenHeight;

    float aoKernel[kAoKernelSize][4];

    Mat4 projection;
    Mat4 view;
    Mat4 viewProjection;
    Vec3 cameraTarget;
    float cameraDistance;
    float cameraYaw;
    float cameraPitch;
    int debugMode;
    bool fxaaEnabled;

    uint32_t cubeIndexCount;
    uint32_t planeIndexCount;
};

namespace {

// The g-buffer, AO, and blur targets are the only textures whose size
// depends on the drawable, so they are (re)created here whenever it
// changes. Everything else the renderer owns is allocated once in
// RendererInit.
void AllocateScreenTargets(RendererState *state, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    if (state->gPositionTexture != nil && state->screenWidth == width &&
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
    state->gPositionTexture = [state->device newTextureWithDescriptor:floatTarget];
    state->gNormalTexture = [state->device newTextureWithDescriptor:floatTarget];

    MTLTextureDescriptor *depthTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:state->depthFormat
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    depthTarget.usage = MTLTextureUsageRenderTarget;
    depthTarget.storageMode = MTLStorageModePrivate;
    state->sceneDepthTexture = [state->device newTextureWithDescriptor:depthTarget];

    MTLTextureDescriptor *aoTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    aoTarget.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    aoTarget.storageMode = MTLStorageModePrivate;
    state->aoRawTexture = [state->device newTextureWithDescriptor:aoTarget];
    state->aoBlurTexture = [state->device newTextureWithDescriptor:aoTarget];

    MTLTextureDescriptor *litTarget =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:state->colorFormat
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    litTarget.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    litTarget.storageMode = MTLStorageModePrivate;
    state->litColorTexture = [state->device newTextureWithDescriptor:litTarget];
}

void BuildAoKernel(RendererState *state) {
    srand(1);
    for (int i = 0; i < kAoKernelSize; ++i) {
        Vec3 sample = {
            RandomUnit() * 2.0f - 1.0f,
            RandomUnit() * 2.0f - 1.0f,
            RandomUnit(),
        };
        float length = sqrtf(sample.x * sample.x + sample.y * sample.y + sample.z * sample.z);
        sample = {sample.x / length, sample.y / length, sample.z / length};

        float scale = (float)i / (float)kAoKernelSize;
        scale = 0.1f + 0.9f * scale * scale;

        state->aoKernel[i][0] = sample.x * scale;
        state->aoKernel[i][1] = sample.y * scale;
        state->aoKernel[i][2] = sample.z * scale;
        state->aoKernel[i][3] = 0.0f;
    }
}

id<MTLTexture> BuildNoiseTexture(id<MTLDevice> device) {
    float texels[kAoNoiseSize * kAoNoiseSize * 4];
    for (int i = 0; i < kAoNoiseSize * kAoNoiseSize; ++i) {
        texels[i * 4 + 0] = RandomUnit() * 2.0f - 1.0f;
        texels[i * 4 + 1] = RandomUnit() * 2.0f - 1.0f;
        texels[i * 4 + 2] = 0.0f;
        texels[i * 4 + 3] = 0.0f;
    }

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

} // namespace

RendererState *RendererInit(Arena *arena, id<MTLDevice> device,
                             MTLPixelFormat colorFormat, MTLPixelFormat depthFormat,
                             float drawableWidth, float drawableHeight) {
    RendererState *state = ArenaPushStruct(arena, RendererState);
    state->device = device;
    state->colorFormat = colorFormat;
    state->depthFormat = depthFormat;
    state->cubeIndexCount = sizeof(kCubeIndices) / sizeof(kCubeIndices[0]);
    state->planeIndexCount = sizeof(kPlaneIndices) / sizeof(kPlaneIndices[0]);

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
    state->uniformBuffer = [device newBufferWithLength:kUniformStride * kMaxSceneObjects
                                                options:MTLResourceStorageModeShared];

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
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
    state->blurPipeline = MakeFullscreenPipeline(device, library, @"blur_fragment", MTLPixelFormatR8Unorm);
    state->lightingPipeline = MakeFullscreenPipeline(device, library, @"lighting_fragment", colorFormat);
    state->fxaaPipeline = MakeFullscreenPipeline(device, library, @"fxaa_fragment", colorFormat);

    MTLDepthStencilDescriptor *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
    depthDescriptor.depthWriteEnabled = YES;
    state->depthState = [device newDepthStencilStateWithDescriptor:depthDescriptor];

    BuildAoKernel(state);
    state->noiseTexture = BuildNoiseTexture(device);

    // Derive the initial orbit parameters from the original fixed eye/target
    // so the starting view is unchanged from before camera controls existed.
    Vec3 initialEye = {0.0f, 3.5f, 12.0f};
    state->cameraTarget = Vec3{0.0f, 0.0f, 0.0f};
    Vec3 offset = {initialEye.x - state->cameraTarget.x, initialEye.y - state->cameraTarget.y,
                    initialEye.z - state->cameraTarget.z};
    state->cameraDistance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    state->cameraYaw = atan2f(offset.x, offset.z);
    state->cameraPitch = asinf(offset.y / state->cameraDistance);
    state->debugMode = 0;
    state->fxaaEnabled = true;

    RendererResize(state, drawableWidth, drawableHeight);

    return state;
}

void RendererResize(RendererState *renderer, float drawableWidth, float drawableHeight) {
    if (drawableWidth <= 0.0f || drawableHeight <= 0.0f) {
        return;
    }
    float aspectRatio = drawableWidth / drawableHeight;
    renderer->projection =
        Mat4Perspective(60.0f * (float)M_PI / 180.0f, aspectRatio, 0.1f, 100.0f);
    renderer->view = OrbitCameraViewMatrix(renderer->cameraTarget, renderer->cameraDistance,
                                            renderer->cameraYaw, renderer->cameraPitch);
    renderer->viewProjection = Mat4Multiply(renderer->projection, renderer->view);

    AllocateScreenTargets(renderer, (uint32_t)drawableWidth, (uint32_t)drawableHeight);
}

void RendererUpdateCamera(RendererState *renderer, CameraInput input) {
    if (input.cycleDebugView) {
        renderer->debugMode = (renderer->debugMode + 1) % 3;
    }
    if (input.toggleFxaa) {
        renderer->fxaaEnabled = !renderer->fxaaEnabled;
    }

    renderer->cameraYaw -= input.orbitYaw * kOrbitSensitivity;
    renderer->cameraPitch =
        Clamp(renderer->cameraPitch + input.orbitPitch * kOrbitSensitivity, -kMaxCameraPitch, kMaxCameraPitch);
    renderer->cameraDistance =
        Clamp(renderer->cameraDistance * (1.0f - input.zoomDelta), kMinCameraDistance, kMaxCameraDistance);

    // Screen-space right/up axes of the current orbit camera, derived
    // algebraically from yaw/pitch rather than re-deriving them from a
    // second Mat4LookAt call.
    Vec3 right = {cosf(renderer->cameraYaw), 0.0f, -sinf(renderer->cameraYaw)};
    Vec3 up = {
        -sinf(renderer->cameraPitch) * sinf(renderer->cameraYaw),
        cosf(renderer->cameraPitch),
        -sinf(renderer->cameraPitch) * cosf(renderer->cameraYaw),
    };

    float panScale = kPanSensitivity * renderer->cameraDistance;
    renderer->cameraTarget.x += (-right.x * input.panX + up.x * input.panY) * panScale;
    renderer->cameraTarget.y += (-right.y * input.panX + up.y * input.panY) * panScale;
    renderer->cameraTarget.z += (-right.z * input.panX + up.z * input.panY) * panScale;

    renderer->view = OrbitCameraViewMatrix(renderer->cameraTarget, renderer->cameraDistance,
                                            renderer->cameraYaw, renderer->cameraPitch);
    renderer->viewProjection = Mat4Multiply(renderer->projection, renderer->view);
}

namespace {

void EncodeGeometryPass(RendererState *renderer, const GameState *game, id<MTLCommandBuffer> commandBuffer) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = renderer->gPositionTexture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[1].texture = renderer->gNormalTexture;
    pass.colorAttachments[1].loadAction = MTLLoadActionClear;
    pass.colorAttachments[1].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = renderer->sceneDepthTexture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.clearDepth = 1.0;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->geometryPipeline];
    [encoder setDepthStencilState:renderer->depthState];
    [encoder setCullMode:MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];

    uint8_t *uniformContents = (uint8_t *)[renderer->uniformBuffer contents];
    for (int i = 0; i < game->objectCount; ++i) {
        const SceneObject &object = game->objects[i];

        Mat4 model = Mat4Multiply(Mat4Translation(object.position),
                                   Mat4Multiply(object.rotation, Mat4Scale(object.scale)));

        GeoUniforms uniforms;
        uniforms.modelViewProjection = Mat4Multiply(renderer->viewProjection, model);
        uniforms.modelView = Mat4Multiply(renderer->view, model);

        size_t offset = i * kUniformStride;
        memcpy(uniformContents + offset, &uniforms, sizeof(GeoUniforms));

        bool isCube = object.primitive == Primitive::Cube;
        id<MTLBuffer> vertexBuffer = isCube ? renderer->cubeVertexBuffer : renderer->planeVertexBuffer;
        id<MTLBuffer> indexBuffer = isCube ? renderer->cubeIndexBuffer : renderer->planeIndexBuffer;
        uint32_t indexCount = isCube ? renderer->cubeIndexCount : renderer->planeIndexCount;

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
    params.projection = renderer->projection;
    memcpy(params.sampleOffsets, renderer->aoKernel, sizeof(params.sampleOffsets));
    params.params0[0] = kAoRadius;
    params.params0[1] = kAoBias;
    params.params0[2] = kAoPower;
    params.params0[3] = 0.0f;
    params.params1[0] = (float)renderer->screenWidth;
    params.params1[1] = (float)renderer->screenHeight;
    params.params1[2] = 0.0f;
    params.params1[3] = 0.0f;

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->aoPipeline];
    [encoder setFragmentTexture:renderer->gPositionTexture atIndex:0];
    [encoder setFragmentTexture:renderer->gNormalTexture atIndex:1];
    [encoder setFragmentTexture:renderer->noiseTexture atIndex:2];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void EncodeBlurPass(RendererState *renderer, id<MTLCommandBuffer> commandBuffer) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = renderer->aoBlurTexture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    float texelSize[2] = {1.0f / (float)renderer->screenWidth, 1.0f / (float)renderer->screenHeight};

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->blurPipeline];
    [encoder setFragmentTexture:renderer->aoRawTexture atIndex:0];
    [encoder setFragmentBytes:texelSize length:sizeof(texelSize) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void EncodeLightingPass(RendererState *renderer, id<MTLCommandBuffer> commandBuffer,
                        id<MTLTexture> destination) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    Vec3 lightView = Mat4TransformDirection(renderer->view, kLightDirectionWorld);
    LightParams params;
    params.lightDirectionView[0] = lightView.x;
    params.lightDirectionView[1] = lightView.y;
    params.lightDirectionView[2] = lightView.z;
    params.lightDirectionView[3] = 0.0f;
    params.misc[0] = (float)renderer->debugMode;
    params.misc[1] = 0.0f;
    params.misc[2] = 0.0f;
    params.misc[3] = 0.0f;

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->lightingPipeline];
    [encoder setFragmentTexture:renderer->gPositionTexture atIndex:0];
    [encoder setFragmentTexture:renderer->gNormalTexture atIndex:1];
    [encoder setFragmentTexture:renderer->aoBlurTexture atIndex:2];
    [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

void EncodeFxaaPass(RendererState *renderer, id<MTLCommandBuffer> commandBuffer,
                    id<MTLTexture> destination) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    float inverseScreenSize[2] = {1.0f / (float)renderer->screenWidth,
                                  1.0f / (float)renderer->screenHeight};

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:renderer->fxaaPipeline];
    [encoder setFragmentTexture:renderer->litColorTexture atIndex:0];
    [encoder setFragmentBytes:inverseScreenSize length:sizeof(inverseScreenSize) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

} // namespace

void RendererRender(RendererState *renderer, const GameState *game, RenderTarget target) {
    EncodeGeometryPass(renderer, game, target.commandBuffer);
    EncodeAoPass(renderer, target.commandBuffer);
    EncodeBlurPass(renderer, target.commandBuffer);

    if (renderer->fxaaEnabled) {
        EncodeLightingPass(renderer, target.commandBuffer, renderer->litColorTexture);
        EncodeFxaaPass(renderer, target.commandBuffer, target.drawable.texture);
    } else {
        EncodeLightingPass(renderer, target.commandBuffer, target.drawable.texture);
    }

    [target.commandBuffer presentDrawable:target.drawable];
    [target.commandBuffer commit];
}
