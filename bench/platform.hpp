#pragma once

// Small OS-specific helpers for the benchmarks: CPU identification, thread placement and a
// barrier against dead-code elimination. Nothing here is used by the engine itself.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace bench {

inline std::string cpu_name() {
#if defined(__APPLE__)
  char buf[256] = {};
  std::size_t len = sizeof(buf);
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
#elif defined(__linux__)
  std::ifstream in("/proc/cpuinfo");
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("model name", 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) return line.substr(colon + 2);
    }
  }
#endif
  return "unknown CPU";
}

inline std::string compiler_name() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#else
  return "unknown compiler";
#endif
}

inline std::string os_name() {
#if defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#else
  return "unknown OS";
#endif
}

/// Asks the scheduler to keep the calling thread on a fast core and, where the OS allows it, pins
/// it to `cpu` (ignored when negative). macOS has no hard affinity API; the closest lever is the
/// user-interactive QoS class, which steers the thread to performance cores.
/// Returns a short description of what was actually applied.
inline std::string place_current_thread(int cpu) {
#if defined(__APPLE__)
  (void)cpu;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  return "QoS user-interactive (macOS has no thread pinning)";
#elif defined(__linux__)
  if (cpu < 0) return "unpinned";
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0)
    return "pinned to CPU " + std::to_string(cpu);
  return "pinning to CPU " + std::to_string(cpu) + " failed";
#else
  (void)cpu;
  return "unpinned";
#endif
}

/// Makes `value` observable to the compiler so the computation producing it cannot be removed.
template <class T>
inline void keep(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

}  // namespace bench
