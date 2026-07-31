---
status: unreviewed
---
# Engine SDK — C API Reference

**Header:** `include/engine/engine_api.h` · **Last updated:** June 30, 2026

This is the engine's **stable, flat, FFI-safe surface** — the *one doorway*
through which everything outside the engine reaches in. Kits, hot-reloaded game
modules, Lua scripts, and (future) C# all call these same `engine*` functions;
none of them touch flecs / bgfx / ImGui / the logger directly.

## Who calls it, and why it's flat

```
  kits · game modules · Lua · C#         (consumers — link no engine internals)
        │  call engine* C functions
        ▼
  engine_api.cpp  →  ScriptHost / services / renderer / UI backend   (the engine)
```

- **Hot-reload modules & kits** resolve these symbols from the *host* at load,
  so the calls execute host-side against the *real* engine state (not a copy).
- The surface is plain C (`const char*`, scalars, small POD structs, opaque
  ids) so it stays ABI-stable across compilers, languages, and module reloads.

## Safety & lifetime rules

- **Always safe to call.** With no simulation world bound, world-touching calls
  no-op and return `0` / `false`. UI calls no-op when no UI backend is
  registered (e.g. under `engine_host`).
- **`EngineEntity` is a flecs entity id** (`uint64_t`, `0` = invalid), valid for
  the **current simulation session only** — never persist one across Stop/Play.
- Strings are borrowed for the duration of the call; copy if you need to keep
  them.

## Types

```c
typedef uint64_t EngineEntity;                 // 0 = invalid
typedef struct { float x, y, z; }    EngineVec3;
typedef struct { float x, y, z, w; } EngineQuat;
typedef struct { EngineVec3 position; EngineQuat rotation; EngineVec3 scale; } EngineTransform;
typedef struct {
    bool         hit;
    EngineEntity entity;       // what was hit (0 if none)
    EngineVec3   point;        // world hit point
    EngineVec3   normal;       // surface normal at the hit
    float        distance;     // along the ray
} EngineRaycastHit;
```

## Logging

| Function | Purpose |
|---|---|
| `void engineLogInfo(const char* msg)`  | info line to the engine log/console |
| `void engineLogWarn(const char* msg)`  | warning line |
| `void engineLogError(const char* msg)` | error line |

## Input

Key names are strings (`"W"`, `"Space"`, `"LeftShift"`, `"Escape"`, …). Mouse
buttons are integers (`0` = left, `1` = right, `2` = middle).

| Function | Purpose |
|---|---|
| `bool engineKeyDown(const char* key)`     | held this frame |
| `bool engineKeyPressed(const char* key)`  | pressed edge this frame |
| `float engineAxis(const char* axis)`      | named axis (e.g. `"MoveForward"`), −1..1 |
| `bool engineMouseDown(int button)`        | mouse button held |
| `void engineMouseDelta(float* dx, float* dy)` | mouse movement since last frame |

## Actions (input-agnostic gameplay input)

Gameplay binds to **actions**, never devices: the project's `input.json`
wires devices → actions (contexts, axes, scales) and is scaffolded with FPS
defaults on first run. Backed by the raw-input pipeline (`modules/hid`:
unaccelerated counts, hardware timestamps) when the platform backend is up,
window input otherwise — game code cannot tell the difference.

| Function | Purpose |
|---|---|
| `bool engineActionDown(const char* a)`     | action currently held |
| `bool engineActionPressed(const char* a)`  | edge: went down this tick |
| `bool engineActionReleased(const char* a)` | edge: went up this tick |
| `float engineActionAxis1(const char* a)`   | 1D axis (scroll, key pairs) |
| `void engineActionAxis2(const char* a, float* x, float* y)` | 2D axis (WASD, mouse motion) |
| `void engineLookDelta(float* dx, float* dy)` | **late-latch camera path**: freshest accumulated raw mouse counts, drained on read — call once per frame from camera code, apply your own sensitivity |

