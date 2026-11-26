#pragma once
// ── provider_resolve — one function decides which provider fills each kind ───
// WO-086, docs/plans/provider-selection.md §4–5 (DR-0037).
//
// Given a project's "providers" block, a TARGET (platform + flavour), every
// declared provider and the platform tables, `resolve` picks one provider per
// kind, or refuses by name. The same function runs in:
//   * engine_build --platform <p> [--flavour <f>], before anything is cooked or
//     compiled, reading the manifests from the source tree;
//   * a development runtime at startup, for the machine it runs on, reading what
//     CMake compiled in (engine_providers_built.h).
// A shipped player does not run it: engine_build has already chosen, and the
// player is built with exactly those providers (ENGINE_PROVIDERS_FIXED).
//
// The project may choose per target:
//
//   "renderer": { "default": "forward-bgfx", "web": "web", "server": "none" }
//
// The most specific key wins: "<platform>.<flavour>", then "<flavour>", then
// "<platform>", then "default". A plain string means every target. A kind the
// project does not name, or names for other targets only, takes the engine's
// default for that kind (engineDefaults()).
//
// Checks, in order, each refused by name with what was available:
//   1. the block is an object, each key a kind, each value a name or an object
//      of target keys, each target key a platform, flavour or platform.flavour;
//   2. the chosen name is a provider some manifest declares;
//   3. its platforms include the target's;          4. its flavours too;
//   5. the target's platform has every fact in its needs;
//   6. it was built (an SDK found, and so on), when that is known;
//   7. every service it requires comes from core or another chosen provider.
// One provider per kind holds by construction: each kind gets one choice.
//
// Pure: the standard library and nlohmann::json. No engine headers.
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace provsel {

// The provider kinds, in attach order.
const std::vector<std::string>& kinds();
// Services the engine itself provides to every provider.
const std::vector<std::string>& coreServices();
// The engine's choice for a kind the project does not name.
const nlohmann::json& engineDefaults();
const std::vector<std::string>& flavours();     // client, server

struct Decl {
    std::string kind, name, contract;
    std::vector<std::string> platforms, flavours, needs, sdk, requires_, provides;
    bool built = true;          // false: not built for this target, and why
    std::string whyNotBuilt;
};

struct Platform {
    std::string name;
    std::map<std::string, bool> facts;
};

struct Target { std::string platform, flavour; };

struct Alternative { std::string name, whyNot; };   // whyNot empty: usable
struct KindResult {
    std::string kind;
    std::string chosen;         // empty when refused
    std::string asked;          // the name the key asked for, chosen or not
    std::string key;            // which key chose it: "default", "web", ..., "engine default", "override"
    std::vector<Alternative> others;
};

struct Result {
    Target target;
    std::vector<KindResult> kinds;
    std::vector<std::string> refusals;
    bool ok() const { return refusals.empty(); }
    const std::string* chosen(const std::string& kind) const;
};

struct Inputs {
    nlohmann::json providers;                                      // the project's block; null: none
    std::vector<std::pair<std::string, std::string>> overrides;    // (kind, name): beat the project
    Target target;
    std::vector<Decl> decls;
    std::map<std::string, Platform> platforms;                     // every known platform, by name
};

Result resolve(const Inputs& in);

// Readers. False, with `why`, for a malformed input.
bool declFromManifest(const nlohmann::json& manifest, Decl& out, std::string& why);
bool platformFromTable(const nlohmann::json& table, Platform& out, std::string& why);

// The result as a table for people, or JSON for tools.
std::string explainText(const Result& r);
nlohmann::json explainJson(const Result& r);

} // namespace provsel
