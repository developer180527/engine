// ── cook_common — see cook_common.h ───────────────────────────────────────────
// Moved verbatim from mesh_cooker.cpp (WO-011); only the signatures changed.
#include "assets/cookers/mesh/cook_common.h"
#include "assets/cookers/mesh/decimate.h"

#include <assetlib/ddc.h>                    // blake3Bytes — sibling dedup

#include <ozz/base/io/stream.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace meshcook {

// Drain an ozz output archive into a byte vector.
std::vector<uint8_t> drainOzzStream(ozz::io::MemoryStream& ms) {
    const int size = ms.Tell();
    std::vector<uint8_t> out((size_t)size);
    ms.Seek(0, ozz::io::Stream::kSet);
    ms.Read(out.data(), (size_t)size);
    return out;
}

// ── Sibling texture writing, with CONTENT DEDUP ─────────────────────────────
// Materials routinely share images: the fps_shooter pistol has several
// materials naming the same base-colour and normal maps, and the cooker used to
// encode and write each one to its OWN slot file. Measured cost: t0 and t2 were
// byte-identical (10.7 MB each) and t1 and t3 were byte-identical (21.3 MB
// each) — 32 MB duplicated on disk, in the shipped dist, AND in VRAM, for a
// three-mesh scene against a 128 MB target budget.
//
// Note the runtime cache CANNOT fix this: the duplicates have different
// filenames, so path identity does not see them. Content identity does, and the
// cheapest place to apply it is here — one write instead of two, which shrinks
// the cook, the DDC, the dist and the GPU upload at once.
//
// Keyed by the ENCODED bytes (post BC compression + mips), so two source images
// that compress identically also collapse.

// ── LOD levels ──────────────────────────────────────────────────────────────
// R20 shipped LOD selection that bought nothing measurable, because nothing
// could produce a cheaper mesh. This is that missing half: each level is
// decimated toward a triangle ratio and only KEPT if it is meaningfully
// cheaper than its parent — a level that is not cheaper costs memory and a
// swap for no benefit, which is exactly the state that was measured.
void appendLodLevels(assetlib::MeshAsset& asset) {
    // Ratios, not grid resolutions: a fixed grid reduces a dense mesh by 96%
    // and a low-poly prop by 0%, so it cannot define a level across mixed
    // content. See decimate.h for the measurements.
    static constexpr float kLevelRatios[] = { 0.40f, 0.15f, 0.05f };

    // Nothing to gain below this: the per-level vertex/index buffers and the
    // swap cost more than the triangles saved.
    static constexpr uint32_t kMinTrianglesForLod = 2000;

    asset.lods.clear();
    // Skinned meshes are excluded. Clustering carries whole vertices, so joints
    // and weights would survive — but the renderer does not expand skinned items
    // for LOD (R18), so a level would never be selected. Revisit together.
    if (asset.header.boneCount > 0) return;
    if (asset.header.indexCount / 3 < kMinTrianglesForLod) return;
    if (asset.header.indexStride != 2 && asset.header.indexStride != 4) return;

    // MOST PROPS USE 16-BIT INDICES — the first real content this ran on was
    // skipped entirely by a 32-bit-only path. Widen here rather than in the
    // decimator, which stays single-format: expand to 32-bit, decimate, and
    // narrow again below if the level still fits.
    std::vector<uint32_t> idx32;
    const uint32_t* indices = nullptr;
    if (asset.header.indexStride == 2) {
        const auto* src = reinterpret_cast<const uint16_t*>(asset.indexData.data());
        idx32.assign(src, src + asset.header.indexCount);
        indices = idx32.data();
    } else {
        indices = reinterpret_cast<const uint32_t*>(asset.indexData.data());
    }

    // Material groups go IN, so they come back out (mesh_asset.h LodLevel). A
    // level without them draws entirely with material[0], which made every
    // multi-material prop change colour at its first LOD threshold.
    std::vector<meshcook::SubRange> srcRanges;
    srcRanges.reserve(asset.submeshes.size());
    for (const auto& s : asset.submeshes)
        srcRanges.push_back({ s.indexOffset, s.indexCount, s.materialIndex });

    meshcook::DecimateInput in;
    in.vertices    = asset.vertexData.data();
    in.vertexCount = asset.header.vertexCount;
    in.stride      = asset.header.vertexStride;
    in.posOffset   = assetlib::vertexAttributeOffset(asset.header.vertexFlags,
                                                     assetlib::VF_POSITION);
    in.indices     = indices;
    in.indexCount  = asset.header.indexCount;
    in.ranges      = srcRanges.empty() ? nullptr : srcRanges.data();
    in.rangeCount  = (uint32_t)srcRanges.size();

    uint32_t parentTris = asset.header.indexCount / 3;
    for (float ratio : kLevelRatios) {
        const auto r = meshcook::decimateToRatio(in, ratio);
        if (!r.ok || r.triangles == 0) break;
        // Each level must beat its PARENT, not the original: once the search
        // stops making progress, further levels are pure cost.
        if ((float)r.triangles > meshcook::kMinReductionRatio * (float)parentTris)
            break;

        assetlib::MeshAsset::LodLevel lvl;
        lvl.vertexCount = r.vertexCount(in.stride);
        lvl.indexCount  = (uint32_t)r.indices.size();
        lvl.vertexData  = r.vertices;
        // Field by field: MeshSubmesh carries a reserved materialUUID between
        // indexCount and materialIndex, so brace-init would land the material
        // index in the UUID bytes.
        for (const auto& rr : r.ranges) {
            assetlib::MeshSubmesh ms;
            ms.indexOffset   = rr.indexOffset;
            ms.indexCount    = rr.indexCount;
            ms.materialIndex = rr.materialIndex;
            lvl.submeshes.push_back(ms);
        }
        // Levels inherit the parent's index width. A level always has FEWER
        // vertices than its parent, so a 16-bit parent's level always fits —
        // but it is checked rather than assumed.
        if (asset.header.indexStride == 2 && lvl.vertexCount <= 0xFFFFu) {
            lvl.indexData.resize(r.indices.size() * 2);
            auto* dst = reinterpret_cast<uint16_t*>(lvl.indexData.data());
            for (size_t k = 0; k < r.indices.size(); ++k)
                dst[k] = (uint16_t)r.indices[k];
        } else if (asset.header.indexStride == 2) {
            break;      // cannot narrow: stop the chain rather than mis-encode
        } else {
            lvl.indexData.resize(r.indices.size() * 4);
            std::memcpy(lvl.indexData.data(), r.indices.data(), lvl.indexData.size());
        }
        asset.lods.push_back(std::move(lvl));
        parentTris = r.triangles;
    }
    if (!asset.lods.empty()) {
        // RAISE, never assign: the skinned path already set 6, and an
        // unconditional `= 5` here would silently downgrade it and drop the
        // blob digests on any skinned mesh that also has LODs.
        if (asset.header.version < 5) asset.header.version = 5;
        std::printf("[MeshCooker]   lods: %u tris ->", asset.header.indexCount / 3);
        for (const auto& l : asset.lods) std::printf(" %u", l.indexCount / 3);
        std::printf("\n");
    }
}

