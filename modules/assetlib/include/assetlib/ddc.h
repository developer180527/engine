#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>

namespace assetlib {

// ── Content hashing (BLAKE3-256, hex) ─────────────────────────────────────────
// THE hash for asset identity and DDC keys. Cryptographic strength is not
// paranoia here: on a SHARED cache a collision silently serves the wrong
// cooked bytes to every machine in the studio. (FNV-1a remains acceptable
// only for local change *detection*, never for content *addressing*.)
std::string blake3File(const std::filesystem::path& p);   // "" on I/O error
std::string blake3Bytes(const void* data, size_t len);

// ── DDC key ───────────────────────────────────────────────────────────────────
// The cooked output of an asset is a pure function of these inputs. Hash them
// and the hash names the output — that is the entire trick. Per-cooker id +
// version live in the key, so bumping the texture cooker never invalidates a
// single cooked mesh (the old global kCurrentCookVersion recooked the world).
struct DdcKeyInputs {
    std::string cookerId;                 // stable, e.g. "mesh", "texture"
    uint32_t    cookerVersion = 0;        // bump on output-format/logic change
    std::string settings;                 // cooker knobs that alter output
                                          // (quality tier, per-asset flags…)
    std::string sourceHash;               // blake3 of the source file bytes
    std::vector<std::string> depHashes;   // hashes of inputs BEYOND the source
                                          // file (sorted internally; reserved
                                          // for multi-file sources)
};
std::string computeDdcKey(const DdcKeyInputs& in);

// ── Derived Data Cache ────────────────────────────────────────────────────────
// Two-tier content-addressed blob store: a local tier (fast, per-machine,
// shared across every project on the box) and an optional shared tier (any
// path both machines can see — an NFS/SMB mount is a studio cache with zero
// server code). Read path: local → shared (hit promotes the blob into local).
// Write path: ingest local, then push shared best-effort — a dead network
// mount must never fail a cook that already produced correct output.
//
// Blobs are immutable and stored read-only; materialization into a project's
// .cache/ is by hardlink when possible (zero bytes copied), copy otherwise.
class DdcStore {
public:
    // Empty localRoot → defaultLocalRoot(). Empty sharedRoot → no shared tier.
    explicit DdcStore(std::filesystem::path localRoot  = {},
                      std::filesystem::path sharedRoot = {});

    // True if either tier holds the blob.
    bool contains(const std::string& key) const;

    // Materialize the blob for `key` at `dst` (replacing dst). False on miss.
    bool fetch(const std::string& key, const std::filesystem::path& dst);

    // Ingest a produced file under `key` (atomic: temp + rename; first writer
    // wins, identical content by construction). `src` is left in place.
    bool store(const std::string& key, const std::filesystem::path& src);

    // Small-record variants (manifests): store/fetch a byte string under a
    // key, same tiering and atomicity as file blobs.
    bool storeBytes(const std::string& key, const std::string& bytes);
    bool fetchBytes(const std::string& key, std::string& out);

    // Drop the LOCAL blob for `key` (force-recook path: suspicion of a bad
    // blob must bypass the cache, or the "re-cook" just re-fetches it).
    // Never touches the shared tier — other machines may be serving from it.
    void evictLocal(const std::string& key);

    const std::filesystem::path& localRoot()  const { return m_local; }
    const std::filesystem::path& sharedRoot() const { return m_shared; }

    struct Stats { uint64_t localHits=0, sharedHits=0, misses=0, stores=0; };
    Stats stats() const;

    // $ENGINE_DDC, else <home>/.engine/ddc — per-machine, cross-project.
    static std::filesystem::path defaultLocalRoot();
    // $ENGINE_DDC_SHARED, else empty (no shared tier).
    static std::filesystem::path sharedRootFromEnv();

private:
    static std::filesystem::path blobPath(const std::filesystem::path& root,
                                          const std::string& key);
    bool ingest(const std::filesystem::path& root, const std::string& key,
                const std::filesystem::path& src) const;
    bool materialize(const std::filesystem::path& blob,
                     const std::filesystem::path& dst) const;

    std::filesystem::path m_local;
    std::filesystem::path m_shared;
    mutable std::atomic<uint64_t> m_localHits{0}, m_sharedHits{0},
                                  m_misses{0},   m_stores{0};
};

} // namespace assetlib
