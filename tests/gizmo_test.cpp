// Headless test for the pure-C++ gizmo math (src/gizmo.cpp). No Metal, no
// scene pools: the four SceneState functions the tool actions call are faked
// below and record their arguments.
//
// Build + run:
//   clang++ -std=c++17 -Wall -Wextra -I src tests/gizmo_test.cpp src/gizmo.cpp \
//     -o build/gizmo_test && build/gizmo_test

#include <cmath>
#include <cstdio>

#include "gizmo.h"

// --- fake SceneState surface --------------------------------------------------

static EntityId g_nextEntityId = 1;
static EntityId g_lastCreated = 0;
static EntityKind g_lastKind = EntityKind_Mesh;
static Transform g_lastTransform;
static EntityId g_activeListener = 0;
static int g_createCount = 0;
static int g_setTransformCount = 0;

EntityId SceneCreateEntityAt(SceneState *, EntityKind kind, const char *, Transform transform) {
    g_lastCreated = g_nextEntityId++;
    g_lastKind = kind;
    g_lastTransform = transform;
    ++g_createCount;
    return g_lastCreated;
}
EntityId SceneCreateEntity(SceneState *scene, EntityKind kind, const char *name) {
    return SceneCreateEntityAt(scene, kind, name, TransformIdentity());
}
void SceneSetEntityTransform(SceneState *, EntityId, Transform next) {
    g_lastTransform = next;
    ++g_setTransformCount;
}
EntityId SceneActiveListenerEntity(const SceneState *) { return g_activeListener; }
void SceneSetActiveListener(SceneState *, EntityId listenerEntity) {
    g_activeListener = listenerEntity;
}

// --- tiny assert harness -----------------------------------------------------

