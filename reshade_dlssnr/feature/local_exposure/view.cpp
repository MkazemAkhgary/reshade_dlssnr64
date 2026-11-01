#include "feature/local_exposure/view.h"
#include "profiling/profile.h"

#include <string>

#include "ctl/values.h"
#include "feature/local_exposure/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace local_exposure {

LocalExposureSettings Read(const ctl::ValueStore& store,
                           const ctl::InstanceKey& key) {
  DLSSNR_PROFILE_CTL_L3("ReadLocalExposure");
  const std::string sec = kSection;
  LocalExposureSettings s;
  s.enabled = store.Read(sec, key, kEnabled).as_bool;
  s.strength = store.Read(sec, key, kStrength).as_float;
  s.max_brighten = store.Read(sec, key, kMaxBrighten).as_float;
  s.max_darken = store.Read(sec, key, kMaxDarken).as_float;
  s.midtone = store.Read(sec, key, kMidtone).as_float;
  s.shadow_strength = store.Read(sec, key, kShadowStrength).as_float;
  s.highlight_strength = store.Read(sec, key, kHighlightStrength).as_float;
  s.shadow_range = store.Read(sec, key, kShadowRange).as_float;
  s.highlight_range = store.Read(sec, key, kHighlightRange).as_float;
  s.highlight_protection = store.Read(sec, key, kHighlightProtection).as_float;
  s.shadow_protection = store.Read(sec, key, kShadowProtection).as_float;
  s.detail_retention = store.Read(sec, key, kDetailRetention).as_float;
  s.edge_protection = store.Read(sec, key, kEdgeProtection).as_float;
  s.radius_px = store.Read(sec, key, kRadius).as_float;
  s.samples = store.Read(sec, key, kSamples).as_int;
  s.restore_exposure = store.Read(sec, key, kRestoreEnabled).as_bool;
  s.restore_mode = store.Read(sec, key, kRestoreMode).as_float;
  s.restore_strength = store.Read(sec, key, kRestoreStrength).as_float;
  return s;
}

}  // namespace local_exposure
}  // namespace feature
}  // namespace reshade_dlssnr
