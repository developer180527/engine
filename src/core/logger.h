#pragma once
// ── elog:: — a lock-free ring the engine can afford to write to ──────────────
//
// `elog`, not `log`: a namespace called `log` makes every unqualified `log(x)`
// call ambiguous against ::log from <cmath>, which broke third_party/imgui the
// moment this header reached the same translation unit. Matches the terseness of
// the neighbours (mem, prof, dbg, jobs).
//
// The old Logger cost **2.44 µs per line**, measured with stdout redirected to
// /dev/null so terminal I/O was excluded. Decomposed:
//
//     vsnprintf only            0.157 us
//     + printf to /dev/null     0.280 us  (+0.123)
//     + mutex + vector shift    2.431 us  (+2.151)   <- 88% of the cost
//
// The "ring" was a `std::vector<LogEntry>` doing `erase(begin())` at 1024 — an
// O(n) shift of a thousand entries, each holding two `std::string`s, plus two
// heap allocations per push. A thousand lines in a frame cost 2.4 ms, which is
// most of a 60 Hz budget spent on telling somebody about it. That is why
// "stream the logs of a subsystem" was not previously a thing you could do.
//
// FOUR DECISIONS, in the order they matter:
//
//   1. THE FILTER IS CHECKED BEFORE FORMATTING. There was no filter at all —
//      every LOG_* formatted and printed unconditionally. Each call site now
//      resolves its category ONCE (a function-local static) and then tests one
//      relaxed atomic load, so a category you are not watching costs ~1 ns
//      instead of 2 440. This is what makes per-subsystem targeting free for
//      the subsystems you are NOT targeting.
//   2. FIXED-SIZE SLOTS, NO ALLOCATION, NO LOCK. A writer takes one
//      `fetch_add` on a sequence counter and formats straight into its slot.
//      Nothing allocates, nothing blocks, and any number of threads can write
//      at once — which matters because job workers log.
//   3. RECORDS ARE PUBLISHED WITH A SEQUENCE STAMP, so a reader can tell that
//      the slot it was reading got recycled underneath it (see `read`). The
//      alternative — locking readers against writers — puts the console's frame
//      time inside the engine's log path.
//   4. THE ORDERING IS THE CORRECTNESS, and two halves of it were wrong in the
//      first version — both benign on x86's strong ordering and both real on
//      ARM64, which is every phone this engine targets. `Category::name` is now
//      published with release AFTER the slot is filled (the index is reserved by
//      a fetch_add, which makes the slot visible first), and the in-progress
//      poison is followed by a release FENCE so the field writes cannot become
//      visible ahead of it. Run the TSan lane — `cmake -B build-tsan
//      -DENGINE_SANITIZE=thread` — after touching any of this; a comment is not
//      a proof and neither is a green run on a Mac.
//   5. LOSS IS COUNTED AND VISIBLE. A ring drops the oldest; that is correct
//      and it must not be silent. `evicted()` is exact, and the console shows
//      it, because a console that quietly omits the line you are looking for is
//      worse than one that says it lost 1 342.
//
// WHAT DOES NOT BELONG HERE: per-item telemetry. Ten thousand events a second
// of "this draw did X" is a TRACE, not a log — it belongs in the profiler and in
// counters (`rdiag::SubmitStats`), where it costs a increment instead of a
// formatted string. The renderer's rule that every log site is latched exists
// for the same reason. This ring is for events a human reads.
//
// TAGS MUST HAVE STATIC LIFETIME. Every existing call site passes a literal
// ("Renderer", "Scene", …); the pointer is stored, not the characters.
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace elog {

enum class Level : uint8_t { Trace, Debug, Info, Success, Warning, Error, Count };

inline const char* levelName(Level l) {
    switch (l) {
        case Level::Trace:   return "trace";
        case Level::Debug:   return "debug";
        case Level::Info:    return "info";
        case Level::Success: return "ok";
        case Level::Warning: return "warn";
        case Level::Error:   return "error";
        default:             return "?";
    }
}

// ── Categories ──────────────────────────────────────────────────────────────
// One per subsystem tag, discovered at first use. Fixed capacity because the
// registry is touched from the log path and must never allocate; 64 is far more
// than the tag list in this engine and overflow degrades to "uncategorised"
// rather than failing.
constexpr int kMaxCategories = 64;
// The LAST slot is reserved as the overflow bucket, so 63 subsystems can be
// targeted independently and the 64th onward are visibly pooled rather than
// silently merged into whichever real subsystem happened to register first.
constexpr int kUsableCategories = kMaxCategories - 1;
constexpr int kOverflowIndex    = kMaxCategories - 1;

// ── Who a line is FOR ───────────────────────────────────────────────────────
// Two consoles read this one ring, because two different people are looking:
//
//   Game   a person building a game. They want to know that THEIR content or
//          THEIR script is wrong. "AssetService loaded 176 meshes" is not their
//          problem; "your material references a texture that does not exist" is.
//   Engine a person debugging the engine. They want the machinery: extraction
//          phases, job pool state, allocator growth, submit counters.
//
// Default is Engine, and the curated set below is opened up explicitly. THE
// IMPORTANT PART: `Game` is an allowlist for INFO-LEVEL CHATTER ONLY. Warnings
// and errors from every category always reach the game console (see
// `visibleToGame`), so a tag nobody remembered to mark can cost noise or silence
// at info level and can never hide a failure. That asymmetry is what makes the
// split safe to get wrong.
enum class Audience : uint8_t { Engine, Game };

// Longest category name kept when the engine has to OWN the string. Engine call
// sites pass literals and cost nothing; see `categoryCopied`.
constexpr int kMaxNameLen = 32;

struct Category {
    // ATOMIC, and published LAST. The index is reserved with a fetch_add, which
    // makes the slot visible to every other thread BEFORE its contents are
    // written — so a plain `const char* name` was a genuine data race, not the
    // benign duplication the comment below describes: a concurrent reader could
    // observe the bumped count, index this slot, and read `name` with no
    // happens-before edge covering the write. A release store here paired with an
    // acquire load in every reader is the edge. Null means "reserved, not ready";
    // readers skip it, which degrades to the duplication case that IS benign.
    std::atomic<const char*> name;
    // Storage for a name the engine had to copy. Zero-length for the common case
    // (a literal in engine code), in which case `name` points at that literal.
    char                owned[kMaxNameLen];
    // Which levels are recorded. A bitmask rather than a minimum level so a
    // caller can watch errors from everything and info from ONE subsystem.
    std::atomic<uint32_t> mask;
    std::atomic<uint64_t> written;   // lifetime lines from this category
    std::atomic<uint8_t>  audience;  // Audience; zero-init = Engine
};

struct Registry {
    Category            cats[kMaxCategories];
    std::atomic<int>    count;
    std::atomic<uint32_t> defaultMask;   // applied to categories on creation
};
inline Registry g_reg;

constexpr uint32_t levelBit(Level l) { return 1u << (uint32_t)l; }
// Everything except Trace, which is opt-in per category.
constexpr uint32_t kDefaultMask = 0xFFFFFFFFu & ~levelBit(Level::Trace);

// ── Overflow ────────────────────────────────────────────────────────────────
// Past 63 distinct tags, everything lands here. This used to return
// `&cats[0]` with no diagnostic of any kind, which is a much worse failure than
// "degrade, do not fail" suggests: the 64th tag inherited slot 0's mask, bumped
// slot 0's counter, and DISPLAYED UNDER SLOT 0'S NAME — so a kit's lines would
// appear to come from, say, the renderer, and could not be targeted or silenced
// independently. That directly defeats the one feature this file exists for.
//
// A named bucket keeps the lines identifiable and keeps the first 63 subsystems
// individually targetable, and the count says how much was pooled.
inline std::atomic<uint64_t> g_overflowHits{0};
inline std::atomic<bool>     g_overflowWarned{false};

inline Category* overflowCategory(const char* wanted) {
    Category& c = g_reg.cats[kOverflowIndex];
    if (c.name.load(std::memory_order_acquire) == nullptr) {
        c.mask.store(kDefaultMask, std::memory_order_relaxed);
        c.name.store("(categories exhausted)", std::memory_order_release);
    }
    g_overflowHits.fetch_add(1, std::memory_order_relaxed);
    if (!g_overflowWarned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[elog] more than %d distinct log categories — '%s' "
                     "and everything after it is pooled into "
                     "'(categories exhausted)' and cannot be targeted or silenced "
                     "on its own. Raise kMaxCategories.\n",
                     kUsableCategories, wanted ? wanted : "?");
    return &c;
}
inline uint64_t overflowHits() { return g_overflowHits.load(std::memory_order_relaxed); }

// Resolve (or create) a category. Called once per CALL SITE via the macros'
// function-local static, so the linear scan below runs a few dozen times in the
// life of the process, not once per line.
inline Category* category(const char* name) {
    const int n = g_reg.count.load(std::memory_order_acquire);
    const int scan = n < kMaxCategories ? n : kMaxCategories;
    for (int i = 0; i < scan; ++i) {
        // Acquire: pairs with the release store at the bottom of this function.
        // A null name is a slot another thread has RESERVED but not filled —
        // skipping it is what makes claiming a second slot for the same tag the
        // worst case, instead of reading a pointer mid-write.
        const char* nm = g_reg.cats[i].name.load(std::memory_order_acquire);
        if (nm && (nm == name || std::strcmp(nm, name) == 0))
            return &g_reg.cats[i];
    }
    // Claim a slot. A race just means two call sites with the same tag get two
    // slots — harmless duplication in the UI, never a wrong filter decision.
    const int idx = g_reg.count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= kUsableCategories) {
        // Do not let count run away past the array — categoryCount() clamps, but
        // an ever-growing counter makes the diagnostic below meaningless.
        g_reg.count.store(kMaxCategories, std::memory_order_release);
        return overflowCategory(name);
    }
    Category& c = g_reg.cats[idx];
    uint32_t dm = g_reg.defaultMask.load(std::memory_order_relaxed);
    c.mask.store(dm ? dm : kDefaultMask, std::memory_order_relaxed);
    // PUBLISHED LAST, with release: everything above must be visible to a thread
    // that observes this pointer.
    c.name.store(name, std::memory_order_release);
    return &c;
}

