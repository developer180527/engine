#include "assetlib/scene_asset.h"
#include <fstream>
#include <cstdio>

namespace assetlib {

bool saveScene(const SceneAsset& scene, const std::filesystem::path& outPath) {
    std::filesystem::create_directories(outPath.parent_path());
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Cannot write: %s\n",
                     outPath.string().c_str());
        return false;
    }

    // Write header
    SceneHeader hdr = scene.header;
    hdr.magic           = kSceneMagic;
    hdr.version         = kSceneVersion;
    hdr.entityCount     = static_cast<uint32_t>(scene.entities.size());
    hdr.stringTableSize = static_cast<uint32_t>(scene.stringTable.size());
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(SceneHeader));

    // Write entity records (contiguous fixed-size block)
    if (!scene.entities.empty()) {
        f.write(reinterpret_cast<const char*>(scene.entities.data()),
                static_cast<std::streamsize>(
                    scene.entities.size() * sizeof(SceneEntity)));
    }

    // Write string table
    if (!scene.stringTable.empty()) {
        f.write(scene.stringTable.data(),
                static_cast<std::streamsize>(scene.stringTable.size()));
    }

    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Write error: %s\n",
                     outPath.string().c_str());
        return false;
    }

    std::printf("[SceneAsset] Saved %s  entities=%u strings=%u bytes\n",
                outPath.filename().string().c_str(),
                hdr.entityCount, hdr.stringTableSize);
    return true;
}

bool loadScene(SceneAsset& out, const std::filesystem::path& inPath) {
    std::ifstream f(inPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Cannot open: %s\n",
                     inPath.string().c_str());
        return false;
    }

    // Read header
    f.read(reinterpret_cast<char*>(&out.header), sizeof(SceneHeader));
    if (!f || out.header.magic != kSceneMagic) {
        std::fprintf(stderr, "[SceneAsset] Bad magic: %s\n",
                     inPath.string().c_str());
        return false;
    }
    if (out.header.version != kSceneVersion) {
        std::fprintf(stderr, "[SceneAsset] Unsupported version %u: %s\n",
                     out.header.version, inPath.string().c_str());
        return false;
    }

    // Read entity records
    out.entities.resize(out.header.entityCount);
    if (out.header.entityCount > 0) {
        f.read(reinterpret_cast<char*>(out.entities.data()),
               static_cast<std::streamsize>(
                   out.header.entityCount * sizeof(SceneEntity)));
    }

    // Read string table
    out.stringTable.resize(out.header.stringTableSize);
    if (out.header.stringTableSize > 0) {
        f.read(out.stringTable.data(),
               static_cast<std::streamsize>(out.header.stringTableSize));
    }

    return f.good() || f.eof();
}

} // namespace assetlib
