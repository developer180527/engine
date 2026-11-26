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
// scene the runtime actually loads.
//
// ── What this proves, and what it does not ──────────────────────────────────
// PROVES: the CLI chain works end to end — scaffold, hand-author, cook — and a
// hand-written scene's data reaches the cooked artifact intact.
//
// ALSO PINS: the surprising claims in docs/reference/scene-format.md (section 5).
// `status: as-built` gets the docs gate to flag that file stale when the cooker
// changes, which prompts a human to re-read it — it cannot tell whether the prose
// is TRUE. Writing that document turned up one claim that was wrong, so the rest
// are asserted here rather than trusted.
//
// DOES NOT PROVE: that the packaged game renders on screen. That needs a display
// and a GPU, which CI runners do not reliably have, and a lane that flakes is
// worse than one that is honest about its edges. The `sdk-only-game` CI job
// covers the other half this cannot: that all of the above still builds and runs
// with ENGINE_BUILD_EDITOR=OFF, i.e. with no editor binary in the tree at all.
#include <flecs.h>

#include "components/camera.h"
#include "components/entity_id.h"
#include "components/light.h"
#include "components/meta_registry.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "core/transform.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "runtime/services/asset_service.h"
#include "runtime/services/scene_service.h"

#include "test_watchdog.h"
#include "shell_run.h"   // runShell: std::system with one meaning on every OS

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// The cooked scene, loaded the way the player loads it: SceneService into a
// world of its own. No GPU: the scenes here have no meshes.
struct Loaded {
    std::unique_ptr<flecs::world> world = std::make_unique<flecs::world>();
    std::vector<flecs::entity> entities;
    bool ok = false;
    flecs::entity byName(const char* want) const {
        for (flecs::entity e : entities)
            if (const Name* n = e.try_get<Name>(); n && n->value == want) return e;
        return flecs::entity{};
    }
};
static Loaded loadCooked(const fs::path& project) {
    Loaded out;
    MetaRegistry::registerAll(*out.world);
    AssetRegistry meshes; TextureRegistry textures; MaterialRegistry materials;
    AssetService assets({meshes, textures, materials});
    assets.setProjectRoot(project);
    SceneService svc({assets, meshes, textures, materials, *out.world});
    svc.setCacheRoot(project / ".cache");
    const uint32_t h = svc.loadScene("scenes/main.cooked");
    out.ok = h != 0;
    out.world->each([&](flecs::entity e, const EntityId&) { out.entities.push_back(e); });
    return out;   // the service's record goes with it; the entities stay in the world
}

