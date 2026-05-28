#pragma once
#include <string>
#include <deque>
#include <nlohmann/json.hpp>
#include <flecs.h>
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/camera.h"
#include "components/rigid_body.h"
#include "engine/logger.h"

// ── Command types ──────────────────────────────────────────────────────────
enum class UndoCmdType { Transform, EntityAdd, EntityDelete, Reparent };

struct UndoCommand {
    UndoCmdType  type;
    std::string  description; // shown in Edit menu e.g. "Move TV"
    nlohmann::json before;   // state before the action
    nlohmann::json after;    // state after the action
};

// ── UndoStack ──────────────────────────────────────────────────────────────
// Session-only. Commands reference entities by Name component value.
// Max kMaxDepth entries — oldest are dropped when exceeded.
class UndoStack {
public:
    static constexpr int kMaxDepth = 200;

    // Record a command (the action has already been applied by the caller).
    // Clears redo history.
    void push(UndoCommand cmd) {
        if (m_index < (int)m_history.size() - 1)
            m_history.erase(m_history.begin() + m_index + 1, m_history.end());
        m_history.push_back(std::move(cmd));
        m_index = (int)m_history.size() - 1;
        while ((int)m_history.size() > kMaxDepth) {
            m_history.pop_front();
            if (m_index > 0) --m_index;
        }
    }

    bool undo(flecs::world& ecs) {
        if (!canUndo()) return false;
        apply(ecs, m_history[m_index], /*reverse=*/true);
        --m_index;
        return true;
    }

    bool redo(flecs::world& ecs) {
        if (!canRedo()) return false;
        ++m_index;
        apply(ecs, m_history[m_index], /*reverse=*/false);
        return true;
    }

    bool        canUndo()          const { return m_index >= 0; }
    bool        canRedo()          const { return m_index < (int)m_history.size()-1; }
    std::string undoDescription()  const { return canUndo() ? m_history[m_index].description : ""; }
    std::string redoDescription()  const { return canRedo() ? m_history[m_index+1].description : ""; }
    void        clear()                  { m_history.clear(); m_index = -1; }

    // ── Factory helpers ────────────────────────────────────────────────────
    void pushTransform(const std::string& name,
                       const Transform& before, const Transform& after) {
        if (name.empty()) return;
        UndoCommand c;
        c.type = UndoCmdType::Transform;
        c.description = "Move " + name;
        c.before = serTf(before); c.before["entity"] = name;
        c.after  = serTf(after);  c.after["entity"]  = name;
        push(std::move(c));
    }

    void pushEntityAdd(flecs::entity e) {
        UndoCommand c;
        c.type = UndoCmdType::EntityAdd;
        c.after = snapshot(e);
        c.description = "Add " + c.after.value("name", "Entity");
        push(std::move(c));
    }

    // Call BEFORE destroying the entity.
    void pushEntityDelete(flecs::entity e) {
        UndoCommand c;
        c.type = UndoCmdType::EntityDelete;
        c.before = snapshot(e);
        c.description = "Delete " + c.before.value("name", "Entity");
        push(std::move(c));
    }

    void pushReparent(const std::string& entityName,
                      const std::string& oldParent, const Transform& oldLocal,
                      const std::string& newParent, const Transform& newLocal) {
        UndoCommand c;
        c.type = UndoCmdType::Reparent;
        c.description = "Reparent " + entityName;
        c.before = { {"entity", entityName}, {"parent", oldParent}, {"transform", serTf(oldLocal)} };
        c.after  = { {"entity", entityName}, {"parent", newParent}, {"transform", serTf(newLocal)} };
        push(std::move(c));
    }

private:
    std::deque<UndoCommand> m_history;
    int                     m_index = -1;

    // ── Dispatch ───────────────────────────────────────────────────────────
    void apply(flecs::world& ecs, const UndoCommand& cmd, bool reverse) {
        switch (cmd.type) {
        case UndoCmdType::Transform:
            applyTransform(ecs, reverse ? cmd.before : cmd.after);    break;
        case UndoCmdType::EntityAdd:
            if (reverse) destroyByName(ecs, cmd.after.value("name",""));
            else         spawnFromSnapshot(ecs, cmd.after);             break;
        case UndoCmdType::EntityDelete:
            if (reverse) spawnFromSnapshot(ecs, cmd.before);
            else         destroyByName(ecs, cmd.before.value("name","")); break;
        case UndoCmdType::Reparent:
            applyReparent(ecs, reverse ? cmd.before : cmd.after);      break;
        }
    }

    static void applyTransform(flecs::world& ecs, const nlohmann::json& j) {
        std::string name = j.value("entity", "");
        flecs::entity e = ecs.lookup(name.c_str());
        if (!e || !e.is_alive()) {
            LOG_WARN("Undo", "Entity not found: %s", name.c_str()); return;
        }
        Transform& t = e.get_mut<Transform>();
        desTf(j, t);
    }

