#pragma once
#include <cstdint>
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
    uint8_t  _pad[4]       = {};           // was _pad[4..7]
};
static_assert(sizeof(MeshHeader) == 80, "MeshHeader size changed");

struct MeshSubmesh {
    uint32_t indexOffset      = 0;
    uint32_t indexCount       = 0;
    uint8_t  materialUUID[16] = {};
    uint8_t  _pad[8]          = {};
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
};

bool     saveMesh(const MeshAsset& mesh, const std::filesystem::path& outPath);
bool     loadMesh(MeshAsset& out,        const std::filesystem::path& inPath);
uint32_t vertexStride(uint32_t flags);
uint32_t vertexAttributeOffset(uint32_t flags, VertexFlags attr);

} // namespace assetlib