// ── A category whose NAME the engine must own ───────────────────────────────
// For anything registering across the module ABI. A kit's string literal lives
// in the kit's dylib, and `Category::name` plus every ring record's `cat` field
// are POINTERS to it — so the moment that kit is dlclosed, the console reads
// freed memory. Copying is the only fix that survives a kit being unloaded, or
// crashing, or being force-unloaded by the editor. Same class of hazard as a
// deferred `onMain` callback outliving its library.
inline Category* categoryCopied(const char* name) {
    if (!name || !*name) name = "?";
    Category* c = category(name);       // resolves by text, so this still dedups
    if (c->name.load(std::memory_order_acquire) != c->owned) {
        std::snprintf(c->owned, sizeof(c->owned), "%s", name);
        c->name.store(c->owned, std::memory_order_release);
    }
    return c;
}

// ── Stable ids, for handles that cross the ABI ──────────────────────────────
// A pointer is not something to hand a module: it invites arithmetic and it
// cannot be validated on the way back. An index can be range-checked, and 0 is
// reserved so a zero-initialised struct means "no category".
inline uint32_t idOf(const Category* c) {
    if (!c) return 0;
    const ptrdiff_t i = c - &g_reg.cats[0];
    return (i >= 0 && i < kMaxCategories) ? (uint32_t)i + 1 : 0;
}
inline Category* categoryById(uint32_t id) {
    if (id == 0 || id > (uint32_t)kMaxCategories) return nullptr;
    const int n = g_reg.count.load(std::memory_order_acquire);
    return (int)(id - 1) < n ? &g_reg.cats[id - 1] : nullptr;
}

