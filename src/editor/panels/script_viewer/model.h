#pragma once
// ── Script viewer model — opening and highlighting code, without a GUI ───────
// The set of open documents (opening one twice focuses it), reading a file
// (1 MB cap, tabs to four spaces) and a small syntax highlighter for Lua,
// Python and C++: lines of coloured spans. The ImGui viewer and the libgui
// front end draw the same spans.
#include "editor/core/color.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace code {

enum class Lang { Lua, Python, Cpp, Plain };
struct Span { edui::Color col; std::string text; };
using Lines = std::vector<std::vector<Span>>;

struct Doc {
    std::string path, title, text;
    Lang  lang = Lang::Plain;
    bool  open = true;
    bool  truncated = false;
    Lines lines;
};

struct Highlighter {
    static edui::Color cDefault() { return edui::Color{0.8314f, 0.8314f, 0.8314f, 1.0f}; }
    static edui::Color cKeyword() { return edui::Color{0.4314f, 0.6667f, 0.8824f, 1.0f}; }
    static edui::Color cString()  { return edui::Color{0.8078f, 0.5686f, 0.4706f, 1.0f}; }
    static edui::Color cComment() { return edui::Color{0.4314f, 0.5882f, 0.4118f, 1.0f}; }
    static edui::Color cNumber()  { return edui::Color{0.7098f, 0.8078f, 0.6588f, 1.0f}; }

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

    static void emit(std::vector<std::vector<Span>>& lines, edui::Color col,
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

    static std::vector<std::vector<Span>> highlight(const std::string& t, Lang lang) {
        std::vector<std::vector<Span>> lines; lines.push_back({});
        const auto& kw = keywords(lang);
        const char* lineCmt = (lang==Lang::Lua) ? "--" :
                              (lang==Lang::Python) ? "#" :
                              (lang==Lang::Cpp) ? "//" : nullptr;
        size_t i = 0, n = t.size(), runStart = 0;

        auto flushDefault = [&](size_t from, size_t to) {
            if (to > from) emit(lines, cDefault(), t.data()+from, t.data()+to);
        };
        auto emitTok = [&](edui::Color col, size_t from, size_t to) {
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

inline const char* langName(Lang l) { return Highlighter::langName(l); }

// The open documents. open() of a path already open focuses it instead.
class DocSet {
public:
    void open(const std::string& path) {
        for (auto& d : m_docs)
            if (d.path == path) { d.open = true; m_focus = path; return; }
        Doc d;
        d.path  = path;
        d.title = std::filesystem::path(path).filename().string();
        d.lang  = Highlighter::langFromExt(Highlighter::lowerExt(path));
        d.text  = Highlighter::readFile(path, d.truncated);
        d.lines = Highlighter::highlight(d.text, d.lang);
        m_focus = path;
        m_docs.push_back(std::move(d));
    }
    std::vector<Doc>& docs() { return m_docs; }
    // The path to bring to the front this frame, once.
    std::string takeFocus() { std::string f; f.swap(m_focus); return f; }
    // Drop documents whose window the user closed.
    void prune() {
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(),
                     [](const Doc& d) { return !d.open; }), m_docs.end());
    }
    bool anyOpen() const { return !m_docs.empty(); }
private:
    std::vector<Doc> m_docs;
    std::string      m_focus;
};

}  // namespace code
