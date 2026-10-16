// RenoDX-compatible NVSDK_NGX_D3D12 hook bridge for guide capture.
#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <limits>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <detours.h>

#include <reshade.hpp>

#include "addon_controller.h"
#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "d3d12_utils.h"
#include "local_vtable.h"
#include "proxy_unwrap.h"
#include "state/settings.h"

namespace reshade_dlssnr::ngx_hook_compat {

// Installing the D3D12 hooks allocates a command allocator and a command
// list, so a retry that never succeeds must not run forever.
constexpr int kMaxHookInstallRetries = 120;

static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;
static decltype(&NVSDK_NGX_D3D12_CreateFeature) real_NVSDK_NGX_D3D12_CreateFeature = nullptr;

static inline std::unordered_map<unsigned int, NVSDK_NGX_Feature> g_handle_to_feature;
static inline std::mutex g_handle_to_feature_mutex;

// Logged once: whether the game's command list had to be resolved. ReShade
// always wraps, so on any host this should say "unwrapped" -- if it ever says
// otherwise, the resolution stopped working and every restore is a no-op
// again, silently.
static void LogCommandListProxyOnce(bool was_proxy) {
  static bool logged = false;
  if (logged) return;
  logged = true;

  reshade::log::message(
      reshade::log::level::info,
      was_proxy
          ? "Command list resolved to the interface our D3D12 hooks recorded"
          : "Command list is already the interface our D3D12 hooks recorded");
}

static void LogOnce(bool& flag, reshade::log::level level, const char* message) {
  if (flag) return;
  flag = true;
  reshade::log::message(level, message);
}

// The vtable hooks are installed at device init, but they are what lets us
// put the game's command list state back after a Post-DLSS pass, so try again
// here if the first attempt did not take.
static bool EnsureHooks(ID3D12GraphicsCommandList* command_list) {
  if (D3D12Hooks::Get().Installed()) return true;
  if (command_list == nullptr) return false;

  static int retries = 0;
  if (retries >= kMaxHookInstallRetries) return false;
  ++retries;

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return false;
  }

  const bool installed = D3D12Hooks::Get().InstallEarlyHooks(device);
  device->Release();
  return installed;
}

static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* command_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** out_handle) {
  NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_CreateFeature(
      command_list, feature_id, parameters, out_handle);

  if (result == NVSDK_NGX_Result_Success && out_handle != nullptr && *out_handle != nullptr) {
    std::lock_guard<std::mutex> lock(g_handle_to_feature_mutex);
    g_handle_to_feature[(*out_handle)->Id] = feature_id;
  }

  return result;
}

static ID3D12Resource* GetResourceParameter(NVSDK_NGX_Parameter* parameters,
                                                const char* name) {
  if (parameters == nullptr || name == nullptr) return nullptr;
  ID3D12Resource* resource = nullptr;
  if (parameters->Get(name, &resource) == NVSDK_NGX_Result_Success &&
      resource != nullptr) {
    return resource;
  }
  void* untyped = nullptr;
  if (parameters->Get(name, &untyped) == NVSDK_NGX_Result_Success &&
      untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }
  return nullptr;
}

enum class PreDlssColorRejection : size_t {
  None,
  MissingColor,
  MissingParameters,
  NotTexture2D,
  ArraySize,
  EmptyExtent,
  MissingActiveExtent,
  InvalidSubrect,
  Count,
};

struct PreDlssColorGeometry {
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int base_x = 0;
  unsigned int base_y = 0;
  bool needs_adapter = false;
};

enum class PreDlssColorAdapterFailure : size_t {
  None,
  DeviceUnavailable,
  UnsupportedResolveFormat,
  ResolveCommandListUnavailable,
  ResourceCreationFailed,
  Count,
};

static unsigned int GetUIntParameter(
    NVSDK_NGX_Parameter* parameters,
    const char* name) {
  unsigned int value = 0;
  if (parameters != nullptr && name != nullptr)
    parameters->Get(name, &value);
  return value;
}

static DXGI_FORMAT TypedPreDlssColorFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      return format;
  }
}

