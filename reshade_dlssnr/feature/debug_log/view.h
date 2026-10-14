// Debug-log view: read the shared debug view selection from ctl storage.
#pragma once

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace debug_log {

int Read(const ctl::ValueStore& store, const ctl::InstanceKey& key);

}  // namespace debug_log
}  // namespace feature
}  // namespace reshade_dlssnr
