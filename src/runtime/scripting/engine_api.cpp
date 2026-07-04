// Implementation of the engine C API (include/engine/engine_api.h).
// A thin extern "C" veneer over the runtime's canonical ScriptHost — the
// same object the Lua bindings drive, so every language sees identical
// semantics. Compiled into engine_runtime; symbols export from the host
// executable so hot-reloaded game modules resolve them at load time.
#include <engine/engine_api.h>
#include "runtime/scripting/engine_api_binding.h"
#include "runtime/input/input_manager.h"
#include "runtime/scripting/script_host.h"
#include <cstdarg>
#include <cstdio>

static ScriptHost* g_host = nullptr;

void engineApiBindHost(ScriptHost* host) { g_host = host; }

// World-touching calls additionally need a bound simulation world.
static ScriptHost* hostWithWorld() {
    return (g_host && g_host->world()) ? g_host : nullptr;
}
static flecs::entity resolve(ScriptHost* h, EngineEntity e) {
    return h->world()->entity((flecs::entity_t)e);
}

// ── Log ──────────────────────────────────────────────────────────────────────
void engineLogInfo (const char* m) { if (g_host && m) g_host->logInfo(m); }
void engineLogWarn (const char* m) { if (g_host && m) g_host->logWarn(m); }
void engineLogError(const char* m) { if (g_host && m) g_host->logError(m); }

// ── Input ────────────────────────────────────────────────────────────────────
bool  engineKeyDown   (const char* k) { return g_host && k ? g_host->keyDown(k)    : false; }
bool  engineKeyPressed(const char* k) {
    static bool warned = []{ LOG_WARN("EngineApi", "engineKeyPressed is DEPRECATED — bind an action in input.json and use engineAction*"); return true; }();
    (void)warned; return g_host && k ? g_host->keyPressed(k) : false; }
float engineAxis      (const char* a) { return g_host && a ? g_host->axis(a)       : 0.0f; }
bool  engineMouseDown (int b)         { return g_host ? g_host->mouseDown(b)       : false; }
void  engineMouseDelta(float* dx, float* dy) {
    static bool warned = []{ LOG_WARN("EngineApi", "engineMouseDelta is DEPRECATED — bind an action in input.json and use engineAction*"); return true; }();
    (void)warned;
    float x = 0.0f, y = 0.0f;
    if (g_host) g_host->mouseDelta(x, y);
    if (dx) *dx = x;
    if (dy) *dy = y;
}

// ── Cursor ───────────────────────────────────────────────────────────────────
void engineSetCursorCaptured(bool c) { if (g_host) g_host->setCursorCaptured(c); }
bool engineCursorCaptured(void)       { return g_host ? g_host->cursorCaptured() : false; }

// ── Time ─────────────────────────────────────────────────────────────────────
float    engineDeltaTime(void) { return g_host ? g_host->dt()      : 0.0f; }
double   engineElapsed(void)   { return g_host ? g_host->elapsed() : 0.0;  }
uint64_t engineFrame(void)     { return g_host ? g_host->frame()   : 0;    }

// ── Entities ─────────────────────────────────────────────────────────────────
EngineEntity engineEntityCreate(const char* name) {
    ScriptHost* h = hostWithWorld();
    return (h && name) ? (EngineEntity)h->create(name).id() : 0;
}
void engineEntityDestroy(EngineEntity e) {
    if (ScriptHost* h = hostWithWorld()) h->destroy(resolve(h, e));
}
EngineEntity engineEntityFind(const char* name) {
    ScriptHost* h = hostWithWorld();
    return (h && name) ? (EngineEntity)h->find(name).id() : 0;
}
bool engineEntityAlive(EngineEntity e) {
    ScriptHost* h = hostWithWorld();
    return h ? h->isAlive(resolve(h, e)) : false;
}
void engineEntitySetParent(EngineEntity c, EngineEntity p) {
    if (ScriptHost* h = hostWithWorld())
        h->setParent(resolve(h, c), resolve(h, p));
}
void engineEntityClearParent(EngineEntity c) {
    if (ScriptHost* h = hostWithWorld()) h->clearParent(resolve(h, c));
}

// ── Transform ────────────────────────────────────────────────────────────────
bool engineGetTransform(EngineEntity e, EngineTransform* out) {
    ScriptHost* h = hostWithWorld();
    if (!h || !out) return false;
    Transform t;
    if (!h->getTransform(resolve(h, e), t)) return false;
    out->position = { t.position.x, t.position.y, t.position.z };
    out->rotation = { t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w };
    out->scale    = { t.scale.x, t.scale.y, t.scale.z };
    return true;
}
void engineSetTransform(EngineEntity e, const EngineTransform* in) {
    ScriptHost* h = hostWithWorld();
    if (!h || !in) return;
    Transform t;
    t.position = { in->position.x, in->position.y, in->position.z };
    t.rotation = { in->rotation.x, in->rotation.y, in->rotation.z, in->rotation.w };
    t.scale    = { in->scale.x, in->scale.y, in->scale.z };
    h->setTransform(resolve(h, e), t);
}

