// ── texture_format_test — the mobile block formats, and the DDC key ──────────
//
// Three things are being held down here, in rising order of how badly they fail
// when wrong.
//
// 1. BLOCK GEOMETRY. Everything before this change assumed a 4x4 block. ASTC
//    exists at 6x6 and 8x8, so `ceil(w/4)*ceil(h/4)*bytes` is simply the wrong
//    size — and a mip whose size is wrong does not fail loudly. It shifts every
//    subsequent mip in the chain, so mip 3 reads mip 4's bytes as pixels and the
//    texture goes to fog at a distance. Pure arithmetic, so it is checked
//    exhaustively and cheaply.
//
// 2. THE TARGET IN THE DDC KEY. The source PNG is byte-identical whichever
//    device the build is for. Without the target in `settingsFingerprint`, a
//    desktop machine's cached BC7 blob answers a phone's ASTC request — and the
//    thing that broke the build is the CACHE, whose whole promise is that a hit
//    equals a cook. Same shape as the arm64/x86 NaN divergence: invisible on the
//    machine that produced it.
//
// 3. THE EAC ENCODER we had to write, because bimg has ASTC and ETC2-RGB and no
//    EAC at all. Alpha is round-tripped through BIMG'S decoder — an
//    implementation nobody here wrote — so the bit layout, the nibble order and
//    the column-major index packing are pinned against a third party.
//
//    R11 is round-tripped through our own decoder, which proves the search is
//    sane and proves NOTHING about matching the hardware: a consistently wrong
//    pair of functions passes. Said plainly here rather than left for someone to
//    assume, and the honest fix is a device lane, not another host-side test.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <assetlib/texture_asset.h>
#include <bimg/bimg.h>
#include <bx/allocator.h>

#include "assets/cookers/texture/texture_cooker.h"
#include "assets/cookers/texture/texture_eac.h"
#include "assets/cookers/texture/texture_encode.h"
#include "assets/cookers/texture/texture_target.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

using namespace assetlib;

// ── A source image with something in every channel ──────────────────────────
// Flat colour would let a broken encoder pass everything: a constant block is
// exact in every codec. This has a horizontal gradient (tests the modifier
// search), a diagonal (tests index ORDER — a transposed block still looks like
// an image), and a non-trivial alpha ramp.
static std::vector<uint8_t> testImage(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            p[2] = (uint8_t)((x + y) & 0xff);
            p[3] = (uint8_t)(((x * 3 + y * 5) & 0x7f) + 128);
        }
    return px;
}

static void withTarget(const char* v) {
    if (v) ::setenv("COOK_TEX_TARGET", v, 1);
    else   ::unsetenv("COOK_TEX_TARGET");
}

// ── 1. Block geometry ───────────────────────────────────────────────────────
static void testBlockGeometry() {
    std::printf("\n── block geometry ──\n");

    CHECK(texBlockDims(kTexBC1).bytes == 8 && texBlockDims(kTexBC1).w == 4,
          "BC1 is 8 bytes per 4x4 block");
    CHECK(texBlockDims(kTexASTC6x6).w == 6 && texBlockDims(kTexASTC6x6).h == 6 &&
          texBlockDims(kTexASTC6x6).bytes == 16,
          "ASTC 6x6 is 16 bytes per 6x6 block — every ASTC block is 128 bits");
    CHECK(texBlockDims(kTexETC2).bytes == 8 && texBlockDims(kTexETC2A).bytes == 16,
          "ETC2 RGB is 8 bytes; adding EAC alpha makes it 16");

    // The case the old 4x4-only math got wrong. A 12x12 ASTC 6x6 mip is 2x2
    // blocks = 64 bytes; the hard-coded version would have said 3x3 = 144.
    CHECK(texMipBytes(kTexASTC6x6, 12, 12) == 64,
          "12x12 at ASTC 6x6 is 2x2 blocks (64 B), not ceil(12/4)^2 (144 B)");
    CHECK(texMipBytes(kTexASTC8x8, 9, 9) == 4 * 16,
          "9x9 at ASTC 8x8 rounds UP to 2x2 blocks — partial edge blocks are "
          "read in full by the hardware");
    CHECK(texMipBytes(kTexASTC6x6, 1, 1) == 16,
          "a 1x1 mip is still one whole 6x6 block");
    CHECK(texMipBytes(kTexRGBA8, 4, 4) == 64, "RGBA8 stays 4 bytes per texel");

    // texChainBytes must agree with summing the levels by hand, which is the
    // only thing the loader can check a cooked file against.
    for (uint32_t fmt : { kTexBC1, kTexBC7, kTexASTC4x4, kTexASTC6x6,
                          kTexASTC8x8, kTexETC2, kTexETC2A, kTexEACRG11 }) {
        size_t byHand = 0;
        uint32_t w = 64, h = 32, mips = 0;
        for (;;) { byHand += texMipBytes(fmt, w, h); ++mips;
                   if (w == 1 && h == 1) break;
                   w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1; }
        CHECK(texChainBytes(fmt, 64, 32, mips) == byHand,
              "%s: texChainBytes matches a hand-summed 64x32 chain (%zu B, %u mips)",
              texFormatName(fmt), byHand, mips);
    }
}

