#pragma once
#include "uuid.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdint>

namespace assetlib {

enum class AssetType : uint32_t {
    Unknown=0, Mesh=1, Texture=2, Material=3,
    Scene=4, Prefab=5, Shader=6, Audio=7,
};
std::string assetTypeName(AssetType t);
AssetType   assetTypeFromExtension(const std::string& ext);

enum class AssetState : uint8_t {
    Unknown=0, Registered=1, Importing=2, Ready=3,
    Stale=4, Missing=5, Failed=6, Dirty=7,
};
std::string assetStateName(AssetState s);

struct ImportSettings {
    std::string json;
    bool empty() const { return json.empty(); }
};

struct AssetRecord {
    UUID          uuid;
    AssetType     type            = AssetType::Unknown;
    AssetState    state           = AssetState::Unknown;
    std::string   sourcePath;
    std::string   sourceHash;
    int64_t       sourceMtime     = 0;
    int64_t       sourceSize      = 0;
    std::string   cookedPath;
    uint32_t      cookVersion     = 0;
    uint32_t      importerVersion = 0;
    int64_t       cookedAt        = 0;
    ImportSettings importSettings;
    std::string   errorMessage;
    // DDC key of the last cook ATTEMPT (success, skip, or failure) — the
    // content address of everything that went into it. Staleness is simply
    // "stored key != current key"; a Failed record with a matching key means
    // "these exact inputs already failed, don't retry until they change".
    std::string   ddcKey;
};

class AssetRegistry {
public:
    AssetRegistry()  = default;
    ~AssetRegistry();

    bool open(const std::filesystem::path& dbPath);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    bool insert(const AssetRecord& rec);
    bool update(const AssetRecord& rec);
    bool remove(const UUID& uuid);

    std::optional<AssetRecord> findByUUID(const UUID& uuid)             const;
    std::optional<AssetRecord> findBySourcePath(const std::string& rel) const;
    std::vector<AssetRecord>   findByType(AssetType type)               const;
    std::vector<AssetRecord>   findByState(AssetState state)            const;
    std::vector<AssetRecord>   all()                                    const;

    bool setState(const UUID& uuid, AssetState state,
                  const std::string& errorMsg = {});

    bool              addDependency(const UUID& asset, const UUID& dep);
    bool              removeDependencies(const UUID& asset);
    bool              wouldCreateCycle(const UUID& from, const UUID& to) const;
    std::vector<UUID> dependents(const UUID& uuid)           const;
    std::vector<UUID> dependencies(const UUID& uuid)         const;
    std::vector<UUID> transitiveDependents(const UUID& uuid) const;

    int scan(const std::filesystem::path& assetsRoot,
             const std::filesystem::path& projectRoot);

private:
    void* m_db = nullptr;
    bool  execSQL(const std::string& sql);
    void  migrate();
    static bool statChanged(const AssetRecord& rec,
                            const std::filesystem::path& p);
};

} // namespace assetlib
