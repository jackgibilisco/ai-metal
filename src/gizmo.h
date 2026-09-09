#pragma once

// Pure C++ gizmo math: screen-constant sizing, handle hit-testing, drag ->
// TransformDelta, and the line/triangle geometry the renderer draws. No
// Metal, no AppKit. Depends on scene.h for the shared Ray / TransformDelta /
// ToolMode types (and, for the tool actions, the command + create API).
//
// Split of concerns:
//   - sizing / hit-test / drag / geometry: no SceneState access, headless
//     testable with only the headers.
//   - ToolAddAudioSource / ToolAddListener: submit scene commands.
//
// Colours are passed in by the renderer (which reads them from
// theme.h); this file hard-codes no colour.

#include "math3d.h"
#include "scene.h" // Ray, PickResult, TransformDelta, ToolMode, command + create API

// Which part of the gizmo the pointer is over or dragging. Axis handles map
// to translate-along / rotate-about / scale-along by the active ToolMode.
// Plane handles are translate-only; Uniform (the centre box) is scale-only.
enum GizmoHandle {
    GizmoHandle_None,
    GizmoHandle_AxisX,
    GizmoHandle_AxisY,
    GizmoHandle_AxisZ,
    GizmoHandle_PlaneXY,
    GizmoHandle_PlaneYZ,
    GizmoHandle_PlaneZX,
    GizmoHandle_Uniform,
};

// True for the three tool modes that show a gizmo (everything but Select).
inline bool ToolModeHasGizmo(ToolMode mode) { return mode != ToolMode_Select; }

// --- sizing ---------------------------------------------------------------

// World length that projects to about `desiredPixels` of viewport height at
// `pivot`'s view depth, so the gizmo (and the billboards) hold a constant
// on-screen size as the camera dollies. `viewportHeightPixels` is the
// content-region height.
float GizmoWorldScale(Vec3 pivot, Vec3 cameraEye, float fovYRadians,
                      float viewportHeightPixels, float desiredPixels);

// --- hit-testing --------------------------------------------------------------

// Handle under `ray`, or GizmoHandle_None. `mode` selects which handles
// exist (None for ToolMode_Select). `scale` is GizmoWorldScale's result.
GizmoHandle GizmoHitTest(ToolMode mode, Vec3 pivot, float scale, Ray ray);

// --- dragging -----------------------------------------------------------------

// In-progress drag. Trivially copyable, holds no pointers; app.cpp keeps
// one in AppState across frames. Treat the fields as opaque.
struct GizmoDrag {
    ToolMode mode;
    GizmoHandle handle;
    Vec3 pivot;
    float scale;        // frozen at drag start
    Vec3 axis;          // unit world axis for AxisX/Y/Z drags
    Vec3 planeNormal;   // unit; the plane a translate/rotate drag runs in
    Vec3 anchorPoint;   // world point where the drag began
    float anchorAngle;  // rotate: pointer angle in the drag plane at start
    float accumAngle;   // rotate: unwrapped angle carried across frames
    bool active;
};

// Begin a drag on `handle` (must not be None). Captures the anchor so later
// updates are absolute (no drift).
GizmoDrag GizmoBeginDrag(ToolMode mode, GizmoHandle handle, Vec3 pivot,
                         float scale, Ray ray);

// Cumulative change from the drag start for the current pointer ray. The
// pivot is carried in the result; feed it to ScenePreviewTransformDrag each
// frame. Returns a zero delta when `drag->active` is false.
TransformDelta GizmoUpdateDrag(GizmoDrag *drag, Ray ray);

// Final delta for the current ray; also clears `drag->active`. Feed the
// result to SceneEndTransformDrag (the one undoable command).
TransformDelta GizmoEndDrag(GizmoDrag *drag, Ray ray);

// --- geometry for the renderer ---------------------------------------------

struct GizmoVertex {
    float pos[3];
    float rgba[4];
};

// The renderer hands in two scratch arrays; the builders append line-list
// and triangle-list vertices and report the counts. Overflow is clamped,
// not asserted (the renderer sizes generously).
struct GizmoMeshBuilder {
    GizmoVertex *lines;
    int lineCount;
    int lineCapacity;
    GizmoVertex *tris;
    int triCount;
    int triCapacity;
};

// RGBA the renderer copies out of theme.h. Field names are theme-agnostic;
// the renderer maps theme.h's GizmoAxisX/Y/Z + GizmoActive onto them.
struct GizmoColors {
    float axisX[4];
    float axisY[4];
    float axisZ[4];
    float highlight[4]; // theme.h GizmoActive; used for the hovered handle
    float uniform[4];   // centre box in Scale mode when not hovered
};

// Axis handles + the ring/box for `mode`, at `pivot`, sized by `scale`. The
// `hovered` handle is emitted in `colors.highlight`. No-op for
// ToolMode_Select.
void GizmoBuild(GizmoMeshBuilder *out, ToolMode mode, Vec3 pivot, float scale,
                GizmoHandle hovered, const GizmoColors &colors);

// A viewport icon for an entity with no mesh. `kind`: 0 audio source,
// 1 inactive listener, 2 active listener. When `selected` and kind 0, two
// wireframe spheres are appended at `minDistance` / `maxDistance`.
struct IconInstance {
    Vec3 position;
    int kind;
    bool selected;
    float minDistance;
    float maxDistance;
};

// theme.h: AudioSourceIcon / ListenerIcon / ListenerActive / SelectionOutline
// (+ DistanceSphere).
struct IconColors {
    float source[4];
    float listener[4];
    float activeListener[4];
    float selectedOutline[4];
    float distanceSphere[4];
};

// `cameraRight` / `cameraUp` orient the billboards toward the camera;
// `iconWorldSize` is a GizmoWorldScale-style constant-screen-size value.
void GizmoBuildIcons(GizmoMeshBuilder *out, const IconInstance *icons, int count,
                     Vec3 cameraRight, Vec3 cameraUp, float iconWorldSize,
                     const IconColors &colors);

// --- placement helper -----------------------------------------------------

// Intersect `ray` with the horizontal plane y = planeHeight. Returns false
// when the ray is parallel to the plane or points away from it.
bool RayGroundIntersect(Ray ray, float planeHeight, Vec3 *outPoint);

// --- tool actions (submit scene commands) -----------------------------

// Create an audio-source entity on the ground plane (y = 0) under the
// cursor, or at `cameraFallbackPoint` when `cursorRay` misses the ground.
// Goes through one create command; returns the new entity id.
EntityId ToolAddAudioSource(SceneState *scene, Ray cursorRay,
                            Vec3 cameraFallbackPoint);

// Same placement. The new listener is made active only when the scene had no
// listeners before this call.
EntityId ToolAddListener(SceneState *scene, Ray cursorRay,
                         Vec3 cameraFallbackPoint);
