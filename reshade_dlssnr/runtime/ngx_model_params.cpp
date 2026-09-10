#include "ngx_model_params.h"

#include <algorithm>

namespace reshade_dlssnr {

void ApplyModelTuningParams(NgxParameterBlock& block, const PassSettings& pass) {
  const unsigned int model_value =
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2));

  block.SetUInt("DLSSNR.Enabled", 1u);
  block.SetUInt("DLSSNR.Upscaling", 1u);
  block.SetFloat("DLSSNR.ScalingRatio", 1.0f);
  block.SetFloat("DLSSNR.Scale", 1.0f);
  block.SetUInt("DLSSNR.Hint.Render.Preset", model_value + 1u);
  block.SetUInt("DLSSNR.Style", model_value);
  block.SetFloat("DLSSNR.Intensity", pass.intensity);
  block.SetFloat("DLSSNR.LocalStructureStrength", pass.local_structure);
  block.SetFloat("DLSSNR.LocalToneStrength", pass.local_tone);
  block.SetFloat("DLSSNR.SkinStructureStrength",
                 pass.auto_mask ? pass.skin_structure : -1.0f);
  block.SetUInt("DLSSNR.UseAutoMask", pass.auto_mask ? 1u : 0u);
  block.SetUInt("DLSSNR.UICorrection", pass.ui_correction ? 1u : 0u);
}

}  // namespace reshade_dlssnr
