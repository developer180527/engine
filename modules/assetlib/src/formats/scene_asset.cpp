#include "assetlib/scene_asset.h"
#include <cstdio>
#include <fstream>

namespace assetlib {

bool saveScene(const SceneAsset& scene, const std::filesystem::path& outPath) {
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Cannot write: %s\n", outPath.string().c_str());
        return false;
    }
    SceneHeader hdr   = scene.header;
    hdr.magic         = kSceneMagic;
    hdr.version       = kSceneVersion;
    hdr.payloadSize   = scene.payload.size();
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(SceneHeader));
    if (!scene.payload.empty())
        f.write(reinterpret_cast<const char*>(scene.payload.data()),
                static_cast<std::streamsize>(scene.payload.size()));
    if (!f) {
        std::fprintf(stderr, "[SceneAsset] Write error: %s\n", outPath.string().c_str());
        return false;
    }
    return true;
}

bool loadScene(SceneAsset& out, const std::filesystem::path& inPath, std::string* why) {
    auto refuse = [&](std::string reason) {
        std::fprintf(stderr, "[SceneAsset] %s: %s\n", reason.c_str(), inPath.string().c_str());
        if (why) *why = std::move(reason);
        return false;
    };
    std::error_code ec;
    const auto fileSizeRaw = std::filesystem::file_size(inPath, ec);
    if (ec) return refuse("cannot stat");
    const uint64_t fileSize = static_cast<uint64_t>(fileSizeRaw);

    std::ifstream f(inPath, std::ios::binary);
    if (!f) return refuse("cannot open");
    if (fileSize < sizeof(SceneHeader)) return refuse("truncated (no header)");

    f.read(reinterpret_cast<char*>(&out.header), sizeof(SceneHeader));
    if (!f || out.header.magic != kSceneMagic) return refuse("bad magic");
    if (out.header.version != kSceneVersion)
        return refuse("cooked scene format " + std::to_string(out.header.version)
                      + ", this engine reads " + std::to_string(kSceneVersion)
                      + " (re-cook the scene)");
    if (out.header.encoding != (uint32_t)ScenePayload::Cbor
        && out.header.encoding != (uint32_t)ScenePayload::Packed)
        return refuse("unknown payload encoding " + std::to_string(out.header.encoding));

    // The header must account for every byte, no more and no fewer: a payload
    // shorter than declared is a truncated file, a longer one a corrupt header,
    // and only after this check is the declared size safe to allocate from.
    if (out.header.payloadSize != fileSize - sizeof(SceneHeader))
        return refuse("header declares " + std::to_string(out.header.payloadSize)
                      + " B of payload but the file holds "
                      + std::to_string(fileSize - sizeof(SceneHeader)));

    out.payload.resize(static_cast<size_t>(out.header.payloadSize));
    if (!out.payload.empty())
        f.read(reinterpret_cast<char*>(out.payload.data()),
               static_cast<std::streamsize>(out.payload.size()));
    // The size check proved the bytes are there, so a short read here means the
    // file changed underneath us; eof() is not accepted.
    if (!f.good()) return refuse("short read");
    return true;
}

}  // namespace assetlib
