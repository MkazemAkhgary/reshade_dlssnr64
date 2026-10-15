#include "feature/register_all.h"

namespace reshade_dlssnr {
namespace feature {

// One line per feature. Each Register function is defined in that feature's
// own ctl/schema.cpp and declares the feature's whole settings surface there.
void RegisterPreferencesSchema(ctl::Schema& schema);
void RegisterDebugLogSchema(ctl::Schema& schema);
void RegisterGradeSchema(ctl::Schema& schema);
void RegisterHdrSchema(ctl::Schema& schema);
void RegisterImportanceMaskSchema(ctl::Schema& schema);
void RegisterPassSchema(ctl::Schema& schema);
void RegisterUiStateSchema(ctl::Schema& schema);

void RegisterAllSchemas(ctl::Schema& schema) {
  RegisterUiStateSchema(schema);
  RegisterPreferencesSchema(schema);
  RegisterDebugLogSchema(schema);
  RegisterGradeSchema(schema);
  RegisterHdrSchema(schema);
  RegisterImportanceMaskSchema(schema);
  RegisterPassSchema(schema);
}

}  // namespace feature
}  // namespace reshade_dlssnr