std::string writeSiblingTexture(const assetlib::TextureAsset& tex,
                                const assetlib::CookContext& ctx, int slot,
                                uint32_t w, uint32_t h, bool isNormalMap,
                                const char* origin, SiblingDedup& g_siblingByContent) {
    // Hash the payload plus the header fields that change interpretation, so
    // two textures with identical blocks but different dimensions/format can
    // never collide.
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "%u|%u|%u|%u|", tex.header.width,
                  tex.header.height, tex.header.format, tex.header.mipCount);
    std::string blob(hdr);
    blob.append(reinterpret_cast<const char*>(tex.pixels.data()),
                tex.pixels.size());
    const std::string key = assetlib::blake3Bytes(blob.data(), blob.size());

    if (auto it = g_siblingByContent.find(key); it != g_siblingByContent.end()) {
        std::printf("[MeshCooker] %s texture -> %s (DEDUP: identical to an "
                    "earlier slot, %.1f MB saved)\n",
                    origin, it->second.c_str(),
                    (double)tex.pixels.size() / (1024.0 * 1024.0));
        return it->second;          // reference the existing sibling
    }

    char name[64];
    std::snprintf(name, sizeof(name), "%s_t%d.ctex",
                  ctx.outputPath.stem().string().c_str(), slot);
    const auto outPath = ctx.outputPath.parent_path() / name;
    if (!assetlib::saveTexture(tex, outPath)) return {};
    if (ctx.addOutput) ctx.addOutput(outPath);   // travels with the DDC record

    g_siblingByContent.emplace(key, name);
    std::printf("[MeshCooker] %s texture -> %s (%ux%u %s, %u mips, %.1f MB)\n",
                origin, name, w, h, isNormalMap ? "BC5" : "BC7",
                tex.header.mipCount,
                (double)tex.pixels.size() / (1024.0 * 1024.0));
    return name;
}
// Inverse-transpose of the upper 3x3 with the SAME scale-invariant
// singularity guard as cookNormalMatrix (|det| vs row-norm product — a
// bare epsilon collapses for small uniform scales; see the audit note).
void normalMatrix(const float m[16], float n[9]) {
    const float a=m[0],b=m[4],c=m[8], d=m[1],e=m[5],f=m[9], g=m[2],h=m[6],i=m[10];
    const float A=e*i-f*h, B=f*g-d*i, C=d*h-e*g;
    const float det  = a*A + b*B + c*C;
    const float r0   = std::sqrt(a*a + b*b + c*c);
    const float r1   = std::sqrt(d*d + e*e + f*f);
    const float r2   = std::sqrt(g*g + h*h + i*i);
    const float norm = r0 * r1 * r2;
    if (norm <= 0.0f || std::fabs(det) <= 1e-6f * norm) {
        n[0]=n[4]=n[8]=1.0f; n[1]=n[2]=n[3]=n[5]=n[6]=n[7]=0.0f;
        return;
    }
    const float s = 1.0f / det;
    n[0]=A*s;         n[1]=B*s;         n[2]=C*s;
    n[3]=(c*h-b*i)*s; n[4]=(a*i-c*g)*s; n[5]=(b*g-a*h)*s;
    n[6]=(b*f-c*e)*s; n[7]=(c*d-a*f)*s; n[8]=(a*e-b*d)*s;
}

}  // namespace meshcook