    static void applyReparent(flecs::world& ecs, const nlohmann::json& j) {
        std::string name   = j.value("entity", "");
        std::string parent = j.value("parent", "");
        flecs::entity e = ecs.lookup(name.c_str());
        if (!e || !e.is_alive()) return;
        e.remove(flecs::ChildOf, flecs::Wildcard);
        if (!parent.empty()) {
            flecs::entity p = ecs.lookup(parent.c_str());
            if (p && p.is_alive()) e.add(flecs::ChildOf, p);
        }
        if (j.contains("transform")) {
            Transform& t = e.get_mut<Transform>();
            desTf(j["transform"], t);
        }
    }

    static void destroyByName(flecs::world& ecs, const std::string& name) {
        if (name.empty()) return;
        flecs::entity e = ecs.lookup(name.c_str());
        if (e && e.is_alive()) e.destruct();
    }

    static void spawnFromSnapshot(flecs::world& ecs, const nlohmann::json& j) {
        std::string name = j.value("name", "");
        if (name.empty()) return;
        // Don't double-spawn
        if (flecs::entity ex = ecs.lookup(name.c_str()); ex && ex.is_alive()) return;
        Transform t{};
        if (j.contains("transform")) desTf(j["transform"], t);
        auto e = ecs.entity(name.c_str()).set<Transform>(t).set<Name>({name});
        if (j.contains("meshRenderer")) {
            uint32_t hid = j["meshRenderer"].value("handleId", 0u);
            if (hid > 0) { MeshHandle h; h.id = hid; e.set<MeshRenderer>({h}); }
        }
        if (j.contains("camera")) {
            const auto& jc = j["camera"];
            Camera cam;
            cam.isPrimary  = jc.value("isPrimary", false);
            cam.projection = (ProjectionType)jc.value("projection", 0);
            cam.fov        = jc.value("fov", 60.0f);
            cam.nearPlane  = jc.value("near", 0.1f);
            cam.farPlane   = jc.value("far", 1000.0f);
            e.set<Camera>(cam);
        }
        if (j.contains("rigidBody")) {
            const auto& jr = j["rigidBody"];
            RigidBody rb;
            rb.bodyType    = (PhysicsBodyType)jr.value("bodyType", 1);
            rb.shape       = (PhysicsShape)   jr.value("shape", 0);
            rb.mass        = jr.value("mass", 1.0f);
            rb.restitution = jr.value("restitution", 0.3f);
            rb.friction    = jr.value("friction", 0.6f);
            rb.useGravity  = jr.value("useGravity", true);
            e.set<RigidBody>(rb);
        }
        std::string par = j.value("parent", "");
        if (!par.empty()) {
            flecs::entity p = ecs.lookup(par.c_str());
            if (p && p.is_alive()) e.add(flecs::ChildOf, p);
        }
    }

    // ── Serialization ──────────────────────────────────────────────────────
    static nlohmann::json serTf(const Transform& t) {
        return {
            {"position", {t.position.x, t.position.y, t.position.z}},
            {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}},
            {"scale",    {t.scale.x,    t.scale.y,    t.scale.z}}
        };
    }
    static void desTf(const nlohmann::json& j, Transform& t) {
        if (j.contains("position"))
            t.position = {j["position"][0], j["position"][1], j["position"][2]};
        if (j.contains("rotation"))
            t.rotation = {j["rotation"][0], j["rotation"][1], j["rotation"][2], j["rotation"][3]};
        if (j.contains("scale"))
            t.scale = {j["scale"][0], j["scale"][1], j["scale"][2]};
    }
    static nlohmann::json snapshot(flecs::entity e) {
        nlohmann::json j;
        const Name* n = e.try_get<Name>(); j["name"] = n ? n->value : "";
        const Transform* t = e.try_get<Transform>();
        if (t) j["transform"] = serTf(*t);
        flecs::entity par = e.target(flecs::ChildOf);
        if (par && par.is_alive()) {
            const Name* pn = par.try_get<Name>(); j["parent"] = pn ? pn->value : "";
        }
        if (const MeshRenderer* mr = e.try_get<MeshRenderer>())
            j["meshRenderer"]["handleId"] = mr->mesh.id;
        if (const Camera* cam = e.try_get<Camera>())
            j["camera"] = {{"isPrimary",cam->isPrimary},{"projection",(int)cam->projection},
                           {"fov",cam->fov},{"near",cam->nearPlane},{"far",cam->farPlane}};
        if (const RigidBody* rb = e.try_get<RigidBody>())
            j["rigidBody"] = {{"bodyType",(int)rb->bodyType},{"shape",(int)rb->shape},
                              {"mass",rb->mass},{"restitution",rb->restitution},
                              {"friction",rb->friction},{"useGravity",rb->useGravity}};
        return j;
    }
};
