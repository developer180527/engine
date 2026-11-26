// ── fuzz_scene_loader_test — cooked-scene deserializer ───────────────────────
// A cooked scene is assetlib's 32-byte header and a payload whose size the
// header declares; since WO-091 (format 4) the payload is the scene itself,
// CBOR-encoded, decoded by `cooked_scene::decode` (src/assets/cooked_scene.h).
// Both halves read a file from a player's disk, so both are fuzzed here.
//
// Format 3 was a fixed record per entity with strings addressed by (offset,
// length); its three bugs (truncation accepted, allocation from the header, a
// 32-bit offset+length wrap) are pinned in the corpus, found under generator 1.
// The same three questions are asked of format 4: does a short or long file get
// through, can 32 bytes size an allocation, can the payload's own structure
// (CBOR lengths, nesting) do what the record's offsets did.
//
// Properties asserted per case:
//   1. Never crashes, hangs, or reads out of bounds (run under ASan/UBSan for the
//      last clause to mean anything). Includes nesting deep enough that an
//      unbounded recursive reader would overflow the stack.
//   2. BOUNDED WORK: what loadScene allocates is backed by bytes the file has.
//   3. CONSISTENCY on success: the payload is exactly what the header declared
//      and the file held, and a decoded payload is a scene object.
//   4. Round-trip: anything the cook writes loads and decodes to the same scene.
#include <cstdio>
#include "fuzz/fuzz.h"

#include <assetlib/scene_asset.h>
#include <nlohmann/json.hpp>

#include "assets/cooked_scene.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// 2: format 4 (WO-091). The seeds in the corpus were found under 1 (format 3).
static constexpr uint32_t kGeneratorVersion = 2;

