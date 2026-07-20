#pragma once
// ── texture_encode — RGBA8 → GPU-ready block-compressed TextureAsset ────────
// The one encode path every cooker shares (standalone TextureCooker, the
// FBX embedded-texture flow, the glTF material flow). Offline it does ALL
// the heavy lifting: full mip chain (gamma-correct box filter for color,
// renormalized averaging for normal maps), then hardware block compression
// per mip — BC7 for color (near-lossless 4:1), BC5 for normal maps (two-
// channel XY, Z reconstructed in-shader, no BC1-style chroma artifacts).
// The runtime then streams the blocks straight to the GPU: header read,
// zero CPU decode.
#include <assetlib/texture_asset.h>
#include <cstdint>

namespace cook {

// Encode `rgba` (w*h*4) into `out` (header + packed mip blocks).
// isNormalMap selects BC5 + linear-space mips; otherwise BC7 + sRGB mips.
bool encodeTexture(const uint8_t* rgba, uint32_t w, uint32_t h,
                   bool isNormalMap, assetlib::TextureAsset& out);

// Filename heuristic for standalone texture files where usage is unknown
// ("_nor", "_normal", "_nrm", "_n_gl" …). Mesh cookers know the material
// slot and pass isNormalMap explicitly instead.
bool looksLikeNormalMap(const char* filename);

} // namespace cook
