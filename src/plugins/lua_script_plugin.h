#pragma once
#include "runtime/plugin.h"
#include "runtime/logger.h"
#include "engine_context.h"      // full EngineContext (plugin.h only fwd-decls it)
#include "io/project_context.h"  // ProjectContext::projectRoot
#include "runtime/scripting/script_host.h"
#include "runtime/scripting/lua_bindings.h"
#include "components/script_component.h"
#include "components/collision_events.h"
#include <lua.hpp>
#include <imgui.h>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <string>

// ── LuaScriptPlugin ────────────────────────────────────────────────────────
// Lua 5.4 backend with per-entity script lifecycle.
//
// Script file convention — returns a table of methods (a "behavior"):
//   local M = {}
//   function M:onStart()        self.speed = 5 end
//   function M:onUpdate(dt)     ... self.entity:getTransform() ... end
//   function M:onDestroy()      ... end
//   return M
//
// Each entity with a ScriptComponent gets an instance table (state lives on
// self; self.entity is the entity handle). Modules are cached by path and
// cleared on Stop, so editing a script and pressing Play reloads it.
class LuaScriptPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Scripting"; }
    const char* version() const override { return "5.4-lua"; }

    void onAttach(EngineContext& ec) override {
        m_projectRoot = ec.project.projectRoot;
        if (ec.assetService)
            m_host.setAssetService(ec.assetService);
        if (ec.sceneService)
            m_host.setSceneService(ec.sceneService);
        m_L = luaL_newstate();
        if (!m_L) { LOG_ERROR("Script", "Failed to create Lua state"); return; }
        openSafeLibs(m_L);
        LuaBindings::install(m_L, &m_host);
        LOG_SUCCESS("Script", "%s online — bindings installed", LUA_RELEASE);
    }

    void onDetach() override {
        clearInstances(); clearModules();
        if (m_L) { lua_close(m_L); m_L = nullptr; }
    }

    void onSimulationStart(flecs::world& gw) override {
        if (!m_L) return;
        m_host.setWorld(&gw);
        m_host.beginSession();   // invalidate entity refs stashed in prior plays
        m_elapsed = 0.0; m_frame = 0;
        bindPhysics(gw);         // grab the physics service if it is already up
        bindAudio(gw);           // and the audio service

        // Collect first (don't instantiate inside the query — onStart may
        // make structural changes). Then instantiate inside a defer scope.
        std::vector<Init> toInit;
        gw.query_builder<ScriptComponent>().build()
            .each([&](flecs::entity e, ScriptComponent& sc) {
                if (!sc.scriptPath.empty()) toInit.push_back({e, sc.scriptPath});
            });

        // Autorun: scan scripts/autorun/ and create headless entities for each
        // .lua file found. These participate in the full lifecycle (onStart,
        // onUpdate, onDestroy) but don't require manual entity creation.
        // Ideal for test scripts, global systems, and one-shot setup code.
        collectAutorunScripts(gw, toInit);

        gw.defer_begin();
        for (auto& i : toInit) instantiate(i.e, i.path);
        gw.defer_end();
        LOG_INFO("Script", "Play: %zu script instance(s) (%zu autorun)",
                 m_instances.size(), m_autorunEntities.size());
    }

    void onSimulationStop() override {
        for (auto& inst : m_instances)
            if (!inst.errored) dispatch(inst, "onDestroy", false, 0.0f);
        clearInstances();
        clearModules();          // next Play reloads scripts from disk
        // Destroy autorun entities — they were created by us, not the scene
        for (auto eid : m_autorunEntities) {
            flecs::entity e = m_host.world()->entity(eid);
            if (e.is_alive()) e.destruct();
        }
        m_autorunEntities.clear();
        m_host.setPhysicsService(nullptr);
        m_host.setAudioService(nullptr);
        // AssetService is NOT cleared — it's engine infrastructure, not
        // per-simulation state. Scripts can safely preload assets in onAttach.
        m_host.setWorld(nullptr);
    }

    void onUpdate(flecs::world& gw, float dt) override {
        if (!m_L) return;
        bindPhysics(gw);         // order-independent: service is up by first update
        bindAudio(gw);
        m_elapsed += dt; ++m_frame;
        m_host.setFrame(dt, m_elapsed, m_frame);
        gw.defer_begin();        // defer spawn/destroy issued from scripts
        for (auto& inst : m_instances) {
            if (inst.errored) continue;
            if (!gw.entity(inst.id).is_alive()) continue;
            dispatch(inst, "onUpdate", true, dt);
        }
        gw.defer_end();
    }

    // Post-physics: deliver this frame's contacts to scripts as
    // onCollisionEnter(other) / onCollisionExit(other). Physics::onPostPhysics
    // (registered first) populated CollisionEvents just before this runs.
    void onPostPhysics(flecs::world& gw) override {
        if (!m_L || m_instances.empty()) return;
        gw.defer_begin();   // handlers may spawn/destroy
        gw.query_builder<const CollisionEvents>().build()
            .each([&](flecs::entity e, const CollisionEvents& ce) {
                Instance* inst = findInstance(e.id());
                if (!inst || inst->errored) return;
                for (auto other : ce.entered) dispatchEntity(*inst, "onCollisionEnter", other);
                for (auto other : ce.exited)  dispatchEntity(*inst, "onCollisionExit",  other);
            });
        gw.defer_end();
    }

    void onEditorUI() override {
        ImGui::TextDisabled("Backend:   Lua 5.4 (PUC-Rio)");
        ImGui::TextDisabled("State:     %s", m_L ? "VM running" : "off");
        ImGui::TextDisabled("Instances: %d", (int)m_instances.size());
        ImGui::Spacing();
        ImGui::TextDisabled("Lifecycle: onStart / onUpdate / onDestroy");
        ImGui::TextDisabled("Scripts reload on each Play");
    }

