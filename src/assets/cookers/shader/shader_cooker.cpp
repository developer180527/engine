#include "assets/cookers/shader/shader_cooker.h"
#include "assets/cookers/shader/shader_manifest.h"
#include "assets/cookers/shader/shader_reflect.h"
#include "assets/cookers/shader/shader_verify.h"
#include "assets/cookers/shader/shaderc_invoke.h"
#include "core/logger.h"

#include <cstdlib>
#include <mutex>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace shadercook;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';' || c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

std::vector<uint32_t> ShaderCooker::resolveProfiles(std::string* whyEmpty) {
    std::vector<uint32_t> out;

    if (const char* env = std::getenv("COOK_SHADER_PROFILES")) {
        std::string unknown, uncookable;
        for (const auto& name : splitCsv(env)) {
            uint32_t p = 0;
            if (!assetlib::profileFromName(name, p)) {
                if (!unknown.empty()) unknown += ", ";
                unknown += name;
                continue;
            }
            // An explicitly requested profile the host cannot emit is dropped,
            // but LOUDLY. Silently omitting it produces a package that renders
            // nothing on that target — the failure mode this whole check
            // exists to prevent.
            if (!assetlib::profileCookableOnThisHost(p)) {
                if (!uncookable.empty()) uncookable += ", ";
                uncookable += name;
                continue;
            }
            out.push_back(p);
        }
        // Once per process. resolveProfiles() is also called from
        // settingsFingerprint(), which the pipeline evaluates repeatedly per
        // asset — without this the same warning buries the actual cook log.
        static std::once_flag warnOnce;
        std::call_once(warnOnce, [&] {
            if (!unknown.empty())
                LOG_WARN("ShaderCooker", "unknown profile(s) in "
                         "COOK_SHADER_PROFILES: %s", unknown.c_str());
            if (!uncookable.empty())
                LOG_WARN("ShaderCooker", "profile(s) %s cannot be compiled on "
                         "this host — that target needs its own cook runner",
                         uncookable.c_str());
        });
        if (out.empty() && whyEmpty)
            *whyEmpty = "COOK_SHADER_PROFILES named no profile this host can emit";
        return out;
    }

    for (uint32_t p = 0; p < assetlib::kProfileCount; ++p)
        if (assetlib::profileCookableOnThisHost(p)) out.push_back(p);
    if (out.empty() && whyEmpty)
        *whyEmpty = "this host can emit no shader profiles at all";
    return out;
}

std::string ShaderCooker::settingsFingerprint(const assetlib::CookContext&) const {
    std::string fp = "profiles=";
    for (uint32_t p : resolveProfiles()) { fp += assetlib::profileName(p); fp += ','; }

    // The compiler is part of the recipe. Two shaderc builds can emit different
    // bytecode from identical source, so keying on source alone would let a
    // stale blob survive a bgfx upgrade — and the symptom would be a shader
    // that fails to load on some backends only.
    const fs::path exe = findShaderc();
    std::error_code ec;
    fp += ";shaderc=";
    fp += exe.empty() ? "none" : exe.filename().string();
    if (!exe.empty()) {
        const auto t = fs::last_write_time(exe, ec);
        if (!ec) fp += "@" + std::to_string((long long)t.time_since_epoch().count());
    }
    return fp;
}

size_t ShaderCooker::estimatePeakBytes(const assetlib::CookContext&) const {
    // Compiles run in child processes, so the cooker's own heap stays small;
    // what the budget must account for is the child's peak. glslang +
    // SPIRV-Cross on one shader is tens of MB, and variants run sequentially
    // within a cook, so one child's worth is the right figure.
    return (size_t)192 << 20;
}