static int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                         \
    do {                                                                              \
        float diff_ = (a) - (b);                                                      \
        if (diff_ < 0.0f) diff_ = -diff_;                                             \
        if (diff_ > (eps)) {                                                          \
            std::printf("FAIL %s:%d  %s ~= %s   (%.6f vs %.6f)\n", __FILE__, __LINE__, \
                        #a, #b, (double)(a), (double)(b));                            \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

static Ray MakeRay(Vec3 origin, Vec3 through) {
    Ray r;
    r.origin = origin;
    r.dir = Vec3Normalize(Vec3Sub(through, origin));
    return r;
}

static const Vec3 kOrigin = {0.0f, 0.0f, 0.0f};

// --- tests -----------------------------------------------------------------

static void TestWorldScale() {
    Vec3 eye = {0.0f, 0.0f, 0.0f};
    float fov = 1.0f;
    float height = 800.0f;
    float near_ = GizmoWorldScale(Vec3{0, 0, -5}, eye, fov, height, 90.0f);
    float far_ = GizmoWorldScale(Vec3{0, 0, -20}, eye, fov, height, 90.0f);
    CHECK(far_ > near_);
    CHECK_NEAR(far_ / near_, 4.0f, 1e-3f);

    float expected = (2.0f * 10.0f * tanf(0.5f) / height) * 90.0f;
    CHECK_NEAR(GizmoWorldScale(Vec3{0, 0, -10}, eye, fov, height, 90.0f), expected, 1e-4f);

    // Degenerate: pivot on the eye must not divide by zero.
    CHECK(GizmoWorldScale(eye, eye, fov, height, 90.0f) >= 0.0f);
}

static void TestHitTestSelectIsNone() {
    Ray r = MakeRay(Vec3{0.5f, 0.0f, 5.0f}, Vec3{0.5f, 0.0f, 0.0f});
    CHECK(GizmoHitTest(ToolMode_Select, kOrigin, 1.0f, r) == GizmoHandle_None);
}

static void TestHitTestTranslateAxes() {
    // Straight down each axis handle from +Z.
    CHECK(GizmoHitTest(ToolMode_Translate, kOrigin, 1.0f,
                       MakeRay(Vec3{0.5f, 0.0f, 5.0f}, Vec3{0.5f, 0.0f, 0.0f})) == GizmoHandle_AxisX);
    CHECK(GizmoHitTest(ToolMode_Translate, kOrigin, 1.0f,
                       MakeRay(Vec3{0.0f, 0.5f, 5.0f}, Vec3{0.0f, 0.5f, 0.0f})) == GizmoHandle_AxisY);
    // Z handle: look along +X at a point on +Z.
    CHECK(GizmoHitTest(ToolMode_Translate, kOrigin, 1.0f,
                       MakeRay(Vec3{5.0f, 0.0f, 0.5f}, Vec3{0.0f, 0.0f, 0.5f})) == GizmoHandle_AxisZ);
    // Into empty space.
    CHECK(GizmoHitTest(ToolMode_Translate, kOrigin, 1.0f,
                       MakeRay(Vec3{5.0f, 5.0f, 5.0f}, Vec3{5.0f, 5.0f, -5.0f})) == GizmoHandle_None);
}

static void TestHitTestRotateRings() {
    // Ray hits z=0 plane at radius 1 -> the Z ring (XY plane).
    CHECK(GizmoHitTest(ToolMode_Rotate, kOrigin, 1.0f,
                       MakeRay(Vec3{1.0f, 0.0f, 5.0f}, Vec3{1.0f, 0.0f, 0.0f})) == GizmoHandle_AxisZ);
    // Ray hits x=0 plane at radius 1 -> the X ring (YZ plane).
    CHECK(GizmoHitTest(ToolMode_Rotate, kOrigin, 1.0f,
                       MakeRay(Vec3{5.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f})) == GizmoHandle_AxisX);
    // Well inside every ring -> no hit.
    CHECK(GizmoHitTest(ToolMode_Rotate, kOrigin, 1.0f,
                       MakeRay(Vec3{0.1f, 0.0f, 5.0f}, Vec3{0.1f, 0.0f, 0.0f})) == GizmoHandle_None);
}

static void TestHitTestScaleUniform() {
    // Ray through the pivot -> centre box.
    CHECK(GizmoHitTest(ToolMode_Scale, kOrigin, 1.0f,
                       MakeRay(Vec3{0.0f, 0.0f, 5.0f}, kOrigin)) == GizmoHandle_Uniform);
    // Offset onto the X handle.
    CHECK(GizmoHitTest(ToolMode_Scale, kOrigin, 1.0f,
                       MakeRay(Vec3{0.6f, 0.0f, 5.0f}, Vec3{0.6f, 0.0f, 0.0f})) == GizmoHandle_AxisX);
}

static void TestDragTranslate() {
    GizmoDrag drag = GizmoBeginDrag(ToolMode_Translate, GizmoHandle_AxisX, kOrigin, 1.0f,
                                    MakeRay(Vec3{0.0f, 0.0f, 5.0f}, kOrigin));
    TransformDelta d = GizmoUpdateDrag(&drag, MakeRay(Vec3{2.0f, 0.0f, 5.0f}, Vec3{2.0f, 0.0f, 0.0f}));
    CHECK_NEAR(d.translate.x, 2.0f, 1e-3f);
    CHECK_NEAR(d.translate.y, 0.0f, 1e-4f);
    CHECK_NEAR(d.translate.z, 0.0f, 1e-4f);
    CHECK_NEAR(d.scale.x, 1.0f, 1e-6f);
    CHECK_NEAR(d.rotate.w, 1.0f, 1e-6f);

    // Dragging back the other way.
    TransformDelta back = GizmoUpdateDrag(&drag, MakeRay(Vec3{-1.5f, 0.0f, 5.0f}, Vec3{-1.5f, 0.0f, 0.0f}));
    CHECK_NEAR(back.translate.x, -1.5f, 1e-3f);
}

static void TestDragRotate() {
    GizmoDrag drag = GizmoBeginDrag(ToolMode_Rotate, GizmoHandle_AxisZ, kOrigin, 1.0f,
                                    MakeRay(Vec3{1.0f, 0.0f, 5.0f}, Vec3{1.0f, 0.0f, 0.0f}));
    // Pointer sweeps from (1,0,0) to (0,1,0): +90 deg about +Z.
    TransformDelta d = GizmoUpdateDrag(&drag, MakeRay(Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 1.0f, 0.0f}));
    float halfAngle = 3.14159265f / 4.0f;
    CHECK_NEAR(d.rotate.z, sinf(halfAngle), 1e-3f);
    CHECK_NEAR(d.rotate.w, cosf(halfAngle), 1e-3f);
    CHECK_NEAR(d.rotate.x, 0.0f, 1e-4f);
    CHECK_NEAR(d.rotate.y, 0.0f, 1e-4f);

    // Continue the sweep past 180 deg: accumulation must not wrap.
    GizmoUpdateDrag(&drag, MakeRay(Vec3{-1.0f, 0.0f, 5.0f}, Vec3{-1.0f, 0.0f, 0.0f}));
    TransformDelta d3 = GizmoUpdateDrag(&drag, MakeRay(Vec3{0.0f, -1.0f, 5.0f}, Vec3{0.0f, -1.0f, 0.0f}));
    // 270 deg -> half-angle 135 deg -> w = cos(135) < 0.
    CHECK(d3.rotate.w < 0.0f);
}

static void TestDragScaleAxis() {
    GizmoDrag drag = GizmoBeginDrag(ToolMode_Scale, GizmoHandle_AxisY, kOrigin, 1.0f,
                                    MakeRay(Vec3{0.0f, 0.0f, 5.0f}, kOrigin));
    // Move the pointer +1 along Y with scale reference length 1 -> factor 2.
    TransformDelta d = GizmoUpdateDrag(&drag, MakeRay(Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 1.0f, 0.0f}));
    CHECK_NEAR(d.scale.y, 2.0f, 1e-3f);
    CHECK_NEAR(d.scale.x, 1.0f, 1e-6f);
    CHECK_NEAR(d.scale.z, 1.0f, 1e-6f);
}

static void TestDragUniformScale() {
    GizmoDrag drag = GizmoBeginDrag(ToolMode_Scale, GizmoHandle_Uniform, kOrigin, 1.0f,
                                    MakeRay(Vec3{2.0f, 0.0f, 5.0f}, Vec3{2.0f, 0.0f, 0.0f}));
    TransformDelta d = GizmoUpdateDrag(&drag, MakeRay(Vec3{4.0f, 0.0f, 5.0f}, Vec3{4.0f, 0.0f, 0.0f}));
    CHECK_NEAR(d.scale.x, 2.0f, 1e-3f);
    CHECK_NEAR(d.scale.y, 2.0f, 1e-3f);
    CHECK_NEAR(d.scale.z, 2.0f, 1e-3f);
}

static void TestEndDragClearsActive() {
    GizmoDrag drag = GizmoBeginDrag(ToolMode_Translate, GizmoHandle_AxisX, kOrigin, 1.0f,
                                    MakeRay(Vec3{0.0f, 0.0f, 5.0f}, kOrigin));
    CHECK(drag.active);
    GizmoEndDrag(&drag, MakeRay(Vec3{1.0f, 0.0f, 5.0f}, Vec3{1.0f, 0.0f, 0.0f}));
    CHECK(!drag.active);
    TransformDelta afterEnd = GizmoUpdateDrag(&drag, MakeRay(Vec3{9.0f, 0.0f, 5.0f}, Vec3{9.0f, 0.0f, 0.0f}));
    CHECK_NEAR(afterEnd.translate.x, 0.0f, 1e-6f);
}

static void TestRayGround() {
    Vec3 hit;
    CHECK(RayGroundIntersect(MakeRay(Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}), 0.0f, &hit));
    CHECK_NEAR(hit.x, 0.0f, 1e-4f);
    CHECK_NEAR(hit.y, 0.0f, 1e-4f);

    Ray angled = MakeRay(Vec3{0.0f, 10.0f, 0.0f}, Vec3{10.0f, 0.0f, 0.0f});
    CHECK(RayGroundIntersect(angled, 0.0f, &hit));
    CHECK_NEAR(hit.x, 10.0f, 1e-3f);
    CHECK_NEAR(hit.y, 0.0f, 1e-3f);

    // Parallel to the plane, and pointing away from it.
    Ray parallel = {Vec3{0.0f, 5.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
    CHECK(!RayGroundIntersect(parallel, 0.0f, &hit));
    Ray away = {Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}};
    CHECK(!RayGroundIntersect(away, 0.0f, &hit));
}

static void TestBuildGeometry() {
    GizmoVertex lines[4096];
    GizmoVertex tris[4096];
    GizmoColors colors = {};
    colors.axisX[3] = colors.axisY[3] = colors.axisZ[3] = 1.0f;
    colors.highlight[3] = colors.uniform[3] = 1.0f;

    GizmoMeshBuilder b = {lines, 0, 4096, tris, 0, 4096};
    GizmoBuild(&b, ToolMode_Select, kOrigin, 1.0f, GizmoHandle_None, colors);
    CHECK(b.lineCount == 0 && b.triCount == 0);

    b = GizmoMeshBuilder{lines, 0, 4096, tris, 0, 4096};
    GizmoBuild(&b, ToolMode_Translate, kOrigin, 1.0f, GizmoHandle_AxisX, colors);
    CHECK(b.lineCount >= 6); // 3 axis shafts
    CHECK(b.triCount > 0);   // arrow heads
    // Hovered axis emitted in the highlight colour.
    CHECK_NEAR(b.lines[0].rgba[3], 1.0f, 1e-6f);

    b = GizmoMeshBuilder{lines, 0, 4096, tris, 0, 4096};
    GizmoBuild(&b, ToolMode_Rotate, kOrigin, 1.0f, GizmoHandle_None, colors);
    CHECK(b.lineCount > 0);
    CHECK(b.triCount == 0);

    b = GizmoMeshBuilder{lines, 0, 4096, tris, 0, 4096};
    GizmoBuild(&b, ToolMode_Scale, kOrigin, 1.0f, GizmoHandle_Uniform, colors);
    CHECK(b.lineCount > 0); // axis shafts + tip boxes + centre box
}

static void TestBuildIcons() {
    GizmoVertex lines[8192];
    GizmoVertex tris[8192];
    IconColors colors = {};
    colors.source[3] = colors.listener[3] = colors.activeListener[3] = 1.0f;
    colors.selectedOutline[3] = colors.distanceSphere[3] = 1.0f;

    IconInstance one = {};
    one.position = Vec3{1.0f, 0.0f, 0.0f};
    one.kind = 0;
    one.selected = false;

    GizmoMeshBuilder b = {lines, 0, 8192, tris, 0, 8192};
    GizmoBuildIcons(&b, &one, 1, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 0.5f, colors);
    CHECK(b.triCount == 81);  // backdrop disc (72) + body quad (6) + cone (3)
    CHECK(b.lineCount == 74); // ring (48) + two arc waves (12 + 14)
    int unselectedLines = b.lineCount;

    one.selected = true;
    one.minDistance = 1.0f;
    one.maxDistance = 5.0f;
    b = GizmoMeshBuilder{lines, 0, 8192, tris, 0, 8192};
    GizmoBuildIcons(&b, &one, 1, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 0.5f, colors);
    CHECK(b.triCount == 81); // selection does not add fill
    CHECK(b.lineCount == unselectedLines + 8 + 2 * 288); // 4-seg outline + two wire spheres
}

static void TestOverflowClamped() {
    GizmoVertex lines[8];
    GizmoVertex tris[8];
    GizmoColors colors = {};
    GizmoMeshBuilder b = {lines, 0, 8, tris, 0, 8};
    GizmoBuild(&b, ToolMode_Rotate, kOrigin, 1.0f, GizmoHandle_None, colors);
    CHECK(b.lineCount <= 8);
    CHECK(b.triCount <= 8);
}

static void TestToolAddAudioSource() {
    g_createCount = 0;
    g_setTransformCount = 0;
    Ray cursor = MakeRay(Vec3{3.0f, 10.0f, -2.0f}, Vec3{3.0f, 0.0f, -2.0f});
    EntityId id = ToolAddAudioSource(nullptr, cursor, Vec3{0, 0, 0});
    CHECK(EntityIdValid(id));
    CHECK(g_lastKind == EntityKind_AudioSource);
    CHECK(g_createCount == 1);
    // Placement rides on the create command, so there is exactly one undo step.
    CHECK(g_setTransformCount == 0);
    CHECK_NEAR(g_lastTransform.position.x, 3.0f, 1e-3f);
    CHECK_NEAR(g_lastTransform.position.y, 0.0f, 1e-3f);
    CHECK_NEAR(g_lastTransform.position.z, -2.0f, 1e-3f);
}

static void TestToolAddAudioSourceFallback() {
    // Cursor ray parallel to the ground -> the fallback point is used.
    Ray cursor = {Vec3{0.0f, 4.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
    ToolAddAudioSource(nullptr, cursor, Vec3{7.0f, 1.0f, 8.0f});
    CHECK_NEAR(g_lastTransform.position.x, 7.0f, 1e-4f);
    CHECK_NEAR(g_lastTransform.position.y, 1.0f, 1e-4f);
    CHECK_NEAR(g_lastTransform.position.z, 8.0f, 1e-4f);
}

static void TestToolAddListenerActivation() {
    g_activeListener = 0;
    Ray cursor = MakeRay(Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f});
    EntityId first = ToolAddListener(nullptr, cursor, Vec3{0, 0, 0});
    CHECK(g_lastKind == EntityKind_AudioListener);
    CHECK(g_activeListener == first); // first listener auto-activates

    EntityId second = ToolAddListener(nullptr, cursor, Vec3{0, 0, 0});
    CHECK(g_activeListener == first); // second does not steal active
    CHECK(second != first);
}

int main() {
    TestWorldScale();
    TestHitTestSelectIsNone();
    TestHitTestTranslateAxes();
    TestHitTestRotateRings();
    TestHitTestScaleUniform();
    TestDragTranslate();
    TestDragRotate();
    TestDragScaleAxis();
    TestDragUniformScale();
    TestEndDragClearsActive();
    TestRayGround();
    TestBuildGeometry();
    TestBuildIcons();
    TestOverflowClamped();
    TestToolAddAudioSource();
    TestToolAddAudioSourceFallback();
    TestToolAddListenerActivation();

    if (g_failures == 0) {
        std::printf("gizmo_test: all checks passed\n");
        return 0;
    }
    std::printf("gizmo_test: %d check(s) failed\n", g_failures);
    return 1;
}
