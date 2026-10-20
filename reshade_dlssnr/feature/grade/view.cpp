// Grade view implementation. See view.h.
//
// Every read goes through ctl::ValueStore::Read, which returns the same Value
// the drawer would have resolved. The runtime therefore sees exactly what the
// user sees, with no second copy of the grade to keep in step.

#include "feature/grade/view.h"

#include "ctl/values.h"
#include "feature/grade/settings.h"
#include "fragments/grade_rows/rows.h"

namespace reshade_dlssnr {
namespace feature {
namespace grade {

namespace {

// prefix + suffix, matching how grade_rows::Declare spelled the row.
std::string Row(const std::string& prefix, const char* suffix) {
  return prefix + CStrToString(suffix);
}

}  // namespace

GradeSettings Read(const ctl::ValueStore& store, const ctl::InstanceKey& key,
                   const std::string& section, const std::string& prefix) {
  using grade_rows::names::kBlackLevel;
  using grade_rows::names::kChroma;
  using grade_rows::names::kChromaMode;
  using grade_rows::names::kContrast;
  using grade_rows::names::kEdgeSofteningEnabled;
  using grade_rows::names::kEdgeSoftening;
  using grade_rows::names::kCasEnabled;
  using grade_rows::names::kCasSharpness;
  using grade_rows::names::kHighlights;
  using grade_rows::names::kLuma;
  using grade_rows::names::kLumaMode;
  using grade_rows::names::kShadows;
  using grade_rows::names::kWhiteLevel;

  GradeSettings g;
  // The section's top-level toggle is stored under the bare name "enabled",
  // not under the row prefix: it is declared with HeaderToggle(kEnabled) and
  // a bare Add<BoolSetting>(kEnabled, ...). Reading it here is what makes the
  // grade actually run; without it GradeSettings::enabled stays false and
  // every grade step bails out early.
  g.enabled = store.Read(section, key, kEnabled).as_bool;
  g.luma = store.Read(section, key, Row(prefix, kLuma)).as_float;
  g.chroma = store.Read(section, key, Row(prefix, kChroma)).as_float;
  g.luma_mode = static_cast<GradeLumaMode>(
      store.Read(section, key, Row(prefix, kLumaMode)).as_int);
  g.chroma_mode = static_cast<GradeChromaMode>(
      store.Read(section, key, Row(prefix, kChromaMode)).as_int);
  g.black_level = store.Read(section, key, Row(prefix, kBlackLevel)).as_float;
  g.white_level = store.Read(section, key, Row(prefix, kWhiteLevel)).as_float;
  g.contrast = store.Read(section, key, Row(prefix, kContrast)).as_float;
  g.shadows = store.Read(section, key, Row(prefix, kShadows)).as_float;
  g.highlights = store.Read(section, key, Row(prefix, kHighlights)).as_float;
  g.edge_softening_enabled =
      store.Read(section, key, Row(prefix, kEdgeSofteningEnabled)).as_bool;
  g.edge_softening = store.Read(section, key, Row(prefix, kEdgeSoftening)).as_float;
  g.cas_enabled = store.Read(section, key, Row(prefix, kCasEnabled)).as_bool;
  g.cas_sharpness = store.Read(section, key, Row(prefix, kCasSharpness)).as_float;
  return g;
}

GradeSnapshot ReadBackbuffer(const ctl::ValueStore& store) {
  return Read(store, ctl::InstanceKey::FromInt(0), kSection);
}

}  // namespace grade
}  // namespace feature
}  // namespace reshade_dlssnr
