// ── fuzz_mesh_loader_test — cooked-mesh deserializer ─────────────────────────
// assetlib::loadMesh parses a .cooked binary whose header declares counts and
// strides that the payload must actually satisfy. `src/assets/issues.md`
// records what happens when it doesn't: a truncated file whose header still
// claims the full index count fed the GPU an under-allocated buffer and caused
// out-of-bounds reads at draw time, and an indexStride of 0/1/3 was silently
// reinterpreted as 16-bit, scrambling geometry.
//
// This is the ideal fuzz target: pure CPU, no GPU, no allocation of engine
// singletons, microseconds per case — and it sits on the boundary where
// untrusted bytes (a DDC blob from another machine, a partially written cook,
// a corrupt disk) become structured data.
//
// Properties asserted per case:
//   1. Never crashes, never hangs, never reads out of bounds (run this lane
//      under ASan/UBSan for the last one to mean anything).
//   2. INTERNAL CONSISTENCY on success: if loadMesh returns true, every
//      declared count matches the bytes actually present. A caller that
//      trusts the header must not be able to walk off the end.
//   3. Round-trip: a mesh written by saveMesh always loads back, with the
//      payload intact. Rejecting our own valid output would be a silent
//      content-loss bug.
#include "fuzz/fuzz.h"

#include <assetlib/mesh_asset.h>

#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static constexpr uint32_t kGeneratorVersion = 1;

