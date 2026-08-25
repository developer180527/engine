// engine_cook_worker — cook EXACTLY ONE asset in an isolated child process.
//
//   engine_cook_worker <sourcePath> <outputPath> <resultPath> <memCapMB>
//
// Spawned by CookPipeline (never run by hand). The whole point is blast
// containment: Assimp/stb parsing a corrupt file can SIGSEGV, SIGBUS, or
// abort() — none of which a try/catch can trap. In here, that kills THIS
// process; the pipeline reaps the signal and marks one asset failed while
// the editor keeps running. The memory cap is a real setrlimit, so a
// runaway import hits bad_alloc inside its own sandbox instead of OOM-ing
// the machine.
//
// Outcome protocol: a sidecar result file (never stdout — cookers print
// freely). See runWorkerProcess() in cook_pipeline.cpp:
//     RESULT ok|skip|fail
//     ERROR  <single-line message>
//     OUTPUT <extra output path>     (mesh's sibling .ctex files)
//     DEP    <uuid>
// wrapped in the magic/END frame defined by assetlib/cook_result_file.h, so a
// write this process does not finish is detectably incomplete rather than
// reading as a success that lost its OUTPUT lines.
// Exit code 0 means "result file written"; anything else means crash.
#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/cookers/texture/texture_cooker.h"
#include "assets/cookers/shader/shader_cooker.h"
#include "assets/cookers/material/material_cooker.h"

#include <assetlib/cook_result_file.h>   // the framing, shared with the parent

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#if defined(_WIN32)
// Guarded: the top-level CMakeLists already defines both for MSVC
// (add_compile_definitions), and an unguarded #define here is
// "warning C4005: macro redefinition" on every Windows TU that includes
// this. Harmless individually, and collectively it buries the warnings
// that mean something.
    #ifndef WIN32_LEAN_AND_MEAN
    #  define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #  define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/resource.h>
    #include <csignal>
#endif
#if defined(__APPLE__)
    #include <pthread/qos.h>
#elif defined(__linux__)
    #include <unistd.h>
#endif

static std::string oneLine(std::string s) {
    for (auto& c : s)
        if (c == '\n' || c == '\r') c = '|';
    return s;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
            "engine_cook_worker: internal tool, spawned by the cook pipeline\n"
            "usage: engine_cook_worker <source> <output> <result> <memCapMB>\n");
        return 64;
    }
    const std::filesystem::path sourcePath = argv[1];
    const std::filesystem::path outputPath = argv[2];
    const std::filesystem::path resultPath = argv[3];
    const long                  memCapMb   = std::atol(argv[4]);

#if defined(_WIN32)
    // On Windows the cap is already in force: the parent assigned this
    // process to a job object with JOB_OBJECT_LIMIT_PROCESS_MEMORY while it
    // was still suspended, so the limit predates our first instruction.
    // Nothing to do here — see cook_dispatch.cpp.
    (void)memCapMb;
#else
    // Hard memory cap — the pipeline's MemGovernor schedules by ESTIMATE;
    // this is the enforcement. RLIMIT_AS is a no-op on macOS, RLIMIT_DATA
    // does work there (and on Linux covers the heap) — set both, best-effort.
    if (memCapMb > 0) {
        rlimit rl{ (rlim_t)memCapMb << 20, (rlim_t)memCapMb << 20 };
        setrlimit(RLIMIT_DATA, &rl);
        setrlimit(RLIMIT_AS,   &rl);
    }
#endif
#if defined(_WIN32)
    // Same intent as the QoS demotion below: keep the cook off the foreground
    // scheduler. BELOW_NORMAL_PRIORITY_CLASS is the process-wide equivalent.
    ::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#elif defined(__APPLE__)
    // The parent's cook threads are QoS-demoted; a spawned child is not.
    // Re-demote so the cook stays invisible to foreground work.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    nice(10);
#endif

    // Fault-injection hooks for testing the containment paths end to end
    // (a crash you can't reproduce on demand is a crash path you never
    // actually verified). Trigger only when the env var value appears in
    // the source filename. These must exist on every platform, or the
    // containment tests silently stop covering the one being ported.
    if (const char* t = std::getenv("COOK_WORKER_TEST_CRASH");
        t && *t && sourcePath.filename().string().find(t) != std::string::npos) {
#if defined(_WIN32)
        // Raises STATUS_ACCESS_VIOLATION (0xC0000005), which the parent
        // classifies as a crash — the counterpart of raise(SIGSEGV).
        *(volatile int*)nullptr = 0;
#else
        raise(SIGSEGV);
#endif
    }
    if (const char* t = std::getenv("COOK_WORKER_TEST_HANG");
        t && *t && sourcePath.filename().string().find(t) != std::string::npos)
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));

    std::vector<std::unique_ptr<assetlib::ICooker>> cookers;
    cookers.push_back(std::make_unique<MeshCooker>());
    cookers.push_back(std::make_unique<TextureCooker>());
    // Shader compiles spawn shaderc, a third-party compiler stack already
    // known to abort under sanitizers — exactly the kind of blast radius this
    // child process exists to contain.
    cookers.push_back(std::make_unique<ShaderCooker>());
    cookers.push_back(std::make_unique<MaterialCooker>());

    std::string ext = sourcePath.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    assetlib::ICooker* cooker = nullptr;
    for (auto& c : cookers)
        for (auto& e : c->extensions())
            if (e == ext) { cooker = c.get(); break; }

    std::vector<std::string>           deps;
    std::vector<std::filesystem::path> extras;
    assetlib::CookResult res;
    if (!cooker) {
        res = { .success=false,
                .error="worker has no cooker for extension: " + ext };
    } else {
        assetlib::CookContext ctx;
        ctx.sourcePath    = sourcePath;
        ctx.outputPath    = outputPath;
        ctx.addDependency = [&deps](const assetlib::UUID& u) {
            deps.push_back(u.toString());
        };
        ctx.addOutput     = [&extras](const std::filesystem::path& p) {
            extras.push_back(p);
        };
        try {
            res = cooker->cook(ctx);
        } catch (const std::exception& e) {
            res = { .success=false,
                    .error=std::string("cooker threw: ") + e.what() };
        } catch (...) {
            res = { .success=false, .error="cooker threw a non-std exception" };
        }
    }

    // Build the body in memory, then frame it. A worker killed mid-write (the
    // deadline SIGKILL, an OOM, a signal from a corrupt parse) used to leave a
    // file whose FIRST line was already "RESULT ok" and whose OUTPUT lines were
    // gone — the parent read success and shipped an asset missing its siblings.
    // That is the silently-untextured-build failure mode, arriving through the
    // IPC channel. The trailer makes truncation detectable: no END line, or a
    // line count or digest that disagrees, means incomplete.
    std::string body;
    size_t lines = 0;
    auto put = [&](const std::string& s) { body += s; body += '\n'; ++lines; };

    put(std::string("RESULT ") + (res.success ? "ok" : res.skipped ? "skip" : "fail"));
    if (!res.error.empty()) put("ERROR " + oneLine(res.error));
    for (const auto& p : extras) put("OUTPUT " + p.string());
    for (const auto& d : deps)   put("DEP "    + d);

    const std::string framed = assetlib::cookresult::frame(body, lines);

    std::ofstream f(resultPath, std::ios::binary | std::ios::trunc);
    if (!f) return 65;                               // parent reports "no result"
    f.write(framed.data(), (std::streamsize)framed.size());
    f.flush();
    // Close before reporting success: a buffered write that only fails at close
    // would otherwise be reported as a complete result.
    f.close();
    return f.good() ? 0 : 65;
}
