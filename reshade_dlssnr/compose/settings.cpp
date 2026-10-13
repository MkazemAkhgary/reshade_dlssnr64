#include "compose/settings.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

void LoadResidual(const std::string& prefix, ResidualSettings& residual) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditStrength").c_str(),
                            residual.edit_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaStrength").c_str(),
                            residual.chroma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditGamma").c_str(),
                            residual.edit_gamma);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "HighlightProtect").c_str(),
                            residual.highlight_protect);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ShadowProtect").c_str(),
                            residual.shadow_protect);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "UpsampleSharpness").c_str(),
                            residual.upsample_sharpness);
  if (residual.upsample_sharpness < 0.5f) residual.upsample_sharpness = 0.5f;
  if (residual.upsample_sharpness > 32.0f) residual.upsample_sharpness = 32.0f;
  if (residual.edit_strength < 0.0f) residual.edit_strength = 0.0f;
  if (residual.edit_strength > 2.0f) residual.edit_strength = 2.0f;
  if (residual.chroma_strength < 0.0f) residual.chroma_strength = 0.0f;
  if (residual.chroma_strength > 1.0f) residual.chroma_strength = 1.0f;
  if (residual.edit_gamma < 0.25f) residual.edit_gamma = 0.25f;
  if (residual.edit_gamma > 4.0f) residual.edit_gamma = 4.0f;
  if (residual.highlight_protect < 0.0f) residual.highlight_protect = 0.0f;
  if (residual.highlight_protect > 1.0f) residual.highlight_protect = 1.0f;
  if (residual.shadow_protect < 0.0f) residual.shadow_protect = 0.0f;
  if (residual.shadow_protect > 1.0f) residual.shadow_protect = 1.0f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioEps").c_str(),
                            residual.ratio_eps);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioBrighten").c_str(),
                            residual.ratio_brighten);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioDarken").c_str(),
                            residual.ratio_darken);
  if (residual.ratio_eps < 1e-6f) residual.ratio_eps = 1e-6f;
  if (residual.ratio_eps > 0.5f) residual.ratio_eps = 0.5f;
  if (residual.ratio_brighten < 1.0f) residual.ratio_brighten = 1.0f;
  if (residual.ratio_brighten > 256.0f) residual.ratio_brighten = 256.0f;
  if (residual.ratio_darken > 1.0f) residual.ratio_darken = 1.0f;
  if (residual.ratio_darken < 1.0f / 256.0f)
    residual.ratio_darken = 1.0f / 256.0f;

}

void SaveResidual(const std::string& prefix, const ResidualSettings& residual) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditStrength").c_str(),
                            residual.edit_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaStrength").c_str(),
                            residual.chroma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditGamma").c_str(),
                            residual.edit_gamma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "HighlightProtect").c_str(),
                            residual.highlight_protect);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ShadowProtect").c_str(),
                            residual.shadow_protect);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "UpsampleSharpness").c_str(),
                            residual.upsample_sharpness);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioEps").c_str(),
                            residual.ratio_eps);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioBrighten").c_str(),
                            residual.ratio_brighten);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioDarken").c_str(),
                            residual.ratio_darken);
}

void LoadStrength(const std::string& prefix, StrengthSettings& strength) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "LumaStrength").c_str(),
                            strength.luma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaStrength").c_str(),
                            strength.chroma_strength);
  if (strength.luma_strength < 0.0f) strength.luma_strength = 0.0f;
  if (strength.luma_strength > 1.0f) strength.luma_strength = 1.0f;
  if (strength.chroma_strength < 0.0f) strength.chroma_strength = 0.0f;
  if (strength.chroma_strength > 1.0f) strength.chroma_strength = 1.0f;
}

void SaveStrength(const std::string& prefix, const StrengthSettings& strength) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "LumaStrength").c_str(),
                            strength.luma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaStrength").c_str(),
                            strength.chroma_strength);
}

void LoadSoftCompression(const std::string& prefix,
                         SoftCompressionSettings& soft_compression) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftCompression").c_str(),
                            soft_compression.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftMaxBrighten").c_str(),
                            soft_compression.max_brighten);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftMaxDarken").c_str(),
                            soft_compression.max_darken);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftMaxChroma").c_str(),
                            soft_compression.max_chroma);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftKnee").c_str(),
                            soft_compression.knee);
  if (soft_compression.max_brighten < 0.0f) soft_compression.max_brighten = 0.0f;
  if (soft_compression.max_brighten > 2.0f) soft_compression.max_brighten = 2.0f;
  if (soft_compression.max_darken < 0.0f) soft_compression.max_darken = 0.0f;
  if (soft_compression.max_darken > 2.0f) soft_compression.max_darken = 2.0f;
  if (soft_compression.max_chroma < 0.0f) soft_compression.max_chroma = 0.0f;
  if (soft_compression.max_chroma > 2.0f) soft_compression.max_chroma = 2.0f;
  if (soft_compression.knee < 0.0f) soft_compression.knee = 0.0f;
  if (soft_compression.knee > 4.0f) soft_compression.knee = 4.0f;
}

void SaveSoftCompression(const std::string& prefix,
                         const SoftCompressionSettings& soft_compression) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftCompression").c_str(),
                            soft_compression.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftMaxBrighten").c_str(),
                            soft_compression.max_brighten);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftMaxDarken").c_str(),
                            soft_compression.max_darken);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftMaxChroma").c_str(),
                            soft_compression.max_chroma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "SoftKnee").c_str(),
                            soft_compression.knee);
}

}  // namespace reshade_dlssnr
