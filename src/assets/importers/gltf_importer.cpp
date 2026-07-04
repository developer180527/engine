#include "gltf_importer.h"

#include <cgltf.h>
#include <stb_image.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <utility>

#include "render/vertex.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"

namespace {

bool readFloats(const cgltf_accessor* acc, size_t i, float* out, size_t n) {
    if (!acc) return false;
    return cgltf_accessor_read_float(acc, i, out, n) != 0;
}

const cgltf_accessor* findAttribute(const cgltf_primitive* prim,
                                    cgltf_attribute_type type,
                                    int index = 0) {
    for (cgltf_size i = 0; i < prim->attributes_count; ++i) {
        if (prim->attributes[i].type == type &&
            prim->attributes[i].index == index)
            return prim->attributes[i].data;
    }
    return nullptr;
}

// Upload raw RGBA pixel data to bgfx and return a Texture.
Texture uploadRGBA(const uint8_t* pixels, int w, int h) {
    const bgfx::Memory* mem = bgfx::alloc((uint32_t)(w * h * 4));
    std::memcpy(mem->data, pixels, w * h * 4);
    bgfx::TextureHandle handle = bgfx::createTexture2D(
        (uint16_t)w, (uint16_t)h, false, 1,
        bgfx::TextureFormat::RGBA8, 0, mem);
    return bgfx::isValid(handle)
        ? Texture(handle, (uint16_t)w, (uint16_t)h)
        : Texture{};
}

// Load a texture referenced by the glTF material.
// Handles both embedded (GLB) and external (separate file) images.
TextureHandle loadTexture(const cgltf_texture* cTex,
                          const std::string& gltfDir,
                          AssetStorage& storage) {
    if (!cTex || !cTex->image) return {};

    int w = 0, h = 0, comp = 0;
    uint8_t* pixels = nullptr;

    if (cTex->image->buffer_view) {
        // Embedded image (typical in .glb files)
        const auto* bv = cTex->image->buffer_view;
        const uint8_t* data =
            static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
        pixels = stbi_load_from_memory(data, (int)bv->size, &w, &h, &comp, 4);
    } else if (cTex->image->uri) {
        // External image file (typical in .gltf + separate .png/.jpg)
        auto texPath = std::filesystem::path(gltfDir) / cTex->image->uri;
        pixels = stbi_load(texPath.string().c_str(), &w, &h, &comp, 4);
    }

    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        std::printf("[glTF] WARN: failed to decode texture image\n");
        return {};
    }

    Texture tex = uploadRGBA(pixels, w, h);
    stbi_image_free(pixels);

    if (!tex.valid()) {
        std::printf("[glTF] WARN: failed to upload texture to GPU\n");
        return {};
    }

    std::printf("[glTF]   Texture %dx%d uploaded\n", w, h);
    return storage.textures.addTexture(std::move(tex));
}

} // namespace

bool GltfImporter::supports(std::string_view ext) const {
    return ext == "gltf" || ext == "glb";
}

