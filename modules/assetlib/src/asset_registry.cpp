#include "assetlib/asset_registry.h"
#include <sqlite3.h>
#include <cassert>
#include <ctime>
#include <algorithm>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace assetlib {

// ── Helpers ───────────────────────────────────────────────────────────────────
static sqlite3* db(void* p) { return static_cast<sqlite3*>(p); }

static std::string hashFile(const std::filesystem::path& p) {
    // Simple FNV-1a 64 hash — no OpenSSL dependency
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    uint64_t hash = 14695981039346656037ULL;
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount()) {
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            hash ^= static_cast<uint8_t>(buf[i]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
}

static AssetRecord rowToRecord(sqlite3_stmt* stmt) {
    AssetRecord r;
    r.uuid        = UUID::fromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    r.type        = static_cast<AssetType>(sqlite3_column_int(stmt, 1));
    r.sourcePath  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    auto cooked   = sqlite3_column_text(stmt, 3);
    r.cookedPath  = cooked ? reinterpret_cast<const char*>(cooked) : "";
    auto hash     = sqlite3_column_text(stmt, 4);
    r.sourceHash  = hash ? reinterpret_cast<const char*>(hash) : "";
    r.cookVersion = sqlite3_column_int(stmt, 5);
    r.cookedAt    = sqlite3_column_int64(stmt, 6);
    return r;
}

// ── Schema ────────────────────────────────────────────────────────────────────
static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS assets (
    uuid         TEXT PRIMARY KEY,
    type         INTEGER NOT NULL DEFAULT 0,
    source_path  TEXT NOT NULL,
    cooked_path  TEXT,
    source_hash  TEXT,
    cook_version INTEGER NOT NULL DEFAULT 0,
    cooked_at    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS dependencies (
    asset_uuid      TEXT NOT NULL,
    depends_on_uuid TEXT NOT NULL,
    PRIMARY KEY (asset_uuid, depends_on_uuid),
    FOREIGN KEY (asset_uuid)      REFERENCES assets(uuid) ON DELETE CASCADE,
    FOREIGN KEY (depends_on_uuid) REFERENCES assets(uuid) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_source_path ON assets(source_path);
CREATE INDEX IF NOT EXISTS idx_type        ON assets(type);
CREATE INDEX IF NOT EXISTS idx_dep_on      ON dependencies(depends_on_uuid);
)SQL";

// ── Lifecycle ─────────────────────────────────────────────────────────────────
AssetRegistry::~AssetRegistry() { close(); }

bool AssetRegistry::open(const std::filesystem::path& dbPath) {
    std::filesystem::create_directories(dbPath.parent_path());
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &raw) != SQLITE_OK) {
        sqlite3_close(raw);
        return false;
    }
    m_db = raw;
    sqlite3_exec(db(m_db), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db(m_db), "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    migrate();
    return true;
}

void AssetRegistry::close() {
    if (m_db) { sqlite3_close(db(m_db)); m_db = nullptr; }
}

void AssetRegistry::migrate() {
    sqlite3_exec(db(m_db), kSchema, nullptr, nullptr, nullptr);
}

bool AssetRegistry::execSQL(const std::string& sql) {
    char* err = nullptr;
    bool ok = sqlite3_exec(db(m_db), sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    if (!ok && err) { sqlite3_free(err); }
    return ok;
}

// ── CRUD ──────────────────────────────────────────────────────────────────────
bool AssetRegistry::insert(const AssetRecord& r) {
    const char* sql =
        "INSERT OR REPLACE INTO assets "
        "(uuid, type, source_path, cooked_path, source_hash, cook_version, cooked_at) "
        "VALUES (?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    auto s = r.uuid.toString();
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, static_cast<int>(r.type));
    sqlite3_bind_text(stmt, 3, r.sourcePath.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, r.cookedPath.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, r.sourceHash.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 6, static_cast<int>(r.cookVersion));
    sqlite3_bind_int64(stmt, 7, r.cookedAt);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool AssetRegistry::update(const AssetRecord& r) { return insert(r); }

bool AssetRegistry::remove(const UUID& uuid) {
    const char* sql = "DELETE FROM assets WHERE uuid = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    auto s = uuid.toString();
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static std::optional<AssetRecord> stepOne(sqlite3_stmt* stmt) {
    std::optional<AssetRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = rowToRecord(stmt);
    sqlite3_finalize(stmt);
    return result;
}

static std::vector<AssetRecord> stepAll(sqlite3_stmt* stmt) {
    std::vector<AssetRecord> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(rowToRecord(stmt));
    sqlite3_finalize(stmt);
    return out;
}

std::optional<AssetRecord> AssetRegistry::findByUUID(const UUID& uuid) const {
    const char* sql =
        "SELECT uuid,type,source_path,cooked_path,source_hash,cook_version,cooked_at "
        "FROM assets WHERE uuid=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    auto s = uuid.toString();
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    return stepOne(stmt);
}

std::optional<AssetRecord> AssetRegistry::findBySourcePath(const std::string& rel) const {
    const char* sql =
        "SELECT uuid,type,source_path,cooked_path,source_hash,cook_version,cooked_at "
        "FROM assets WHERE source_path=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_text(stmt, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    return stepOne(stmt);
}

std::vector<AssetRecord> AssetRegistry::findByType(AssetType type) const {
    const char* sql =
        "SELECT uuid,type,source_path,cooked_path,source_hash,cook_version,cooked_at "
        "FROM assets WHERE type=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int(stmt, 1, static_cast<int>(type));
    return stepAll(stmt);
}

std::vector<AssetRecord> AssetRegistry::all() const {
    const char* sql =
        "SELECT uuid,type,source_path,cooked_path,source_hash,cook_version,cooked_at "
        "FROM assets;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    return stepAll(stmt);
}

// ── Dependencies ──────────────────────────────────────────────────────────────
bool AssetRegistry::addDependency(const UUID& asset, const UUID& dep) {
    const char* sql =
        "INSERT OR IGNORE INTO dependencies(asset_uuid, depends_on_uuid) VALUES(?,?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    auto a = asset.toString(), d = dep.toString();
    sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool AssetRegistry::removeDependencies(const UUID& asset) {
    const char* sql = "DELETE FROM dependencies WHERE asset_uuid=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    auto a = asset.toString();
    sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<UUID> AssetRegistry::dependents(const UUID& uuid) const {
    const char* sql = "SELECT asset_uuid FROM dependencies WHERE depends_on_uuid=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    auto s = uuid.toString();
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<UUID> out;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back(UUID::fromString(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))));
    sqlite3_finalize(stmt);
    return out;
}

std::vector<UUID> AssetRegistry::dependencies(const UUID& uuid) const {
    const char* sql = "SELECT depends_on_uuid FROM dependencies WHERE asset_uuid=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db), sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    auto s = uuid.toString();
    sqlite3_bind_text(stmt, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<UUID> out;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back(UUID::fromString(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))));
    sqlite3_finalize(stmt);
    return out;
}

std::vector<UUID> AssetRegistry::transitiveDependents(const UUID& uuid) const {
    // BFS over the dependents graph
    std::vector<UUID> result;
    std::unordered_set<std::string> visited;
    std::vector<UUID> queue = { uuid };
    while (!queue.empty()) {
        UUID cur = queue.back(); queue.pop_back();
        auto s = cur.toString();
        if (!visited.insert(s).second) continue;
        for (const UUID& dep : dependents(cur)) {
            result.push_back(dep);
            queue.push_back(dep);
        }
    }
    return result;
}

// ── Scanner ───────────────────────────────────────────────────────────────────
std::string assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Mesh:     return "mesh";
        case AssetType::Texture:  return "texture";
        case AssetType::Material: return "material";
        case AssetType::Scene:    return "scene";
        case AssetType::Prefab:   return "prefab";
        case AssetType::Shader:   return "shader";
        case AssetType::Audio:    return "audio";
        default:                  return "unknown";
    }
}

