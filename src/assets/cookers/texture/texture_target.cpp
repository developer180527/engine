#include "assets/cookers/texture/texture_target.h"

#include <assetlib/texture_asset.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cook {

TexTarget resolveTexTarget(std::string* why) {
    const char* env = std::getenv("COOK_TEX_TARGET");
    if (!env || !*env) return TexTarget::BC;

    std::string s(env);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "bc" || s == "desktop" || s == "dxt")   return TexTarget::BC;
    if (s == "astc" || s == "ios" || s == "mobile")  return TexTarget::ASTC;
    if (s == "etc2" || s == "gles3")                 return TexTarget::ETC2;

    if (why)
        *why = "COOK_TEX_TARGET='" + s + "' is not one of bc|astc|etc2 — "
               "cooking for bc (desktop). A mobile build script that lands here "
               "ships blocks no phone can decode.";
    return TexTarget::BC;
}

const char* texTargetName(TexTarget t) {
    switch (t) {
        case TexTarget::BC:   return "bc";
        case TexTarget::ASTC: return "astc";
        case TexTarget::ETC2: return "etc2";
    }
    return "?";
}

uint32_t texFormatFor(TexTarget target, bool isNormalMap, bool hasAlpha,
                      bool hq) {
    switch (target) {
        case TexTarget::BC:
            if (isNormalMap) return assetlib::kTexBC5;
            if (hq)          return assetlib::kTexBC7;
            return hasAlpha ? assetlib::kTexBC3 : assetlib::kTexBC1;

        case TexTarget::ASTC:
            // 4x4 for normals ALWAYS, not just on the HQ tier. A normal map is
            // the one texture where compression error becomes visible geometry
            // — wobbling highlights on a flat wall — and 6x6 on a normal map is
            // where mobile ports get the reputation they have.
            if (isNormalMap) return assetlib::kTexASTC4x4;
            // Alpha needs no separate format: ASTC carries 4 channels in every
            // block size, which is one of the real advantages over BC's
            // BC1/BC3 split.
            return hq ? assetlib::kTexASTC4x4 : assetlib::kTexASTC6x6;

        case TexTarget::ETC2:
            if (isNormalMap) return assetlib::kTexEACRG11;
            return hasAlpha ? assetlib::kTexETC2A : assetlib::kTexETC2;
    }
    return assetlib::kTexBC1;
}

} // namespace cook
