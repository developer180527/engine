#pragma once
// ── Terminal model — a shell session for a panel, without a GUI ──────────────
// Runs a command in the project root, collects its output, and keeps the
// command history that Up/Down walk. The ImGui Console's Terminal tab and the
// libgui front end draw the same session.
//
// Commands run SYNCHRONOUSLY (popen) on the calling thread — the panel freezes
// until the command exits. That is the existing behaviour, kept; a long
// command wants a job, not this.
#include "core/logger.h"
#include "editor/core/color.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define ENGINE_POPEN(cmd, mode)  ::_popen(cmd, mode)
#  define ENGINE_PCLOSE(pipe)      ::_pclose(pipe)
#else
#  define ENGINE_POPEN(cmd, mode)  ::popen(cmd, mode)
#  define ENGINE_PCLOSE(pipe)      ::pclose(pipe)
#endif

namespace term {

class TerminalSession {
public:
    std::string              projectRoot;
    std::vector<std::string> history;      // output lines
    std::vector<std::string> cmdHistory;   // commands, oldest first
    bool                     scrollToBottom = true;

    void setProjectRoot(const std::string& root) {
        projectRoot = root;
        history.clear();
        history.push_back("Terminal — " + root);
        history.push_back("Type a command and press Enter.");
        history.push_back("");
    }

    // `cd` into `dir`, quoted so the shell reads the path as exactly those
    // bytes. POSIX: single quotes, inside which nothing expands (double quotes
    // still expand $, ` and \), and a ' is written '\''. Windows: double
    // quotes, which cmd.exe does not expand inside for these characters, and a
    // Windows path cannot contain a " at all.
    static std::string cdInto(const std::string& dir) {
#if defined(_WIN32)
        return "cd /d \"" + dir + "\"";
#else
        std::string q = "cd '";
        for (char c : dir) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
#endif
    }

    void runCommand(const std::string& cmd) {
        if (cmd.empty()) return;
        cmdHistory.push_back(cmd);
        m_historyIdx = -1;
        history.push_back("$ " + cmd);

        // The root is QUOTED (a project folder with a space broke every command,
        // on every OS), and on Windows `cd /d`: plain `cd` does not change drive,
        // so with the project on D: and the shell starting on C: the cd failed
        // and nothing after && ran (editor_tool_panels_test, Windows CI, WO-038).
        const std::string full = cdInto(projectRoot) + " && " + cmd + " 2>&1";
        FILE* pipe = ENGINE_POPEN(full.c_str(), "r");
        if (!pipe) { history.push_back("[error] popen failed"); return; }
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();   // Windows pipes deliver \r\n
            history.push_back(line);
        }
        ENGINE_PCLOSE(pipe);
        history.push_back("");
        scrollToBottom = true;
        LOG_DEBUG("Terminal", "Ran: %s", cmd.c_str());
    }

    // Up (older = +1) / Down (newer = -1) through past commands, as a shell
    // does. Returns the command to put in the input, or "" past the newest.
    // `changed` says whether the input should be replaced at all.
    std::string stepHistory(int direction, bool& changed) {
        int idx = m_historyIdx;
        if (direction > 0) idx = std::min((int)cmdHistory.size() - 1, idx + 1);
        else               idx = std::max(-1, idx - 1);
        changed = idx != m_historyIdx;
        m_historyIdx = idx;
        return idx >= 0 ? cmdHistory[cmdHistory.size() - 1 - (size_t)idx] : std::string();
    }

    // How a line is coloured: commands green, errors red, output plain.
    static edui::Color lineColor(const std::string& line) {
        if (!line.empty() && line[0] == '$') return {0.40f, 0.90f, 0.50f, 1};
        if (line.find("[error]") != std::string::npos) return {1.00f, 0.35f, 0.35f, 1};
        return {0.85f, 0.85f, 0.85f, 1};
    }

private:
    int m_historyIdx = -1;
};

}  // namespace term
