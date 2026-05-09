#pragma once
#include <string>
#include <string_view>
#include "core/handle.h"
#include "io/asset_storage.h"

struct MeshImportResult {
    bool        success = false;
    MeshHandle  mesh;
    std::string error;

    static MeshImportResult ok(MeshHandle h)       { return {true,  h, ""}; }
    static MeshImportResult fail(std::string msg)  { return {false, {}, std::move(msg)}; }
};

class MeshImporter {
public:
    virtual ~MeshImporter() = default;
    virtual bool             supports(std::string_view extension) const = 0;
    virtual MeshImportResult load(const std::string& path,
                                  AssetStorage& storage) = 0;
};
