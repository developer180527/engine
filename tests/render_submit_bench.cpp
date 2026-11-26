// ── render_submit_bench — what recording the colour pass costs, in numbers ────
//
// Not a pass/fail test. It times ForwardPipeline::render on bgfx's Noop backend,
// which executes nothing, so the number is the CPU cost of RECORDING draws (cull,
// sort, material binds, bgfx encoder calls) and none of the backend's. Build it
// optimised:
//
//   cmake --build build-rel --target render_submit_bench && build-rel/render_submit_bench
//
// Each line is the median of 31 frames. `draws` distinct meshes means no draw can
// instance, which is the case parallel recording exists for; 4096 is the pipeline's
// per-frame ceiling (kMaxDrawsPerFrame), so it is the worst a frame can be.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include "gpu_test_device.h"
#include "render/gpu_bgfx.h"

#include "core/jobs/jobs.h"
#include "render/asset_registry.h"
#include "render/forward_pipeline.h"
#include "render/material_registry.h"
#include "render/render_context.h"
#include "render/texture_registry.h"
#include "render/world/cull_stream.h"
#include "render/world/frustum.h"

namespace {

struct Scene {
    AssetRegistry    assets;
    TextureRegistry  textures;
    MaterialRegistry materials;
    bgfx::VertexLayout layout;
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh = BGFX_INVALID_HANDLE;
    bgfx::ViewId viewCursor = 5;
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::vector<RenderItem> items;
    rworld::CullStreamStore stream;

    Scene() {
        layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
        static const float verts[9] = { 0,0,0, 1,0,0, 0,1,0 };
        static const uint16_t idx[3] = { 0, 1, 2 };
        vbh = bgfx::createVertexBuffer(bgfx::makeRef(verts, sizeof(verts)), layout);
        ibh = bgfx::createIndexBuffer(bgfx::makeRef(idx, sizeof(idx)));
    }
    ~Scene() {
        for (auto& m : meshes) { m->vbh = {}; m->ibh = {}; }
        meshes.clear();
        bgfx::destroy(ibh);
        bgfx::destroy(vbh);
    }

    // `draws` items on a grid in front of the camera, each its own mesh, cycling
    // through `materials` materials so binds are realistic rather than one per frame.
    void build(uint32_t draws, uint32_t materialCount) {
        std::vector<MaterialHandle> mats;
        for (uint32_t i = 0; i < materialCount; ++i)
            mats.push_back(materials.addMaterial(Material{}));
        for (uint32_t i = 0; i < draws; ++i) {
            meshes.push_back(std::make_unique<Mesh>());
            Mesh& m = *meshes.back();
            const MaterialHandle mat = mats[i % materialCount];
            m.vbh = gpu::fromBgfx(vbh); m.ibh = gpu::fromBgfx(ibh); m.indexCount = 3;
            m.material = mat;
            m.boundsMin = { -0.5f, -0.5f, -0.5f };
            m.boundsMax = {  0.5f,  0.5f,  0.5f };
            RenderItem it;
            bx::mtxTranslate(it.model.m, (float)(i % 64) - 32.0f,
                             (float)(i / 64) - 32.0f, 0.0f);
            it.mesh = &m; it.material = mat;
            it.meshKey = i + 1; it.matKey = mat.id;
            it.hasBounds = true;
            it.boundsCenter = m.boundsCenter();
            it.boundsSize   = m.boundsSize();
            items.push_back(it);
        }
    }

    RenderView view() {
        RenderView rv;
        const bx::Vec3 eye{ 0, 0, 60 }, at{ 0, 0, 0 }, up{ 0, 1, 0 };
        bx::mtxLookAt(rv.view.m, eye, at, up);
        bx::mtxProj(rv.proj.m, 90.0f, 1.0f, 0.1f, 500.0f, bgfx::getCaps()->homogeneousDepth);
        rv.camPos = { eye.x, eye.y, eye.z, 1.0f };
        float vp[16];
        bx::mtxMul(vp, rv.view.m, rv.proj.m);
        rworld::extractFrustumPlanes(vp, rv.frustum);
        rv.target.fb = BGFX_INVALID_HANDLE;
        rv.target.w = 64; rv.target.h = 64;
        rv.baseViewId = 1;
        rv.ambient = 0.25f;
        rv.items = { items.data(), items.size() };
        rworld::fillCullStream(rv.items, stream);
        rv.cull = stream.view();
        return rv;
    }

    RenderContext context() {
        RenderContext rc{ assets, textures, materials };
        rc.viewCursor = &viewCursor;
        rc.shadowViewId = 0;
        return rc;
    }
};

double medianMs(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void measure(uint32_t draws, uint32_t materialCount) {
    Scene s;
    s.build(draws, materialCount);
    ForwardPipeline pipe;
    RenderContext rc = s.context();
    pipe.onAttach(rc);
    std::vector<double> ms;
    for (int f = 0; f < 36; ++f) {
        RenderView rv = s.view();
        *rc.viewCursor = 5;
        const auto t0 = std::chrono::steady_clock::now();
        pipe.render(rv, rc);
        const auto t1 = std::chrono::steady_clock::now();
        bgfx::frame();
        if (f >= 5) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const auto& st = pipe.submitStats();
    std::printf("draws %5u  materials %4u  submitted %5u  render %7.3f ms\n",
                draws, materialCount, st.draws, medianMs(ms));
    pipe.onDetach();
    bgfx::frame();
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    jobs::init();
    if (!initTestDevice()) return 1;
    std::printf("render_submit_bench: Noop backend, %u job workers\n", jobs::workerCount());
    for (uint32_t d : { 256u, 1024u, 4000u })
        for (uint32_t m : { 1u, 64u, 1024u })
            measure(d, m);
    shutdownTestDevice();
    jobs::shutdown();
    return 0;
}
