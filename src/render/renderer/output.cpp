// ── Renderer: the output pass (colour pipeline stage A) ──────────────────────
// See render/renderer/output_pass.h for why this exists and why the renderer,
// not the pipeline, owns it. THIS is the only translation unit that includes the
// output shaders' generated headers, for the reason shader_blobs.h gives: the
// arrays are `static`, and every additional includer carries its own copy.
#include "render/renderer/output_pass.h"

#include <atomic>

#include "core/logger.h"

#if defined(__APPLE__)
    #include "metal/vs_output.sc.bin.h"
    #include "metal/fs_output.sc.bin.h"
    #define VS_OUTPUT_DATA vs_output_mtl
    #define FS_OUTPUT_DATA fs_output_mtl
#elif defined(_WIN32)
    #include "dxbc/vs_output.sc.bin.h"
    #include "dxbc/fs_output.sc.bin.h"
    #define VS_OUTPUT_DATA vs_output_dxbc
    #define FS_OUTPUT_DATA fs_output_dxbc
#else // Linux — Vulkan (SPIR-V)
    #include "spirv/vs_output.sc.bin.h"
    #include "spirv/fs_output.sc.bin.h"
    #define VS_OUTPUT_DATA vs_output_spv
    #define FS_OUTPUT_DATA fs_output_spv
#endif

bool OutputPass::create() {
    // ── The HDR format: float where the backend can render to it ────────────
    const bgfx::Caps* caps = bgfx::getCaps();
    if (caps->formats[bgfx::TextureFormat::RGBA16F]
        & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) {
        hdrFormat = bgfx::TextureFormat::RGBA16F;
    } else {
        hdrFormat = bgfx::TextureFormat::RGBA8;
        LOG_WARN("Renderer", "RGBA16F is not renderable on this backend — the "
                 "scene target falls back to RGBA8: lighting stays linear, but "
                 "highlights clip at 1.0 before the output encode and dark "
                 "gradients band");
    }

    layout.begin()
        .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    program = bgfx::createProgram(
        bgfx::createShader(bgfx::makeRef(VS_OUTPUT_DATA, sizeof(VS_OUTPUT_DATA))),
        bgfx::createShader(bgfx::makeRef(FS_OUTPUT_DATA, sizeof(FS_OUTPUT_DATA))),
        true);
    sHdr = bgfx::createUniform("s_hdr", bgfx::UniformType::Sampler);

    if (!bgfx::isValid(program)) {
        LOG_ERROR("Renderer", "output program failed to build — the scene will "
                  "not reach the display");
        return false;
    }
    LOG_INFO("Renderer", "output pass: %s scene target, sRGB encode",
             hdrFormat == bgfx::TextureFormat::RGBA16F ? "RGBA16F" : "RGBA8");
    return true;
}

void OutputPass::destroy() {
    if (bgfx::isValid(program)) bgfx::destroy(program);
    if (bgfx::isValid(sHdr))    bgfx::destroy(sHdr);
    program = BGFX_INVALID_HANDLE;
    sHdr    = BGFX_INVALID_HANDLE;
}

void OutputPass::submit(bgfx::ViewId view, bgfx::TextureHandle hdr,
                        bgfx::FrameBufferHandle dst, uint16_t w, uint16_t h) {
    if (!bgfx::isValid(program) || !bgfx::isValid(hdr) || w == 0 || h == 0)
        return;

    bgfx::setViewName(view, "Output");
    bgfx::setViewFrameBuffer(view, dst);
    bgfx::setViewRect(view, 0, 0, w, h);
    bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(view, nullptr, nullptr);

    // Latched, like every per-frame renderer log site: a transient-buffer
    // shortage recurs every frame, and one line says what a thousand would.
    if (bgfx::getAvailTransientVertexBuffer(3, layout) < 3) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set())
            LOG_WARN("Renderer", "output pass skipped: transient vertex buffer "
                     "exhausted (reported once)");
        return;
    }

    // ── One oversized triangle covering the viewport ────────────────────────
    // Clip space in, no transform. The UV's v axis depends on where the backend
    // puts texture row 0: top on Metal/D3D/Vulkan, bottom on OpenGL. Getting it
    // wrong renders the scene upside down, so it is derived from the caps rather
    // than assumed.
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    struct V { float x, y, z, u, v; };
    auto vert = [bottomLeft](float x, float y) {
        return V{ x, y, 0.0f, x * 0.5f + 0.5f,
                  bottomLeft ? y * 0.5f + 0.5f : 0.5f - y * 0.5f };
    };
    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, 3, layout);
    V* out = reinterpret_cast<V*>(tvb.data);
    out[0] = vert(-1.0f, -1.0f);
    out[1] = vert( 3.0f, -1.0f);
    out[2] = vert(-1.0f,  3.0f);

    bgfx::setVertexBuffer(0, &tvb);
    // Point + clamp: the HDR target is exactly the output's size, so filtering
    // could only blur, and clamping keeps the oversized triangle's out-of-range
    // UVs from wrapping.
    bgfx::setTexture(0, sHdr, hdr, BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(view, program);
}