private:
    struct Init { flecs::entity e; std::string path; };
    struct Instance { flecs::entity_t id; int ref; bool errored = false; };

    // Sandbox: open only base/table/string/math/coroutine. io/os/package/
    // debug are never opened; strip code-loading base globals.
    static void openSafeLibs(lua_State* L) {
        luaL_requiref(L, LUA_GNAME,       luaopen_base,      1); lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,     1); lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string,    1); lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,      1); lua_pop(L, 1);
        luaL_requiref(L, LUA_COLIBNAME,   luaopen_coroutine, 1); lua_pop(L, 1);
        static const char* kStrip[] = {"dofile","loadfile","load","require","collectgarbage",nullptr};
        for (int i = 0; kStrip[i]; ++i) { lua_pushnil(L); lua_setglobal(L, kStrip[i]); }
    }

    // Reject absolute paths, ".." components, and any path that resolves
    // outside the project root.
    bool validateScriptPath(const std::string& rel, std::filesystem::path& outFull) {
        namespace fs = std::filesystem;
        fs::path p(rel);
        if (p.is_absolute()) {
            LOG_ERROR("Script", "Rejected absolute script path: %s", rel.c_str()); return false;
        }
        for (const auto& part : p)
            if (part == "..") { LOG_ERROR("Script", "Rejected '..' in script path: %s", rel.c_str()); return false; }
        fs::path root = fs::weakly_canonical(m_projectRoot);
        fs::path full = fs::weakly_canonical(m_projectRoot / p);
        fs::path relToRoot = full.lexically_relative(root);
        bool escapes = relToRoot.empty();
        if (!escapes) { auto f = relToRoot.begin(); if (f != relToRoot.end() && *f == "..") escapes = true; }
        if (escapes) { LOG_ERROR("Script", "Script path escapes project root: %s", rel.c_str()); return false; }
        outFull = full; return true;
    }

    int loadModule(const std::string& path) {
        auto it = m_moduleRefs.find(path);
        if (it != m_moduleRefs.end()) return it->second;
        std::filesystem::path full;
        if (!validateScriptPath(path, full)) return LUA_NOREF;
        if (luaL_loadfile(m_L, full.string().c_str()) != LUA_OK) {
            LOG_ERROR("Script", "load %s: %s", path.c_str(), lua_tostring(m_L, -1));
            lua_pop(m_L, 1); return LUA_NOREF;
        }
        if (lua_pcall(m_L, 0, 1, 0) != LUA_OK) {
            LOG_ERROR("Script", "run %s: %s", path.c_str(), lua_tostring(m_L, -1));
            lua_pop(m_L, 1); return LUA_NOREF;
        }
        if (!lua_istable(m_L, -1)) {
            LOG_ERROR("Script", "%s must 'return' a table of methods", path.c_str());
            lua_pop(m_L, 1); return LUA_NOREF;
        }
        int ref = luaL_ref(m_L, LUA_REGISTRYINDEX);
        m_moduleRefs[path] = ref;
        return ref;
    }

    void instantiate(flecs::entity e, const std::string& path) {
        int moduleRef = loadModule(path);
        if (moduleRef == LUA_NOREF) return;
        // instance = setmetatable({ entity = <ud> }, { __index = module })
        lua_newtable(m_L);                              // instance
        lua_rawgeti(m_L, LUA_REGISTRYINDEX, moduleRef); // module
        lua_newtable(m_L);                              // mt
        lua_pushvalue(m_L, -2);                         // module (dup)
        lua_setfield(m_L, -2, "__index");               // mt.__index = module
        lua_setmetatable(m_L, -3);                      // setmetatable(instance, mt)
        lua_pop(m_L, 1);                                // pop module
        LuaBindings::pushEntity(m_L, e, &m_host);       // entity userdata (epoch-stamped)
        lua_setfield(m_L, -2, "entity");                // instance.entity = ud
        int instRef = luaL_ref(m_L, LUA_REGISTRYINDEX); // pop instance
        m_instances.push_back({ (flecs::entity_t)e.id(), instRef, false });
        dispatch(m_instances.back(), "onStart", false, 0.0f);
    }

    // instance:method([dt]) via __index → module. pcall-guarded.
    void dispatch(Instance& inst, const char* method, bool hasDt, float dt) {
        lua_rawgeti(m_L, LUA_REGISTRYINDEX, inst.ref);  // instance
        lua_getfield(m_L, -1, method);                  // method
        if (!lua_isfunction(m_L, -1)) { lua_pop(m_L, 2); return; }
        lua_pushvalue(m_L, -2);                         // self
        int nargs = 1;
        if (hasDt) { lua_pushnumber(m_L, dt); nargs = 2; }
        if (lua_pcall(m_L, nargs, 0, 0) != LUA_OK) {
            LOG_ERROR("Script", "%s: %s", method, lua_tostring(m_L, -1));
            lua_pop(m_L, 1);
            inst.errored = true;   // stop re-running a broken script
        }
        lua_pop(m_L, 1);           // pop instance
    }

    // Like dispatch(), but passes another entity as the sole argument:
    // instance:method(other). Used for collision callbacks.
    void dispatchEntity(Instance& inst, const char* method, flecs::entity_t otherId) {
        lua_rawgeti(m_L, LUA_REGISTRYINDEX, inst.ref);  // instance
        lua_getfield(m_L, -1, method);                  // method
        if (!lua_isfunction(m_L, -1)) { lua_pop(m_L, 2); return; }
        lua_pushvalue(m_L, -2);                         // self
        LuaBindings::pushEntity(m_L, m_host.world()->entity(otherId), &m_host);
        if (lua_pcall(m_L, 2, 0, 0) != LUA_OK) {        // self + other
            LOG_ERROR("Script", "%s: %s", method, lua_tostring(m_L, -1));
            lua_pop(m_L, 1);
            inst.errored = true;
        }
        lua_pop(m_L, 1);                                // pop instance
    }

    Instance* findInstance(flecs::entity_t id) {
        for (auto& inst : m_instances) if (inst.id == id) return &inst;
        return nullptr;
    }

    void bindAudio(flecs::world& gw) {
        if (const AudioServiceRef* ref = gw.try_get<AudioServiceRef>())
            m_host.setAudioService(ref->svc);
    }
    void bindPhysics(flecs::world& gw) {
        if (const PhysicsServiceRef* ref = gw.try_get<PhysicsServiceRef>())
            m_host.setPhysicsService(ref->svc);
    }
    // Scan scripts/autorun/ for .lua files. For each one, create a headless
    // entity with Name + ScriptComponent and add it to the init list.
    void collectAutorunScripts(flecs::world& gw,
                               std::vector<Init>& toInit) {
        namespace fs = std::filesystem;
        fs::path autoDir = m_projectRoot / "scripts" / "autorun";
        if (!fs::exists(autoDir) || !fs::is_directory(autoDir)) return;

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(autoDir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lua") continue;

            std::string relPath = fs::relative(entry.path(), m_projectRoot, ec).string();
            if (ec) continue;

            // Create a headless entity for this autorun script
            std::string label = "[autorun] " + entry.path().stem().string();
            flecs::entity e = gw.entity();
            e.set<Name>({label});
            e.set<Transform>({});      // required by scene queries
            e.set<ScriptComponent>({relPath});

            toInit.push_back({e, relPath});
            m_autorunEntities.push_back(e.id());
            LOG_INFO("Script", "Autorun: %s → %s", label.c_str(), relPath.c_str());
        }
    }

    void clearInstances() {
        if (m_L) for (auto& inst : m_instances) luaL_unref(m_L, LUA_REGISTRYINDEX, inst.ref);
        m_instances.clear();
    }
    void clearModules() {
        if (m_L) for (auto& kv : m_moduleRefs) luaL_unref(m_L, LUA_REGISTRYINDEX, kv.second);
        m_moduleRefs.clear();
    }

    lua_State*  m_L = nullptr;
    ScriptHost  m_host{nullptr};
    std::filesystem::path m_projectRoot;
    std::unordered_map<std::string,int> m_moduleRefs;
    std::vector<Instance>        m_instances;
    std::vector<flecs::entity_t> m_autorunEntities;  // headless entities for autorun scripts
    double   m_elapsed = 0.0;
    uint64_t m_frame   = 0;
};