The old key-name polling (`engineKeyDown` etc.) still works but new code
should use actions — bindings then live in data, not kits.

## Cursor

| Function | Purpose |
|---|---|
| `void engineSetCursorCaptured(bool captured)` | lock+hide the cursor (FPS look) or release it |
| `bool engineCursorCaptured(void)`             | current capture state |

## Time

| Function | Purpose |
|---|---|
| `float engineDeltaTime(void)`  | seconds since last frame |
| `double engineElapsed(void)`   | seconds since simulation start |
| `uint64_t engineFrame(void)`   | simulation frame counter |

## Entities

| Function | Purpose |
|---|---|
| `EngineEntity engineEntityCreate(const char* name)` | create a named entity |
| `void engineEntityDestroy(EngineEntity e)`          | destroy it |
| `EngineEntity engineEntityFind(const char* name)`   | find by name (`0` if none) |
| `bool engineEntityAlive(EngineEntity e)`            | still valid? |
| `void engineEntitySetParent(EngineEntity child, EngineEntity parent)` | re-parent |
| `void engineEntityClearParent(EngineEntity child)`  | detach to world root |

## Transform

| Function | Purpose |
|---|---|
| `bool engineGetTransform(EngineEntity e, EngineTransform* out)`       | read local transform |
| `void engineSetTransform(EngineEntity e, const EngineTransform* in)`  | write local transform |

## Physics

| Function | Purpose |
|---|---|
| `void engineApplyImpulse(EngineEntity e, float x, float y, float z)` | impulse to a dynamic body |
| `void engineSetVelocity(EngineEntity e, float x, float y, float z)`  | set linear velocity |
| `bool engineGetVelocity(EngineEntity e, float* x, float* y, float* z)` | read linear velocity |
| `EngineRaycastHit engineRaycast(float ox,oy,oz, float dx,dy,dz, float maxDist)` | hitscan ray |
| `void engineCharMove(EngineEntity e, float vx, float vz)` | set a character controller's horizontal velocity |
| `void engineCharJump(EngineEntity e, float speed)`        | jump if grounded |
| `bool engineCharGrounded(EngineEntity e)`                 | is the character on the ground? |

## Audio

| Function | Purpose |
|---|---|
| `uint32_t enginePlaySound(const char* path)`                       | 2D one-shot; returns a voice handle |
| `uint32_t enginePlaySoundAt(const char* path, float x,y,z)`        | positional one-shot |
| `void engineStopSound(uint32_t handle)`                            | stop a voice |

## Animation

Skeletal animation control for entities with a `SkinnedMesh` + `Animator`.
Clip paths are project-relative (or absolute) animation files; the host binds
them to the entity's skeleton by bone name and caches the result, so repeat
plays are cheap. Switching clips crossfades automatically on the runtime's
blend path (ozz `BlendingJob`).