// ── Physics ──────────────────────────────────────────────────────────────────
void engineApplyImpulse(EngineEntity e, float x, float y, float z) {
    if (ScriptHost* h = hostWithWorld()) h->applyImpulse(resolve(h, e), x, y, z);
}
void engineSetVelocity(EngineEntity e, float x, float y, float z) {
    if (ScriptHost* h = hostWithWorld()) h->setVelocity(resolve(h, e), x, y, z);
}
bool engineGetVelocity(EngineEntity e, float* x, float* y, float* z) {
    ScriptHost* h = hostWithWorld();
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    bool ok = h ? h->getVelocity(resolve(h, e), vx, vy, vz) : false;
    if (x) *x = vx;
    if (y) *y = vy;
    if (z) *z = vz;
    return ok;
}
EngineRaycastHit engineRaycast(float ox, float oy, float oz,
                               float dx, float dy, float dz, float maxDist) {
    EngineRaycastHit out = {};
    if (ScriptHost* h = hostWithWorld()) {
        RaycastHit r = h->raycast(ox, oy, oz, dx, dy, dz, maxDist);
        out.hit      = r.hit;
        out.entity   = (EngineEntity)r.entity;
        out.point    = { r.point[0], r.point[1], r.point[2] };
        out.normal   = { r.normal[0], r.normal[1], r.normal[2] };
        out.distance = r.distance;
    }
    return out;
}
void engineCharMove(EngineEntity e, float vx, float vz) {
    if (ScriptHost* h = hostWithWorld()) h->charMove(resolve(h, e), vx, vz);
}
void engineCharJump(EngineEntity e, float speed) {
    if (ScriptHost* h = hostWithWorld()) h->charJump(resolve(h, e), speed);
}
bool engineCharGrounded(EngineEntity e) {
    ScriptHost* h = hostWithWorld();
    return h ? h->charGrounded(resolve(h, e)) : false;
}

// ── Audio ────────────────────────────────────────────────────────────────────
uint32_t enginePlaySound(const char* p) {
    return (g_host && p) ? g_host->playSound(p) : 0;
}
uint32_t enginePlaySoundAt(const char* p, float x, float y, float z) {
    return (g_host && p) ? g_host->playSoundAt(p, x, y, z) : 0;
}
void engineStopSound(uint32_t handle) { if (g_host) g_host->stopSound(handle); }

/* ── Navigation ─────────────────────────────────────────────────────────── */
int engineNavFindPath(float sx, float sy, float sz, float ex, float ey, float ez,
                      float* out, int maxPoints) {
    return (g_host && out && maxPoints > 0)
        ? g_host->navFindPath(sx, sy, sz, ex, ey, ez, out, maxPoints) : 0;
}
bool engineNavProject(float x, float y, float z, float* out) {
    return g_host && out && g_host->navProject(x, y, z, out);
}
bool engineNavReady(void) { return g_host && g_host->navReady(); }

// ── Assets ───────────────────────────────────────────────────────────────────
uint32_t engineAssetLoadMesh(const char* p)       { return (g_host && p) ? g_host->assetLoadMesh(p) : 0; }
bool     engineAssetUnloadMesh(uint32_t id)       { return g_host ? g_host->assetUnloadMesh(id) : false; }
uint32_t engineAssetLoadTexture(const char* p)    { return (g_host && p) ? g_host->assetLoadTexture(p) : 0; }
bool     engineAssetUnloadTexture(uint32_t id)    { return g_host ? g_host->assetUnloadTexture(id) : false; }
void     engineAssetLoadMeshAsync(const char* p)  { if (g_host && p) g_host->assetLoadMeshAsync(p); }
void     engineAssetLoadTextureAsync(const char* p) { if (g_host && p) g_host->assetLoadTextureAsync(p); }
uint32_t engineAssetQueryMesh(const char* p)      { return (g_host && p) ? g_host->assetQueryMesh(p) : 0; }
uint32_t engineAssetQueryTexture(const char* p)   { return (g_host && p) ? g_host->assetQueryTexture(p) : 0; }
bool     engineAssetIsLoading(const char* p)      { return (g_host && p) ? g_host->assetIsLoading(p) : false; }

// ── Actions — routed to the runtime's InputManager ────────────────────────
static input::InputManager* g_input = nullptr;
void engineInputBindManager(input::InputManager* m) { g_input = m; }

bool  engineActionDown(const char* a)     { return g_input && a && g_input->actionDown(a); }
bool  engineActionPressed(const char* a)  { return g_input && a && g_input->actionPressed(a); }
bool  engineActionReleased(const char* a) { return g_input && a && g_input->actionReleased(a); }
float engineActionAxis1(const char* a)    { return (g_input && a) ? g_input->axis1(a) : 0.0f; }
void  engineActionAxis2(const char* a, float* x, float* y) {
    if (g_input && a) { g_input->axis2(a, x, y); return; }
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}
void  engineLookDelta(float* dx, float* dy) {
    if (g_input) { g_input->consumeLook(dx, dy); return; }
    if (dx) *dx = 0.0f;
    if (dy) *dy = 0.0f;
}
void  engineLookTotal(double* x, double* y) {
    if (g_input) { g_input->lookTotal(x, y); return; }
    if (x) *x = 0.0;
    if (y) *y = 0.0;
}

