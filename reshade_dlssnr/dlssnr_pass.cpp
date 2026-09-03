#include "dlssnr_pass.h"

#include <filesystem>
#include <mutex>

#include <reshade.hpp>

namespace reshade_dlssnr {

namespace {
const char* NgxResultName(unsigned int r) {
  switch (r) {
    case 0x1: return "Success";
    case 0xBAD00001: return "FeatureNotSupported";
    case 0xBAD00002: return "PlatformError";
    case 0xBAD00003: return "FeatureAlreadyExists";
    case 0xBAD00004: return "FeatureNotFound";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00006: return "ScratchBufferTooSmall";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD00009: return "RWFlagMissing";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000C: return "OutOfDate";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    case 0xBAD0000F: return "UnableToWriteToAppDataPath";
    case 0xBAD00010: return "UnsupportedParameter";
    case 0xBAD00011: return "Denied";
    case 0xBAD00012: return "NotImplemented";
    default: return "unknown";
  }
}

std::filesystem::path GetModuleDirectory() {
  HMODULE module = nullptr;
  static auto* image_base = reinterpret_cast<unsigned char*>(&NgxResultName);
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(image_base), &module);

  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).remove_filename();
}

std::optional<std::filesystem::path> FindFile(const wchar_t* name) {
  auto module_dir = GetModuleDirectory();
  auto candidate = module_dir / name;

  if (std::filesystem::exists(candidate)) {
    return candidate;
  }

  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  auto exe_dir = std::filesystem::path(exe_path).remove_filename();
  candidate = exe_dir / name;

  if (std::filesystem::exists(candidate)) {
    return candidate;
  }

  return std::nullopt;
}

std::mutex g_mutex;
}  // namespace

DlssNrPass& DlssNrPass::Get() {
  static DlssNrPass pass;
  return pass;
}

bool DlssNrPass::IsReady() const {
  return feature_ != nullptr && !failed_;
}

const char* DlssNrPass::GetLastError() const {
  return failed_ ? reason_ : "";
}

bool DlssNrPass::EnsureForwarder() {
  if (forwarder_.IsLoaded()) return true;

  auto found = FindFile(GetDefaultForwarderName());
  if (!found.has_value()) {
    reason_ = "nvngx.dll_dlssnr.dll not found";
    return false;
  }

  if (!forwarder_.Load(found->wstring().c_str())) {
    reason_ = "nvngx.dll_dlssnr.dll would not load";
    return false;
  }

  return true;
}

bool DlssNrPass::EnsureCapabilityParams(ID3D12Device* device) {
  (void) device;
  if (capability_params_ != nullptr) return true;

  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) {
    nvngx_dlss = LoadLibraryW(L"nvngx_dlss.dll");
  }

  if (nvngx_dlss == nullptr) {
    reason_ = "nvngx_dlss.dll is not loaded";
    return false;
  }

  using PFN_GetCapabilityParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
  auto get_capability_parameters = reinterpret_cast<PFN_GetCapabilityParameters>(
      GetProcAddress(nvngx_dlss, "NVSDK_NGX_D3D12_GetCapabilityParameters"));

  if (get_capability_parameters == nullptr) {
    reason_ = "NVSDK_NGX_D3D12_GetCapabilityParameters export not found";
    return false;
  }

  NVSDK_NGX_Result result = get_capability_parameters(&capability_params_);
  if (result != NVSDK_NGX_Result_Success || capability_params_ == nullptr) {
    capability_params_ = nullptr;
    reason_ = "NGX capability parameters unavailable";
    return false;
  }

  DiscoverFloatSlot(capability_params_);
  return true;
}

void DlssNrPass::DiscoverFloatSlot(NVSDK_NGX_Parameter* params) {
  if (float_slot_known_ || params == nullptr || forwarder_.probe_float == nullptr || forwarder_.set_float_slot == nullptr) {
    return;
  }

  float_slot_known_ = true;

  constexpr const char* probe_key = "DLSSNR.ReshadeFloatProbe";
  constexpr float expected = 0.375f;
  static const int candidates[] = {1, 2, 5, 6, 7, 4, 3, 0};

  for (int slot : candidates) {
    float value = 0.0f;
    forwarder_.probe_float(params, probe_key, expected, slot);
    if (params->Get(probe_key, &value) == NVSDK_NGX_Result_Success && value == expected) {
      float_slot_ = slot;
      forwarder_.set_float_slot(slot);
      reshade::log::message(reshade::log::level::info, "DLSSNR float slot discovered");
      return;
    }
  }

  reshade::log::message(reshade::log::level::error, "DLSSNR float slot not discovered");
}

