#pragma once
/* ── EngineApi client shim — include ONCE in a module's main TU ──────────────
 *
 * Turns every engine* call in this module into a dispatch through the host's
 * EngineApiTableV1 instead of a dynamically-looked-up symbol. Kit SOURCE code
 * does not change — the flat functions keep their names and signatures; this
 * shim DEFINES them locally as forwarders, so the linker binds calls here and
 * the dynamic linker is out of the picture (portable to Windows, explicit,
 * versioned).
 *
 *     // module.cpp
 *     #include "my_kit.h"
 *     #include <engine/game_module.h>
 *     #include <engine/engine_api_client.h>
 *     ENGINE_GAME_MODULE(MyKit)
 *
 * PER-SUBSYSTEM versioning: bind time compares each group's version against
 * the constants THIS module compiled with. A matching group works normally.
 * A mismatched group disables only ITS functions — first use logs an error
 * naming the group, calls return safe defaults — while every other group
 * keeps working. A physics-only kit survives an animation API bump.
 */
#include <stdarg.h>
#include <stdio.h>

#include "engine_api.h"
#include "engine_api_table.h"

/* Module-local state — one per dylib, invisible outside it. */
static const EngineApiTableV1* g_eapi = nullptr;
static bool g_eapiOk[9] = {};         /* core,input,physics,audio,assets,anim,ui,nav,draw */

enum { EAPI_CORE, EAPI_INPUT, EAPI_PHYSICS, EAPI_AUDIO,
       EAPI_ASSETS, EAPI_ANIM, EAPI_UI, EAPI_NAV, EAPI_DRAW, EAPI_COUNT };

/* Capability query — true when the group is bound AND version-compatible.
 * Groups published with version 0 are ABSENT by convention. */
static inline bool engineApiHas(int group) {
    return g_eapi && group >= 0 && group < EAPI_COUNT && g_eapiOk[group];
}

static bool eapiGuard(int group, const char* name) {
    if (g_eapi && g_eapiOk[group]) return true;
    static bool warned[7] = {};
    if (!warned[group]) {
        warned[group] = true;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "engine API group unavailable for '%s' — %s",
                 name, g_eapi ? "version mismatch at bind (see load log)"
                              : "module was never bound (old host?)");
        if (g_eapi && g_eapi->core.logError) g_eapi->core.logError(buf);
        else fprintf(stderr, "[EngineApi] %s\n", buf);
    }
    return false;
}

extern "C" __attribute__((visibility("default")))
void engineModuleBindApiV1(const EngineApiTableV1* t) {
    if (!t || t->structSize != sizeof(EngineApiTableV1)) {
        fprintf(stderr, "[EngineApi] table size mismatch — rebuild module\n");
        return;
    }
    g_eapi = t;
    struct { int idx; uint32_t have, want; const char* name; } checks[] = {
        { EAPI_CORE,    t->core.version,    ENGINE_API_CORE_V,    "core"    },
        { EAPI_INPUT,   t->input.version,   ENGINE_API_INPUT_V,   "input"   },
        { EAPI_PHYSICS, t->physics.version, ENGINE_API_PHYSICS_V, "physics" },
        { EAPI_AUDIO,   t->audio.version,   ENGINE_API_AUDIO_V,   "audio"   },
        { EAPI_ASSETS,  t->assets.version,  ENGINE_API_ASSETS_V,  "assets"  },
        { EAPI_ANIM,    t->anim.version,    ENGINE_API_ANIM_V,    "anim"    },
        { EAPI_UI,      t->ui.version,      ENGINE_API_UI_V,      "ui"      },
        { EAPI_NAV,     t->nav.version,     ENGINE_API_NAV_V,     "nav"     },
        { EAPI_DRAW,    t->draw.version,    ENGINE_API_DRAW_V,    "draw"    },
    };
    for (auto& c : checks) {
        g_eapiOk[c.idx] = (c.have == c.want);
        /* v0 is the ABSENT convention — the host doesn't offer this group at
         * all (a headless host has no ui, etc.). That's negotiated capability,
         * not breakage, so stay silent; the kit adapts via engineApiHas(). Only
         * a NONZERO version that disagrees is a genuine skew worth warning on. */
        if (!g_eapiOk[c.idx] && c.have != 0 && t->core.logWarn) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "API group '%s' v%u != module's v%u — group disabled",
                     c.name, c.have, c.want);
            t->core.logWarn(buf);
        }
    }
}

