// ── engine_module_probe — run the module load gauntlet and say what happened ─
//
// Loads one or more shared libraries through the REAL `ModuleLibrary::load` and
// reports, per module, whether the host accepted or refused it. Two jobs, and
// the second is the reason this is a program rather than a test:
//
//  1. A diagnostic. "My kit won't load" is a question the engine could only
//     answer by running the editor and reading the log. This answers it in one
//     command, against the same code path the editor uses, with no GPU, no
//     window and no project.
//
//  2. A TESTABLE BOUNDARY. The gauntlet in module_loader.h is C++ internals —
//     there is no C entry point to call and inventing one would mean writing
//     untested C++ for the privilege of testing in Rust. A command line is a
//     boundary that already has to be stable, the same way engine_cook_worker's
//     is, so the Rust suite drives this and no shim exists purely to be tested.
//
// ── Output contract ─────────────────────────────────────────────────────────
// One line per module on stdout, in load order, then a terminator:
//
//     module <index> <ok|refused> <path>
//     probe done
//
// The host's own LOG_ERROR explanation goes to stderr, untouched — that is the
// half a human wants and the half a test should not parse.
//
// Exit status is 0 whenever the probe RAN, refusals included: a refusal is a
// successful probe of a bad module, and collapsing it into a non-zero exit would
// make it indistinguishable from a crash. 2 is usage, 3 is a path that is not
// there. That separation is what lets the test suite assert that a rejected
// module is rejected CLEANLY rather than by falling over.
//
// ── Why several modules in one run ──────────────────────────────────────────
// The kit-to-kit contract check compares a module's declarations against a
// registry that starts EMPTY, so a single module can never fail it — the first
// one to declare a contract is the one that pins it. Provoking a mismatch needs
// two modules in one process, in order, with the first still loaded. Every
// library stays alive until the probe exits for exactly that reason.
#include "runtime/module_loader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int usage() {
    std::fprintf(stderr,
        "usage: engine_module_probe [--reload=never|allowed] <module> [module...]\n"
        "\n"
        "  Loads each module through the engine's compatibility gauntlet and\n"
        "  prints 'module <i> ok|refused <path>' for each, in order.\n"
        "\n"
        "  --reload=never    dlopen the file in place (what a shipped player does)\n"
        "  --reload=allowed  copy to temp first, the editor's hot-reload path (default)\n");
    return 2;
}

using modload::ModuleLibrary;

int main(int argc, char** argv) {
    auto reload = ModuleLibrary::Reload::Allowed;
    std::vector<fs::path> paths;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--reload=never") == 0) {
            reload = ModuleLibrary::Reload::Never;
        } else if (std::strcmp(a, "--reload=allowed") == 0) {
            reload = ModuleLibrary::Reload::Allowed;
        } else if (a[0] == '-') {
            std::fprintf(stderr, "engine_module_probe: unknown option '%s'\n", a);
            return usage();
        } else {
            paths.emplace_back(a);
        }
    }
    if (paths.empty()) return usage();

    // Checked up front, before anything is loaded. A missing file would
    // otherwise surface as an ordinary refusal, and "the gate rejected this
    // module" is a very different fact from "there was no module".
    for (const fs::path& p : paths) {
        if (!fs::exists(p)) {
            std::fprintf(stderr, "engine_module_probe: no such file: %s\n",
                         p.string().c_str());
            return 3;
        }
    }

    // Held for the whole run: a module unloaded early would release its contract
    // pins, and the next module would then find an empty registry and load
    // clean — the exact skew the contract check exists to catch.
    std::vector<std::unique_ptr<ModuleLibrary>> held;
    held.reserve(paths.size());

    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto lib = std::make_unique<ModuleLibrary>();
        const bool ok = lib->load(paths[i], reload);
        std::printf("module %zu %s %s\n", i, ok ? "ok" : "refused",
                    paths[i].string().c_str());
        std::fflush(stdout);
        held.push_back(std::move(lib));
    }

    // A terminator, so a test can tell a complete run from output truncated by a
    // crash partway through. Without it, a probe that segfaulted after printing
    // two of three lines looks like a probe that was asked about two modules.
    std::printf("probe done\n");
    std::fflush(stdout);
    return 0;
}