// ── 2. Target resolution and the format matrix ──────────────────────────────
static void testTargets() {
    std::printf("\n── targets ──\n");
    using cook::TexTarget;

    withTarget(nullptr);
    CHECK(cook::resolveTexTarget() == TexTarget::BC,
          "no COOK_TEX_TARGET means bc — a desktop-first engine's default");
    withTarget("astc");
    CHECK(cook::resolveTexTarget() == TexTarget::ASTC, "'astc' resolves");
    withTarget("ios");
    CHECK(cook::resolveTexTarget() == TexTarget::ASTC, "'ios' is an alias for astc");
    withTarget("etc2");
    CHECK(cook::resolveTexTarget() == TexTarget::ETC2, "'etc2' resolves");

    withTarget("ASTC");
    CHECK(cook::resolveTexTarget() == TexTarget::ASTC, "case is not significant");

    // A typo must not fall back SILENTLY: that is a mobile build script quietly
    // producing a desktop build, discovered on a device.
    withTarget("astc4x4");
    std::string why;
    CHECK(cook::resolveTexTarget(&why) == TexTarget::BC &&
          why.find("astc4x4") != std::string::npos,
          "an unrecognised value falls back to bc AND says so");
    withTarget(nullptr);

    // The matrix. hasAlpha is irrelevant on ASTC (every block size carries 4
    // channels) and decisive on BC and ETC2.
    CHECK(cook::texFormatFor(TexTarget::BC, false, false, false) == kTexBC1 &&
          cook::texFormatFor(TexTarget::BC, false, true,  false) == kTexBC3 &&
          cook::texFormatFor(TexTarget::BC, false, false, true)  == kTexBC7 &&
          cook::texFormatFor(TexTarget::BC, true,  false, false) == kTexBC5,
          "bc: BC1/BC3 by alpha, BC7 on hq, BC5 for normals");
    CHECK(cook::texFormatFor(TexTarget::ASTC, false, false, false) == kTexASTC6x6 &&
          cook::texFormatFor(TexTarget::ASTC, false, true,  false) == kTexASTC6x6 &&
          cook::texFormatFor(TexTarget::ASTC, false, false, true)  == kTexASTC4x4,
          "astc: 6x6 by default, 4x4 on hq, and alpha changes nothing");
    CHECK(cook::texFormatFor(TexTarget::ASTC, true, false, false) == kTexASTC4x4,
          "astc: normals get 4x4 even off the hq tier — 6x6 normals are where "
          "mobile ports get their reputation");
    CHECK(cook::texFormatFor(TexTarget::ETC2, false, false, false) == kTexETC2 &&
          cook::texFormatFor(TexTarget::ETC2, false, true,  false) == kTexETC2A &&
          cook::texFormatFor(TexTarget::ETC2, true,  false, false) == kTexEACRG11,
          "etc2: ETC2/ETC2A by alpha, EAC RG11 for normals");
}

