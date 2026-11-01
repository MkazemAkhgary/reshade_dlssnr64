// HDR section and setting names.
//
// A leaf header: names only, no includes, so anything that needs to read an
// HDR setting -- the feature's own schema, view and snapshot -- agrees on the
// spelling without pulling in the runtime's HdrSettings struct. The section
// names live here too, so the feature that owns a section is the only place
// its spelling appears.
//
// The layer sections carry their own prefixes: two independently collapsible
// exposure layers would otherwise collide on "enabled" and "midtone".
//
// Local Exposure is not here. It is a section of its own
// (feature/local_exposure/settings.h) because it needs no HDR input.

#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace hdr {

// Section names.
inline constexpr const char* kSection = "HDR";
inline constexpr const char* kGameSection = "HDR Game Exposure";
inline constexpr const char* kAdaptiveSection = "HDR Adaptive Exposure";

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
inline constexpr const char* kMeteringDensity = "metering_density";
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

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr
