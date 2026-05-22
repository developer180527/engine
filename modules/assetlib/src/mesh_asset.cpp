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
    uint32_t s=0;
    for (auto& a:kAttribs) if (flags&a.flag) s+=a.size;
    return s;
}

uint32_t vertexAttributeOffset(uint32_t flags, VertexFlags attr) {
    uint32_t off=0;
    for (auto& a:kAttribs) {
        if (a.flag==static_cast<uint32_t>(attr)) break;
        if (flags&a.flag) off+=a.size;
    }
    return off;
}

bool saveMesh(const MeshAsset& mesh, const std::filesystem::path& outPath) {
    std::filesystem::create_directories(outPath.parent_path());
    std::ofstream f(outPath, std::ios::binary|std::ios::trunc);
    if (!f) { std::fprintf(stderr,"[MeshAsset] Cannot write: %s\n",outPath.string().c_str()); return false; }
    f.write(reinterpret_cast<const char*>(&mesh.header), sizeof(MeshHeader));
    f.write(reinterpret_cast<const char*>(mesh.vertexData.data()),
            static_cast<std::streamsize>(mesh.vertexData.size()));
    f.write(reinterpret_cast<const char*>(mesh.indexData.data()),
            static_cast<std::streamsize>(mesh.indexData.size()));
    f.write(reinterpret_cast<const char*>(mesh.submeshes.data()),
            static_cast<std::streamsize>(mesh.submeshes.size()*sizeof(MeshSubmesh)));
    if (!f) { std::fprintf(stderr,"[MeshAsset] Write error: %s\n",outPath.string().c_str()); return false; }
    std::printf("[MeshAsset] Saved %s  verts=%u idx=%u submeshes=%u\n",
        outPath.filename().string().c_str(),
        mesh.header.vertexCount, mesh.header.indexCount, mesh.header.submeshCount);
    return true;
}

bool loadMesh(MeshAsset& out, const std::filesystem::path& inPath) {
    std::ifstream f(inPath, std::ios::binary);
    if (!f) { std::fprintf(stderr,"[MeshAsset] Cannot open: %s\n",inPath.string().c_str()); return false; }
    f.read(reinterpret_cast<char*>(&out.header), sizeof(MeshHeader));
    if (!f || out.header.magic!=0x4D455348) {
        std::fprintf(stderr,"[MeshAsset] Bad magic: %s\n",inPath.string().c_str()); return false;
    }
    if (out.header.version!=1) {
        std::fprintf(stderr,"[MeshAsset] Unsupported version %u: %s\n",out.header.version,inPath.string().c_str()); return false;
    }
    const size_t vbBytes = static_cast<size_t>(out.header.vertexCount)*out.header.vertexStride;
    out.vertexData.resize(vbBytes);
    f.read(reinterpret_cast<char*>(out.vertexData.data()), static_cast<std::streamsize>(vbBytes));
    const size_t ibBytes = static_cast<size_t>(out.header.indexCount)*out.header.indexStride;
    out.indexData.resize(ibBytes);
    f.read(reinterpret_cast<char*>(out.indexData.data()), static_cast<std::streamsize>(ibBytes));
    out.submeshes.resize(out.header.submeshCount);
    f.read(reinterpret_cast<char*>(out.submeshes.data()),
           static_cast<std::streamsize>(out.header.submeshCount*sizeof(MeshSubmesh)));
    return f.good() || f.eof();
}

} // namespace assetlib
