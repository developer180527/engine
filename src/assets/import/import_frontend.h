#pragma once
// ── import_frontend — the contract every source-format front end implements ───
//
// docs/contracts/import-frontend.md is the contract; this is its shape. In short:
//   * importScene() returns an ImportedScene (with its `dropped` list) or an
//     ImportError. It never throws across this boundary and never logs a
//     failure itself: the caller has the context (asset, cook, project).
//   * SYNCHRONOUS on purpose. Front ends run inside cook workers; the runtime
//     never imports (WO-018), so an async API would only invite it to.
//   * REENTRANT: each call owns its parser state, so cook workers call it
//     concurrently.
//   * The result owns everything by value. The library's own memory (aiScene,
//     cgltf_data) is freed before the call returns.
//
// "Nothing" has three distinct answers, and they are not interchangeable:
//   Unsupported — no front end handles this extension (the registry's stub).
//   Unreadable  — a front end handles it, and the file could not be read/parsed.
//   Empty       — it parsed, and there is nothing to import. Never an empty
//                 ImportedScene: that could not be told apart from a bug.
#include "assets/import/imported_scene.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace imp {

struct ImportError {
    enum class Kind { Unsupported, Unreadable, Empty };
    Kind        kind = Kind::Unreadable;
    std::string message;                 // includes the path
};

inline const char* toString(ImportError::Kind k) {
    switch (k) {
        case ImportError::Kind::Unsupported: return "unsupported";
        case ImportError::Kind::Unreadable:  return "unreadable";
        case ImportError::Kind::Empty:       return "empty";
    }
    return "?";
}

// Either a scene or an error. A thin wrapper so call sites read as intent
// (`if (!r) … r.error() … r.scene()`) rather than std::get<> plumbing.
class ImportResult {
public:
    ImportResult(ImportedScene s) : m_v(std::move(s)) {}
    ImportResult(ImportError e)   : m_v(std::move(e)) {}

    explicit operator bool() const { return std::holds_alternative<ImportedScene>(m_v); }
    const ImportedScene& scene() const { return std::get<ImportedScene>(m_v); }
    ImportedScene&       scene()       { return std::get<ImportedScene>(m_v); }
    const ImportError&   error() const { return std::get<ImportError>(m_v); }

private:
    std::variant<ImportedScene, ImportError> m_v;
};

// A clip's display name. DCC tools name every take "Take 001" (or Mixamo's
// "mixamo.com", or nothing); such a clip is named after the file, "<stem>_<i>"
// when the file has several. Shared by every front end, so an FBX and a glTF
// of the same clip name it the same.
inline std::string clipDisplayName(const std::string& raw, const std::string& stem,
                                   unsigned index, unsigned count) {
    if (!raw.empty() && raw != "mixamo.com" && raw.rfind("Take", 0) != 0) return raw;
    return count > 1 ? stem + "_" + std::to_string(index) : stem;
}

struct ImportOptions {
    // Reserved. Conventions are fixed (imported_scene.h), so nothing a caller
    // passes may change what a scene means; options can only ever add detail.
};

class IImportFrontend {
public:
    virtual ~IImportFrontend() = default;
    virtual const char* name() const = 0;
    // Lower-case, without the dot: {"gltf", "glb"}.
    virtual std::vector<std::string> extensions() const = 0;
    virtual ImportResult importScene(const std::filesystem::path& source,
                                     const ImportOptions& options) const = 0;
};

// Routes a path to the front end that claims its extension. With none, the
// answer is the contract's stub: Unsupported, naming the extension.
class ImportFrontendRegistry {
public:
    void add(std::unique_ptr<IImportFrontend> f) { m_frontends.push_back(std::move(f)); }

    const IImportFrontend* forPath(const std::filesystem::path& p) const {
        const std::string ext = lowerExtension(p);
        for (const auto& f : m_frontends)
            for (const auto& e : f->extensions())
                if (e == ext) return f.get();
        return nullptr;
    }

    ImportResult importScene(const std::filesystem::path& source,
                             const ImportOptions& options = {}) const {
        if (const IImportFrontend* f = forPath(source))
            return f->importScene(source, options);
        const std::string ext = lowerExtension(source);
        return ImportError{ImportError::Kind::Unsupported,
                           "no import front end for '." + ext + "' files: " + source.string()};
    }

private:
    static std::string lowerExtension(const std::filesystem::path& p) {
        std::string e = p.extension().string();
        if (!e.empty() && e[0] == '.') e.erase(0, 1);
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return e;
    }

    std::vector<std::unique_ptr<IImportFrontend>> m_frontends;
};

}  // namespace imp
