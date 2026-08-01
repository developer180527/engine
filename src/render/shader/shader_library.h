#pragma once
// ── ShaderLibrary — cooked .cshader in, bgfx programs out ───────────────────
//
// ONE concern: hand out the program (and the uniform handles) for a cooked
// shader variant, creating each at most once.
//
// This is what lets a project ship its own shading. `ForwardPipeline` currently
// #includes compiled-in byte arrays, so the only shaders that can ever run are
// the ones the engine was built with — the concrete reason `IRenderPipeline`
// isn't a usable customization point (docs/renderer-audit-and-plan.md R3).
//
// Programs are content-keyed through GpuResourceCache, the same machinery the
// textures use: two materials on the same variant share one program, refcounted
// and reported in the VRAM census rather than leaked into an untracked map.
//
// The decision-making half lives in shader_select.h and is GPU-free, so variant
// selection has tests. What remains here is bgfx calls, which do not.
#include "render/gpu_resource_cache.h"
#include "render/shader/shader_select.h"

#include <assetlib/shader_asset.h>
#include <bgfx/bgfx.h>

#include <filesystem>
#include <string>
#include <unordered_map>

class ShaderLibrary {
public:
    ShaderLibrary();
    ~ShaderLibrary();

    ShaderLibrary(const ShaderLibrary&)            = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    // Load (or reuse) a cooked shader and return the program for `featureMask`
    // on the live renderer. Returns an invalid handle on any failure, having
    // logged WHY — a renderer with no program draws nothing, and "nothing
    // rendered" with no message is the worst outcome in this whole subsystem.
    bgfx::ProgramHandle program(const std::filesystem::path& cookedPath,
                                uint32_t featureMask);

    // The cooked interface, for a material to bind against. Null when the
    // shader could not be loaded.
    const assetlib::ShaderAsset* interfaceOf(const std::filesystem::path& cookedPath);

    // Uniform handle by name, created once and shared. bgfx refcounts uniforms
    // internally, but it hands back a NEW handle each createUniform() call —
    // destroying one then destroys a uniform another material still uses, so
    // the dedup has to happen here.
    bgfx::UniformHandle uniform(const std::string& name,
                                bgfx::UniformType::Enum type, uint16_t num = 1);

    // Release every program and uniform. Called on renderer shutdown; safe to
    // call twice.
    void shutdown();

    gpucache::CacheStats stats() const { return m_programs.stats(); }

    // The live renderer as shader_select sees it. Exposed for diagnostics and
    // for the "cooked for the wrong backend" message.
    static rshader::RendererKind currentRenderer();

private:
    // Cached parse of a .cshader. Keyed by path: the bytecode blob is the bulk
    // of it and is only needed while creating programs, but the declared
    // interface is read per draw-setup, so it stays.
    std::unordered_map<std::string, assetlib::ShaderAsset> m_assets;

    gpucache::GpuResourceCache<bgfx::ProgramHandle> m_programs;
    std::unordered_map<std::string, bgfx::UniformHandle> m_uniforms;

    // Paths already reported as broken. Without this a shader that fails to
    // load logs once per draw, per frame — which buries the message it was
    // trying to deliver.
    std::unordered_map<std::string, bool> m_reported;

    const assetlib::ShaderAsset* load(const std::filesystem::path& cookedPath);
};
