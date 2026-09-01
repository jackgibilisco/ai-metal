#pragma once

#include <cmath>

struct Vec3 {
    float x, y, z;
};

inline Vec3 Vec3Add(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Vec3Sub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Vec3Scale(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
inline Vec3 Vec3MulComponents(Vec3 a, Vec3 b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Vec3Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Vec3Cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Vec3Length(Vec3 a) { return sqrtf(Vec3Dot(a, a)); }

inline Vec3 Vec3Normalize(Vec3 a) {
    float length = Vec3Length(a);
    if (length <= 1e-8f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    return Vec3Scale(a, 1.0f / length);
}

// Unit quaternion, column-vector convention matching the Mat4 helpers:
// QuatMul(a, b) applies b first, then a, like Mat4Multiply(A, B).
struct Quat {
    float x, y, z, w;
};

// Column-major 4x4, matching the layout Metal Shading Language expects
// for float4x4 when the bytes are copied straight into a uniform buffer.
struct Mat4 {
    float m[16];
};

inline Mat4 Mat4Identity() {
    Mat4 result = {};
    result.m[0] = 1.0f;
    result.m[5] = 1.0f;
    result.m[10] = 1.0f;
    result.m[15] = 1.0f;
    return result;
}

inline Mat4 Mat4Multiply(const Mat4 &a, const Mat4 &b) {
    Mat4 result = {};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            result.m[col * 4 + row] = sum;
        }
    }
    return result;
}

inline Mat4 Mat4Translation(Vec3 t) {
    Mat4 result = Mat4Identity();
    result.m[12] = t.x;
    result.m[13] = t.y;
    result.m[14] = t.z;
    return result;
}

inline Mat4 Mat4RotationX(float radians) {
    Mat4 result = Mat4Identity();
    float c = cosf(radians);
    float s = sinf(radians);
    result.m[5] = c;
    result.m[6] = s;
    result.m[9] = -s;
    result.m[10] = c;
    return result;
}

inline Mat4 Mat4RotationY(float radians) {
    Mat4 result = Mat4Identity();
    float c = cosf(radians);
    float s = sinf(radians);
    result.m[0] = c;
    result.m[2] = -s;
    result.m[8] = s;
    result.m[10] = c;
    return result;
}

inline Mat4 Mat4RotationZ(float radians) {
    Mat4 result = Mat4Identity();
    float c = cosf(radians);
    float s = sinf(radians);
    result.m[0] = c;
    result.m[1] = s;
    result.m[4] = -s;
    result.m[5] = c;
    return result;
}

inline Mat4 Mat4Scale(Vec3 s) {
    Mat4 result = Mat4Identity();
    result.m[0] = s.x;
    result.m[5] = s.y;
    result.m[10] = s.z;
    return result;
}

// Blender's default ('XYZ') Euler order: intrinsic rotation X, then Y, then
// Z, equivalent to the fixed-axis matrix product Rz * Ry * Rx applied to a
// column vector.
inline Mat4 Mat4EulerXYZ(Vec3 radians) {
    Mat4 rotation = Mat4Multiply(Mat4RotationY(radians.y), Mat4RotationX(radians.x));
    rotation = Mat4Multiply(Mat4RotationZ(radians.z), rotation);
    return rotation;
}

inline Mat4 Mat4Perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    Mat4 result = {};
    float yScale = 1.0f / tanf(fovYRadians * 0.5f);
    float xScale = yScale / aspect;
    float zRange = farZ - nearZ;

    result.m[0] = xScale;
    result.m[5] = yScale;
    result.m[10] = -(farZ + nearZ) / zRange;
    result.m[11] = -1.0f;
    result.m[14] = -(2.0f * farZ * nearZ) / zRange;
    return result;
}

inline Vec3 Mat4TransformDirection(const Mat4 &m, Vec3 direction) {
    return Vec3{
        m.m[0] * direction.x + m.m[4] * direction.y + m.m[8] * direction.z,
        m.m[1] * direction.x + m.m[5] * direction.y + m.m[9] * direction.z,
        m.m[2] * direction.x + m.m[6] * direction.y + m.m[10] * direction.z,
    };
}

inline Mat4 Mat4LookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 zAxis = {eye.x - target.x, eye.y - target.y, eye.z - target.z};
    float zLen = sqrtf(zAxis.x * zAxis.x + zAxis.y * zAxis.y + zAxis.z * zAxis.z);
    zAxis = {zAxis.x / zLen, zAxis.y / zLen, zAxis.z / zLen};

    Vec3 xAxis = {
        up.y * zAxis.z - up.z * zAxis.y,
        up.z * zAxis.x - up.x * zAxis.z,
        up.x * zAxis.y - up.y * zAxis.x,
    };
    float xLen = sqrtf(xAxis.x * xAxis.x + xAxis.y * xAxis.y + xAxis.z * xAxis.z);
    xAxis = {xAxis.x / xLen, xAxis.y / xLen, xAxis.z / xLen};

    Vec3 yAxis = {
        zAxis.y * xAxis.z - zAxis.z * xAxis.y,
        zAxis.z * xAxis.x - zAxis.x * xAxis.z,
        zAxis.x * xAxis.y - zAxis.y * xAxis.x,
    };

    Mat4 result = Mat4Identity();
    result.m[0] = xAxis.x;
    result.m[4] = xAxis.y;
    result.m[8] = xAxis.z;
    result.m[1] = yAxis.x;
    result.m[5] = yAxis.y;
    result.m[9] = yAxis.z;
    result.m[2] = zAxis.x;
    result.m[6] = zAxis.y;
    result.m[10] = zAxis.z;
    result.m[12] = -(xAxis.x * eye.x + xAxis.y * eye.y + xAxis.z * eye.z);
    result.m[13] = -(yAxis.x * eye.x + yAxis.y * eye.y + yAxis.z * eye.z);
    result.m[14] = -(zAxis.x * eye.x + zAxis.y * eye.y + zAxis.z * eye.z);
    return result;
}

