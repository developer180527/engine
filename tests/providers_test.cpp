// ── providers_test — providers are chosen by NAME from the build's registry ──
// WO-076. A project.json "providers" block (or EngineConfig overrides) picks a
// physics, scripting, audio and renderer provider by name from the registry
// (runtime/provider_registry.h, filled by plugins/stock_providers.cpp). An
// unknown name or kind is refused at startup, by name, with the names this
// build has (DR-0035). A host may inject its own provider object. Discriminator
// for physics: JoltPlugin publishes PhysicsServiceRef into the sim world at
// onSimulationStart; the `none` provider does not. Headless. Exits non-zero on
// the first failure.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/scripting/script_services.h"
#include "plugins/stock_plugins.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static fs::path makeProject(const char* name, const char* providersJson) {
    const fs::path root = fs::temp_directory_path() / "engine_providers_test" / name;
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    std::ofstream f(root / "project.json");
    f << "{\"name\":\"" << name << "\",\"assetRoot\":\"assets\","
      << "\"providers\":" << providersJson << "}";
    return root;
}

static EngineConfig configFor(const fs::path& root) {
    EngineConfig cfg;
    cfg.projectRoot       = root;
    cfg.defaultScene      = false;
    cfg.openAssetDatabase = false;
    return cfg;
}

// What one boot did: whether init and addProviders accepted the choices, why
// not, the renderer chosen, the plugins added, and whether physics published.
struct Boot {
    bool initOk = false, providersOk = false, physicsPublished = false;
    std::string error, renderer;
    std::vector<std::string> plugins;
};

// A fake physics provider handed to the engine by the host, without a name.
struct FakePhysics final : IEnginePlugin {
    int* attached;
    explicit FakePhysics(int* a) : attached(a) {}
    const char* name()    const override { return "FakePhysics"; }
    const char* version() const override { return "test"; }
    void onAttach(RuntimeContext&) override { ++*attached; }
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}
};