inline int categoryCount() {
    const int n = g_reg.count.load(std::memory_order_acquire);
    return n < kMaxCategories ? n : kMaxCategories;
}
inline Category& categoryAt(int i) { return g_reg.cats[i]; }

// THE HOT CHECK — one relaxed load and a bit test.
inline bool enabled(const Category* c, Level l) {
    return c && (c->mask.load(std::memory_order_relaxed) & levelBit(l)) != 0;
}

inline void setLevel(Category& c, Level l, bool on) {
    uint32_t m = c.mask.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t nm = on ? (m | levelBit(l)) : (m & ~levelBit(l));
        if (c.mask.compare_exchange_weak(m, nm, std::memory_order_relaxed)) return;
    }
}

// Watch ONE subsystem: everything at every level from `only`, errors and
// warnings from the rest. This is "set a target on a subsystem and stream it".
inline void solo(const Category* only) {
    const int n = categoryCount();
    for (int i = 0; i < n; ++i) {
        Category& c = g_reg.cats[i];
        c.mask.store(&c == only ? 0xFFFFFFFFu
                                : (levelBit(Level::Error) | levelBit(Level::Warning)),
                     std::memory_order_relaxed);
    }
}
// ── Audience plumbing ───────────────────────────────────────────────────────
inline void setAudience(Category& c, Audience a) {
    c.audience.store((uint8_t)a, std::memory_order_relaxed);
}
inline Audience audienceOf(const Category& c) {
    return (Audience)c.audience.load(std::memory_order_relaxed);
}

