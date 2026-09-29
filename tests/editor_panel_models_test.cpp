// editor_panel_models_test — the Hierarchy and Inspector, without a GUI.
//
// hier::HierarchyModel and the inspector model (inspect::) are what the ImGui
// editor draws and what the libgui experiment draws, so the two front ends
// cannot disagree about what an action does. This pins the behaviour that
// used to live inside ImGui draw calls:
//   - which entities are listed, and how they nest;
//   - add / delete / reparent, each one undoable, reparent keeping the world
//     pose, and structural changes deferred until apply();
//   - one continuous edit = one undo step, whatever widget produced it;
//   - reflected (meta-schema) components enumerate as typed fields.
#include "editor/panels/hierarchy/model.h"
#include "editor/panels/inspector_panel/model.h"

#ifdef IMGUI_VERSION
#error "the panel models must not include ImGui: every GUI front end shares them"
#endif

#include "assets/importers/importer_registry.h"
#include "project/project_context.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
                   std::printf(__VA_ARGS__);                           \
                   std::printf("\n"); ++g_failures; }                  \
} while (0)

static bool near3(const bx::Vec3& a, float x, float y, float z) {
    return std::fabs(a.x - x) < 1e-3f && std::fabs(a.y - y) < 1e-3f && std::fabs(a.z - z) < 1e-3f;
}

static flecs::entity make(flecs::world& w, const char* name, float x = 0, float y = 0, float z = 0) {
    Transform t{}; t.scale = {1, 1, 1}; t.rotation = {0, 0, 0, 1}; t.position = {x, y, z};
    auto e = w.entity().set<Name>({name}).set<Transform>(t);
    ensureEntityId(e);
    return e;
}

static bx::Vec3 worldPos(flecs::entity e) {
    float m[16]; getWorldMatrix(e, m);
    return {m[12], m[13], m[14]};
}

// A kit-style component: meta-registered, no hand-written inspector section.
struct Vec3f { float x, y, z; };
struct Health { float hp; int32_t lives; Vec3f spawn; bool alive; };

