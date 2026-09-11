#include "renderer.h"

#include "gl_shader.h"
#include "gpu_gl.h"
#include "renderer_common.h"
#include "theme.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

// The GLSL below is renderer_metal.mm's MSL ported line for line. Two GL
// conventions differ from Metal and are absorbed here:
// - Depth: Metal clips z to [0, w]. Vertex shaders emit z' = 2z - w, so GL's
//   [-w, w] clip range and the stored depth both match Metal.
// - Y: passes that render into the renderer's own textures negate clip-space
//   y (flipY = -1), so those textures store the top row first exactly like
//   Metal's, and every shader samples them with Metal's uv unchanged. Only
//   the final composite into the RenderTarget is drawn upright (flipY = 1).

namespace {

struct GlViewport {
    GLint x;
    GLint y;
    GLsizei width;
    GLsizei height;
};

// Where a fullscreen or overlay pass writes.
struct PassTarget {
    GLuint framebuffer;
    GlViewport viewport;
    float flipY;
};

// Every fragment shader starts with this; theme.h owns the colours.
const char *kFragmentPreludeFormat = R"(
const vec3 kThemeViewportBackground = vec3(%f, %f, %f);
const vec3 kThemeMeshBase = vec3(%f, %f, %f);
)";

const char *kGeometryVertex = R"(
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

uniform mat4 modelViewProjection;
uniform mat4 modelView;
uniform vec4 baseColor;

out float viewZ;
out vec3 viewNormal;
out vec3 albedo;

void main() {
    gl_Position = modelViewProjection * vec4(inPosition, 1.0);
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
    viewZ = (modelView * vec4(inPosition, 1.0)).z;
    viewNormal = (modelView * vec4(inNormal, 0.0)).xyz;
    albedo = baseColor.rgb;
}
)";

// G-buffer: location 0 xyz = view-space normal, w = view-space Z (always
// negative for real geometry; the pass clears w to 1.0, so w >= 0.5 means
// "background"). location 1 rgb = per-object albedo the lighting pass shades.
const char *kGeometryFragment = R"(
in float viewZ;
in vec3 viewNormal;
in vec3 albedo;

layout(location = 0) out vec4 outGBuffer;
layout(location = 1) out vec4 outAlbedo;

void main() {
    outGBuffer = vec4(normalize(viewNormal), viewZ);
    outAlbedo = vec4(albedo, 1.0);
}
)";

const char *kFullscreenVertex = R"(
uniform float flipY;

out vec2 uv;

void main() {
    vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 ndc = corners[gl_VertexID];
    gl_Position = vec4(ndc.x, ndc.y * flipY, 0.0, 1.0);
    uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
)";

const char *kAoFragment = R"(
const int kKernelSize = 32;

in vec2 uv;

uniform sampler2D normalTexture;
uniform sampler2D noiseTexture;
uniform mat4 projection;
uniform vec4 sampleOffsets[kKernelSize];
uniform vec4 params0; // radius, bias, power, unused
uniform vec4 params1; // screenWidth, screenHeight, unused, unused

out float outAo;

vec3 ReconstructViewPosition(vec2 uv, float viewZ, mat4 projection) {
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = 1.0 - uv.y * 2.0;
    float viewX = ndcX * (-viewZ) / projection[0][0];
    float viewY = ndcY * (-viewZ) / projection[1][1];
    return vec3(viewX, viewY, viewZ);
}

void main() {
    vec4 normalSample = texture(normalTexture, uv);
    if (normalSample.w >= 0.5) {
        outAo = 1.0;
        return;
    }
    vec3 fragPosition = ReconstructViewPosition(uv, normalSample.w, projection);
    vec3 normal = normalize(normalSample.xyz);

    vec2 noiseScale = params1.xy / float(4.0);
    vec4 noiseSample = texture(noiseTexture, uv * noiseScale);
    vec3 randomVec = normalize(noiseSample.xyz);
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 tangentToView = mat3(tangent, bitangent, normal);

    float radius = params0.x * (0.75 + 0.5 * noiseSample.w);
    float bias = params0.y;
    float occlusion = 0.0;
    for (int i = 0; i < kKernelSize; ++i) {
        vec3 samplePosition = fragPosition + (tangentToView * sampleOffsets[i].xyz) * radius;
        vec4 clip = projection * vec4(samplePosition, 1.0);
        clip.xyz /= clip.w;
        vec2 sampleUV = vec2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);

        vec4 occluderSample = texture(normalTexture, sampleUV);
        if (occluderSample.w >= 0.5) {
            continue;
        }
        float occluderZ = occluderSample.w;
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPosition.z - occluderZ));
        occlusion += (occluderZ >= samplePosition.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(kKernelSize);
    outAo = pow(max(ao, 0.0), params0.z);
}
)";

