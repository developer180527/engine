# Asset Cooking Pipeline — Technical Debt & Issues Tracker

This file tracks architectural, mathematical, and algorithmic issues discovered during the code review of the background asset cook pipeline. 

---

## 1. Concurrency & Resource Management

### [CRITICAL] Assimp Parallel Memory Explosion (OOM Risk)
* **File / Location:** `cook_service.cpp` (Worker Thread Fan-out) & `mesh_cooker.cpp`
* **Mechanism:** The pipeline schedules multiple concurrent cooks on available worker threads. Each thread processing a 3D asset instantiates an `Assimp::Importer`. Assimp builds heavy internal node hierarchies, splits vertex channels, and caches massive datasets during preprocessing (`aiProcess_JoinIdenticalVertices`, `aiProcess_CalcTangentSpace`). 
* **Impact:** Dropping multiple high-poly meshes (e.g., millions of triangles or multi-layered production FBX files) into the watch folder simultaneously will cause concurrent spikes in RAM that easily overrun hardware limits, leading to an OS-level Out-of-Memory (OOM) crash of the engine process.
* **Resolution Strategy:** Implement a localized execution limiter (such as a named counting semaphore or a specialized job tag inside your task graph system) specifically for `MeshCooker::cook`. Allow lightweight texture/scene cooks to scale freely across all cores, but limit heavy mesh imports to a strict concurrent cap (e.g., maximum of 2 to 4 running simultaneously).

### [MEDIUM] Race Condition on Unfinished File Writes
* **File / Location:** `texture_cooker.cpp` (`stbi_load`) & `mesh_cooker.cpp` (`ReadFile`)
* **Mechanism:** The background thread detects a file change based on basic directory iteration or OS file system notifications and immediately fires off cook tasks. It doesn't verify if the file has finished writing.
* **Impact:** If an external DCC tool (Blender, Substance Designer, Photoshop) is in the middle of saving a huge file when the asset pipeline triggers, `stbi_load` or Assimp will read a truncated/corrupted asset, leading to false-positive cook failures or data corruption.
* **Resolution Strategy:** Before passing the asset file path to the cooker, check if the file is locked by another process or poll its size/modification timestamp across two brief intervals to confirm it has fully settled.

---

## 2. Dependency Tracking & Invalidation

