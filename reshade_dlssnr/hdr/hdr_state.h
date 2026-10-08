// HDR measurement state. Owns the histogram buffer, readback ring,
// and global exposure / white-point adaptation. HDR exposure uses the full
// scene histogram; depth and skin masks are deliberately not part of metering.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <chrono>

#include "command_context.h"
#include "compute_engine.h"
#include "gpu_readback.h"
#include "state/settings.h"

namespace reshade_dlssnr {

enum class MeteringSource {
  FullFrame = 0,
};

struct ExposureParams {
  float midtone = 0.18f;
  float white_percentile = 0.99f;  // Legacy Median method only.
  HdrExposureMethod exposure_method = HdrExposureMethod::AutoLocal;
  float auto_low_percent = 0.70f;
  float auto_high_percent = 0.90f;
  float speed_up = 3.0f;
  float speed_down = 1.0f;
  float rate_cap = 0.1f;           // Legacy Median method only.
  uint32_t measure_interval = 4;
};

struct ExposureTracker {
  float exposure = 1.0f;
  // Smoothed white percentile from the same full-frame metering histogram,
  // in the 100-nit SDR/gain-map working domain.
  float scene_white = 1.0f;
  float target_exposure = 1.0f;
  float exposure_reference_luma = 0.0f;
  float warmup_frames = 0.0f;
  bool have_measurement = false;
  MeteringSource metering_source = MeteringSource::FullFrame;

  void Reset();
  void Update(const uint32_t* bins, const ExposureParams& params,
              float delta_seconds);
};

struct HdrSnapshot {
  bool enabled = false;
  bool ready = false;
  HdrFormat resolved_format = HdrFormat::Sdr;
  float exposure = 1.0f;
  float scene_white = 1.0f;
  uint32_t frames_since_measure = 0;

  bool has_histogram = false;
  uint32_t histogram[256] = {};
  float exposure_reference_luma = 0.0f;
  float p_hi_luma = 0.0f;
  float target_exposure = 1.0f;
  uint32_t samples = 0;
  uint64_t measurement_generation = 0;
  int metering_source = 0;

  bool gain_map_ready = false;
  float gain_min_log2 = 0.0f;
  float gain_max_log2 = 0.0f;
  uint32_t gain_sdr_clipped_pixels = 0;
  uint32_t gain_out_of_range_pixels = 0;
};

HdrFormat DetectFormatFromDxgi(DXGI_FORMAT format);

class HdrState {
 public:
  bool Ensure(ID3D12Device* device);
  void Shutdown();

  void BeginFrame(CommandContext& cmd,
                  ID3D12GraphicsCommandList* command_list,
                  ID3D12Resource* source,
                  unsigned int width,
                  unsigned int height,
                  const Settings& settings,
                  ComputeEngine& codec);

  bool EnqueueGainMapStatsReadback(ID3D12GraphicsCommandList* command_list,
                                   ID3D12Resource* stats);

  void SignalAtPresent(ID3D12CommandQueue* queue);

  bool Enabled() const { return enabled_; }
  HdrFormat ResolvedFormat() const { return resolved_format_; }
  float Exposure() const { return regular_.exposure; }
  float SceneWhite() const { return regular_.scene_white; }
  float EffectiveSceneWhite(const HdrSettings& hdr) const {
    return CalcEffectiveSceneWhite(SceneWhite(), hdr);
  }
  float EffectiveSceneWhiteNits(const HdrSettings& hdr) const {
    return CalcEffectiveSceneWhiteNits(SceneWhite(), hdr);
  }

  // Kept for the skin-detect feature. Skin detection does not participate in
  // HDR exposure metering; it only consumes the current scene state.
  float FallbackExposure() const { return regular_.exposure; }
  float FallbackSceneWhite() const { return regular_.scene_white; }
  float FallbackEffectiveSceneWhite(const HdrSettings& hdr) const {
    return CalcEffectiveSceneWhite(SceneWhite(), hdr);
  }
  float FallbackEffectiveSceneWhiteNits(const HdrSettings& hdr) const {
    return CalcEffectiveSceneWhiteNits(SceneWhite(), hdr);
  }

  HdrSnapshot GetSnapshot() const;

 private:
  void PollReadback(ID3D12Fence* fence, const Settings& settings);
  void UpdateExposure(const uint32_t* bins, const Settings& settings);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12Resource> histogram_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0;
  uint64_t pending_fence_value_ = 0;
  bool pending_measurement_ = false;
  GpuReadback readback_;

  static constexpr size_t kGainStatsBytes = 4u * sizeof(uint32_t);
  uint64_t pending_gain_fence_value_ = 0;
  bool pending_gain_stats_ = false;
  GpuReadback gain_readback_;

  bool enabled_ = false;
  HdrFormat resolved_format_ = HdrFormat::Sdr;

  ExposureTracker regular_;
  uint32_t frame_counter_ = 0;
  uint32_t frames_until_measure_ = 0;
  uint32_t frames_since_measure_ = 0;
  uint64_t measurement_generation_ = 0;
  bool have_logged_format_ = false;

  bool have_histogram_ = false;
  uint32_t last_histogram_[256] = {};
  float p_hi_luma_ = 0.0f;
  uint32_t last_samples_ = 0;
  std::chrono::steady_clock::time_point last_measurement_time_{};
  bool have_measurement_time_ = false;

  bool have_gain_stats_ = false;
  float gain_min_log2_ = 0.0f;
  float gain_max_log2_ = 0.0f;
  uint32_t gain_sdr_clipped_pixels_ = 0;
  uint32_t gain_out_of_range_pixels_ = 0;
};

}  // namespace reshade_dlssnr
