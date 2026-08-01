#include "assetlib/asset_registry.h"
#include "assetlib/ddc.h"
#include <sqlite3.h>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <sys/stat.h>

namespace assetlib {

static sqlite3* db(void* p) { return static_cast<sqlite3*>(p); }

// ── Content hash ──────────────────────────────────────────────────────────────
// BLAKE3-256 (64 hex chars) — the SAME hash the DDC keys cooked output by, so
// the registry's source_hash feeds straight into DDC key computation. The old
// FNV-1a 64-bit hash (16 hex chars) was fine for local change detection but
// is unsafe to content-address a shared cache with; scan() detects the short
// legacy format and rehashes in place.
static std::string hashFile(const std::filesystem::path& p) {
    return blake3File(p);
}
static bool isLegacyHash(const std::string& h) { return h.size() != 64; }

// ── Stat helpers (Fix 1) ──────────────────────────────────────────────────────
bool AssetRegistry::statChanged(const AssetRecord& rec,
                                const std::filesystem::path& p) {
    struct ::stat st{};
    if (::stat(p.string().c_str(), &st) != 0) return true;
    return rec.sourceMtime != static_cast<int64_t>(st.st_mtime)
        || rec.sourceSize  != static_cast<int64_t>(st.st_size);
}

static void fillStat(AssetRecord& rec, const std::filesystem::path& p) {
    struct ::stat st{};
    if (::stat(p.string().c_str(), &st) == 0) {
        rec.sourceMtime = static_cast<int64_t>(st.st_mtime);
        rec.sourceSize  = static_cast<int64_t>(st.st_size);
    }
}

// ── Names ─────────────────────────────────────────────────────────────────────
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
std::string assetStateName(AssetState s) {
    switch (s) {
        case AssetState::Registered: return "registered";
        case AssetState::Importing:  return "importing";
        case AssetState::Ready:      return "ready";
        case AssetState::Stale:      return "stale";
        case AssetState::Missing:    return "missing";
        case AssetState::Failed:     return "failed";
        case AssetState::Dirty:      return "dirty";
        default:                     return "unknown";
    }
}
AssetType assetTypeFromExtension(const std::string& ext) {
    if (ext==".fbx"||ext==".obj"||ext==".glb"||ext==".gltf"||
        ext==".dae"||ext==".ply"||ext==".stl") return AssetType::Mesh;
    if (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".tga"||
        ext==".bmp"||ext==".hdr"||ext==".exr") return AssetType::Texture;
    // `.material` is the cookable unit; `.mat` predates it and has no cooker.
    if (ext==".material"||ext==".mat")           return AssetType::Material;
    if (ext==".scene")                          return AssetType::Scene;
    if (ext==".prefab")                         return AssetType::Prefab;
    // `.shader` is the COOKABLE unit — a manifest naming its .sc sources plus
    // the material interface they publish. The raw stage sources are inputs to
    // it, registered as assets but handled by no cooker of their own.
    if (ext==".shader")                         return AssetType::Shader;
    if (ext==".glsl"||ext==".sc"||ext==".hlsl") return AssetType::Shader;
    if (ext==".wav"||ext==".ogg"||ext==".mp3")  return AssetType::Audio;
    return AssetType::Unknown;
}

// ── Schema ────────────────────────────────────────────────────────────────────
static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS assets (
    uuid             TEXT    PRIMARY KEY,
    type             INTEGER NOT NULL DEFAULT 0,
    state            INTEGER NOT NULL DEFAULT 0,
    source_path      TEXT    NOT NULL,
    cooked_path      TEXT    NOT NULL DEFAULT '',
    source_hash      TEXT    NOT NULL DEFAULT '',
    source_mtime     INTEGER NOT NULL DEFAULT 0,
    source_size      INTEGER NOT NULL DEFAULT 0,
    cook_version     INTEGER NOT NULL DEFAULT 0,
    importer_version INTEGER NOT NULL DEFAULT 0,
    import_settings  TEXT    NOT NULL DEFAULT '',
    error_message    TEXT    NOT NULL DEFAULT '',
    cooked_at        INTEGER NOT NULL DEFAULT 0,
    ddc_key          TEXT    NOT NULL DEFAULT ''
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
CREATE INDEX IF NOT EXISTS idx_state       ON assets(state);
CREATE INDEX IF NOT EXISTS idx_dep_on      ON dependencies(depends_on_uuid);
)SQL";

static const char* kMigrations[] = {
    "ALTER TABLE assets ADD COLUMN state            INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN source_mtime     INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN source_size      INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN importer_version INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE assets ADD COLUMN import_settings  TEXT    NOT NULL DEFAULT '';",
    "ALTER TABLE assets ADD COLUMN error_message    TEXT    NOT NULL DEFAULT '';",
    "ALTER TABLE assets ADD COLUMN ddc_key          TEXT    NOT NULL DEFAULT '';",
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────
AssetRegistry::~AssetRegistry() { close(); }

bool AssetRegistry::open(const std::filesystem::path& dbPath) {
    std::filesystem::create_directories(dbPath.parent_path());
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &raw) != SQLITE_OK)
        { sqlite3_close(raw); return false; }
    m_db = raw;
    sqlite3_exec(db(m_db), "PRAGMA foreign_keys  = ON;",     nullptr,nullptr,nullptr);
    sqlite3_exec(db(m_db), "PRAGMA journal_mode  = WAL;",    nullptr,nullptr,nullptr);
    sqlite3_exec(db(m_db), "PRAGMA synchronous   = NORMAL;", nullptr,nullptr,nullptr);
    // Connections open mid-cook (scene tasks on the worker pool) while the
    // drain lane writes — retry briefly on lock contention instead of
    // failing a read that would have succeeded 5ms later.
    sqlite3_busy_timeout(db(m_db), 5000);
    migrate();
    return true;
}
void AssetRegistry::close() {
    if (m_db) { sqlite3_close(db(m_db)); m_db = nullptr; }
}
bool AssetRegistry::execSQL(const std::string& sql) {
    char* err = nullptr;
    bool ok = sqlite3_exec(db(m_db), sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok;
}
void AssetRegistry::migrate() {
    sqlite3_exec(db(m_db), kSchema, nullptr, nullptr, nullptr);
    for (const char* m : kMigrations)
        sqlite3_exec(db(m_db), m, nullptr, nullptr, nullptr);
}

// ── Row helper ────────────────────────────────────────────────────────────────
static const char* kCols =
    "uuid,type,state,source_path,cooked_path,source_hash,"
    "source_mtime,source_size,cook_version,importer_version,"
    "import_settings,error_message,cooked_at,ddc_key";

static AssetRecord rowToRecord(sqlite3_stmt* s) {
    AssetRecord r;
    auto txt = [&](int col) -> std::string {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s, col));
        return p ? p : "";
    };
    r.uuid              = UUID::fromString(txt(0));
    r.type              = static_cast<AssetType> (sqlite3_column_int  (s,1));
    r.state             = static_cast<AssetState>(sqlite3_column_int  (s,2));
    r.sourcePath        = txt(3);
    r.cookedPath        = txt(4);
    r.sourceHash        = txt(5);
    r.sourceMtime       = sqlite3_column_int64(s,6);
    r.sourceSize        = sqlite3_column_int64(s,7);
    r.cookVersion       = static_cast<uint32_t>(sqlite3_column_int(s,8));
    r.importerVersion   = static_cast<uint32_t>(sqlite3_column_int(s,9));
    r.importSettings.json = txt(10);
    r.errorMessage      = txt(11);
    r.cookedAt          = sqlite3_column_int64(s,12);
    r.ddcKey            = txt(13);
    return r;
}