assetlib::CookResult ShaderCooker::cook(const assetlib::CookContext& ctx) {
    assetlib::CookResult res;

    // ── 1. manifest ─────────────────────────────────────────────────────────
    const std::string text = slurp(ctx.sourcePath);
    if (text.empty()) {
        res.error = "cannot read " + ctx.sourcePath.string();
        return res;
    }
    ShaderManifest man;
    const auto parsed = parseShaderManifest(text, ctx.sourcePath.parent_path(), man);
    if (!parsed.ok) {
        res.error = "invalid .shader: " + parsed.error;
        return res;
    }

    std::error_code ec;
    for (const auto* p : { &man.vertexPath, &man.fragmentPath }) {
        if (!fs::exists(*p, ec)) {
            res.error = "shader source not found: " + p->string();
            return res;
        }
    }

    // ── 2. what to target ───────────────────────────────────────────────────
    std::string whyEmpty;
    const auto profiles = resolveProfiles(&whyEmpty);
    if (profiles.empty()) {
        res.error = "no shader profiles to cook — " + whyEmpty;
        return res;
    }
    const fs::path shadercExe = findShaderc();
    if (shadercExe.empty()) {
        res.error = "shaderc not found — set ENGINE_SHADERC or build bgfx::shaderc";
        return res;
    }

    // ── 3. compile the matrix ───────────────────────────────────────────────
    assetlib::ShaderAsset out;
    out.name     = man.name;
    out.params   = man.params;
    out.samplers = man.samplers;
    for (const auto& f : man.features) out.features.push_back(f.name);

    // bgfx's own headers first (every .sc includes bgfx_shader.sh), then the
    // shader's own directory so a project can include its own .sh files.
    std::vector<fs::path> includes = defaultIncludeDirs();
    includes.push_back(man.vertexPath.parent_path());
    if (ctx.sourcePath.parent_path() != man.vertexPath.parent_path())
        includes.push_back(ctx.sourcePath.parent_path());

    const uint32_t variants = man.variantCount();
    bool verified = false;

    for (uint32_t profile : profiles) {
        for (uint32_t mask = 0; mask < variants; ++mask) {
            CompileRequest req;
            req.varyingDef  = man.varyingPath;
            req.includeDirs = includes;
            req.defines     = man.definesFor(mask);
            req.profile     = profile;

            req.source = man.vertexPath;
            req.stage  = ShaderStage::Vertex;
            const CompileResult vs = compileShader(shadercExe, req);
            if (!vs.ok) {
                res.error = man.name + " [" + assetlib::profileName(profile)
                          + " mask " + std::to_string(mask) + "] vertex: "
                          + vs.error
                          + (vs.diagnostics.empty() ? "" : "\n" + vs.diagnostics);
                return res;
            }

            req.source = man.fragmentPath;
            req.stage  = ShaderStage::Fragment;
            const CompileResult fsr = compileShader(shadercExe, req);
            if (!fsr.ok) {
                res.error = man.name + " [" + assetlib::profileName(profile)
                          + " mask " + std::to_string(mask) + "] fragment: "
                          + fsr.error
                          + (fsr.diagnostics.empty() ? "" : "\n" + fsr.diagnostics);
                return res;
            }

            // ── 4. verify the declaration against the real uniform table ────
            // Once per shader, not once per variant. The interface is a
            // property of the shader; running it on every variant would report
            // the same mismatch 2^n × profiles times. The FIRST variant is the
            // baseline — feature defines can only add uniforms, and an added
            // uniform is a warning, not an error.
            if (!verified) {
                const Reflection rv = reflectShaderBinary(vs.bytecode.data(),
                                                          vs.bytecode.size());
                const Reflection rf = reflectShaderBinary(fsr.bytecode.data(),
                                                          fsr.bytecode.size());
                const VerifyResult v = verifyInterface(man.params, man.samplers,
                                                       rv, rf);
                for (const auto& w : v.warnings)
                    LOG_INFO("ShaderCooker", "%s: %s", man.name.c_str(), w.c_str());
                if (!v.ok) {
                    res.error = "declared interface does not match the compiled "
                                "shader:\n  " + v.joined();
                    return res;
                }
                verified = true;
            }

            assetlib::ShaderVariant var;
            var.featureMask = mask;
            var.profile     = profile;
            var.vsOffset    = (uint32_t)out.blob.size();
            var.vsSize      = (uint32_t)vs.bytecode.size();
            out.blob.insert(out.blob.end(), vs.bytecode.begin(), vs.bytecode.end());
            var.fsOffset    = (uint32_t)out.blob.size();
            var.fsSize      = (uint32_t)fsr.bytecode.size();
            out.blob.insert(out.blob.end(), fsr.bytecode.begin(), fsr.bytecode.end());
            out.variants.push_back(var);
        }
    }

    // The .sc sources are inputs, but they are not registry assets, so they
    // cannot be reported through addDependency (which takes a UUID). Editing a
    // .sc therefore does NOT currently invalidate the cooked shader — see the
    // known limitation in src/assets/cookers/shader/info.md.

    if (!assetlib::saveShader(out, ctx.outputPath)) {
        res.error = "failed to write " + ctx.outputPath.string();
        return res;
    }

    std::string profileList;
    for (uint32_t p : profiles) {
        if (!profileList.empty()) profileList += ',';
        profileList += assetlib::profileName(p);
    }
    LOG_INFO("ShaderCooker", "%s — %u variant(s) x %zu profile(s) [%s], %.1f KB",
             man.name.c_str(), variants, profiles.size(), profileList.c_str(),
             (double)out.blob.size() / 1024.0);

    res.success = true;
    return res;
}
