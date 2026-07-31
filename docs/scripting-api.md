---
status: unreviewed
---
# Scripting API Reference

**Last updated:** June 8, 2026

## Architecture

The scripting system uses a three-layer design that makes adding new languages straightforward:

```
  Game scripts                (Lua 5.4, C#, C++ — per-language syntax)
       |
  Language wrapper            (LuaBindings, CSharpBindings, NativeBindings)
       |                      (thin: translates language types to C++ primitives)
       |
  ScriptHost                  (THE contract — flat C++ methods, no templates,
       |                       only primitives/const char*/uint32_t for FFI)
       |
  Engine services             (AssetService, SceneService, flecs::world,
                               IPhysicsService, IAudioService)
```

**ScriptHost** is the single source of truth. Every language backend exposes the exact same
methods. If you add a method to ScriptHost, it becomes available in ALL languages once each
wrapper adds a one-line forwarding call.

### Why flat signatures?

Every ScriptHost method uses only:
- `const char*` for strings
- `uint32_t` / `float` / `double` / `bool` for values
- `flecs::entity` for entity references (exposed as opaque handles to scripts)

This makes FFI trivial: Lua's `lua_pushinteger`, C#'s `[DllImport]` P/Invoke, and C's
function pointers all work without marshaling structs across language boundaries.

## Script Lifecycle

Every script file returns a table of methods (a "behavior"):

```lua
local M = {}
function M:onStart()    end   -- called once when entity enters simulation
function M:onUpdate(dt) end   -- called every frame (dt in seconds)
function M:onDestroy()  end   -- called when entity is removed or simulation stops
function M:onCollisionEnter(other) end  -- physics contact began
function M:onCollisionExit(other)  end  -- physics contact ended
return M
```

Each entity with a `ScriptComponent` gets its own **instance table** (state lives on `self`).
`self.entity` is the entity handle. Modules are cached by path and cleared on Stop, so editing
a script and pressing Play reloads it.

## API Reference

### Entity (self.entity)

Methods called on the entity handle attached to each script instance.

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `name` | `entity:name()` | `string` | Entity's display name (from Name component) |
| `isAlive` | `entity:isAlive()` | `bool` | Is this entity still alive in the ECS? |
| `destroy` | `entity:destroy()` | `void` | Destroy this entity (deferred until end of frame) |
| `getTransform` | `entity:getTransform()` | `table\|nil` | Get position/rotation/scale (see below) |
| `setTransform` | `entity:setTransform(t)` | `void` | Set position/rotation/scale |
| `setParent` | `entity:setParent(other)` | `void` | Make this entity a child of `other` |
| `clearParent` | `entity:clearParent()` | `void` | Remove parent (become root) |
| `applyImpulse` | `entity:applyImpulse(x,y,z)` | `void` | Apply physics impulse (requires physics service) |
| `setVelocity` | `entity:setVelocity(x,y,z)` | `void` | Set linear velocity (requires physics service) |
| `move` | `entity:move(vx,vz)` | `void` | Character controller: move on XZ plane |
| `jump` | `entity:jump([speed])` | `void` | Character controller: jump (default speed=5) |
| `isGrounded` | `entity:isGrounded()` | `bool` | Character controller: touching ground? |

#### Transform table format

```lua
{
    position = { x = 0, y = 0, z = 0 },
    rotation = { x = 0, y = 0, z = 0, w = 1 },  -- quaternion
    scale    = { x = 1, y = 1, z = 1 }
}
```

Example:
```lua
function M:onUpdate(dt)
    local tr = self.entity:getTransform()
    tr.position.y = tr.position.y + dt * 2  -- move up
    self.entity:setTransform(tr)
end
```

---

### Input

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `keyDown` | `Input.keyDown(key)` | `bool` | Is key held down this frame? |
| `keyPressed` | `Input.keyPressed(key)` | `bool` | Was key just pressed this frame? (edge trigger) |
| `axis` | `Input.axis(name)` | `float` | Named axis value (-1 to 1) |
| `mouseDown` | `Input.mouseDown(button)` | `bool` | Is mouse button held? (0=left, 1=right, 2=middle) |
| `mouseDelta` | `Input.mouseDelta()` | `dx, dy` | Mouse movement since last frame (two return values) |

