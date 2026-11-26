#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace assetlib {

// ═══════════════════════════════════════════════════════════════════════════
// Cooked scene — "SCNE" (0x53434E45)
//
// A 32-byte header and a payload. Version 4 (WO-091): the payload is the scene
// in the engine's own saved form (what EntitySerde writes for a .scene, with mesh
// references resolved by the cooker), CBOR-encoded. assetlib does not interpret
// it: the engine reads it through its schema registry, so every component a scene
// can hold reaches the shipped player, kits' and unknown ones included.
//
// Version 3 and earlier were a fixed 256-byte record per entity with slots for
// nine components; nothing else survived the cook. They are refused at load by
// version, and re-cooked: a cooked scene is derived data, the .scene is the truth.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t kSceneMagic   = 0x53434E45;  // "SCNE"
static constexpr uint32_t kSceneVersion = 4;           // v4: the registry's form, CBOR

enum class ScenePayload : uint32_t {
    Cbor   = 1,   // RFC 8949, as nlohmann::json::to_cbor writes it
    // WO-092: registered components packed by their schemas (a schema table,
    // then each entity's packed records and, as CBOR, whatever was not packed).
    // The engine's src/assets/cooked_scene.h describes it.
    Packed = 2,
};

struct SceneHeader {
    uint32_t magic       = kSceneMagic;
    uint32_t version     = kSceneVersion;
    uint32_t encoding    = (uint32_t)ScenePayload::Cbor;
    uint32_t reserved0   = 0;
    uint64_t payloadSize = 0;    // bytes after the header; the file is exactly this long
    uint8_t  _pad[8]     = {};
};
static_assert(sizeof(SceneHeader) == 32, "SceneHeader size changed");

struct SceneAsset {
    SceneHeader          header;
    std::vector<uint8_t> payload;
};

bool saveScene(const SceneAsset& scene, const std::filesystem::path& outPath);

// Refuses, with the reason in `why` when it is non-null: a file that cannot be
// read, a bad magic, another version (an older cook, to be re-cooked), an
// encoding it does not know, or a size that disagrees with the header.
bool loadScene(SceneAsset& out, const std::filesystem::path& inPath,
               std::string* why = nullptr);

}  // namespace assetlib
