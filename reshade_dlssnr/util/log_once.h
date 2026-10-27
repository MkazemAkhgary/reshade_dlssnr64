#pragma once

// Log-once helpers.
//
// Consolidates the "log the first time only" pattern that otherwise appears
// as a `static bool logged; if (logged) return; logged = true;` in every
// caller. The state is an atomic so a call site reached from two threads
// still fires the message exactly once.

#include <atomic>
#include <cstddef>

#include <reshade.hpp>

namespace reshade_dlssnr {

struct LogOnceState {
  std::atomic_bool fired{false};
};

inline bool ShouldLogOnce(LogOnceState& state) {
  return !state.fired.exchange(true, std::memory_order_relaxed);
}

inline bool ShouldLogOnce(std::atomic_bool& state) {
  return !state.exchange(true, std::memory_order_relaxed);
}

template <std::size_t N>
bool ShouldLogEnumOnce(std::atomic_bool (&seen)[N], std::size_t index) {
  if (index == 0 || index >= N) return false;
  return !seen[index].exchange(true, std::memory_order_relaxed);
}

}  // namespace reshade_dlssnr

// Log the message the first time this call site is reached.
#define DLSSNR_LOG_ONCE(level, ...)                                          \
  do {                                                                       \
    static ::reshade_dlssnr::LogOnceState dlssnr_log_once_state_ = {};       \
    if (::reshade_dlssnr::ShouldLogOnce(dlssnr_log_once_state_)) {           \
      reshade::log::message(level, __VA_ARGS__);                             \
    }                                                                        \
  } while (0)

// Log the message the first time `flag` is set, atomically.
#define DLSSNR_LOG_ONCE_FLAG(flag, level, ...)                               \
  do {                                                                       \
    if (::reshade_dlssnr::ShouldLogOnce(flag)) {                             \
      reshade::log::message(level, __VA_ARGS__);                             \
    }                                                                        \
  } while (0)
