#pragma once
// ── cube_lut — the .cube grading LUT: parser and CPU reference ──────────────
//
// Colour pipeline stage B3. A grade is authored offline (DaVinci Resolve, or an
// OCIO processor baked to one 3D LUT) and exported as a .cube: a text file of
// N³ RGB triples. The runtime samples it as a 3D texture after tone mapping and
// the sRGB encode, so the LUT sees display-referred, sRGB-encoded [0,1] — the
// domain a "Rec.709 / sRGB" export from those tools expects.
//
// ── A .cube IS UNTRUSTED INPUT ──────────────────────────────────────────────
// It comes from outside the engine, by path from a scene file anyone can edit,
// so this is a parser in exactly the sense engineering-standards.md means, and
// fuzz_cube_lut drives it. It REFUSES rather than guesses: an unknown keyword,
// a 1D LUT, a size out of range, a count that is not exactly size³, a
// non-finite value or an inverted domain each fail with a reason. A LUT that
// half-parses would grade an image wrong with nothing in the log.
//
// ── FORMAT (Adobe Cube LUT Specification 1.0) ───────────────────────────────
//   TITLE "text"            optional
//   LUT_3D_SIZE N           required, 2..128 here
//   DOMAIN_MIN r g b        optional, default 0 0 0
//   DOMAIN_MAX r g b        optional, default 1 1 1
//   LUT_3D_INPUT_RANGE a b  Resolve's shorthand for a uniform domain
//   r g b                   N³ data lines, RED CHANGING FASTEST, then green,
//                           then blue
// Lines starting with '#' are comments. The axis order is the one detail every
// .cube reader gets wrong once; colour_test pins it with a LUT whose channels
// are distinguishable, because an identity LUT read with red and blue swapped
// still looks like an identity on grey.
//
// Header-only and dependency-free, like the rest of core/: the runtime, the
// fuzzer and any tool parse with the same code.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

inline constexpr uint32_t kCubeLutMinSize = 2;
// 128³ × 3 floats is 24 MB — past any grade a game ships (33 and 65 are the
// common sizes) and a sane ceiling on what a hostile header can make us allocate.
inline constexpr uint32_t kCubeLutMaxSize = 128;

struct CubeLut {
    uint32_t           size = 0;
    float              domainMin[3] = { 0.0f, 0.0f, 0.0f };
    float              domainMax[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<float> rgb;          // size³ × 3, red fastest
    std::string        title;
};

namespace cube_lut_detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.remove_suffix(1);
    return s;
}

// Exactly `n` finite floats and nothing else. strtof on a NUL-terminated copy:
// string_view is not terminated, and strtof reading past the line would accept
// the next line's number as this one's.
inline bool readFloats(std::string_view s, float* out, int n) {
    std::string buf(s);
    const char* p = buf.c_str();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p || !std::isfinite(v)) return false;
        out[i] = v;
        p = end;
    }
    while (*p && std::isspace((unsigned char)*p)) ++p;
    return *p == '\0';
}

}  // namespace cube_lut_detail

