// HDR exposure-layer state and asynchronous GPU measurements.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>

#include "command_context.h"
#include "compute_engine.h"
#include "gpu_readback.h"
#include "hdr/settings.h"

namespace reshade_dlssnr {

enum class MeteringSource {
  FullFrame = 0,
  FilteredPopulation = 1,
  GameExposureSource = 2,  // the game's 1x1 NGX exposure, not a histogram band
};

struct ExposureParams {
  float midtone = 0.18f;
  float percentile_low = 0.0f;
  float percentile_high = 1.0f;
  float speed_up = 3.0f;
  float speed_down = 1.0f;
  bool use_percentile_range = false;
};

struct ExposureTracker {
  float exposure = 1.0f;
  float scene_white = 1.0f;
  float exposure_reference_luma = 0.0f;
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
  float adaptive_scene_white = 0.0f;
  uint32_t samples = 0;

  bool game_exposure_available = false;
  float game_exposure = 1.0f;
  uint32_t game_exposure_width = 0;
  uint32_t game_exposure_height = 0;
  int game_exposure_format = static_cast<int>(DXGI_FORMAT_UNKNOWN);

  uint64_t measurement_generation = 0;
  int metering_source = 0;
  bool gain_map_ready = false;
  float gain_min_log2 = 0.0f;
  float gain_max_log2 = 0.0f;
  uint32_t gain_sdr_excursion_pixels = 0;
  uint32_t gain_out_of_range_pixels = 0;
};

HdrFormat DetectFormatFromDxgi(DXGI_FORMAT format);

class HdrState {
 public:
  bool Ensure(ID3D12Device* device);
  void Shutdown();
  void ReuseMeasurementFrom(const HdrState& previous,
                            const HdrSettings& settings);

  void BeginFrame(CommandContext& cmd,
                  ID3D12GraphicsCommandList* command_list,
                  ID3D12Resource* source,
                  unsigned int width,
                  unsigned int height,
                  const HdrSettings& hdr_settings,
                  ComputeEngine& codec,
                  ID3D12Resource* exposure_mask = nullptr,
                  ID3D12Resource* game_exposure_texture = nullptr,
                  ID3D12Resource* depth_texture = nullptr,
                  bool depth_inverted = false,
                  unsigned int exposure_mask_width = 0,
                  unsigned int exposure_mask_height = 0);

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

  HdrSnapshot GetSnapshot() const;

 private:
  void PollReadback(ID3D12Fence* fence, const HdrSettings& settings);
  void UpdateExposure(const uint32_t* full_bins,
                      const uint32_t* adaptive_bins,
                      const HdrSettings& settings);
  void ResolveExposureStack(const HdrSettings& settings);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12Resource> histogram_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;

  Microsoft::WRL::ComPtr<ID3D12Resource> game_exposure_value_buffer_;
  GpuReadback game_exposure_readback_;
  uint64_t pending_game_exposure_fence_value_ = 0;
  bool pending_game_exposure_ = false;
  float game_exposure_value_ = 1.0f;
  bool game_exposure_available_ = false;
  uint32_t game_exposure_width_ = 0;
  uint32_t game_exposure_height_ = 0;
  DXGI_FORMAT game_exposure_format_ = DXGI_FORMAT_UNKNOWN;
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
  ExposureTracker adaptive_;
  uint32_t frames_until_measure_ = 0;
  uint32_t frames_since_measure_ = 0;
  uint64_t measurement_generation_ = 0;
  bool have_logged_format_ = false;

  bool have_histogram_ = false;
  uint32_t last_histogram_[256] = {};
  uint32_t last_samples_ = 0;
  std::chrono::steady_clock::time_point last_measurement_time_{};
  bool have_measurement_time_ = false;

  bool have_gain_stats_ = false;
  float gain_min_log2_ = 0.0f;
  float gain_max_log2_ = 0.0f;
  uint32_t gain_sdr_excursion_pixels_ = 0;
  uint32_t gain_out_of_range_pixels_ = 0;
};

}  // namespace reshade_dlssnr
