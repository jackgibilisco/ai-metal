#include "gizmo.h"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

// All lengths are multiples of the screen-constant `scale` from
// GizmoWorldScale, so the whole gizmo holds a fixed on-screen size.
constexpr float kAxisInnerStart = 0.16f; // gap at the centre for the uniform box
constexpr float kAxisLength = 1.0f;      // handle tip distance
constexpr float kAxisPickRadius = 0.13f; // ray-to-axis distance that counts as a hit
constexpr float kRingRadius = 1.0f;
constexpr float kRingPickBand = 0.14f;   // |hitRadius - ringRadius| that counts as a hit
constexpr float kUniformPickRadius = 0.17f;
constexpr float kUniformBoxHalf = 0.10f;
constexpr float kArrowRadius = 0.09f;
constexpr float kArrowLength = 0.28f;
constexpr float kScaleBoxHalf = 0.08f;
constexpr int kRingSegments = 48;
constexpr int kConeSegments = 12;

constexpr float kMinScaleFactor = 0.01f;
constexpr float kMaxScaleFactor = 100.0f;

// Scene icons, as fractions of the icon's on-screen size. The disc stops short
// of the quad edge and the glyph is shrunk so both keep a visible margin.
constexpr float kIconDiscRadius = 0.46f;
constexpr float kIconGlyphScale = 0.8f;

Vec3 HandleAxis(GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle_AxisX: return Vec3{1.0f, 0.0f, 0.0f};
        case GizmoHandle_AxisY: return Vec3{0.0f, 1.0f, 0.0f};
        case GizmoHandle_AxisZ: return Vec3{0.0f, 0.0f, 1.0f};
        default: return Vec3{0.0f, 0.0f, 0.0f};
    }
}

// Two orthonormal in-plane axes derived from `normal` alone, so the same
// basis comes back every frame of a rotate drag.
void PlaneBasis(Vec3 normal, Vec3 *tangent, Vec3 *bitangent) {
    Vec3 seed = (fabsf(normal.x) < 0.9f) ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    Vec3 t = Vec3Normalize(Vec3Sub(seed, Vec3Scale(normal, Vec3Dot(seed, normal))));
    *tangent = t;
    *bitangent = Vec3Cross(normal, t);
}

float AngleInPlane(Vec3 center, Vec3 normal, Vec3 point) {
    Vec3 tangent, bitangent;
    PlaneBasis(normal, &tangent, &bitangent);
    Vec3 rel = Vec3Sub(point, center);
    return atan2f(Vec3Dot(rel, bitangent), Vec3Dot(rel, tangent));
}

float WrapPi(float angle) {
    angle = fmodf(angle, kTwoPi);
    if (angle > kPi) angle -= kTwoPi;
    if (angle < -kPi) angle += kTwoPi;
    return angle;
}

float ClampScaleFactor(float factor) {
    if (factor < kMinScaleFactor) return kMinScaleFactor;
    if (factor > kMaxScaleFactor) return kMaxScaleFactor;
    return factor;
}

// Intersect ray (origin `o`, unit dir `d`) with the plane through
// `planePoint` with unit normal `n`. Rejects parallel rays and hits behind
// the origin.
bool RayPlane(Vec3 o, Vec3 d, Vec3 planePoint, Vec3 n, Vec3 *outHit) {
    float denom = Vec3Dot(d, n);
    if (fabsf(denom) < 1e-6f) return false;
    float t = Vec3Dot(Vec3Sub(planePoint, o), n) / denom;
    if (t < 0.0f) return false;
    if (outHit) *outHit = Vec3Add(o, Vec3Scale(d, t));
    return true;
}