// ── 2b. The cook KEY moves with the target ──────────────────────────────────
// This is the assertion the whole mobile pipeline rests on, and it is one line of
// string concatenation away from being silently false. Tested directly rather
// than inferred from the fact that the code reads the env var: `resolveTexTarget`
// being called is not the same as its answer reaching the key.
static void testFingerprintKeysTheTarget() {
    std::printf("\n── the target keys the DDC ──\n");
    TextureCooker cooker;
    assetlib::CookContext ctx;
    ctx.sourcePath = "art/rock_albedo.png";
    ctx.outputPath = "cache/rock.ctex";

    withTarget("bc");   const std::string bc   = cooker.settingsFingerprint(ctx);
    withTarget("astc"); const std::string astc = cooker.settingsFingerprint(ctx);
    withTarget("etc2"); const std::string etc2 = cooker.settingsFingerprint(ctx);
    withTarget(nullptr);

    CHECK(bc != astc && astc != etc2 && bc != etc2,
          "all three targets produce DIFFERENT fingerprints ('%s' / '%s' / '%s')",
          bc.c_str(), astc.c_str(), etc2.c_str());

    // And it must be STABLE for a fixed environment, or the key changes on every
    // cook and the cache never hits at all — the opposite failure, equally fatal
    // and much easier to ship because everything still looks correct.
    withTarget("astc");
    CHECK(cooker.settingsFingerprint(ctx) == astc,
          "and the same environment gives the same fingerprint twice");
    withTarget(nullptr);

    // The normal-map heuristic must still separate, since it also changes the
    // format for the same source bytes.
    assetlib::CookContext nrm = ctx;
    nrm.sourcePath = "art/rock_normal.png";
    CHECK(cooker.settingsFingerprint(nrm) != bc,
          "the normal-map heuristic still moves the key too");
}

// ── 3. Whole-image encode, every family ─────────────────────────────────────
static void testEncode() {
    std::printf("\n── encode ──\n");

    struct Case { const char* target; bool normal; bool hq; uint32_t expect; };
    const Case cases[] = {
        { "bc",   false, false, kTexBC3     },   // the test image has alpha
        { "bc",   true,  false, kTexBC5     },
        { "bc",   false, true,  kTexBC7     },
        { "astc", false, false, kTexASTC6x6 },
        { "astc", false, true,  kTexASTC4x4 },
        { "astc", true,  false, kTexASTC4x4 },
        { "etc2", false, false, kTexETC2A   },
        { "etc2", true,  false, kTexEACRG11 },
    };

    // 40x24: deliberately NOT a multiple of 6 or 8, so the ASTC cases exercise
    // partial edge blocks, and the chain runs down through 5x3, 2x1, 1x1 —
    // the sizes that made bimg's ETC2 loop read past the end of the image.
    const uint32_t W = 40, H = 24;
    const std::vector<uint8_t> src = testImage(W, H);

    for (const Case& c : cases) {
        withTarget(c.target);
        if (c.hq) ::setenv("COOK_TEX_HQ", "1", 1); else ::unsetenv("COOK_TEX_HQ");

        TextureAsset a;
        const bool ok = cook::encodeTexture(src.data(), W, H, c.normal, a);
        CHECK(ok, "%s%s%s encodes", c.target, c.normal ? " normal" : "",
              c.hq ? " hq" : "");
        if (!ok) continue;

        CHECK(a.header.format == c.expect,
              "  -> %s (expected %s)", texFormatName(a.header.format),
              texFormatName(c.expect));
        // 40x24 -> 20x12 -> 10x6 -> 5x3 -> 2x1 -> 1x1: six levels. The chain
        // stops when BOTH dimensions reach 1, so the count follows the LONGER
        // side. (Asserted at 7 first, which was simply my arithmetic being
        // wrong — worth keeping the real derivation written down.)
        CHECK(a.header.mipCount == 6,
              "  -> 6 mips for 40x24 (%u)", a.header.mipCount);
        // The payload must be EXACTLY the chain size. Too small and the GPU
        // reads past the buffer; too large and every mip after the first is
        // offset, which is the failure that looks like bad texture filtering.
        const size_t want = texChainBytes(a.header.format, W, H, a.header.mipCount);
        CHECK(a.pixels.size() == want,
              "  -> payload is exactly the mip chain: %zu B", a.pixels.size());

        // Not all one value. A wired-up-but-broken encoder that writes zeroes
        // satisfies every size check above.
        size_t nonZero = 0;
        for (uint8_t b : a.pixels) if (b) ++nonZero;
        CHECK(nonZero > a.pixels.size() / 10,
              "  -> the blocks carry data (%zu/%zu non-zero)", nonZero,
              a.pixels.size());

        // Determinism, which the DDC depends on absolutely — and which
        // ENGINE_COOK_DETERMINISM_CHECK would catch at the pipeline level, but
        // only if someone had it switched on.
        TextureAsset b;
        CHECK(cook::encodeTexture(src.data(), W, H, c.normal, b) &&
              b.pixels == a.pixels && b.header.format == a.header.format,
              "  -> encoding twice gives identical bytes");
    }
    withTarget(nullptr);
    ::unsetenv("COOK_TEX_HQ");
}

