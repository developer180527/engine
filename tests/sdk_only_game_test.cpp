// ── The no-editor path, exercised ────────────────────────────────────────────
//
// The engine's tools are all CLI programs and every source format is text, so a
// complete game can be built with the SDK, a text editor and a Makefile — no
// editor application anywhere in the loop. `tool-ecosystem.md` treats that as a
// property worth having: the editor is a CLIENT of the same tools, never a
// privileged path.
//
// Nothing verified it. That is the shape of failure this repository has already
// been bitten by twice:
//
//   * ENGINE_AUDIO_FROZEN carried static asserts in a header no translation unit
//     included, so not one of them had ever compiled — for months.
//   * kit_lifecycle_test needs .so files from gitignored Kits repos, so on a
//     clean checkout ctest does not list it and CI had never run it once.
//
// Both were the same lesson: a feature nothing exercises is a comment. The
// no-editor path was exactly that shape — it should work, nobody checked, and
// everyone developing here uses the editor.
//
// ── The specific drift this catches ─────────────────────────────────────────
// It is not a vague worry. `.scene` files are hand-editable JSON, and today the
// EDITOR writes them while the COOKER reads them. Two ways that quietly breaks:
//
//   * the cooker starts requiring a field only the editor emits — every
//     hand-written scene stops cooking correctly;
//   * the editor starts emitting a field the cooker needs by default — the same
//     bug, arriving from the other side.
//
// Neither fails a build. Neither fails any other test, because every .scene in
// this repository came out of the editor. So this test writes one BY HAND, with
// values no template would produce, and requires them to survive into the cooked
// binary the runtime actually loads.
//
// ── What this proves, and what it does not ──────────────────────────────────
// PROVES: the CLI chain works end to end — scaffold, hand-author, cook — and a
// hand-written scene's data reaches the cooked artifact intact.
//
// DOES NOT PROVE: that the packaged game renders on screen. That needs a display
// and a GPU, which CI runners do not reliably have, and a lane that flakes is
// worse than one that is honest about its edges. The `sdk-only-game` CI job
// covers the other half this cannot: that all of the above still builds and runs
// with ENGINE_BUILD_EDITOR=OFF, i.e. with no editor binary in the tree at all.
#include <assetlib/scene_asset.h>

#include "test_watchdog.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Run a tool and return its exit code, or -1 if it could not run at all.
static int runTool(const std::string& exe, const std::string& args) {
    const std::string cmd = "\"" + exe + "\" " + args +
#if defined(_WIN32)
        " > NUL 2>&1";
#else
        " > /dev/null 2>&1";
#endif
    const int rc = std::system(cmd.c_str());
#if defined(_WIN32)
    return rc;
#else
    return (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
#endif
}

// ── The hand-written scene ──────────────────────────────────────────────────
// Deliberately NOT what `engine_project`'s template produces. Every number here
// is odd enough that it could not arrive by default: if a value below turns up
// as 60.0 or 3.0 in the cooked file, a default won and the hand-written input was
// dropped somewhere between the JSON and the binary.
//
// Only the two purely-declarative components are used — camera and light. A
// meshRenderer would drag in asset resolution through the registry, which is
// cook_deps_test's subject; the question here is whether HAND-WRITTEN JSON
// reaches the cooked artifact, and these two answer it without a second variable.
static const char* kHandWrittenScene = R"JSON({
  "entities": [
    {
      "name": "HandWrittenCamera",
      "transform": { "position": [1.5, 2.5, 3.5] },
      "camera": {
        "isPrimary": true,
        "fov": 71.5,
        "nearPlane": 0.25,
        "farPlane": 512.0
      }
    },
    {
      "name": "HandWrittenLight",
      "transform": { "position": [-4.0, 8.0, -2.0], "scale": [2.0, 2.0, 2.0] },
      "light": {
        "intensity": 7.25,
        "range": 33.5,
        "castShadows": true
      }
    }
  ]
})JSON";

