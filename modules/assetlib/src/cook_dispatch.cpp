#include "cook_dispatch.h"
#include "cook_env.h"
#include "assetlib/cook_result_file.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <unistd.h>
#endif

namespace assetlib {

namespace {

#if !defined(_WIN32)
// Distinct from the worker's own codes (64 = bad usage, 65 = could not write a
// result) so "the binary is missing or not executable" is diagnosable rather
// than looking like a cook that crashed.
constexpr int kExecFailedExit = 66;
#endif

// The worker's sidecar RESULT FILE protocol (never parsed from stdout —
// cookers print freely):
//     RESULT ok|skip|fail
//     ERROR  <single-line message>       (optional)
//     OUTPUT <extra output path>         (0..n, mesh's sibling .ctex)
//     DEP    <uuid>                      (0..n)
//
// Framed with a magic header and an END trailer — see assetlib/cook_result_file.h
// for why (a truncated file used to read as a clean success with its OUTPUT lines
// missing). The frame is validated before any field is read.
//
// Platform-independent: both the POSIX and Windows spawn paths converge here
// once the child has been reaped. `exitDesc` describes how the child ended,
// for the diagnostic when it wrote no result at all.
CookResult finishFromResultFile(const std::filesystem::path& resultPath,
                                const CookContext& ctx,
                                const std::string& exitDesc) {
    namespace fs = std::filesystem;
    auto cleanup = [&] { std::error_code e; fs::remove(resultPath, e); };

    std::ifstream f(resultPath, std::ios::binary);
    if (!f)
        return { .success=false,
                 .error="cook worker " + exitDesc + " without writing a result" };

    const std::string raw((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    f.close();

    // Validate the frame BEFORE reading a single field. `RESULT ok` is the first
    // body line, so a worker killed mid-write leaves a file that parses as a
    // clean success with its OUTPUT lines missing — and the asset commits
    // without its siblings. Refuse anything whose trailer does not agree with
    // its body; an incomplete result is a failed cook, not a successful one.
    std::string body, frameErr;
    if (!cookresult::unframe(raw, body, frameErr)) {
        cleanup();
        return { .success=false,
                 .error="cook worker " + exitDesc + ", but its result file is "
                        "unusable: " + frameErr };
    }

    std::string verdict, error, line;
    std::istringstream in(body);
    while (std::getline(in, line)) {
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
    cleanup();

    if (verdict == "ok")   return { .success=true };
    if (verdict == "skip") return { .success=false, .skipped=true, .error=error };
    if (verdict == "fail") return { .success=false, .error=error };
    return { .success=false, .error="worker result file had no RESULT line" };
}

// The per-task memory cap in MB: 2x the cooker's estimate with a 1 GB floor
// (estimates are ceilings, not promises), or COOK_TASK_MEM_CAP_MB verbatim.
long taskMemCapMb(ICooker& cooker, const CookContext& ctx) {
    long capMb = envLong("COOK_TASK_MEM_CAP_MB", 0);
    if (capMb <= 0)
        capMb = std::max((long)((cooker.estimatePeakBytes(ctx) * 2) >> 20), 1024L);
    return capMb;
}

} // namespace

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

// One asset per child process. Exit-by-signal, a missing/garbled result file,
// or a deadline overrun all become a per-asset failure — the host process
// (editor!) never dies with it.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx,
                               const CancelFn& isCancelled) {
    namespace fs = std::filesystem;
    const fs::path resultPath = ctx.outputPath.string() + ".result";
    std::error_code ec;
    fs::remove(resultPath, ec);

    // Hard per-task memory cap, applied by the CHILD via setrlimit: a runaway
    // import hits ENOMEM/bad_alloc inside its own process instead of OOM-ing
    // the machine.
    const long capMb = taskMemCapMb(cooker, ctx);

    std::string exeArg = workerExe.string();
    std::string srcArg = ctx.sourcePath.string();
    std::string outArg = ctx.outputPath.string();
    std::string resArg = resultPath.string();
    std::string capArg = std::to_string(capMb);
    char* argv[] = { exeArg.data(), srcArg.data(), outArg.data(),
                     resArg.data(), capArg.data(), nullptr };

    // fork + setrlimit + exec, NOT posix_spawn. rlimits are inherited across
    // exec, so applying the cap in the child between fork and exec makes it
    // predate the worker's first instruction — including its static
    // initializers. Applying it inside the worker's own main() (from argv, which
    // is what this used to do, and still does as a belt-and-braces second
    // application) leaves everything before main uncapped. posix_spawn has no
    // rlimit attribute in POSIX, so there is no way to express this through it.
    // This gives POSIX the property the Windows path already had: it assigns the
    // job object while the process is still suspended.
    //
    // Between fork and exec, only async-signal-safe calls are legal. This parent
    // is multithreaded (the cook graph's worker pool), so a malloc here could
    // deadlock on a lock some other thread held at fork time. setrlimit and
    // execv are syscalls, and every string above was built before the fork.
    pid_t pid = ::fork();
    if (pid < 0)
        return { .success=false,
                 .error=std::string("cannot fork cook worker: ")
                        + std::strerror(errno) };
    if (pid == 0) {
        if (capMb > 0) {
            rlimit rl{ (rlim_t)capMb << 20, (rlim_t)capMb << 20 };
            ::setrlimit(RLIMIT_DATA, &rl);   // heap; the one that bites on macOS
            ::setrlimit(RLIMIT_AS,   &rl);   // address space; no-op on macOS
        }
        ::execv(exeArg.c_str(), argv);       // inherits environ, so the
                                             // fault-injection vars still reach it
        ::_exit(kExecFailedExit);            // exec returns only on failure
    }

    // Reap with a deadline. Default is generous — an HQ BC7 8K encode is
    // legitimately minutes — but bounded, so one wedged parse can't hold a
    // worker slot forever (COOK_TASK_TIMEOUT_SEC overrides).
    const long timeoutSec = envLong("COOK_TASK_TIMEOUT_SEC", 3600);
    const auto deadline   = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSec);
    int  status    = 0;
    bool timedOut  = false;
    bool aborted   = false;
    bool reapError = false;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            // EINTR is a benign signal interruption of the call itself, NOT a
            // dead child — retrying is mandatory. Treating it as fatal both
            // misreported healthy cooks as crashes and leaked the child
            // (we'd leave the loop without ever reaping it).
            if (errno == EINTR) continue;
            reapError = true;
            break;
        }
        // Kill on cancellation (host shutting down) or deadline overrun. Both
        // reap the child so it can never outlive us as an orphan.
        const bool cancel = isCancelled && isCancelled();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            if (cancel) aborted  = true;
            else        timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto cleanupResult = [&] { std::error_code e; fs::remove(resultPath, e); };

