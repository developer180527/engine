#pragma once
// ── fuzz.h — seeded, reproducible fuzz harness ────────────────────────────────
// Header-only so a fuzz target is one self-contained .cpp registered with the
// normal engine_test() macro. No external fuzzing dependency: the runner is
// portable C++20 (MSVC included), which matters because the cross-platform
// port is the current priority. libFuzzer can be layered on later for the
// byte-oriented targets where coverage guidance pays — it is Clang-only.
//
// THE CONTRACT: every failure is reproducible from one 64-bit integer.
//   ./fuzz_x_test --seed 12345            re-run exactly that case
//   ./fuzz_x_test --iterations 100000     explore (fresh cases from a base seed)
//   ./fuzz_x_test --corpus <dir>          replay every committed case (the gate)
//
// Two lanes, one binary:
//   fuzz-regress  replays the committed corpus. Fast, deterministic, gating.
//                 Every bug ever found lives here forever, so it cannot return.
//   fuzz-explore  fresh random seeds, long-running, nightly. Never gates; its
//                 only job is to FEED the corpus.
//
// When explore finds a failure it prints a ReproKey. Committing that seed to
// the target's corpus/ directory is what converts a one-off discovery into a
// permanent regression test.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <process.h>          // _getpid, without dragging in windows.h
#else
    #include <unistd.h>           // getpid
#endif

namespace fuzz {

// Process id — scratch directories must not collide when ctest runs the
// regress and explore lanes of the same binary in parallel.
inline uint64_t currentPid() {
#if defined(_WIN32)
    return (uint64_t)::_getpid();
#else
    return (uint64_t)::getpid();
#endif
}

// ── Deterministic RNG ────────────────────────────────────────────────────────
// splitmix64: tiny, fast, good enough for structure generation, and — crucially
// — its output depends on NOTHING but its state. No std::random_device, no
// clock, no OS entropy anywhere in this harness: hidden entropy is what quietly
// turns "reproducible by seed" into a lie the first time it matters.
inline uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline uint64_t fnv1a64(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    for (; s && *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

// Per-DOMAIN substreams, not one shared RNG. A single shared generator has a
// nasty property: consuming a number for one purpose shifts the sequence for
// everything downstream, so ADDING a new random consumer silently changes every
// other domain's output for a given seed — invalidating every archived repro
// key. Deriving an independent stream per domain keeps old keys valid forever
// as the fuzz surface grows.
inline uint64_t deriveSeed(uint64_t masterSeed, const char* domain) {
    uint64_t x = masterSeed ^ fnv1a64(domain);
    return splitmix64(x);
}

class Rng {
public:
    explicit Rng(uint64_t seed) : m_state(seed ? seed : 0xD1CE5EEDULL) {}

    uint64_t next()             { return splitmix64(m_state); }
    // Uniform in [0, n). n == 0 yields 0.
    uint32_t below(uint32_t n)  { return n ? (uint32_t)(next() % n) : 0u; }
    // Inclusive range.
    uint32_t range(uint32_t lo, uint32_t hi) {
        return hi <= lo ? lo : lo + below(hi - lo + 1);
    }
    bool     chance(uint32_t percent) { return below(100) < percent; }
    uint8_t  byte()             { return (uint8_t)(next() & 0xFF); }
    // Values that historically break parsers: 0, 1, max, off-by-ones, and the
    // sign-bit boundaries. Uniform random 32-bit numbers almost never hit
    // these, and they are exactly where the bugs are.
    uint32_t interestingU32() {
        static const uint32_t kEdges[] = {
            0u, 1u, 2u, 3u, 4u, 7u, 8u, 0xFFu, 0x100u, 0xFFFFu, 0x10000u,
            0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
        };
        if (chance(70)) return kEdges[below((uint32_t)(sizeof(kEdges)/sizeof(*kEdges)))];
        return (uint32_t)next();
    }
private:
    uint64_t m_state;
};

// ── Repro key ────────────────────────────────────────────────────────────────
// Pins the GENERATOR version as well as the seed. If someone widens a
// generator's random range, the same seed produces different data — so a key
// that records only the seed silently stops reproducing the day the fuzzer is
// improved. Bump kGeneratorVersion in a target whenever its generator changes
// meaning; old corpus entries keep their recorded version and are still
// replayable as raw artifacts.
struct ReproKey {
    uint64_t    masterSeed       = 0;
    uint32_t    generatorVersion = 0;
    std::string target;
    std::string detail;          // what tripped, for the log

    std::string toString() const {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s/gen%u/seed=%llu",
                      target.c_str(), generatorVersion,
                      (unsigned long long)masterSeed);
        return buf;
    }
};

// ── Failure reporting ────────────────────────────────────────────────────────
// A fuzz target asserts through this. It records rather than aborts so one run
// can report several distinct failures instead of dying on the first.
class Report {
public:
    void fail(const ReproKey& key, const std::string& what) {
        ++m_failures;
        std::printf("  FAIL  %s\n        %s\n", key.toString().c_str(),
                    what.c_str());
        if (m_failures == 1) m_first = key;
    }
    int             failures() const { return m_failures; }
    const ReproKey& first()    const { return m_first; }
private:
    int      m_failures = 0;
    ReproKey m_first;
};

// One fuzz case: given a seed, build an input, exercise the target, assert.
// Must be self-contained and leave no global state behind — the runner calls
// it thousands of times in one process.
using CaseFn = std::function<void(uint64_t masterSeed, Report&)>;

// ── Runner ───────────────────────────────────────────────────────────────────
struct Options {
    uint64_t              seed       = 0;      // --seed: run exactly this one
    bool                  haveSeed   = false;
    uint64_t              iterations = 0;      // --iterations: explore
    std::filesystem::path corpus;              // --corpus: replay directory
    bool                  verbose    = false;
};

inline Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](uint64_t& dst) {
            if (i + 1 < argc) dst = std::strtoull(argv[++i], nullptr, 10);
        };
        if      (a == "--seed")       { val(o.seed); o.haveSeed = true; }
        else if (a == "--iterations") { val(o.iterations); }
        else if (a == "--corpus")    { if (i + 1 < argc) o.corpus = argv[++i]; }
        else if (a == "--verbose")   { o.verbose = true; }
    }
    return o;
}

