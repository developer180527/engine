#include "gltf_importer.h"

#include <cgltf.h>
#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "render/vertex.h"
#include "render/mesh.h"

namespace {

// Helper: read N floats from a cgltf accessor at element index `i`.
// cgltf supports many storage formats (FLOAT, UNSIGNED_SHORT_NORMALIZED,
// etc.) at various strides. cgltf_accessor_read_float abstracts all of that
// behind a uniform "give me floats" API. Saves us implementing each format.
//
// Returns true on success, false if the accessor is null or the read fails.
bool readFloats(const cgltf_accessor* acc, size_t i, float* out, size_t n) {
    if (!acc) return false;
    return cgltf_accessor_read_float(acc, i, out, n);
}

// Find the first attribute matching a given semantic (POSITION, NORMAL, etc.).
// Returns nullptr if not present. cgltf stores attributes as a flat array
// because a single primitive can have multiple of the same semantic at
// different indices (TEXCOORD_0, TEXCOORD_1, COLOR_0, COLOR_1, ...).
const cgltf_accessor* findAttribute(const cgltf_primitive* prim,
                                    cgltf_attribute_type type,
                                    int index = 0) {
    for (cgltf_size i = 0; i < prim->attributes_count; ++i) {
        const cgltf_attribute& attr = prim->attributes[i];
        if (attr.type == type && attr.index == index) {
            return attr.data;
        }
    }
    return nullptr;
}

} // namespace

bool GltfImporter::supports(std::string_view extension) const {
    return extension == "gltf" || extension == "glb";
}

MeshImportResult GltfImporter::load(const std::string& path,
                                    AssetRegistry& assets) {
    // ---- Step 1: parse the file with cgltf ----
    //
    // cgltf_parse_file handles both .gltf (JSON + external .bin) and .glb
    // (single binary) automatically based on file extension and magic bytes.
    cgltf_options options{};
    cgltf_data*   data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, path.c_str(), &data);
    if (result != cgltf_result_success) {
        return MeshImportResult::fail(
            "Failed to parse glTF file: " + path);
    }

    // RAII guard for cgltf_data — ensures cgltf_free runs on any return path.
    struct CgltfGuard {
        cgltf_data* data;
        ~CgltfGuard() { if (data) cgltf_free(data); }
    } guard{data};

    // ---- Step 2: load buffer data (vertex/index bytes from .bin or .glb) ----
    //
    // For .gltf, this reads the external .bin file referenced in the JSON.
    // For .glb, this is a no-op (the binary is already inside the .glb).
    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success) {
        return MeshImportResult::fail(
            "Failed to load glTF buffer data for: " + path);
    }

    // ---- Step 3: validate the parsed data ----
    //
    // cgltf has a built-in validator that catches malformed glTF (out-of-range
    // accessor indices, mismatched buffer views, etc.). Cheap insurance.
    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        return MeshImportResult::fail(
            "glTF validation failed for: " + path);
    }

    // ---- Step 4: locate the first primitive of the first mesh ----
    if (data->meshes_count == 0) {
        return MeshImportResult::fail(
            "glTF contains no meshes: " + path);
    }
    const cgltf_mesh& mesh = data->meshes[0];

    if (mesh.primitives_count == 0) {
        return MeshImportResult::fail(
            "First mesh has no primitives: " + path);
    }
    const cgltf_primitive& prim = mesh.primitives[0];

    // We only handle triangles. glTF can encode triangle strips, lines, and
    // points too, but supporting them all complicates the loader for negligible
    // gain — virtually all real assets use plain triangles.
    if (prim.type != cgltf_primitive_type_triangles) {
        return MeshImportResult::fail(
            "First primitive is not triangles (only triangles supported): " + path);
    }

    // ---- Step 5: locate required attributes ----
    const cgltf_accessor* posAcc    = findAttribute(&prim, cgltf_attribute_type_position);
    const cgltf_accessor* normalAcc = findAttribute(&prim, cgltf_attribute_type_normal);
    const cgltf_accessor* uvAcc     = findAttribute(&prim, cgltf_attribute_type_texcoord);

    if (!posAcc) {
        return MeshImportResult::fail(
            "Primitive has no POSITION attribute: " + path);
    }
    if (!normalAcc) {
        return MeshImportResult::fail(
            "Primitive has no NORMAL attribute (required by current loader): " + path);
    }
    // UV is optional — we synthesize zeros if absent.

    if (!prim.indices) {
        return MeshImportResult::fail(
            "Primitive has no indices (only indexed meshes supported): " + path);
    }

    // ---- Step 6: build the vertex array in our Vertex format ----
    //
    // cgltf gives us per-attribute accessors over the original buffer data.
    // We interleave them into a single contiguous Vertex array because that's
    // what bgfx::createVertexBuffer expects with our existing layout.
    const size_t vertexCount = posAcc->count;

    // Sanity check: all attribute accessors should have the same count.
    if (normalAcc->count != vertexCount) {
        return MeshImportResult::fail(
            "POSITION and NORMAL accessors have different vertex counts: " + path);
    }

    std::vector<Vertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        Vertex& v = vertices[i];

        if (!readFloats(posAcc, i, v.position, 3)) {
            return MeshImportResult::fail(
                "Failed to read position at vertex " + std::to_string(i));
        }
        if (!readFloats(normalAcc, i, v.normal, 3)) {
            return MeshImportResult::fail(
                "Failed to read normal at vertex " + std::to_string(i));
        }
        if (uvAcc) {
            if (!readFloats(uvAcc, i, v.uv, 2)) {
                return MeshImportResult::fail(
                    "Failed to read UV at vertex " + std::to_string(i));
            }
        } else {
            v.uv[0] = 0.0f;
            v.uv[1] = 0.0f;
        }
    }

    // ---- Step 7: build the index array ----
    //
    // glTF can use 16-bit or 32-bit indices. Our existing cube path uses 16-bit;
    // for now we use 32-bit unconditionally since real models often exceed 65k
    // vertices. bgfx supports both, but the createIndexBuffer call needs a flag
    // for 32-bit. We also need to update Mesh's index format if we want to
    // mix and match — for now this importer always emits 32-bit indices.
    const size_t indexCount = prim.indices->count;
    std::vector<uint32_t> indices(indexCount);
    for (size_t i = 0; i < indexCount; ++i) {
        indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
    }

    // ---- Step 8: upload to GPU ----
    //
    // We use copy semantics (bgfx::copy, not bgfx::makeRef) because our
    // local std::vectors go out of scope when this function returns. makeRef
    // would create a use-after-free.
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), uint32_t(vertices.size() * sizeof(Vertex))),
        Vertex::layout()
    );

    if (!bgfx::isValid(vbh)) {
        return MeshImportResult::fail(
            "Failed to create GPU vertex buffer for: " + path);
    }

    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
        bgfx::copy(indices.data(), uint32_t(indices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32  // 32-bit indices
    );

    if (!bgfx::isValid(ibh)) {
        bgfx::destroy(vbh);
        return MeshImportResult::fail(
            "Failed to create GPU index buffer for: " + path);
    }

    // ---- Step 9: register the Mesh in the asset registry ----
    MeshHandle handle = assets.addMesh(Mesh{ vbh, ibh, uint32_t(indexCount) });

    std::printf("[glTF] Loaded '%s': %zu vertices, %zu indices\n",
                path.c_str(), vertexCount, indexCount);

    return MeshImportResult::ok(handle);
}
