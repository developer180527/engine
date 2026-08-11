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
  





## Review of the decimator and the cook path (2026-08-10) ✅ FIXED

Found reading everything between `b2a64ae` and `9b36bb1`. See `src/render/issues.md`
R21 for the renderer-side half and `src/runtime/docs/issues.md` for the services.

**`(int32_t)std::floor(NaN)` in the cell index.** The bounds pass skipped non-finite
positions deliberately (NaN fails both comparisons, commented), but the cell pass
computed an index from them anyway — and that conversion is undefined: `INT_MIN` on
x86-64, `0` on arm64. It made the cook ARCHITECTURE-DEPENDENT, which breaks the single
property the algorithm choice rests on (`decimate.h`: two machines cook byte-identical
levels, so the DDC stays a cache rather than a coin flip). Non-finite positions now get
no cell and the triangle pass drops anything referencing them, so a bad vertex costs its
own triangles and nothing else.

**Signed overflow in `CellHash`.** `c.x * 73856093` on `int32_t` overflows at a cell
index of **30** — `30 * 73856093 = 2 215 682 790 > INT32_MAX` — and the resolution
search goes to 1024, so essentially every real mesh hit it. Undefined behaviour that
wrapped harmlessly in practice and would trip the UBSan build
(`-DENGINE_SANITIZE=undefined`) on an ordinary asset. Multiplied as `uint32_t` now:
defined, identical bits.

**Submesh ranges were dropped, so every LOD level drew with `material[0]`.** The
user-visible half of this is in R21; the decimator half is that clustering is global
(a vertex belongs to one cell whichever group references it) while the index rebuild is
not, so partitioning the rebuild by group costs nothing. Output ranges tile from zero,
which keeps `Mesh::submeshesTile()` true and the shadow pass on its one-draw path.

**The zero-extent path dropped them too** — it copies indices verbatim, so the parent's
ranges still describe them exactly and are now carried through. One code path silently
producing single-material levels is exactly the kind of hole that outlives the fix.

**`if (mid == 0) break;` in `decimateToRatio` was unreachable** (`lo` starts at 1 and
only grows). Removed rather than left to imply a case that cannot happen.

**The scene cooker held a FIFTH copy of the JSON UB `core/json_read.h` was written to
remove.** That header lists four sites (entity_serializer, editor_prefs, undo_stack) and
missed the cooker, which reads the same hand-editable `.scene`:
`t["position"][0]` on `"position": []` indexes an empty vector through nlohmann's
unchecked const `operator[](size_type)`. The same pass also fixed ~45 `value()` calls,
which THROW on a key present with the wrong type — and the only `try`/`catch` here wraps
the parse, so `"id": "3"` escaped `cookSceneFile` on CookService's background thread.
Now every read goes through `jsonread::`, `entitiesOf()` tolerates a non-array
`"entities"`, and a non-object entity element is skipped instead of being indexed.

**Cooked mesh v4's LOD section stored counts independently of payload size and validated
neither** — see `modules/assetlib/info.md`. It was also unfuzzed: `buildPlausible` only
emitted v2, so no case reached it. `fuzz_mesh_loader_test` now generates v4/v5 levels,
carries `InflateLod*` corruptions (the generic ones only damage HEADER fields, so the
level checks were unreachable without them — confirmed by deleting the validation and
watching the lane pass anyway), round-trips levels including their range tables, and has
four regression seeds.


## The audio provider ABI and the frozen-layout mechanism (2026-08-10) ✅ FIXED

Same review as `src/runtime/docs/issues.md`. These are the contract defects — all
found before a single provider was written, which is the only time they are cheap.

**`createSound` could not honour its own documented lifetime rule.** The header
said `ENGINE_AUDIO_F_STREAM` means "the engine must keep `bytes` alive until
destroySound" — but F_STREAM was a PLAY-time flag. So the provider had to choose
retain-or-decode during `createSound`, before any `play()` told it which, and the
engine had to decide whether to keep the buffer alive on the strength of a call
that had not happened. Streaming is a property of the RESOURCE: the flag moved to
`createSound`.

**`EngineAudioEmitterUpdate` is a bulk array, so its stride was baked into every
provider.** The header's blanket rule ("append-only and carry structSize") cannot
hold for an array element — a size field per row would cost 4 bytes times hundreds
every frame. `updateEmitters` now takes a `stride`, passed once, which is what
Vulkan and D3D12 do and the only thing that makes the struct extensible: without
it, adding a field silently misaligns every read in every provider already
compiled. `EngineAudioListener` gained the `structSize` every other struct in the
header already carried.

**`getStats` was documented "(any thread)" while the struct promised no two calls
would ever overlap.** A diagnostics overlay reading stats while the game thread
calls `play()` IS that overlap, and a provider author following the general rule
would not have made it safe. Now an explicit carve-out.

**Nothing detected C-side drift in the audio layout.** The conformance crate
hand-transcribes the structs as Rust `#[repr(C)]` and asserts hardcoded sizes —
against its OWN layout, so it caught Rust-side drift and was blind to the C side.
All seven agreed when checked, but a change to the header would have left the
suite green. `ENGINE_AUDIO_FROZEN` now asserts the same numbers in C, so neither
half can move alone. Verified by making them disagree.

**`ENGINE_API_FROZEN` enforced one of rule 1's four prohibitions.** Rule 1 forbids
appending, reordering, removing and repurposing; `sizeof` catches only appending.
Swapping two groups, or two same-typed pointers inside a group, keeps every size
identical and redirects every call an older module makes. Added
`ENGINE_API_GROUP_AT` (twelve group offsets, checked against the compiler) and
`ENGINE_API_FIELD_AT` (first and last pointer of each new group). Mutation-proved:
a group swap fails three asserts, and a same-size field reorder — invisible to the
size check — fails the field asserts.

**And the C ABI header did not compile as C.** `ENGINE_API_FROZEN` used bare
`static_assert`, which is C++ (or C23); C11 spells it `_Static_assert`, so the
whole `extern "C"` table header failed in any C11 build. Unnoticed because
everything including it today is C++ — but the point of a C ABI table is that a C
kit can use it. Both headers now dispatch on `__STDC_VERSION__`, and a C11 compile
is part of what was checked.

**Hygiene:** 17 MB of Rust `target/` was one `git add -A` from being committed —
`.gitignore` listed `modules/net/target/` specifically, so the new crate was not
covered. And the conformance suite ran NOWHERE: not in ctest, not in CI. It is now
`audio_abi_conformance` in the unit lane, gated on cargo like `modules/net`.
`native_provider_conforms` still skips unless `ENGINE_AUDIO_PROVIDER` names a
module — correct, since no host provider exists yet — but the reference provider
and a new wide-stride forward-compat case now run on every ctest.