MeshImportResult GltfImporter::load(const std::string& path,
                                    AssetStorage& storage) {
    cgltf_options options{};
    cgltf_data*   data = nullptr;

    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        return MeshImportResult::fail("Failed to parse: " + path);

    struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{data};

    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
        return MeshImportResult::fail("Failed to load buffers: " + path);

    if (cgltf_validate(data) != cgltf_result_success)
        return MeshImportResult::fail("glTF validation failed: " + path);

    if (data->meshes_count == 0)
        return MeshImportResult::fail("No meshes: " + path);

    const std::string gltfDir =
        std::filesystem::path(path).parent_path().string();

    // Resolve a glTF material to an engine MaterialHandle, caching by pointer so
    // primitives that share a material don't re-upload its textures. Null glTF
    // material -> invalid handle (renderer uses its defaults).
    std::vector<std::pair<const cgltf_material*, MaterialHandle>> matCache;
    auto resolveMaterial = [&](const cgltf_material* cm) -> MaterialHandle {
        if (!cm) return {};
        for (auto& e : matCache) if (e.first == cm) return e.second;
        Material mat;
        mat.doubleSided = cm->double_sided;
        const auto& pbr = cm->pbr_metallic_roughness;
        mat.baseColorFactor[0] = pbr.base_color_factor[0];
        mat.baseColorFactor[1] = pbr.base_color_factor[1];
        mat.baseColorFactor[2] = pbr.base_color_factor[2];
        mat.baseColorFactor[3] = pbr.base_color_factor[3];
        if (pbr.base_color_texture.texture)
            mat.baseColorTexture = loadTexture(pbr.base_color_texture.texture, gltfDir, storage);
        MaterialHandle h = storage.materials.addMaterial(std::move(mat));
        matCache.emplace_back(cm, h);
        return h;
    };

    // MERGE every triangle primitive of every mesh into one shared VB/IB, one
    // SubmeshRange (with its own material) per primitive. Reading only
    // meshes[0].primitives[0] silently dropped the rest of the geometry.
    std::vector<Vertex>       vertices;
    std::vector<uint32_t>     indices;
    std::vector<SubmeshRange> submeshes;
    bx::Vec3 bMin{ 1e30f,  1e30f,  1e30f};
    bx::Vec3 bMax{-1e30f, -1e30f, -1e30f};
    bool anyDoubleSided = false;

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* posAcc    = findAttribute(&prim, cgltf_attribute_type_position);
            const cgltf_accessor* normalAcc = findAttribute(&prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uvAcc     = findAttribute(&prim, cgltf_attribute_type_texcoord);
            if (!posAcc || !prim.indices) continue;   // unusable primitive — skip

            const uint32_t vertBase = (uint32_t)vertices.size();
            for (size_t i = 0; i < posAcc->count; ++i) {
                Vertex v{};
                readFloats(posAcc, i, v.position, 3);
                if (normalAcc && i < normalAcc->count) readFloats(normalAcc, i, v.normal, 3);
                else v.normal[1] = 1.0f;
                if (uvAcc) readFloats(uvAcc, i, v.uv, 2);
                bMin.x = std::min(bMin.x, v.position[0]); bMax.x = std::max(bMax.x, v.position[0]);
                bMin.y = std::min(bMin.y, v.position[1]); bMax.y = std::max(bMax.y, v.position[1]);
                bMin.z = std::min(bMin.z, v.position[2]); bMax.z = std::max(bMax.z, v.position[2]);
                vertices.push_back(v);
            }
            const uint32_t idxStart = (uint32_t)indices.size();
            for (size_t i = 0; i < prim.indices->count; ++i)
                indices.push_back(vertBase + (uint32_t)cgltf_accessor_read_index(prim.indices, i));

            SubmeshRange r;
            r.indexOffset = idxStart;
            r.indexCount  = (uint32_t)indices.size() - idxStart;
            r.material    = resolveMaterial(prim.material);
            submeshes.push_back(r);
            if (prim.material && prim.material->double_sided) anyDoubleSided = true;
        }
    }

    if (indices.empty())
        return MeshImportResult::fail("No triangle primitives: " + path);

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(Vertex))),
        Vertex::layout());
    bgfx::IndexBufferHandle  ibh = bgfx::createIndexBuffer(
        bgfx::copy(indices.data(),  (uint32_t)(indices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);
    if (!bgfx::isValid(vbh) || !bgfx::isValid(ibh)) {
        if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
        if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
        return MeshImportResult::fail("GPU buffer creation failed: " + path);
    }

    const size_t subCount = submeshes.size();
    Mesh engineMesh(vbh, ibh, (uint32_t)indices.size());
    engineMesh.doubleSided = anyDoubleSided;
    engineMesh.boundsMin   = bMin;
    engineMesh.boundsMax   = bMax;
    // A single submesh stays on the simple single-draw path (material set,
    // submeshes empty) so common one-material models are unchanged.
    engineMesh.material    = submeshes.front().material;
    if (subCount > 1) engineMesh.submeshes = std::move(submeshes);

    MeshHandle handle = storage.meshes.addMesh(std::move(engineMesh));
    std::printf("[glTF] Loaded '%s': %zu submesh(es), %zu verts, %zu idx\n",
                path.c_str(), subCount, vertices.size(), indices.size());
    return MeshImportResult::ok(handle);
}