// The rule the game console uses. Info and below only for game-facing tags;
// warnings and errors from EVERYTHING, always — a game builder must not miss a
// hard failure because it came out of a subsystem nobody marked.
inline bool visibleToGame(const Category* c, Level l) {
    if (l >= Level::Warning) return true;
    return c && audienceOf(*c) == Audience::Game;
}

// The tags a game builder is answerable for. Deliberately a short, visible list
// rather than a flag on 56 call sites: these are the subsystems that report on
// the USER'S content — a broken material, an unresolved mesh, a script that
// threw. Everything else is machinery.
//
// "Script" covers Lua and every kit, because ScriptHost::logInfo/Warn/Error —
// which is what the whole `EngineApiCoreV1` logging group forwards to — writes
// under that tag. A kit's own diagnostics are the game builder's business.
inline void markGameFacingDefaults() {
    static const char* kGameTags[] = {
        "Script",        // Lua + every kit, via the C API
        "Scene",         // the user's scene failed to load / entity unresolved
        "AssetService",  // their asset is missing or unreadable
        "Cook", "SceneCook", "MaterialCook", "ShaderCook",   // their content
        "Import",        // their FBX/glTF
        "Project",       // their project.json
        "Input",         // their bindings
    };
    for (const char* t : kGameTags) setAudience(*category(t), Audience::Game);
}

// ── The shipping default ────────────────────────────────────────────────────
// A released game should not print engine internals into the player's log.
// "Loaded material: X -> shader Y (3 blocks, 2 textures, features 0x5)" is a
// developer's line; there are 117 Info/Success sites in the engine and they make
// a shipped log useless for the one thing it is for — finding the problem when
// something breaks.
//
// So: ENGINE categories drop to warnings and errors, GAME categories are left
// exactly as they are. That distinction is the whole reason `Audience` exists —
// a game's own diagnostics (its scripts, its kits, its content pipeline
// complaints) belong to the developer who shipped it, and silencing those would
// break anyone using the engine's logger as their game's logger.
//
// NOT a stdout switch. The ring is memory-only, so turning the mirror off would
// leave a shipped build with no persistence at all and a crash report with
// nothing in it. 0.12 us a line is a fair price for the only log that survives.
//
// Applied to categories that already exist AND set as the default for ones
// created later, so it does not matter whether the host calls this before or
// after boot logging has started.
inline void quietForShipping() {
    const uint32_t engineMask = levelBit(Level::Error) | levelBit(Level::Warning);
    g_reg.defaultMask.store(engineMask, std::memory_order_relaxed);
    const int n = categoryCount();
    for (int i = 0; i < n; ++i) {
        Category& c = g_reg.cats[i];
        if (audienceOf(c) == Audience::Engine)
            c.mask.store(engineMask, std::memory_order_relaxed);
    }
}

