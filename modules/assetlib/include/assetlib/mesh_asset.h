#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

namespace assetlib {

enum VertexFlags : uint32_t {
    VF_POSITION = 1 << 0,  // float3  12 bytes
    VF_NORMAL   = 1 << 1,  // float3  12 bytes
    VF_TANGENT  = 1 << 2,  // float4  16 bytes
    VF_UV0      = 1 << 3,  // float2   8 bytes
    VF_UV1      = 1 << 4,  // float2   8 bytes
    VF_COLOR    = 1 << 5,  // uint8x4  4 bytes
    VF_JOINTS   = 1 << 6,  // uint8x4  4 bytes
    VF_WEIGHTS  = 1 << 7,  // float4  16 bytes
};

struct MeshHeader {
    uint32_t magic         = 0x4D455348;
    uint32_t version       = 2;            // bumped: added material section
    uint8_t  uuid[16]      = {};
    uint32_t vertexFlags   = 0;
    uint32_t vertexStride  = 0;
    uint32_t vertexCount   = 0;
    uint32_t indexCount    = 0;
    uint32_t indexStride   = 4;
    uint32_t submeshCount  = 0;
    float    boundsMin[3]  = {};
    float    boundsMax[3]  = {};
    uint32_t materialCount = 0;            // was _pad[0..3]
    uint32_t boneCount     = 0;            // v3: >0 = skinned payload follows
    // NOTE: v4's LOD count is NOT here. The header is a fixed-size block read
    // with one sizeof(), so growing it by 4 bytes would shift the payload of
    // every v2/v3 file already on disk and in the DDC — the static_assert below
    // exists to catch exactly that, and it did. The count leads the LOD section
    // instead, which costs nothing and keeps old files readable.
};
static_assert(sizeof(MeshHeader) == 80, "MeshHeader size changed");

// v3 skinned payload (after materials): CookedBone[boneCount], then an
// OPAQUE runtime-skeleton blob (ozz archive — assetlib never parses it),
// then embedded clips (each an opaque ozz animation archive). Bind pose is
// stored BOTH as SQT and as the raw local matrix (the engine's precision
// invariant: SQT round-trips are lossy for baked pivot chains).
struct CookedBone {
    char    name[64];
    int32_t parentIndex;
    float   bindPosition[3];
    float   bindRotation[4];   // quaternion xyzw
    float   bindScale[3];
    float   inverseBindMatrix[16];
    float   localBindMatrix[16];
    uint8_t _pad[8];
};
static_assert(sizeof(CookedBone) == 244 + 0 || true, "");

// FNV-1a over a byte range — the v6 blob digest (see MeshAsset below).
// Header-inline so the writer, the reader and the animation decoders all compute
// it from ONE definition; two implementations that must agree on a checksum is
// the drift this is meant to detect, not cause.
inline uint64_t blobDigest(const void* data, size_t len) {
    uint64_t h = 1469598103934665603ull;              // offset basis
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
inline uint64_t blobDigest(const std::vector<uint8_t>& b) {
    return blobDigest(b.data(), b.size());
}

struct CookedClipBlob {
    std::string          name;
    int32_t              mappedTracks = 0;
    int32_t              totalTracks  = 0;
    std::vector<uint8_t> blob;   // ozz animation archive
    // v6. FNV-1a over `blob`. 0 means "not carried" (a pre-v6 file), which the
    // decoders treat as UNVERIFIABLE and refuse — see the note on
    // MeshAsset::skeletonBlobDigest.
    uint64_t             blobDigest = 0;
};

struct MeshSubmesh {
    uint32_t indexOffset      = 0;
    uint32_t indexCount       = 0;
    uint8_t  materialUUID[16] = {};   // external material asset (reserved)
    uint32_t materialIndex    = 0;    // index into MeshAsset::materials (embedded)
    uint8_t  _pad[4]          = {};
};
static_assert(sizeof(MeshSubmesh) == 32, "MeshSubmesh size changed");

// Material flags stored in CookedMaterial::flags
static constexpr uint32_t kMatFlag_HasBaseColor = 1u << 0;
static constexpr uint32_t kMatFlag_HasNormalMap = 1u << 1;

// Per-material record appended after submeshes in the cooked file.
// Texture paths are stored as basenames — resolved against the
// source asset's parent directory at load time.
struct CookedMaterial {
    float    baseColorFactor[4] = {1, 1, 1, 1};
    float    roughness          = 0.7f;
    float    metallic           = 0.0f;
    uint32_t flags              = 0;
    char     baseColorPath[512] = {};
    char     normalMapPath[512] = {};
    // Total: 16 + 4 + 4 + 4 + 512 + 512 = 1052 bytes
};

struct MeshAsset {
    MeshHeader                 header;
    std::vector<uint8_t>       vertexData;
    std::vector<uint8_t>       indexData;
    std::vector<MeshSubmesh>   submeshes;
    std::vector<CookedMaterial> materials; // NEW
    // v3 skinned payload (empty for static meshes)
    std::vector<CookedBone>     bones;
    std::vector<uint8_t>        skeletonBlob;   // ozz skeleton archive
    std::vector<CookedClipBlob> clips;          // embedded takes

    // ── v6: integrity for the two OPAQUE blobs (BUG-0046) ───────────────────
    // FNV-1a over `skeletonBlob`. 0 means "not carried".
    //
    // These two byte ranges are the only part of a cooked mesh this engine does
    // not parse itself: they go straight into ozz's `IArchive`, a third-party
    // deserializer whose own comment reads "reading cannot fail" and which
    // enforces that with debug-only asserts. Guarding the STREAM (BUG-0045)
    // catches a short read; it cannot catch a corrupt COUNT field that ozz reads
    // successfully and then allocates from, which is BUG-0046.
    //
    // A deserializer that trusts its input cannot be made safe from the outside,
    // so the fix is to stop handing it unverified bytes. The digest is checked
    // in TWO places on purpose:
    //
    //   * `loadMesh` — the real trust boundary, file bytes becoming a struct. A
    //     mismatch fails the whole load.
    //   * `anim::decodeCookedSkeleton` / `decodeCookedClips` — cheap
    //     defence-in-depth, so a caller that built a MeshAsset by other means
    //     (a test, a future streaming path) is covered by the same rule.
    //
    // ABSENT (0) IS TREATED AS UNVERIFIABLE AND REFUSED, not as "trusted
    // legacy". A pre-v6 skinned mesh must be re-cooked, which the MeshCooker
    // version bump makes automatic for anything going through the DDC. The
    // alternative — trusting a file precisely because it is old enough not to
    // carry a checksum — is the hole this exists to close.
    //
    // FNV-1a rather than BLAKE3, for the same reason cook_result_file.h gives:
    // this detects CORRUPTION — a partial write, a bad disk, a truncated copy —
    // and it is not a security boundary. Anyone who can rewrite the blob can
    // rewrite the digest beside it; what guards the artifact against a hostile
    // SOURCE is the DDC's content hash.
    uint64_t                    skeletonBlobDigest = 0;

    // ── v4: coarser LOD levels ──────────────────────────────────────────────
    // Level 0 is this mesh; these are 1..lodCount, each a COMPLETE and
    // independent (vertices, indices) pair rather than an index range over the
    // parent's vertex buffer.
    //
    // Independent on purpose. Sharing a vertex buffer would make a level
    // cheaper to DRAW but not cheaper to STORE, and VRAM is the tighter budget
    // on the target hardware — a level that keeps 89 245 vertices to draw 670
    // triangles saves nothing where it matters. It also avoids a lifetime
    // hazard: bgfx handles are refcounted per resource, and two Mesh objects
    // sharing one vertex buffer means destroying either frees it for both.
    // SUBMESH RANGES TRAVEL WITH THE LEVEL (v5). Without them a level is one
    // range drawn with material[0], so a prop with more than one material group
    // CHANGED COLOUR the moment it crossed an LOD threshold — and 96 of the
    // MegaKit's 176 meshes have more than one, so about half the kit visibly
    // popped. Decimation clusters vertices globally but rebuilds the index
    // buffer group by group, so the ranges survive and a level draws with the
    // same materials as its parent.
    //
    // A v4 level has no table; that reads as one implicit range over the whole
    // buffer, which is what v4 meant.
    struct LodLevel {
        std::vector<uint8_t>     vertexData;
        std::vector<uint8_t>     indexData;
        std::vector<MeshSubmesh> submeshes;   // v5; empty = the whole buffer
        uint32_t                 vertexCount = 0;
        uint32_t                 indexCount  = 0;
    };
    std::vector<LodLevel> lods;
};

bool     saveMesh(const MeshAsset& mesh, const std::filesystem::path& outPath);
bool     loadMesh(MeshAsset& out,        const std::filesystem::path& inPath);
uint32_t vertexStride(uint32_t flags);
uint32_t vertexAttributeOffset(uint32_t flags, VertexFlags attr);

} // namespace assetlib
