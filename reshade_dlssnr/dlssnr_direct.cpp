#include "dlssnr_direct.h"

#include <filesystem>
#include <mutex>

#include <reshade.hpp>

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

void DlssNrDirect::OnFinishEffects(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* target) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (failed_ || command_list == nullptr || target == nullptr) return;

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;

  if (!EnsureModelAndPatch() || !EnsureInitialized(device)) {
    failed_ = true;
    reshade::log::message(reshade::log::level::error, reason_);
    device->Release();
    return;
  }

  const D3D12_RESOURCE_DESC desc = target->GetDesc();
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
      device->Release();
      return;
    }

    output_width_ = width;
    output_height_ = height;
  }

  if (feature_handle_ == nullptr) {
    if (!EnsureFeature(command_list, width, height)) {
      failed_ = true;
      device->Release();
      return;
    }

    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created; skipping evaluation this frame");
    device->Release();
    return;
  }

  SimpleParams eval_params;
  eval_params.Set("DLSSNR.Color", target);
  eval_params.Set("DLSSNR.Output", output_);
  eval_params.Set("DLSSNR.Enabled", 1u);
  eval_params.Set("DLSSNR.Hint.Render.Preset", settings.preset);
  eval_params.Set("DLSSNR.Intensity", settings.intensity);
  eval_params.Set("DLSSNR.Style", settings.style);
  eval_params.Set("DLSSNR.LocalStructureStrength", settings.local_structure);
  eval_params.Set("DLSSNR.LocalToneStrength", settings.local_tone);
  eval_params.Set("DLSSNR.SkinStructureStrength", settings.skin_structure);
  eval_params.Set("DLSSNR.GlobalToneStrength", 1.0f);
  eval_params.Set("DLSSNR.UseAutoMask", settings.auto_mask ? 1u : 0u);
  eval_params.Set("DLSSNR.Reset", 1u);

  Barrier(command_list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const NVSDK_NGX_Result result = evaluate_feature_(command_list, feature_handle_, &eval_params, nullptr);
  Barrier(command_list, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(command_list, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(target, output_);
  Barrier(command_list, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
  Barrier(command_list, output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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

  built_width_ = 0;
  built_height_ = 0;
  output_width_ = 0;
  output_height_ = 0;
  failed_ = false;
  reason_ = "";
}

}  // namespace reshade_dlssnr
