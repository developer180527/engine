#include "assetlib/mesh_asset.h"
#include <fstream>
#include <cstring>
#include <cstdio>

namespace assetlib {

static const struct { uint32_t flag; uint32_t size; } kAttribs[] = {
    {VF_POSITION,12},{VF_NORMAL,12},{VF_TANGENT,16},
    {VF_UV0,8},{VF_UV1,8},{VF_COLOR,4},{VF_JOINTS,4},{VF_WEIGHTS,16},
};

uint32_t vertexStride(uint32_t flags) {
    uint32_t s = 0;
    for (auto& a : kAttribs) if (flags & a.flag) s += a.size;
    return s;
}

uint32_t vertexAttributeOffset(uint32_t flags, VertexFlags attr) {
    uint32_t off = 0;
    for (auto& a : kAttribs) {
        if (a.flag == static_cast<uint32_t>(attr)) break;
        if (flags & a.flag) off += a.size;
    }
    return off;
}

bool saveMesh(const MeshAsset& mesh, const std::filesystem::path& outPath) {
    std::filesystem::create_directories(outPath.parent_path());
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Cannot write: %s\n",
                     outPath.string().c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&mesh.header),      sizeof(MeshHeader));
    f.write(reinterpret_cast<const char*>(mesh.vertexData.data()),
            static_cast<std::streamsize>(mesh.vertexData.size()));
    f.write(reinterpret_cast<const char*>(mesh.indexData.data()),
            static_cast<std::streamsize>(mesh.indexData.size()));
    f.write(reinterpret_cast<const char*>(mesh.submeshes.data()),
            static_cast<std::streamsize>(mesh.submeshes.size() * sizeof(MeshSubmesh)));
    // Material section — always written; materialCount in header says how many
    f.write(reinterpret_cast<const char*>(mesh.materials.data()),
            static_cast<std::streamsize>(mesh.materials.size() * sizeof(CookedMaterial)));

    // v3 skinned payload
    if (mesh.header.boneCount > 0) {
        f.write(reinterpret_cast<const char*>(mesh.bones.data()),
                (std::streamsize)(mesh.bones.size() * sizeof(CookedBone)));
        const uint32_t skelSize = (uint32_t)mesh.skeletonBlob.size();
        f.write(reinterpret_cast<const char*>(&skelSize), 4);
        f.write(reinterpret_cast<const char*>(mesh.skeletonBlob.data()), skelSize);
        const uint32_t clipCount = (uint32_t)mesh.clips.size();
        f.write(reinterpret_cast<const char*>(&clipCount), 4);
        for (const auto& c : mesh.clips) {
            const uint32_t nameLen = (uint32_t)c.name.size();
            f.write(reinterpret_cast<const char*>(&nameLen), 4);
            f.write(c.name.data(), nameLen);
            f.write(reinterpret_cast<const char*>(&c.mappedTracks), 4);
            f.write(reinterpret_cast<const char*>(&c.totalTracks), 4);
            const uint32_t blobSize = (uint32_t)c.blob.size();
            f.write(reinterpret_cast<const char*>(&blobSize), 4);
            f.write(reinterpret_cast<const char*>(c.blob.data()), blobSize);
        }
    }
    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Write error: %s\n",
                     outPath.string().c_str());
        return false;
    }
    std::printf("[MeshAsset] Saved %s  verts=%u idx=%u submeshes=%u mats=%u\n",
        outPath.filename().string().c_str(),
        mesh.header.vertexCount, mesh.header.indexCount,
        mesh.header.submeshCount, mesh.header.materialCount);
    return true;
}

bool loadMesh(MeshAsset& out, const std::filesystem::path& inPath) {
    std::ifstream f(inPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[MeshAsset] Cannot open: %s\n",
                     inPath.string().c_str());
        return false;
    }
    f.read(reinterpret_cast<char*>(&out.header), sizeof(MeshHeader));
    if (!f || out.header.magic != 0x4D455348) {
        std::fprintf(stderr, "[MeshAsset] Bad magic: %s\n",
                     inPath.string().c_str());
        return false;
    }
    if (out.header.version != 2 && out.header.version != 3) {
        std::fprintf(stderr, "[MeshAsset] Unsupported version %u: %s\n",
                     out.header.version, inPath.string().c_str());
        return false;
    }
    const size_t vbBytes = static_cast<size_t>(out.header.vertexCount)
                           * out.header.vertexStride;
    out.vertexData.resize(vbBytes);
    f.read(reinterpret_cast<char*>(out.vertexData.data()),
           static_cast<std::streamsize>(vbBytes));

    const size_t ibBytes = static_cast<size_t>(out.header.indexCount)
                           * out.header.indexStride;
    out.indexData.resize(ibBytes);
    f.read(reinterpret_cast<char*>(out.indexData.data()),
           static_cast<std::streamsize>(ibBytes));

    out.submeshes.resize(out.header.submeshCount);
    f.read(reinterpret_cast<char*>(out.submeshes.data()),
           static_cast<std::streamsize>(
               out.header.submeshCount * sizeof(MeshSubmesh)));

    // Material section (version 2+)
    if (out.header.materialCount > 0) {
        out.materials.resize(out.header.materialCount);
        f.read(reinterpret_cast<char*>(out.materials.data()),
               static_cast<std::streamsize>(
                   out.header.materialCount * sizeof(CookedMaterial)));
    }

    // v3 skinned payload (older files / static meshes: boneCount == 0)
    if (out.header.version >= 3 && out.header.boneCount > 0) {
        out.bones.resize(out.header.boneCount);
        f.read(reinterpret_cast<char*>(out.bones.data()),
               static_cast<std::streamsize>(out.bones.size() * sizeof(CookedBone)));
        uint32_t skelSize = 0;
        f.read(reinterpret_cast<char*>(&skelSize), 4);
        out.skeletonBlob.resize(skelSize);
        f.read(reinterpret_cast<char*>(out.skeletonBlob.data()), skelSize);
        uint32_t clipCount = 0;
        f.read(reinterpret_cast<char*>(&clipCount), 4);
        out.clips.resize(clipCount);
        for (auto& c : out.clips) {
            uint32_t nameLen = 0;
            f.read(reinterpret_cast<char*>(&nameLen), 4);
            c.name.resize(nameLen);
            f.read(c.name.data(), nameLen);
            f.read(reinterpret_cast<char*>(&c.mappedTracks), 4);
            f.read(reinterpret_cast<char*>(&c.totalTracks), 4);
            uint32_t blobSize = 0;
            f.read(reinterpret_cast<char*>(&blobSize), 4);
            c.blob.resize(blobSize);
            f.read(reinterpret_cast<char*>(c.blob.data()), blobSize);
        }
    }

    return f.good() || f.eof();
}

} // namespace assetlib
