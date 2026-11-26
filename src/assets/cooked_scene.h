#pragma once
// ── cooked_scene — the cooked scene's payload, both ways (WO-091) ─────────────
//
// A cooked scene (`.cooked` under scenes/) is assetlib's 32-byte header and a
// payload; the payload is the scene as EntitySerde saves it, CBOR-encoded. The
// cooker writes it, the shipped player's SceneService reads it through the schema
// registry, and the packager reads the mesh paths out of it. All three go through
// here, so there is one encoding and one set of checks.
//
// UNTRUSTED ON READ. A cooked file is build output, but it is also a file on a
// player's disk, so decode() refuses rather than crashes: CBOR that does not
// parse, a payload that is not a scene object, and nesting deeper than any scene
// has (nlohmann's CBOR reader recurses per level, so an unbounded depth is a
// stack overflow; the cap stops it at the level that would exceed it).
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <assetlib/scene_asset.h>
#include <functional>
#include <nlohmann/json.hpp>

#include "assets/packed_scene.h"

namespace cooked_scene {

// Deeper than any scene: entity > component > field > array > element is 5, and
// a migrated or extension component might add a few. Reflected kit components
// are flecs meta structs, which nest no deeper in practice.
inline constexpr std::size_t kMaxDepth = 64;

inline void encode(const nlohmann::json& scene, assetlib::SceneAsset& out) {
    out.header  = assetlib::SceneHeader{};
    out.payload = nlohmann::json::to_cbor(scene);
}

// Writes `scene` as a packed payload (packed_scene.h), or returns false with the
// reason. Supplied by the scene layer, which owns the schemas
// (EntitySerde::scenePacker()); a cook without one writes CBOR.
using Packer = std::function<bool(const nlohmann::json& scene, assetlib::SceneAsset& out,
                                  std::string& why)>;

namespace detail {
// The DOM builder, refusing to open a level past the cap.
class DepthLimitedSax : public nlohmann::detail::json_sax_dom_parser<nlohmann::json> {
    using Base = nlohmann::detail::json_sax_dom_parser<nlohmann::json>;
public:
    explicit DepthLimitedSax(nlohmann::json& r) : Base(r, /*allow_exceptions*/ false) {}
    bool start_object(std::size_t n) { return ++m_depth <= kMaxDepth && Base::start_object(n); }
    bool start_array(std::size_t n)  { return ++m_depth <= kMaxDepth && Base::start_array(n); }
    bool end_object() { --m_depth; return Base::end_object(); }
    bool end_array()  { --m_depth; return Base::end_array(); }
    bool tooDeep() const { return m_depth > kMaxDepth; }
private:
    std::size_t m_depth = 0;
};
}  // namespace detail

// The scene object, or false with the reason. On success `out` is an object
// whose "entities" is an array (possibly empty).
inline bool decode(const assetlib::SceneAsset& in, nlohmann::json& out, std::string& why) {
    out = nlohmann::json();
    detail::DepthLimitedSax sax(out);
    bool ok = false;
    try {
        ok = nlohmann::json::sax_parse(in.payload.begin(), in.payload.end(), &sax,
                                       nlohmann::json::input_format_t::cbor,
                                       /*strict*/ true);
    } catch (const std::exception& e) {
        why = std::string("payload does not decode: ") + e.what();
        return false;
    }
    if (!ok || sax.is_errored()) {
        why = sax.tooDeep() ? "payload nests deeper than " + std::to_string(kMaxDepth)
                                  + " levels"
                            : "payload is not valid CBOR";
        return false;
    }
    if (!out.is_object() || !out.contains("entities") || !out["entities"].is_array()) {
        why = "payload is not a scene (an object with an \"entities\" array)";
        return false;
    }
    return true;
}

// Every cooked mesh the scene names, each once, in the order first named: each
// entity's meshRenderer and its LOD levels. What a package must ship and what a
// preload fetches.
template <class Add>
inline void addEntityMeshes(const nlohmann::json& e, Add& add) {
    if (!e.is_object()) return;
    if (auto mr = e.find("meshRenderer"); mr != e.end()) add(*mr);
    if (auto lm = e.find("lodMesh"); lm != e.end() && lm->is_object())
        if (auto lv = lm->find("levels"); lv != lm->end() && lv->is_array())
            for (const nlohmann::json& level : *lv) add(level);
}

inline std::vector<std::string> meshPaths(const nlohmann::json& scene) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const nlohmann::json& obj) {
        if (!obj.is_object()) return;
        auto it = obj.find("cookedPath");
        if (it == obj.end() || !it->is_string()) return;
        const std::string p = it->get<std::string>();
        if (!p.empty() && seen.insert(p).second) out.push_back(p);
    };
    if (!scene.is_object() || !scene.contains("entities") || !scene["entities"].is_array())
        return out;
    for (const nlohmann::json& e : scene["entities"]) addEntityMeshes(e, add);
    return out;
}

// The same, from a cooked scene of either encoding. A packed scene carries its
// mesh references in each entity's rest (the mesh component has a codec of its
// own, so it is never packed). False, with the reason, for a payload that does
// not read.
inline bool meshPathsOf(const assetlib::SceneAsset& file, std::vector<std::string>& out,
                        std::string& why) {
    out.clear();
    if (file.header.encoding != (uint32_t)assetlib::ScenePayload::Packed) {
        nlohmann::json scene;
        if (!decode(file, scene, why)) return false;
        out = meshPaths(scene);
        return true;
    }
    packed::View view;
    if (!view.open(file.payload.data(), file.payload.size(), why)) return false;
    std::set<std::string> seen;
    auto add = [&](const nlohmann::json& obj) {
        if (!obj.is_object()) return;
        auto it = obj.find("cookedPath");
        if (it == obj.end() || !it->is_string()) return;
        const std::string p = it->get<std::string>();
        if (!p.empty() && seen.insert(p).second) out.push_back(p);
    };
    packed::Entity e;
    while (view.next(e)) {
        if (!e.rest) continue;
        assetlib::SceneAsset rest;
        rest.payload.assign(e.rest, e.rest + e.restSize);
        nlohmann::json obj;
        detail::DepthLimitedSax sax(obj);
        bool ok = false;
        try {
            ok = nlohmann::json::sax_parse(rest.payload.begin(), rest.payload.end(), &sax,
                                           nlohmann::json::input_format_t::cbor, true);
        } catch (const std::exception&) { ok = false; }
        if (!ok || sax.is_errored()) { why = "an entity's unpacked components do not decode"; return false; }
        addEntityMeshes(obj, add);
    }
    if (!view.complete()) { why = "the packed scene is truncated or corrupt"; return false; }
    return true;
}

}  // namespace cooked_scene