namespace {

// Build a structurally plausible mesh, then optionally corrupt it. Starting
// from valid-ish data is the whole point: random bytes fail the magic check in
// the first four bytes and never reach the code that has the bugs.
assetlib::MeshAsset buildPlausible(fuzz::Rng& rng) {
    using namespace assetlib;
    MeshAsset m;

    // A flag combination the cooker could actually emit.
    uint32_t flags = VF_POSITION;
    if (rng.chance(80)) flags |= VF_NORMAL;
    if (rng.chance(60)) flags |= VF_UV0;
    if (rng.chance(25)) flags |= VF_TANGENT;
    if (rng.chance(15)) flags |= VF_COLOR;
    if (rng.chance(20)) flags |= (VF_JOINTS | VF_WEIGHTS);

    m.header.vertexFlags  = flags;
    m.header.vertexStride = vertexStride(flags);
    m.header.vertexCount  = rng.range(0, 400);
    m.header.indexStride  = rng.chance(70) ? 4 : 2;
    m.header.indexCount   = rng.range(0, 900);
    m.header.version      = rng.chance(85) ? 2 : rng.range(0, 5);

    m.vertexData.resize((size_t)m.header.vertexCount * m.header.vertexStride);
    m.indexData.resize((size_t)m.header.indexCount * m.header.indexStride);
    for (auto& b : m.vertexData) b = rng.byte();
    for (auto& b : m.indexData)  b = rng.byte();

    const uint32_t subs = rng.chance(60) ? rng.range(0, 4) : 0;
    for (uint32_t i = 0; i < subs; ++i) {
        MeshSubmesh s{};
        s.indexOffset   = rng.below(m.header.indexCount ? m.header.indexCount : 1);
        s.indexCount    = rng.below(m.header.indexCount ? m.header.indexCount : 1);
        s.materialIndex = rng.below(4);
        m.submeshes.push_back(s);
    }
    m.header.submeshCount = (uint32_t)m.submeshes.size();

    const uint32_t mats = rng.chance(50) ? rng.range(0, 3) : 0;
    for (uint32_t i = 0; i < mats; ++i) {
        CookedMaterial c{};
        c.roughness = 0.5f;
        // Exercise the fixed-size char arrays, including the un-terminated case.
        const uint32_t n = rng.range(0, rng.chance(10) ? 511 : 32);
        for (uint32_t k = 0; k < n; ++k) c.baseColorPath[k] = (char)rng.range(32, 126);
        if (rng.chance(10)) std::memset(c.normalMapPath, 'x', sizeof(c.normalMapPath));
        m.materials.push_back(c);
    }
    m.header.materialCount = (uint32_t)m.materials.size();
    return m;
}

// Corruptions that mirror real failure modes: truncation (interrupted write,
// partial download), header/payload disagreement, and the stride values the
// audit called out.
enum class Corruption {
    None, Truncate, InflateVertexCount, InflateIndexCount, BadIndexStride,
    BadVertexStride, InflateSubmeshCount, InflateMaterialCount, BadMagic,
    HugeCounts, BitFlips, Empty, HeaderOnly, COUNT
};

void writeCorrupted(const fs::path& p, const assetlib::MeshAsset& src,
                    Corruption c, fuzz::Rng& rng) {
    using namespace assetlib;
    MeshAsset m = src;

    switch (c) {
        case Corruption::InflateVertexCount:
            m.header.vertexCount = rng.interestingU32(); break;
        case Corruption::InflateIndexCount:
            m.header.indexCount = rng.interestingU32(); break;
        case Corruption::BadIndexStride:
            m.header.indexStride = rng.chance(60) ? rng.below(4)   // 0,1,2,3
                                                  : rng.interestingU32(); break;
        case Corruption::BadVertexStride:
            m.header.vertexStride = rng.chance(60) ? rng.below(8)
                                                   : rng.interestingU32(); break;
        case Corruption::InflateSubmeshCount:
            m.header.submeshCount = rng.interestingU32(); break;
        case Corruption::InflateMaterialCount:
            m.header.materialCount = rng.interestingU32(); break;
        case Corruption::HugeCounts:
            m.header.vertexCount = 0xFFFFFFFFu;
            m.header.indexCount  = 0xFFFFFFFFu;
            m.header.vertexStride = 0xFFFFFFFFu; break;
        default: break;
    }

    // Serialize through the real writer, then damage the bytes on disk — this
    // keeps the layout honest instead of hand-rolling a second format encoder
    // that could drift from saveMesh().
    if (!saveMesh(m, p)) return;

    if (c == Corruption::BadMagic || c == Corruption::BitFlips ||
        c == Corruption::Truncate || c == Corruption::Empty ||
        c == Corruption::HeaderOnly) {
        std::vector<uint8_t> bytes;
        { std::ifstream in(p, std::ios::binary);
          bytes.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()); }
        if (c == Corruption::BadMagic && bytes.size() >= 4) {
            for (int i = 0; i < 4; ++i) bytes[i] = rng.byte();
        } else if (c == Corruption::Truncate && !bytes.empty()) {
            bytes.resize(rng.below((uint32_t)bytes.size()));
        } else if (c == Corruption::HeaderOnly) {
            bytes.resize(bytes.size() < sizeof(MeshHeader)
                         ? bytes.size() : sizeof(MeshHeader));
        } else if (c == Corruption::Empty) {
            bytes.clear();
        } else if (c == Corruption::BitFlips) {
            const uint32_t flips = rng.range(1, 24);
            for (uint32_t i = 0; i < flips && !bytes.empty(); ++i)
                bytes[rng.below((uint32_t)bytes.size())] ^= (uint8_t)(1u << rng.below(8));
        }
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    using namespace assetlib;
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "mesh_loader";

    fuzz::Rng shapeRng(fuzz::deriveSeed(masterSeed, "mesh_shape"));
    fuzz::Rng corruptRng(fuzz::deriveSeed(masterSeed, "mesh_corrupt"));

    fuzz::Scratch scratch("meshload");
    const fs::path file = scratch.path() / "fuzz.cooked";

    const MeshAsset original = buildPlausible(shapeRng);
    const auto corruption = (Corruption)corruptRng.below((uint32_t)Corruption::COUNT);
    writeCorrupted(file, original, corruption, corruptRng);

    // The call under test.
    MeshAsset loaded;
    const bool ok = loadMesh(loaded, file);

    if (!ok) return;    // a clean rejection is always an acceptable outcome

    // ── Property 2: a successful load must be internally consistent ──────────
    const size_t needVerts = (size_t)loaded.header.vertexCount
                           * (size_t)loaded.header.vertexStride;
    const size_t needIdx   = (size_t)loaded.header.indexCount
                           * (size_t)loaded.header.indexStride;

    if (loaded.vertexData.size() != needVerts)
        rep.fail(key, "accepted a mesh whose vertexData ("
                 + std::to_string(loaded.vertexData.size()) + " B) != vertexCount*"
                 "stride (" + std::to_string(needVerts) + " B) — a caller "
                 "trusting the header reads out of bounds");
    else if (loaded.indexData.size() != needIdx)
        rep.fail(key, "accepted a mesh whose indexData ("
                 + std::to_string(loaded.indexData.size()) + " B) != indexCount*"
                 "stride (" + std::to_string(needIdx) + " B)");
    else if (loaded.header.indexCount > 0 &&
             loaded.header.indexStride != 2 && loaded.header.indexStride != 4)
        rep.fail(key, "accepted indexStride=" + std::to_string(loaded.header.indexStride)
                 + " — only 2 or 4 are meaningful; anything else is silently "
                   "reinterpreted and scrambles geometry");
    else if (loaded.submeshes.size() != loaded.header.submeshCount)
        rep.fail(key, "submesh vector/count disagree");
    else if (loaded.materials.size() != loaded.header.materialCount)
        rep.fail(key, "material vector/count disagree");
    else {
        // Submesh ranges must lie inside the index buffer, or the renderer
        // draws past the end of the buffer it was handed.
        for (const auto& s : loaded.submeshes) {
            const uint64_t end = (uint64_t)s.indexOffset + s.indexCount;
            if (end > loaded.header.indexCount) {
                rep.fail(key, "accepted a submesh range ["
                         + std::to_string(s.indexOffset) + ","
                         + std::to_string(end) + ") outside indexCount "
                         + std::to_string(loaded.header.indexCount));
                break;
            }
        }
    }
}

// ── Property 3: round-trip of uncorrupted output ─────────────────────────────
// Runs on every case with a fresh scratch file, so it costs nothing extra and
// guards the far more damaging direction: silently refusing valid content.
void roundTripCase(uint64_t masterSeed, fuzz::Report& rep) {
    using namespace assetlib;
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "mesh_loader/roundtrip";

    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "mesh_roundtrip"));
    fuzz::Scratch scratch("meshrt");
    const fs::path file = scratch.path() / "rt.cooked";

    MeshAsset m = buildPlausible(rng);
    m.header.version = 2;                       // a current, valid file
    if (m.header.indexStride != 2 && m.header.indexStride != 4)
        m.header.indexStride = 4;
    // Keep submesh ranges legal — we are testing the format, not the cooker.
    for (auto& s : m.submeshes) {
        if (m.header.indexCount == 0) { s.indexOffset = 0; s.indexCount = 0; continue; }
        s.indexOffset = s.indexOffset % m.header.indexCount;
        s.indexCount  = m.header.indexCount - s.indexOffset;
    }
    m.vertexData.resize((size_t)m.header.vertexCount * m.header.vertexStride);
    m.indexData.resize((size_t)m.header.indexCount * m.header.indexStride);

    if (!saveMesh(m, file)) { rep.fail(key, "saveMesh failed on valid input"); return; }

    MeshAsset back;
    if (!loadMesh(back, file)) {
        rep.fail(key, "loadMesh REJECTED output written by saveMesh — valid "
                      "cooked content would be treated as missing");
        return;
    }
    if (back.vertexData != m.vertexData || back.indexData != m.indexData)
        rep.fail(key, "round-trip changed the payload bytes");
    else if (back.header.vertexCount != m.header.vertexCount ||
             back.header.indexCount  != m.header.indexCount  ||
             back.header.vertexFlags != m.header.vertexFlags)
        rep.fail(key, "round-trip changed the header");
}

} // namespace

int main(int argc, char** argv) {
    return fuzz::run("mesh_loader", argc, argv,
                     [](uint64_t seed, fuzz::Report& rep) {
                         oneCase(seed, rep);
                         roundTripCase(seed, rep);
                     });
}
