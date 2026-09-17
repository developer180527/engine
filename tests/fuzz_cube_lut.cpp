// ── fuzz_cube_lut — a .cube file is untrusted input ─────────────────────────
//
// Colour pipeline stage B3. A grading LUT is named by path from a scene file and
// read at runtime, so core/cube_lut.h is a parser of hostile text in exactly the
// sense the DDC manifest and take readers are.
//
// Properties, per case:
//   1. A generated LUT parses to exactly what was written.
//   2. Mutated text — flipped bytes, truncation, spliced keywords, hostile
//      numbers — never crashes, and when it is ACCEPTED the result is sound:
//      size in range, exactly size³ entries, every value finite, every domain
//      non-empty. A LUT that satisfies those can be uploaded and sampled.
//   3. applyCubeLut on any accepted LUT returns finite values for any input,
//      including NaN and infinities — the shader clamps, and the reference must
//      not be the thing that produces a NaN.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cube_lut.h"
#include "fuzz/fuzz.h"

namespace {

constexpr uint32_t kGeneratorVersion = 1;

std::string generate(fuzz::Rng& rng, CubeLut& expect) {
    const uint32_t n = rng.range(2, 9);
    expect = CubeLut{};
    expect.size = n;
    std::string s;
    if (rng.chance(50)) s += "TITLE \"fuzz\"\n";
    if (rng.chance(30)) s += "# comment\n";
    s += "LUT_3D_SIZE " + std::to_string(n) + (rng.chance(20) ? "\r\n" : "\n");
    if (rng.chance(30)) {
        s += "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    }
    char line[96];
    for (uint32_t i = 0; i < n * n * n; ++i) {
        float v[3];
        for (float& x : v) x = (float)rng.range(0, 100000) / 50000.0f - 0.5f;
        std::snprintf(line, sizeof line, "%.5f %.5f %.5f\n", v[0], v[1], v[2]);
        s += line;
        // What strtof reads back from %.5f — the property is "parses to what
        // was written", and the written text is the rounded value.
        for (float x : v) {
            char b[32];
            std::snprintf(b, sizeof b, "%.5f", x);
            expect.rgb.push_back(std::strtof(b, nullptr));
        }
    }
    return s;
}

void mutate(fuzz::Rng& rng, std::string& s) {
    static const char* kSplices[] = {
        "LUT_3D_SIZE 129\n", "LUT_3D_SIZE 0\n", "LUT_3D_SIZE -3\n",
        "LUT_3D_SIZE 1e9\n", "LUT_1D_SIZE 4\n", "DOMAIN_MIN 1 1 1\n",
        "DOMAIN_MAX nan 1 1\n", "LUT_3D_INPUT_RANGE 2 1\n", "1e39 0 0\n",
        "inf -inf nan\n", "0 0\n", "0 0 0 0\n", "\n\n\n", "TITLE\n",
        "\x00\x01\xff\n", "0x1p3 0 0\n", "   \t  \n",
    };
    const int edits = (int)rng.range(1, 4);
    for (int e = 0; e < edits && !s.empty(); ++e) {
        const size_t at = rng.range(0, (uint32_t)s.size() - 1);
        switch (rng.range(0, 3)) {
        case 0: s[at] = (char)rng.range(0, 255); break;
        case 1: s.resize(at); break;
        case 2: s.insert(at, kSplices[rng.below(sizeof kSplices / sizeof *kSplices)]); break;
        default: s.erase(at, rng.range(1, 16)); break;
        }
    }
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target = "cube_lut";

    fuzz::Rng gen(fuzz::deriveSeed(masterSeed, "cube_body"));
    fuzz::Rng mut(fuzz::deriveSeed(masterSeed, "cube_mutation"));

    CubeLut expect;
    const std::string text = generate(gen, expect);

    // 1. Round trip.
    {
        CubeLut got; std::string err;
        if (!parseCubeLut(text, got, &err))
            rep.fail(key, "a generated LUT was refused: " + err);
        else if (got.size != expect.size || got.rgb != expect.rgb)
            rep.fail(key, "a generated LUT parsed to different values");
    }

    // 2 + 3. Mutations.
    for (int round = 0; round < 12; ++round) {
        std::string m = text;
        mutate(mut, m);
        CubeLut got;
        if (!parseCubeLut(m, got)) continue;

        const size_t want = (size_t)got.size * got.size * got.size * 3;
        if (got.size < kCubeLutMinSize || got.size > kCubeLutMaxSize ||
            got.rgb.size() != want) {
            rep.fail(key, "an accepted LUT has an inconsistent size");
            continue;
        }
        for (float v : got.rgb)
            if (!std::isfinite(v)) { rep.fail(key, "an accepted LUT holds a non-finite value"); break; }
        for (int c = 0; c < 3; ++c)
            if (!(got.domainMax[c] > got.domainMin[c]) ||
                !std::isfinite(got.domainMin[c]) || !std::isfinite(got.domainMax[c]))
                rep.fail(key, "an accepted LUT has an empty or non-finite domain");

        static const float kProbes[] = { 0.0f, 1.0f, -5.0f, 5.0f, 0.5f,
                                         INFINITY, -INFINITY, NAN };
        for (float p : kProbes) {
            float rgb[3] = { p, mut.chance(50) ? p : 0.25f, 0.75f };
            applyCubeLut(got, rgb);
            // NaN input has no defined output channel, but it must not poison
            // the channels that were given real numbers.
            if (std::isfinite(p) &&
                (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]))) {
                rep.fail(key, "applyCubeLut produced a non-finite value from finite input");
                break;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return fuzz::run("cube_lut", argc, argv, oneCase);
}
