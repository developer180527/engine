#include "assetlib/ddc.h"
#include "blake3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <process.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif

namespace assetlib {

namespace fs = std::filesystem;

namespace {

// Blobs are stored read-only (see store()). POSIX honours the containing
// directory's write permission when unlinking, so remove() just works — but
// Windows refuses to delete a FILE_ATTRIBUTE_READONLY file outright. Clearing
// the attribute first is what makes eviction and replacement portable;
// without it every blob removal on Windows fails silently and the cache grows
// without bound.
void removeBlob(const fs::path& p, std::error_code& ec) {
#if defined(_WIN32)
    const DWORD attrs = ::GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(p.wstring().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
#endif
    fs::remove(p, ec);
}

// Record "this blob was used just now" as its mtime, so GC's LRU order reflects
// USE and not ingest time. Best-effort and deliberately silent: the store stays
// correct if the touch fails (a read-only mount, a foreign owner), it just
// evicts in ingest order for that blob. Blobs are 0444, which is fine — setting
// mtime needs ownership, not write permission, and we own what we ingested.
void touchForLru(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
}

// Mark a finished blob immutable.
void makeReadOnly(const fs::path& p) {
#if defined(_WIN32)
    ::SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(p.string().c_str(), 0444);
#endif
}

} // namespace

// ── Hashing ───────────────────────────────────────────────────────────────────

static std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i*2]   = k[d[i] >> 4];
        s[i*2+1] = k[d[i] & 0xf];
    }
    return s;
}

