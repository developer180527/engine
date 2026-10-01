// ── render_scene — see render_scene.h ─────────────────────────────────────────
#include "render/world/render_scene.h"

#include <algorithm>
#include <cstring>

namespace rworld {

namespace {
bool sameBits(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }
}  // namespace

std::string RowData::firstDifference(const RowData& o) const {
    if (!sameBits(model.m, o.model.m, sizeof(model.m)))  return "model";
    if (!sameBits(&sphere, &o.sphere, sizeof(sphere)))   return "sphere";
    if (keyBase != o.keyBase)                            return "keyBase";
    if (mesh != o.mesh)                                  return "mesh";
    if (material != o.material)                          return "material";
    if (palette != o.palette)                            return "palette";
    if (flags != o.flags)                                return "flags";
    return {};
}

// ── RenderTable ───────────────────────────────────────────────────────────────
RenderObjectId RenderTable::create(uint64_t owner, const RowData& row) {
    uint32_t i;
    if (!m_free.empty()) {
        i = m_free.back();
        m_free.pop_back();
    } else {
        i = (uint32_t)m_state.size();
        m_model.emplace_back();   m_sphere.emplace_back(); m_keyBase.push_back(0);
        m_mesh.emplace_back();    m_material.emplace_back();
        m_palette.push_back(RowData::kNoPalette); m_flags.push_back(0);
        m_state.push_back(State::Free); m_generation.push_back(0);
        m_retireFrame.push_back(0);     m_owner.push_back(0);
    }
    m_state[i] = State::Live;
    m_owner[i] = owner;
    ++m_live;
    const RenderObjectId id{i, m_generation[i]};
    update(id, row);
    return id;
}

bool RenderTable::update(RenderObjectId id, const RowData& row) {
    if (!isLive(id)) return false;          // stale or invalid: detected, not aliased
    const uint32_t i = id.index;
    m_model[i]    = row.model;
    m_sphere[i]   = row.sphere;
    m_keyBase[i]  = row.keyBase;
    m_mesh[i]     = row.mesh;
    m_material[i] = row.material;
    m_palette[i]  = row.palette;
    m_flags[i]    = row.flags;
    return true;
}

bool RenderTable::destroy(RenderObjectId id, uint64_t frame) {
    if (!isLive(id)) return false;
    const uint32_t i = id.index;
    m_state[i]       = State::Retired;
    m_retireFrame[i] = frame;
    m_owner[i]       = 0;
    // Bumped HERE, at retire, so the id is invalid from this moment rather than
    // from the moment the slot is reused: a holder learns its object is gone
    // on the same frame, not one fence later.
    ++m_generation[i];
    m_retired.push_back(i);
    --m_live;
    return true;
}

std::size_t RenderTable::collect(uint64_t completedFrame) {
    // m_retired is in retire order, and retire frames only grow, so the
    // fenced ones are a prefix.
    std::size_t n = 0;
    while (n < m_retired.size() && m_retireFrame[m_retired[n]] <= completedFrame) {
        const uint32_t i = m_retired[n];
        m_state[i] = State::Free;
        m_free.push_back(i);
        ++n;
    }
    m_retired.erase(m_retired.begin(), m_retired.begin() + (std::ptrdiff_t)n);
    return n;
}

bool RenderTable::alive(RenderObjectId id) const { return isLive(id); }

bool RenderTable::read(RenderObjectId id, RowData& out) const {
    // A copy, gathered from the columns. Not for hot paths: P3c reads the
    // columns directly.
    if (!isLive(id)) return false;
    out = rowAt(id.index);
    return true;
}

uint64_t RenderTable::owner(RenderObjectId id) const {
    return isLive(id) ? m_owner[id.index] : 0;
}

RowData RenderTable::rowAt(uint32_t i) const {
    RowData r;
    r.model    = m_model[i];
    r.sphere   = m_sphere[i];
    r.keyBase  = m_keyBase[i];
    r.mesh     = m_mesh[i];
    r.material = m_material[i];
    r.palette  = m_palette[i];
    r.flags    = m_flags[i];
    return r;
}

// ── RenderScene ───────────────────────────────────────────────────────────────
void RenderScene::apply(const std::vector<CapturedRow>& rows, uint64_t frame) {
    for (const CapturedRow& c : rows) {
        const bool skinned = (c.row.flags & kRowSkinned) != 0;
        auto [it, fresh] = m_entries.try_emplace(c.owner);
        Entry& e = it->second;
        if (!fresh && e.skinned != skinned) {        // changed table: re-home it
            tableFor(e.skinned).destroy(e.id, frame);
            fresh = true;
        }
        if (fresh) {
            e.id      = tableFor(skinned).create(c.owner, c.row);
            e.skinned = skinned;
        } else {
            tableFor(skinned).update(e.id, c.row);
        }
        e.seen = frame;
    }
    // Owners not captured this frame are gone (destroyed, lost their mesh, or
    // their mesh failed to resolve): retire their rows.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.seen != frame) {
            tableFor(it->second.skinned).destroy(it->second.id, frame);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    if (frame >= kFramesInFlight) {
        m_static.collect(frame - kFramesInFlight);
        m_skinned.collect(frame - kFramesInFlight);
    }
    m_lastApplied = frame;
    m_applied     = true;
}

RenderObjectId RenderScene::idOf(uint64_t owner) const {
    auto it = m_entries.find(owner);
    return it == m_entries.end() ? RenderObjectId{} : it->second.id;
}

std::string RenderScene::diff(const std::vector<CapturedRow>& rows) const {
    // The rebuild: a scene built from nothing out of the same rows.
    RenderScene fresh;
    fresh.apply(rows, m_lastApplied);

    auto describe = [](uint64_t owner, const std::string& what) {
        return "entity " + std::to_string(owner) + ": " + what;
    };
    // Every rebuilt row must exist here, in the same table, with equal fields.
    std::string out;
    for (const auto& [owner, fe] : fresh.m_entries) {
        auto mine = m_entries.find(owner);
        if (mine == m_entries.end())
            return describe(owner, "extracted this frame but has no row in the retained table");
        if (mine->second.skinned != fe.skinned)
            return describe(owner, std::string("row is in the ") +
                                   (mine->second.skinned ? "skinned" : "static") +
                                   " table, the rebuild puts it in the " +
                                   (fe.skinned ? "skinned" : "static") + " one");
        const RenderTable& t = mine->second.skinned ? m_skinned : m_static;
        RowData kept, rebuilt;
        if (!t.read(mine->second.id, kept))
            return describe(owner, "its id is not LIVE in the retained table (stale id)");
        (fe.skinned ? fresh.m_skinned : fresh.m_static).read(fe.id, rebuilt);
        const std::string col = kept.firstDifference(rebuilt);
        if (!col.empty()) return describe(owner, "column '" + col + "' differs from the rebuild");
        if (t.owner(mine->second.id) != owner)
            return describe(owner, "its row belongs to entity " +
                                   std::to_string(t.owner(mine->second.id)));
    }
    // And nothing extra: a LIVE row the rebuild does not have is a leaked row.
    if (liveCount() != fresh.liveCount()) {
        std::string leaked;
        auto find = [&](const RenderTable& t) {
            t.forEachLive([&](RenderObjectId, uint64_t owner, const RowData&) {
                if (leaked.empty() && !fresh.m_entries.count(owner))
                    leaked = describe(owner, "has a LIVE row but was not extracted this frame");
            });
        };
        find(m_static);
        find(m_skinned);
        if (!leaked.empty()) return leaked;
        return "the retained table has " + std::to_string(liveCount()) +
               " LIVE rows, the rebuild " + std::to_string(fresh.liveCount());
    }
    return out;
}

}  // namespace rworld
