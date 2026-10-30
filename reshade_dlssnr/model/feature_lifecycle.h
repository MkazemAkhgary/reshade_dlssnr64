// Feature handle creation, tuning-change detection, and rebuild state.
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include "feature/pass/view.h"
#include "model/runtime.h"

namespace reshade_dlssnr {

class FeatureLifecycle {
 public:
  bool EnsureFeature(
      NgxRuntime& runtime,
      const feature::pass::PassSnapshot& pass,
      ID3D12GraphicsCommandList* command_list,
      unsigned int width,
      unsigned int height);

  // Moves the active handle to the deferred-retirement list. The controller
  // releases it only after a queue fence proves earlier evaluations completed.
  void RetireFeature(bool request_reset = true);

  bool HasUnfencedRetiredFeatures() const;
  void TagRetiredFeatures(uint64_t fence_value);
  void ReleaseRetiredFeaturesCompleted(NgxRuntime& runtime,
                                       uint64_t completed_fence_value);

  bool HasFeature() const { return feature_handle_ != nullptr; }
  NVSDK_NGX_Handle* FeatureHandle() const { return feature_handle_; }

  bool ShouldReset() const { return reset_pending_; }
  void MarkEvaluated() { reset_pending_ = false; }
  void SetShouldReset(bool value) { reset_pending_ = value; }

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
  struct ModelSettingsState {
    int target = 2;
    unsigned int style = 0;
    unsigned int render_preset = 1;
    float working_scale = 1.0f;
    bool working_scale_x2 = false;
    float local_tone = 1.0f;
    float local_structure = 1.0f;
    float skin_structure = -1.0f;
    bool auto_mask = true;
    bool ui_correction = true;
  };

  // Returns true when DLSSNR.Hint.Render.Preset changed. Unlike other model
  // controls, NGX latches this hint at feature creation, so it requires a
  // feature rebuild rather than only a temporal Reset.
  bool ObserveModelSettings(const feature::pass::PassSnapshot& pass);

  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  // Kept pending until NGX accepts an evaluation carrying DLSSNR.Reset=1.
  bool reset_pending_ = true;
  bool just_rebuilt_ = false;
  const char* reason_ = "";

  struct RetiredFeature {
    NVSDK_NGX_Handle* handle = nullptr;
    uint64_t fence_value = 0;
  };

  std::vector<RetiredFeature> retired_features_;
  ModelSettingsState model_settings_ = {};
  bool model_settings_valid_ = false;

  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;
};

}  // namespace reshade_dlssnr
