---
status: unreviewed
---
# Animation Subsystem — Architecture & Build Plan

## 1. Design Principles

- **Bones are NOT Flecs entities.** A skeleton is a flat array inside the animation
  system. 100 animated characters with 80-bone skeletons = 8,000 bone evaluations
  per frame — these must be a contiguous cache-friendly array, not scattered across
  ECS archetype tables.
- **Reuse existing pipelines.** Assimp already parses `aiAnimation`, `aiBone`,
  `aiNode` hierarchies. The cooked format already defines `VF_JOINTS` and
  `VF_WEIGHTS` flags. The `Handle<Tag>` + dense-vector registry pattern is proven.
  We extend, not reinvent.
- **Artist workflow: create in DCC, play in engine.** Blender/Maya/Mixamo produce
  animations. The engine imports, blends, layers, and drives them at runtime via
  state machines the designer builds in the editor. No frame-by-frame authoring.
- **Each phase produces a visible milestone.** No invisible infrastructure marathons.

---

## 2. What Already Exists (Reuse Map)

| System | File(s) | What it gives us | What needs changing |
|--------|---------|-------------------|---------------------|
| **Assimp importer** | `io/assimp_importer.h/.cpp` | Parses `aiScene` with `mAnimations`, `mBones`, joint hierarchy from `mRootNode` | Extract bone data + skin weights alongside geometry |
| **MeshCooker** | `cookers/mesh_cooker.h/.cpp` | Bakes geometry to `.cooked` binary | Add `VF_JOINTS`/`VF_WEIGHTS` extraction; emit skeleton + clips as sidecar data |
| **Cooked format** | `assetlib/mesh_asset.h` | `VF_JOINTS` (uint8x4) and `VF_WEIGHTS` (float4) already defined in `VertexFlags` | Populate these flags when bones are present |
| **Vertex layout** | `render/vertex.h` | 48-byte `Vertex` struct + bgfx layout | Create a second `SkinnedVertex` layout (48 + 20 = 68 bytes) |
| **Handle system** | `core/handle.h` | `Handle<Tag>` template with dense registry | Add `SkeletonTag`, `AnimClipTag`, `AnimControllerTag` |
| **Asset registries** | `render/asset_registry.h` | Vector-backed add/get/remove with slot reuse | New `SkeletonRegistry`, `AnimClipRegistry` following same pattern |
| **Forward pipeline** | `render/forward_pipeline.h` | Program creation, uniform binding, draw submission | Add skinned program variant + `u_boneMatrices` uniform |
| **Shaders** | `shaders/vs_triangle.sc` | Standard vertex transform | New `vs_skinned.sc` with bone matrix palette application |
| **EntitySerde** | `io/entity_serializer.h` | Component save/load table — new components auto-get undo + scene save | Add `"animator"`, `"skinnedMesh"` descriptors |
| **Inspector** | `editor/inspector_panel/` | Per-component section pattern | New `animator_section.h` |
| **Asset browser** | `editor/asset_browser/actions.h` | Right-click context menu, file spawn | "New > Animation Controller" entry |
| **Cook pipeline** | `assetlib/cook_pipeline.h` | `ICooker` interface + background cook service | Register new `SkeletonCooker`, `AnimClipCooker` |
| **Render extraction** | `render/render_view.h` | `RenderItem` with model matrix, mesh, material pointers | Add bone palette pointer to `RenderItem` for skinned meshes |

---

## 3. New Asset Types

### 3.1 Skeleton

A shared asset, like Mesh. Many entities reference one skeleton.

```
File format: .skeleton (cooked binary)
Handle:      SkeletonHandle = Handle<SkeletonTag>
Registry:    SkeletonRegistry (vector-backed, same pattern as AssetRegistry)
```

```cpp
// src/animation/skeleton.h
struct Bone {
    std::string    name;
    int            parentIndex;     // -1 = root bone
    bx::Vec3       bindPosition;    // rest pose local translation
    bx::Quaternion bindRotation;    // rest pose local rotation
    bx::Vec3       bindScale;       // rest pose local scale
    float          inverseBindMatrix[16]; // mesh-space -> bone-space
};

struct Skeleton {
    std::vector<Bone>                    bones;  // topologically sorted
    std::unordered_map<std::string, int> boneMap; // name -> index

    int findBone(const std::string& name) const;
};
```

