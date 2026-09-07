// Driver NGX capability parameter block access with vtable slot probing.
#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

namespace reshade_dlssnr {

class NgxParameterBlock {
 public:
  bool Initialize();
  void Shutdown();

  bool IsReady() const { return params_ != nullptr; }
  NVSDK_NGX_Parameter* Get() const { return params_; }

  void SetUInt(const char* name, unsigned int value);
  void SetInt(const char* name, int value);
  void SetFloat(const char* name, float value);
  void SetResource(const char* name, ID3D12Resource* value);
  void SetPointer(const char* name, void* value);

 private:
  void ProbeFloatSlot();

  HMODULE core_module_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  int float_slot_ = 1;
  bool float_slot_known_ = false;
};

}  // namespace reshade_dlssnr