// The default a fresh category gets. `quietForShipping` is the one caller today;
// exposed because a dedicated server or a soak harness may want its own posture.
inline void setDefaultMask(uint32_t mask) {
    g_reg.defaultMask.store(mask, std::memory_order_relaxed);
}

inline void watchAll() {
    // Resets the DEFAULT too, not just the categories that exist. "Watch all"
    // has to mean subsystems discovered later as well — otherwise a dev who has
    // called quietForShipping (or is on a host that did) clicks Watch all in the
    // Internal Console, and every subsystem that registers after that click is
    // still silent, with the UI claiming otherwise. Caught by an unrelated
    // assertion in logger_test: the sticky default leaked into a later case.
    g_reg.defaultMask.store(kDefaultMask, std::memory_order_relaxed);
    const int n = categoryCount();
    for (int i = 0; i < n; ++i)
        g_reg.cats[i].mask.store(kDefaultMask, std::memory_order_relaxed);
}

// ── The ring ────────────────────────────────────────────────────────────────
// 4096 slots x 192-byte messages ~= 900 KB of BSS. Fixed, so the log path never
// allocates and the footprint never surprises a memory budget.
constexpr uint32_t kSlots  = 4096;             // power of two: index is a mask
constexpr uint32_t kMsgMax = 192;

// ── The record, and why the payload is atomic WORDS ─────────────────────────
// This is a seqlock: the reader copies a slot a writer may be overwriting and
// validates afterwards with the stamp. That protocol is correct — it is proved
// under load in logger_test, 4.6 M reads with zero spliced records — but as
// PLAIN fields it is also a data race by the letter of the standard, and
// therefore undefined behaviour, and ThreadSanitizer says so nine times.
//
// The memory model has no way to spell "a racy read I will discard". Relaxed
// atomic accesses are the way to spell it: they are well-defined under a race
// and on arm64 (and x86) a relaxed load/store of a word compiles to exactly the
// same instruction a plain one does — so this costs the extra copy through a
// local and nothing else.
//
// The alternative was to leave the race and suppress it. Rejected: a permanently
// red sanitizer lane is worse than the bug it is reporting, because it trains
// everyone to stop reading it.
struct Rec {
    double      t;          // seconds since first use
    uint64_t    frame;
    const char* cat;        // category name; engine-owned, stable for the process
    uint8_t     level;
    uint8_t     truncated;
    uint16_t    len;
    uint32_t    _pad;
    char        msg[kMsgMax];
};
static_assert(sizeof(Rec) % sizeof(uint64_t) == 0,
              "the payload is copied a word at a time");
constexpr uint32_t kRecWords = sizeof(Rec) / sizeof(uint64_t);

struct Slot {
    // seq+1 once the record is complete; 0 = never written. Deliberately NOT
    // initialised here, so the whole array is statically zero-initialised and
    // safe to write from a static-init-time log call.
    std::atomic<uint64_t> stamp;
    std::atomic<uint64_t> w[kRecWords];
};

inline Slot                  g_slots[kSlots];
inline std::atomic<uint64_t> g_seq{0};        // total lines ever written
inline std::atomic<uint64_t> g_truncated{0};
inline std::atomic<uint64_t> g_frame{0};
inline std::atomic<bool>     g_toStdout{true};