AssetType assetTypeFromExtension(const std::string& ext) {
    if (ext==".fbx"||ext==".obj"||ext==".glb"||ext==".gltf"||
        ext==".dae"||ext==".ply"||ext==".stl") return AssetType::Mesh;
    if (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".tga"||
        ext==".bmp"||ext==".hdr"||ext==".exr") return AssetType::Texture;
    if (ext==".mat")                            return AssetType::Material;
    if (ext==".scene")                          return AssetType::Scene;
    if (ext==".prefab")                         return AssetType::Prefab;
    if (ext==".glsl"||ext==".sc"||ext==".hlsl") return AssetType::Shader;
    if (ext==".wav"||ext==".ogg"||ext==".mp3")  return AssetType::Audio;
    return AssetType::Unknown;
}

int AssetRegistry::scan(const std::filesystem::path& assetsRoot,
                        const std::filesystem::path& projectRoot) {
    int count = 0;
    for (auto& entry : std::filesystem::recursive_directory_iterator(
             assetsRoot, std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;

        auto ext  = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        auto type = assetTypeFromExtension(ext);
        if (type == AssetType::Unknown) continue;

        auto relPath = std::filesystem::relative(entry.path(), projectRoot).string();
        auto hash    = hashFile(entry.path());

        auto existing = findBySourcePath(relPath);
        if (existing) {
            // Already registered — update hash if changed
            if (existing->sourceHash != hash) {
                existing->sourceHash = hash;
                update(*existing);
                ++count;
                std::printf("[AssetLib] Updated: %s\n", relPath.c_str());
            }
        } else {
            // New file — assign fresh UUID
            AssetRecord rec;
            rec.uuid       = UUID::generate();
            rec.type       = type;
            rec.sourcePath = relPath;
            rec.sourceHash = hash;
            insert(rec);
            ++count;
            std::printf("[AssetLib] Registered: %s → %s\n",
                        relPath.c_str(), rec.uuid.toString().c_str());
        }
    }
    return count;
}

} // namespace assetlib