// False, with a reason in `err`, for anything that is not wholly a 3D .cube this
// engine can use. `out` is untouched on failure.
inline bool parseCubeLut(std::string_view text, CubeLut& out,
                         std::string* err = nullptr) {
    using namespace cube_lut_detail;
    auto fail = [&](std::string why) { if (err) *err = std::move(why); return false; };

    CubeLut lut;
    size_t expected = 0;
    size_t lineNo = 0;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        if (line.empty() || line.front() == '#') continue;

        const bool keyword = std::isalpha((unsigned char)line.front()) != 0;
        if (keyword) {
            if (!lut.rgb.empty())
                return fail("line " + std::to_string(lineNo) + ": keyword after data");
            const size_t sp = line.find_first_of(" \t");
            const std::string_view key = line.substr(0, sp);
            const std::string_view rest =
                sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp));
            if (key == "TITLE") {
                lut.title = std::string(rest);
            } else if (key == "LUT_3D_SIZE") {
                float v[1];
                if (!readFloats(rest, v, 1) || v[0] != std::floor(v[0]) ||
                    v[0] < (float)kCubeLutMinSize || v[0] > (float)kCubeLutMaxSize)
                    return fail("LUT_3D_SIZE must be a whole number in " +
                                std::to_string(kCubeLutMinSize) + ".." +
                                std::to_string(kCubeLutMaxSize));
                lut.size = (uint32_t)v[0];
                expected = (size_t)lut.size * lut.size * lut.size;
                lut.rgb.reserve(expected * 3);
            } else if (key == "DOMAIN_MIN") {
                if (!readFloats(rest, lut.domainMin, 3)) return fail("bad DOMAIN_MIN");
            } else if (key == "DOMAIN_MAX") {
                if (!readFloats(rest, lut.domainMax, 3)) return fail("bad DOMAIN_MAX");
            } else if (key == "LUT_3D_INPUT_RANGE") {
                float r[2];
                if (!readFloats(rest, r, 2)) return fail("bad LUT_3D_INPUT_RANGE");
                for (int i = 0; i < 3; ++i) { lut.domainMin[i] = r[0]; lut.domainMax[i] = r[1]; }
            } else if (key == "LUT_1D_SIZE") {
                return fail("1D LUTs are not supported — export a 3D LUT");
            } else {
                return fail("line " + std::to_string(lineNo) + ": unknown keyword '" +
                            std::string(key.substr(0, 32)) + "'");
            }
            continue;
        }

        if (lut.size == 0) return fail("data before LUT_3D_SIZE");
        if (lut.rgb.size() >= expected * 3)
            return fail("more than LUT_3D_SIZE³ data lines");
        float v[3];
        if (!readFloats(line, v, 3))
            return fail("line " + std::to_string(lineNo) + ": expected three finite numbers");
        lut.rgb.insert(lut.rgb.end(), v, v + 3);
    }

    if (lut.size == 0) return fail("no LUT_3D_SIZE");
    if (lut.rgb.size() != expected * 3)
        return fail("expected " + std::to_string(expected) + " entries, found " +
                    std::to_string(lut.rgb.size() / 3));
    for (int i = 0; i < 3; ++i)
        if (!(lut.domainMax[i] > lut.domainMin[i]))
            return fail("DOMAIN_MAX must exceed DOMAIN_MIN on every channel");
    out = std::move(lut);
    return true;
}

// Trilinear lookup — what the GPU's linear 3D sample does, and the reference the
// shader's texel-centre mapping is written against. Inputs are clamped to the
// domain, as a clamped 3D texture clamps them.
inline void applyCubeLut(const CubeLut& lut, float rgb[3]) {
    const uint32_t n = lut.size;
    if (n < 2) return;
    int   i0[3], i1[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        float t = (rgb[c] - lut.domainMin[c]) / (lut.domainMax[c] - lut.domainMin[c]);
        t = std::clamp(t, 0.0f, 1.0f) * (float)(n - 1);
        i0[c] = std::min((int)t, (int)n - 2);
        i1[c] = i0[c] + 1;
        f[c]  = t - (float)i0[c];
    }
    auto at = [&](int r, int g, int b, int ch) {
        return lut.rgb[(((size_t)b * n + (size_t)g) * n + (size_t)r) * 3 + (size_t)ch];
    };
    for (int ch = 0; ch < 3; ++ch) {
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        const float c00 = lerp(at(i0[0], i0[1], i0[2], ch), at(i1[0], i0[1], i0[2], ch), f[0]);
        const float c10 = lerp(at(i0[0], i1[1], i0[2], ch), at(i1[0], i1[1], i0[2], ch), f[0]);
        const float c01 = lerp(at(i0[0], i0[1], i1[2], ch), at(i1[0], i0[1], i1[2], ch), f[0]);
        const float c11 = lerp(at(i0[0], i1[1], i1[2], ch), at(i1[0], i1[1], i1[2], ch), f[0]);
        rgb[ch] = lerp(lerp(c00, c10, f[1]), lerp(c01, c11, f[1]), f[2]);
    }
}
