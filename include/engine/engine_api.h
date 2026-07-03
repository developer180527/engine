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

#ifdef __cplusplus
}
#endif