**Key names**: Single letters (`"W"`, `"A"`, `"S"`, `"D"`), digits (`"0"`-`"9"`), and named keys:
`Space`, `Escape`, `Enter`/`Return`, `Tab`, `Backspace`, `Delete`, `Right`, `Left`, `Down`, `Up`,
`LeftShift`, `LeftControl`/`LeftCtrl`, `LeftAlt`, `RightShift`, `RightControl`, `RightAlt`.

Case-insensitive: `"w"` and `"W"` both work.

```lua
function M:onUpdate(dt)
    if Input.keyDown("W") then
        -- move forward
    end
    if Input.keyPressed("Space") then
        self.entity:jump()
    end
    local dx, dy = Input.mouseDelta()
end
```

---

### Log

| Method | Signature | Description |
|--------|-----------|-------------|
| `info` | `Log.info(msg)` | Info message (white in console) |
| `warn` | `Log.warn(msg)` | Warning (yellow in console) |
| `error` | `Log.error(msg)` | Error (red in console) |

```lua
Log.info("Player spawned at " .. tr.position.x .. ", " .. tr.position.y)
```

---

### Time

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `dt` | `Time.dt()` | `float` | Delta time in seconds since last frame |
| `elapsed` | `Time.elapsed()` | `double` | Total elapsed time since simulation start |
| `frame` | `Time.frame()` | `int` | Frame counter since simulation start |

---

### World

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `find` | `World.find(name)` | `entity\|nil` | Find entity by name (searches Name component first, then flecs builtin) |
| `create` | `World.create(name)` | `entity` | Create a new entity with Name + Transform |

```lua
function M:onStart()
    local player = World.find("Player")
    if player and player:isAlive() then
        Log.info("Found player: " .. player:name())
    end
    
    local marker = World.create("Waypoint")
    local tr = marker:getTransform()
    tr.position = { x = 10, y = 0, z = 5 }
    marker:setTransform(tr)
end
```

---

### Physics

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `raycast` | `Physics.raycast(origin, dir, [maxDist])` | `table` | Cast a ray, get hit info |

**Raycast result table**:
```lua
{
    hit = true,           -- bool: did we hit anything?
    distance = 5.3,       -- float: distance along ray
    point = {x,y,z},      -- world-space hit point
    normal = {x,y,z},     -- surface normal at hit
    entity = <entity>      -- entity that was hit (or nil)
}
```

```lua
local origin = { x = 0, y = 10, z = 0 }
local dir    = { x = 0, y = -1, z = 0 }
local result = Physics.raycast(origin, dir, 100)
if result.hit then
    Log.info("Hit " .. result.entity:name() .. " at distance " .. result.distance)
end
```

All physics methods are **safe no-ops** if no physics service is registered. They return
false/zero/empty until a physics backend (Jolt) is attached.

---

### Audio

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `play` | `Audio.play(path)` | `int` | Play a sound, returns handle (0 on failure) |
| `playAt` | `Audio.playAt(path,x,y,z)` | `int` | Play a 3D positioned sound |

All audio methods are **safe no-ops** until an audio service (miniaudio) is attached.

---

### Assets

Runtime asset loading via cooked binary files. Handles are integers; 0 = invalid.

#### Sync Loading

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `loadMesh` | `Assets.loadMesh(path)` | `int` | Load cooked mesh. Returns handle or 0 |
| `loadTexture` | `Assets.loadTexture(path)` | `int` | Load cooked texture. Returns handle or 0 |

Sync loads do file I/O + GPU handle creation on the calling thread. Use for critical/small
assets or preload phases.

#### Async Loading

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `loadMeshAsync` | `Assets.loadMeshAsync(path)` | `void` | Queue for background loading |
| `loadTextureAsync` | `Assets.loadTextureAsync(path)` | `void` | Queue for background loading |
| `queryMesh` | `Assets.queryMesh(path)` | `int` | Poll: handle if ready, 0 if still loading |
| `queryTexture` | `Assets.queryTexture(path)` | `int` | Poll: handle if ready, 0 if still loading |
| `isLoading` | `Assets.isLoading(path)` | `bool` | Is this asset still in-flight? |

```lua
function M:onStart()
    Assets.loadMeshAsync("meshs/big_level.cooked")
end

function M:onUpdate(dt)
    local h = Assets.queryMesh("meshs/big_level.cooked")
    if h > 0 then
        Log.info("Level loaded! handle=" .. h)
    end
end
```

