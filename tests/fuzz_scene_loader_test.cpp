// ── fuzz_scene_loader_test — cooked-scene deserializer ───────────────────────
// `assetlib::loadScene` parses a .scene binary whose 32-byte header declares an
// entity count and a string-table size that the file must actually contain, and
// whose 256-byte entity records address strings by (offset, length) into that
// table. Three separate places where a header field is trusted before it is
// checked.
//
// This target exists because the SAME two bugs were already found and fixed in
// the mesh deserializer by `fuzz_mesh_loader_test` — unbounded allocation from a
// header count, and `return f.good() || f.eof()` accepting a truncated file as
// success — and the scene loader carried both verbatim. A hardening that lands
// in one deserializer and not its sibling is not hardening; it is a coincidence.
// The scene path is the more exposed of the two, since a shipped dist loads
// scenes with no registry and no source assets to fall back on.
//
// Properties asserted per case:
//   1. Never crashes, hangs, or reads out of bounds. Run under ASan/UBSan for
//      the last clause to mean anything — the string-table reads below are
//      heap overreads that a non-instrumented build will happily return.
//   2. BOUNDED WORK: a 32-byte header must never be able to ask for gigabytes.
//   3. INTERNAL CONSISTENCY on success: if loadScene returns true, the entity
//      vector and string table match the header, and every string offset a
//      caller would dereference is inside the table.
//   4. Round-trip: anything saveScene writes, loadScene reads back intact.
//      Rejecting our own valid output is silent content loss.
#include "fuzz/fuzz.h"

#include <assetlib/scene_asset.h>

#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static constexpr uint32_t kGeneratorVersion = 1;

namespace {

// Every string-table field a consumer actually dereferences. Kept as one list
// so a new field added to SceneEntity gets fuzzed by being added here, rather
// than by someone remembering to widen an assertion.
struct StrField { const char* name; uint32_t off; uint32_t len; bool zTerm; };

std::vector<StrField> stringFieldsOf(const assetlib::SceneEntity& e) {
    return {
        { "name",         e.nameOffset,          e.nameLength,          false },
        { "meshCooked",   e.meshCookedOffset,    e.meshCookedLength,    false },
        { "meshSource",   e.meshSourceOffset,    e.meshSourceLength,    false },
        { "scriptPath",   e.scriptPathOffset,    e.scriptPathLength,    false },
        { "animClipPath", e.animClipPathOffset,  e.animClipPathLength,  false },
        // v3: null-terminated, no length field — there is no room in the 256
        // bytes. Read through stringTableReadZ instead.
        { "materialName", e.materialNameOffset,  0,                     true  },
    };
}

// Build a structurally plausible scene, then corrupt it. Starting from valid
// data is the point: random bytes die on the magic check in the first four
// bytes and never reach the code that has the bugs.
assetlib::SceneAsset buildPlausible(fuzz::Rng& rng) {
    using namespace assetlib;
    SceneAsset s;

    const uint32_t n = rng.range(0, 24);
    for (uint32_t i = 0; i < n; ++i) {
        SceneEntity e{};
        e.entityId = rng.next();
        // A real parent sometimes, a dangling one sometimes, self-parent
        // occasionally — the reparent guard is a consumer-side concern but the
        // loader must hand the bytes over without caring.
        e.parentId = rng.chance(40) ? (i ? s.entities[rng.below(i)].entityId : 0)
                   : rng.chance(50) ? e.entityId
                                    : rng.next();

        uint32_t mask = 0;
        if (rng.chance(95)) mask |= kComp_Transform;
        if (rng.chance(70)) mask |= kComp_Name;
        if (rng.chance(60)) mask |= kComp_MeshRenderer;
        if (rng.chance(15)) mask |= kComp_Camera;
        if (rng.chance(25)) mask |= kComp_RigidBody;
        if (rng.chance(20)) mask |= kComp_Script;
        if (rng.chance(10)) mask |= kComp_CharacterController;
        if (rng.chance(20)) mask |= kComp_Light;
        if (rng.chance(15)) mask |= kComp_Animator;
        // Bits with no component behind them: a v4 file read by a v3 consumer.
        if (rng.chance(10)) mask |= (1u << rng.range(9, 31));
        e.componentMask = mask;

        auto putStr = [&](uint32_t& off, uint32_t& len) {
            const uint32_t chars = rng.range(0, rng.chance(8) ? 300 : 40);
            std::string v;
            for (uint32_t k = 0; k < chars; ++k)
                v.push_back((char)rng.range(32, 126));
            const auto [o, l] = stringTableAppend(s.stringTable, v);
            off = o; len = l;
        };
        if (mask & kComp_Name)         putStr(e.nameOffset, e.nameLength);
        if (mask & kComp_MeshRenderer) {
            putStr(e.meshCookedOffset, e.meshCookedLength);
            if (rng.chance(50)) putStr(e.meshSourceOffset, e.meshSourceLength);
            if (rng.chance(50)) {
                uint32_t o, l;
                putStr(o, l);
                e.materialNameOffset = o;   // z-terminated, length discarded
            }
            e.meshSourceType = (uint8_t)rng.below(2);
        }
        if (mask & kComp_Script)   putStr(e.scriptPathOffset, e.scriptPathLength);
        if (mask & kComp_Animator) {
            if (rng.chance(50)) putStr(e.animClipPathOffset, e.animClipPathLength);
            e.animClipIndex = (int16_t)rng.next();
            e.animPlaying   = (uint8_t)rng.below(2);
        }
        e.lightType   = (uint8_t)rng.below(4);
        e.rbBodyType  = (uint8_t)rng.below(4);
        e.rbShape     = (uint8_t)rng.below(4);
        s.entities.push_back(e);
    }
    s.header.entityCount     = (uint32_t)s.entities.size();
    s.header.stringTableSize = (uint32_t)s.stringTable.size();
    return s;
}

enum class Corruption {
    None, Truncate, HeaderOnly, Empty, BadMagic, BadVersion,
    InflateEntityCount, InflateStringTableSize, HugeCounts,
    // The string-table field corruptions: the offsets and lengths a consumer
    // dereferences. `OverflowOffsetLen` is the specific shape that makes
    // `offset + length` wrap in 32-bit arithmetic.
    BadStringOffset, BadStringLength, OverflowOffsetLen,
    BitFlips, COUNT
};

void writeCorrupted(const fs::path& p, const assetlib::SceneAsset& src,
                    Corruption c, fuzz::Rng& rng) {
    using namespace assetlib;
    SceneAsset s = src;

    auto anyEntity = [&]() -> SceneEntity* {
        return s.entities.empty() ? nullptr
                                  : &s.entities[rng.below((uint32_t)s.entities.size())];
    };

    switch (c) {
        case Corruption::InflateEntityCount:
            s.header.entityCount = rng.interestingU32(); break;
        case Corruption::InflateStringTableSize:
            s.header.stringTableSize = rng.interestingU32(); break;
        case Corruption::HugeCounts:
            s.header.entityCount     = 0xFFFFFFFFu;
            s.header.stringTableSize = 0xFFFFFFFFu; break;
        case Corruption::BadStringOffset:
            if (auto* e = anyEntity()) {
                uint32_t v = rng.interestingU32();
                switch (rng.below(6)) {
                    case 0: e->nameOffset         = v; break;
                    case 1: e->meshCookedOffset   = v; break;
                    case 2: e->meshSourceOffset   = v; break;
                    case 3: e->scriptPathOffset   = v; break;
                    case 4: e->animClipPathOffset = v; break;
                    default: e->materialNameOffset = v; break;
                }
            }
            break;
        case Corruption::BadStringLength:
            if (auto* e = anyEntity()) {
                uint32_t v = rng.interestingU32();
                switch (rng.below(5)) {
                    case 0: e->nameLength         = v; break;
                    case 1: e->meshCookedLength   = v; break;
                    case 2: e->meshSourceLength   = v; break;
                    case 3: e->scriptPathLength   = v; break;
                    default: e->animClipPathLength = v; break;
                }
            }
            break;
        case Corruption::OverflowOffsetLen:
            // offset + length must WRAP: both fields are uint32_t, so a bounds
            // check written as `offset + length > table.size()` computes in
            // 32-bit and passes for a pair that addresses far past the end.
            if (auto* e = anyEntity()) {
                const uint32_t off = 0xFFFFFFFFu - rng.range(1, 64);
                const uint32_t len = (uint32_t)(0x100000000ULL - off) + rng.range(1, 64);
                switch (rng.below(5)) {
                    case 0: e->nameOffset = off;         e->nameLength = len; break;
                    case 1: e->meshCookedOffset = off;   e->meshCookedLength = len; break;
                    case 2: e->meshSourceOffset = off;   e->meshSourceLength = len; break;
                    case 3: e->scriptPathOffset = off;   e->scriptPathLength = len; break;
                    default: e->animClipPathOffset = off; e->animClipPathLength = len; break;
                }
            }
            break;
        default: break;
    }

    // Serialize through the real writer, then damage the bytes on disk. Writing
    // through saveScene keeps the layout honest rather than hand-rolling a
    // second encoder that could drift from the real one.
    if (!saveScene(s, p)) return;

    // saveScene overwrites the header counts from the vectors, so the count
    // corruptions have to be re-applied to the bytes after the fact.
    const bool patchHeader = c == Corruption::InflateEntityCount
                          || c == Corruption::InflateStringTableSize
                          || c == Corruption::HugeCounts
                          || c == Corruption::BadVersion;
    const bool patchBytes  = c == Corruption::BadMagic || c == Corruption::BitFlips
                          || c == Corruption::Truncate || c == Corruption::Empty
                          || c == Corruption::HeaderOnly;
    if (!patchHeader && !patchBytes) return;

    std::vector<uint8_t> bytes;
    { std::ifstream in(p, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>()); }

    if (patchHeader && bytes.size() >= sizeof(SceneHeader)) {
        SceneHeader h{};
        std::memcpy(&h, bytes.data(), sizeof(SceneHeader));
        if (c == Corruption::BadVersion)              h.version = rng.interestingU32();
        else if (c == Corruption::InflateEntityCount) h.entityCount = s.header.entityCount;
        else if (c == Corruption::InflateStringTableSize)
                                                      h.stringTableSize = s.header.stringTableSize;
        else { h.entityCount = 0xFFFFFFFFu; h.stringTableSize = 0xFFFFFFFFu; }
        std::memcpy(bytes.data(), &h, sizeof(SceneHeader));
    }
    if (c == Corruption::BadMagic && bytes.size() >= 4) {
        for (int i = 0; i < 4; ++i) bytes[i] = rng.byte();
    } else if (c == Corruption::Truncate && !bytes.empty()) {
        bytes.resize(rng.below((uint32_t)bytes.size()));
    } else if (c == Corruption::HeaderOnly) {
        if (bytes.size() > sizeof(SceneHeader)) bytes.resize(sizeof(SceneHeader));
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

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    using namespace assetlib;
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "scene_loader";

    fuzz::Rng shapeRng(fuzz::deriveSeed(masterSeed, "scene_shape"));
    fuzz::Rng corruptRng(fuzz::deriveSeed(masterSeed, "scene_corrupt"));

    fuzz::Scratch scratch("sceneload");
    const fs::path file = scratch.path() / "fuzz.scene";

    const SceneAsset original = buildPlausible(shapeRng);
    const auto corruption = (Corruption)corruptRng.below((uint32_t)Corruption::COUNT);
    writeCorrupted(file, original, corruption, corruptRng);

    std::error_code ec;
    const auto onDisk = (size_t)std::filesystem::file_size(file, ec);
    const size_t onDiskSize = ec ? 0 : onDisk;

    // ── The call under test ─────────────────────────────────────────────────
    SceneAsset loaded;
    const bool ok = loadScene(loaded, file);

    // ── Property 2: bounded work, whether it succeeded or not ───────────────
    // A rejection that allocates a terabyte first is not a rejection. The
    // header is 32 bytes; nothing it says may cost more than the file could
    // plausibly justify. (This is checked on the OUT param because a failed
    // load still leaves whatever it allocated behind.)
    const size_t entityBytes = loaded.entities.size() * sizeof(SceneEntity);
    const size_t tableBytes  = loaded.stringTable.size();
    if (entityBytes + tableBytes > onDiskSize + 64 * 1024) {
        rep.fail(key, "allocated " + std::to_string(entityBytes + tableBytes)
                 + " B from a " + std::to_string(onDiskSize) + " B file — a "
                 "32-byte header must not be able to size an allocation the "
                 "file cannot back");
        return;
    }

    if (!ok) return;    // a clean rejection is always an acceptable outcome

    // ── Property 3: a successful load is internally consistent ──────────────
    if (loaded.entities.size() != loaded.header.entityCount) {
        rep.fail(key, "accepted a scene whose entity vector ("
                 + std::to_string(loaded.entities.size()) + ") != header "
                 "entityCount (" + std::to_string(loaded.header.entityCount) + ")");
        return;
    }
    if (loaded.stringTable.size() != loaded.header.stringTableSize) {
        rep.fail(key, "accepted a scene whose string table ("
                 + std::to_string(loaded.stringTable.size()) + " B) != header "
                 "stringTableSize (" + std::to_string(loaded.header.stringTableSize)
                 + " B)");
        return;
    }
    // The file must actually have contained what the header claimed. This is
    // the truncation property: `f.good() || f.eof()` returns true for a short
    // read, handing back zero-filled records the file never contained.
    const size_t declared = sizeof(SceneHeader)
                          + (size_t)loaded.header.entityCount * sizeof(SceneEntity)
                          + (size_t)loaded.header.stringTableSize;
    if (declared > onDiskSize) {
        rep.fail(key, "accepted a TRUNCATED scene: header declares "
                 + std::to_string(declared) + " B but the file is "
                 + std::to_string(onDiskSize) + " B — the tail is zero-filled "
                 "and indistinguishable from real content");
        return;
    }

    // ── Property 1 (the memory-safety half): every read a consumer makes ────
    // SceneService dereferences these for real. Calling them here is what puts
    // the overread under the sanitizer; the length assertion is what catches it
    // on a build without one.
    for (const auto& e : loaded.entities) {
        for (const auto& f : stringFieldsOf(e)) {
            if (f.zTerm) {
                const std::string v = stringTableReadZ(loaded.stringTable, f.off);
                if (v.size() > loaded.stringTable.size()) {
                    rep.fail(key, std::string(f.name) + ": stringTableReadZ "
                             "returned more bytes than the table holds");
                    return;
                }
                continue;
            }
            const std::string v = stringTableRead(loaded.stringTable, f.off, f.len);
            if (v.empty()) continue;
            // A non-empty result must lie wholly inside the table. Computed in
            // 64-bit deliberately — the bug being hunted is a bounds check that
            // wrapped because it was computed in 32.
            const uint64_t end = (uint64_t)f.off + (uint64_t)f.len;
            if (end > (uint64_t)loaded.stringTable.size()) {
                rep.fail(key, std::string(f.name) + ": returned a "
                         + std::to_string(v.size()) + "-byte string from offset "
                         + std::to_string(f.off) + " length " + std::to_string(f.len)
                         + " in a " + std::to_string(loaded.stringTable.size())
                         + "-byte table — offset+length wrapped in 32-bit and the "
                           "read ran off the heap");
                return;
            }
            if (v.size() != f.len) {
                rep.fail(key, std::string(f.name) + ": returned "
                         + std::to_string(v.size()) + " bytes for a declared "
                         "length of " + std::to_string(f.len));
                return;
            }
        }
    }
}

// ── Property 4: round-trip ───────────────────────────────────────────────────
// Separate from the corruption case so a generator bug cannot make it vacuous.
void roundTripCase(uint64_t masterSeed, fuzz::Report& rep) {
    using namespace assetlib;
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "scene_loader/roundtrip";

    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "scene_roundtrip"));
    fuzz::Scratch scratch("sceneround");
    const fs::path file = scratch.path() / "rt.scene";

    const SceneAsset original = buildPlausible(rng);
    if (!saveScene(original, file)) {
        rep.fail(key, "saveScene failed on a scene it generated itself");
        return;
    }
    SceneAsset back;
    if (!loadScene(back, file)) {
        rep.fail(key, "loadScene REJECTED valid saveScene output — silent "
                      "content loss for every scene of this shape");
        return;
    }
    if (back.entities.size() != original.entities.size()) {
        rep.fail(key, "round-trip lost entities: wrote "
                 + std::to_string(original.entities.size()) + ", read "
                 + std::to_string(back.entities.size()));
        return;
    }
    if (back.stringTable != original.stringTable) {
        rep.fail(key, "round-trip corrupted the string table");
        return;
    }
    if (!original.entities.empty() &&
        std::memcmp(back.entities.data(), original.entities.data(),
                    original.entities.size() * sizeof(SceneEntity)) != 0) {
        rep.fail(key, "round-trip changed an entity record byte-for-byte");
    }
}

} // namespace

int main(int argc, char** argv) {
    return fuzz::run("scene_loader", argc, argv,
                     [](uint64_t seed, fuzz::Report& rep) {
                         oneCase(seed, rep);
                         roundTripCase(seed, rep);
                     });
}
