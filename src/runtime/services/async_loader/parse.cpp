// ── AsyncLoader — COOKED READ (worker thread) ────────────────────────────────
// One of AsyncLoader's three TUs (loader.cpp queue/lifecycle, upload.cpp GPU
// handle creation). Everything here runs on the job pool: reading a cooked
// mesh and the cooked textures its materials name, decoding its skeleton and
// clips (ozz), and the gpu::copy staging memcpys (thread-safe — the backend's
// pool allocator wraps malloc). No GPU HANDLES are created here.
//
// COOKED CONTENT ONLY (WO-018). This file used to fall back to parsing the
// SOURCE with Assimp when nothing was cooked, and to find textures by
// searching the model's folder for files named like it ("_Albedo",
// "textures/"). That was a second parser beside the cook's, so an asset could
// look different depending on whether it had been cooked yet. Now an uncooked
// mesh, or an uncooked texture a cooked material names, is reported back as
// NotCooked, and loader.cpp turns that into a cook request (ICookRequests) or,
// in a build with no cooker, a failure.
#include "runtime/services/async_loader.h"
#include "runtime/services/texture_colour.h"
#include "core/logger.h"
#include "render/vertex.h"
#include "render/skinned_vertex.h"
#include "animation/cooked_skin.h"

#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// A cooked texture, staged for upload. Empty on a miss.
TextureGPUData stageTexture(const assetlib::TextureAsset& t, const std::string& name) {
    const gpu::ColourSpace cs = cookedColourSpace(t.header, name.c_str());
    // BEFORE staging. gpu::createTexture2D can refuse this format, and a
    // refusal there strands the staged payload for the life of the process
    // (render/gpu.h). On content cooked for the wrong target EVERY texture
    // refuses, so checking first is the difference between one skipped
    // texture and leaking the whole set.
    if (t.header.width == 0 || !gpu::textureFormatSupported(t.header.format, cs)) return {};
    TextureGPUData g;
    g.cs     = cs;
    g.mem    = gpu::copy(t.pixels.data(), (uint32_t)t.pixels.size());
    g.w      = (uint16_t)t.header.width;
    g.h      = (uint16_t)t.header.height;
    g.format = t.header.format;   // BC blocks upload as-is
    g.mips   = t.header.mipCount ? t.header.mipCount : 1;
    return g;
}

}  // namespace

// The registry record for a source path, keyed the way the registry keys it
// (project-relative, generic separators).
std::optional<assetlib::AssetRecord> AsyncLoader::recordFor(const std::string& path) const {
    if (!m_registry) return std::nullopt;
    const std::string relKey = !m_projectRoot.empty()
        ? std::filesystem::relative(std::filesystem::path(path), m_projectRoot).generic_string()
        : std::filesystem::path(path).filename().string();
    return m_registry->findBySourcePath(relKey);
}

// The cooked file for a source path, if the registry has one Ready on disk;
// empty otherwise. The one lookup behind processFile and hasCooked.
std::filesystem::path AsyncLoader::cookedPathFor(const std::string& path) const {
    auto rec = recordFor(path);
    if (!rec || rec->cookedPath.empty() || rec->state != assetlib::AssetState::Ready) return {};
    // cooked_path in DB is relative to .cache/ dir
    std::filesystem::path cookedAbs = m_projectRoot / ".cache" / rec->cookedPath;
    return std::filesystem::exists(cookedAbs) ? cookedAbs : std::filesystem::path{};
}

