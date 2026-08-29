#pragma once
// ── cooked_skin — decode a v3 cooked mesh's skinned payload ──────────────────
// Bones + the opaque ozz skeleton/clip archives embedded in a MeshAsset,
// decoded into runtime Skeleton/AnimClip objects. Pure CPU, no Assimp, no
// bgfx — safe on any worker thread and in shipping builds. Shared by the
// editor's AsyncLoader (async_loader/parse.cpp) and AssetService's cooked
// streaming path so both flows produce identical runtime data.
#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "core/logger.h"

#include <assetlib/mesh_asset.h>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/memory/unique_ptr.h>

#include <cstring>
#include <vector>

namespace anim {

// ── Why a guarded stream, and not just TestTag ───────────────────────────────
// ozz's IArchive is documented — in its own source — as trusting its input:
//
//     // Class type loading.
//     template <typename _Ty> void operator>>(_Ty& _ty) {
//       // Only uses tag validation for assertions, as reading cannot fail.
//       OZZ_IF_DEBUG(bool valid =) internal::Tagger<const _Ty>::Validate(*this);
//
// and every primitive array load discards the byte count it actually got:
//
//     OZZ_IF_DEBUG(size_t size =) _archive.LoadBinary(array, count * sizeof(T));
//     assert(size == count * sizeof(T));           // DEBUG ONLY
//
// `OZZ_IF_DEBUG` is empty in release, so in a shipping build a truncated or
// corrupt archive reads FEWER bytes than asked, the short read is discarded, the
// destination is left partially uninitialised, and ozz goes on to build a
// Skeleton or Animation out of it. No error, no log, no crash — on the
// AsyncLoader worker thread, from a file that may be a partial cook, a DDC blob
// from another machine, or a bad disk. In a debug build the same input aborts
// the process on ozz's assert instead.
//
// `TestTag` does not help: it validates the tag and version at the FRONT of the
// archive and says nothing about the body behind it. A blob with an intact
// header and a mangled interior is exactly the input that gets furthest in.
//
// So the engine guards the one seam it owns — the Stream. Every short read is
// recorded, and a decode that consumed less than it asked for is refused rather
// than returned half-built. Found by `tests/fuzz_cooked_skin_test.cpp` on its
// second case; repro `--seed 12660276661850738527`.
//
// RESIDUAL, stated because it is not zero: this catches the read side. A corrupt
// COUNT field large enough to make ozz allocate before it reads is still an
// allocation this cannot see, and the real fix for that is integrity the format
// carries itself. See BUG-0044.
class GuardedStream : public ozz::io::Stream {
public:
    explicit GuardedStream(const std::vector<uint8_t>& bytes) {
        if (!bytes.empty()) m_ms.Write(bytes.data(), bytes.size());
        m_ms.Seek(0, ozz::io::Stream::kSet);
    }
    bool shortRead() const { return m_short; }

