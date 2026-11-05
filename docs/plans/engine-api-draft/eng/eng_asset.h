/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_asset.h: load cooked assets by name, or make them from your own data. The
 * engine owns CPU data; the renderer owns GPU copies. Loading never blocks:
 * ask for an asset, use it, and check its state if you care. */
#ifndef ENG_ASSET_H
#define ENG_ASSET_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

typedef uint32_t EngAssetState;
enum {
    ENG_ASSET_LOADING = 0,
    ENG_ASSET_READY = 1,
    ENG_ASSET_FAILED = 2 /* engGetLastErrorMessage says why */
};

/* name: as authored, "zombie_sickly". Asking twice gives the same handle and a
 * second reference. Release once per request. */
ENG_API EngResult engRequestAsset(EngEngine* engine, const char* name, EngAsset* out);
ENG_API void engReleaseAsset(EngEngine* engine, EngAsset asset);
ENG_API EngAssetState engGetAssetState(EngEngine* engine, EngAsset asset);

/* ── Made in code: a code-first game needs no cooker for these ── */
typedef struct EngMeshDesc {
    uint32_t struct_size;
    uint32_t vertex_count;
    const EngVec3* positions; /* required */
    const EngVec3* normals;   /* NULL: computed */
    const float* uvs;         /* 2 floats per vertex; NULL: none */
    const uint32_t* indices;  /* NULL: positions are triangles in order */
    uint32_t index_count;
    uint32_t reserved;        /* must be zero */
} EngMeshDesc;
ENG_API EngResult engCreateMesh(EngEngine* engine, const EngMeshDesc* desc, EngAsset* out);

typedef struct EngTextureDesc {
    uint32_t struct_size;
    uint32_t width_pixels, height_pixels;
    EngBool linear;      /* 0: sRGB colour */
    const void* rgba8;   /* top row first */
} EngTextureDesc;
ENG_API EngResult engCreateTexture(EngEngine* engine, const EngTextureDesc* desc, EngAsset* out);
/* New pixels for a texture made here: a CRT or dial drawn in code. */
ENG_API EngResult engUpdateTexture(EngEngine* engine, EngAsset texture, const void* rgba8);

/* A plain PBR parameter set. Each renderer builds its own shaders from it. */
typedef struct EngMaterialDesc {
    uint32_t struct_size;
    uint32_t reserved;     /* must be zero */
    float base_color[4];   /* all zero: white */
    float emissive[3];
    float metallic, roughness;
    EngAsset base_color_texture, normal_texture, emissive_texture;
} EngMaterialDesc;
ENG_API EngResult engCreateMaterial(EngEngine* engine, const EngMaterialDesc* desc, EngAsset* out);

/* ── Scenes ── */
/* Adds a cooked scene to the world, also without blocking. The returned asset
 * unloads what it added when released. */
ENG_API EngResult engLoadScene(EngWorld* world, const char* name, EngAsset* out);

ENG_EXTERN_C_END
#endif /* ENG_ASSET_H */
