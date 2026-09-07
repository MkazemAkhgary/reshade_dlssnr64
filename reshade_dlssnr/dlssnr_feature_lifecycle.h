#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include "dlssnr_runtime.h"
#include "settings.h"

namespace reshade_dlssnr {

class DlssNrFeatureLifecycle {
 public:
  bool EnsureFeature(
      const DlssNrRuntime& runtime,
      const PassSettings& pass,
      ID3D12GraphicsCommandList* command_list,
      unsigned int width,
      unsigned int height);

  void ReleaseFeature(const DlssNrRuntime& runtime);

  bool HasFeature() const { return feature_handle_ != nullptr; }
  NVSDK_NGX_Handle* FeatureHandle() const { return feature_handle_; }

  bool ShouldReset() const { return first_evaluate_; }
  void MarkEvaluated() { first_evaluate_ = false; }
  void SetShouldReset(bool value) { first_evaluate_ = value; }

  void Shutdown(const DlssNrRuntime& runtime);

  const char* GetLastError() const { return reason_; }

 private:
  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  bool first_evaluate_ = true;
  const char* reason_ = "";

  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;
  float built_working_scale_ = -1.0f;
  unsigned int built_preset_ = 0xFFFFFFFFu;
  float built_intensity_ = -1.0f;
  float built_local_structure_ = -1.0f;
  float built_local_tone_ = -1.0f;
  float built_skin_structure_ = -1.0f;
  bool built_auto_mask_ = false;
  bool built_ui_correction_ = false;
};

}  // namespace reshade_dlssnr
