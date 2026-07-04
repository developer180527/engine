#pragma once
// ── AnimService — the runtime's animation control surface ───────────────────
// IAnimService implementation the C API/Lua reach through ScriptHost.
// Extracted from ScriptHost (which was accumulating subsystem
// IMPLEMENTATIONS — the god-object trajectory): ScriptHost now coordinates
// and delegates; this owns the anim policy (path resolution, clip binding,
// Animator writes). The engine owns the machinery (ozz sampling/blending in
// AnimatorSystem); this is only the gameplay-facing control layer.
//
// KNOWN COST (also flagged in the API review): ClipLibrary::load on an
// un-cooked clip is a synchronous Assimp parse — first bind of a clip
// hitches if triggered mid-play. Projects preload at sim start; the real fix
// is the animation cooker (ozz archives), then an async bind path.
#include <filesystem>

#include <flecs.h>

#include "animation/clip_library.h"
#include "animation/clip_registry.h"
#include "animation/skeleton_registry.h"
#include "components/animator.h"
#include "components/skinned_mesh.h"
#include "project/project_context.h"
#include "runtime/scripting/script_services.h"

class AnimService final : public IAnimService {
public:
    void init(SkeletonRegistry* skels, AnimClipRegistry* clips,
              ClipLibrary* lib, const ProjectContext* project) {
        m_skelReg = skels; m_clipReg = clips;
        m_clipLib = lib;   m_projectCtx = project;
    }

    // Resolve the (world, id) pair back into a flecs::entity for component
    // access — this service is all about the entity's Animator/SkinnedMesh.
    bool play(flecs::world& w, flecs::entity_t eId, const char* clipPath, float fade) override {
        flecs::entity e = w.entity(eId);
        if (!e.is_alive() || !clipPath || !m_skelReg || !m_clipReg || !m_clipLib)
            return false;
        const SkinnedMesh* sm = e.try_get<SkinnedMesh>();
        if (!sm || !sm->skeleton.valid()) return false;
        const Skeleton* sk = m_skelReg->get(sm->skeleton);
        if (!sk) return false;

        std::filesystem::path p(clipPath);
        if (p.is_relative() && m_projectCtx)
            p = m_projectCtx->projectRoot / p;
        AnimClipHandle h = m_clipLib->load(p.string(), sm->skeleton, *sk, *m_clipReg);
        if (!h.valid()) return false;

        Animator a = e.has<Animator>() ? e.get<Animator>() : Animator{};
        if (fade >= 0.0f) a.fade = fade;
        if (a.clip.id == h.id) return true;   // already playing this clip
        a.clip     = h;
        a.clipPath = p.string();
        a.time     = 0.0f;
        a.playing  = true;
        e.set<Animator>(a);
        return true;
    }
    void setSpeed(flecs::world& w, flecs::entity_t eId, float s) override {
        if (Animator* a = mut(w.entity(eId))) a->speed = s;
    }
    void setLooping(flecs::world& w, flecs::entity_t eId, bool loop) override {
        if (Animator* a = mut(w.entity(eId))) a->looping = loop;
    }
    void setPlaying(flecs::world& w, flecs::entity_t eId, bool playing) override {
        if (Animator* a = mut(w.entity(eId))) a->playing = playing;
    }
    bool isPlaying(flecs::world& w, flecs::entity_t eId) const override {
        flecs::entity e = w.entity(eId);
        const Animator* a = e.is_alive() ? e.try_get<Animator>() : nullptr;
        return a && a->playing;
    }
    float time(flecs::world& w, flecs::entity_t eId) const override {
        flecs::entity e = w.entity(eId);
        const Animator* a = e.is_alive() ? e.try_get<Animator>() : nullptr;
        return a ? a->time : 0.0f;
    }
    float duration(flecs::world& w, flecs::entity_t eId) const override {
        flecs::entity e = w.entity(eId);
        const Animator* a = e.is_alive() ? e.try_get<Animator>() : nullptr;
        if (!a || !m_clipReg) return 0.0f;
        const AnimClip* c = m_clipReg->get(a->clip);
        return c ? c->duration : 0.0f;
    }

private:
    static Animator* mut(flecs::entity e) {
        return e.is_alive() && e.has<Animator>() ? &e.get_mut<Animator>() : nullptr;
    }
    SkeletonRegistry*     m_skelReg    = nullptr;
    AnimClipRegistry*     m_clipReg    = nullptr;
    ClipLibrary*          m_clipLib    = nullptr;
    const ProjectContext* m_projectCtx = nullptr;
};
