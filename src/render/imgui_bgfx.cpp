#include "imgui_bgfx.h"

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bx/math.h>

#include <imgui.h>
#include "imgui_impl_glfw.h"

#include "vs_ocornut_imgui.bin.h"
#include "fs_ocornut_imgui.bin.h"
#include "roboto_regular.ttf.h"

#include <cstring>

namespace {

// Precompiled shader bytecode for every backend bgfx supports.
// bgfx::createEmbeddedShader picks the right one at runtime based on the active renderer.
const bgfx::EmbeddedShader kShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER(fs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER_END(),
};

bgfx::VertexLayout  g_layout;
bgfx::ProgramHandle g_program    = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_texUniform = BGFX_INVALID_HANDLE;

bool checkTransientAvail(uint32_t numV, const bgfx::VertexLayout& layout, uint32_t numI) {
    return numV == bgfx::getAvailTransientVertexBuffer(numV, layout)
        && (0 == numI || numI == bgfx::getAvailTransientIndexBuffer(numI));
}

// ImGui's new dynamic texture API: textures live in ImDrawData->Textures and
// have a lifecycle (WantCreate -> OK -> WantUpdates -> WantDestroy -> Destroyed).
// Our job here is to back each ImTextureData with a bgfx::TextureHandle.
void processTextures(ImDrawData* drawData) {
    if (drawData->Textures == nullptr) return;
    for (ImTextureData* tex : *drawData->Textures) {
        switch (tex->Status) {
        case ImTextureStatus_WantCreate: {
            bgfx::TextureHandle h = bgfx::createTexture2D(
                (uint16_t)tex->Width, (uint16_t)tex->Height,
                false, 1, bgfx::TextureFormat::RGBA8, 0
            );
            bgfx::setName(h, "ImGui Texture");
            bgfx::updateTexture2D(h, 0, 0, 0, 0,
                (uint16_t)tex->Width, (uint16_t)tex->Height,
                bgfx::copy(tex->GetPixels(), tex->GetSizeInBytes()));
            tex->SetTexID((ImTextureID)(uintptr_t)h.idx);
            tex->SetStatus(ImTextureStatus_OK);
        } break;

        case ImTextureStatus_WantUpdates: {
            bgfx::TextureHandle h = { (uint16_t)(uintptr_t)tex->GetTexID() };
            if (!bgfx::isValid(h)) break;
            for (ImTextureRect& r : tex->Updates) {
                const uint32_t bpp = tex->BytesPerPixel;
                const bgfx::Memory* mem = bgfx::alloc(r.h * r.w * bpp);
                for (int y = 0; y < r.h; ++y) {
                    std::memcpy(mem->data + y * r.w * bpp,
                                tex->GetPixelsAt(r.x, r.y + y),
                                r.w * bpp);
                }
                bgfx::updateTexture2D(h, 0, 0, r.x, r.y, r.w, r.h, mem);
            }
        } break;

        case ImTextureStatus_WantDestroy: {
            bgfx::TextureHandle h = { (uint16_t)(uintptr_t)tex->GetTexID() };
            if (bgfx::isValid(h)) bgfx::destroy(h);
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        } break;

        default: break;
        }
    }
}

} // namespace

void imguiInit(GLFWwindow* window, float fontSize) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr; // disable imgui.ini for now

    ImGui::StyleColorsDark();

    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(
            (void*)s_robotoRegularTtf, sizeof(s_robotoRegularTtf), fontSize, &cfg);
    }

    // GLFW backend handles all input plumbing for us.
    ImGui_ImplGlfw_InitForOther(window, true);

    // bgfx side: load shaders, create vertex layout and texture sampler uniform.
    bgfx::RendererType::Enum type = bgfx::getRendererType();
    g_program = bgfx::createProgram(
        bgfx::createEmbeddedShader(kShaders, type, "vs_ocornut_imgui"),
        bgfx::createEmbeddedShader(kShaders, type, "fs_ocornut_imgui"),
        true
    );

    g_layout
        .begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
        .end();

    g_texUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
}

