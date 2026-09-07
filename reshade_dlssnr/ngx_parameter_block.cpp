#include "ngx_parameter_block.h"

namespace reshade_dlssnr {

namespace {

using PFN_SetUInt = void(__thiscall*)(void*, const char*, unsigned int);
using PFN_SetInt = void(__thiscall*)(void*, const char*, int);
using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);
using PFN_SetULL = void(__thiscall*)(void*, const char*, unsigned long long);

constexpr int kNgxSetUllSlot = 0;
constexpr int kNgxSetUIntSlot = 3;

void CallSetUInt(void* params, const char* name, unsigned int value) {
  void** vtable = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetUInt>(vtable[kNgxSetUIntSlot])(params, name, value);
}

void CallSetInt(void* params, const char* name, int value) {
  void** vtable = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetInt>(vtable[kNgxSetUIntSlot])(params, name, value);
}

void CallSetFloat(void* params, const char* name, float value, int slot) {
  void** vtable = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetFloat>(vtable[slot])(params, name, value);
}

void CallSetResource(void* params, const char* name, ID3D12Resource* value) {
  void** vtable = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetULL>(vtable[kNgxSetUllSlot])(
      params, name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(value)));
}

void CallSetPointer(void* params, const char* name, void* value) {
  void** vtable = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetULL>(vtable[kNgxSetUllSlot])(
      params, name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(value)));
}

}  // namespace

bool NgxParameterBlock::Initialize() {
  if (params_ != nullptr) return true;

  core_module_ = GetModuleHandleW(L"_nvngx.dll");
  if (core_module_ == nullptr) {
    core_module_ = LoadLibraryW(L"_nvngx.dll");
  }

  if (core_module_ == nullptr) {
    return false;
  }

  using PFN_GetCapabilityParameters =
      NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
  auto* get_capability_parameters =
      reinterpret_cast<PFN_GetCapabilityParameters>(
          GetProcAddress(core_module_, "NVSDK_NGX_D3D12_GetCapabilityParameters"));

  if (get_capability_parameters == nullptr) {
    return false;
  }

  NVSDK_NGX_Parameter* params = nullptr;
  const NVSDK_NGX_Result result = get_capability_parameters(&params);

  if (result != NVSDK_NGX_Result_Success || params == nullptr) {
    return false;
  }

  params_ = params;
  ProbeFloatSlot();
  return true;
}

void NgxParameterBlock::ProbeFloatSlot() {
  if (params_ == nullptr || float_slot_known_) return;

  float_slot_known_ = true;

  static constexpr const char* kProbeKey = "DLSSNR.ReshadeFloatProbe";
  static constexpr float kProbeValue = 0.375f;
  static const int kCandidates[] = {1, 2, 5, 6, 7, 4, 3, 0};

  for (int slot : kCandidates) {
    float read_back = 0.0f;
    CallSetFloat(params_, kProbeKey, kProbeValue, slot);

    if (params_->Get(kProbeKey, &read_back) == NVSDK_NGX_Result_Success &&
        read_back == kProbeValue) {
      float_slot_ = slot;
      return;
    }
  }

  float_slot_ = 1;
}

void NgxParameterBlock::Shutdown() {
  params_ = nullptr;
}

void NgxParameterBlock::SetUInt(const char* name, unsigned int value) {
  if (params_ != nullptr) CallSetUInt(params_, name, value);
}

void NgxParameterBlock::SetInt(const char* name, int value) {
  if (params_ != nullptr) CallSetInt(params_, name, value);
}

void NgxParameterBlock::SetFloat(const char* name, float value) {
  if (params_ != nullptr) CallSetFloat(params_, name, value, float_slot_);
}

void NgxParameterBlock::SetResource(const char* name, ID3D12Resource* value) {
  if (params_ != nullptr) CallSetResource(params_, name, value);
}

void NgxParameterBlock::SetPointer(const char* name, void* value) {
  if (params_ != nullptr) CallSetPointer(params_, name, value);
}

}  // namespace reshade_dlssnr
