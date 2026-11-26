// stock_providers.cpp — the build's one list of providers (WO-076).
// See stock_plugins.h. Explicit calls, not static initialisers: a static
// library drops objects nobody references, so self-registration would silently
// lose providers from a shipped game.
#include "plugins/stock_plugins.h"

#include "runtime/runtime.h"
#include "core/logger.h"

#include "plugins/jolt_plugin.h"
#include "plugins/null_physics_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/null_script_plugin.h"
#include "plugins/audio_plugin.h"
#include "plugins/null_audio_plugin.h"

using providers::Kind;

bool registerStockProviders(providers::Registry& r, std::string* why) {
    EngineRuntime::registerRenderers(r);
    if (!r.claimList("stock")) return true;   // already registered into this one
    bool ok = true;
    ok &= r.addPlugin(Kind::Physics, "jolt", "Jolt Physics, on the engine job pool",
                      [] { return std::make_shared<JoltPlugin>(); }, why);
    ok &= r.addPlugin(Kind::Physics, "none", "no physics: bodies do not move",
                      [] { return std::make_shared<NullPhysicsPlugin>(); }, why);
    ok &= r.addPlugin(Kind::Scripting, "lua", "Lua scripts through ScriptHost",
                      [] { return std::make_shared<LuaScriptPlugin>(); }, why);
    ok &= r.addPlugin(Kind::Scripting, "none", "no scripting",
                      [] { return std::make_shared<NullScriptPlugin>(); }, why);
    ok &= r.addPlugin(Kind::Audio, "miniaudio", "miniaudio behind EngineAudioProviderV1",
                      [] { return std::make_shared<AudioPlugin>(); }, why);
    ok &= r.addPlugin(Kind::Audio, "none", "no audio device",
                      [] { return std::make_shared<NullAudioPlugin>(); }, why);
    return ok;
}

bool addProviders(EngineRuntime& engine) {
    std::string why;
    if (!registerStockProviders(engine.providerRegistry(), &why)) {
        why = "the build's provider list is wrong: " + why;
        LOG_ERROR("Providers", "%s", why.c_str());
        engine.setBootError(why);
        return false;
    }
    if (!engine.checkProviderChoices(why)) {
        LOG_ERROR("Providers", "%s", why.c_str());
        engine.setBootError(why);
        return false;
    }
    const auto& reg = engine.providerRegistry();
    const auto& p   = engine.project().providers;
    const std::pair<Kind, const std::string*> order[] = {
        {Kind::Physics, &p.physics}, {Kind::Scripting, &p.scripting}, {Kind::Audio, &p.audio}};
    for (const auto& [kind, name] : order) {
        if (auto inj = reg.injected(kind)) {
            LOG_INFO("Providers", "%s: provided by the host", providers::kindName(kind));
            engine.plugins().add(std::move(inj));
            continue;
        }
        const providers::Entry* e = reg.find(kind, *name);   // checked above
        LOG_INFO("Providers", "%s: %s", providers::kindName(kind), name->c_str());
        engine.plugins().add(e->makePlugin());
    }
    return true;
}