// ── CRUD ──────────────────────────────────────────────────────────────────────
bool AssetRegistry::insert(const AssetRecord& r) {
    const char* sql =
        "INSERT INTO assets"
        "(uuid,type,state,source_path,cooked_path,source_hash,"
        " source_mtime,source_size,cook_version,importer_version,"
        " import_settings,error_message,cooked_at,ddc_key)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(uuid) DO UPDATE SET"
        "  type=excluded.type, state=excluded.state,"
        "  source_path=excluded.source_path,"
        "  cooked_path=excluded.cooked_path,"
        "  source_hash=excluded.source_hash,"
        "  source_mtime=excluded.source_mtime,"
        "  source_size=excluded.source_size,"
        "  cook_version=excluded.cook_version,"
        "  importer_version=excluded.importer_version,"
        "  import_settings=excluded.import_settings,"
        "  error_message=excluded.error_message,"
        "  cooked_at=excluded.cooked_at,"
        "  ddc_key=excluded.ddc_key;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql,-1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto us = r.uuid.toString();
    sqlite3_bind_text (stmt,1,  us.c_str(),                    -1,SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt,2,  static_cast<int>(r.type));
    sqlite3_bind_int  (stmt,3,  static_cast<int>(r.state));
    sqlite3_bind_text (stmt,4,  r.sourcePath.c_str(),          -1,SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt,5,  r.cookedPath.c_str(),          -1,SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt,6,  r.sourceHash.c_str(),          -1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,7,  r.sourceMtime);
    sqlite3_bind_int64(stmt,8,  r.sourceSize);
    sqlite3_bind_int  (stmt,9,  static_cast<int>(r.cookVersion));
    sqlite3_bind_int  (stmt,10, static_cast<int>(r.importerVersion));
    sqlite3_bind_text (stmt,11, r.importSettings.json.c_str(), -1,SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt,12, r.errorMessage.c_str(),        -1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,13, r.cookedAt);
    sqlite3_bind_text (stmt,14, r.ddcKey.c_str(),              -1,SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}
bool AssetRegistry::update(const AssetRecord& r) { return insert(r); }

bool AssetRegistry::remove(const UUID& uuid) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "DELETE FROM assets WHERE uuid=?;",-1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto s=uuid.toString();
    sqlite3_bind_text(stmt,1,s.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}

bool AssetRegistry::setState(const UUID& uuid, AssetState state,
                             const std::string& errorMsg) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "UPDATE assets SET state=?,error_message=? WHERE uuid=?;",
        -1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto s=uuid.toString();
    sqlite3_bind_int (stmt,1,static_cast<int>(state));
    sqlite3_bind_text(stmt,2,errorMsg.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,3,s.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}

// ── Queries ───────────────────────────────────────────────────────────────────
static std::optional<AssetRecord> stepOne(sqlite3_stmt* s) {
    std::optional<AssetRecord> r;
    if (sqlite3_step(s)==SQLITE_ROW) r=rowToRecord(s);
    sqlite3_finalize(s); return r;
}
static std::vector<AssetRecord> stepAll(sqlite3_stmt* s) {
    std::vector<AssetRecord> v;
    while (sqlite3_step(s)==SQLITE_ROW) v.push_back(rowToRecord(s));
    sqlite3_finalize(s); return v;
}

std::optional<AssetRecord> AssetRegistry::findByUUID(const UUID& uuid) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE uuid=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    auto s=uuid.toString();
    sqlite3_bind_text(stmt,1,s.c_str(),-1,SQLITE_TRANSIENT);
    return stepOne(stmt);
}
std::optional<AssetRecord> AssetRegistry::findBySourcePath(const std::string& rel) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE source_path=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_text(stmt,1,rel.c_str(),-1,SQLITE_TRANSIENT);
    return stepOne(stmt);
}
std::vector<AssetRecord> AssetRegistry::findByType(AssetType type) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE type=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_int(stmt,1,static_cast<int>(type));
    return stepAll(stmt);
}
std::vector<AssetRecord> AssetRegistry::findByState(AssetState state) const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets WHERE state=?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_int(stmt,1,static_cast<int>(state));
    return stepAll(stmt);
}
std::vector<AssetRecord> AssetRegistry::all() const {
    std::string sql=std::string("SELECT ")+kCols+" FROM assets;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),sql.c_str(),-1,&stmt,nullptr)!=SQLITE_OK) return {};
    return stepAll(stmt);
}

