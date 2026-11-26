// ── provider_resolve_test — the resolver, one check at a time (WO-086) ───────
// provsel::resolve picks one provider per kind for a target, or refuses by
// name. This drives it with hand-made declarations and platform tables, so it
// covers targets this machine cannot build (web, android) as engine_build will.
#include <cstdio>
#include <string>

#include "project/provider_resolve.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

using nlohmann::json;
static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
static std::string firstRefusal(const provsel::Result& r) { return r.refusals.empty() ? "" : r.refusals.front(); }

static provsel::Decl decl(const char* kind, const char* name, std::vector<std::string> platforms,
                          std::vector<std::string> needs = {}, std::vector<std::string> flavours = {"client", "server"}) {
    provsel::Decl d;
    d.kind = kind; d.name = name; d.contract = "c";
    d.platforms = std::move(platforms); d.needs = std::move(needs); d.flavours = std::move(flavours);
    return d;
}

static provsel::Inputs base(const char* platform, const char* flavour, json providers = nullptr) {
    provsel::Inputs in;
    in.providers = std::move(providers);
    in.target = {platform, flavour};
    const std::vector<std::string> desk = {"windows", "linux", "macos"};
    const std::vector<std::string> all = {"windows", "linux", "macos", "android", "ios", "web"};
    in.decls = {
        decl("physics", "jolt", desk), decl("physics", "none", all),
        decl("scripting", "lua", desk), decl("scripting", "none", all),
        decl("audio", "miniaudio", desk, {"audio_output"}), decl("audio", "none", all),
        decl("renderer", "forward-bgfx", desk, {"display"}, {"client"}),
        decl("renderer", "web", {"web"}, {"display"}, {"client"}),
        decl("renderer", "threaded", all, {"threads"}, {"client"}),
        decl("renderer", "none", all),
    };
    for (const char* p : {"windows", "linux", "macos", "android", "ios"})
        in.platforms[p] = {p, {{"display", true}, {"threads", true}, {"audio_output", true}}};
    in.platforms["web"] = {"web", {{"display", true}, {"threads", false}, {"audio_output", true}}};
    return in;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("provider_resolve_test\n");

    // ── Defaults: the engine's table, per flavour ─────────────────────────
    {
        auto r = provsel::resolve(base("macos", "client"));
        CHECK(r.ok() && *r.chosen("renderer") == "forward-bgfx" && *r.chosen("physics") == "jolt",
              "no block, macos client: forward-bgfx and jolt (engine default)");
        r = provsel::resolve(base("linux", "server"));
        CHECK(r.ok() && *r.chosen("renderer") == "none", "no block, linux server: renderer none (engine default `server`)");
    }

    // ── Precedence: platform.flavour > flavour > platform > default ───────
    {
        // On web nothing but `none` exists for the other kinds, so choose it.
        json p = {{"renderer", {{"default", "forward-bgfx"}, {"web", "web"}, {"server", "none"},
                                {"web.server", "none"}}},
                  {"physics", {{"default", "jolt"}, {"web", "none"}}},
                  {"scripting", {{"default", "lua"}, {"web", "none"}}},
                  {"audio", {{"default", "miniaudio"}, {"web", "none"}}}};
        auto r = provsel::resolve(base("web", "client", p));
        CHECK(r.ok() && *r.chosen("renderer") == "web" && r.kinds[3].key == "web", "web client: the `web` key");
        r = provsel::resolve(base("web", "server", p));
        CHECK(r.ok() && r.kinds[3].key == "web.server", "web server: `web.server` beats `server` and `web`");
        r = provsel::resolve(base("linux", "server", p));
        CHECK(r.ok() && r.kinds[3].key == "server", "linux server: `server` beats `default`");
        r = provsel::resolve(base("windows", "client", p));
        CHECK(r.ok() && r.kinds[3].key == "default", "windows client: `default`");
        json only = {{"renderer", {{"web", "web"}}}};
        r = provsel::resolve(base("macos", "client", only));
        CHECK(r.ok() && *r.chosen("renderer") == "forward-bgfx" && r.kinds[3].key == "engine default",
              "no key for this target: the engine default");
    }

    // ── 1. Shape ──────────────────────────────────────────────────────────
    {
        auto r = provsel::resolve(base("macos", "client", json::array()));
        CHECK(!r.ok() && has(firstRefusal(r), "must be an object"), "a block that is not an object");
        r = provsel::resolve(base("macos", "client", {{"phyiscs", "jolt"}}));
        CHECK(!r.ok() && has(firstRefusal(r), "'phyiscs' is not a provider kind"), "an unknown kind: %s", firstRefusal(r).c_str());
        r = provsel::resolve(base("macos", "client", {{"physics", 3}}));
        CHECK(!r.ok() && has(firstRefusal(r), "must be a provider name"), "a value that is not a name");
        r = provsel::resolve(base("macos", "client", {{"renderer", {{"widnows", "none"}}}}));
        CHECK(!r.ok() && has(firstRefusal(r), "'widnows' is not a target key") && has(firstRefusal(r), "macos"),
              "an unknown target key, with the keys allowed: %s", firstRefusal(r).c_str());
        r = provsel::resolve(base("macos", "client", {{"renderer", {{"web.lan", "none"}}}}));
        CHECK(!r.ok() && has(firstRefusal(r), "'web.lan'"), "platform.<not a flavour> is refused");
        r = provsel::resolve(base("macos", "client", {{"renderer", {{"default", 1}}}}));
        CHECK(!r.ok() && has(firstRefusal(r), "renderer.default"), "a target value that is not a name");
    }

    // ── 2–6. The choice itself ────────────────────────────────────────────
    {
        auto r = provsel::resolve(base("macos", "client", {{"physics", "havok"}}));
        CHECK(!r.ok() && has(firstRefusal(r), "no physics provider named 'havok'") && has(firstRefusal(r), "jolt, none"),
              "2. an undeclared name, with the declared ones: %s", firstRefusal(r).c_str());
        r = provsel::resolve(base("web", "client", {{"physics", "none"}, {"scripting", "none"}, {"audio", "none"}}));
        CHECK(!r.ok() && has(firstRefusal(r), "forward-bgfx") && has(firstRefusal(r), "not web"),
              "3. the default renderer on web is refused, by name: %s", firstRefusal(r).c_str());
        r = provsel::resolve(base("macos", "server", {{"renderer", "forward-bgfx"}}));
        CHECK(!r.ok() && has(firstRefusal(r), "not for server builds"), "4. a client-only renderer on a server");
        r = provsel::resolve(base("web", "client", {{"renderer", "threaded"}, {"physics", "none"},
                                                    {"scripting", "none"}, {"audio", "none"}}));
        CHECK(!r.ok() && has(firstRefusal(r), "needs 'threads'"), "5. a need the platform lacks: %s", firstRefusal(r).c_str());
        auto in = base("macos", "client");
        for (auto& d : in.decls) if (d.name == "miniaudio") { d.built = false; d.whyNotBuilt = "the SDK X was not found"; }
        r = provsel::resolve(in);
        CHECK(!r.ok() && has(firstRefusal(r), "the SDK X was not found"), "6. not built here, with the reason");
    }

    // ── 7. Services ──────────────────────────────────────────────────────
    {
        auto in = base("macos", "client");
        for (auto& d : in.decls) if (d.name == "jolt") d.requires_ = {"engine.jobs.v1", "voxel.world.v1"};
        auto r = provsel::resolve(in);
        CHECK(!r.ok() && has(firstRefusal(r), "requires the service 'voxel.world.v1'"),
              "a service nobody provides: %s", firstRefusal(r).c_str());
        for (auto& d : in.decls) if (d.name == "lua") d.provides = {"voxel.world.v1"};
        r = provsel::resolve(in);
        CHECK(r.ok(), "the same service provided by another chosen provider");
    }

    // ── Overrides, web resolved fully, and the explanation ───────────────
    {
        auto in = base("macos", "client", {{"physics", "jolt"}});
        in.overrides = {{"physics", "none"}};
        auto r = provsel::resolve(in);
        CHECK(r.ok() && *r.chosen("physics") == "none" && r.kinds[0].key == "override", "an override beats the project");
        in.overrides = {{"pathfinding", "recast"}};
        r = provsel::resolve(in);
        CHECK(!r.ok() && has(firstRefusal(r), "'pathfinding'"), "an override of an unknown kind");

        json web = {{"renderer", {{"default", "forward-bgfx"}, {"web", "web"}}},
                    {"physics", {{"default", "jolt"}, {"web", "none"}}},
                    {"scripting", {{"default", "lua"}, {"web", "none"}}},
                    {"audio", {{"default", "miniaudio"}, {"web", "none"}}}};
        r = provsel::resolve(base("web", "client", web));
        CHECK(r.ok() && *r.chosen("renderer") == "web", "a project written for web resolves there");
        const std::string text = provsel::explainText(r);
        CHECK(has(text, "renderer") && has(text, "web  (web)") && has(text, "not forward-bgfx: it supports"),
              "the explanation names the choice, its key, and why the others were not used");
        const json j = provsel::explainJson(r);
        CHECK(j["ok"] == true && j["kinds"].size() == 4 && j["platform"] == "web", "and as JSON");
    }

    // ── Readers ──────────────────────────────────────────────────────────
    {
        provsel::Decl d; std::string why;
        CHECK(provsel::declFromManifest(json{{"kind", "audio"}, {"name", "fmod"}, {"contract", "audio-provider"},
                                             {"platforms", {"windows"}}}, d, why) && d.flavours.size() == 2,
              "a manifest without flavours means every flavour");
        CHECK(!provsel::declFromManifest(json{{"kind", "audio"}, {"name", "fmod"}, {"contract", "a"}}, d, why)
              && has(why, "platforms"), "a manifest without platforms is refused");
        provsel::Platform p;
        CHECK(!provsel::platformFromTable(json{{"platform", "web"}, {"facts", {{"threads", "no"}}}}, p, why),
              "a fact that is not true/false is refused");
    }

    if (g_failures) { std::printf("provider_resolve_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("provider_resolve_test: ALL PASS\n");
    return 0;
}
