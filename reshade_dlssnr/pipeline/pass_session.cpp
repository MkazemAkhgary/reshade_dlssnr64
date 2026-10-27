#include "pass_session.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <mutex>

#include <reshade.hpp>

#include "debug/debug_view.h"
#include "feature/settings_store.h"
#include "ctl/values.h"
#include "d3d12_utils.h"
#include "gpu_readback.h"
#include "dlss_guides.h"
#include "feature/debug_log/view.h"
#include "feature/grade/view.h"
#include "feature/hdr/view.h"
#include "feature/exposure_mask/view.h"
#include "feature/pass/settings.h"
#include "feature/preferences/settings.h"
#include "feature/pass/ui.h"
#include "model/model_pipeline.h"
#include "model/ngx_model_params.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"
#include "exposure_mask/exposure_mask.h"
#include "streamline_compat.h"

namespace reshade_dlssnr {

  // namespace

// Every enabled pass, in tab order.
//
// Enabled-ness and target are settings, so they are read from the store by
// stable id rather than from Settings; only the tab order comes from Settings,
// because that is application state the store does not hold.
std::vector<EnabledPass> EnabledPasses(const Settings& settings) {
  std::vector<EnabledPass> passes;
  ctl::ValueStore& store = feature::SettingsStore();
  for (uint32_t position = 0; position < settings.pass_order.size();
       ++position) {
    const feature::pass::PassId id = settings.pass_order[position];
    const feature::pass::PassSnapshot pass =
        feature::pass::Read(store, feature::pass::PassKey(id));
    if (!pass.enabled) continue;
    EnabledPass entry;
    entry.id = id;
    entry.target = pass.target;
    passes.push_back(entry);
  }
  return passes;
}

namespace {

std::mutex g_session_mutex;

// One line, because it is the difference between two very different worlds:
// Streamline 1.x proxies the device and the command queue unconditionally, so
// anything we create from them is an SL proxy, while a 2.x host on its default
// flags has no proxies at all.
void LogPresentInterfacesOnce(bool device_is_proxy, ID3D12CommandQueue* queue) {
  static bool logged = false;
  if (logged) return;
  logged = true;

  bool queue_is_proxy = false;
  sl_compat::Native(queue, &queue_is_proxy);

  char message[192];
  std::snprintf(message, sizeof(message),
                "DLSSNR pass: Present device %s, command queue %s",
                device_is_proxy ? "was a Streamline proxy" : "is not a Streamline proxy",
                queue_is_proxy ? "was a Streamline proxy" : "is not a Streamline proxy");
  reshade::log::message(reshade::log::level::info, message);
}

using PassResourceGetter = ID3D12Resource* (PassResources::*)() const;

struct PassResourceState {
  PassResourceGetter get;
  D3D12_RESOURCE_STATES state;
};

constexpr PassResourceState kPassResourceStates[] = {
    {&PassResources::GetOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetCompositeOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetWorkingInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetWorkingOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetGradedWorkingInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetUnliftedWorkingInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetPackedOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
    {&PassResources::GetEffectiveExposureMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
    {&PassResources::GetGainStats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetTemporalPreviousInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
    {&PassResources::GetTemporalPreviousEdit, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetTemporalPreviousDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetTemporalCandidate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetTemporalValidatedCandidate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
    {&PassResources::GetTemporalStats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
};

}  // namespace

void PassSession::Fail(const char* reason) {
  failed_ = true;
  reason_ = reason;
  TRACE_EVT(this, "FAIL pass=%u reason=%s", pass_id_,
            reason != nullptr ? reason : "(null)");
  // FailForStep() already substitutes a default before calling here, but Fail
  // is also reachable directly, and passing nullptr to the logger would
  // dereference it.
  reshade::log::message(reshade::log::level::error,
                        reason != nullptr ? reason : "DLSSNR pass failed");
}

void PassSession::Retry(NgxRuntime& runtime) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  // Only meaningful on a failed pass. Calling this on a working pass would
  // release a good feature for no reason.
  if (!failed_) return;

  // Release the feature so EnsureFeature rebuilds it on the next run. This
  // makes the retry robust against a feature handle that got into a bad
  // state (e.g. after an EvaluateFeature failure) without us needing to
  // know what specifically went wrong.
  feature_.ReleaseFeature(runtime);

  failed_ = false;
  reason_ = "";

  // Re-arm the evaluate diagnostic. The retry rebuilds the feature, so the
  // next evaluation genuinely is a new one and its descriptors belong in the
  // log even if they match the failed attempt's.
  eval_log_key_ = EvalLogKey{};
  eval_log_key_valid_ = false;
  eval_log_written_this_frame_ = false;

  reshade::log::message(reshade::log::level::info,
                        "DLSSNR pass: retrying after failure");
}

namespace {

// Bytes per pixel for the DXGI formats this addon creates. Anything else is
// counted as 4 bytes, which is a harmless over/under-estimate for a format
// the addon does not itself allocate.
uint32_t BytesPerPixel(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
      return 2;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
      return 4;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
      return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
      return 16;
    default:
      return 4;
  }
}

uint64_t EstimateBytes(ID3D12Resource* resource) {
  if (resource == nullptr) return 0;
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    return desc.Width;
  }
  return static_cast<uint64_t>(desc.Width) * desc.Height *
         BytesPerPixel(desc.Format);
}

void AppendResource(PassResourceStats& stats, const char* name,
                    ID3D12Resource* resource) {
  if (resource == nullptr || stats.count >= kMaxPassResourceEntries) return;
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  ResourceInfoEntry& entry = stats.entries[stats.count++];
  std::snprintf(entry.name, sizeof(entry.name), "%s", name);
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    entry.width = static_cast<uint32_t>(desc.Width);
    entry.height = 0;
    entry.format = static_cast<uint32_t>(DXGI_FORMAT_UNKNOWN);
    entry.bytes = desc.Width;
  } else {
    entry.width = static_cast<uint32_t>(desc.Width);
    entry.height = desc.Height;
    entry.format = static_cast<uint32_t>(desc.Format);
    entry.bytes = EstimateBytes(resource);
  }
  stats.total_bytes += entry.bytes;
}

}  // namespace

PassStatus PassSession::GetStatus() const {
  // Only the runtime-owned fields are populated here. `enabled` is
  // setting-owned and is filled in by AddonController::RefreshStatus.
  PassStatus s;
  s.running = !failed_ && feature_.HasFeature();
  s.failed = failed_;
  s.has_feature = feature_.HasFeature();
  if (reason_ != nullptr && reason_[0] != '\0') {
    std::snprintf(s.reason, sizeof(s.reason), "%s", reason_);
  }

  // Enumerate the pass's own GPU allocations. Every accessor is const and
  // returns the live resource, so a reallocation between frames is reflected
  // on the next Present without any explicit invalidation. The order groups
  // related resources so a related set (working-scale buffers, temporal
  // history, synthetic guides) reads as a block in the UI.
  AppendResource(s.resources, "output", resources_.GetOutput());
  AppendResource(s.resources, "composite_output",
                 resources_.GetCompositeOutput());
  AppendResource(s.resources, "packed_hdr_sdr", resources_.GetPackedOutput());
  AppendResource(s.resources, "working_input", resources_.GetWorkingInput());
  AppendResource(s.resources, "working_output", resources_.GetWorkingOutput());
  AppendResource(s.resources, "graded_working_input",
                 resources_.GetGradedWorkingInput());
  AppendResource(s.resources, "unlifted_working_input",
                 resources_.GetUnliftedWorkingInput());
  AppendResource(s.resources, "effective_exposure_map",
                 resources_.GetEffectiveExposureMap());
  AppendResource(s.resources, "local_exposure_partial_sums",
                 resources_.GetLocalExposurePartialSums());
  AppendResource(s.resources, "local_exposure_global_scale",
                 resources_.GetLocalExposureGlobalScale());
  AppendResource(s.resources, "gain_stats", resources_.GetGainStats());
  AppendResource(s.resources, "temporal_previous_input",
                 resources_.GetTemporalPreviousInput());
  AppendResource(s.resources, "temporal_previous_edit",
                 resources_.GetTemporalPreviousEdit());
  AppendResource(s.resources, "temporal_previous_depth",
                 resources_.GetTemporalPreviousDepth());
  AppendResource(s.resources, "temporal_candidate",
                 resources_.GetTemporalCandidate());
  AppendResource(s.resources, "temporal_validated_candidate",
                 resources_.GetTemporalValidatedCandidate());
  AppendResource(s.resources, "temporal_stats",
                 resources_.GetTemporalStats());
  AppendResource(s.resources, "temporal_cheap_input",
                 resources_.GetCheapTemporalInput());
  AppendResource(s.resources, "temporal_cheap_output",
                 resources_.GetCheapTemporalOutput());
  AppendResource(s.resources, "synthetic_previous_input",
                 resources_.GetSyntheticPreviousInput());
  AppendResource(s.resources, "synthetic_previous_depth",
                 resources_.GetSyntheticPreviousDepth());
  AppendResource(s.resources, "synthetic_current_depth",
                 resources_.GetSyntheticCurrentDepth());
  AppendResource(s.resources, "synthetic_motion",
                 resources_.GetSyntheticMotion());
  for (uint32_t i = 0; i < kScratchCount; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "scratch[%u]", i);
    AppendResource(s.resources, name, resources_.GetScratch(i));
  }
  return s;
}

bool PassSession::AcquireBackbuffer(
    IDXGISwapChain* swapchain,
    ID3D12CommandQueue* command_queue,
    Microsoft::WRL::ComPtr<IDXGISwapChain3>& out_swapchain3,
    Microsoft::WRL::ComPtr<ID3D12Device>& out_device,
    Microsoft::WRL::ComPtr<ID3D12Resource>& out_backbuffer,
    D3D12_RESOURCE_DESC& out_desc) {
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&out_swapchain3))) ||
      out_swapchain3 == nullptr) {
    reshade::log::message(
        reshade::log::level::error,
        "DLSSNR pass: swapchain is not an IDXGISwapChain3");
    return false;
  }

  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&out_device))) ||
      out_device == nullptr) {
    reshade::log::message(
        reshade::log::level::error,
        "DLSSNR pass: could not retrieve device from command queue");
    return false;
  }

  // Streamline 1.x proxies the command queue, and a proxy queue reports its
  // *proxy* device here. Building the session's command list on that device
  // would make our list an SL proxy too -- the 2.x guide is explicit that a
  // third party must not use them, and it is the one structural difference
  // between a 1.x host and a 2.x one. Resolve to the native device so every
  // object this pass creates belongs to d3d12.dll.
  bool device_is_proxy = false;
  Microsoft::WRL::ComPtr<ID3D12Device> native_device =
      sl_compat::Native(out_device.Get(), &device_is_proxy);
  if (native_device == nullptr) {
    reshade::log::message(
        reshade::log::level::error,
        "DLSSNR pass: could not resolve the native device");
    return false;
  }

  out_device = native_device;
  LogPresentInterfacesOnce(device_is_proxy, command_queue);

  const UINT back_buffer_index = out_swapchain3->GetCurrentBackBufferIndex();
  if (FAILED(out_swapchain3->GetBuffer(
          back_buffer_index, IID_PPV_ARGS(&out_backbuffer))) ||
      out_backbuffer == nullptr) {
    reshade::log::message(
        reshade::log::level::error,
        "DLSSNR pass: could not retrieve swapchain backbuffer");
    return false;
  }

  out_desc = out_backbuffer->GetDesc();
  return true;
}

