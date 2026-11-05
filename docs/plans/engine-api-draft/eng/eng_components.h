/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_components.h: the common vocabulary core owns. Every renderer and physics
 * provider understands these. They store intent, never implementation: no GPU
 * handle and no body pointer. A frozen layout never changes; a real change is a
 * new name ("core.Camera2"), with a migration from the old one.
 *
 * This first set shows the shape. Light, RigidBody, Collider and AudioSource
 * follow the same pattern. */
#ifndef ENG_COMPONENTS_H
#define ENG_COMPONENTS_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

#define ENG_COMPONENT_TRANSFORM "core.Transform" /* EngTransform */
#define ENG_TRANSFORM_IDENTITY { {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1} }

#define ENG_COMPONENT_CAMERA "core.Camera"
typedef struct EngCamera {
    float vertical_fov_radians;
    float near_plane, far_plane; /* far 0: infinite */
    uint32_t orthographic;       /* EngBool; then ortho_height is used */
    float ortho_height;
    uint32_t reserved[3];        /* zero */
} EngCamera;

#define ENG_COMPONENT_MESH_INSTANCE "core.MeshInstance"
typedef struct EngMeshInstance {
    EngAsset mesh;
    EngAsset material;  /* 0: the mesh's own */
    uint32_t layer_mask;/* 0: all layers */
    uint32_t reserved;  /* zero */
} EngMeshInstance;

ENG_EXTERN_C_END
#endif /* ENG_COMPONENTS_H */
