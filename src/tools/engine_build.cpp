// ── engine_build — package a project into a shippable dist/ ─────────────────
//
//   engine_build <project-dir> [--dist <dir>]        (default <project>/dist)
//
// The export pipeline (backlog Phase G #29), end to end:
//   1. COOK       engine_cook — every asset + scene to cooked binaries
//   2. PLAYER     configure + build the SHIPPING engine tree
//                 (ENGINE_WITH_SOURCE_IMPORTERS=OFF, Release): a player with
//                 zero Assimp linked — the runtime never parses FBX
//   3. KITS       rebuild every manifest kit + the game module in Release
//                 against the same posture. Kit binaries are PER-CONFIG
//                 artifacts: a Debug kit references debug-only engine-side
//                 symbols (ecs_assert_log_ et al) that a Release player
//                 doesn't carry — mixing configs fails at dlopen, verified.
//   4. ASSEMBLE   dist/: player + kits/ + cooked .cache/ + input.json +
//                 a project.json rewritten to point at the shipped kits +
//                 assets/ MINUS source-mesh formats (cooked already) + run.sh
//
// Dev-posture tool: lives in the dev tree, shells out to cmake.
//
// ── Add-on protocol: why this tool speaks it ─────────────────────────────────
// This is the editor's fifth job — export — and the editor cannot drive it, for
// a reason worth stating precisely rather than as "it needs an API".
//
// This tool has TWELVE warning sites, and every one of them describes a defect
// in the SHIPPED GAME: an unreadable cooked scene whose every object is missing,
// a scene referencing a mesh that is not there, a mesh referencing a texture
// that is not there, two shaders both claiming one name, no shader providing
// "standard" so the player silently falls back to compiled-in shading, material
// textures that resolve to nothing and ship white.
//
// All twelve went to stderr, and the process exited 0.
//
// That is the silently-untextured build — the exact failure mode
// engine_cook_worker's result trailer was written to stop — arriving through the
// packager instead of the IPC channel. A caller could not see it: not the editor,
// not CI, not a build farm. Only a human reading scrollback, who has no reason to
// scroll back because the exit code said the package was fine.
//
// So the warnings are now RECORDS in a framed result file, and there is a
// separate `WARNINGS <n>` count. Two consequences worth being explicit about:
//
//   * THE TOOL REPORTS; THE CALLER DECIDES. A warning does not fail the build by
//     itself, because whether a missing texture blocks a ship is a policy
//     question and this tool is not where policy lives. `--strict` is how a
//     caller that wants them fatal says so, and CI is expected to pass it.
//   * EXIT 1 IS GONE. It used to mean fifteen different things here, and the
//     protocol reserves 1 for "unknown, treat as a crash" precisely because it is
//     the exit code of every accident. A failed kit build and a segfault in cmake
//     were indistinguishable; now they are not.
#include "tools/packaging/package_closure.h"

#include <engine/addon_protocol.h>

#include <assetlib/asset_registry.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <algorithm>
#include <cstdint>
#if defined(__APPLE__)
  #include <sys/sysctl.h>
#elif defined(__linux__)
  #include <unistd.h>
#endif

#include <vector>

#include <nlohmann/json.hpp>
#include <assetlib/scene_asset.h>

namespace fs = std::filesystem;

#ifndef ENGINE_SOURCE_ROOT
#error "ENGINE_SOURCE_ROOT must be defined by the build"
#endif

static int run(const std::string& cmd) {
    std::printf("[engine_build] $ %s\n", cmd.c_str());
    return std::system(cmd.c_str());
}
static std::string q(const fs::path& p) { return "\"" + p.string() + "\""; }

// ── The report ──────────────────────────────────────────────────────────────
namespace addon = engine::addon;

static addon::Result g_report;
static std::string   g_resultPath;      // empty: no caller asked for a result
static int           g_warnings = 0;
static bool          g_strict   = false;

// A warning: printed for a human AND recorded for a caller.
//
// Every call site below used to be a bare fprintf to stderr that no caller could
// observe, on a run that then exited 0. The stderr line is kept verbatim — it is
// the human channel and this tool is still mostly run by hand — and the record is
// what makes it visible to the editor, to CI, and to a build farm.
static void warn(const std::string& msg) {
    std::fprintf(stderr, "[engine_build] WARNING: %s\n", msg.c_str());
    g_report.record("WARNING", msg);
    ++g_warnings;
}