bool PassSession::EnsureFrameResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int work_width,
    unsigned int work_height,
    bool allow_identity_input,
    bool need_graded_input,
    bool fp16_storage) {
  if (!resources_.EnsureCompositeOutput(device, desc)) {
    reason_ = "failed to create DLSSNR composite resource";
    return false;
  }

  D3D12_RESOURCE_DESC model_desc = desc;
  const HdrFormat frame_format = DetectFormatFromDxgi(desc.Format);
  const bool hdr_frame = frame_format == HdrFormat::ScRgb ||
                         frame_format == HdrFormat::Hdr10;
  const DXGI_FORMAT hdr_storage_format = fp16_storage
      ? DXGI_FORMAT_R16G16B16A16_FLOAT
      : DXGI_FORMAT_R32G32B32A32_FLOAT;
  if (hdr_frame) {
    model_desc.Format = hdr_storage_format;
  }

  const bool identity_input = allow_identity_input && !hdr_frame &&
      desc.Width == work_width && desc.Height == work_height;
  if (identity_input) {
    resources_.ReleaseOutput();
  } else if (!resources_.EnsureOutput(device, model_desc)) {
    reason_ = "failed to create DLSSNR model output";
    return false;
  }
  const bool need_graded = need_graded_input && !identity_input;
  if (!resources_.EnsureWorkingResources(device, model_desc,
                                         work_width, work_height,
                                         !identity_input, need_graded)) {
    reason_ = "failed to create DLSSNR working resources";
    return false;
  }

  if (hdr_frame) {
    if (!resources_.EnsurePackedOutput(device, desc, hdr_storage_format) ||
        !resources_.EnsureGainStats(device)) {
      reason_ = "failed to create HDR gain-map resources";
      return false;
    }
  }

  return true;
}

