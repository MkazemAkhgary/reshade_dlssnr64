// HDR view implementation. See view.h.

#include "feature/hdr/view.h"

#include "ctl/values.h"
#include "feature/hdr/settings.h"
#include "fragments/hdr_filter_rows/rows.h"

namespace reshade_dlssnr {
namespace feature {
namespace hdr {

namespace {

// Read the filter rows shared by every layer. Each layer is its own section,
// so the names are bare and cannot collide.
ExposureFilterSettings ReadFilters(const ctl::ValueStore& store,
                                   const ctl::InstanceKey& key,
                                   const std::string& section) {
  using namespace hdr_filter_rows::names;
  ExposureFilterSettings f;
  f.importance_mask = store.Read(section, key, kImportanceMask).as_bool;
  f.depth = store.Read(section, key, kDepth).as_bool;
  f.percentile_range = store.Read(section, key, kPercentileRange).as_bool;
  f.depth_near = store.Read(section, key, kDepthNear).as_float;
  f.depth_far = store.Read(section, key, kDepthFar).as_float;
  f.percentile_low = store.Read(section, key, kPercentileLow).as_float;
  f.percentile_high = store.Read(section, key, kPercentileHigh).as_float;
  return f;
}

}  // namespace

HdrSnapshotView Read(const ctl::ValueStore& store,
                     const ctl::InstanceKey& key) {
  const std::string hdr = kSection;
  HdrSnapshotView s;

  s.enable = store.Read(hdr, key, kEnable).as_bool;
  s.format = static_cast<HdrFormat>(store.Read(hdr, key, kFormat).as_int);
  s.measure_interval = store.Read(hdr, key, kMeasureInterval).as_int;
  s.highlight_headroom = store.Read(hdr, key, kHighlightHeadroom).as_float;
  s.detailed_statistics = store.Read(hdr, key, kDetailedStatistics).as_bool;
  s.faster_metering = store.Read(hdr, key, kFasterMetering).as_bool;
  s.bt2446_k1 = store.Read(hdr, key, kBt2446K1).as_float;
  s.bt2446_k2 = store.Read(hdr, key, kBt2446K2).as_float;
  s.bt2446_k3 = store.Read(hdr, key, kBt2446K3).as_float;
  s.bt2446_k4 = store.Read(hdr, key, kBt2446K4).as_float;
  s.bt2446_crosstalk = store.Read(hdr, key, kBt2446Crosstalk).as_float;
  s.gain_min_log2 = store.Read(hdr, key, kGainMinLog2).as_float;
  s.gain_max_log2 = store.Read(hdr, key, kGainMaxLog2).as_float;

  // Game layer.
  {
    const std::string sec = kGameSection;
    GameExposureLayerSettings& g = s.game_exposure;
    g.enabled = store.Read(sec, key, kGameEnabled).as_bool;
    g.bias_stops = store.Read(sec, key, kGameBiasStops).as_float;
  }

  // Adaptive layer.
  {
    const std::string sec = kAdaptiveSection;
    AdaptiveExposureLayerSettings& a = s.adaptive_exposure;
    a.enabled = store.Read(sec, key, kAdaptiveEnabled).as_bool;
    a.midtone = store.Read(sec, key, kAdaptiveMidtone).as_float;
    a.speed_up = store.Read(sec, key, kAdaptiveSpeedUp).as_float;
    a.speed_down = store.Read(sec, key, kAdaptiveSpeedDown).as_float;
    a.strength_over_game =
        store.Read(sec, key, kAdaptiveStrengthOverGame).as_float;
    a.max_correction_stops =
        store.Read(sec, key, kAdaptiveMaxCorrection).as_float;
    a.filters = ReadFilters(store, key, sec);
  }

  // Local layer.
  {
    const std::string sec = kLocalSection;
    LocalExposureLayerSettings& l = s.local_exposure;
    l.enabled = store.Read(sec, key, kLocalEnabled).as_bool;
    l.strength = store.Read(sec, key, kLocalStrength).as_float;
    l.max_brighten = store.Read(sec, key, kLocalMaxBrighten).as_float;
    l.max_darken = store.Read(sec, key, kLocalMaxDarken).as_float;
    l.midtone = store.Read(sec, key, kLocalMidtone).as_float;
    l.precomputed_luma = store.Read(sec, key, kLocalPrecomputedLuma).as_bool;
    l.fp16_reference = store.Read(sec, key, kLocalFp16Reference).as_bool;
    l.restore_global_exposure =
        store.Read(sec, key, kLocalRestoreGlobalExposure).as_float;
  }

  return s;
}

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr
