#include "renderer_common.h"

#include <cmath>
#include <cstdlib>

#include "theme.h"

// clang-format off
const MeshVertex kCubeVertices[kCubeVertexCount] = {
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

const uint16_t kCubeIndices[kCubeIndexCount] = {
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
const MeshVertex kPlaneVertices[kPlaneVertexCount] = {
    {{-1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-1.0f, 0.0f,  1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 1.0f, 0.0f,  1.0f}, {0.0f, 1.0f, 0.0f}},
    {{ 1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
};

const uint16_t kPlaneIndices[kPlaneIndexCount] = {
    0, 1, 2, 0, 2, 3,
};
// clang-format on

namespace {

constexpr float kFovYRadians = 60.0f * (float)M_PI / 180.0f;

// On-screen size the gizmo arms and the entity icons hold as the camera
// dollies, in content-viewport pixels.
constexpr float kGizmoPixelSize = 80.0f;
constexpr float kIconPixelSize = 40.0f;

// Orbit-camera feel. Tuned by hand, not measured against a specific
// trackpad — adjust here if a gesture feels inverted or too fast/slow.
constexpr float kPanSensitivity = 0.0025f;  // world units per point, per unit of distance
constexpr float kOrbitSensitivity = 0.006f; // radians per point
constexpr float kMinCameraDistance = 2.5f;
constexpr float kMaxCameraDistance = 60.0f;
constexpr float kMaxCameraPitch = 1.5f; // radians; keeps the view short of the poles

float Clamp(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

// A linear congruential generator rather than rand(): the C library's sequence
// differs between platforms, which would leave macOS and Windows sampling AO
// through different kernels and break image parity.
float RandomUnit(unsigned int *state) {
    *state = *state * 1664525u + 1013904223u;
    return (float)(*state >> 8) / (float)(1u << 24);
}

// Screen-space right/up axes of the orbit camera, derived algebraically from
// yaw/pitch rather than from a second Mat4LookAt.
Vec3 CameraRight(const RendererCamera *camera) {
    return Vec3{cosf(camera->yaw), 0.0f, -sinf(camera->yaw)};
}

Vec3 CameraUp(const RendererCamera *camera) {
    return Vec3{
        -sinf(camera->pitch) * sinf(camera->yaw),
        cosf(camera->pitch),
        -sinf(camera->pitch) * cosf(camera->yaw),
    };
}

// Looks at the target with world-up (0, 1, 0).
void UpdateViewMatrices(RendererCamera *camera) {
    Vec3 up = {0.0f, 1.0f, 0.0f};
    camera->view = Mat4LookAt(CameraEye(camera), camera->target, up);
    camera->viewProjection = Mat4Multiply(camera->projection, camera->view);
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

} // namespace

RendererCamera CameraCreate() {
    // Derive the initial orbit parameters from the original fixed eye/target
    // so the starting view is unchanged from before camera controls existed.
    RendererCamera camera = {};
    Vec3 initialEye = {0.0f, 3.5f, 12.0f};
    camera.target = Vec3{0.0f, 0.0f, 0.0f};
    Vec3 offset = {initialEye.x - camera.target.x, initialEye.y - camera.target.y,
                   initialEye.z - camera.target.z};
    camera.distance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    camera.yaw = atan2f(offset.x, offset.z);
    camera.pitch = asinf(offset.y / camera.distance);
    return camera;
}

void CameraSetContentRect(RendererCamera *camera, float originX, float originY, float width,
                          float height) {
    camera->contentOriginX = originX;
    camera->contentOriginY = originY;
    camera->contentWidth = width;
    camera->contentHeight = height;
    camera->projection = Mat4Perspective(kFovYRadians, width / height, 0.1f, 100.0f);
    UpdateViewMatrices(camera);
}

void CameraUpdate(RendererCamera *camera, FrameInput input) {
    camera->yaw -= input.orbitYaw * kOrbitSensitivity;
    camera->pitch =
        Clamp(camera->pitch + input.orbitPitch * kOrbitSensitivity, -kMaxCameraPitch, kMaxCameraPitch);
    camera->distance =
        Clamp(camera->distance * (1.0f - input.zoomDelta), kMinCameraDistance, kMaxCameraDistance);

    Vec3 right = CameraRight(camera);
    Vec3 up = CameraUp(camera);

    float panScale = kPanSensitivity * camera->distance;
    camera->target.x += (-right.x * input.panX + up.x * input.panY) * panScale;
    camera->target.y += (-right.y * input.panX + up.y * input.panY) * panScale;
    camera->target.z += (-right.z * input.panX + up.z * input.panY) * panScale;

    UpdateViewMatrices(camera);
}

// eye = target + distance * sphericalDirection(yaw, pitch). yaw is measured
// from +Z toward +X.
Vec3 CameraEye(const RendererCamera *camera) {
    Vec3 direction = {
        cosf(camera->pitch) * sinf(camera->yaw),
        sinf(camera->pitch),
        cosf(camera->pitch) * cosf(camera->yaw),
    };
    return Vec3Add(camera->target, Vec3Scale(direction, camera->distance));
}

float CameraGizmoScale(const RendererCamera *camera, Vec3 pivot) {
    return GizmoWorldScale(pivot, CameraEye(camera), kFovYRadians, camera->contentHeight,
                           kGizmoPixelSize);
}

Ray CameraScreenPointToRay(const RendererCamera *camera, float screenX, float screenY) {
    float viewportX = screenX - camera->contentOriginX;
    float viewportY = screenY - camera->contentOriginY;
    float ndcX = 2.0f * viewportX / camera->contentWidth - 1.0f;
    float ndcY = 1.0f - 2.0f * viewportY / camera->contentHeight;

    // Unproject two points on this pixel's line and use the segment between
    // them as the direction.
    Mat4 inverseViewProjection = Mat4Inverse(camera->viewProjection);
    Vec3 nearPoint = Mat4TransformPoint(inverseViewProjection, Vec3{ndcX, ndcY, 0.0f});
    Vec3 farPoint = Mat4TransformPoint(inverseViewProjection, Vec3{ndcX, ndcY, 1.0f});

    Ray ray;
    ray.origin = CameraEye(camera);
    ray.dir = Vec3Normalize(Vec3Sub(farPoint, nearPoint));
    return ray;
}

void BuildAoSamples(float kernel[kAoKernelSize][4],
                    float noiseTexels[kAoNoiseSize * kAoNoiseSize * 4]) {
    unsigned int random = 1;
    for (int i = 0; i < kAoKernelSize; ++i) {
        Vec3 sample = {
            RandomUnit(&random) * 2.0f - 1.0f,
            RandomUnit(&random) * 2.0f - 1.0f,
            RandomUnit(&random),
        };
        float length = sqrtf(sample.x * sample.x + sample.y * sample.y + sample.z * sample.z);
        sample = {sample.x / length, sample.y / length, sample.z / length};

        float scale = (float)i / (float)kAoKernelSize;
        scale = 0.1f + 0.9f * scale * scale;

        kernel[i][0] = sample.x * scale;
        kernel[i][1] = sample.y * scale;
        kernel[i][2] = sample.z * scale;
        kernel[i][3] = 0.0f;
    }

    for (int i = 0; i < kAoNoiseSize * kAoNoiseSize; ++i) {
        noiseTexels[i * 4 + 0] = RandomUnit(&random) * 2.0f - 1.0f;
        noiseTexels[i * 4 + 1] = RandomUnit(&random) * 2.0f - 1.0f;
        noiseTexels[i * 4 + 2] = 0.0f;
        noiseTexels[i * 4 + 3] = RandomUnit(&random);
    }
}

void BuildOverlayGeometry(const RendererCamera *camera, const RendererSceneView *view,
                          GizmoVertex *lines, GizmoVertex *tris, int *outLineCount,
                          int *outTriCount) {
    GizmoMeshBuilder builder = {};
    builder.lines = lines;
    builder.lineCapacity = kMaxOverlayLineVertices;
    builder.tris = tris;
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

    Vec3 eye = CameraEye(camera);
    Vec3 right = CameraRight(camera);
    Vec3 up = CameraUp(camera);

    // One call per icon: the billboard size is distance-dependent, so each
    // icon needs its own world size to hold a constant size on screen.
    for (int i = 0; i < iconCount; ++i) {
        float iconWorldSize = GizmoWorldScale(icons[i].position, eye, kFovYRadians,
                                              camera->contentHeight, kIconPixelSize);
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
                   CameraGizmoScale(camera, view->gizmoPivot), highlighted, gizmoColors);
    }

    *outLineCount = builder.lineCount;
    *outTriCount = builder.triCount;
}