// A path the caller is expected to OPEN, so it must survive exactly or not at
// all. `recordExact` refuses control characters rather than replacing them,
// because a mangled path is a file the caller cannot find and a silent wrong
// answer beats nothing at all only in the mind of the tool that wrote it.
// Returns false so the caller can fail the run naming the offending path.
[[nodiscard]] static bool recordPath(const char* key, const fs::path& p) {
    if (g_report.recordExact(key, p.string())) return true;
    std::fprintf(stderr, "[engine_build] path cannot be reported: %s\n",
                 p.string().c_str());
    return false;
}

// Every exit from main after argument parsing goes through here, so a run that
// RAN always leaves a readable result and a run that did not never leaves a
// stale one.
static int finish(addon::Verdict v, const std::string& err = {}) {
    std::string error = err;

    // Promotion, not detection: the warnings were already recorded. `--strict`
    // only decides whether they are fatal, because whether a missing texture
    // blocks a ship is policy and this tool is not where policy lives.
    if (g_strict && g_warnings > 0 && v == addon::Verdict::Ok) {
        v     = addon::Verdict::Fail;
        error = std::to_string(g_warnings)
              + " warning(s) and --strict was given; each one is a defect in the "
                "shipped package";
    }
    g_report.verdict(v);
    if (!error.empty()) g_report.error(error);
    g_report.record("WARNINGS", std::to_string(g_warnings));

    if (!g_resultPath.empty()) {
        std::string werr;
        if (!g_report.writeTo(g_resultPath, werr)) {
            // The tool finished and cannot report it. That is the tool failing,
            // not a verdict about the project — and there is by definition no
            // result file to say so in.
            std::fprintf(stderr, "engine_build: %s\n", werr.c_str());
            return addon::exitCode(addon::Exit::Failed);
        }
    }
    if (v == addon::Verdict::Fail) {
        if (error.empty())
            std::fprintf(stderr, "engine_build: FAILED\n");
        else
            std::fprintf(stderr, "engine_build: FAILED — %s\n", error.c_str());
        return addon::exitCode(addon::Exit::RanWithFailure);
    }
    return addon::exitCode(addon::Exit::Ran);
}

static int failed(const std::string& why) {
    return finish(addon::Verdict::Fail, why);
}

// ── Bounded build parallelism ────────────────────────────────────────────────
// A BARE `-j` (what this used to pass) means UNLIMITED jobs to make: one
// compiler process per ready target, all at once. Packaging configures a fresh
// ship tree, so `--target engine_player` compiles the whole engine — Assimp,
// bgfx, Jolt, ozz, hundreds of translation units. At roughly 0.5–2 GB per heavy
// C++ TU that exhausted 24 GB of RAM and froze the machine outright.
//
// Same policy the cooker already follows (docs/architecture/asset-cook-architecture.md §4.3,
// "an offline chore is a background chore"): leave the OS and the developer's
// editor a couple of cores, and stay explicit about it. Packaging had simply
// never been held to the rule the cook was.
//
// ENGINE_BUILD_JOBS overrides for a build farm, where saturating the box IS
// the goal.
// Bounded by cores AND by RAM. Cores alone is not enough: a compiler process
// on a heavy templated TU peaks well past a gigabyte, so the limit that
// actually matters is memory. ~2 GB per job is deliberately pessimistic —
// this is the mirror of the cooker's memory-budget admission, and for the same
// reason (an estimate that is too low costs time; too high costs the machine).
//
// The formula must also be right on the low end: a 4-core / 4 GB box gets 2
// jobs, not 2 because of cores but because of RAM.
static std::string jobFlag() {
    unsigned byCores = std::thread::hardware_concurrency();
    if (byCores == 0) byCores = 2;
    byCores = (byCores > 2) ? byCores - 2 : 1;      // leave the OS + editor room

    unsigned byMem = byCores;
#if defined(__APPLE__) || defined(__linux__)
    uint64_t ramBytes = 0;
  #if defined(__APPLE__)
    size_t len = sizeof(ramBytes);
    if (sysctlbyname("hw.memsize", &ramBytes, &len, nullptr, 0) != 0) ramBytes = 0;
  #else
    const long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psz > 0) ramBytes = (uint64_t)pages * (uint64_t)psz;
  #endif
    if (ramBytes) {
        // 60% of RAM, not all of it — the same fraction the cooker's
        // COOK_MEM_BUDGET_MB defaults to, and for the same reason: the OS, the
        // editor and a browser are still running. Budgeting 100% of physical
        // memory is how a "safe" limit still swaps the machine to death.
        const uint64_t usable = ramBytes * 3 / 5;
        const uint64_t perJob = 2ull * 1024 * 1024 * 1024;   // 2 GB per compile
        byMem = (unsigned)std::max<uint64_t>(1, usable / perJob);
    }
