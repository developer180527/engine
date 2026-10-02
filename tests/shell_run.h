#pragma once
// ── shell_run — std::system, with the same meaning on every OS ──────────────
//
// Two differences made the tests that run tools fail on Windows (WO-038):
//
//   * cmd.exe's quote rule. std::system hands the line to `cmd /c`, and when it
//     starts with a quote and holds more than two, cmd strips the FIRST and LAST
//     quote characters and runs what is left: `"C:\a\tool.exe" "arg"` becomes
//     `C:\a\tool.exe" "arg`, and cmd reports "The filename, directory name, or
//     volume label syntax is incorrect." The documented cure is one more pair of
//     quotes around the whole line, which cmd then strips instead.
//   * the return value. POSIX returns a wait status (decode with WEXITSTATUS);
//     Windows returns the exit code.
//
// runShell takes the command as POSIX sh or cmd.exe would read it (quote paths
// with double quotes, redirect with >), and returns the exit code, or -1 if the
// command could not be run at all.
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#  include <sys/wait.h>
#endif

inline int runShell(const std::string& cmd) {
#if defined(_WIN32)
    return std::system(("\"" + cmd + "\"").c_str());
#else
    const int rc = std::system(cmd.c_str());
    return (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
#endif
}

// Where to send output nobody reads.
inline const char* nullDevice() {
#if defined(_WIN32)
    return "NUL";
#else
    return "/dev/null";
#endif
}
