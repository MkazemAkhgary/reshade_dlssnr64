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

// Read-only snapshot of the current HDR measurement state, safe to copy to
// the UI thread.
struct HdrSnapshot {
  bool enabled = false;
  bool ready = false;             // has a valid measurement to report
  HdrFormat resolved_format = HdrFormat::Sdr;
  float exposure = 1.0f;
  float white_point = 1.0f;
  uint32_t frames_since_measure = 0;  // frames since last histogram arrived
};

class HdrState {
 public:
  bool Ensure(ID3D12Device* device);
  void Shutdown();

  // Called by AddonController before the Post-DLSS pass loop. `source`
  // is the image to measure (typically the captured DLSS output clone).
  // `codec` is any ComputeEngine; the histogram passes are dispatched on
  // `command_list`, which is the game's list.
  void BeginFrame(CommandContext& cmd,
                  ID3D12GraphicsCommandList* command_list,
                  ID3D12Resource* source,
                  unsigned int width,
                  unsigned int height,
                  const Settings& settings,
                  ComputeEngine& codec);

  // Called by AddonController at Present, with the game's queue. Signals
  // the internal fence so the pending measurement can be validated on the
  // next frame.
  void SignalAtPresent(ID3D12CommandQueue* queue);

  // Accessors used by the pass session.
  bool Enabled() const { return enabled_; }
  HdrFormat ResolvedFormat() const { return resolved_format_; }
  float Exposure() const { return exposure_ema_; }
  float WhitePoint() const { return white_point_ema_; }

  HdrSnapshot GetSnapshot() const;

 private:
  void PollReadback(ID3D12Fence* fence, const Settings& settings);
  void UpdateExposure(const uint32_t* bins, uint32_t total,
                      const Settings& settings);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12Resource> histogram_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0;
  uint64_t pending_fence_value_ = 0;
  bool pending_measurement_ = false;
  GpuReadback readback_;

  bool enabled_ = false;
  HdrFormat resolved_format_ = HdrFormat::Sdr;

  float exposure_ema_ = 1.0f;
  float white_point_ema_ = 1.0f;
  bool have_measurement_ = false;
  uint32_t frame_counter_ = 0;
  uint32_t frames_until_measure_ = 0;
  uint32_t warmup_frames_ = 0;
  uint32_t frames_since_log_ = 0;
  uint32_t frames_since_measure_ = 0;
  bool prev_enabled_ = false;
  bool have_logged_format_ = false;
};

}  // namespace reshade_dlssnr