#endif

    unsigned n = std::min(byCores, byMem);
    if (const char* env = std::getenv("ENGINE_BUILD_JOBS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0) n = (unsigned)v;   // build farms: saturating the box IS the goal
    }
    std::printf("[engine_build] build parallelism: -j %u "
                "(cores allow %u, RAM allows %u)\n", n, byCores, byMem);
    return " -j " + std::to_string(n);
}

// Runtime-consumed asset formats — the WHITELIST. Everything else in
// assets/ is authoring-side input (source meshes, source textures for
// uncooked formats, PSDs…) that the cooked pipeline already digested;
// shipping it is pure bloat and the shipping player couldn't read the mesh
// formats anyway (no importers linked).
static bool isRuntimeAsset(const fs::path& p) {
    static const std::set<std::string> kExt = {
        ".wav", ".mp3", ".ogg", ".flac",   // audio streams from source
        ".lua",                            // scripts
    };
    std::string e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return kExt.count(e) > 0;
}

static bool copyFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "[engine_build] copy failed: %s (%s)\n",
                     from.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// Reference walk: parse every cooked scene, collect the cooked meshes it
// actually uses, and expand each to its sibling files (<uuid>_tN.ctex
// embedded-texture extractions share the mesh's uuid stem). Only THAT
// closure ships — fps_shooter's un-walked .cache was 3.1 GB of cooked
// textures and stale meshes nothing referenced.

// What this tool claims about itself, for a host that has not been taught this
// particular binary. The RECORD lines are the vocabulary a caller may parse; the
// conformance test cross-checks them against what a real run emits, because a
// manifest nothing cross-checks is decorative.
static void emitManifest() {
    addon::Result m;
    m.verdict(addon::Verdict::Ok);
    m.record("ID",       "engine_build");
    m.record("TOOL",     "1");
    m.record("RECORD",   "WARNING");
    m.record("RECORD",   "WARNINGS");
    m.record("RECORD",   "DIST");
    m.record("RECORD",   "BYTES");
    m.record("RECORD",   "STEP");
    m.record("CONSUMES", "engine-project");
    m.record("PRODUCES", "shippable-dist");
    addon::writeManifest(m);
}

