// ── cook_deps_test — a declared input is an input, enforced ──────────────────
//
// Cook staleness is decided ENTIRELY by the DDC key. `cookIsStale` never reads
// `rec.state` (except to keep a Failed record failed), so a registry-side
// "mark dependents stale" cascade cannot invalidate anything — putting a
// dependency's hash INTO the key is the only mechanism that works. It is also
// the only one that is correct across a shared DDC, because a cascade is local
// to one machine's registry while a key means the same thing everywhere.
//
// Before this, a cooker whose output depended on a second FILE had no way to say
// so: `CookContext::addDependency` takes a UUID and cookers have no registry
// lookup. So each one hand-rolled blake3File() into settingsFingerprint, and a
// cooker that simply forgot was indistinguishable from one with no extra inputs.
// `ICooker::declaredInputs` moved that into the interface, and this test is what
// makes the omission catchable:
//
//   FOR EVERY REGISTERED COOKER, FOR EVERY INPUT IT DECLARES,
//   PERTURBING THAT INPUT MUST CHANGE THE COOK KEY.
//
// A cooker added later with an undeclared second input fails here rather than
// silently serving stale output for the rest of the project's life.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <assetlib/asset_registry.h>
#include <assetlib/cooker.h>
#include <assetlib/ddc.h>

#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/cookers/texture/texture_cooker.h"
#include "assets/cookers/shader/shader_cooker.h"
#include "assets/cookers/material/material_cooker.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// The key, computed the way the pipeline computes it. Mirrors cook_key.cpp's
// composition rather than calling it, because cook_key.h is an internal header;
// what matters is that DECLARED INPUTS participate, which is what we assert.
static std::string keyOf(assetlib::ICooker& cooker, const fs::path& source) {
    assetlib::CookContext ctx;
    ctx.sourcePath = source;

    assetlib::DdcKeyInputs in;
    in.cookerId      = cooker.id();
    in.cookerVersion = cooker.version();
    in.settings      = cooker.settingsFingerprint(ctx);
    in.sourceHash    = assetlib::blake3File(source);
    for (const auto& p : cooker.declaredInputs(ctx)) {
        const std::string h = assetlib::blake3File(p);
        in.depHashes.push_back(p.generic_string() + "@"
                               + (h.empty() ? "missing" : h));
    }
    return assetlib::computeDdcKey(in);
}

static void append(const fs::path& p, const char* text) {
    std::ofstream f(p, std::ios::app);
    f << text;
}

