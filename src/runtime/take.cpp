// ── take — implementation ───────────────────────────────────────────────────
#include "runtime/take.h"

#include <cstring>
#include <string_view>
#include <utility>

// FNV from addon_protocol.h, as sim_command and sim_intent take it: header-only
// and stdlib-only, so a take can be read by a tool with no engine linked.
#include <engine/addon_protocol.h>
#include "core/memory/mem.h"

namespace take {
namespace {

constexpr uint32_t kMagic = 0x454B4154u;              // "TAKE", little-endian
constexpr uint32_t kMaxIntentsPerTick = 1u << 16;     // a sanity bound, not a limit
constexpr size_t   kTickHeaderBytes = 32;             // tick, 2 digests, count, pad
constexpr size_t   kTrailerBytes = 16;                // body length + digest

struct Writer {
    std::vector<uint8_t>& b;
    void raw(const void* p, size_t n) {
        const auto* c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
    }
    void u32(uint32_t v) { raw(&v, sizeof v); }
    void u64(uint64_t v) { raw(&v, sizeof v); }
};

// Every read is bounds-checked against the BODY, never the whole buffer, so a
// field can never be read out of the trailer.
struct Reader {
    const uint8_t* p;
    size_t         n;
    size_t         at = 0;
    bool raw(void* dst, size_t k) {
        if (k == 0) return true;
        if (k > n - at) return false;
        std::memcpy(dst, p + at, k);
        at += k;
        return true;
    }
    bool u32(uint32_t& v) { return raw(&v, sizeof v); }
    bool u64(uint64_t& v) { return raw(&v, sizeof v); }
};

uint64_t digestOf(const uint8_t* p, size_t n) {
    return engine::addon::fnv1a64(
        std::string_view(reinterpret_cast<const char*>(p), n));
}

}  // namespace

bool wellFormed(const Take& t, std::string* why) {
    auto fail = [&](std::string s) { if (why) *why = std::move(s); return false; };
    uint64_t next = 0;
    for (size_t i = 0; i < t.ticks.size(); ++i) {
        const Tick& k = t.ticks[i];
        if (k.tick != i + 1)
            return fail("slot " + std::to_string(i) + " is tick " +
                        std::to_string(k.tick) + ", not " + std::to_string(i + 1));
        if (k.firstIntent != next)
            return fail("tick " + std::to_string(k.tick) + "'s intents do not "
                        "follow the previous tick's");
        next += k.intentCount;
    }
    if (next != t.intents.size())
        return fail("the ticks name " + std::to_string(next) + " intents, the "
                    "take holds " + std::to_string(t.intents.size()));
    return true;
}

std::vector<uint8_t> encode(const Take& t) {
    MEM_SCOPE(mem::Tag::Replay);
    std::vector<uint8_t> out;
    out.reserve(64 + t.start.size() + t.ticks.size() * kTickHeaderBytes +
                t.intents.size() * sizeof(simintent::Intent) + kTrailerBytes);
    Writer w{out};
    w.u32(kMagic);
    w.u32(Take::kVersion);
    w.u64(t.actionHash);
    w.u64(t.localController);
    uint32_t dtBits = 0;
    std::memcpy(&dtBits, &t.simDt, sizeof dtBits);
    w.u32(dtBits);
    w.u32(0);                                          // explicit padding
    w.u64(t.start.size());
    w.raw(t.start.data(), t.start.size());
    w.u64(t.ticks.size());
    for (const Tick& k : t.ticks) {
        w.u64(k.tick);
        w.u64(k.commandDigest);
        w.u64(k.worldHash);
        const std::span<const simintent::Intent> run = intentsOf(t, k);
        w.u32(static_cast<uint32_t>(run.size()));
        w.u32(0);
        w.raw(run.data(), run.size() * sizeof(simintent::Intent));
    }
    const uint64_t body = out.size();
    const uint64_t dig  = digestOf(out.data(), out.size());
    w.u64(body);
    w.u64(dig);
    return out;
}

bool decode(const uint8_t* p, size_t n, Take& out, std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    if (!p || n < kTrailerBytes + 8) return fail("too short to be a take");
    MEM_SCOPE(mem::Tag::Replay);

    // The trailer FIRST: a take is valid only if it is entirely present, and
    // checking the whole before reading any part is what makes a truncated file
    // a refusal instead of a shorter replay.
    uint64_t body = 0, dig = 0;
    std::memcpy(&body, p + n - kTrailerBytes, sizeof body);
    std::memcpy(&dig,  p + n - 8,             sizeof dig);
    if (body != n - kTrailerBytes) return fail("length mismatch — truncated or padded");
    if (digestOf(p, static_cast<size_t>(body)) != dig)
        return fail("digest mismatch — corrupted");

    Reader r{p, static_cast<size_t>(body)};
    uint32_t magic = 0, ver = 0, dtBits = 0, pad = 0;
    if (!r.u32(magic) || magic != kMagic) return fail("not a take (bad magic)");
    if (!r.u32(ver) || ver != Take::kVersion) return fail("unsupported take version");

    Take t;
    if (!r.u64(t.actionHash) || !r.u64(t.localController) ||
        !r.u32(dtBits) || !r.u32(pad))
        return fail("truncated header");
    // Padding must be ZERO. Without this, two files that differ byte for byte
    // decode as the same take — which makes "same file, same take" false and
    // leaves room to smuggle data through a field nothing reads.
    if (pad != 0) return fail("non-zero header padding");
    std::memcpy(&t.simDt, &dtBits, sizeof dtBits);

    uint64_t startLen = 0;
    if (!r.u64(startLen) || startLen > r.n - r.at) return fail("bad start length");
    t.start.assign(reinterpret_cast<const char*>(p + r.at),
                   static_cast<size_t>(startLen));
    r.at += static_cast<size_t>(startLen);

    uint64_t tickCount = 0;
    if (!r.u64(tickCount)) return fail("truncated tick count");
    // Bound the allocation by what the bytes could possibly hold, so a hostile
    // count cannot make this reserve gigabytes before failing.
    if (tickCount > (r.n - r.at) / kTickHeaderBytes)
        return fail("tick count exceeds the data");
    t.ticks.resize(static_cast<size_t>(tickCount));
    for (Tick& k : t.ticks) {
        uint32_t count = 0, pad2 = 0;
        if (!r.u64(k.tick) || !r.u64(k.commandDigest) || !r.u64(k.worldHash) ||
            !r.u32(count) || !r.u32(pad2))
            return fail("truncated tick");
        if (pad2 != 0) return fail("non-zero tick padding");
        if (count > kMaxIntentsPerTick) return fail("implausible intent count");
        // Bounded by the bytes BEFORE growing, so a hostile count cannot make
        // the shared array allocate what the file cannot hold.
        if ((size_t)count * sizeof(simintent::Intent) > r.n - r.at)
            return fail("truncated intents");
        k.firstIntent = static_cast<uint32_t>(t.intents.size());
        k.intentCount = count;
        t.intents.resize(t.intents.size() + count);
        r.raw(t.intents.data() + k.firstIntent, count * sizeof(simintent::Intent));
        for (const simintent::Intent& in : intentsOf(t, k))
            if (in._pad != 0 || in._pad2 != 0)
                return fail("non-zero intent padding");
    }
    if (r.at != r.n) return fail("bytes left over inside the body");

    out = std::move(t);
    return true;
}

}  // namespace take
