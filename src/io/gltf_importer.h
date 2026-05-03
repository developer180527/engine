#pragma once

#include "mesh_importer.h"

// glTF / GLB importer using cgltf.
//
// Supports the modern .gltf (JSON + external .bin) and .glb (single
// binary) formats from Khronos. Both are parsed by cgltf into the same
// internal representation so this importer handles both with one code path.
//
// Current limitations (each documented as a clear "not supported" error
// rather than a silent crash):
//
//   - Only the first mesh of the first primitive is loaded. Multi-mesh
//     glTFs (a character with body + clothes as separate meshes) get
//     just the first mesh. Multi-mesh handling comes later.
//   - Materials are ignored. The engine does not yet have a material
//     system; meshes render with the normal-debug shader regardless of
//     what the file specifies.
//   - Textures are ignored. Same reason.
//   - Animations are ignored. Skeletal animation comes much later.
//   - Skinning weights are ignored.
//   - Morph targets are ignored.
//
// Geometry that IS handled:
//   - Vertex positions (required, error if missing)
//   - Vertex normals (required, error if missing — cgltf can compute them
//     for some inputs, but we keep it strict for now)
//   - Vertex texture coordinates (UV0). If absent, we synthesize zeros so
//     the vertex format stays consistent.
//   - Triangle indices (required; we don't handle line/point primitives)
//
// All loading is fault-tolerant: any failure produces a MeshImportResult
// with success=false and an informative error message. The engine never
// crashes from a bad glTF file.

class GltfImporter : public MeshImporter {
public:
    bool supports(std::string_view extension) const override;

    MeshImportResult load(const std::string& path,
                          AssetRegistry& assets) override;
};
