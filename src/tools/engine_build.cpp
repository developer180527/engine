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
#include "tools/build/package_closure.h"

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

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "usage: engine_build <project-dir> [--dist <dir>]\n");
        return 2;
    }
    const fs::path project = fs::absolute(argv[1]);
    fs::path dist = project / "dist";
    for (int i = 2; i < argc - 1; ++i)
        if (std::string(argv[i]) == "--dist") dist = fs::absolute(argv[i + 1]);

    const fs::path engineRoot = ENGINE_SOURCE_ROOT;
    const fs::path shipTree   = engineRoot / "build-ship";
    const fs::path selfDir    = fs::absolute(argv[0]).parent_path(); // dev build dir

    if (!fs::exists(project / "project.json")) {
        std::fprintf(stderr, "engine_build: no project.json at %s\n",
                     project.string().c_str());
        return 2;
    }

    std::printf("[engine_build] project: %s\n[engine_build] dist:    %s\n",
                project.string().c_str(), dist.string().c_str());

    // ── 1. Cook everything ────────────────────────────────────────────────
    if (run(q(selfDir / "engine_cook") + " " + q(project)) != 0) {
        std::fprintf(stderr, "engine_build: cook FAILED — fix assets first\n");
        return 1;
    }

    // ── 2. Shipping player (Assimp-free posture, Release) ────────────────
    if (run("cmake -S " + q(engineRoot) + " -B " + q(shipTree) +
            " -DENGINE_WITH_SOURCE_IMPORTERS=OFF -DCMAKE_BUILD_TYPE=Release") != 0)
        return 1;
    if (run("cmake --build " + q(shipTree) + " --target engine_player" + jobFlag()) != 0)
        return 1;

    // ── 3. Kits + game module, rebuilt Release ────────────────────────────
    nlohmann::json proj;
    try {
        std::ifstream f(project / "project.json");
        proj = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "engine_build: bad project.json: %s\n", e.what());
        return 1;
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
            std::fprintf(stderr, "engine_build: kit '%s': no CMakeLists at %s\n",
                         name.c_str(), srcDir.string().c_str());
            return 1;
        }
        if (run("cmake -S " + q(srcDir) + " -B " + q(kitBuild) +
                " -DCMAKE_BUILD_TYPE=Release -DENGINE_ROOT=" + q(engineRoot)) != 0)
            return 1;
        if (run("cmake --build " + q(kitBuild) + jobFlag()) != 0) return 1;

        const fs::path shipped = dist / "kits" / module.filename();
        if (!fs::copy_file(kitBuild / module.filename(), shipped,
                           fs::copy_options::overwrite_existing, ec) || ec) {
            std::fprintf(stderr, "engine_build: kit '%s': built module not "
                         "found at %s\n", name.c_str(),
                         (kitBuild / module.filename()).string().c_str());
            return 1;
        }
        kit["module"] = (fs::path("kits") / module.filename()).generic_string();
        std::printf("[engine_build] kit '%s' -> %s (Release)\n",
                    name.c_str(), shipped.filename().string().c_str());
    }
    fs::remove_all(dist / ".kitbuild", ec);

    // ── 4. Assemble ───────────────────────────────────────────────────────
    fs::copy_file(shipTree / "engine_player", dist / "engine_player",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "engine_build: player copy failed\n"); return 1; }

    // Cooked content: scenes + the reference-walked mesh closure + anim
    // clips. NOT the registry (editor-only) and NOT .cache/textures (no
    // runtime consumer path yet — cooked standalone textures are dead
    // weight until the material system binds them).
    const fs::path cache = project / ".cache";
    for (auto& e : fs::directory_iterator(cache / "scenes", ec))
        if (e.path().extension() == ".cooked")
            if (!copyFile(e.path(), dist / ".cache" / "scenes"
                                         / e.path().filename())) return 1;

    const auto sceneRefs = pkg::sceneMeshRefs(cache / "scenes");
    for (const std::string& bad : sceneRefs.unreadableScenes)
        std::fprintf(stderr, "[engine_build] WARNING: unreadable cooked scene "
                     "%s — every object in it is MISSING from the package\n",
                     bad.c_str());
    if (sceneRefs.scenesRead == 0)
        std::fprintf(stderr, "[engine_build] WARNING: no cooked scenes read — "
                     "the package has no content\n");
    const auto& meshRefs = sceneRefs.meshes;
    size_t shippedMeshFiles = 0;
    for (const std::string& rel : meshRefs) {               // "meshs/<uuid>.cooked"
        const fs::path cooked = cache / rel;
        if (!fs::exists(cooked)) {
            std::fprintf(stderr, "[engine_build] WARNING: scene references "
                         "missing cooked mesh: %s\n", rel.c_str());
            continue;
        }
        // Ship what the mesh REFERENCES, never what its filename suggests —
        // src/tools/build/package_closure.h records the bug that rule encodes,
        // and tests/package_closure_test.cpp keeps it from coming back.
        const auto closure = pkg::meshClosure(cooked);
        if (closure.unreadable) {
            std::fprintf(stderr, "[engine_build] WARNING: cannot read cooked "
                         "mesh %s — it and its textures will not ship\n",
                         rel.c_str());
            continue;
        }
        for (const std::string& miss : closure.missing)
            std::fprintf(stderr, "[engine_build] WARNING: mesh %s references "
                         "missing texture %s — it will render untextured\n",
                         rel.c_str(), miss.c_str());
        for (const fs::path& f : closure.files) {
            const fs::path dst = dist / ".cache" / "meshs" / f.filename();
            if (fs::exists(dst, ec)) continue;   // shared between meshes
            if (!copyFile(f, dst)) return 1;
            ++shippedMeshFiles;
        }
    }
    // Cooked shaders. The runtime resolves these BY NAME from .cache/shaders
    // (a dist has no registry), so the whole directory ships — it is a handful
    // of KB, and the closed-feature rule keeps it that way. Without this the
    // player silently falls back to compiled-in shaders and a project's custom
    // shading never runs in the shipped game.
    size_t shippedShaders = 0;
    if (fs::exists(cache / "shaders")) {
        for (auto& e : fs::directory_iterator(cache / "shaders", ec))
            if (e.path().extension() == ".cooked") {
                if (!copyFile(e.path(), dist / ".cache" / "shaders"
                                             / e.path().filename())) return 1;
                ++shippedShaders;
            }
    }
    if (shippedShaders == 0)
        std::fprintf(stderr, "[engine_build] WARNING: no cooked shaders — the "
                     "player will fall back to compiled-in ones\n");
    else
        std::printf("[engine_build] cooked shaders: %zu\n", shippedShaders);

    if (fs::exists(cache / "anim"))
        for (auto& e : fs::directory_iterator(cache / "anim", ec))
            if (!copyFile(e.path(), dist / ".cache" / "anim"
                                         / e.path().filename())) return 1;
    std::printf("[engine_build] cooked closure: %zu scene mesh ref(s), "
                "%zu mesh file(s)\n", meshRefs.size(), shippedMeshFiles);

    // assets/: runtime-consumed formats only (audio, lua) — see whitelist.
    for (auto it = fs::recursive_directory_iterator(project / "assets", ec);
         it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file() && isRuntimeAsset(it->path())) {
            const fs::path rel = fs::relative(it->path(), project, ec);
            if (!copyFile(it->path(), dist / rel)) return 1;
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
    return 0;
}
