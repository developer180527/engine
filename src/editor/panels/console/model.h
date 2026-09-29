#pragma once
// ── Console model — reading the log ring for a panel, without a GUI ──────────
//
// Both consoles read the same ring (core/logger.h) and differ in who they are
// for: the Console shows a GAME builder their content and every failure; the
// Internal Console shows an ENGINE developer the machinery. What each shows is
// a filter over the ring, and that filter — plus "Clear" (which moves the
// view, not the ring) and the error/warning counts — is this file. The ImGui
// panels and the libgui front end draw the same lines.
#include "core/logger.h"
#include "editor/core/color.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace console {

// One colour per level, shared so every console agrees.
inline edui::Color levelColor(elog::Level l) {
    static const edui::Color c[(int)elog::Level::Count] = {
        {0.45f, 0.45f, 0.55f, 1},   // Trace
        {0.50f, 0.50f, 0.50f, 1},   // Debug
        {0.85f, 0.85f, 0.85f, 1},   // Info
        {0.30f, 0.90f, 0.40f, 1},   // Success
        {1.00f, 0.80f, 0.20f, 1},   // Warning
        {1.00f, 0.35f, 0.35f, 1},   // Error
    };
    const int i = (int)l;
    return (i >= 0 && i < (int)elog::Level::Count) ? c[i] : c[(int)elog::Level::Info];
}

enum class Audience { Game, Engine };

struct Line {
    double      t = 0;
    uint64_t    frame = 0;
    elog::Level level = elog::Level::Info;
    std::string cat;
    std::string msg;      // with " …" appended when the ring truncated it
};

class LogView {
public:
    explicit LogView(Audience a) : m_audience(a) {
        // The game console never offers Debug/Trace; the internal one shows all.
        for (int i = 0; i < (int)elog::Level::Count; ++i)
            m_show[i] = a == Audience::Engine || i >= (int)elog::Level::Info;
    }

    Audience audience() const { return m_audience; }

    // Display filters: hide what was recorded (the engine-side masks decide
    // what gets recorded at all — that is where the cost is).
    bool  showing(elog::Level l) const { return m_show[(int)l]; }
    void  setShowing(elog::Level l, bool on) { m_show[(int)l] = on; }
    std::string& find() { return m_find; }
    // Internal console only: hide game-facing chatter below Warning.
    bool& engineOnly() { return m_engineOnly; }

    // "Clear": from now on, show only lines written after this moment.
    void clear() { m_viewFrom = elog::written(); }

    // The lines to show, oldest first, at most `maxLines` (the most recent
    // ones), and this view's error/warning counts over everything it covers.
    struct Result { std::vector<Line> lines; uint32_t warnings = 0, errors = 0; };
    Result collect(size_t maxLines = 2000) const {
        Result r;
        const uint64_t total = elog::written();
        const uint64_t begin = elog::oldest() > m_viewFrom ? elog::oldest() : m_viewFrom;
        elog::Entry e;
        for (uint64_t s = begin; s < total; ++s) {
            // `read` refuses a slot recycled mid-copy: a flooding subsystem
            // shows fewer lines rather than spliced ones.
            if (!elog::read(s, e)) continue;
            const int li = (int)e.level;
            if (li < 0 || li >= (int)elog::Level::Count) continue;
            const char* cat = e.cat ? e.cat : "?";
            if (m_audience == Audience::Game) {
                // THE audience rule for the game console.
                if (!elog::visibleToGame(elog::category(cat), e.level)) continue;
                if (e.level == elog::Level::Warning) ++r.warnings;
                if (e.level == elog::Level::Error)   ++r.errors;
            } else {
                if (e.level == elog::Level::Warning) ++r.warnings;
                if (e.level == elog::Level::Error)   ++r.errors;
                if (m_engineOnly && e.level < elog::Level::Warning &&
                    elog::visibleToGame(elog::category(cat), elog::Level::Info))
                    continue;
            }
            if (!m_show[li]) continue;
            if (!m_find.empty() && !std::strstr(e.msg, m_find.c_str()) &&
                !std::strstr(cat, m_find.c_str()))
                continue;
            Line ln;
            ln.t = e.t; ln.frame = e.frame; ln.level = e.level; ln.cat = cat;
            ln.msg = e.msg;
            if (e.truncated) ln.msg += " …";
            r.lines.push_back(std::move(ln));
        }
        if (r.lines.size() > maxLines)
            r.lines.erase(r.lines.begin(), r.lines.end() - (long)maxLines);
        return r;
    }

private:
    Audience    m_audience;
    bool        m_show[(int)elog::Level::Count] = {};
    std::string m_find;
    bool        m_engineOnly = false;
    uint64_t    m_viewFrom = 0;
};

// The internal console records engine detail only while it is open (demand
// gating, elog::acquireWatch). acquire/release is a REFCOUNT, so this is
// edge-triggered: call every frame with whether the panel is open.
class WatchWhileOpen {
public:
    void update(bool open) {
        if (open == m_watching) return;
        m_watching = open;
        if (open) elog::acquireWatch(); else elog::releaseWatch();
    }
    ~WatchWhileOpen() { if (m_watching) elog::releaseWatch(); }
private:
    bool m_watching = false;
};

}  // namespace console