// -----------------------------------------------------------------------
// processFile — runs entirely on the worker.
// By the time a Loaded asset reaches the main thread, all data is staged;
// handle creation is the only main-thread work.
// -----------------------------------------------------------------------
LoadedAsset AsyncLoader::processFile(const std::string& path, const std::string& name,
                                     bool waitForTextures) {
    LoadedAsset out;
    out.path = path; out.name = name;

    const std::filesystem::path cookedAbs = cookedPathFor(path);
    if (cookedAbs.empty()) {
        // The cook either has not run for this source yet, or ran and failed.
        // Only the second is final.
        if (auto rec = recordFor(path); rec && rec->state == assetlib::AssetState::Failed) {
            out.outcome = LoadedAsset::Outcome::Failed;
            out.error   = "cook failed: " + path
                        + (rec->errorMessage.empty() ? "" : " — " + rec->errorMessage);
            return out;
        }
        out.outcome = LoadedAsset::Outcome::NotCooked;
        out.needsCook.push_back(path);
        out.error = "not cooked: " + path;
        return out;
    }

    assetlib::MeshAsset asset;
    if (!assetlib::loadMesh(asset, cookedAbs)) {
        out.outcome = LoadedAsset::Outcome::Failed;
        out.error   = "unreadable cooked mesh: " + cookedAbs.string();
        return out;
    }
    const auto& h = asset.header;
    const bool cookedSkinned = h.version >= 3 && h.boneCount > 0
                            && h.vertexStride == sizeof(SkinnedVertex);
    if (h.vertexStride != sizeof(Vertex) && !cookedSkinned) {
        // A stale cook (an older vertex layout). It used to fall through to
        // Assimp; a stale cook is the cooker's to redo, so it is a failure
        // here, and the message says which.
        out.outcome = LoadedAsset::Outcome::Failed;
        out.error   = "cooked vertex stride " + std::to_string(h.vertexStride)
                    + " does not match the runtime's " + std::to_string(sizeof(Vertex))
                    + " (re-cook): " + path;
        return out;
    }

    // ── Textures first, BEFORE any staging ────────────────────────────────
    // A cooked material names its textures either as a `.ctex` the cooker
    // extracted next to the mesh, or as a source path the registry cooks on
    // its own. Resolve every one now: a texture that is not cooked yet makes
    // the whole asset wait (when there is a cooker to wait for), so it never
    // appears untextured and then changes. Nothing is staged until we know.
    const std::filesystem::path srcDir = std::filesystem::path(path).parent_path();
    struct TexRef { std::filesystem::path cooked; std::string name; };
    auto resolveTex = [&](const char* p) -> TexRef {
        const std::string sp = p;
        if (sp.size() > 5 && sp.compare(sp.size() - 5, 5, ".ctex") == 0)
            return {cookedAbs.parent_path() / sp, sp};
        const std::string srcTex = (srcDir / sp).lexically_normal().string();
        const auto c = cookedPathFor(srcTex);
        if (c.empty()) {
            auto rec = recordFor(srcTex);
            if (!(rec && rec->state == assetlib::AssetState::Failed))
                out.needsCook.push_back(srcTex);
            else
                LOG_WARN("Loader", "%s: texture %s failed to cook — drawn without it",
                         name.c_str(), sp.c_str());
        }
        return {c, sp};
    };
    std::vector<TexRef> baseTex(asset.materials.size()), normTex(asset.materials.size());
    for (size_t m = 0; m < asset.materials.size(); ++m) {
        const auto& cm = asset.materials[m];
        if (cm.flags & assetlib::kMatFlag_HasBaseColor) baseTex[m] = resolveTex(cm.baseColorPath);
        if (cm.flags & assetlib::kMatFlag_HasNormalMap) normTex[m] = resolveTex(cm.normalMapPath);
    }
    if (!out.needsCook.empty() && waitForTextures) {
        out.outcome = LoadedAsset::Outcome::NotCooked;
        out.error   = "textures not cooked: " + path;
        return out;
    }
    for (const std::string& t : out.needsCook)   // not waiting: say what is missing
        LOG_WARN("Loader", "%s: texture not cooked, drawn without it: %s",
                 name.c_str(), t.c_str());
    out.needsCook.clear();

    MeshGPUData gd;
    gd.vertexMem   = gpu::copy(asset.vertexData.data(), (uint32_t)asset.vertexData.size());
    gd.indexMem    = gpu::copy(asset.indexData.data(),  (uint32_t)asset.indexData.size());
    gd.indexCount  = h.indexCount;
    gd.use32       = (h.indexStride == 4);
    gd.doubleSided = false;
    gd.skinned     = cookedSkinned;
    gd.hasBounds   = true;
    std::memcpy(gd.boundsMin, h.boundsMin, sizeof(gd.boundsMin));
    std::memcpy(gd.boundsMax, h.boundsMax, sizeof(gd.boundsMax));
    if (asset.submeshes.size() > 1) {
        for (const auto& sub : asset.submeshes) {
            SubRange sr;
            sr.indexOffset = sub.indexOffset;
            sr.indexCount  = sub.indexCount;
            sr.matIndex    = 0;   // MaterialCooker wires per-submesh mats
            gd.subRanges.push_back(sr);
        }
    }
    out.meshes.push_back(std::move(gd));

    // ── v3 skinned payload: skeleton + embedded clips ─────────────────────
    // (shared decode with AssetService's cooked streaming path —
    // animation/cooked_skin.h)
    if (cookedSkinned) {
        Skeleton skel = anim::decodeCookedSkeleton(asset);
        if (skel.ozz) {
            out.skeleton    = std::move(skel);
            out.hasSkeleton = true;
            out.animClips   = anim::decodeCookedClips(asset);
        } else {
            LOG_WARN("Loader", "%s: cooked skeleton blob unreadable — bind pose only",
                     name.c_str());
        }
    }

    auto loadTex = [&](const TexRef& r) -> TextureGPUData {
        if (r.cooked.empty()) return {};
        assetlib::TextureAsset t;
        if (!assetlib::loadTexture(t, r.cooked)) return {};
        return stageTexture(t, r.name);
    };
    for (size_t m = 0; m < asset.materials.size(); ++m) {
        const auto& cm = asset.materials[m];
        MaterialGPUData mg;
        std::memcpy(mg.baseColorFactor, cm.baseColorFactor, 16);
        mg.roughness = cm.roughness;
        mg.metallic  = cm.metallic;
        mg.baseColorTexture = loadTex(baseTex[m]);
        mg.normalMapTexture = loadTex(normTex[m]);
        mg.baseColorName = (cm.flags & assetlib::kMatFlag_HasBaseColor) ? cm.baseColorPath : "";
        mg.normalMapName = (cm.flags & assetlib::kMatFlag_HasNormalMap) ? cm.normalMapPath : "";
        out.materials.push_back(std::move(mg));
    }
    // A geometry-only cook has no material section: one default material, and
    // no longer a search of the source folder for something named like it.
    if (out.materials.empty()) out.materials.emplace_back();

    out.outcome = LoadedAsset::Outcome::Loaded;
    out.success = true;
    LOG_INFO("Loader", "%-30s verts=%u idx=%u%s", name.c_str(), h.vertexCount,
             h.indexCount, out.hasSkeleton ? " [skinned]" : "");
    return out;
}
