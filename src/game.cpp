#include "game.h"

#include "blend_file.h"

#include <cstdio>

namespace {

constexpr int kDemoCubeCount = 3;

constexpr int32_t kObjectTypeMesh = 1;         // Blender's OB_MESH
constexpr int32_t kRotationModeQuaternion = 0; // Blender's ROT_MODE_QUAT

// Blender is Z-up; this renderer is Y-up. Both matrices convert between the
// two spaces: (x, y, z)_blender -> (x, z, -y)_render. See PLAN.md ("Feature:
// File > Import File...") for the derivation.
constexpr Mat4 kBlenderToRenderUp = {{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1}};
constexpr Mat4 kBlenderToRenderUpTransposed = {{1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1}};

Vec3 ConvertPosition(Vec3 blenderPosition) {
    return Vec3{blenderPosition.x, blenderPosition.z, -blenderPosition.y};
}

Vec3 ConvertScale(Vec3 blenderScale) {
    return Vec3{blenderScale.x, blenderScale.z, blenderScale.y};
}

Mat4 ConvertRotation(const Mat4 &blenderSpaceRotation) {
    return Mat4Multiply(Mat4Multiply(kBlenderToRenderUp, blenderSpaceRotation),
                         kBlenderToRenderUpTransposed);
}

Mat4 QuaternionToMat4(float w, float x, float y, float z) {
    Mat4 result = Mat4Identity();
    result.m[0] = 1 - 2 * (y * y + z * z);
    result.m[1] = 2 * (x * y + z * w);
    result.m[2] = 2 * (x * z - y * w);
    result.m[4] = 2 * (x * y - z * w);
    result.m[5] = 1 - 2 * (x * x + z * z);
    result.m[6] = 2 * (y * z + x * w);
    result.m[8] = 2 * (x * z + y * w);
    result.m[9] = 2 * (y * z - x * w);
    result.m[10] = 1 - 2 * (x * x + y * y);
    return result;
}

} // namespace

GameState *GameInit(Arena *arena) {
    GameState *state = ArenaPushStruct(arena, GameState);

    state->objectCount = kDemoCubeCount;
    for (int i = 0; i < kDemoCubeCount; ++i) {
        SceneObject &object = state->objects[i];
        object.position = Vec3{(i - 1) * 3.0f, 0.0f, 0.0f};
        object.scale = Vec3{1.0f, 1.0f, 1.0f};
        object.rotation = Mat4Identity();
        object.rotationEuler = Vec3{0.0f, 0.0f, 0.0f};
        object.rotationSpeed = Vec3{0.6f + 0.2f * i, 0.9f - 0.15f * i, 0.0f};
        object.primitive = Primitive::Cube;
    }

    return state;
}

void GameUpdate(GameState *state, float deltaTime) {
    for (int i = 0; i < state->objectCount; ++i) {
        SceneObject &object = state->objects[i];
        bool spins = object.rotationSpeed.x != 0.0f || object.rotationSpeed.y != 0.0f ||
                     object.rotationSpeed.z != 0.0f;
        if (!spins) {
            continue;
        }
        object.rotationEuler.x += object.rotationSpeed.x * deltaTime;
        object.rotationEuler.y += object.rotationSpeed.y * deltaTime;
        object.rotationEuler.z += object.rotationSpeed.z * deltaTime;
        object.rotation = Mat4EulerXYZ(object.rotationEuler);
    }
}

bool GameImportBlendFile(GameState *state, const char *filepath) {
    BlendFile *file = BlendFileOpen(filepath);
    if (!file) {
        fprintf(stderr, "Failed to open blend file: %s\n", filepath);
        return false;
    }

    int importedCount = 0;
    int objectStructIndex = 0;
    const void *objectBlock = nullptr;
    while (importedCount < kMaxSceneObjects) {
        objectBlock = BlendFileNextBlock(file, "OB", objectBlock, &objectStructIndex);
        if (!objectBlock) {
            break;
        }

        int32_t objectType = 0;
        if (!BlendFileReadInt(file, objectBlock, objectStructIndex, "type", &objectType) ||
            objectType != kObjectTypeMesh) {
            continue;
        }

        int meshStructIndex = 0;
        const void *meshBlock =
            BlendFileFollowPointer(file, objectBlock, objectStructIndex, "data", &meshStructIndex);
        if (!meshBlock) {
            continue;
        }

        // Field name varies by Blender version ("totvert" pre-4.x-ish,
        // "verts_num" in newer versions); try both.
        int32_t vertsNum = 0;
        if (!BlendFileReadInt(file, meshBlock, meshStructIndex, "verts_num", &vertsNum)) {
            BlendFileReadInt(file, meshBlock, meshStructIndex, "totvert", &vertsNum);
        }
        Primitive primitive = (vertsNum == 4) ? Primitive::Plane : Primitive::Cube;

        float loc[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        BlendFileReadFloatArray(file, objectBlock, objectStructIndex, "loc[3]", loc, 3);
        // Field name varies by Blender version ("size" pre-5.x-ish, "scale"
        // in newer versions); try both.
        if (!BlendFileReadFloatArray(file, objectBlock, objectStructIndex, "scale[3]", scale, 3)) {
            BlendFileReadFloatArray(file, objectBlock, objectStructIndex, "size[3]", scale, 3);
        }

        int32_t rotationMode = 1;
        BlendFileReadInt(file, objectBlock, objectStructIndex, "rotmode", &rotationMode);

        Mat4 rotationBlenderSpace;
        if (rotationMode == kRotationModeQuaternion) {
            float quat[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            BlendFileReadFloatArray(file, objectBlock, objectStructIndex, "quat[4]", quat, 4);
            rotationBlenderSpace = QuaternionToMat4(quat[0], quat[1], quat[2], quat[3]);
        } else {
            float rot[3] = {0.0f, 0.0f, 0.0f};
            BlendFileReadFloatArray(file, objectBlock, objectStructIndex, "rot[3]", rot, 3);
            rotationBlenderSpace = Mat4EulerXYZ(Vec3{rot[0], rot[1], rot[2]});
        }

        SceneObject &object = state->objects[importedCount];
        object.position = ConvertPosition(Vec3{loc[0], loc[1], loc[2]});
        object.scale = ConvertScale(Vec3{scale[0], scale[1], scale[2]});
        if (primitive == Primitive::Cube) {
            // The engine's built-in cube mesh is a 1x1x1 unit cube; Blender's
            // default cube is 2x2x2.
            object.scale.x *= 2.0f;
            object.scale.y *= 2.0f;
            object.scale.z *= 2.0f;
        }
        object.rotation = ConvertRotation(rotationBlenderSpace);
        object.rotationEuler = Vec3{0.0f, 0.0f, 0.0f};
        object.rotationSpeed = Vec3{0.0f, 0.0f, 0.0f};
        object.primitive = primitive;
        ++importedCount;
    }

    BlendFileClose(file);

    if (importedCount == 0) {
        fprintf(stderr, "No cube/plane mesh objects found in: %s\n", filepath);
        return false;
    }

    state->objectCount = importedCount;
    return true;
}
