// ── device_select_test — the backend order and the device check (WO-089) ─────
// core/device_select.h decides, with no GPU: which backends a project may list
// for a platform, which order is tried, what happens past its end, and whether
// a device meets a renderer's manifest. A FAKE device stands in for the GPU:
// `attempt` says which backends create a device, `have` lists its capabilities.
// The resolver's part (engine_build refuses a backend a platform lacks) and the
// in-tree renderer manifest are checked here too.
#include <cstdio>
#include <fstream>
#include <set>
#include <string>

#include "core/device_select.h"
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
static std::string first(const std::vector<std::string>& v) { return v.empty() ? "" : v.front(); }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("device_select_test\n");

    const std::vector<std::string> platforms = {"windows", "linux", "macos", "android", "ios", "web"};
    const std::map<std::string, std::vector<std::string>> known = {
        {"windows", {"d3d12", "d3d11", "vulkan"}}, {"linux", {"vulkan"}}, {"macos", {"metal"}}};

    // ── The project's "rhi" block ─────────────────────────────────────────
    {
        CHECK(rhisel::checkRhi(nullptr, platforms, known).empty(), "no block: nothing to refuse");
        CHECK(rhisel::checkRhi(json{{"windows", {"vulkan", "d3d12"}}, {"linux", {"vulkan"}}}, platforms, known).empty(),
              "an order of backends each platform has is accepted");
        auto bad = rhisel::checkRhi(json{{"macos", {"vulkan"}}}, platforms, known);
        CHECK(has(first(bad), "'vulkan' is not built for macos") && has(first(bad), "macos has metal"),
              "a backend the platform lacks is refused by name, with what it has: %s", first(bad).c_str());
        bad = rhisel::checkRhi(json{{"widnows", {"d3d12"}}}, platforms, known);
        CHECK(has(first(bad), "'widnows' is not a platform"), "a misspelled platform is refused: %s", first(bad).c_str());
        bad = rhisel::checkRhi(json{{"windows", {"directx"}}}, platforms, known);
        CHECK(has(first(bad), "'directx' is not a backend"), "an unknown backend is refused: %s", first(bad).c_str());
        bad = rhisel::checkRhi(json{{"windows", json::array()}}, platforms, known);
        CHECK(has(first(bad), "non-empty"), "an empty list is refused (it would try nothing)");
        bad = rhisel::checkRhi(json{{"windows", {"d3d12", "d3d12"}}}, platforms, known);
        CHECK(has(first(bad), "listed twice"), "a backend listed twice is refused");
        bad = rhisel::checkRhi(json{{"windows", "d3d12"}}, platforms, known);
        CHECK(has(first(bad), "list"), "a name where a list belongs is refused");
        bad = rhisel::checkRhi(json::array({"d3d12"}), platforms, known);
        CHECK(has(first(bad), "must be an object"), "a block that is not an object is refused");
        CHECK(rhisel::checkRhi(json{{"android", {"vulkan"}}}, platforms, known).empty(),
              "a platform whose table is not read here is checked for shape only");
    }

    // ── The order tried ───────────────────────────────────────────────────
    {
        std::string by;
        auto o = rhisel::orderFor(json{{"windows", {"vulkan", "d3d12"}}}, "windows", known.at("windows"), &by);
        CHECK(o == std::vector<std::string>({"vulkan", "d3d12"}) && has(by, "\"rhi\".windows"),
              "the project's order wins for its platform (%s)", by.c_str());
        o = rhisel::orderFor(json{{"windows", {"vulkan"}}}, "linux", known.at("linux"), &by);
        CHECK(o == std::vector<std::string>({"vulkan"}) && has(by, "linux default"),
              "a platform the block leaves out takes its own order (%s)", by.c_str());
        o = rhisel::orderFor(nullptr, "windows", known.at("windows"), &by);
        CHECK(o == std::vector<std::string>({"d3d12"}),
              "no block: the platform's first backend alone, as before WO-089 (its table is not a fallback chain)");
    }

    // ── A fake device: which backends create one ──────────────────────────
    {
        std::vector<std::string> asked;
        auto fake = [&](std::set<std::string> works) {
            return [&asked, works](const std::string& b) -> std::string {
                asked.push_back(b);
                return works.count(b) ? "" : "no device";
            };
        };
        auto r = rhisel::tryInOrder({"d3d12", "vulkan"}, "test", fake({"d3d12", "vulkan"}));
        CHECK(r.ok() && r.used == "d3d12" && asked.size() == 1, "the first that creates a device is used, and nothing after it");
        asked.clear();
        r = rhisel::tryInOrder({"d3d12", "vulkan"}, "test", fake({"vulkan"}));
        CHECK(r.ok() && r.used == "vulkan" && r.failed.size() == 1 && r.failed[0].backend == "d3d12"
              && r.failed[0].whyNot == "no device", "an earlier one that fails is recorded, with why");
        asked.clear();
        r = rhisel::tryInOrder({"d3d12", "vulkan"}, "project.json \"rhi\".windows", fake({"d3d11"}));
        CHECK(!r.ok() && asked == std::vector<std::string>({"d3d12", "vulkan"}),
              "past the end of the list nothing else is tried (d3d11 works, and is not asked)");
        CHECK(has(r.refusal, "\"rhi\".windows") && has(r.refusal, "d3d12: no device") && has(r.refusal, "vulkan: no device"),
              "the refusal names the order and each backend tried: %s", r.refusal.c_str());
    }

    // ── A fake device that lacks one capability: both outcomes ────────────
    {
        const std::set<std::string> device = {"index32", "depth_d32f", "render_rgba16f", "texture3d_rgba16f"};  // no instancing
        rhisel::DeviceDecl fallsBack{{"index32", "depth_d32f"}, {"instancing"}};
        auto c = rhisel::checkDevice(fallsBack, device);
        CHECK(c.ok() && c.fellBack == std::vector<std::string>({"instancing"}),
              "a missing capability the manifest gives a fallback: the fallback, reported");
        rhisel::DeviceDecl needs{{"index32", "instancing"}, {}};
        c = rhisel::checkDevice(needs, device);
        const std::string why = c.refusal("forward-bgfx", "vulkan");
        CHECK(!c.ok() && has(why, "'instancing'") && has(why, "forward-bgfx") && has(why, "vulkan device"),
              "a missing capability the manifest needs: refused by name: %s", why.c_str());
        c = rhisel::checkDevice({{}, {}}, {});
        CHECK(c.ok() && c.fellBack.empty(), "a capability the manifest does not name is not checked");
    }

    // ── A manifest's "device" block ───────────────────────────────────────
    {
        CHECK(rhisel::checkDeviceDecl({{"index32"}, {"instancing"}}).empty(), "known names, needs and fallbacks apart: accepted");
        CHECK(has(rhisel::checkDeviceDecl({{"bindless"}, {}}), "'bindless' is not a device capability"),
              "an unknown capability is refused (a typo must not pass as never-missing)");
        CHECK(has(rhisel::checkDeviceDecl({{"instancing"}, {"instancing"}}), "both"),
              "a capability both needed and given a fallback is refused");
        provsel::Decl d; std::string why;
        CHECK(!provsel::declFromManifest(json{{"kind", "renderer"}, {"name", "x"}, {"contract", "c"},
                                              {"platforms", {"linux"}}, {"device", {{"need", {"index32"}}}}}, d, why)
              && has(why, "'need' is not a key"), "the manifest reader refuses a misspelled device key: %s", why.c_str());

        why.clear();
        std::ifstream f(ENGINE_SOURCE_DIR "/src/runtime/renderer-forward-bgfx.provider.json");
        const bool read = f && provsel::declFromManifest(json::parse(f), d, why);
        CHECK(read && !d.device.needs.empty(), "the forward-bgfx manifest names its device needs, and reads (%s)", why.c_str());
    }

    // ── Through the resolver, as engine_build runs it ─────────────────────
    {
        provsel::Inputs in;
        in.target = {"macos", "client"};
        for (const char* k : {"physics", "scripting", "audio", "renderer"}) {
            provsel::Decl d; d.kind = k; d.name = "none"; d.contract = "c"; d.platforms = platforms;
            d.flavours = {"client", "server"};
            in.decls.push_back(d);
        }
        in.providers = {{"physics", "none"}, {"scripting", "none"}, {"audio", "none"}, {"renderer", "none"}};
        for (const auto& p : platforms) in.platforms[p].name = p;
        for (const auto& [p, apis] : known) in.platforms[p].gpuApis = apis;

        in.rhi = {{"windows", {"vulkan", "d3d12"}}};
        auto r = provsel::resolve(in);
        CHECK(r.ok() && r.rhiOrder == std::vector<std::string>({"metal"}) && has(r.rhiChosenBy, "macos default"),
              "packaging for macos: the block's windows order is checked, macos takes metal");
        in.rhi = {{"windows", {"metal"}}};
        r = provsel::resolve(in);
        CHECK(!r.ok() && has(r.refusals.front(), "'metal' is not built for windows"),
              "a backend not built for a platform is refused at engine_build, whichever target: %s",
              r.refusals.empty() ? "" : r.refusals.front().c_str());
        in.target = {"windows", "client"};
        in.rhi = {{"windows", {"vulkan"}}};
        r = provsel::resolve(in);
        CHECK(r.ok() && r.rhiOrder == std::vector<std::string>({"vulkan"}) && has(provsel::explainText(r), "rhi       vulkan"),
              "the target's order is in the result and the explanation");
    }

    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
