#include "render/shader/shader_library.h"
#include "core/logger.h"

namespace fs = std::filesystem;

ShaderLibrary::ShaderLibrary()
    : m_programs([](const bgfx::ProgramHandle& h) {
          // Destroying a program also releases the two shaders it was built
          // from — bgfx refcounts them, and createProgram(..., true) transferred
          // ownership at creation.
          if (bgfx::isValid(h)) bgfx::destroy(h);
      }) {}

ShaderLibrary::~ShaderLibrary() { shutdown(); }

rshader::RendererKind ShaderLibrary::currentRenderer() {
    switch (bgfx::getRendererType()) {
        case bgfx::RendererType::Metal:      return rshader::RendererKind::Metal;
        case bgfx::RendererType::Vulkan:     return rshader::RendererKind::Vulkan;
        case bgfx::RendererType::Direct3D11: return rshader::RendererKind::Direct3D11;
        case bgfx::RendererType::Direct3D12: return rshader::RendererKind::Direct3D12;
        case bgfx::RendererType::OpenGL:
        case bgfx::RendererType::OpenGLES:   return rshader::RendererKind::OpenGL;
        default:                             return rshader::RendererKind::Other;
    }
}

const assetlib::ShaderAsset* ShaderLibrary::load(const fs::path& cookedPath) {
    const std::string key = cookedPath.string();
    if (const auto it = m_assets.find(key); it != m_assets.end())
        return &it->second;

    assetlib::ShaderAsset sh;
    if (!assetlib::loadShader(sh, cookedPath)) {
        if (!m_reported[key]) {
            m_reported[key] = true;
            LOG_ERROR("ShaderLibrary", "cannot load cooked shader %s", key.c_str());
        }
        return nullptr;
    }
    LOG_INFO("ShaderLibrary", "loaded %s: %zu variant(s), %zu param(s), "
             "%zu sampler(s)", sh.name.c_str(), sh.variants.size(),
             sh.params.size(), sh.samplers.size());
    return &m_assets.emplace(key, std::move(sh)).first->second;
}

const assetlib::ShaderAsset* ShaderLibrary::interfaceOf(const fs::path& cookedPath) {
    return load(cookedPath);
}

bgfx::ProgramHandle ShaderLibrary::program(const fs::path& cookedPath,
                                           uint32_t featureMask) {
    const assetlib::ShaderAsset* sh = load(cookedPath);
    if (!sh) return BGFX_INVALID_HANDLE;

    const auto choice = rshader::selectVariant(*sh, featureMask, currentRenderer());
    if (!choice.ok()) {
        const std::string k = cookedPath.string() + "#sel";
        if (!m_reported[k]) {
            m_reported[k] = true;
            LOG_ERROR("ShaderLibrary", "%s", choice.error.c_str());
        }
        return BGFX_INVALID_HANDLE;
    }

    uint32_t profile = 0;
    rshader::profileForRenderer(currentRenderer(), profile);
    const std::string key = rshader::programKey(cookedPath.string(), featureMask,
                                                profile);

    bgfx::ProgramHandle prog = BGFX_INVALID_HANDLE;
    const bool ok = m_programs.acquire(
        key, sh->name + " [" + assetlib::profileName(profile) + "]",
        [&](bgfx::ProgramHandle& out, size_t& bytes) {
            const assetlib::ShaderVariant& v = *choice.variant;
            // bgfx::copy, not makeRef: makeRef requires the bytes to outlive
            // the frame, and m_assets is free to be evicted or the blob moved.
            const bgfx::Memory* vsm = bgfx::copy(sh->blob.data() + v.vsOffset,
                                                 v.vsSize);
            const bgfx::Memory* fsm = bgfx::copy(sh->blob.data() + v.fsOffset,
                                                 v.fsSize);
            bgfx::ShaderHandle vs = bgfx::createShader(vsm);
            bgfx::ShaderHandle fs = bgfx::createShader(fsm);
            if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
                // Leaving a valid half behind would leak it for the process
                // lifetime — createProgram never runs to take ownership.
                if (bgfx::isValid(vs)) bgfx::destroy(vs);
                if (bgfx::isValid(fs)) bgfx::destroy(fs);
                return false;
            }
            out   = bgfx::createProgram(vs, fs, /*destroyShaders*/ true);
            bytes = v.vsSize + v.fsSize;
            return bgfx::isValid(out);
        },
        prog);

    if (!ok) {
        const std::string k = cookedPath.string() + "#prog";
        if (!m_reported[k]) {
            m_reported[k] = true;
            LOG_ERROR("ShaderLibrary", "failed to create program for %s "
                      "(mask 0x%x, %s)", sh->name.c_str(), featureMask,
                      assetlib::profileName(profile));
        }
        return BGFX_INVALID_HANDLE;
    }
    return prog;
}

bgfx::UniformHandle ShaderLibrary::uniform(const std::string& name,
                                           bgfx::UniformType::Enum type,
                                           uint16_t num) {
    if (const auto it = m_uniforms.find(name); it != m_uniforms.end())
        return it->second;
    const bgfx::UniformHandle h = bgfx::createUniform(name.c_str(), type, num);
    m_uniforms.emplace(name, h);
    return h;
}

void ShaderLibrary::shutdown() {
    // Unreferenced entries first — the cache's destroyer does the bgfx::destroy
    // for those, so doing it by hand as well would be a double-destroy.
    m_programs.evictAllUnreferenced();

    // Whatever survives is still referenced, which at shutdown means a pipeline
    // forgot to release rather than that the program is in use. The cache
    // deliberately refuses to evict referenced entries (that would be a
    // use-after-free mid-frame), so destroy these directly and say so.
    for (const auto& r : m_programs.stillReferenced()) {
        LOG_WARN("ShaderLibrary", "program still referenced at shutdown: %s "
                 "(refs %u)", r.owner.c_str(), r.refs);
        if (bgfx::isValid(r.handle)) bgfx::destroy(r.handle);
    }
    for (auto& [name, h] : m_uniforms)
        if (bgfx::isValid(h)) bgfx::destroy(h);
    m_uniforms.clear();
    m_assets.clear();
    m_reported.clear();
}