// ── Editor UI facade ───────────────────────────────────────────────────────
// Routed to a host-registered backend (the editor, over ImGui). No backend
// (engine_host / headless) => no-op. The kit never sees ImGui.
static const EngineUiBackend* g_ui = nullptr;
void engineUiSetBackend(const EngineUiBackend* backend) { g_ui = backend; }
// Host-internal: is a UI widget backend live right now? Drives live capability
// negotiation — engineApiHostTable() publishes ui.version = 0 (group ABSENT)
// when no backend is registered, so a kit binding in a headless host (engine_
// host) sees ui as unavailable and skips building its tuning panel.
bool engineUiHasBackend(void) { return g_ui != nullptr; }

void engineUiText(const char* fmt, ...) {
    if (!g_ui || !g_ui->text || !fmt) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_ui->text(buf);
}
bool engineUiButton(const char* label) {
    return (g_ui && g_ui->button && label) ? g_ui->button(label) : false;
}
bool engineUiSliderFloat(const char* label, float* v, float lo, float hi) {
    return (g_ui && g_ui->sliderFloat && label && v) ? g_ui->sliderFloat(label, v, lo, hi) : false;
}
bool engineUiCheckbox(const char* label, bool* v) {
    return (g_ui && g_ui->checkbox && label && v) ? g_ui->checkbox(label, v) : false;
}
void engineUiSeparator(void) { if (g_ui && g_ui->separator) g_ui->separator(); }

// ── Animation ────────────────────────────────────────────────────────────────
bool engineAnimPlay(EngineEntity e, const char* clipPath, float fadeSeconds) {
    ScriptHost* h = hostWithWorld();
    return (h && clipPath) ? h->animPlay(resolve(h, e), clipPath, fadeSeconds) : false;
}
void engineAnimSetSpeed(EngineEntity e, float s) {
    if (ScriptHost* h = hostWithWorld()) h->animSetSpeed(resolve(h, e), s);
}
void engineAnimSetLooping(EngineEntity e, bool loop) {
    if (ScriptHost* h = hostWithWorld()) h->animSetLooping(resolve(h, e), loop);
}
void engineAnimSetPlaying(EngineEntity e, bool playing) {
    if (ScriptHost* h = hostWithWorld()) h->animSetPlaying(resolve(h, e), playing);
}
bool engineAnimIsPlaying(EngineEntity e) {
    ScriptHost* h = hostWithWorld();
    return h ? h->animIsPlaying(resolve(h, e)) : false;
}
float engineAnimTime(EngineEntity e) {
    ScriptHost* h = hostWithWorld();
    return h ? h->animTime(resolve(h, e)) : 0.0f;
}
float engineAnimDuration(EngineEntity e) {
    ScriptHost* h = hostWithWorld();
    return h ? h->animDuration(resolve(h, e)) : 0.0f;
}

// ── Debug draw ───────────────────────────────────────────────────────────────
// Route to the runtime's per-frame line collector (via ScriptHost). No collector
// (headless) => no-op.
void engineDrawLine(float x0, float y0, float z0, float x1, float y1, float z1,
                    float r, float g, float b) {
    if (!g_host) return;
    if (dbg::DebugDraw* d = g_host->debugDraw())
        d->line({x0, y0, z0}, {x1, y1, z1}, dbg::packRGBA(r, g, b));
}
void engineDrawSphere(float cx, float cy, float cz, float radius,
                      float r, float g, float b) {
    if (!g_host) return;
    if (dbg::DebugDraw* d = g_host->debugDraw())
        d->sphere({cx, cy, cz}, radius, dbg::packRGBA(r, g, b));
}
void engineDrawBox(float cx, float cy, float cz, float hx, float hy, float hz,
                   float r, float g, float b) {
    if (!g_host) return;
    if (dbg::DebugDraw* d = g_host->debugDraw())
        d->box({cx, cy, cz}, {hx, hy, hz}, dbg::packRGBA(r, g, b));
}
void engineDrawDisk(float cx, float cy, float cz, float nx, float ny, float nz,
                    float radius, float r, float g, float b) {
    if (!g_host) return;
    if (dbg::DebugDraw* d = g_host->debugDraw())
        d->disk({cx, cy, cz}, {nx, ny, nz}, radius, dbg::packRGBA(r, g, b));
}

// ── Scenes ───────────────────────────────────────────────────────────────────
uint32_t engineSceneLoad(const char* p)    { return (g_host && p) ? g_host->sceneLoad(p) : 0; }
bool     engineSceneUnload(uint32_t h)     { return g_host ? g_host->sceneUnload(h) : false; }
void     engineScenePreload(const char* p) { if (g_host && p) g_host->scenePreload(p); }
bool     engineSceneIsReady(const char* p) { return (g_host && p) ? g_host->sceneIsReady(p) : false; }
