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
 * CAPABILITY convention: a group published with version == 0 is ABSENT
 * (e.g. a headless host may publish ui as absent) — the client shim treats
 * it as a mismatch and disables that group; modules query
 * engineApiHas(group) to adapt instead of assuming every service exists.
 *
 * ── COMPATIBILITY CONTRACT: a kit built for v1 MUST run on v5 ───────────────
 * This is the promise the whole table exists to make, and it constrains
 * evolution more tightly than "append and bump" does.
 *
 * The groups are INLINE, so a group's size determines every later group's
 * OFFSET. Appending a field to `core` moves `input`, `assets` and everything
 * after it — and an older module, which computes those offsets from its own
 * smaller headers, would then read the wrong pointers and call the wrong
 * functions. Silently. So:
 *
 *   1. A SHIPPED GROUP IS FROZEN. Never append to it, never reorder, never
 *      remove, never repurpose a field. The static_asserts at the bottom of
 *      this header enforce it: growing a group fails the BUILD.
 *   2. NEW FUNCTIONALITY GOES IN A NEW GROUP, appended to the end of the
 *      table. That only moves structSize, which is exactly what old modules
 *      are allowed to ignore.
 *   3. VERSIONS MEAN "AT LEAST". The client accepts host_version >=
 *      module_version and host structSize >= the module's. A newer host always
 *      satisfies an older module; the reverse is refused.
 *   4. A genuine break makes an EngineApiTableV2 with a new bind symbol, and
 *      V1 KEEPS BEING FILLED — indefinitely, not "for a release or two".
 *
 * Rule 1 is why input and assets sit at v2 with no room to grow: those bumps
 * predate this contract. From here, a group's version changes only when its
 * MEANING is clarified, never its layout.
 *
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
#define ENGINE_API_INPUT_V   2  /* v2: + lookTotal (cumulative look)       */
#define ENGINE_API_PHYSICS_V 1  /* body forces, raycast, character         */
#define ENGINE_API_AUDIO_V   1
#define ENGINE_API_ASSETS_V  2  /* v2: + loadMaterial / entitySetMaterial  */
#define ENGINE_API_ANIM_V    1
#define ENGINE_API_UI_V      1  /* editor-UI WIDGETS (negotiated; 0 headless)*/
#define ENGINE_API_NAV_V     1  /* navmesh path queries (Recast/Detour)    */
#define ENGINE_API_DRAW_V    1  /* immediate-mode debug draw (any renderer)*/
/* ── The PRIMITIVE tier ──────────────────────────────────────────────────────
 * The groups above expose the engine's SUBSYSTEMS — finished opinions about
 * physics, animation, navigation. These three expose what those subsystems are
 * BUILT ON, so a developer who wants their own system is not forced to adopt
 * ours or to fork. Without them, "extend the engine" means "use our animator";
 * with them, our animator is merely the default implementation. */
#define ENGINE_API_JOBS_V    1  /* worker pool: parallelFor, main-thread defer */
#define ENGINE_API_MEMORY_V  1  /* tagged heaps + the per-frame arena          */
#define ENGINE_API_DRAWSUB_V 1  /* submit geometry with no entity/component    */

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
    /* v2 additions (append-only) */
    void  (*lookTotal)(double*, double*);
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
    /* v2 — APPENDED, per the evolution rules above. A cooked material by its
       authored NAME, and applying one to an entity. This is how a game ships a
       look the engine knows nothing about. */
    uint32_t (*loadMaterial)(const char* name);
    bool     (*entitySetMaterial)(EngineEntity, uint32_t materialId);
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

/* Editor-UI WIDGETS only — capability-negotiated (version 0 when no widget
 * backend, i.e. engine_host). Debug DRAW moved to its own group below: it's a
 * RENDERER capability, present wherever there's a renderer (editor + host),
 * and must not be gated behind the editor widget backend. */
typedef struct EngineApiUiV1 {
    uint32_t version;
    void (*text)(const char*);   /* PRE-FORMATTED — the client shim printf's */
    bool (*button)(const char*);
    bool (*sliderFloat)(const char*, float*, float, float);
    bool (*checkbox)(const char*, bool*);
    void (*separator)(void);
} EngineApiUiV1;

/* Immediate-mode debug draw (3D lines/shapes, THIS frame). A renderer
 * capability, not an editor one — always published; the calls no-op safely in a
 * truly headless host (no renderer), like physics/audio without a backend. */
typedef struct EngineApiDrawV1 {
    uint32_t version;
    void (*drawLine)(float, float, float, float, float, float,
                     float, float, float);
    void (*drawSphere)(float, float, float, float, float, float, float);
    void (*drawBox)(float, float, float, float, float, float,
                    float, float, float);
    void (*drawDisk)(float, float, float, float, float, float, float,
                     float, float, float);
} EngineApiDrawV1;

/* Navigation: pathfinding over the engine's baked navmesh (NavService). Kits
 * never link Recast/Detour — they receive world-space straight-path waypoints.
 * findPath returns the waypoint count written to out (xyz triples, maxPoints
 * capacity); project snaps a point onto walkable ground; ready() is true once a
 * navmesh has been baked (else the queries no-op to 0/false). */