    bool   opened() const override { return m_ms.opened(); }
    size_t Read(void* buffer, size_t size) override {
        const size_t got = m_ms.Read(buffer, size);
        if (got != size) m_short = true;
        return got;
    }
    size_t Write(const void* buffer, size_t size) override {
        return m_ms.Write(buffer, size);
    }
    int    Seek(int offset, Origin origin) override { return m_ms.Seek(offset, origin); }
    int    Tell() const override                    { return m_ms.Tell(); }
    size_t Size() const override                    { return m_ms.Size(); }

private:
    ozz::io::MemoryStream m_ms;
    bool                  m_short = false;
};

// Rebuild the runtime Skeleton (bones + ozz runtime skeleton + our-bone →
// ozz-joint mapping) from a cooked mesh. Returns a skeleton with .ozz null
// if the archive blob is unreadable — caller decides the fallback.
inline Skeleton decodeCookedSkeleton(const assetlib::MeshAsset& asset) {
    Skeleton skel;
    skel.bones.reserve(asset.bones.size());
    for (const auto& cb : asset.bones) {
        Bone b;
        b.name        = cb.name;
        b.parentIndex = cb.parentIndex;
        b.bindPosition = {cb.bindPosition[0], cb.bindPosition[1],
                          cb.bindPosition[2]};
        b.bindRotation = {cb.bindRotation[0], cb.bindRotation[1],
                          cb.bindRotation[2], cb.bindRotation[3]};
        b.bindScale    = {cb.bindScale[0], cb.bindScale[1], cb.bindScale[2]};
        std::memcpy(b.inverseBindMatrix, cb.inverseBindMatrix, 64);
        std::memcpy(b.localBindMatrix,   cb.localBindMatrix,   64);
        skel.bones.push_back(std::move(b));
    }

    // ── v6 integrity, checked BEFORE ozz sees a byte (BUG-0046) ────────────
    // GuardedStream catches a short read. It cannot catch a corrupt COUNT that
    // ozz reads successfully and then allocates from — that input never comes
    // up short. A deserializer documented as trusting its input cannot be made
    // safe from the outside, so the rule is that it only ever receives bytes
    // whose digest matched.
    //
    // A MISSING digest (0) is refused, not trusted. Trusting a blob precisely
    // because it is old enough not to carry a checksum is the hole this closes;
    // the MeshCooker version bump re-cooks anything going through the DDC.
    if (!asset.skeletonBlob.empty()) {
        const uint64_t want = asset.skeletonBlobDigest;
        if (want == 0) {
            LOG_ERROR("Anim", "cooked skeleton carries no integrity digest "
                      "(pre-v6 mesh) — re-cook the asset");
            skel.buildBoneMap();
            return skel;
        }
        if (assetlib::blobDigest(asset.skeletonBlob) != want) {
            LOG_ERROR("Anim", "cooked skeleton blob digest mismatch (%zu B) — "
                      "refusing to decode corrupt bytes",
                      asset.skeletonBlob.size());
            skel.buildBoneMap();
            return skel;
        }
    }

    // ozz skeleton from the opaque archive blob.
    {
        GuardedStream gs(asset.skeletonBlob);
        ozz::io::IArchive ar(&gs);
        if (ar.TestTag<ozz::animation::Skeleton>()) {
            auto sk = ozz::make_unique<ozz::animation::Skeleton>();
            ar >> *sk;
            if (gs.shortRead()) {
                LOG_ERROR("Anim", "cooked skeleton archive is truncated or "
                          "corrupt (%zu bytes) — refusing the partially read "
                          "skeleton", asset.skeletonBlob.size());
            } else {
                skel.ozz = std::shared_ptr<const ozz::animation::Skeleton>(
                    sk.release(), ozz::Deleter<ozz::animation::Skeleton>());
            }
        }
    }
    if (skel.ozz) {
        // our-bone -> ozz-joint mapping by name (cheap).
        const auto names = skel.ozz->joint_names();
        skel.ozzJointOf.assign(skel.bones.size(), 0);
        for (size_t i = 0; i < skel.bones.size(); ++i)
            for (int j = 0; j < (int)names.size(); ++j)
                if (skel.bones[i].name == names[j]) {
                    skel.ozzJointOf[i] = j;
                    break;
                }
    }
    // findBone() answers from boneMap — WITHOUT this, every name lookup
    // returns -1 and standalone clips bind 0 tracks ("wrong rig?"). The
    // old inline decode never built it either; cooked-clip cache hits
    // masked the bug until the first fresh bind against a cooked skeleton.
    skel.buildBoneMap();
    return skel;
}

// Deserialize every clip archive embedded in a cooked mesh.
inline std::vector<AnimClip> decodeCookedClips(const assetlib::MeshAsset& asset) {
    std::vector<AnimClip> out;
    for (const auto& cc : asset.clips) {
        // Same rule as the skeleton: verified bytes or none. See above.
        if (cc.blobDigest == 0) {
            LOG_ERROR("Anim", "cooked clip carries no integrity digest (pre-v6 "
                      "mesh) — re-cook the asset");
            continue;
        }
        if (assetlib::blobDigest(cc.blob) != cc.blobDigest) {
            LOG_ERROR("Anim", "cooked clip blob digest mismatch (%zu B) — "
                      "refusing to decode corrupt bytes", cc.blob.size());
            continue;
        }
        GuardedStream gs(cc.blob);
        ozz::io::IArchive ar(&gs);
        if (!ar.TestTag<ozz::animation::Animation>()) continue;
        auto a = ozz::make_unique<ozz::animation::Animation>();
        ar >> *a;
        if (gs.shortRead()) {
            LOG_ERROR("Anim", "cooked clip archive is truncated or corrupt "
                      "(%zu bytes) — skipping it", cc.blob.size());
            continue;
        }
        AnimClip clip;
        clip.name         = a->name();
        clip.duration     = a->duration();
        clip.mappedTracks = cc.mappedTracks;
        clip.totalTracks  = cc.totalTracks;
        clip.ozz = std::shared_ptr<const ozz::animation::Animation>(
            a.release(), ozz::Deleter<ozz::animation::Animation>());
        out.push_back(std::move(clip));
    }
    return out;
}

} // namespace anim
