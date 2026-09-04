#include "core/thread_qos.h"

#if defined(__APPLE__)
    // <pthread/qos.h> declares the qos calls but NOT pthread_self, which
    // pthread_get_qos_class_np needs — engine_cook_worker.cpp gets away with
    // the narrow include only because it never reads a class back.
    #include <pthread.h>
    #include <pthread/qos.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #  define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #  define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__linux__)
    #include <sys/resource.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#endif

namespace engine::qos {
namespace {

// What we last set, per thread. On Apple this is redundant with the OS; on
// every other platform it IS the getter, because neither Windows nor Linux
// exposes a symmetric read of what we wrote (GetThreadPriority reports the
// offset we passed, not the effective class; getpriority is per-process on
// some libc configurations and per-thread on others).
thread_local Class t_last = Class::Unclassified;

} // namespace

bool setForCurrentThread(Class c) {
    // Read-only sentinel; see the enum. Rejecting it here rather than mapping
    // it to "default" keeps the setter's vocabulary to things a caller can
    // justify from the work.
    if (c == Class::Unclassified) return false;

    t_last = c;

#if defined(__APPLE__)
    qos_class_t q = QOS_CLASS_USER_INITIATED;
    switch (c) {
        case Class::Interactive: q = QOS_CLASS_USER_INTERACTIVE; break;
        case Class::Initiated:   q = QOS_CLASS_USER_INITIATED;   break;
        case Class::Utility:     q = QOS_CLASS_UTILITY;          break;
        case Class::Unclassified: return false;   // unreachable; keeps -Wswitch quiet
    }
    return pthread_set_qos_class_self_np(q, 0) == 0;

#elif defined(_WIN32)
    // Windows has no QoS classes; thread priority is the nearest equivalent.
    // AvSetMmThreadCharacteristics("Games") is the fuller answer for the frame
    // thread and is deliberately NOT called here -- it needs a matching
    // AvRevertMmThreadCharacteristics and therefore an owner with a lifetime,
    // which a fire-and-forget setter does not have. Tracked in
    // docs/plans/resource-policy.md section 3.1.
    int prio = THREAD_PRIORITY_NORMAL;
    switch (c) {
        case Class::Interactive: prio = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case Class::Initiated:   prio = THREAD_PRIORITY_NORMAL;       break;
        case Class::Utility:     prio = THREAD_PRIORITY_BELOW_NORMAL; break;
        case Class::Unclassified: return false;   // unreachable
    }
    return ::SetThreadPriority(::GetCurrentThread(), prio) != 0;

#elif defined(__linux__)
    // Raising nice needs privileges we will not have, so Interactive is a
    // no-op rather than a failure -- claiming it failed would make callers
    // log a warning on every boot for something working as intended.
    int nice = 0;
    switch (c) {
        case Class::Interactive: return true;
        case Class::Initiated:   nice = 0;  break;
        case Class::Utility:     nice = 10; break;
        case Class::Unclassified: return false;   // unreachable
    }
    // PRIO_PROCESS + a TID is per-THREAD on Linux, where "process" means the
    // task. Passing 0 would nice the whole process, which for the job pool
    // would demote the main thread too -- the exact inversion this file is
    // trying to prevent.
    const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
    return ::setpriority(PRIO_PROCESS, tid, nice) == 0;

#else
    return false;
#endif
}

Class currentThreadClass() {
#if defined(__APPLE__)
    qos_class_t q = QOS_CLASS_UNSPECIFIED;
    if (pthread_get_qos_class_np(pthread_self(), &q, nullptr) == 0) {
        switch (q) {
            case QOS_CLASS_USER_INTERACTIVE: return Class::Interactive;
            case QOS_CLASS_USER_INITIATED:   return Class::Initiated;
            case QOS_CLASS_UTILITY:
            case QOS_CLASS_BACKGROUND:       return Class::Utility;
            // QOS_CLASS_DEFAULT (21) is what pthread_create hands out and is
            // the whole defect: distinct from, and below, the main thread's
            // USER_INTERACTIVE (33). Reporting it as Unclassified rather than
            // falling through to the echo is what lets a test tell "we set
            // this" apart from "nobody did".
            case QOS_CLASS_DEFAULT:
            case QOS_CLASS_UNSPECIFIED:      return Class::Unclassified;
            default: break;
        }
    }
#endif
    return t_last;
}

bool classIsObservable() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

} // namespace engine::qos
