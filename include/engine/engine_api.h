#pragma once
// ── Engine C API ─────────────────────────────────────────────────────────────
// The flat, FFI-stable surface over ScriptHost — one set of exported C
// functions that every consumer shares:
//
//   • hot-reloaded game modules  (resolve these from the host at load —
//     unlike header-inline singletons, the calls execute HOST-side, so
//     input/audio/physics state is the real one)
//   • the Lua bindings           (same semantics, same host object)
//   • future C# hosting          ([DllImport]/function pointers point here)
//
// All functions are safe to call at any time: when no simulation world is
// bound they no-op and return zero/false. Entity handles are flecs entity
// ids (0 = invalid) valid for the current simulation session only.
//
// This header is C-compatible — keep it free of C++ types.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t EngineEntity;   /* flecs entity id, 0 = invalid */

typedef struct EngineVec3      { float x, y, z; }    EngineVec3;
typedef struct EngineQuat      { float x, y, z, w; } EngineQuat;
typedef struct EngineTransform {
    EngineVec3 position;
    EngineQuat rotation;
    EngineVec3 scale;
} EngineTransform;

typedef struct EngineRaycastHit {
    bool         hit;
    EngineEntity entity;
    EngineVec3   point;
    EngineVec3   normal;
    float        distance;
} EngineRaycastHit;

/* ── Log ─────────────────────────────────────────────────────────────────── */
void engineLogInfo (const char* msg);
void engineLogWarn (const char* msg);
void engineLogError(const char* msg);

/* ── Input — DEPRECATED (legacy polling; retired for gameplay use) ─────────
 * Use ACTIONS below instead: bindings live in the project's input.json, the
 * context stack + focus gates apply, devices stay swappable. These stubs
 * keep old modules loading (table V1 slots are frozen) and warn once per
 * process. Editor-internal polling uses InputSystem directly, not this. */
bool  engineKeyDown   (const char* key);
bool  engineKeyPressed(const char* key);
float engineAxis      (const char* axisName);   /* bound via InputMap */
bool  engineMouseDown (int button);             /* 0=left 1=right 2=middle */
void  engineMouseDelta(float* dx, float* dy);

/* ── Actions (input-agnostic gameplay input) ─────────────────────────────
 * Gameplay binds to ACTIONS, never devices/keys: the project's input.json
 * wires devices -> actions (contexts, axes, scales). Backed by the raw-input
 * pipeline (modules/hid) when available, window input otherwise.
 * engineLookDelta is the LATE-LATCH camera path: freshest accumulated raw
 * mouse counts, drained on read — call once per frame from the camera code,
 * apply your own sensitivity. */
bool  engineActionDown    (const char* action);
bool  engineActionPressed (const char* action);
bool  engineActionReleased(const char* action);
float engineActionAxis1   (const char* action);
void  engineActionAxis2   (const char* action, float* x, float* y);
void  engineLookDelta     (float* dx, float* dy);
/* Cumulative raw look counts (never reset): multi-consumer form — each
 * consumer diffs against its own last-read totals. Input API v2. */
void  engineLookTotal     (double* x, double* y);

/* ── Cursor (mouse-look capture; no-op headless) ─────────────────────────── */
void engineSetCursorCaptured(bool captured);
bool engineCursorCaptured(void);

/* ── Time (simulation clock — set each tick) ─────────────────────────────── */
float    engineDeltaTime(void);
double   engineElapsed(void);
uint64_t engineFrame(void);

/* ── Entities ────────────────────────────────────────────────────────────── */
EngineEntity engineEntityCreate(const char* name);
void         engineEntityDestroy(EngineEntity e);
EngineEntity engineEntityFind(const char* name);
bool         engineEntityAlive(EngineEntity e);
void         engineEntitySetParent(EngineEntity child, EngineEntity parent);
void         engineEntityClearParent(EngineEntity child);

/* ── Transform ───────────────────────────────────────────────────────────── */
bool engineGetTransform(EngineEntity e, EngineTransform* out);
void engineSetTransform(EngineEntity e, const EngineTransform* t);

/* ── Physics (no-op until a physics plugin is simulating) ────────────────── */
void engineApplyImpulse(EngineEntity e, float x, float y, float z);
void engineSetVelocity (EngineEntity e, float x, float y, float z);
bool engineGetVelocity (EngineEntity e, float* x, float* y, float* z);
EngineRaycastHit engineRaycast(float ox, float oy, float oz,
                               float dx, float dy, float dz, float maxDist);
void engineCharMove(EngineEntity e, float vx, float vz);
void engineCharJump(EngineEntity e, float speed);
bool engineCharGrounded(EngineEntity e);

/* ── Navigation (no-op until a navmesh is baked) ──────────────────────────────
 * Path queries over the engine's navmesh (Recast/Detour) — kits get world-space
 * waypoints without linking a nav library. engineNavFindPath fills out (xyz
 * triples, capacity maxPoints) with the straight-path corridor and returns the
 * waypoint count (0 = no path / no navmesh). engineNavProject snaps a point onto
 * walkable ground (out[3]); engineNavReady reports whether a navmesh exists. */
