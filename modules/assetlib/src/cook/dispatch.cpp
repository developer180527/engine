// ── Execution mode selection ─────────────────────────────────────────────────
// Which way does this cook run: isolated child process, or in-process behind the
// exception net? That decision, and the in-process path itself, are all that is
// here — the child implementations are per-platform files, so neither is
// half-visible to the compiler that cannot build it.
#include "cook/dispatch.h"
#include "cook/dispatch_internal.h"
#include "cook/env.h"
#include "assetlib/cook_result_file.h"
#include "assetlib/ddc.h"   // blake3File — the same hash the DDC keys on

#include <algorithm>
#include <map>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <unistd.h>
#endif

namespace assetlib {

CookResult cookInProcess(ICooker& cooker, const CookContext& ctx) {
    // Exception net: cook() runs third-party parsers (Assimp, stb, json) on
    // corrupt files — a throw escaping a worker std::thread is
    // std::terminate for the whole host process (cooker audit: "Unwrapped
    // Worker Thread Exception Paths"). Convert to a per-asset failure.
    try {
        return cooker.cook(ctx);
    } catch (const std::exception& e) {
        return { .success=false,
                 .error=std::string("cooker threw: ") + e.what() };
    } catch (...) {
        return { .success=false, .error="cooker threw a non-std exception" };
    }
}

// ── ENGINE_COOK_DETERMINISM_CHECK — cook twice, compare, fail on a difference ──
//
// The DDC is content-addressed: a cooked output is named by a hash of its INPUTS,
// so if a cooker is not a pure function of those inputs the cache serves bytes
// that recooking would not reproduce. Two machines then disagree about the same
// asset, and the cache has quietly stopped being a cache.
//
// We shipped exactly that bug and found it by READING: the decimator computed
// `(int32_t)std::floor(NaN)` for a non-finite vertex, which is INT_MIN on x86-64
// and 0 on arm64, so two machines cooked different LOD levels from one mesh. A
// check that cooks twice and compares would have caught it with nobody looking,
// and will catch the next one — hash-map iteration order, uninitialised padding,
// a timestamp, a path leaking into output.
//
// An ENVIRONMENT VARIABLE rather than a build flag, deliberately, and copied from
// vCAD's CAD_PLUGIN_DETERMINISM_CHECK: it can be turned on against a build that
// already exists, on the day an artist says an asset "looks different on the build
// machine". A build flag would need the thing rebuilt before it could be
// investigated. Off by default because it doubles every cook.
//
// The comparison is on the OUTPUT BYTES, not on some internal notion of equality,
// because bytes are what the DDC stores and what another machine receives. Extra
// outputs are compared too — the mesh cooker writes sibling .ctex blobs, and a
// cooker that is deterministic in its primary output and not in its siblings is
// still broken. Recorded dependencies are compared for the same reason: they feed
// the next cook's key.
namespace {

bool determinismCheckEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("ENGINE_COOK_DETERMINISM_CHECK");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

// {basename -> digest} rather than {absolute path -> digest}: the second cook
// writes into a different directory on purpose, so absolute paths cannot match
// and comparing them would report every asset as non-deterministic.
std::map<std::string, std::string> digestOutputs(
        const std::filesystem::path& primary,
        const std::vector<std::filesystem::path>& extras) {
    std::map<std::string, std::string> out;
    out["<primary>"] = blake3File(primary);
    for (const auto& e : extras) out[e.filename().string()] = blake3File(e);
    return out;
}

std::string describeDifference(const std::map<std::string, std::string>& a,
                               const std::map<std::string, std::string>& b) {
    for (const auto& [name, dig] : a) {
        auto it = b.find(name);
        if (it == b.end()) return "second cook did not produce '" + name + "'";
        if (it->second != dig)
            return "'" + name + "' differs (" + dig.substr(0, 12) + " vs "
                 + it->second.substr(0, 12) + ")";
    }
    for (const auto& [name, dig] : b) {
        (void)dig;
        if (a.find(name) == a.end())
            return "second cook produced an extra file '" + name + "'";
    }
    return {};
}

} // namespace

CookResult dispatchCook(const std::filesystem::path& workerExe,
                        ICooker& cooker, const CookContext& ctx,
                        const CancelFn& isCancelled) {
    // Never start work the caller has already given up on.
    if (isCancelled && isCancelled())
        return { .success=false, .cancelled=true, .error="cook cancelled" };

    auto runOnce = [&](const CookContext& c) {
        return workerExe.empty() ? cookInProcess(cooker, c)
                                 : cookInWorkerProcess(workerExe, cooker, c, isCancelled);
    };

    // Collect the first cook's extra outputs and dependencies so both halves can
    // be compared. The caller's own sinks still receive everything.
    std::vector<std::filesystem::path> extras1;
    std::vector<UUID>                  deps1;
    CookContext c1 = ctx;
    c1.addOutput = [&](const std::filesystem::path& p) {
        extras1.push_back(p);
        if (ctx.addOutput) ctx.addOutput(p);
    };
    c1.addDependency = [&](const UUID& d) {
        deps1.push_back(d);
        if (ctx.addDependency) ctx.addDependency(d);
    };

    CookResult first = runOnce(c1);
    if (!determinismCheckEnabled() || !first.success) return first;

    // ── The second cook, into a directory of its own ────────────────────────
    std::error_code ec;
    const auto scratch = ctx.outputPath.parent_path()
                       / ("determinism_" + ctx.uuid.toString());
    std::filesystem::create_directories(scratch, ec);
    if (ec) {
        std::fprintf(stderr, "[Determinism] cannot create scratch dir — check "
                             "skipped for %s\n", ctx.sourcePath.string().c_str());
        return first;
    }

    std::vector<std::filesystem::path> extras2;
    std::vector<UUID>                  deps2;
    CookContext c2   = ctx;
    c2.outputPath    = scratch / ctx.outputPath.filename();
    // NOT forwarded to the caller: this cook is an experiment, and registering
    // its outputs would make the pipeline ship the scratch copy.
    c2.addOutput     = [&](const std::filesystem::path& p) { extras2.push_back(p); };
    c2.addDependency = [&](const UUID& d) { deps2.push_back(d); };

    CookResult second = runOnce(c2);

    std::string problem;
    if (!second.success) {
        problem = "the second cook FAILED where the first succeeded ("
                + second.error + ")";
    } else {
        problem = describeDifference(digestOutputs(ctx.outputPath, extras1),
                                     digestOutputs(c2.outputPath, extras2));
        if (problem.empty() && deps1 != deps2)
            problem = "the two cooks recorded different dependencies ("
                    + std::to_string(deps1.size()) + " vs "
                    + std::to_string(deps2.size()) + ") — the DDC key is built "
                      "from these, so the next cook keys differently";
    }
    std::filesystem::remove_all(scratch, ec);

    if (problem.empty()) return first;

    // FAILS THE COOK. A warning would be read as advisory and a non-deterministic
    // cooker poisons a shared cache for everyone who pulls from it — the whole
    // reason to look is that the damage is not local.
    const std::string msg =
        "NON-DETERMINISTIC COOK: " + ctx.sourcePath.filename().string() + " — "
        + problem + ". Identical inputs must produce identical bytes: the DDC "
          "names an output by a hash of its inputs, so a cooker that varies makes "
          "the cache serve what recooking would not reproduce. Look for "
          "uninitialised padding, hash-map iteration order, a timestamp, an "
          "absolute path in the output, or undefined float conversions.";
    std::fprintf(stderr, "[Determinism] %s\n", msg.c_str());
    return { .success=false, .error=msg };
}

} // namespace assetlib
