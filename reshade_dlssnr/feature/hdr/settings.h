// HDR section and setting names.
//
// A leaf header: names only, no includes, so anything that needs to read an
// HDR setting -- the feature's own schema, view and snapshot -- agrees on the
// spelling without pulling in the runtime's HdrSettings struct. The section
// names live here too, so the feature that owns a section is the only place
// its spelling appears.
//
// The layer sections carry their own prefixes: three independently collapsible
// exposure layers would otherwise collide on "enabled" and "midtone".

#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace hdr {

// Section names.
inline constexpr const char* kSection = "HDR";
inline constexpr const char* kGameSection = "HDR Game Exposure";
inline constexpr const char* kAdaptiveSection = "HDR Adaptive Exposure";
inline constexpr const char* kLocalSection = "HDR Local Exposure";

// Section "HDR".
inline constexpr const char* kEnable = "enable";
inline constexpr const char* kFormat = "format";
inline constexpr const char* kMeasureInterval = "measure_interval";
inline constexpr const char* kHighlightHeadroom = "highlight_headroom";
inline constexpr const char* kBt2446K1 = "bt2446_k1";
inline constexpr const char* kBt2446K3 = "bt2446_k3";
inline constexpr const char* kBt2446Ip = "bt2446_ip";
inline constexpr const char* kBt2446Crosstalk = "bt2446_crosstalk";
inline constexpr const char* kGainMinLog2 = "gain_min_log2";
inline constexpr const char* kGainMaxLog2 = "gain_max_log2";
inline constexpr const char* kDetailedStatistics = "detailed_statistics";
inline constexpr const char* kFasterMetering = "faster_metering";
inline constexpr const char* kReusePreviousPassMeasurement = "reuse_previous_pass_measurement";

// Section "HDR Game Exposure".
inline constexpr const char* kGameEnabled = "enabled";
inline constexpr const char* kGameBiasStops = "bias_stops";

// Section "HDR Adaptive Exposure".
inline constexpr const char* kAdaptiveEnabled = "enabled";
inline constexpr const char* kAdaptiveMidtone = "midtone";
inline constexpr const char* kAdaptiveSpeedUp = "speed_up";
inline constexpr const char* kAdaptiveSpeedDown = "speed_down";
inline constexpr const char* kAdaptiveStrengthOverGame = "strength_over_game";
inline constexpr const char* kAdaptiveMaxCorrection = "max_correction";

// Section "HDR Local Exposure".
inline constexpr const char* kLocalEnabled = "enabled";
inline constexpr const char* kLocalStrength = "strength";
inline constexpr const char* kLocalMaxBrighten = "max_brighten";
inline constexpr const char* kLocalMaxDarken = "max_darken";
inline constexpr const char* kLocalMidtone = "midtone";
inline constexpr const char* kLocalShadowStrength = "shadow_strength";
inline constexpr const char* kLocalHighlightStrength = "highlight_strength";
inline constexpr const char* kLocalShadowRange = "shadow_range";
inline constexpr const char* kLocalHighlightRange = "highlight_range";
inline constexpr const char* kLocalHighlightProtection = "highlight_protection";
inline constexpr const char* kLocalShadowProtection = "shadow_protection";
inline constexpr const char* kLocalDetailRetention = "detail_retention";
inline constexpr const char* kLocalEdgeProtection = "edge_protection";
inline constexpr const char* kLocalRadius = "radius";
inline constexpr const char* kLocalSamples = "samples";
inline constexpr const char* kLocalRestoreEnabled = "restore_exposure";
inline constexpr const char* kLocalRestoreMode = "restore_mode";
inline constexpr const char* kLocalRestoreStrength = "restore_strength";

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr
