#include "addon_controller.h"
#include "profiling/profile.h"

#include "ctl/values.h"
#include "feature/grade/view.h"
#include "feature/settings_store.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"
#include "state/settings.h"
#include "streamline_compat.h"

namespace reshade_dlssnr {

// ---------------------------------------------------------------------------
// Release-stage step bodies
// ---------------------------------------------------------------------------
//
// These steps touch only controller state and the swapchain queue, so they
// live here rather than in the translation units that own the pass and HDR
// machinery. They are registered through the same lookup the runner uses for
// every other step.

namespace {

// Lazily initializes this pass's HDR measurement resources. The HDR state is
// owned by PassSession, so each pass keeps independent exposure history.
PipelineStatus RunEnsureHdr(PassRun& run) {
  if (run.hdr == nullptr || !run.hdr_state.enable)
    return PipelineStatus::Ok;
  if (run.list == nullptr || run.codec == nullptr)
    return PipelineStatus::SkipFrame;

  Microsoft::WRL::ComPtr<ID3D12Device> device =
      sl_compat::NativeDeviceOf(run.list);
  if (device == nullptr) return PipelineStatus::SkipFrame;

  if (run.codec->Init(device.Get()) && run.hdr->Ensure(device.Get())) {
    TRACE_EVT(run.session, "hdr_initialized pass=%u", run.pass_id);
  }
  // A failure here is not fatal: the pass runs without HDR measurement and
  // retries the ensure next frame.
  return PipelineStatus::Ok;
}

// Grades the final backbuffer, after every pass has run. The one grade that
// is not recorded through a pass session: it acts on the swapchain image
// rather than on a pass's own resources.
//
// Listed first in the Release stage on purpose, so the debug-present override
// below - which replaces the backbuffer wholesale - still wins.
PipelineStatus RunGradeBackbuffer(PassRun& run) {
  if (run.settings == nullptr || run.codec == nullptr) return PipelineStatus::Ok;
  if (!run.settings->enabled) return PipelineStatus::Ok;

  // Read the grade from the store -- the source of truth -- rather than from
  // a copy kept in Settings. The backbuffer grade is single-instance, so its
  // key is the same fixed key its section draws with.
  const GradeSettings grade = feature::grade::ReadBackbuffer(feature::SettingsStore());
  if (!grade.enabled) return PipelineStatus::Ok;

  TRACE_EVT(run.session, "grade_backbuffer luma=%.3f chroma=%.3f", grade.luma,
            grade.chroma);
  AddonController::Get().GradeBackbuffer(*run.codec, run.command_queue,
                                         run.swapchain, grade);
  return PipelineStatus::Ok;
}

// Copies the debug image onto the backbuffer, if a pass produced one this
// frame. This is the last touch before the swapchain present, so it does not
// interfere with anything the game did.
PipelineStatus RunWriteDebugToBackbuffer(PassRun& run) {
  AddonController& controller = AddonController::Get();
  if (run.frame_debug == nullptr || !run.frame_debug->valid) {
    return PipelineStatus::Ok;
  }
  // With no view selected nothing renders, so the image left over from the
  // previous selection would be presented on every frame from here on.
  if (run.debug_view == 0) {
    run.frame_debug->Reset();
    return PipelineStatus::Ok;
  }
  TRACE_EVT(run.session, "present_debug_output");
  controller.PresentDebugOutput(run.command_queue, run.swapchain,
                                *run.frame_debug);
  return PipelineStatus::Ok;
}

// Signals the HDR fence on the game's queue. The game has already submitted
// its command list, so this marks the point at which any pending histogram
// copy has completed.
PipelineStatus RunSignalHdrAtPresent(PassRun& run) {
  AddonController::Get().SignalHdrAtPresent(run.command_queue);
  return PipelineStatus::Ok;
}

// `constexpr`, not `const`: a static_assert can only read an array whose
// initializer is a constant expression. `const` would compile fine for the
// runtime lookup in ControllerStepFor below and still fail the assertion at
// the end of this namespace with C2131.
constexpr PipelineNodeId kEnsureHdrRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kGradeBackbufferRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kWriteDebugRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kSignalHdrRequires[] = {PipelineNodeId::Count};

constexpr PipelineStepDef kControllerSteps[] = {
    // Reads run.list only to resolve the device, and skips cleanly when it is
    // null, so it requires nothing: Present and Release both pass no list, and
    // the step is listed in Release.
    {PipelineNodeId::EnsureHdr, "EnsureHdr", &RunEnsureHdr, kEnsureHdrRequires,
     /*required_inputs=*/StageInput::None, /*requires_hdr_aware=*/true},
    {PipelineNodeId::GradeBackbuffer, "GradeBackbuffer", &RunGradeBackbuffer,
     kGradeBackbufferRequires,
     /*required_inputs=*/StageInput::Swapchain | StageInput::CommandQueue,
     /*requires_hdr_aware=*/false},
    {PipelineNodeId::WriteDebugToBackbuffer, "WriteDebugToBackbuffer",
     &RunWriteDebugToBackbuffer, kWriteDebugRequires,
     /*required_inputs=*/StageInput::Swapchain | StageInput::CommandQueue,
     /*requires_hdr_aware=*/false},
    {PipelineNodeId::SignalHdrAtPresent, "SignalHdrAtPresent",
     &RunSignalHdrAtPresent, kSignalHdrRequires,
     /*required_inputs=*/StageInput::CommandQueue, /*requires_hdr_aware=*/false},
};

constexpr StageInput kReleaseStageInputs =
    StageInput::Swapchain | StageInput::CommandQueue | StageInput::Stage;

}  // namespace

const PipelineStepDef* ControllerStepFor(PipelineNodeId id) {
  return FindStepDef(kControllerSteps, id);
}

// The controller's steps run in the Release stage, against the stage node list
// published in pipeline_graph.h. This is what turns "a step declares a
// prerequisite that the runner can never satisfy" from a per-frame error log
// into a build failure. Add a step here and point it at a prerequisite listed
// later in the release stage - or at one the stage never lists - and the build
// stops.
static_assert(PrerequisitesPrecedeSteps(kReleaseStepNodes,
                                        ArrayCount(kReleaseStepNodes),
                                        kControllerSteps,
                                        ArrayCount(kControllerSteps)),
              "a controller step in the Release stage declares a prerequisite "
              "that runs after it, or that the stage never lists");

// The Release stage is entered from RunSingleStage, which supplies the queue
// and swapchain but no command list. A controller step that reached for
// run.list without guarding it would dereference null here.
//
// A release step that needs to record GPU work does not get a list through
// run.list at all: it opens the stage's own list with BeginReleaseCommands,
// which returns null when the list cannot be created and lets the step skip.
// So the queue, not the list, is what a recording step must declare.
static_assert(
    StageInputsCoverSteps(kReleaseStepNodes, ArrayCount(kReleaseStepNodes),
                          kControllerSteps, ArrayCount(kControllerSteps),
                          kReleaseStageInputs),
    "a controller step in the Release stage needs an input the Release call "
    "site does not supply");


}  // namespace reshade_dlssnr
