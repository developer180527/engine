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
//   4. LOSS IS COUNTED AND VISIBLE. A ring drops the oldest; that is correct
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

struct Category {
    const char*         name;
    // Which levels are recorded. A bitmask rather than a minimum level so a
    // caller can watch errors from everything and info from ONE subsystem.
    std::atomic<uint32_t> mask;
    std::atomic<uint64_t> written;   // lifetime lines from this category
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

// Resolve (or create) a category. Called once per CALL SITE via the macros'
// function-local static, so the linear scan below runs a few dozen times in the
// life of the process, not once per line.
inline Category* category(const char* name) {
    const int n = g_reg.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_reg.cats[i].name == name ||
            (g_reg.cats[i].name && std::strcmp(g_reg.cats[i].name, name) == 0))
            return &g_reg.cats[i];
    // Claim a slot. A race just means two call sites with the same tag get two
    // slots — harmless duplication in the UI, never a wrong filter decision.
    const int idx = g_reg.count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= kMaxCategories) {
        g_reg.count.store(kMaxCategories, std::memory_order_release);
        return &g_reg.cats[0];        // degrade, do not fail
    }
    Category& c = g_reg.cats[idx];
    c.name = name;
    uint32_t dm = g_reg.defaultMask.load(std::memory_order_relaxed);
    c.mask.store(dm ? dm : kDefaultMask, std::memory_order_release);
    return &c;
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
inline void watchAll() {
    const int n = categoryCount();
    for (int i = 0; i < n; ++i)
        g_reg.cats[i].mask.store(kDefaultMask, std::memory_order_relaxed);
}

// ── The ring ────────────────────────────────────────────────────────────────
// 4096 slots x 192-byte messages ~= 900 KB of BSS. Fixed, so the log path never
// allocates and the footprint never surprises a memory budget.
constexpr uint32_t kSlots  = 4096;             // power of two: index is a mask
constexpr uint32_t kMsgMax = 192;

struct Slot {
    // seq+1 once the record is complete; 0 = never written. Deliberately NOT
    // initialised here, so the whole array is statically zero-initialised and
    // safe to write from a static-init-time log call.
    std::atomic<uint64_t> stamp;
    double      t;          // seconds since first use
    uint64_t    frame;
    const char* cat;
    uint8_t     level;
    uint16_t    len;
    bool        truncated;
    char        msg[kMsgMax];
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
    // Reserve first: the slot is ours for as long as it takes 4096 more lines to
    // lap us, which is orders of magnitude longer than a vsnprintf.
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    Slot& s = g_slots[seq & (kSlots - 1)];
    // Mark in-progress so a concurrent reader discards rather than reads half a
    // record. (stamp != seq+1 means "not publishable".)
    s.stamp.store(0, std::memory_order_relaxed);

    const int n = std::vsnprintf(s.msg, kMsgMax, fmt, args);
    uint16_t len;
    if (n < 0) { s.msg[0] = '\0'; len = 0; }
    else if ((uint32_t)n >= kMsgMax) {
        len = (uint16_t)(kMsgMax - 1);
        s.truncated = true;
        g_truncated.fetch_add(1, std::memory_order_relaxed);
    } else { len = (uint16_t)n; s.truncated = false; }
    s.len   = len;
    s.t     = now();
    s.frame = g_frame.load(std::memory_order_relaxed);
    s.cat   = c ? c->name : "?";
    s.level = (uint8_t)level;
    // PUBLISH LAST: everything above must be visible before the stamp is.
    s.stamp.store(seq + 1, std::memory_order_release);

    if (c) c->written.fetch_add(1, std::memory_order_relaxed);
    if (g_toStdout.load(std::memory_order_relaxed))
        std::printf("[%s] %s\n", s.cat, s.msg);
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
    out.t = s.t; out.frame = s.frame; out.seq = seq; out.cat = s.cat;
    out.level = (Level)s.level; out.truncated = s.truncated;
    const uint16_t len = s.len < kMsgMax ? s.len : (uint16_t)(kMsgMax - 1);
    std::memcpy(out.msg, s.msg, len);
    out.msg[len] = '\0';
    std::atomic_thread_fence(std::memory_order_acquire);
    // Re-check: if a writer lapped us mid-copy, what we just read is a mix of
    // two records and must be discarded rather than displayed.
    return s.stamp.load(std::memory_order_acquire) == before;
}

inline void clear() {
    // Does NOT reset g_seq — sequence numbers stay monotonic for the life of the
    // process, so "cleared" cannot be confused with "wrapped". The console just
    // starts reading from here.
    const uint64_t w = written();
    for (uint32_t i = 0; i < kSlots; ++i)
        g_slots[i].stamp.store(0, std::memory_order_relaxed);
    (void)w;
}

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