namespace {

json junkValue(fuzz::Rng& rng, int depth = 0) {
    switch (depth > 3 ? rng.below(4) : rng.below(7)) {
        case 0:  return json(nullptr);
        case 1:  return json(rng.chance(50));
        case 2:  return json((int64_t)rng.interestingU32() - (rng.chance(30) ? 1ll << 32 : 0));
        case 3: {
            std::string s;
            const uint32_t n = rng.range(0, 30);
            for (uint32_t i = 0; i < n; ++i) s.push_back((char)rng.range(1, 255));
            return json(s);
        }
        case 4:  return json((double)rng.next() / 7.0);
        case 5: {
            json a = json::array();
            const uint32_t n = rng.range(0, 4);
            for (uint32_t i = 0; i < n; ++i) a.push_back(junkValue(rng, depth + 1));
            return a;
        }
        default: {
            json o = json::object();
            const uint32_t n = rng.range(0, 4);
            for (uint32_t i = 0; i < n; ++i) o["k" + std::to_string(i)] = junkValue(rng, depth + 1);
            return o;
        }
    }
}

// A scene of the shape the cook writes: entities with ids, parents and the
// engine's component keys, plus a kit's reflected block and unknown keys.
json buildPlausible(fuzz::Rng& rng) {
    json scene = { {"version", 1}, {"entities", json::array()} };
    const uint32_t n = rng.range(0, 40);
    for (uint32_t i = 0; i < n; ++i) {
        json e = json::object();
        e["id"] = (uint64_t)rng.next();
        if (rng.chance(30)) e["parentId"] = (uint64_t)rng.next();
        if (rng.chance(80)) e["name"] = "e" + std::to_string(i);
        if (rng.chance(90))
            e["transform"] = { {"position", {1.0, 2.0, 3.0}}, {"rotation", {0, 0, 0, 1}},
                               {"scale", {1, 1, 1}} };
        if (rng.chance(50))
            e["meshRenderer"] = { {"cookedPath", "meshs/m" + std::to_string(rng.below(8)) + ".cooked"},
                                  {"path", "assets/m.glb"} };
        if (rng.chance(10))
            e["lodMesh"] = { {"levels", json::array({ { {"cookedPath", "meshs/l1.cooked"} } })},
                             {"coarsenBelow", {0.3}} };
        if (rng.chance(20)) e["light"] = { {"type", 1}, {"intensity", 4.0} };
        if (rng.chance(10)) e["colourGrading"] = { {"exposureEV", -1.0}, {"lutPath", "l.cube"} };
        if (rng.chance(10)) e["reflected"] = { {"combat::Health", { {"current", 3} }} };
        if (rng.chance(15)) e["k" + std::to_string(rng.below(5))] = junkValue(rng);
        scene["entities"].push_back(std::move(e));
    }
    return scene;
}

enum class Corruption : uint32_t {
    None, Truncate, Extend, HeaderByte, PayloadSize, Version, Encoding, PayloadBits,
    DeepNesting, LongLength, NotAScene, COUNT
};

void writeCorrupted(const fs::path& p, const json& scene, Corruption c, fuzz::Rng& rng) {
    assetlib::SceneAsset s;
    cooked_scene::encode(scene, s);
    switch (c) {
        case Corruption::DeepNesting: {
            // An array nested far past the cap: 0x81 is "array of 1 item". An
            // unbounded recursive reader overflows the stack on this.
            const uint32_t depth = rng.chance(50) ? rng.range(60, 70) : rng.range(1000, 200000);
            s.payload.assign(depth, 0x81);
            s.payload.push_back(0xF6);   // null
            break;
        }
        case Corruption::LongLength: {
            // A byte string (0x5B, 8-byte length) declaring far more than follows.
            s.payload = { 0x5B, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 'x' };
            break;
        }
        case Corruption::NotAScene:
            s.payload = json::to_cbor(rng.chance(50) ? junkValue(rng) : json{{"entities", 5}});
            break;
        default: break;
    }
    s.header.payloadSize = s.payload.size();
    std::vector<uint8_t> bytes(sizeof(assetlib::SceneHeader));
    std::memcpy(bytes.data(), &s.header, sizeof(assetlib::SceneHeader));
    bytes.insert(bytes.end(), s.payload.begin(), s.payload.end());

    switch (c) {
        case Corruption::Truncate:
            bytes.resize(rng.below((uint32_t)bytes.size() + 1)); break;
        case Corruption::Extend:
            for (uint32_t i = rng.range(1, 64); i > 0; --i) bytes.push_back((uint8_t)rng.next()); break;
        case Corruption::HeaderByte:
            bytes[rng.below(sizeof(assetlib::SceneHeader))] ^= (uint8_t)(1u << rng.below(8)); break;
        case Corruption::PayloadSize: {
            uint64_t v = rng.chance(50) ? (uint64_t)rng.interestingU32() : ~0ull - rng.below(16);
            std::memcpy(bytes.data() + offsetof(assetlib::SceneHeader, payloadSize), &v, 8);
            break;
        }
        case Corruption::Version: {
            const uint32_t v = rng.chance(50) ? 3u : rng.interestingU32();
            std::memcpy(bytes.data() + offsetof(assetlib::SceneHeader, version), &v, 4);
            break;
        }
        case Corruption::Encoding: {
            const uint32_t v = rng.interestingU32();
            std::memcpy(bytes.data() + offsetof(assetlib::SceneHeader, encoding), &v, 4);
            break;
        }
        case Corruption::PayloadBits:
            if (bytes.size() > sizeof(assetlib::SceneHeader))
                for (uint32_t i = rng.range(1, 8); i > 0; --i)
                    bytes[sizeof(assetlib::SceneHeader)
                          + rng.below((uint32_t)(bytes.size() - sizeof(assetlib::SceneHeader)))]
                        ^= (uint8_t)(1u << rng.below(8));
            break;
        default: break;
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "scene_loader";

    fuzz::Rng shapeRng(fuzz::deriveSeed(masterSeed, "scene_shape"));
    fuzz::Rng corruptRng(fuzz::deriveSeed(masterSeed, "scene_corrupt"));
    fuzz::Scratch scratch("sceneload");
    const fs::path file = scratch.path() / "fuzz.cooked";

    const json original = buildPlausible(shapeRng);
    const auto corruption = (Corruption)corruptRng.below((uint32_t)Corruption::COUNT);
    writeCorrupted(file, original, corruption, corruptRng);

    std::error_code ec;
    const uint64_t onDisk = (uint64_t)fs::file_size(file, ec);

    // ── The calls under test ────────────────────────────────────────────────
    assetlib::SceneAsset loaded;
    std::string why;
    const bool ok = assetlib::loadScene(loaded, file, &why);

    // ── Property 2: bounded work, whether it succeeded or not ───────────────
    if (loaded.payload.size() > onDisk) {
        rep.fail(key, "allocated a " + std::to_string(loaded.payload.size())
                 + " B payload from a " + std::to_string(onDisk) + " B file");
        return;
    }
    if (!ok) {
        if (why.empty()) rep.fail(key, "refused without a reason");
        return;   // a clean rejection with a reason is always acceptable
    }

    // ── Property 3: a successful load is consistent ─────────────────────────
    if (loaded.payload.size() != loaded.header.payloadSize
        || sizeof(assetlib::SceneHeader) + loaded.payload.size() != onDisk) {
        rep.fail(key, "accepted a scene whose payload (" + std::to_string(loaded.payload.size())
                 + " B) is not what the header declared ("
                 + std::to_string(loaded.header.payloadSize) + " B) and the file held ("
                 + std::to_string(onDisk) + " B)");
        return;
    }

    json scene;
    bool decoded = false;
    try {
        decoded = cooked_scene::decode(loaded, scene, why);
    } catch (const std::exception& e) {
        rep.fail(key, std::string("decode threw: ") + e.what());
        return;
    }
    if (!decoded) {
        if (why.empty()) rep.fail(key, "decode refused without a reason");
        return;
    }
    if (!scene.is_object() || !scene.contains("entities") || !scene["entities"].is_array()) {
        rep.fail(key, "decode accepted a payload that is not a scene");
        return;
    }
    // What the player does next with a decoded scene, and the packager too.
    (void)cooked_scene::meshPaths(scene);
    if (corruption == Corruption::None && scene != original)
        rep.fail(key, "an uncorrupted file decoded to a different scene");
}

// ── Property 4: round-trip ───────────────────────────────────────────────────
void roundTripCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "scene_loader/roundtrip";

    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "scene_roundtrip"));
    fuzz::Scratch scratch("sceneround");
    const fs::path file = scratch.path() / "rt.cooked";

    const json original = buildPlausible(rng);
    assetlib::SceneAsset out;
    cooked_scene::encode(original, out);
    if (!assetlib::saveScene(out, file)) {
        rep.fail(key, "saveScene failed on a scene it generated itself");
        return;
    }
    assetlib::SceneAsset back;
    json scene;
    std::string why;
    if (!assetlib::loadScene(back, file, &why) || !cooked_scene::decode(back, scene, why)) {
        rep.fail(key, "REJECTED valid cooked output (" + why + ") — silent content loss "
                      "for every scene of this shape");
        return;
    }
    if (scene != original) rep.fail(key, "round-trip changed the scene");
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return fuzz::run("scene_loader", argc, argv,
                     [](uint64_t seed, fuzz::Report& rep) {
                         oneCase(seed, rep);
                         roundTripCase(seed, rep);
                     });
}
