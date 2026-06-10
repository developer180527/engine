#pragma once
// Content services — reachable via engine.assetService() / engine.sceneService().
//
//   AssetService — handle-based async mesh/texture loading (worker decode,
//                  main-thread GPU upload drained by the frame loop)
//   SceneService — cooked binary scene loading

#include "runtime/asset_service.h"
#include "runtime/scene_service.h"
