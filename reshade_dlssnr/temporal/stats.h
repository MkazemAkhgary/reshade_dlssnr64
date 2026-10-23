// Live temporal-reprojection readout: the temporal stage publishes one snapshot
// per frame and the overlay reads it. Last writer wins, so with several passes
// the panel shows the most recently executed one.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {
namespace temporal_stats {

// GPU stats buffer size; the layout is defined by validate_depth.hlsl.
inline constexpr unsigned int kBytes = 8u * sizeof(unsigned int);

struct Snapshot {
  bool active = false;          // temporal ran on the last frame
  bool reuse = false;           // NGX was skipped
  bool feedback_valid = false;  // invalid fraction came from the GPU
  bool cadence_short = false;   // reuse window was halved
  bool gpu_valid = false;       // mean_conf/mean_edit came from the GPU

  uint64_t age = 0;             // frames since the last NGX evaluation
  uint64_t cadence = 0;         // effective reuse window
  float invalid_fraction = 1.0f;

  // GPU means over the last completed sample.
  float mean_conf = 0.0f;
  float mean_edit = 0.0f;       // mean |log gain| of the reprojected edit

  // Frame interval. reuse_ms vs refresh_ms is the judder amplitude: the
  // reuse window only helps if the two are close, and the panel is the only
  // place that difference is visible while playing.
  float dt_ms = 0.0f;
  float dt_reuse_ms = 0.0f;
  float dt_refresh_ms = 0.0f;
  uint64_t reuse_frames = 0;
  uint64_t refresh_frames = 0;
};

inline Snapshot& Mutable() {
  static Snapshot snapshot;
  return snapshot;
}

inline const Snapshot& Get() { return Mutable(); }

}  // namespace temporal_stats
}  // namespace reshade_dlssnr
