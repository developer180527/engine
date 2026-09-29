#pragma once
// ── Hierarchy model — the scene tree's behaviour, without a GUI ─────────────
//
// What the Hierarchy panel knows and does, separated from how it is drawn:
// which entities are listed and how they nest, what each is called, and the
// operations on them — add, delete, reparent (keeping the world pose), each
// recorded on the undo stack. hierarchy_panel.h draws it with ImGui; the
// libgui experiment draws the same model.
//
// Structural ECS changes cannot happen while a flecs query is iterating (the
// table is locked), and a front end draws the tree FROM a query. So delete and
// reparent are REQUESTED while drawing and carried out by apply() afterwards —
// the rule both front ends must follow, which is why it lives here.
#include "editor/engine_context.h"
#include "editor/panels/asset_browser/spawn.h"   // uniqueEntityName

#include "components/camera.h"
#include "components/entity_id_util.h"
#include "components/light.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "components/spinner.h"
#include "components/transform_hierarchy.h"
#include "core/transform.h"
#include "render/primitive_library.h"

#include <flecs.h>
#include <string>
#include <vector>

namespace hier {

// ── What is listed, and how it nests ────────────────────────────────────────
// Listed: entities with a Name. Roots: those whose parent is missing, dead or
// unnamed. Spinners (the demo grid) are left out at the root, as they always
// have been.
inline bool isRoot(flecs::entity e) {
    flecs::entity p = e.target(flecs::ChildOf);
    return !p || !p.is_alive() || !p.has<Name>();
}

template <class F> void forEachRoot(flecs::world& ecs, F&& f) {
    ecs.query_builder<const Name>().without<Spinner>().build()
        .each([&](flecs::entity e, const Name&) { if (isRoot(e)) f(e); });
}

template <class F> void forEachChild(flecs::entity e, F&& f) {
    e.children([&](flecs::entity c) { if (c.has<Name>()) f(c); });
}

inline bool hasChildren(flecs::entity e) {
    bool any = false;
    forEachChild(e, [&](flecs::entity) { any = true; });
    return any;
}

inline int entityCount(flecs::world& ecs) {
    int n = 0;
    ecs.query_builder<const Name>().without<Spinner>().build()
        .each([&](flecs::entity, const Name&) { ++n; });
    return n;
}

inline std::string label(flecs::entity e) {
    const Name* n = e.try_get<Name>();
    if (!n) return {};
    return e.has<Camera>() ? "[Cam] " + n->value : n->value;
}

// ── What can be added ───────────────────────────────────────────────────────
enum class AddKind { Camera, Cube, Sphere, Plane, DirectionalLight, PointLight, Empty };

struct AddItem { AddKind kind; const char* label; const char* group; };

inline const std::vector<AddItem>& addItems() {
    static const std::vector<AddItem> items = {
        {AddKind::Camera,           "Camera",            "Add Entity"},
        {AddKind::Cube,             "Cube",              "Primitives"},
        {AddKind::Sphere,           "Sphere",            "Primitives"},
        {AddKind::Plane,            "Plane",             "Primitives"},
        {AddKind::DirectionalLight, "Directional Light", "Lights"},
        {AddKind::PointLight,       "Point Light",       "Lights"},
        {AddKind::Empty,            "Empty Object",      "Other"},
    };
    return items;
}

class HierarchyModel {
public:
    // ── Add ─────────────────────────────────────────────────────────────────
    // Primitives need their meshes, which exist only once the primitive
    // library has loaded (never, without a renderer).
    bool canAdd(const EngineContext& ctx, AddKind k) const {
        switch (k) {
        case AddKind::Cube: case AddKind::Sphere: case AddKind::Plane:
            return ctx.primitives && ctx.primitives->ready();
        default: return true;
        }
    }

    // Creates it, records it for undo, selects it (except a camera, which
    // the panel never selected), and marks the scene dirty.
    flecs::entity add(EngineContext& ctx, AddKind k) {
        if (!canAdd(ctx, k)) return {};
        Transform t{}; t.scale = {1, 1, 1}; t.rotation = {0, 0, 0, 1};
        auto make = [&](const char* base) {
            const std::string name = uniqueEntityName(ctx.ecs, base);
            return ctx.ecs.entity(name.c_str()).set<Transform>(t).set<Name>({name});
        };
        flecs::entity e;
        bool select = true;
        switch (k) {
        case AddKind::Camera: {
            Camera cam; cam.isPrimary = false;
            e = make("Camera").set<Camera>(cam);
            select = false;
            break;
        }
        case AddKind::Cube:   e = make("Cube").set<MeshRenderer>({ctx.primitives->cube()});     break;
        case AddKind::Sphere: e = make("Sphere").set<MeshRenderer>({ctx.primitives->sphere()}); break;
        case AddKind::Plane:  e = make("Plane").set<MeshRenderer>({ctx.primitives->plane()});   break;
        case AddKind::DirectionalLight: {
            t.rotation = {-0.70710678f, 0.0f, 0.0f, 0.70710678f};
            Light lc; lc.type = LightType::Directional; lc.intensity = 3.0f;
            e = make("DirectionalLight").set<Light>(lc);
            break;
        }
        case AddKind::PointLight: {
            t.position = {0.0f, 5.0f, 0.0f};
            Light lc; lc.type = LightType::Point; lc.range = 20.0f; lc.intensity = 25.0f;
            e = make("PointLight").set<Light>(lc);
            break;
        }
        case AddKind::Empty: e = make("Empty"); break;
        }
        ctx.editor.undoStack.pushEntityAdd(e);
        if (select) ctx.editor.selected = e;
        ctx.editor.sceneDirty = true;
        return e;
    }

