#pragma once
// Read-only, syntax-highlighted code viewer. Each opened file becomes its own
// floating ImGui window (filename title + built-in close button). Highlighting
// is a small stateful lexer covering Lua / Python / C++: keywords, strings
// (incl. multi-line), comments (incl. block), and numbers. No external deps.
#include <imgui.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include <cstring>
#include <cctype>

class ScriptViewer {
public:
    void open(const std::string& path) {
        for (auto& d : m_docs)
            if (d.path == path) { d.open = true; m_focus = path; return; }
        Doc d;
        d.path  = path;
        d.title = std::filesystem::path(path).filename().string();
        d.lang  = langFromExt(lowerExt(path));
        d.text  = readFile(path, d.truncated);
        d.lines = highlight(d.text, d.lang);
        m_focus = path;
        m_docs.push_back(std::move(d));
    }

    void draw() {
        for (auto& d : m_docs) {
            if (!d.open) continue;
            if (d.path == m_focus) ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
            std::string id = d.title + "###scriptview_" + d.path;
            if (ImGui::Begin(id.c_str(), &d.open)) {
                ImGui::TextDisabled("%s", langName(d.lang));
                ImGui::SameLine(); ImGui::TextDisabled("  %s", d.path.c_str());
                if (d.truncated) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.9f,0.7f,0.3f,1), " (truncated)");
                }
                ImGui::Separator();
                drawCode(d);
            }
            ImGui::End();
        }
        m_focus.clear();
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(),
                     [](const Doc& d){ return !d.open; }), m_docs.end());
    }

    bool anyOpen() const { return !m_docs.empty(); }

