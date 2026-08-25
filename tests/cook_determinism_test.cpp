// ── cook_determinism_test — the check that catches a poisoned cache ──────────
//
// The DDC names a cooked output by a hash of its INPUTS. So a cooker that is not
// a pure function of those inputs makes the cache serve bytes that recooking
// would not reproduce: two machines disagree about one asset, and the disagreement
// travels to everyone who pulls from the shared store.
//
// We shipped exactly that and found it by READING code, not by testing: the
// decimator computed `(int32_t)std::floor(NaN)` for a non-finite vertex — INT_MIN
// on x86-64, 0 on arm64 — so two machines cooked different LOD levels from one
// mesh. `ENGINE_COOK_DETERMINISM_CHECK=1` cooks twice and compares, which would
// have caught it with nobody looking.
//
// This file's job is to prove the CHECK catches things, in each of the ways a
// cooker can drift, because a determinism check that only ever passes is
// indistinguishable from one that does nothing:
//
//   * the primary output differs
//   * the primary matches but a SIBLING output differs (the mesh cooker writes
//     .ctex blobs; deterministic in one and not the other is still broken)
//   * the recorded DEPENDENCIES differ (they feed the next cook's key)
//   * the second cook fails where the first succeeded
//
// Two ctest entries run this binary: one with the check on, one with it off. The
// off case matters as much — a check that fires when disabled would double every
// cook in every build, and the flag would be turned off again and stay off.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <assetlib/cooker.h>
#include "cook/dispatch.h"          // internal header — this tests the choke point
#include "test_env.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

using namespace assetlib;

static void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// ── Fake cookers, one per way of being wrong ────────────────────────────────

// The control. Output is a pure function of the source path's contents.
struct PureCooker : ICooker {
    const char* id() const override { return "pure"; }
    uint32_t version() const override { return 1; }
    std::vector<std::string> extensions() const override { return { ".src" }; }
    CookResult cook(const CookContext& ctx) override {
        std::ifstream in(ctx.sourcePath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), {});
        writeFile(ctx.outputPath, "cooked:" + body);
        return { .success = true };
    }
};

// A counter in the output. Stands in for a timestamp, a random seed, a session
// id — anything that varies between two runs of the same build.
struct DriftingCooker : ICooker {
    int runs = 0;
    const char* id() const override { return "drift"; }
    uint32_t version() const override { return 1; }
    std::vector<std::string> extensions() const override { return { ".src" }; }
    CookResult cook(const CookContext& ctx) override {
        writeFile(ctx.outputPath, "run=" + std::to_string(++runs));
        return { .success = true };
    }
};

// Primary output identical, SIBLING different. This is the shape the mesh cooker
// has (embedded textures as sibling .ctex blobs), and the one a naive check that
// only hashes outputPath would wave through.
struct DriftingSiblingCooker : ICooker {
    int runs = 0;
    const char* id() const override { return "sibling"; }
    uint32_t version() const override { return 1; }
    std::vector<std::string> extensions() const override { return { ".src" }; }
    CookResult cook(const CookContext& ctx) override {
        writeFile(ctx.outputPath, "stable primary");
        const fs::path sib = ctx.outputPath.parent_path() / "sibling.blob";
        writeFile(sib, "run=" + std::to_string(++runs));
        if (ctx.addOutput) ctx.addOutput(sib);
        return { .success = true };
    }
};

// Same bytes, different declared dependencies. Those feed the NEXT cook's key,
// so a cooker that varies here makes staleness itself non-deterministic.
struct DriftingDepsCooker : ICooker {
    int runs = 0;
    const char* id() const override { return "deps"; }
    uint32_t version() const override { return 1; }
    std::vector<std::string> extensions() const override { return { ".src" }; }
    CookResult cook(const CookContext& ctx) override {
        writeFile(ctx.outputPath, "stable");
        if (ctx.addDependency && ++runs == 1) ctx.addDependency(UUID::generate());
        return { .success = true };
    }
};

// Succeeds once, then fails. A cooker holding one-shot state looks fine in a
// clean build and breaks the moment anything cooks twice.
struct FlakyCooker : ICooker {
    int runs = 0;
    const char* id() const override { return "flaky"; }
    uint32_t version() const override { return 1; }
    std::vector<std::string> extensions() const override { return { ".src" }; }
    CookResult cook(const CookContext& ctx) override {
        if (++runs > 1) return { .success = false, .error = "one-shot state consumed" };
        writeFile(ctx.outputPath, "first run only");
        return { .success = true };
    }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool checkOn = !(argc > 1 && std::strcmp(argv[1], "--off") == 0);

