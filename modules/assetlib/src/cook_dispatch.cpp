#include "cook_dispatch.h"
#include "cook_env.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
    #include <spawn.h>
    #include <sys/wait.h>
    #include <signal.h>
    extern char** environ;
#endif

namespace assetlib {

CookResult cookInProcess(ICooker& cooker, const CookContext& ctx) {
    // Exception net: cook() runs third-party parsers (Assimp, stb, json) on
    // corrupt files — a throw escaping a worker std::thread is
    // std::terminate for the whole host process (cooker audit: "Unwrapped
    // Worker Thread Exception Paths"). Convert to a per-asset failure.
    try {
        return cooker.cook(ctx);
    } catch (const std::exception& e) {
        return { .success=false,
                 .error=std::string("cooker threw: ") + e.what() };
    } catch (...) {
        return { .success=false, .error="cooker threw a non-std exception" };
    }
}

#if !defined(_WIN32)

// One asset per child process. The worker writes its outcome to a sidecar
// RESULT FILE (never parsed from stdout — cookers print freely) with a tiny
// line protocol:
//     RESULT ok|skip|fail
//     ERROR  <single-line message>       (optional)
//     OUTPUT <extra output path>         (0..n, mesh's sibling .ctex)
//     DEP    <uuid>                      (0..n)
// Exit-by-signal, a missing/garbled result file, or a deadline overrun all
// become a per-asset failure — the host process (editor!) never dies with it.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx) {
    namespace fs = std::filesystem;
    const fs::path resultPath = ctx.outputPath.string() + ".result";
    std::error_code ec;
    fs::remove(resultPath, ec);

    // Hard per-task memory cap, applied by the CHILD via setrlimit: a runaway
    // import hits ENOMEM/bad_alloc inside its own process instead of OOM-ing
    // the machine. 2× the estimate with a 1 GB floor (estimates are ceilings,
    // not promises); COOK_TASK_MEM_CAP_MB pins it explicitly.
    long capMb = envLong("COOK_TASK_MEM_CAP_MB", 0);
    if (capMb <= 0)
        capMb = std::max((long)((cooker.estimatePeakBytes(ctx) * 2) >> 20),
                         1024L);

    std::string exeArg = workerExe.string();
    std::string srcArg = ctx.sourcePath.string();
    std::string outArg = ctx.outputPath.string();
    std::string resArg = resultPath.string();
    std::string capArg = std::to_string(capMb);
    char* argv[] = { exeArg.data(), srcArg.data(), outArg.data(),
                     resArg.data(), capArg.data(), nullptr };

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exeArg.c_str(), nullptr, nullptr,
                               argv, environ);
    if (rc != 0)
        return { .success=false,
                 .error=std::string("cannot spawn cook worker: ") + std::strerror(rc) };

    // Reap with a deadline. Default is generous — an HQ BC7 8K encode is
    // legitimately minutes — but bounded, so one wedged parse can't hold a
    // worker slot forever (COOK_TASK_TIMEOUT_SEC overrides).
    const long timeoutSec = envLong("COOK_TASK_TIMEOUT_SEC", 3600);
    const auto deadline   = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSec);
    int  status   = 0;
    bool timedOut = false;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0)   { status = -1; break; }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto cleanupResult = [&] { std::error_code e; fs::remove(resultPath, e); };

    if (timedOut) {
        cleanupResult();
        return { .success=false,
                 .error="cook timed out after " + std::to_string(timeoutSec)
                      + "s (worker killed)" };
    }
    if (WIFSIGNALED(status)) {
        cleanupResult();
        const int sig = WTERMSIG(status);
        return { .success=false,
                 .error=std::string("cook worker crashed: signal ")
                      + std::to_string(sig) + " (" + strsignal(sig) + ")" };
    }

    std::ifstream f(resultPath);
    if (!f) {
        return { .success=false,
                 .error="cook worker exited (code "
                      + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                      + ") without writing a result" };
    }
    std::string verdict, error, line;
    while (std::getline(f, line)) {
        if      (line.rfind("RESULT ", 0) == 0) verdict = line.substr(7);
        else if (line.rfind("ERROR ", 0)  == 0) error   = line.substr(6);
        else if (line.rfind("OUTPUT ", 0) == 0) {
            if (ctx.addOutput) ctx.addOutput(fs::path(line.substr(7)));
        }
        else if (line.rfind("DEP ", 0) == 0) {
            if (ctx.addDependency)
                ctx.addDependency(UUID::fromString(line.substr(4)));
        }
    }
    f.close();
    cleanupResult();
    if (verdict == "ok")   return { .success=true };
    if (verdict == "skip") return { .success=false, .skipped=true, .error=error };
    if (verdict == "fail") return { .success=false, .error=error };
    return { .success=false, .error="worker result file had no RESULT line" };
}

#else  // _WIN32

CookResult cookInWorkerProcess(const std::filesystem::path&,
                               ICooker&, const CookContext&) {
    return { .success=false,
             .error="out-of-process cooking not implemented on Windows" };
}

#endif

CookResult dispatchCook(const std::filesystem::path& workerExe,
                        ICooker& cooker, const CookContext& ctx) {
#if !defined(_WIN32)
    if (!workerExe.empty())
        return cookInWorkerProcess(workerExe, cooker, ctx);
#endif
    return cookInProcess(cooker, ctx);
}

} // namespace assetlib