std::string blake3File(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    blake3_hasher h;
    blake3_hasher_init(&h);
    char buf[1 << 16];
    while (f.read(buf, sizeof(buf)) || f.gcount())
        blake3_hasher_update(&h, buf, (size_t)f.gcount());
    if (f.bad()) return "";
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

std::string blake3Bytes(const void* data, size_t len) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

std::string computeDdcKey(const DdcKeyInputs& in) {
    // Canonical, unambiguous byte stream: length-prefixed fields so no
    // concatenation of two different input sets can collide ("ab"+"c" vs
    // "a"+"bc"). Bump the prefix if the key recipe itself ever changes.
    blake3_hasher h;
    blake3_hasher_init(&h);
    auto field = [&](const void* d, size_t n) {
        uint64_t len = (uint64_t)n;
        blake3_hasher_update(&h, &len, sizeof(len));
        blake3_hasher_update(&h, d, n);
    };
    auto str = [&](const std::string& s) { field(s.data(), s.size()); };

    str("engine-ddc-v1");
    str(in.cookerId);
    field(&in.cookerVersion, sizeof(in.cookerVersion));
    str(in.settings);
    str(in.sourceHash);
    auto deps = in.depHashes;                  // order-independent
    std::sort(deps.begin(), deps.end());
    for (const auto& d : deps) str(d);

    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

// ── Store ─────────────────────────────────────────────────────────────────────

fs::path DdcStore::defaultLocalRoot() {
    if (const char* v = std::getenv("ENGINE_DDC"); v && *v) return v;
#if defined(_WIN32)
    if (const char* v = std::getenv("LOCALAPPDATA"); v && *v)
        return fs::path(v) / "engine" / "ddc";
#endif
    if (const char* v = std::getenv("HOME"); v && *v)
        return fs::path(v) / ".engine" / "ddc";
    return fs::temp_directory_path() / "engine-ddc";
}

fs::path DdcStore::sharedRootFromEnv() {
    if (const char* v = std::getenv("ENGINE_DDC_SHARED"); v && *v) return v;
    return {};
}

DdcStore::DdcStore(fs::path localRoot, fs::path sharedRoot)
    : m_local(localRoot.empty() ? defaultLocalRoot() : std::move(localRoot))
    , m_shared(sharedRoot.empty() ? sharedRootFromEnv() : std::move(sharedRoot)) {
    std::error_code ec;
    fs::create_directories(m_local, ec);
    if (ec)
        std::fprintf(stderr, "[DDC] cannot create local store %s: %s\n",
                     m_local.string().c_str(), ec.message().c_str());
    // Deliberately do NOT create the shared root — if the mount is absent we
    // must degrade to local-only, not scribble a directory onto the mount
    // point and shadow the real cache when it comes back.
}

fs::path DdcStore::blobPath(const fs::path& root, const std::string& key) {
    // Fan out on the first byte so no directory collects millions of entries.
    return root / key.substr(0, 2) / (key + ".blob");
}

namespace {
// A temp name unique across processes AND threads. Both must be in it: two
// cook workers on one machine share a pid-less name, and two graph workers
// storing the SAME key concurrently (duplicate assets, or two meshes with
// byte-identical embedded textures) share a thread-less name — either
// collision means both write the same file and one ships a truncated blob.
fs::path uniqueTempPath(const fs::path& dir, const std::string& key,
                        const char* tag) {
    static std::atomic<uint64_t> ctr{0};
#if defined(_WIN32)
    const uint64_t pid = (uint64_t)::_getpid();
#else
    const uint64_t pid = (uint64_t)::getpid();
#endif
    return dir / (key + "." + tag + "." + std::to_string(pid)
                      + "." + std::to_string(ctr.fetch_add(1)));
}
} // namespace

bool DdcStore::contains(const std::string& key) const {
    if (key.empty()) return false;
    std::error_code ec;
    if (fs::exists(blobPath(m_local, key), ec)) return true;
    return !m_shared.empty() && fs::exists(blobPath(m_shared, key), ec);
}

bool DdcStore::ingest(const fs::path& root, const std::string& key,
                      const fs::path& src) const {
    std::error_code ec;
    const fs::path blob = blobPath(root, key);
    if (fs::exists(blob, ec)) return true;     // first writer already won
    fs::create_directories(blob.parent_path(), ec);
    if (ec) return false;

    // Copy to a private temp name IN the destination directory, then rename:
    // rename is atomic on the same filesystem, so a concurrent reader can
    // never see a half-written blob — it sees nothing, or the whole thing.
    const fs::path tmp = uniqueTempPath(blob.parent_path(), key, "ingest");

    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    // Blobs are immutable — read-only so a stray ofstream (or a cooker handed
    // a hardlinked path by mistake) fails to open rather than corrupting the
    // cache for every project sharing it.
    makeReadOnly(tmp);
    fs::rename(tmp, blob, ec);
    if (ec) {
        removeBlob(tmp, ec);              // read-only by now: needs the helper
        std::error_code ec2;
        return fs::exists(blob, ec2);          // raced with another writer: fine
    }
    return true;
}

bool DdcStore::materialize(const fs::path& blob, const fs::path& dst) const {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    removeBlob(dst, ec);                        // replace, never write-through
    fs::create_hard_link(blob, dst, ec);        // zero-copy on same volume
    if (!ec) return true;
    ec.clear();
    fs::copy_file(blob, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool DdcStore::fetch(const std::string& key, const fs::path& dst) {
    if (key.empty()) return false;
    std::error_code ec;

    const fs::path local = blobPath(m_local, key);
    if (fs::exists(local, ec)) {
        if (materialize(local, dst)) { touchForLru(local); ++m_localHits; return true; }
        return false;
    }

    if (!m_shared.empty()) {
        const fs::path shared = blobPath(m_shared, key);
        if (fs::exists(shared, ec)) {
            // Promote into the local tier first, then materialize from local —
            // the next fetch of this key never touches the network again.
            if (ingest(m_local, key, shared) && materialize(local, dst)) {
                ++m_sharedHits;
                return true;
            }
            // Promotion failed (local disk full?) — serve straight from shared.
            if (materialize(shared, dst)) { ++m_sharedHits; return true; }
            return false;
        }
    }
    ++m_misses;
    return false;
}

bool DdcStore::store(const std::string& key, const fs::path& src) {
    if (key.empty()) return false;
    if (!ingest(m_local, key, src)) return false;
    ++m_stores;
    if (!m_shared.empty()) {
        std::error_code ec;
        if (fs::exists(m_shared, ec) && !ingest(m_shared, key, src))
            std::fprintf(stderr, "[DDC] shared push failed for %s (local copy "
                         "intact)\n", key.c_str());
    }
    return true;
}

bool DdcStore::storeBytes(const std::string& key, const std::string& bytes) {
    if (key.empty()) return false;
    // Spill to a private temp file, then reuse the file ingest path (same
    // atomicity, same tiering).
    std::error_code ec;
    fs::create_directories(m_local, ec);
    const fs::path tmp = uniqueTempPath(m_local, key, "bytes");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(bytes.data(), (std::streamsize)bytes.size())) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    const bool ok = store(key, tmp);
    fs::remove(tmp, ec);
    return ok;
}

bool DdcStore::fetchBytes(const std::string& key, std::string& out) {
    if (key.empty()) return false;
    auto read = [&](const fs::path& blob) -> bool {
        std::ifstream f(blob, std::ios::binary);
        if (!f) return false;
        std::string s((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
        if (f.bad()) return false;
        out = std::move(s);
        return true;
    };
    std::error_code ec;
    const fs::path local = blobPath(m_local, key);
    if (fs::exists(local, ec) && read(local)) {
        touchForLru(local); ++m_localHits; return true;
    }
    if (!m_shared.empty()) {
        const fs::path shared = blobPath(m_shared, key);
        if (fs::exists(shared, ec) && read(shared)) {
            ingest(m_local, key, shared);          // promote, best-effort
            ++m_sharedHits;
            return true;
        }
    }
    ++m_misses;
    return false;
}

void DdcStore::evictLocal(const std::string& key) {
    if (key.empty()) return;
    std::error_code ec;
    removeBlob(blobPath(m_local, key), ec);
}

DdcStore::Stats DdcStore::stats() const {
    return { m_localHits.load(), m_sharedHits.load(),
             m_misses.load(),    m_stores.load() };
}

// ── Garbage collection ───────────────────────────────────────────────────────

uint64_t DdcStore::budgetBytesFromEnv() {
    if (const char* v = std::getenv("ENGINE_DDC_MAX_MB"); v && *v) {
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(v, &end, 10);
        if (end != v) return (uint64_t)mb << 20;      // 0 is legal: unbounded
    }
    return kDefaultBudgetMb << 20;
}

DdcStore::GcStats DdcStore::collectGarbage(uint64_t maxBytes, bool prune) {
    GcStats st;
    if (m_local.empty()) return st;

    struct Entry {
        fs::path            path;
        uint64_t            bytes = 0;
        fs::file_time_type  used{};
    };
    std::vector<Entry> evictable;

    std::error_code ec;
    // Two levels: <root>/<2 hex>/<key>.blob. A non-recursive walk of the fan-out
    // dirs keeps this from wandering into anything else that shares the root.
    for (const auto& bucket : fs::directory_iterator(m_local, ec)) {
        if (!bucket.is_directory(ec)) continue;
        for (const auto& e : fs::directory_iterator(bucket.path(), ec)) {
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".blob") continue;   // skip *.ingest temps

            std::error_code sizeEc, linkEc, timeEc;
            const uint64_t bytes = (uint64_t)fs::file_size(e.path(), sizeEc);
            if (sizeEc) continue;
            ++st.blobs;
            st.totalBytes += bytes;

            // Hardlinked into a project's .cache: unlinking here reclaims
            // nothing, because the project's link keeps the inode alive.
            const uintmax_t links = fs::hard_link_count(e.path(), linkEc);
            if (!linkEc && links > 1) { st.pinnedBytes += bytes; continue; }

            const auto used = fs::last_write_time(e.path(), timeEc);
            evictable.push_back({ e.path(), bytes,
                                  timeEc ? fs::file_time_type{} : used });
        }
    }

    if (maxBytes == 0 || st.totalBytes <= maxBytes) return st;   // unbounded / fits
    st.overBudgetBytes = st.totalBytes - maxBytes;

    // Oldest use first.
    std::sort(evictable.begin(), evictable.end(),
              [](const Entry& a, const Entry& b) { return a.used < b.used; });

    // Evict until the TOTAL (pinned included — those bytes are really on the
    // disk) is under budget. If pinned data alone exceeds the budget we cannot
    // reach it; report honestly rather than deleting everything unpinned in a
    // futile attempt.
    uint64_t live = st.totalBytes;
    for (const Entry& e : evictable) {
        if (live <= maxBytes) break;
        if (prune) {
            std::error_code delEc;
            removeBlob(e.path, delEc);          // 0444: needs the helper
            if (delEc) continue;                // someone else got it, or in use
            ++st.deleted;
            st.freedBytes += e.bytes;
        }
        live -= e.bytes;
    }
    return st;
}

} // namespace assetlib