const char *kLightingFragment = R"(
in vec2 uv;

uniform sampler2D normalTexture;
uniform sampler2D aoTexture;
uniform sampler2D albedoTexture;
uniform vec4 lightDirectionView;
uniform vec4 misc; // x: debug mode, yz: 1/aoWidth, 1/aoHeight

out vec4 outColor;

void main() {
    vec4 normalSample = texture(normalTexture, uv);
    int debugMode = int(misc.x);

    // 4x4 box blur of the half-res AO.
    vec2 aoTexel = misc.yz;
    float ao = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            ao += texture(aoTexture, uv + vec2(x, y) * aoTexel).r;
        }
    }
    ao /= 16.0;

    if (normalSample.w >= 0.5) {
        outColor = vec4(kThemeViewportBackground, 1.0);
        return;
    }
    if (debugMode == 1) {
        outColor = vec4(ao, ao, ao, 1.0);
        return;
    }

    vec3 normal = normalize(normalSample.xyz);
    vec3 lightDirection = normalize(lightDirectionView.xyz);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float ambientOcclusion = (debugMode == 2) ? 1.0 : ao;
    vec3 baseColor = texture(albedoTexture, uv).rgb;
    vec3 color = baseColor * (0.35 * ambientOcclusion + 0.65 * diffuse);
    outColor = vec4(color, 1.0);
}
)";

const char *kFxaaFragment = R"(
const float kFxaaSpanMax = 8.0;
const float kFxaaReduceMul = 1.0 / 8.0;
const float kFxaaReduceMin = 1.0 / 128.0;
const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

in vec2 uv;

uniform sampler2D litTexture;
uniform vec2 inverseScreenSize;

out vec4 outColor;

void main() {
    float lumaNW = dot(texture(litTexture, uv + vec2(-1.0, -1.0) * inverseScreenSize).rgb, kLumaWeights);
    float lumaNE = dot(texture(litTexture, uv + vec2( 1.0, -1.0) * inverseScreenSize).rgb, kLumaWeights);
    float lumaSW = dot(texture(litTexture, uv + vec2(-1.0,  1.0) * inverseScreenSize).rgb, kLumaWeights);
    float lumaSE = dot(texture(litTexture, uv + vec2( 1.0,  1.0) * inverseScreenSize).rgb, kLumaWeights);
    vec3 rgbM = texture(litTexture, uv).rgb;
    float lumaM = dot(rgbM, kLumaWeights);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < lumaMax * 0.125) {
        outColor = vec4(rgbM, 1.0);
        return;
    }

    vec2 direction = vec2(
        -((lumaNW + lumaNE) - (lumaSW + lumaSE)),
         ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
    float directionReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kFxaaReduceMul), kFxaaReduceMin);
    float inverseDirectionMin = 1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirectionMin,
                      vec2(-kFxaaSpanMax), vec2(kFxaaSpanMax)) * inverseScreenSize;

    vec3 rgbInner = 0.5 * (
        texture(litTexture, uv + direction * (1.0 / 3.0 - 0.5)).rgb +
        texture(litTexture, uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbOuter = rgbInner * 0.5 + 0.25 * (
        texture(litTexture, uv + direction * -0.5).rgb +
        texture(litTexture, uv + direction *  0.5).rgb);

    float lumaOuter = dot(rgbOuter, kLumaWeights);
    if (lumaOuter < lumaMin || lumaOuter > lumaMax) {
        outColor = vec4(rgbInner, 1.0);
        return;
    }
    outColor = vec4(rgbOuter, 1.0);
}
)";

const char *kOverlayVertex = R"(
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

uniform mat4 viewProjection;
uniform float flipY;

out vec4 color;

void main() {
    gl_Position = viewProjection * vec4(inPosition, 1.0);
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y *= flipY;
    color = inColor;
}
)";

const char *kOverlayFragment = R"(
in vec4 color;

out vec4 outColor;

void main() {
    outColor = color;
}
)";

