// Overlay text that more than one file draws.
//
// A translation entry is identified by namespace and key, so a string used in
// two places has to be declared once. These live in a header for that reason:
// the stage names and the run states appear in the pass tabs, the toolbar, and
// the status readout.

#pragma once

#include "localization/localization.h"

namespace reshade_dlssnr {
namespace ui_text {

inline const localization::TextRef kStagePreDlss =
    NSLOC_TEXT("ui", "stage.pre_dlss", "Pre-DLSS");
inline const localization::TextRef kStagePostDlss =
    NSLOC_TEXT("ui", "stage.post_dlss", "Post-DLSS");
inline const localization::TextRef kStageBackbuffer =
    NSLOC_TEXT("ui", "stage.backbuffer", "Backbuffer");

inline const localization::TextRef kStateEnabled =
    NSLOC_TEXT("ui", "state.enabled", "enabled");
inline const localization::TextRef kStateDisabled =
    NSLOC_TEXT("ui", "state.disabled", "disabled");
inline const localization::TextRef kStateFailed =
    NSLOC_TEXT("ui", "state.failed", "failed");
inline const localization::TextRef kStateOff =
    NSLOC_TEXT("ui", "state.off", "Off");

// The positional pass name, used by the tab bar and the status readout.
inline const localization::TextRef kPassLabel =
    NSLOC_TEXT("ui", "pass_label", "Pass %u");

}  // namespace ui_text
}  // namespace reshade_dlssnr