int main() {
    testwd::begin("sdk_only_game_test", 120);

    const char* projectTool = std::getenv("ENGINE_PROJECT_TOOL");
    const char* cookTool    = std::getenv("ENGINE_COOK_TOOL");
    const bool required     = std::getenv("ENGINE_REQUIRE_SDK_ONLY_TEST") != nullptr;

    if (!projectTool || !*projectTool || !fs::exists(projectTool) ||
        !cookTool    || !*cookTool    || !fs::exists(cookTool)) {
        // Under ctest both paths are set and both binaries are known to exist, so
        // absence is a finding. From a bare shell there is no build to point at
        // and skipping is right — the same split the cooked-format and module-ABI
        // lanes use.
        if (required) {
            std::printf("  FAIL  ENGINE_PROJECT_TOOL / ENGINE_COOK_TOOL missing, "
                        "and this run was expected to have them\n");
            testwd::end();
            return 1;
        }
        std::printf("  skip  ENGINE_PROJECT_TOOL / ENGINE_COOK_TOOL unset — "
                    "run through ctest\n");
        testwd::end();
        return 0;
    }

    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "engine_sdk_only_game";
    fs::remove_all(root, ec);
    const fs::path project = root / "game";

    // ── 1. Scaffold, by CLI ─────────────────────────────────────────────────
    testwd::phase("engine_project create");
    std::printf("\n-- the CLI chain --\n");
    const int projRc = runTool(projectTool,
                               "create \"" + project.string() + "\" --name SdkOnly");
    CHECK(projRc == 0, "engine_project create exits 0");
    CHECK(fs::exists(project / "project.json"),
          "...and writes a project.json (a text file, hand-editable)");

    const fs::path scenePath = project / "assets" / "scenes" / "main.scene";
    CHECK(fs::exists(scenePath), "...and a .scene the template provided");
    if (g_failures) {                       // nothing below can mean anything
        std::printf("\nsdk_only_game_test: scaffolding failed, stopping\n");
        testwd::end();
        return 1;
    }

    // ── 2. Author BY HAND ───────────────────────────────────────────────────
    // Overwriting the template's scene with text. This is the step that has no
    // editor in it, and the whole point of the test.
    testwd::phase("hand-write the scene");
    {
        std::ofstream f(scenePath, std::ios::binary | std::ios::trunc);
        f << kHandWrittenScene << "\n";
        CHECK(f.good(), "a scene written by hand replaces the template's");
    }

    // ── 3. Cook, by CLI ─────────────────────────────────────────────────────
    testwd::phase("engine_cook");
    const int cookRc = runTool(cookTool, "\"" + project.string() + "\"");
    CHECK(cookRc == 0, "engine_cook exits 0 on a hand-written scene");

    const fs::path cooked = project / ".cache" / "scenes" / "main.cooked";
    CHECK(fs::exists(cooked), "...and produces .cache/scenes/main.cooked");
    if (!fs::exists(cooked)) {
        std::printf("\nsdk_only_game_test: no cooked scene, stopping\n");
        testwd::end();
        return 1;
    }

    // ── 4. Read it back with the REAL runtime loader ─────────────────────────
    // assetlib::loadScene, the same function the player calls. Not a JSON
    // re-read and not a size check: the question is whether the bytes the
    // RUNTIME will consume carry what was typed.
    testwd::phase("loadScene");
    std::printf("\n-- what survived into the cooked binary --\n");
    assetlib::SceneAsset scene;
    CHECK(assetlib::loadScene(scene, cooked),
          "the cooked scene loads through assetlib::loadScene");
    CHECK(scene.entities.size() == 2,
          "both hand-written entities are present (got %zu)",
          scene.entities.size());
    if (scene.entities.size() != 2) {
        std::printf("\nsdk_only_game_test: wrong entity count, stopping\n");
        testwd::end();
        return 1;
    }

    auto nameOf = [&](const assetlib::SceneEntity& e) {
        return e.nameOffset == 0xFFFFFFFFu
                   ? std::string{}
                   : assetlib::stringTableRead(scene.stringTable, e.nameOffset,
                                               e.nameLength);
    };

    const assetlib::SceneEntity* cam = nullptr;
    const assetlib::SceneEntity* light = nullptr;
    for (const auto& e : scene.entities) {
        const std::string n = nameOf(e);
        if (n == "HandWrittenCamera") cam = &e;
        if (n == "HandWrittenLight")  light = &e;
    }

    // Names first: they are how the other assertions find their entity, so a
    // failure here would otherwise present as six confusing failures.
    CHECK(cam != nullptr, "the camera entity kept its hand-written NAME");
    CHECK(light != nullptr, "the light entity kept its hand-written NAME");
    if (!cam || !light) {
        std::printf("\nsdk_only_game_test: named entities missing, stopping\n");
        testwd::end();
        return 1;
    }

    CHECK(cam->componentMask & assetlib::kComp_Camera,
          "the camera's component bit is set");
    CHECK(cam->componentMask & assetlib::kComp_Transform,
          "...and its transform bit");
    CHECK(nearly(cam->position[0], 1.5f) && nearly(cam->position[1], 2.5f) &&
          nearly(cam->position[2], 3.5f),
          "transform position survived: (%.2f, %.2f, %.2f)",
          cam->position[0], cam->position[1], cam->position[2]);

    // 71.5, not 60.0. A default winning here is precisely the drift this test
    // exists to catch, so the message says what the wrong answer would mean.
    CHECK(nearly(cam->cameraFov, 71.5f),
          "camera fov is the hand-written 71.5, not the 60.0 default (got %.2f)",
          cam->cameraFov);
    CHECK(nearly(cam->cameraNearPlane, 0.25f),
          "camera nearPlane is 0.25, not the 0.1 default (got %.3f)",
          cam->cameraNearPlane);
    CHECK(nearly(cam->cameraFarPlane, 512.0f),
          "camera farPlane is 512, not the 1000 default (got %.1f)",
          cam->cameraFarPlane);
    CHECK(cam->cameraIsPrimary == 1, "camera isPrimary survived");

    CHECK(light->componentMask & assetlib::kComp_Light,
          "the light's component bit is set");
    CHECK(nearly(light->lightIntensity, 7.25f),
          "light intensity is the hand-written 7.25, not the 3.0 default "
          "(got %.2f)", light->lightIntensity);
    CHECK(nearly(light->lightRange, 33.5f),
          "light range is 33.5, not the 15.0 default (got %.2f)",
          light->lightRange);
    CHECK(light->lightCastShadows == 1,
          "light castShadows survived as true (a bool through JSON, not a float)");
    CHECK(nearly(light->scale[0], 2.0f),
          "a non-default transform scale survived (got %.2f)", light->scale[0]);

    // ── 5. Idempotence ──────────────────────────────────────────────────────
    // Cooking again must not fail, and must leave the artifact loadable. The
    // second run takes the up-to-date path, which is the one a developer hits on
    // every build after the first — and the one nothing else here exercises.
    testwd::phase("engine_cook again");
    std::printf("\n-- cooking twice --\n");
    CHECK(runTool(cookTool, "\"" + project.string() + "\"") == 0,
          "a second engine_cook of an unchanged project exits 0");
    assetlib::SceneAsset again;
    CHECK(assetlib::loadScene(again, cooked) &&
          again.entities.size() == scene.entities.size(),
          "...and the cooked scene is still loadable with the same entity count");

    fs::remove_all(root, ec);
    testwd::end();
    if (g_failures) {
        std::printf("\nsdk_only_game_test: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nsdk_only_game_test: all checks passed — a game's scene went "
                "from hand-written text to a runtime-loadable binary with no "
                "editor involved\n");
    return 0;
}
