// ── AsyncLoader — GPU UPLOAD (main thread only) ──────────────────────────────
// One of AsyncLoader's three TUs (loader.cpp queue/lifecycle, parse.cpp CPU
// import). drainOne() is the ONLY place this subsystem creates GPU handles:
// all data was pre-copied by the worker, so handle creation is
// O(microseconds) command submission — no memcpy, no stall.
#include "runtime/services/async_loader.h"
#include "runtime/services/async_loader/loader_internal.h"
#include "core/logger.h"
#include "render/mesh.h"
#include "render/vertex.h"
#include "render/skinned_vertex.h"
#include "render/texture.h"
#include "render/material.h"

#include <bgfx/bgfx.h>

#include <cstring>

using asyncldr::normalizeKey;

bool AsyncLoader::drainOne(AssetStorage& storage) {
    UploadRequest req;
    {
        std::lock_guard<std::mutex> lk(m_readyMtx);
        if (m_ready.empty()) return false;
        req = std::move(m_ready.front());
        m_ready.pop();
    }

    const std::string key = normalizeKey(req.asset.path);

    if (!req.asset.success) {
        // Erase from inFlight so the path can be retried or waited on cleanly
        { std::lock_guard<std::mutex> lk(m_pendingMtx);
          m_inFlight.erase(key); }
        LOG_ERROR("Loader", "Upload skipped (parse failed): %s",
                  req.asset.name.c_str());
        AsyncLoadResult failResult{};
        if (req.cb) req.cb(failResult, req.asset.name);
        // Drain waiters with invalid result so they don't block forever
        std::vector<OnLoaded> failWaiters;
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            auto it = m_waiters.find(key);
            if (it != m_waiters.end()) {
                failWaiters = std::move(it->second);
                m_waiters.erase(it);
            }
        }
        for (auto& w : failWaiters) if (w) w(failResult, req.asset.name);
        return true;
    }

    // Upload materials (handle creation only — data already in bgfx pool)
    std::vector<MaterialHandle> matHandles(req.asset.materials.size());
    for (size_t i = 0; i < req.asset.materials.size(); ++i) {
        const MaterialGPUData& mg = req.asset.materials[i];
        Material mat;
        std::memcpy(mat.baseColorFactor, mg.baseColorFactor, 16);
        if (mg.baseColorTexture.mem) {
            bgfx::TextureHandle th = bgfx::createTexture2D(
                mg.baseColorTexture.w, mg.baseColorTexture.h,
                false, 1, bgfx::TextureFormat::RGBA8,
                0, mg.baseColorTexture.mem); // mem already copied — instant
            if (bgfx::isValid(th)) {
                Texture tex; tex.handle = th;
                mat.baseColorTexture = storage.textures.addTexture(std::move(tex));
            }
        }
        if (mg.normalMapTexture.mem) {
            bgfx::TextureHandle th = bgfx::createTexture2D(
                mg.normalMapTexture.w, mg.normalMapTexture.h,
                false, 1, bgfx::TextureFormat::RGBA8,
                0, mg.normalMapTexture.mem);
            if (bgfx::isValid(th)) {
                Texture tex; tex.handle = th;
                mat.normalMapTexture = storage.textures.addTexture(std::move(tex));
            }
        }
        mat.baseColorName = mg.baseColorName;
        mat.normalMapName = mg.normalMapName;
        matHandles[i] = storage.materials.addMaterial(std::move(mat));
    }

    // Upload meshes (handle creation only — data already in bgfx pool)
    MeshHandle firstHandle{};
    for (const MeshGPUData& mg : req.asset.meshes) {
        if (!mg.vertexMem || !mg.indexMem) {
            LOG_ERROR("Loader", "Skipping mesh with null GPU memory: %s",
                      req.asset.name.c_str());
            continue;
        }
        // Select vertex layout based on whether this mesh has bone data
        bgfx::VertexBufferHandle vbh = mg.skinned
            ? bgfx::createVertexBuffer(mg.vertexMem, SkinnedVertex::layout())
            : bgfx::createVertexBuffer(mg.vertexMem, Vertex::layout());

        bgfx::IndexBufferHandle ibh = mg.use32
            ? bgfx::createIndexBuffer(mg.indexMem, BGFX_BUFFER_INDEX32)
            : bgfx::createIndexBuffer(mg.indexMem); // instant — no memcpy

        Mesh mesh(vbh, ibh, mg.indexCount);
        mesh.doubleSided = mg.doubleSided;
        if (mg.hasBounds) {
            mesh.boundsMin = {mg.boundsMin[0], mg.boundsMin[1], mg.boundsMin[2]};
            mesh.boundsMax = {mg.boundsMax[0], mg.boundsMax[1], mg.boundsMax[2]};
        }
        if (mg.matIndex < matHandles.size())
            mesh.material = matHandles[mg.matIndex];

        mesh.sourcePath = req.asset.path;
        // Wire submesh ranges for multi-draw rendering
        for (const auto& sr : mg.subRanges) {
            SubmeshRange range;
            range.indexOffset = sr.indexOffset;
            range.indexCount  = sr.indexCount;
            range.material    = sr.matIndex < matHandles.size()
                                ? matHandles[sr.matIndex] : mesh.material;
            mesh.submeshes.push_back(range);
        }
        MeshHandle h = storage.meshes.addMesh(std::move(mesh));
        if (!firstHandle.valid()) firstHandle = h;
    }

    // ── Register skeleton + clips (main-thread registry operations) ───────
    AsyncLoadResult result;
    result.mesh = firstHandle;

    if (req.asset.hasSkeleton && storage.skeletons) {
        result.skeleton = storage.skeletons->add(
            Skeleton(req.asset.skeleton));   // copy into registry
        for (auto& clip : req.asset.animClips) {
            if (storage.clips)
                result.clips.push_back(storage.clips->add(std::move(clip)));
        }
    }

    LOG_SUCCESS("Loader", "Uploaded: %s (handle %u)%s",
                req.asset.name.c_str(), firstHandle.id,
                result.skeleton.valid() ? " [skinned]" : "");

    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        m_loadedResults[key] = result;
    }

    if (req.cb) req.cb(result, req.asset.name);
    // Drain any callbacks that queued while this path was in-flight
    std::vector<OnLoaded> waiters;
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        auto it = m_waiters.find(key);
        if (it != m_waiters.end()) {
            waiters = std::move(it->second);
            m_waiters.erase(it);
        }
        m_inFlight.erase(key);                          // clear in-flight on success
    }
    for (auto& w : waiters)
        if (w) w(result, req.asset.name);
    return true;
}
