// HDR measurement state. Owns the histogram buffer, the readback ring,
// and the exposure / white point EMA. Only the Post-DLSS path uses this.
//
// Lifecycle within a frame (Post-DLSS injection):
//   1. AddonController calls BeginFrame before the pass loop. This may
//      enqueue a histogram measurement onto the game's command list.
//   2. AddonController calls SignalAtPresent with the game's queue.
//      This signals our internal fence so we know when the measurement
//      has completed.
//   3. On the next BeginFrame, we poll the fence and, if the copy is
//      ready, update the exposure EMA.
//
// See HDR_DESIGN.md for the pipeline this feeds into.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "command_context.h"
#include "compute_engine.h"
#include "gpu_readback.h"
#include "settings.h"

namespace reshade_dlssnr {

// Which histogram the exposure median was drawn from.
enum class MeteringSource {
  FullFrame = 0,
  DepthFiltered = 1,
  Skin = 2,
};

// One exposure/white-point EMA driven from a histogram. HdrState holds two:
// one for regular passes (skin-driven when a mask is available) and one for
// the skin-detect pass (depth -> full fallback only, no skin). Splitting them
// breaks the loop where the mask drives the skin histogram, which drives the
// exposure, which changes the tonemap that produced the mask.
struct ExposureTracker {
  float exposure = 1.0f;
  float white_point = 1.0f;
  float target_exposure = 1.0f;
  float p50_luma = 0.0f;
  float skin_fraction = 0.0f;
  float depth_filter_survived = 0.0f;
  uint32_t warmup_frames = 0;
  bool have_measurement = false;
  bool target_is_skin = false;
  MeteringSource metering_source = MeteringSource::FullFrame;

  void Reset();
  void Update(const uint32_t* bins, const Settings& settings, bool use_skin);
};

// Read-only snapshot of the current HDR measurement state, safe to copy to
// the UI thread.
struct HdrSnapshot {
  bool enabled = false;
  bool ready = false;             // has a valid measurement to report
  HdrFormat resolved_format = HdrFormat::Sdr;
  float exposure = 1.0f;
  float white_point = 1.0f;
  uint32_t frames_since_measure = 0;  // frames since last histogram arrived

  // Raw measurement, for the diagnostics UI. Populated whenever a fresh
  // histogram has been consumed. `has_histogram` is false until then.
  bool has_histogram = false;
  uint32_t histogram[256] = {};       // full frame
  uint32_t histogram_skin[256] = {};  // skin, depth-filtered
  float p50_luma = 0.0f;         // scene median, before exposure
  float p_hi_luma = 0.0f;        // white-point percentile, before exposure
  float target_exposure = 1.0f;  // value the EMA is chasing
  uint32_t samples = 0;          // total pixels sampled in the last histogram

  // Which histogram the exposure median was taken from, and how much of the
  // frame was skin. Diagnostics only.
  float skin_fraction = 0.0f;
  bool target_is_skin = false;
  // Fraction of the frame the depth filter kept. Zero when there is no
  // depth guide; the histogram is the full frame in that case.
  float depth_filter_survived = 0.0f;
  int metering_source = 0;  // MeteringSource, as an int for the UI

  // Fallback EMA, used by the skin-detect pass. Depth -> full only, no skin
  // slice, so it does not depend on the mask.
  float fallback_exposure = 1.0f;
  float fallback_white_point = 1.0f;
  int fallback_metering_source = 0;
};

// Maps a DXGI format to the HdrFormat the shaders should treat it as. The
// single definition; every format detection in the addon routes through it.
HdrFormat DetectFormatFromDxgi(DXGI_FORMAT format);

class HdrState {
 public:
  bool Ensure(ID3D12Device* device);
  void Shutdown();

  // Called by AddonController before the Post-DLSS pass loop. `source`
  // is the image to measure (typically the captured DLSS output clone).
  // `codec` is any ComputeEngine; the histogram passes are dispatched on
  // `command_list`, which is the game's list.
  // `depth` may be null when the game does not provide a depth guide; the
  // histogram then falls back to the full frame. `depth_inverted` is only
  // meaningful when `depth` is non-null.
  //
  // `mask` may be null; when non-null it is an R8 skin mask at detect scale
  // and replaces the shader's hue classifier for the skin histogram.
  void BeginFrame(CommandContext& cmd,
                  ID3D12GraphicsCommandList* command_list,
                  ID3D12Resource* source,
                  unsigned int width,
                  unsigned int height,
                  ID3D12Resource* depth,
                  bool depth_inverted,
                  ID3D12Resource* mask,
                  const Settings& settings,
                  ComputeEngine& codec);

  // Called by AddonController at Present, with the game's queue. Signals
  // the internal fence so the pending measurement can be validated on the
  // next frame.
  void SignalAtPresent(ID3D12CommandQueue* queue);

  // Accessors used by the pass session.
  bool Enabled() const { return enabled_; }
  HdrFormat ResolvedFormat() const { return resolved_format_; }
  // Regular passes: skin-driven when a mask is available.
  float Exposure() const { return regular_.exposure; }
  float WhitePoint() const { return regular_.white_point; }
  // Skin-detect pass: depth -> full only, no skin slice.
  float FallbackExposure() const { return fallback_.exposure; }
  float FallbackWhitePoint() const { return fallback_.white_point; }

  HdrSnapshot GetSnapshot() const;

 private:
  void PollReadback(ID3D12Fence* fence, const Settings& settings);
  // `bins` is the packed 768-entry buffer: full, depth-filtered, skin.
  void UpdateExposure(const uint32_t* bins, const Settings& settings);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12Resource> histogram_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0;
  uint64_t pending_fence_value_ = 0;
  bool pending_measurement_ = false;
  GpuReadback readback_;

  bool enabled_ = false;
  HdrFormat resolved_format_ = HdrFormat::Sdr;

  ExposureTracker regular_;
  ExposureTracker fallback_;
  uint32_t frame_counter_ = 0;
  uint32_t frames_until_measure_ = 0;
  uint32_t frames_since_measure_ = 0;
  bool have_logged_format_ = false;

  // Diagnostics: last measured histogram and the shared white-point
  // percentile (identical for both trackers).
  bool have_histogram_ = false;
  uint32_t last_histogram_[256] = {};
  uint32_t last_histogram_skin_[256] = {};
  float p_hi_luma_ = 0.0f;
  uint32_t last_samples_ = 0;
};

}  // namespace reshade_dlssnr
