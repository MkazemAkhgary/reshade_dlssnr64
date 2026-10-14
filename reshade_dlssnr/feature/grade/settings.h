// Grade setting and section names.
//
// A leaf header: names only, no includes. The section name lives here rather
// than in a framework header, so the feature that owns the section is the only
// place its spelling appears -- and the only file that changes when it is
// renamed.

#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace grade {

inline constexpr const char* kSection = "Grade Backbuffer";
inline constexpr const char* kEnabled = "enabled";

}  // namespace grade
}  // namespace feature
}  // namespace reshade_dlssnr