    if (aborted) {
        cleanupResult();
        return { .success=false, .cancelled=true, .error="cook cancelled" };
    }
    if (timedOut) {
        cleanupResult();
        return { .success=false,
                 .error="cook timed out after " + std::to_string(timeoutSec)
                      + "s (worker killed)" };
    }
    if (reapError) {
        cleanupResult();
        return { .success=false,
                 .error=std::string("cannot reap cook worker: ")
                      + std::strerror(errno) };
    }
    if (WIFSIGNALED(status)) {
        cleanupResult();
        const int sig = WTERMSIG(status);
        return { .success=false,
                 .error=std::string("cook worker crashed: signal ")
                      + std::to_string(sig) + " (" + strsignal(sig) + ")" };
    }

    // exec never ran: a missing, non-executable, or wrong-architecture worker
    // binary. Distinguish it, because "cook worker exited (code 66) without
    // writing a result" sends you looking for a cooker bug instead of a build
    // or install problem.
    if (WIFEXITED(status) && WEXITSTATUS(status) == kExecFailedExit) {
        cleanupResult();
        return { .success=false,
                 .error="cannot exec cook worker at " + exeArg
                      + " (missing, not executable, or wrong architecture)" };
    }

    return finishFromResultFile(
        resultPath, ctx,
        "exited (code "
            + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1)
            + ")");
}

#else  // _WIN32

namespace {

// Quote one argument for a CreateProcess command line, per the rules
// CommandLineToArgvW parses back: backslashes are literal EXCEPT when they
// immediately precede a quote, where each must be doubled. Asset paths
// routinely contain spaces, so naive concatenation silently splits arguments
// and the worker receives garbage.
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
        if (i == arg.size()) {
            cmd.append(slashes * 2, L'\\');   // before the closing quote
            break;
        }
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

std::string lastErrorText(DWORD err) {
    char* buf = nullptr;
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (char*)&buf, 0, nullptr);
    std::string s = n && buf ? std::string(buf, n) : ("error " + std::to_string(err));
    if (buf) ::LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h) ::CloseHandle(h); }
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

} // namespace

