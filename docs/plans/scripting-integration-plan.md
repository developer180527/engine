---
status: unreviewed
---
# C# and C++ Scripting Integration Plan

**Last updated:** June 8, 2026

## Current State

The scripting system has a clean three-layer design:

```
Language wrapper  →  ScriptHost  →  Engine services
(per-language)       (THE contract)   (shared C++)
```

`ScriptHost` has ~50 flat methods using only primitives (`uint32_t`, `float`, `const char*`,
`flecs::entity`). Adding a new language means writing a thin wrapper that translates language
types to these primitives — the engine side stays untouched.

Lua is the reference implementation: `LuaBindings` (320 lines) + `LuaScriptPlugin` (300 lines).

---

## Phase A: C++ Native Scripting (Hot-Reload)

### Goal
Let gameplay programmers write scripts in C++ with the same lifecycle as Lua scripts, but with
full type safety, IDE autocompletion, and native performance. Hot-reload via shared library
swap on Play.

### Architecture

```
scripts/
  cpp/
    player_controller.cpp   ─┐
    enemy_ai.cpp             │  cmake builds these into
    game_manager.cpp         ─┘  a single shared library
                                        │
                                game_scripts.dylib / .dll / .so
                                        │
                                NativeScriptPlugin (loads via dlopen/LoadLibrary)
                                        │
                                ScriptHost (same contract as Lua)
```

### Script File Convention

```cpp
// scripts/cpp/player_controller.cpp
#include <engine/native_script.h>

class PlayerController : public NativeScript {
public:
    float speed = 5.0f;
    float jumpForce = 8.0f;

    void onStart() override {
        Log::info("Player started: " + entity().name());
    }

    void onUpdate(float dt) override {
        if (input().keyDown("W")) {
            auto tr = entity().getTransform();
            tr.position.z -= speed * dt;
            entity().setTransform(tr);
        }
        if (input().keyPressed("Space") && entity().isGrounded()) {
            entity().jump(jumpForce);
        }
    }

    void onDestroy() override {
        Log::info("Player destroyed");
    }

    void onCollisionEnter(Entity other) override {
        Log::info("Hit: " + other.name());
    }
};

REGISTER_SCRIPT(PlayerController)
```

### Key Components

#### 1. `NativeScript` base class (`src/engine/native_script.h`)

```cpp
class NativeScript {
public:
    virtual ~NativeScript() = default;
    virtual void onStart() {}
    virtual void onUpdate(float dt) {}
    virtual void onDestroy() {}
    virtual void onCollisionEnter(Entity other) {}
    virtual void onCollisionExit(Entity other) {}

protected:
    // Thin wrappers around ScriptHost — same API as Lua but type-safe
    Entity entity();                    // self.entity equivalent
    InputProxy input();                 // Input.keyDown etc.
    AssetProxy assets();                // Assets.loadMesh etc.
    SceneProxy scene();                 // Scene.load etc.
    WorldProxy world();                 // World.find etc.

private:
    friend class NativeScriptPlugin;
    ScriptHost* m_host = nullptr;
    flecs::entity m_entity;
};

// Macro: registers a factory function in the shared library's export table
#define REGISTER_SCRIPT(Class) \
    extern "C" NativeScript* create_##Class() { return new Class(); } \
    static ScriptRegistrar reg_##Class(#Class, create_##Class);
```

#### 2. `NativeScriptPlugin` (`src/plugins/native_script_plugin.h`)

```
onAttach:
    - store EngineContext refs
    - locate scripts/cpp/ directory

onSimulationStart:
    - compile scripts/cpp/ → game_scripts.dylib  (cmake subprocess)
    - dlopen() the library
    - enumerate exported factory functions
    - for each entity with NativeScriptComponent:
        find factory by class name → create instance → call onStart()

onUpdate:
    - for each instance: call onUpdate(dt)

onSimulationStop:
    - for each instance: call onDestroy()
    - dlclose() the library  (allows recompile on next Play)
```

#### 3. Hot-Reload Flow

```
Developer edits player_controller.cpp
    → presses Play
    → NativeScriptPlugin:
        1. cmake --build scripts/cpp/build/ --target game_scripts
        2. dlopen("game_scripts.dylib")
        3. enumerate factories via exported symbol table
        4. instantiate per-entity, call onStart()
    → presses Stop
    → NativeScriptPlugin:
        1. call onDestroy() on all instances
        2. delete all instances
        3. dlclose() — library unloaded, can recompile
    → edits code, presses Play again → repeat
```

#### 4. Build Integration

```cmake
# scripts/cpp/CMakeLists.txt (auto-generated or hand-maintained)
cmake_minimum_required(VERSION 3.20)
project(game_scripts)
add_library(game_scripts SHARED
    player_controller.cpp
    enemy_ai.cpp
    game_manager.cpp
)
target_link_libraries(game_scripts PRIVATE engine_scripting_api)
# engine_scripting_api = headers-only target with NativeScript + proxy classes
```

### Advantages over Lua
- Full IDE support (autocomplete, go-to-definition, refactor, breakpoints)
- Compile-time type checking
- Native performance (no FFI overhead, no GC pauses)
- Access to STL containers, custom allocators, SIMD intrinsics

### Limitations
- Compile step on each Play (~1-3 seconds for incremental build)
- ABI fragility: NativeScript vtable must stay stable across reloads
- No serialized state survival across hot-reload (Lua can preserve globals)

---

## Phase B: C# Scripting (Mono/.NET)

### Goal
Unity-style C# scripting with hot-reload. Targets developers who prefer managed languages
with garbage collection, rich standard library, and mature tooling.

### Runtime Options

| Runtime | Pros | Cons |
|---------|------|------|
| **Mono** (embedded) | Battle-tested (Unity, Godot). Easy embedding. Works on all platforms | Older .NET compat, slower JIT than CoreCLR |
| **CoreCLR** (hostfxr) | Latest .NET 8/9, best perf, full BCL | Harder to embed, larger runtime, mobile tricky |
| **.NET NativeAOT** | No JIT needed, small binary | No hot-reload, long compile |

**Recommendation**: Start with **Mono** for embedding simplicity and cross-platform support.
Migrate to CoreCLR later if perf demands it. The interop layer is the same either way.

### Architecture

```
scripts/
  csharp/
    PlayerController.cs     ─┐
    EnemyAI.cs               │  dotnet build / mcs
    GameManager.cs           ─┘  → GameScripts.dll
                                        │
                                C# Runtime (Mono embedded)
                                        │
                                CSharpScriptPlugin
                                        │
                                ┌───────┴────────┐
                                │ C interop layer │  (P/Invoke ↔ host functions)
                                │ engine_interop.c│
                                └───────┬────────┘
                                        │
                                   ScriptHost
```

### Script File Convention

```csharp
// scripts/csharp/PlayerController.cs
using Engine;

public class PlayerController : Script
{
    public float Speed = 5.0f;

    public override void OnStart()
    {
        Log.Info($"Player started: {Entity.Name}");
    }

    public override void OnUpdate(float dt)
    {
        if (Input.KeyDown("W"))
        {
            var tr = Entity.GetTransform();
            tr.Position.Z -= Speed * dt;
            Entity.SetTransform(tr);
        }
        if (Input.KeyPressed("Space") && Entity.IsGrounded)
        {
            Entity.Jump(8.0f);
        }
    }

    public override void OnCollisionEnter(Entity other)
    {
        Log.Info($"Hit: {other.Name}");
    }
}
```

### Key Components

#### 1. C Interop Layer (`src/engine/scripting/engine_interop.h`)

Exposes ScriptHost methods as `extern "C"` functions that C# calls via P/Invoke:

```c
// Flat C API — each function takes a host pointer + entity ID + args
extern "C" {
    // Entity
    void        engine_entity_destroy(uint64_t entityId);
    bool        engine_entity_isAlive(uint64_t entityId);
    const char* engine_entity_name(uint64_t entityId);

    // Transform
    bool  engine_entity_getTransform(uint64_t entityId, float* out15);
    void  engine_entity_setTransform(uint64_t entityId, const float* in15);

    // Input
    bool  engine_input_keyDown(const char* key);
    bool  engine_input_keyPressed(const char* key);
    float engine_input_axis(const char* name);

    // Assets
    uint32_t engine_assets_loadMesh(const char* path);
    bool     engine_assets_unloadMesh(uint32_t handle);
    void     engine_assets_loadMeshAsync(const char* path);
    uint32_t engine_assets_queryMesh(const char* path);
    // ... etc. — one function per ScriptHost method

    // Scene
    uint32_t engine_scene_load(const char* path);
    bool     engine_scene_unload(uint32_t handle);
    void     engine_scene_preload(const char* path);
    bool     engine_scene_isReady(const char* path);

    // Log
    void engine_log_info(const char* msg);
    void engine_log_warn(const char* msg);
    void engine_log_error(const char* msg);
}
```

#### 2. C# Engine Bindings (`scripts/csharp/Engine/`)

```csharp
// Engine/Interop.cs — raw P/Invoke declarations
internal static class Interop
{
    [DllImport("engine_interop")]
    internal static extern uint engine_assets_loadMesh(string path);

    [DllImport("engine_interop")]
    internal static extern bool engine_assets_unloadMesh(uint handle);

    // ... one per interop function
}

// Engine/Assets.cs — friendly wrapper
public static class Assets
{
    public static uint LoadMesh(string path) => Interop.engine_assets_loadMesh(path);
    public static bool UnloadMesh(uint handle) => Interop.engine_assets_unloadMesh(handle);
    public static void LoadMeshAsync(string path) => Interop.engine_assets_loadMeshAsync(path);
    public static uint QueryMesh(string path) => Interop.engine_assets_queryMesh(path);
    public static bool IsLoading(string path) => Interop.engine_assets_isLoading(path);
    public static int MeshCount => (int)Interop.engine_assets_meshCount();
    // ...
}

// Engine/Script.cs — base class
public abstract class Script
{
    public Entity Entity { get; internal set; }
    public virtual void OnStart() {}
    public virtual void OnUpdate(float dt) {}
    public virtual void OnDestroy() {}
    public virtual void OnCollisionEnter(Entity other) {}
    public virtual void OnCollisionExit(Entity other) {}
}
```

#### 3. `CSharpScriptPlugin` (`src/plugins/csharp_script_plugin.h`)

```
onAttach:
    - mono_jit_init("GameScripts")
    - register internal calls (mono_add_internal_call for each interop function)
    - store EngineContext refs

onSimulationStart:
    - compile scripts/csharp/ → GameScripts.dll  (dotnet build or mcs)
    - mono_domain_assembly_open("GameScripts.dll")
    - for each entity with CSharpScriptComponent:
        - mono_object_new(scriptClass)
        - mono_runtime_invoke("OnStart")

onUpdate:
    - for each instance: mono_runtime_invoke("OnUpdate", dt)

onSimulationStop:
    - for each instance: mono_runtime_invoke("OnDestroy")
    - mono_domain_unload → mono_domain_create → ready for next Play
```

### Hot-Reload Flow

```
Developer edits PlayerController.cs
    → presses Play
    → CSharpScriptPlugin:
        1. dotnet build scripts/csharp/ -o .cache/csharp/
        2. create new Mono AppDomain
        3. load GameScripts.dll into new domain
        4. instantiate script classes, call OnStart()
    → presses Stop
    → CSharpScriptPlugin:
        1. call OnDestroy() on all instances
        2. unload AppDomain (GC collects everything)
    → edits code, presses Play → new AppDomain with fresh DLL
```

---

## Implementation Order

### Step 1: C Interop Layer (shared by both C++ and C#)
Write `engine_interop.h/.cpp` — flat `extern "C"` functions wrapping every ScriptHost method.
This is useful even without C# because it becomes the stable ABI boundary for native scripts too.

**Files**: `src/engine/scripting/engine_interop.h`, `src/engine/scripting/engine_interop.cpp`
**Effort**: ~200 lines. Mechanical — one function per ScriptHost method.

### Step 2: C++ Native Scripting
1. `NativeScript` base class + proxy wrappers
2. `REGISTER_SCRIPT` macro + factory pattern
3. `NativeScriptPlugin` — dlopen/dlclose lifecycle
4. CMake integration for scripts/cpp/
5. `NativeScriptComponent` (stores class name string)

**Files**: `src/engine/native_script.h`, `src/plugins/native_script_plugin.h`
**Effort**: ~400 lines total. Moderate — dlopen/dlclose platform abstraction is the tricky part.

### Step 3: C# Scripting
1. Embed Mono runtime (`mono_jit_init`, domain management)
2. Wire interop functions (`mono_add_internal_call`)
3. C# Engine bindings DLL (`Engine/` namespace)
4. `CSharpScriptPlugin` — compile, load, lifecycle
5. `CSharpScriptComponent` (stores class name string)

**Files**: `src/plugins/csharp_script_plugin.h`, `scripts/csharp/Engine/*.cs`
**Effort**: ~800 lines C++ + ~500 lines C#. Hardest part is Mono embedding + AppDomain lifecycle.

### Step 4: Editor Integration
1. Script component inspector shows language dropdown (Lua / C++ / C#)
2. One-click "Create Script" for each language
3. Console output tagged by language `[Lua]`, `[C++]`, `[C#]`
4. Script compilation errors shown in editor console

---

## Cross-Language Interop Matrix

Every ScriptHost method gets exposed in every language. The matrix:

| ScriptHost method | Lua | C++ Native | C# |
|-------------------|-----|------------|-----|
| Entity: name, isAlive, destroy | `entity:name()` | `entity().name()` | `Entity.Name` |
| Transform: get/set | `entity:getTransform()` | `entity().getTransform()` | `Entity.GetTransform()` |
| Input: keyDown, keyPressed, axis | `Input.keyDown("W")` | `input().keyDown("W")` | `Input.KeyDown("W")` |
| Assets: load/unload/async | `Assets.loadMesh(p)` | `assets().loadMesh(p)` | `Assets.LoadMesh(p)` |
| Scene: load/unload/preload | `Scene.load(p)` | `scene().load(p)` | `Scene.Load(p)` |
| Physics: raycast, impulse | `Physics.raycast(o,d)` | `physics().raycast(o,d)` | `Physics.Raycast(o,d)` |
| Audio: play, playAt | `Audio.play(p)` | `audio().play(p)` | `Audio.Play(p)` |
| Log: info, warn, error | `Log.info(m)` | `Log::info(m)` | `Log.Info(m)` |
| Time: dt, elapsed, frame | `Time.dt()` | `time().dt()` | `Time.Dt` |
| World: find, create | `World.find(n)` | `world().find(n)` | `World.Find(n)` |

---

## Platform Considerations

| Platform | Lua | C++ Native | C# (Mono) |
|----------|-----|------------|-----------|
| macOS | yes | dlopen + .dylib | yes |
| Windows | yes | LoadLibrary + .dll | yes |
| Linux | yes | dlopen + .so | yes |
| Web/WASM | yes (compiled in) | no (no dlopen) | Blazor/IL2CPP (future) |
| iOS | yes (compiled in) | no (no dlopen) | Mono AOT |
| Android | yes (compiled in) | dlopen + .so | Mono AOT |

**Web/WASM note**: C++ native scripts can be statically compiled into the WASM binary
instead of using dlopen. The factory registration pattern still works.

---

## Dependencies

| Component | New dependency | Size |
|-----------|---------------|------|
| C++ Native | None (dlopen is POSIX, LoadLibrary is Win32) | 0 |
| C# (Mono) | libmono-2.0 (or mono-sgen) | ~15 MB runtime |
| C# (CoreCLR) | dotnet runtime | ~30 MB runtime |

---

## Testing Strategy

Extend the existing autorun test pattern:

```
scripts/
  autorun/
    test_assets.lua       -- existing (16 tests)
    test_scenes.lua       -- existing (11 tests)
  cpp/
    test_assets.cpp       -- same tests in C++
    test_scenes.cpp
  csharp/
    TestAssets.cs          -- same tests in C#
    TestScenes.cs
```

All three should produce identical results. The test scripts validate that every ScriptHost
method works correctly regardless of which language calls it.