/* ── Forwarders (same names/signatures as engine_api.h declares) ──────────── */
extern "C" {

/* core */
void engineLogInfo (const char* m) { if (eapiGuard(EAPI_CORE,"logInfo"))  g_eapi->core.logInfo(m); }
void engineLogWarn (const char* m) { if (eapiGuard(EAPI_CORE,"logWarn"))  g_eapi->core.logWarn(m); }
void engineLogError(const char* m) { if (eapiGuard(EAPI_CORE,"logError")) g_eapi->core.logError(m); }
float    engineDeltaTime(void) { return eapiGuard(EAPI_CORE,"deltaTime") ? g_eapi->core.deltaTime() : 0.0f; }
double   engineElapsed(void)   { return eapiGuard(EAPI_CORE,"elapsed")   ? g_eapi->core.elapsed()   : 0.0;  }
uint64_t engineFrame(void)     { return eapiGuard(EAPI_CORE,"frame")     ? g_eapi->core.frame()     : 0;    }
EngineEntity engineEntityCreate(const char* n) { return eapiGuard(EAPI_CORE,"entityCreate") ? g_eapi->core.entityCreate(n) : 0; }
void         engineEntityDestroy(EngineEntity e) { if (eapiGuard(EAPI_CORE,"entityDestroy")) g_eapi->core.entityDestroy(e); }
EngineEntity engineEntityFind(const char* n) { return eapiGuard(EAPI_CORE,"entityFind") ? g_eapi->core.entityFind(n) : 0; }
bool         engineEntityAlive(EngineEntity e) { return eapiGuard(EAPI_CORE,"entityAlive") && g_eapi->core.entityAlive(e); }
void engineEntitySetParent(EngineEntity c, EngineEntity p) { if (eapiGuard(EAPI_CORE,"entitySetParent")) g_eapi->core.entitySetParent(c, p); }
void engineEntityClearParent(EngineEntity c) { if (eapiGuard(EAPI_CORE,"entityClearParent")) g_eapi->core.entityClearParent(c); }
bool engineGetTransform(EngineEntity e, EngineTransform* o) { return eapiGuard(EAPI_CORE,"getTransform") && g_eapi->core.getTransform(e, o); }
void engineSetTransform(EngineEntity e, const EngineTransform* t) { if (eapiGuard(EAPI_CORE,"setTransform")) g_eapi->core.setTransform(e, t); }

/* input */
bool  engineKeyDown(const char* k)    { return eapiGuard(EAPI_INPUT,"keyDown") && g_eapi->input.keyDown(k); }
bool  engineKeyPressed(const char* k) { return eapiGuard(EAPI_INPUT,"keyPressed") && g_eapi->input.keyPressed(k); }
float engineAxis(const char* a)       { return eapiGuard(EAPI_INPUT,"axis") ? g_eapi->input.axis(a) : 0.0f; }
bool  engineMouseDown(int b)          { return eapiGuard(EAPI_INPUT,"mouseDown") && g_eapi->input.mouseDown(b); }
void  engineMouseDelta(float* dx, float* dy) {
    if (eapiGuard(EAPI_INPUT,"mouseDelta")) { g_eapi->input.mouseDelta(dx, dy); return; }
    if (dx) *dx = 0.0f;
    if (dy) *dy = 0.0f;
}
bool  engineActionDown(const char* a)     { return eapiGuard(EAPI_INPUT,"actionDown") && g_eapi->input.actionDown(a); }
bool  engineActionPressed(const char* a)  { return eapiGuard(EAPI_INPUT,"actionPressed") && g_eapi->input.actionPressed(a); }
bool  engineActionReleased(const char* a) { return eapiGuard(EAPI_INPUT,"actionReleased") && g_eapi->input.actionReleased(a); }
float engineActionAxis1(const char* a)    { return eapiGuard(EAPI_INPUT,"actionAxis1") ? g_eapi->input.actionAxis1(a) : 0.0f; }
void  engineActionAxis2(const char* a, float* x, float* y) {
    if (eapiGuard(EAPI_INPUT,"actionAxis2")) { g_eapi->input.actionAxis2(a, x, y); return; }
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}
void  engineLookDelta(float* dx, float* dy) {
    if (eapiGuard(EAPI_INPUT,"lookDelta")) { g_eapi->input.lookDelta(dx, dy); return; }
    if (dx) *dx = 0.0f;
    if (dy) *dy = 0.0f;
}
void engineLookTotal(double* x, double* y) {
    if (eapiGuard(EAPI_INPUT,"lookTotal") && g_eapi->input.lookTotal) {
        g_eapi->input.lookTotal(x, y); return;
    }
    if (x) *x = 0.0;
    if (y) *y = 0.0;
}
void engineSetCursorCaptured(bool c) { if (eapiGuard(EAPI_INPUT,"setCursorCaptured")) g_eapi->input.setCursorCaptured(c); }
bool engineCursorCaptured(void)      { return eapiGuard(EAPI_INPUT,"cursorCaptured") && g_eapi->input.cursorCaptured(); }

/* physics */
void engineApplyImpulse(EngineEntity e, float x, float y, float z) { if (eapiGuard(EAPI_PHYSICS,"applyImpulse")) g_eapi->physics.applyImpulse(e, x, y, z); }
void engineSetVelocity(EngineEntity e, float x, float y, float z)  { if (eapiGuard(EAPI_PHYSICS,"setVelocity")) g_eapi->physics.setVelocity(e, x, y, z); }
bool engineGetVelocity(EngineEntity e, float* x, float* y, float* z) { return eapiGuard(EAPI_PHYSICS,"getVelocity") && g_eapi->physics.getVelocity(e, x, y, z); }
EngineRaycastHit engineRaycast(float ox, float oy, float oz,
                               float dx, float dy, float dz, float maxDist) {
    if (eapiGuard(EAPI_PHYSICS,"raycast"))
        return g_eapi->physics.raycast(ox, oy, oz, dx, dy, dz, maxDist);
    EngineRaycastHit h; h.hit = false; h.entity = 0;
    h.point.x = h.point.y = h.point.z = 0.0f;
    h.normal.x = h.normal.y = h.normal.z = 0.0f;
    h.distance = 0.0f;
    return h;
}
void engineCharMove(EngineEntity e, float vx, float vz) { if (eapiGuard(EAPI_PHYSICS,"charMove")) g_eapi->physics.charMove(e, vx, vz); }
void engineCharJump(EngineEntity e, float s) { if (eapiGuard(EAPI_PHYSICS,"charJump")) g_eapi->physics.charJump(e, s); }
bool engineCharGrounded(EngineEntity e) { return eapiGuard(EAPI_PHYSICS,"charGrounded") && g_eapi->physics.charGrounded(e); }

/* audio */
uint32_t enginePlaySound(const char* p) { return eapiGuard(EAPI_AUDIO,"playSound") ? g_eapi->audio.playSound(p) : 0; }
uint32_t enginePlaySoundAt(const char* p, float x, float y, float z) { return eapiGuard(EAPI_AUDIO,"playSoundAt") ? g_eapi->audio.playSoundAt(p, x, y, z) : 0; }
void     engineStopSound(uint32_t h) { if (eapiGuard(EAPI_AUDIO,"stopSound")) g_eapi->audio.stopSound(h); }

/* nav */
int  engineNavFindPath(float sx, float sy, float sz, float ex, float ey, float ez, float* out, int maxPoints) {
    return eapiGuard(EAPI_NAV,"navFindPath") ? g_eapi->nav.findPath(sx,sy,sz,ex,ey,ez,out,maxPoints) : 0; }
bool engineNavProject(float x, float y, float z, float* out) {
    return eapiGuard(EAPI_NAV,"navProject") && g_eapi->nav.project(x,y,z,out); }
bool engineNavReady(void) { return eapiGuard(EAPI_NAV,"navReady") && g_eapi->nav.ready(); }

/* assets + scenes */
uint32_t engineAssetLoadMesh(const char* p)     { return eapiGuard(EAPI_ASSETS,"loadMesh") ? g_eapi->assets.loadMesh(p) : 0; }
bool     engineAssetUnloadMesh(uint32_t h)      { return eapiGuard(EAPI_ASSETS,"unloadMesh") && g_eapi->assets.unloadMesh(h); }
uint32_t engineAssetLoadTexture(const char* p)  { return eapiGuard(EAPI_ASSETS,"loadTexture") ? g_eapi->assets.loadTexture(p) : 0; }
bool     engineAssetUnloadTexture(uint32_t h)   { return eapiGuard(EAPI_ASSETS,"unloadTexture") && g_eapi->assets.unloadTexture(h); }
void     engineAssetLoadMeshAsync(const char* p)    { if (eapiGuard(EAPI_ASSETS,"loadMeshAsync")) g_eapi->assets.loadMeshAsync(p); }
void     engineAssetLoadTextureAsync(const char* p) { if (eapiGuard(EAPI_ASSETS,"loadTextureAsync")) g_eapi->assets.loadTextureAsync(p); }
uint32_t engineAssetQueryMesh(const char* p)    { return eapiGuard(EAPI_ASSETS,"queryMesh") ? g_eapi->assets.queryMesh(p) : 0; }
uint32_t engineAssetQueryTexture(const char* p) { return eapiGuard(EAPI_ASSETS,"queryTexture") ? g_eapi->assets.queryTexture(p) : 0; }
bool     engineAssetIsLoading(const char* p)    { return eapiGuard(EAPI_ASSETS,"isLoading") && g_eapi->assets.isLoading(p); }
uint32_t engineSceneLoad(const char* p)    { return eapiGuard(EAPI_ASSETS,"sceneLoad") ? g_eapi->assets.sceneLoad(p) : 0; }
bool     engineSceneUnload(uint32_t h)     { return eapiGuard(EAPI_ASSETS,"sceneUnload") && g_eapi->assets.sceneUnload(h); }
void     engineScenePreload(const char* p) { if (eapiGuard(EAPI_ASSETS,"scenePreload")) g_eapi->assets.scenePreload(p); }
bool     engineSceneIsReady(const char* p) { return eapiGuard(EAPI_ASSETS,"sceneIsReady") && g_eapi->assets.sceneIsReady(p); }

/* anim */
bool  engineAnimPlay(EngineEntity e, const char* c, float f) { return eapiGuard(EAPI_ANIM,"play") && g_eapi->anim.play(e, c, f); }
void  engineAnimSetSpeed(EngineEntity e, float s)   { if (eapiGuard(EAPI_ANIM,"setSpeed")) g_eapi->anim.setSpeed(e, s); }
void  engineAnimSetLooping(EngineEntity e, bool l)  { if (eapiGuard(EAPI_ANIM,"setLooping")) g_eapi->anim.setLooping(e, l); }
void  engineAnimSetPlaying(EngineEntity e, bool p)  { if (eapiGuard(EAPI_ANIM,"setPlaying")) g_eapi->anim.setPlaying(e, p); }
bool  engineAnimIsPlaying(EngineEntity e) { return eapiGuard(EAPI_ANIM,"isPlaying") && g_eapi->anim.isPlaying(e); }
float engineAnimTime(EngineEntity e)      { return eapiGuard(EAPI_ANIM,"time") ? g_eapi->anim.time(e) : 0.0f; }
float engineAnimDuration(EngineEntity e)  { return eapiGuard(EAPI_ANIM,"duration") ? g_eapi->anim.duration(e) : 0.0f; }

/* ui facade + debug draw (variadic text formatted HERE — table is plain) */
void engineUiText(const char* fmt, ...) {
    if (!eapiGuard(EAPI_UI,"text") || !fmt) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_eapi->ui.text(buf);
}
bool engineUiButton(const char* l) { return eapiGuard(EAPI_UI,"button") && g_eapi->ui.button(l); }
bool engineUiSliderFloat(const char* l, float* v, float lo, float hi) { return eapiGuard(EAPI_UI,"sliderFloat") && g_eapi->ui.sliderFloat(l, v, lo, hi); }
bool engineUiCheckbox(const char* l, bool* v) { return eapiGuard(EAPI_UI,"checkbox") && g_eapi->ui.checkbox(l, v); }
void engineUiSeparator(void) { if (eapiGuard(EAPI_UI,"separator")) g_eapi->ui.separator(); }
/* draw (debug draw — its own group, works in any host with a renderer) */
void engineDrawLine(float x0, float y0, float z0, float x1, float y1, float z1,
                    float r, float g, float b) {
    if (eapiGuard(EAPI_DRAW,"drawLine")) g_eapi->draw.drawLine(x0, y0, z0, x1, y1, z1, r, g, b);
}
void engineDrawSphere(float cx, float cy, float cz, float rad,
                      float r, float g, float b) {
    if (eapiGuard(EAPI_DRAW,"drawSphere")) g_eapi->draw.drawSphere(cx, cy, cz, rad, r, g, b);
}
void engineDrawBox(float cx, float cy, float cz, float hx, float hy, float hz,
                   float r, float g, float b) {
    if (eapiGuard(EAPI_DRAW,"drawBox")) g_eapi->draw.drawBox(cx, cy, cz, hx, hy, hz, r, g, b);
}
void engineDrawDisk(float cx, float cy, float cz, float nx, float ny, float nz,
                    float rad, float r, float g, float b) {
    if (eapiGuard(EAPI_DRAW,"drawDisk")) g_eapi->draw.drawDisk(cx, cy, cz, nx, ny, nz, rad, r, g, b);
}

} /* extern "C" */
