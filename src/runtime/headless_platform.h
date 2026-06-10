#pragma once
#include "runtime/platform.h"

// No window, no GPU. nativeWindowHandle() returns null, which makes
// EngineRuntime skip renderer initialization — ECS, assets, animation,
// physics and scripting all run normally. For dedicated servers, asset
// pipelines, and headless tests.
class HeadlessPlatform final : public IPlatform {
public:
    bool init(const PlatformConfig& cfg) override {
        m_width  = cfg.width;
        m_height = cfg.height;
        return true;
    }
    void shutdown() override {}

    void* nativeWindowHandle() const override { return nullptr; }
    void  pollEvents() override {}
    bool  shouldClose() const override { return m_closeRequested; }
    void  requestClose() override { m_closeRequested = true; }
    void  framebufferSize(int& w, int& h) const override {
        w = m_width; h = m_height;
    }

private:
    int  m_width  = 0;
    int  m_height = 0;
    bool m_closeRequested = false;
};
