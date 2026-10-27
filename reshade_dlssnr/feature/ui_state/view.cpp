#include "feature/ui_state/view.h"
#include "profiling/profile.h"

#include "feature/settings_store.h"
#include "ctl/values.h"
#include "feature/ui_state/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace ui_state {
namespace {

const ctl::InstanceKey kSharedKey = ctl::InstanceKey::Shared();

}  // namespace

bool Read(const char* setting) {
  DLSSNR_PROFILE_CTL_L3("Read");
  return feature::SettingsStore().Read(kSection, kSharedKey, setting).as_bool;
}

bool Write(const char* setting, bool value) {
  DLSSNR_PROFILE_CTL_L3("Write");
  return feature::SettingsStore().Write(kSection, kSharedKey, setting,
                            ctl::Value::Bool(value));
}

}  // namespace ui_state
}  // namespace feature
}  // namespace reshade_dlssnr