static int usage() {
    std::fprintf(stderr,
        "usage: engine_build <project-dir> [--dist <dir>] [options]\n"
        "       engine_build --addon-manifest\n"
        "\n"
        "  --dist <dir>            where to assemble the package\n"
        "  --addon-result=<path>   write the machine-readable result there\n"
        "  --strict                make warnings fatal. Every warning this tool\n"
        "                          emits is a defect in the shipped package, so CI\n"
        "                          is expected to pass this; a human iterating is\n"
        "                          not.\n"
        "  --addon-manifest        print this tool's Add-on manifest and exit\n");
    return addon::exitCode(addon::Exit::Usage);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string projectArg, distArg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--addon-manifest") { emitManifest(); return addon::exitCode(addon::Exit::Ran); }
        if (a == "--strict")         { g_strict = true; continue; }
        if (a.rfind("--addon-result=", 0) == 0) {
            g_resultPath = a.substr(15);
            if (g_resultPath.empty()) {
                std::fprintf(stderr, "engine_build: --addon-result= needs a path\n");
                return usage();
            }
            continue;
        }
        if (a == "--dist") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "engine_build: --dist needs a directory\n");
                return usage();
            }
            distArg = argv[++i];
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "engine_build: unknown option '%s'\n", a.c_str());
            return usage();
        }
        if (projectArg.empty()) projectArg = a;
        else {
            std::fprintf(stderr, "engine_build: unexpected argument '%s'\n", a.c_str());
            return usage();
        }
    }
    if (projectArg.empty()) return usage();

    const fs::path project = fs::absolute(projectArg);
    fs::path dist = distArg.empty() ? project / "dist" : fs::absolute(distArg);

    // Removed before any work, so a result file can never outlive the run that
    // wrote it. A stale success is the one failure the digest trailer cannot
    // catch, because the file it describes is perfectly intact.
    if (!g_resultPath.empty()) {
        std::error_code rc;
        fs::remove(g_resultPath, rc);
    }

    const fs::path engineRoot = ENGINE_SOURCE_ROOT;
    const fs::path shipTree   = engineRoot / "build-ship";
    const fs::path selfDir    = fs::absolute(argv[0]).parent_path(); // dev build dir

    // MissingInput, not Usage: the command line was fine, the named input was
    // not there. A caller can act on that difference — one is a bug in the
    // caller, the other is a bug in the project.
    if (!fs::exists(project / "project.json")) {
        std::fprintf(stderr, "engine_build: no project.json at %s\n",
                     project.string().c_str());
        return addon::exitCode(addon::Exit::MissingInput);
    }

    std::printf("[engine_build] project: %s\n[engine_build] dist:    %s\n",
                project.string().c_str(), dist.string().c_str());

    // ── 1. Cook everything ────────────────────────────────────────────────
    if (run(q(selfDir / "engine_cook") + " " + q(project)) != 0)
        return failed("cook failed — fix assets first");
    g_report.record("STEP", "cook ok");

    // ── 2. Shipping player (Assimp-free posture, Release) ────────────────
    if (run("cmake -S " + q(engineRoot) + " -B " + q(shipTree) +
            " -DENGINE_WITH_SOURCE_IMPORTERS=OFF -DCMAKE_BUILD_TYPE=Release") != 0)
        return failed("configuring the shipping tree failed");
    if (run("cmake --build " + q(shipTree) + " --target engine_player" + jobFlag()) != 0)
        return failed("building engine_player failed");
    g_report.record("STEP", "player ok");

    // ── 3. Kits + game module, rebuilt Release ────────────────────────────
    nlohmann::json proj;
    try {
        std::ifstream f(project / "project.json");
        proj = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        return failed(std::string("bad project.json: ") + e.what());
    }

    std::error_code ec;
    fs::remove_all(dist, ec);
    fs::create_directories(dist / "kits");

    // NB: iterate proj["kits"] DIRECTLY — json::value() returns a copy, and
    // the module-path rewrite below must land in the document we ship.
    if (!proj.contains("kits")) proj["kits"] = nlohmann::json::array();
    for (auto& kit : proj["kits"]) {
        if (!kit.value("enabled", true)) continue;
        const std::string name   = kit.value("name", "?");
        const fs::path    module = kit.value("module", std::string{});
        // Layout convention: <kit-src>/build/lib<x>.so — the source dir is
        // two levels up from the module binary.
        const fs::path srcDir = fs::weakly_canonical(
            project / module.parent_path().parent_path());
        const fs::path kitBuild = dist / ".kitbuild" / name;
        if (!fs::exists(srcDir / "CMakeLists.txt")) {
            return failed("kit '" + name + "': no CMakeLists at "
                          + srcDir.string());
        }
        if (run("cmake -S " + q(srcDir) + " -B " + q(kitBuild) +
                " -DCMAKE_BUILD_TYPE=Release -DENGINE_ROOT=" + q(engineRoot)) != 0)
            return failed("kit '" + name + "': configure failed");
        if (run("cmake --build " + q(kitBuild) + jobFlag()) != 0)
            return failed("kit '" + name + "': build failed");

        const fs::path shipped = dist / "kits" / module.filename();
        if (!fs::copy_file(kitBuild / module.filename(), shipped,
                           fs::copy_options::overwrite_existing, ec) || ec) {
            return failed("kit '" + name + "': built module not found at "
                          + (kitBuild / module.filename()).string());
        }
        kit["module"] = (fs::path("kits") / module.filename()).generic_string();
        std::printf("[engine_build] kit '%s' -> %s (Release)\n",
                    name.c_str(), shipped.filename().string().c_str());
    }
    fs::remove_all(dist / ".kitbuild", ec);

    // ── 4. Assemble ───────────────────────────────────────────────────────
    fs::copy_file(shipTree / "engine_player", dist / "engine_player",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return failed("player copy failed: " + ec.message());
    g_report.record("STEP", "kits ok");

    // Cooked content: scenes + the reference-walked mesh closure + anim
    // clips. NOT the registry (editor-only) and NOT .cache/textures (no
    // runtime consumer path yet — cooked standalone textures are dead
    // weight until the material system binds them).
    const fs::path cache = project / ".cache";
    for (auto& e : fs::directory_iterator(cache / "scenes", ec))
        if (e.path().extension() == ".cooked")
            if (!copyFile(e.path(), dist / ".cache" / "scenes"
                                         / e.path().filename()))
                return failed("copying a cooked scene failed");

    const auto sceneRefs = pkg::sceneMeshRefs(cache / "scenes");
    for (const std::string& bad : sceneRefs.unreadableScenes)
        warn("unreadable cooked scene " + bad
             + " — every object in it is MISSING from the package");
    if (sceneRefs.scenesRead == 0)
        warn("no cooked scenes read — the package has no content");
    const auto& meshRefs = sceneRefs.meshes;
    size_t shippedMeshFiles = 0;
    for (const std::string& rel : meshRefs) {               // "meshs/<uuid>.cooked"
        const fs::path cooked = cache / rel;
        if (!fs::exists(cooked)) {
            warn("scene references missing cooked mesh: " + rel);
            continue;
        }
        // Ship what the mesh REFERENCES, never what its filename suggests —
        // src/tools/packaging/package_closure.h records the bug that rule encodes,
        // and tests/package_closure_test.cpp keeps it from coming back.
        const auto closure = pkg::meshClosure(cooked);
        if (closure.unreadable) {
            warn("cannot read cooked mesh " + rel
                 + " — it and its textures will not ship");
            continue;
        }
        for (const std::string& miss : closure.missing)
            warn("mesh " + rel + " references missing texture " + miss
                 + " — it will render untextured");
        for (const fs::path& f : closure.files) {
            const fs::path dst = dist / ".cache" / "meshs" / f.filename();
            if (fs::exists(dst, ec)) continue;   // shared between meshes
            if (!copyFile(f, dst)) return failed("copying a mesh file failed");
            ++shippedMeshFiles;
        }
    }
    // Cooked shaders. The runtime resolves these BY NAME from .cache/shaders
    // (a dist has no registry), so the whole directory ships — it is a handful
    // of KB, and the closed-feature rule keeps it that way. Without this the
    // player silently falls back to compiled-in shaders and a project's custom
    // shading never runs in the shipped game.
    const auto shaders = pkg::shaderFiles(cache / "shaders");
    for (const std::string& bad : shaders.unreadable)
        warn("unreadable cooked shader " + bad
             + " — the shader it provides will be missing");
    for (const std::string& dup : shaders.duplicateNames)
        warn("two cooked shaders both declare \"" + dup
             + "\" — one shadows the other (stale cook?)");
    for (const fs::path& f : shaders.files)
        if (!copyFile(f, dist / ".cache" / "shaders" / f.filename()))
            return failed("copying a cooked shader failed");

    // Name-level, not file-level. ForwardPipeline asks for "standard" by name;
    // without it the player falls back to compiled-in shaders and renders
    // CORRECTLY, so nothing looks wrong while the project's shading never runs.
    if (!shaders.provides("standard"))
        warn("no cooked shader declares \"standard\" — the player will fall back "
             "to compiled-in shaders and custom shading will not run");
    else
        std::printf("[engine_build] cooked shaders: %zu file(s) providing %zu "
                    "name(s)\n", shaders.files.size(), shaders.names.size());

    // Cooked materials. Shipped wholesale for the same reason as shaders: a
    // game names them at runtime, and nothing in a scene references them yet,
    // so there is no closure to walk. A .cmat is a few hundred bytes.
    const auto materials = pkg::materialFiles(cache / "materials");
    for (const std::string& bad : materials.unreadable)
        warn("unreadable cooked material " + bad
             + " — the look it provides will be missing");
    for (const std::string& dup : materials.duplicateNames)
        warn("two cooked materials both declare \"" + dup
             + "\" — one shadows the other (stale cook?)");
    // Resolve each material's textures BEFORE copying: the resolver rewrites
    // the .cmat in the cache with a cache-relative cooked path, and the dist
    // must receive the rewritten file. A dist has no registry.db, so a source
    // path is unresolvable there — without this every textured material in the
    // shipped game binds its white fallback.
    {
        assetlib::AssetRegistry reg;
        if (reg.open(cache / "registry.db")) {
            const auto texSet = pkg::resolveMaterialTextures(
                materials, reg, cache, dist / ".cache" / "materials");
            for (const std::string& rel : texSet.cookedRel)
                if (!copyFile(cache / rel, dist / ".cache" / rel))
                    return failed("copying a material texture failed");
            for (const std::string& bad : texSet.unresolved)
                warn("material texture " + bad
                     + " has no cooked output — it will ship UNTEXTURED");
            if (!texSet.cookedRel.empty())
                std::printf("[engine_build] material textures: %zu file(s)\n",
                            texSet.cookedRel.size());
        } else {
            warn("no registry at " + (cache / "registry.db").string()
                 + " — material textures cannot be resolved and will ship "
                   "UNTEXTURED");
        }
    }

    // NOT copied here: resolveMaterialTextures above already placed each .cmat
    // in the package with its cooked texture references filled in. Copying
    // again would overwrite them with the unresolved originals.
    if (!materials.files.empty())
        std::printf("[engine_build] cooked materials: %zu file(s) providing "
                    "%zu name(s)\n", materials.files.size(),
                    materials.names.size());

    if (fs::exists(cache / "anim"))
        for (auto& e : fs::directory_iterator(cache / "anim", ec))
            if (!copyFile(e.path(), dist / ".cache" / "anim"
                                         / e.path().filename()))
                return failed("copying an anim clip failed");
    std::printf("[engine_build] cooked closure: %zu scene mesh ref(s), "
                "%zu mesh file(s)\n", meshRefs.size(), shippedMeshFiles);

    // assets/: runtime-consumed formats only (audio, lua) — see whitelist.
    for (auto it = fs::recursive_directory_iterator(project / "assets", ec);
         it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file() && isRuntimeAsset(it->path())) {
            const fs::path rel = fs::relative(it->path(), project, ec);
            if (!copyFile(it->path(), dist / rel))
                return failed("copying a runtime asset failed");
        }

    if (fs::exists(project / "input.json"))
        fs::copy_file(project / "input.json", dist / "input.json",
                      fs::copy_options::overwrite_existing, ec);

    { std::ofstream f(dist / "project.json"); f << proj.dump(2) << "\n"; }
    {
        std::ofstream f(dist / "run.sh");
        f << "#!/bin/sh\ncd \"$(dirname \"$0\")\"\nexec ./engine_player .\n";
    }
    fs::permissions(dist / "run.sh",
                    fs::perms::owner_all | fs::perms::group_read |
                    fs::perms::group_exec | fs::perms::others_read |
                    fs::perms::others_exec, ec);

    // Size report — the whole point of the exercise.
    uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dist, ec);
         it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file()) total += it->file_size(ec);
    std::printf("[engine_build] DONE — %s (%.1f MB)\n  run it: %s/run.sh\n",
                dist.string().c_str(), total / (1024.0 * 1024.0),
                dist.string().c_str());

    // The dist path is what a caller does something WITH — runs it, uploads it,
    // signs it — so it must arrive exactly or not at all. A path this format
    // cannot carry is a failed package, not a package with a mangled address.
    if (!recordPath("DIST", dist))
        return failed("the dist path contains characters this result format "
                      "cannot carry: " + dist.string());
    g_report.record("BYTES", std::to_string(total));
    g_report.record("STEP",  "assemble ok");

    if (g_warnings > 0)
        std::fprintf(stderr,
            "[engine_build] %d warning(s) — each one is a defect in the SHIPPED "
            "package, not a style note. Pass --strict to make them fatal.\n",
            g_warnings);

    return finish(addon::Verdict::Ok);
}