static PreDlssColorRejection GetPreDlssColorGeometry(
    ID3D12Resource* color,
    NVSDK_NGX_Parameter* parameters,
    PreDlssColorGeometry* geometry) {
  if (color == nullptr) return PreDlssColorRejection::MissingColor;
  if (parameters == nullptr) return PreDlssColorRejection::MissingParameters;
  if (geometry == nullptr) return PreDlssColorRejection::InvalidSubrect;

  const D3D12_RESOURCE_DESC desc = color->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return PreDlssColorRejection::NotTexture2D;
  if (desc.DepthOrArraySize != 1)
    return PreDlssColorRejection::ArraySize;
  if (desc.Width == 0 || desc.Height == 0 ||
      desc.Width > std::numeric_limits<unsigned int>::max()) {
    return PreDlssColorRejection::EmptyExtent;
  }

  const unsigned int allocated_width = static_cast<unsigned int>(desc.Width);
  const unsigned int allocated_height = desc.Height;
  const unsigned int input_width = GetUIntParameter(parameters, NVSDK_NGX_Parameter_Width);
  const unsigned int input_height = GetUIntParameter(parameters, NVSDK_NGX_Parameter_Height);
  const unsigned int render_subrect_width = GetUIntParameter(
      parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
  const unsigned int render_subrect_height = GetUIntParameter(
      parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
  const unsigned int base_x = GetUIntParameter(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X);
  const unsigned int base_y = GetUIntParameter(
      parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);

  // Render-subrect dimensions describe the pixels DLSS consumes. Fall back
  // to Width/Height when they are not provided. With a nonzero origin, never
  // guess the active extent from the remaining allocation: that could expose
  // unrendered pixels to the model.
  if (base_x > allocated_width || base_y > allocated_height)
    return PreDlssColorRejection::InvalidSubrect;
  if ((render_subrect_width == 0 && input_width == 0 && base_x != 0) ||
      (render_subrect_height == 0 && input_height == 0 && base_y != 0)) {
    return PreDlssColorRejection::MissingActiveExtent;
  }

  unsigned int active_width = render_subrect_width;
  unsigned int active_height = render_subrect_height;
  if (active_width == 0) active_width = input_width;
  if (active_height == 0) active_height = input_height;
  if (active_width == 0) active_width = allocated_width;
  if (active_height == 0) active_height = allocated_height;

  if (active_width == 0 || active_height == 0 ||
      active_width > allocated_width - base_x ||
      active_height > allocated_height - base_y ||
      base_x > static_cast<unsigned int>(std::numeric_limits<LONG>::max()) ||
      base_y > static_cast<unsigned int>(std::numeric_limits<LONG>::max()) ||
      active_width > static_cast<unsigned int>(std::numeric_limits<LONG>::max()) - base_x ||
      active_height > static_cast<unsigned int>(std::numeric_limits<LONG>::max()) - base_y) {
    return PreDlssColorRejection::InvalidSubrect;
  }

  geometry->width = active_width;
  geometry->height = active_height;
  geometry->base_x = base_x;
  geometry->base_y = base_y;
  geometry->needs_adapter = desc.MipLevels != 1 ||
      desc.SampleDesc.Count != 1 || base_x != 0 || base_y != 0 ||
      active_width != allocated_width || active_height != allocated_height;
  return PreDlssColorRejection::None;
}

static void LogPreDlssColorRejection(PreDlssColorRejection rejection) {
  static bool logged[static_cast<size_t>(PreDlssColorRejection::Count)] = {};
  static const char* const messages[] = {
      "",
      "Pre-DLSS skipped: NGX Color resource is null",
      "Pre-DLSS skipped: NGX parameter block is null",
      "Pre-DLSS skipped: NGX Color must be a Texture2D resource",
      "Pre-DLSS skipped: NGX Color texture array size must be 1",
      "Pre-DLSS skipped: NGX Color has zero width or height",
      "Pre-DLSS skipped: cannot determine active NGX Color dimensions for a nonzero subrect origin",
      "Pre-DLSS skipped: NGX Color active subrect is outside the allocated texture or exceeds D3D12 region limits",
  };

  const size_t index = static_cast<size_t>(rejection);
  if (rejection == PreDlssColorRejection::None ||
      index >= static_cast<size_t>(PreDlssColorRejection::Count) ||
      logged[index]) {
    return;
  }

  logged[index] = true;
  reshade::log::message(reshade::log::level::warning, messages[index]);
}

static void TransitionColorSubresource(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    UINT subresource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  if (command_list == nullptr || resource == nullptr || before == after) return;

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);
}

static PreDlssColorAdapterFailure CreatePreDlssColorAdapter(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    const PreDlssColorGeometry& geometry,
    Microsoft::WRL::ComPtr<ID3D12Resource>* adapter) {
  if (command_list == nullptr || source == nullptr || adapter == nullptr)
    return PreDlssColorAdapterFailure::DeviceUnavailable;

  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  const bool multisampled = source_desc.SampleDesc.Count > 1;
  const DXGI_FORMAT adapter_format = TypedPreDlssColorFormat(source_desc.Format);

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    return PreDlssColorAdapterFailure::DeviceUnavailable;

  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> command_list1;
  if (multisampled) {
    if (FAILED(command_list->QueryInterface(IID_PPV_ARGS(&command_list1))) ||
        command_list1 == nullptr) {
      return PreDlssColorAdapterFailure::ResolveCommandListUnavailable;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = adapter_format;
    if (adapter_format == DXGI_FORMAT_UNKNOWN ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                           &support, sizeof(support))) ||
        (support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) == 0) {
      return PreDlssColorAdapterFailure::UnsupportedResolveFormat;
    }
  }

  D3D12_RESOURCE_DESC adapter_desc = source_desc;
  adapter_desc.Width = geometry.width;
  adapter_desc.Height = geometry.height;
  adapter_desc.DepthOrArraySize = 1;
  adapter_desc.MipLevels = 1;
  adapter_desc.Format = adapter_format;
  adapter_desc.SampleDesc.Count = 1;
  adapter_desc.SampleDesc.Quality = 0;
  adapter_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  adapter_desc.Alignment = 0;
  adapter_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* raw_adapter = nullptr;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &adapter_desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr,
          IID_PPV_ARGS(&raw_adapter))) || raw_adapter == nullptr) {
    return PreDlssColorAdapterFailure::ResourceCreationFailed;
  }
  adapter->Attach(raw_adapter);

  const UINT source_subresource = D3D12CalcSubresource(
      0, 0, 0, source_desc.MipLevels, source_desc.DepthOrArraySize);
  const D3D12_RESOURCE_STATES source_initial_state = InferResourceState(source);
  const D3D12_RESOURCE_STATES source_read_state = multisampled
      ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
      : D3D12_RESOURCE_STATE_COPY_SOURCE;
  const D3D12_RESOURCE_STATES adapter_write_state = multisampled
      ? D3D12_RESOURCE_STATE_RESOLVE_DEST
      : D3D12_RESOURCE_STATE_COPY_DEST;

  TransitionColorSubresource(command_list, source, source_subresource,
                             source_initial_state, source_read_state);
  TransitionColorSubresource(command_list, adapter->Get(), 0,
                             D3D12_RESOURCE_STATE_COMMON, adapter_write_state);

  if (multisampled) {
    D3D12_RECT source_rect = {
        static_cast<LONG>(geometry.base_x),
        static_cast<LONG>(geometry.base_y),
        static_cast<LONG>(geometry.base_x + geometry.width),
        static_cast<LONG>(geometry.base_y + geometry.height)};
    command_list1->ResolveSubresourceRegion(
        adapter->Get(), 0, 0, 0, source, source_subresource,
        &source_rect, adapter_format, D3D12_RESOLVE_MODE_AVERAGE);
  } else {
    D3D12_TEXTURE_COPY_LOCATION destination_location = {};
    destination_location.pResource = adapter->Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination_location.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source_location = {};
    source_location.pResource = source;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = source_subresource;

    const D3D12_BOX source_box = {
        geometry.base_x,
        geometry.base_y,
        0,
        geometry.base_x + geometry.width,
        geometry.base_y + geometry.height,
        1};
    command_list->CopyTextureRegion(&destination_location, 0, 0, 0,
                                    &source_location, &source_box);
  }

  TransitionColorSubresource(command_list, adapter->Get(), 0,
                             adapter_write_state, D3D12_RESOURCE_STATE_COMMON);
  TransitionColorSubresource(command_list, source, source_subresource,
                             source_read_state, source_initial_state);
  return PreDlssColorAdapterFailure::None;
}

