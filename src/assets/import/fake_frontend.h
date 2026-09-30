#pragma once
// ── FakeFrontend — the import-frontend contract's fake implementation ─────────
//
// Returns exactly what it was told to for a path: a scene or an error. It
// parses nothing. It is what makes the two sides of the contract independent:
// the back end (WO-011) is built and tested against scenes described here,
// before any real parser is ported to ImportedScene (WO-012, WO-013).
//
// "Described" means the ImportedScene value itself. There is no mini-language:
// the type IS the description, and a second format for it would be one more
// thing to keep in step.
//
// Reentrant like any front end: set() everything first, then importScene() may
// be called concurrently (it only reads).
#include "assets/import/import_frontend.h"

#include <map>
#include <variant>

namespace imp {

class FakeFrontend final : public IImportFrontend {
public:
    const char* name() const override { return "fake"; }
    std::vector<std::string> extensions() const override { return {"fake"}; }

    void set(const std::filesystem::path& source, ImportedScene scene) {
        m_answers[source.generic_string()] = std::move(scene);
    }
    void set(const std::filesystem::path& source, ImportError error) {
        m_answers[source.generic_string()] = std::move(error);
    }

    ImportResult importScene(const std::filesystem::path& source,
                             const ImportOptions&) const override {
        auto it = m_answers.find(source.generic_string());
        if (it == m_answers.end())
            return ImportError{ImportError::Kind::Unreadable, "no such file: " + source.string()};
        if (const auto* e = std::get_if<ImportError>(&it->second)) return *e;
        ImportedScene s = std::get<ImportedScene>(it->second);   // by value, as the contract says
        s.source = source.string();
        return s;
    }

private:
    std::map<std::string, std::variant<ImportedScene, ImportError>> m_answers;
};

}  // namespace imp