    bool addRigidBodyToSelected(EngineContext& ctx) {
        flecs::entity s = ctx.editor.selected;
        if (!s.is_alive() || s.has<RigidBody>()) return false;
        s.set<RigidBody>({});
        ctx.editor.sceneDirty = true;
        return true;
    }

    // ── Reparent and delete: requested while drawing, applied after ─────────
    // Would dropping `child` under `newParent` (null = root) be allowed? Not
    // onto itself, and not onto its own descendant — that would make a cycle.
    static bool canReparent(flecs::entity child, flecs::entity newParent) {
        if (!child || !child.is_alive()) return false;
        if (!newParent) return true;
        return newParent.is_alive() && newParent != child && !isAncestorOf(child, newParent);
    }

    bool requestReparent(flecs::entity child, flecs::entity newParent) {
        if (!canReparent(child, newParent)) return false;
        m_reparent.child     = child;
        m_reparent.newParent = newParent;
        m_reparent.oldParent = child.target(flecs::ChildOf);
        if (const Transform* t = child.try_get<Transform>()) m_reparent.oldLocal = *t;
        m_reparent.pending   = true;
        return true;
    }
    bool requestUnparent(flecs::entity child) { return requestReparent(child, flecs::entity{}); }
    void requestDelete(flecs::entity e) { m_delete = e; }

    bool pending() const { return m_reparent.pending || m_delete; }

    // Outside any query. Reparenting keeps the entity where it is in the
    // world: its local transform is recomputed against the new parent (a
    // singular parent is clamped, not inverted to identity).
    void apply(EngineContext& ctx) {
        if (m_reparent.pending && m_reparent.child.is_alive()) {
            Reparent& r = m_reparent;
            float childWorld[16];
            getWorldMatrix(r.child, childWorld);
            bx::Vec3 wPos{0, 0, 0}; bx::Quaternion wRot{0, 0, 0, 1}; bx::Vec3 wScale{1, 1, 1};
            decomposeMatrix(childWorld, wPos, wRot, wScale);

            // Structural changes FIRST, the Transform reference AFTER: adding
            // or removing ChildOf moves the entity to another table, and a
            // reference taken before that points at the old storage.
            r.child.remove(flecs::ChildOf, flecs::Wildcard);
            const bool toParent = r.newParent && r.newParent.is_alive();
            if (toParent) r.child.add(flecs::ChildOf, r.newParent);
            Transform& t = r.child.get_mut<Transform>();
            if (toParent) {
                float parentWorld[16];
                getWorldMatrix(r.newParent, parentWorld);
                bx::Vec3 lPos{0, 0, 0}; bx::Quaternion lRot{0, 0, 0, 1}; bx::Vec3 lScale{1, 1, 1};
                worldToLocalPose(lPos, lRot, &lScale, childWorld, parentWorld);
                t.position = lPos; t.rotation = lRot; t.scale = lScale;
            } else {
                t.position = wPos; t.rotation = wRot; t.scale = wScale;
            }
            const uint64_t oldPid = (r.oldParent && r.oldParent.is_alive()) ? ensureEntityId(r.oldParent) : 0;
            const uint64_t newPid = (r.newParent && r.newParent.is_alive()) ? ensureEntityId(r.newParent) : 0;
            ctx.editor.undoStack.pushReparent(r.child, oldPid, r.oldLocal, newPid, t);
            ctx.editor.sceneDirty = true;
        }
        m_reparent = {};

        if (m_delete && m_delete.is_alive()) {
            ctx.editor.undoStack.pushEntityDelete(m_delete);
            if (ctx.editor.selected == m_delete) ctx.editor.selected = {};
            m_delete.destruct();
            ctx.editor.sceneDirty = true;
        }
        m_delete = {};
    }

private:
    struct Reparent {
        flecs::entity child, newParent, oldParent;
        Transform     oldLocal{};
        bool          pending = false;
    };
    Reparent      m_reparent;
    flecs::entity m_delete;
};

}  // namespace hier
