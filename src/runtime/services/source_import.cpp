#include "runtime/services/source_import.h"

#include <memory>

#include "animation/clip_library.h"
#include "assets/clip_source.h"
#include "assets/importers/assimp_importer.h"
#include "assets/importers/gltf_importer.h"
#include "runtime/runtime.h"
#include "runtime/runtime_context.h"

namespace sourceimport {

void install(EngineRuntime& rt) {
    // Idempotent: a second call must not register every importer twice.
    if (!rt.headless() && !rt.ctx().importers.supports("glb")) {
        rt.ctx().importers.registerImporter(std::make_unique<GltfImporter>());
        rt.ctx().importers.registerImporter(std::make_unique<AssimpImporter>());
    }
    rt.clipLibrary().setSourceReader(&imp::readSourceClip);
}

}  // namespace sourceimport
