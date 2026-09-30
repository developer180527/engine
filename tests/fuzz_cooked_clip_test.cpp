// ── fuzz_cooked_clip_test — the cooked standalone clip (WO-016) ───────────────
//
// anim::decodeCookedClip reads a file the runtime found on disk: in a shipped
// build, a package; in the editor, a cook output or a DDC blob from another
// machine. It is two parsers: the engine's own header and bone-name table, and
// then an ozz archive of a RawAnimation, a third-party deserializer that trusts
// its input (cooked_skin.h). The threat model is fuzz_cooked_skin_test's: the
// digest covers accidental damage (a partial write, a bad disk), which rewrites
// the bytes and not the digest; a forger who rewrites both is out of scope.
//
// Two modes per case, on a real clip encoded as the cooker encodes it:
//   A. damage anywhere, digest left as written: refused, and never decoded
//   B. damage only the engine's own name table, digest RECOMPUTED, so the
//      engine's bounds checks are what stands between the bytes and a read
//      past the end (the digest cannot help here, by construction)
// Properties: no crash, no read out of bounds (run under ASan), a refusal
// always says why, and anything that decodes binds to a skeleton and samples.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "animation/cooked_clip.h"
#include "animation/ozz_bridge.h"
#include "fuzz/fuzz.h"

#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

namespace {

constexpr uint32_t kGeneratorVersion = 1;

Skeleton flatSkeleton(int bones) {
    Skeleton s;
    s.bones.resize((size_t)bones);
    for (int i = 0; i < bones; ++i) { s.bones[(size_t)i].name = "b" + std::to_string(i); s.bones[(size_t)i].parentIndex = i ? 0 : -1; }
    s.buildBoneMap();
    anim::buildOzzSkeleton(s);
    return s;
}

// A valid clip: random tracks naming bones (some that no skeleton has), each
// with sorted keys inside the duration.
anim::CookedClip makeClip(fuzz::Rng& rng) {
    anim::CookedClip c;
    c.keys.name = "fuzz";
    c.keys.duration = 0.1f + (float)rng.below(50) / 10.0f;
    const uint32_t tracks = rng.below(12);
    c.keys.tracks.resize(tracks);
    for (uint32_t t = 0; t < tracks; ++t) {
        c.trackBones.push_back(rng.chance(80) ? "b" + std::to_string(rng.below(16)) : "stray" + std::to_string(t));
        const uint32_t keys = rng.below(6);
        for (uint32_t k = 0; k < keys; ++k) {
            const float time = c.keys.duration * (float)k / (float)(keys ? keys : 1);
            c.keys.tracks[t].translations.push_back({time, {(float)k, 0, 0}});
            c.keys.tracks[t].rotations.push_back({time, {0, 0, 0, 1}});
        }
    }
    return c;
}

void damage(fuzz::Rng& rng, std::vector<uint8_t>& b, size_t from, size_t to) {
    if (to <= from) return;
    const int edits = (int)rng.range(1, 4);
    for (int e = 0; e < edits; ++e) {
        const size_t at = from + rng.below((uint32_t)(to - from));
        switch (rng.below(4)) {
        case 0: b[at] ^= (uint8_t)(1u << rng.below(8)); break;
        case 1: b[at] = (uint8_t)rng.interestingU32(); break;
        case 2: b.resize(at); return;                                        // truncate
        default: { const uint32_t v = rng.interestingU32(); std::memcpy(&b[at], &v, std::min<size_t>(4, b.size() - at)); }
        }
    }
}

void oneCase(uint64_t seed, fuzz::Report& rep) {
    fuzz::Rng rng(seed);
    const fuzz::ReproKey key{seed, kGeneratorVersion, "cooked_clip"};
    const anim::CookedClip clip = makeClip(rng);
    const std::vector<uint8_t> good = anim::encodeCookedClip(clip);

    anim::CookedClip out; std::string why;
    if (!anim::decodeCookedClip(good.data(), good.size(), out, why)) { rep.fail(key, "a clean clip was refused: " + why); return; }
    if (out.trackBones != clip.trackBones) { rep.fail(key, "a clean clip's bone names did not round-trip"); return; }

    // Where the engine's own table ends and the ozz archive begins.
    size_t tableEnd = 20;
    for (const std::string& n : clip.trackBones) tableEnd += 2 + n.size();

    std::vector<uint8_t> bad = good;
    const bool modeB = rng.chance(50);
    if (modeB) {
        damage(rng, bad, 16, std::min(tableEnd + 8, bad.size()));          // names, counts, archive size
        if (bad.size() >= 16) {
            const uint64_t d = assetlib::blobDigest(bad.data() + 16, bad.size() - 16);
            std::memcpy(bad.data() + 8, &d, 8);
        }
    } else {
        damage(rng, bad, 0, bad.size());
    }
    if (bad == good) return;

    const bool ok = anim::decodeCookedClip(bad.data(), bad.size(), out, why);
    if (!ok) {
        if (why.empty()) rep.fail(key, "a refusal gave no reason");
        if (!modeB && bad.size() == good.size() && std::memcmp(bad.data() + 8, good.data() + 8, 8) == 0 &&
            why.find("digest") == std::string::npos && why.find("magic") == std::string::npos &&
            why.find("not a cooked clip") == std::string::npos && why.find("version") == std::string::npos)
            rep.fail(key, "damage past the header was not caught by the digest: " + why);
        return;
    }
    // It decoded (mode B left something consistent): it must bind and sample.
    const Skeleton skel = flatSkeleton(16);
    const AnimClip a = anim::bindRawClip(out.trackBones, out.keys, skel);
    if (a.valid()) {
        ozz::animation::SamplingJob::Context ctx(a.ozz->num_tracks());
        std::vector<ozz::math::SoaTransform> local((size_t)skel.ozz->num_soa_joints());
        ozz::animation::SamplingJob job;
        job.animation = a.ozz.get(); job.context = &ctx; job.ratio = 0.5f; job.output = ozz::make_span(local);
        if (!job.Run()) rep.fail(key, "a decoded clip bound but would not sample");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return fuzz::run("cooked_clip", argc, argv, oneCase);
}
