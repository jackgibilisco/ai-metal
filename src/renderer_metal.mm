#include "renderer_metal.h"

#include <cstddef>
#include <cstring>

namespace {

struct CubeVertex {
    float position[3];
    float normal[3];
};

// One 256-byte-aligned slot per cube in the uniform buffer (256 is Metal's
// minimum constant buffer offset alignment on macOS).
constexpr size_t kUniformStride = 256;

// Orbit-camera feel. Tuned by hand, not measured against a specific
// trackpad — adjust here if a gesture feels inverted or too fast/slow.
constexpr float kPanSensitivity = 0.0025f;  // world units per point, per unit of distance
constexpr float kOrbitSensitivity = 0.006f; // radians per point
constexpr float kMinCameraDistance = 2.5f;
constexpr float kMaxCameraDistance = 60.0f;
constexpr float kMaxCameraPitch = 1.5f; // radians; keeps the view short of the poles

struct Uniforms {
    Mat4 modelViewProjection;
    Mat4 model;
};

// clang-format off
const CubeVertex kCubeVertices[] = {
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
// clang-format on

const char *kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
};

struct Uniforms {
    float4x4 modelViewProjection;
    float4x4 model;
};

vertex VertexOut vertex_main(VertexIn in [[stage_in]],
                              constant Uniforms &uniforms [[buffer(1)]]) {
    VertexOut out;
    out.position = uniforms.modelViewProjection * float4(in.position, 1.0);
    out.normal = (uniforms.model * float4(in.normal, 0.0)).xyz;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]]) {
    float3 lightDirection = normalize(float3(0.4, 1.0, 0.6));
    float diffuse = max(dot(normalize(in.normal), lightDirection), 0.0);
    float3 color = float3(0.25, 0.55, 0.95) * (0.35 + 0.65 * diffuse);
    return float4(color, 1.0);
}
)";

float Clamp(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
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

} // namespace

struct RendererState {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipelineState;
    id<MTLDepthStencilState> depthState;
    id<MTLBuffer> vertexBuffer;
    id<MTLBuffer> indexBuffer;
    id<MTLBuffer> uniformBuffer;
    Mat4 projection;
    Mat4 viewProjection;
    Vec3 cameraTarget;
    float cameraDistance;
    float cameraYaw;
    float cameraPitch;
    uint32_t indexCount;
};

RendererState *RendererInit(Arena *arena, id<MTLDevice> device,
                             MTLPixelFormat colorFormat, MTLPixelFormat depthFormat,
                             float aspectRatio) {
    RendererState *state = ArenaPushStruct(arena, RendererState);
    state->device = device;
    state->indexCount = sizeof(kCubeIndices) / sizeof(kCubeIndices[0]);

    state->vertexBuffer = [device newBufferWithBytes:kCubeVertices
                                               length:sizeof(kCubeVertices)
                                              options:MTLResourceStorageModeShared];
    state->indexBuffer = [device newBufferWithBytes:kCubeIndices
                                              length:sizeof(kCubeIndices)
                                             options:MTLResourceStorageModeShared];
    state->uniformBuffer = [device newBufferWithLength:kUniformStride * kCubeCount
                                                options:MTLResourceStorageModeShared];

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                                   options:nil
                                                     error:&error];
    if (library == nil) {
        NSLog(@"Failed to compile shader library: %@", error);
        abort();
    }

    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragment_main"];

    MTLVertexDescriptor *vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    vertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
    vertexDescriptor.attributes[0].offset = offsetof(CubeVertex, position);
    vertexDescriptor.attributes[0].bufferIndex = 0;
    vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
    vertexDescriptor.attributes[1].offset = offsetof(CubeVertex, normal);
    vertexDescriptor.attributes[1].bufferIndex = 0;
    vertexDescriptor.layouts[0].stride = sizeof(CubeVertex);

    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.vertexDescriptor = vertexDescriptor;
    pipelineDescriptor.colorAttachments[0].pixelFormat = colorFormat;
    pipelineDescriptor.depthAttachmentPixelFormat = depthFormat;

    NSError *pipelineError = nil;
    state->pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor
                                                                    error:&pipelineError];
    if (state->pipelineState == nil) {
        NSLog(@"Failed to create render pipeline state: %@", pipelineError);
        abort();
    }

    MTLDepthStencilDescriptor *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
    depthDescriptor.depthWriteEnabled = YES;
    state->depthState = [device newDepthStencilStateWithDescriptor:depthDescriptor];

    // Derive the initial orbit parameters from the original fixed eye/target
    // so the starting view is unchanged from before camera controls existed.
    Vec3 initialEye = {0.0f, 3.5f, 12.0f};
    state->cameraTarget = Vec3{0.0f, 0.0f, 0.0f};
    Vec3 offset = {initialEye.x - state->cameraTarget.x, initialEye.y - state->cameraTarget.y,
                    initialEye.z - state->cameraTarget.z};
    state->cameraDistance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    state->cameraYaw = atan2f(offset.x, offset.z);
    state->cameraPitch = asinf(offset.y / state->cameraDistance);

    state->projection = Mat4Perspective(60.0f * (float)M_PI / 180.0f, aspectRatio, 0.1f, 100.0f);
    Mat4 view = OrbitCameraViewMatrix(state->cameraTarget, state->cameraDistance, state->cameraYaw,
                                       state->cameraPitch);
    state->viewProjection = Mat4Multiply(state->projection, view);

    return state;
}

void RendererUpdateCamera(RendererState *renderer, CameraInput input) {
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

    Mat4 view = OrbitCameraViewMatrix(renderer->cameraTarget, renderer->cameraDistance, renderer->cameraYaw,
                                       renderer->cameraPitch);
    renderer->viewProjection = Mat4Multiply(renderer->projection, view);
}

void RendererRender(RendererState *renderer, const GameState *game, RenderTarget target) {
    id<MTLRenderCommandEncoder> encoder =
        [target.commandBuffer renderCommandEncoderWithDescriptor:target.passDescriptor];

    [encoder setRenderPipelineState:renderer->pipelineState];
    [encoder setDepthStencilState:renderer->depthState];
    [encoder setCullMode:MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder setVertexBuffer:renderer->vertexBuffer offset:0 atIndex:0];

    uint8_t *uniformContents = (uint8_t *)[renderer->uniformBuffer contents];

    for (int i = 0; i < kCubeCount; ++i) {
        const Cube &cube = game->cubes[i];

        Mat4 rotation = Mat4Multiply(Mat4RotationY(cube.rotation.y), Mat4RotationX(cube.rotation.x));
        Mat4 model = Mat4Multiply(Mat4Translation(cube.position), rotation);
        Mat4 modelViewProjection = Mat4Multiply(renderer->viewProjection, model);

        Uniforms uniforms;
        uniforms.modelViewProjection = modelViewProjection;
        uniforms.model = model;

        size_t offset = i * kUniformStride;
        memcpy(uniformContents + offset, &uniforms, sizeof(Uniforms));

        [encoder setVertexBuffer:renderer->uniformBuffer offset:offset atIndex:1];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:renderer->indexCount
                              indexType:MTLIndexTypeUInt16
                            indexBuffer:renderer->indexBuffer
                      indexBufferOffset:0];
    }

    [encoder endEncoding];
    [target.commandBuffer presentDrawable:target.drawable];
    [target.commandBuffer commit];
}
