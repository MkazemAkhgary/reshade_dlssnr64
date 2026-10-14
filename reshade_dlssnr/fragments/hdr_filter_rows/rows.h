// The exposure-filter rows, as a reusable fragment.
//
// Same shape and same reason as grade_rows: every exposure layer owns an
// independent ExposureFilterSettings, so this appends the rows and the layers
// do not depend on each other. Each layer is its own section, so the names are
// bare and cannot collide.
//
// Adding a filter row touches this file and nobody else.

#pragma once

namespace reshade_dlssnr {
namespace ctl {
class Section;
}

namespace hdr_filter_rows {

// The row names. A reader that looks a filter row up by name
// (feature/hdr/view.cpp) uses these, so the spelling appears once here and the
// fragment and its readers cannot drift apart.
namespace names {
inline constexpr const char* kImportanceMask = "filter_importance_mask";
inline constexpr const char* kDepth = "filter_depth";
inline constexpr const char* kPercentileRange = "filter_percentile_range";
inline constexpr const char* kDepthNear = "depth_near";
inline constexpr const char* kDepthFar = "depth_far";
inline constexpr const char* kPercentileLow = "percentile_low";
inline constexpr const char* kPercentileHigh = "percentile_high";
}  // namespace names

// The three checkbox rows are always interactive: a checkbox is the gate's own
// input, so gating it would make it impossible to turn back on. The parameter
// rows below each checkbox carry that checkbox's value as their gate, declared
// here so the caller never names a row.
void Declare(ctl::Section& section, const char* category = nullptr);

}  // namespace hdr_filter_rows
}  // namespace reshade_dlssnr
