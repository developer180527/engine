---
status: unreviewed
---
# Asset Management System

**Last updated:** June 8, 2026 — Phase 1 + Phase 2 + Phase 3 complete

## Overview

The asset management system handles loading, unloading, and lifetime management of GPU resources (meshes, textures, materials) at runtime. It is **engine infrastructure**, not a plugin — it's available before any `IEnginePlugin` attaches and persists across play sessions.

The system has two distinct operational modes:

- **Editor mode**: assets are imported from source formats (FBX, glTF, PNG, etc.) via Assimp/cgltf, cooked into optimized binary formats with UUIDs, and stored in a SQLite-backed registry. This is heavy, offline work.
- **Runtime mode**: only cooked binary assets are loaded. File I/O is minimal (read header + bulk data), parsing is trivial (memcpy into bgfx memory), and GPU handle creation is instant. This is what ships with the game.

The `AssetService` is the boundary between these two worlds — it only speaks cooked binaries and typed handles.

## Architecture

```
                        EDITOR SIDE                           RUNTIME SIDE
                   (development only)                    (ships with the game)

  Source files (.fbx, .gltf, .png)
         |
    Assimp / cgltf / stb_image
         |
    CookPipeline (ICooker)
         |                                          Lua / C# / C++ gameplay code
    .cooked binaries                                         |
    (.cache/ directory)                                 ScriptHost
         |                                          (flat C++ methods,
    SQLite AssetRegistry                             uint32_t handles)
    (UUID, hash, state,                                      |
     dependency graph)                                  AssetService
         |                                          (sync + async API)
         +--------------------------------------------------+
                                |
                    AssetRegistry (meshes)
                    TextureRegistry (textures)
                    MaterialRegistry (materials)
                    [free-list slot reuse]
                                |
                           bgfx GPU handles
                    (VB, IB, Texture2D)
```

## Editor / Runtime Separation

### Editor-only systems (NOT in AssetService)

| System | What it does | Where it lives |
|--------|-------------|----------------|
| **assetlib::AssetRegistry** | SQLite DB tracking every source asset — UUID, hash, cook state, dependencies | `modules/assetlib/` |
| **CookPipeline** | Converts source assets to `.cooked` binaries (mesh cooker, texture cooker) | `modules/assetlib/`, `src/cookers/` |
| **CookService** | Background thread that watches for stale assets and re-cooks them | `src/io/cook_service.h` |
| **AsyncLoader** (legacy) | Assimp-based loader with texture discovery heuristics, used by the editor's drag-and-drop import | `src/engine/async_loader.h` |
| **ImporterRegistry** | Registered importers (GltfImporter, AssimpImporter) for source format parsing | `src/io/importer_registry.h` |

### Runtime systems (in AssetService)

| System | What it does | Where it lives |
|--------|-------------|----------------|
| **AssetService** | Flat API for loading/unloading cooked assets, sync + async | `src/engine/asset_service.h` |
| **AssetRegistry** | Runtime mesh storage, handle-based access, free-list slot reuse | `src/render/asset_registry.h` |
| **TextureRegistry** | Runtime texture storage, same pattern | `src/render/texture_registry.h` |
| **MaterialRegistry** | Runtime material storage, with occupancy tracking | `src/render/material_registry.h` |
| **Handle system** | Typed `Handle<Tag>` with `uint32_t id`, index 0 = invalid | `src/core/handle.h` |

### Why this separation matters

When a game ships, none of the editor systems exist. No SQLite, no Assimp, no cookers. The runtime only needs:
1. A directory of `.cooked` files (or a packed archive, later)
2. `AssetService` to load them
3. The three registries to store GPU handles

This keeps the shipped binary small and the loading path fast.

## Cooked Asset Formats

### Mesh (magic: `0x4D455348` / "MESH")

```
[MeshHeader]         80 bytes — magic, version, UUID, vertex flags/stride/count,
                               index count/stride, submesh count, AABB bounds,
                               material count
[Vertex data]        vertexCount * vertexStride bytes
[Index data]         indexCount * indexStride bytes
[MeshSubmesh[]]      submeshCount * 32 bytes each — index offset/count, material index
[CookedMaterial[]]   materialCount * 1052 bytes each — PBR params + texture paths
```

Vertex layout: position(float3) + normal(float3) + tangent(float4) + uv0(float2) = 48 bytes.

### Texture (magic: `0x54455820` / "TEX ")

```
[TextureHeader]      32 bytes — magic, version, width, height, channels (always RGBA8)
[Pixel data]         width * height * 4 bytes (raw RGBA8)
```

## Phase 1 Implementation Details

### Step 1: Registry Slot Reuse

**Problem**: The three runtime registries (mesh, texture, material) were append-only vectors. Loading and unloading assets over time would grow the vectors unboundedly — indices were never reclaimed.

**Solution**: Free-list based slot reuse.

```
addMesh(mesh):
    if freeSlots not empty:
        slot = freeSlots.pop_back()
        meshes[slot] = move(mesh)
        return MeshHandle{slot}
    else:
        meshes.push_back(move(mesh))
        return MeshHandle{meshes.size() - 1}

removeMesh(handle):
    meshes[handle.id] = Mesh{}      // RAII destroys bgfx buffers
    freeSlots.push_back(handle.id)
```

- **Mesh/Texture**: removal assigns a default object, which triggers RAII destruction of bgfx handles. The existing `valid()` method (checks `bgfx::isValid(handle)`) naturally returns false for freed slots.
- **Material**: has no GPU resources and no `valid()` method, so `MaterialRegistry` uses a parallel `std::vector<bool> m_occupied` to track which slots are live.
- **meshCount()** now returns `size - 1 - freeSlots.size()` (excludes reserved slot 0 and freed slots).

**Files**: `asset_registry.h`, `texture_registry.h`, `material_registry.h`

### Step 2: AssetService (Sync API)

**Problem**: No centralized, scriptable API for loading cooked assets. The existing `AsyncLoader` was tightly coupled to the editor (Assimp fallback, texture discovery heuristics, entity spawning).

**Solution**: `AssetService` — a flat, FFI-friendly class that only handles cooked binaries.

```cpp
class AssetService {
    MeshHandle    loadMesh(const char* cookedPath);
    TextureHandle loadTexture(const char* cookedPath);
    bool          unloadMesh(MeshHandle h);
    bool          unloadTexture(TextureHandle h);
    bool          unloadMaterial(MaterialHandle h);
};
```

**loadMesh flow**:
1. Resolve path (relative paths go under `.cache/`)
2. Read cooked binary via `assetlib::loadMesh()`
3. Validate vertex stride matches runtime `Vertex` layout
4. `bgfx::copy()` vertex + index data into bgfx's allocator
5. Create `bgfx::VertexBufferHandle` + `bgfx::IndexBufferHandle`
6. For each embedded `CookedMaterial`: resolve texture paths via assetlib DB, load cooked textures, create bgfx texture handles, register in `TextureRegistry` and `MaterialRegistry`
7. Wire materials to mesh submeshes
8. Register `Mesh` in `AssetRegistry`, return `MeshHandle`

**Ownership**: `EngineRuntime` owns `AssetService` via `unique_ptr`. It's created in `initSystems()` with references to the three registries. The assetlib DB pointer and project root are wired later from `main.cpp` via `setAssetLib()` / `setProjectRoot()`.

**Files**: `asset_service.h`, `asset_service.cpp`, `runtime.h`, `runtime.cpp`, `runtime_context.h`, `engine_context.h`, `main.cpp`

### Step 3: Lua Bindings

**Problem**: Game developers need to control asset loading from scripts.

**Solution**: Three-layer binding following the existing engine pattern:

```
Lua script         →  LuaBindings (C functions)  →  ScriptHost (flat methods)  →  AssetService
Assets.loadMesh()     assets_loadMesh()              assetLoadMesh()               loadMesh()
```

