#include "model/runtime.h"
#include "profiling/profile.h"

#include <cstdio>

#include <reshade.hpp>

#pragma comment(lib, "advapi32.lib")

namespace reshade_dlssnr {

// Depth of the addon's own model evaluation on the current thread. The driver's
// NGX core sits downstream of every model module, so a detour on the core sees
// the addon's own evaluations as well as the game's.
static thread_local int g_evaluation_depth = 0;

bool AddonEvaluationInProgress() { return g_evaluation_depth > 0; }

namespace {

// Marks an evaluation for its duration, including any nested call the model
// makes into the driver's core.
struct EvaluationDepthScope {
  EvaluationDepthScope() { ++g_evaluation_depth; }
  ~EvaluationDepthScope() { --g_evaluation_depth; }
};

std::mutex g_runtime_mutex;

// The driver's NGX core, as named from driver 471.11 onwards.
constexpr const wchar_t* kCoreModuleName = L"_nvngx.dll";

// Reported to the model module when it asks where it was loaded from. The
// check it satisfies is on the directory: the model has to look like it was
// installed next to the driver's core.
wchar_t g_spoof_path[MAX_PATH] = {};

// Used only when neither the process nor the driver's registry keys name a
// directory for the installed core.
constexpr const wchar_t* kFallbackSpoofPath =
    L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nvmdi.inf_amd64\\nvngx.dll";

std::filesystem::path GetModuleDirectory() {
  HMODULE module = nullptr;
  // Use a function address for FROM_ADDRESS lookup.
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(&GetModuleDirectory),
      &module);

  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).remove_filename();
}

bool ReadRegistryString(const wchar_t* subkey, const wchar_t* value,
                        wchar_t* out, DWORD size) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) {
    return false;
  }

  DWORD type = 0;
  DWORD bytes = size * sizeof(wchar_t);
  const LONG status = RegQueryValueExW(key, value, nullptr, &type,
                                       reinterpret_cast<LPBYTE>(out), &bytes);
  RegCloseKey(key);

  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return false;
  }
  out[size - 1] = L'\0';
  return out[0] != L'\0';
}

