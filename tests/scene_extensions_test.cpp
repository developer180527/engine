// ── scene_extensions_test — a project's required extensions, refused by name ──
//
// WO-071, DR-0035. project.json may say which component namespaces its scenes
// need beyond the core:
//
//   "extensions": { "required": [...], "used": [...] }
//
// A required one that neither the build nor the project's kits provide refuses
// the simulation at start, by name, with what is available; a used one is a
// warning. This boots a headless engine on throwaway projects and starts Play.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "plugins/stock_plugins.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static fs::path makeProject(const char* name, const char* extensionsJson) {
    const fs::path root = fs::temp_directory_path() / "engine_extensions_test" / name;
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    std::ofstream f(root / "project.json");
    f << "{\"name\":\"" << name << "\",\"assetRoot\":\"assets\"";
    if (extensionsJson) f << ",\"extensions\":" << extensionsJson;
    f << "}";
    return root;
}

struct Play { bool booted = false, started = false, simulatingAfter = false; std::string error; };

static Play play(const fs::path& root) {
    Play p;
    EngineConfig cfg;
    cfg.projectRoot       = root;
    cfg.defaultScene      = false;
    cfg.openAssetDatabase = false;
    EngineRuntime engine;
    p.booted = engine.init(cfg, std::make_unique<HeadlessPlatform>()) && addProviders(engine);
    if (p.booted) {
        engine.attachPlugins();
        p.started         = engine.startSimulation();
        p.simulatingAfter = engine.simulating();
        p.error           = engine.simulationError();
        if (p.started) engine.stopSimulation();
    }
    engine.shutdown();
    return p;
}

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("scene_extensions_test: required extensions refuse Play by name (WO-071)\n");

    std::printf("\n-- a required extension nothing provides --\n");
    {
        const Play p = play(makeProject("missing", R"({"required": ["apollo", "core"]})"));
        CHECK(p.booted, "the project opens: what it lacks is known only at Play");
        CHECK(!p.started && !p.simulatingAfter, "the simulation is refused, and is not left running");
        const std::size_t at = p.error.find("available: ");
        const std::string missing = p.error.substr(0, at), avail = at == std::string::npos
                                  ? std::string{} : p.error.substr(at);
        CHECK(has(missing, "apollo") && !has(missing, "core") && avail == "available: core",
              "the reason names the missing one, and what is available, engine internals left out"
              " (%s)", p.error.c_str());
    }

    std::printf("\n-- what is there, and what is only used --\n");
    {
        const Play req = play(makeProject("present", R"({"required": ["core"]})"));
        CHECK(req.started && req.error.empty(), "a required extension the build has starts");
        const Play used = play(makeProject("used", R"({"used": ["apollo"]})"));
        CHECK(used.started, "a used one that is missing is a warning, not a refusal");
        const Play none = play(makeProject("none", nullptr));
        CHECK(none.started, "a project that declares none starts as before");
    }

    // A block written wrongly is refused at Play, by name, rather than read as
    // declaring nothing: a required extension in a misspelled key would
    // otherwise never be checked (DR-0035). The project still opens.
    std::printf("\n-- a malformed block is refused by name --\n");
    {
        const Play odd = play(makeProject("odd", R"({"required": "apollo", "used": [3, ""]})"));
        CHECK(odd.booted && !odd.started && has(odd.error, "\"required\" is not a list of names")
              && has(odd.error, "holds 3"), "a list that is not a list of names (%s)", odd.error.c_str());
        const Play typo = play(makeProject("typo", R"({"requried": ["apollo"]})"));
        CHECK(typo.booted && !typo.started && has(typo.error, "\"requried\" is not a key it has"),
              "a misspelled key (%s)", typo.error.c_str());
        const Play notObject = play(makeProject("not-object", R"(["apollo"])"));
        CHECK(notObject.booted && !notObject.started && has(notObject.error, "not an object"),
              "a block that is not an object (%s)", notObject.error.c_str());
    }

    std::printf("\n-- the block survives the editor saving the project --\n");
    {
        const fs::path root = makeProject("saved", R"({"required": ["combat"], "used": ["apollo"]})");
        ProjectContext ctx = ProjectContext::load(root);
        CHECK(ctx.extensions.required.size() == 1 && ctx.extensions.required[0] == "combat"
              && ctx.extensions.used.size() == 1 && ctx.extensions.used[0] == "apollo",
              "required and used are read");
        ctx.save();
        const ProjectContext again = ProjectContext::load(root);
        CHECK(again.extensions.raw == ctx.extensions.raw && again.extensions.required == ctx.extensions.required,
              "saved back as written");
    }

    fs::remove_all(fs::temp_directory_path() / "engine_extensions_test");
    if (g_failures) {
        std::printf("\nscene_extensions_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nscene_extensions_test: all checks passed\n");
    return 0;
}
