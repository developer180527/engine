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
#include "core/cube_lut.h"
#include "core/display_transform.h"
#include "core/output_transform.h"
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

    // ── 6. Stage B: exposure and PBR Neutral ───────────────────────────────
    {
        std::printf("\n-- 6. exposure and tone mapping --\n");
        using namespace display;
        CHECK(std::fabs(ev100(16.0f, 1.0f / 100.0f, 100.0f) - 14.6439f) < 1e-3f,
              "sunny-16 settings are EV100 14.64 (%.4f)",
              (double)ev100(16.0f, 1.0f / 100.0f, 100.0f));
        CHECK(std::fabs(ev100(1.0f, 1.0f, 100.0f)) < 1e-6f &&
              std::fabs(exposureFromEv100(0.0f) - 1.0f / 1.2f) < 1e-6f,
              "EV100 0 scales radiance by 1/1.2 (Filament's Exposure.cpp)");
        CHECK(exposureGain(0.0f) == 1.0f && exposureGain(1.0f) == 2.0f &&
              exposureGain(-1.0f) == 0.5f,
              "manual exposure: 0 stops changes nothing, +1 doubles, -1 halves");

        // Below the knee the colour is only offset — hue and ratios survive.
        Rgb lo = pbrNeutral({ 0.5f, 0.4f, 0.3f });
        CHECK(std::fabs(lo.r - 0.46f) < 1e-6f && std::fabs(lo.g - 0.36f) < 1e-6f &&
              std::fabs(lo.b - 0.26f) < 1e-6f,
              "below the knee PBR Neutral subtracts 0.04 and nothing else "
              "(%.4f %.4f %.4f)", (double)lo.r, (double)lo.g, (double)lo.b);
        Rgb toe = pbrNeutral({ 0.05f, 0.05f, 0.05f });
        CHECK(std::fabs(toe.r - 0.015625f) < 1e-6f,
              "in the toe the offset is x - 6.25x² (0.05 -> %.6f)", (double)toe.r);

        bool bounded = true, mono = true;
        float prev = -1.0f;
        for (int i = 0; i <= 20000; ++i) {
            const float v = (float)i * 0.05f;        // 0 .. 1000
            const float o = pbrNeutral({ v, v, v }).r;
            if (o > 1.0f) bounded = false;
            if (o < prev - 1e-6f) mono = false;
            prev = o;
        }
        CHECK(bounded && mono,
              "grey 0..1000 maps monotonically into [0, 1] — highlights roll off "
              "instead of clipping");
        const float below = pbrNeutral({ 0.7999f, 0.7999f, 0.7999f }).r;
        const float above = pbrNeutral({ 0.8001f, 0.8001f, 0.8001f }).r;
        CHECK(std::fabs(above - below) < 1e-3f,
              "and it is continuous at the compression knee (%.5f vs %.5f)",
              (double)below, (double)above);

        // The inverse is DERIVED in display_transform.h, not copied, so it is
        // held to the forward function here over a spread of colours.
        float worstInv = 0.0f;
        for (int i = 0; i < 4000; ++i) {
            const float t = (float)i / 4000.0f;
            const Rgb in = { 8.0f * t * t, 3.0f * t, 0.5f + 2.0f * t * (1.0f - t) };
            const Rgb back = pbrNeutralInverse(pbrNeutral(in));
            const float e = std::max({ std::fabs(back.r - in.r), std::fabs(back.g - in.g),
                                       std::fabs(back.b - in.b) }) /
                            std::max(1.0f, std::max({ in.r, in.g, in.b }));
            worstInv = std::max(worstInv, e);
        }
        CHECK(worstInv < 2e-3f,
              "inverse(forward(x)) returns x across [0, 8] (worst relative %.2e)",
              (double)worstInv);

        std::ifstream f(ENGINE_SOURCE_DIR "/shaders/colour.sh");
        std::stringstream ss; ss << f.rdbuf();
        const std::vector<double> got = literalsIn(ss.str(), "vec3 pbrNeutral");
        const std::vector<double> want = { kPbrToeEnd, kPbrToeScale, kPbrOffset,
                                           kPbrStartCompression, kPbrDesaturation };
        bool all = !got.empty(), stray = false;
        for (double w : want) all = all && containsNear(got, w);
        for (double g : got)
            if (!containsNear(want, g) && std::fabs(g) > 1e-9 && std::fabs(g - 1.0) > 1e-9)
                stray = true;
        CHECK(all && !stray,
              "shaders/colour.sh's pbrNeutral uses exactly the reference constants");
    }

    // ── 7. Stage B3: the .cube grading LUT ─────────────────────────────────
    {
        std::printf("\n-- 7. .cube LUTs --\n");
        // A LUT whose output IS its input coordinate. Reading the axes in the
        // wrong order still returns an identity on GREY, so the probe colour
        // below has three different channels.
        auto identityText = [](int n, const char* header = "") {
            std::string s = std::string("TITLE \"identity\"\r\n") + header +
                            "LUT_3D_SIZE " + std::to_string(n) + "\n# data\n";
            for (int b = 0; b < n; ++b)
                for (int g = 0; g < n; ++g)
                    for (int r = 0; r < n; ++r) {           // red fastest
                        char line[64];
                        std::snprintf(line, sizeof line, "%.6f %.6f %.6f\r\n",
                                      r / double(n - 1), g / double(n - 1), b / double(n - 1));
                        s += line;
                    }
            return s;
        };
        CubeLut lut; std::string err;
        CHECK(parseCubeLut(identityText(17), lut, &err) && lut.size == 17 &&
              lut.rgb.size() == 17u * 17 * 17 * 3 && lut.title == "\"identity\"",
              "a 17³ identity .cube parses, CRLF line endings included (%s)", err.c_str());
        float probe[3] = { 0.2f, 0.5f, 0.9f };
        applyCubeLut(lut, probe);
        CHECK(std::fabs(probe[0] - 0.2f) < 1e-5f && std::fabs(probe[1] - 0.5f) < 1e-5f &&
              std::fabs(probe[2] - 0.9f) < 1e-5f,
              "and returns (0.2, 0.5, 0.9) unchanged — red, not blue, changes fastest "
              "(%.4f %.4f %.4f)", (double)probe[0], (double)probe[1], (double)probe[2]);
        float over[3] = { 1.5f, -0.5f, 0.5f };
        applyCubeLut(lut, over);
        CHECK(std::fabs(over[0] - 1.0f) < 1e-5f && std::fabs(over[1]) < 1e-5f,
              "inputs outside the domain clamp to its edge");

        CubeLut ranged;
        CHECK(parseCubeLut(identityText(2, "LUT_3D_INPUT_RANGE 0 2\n"), ranged, &err) &&
              ranged.domainMax[0] == 2.0f,
              "LUT_3D_INPUT_RANGE sets the domain (%s)", err.c_str());
        float half[3] = { 1.0f, 1.0f, 1.0f };
        applyCubeLut(ranged, half);
        CHECK(std::fabs(half[0] - 0.5f) < 1e-5f, "and an input of 1 in [0,2] lands mid-LUT");

        struct Bad { const char* why; std::string text; };
        const Bad bad[] = {
            { "a 1D LUT",                "LUT_1D_SIZE 16\n0 0 0\n" },
            { "size 1",                  "LUT_3D_SIZE 1\n0 0 0\n" },
            { "size 129",                "LUT_3D_SIZE 129\n" },
            { "a fractional size",       "LUT_3D_SIZE 2.5\n" },
            { "too few entries",         "LUT_3D_SIZE 2\n0 0 0\n1 1 1\n" },
            { "a NaN entry",             identityText(2).replace(identityText(2).rfind("1.000000"), 8, "nan") },
            { "an unknown keyword",      "LUT_3D_SIZE 2\nFOO 1\n" },
            { "a keyword after data",    identityText(2) + "DOMAIN_MIN 0 0 0\n" },
            { "an inverted domain",      identityText(2, "DOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n") },
            { "four numbers on a line",  "LUT_3D_SIZE 2\n0 0 0 0\n" },
            { "data before the size",    "0 0 0\nLUT_3D_SIZE 2\n" },
            { "nothing at all",          "" },
        };
        CubeLut keep; keep.size = 99;
        bool allRefused = true;
        for (const Bad& b : bad) {
            std::string why;
            if (parseCubeLut(b.text, keep, &why)) {
                allRefused = false;
                std::printf("        accepted %s\n", b.why);
            }
        }
        CHECK(allRefused && keep.size == 99,
              "all %zu malformed LUTs are refused, and the output is left alone",
              sizeof bad / sizeof *bad);
    }

    // ── 8. The ORDER of the output pass, and the LUT's domain ─────────────
    // Review, 2026-09-17: a LUT can be built for linear, log or display-
    // referred input, and nothing but prose said which this engine feeds it.
    // These make the answer — sRGB-ENCODED, display-referred [0,1] — fail
    // loudly if it ever changes.
    {
        std::printf("\n-- 8. output pass order and the grade's domain --\n");
        using namespace display;

        // A LUT that SQUARES its input coordinate. Linear 0.214041 encodes to
        // exactly 0.5, which sits on a grid point of a 17³ LUT, so a grade of
        // ENCODED values returns 0.25. Grading LINEAR values and encoding after
        // would give encode(0.214041²) = 0.235 — measurably different.
        std::string sq = "LUT_3D_SIZE 17\n";
        for (int b = 0; b < 17; ++b)
            for (int g = 0; g < 17; ++g)
                for (int r = 0; r < 17; ++r) {
                    char line[80];
                    const double x = r / 16.0, y = g / 16.0, z = b / 16.0;
                    std::snprintf(line, sizeof line, "%.8f %.8f %.8f\n", x * x, y * y, z * z);
                    sq += line;
                }
        CubeLut square; std::string err;
        CHECK(parseCubeLut(sq, square, &err), "a squaring LUT parses (%s)", err.c_str());
        const Rgb graded = outputPixel({ 0.214041f, 0.214041f, 0.214041f }, 1.0f,
                                       ToneMapper::None, &square);
        CHECK(std::fabs(graded.r - 0.25f) < 1e-4f,
              "the grade sees the ENCODED value: linear 0.214 -> sRGB 0.5 -> "
              "LUT -> %.4f (0.25 expected; grading linear would give ~0.235)",
              (double)graded.r);

        std::string idText = "LUT_3D_SIZE 9\n";
        for (int b = 0; b < 9; ++b)
            for (int g = 0; g < 9; ++g)
                for (int r = 0; r < 9; ++r) {
                    char line[64];
                    std::snprintf(line, sizeof line, "%.8f %.8f %.8f\n", r / 8.0, g / 8.0, b / 8.0);
                    idText += line;
                }
        CubeLut id;
        parseCubeLut(idText, id);
        float worst = 0.0f;
        for (int i = 0; i <= 400; ++i) {
            const float v = (float)i * 0.01f;       // linear 0..4, past the knee
            const Rgb a = outputPixel({ v, v * 0.5f, v * 0.25f }, 1.0f, ToneMapper::PbrNeutral);
            const Rgb b = outputPixel({ v, v * 0.5f, v * 0.25f }, 1.0f, ToneMapper::PbrNeutral, &id);
            worst = std::max({ worst, std::fabs(a.r - b.r), std::fabs(a.g - b.g), std::fabs(a.b - b.b) });
        }
        CHECK(worst < 1e-5f, "an identity grade changes nothing (worst %.2e)", (double)worst);

        // Negative linear input — the reference pbrNeutral is not defined for
        // it, and folds it BRIGHT. Pinned as a fact about the reference, so the
        // clamp below is visibly necessary rather than decorative.
        const Rgb raw = pbrNeutral({ -1.0f, -1.0f, -1.0f });
        CHECK(raw.r > 0.5f,
              "the Khronos reference maps linear -1 to %.3f (bright) — it needs an "
              "upstream clamp, and gets one", (double)raw.r);
        const Rgb neg  = outputPixel({ -1.0f, -0.5f, 0.2f }, 1.0f, ToneMapper::PbrNeutral);
        const Rgb zero = outputPixel({  0.0f,  0.0f, 0.2f }, 1.0f, ToneMapper::PbrNeutral);
        CHECK(neg.r == zero.r && neg.g == zero.g && neg.b == zero.b,
              "the output pass clamps before the tone map: negative channels render "
              "exactly as zero (%.4f vs %.4f)", (double)neg.r, (double)zero.r);

        // The shader must run the same four steps in the same order.
        std::ifstream f(ENGINE_SOURCE_DIR "/shaders/fs_output.sc");
        std::stringstream ss; ss << f.rdbuf();
        std::string src = ss.str();
        const size_t mainAt = src.find("void main()");
        src = mainAt == std::string::npos ? std::string() : src.substr(mainAt);
        const size_t clampAt  = src.find("max(hdr.rgb * u_display.x, vec3_splat(0.0))");
        const size_t toneAt   = src.find("pbrNeutral(");
        const size_t encodeAt = src.find("linearToSrgb(");
        const size_t gradeAt  = src.find("texture3D(s_lut");
        const bool found = clampAt != std::string::npos && toneAt != std::string::npos &&
                           encodeAt != std::string::npos && gradeAt != std::string::npos;
        CHECK(found && clampAt < toneAt && toneAt < encodeAt && encodeAt < gradeAt,
              "fs_output.sc runs exposure+clamp, tone map, encode, grade — in that "
              "order (%zu < %zu < %zu < %zu)", clampAt, toneAt, encodeAt, gradeAt);
    }

    if (g_failures) {
        std::printf("\ncolour_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncolour_test: ALL PASS\n");
    return 0;
}