**Source:** Assimp's `aiMesh::mBones[]` provides inverse bind matrices and
vertex weights. The bone hierarchy comes from `aiNode` tree traversal — each
bone name matches a node name. Parent-child relationships come from the node
tree structure.

### 3.2 Animation Clip

A bag of channels, each targeting one bone property over time.

```
File format: .anim (cooked binary)
Handle:      AnimClipHandle = Handle<AnimClipTag>
Registry:    AnimClipRegistry (vector-backed)
```

```cpp
// src/animation/animation_clip.h
enum class AnimProperty : uint8_t { Translation, Rotation, Scale };
enum class AnimInterp   : uint8_t { Step, Linear, CubicSpline };

struct AnimChannel {
    int           boneIndex;   // resolved against skeleton at bind time
    AnimProperty  property;
    AnimInterp    interpolation;
    std::vector<float>         timestamps;
    std::vector<float>         values;     // vec3 or quat, packed flat
};

struct AnimClip {
    std::string              name;
    float                    duration;      // seconds
    float                    ticksPerSecond;
    std::vector<AnimChannel> channels;
    std::string              skeletonRef;   // which skeleton this was authored for
};
```

**Source:** Assimp's `aiAnimation::mChannels[]` — each `aiNodeAnim` has
position/rotation/scale keyframes per bone node. `mDuration` and
`mTicksPerSecond` give timing.

### 3.3 Animation Controller (State Machine) — Phase 5

A designer-authored asset defining states, transitions, blend trees.

```
File format: .animcontroller (JSON)
Handle:      AnimControllerHandle = Handle<AnimControllerTag>
```

This is Phase 5 — we don't build it until Phases 1-4 are proven.

---

## 4. New ECS Components

### 4.1 SkinnedMesh (extends rendering)

```cpp
// src/components/skinned_mesh.h
struct SkinnedMesh {
    SkeletonHandle skeleton;
    // Computed each frame by AnimatorSystem, consumed by renderer:
    std::vector<float> boneMatrices; // MAX_BONES * 16 floats (mat4 palette)
};
```

Exists alongside `MeshRenderer` on the same entity. The renderer checks for
`SkinnedMesh` — if present, uses the skinned shader program and uploads the
bone palette.

### 4.2 Animator (drives playback)

```cpp
// src/components/animator.h
struct Animator {
    AnimClipHandle currentClip;
    float          time        = 0.0f;
    float          speed       = 1.0f;
    bool           playing     = true;
    bool           looping     = true;
    // Phase 5: AnimControllerHandle controller;
};
```

---

## 5. Pipeline Modifications (Exact Diffs)

### 5.1 Vertex Format — New `SkinnedVertex`

**File:** `src/render/vertex.h`

```cpp
// Add alongside existing Vertex:
struct SkinnedVertex {
    float    position[3];     // 12 bytes
    float    normal[3];       // 12 bytes
    float    tangent[4];      // 16 bytes
    float    uv[2];           //  8 bytes
    uint8_t  joints[4];       //  4 bytes  (bone indices, max 256 bones)
    float    weights[4];      // 16 bytes  (blend weights, sum to 1.0)
    // Total: 68 bytes per vertex

    static bgfx::VertexLayout& layout() {
        static bgfx::VertexLayout layout = [] {
            bgfx::VertexLayout l;
            l.begin()
                .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Indices,   4, bgfx::AttribType::Uint8, true)
                .add(bgfx::Attrib::Weight,    4, bgfx::AttribType::Float)
                .end();
            return l;
        }();
        return layout;
    }
};
```

### 5.2 Handle Tags

**File:** `src/core/handle.h`

```cpp
// Add new tags:
struct SkeletonTag {};
struct AnimClipTag {};
struct AnimControllerTag {};

// Add aliases:
using SkeletonHandle        = core::Handle<core::SkeletonTag>;
using AnimClipHandle        = core::Handle<core::AnimClipTag>;
using AnimControllerHandle  = core::Handle<core::AnimControllerTag>;
```

### 5.3 Assimp Importer — Bone & Animation Extraction

**File:** `src/io/assimp_importer.cpp`

Currently `importMesh()` ignores `aiMesh::mBones` entirely. Modifications:

1. **Detect skinned meshes:** Check `aiMesh::mNumBones > 0`
2. **Extract bone weights per vertex:** Each `aiBone` has a list of
   `(vertexIndex, weight)` pairs. Build per-vertex `joints[4]` + `weights[4]`
   arrays (keep top 4 influences, normalize weights to sum to 1.0)
