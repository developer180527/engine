#pragma once
// ── stock_plugins — the build's providers, and adding the chosen ones ────────
// WO-076. `registerStockProviders` is the ONE list of providers this build has
// (stock_providers.cpp): kind, name, factory, registered explicitly. The
// runtime registers its own renderers in init(); addProviders() registers the
// rest, so a host never names a provider class.
//
// A project chooses by name in project.json:
//
//   "providers": { "physics": "jolt", "scripting": "lua", "audio": "miniaudio" }
//
// and a host calls addProviders(engine) after init(). A name this build does not
// have is refused, by name, with the names it does have (DR-0035); nothing
// falls back to another provider.
//
// Adding a NEW provider (PhysX, FMOD, …): write the plugin (an IEnginePlugin
// that also implements the service interface; JoltPlugin is the template) and
// add one line to stock_providers.cpp. Gameplay code is untouched: it only ever
// sees IPhysicsService / the enginePhysics* C API.
#include <string>
#include <vector>

#include "runtime/provider_registry.h"

class EngineRuntime;

// Registers every provider this build has, renderers included, once per
// registry. False (with `why`) only for a defect in the list itself, such as
// two providers with one name.
bool registerStockProviders(providers::Registry& r, std::string* why = nullptr);

// Registers the build's list into the engine's registry, then adds the chosen
// physics, scripting and audio providers to its plugin list, in that order
// (attach order). A provider the host injected fills its
// kind instead of the project's choice. Checks every choice first and adds
// nothing if one is refused: false, the reason in engine.bootError() and the log.
bool addProviders(EngineRuntime& engine);
