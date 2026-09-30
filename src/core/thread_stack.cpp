#include "core/thread_stack.h"
#include "core/os_family.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif ENGINE_OS_POSIX
#  include <pthread.h>
#else
#  error "port: core/thread_stack needs a thread whose stack size can be set"
#endif

namespace engine::threads {

namespace {
struct Call { void (*fn)(void*); void* ctx; };
}  // namespace

bool runWithStack(size_t bytes, void (*fn)(void*), void* ctx) {
    Call call{fn, ctx};
#if defined(_WIN32)
    auto entry = [](LPVOID p) -> DWORD { auto* c = static_cast<Call*>(p); c->fn(c->ctx); return 0; };
    HANDLE t = ::CreateThread(nullptr, bytes, entry, &call, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return false;
    ::WaitForSingleObject(t, INFINITE);
    ::CloseHandle(t);
    return true;
#elif ENGINE_OS_POSIX
    auto entry = [](void* p) -> void* { auto* c = static_cast<Call*>(p); c->fn(c->ctx); return nullptr; };
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_t t;
    const bool made = pthread_attr_setstacksize(&attr, bytes) == 0 &&
                      pthread_create(&t, &attr, entry, &call) == 0;
    pthread_attr_destroy(&attr);
    if (made) pthread_join(t, nullptr);
    return made;
#endif
}

}  // namespace engine::threads