void DlssNrPass::SetExtras() {
  if (forwarder_.set_extras == nullptr || capability_params_ == nullptr) return;

  forwarder_.set_extras(capability_params_, 1.0f, nullptr, nullptr, nullptr, 0, 0, 0, 0);
}

bool DlssNrPass::EnsureFeature(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  if (feature_ != nullptr) return true;

  auto model = FindFile(GetDefaultModelName());
  if (!model.has_value()) {
    reason_ = "nvngx_dlssnr.dll not found";
    return false;
  }

  SetExtras();

  const Settings& settings = GetSettings();
  const auto work_scale = std::clamp(settings.working_scale, 0.25f, 1.0f);
  work_width_ = static_cast<unsigned int>(width * work_scale + 0.5f);
  work_height_ = static_cast<unsigned int>(height * work_scale + 0.5f);

  const wchar_t* data_path = L"";

  feature_ = forwarder_.create(
      model->wstring().c_str(),
      data_path,
      device,
      command_list,
      capability_params_,
      work_width_,
      work_height_,
      static_cast<int>(settings.preset),
      settings.intensity,
      static_cast<int>(settings.style),
      settings.local_structure,
      settings.local_tone,
      settings.skin_structure,
      settings.auto_mask ? 1 : 0,
      1);

  if (feature_ == nullptr) {
    failed_ = true;
    reason_ = "DLSSNR model would not initialise";
    const auto init_result = forwarder_.last_init != nullptr ? *forwarder_.last_init : 0;
    const auto create_result = forwarder_.last_create != nullptr ? *forwarder_.last_create : 0;

    std::string message = "DLSSNR create failed: init 0x";
    char buffer[32] = {};
    sprintf_s(buffer, "%X", init_result);
    message += buffer;
    message += " create 0x";
    sprintf_s(buffer, "%X", create_result);
    message += buffer;
    reshade::log::message(reshade::log::level::error, message.c_str());
    return false;
  }

  reset_ = true;
  built_tuning_dirty_ = false;
  return true;
}

DXGI_FORMAT DlssNrPass::TypedGuideFormat(DXGI_FORMAT format) const {
  switch (format) {
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
  }
}

bool DlssNrPass::FormatCanHoldLinearHdr(DXGI_FORMAT format) const {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return true;
    default:
      return false;
  }
}

ID3D12Resource* DlssNrPass::CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ID3D12Resource* resource = nullptr;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  nullptr, IID_PPV_ARGS(&resource));
  return resource;
}

ID3D12Resource* DlssNrPass::CreateGuideClone(ID3D12Device* device, ID3D12Resource* source) {
  D3D12_RESOURCE_DESC desc = source->GetDesc();
  desc.Format = TypedGuideFormat(desc.Format);
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  ID3D12Resource* resource = nullptr;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                  nullptr, IID_PPV_ARGS(&resource));
  return resource;
}

