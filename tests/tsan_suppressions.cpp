// ── tsan_suppressions — what Thread Sanitizer cannot see synchronise ─────────
// Linked into every test executable, and only in the thread-sanitizer build
// (tests/CMakeLists.txt). TSan reads this function at start. Each entry is one
// proven false positive, scoped as narrowly as its stack allows; a real race
// must never be silenced here.
//
// addCompletedHandler and addScheduledHandler (bgfx's Metal renderer): a
// handler's std::function is copied into a block on the main thread when the
// command buffer is committed, and Metal later calls it on its own thread when
// the GPU schedules or finishes the work. Commit happens before both, which
// Metal guarantees, but that ordering lives inside Metal and libdispatch,
// which are not built with TSan, so it reports the copy and the call (and the
// copied object's vptr) as races. First seen when a sanitizer test first
// rendered through a real Metal device (eng_window_test, WO-070).
//
// NOT suppressed: what a handler then writes. bgfx's GPU timer wrote its
// timestamps unsynchronised, a real race, fixed by
// third_party/patches/bgfx.cmake+bgfx__mtl-timer-query-race.patch.
extern "C" const char* __tsan_default_suppressions() {
    return "race:MTL::CommandBuffer::addCompletedHandler\n"
           "race:MTL::CommandBuffer::addScheduledHandler\n";
}
