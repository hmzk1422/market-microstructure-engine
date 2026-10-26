#pragma once

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

inline bool pin_current_thread_to_cpu(std::uint32_t cpu) noexcept {
#ifdef _WIN32
    if (cpu >= sizeof(DWORD_PTR) * 8) return false;
    return SetThreadAffinityMask(GetCurrentThread(),
                                 static_cast<DWORD_PTR>(1) << cpu) != 0;
#elif defined(__linux__)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) == 0;
#else
    (void)cpu;
    return false;
#endif
}