int  engineNavFindPath(float sx, float sy, float sz,
                       float ex, float ey, float ez,
                       float* out, int maxPoints);
bool engineNavProject (float x, float y, float z, float* out);
bool engineNavReady   (void);

/* ── Audio (no-op until an audio plugin is attached) ─────────────────────── */
uint32_t enginePlaySound  (const char* path);
uint32_t enginePlaySoundAt(const char* path, float x, float y, float z);
void     engineStopSound  (uint32_t handle);

/* ── Assets (cooked binaries; returns handle ids, 0 = invalid) ───────────── */
uint32_t engineAssetLoadMesh      (const char* cookedPath);
bool     engineAssetUnloadMesh    (uint32_t handleId);
/* Cooked .material, BY ITS AUTHORED NAME ("zombie_sickly") — not by path: a
   cooked file is <uuid>.cooked and nobody should hand-write a uuid. The
   material is DATA: its shader and parameter values came from the cook, already
   checked against that shader's declared interface, so a misspelled parameter
   failed the cook rather than silently doing nothing here. Repeat calls return
   the same handle. 0 = no material of that name (the names that DO exist are
   logged). */
uint32_t engineAssetLoadMaterial  (const char* name);
/* Point an entity's MeshRenderer at a loaded material. This is how a game
   applies an authored look without the engine knowing what that look is. */
bool     engineEntitySetMaterial  (EngineEntity e, uint32_t materialId);
uint32_t engineAssetLoadTexture   (const char* cookedPath);
bool     engineAssetUnloadTexture (uint32_t handleId);
void     engineAssetLoadMeshAsync   (const char* cookedPath);
void     engineAssetLoadTextureAsync(const char* cookedPath);
uint32_t engineAssetQueryMesh   (const char* cookedPath);
uint32_t engineAssetQueryTexture(const char* cookedPath);
bool     engineAssetIsLoading   (const char* cookedPath);

/* ── Scenes (cooked binary scenes) ───────────────────────────────────────── */
uint32_t engineSceneLoad   (const char* cookedPath);
bool     engineSceneUnload (uint32_t handle);
void     engineScenePreload(const char* cookedPath);
bool     engineSceneIsReady(const char* cookedPath);

/* ── Editor UI facade ────────────────────────────────────────────────────────
 * Immediate-mode widgets a kit/plugin draws from its onEditorUI(), WITHOUT
 * touching ImGui. The host (the editor) registers a backend; in a host with no
 * UI surface (engine_host) these no-op. One doorway — the kit never links UI. */
typedef struct EngineUiBackend {
    void (*text)       (const char* str);
    bool (*button)     (const char* label);
    bool (*sliderFloat)(const char* label, float* v, float lo, float hi);
    bool (*checkbox)   (const char* label, bool* v);
    void (*separator)  (void);
} EngineUiBackend;

void engineUiSetBackend(const EngineUiBackend* backend); /* host registers; null = none */

void engineUiText       (const char* fmt, ...);
bool engineUiButton     (const char* label);
bool engineUiSliderFloat(const char* label, float* v, float lo, float hi);
bool engineUiCheckbox   (const char* label, bool* v);
void engineUiSeparator  (void);

/* ── Animation ───────────────────────────────────────────────────────────────
 * Control surface over the engine's animation machinery (ozz sampling +
 * blending). Play binds a STANDALONE clip asset (project-relative or absolute
 * path) to the entity's skeleton and starts it; switching clips crossfades
 * automatically over `fadeSeconds` (0 = hard cut, <0 = keep current fade). */
bool  engineAnimPlay      (EngineEntity e, const char* clipPath, float fadeSeconds);
void  engineAnimSetSpeed  (EngineEntity e, float speed);
void  engineAnimSetLooping(EngineEntity e, bool looping);
void  engineAnimSetPlaying(EngineEntity e, bool playing);
bool  engineAnimIsPlaying (EngineEntity e);
float engineAnimTime      (EngineEntity e);
float engineAnimDuration  (EngineEntity e);

/* ── Debug draw (immediate-mode 3D lines/wireframes, THIS frame only) ─────────
 * A kit queues shapes from its per-frame code; the renderer draws them into the
 * world views and clears the queue next frame. Color is 0..1 RGB. Works in any
 * host with a renderer (editor + engine_host); no-ops headless. */
void engineDrawLine  (float x0, float y0, float z0, float x1, float y1, float z1,
                      float r, float g, float b);
void engineDrawSphere(float cx, float cy, float cz, float radius,
                      float r, float g, float b);
void engineDrawBox   (float cx, float cy, float cz, float hx, float hy, float hz,
                      float r, float g, float b);
void engineDrawDisk  (float cx, float cy, float cz, float nx, float ny, float nz,
                      float radius, float r, float g, float b);

