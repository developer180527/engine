#pragma once
// ── render_scene — the retained scene's table, ids and lifetime (P3a) ─────────
//
// renderer-program.md §9 is the design; this is its first phase (WO-019). Every
// frame the renderer used to rebuild its whole description of the scene and
// throw it away. This is the table that will REMEMBER it instead: one row per
// drawable object, named by a stable `RenderObjectId`, alive from the frame the
// object appears to the frame the GPU is certainly done with it.
//
// P3a ONLY BUILDS AND PROVES IT. Extraction still computes every row from
// scratch each frame and writes it here wholesale (`RenderScene::apply`); nothing
// reads the table yet. That is deliberate (§9.11): the lifetime rules and the
// rebuild-and-diff check are proven correct while the table is still a mirror,
// before anything depends on it. P3b makes the writes incremental, P3c makes
// extraction read it, P3d moves the contents to the GPU.
//
// GPU-FREE AND RUNTIME-FREE, like everything in render/world/ (audit LAYER-04):
// no bgfx type, no flecs, no clock. An owner is an opaque 64-bit key (the
// renderer uses the entity id), and frames are plain counters the caller
// advances. That is what makes all of it unit-testable
// (tests/render_scene_test.cpp) and what keeps the RHI swap to the uploader.
//
// THE THREE STATES (§9.1), and why RETIRED exists:
//
//   LIVE ──destroy()──► RETIRED ──collect(fenced frame)──► FREE ──create()──► LIVE
//
// A slot destroyed on frame N cannot be reused on frame N: command buffers from
// N-1 may still read it. It waits in RETIRED until the caller says the frame it
// retired on has completed on the GPU (a frame fence on bgfx, a timeline value
// under the RHI: same rule). Reusing it early is the classic "only corrupts under
// load" bug, so the test turns red if the fence is skipped.
//
// THE GENERATION bumps when a slot retires, so an id held past its object's
// death is DETECTED (alive() says no, update() refuses) instead of silently
// writing whatever took the slot next. The test turns red if the bump is skipped.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/handle.h"
#include "core/math_types.h"
#include "render/world/render_world.h"   // CullSphere

namespace rworld {

// A row's name. A value, never a pointer: compare and copy it freely.
struct RenderObjectId {
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    uint32_t index      = kInvalid;
    uint32_t generation = 0;

    bool valid() const { return index != kInvalid; }
    bool operator==(const RenderObjectId& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const RenderObjectId& o) const { return !(*this == o); }
};

enum RowFlags : uint8_t {
    kRowHasBounds   = 1u << 0,
    kRowSkinned     = 1u << 1,
    // Reserved for streaming (§9.2 #7): a LIVE row whose mesh was evicted is
    // non-drawable WITHOUT its id becoming invalid. Nothing sets it yet.
    kRowStreamedOut = 1u << 2,
};

// What a row holds: the VIEW-INDEPENDENT half of an object (§9.4). LOD is
// chosen per view, so the mesh and key here are level 0's; the model matrix is
// this frame's interpolated one, the same for every view of the frame.
struct RowData {
    static constexpr uint32_t kNoPalette = 0xFFFFFFFFu;
    Mat4           model;
    CullSphere     sphere;
    uint64_t       keyBase  = 0;       // sort key, depth left zero
    MeshHandle     mesh;               // level 0
    MaterialHandle material;           // override, else the mesh's own
    uint32_t       palette  = kNoPalette;   // skinned: anim::skinPalettes() slot
    uint8_t        flags    = 0;       // RowFlags

    // Field by field, naming the first that differs ("" when equal). Floats
    // compare BITWISE: the diff exists to catch a row that was not rewritten,
    // and a rebuilt row reproduces the same bits from the same inputs.
    std::string firstDifference(const RowData& o) const;
};

// The interface the contract names (docs/contracts/render-scene.md):
// create / update / destroy, by id. RenderTable is the real one;
// NullRenderScene is what a build with no renderer gets.
class IRenderScene {
public:
    virtual ~IRenderScene() = default;
    virtual RenderObjectId create(uint64_t owner, const RowData& row) = 0;
    virtual bool update(RenderObjectId id, const RowData& row) = 0;
    // LIVE -> RETIRED, recording `frame` as the frame it died on.
    virtual bool destroy(RenderObjectId id, uint64_t frame) = 0;
};

class NullRenderScene final : public IRenderScene {
public:
    RenderObjectId create(uint64_t, const RowData&) override { return {}; }
    bool update(RenderObjectId, const RowData&) override { return false; }
    bool destroy(RenderObjectId, uint64_t) override { return false; }
};

// One SoA table. Columns are split by who reads them (§9.4); the bookkeeping
// columns (state, generation, retire frame, owner) are the table's own.
class RenderTable final : public IRenderScene {
public:
    enum class State : uint8_t { Free, Live, Retired };