private:
    enum class Lang { Lua, Python, Cpp, Plain };
    struct Span { ImU32 col; std::string text; };
    struct Doc {
        std::string path, title, text;
        Lang lang = Lang::Plain;
        bool open = true;
        bool truncated = false;
        std::vector<std::vector<Span>> lines;
    };
    std::vector<Doc> m_docs;
    std::string      m_focus;

    static ImU32 cDefault() { return IM_COL32(212,212,212,255); }
    static ImU32 cKeyword() { return IM_COL32(110,170,225,255); }
    static ImU32 cString()  { return IM_COL32(206,145,120,255); }
    static ImU32 cComment() { return IM_COL32(110,150,105,255); }
    static ImU32 cNumber()  { return IM_COL32(181,206,168,255); }

    static const char* langName(Lang l) {
        switch (l) { case Lang::Lua: return "Lua"; case Lang::Python: return "Python";
                     case Lang::Cpp: return "C++"; default: return "Text"; }
    }
    static std::string lowerExt(const std::string& path) {
        std::string e = std::filesystem::path(path).extension().string();
        for (auto& c : e) c = (char)std::tolower((unsigned char)c);
        return e;
    }
    static Lang langFromExt(const std::string& e) {
        if (e==".lua") return Lang::Lua;
        if (e==".py")  return Lang::Python;
        if (e==".cpp"||e==".cc"||e==".cxx"||e==".c"||e==".h"||e==".hpp"||e==".hxx"||e==".inl")
            return Lang::Cpp;
        return Lang::Plain;
    }
    static std::string readFile(const std::string& path, bool& truncated) {
        truncated = false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return "(could not open file)";
        std::stringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        constexpr size_t kMax = 1u << 20; // 1 MB cap
        if (s.size() > kMax) { s.resize(kMax); truncated = true; }
        std::string out; out.reserve(s.size());
        for (char c : s) {
            if (c == '\r') continue;
            if (c == '\t') { out += "    "; continue; }
            out.push_back(c);
        }
        return out;
    }

    void drawCode(Doc& d) {
        ImGui::BeginChild("##code", ImVec2(0,0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
        int ln = 1;
        for (auto& line : d.lines) {
            ImGui::TextDisabled("%4d", ln++);
            ImGui::SameLine(0, 14);
            if (line.empty()) { ImGui::TextUnformatted(" "); continue; }
            for (size_t s = 0; s < line.size(); ++s) {
                if (s) ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, line[s].col);
                ImGui::TextUnformatted(line[s].text.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    // ── lexer ───────────────────────────────────────────────────────────
    static const std::unordered_set<std::string>& keywords(Lang l) {
        static const std::unordered_set<std::string> lua = {
            "and","break","do","else","elseif","end","false","for","function","goto",
            "if","in","local","nil","not","or","repeat","return","then","true","until",
            "while","self"};
        static const std::unordered_set<std::string> py = {
            "False","None","True","and","as","assert","async","await","break","class",
            "continue","def","del","elif","else","except","finally","for","from","global",
            "if","import","in","is","lambda","nonlocal","not","or","pass","raise","return",
            "try","while","with","yield","self"};
        static const std::unordered_set<std::string> cpp = {
            "alignas","alignof","auto","bool","break","case","catch","char","class","const",
            "constexpr","continue","default","delete","do","double","else","enum","explicit",
            "export","extern","false","float","for","friend","goto","if","inline","int","long",
            "mutable","namespace","new","noexcept","nullptr","operator","override","private",
            "protected","public","return","short","signed","sizeof","static","struct","switch",
            "template","this","throw","true","try","typedef","typename","union","unsigned",
            "using","virtual","void","volatile","while","uint8_t","uint32_t","int32_t",
            "uint64_t","int64_t","size_t"};
        static const std::unordered_set<std::string> none;
        switch (l) { case Lang::Lua: return lua; case Lang::Python: return py;
                     case Lang::Cpp: return cpp; default: return none; }
    }

    static void emit(std::vector<std::vector<Span>>& lines, ImU32 col,
                     const char* b, const char* e) {
        if (lines.empty()) lines.push_back({});
        const char* s = b;
        for (const char* p = b; p < e; ++p) {
            if (*p == '\n') {
                if (p > s) lines.back().push_back({col, std::string(s, p)});
                lines.push_back({});
                s = p + 1;
            }
        }
        if (e > s) lines.back().push_back({col, std::string(s, e)});
    }
    static bool match(const std::string& t, size_t i, const char* lit) {
        size_t n = std::strlen(lit);
        return i + n <= t.size() && t.compare(i, n, lit) == 0;
    }

    std::vector<std::vector<Span>> highlight(const std::string& t, Lang lang) {
        std::vector<std::vector<Span>> lines; lines.push_back({});
        const auto& kw = keywords(lang);
        const char* lineCmt = (lang==Lang::Lua) ? "--" :
                              (lang==Lang::Python) ? "#" :
                              (lang==Lang::Cpp) ? "//" : nullptr;
        size_t i = 0, n = t.size(), runStart = 0;

        auto flushDefault = [&](size_t from, size_t to) {
            if (to > from) emit(lines, cDefault(), t.data()+from, t.data()+to);
        };
        auto emitTok = [&](ImU32 col, size_t from, size_t to) {
            flushDefault(runStart, from);
            emit(lines, col, t.data()+from, t.data()+to);
            runStart = to;
        };

        while (i < n) {
            char c = t[i];
            // Lua long comment (before line comment)
            if (lang==Lang::Lua && match(t,i,"--[[")) {
                size_t j=i+4; while (j<n && !match(t,j,"]]")) ++j; j=(j<n)?j+2:n;
                emitTok(cComment(), i, j); i=j; continue;
            }
            if (lineCmt && match(t,i,lineCmt)) {
                size_t j=i; while (j<n && t[j]!='\n') ++j;
                emitTok(cComment(), i, j); i=j; continue;
            }
            if (lang==Lang::Cpp && match(t,i,"/*")) {
                size_t j=i+2; while (j<n && !match(t,j,"*/")) ++j; j=(j<n)?j+2:n;
                emitTok(cComment(), i, j); i=j; continue;
            }
            if (lang==Lang::Python && (match(t,i,"\"\"\"") || match(t,i,"'''"))) {
                std::string d(3, t[i]);
                size_t j=i+3; while (j<n && !match(t,j,d.c_str())) ++j; j=(j<n)?j+3:n;
                emitTok(cString(), i, j); i=j; continue;
            }
            if (lang==Lang::Lua && match(t,i,"[[")) {
                size_t j=i+2; while (j<n && !match(t,j,"]]")) ++j; j=(j<n)?j+2:n;
                emitTok(cString(), i, j); i=j; continue;
            }
            if (c=='"' || c=='\'') {
                size_t j=i+1;
                while (j<n && t[j]!=c) {
                    if (t[j]=='\\' && j+1<n) { ++j; }
                    else if (t[j]=='\n') break;
                    ++j;
                }
                j = (j<n && t[j]==c) ? j+1 : j;
                emitTok(cString(), i, j); i=j; continue;
            }
            if (std::isdigit((unsigned char)c) ||
                (c=='.' && i+1<n && std::isdigit((unsigned char)t[i+1]))) {
                size_t j=i;
                while (j<n && (std::isalnum((unsigned char)t[j]) || t[j]=='.' || t[j]=='_')) ++j;
                emitTok(cNumber(), i, j); i=j; continue;
            }
            if (std::isalpha((unsigned char)c) || c=='_') {
                size_t j=i;
                while (j<n && (std::isalnum((unsigned char)t[j]) || t[j]=='_')) ++j;
                if (kw.count(t.substr(i, j-i))) emitTok(cKeyword(), i, j);
                i=j; continue;  // non-keyword stays in the default run
            }
            ++i; // operators/punct/whitespace accumulate as default
        }
        flushDefault(runStart, n);
        return lines;
    }
};
