#include "renderer.h"

#include "gpu_metal.h"
#include "theme.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

struct MeshVertex {
    float position[3];
    float normal[3];
};

constexpr float kFovYRadians = 60.0f * (float)M_PI / 180.0f;

// On-screen size the gizmo arms and the entity icons hold as the camera
// dollies, in content-viewport pixels.
constexpr float kGizmoPixelSize = 80.0f;
constexpr float kIconPixelSize = 40.0f;

// Per-frame overlay geometry (gizmo + icons). Sized for the worst realistic
// frame; GizmoMeshBuilder clamps rather than overruns.
constexpr int kMaxOverlayLineVertices = 32768;
constexpr int kMaxOverlayTriVertices = 16384;

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

float Clamp(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float RandomUnit() {
    return (float)rand() / (float)RAND_MAX;
}

// eye = target + distance * sphericalDirection(yaw, pitch). yaw is measured
// from +Z toward +X.
Vec3 OrbitCameraEye(Vec3 target, float distance, float yaw, float pitch) {
    Vec3 direction = {
        cosf(pitch) * sinf(yaw),
        sinf(pitch),
        cosf(pitch) * cosf(yaw),
    };
    return Vec3Add(target, Vec3Scale(direction, distance));
}

// Screen-space right/up axes of the orbit camera, derived algebraically from
// yaw/pitch rather than from a second Mat4LookAt.
Vec3 OrbitCameraRight(float yaw) { return Vec3{cosf(yaw), 0.0f, -sinf(yaw)}; }

Vec3 OrbitCameraUp(float yaw, float pitch) {
    return Vec3{
        -sinf(pitch) * sinf(yaw),
        cosf(pitch),
        -sinf(pitch) * cosf(yaw),
    };
}

// Looks at target with world-up (0, 1, 0).
Mat4 OrbitCameraViewMatrix(Vec3 target, float distance, float yaw, float pitch) {
    Vec3 eye = OrbitCameraEye(target, distance, yaw, pitch);
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
    float contentOriginX;
    float contentOriginY;
    float contentWidth;
    float contentHeight;

    id<MTLCounterSampleBuffer> timestampSampleBuffer;
    bool timingSupported;
    float passMs[5];

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
        texels[i * 4 + 3] = RandomUnit();
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

    BuildAoKernel(state);
    state->noiseTexture = BuildNoiseTexture(device);
    state->timestampSampleBuffer = MakeTimestampSampleBuffer(device);
    state->timingSupported = state->timestampSampleBuffer != nil;

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

    RendererSetContentRect(state, 0.0f, 0.0f, drawableWidth, drawableHeight);

    return state;
}

void RendererSetContentRect(RendererState *renderer, float originX, float originY, float width,
                            float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    renderer->contentOriginX = originX;
    renderer->contentOriginY = originY;
    renderer->contentWidth = width;
    renderer->contentHeight = height;

    float aspectRatio = width / height;
    renderer->projection = Mat4Perspective(kFovYRadians, aspectRatio, 0.1f, 100.0f);
    renderer->view = OrbitCameraViewMatrix(renderer->cameraTarget, renderer->cameraDistance,
                                            renderer->cameraYaw, renderer->cameraPitch);
    renderer->viewProjection = Mat4Multiply(renderer->projection, renderer->view);

    AllocateScreenTargets(renderer, (uint32_t)(width + 0.5f), (uint32_t)(height + 0.5f));
}

void RendererSetDebugView(RendererState *renderer, int mode) { renderer->debugMode = mode; }

void RendererSetFxaaEnabled(RendererState *renderer, bool enabled) {
    renderer->fxaaEnabled = enabled;
}

void RendererUpdateCamera(RendererState *renderer, FrameInput input) {
    renderer->cameraYaw -= input.orbitYaw * kOrbitSensitivity;
    renderer->cameraPitch =
        Clamp(renderer->cameraPitch + input.orbitPitch * kOrbitSensitivity, -kMaxCameraPitch, kMaxCameraPitch);
    renderer->cameraDistance =
        Clamp(renderer->cameraDistance * (1.0f - input.zoomDelta), kMinCameraDistance, kMaxCameraDistance);

    Vec3 right = OrbitCameraRight(renderer->cameraYaw);
    Vec3 up = OrbitCameraUp(renderer->cameraYaw, renderer->cameraPitch);

    float panScale = kPanSensitivity * renderer->cameraDistance;
    renderer->cameraTarget.x += (-right.x * input.panX + up.x * input.panY) * panScale;
    renderer->cameraTarget.y += (-right.y * input.panX + up.y * input.panY) * panScale;
    renderer->cameraTarget.z += (-right.z * input.panX + up.z * input.panY) * panScale;

    renderer->view = OrbitCameraViewMatrix(renderer->cameraTarget, renderer->cameraDistance,
                                            renderer->cameraYaw, renderer->cameraPitch);
    renderer->viewProjection = Mat4Multiply(renderer->projection, renderer->view);
}

Vec3 RendererCameraFocus(const RendererState *renderer) {
    return renderer->cameraTarget;
}

Mat4 RendererViewProjection(const RendererState *renderer) {
    return renderer->viewProjection;
}

float RendererGizmoScale(const RendererState *renderer, Vec3 pivot) {
    Vec3 eye = OrbitCameraEye(renderer->cameraTarget, renderer->cameraDistance, renderer->cameraYaw,
                              renderer->cameraPitch);
    return GizmoWorldScale(pivot, eye, kFovYRadians, renderer->contentHeight, kGizmoPixelSize);
}

Ray RendererScreenPointToRay(const RendererState *renderer, float screenX, float screenY) {
    Vec3 eye = OrbitCameraEye(renderer->cameraTarget, renderer->cameraDistance, renderer->cameraYaw,
                              renderer->cameraPitch);

    float viewportX = screenX - renderer->contentOriginX;
    float viewportY = screenY - renderer->contentOriginY;
    float ndcX = 2.0f * viewportX / renderer->contentWidth - 1.0f;
    float ndcY = 1.0f - 2.0f * viewportY / renderer->contentHeight;

    // Metal clip space is z in [0, 1]; unproject the near and far points of
    // this pixel's line and use the segment between them as the direction.
    Mat4 inverseViewProjection = Mat4Inverse(renderer->viewProjection);
    Vec3 nearPoint = Mat4TransformPoint(inverseViewProjection, Vec3{ndcX, ndcY, 0.0f});
    Vec3 farPoint = Mat4TransformPoint(inverseViewProjection, Vec3{ndcX, ndcY, 1.0f});

    Ray ray;
    ray.origin = eye;
    ray.dir = Vec3Normalize(Vec3Sub(farPoint, nearPoint));
    return ray;
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
        uniforms.modelViewProjection = Mat4Multiply(renderer->viewProjection, model);
        uniforms.modelView = Mat4Multiply(renderer->view, model);
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

    Vec3 lightView = Mat4TransformDirection(renderer->view, kLightDirectionWorld);
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

void CopyThemeColor(float *out, theme::Color color) {
    theme::Rgba rgba = theme::ToFloat(color);
    out[0] = rgba.r;
    out[1] = rgba.g;
    out[2] = rgba.b;
    out[3] = rgba.a;
}

bool EntitySelected(const SceneState *scene, EntityId entity) {
    return SceneSelectionContains(scene, SelectionItem{SelectionKind_Entity, (uint32_t)entity});
}

// Fills the two shared overlay buffers with this frame's gizmo handles and
// entity icons. Every colour comes from theme.h.
void BuildOverlayGeometry(RendererState *renderer, const RendererSceneView *view, int *outLineCount,
                          int *outTriCount) {
    GizmoMeshBuilder builder = {};
    builder.lines = (GizmoVertex *)[renderer->overlayLineBuffer contents];
    builder.lineCapacity = kMaxOverlayLineVertices;
    builder.tris = (GizmoVertex *)[renderer->overlayTriBuffer contents];
    builder.triCapacity = kMaxOverlayTriVertices;

    static IconInstance icons[kMaxAudioSources + kMaxAudioListeners];
    int iconCount = 0;

    static SceneAudioSourceView sources[kMaxAudioSources];
    int sourceCount = SceneAudioSources(view->scene, sources, kMaxAudioSources);
    for (int i = 0; i < sourceCount; ++i) {
        icons[iconCount].position = sources[i].worldPos;
        icons[iconCount].kind = 0;
        icons[iconCount].selected = EntitySelected(view->scene, sources[i].entity);
        icons[iconCount].minDistance = sources[i].params.minDist;
        icons[iconCount].maxDistance = sources[i].params.maxDist;
        iconCount++;
    }

    static SceneAudioListenerView listeners[kMaxAudioListeners];
    int listenerCount = SceneAudioListeners(view->scene, listeners, kMaxAudioListeners);
    for (int i = 0; i < listenerCount; ++i) {
        icons[iconCount].position = listeners[i].worldPos;
        icons[iconCount].kind = listeners[i].active ? 2 : 1;
        icons[iconCount].selected = EntitySelected(view->scene, listeners[i].entity);
        icons[iconCount].minDistance = 0.0f;
        icons[iconCount].maxDistance = 0.0f;
        iconCount++;
    }

    IconColors iconColors;
    CopyThemeColor(iconColors.source, theme::AudioSourceIcon);
    CopyThemeColor(iconColors.listener, theme::ListenerIcon);
    CopyThemeColor(iconColors.activeListener, theme::ListenerActive);
    CopyThemeColor(iconColors.backdrop, theme::IconBackdrop);
    CopyThemeColor(iconColors.selectedOutline, theme::SelectionOutline);
    CopyThemeColor(iconColors.distanceSphere, theme::AudioDistanceSphere);

    Vec3 eye = OrbitCameraEye(renderer->cameraTarget, renderer->cameraDistance, renderer->cameraYaw,
                              renderer->cameraPitch);
    Vec3 right = OrbitCameraRight(renderer->cameraYaw);
    Vec3 up = OrbitCameraUp(renderer->cameraYaw, renderer->cameraPitch);

    // One call per icon: the billboard size is distance-dependent, so each
    // icon needs its own world size to hold a constant size on screen.
    for (int i = 0; i < iconCount; ++i) {
        float iconWorldSize = GizmoWorldScale(icons[i].position, eye, kFovYRadians,
                                              renderer->contentHeight, kIconPixelSize);
        GizmoBuildIcons(&builder, &icons[i], 1, right, up, iconWorldSize, iconColors);
    }

    if (view->gizmoVisible) {
        GizmoColors gizmoColors;
        CopyThemeColor(gizmoColors.axisX, theme::GizmoAxisX);
        CopyThemeColor(gizmoColors.axisY, theme::GizmoAxisY);
        CopyThemeColor(gizmoColors.axisZ, theme::GizmoAxisZ);
        CopyThemeColor(gizmoColors.highlight, theme::GizmoActive);
        CopyThemeColor(gizmoColors.uniform, theme::SelectionOutline);

        GizmoHandle highlighted =
            view->activeHandle != GizmoHandle_None ? view->activeHandle : view->hoveredHandle;
        GizmoBuild(&builder, view->toolMode, view->gizmoPivot,
                   RendererGizmoScale(renderer, view->gizmoPivot), highlighted, gizmoColors);
    }

    *outLineCount = builder.lineCount;
    *outTriCount = builder.triCount;
}

// Unlit, alpha-blended, no depth test: the gizmo and icons always read on top
// of the lit scene, which is what an editor overlay wants.
void EncodeOverlayPass(RendererState *renderer, const RendererSceneView *view,
                       id<MTLCommandBuffer> commandBuffer, id<MTLTexture> destination,
                       MTLViewport viewport) {
    int lineCount = 0;
    int triCount = 0;
    BuildOverlayGeometry(renderer, view, &lineCount, &triCount);
    if (lineCount == 0 && triCount == 0) {
        return;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    Mat4 viewProjection = renderer->viewProjection;

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
    MTLViewport contentViewport = {floor((double)renderer->contentOriginX),
                                   floor((double)renderer->contentOriginY),
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
        EncodeFxaaPass(renderer, target.commandBuffer, target.drawable.texture, contentViewport);
    } else {
        EncodeLightingPass(renderer, target.commandBuffer, target.drawable.texture, contentViewport);
        EncodeOverlayPass(renderer, view, target.commandBuffer, target.drawable.texture,
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
