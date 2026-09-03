#include "forwarder.h"

namespace reshade_dlssnr {

bool ForwarderApi::Load(const wchar_t* path) {
  if (module != nullptr) return IsLoaded();

  module = LoadLibraryW(path);
  if (module == nullptr) return false;

  set_float_slot = reinterpret_cast<PFN_DlssnrSetFloatSlot>(GetProcAddress(module, "dlssnr_call_set_float_slot"));
  probe_float = reinterpret_cast<PFN_DlssnrProbeFloat>(GetProcAddress(module, "dlssnr_call_probe_float"));
  create = reinterpret_cast<PFN_DlssnrCreate>(GetProcAddress(module, "dlssnr_call_create"));
  evaluate = reinterpret_cast<PFN_DlssnrEvaluate>(GetProcAddress(module, "dlssnr_call_evaluate"));
  release = reinterpret_cast<PFN_DlssnrRelease>(GetProcAddress(module, "dlssnr_call_release"));
  set_extras = reinterpret_cast<PFN_DlssnrSetExtras>(GetProcAddress(module, "dlssnr_call_set_extras"));
  last_init = reinterpret_cast<int*>(GetProcAddress(module, "dlssnr_call_last_init"));
  last_create = reinterpret_cast<int*>(GetProcAddress(module, "dlssnr_call_last_create"));

  return IsLoaded();
}

const wchar_t* GetDefaultForwarderName() {
  return L"nvngx.dll_dlssnr.dll";
}

const wchar_t* GetDefaultModelName() {
  return L"nvngx_dlssnr.dll";
}

}  // namespace reshade_dlssnr
