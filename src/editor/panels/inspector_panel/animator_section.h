#pragma once

#include <imgui.h>
#include <flecs.h>
#include <cstdio>
#include <filesystem>
#include <string>

#include "editor/engine_context.h"
#include "components/animator.h"
#include "components/skinned_mesh.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"
#include "animation/clip_library.h"
#include "editor/panels/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawAnimatorSection(EngineContext& ctx, flecs::entity e) {
    // Show section if entity has SkinnedMesh or Animator (or both)
    const bool hasSkin = e.has<SkinnedMesh>();
    const bool hasAnim = e.has<Animator>();
    if (!hasSkin && !hasAnim) return;

    sectionHeader("Animation");

    // ── SkinnedMesh info ────────────────────────────────────────────────────
    if (hasSkin) {
        const SkinnedMesh* skin_ptr = e.try_get<SkinnedMesh>();
        const SkinnedMesh& skin = *skin_ptr;

        const char* skelName = "(none)";
        int boneCount = 0;
        if (ctx.skeletons && skin.skeleton.valid()) {
            const Skeleton* skel = ctx.skeletons->get(skin.skeleton);
            if (skel) {
                boneCount = skel->boneCount();
                skelName = "(loaded)";
            }
        }

        ImGui::Text("Skeleton"); ImGui::SameLine(100.0f);
        ImGui::TextDisabled("handle=%u  %s  %d bones",
                            skin.skeleton.id, skelName, boneCount);

        ImGui::Text("Skin"); ImGui::SameLine(100.0f);
        ImGui::TextDisabled("%s", skin.hasSkinMatrices ? "active" : "bind pose");
    }

    // ── Animator playback controls ──────────────────────────────────────────
    if (hasAnim) {
        Animator& anim = e.get_mut<Animator>();

        // Clip info
        const char* clipName = "(none)";
        float clipDuration = 0.0f;
        if (ctx.clips && anim.clip.valid()) {
            const AnimClip* clip = ctx.clips->get(anim.clip);
            if (clip) {
                clipName    = clip->name.c_str();
                clipDuration = clip->duration;
            }
        }

        ImGui::Text("Clip"); ImGui::SameLine(100.0f);
        ImGui::TextDisabled("%s", clipName[0] ? clipName : "(unnamed)");
        if (clipDuration > 0.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%.2fs)", clipDuration);
        }

        // ── Clip asset slot — drag an animation file from Assets ────────────
        // Standalone clips (Mixamo clip FBXs) bind to THIS entity's skeleton
        // by bone name. Binding happens immediately if the skeleton is loaded;
        // otherwise at import time (the same path scene loading uses).
        {
            const std::string slotLabel = anim.clipPath.empty()
                ? std::string("(drop animation file here)")
                : std::filesystem::path(anim.clipPath).filename().string();
            ImGui::Text("Clip asset"); ImGui::SameLine(100.0f);
            ImGui::Button(slotLabel.c_str(), {-1, 0});
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string path((const char*)pl->Data);
                    auto bk = UndoStack::snapshotComponent(e, "animator");
                    anim.clipPath = path;
                    anim.time     = 0.0f;
                    if (ctx.clipLibrary && ctx.skeletons && ctx.clips && hasSkin) {
                        const SkinnedMesh& sm = e.get<SkinnedMesh>();
                        if (const Skeleton* sk = ctx.skeletons->get(sm.skeleton)) {
                            AnimClipHandle h = ctx.clipLibrary->load(
                                path, sm.skeleton, *sk, *ctx.clips);
                            if (h.valid()) { anim.clip = h; anim.playing = true; }
                        }
                    }
                    ctx.editor.undoStack.pushPropertyEdit(e, "animator", bk,
                        UndoStack::snapshotComponent(e, "animator"), "Assign clip asset");
                    ctx.editor.sceneDirty = true;
                }
                ImGui::EndDragDropTarget();
            }
            if (!anim.clipPath.empty()) {
                ImGui::Text(""); ImGui::SameLine(100.0f);
                if (ImGui::SmallButton("Clear clip asset")) {
                    auto bk = UndoStack::snapshotComponent(e, "animator");
                    anim.clipPath.clear();
                    anim.clip = {};
                    ctx.editor.undoStack.pushPropertyEdit(e, "animator", bk,
                        UndoStack::snapshotComponent(e, "animator"), "Clear clip asset");
                    ctx.editor.sceneDirty = true;
                }
            }
        }

        // Play/Pause button
        ImGui::Text("Playback"); ImGui::SameLine(100.0f);
        {
            auto bk = UndoStack::snapshotComponent(e, "animator");
            if (ImGui::Button(anim.playing ? "Pause" : "Play", {60, 0})) {
                anim.playing = !anim.playing;
                ctx.editor.undoStack.pushPropertyEdit(e, "animator", bk,
                    UndoStack::snapshotComponent(e, "animator"), "Toggle animation playback");
                ctx.editor.sceneDirty = true;
            }
        }
        ImGui::SameLine();
        {
            auto bk = UndoStack::snapshotComponent(e, "animator");
            if (ImGui::Button("Reset", {50, 0})) {
                anim.time = 0.0f;
                ctx.editor.undoStack.pushPropertyEdit(e, "animator", bk,
                    UndoStack::snapshotComponent(e, "animator"), "Reset animation time");
                ctx.editor.sceneDirty = true;
            }
        }

        // Looping toggle
        ImGui::SameLine();
        {
            auto bk = UndoStack::snapshotComponent(e, "animator");
            if (ImGui::Checkbox("Loop", &anim.looping)) {
                ctx.editor.undoStack.pushPropertyEdit(e, "animator", bk,
                    UndoStack::snapshotComponent(e, "animator"), "Toggle animation looping");
                ctx.editor.sceneDirty = true;
            }
        }

        // Time scrubber
        ImGui::Text("Time"); ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(-1);
        if (clipDuration > 0.0f) {
            ImGui::SliderFloat("##animTime", &anim.time, 0.0f, clipDuration, "%.3fs");
        } else {
            ImGui::DragFloat("##animTime", &anim.time, 0.01f, 0.0f, 100.0f, "%.3fs");
        }
        propEdit(ctx, e, "animator", "Scrub animation time");

        // Speed slider
        ImGui::Text("Speed"); ImGui::SameLine(100.0f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##animSpeed", &anim.speed, -2.0f, 4.0f, "%.2fx");
        propEdit(ctx, e, "animator", "Change animation speed");
    }

    // Remove buttons
    ImGui::Spacing();
    if (hasAnim) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove Animator", {-1, 0})) {
            ctx.editor.undoStack.pushComponentRemove(e, "animator", "Remove Animator");
            e.remove<Animator>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    }
    if (hasSkin) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove SkinnedMesh", {-1, 0})) {
            ctx.editor.undoStack.pushComponentRemove(e, "skinnedMesh", "Remove SkinnedMesh");
            e.remove<SkinnedMesh>();
            // Also remove Animator since it depends on SkinnedMesh
            if (e.has<Animator>()) {
                ctx.editor.undoStack.pushComponentRemove(e, "animator", "Remove Animator");
                e.remove<Animator>();
            }
            ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    }
}

} // namespace inspector_detail