typedef struct EngineApiNavV1 {
    uint32_t version;
    int  (*findPath)(float sx, float sy, float sz, float ex, float ey, float ez,
                     float* out, int maxPoints);
    bool (*project)(float x, float y, float z, float* out);
    bool (*ready)(void);
} EngineApiNavV1;

/* Jobs — the engine's own worker pool. A kit that spawns its own threads is
 * competing with the pool already saturating the machine, which is why this is
 * a primitive rather than something each module solves privately. See
 * engine_api.h for why v1 has no job handles. */
typedef struct EngineApiJobsV1 {
    uint32_t version;
    uint32_t (*workerCount)(void);
    void (*parallelFor)(const char* name, uint32_t count, uint32_t grain,
                        void (*fn)(void* user, uint32_t begin, uint32_t end),
                        void* user);
    void (*onMain)(void (*fn)(void* user), void* user);
} EngineApiJobsV1;

/* Memory — the tagged heaps the engine budgets against, plus the frame arena.
 * A kit on plain malloc is invisible to the budget telemetry and fragments a
 * heap that was tuned. `tag` is mem::Tag's underlying value; an unknown tag is
 * charged to the general tag rather than refused, so a kit built against a
 * newer tag list still runs on an older host. */
typedef struct EngineApiMemoryV1 {
    uint32_t version;
    void*    (*alloc)(size_t size, size_t align, uint8_t tag);
    void     (*free)(void* p);
    size_t   (*allocSize)(void* p);
    void*    (*frameAlloc)(size_t size, size_t align);   /* null when exhausted */
    uint64_t (*taggedBytes)(uint8_t tag);
} EngineApiMemoryV1;

/* Draw submission — geometry that exists for one frame and owns no entity.
 * The engine draws MeshRenderer components; this is how a system the engine
 * never anticipated gets pixels on screen without adopting that
 * representation. Submissions are cleared at the frame flip. */
typedef struct EngineApiDrawSubmitV1 {
    uint32_t version;
    void     (*submitMesh)(uint32_t meshHandle, uint32_t materialHandle,
                           const float model[16]);
    uint32_t (*submittedCount)(void);
} EngineApiDrawSubmitV1;

typedef struct EngineApiTableV1 {
    uint32_t structSize;   /* = sizeof(EngineApiTableV1) — hard gate */
    EngineApiCoreV1    core;
    EngineApiInputV1   input;
    EngineApiPhysicsV1 physics;
    EngineApiAudioV1   audio;
    EngineApiAssetsV1  assets;
    EngineApiAnimV1    anim;
    EngineApiUiV1      ui;
    EngineApiNavV1     nav;   /* appended — bumps structSize; modules rebuild */
    EngineApiDrawV1    draw;  /* debug draw, split out of ui (renderer capability) */
    /* The primitive tier — appended, so every field above keeps its offset and
     * only structSize moves. Modules rebuild; the rule is unchanged. */
    EngineApiJobsV1       jobs;
    EngineApiMemoryV1     memory;
    EngineApiDrawSubmitV1 drawSubmit;
} EngineApiTableV1;

/* ── Frozen layout ───────────────────────────────────────────────────────────
 * Every shipped group's size, and therefore every group's offset, is nailed
 * down here. These are not documentation: they are the mechanism behind the
 * compatibility contract above. If you appended a field to a group, one of
 * these fails and the message tells you what to do instead — add a NEW group.
 *
 * Bumping a number here is a decision to break every module built before it.
 * There is exactly one case where that is legitimate: this table is pre-1.0
 * and no third party has shipped a kit yet. After that, the answer is a new
 * group or an EngineApiTableV2. */
#define ENGINE_API_FROZEN(group, bytes)                                        \
    static_assert(sizeof(group) == (bytes),                                    \
        #group " changed size. A shipped group is FROZEN: growing it shifts "  \
        "every later group's offset and makes older modules read the wrong "   \
        "function pointers, silently. Add a NEW group at the end of "          \
        "EngineApiTableV1 instead — see the compatibility contract at the "    \
        "top of this header.")

ENGINE_API_FROZEN(EngineApiCoreV1,       120);
ENGINE_API_FROZEN(EngineApiInputV1,      120);
ENGINE_API_FROZEN(EngineApiPhysicsV1,     64);
ENGINE_API_FROZEN(EngineApiAudioV1,       32);
ENGINE_API_FROZEN(EngineApiAssetsV1,     128);
ENGINE_API_FROZEN(EngineApiAnimV1,        64);
ENGINE_API_FROZEN(EngineApiUiV1,          48);
ENGINE_API_FROZEN(EngineApiNavV1,         32);
ENGINE_API_FROZEN(EngineApiDrawV1,        40);
ENGINE_API_FROZEN(EngineApiJobsV1,        32);
ENGINE_API_FROZEN(EngineApiMemoryV1,      48);
ENGINE_API_FROZEN(EngineApiDrawSubmitV1,  24);

/* Host-side: the filled table (engine_api_table.cpp). */
const EngineApiTableV1* engineApiHostTable(void);

/* Module-side (defined by engine_api_client.h when included): the loader
 * calls this right after dlopen, before the module create function. */
typedef void (*EngineModuleBindApiV1Fn)(const EngineApiTableV1*);

#ifdef __cplusplus
}
#endif