// Squared distance between ray (origin `o`, unit dir `d`, t >= 0) and
// segment [a, b]. `outT` receives the ray parameter at the closest point.
float ClosestRaySegmentDistanceSq(Vec3 o, Vec3 d, Vec3 a, Vec3 b, float *outT) {
    Vec3 segDir = Vec3Sub(b, a);
    Vec3 w = Vec3Sub(o, a);
    float segLenSq = Vec3Dot(segDir, segDir);
    float dDotSeg = Vec3Dot(d, segDir);
    float dDotW = Vec3Dot(d, w);
    float segDotW = Vec3Dot(segDir, w);

    float denom = segLenSq - dDotSeg * dDotSeg; // 1*segLenSq - (d.seg)^2, d is unit
    float s;
    if (denom < 1e-8f) {
        s = 0.0f;
    } else {
        s = (segDotW - dDotW * dDotSeg) / denom;
    }
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;

    float t = Vec3Dot(d, Vec3Add(Vec3Scale(w, -1.0f), Vec3Scale(segDir, s)));
    if (t < 0.0f) t = 0.0f;

    Vec3 pointOnRay = Vec3Add(o, Vec3Scale(d, t));
    Vec3 pointOnSeg = Vec3Add(a, Vec3Scale(segDir, s));
    Vec3 diff = Vec3Sub(pointOnRay, pointOnSeg);
    if (outT) *outT = t;
    return Vec3Dot(diff, diff);
}

// A plane that contains `axis` and faces the ray as squarely as possible,
// so projecting the ray hit back onto the axis is numerically stable.
Vec3 PlaneNormalForAxisDrag(Vec3 axis, Vec3 rayDir) {
    Vec3 perp = Vec3Sub(rayDir, Vec3Scale(axis, Vec3Dot(rayDir, axis)));
    float length = Vec3Length(perp);
    if (length < 1e-4f) {
        Vec3 alt = (fabsf(axis.x) < 0.9f) ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        perp = Vec3Sub(alt, Vec3Scale(axis, Vec3Dot(alt, axis)));
        length = Vec3Length(perp);
    }
    return Vec3Scale(perp, 1.0f / length);
}

// Closest point on the ray to `p`, clamped to t >= 0.
Vec3 ClosestRayPoint(Vec3 o, Vec3 d, Vec3 p) {
    float t = Vec3Dot(Vec3Sub(p, o), d);
    if (t < 0.0f) t = 0.0f;
    return Vec3Add(o, Vec3Scale(d, t));
}

// ---- mesh builder helpers ------------------------------------------------

void CopyColor(float *dst, const float *src) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

void PushLine(GizmoMeshBuilder *out, Vec3 a, Vec3 b, const float color[4]) {
    if (out->lineCount + 2 > out->lineCapacity) return;
    GizmoVertex *v = &out->lines[out->lineCount];
    v[0].pos[0] = a.x; v[0].pos[1] = a.y; v[0].pos[2] = a.z; CopyColor(v[0].rgba, color);
    v[1].pos[0] = b.x; v[1].pos[1] = b.y; v[1].pos[2] = b.z; CopyColor(v[1].rgba, color);
    out->lineCount += 2;
}

void PushTri(GizmoMeshBuilder *out, Vec3 a, Vec3 b, Vec3 c, const float color[4]) {
    if (out->triCount + 3 > out->triCapacity) return;
    GizmoVertex *v = &out->tris[out->triCount];
    v[0].pos[0] = a.x; v[0].pos[1] = a.y; v[0].pos[2] = a.z; CopyColor(v[0].rgba, color);
    v[1].pos[0] = b.x; v[1].pos[1] = b.y; v[1].pos[2] = b.z; CopyColor(v[1].rgba, color);
    v[2].pos[0] = c.x; v[2].pos[1] = c.y; v[2].pos[2] = c.z; CopyColor(v[2].rgba, color);
    out->triCount += 3;
}

void BuildRing(GizmoMeshBuilder *out, Vec3 center, Vec3 normal, float radius, const float color[4]) {
    Vec3 tangent, bitangent;
    PlaneBasis(normal, &tangent, &bitangent);
    Vec3 prev = Vec3Add(center, Vec3Scale(tangent, radius));
    for (int i = 1; i <= kRingSegments; ++i) {
        float a = (float)i / (float)kRingSegments * kTwoPi;
        Vec3 point = Vec3Add(center, Vec3Add(Vec3Scale(tangent, radius * cosf(a)),
                                             Vec3Scale(bitangent, radius * sinf(a))));
        PushLine(out, prev, point, color);
        prev = point;
    }
}