/* ── Jobs — the engine's worker pool, not a thread library ────────────────────
 * The PRIMITIVE beneath every parallel system the engine ships. A kit that
 * wants its own animation or AI system needs the same pool the engine uses, or
 * it spawns threads that fight the ones already running.
 *
 * parallelFor is BLOCKING and splits [0,count) into grain-sized chunks; `fn`
 * receives a half-open [begin,end) range and the caller's user pointer. Ranges
 * within one call may run concurrently on any worker, so `fn` must be safe to
 * call from several threads at once.
 *
 * Deliberately NO job handles in v1. A handle crossing a module boundary means
 * the module owns a lifetime the host allocated, and a kit unloaded mid-job
 * takes the process with it (the engine already fixed one JobHandle
 * use-after-free of its own). Blocking parallelFor covers the data-parallel
 * work kits actually have; async handles can be appended in v2 when something
 * needs them, which is exactly what per-group versioning is for. */
uint32_t engineJobsWorkerCount(void);
void engineJobsParallelFor(const char* name, uint32_t count, uint32_t grain,
                           void (*fn)(void* user, uint32_t begin, uint32_t end),
                           void* user);
/* Defer to the main thread, run at the next pump. For work that must not run
 * off-thread (window/GPU/UI calls). Fire-and-forget: no completion signal. */
void engineJobsOnMain(void (*fn)(void* user), void* user);

/* ── Memory — the engine's tagged heaps and frame arena ───────────────────────
 * A kit allocating with plain malloc is invisible to the budget telemetry and
 * to the leak accounting, and it fragments a heap the engine tuned. `tag`
 * matches mem::Tag; anything out of range is charged to the general tag rather
 * than rejected, because a kit built against a newer tag list must still run.
 *
 * frameAlloc hands out FRAME-SCOPED memory: valid until the end of the current
 * frame, freed wholesale by a pointer reset, never individually. Trivially
 * destructible data only — nothing is destructed.
 *
 * Returns null when unbound, when size is 0, or when the request EXCEEDS THE
 * ARENA'S CAPACITY. That last one is a deliberate boundary check rather than a
 * pass-through: the arena itself spills to the heap when it overflows, which
 * suits engine code that overshoots by a little and is wrong for a module that
 * can ask for anything. A null return is a normal outcome to handle, not an
 * error to assert on. */
void*    engineMemAlloc(size_t size, size_t align, uint8_t tag);
void     engineMemFree(void* p);
size_t   engineMemAllocSize(void* p);
void*    engineMemFrameAlloc(size_t size, size_t align);
uint64_t engineMemTaggedBytes(uint8_t tag);

/* ── Draw submission — geometry without the engine's opinions ────────────────
 * The engine draws what carries a MeshRenderer component. This is the escape
 * hatch: submit a mesh + material + transform for THIS frame directly, with no
 * entity, no component and no scene-graph involvement.
 *
 * It is the primitive a developer needs to build a renderer-facing system the
 * engine did not anticipate — their own particle system, impostors, a custom
 * culling scheme — without adopting the ECS representation. Submissions last
 * one frame and are cleared at the flip, so a system re-submits every frame,
 * which is also what makes it safe: nothing outlives the frame that made it.
 *
 * `model` is 16 floats, row-major, matching EngineTransform's convention.
 * Bounds come from the mesh, so submitted geometry is culled like anything
 * else. No-ops headless. */
void     engineDrawSubmitMesh(uint32_t meshHandle, uint32_t materialHandle,
                              const float model[16]);
uint32_t engineDrawSubmittedCount(void);


/* ── Log categories (ENGINE_API_LOG_V) ───────────────────────────────────────
 * A SUBSYSTEM's path into the engine's log ring, as opposed to engineLogInfo's
 * one-liner (which logs under "Script"). A category gets its own row in the
 * editor's Internal Console, its own per-level filters and its own Solo button.
 *
 * The name is COPIED — a literal in your dylib would dangle the moment the
 * module unloads, and the engine keeps pointers to it. 0 means "no category"
 * and writing to it is a no-op.
 *
 * ALWAYS engineLogEnabled() BEFORE FORMATTING. That check is one atomic load;
 * the formatting is what costs. ENGINE_LOG in engine_api_client.h does it for
 * you. */
/* Numerically identical to elog::Level / elog::Audience. Spelled out here
 * because a module cannot see the C++ enum — and once a module ships with these
 * baked in they can never be renumbered. Appending a level is fine; reordering
 * is not. */
#define ENGINE_LOG_TRACE   0u
#define ENGINE_LOG_DEBUG   1u
#define ENGINE_LOG_INFO    2u
#define ENGINE_LOG_SUCCESS 3u
#define ENGINE_LOG_WARN    4u
#define ENGINE_LOG_ERROR   5u

/* Which console the lines surface in: a kit's gameplay diagnostics belong to the
 * game developer, a subsystem replacement's internals to whoever debugs the
 * engine. Warnings and errors reach the game console either way. */
#define ENGINE_LOG_AUDIENCE_ENGINE 0u
#define ENGINE_LOG_AUDIENCE_GAME   1u

typedef uint32_t EngineLogCat;
EngineLogCat engineLogCategory(const char* name);
int32_t      engineLogEnabled(EngineLogCat cat, uint32_t level);
void         engineLogWrite(EngineLogCat cat, uint32_t level, const char* msg);
void         engineLogSetAudience(EngineLogCat cat, uint32_t audience);

#ifdef __cplusplus
}
#endif