inline double now() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

// Mirror to stdout. On by default because every tool and test in the tree reads
// it; a shipping game turns it off and keeps the ring.
inline void setStdout(bool on) { g_toStdout.store(on, std::memory_order_relaxed); }
inline void setFrame(uint64_t f) { g_frame.store(f, std::memory_order_relaxed); }

inline void writeV(Category* c, Level level, const char* fmt, va_list args) {
    // Built on the STACK first, then published a word at a time. Formatting
    // straight into the slot would be one copy cheaper and would put a plain,
    // racy write where the reader is looking — see the note on Rec.
    Rec r;
    const int n = std::vsnprintf(r.msg, kMsgMax, fmt, args);
    if (n < 0) { r.msg[0] = '\0'; r.len = 0; r.truncated = 0; }
    else if ((uint32_t)n >= kMsgMax) {
        r.len = (uint16_t)(kMsgMax - 1);
        r.truncated = 1;
        g_truncated.fetch_add(1, std::memory_order_relaxed);
    } else { r.len = (uint16_t)n; r.truncated = 0; }
    r.t     = now();
    r.frame = g_frame.load(std::memory_order_relaxed);
    r.cat   = c ? c->name.load(std::memory_order_relaxed) : "?";
    if (!r.cat) r.cat = "?";          // a slot reserved but not yet named
    r.level = (uint8_t)level;
    r._pad  = 0;                      // padding is COPIED, so never leave it dirty

    uint64_t buf[kRecWords];
    std::memcpy(buf, &r, sizeof(r));

    // Reserve late: the slot is ours from here, and the window in which a reader
    // can catch it half-written is now a memcpy rather than a vsnprintf.
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    Slot& s = g_slots[seq & (kSlots - 1)];

    // ── Mark in-progress, then FENCE ────────────────────────────────────────
    // The poison tells a reader mid-copy that this slot is being recycled. A
    // relaxed store alone does not achieve that on a weakly-ordered machine —
    // which is every ARM64 target, i.e. every phone this engine is aimed at: the
    // payload stores below could become visible BEFORE the poison, so a reader
    // holding the previous sequence would pass its first stamp check, copy the new
    // record's words, and pass the re-check too (the writer's own stamp update not
    // being visible yet). It would return a splice of two records and believe it.
    //
    // The release fence orders this store ahead of every store after it, and the
    // reader's acquire fence orders its word loads ahead of its stamp re-read.
    // Together: if a reader saw ANY new word, its re-check must see at least the
    // poison, so it discards.
    s.stamp.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    for (uint32_t i = 0; i < kRecWords; ++i)
        s.w[i].store(buf[i], std::memory_order_relaxed);

    // PUBLISH LAST: every word above must be visible before the stamp is.
    s.stamp.store(seq + 1, std::memory_order_release);

    if (c) c->written.fetch_add(1, std::memory_order_relaxed);
    if (g_toStdout.load(std::memory_order_relaxed))
        std::printf("[%s] %s\n", r.cat, r.msg);
}

inline void write(Category* c, Level level, const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    writeV(c, level, fmt, a);
    va_end(a);
}

// ── Reading ─────────────────────────────────────────────────────────────────
struct Entry {
    double      t;
    uint64_t    frame;
    uint64_t    seq;
    const char* cat;
    Level       level;
    bool        truncated;
    char        msg[kMsgMax];
};

inline uint64_t written()  { return g_seq.load(std::memory_order_acquire); }
// Lines that scrolled out of the ring before anyone read them. EXACT, not an
// estimate, and the console shows it — a ring that silently omits the line you
// are hunting is worse than one that admits the loss.
inline uint64_t evicted() {
    const uint64_t w = written();
    return w > kSlots ? w - kSlots : 0;
}
inline uint64_t truncated() { return g_truncated.load(std::memory_order_relaxed); }
inline uint64_t oldest()    { return evicted(); }

