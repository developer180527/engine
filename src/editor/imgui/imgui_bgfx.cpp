#include "imgui_bgfx.h"
#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bx/math.h>
#include <imgui.h>
// The ImGui PLATFORM backend is the one genuinely backend-specific dependency
// in the editor; this file is the single place it is named.
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    #include "imgui_impl_sdl3.h"
    #include <SDL3/SDL.h>
#else
    #include "imgui_impl_glfw.h"
#endif
#include "vs_ocornut_imgui.bin.h"
#include "fs_ocornut_imgui.bin.h"
#include "roboto_regular.ttf.h"
#include "fa_solid_900.ttf.h"
#include "core/memory/mem.h"           // ImGui → Editor heap
#include "editor/editor_icons.h"
#include "project/project_context.h"   // homeDir() for the stable ini path
#include <engine/engine_api.h>         // EngineUiBackend — kit/plugin editor UI
#include <cstring>
#include <string>
#include <filesystem>
#include <system_error>
#include "editor/window_ops.h"

namespace {
const bgfx::EmbeddedShader kShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER(fs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER_END(),
};
bgfx::VertexLayout  g_layout;
bgfx::ProgramHandle g_program    = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_texUniform = BGFX_INVALID_HANDLE;
bgfx::ViewId        g_nextViewId = 200;
struct BgfxViewportData {
    bgfx::FrameBufferHandle fb     = BGFX_INVALID_HANDLE;
    bgfx::ViewId            viewId = 0;
};
bool checkTransientAvail(uint32_t numV, const bgfx::VertexLayout& layout, uint32_t numI) {
    return numV == bgfx::getAvailTransientVertexBuffer(numV, layout)
        && (0 == numI || numI == bgfx::getAvailTransientIndexBuffer(numI));
}
void processTextures(ImDrawData* drawData) {
    if (drawData->Textures == nullptr) return;
    for (ImTextureData* tex : *drawData->Textures) {
        switch (tex->Status) {
        case ImTextureStatus_WantCreate: {
            bgfx::TextureHandle h = bgfx::createTexture2D(
                (uint16_t)tex->Width, (uint16_t)tex->Height,
                false, 1, bgfx::TextureFormat::RGBA8, 0);
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
                const uint32_t      bpp = tex->BytesPerPixel;
                const bgfx::Memory* mem = bgfx::alloc(r.w * r.h * bpp);
                const uint8_t*      src = (const uint8_t*)tex->GetPixels()
                    + (r.y * tex->Width + r.x) * bpp;
                for (int row = 0; row < r.h; ++row)
                    std::memcpy(mem->data + row * r.w * bpp,
                                src + row * tex->Width * bpp, r.w * bpp);
                bgfx::updateTexture2D(h, 0, 0, r.x, r.y, r.w, r.h, mem, r.w * bpp);
            }
            tex->SetStatus(ImTextureStatus_OK);
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
void renderDrawData(ImDrawData* drawData, bgfx::ViewId viewId,
                    bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE) {
    const int dispW = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int dispH = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (dispW <= 0 || dispH <= 0) return;
    bgfx::setViewName(viewId, "ImGui");
    bgfx::setViewMode(viewId, bgfx::ViewMode::Sequential);
    bgfx::setViewFrameBuffer(viewId, fb);
    if (bgfx::isValid(fb))
        bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR, 0x1a1a1aff, 1.0f, 0);
    const bgfx::Caps* caps = bgfx::getCaps();
    float ortho[16];
    bx::mtxOrtho(ortho,
        drawData->DisplayPos.x, drawData->DisplayPos.x + drawData->DisplaySize.x,
        drawData->DisplayPos.y + drawData->DisplaySize.y, drawData->DisplayPos.y,
        0.0f, 1000.0f, 0.0f, caps->homogeneousDepth);
    bgfx::setViewTransform(viewId, nullptr, ortho);
    bgfx::setViewRect(viewId, 0, 0, (uint16_t)dispW, (uint16_t)dispH);
    const ImVec2 clipPos   = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* dl   = drawData->CmdLists[n];
        const uint32_t    numV = (uint32_t)dl->VtxBuffer.size();
        const uint32_t    numI = (uint32_t)dl->IdxBuffer.size();
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
            const uint64_t state =
                BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
                BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
            bgfx::TextureHandle th = BGFX_INVALID_HANDLE;
            ImTextureID tid = cmd->GetTexID();
            if (tid != ImTextureID_Invalid) th = { (uint16_t)(uintptr_t)tid };
            const float cx = (cmd->ClipRect.x - clipPos.x) * clipScale.x;
            const float cy = (cmd->ClipRect.y - clipPos.y) * clipScale.y;
            const float cz = (cmd->ClipRect.z - clipPos.x) * clipScale.x;
            const float cw = (cmd->ClipRect.w - clipPos.y) * clipScale.y;
            if (cx < dispW && cy < dispH && cz >= 0.0f && cw >= 0.0f) {
                const uint16_t sx = (uint16_t)bx::max(cx, 0.0f);
                const uint16_t sy = (uint16_t)bx::max(cy, 0.0f);
                enc->setScissor(sx, sy,
                    (uint16_t)bx::min(cz, 65535.0f) - sx,
                    (uint16_t)bx::min(cw, 65535.0f) - sy);
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
void setupViewportCallbacks() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_CreateWindow = [](ImGuiViewport* vp) {
        auto* d   = new BgfxViewportData;
        d->viewId = g_nextViewId++;
        edwin::WindowHandle win = vp->PlatformHandle;
        int fbW, fbH; edwin::framebufferSize(win, fbW, fbH);
        d->fb = bgfx::createFrameBuffer(edwin::nativeWindowHandle(win),
                                        (uint16_t)fbW, (uint16_t)fbH);
        vp->RendererUserData = d;
    };
    pio.Renderer_DestroyWindow = [](ImGuiViewport* vp) {
        auto* d = (BgfxViewportData*)vp->RendererUserData;
        if (!d) return;
        bgfx::frame();
        if (bgfx::isValid(d->fb)) bgfx::destroy(d->fb);
        delete d; vp->RendererUserData = nullptr;
    };
    pio.Renderer_SetWindowSize = [](ImGuiViewport* vp, ImVec2) {
        auto* d = (BgfxViewportData*)vp->RendererUserData;
        if (!d) return;
        edwin::WindowHandle win = vp->PlatformHandle;
        int fbW, fbH; edwin::framebufferSize(win, fbW, fbH);
        if (bgfx::isValid(d->fb)) bgfx::destroy(d->fb);
        d->fb = bgfx::createFrameBuffer(edwin::nativeWindowHandle(win),
                                        (uint16_t)fbW, (uint16_t)fbH);
    };
    pio.Renderer_RenderWindow = [](ImGuiViewport* vp, void*) {
        auto* d = (BgfxViewportData*)vp->RendererUserData;
        if (!d || !bgfx::isValid(d->fb)) return;
        processTextures(vp->DrawData);
        renderDrawData(vp->DrawData, d->viewId, d->fb);
    };
}
} // namespace
void imguiInit(void* window, float fontSize) {
    IMGUI_CHECKVERSION();
    // Editor heap — set BEFORE the context so every ImGui allocation routes.
    ImGui::SetAllocatorFunctions(
        [](size_t sz, void*) { return mem::alloc(sz, 16, mem::Tag::Editor); },
        [](void* p, void*) { mem::free(p); });
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    // Panels dragged out of the main window become real OS windows. Let them
    // wear native decorations — title bar (synced from the panel name by the
    // GLFW backend), close button, and the platform's window controls
    // (traffic lights on macOS). Flip back to true for borderless ImGui-only
    // chrome. NOTE: ImGui still draws its own title bar inside, so a torn-off
    // window shows both the OS bar and ImGui's.
    io.ConfigViewportsNoDecoration = false;

    // Docking layout persists across runs. Pin it to a stable per-user path so
    // it survives no matter which directory the editor is launched from (the
    // default "imgui.ini" is relative to the CWD and easily lost).
    static std::string s_iniPath = [] {
        auto dir = ProjectContext::homeDir() / ".engine";
        std::error_code ec; std::filesystem::create_directories(dir, ec);
        return (dir / "editor_layout.ini").string();
    }();
    io.IniFilename = s_iniPath.c_str();
    ImGui::StyleColorsDark();        // neutral base; editor applies its own theme
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding              = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(
            (void*)s_robotoRegularTtf, sizeof(s_robotoRegularTtf), fontSize, &cfg);

        // Merge Font Awesome 6 icons into the same font atlas.
        // MergeMode = true appends glyphs to the previous font instead of
        // creating a new ImFont*. GlyphMinAdvanceX keeps icons monospaced.
        ImFontConfig iconCfg;
        iconCfg.MergeMode          = true;
        iconCfg.PixelSnapH         = true;
        iconCfg.FontDataOwnedByAtlas = false;
        iconCfg.GlyphMinAdvanceX   = fontSize;  // monospaced icons
        static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        io.Fonts->AddFontFromMemoryTTF(
            (void*)s_faSolid900Ttf, sizeof(s_faSolid900Ttf),
            fontSize - 1.0f, &iconCfg, iconRanges);
    }
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    // InitForOther: bgfx owns the device, so ImGui must not create a graphics
    // context of its own. Events reach it via imguiProcessNativeEvent().
    ImGui_ImplSDL3_InitForOther((SDL_Window*)window);
#else
    ImGui_ImplGlfw_InitForOther((GLFWwindow*)window, true);
#endif
    bgfx::RendererType::Enum type = bgfx::getRendererType();
    g_program = bgfx::createProgram(
        bgfx::createEmbeddedShader(kShaders, type, "vs_ocornut_imgui"),
        bgfx::createEmbeddedShader(kShaders, type, "fs_ocornut_imgui"), true);
    g_layout.begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
        .end();
    g_texUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    setupViewportCallbacks();

    // Register the editor UI backend so kits/plugins can draw via the engineUi*
    // facade (they never touch ImGui). Non-capturing lambdas -> C fn pointers.
    static const EngineUiBackend s_uiBackend = {
        [](const char* s) { ImGui::TextUnformatted(s); },
        [](const char* l) { return ImGui::Button(l); },
        [](const char* l, float* v, float lo, float hi) { return ImGui::SliderFloat(l, v, lo, hi); },
        [](const char* l, bool* v) { return ImGui::Checkbox(l, v); },
        []() { ImGui::Separator(); },
    };
    engineUiSetBackend(&s_uiBackend);
}
void imguiShutdown() {
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    ImGui_ImplSDL3_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
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
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    ImGui_ImplSDL3_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();
}
void imguiRender(bgfx::ViewId viewId) {
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    processTextures(drawData);
    renderDrawData(drawData, viewId);
}
void imguiRenderViewports() {
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void imguiProcessNativeEvent(const void* nativeEvent) {
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    // SDL's queue is pumped by the platform; ImGui only observes. Without this
    // the editor would render but receive no keyboard, mouse or text at all —
    // GLFW's backend installs its own callbacks instead and needs nothing here.
    if (nativeEvent)
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(nativeEvent));
#else
    (void)nativeEvent;
#endif
}
