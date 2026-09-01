#include "scene_import.h"

#include "blend_file.h"
#include "scene.h"

#include <cstdio>

namespace {

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

bool SceneImportBlendFile(SceneState *scene, const char *filepath) {
    BlendFile *file = BlendFileOpen(filepath);
    if (!file) {
        fprintf(stderr, "Failed to open blend file: %s\n", filepath);
        return false;
    }

    SceneClear(scene);

    int importedCount = 0;
    BlendBlock objectBlock;
    while (importedCount < kMaxMeshRenderers) {
        objectBlock = BlendFileNextBlock(file, "OB", objectBlock);
        if (!objectBlock) {
            break;
        }

        int32_t objectType = 0;
        if (!BlendFileReadInt(file, objectBlock, "type", &objectType) ||
            objectType != kObjectTypeMesh) {
            continue;
        }

        BlendBlock meshBlock = BlendFileFollowPointer(file, objectBlock, "data");
        if (!meshBlock) {
            continue;
        }

        // Field name varies by Blender version ("totvert" pre-4.x-ish,
        // "verts_num" in newer versions); try both.
        int32_t vertsNum = 0;
        if (!BlendFileReadInt(file, meshBlock, "verts_num", &vertsNum)) {
            BlendFileReadInt(file, meshBlock, "totvert", &vertsNum);
        }
        MeshId mesh = (vertsNum == 4) ? MeshId_Plane : MeshId_Cube;

        float loc[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        BlendFileReadFloatArray(file, objectBlock, "loc[3]", loc, 3);
        // Field name varies by Blender version ("size" pre-5.x-ish, "scale"
        // in newer versions); try both.
        if (!BlendFileReadFloatArray(file, objectBlock, "scale[3]", scale, 3)) {
            BlendFileReadFloatArray(file, objectBlock, "size[3]", scale, 3);
        }

        int32_t rotationMode = 1;
        BlendFileReadInt(file, objectBlock, "rotmode", &rotationMode);

        Mat4 rotationBlenderSpace;
        if (rotationMode == kRotationModeQuaternion) {
            float quat[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            BlendFileReadFloatArray(file, objectBlock, "quat[4]", quat, 4);
            rotationBlenderSpace = QuaternionToMat4(quat[0], quat[1], quat[2], quat[3]);
        } else {
            float rot[3] = {0.0f, 0.0f, 0.0f};
            BlendFileReadFloatArray(file, objectBlock, "rot[3]", rot, 3);
            rotationBlenderSpace = Mat4EulerXYZ(Vec3{rot[0], rot[1], rot[2]});
        }

        Transform transform = TransformIdentity();
        transform.position = ConvertPosition(Vec3{loc[0], loc[1], loc[2]});
        transform.scale = ConvertScale(Vec3{scale[0], scale[1], scale[2]});
        if (mesh == MeshId_Cube) {
            // The engine's built-in cube mesh is a 1x1x1 unit cube; Blender's
            // default cube is 2x2x2.
            transform.scale.x *= 2.0f;
            transform.scale.y *= 2.0f;
            transform.scale.z *= 2.0f;
        }
        transform.rotation = QuatNormalize(QuatFromMat4(ConvertRotation(rotationBlenderSpace)));

        char name[32];
        snprintf(name, sizeof(name), "Imported %d", importedCount);
        EntityId entity = SceneAddEntity(scene, EntityKind_Mesh, name, transform);
        Vec3 aabbMin = (mesh == MeshId_Plane) ? Vec3{-0.5f, 0.0f, -0.5f} : Vec3{-0.5f, -0.5f, -0.5f};
        Vec3 aabbMax = (mesh == MeshId_Plane) ? Vec3{0.5f, 0.0f, 0.5f} : Vec3{0.5f, 0.5f, 0.5f};
        SceneSetMesh(scene, entity, mesh, aabbMin, aabbMax);
        ++importedCount;
    }

    BlendFileClose(file);

    if (importedCount == 0) {
        fprintf(stderr, "No cube/plane mesh objects found in: %s\n", filepath);
        return false;
    }

    return true;
}