// A corpus file is one decimal seed per line (blank lines and #-comments
// ignored). Seeds, not blobs: a seed is 20 bytes in git and regenerates the
// whole structured input, and the generator version in the file name records
// which generator it was found with.
inline std::vector<uint64_t> loadCorpus(const std::filesystem::path& dir) {
    std::vector<uint64_t> seeds;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return seeds;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".seeds")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());          // stable replay order
    for (const auto& f : files) {
        std::ifstream in(f);
        std::string line;
        while (std::getline(in, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            seeds.push_back(std::strtoull(line.c_str() + b, nullptr, 10));
        }
    }
    return seeds;
}

// Runs the target. Returns a process exit code.
inline int run(const char* targetName, int argc, char** argv, const CaseFn& fn) {
    const Options o = parseArgs(argc, argv);
    Report rep;
    std::printf("=== fuzz:%s ===\n", targetName);

    if (o.haveSeed) {                       // single-case repro
        std::printf("[seed] %llu\n", (unsigned long long)o.seed);
        fn(o.seed, rep);
    } else if (!o.corpus.empty()) {          // REGRESSION lane
        const auto seeds = loadCorpus(o.corpus);
        std::printf("[corpus] %zu case(s) from %s\n", seeds.size(),
                    o.corpus.string().c_str());
        // An empty/missing corpus must not silently pass as "0 failures" —
        // that is how a regression lane quietly stops testing anything.
        if (seeds.empty()) {
            std::printf("  FAIL  corpus is empty or unreadable — the "
                        "regression lane would test nothing\n");
            return 1;
        }
        for (uint64_t s : seeds) {
            if (o.verbose) std::printf("  [replay] %llu\n", (unsigned long long)s);
            fn(s, rep);
        }
    } else {                                 // EXPLORE lane
        const uint64_t iters = o.iterations ? o.iterations : 1000;
        // Base seed is FIXED, not clock-derived: an explore run is itself
        // reproducible end to end. Vary campaigns with --seed-base via the
        // iteration index offset, or just a different --iterations count.
        const uint64_t base = 0xC00C1E5EEDULL;
        std::printf("[explore] %llu iteration(s) from base %llu\n",
                    (unsigned long long)iters, (unsigned long long)base);
        for (uint64_t i = 0; i < iters; ++i) {
            uint64_t x = base + i;
            fn(splitmix64(x), rep);
            if (rep.failures() >= 25) {      // stop spamming; enough to triage
                std::printf("  ... stopping after 25 failures\n");
                break;
            }
        }
    }

    if (rep.failures()) {
        std::printf("\nfuzz:%s FAILED — %d case(s)\n", targetName, rep.failures());
        std::printf("Reproduce:      --seed %llu\n",
                    (unsigned long long)rep.first().masterSeed);
        std::printf("Make permanent: echo %llu >> tests/fuzz/corpus/%s/found.seeds\n",
                    (unsigned long long)rep.first().masterSeed, targetName);
        return 1;
    }
    std::printf("fuzz:%s ALL PASS\n", targetName);
    return 0;
}

// ── Scratch directory helper ─────────────────────────────────────────────────
// Per-process unique so parallel ctest runs never collide, and removed on
// scope exit so a 100k-iteration explore run doesn't fill the disk.
class Scratch {
public:
    explicit Scratch(const char* tag) {
        m_dir = std::filesystem::temp_directory_path()
              / ("engine-fuzz-" + std::string(tag) + "-"
                 + std::to_string((unsigned long long)currentPid()));
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
        std::filesystem::create_directories(m_dir, ec);
    }
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(m_dir, ec); }
    const std::filesystem::path& path() const { return m_dir; }
private:
    std::filesystem::path m_dir;
};

} // namespace fuzz
