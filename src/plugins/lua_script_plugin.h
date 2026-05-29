#pragma once
#include "engine/plugin.h"
#include "engine/logger.h"
#include <lua.hpp>   // extern "C" wrapper around lua.h/lualib.h/lauxlib.h
#include <imgui.h>

// ── LuaScriptPlugin ────────────────────────────────────────────────────────
// Lua 5.4 (PUC-Rio) scripting backend. Owns one lua_State for the session.
//
// Stage 1 (here): boot the VM, open standard libraries, run a smoke test to
//   prove the build links and the interpreter executes on this platform.
// Stage 2: bind ScriptHost into Lua, load per-entity scripts, dispatch
//   onStart/onUpdate during Play.
// Stage 3: ScriptComponent inspector UI + a real gameplay test script.
class LuaScriptPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Scripting"; }
    const char* version() const override { return "5.4-lua"; }

    void onAttach(EngineContext&) override {
        m_L = luaL_newstate();
        if (!m_L) { LOG_ERROR("Script", "Failed to create Lua state"); return; }
        luaL_openlibs(m_L);

        // Smoke test — prove the VM actually executes bytecode.
        if (luaL_dostring(m_L, "return 2 + 2") == LUA_OK) {
            int r = (int)lua_tointeger(m_L, -1);
            lua_pop(m_L, 1);
            LOG_SUCCESS("Script", "%s online — smoke test 2+2=%d", LUA_RELEASE, r);
        } else {
            LOG_ERROR("Script", "Lua smoke test failed: %s", lua_tostring(m_L, -1));
            lua_pop(m_L, 1);
        }
    }

    void onDetach() override {
        if (m_L) { lua_close(m_L); m_L = nullptr; }
    }

    void onSimulationStart(flecs::world&) override {} // Stage 2
    void onSimulationStop()               override {} // Stage 2
    void onUpdate(flecs::world&, float)   override {} // Stage 2

    void onEditorUI() override {
        ImGui::TextDisabled("Backend:  Lua 5.4 (PUC-Rio)");
        ImGui::TextDisabled("State:    %s", m_L ? "VM running" : "not initialized");
        ImGui::Spacing();
        ImGui::TextDisabled("Stage 1:  VM boot + smoke test   [active]");
        ImGui::TextDisabled("Stage 2:  ScriptHost bindings + onStart/onUpdate");
        ImGui::TextDisabled("Stage 3:  ScriptComponent inspector + test script");
    }

private:
    lua_State* m_L = nullptr;
};