void BuildArrowHead(GizmoMeshBuilder *out, Vec3 tip, Vec3 axis, const float color[4], float sizeScale) {
    Vec3 baseCenter = Vec3Sub(tip, Vec3Scale(axis, kArrowLength * sizeScale));
    Vec3 tangent, bitangent;
    PlaneBasis(axis, &tangent, &bitangent);
    float radius = kArrowRadius * sizeScale;
    Vec3 prev = Vec3Add(baseCenter, Vec3Scale(tangent, radius));
    for (int i = 1; i <= kConeSegments; ++i) {
        float a = (float)i / (float)kConeSegments * kTwoPi;
        Vec3 point = Vec3Add(baseCenter, Vec3Add(Vec3Scale(tangent, radius * cosf(a)),
                                                 Vec3Scale(bitangent, radius * sinf(a))));
        PushTri(out, prev, point, tip, color);
        PushTri(out, prev, baseCenter, point, color);
        prev = point;
    }
}

void BuildBox(GizmoMeshBuilder *out, Vec3 center, float halfExtent, const float color[4]) {
    Vec3 v[8];
    for (int i = 0; i < 8; ++i) {
        v[i] = Vec3{center.x + ((i & 1) ? halfExtent : -halfExtent),
                    center.y + ((i & 2) ? halfExtent : -halfExtent),
                    center.z + ((i & 4) ? halfExtent : -halfExtent)};
    }
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int i = 0; i < 12; ++i) {
        PushLine(out, v[edges[i][0]], v[edges[i][1]], color);
    }
}

void BuildWireSphere(GizmoMeshBuilder *out, Vec3 center, float radius, const float color[4]) {
    if (radius <= 1e-4f) return;
    BuildRing(out, center, Vec3{1.0f, 0.0f, 0.0f}, radius, color);
    BuildRing(out, center, Vec3{0.0f, 1.0f, 0.0f}, radius, color);
    BuildRing(out, center, Vec3{0.0f, 0.0f, 1.0f}, radius, color);
}

Vec3 GroundOrFallback(Ray cursorRay, Vec3 fallback) {
    Vec3 hit;
    if (RayGroundIntersect(cursorRay, 0.0f, &hit)) return hit;
    return fallback;
}

EntityId CreateEntityAt(SceneState *scene, EntityKind kind, const char *name, Vec3 position) {
    Transform transform = TransformIdentity();
    transform.position = position;
    return SceneCreateEntityAt(scene, kind, name, transform);
}

} // namespace

// ---------------------------------------------------------------------------

float GizmoWorldScale(Vec3 pivot, Vec3 cameraEye, float fovYRadians,
                      float viewportHeightPixels, float desiredPixels) {
    float distance = Vec3Length(Vec3Sub(pivot, cameraEye));
    if (distance < 1e-4f) distance = 1e-4f;
    if (viewportHeightPixels < 1.0f) viewportHeightPixels = 1.0f;
    float worldHeightAtPivot = 2.0f * distance * tanf(fovYRadians * 0.5f);
    float worldPerPixel = worldHeightAtPivot / viewportHeightPixels;
    return worldPerPixel * desiredPixels;
}

GizmoHandle GizmoHitTest(ToolMode mode, Vec3 pivot, float scale, Ray ray) {
    if (!ToolModeHasGizmo(mode)) return GizmoHandle_None;
    Vec3 o = ray.origin;
    Vec3 d = ray.dir;
    const GizmoHandle axisHandles[3] = {GizmoHandle_AxisX, GizmoHandle_AxisY, GizmoHandle_AxisZ};

    if (mode == ToolMode_Rotate) {
        GizmoHandle best = GizmoHandle_None;
        float bestError = kRingPickBand * scale;
        for (int i = 0; i < 3; ++i) {
            Vec3 normal = HandleAxis(axisHandles[i]);
            Vec3 hit;
            if (!RayPlane(o, d, pivot, normal, &hit)) continue;
            float error = fabsf(Vec3Length(Vec3Sub(hit, pivot)) - kRingRadius * scale);
            if (error < bestError) {
                bestError = error;
                best = axisHandles[i];
            }
        }
        return best;
    }

    if (mode == ToolMode_Scale) {
        Vec3 closest = ClosestRayPoint(o, d, pivot);
        float radius = kUniformPickRadius * scale;
        if (Vec3Dot(Vec3Sub(closest, pivot), Vec3Sub(closest, pivot)) <= radius * radius) {
            return GizmoHandle_Uniform;
        }
    }

    GizmoHandle best = GizmoHandle_None;
    float bestT = 1e30f;
    float pickRadiusSq = (kAxisPickRadius * scale) * (kAxisPickRadius * scale);
    for (int i = 0; i < 3; ++i) {
        Vec3 axis = HandleAxis(axisHandles[i]);
        Vec3 a = Vec3Add(pivot, Vec3Scale(axis, kAxisInnerStart * scale));
        Vec3 b = Vec3Add(pivot, Vec3Scale(axis, kAxisLength * scale));
        float t;
        if (ClosestRaySegmentDistanceSq(o, d, a, b, &t) <= pickRadiusSq && t < bestT) {
            bestT = t;
            best = axisHandles[i];
        }
    }
    return best;
}