int main() {
    flecs::world ecs;
    AssetRegistry assets; TextureRegistry textures; MaterialRegistry materials;
    ProjectContext project; ImporterRegistry importers;
    EditorState editor; GizmoState gizmo;
    EngineContext ctx{ecs, assets, textures, materials, project, importers, editor, gizmo};

    hier::HierarchyModel h;

    // ── 1. What is listed, and how it nests ─────────────────────────────────
    auto root  = make(ecs, "Root");
    auto child = make(ecs, "Child");
    child.add(flecs::ChildOf, root);
    auto cam   = make(ecs, "Cam").set<Camera>({});
    ecs.entity().set<Transform>({});   // unnamed: never listed
    {
        std::vector<std::string> roots;
        hier::forEachRoot(ecs, [&](flecs::entity e) { roots.push_back(hier::label(e)); });
        CHECK(roots.size() == 2, "two named roots, got %zu", roots.size());
        CHECK(hier::hasChildren(root) && !hier::hasChildren(child), "nesting");
        CHECK(hier::label(cam) == "[Cam] Cam", "camera label: %s", hier::label(cam).c_str());
        CHECK(!hier::isRoot(child) && hier::isRoot(root), "roots");
        CHECK(hier::entityCount(ecs) == 3, "three named entities, got %d", hier::entityCount(ecs));
    }

    // ── 2. Add: undoable, selected, dirty; primitives need their meshes ─────
    {
        CHECK(!h.canAdd(ctx, hier::AddKind::Cube), "no primitive library: a cube cannot be added");
        CHECK(!h.add(ctx, hier::AddKind::Cube), "and add() refuses rather than crashing");
        auto e = h.add(ctx, hier::AddKind::Empty);
        CHECK(e.is_alive() && editor.selected == e && editor.sceneDirty, "an added entity is selected and dirties the scene");
        CHECK(editor.undoStack.canUndo(), "adding is undoable");
        auto c2 = h.add(ctx, hier::AddKind::Camera);
        CHECK(c2.has<Camera>() && editor.selected == e, "a new camera is not selected (as before)");
        auto e2 = h.add(ctx, hier::AddKind::Empty);
        CHECK(e2.get<Name>().value != e.get<Name>().value, "names are made unique");
        editor.undoStack.undo(ecs);
        CHECK(!e2.is_alive(), "undo removes the last added entity");
    }

    // ── 3. Reparent: no cycles, deferred, keeps the world pose, undoable ────
    {
        auto parent = make(ecs, "Parent", 10, 0, 0);
        auto mover  = make(ecs, "Mover", 3, 0, 0);
        CHECK(!h.requestReparent(root, child), "a parent cannot move under its own child");
        CHECK(!h.requestReparent(root, root), "nor under itself");
        CHECK(h.requestReparent(mover, parent), "a legal reparent is accepted");
        CHECK(hier::isRoot(mover), "nothing changes until apply()");
        h.apply(ctx);
        CHECK(!hier::isRoot(mover) && mover.target(flecs::ChildOf) == parent, "applied");
        CHECK(near3(mover.get<Transform>().position, -7, 0, 0), "local position recomputed against the parent");
        CHECK(near3(worldPos(mover), 3, 0, 0), "and the world position is unchanged");
        editor.undoStack.undo(ecs);
        CHECK(hier::isRoot(mover) && near3(mover.get<Transform>().position, 3, 0, 0), "undo puts it back");

        CHECK(h.requestUnparent(child) && (h.apply(ctx), hier::isRoot(child)), "unparent to the root");
    }

    // ── 4. Delete: deferred, clears the selection, undoable ─────────────────
    {
        auto victim = make(ecs, "Victim");
        editor.selected = victim;
        h.requestDelete(victim);
        CHECK(victim.is_alive(), "not deleted until apply()");
        h.apply(ctx);
        CHECK(!victim.is_alive() && !editor.selected, "deleted and deselected");
        editor.undoStack.undo(ecs);
        bool back = false;
        hier::forEachRoot(ecs, [&](flecs::entity e) { back |= e.get<Name>().value == "Victim"; });
        CHECK(back, "undo restores a deleted entity");
    }

    // ── 5. One continuous edit = one undo step ──────────────────────────────
    {
        auto e = make(ecs, "Before");
        inspect::PropertyEdit edit;
        const int steps0 = editor.undoStack.stepsDone();
        // Frame 1: the widget starts being edited; frames 2-4: typing.
        edit.track(ctx, e, "name", "Rename", /*started*/ true, false, false);
        for (const char* v : {"A", "Af", "After"}) {
            e.get_mut<Name>().value = v;
            edit.track(ctx, e, "name", "Rename", false, false, false);
        }
        edit.track(ctx, e, "name", "Rename", false, /*endedAfterEdit*/ true, true);
        CHECK(editor.undoStack.stepsDone() == steps0 + 1, "a whole edit is one undo step (%d -> %d)",
              steps0, editor.undoStack.stepsDone());
        editor.undoStack.undo(ecs);
        CHECK(e.get<Name>().value == "Before", "undo restores the value before the edit: %s", e.get<Name>().value.c_str());

        const int steps1 = editor.undoStack.stepsDone();
        edit.track(ctx, e, "name", "Rename", true, false, false);
        edit.track(ctx, e, "name", "Rename", false, false, /*ended*/ true);
        CHECK(!edit.active() && editor.undoStack.stepsDone() == steps1, "an edit that changed nothing records nothing");

        inspect::TransformEdit tedit;
        const int steps2 = editor.undoStack.stepsDone();
        tedit.begin(e);
        e.get_mut<Transform>().position = {1, 2, 3};
        CHECK(tedit.commit(ctx) && editor.undoStack.stepsDone() == steps2 + 1, "a transform drag is one step");
        editor.undoStack.undo(ecs);
        CHECK(near3(e.get<Transform>().position, 0, 0, 0), "and undoes");
        CHECK(!tedit.commit(ctx), "commit without begin does nothing");
    }

    // ── 6. Reflected components enumerate as typed fields ───────────────────
    {
        ecs.component<Vec3f>().member<float>("x").member<float>("y").member<float>("z");
        ecs.component<Health>()
            .member<float>("hp").member<int32_t>("lives")
            .member<Vec3f>("spawn").member<bool>("alive");
        auto e = make(ecs, "Kit");
        e.set<Health>({50.0f, 3, {1, 2, 3}, true});

        auto comps = inspect::reflectedComponents(e);
        bool found = false;
        for (const auto& rc : comps) {
            CHECK(rc.path != "Transform" && rc.path != "Name", "hand-written components are not reflected: %s", rc.path.c_str());
            if (rc.type == ecs.component<Health>()) found = true;
        }
        CHECK(found, "the kit component is listed");

        std::vector<std::string> seen;
        void* ptr = inspect::componentPtr(e, ecs.component<Health>());
        inspect::forEachField(ecs, ecs.component<Health>(), ptr, [&](const inspect::Field& f) {
            seen.push_back(std::string(f.name) + ":" + std::to_string((int)f.kind) + ":" + std::to_string(f.depth));
            if (f.kind == inspect::FieldKind::F32 && std::string(f.name) == "hp") *(float*)f.ptr = 75.0f;
        });
        const std::vector<std::string> want = {
            "hp:0:0", "lives:3:0", "spawn:7:0", "x:0:1", "y:0:1", "z:0:1", "alive:2:0"};
        CHECK(seen == want, "fields in declaration order, nested struct one level deeper (got %zu)", seen.size());
        inspect::markEdited(ctx, e, ecs.component<Health>());
        CHECK(e.get<Health>().hp == 75.0f, "writing through a field edits the component");
        inspect::removeComponent(ctx, e, ecs.component<Health>());
        CHECK(!e.has<Health>(), "remove");
    }

    // ── 7. Euler degrees survive a round trip through the quaternion ────────
    {
        const bx::Vec3 in{30.0f, -45.0f, 10.0f};
        const bx::Vec3 out = inspect::quatToEulerDeg(inspect::eulerDegToQuat(in));
        CHECK(near3(out, in.x, in.y, in.z), "euler round trip: %f %f %f", out.x, out.y, out.z);
    }

    if (g_failures) {
        std::printf("\neditor_panel_models_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("editor_panel_models_test: all checks passed\n");
    return 0;
}
