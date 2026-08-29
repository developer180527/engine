// ── fuzz_cooked_skin_test — the ozz archive blobs inside a cooked mesh ───────
//
// `src/animation/info.md` declares `parses-external-input: true`, and until this
// file the subsystem had no fuzz target. `docs/plans/subsystem-audit.md` flagged
// it as the place most likely to be hiding something: nine dependents, no
// endurance lane, and ZERO ledger entries — which by the audit's own caveat
// measures attention rather than health, because nothing had ever aimed a lane
// at it.
//
// ── What the untrusted bytes actually are ───────────────────────────────────
// A cooked `.mesh` carries two OPAQUE THIRD-PARTY ARCHIVES:
//
//     asset.skeletonBlob   an ozz::animation::Skeleton  archive
//     asset.clips[i].blob  an ozz::animation::Animation archive
//
// `anim::decodeCookedSkeleton` / `decodeCookedClips` hand those bytes straight to
// `ozz::io::IArchive`. That is a third-party binary deserializer, running on
// AsyncLoader worker threads and on AssetService's cooked streaming path, fed
// from a file that may be a partially written cook, a DDC blob pulled from
// another machine, or a corrupt disk.
//
// `fuzz_mesh_loader_test` fuzzes the mesh CONTAINER — the header, counts and
// strides that `assetlib::loadMesh` parses. It stops at the blob: the payload is
// opaque to it and is never decoded. So the container is fuzzed and the two
// deserializers reading out of it are not.
//
// ── The specific shape being probed ─────────────────────────────────────────
// `cooked_skin.h` guards with `TestTag<T>()` and then does an UNCHECKED extract:
//
//     if (ar.TestTag<ozz::animation::Skeleton>()) {
//         auto sk = ozz::make_unique<ozz::animation::Skeleton>();
//         ar >> *sk;                      // <- no success test on the extract
//
// `TestTag` validates the tag and version at the front of the archive. It says
// nothing about the body behind it. So a blob with an intact header and a
// mangled interior is exactly the input that gets furthest into ozz, which is
// why the corruption modes below preserve the tag as often as they destroy it.
//
// ── Properties asserted per case ────────────────────────────────────────────
//   1. Never crashes, never hangs, never reads out of bounds (run under
//      ASan/UBSan for the last one to mean anything).
//   2. Bounded work: a corrupt length field must not become a huge allocation
//      or a long loop. Enforced by the harness timeout and by the size cap.
//   3. HONEST FAILURE: when decoding does not succeed, the result must be a
//      cleanly EMPTY one — `skel.ozz == nullptr`, or the clip simply absent —
//      never a half-built object a caller would treat as usable.
//   4. Round-trip: an UNCORRUPTED blob always decodes. Without this the target
//      would pass by rejecting everything, which is the failure mode of every
//      parser fuzzer that only asserts "did not crash".
#include <cstdio>
#include "fuzz/fuzz.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <assetlib/mesh_asset.h>

#include "animation/cooked_skin.h"
#include "animation/ozz_bridge.h"
#include "animation/skeleton.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/memory/unique_ptr.h>

// Bumped when the generator's shape changes: a stored repro seed only means
// something against the generator that produced it.
// 2: v6 blob digests. Cases now set the digest from the PRISTINE blob and then
// corrupt the bytes, which is the real-world model — the cooker writes blob and
// digest together, and a partial write or a bad disk damages the blob afterwards.
static constexpr uint32_t kGeneratorVersion = 2;

