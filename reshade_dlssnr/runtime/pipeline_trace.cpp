#include "pipeline_trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace reshade_dlssnr::trace {

namespace {

std::mutex& Mutex() {
  // Function-local statics are initialized on first use and, unlike namespace
  // scope objects, are not torn down in an order that depends on which
  // translation unit happens to be destroyed first. A namespace-scope
  // std::mutex here would be a live hazard: the TraceShutdown destructor at
  // the bottom of this file calls Shutdown(), which locks it, and the two are
  // destroyed in reverse order of construction across TUs — so the mutex could
  // already be gone. Both this and `g_file` are deliberately leaked instead.
  static std::mutex* instance = new std::mutex();
  return *instance;
}

FILE* g_file = nullptr;
bool g_checked = false;

uint64_t g_sequence = 0;

// Reads an environment variable without tripping the CRT's deprecation warning
// on getenv. Returns an empty string when unset.
std::string ReadEnv(const char* name) {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return std::string();
  }
  std::string result(value, length > 0 ? length - 1 : 0);
  std::free(value);
  return result;
}

// Resource identity is assigned in first-seen order so that logs from two
// runs with the same command sequence line up even though the allocator
// hands out different addresses.
std::unordered_map<const void*, int> g_ids;
int g_next_id = 0;

std::unordered_map<const void*, std::string> g_names;

const char* const kStateNames[] = {
    "COMMON",                   // 0x0
    "VERTEX_AND_CONSTANT_BUFFER",  // 0x1
    "INDEX_BUFFER",            // 0x2
    "RENDER_TARGET",           // 0x4
    "UNORDERED_ACCESS",        // 0x8
    "DEPTH_WRITE",             // 0x10
    "DEPTH_READ",              // 0x20
    "NON_PIXEL_SHADER_RESOURCE",  // 0x40
    "PIXEL_SHADER_RESOURCE",   // 0x80
    "STREAM_OUT",              // 0x100
    "INDIRECT_ARGUMENT",       // 0x200
    "COPY_DEST",               // 0x400
    "COPY_SOURCE",             // 0x800
    "RESOLVE_DEST",            // 0x1000
    "RESOLVE_SOURCE",          // 0x2000
    "RAYTRACING_ACCELERATION_STRUCTURE",  // 0x4000
    "SHADING_RATE_SOURCE",     // 0x8000
    "PREDICATION",             // 0x1000000
    "PRESENT",                 // 0x0 with the present sentinel
};

// The bit values above, in the same order, for the combination loop.
const D3D12_RESOURCE_STATES kStateValues[] = {
    D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
    D3D12_RESOURCE_STATE_INDEX_BUFFER,
    D3D12_RESOURCE_STATE_RENDER_TARGET,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_DEPTH_WRITE,
    D3D12_RESOURCE_STATE_DEPTH_READ,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_STREAM_OUT,
    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
    D3D12_RESOURCE_STATE_COPY_DEST,
    D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_RESOLVE_DEST,
    D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
    D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,
    D3D12_RESOURCE_STATE_PREDICATION,
    D3D12_RESOURCE_STATE_PRESENT,
};

constexpr size_t kStateCount = sizeof(kStateValues) / sizeof(kStateValues[0]);

}  // namespace

const char* StateName(D3D12_RESOURCE_STATES state) {
  if (state == D3D12_RESOURCE_STATE_COMMON) return "COMMON";
  if (state == D3D12_RESOURCE_STATE_PRESENT) return "PRESENT";
  if (state == D3D12_RESOURCE_STATE_GENERIC_READ) return "GENERIC_READ";

  // Multi-flag states (e.g. NON_PIXEL_SHADER_RESOURCE|PIXEL_SHADER_RESOURCE)
  // are rendered as a '|'-joined list. A thread-local buffer keeps this
  // allocation-free and reentrant.
  static thread_local char buffer[256];
  buffer[0] = '\0';
  size_t used = 0;

  for (size_t i = 0; i < kStateCount; ++i) {
    const D3D12_RESOURCE_STATES bit = kStateValues[i];
    if (bit == D3D12_RESOURCE_STATE_COMMON || bit == D3D12_RESOURCE_STATE_PRESENT) {
      continue;  // handled above; they are not real bit flags
    }
    if ((state & bit) == 0) continue;
    const int written = std::snprintf(buffer + used, sizeof(buffer) - used,
                                     "%s%s", used == 0 ? "" : "|",
                                     kStateNames[i]);
    if (written <= 0) break;
    used += static_cast<size_t>(written);
    if (used >= sizeof(buffer)) break;
  }

  return buffer[0] != '\0' ? buffer : "UNKNOWN";
}

void TraceInit() {
  std::lock_guard<std::mutex> lock(Mutex());
  if (g_checked) return;
  g_checked = true;

  const std::string path = ReadEnv("DLSSNR_TRACE");
  if (path.empty()) return;

  errno_t open_error = fopen_s(&g_file, path.c_str(), "wb");
  if (open_error != 0 || g_file == nullptr) {
    g_file = nullptr;
    return;
  }

  std::fprintf(g_file, "# reshade_dlssnr pipeline trace\n");
  std::fflush(g_file);
}

bool Enabled() {
  if (!g_checked) TraceInit();
  return g_file != nullptr;
}

int ResourceId(const void* resource) {
  if (resource == nullptr) return -1;
  auto it = g_ids.find(resource);
  if (it != g_ids.end()) return it->second;
  const int id = g_next_id++;
  g_ids.emplace(resource, id);
  return id;
}

const char* ResourceName(const void* resource) {
  if (resource == nullptr) return nullptr;
  auto it = g_names.find(resource);
  return it != g_names.end() ? it->second.c_str() : nullptr;
}

void NameResource(const void* resource, const char* name) {
  if (resource == nullptr || name == nullptr) return;
  std::lock_guard<std::mutex> lock(Mutex());
  g_names[resource] = name;
}

void Line(const char* category, const char* function, const void* user_object,
          const char* fmt, ...) {
  if (g_file == nullptr) return;

  std::lock_guard<std::mutex> lock(Mutex());
  if (g_file == nullptr) return;

  std::fprintf(g_file, "%06llu %-3s %-28s obj=%d ",
               static_cast<unsigned long long>(g_sequence++),
               category != nullptr ? category : "?",
               function != nullptr ? function : "?",
               ResourceId(user_object));

  va_list args;
  va_start(args, fmt);
  std::vfprintf(g_file, fmt, args);
  va_end(args);

  std::fputc('\n', g_file);

  // Flushed per line so a crash still leaves a complete tail. This is a
  // debug path; the cost is irrelevant next to the GPU work being traced.
  std::fflush(g_file);
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(Mutex());
  if (g_file != nullptr) {
    std::fclose(g_file);
    g_file = nullptr;
  }
}

namespace {
struct TraceShutdown {
  ~TraceShutdown() { Shutdown(); }
};
TraceShutdown g_trace_shutdown;
}  // namespace

}  // namespace reshade_dlssnr::trace