static void LogPreDlssColorAdapterFailure(
    PreDlssColorAdapterFailure failure) {
  static bool logged[static_cast<size_t>(PreDlssColorAdapterFailure::Count)] = {};
  static const char* const messages[] = {
      "",
      "Pre-DLSS skipped: could not resolve the NGX Color resource's D3D12 device",
      "Pre-DLSS skipped: NGX Color format does not support MSAA resolve",
      "Pre-DLSS skipped: command list lacks regional MSAA resolve support",
      "Pre-DLSS skipped: failed to allocate the compact NGX Color texture",
  };

  const size_t index = static_cast<size_t>(failure);
  if (failure == PreDlssColorAdapterFailure::None ||
      index >= static_cast<size_t>(PreDlssColorAdapterFailure::Count) ||
      logged[index]) {
    return;
  }
  logged[index] = true;
  reshade::log::message(reshade::log::level::warning, messages[index]);
}

struct PreDlssColorOverride {
  NVSDK_NGX_Parameter* parameters = nullptr;
  ID3D12Resource* original_resource = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Resource> adapter;
  unsigned int original_width = 0;
  unsigned int original_height = 0;
  unsigned int original_base_x = 0;
  unsigned int original_base_y = 0;
  unsigned int original_format = 0;
  bool had_width = false;
  bool had_height = false;
  bool had_base_x = false;
  bool had_base_y = false;
  bool had_format = false;
  bool changed_width = false;
  bool changed_height = false;
  bool changed_base_x = false;
  bool changed_base_y = false;
  bool changed_format = false;
  bool active = false;