GizmoDrag GizmoBeginDrag(ToolMode mode, GizmoHandle handle, Vec3 pivot, float scale, Ray ray) {
    GizmoDrag drag = {};
    drag.mode = mode;
    drag.handle = handle;
    drag.pivot = pivot;
    drag.scale = scale;
    drag.accumAngle = 0.0f;
    drag.active = true;

    Vec3 o = ray.origin;
    Vec3 d = ray.dir;

    if (handle == GizmoHandle_Uniform) {
        drag.anchorPoint = ClosestRayPoint(o, d, pivot);
        return drag;
    }

    Vec3 axis = HandleAxis(handle);
    drag.axis = axis;

    if (mode == ToolMode_Rotate) {
        drag.planeNormal = axis;
        Vec3 hit;
        if (RayPlane(o, d, pivot, axis, &hit)) {
            drag.anchorPoint = hit;
            drag.anchorAngle = AngleInPlane(pivot, axis, hit);
        } else {
            drag.anchorPoint = pivot;
            drag.anchorAngle = 0.0f;
        }
        return drag;
    }

    drag.planeNormal = PlaneNormalForAxisDrag(axis, d);
    Vec3 hit;
    if (RayPlane(o, d, pivot, drag.planeNormal, &hit)) {
        float along = Vec3Dot(Vec3Sub(hit, pivot), axis);
        drag.anchorPoint = Vec3Add(pivot, Vec3Scale(axis, along));
    } else {
        drag.anchorPoint = pivot;
    }
    return drag;
}

TransformDelta GizmoUpdateDrag(GizmoDrag *drag, Ray ray) {
    TransformDelta delta = {};
    delta.rotate = QuatIdentity();
    delta.scale = Vec3{1.0f, 1.0f, 1.0f};
    delta.pivot = drag ? drag->pivot : Vec3{0.0f, 0.0f, 0.0f};
    delta.useCentroidPivot = true;
    if (!drag || !drag->active) return delta;

    Vec3 o = ray.origin;
    Vec3 d = ray.dir;

    if (drag->handle == GizmoHandle_Uniform) {
        Vec3 current = ClosestRayPoint(o, d, drag->pivot);
        float anchorDistance = Vec3Length(Vec3Sub(drag->anchorPoint, drag->pivot));
        float currentDistance = Vec3Length(Vec3Sub(current, drag->pivot));
        float factor = (anchorDistance > 1e-5f) ? (currentDistance / anchorDistance) : 1.0f;
        factor = ClampScaleFactor(factor);
        delta.scale = Vec3{factor, factor, factor};
        return delta;
    }

    if (drag->mode == ToolMode_Rotate) {
        Vec3 hit;
        if (!RayPlane(o, d, drag->pivot, drag->planeNormal, &hit)) return delta;
        float angle = AngleInPlane(drag->pivot, drag->planeNormal, hit);
        drag->accumAngle += WrapPi(angle - drag->anchorAngle);
        drag->anchorAngle = angle;
        delta.rotate = QuatFromAxisAngle(drag->axis, drag->accumAngle);
        return delta;
    }

    Vec3 hit;
    if (!RayPlane(o, d, drag->pivot, drag->planeNormal, &hit)) return delta;
    float along = Vec3Dot(Vec3Sub(hit, drag->pivot), drag->axis);
    float anchorAlong = Vec3Dot(Vec3Sub(drag->anchorPoint, drag->pivot), drag->axis);
    float displacement = along - anchorAlong;

    if (drag->mode == ToolMode_Scale) {
        float referenceLength = (drag->scale > 1e-4f) ? drag->scale : 1.0f;
        float factor = ClampScaleFactor(1.0f + displacement / referenceLength);
        Vec3 s = Vec3{1.0f, 1.0f, 1.0f};
        if (drag->handle == GizmoHandle_AxisX) s.x = factor;
        else if (drag->handle == GizmoHandle_AxisY) s.y = factor;
        else if (drag->handle == GizmoHandle_AxisZ) s.z = factor;
        delta.scale = s;
        return delta;
    }

    delta.translate = Vec3Scale(drag->axis, displacement);
    return delta;
}

