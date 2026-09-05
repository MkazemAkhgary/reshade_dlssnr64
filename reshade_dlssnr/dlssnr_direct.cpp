#include "dlssnr_direct.h"

#include <filesystem>
#include <mutex>

#include <reshade.hpp>

#include "codec.h"
#include "dlss_guides.h"
#include "settings.h"

namespace reshade_dlssnr {

namespace {

std::mutex g_direct_mutex;

std::filesystem::path GetModuleDirectory() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(&g_direct_mutex),
      &module);

  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).remove_filename();
}

wchar_t g_spoof_path[MAX_PATH] = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nvmdi.inf_amd64\\nvngx.dll";

DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
  const DWORD len = static_cast<DWORD>(wcslen(g_spoof_path));
  const DWORD copy_len = len >= size ? size - 1 : len;
  wcsncpy_s(filename, size, g_spoof_path, copy_len);
  return copy_len;
}

bool PatchImport(HMODULE module, const char* dll_name, const char* function_name, void* replacement) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  auto& import_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_dir.VirtualAddress == 0) return false;

  auto* import_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + import_dir.VirtualAddress);
  for (; import_desc->Name != 0; ++import_desc) {
    const char* import_dll_name = reinterpret_cast<const char*>(base + import_desc->Name);
    if (_stricmp(import_dll_name, dll_name) != 0) continue;

    auto thunk_rva = import_desc->OriginalFirstThunk != 0
                         ? import_desc->OriginalFirstThunk
                         : import_desc->FirstThunk;
    auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + thunk_rva);
    auto* iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import_desc->FirstThunk);

    for (; original_thunk->u1.AddressOfData != 0; ++original_thunk, ++iat_thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) continue;

      auto* import_by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original_thunk->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(import_by_name->Name), function_name) != 0) continue;

      void** slot = reinterpret_cast<void**>(&iat_thunk->u1.Function);
      DWORD old_protect = 0;
      if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) return false;
      *slot = replacement;
      DWORD ignored = 0;
      VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
      return true;
    }
  }

  return false;
}

}  // namespace

