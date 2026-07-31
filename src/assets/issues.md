---
status: unreviewed
---
# Issues (Sat Jul 11 17:17)

### Unchecked GPU Buffer Allocations
In mesh_loader.cpp, the handles returned by bgfx::createVertexBuffer and bgfx::createIndexBuffer are wrapped directly into a Mesh object and registered into storage.meshes without any validation. If the graphics server runs out of memory, receives a zero size due to file corruption, or fails to allocate the buffers, these invalid handles are silently stored. This will trigger downstream rendering crashes or undefined behavior when the engine attempts to draw with them.  

### Missing Vector Size and Bound Validation
While MeshBinaryLoader::load performs a sanity check on the vertex stride size, it completely omits verification of the actual data payloads. The loader does not check if asset.vertexData.size() == h.vertexCount * h.vertexStride or if asset.indexData.size() == h.indexCount * h.indexStride. If a .cooked file is truncated or corrupted, bgfx::copy will only copy the smaller available byte size. However, the Mesh object is still registered with the full h.indexCount. When the GPU attempts to render the mesh, it will perform out-of-bounds reads on the under-allocated buffer, causing a driver timeout (TDR) or severe visual artifacts.  
### Crash via Uncaught Exceptions During Static Initialization
In asset_path.h, executableDir() caches its
resolved path within a lambda that initializes a static const std::filesystem::path dir. On Linux,
this lambda unconditionally executes fs::canonical("/proc/self/exe"). If the application is executed
within a restricted sandbox, flatpak, chroot, or specific container environment where the /proc
filesystem is masked or inaccessible, fs::canonical will throw a std::filesystem::filesystem_error.
Because there is no try-catch block around this static allocation, the entire application will
instantly crash before main() even begins executing.  

### ANSI Encoding and Buffer Truncation Flaws on Windows
The Windows resolution path in asset_path::executableDir utilizes GetModuleFileNameA alongside a fixed-size buffer of char buf[1024]. This introduces two distinct bugs:  

* Unicode Breakdown: If the engine is installed in a path containing non-ASCII characters (such as localized user folders or emojis), the ANSI variant GetModuleFileNameA will mangle the characters. This causes fs::canonical(buf) to fail to resolve the path or throw an exception.  

* Silent Truncation: If the absolute path to the executable exceeds 1024 characters, GetModuleFileNameA truncates the string to fit the buffer and does not null-terminate it cleanly. This passes corrupted garbage memory straight into fs::canonical.  

### CWD Dependency Fallback When projectRoot Is Empty
In asset_ref.h, the assetref::resolve function contains a logical flow gap when projectRoot is empty. If projectRoot is empty, Step 2 is skipped completely, and execution falls through to Step 3: if (fs::exists(p)) return p.string();. If ref.path contains a relative path, calling fs::exists(p) directly evaluates that relative path against the application's current working directory (CWD). This reintroduces a fragile and unpredictable dependency on the CWD, directly violating the core engineering objective documented in asset_path.h.  

### Destructive Strided Index Fallback Assumption
In mesh_loader.cpp, the index layout is determined by checking const bool use32 = (h.indexStride == 4);. If a corrupted or malformed cooked header supplies an invalid stride value (such as 0, 1, or 3), use32 evaluates to false. The loader then passes BGFX_BUFFER_NONE, forcing the system to interpret the data stream as 16-bit (uint16_t) indices. This silent, incorrect fallback misaligns the byte offsets during rendering, leading to completely scrambled primitive geometry.

---

## RESOLUTION (verified against source 2026-07-21)

All six re-checked against source and **fixed**. Note: `mesh_loader.cpp` (`MeshBinaryLoader`) is currently a legacy path with **no live callers** — the runtime streams cooked meshes through `AsyncLoader`/`AssetService` — but it compiles into `engine_runtime`, so it was hardened defensively rather than left as a trap for a future caller.

| # | Claim | Verdict | Fix |
|---|-------|---------|-----|
| 1 | Unchecked GPU buffer allocations (mesh_loader) | **TRUE** | `isValid` guard on `vbh`/`ibh`; invalid → destroy + clean fail, never registered. |
| 2 | Missing payload size/bound validation (mesh_loader) | **TRUE** | Header counts now validated against actual byte payloads (`vertexData.size() == vertexCount*stride`, same for indices); a truncated `.cooked` fails cleanly instead of the GPU reading OOB at draw. |
| 3 | Uncaught exception in static init (asset_path) | **TRUE** | `executableDir()` now uses the `error_code` overloads of `fs::canonical`/`current_path`, wrapped in `try/catch`, degrading to CWD. A masked `/proc` (chroot/flatpak) can no longer `std::terminate` before `main`. |
| 4 | ANSI/truncation flaws on Windows (asset_path) | **TRUE** | Switched to `GetModuleFileNameW` + a growing buffer with an explicit truncation check — handles non-ASCII install paths and long paths without unterminated garbage. (Windows port still deferred, but the trap is closed.) |
| 5 | CWD fallback when projectRoot empty (asset_ref) | **TRUE** | `resolve()` step 3 now accepts only an **absolute** legacy path; a bare relative ref no longer silently resolves against the process CWD. |
| 6 | Destructive strided index fallback (mesh_loader) | **TRUE** | Index stride is now validated to be exactly 2 or 4; a corrupt `0/1/3` fails cleanly instead of being reinterpreted as 16-bit. |
  



