#include "model/ngx_parameter_block.h"
#include "profiling/profile.h"

#include <cstdio>

#include <reshade.hpp>

namespace reshade_dlssnr {

namespace {

using PFN_SetUInt = void(__thiscall*)(void*, const char*, unsigned int);
using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);
using PFN_SetULL = void(__thiscall*)(void*, const char*, unsigned long long);

constexpr int kNgxSetUllSlot = 0;

void CallSetUInt(void* params, const char* name, unsigned int value, int slot) {
  void** vtable = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetUInt>(vtable[slot])(params, name, value);
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

}  // namespace

bool NgxParameterBlock::Initialize() {
  DLSSNR_PROFILE_CORE_L2("NgxParameterBlock::Initialize");
  if (params_ != nullptr) return true;

  core_module_ = GetModuleHandleW(L"_nvngx.dll");
  if (core_module_ == nullptr) {
    core_module_ = LoadLibraryW(L"_nvngx.dll");
  }

  if (core_module_ == nullptr) {
    reshade::log::message(
        reshade::log::level::error,
        "DLSSNR: the driver's NGX core (_nvngx.dll) is not in the process and "
        "could not be loaded by name");
    return false;
  }

  using PFN_GetCapabilityParameters =
      NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
  auto* get_capability_parameters =
      reinterpret_cast<PFN_GetCapabilityParameters>(
          GetProcAddress(core_module_, "NVSDK_NGX_D3D12_GetCapabilityParameters"));

  if (get_capability_parameters == nullptr) {
    reshade::log::message(
        reshade::log::level::error,
        "DLSSNR: the driver's NGX core does not export "
        "NVSDK_NGX_D3D12_GetCapabilityParameters");
    return false;
  }

  NVSDK_NGX_Parameter* params = nullptr;
  const NVSDK_NGX_Result result = get_capability_parameters(&params);

  if (result != NVSDK_NGX_Result_Success || params == nullptr) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "DLSSNR: NGX capability parameters unavailable: result "
                  "0x%08X, block %p",
                  static_cast<unsigned int>(result),
                  static_cast<void*>(params));
    reshade::log::message(reshade::log::level::error, message);
    return false;
  }

  params_ = params;
  ProbeUIntSlot();
  ProbeFloatSlot();
  return true;
}

// Each overload is a separate vtable slot, and the driver's table is not the
// one the SDK header describes: the float setter measured at slot 6 on driver
// 621.1, where the header puts it at 1. The remaining slots are measured the
// same way rather than assumed, because a wrong one is silent.
void NgxParameterBlock::ProbeUIntSlot() {
  if (params_ == nullptr || uint_slot_known_) return;

  uint_slot_known_ = true;

  static constexpr const char* kProbeKey = "DLSSNR.ReshadeUIntProbe";
  static constexpr unsigned int kProbeValue = 0x5A5A1234u;
  // 3 is where the header puts Set(unsigned int); the rest follow in the order
  // a table that moved the float setter to 6 would most plausibly take.
  static const int kCandidates[] = {3, 4, 0, 1, 2, 5, 6, 7};

  for (int slot : kCandidates) {
    unsigned int read_back = 0;
    CallSetUInt(params_, kProbeKey, kProbeValue, slot);

    if (params_->Get(kProbeKey, &read_back) == NVSDK_NGX_Result_Success &&
        read_back == kProbeValue) {
      uint_slot_ = slot;
      return;
    }
  }

  uint_slot_ = 3;
  reshade::log::message(
      reshade::log::level::warning,
      "DLSSNR: no candidate vtable slot accepted an unsigned parameter on the "
      "driver's block; assuming slot 3");
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
  reshade::log::message(
      reshade::log::level::warning,
      "DLSSNR: no candidate vtable slot accepted a float parameter on the "
      "driver's block; assuming slot 1");
}

void NgxParameterBlock::Shutdown() {
  params_ = nullptr;
}

void NgxParameterBlock::SetUInt(const char* name, unsigned int value) {
  DLSSNR_PROFILE_CORE_L3("NgxParameterBlock::SetUInt");
  if (params_ != nullptr) CallSetUInt(params_, name, value, uint_slot_);
}

void NgxParameterBlock::SetFloat(const char* name, float value) {
  DLSSNR_PROFILE_CORE_L3("NgxParameterBlock::SetFloat");
  if (params_ != nullptr) CallSetFloat(params_, name, value, float_slot_);
}

void NgxParameterBlock::SetResource(const char* name, ID3D12Resource* value) {
  DLSSNR_PROFILE_CORE_L3("NgxParameterBlock::SetResource");
  if (params_ != nullptr) CallSetResource(params_, name, value);
}

}  // namespace reshade_dlssnr
