#pragma once

// Compile-time Tracy instrumentation policy.
// Set DLSSNR_PROFILE_VERBOSITY to 0..3 and each DLSSNR_PROFILE_DOMAIN_* to 0/1.
// Level 1 is coarse lifecycle/frame work, level 2 adds pass/resource/async
// operations, and level 3 adds nested helper, setting-row, and validation work.

#ifndef DLSSNR_PROFILE_VERBOSITY
#define DLSSNR_PROFILE_VERBOSITY 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_CORE
#define DLSSNR_PROFILE_DOMAIN_CORE 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_RENDER
#define DLSSNR_PROFILE_DOMAIN_RENDER 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_RESOURCE
#define DLSSNR_PROFILE_DOMAIN_RESOURCE 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_ASYNC
#define DLSSNR_PROFILE_DOMAIN_ASYNC 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_UI
#define DLSSNR_PROFILE_DOMAIN_UI 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_CTL
#define DLSSNR_PROFILE_DOMAIN_CTL 1
#endif
#ifndef DLSSNR_PROFILE_DOMAIN_GPU
#define DLSSNR_PROFILE_DOMAIN_GPU 0
#endif

// Tracy is enabled when verbosity > 0 and at least one CPU domain is on.
// TRACY_ENABLE must also be defined in the build; the vcxproj adds it.
#if DLSSNR_PROFILE_VERBOSITY > 0 && \
    (DLSSNR_PROFILE_DOMAIN_CORE || DLSSNR_PROFILE_DOMAIN_RENDER || \
     DLSSNR_PROFILE_DOMAIN_RESOURCE || DLSSNR_PROFILE_DOMAIN_ASYNC || \
     DLSSNR_PROFILE_DOMAIN_UI || DLSSNR_PROFILE_DOMAIN_CTL || \
     DLSSNR_PROFILE_DOMAIN_GPU)
#define DLSSNR_PROFILE_TRACY_ENABLED 1
// TracyD3D12.hpp assigns a ULONGLONG timestamp counter to an int on
// one path. The truncation is benign (it feeds a small hash), but it
// is third-party code and should not be edited. Push a warning state,
// disable C4244 for the duration of the include, and pop it so the
// same warning in addon code is still reported.
#pragma warning(push)
#pragma warning(disable: 4244)
#include "tracy/Tracy.hpp"
#include "tracy/TracyD3D12.hpp"
#pragma warning(pop)
#else
#define DLSSNR_PROFILE_TRACY_ENABLED 0
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED
#include <cstdio>
#include <string>

namespace reshade_dlssnr::profiling {

// Tracy's ZoneText macro takes (const char*, size_t). These tag macros carry
// a label plus a value; format them into "label: value" and pass the string's
// data and length.
inline std::string FormatProfileTag(const char* name, const char* value) {
    std::string result(name ? name : "");
    result += ": ";
    result += value ? value : "";
    return result;
}

template <typename T>
inline std::string FormatProfileTag(const char* name, T value) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %lld", name ? name : "",
                  static_cast<long long>(value));
    return buf;
}

}  // namespace reshade_dlssnr::profiling

#define DLSSNR_PROFILE_TAG_IMPL(name, value)                                   \
    do {                                                                       \
        auto _dlssnr_tag_ =                                                    \
            ::reshade_dlssnr::profiling::FormatProfileTag((name), (value));    \
        ZoneText(_dlssnr_tag_.c_str(), _dlssnr_tag_.size());                   \
    } while (0)
#endif

// -- CPU scopes -------------------------------------------------------------
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1 && DLSSNR_PROFILE_DOMAIN_CORE
#define DLSSNR_PROFILE_CORE_L1(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_CORE_TAG_L1(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_CORE_L1(name) ((void)0)
#define DLSSNR_PROFILE_CORE_TAG_L1(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_CORE
#define DLSSNR_PROFILE_CORE_L2(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_CORE_TAG_L2(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_CORE_L2(name) ((void)0)
#define DLSSNR_PROFILE_CORE_TAG_L2(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 3 && DLSSNR_PROFILE_DOMAIN_CORE
#define DLSSNR_PROFILE_CORE_L3(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_CORE_TAG_L3(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_CORE_L3(name) ((void)0)
#define DLSSNR_PROFILE_CORE_TAG_L3(name, value) ((void)0)
#endif

// -- Render domain ---------------------------------------------------------
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1 && DLSSNR_PROFILE_DOMAIN_RENDER
#define DLSSNR_PROFILE_RENDER_L1(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_RENDER_TAG_L1(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_RENDER_L1(name) ((void)0)
#define DLSSNR_PROFILE_RENDER_TAG_L1(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_RENDER
#define DLSSNR_PROFILE_RENDER_L2(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_RENDER_TAG_L2(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_RENDER_L2(name) ((void)0)
#define DLSSNR_PROFILE_RENDER_TAG_L2(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 3 && DLSSNR_PROFILE_DOMAIN_RENDER
#define DLSSNR_PROFILE_RENDER_L3(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_RENDER_TAG_L3(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_RENDER_L3(name) ((void)0)
#define DLSSNR_PROFILE_RENDER_TAG_L3(name, value) ((void)0)
#endif

