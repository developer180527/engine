#pragma once
// ── Verification — does the declared interface match the compiled shader? ───
//
// ONE concern: compare what the `.shader` CLAIMS against what reflection found
// in the bytecode, and produce errors an author can act on.
//
// This is the whole reason the interface is trustworthy. Three failures it
// catches, all of which are otherwise invisible until someone notices the
// render looks wrong:
//
//   • a parameter naming a uniform the shader doesn't have (typo, or the
//     uniform got renamed) — the value writes nowhere;
//   • a parameter whose float offset lies outside the uniform's registers —
//     the write lands in adjacent uniform memory;
//   • a sampler declared against a non-sampler uniform, or vice versa.
//
// Kept apart from reflection (which parses) and from the cooker (which
// orchestrates) so the rules can be tested against hand-built uniform tables
// with no compiler on the machine.
#include "assets/cookers/shader/shader_reflect.h"
#include <assetlib/shader_asset.h>

#include <string>
#include <vector>

namespace shadercook {

struct VerifyResult {
    bool ok = false;
    std::vector<std::string> errors;     // every mismatch, not just the first
    std::vector<std::string> warnings;   // shader has it, declaration doesn't

    std::string joined(const char* sep = "\n  ") const;
};

// `vs` / `fs` are the reflections of the two stages of ONE variant. Uniforms
// are matched against the union: a parameter may legitimately live in either
// stage (u_params is fragment-side, u_shadowMtx vertex-side), and demanding
// both would reject correct shaders.
VerifyResult verifyInterface(const std::vector<assetlib::ShaderParam>& params,
                             const std::vector<assetlib::ShaderSampler>& samplers,
                             const Reflection& vs, const Reflection& fs);

} // namespace shadercook
