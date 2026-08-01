#pragma once
// ── Variant selection — which cooked bytecode does THIS machine want? ───────
//
// ONE concern: given a cooked shader and the live renderer, pick the variant.
// No bgfx calls, no GPU, no file I/O — so the part of shader loading that has
// real decisions in it can be tested, which the rest of shader loading cannot.
//
// Two ways this fails, and both are silent without a check:
//   • the cook targeted profiles this machine's renderer doesn't speak (a
//     macOS-cooked package on a D3D11 machine). The program simply never
//     builds, and a renderer with no program draws nothing.
//   • the material asked for a feature combination that was never cooked. The
//     nearest cooked variant is WRONG, not approximate — a skinned mesh drawn
//     with the unskinned program renders as a folded heap at the origin.
#include <assetlib/shader_asset.h>

#include <cstdint>
#include <string>

namespace rshader {

// bgfx::RendererType::Enum, mirrored so this header stays GPU-free. Values are
// NOT bgfx's — only the mapping function below knows those.
enum class RendererKind {
    Metal, Vulkan, Direct3D11, Direct3D12, OpenGL, Other,
};

// Which cooked profile a renderer needs. Returns false for renderers no cooked
// profile serves (Noop, WebGPU, ...), which is a legitimate state for a
// headless run and must not be confused with "cook was wrong".
bool profileForRenderer(RendererKind kind, uint32_t& outProfile);

struct VariantChoice {
    const assetlib::ShaderVariant* variant = nullptr;
    std::string error;                       // set when variant == nullptr
    bool ok() const { return variant != nullptr; }
};

// Exact match on (featureMask, profile). Deliberately NOT a nearest match:
// falling back to a different feature set renders confidently wrong output,
// which costs far more to debug than a hard failure at load.
VariantChoice selectVariant(const assetlib::ShaderAsset& sh,
                            uint32_t featureMask, RendererKind kind);

// Cache identity for one program. Content-keyed like every other GPU resource,
// so the same variant requested by two materials is created once.
std::string programKey(const std::string& cookedPath, uint32_t featureMask,
                       uint32_t profile);

} // namespace rshader