// ── 4. EAC alpha against bimg's decoder ─────────────────────────────────────
// MEASURED separation, so the bounds below are not guesses (probe run against
// bimg's decoder on the diagonal pattern):
//
//   correct encode ................ max  25   rms  10.9
//   modifier-table nibble flipped .. max  35   rms  17.8
//   multiplier nibble flipped ...... max 115   rms  75.2
//   index bytes reversed ........... max 190   rms 118.9
//
// So RMS is the metric to bound and MAX is not: a correct encode of a
// deliberately bimodal block is 25/255 off at its worst texel, which is only
// 10 away from the weakest corruption. RMS puts correct at 10.9 against a
// worst-case-corruption floor of 17.8 — a real gap, and the bit-layout errors
// that actually matter are 7x-11x outside it.
static void testEacAlphaAgainstBimg() {
    std::printf("\n── EAC alpha, decoded by bimg ──\n");
    static bx::DefaultAllocator alloc;

    struct Pattern { const char* name; uint8_t a[16]; bool exact; };
    Pattern pats[] = {
        { "constant",  {200,200,200,200, 200,200,200,200,
                        200,200,200,200, 200,200,200,200}, true },
        { "two-level", {  0,  0,  0,  0, 255,255,255,255,
                          0,  0,  0,  0, 255,255,255,255}, true },
        { "ramp",      {  0, 17, 34, 51, 68, 85,102,119,
                        136,153,170,187, 204,221,238,255}, false },
        // Distinct in every position, so a transposed or reversed index order
        // cannot possibly pass. This is the one that catches a bit-layout error.
        { "diagonal",  { 10,120,130,140, 150, 20,160,170,
                        180,190, 30,200, 210,220,230, 40}, false },
    };

    // Encode, wrap as a full ETC2A block, decode through bimg, report RMS+max.
    // bimg reads alpha from the FIRST 8 bytes — if that order were wrong this
    // would show garbage, which is the whole point of using its decoder.
    auto roundTrip = [](const uint8_t src[16], int corrupt,
                        double& rms, int& maxErr) {
        uint8_t block[8];
        cook::encodeEacAlphaBlock(src, block);
        uint8_t etc2a[16];
        std::memcpy(etc2a, block, 8);
        std::memset(etc2a + 8, 0, 8);
        if (corrupt == 1) etc2a[1] ^= 0x0f;                    // modifier table
        if (corrupt == 2) etc2a[1] ^= 0xf0;                    // multiplier
        if (corrupt == 3) for (int i = 0; i < 3; ++i)           // index order
                              std::swap(etc2a[2 + i], etc2a[7 - i]);

        static bx::DefaultAllocator a2;
        uint8_t rgba[4 * 4 * 4] = {};
        bimg::imageDecodeToRgba8(&a2, rgba, etc2a, 4, 4, 4 * 4,
                                 bimg::TextureFormat::ETC2A);
        double sse = 0; maxErr = 0;
        for (int i = 0; i < 16; ++i) {
            const int d = (int)rgba[i * 4 + 3] - (int)src[i];
            sse += (double)d * d;
            maxErr = std::max(maxErr, std::abs(d));
        }
        rms = std::sqrt(sse / 16.0);
    };
    (void)alloc;

    for (Pattern& p : pats) {
        double rms; int maxErr;
        roundTrip(p.a, 0, rms, maxErr);
        if (p.exact) {
            CHECK(maxErr == 0, "%s: EXACT through bimg's decoder", p.name);
        } else {
            CHECK(rms <= 14.0 && maxErr <= 28,
                  "%s: rms %.1f (<=14), max %d (<=28) through bimg's decoder",
                  p.name, rms, maxErr);
        }
    }

    // NEGATIVE CONTROLS. Every corruption of the three fields must break the RMS
    // bound — otherwise the bound above is measuring nothing and this whole file
    // is decoration. The index-order case is the important one: it is the error
    // a hand-written bit packer actually makes, and it produces an image that
    // still looks like an image.
    struct Corruption { int kind; const char* what; };
    for (const Corruption& c : { Corruption{1, "modifier table nibble"},
                                 Corruption{2, "multiplier nibble"},
                                 Corruption{3, "index byte order"} }) {
        double rms; int maxErr;
        roundTrip(pats[3].a, c.kind, rms, maxErr);
        CHECK(rms > 14.0, "corrupting the %s breaks the bound (rms %.1f, max %d)",
              c.what, rms, maxErr);
    }
}