// -- Resource domain -------------------------------------------------------
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1 && DLSSNR_PROFILE_DOMAIN_RESOURCE
#define DLSSNR_PROFILE_RESOURCE_L1(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_RESOURCE_TAG_L1(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_RESOURCE_L1(name) ((void)0)
#define DLSSNR_PROFILE_RESOURCE_TAG_L1(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_RESOURCE
#define DLSSNR_PROFILE_RESOURCE_L2(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_RESOURCE_TAG_L2(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_RESOURCE_L2(name) ((void)0)
#define DLSSNR_PROFILE_RESOURCE_TAG_L2(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 3 && DLSSNR_PROFILE_DOMAIN_RESOURCE
#define DLSSNR_PROFILE_RESOURCE_L3(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_RESOURCE_TAG_L3(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_RESOURCE_L3(name) ((void)0)
#define DLSSNR_PROFILE_RESOURCE_TAG_L3(name, value) ((void)0)
#endif

// -- Async domain ----------------------------------------------------------
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1 && DLSSNR_PROFILE_DOMAIN_ASYNC
#define DLSSNR_PROFILE_ASYNC_L1(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_ASYNC_TAG_L1(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_ASYNC_L1(name) ((void)0)
#define DLSSNR_PROFILE_ASYNC_TAG_L1(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_ASYNC
#define DLSSNR_PROFILE_ASYNC_L2(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_ASYNC_TAG_L2(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_ASYNC_L2(name) ((void)0)
#define DLSSNR_PROFILE_ASYNC_TAG_L2(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 3 && DLSSNR_PROFILE_DOMAIN_ASYNC
#define DLSSNR_PROFILE_ASYNC_L3(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_ASYNC_TAG_L3(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_ASYNC_L3(name) ((void)0)
#define DLSSNR_PROFILE_ASYNC_TAG_L3(name, value) ((void)0)
#endif

// -- UI domain -------------------------------------------------------------
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1 && DLSSNR_PROFILE_DOMAIN_UI
#define DLSSNR_PROFILE_UI_L1(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_UI_TAG_L1(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_UI_L1(name) ((void)0)
#define DLSSNR_PROFILE_UI_TAG_L1(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_UI
#define DLSSNR_PROFILE_UI_L2(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_UI_TAG_L2(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_UI_L2(name) ((void)0)
#define DLSSNR_PROFILE_UI_TAG_L2(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 3 && DLSSNR_PROFILE_DOMAIN_UI
#define DLSSNR_PROFILE_UI_L3(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_UI_TAG_L3(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_UI_L3(name) ((void)0)
#define DLSSNR_PROFILE_UI_TAG_L3(name, value) ((void)0)
#endif

// -- CTL domain ------------------------------------------------------------
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1 && DLSSNR_PROFILE_DOMAIN_CTL
#define DLSSNR_PROFILE_CTL_L1(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_CTL_TAG_L1(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_CTL_L1(name) ((void)0)
#define DLSSNR_PROFILE_CTL_TAG_L1(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_CTL
#define DLSSNR_PROFILE_CTL_L2(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_CTL_TAG_L2(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_CTL_L2(name) ((void)0)
#define DLSSNR_PROFILE_CTL_TAG_L2(name, value) ((void)0)
#endif

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 3 && DLSSNR_PROFILE_DOMAIN_CTL
#define DLSSNR_PROFILE_CTL_L3(name) ZoneScopedN(name)
#define DLSSNR_PROFILE_CTL_TAG_L3(name, value) DLSSNR_PROFILE_TAG_IMPL(name, value)
#else
#define DLSSNR_PROFILE_CTL_L3(name) ((void)0)
#define DLSSNR_PROFILE_CTL_TAG_L3(name, value) ((void)0)
#endif

// -- Frame marker ----------------------------------------------------------
// The argument is ignored: Tracy's FrameMark has no name. Keeping the
// argument preserves the existing call sites.
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 1
#define DLSSNR_PROFILE_FRAME_L1(name) FrameMark
#else
#define DLSSNR_PROFILE_FRAME_L1(name) ((void)0)
#endif

// -- GPU zones -------------------------------------------------------------
// DLSSNR_PROFILE_GPU_CONTEXT_L2 stashes the command list so
// DLSSNR_PROFILE_GPU_L2 can record a Tracy D3D12 zone on it. Both are no-ops
// below verbosity 2 or when the GPU domain is off.
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
#define DLSSNR_PROFILE_GPU_CONTEXT_L2(command_list) \
    do { ::reshade_dlssnr::profiling::SetGpuCommandList(command_list); } while (0)
#define DLSSNR_PROFILE_GPU_L2(name) \
    do { \
        if (auto* ctx = ::reshade_dlssnr::profiling::GetD3D12Context()) { \
            TracyD3D12Zone(ctx, ::reshade_dlssnr::profiling::GetGpuCommandList(), name); \
        } \
    } while (0)
#else
#define DLSSNR_PROFILE_GPU_CONTEXT_L2(command_list) ((void)0)
#define DLSSNR_PROFILE_GPU_L2(name) ((void)0)
#endif