// Windows out-of-process cooking. Same contract as the POSIX path: one asset
// per child, outcome via the sidecar result file, and no failure mode that can
// take the host down with it.
//
// Two things differ from POSIX by necessity:
//  - The memory cap is applied by the PARENT through a job object, not by the
//    child through setrlimit (Windows has no setrlimit). This is strictly
//    better: the limit is in force before the child runs its first
//    instruction, with no window where it could allocate freely.
//  - JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE guarantees the child dies with us even
//    if the host is killed outright — orphan protection POSIX doesn't give us
//    here, since a SIGKILLed parent never reaches its own kill() call.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx,
                               const CancelFn& isCancelled) {
    namespace fs = std::filesystem;
    const fs::path resultPath = ctx.outputPath.string() + ".result";
    std::error_code ec;
    fs::remove(resultPath, ec);

    const long capMb = taskMemCapMb(cooker, ctx);

    std::wstring cmd;
    appendQuoted(cmd, workerExe.wstring());
    appendQuoted(cmd, ctx.sourcePath.wstring());
    appendQuoted(cmd, ctx.outputPath.wstring());
    appendQuoted(cmd, resultPath.wstring());
    appendQuoted(cmd, std::to_wstring(capMb));

    // Job object carrying the memory cap. Created before the process so the
    // child can be assigned while still suspended.
    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (capMb > 0) {
            li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            li.ProcessMemoryLimit = (SIZE_T)capMb << 20;
        }
        ::SetInformationJobObject(job.h, JobObjectExtendedLimitInformation,
                                  &li, sizeof(li));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_SUSPENDED so the job (and its memory cap) is attached before the
    // child executes anything.
    std::wstring cmdMutable = cmd;   // CreateProcessW may write to this buffer
    if (!::CreateProcessW(nullptr, cmdMutable.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        return { .success=false,
                 .error="cannot spawn cook worker: "
                      + lastErrorText(::GetLastError()) };
    }
    Handle proc(pi.hProcess), thread(pi.hThread);

    if (job) ::AssignProcessToJobObject(job.h, proc.h);
    if (::ResumeThread(thread.h) == (DWORD)-1) {
        ::TerminateProcess(proc.h, 1);
        return { .success=false,
                 .error="cannot resume cook worker: "
                      + lastErrorText(::GetLastError()) };
    }

    const long timeoutSec = envLong("COOK_TASK_TIMEOUT_SEC", 3600);
    const auto deadline   = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSec);
    bool timedOut = false, aborted = false;
    for (;;) {
        const DWORD w = ::WaitForSingleObject(proc.h, 20);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_FAILED) {
            ::TerminateProcess(proc.h, 1);
            ::WaitForSingleObject(proc.h, INFINITE);
            return { .success=false,
                     .error="cannot wait on cook worker: "
                          + lastErrorText(::GetLastError()) };
        }
        const bool cancel = isCancelled && isCancelled();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            ::TerminateProcess(proc.h, 1);
            ::WaitForSingleObject(proc.h, INFINITE);   // never leave an orphan
            if (cancel) aborted  = true;
            else        timedOut = true;
            break;
        }
    }

    auto cleanupResult = [&] { std::error_code e; fs::remove(resultPath, e); };

    if (aborted) {
        cleanupResult();
        return { .success=false, .cancelled=true, .error="cook cancelled" };
    }
    if (timedOut) {
        cleanupResult();
        return { .success=false,
                 .error="cook timed out after " + std::to_string(timeoutSec)
                      + "s (worker killed)" };
    }

    DWORD code = 0;
    ::GetExitCodeProcess(proc.h, &code);
    // An unhandled SEH exception surfaces as the NTSTATUS itself, which always
    // has the top two bits set (0xC0000005 = access violation, 0xC000000D =
    // invalid parameter, 0x80000003 = breakpoint). That is this platform's
    // equivalent of WIFSIGNALED, and it must not be confused with a cooker
    // that merely returned a nonzero code.
    if ((code & 0xF0000000u) == 0xC0000000u || code == 0x80000003u) {
        cleanupResult();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)code);
        return { .success=false,
                 .error=std::string("cook worker crashed: exception ") + buf };
    }

    return finishFromResultFile(
        resultPath, ctx, "exited (code " + std::to_string((long)code) + ")");
}

#endif

CookResult dispatchCook(const std::filesystem::path& workerExe,
                        ICooker& cooker, const CookContext& ctx,
                        const CancelFn& isCancelled) {
    // Never start work the caller has already given up on.
    if (isCancelled && isCancelled())
        return { .success=false, .cancelled=true, .error="cook cancelled" };
    if (!workerExe.empty())
        return cookInWorkerProcess(workerExe, cooker, ctx, isCancelled);
    return cookInProcess(cooker, ctx);
}

} // namespace assetlib