// Run a tool and return its exit code, or -1 if it could not run at all.
static int runTool(const std::string& exe, const std::string& args) {
    return runShell("\"" + exe + "\" " + args + " > " + nullDevice() + " 2>&1");
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

    // ── 4. Load it with the REAL runtime loader ─────────────────────────────
    // SceneService::loadScene, the function the player calls, into a world: the
    // question is whether the components the GAME gets carry what was typed.
    // Since WO-091 the cooked scene is the scene's own form, read through the
    // schema registry, so this is also the editor's reading of the same file.
    testwd::phase("loadScene");
    std::printf("\n-- what the game gets from the cooked scene --\n");
    Loaded scene = loadCooked(project);
    CHECK(scene.ok, "the cooked scene loads through SceneService::loadScene");
    CHECK(scene.entities.size() == 2,
          "both hand-written entities are present (got %zu)", scene.entities.size());
    if (scene.entities.size() != 2) {
        std::printf("\nsdk_only_game_test: wrong entity count, stopping\n");
        testwd::end();
        return 1;
    }

    // Names first: they are how the other assertions find their entity, so a
    // failure here would otherwise present as six confusing failures.
    const flecs::entity cam   = scene.byName("HandWrittenCamera");
    const flecs::entity light = scene.byName("HandWrittenLight");
    CHECK(cam, "the camera entity kept its hand-written NAME");
    CHECK(light, "the light entity kept its hand-written NAME");
    if (!cam || !light) {
        std::printf("\nsdk_only_game_test: named entities missing, stopping\n");
        testwd::end();
        return 1;
    }

    const Camera* c = cam.try_get<Camera>();
    const Transform* ct = cam.try_get<Transform>();
    CHECK(c != nullptr, "the camera entity has a Camera");
    CHECK(ct && nearly(ct->position.x, 1.5f) && nearly(ct->position.y, 2.5f) &&
          nearly(ct->position.z, 3.5f), "transform position survived: (%.2f, %.2f, %.2f)",
          ct ? ct->position.x : 0.f, ct ? ct->position.y : 0.f, ct ? ct->position.z : 0.f);
    // 71.5, not 60.0. A default winning here is precisely the drift this test
    // exists to catch, so the message says what the wrong answer would mean.
    CHECK(c && nearly(c->fov, 71.5f),
          "camera fov is the hand-written 71.5, not the 60.0 default (got %.2f)", c ? c->fov : 0.f);
    CHECK(c && nearly(c->nearPlane, 0.25f),
          "camera nearPlane is 0.25, not the 0.1 default (got %.3f)", c ? c->nearPlane : 0.f);
    CHECK(c && nearly(c->farPlane, 512.0f),
          "camera farPlane is 512, not the 1000 default (got %.1f)", c ? c->farPlane : 0.f);
    CHECK(c && c->isPrimary, "camera isPrimary survived");

    const Light* l = light.try_get<Light>();
    const Transform* lt = light.try_get<Transform>();
    CHECK(l != nullptr, "the light entity has a Light");
    CHECK(l && nearly(l->intensity, 7.25f),
          "light intensity is the hand-written 7.25, not the 3.0 default (got %.2f)",
          l ? l->intensity : 0.f);
    CHECK(l && nearly(l->range, 33.5f),
          "light range is 33.5, not the 15.0 default (got %.2f)", l ? l->range : 0.f);
    CHECK(l && l->castShadows,
          "light castShadows survived as true (a bool through JSON, not a float)");
    CHECK(lt && nearly(lt->scale.x, 2.0f),
          "a non-default transform scale survived (got %.2f)", lt ? lt->scale.x : 0.f);

    // ── 5. The documented quirks ────────────────────────────────────────────
    // docs/reference/scene-format.md tells a hand-author how wrong values
    // behave, and those claims are the most load-bearing part of the document:
    // they are what someone reaches for when a component "isn't working".
    //
    // `status: as-built` gets the docs gate to flag the file as STALE when the
    // loader changes — which prompts a human to re-read it, and nothing more. It
    // cannot tell whether the prose is TRUE. So the surprising claims are pinned
    // here instead, because a schema claim nothing exercises is a guess.
    //
    // Since WO-091 these are the schema registry's rules, the same in the editor
    // and the game. Before, the cooker had its own, and two of them disagreed
    // with the editor: a non-object value attached a default component in the
    // game and none in the editor, and a missing id was 0 in the game and fresh
    // in the editor. `"fov": 1e999` is not here: nlohmann's parser rejects the
    // number and the whole scene fails to cook, so there is no artifact.
    testwd::phase("documented quirks");
    std::printf("\n-- the quirks the schema doc promises --\n");
    {
        static const char* kQuirks = R"JSON({
  "entities": [
    { "name": "GarbageCameraValue", "camera": 5 },
    { "name": "QuotedBool",  "light": { "castShadows": "true", "intensity": 9.5 } },
    { "name": "RealBool",    "light": { "castShadows": true, "type": 2 } },
    { "name": "ShortScale",  "transform": { "scale": [2, 2] } },
    { "name": "StringFov",   "camera": { "fov": "71.5" } },
    { "name": "StringEnum",  "light": { "type": "point" } },
    { "name": "EmptyBody",   "rigidBody": {} },
    { "name": 12345 },
    "this array element is not an object",
    { "id": 7, "name": "Parent" },
    { "id": 8, "parentId": 7, "name": "Child" }
  ]
})JSON";
        {
            std::ofstream f(scenePath, std::ios::binary | std::ios::trunc);
            f << kQuirks << "\n";
        }
        CHECK(runTool(cookTool, "\"" + project.string() + "\"") == 0,
              "a scene full of deliberately wrong values still cooks");

        Loaded q = loadCooked(project);
        CHECK(q.ok, "...and loads");
        // 11 array elements, one of which is a string. A non-object element is
        // skipped, which is the doc's claim and also the reason an entity count
        // lower than you wrote is worth checking in the log.
        CHECK(q.entities.size() == 10,
              "the non-object array element was skipped: 11 written, 10 loaded (got %zu)",
              q.entities.size());

        // A component whose value is not an object is SKIPPED, with a warning
        // naming it; the rest of the entity loads.
        if (flecs::entity e = q.byName("GarbageCameraValue")) {
            CHECK(!e.has<Camera>(), "\"camera\": 5 attaches NO camera: a value that is not "
                                    "an object is skipped, and the log says so");
        } else CHECK(false, "GarbageCameraValue entity missing");

        // Booleans are strict: is_boolean() only.
        if (flecs::entity e = q.byName("QuotedBool")) {
            const Light* x = e.try_get<Light>();
            CHECK(x && !x->castShadows,
                  "\"castShadows\": \"true\" is FALSE — a quoted boolean is not a "
                  "boolean, and this is the one that wastes the most time");
            CHECK(x && nearly(x->intensity, 9.5f),
                  "...while a correctly-typed sibling field in the same object is "
                  "still read (intensity %.2f)", x ? x->intensity : 0.f);
        } else CHECK(false, "QuotedBool entity missing");

        if (flecs::entity e = q.byName("RealBool")) {
            const Light* x = e.try_get<Light>();
            CHECK(x && x->castShadows, "a real `true` works");
            CHECK(x && x->type == LightType::Spot, "and an integer enum works (spot == 2)");
        } else CHECK(false, "RealBool entity missing");

        // Short float arrays keep their DEFAULT, they do not zero. A (2,2,0)
        // here would flatten the object to a plane.
        if (flecs::entity e = q.byName("ShortScale")) {
            const Transform* t = e.try_get<Transform>();
            CHECK(t && nearly(t->scale.x, 2.0f) && nearly(t->scale.y, 2.0f) &&
                  nearly(t->scale.z, 1.0f),
                  "\"scale\": [2,2] gives (%.1f, %.1f, %.1f) — the missing element keeps "
                  "its default of 1, it does not become 0",
                  t ? t->scale.x : 0.f, t ? t->scale.y : 0.f, t ? t->scale.z : 0.f);
        } else CHECK(false, "ShortScale entity missing");

        if (flecs::entity e = q.byName("StringFov")) {
            const Camera* x = e.try_get<Camera>();
            CHECK(x && nearly(x->fov, 60.0f),
                  "a wrong-typed number falls back to the default (fov %.1f)", x ? x->fov : 0.f);
        } else CHECK(false, "StringFov entity missing");

        // Enums are integers. There are no string enums in this format.
        if (flecs::entity e = q.byName("StringEnum")) {
            const Light* x = e.try_get<Light>();
            CHECK(x && x->type == LightType::Directional,
                  "\"type\": \"point\" is NOT 1 — string enums do not exist here, so it "
                  "silently means directional");
        } else CHECK(false, "StringEnum entity missing");

        // The default nobody expects: kinematic, not dynamic.
        if (flecs::entity e = q.byName("EmptyBody")) {
            const RigidBody* rb = e.try_get<RigidBody>();
            CHECK(rb != nullptr, "\"rigidBody\": {} attaches a rigid body");
            CHECK(rb && rb->bodyType == PhysicsBodyType::Kinematic,
                  "...whose bodyType defaults to KINEMATIC, not dynamic — which is why a "
                  "body you expected to fall sits still");
        } else CHECK(false, "EmptyBody entity missing");

        // The documented exception to "wrong type means default".
        CHECK(q.byName("Entity"),
              "a non-string \"name\" becomes the literal string \"Entity\" — the one "
              "field where a wrong type does not mean its default");

        // Hierarchy, and what an entity without an id gets.
        const flecs::entity parent = q.byName("Parent");
        const flecs::entity child  = q.byName("Child");
        CHECK(parent && parent.try_get<EntityId>() && parent.try_get<EntityId>()->value == 7,
              "an explicit id survives the cook");
        CHECK(child && parent && child.target(flecs::ChildOf) == parent,
              "parentId links to another entity's id in the same file");
        const flecs::entity shortScale = q.byName("ShortScale");
        const flecs::entity strFov     = q.byName("StringFov");
        const uint64_t a = shortScale && shortScale.try_get<EntityId>() ? shortScale.try_get<EntityId>()->value : 0;
        const uint64_t b = strFov && strFov.try_get<EntityId>() ? strFov.try_get<EntityId>()->value : 0;
        CHECK(a != 0 && b != 0 && a != b,
              "an entity that omits \"id\" is given a fresh one, distinct from every "
              "other — but a parentId can only name an id that was written, so every "
              "entity in a hierarchy still needs an explicit one");

        // Put the hand-written scene back for the idempotence check below.
        std::ofstream f(scenePath, std::ios::binary | std::ios::trunc);
        f << kHandWrittenScene << "\n";
    }

    // ── 6. Idempotence ──────────────────────────────────────────────────────
    // Cooking again must not fail, and must leave the artifact loadable. The
    // second run takes the up-to-date path, which is the one a developer hits on
    // every build after the first — and the one nothing else here exercises.
    testwd::phase("engine_cook again");
    std::printf("\n-- cooking twice --\n");
    CHECK(runTool(cookTool, "\"" + project.string() + "\"") == 0,
          "a second engine_cook of an unchanged project exits 0");
    Loaded again = loadCooked(project);
    CHECK(again.ok && again.entities.size() == scene.entities.size(),
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