- **ScriptHost** methods use `uint32_t` (not `MeshHandle`) for FFI compatibility — handles are just integers at the scripting boundary.
- **LuaBindings** registers an `Assets` global table with all methods.
- **LuaScriptPlugin** wires the `AssetService*` from `EngineContext` in `onAttach()`.

Lua API:
```lua
-- Sync (blocks until loaded)
local mesh = Assets.loadMesh("meshes/character.cooked")
local tex  = Assets.loadTexture("textures/skin.cooked")

-- Unload
Assets.unloadMesh(mesh)
Assets.unloadTexture(tex)
Assets.unloadMaterial(mat)

-- Query
Assets.meshCount()
Assets.textureCount()
Assets.materialCount()
```

The `AssetService` pointer is NOT cleared on simulation stop — it's engine infrastructure. Scripts can preload assets that persist across play sessions.

**Files**: `script_host.h`, `lua_bindings.h`, `lua_script_plugin.h`

### Step 4: Async Loading

**Problem**: Sync `loadMesh()` does file I/O on the main thread, which stalls the frame for large assets.

**Solution**: Worker thread handles all file I/O, main thread only creates GPU handles.

```
Main thread                          Worker thread
-----------                          -------------
loadMeshAsync("path")
  → push to pending queue    ───→    wake up
  → return immediately               read cooked binary
                                      bgfx::copy() vertex/index/texture data
                                      push ReadyAsset to ready queue
                                      sleep

drainUploads() (once per frame)
  ← pop from ready queue
  create VB, IB, textures (microseconds)
  register in registries
  store in loaded cache
```

**Thread safety**:
- Worker reads: `assetlib::loadMesh/loadTexture` (file I/O), `bgfx::copy()` (documented thread-safe), `assetLib->findBySourcePath()` (SQLite WAL reader)
- Main thread: `bgfx::createVertexBuffer/createIndexBuffer/createTexture2D`, registry mutations
- Three mutexes: `pendingMtx` (request queue + in-flight set), `readyMtx` (result queue), `loadedMtx` (loaded cache)

**Deduplication**: in-flight set prevents duplicate loads. Loaded cache returns immediately for already-loaded assets.

**Pimpl pattern**: `AsyncState` struct is defined only in the `.cpp` — threading headers (`<thread>`, `<mutex>`, `<condition_variable>`, etc.) stay out of the public header.

**Lazy worker**: the worker thread is only started on the first `loadMeshAsync()` / `loadTextureAsync()` call. If no async loads are ever made, no thread is created.

Lua async API:
```lua
function M:onStart()
    Assets.loadMeshAsync("meshes/big_level.cooked")
end

function M:onUpdate(dt)
    local h = Assets.queryMesh("meshes/big_level.cooked")
    if h > 0 and not self.ready then
        self.ready = true
        Log.info("Level mesh loaded: " .. h)
    end
end
```

**Files**: `asset_service.h`, `asset_service.cpp`, `script_host.h`, `lua_bindings.h`, `editor_app.h` (drainUploads call in main loop)

## Cross-Platform Rendering (Bonus)

Alongside the asset system, four cross-platform quick wins were implemented:

| File | Change |
|------|--------|
| `runtime.cpp` | `getNativeWindowHandle()` — dispatches to `glfwGetCocoaWindow` / `glfwGetWin32Window` / `glfwGetX11Window` / `glfwGetWaylandWindow` based on platform |
| `renderer.cpp` | Platform-conditional `bgfx::RendererType` — Metal (macOS), Direct3D12 (Windows), Vulkan (Linux) |
| `forward_pipeline.h` | Platform-conditional shader includes with `#define` aliases (`VS_TRIANGLE_DATA`, etc.) |
| `CMakeLists.txt` | Windows link libs (gdi32, shell32, user32, winmm) + Linux link libs (X11, dl, Threads) |

## Phase 2: Binary Scene Format + SceneService

### Binary Scene Format (magic: `0x53434E45` / "SCNE")

The editor saves scenes as JSON (human-readable, git-diff-friendly). The cook pipeline bakes
JSON into a binary format for runtime use. The binary format is designed for zero parsing overhead.