TransformDelta GizmoEndDrag(GizmoDrag *drag, Ray ray) {
    TransformDelta delta = GizmoUpdateDrag(drag, ray);
    if (drag) drag->active = false;
    return delta;
}

void GizmoBuild(GizmoMeshBuilder *out, ToolMode mode, Vec3 pivot, float scale,
                GizmoHandle hovered, const GizmoColors &colors) {
    if (!ToolModeHasGizmo(mode)) return;

    const GizmoHandle axisHandles[3] = {GizmoHandle_AxisX, GizmoHandle_AxisY, GizmoHandle_AxisZ};
    const Vec3 axes[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    const float *axisColors[3] = {colors.axisX, colors.axisY, colors.axisZ};

    if (mode == ToolMode_Rotate) {
        for (int i = 0; i < 3; ++i) {
            const float *color = (hovered == axisHandles[i]) ? colors.highlight : axisColors[i];
            BuildRing(out, pivot, axes[i], kRingRadius * scale, color);
        }
        return;
    }

    for (int i = 0; i < 3; ++i) {
        const float *color = (hovered == axisHandles[i]) ? colors.highlight : axisColors[i];
        Vec3 start = Vec3Add(pivot, Vec3Scale(axes[i], kAxisInnerStart * scale));
        Vec3 tip = Vec3Add(pivot, Vec3Scale(axes[i], kAxisLength * scale));
        PushLine(out, start, tip, color);
        if (mode == ToolMode_Translate) {
            BuildArrowHead(out, tip, axes[i], color, scale);
        } else {
            BuildBox(out, tip, kScaleBoxHalf * scale, color);
        }
    }

    if (mode == ToolMode_Scale) {
        const float *color = (hovered == GizmoHandle_Uniform) ? colors.highlight : colors.uniform;
        BuildBox(out, pivot, kUniformBoxHalf * scale, color);
    }
}

// Icons are camera-facing billboards. (u, v) are fractions of iconWorldSize in
// the camera's right / up axes, roughly [-0.5, 0.5] around the entity origin.
Vec3 BillboardPoint(Vec3 center, Vec3 right, Vec3 up, float size, float u, float v) {
    Vec3 p = Vec3Add(center, Vec3Scale(right, u * size));
    return Vec3Add(p, Vec3Scale(up, v * size));
}

void IconQuad(GizmoMeshBuilder *out, Vec3 c, Vec3 r, Vec3 u, float s, float u0, float v0, float u1,
              float v1, const float color[4]) {
    Vec3 a = BillboardPoint(c, r, u, s, u0, v0);
    Vec3 b = BillboardPoint(c, r, u, s, u1, v0);
    Vec3 d = BillboardPoint(c, r, u, s, u1, v1);
    Vec3 e = BillboardPoint(c, r, u, s, u0, v1);
    PushTri(out, a, b, d, color);
    PushTri(out, a, d, e, color);
}

void IconArc(GizmoMeshBuilder *out, Vec3 c, Vec3 r, Vec3 u, float s, float cu, float cv,
             float radius, float a0, float a1, int segments, const float color[4]) {
    Vec3 prev =
        BillboardPoint(c, r, u, s, cu + cosf(a0) * radius, cv + sinf(a0) * radius);
    for (int i = 1; i <= segments; ++i) {
        float a = a0 + (a1 - a0) * (float)i / (float)segments;
        Vec3 cur = BillboardPoint(c, r, u, s, cu + cosf(a) * radius, cv + sinf(a) * radius);
        PushLine(out, prev, cur, color);
        prev = cur;
    }
}

// Camera-facing filled circle centred on the billboard, radius in the same
// [-0.5, 0.5] units as BillboardPoint.
void IconDisc(GizmoMeshBuilder *out, Vec3 c, Vec3 r, Vec3 u, float s, float radius, int segments,
              const float color[4]) {
    Vec3 prev = BillboardPoint(c, r, u, s, radius, 0.0f);
    for (int i = 1; i <= segments; ++i) {
        float a = kTwoPi * (float)i / (float)segments;
        Vec3 cur = BillboardPoint(c, r, u, s, cosf(a) * radius, sinf(a) * radius);
        PushTri(out, c, prev, cur, color);
        prev = cur;
    }
}

void GizmoBuildIcons(GizmoMeshBuilder *out, const IconInstance *icons, int count,
                     Vec3 cameraRight, Vec3 cameraUp, float iconWorldSize,
                     const IconColors &colors) {
    for (int i = 0; i < count; ++i) {
        const IconInstance &icon = icons[i];
        const float *fill = (icon.kind == 0)   ? colors.source
                            : (icon.kind == 2) ? colors.activeListener
                                               : colors.listener;
        Vec3 p = icon.position;
        Vec3 r = cameraRight;
        Vec3 u = cameraUp;
        float s = iconWorldSize;
        // Glyphs are drawn at a reduced size so they keep a clear margin
        // inside the ring; the coordinates below are in that glyph space.
        float g = iconWorldSize * kIconGlyphScale;

        IconDisc(out, p, r, u, s, kIconDiscRadius, 24, colors.backdrop);
        IconArc(out, p, r, u, s, 0.0f, 0.0f, kIconDiscRadius, 0.0f, kTwoPi, 24, fill);

        if (icon.kind == 0) {
            // Volume: rectangular body + cone opening to the right, two waves.
            IconQuad(out, p, r, u, g, -0.34f, -0.12f, -0.16f, 0.12f, fill);
            Vec3 coneTop = BillboardPoint(p, r, u, g, -0.16f, 0.30f);
            Vec3 coneBottom = BillboardPoint(p, r, u, g, -0.16f, -0.30f);
            Vec3 coneTip = BillboardPoint(p, r, u, g, 0.06f, 0.0f);
            PushTri(out, coneBottom, coneTip, coneTop, fill);
            IconArc(out, p, r, u, g, 0.06f, 0.0f, 0.20f, -0.9f, 0.9f, 6, fill);
            IconArc(out, p, r, u, g, 0.06f, 0.0f, 0.34f, -0.8f, 0.8f, 7, fill);
        } else {
            // Headphones: headband arc + two ear cups.
            IconArc(out, p, r, u, g, 0.0f, -0.06f, 0.34f, 0.30f, kPi - 0.30f, 12, fill);
            IconQuad(out, p, r, u, g, -0.40f, -0.30f, -0.24f, 0.06f, fill);
            IconQuad(out, p, r, u, g, 0.24f, -0.30f, 0.40f, 0.06f, fill);
        }

        if (icon.selected) {
            Vec3 bl = BillboardPoint(p, r, u, s, -0.5f, -0.5f);
            Vec3 br = BillboardPoint(p, r, u, s, 0.5f, -0.5f);
            Vec3 tr = BillboardPoint(p, r, u, s, 0.5f, 0.5f);
            Vec3 tl = BillboardPoint(p, r, u, s, -0.5f, 0.5f);
            PushLine(out, bl, br, colors.selectedOutline);
            PushLine(out, br, tr, colors.selectedOutline);
            PushLine(out, tr, tl, colors.selectedOutline);
            PushLine(out, tl, bl, colors.selectedOutline);
            if (icon.kind == 0) {
                BuildWireSphere(out, p, icon.minDistance, colors.distanceSphere);
                BuildWireSphere(out, p, icon.maxDistance, colors.distanceSphere);
            }
        }
    }
}

bool RayGroundIntersect(Ray ray, float planeHeight, Vec3 *outPoint) {
    return RayPlane(ray.origin, ray.dir, Vec3{0.0f, planeHeight, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                    outPoint);
}

EntityId ToolAddAudioSource(SceneState *scene, Ray cursorRay, Vec3 cameraFallbackPoint) {
    return CreateEntityAt(scene, EntityKind_AudioSource, "Audio Source",
                          GroundOrFallback(cursorRay, cameraFallbackPoint));
}

EntityId ToolAddListener(SceneState *scene, Ray cursorRay, Vec3 cameraFallbackPoint) {
    bool hadListener = EntityIdValid(SceneActiveListenerEntity(scene));
    EntityId id = CreateEntityAt(scene, EntityKind_AudioListener, "Listener",
                                 GroundOrFallback(cursorRay, cameraFallbackPoint));
    if (EntityIdValid(id) && !hadListener) {
        SceneSetActiveListener(scene, id);
    }
    return id;
}
