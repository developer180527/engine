#include "assets/cookers/texture/texture_eac.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cook {

namespace {

// The ETC2/EAC modifier table. Sixteen sets of eight signed modifiers, and the
// positive half is the negative half's magnitude minus one — which is the
// property to sanity-check a transcription against, because a single wrong entry
// produces artifacts only on the textures that happen to use that table.
//
// Copied from the same table bimg's decoder reads (`s_etc2aMod` in
// bimg/src/image.cpp). Deliberately: the alpha round-trip test decodes through
// bimg, so an error HERE would be cancelled by the same error THERE. What that
// test proves is the bit layout, not the table. The table is checked instead by
// the antisymmetry assert below, which is exactly the transcription mistake a
// human makes.
constexpr int8_t kMod[16][8] = {
    { -3, -6,  -9, -15, 2, 5, 8, 14 },
    { -3, -7, -10, -13, 2, 6, 9, 12 },
    { -2, -5,  -8, -13, 1, 4, 7, 12 },
    { -2, -4,  -6, -13, 1, 3, 5, 12 },
    { -3, -6,  -8, -12, 2, 5, 7, 11 },
    { -3, -7,  -9, -11, 2, 6, 8, 10 },
    { -4, -7,  -8, -11, 3, 6, 7, 10 },
    { -3, -5,  -8, -11, 2, 4, 7, 10 },
    { -2, -6,  -8, -10, 1, 5, 7,  9 },
    { -2, -5,  -8, -10, 1, 4, 7,  9 },
    { -2, -4,  -8, -10, 1, 3, 7,  9 },
    { -2, -5,  -7, -10, 1, 4, 6,  9 },
    { -3, -4,  -7, -10, 2, 3, 6,  9 },
    { -1, -2,  -3, -10, 0, 1, 2,  9 },
    { -4, -6,  -8,  -9, 3, 5, 7,  8 },
    { -3, -5,  -7,  -9, 2, 4, 6,  8 },
};

// Every row satisfies mod[4+i] == -mod[i] - 1: the eight reachable modifiers are
// four negatives and their magnitudes-minus-one. A mistyped digit breaks it.
// (The first version of this assert paired i with 3-i and rejected the REAL
// table at row 13 — the check has to be derived from the table, not guessed at.)
constexpr bool modTableIsAntisymmetric() {
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < 4; ++i)
            if (kMod[t][4 + i] != -kMod[t][i] - 1) return false;
    return true;
}
static_assert(modTableIsAntisymmetric(),
              "EAC modifier table transcription error — the positive half of "
              "each row must be the negative half's magnitude minus one");

// Index i of 16 addresses pixel (x = i / 4, y = i % 4): the 3-bit indices run
// DOWN each column, and the first one sits at the top of the 48-bit field.
// Getting this backwards transposes every block — which looks like a plausible
// image, so it survives eyeballing and fails on a diagonal test pattern.
inline int rowMajorOfIndexSlot(int slot) {
    const int x = slot / 4, y = slot % 4;
    return y * 4 + x;
}

// base + modifier tables give 8 reachable values; pick the nearest.
template <typename T>
inline int bestModifier(const int8_t mod[8], int base, int step, T target,
                        int lo, int hi, int& err) {
    int bestI = 0, bestE = 0x7fffffff;
    for (int i = 0; i < 8; ++i) {
        const int v = std::clamp(base + mod[i] * step, lo, hi);
        const int d = (int)target - v;
        const int e = d * d;
        if (e < bestE) { bestE = e; bestI = i; }
    }
    err = bestE;
    return bestI;
}

void packBlock(int base8, int mult, int table, const int idx[16],
               uint8_t out[8]) {
    out[0] = (uint8_t)base8;
    out[1] = (uint8_t)(((mult & 0xf) << 4) | (table & 0xf));
    uint64_t bits = 0;
    for (int slot = 0; slot < 16; ++slot)
        bits |= (uint64_t)(idx[slot] & 7) << (45 - slot * 3);
    for (int i = 0; i < 6; ++i)
        out[2 + i] = (uint8_t)((bits >> (40 - i * 8)) & 0xff);
}

// ── The shared search ───────────────────────────────────────────────────────
// One pass over 16 tables. For each, the multiplier comes straight from the
// block's range (the table spans mod[7]-mod[0] steps), the base from the
// midpoint, and then ONE refinement: assign every texel its best modifier, move
// the base to the mean residual, re-assign. That second pass is what recovers
// the common case of a block whose values cluster off-centre.
//
// Cost is ~16 tables x 2 passes x 16 texels x 8 modifiers = 4k comparisons per
// block. Deliberately not an exhaustive base x multiplier x table search
// (1M per block): ETC2 is the compatibility floor, and a target nobody ships as
// their primary does not earn a minute per texture. Measured in
// texture_eac_test.
struct Solved { int base, mult, table; int idx[16]; long long err; };

