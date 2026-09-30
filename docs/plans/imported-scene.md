---
status: target
---
# ImportedScene — the engine's own import format

> **Status: design (WO-009), type landed (WO-010), back end built (WO-011), glTF switched (WO-012), Assimp switched (WO-013), glTF skins and clips (WO-014): every mesh format cooks through ImportedScene, glTF characters included.** `src/assets/import/` holds
> the type, the contract's shape, its structural checks and its fake, and
> `tests/import_contract.h` is the suite every front end must pass. Still to
> build: WO-011 (the one back end), WO-012/013 (the front ends). The contract is
> `import-frontend` in `docs/contracts/`, now `provisional`.

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

**`src/assets/import/imported_scene.h` is the source of truth; this sketch is the design that led to it.** The one difference: the header uses its own small POD math types (`Float3`, `Float4x4`, …) instead of the engine's `Vec3`/`Mat4`, because `Vec3` is `bx::Vec3`, and a format that must outlive every library cannot borrow one (audit LAYER-05). Also added: `Float4x4` is column-major with the translation in `m[12..14]`, and counter-clockwise is the front face.

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

### 7.1 What the back end decided, where the old paths disagreed (WO-011)

`src/assets/cookers/mesh/mesh_backend.cpp` is built. Reading all three old cook
paths in full found that they disagree on more than §1 listed, so "byte-identical
to today" cannot hold for every format: no single rule reproduces three that
contradict each other. Each choice below is deliberate, and WO-012/013 compare
old and new output **except** for these, which they list per test asset.