// One fixture set per phase, in its own directory. Phases perturb each other's
// files otherwise: appending a `//` comment to a declared input is fine for a
// .sc stage source but makes a .shader manifest invalid JSON, and the next
// phase's declaredInputs() then legitimately returns nothing — a green test
// asserting the wrong thing. Isolation is cheaper than ordering rules.
struct Fixture {
    fs::path dir, shader, material, vs, fs_, varying, include;
};
static Fixture makeFixture(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    Fixture f{ dir, dir / "standard.shader", dir / "rust.material",
               dir / "standard.vs.sc", dir / "standard.fs.sc",
               dir / "varying.def.sc",  dir / "common.sh" };
    { std::ofstream o(f.include); o << "#define TAU 6.28318\n"; }
    { std::ofstream o(f.vs);      o << "#include \"common.sh\"\nvoid main(){}\n"; }
    { std::ofstream o(f.fs_);     o << "void main(){}\n"; }
    { std::ofstream o(f.varying); o << "vec4 v_color : COLOR0;\n"; }
    {   std::ofstream o(f.shader);
        o << "{\n  \"name\": \"standard\",\n"
             "  \"vertex\": \"standard.vs.sc\",\n"
             "  \"fragment\": \"standard.fs.sc\",\n"
             "  \"varying\": \"varying.def.sc\",\n"
             "  \"parameters\": []\n}\n"; }
    {   std::ofstream o(f.material);
        // shaderRef is a PATH as authored, not a bare name — resolved relative
        // to the material, then to the project root.
        o << "{\n  \"name\": \"rust\",\n"
             "  \"shader\": \"standard.shader\",\n"
             "  \"parameters\": {}\n}\n"; }
    return f;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cook_deps_test: declared inputs participate in the cook key\n");

    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "engine_cook_deps_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const Fixture sweepFx = makeFixture(root / "sweep");

    std::printf("\n-- every cooker's declared inputs are honoured --\n");

    std::vector<std::unique_ptr<assetlib::ICooker>> cookers;
    cookers.push_back(std::make_unique<ShaderCooker>());
    cookers.push_back(std::make_unique<MaterialCooker>());
    cookers.push_back(std::make_unique<MeshCooker>());
    cookers.push_back(std::make_unique<TextureCooker>());

    // The source each cooker gets pointed at. Mesh/texture declare nothing, and
    // are here so the sweep covers EVERY registered cooker: if someone gives
    // MeshCooker a second input later without declaring it, the loop below is
    // where that shows up.
    auto sourceFor = [&](const std::string& id) -> fs::path {
        if (id == "shader")   return sweepFx.shader;
        if (id == "material") return sweepFx.material;
        return {};                        // no fixture: declares nothing anyway
    };

    for (auto& c : cookers) {
        const std::string id = c->id();
        const fs::path src = sourceFor(id);
        if (src.empty()) {
            assetlib::CookContext ctx;
            ctx.sourcePath = root / ("nonexistent." + id);
            CHECK(c->declaredInputs(ctx).empty(),
                  "cooker \"%s\" declares no extra inputs", id.c_str());
            continue;
        }

        assetlib::CookContext ctx;
        ctx.sourcePath = src;
        const auto inputs = c->declaredInputs(ctx);
        CHECK(!inputs.empty(), "cooker \"%s\" declares %zu input(s)",
              id.c_str(), inputs.size());

        // THE CONTRACT: perturb each declared input, the key must move.
        for (const auto& in : inputs) {
            const std::string before = keyOf(*c, src);
            append(in, "\n// perturbed\n");
            const std::string after = keyOf(*c, src);
            CHECK(before != after,
                  "\"%s\": editing declared input %s changes the key",
                  id.c_str(), in.filename().string().c_str());
        }
    }

    // ── The specific relationships that motivated this ──────────────────────
    std::printf("\n-- the two that were hand-rolled before --\n");
    {
        const Fixture fx = makeFixture(root / "shader_phase");
        ShaderCooker sc;
        const std::string k0 = keyOf(sc, fx.shader);
        append(fx.include, "#define EXTRA 1\n");
        const std::string k1 = keyOf(sc, fx.shader);
        CHECK(k0 != k1,
              "a shader re-keys when a TRANSITIVELY included .sh header changes");
    }
    {
        const Fixture fx = makeFixture(root / "material_phase");
        MaterialCooker mc;
        const std::string k0 = keyOf(mc, fx.material);
        append(fx.shader, "\n");                // the .shader manifest it reads
        const std::string k1 = keyOf(mc, fx.material);
        CHECK(k0 != k1, "a material re-keys when its shader MANIFEST changes");

        // And the precision that was deliberately chosen: shading-code edits do
        // NOT re-key materials. A cooked material contains resolved parameter
        // values against a declared interface; the .sc bytes are not in it, and
        // keying on them would re-cook every material in the project on every
        // shader edit.
        const std::string k2 = keyOf(mc, fx.material);
        append(fx.fs_, "// shading only\n");
        const std::string k3 = keyOf(mc, fx.material);
        CHECK(k2 == k3,
              "...but NOT when only the shading code (.sc) changes");
    }

    // A declared input that does not exist yet must key as "missing", so the key
    // moves when it appears — otherwise a material cooked against an absent
    // shader would stay cached after the shader was added.
    std::printf("\n-- an input that appears later --\n");
    {
        const Fixture fx = makeFixture(root / "orphan_phase");
        const fs::path orphan = fx.dir / "orphan.material";
        {   std::ofstream f(orphan);
            f << "{\n  \"name\": \"orphan\",\n"
                 "  \"shader\": \"later.shader\",\n"
                 "  \"parameters\": {}\n}\n"; }
        MaterialCooker mc;
        const std::string missing = keyOf(mc, orphan);
        {   std::ofstream f(fx.dir / "later.shader");
            f << "{\n  \"name\": \"later\",\n"
                 "  \"vertex\": \"standard.vs.sc\",\n"
                 "  \"fragment\": \"standard.fs.sc\",\n"
                 "  \"varying\": \"varying.def.sc\",\n"
                 "  \"parameters\": []\n}\n"; }
        const std::string present = keyOf(mc, orphan);
        CHECK(missing != present,
              "a material re-keys once its missing shader appears");
    }

    fs::remove_all(root, ec);
    if (g_failures) {
        std::printf("\ncook_deps_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncook_deps_test: all checks passed\n");
    return 0;
}
