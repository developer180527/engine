#pragma once
// ── thread_stack — run work on a thread whose stack size WE chose ────────────
//
// How deep code may recurse depends on which thread runs it: 8 MB on a Linux or
// macOS main thread, 512 KB on a macOS secondary thread (the job pool, the cook
// worker's threads), 1 MB on Windows. Code whose recursion depth is set by an
// input file (Assimp's readers recurse once per level of a node tree, and so do
// its destructors) must not crash or not depending on which thread it landed
// on. runWithStack gives it a stack of a stated size instead.
//
// The size is a RESERVATION: every OS here commits stack pages only as they are
// touched, so asking for a large stack costs address space, not memory.
//
// Returns false, having run nothing, when no such thread could be made. The
// caller decides what that means; it never silently runs on its own stack.
#include <cstddef>
#include <type_traits>
#include <utility>

namespace engine::threads {

bool runWithStack(size_t bytes, void (*fn)(void*), void* ctx);

template <class F>
bool runWithStack(size_t bytes, F&& f) {
    auto trampoline = [](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); };
    return runWithStack(bytes, +trampoline, static_cast<void*>(&f));
}

}  // namespace engine::threads
