// Command-stream tracing, for inspecting what the addon actually did.
//
// This is a development aid, not part of the addon's runtime behavior. It
// records every GPU-visible command the addon records (transitions, UAV
// barriers, dispatches, copies, submits) plus every failure reason, into a
// linear text log, and every settings change (`cfg`) alongside them. Reach for
// it when something crashes or behaves unexpectedly and the ReShade log does
// not say why: it shows the commands that were actually recorded, in the order
// they were recorded, rather than what the code intends.
//
// Enable with the environment variable DLSSNR_TRACE=<path>, e.g.
//
//   set DLSSNR_TRACE=%TEMP%\trace_before.log
//
// The file is truncated on first use per process. When the variable is unset
// (the normal case) TraceInit() short-circuits and every macro below compiles
// to nothing observable — a single predictable branch on a global.
//
// The resource pointer is printed as a stable index rather than a raw address:
// COM heaps reuse addresses between runs, so raw pointers produce spurious
// diffs. See ResourceId() for the assignment rule.
//
// Every line also carries the two frame counters, so a frame reads straight
// out of the log without matching markers. A run that drops a frame shifts
// every later counter, so comparing two captures diverges at that point —
// which is the useful direction, because the divergence names the frame that
// differs.
#pragma once

#include <cstdint>

#include <d3d12.h>

// Compile-time gate. Off strips every TRACE_* macro and reduces the
// trace entry points to empty functions, so no format string or line
// writer survives in the binary. DLSSNR_TRACE still selects whether a
// build that has tracing compiled in writes anything at runtime.
#ifndef DLSSNR_TRACE_ENABLED
#define DLSSNR_TRACE_ENABLED 0
#endif

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
// individually; EndFrame calls this, so a frame boundary is durable without
// the caller doing anything.
void Flush();

// ---------------------------------------------------------------------------
// Frame boundaries
// ---------------------------------------------------------------------------
//
// A game frame is not one stream. The DLSS evaluate happens mid-frame and the
// present happens at the end, so a single counter incremented at either point
// labels the other stream's lines with the wrong frame. Each entry point gets
// its own counter, printed on every line as `b=` (PreDlss), `d=` (PostDlss),
// and `p=` (Present).
//
// BeginFrame/EndFrame also emit a `frm` marker line, so frames are greppable
// and each END names the line range the frame spans:
//
//   grep ' frm '                 every boundary
//   grep 'frm.*BEGIN'            every frame start
//
// None of these counters is a game-frame number when the game calls DLSS more
// than once per frame; the two evaluate counters count hook calls, not frames.
enum class FrameStream : uint32_t {
  PreDlss = 0,
  PostDlss = 1,
  Present = 2,
};

// Emits the BEGIN marker for `stream` and increments its counter. Returns the
// new counter value, which every subsequent line carries until EndFrame.
uint64_t BeginFrame(FrameStream stream);

// Emits the END marker for `stream`, naming the line range the frame spans,
// and flushes so the frame is durable.
void EndFrame(FrameStream stream);

// Flushes and closes the log. Called from the addon's Shutdown path and from
// a static destructor, so a crash still leaves a usable tail.
void Shutdown();

// Formats a D3D12_RESOURCE_STATES value as a short stable string. Bit flags
// are combined as "A|B"; the common single-flag states get their bare name.
//
// Returns a shared per-thread buffer, so only one result can be live at a
// time: two calls in one argument list both print the second state. Use
// StatePairName() for a transition.
const char* StateName(D3D12_RESOURCE_STATES state);

// Formats a transition as "before -> after". StateName() cannot be called
// twice for this, which is why the pair has its own entry point.
const char* StatePairName(D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after);

}  // namespace reshade_dlssnr::trace

#if !DLSSNR_TRACE_ENABLED

#define TRACE_CMD(obj, ...) ((void)0)
#define TRACE_EVT(obj, ...) ((void)0)
#define TRACE_CFG(...) ((void)0)

#else

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

// Record a settings change. Written by state/settings.cpp, which serializes
// the settings and emits the fields that differ from the previous snapshot:
//
//   init <name> <value>             once, at LoadSettings
//   set <name> <old> -> <new>       on every SaveSettings
//
// `grep ' cfg '` isolates them, so a change in behavior can be read against
// the frame counters (`b=`/`d=`/`p=`) of the frame it happened on.
#define TRACE_CFG(...)                                               \
  do {                                                               \
    if (::reshade_dlssnr::trace::Enabled()) {                        \
      ::reshade_dlssnr::trace::Line("cfg", "Settings", nullptr,      \
                                    __VA_ARGS__);                    \
    }                                                                \
  } while (0)

#endif  // !DLSSNR_TRACE_ENABLED