ID3D12Resource* DlssNrPass::ReadableGuide(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    ID3D12Resource** clone) {
  if (source == nullptr || TypedGuideFormat(source->GetDesc().Format) == source->GetDesc().Format) {
    return source;
  }

  if (*clone == nullptr) {
    *clone = CreateGuideClone(device, source);
  }

  if (*clone == nullptr) {
    return nullptr;
  }

  Barrier(command_list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_list->CopyResource(*clone, source);
  Barrier(command_list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(command_list, *clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return *clone;
}

void DlssNrPass::Barrier(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  if (before == after || resource == nullptr) return;

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);
}

ID3D12Resource* DlssNrPass::GetResource(const NVSDK_NGX_Parameter* params, const char* name_a, const char* name_b) {
  ID3D12Resource* resource = nullptr;

  if (params->Get(name_a, &resource) == NVSDK_NGX_Result_Success && resource != nullptr) {
    return resource;
  }

  resource = nullptr;
  if (params->Get(name_b, &resource) == NVSDK_NGX_Result_Success && resource != nullptr) {
    return resource;
  }

  void* untyped = nullptr;
  if (params->Get(name_a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }

  untyped = nullptr;
  if (params->Get(name_b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }

  return nullptr;
}

void DlssNrPass::RetireResources(ID3D12Device*) {
  // Delayed release is intentionally omitted in this first pass; real
  // shutdown is handled in Shutdown().
}

void DlssNrPass::OnEvaluateAfterUpscale(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Feature feature_id,
    ID3D12CommandQueue* queue) {
  (void) queue;
  std::lock_guard<std::mutex> lock(g_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;

  if (settings.only_super_sampling && feature_id != NVSDK_NGX_Feature_SuperSampling) {
    return;
  }

  if (failed_ || command_list == nullptr || params == nullptr) return;

  ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
  ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
  ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

  if (target == nullptr || depth == nullptr || motion == nullptr) {
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return;
  }

  if (!EnsureForwarder() || !EnsureCapabilityParams(device)) {
    failed_ = true;
    reshade::log::message(reshade::log::level::error, reason_);
    device->Release();
    return;
  }

  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  width_ = static_cast<unsigned int>(desc.Width);
  height_ = static_cast<unsigned int>(desc.Height);

  const auto work_scale = std::clamp(settings.working_scale, 0.25f, 1.0f);
  work_width_ = static_cast<unsigned int>(width_ * work_scale + 0.5f);
  work_height_ = static_cast<unsigned int>(height_ * work_scale + 0.5f);
  const bool reduced = work_width_ != width_ || work_height_ != height_;

  unsigned int guide_width = 0;
  unsigned int guide_height = 0;
  params->Get(NVSDK_NGX_Parameter_Width, &guide_width);
  params->Get(NVSDK_NGX_Parameter_Height, &guide_height);

  if (guide_width == 0 || guide_height == 0) {
    guide_width = width_;
    guide_height = height_;
  }

  unsigned int create_flags = 0;
  params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &create_flags);
  const bool depth_inverted = (create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  const bool game_says_hdr = (create_flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
  const bool is_hdr_buffer = game_says_hdr && FormatCanHoldLinearHdr(desc.Format);

  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;
  if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mv_scale_x) != NVSDK_NGX_Result_Success) {
    mv_scale_x = 1.0f;
  }
  if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mv_scale_y) != NVSDK_NGX_Result_Success) {
    mv_scale_y = 1.0f;
  }

  if (codec_ == nullptr) {
    codec_ = std::make_unique<DlssNrCodec>();
  }

  if (!codec_->IsInit()) {
    if (!codec_->Init(device)) {
      failed_ = true;
      reason_ = "DLSSNR codec would not initialise";
      reshade::log::message(reshade::log::level::error, reason_);
      device->Release();
      return;
    }
  }

  if (color_copy_ == nullptr) {
    color_copy_ = CreateScratch(device, desc.Format, width_, height_);
    hdr_copy_ = CreateScratch(device, desc.Format, width_, height_);
    output_ = CreateScratch(device, desc.Format, work_width_, work_height_);
  }

  if (reduced && color_small_ == nullptr) {
    color_small_ = CreateScratch(device, desc.Format, work_width_, work_height_);
  }

  if (!EnsureFeature(device, command_list, width_, height_)) {
    device->Release();
    return;
  }

  DlssNrConstants encode = {};
  encode.Mode = DlssNrMode_Encode;
  encode.Passthrough = is_hdr_buffer ? 0u : 1u;
  encode.WhitePoint = settings.white_point_scale;
  encode.Width = width_;
  encode.Height = height_;
  encode.MvScaleX = mv_scale_x;
  encode.MvScaleY = mv_scale_y;
  encode.GuideWidth = guide_width;
  encode.GuideHeight = guide_height;

  Barrier(command_list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  codec_->Dispatch(command_list, encode, target, nullptr, nullptr, nullptr, nullptr, color_copy_, hdr_copy_);
  Barrier(command_list, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  Barrier(command_list, color_copy_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(command_list, hdr_copy_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* model_input = color_copy_;

  if (reduced && color_small_ != nullptr) {
    DlssNrConstants down = {};
    down.Mode = DlssNrMode_Downsample;
    down.Width = work_width_;
    down.Height = work_height_;
    codec_->Dispatch(command_list, down, model_input, nullptr, nullptr, nullptr, nullptr, color_small_, nullptr);
    Barrier(command_list, color_small_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    model_input = color_small_;
  }

  ID3D12Resource* depth_in = ReadableGuide(device, command_list, depth, &depth_clone_);
  ID3D12Resource* motion_in = ReadableGuide(device, command_list, motion, &motion_clone_);

  if (depth_in == nullptr || motion_in == nullptr) {
    failed_ = true;
    reason_ = "depth or motion vectors could not be made readable";
    reshade::log::message(reshade::log::level::error, reason_);
    device->Release();
    return;
  }

  const float mv_to_work = width_ != 0 ? static_cast<float>(work_width_) / static_cast<float>(width_) : 1.0f;

  SetExtras();

  const int result = forwarder_.evaluate(
      command_list,
      feature_,
      capability_params_,
      model_input,
      depth_in,
      motion_in,
      output_,
      work_width_,
      work_height_,
      guide_width,
      guide_height,
      depth_inverted ? 1 : 0,
      reset_ ? 1 : 0,
      settings.intensity,
      static_cast<int>(settings.style),
      settings.local_structure,
      settings.local_tone,
      settings.skin_structure,
      settings.auto_mask ? 1 : 0,
      mv_scale_x * mv_to_work,
      mv_scale_y * mv_to_work);

  reset_ = false;

  if (result == NVSDK_NGX_Result_Success) {
    DlssNrConstants resolve = {};
    resolve.Mode = DlssNrMode_Resolve;
    resolve.WhitePoint = settings.white_point_scale;
    resolve.Width = width_;
    resolve.Height = height_;
    resolve.TransferStrength = settings.transfer_strength;
    resolve.ColourStrength = settings.colour_strength;
    resolve.DebugView = settings.debug_view;
    resolve.MaxRatio = settings.max_ratio;
    resolve.Passthrough = is_hdr_buffer ? 0u : 1u;
    resolve.CompareMode = settings.compare_mode;
    resolve.CompareSplit = settings.compare_split;
    resolve.CompareZoom = std::max(1.0f, settings.compare_zoom);
    resolve.CompareSwap = settings.compare_swap ? 1u : 0u;
    resolve.MvScaleX = mv_scale_x;
    resolve.MvScaleY = mv_scale_y;
    resolve.GuideWidth = guide_width;
    resolve.GuideHeight = guide_height;

    Barrier(command_list, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    codec_->Dispatch(command_list, resolve, model_input, output_, hdr_copy_, motion_in, nullptr, target, nullptr);
    Barrier(command_list, output_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    failed_ = true;
    reason_ = "DLSSNR evaluate failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }

  Barrier(command_list, hdr_copy_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Barrier(command_list, color_copy_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (depth_clone_ != nullptr) {
    Barrier(command_list, depth_clone_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }

  if (motion_clone_ != nullptr) {
    Barrier(command_list, motion_clone_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }

  if (reduced && color_small_ != nullptr) {
    Barrier(command_list, color_small_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  device->Release();
}

void DlssNrPass::Shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);

  if (feature_ != nullptr && forwarder_.release != nullptr) {
    forwarder_.release(feature_);
    feature_ = nullptr;
  }

  for (ID3D12Resource** resource : {&color_copy_, &hdr_copy_, &output_, &color_small_, &depth_clone_, &motion_clone_}) {
    if (*resource != nullptr) {
      (*resource)->Release();
      *resource = nullptr;
    }
  }

  if (codec_ != nullptr) {
    codec_->Destroy();
    codec_.reset();
  }

  capability_params_ = nullptr;
  failed_ = false;
  reason_ = "";
}

}  // namespace reshade_dlssnr
