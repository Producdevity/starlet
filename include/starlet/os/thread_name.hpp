#pragma once

#if defined(__linux__)
#include <pthread.h>
#endif

namespace starlet::os {

// Names the calling thread for logs, top -H and profiles (Linux keeps 15 characters).
inline void name_thread(const char* name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

}
