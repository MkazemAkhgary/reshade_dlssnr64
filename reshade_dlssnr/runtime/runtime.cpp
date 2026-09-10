#include "dlssnr_runtime.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

namespace {

std::mutex g_runtime_mutex;

std::filesystem::path GetModuleDirectory() {
  HMODULE module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(&g_runtime_mutex),
      &module);

  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).remove_filename();
}

wchar_t g_spoof_path[MAX_PATH] =
    L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nvmdi.inf_amd64\\nvngx.dll";

DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
  const DWORD len = static_cast<DWORD>(wcslen(g_spoof_path));
  const DWORD copy_len = len >= size ? size - 1 : len;
  wcsncpy_s(filename, size, g_spoof_path, copy_len);
  return copy_len;
}

bool PatchImport(HMODULE module, const char* dll_name,
                 const char* function_name, void* replacement) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  auto& import_dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_dir.VirtualAddress == 0) return false;

  auto* import_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
      base + import_dir.VirtualAddress);
  for (; import_desc->Name != 0; ++import_desc) {
    const char* import_dll_name =
        reinterpret_cast<const char*>(base + import_desc->Name);
    if (_stricmp(import_dll_name, dll_name) != 0) continue;

    auto thunk_rva = import_desc->OriginalFirstThunk != 0
                         ? import_desc->OriginalFirstThunk
                         : import_desc->FirstThunk;
    auto* original_thunk =
        reinterpret_cast<IMAGE_THUNK_DATA*>(base + thunk_rva);
    auto* iat_thunk =
        reinterpret_cast<IMAGE_THUNK_DATA*>(base + import_desc->FirstThunk);

    for (; original_thunk->u1.AddressOfData != 0;
         ++original_thunk, ++iat_thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) continue;

      auto* import_by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
          base + original_thunk->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(import_by_name->Name),
                 function_name) != 0) continue;

      void** slot = reinterpret_cast<void**>(&iat_thunk->u1.Function);
      DWORD old_protect = 0;
      if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        return false;
      }

      *slot = replacement;
      DWORD ignored = 0;
      VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
      return true;
    }
  }

  return false;
}

}  // namespace

bool NgxRuntime::EnsureModelAndPatch() {
  if (model_module_ != nullptr) return true;

  model_module_ = GetModuleHandleW(L"nvngx_dlssnr.dll");
  if (model_module_ == nullptr) {
    model_module_ = LoadLibraryW(L"nvngx_dlssnr.dll");
  }

  if (model_module_ == nullptr) {
    reason_ = "nvngx_dlssnr.dll not found";
    return false;
  }

  PatchImport(model_module_, "KERNEL32.dll", "GetModuleFileNameW",
              HookedGetModuleFileNameW);

  init_ext_ = reinterpret_cast<PFN_InitExt>(
      GetProcAddress(model_module_, "NVSDK_NGX_D3D12_Init_Ext"));
  create_feature_ = reinterpret_cast<PFN_CreateFeature>(
      GetProcAddress(model_module_, "NVSDK_NGX_D3D12_CreateFeature"));
  evaluate_feature_ = reinterpret_cast<PFN_EvaluateFeature>(
      GetProcAddress(model_module_, "NVSDK_NGX_D3D12_EvaluateFeature"));
  release_feature_ = reinterpret_cast<PFN_ReleaseFeature>(
      GetProcAddress(model_module_, "NVSDK_NGX_D3D12_ReleaseFeature"));

  if (create_feature_ == nullptr || evaluate_feature_ == nullptr) {
    reason_ = "nvngx_dlssnr.dll is missing required exports";
    return false;
  }

  reshade::log::message(
      reshade::log::level::info,
      "Loaded and patched nvngx_dlssnr.dll directly");
  return true;
}

bool NgxRuntime::EnsureInitialized(ID3D12Device* device) {
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
  reshade::log::message(
      reshade::log::level::info,
      "nvngx_dlssnr.dll initialized directly");
  return true;
}

bool NgxRuntime::Initialize(ID3D12Device* device) {
  std::lock_guard<std::mutex> lock(g_runtime_mutex);

  if (!EnsureModelAndPatch()) return false;
  if (!EnsureInitialized(device)) return false;
  if (!parameter_block_.Initialize()) {
    reason_ = "driver NGX capability parameter block unavailable";
    return false;
  }

  return true;
}

void NgxRuntime::Shutdown() {
  std::lock_guard<std::mutex> lock(g_runtime_mutex);
  parameter_block_.Shutdown();
  initialized_ = false;
}

NVSDK_NGX_Result NgxRuntime::CreateFeature(
    ID3D12GraphicsCommandList* command_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** out_handle) {
  if (create_feature_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return create_feature_(command_list, feature_id, params, out_handle);
}

NVSDK_NGX_Result NgxRuntime::EvaluateFeature(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* params,
    void* callback) {
  if (evaluate_feature_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return evaluate_feature_(command_list, feature_handle, params, callback);
}

NVSDK_NGX_Result NgxRuntime::ReleaseFeature(NVSDK_NGX_Handle* feature_handle) {
  if (release_feature_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return release_feature_(feature_handle);
}

}  // namespace reshade_dlssnr
