#pragma once
// ECS components — attach to flecs entities via engine.ctx().ecs.
// Register your own game components on the same world; the engine does not
// wrap flecs.

#include "core/transform.h"
#include "components/name.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh.h"
#include "components/animator.h"
#include "components/rigid_body.h"
#include "components/character_controller.h"
#include "components/collision_events.h"
#include "components/script_component.h"
#include "components/entity_id.h"
