#include "skin/settings.h"

#include <algorithm>

#include <reshade.hpp>

namespace reshade_dlssnr {

void LoadSkinSettings(SkinSettings& skin) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Enable", skin.enable);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Scale", skin.scale);
  int target = static_cast<int>(skin.target_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Target", target);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "StructureDetect.Target", target);
  if (target < 0 || target > 1) target = 0;
  skin.target_mode = static_cast<DetectionTargetMode>(target);
  skin.scale = std::clamp(skin.scale, 0.10f, 1.0f);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LumaThreshold", skin.luma_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.ChromaThreshold", skin.chroma_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LumaRelative", skin.luma_relative);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.ClipThreshold", skin.clip_threshold);
  skin.luma_threshold = std::max(skin.luma_threshold, 0.0f);
  skin.chroma_threshold = std::max(skin.chroma_threshold, 0.0f);
  skin.luma_relative = std::clamp(skin.luma_relative, 0.0f, 1.0f);
  skin.clip_threshold = std::clamp(skin.clip_threshold, 0.5f, 1.0f);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Preset", skin.preset);
  skin.preset = std::clamp(skin.preset, 0, 2);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Intensity", skin.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LocalTone", skin.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LocalStructure", skin.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.SkinStructure", skin.skin_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.UICorrection", skin.ui_correction);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.EditStrength", skin.edit_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.MaskSmoothing", skin.mask_smoothing);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.MaskResponse", skin.mask_response);
  skin.edit_strength = std::clamp(skin.edit_strength, 0.1f, 8.0f);
  skin.mask_smoothing = std::clamp(skin.mask_smoothing, 0.0f, 1.0f);
  skin.mask_response = std::clamp(skin.mask_response, 0.0f, 1.0f);
}

void SaveSkinSettings(const SkinSettings& skin) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Enable", skin.enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Scale", skin.scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "StructureDetect.Target", static_cast<int>(skin.target_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LumaThreshold", skin.luma_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.ChromaThreshold", skin.chroma_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LumaRelative", skin.luma_relative);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.ClipThreshold", skin.clip_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Preset", skin.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Intensity", skin.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LocalTone", skin.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LocalStructure", skin.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.SkinStructure", skin.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.UICorrection", skin.ui_correction);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.EditStrength", skin.edit_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.MaskSmoothing", skin.mask_smoothing);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.MaskResponse", skin.mask_response);
}

}  // namespace reshade_dlssnr