static Boot boot(const EngineConfig& cfg, std::shared_ptr<IEnginePlugin> injectPhysics = nullptr) {
    Boot b;
    EngineRuntime engine;
    if (injectPhysics) engine.providerRegistry().inject(providers::Kind::Physics, injectPhysics);
    b.initOk = engine.init(cfg, std::make_unique<HeadlessPlatform>());
    b.error  = engine.bootError();
    if (b.initOk) {
        b.renderer    = engine.rendererName();
        b.providersOk = addProviders(engine);
        if (!b.providersOk) b.error = engine.bootError();
        for (const auto& p : engine.plugins().all()) b.plugins.push_back(p->name());
        if (b.providersOk) {
            engine.attachPlugins();
            engine.startSimulation();
            engine.tickSimulation(1.0f / 60.0f);
            b.physicsPublished = engine.simWorld().try_get<PhysicsServiceRef>() != nullptr;
            engine.stopSimulation();
        }
    }
    engine.shutdown();
    return b;
}

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("providers_test: provider selection by name (WO-076)\n");

    // ── 1. The registry itself ───────────────────────────────────────────
    {
        providers::Registry r;
        std::string why;
        CHECK(registerStockProviders(r, &why), "the build's list registers cleanly");
        CHECK(r.namesList(providers::Kind::Physics) == "jolt, none", "physics: jolt, none");
        CHECK(r.namesList(providers::Kind::Scripting) == "lua, none", "scripting: lua, none");
        CHECK(r.namesList(providers::Kind::Audio) == "miniaudio, none", "audio: miniaudio, none");
        CHECK(r.find(providers::Kind::Renderer, "none") != nullptr, "renderer: none is registered");
        for (providers::Kind k : providers::kAllKinds)
            CHECK(r.find(k, "none") != nullptr, "%s has a `none` provider", providers::kindName(k));
        CHECK(!r.addPlugin(providers::Kind::Physics, "jolt", "", [](const providers::ProviderContext&) { return nullptr; }, &why)
              && has(why, "two physics providers named 'jolt'"),
              "a second provider with the same name is refused: %s", why.c_str());
        CHECK(!r.resolve(providers::Kind::Audio, "fmod", why) && has(why, "fmod")
              && has(why, "miniaudio, none"), "an unknown name names itself and the known ones: %s", why.c_str());
        CHECK(has(r.describe(), "renderer:") && has(r.describeJson(), "\"physics\""),
              "the listing covers every kind, as text and JSON");
    }

    // ── 1b. Manifests and the registry agree (WO-085) ────────────────────
    // Every provider the build declares and built is registered; every one it
    // declared and did not build is registered as unavailable, with the reason;
    // nothing is registered that no manifest declares.
    {
        providers::Registry r;
        registerStockProviders(r);
        size_t declaredBuilt = 0;
        for (const providers::BuiltRow& d : providers::builtRows()) {
            providers::Kind k;
            CHECK(providers::kindFromName(d.kind, k), "manifest kind '%s' is a kind", d.kind);
            if (d.built) {
                ++declaredBuilt;
                CHECK(r.find(k, d.name) != nullptr, "%s '%s' is declared, built and registered", d.kind, d.name);
            } else {
                CHECK(r.find(k, d.name) == nullptr && r.whyUnavailable(k, d.name) != nullptr,
                      "%s '%s' is not built here, and says why: %s", d.kind, d.name, d.whyNot);
            }
        }
        for (const providers::Entry& e : r.entries()) {
            bool declared = false;
            for (const providers::BuiltRow& d : providers::builtRows())
                declared |= d.built && e.name == d.name && std::string(providers::kindName(e.kind)) == d.kind;
            CHECK(declared, "registered %s '%s' has a manifest", providers::kindName(e.kind), e.name.c_str());
        }
        CHECK(declaredBuilt == r.entries().size(), "as many registered as declared-and-built (%zu)", declaredBuilt);

        providers::Registry u;
        u.addUnavailable(providers::Kind::Physics, "havok", "the SDK HAVOK_SDK was not found");
        std::string why;
        CHECK(!u.resolve(providers::Kind::Physics, "havok", why) && has(why, "not built here")
              && has(why, "HAVOK_SDK"), "choosing a provider that is not built gives the reason: %s", why.c_str());
        CHECK(has(u.describe(), "not built: the SDK HAVOK_SDK"), "the listing shows it, with the reason");
    }

    // ── 2. Explicit `none` everywhere: nothing published, audio a real plugin ──
    {
        const Boot b = boot(configFor(makeProject("none",
            R"({"physics":"none","scripting":"none","audio":"none","renderer":"none"})")));
        CHECK(b.initOk && b.providersOk, "all `none` accepted");
        CHECK(!b.physicsPublished, "physics 'none': no PhysicsServiceRef published");
        CHECK(b.plugins.size() == 3 && b.plugins[2] == "Audio",
              "audio 'none' is a registered provider, not an empty slot (%zu plugins)", b.plugins.size());
        CHECK(b.renderer == "none", "renderer 'none' chosen by name");
    }

    // ── 3. Defaults (empty block): jolt publishes; headless renderer is `none` ──
    {
        const Boot b = boot(configFor(makeProject("defaults", R"({})")));
        CHECK(b.initOk && b.providersOk && b.physicsPublished, "default providers: Jolt publishes PhysicsServiceRef");
        CHECK(b.renderer == "none", "no display: the default renderer is `none`");
    }
    {
        // A runtime with no display resolves as a `server` target (WO-086):
        // a plain "forward-bgfx" asks for it there too, and is refused by name.
        const Boot b = boot(configFor(makeProject("bgfx-headless", R"({"renderer":"forward-bgfx"})")));
        CHECK(!b.initOk && has(b.error, "forward-bgfx") && has(b.error, "server"),
              "forward-bgfx for every target, with no display: refused: %s", b.error.c_str());
    }

    // ── 3b. Per-target choices (WO-086) ──────────────────────────────────
    {
        const Boot b = boot(configFor(makeProject("per-target",
            R"({"renderer":{"default":"forward-bgfx","server":"none"},"physics":{"default":"jolt","server":"none"}})")));
        CHECK(b.initOk && b.providersOk && b.renderer == "none" && !b.physicsPublished,
              "the `server` key wins with no display: renderer none, physics none");
    }
    {
        const Boot b = boot(configFor(makeProject("per-target-platform",
            R"({"physics":{"default":"none","macos":"jolt","linux":"jolt","windows":"jolt"}})")));
        CHECK(b.initOk && b.providersOk && b.physicsPublished, "a platform key beats `default`");
    }
    {
        const Boot b = boot(configFor(makeProject("bad-target-key", R"({"renderer":{"widnows":"none"}})")));
        CHECK(!b.initOk && has(b.error, "'widnows' is not a target key"),
              "an unknown target key is refused: %s", b.error.c_str());
    }

    // ── 4. Every kind refuses a name it does not have, by name ────────────
    // The renderer at init(), before anything is created; physics, scripting
    // and audio at addProviders(), which owns the build's list.
    struct Typo { const char* name; const char* json; const char* bad; const char* known; };
    const Typo typos[] = {
        {"typo-physics",   R"({"physics":"pysx"})",       "pysx",   "jolt, none"},
        {"typo-scripting", R"({"scripting":"luajit"})",   "luajit", "lua, none"},
        {"typo-audio",     R"({"audio":"fmod"})",         "fmod",   "miniaudio, none"},
        {"typo-renderer",  R"({"renderer":"aaa"})",       "aaa",    "none"},
    };
    for (const Typo& t : typos) {
        const Boot b = boot(configFor(makeProject(t.name, t.json)));
        CHECK(!(b.initOk && b.providersOk) && has(b.error, t.bad) && has(b.error, t.known),
              "%s refused at startup: %s", t.name, b.error.c_str());
    }

    // ── 5. An unknown kind, and a value that is not a name ───────────────
    {
        const Boot b = boot(configFor(makeProject("typo-kind", R"({"phyiscs":"jolt"})")));
        CHECK(!b.initOk && has(b.error, "phyiscs") && has(b.error, "physics, scripting, audio, renderer"),
              "an unknown kind is refused: %s", b.error.c_str());
    }
    {
        const Boot b = boot(configFor(makeProject("not-a-name", R"({"physics":3})")));
        CHECK(!b.initOk && has(b.error, "the value must be a provider name"),
              "a value that is not a name is refused: %s", b.error.c_str());
    }

    // ── 6. Overrides (EngEngineDesc.providers) beat the project, and are checked ──
    {
        EngineConfig cfg = configFor(makeProject("override", R"({"physics":"jolt"})"));
        cfg.providerOverrides = {{"physics", "none"}};
        const Boot b = boot(cfg);
        CHECK(b.initOk && b.providersOk && !b.physicsPublished, "an override of physics to `none` wins");
        cfg.providerOverrides = {{"physics", "havok"}};
        const Boot bad = boot(cfg);
        CHECK(!(bad.initOk && bad.providersOk) && has(bad.error, "havok"), "a bad override is refused: %s", bad.error.c_str());
    }

    // ── 7. Direct injection: the host's own physics object, no name ───────
    {
        int attached = 0;
        const Boot b = boot(configFor(makeProject("inject", R"({"physics":"my-physics"})")),
                            std::make_shared<FakePhysics>(&attached));
        CHECK(b.initOk && b.providersOk, "an injected provider fills its kind; the project's name is not looked up");
        CHECK(!b.plugins.empty() && b.plugins[0] == "FakePhysics" && attached == 1,
              "the injected object is the physics provider, attached once");
        CHECK(!b.physicsPublished, "Jolt is not created alongside it");
    }

    fs::remove_all(fs::temp_directory_path() / "engine_providers_test");

    if (g_failures) { std::printf("providers_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("providers_test: ALL PASS\n");
    return 0;
}
