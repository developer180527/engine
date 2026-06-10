#pragma once
// Render extension point — swap the whole pipeline:
//
//   engine.renderer().setPipeline(std::make_unique<MyPipeline>());
//
// IRenderPipeline receives a RenderView (extracted items/lights + camera +
// target) and a RenderContext (registries, fallback textures, view-id
// allocator). It never touches the ECS.

#include "render/render_pipeline.h"
#include "render/render_view.h"
#include "render/render_context.h"
