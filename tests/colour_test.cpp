// ── colour_test — the sRGB transfer function, and its GPU twin ───────────────
//
// Colour pipeline stage A. Every other part of the stage — cooked textures
// tagged sRGB, hardware decode on sample, the output pass's encode — rests on
// two functions being right and on the shader agreeing with them. Neither is
// visible to any existing test: the GPU lane renders through bgfx Noop, and a
// wrong curve produces an image that is merely a little off.
//
// So this pins the CPU reference against the standard's own known values, and
// then reads shaders/colour.sh and checks the GPU copy uses the same constants.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/colour.h"
#include "render/clear_colour.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

// Every numeric literal in one shader function's body, in order.
static std::vector<double> literalsIn(const std::string& src, const std::string& fn) {
    std::vector<double> out;
    size_t at = src.find(fn);
    if (at == std::string::npos) return out;
    size_t open = src.find('{', at), close = src.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return out;
    const std::string body = src.substr(open, close - open);
    for (size_t i = 0; i < body.size();) {
        const bool startsNum = std::isdigit((unsigned char)body[i]) ||
            (body[i] == '.' && i + 1 < body.size() && std::isdigit((unsigned char)body[i + 1]));
        // A digit inside an identifier (vec3, u_foo2) is not a literal.
        const bool inIdent = i > 0 && (std::isalnum((unsigned char)body[i - 1]) || body[i - 1] == '_');
        if (startsNum && !inIdent) {
            size_t j = i;
            while (j < body.size() && (std::isdigit((unsigned char)body[j]) || body[j] == '.')) ++j;
            out.push_back(std::stod(body.substr(i, j - i)));
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

// Float precision, not double: the CPU constants are floats (12.92f is
// 12.920000076... as a double), so an exact double compare would call two
// identical literals different.
static bool containsNear(const std::vector<double>& v, double x) {
    return std::any_of(v.begin(), v.end(), [&](double y) { return std::fabs(x - y) < 1e-6; });
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("colour_test — the sRGB transfer function\n");

    using colour::srgbToLinear;
    using colour::linearToSrgb;

    // ── 1. Known values from the standard ──────────────────────────────────
    {
        std::printf("\n-- 1. known values --\n");
        CHECK(srgbToLinear(0.0f) == 0.0f && srgbToLinear(1.0f) > 0.99999f
              && srgbToLinear(1.0f) < 1.00001f, "black and white are fixed points");
        // sRGB mid-grey 0.5 is ~21.4% linear light — the figure behind "a
        // gamma-incorrect shader uses 2.3x too much reflectance at mid-grey".
        CHECK(std::fabs(srgbToLinear(0.5f) - 0.214041f) < 1e-5f,
              "sRGB 0.5 decodes to 0.214041 linear (%.6f)", (double)srgbToLinear(0.5f));
        CHECK(std::fabs(linearToSrgb(0.214041f) - 0.5f) < 1e-5f,
              "and 0.214041 linear encodes back to 0.5 (%.6f)",
              (double)linearToSrgb(0.214041f));
        // The toe: below the cutoff the curve is a straight line, not a power.
        CHECK(std::fabs(srgbToLinear(0.02f) - 0.02f / 12.92f) < 1e-7f,
              "below the toe cutoff decode is linear (/12.92)");
        CHECK(std::fabs(linearToSrgb(0.001f) - 0.001f * 12.92f) < 1e-7f,
              "and encode is linear (*12.92)");
    }

    // ── 2. The two segments meet ───────────────────────────────────────────
    // A piecewise curve with a step at the join produces a visible band in a
    // smooth gradient. The standard's constants make the two halves agree.
    {
        std::printf("\n-- 2. continuity at the cutoffs --\n");
        const float d = colour::kDecodeCutoff;
        const float below = d / colour::kToeSlope;
        const float above = std::pow((d + colour::kOffset) / colour::kScale, colour::kExponent);
        CHECK(std::fabs(below - above) < 1e-6f,
              "decode segments agree at 0.04045 (%.8f vs %.8f)", (double)below, (double)above);
        const float e = colour::kEncodeCutoff;
        const float lo = e * colour::kToeSlope;
        const float hi = colour::kScale * std::pow(e, 1.0f / colour::kExponent) - colour::kOffset;
        CHECK(std::fabs(lo - hi) < 1e-5f,
              "encode segments agree at 0.0031308 (%.8f vs %.8f)", (double)lo, (double)hi);
    }

    // ── 3. Every 8-bit value round-trips, and the curve never goes backwards ─
    {
        std::printf("\n-- 3. round trip and monotonicity --\n");
        float worst = 0.0f;
        for (int i = 0; i <= 255; ++i) {
            const float c = (float)i / 255.0f;
            worst = std::max(worst, std::fabs(linearToSrgb(srgbToLinear(c)) - c));
        }
        CHECK(worst < 2e-5f, "all 256 8-bit values survive decode->encode (worst %.2e)",
              (double)worst);
        bool mono = true;
        float prevD = -1.0f, prevE = -1.0f;
        for (int i = 0; i <= 10000; ++i) {
            const float x = (float)i / 10000.0f;
            const float dv = srgbToLinear(x), ev = linearToSrgb(x);
            if (dv < prevD || ev < prevE) { mono = false; break; }
            prevD = dv; prevE = ev;
        }
        CHECK(mono, "both directions are monotonic across [0,1] in 10k steps");
    }

    // ── 4. THE GPU COPY USES THE SAME CURVE ────────────────────────────────
    // shaders/colour.sh re-states these constants in GLSL. Checked by reading
    // the file: every constant the CPU uses must appear in the matching
    // function, and the function may contain no OTHER literal besides the
    // structural 0.0 and 1.0 — so a typo (2.2 for 2.4, 12.29 for 12.92) fails
    // here instead of rendering a slightly wrong frame.
    {
        std::printf("\n-- 4. shaders/colour.sh mirrors core/colour.h --\n");
        std::ifstream f(ENGINE_SOURCE_DIR "/shaders/colour.sh");
        std::stringstream ss; ss << f.rdbuf();
        const std::string src = ss.str();
        CHECK(!src.empty(), "shaders/colour.sh is readable (%zu bytes)", src.size());

        const std::set<double> structural = { 0.0, 1.0 };
        auto mirrors = [&](const char* fn, std::vector<double> want) {
            const std::vector<double> got = literalsIn(src, fn);
            bool allPresent = !got.empty();
            for (double w : want) allPresent = allPresent && containsNear(got, w);
            bool noStrays = true;
            for (double g : got)
                if (!containsNear(want, g) && !structural.count(g)) noStrays = false;
            return allPresent && noStrays;
        };
        CHECK(mirrors("vec3 linearToSrgb",
                      { colour::kToeSlope, colour::kScale, colour::kOffset,
                        colour::kExponent, colour::kEncodeCutoff }),
              "linearToSrgb uses exactly the CPU's encode constants");
        CHECK(mirrors("vec3 srgbToLinear",
                      { colour::kToeSlope, colour::kScale, colour::kOffset,
                        colour::kExponent, colour::kDecodeCutoff }),
              "srgbToLinear uses exactly the CPU's decode constants");
    }

    // ── 5. Clear colours: decoded, and never another view's ────────────────
    // bgfx's float palette is ONE per frame. The first version keyed the slot
    // as viewId % 16, so view 17 shared view 1's slot and whichever set its
    // colour last cleared both — latent only because nothing allocated a
    // sixteenth view yet. render/clear_colour.h makes that decision; this pins it.
    {
        std::printf("\n-- 5. view clear colours --\n");
        const ViewClearColour scene = viewClearColour(1, 0.102f, 0.102f, 0.102f, 1.0f);
        const ViewClearColour game  = viewClearColour(4, 0.2f, 0.4f, 0.6f, 1.0f);
        CHECK(scene.usePalette && game.usePalette && scene.slot == 1 && game.slot == 4,
              "the engine's clearing views (1, 4) use the float palette, each its "
              "own slot (%u, %u)", scene.slot, game.slot);
        CHECK(std::fabs(scene.linear[0] - srgbToLinear(0.102f)) < 1e-7f,
              "and the colour is DECODED to linear (%.5f)", (double)scene.linear[0]);

        const ViewClearColour high = viewClearColour(17, 0.9f, 0.1f, 0.1f, 1.0f);
        CHECK(!high.usePalette,
              "view 17 does NOT use the palette — with %% 16 it would have shared "
              "view 1's slot and taken its colour");
        const ViewClearColour last = viewClearColour(15, 0.5f, 0.5f, 0.5f, 1.0f);
        CHECK(last.usePalette && last.slot == 15, "view 15 still owns slot 15");

        // The packed fallback rounds (0.0103 * 255 = 2.63 -> 3, not 2) and
        // leaves alpha as coverage.
        CHECK(scene.packedRgba == 0x030303ffu,
              "the packed fallback rounds the decoded dark grey (%08x)", scene.packedRgba);
        const ViewClearColour half = viewClearColour(20, 1.0f, 1.0f, 1.0f, 0.5f);
        CHECK(half.linear[3] == 0.5f && (half.packedRgba & 0xffu) == 128u,
              "alpha is never encoded (%.2f, %u)", (double)half.linear[3],
              half.packedRgba & 0xffu);
    }

    if (g_failures) {
        std::printf("\ncolour_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncolour_test: ALL PASS\n");
    return 0;
}
