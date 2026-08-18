#pragma once
// ── texture_eac — the EAC half of ETC2 ───────────────────────────────────────
//
// bimg gives us ETC2 RGB (`ProcessRGB_ETC2`) and ASTC (astc-encoder), which is
// most of the mobile story. What it does NOT have is any EAC encoder, and EAC is
// what carries the two channels that matter most:
//
//   * ETC2A's alpha block — without it the ETC2 target cannot cook a cutout
//     leaf, a decal, or a UI atlas. Half the textures in a real game.
//   * EAC RG11 — the ETC2 answer to BC5. Normal maps encoded as RGB lose the
//     Z-reconstruction trick and pick up chroma artifacts exactly where a
//     normal map cannot afford them.
//
// So these two are written here. Both are small, well-specified codecs: a base
// codeword, a multiplier, one of 16 modifier tables, and sixteen 3-bit indices.
//
// ── HOW FAR THE TESTS GO, AND WHERE THEY STOP ────────────────────────────────
// `texture_eac_test` round-trips the ALPHA encoder through bimg's
// `decodeBlockEtc2Alpha` — an implementation nobody here wrote — so the bit
// layout, the table index, the multiplier nibble and the column-major index
// order are all pinned against a third party. That is real evidence.
//
// bimg has NO EAC R11 decoder, so RG11 is round-tripped through the decoder in
// this file instead. That proves the encoder's search is sane and proves NOTHING
// about whether the bit layout matches the hardware: a consistent pair of wrong
// functions passes. The layout is shared with the alpha block (same index
// packing, same nibbles, per the GLES 3.0 spec's EAC section) and that shared
// half IS covered above — but the 11-bit reconstruction `base*8 + 4 + mod*mul*8`
// is checked by nothing until this runs on a device. Treat it as unverified
// until the device lane exists.
#include <cstdint>

namespace cook {

// ── ETC2A alpha: one 8-byte EAC block from 16 alpha bytes ───────────────────
// `alpha` is 16 values in ROW-MAJOR 4x4 order (a[y*4+x]); the packing to
// column-major index order happens inside.
void encodeEacAlphaBlock(const uint8_t alpha[16], uint8_t out[8]);

// ── EAC R11: one 8-byte block from 16 8-bit samples ─────────────────────────
// Source is 8-bit (our decoded RGBA8), widened to the codec's 11-bit range.
// Two of these back to back are an EAC RG11 block: R first, then G.
void encodeEacR11Block(const uint8_t value[16], uint8_t out[8]);

// ── Decoders, for tests and for nothing else ────────────────────────────────
// A shipped runtime never decodes; the GPU does. These exist so the encoders
// can be held to an output, and are the ONLY check RG11 currently has.
void decodeEacAlphaBlock(const uint8_t in[8], uint8_t alpha[16]);
void decodeEacR11Block(const uint8_t in[8], uint16_t value[16]);

} // namespace cook