bool PassSession::FillEvaluateParams(
    NgxRuntime& runtime,
    const feature::pass::PassSnapshot& pass,
    const DlssGuideState& guides,
    ID3D12Resource* model_color,
    ID3D12Resource* model_output,
    unsigned int eval_width,
    unsigned int eval_height,
    unsigned int width,
    unsigned int height) {
  auto* params = runtime.Parameters().Get();
  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return false;
  }

  // No Reset(): the block is shared with Streamline's sl.common.

  NgxParameterBlock& block = runtime.Parameters();

  block.SetResource("DLSSNR.Color", model_color);
  block.SetResource("DLSSNR.Output", model_output);
  block.SetUInt("DLSSNR.Width", eval_width);
  block.SetUInt("DLSSNR.Height", eval_height);
  block.SetUInt("DLSSNR.InputWidth", width);
  block.SetUInt("DLSSNR.InputHeight", height);
  block.SetUInt("DLSSNR.OutputWidth", eval_width);
  block.SetUInt("DLSSNR.OutputHeight", eval_height);
  ApplyModelTuningParams(block, pass);

  // UI mask capability is intentionally unused for now. The model supports:
  //   block.SetResource("DLSSNR.UIAlpha", <ui_mask_resource>);
  //   block.SetResource("DLSSNR.Backbuffer", <backbuffer>);
  // which will be re-enabled when a reliable UI mask source is available.

  // Always overwrite guide fields. The NGX parameter object is shared and
  // persists between evaluations; skipping these writes when guides are absent
  // would leave pointers to resources from an older frame.
  block.SetResource("DLSSNR.Depth", guides.valid ? guides.depth : nullptr);
  block.SetResource("DLSSNR.MVec", guides.valid ? guides.motion : nullptr);
  block.SetUInt("DLSSNR.DepthInverted",
                guides.valid && guides.depth_inverted ? 1u : 0u);
  block.SetFloat("DLSSNR.MVecScaleX",
                 guides.valid ? guides.mv_scale_x : 0.0f);
  block.SetFloat("DLSSNR.MVecScaleY",
                 guides.valid ? guides.mv_scale_y : 0.0f);

  block.SetUInt("DLSSNR.ColorSubrectBaseX", 0u);
  block.SetUInt("DLSSNR.ColorSubrectBaseY", 0u);
  block.SetUInt("DLSSNR.ColorSubrectWidth", eval_width);
  block.SetUInt("DLSSNR.ColorSubrectHeight", eval_height);
  block.SetUInt("DLSSNR.OutputSubrectBaseX", 0u);
  block.SetUInt("DLSSNR.OutputSubrectBaseY", 0u);
  block.SetUInt("DLSSNR.OutputSubrectWidth", eval_width);
  block.SetUInt("DLSSNR.OutputSubrectHeight", eval_height);
  block.SetUInt("DLSSNR.DepthSubrectBaseX", 0u);
  block.SetUInt("DLSSNR.DepthSubrectBaseY", 0u);
  block.SetUInt("DLSSNR.DepthSubrectWidth",
                guides.valid ? guides.guide_width : 0u);
  block.SetUInt("DLSSNR.DepthSubrectHeight",
                guides.valid ? guides.guide_height : 0u);
  block.SetUInt("DLSSNR.MVecSubrectBaseX", 0u);
  block.SetUInt("DLSSNR.MVecSubrectBaseY", 0u);
  block.SetUInt("DLSSNR.MVecSubrectWidth",
                guides.valid ? guides.motion_width : 0u);
  block.SetUInt("DLSSNR.MVecSubrectHeight",
                guides.valid ? guides.motion_height : 0u);
  block.SetUInt("DLSSNR.Reset", feature_.ShouldReset() ? 1u : 0u);
  feature_.MarkEvaluated();

  return true;
}