// ── 5. EAC R11 ──────────────────────────────────────────────────────────────
static void testEacR11() {
    std::printf("\n── EAC R11 (our decoder only — see the header) ──\n");

    const uint8_t ramp[16] = { 0, 17, 34, 51, 68, 85, 102, 119,
                              136, 153, 170, 187, 204, 221, 238, 255 };
    const uint8_t flat[16] = { 128,128,128,128, 128,128,128,128,
                               128,128,128,128, 128,128,128,128 };
    const uint8_t diag[16] = { 10,120,130,140, 150, 20,160,170,
                              180,190, 30,200, 210,220,230, 40 };

    struct C { const char* name; const uint8_t* v; int bound; };
    // Bounds from measurement, with headroom: ramp 17, flat 0, diagonal 19 as
    // written. Max-error rather than RMS here only because there is no
    // third-party decoder to compare RMS against — see the file header.
    for (const C& c : { C{"ramp", ramp, 22}, C{"flat", flat, 2},
                        C{"diagonal", diag, 22} }) {
        uint8_t block[8];
        uint16_t out11[16];
        cook::encodeEacR11Block(c.v, block);
        cook::decodeEacR11Block(block, out11);
        int maxErr = 0;
        for (int i = 0; i < 16; ++i) {
            const int got8 = (int)((out11[i] * 255 + 1023) / 2047);
            maxErr = std::max(maxErr, std::abs(got8 - (int)c.v[i]));
        }
        CHECK(maxErr <= c.bound, "%s: max error %d/255 (bound %d)",
              c.name, maxErr, c.bound);
    }

    // The multiplier-zero trap. R11 reads mult==0 as 1/8, so a flat block
    // encoded with mult 0 decodes to base + modifier, NOT to base — the first
    // draft of the encoder shared alpha's handling and got exactly this wrong.
    {
        uint8_t block[8];
        cook::encodeEacR11Block(flat, block);
        CHECK(((block[1] >> 4) & 0xf) != 0,
              "a flat R11 block does not use multiplier 0 (it would decode to "
              "base + modifier, not base)");
    }

    // Order guard: the diagonal must come back in the SAME positions. A
    // transposed index packing passes every error bound above, because the set
    // of values is unchanged.
    {
        uint8_t block[8]; uint16_t out11[16];
        cook::encodeEacR11Block(diag, block);
        cook::decodeEacR11Block(block, out11);
        int worstPos = 0;
        for (int i = 0; i < 16; ++i) {
            const int got8 = (int)((out11[i] * 255 + 1023) / 2047);
            // Each texel must be nearest to ITS OWN source value, not to some
            // other texel's.
            for (int j = 0; j < 16; ++j)
                if (j != i && std::abs(got8 - (int)diag[j]) + 25 <
                              std::abs(got8 - (int)diag[i]))
                    ++worstPos;
        }
        CHECK(worstPos == 0,
              "every texel decodes nearest to its own source value — the index "
              "order is not transposed (%d violations)", worstPos);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("texture_format_test\n");
    testBlockGeometry();
    testTargets();
    testFingerprintKeysTheTarget();
    testEncode();
    testEacAlphaAgainstBimg();
    testEacR11();
    if (g_failures) {
        std::printf("\ntexture_format_test: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\ntexture_format_test: PASS\n");
    return 0;
}
