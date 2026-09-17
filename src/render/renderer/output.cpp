// ── Renderer: the output pass (colour pipeline stages A and B) ───────────────
// See render/renderer/output_pass.h for why this exists and why the renderer,
// not the pipeline, owns it. THIS is the only translation unit that includes the
// output shaders' generated headers, for the reason shader_blobs.h gives: the
// arrays are `static`, and every additional includer carries its own copy.
#include "render/renderer/output_pass.h"

#include <atomic>

#include <bx/uint32_t.h>   // bx::halfFromFloat

#include "core/cube_lut.h"
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

// A parsed LUT -> a 3D RGBA16F texture. The texel order bgfx expects for a 3D
// upload is x fastest, then y, then z — which is exactly .cube's red-fastest
// order with r=x, g=y, b=z, so the data copies straight across and the shader
// samples it with uvw = (r, g, b).
static bgfx::TextureHandle uploadLut(const CubeLut& lut) {
    const uint32_t n = lut.size;
    const uint32_t count = n * n * n;
    const bgfx::Memory* mem = bgfx::alloc(count * 4 * sizeof(uint16_t));
    uint16_t* out = reinterpret_cast<uint16_t*>(mem->data);
    const uint16_t one = bx::halfFromFloat(1.0f);
    for (uint32_t i = 0; i < count; ++i) {
        out[i * 4 + 0] = bx::halfFromFloat(lut.rgb[i * 3 + 0]);
        out[i * 4 + 1] = bx::halfFromFloat(lut.rgb[i * 3 + 1]);
        out[i * 4 + 2] = bx::halfFromFloat(lut.rgb[i * 3 + 2]);
        out[i * 4 + 3] = one;
    }
    // Linear filtering (the default) is the trilinear interpolation the grade
    // needs; clamp keeps the edge cells from wrapping into each other.
    return bgfx::createTexture3D((uint16_t)n, (uint16_t)n, (uint16_t)n, false,
                                 bgfx::TextureFormat::RGBA16F,
                                 BGFX_SAMPLER_UVW_CLAMP, mem);
}

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
    sHdr      = bgfx::createUniform("s_hdr",      bgfx::UniformType::Sampler);
    sLut      = bgfx::createUniform("s_lut",      bgfx::UniformType::Sampler);
    uDisplay  = bgfx::createUniform("u_display",  bgfx::UniformType::Vec4);
    uLutMin   = bgfx::createUniform("u_lutMin",   bgfx::UniformType::Vec4);
    uLutScale = bgfx::createUniform("u_lutScale", bgfx::UniformType::Vec4);

    // ── Grading needs a sampleable 3D half-float texture ────────────────────
    lutSupported = (caps->supported & BGFX_CAPS_TEXTURE_3D) &&
                   (caps->formats[bgfx::TextureFormat::RGBA16F]
                    & BGFX_CAPS_FORMAT_TEXTURE_3D);
    if (lutSupported) {
        CubeLut identity;
        identity.size = 2;
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    identity.rgb.insert(identity.rgb.end(),
                                        { (float)r, (float)g, (float)b });
        identityLut = uploadLut(identity);
    } else {
        LOG_WARN("Renderer", "this backend has no RGBA16F 3D textures — grading "
                 "LUTs are disabled; exposure and tone mapping still apply");
    }

    if (!bgfx::isValid(program)) {
        LOG_ERROR("Renderer", "output program failed to build — the scene will "
                  "not reach the display");
        return false;
    }
    LOG_INFO("Renderer", "output pass: %s scene target, exposure + tone map + "
             "sRGB encode%s",
             hdrFormat == bgfx::TextureFormat::RGBA16F ? "RGBA16F" : "RGBA8",
             lutSupported ? " + grade" : "");
    return true;
}

void OutputPass::destroy() {
    for (auto& [key, entry] : luts)
        if (bgfx::isValid(entry.tex)) bgfx::destroy(entry.tex);
    luts.clear();
    auto kill = [](auto& h) { if (bgfx::isValid(h)) bgfx::destroy(h); h = BGFX_INVALID_HANDLE; };
    kill(identityLut);
    kill(program);
    kill(sHdr);
    kill(sLut);
    kill(uDisplay);
    kill(uLutMin);
    kill(uLutScale);
}

bgfx::TextureHandle OutputPass::lutTexture(const std::shared_ptr<const CubeLut>& lut) {
    if (!lut || lut->size < 2) return BGFX_INVALID_HANDLE;
    if (!lutSupported) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set())
            LOG_WARN("Renderer", "a grading LUT was requested but this backend "
                     "cannot sample it — rendering ungraded (reported once)");
        return BGFX_INVALID_HANDLE;
    }
    auto it = luts.find(lut.get());
    if (it == luts.end())
        it = luts.emplace(lut.get(), LutTexture{ lut, uploadLut(*lut) }).first;
    return it->second.tex;
}

void OutputPass::submit(bgfx::ViewId view, bgfx::TextureHandle hdr,
                        bgfx::FrameBufferHandle dst, uint16_t w, uint16_t h,
                        const DisplayTransform& display) {
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

    // ── Stage B: what this view's camera asked for ──────────────────────────
    const bgfx::TextureHandle graded = lutTexture(display.lut);
    const bool useLut = bgfx::isValid(graded);
    const float disp[4] = {
        display.exposure,
        (float)(uint8_t)display.toneMapper,
        useLut ? 1.0f : 0.0f,
        useLut ? (float)display.lut->size : 2.0f,
    };
    float lutMin[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };
    float lutScale[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
    if (useLut)
        for (int c = 0; c < 3; ++c) {
            lutMin[c]   = display.lut->domainMin[c];
            lutScale[c] = 1.0f / (display.lut->domainMax[c] - display.lut->domainMin[c]);
        }
    bgfx::setUniform(uDisplay,  disp);
    bgfx::setUniform(uLutMin,   lutMin);
    bgfx::setUniform(uLutScale, lutScale);

    bgfx::setVertexBuffer(0, &tvb);
    // Point + clamp: the HDR target is exactly the output's size, so filtering
    // could only blur, and clamping keeps the oversized triangle's out-of-range
    // UVs from wrapping.
    bgfx::setTexture(0, sHdr, hdr, BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
    const bgfx::TextureHandle lutTex = useLut ? graded : identityLut;
    if (bgfx::isValid(lutTex))
        bgfx::setTexture(1, sLut, lutTex, BGFX_SAMPLER_UVW_CLAMP);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(view, program);
}
