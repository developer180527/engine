#pragma once
#include "uuid.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdint>

namespace assetlib {

// ── Asset type tag ────────────────────────────────────────────────────────────
enum class AssetType : uint32_t {
    Unknown  = 0,
    Mesh     = 1,
    Texture  = 2,
    Material = 3,
    Scene    = 4,
    Prefab   = 5,
    Shader   = 6,
    Audio    = 7,
};

std::string     assetTypeName(AssetType t);
AssetType       assetTypeFromExtension(const std::string& ext);

// ── Record ────────────────────────────────────────────────────────────────────
// One row in the assets table.
struct AssetRecord {
    UUID        uuid;
    AssetType   type         = AssetType::Unknown;
    std::string sourcePath;   // relative to project root
    std::string cookedPath;   // relative to .cache/cooked/
    std::string sourceHash;   // SHA-256 hex of source file
    uint32_t    cookVersion  = 0;
    int64_t     cookedAt     = 0; // unix timestamp, 0 = never cooked
};

// ── Registry ──────────────────────────────────────────────────────────────────
// Thin wrapper around a SQLite database.
// All methods are synchronous — call from a background thread if needed.
class AssetRegistry {
public:
    AssetRegistry() = default;
    ~AssetRegistry();

    // Open (or create) the registry at dbPath.
    // Runs migrations automatically.
    bool open(const std::filesystem::path& dbPath);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // ── CRUD ──────────────────────────────────────────────────────────────────
    bool                        insert(const AssetRecord& rec);
    bool                        update(const AssetRecord& rec);
    bool                        remove(const UUID& uuid);

    std::optional<AssetRecord>  findByUUID(const UUID& uuid)             const;
    std::optional<AssetRecord>  findBySourcePath(const std::string& rel) const;
    std::vector<AssetRecord>    findByType(AssetType type)               const;
    std::vector<AssetRecord>    all()                                    const;

    // ── Dependency tracking ───────────────────────────────────────────────────
    // Record that `asset` depends on `dependency`
    bool                        addDependency(const UUID& asset,
                                              const UUID& dependency);
    bool                        removeDependencies(const UUID& asset);

    // Who directly depends on `uuid`? (reverse lookup — for cascade invalidation)
    std::vector<UUID>           dependents(const UUID& uuid)             const;
    // What does `uuid` directly depend on?
    std::vector<UUID>           dependencies(const UUID& uuid)           const;
    // Full transitive dependents (everything that must be re-cooked)
    std::vector<UUID>           transitiveDependents(const UUID& uuid)   const;

    // ── Scanning ──────────────────────────────────────────────────────────────
    // Walk assetsRoot, assign UUIDs to new files, update hashes for changed files.
    // Returns count of new + updated records.
    int                         scan(const std::filesystem::path& assetsRoot,
                                     const std::filesystem::path& projectRoot);

private:
    void*   m_db = nullptr; // sqlite3* — opaque to keep sqlite3.h out of header
    bool    execSQL(const std::string& sql);
    void    migrate();
};

} // namespace assetlib