void PassSession::SeedFrameState(ID3D12Resource* backbuffer) {
  DLSSNR_PROFILE_RESOURCE_L2("PassSession::SeedFrameState");
  if (backbuffer != nullptr) {
    command_.SetResourceState(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
  }
  for (const PassResourceState& entry : kPassResourceStates) {
    ID3D12Resource* resource = (resources_.*entry.get)();
    if (resource != nullptr) {
      command_.SetResourceState(resource, entry.state);
    }
  }
}

void PassSession::CleanupFrameState(ID3D12GraphicsCommandList* command_list) {
  DLSSNR_PROFILE_RESOURCE_L2("PassSession::CleanupFrameState");
  if (command_list == nullptr) return;
  for (const PassResourceState& entry : kPassResourceStates) {
    command_.Transition(command_list, (resources_.*entry.get)(), entry.state);
  }
}

void PassSession::SubmitBackbuffer() {
  DLSSNR_PROFILE_ASYNC_L2("PassSession::SubmitBackbuffer");
  // The backbuffer commands are ordered before Present on this same queue.
  // Signal completion for future allocator reuse and deferred retirement, but
  // do not make every frame's CPU thread wait for that fence.
  command_.CloseAndSubmit(command_queue_);
  command_.SignalGpu(command_queue_);
}

bool PassSession::MaybeRenderDebug(
    int debug_view,
    feature::pass::PassId active_pass_id,
    ComputeEngine& codec,
    ID3D12GraphicsCommandList* command_list,
    FrameDebugContext& frame_debug,
    const DebugSnapshot& snapshot,
    const model_pipeline::HdrUniforms* hdr,
    unsigned int width,
    unsigned int height,
    DXGI_FORMAT format) {
  // Debug view is a shared setting; only the active pass renders it.
  if (debug_view == 0) return false;
  if (pass_id_ != active_pass_id) return false;
  if (command_list == nullptr) return false;

  // The debug output is displayed on the backbuffer, so prefer the
  // backbuffer's format. Fall back to the pass's own format only when
  // the controller hasn't yet cached the backbuffer format.
  const DXGI_FORMAT target_format =
      frame_debug.preferred_format != DXGI_FORMAT_UNKNOWN
          ? frame_debug.preferred_format
          : format;

  // Debug views are displayed on the backbuffer. When the source stage runs
  // at render resolution (Pre-DLSS), render the diagnostic at the cached
  // backbuffer extent so the present copy remains valid and the source is
  // scaled by DebugDisplay rather than being silently skipped.
  if (frame_debug.preferred_width == 0 || frame_debug.preferred_height == 0) {
    // Pre/Post-DLSS execute before the first Present can populate the target
    // extent. Skip only that first diagnostic rather than allocating a
    // render-sized texture that would be retired on the following frame.
    return false;
  }
  const unsigned int target_width = frame_debug.preferred_width;
  const unsigned int target_height = frame_debug.preferred_height;

  // (Re)create the shared debug output when its shape changes. The output
  // is owned by FrameDebugContext and lives across frames; a replaced one is
  // retired rather than released, because the previous frame's present copy
  // may still be reading it. The device is fetched first because the slot check
  // compares it.
  Microsoft::WRL::ComPtr<ID3D12Device> debug_device =
      sl_compat::NativeDeviceOf(command_list);
  if (debug_device == nullptr) return false;
  if (!frame_debug.output.Matches(debug_device.Get(), target_width,
                                  target_height, target_format)) {
    if (!frame_debug.EnsureOutput(debug_device.Get(), target_width,
                                  target_height, target_format)) {
      return false;
    }
  }

  // Seed the pass tracker: at this point the debug output is always UAV
  // (it was left UAV by the previous frame's copy, or just created UAV).
  command_.SetResourceState(frame_debug.output.Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (!RenderDebugView(command_, codec, command_list, snapshot,
                       hdr,
                       debug_view, frame_debug.output.Get(),
                       target_width, target_height)) {
    return false;
  }

  frame_debug.valid = true;
  return true;
}

void PassSession::Shutdown(NgxRuntime& runtime) {
  DLSSNR_PROFILE_RESOURCE_L2("PassSession::Shutdown");
  std::lock_guard<std::mutex> lock(g_session_mutex);

  temporal_.readback.Shutdown();
  temporal_ = PassTemporalState{};
  feature_.Shutdown(runtime);
  resources_.Shutdown();
  exposure_mask_.Shutdown();
  hdr_state_.Shutdown();
  command_.Shutdown();

  command_queue_ = nullptr;
  failed_ = false;
  reason_ = "";

  // Drop the cached diagnostic so a re-created session logs its first
  // evaluation unconditionally.
  eval_log_key_ = EvalLogKey{};
  eval_log_key_valid_ = false;
  eval_log_written_this_frame_ = false;
}

void PassSession::PrepareRun(PassRun& run,
                             const Settings& settings,
                             ComputeEngine& codec,
                             NgxRuntime& runtime,
                             ID3D12GraphicsCommandList* command_list,
                             ID3D12Resource* output_resource,
                             bool is_first_pass,
                             FrameDebugContext& frame_debug,
                             HdrState* hdr,
                             HdrState* previous_hdr,
                             CommandContext* hdr_cmd,
                             ExposureMask* exposure_mask,
                             ID3D12Resource* game_exposure_texture,
                             ID3D12CommandQueue* command_queue,
                             IDXGISwapChain* swapchain,
                             feature::pass::PassId pass_id,
                             uint64_t frame_id,
                             ID3D12Fence* temporal_feedback_fence,
                             uint64_t temporal_feedback_fence_value) {
  // Set before anything can read it: PrepareRun keys the store by it.
  pass_id_ = pass_id;
  run.session = this;
  run.cmd = &command_;
  run.resources = &resources_;
  run.feature = &feature_;
  run.codec = &codec;
  run.runtime = &runtime;
  run.hdr = hdr;
  run.previous_hdr = previous_hdr;
  run.hdr_cmd = hdr_cmd;
  run.exposure_mask = exposure_mask;
  run.game_exposure_texture = game_exposure_texture;
  run.frame_debug = &frame_debug;
  run.frame_id = frame_id;
  run.temporal_feedback_fence = temporal_feedback_fence;
  run.temporal_feedback_fence_value = temporal_feedback_fence_value;

  run.settings = &settings;
  run.pass_bound = true;

  // Read the pass's settings and its two model-boundary grades out of the
  // store and carry them by value. The store is the source of truth; the
  // render path never reaches for it again after this.
  //
  // Keyed by the pass's stable id, which the caller passes in: it is not the
  // session's slot, because a reorder rearranges the tabs without moving any
  // setting.
  {
    const ctl::ValueStore& store = feature::SettingsStore();
    const ctl::InstanceKey key = feature::pass::PassKey(pass_id_);
    run.pass_state = feature::pass::Read(store, key);
    run.grade_input =
        feature::grade::Read(store, key, feature::pass::kGradeInputSection,
                             feature::pass::kGradeInputPrefix);
    run.grade_output =
        feature::grade::Read(store, key, feature::pass::kGradeOutputSection,
                             feature::pass::kGradeOutputPrefix);
    run.input_grade_apply_to_pre_grade =
        store.Read(feature::pass::kGradeInputSection, key,
                   feature::pass::kGradeInputApplyToPreGrade)
            .as_bool;

    // HDR metering and Exposure Mask settings are pass-keyed just like the
    // model and its two grade boundaries. Each pass has its own state/history.
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    run.debug_view = feature::debug_log::Read(store, shared);
    run.hdr_state = feature::hdr::Read(store, key);
    run.mask_state = feature::exposure_mask::Read(store, key);
    run.fp16_storage =
        store.Read(feature::preferences::kSection, shared,
                   feature::preferences::kFp16Storage).as_bool;
    if (run.fp16_storage != fp16_storage_last_) {
      fp16_storage_last_ = run.fp16_storage;
      // Releasing the feature here crashes the streamline v1 runtime: this runs
      // while the frame that still references the feature is in flight. The
      // preference only selects the format of our own intermediate buffers,
      // which are recreated below, so the feature does not need rebuilding.
      feature_.SetShouldReset(true);
    }
  }

  run.list = command_list;
  run.output_resource = output_resource;
  run.command_queue = command_queue;
  run.swapchain = swapchain;

  const ctl::ValueStore& store = feature::SettingsStore();
  const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
  const bool invert_fallback_depth = store.Read(
      feature::preferences::kSection, shared,
      feature::preferences::kInvertFallbackDepth).as_bool;
  run.guides = guides_->GetState(invert_fallback_depth);
  run.guides_valid = run.guides.valid;
  run.is_first_pass = is_first_pass;
}

PipelineStatus PassSession::RunStage(const PipelineStageDef& stage,
                                     const Settings& settings,
                                     ComputeEngine& codec,
                                     NgxRuntime& runtime,
                                     ID3D12GraphicsCommandList* command_list,
                                     ID3D12Resource* output_resource,
                                     bool is_first_pass,
                                     FrameDebugContext& frame_debug,
                                     HdrState* hdr,
                                     HdrState* previous_hdr,
                                     CommandContext* hdr_cmd,
                                     ExposureMask* exposure_mask,
                                     ID3D12Resource* game_exposure_texture,
                                     ID3D12CommandQueue* command_queue,
                                     IDXGISwapChain* swapchain,
                                     feature::pass::PassId pass_id,
                                     uint64_t frame_id,
                                     ID3D12Fence* temporal_feedback_fence,
                                     uint64_t temporal_feedback_fence_value) {
  DLSSNR_PROFILE_RENDER_L1("PassSession::RunStage");
  DLSSNR_PROFILE_RENDER_TAG_L2("PassId", static_cast<uint32_t>(pass_id));
  // Serializes stage execution across passes.
  std::lock_guard<std::mutex> lock(g_session_mutex);

  // Guards common to both stages. A disabled addon or a failed pass does
  // nothing at all; a missing input is likewise not an error, just nothing
  // to do.
  if (!settings.enabled) return PipelineStatus::SkipFrame;
  if (failed_) return PipelineStatus::SkipFrame;

  if (!stage.owns_submit) {
    // PreDLSS and PostDLSS record on the game's list and publish to a game
    // resource; both are required. Backbuffer passes own their command list.
    if (command_list == nullptr || output_resource == nullptr) {
      return PipelineStatus::SkipFrame;
    }
  } else if (command_queue == nullptr || swapchain == nullptr) {
    return PipelineStatus::SkipFrame;
  }

  TRACE_EVT(this, "ENTER stage=%s pass=%u", stage.name, pass_id_);

  // Each game-list invocation starts with a clean per-stage state map. This
  // also makes early-exit cleanup independent of pointers/states cached by the
  // preceding frame's invocation of this pass.
  if (!stage.owns_submit) command_.ClearState();

  PassRun run;
  PrepareRun(run, settings, codec, runtime, command_list, output_resource,
             is_first_pass, frame_debug, hdr, previous_hdr, hdr_cmd, exposure_mask,
             game_exposure_texture, command_queue, swapchain, pass_id,
             frame_id, temporal_feedback_fence,
             temporal_feedback_fence_value);

  codec.InvalidateBindings();
  const PipelineStatus status = PipelineRunner::RunStage(stage, run);

  // Direct Post-DLSS mode transitions the game's output to SRV before model
  // work. A stage can skip before its normal WriteToGameResource restore (for
  // example while shaders compile). Restore the output state on every such
  // exit. In clone mode the original output is not touched unless writeback
  // began, which is tracked by command_.
  if (status != PipelineStatus::Ok && !stage.owns_submit && !stage.is_pre_dlss &&
      !stage.output_state_managed_externally && output_resource != nullptr &&
      run.guides.dlss_output_color != nullptr) {
    D3D12_RESOURCE_STATES tracked = command_.GetResourceState(output_resource);
    if (!run.guides.clone_resources &&
        tracked == D3D12_RESOURCE_STATE_COMMON) {
      // CaptureOutput already transitioned the direct resource to SRV, but
      // an early skip may precede this stage's resource-state seeding.
      tracked = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      command_.SetResourceState(output_resource, tracked);
    }
    if (tracked != D3D12_RESOURCE_STATE_COMMON) {
      command_.Transition(command_list, output_resource,
                          InferResourceState(output_resource));
    }
  }

  // A Present-stage step can stop after StepSeedResourceStates has already
  // opened this pass's private command list, but before the normal
  // StepSubmitBackbuffer tail is reached. Discard that abandoned recording so the
  // next frame never tries to Reset an allocator/list that is still recording.
  // This is transient frame cleanup, not a GPU execution error.
  if (status != PipelineStatus::Ok && stage.owns_submit) {
    command_.Abort();
  }

  TRACE_EVT(this, "EXIT stage=%s pass=%u status=%d", stage.name, pass_id_,
            static_cast<int>(status));
  return status;
}

// ---------------------------------------------------------------------------
// Pipeline step bodies
// ---------------------------------------------------------------------------
//
// The step order lives in pipeline_graph.cpp; this file says how each step
// runs.
//
// They are static members of PassSession rather than free functions because
// they are the session's own work: they touch resources_, feature_, command_
// and guides_ directly, which is exactly what a free function could not do
// without a dozen accessors that would only exist to defeat encapsulation.
// The step signature is the same either way - a static member converts to the
// PipelineStepFn pointer type.
//
// Two conventions hold throughout:
//
//   - Ok means the stage may continue. SkipFrame means the rest of the stage
//     does not run this frame but the pass stays healthy (feature just
//     rebuilt, codec still compiling, a guide not yet available). Fail means
//     the pass has been marked failed via Fail() and the stage stops.
//   - A step reads what it needs from `run` and writes results back to it.

// Sets failed_/reason_ and logs. The step wrappers use this instead of calling
// Fail() on the session directly so that a step which reports Fail always has
// left a reason behind: passing nullptr keeps an existing reason_ if one is
// already set, which is what the pre-refactor call sites did when they had
// produced a reason deeper down.
void PassSession::FailForStep(const char* reason) {
  DLSSNR_PROFILE_CORE_L2("PassSession::FailForStep");
  if (reason == nullptr && reason_ != nullptr && reason_[0] != '\0') {
    Fail(reason_);
    return;
  }
  Fail(reason != nullptr ? reason : "DLSSNR pass failed");
}

PipelineStatus PassSession::FailStep(const char* reason) {
  FailForStep(reason);
  return PipelineStatus::Fail;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

// Backbuffer only: acquire the swapchain backbuffer and the device behind the
// command queue.
PipelineStatus PassSession::StepAcquireBackbuffer(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepAcquireBackbuffer");
  PassSession* self = run.session;
  IDXGISwapChain* swapchain = run.swapchain;
  ID3D12CommandQueue* queue = run.command_queue;
  if (swapchain == nullptr || queue == nullptr) return PipelineStatus::SkipFrame;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
  D3D12_RESOURCE_DESC desc = {};

  if (!self->AcquireBackbuffer(swapchain, queue, swapchain3, device, backbuffer,
                         desc)) {
    TRACE_EVT(self, "SKIP stage=Present pass=%u reason=acquire_backbuffer",
              self->pass_id_);
    return PipelineStatus::SkipFrame;
  }

  run.swapchain3 = swapchain3;
  run.device_owner = device;
  run.backbuffer_owner = backbuffer;
  run.device = run.device_owner.Get();
  run.backbuffer = run.backbuffer_owner.Get();
  run.desc = desc;
  run.width = static_cast<unsigned int>(desc.Width);
  run.height = static_cast<unsigned int>(desc.Height);
  return PipelineStatus::Ok;
}

// Resolve the device and bring up the two objects every later step needs:
// the NGX runtime and this session's own command list.
//
// Backbuffer gets the device from the queue; PreDLSS/PostDLSS from the game's
// command list. A missing device is transient for the game-list stages
// because the game may not have loaded the DLSS model yet, and is fatal for
// Backbuffer.
PipelineStatus PassSession::StepResolveDevice(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepResolveDevice");
  PassSession* self = run.session;
  const bool uses_game_list = run.stage != nullptr && !run.stage->owns_submit;

  if (uses_game_list) {
    if (run.list == nullptr) return PipelineStatus::SkipFrame;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(run.list->GetDevice(IID_PPV_ARGS(&device))) ||
        device == nullptr) {
      // Transient: don't fail the pass permanently. This can happen on frames
      // where ReShade hasn't fully wired up the command list yet.
      TRACE_EVT(self, "SKIP stage=PostDLSS pass=%u reason=no_device",
                self->pass_id_);
      return PipelineStatus::SkipFrame;
    }

    // A Streamline proxy list reports its proxy device here, and creating our
    // textures on that would put them under Streamline's resource tracking.
    device = sl_compat::Native(device.Get());
    if (device == nullptr) {
      TRACE_EVT(self, "SKIP stage=PostDLSS pass=%u reason=no_native_device",
                self->pass_id_);
      return PipelineStatus::SkipFrame;
    }

    run.device_owner = device;
    run.device = run.device_owner.Get();

    if (run.runtime == nullptr) return PipelineStatus::SkipFrame;
    if (!run.runtime->Initialize(run.device)) {
      // Not fatal here either - the game may not have loaded the DLSS model
      // yet this frame. Log once and keep trying.
      TRACE_EVT(self, "SKIP stage=PostDLSS pass=%u reason=runtime_not_ready",
                self->pass_id_);
      const char* err = run.runtime->GetLastError();
      if (err != nullptr && err[0] != '\0') {
        reshade::log::message(reshade::log::level::warning, err);
      }
      return PipelineStatus::SkipFrame;
    }

    // PreDLSS records onto the game's list against the input Color resource;
    // PostDLSS targets the upscaled output. In both cases dimensions and format
    // come from the game resource this stage will write into.
    run.desc = run.output_resource != nullptr ? run.output_resource->GetDesc()
                                              : D3D12_RESOURCE_DESC{};
  } else {
    // Present resolved the device from the command queue in
    // StepAcquireBackbuffer; all that is left is bringing up the runtime and
    // this session's own list, both fatal here.
    if (run.device == nullptr || run.runtime == nullptr) {
      return PipelineStatus::SkipFrame;
    }
    if (!run.runtime->Initialize(run.device)) {
      return self->FailStep(run.runtime->GetLastError());
    }
    if (!self->command_.Ensure(run.device)) {
      return self->FailStep("failed to create DLSSNR command list");
    }
    self->command_queue_ = run.command_queue;
    run.list = self->command_.GetCommandList();
    run.desc = run.desc;  // already set from the backbuffer
  }

  run.width = static_cast<unsigned int>(run.desc.Width);
  run.height = static_cast<unsigned int>(run.desc.Height);
  return PipelineStatus::Ok;
}

// The engine is initialized once and shared by every pass. A background compile
// means "not ready yet", which is not a failure.
PipelineStatus PassSession::StepEnsureComputeEngine(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepEnsureComputeEngine");
  PassSession* self = run.session;
  if (run.device == nullptr || run.codec == nullptr) {
    return PipelineStatus::SkipFrame;
  }

  if (!run.codec->IsInit() && !run.codec->Init(run.device)) {
    return self->FailStep("failed to initialize compute engine");
  }
  if (!run.codec->IsReady()) {
    TRACE_EVT(self, "SKIP stage=%s pass=%u reason=codec_not_ready",
              run.stage != nullptr ? run.stage->name : "?", self->pass_id_);
    return PipelineStatus::SkipFrame;
  }
  return PipelineStatus::Ok;
}

// ---------------------------------------------------------------------------
// Frame resources and state
// ---------------------------------------------------------------------------

// Compute the per-pass working resolution from the frame extent and the pass's
// working-scale setting.
PipelineStatus PassSession::StepResolveWorkingScale(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepResolveWorkingScale");
  if (!run.pass_bound) return PipelineStatus::SkipFrame;

  const float working_scale =
      std::clamp(run.pass_state.working_scale, 0.10f, 1.0f) *
      (run.pass_state.working_scale_x2 ? 2.0f : 1.0f);
  unsigned int work_w = std::max(
      1u, static_cast<unsigned int>(run.width * working_scale + 0.5f));
  unsigned int work_h = std::max(
      1u, static_cast<unsigned int>(run.height * working_scale + 0.5f));

  // Floor the working resolution: very low resolutions leave temporal
  // history and model input without usable data. Scale both dimensions
  // together so the frame's aspect ratio is preserved.
  if (work_h < kMinWorkingResolutionHeight) {
    const float ratio = static_cast<float>(kMinWorkingResolutionHeight) /
                        static_cast<float>(work_h);
    work_w = std::max(1u, static_cast<unsigned int>(
        static_cast<float>(work_w) * ratio + 0.5f));
    work_h = kMinWorkingResolutionHeight;
  }

  run.work_width = work_w;
  run.work_height = work_h;
  return PipelineStatus::Ok;
}

// Create/resize the per-pass output, composite and working resources.
//
// Always retire replaced resources rather than releasing them immediately.
// Commands from earlier frames may still read them; the controller tags each
// retired item with a queue-ordered Release fence and drains it asynchronously
// after that fence has completed.
PipelineStatus PassSession::StepEnsureFrameResources(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepEnsureFrameResources");
  PassSession* self = run.session;
  if (run.device == nullptr) return PipelineStatus::SkipFrame;

  const bool allow_identity_input = run.pass_bound &&
      !run.grade_input.enabled && run.hdr_state.format != HdrFormat::ScRgb &&
      run.hdr_state.format != HdrFormat::Hdr10;
  if (!self->EnsureFrameResources(run.device, run.desc, run.work_width,
                                  run.work_height, allow_identity_input,
                                  run.grade_input.enabled,
                                  run.fp16_storage)) {
    return self->FailStep(self->reason_);
  }
  return PipelineStatus::Ok;
}

// Register the known frame-boundary state of every pass-owned resource, so
// inference never has to guess for resources we own.
PipelineStatus PassSession::StepSeedResourceStates(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepSeedResourceStates");
  PassSession* self = run.session;
  if (run.cmd == nullptr) return PipelineStatus::SkipFrame;

  if (run.stage != nullptr && !run.stage->owns_submit) {
    // Fresh state tracking per pass: no cross-pass aliasing to rely on.
    self->command_.ClearState();
    self->SeedFrameState(nullptr);

    // `output_resource` belongs to the game (the Color input for PreDLSS,
    // the DLSS output for PostDLSS), and we do not know its exact current
    // state. InferResourceState() guesses from the descriptor flags
    // (UAV-flagged -> UNORDERED_ACCESS, RT-flagged -> RENDER_TARGET, else
    // COMMON), which matches what DLSS produces in practice but is not
    // guaranteed. There is no fully safe assumption for a resource we did not
    // create, so this is best-effort. If a game ships with a mismatched
    // barrier here, the D3D12 debug layer will name it at the CopyResource in
    // WriteToGameResource.
    // The previous PreDLSS pass deliberately leaves Color in SRV for the real
    // NGX consumer, so chained PreDLSS passes start from that known state.
    const D3D12_RESOURCE_STATES initial_state =
        run.stage != nullptr && run.stage->is_pre_dlss && !run.is_first_pass
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : InferResourceState(run.output_resource);
    self->command_.SetResourceState(run.output_resource, initial_state);
  } else {
    self->command_.Reset();
    self->command_.ClearState();
    self->SeedFrameState(run.backbuffer);
  }
  return PipelineStatus::Ok;
}

// Seed the guide resources' tracked state from the snapshot taken once by
// PrepareRun. Do not re-read the global guide state mid-stage: another NGX
// evaluation must not mix its guides with the model input selected at stage
// entry, and retired resources remain alive through the Release fence.
//
// Seeding the guide states belongs here rather than in SeedResourceStates
// because the two stages place it differently relative to the feature build;
// grouping it with the snapshot is what makes one step table serve both.
PipelineStatus PassSession::StepSnapshotGuides(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepSnapshotGuides");
  PassSession* self = run.session;

  // PrepareRun already took the stage's immutable guide snapshot.
  if (run.cmd == nullptr) return PipelineStatus::Ok;

  // The clone copies leave addon-owned guides in NON_PIXEL_SHADER_RESOURCE.
  // Direct depth/motion are already in the DLSS-readable state at this point;
  // seed those expectations explicitly because typeless descriptors are not
  // enough to infer their read-only state. Do not overwrite the output game's
  // state seeded by StepSeedResourceStates: in direct Post-DLSS mode the output
  // guide aliases run.output_resource, which may still be in UAV state.
  if (run.guides_valid) {
    self->command_.SetResourceState(
        run.guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.SetResourceState(
        run.guides.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (run.guides.clone_resources) {
      self->command_.SetResourceState(
          run.guides.dlss_output_color,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }
  return PipelineStatus::Ok;
}

// PostDLSS seeds guide states before model-input resolve rather than after
// the feature build. Its guide pointers/dimensions were snapshotted by
// PrepareRun, just like the other stages.
PipelineStatus PassSession::StepSeedGuideStates(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepSeedGuideStates");
  PassSession* self = run.session;
  if (run.cmd == nullptr) return PipelineStatus::Ok;

  if (run.guides_valid) {
    self->command_.SetResourceState(
        run.guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.SetResourceState(
        run.guides.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (run.guides.clone_resources) {
      self->command_.SetResourceState(
          run.guides.dlss_output_color,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else if (run.stage != nullptr && !run.stage->is_pre_dlss &&
               run.guides.dlss_output_color != nullptr) {
      // CaptureOutput transitioned the direct DLSS output into shader-read
      // state before HdrBeginFrame; don't leave the tracker at the descriptor
      // inference used before that hook-side barrier.
      self->command_.SetResourceState(
          run.guides.dlss_output_color,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }
  return PipelineStatus::Ok;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

// Backbuffer: publish the composed image on the swapchain backbuffer.
PipelineStatus PassSession::StepWriteToBackbuffer(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepWriteToBackbuffer");
  PassSession* self = run.session;
  // Always write the real composed output; the debug override happens in the
  // Release stage and does not affect the game's resources.
  self->command_.CopyResourceToBackbuffer(self->command_.GetCommandList(),
                                    self->resources_.GetCompositeOutput(),
                                    run.backbuffer);
  self->command_.Transition(self->command_.GetCommandList(), run.model_output,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return PipelineStatus::Ok;
}

// PreDLSS/PostDLSS: publish the composed image into the game's resource.
PipelineStatus PassSession::StepWriteToGameResource(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepWriteToGameResource");
  PassSession* self = run.session;
  if (run.output_resource == nullptr) return PipelineStatus::SkipFrame;

  // Always write the real composed output to the game's resource. The debug
  // image is applied to the backbuffer at Present only.
  TRACE_CMD(self, "write_to_game_resource src=%d dst=%d",
            trace::ResourceId(self->resources_.GetCompositeOutput()),
            trace::ResourceId(run.output_resource));
  self->command_.Transition(run.list, self->resources_.GetCompositeOutput(),
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_RESOURCE_STATES out_state =
      (run.stage != nullptr && run.stage->is_pre_dlss)
          ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
          : InferResourceState(run.output_resource);
  self->command_.Transition(run.list, run.output_resource,
                      D3D12_RESOURCE_STATE_COPY_DEST);
  run.list->CopyResource(run.output_resource, self->resources_.GetCompositeOutput());
  self->command_.Transition(run.list, run.output_resource, out_state);
  self->command_.Transition(run.list, self->resources_.GetCompositeOutput(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // Leave pass-owned resources in the state SeedFrameState() expects.
  self->CleanupFrameState(run.list);
  return PipelineStatus::Ok;
}

// Backbuffer: return pass-owned resources to their frame-boundary states,
// submit with a queue fence, and return without blocking the CPU. The fence is
// polled later; allocator reuse waits only when this session's ring slot wraps.
PipelineStatus PassSession::StepSubmitBackbuffer(PassRun& run) {
  DLSSNR_PROFILE_ASYNC_L2("StepSubmitBackbuffer");
  PassSession* self = run.session;
  self->CleanupFrameState(self->command_.GetCommandList());
  self->SubmitBackbuffer();

  // Backbuffer reports the NGX result after the frame is on the GPU: the frame
  // is never dropped because the model failed, but the pass is marked so the
  // user sees it.
  //
  // The numeric result is logged as well as named. "EvaluateFeature failed" on
  // its own is not diagnosable - the code distinguishes a missing input from a
  // bad parameter from an unsupported format, and those need different fixes.
  if (run.eval_result != NVSDK_NGX_Result_Success) {
    // Re-print the descriptors when the evaluation step suppressed them as
    // unchanged. A failure is exactly the case where the line matters, and a
    // pass that fails the same way every frame would otherwise go silent after
    // its first frame - the steady state the change check exists for is also
    // the state a persistent failure gets stuck in.
    if (!self->eval_log_written_this_frame_ && self->eval_log_key_valid_) {
      char line[384] = {};
      FormatEvalLine(run, self->eval_log_key_, line, sizeof(line));
      reshade::log::message(reshade::log::level::error, line);
      TRACE_EVT(self, "%s", line);
    }

    char reason[160] = {};
    snprintf(reason, sizeof(reason), "DLSSNR EvaluateFeature failed: %s (0x%08X)",
             NgxResultName(static_cast<NVSDK_NGX_Result>(run.eval_result)),
             static_cast<unsigned int>(run.eval_result));
    return self->FailStep(reason);
  }
  return PipelineStatus::Ok;
}

// ---------------------------------------------------------------------------
// Step table
// ---------------------------------------------------------------------------

constexpr PipelineNodeId kNoDeps[] = {PipelineNodeId::Count};

// Step table.
//
// The `required_inputs` field is a claim about which frame inputs the step
// dereferences. The runner checks the stage's declaration against the live
// PassRun at stage entry, and StageInputsCoverSteps (asserted below) proves
// each stage declares at least what its steps need.
//
// Only *reads* are declared. A step that assigns run.device or run.backbuffer
// is producing those for later steps, not consuming them, so requiring them
// would be wrong - AcquireBackbuffer writes both, and the steps that read them
// run after it.
constexpr PipelineNodeId kTemporalPrepareRequires[] = {
    PipelineNodeId::GradeModelInput,
    PipelineNodeId::SnapshotGuides,
    PipelineNodeId::Count};

constexpr PipelineStepDef kSessionSteps[] = {
    // -- setup --------------------------------------------------------------
    // Produces run.device and run.backbuffer; consumes the swapchain and the
    // queue it records the acquire on.
    {PipelineNodeId::AcquireBackbuffer, "AcquireBackbuffer",
     &PassSession::StepAcquireBackbuffer, kNoDeps, StageInput::None, false},
    // Reads the run's own carriers and writes run.list. In PostDLSS run.list
    // is already the game's list; this only resolves it for Backbuffer.
    {PipelineNodeId::ResolveDevice, "ResolveDevice",
     &PassSession::StepResolveDevice, kNoDeps, StageInput::Stage, false},
    {PipelineNodeId::EnsureComputeEngine, "EnsureComputeEngine",
     &PassSession::StepEnsureComputeEngine, kNoDeps, StageInput::None, false},
    {PipelineNodeId::ResolveWorkingScale, "ResolveWorkingScale",
     &PassSession::StepResolveWorkingScale, kNoDeps, StageInput::None, false},
    {PipelineNodeId::EnsureFrameResources, "EnsureFrameResources",
     &PassSession::StepEnsureFrameResources, kNoDeps, StageInput::None, false},
    {PipelineNodeId::SeedResourceStates, "SeedResourceStates",
     &PassSession::StepSeedResourceStates, kNoDeps, StageInput::None, false},
    {PipelineNodeId::SeedGuideStates, "SeedGuideStates",
     &PassSession::StepSeedGuideStates, kNoDeps, StageInput::None, false},

    // -- model input --------------------------------------------------------
    {PipelineNodeId::ResolveModelInput, "ResolveModelInput",
     &PassSession::StepResolveModelInput, kNoDeps, StageInput::None, false},
    {PipelineNodeId::BuildHdrUniforms, "BuildHdrUniforms",
     &PassSession::StepBuildHdrUniforms, kNoDeps, StageInput::None, true},
    {PipelineNodeId::GainMapSplit, "GainMapSplit",
     &PassSession::StepGainMapSplit, kNoDeps, StageInput::None, true},

    // -- model evaluation ---------------------------------------------------
    {PipelineNodeId::EnsureFeature, "EnsureFeature", &PassSession::StepEnsureFeature,
     kNoDeps, StageInput::None, false},
    {PipelineNodeId::SnapshotGuides, "SnapshotGuides",
     &PassSession::StepSnapshotGuides, kNoDeps, StageInput::None, false},
    {PipelineNodeId::PrepareModelInput, "PrepareModelInput",
     &PassSession::StepPrepareModelInput, kNoDeps, StageInput::None, false},
    {PipelineNodeId::BuildEffectiveLocalExposure, "BuildEffectiveLocalExposure",
     &PassSession::StepBuildEffectiveLocalExposure, kNoDeps, StageInput::None, false},
    // Records through StageList(), which is the session's own list in Backbuffer
    // and the game's in PostDLSS, so like RenderDebugView it declares no
    // command list: run.list is not what it reads in Backbuffer.
    {PipelineNodeId::GradeModelInput, "GradeModelInput",
     &PassSession::StepGradeModelInput, kNoDeps, StageInput::None, false},
    {PipelineNodeId::TemporalPrepare, "TemporalPrepare",
     &PassSession::StepTemporalPrepare, kTemporalPrepareRequires,
     StageInput::Stage, false},
    {PipelineNodeId::FillEvaluateParams, "FillEvaluateParams",
     &PassSession::StepFillEvaluateParams, kNoDeps, StageInput::None, false},
    {PipelineNodeId::EvaluateFeature, "EvaluateFeature",
     &PassSession::StepEvaluateFeature, kNoDeps, StageInput::None, false},
    {PipelineNodeId::ModelOutputBarrier, "ModelOutputBarrier",
     &PassSession::StepModelOutputBarrier, kNoDeps, StageInput::None, false},

    // -- composition --------------------------------------------------------
    {PipelineNodeId::ComposeModelOutput, "ComposeModelOutput",
     &PassSession::StepComposeModelOutput, kNoDeps, StageInput::None, false},
    // Grades the model output in place, in the model's sRGB-encoded SDR
    // domain, before the compose step (and everything after it) reads it.
    {PipelineNodeId::GradeModelOutput, "GradeModelOutput",
     &PassSession::StepGradeModelOutput, kNoDeps, StageInput::None, false},

    // -- output -------------------------------------------------------------
    {PipelineNodeId::RenderDebugView, "RenderDebugView",
     &PassSession::StepRenderDebugView, kNoDeps, StageInput::None, false},
    {PipelineNodeId::WriteToBackbuffer, "WriteToBackbuffer",
     &PassSession::StepWriteToBackbuffer, kNoDeps, StageInput::None, false},
    // Reads run.list directly. Only safe because it is listed in PostDLSS,
    // whose call site supplies the game's command list; StageInputsCoverSteps
    // below turns moving it into Backbuffer into a build error.
    {PipelineNodeId::WriteToGameResource, "WriteToGameResource",
     &PassSession::StepWriteToGameResource, kNoDeps,
     StageInput::CommandList | StageInput::OutputResource, false},
    {PipelineNodeId::SubmitBackbuffer, "SubmitBackbuffer", &PassSession::StepSubmitBackbuffer,
     kNoDeps, StageInput::None, false},
};

// Every prerequisite these steps declare must be satisfiable, which means
// listed earlier in the stage they run in. Getting this wrong does not fail to
// build and does not crash: the runner logs at error level and skips the step,
// every frame, forever. These assertions make it a build error instead.
//
// Both stage lists live in pipeline_graph.h so a translation unit that defines
// a step can check itself without depending on the stage table in
// pipeline_graph.cpp.
static_assert(PrerequisitesPrecedeSteps(kPresentStepNodes,
                                        ArrayCount(kPresentStepNodes),
                                        kSessionSteps,
                                        ArrayCount(kSessionSteps)),
              "a session step in the Backbuffer stage declares a prerequisite "
              "that runs after it");
static_assert(PrerequisitesPrecedeSteps(kPreDlssStepNodes,
                                        ArrayCount(kPreDlssStepNodes),
                                        kSessionSteps,
                                        ArrayCount(kSessionSteps)),
              "a session step in the PreDLSS stage declares a prerequisite "
              "that runs after it");
static_assert(PrerequisitesPrecedeSteps(kPostDlssStepNodes,
                                        ArrayCount(kPostDlssStepNodes),
                                        kSessionSteps,
                                        ArrayCount(kSessionSteps)),
              "a session step in the PostDLSS stage declares a prerequisite "
              "that runs after it");

// The Backbuffer stage is not HDR-capable, so no step listed there may be marked
// `requires_hdr_aware`: the runner would skip it silently.
//
// PrepareModelInput and ComposeModelOutput are not HDR-only: they pick the HDR
// shader when
// `hdr_uniforms_ready` is set and the SDR shader otherwise. Only BuildHdrUniforms
// genuinely needs `run.hdr`, and it is listed in PostDLSS alone.
static_assert(NoHdrAwareSteps(kPresentStepNodes, ArrayCount(kPresentStepNodes),
                              kSessionSteps, ArrayCount(kSessionSteps)),
              "a step in the Backbuffer stage is marked requires_hdr_aware, but "
              "Backbuffer is not HDR-capable so it would never run");

// Every frame input a step declares must be one its stage's call site
// actually supplies.
//
// This is the guard for the null-dereference class of table edit: the steps
// are shared between stages, and the two call sites pass different things
// (Backbuffer gets a swapchain and no command list, game-resource stages the reverse), so
// moving one line between the step tables is enough to hand a step a null
// pointer it dereferences. StepWriteToGameResource reads run.list, and reaches
// it safely today only because it is listed in PostDLSS.
//
// The declaration on the step is the part the compiler cannot verify, so a
// step that reads an input without declaring it still passes this check. What
// it does verify is that no step is listed in a stage that cannot supply it.
static_assert(
    StageInputsCoverSteps(kPreDlssStepNodes, ArrayCount(kPreDlssStepNodes),
                          kSessionSteps, ArrayCount(kSessionSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "a session step in the PreDLSS stage needs an input its call site does not supply");
static_assert(
    StageInputsCoverSteps(kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes),
                          kSessionSteps, ArrayCount(kSessionSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "a session step in the PostDLSS stage needs an input the PostDLSS call "
    "site does not supply");

static_assert(
    StageInputsCoverSteps(kPresentStepNodes, ArrayCount(kPresentStepNodes),
                          kSessionSteps, ArrayCount(kSessionSteps),
                          StageInput::Swapchain | StageInput::CommandQueue |
                              StageInput::Stage),
    "a session step in the Backbuffer stage needs an input the Backbuffer call site "
    "does not supply");

// The dispatch ring has to hold one whole frame at the maximum pass count: its
// descriptors and constants are written when the command is recorded and read
// when it executes, so a slot reused too early hands a pending dispatch another
// dispatch's resources. See kDescriptorRingSize in compute_engine.h.
// +3, not +2: the histogram contributes two and the Release stage's
// backbuffer grade contributes one. Leaving the Release grade out sized the
// ring for fifteen frames of work rather than sixteen, which is what
// kMaxFramesInFlight promises.
static_assert(kMaxDispatchesPerFrame >= kMaxPasses * 9 + 4 + 3,
              "kMaxDispatchesPerFrame no longer covers a frame at kMaxPasses "
              "with the extra Exposure Mask build dispatch - raise it, and kDescriptorRingSize with it");

const PipelineStepDef* SessionStepFor(PipelineNodeId id) {
  return FindStepDef(kSessionSteps, id);
}

}  // namespace reshade_dlssnr