inline Quat QuatIdentity() { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

inline Quat QuatFromAxisAngle(Vec3 axis, float radians) {
    Vec3 unit = Vec3Normalize(axis);
    float half = radians * 0.5f;
    float s = sinf(half);
    return Quat{unit.x * s, unit.y * s, unit.z * s, cosf(half)};
}

// Applies b first, then a (matches Mat4Multiply ordering).
inline Quat QuatMul(Quat a, Quat b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline Quat QuatNormalize(Quat q) {
    float length = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length <= 1e-8f) {
        return QuatIdentity();
    }
    float inv = 1.0f / length;
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline Quat QuatConjugate(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

inline Vec3 QuatRotate(Quat q, Vec3 v) {
    Vec3 u = Vec3{q.x, q.y, q.z};
    Vec3 t = Vec3Scale(Vec3Cross(u, v), 2.0f);
    return Vec3Add(v, Vec3Add(Vec3Scale(t, q.w), Vec3Cross(u, t)));
}

inline Mat4 QuatToMat4(Quat q) {
    Quat n = QuatNormalize(q);
    float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
    float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
    float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;

    Mat4 result = Mat4Identity();
    result.m[0] = 1.0f - 2.0f * (yy + zz);
    result.m[1] = 2.0f * (xy + wz);
    result.m[2] = 2.0f * (xz - wy);
    result.m[4] = 2.0f * (xy - wz);
    result.m[5] = 1.0f - 2.0f * (xx + zz);
    result.m[6] = 2.0f * (yz + wx);
    result.m[8] = 2.0f * (xz + wy);
    result.m[9] = 2.0f * (yz - wx);
    result.m[10] = 1.0f - 2.0f * (xx + yy);
    return result;
}

// Extracts the rotation from the upper-left 3x3 of a (rotation-only) matrix.
// Inverse of QuatToMat4; used by the .blend importer, which builds rotation
// as a Mat4.
inline Quat QuatFromMat4(const Mat4 &m) {
    float m00 = m.m[0], m10 = m.m[1], m20 = m.m[2];
    float m01 = m.m[4], m11 = m.m[5], m21 = m.m[6];
    float m02 = m.m[8], m12 = m.m[9], m22 = m.m[10];
    float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return QuatNormalize(q);
}

inline Quat QuatSlerp(Quat a, Quat b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b = Quat{-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }
    if (dot > 0.9995f) {
        return QuatNormalize(Quat{
            a.x + t * (b.x - a.x),
            a.y + t * (b.y - a.y),
            a.z + t * (b.z - a.z),
            a.w + t * (b.w - a.w),
        });
    }
    float theta0 = acosf(dot);
    float theta = theta0 * t;
    float sinTheta0 = sinf(theta0);
    float scaleA = cosf(theta) - dot * sinf(theta) / sinTheta0;
    float scaleB = sinf(theta) / sinTheta0;
    return Quat{
        scaleA * a.x + scaleB * b.x,
        scaleA * a.y + scaleB * b.y,
        scaleA * a.z + scaleB * b.z,
        scaleA * a.w + scaleB * b.w,
    };
}