3. **Build skeleton from node tree:** Walk `aiScene::mRootNode`, match bone
   names to nodes, record parent-child relationships, extract local bind pose
   transforms
4. **Extract animation clips:** Iterate `aiScene::mAnimations[]`, convert each
   `aiNodeAnim` channel to our `AnimChannel` format
5. **Use `SkinnedVertex` layout** when bones are present, standard `Vertex`
   otherwise

### 5.4 MeshCooker — Skinned Mesh Cooking

**File:** `src/cookers/mesh_cooker.cpp`

1. **Detect bones:** If any `aiMesh` in the scene has `mNumBones > 0`, set
   `VF_JOINTS | VF_WEIGHTS` in `vertexFlags`
2. **Use extended CookVertex:** Add `uint8_t joints[4]` and `float weights[4]`
   fields when cooking skinned meshes
3. **Emit skeleton sidecar:** Write a `.skeleton` file alongside the
   `.cooked` mesh — same UUID prefix, different extension
4. **Emit animation clips:** Write one `.anim` file per `aiAnimation` found
   in the scene

The cook pipeline already supports one-source-many-outputs conceptually. The
MeshCooker becomes a "ModelCooker" that emits geometry + skeleton + clips.

### 5.5 Shader — Skinned Vertex Shader

**New file:** `shaders/vs_skinned.sc`

```glsl
$input a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight
$output v_worldPos, v_worldNormal, v_worldTangent, v_texcoord0
#include <bgfx_shader.sh>

// Bone palette — 128 bones max, each is a mat4 (4 vec4s = 512 vec4 uniforms)
uniform vec4 u_boneMatrices[512]; // 128 * 4

mat4 getBoneMatrix(int idx) {
    int base = idx * 4;
    return mat4(
        u_boneMatrices[base + 0],
        u_boneMatrices[base + 1],
        u_boneMatrices[base + 2],
        u_boneMatrices[base + 3]
    );
}

void main() {
    // Linear blend skinning (LBS)
    ivec4 idx = ivec4(a_indices * 255.0); // bgfx normalized -> int
    mat4 skin = getBoneMatrix(idx.x) * a_weight.x
              + getBoneMatrix(idx.y) * a_weight.y
              + getBoneMatrix(idx.z) * a_weight.z
              + getBoneMatrix(idx.w) * a_weight.w;

    vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));

    mat4 model = u_model[0];
    vec4 worldPos = mul(model, skinnedPos);
    v_worldPos    = worldPos.xyz;
    gl_Position   = mul(u_viewProj, worldPos);

    // Skin normals (using linear part of skin matrix)
    vec3 skinnedNormal  = mul(skin, vec4(a_normal, 0.0)).xyz;
    vec3 skinnedTangent = mul(skin, vec4(a_tangent.xyz, 0.0)).xyz;

    // Cofactor normal matrix (same as vs_triangle.sc)
    vec3 mc0 = model[0].xyz;
    vec3 mc1 = model[1].xyz;
    vec3 mc2 = model[2].xyz;
    vec3 nm0 = cross(mc1, mc2);
    vec3 nm1 = cross(mc2, mc0);
    vec3 nm2 = cross(mc0, mc1);
    v_worldNormal  = normalize(nm0*skinnedNormal.x + nm1*skinnedNormal.y + nm2*skinnedNormal.z);
    vec3 wtan = normalize(nm0*skinnedTangent.x + nm1*skinnedTangent.y + nm2*skinnedTangent.z);
    v_worldTangent = vec4(wtan, a_tangent.w);

    v_texcoord0 = a_texcoord0;
}
```

Same fragment shader (`fs_triangle.sc`) works for both skinned and static meshes.

### 5.6 Varying Definitions

**File:** `shaders/varying.def.sc`

```
// Add these two lines:
uvec4 a_indices  : BLENDINDICES;
vec4  a_weight   : BLENDWEIGHT;
```

### 5.7 Forward Pipeline — Skinned Program

**File:** `src/render/forward_pipeline.h`