// ── Dependencies ──────────────────────────────────────────────────────────────
bool AssetRegistry::wouldCreateCycle(const UUID& from, const UUID& to) const {
    std::unordered_set<std::string> visited;
    std::vector<UUID> stack={to};
    while (!stack.empty()) {
        UUID cur=stack.back(); stack.pop_back();
        auto s=cur.toString();
        if (s==from.toString()) return true;
        if (!visited.insert(s).second) continue;
        for (const UUID& d:dependencies(cur)) stack.push_back(d);
    }
    return false;
}
bool AssetRegistry::addDependency(const UUID& asset, const UUID& dep) {
    if (wouldCreateCycle(asset,dep)) {
        std::fprintf(stderr,"[AssetLib] Cycle rejected: %s -> %s\n",
            asset.toString().c_str(), dep.toString().c_str());
        return false;
    }
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "INSERT OR IGNORE INTO dependencies(asset_uuid,depends_on_uuid) VALUES(?,?);",
        -1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto a=asset.toString(), d=dep.toString();
    sqlite3_bind_text(stmt,1,a.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,d.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}
bool AssetRegistry::removeDependencies(const UUID& asset) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db(m_db),
        "DELETE FROM dependencies WHERE asset_uuid=?;",
        -1,&stmt,nullptr)!=SQLITE_OK) return false;
    auto a=asset.toString();
    sqlite3_bind_text(stmt,1,a.c_str(),-1,SQLITE_TRANSIENT);
    bool ok=sqlite3_step(stmt)==SQLITE_DONE;
    sqlite3_finalize(stmt); return ok;
}
static std::vector<UUID> queryUUIDs(sqlite3* d,const char* sql,const std::string& bind) {
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(d,sql,-1,&stmt,nullptr)!=SQLITE_OK) return {};
    sqlite3_bind_text(stmt,1,bind.c_str(),-1,SQLITE_TRANSIENT);
    std::vector<UUID> out;
    while (sqlite3_step(stmt)==SQLITE_ROW)
        out.push_back(UUID::fromString(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt,0))));
    sqlite3_finalize(stmt); return out;
}
std::vector<UUID> AssetRegistry::dependents(const UUID& uuid) const {
    return queryUUIDs(db(m_db),
        "SELECT asset_uuid FROM dependencies WHERE depends_on_uuid=?;",
        uuid.toString());
}
std::vector<UUID> AssetRegistry::dependencies(const UUID& uuid) const {
    return queryUUIDs(db(m_db),
        "SELECT depends_on_uuid FROM dependencies WHERE asset_uuid=?;",
        uuid.toString());
}
std::vector<UUID> AssetRegistry::transitiveDependents(const UUID& uuid) const {
    std::vector<UUID> result;
    std::unordered_set<std::string> visited;
    std::vector<UUID> queue={uuid};
    while (!queue.empty()) {
        UUID cur=queue.back(); queue.pop_back();
        auto s=cur.toString();
        if (!visited.insert(s).second) continue;
        for (const UUID& d:dependents(cur)) { result.push_back(d); queue.push_back(d); }
    }
    return result;
}

