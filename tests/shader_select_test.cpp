// ── shader_select_test — which cooked variant does this machine want? ───────
//
// Loading a cooked shader is mostly bgfx calls, which need a GPU. The part with
// actual decisions in it — map the live renderer to a cooked profile, find the
// exact variant, explain a miss — is split into shader_select.h precisely so it
// can be tested here.
//
// Both failures this guards are silent otherwise:
//   • a package cooked for the wrong backend produces no program, and a
//     renderer with no program draws nothing at all;
//   • a feature mask with no cooked variant must FAIL, because the nearest
//     variant is confidently wrong — a skinned mesh drawn with the unskinned
//     program collapses into a heap at the origin, which looks like an
//     animation bug and costs a day.
#include <cstdio>
#include <string>

#include "render/shader/shader_select.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using namespace rshader;

static assetlib::ShaderAsset makeAsset() {
    assetlib::ShaderAsset sh;
    sh.name = "standard";
    sh.features = { "SKINNED" };
    // What a macOS cook actually produces: metal + spirv + glsl, both masks.
    for (uint32_t p : { assetlib::kProfileMetal, assetlib::kProfileSpirv,
                        assetlib::kProfileGlsl })
        for (uint32_t m : { 0u, 1u })
            sh.variants.push_back({ m, p, 0, 8, 8, 8 });
    sh.blob.resize(16);
    return sh;
}

int main() {
    std::printf("shader_select_test\n");
    const auto sh = makeAsset();

    // ── renderer -> profile ─────────────────────────────────────────────────
    {
        uint32_t p = 99;
        CHECK(profileForRenderer(RendererKind::Metal, p)
              && p == assetlib::kProfileMetal, "Metal -> metal");
        CHECK(profileForRenderer(RendererKind::Vulkan, p)
              && p == assetlib::kProfileSpirv, "Vulkan -> spirv");
        CHECK(profileForRenderer(RendererKind::Direct3D11, p)
              && p == assetlib::kProfileDx11, "D3D11 -> dx11");
        CHECK(profileForRenderer(RendererKind::Direct3D12, p)
              && p == assetlib::kProfileDx12, "D3D12 -> dx12");
        CHECK(profileForRenderer(RendererKind::OpenGL, p)
              && p == assetlib::kProfileGlsl, "OpenGL -> glsl");
        // Noop/headless is a legitimate state, not a cook error — it must be
        // distinguishable so a headless run doesn't log "your package is broken".
        CHECK(!profileForRenderer(RendererKind::Other, p),
              "an unsupported renderer reports no profile rather than guessing");
    }

    // ── selection ───────────────────────────────────────────────────────────
    {
        const auto c = selectVariant(sh, 0, RendererKind::Metal);
        CHECK(c.ok() && c.variant->profile == assetlib::kProfileMetal
              && c.variant->featureMask == 0, "exact match on metal, mask 0");

        const auto s = selectVariant(sh, 1, RendererKind::Vulkan);
        CHECK(s.ok() && s.variant->profile == assetlib::kProfileSpirv
              && s.variant->featureMask == 1, "exact match on spirv, mask 1");
    }

    // ── the two misses, told apart ──────────────────────────────────────────
    {
        // The macOS-cook-on-a-Windows-machine case. The fix is "re-cook for
        // this target", so the message has to say which profiles ARE present.
        const auto wrongBackend = selectVariant(sh, 0, RendererKind::Direct3D11);
        CHECK(!wrongBackend.ok(), "a profile that was never cooked FAILS");
        CHECK(wrongBackend.error.find("dx11") != std::string::npos
              && wrongBackend.error.find("metal") != std::string::npos,
              "...naming both what was wanted and what exists: %s",
              wrongBackend.error.c_str());

        // Completely different fix: the material asked for a feature the
        // shader doesn't have. Silently substituting mask 0 here is exactly
        // the bug this test exists to prevent.
        const auto noMask = selectVariant(sh, 0x8, RendererKind::Metal);
        CHECK(!noMask.ok(), "an uncooked feature mask FAILS rather than "
                            "falling back to a different variant");
        CHECK(noMask.error.find("0x8") != std::string::npos,
              "...naming the mask: %s", noMask.error.c_str());

        const auto headless = selectVariant(sh, 0, RendererKind::Other);
        CHECK(!headless.ok()
              && headless.error.find("headless") != std::string::npos,
              "a headless renderer is reported as such, not as a bad package");

        assetlib::ShaderAsset empty;
        empty.name = "empty";
        const auto none = selectVariant(empty, 0, RendererKind::Metal);
        CHECK(!none.ok() && none.error.find("nothing") != std::string::npos,
              "a shader with no variants at all fails clearly");
    }

    // ── program keys ────────────────────────────────────────────────────────
    {
        const auto a = programKey("a.cshader", 0, assetlib::kProfileMetal);
        // Every axis must be in the key. Two variants sharing a key means the
        // second material silently draws with the first's program.
        CHECK(a != programKey("b.cshader", 0, assetlib::kProfileMetal),
              "path is part of the key");
        CHECK(a != programKey("a.cshader", 1, assetlib::kProfileMetal),
              "feature mask is part of the key");
        CHECK(a != programKey("a.cshader", 0, assetlib::kProfileSpirv),
              "profile is part of the key");
        CHECK(a == programKey("a.cshader", 0, assetlib::kProfileMetal),
              "the same request yields the same key (so the cache dedups)");
    }

    if (g_failures) {
        std::printf("shader_select_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("shader_select_test: PASS\n");
    return 0;
}
