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
// Exit code 0 means "result file written"; anything else means crash.
#include "assets/cookers/mesh_cooker.h"
#include "assets/cookers/texture_cooker.h"

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

#if !defined(_WIN32)
    #include <sys/resource.h>
    #include <csignal>
    #include <thread>
    #include <chrono>
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

#if !defined(_WIN32)
    // Hard memory cap — the pipeline's MemGovernor schedules by ESTIMATE;
    // this is the enforcement. RLIMIT_AS is a no-op on macOS, RLIMIT_DATA
    // does work there (and on Linux covers the heap) — set both, best-effort.
    if (memCapMb > 0) {
        rlimit rl{ (rlim_t)memCapMb << 20, (rlim_t)memCapMb << 20 };
        setrlimit(RLIMIT_DATA, &rl);
        setrlimit(RLIMIT_AS,   &rl);
    }
#endif
#if defined(__APPLE__)
    // The parent's cook threads are QoS-demoted; a spawned child is not.
    // Re-demote so the cook stays invisible to foreground work.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    nice(10);
#endif

#if !defined(_WIN32)
    // Fault-injection hooks for testing the containment paths end to end
    // (a crash you can't reproduce on demand is a crash path you never
    // actually verified). Trigger only when the env var value appears in
    // the source filename.
    if (const char* t = std::getenv("COOK_WORKER_TEST_CRASH");
        t && *t && sourcePath.filename().string().find(t) != std::string::npos)
        raise(SIGSEGV);
    if (const char* t = std::getenv("COOK_WORKER_TEST_HANG");
        t && *t && sourcePath.filename().string().find(t) != std::string::npos)
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));
#endif

    std::vector<std::unique_ptr<assetlib::ICooker>> cookers;
    cookers.push_back(std::make_unique<MeshCooker>());
    cookers.push_back(std::make_unique<TextureCooker>());

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

    std::ofstream f(resultPath, std::ios::binary | std::ios::trunc);
    if (!f) return 65;                               // parent reports "no result"
    f << "RESULT " << (res.success ? "ok" : res.skipped ? "skip" : "fail") << '\n';
    if (!res.error.empty()) f << "ERROR "  << oneLine(res.error) << '\n';
    for (const auto& p : extras) f << "OUTPUT " << p.string() << '\n';
    for (const auto& d : deps)   f << "DEP "    << d << '\n';
    f.flush();
    return f.good() ? 0 : 65;
}
