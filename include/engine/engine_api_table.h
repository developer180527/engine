#pragma once
/* ── EngineApiTableV1 — the explicit, versioned API handoff ──────────────────
 *
 * Modules (kits, game modules) historically resolved engine* symbols from the
 * host through the dynamic linker (-undefined dynamic_lookup). That is
 * implicit, macOS-specific, and versions the API as one monolithic surface.
 * This table replaces it: the host hands each module ONE struct of function
 * pointers at load (engineModuleBindApiV1), grouped by SUBSYSTEM, each group
 * carrying its own version number.
 *
 * Why per-subsystem versions: a kit that only touches physics must not be
 * invalidated because the animation API grew. The client shim
 * (engine_api_client.h) checks versions PER GROUP at bind time: matching
 * groups work normally; a mismatched group disables only ITS functions (loud
 * per-call error, safe defaults) while the rest of the module runs.
 *
 * Evolution rules:
 *  - Append fields to the END of a group and bump that group's version.
 *  - Never reorder or remove fields within V1 structs; breaking changes make
 *    an EngineApiTableV2 with a new bind symbol, V1 keeps working.
 *  - engineUiSetBackend is deliberately absent: it is host-only.
 *
 * The dynamic_lookup path still works for modules that don't include the
 * client shim — migration, not flag day. (Windows has no dynamic_lookup;
 * there this table is the ONLY path, which is the point.)
 */
#include "engine_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENGINE_API_CORE_V    1  /* log, time, entities, transform          */
#define ENGINE_API_INPUT_V   1  /* legacy keys, actions, look, cursor      */
#define ENGINE_API_PHYSICS_V 1  /* body forces, raycast, character         */
#define ENGINE_API_AUDIO_V   1
#define ENGINE_API_ASSETS_V  1  /* cooked assets + scenes                  */
#define ENGINE_API_ANIM_V    1
#define ENGINE_API_UI_V      1  /* editor-UI facade + debug draw           */

typedef struct EngineApiCoreV1 {
    uint32_t version;
    void (*logInfo)(const char*);
    void (*logWarn)(const char*);
    void (*logError)(const char*);
    float    (*deltaTime)(void);
    double   (*elapsed)(void);
    uint64_t (*frame)(void);
    EngineEntity (*entityCreate)(const char*);
    void         (*entityDestroy)(EngineEntity);
    EngineEntity (*entityFind)(const char*);
    bool         (*entityAlive)(EngineEntity);
    void         (*entitySetParent)(EngineEntity, EngineEntity);
    void         (*entityClearParent)(EngineEntity);
    bool (*getTransform)(EngineEntity, EngineTransform*);
    void (*setTransform)(EngineEntity, const EngineTransform*);
} EngineApiCoreV1;

typedef struct EngineApiInputV1 {
    uint32_t version;
    bool  (*keyDown)(const char*);
    bool  (*keyPressed)(const char*);
    float (*axis)(const char*);
    bool  (*mouseDown)(int);
    void  (*mouseDelta)(float*, float*);
    bool  (*actionDown)(const char*);
    bool  (*actionPressed)(const char*);
    bool  (*actionReleased)(const char*);
    float (*actionAxis1)(const char*);
    void  (*actionAxis2)(const char*, float*, float*);
    void  (*lookDelta)(float*, float*);
    void  (*setCursorCaptured)(bool);
    bool  (*cursorCaptured)(void);
} EngineApiInputV1;

typedef struct EngineApiPhysicsV1 {
    uint32_t version;
    void (*applyImpulse)(EngineEntity, float, float, float);
    void (*setVelocity)(EngineEntity, float, float, float);
    bool (*getVelocity)(EngineEntity, float*, float*, float*);
    EngineRaycastHit (*raycast)(float, float, float, float, float, float, float);
    void (*charMove)(EngineEntity, float, float);
    void (*charJump)(EngineEntity, float);
    bool (*charGrounded)(EngineEntity);
} EngineApiPhysicsV1;

typedef struct EngineApiAudioV1 {
    uint32_t version;
    uint32_t (*playSound)(const char*);
    uint32_t (*playSoundAt)(const char*, float, float, float);
    void     (*stopSound)(uint32_t);
} EngineApiAudioV1;

typedef struct EngineApiAssetsV1 {
    uint32_t version;
    uint32_t (*loadMesh)(const char*);
    bool     (*unloadMesh)(uint32_t);
    uint32_t (*loadTexture)(const char*);
    bool     (*unloadTexture)(uint32_t);
    void     (*loadMeshAsync)(const char*);
    void     (*loadTextureAsync)(const char*);
    uint32_t (*queryMesh)(const char*);
    uint32_t (*queryTexture)(const char*);
    bool     (*isLoading)(const char*);
    uint32_t (*sceneLoad)(const char*);
    bool     (*sceneUnload)(uint32_t);
    void     (*scenePreload)(const char*);
    bool     (*sceneIsReady)(const char*);
} EngineApiAssetsV1;

typedef struct EngineApiAnimV1 {
    uint32_t version;
    bool  (*play)(EngineEntity, const char*, float);
    void  (*setSpeed)(EngineEntity, float);
    void  (*setLooping)(EngineEntity, bool);
    void  (*setPlaying)(EngineEntity, bool);
    bool  (*isPlaying)(EngineEntity);
    float (*time)(EngineEntity);
    float (*duration)(EngineEntity);
} EngineApiAnimV1;

typedef struct EngineApiUiV1 {
    uint32_t version;
    void (*text)(const char*);   /* PRE-FORMATTED — the client shim printf's */
    bool (*button)(const char*);
    bool (*sliderFloat)(const char*, float*, float, float);
    bool (*checkbox)(const char*, bool*);
    void (*separator)(void);
    void (*drawLine)(float, float, float, float, float, float,
                     float, float, float);
    void (*drawSphere)(float, float, float, float, float, float, float);
    void (*drawBox)(float, float, float, float, float, float,
                    float, float, float);
    void (*drawDisk)(float, float, float, float, float, float, float,
                     float, float, float);
} EngineApiUiV1;

typedef struct EngineApiTableV1 {
    uint32_t structSize;   /* = sizeof(EngineApiTableV1) — hard gate */
    EngineApiCoreV1    core;
    EngineApiInputV1   input;
    EngineApiPhysicsV1 physics;
    EngineApiAudioV1   audio;
    EngineApiAssetsV1  assets;
    EngineApiAnimV1    anim;
    EngineApiUiV1      ui;
} EngineApiTableV1;

/* Host-side: the filled table (engine_api_table.cpp). */
const EngineApiTableV1* engineApiHostTable(void);

/* Module-side (defined by engine_api_client.h when included): the loader
 * calls this right after dlopen, before the module create function. */
typedef void (*EngineModuleBindApiV1Fn)(const EngineApiTableV1*);

#ifdef __cplusplus
}
#endif
