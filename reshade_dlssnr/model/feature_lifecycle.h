// Feature handle creation, tuning-change detection, and rebuild state.
#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include "model/runtime.h"
#include "state/settings.h"

namespace reshade_dlssnr {

class FeatureLifecycle {
 public:
  bool EnsureFeature(
      NgxRuntime& runtime,
      const PassSettings& pass,
      ID3D12GraphicsCommandList* command_list,
      unsigned int width,
      unsigned int height);

  void ReleaseFeature(NgxRuntime& runtime);

  bool HasFeature() const { return feature_handle_ != nullptr; }
  NVSDK_NGX_Handle* FeatureHandle() const { return feature_handle_; }

  bool ShouldReset() const { return first_evaluate_; }
  void MarkEvaluated() { first_evaluate_ = false; }
  void SetShouldReset(bool value) { first_evaluate_ = value; }

  // True once after EnsureFeature actually (re)created the handle. Callers
  // use this to skip the evaluation frame so the model has one frame to
  // fully initialize. Consuming the flag clears it.
  bool ConsumeJustRebuilt() {
    const bool v = just_rebuilt_;
    just_rebuilt_ = false;
    return v;
  }

  void Shutdown(NgxRuntime& runtime);

  const char* GetLastError() const { return reason_; }

 private:
  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  bool first_evaluate_ = true;
  bool just_rebuilt_ = false;
  const char* reason_ = "";

  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;
};

}  // namespace reshade_dlssnr