    // Set BEFORE the first dispatchCook: the flag is read once and cached, so a
    // process tests one mode. Hence two ctest entries rather than one.
    if (checkOn) testenv::set("ENGINE_COOK_DETERMINISM_CHECK", "1");
    else         testenv::unset("ENGINE_COOK_DETERMINISM_CHECK");

    std::printf("cook_determinism_test: check %s\n", checkOn ? "ON" : "OFF");

    // PER-MODE directory. Both ctest entries run this binary, ctest runs them in
    // PARALLEL, and a shared path meant one process's remove_all raced the
    // other's writes — the test passed alone and failed under -j8, which is the
    // exact flake that teaches people to re-run a suite instead of reading it.
    const fs::path root = fs::temp_directory_path()
                        / (std::string("engine_cook_determinism_")
                           + (checkOn ? "on" : "off"));
    fs::remove_all(root);
    const fs::path src = root / "in.src";
    writeFile(src, "hello source");

    int caseNo = 0;
    auto run = [&](ICooker& cooker) {
        CookContext ctx;
        ctx.uuid       = UUID::generate();
        ctx.sourcePath = src;
        ctx.outputPath = root / ("out" + std::to_string(++caseNo)) / "cooked.bin";
        fs::create_directories(ctx.outputPath.parent_path());
        // Empty workerExe = cook in-process, so this needs no worker binary.
        return dispatchCook({}, cooker, ctx, {});
    };

    // ── The control: a pure cooker must pass either way ─────────────────────
    {
        PureCooker c;
        CookResult r = run(c);
        CHECK(r.success, "a deterministic cooker succeeds with the check %s (%s)",
              checkOn ? "on" : "off", r.error.c_str());
    }

    // ── The four ways to drift ──────────────────────────────────────────────
    {
        DriftingCooker c;
        CookResult r = run(c);
        if (checkOn) {
            CHECK(!r.success, "a varying PRIMARY output is caught");
            CHECK(r.error.find("NON-DETERMINISTIC") != std::string::npos &&
                  r.error.find("<primary>") != std::string::npos,
                  "...and the message names what differed ('%s')",
                  r.error.substr(0, 90).c_str());
        } else {
            CHECK(r.success,
                  "with the check OFF the same cooker sails through — which is "
                  "what the check exists to stop, and why it must not be the "
                  "default");
            CHECK(c.runs == 1, "and the cooker ran ONCE, not twice (%d)", c.runs);
        }
    }
    {
        DriftingSiblingCooker c;
        CookResult r = run(c);
        if (checkOn) {
            CHECK(!r.success,
                  "a varying SIBLING output is caught even though the primary matches");
            CHECK(r.error.find("sibling.blob") != std::string::npos,
                  "...naming the sibling ('%s')", r.error.substr(0, 90).c_str());
        } else {
            CHECK(r.success, "and passes with the check off");
        }
    }
    {
        DriftingDepsCooker c;
        CookResult r = run(c);
        if (checkOn) {
            CHECK(!r.success,
                  "differing DEPENDENCIES are caught — they feed the next key");
            CHECK(r.error.find("dependencies") != std::string::npos,
                  "...and say so ('%s')", r.error.substr(0, 90).c_str());
        } else {
            CHECK(r.success, "and passes with the check off");
        }
    }
    {
        FlakyCooker c;
        CookResult r = run(c);
        if (checkOn) {
            CHECK(!r.success, "a cooker that fails on the SECOND run is caught");
            CHECK(r.error.find("second cook FAILED") != std::string::npos,
                  "...distinguished from a byte difference ('%s')",
                  r.error.substr(0, 90).c_str());
        } else {
            CHECK(r.success, "and passes with the check off");
        }
    }

    // ── The scratch directory must not survive ──────────────────────────────
    // The second cook writes into a directory of its own; leaving it behind would
    // put an unreferenced copy of every cooked asset in the project's .cache.
    if (checkOn) {
        bool leftover = false;
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_directory() &&
                e.path().filename().string().rfind("determinism_", 0) == 0)
                leftover = true;
        CHECK(!leftover, "the second cook's scratch directory is cleaned up");
    }

    fs::remove_all(root);
    if (g_failures) {
        std::printf("\ncook_determinism_test: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\ncook_determinism_test: PASS\n");
    return 0;
}
