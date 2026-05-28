#include "assetlib/cook_pipeline.h"
#include <ctime>

namespace assetlib {

CookPipeline::CookPipeline(AssetRegistry& registry,
                           std::filesystem::path projectRoot,
                           std::filesystem::path cacheRoot)
    : m_registry(registry)
    , m_projectRoot(std::move(projectRoot))
    , m_cacheRoot(std::move(cacheRoot)) {}

void CookPipeline::registerCooker(std::unique_ptr<ICooker> cooker) {
    m_cookers.push_back(std::move(cooker));
}

ICooker* CookPipeline::findCooker(const std::string& ext) const {
    for (auto& c : m_cookers)
        for (auto& e : c->extensions())
            if (e == ext) return c.get();
    return nullptr;
}

bool CookPipeline::hasCookerFor(const std::string& ext) const {
    std::string lower = ext;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    return findCooker(lower) != nullptr;
}
bool CookPipeline::isStale(const AssetRecord& rec) const {
    if (rec.cookedPath.empty())                  return true;
    if (rec.cookVersion != kCurrentCookVersion)  return true;
    auto cooked = m_cacheRoot / rec.cookedPath;
    if (!std::filesystem::exists(cooked))        return true;
    if (rec.state == AssetState::Stale)          return true;
    // Source file newer than cooked file — catches edits on disk
    auto source = m_projectRoot / rec.sourcePath;
    if (std::filesystem::exists(source)) {
        auto st = std::filesystem::last_write_time(source);
        auto ct = std::filesystem::last_write_time(cooked);
        if (st > ct) return true;
    }
    return false;
}

CookResult CookPipeline::cookOne(const UUID& uuid) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found in registry" };
    if (!isStale(*rec)) return { .success=true }; // already fresh

    auto ext = std::filesystem::path(rec->sourcePath).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    ICooker* cooker = findCooker(ext);
    if (!cooker) return { .success=false, .error="No cooker registered for extension: " + ext };

    auto outDir = m_cacheRoot / (assetTypeName(rec->type) + "s");
    std::filesystem::create_directories(outDir);
    auto outPath = outDir / (uuid.toString() + ".cooked");

    CookContext ctx;
    ctx.uuid       = uuid;
    ctx.sourcePath = m_projectRoot / rec->sourcePath;
    ctx.outputPath = outPath;
    ctx.addDependency = [&](const UUID& dep) {
        m_registry.addDependency(uuid, dep);
    };

    auto result = cooker->cook(ctx);
    if (result.success) {
        rec->cookedPath   = std::filesystem::relative(outPath, m_cacheRoot).string();
        rec->cookVersion  = kCurrentCookVersion;
        rec->cookedAt     = static_cast<int64_t>(std::time(nullptr));
        rec->state        = AssetState::Ready;
        m_registry.update(*rec);
        std::printf("[AssetLib] Cooked: %s\n", rec->sourcePath.c_str());
    }
    return result;
}

int CookPipeline::cookAll(std::function<void(int,int)> progress) {
    auto all   = m_registry.all();
    int  cooked = 0;
    int  total  = static_cast<int>(all.size());
    for (int i = 0; i < total; ++i) {
        if (isStale(all[i])) {
            auto r = cookOne(all[i].uuid);
            if (r.success) ++cooked;
        } else if (all[i].state != AssetState::Ready) {
            // Already cooked — just mark Ready so binary loader can trust the state
            auto rec = m_registry.findByUUID(all[i].uuid);
            if (rec) { rec->state = AssetState::Ready; m_registry.update(*rec); }
        }
        if (progress) progress(i + 1, total);
    }
    return cooked;
}

CookResult CookPipeline::forceRecook(const UUID& uuid) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found" };
    // Clear cooked path to force staleness
    rec->cookedPath = "";
    m_registry.update(*rec);
    return cookOne(uuid);
}

} // namespace assetlib
