// stock_providers.cpp — the build's one list of providers (WO-076).
// See stock_plugins.h. Explicit calls, not static initialisers: a static
// library drops objects nobody references, so self-registration would silently
// lose providers from a shipped game.
#include "plugins/stock_plugins.h"

#include "runtime/runtime.h"
#include "core/logger.h"

// Which providers this target builds: one macro per manifest, 1 or 0, written
// by cmake/EngineProviders.cmake (WO-085). A provider is registered only where
// it is built; one that is not is registered as unavailable, with the reason.
#include "engine_providers_built.h"
#if !defined(ENGINE_PROVIDER_PHYSICS_JOLT) || !defined(ENGINE_PROVIDER_PHYSICS_NONE) || \
    !defined(ENGINE_PROVIDER_SCRIPTING_LUA) || !defined(ENGINE_PROVIDER_SCRIPTING_NONE) || \
    !defined(ENGINE_PROVIDER_AUDIO_MINIAUDIO) || !defined(ENGINE_PROVIDER_AUDIO_NONE)
#error "a provider in this list has no manifest (no engine_provider() call in src/CMakeLists.txt)"
#endif

#if ENGINE_PROVIDER_PHYSICS_JOLT
#include "plugins/jolt_plugin.h"
#endif
#if ENGINE_PROVIDER_PHYSICS_NONE
#include "plugins/null_physics_plugin.h"
#endif
#if ENGINE_PROVIDER_SCRIPTING_LUA
#include "plugins/lua_script_plugin.h"
#endif
#if ENGINE_PROVIDER_SCRIPTING_NONE
#include "plugins/null_script_plugin.h"
#endif
#if ENGINE_PROVIDER_AUDIO_MINIAUDIO
#include "plugins/audio_plugin.h"
#endif
#if ENGINE_PROVIDER_AUDIO_NONE
#include "plugins/null_audio_plugin.h"
#endif

using providers::Kind;

bool registerStockProviders(providers::Registry& r, std::string* why) {
    EngineRuntime::registerRenderers(r);
    if (!r.claimList("stock")) return true;   // already registered into this one
    bool ok = true;
#if ENGINE_PROVIDER_PHYSICS_JOLT
    ok &= r.addPlugin(Kind::Physics, "jolt", "Jolt Physics, on the engine job pool",
                      [](const providers::ProviderContext& c) { return std::make_shared<JoltPlugin>(c); }, why);
#endif
#if ENGINE_PROVIDER_PHYSICS_NONE
    ok &= r.addPlugin(Kind::Physics, "none", "no physics: bodies do not move",
                      [](const providers::ProviderContext&) { return std::make_shared<NullPhysicsPlugin>(); }, why);
#endif
#if ENGINE_PROVIDER_SCRIPTING_LUA
    ok &= r.addPlugin(Kind::Scripting, "lua", "Lua scripts through ScriptHost",
                      [](const providers::ProviderContext& c) { return std::make_shared<LuaScriptPlugin>(c); }, why);
#endif
#if ENGINE_PROVIDER_SCRIPTING_NONE
    ok &= r.addPlugin(Kind::Scripting, "none", "no scripting",
                      [](const providers::ProviderContext&) { return std::make_shared<NullScriptPlugin>(); }, why);
#endif
#if ENGINE_PROVIDER_AUDIO_MINIAUDIO
    ok &= r.addPlugin(Kind::Audio, "miniaudio", "miniaudio behind EngineAudioProviderV1",
                      [](const providers::ProviderContext& c) { return std::make_shared<AudioPlugin>(c); }, why);
#endif
#if ENGINE_PROVIDER_AUDIO_NONE
    ok &= r.addPlugin(Kind::Audio, "none", "no audio device",
                      [](const providers::ProviderContext&) { return std::make_shared<NullAudioPlugin>(); }, why);
#endif
    for (const providers::BuiltRow& d : providers::builtRows()) {
        Kind k;
        if (d.built || !providers::kindFromName(d.kind, k) || k == Kind::Renderer) continue;
        r.addUnavailable(k, d.name, d.whyNot);
    }
    return ok;
}

providers::ProviderContext providerContextFor(EngineRuntime& engine) {
    providers::ProviderContext pc;
    if (engine.hasProject()) {
        pc.projectRoot = engine.project().projectRoot;
        pc.assetsRoot  = engine.project().assetsRoot;
    }
    pc.scriptHost = engine.ctx().scriptHost;
    pc.debugDraw  = &engine.debugDraw();
    return pc;
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
    const providers::ProviderContext pc = providerContextFor(engine);
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
        engine.plugins().add(e->makePlugin(pc));
    }
    return true;
}
