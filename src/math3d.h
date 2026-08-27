#pragma once

#include <cmath>

struct Vec3 {
    float x, y, z;
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
