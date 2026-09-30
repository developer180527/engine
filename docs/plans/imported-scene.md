---
status: target
---
# ImportedScene — the engine's own import format

> **Status: design (WO-009).** Nothing here is built yet. WO-010 builds the type
> and its contract test, WO-011 the one back end, WO-012/013 the front ends.
> The contract is `import-frontend` in `docs/contracts/`.

## 1. The problem, measured

Nothing the engine owns sits between "a parser read the file" and "write the
cooked asset". Each source format has a complete cook path of its own, and the
runtime has three more. Read on 2026-09-30:

**Two libraries, seven read sites, five Assimp configurations:**

| read site | library | where | what it produces |
|---|---|---|---|
| `cookStatic` (inline in `MeshCooker::cook`) | Assimp | `mesh_cooker.cpp` | `MeshAsset` v2, world transforms baked |
| `cookSkinned` | Assimp | `mesh_cooker.cpp` | `MeshAsset` v6, mesh space, ozz skeleton + clips |
| `cookGltf` | cgltf | `mesh_cooker.cpp` | `MeshAsset` v2, world transforms baked |
| `GltfImporter` | cgltf | `assets/importers/gltf_importer.cpp` | a live GPU `Mesh`, static only |
| `AssimpImporter` | Assimp | `assets/importers/assimp_importer.cpp` | a live GPU `Mesh` |
| `AsyncLoader::processFile` | Assimp | `runtime/services/async_loader/parse.cpp` | a live GPU `Mesh` + skeleton + clips |
| cook-on-first-bind | Assimp | `animation/clip_library.h` | an ozz clip |

**They disagree, and each disagreement is a way for one asset to look different
depending on which path read it:**

| | cooker static | cooker skinned | `AssimpImporter` | `parse.cpp` | `clip_library` |
|---|---|---|---|---|---|
| `FBX_PRESERVE_PIVOTS` | default (true) | **false** | default (true) | **false** | **false** |
| `ImproveCacheLocality` | yes | yes | no | no | no |
| points/lines | `SBP_REMOVE` | `SBP_REMOVE` | skipped by hand | skipped by hand | n/a |

- **Tangents.** Assimp computes them (`CalcTangentSpace`). The cgltf path only
  reads them: a glTF with no `TANGENT` gets a constant `(1,0,0,1)` on every
  vertex, so a normal-mapped glTF exported without tangents shades wrong, and
  the same model as FBX shades right. The glTF spec says to generate them.
- **Texture lookup.** The cooker stores a texture's *basename*. `parse.cpp`
  tries `dir/<path>`, then the bare filename under `""`, `textures/`,
  `Textures/` and `tex/`, then a case-insensitive match against embedded
  textures. Two rules for finding the same texture.
- **Skins and animations** exist on the Assimp paths only. A skinned glTF is
  refused (WO-002).
- **Animation takes Assimp types**: `buildOzzClip(aiAnimation*)`,
  `extractSkeleton(aiScene*)`.

A new format today would be an eighth read site. The design below makes it one
front end.

## 2. The shape

```
 source file ──▶ FRONT END (one per library) ──▶ ImportedScene + dropped list
                 Assimp · cgltf · (USD, FBX SDK, own format …)        │
                                                                     ▼
                                          BACK END (one) ──▶ cooked assets
                                          transforms · tangents · LOD · skin ·
                                          textures · clips
```

- A **front end** converts a library's data structure into `ImportedScene`, in
  the engine's conventions (§4), and reports what it could not represent (§5).
  It does no cooking decisions: no baking, no tangent generation, no LOD, no
  texture encoding.
- The **back end** is the only code that writes cooked assets. It is written
  once, against `ImportedScene`, and never sees a library type.
- `aiScene`, `aiMesh`, `aiAnimation` and `cgltf_*` never leave a front end.
  That becomes an audit rule (WO-013), in the same style as LAYER-04.

## 3. The type

Values, not views: an `ImportedScene` owns everything in it, so the library's
own memory (`aiScene`, `cgltf_data`) is freed before the front end returns.