GLuint CreatePassProgram(const char *prelude, const char *vertexSource, const char *fragmentBody,
                         const char *label) {
    static char fragmentSource[16384];
    snprintf(fragmentSource, sizeof(fragmentSource), "%s%s", prelude, fragmentBody);
    return GlCreateProgram(vertexSource, fragmentSource, label);
}

void SetSamplerUnit(GLuint program, const char *name, GLint unit) {
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, name), unit);
}

void BindTexture(GLenum unit, GLuint texture) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
}

// Binds the pass's framebuffer and viewport, and hands `program` its flipY.
void BindPassTarget(PassTarget target, GLuint program) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(target.viewport.x, target.viewport.y, target.viewport.width,
               target.viewport.height);
    glUniform1f(glGetUniformLocation(program, "flipY"), target.flipY);
}

GLuint CreateTexture(GLenum internalFormat, int width, int height, GLenum format, GLenum type,
                     GLint filter, GLint wrap, const void *pixels) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return texture;
}

GLuint CreateMeshVertexArray(const MeshVertex *vertices, int vertexCount, const uint16_t *indices,
                             int indexCount) {
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);

    GLuint vertexBuffer = 0;
    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(MeshVertex) * vertexCount, vertices, GL_STATIC_DRAW);

    GLuint indexBuffer = 0;
    glGenBuffers(1, &indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16_t) * indexCount, indices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                          (const void *)offsetof(MeshVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                          (const void *)offsetof(MeshVertex, normal));
    glBindVertexArray(0);
    return vertexArray;
}

GLuint CreateOverlayVertexArray(GLuint *outBuffer) {
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);

    glGenBuffers(1, outBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, *outBuffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                          (const void *)offsetof(GizmoVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                          (const void *)offsetof(GizmoVertex, rgba));
    glBindVertexArray(0);
    return vertexArray;
}

void CheckFramebuffer(GLuint framebuffer, const char *label) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "%s framebuffer incomplete: 0x%x\n", label, status);
        abort();
    }
}

} // namespace

struct RendererState {
    GLuint geometryProgram;
    GLuint aoProgram;
    GLuint lightingProgram;
    GLuint fxaaProgram;
    GLuint overlayProgram;

    GLuint cubeVertexArray;
    GLuint planeVertexArray;
    GLuint fullscreenVertexArray; // empty: the fullscreen triangle comes from gl_VertexID
    GLuint overlayLineVertexArray;
    GLuint overlayTriVertexArray;
    GLuint overlayLineBuffer;
    GLuint overlayTriBuffer;

    GLuint gBufferFramebuffer;
    GLuint aoFramebuffer;
    GLuint litFramebuffer;
    GLuint gNormalTexture;
    GLuint gAlbedoTexture;
    GLuint sceneDepthRenderbuffer;
    GLuint aoRawTexture;
    GLuint litColorTexture;
    GLuint noiseTexture;
    uint32_t screenWidth;
    uint32_t screenHeight;
    uint32_t aoWidth;
    uint32_t aoHeight;

    float aoKernel[kAoKernelSize][4];

    RendererCamera camera;
    int debugMode;
    bool fxaaEnabled;

    GizmoVertex overlayLines[kMaxOverlayLineVertices];
    GizmoVertex overlayTris[kMaxOverlayTriVertices];
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
    if (state->gNormalTexture != 0 && state->screenWidth == width &&
        state->screenHeight == height) {
        return;
    }
    state->screenWidth = width;
    state->screenHeight = height;

    GLuint oldTextures[4] = {state->gNormalTexture, state->gAlbedoTexture, state->aoRawTexture,
                             state->litColorTexture};
    glDeleteTextures(4, oldTextures);
    glDeleteRenderbuffers(1, &state->sceneDepthRenderbuffer);

    state->gNormalTexture = CreateTexture(GL_RGBA16F, width, height, GL_RGBA, GL_FLOAT, GL_NEAREST,
                                          GL_CLAMP_TO_EDGE, nullptr);
    state->gAlbedoTexture = CreateTexture(GL_RGBA16F, width, height, GL_RGBA, GL_FLOAT, GL_NEAREST,
                                          GL_CLAMP_TO_EDGE, nullptr);
    glGenRenderbuffers(1, &state->sceneDepthRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, state->sceneDepthRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, width, height);

