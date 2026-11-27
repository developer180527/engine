#pragma once
// ── retained_input — which rows of the retained scene to write this frame ────
// WO-072, the P3b step of docs/plans/renderer-program.md §9: the retained
// render scene (render/world/render_scene.h) is written INCREMENTALLY, from
// the world's change streams (components/change_stream.h), instead of
// wholesale from a capture of every object.
//
// A row is computed from: the pose and its interpolation history, the mesh
// and its material override, the skin's palette slot, and what takes an
// entity into or out of the item queries (Disabled, Prefab, a parent). This
// subscribes to those, and each apply frame reads them into a set of
// reported owners. A row is then written when:
//
//   * its owner was reported (added, changed or removed: a removed one, or
//     one that left the queries, is retired because it produced no row);
//   * it is still INTERPOLATING (alpha < 1 and PrevTransform != Transform):
//     its matrix moves with alpha every frame though no component changes;
//   * it is PARENTED: its matrix depends on every ancestor, which its own
//     reports do not cover. Children are a small minority of a scene.
//
// And the whole table is rewritten (`full`) on the first frame, when a mesh
// or skeleton was added or removed (what a handle resolves to changed with no
// component changing), and when interpolation switches on or off.
//
// Here, not in render/world/, because it knows flecs and the components; and
// not inside the Renderer, so a test drives exactly what extraction uses
// without a GPU (tests/change_stream_test.cpp).
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <flecs.h>

#include "components/change_stream.h"
#include "components/mesh_renderer.h"
#include "components/prev_transform.h"
#include "components/skinned_mesh.h"
#include "core/transform.h"

namespace rworld {

class RetainedInput {
public:
    struct Frame {
        uint64_t meshRevision     = 0;   // AssetRegistry::revision()
        uint64_t skeletonRevision = 0;   // SkeletonRegistry::revision()
        float    alpha            = 1.0f;
    };

    // Reads what changed in `world` since the last call. Returns whether the
    // whole table must be rewritten this frame (then the reported set is
    // still filled, but every row is written regardless).
    bool read(flecs::world& world, const Frame& f) {
        changes::Streams& s = changes::Streams::ensure(world.c_ptr());
        bool full = false;
        if (!m_started || m_epoch != s.epoch()) {
            // New, or a different world at this address: any old stream ids
            // belonged to a world that is gone, and are dropped, not ended.
            m_streams.clear();
            const flecs::id_t ids[] = {
                world.id<Transform>(), world.id<PrevTransform>(),
                world.id<MeshRenderer>(), world.id<SkinnedMesh>(),
                flecs::Disabled, flecs::Prefab, ecs_pair(flecs::ChildOf, flecs::Wildcard),
            };
            for (flecs::id_t id : ids) m_streams.push_back(s.subscribe(id));
            m_epoch = s.epoch();
            m_started = true;
            full = true;
        }
        m_reported.clear();
        constexpr uint32_t kBatch = 1024;
        changes::Change buf[kBatch];
        for (changes::StreamId id : m_streams) {
            uint32_t n;
            do {
                n = s.read(id, buf, kBatch);
                for (uint32_t i = 0; i < n; ++i) m_reported.insert(buf[i].entity);
            } while (n == kBatch);
        }
        if (f.meshRevision != m_meshRevision || f.skeletonRevision != m_skeletonRevision) full = true;
        m_meshRevision = f.meshRevision;
        m_skeletonRevision = f.skeletonRevision;
        const bool interp = f.alpha < 1.0f;
        if (interp != m_interpolating) full = true;
        m_interpolating = interp;
        m_full = full;
        m_touched.assign(m_reported.begin(), m_reported.end());
        return full;
    }

    // Whether a PARENTLESS row must be written. Read-only: the parallel
    // extraction chunks may all ask at once.
    bool needsRow(uint64_t owner, const Transform& t, const PrevTransform* p) const {
        return m_full || m_reported.count(owner) != 0
            || (p && m_interpolating && !samePose(t, *p));
    }

    // The owners reported this frame: those without a row are retired.
    const std::vector<uint64_t>& touched() const { return m_touched; }

    // Ends the subscriptions. Only while `world` is alive.
    void stop(flecs::world& world) {
        changes::Streams* s = changes::Streams::of(world.c_ptr());
        if (s && m_started && s->epoch() == m_epoch)
            for (changes::StreamId id : m_streams) s->unsubscribe(id);
        m_streams.clear();
        m_started = false;
    }

private:
    bool     m_started = false;
    uint64_t m_epoch = 0;
    std::vector<changes::StreamId> m_streams;
    uint64_t m_meshRevision = ~0ull, m_skeletonRevision = ~0ull;
    bool     m_interpolating = false;
    bool     m_full = true;
    std::unordered_set<uint64_t> m_reported;
    std::vector<uint64_t>        m_touched;
};

} // namespace rworld
