// Grade schema: the backbuffer colour grade.
//
// The nine rows come from the grade_rows fragment, which the pass feature also
// uses for its input and output grades. Neither feature includes the other.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/grade/settings.h"
#include "fragments/grade_rows/rows.h"

namespace reshade_dlssnr {
namespace feature {
namespace grade {

namespace {

// The backbuffer grade is the only grade on its section, so it needs no name
// prefix.
constexpr const char* kPrefix = "";

}  // namespace

}  // namespace grade

void RegisterGradeSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterGradeSchema");
  using namespace grade;

  ctl::Section& section = schema.AddSection(kSection, ctl::SectionKind::Single);
  section.HeaderToggle(kEnabled);

  section
      .Add<ctl::BoolSetting>(kEnabled, "Enabled", false)
      .Tooltip("Grade the final swapchain image after every pass, "
               "immediately before presentation. This is independent of each "
               "pass's model-input and model-output grades.");

  grade_rows::Declare(section, kPrefix, kEnabled);
}

}  // namespace feature
}  // namespace reshade_dlssnr