  void Apply(NVSDK_NGX_Parameter* params,
             ID3D12Resource* original,
             Microsoft::WRL::ComPtr<ID3D12Resource>&& compact,
             const PreDlssColorGeometry& geometry,
             DXGI_FORMAT original_format_value) {
    parameters = params;
    original_resource = original;
    adapter = std::move(compact);

    had_width = parameters->Get(NVSDK_NGX_Parameter_Width, &original_width) ==
                NVSDK_NGX_Result_Success;
    had_height = parameters->Get(NVSDK_NGX_Parameter_Height, &original_height) ==
                 NVSDK_NGX_Result_Success;
    had_base_x = parameters->Get(
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
        &original_base_x) == NVSDK_NGX_Result_Success;
    had_base_y = parameters->Get(
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        &original_base_y) == NVSDK_NGX_Result_Success;
    had_format = parameters->Get(NVSDK_NGX_Parameter_Color_Format,
                                 &original_format) == NVSDK_NGX_Result_Success;

    if (had_width && original_width != geometry.width) {
      parameters->Set(NVSDK_NGX_Parameter_Width, geometry.width);
      changed_width = true;
    }
    if (had_height && original_height != geometry.height) {
      parameters->Set(NVSDK_NGX_Parameter_Height, geometry.height);
      changed_height = true;
    }
    if (had_base_x && original_base_x != 0) {
      parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
      changed_base_x = true;
    }
    if (had_base_y && original_base_y != 0) {
      parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
      changed_base_y = true;
    }
    const unsigned int compact_format = static_cast<unsigned int>(adapter->GetDesc().Format);
    if (had_format && original_format != compact_format &&
        original_format_value != adapter->GetDesc().Format) {
      parameters->Set(NVSDK_NGX_Parameter_Color_Format, compact_format);
      changed_format = true;
    }

    parameters->Set(NVSDK_NGX_Parameter_Color, adapter.Get());
    active = true;
  }

  void Restore() {
    if (!active || parameters == nullptr) return;
    parameters->Set(NVSDK_NGX_Parameter_Color, original_resource);
    if (changed_width) parameters->Set(NVSDK_NGX_Parameter_Width, original_width);
    if (changed_height) parameters->Set(NVSDK_NGX_Parameter_Height, original_height);
    if (changed_base_x) {
      parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                      original_base_x);
    }
    if (changed_base_y) {
      parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                      original_base_y);
    }
    if (changed_format) {
      parameters->Set(NVSDK_NGX_Parameter_Color_Format, original_format);
    }
    active = false;
  }

  void RetireAdapter() {
    if (adapter != nullptr)
      DlssGuides::Get().RetireGameResource(adapter.Detach());
  }
};