```cpp
struct ImportedScene {
    std::string              source;      // the path it came from, for messages
    std::vector<ImportedNode>     nodes;  // hierarchy; nodes[0] is the root
    std::vector<ImportedMesh>     meshes;
    std::vector<ImportedMaterial> materials;
    std::optional<ImportedSkeleton> skeleton;
    std::vector<ImportedClip>     clips;
    std::vector<Dropped>          dropped;  // §5 — never empty-by-omission
};

struct ImportedNode {
    std::string name;
    int32_t     parent = -1;
    Mat4        local;                  // in engine conventions (§4)
    std::vector<uint32_t> meshes;       // instancing: one mesh, many nodes
};

struct ImportedMesh {
    std::string name;
    // Streams, one entry per vertex. positions/normals always; the rest may be
    // empty, and an empty stream is a FACT the back end acts on (tangents).
    std::vector<Vec3> positions, normals;
    std::vector<Vec4> tangents;         // w = handedness; EMPTY if the source had none
    std::vector<Vec2> uv0;
    std::vector<std::array<uint16_t,4>> joints;   // empty unless skinned
    std::vector<Vec4>                   weights;  // empty unless skinned
    std::vector<uint32_t> indices;      // triangles only
    std::vector<ImportedSubmesh> submeshes;   // {firstIndex, indexCount, material}
};

struct ImportedMaterial {
    std::string name;
    Vec4        baseColorFactor {1,1,1,1};
    TextureRef  baseColor, normal;      // either may be empty
};

struct TextureRef {                     // exactly one of these, or neither
    std::string path;                   // as written in the source, resolved per §4
    std::vector<uint8_t> embedded;      // bytes of an embedded image
    std::string embeddedName;           // for dedup and messages
};

struct ImportedSkeleton {
    struct Bone { std::string name; int32_t parent; Mat4 bindLocal; Mat4 inverseBind; };
    std::vector<Bone> bones;            // parents before children
};

struct ImportedClip {
    std::string name;
    float       duration;               // seconds
    struct Track { std::string bone; std::vector<Key3> t, s; std::vector<KeyQ> r; };
    std::vector<Track> tracks;          // bound by bone NAME, as ozz binding is today
};
```

### Decision: one mesh type, not a static one and a skinned one

A skinned mesh is a mesh with two more streams. Real files mix them: a character
with a rigid helmet is one model with bone-less submeshes, which the importers
already bind rigidly to bone 0. Two types would force every front end to decide
early which one a file is. That decision belongs to the back end, which sees the
whole scene: skeleton present → the skinned layout (v6), otherwise the static
one (v2).

**This is not retained-scene open question #13.** WO-009 and WO-020 both named
#13 as "the same question", and that was wrong. #13 asks whether the *render*
instance table has one row type or two. That is a runtime layout question,
decided by what the renderer reads each frame. This is an *import* representation,
decided by what files contain. They are answered separately. #13 stays open in
`renderer-program.md` §9.2.

## 4. Conventions — converted once, in the front end

The back end assumes these and never checks a source format for them:

| | engine convention | Assimp front end | cgltf front end |
|---|---|---|---|
| handedness / up | right-handed, +Y up | whatever the file's node transforms say (see below) | native |
| units | metres | see below | native |
| UV origin | top-left | `aiProcess_FlipUVs` | native |
| triangles only | yes | `Triangulate` + `SortByPType` + `SBP_REMOVE` | non-triangle primitives → `dropped` |
| FBX pivots | baked (`PRESERVE_PIVOTS=false`) | **one setting for static and skinned** | n/a |
| texture paths | relative to the source file's directory; embedded by exact name | one rule, §4.1 | same rule |

**Units and axes for FBX are an open question, not a claim.** Today the static
path bakes world transforms, so a centimetre, Z-up FBX comes out right because
the scale and rotation live in its node transforms. The skinned path keeps
vertices in mesh space, so it does not obviously get the same correction. The
cooker test's `rotXNeg90Scaled` fixture is exactly this case for static meshes.
**WO-010's contract suite must contain a centimetre, Z-up skinned fixture**, and
its result decides whether the Assimp front end applies
`AI_CONFIG_FBX_CONVERT_TO_M` or converts the root itself.

### 4.1 Texture references — one rule

1. A relative path resolves against the source file's directory. An absolute
   path is used as-is.
