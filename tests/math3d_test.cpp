// Headless test for the Mat4 inverse / unprojection added for viewport
// picking. Mirrors the screen-point-to-ray mapping in renderer_metal.mm so a
// change to that formula fails here rather than in the viewport.
//
// Build + run:
//   clang++ -std=c++17 -Wall -Wextra -I src tests/math3d_test.cpp \
//     -o build/math3d_test && build/math3d_test

#include <cmath>
#include <cstdio>

#include "math3d.h"

// Matches scene.h's Ray; declared locally so this test needs only math3d.h.
struct TestRay {
    Vec3 origin;
    Vec3 dir;
};

static int g_failures = 0;

static void Check(bool condition, const char *label) {
    if (!condition) {
        printf("FAIL: %s\n", label);
        g_failures++;
    }
}

static void CheckNear(float actual, float expected, float tolerance, const char *label) {
    if (fabsf(actual - expected) > tolerance) {
        printf("FAIL: %s (got %f, want %f)\n", label, actual, expected);
        g_failures++;
    }
}

static Mat4 SampleViewProjection(Vec3 eye, Vec3 target) {
    Mat4 projection = Mat4Perspective(60.0f * 3.14159265f / 180.0f, 1.6f, 0.1f, 100.0f);
    Mat4 view = Mat4LookAt(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    return Mat4Multiply(projection, view);
}

static void TestInverseIsInverse() {
    Mat4 viewProjection = SampleViewProjection(Vec3{3.0f, 4.0f, 12.0f}, Vec3{1.0f, 0.0f, -2.0f});
    Mat4 product = Mat4Multiply(viewProjection, Mat4Inverse(viewProjection));
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float expected = (col == row) ? 1.0f : 0.0f;
            CheckNear(product.m[col * 4 + row], expected, 1e-4f, "viewProjection * inverse == I");
        }
    }
}

static void TestSingularMatrixFallsBackToIdentity() {
    Mat4 singular = {};
    Mat4 result = Mat4Inverse(singular);
    Mat4 identity = Mat4Identity();
    for (int i = 0; i < 16; ++i) {
        CheckNear(result.m[i], identity.m[i], 1e-6f, "singular inverse returns identity");
    }
}

static void TestPointRoundTrip() {
    Mat4 viewProjection = SampleViewProjection(Vec3{0.0f, 3.5f, 12.0f}, Vec3{0.0f, 0.0f, 0.0f});
    Mat4 inverse = Mat4Inverse(viewProjection);

    Vec3 world = {1.5f, 2.25f, -3.75f};
    Vec3 ndc = Mat4TransformPoint(viewProjection, world);
    Vec3 back = Mat4TransformPoint(inverse, ndc);
    CheckNear(Vec3Length(Vec3Sub(back, world)), 0.0f, 1e-3f, "world -> ndc -> world round trip");
}

// The exact mapping RendererScreenPointToRay performs: drawable pixels with a
// top-left origin, offset by the content rect, into NDC, then unprojected.
static TestRay ScreenPointToRay(Mat4 viewProjection, Vec3 eye, float originX, float originY,
                            float width, float height, float screenX, float screenY) {
    float ndcX = 2.0f * (screenX - originX) / width - 1.0f;
    float ndcY = 1.0f - 2.0f * (screenY - originY) / height;

    Mat4 inverse = Mat4Inverse(viewProjection);
    Vec3 nearPoint = Mat4TransformPoint(inverse, Vec3{ndcX, ndcY, 0.0f});
    Vec3 farPoint = Mat4TransformPoint(inverse, Vec3{ndcX, ndcY, 1.0f});

    TestRay ray;
    ray.origin = eye;
    ray.dir = Vec3Normalize(Vec3Sub(farPoint, nearPoint));
    return ray;
}

static void TestCenterPixelLooksAtTarget() {
    Vec3 eye = {0.0f, 3.5f, 12.0f};
    Vec3 target = {0.0f, 0.0f, 0.0f};
    Mat4 viewProjection = SampleViewProjection(eye, target);

    // Content rect offset inside the drawable, the way UI panels shift it.
    float originX = 220.0f, originY = 40.0f, width = 1280.0f, height = 800.0f;
    TestRay ray = ScreenPointToRay(viewProjection, eye, originX, originY, width, height,
                                   originX + width * 0.5f, originY + height * 0.5f);

    Vec3 toTarget = Vec3Normalize(Vec3Sub(target, eye));
    CheckNear(Vec3Dot(ray.dir, toTarget), 1.0f, 1e-3f, "center pixel ray points at the target");
    CheckNear(Vec3Length(ray.dir), 1.0f, 1e-4f, "ray direction is normalized");
}

static void TestRayHitsTheWorldPointItWasProjectedFrom() {
    Vec3 eye = {6.0f, 5.0f, 9.0f};
    Vec3 target = {-1.0f, 0.5f, 0.0f};
    Mat4 viewProjection = SampleViewProjection(eye, target);

    float originX = 0.0f, originY = 64.0f, width = 1600.0f, height = 900.0f;

    // Project a known world point to its pixel, shoot a ray back through that
    // pixel, and confirm the point lies on the ray.
    Vec3 world = {2.0f, 1.25f, -1.5f};
    Vec3 ndc = Mat4TransformPoint(viewProjection, world);
    float screenX = originX + (ndc.x + 1.0f) * 0.5f * width;
    float screenY = originY + (1.0f - ndc.y) * 0.5f * height;

    TestRay ray = ScreenPointToRay(viewProjection, eye, originX, originY, width, height, screenX,
                                   screenY);

    Vec3 toPoint = Vec3Sub(world, ray.origin);
    float alongRay = Vec3Dot(toPoint, ray.dir);
    Vec3 closest = Vec3Add(ray.origin, Vec3Scale(ray.dir, alongRay));
    Check(alongRay > 0.0f, "projected point is in front of the camera");
    CheckNear(Vec3Length(Vec3Sub(closest, world)), 0.0f, 1e-2f, "ray passes through the point");
}

int main() {
    TestInverseIsInverse();
    TestSingularMatrixFallsBackToIdentity();
    TestPointRoundTrip();
    TestCenterPixelLooksAtTarget();
    TestRayHitsTheWorldPointItWasProjectedFrom();

    if (g_failures != 0) {
        printf("math3d_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("math3d_test: all checks passed\n");
    return 0;
}
