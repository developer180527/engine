#include "assetlib/ddc_manifest.h"

namespace assetlib {

namespace {
constexpr const char* kMagic = "ddc-manifest-v1";
constexpr const char* kPrimaryName = "@";
} // namespace

bool ddcStoreRecord(DdcStore& ddc, const std::string& key,
                    const std::filesystem::path& primary,
                    const std::vector<std::filesystem::path>& extras) {
    std::string manifest = std::string(kMagic) + "\n";
    auto addMember = [&](const std::filesystem::path& file,
                         const std::string& name) -> bool {
        const std::string mk = blake3File(file);
        if (mk.empty() || !ddc.store(mk, file)) return false;
        manifest += mk; manifest += '\t'; manifest += name; manifest += '\n';
        return true;
    };
    if (!addMember(primary, kPrimaryName)) return false;
    for (const auto& e : extras)
        if (!addMember(e, e.filename().string())) return false;
    return ddc.storeBytes(key, manifest);
}

bool ddcFetchRecord(DdcStore& ddc, const std::string& key,
                    const std::filesystem::path& outPath) {
    std::string manifest;
    if (!ddc.fetchBytes(key, manifest)) return false;

    size_t pos = manifest.find('\n');
    if (pos == std::string::npos ||
        manifest.compare(0, pos, kMagic) != 0) return false;
    ++pos;
    while (pos < manifest.size()) {
        size_t eol = manifest.find('\n', pos);
        if (eol == std::string::npos) eol = manifest.size();
        const std::string line = manifest.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        const std::string mk   = line.substr(0, tab);
        const std::string name = line.substr(tab + 1);
        // Names are plain sibling filenames — a manifest from a SHARED store
        // is remote input; never let one path-traverse out of the cache dir.
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name.find("..") == 0)
            return false;
        const std::filesystem::path dst =
            (name == kPrimaryName) ? outPath : outPath.parent_path() / name;
        if (!ddc.fetch(mk, dst)) return false;
    }
    return true;
}

} // namespace assetlib
