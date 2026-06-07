# Asset Management System

**Last updated:** June 7, 2026 — Phase 1 complete

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

## Roadmap (Remaining Phases)

### Phase 1 Step 5: Load Groups (next)
- `createGroup()` / `unloadGroup()` — batch-manage asset lifetimes
- Load a level's worth of assets as one unit, unload them all at once
- Tracks which meshes/textures/materials belong to each group

### Phase 2: Scene API
- `Scene.load()` / `Scene.unload()` / `Scene.preload()` — higher-level API built on AssetService
- `World.spawn(prefabPath)` — instantiate a prefab as an entity hierarchy
- Scene files reference assets by UUID, not path

### Phase 3: Editor Integration
- Refactor `AsyncLoader` to delegate to `AssetService` for cooked files
- Keep Assimp fallback for uncoooked/in-progress imports
- Unified loading path: editor and runtime use the same service

### Future
- Handle generation counters (prevent stale handle use-after-free)
- Asset packing (bundle cooked files into a single archive for shipping)
- WASM/mobile cooking (platform-specific texture compression, shader variants)
- C# scripting bindings (same ScriptHost methods, thin P/Invoke wrapper)
