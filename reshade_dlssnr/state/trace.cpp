#include "state/trace.h"

#include "ctl/trace.h"
#include "feature/settings_store.h"
#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

void EmitCtlChange(void*, const ctl::TraceChange& change) {
  switch (change.kind) {
    case ctl::TraceChangeKind::Initial:
      TRACE_CFG("init %s %s", change.name.c_str(), change.value.c_str());
      break;
    case ctl::TraceChangeKind::Changed:
      TRACE_CFG("set %s %s -> %s", change.name.c_str(),
                change.previous_value.c_str(), change.value.c_str());
      break;
    case ctl::TraceChangeKind::Removed:
      TRACE_CFG("unset %s %s", change.name.c_str(),
                change.previous_value.c_str());
      break;
  }
}

struct RuntimeTraceState {
  bool initialized = false;
  bool enabled = false;
  feature::pass::PassId active_pass_id = 0;
};

RuntimeTraceState g_runtime_trace;
ctl::TraceRecorder g_settings_trace;

void TraceRuntimeState(const Settings& settings) {
  if (!g_runtime_trace.initialized) {
    TRACE_CFG("init runtime.enabled %u", settings.enabled ? 1u : 0u);
    TRACE_CFG("init runtime.active_pass_id %u",
              static_cast<unsigned int>(settings.active_pass_id));
    g_runtime_trace.initialized = true;
  } else {
    if (g_runtime_trace.enabled != settings.enabled) {
      TRACE_CFG("set runtime.enabled %u -> %u",
                g_runtime_trace.enabled ? 1u : 0u,
                settings.enabled ? 1u : 0u);
    }
    if (g_runtime_trace.active_pass_id != settings.active_pass_id) {
      TRACE_CFG("set runtime.active_pass_id %u -> %u",
                static_cast<unsigned int>(g_runtime_trace.active_pass_id),
                static_cast<unsigned int>(settings.active_pass_id));
    }
  }
  g_runtime_trace.enabled = settings.enabled;
  g_runtime_trace.active_pass_id = settings.active_pass_id;
}

}  // namespace

void TraceSettings(const Settings& settings) {
  if (!trace::Enabled()) return;
  ctl::ValueStore& store = feature::SettingsStore();
  g_settings_trace.Record(store, &EmitCtlChange);
  TraceRuntimeState(settings);
}

}  // namespace reshade_dlssnr