Solved solveEac(const int target[16], int step, int lo, int hi, int baseShift,
                int baseBias, int minMult) {
    // step: the multiplier's own scale (1 for alpha, 8 for R11).
    // Decoded value = clamp(base*baseShift + baseBias + mod*mult*step).
    Solved best{}; best.err = -1;

    int mn = target[0], mx = target[0];
    for (int i = 1; i < 16; ++i) { mn = std::min(mn, target[i]); mx = std::max(mx, target[i]); }

    for (int t = 0; t < 16; ++t) {
        const int span = kMod[t][7] - kMod[t][0];         // always positive
        const int est = (mx - mn + span * step / 2) / (span * step);
        // THREE multiplier candidates around the range estimate, not one. With a
        // single candidate a high-variance block — a hard cutout edge, an
        // anisotropic normal — landed 25/255 off, close enough to the error a
        // CORRUPTED block produces that no test could tell them apart. Widening
        // to est-1/est/est+1 costs 3x a search that is already a rounding error
        // next to astcenc, and the measured worst case drops to single digits.
      for (int mc = -1; mc <= 1; ++mc) {
        int mult = std::clamp(est + mc, minMult, 15);
        // A multiplier of zero means two DIFFERENT things in the two codecs, and
        // conflating them was a live bug in the first draft of this file. In the
        // alpha block it collapses every modifier to zero, so the block decodes
        // to a constant — exact, and the right choice for a flat region. In R11
        // the spec reads it as a multiplier of 1/8, which after the format's own
        // x8 leaves the modifier applying UNSCALED — so a flat block encoded
        // that way decodes to base + mod, not base. Hence minMult: alpha may
        // use 0, R11 never may.
        if (mult == 0 && mn != mx) mult = 1;

        int baseVal = (mn + mx) / 2;
        Solved cand{};
        for (int pass = 0; pass < 3; ++pass) {
            const int baseCode = std::clamp((baseVal - baseBias) / baseShift, 0, 255);
            const int decodedBase = baseCode * baseShift + baseBias;
            long long err = 0, residual = 0;
            for (int slot = 0; slot < 16; ++slot) {
                const int rm = rowMajorOfIndexSlot(slot);
                int e = 0;
                const int i = bestModifier(kMod[t], decodedBase, mult * step,
                                           target[rm], lo, hi, e);
                cand.idx[slot] = i;
                err += e;
                residual += target[rm] - kMod[t][i] * mult * step;
            }
            cand.base = baseCode; cand.mult = mult; cand.table = t; cand.err = err;
            if (pass + 1 < 3) baseVal = (int)(residual / 16);   // re-centre, redo
        }
        if (best.err < 0 || cand.err < best.err) best = cand;
      }
    }
    return best;
}

} // namespace

void encodeEacAlphaBlock(const uint8_t alpha[16], uint8_t out[8]) {
    int target[16];
    for (int i = 0; i < 16; ++i) target[i] = alpha[i];
    // Alpha decodes as base + mod*mult, clamped to a byte: no widening, no bias.
    const Solved s = solveEac(target, /*step=*/1, /*lo=*/0, /*hi=*/255,
                              /*baseShift=*/1, /*baseBias=*/0, /*minMult=*/0);
    packBlock(s.base, s.mult, s.table, s.idx, out);
}

void encodeEacR11Block(const uint8_t value[16], uint8_t out[8]) {
    // 8-bit source into the codec's 11-bit range. Rounding matters: truncating
    // here costs half an LSB on every texel of a normal map, which is a visible
    // banding pattern on a smooth curved surface.
    int target[16];
    for (int i = 0; i < 16; ++i)
        target[i] = (int)std::lround(value[i] * 2047.0 / 255.0);
    // R11 decodes as base*8 + 4 + mod*mult*8.
    const Solved s = solveEac(target, /*step=*/8, /*lo=*/0, /*hi=*/2047,
                              /*baseShift=*/8, /*baseBias=*/4, /*minMult=*/1);
    packBlock(s.base, s.mult, s.table, s.idx, out);
}

void decodeEacAlphaBlock(const uint8_t in[8], uint8_t alpha[16]) {
    const int base = in[0];
    const int mult = (in[1] >> 4) & 0xf;
    const int8_t* mod = kMod[in[1] & 0xf];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)in[2 + i] << (40 - i * 8);
    for (int slot = 0; slot < 16; ++slot) {
        const int i = (int)((bits >> (45 - slot * 3)) & 7);
        alpha[rowMajorOfIndexSlot(slot)] =
            (uint8_t)std::clamp(base + mod[i] * mult, 0, 255);
    }
}

void decodeEacR11Block(const uint8_t in[8], uint16_t value[16]) {
    const int base = in[0] * 8 + 4;
    const int mult = (in[1] >> 4) & 0xf;
    const int8_t* mod = kMod[in[1] & 0xf];
    // A multiplier of zero is not "no modifier" — the spec reads it as 1/8,
    // which after the codec's own x8 means the modifier applies unscaled.
    const int step = mult == 0 ? 1 : mult * 8;
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)in[2 + i] << (40 - i * 8);
    for (int slot = 0; slot < 16; ++slot) {
        const int i = (int)((bits >> (45 - slot * 3)) & 7);
        value[rowMajorOfIndexSlot(slot)] =
            (uint16_t)std::clamp(base + mod[i] * step, 0, 2047);
    }
}

} // namespace cook
