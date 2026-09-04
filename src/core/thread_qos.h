#pragma once

// ── thread_qos — tell the OS scheduler what a thread is FOR ─────────────────
//
// A thread's scheduling class is not a performance tweak; it is the only way
// the OS can tell the difference between work a player is waiting on and work
// that exists to happen eventually. Left unset, every thread we create claims
// the SAME middling priority, and the scheduler has nothing to go on when the
// machine is oversubscribed.
//
// MEASURED, 2026-08-29, M-series (12 cores), 32 threads / 2 s of contention:
//
//     8  USER_INTERACTIVE threads   78,285 units each
//     24 BACKGROUND       threads    5,407 units each
//     -> 14.5x per-thread advantage
//
// Two things that measurement corrected, recorded because both were assumed:
//
//   * main() ALREADY gets USER_INTERACTIVE from the OS. The hypothesis that it
//     might be scheduled onto E-cores was measured and refuted; the main thread
//     was never the problem.
//   * pthread_create() produces a thread at QOS_CLASS_DEFAULT and does NOT
//     inherit the creating thread's class. That is the actual defect: every
//     pool worker ran one class BELOW the main thread, by omission rather than
//     by any decision.
//
// AND IT DOES NOT MATTER ON A DEVELOPER MACHINE. 8 workers on 12 idle cores
// never compete, and a direct measurement of that case put every class inside
// the noise. It matters on a player's four-core laptop with a browser open,
// which is the shipping condition rather than the desk condition. Do not
// expect a local before/after to show anything.
//
// See docs/plans/resource-policy.md section 3.1.
namespace engine::qos {

// Deliberately three, not the OS's full ladder. Each one is a STATEMENT ABOUT
// THE WORK, and a class nobody can justify from the work is a class that gets
// applied by copy-paste.
enum class Class {
    // The frame. A player is waiting on this right now. The main thread has it
    // already, from the OS, without us asking.
    Interactive,
    // On the frame's critical path but not the frame itself: the job pool.
    // Deliberately one step BELOW Interactive -- eleven threads all claiming
    // the top class is how a process starves its own main thread, which is the
    // failure this whole file exists to avoid rather than to cause.
    Initiated,
    // Exists to yield. Cooking, background IO, anything whose lateness costs
    // nothing a player can perceive.
    Utility,

    // READ-ONLY. What a thread nobody classified reports: the OS's own default,
    // which on Apple is QOS_CLASS_DEFAULT (21) — a real value, one step BELOW
    // the main thread's USER_INTERACTIVE (33). Passing it to
    // setForCurrentThread() is rejected, because "put this thread back to
    // whatever it was" is not a thing any caller here wants to say.
    //
    // It exists because without it the getter could not tell an unclassified
    // thread from a classified one, and the first version of thread_qos_test
    // silently passed on that ambiguity: it asserted a fresh thread was "not
    // Interactive" and got the answer from a fallback constant rather than from
    // the OS. A distinction the tests cannot see is a distinction that is not
    // being tested.
    Unclassified,
};

// Applies `c` to the CALLING thread. Must be called ON the thread being
// classified -- every platform primitive underneath is self-scoped, and there
// is deliberately no setFor(handle) overload, because the OS APIs disagree
// about whether that is even possible.
//
// Returns false when the platform has no mechanism, the call failed, or `c` is
// Class::Unclassified. A false return is not an error worth failing a boot
// over: the process still runs, just without the hint.
bool setForCurrentThread(Class c);

// The calling thread's class, or Class::Unclassified when nobody set one.
// Exists so a TEST can assert the class actually landed -- a setter with no
// getter is a call site nobody can prove ran, which in this repo is the same
// as a comment.
//
// Only Apple reports this faithfully. Elsewhere it returns what we last SET
// on this thread, which is weaker (it cannot catch the OS overriding us) and
// is why thread_qos_test asserts the strong property only on Apple.
Class currentThreadClass();

// Whether currentThreadClass() reads the OS or merely echoes our own last
// call. Tests branch on this rather than on the platform, so a future platform
// gaining a real getter strengthens the test without editing it.
bool classIsObservable();

} // namespace engine::qos
