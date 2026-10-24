// Pass view implementation. See view.h.

#include "feature/pass/view.h"
#include "profiling/profile.h"

#include "ctl/values.h"
#include "feature/pass/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace pass {

PassSnapshot Read(const ctl::ValueStore& store, const ctl::InstanceKey& key) {
  DLSSNR_PROFILE_CTL_L3("Read");
  const std::string section = kSection;
  PassSnapshot p;

  p.enabled = store.Read(section, key, kPassEnabled).as_bool;
  p.target = store.Read(section, key, kTarget).as_int;
  p.custom_name = store.Read(section, key, kCustomName).as_string;

  p.preset = store.Read(section, key, kPreset).as_int;
  p.working_scale = store.Read(section, key, kWorkingScale).as_float;
  p.intensity = store.Read(section, key, kIntensity).as_float;
  p.temporal_mode = store.Read(section, key, kTemporalMode).as_int;
  p.synthetic_motion_guides =
      store.Read(section, key, kSyntheticMotionGuides).as_bool;
  p.temporal_photometric_abs_threshold =
      store.Read(section, key, kTemporalPhotometricAbsThreshold).as_float;
  p.temporal_photometric_rel_threshold =
      store.Read(section, key, kTemporalPhotometricRelThreshold).as_float;
  p.temporal_depth_abs_threshold =
      store.Read(section, key, kTemporalDepthAbsThreshold).as_float;
  p.temporal_depth_rel_threshold =
      store.Read(section, key, kTemporalDepthRelThreshold).as_float;
  p.temporal_max_displacement =
      store.Read(section, key, kTemporalMaxDisplacement).as_float;
  p.temporal_max_invalid_percent =
      store.Read(section, key, kTemporalMaxInvalidPercent).as_float;
  p.temporal_fp16_history = store.Read(section, key, kTemporalFp16History).as_bool;
  p.local_tone = store.Read(section, key, kLocalTone).as_float;
  p.local_structure = store.Read(section, key, kLocalStructure).as_float;
  p.skin_structure = store.Read(section, key, kSkinStructure).as_float;
  p.auto_mask = store.Read(section, key, kAutoMask).as_bool;
  p.ui_correction = store.Read(section, key, kUiCorrection).as_bool;

  p.strength.luma_strength =
      store.Read(section, key, kLumaStrength).as_float;
  p.strength.chroma_strength =
      store.Read(section, key, kChromaStrength).as_float;

  p.residual.edit_strength =
      store.Read(section, key, kEditStrength).as_float;
  p.residual.chroma_strength =
      store.Read(section, key, kResidualChroma).as_float;
  p.residual.edit_gamma = store.Read(section, key, kResidualGamma).as_float;
  p.residual.highlight_protect =
      store.Read(section, key, kResidualHighlight).as_float;
  p.residual.shadow_protect =
      store.Read(section, key, kResidualShadow).as_float;
  p.residual.upsample_sharpness =
      store.Read(section, key, kUpsampleSharpness).as_float;
  p.residual.ratio_eps = store.Read(section, key, kRatioEps).as_float;
  p.residual.ratio_brighten =
      store.Read(section, key, kRatioBrighten).as_float;
  p.residual.ratio_darken = store.Read(section, key, kRatioDarken).as_float;
  p.residual.ratio_clamps_enabled =
      store.Read(section, key, kRatioClampsEnabled).as_bool;

  return p;
}

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr
