#pragma once

// Standard library.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Windows base. Required by D3D12, WRL, Detours, and NGX.
#include <windows.h>
#include <wrl/client.h>

// D3D12 family.
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

// Third-party SDKs.
#include <detours.h>
#include <nvsdk_ngx.h>

// ImGui and ReShade.
#include <imgui.h>
#include <reshade.hpp>

// Stable addon headers.
#include "profiling/profile.h"

// ctl core.
#include "ctl/schema.h"
#include "ctl/types.h"
#include "ctl/values.h"