void imguiShutdown() {
    ImGui_ImplGlfw_Shutdown();

    // Release any textures still alive in ImGui's atlas.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount == 1 && tex->TexID != ImTextureID_Invalid) {
            bgfx::TextureHandle h = { (uint16_t)(uintptr_t)tex->GetTexID() };
            if (bgfx::isValid(h)) bgfx::destroy(h);
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }

    if (bgfx::isValid(g_texUniform)) bgfx::destroy(g_texUniform);
    if (bgfx::isValid(g_program))    bgfx::destroy(g_program);

    ImGui::DestroyContext();
}

void imguiNewFrame() {
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void imguiRender(bgfx::ViewId viewId) {
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    processTextures(drawData);

    int dispW = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int dispH = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (dispW <= 0 || dispH <= 0) return;

    bgfx::setViewName(viewId, "ImGui");
    bgfx::setViewMode(viewId, bgfx::ViewMode::Sequential);

    const bgfx::Caps* caps = bgfx::getCaps();
    {
        float ortho[16];
        const float L = drawData->DisplayPos.x;
        const float T = drawData->DisplayPos.y;
        const float W = drawData->DisplaySize.x;
        const float H = drawData->DisplaySize.y;
        bx::mtxOrtho(ortho, L, L + W, T + H, T, 0.0f, 1000.0f, 0.0f, caps->homogeneousDepth);
        bgfx::setViewTransform(viewId, nullptr, ortho);
        bgfx::setViewRect(viewId, 0, 0, (uint16_t)dispW, (uint16_t)dispH);
    }

    const ImVec2 clipPos   = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* dl = drawData->CmdLists[n];
        const uint32_t numV = (uint32_t)dl->VtxBuffer.size();
        const uint32_t numI = (uint32_t)dl->IdxBuffer.size();

        if (!checkTransientAvail(numV, g_layout, numI)) break;

        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer  tib;
        bgfx::allocTransientVertexBuffer(&tvb, numV, g_layout);
        bgfx::allocTransientIndexBuffer(&tib, numI, sizeof(ImDrawIdx) == 4);
        std::memcpy(tvb.data, dl->VtxBuffer.Data, numV * sizeof(ImDrawVert));
        std::memcpy(tib.data, dl->IdxBuffer.Data, numI * sizeof(ImDrawIdx));

        bgfx::Encoder* enc = bgfx::begin();

        for (const ImDrawCmd* cmd = dl->CmdBuffer.begin(); cmd != dl->CmdBuffer.end(); ++cmd) {
            if (cmd->UserCallback) { cmd->UserCallback(dl, cmd); continue; }
            if (cmd->ElemCount == 0) continue;

            uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA
                | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);

            bgfx::TextureHandle th = BGFX_INVALID_HANDLE;
            ImTextureID tid = cmd->GetTexID();
            if (tid != ImTextureID_Invalid) th = { (uint16_t)(uintptr_t)tid };

            ImVec4 clip;
            clip.x = (cmd->ClipRect.x - clipPos.x) * clipScale.x;
            clip.y = (cmd->ClipRect.y - clipPos.y) * clipScale.y;
            clip.z = (cmd->ClipRect.z - clipPos.x) * clipScale.x;
            clip.w = (cmd->ClipRect.w - clipPos.y) * clipScale.y;

            if (clip.x < dispW && clip.y < dispH && clip.z >= 0.0f && clip.w >= 0.0f) {
                const uint16_t x = (uint16_t)bx::max(clip.x, 0.0f);
                const uint16_t y = (uint16_t)bx::max(clip.y, 0.0f);
                enc->setScissor(x, y,
                    (uint16_t)bx::min(clip.z, 65535.0f) - x,
                    (uint16_t)bx::min(clip.w, 65535.0f) - y);
                enc->setState(state);
                enc->setTexture(0, g_texUniform, th);
                enc->setVertexBuffer(0, &tvb, cmd->VtxOffset, numV);
                enc->setIndexBuffer(&tib, cmd->IdxOffset, cmd->ElemCount);
                enc->submit(viewId, g_program);
            }
        }
        bgfx::end(enc);
    }
}
