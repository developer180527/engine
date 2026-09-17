#include "assetlib/texture_asset.h"
#include <cstdio>
#include <cstring>

namespace assetlib {

bool saveTexture(const TextureAsset& tex, const std::filesystem::path& outPath) {
    FILE* f = std::fopen(outPath.string().c_str(), "wb");
    if (!f) return false;
    std::fwrite(&tex.header, sizeof(TextureHeader), 1, f);
    std::fwrite(tex.pixels.data(), 1, tex.pixels.size(), f);
    std::fclose(f);
    return true;
}

bool loadTexture(TextureAsset& out, const std::filesystem::path& inPath) {
    FILE* f = std::fopen(inPath.string().c_str(), "rb");
    if (!f) return false;
    TextureHeader h;
    if (std::fread(&h, sizeof(TextureHeader), 1, f) != 1 ||
        h.magic != 0x54455820) {
        std::fclose(f);
        return false;
    }
    out.header = h;
    if (out.header.mipCount == 0) out.header.mipCount = 1;   // v1 pad byte

    // ── Colour space: forced legacy below v3, refused if unknown at v3+ ──────
    // A v2 writer zero-initialised these bytes, but "the pad happened to be zero"
    // is a property of one writer, not of the format — so anything older than v3
    // is legacy by version, not by content. At v3+ a value this build does not
    // know is refused like an unknown format id: guessing a colour space renders
    // a texture wrong with nothing in the log to say why.
    if (h.version < 3) {
        out.header.colourSpace = kTexColourLegacy;
    } else if (h.colourSpace >= kTexColourCount) {
        std::printf("[TextureAsset] %s: unknown colour space %u — refusing. "
                    "Cooked by a newer engine?\n",
                    inPath.string().c_str(), (unsigned)h.colourSpace);
        std::fclose(f);
        return false;
    }

    // v1 / raw RGBA8: exact pixel size. v2 BC blocks + mips: the payload is
    // everything after the header — sized by seek, zero CPU parsing.
    size_t payload;
    if (h.version <= 1 || h.format == kTexRGBA8) {
        payload = (size_t)h.width * h.height * h.channels;
    } else {
        std::fseek(f, 0, SEEK_END);
        payload = (size_t)std::ftell(f) - sizeof(TextureHeader);
        std::fseek(f, (long)sizeof(TextureHeader), SEEK_SET);
    }
    out.pixels.resize(payload);
    const size_t got = std::fread(out.pixels.data(), 1, payload, f);
    std::fclose(f);
    return got == payload;
}

} // namespace assetlib