```cpp
// In onAttach(): create a second program for skinned meshes
m_skinnedProgram = bgfx::createProgram(
    bgfx::createShader(bgfx::makeRef(VS_SKINNED_DATA, VS_SKINNED_SIZE)),
    bgfx::createShader(bgfx::makeRef(FS_TRIANGLE_DATA, FS_TRIANGLE_SIZE)),
    true);
m_uBoneMatrices = bgfx::createUniform("u_boneMatrices", bgfx::UniformType::Vec4, 512);

// In render(): check if RenderItem has bone data
if (it.boneMatrices) {
    bgfx::setUniform(m_uBoneMatrices, it.boneMatrices, 512);
    bgfx::submit(id, m_skinnedProgram);
} else {
    bgfx::submit(id, m_program);
}
```

### 5.8 RenderItem — Bone Palette Pointer

**File:** `src/render/render_view.h`

```cpp
struct RenderItem {
    Mat4            model;
    const Mesh*     mesh = nullptr;
    const Material* mat  = nullptr;
    const Texture*  tex  = nullptr;
    const float*    boneMatrices = nullptr;  // NEW: null = static mesh
    uint32_t        meshKey = 0;
    uint32_t        matKey  = 0;
};
```

### 5.9 EntitySerde — New Components

**File:** `src/io/entity_serializer.h`

Add to the `table()`:

```cpp
{"animator", has<Animator>, saveAnimator, loadAnimator},
{"skinnedMesh", has<SkinnedMesh>, saveSkinnedMesh, loadSkinnedMesh},
```

This gives undo/redo, scene save/load, and play-mode snapshot for free via
the existing UndoStack infrastructure.

---

## 6. Build Phases

### Phase 1 — Skeleton + Clip Data Structures + Pose Math

**Goal:** Pure data and math. Load a skinned .fbx, parse skeleton and clips,
sample a pose at time t. No rendering.

**New files:**
```
src/animation/
    skeleton.h              Bone, Skeleton structs
    animation_clip.h        AnimClip, AnimChannel structs
    pose.h                  Pose struct, sampleClip(), blendPoses()
    animation_math.h        slerp, cubic hermite helpers (if bx:: doesn't cover)
```

**Modified files:**
```
src/core/handle.h           Add SkeletonTag, AnimClipTag
```