static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  D3D12Hooks::Get().SetTrackingEnabled(false);

  // Exposure and Color are captured before NGX evaluates, since the parameter
  // block may be reused or mutated by the driver's implementation.
  ID3D12Resource* exposure_texture =
      GetResourceParameter(parameters, NVSDK_NGX_Parameter_ExposureTexture);

  bool is_proxy = false;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> native_list =
      proxy_unwrap::NativeChain(command_list, &is_proxy);
  LogCommandListProxyOnce(is_proxy);
  ID3D12GraphicsCommandList* work_list =
      native_list != nullptr ? native_list.Get() : command_list;

  NVSDK_NGX_Feature feature_id = NVSDK_NGX_Feature_Reserved0;
  if (feature_handle != nullptr) {
    std::lock_guard<std::mutex> lock(g_handle_to_feature_mutex);
    auto it = g_handle_to_feature.find(feature_handle->Id);
    if (it != g_handle_to_feature.end()) feature_id = it->second;
  }

  const Settings& settings = GetSettings();
  bool any_pass_enabled = false;
  bool any_pre_dlss = false;
  bool any_post_dlss = false;
  // Tab order, keyed by stable pass id: the same list the render path walks.
  for (const EnabledPass& pass : EnabledPasses(settings)) {
    any_pass_enabled = true;
    any_pre_dlss |= pass.target == static_cast<int>(InjectionStage::PreDLSS);
    any_post_dlss |= pass.target == static_cast<int>(InjectionStage::PostDLSS);
  }

  bool recorded_pass = false;
  bool hooks_available = false;
  bool guides_captured_before_evaluate = false;
  PreDlssColorOverride pre_dlss_color_override;
  if (feature_id == NVSDK_NGX_Feature_SuperSampling && settings.enabled &&
      any_pass_enabled) {
    hooks_available = EnsureHooks(work_list);
    if (hooks_available) {
      // DLSS has already populated these resources before EvaluateFeature is
      // entered. Capture the current frame before Pre-DLSS so temporal guide
      // inputs are correct for all three injection points.
      DlssGuides::Get().CaptureGuides(work_list, parameters, feature_id);
      guides_captured_before_evaluate = true;
    }
  }

  if (feature_id == NVSDK_NGX_Feature_SuperSampling && settings.enabled &&
      any_pre_dlss) {
    if (!hooks_available) {
      static bool logged_pre_hook_failure = false;
      LogOnce(logged_pre_hook_failure, reshade::log::level::warning,
              "Pre-DLSS skipped: D3D12 command-list state hooks are unavailable");
    } else {
      ID3D12Resource* color =
          GetResourceParameter(parameters, NVSDK_NGX_Parameter_Color);
      PreDlssColorGeometry geometry = {};
      const PreDlssColorRejection rejection =
          GetPreDlssColorGeometry(color, parameters, &geometry);
      if (rejection != PreDlssColorRejection::None) {
        LogPreDlssColorRejection(rejection);
      } else {
        ID3D12Resource* pre_input = color;
        if (geometry.needs_adapter) {
          Microsoft::WRL::ComPtr<ID3D12Resource> compact_color;
          const PreDlssColorAdapterFailure adapter_failure =
              CreatePreDlssColorAdapter(work_list, color, geometry,
                                        &compact_color);
          if (adapter_failure != PreDlssColorAdapterFailure::None) {
            LogPreDlssColorAdapterFailure(adapter_failure);
            pre_input = nullptr;
          } else {
            const DXGI_FORMAT original_format = color->GetDesc().Format;
            pre_dlss_color_override.Apply(parameters, color,
                                           std::move(compact_color), geometry,
                                           original_format);
            pre_input = pre_dlss_color_override.adapter.Get();
            static bool logged_compact_color = false;
            LogOnce(logged_compact_color, reshade::log::level::info,
                    "Pre-DLSS: using a compact single-sample Color texture for the active NGX region; the original allocation is preserved");
            if (color->GetDesc().SampleDesc.Count > 1) {
              static bool logged_msaa_resolve = false;
              LogOnce(logged_msaa_resolve, reshade::log::level::info,
                      "Pre-DLSS: multisampled Color is resolved to a single-sample image for NGX; the original MSAA allocation is not modified");
            }
          }
        }

        if (pre_input != nullptr) {
          AddonController::Get().OnPreDlssEvaluate(
              work_list, pre_input, exposure_texture);
          // Restore the application's command-list bindings before NGX records
          // its own work. The final restore below retains the established
          // post-evaluation behavior when another addon stage also ran.
          D3D12Hooks::Get().RestoreCommandList(work_list);
          recorded_pass = true;
        }
      }
    }
  }

  const NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_EvaluateFeature(
      command_list, feature_handle, parameters, callback);

  // Parameters belong to the game's shared NGX block. Restore the original
  // resource and geometry as soon as this evaluation has recorded its work;
  // the compact texture stays alive until the present-queue fence completes.
  pre_dlss_color_override.Restore();
  pre_dlss_color_override.RetireAdapter();

  static bool logged_first = false;
  if (!logged_first) {
    logged_first = true;
    char message[128];
    std::snprintf(message, sizeof(message),
                  "NGX evaluate hook fired (result=%d)",
                  static_cast<int>(result));
    reshade::log::message(reshade::log::level::info, message);
  }

  if (result == NVSDK_NGX_Result_Success && feature_handle != nullptr &&
      parameters != nullptr && feature_id == NVSDK_NGX_Feature_SuperSampling) {
    if (!hooks_available) hooks_available = EnsureHooks(work_list);
    if (!hooks_available) {
      if (any_post_dlss) {
        AddonController::Get().SetPostDlssBlocked(
            "Post-DLSS disabled: D3D12 command list hooks are not installed");
      }
    } else {
      // Rare hook-install fallback: if the pre-evaluation capture could not
      // run, refresh current guides now. A Pre-DLSS stage was already skipped
      // in this case, but Post-DLSS/Backbuffer can still use this snapshot.
      if (settings.enabled && any_pass_enabled &&
          !guides_captured_before_evaluate) {
        DlssGuides::Get().CaptureGuides(work_list, parameters, feature_id);
      }

      if (settings.enabled) {
        // Capture the current upscaled output only when PostDLSS passes need it.
        // HDR measurement runs before the first Pre/Post-DLSS pass, using the
        // published previous-frame Importance Mask. The new mask is generated
        // after the first successful model evaluation. Backbuffer-only setups
        // never enter that path.
        ID3D12Resource* output = nullptr;
        if (any_post_dlss) {
          output = GetResourceParameter(parameters, NVSDK_NGX_Parameter_Output);
          if (output != nullptr) {
            DlssGuides::Get().CaptureOutput(work_list, parameters, feature_id);
          }
        }

        if (any_post_dlss) {
          AddonController::Get().SetPostDlssBlocked(nullptr);
          if (output == nullptr) {
            output = GetResourceParameter(parameters, NVSDK_NGX_Parameter_Output);
          }
          if (output != nullptr) {
            AddonController::Get().OnPostDlssEvaluate(
                work_list, output, exposure_texture);
            recorded_pass = true;
          }
        } else {
          AddonController::Get().SetPostDlssBlocked(nullptr);
        }
      }
    }
  }

  // The restored bindings are only replayed if one of our game-list targets
  // ran. Otherwise the tracker may hold pointers from a previous recording.
  if (recorded_pass) {
    D3D12Hooks::Get().RestoreCommandList(work_list);
  }
  D3D12Hooks::Get().SetTrackingEnabled(true);
  return result;
}