// Copy one record out. False when that sequence was never written or has already
// been recycled — the double stamp check is what makes reading safe without
// locking the writers out.
inline bool read(uint64_t seq, Entry& out) {
    if (seq >= written()) return false;
    const Slot& s = g_slots[seq & (kSlots - 1)];
    const uint64_t before = s.stamp.load(std::memory_order_acquire);
    if (before != seq + 1) return false;              // not ours (yet, or any more)

    // Copied into locals FIRST and only published to `out` after the re-check.
    // A torn record can hold a garbage `cat` pointer, so nothing read here may
    // reach the caller — or be dereferenced — before the stamp is confirmed.
    uint64_t buf[kRecWords];
    for (uint32_t i = 0; i < kRecWords; ++i)
        buf[i] = s.w[i].load(std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_acquire);
    // If a writer lapped us mid-copy, what we just read is a mix of two records
    // and must be discarded rather than displayed.
    if (s.stamp.load(std::memory_order_acquire) != before) return false;

    Rec r;
    std::memcpy(&r, buf, sizeof(r));
    out.t = r.t; out.frame = r.frame; out.seq = seq; out.cat = r.cat;
    out.level = (Level)r.level; out.truncated = r.truncated != 0;
    const uint16_t len = r.len < kMsgMax ? r.len : (uint16_t)(kMsgMax - 1);
    std::memcpy(out.msg, r.msg, len);
    out.msg[len] = '\0';
    return true;
}

// NO clear(). Both consoles move a `viewFrom` sequence instead, which is
// strictly better and has no concurrency story at all: sequence numbers stay
// monotonic for the life of the process, so "cleared" can never be confused with
// "wrapped", and nothing has to be coordinated with in-flight writers.
//
// The version that existed zeroed every stamp under no lock, so a record whose
// publish landed after its slot was zeroed survived the clear while one that
// landed before did not — a clear that is not atomic across the ring, which was
// stated nowhere. It also had no callers. An unused primitive with an unstated
// concurrency caveat is a trap for whoever reaches for it first.

} // namespace elog

// ── The macros — signatures unchanged, so all 56 call sites are untouched ────
// The category is resolved ONCE per call site by the function-local static; the
// per-call cost when the category is filtered out is one atomic load and a
// branch. Note the arguments are not even evaluated when disabled, so an
// expensive expression inside a LOG_TRACE costs nothing in a build that filters
// it — which is only true because the check comes first.
#define ENGINE_LOG_(lvl, tag, ...)                                             \
    do {                                                                       \
        static ::elog::Category* engine_log_cat_ = ::elog::category(tag);         \
        if (::elog::enabled(engine_log_cat_, lvl))                              \
            ::elog::write(engine_log_cat_, lvl, __VA_ARGS__);                    \
    } while (0)

#define LOG_INFO(tag, ...)    ENGINE_LOG_(::elog::Level::Info,    tag, __VA_ARGS__)
#define LOG_SUCCESS(tag, ...) ENGINE_LOG_(::elog::Level::Success, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)    ENGINE_LOG_(::elog::Level::Warning, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...)   ENGINE_LOG_(::elog::Level::Error,   tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...)   ENGINE_LOG_(::elog::Level::Debug,   tag, __VA_ARGS__)

// TRACE compiles to nothing unless asked for. A shipping build must not pay for
// per-frame diagnostics even as a filtered-out branch.
#if defined(ENGINE_LOG_TRACE)
#  define LOG_TRACE(tag, ...) ENGINE_LOG_(::elog::Level::Trace, tag, __VA_ARGS__)
#else
#  define LOG_TRACE(tag, ...) do { } while (0)
#endif

// ── Back-compat shim ────────────────────────────────────────────────────────
// `LogLevel` and `Logger::get()` had exactly one consumer outside the macros
// (the editor console), which now reads elog:: directly. Kept so anything added
// in a branch still compiles.
using LogLevel = ::elog::Level;