    RenderObjectId create(uint64_t owner, const RowData& row) override;
    bool update(RenderObjectId id, const RowData& row) override;
    bool destroy(RenderObjectId id, uint64_t frame) override;

    // RETIRED -> FREE for every row retired on or before `completedFrame`:
    // the newest frame the GPU has certainly finished. Returns how many.
    std::size_t collect(uint64_t completedFrame);

    bool           alive(RenderObjectId id) const;
    bool           read(RenderObjectId id, RowData& out) const;   // false unless alive
    uint64_t       owner(RenderObjectId id) const;   // 0 unless alive

    std::size_t liveCount()    const { return m_live; }
    std::size_t retiredCount() const { return m_retired.size(); }
    std::size_t capacity()     const { return m_state.size(); }

    // Visit every LIVE row: fn(RenderObjectId, owner, const RowData&).
    template <typename Fn> void forEachLive(Fn&& fn) const {
        for (uint32_t i = 0; i < (uint32_t)m_state.size(); ++i)
            if (m_state[i] == State::Live)
                fn(RenderObjectId{i, m_generation[i]}, m_owner[i], rowAt(i));
    }

private:
    RowData rowAt(uint32_t i) const;
    bool    isLive(RenderObjectId id) const {
        return id.index < m_state.size() && m_state[id.index] == State::Live
            && m_generation[id.index] == id.generation;
    }

    // ── Content columns (§9.4) ──────────────────────────────────────────────
    std::vector<Mat4>           m_model;
    std::vector<CullSphere>     m_sphere;
    std::vector<uint64_t>       m_keyBase;
    std::vector<MeshHandle>     m_mesh;
    std::vector<MaterialHandle> m_material;
    std::vector<uint32_t>       m_palette;
    std::vector<uint8_t>        m_flags;
    // ── Bookkeeping ─────────────────────────────────────────────────────────
    std::vector<State>          m_state;
    std::vector<uint32_t>       m_generation;
    std::vector<uint64_t>       m_retireFrame;
    std::vector<uint64_t>       m_owner;
    std::vector<uint32_t>       m_free;      // FREE slots, reused LIFO
    std::vector<uint32_t>       m_retired;   // RETIRED slots, oldest first
    std::size_t                 m_live = 0;
};

// One object as extraction saw it this frame.
struct CapturedRow {
    uint64_t owner = 0;
    RowData  row;
};

// The retained scene of ONE world: a static table and a skinned table (§9.2 #13:
// two tables, one implementation), and which owner has which row. Owned by the
// renderer, one per world it draws.
class RenderScene {
public:
    // How many frames the GPU may still be working behind the CPU. A slot
    // retired on frame F is reused no earlier than frame F + this. bgfx
    // pipelines at most two frames (API thread + render thread); three is the
    // conservative bound, and it costs only retired slots waiting a frame longer.
    static constexpr uint64_t kFramesInFlight = 3;

    // Write this frame's rows WHOLESALE (P3a): rows for new owners are
    // created, existing ones updated, and owners absent this frame retired.
    // An owner that switches table (gained or lost a skin) is retired from one
    // and created in the other. Then collects what the fence allows.
    void apply(const std::vector<CapturedRow>& rows, uint64_t frame);

    // THE REBUILD-AND-DIFF CHECK (§9.7). Builds a fresh scene from `rows` and
    // compares it against this one: the same owners LIVE, each in the same
    // table, each row equal field by field. Returns "" when they agree, else
    // the first mismatch, naming the owner and the column. Only meaningful
    // right after apply() with the same rows; P3b, when writes become
    // incremental, is where it starts catching missed change hooks.
    std::string diff(const std::vector<CapturedRow>& rows) const;

    // The frame apply() last ran for, so a renderer drawing one world in
    // several views a frame applies only once (§9.3: one apply point).
    uint64_t lastApplied() const { return m_lastApplied; }
    bool     appliedThisFrame(uint64_t frame) const {
        return m_applied && m_lastApplied == frame;
    }

    RenderObjectId idOf(uint64_t owner) const;
    const RenderTable& staticTable()  const { return m_static; }
    const RenderTable& skinnedTable() const { return m_skinned; }
    std::size_t liveCount() const { return m_static.liveCount() + m_skinned.liveCount(); }

private:
    struct Entry {
        RenderObjectId id;
        bool           skinned = false;
        uint64_t       seen    = 0;    // the frame it was last captured
    };
    RenderTable& tableFor(bool skinned) { return skinned ? m_skinned : m_static; }

    RenderTable m_static;
    RenderTable m_skinned;
    std::unordered_map<uint64_t, Entry> m_entries;
    uint64_t m_lastApplied = 0;
    bool     m_applied     = false;
};

}  // namespace rworld
