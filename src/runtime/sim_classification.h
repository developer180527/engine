#pragma once
// Registers every engine component with simhash's classification registry.
// Call once per world, immediately after MetaRegistry::registerAll — the two
// are deliberately separate (reflection vs reproducibility are different
// questions). Idempotent. See sim_classification.cpp for the governing rule
// and for every exemption's written reason.
#include <flecs.h>
#include "runtime/sim_hash.h"

namespace simhash { void registerClassification(flecs::world& w); }