// Compares two directory paths without regard to case or a trailing separator.
bool SameDirectory(const std::wstring& left_path, const std::wstring& right_path) {
  std::wstring left = left_path;
  std::wstring right = right_path;
  while (!left.empty() && left.back() == L'\\') left.pop_back();
  while (!right.empty() && right.back() == L'\\') right.pop_back();
  return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

// The directory the installed driver keeps its NGX core in. A driver install
// lays down a new DriverStore folder, so this is read from the process or from
// the driver's own registry keys rather than written down here.
bool ResolveDriverNgxDirectory(wchar_t* out, DWORD size) {
  // Only a core the driver already put in the process is trusted. Loading it by
  // bare name here could pick up a stray copy from the game directory, which is
  // the one location the model must not appear to live in.
  HMODULE core = GetModuleHandleW(kCoreModuleName);
  if (core != nullptr) {
    wchar_t full[MAX_PATH] = {};
    if (GetModuleFileNameW(core, full, MAX_PATH) != 0) {
      wchar_t* separator = wcsrchr(full, L'\\');
      if (separator != nullptr) {
        *separator = L'\0';
        // Streamline and some games ship their own copy next to the executable.
        if (!SameDirectory(full, GetModuleDirectory().wstring())) {
          wcscpy_s(out, size, full);
          return true;
        }
      }
    }
  }

  struct RegistryKey {
    const wchar_t* subkey;
    const wchar_t* value;
  };
  static const RegistryKey kKeys[] = {
      {L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
       L"NGXPath"},
      {L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath"},
      {L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath"},
  };
  for (const RegistryKey& key : kKeys) {
    if (ReadRegistryString(key.subkey, key.value, out, size)) return true;
  }

  return false;
}

// Builds the path the model module is told it lives at. The file name stays
// `nvngx.dll`; only the directory follows the installed driver.
bool EnsureSpoofPath() {
  if (g_spoof_path[0] != L'\0') return true;

  wchar_t directory[MAX_PATH] = {};
  wchar_t path[MAX_PATH] = {};
  if (ResolveDriverNgxDirectory(directory, MAX_PATH) &&
      swprintf_s(path, L"%s\\nvngx.dll", directory) > 0) {
    wcscpy_s(g_spoof_path, path);
    return true;
  }

  wcscpy_s(g_spoof_path, kFallbackSpoofPath);
  reshade::log::message(
      reshade::log::level::warning,
      "DLSSNR: the installed driver's NGX directory could not be resolved; "
      "the model is given the built-in path");
  return false;
}

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
  DLSSNR_PROFILE_CORE_L1("NgxRuntime::EnsureModule");
  if (model_module_ != nullptr) return true;

  model_module_ = GetModuleHandleW(L"nvngx_dlssnr.dll");
  if (model_module_ == nullptr) {
    model_module_ = LoadLibraryW(L"nvngx_dlssnr.dll");
  }

  if (model_module_ == nullptr) {
    reason_ = "nvngx_dlssnr.dll not found";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  EnsureSpoofPath();

  // A failed patch is not fatal here, because the model may reach
  // GetModuleFileNameW through an API set rather than through KERNEL32. It is
  // still the first thing to suspect when initialization is refused.
  if (!PatchImport(model_module_, "KERNEL32.dll", "GetModuleFileNameW",
                   HookedGetModuleFileNameW)) {
    reshade::log::message(
        reshade::log::level::warning,
        "DLSSNR: nvngx_dlssnr.dll does not import GetModuleFileNameW from "
        "KERNEL32; its location check cannot be satisfied");
  }

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
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  reshade::log::message(
      reshade::log::level::info,
      "Loaded and patched nvngx_dlssnr.dll directly");
  return true;
}

bool NgxRuntime::EnsureInitialized(ID3D12Device* device) {
  DLSSNR_PROFILE_CORE_L2("NgxRuntime::EnsureInitialized");
  if (initialized_) return true;
  if (init_ext_ == nullptr) {
    reason_ = "nvngx_dlssnr.dll does not export NVSDK_NGX_D3D12_Init_Ext";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  const std::wstring data_path = GetModuleDirectory().wstring();
  const NVSDK_NGX_Result result = init_ext_(
      0ull,
      data_path.c_str(),
      device,
      NVSDK_NGX_Version_API,
      nullptr);

  if (result != NVSDK_NGX_Result_Success) {
    reason_ = "nvngx_dlssnr.dll Init_Ext failed";
    char message[192];
    std::snprintf(message, sizeof(message),
                  "DLSSNR: Init_Ext failed: %s (0x%08X); SDK version asked "
                  "for was 0x%08X",
                  NgxResultName(result), static_cast<unsigned int>(result),
                  static_cast<unsigned int>(NVSDK_NGX_Version_API));
    reshade::log::message(reshade::log::level::error, message);
    return false;
  }

  initialized_ = true;
  reshade::log::message(
      reshade::log::level::info,
      "nvngx_dlssnr.dll initialized directly");
  return true;
}

bool NgxRuntime::Initialize(ID3D12Device* device) {
  DLSSNR_PROFILE_CORE_L1("NgxRuntime::Initialize");
  std::lock_guard<std::mutex> lock(g_runtime_mutex);

  if (!EnsureModelAndPatch()) return false;
  if (!EnsureInitialized(device)) return false;
  if (!parameter_block_.Initialize()) {
    reason_ = "driver NGX capability parameter block unavailable";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  return true;
}

void NgxRuntime::Shutdown() {
  DLSSNR_PROFILE_CORE_L1("NgxRuntime::Shutdown");
  std::lock_guard<std::mutex> lock(g_runtime_mutex);
  parameter_block_.Shutdown();
  initialized_ = false;
}

NVSDK_NGX_Result NgxRuntime::CreateFeature(
    ID3D12GraphicsCommandList* command_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Handle** out_handle) {
  DLSSNR_PROFILE_CORE_L2("NgxRuntime::CreateFeature");
  if (create_feature_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return create_feature_(command_list, feature_id, params, out_handle);
}

NVSDK_NGX_Result NgxRuntime::EvaluateFeature(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* feature_handle,
    const NVSDK_NGX_Parameter* params,
    void* callback) {
  DLSSNR_PROFILE_CORE_L2("NgxRuntime::EvaluateFeature");
  if (evaluate_feature_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  const EvaluationDepthScope depth;
  return evaluate_feature_(command_list, feature_handle, params, callback);
}

NVSDK_NGX_Result NgxRuntime::ReleaseFeature(NVSDK_NGX_Handle* feature_handle) {
  DLSSNR_PROFILE_RESOURCE_L2("NgxRuntime::ReleaseFeature");
  if (release_feature_ == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
  return release_feature_(feature_handle);
}

const char* NgxResultName(NVSDK_NGX_Result result) {
  switch (result) {
    case NVSDK_NGX_Result_Success:
      return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
      return "FAIL_FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError:
      return "FAIL_PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:
      return "FAIL_FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound:
      return "FAIL_FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter:
      return "FAIL_InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:
      return "FAIL_ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized:
      return "FAIL_NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
      return "FAIL_UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing:
      return "FAIL_RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput:
      return "FAIL_MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
      return "FAIL_UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate:
      return "FAIL_OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
      return "FAIL_OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
      return "FAIL_UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
      return "FAIL_UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter:
      return "FAIL_UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied:
      return "FAIL_Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented:
      return "FAIL_NotImplemented";
    default:
      // The enum grows with every SDK release, so an unrecognised code is
      // reported numerically rather than as a bare "UNKNOWN".
      static thread_local char unknown[24];
      std::snprintf(unknown, sizeof(unknown), "0x%08X",
                    static_cast<unsigned int>(result));
      return unknown;
  }
}

}  // namespace reshade_dlssnr
