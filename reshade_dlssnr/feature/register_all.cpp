#include "feature/register_all.h"
#include "profiling/profile.h"

namespace reshade_dlssnr {
namespace feature {

// One line per feature. Each Register function is defined in that feature's
// own ctl/schema.cpp and declares the feature's whole settings surface there.
void RegisterPreferencesSchema(ctl::Schema& schema);
void RegisterDebugLogSchema(ctl::Schema& schema);
void RegisterGradeSchema(ctl::Schema& schema);
void RegisterHdrSchema(ctl::Schema& schema);
void RegisterExposureMaskSchema(ctl::Schema& schema);
void RegisterPassSchema(ctl::Schema& schema);
void RegisterUiStateSchema(ctl::Schema& schema);

void RegisterAllSchemas(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L1("RegisterAllSchemas");
  RegisterUiStateSchema(schema);
  RegisterPreferencesSchema(schema);
  RegisterDebugLogSchema(schema);
  // Pass Order must be registered before every pass-keyed instanced
  // section: Pass and its two model grades, the four HDR sections, and
  // Exposure Mask all resolve their instance keys from the loaded
  // Pass Order, and ctl::Load walks instanced sections in schema order.
  // Registering Pass first puts Pass Order ahead of every section that
  // needs it. RegisterGradeSchema (Grade Backbuffer) is single-instance
  // and does not need the pass keys, so its position is unaffected.
  RegisterPassSchema(schema);
  RegisterGradeSchema(schema);
  RegisterHdrSchema(schema);
  RegisterExposureMaskSchema(schema);
}

}  // namespace feature
}  // namespace reshade_dlssnr