// ── Scanner ───────────────────────────────────────────────────────────────────
int AssetRegistry::scan(const std::filesystem::path& assetsRoot,
                        const std::filesystem::path& projectRoot) {
    int changed=0;
    std::unordered_set<std::string> seen;

    // Wrap all writes in one transaction — N individual writes become 1.
    // On a 10,000-asset project this is the difference between 10s and 100ms.
    // RAII: the scan walks the filesystem and can throw (or return early), and
    // an abandoned BEGIN leaves this connection stuck in an open transaction
    // for its whole lifetime — every later write fails and the DB looks
    // locked. The guard rolls back unless we reach the explicit commit.
    struct Transaction {
        sqlite3* db;
        bool     done = false;
        explicit Transaction(sqlite3* d) : db(d) {
            sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
        }
        void commit() {
            if (done) return;
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            done = true;
        }
        ~Transaction() {
            if (!done) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    } txn(db(m_db));

    // Move detection needs to look up records by content hash. Querying all()
    // per new file was O(new × total) full-table scans — a 10k-asset first
    // scan meant 10k SELECT *, i.e. 100M rows walked. Snapshot ONCE into a
    // hash index instead, built lazily so an up-to-date tree never pays for
    // it. `claimed` keeps one record from being re-pointed by two different
    // files: the old per-file all() re-read picked that up via the `seen`
    // check, a snapshot cannot.
    std::unordered_map<std::string, std::vector<AssetRecord>> byHash;
    std::unordered_set<std::string> claimed;
    bool haveIndex = false;
    auto hashIndex = [&]() -> decltype(byHash)& {
        if (!haveIndex) {
            for (auto& rec : all()) byHash[rec.sourceHash].push_back(rec);
            haveIndex = true;
        }
        return byHash;
    };

    // error_code overload: an unreadable directory mid-walk must not throw
    // past the transaction guard (it would still roll back, but a scan that
    // reports what it managed to do beats one that unwinds into the caller).
    std::error_code walkEc;
    for (auto& entry : std::filesystem::recursive_directory_iterator(
             assetsRoot, std::filesystem::directory_options::skip_permission_denied,
             walkEc)) {
        if (!entry.is_regular_file()) continue;
        auto ext=entry.path().extension().string();
        for (auto& c:ext) c=static_cast<char>(std::tolower(c));
        if (assetTypeFromExtension(ext)==AssetType::Unknown) continue;

        auto rel=std::filesystem::relative(entry.path(),projectRoot).string();
        seen.insert(rel);
        auto existing=findBySourcePath(rel);

        if (existing) {
            // Fast skip only when the stat is unchanged AND the stored hash is
            // already BLAKE3 — legacy FNV hashes are upgraded in place once.
            if (!statChanged(*existing, entry.path())
                    && !isLegacyHash(existing->sourceHash)) continue;
            auto newHash=hashFile(entry.path());
            if (newHash==existing->sourceHash) {
                fillStat(*existing,entry.path()); update(*existing); // touch only
            } else {
                existing->sourceHash=newHash;
                existing->state=AssetState::Stale;
                fillStat(*existing,entry.path());
                update(*existing);
                std::printf("[AssetLib] Stale: %s\n",rel.c_str());
                ++changed;
            }
        } else {
            auto newHash=hashFile(entry.path());

            // Moved/renamed file? Same content hash as a record whose source
            // file is gone — re-point that record instead of minting a new
            // UUID, so scene references (which key on UUID) survive renames.
            std::optional<AssetRecord> moved;
            auto& index = hashIndex();
            if (auto it = index.find(newHash); it != index.end()) {
                for (auto& rec : it->second) {
                    if (rec.sourcePath==rel) continue;
                    if (claimed.count(rec.uuid.toString())) continue;  // taken
                    if (seen.count(rec.sourcePath)) continue;          // still present
                    if (std::filesystem::exists(projectRoot/rec.sourcePath)) continue;
                    moved=rec; break;
                }
            }
            if (moved) {
                claimed.insert(moved->uuid.toString());
                std::printf("[AssetLib] Moved: %s -> %s (%s)\n",
                            moved->sourcePath.c_str(),rel.c_str(),
                            moved->uuid.toString().c_str());
                moved->sourcePath=rel;
                // Content is identical, so an existing cooked binary is still
                // valid; only revive records previously marked Missing.
                if (moved->state==AssetState::Missing)
                    moved->state=moved->cookedPath.empty()
                        ? AssetState::Registered : AssetState::Ready;
                fillStat(*moved,entry.path());
                update(*moved);
                ++changed;
                continue;
            }

            AssetRecord rec;
            rec.uuid=UUID::generate(); rec.type=assetTypeFromExtension(ext);
            rec.state=AssetState::Registered; rec.sourcePath=rel;
            rec.sourceHash=newHash;
            fillStat(rec,entry.path());
            insert(rec);
            std::printf("[AssetLib] New: %s -> %s\n",rel.c_str(),rec.uuid.toString().c_str());
            ++changed;
        }
    }
    // Mark deleted files as Missing
    for (auto& rec:all()) {
        if (rec.state==AssetState::Missing||seen.count(rec.sourcePath)) continue;
        setState(rec.uuid,AssetState::Missing);
        std::printf("[AssetLib] Missing: %s\n",rec.sourcePath.c_str());
        ++changed;
    }
    txn.commit();
    if (walkEc)
        std::printf("[AssetLib] scan: %s (partial scan committed)\n",
                    walkEc.message().c_str());
    return changed;
}

} // namespace assetlib
