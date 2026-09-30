#pragma once
// ── clip_asset — the cooked standalone clip's identity (WO-016) ────────────────
//
// What a cooked clip IS, for code that must recognise or place one without
// decoding it: its magic, and the file name it has in a package. The payload
// (keys by bone name, an ozz RawAnimation archive) is encoded and decoded by
// the animation module (src/animation/cooked_clip.h), which owns the ozz side.
// Here, beside the other cooked formats, so packaging (src/tools) can find and
// name cooked clips without depending on animation.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace assetlib {

inline constexpr uint32_t kCookedClipMagic   = 0x50494C43;   // 'CLIP'
inline constexpr uint32_t kCookedClipVersion = 1;

// Enough bytes to tell a cooked clip from any other cooked output: a mesh and
// a clip share the pipeline's .cache/meshs/ directory (an animation-only model
// file cooks to a clip), so the magic is how they are told apart.
inline bool isCookedClip(const uint8_t* data, size_t size) {
    uint32_t magic = 0;
    if (size < 4) return false;
    std::memcpy(&magic, data, 4);
    return magic == kCookedClipMagic;
}

// The file name a cooked clip has in a PACKAGE (<dist>/.cache/anim/), from its
// project-relative source path, separators normalised. A dist has no asset
// registry, and a cooked scene names a clip by that source path, so the runtime
// finds it by this name alone (ClipLibrary; engine_build writes it).
inline std::string packagedClipFileName(const std::string& projectRelativePath) {
    const std::string key = std::filesystem::path(projectRelativePath).generic_string();
    uint64_t h = 1469598103934665603ull;
    for (char c : key) h = (h ^ (uint64_t)(uint8_t)c) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.clip", (unsigned long long)h);
    return name;
}

}  // namespace assetlib
