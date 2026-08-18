#pragma once
// ── texture_target — WHICH block format family this cook is for ──────────────
//
// A texture's compressed format is not a property of the asset. It is a
// property of the machine that will sample it, and no one family runs
// everywhere:
//
//   BC     desktop + Steam Deck (RDNA2 is a PC GPU, not a mobile one).
//          Absent from every iOS and Android device ever made.
//   ASTC   every Metal-capable iPhone/iPad, every modern Adreno/Mali/PowerVR.
//          Absent from AMD and NVIDIA desktop parts.
//   ETC2   mandatory in GLES 3.0, so it reaches the old Android tail that
//          predates ASTC. Worse than either of the above at equal bitrate —
//          it buys reach, not quality.
//
// So this comes from the ENVIRONMENT, exactly like COOK_SHADER_PROFILES:
//
//   COOK_TEX_TARGET=bc        (default — desktop/Deck)
//   COOK_TEX_TARGET=astc      (iOS, modern Android, handhelds with ASTC)
//   COOK_TEX_TARGET=etc2      (GLES 3.0 floor)
//
// ── WHY THIS MUST KEY THE DDC ────────────────────────────────────────────────
// The DDC names a cooked output by a hash of its inputs and shares that store
// between machines. The source PNG is byte-identical for every target, so
// WITHOUT the target in the key a desktop machine's BC7 blob satisfies a
// phone's ASTC request: the phone build silently ships blocks its GPU cannot
// decode, and the DDC — whose whole promise is that a cache hit equals a cook —
// is the thing that broke it. That is the same class of bug as the arm64/x86
// NaN divergence, and it is why `settingsFingerprint` is not optional here.
//
// ONE target per cook run, not a fat multi-format container. A .ctex holding
// all three families would triple every download so that each device could
// throw two thirds away; shipping one build per store listing is what everyone
// actually does.
#include <cstdint>
#include <string>

namespace cook {

enum class TexTarget : uint8_t {
    BC,     // desktop / Steam Deck
    ASTC,   // iOS + modern Android
    ETC2,   // GLES 3.0 compatibility floor
};

// Resolved from COOK_TEX_TARGET. Unrecognised values fall back to BC and set
// `*why` — a silent fallback would ship a desktop-only build from a mobile
// build script and the failure would surface on a device, not in the log.
TexTarget   resolveTexTarget(std::string* why = nullptr);
const char* texTargetName(TexTarget t);

// The format this target uses for a given usage. Split out of the encoder so
// `TextureCooker::settingsFingerprint` and the cook log can name the format
// WITHOUT running an encode — and so the two can never disagree, which is how
// a fingerprint stops describing the thing it keys.
//
//   hq: the final-bake tier (COOK_TEX_HQ). Buys quality with encode time and,
//       on the mobile targets, with bitrate — ASTC 4x4 is 8 bpp against 6x6's
//       3.56, which is real memory on a device with a jetsam limit.
uint32_t texFormatFor(TexTarget target, bool isNormalMap, bool hasAlpha,
                      bool hq);

} // namespace cook
