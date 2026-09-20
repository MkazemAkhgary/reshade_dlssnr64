// Command-stream tracing for verifying behavior-preserving pipeline changes.
//
// This is a development aid, not part of the addon's runtime behavior. It
// records every GPU-visible command the addon records (transitions, UAV
// barriers, dispatches, copies, submits) plus every failure reason, into a
// linear text log. Two builds that produce identical logs produce an
// identical command stream, which is exactly the property a refactor of the
// frame pipeline must preserve.
//
// Enable with the environment variable DLSSNR_TRACE=<path>, e.g.
//
//   set DLSSNR_TRACE=%TEMP%\trace_before.log
//
// The file is truncated on first use per process. When the variable is unset
// (the normal case) TraceInit() short-circuits and every macro below compiles
// to nothing observable — a single predictable branch on a global bool.
//
// The resource pointer is printed as a stable index rather than a raw address:
// COM heaps reuse addresses between runs, so raw pointers produce spurious
// diffs. See ResourceId() for the assignment rule.
#pragma once

#include <cstdint>

#include <d3d12.h>

namespace reshade_dlssnr::trace {

// Reads DLSSNR_TRACE once. Safe to call from any thread; idempotent.
void TraceInit();

// True once TraceInit() has found a non-empty path. Cheap enough to call on
// every traced command (a relaxed load of a bool).
bool Enabled();

// Stable slot number for a resource, assigned in first-seen order. Returns
// -1 for nullptr. Never returns a raw pointer, so logs are diffable across
// runs where the allocator hands back different addresses.
int ResourceId(const void* resource);

// Human-readable name for a resource that the addon gave a known role, e.g.
// "composite_output" or "scratch[0]", registered at creation/seed time.
// Returns nullptr when the resource has no registered role.
const char* ResourceName(const void* resource);

// ResourceName() with a fallback, in one lookup. Use this rather than testing
// ResourceName() for null and then calling it again for the value.
const char* ResourceNameOr(const void* resource, const char* fallback);

// Registers a role name for a resource so later trace lines can print it
// instead of a bare index. Re-registration overwrites. No-op when tracing is
// off: names are only ever read from trace arguments.
void NameResource(const void* resource, const char* name);

// Records one already-formatted line. Used by the TRACE_* macros; the user
// pointer is included so interleaved threads stay distinguishable.
void Line(const char* category, const char* function, const void* user_object,
          const char* fmt, ...);

// Flushes buffered lines to disk. Lines are batched rather than flushed
// individually, so call this at a frame boundary to make the frame durable.
void Flush();

// Flushes and closes the log. Called from the addon's Shutdown path and from
// a static destructor, so a crash still leaves a usable tail.
void Shutdown();

// Formats a D3D12_RESOURCE_STATES value as a short stable string. Bit flags
// are combined as "A|B"; the common single-flag states get their bare name.
const char* StateName(D3D12_RESOURCE_STATES state);

}  // namespace reshade_dlssnr::trace

// Record a command when tracing is on. `obj` identifies the owning object
// (CommandContext*, ComputeEngine*, PassSession*) so concurrent streams can
// be told apart.
#define TRACE_CMD(obj, ...)                                          \
  do {                                                               \
    if (::reshade_dlssnr::trace::Enabled()) {                        \
      ::reshade_dlssnr::trace::Line("cmd", __func__, (obj), __VA_ARGS__); \
    }                                                                \
  } while (0)

// Record a state/logic event, e.g. a failure reason or a skip decision.
#define TRACE_EVT(obj, ...)                                          \
  do {                                                               \
    if (::reshade_dlssnr::trace::Enabled()) {                        \
      ::reshade_dlssnr::trace::Line("evt", __func__, (obj), __VA_ARGS__); \
    }                                                                \
  } while (0)