2. A reference that names an embedded texture (Assimp `*N` or an exact embedded
   name; a glTF `bufferView`) carries the bytes in `embedded`.
3. **Nothing else.** No search of `textures/`, `Textures/` or `tex/`, and no
   case-insensitive match. A reference that does not resolve goes in `dropped`
   with its path, so the user sees which texture is missing instead of getting
   a different file that happened to share its name.

Rule 3 is deliberately stricter than `parse.cpp`, and it will make some
currently-working assets report a missing texture. That is the point: they work
today by accident, and differently in the cooker.

## 5. The dropped list — every loss is reported

The WO-002 lesson made general. `gltf_losses.h` was the first case of it.

```cpp
struct Dropped {
    enum class Kind { Skin, Animation, MorphTargets, VertexColours, ExtraUvSets,
                      NonTriangles, Texture, Camera, Light, Extension };
    enum class Effect { Wrong, Less };   // see below
    Kind        kind;
    Effect      effect;
    uint32_t    count = 1;
    std::string what;                    // "3 morph targets on 'Face'", a texture path, …
};
```

- **`Wrong`**: the asset without it is incorrect. A character without its
  skeleton, or a required glTF extension the front end cannot read. **The back
  end refuses the cook** and names what was lost.
- **`Less`**: the asset is still correct, just less. Node animation on a static
  prop, vertex colours the shader does not read, a light in the file. **The
  back end cooks and logs it.**

The front end decides the effect, because only it knows the source. The back end
decides the action. A front end that silently omits something is a contract
violation that the contract suite tests for (WO-010).

## 6. What each of today's read sites becomes

| today | becomes | what it stops doing |
|---|---|---|
| `cookStatic`, `cookSkinned` | Assimp front end + back end (WO-013) | two Assimp setups become one; baking and tangent handling move to the back end |
| `cookGltf` | cgltf front end + back end (WO-012) | baking moves to the back end; missing tangents become generated instead of `(1,0,0)` |
| `GltfImporter`, `AssimpImporter`, `parse.cpp` | **deleted** by WO-018: the runtime loads cooked assets only, and a missing one becomes a cook job | three runtime parsers, the four-directory texture search, and the "looks different until cooked" class of bug |
| `clip_library` cook-on-first-bind | clip cooker over the same front end (WO-016) | an Assimp parse inside the running editor |
| `extractSkeleton(aiScene*)`, `buildOzzClip(aiAnimation*)` | take `ImportedSkeleton` / `ImportedClip` (WO-015) | animation depending on one parser's types |
| `gltf_losses.h` | the cgltf front end's dropped list (WO-012) | a one-off loss check |

## 7. The back end's responsibilities, listed so none are dropped

Written once, against `ImportedScene`:
- **Bake** node world transforms into vertices for static meshes, with the
  scale-invariant normal-matrix guard (`gltfNormalMatrix` today). Keep mesh
  space for skinned meshes.
- **Tangents**: generate them for any mesh with a normal texture and an empty
  `tangents` stream. *Open: which generator.* MikkTSpace is the one the glTF
  spec names, and it is not in `third_party/`. Adding it is a dependency
  decision for WO-011.
- **Vertex cache order** (`ImproveCacheLocality`'s job, today Assimp-only), so
  every format gets it.
- LOD chain and decimation, sibling textures and their content dedup, material
  records, the ozz skeleton and clips: everything `mesh_cooker.cpp` does today
  after reading.
- **Byte-identical output** for the existing test assets, as the bar for
  switching each format over (WO-011). Where it cannot be identical (vertex
  order from cache optimisation on glTF, generated tangents), each difference
  is listed and accepted.

## 8. Open questions this design leaves

| # | question | decided in |
|---|---|---|
| 1 | FBX units/axes on the skinned path (§4) | WO-010's fixture |
| 2 | tangent generator: MikkTSpace or our own | WO-011 |
| 3 | morph targets: dropped (`Wrong` or `Less`?) until the renderer can play them | WO-010 |
| 4 | vertex colours, extra UV sets: `Less` today; carried when a shader reads them | when a shader needs them |
| 5 | whether the Assimp front end keeps its memory permit (`AssimpGatePass`) or the cook pipeline owns it | WO-013 |
