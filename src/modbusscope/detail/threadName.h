#pragma once

#if defined(__linux__)
#include <pthread.h>
#endif

namespace modbusscope::detail {

// Names the calling thread for `top -H` / debugger output. Linux truncates the
// name to 15 characters; other platforms are a no-op until a platform-specific
// implementation is added.
inline void set_current_thread_name(const char *name) noexcept {
#if defined(__linux__)
    if (name) {
        (void)pthread_setname_np(pthread_self(), name);
    }
#else
    (void)name;
#endif
}

} // namespace modbusscope::detail
