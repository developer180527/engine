#include "assets/cookers/shader/shaderc_invoke.h"
#include <assetlib/shader_asset.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#  include <process.h>   // _getpid
#else
#  include <unistd.h>    // getpid
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

namespace {
// UTF-8 -> UTF-16 for the wide CreateProcess/CreateFile APIs. The narrow
// variants mangle non-ASCII install paths, which is how a project under a
// localised user folder stops cooking for reasons nobody connects to shaders.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                        nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Quote one argument the way CommandLineToArgvW parses it back: backslashes are
// literal EXCEPT immediately before a quote, where each must be doubled. Same
// rules as modules/assetlib/src/cook/worker_win32.cpp — naive concatenation
// silently splits any path containing a space.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd.push_back(L'"');
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++slashes; ++i; }
        if (i == arg.size()) { cmd.append(slashes * 2, L'\\'); break; }
        if (arg[i] == L'"') {
            cmd.append(slashes * 2 + 1, L'\\');
            cmd.push_back(L'"');
        } else {
            cmd.append(slashes, L'\\');
            cmd.push_back(arg[i]);
        }
    }
    cmd.push_back(L'"');
}
} // namespace
#endif

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

// ── The process id, which is not spelled the same everywhere ────────────────
// POSIX has getpid() in <unistd.h>; Windows has _getpid() in <process.h> and no
// <unistd.h> at all, so `::getpid()` is "error C2039: 'getpid': is not a member
// of '`global namespace''" on MSVC. Wrapped in one function rather than an ifdef
// at the call site: the CALLER wants "something unique to this process", and that
// is a capability, not a platform question.
int currentProcessId() {
#if defined(_WIN32)
    return (int)::_getpid();
#else
    return (int)::getpid();
#endif
}

// A scratch path unique per process AND per call. Two variants of the same
// shader compile concurrently under the task scheduler, so a name derived only
// from the source would have them overwrite each other's output — producing
// bytecode for the wrong feature mask, silently.
fs::path scratchPath(const char* tag) {
    static std::atomic<uint64_t> counter{ 0 };
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "engine_shaderc_%d_%llu_%s",
                  currentProcessId(), (unsigned long long)n, tag);
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
    // ── CreateProcessW, mirroring the POSIX path below step for step ─────────
    // No shell, for the reason the POSIX side uses posix_spawn: shader paths
    // routinely contain spaces, and handing a command string to cmd.exe makes
    // argument splitting a correctness problem instead of a formatting one.
    //
    // stdout AND stderr go to the same FILE the POSIX branch uses, rather than a
    // pipe. A pipe would have to be drained while the child runs or shaderc
    // blocks once the buffer fills — a deadlock that shows up only on shaders
    // with many diagnostics, which are exactly the ones whose output matters.
    // Redirecting to a file has no such failure mode and keeps both branches
    // reading their diagnostics the same way.
    std::wstring cmd;
    appendQuoted(cmd, shadercExe.wstring());
    for (const auto& a : args) appendQuoted(cmd, widen(a));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;               // the child must inherit the log
    HANDLE log = ::CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        res.error = "cannot open a shaderc log file (GetLastError " +
                    std::to_string((unsigned long)::GetLastError()) + ")";
        return res;
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError  = log;                    // same target: adddup2's equivalent
    si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    std::wstring cmdMutable = cmd;          // CreateProcessW may write to this
    const BOOL ok = ::CreateProcessW(nullptr, cmdMutable.data(), nullptr, nullptr,
                                     TRUE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(log);
        res.error = "cannot spawn shaderc (GetLastError " +
                    std::to_string((unsigned long)err) + ")";
        fs::remove(logPath, ec);
        return res;
    }
    ::CloseHandle(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hProcess);
    // Closed BEFORE slurping: the child's writes are only guaranteed visible
    // once every handle to the file is closed, and ours is the last one.
    ::CloseHandle(log);

    res.diagnostics = slurp(logPath);
    fs::remove(logPath, ec);

    if (exitCode != 0) {
        res.error = "shaderc failed (exit " + std::to_string((unsigned long)exitCode) + ")";
        fs::remove(outPath, ec);
        return res;
    }
    if (!readBinary(outPath, res.bytecode)) {
        // Same reasoning as the POSIX branch: exit 0 with no usable output is a
        // failure, not an empty success. An empty program would reach bgfx and
        // fail there, far from the cause.
        res.error = "shaderc reported success but produced no output";
        fs::remove(outPath, ec);
        return res;
    }
    fs::remove(outPath, ec);
    res.ok = true;
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