using HookItem = local_vtable::HookItem;

static const std::vector<HookItem> DLSS_HOOKS = {
    {"NVSDK_NGX_D3D12_CreateFeature", (void**)&real_NVSDK_NGX_D3D12_CreateFeature, (void*)&hooked_NVSDK_NGX_D3D12_CreateFeature},
    {"NVSDK_NGX_D3D12_EvaluateFeature", (void**)&real_NVSDK_NGX_D3D12_EvaluateFeature, (void*)&hooked_NVSDK_NGX_D3D12_EvaluateFeature},
};

// Installs the NGX hooks. Safe to call until it succeeds.
static bool TryHookNgx() {
  if (real_NVSDK_NGX_D3D12_EvaluateFeature != nullptr) return true;

  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) {
    // nvngx_dlss.dll is loaded on demand, so load it to get the hook in place
    // ahead of the first CreateFeature -- a feature created before this
    // attaches would never be recognised and nothing would run. Checking
    // GetModuleHandle first means an instance Streamline has already loaded is
    // never shadowed by a second copy.
    nvngx_dlss = LoadLibraryW(L"nvngx_dlss.dll");
  }
  if (nvngx_dlss == nullptr) return false;

  if (!local_vtable::Hook(nvngx_dlss, DLSS_HOOKS)) return false;

  static bool logged = false;
  LogOnce(logged, reshade::log::level::info,
          "Hooked NVSDK_NGX_D3D12_CreateFeature/EvaluateFeature in nvngx_dlss.dll");
  return true;
}

static void UnhookNgx() {
  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) return;

  local_vtable::Unhook(nvngx_dlss, DLSS_HOOKS);
}

}  // namespace reshade_dlssnr::ngx_hook_compat