**Assimp data we read (but don't import to GPU yet):**
- `aiMesh::mBones[i]->mName` — bone name
- `aiMesh::mBones[i]->mOffsetMatrix` — inverse bind matrix
- `aiMesh::mBones[i]->mWeights[]` — per-vertex influences
- `aiScene::mRootNode` tree — skeleton hierarchy (match names)
- `aiAnimation::mChannels[i]->mPositionKeys/mRotationKeys/mScalingKeys`
- `aiAnimation::mDuration`, `mTicksPerSecond`

**Milestone test:** Load `soldier.fbx`, assert skeleton has N bones, sample
walk clip at t=0.5s, verify root bone world position matches Blender.

**No rendering, no editor, no ECS integration yet.**

---

### Phase 2 — GPU Skinning (First Visual Result)

**Goal:** Skinned mesh renders in bind pose. Then with a hardcoded clip
playing, the mesh deforms.

**New files:**
```
src/render/skinned_vertex.h       SkinnedVertex struct + bgfx layout
src/animation/skeleton_registry.h Dense registry for Skeleton assets
src/animation/clip_registry.h     Dense registry for AnimClip assets
src/components/skinned_mesh.h     SkinnedMesh ECS component
shaders/vs_skinned.sc             Skinned vertex shader
```

**Modified files:**
```
src/io/assimp_importer.cpp    Extract bone weights + joints per vertex
                              Build Skeleton from aiNode tree
                              Extract AnimClip from aiAnimation
                              Use SkinnedVertex layout when bones present
src/render/render_view.h      Add boneMatrices to RenderItem
src/render/forward_pipeline.h Add skinned program + u_boneMatrices
shaders/varying.def.sc        Add a_indices, a_weight
src/io/asset_storage.h        Add skeleton + clip registries
src/engine_context.h          Add registries to EngineContext
```

**Milestone:** Drop a Mixamo character .fbx into the scene. It renders in
bind pose (T-pose). Manually set time=0.5 in a test — mesh deforms to match
the walk animation frame.

---

### Phase 3 — Animator Component + Playback System

**Goal:** Animated characters play clips in a loop. Inspector shows playback
controls.

**New files:**
```
src/components/animator.h               Animator ECS component
src/systems/animator_system.h           Flecs system: tick time, sample, skin
src/editor/inspector_panel/animator_section.h   Inspector UI
```

**Modified files:**
```
src/io/entity_serializer.h    Add "animator" + "skinnedMesh" to serde table
src/editor/inspector_panel/panel.h   Include animator_section.h
```

**Animator system per-frame logic:**
1. Query entities with `Animator` + `SkinnedMesh` + `MeshRenderer`
2. `animator.time += dt * animator.speed`
3. If looping: `time = fmod(time, clip.duration)`
4. `Pose localPose = sampleClip(clip, time)`
5. `mat4[] worldMatrices = computeWorldPose(skeleton, localPose)`
6. `mat4[] skinMatrices = worldMatrices[i] * inverseBindMatrix[i]`
7. Write into `skinnedMesh.boneMatrices`

**Render extraction reads `skinnedMesh.boneMatrices` and sets it on
`RenderItem::boneMatrices`.**

**Milestone:** Drag a Mixamo .fbx into the scene -> entity gets
`MeshRenderer` + `SkinnedMesh` + `Animator` auto-attached -> walk animation
plays in a loop in the viewport. Inspector shows clip name, time scrubber,
speed slider, play/pause. Undo works on all Animator properties.

---

### Phase 4 — Blending + Transitions

**Goal:** Smooth animation. Cross-fades, 1D blend trees, animation layers.

**New files:**
```
src/animation/cross_fade.h     Timed blend between two poses
src/animation/blend_tree.h     1D/2D parameter-driven blending
src/animation/anim_layer.h     Masked layer blending (upper/lower body)
src/animation/additive_clip.h  Additive pose = clip - reference pose
src/animation/bone_mask.h      Per-bone weight mask for layers
```

**Modified files:**
```
src/components/animator.h      Add blend state, layer support
src/systems/animator_system.h  Multi-clip evaluation, blend tree tick
```

**Key algorithms:**
- `blendPoses(a, b, alpha)`: per-bone lerp(position), slerp(rotation),
  lerp(scale)
- `crossFade`: blend two clip poses over a duration, switch when done
- `1D blend tree`: sort clips by threshold, find two neighbors for current
  parameter value, blend between them (walk at speed=2.0 blends 60% walk +
  40% run)
- `additive blend`: `result = base + alpha * (additive - reference)` — used
  for breathing overlays, hit reactions

**Milestone:** Character transitions smoothly from idle -> walk -> run based
on a float parameter. No popping. Upper body can play aim animation
independently.

---

### Phase 5 — Animation Controller (State Machine + Editor)

**Goal:** Visual state machine editor. Designer-driven animation logic.

**New files:**
```
src/animation/anim_controller.h         State machine: states, transitions, params
src/animation/anim_parameter.h          Float/Bool/Int/Trigger types
src/io/anim_controller_serde.h          JSON save/load for .animcontroller
src/editor/anim_graph_panel/            New dockable panel
    panel.h                             Node graph canvas
    state_node.h                        State rendering (box + clip name)
    transition_edge.h                   Arrow + condition editor
    parameter_sidebar.h                 Parameter list
```

**Modified files:**
```
src/core/handle.h                Add AnimControllerTag
src/editor/asset_browser/actions.h   "New > Animation Controller"
src/components/animator.h        Add AnimControllerHandle controller field
src/systems/animator_system.h    Evaluate state machine each frame
```

**State machine runtime loop (per frame):**
1. Evaluate all transition conditions against current parameter values
2. If a transition fires: begin cross-fade from current state to target state
3. During cross-fade: sample both clips, `blendPoses(a, b, fadeProgress)`
4. When fade completes: switch active state

**Milestone:** Right-click asset browser > New > Animation Controller.
Double-click opens graph panel. Create Idle/Walk/Run states, wire
transitions with speed conditions. Assign to entity. Character drives from
state machine.

---

### Phase 6 — Cooker Integration (Production Path)

**Goal:** Skeleton + clips cook to binary. Fast load replaces runtime Assimp.

**New files:**
```
src/cookers/skeleton_cooker.h/.cpp    Cook skeleton to binary .skeleton
src/cookers/anim_clip_cooker.h/.cpp   Cook clips to binary .anim
modules/assetlib/include/assetlib/skeleton_asset.h   Binary format
modules/assetlib/include/assetlib/anim_clip_asset.h  Binary format
```

**Modified files:**
```
src/cookers/mesh_cooker.cpp    Emit VF_JOINTS + VF_WEIGHTS when bones present
                               Write skeleton/clip sidecar files
src/io/cook_service.h          Register new cookers
```

**Cooked skeleton format (binary):**
```
Magic: 0x534B454C ("SKEL")
Header: boneCount, version
Per-bone: parentIndex(i32), inverseBindMatrix(16f), name(64 char)
```

**Cooked clip format (binary):**
```
Magic: 0x414E494D ("ANIM")
Header: channelCount, duration, ticksPerSecond, version
Per-channel: boneIndex(i32), property(u8), interp(u8), keyCount(u32)
Keyframe data: timestamps(f32[]), values(f32[])
```

**Milestone:** Drop .fbx into assets/ folder. Cook service produces
`.cooked` (with joints/weights), `.skeleton`, and `.anim` files in `.cache/`.
Runtime loads from cooked files — Assimp never runs in shipped builds.

---

### Phase 7 — Root Motion + IK

**Goal:** Characters move through the world via animation. Feet plant on
terrain.

**New files:**
```
src/animation/root_motion.h     Extract root bone delta, apply to Transform
src/animation/ik_solver.h       FABRIK (any chain length)
src/animation/two_bone_ik.h     Analytical (elbows, knees — fast)
src/animation/foot_ik.h         Ground adaptation: raycast -> adjust feet
src/animation/look_at_ik.h      Head/spine aim constraint
```

**Root motion flow:**
1. Sample root bone position at `t` and `t - dt`
2. Delta = `pos(t) - pos(t-dt)` in bone space
3. Transform delta to world space via entity orientation
4. Apply delta to entity `Transform.position`
5. Zero out root bone translation in the pose (so it doesn't double-move)

**Foot IK flow:**
1. After pose evaluation, get world position of each foot bone
2. Raycast down from each foot to find ground height
3. Compute required offset for each foot to reach ground
4. Apply two-bone IK to each leg chain (hip->knee->foot)
5. Offset pelvis by the largest required foot adjustment

**Milestone:** Character walks with root motion — feet don't slide. Stands
on a slope with foot IK — feet plant on the surface, knees bend correctly.

---

### Phase 8 — Motion Matching

**Goal:** Data-driven animation selection. No explicit state machine needed.

**New files:**
```
src/animation/pose_database.h     Precomputed features per frame
src/animation/trajectory.h        Desired future trajectory from input
src/animation/motion_matcher.h    Feature matching (KD-tree or brute force)
src/animation/database_baker.h    Offline: bake clip library -> feature DB
src/editor/motion_match_panel/    Debug visualization panel
```

**Feature vector per frame:**
- Joint positions (feet, hands) relative to root — 6-12 floats
- Joint velocities — 6-12 floats
- Future trajectory (2-3 points at 0.2s, 0.5s, 1.0s) — 6-9 floats
- Total: ~30-40 floats per frame, across all clips

**Runtime loop:**
1. Build desired trajectory from player input
2. Build current pose features from skeleton state
3. Concatenate into query vector
4. Search database for nearest neighbor (weighted L2 distance)
5. If best match is far from current playback position: transition
6. Cross-fade to new clip at the matched frame

**Milestone:** Character responds fluidly to input without an explicit state
machine. Direction changes, stops, and starts are all emergent from the
database. Editor debug panel visualizes candidates.

---

## 7. Assimp Data Extraction Reference

This is exactly what Assimp gives us and how it maps to our types:

```
aiScene
├── mMeshes[]
│   ├── mVertices[], mNormals[], mTextureCoords[]  → Vertex/SkinnedVertex
│   ├── mBones[]                                    → per-vertex joints/weights
│   │   ├── mName                                   → bone name (matches aiNode)
│   │   ├── mOffsetMatrix                           → Bone::inverseBindMatrix
│   │   └── mWeights[]                              → {vertexId, weight} pairs
│   └── mFaces[]                                    → index buffer
│
├── mRootNode (aiNode tree)                         → Skeleton bone hierarchy
│   ├── mName                                       → bone name matching
│   ├── mTransformation                             → Bone::bindPosition/Rotation/Scale
│   └── mChildren[]                                 → Bone::parentIndex
│
├── mAnimations[]                                   → AnimClip[]
│   ├── mName                                       → AnimClip::name
│   ├── mDuration                                   → AnimClip::duration
│   ├── mTicksPerSecond                             → AnimClip::ticksPerSecond
│   └── mChannels[] (aiNodeAnim)                    → AnimChannel[]
│       ├── mNodeName                               → resolve to bone index
│       ├── mPositionKeys[]                         → Translation channel
│       ├── mRotationKeys[]                         → Rotation channel
│       └── mScalingKeys[]                          → Scale channel
│
└── mMaterials[]                                    → (already handled)
```

**Bone weight extraction (per mesh):**
```
For each aiBone in aiMesh::mBones:
    boneIndex = skeleton.findBone(bone->mName)
    For each aiVertexWeight in bone->mWeights:
        vertexJoints[weight.mVertexId].add(boneIndex, weight.mWeight)

Per vertex, keep only top 4 influences, normalize weights to sum to 1.0.
```

---

## 8. File Inventory (All New Files)

```
src/animation/
    skeleton.h                  Bone, Skeleton (flat array + bone map)
    animation_clip.h            AnimClip, AnimChannel
    pose.h                      Pose, sampleClip, blendPoses, computeWorldPose
    animation_math.h            Interpolation utilities
    skeleton_registry.h         SkeletonRegistry (dense vector + slot reuse)
    clip_registry.h             AnimClipRegistry
    cross_fade.h                CrossFade state
    blend_tree.h                1D/2D blend tree
    anim_layer.h                Layer + bone mask blending
    additive_clip.h             Additive pose math
    bone_mask.h                 Per-bone weight mask
    anim_controller.h           State machine runtime
    anim_parameter.h            Parameter types
    root_motion.h               Root bone extraction
    ik_solver.h                 FABRIK
    two_bone_ik.h               Analytical 2-bone
    foot_ik.h                   Ground adaptation
    look_at_ik.h                Aim constraint
    pose_database.h             Motion matching DB
    trajectory.h                Input trajectory
    motion_matcher.h            NN search
    database_baker.h            Offline bake

src/components/
    skinned_mesh.h              SkinnedMesh component
    animator.h                  Animator component

src/systems/
    animator_system.h           Per-frame animation tick

src/render/
    skinned_vertex.h            SkinnedVertex layout

src/io/
    anim_controller_serde.h     JSON save/load

src/cookers/
    skeleton_cooker.h/.cpp      Binary skeleton cook
    anim_clip_cooker.h/.cpp     Binary clip cook

src/editor/
    inspector_panel/
        animator_section.h      Inspector UI
    anim_graph_panel/
        panel.h                 Graph canvas
        state_node.h            State node
        transition_edge.h       Transition edge
        parameter_sidebar.h     Params UI

modules/assetlib/include/assetlib/
    skeleton_asset.h            Binary format
    anim_clip_asset.h           Binary format

shaders/
    vs_skinned.sc               Skinned vertex shader
```

---

## 9. Known Constraints & Decisions

| Decision | Rationale |
|----------|-----------|
| Max 128 bones per skeleton | 128 * 4 = 512 vec4 uniforms fits in bgfx's uniform limit. Covers humanoids (65-90 bones) with headroom. Use SSBO if we ever need more. |
| 4 bone influences per vertex | Industry standard. 99%+ of artist-authored content uses <= 4. More is wasted bandwidth for invisible quality. |
| Separate skinned shader program | Avoids branching in the hot path. Static meshes use existing program, skinned meshes use the new one. Pipeline selects based on `boneMatrices != null`. |
| Skeleton is a shared asset | Like Mesh — 100 soldiers share one skeleton. Per-entity data is only the computed bone palette (in SkinnedMesh). |
| Animation clips stored separately from mesh | A skeleton can have dozens of clips from different sources (Mixamo, hand-animated, motion captured). One clip per file keeps the asset graph clean. |
| State machine is JSON, not binary | It's a design asset edited in the graph UI. Human-readable format matters for version control diffs. Tiny file size — no need for binary optimization. |
| Material edits on skinned meshes | Same MaterialRegistry path. Skinned meshes only change the vertex shader, not the fragment shader. Materials work identically. |
| cgltf path (glTF/GLB) | Needs equivalent skeleton/clip extraction. Same data structures, different parser. Handle in Phase 2 alongside Assimp. |

---

## 10. Start Here

**Phase 1 is pure code — no pipeline changes, no shader work, no editor UI.**

```
src/animation/skeleton.h
src/animation/animation_clip.h
src/animation/pose.h
src/core/handle.h (add tags)
```

Four files. The foundation everything else builds on.
