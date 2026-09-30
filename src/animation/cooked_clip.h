#pragma once
// ── cooked_clip — a standalone clip, cooked once for any skeleton (WO-016) ─────
//
// What the clip cooker writes for an animation-only source file (a Mixamo clip
// FBX), and what ClipLibrary reads. It holds the clip's KEYS BY BONE NAME, not
// an ozz Animation: an ozz Animation is compiled against one skeleton's joint
// order, and the cooker cannot know which character a clip will play on. So
// the cook is skeleton-independent, and binding (anim::bindRawClip, by name)
// happens at load, in the same code every clip is built with.
//
// Before WO-016 a clip was cooked only when the editor first bound it, into a
// cache keyed by the TARGET skeleton; a shipped build then failed with "clip
// not cooked" for any clip nobody had played in the editor.
//
// Layout, little-endian:
//   u32 magic 'CLIP', u32 version
//   u64 digest       FNV-1a (assetlib::blobDigest) over every byte after it
//   u32 trackCount,  then per track: u16 length + that many bytes of bone name
//   u64 archiveSize, then an ozz archive of one offline::RawAnimation whose
//                    tracks[i] animates the bone named trackBones[i]
//
// The ozz archive is a third-party deserializer that trusts its input
// (cooked_skin.h, BUG-0045/0046), so nothing reaches it unverified: the digest
// is checked first, the read goes through a GuardedStream, and the decoded
// animation must match the names and pass RawAnimation::Validate.
#include "animation/animation_clip.h"
#include "animation/cooked_skin.h"   // GuardedStream
#include "animation/skeleton.h"

#include <assetlib/clip_asset.h>      // magic, version, packaged name (the format's identity)
#include <assetlib/mesh_asset.h>      // assetlib::blobDigest

#include <ozz/animation/offline/raw_animation.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace anim {

struct CookedClip {
    std::vector<std::string>             trackBones;   // trackBones[i] is animated by keys.tracks[i]
    ozz::animation::offline::RawAnimation keys;        // name, duration (s), keys in the source's convention
};

using assetlib::kCookedClipMagic;
using assetlib::kCookedClipVersion;
using assetlib::isCookedClip;
using assetlib::packagedClipFileName;

inline std::vector<uint8_t> encodeCookedClip(const CookedClip& c) {
    std::vector<uint8_t> body;
    auto put = [&](const void* p, size_t n) { const auto* b = static_cast<const uint8_t*>(p); body.insert(body.end(), b, b + n); };
    const uint32_t tracks = (uint32_t)c.trackBones.size();
    put(&tracks, 4);
    for (const std::string& n : c.trackBones) {
        const uint16_t len = (uint16_t)std::min<size_t>(n.size(), 0xFFFF);
        put(&len, 2); put(n.data(), len);
    }
    ozz::io::MemoryStream ms;
    { ozz::io::OArchive ar(&ms); ar << c.keys; }
    std::vector<uint8_t> archive((size_t)ms.Size());
    ms.Seek(0, ozz::io::Stream::kSet);
    ms.Read(archive.data(), archive.size());
    const uint64_t archiveSize = archive.size();
    put(&archiveSize, 8); put(archive.data(), archive.size());

    std::vector<uint8_t> out(16);
    const uint64_t digest = assetlib::blobDigest(body);
    std::memcpy(out.data(), &kCookedClipMagic, 4);
    std::memcpy(out.data() + 4, &kCookedClipVersion, 4);
    std::memcpy(out.data() + 8, &digest, 8);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Refuses, with a reason, anything that is not exactly a clip this writes.
inline bool decodeCookedClip(const uint8_t* data, size_t size, CookedClip& out, std::string& why) {
    if (size < 16 || !isCookedClip(data, size)) { why = "not a cooked clip"; return false; }
    uint32_t version = 0; uint64_t digest = 0;
    std::memcpy(&version, data + 4, 4);
    std::memcpy(&digest, data + 8, 8);
    if (version != kCookedClipVersion) { why = "cooked clip version " + std::to_string(version) + ", want " + std::to_string(kCookedClipVersion); return false; }
    const uint8_t* p = data + 16;
    const uint8_t* end = data + size;
    if (assetlib::blobDigest(p, (size_t)(end - p)) != digest) { why = "cooked clip digest mismatch (corrupt or truncated)"; return false; }

    auto take = [&](void* dst, size_t n) { if ((size_t)(end - p) < n) return false; std::memcpy(dst, p, n); p += n; return true; };
    uint32_t tracks = 0;
    if (!take(&tracks, 4) || tracks > (size_t)(end - p) / 2) { why = "cooked clip track count out of range"; return false; }
    out.trackBones.clear();
    out.trackBones.reserve(tracks);
    for (uint32_t i = 0; i < tracks; ++i) {
        uint16_t len = 0;
        if (!take(&len, 2) || (size_t)(end - p) < len) { why = "cooked clip bone name runs past the end"; return false; }
        out.trackBones.emplace_back(reinterpret_cast<const char*>(p), len);
        p += len;
    }
    uint64_t archiveSize = 0;
    if (!take(&archiveSize, 8) || archiveSize != (uint64_t)(end - p)) { why = "cooked clip archive size does not match the file"; return false; }

    GuardedStream gs(std::vector<uint8_t>(p, end));
    ozz::io::IArchive ar(&gs);
    if (!ar.TestTag<ozz::animation::offline::RawAnimation>()) { why = "cooked clip holds no raw animation"; return false; }
    ar >> out.keys;
    if (gs.shortRead()) { why = "cooked clip archive is truncated"; return false; }
    if (out.keys.tracks.size() != tracks) { why = "cooked clip has " + std::to_string(out.keys.tracks.size()) + " tracks for " + std::to_string(tracks) + " bone names"; return false; }
    if (!out.keys.Validate()) { why = "cooked clip keys are invalid (times out of order or past the duration)"; return false; }
    return true;
}

inline bool readCookedClipFile(const std::filesystem::path& path, CookedClip& out, std::string& why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { why = "cannot open " + path.string(); return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decodeCookedClip(bytes.data(), bytes.size(), out, why);
}

}  // namespace anim