#### Unloading

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `unloadMesh` | `Assets.unloadMesh(handle)` | `bool` | Unload mesh + release GPU buffers |
| `unloadTexture` | `Assets.unloadTexture(handle)` | `bool` | Unload texture + release GPU texture |
| `unloadMaterial` | `Assets.unloadMaterial(handle)` | `bool` | Unload material (no GPU resources) |

Returns `false` if the handle was invalid or already freed.

#### Counts

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `meshCount` | `Assets.meshCount()` | `int` | Number of live meshes (excludes freed slots) |
| `textureCount` | `Assets.textureCount()` | `int` | Number of live textures |
| `materialCount` | `Assets.materialCount()` | `int` | Number of live materials |

**Path resolution**: All paths are relative to `.cache/`. `Assets.loadMesh("meshs/abc123.cooked")`
resolves to `<projectRoot>/.cache/meshs/abc123.cooked`.

---

### Scene

Binary scene loading built on top of Assets. One call loads an entire entity hierarchy.

| Method | Signature | Returns | Description |
|--------|-----------|---------|-------------|
| `load` | `Scene.load(path)` | `int` | Load binary scene, spawn entities. Returns scene handle or 0 |
| `unload` | `Scene.unload(handle)` | `bool` | Destroy all entities + unload their assets |
| `preload` | `Scene.preload(path)` | `void` | Async pre-load all referenced assets (no entities spawned) |
| `isReady` | `Scene.isReady(path)` | `bool` | Are all preloaded assets ready? |
| `entityCount` | `Scene.entityCount(handle)` | `int` | How many entities this scene spawned |
| `activeCount` | `Scene.activeCount()` | `int` | How many scenes are currently loaded |

```lua
function M:onStart()
    -- Preload next level while playing current
    Scene.preload("scenes/level2.cooked")
end

function M:onUpdate(dt)
    if Scene.isReady("scenes/level2.cooked") then
        -- Unload current
        Scene.unload(self.currentScene)
        -- Load next (instant — assets already cached)
        self.currentScene = Scene.load("scenes/level2.cooked")
        Log.info("Level 2 loaded: " .. Scene.entityCount(self.currentScene) .. " entities")
    end
end
```

---

## Autorun Scripts

Any `.lua` file in `scripts/autorun/` runs automatically when Play is pressed. No entity
attachment needed.

- Headless entities are created automatically with `[autorun] <filename>` as the name
- Full lifecycle: `onStart()`, `onUpdate(dt)`, `onDestroy()`
- Destroyed on Stop, reloaded on next Play
- Use cases: test suites, global managers, debug overlays, gameplay controllers

```
scripts/
  autorun/
    test_assets.lua     -- runs automatically
    test_scenes.lua     -- runs automatically
    game_manager.lua    -- runs automatically
  spin.lua              -- requires manual entity attachment
  player.lua            -- requires manual entity attachment
```

---

## Safety & Sandboxing

- **Sandbox**: Only `base`, `table`, `string`, `math`, `coroutine` libraries are loaded.
  `io`, `os`, `package`, `debug` are **never** opened.
- **Stripped globals**: `dofile`, `loadfile`, `load`, `require`, `collectgarbage` are removed.
- **Path validation**: Absolute paths and `..` traversal are rejected. Scripts cannot escape
  the project root.
- **Epoch stamps**: Entity references carry a session epoch. Stale refs from a prior Play
  session raise a Lua error instead of aliasing a different entity.
- **Error isolation**: A script that errors in `onUpdate` is flagged `errored` and never
  re-dispatched. Other scripts continue running.
- **Deferred structural changes**: All entity create/destroy/reparent calls happen inside
  `defer_begin`/`defer_end` blocks, preventing iterator invalidation.

---

## File Map

| File | What it contains |
|------|------------------|
| `src/engine/scripting/script_host.h` | **THE contract** — all engine methods scripts can call |
| `src/engine/scripting/script_services.h` | `IPhysicsService`, `IAudioService` interfaces |
| `src/engine/scripting/lua_bindings.h` | Lua 5.4 wrapper (C functions → ScriptHost) |
| `src/plugins/lua_script_plugin.h` | Plugin: VM lifecycle, per-entity instances, autorun |
| `src/engine/asset_service.h` | Asset loading API (sync + async) |
| `src/engine/scene_service.h` | Scene loading API (built on AssetService) |
| `src/core/handle.h` | Typed handle system (`Handle<Tag>`) |