| Function | Purpose |
|---|---|
| `bool engineAnimPlay(EngineEntity e, const char* clipPath, float fadeSeconds)` | bind + play a clip; crossfades from the current one over `fadeSeconds` (`0` = hard cut, `<0` = keep the Animator's fade). Returns `false` if the entity has no skeleton or the clip fails to bind |
| `void engineAnimSetSpeed(EngineEntity e, float speed)`     | playback rate (negative = reverse) |
| `void engineAnimSetLooping(EngineEntity e, bool looping)`  | loop vs clamp at the end |
| `void engineAnimSetPlaying(EngineEntity e, bool playing)`  | pause / resume |
| `bool engineAnimIsPlaying(EngineEntity e)`                 | currently advancing? |
| `float engineAnimTime(EngineEntity e)`                     | current clip time (s) |
| `float engineAnimDuration(EngineEntity e)`                 | bound clip duration (s), `0` if none |

## Assets (cooked binaries)

Paths are **cooked** asset paths. Handles are `uint32_t` (`0` = invalid).

| Function | Purpose |
|---|---|
| `uint32_t engineAssetLoadMesh(const char* cookedPath)`     | load (sync), returns handle |
| `bool engineAssetUnloadMesh(uint32_t id)`                  | release |
| `uint32_t engineAssetLoadTexture(const char* cookedPath)`  | load (sync) |
| `bool engineAssetUnloadTexture(uint32_t id)`               | release |
| `void engineAssetLoadMeshAsync(const char* cookedPath)`    | kick off background load |
| `void engineAssetLoadTextureAsync(const char* cookedPath)` | kick off background load |
| `uint32_t engineAssetQueryMesh(const char* cookedPath)`    | handle if already resident, else `0` |
| `uint32_t engineAssetQueryTexture(const char* cookedPath)` | handle if already resident, else `0` |
| `bool engineAssetIsLoading(const char* cookedPath)`        | async load in flight? |

## Scenes (cooked binaries)

| Function | Purpose |
|---|---|
| `uint32_t engineSceneLoad(const char* cookedPath)` | load additively, returns handle |
| `bool engineSceneUnload(uint32_t handle)`          | unload |
| `void engineScenePreload(const char* cookedPath)`  | warm caches without instantiating |
| `bool engineSceneIsReady(const char* cookedPath)`  | preloaded & ready? |

## Editor UI facade

Immediate-mode widgets a kit/plugin draws from `IEnginePlugin::onEditorUI()`
**without touching ImGui**. The host registers a backend (the editor, over
ImGui); where there is no UI surface (`engine_host`, headless) these no-op. See
[Drawing your own editor UI](#drawing-your-own-editor-ui).

| Function | Purpose |
|---|---|
| `void engineUiSetBackend(const EngineUiBackend* backend)` | **host-only** — register/clear the backend |
| `void engineUiText(const char* fmt, ...)`                | label / formatted text |
| `bool engineUiButton(const char* label)`                 | button; `true` the frame it's clicked |
| `bool engineUiSliderFloat(const char* label, float* v, float lo, float hi)` | slider; `true` while editing |
| `bool engineUiCheckbox(const char* label, bool* v)`      | checkbox; `true` on toggle |
| `void engineUiSeparator(void)`                           | horizontal rule |

## Debug draw

Immediate-mode 3D wireframes for **this frame only** — a kit queues shapes from
its per-frame code; the renderer draws them into every world view and clears the
queue next frame. Color is 0..1 RGB. Works in any host with a renderer (editor +
`engine_host`); no-ops headless. This is the 3D counterpart to the UI facade —
the tool for damage radii, AI/trajectory overlays, motion-matching debuggers.

| Function | Purpose |
|---|---|
| `void engineDrawLine(float x0,y0,z0, float x1,y1,z1, float r,g,b)` | a segment |
| `void engineDrawSphere(float cx,cy,cz, float radius, float r,g,b)` | 3 great-circle wire sphere |
| `void engineDrawBox(float cx,cy,cz, float hx,hy,hz, float r,g,b)`   | wire box (half-extents) |
| `void engineDrawDisk(float cx,cy,cz, float nx,ny,nz, float radius, float r,g,b)` | wire circle in a plane |

```cpp
void onUpdate(flecs::world&, float) override {
    engineDrawSphere(px, py, pz, blastRadius, 1.0f, 0.3f, 0.1f);  // damage radius
}
```

### Drawing your own editor UI

A kit overrides `onEditorUI()` and draws with `engineUi*` only:

```cpp
void onEditorUI() override {
    engineUiText("Damage rule: flat x multiplier");
    engineUiSliderFloat("Damage multiplier", &m_mult, 0.0f, 5.0f);
}
```

The editor calls this for every loaded plugin/kit (in the Plug-in Manager) and
routes the widgets to ImGui. The kit links no UI code and loads identically in
`engine_host`, where the same calls simply do nothing.

## Related

- **Module contract** (how a kit/game exposes itself as a loadable module):
  `include/engine/game_module.h`.
- **Scripting layer** (Lua over the same surface): `docs/scripting-api.md`.
