#!/usr/bin/env python3
"""Find std:: uses whose standard header nobody included.

WHY THIS EXISTS
---------------
libc++ (macOS) pulls a lot of the standard library in transitively; libstdc++
(Linux) does not. So a file that uses `std::memcpy` with no `<cstring>`, or
`std::string` with no `<string>`, compiles perfectly on the development machine
and fails on every Linux leg of CI.

That has now cost three separate CI round-trips, one error at a time, because a
build stops at the first failure: `<cstring>` in four files, then `<string>` in
`gpu_budget.cpp`, and there was no reason to believe that was the last one. One
round-trip per missing include is not a workflow.

This finds all of them at once, locally, without a Linux toolchain.

HOW IT WORKS, AND WHAT IT CANNOT DO
-----------------------------------
For each file it computes the transitive closure of OUR OWN includes (quoted
includes that resolve inside the repo), collects the system headers reachable
that way, and flags any `std::` symbol whose providing header is not among them.

DELIBERATELY OMITTED: `<utility>` (`std::move`, `std::pair`, `std::forward`,
`std::swap`), `<exception>`, `<initializer_list>` and `std::ptrdiff_t`. The first
pass flagged 136 uses, about 70 of them `std::move` — and libstdc++ pulls
`<utility>` in through every container header, so those are noise. A lint whose
output is mostly noise gets ignored wholesale, including the eight lines in it
that were real. `<initializer_list>` is a compiler builtin.

It is deliberately CONSERVATIVE: only well-known symbol -> header mappings, and
a symbol passes if ANY of its candidate headers is present (`std::min` is in
`<algorithm>`, `std::swap` in both `<utility>` and `<algorithm>`). It does not
parse C++ — it does not know about macros, `#ifdef`, or templates — so it is a
LINT, not a compiler. It will miss things a real libstdc++ build would catch.
Its job is to make the common case cheap, not to replace the Linux legs.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where our code lives. third_party is not ours to fix.
ROOTS = ["src", "tests", "include", "modules"]
SKIP_DIRS = {"third_party", "build", ".git", ".cache", "target", "node_modules"}
EXTS = {".cpp", ".h", ".hpp", ".cc", ".mm", ".inl"}

# symbol -> headers that provide it (any one suffices)
PROVIDERS = {
    # <cstring>
    **{s: ["cstring", "string.h"] for s in
       ["memcpy", "memset", "memcmp", "memmove", "strlen", "strcmp", "strncmp",
        "strstr", "strcpy", "strncpy", "strchr", "strrchr", "strtok"]},
    # <cstdio>
    **{s: ["cstdio", "stdio.h"] for s in
       ["printf", "fprintf", "snprintf", "sprintf", "vsnprintf", "fopen",
        "fclose", "fread", "fwrite", "fseek", "ftell", "fputs", "fgets",
        "setvbuf", "remove", "rename", "perror", "FILE", "fflush"]},
    # <cstdlib>
    **{s: ["cstdlib", "stdlib.h"] for s in
       ["malloc", "calloc", "realloc", "free", "getenv", "exit", "abort",
        "atoi", "atof", "strtol", "strtoul", "strtod", "qsort", "system",
        "labs", "div"]},
    # <cmath>
    **{s: ["cmath", "math.h"] for s in
       ["sqrt", "sqrtf", "pow", "powf", "floor", "ceil", "fabs", "fmod",
        "lround", "llround", "round", "sin", "cos", "tan", "atan2", "asin",
        "acos", "log", "log2", "exp", "isfinite", "isnan", "isinf", "hypot",
        "copysign", "trunc"]},
    # containers and friends
    "string": ["string"],
    "to_string": ["string"],
    "stoi": ["string"], "stol": ["string"], "stof": ["string"], "stod": ["string"],
    "wstring": ["string"],
    "string_view": ["string_view"],
    "vector": ["vector"],
    "array": ["array"],
    "deque": ["deque"],
    "list": ["list"],
    "map": ["map"], "multimap": ["map"],
    "set": ["set"], "multiset": ["set"],
    "unordered_map": ["unordered_map"],
    "unordered_set": ["unordered_set"],
    "tuple": ["tuple"], "make_tuple": ["tuple"], "get": None,   # too ambiguous
    "optional": ["optional"], "nullopt": ["optional"],
    "variant": ["variant"], "visit": ["variant"],
    "span": ["span"],
    "function": ["functional"],
    "unique_ptr": ["memory"], "shared_ptr": ["memory"], "weak_ptr": ["memory"],
    "make_unique": ["memory"], "make_shared": ["memory"],
    "atomic": ["atomic"], "atomic_thread_fence": ["atomic"],
    "memory_order": ["atomic"], "memory_order_relaxed": ["atomic"],
    "mutex": ["mutex"], "lock_guard": ["mutex"], "unique_lock": ["mutex"],
    "scoped_lock": ["mutex"], "call_once": ["mutex"], "once_flag": ["mutex"],
    "recursive_mutex": ["mutex"],
    "condition_variable": ["condition_variable"],
    "thread": ["thread"],
    "numeric_limits": ["limits"],
    "runtime_error": ["stdexcept"], "logic_error": ["stdexcept"],
    "invalid_argument": ["stdexcept"], "out_of_range": ["stdexcept"],
    "ifstream": ["fstream"], "ofstream": ["fstream"], "fstream": ["fstream"],
    "ostringstream": ["sstream"], "istringstream": ["sstream"],
    "stringstream": ["sstream"],
    "cout": ["iostream"], "cerr": ["iostream"], "endl": ["iostream", "ostream"],
    "int8_t": ["cstdint", "stdint.h"], "int16_t": ["cstdint", "stdint.h"],
    "int32_t": ["cstdint", "stdint.h"], "int64_t": ["cstdint", "stdint.h"],
    "uint8_t": ["cstdint", "stdint.h"], "uint16_t": ["cstdint", "stdint.h"],
    "uint32_t": ["cstdint", "stdint.h"], "uint64_t": ["cstdint", "stdint.h"],
    "uintptr_t": ["cstdint", "stdint.h"], "intptr_t": ["cstdint", "stdint.h"],
    # provided by nearly every C header; treat generously
    "size_t": ["cstddef", "stddef.h", "cstdio", "cstring", "cstdlib", "cstdint",
               "string", "vector"],
    # <algorithm> / <utility>
    **{s: ["algorithm"] for s in
       ["sort", "stable_sort", "find", "find_if", "copy", "fill", "transform",
        "lower_bound", "upper_bound", "count", "count_if", "reverse", "unique",
        "remove_if", "any_of", "all_of", "none_of", "accumulate", "clamp",
        "max_element", "min_element", "rotate", "shuffle"]},
    "min": ["algorithm"], "max": ["algorithm"],
    "chrono": ["chrono"],
    "filesystem": ["filesystem"],
    "this_thread": ["thread"],
}
PROVIDERS = {k: v for k, v in PROVIDERS.items() if v is not None}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*(["<])([^">]+)[">]', re.M)
STD_RE = re.compile(r'\bstd::([A-Za-z_][A-Za-z0-9_]*)')

# Comments and string literals are PROSE, not code, and this codebase comments
# heavily about the standard library. Without stripping them the first run
# "found" that logger.h needs <vector> — from a header comment explaining that
# the ring USED to be a std::vector and is not one any more. A lint that asks you
# to add an include for a sentence is worse than no lint.
_STRIP = re.compile(
    r'/\*.*?\*/'          # block comments
    r'|//[^\n]*'           # line comments
    r'|"(?:\\.|[^"\\\n])*"'   # string literals
    r"|'(?:\\.|[^'\\\n])*'",  # char literals
    re.S)


def strip_noncode(text):
    """Blank out comments/literals, preserving newlines so line numbers hold."""
    return _STRIP.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)


def source_files():
    for root in ROOTS:
        base = os.path.join(REPO, root)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                if os.path.splitext(fn)[1] in EXTS:
                    yield os.path.join(dirpath, fn)


def parse(path):
    """(system_headers, our_includes_as_raw_paths, text)"""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return set(), [], ""
    sysh, ours = set(), []
    for kind, name in INCLUDE_RE.findall(text):
        if kind == "<":
            sysh.add(name)
        else:
            ours.append(name)
    return sysh, ours, strip_noncode(text)


# Index our headers by basename and by suffix path so quoted includes resolve.
def build_index():
    by_suffix = {}
    for p in source_files():
        rel = os.path.relpath(p, REPO).replace(os.sep, "/")
        parts = rel.split("/")
        for i in range(len(parts)):
            by_suffix.setdefault("/".join(parts[i:]), p)
    return by_suffix


def main():
    index = build_index()
    parsed = {}
    for p in source_files():
        parsed[p] = parse(p)

    def closure(path, seen=None):
        """System headers reachable through our own include graph."""
        if seen is None:
            seen = set()
        if path in seen:
            return set()
        seen.add(path)
        sysh, ours, _ = parsed.get(path, (set(), [], ""))
        out = set(sysh)
        for inc in ours:
            target = index.get(inc)
            if target is None:
                target = index.get(inc.split("/")[-1])
            if target and target != path:
                out |= closure(target, seen)
        return out

    findings = []
    for p in sorted(parsed):
        sysh, ours, text = parsed[p]
        if not text:
            continue
        avail = closure(p)
        # A file with no includes at all is almost certainly an .inl included
        # elsewhere; judging it on its own would be pure noise.
        if not avail and not ours:
            continue
        seen_here = {}
        for m in STD_RE.finditer(text):
            sym = m.group(1)
            cands = PROVIDERS.get(sym)
            if not cands or any(c in avail for c in cands):
                continue
            if sym in seen_here:
                continue
            seen_here[sym] = text.count("\n", 0, m.start()) + 1
        for sym, line in sorted(seen_here.items(), key=lambda kv: kv[1]):
            findings.append((os.path.relpath(p, REPO), line, sym,
                             PROVIDERS[sym][0]))

    if not findings:
        print("check_std_includes: no missing standard headers found")
        return 0

    print(f"check_std_includes: {len(findings)} use(s) of std:: with no header "
          f"that provides them\n")
    for rel, line, sym, want in findings:
        print(f"  {rel}:{line}: std::{sym} needs <{want}>")
    print("\nlibc++ provides these transitively and libstdc++ does not, so each "
          "one is a Linux-only build failure.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
