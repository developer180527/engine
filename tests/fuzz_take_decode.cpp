// ── fuzz_take_decode — a take file is untrusted input ───────────────────────
//
// A take is a file a user loads — shared by a teammate, attached to a bug
// report, downloaded. take::decode is therefore a parser of hostile bytes, the
// same class as the DDC manifest and scene loaders that already have targets.
//
// Properties, per case:
//   1. A generated take round-trips, and re-encodes to the SAME bytes.
//   2. Every truncation is refused, and leaves the output untouched.
//   3. Any bit flipped with the trailer left alone is refused (the digest).
//   4. A body mutated and RE-SIGNED — so it gets past the digest and reaches the
//      field parser — never crashes, never over-allocates, and when it is
//      accepted it re-encodes to EXACTLY the input. That last clause is what
//      the padding checks are for: without them two different files decode as
//      one take, and "same file, same take" stops being true.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <engine/addon_protocol.h>

#include "fuzz/fuzz.h"
#include "runtime/take.h"

namespace {

constexpr uint32_t kGeneratorVersion = 1;

take::Take generate(fuzz::Rng& rng) {
    take::Take t;
    t.actionHash      = rng.next();
    t.localController = rng.chance(20) ? 0 : rng.next();
    t.simDt           = rng.chance(80) ? 1.0f / 60.0f : (float)rng.range(1, 1000) * 1e-4f;
    const uint32_t startLen = rng.chance(20) ? 0u : (uint32_t)rng.range(1, 400);
    for (uint32_t i = 0; i < startLen; ++i) t.start.push_back((char)rng.range(0, 255));
    const uint32_t ticks = rng.chance(10) ? 0u : (uint32_t)rng.range(1, 24);
    for (uint32_t k = 0; k < ticks; ++k) {
        take::Tick tk;
        tk.tick          = k + 1;
        tk.commandDigest = rng.next();
        tk.worldHash     = rng.next();
        const uint32_t n = rng.chance(30) ? 0u : (uint32_t)rng.range(1, 4);
        for (uint32_t i = 0; i < n; ++i) {
            simintent::Intent in{};
            in.entity   = rng.next();
            in.moveX    = (float)rng.range(0, 2000) * 1e-3f - 1.0f;
            in.moveY    = (float)rng.range(0, 2000) * 1e-3f - 1.0f;
            in.lookDx   = (float)((int)rng.range(0, 200) - 100);
            in.lookDy   = (float)((int)rng.range(0, 200) - 100);
            in.held     = rng.interestingU32();
            in.pressed  = rng.interestingU32();
            in.released = rng.interestingU32();
            tk.intents.push_back(in);
        }
        t.ticks.push_back(std::move(tk));
    }
    return t;
}

// Rewrites the trailer so a mutated body passes the length and digest checks
// and reaches the field parser — the part a random flip never gets to.
void resign(std::vector<uint8_t>& b) {
    if (b.size() < 16) return;
    const uint64_t body = b.size() - 16;
    const uint64_t dig  = engine::addon::fnv1a64(
        std::string_view(reinterpret_cast<const char*>(b.data()), (size_t)body));
    std::memcpy(b.data() + body,     &body, 8);
    std::memcpy(b.data() + body + 8, &dig,  8);
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "take_decode";

    fuzz::Rng gen(fuzz::deriveSeed(masterSeed, "take_body"));
    fuzz::Rng mut(fuzz::deriveSeed(masterSeed, "take_mutation"));

    const take::Take t = generate(gen);
    const std::vector<uint8_t> bytes = take::encode(t);

    // 1. Round trip, canonical.
    {
        take::Take back; std::string err;
        if (!take::decode(bytes.data(), bytes.size(), back, &err))
            rep.fail(key, "a generated take was refused: " + err);
        else if (take::encode(back) != bytes)
            rep.fail(key, "a take did not re-encode to the same bytes");
    }

    // 2. Truncation, at a few random lengths plus the edges.
    {
        const size_t cuts[] = { 0, 1, bytes.size() - 1,
                                (size_t)mut.range(0, (uint32_t)bytes.size() - 1),
                                (size_t)mut.range(0, (uint32_t)bytes.size() - 1) };
        for (size_t cut : cuts) {
            take::Take untouched; untouched.actionHash = 0xA5A5;
            if (take::decode(bytes.data(), cut, untouched) || untouched.actionHash != 0xA5A5)
                rep.fail(key, "a take truncated to " + std::to_string(cut) +
                              " bytes was accepted or wrote its output");
        }
    }

    // 3. One flipped bit, trailer untouched.
    {
        std::vector<uint8_t> f = bytes;
        const size_t at = mut.range(0, (uint32_t)f.size() - 1);
        f[at] ^= (uint8_t)(1u << mut.range(0, 7));
        take::Take out;
        if (take::decode(f.data(), f.size(), out))
            rep.fail(key, "a flipped bit at byte " + std::to_string(at) + " was accepted");
    }

    // 4. Mutated and re-signed: reaches the parser.
    for (int round = 0; round < 8; ++round) {
        std::vector<uint8_t> m = bytes;
        const size_t body = m.size() - 16;
        const int edits = (int)mut.range(1, 4);
        for (int e = 0; e < edits && body > 0; ++e) {
            const size_t at = mut.range(0, (uint32_t)body - 1);
            switch (mut.range(0, 3)) {
            case 0: m[at] ^= (uint8_t)(1u << mut.range(0, 7)); break;
            case 1: m[at] = (uint8_t)mut.range(0, 255); break;
            case 2: {                            // a hostile 32-bit field
                const uint32_t v = mut.interestingU32();
                if (at + 4 <= body) std::memcpy(m.data() + at, &v, 4);
                break;
            }
            default: {                           // a hostile 64-bit length/count
                const uint64_t v = mut.chance(50) ? ~0ull : (uint64_t)mut.interestingU32();
                if (at + 8 <= body) std::memcpy(m.data() + at, &v, 8);
                break;
            }
            }
        }
        resign(m);
        take::Take out;
        if (take::decode(m.data(), m.size(), out) && take::encode(out) != m)
            rep.fail(key, "a re-signed mutation was ACCEPTED and re-encodes to "
                          "different bytes — two files decode as one take");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return fuzz::run("take_decode", argc, argv, oneCase);
}
