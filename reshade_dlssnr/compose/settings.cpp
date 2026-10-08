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

  int hdr_compose_mode = static_cast<int>(residual.hdr_compose_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "HdrComposeMode").c_str(),
                            hdr_compose_mode);
  if (hdr_compose_mode == 2) hdr_compose_mode = 1;
  if (hdr_compose_mode < 0 || hdr_compose_mode > 1) hdr_compose_mode = 0;
  residual.hdr_compose_mode = static_cast<HdrComposeMode>(hdr_compose_mode);
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
                            (prefix + "RatioEps").c_str(),
                            residual.ratio_eps);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioBrighten").c_str(),
                            residual.ratio_brighten);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioDarken").c_str(),
                            residual.ratio_darken);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "HdrComposeMode").c_str(),
                            static_cast<int>(residual.hdr_compose_mode));
}

}  // namespace reshade_dlssnr
