#include "gltf_importer.h"

#include <cgltf.h>
#include <stb_image.h>
#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>

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

    const cgltf_mesh&      mesh = data->meshes[0];
    if (mesh.primitives_count == 0)
        return MeshImportResult::fail("No primitives: " + path);

    const cgltf_primitive& prim = mesh.primitives[0];

    if (prim.type != cgltf_primitive_type_triangles)
        return MeshImportResult::fail("Non-triangle primitive: " + path);

    const cgltf_accessor* posAcc    = findAttribute(&prim, cgltf_attribute_type_position);
    const cgltf_accessor* normalAcc = findAttribute(&prim, cgltf_attribute_type_normal);
    const cgltf_accessor* uvAcc     = findAttribute(&prim, cgltf_attribute_type_texcoord);

    if (!posAcc)    return MeshImportResult::fail("No POSITION: " + path);
    if (!normalAcc) return MeshImportResult::fail("No NORMAL: " + path);
    if (!prim.indices) return MeshImportResult::fail("No indices: " + path);

    const size_t vertexCount = posAcc->count;
    if (normalAcc->count != vertexCount)
        return MeshImportResult::fail("Attribute count mismatch: " + path);

    // Build vertex buffer + AABB
    bx::Vec3 bMin{ 1e30f,  1e30f,  1e30f};
    bx::Vec3 bMax{-1e30f, -1e30f, -1e30f};

    std::vector<Vertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        Vertex& v = vertices[i];
        if (!readFloats(posAcc,    i, v.position, 3)) return MeshImportResult::fail("Bad POSITION");
        if (!readFloats(normalAcc, i, v.normal,   3)) return MeshImportResult::fail("Bad NORMAL");
        if (uvAcc) { readFloats(uvAcc, i, v.uv, 2); }
        else       { v.uv[0] = v.uv[1] = 0.0f; }

        if (v.position[0] < bMin.x) bMin.x = v.position[0];
        if (v.position[1] < bMin.y) bMin.y = v.position[1];
        if (v.position[2] < bMin.z) bMin.z = v.position[2];
        if (v.position[0] > bMax.x) bMax.x = v.position[0];
        if (v.position[1] > bMax.y) bMax.y = v.position[1];
        if (v.position[2] > bMax.z) bMax.z = v.position[2];
    }

    // Build index buffer (32-bit)
    const size_t indexCount = prim.indices->count;
    std::vector<uint32_t> indices(indexCount);
    for (size_t i = 0; i < indexCount; ++i)
        indices[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);

    // Upload to GPU
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), (uint32_t)(vertexCount * sizeof(Vertex))),
        Vertex::layout());
    bgfx::IndexBufferHandle  ibh = bgfx::createIndexBuffer(
        bgfx::copy(indices.data(),  (uint32_t)(indexCount  * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    if (!bgfx::isValid(vbh) || !bgfx::isValid(ibh)) {
        if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
        if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
        return MeshImportResult::fail("GPU buffer creation failed: " + path);
    }

    // Load material + texture
    const std::string gltfDir =
        std::filesystem::path(path).parent_path().string();

    Material mat;
    mat.doubleSided = prim.material ? prim.material->double_sided : false;

    if (prim.material) {
        auto& pbr = prim.material->pbr_metallic_roughness;
        mat.baseColorFactor[0] = pbr.base_color_factor[0];
        mat.baseColorFactor[1] = pbr.base_color_factor[1];
        mat.baseColorFactor[2] = pbr.base_color_factor[2];
        mat.baseColorFactor[3] = pbr.base_color_factor[3];

        if (pbr.base_color_texture.texture) {
            mat.baseColorTexture =
                loadTexture(pbr.base_color_texture.texture, gltfDir, storage);
        }
    }

    MaterialHandle matHandle = storage.materials.addMaterial(std::move(mat));

    Mesh engineMesh(vbh, ibh, (uint32_t)indexCount);
    engineMesh.doubleSided = mat.doubleSided;
    engineMesh.boundsMin   = bMin;
    engineMesh.boundsMax   = bMax;
    engineMesh.material    = matHandle;

    MeshHandle handle = storage.meshes.addMesh(std::move(engineMesh));

    std::printf("[glTF] Loaded '%s': %zu verts, %zu idx, "
                "bounds (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f), "
                "tex=%s, double-sided=%s\n",
                path.c_str(), vertexCount, indexCount,
                bMin.x, bMin.y, bMin.z, bMax.x, bMax.y, bMax.z,
                mat.baseColorTexture.valid() ? "yes" : "no",
                mat.doubleSided ? "yes" : "no");

    return MeshImportResult::ok(handle);
}
