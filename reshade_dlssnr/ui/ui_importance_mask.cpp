#include <imgui.h>
#include <reshade.hpp>

#include "state/settings.h"
#include "importance_mask/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

namespace {

void ResetImportanceMask(void* context) {
  *static_cast<ImportanceMaskSettings*>(context) = ImportanceMaskSettings{};
}

}  // namespace

void DrawImportanceMaskSection(Settings& settings, bool hdr_relevant,
                               bool& changed) {
  ImportanceMaskSettings& mask = settings.importance_mask;
  const bool available = hdr_relevant && settings.hdr.enable;
  if (!PersistentHeader(
          "Importance Mask", settings.ui_importance_mask_open, changed,
          "Build a mask from the first Pre/Post-DLSS pass by comparing the actual model input, including input grading and Local Exposure, with its raw model output. The result is used for exposure on the next frame.",
          HeaderControls{&mask.enable,
                         "The first Pre/Post-DLSS pass builds the mask after model evaluation. It never affects exposure or later passes until the next frame.",
                         !available, ResetImportanceMask, &mask})) {
    return;
  }

  IndentScope section_indent;
  if (!available) {
    ImGui::TextDisabled("Requires HDR and an enabled Pre-DLSS or Post-DLSS pass.");
    return;
  }

  WithDisabled(!mask.enable, [&] {
    changed |= DrawFloatRow(0, "importance_mask_edit_strength", "Edit Strength",
                            "Scale the measured model-input/output difference before thresholding.",
                            &mask.edit_strength, nullptr, 1.0f, 0.1f, 8.0f, 1.0f);
    changed |= DrawFloatRow(0, "importance_mask_smoothing", "Spatial Smoothing",
                            "Majority-filter the binary importance votes in a small neighborhood.",
                            &mask.mask_smoothing, nullptr, 0.5f, 0.0f, 1.0f, 0.5f);
    changed |= DrawFloatRow(0, "importance_mask_response", "Temporal Response",
                            "Blend new mask votes with the previous frame's mask.",
                            &mask.mask_response, nullptr, 0.15f, 0.0f, 1.0f, 0.15f);
    changed |= DrawFloatRow(0, "importance_mask_luma_threshold", "Luma Threshold",
                            "Minimum absolute luma difference to mark a model edit.",
                            &mask.luma_threshold, nullptr, 0.02f, 0.0f, 0.5f, 0.02f);
    changed |= DrawFloatRow(0, "importance_mask_luma_relative", "Relative Luma",
                            "Minimum luma difference relative to the brighter input/output luma.",
                            &mask.luma_relative, nullptr, 0.05f, 0.0f, 1.0f, 0.05f);
    changed |= DrawFloatRow(0, "importance_mask_chroma_threshold", "Chroma Threshold",
                            "Minimum chroma-only difference to mark a model edit.",
                            &mask.chroma_threshold, nullptr, 0.02f, 0.0f, 0.5f, 0.02f);
  });
}

}  // namespace reshade_dlssnr
