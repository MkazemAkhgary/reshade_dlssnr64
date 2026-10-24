// Exposure Mask view implementation. See view.h.

#include "feature/exposure_mask/view.h"
#include "profiling/profile.h"

#include "ctl/values.h"
#include "feature/exposure_mask/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace exposure_mask {

ExposureMaskSnapshot Read(const ctl::ValueStore& store,
                            const ctl::InstanceKey& key) {
  DLSSNR_PROFILE_CTL_L3("Read");
  const std::string sec = kSection;
  ExposureMaskSnapshot s;
  s.enable = store.Read(sec, key, kEnable).as_bool;
  s.edit_strength = store.Read(sec, key, kEditStrength).as_float;
  s.mask_smoothing = store.Read(sec, key, kSmoothing).as_float;
  s.mask_response = store.Read(sec, key, kResponse).as_float;
  s.luma_threshold = store.Read(sec, key, kLumaThreshold).as_float;
  s.luma_relative = store.Read(sec, key, kLumaRelative).as_float;
  s.chroma_threshold = store.Read(sec, key, kChromaThreshold).as_float;
  return s;
}

}  // namespace exposure_mask
}  // namespace feature
}  // namespace reshade_dlssnr