```
[SceneHeader]                32 bytes   magic, version, entityCount, stringTableSize
[SceneEntity × N]           256 bytes each — ALL component data inlined, fixed-size
[String table]              packed null-terminated strings
```

**10K entities = 2.5 MB.** No parsing, no allocations, no key lookups — `memcpy` the entity
block, read variable-length strings by offset.

#### SceneHeader (32 bytes)
| Field | Type | Description |
|-------|------|-------------|
| magic | uint32 | `0x53434E45` ("SCNE") |
| version | uint32 | Currently `1` |
| entityCount | uint32 | Number of entity records |
| stringTableSize | uint32 | Byte size of the string table |
| _pad | uint8[16] | Reserved |

#### SceneEntity (256 bytes, fixed)
Each entity record inlines ALL possible component data. A `componentMask` bitfield says which
are meaningful; unused fields are zeroed.

| Bit | Component | Data inlined |
|-----|-----------|--------------|
| 0 | Transform | position(float3), rotation(quat4), scale(float3) |
| 1 | Name | offset + length into string table |
| 2 | MeshRenderer | cookedPath, sourcePath, sourceType, matOverrideId |
| 3 | Camera | fov, projection, near/far, orthoSize, clearColor |
| 4 | RigidBody | bodyType, shape, mass, restitution, friction, halfExtent, radius, halfHeight |
| 5 | Script | scriptPath offset + length into string table |
| 6 | CharacterController | radius, height, maxSlope, stepHeight, mass, gravityScale |
| 7 | Light | type, color, intensity, range, spotAngles, castShadows, temperature |

Variable-length data (entity names, mesh paths, script paths) are stored as `(offset, length)`
pairs pointing into the string table. Sentinel offset `0xFFFFFFFF` = not present.

**File**: `modules/assetlib/include/assetlib/scene_asset.h`, `modules/assetlib/src/scene_asset.cpp`

### SceneService

`SceneService` sits on top of `AssetService`. One call loads an entire scene — entities,
transforms, parent links, meshes, cameras, lights, scripts, physics, everything.

```
SceneService
    ├── reads binary .scene file (memcpy entity block + string table)
    ├── for each MeshRenderer entity: AssetService::loadMesh(cookedPath)
    │   └── fallback: PrimitiveLibrary::byName() for built-in shapes
    ├── creates flecs entities, sets all components from binary data
    ├── restores ChildOf parent links in a second pass
    └── tracks spawned entities + loaded assets for unloadScene()
```

**Ownership**: Like AssetService, SceneService is **engine infrastructure, not a plugin**.
Owned by `EngineRuntime` via `unique_ptr`, created in `initSystems()`.

**API** (all FFI-friendly, `uint32_t` handles):

| Method | Signature | Description |
|--------|-----------|-------------|
| `loadScene` | `uint32_t loadScene(const char* cookedPath)` | Parse binary, load all assets, spawn entities. Returns scene handle or 0 |
| `unloadScene` | `bool unloadScene(uint32_t handle)` | Destroy entities + unload assets. Returns false if handle unknown |
| `preloadScene` | `void preloadScene(const char* cookedPath)` | Async pre-load all referenced assets (does NOT spawn entities) |
| `isSceneReady` | `bool isSceneReady(const char* cookedPath)` | Poll: are all preloaded assets ready? |
| `sceneEntityCount` | `uint32_t sceneEntityCount(uint32_t handle)` | How many entities this scene spawned |
| `activeSceneCount` | `uint32_t activeSceneCount()` | How many scenes are currently loaded |

**Lua surface**:
```lua
local scene = Scene.load("scenes/level1.cooked")
Scene.entityCount(scene)    -- e.g. 47
Scene.activeCount()         -- e.g. 1
Scene.unload(scene)

-- Async preload pattern
Scene.preload("scenes/level2.cooked")
-- later...
if Scene.isReady("scenes/level2.cooked") then
    local h = Scene.load("scenes/level2.cooked")  -- instant, assets cached
end
```

**Files**: `scene_service.h`, `scene_service.cpp`, `scene_asset.h`, `scene_asset.cpp`

### Scene Cook Pipeline

The editor's `SceneSerializer::cookScene()` converts JSON scenes to binary:

```cpp
SceneSerializer::cookScene("scenes/main.scene",         // input: editor JSON
                           ".cache/scenes/main.cooked",  // output: binary
                           &assetLib,                     // source→cooked path resolution
                           projectRoot);
```

During cooking, source paths (`assets/models/character.fbx`) are resolved to cooked paths
(`meshs/abc123.cooked`) via the assetlib SQLite DB. The binary file embeds cooked paths so
the runtime never needs to touch the DB.

### Autorun Scripts

Any `.lua` file in `scripts/autorun/` runs automatically when Play is pressed.

- The `LuaScriptPlugin` scans the directory on `onSimulationStart()`
- For each `.lua` file, creates a headless entity with `Name`, `Transform`, and `ScriptComponent`
- Full lifecycle: `onStart()` → `onUpdate(dt)` → `onDestroy()`
- On Stop: autorun entities are destroyed, modules cleared, next Play reloads from disk
- Use cases: test suites, global systems, one-shot setup code

**Files**: `lua_script_plugin.h` (collectAutorunScripts method)

## Phase 3: Editor Integration (COMPLETE)

### Scene Auto-Cooking

Scene files (JSON `.scene` in `scenes/`) are now automatically cooked into binary
(`.cooked` in `.cache/scenes/`) at two trigger points:

1. **CookService background pass** — after cooking meshes and textures, `cookSceneFiles()`
   scans `scenes/` for stale `.scene` files and cooks them. A scene is stale if:
   - Its binary doesn't exist yet
   - Its JSON is newer than the binary
   - Any mesh/texture was cooked in the same pass (cooked paths may have changed)

2. **Editor save (Cmd+S)** — `saveScene()` calls `cookScene()` immediately after writing
   the JSON, so the binary is always in sync.

### Scene Cooker Architecture

```
SceneSerializer::cookScene()     EditorApp::saveScene()
          │                              │
          └──────────┬───────────────────┘
                     │
              cookSceneFile()         ← standalone function in cookers/scene_cooker.cpp
              (JSON parse + assetlib   ← minimal deps: no flecs, no entity_serializer
               path resolution +       ← thread-safe: CookService calls from bg thread,
               binary write)             editor calls from main thread
```

The scene cooker resolves source paths to cooked paths via the assetlib DB, so the binary
scene file embeds cooked paths directly. At runtime, `SceneService::loadScene()` reads
these paths and calls `AssetService::loadMesh()` — no DB lookup, no Assimp, instant.

### Unified Loading Path

The entity serializer's `loadMesh()` already has a two-tier resolution:

1. **Fast path**: if JSON has `cookedPath` and `AssetService*` is non-null, load the cooked
   binary directly — microseconds, no Assimp
2. **Legacy fallback**: source path → primitive / glTF (sync) / Assimp (async worker)

This means the editor loading path (`SceneSerializer::loadAsync()`) automatically uses
cooked assets when available, falling through to Assimp only for uncooked or in-progress
imports.

**Files**: `cookers/scene_cooker.h`, `cookers/scene_cooker.cpp`, `cook_service.h/cpp`,
`editor_app.h`, `scene_serializer.h`

## Roadmap (Remaining Phases)

### Load Groups (next)
- `createGroup()` / `unloadGroup()` — batch-manage asset lifetimes
- Load a level's worth of assets as one unit, unload them all at once
- Tracks which meshes/textures/materials belong to each group

### Future
- Handle generation counters (prevent stale handle use-after-free)
- Asset packing (bundle cooked files into a single archive for shipping)
- WASM/mobile cooking (platform-specific texture compression, shader variants)
- C# scripting bindings (same ScriptHost methods, thin P/Invoke wrapper)
- C++ native scripting (hot-reloadable shared libraries)
