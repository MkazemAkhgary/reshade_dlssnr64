// The reusable colour-grade rows, as a reusable fragment.
//
// This is not a feature. It has no section of its own, no runtime, no state
// and no registry entry; it is a function that appends rows, like a primitive.
// The backbuffer grade and each pass's input/output grades all append the same
// nine rows under different name prefixes, so no feature depends on another.
//
// Adding a row touches this file and nobody else. Deleting a consumer touches
// that consumer and nobody else.

#pragma once

#include <string>

#include "ctl/types.h"

namespace reshade_dlssnr {
namespace ctl {
class Section;
}

namespace grade_rows {

// The row suffixes, without the caller's prefix. A reader that needs to look
// a row up by name (feature/grade/view.cpp) uses these, so the spelling
// appears once here and the fragment and its readers cannot drift apart.
//
// Declare() builds each name as prefix + suffix; these are the suffixes.
namespace names {
inline constexpr const char* kBlackLevel = "black_level";
inline constexpr const char* kWhiteLevel = "white_level";
inline constexpr const char* kContrast = "contrast";
inline constexpr const char* kShadows = "shadows";
inline constexpr const char* kHighlights = "highlights";
inline constexpr const char* kLumaMode = "luma_mode";
inline constexpr const char* kLuma = "luma";
inline constexpr const char* kChromaMode = "chroma_mode";
inline constexpr const char* kChroma = "chroma";
inline constexpr const char* kEdgeSofteningEnabled = "edge_softening_enabled";
inline constexpr const char* kEdgeSoftening = "edge_softening";
inline constexpr const char* kCasEnabled = "cas_enabled";
inline constexpr const char* kCasSharpness = "cas_sharpness";
}  // namespace names

// Declare the grade rows under a name prefix.
//
// `name_prefix` is what lets two grade instances on one section coexist:
// "grade_input." and "grade_output." produce distinct setting names, so a
// category stays a label and never has to carry identity. An empty prefix
// gives the bare names, which is what the backbuffer section uses.
//
// `category`, when given, is applied to every row so the caller gets them
// under one collapsible sub-header.
//
// `enabled`, when given, is applied to every row here -- the caller does not
// know the row names and does not need to. Passing it in is what keeps
// "adding a row touches this file and nobody else" true.
void Declare(ctl::Section& section, const std::string& name_prefix,
             ctl::EnablePredicate enabled = nullptr,
             const void* context = nullptr, const char* category = nullptr);

// Convenience overload: the rows are enabled while the named bool setting in
// the same section is true. Equivalent to passing &ctl::BoolFromSetting with
// that name as the predicate's context.
void Declare(ctl::Section& section, const std::string& name_prefix,
             const char* enabled_setting_name,
             const char* category = nullptr);

}  // namespace grade_rows
}  // namespace reshade_dlssnr
