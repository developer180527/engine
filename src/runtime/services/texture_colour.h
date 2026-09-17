#pragma once
// ── texture_colour — the colour space a cooked texture is uploaded in ────────
//
// Colour pipeline stage A. There are FOUR places a cooked texture reaches the
// GPU: AssetService's synchronous load, its async stage, and the editor's async
// loader (a registry-resolved .ctex and a cooked mesh's sibling .ctex). If each
// derived the colour space itself, a streamed texture and a loaded one could end
// up lit differently — so every one of them asks this.
//
// v3 textures record sRGB (colour) or linear (data). Two cases fall back to
// linear, the pre-stage-A behaviour, and each is reported ONCE per process:
//   * legacy — cooked before v3. Both cookers' fingerprints name the format
//     version, so the next cook of the project fixes it.
//   * sRGB on a GPU with no sRGB variant of the format — colour renders too
//     bright, which is worth a line rather than a mystery (gpu:: prints it).
//
// Safe from a worker thread: gpu::textureFormatSupported is a pure caps query,
// and the once-flag is atomic. Defined in asset_service.cpp.
#include <assetlib/texture_asset.h>

#include "render/gpu.h"

gpu::ColourSpace cookedColourSpace(const assetlib::TextureHeader& header,
                                   const char* name);
