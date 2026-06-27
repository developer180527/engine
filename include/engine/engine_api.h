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

/* ── Input (key names: "W", "Space", "LeftShift", ... — GLFW-style) ──────── */
bool  engineKeyDown   (const char* key);
bool  engineKeyPressed(const char* key);
float engineAxis      (const char* axisName);   /* bound via InputMap */
bool  engineMouseDown (int button);             /* 0=left 1=right 2=middle */
void  engineMouseDelta(float* dx, float* dy);

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

/* ── Audio (no-op until an audio plugin is attached) ─────────────────────── */
uint32_t enginePlaySound  (const char* path);
uint32_t enginePlaySoundAt(const char* path, float x, float y, float z);
void     engineStopSound  (uint32_t handle);

/* ── Assets (cooked binaries; returns handle ids, 0 = invalid) ───────────── */
uint32_t engineAssetLoadMesh      (const char* cookedPath);
bool     engineAssetUnloadMesh    (uint32_t handleId);
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

#ifdef __cplusplus
}
#endif
