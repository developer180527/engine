---
status: unreviewed
---
# Scripting

One scripting surface, multiple languages. The runtime owns a single
`ScriptHost` (`src/runtime/scripting/script_host.h`); every language is a
frontend over it:

| Frontend | Status | Consumes |
|----------|--------|----------|
| **Lua 5.4** | shipping | `LuaScriptPlugin` → ScriptHost |
| **C API** | shipping | `include/engine/engine_api.h` → ScriptHost (game modules, FFI) |
| **C#** | planned | CoreCLR host → the same C API |

The host binds to the simulation world at `startSimulation` and goes dormant
at stop — all calls no-op safely outside a session.

## Lua

Attach `LuaScriptPlugin`, then either put a `ScriptComponent` (script path,
project-relative) on an entity, or drop `.lua` files into
`assets/scripts/autorun/` (one headless entity per file).

A script returns a table of methods. State lives on `self`; `self.entity` is
the entity handle:

```lua
local M = {}

function M:onStart()
    self.speed = 2.0
    Log.info("hello from " .. self.entity:name())
end

function M:onUpdate(dt)
    local t = self.entity:getTransform()
    t.position.y = t.position.y + self.speed * dt
    self.entity:setTransform(t)
end

function M:onCollisionEnter(other) end
function M:onCollisionExit(other)  end
function M:onDestroy() end

return M
```

**Live reload:** editing any loaded `.lua` while the simulation runs rebuilds
all script instances from fresh source within ~1 second (`onDestroy` →
`onStart`; script-local state resets, entities and the world are untouched).

**Sandbox:** only base/table/string/math/coroutine are open. No `io`, `os`,
`require`, `load`, or file access; script paths are confined to the project.

### Lua API

Entity handles (`self.entity`, `World.find(...)`, collision args):
`getTransform()` / `setTransform(t)` · `isAlive()` · `name()` · `destroy()` ·
`setParent(other)` / `clearParent()` · `applyImpulse(x,y,z)` ·
`setVelocity(x,y,z)` · `move(vx,vz)` / `jump(speed)` / `isGrounded()`
(character controller).

Transforms are tables: `{ position={x,y,z}, rotation={x,y,z,w}, scale={x,y,z} }`.

Globals:

| Table | Functions |
|-------|-----------|
| `World` | `find(name)`, `create(name)` |
| `Input` | `keyDown(k)`, `keyPressed(k)`, `axis(name)`, `mouseDown(b)`, `mouseDelta()` |
| `Time` | `dt()`, `elapsed()`, `frame()` |
| `Log` | `info(s)`, `warn(s)`, `error(s)` |
| `Physics` | `raycast(ox,oy,oz, dx,dy,dz, maxDist)` → `{hit, entity, point, normal, distance}` |
| `Audio` | `play(path)`, `playAt(path, x,y,z)` |
| `Assets` | `loadMesh/loadTexture(cookedPath)`, `unload*`, `loadMeshAsync/loadTextureAsync`, `queryMesh/queryTexture`, `isLoading`, `meshCount/textureCount/materialCount` |
| `Scene` | `load/unload`, `preload`, `isReady`, `entityCount`, `activeCount` |

Key names are GLFW-style: `"W"`, `"Space"`, `"LeftShift"`, `"Up"`, ...
Axes (`Input.axis`) are the host app's `InputMap` bindings
(`MoveForward`, `MoveRight`, `MoveUp` by default in engine_host).

## C API (game modules, FFI)

`#include <engine/engine_api.h>` — flat `extern "C"` functions mirroring the
table above (`engineKeyDown`, `engineEntityFind`, `engineGetTransform`,
`engineRaycast`, `enginePlaySound`, ...). Two properties matter:

1. **Host-side execution.** In a hot-reloaded game module these exported
   functions run in the host process — input/audio/physics state is the real
   one. Header-inline singletons (`Input::`, `InputSystem::get()`) duplicate
   across the dylib boundary; never use them in modules.
2. **FFI-stable.** Plain C types only — this is the surface C# (and any
   other language host) will bind against.

All functions are callable at any time; without a bound simulation world they
no-op and return zero/false.