| | static Assimp | skinned Assimp | cgltf | **back end** |
|---|---|---|---|---|
| embedded texture | stored `"*0"` as a basename: **resolves to nothing** | decoded → sibling `.ctex` | decoded → sibling | **decoded → sibling** |
| external texture | basename only | basename only | decoded → sibling | **decoded → sibling** |
| missing tangents | computed by Assimp | computed by Assimp | constant `(1,0,0,1)` | **generated** (Lengyel, Assimp's handedness convention) |
| tangent handedness `w` | computed | **forced to +1** | from the file | **from the source, flipped by a mirroring transform** |
| mirrored instance (det < 0) | inside-out | n/a | inside-out | **winding flipped back** |
| mesh with no weights in a skinned file | n/a | **silently dropped** | n/a | **bound rigidly to the nearest ancestor bone**, and reported |
| clip names | Mixamo junk → file stem | same | n/a | **as given**: junk-name cleanup is the Assimp front end's (WO-013) |

**Why every texture becomes a sibling.** A cooked mesh's texture reference
resolves in two ways (`AssetService::resolveTexture`): a sibling `.ctex` beside
the cooked file, or a registry lookup of `<cooked dir>/<name>`. The registry
step can never match a source texture from there, and a shipped build has no
registry. So a bare basename resolves only by accident. **Suspected, not yet
proven:** a cooked static FBX with external textures renders untextured. WO-013
proves or refutes it with a fixture before it switches that format. The cost of
the rule is that a texture shared by two meshes is cooked once per mesh. Dedup is
per asset. Referencing the texture's own cooked record by uuid is the later fix.

**Tangents.** Generated per triangle from UV derivatives, orthogonalised against
the normal, with `w` chosen so `cross(N, T) * w` points along increasing v: the
convention of the Assimp path's `CalcTangentSpace`, so FBX and glTF agree. This
answers §8 Q2 for now: our own generator, not MikkTSpace. Normal-map seams on
real content are the signal to revisit, and that needs a visual check.

### 7.2 glTF switched over, and what the comparison showed (WO-012)

`imp::CgltfFrontend` (`src/assets/import/frontend_cgltf.*`) replaced
`cookGltf`. Before deleting the old path, both paths cooked every glTF in the
tree, and the results were compared field by field: `Duck`, `robot`,
`Demon_Mask`, `person` (26 primitives, 1.68 M vertices), `Television_01_4k`
and `lion_head_4k`.

- **Identical on every file:** vertex positions and UVs, indices, submeshes,
  bounds, materials (factors, roughness, metallic, flags, texture names), LOD
  counts, and every sibling texture byte-for-byte. Normals differ in the last
  bit of a float (5.96e-8), from normalising after the transform.
- **Tangents differ on every file, as §7.1 intends.** None of these files has a
  `TANGENT` attribute (checked), so the old path wrote a constant `(1,0,0,1)`
  and the new one generates real tangents.
- **`Television_01_4k` has no base-colour texture on either side.** Its glTF
  names `textures/…jpg`, but the files sit beside it with no `textures/` folder
  (the download flattened it). The old path lost the texture silently. The new
  one reports it, and also reports the `COLOR_0` it drops. An asset problem,
  made visible.
- `cannon_01_4k` fails identically on both sides: its `.bin` buffer is missing.

What the front end does that the old path did not, each pinned by
`frontend_cgltf_test`:
- `cgltf_validate` runs before anything is read (hostile input).
- a non-indexed primitive is imported, not skipped
- a primitive with no material gets an appended default, not the file's first
  material
- a `data:` URI image is decoded, not skipped
- a primitive with no normals gets smooth normals (Assimp's behaviour for FBX)
  rather than a constant +Y

### 7.3 The Assimp formats switched over, and what that showed (WO-013)

`imp::AssimpFrontend` (`src/assets/import/frontend_assimp.*`) replaced the static
and skinned Assimp cook paths. `mesh_cooker.cpp` is now 76 lines of dispatch,
and audit IMP-01 keeps Assimp out of everything but the front end.

**Compared before deleting, on eleven real files.** The tracked plants, cottage,
TV, cannon and two dagger clips, plus fps_shooter's car, a Medieval prop, the
Warzombie character and a zombie clip. Both plant models are **byte-identical**.
On every skinned model the skeleton archive, bones, bind rotations, clips and
weights are identical. Every other difference is explained:

| difference | files | cause |
|---|---|---|
| tangent `w` | every skinned model | the old skinned path forced `w = +1`; the source's handedness is kept (§7.1) |
| weights on 139 vertices | cannon | **vertices with no bone influence** used to keep weight 0 and collapse to the origin under skinning. They now follow the root bone, and the cook says so |
| 3 tangents | cottage | were **zero-length** `(0,0,0)` in the old output; now a valid fallback |
| 6 tangent `w` | cottage | triangles whose three corners share one UV point: handedness is undefined there |
| base-colour texture now found | TV, cannon, car | **BUG-0063**: the old cooked basename could never resolve |
| normal maps not cooked | TV, cannon, car | EXR, which stb cannot decode; the old path stored a name nothing could load either |
| no texture on either side | Medieval prop | absolute `C:\Users\…` author paths, and the files are not beside the model: now reported |

**Proven with a fixture before switching** (the order required it):
`cooked_texture_resolution_test` cooks a textured OBJ, loads it through the real
`AssetService`, and asserts the texture is bound. It was red on the old path for
**two stacked bugs**: BUG-0063 (a bare basename that cannot resolve from the
cooked directory) and BUG-0064 (a single-submesh mesh bound to the file's
material 0, which for an OBJ is Assimp's untextured default).

**Found by the contract suite**, which runs on COLLADA files written from each
case:
- **Assimp invents a mesh for a file that has none**: a "skeleton mesh"
  drawing the hierarchy. An empty file imported as one mesh, and an
  animation-only COLLADA would have imported as a stick figure.
  `AI_CONFIG_IMPORT_NO_SKELETON_MESHES` is now set.
- **The vendored Assimp cannot read a COLLADA `<morph>`**:
  `ColladaParser.cpp`'s `<targets>` loop walks the controller's children, so
  every COLLADA file with a morph target fails to import. The suite skips
  `Unrepresentable` for Assimp, with that reason, and checks colours and
  cameras separately. A patch in `third_party/patches/` is future work.
- **Assimp names a COLLADA clip from the `<animation>` holding its channels**,
  and misreads `<library_animation_clips>`.

**Two contract rules were wrong for real files, and were corrected rather than
worked around:**
- `inverseBind` need only be invertible. Real FBX binds skins in poses other
  than the rest pose, and only the file's own matrix skins correctly.
- The suite compares skeletons allowing extra non-deforming ancestor bones
  (where FBX's unit and axis conversion lives), and compares clips by where
  they **move points**, not by local keys, which are in the file's frames.

**Units are unchanged.** FBX arrives in the units it was authored in, as it
always has. Converting to metres would rescale every project's FBX: WO-035.

### 7.4 glTF skins and clips (WO-014)

The cgltf front end now reads skins and animations, and WO-002's refusal is
gone. It follows the glTF spec:
- **the skeleton** is the skins' joints plus their ancestors
- **a skinned mesh's node transform is ignored**; the mesh hangs from a node
  holding the skin's bind space
- **`JOINTS_0`** is remapped from the skin's list to bones
- **clips** are translation, rotation and scale channels on bones. Cubic-spline
  keeps its keyed values, and the loss of its tangents is reported.

**The suite's `inverseBind` check is back, in the form that means something.**
WO-013 relaxed "inverseBind is the inverse of the rest pose", which real FBX
breaks. That left nothing checking inverse binds at all. Now each skinned
vertex is skinned at rest (Σ weight × boneRestWorld × inverseBind × vertex),
and must land where the scene places it: exactly what an inverse bind is for,
in whatever frame a front end keeps it.

**Not moved into the back end:** vertex cache ordering. Assimp's
`ImproveCacheLocality` stays in the Assimp front end, so FBX output is
unchanged. glTF never had it. A back-end optimiser for every format is an
improvement, not part of this switch.

## 8. Open questions this design leaves

| # | question | decided in |
|---|---|---|
| 1 | FBX units/axes on the skinned path (§4) | **COLLADA passes** `AuthoredCentimetreZUp` (WO-013): the front end carries Assimp's root conversion through skeleton and skin. FBX is in centimetres by Assimp's design, and whether to convert is **WO-035** |
| 2 | ~~tangent generator: MikkTSpace or our own~~ **Our own for now (WO-011, §7.1)**; revisit on visible normal-map seams | done, pending a visual check |
| 3 | ~~morph targets: `Wrong` or `Less`?~~ **Decided (WO-010): `Less`.** A mesh drawn at its base shape is correct, just without its expressions, like a static prop whose node animation is dropped. The `Unrepresentable` case pins it. | done |
| 4 | vertex colours, extra UV sets: `Less` today; carried when a shader reads them | when a shader needs them |
| 5 | whether the Assimp front end keeps its memory permit (`AssimpGatePass`) or the cook pipeline owns it | WO-013 |
