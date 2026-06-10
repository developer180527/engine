// engine_project — create and list engine projects from the command line.
//
//   engine_project create <dir> [--name <name>] [--template <id>]
//   engine_project list
//   engine_project templates
//
// SDK users without the editor scaffold projects with this; the editor's
// project hub calls the same engine_core code.
#include "io/project_scaffold.h"
#include "io/known_projects.h"
#include "io/project_context.h"

#include <cstdio>
#include <cstring>
#include <string>

static int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  engine_project create <dir> [--name <name>] [--template <id>]\n"
        "  engine_project list\n"
        "  engine_project templates\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];

    if (cmd == "templates") {
        for (const auto& t : project::templates())
            std::printf("%-10s %s — %s\n", t.id.c_str(), t.name.c_str(),
                        t.description.c_str());
        return 0;
    }

    if (cmd == "list") {
        auto list = project::KnownProjects::load();
        if (list.empty()) { std::printf("no known projects\n"); return 0; }
        for (const auto& kp : list)
            std::printf("%-24s %s%s\n", kp.name.c_str(),
                        kp.path.string().c_str(),
                        kp.exists() ? "" : "   (missing)");
        return 0;
    }

    if (cmd == "create") {
        if (argc < 3) return usage();
        std::filesystem::path dir = argv[2];
        std::string name       = dir.filename().string();
        std::string templateId = "basic3d";
        for (int i = 3; i + 1 < argc; i += 2) {
            if      (!std::strcmp(argv[i], "--name"))     name       = argv[i + 1];
            else if (!std::strcmp(argv[i], "--template")) templateId = argv[i + 1];
            else return usage();
        }

        std::string error;
        if (!project::create(dir, name, templateId, error)) {
            std::fprintf(stderr, "engine_project: %s\n", error.c_str());
            return 1;
        }
        project::KnownProjects::touch(name, std::filesystem::weakly_canonical(dir));
        std::printf("created '%s' (%s) at %s\n", name.c_str(),
                    templateId.c_str(), dir.string().c_str());
        return 0;
    }

    return usage();
}
