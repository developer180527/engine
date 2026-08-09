// ── editor_undo_test — the editor's first automated coverage ─────────────────
// `src/editor` has been the largest untested surface in the tree, and its own
// info.md makes the honest case for that: an ImGui application whose behaviour
// is mouse-driven is genuinely hard to test, and nothing depends on the editor,
// so the blast radius of an editor bug is the editor.
//
// That argument covers the PANELS. It does not cover this. `UndoStack` is
// header-only, has no ImGui in its include graph, and is pure state machine
// plus ECS mutation — the single most consequential piece of editor logic,
// because a wrong undo silently destroys authored work and the user's only
// signal is that their scene is subtly wrong later.
//
// It also runs entity snapshots through the SAME `EntitySerde` table as scene
// save, so it inherits that path's guarantees and its bugs. That is the point
// of the design and worth pinning.
//
// Headless: a flecs world, no window, no ImGui, no GPU.
#include <cstdio>
#include <cmath>
#include <string>

#include <flecs.h>

#include "editor/undo_stack.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/entity_id.h"
#include "components/camera.h"
#include "components/light.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

static flecs::entity makeEntity(flecs::world& w, const char* name,
                                float x = 0, float y = 0, float z = 0) {
    flecs::entity e = w.entity();
    e.set<Name>({ name });
    Transform t;
    t.position = { x, y, z };
    e.set<Transform>(t);
    ensureEntityId(e);
    return e;
}

