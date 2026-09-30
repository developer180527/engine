#pragma once
// ── ImportedScene — the engine's own import format (WO-010) ───────────────────
//
// What a source file MEANS, in the engine's conventions, owned by value. Front
// ends (one per parsing library) produce it; one back end turns it into cooked
// assets. Design, conventions and the reasons for each choice:
// docs/plans/imported-scene.md. The contract: docs/contracts/import-frontend.md.
//
// DEPENDENCY-FREE BY RULE (audit LAYER-05): this header, and every header in
// src/assets/import/ that is not a front end, includes the standard library and
// each other, nothing else. No Assimp, no cgltf, no GPU, no bx. Its own small
// POD math types are the price: a format that has to outlive every library
// behind it cannot borrow any of their types.
//
// ── Conventions every front end converts INTO (plan §4) ─────────────────────
//   right-handed, +Y up, metres; UV origin top-left; triangles only;
//   COUNTER-CLOCKWISE is the front face; matrices column-major with the
//   translation in m[12], m[13], m[14] (the memory layout of glTF, bx and the
//   engine's Mat4 alike).
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace imp {

struct Float2   { float x = 0, y = 0; };
struct Float3   { float x = 0, y = 0, z = 0; };
struct Float4   { float x = 0, y = 0, z = 0, w = 0; };
struct Quat     { float x = 0, y = 0, z = 0, w = 1; };
struct Float4x4 { float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; };

// ── Geometry ────────────────────────────────────────────────────────────────
struct Submesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;     // a multiple of 3
    uint32_t material   = 0;     // index into ImportedScene::materials
};

// One vertex per stream entry. positions and normals are always filled; every
// other stream is either EMPTY or one entry per vertex. An empty stream is a
// fact the back end acts on: no tangents means the back end generates them.
struct Mesh {
    std::string name;
    std::vector<Float3> positions;
    std::vector<Float3> normals;
    std::vector<Float4> tangents;                    // w = ±1, the bitangent's sign
    std::vector<Float2> uv0;
    std::vector<std::array<uint16_t, 4>> joints;     // into ImportedScene::skeleton
    std::vector<Float4> weights;                     // sum to 1; unused slots are 0
    std::vector<uint32_t> indices;                   // triangles, CCW front
    std::vector<Submesh> submeshes;                  // tile `indices`, in order

    bool skinned() const { return !joints.empty(); }
};

struct Node {
    std::string name;
    int32_t     parent = -1;             // parents come before children
    Float4x4    local;
    std::vector<uint32_t> meshes;        // instancing: a mesh may hang off many nodes
};

// ── Materials ───────────────────────────────────────────────────────────────
// Exactly one of `path` / `embedded`, or neither. Resolution is ONE rule (plan
// §4.1): a relative path is relative to the source file's directory; nothing is
// searched for. A reference that does not resolve is a Dropped entry instead.
struct TextureRef {
    std::string          path;
    std::vector<uint8_t> embedded;       // the encoded image bytes (PNG, JPEG, …)
    std::string          embeddedName;   // for dedup and messages

    bool empty() const { return path.empty() && embedded.empty(); }
};

struct Material {
    std::string name;
    Float4      baseColorFactor {1, 1, 1, 1};
    TextureRef  baseColor;               // sRGB by slot, decided by the back end
    TextureRef  normal;                  // linear by slot
};

// ── Animation ───────────────────────────────────────────────────────────────
struct Bone {
    std::string name;                    // unique; clips bind by name
    int32_t     parent = -1;             // parents come before children
    Float4x4    bindLocal;               // bind pose, relative to the parent bone
    Float4x4    inverseBind;             // inverse of the bone's bind WORLD matrix
};

struct Skeleton { std::vector<Bone> bones; };

struct KeyF3 { float time = 0; Float3 value; };
struct KeyQ  { float time = 0; Quat   value; };

struct Track {                           // one bone's channels; any may be empty
    std::string        bone;
    std::vector<KeyF3> translation;
    std::vector<KeyQ>  rotation;
    std::vector<KeyF3> scale;
};

struct Clip {
    std::string        name;             // unique within the scene
    float              duration = 0;     // seconds; every key time is in [0, duration]
    std::vector<Track> tracks;
};

// ── What the front end could not carry (plan §5) ───────────────────────────
// Every loss is reported. Leaving something out WITHOUT an entry here is a
// contract violation, and the contract suite looks for it.
struct Dropped {
    enum class Kind   { Skin, Animation, MorphTargets, VertexColours, ExtraUvSets,
                        NonTriangles, Texture, Camera, Light, Extension };
    // Wrong: the asset is incorrect without it, and the back end refuses to cook.
    // Less:  the asset is correct, just less; the back end cooks and logs it.
    enum class Effect { Wrong, Less };

    Kind        kind   = Kind::Extension;
    Effect      effect = Effect::Wrong;
    uint32_t    count  = 1;
    std::string what;                    // "3 morph targets on 'Face'", a texture path, …
};

// ── The scene ───────────────────────────────────────────────────────────────
struct ImportedScene {
    std::string             source;      // the path it came from, for messages
    std::vector<Node>       nodes;       // nodes[0] is the root
    std::vector<Mesh>       meshes;
    std::vector<Material>   materials;
    std::optional<Skeleton> skeleton;
    std::vector<Clip>       clips;
    std::vector<Dropped>    dropped;
};

inline const char* toString(Dropped::Kind k) {
    switch (k) {
        case Dropped::Kind::Skin:          return "skin";
        case Dropped::Kind::Animation:     return "animation";
        case Dropped::Kind::MorphTargets:  return "morph targets";
        case Dropped::Kind::VertexColours: return "vertex colours";
        case Dropped::Kind::ExtraUvSets:   return "extra UV sets";
        case Dropped::Kind::NonTriangles:  return "non-triangle primitives";
        case Dropped::Kind::Texture:       return "texture";
        case Dropped::Kind::Camera:        return "camera";
        case Dropped::Kind::Light:         return "light";
        case Dropped::Kind::Extension:     return "extension";
    }
    return "?";
}
inline const char* toString(Dropped::Effect e) {
    return e == Dropped::Effect::Wrong ? "wrong" : "less";
}

}  // namespace imp
