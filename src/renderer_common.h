#pragma once

// Graphics-API-free pieces every renderer backend shares (renderer_metal.mm,
// renderer_gl.cpp): mesh data, the orbit camera, SSAO sample data, and the
// gizmo/icon overlay geometry. Only GPU encoding lives in the backends, so
// they agree on everything else by construction.

#include <cstdint>

#include "frame_input.h"
#include "gizmo.h"
#include "math3d.h"
#include "renderer.h"

struct MeshVertex {
    float position[3];
    float normal[3];
};

constexpr int kCubeVertexCount = 24;
constexpr int kCubeIndexCount = 36;
constexpr int kPlaneVertexCount = 4;
constexpr int kPlaneIndexCount = 6;
extern const MeshVertex kCubeVertices[kCubeVertexCount];
extern const uint16_t kCubeIndices[kCubeIndexCount];
extern const MeshVertex kPlaneVertices[kPlaneVertexCount];
extern const uint16_t kPlaneIndices[kPlaneIndexCount];

// Per-frame overlay geometry (gizmo + icons). Sized for the worst realistic
// frame; GizmoMeshBuilder clamps rather than overruns.
constexpr int kMaxOverlayLineVertices = 32768;
constexpr int kMaxOverlayTriVertices = 16384;

// Screen-space ambient occlusion. Radius is in world units — the demo cubes
// are 1 unit across, so this is roughly "darken where surfaces are within
// half a cube of each other". Bias fights depth-precision self-occlusion;
// power sharpens the falloff. Tune with the F3+O debug views.
constexpr float kAoRadius = 0.6f;
constexpr float kAoBias = 0.025f;
constexpr float kAoPower = 1.6f;
constexpr int kAoKernelSize = 32;
constexpr int kAoNoiseSize = 4;

constexpr Vec3 kLightDirectionWorld = {0.4f, 1.0f, 0.6f};

// The orbit camera and the content viewport it projects into (drawable
// pixels, top-left origin).
struct RendererCamera {
    Vec3 target;
    float distance;
    float yaw;
    float pitch;
    float contentOriginX;
    float contentOriginY;
    float contentWidth;
    float contentHeight;
    Mat4 projection;
    Mat4 view;
    Mat4 viewProjection;
};

// The startup orbit. Matrices stay zero until CameraSetContentRect.
RendererCamera CameraCreate();
void CameraSetContentRect(RendererCamera *camera, float originX, float originY, float width,
                          float height);
void CameraUpdate(RendererCamera *camera, FrameInput input);
Vec3 CameraEye(const RendererCamera *camera);
float CameraGizmoScale(const RendererCamera *camera, Vec3 pivot);
Ray CameraScreenPointToRay(const RendererCamera *camera, float screenX, float screenY);

// Seeds rand() and fills both from one stream, so every backend and every run
// gets identical samples.
void BuildAoSamples(float kernel[kAoKernelSize][4],
                    float noiseTexels[kAoNoiseSize * kAoNoiseSize * 4]);

// Fills `lines` / `tris` (capacities kMaxOverlayLineVertices /
// kMaxOverlayTriVertices) with this frame's gizmo handles and entity icons.
void BuildOverlayGeometry(const RendererCamera *camera, const RendererSceneView *view,
                          GizmoVertex *lines, GizmoVertex *tris, int *outLineCount,
                          int *outTriCount);