static bool vec3Near(const bx::Vec3& a, float x, float y, float z) {
    return std::fabs(a.x - x) < 1e-4f && std::fabs(a.y - y) < 1e-4f
        && std::fabs(a.z - z) < 1e-4f;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("editor_undo_test\n");

    // ── Index bookkeeping ───────────────────────────────────────────────────
    {
        flecs::world w;
        UndoStack s;
        CHECK(!s.canUndo() && !s.canRedo(), "a fresh stack can neither undo nor redo");
        CHECK(!s.undo(w) && !s.redo(w), "and both are no-ops that return false");

        flecs::entity e = makeEntity(w, "Cube");
        Transform a; a.position = { 1, 2, 3 };
        Transform b; b.position = { 4, 5, 6 };
        e.set<Transform>(b);
        s.pushTransform(e, a, b);

        CHECK(s.canUndo() && !s.canRedo(), "after one push: undo yes, redo no");
        CHECK(s.undoDescription() == "Move Cube",
              "the description names the entity (\"%s\")", s.undoDescription().c_str());

        CHECK(s.undo(w), "undo succeeds");
        CHECK(vec3Near(e.get<Transform>().position, 1, 2, 3),
              "...and restores the BEFORE transform");
        CHECK(!s.canUndo() && s.canRedo(), "now: undo no, redo yes");

        CHECK(s.redo(w), "redo succeeds");
        CHECK(vec3Near(e.get<Transform>().position, 4, 5, 6),
              "...and restores the AFTER transform");
    }

    // ── A new push truncates the redo tail ──────────────────────────────────
    // Undo twice then do something new: the two undone commands must become
    // unreachable. If they survive, redo replays work the user abandoned.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "E");
        Transform t0; t0.position = { 0, 0, 0 };
        Transform t1; t1.position = { 1, 0, 0 };
        Transform t2; t2.position = { 2, 0, 0 };
        s.pushTransform(e, t0, t1);
        s.pushTransform(e, t1, t2);
        s.undo(w); s.undo(w);
        CHECK(s.canRedo(), "two undos leave a redo tail");

        Transform t9; t9.position = { 9, 0, 0 };
        s.pushTransform(e, t0, t9);
        CHECK(!s.canRedo(), "a new command discards the abandoned redo tail");
        CHECK(s.canUndo(), "...and is itself undoable");
    }

    // ── Depth cap ───────────────────────────────────────────────────────────
    // kMaxDepth evicts from the FRONT while m_index tracks the back. The index
    // arithmetic during eviction is the kind of thing that is right until it is
    // off by one, and an off-by-one here indexes a deque out of bounds.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "E");
        const int kPushes = UndoStack::kMaxDepth + 50;
        for (int i = 0; i < kPushes; ++i) {
            Transform a; a.position = { (float)i,     0, 0 };
            Transform b; b.position = { (float)i + 1, 0, 0 };
            s.pushTransform(e, a, b);
        }
        int undone = 0;
        while (s.undo(w)) {
            if (++undone > UndoStack::kMaxDepth + 10) break;   // runaway guard
        }
        CHECK(undone == UndoStack::kMaxDepth,
              "exactly kMaxDepth commands survive eviction (%d)", undone);
        CHECK(!s.canUndo(), "and the stack is exhausted, not negative");

        int redone = 0;
        while (s.redo(w)) { if (++redone > UndoStack::kMaxDepth + 10) break; }
        CHECK(redone == UndoStack::kMaxDepth,
              "every surviving command redoes back (%d)", redone);
    }

    // ── Delete/undo preserves the ID ────────────────────────────────────────
    // The header states this guarantee explicitly: "Respawn preserves the
    // original id, so other commands still in the stack continue to resolve the
    // entity." If a respawn minted a fresh id, every EARLIER command targeting
    // that entity would silently become a no-op — the scene would look right
    // after one undo and diverge on the next.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "Doomed", 7, 8, 9);
        const uint64_t originalId = ensureEntityId(e);

        s.pushEntityDelete(e);
        e.destruct();
        CHECK(!findById(w, originalId).is_alive(), "the entity is gone");

        CHECK(s.undo(w), "undo of a delete succeeds");
        flecs::entity back = findById(w, originalId);
        CHECK(back.is_alive(), "...and the entity is back under its ORIGINAL id");
        if (back.is_alive()) {
            const Name* n = back.try_get<Name>();
            CHECK(n && n->value == "Doomed", "...with its name restored");
            CHECK(back.try_get<Transform>()
                  && vec3Near(back.get<Transform>().position, 7, 8, 9),
                  "...and its transform");
        }
    }

    // ── An earlier command still resolves after a delete/undo cycle ─────────
    // This is what the id guarantee is FOR, so it gets its own assertion rather
    // than being implied by the one above.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "E", 0, 0, 0);
        const uint64_t id = ensureEntityId(e);

        Transform a; a.position = { 1, 1, 1 };
        Transform b; b.position = { 2, 2, 2 };
        e.set<Transform>(b);
        s.pushTransform(e, a, b);          // command #1, targets `id`

        s.pushEntityDelete(e);             // command #2
        e.destruct();

        s.undo(w);                         // undo #2 -> respawn
        s.undo(w);                         // undo #1 -> must still find it
        flecs::entity back = findById(w, id);
        CHECK(back.is_alive() && back.try_get<Transform>()
              && vec3Near(back.get<Transform>().position, 1, 1, 1),
              "a command pushed BEFORE the delete still resolves after respawn");
    }

    // ── Parent links survive delete/undo ────────────────────────────────────
    {
        flecs::world w;
        UndoStack s;
        flecs::entity parent = makeEntity(w, "Parent");
        flecs::entity child  = makeEntity(w, "Child");
        child.add(flecs::ChildOf, parent);
        const uint64_t parentId = ensureEntityId(parent);
        const uint64_t childId  = ensureEntityId(child);

        s.pushEntityDelete(child);
        child.destruct();
        s.undo(w);

        flecs::entity back = findById(w, childId);
        CHECK(back.is_alive(), "the child comes back");
        if (back.is_alive()) {
            flecs::entity p = back.target(flecs::ChildOf);
            CHECK(p && p.is_alive() && ensureEntityId(p) == parentId,
                  "...still parented to the same entity — an orphaned child is "
                  "a silent hierarchy corruption the user only sees much later");
        }
    }

    // ── Reparent undo cannot build a cycle ──────────────────────────────────
    // applyReparent guards with isAncestorOf. A cycle here hangs every
    // transform walk in the editor, which presents as a freeze, not an error.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity a = makeEntity(w, "A");
        flecs::entity b = makeEntity(w, "B");
        b.add(flecs::ChildOf, a);

        Transform local; local.position = { 0, 0, 0 };
        const uint64_t aId = ensureEntityId(a);
        s.pushReparent(b, aId, local, 0, local);  // b: child-of-a -> root
        b.remove(flecs::ChildOf, flecs::Wildcard);
        s.undo(w);                                // back to child-of-a

        flecs::entity pb = b.target(flecs::ChildOf);
        CHECK(pb && pb == a, "reparent undo restores the original parent");

        // Now the hostile direction: ask A to become a child of its own
        // descendant B. The guard must refuse.
        const uint64_t bId = ensureEntityId(b);
        s.pushReparent(a, 0, local, bId, local);  // A becomes a child of its OWN descendant
        s.redo(w);
        int depth = 0;
        for (flecs::entity cur = a; cur && cur.is_alive() && depth < 64; ++depth)
            cur = cur.target(flecs::ChildOf);
        CHECK(depth < 64, "a reparent that would build a CYCLE is refused "
                          "(ancestor walk terminated at depth %d)", depth);
    }

    // ── Component toggle round-trip ─────────────────────────────────────────
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "Lamp");
        Light l; l.intensity = 42.0f; l.range = 13.0f;
        e.set<Light>(l);

        s.pushComponentRemove(e, "light", "Remove Light");
        e.remove<Light>();
        CHECK(e.try_get<Light>() == nullptr, "the component is removed");

        CHECK(s.undo(w), "undo of a component removal succeeds");
        const Light* back = e.try_get<Light>();
        CHECK(back != nullptr, "...and the component is back");
        if (back)
            CHECK(std::fabs(back->intensity - 42.0f) < 1e-4f
                  && std::fabs(back->range - 13.0f) < 1e-4f,
                  "...with its VALUES, not defaults (intensity %.1f range %.1f)",
                  back->intensity, back->range);

        CHECK(s.redo(w), "redo removes it again");
        CHECK(e.try_get<Light>() == nullptr, "...and it is gone");
    }

    // ── Commands targeting a dead entity degrade, never crash ───────────────
    // The editor can delete an entity by other means while commands referencing
    // it are still in the stack. Every apply* path looks the target up by id and
    // must tolerate not finding it.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "Ghost");
        Transform a; a.position = { 1, 1, 1 };
        Transform b; b.position = { 2, 2, 2 };
        s.pushTransform(e, a, b);
        Light l; l.intensity = 1.0f; e.set<Light>(l);
        s.pushComponentRemove(e, "light", "Remove Light");

        e.destruct();                       // gone, outside the undo stack

        bool survived = true;
        try { while (s.undo(w)) {} } catch (...) { survived = false; }
        CHECK(survived, "undoing commands whose target no longer exists is a "
                        "logged no-op, not a crash or an exception");
    }

    // ── The snapshot goes through the shared serializer ─────────────────────
    // This is the design guarantee in the header: undo cannot drift from scene
    // save, because both drive EntitySerde::table(). A component added to the
    // table must therefore survive a delete/undo without anyone touching this
    // file — so the assertion is on a component the undo stack never names.
    {
        flecs::world w;
        UndoStack s;
        flecs::entity e = makeEntity(w, "Cam");
        Camera c; c.fov = 77.0f; c.nearPlane = 0.25f;
        e.set<Camera>(c);
        const uint64_t id = ensureEntityId(e);

        s.pushEntityDelete(e);
        e.destruct();
        s.undo(w);

        flecs::entity back = findById(w, id);
        const Camera* bc = back.is_alive() ? back.try_get<Camera>() : nullptr;
        CHECK(bc != nullptr, "a component the UndoStack never names by hand "
                             "survives delete/undo");
        if (bc)
            CHECK(std::fabs(bc->fov - 77.0f) < 1e-3f,
                  "...with its value (fov %.1f) — proof the snapshot really is "
                  "the shared EntitySerde table and not a private copy", bc->fov);
    }

    if (g_failures) {
        std::printf("editor_undo_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("editor_undo_test: PASS\n");
    return 0;
}
