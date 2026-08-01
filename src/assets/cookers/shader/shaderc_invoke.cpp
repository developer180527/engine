#include "assets/cookers/shader/shaderc_invoke.h"
#include <assetlib/shader_asset.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace shadercook {

namespace {

// A scratch path unique per process AND per call. Two variants of the same
// shader compile concurrently under the task scheduler, so a name derived only
// from the source would have them overwrite each other's output — producing
// bytecode for the wrong feature mask, silently.
fs::path scratchPath(const char* tag) {
    static std::atomic<uint64_t> counter{ 0 };
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "engine_shaderc_%d_%llu_%s",
                  (int)::getpid(), (unsigned long long)n, tag);
    return fs::temp_directory_path() / buf;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

bool readBinary(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0);
    out.resize((size_t)n);
    return (bool)f.read((char*)out.data(), n);
}

} // namespace

fs::path findShaderc() {
    std::error_code ec;
    if (const char* env = std::getenv("ENGINE_SHADERC")) {
        const fs::path p(env);
        if (fs::exists(p, ec)) return p;
    }
#ifdef ENGINE_SHADERC_PATH
    {
        const fs::path p(ENGINE_SHADERC_PATH);
        if (fs::exists(p, ec)) return p;
    }
#endif
    return {};
}

std::vector<fs::path> defaultIncludeDirs() {
    std::vector<fs::path> out;
    std::error_code ec;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        const fs::path p(s);
        if (fs::exists(p, ec)) out.push_back(p);
    };
    // ';' only, never ':' — a Windows path starts "C:\", so splitting on the
    // colon would shred every absolute path on that platform.
    auto split = [&](const char* list) {
        std::string cur;
        for (const char* c = list; ; ++c) {
            if (*c == ';' || *c == '\0') {
                add(cur); cur.clear();
                if (*c == '\0') break;
            } else {
                cur += *c;
            }
        }
    };
    if (const char* env = std::getenv("ENGINE_SHADER_INCLUDES")) split(env);
#ifdef ENGINE_SHADER_INCLUDE_LIST
    // Expands to a trailing-comma initializer list: "dir1","dir2",
    for (const char* d : { ENGINE_SHADER_INCLUDE_LIST }) add(d);
#endif
    return out;
}

CompileResult compileShader(const fs::path& shadercExe, const CompileRequest& req) {
    CompileResult res;

    std::error_code ec;
    if (shadercExe.empty() || !fs::exists(shadercExe, ec)) {
        res.error = "shaderc not found — set ENGINE_SHADERC or build bgfx::shaderc";
        return res;
    }
    if (!fs::exists(req.source, ec)) {
        res.error = "shader source not found: " + req.source.string();
        return res;
    }
    // Checked before spawning so the message names the host limitation rather
    // than surfacing as an opaque shaderc failure. A D3D profile on macOS is a
    // build-configuration mistake, not a broken shader.
    if (!assetlib::profileCookableOnThisHost(req.profile)) {
        res.error = std::string("profile '") + assetlib::profileName(req.profile)
                  + "' cannot be compiled on this host (needs a Windows"
                    " runner for D3D bytecode)";
        return res;
    }

    const fs::path outPath = scratchPath("out.bin");
    const fs::path logPath = scratchPath("log.txt");

    std::vector<std::string> args;
    args.push_back(shadercExe.string());
    args.push_back("-f");            args.push_back(req.source.string());
    args.push_back("-o");            args.push_back(outPath.string());
    args.push_back("--type");        args.push_back(
        req.stage == ShaderStage::Vertex ? "vertex" : "fragment");
    args.push_back("--platform");    args.push_back(
        assetlib::profileShadercPlatform(req.profile));
    args.push_back("--profile");     args.push_back(
        assetlib::profileShadercProfile(req.profile));
    if (!req.varyingDef.empty()) {
        args.push_back("--varyingdef"); args.push_back(req.varyingDef.string());
    }
    for (const auto& inc : req.includeDirs) {
        args.push_back("-i"); args.push_back(inc.string());
    }
    if (!req.defines.empty()) {
        // shaderc takes one semicolon-separated list, not repeated flags.
        std::string joined;
        for (size_t i = 0; i < req.defines.size(); ++i) {
            if (i) joined += ';';
            joined += req.defines[i];
        }
        args.push_back("--define"); args.push_back(joined);
    }
    args.push_back("-O"); args.push_back(std::to_string(req.optimize));

#if defined(_WIN32)
    // The Windows port is deferred (see the port notes); when it lands this
    // wants CreateProcess for the same reason POSIX uses posix_spawn —
    // argument quoting through a shell is a correctness hazard with paths
    // containing spaces.
    res.error = "shader compilation is not wired for Windows hosts yet";
    return res;
#else
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // Capture shaderc's own diagnostics. A shader that fails to compile must
    // report WHY — "cook failed" with no compiler message is useless to whoever
    // is writing the shader.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, logPath.string().c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, shadercExe.string().c_str(), &fa, nullptr,
                               argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        res.error = std::string("cannot spawn shaderc: ") + std::strerror(rc);
        fs::remove(logPath, ec);
        return res;
    }

    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, 0);
        if (r == pid) break;
        // EINTR interrupts the WAIT, not the child — retrying is mandatory, or
        // a benign signal reads as a compiler crash and leaks the process.
        if (r < 0 && errno == EINTR) continue;
        res.error = "waitpid failed on shaderc";
        fs::remove(logPath, ec);
        fs::remove(outPath, ec);   // every other error path clears both
        return res;
    }

    res.diagnostics = slurp(logPath);
    fs::remove(logPath, ec);

    if (!WIFEXITED(status)) {
        res.error = "shaderc terminated abnormally";
        fs::remove(outPath, ec);
        return res;
    }
    if (WEXITSTATUS(status) != 0) {
        res.error = "shaderc failed (exit " + std::to_string(WEXITSTATUS(status)) + ")";
        fs::remove(outPath, ec);
        return res;
    }
    if (!readBinary(outPath, res.bytecode)) {
        // Exit 0 with no usable output. Treated as a failure rather than an
        // empty success: an empty program would reach bgfx and fail there, far
        // from the cause.
        res.error = "shaderc reported success but produced no output";
        fs::remove(outPath, ec);
        return res;
    }
    fs::remove(outPath, ec);
    res.ok = true;
    return res;
#endif
}

} // namespace shadercook
