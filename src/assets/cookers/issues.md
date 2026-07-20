# Issues (Sat Jul 11 16:56)

1\. The try/catch Illusion with Native Code
Wrapping runOneCookPass in a standard C++ try/catch(...) block only catches C++ exceptions. It will not trap native hardware exceptions or OS signals like SIGSEGV (segmentation faults), SIGBUS, or std::abort() called from deep within raw C/C++ libraries like Assimp. If a corrupted FBX triggers a null-pointer dereference or an out-of-bounds read inside a third-party dependency, the entire editor process will drop instantly.

2\. Static Thread Semaphores vs. Dynamic Memory Pressure
Clamping Assimp parsing to a fixed hardware_concurrency / 4 is a rigid proxy for memory management.

* On low-core systems (e.g., a 4-core machine), it drops the geometry pipeline to 1 single thread, creating a massive bottleneck even if the meshes being loaded are tiny 10KB files.

* On high-core systems (e.g., 32 cores), it allows 8 concurrent threads. If an artist drops eight massive 3GB CAD-exported FBX files into the watch folder simultaneously, those 8 threads can still easily trigger an Out-Of-Memory (OOM) crash.

3\. Non-Uniform Zero-Scale Matrix Wipeout
The row-norm product singularity guard works brilliantly for uniform scaling, but it breaks under intentional non-uniform flattening. If an artist intentionally flattens a mesh along a single axis (e.g., setting the local scale of a UI card, mirror, or 2D sprite plane to X = 0), the row-norm product drops to exactly 0. This incorrectly flags the entire matrix as a degenerate singularity, causing your guard to wipe out all valid rotation and translation data and reset the transform completely to an identity matrix.

4\. The 750ms Burst-Write Trap
Relying strictly on a 750ms modification time (mtime) age assumes that file writes are linear and continuous. If an artist exports a massive 8K texture or a dense scene asset from Substance Painter or Maya over a slower storage medium (like a network drive, a saturated SATA SSD, or an external HDD), the DCC's file-writing thread can easily stall or pause for more than 750ms between data chunks. The CookService will assume the file has settled, attempt to cook a partially written, corrupted asset, and potentially cache a broken build.

5\. Opposing Vector Normalization NaNs
When downsampling normal maps for lower mip levels, averaging vectors before re-normalizing is mathematically correct for preserving direction, but it introduces a severe edge case. If a high-frequency normal map contains micro-creases where adjacent texels point in exactly opposite directions (e.g., one vector is (0.707, 0.707, 0) and its neighbor is (-0.707, -0.707, 0)), averaging them results in a zero vector (0, 0, 0). Calling normalize() on a zero vector produces NaN values, which will cascade down and corrupt the entire remaining mip chain with black pixels or rendering artifacts.

6\. CPU Block Compression Bottlenecks
BC7 texture encoding is notoriously CPU-heavy and slow. If a user drops a dozen new uncompressed textures into the project, running BC7 compression synchronously within the asset worker pool will heavily paralyze the responsiveness of the cook loop. Without a robust Content-Addressable Storage (CAS) or a local GUID-based hash cache to verify if the raw texture bytes have actually changed, minor scene re-saves will force repetitive, expensive compression passes.

---

## RESOLUTION (verified against source 2026-07-21)

Every claim re-checked against the actual code before acting.

| # | Claim | Verdict | Action |
|---|-------|---------|--------|
| 1 | try/catch can't catch SIGSEGV from Assimp | **TRUE (known limitation)** | C++ `try/catch(...)` genuinely can't trap signals. The robust fix is out-of-process cooking (cook worker in a child process, crash = failed asset not dead editor). That's a real architecture change — tracked, not done here. |
| 2 | hw/4 clamp → 1 thread on 4-core, 8 on 32-core | **NUMBERS FALSE** | The gate is `std::clamp(hw/4, 2u, 4u)` (`mesh_cooker.cpp:456`): 4-core → **2**, 32-core → **4**, never 1 or 8. The "memory-aware throttle" ideal is valid but is a hard heuristic (per-file peak RSS is unknowable pre-parse); the fixed clamp is a deliberate, bounded tradeoff. |
| 3 | Non-uniform zero-scale wipes rotation+translation | **FALSE** | `cookNormalMatrix` returns only the 3×3 **normal** matrix. Vertex **positions** are transformed by the separate `world` matrix (`emitMesh` `w3`), which is never touched. A genuinely flattened (rank-deficient) basis has no defined inverse-transpose, so the identity fallback is correct; normals are re-normalized after transform regardless. No position/translation loss. |
| 4 | 750ms mtime settle cooks partial slow writes | **TRUE** | **FIXED** — `fileSettled` (`cook_service.cpp`) now also requires the `(size, mtime)` pair to be **unchanged since the previous poll**. A file still growing between passes fails the stability check even if its mtime looks old; it only settles once writing has actually stopped. |
| 5 | Opposing normal texels → zero vector → NaN mips | **FALSE** | `downsample2x2` already guards it: `if (len > 1e-6f) normalize else n=(0,0,1)` (`texture_encode.cpp:56-58`). No `normalize()` of a zero vector, no NaN. |
| 6 | BC7 CPU-heavy + no CAS/hash cache | **PARTIALLY FALSE** | BC7 cost: **addressed** — quality dropped `Default`→`Fastest` (nvtt exhaustive search was the fan-spinner). CAS claim is **false**: the pipeline already hashes source bytes (`computeHash`/`isStale`); a scene re-save leaves texture hashes unchanged, so textures are **not** recompressed. |

**Fixed:** #4. **Addressed:** #6 (quality). **False/already-handled:** #3, #5, #6 (CAS). **Deferred (real, architectural):** #1. **Refuted numbers:** #2.