namespace {

// Serialize exactly the way mesh_cooker.cpp does, so the seed bytes are the
// bytes a real cook produces rather than a plausible imitation.
std::vector<uint8_t> drain(ozz::io::MemoryStream& ms) {
    const int size = ms.Tell();
    std::vector<uint8_t> out((size_t)(size > 0 ? size : 0));
    ms.Seek(0, ozz::io::Stream::kSet);
    if (!out.empty()) ms.Read(out.data(), out.size());
    return out;
}

// A flat skeleton: bone 0 is the root, everything else is its child. Same shape
// animator_system_test uses, for the same reason — it needs no Assimp and no
// asset files, so this target stays pure CPU and microseconds per case.
Skeleton makeSkeleton(int boneCount) {
    Skeleton s;
    s.bones.resize((size_t)boneCount);
    for (int i = 0; i < boneCount; ++i) {
        s.bones[(size_t)i].name        = "b" + std::to_string(i);
        s.bones[(size_t)i].parentIndex = (i == 0) ? -1 : 0;
    }
    s.buildBoneMap();
    anim::buildOzzSkeleton(s);
    return s;
}

std::vector<uint8_t> skeletonBlobOf(const Skeleton& s) {
    if (!s.ozz) return {};
    ozz::io::MemoryStream ms;
    { ozz::io::OArchive a(&ms); a << *s.ozz; }
    return drain(ms);
}

std::vector<uint8_t> clipBlobOf(const Skeleton& s, float duration) {
    if (!s.ozz) return {};
    ozz::animation::offline::RawAnimation raw;
    raw.duration = duration;
    raw.tracks.resize((size_t)s.ozz->num_joints());
    ozz::animation::offline::AnimationBuilder builder;
    ozz::unique_ptr<ozz::animation::Animation> built = builder(raw);
    if (!built) return {};
    ozz::io::MemoryStream ms;
    { ozz::io::OArchive a(&ms); a << *built; }
    return drain(ms);
}

// Build the CookedBone records that accompany the blob. decodeCookedSkeleton
// reads these directly (no ozz involved), so they are part of the input surface
// even though they are not archive bytes.
void fillBones(assetlib::MeshAsset& asset, const Skeleton& s, fuzz::Rng& rng,
               bool corruptBones) {
    asset.bones.clear();
    for (size_t i = 0; i < s.bones.size(); ++i) {
        assetlib::CookedBone cb{};
        // char[64], not std::string — truncated deliberately rather than
        // clamped silently, so an over-long bone name is visible in the data.
        std::snprintf(cb.name, sizeof(cb.name), "%s", s.bones[i].name.c_str());
        cb.parentIndex = s.bones[i].parentIndex;
        cb.bindPosition[0] = cb.bindPosition[1] = cb.bindPosition[2] = 0.0f;
        cb.bindRotation[3] = 1.0f;
        cb.bindScale[0] = cb.bindScale[1] = cb.bindScale[2] = 1.0f;
        for (int k = 0; k < 16; ++k) {
            cb.inverseBindMatrix[k] = (k % 5 == 0) ? 1.0f : 0.0f;
            cb.localBindMatrix[k]   = (k % 5 == 0) ? 1.0f : 0.0f;
        }
        if (corruptBones) {
            // A parentIndex pointing outside the array, or at itself, is the
            // shape a hand-edited or truncated cook produces. decodeCooked-
            // Skeleton copies these into Bone::parentIndex without checking.
            switch (rng.next() % 3) {
                case 0: cb.parentIndex = (int)(rng.next() % 100000); break;
                case 1: cb.parentIndex = (int)i; break;              // self
                case 2: cb.parentIndex = -(int)(rng.next() % 1000);  break;
            }
        }
        asset.bones.push_back(std::move(cb));
    }
}

// Corruption modes, chosen so the ARCHIVE TAG survives as often as it does not:
// a blob that fails TestTag is rejected in one line and never reaches the
// deserializer, so a generator that mostly destroys the header would spend its
// budget proving the cheap guard works.
void corrupt(std::vector<uint8_t>& b, fuzz::Rng& rng) {
    if (b.empty()) return;
    switch (rng.next() % 6) {
        case 0:   // truncate — the commonest real corruption (partial write)
            b.resize((size_t)(rng.next() % b.size()));
            break;
        case 1: { // flip bits AFTER the tag, so TestTag still passes
            const size_t skip = b.size() > 32 ? 32 : b.size() / 2;
            if (skip >= b.size()) break;
            const size_t n = 1 + rng.next() % 8;
            for (size_t i = 0; i < n; ++i) {
                const size_t at = skip + rng.next() % (b.size() - skip);
                b[at] ^= (uint8_t)(1u << (rng.next() % 8));
            }
            break;
        }
        case 2: { // splice a large value where a count might live
            const size_t skip = b.size() > 32 ? 32 : 0;
            if (skip + 4 > b.size()) break;
            const size_t at = skip + rng.next() % (b.size() - skip - 3);
            const uint32_t big = 0x7FFFFFFFu >> (rng.next() % 8);
            std::memcpy(b.data() + at, &big, 4);
            break;
        }
        case 3:   // destroy the tag outright — the guard SHOULD catch this
            for (size_t i = 0; i < b.size() && i < 8; ++i) b[i] ^= 0xFF;
            break;
        case 4:   // grow with garbage: a body longer than the header describes
            for (int i = 0, n = (int)(rng.next() % 64); i < n; ++i)
                b.push_back(rng.byte());
            break;
        case 5:   // zero a window
            if (b.size() > 8) {
                const size_t at = rng.next() % (b.size() - 4);
                const size_t n  = 1 + rng.next() % (b.size() - at);
                std::memset(b.data() + at, 0, n);
            }
            break;
    }
}

// ── Case 1: corrupt archives must fail cleanly ──────────────────────────────
void oneCase(uint64_t seed, fuzz::Report& rep) {
    fuzz::Rng rng(seed);
    const fuzz::ReproKey key{seed, kGeneratorVersion, "cooked_skin"};

    const int boneCount = 1 + (int)(rng.next() % 40);
    const Skeleton src  = makeSkeleton(boneCount);
    if (!src.ozz) { rep.fail(key, "ozz skeleton builder refused a flat skeleton"); return; }

    assetlib::MeshAsset asset;
    const bool corruptBones = (rng.next() % 4) == 0;
    fillBones(asset, src, rng, corruptBones);

    asset.skeletonBlob = skeletonBlobOf(src);
    if (asset.skeletonBlob.empty()) { rep.fail(key, "skeleton serialized to nothing"); return; }
    // Digest of the PRISTINE bytes, then damage them — the cooker writes both
    // together and the disk corrupts the blob afterwards. A generator that
    // digested the corrupted bytes would be modelling an attacker who can
    // rewrite both, which the format explicitly does not defend against.
    asset.skeletonBlobDigest = assetlib::blobDigest(asset.skeletonBlob);
    const std::vector<uint8_t> pristineSkel = asset.skeletonBlob;
    corrupt(asset.skeletonBlob, rng);
    const bool skelDamaged = (asset.skeletonBlob != pristineSkel);

    const int clipCount = (int)(rng.next() % 3);
    int damagedClips = 0;
    for (int i = 0; i < clipCount; ++i) {
        assetlib::CookedClipBlob cc{};
        cc.blob = clipBlobOf(src, 1.0f + (float)(rng.next() % 10));
        if (cc.blob.empty()) continue;
        cc.blobDigest = assetlib::blobDigest(cc.blob);
        const std::vector<uint8_t> pristineClip = cc.blob;
        corrupt(cc.blob, rng);
        if (cc.blob != pristineClip) ++damagedClips;
        cc.mappedTracks = (int32_t)(rng.next() % 200);
        cc.totalTracks  = (int32_t)(rng.next() % 200);
        asset.clips.push_back(std::move(cc));
    }

    // Property 1 and 2: this must return, without crashing, in bounded time.
    Skeleton out = anim::decodeCookedSkeleton(asset);

    // Property 3, the v6 rule: a blob whose digest no longer matches must NEVER
    // have reached ozz. This is the assertion that would have caught BUG-0046,
    // and unlike "did not crash" it cannot pass vacuously — the round-trip case
    // below proves clean blobs still decode.
    if (skelDamaged && out.ozz) {
        rep.fail(key, "a skeleton blob whose digest no longer matches was "
                      "DECODED — corrupt bytes reached ozz, which is the class "
                      "of input its own deserializer is documented not to "
                      "survive");
        return;
    }

    // Property 3: an honest result. If the archive did not yield an ozz
    // skeleton, the caller is told so by `ozz == nullptr` and decides the
    // fallback — but the bone-derived halves must still be self-consistent,
    // because a caller that sees bones may walk them regardless.
    if (out.ozz) {
        if (out.ozzJointOf.size() != out.bones.size()) {
            rep.fail(key, "decoded skeleton has " + std::to_string(out.bones.size())
                     + " bones but " + std::to_string(out.ozzJointOf.size())
                     + " joint mappings — a caller indexes one by the other");
            return;
        }
        const int joints = out.ozz->num_joints();
        for (size_t i = 0; i < out.ozzJointOf.size(); ++i) {
            if (out.ozzJointOf[i] < 0 || out.ozzJointOf[i] >= joints) {
                rep.fail(key, "ozzJointOf[" + std::to_string(i) + "] = "
                         + std::to_string(out.ozzJointOf[i]) + " is outside the "
                         + std::to_string(joints) + " joints ozz reports — this "
                         "indexes ozz's arrays during sampling");
                return;
            }
        }
    }

    // The clips path, same contract: every clip that survives must carry a
    // usable ozz animation, because AnimClip::ozz is dereferenced at sample
    // time with no further check.
    std::vector<AnimClip> clips = anim::decodeCookedClips(asset);
    if (damagedClips > 0 && clips.size() > (size_t)(clipCount - damagedClips)) {
        rep.fail(key, "a clip blob whose digest no longer matches was decoded: "
                 + std::to_string(clips.size()) + " clips came back from "
                 + std::to_string(clipCount) + " with " + std::to_string(damagedClips)
                 + " damaged");
        return;
    }
    for (size_t i = 0; i < clips.size(); ++i) {
        if (!clips[i].ozz) {
            rep.fail(key, "decodeCookedClips returned clip " + std::to_string(i)
                     + " with a null ozz animation — it is dereferenced at "
                     "sample time without a check");
            return;
        }
        if (!(clips[i].duration >= 0.0f)) {   // catches NaN too
            rep.fail(key, "clip " + std::to_string(i) + " has duration "
                     + std::to_string(clips[i].duration)
                     + "; sampling divides by it");
            return;
        }
    }
}

// ── Case 2: the control — clean blobs must always decode ────────────────────
// Without this the target passes by rejecting everything, which is how a parser
// fuzzer that only asserts "did not crash" ends up guarding nothing.
void roundTripCase(uint64_t seed, fuzz::Report& rep) {
    fuzz::Rng rng(seed);
    const fuzz::ReproKey key{seed, kGeneratorVersion, "cooked_skin_roundtrip"};

    const int boneCount = 1 + (int)(rng.next() % 40);
    const Skeleton src  = makeSkeleton(boneCount);
    if (!src.ozz) return;

    assetlib::MeshAsset asset;
    fillBones(asset, src, rng, /*corruptBones=*/false);
    asset.skeletonBlob       = skeletonBlobOf(src);
    asset.skeletonBlobDigest = assetlib::blobDigest(asset.skeletonBlob);

    assetlib::CookedClipBlob cc{};
    cc.blob = clipBlobOf(src, 2.0f);
    cc.blobDigest = assetlib::blobDigest(cc.blob);
    if (!cc.blob.empty()) asset.clips.push_back(std::move(cc));

    const Skeleton out = anim::decodeCookedSkeleton(asset);
    if (!out.ozz) {
        rep.fail(key, "an UNCORRUPTED skeleton archive failed to decode — the "
                      "corruption cases above would then be passing vacuously");
        return;
    }
    if (out.bones.size() != src.bones.size()) {
        rep.fail(key, "round-trip lost bones (" + std::to_string(out.bones.size())
                 + " of " + std::to_string(src.bones.size()) + ")");
        return;
    }
    // buildBoneMap() is what makes findBone() answer; the header records that
    // omitting it made every standalone clip bind zero tracks.
    if (boneCount > 0 && out.findBone(src.bones[0].name) < 0) {
        rep.fail(key, "the decoded skeleton's bone map is empty — findBone() "
                      "returns -1 and standalone clips bind 0 tracks");
        return;
    }
    if (!asset.clips.empty()) {
        const std::vector<AnimClip> clips = anim::decodeCookedClips(asset);
        if (clips.size() != 1 || !clips[0].ozz) {
            rep.fail(key, "an UNCORRUPTED clip archive failed to decode");
            return;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered, and a
    // test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return fuzz::run("cooked_skin", argc, argv,
                     [](uint64_t seed, fuzz::Report& rep) {
                         oneCase(seed, rep);
                         roundTripCase(seed, rep);
                     });
}
