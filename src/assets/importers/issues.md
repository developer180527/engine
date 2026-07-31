---
status: unreviewed
---
# Issues (Sat Jul 11 17:01)

### Animation Data Loss on Cache Hit
In importer_registry.h, the loadCached function drops all skeleton and animation data upon a cache hit. When a path is found in m_cache, the registry returns MeshImportResult::ok(it->second). Looking at the definition of MeshImportResult::ok in mesh_importer.h, this factory function exclusively assigns the MeshHandle, leaving the skeleton handle initialized to null and the clips vector entirely empty. Consequently, the first load of a skinned asset successfully tracks animations, but any subsequent call to loadCached for that asset strips away the skeleton and clips entirely.

### Rigid Submesh Collapse in Skinned Assets
In assimp_importer.cpp, the asset loader implements two separate paths for static and skinned models. When skinnedModel evaluates to true, the code loops directly through the flat array of scene->mMeshes from 0 to scene->mNumMeshes. This completely bypasses the node tree traversal (emitStaticNode) used by the static path. Because it ignores the scene graph hierarchy, any completely rigid submesh inside a skinned asset file (such as a weapon or armor accessory attached to a node rather than weighted to bones) entirely loses its local transformation matrix and collapses onto the asset's root origin.

### Unchecked GPU Buffer Creation
Inside finalizeMesh in assimp_importer.cpp, the handles returned by bgfx::createIndexBuffer and bgfx::createVertexBuffer are never validated before use. If the graphics server runs out of memory or fails to allocate the buffers, these invalid handles are wrapped straight into the Mesh object and registered into storage.meshes. This stands in direct contrast to gltf_importer.cpp, which explicitly checks bgfx::isValid and returns a failure state if buffer allocation drops.  

### SILENT Dropping of Non-Indexed glTF Geometry
Inside gltf_importer.cpp, the node processing loop checks primitive attributes with the line: if (!posAcc || !prim.indices) continue;. The glTF specification explicitly permits non-indexed geometry, where drawing commands simply stream vertices sequentially from the position accessor. This validation check silently skips any valid non-indexed glTF primitive in the asset file without throwing an error or logging a warning.

### Missing Bounds Checks on Texture Coordinates
During vertex accumulation in gltf_importer.cpp, the normal attribute safely verifies the index limits: if (normalAcc && i < normalAcc->count). However, the texture coordinate parsing block directly below it completely omits the count boundary verification, executing only if (uvAcc) readFloats(uvAcc, i, v.uv, 2);. If a malformed or asymmetric glTF file specifies fewer UV coordinates than vertex positions, the loop will read out-of-bounds indices from cgltf_accessor_read_float when i exceeds the UV count.  

### Erroneous Channel Mapping in Embedded Textures
In importTexture within assimp_importer.cpp, the code processes uncompressed embedded textures with a comment stating: // Assimp stores as ARGB; reorder to RGBA for bgfx. Despite this explicit intent, the loop performs a direct, unswizzled component-to-component mapping:
```
pixels[p*4+0] = src[p].r;
pixels[p*4+1] = src[p].g;
pixels[p*4+2] = src[p].b;
pixels[p*4+3] = src[p].a;
```
Because it maps Red directly to Red and Alpha directly to Alpha, no actual color channel reordering occurs, leaving the color channels inverted or scrambled if Assimp truly exposed them in a non-RGBA layout.  

### Broad Substring Matching in Texture Discovery
The automated file discovery system in discoverTexture uses loose substring matching to find textures when an asset lacks explicit paths. The condition if (lf.find(ls) != std::string::npos) scans the entire lowercased filename for short indicator strings like _d or _color. Because it does not anchor the search to the end of the filename or validate delimiters, a secondary texture map named character_color_normal.png will falsely trigger a match for _color, or object_d_roughness.png will match _d, causing the importer to bind a normal or roughness map as the primary diffuse texture.

### Undefined Behavior Risk via Character Conversion
Across assimp_importer.cpp and importer_registry.h, string lowercasing loops utilize the standard pattern c = (char)std::tolower(c);. In C++, passing a standard signed char directly to std::tolower without first casting it to an unsigned char triggers undefined behavior if the execution encounters high-bit characters (such as international characters or specific UTF-8 sequences in file paths and texture names).

---

## RESOLUTION (verified against source 2026-07-21)

These are the dev/editor **source-import** path (`ImporterRegistry` in `runtime.h`), gated by `ENGINE_WITH_SOURCE_IMPORTERS` and stripped from the shipping runtime. Each claim re-checked against source.

| # | Claim | Verdict | Fix |
|---|-------|---------|-----|
| 1 | Animation data lost on cache hit | **TRUE** | **FIXED + regression** — `m_cache` now stores the full `MeshImportResult`, not just the `MeshHandle`, so a cache hit returns skeleton + clips. `import_test` loads a skinned FBX twice through `loadCached` and asserts both survive; reverting the fix makes it fail (2 assertions), proving coverage. |
| 2 | Rigid submesh collapses in skinned assets | **TRUE (deferred)** | Real: the skinned path merges `scene->mMeshes` flat with no node transform, so a rigid accessory (weapon/armor on a node, not weighted) binds to bone 0 at model origin. Correct fix = per-submesh split (skinned verts as-is, unweighted verts baked at their node world transform in one merged buffer). Involved; tracked, not done here. Does **not** affect the shipping cook path (separate `cookGltf`/Assimp cook code). |
| 3 | Unchecked GPU buffer creation (finalizeMesh) | **TRUE** | **FIXED** — `finalizeMesh` now validates `vbh`/`ibh`, destroying + returning an invalid handle on failure; the caller maps that to `MeshImportResult::fail` instead of reporting success with a dead mesh. Matches what `gltf_importer` already did. |
| 4 | Non-indexed glTF geometry silently dropped | **TRUE** | **FIXED** — the `!prim.indices` skip is gone; a non-indexed primitive (valid per spec) now synthesizes a sequential index run `0..count`. |
| 5 | Missing bounds check on UV coords | **TRUE (no live OOB)** | **FIXED** for symmetry — UV read now guarded by `i < uvAcc->count` like the normal read. Note `cgltf_accessor_read_float` already internally bounds-checks and returns 0 on overflow, so there was no actual OOB; the guard removes the inconsistency and avoids relying on that. |
| 6 | Embedded texture channel scramble (ARGB) | **FALSE** | `aiTexel`'s in-memory layout is BGRA, but the code reads `.r/.g/.b/.a` **by member name**, which yields correct RGBA regardless of byte order. No channel bug — only the old comment was misleading; clarified. |
| 7 | Broad substring texture discovery | **TRUE** | **FIXED** — `discoverTexture` now vetoes any candidate whose name contains a non-color token (`normal`, `_nrm`, `rough`, `metal`, `_ao`, …) before applying diffuse-suffix matching, so `rock_COL_normal.png` no longer binds as the base color. |
| 8 | UB via `(char)tolower(signed char)` | **TRUE** | **FIXED** — all lowercasing loops in `assimp_importer.cpp` and `importer_registry.h` now cast through `(unsigned char)` first. |