    state->aoWidth = (width + 1) / 2;
    state->aoHeight = (height + 1) / 2;
    state->aoRawTexture = CreateTexture(GL_R8, state->aoWidth, state->aoHeight, GL_RED,
                                        GL_UNSIGNED_BYTE, GL_LINEAR, GL_CLAMP_TO_EDGE, nullptr);
    state->litColorTexture = CreateTexture(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                                           GL_LINEAR, GL_CLAMP_TO_EDGE, nullptr);

    glBindFramebuffer(GL_FRAMEBUFFER, state->gBufferFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           state->gNormalTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           state->gAlbedoTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              state->sceneDepthRenderbuffer);
    GLenum drawBuffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, drawBuffers);
    CheckFramebuffer(state->gBufferFramebuffer, "g-buffer");

    glBindFramebuffer(GL_FRAMEBUFFER, state->aoFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           state->aoRawTexture, 0);
    CheckFramebuffer(state->aoFramebuffer, "ao");

    glBindFramebuffer(GL_FRAMEBUFFER, state->litFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           state->litColorTexture, 0);
    CheckFramebuffer(state->litFramebuffer, "lit color");
}

} // namespace

RendererState *RendererInit(Arena *arena, GpuContext *gpu, float drawableWidth,
                            float drawableHeight) {
    (void)gpu;
    RendererState *state = ArenaPushStruct(arena, RendererState);

    theme::Rgba background = theme::ToFloat(theme::ViewportBackground);
    theme::Rgba meshBase = theme::ToFloat(theme::MeshBase);
    char prelude[512];
    snprintf(prelude, sizeof(prelude), kFragmentPreludeFormat, background.r, background.g,
             background.b, meshBase.r, meshBase.g, meshBase.b);

    state->geometryProgram =
        CreatePassProgram(prelude, kGeometryVertex, kGeometryFragment, "geometry");
    state->aoProgram = CreatePassProgram(prelude, kFullscreenVertex, kAoFragment, "ao");
    state->lightingProgram =
        CreatePassProgram(prelude, kFullscreenVertex, kLightingFragment, "lighting");
    state->fxaaProgram = CreatePassProgram(prelude, kFullscreenVertex, kFxaaFragment, "fxaa");
    state->overlayProgram =
        CreatePassProgram(prelude, kOverlayVertex, kOverlayFragment, "overlay");

    // Texture units match the Metal backend's texture indices.
    SetSamplerUnit(state->aoProgram, "normalTexture", 1);
    SetSamplerUnit(state->aoProgram, "noiseTexture", 2);
    SetSamplerUnit(state->lightingProgram, "normalTexture", 1);
    SetSamplerUnit(state->lightingProgram, "aoTexture", 2);
    SetSamplerUnit(state->lightingProgram, "albedoTexture", 3);
    SetSamplerUnit(state->fxaaProgram, "litTexture", 0);

    state->cubeVertexArray =
        CreateMeshVertexArray(kCubeVertices, kCubeVertexCount, kCubeIndices, kCubeIndexCount);
    state->planeVertexArray =
        CreateMeshVertexArray(kPlaneVertices, kPlaneVertexCount, kPlaneIndices, kPlaneIndexCount);
    glGenVertexArrays(1, &state->fullscreenVertexArray);
    state->overlayLineVertexArray = CreateOverlayVertexArray(&state->overlayLineBuffer);
    state->overlayTriVertexArray = CreateOverlayVertexArray(&state->overlayTriBuffer);

    glGenFramebuffers(1, &state->gBufferFramebuffer);
    glGenFramebuffers(1, &state->aoFramebuffer);
    glGenFramebuffers(1, &state->litFramebuffer);

    float noiseTexels[kAoNoiseSize * kAoNoiseSize * 4];
    BuildAoSamples(state->aoKernel, noiseTexels);
    state->noiseTexture = CreateTexture(GL_RGBA32F, kAoNoiseSize, kAoNoiseSize, GL_RGBA, GL_FLOAT,
                                        GL_NEAREST, GL_REPEAT, noiseTexels);

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

void EncodeGeometryPass(RendererState *renderer, const RendererSceneView *view) {
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->gBufferFramebuffer);
    glViewport(0, 0, renderer->screenWidth, renderer->screenHeight);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW); // the negated clip-space y reverses the mesh's CCW winding

    // w = 1.0 marks "background"; real geometry writes its negative view-space Z.
    theme::Rgba gBufferClear = theme::ToFloat(theme::GBufferClear);
    float normalClear[4] = {gBufferClear.r, gBufferClear.g, gBufferClear.b, 1.0f};
    float albedoClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float depthClear = 1.0f;
    glClearBufferfv(GL_COLOR, 0, normalClear);
    glClearBufferfv(GL_COLOR, 1, albedoClear);
    glClearBufferfv(GL_DEPTH, 0, &depthClear);

    GLuint program = renderer->geometryProgram;
    glUseProgram(program);
    GLint modelViewProjectionLocation = glGetUniformLocation(program, "modelViewProjection");
    GLint modelViewLocation = glGetUniformLocation(program, "modelView");
    GLint baseColorLocation = glGetUniformLocation(program, "baseColor");

    static SceneMeshView meshViews[kMaxMeshRenderers];
    int meshCount = SceneMeshRenderers(view->scene, meshViews, kMaxMeshRenderers);

    theme::Rgba meshBaseColor = theme::ToFloat(theme::MeshBase);
    theme::Rgba meshSelectedColor = theme::ToFloat(theme::MeshSelected);

    for (int i = 0; i < meshCount; ++i) {
        const SceneMeshView &object = meshViews[i];

        Mat4 modelViewProjection = Mat4Multiply(renderer->camera.viewProjection, object.model);
        Mat4 modelView = Mat4Multiply(renderer->camera.view, object.model);
        bool selected = SceneSelectionContains(
            view->scene, SelectionItem{SelectionKind_Entity, (uint32_t)object.entity});
        const theme::Rgba &albedo = selected ? meshSelectedColor : meshBaseColor;

        glUniformMatrix4fv(modelViewProjectionLocation, 1, GL_FALSE, modelViewProjection.m);
        glUniformMatrix4fv(modelViewLocation, 1, GL_FALSE, modelView.m);
        glUniform4f(baseColorLocation, albedo.r, albedo.g, albedo.b, 1.0f);

        bool isCube = object.mesh == MeshId_Cube;
        glBindVertexArray(isCube ? renderer->cubeVertexArray : renderer->planeVertexArray);
        glDrawElements(GL_TRIANGLES, isCube ? kCubeIndexCount : kPlaneIndexCount,
                       GL_UNSIGNED_SHORT, nullptr);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

void EncodeAoPass(RendererState *renderer) {
    GLuint program = renderer->aoProgram;
    glUseProgram(program);
    PassTarget target = {renderer->aoFramebuffer,
                         {0, 0, (GLsizei)renderer->aoWidth, (GLsizei)renderer->aoHeight},
                         -1.0f};
    BindPassTarget(target, program);

    glUniformMatrix4fv(glGetUniformLocation(program, "projection"), 1, GL_FALSE,
                       renderer->camera.projection.m);
    glUniform4fv(glGetUniformLocation(program, "sampleOffsets"), kAoKernelSize,
                 &renderer->aoKernel[0][0]);
    glUniform4f(glGetUniformLocation(program, "params0"), kAoRadius, kAoBias, kAoPower, 0.0f);
    glUniform4f(glGetUniformLocation(program, "params1"), (float)renderer->aoWidth,
                (float)renderer->aoHeight, 0.0f, 0.0f);
    BindTexture(1, renderer->gNormalTexture);
    BindTexture(2, renderer->noiseTexture);

    glBindVertexArray(renderer->fullscreenVertexArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// `target` may be the full drawable while the scene only fills the content
// region; its viewport is the region to write.
void EncodeLightingPass(RendererState *renderer, PassTarget target) {
    GLuint program = renderer->lightingProgram;
    glUseProgram(program);
    BindPassTarget(target, program);

    Vec3 lightView = Mat4TransformDirection(renderer->camera.view, kLightDirectionWorld);
    glUniform4f(glGetUniformLocation(program, "lightDirectionView"), lightView.x, lightView.y,
                lightView.z, 0.0f);
    glUniform4f(glGetUniformLocation(program, "misc"), (float)renderer->debugMode,
                1.0f / (float)renderer->aoWidth, 1.0f / (float)renderer->aoHeight, 0.0f);
    BindTexture(1, renderer->gNormalTexture);
    BindTexture(2, renderer->aoRawTexture);
    BindTexture(3, renderer->gAlbedoTexture);

    glBindVertexArray(renderer->fullscreenVertexArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void EncodeFxaaPass(RendererState *renderer, PassTarget target) {
    GLuint program = renderer->fxaaProgram;
    glUseProgram(program);
    BindPassTarget(target, program);

    glUniform2f(glGetUniformLocation(program, "inverseScreenSize"),
                1.0f / (float)renderer->screenWidth, 1.0f / (float)renderer->screenHeight);
    BindTexture(0, renderer->litColorTexture);

    glBindVertexArray(renderer->fullscreenVertexArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void UploadOverlayVertices(GLuint buffer, const GizmoVertex *vertices, int capacity, int count) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GizmoVertex) * capacity, nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GizmoVertex) * count, vertices);
}

// Unlit, alpha-blended, no depth test: the gizmo and icons always read on top
// of the lit scene, which is what an editor overlay wants.
void EncodeOverlayPass(RendererState *renderer, const RendererSceneView *view, PassTarget target) {
    int lineCount = 0;
    int triCount = 0;
    BuildOverlayGeometry(&renderer->camera, view, renderer->overlayLines, renderer->overlayTris,
                         &lineCount, &triCount);
    if (lineCount == 0 && triCount == 0) {
        return;
    }

    GLuint program = renderer->overlayProgram;
    glUseProgram(program);
    BindPassTarget(target, program);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUniformMatrix4fv(glGetUniformLocation(program, "viewProjection"), 1, GL_FALSE,
                       renderer->camera.viewProjection.m);

    if (triCount > 0) {
        UploadOverlayVertices(renderer->overlayTriBuffer, renderer->overlayTris,
                              kMaxOverlayTriVertices, triCount);
        glBindVertexArray(renderer->overlayTriVertexArray);
        glDrawArrays(GL_TRIANGLES, 0, triCount);
    }
    if (lineCount > 0) {
        UploadOverlayVertices(renderer->overlayLineBuffer, renderer->overlayLines,
                              kMaxOverlayLineVertices, lineCount);
        glBindVertexArray(renderer->overlayLineVertexArray);
        glDrawArrays(GL_LINES, 0, lineCount);
    }
    glDisable(GL_BLEND);
}

} // namespace

void RendererRender(RendererState *renderer, const RendererSceneView *view,
                    RenderTarget *targetPtr) {
    RenderTarget target = *targetPtr;
    bool aoEnabled = renderer->debugMode != 2;
    bool fxaaEnabled = renderer->fxaaEnabled;

    PassTarget litColor = {renderer->litFramebuffer,
                           {0, 0, (GLsizei)renderer->screenWidth, (GLsizei)renderer->screenHeight},
                           -1.0f};
    // Pixel-align the origin and overdraw the extent by a few pixels: the final
    // composite must never leave a sub-pixel sliver between the viewport and a
    // docked panel. GL's viewport origin is bottom-left, so the top edge is
    // pinned at the flipped content origin and the overdraw extends downward,
    // under the panels (or past the drawable edge).
    GLsizei contentWidth = (GLsizei)renderer->screenWidth + 4;
    GLsizei contentHeight = (GLsizei)renderer->screenHeight + 4;
    GLint contentTop = (GLint)floorf(renderer->camera.contentOriginY);
    PassTarget drawable = {target.framebuffer,
                           {(GLint)floorf(renderer->camera.contentOriginX),
                            target.height - contentTop - contentHeight, contentWidth,
                            contentHeight},
                           1.0f};

    EncodeGeometryPass(renderer, view);
    if (aoEnabled) {
        EncodeAoPass(renderer);
    }
    // With FXAA on, the gizmo and icons composite into the lit image before the
    // FXAA resolve, so their edges are antialiased along with the geometry.
    if (fxaaEnabled) {
        EncodeLightingPass(renderer, litColor);
        EncodeOverlayPass(renderer, view, litColor);
        EncodeFxaaPass(renderer, drawable);
    } else {
        EncodeLightingPass(renderer, drawable);
        EncodeOverlayPass(renderer, view, drawable);
    }
}

// Apple's GL (4.1 on Metal) reads 0 from both GL_TIMESTAMP and GL_TIME_ELAPSED
// queries, so this backend has no GPU time to report.
RendererPassTimings RendererLastFrameTimings(const RendererState *renderer) {
    (void)renderer;
    return RendererPassTimings{};
}
