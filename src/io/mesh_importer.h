#pragma once

#include <string>
#include <string_view>

#include "core/handle.h"
#include "render/asset_registry.h"

// MeshImporter interface.
//
// One importer per file format family. cgltf handles glTF/GLB; if Assimp
// is added later, an AssimpImporter would handle FBX/OBJ/etc. The asset
// browser asks each registered importer "do you support this extension?"
// and uses the first one that says yes.
//
// Importers must NEVER throw or crash. All failures go through the
// MeshImportResult so the editor can surface clean error messages to the
// user. This is the layer-1 fault tolerance pattern from our architecture:
// validate at boundaries, fail with information.
//
// Why a virtual interface instead of, say, a function pointer table or
// std::variant of importer types? The format extension space is open-ended
// (.gltf, .glb, .fbx, .obj, .dae, .blend, ...) and adding a new format
// shouldn't require modifying every place that touches importers. A virtual
// interface lets a new ConcreteImporter slot in by inheriting and
// registering — no central enumeration to update.

struct MeshImportResult {
    bool        success = false;
    MeshHandle  mesh;       // Valid only if success == true
    std::string error;      // Populated if success == false

    static MeshImportResult ok(MeshHandle h) {
        return { true, h, "" };
    }

    static MeshImportResult fail(std::string msg) {
        return { false, {}, std::move(msg) };
    }
};

class MeshImporter {
public:
    virtual ~MeshImporter() = default;

    // Returns true if this importer handles files with the given extension.
    // Extension is lowercase, no leading dot (e.g. "gltf", "glb", "fbx").
    virtual bool supports(std::string_view extension) const = 0;

    // Load the mesh at `path` and register it in `assets`.
    // Caller guarantees the file exists; importer handles parsing/validation.
    virtual MeshImportResult load(const std::string& path,
                                  AssetRegistry& assets) = 0;
};