### [HIGH] Global `assetsChanged` Scene Invalidation Loop
* **File / Location:** `cook_service.cpp` (`CookService::cookSceneFiles`)
* **Mechanism:** The cooker triggers a blind re-evaluation of all scene files using the following check:
  ```cpp
  if (!stale && assetsChanged) stale = true;
Impact: If a technical artist changes a single low-res texture or modifies a single material setting anywhere in the project, assetsChanged flags true. The compiler will aggressively discard and re-cook every single scene file in the entire workspace. For large projects with thousands of scene chunks, this causes massive, unnecessary build times.Resolution Strategy: Eradicate the global boolean flag. Replace it with a lightweight Asset Dependency Graph. When a scene is cooked, store its direct asset reference UUIDs (meshes, textures, materials) in the SQLite registry. Only invalidate a scene file if its specific asset dependencies have changed.

## 3. Mathematical Precision & Scale Edge Cases 
### [HIGH] Degenerate Normal Matrix Scale Threshold (Determinant Trap)

File / Location: mesh_cooker.cpp (Normal Matrix Inversion/Transposition Pass)Mechanism: The cooker guards against singular, non-invertible transformations by evaluating the 3x3 determinant:

```
C++float det = nm.Determinant();
if (std::fabs(det) > 1e-12f) { ... }
else { nm = aiMatrix3x3(); }
```

Impact: The determinant of a scale matrix drops exponentially as it multiplies across dimensions ($Scale_X \times Scale_Y \times Scale_Z$). If an asset is authored at a large scale in a tool like Blender and scaled down to $0.0001$ inside the editor, its determinant will fall below $10^{-12}$. The cooker will falsely flag this valid matrix as degenerate and overwrite it with an Identity matrix. At runtime, the vertices will shrink, but the lighting normals will point in incorrect directions, resulting in broken shading/normals.Resolution Strategy: Normalize the matrix's basis vectors (stripping the scale component) prior to testing for degeneracy, or scale the epsilon check threshold dynamically relative to the matrix's Frobenius norm.

## 4. Algorithmic Complexity & Performance Bottlenecks

### [MEDIUM] $O(N^2)$ Complexity in Scene String Table GenerationFile / Location:
 scene_cooker.cpp (assetlib::stringTableAppend)Mechanism: As the scene cooker converts JSON records to flat binaries, it continually inserts item names, entity names, and material paths into a unified, sequential string table via linear deduplication scans.Impact: For massive, highly populated scenes (e.g., open-world chunks with tens of thousands of entities, trees, and props), the performance will decay exponentially into an $O(N^2)$ search pattern, turning what should be a fast conversion pass into a massive CPU stall.Resolution Strategy: Instantiate a temporary std::unordered_map<std::string, uint32_t> tracking pool locally inside cookSceneFile. Use this map to quickly handle $O(1)$ string-to-offset lookups during assembly, and write out the completely packed flat byte array to disk exactly once at the end.[LOW] Redundant Dynamic Allocations in Texture IngestionFile / Location: texture_cooker.cppMechanism: Pixel data is copied using vector assignment:C++asset.pixels.assign(px, px + w * h * 4);
Impact: stbi_load allocates heap memory for the image bytes, and std::vector::assign immediately performs a second global allocation and memory copy (memcpy) to move those pixels into the asset structural container. This causes unnecessary memory bandwidth churn during batch asset processing.Resolution Strategy: Replace the standard vector container with a custom, lightweight memory buffer wrapper that can explicitly adopt and wrap the ownership of the pointer returned directly from stbi_load, completely bypassing the extra allocation step.

## 5. Robustness & Error Resilience

### [HIGH] Unwrapped Worker Thread Exception Paths 
File / Location: cook_service.cppMechanism: Worker threads run task lambdas without outer high-level try/catch blocks.Impact: While your cook pipeline checks standard errors gracefully via return codes, if a third-party library (Assimp, nlohmann::json, or standard library vector reallocations) encounters an unexpected corrupted file condition and throws an exception (std::bad_alloc, nlohmann::json::parse_error, std::out_of_range), the background thread pool or worker thread will abruptly terminate. This will cause the background cook pipeline to lock up completely or silently crash the host editor.Resolution Strategy: Wrap the primary execution lambda within CookService in a generalized try { ... } catch (const std::exception& e) { ... } safety net. Log any catastrophic runtime errors cleanly to your Logger, tag the statistical snapshot as a failed asset cook, and keep the worker thread alive to safely process the remaining asset queue.
---

# RESOLUTION STATUS (July 10, 2026)

All claims re-verified against source before fixing; ONE was wrong.
Regression coverage: tests/cooker_test.cpp (unit lane).

| Finding | Verdict | Fix |
|---|---|---|
| Assimp parallel OOM | CONFIRMED | Counting-semaphore gate in MeshCooker::cook — hw/4 permits clamped [2,4]; texture/scene cooks still scale across all cores |
| Unfinished-write race | CONFIRMED | fileSettled(): sources (and scene JSON) must have mtime >= 750ms old; deferred files trigger a follow-up pass (400ms nap + requeue) |
| Global assetsChanged loop | CONFIRMED | Replaced with per-scene sceneDependsOnNewerAssets(): only scenes whose OWN referenced assets have newer cooked outputs re-cook |
| Determinant trap | CONFIRMED | cookNormalMatrix(): scale-invariant test (|det| vs row-norm product) — uniform scale passes at ANY magnitude; genuine singularity still falls back. Old |det|>1e-12 failed all scales below ~1e-4 |
| O(N²) string table | **FALSE** | stringTableAppend never had a dedup scan — pure O(1) append. The REAL defect was the opposite: zero dedup (100 shared refs = 100 copies, 4.4KB → 435B after interning). Fixed with an O(1) intern map |
| Texture double-copy | CONFIRMED, ACCEPTED | LOW; the fix (custom buffer adopting stbi ownership) contradicts this file's own "do NOT micro-optimize cookers" philosophy. Revisit if texture batch cooks ever profile hot |
| Unwrapped exceptions | CONFIRMED (×2) | Nets in BOTH thread sites: CookService::cookLoop (service survives a crashed pass) and CookPipeline::cookMany workers (a throwing cook() = per-asset failure, not process death) |