NVSDK_NGX_Result SimpleParams::Get(const char* name, unsigned long long* value) const {
  auto it = ull_.find(name);
  if (it == ull_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = it->second;
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, float* value) const {
  auto it = f_.find(name);
  if (it == f_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = it->second;
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, double* value) const {
  auto it = d_.find(name);
  if (it == d_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = it->second;
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, unsigned int* value) const {
  auto it = ui_.find(name);
  if (it == ui_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = it->second;
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, int* value) const {
  auto it = i_.find(name);
  if (it == i_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = it->second;
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, ID3D11Resource** value) const {
  auto it = ptr_.find(name);
  if (it == ptr_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = static_cast<ID3D11Resource*>(it->second);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, ID3D12Resource** value) const {
  auto it = ptr_.find(name);
  if (it == ptr_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = static_cast<ID3D12Resource*>(it->second);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result SimpleParams::Get(const char* name, void** value) const {
  auto it = ptr_.find(name);
  if (it == ptr_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *value = it->second;
  return NVSDK_NGX_Result_Success;
}

void SimpleParams::Reset() {
  ull_.clear();
  f_.clear();
  d_.clear();
  ui_.clear();
  i_.clear();
  ptr_.clear();
}

DlssNrDirect& DlssNrDirect::Get() {
  static DlssNrDirect direct;
  return direct;
}

void DlssNrDirect::SetDebugSelectedResource(uint64_t handle) {
  debug_selected_resource_handle_ = handle;
}

void DlssNrDirect::SetPendingSelectedPass(ID3D12Resource* resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
  }

  pending_selected_pass_ = resource;
}

bool DlssNrDirect::IsReady() const {
  return feature_handle_ != nullptr && !failed_;
}

const char* DlssNrDirect::GetLastError() const {
  return failed_ ? reason_ : "";
}

bool DlssNrDirect::EnsureModelAndPatch() {
  if (model_module_ != nullptr) return true;

  model_module_ = GetModuleHandleW(L"nvngx_dlssnr.dll");
  if (model_module_ == nullptr) {
    model_module_ = LoadLibraryW(L"nvngx_dlssnr.dll");
  }

  if (model_module_ == nullptr) {
    reason_ = "nvngx_dlssnr.dll not found";
    return false;
  }

  PatchImport(model_module_, "KERNEL32.dll", "GetModuleFileNameW", HookedGetModuleFileNameW);

  init_ext_ = reinterpret_cast<PFN_InitExt>(GetProcAddress(model_module_, "NVSDK_NGX_D3D12_Init_Ext"));
  create_feature_ = reinterpret_cast<PFN_CreateFeature>(GetProcAddress(model_module_, "NVSDK_NGX_D3D12_CreateFeature"));
  evaluate_feature_ = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(model_module_, "NVSDK_NGX_D3D12_EvaluateFeature"));
  release_feature_ = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(model_module_, "NVSDK_NGX_D3D12_ReleaseFeature"));

  if (create_feature_ == nullptr || evaluate_feature_ == nullptr) {
    reason_ = "nvngx_dlssnr.dll is missing required exports";
    return false;
  }

  reshade::log::message(reshade::log::level::info, "Loaded and patched nvngx_dlssnr.dll directly");
  return true;
}

bool DlssNrDirect::EnsureInitialized(ID3D12Device* device) {
  if (initialized_) return true;
  if (init_ext_ == nullptr) return false;

  const std::wstring data_path = GetModuleDirectory().wstring();
  const NVSDK_NGX_Result result = init_ext_(
      0ull,
      data_path.c_str(),
      device,
      NVSDK_NGX_Version_API,
      nullptr);

  if (result != NVSDK_NGX_Result_Success) {
    reason_ = "nvngx_dlssnr.dll Init_Ext failed";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  initialized_ = true;
  reshade::log::message(reshade::log::level::info, "nvngx_dlssnr.dll initialized directly");
  return true;
}

bool DlssNrDirect::EnsureFeature(
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  if (feature_handle_ != nullptr && built_width_ == width && built_height_ == height) {
    return true;
  }

  if (feature_handle_ != nullptr && release_feature_ != nullptr) {
    release_feature_(feature_handle_);
    feature_handle_ = nullptr;
    first_evaluate_ = true;
  }

  const Settings& settings = GetSettings();

  SimpleParams create_params;
  create_params.Set("DLSSNR.Width", width);
  create_params.Set("DLSSNR.Height", height);
  create_params.Set("DLSSNR.ScalingRatio", 1.0f);
  create_params.Set("DLSSNR.Hint.Render.Preset", settings.preset);
  create_params.Set(NVSDK_NGX_Parameter_PerfQualityValue, 2);
  create_params.Set("DLSSNR.Intensity", settings.intensity);
  create_params.Set("DLSSNR.Style", settings.style);
  create_params.Set("DLSSNR.LocalStructureStrength", settings.local_structure);
  create_params.Set("DLSSNR.LocalToneStrength", settings.local_tone);
  create_params.Set("DLSSNR.SkinStructureStrength", settings.skin_structure);
  create_params.Set("DLSSNR.GlobalToneStrength", 1.0f);
  create_params.Set("DLSSNR.UseAutoMask", settings.auto_mask ? 1u : 0u);
  create_params.Set("DLSSNR.UICorrection", 1u);
  create_params.Set("CreationNodeMask", 1u);
  create_params.Set("VisibilityNodeMask", 1u);

  const NVSDK_NGX_Result result = create_feature_(
      command_list,
      static_cast<NVSDK_NGX_Feature>(18),
      &create_params,
      &feature_handle_);

  if (result != NVSDK_NGX_Result_Success || feature_handle_ == nullptr) {
    feature_handle_ = nullptr;
    reason_ = "DLSSNR CreateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  built_width_ = width;
  built_height_ = height;

  std::string msg = "Direct DLSSNR feature created at ";
  msg += std::to_string(width) + "x" + std::to_string(height);
  reshade::log::message(reshade::log::level::info, msg.c_str());

  return true;
}

void DlssNrDirect::Barrier(
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

bool DlssNrDirect::EnsureCommandInfrastructure(ID3D12Device* device) {
  if (command_allocator_ != nullptr) return true;
  if (device == nullptr) return false;

  if (FAILED(device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&command_allocator_)))) {
    reason_ = "failed to create DLSSNR command allocator";
    return false;
  }

  if (FAILED(device->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          command_allocator_,
          nullptr,
          IID_PPV_ARGS(&command_list_)))) {
    reason_ = "failed to create DLSSNR command list";
    return false;
  }

  command_list_->Close();

  if (FAILED(device->CreateFence(
          0,
          D3D12_FENCE_FLAG_NONE,
          IID_PPV_ARGS(&fence_)))) {
    reason_ = "failed to create DLSSNR fence";
    return false;
  }

  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    reason_ = "failed to create DLSSNR fence event";
    return false;
  }

  return true;
}

void DlssNrDirect::WaitGpu() {
  if (command_queue_ == nullptr || fence_ == nullptr) return;

  const UINT64 value = ++fence_value_;
  command_queue_->Signal(fence_, value);

  if (fence_->GetCompletedValue() < value) {
    fence_->SetEventOnCompletion(value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }

  DlssGuides::Get().ReleaseRetiredGameResources();
}

void DlssNrDirect::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (failed_ || command_queue == nullptr || swapchain == nullptr) return;

  IDXGISwapChain3* swapchain3 = nullptr;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) || swapchain3 == nullptr) {
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;

  if (!EnsureModelAndPatch() || !EnsureInitialized(device) || !EnsureCommandInfrastructure(device)) {
    failed_ = true;
    reshade::log::message(reshade::log::level::error, reason_);
    swapchain3->Release();
    device->Release();
    return;
  }

  command_queue_ = command_queue;

  const UINT back_buffer_index = swapchain3->GetCurrentBackBufferIndex();
  ID3D12Resource* backbuffer = nullptr;

  if (FAILED(swapchain3->GetBuffer(
          back_buffer_index,
          IID_PPV_ARGS(&backbuffer))) || backbuffer == nullptr) {
    swapchain3->Release();
    device->Release();
    return;
  }

  const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = static_cast<unsigned int>(desc.Height);

  if (output_ == nullptr || output_width_ != width || output_height_ != height ||
      output_->GetDesc().Format != desc.Format) {
    if (output_ != nullptr) {
      output_->Release();
      output_ = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC output_desc = desc;
    output_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&output_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR output texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    output_width_ = width;
    output_height_ = height;
  }

  if (ui_mask_ == nullptr || ui_mask_width_ != width || ui_mask_height_ != height ||
      ui_mask_format_ != desc.Format) {
    if (ui_mask_ != nullptr) {
      ui_mask_->Release();
      ui_mask_ = nullptr;
    }

    D3D12_HEAP_PROPERTIES mask_heap = {};
    mask_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC mask_desc = desc;
    mask_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &mask_heap,
            D3D12_HEAP_FLAG_NONE,
            &mask_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&ui_mask_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR UI mask texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    ui_mask_width_ = width;
    ui_mask_height_ = height;
    ui_mask_format_ = desc.Format;
  }

  if (composite_output_ == nullptr ||
      composite_output_width_ != width ||
      composite_output_height_ != height ||
      composite_output_format_ != desc.Format) {
    if (composite_output_ != nullptr) {
      composite_output_->Release();
      composite_output_ = nullptr;
    }

    D3D12_HEAP_PROPERTIES composite_heap = {};
    composite_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC composite_desc = desc;
    composite_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &composite_heap,
            D3D12_HEAP_FLAG_NONE,
            &composite_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&composite_output_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR composite output texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    composite_output_width_ = width;
    composite_output_height_ = height;
    composite_output_format_ = desc.Format;
  }

  command_allocator_->Reset();
  command_list_->Reset(command_allocator_, nullptr);

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().CaptureSelectedPassIfNeeded(
        command_list_,
        pending_selected_pass_);

    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  if (feature_handle_ == nullptr) {
    if (!EnsureFeature(command_list_, width, height)) {
      failed_ = true;
      command_list_->Close();
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created; skipping evaluation this frame");
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  const DlssGuideState guides = DlssGuides::Get().GetState();

  const bool use_selected_input =
      settings.use_selected_pass_as_input &&
      guides.selected_pass_color != nullptr;

  ID3D12Resource* model_input = use_selected_input
                                    ? guides.selected_pass_color
                                    : backbuffer;

  // The UI mask is always generated from the selected clean pass when one is
  // available. Without a selected pass, no UI mask is used at all.
  ID3D12Resource* pre_ui_source = guides.selected_pass_color;

  if (debug_codec_ == nullptr) {
    debug_codec_ = std::make_unique<DlssNrCodec>();
  }

  if (!debug_codec_->IsInit() && !debug_codec_->Init(device)) {
    failed_ = true;
    reason_ = "failed to initialize debug codec";
    reshade::log::message(reshade::log::level::error, reason_);
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (settings.debug_view == 1 ||
      ((settings.debug_view == 2 || settings.debug_view == 3 ||
        settings.debug_view == 5 || settings.debug_view == 6) &&
       (guides.valid || settings.debug_view == 6))) {
    command_allocator_->Reset();
    command_list_->Reset(command_allocator_, nullptr);

    ID3D12Resource* debug_input = nullptr;
    unsigned int debug_guide_width = width;
    unsigned int debug_guide_height = height;

    if (settings.debug_view == 1) {
      debug_input = model_input;
      debug_guide_width = width;
      debug_guide_height = height;
    } else if (settings.debug_view == 2) {
      debug_input = guides.depth;
      debug_guide_width = guides.guide_width;
      debug_guide_height = guides.guide_height;
    } else if (settings.debug_view == 3) {
      debug_input = guides.motion;
      debug_guide_width = guides.guide_width;
      debug_guide_height = guides.guide_height;
    } else if (settings.debug_view == 5) {
      debug_input = guides.pre_ui_color;
      debug_guide_width = width;
      debug_guide_height = height;
    } else if (settings.debug_view == 6) {
      debug_input = guides.selected_pass_color;
      debug_guide_width = guides.selected_width != 0 ? guides.selected_width : width;
      debug_guide_height = guides.selected_height != 0 ? guides.selected_height : height;
    }

    if (debug_input == nullptr) {
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants debug_constants = {};
    debug_constants.Mode = DlssNrMode_Resolve;
    debug_constants.DebugView = 1;
    debug_constants.Passthrough = 1;
    debug_constants.WhitePoint = 1.0f;
    debug_constants.Width = width;
    debug_constants.Height = height;
    debug_constants.GuideWidth = debug_guide_width;
    debug_constants.GuideHeight = debug_guide_height;
    debug_constants.MvScaleX = 1.0f;
    debug_constants.MvScaleY = 1.0f;

    debug_codec_->Dispatch(
        command_list_,
        debug_constants,
        debug_input,
        debug_input,
        debug_input,
        nullptr,
        nullptr,
        output_,
        nullptr);

    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (settings.debug_view == 4 && guides.valid &&
      pre_ui_source != nullptr && ui_mask_ != nullptr) {
    if (!debug_codec_->InitMaskShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize UI mask shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants mask_constants = {};
    mask_constants.Mode = DlssNrMode_Encode;
    mask_constants.WhitePoint = 1.0f;
    mask_constants.Width = width;
    mask_constants.Height = height;
    mask_constants.Passthrough = 1u;
    mask_constants.MaxRatio = settings.ui_mask_threshold;

    debug_codec_->DispatchMask(
        command_list_,
        mask_constants,
        pre_ui_source,
        backbuffer,
        ui_mask_);

    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, ui_mask_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  bool ui_mask_generated = false;

  if (guides.valid && pre_ui_source != nullptr && ui_mask_ != nullptr) {
    if (!debug_codec_->InitMaskShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize UI mask shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants mask_constants = {};
    mask_constants.Mode = DlssNrMode_Encode;
    mask_constants.WhitePoint = 1.0f;
    mask_constants.Width = width;
    mask_constants.Height = height;
    mask_constants.Passthrough = 1u;
    mask_constants.MaxRatio = settings.ui_mask_threshold;

    debug_codec_->DispatchMask(
        command_list_,
        mask_constants,
        pre_ui_source,
        backbuffer,
        ui_mask_);

    ui_mask_generated = true;
  }

  SimpleParams eval_params;
  eval_params.Set("DLSSNR.Color", model_input);
  eval_params.Set("DLSSNR.Output", output_);
  eval_params.Set("DLSSNR.Enabled", 1u);
  eval_params.Set("DLSSNR.Width", width);
  eval_params.Set("DLSSNR.Height", height);
  eval_params.Set("DLSSNR.Hint.Render.Preset", settings.preset);
  eval_params.Set("DLSSNR.Intensity", settings.intensity);
  eval_params.Set("DLSSNR.Style", settings.style);
  eval_params.Set("DLSSNR.LocalStructureStrength", settings.local_structure);
  eval_params.Set("DLSSNR.LocalToneStrength", settings.local_tone);
  eval_params.Set("DLSSNR.SkinStructureStrength", settings.skin_structure);
  eval_params.Set("DLSSNR.GlobalToneStrength", 1.0f);
  eval_params.Set("DLSSNR.UseAutoMask", settings.auto_mask ? 1u : 0u);

  if (ui_mask_generated && !use_selected_input && ui_mask_ != nullptr) {
    eval_params.Set("DLSSNR.UIAlpha", ui_mask_);
    eval_params.Set("DLSSNR.Backbuffer", backbuffer);
  }

  if (guides.valid) {
    eval_params.Set("DLSSNR.Depth", guides.depth);
    eval_params.Set("DLSSNR.MVec", guides.motion);
    eval_params.Set("DLSSNR.DepthInverted", guides.depth_inverted ? 1u : 0u);
    eval_params.Set("DLSSNR.MVecScaleX", guides.mv_scale_x);
    eval_params.Set("DLSSNR.MVecScaleY", guides.mv_scale_y);

    eval_params.Set("DLSSNR.ColorSubrectBaseX", 0u);
    eval_params.Set("DLSSNR.ColorSubrectBaseY", 0u);
    eval_params.Set("DLSSNR.ColorSubrectWidth", width);
    eval_params.Set("DLSSNR.ColorSubrectHeight", height);
    eval_params.Set("DLSSNR.OutputSubrectBaseX", 0u);
    eval_params.Set("DLSSNR.OutputSubrectBaseY", 0u);
    eval_params.Set("DLSSNR.OutputSubrectWidth", width);
    eval_params.Set("DLSSNR.OutputSubrectHeight", height);
    eval_params.Set("DLSSNR.DepthSubrectBaseX", 0u);
    eval_params.Set("DLSSNR.DepthSubrectBaseY", 0u);
    eval_params.Set("DLSSNR.DepthSubrectWidth", guides.guide_width);
    eval_params.Set("DLSSNR.DepthSubrectHeight", guides.guide_height);
    eval_params.Set("DLSSNR.MVecSubrectBaseX", 0u);
    eval_params.Set("DLSSNR.MVecSubrectBaseY", 0u);
    eval_params.Set("DLSSNR.MVecSubrectWidth", guides.guide_width);
    eval_params.Set("DLSSNR.MVecSubrectHeight", guides.guide_height);
  }

  eval_params.Set("DLSSNR.Reset", first_evaluate_ ? 1u : 0u);
  first_evaluate_ = false;

  if (ui_mask_ != nullptr) {
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const NVSDK_NGX_Result result = evaluate_feature_(command_list_, feature_handle_, &eval_params, nullptr);

  if (ui_mask_ != nullptr) {
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  if (use_selected_input && ui_mask_generated &&
      ui_mask_ != nullptr && composite_output_ != nullptr) {
    if (!debug_codec_->InitCompositeShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize UI composite shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants composite_constants = {};
    composite_constants.Mode = DlssNrMode_Encode;
    composite_constants.WhitePoint = 1.0f;
    composite_constants.Width = width;
    composite_constants.Height = height;
    composite_constants.Passthrough = 1u;

    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchComposite(
        command_list_,
        composite_constants,
        output_,
        backbuffer,
        ui_mask_,
        composite_output_);

    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, composite_output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  command_list_->Close();
  ID3D12CommandList* const lists[] = {command_list_};
  command_queue_->ExecuteCommandLists(1, lists);
  WaitGpu();

  static bool reported_first_evaluate = false;
  if (!reported_first_evaluate) {
    reported_first_evaluate = true;
    std::string message = "Direct DLSSNR evaluate result: 0x";
    char buffer[16] = {};
    sprintf_s(buffer, "%X", static_cast<unsigned int>(result));
    message += buffer;
    reshade::log::message(reshade::log::level::info, message.c_str());
  }

  if (result != NVSDK_NGX_Result_Success) {
    failed_ = true;
    reason_ = "DLSSNR EvaluateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }

  backbuffer->Release();
  swapchain3->Release();
  device->Release();
}

void DlssNrDirect::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (feature_handle_ != nullptr && release_feature_ != nullptr) {
    release_feature_(feature_handle_);
    feature_handle_ = nullptr;
  }

  if (output_ != nullptr) {
    output_->Release();
    output_ = nullptr;
  }

  if (ui_mask_ != nullptr) {
    ui_mask_->Release();
    ui_mask_ = nullptr;
  }

  if (composite_output_ != nullptr) {
    composite_output_->Release();
    composite_output_ = nullptr;
  }

  ui_mask_width_ = 0;
  ui_mask_height_ = 0;
  ui_mask_format_ = DXGI_FORMAT_UNKNOWN;
  composite_output_width_ = 0;
  composite_output_height_ = 0;
  composite_output_format_ = DXGI_FORMAT_UNKNOWN;

  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
    fence_event_ = INVALID_HANDLE_VALUE;
  }

  if (fence_ != nullptr) {
    fence_->Release();
    fence_ = nullptr;
  }

  if (command_list_ != nullptr) {
    command_list_->Release();
    command_list_ = nullptr;
  }

  if (command_allocator_ != nullptr) {
    command_allocator_->Release();
    command_allocator_ = nullptr;
  }

  command_queue_ = nullptr;
  fence_value_ = 0;
  built_width_ = 0;
  built_height_ = 0;
  output_width_ = 0;
  output_height_ = 0;
  first_evaluate_ = true;

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  failed_ = false;
  reason_ = "";
}

}  // namespace reshade_dlssnr
