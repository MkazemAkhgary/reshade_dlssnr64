#include "pass_session.h"

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
#include "feature/importance_mask/view.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "model/model_pipeline.h"
#include "model/ngx_model_params.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"
#include "importance_mask/importance_mask.h"
#include "streamline_compat.h"

namespace reshade_dlssnr {

namespace {

template <typename T>
void HashTemporalValue(uint64_t* hash, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&value);
  for (size_t i = 0; i < sizeof(T); ++i) {
    *hash ^= bytes[i];
    *hash *= 1099511628211ull;
  }
}

uint64_t TemporalSettingsSignature(const PassRun& run) {
  uint64_t hash = 1469598103934665603ull;
  const auto& p = run.pass_state;
  HashTemporalValue(&hash, run.width);
  HashTemporalValue(&hash, run.height);
  HashTemporalValue(&hash, run.work_width);
  HashTemporalValue(&hash, run.work_height);
  HashTemporalValue(&hash, p.target);
  HashTemporalValue(&hash, p.preset);
  HashTemporalValue(&hash, p.working_scale);
  HashTemporalValue(&hash, p.intensity);
  HashTemporalValue(&hash, p.temporal_photometric_abs_threshold);
  HashTemporalValue(&hash, p.temporal_photometric_rel_threshold);
  HashTemporalValue(&hash, p.temporal_depth_abs_threshold);
  HashTemporalValue(&hash, p.temporal_depth_rel_threshold);
  HashTemporalValue(&hash, p.temporal_max_displacement);
  HashTemporalValue(&hash, p.temporal_max_invalid_percent);
  HashTemporalValue(&hash, p.temporal_fp16_history);
  HashTemporalValue(&hash, p.local_tone);
  HashTemporalValue(&hash, p.local_structure);
  HashTemporalValue(&hash, p.skin_structure);
  HashTemporalValue(&hash, p.auto_mask);
  HashTemporalValue(&hash, p.ui_correction);
  const auto& g = run.grade_input;
  HashTemporalValue(&hash, g.enabled);
  HashTemporalValue(&hash, g.luma);
  HashTemporalValue(&hash, g.chroma);
  HashTemporalValue(&hash, g.luma_mode);
  HashTemporalValue(&hash, g.chroma_mode);
  HashTemporalValue(&hash, g.black_level);
  HashTemporalValue(&hash, g.white_level);
  HashTemporalValue(&hash, g.contrast);
  HashTemporalValue(&hash, g.shadows);
  HashTemporalValue(&hash, g.highlights);
  // Only include stable HDR interpretation parameters. Frame-varying exposure
  // measurements are deliberately excluded so adaptation does not reset history.
  HashTemporalValue(&hash, run.hdr_state.format);
  HashTemporalValue(&hash, run.hdr_state.bt2446_k1);
  HashTemporalValue(&hash, run.hdr_state.bt2446_k2);
  HashTemporalValue(&hash, run.hdr_state.bt2446_k3);
  HashTemporalValue(&hash, run.hdr_state.bt2446_k4);
  HashTemporalValue(&hash, run.hdr_state.bt2446_crosstalk);
  HashTemporalValue(&hash, run.hdr_uniforms.format);
  HashTemporalValue(&hash, run.hdr_uniforms.local_exposure_strength);
  return hash;
}

float TemporalReadInvalidFraction(GpuReadback& readback, ID3D12Fence* fence,
                                  unsigned int width, unsigned int height,
                                  bool* out_valid) {
  if (out_valid != nullptr) *out_valid = false;
  uint32_t bytes = 0;
  const void* data = readback.TryGet(fence, &bytes);
  if (data == nullptr || bytes < sizeof(uint32_t) * 2 || width == 0 || height == 0)
    return 1.0f;
  const auto* values = static_cast<const uint32_t*>(data);
  if (out_valid != nullptr) *out_valid = values[1] != 0;
  return std::min(1.0f, static_cast<float>(values[0]) /
                           static_cast<float>(static_cast<uint64_t>(width) * height));
}

}  // namespace

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

void BindLocalExposureInputs(const PassRun& run, ComputeConstants& constants,
                             ID3D12Resource* source, ID3D12Resource** srvs) {
  constants.hdr.LocalExposureParams.x = 4.0f;
  constants.hdr.LocalExposureParams.y = 4.0f;
  ID3D12Resource* luma = run.resources != nullptr
      ? run.resources->GetLocalExposureLuma() : nullptr;
  const bool use_precomputed = run.hdr_uniforms.local_precomputed_luma && luma != nullptr;
  constants.hdr.LocalExposureParams.z = use_precomputed ? 1.0f : 0.0f;

  srvs[0] = source;
  srvs[1] = source;
  srvs[2] = source;
  srvs[3] = use_precomputed ? luma : source;
}

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

void PassSession::RetireResourceCallback(ID3D12Resource* resource,
                                         void* user_data) {
  if (resource == nullptr) return;
  auto* guides = static_cast<DlssGuides*>(user_data);
  if (guides != nullptr) {
    // Transfer one reference into the deferred-release queue. The controller
    // tags it with a queue-ordered fence and releases it only after completion.
    resource->AddRef();
    guides->RetireGameResource(resource);
    resource->Release();
  } else {
    resource->Release();
  }
}

bool PassSession::EnsureFrameResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int work_width,
    unsigned int work_height,
    bool allow_identity_input) {
  if (!resources_.EnsureCompositeOutput(device, desc)) {
    reason_ = "failed to create DLSSNR composite resource";
    return false;
  }

  // HDR model-domain values may legitimately exceed 1.0. Keep the NGX-facing
  // input/output and EASU/RCAS working textures in FP32 so super-white values
  // retain enough precision for the inverse model-domain transfer. The packed
  // carrier is also RGBA32F; the final published composite uses the game's
  // native format and therefore retains the format's inherent representable
  // range and quantization.
  D3D12_RESOURCE_DESC model_desc = desc;
  const HdrFormat frame_format = DetectFormatFromDxgi(desc.Format);
  const bool hdr_frame = frame_format == HdrFormat::ScRgb ||
                         frame_format == HdrFormat::Hdr10;
  if (hdr_frame) {
    model_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  }

  const bool identity_input = allow_identity_input && !hdr_frame &&
      desc.Width == work_width && desc.Height == work_height;
  if (identity_input) {
    resources_.ReleaseOutput();
  } else if (!resources_.EnsureOutput(device, model_desc)) {
    reason_ = "failed to create DLSSNR model output";
    return false;
  }
  if (!resources_.EnsureWorkingResources(device, model_desc,
                                         work_width, work_height,
                                         !identity_input)) {
    reason_ = "failed to create DLSSNR working resources";
    return false;
  }

  if (hdr_frame) {
    if (!resources_.EnsurePackedOutput(device, desc) ||
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

  if (guides.valid) {
    block.SetResource("DLSSNR.Depth", guides.depth);
    block.SetResource("DLSSNR.MVec", guides.motion);
    block.SetUInt("DLSSNR.DepthInverted", guides.depth_inverted ? 1u : 0u);
    block.SetFloat("DLSSNR.MVecScaleX", guides.mv_scale_x);
    block.SetFloat("DLSSNR.MVecScaleY", guides.mv_scale_y);

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
    block.SetUInt("DLSSNR.DepthSubrectWidth", guides.guide_width);
    block.SetUInt("DLSSNR.DepthSubrectHeight", guides.guide_height);
    block.SetUInt("DLSSNR.MVecSubrectBaseX", 0u);
    block.SetUInt("DLSSNR.MVecSubrectBaseY", 0u);
    block.SetUInt("DLSSNR.MVecSubrectWidth", guides.motion_width);
    block.SetUInt("DLSSNR.MVecSubrectHeight", guides.motion_height);
  }

  block.SetUInt("DLSSNR.Reset", feature_.ShouldReset() ? 1u : 0u);
  feature_.MarkEvaluated();

  return true;
}

void PassSession::SeedFrameState(ID3D12Resource* backbuffer) {
  if (backbuffer != nullptr) {
    command_.SetResourceState(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
  }
  command_.SetResourceState(resources_.GetOutput(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_.SetResourceState(resources_.GetCompositeOutput(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_.SetResourceState(resources_.GetWorkingInput(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_.SetResourceState(resources_.GetWorkingOutput(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetUnliftedWorkingInput() != nullptr) {
    command_.SetResourceState(resources_.GetUnliftedWorkingInput(),
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (resources_.GetPackedOutput() != nullptr) {
    command_.SetResourceState(resources_.GetPackedOutput(),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (resources_.GetEffectiveExposureMap() != nullptr) {
    command_.SetResourceState(resources_.GetEffectiveExposureMap(),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (resources_.GetGainStats() != nullptr) {
    command_.SetResourceState(resources_.GetGainStats(),
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (resources_.GetTemporalPreviousInput() != nullptr)
    command_.SetResourceState(resources_.GetTemporalPreviousInput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (resources_.GetTemporalPreviousEdit() != nullptr)
    command_.SetResourceState(resources_.GetTemporalPreviousEdit(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalPreviousDepth() != nullptr)
    command_.SetResourceState(resources_.GetTemporalPreviousDepth(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalCandidate() != nullptr)
    command_.SetResourceState(resources_.GetTemporalCandidate(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalValidatedCandidate() != nullptr)
    command_.SetResourceState(resources_.GetTemporalValidatedCandidate(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalStats() != nullptr)
    command_.SetResourceState(resources_.GetTemporalStats(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void PassSession::CleanupFrameState(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;
  command_.Transition(command_list, resources_.GetOutput(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_.Transition(command_list, resources_.GetCompositeOutput(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_.Transition(command_list, resources_.GetWorkingInput(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command_.Transition(command_list, resources_.GetWorkingOutput(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetUnliftedWorkingInput() != nullptr) {
    command_.Transition(command_list, resources_.GetUnliftedWorkingInput(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (resources_.GetPackedOutput() != nullptr) {
    command_.Transition(command_list, resources_.GetPackedOutput(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (resources_.GetEffectiveExposureMap() != nullptr) {
    command_.Transition(command_list, resources_.GetEffectiveExposureMap(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (resources_.GetGainStats() != nullptr) {
    command_.Transition(command_list, resources_.GetGainStats(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (resources_.GetTemporalPreviousInput() != nullptr)
    command_.Transition(command_list, resources_.GetTemporalPreviousInput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (resources_.GetTemporalPreviousEdit() != nullptr)
    command_.Transition(command_list, resources_.GetTemporalPreviousEdit(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalPreviousDepth() != nullptr)
    command_.Transition(command_list, resources_.GetTemporalPreviousDepth(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalCandidate() != nullptr)
    command_.Transition(command_list, resources_.GetTemporalCandidate(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalValidatedCandidate() != nullptr)
    command_.Transition(command_list, resources_.GetTemporalValidatedCandidate(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (resources_.GetTemporalStats() != nullptr)
    command_.Transition(command_list, resources_.GetTemporalStats(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void PassSession::SubmitBackbuffer() {
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
  // is owned by FrameDebugContext and lives across frames.
  if (frame_debug.output == nullptr ||
      frame_debug.width != target_width ||
      frame_debug.height != target_height ||
      frame_debug.format != target_format) {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
        device == nullptr) {
      return false;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = target_width;
    desc.Height = target_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = target_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12Resource> new_output;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&new_output)))) {
      return false;
    }

    frame_debug.output = std::move(new_output);
    frame_debug.width = target_width;
    frame_debug.height = target_height;
    frame_debug.format = target_format;
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
  std::lock_guard<std::mutex> lock(g_session_mutex);

  temporal_readback_.Shutdown();
  temporal_history_valid_ = false;
  temporal_feedback_valid_ = false;
  temporal_invalid_fraction_ = 1.0f;
  temporal_last_refresh_frame_ = 0;
  temporal_last_frame_id_ = 0;
  temporal_last_guide_serial_ = 0;
  temporal_settings_signature_ = 0;
  temporal_last_mode_ = 0;
  temporal_guide_width_ = temporal_guide_height_ = 0;
  temporal_motion_width_ = temporal_motion_height_ = 0;
  temporal_guide_base_x_ = temporal_guide_base_y_ = 0;
  temporal_guide_subrect_width_ = temporal_guide_subrect_height_ = 0;
  temporal_motion_base_x_ = temporal_motion_base_y_ = 0;
  temporal_motion_subrect_width_ = temporal_motion_subrect_height_ = 0;
  temporal_reference_width_ = temporal_reference_height_ = 0;
  temporal_depth_format_ = temporal_motion_format_ = DXGI_FORMAT_UNKNOWN;
  temporal_mv_scale_x_ = temporal_mv_scale_y_ = 1.0f;
  feature_.Shutdown(runtime);
  resources_.Shutdown();
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
                             CommandContext* hdr_cmd,
                             ImportanceMask* importance_mask,
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
  run.hdr_cmd = hdr_cmd;
  run.importance_mask = importance_mask;
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

    // HDR and the importance mask are single-instance, so their snapshots do
    // not depend on the pass index; the fixed key is the same one their
    // sections are drawn with.
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    run.debug_view = feature::debug_log::Read(store, shared);
    run.hdr_state = feature::hdr::Read(store, shared);
    run.mask_state = feature::importance_mask::Read(store, shared);
  }

  run.list = command_list;
  run.output_resource = output_resource;
  run.command_queue = command_queue;
  run.swapchain = swapchain;

  run.guides = guides_->GetState();
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
                                     CommandContext* hdr_cmd,
                                     ImportanceMask* importance_mask,
                                     ID3D12Resource* game_exposure_texture,
                                     ID3D12CommandQueue* command_queue,
                                     IDXGISwapChain* swapchain,
                                     feature::pass::PassId pass_id,
                                     uint64_t frame_id,
                                     ID3D12Fence* temporal_feedback_fence,
                                     uint64_t temporal_feedback_fence_value) {
  // The pre-refactor entry points were serialized against each other with this
  // mutex; the runner walk replaces them, so it has to hold the same lock.
  std::lock_guard<std::mutex> lock(g_session_mutex);

  // Guards common to both stages. They were the first four lines of each
  // stage method. A disabled addon or a failed pass does nothing at all; a
  // missing input is likewise not an error, just nothing to do.
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
             is_first_pass, frame_debug, hdr, hdr_cmd, importance_mask,
             game_exposure_texture, command_queue, swapchain, pass_id,
             frame_id, temporal_feedback_fence,
             temporal_feedback_fence_value);

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
// These are the extracted bodies of the steps that used to be inline in
// OnPresent and OnPostDlssEvaluate. The step order lives in
// runtime/pipeline_graph.cpp; this file says how each step runs.
//
// They are static members of PassSession rather than free functions because
// they are the session's own work: they touch resources_, feature_, command_
// and guides_ directly, which is exactly what a free function could not do
// without a dozen accessors that would only exist to defeat encapsulation.
// The step signature is the same either way — a static member converts to the
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
  if (reason == nullptr && reason_ != nullptr && reason_[0] != '\0') {
    Fail(reason_);
    return;
  }
  Fail(reason != nullptr ? reason : "DLSSNR pass failed");
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

// Backbuffer only: acquire the swapchain backbuffer and the device behind the
// command queue. Replaces the block at the top of the old OnPresent.
PipelineStatus PassSession::StepAcquireBackbuffer(PassRun& run) {
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
// command list. The difference in failure handling is deliberate and matches
// the pre-refactor code: Present treats both failures as fatal, PostDLSS
// treats a missing device as transient (the game may not have loaded the DLSS
// model yet) and a failing runtime.Initialize as transient too.
PipelineStatus PassSession::StepResolveDevice(PassRun& run) {
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
      // Not fatal here either — the game may not have loaded the DLSS model
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
      self->FailForStep(run.runtime->GetLastError());
      return PipelineStatus::Fail;
    }
    if (!self->command_.Ensure(run.device)) {
      self->FailForStep("failed to create DLSSNR command list");
      return PipelineStatus::Fail;
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
  PassSession* self = run.session;
  if (run.device == nullptr || run.codec == nullptr) {
    return PipelineStatus::SkipFrame;
  }

  if (!run.codec->IsInit() && !run.codec->Init(run.device)) {
    self->FailForStep("failed to initialize compute engine");
    return PipelineStatus::Fail;
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
  PassSession* self = run.session;
  if (!run.pass_bound) return PipelineStatus::SkipFrame;

  const float working_scale =
      std::clamp(run.pass_state.working_scale, 0.10f, 1.0f);
  run.work_width = std::max(
      1u, static_cast<unsigned int>(run.width * working_scale + 0.5f));
  run.work_height = std::max(
      1u, static_cast<unsigned int>(run.height * working_scale + 0.5f));
  return PipelineStatus::Ok;
}

// Create/resize the per-pass output, composite and working resources.
//
// Always retire replaced resources rather than releasing them immediately.
// Commands from earlier frames may still read them; the controller tags each
// retired item with a queue-ordered Release fence and drains it asynchronously
// after that fence has completed.
PipelineStatus PassSession::StepEnsureFrameResources(PassRun& run) {
  PassSession* self = run.session;
  if (run.device == nullptr) return PipelineStatus::SkipFrame;

  self->resources_.SetRetireCallback(&PassSession::RetireResourceCallback, self->guides_);

  const bool allow_identity_input = run.pass_bound &&
      !run.grade_input.enabled && run.hdr_state.format != HdrFormat::ScRgb &&
      run.hdr_state.format != HdrFormat::Hdr10;
  if (!self->EnsureFrameResources(run.device, run.desc, run.work_width,
                                  run.work_height, allow_identity_input)) {
    self->FailForStep(self->reason_);
    return PipelineStatus::Fail;
  }
  return PipelineStatus::Ok;
}

// Register the known frame-boundary state of every pass-owned resource, so
// inference never has to guess for resources we own.
PipelineStatus PassSession::StepSeedResourceStates(PassRun& run) {
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
// Model input
// ---------------------------------------------------------------------------

// Select the image the model consumes. Backbuffer reads the swapchain image;
// PreDLSS reads the game's Color input; PostDLSS reads the captured DLSS
// output. Chained game-resource passes snapshot the previous result.
PipelineStatus PassSession::StepResolveModelInput(PassRun& run) {
  PassSession* self = run.session;
  if (run.stage != nullptr && run.stage->owns_submit) {
    run.model_input = run.backbuffer;
    if (run.model_input != nullptr) {
      self->command_.Transition(self->command_.GetCommandList(), run.model_input,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    return PipelineStatus::Ok;
  }

  if (run.stage != nullptr && run.stage->is_pre_dlss) {
    if (run.output_resource == nullptr) return PipelineStatus::SkipFrame;
    run.model_input = run.output_resource;
    return PipelineStatus::Ok;
  }

  // PostDLSS: first pass reads either the game output or its optional clone.
  if (run.is_first_pass) {
    if (run.guides.dlss_output_color == nullptr) {
      TRACE_EVT(self, "SKIP stage=PostDLSS pass=%u reason=no_guide_color",
                self->pass_id_);
      return PipelineStatus::SkipFrame;
    }
    run.model_input = run.guides.dlss_output_color;
    // A cloned output is known to be an SRV. In direct mode this aliases the
    // game's output_resource, whose state was seeded from its descriptor in
    // StepSeedResourceStates; preserve that state instead of overwriting it.
    if (run.guides.clone_resources) {
      self->command_.SetResourceState(
          run.model_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    return PipelineStatus::Ok;
  }

  // Chained pass: snapshot the previous result into scratch slot 0.
  if (run.device == nullptr) return PipelineStatus::SkipFrame;
  constexpr uint32_t kSnapshotSlot = kInputSnapshotSlot;
  if (!self->resources_.EnsureScratch(run.device, run.desc, run.width, run.height,
                                kSnapshotSlot)) {
    // Pre-refactor set failed_ directly here without a reason string; keep
    // that shape but give it a reason so the status UI is not blank.
    self->FailForStep("failed to create input snapshot");
    TRACE_EVT(self, "FAIL pass=%u reason=ensure_scratch", self->pass_id_);
    return PipelineStatus::Fail;
  }
  ID3D12Resource* scratch = self->resources_.GetScratch(kSnapshotSlot);
  TRACE_CMD(self, "snapshot_input scratch=%d from=%d",
            trace::ResourceId(scratch), trace::ResourceId(run.output_resource));
  self->command_.Transition(run.list, run.output_resource,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
  self->command_.Transition(run.list, scratch, D3D12_RESOURCE_STATE_COPY_DEST);
  run.list->CopyResource(scratch, run.output_resource);
  self->command_.Transition(run.list, scratch,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  run.model_input = scratch;
  return PipelineStatus::Ok;
}


// ---------------------------------------------------------------------------
// Model evaluation
// ---------------------------------------------------------------------------

// Build the full-resolution combined local-exposure map used by HDR inverse compose. Working
// scale only affects the per-frame evaluation size, which FillEvaluateParams
// sets on every frame, so changing working scale never triggers a rebuild. A
// rebuild happens only when the swapchain resolution itself changes.
PipelineStatus PassSession::StepEnsureFeature(PassRun& run) {
  PassSession* self = run.session;
  if (run.runtime == nullptr || !run.pass_bound) {
    return PipelineStatus::SkipFrame;
  }

  // The list this stage records on: its own for Backbuffer, the game's for
  // PreDLSS and PostDLSS.
  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);

  // Brackets the call so a crash inside it is attributable rather than just
  // "somewhere in this step".
  TRACE_EVT(self, "ensure_feature stage=%s pass=%u list=%p %ux%u",
            run.stage != nullptr ? run.stage->name : "?", self->pass_id_,
            static_cast<const void*>(list), run.width, run.height);

  if (!self->feature_.EnsureFeature(*run.runtime, run.pass_state, list,
                                    run.width, run.height)) {
    self->FailForStep(self->feature_.GetLastError());
    if (run.stage != nullptr && run.stage->owns_submit) {
      // Backbuffer owns the list, so it has to close it; game-resource stages
      // must leave the game's list alone.
      list->Close();
    }
    return PipelineStatus::Fail;
  }

  if (self->feature_.ConsumeJustRebuilt()) {
    // The feature was just created or rebuilt; skip evaluation this frame so
    // the model has a frame to fully initialize.
    TRACE_EVT(self, "SKIP stage=%s pass=%u reason=feature_rebuilt",
              run.stage != nullptr ? run.stage->name : "?", self->pass_id_);
    if (run.stage != nullptr && run.stage->owns_submit) {
      self->SubmitBackbuffer();
    }
    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created/rebuilt; skipping evaluation this frame");
    return PipelineStatus::SkipFrame;
  }
  return PipelineStatus::Ok;
}

PipelineStatus PassSession::StepGainMapSplit(PassRun& run) {
  PassSession* self = run.session;
  if (self == nullptr || run.codec == nullptr || run.list == nullptr ||
      !run.hdr_uniforms_ready || run.hdr == nullptr ||
      run.hdr->ResolvedFormat() == HdrFormat::Sdr) {
    return PipelineStatus::Ok;
  }

  ID3D12Resource* source = run.model_input;
  ID3D12Resource* packed = self->resources_.GetPackedOutput();
  ID3D12Resource* stats = self->resources_.GetGainStats();
  if (source == nullptr || packed == nullptr) {
    self->FailForStep("HDR gain-map resources are unavailable");
    return PipelineStatus::Fail;
  }

  ComputeConstants constants = {};
  constants.Width = run.width;
  constants.Height = run.height;
  constants.hdr.Format = run.hdr_uniforms.format;
  constants.hdr.Exposure = run.hdr_uniforms.exposure;
  constants.hdr.Bt2446K1 = run.hdr_uniforms.bt2446_k1;
  constants.hdr.Bt2446K2 = run.hdr_uniforms.bt2446_k2;
  // k3 remains a persisted BT.2446C construction parameter for trace/UI parity,
  // but the direct Method-C evaluation uses k1, k2 and k4 plus Y_ip = k4/k1.
  constants.hdr.Bt2446K4 = run.hdr_uniforms.bt2446_k4;
  constants.hdr.Crosstalk = run.hdr_uniforms.bt2446_crosstalk;
  constants.hdr.GainMinLog2 = run.hdr_uniforms.gain_min_log2;
  constants.hdr.GainMaxLog2 = run.hdr_uniforms.gain_max_log2;
  constants.hdr.EffectiveSceneWhiteNits = run.hdr_uniforms.effective_scene_white_nits;
  constants.hdr.LocalExposure.x = run.hdr_uniforms.local_exposure_strength;
  constants.hdr.LocalExposure.y = run.hdr_uniforms.local_max_brighten;
  constants.hdr.LocalExposure.z = run.hdr_uniforms.local_max_darken;
  constants.hdr.LocalExposure.w = run.hdr_uniforms.midtone;

  if (run.hdr_uniforms.local_exposure_strength > 0.0f &&
      run.hdr_uniforms.local_precomputed_luma) {
    ID3D12Resource* luma = nullptr;
    if (!self->resources_.EnsureLocalExposureLuma(run.device, run.width, run.height)) {
      self->FailForStep("failed to create precomputed local exposure luma");
      return PipelineStatus::Fail;
    }
    luma = self->resources_.GetLocalExposureLuma();
    ComputeConstants luma_constants = {};
    luma_constants.Width = run.width;
    luma_constants.Height = run.height;
    luma_constants.hdr.Format = run.hdr_uniforms.format;
    luma_constants.hdr.Exposure = run.hdr_uniforms.exposure;
    luma_constants.hdr.Bt2446K1 = run.hdr_uniforms.bt2446_k1;
    luma_constants.hdr.Bt2446K2 = run.hdr_uniforms.bt2446_k2;
    luma_constants.hdr.Bt2446K4 = run.hdr_uniforms.bt2446_k4;
    luma_constants.hdr.Crosstalk = run.hdr_uniforms.bt2446_crosstalk;
    luma_constants.hdr.EffectiveSceneWhiteNits = run.hdr_uniforms.effective_scene_white_nits;
    ID3D12Resource* luma_srvs[kSrvCount] = {source, source, source, source};
    ID3D12Resource* luma_uavs[kUavCount] = {luma};
    self->command_.Transition(run.list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(run.list, luma, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!run.codec->Dispatch("BuildLocalExposureLuma", run.list, luma_constants,
                             luma_srvs, luma_uavs)) {
      self->FailForStep("failed to precompute local exposure luma");
      return PipelineStatus::Fail;
    }
    self->command_.UavBarrier(run.list, luma);
    self->command_.Transition(run.list, luma, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  ID3D12Resource* global_carrier = nullptr;
  if (run.hdr_uniforms.local_exposure_strength > 0.0f) {
    global_carrier = self->resources_.GetGlobalHdrCarrier();
    if (global_carrier == nullptr) {
      self->FailForStep("unlifted HDR carrier resource is unavailable");
      return PipelineStatus::Fail;
    }
    constants.shader.Params1.x = 1.0f;
    self->command_.Transition(run.list, global_carrier,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  ID3D12Resource* srvs[kSrvCount] = {};
  BindLocalExposureInputs(run, constants, source, srvs);
  ID3D12Resource* uavs[kUavCount] = {packed, global_carrier};
  self->command_.Transition(run.list, source,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(run.list, packed,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!run.codec->Dispatch("GainMapSplit", run.list, constants, srvs, uavs)) {
    self->FailForStep("failed to split HDR into SDR + gain map");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(run.list, packed);
  self->command_.Transition(run.list, packed,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (global_carrier != nullptr) {
    self->command_.UavBarrier(run.list, global_carrier);
    self->command_.Transition(run.list, global_carrier,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Only the first active pass publishes frame-level statistics, and the
  // expensive diagnostic work is opt-in. The shader reduces each thread group
  // before touching the global counters.
  if (!run.hdr_state.detailed_statistics || !run.is_first_pass) {
    return PipelineStatus::Ok;
  }
  if (stats == nullptr) {
    self->FailForStep("HDR gain-map statistics resource is unavailable");
    return PipelineStatus::Fail;
  }
  ID3D12Resource* stats_srvs[kSrvCount] = {packed, packed, packed, packed};
  ID3D12Resource* stats_uavs[kUavCount] = {stats};
  self->command_.Transition(run.list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComputeConstants stat_constants = constants;
  if (!run.codec->Dispatch("ZeroGainMapStats", run.list, stat_constants,
                           stats_srvs, stats_uavs, 2, 1, 1)) {
    self->FailForStep("failed to initialize HDR gain-map statistics");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(run.list, stats);
  if (!run.codec->Dispatch("BuildGainMapStats", run.list, stat_constants,
                           stats_srvs, stats_uavs)) {
    self->FailForStep("failed to build HDR gain-map statistics");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(run.list, stats);
  self->command_.Transition(run.list, stats, D3D12_RESOURCE_STATE_COPY_SOURCE);

  // One readback per game frame is enough. The first pass owns the frame-level
  // diagnostic so chained passes do not enqueue duplicate copies.
  if (run.is_first_pass && run.hdr != nullptr) {
    if (!run.hdr->EnqueueGainMapStatsReadback(run.list, stats)) {
      TRACE_EVT(self, "gain_stats_readback unavailable");
    }
  }

  self->command_.Transition(run.list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return PipelineStatus::Ok;
}

// Downsample the already-split linear SDR base to working resolution with
// exact pixel-footprint box/area averaging, and
// report the resolution the model will actually run at. Gain-map splitting is
// a distinct step so the packed HDR representation is also available to the
// debug and compose paths at full resolution.
PipelineStatus PassSession::StepPrepareModelInput(PassRun& run) {
  PassSession* self = run.session;
  if (run.codec == nullptr) return PipelineStatus::SkipFrame;

  const bool hdr_source = run.hdr_uniforms_ready && run.hdr != nullptr &&
                          run.hdr->ResolvedFormat() != HdrFormat::Sdr;
  ID3D12Resource* model_input = run.stage != nullptr && run.stage->owns_submit
                                    ? run.backbuffer
                                    : (hdr_source ? self->resources_.GetPackedOutput()
                                                  : run.model_input);
  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = self->resources_.GetOutput();
  unsigned int eval_width = run.width;
  unsigned int eval_height = run.height;

  const bool needs_pre_grade_snapshot =
      run.grade_input.enabled && run.settings != nullptr &&
      DebugViewNeedsPreGradeInput(run.debug_view) &&
      self->pass_id_ == run.settings->active_pass_id;
  const GradeSettings* fused_grade =
      run.grade_input.enabled && !needs_pre_grade_snapshot
          ? &run.grade_input : nullptr;
  run.input_grade_fused = fused_grade != nullptr;

  if (!model_pipeline::PrepareInput(
          self->command_, self->StageList(run, self->command_),
          self->resources_, run.codec, model_input,
          &model_color, &model_output, &eval_width, &eval_height,
          run.work_width, run.work_height,
          run.hdr_uniforms_ready && run.hdr != nullptr &&
              run.hdr->ResolvedFormat() != HdrFormat::Sdr,
          run.pass_bound && !run.grade_input.enabled, fused_grade)) {
    self->FailForStep("failed to prepare model input");
    return PipelineStatus::Fail;
  }

  if (hdr_source && run.hdr_uniforms.local_exposure_strength > 0.0f) {
    ID3D12Resource* global_carrier = self->resources_.GetGlobalHdrCarrier();
    if (global_carrier == nullptr ||
        !self->resources_.EnsureUnliftedWorkingInput(
            run.device, self->resources_.GetWorkingInput()->GetDesc(),
            run.work_width, run.work_height)) {
      self->FailForStep("failed to create unlifted HDR composition reference");
      return PipelineStatus::Fail;
    }

    ID3D12Resource* unlifted_input =
        self->resources_.GetUnliftedWorkingInput();
    ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
    self->command_.SetResourceState(unlifted_input,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    self->command_.Transition(list, global_carrier,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, unlifted_input,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ComputeConstants reference_constants = {};
    reference_constants.Width = run.work_width;
    reference_constants.Height = run.work_height;
    reference_constants.shader.Params0.w = 1.0f;
    if (run.input_grade_fused)
      model_pipeline::SetGradeConstants(reference_constants, run.grade_input);
    ID3D12Resource* reference_srvs[kSrvCount] = {
        global_carrier, global_carrier, global_carrier, global_carrier};
    ID3D12Resource* reference_uavs[kUavCount] = {unlifted_input};
    if (!run.codec->Dispatch("PrepareModelInput", list, reference_constants,
                             reference_srvs, reference_uavs)) {
      self->FailForStep("failed to prepare unlifted HDR composition reference");
      return PipelineStatus::Fail;
    }
    self->command_.UavBarrier(list, unlifted_input);
    self->command_.Transition(list, unlifted_input,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  run.model_color = model_color;
  run.model_output = model_output;
  run.eval_width = eval_width;
  run.eval_height = eval_height;
  return PipelineStatus::Ok;
}

// Temporal edit amortization. This stage runs after the actual model input has
// been prepared and graded, but before NGX is called. When all evidence from a
// completed prior frame permits reuse, it writes current_input * exp(warpedGain)
// into the ordinary model-output resource and marks the NGX step as skipped.
// Every rejection is fail-open: the regular fresh model evaluation still runs.
PipelineStatus PassSession::StepTemporalPrepare(PassRun& run) {
  PassSession* self = run.session;
  run.temporal_candidate_ready = false;
  if (self == nullptr || run.codec == nullptr || run.device == nullptr ||
      run.model_color == nullptr || run.model_output == nullptr ||
      run.work_width == 0 || run.work_height == 0) {
    return PipelineStatus::Ok;
  }

  const int mode = std::clamp(run.pass_state.temporal_mode, 0, 3);
  if (mode == 0) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    self->temporal_invalid_fraction_ = 1.0f;
    self->temporal_last_mode_ = 0;
    self->temporal_settings_signature_ = 0;
    return PipelineStatus::Ok;
  }

  const D3D12_RESOURCE_DESC input_desc = run.model_color->GetDesc();
  bool resources_reset = false;
  if (!self->resources_.EnsureTemporalResources(
          run.device, input_desc, run.work_width, run.work_height,
          run.pass_state.temporal_fp16_history, &resources_reset)) {
    // The feature is optional; do not turn a failed temporal allocation into
    // a pass failure. Fresh NGX evaluation remains the rendering fallback.
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    run.temporal_active = false;
    TRACE_EVT(self, "temporal disabled pass=%u reason=resource_create", self->pass_id_);
    return PipelineStatus::Ok;
  }

  const uint64_t signature = TemporalSettingsSignature(run);
  const DXGI_FORMAT depth_format = run.guides.depth != nullptr
      ? run.guides.depth->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
  const DXGI_FORMAT motion_format = run.guides.motion != nullptr
      ? run.guides.motion->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
  const bool contract_changed = resources_reset || self->temporal_last_mode_ != mode ||
      self->temporal_settings_signature_ != signature ||
      self->temporal_guide_width_ != run.guides.guide_width ||
      self->temporal_guide_height_ != run.guides.guide_height ||
      self->temporal_motion_width_ != run.guides.motion_width ||
      self->temporal_motion_height_ != run.guides.motion_height ||
      self->temporal_guide_base_x_ != run.guides.guide_base_x ||
      self->temporal_guide_base_y_ != run.guides.guide_base_y ||
      self->temporal_guide_subrect_width_ != run.guides.guide_subrect_width ||
      self->temporal_guide_subrect_height_ != run.guides.guide_subrect_height ||
      self->temporal_motion_base_x_ != run.guides.motion_base_x ||
      self->temporal_motion_base_y_ != run.guides.motion_base_y ||
      self->temporal_motion_subrect_width_ != run.guides.motion_subrect_width ||
      self->temporal_motion_subrect_height_ != run.guides.motion_subrect_height ||
      self->temporal_reference_width_ != run.guides.reference_width ||
      self->temporal_reference_height_ != run.guides.reference_height ||
      self->temporal_depth_format_ != depth_format ||
      self->temporal_motion_format_ != motion_format ||
      self->temporal_mv_scale_x_ != run.guides.mv_scale_x ||
      self->temporal_mv_scale_y_ != run.guides.mv_scale_y;
  if (contract_changed) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    self->temporal_invalid_fraction_ = 1.0f;
    self->temporal_last_refresh_frame_ = 0;
  }
  self->temporal_last_mode_ = mode;
  self->temporal_settings_signature_ = signature;
  self->temporal_guide_width_ = run.guides.guide_width;
  self->temporal_guide_height_ = run.guides.guide_height;
  self->temporal_motion_width_ = run.guides.motion_width;
  self->temporal_motion_height_ = run.guides.motion_height;
  self->temporal_guide_base_x_ = run.guides.guide_base_x;
  self->temporal_guide_base_y_ = run.guides.guide_base_y;
  self->temporal_guide_subrect_width_ = run.guides.guide_subrect_width;
  self->temporal_guide_subrect_height_ = run.guides.guide_subrect_height;
  self->temporal_motion_base_x_ = run.guides.motion_base_x;
  self->temporal_motion_base_y_ = run.guides.motion_base_y;
  self->temporal_motion_subrect_width_ = run.guides.motion_subrect_width;
  self->temporal_motion_subrect_height_ = run.guides.motion_subrect_height;
  self->temporal_reference_width_ = run.guides.reference_width;
  self->temporal_reference_height_ = run.guides.reference_height;
  self->temporal_depth_format_ = depth_format;
  self->temporal_motion_format_ = motion_format;
  self->temporal_mv_scale_x_ = run.guides.mv_scale_x;
  self->temporal_mv_scale_y_ = run.guides.mv_scale_y;

  // Backbuffer reuse is allowed only if an NGX guide capture happened since
  // the previous Present-stage use. Pre/Post stages own the just-captured
  // guide snapshot directly and therefore need no serial comparison.
  bool guides_fresh = run.guides_valid && run.guides.depth != nullptr &&
      run.guides.motion != nullptr && run.guides.guide_width != 0 &&
      run.guides.guide_height != 0 && run.guides.motion_width != 0 &&
      run.guides.motion_height != 0 &&
      std::isfinite(run.guides.mv_scale_x) && std::isfinite(run.guides.mv_scale_y);
  if (run.stage != nullptr && run.stage->owns_submit) {
    guides_fresh = guides_fresh && run.guides.capture_serial != 0 &&
        run.guides.capture_serial != self->temporal_last_guide_serial_;
  }
  if (run.guides.capture_serial != 0) {
    self->temporal_last_guide_serial_ = run.guides.capture_serial;
  }
  if (!guides_fresh) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    run.temporal_active = false;
    TRACE_EVT(self, "temporal fallback pass=%u reason=guides_not_fresh", self->pass_id_);
    return PipelineStatus::Ok;
  }

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  ID3D12Resource* previous_input = self->resources_.GetTemporalPreviousInput();
  ID3D12Resource* previous_edit = self->resources_.GetTemporalPreviousEdit();
  ID3D12Resource* previous_depth = self->resources_.GetTemporalPreviousDepth();
  ID3D12Resource* candidate = self->resources_.GetTemporalCandidate();
  ID3D12Resource* validated = self->resources_.GetTemporalValidatedCandidate();
  ID3D12Resource* stats = self->resources_.GetTemporalStats();
  if (list == nullptr || previous_input == nullptr || previous_edit == nullptr ||
      previous_depth == nullptr || candidate == nullptr || validated == nullptr ||
      stats == nullptr) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    return PipelineStatus::Ok;
  }

  // The game-list feedback is retired by a Release-queue signal scheduled
  // after the game's submission. Backbuffer feedback uses this session's own
  // fence, signalled by SubmitBackbuffer. Polling never waits on the GPU.
  ID3D12Fence* feedback_fence = run.stage != nullptr && run.stage->owns_submit
      ? self->command_.GetFence() : run.temporal_feedback_fence;
  if (feedback_fence != nullptr) {
    if (self->temporal_readback_.Ensure(run.device, 2u * sizeof(uint32_t))) {
      bool sample_valid = false;
      const float fraction = TemporalReadInvalidFraction(
          self->temporal_readback_, feedback_fence, run.work_width,
          run.work_height, &sample_valid);
      // Feedback validity is carried separately from the invalid fraction.
      if (sample_valid) {
        self->temporal_invalid_fraction_ = fraction;
        self->temporal_feedback_valid_ = true;
      } else if (fraction >= 1.0f && !self->temporal_history_valid_) {
        self->temporal_feedback_valid_ = false;
      }
    }
  }

  const bool duplicate_frame = run.frame_id != 0 &&
      run.frame_id == self->temporal_last_frame_id_;
  run.temporal_duplicate_frame = duplicate_frame;
  run.temporal_active = true;
  run.temporal_reuse = false;
  run.temporal_store_history = !duplicate_frame && run.frame_id != 0;
  self->temporal_last_frame_id_ = run.frame_id;

  self->command_.Transition(list, run.model_color,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, run.model_output,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, previous_input,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, previous_edit,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, previous_depth,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, candidate,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, validated,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, run.guides.motion,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, run.guides.depth,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ComputeConstants constants = {};
  constants.Width = run.work_width;
  constants.Height = run.work_height;
  constants.temporal.Thresholds.x =
      run.pass_state.temporal_photometric_abs_threshold;
  constants.temporal.Thresholds.y =
      run.pass_state.temporal_photometric_rel_threshold;
  constants.temporal.Thresholds.z =
      run.pass_state.temporal_depth_abs_threshold;
  constants.temporal.Thresholds.w =
      run.pass_state.temporal_depth_rel_threshold;
  constants.temporal.MotionLimits.x = run.pass_state.temporal_max_displacement;
  ID3D12Resource* repeated_srvs[kSrvCount] = {
      run.model_color, run.model_color, run.model_color, run.model_color};
  ID3D12Resource* stats_uavs[kUavCount] = {stats};
  if (!run.codec->Dispatch("TemporalClearStats", list, constants,
                           repeated_srvs, stats_uavs, 1, 1, 1)) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    run.temporal_active = false;
    TRACE_EVT(self, "temporal fallback pass=%u reason=clear_stats", self->pass_id_);
    return PipelineStatus::Ok;
  }
  self->command_.UavBarrier(list, stats);

  bool have_candidate = self->temporal_history_valid_ && !contract_changed &&
      previous_input != nullptr && previous_edit != nullptr &&
      previous_depth != nullptr;
  if (have_candidate) {
    ID3D12Resource* reproject_srvs[kSrvCount] = {
        run.model_color, previous_input, previous_edit, run.guides.motion};
    ID3D12Resource* candidate_uavs[kUavCount] = {candidate};
    constants.temporal.MotionScaleBase.x = run.guides.mv_scale_x;
    constants.temporal.MotionScaleBase.y = run.guides.mv_scale_y;
    constants.temporal.MotionScaleBase.z =
        static_cast<float>(run.guides.motion_base_x);
    constants.temporal.MotionScaleBase.w =
        static_cast<float>(run.guides.motion_base_y);
    constants.temporal.MotionReferenceSize.x =
        static_cast<float>(run.guides.reference_width);
    constants.temporal.MotionReferenceSize.y =
        static_cast<float>(run.guides.reference_height);
    constants.temporal.MotionReferenceSize.z =
        static_cast<float>(run.guides.motion_subrect_width);
    constants.temporal.MotionReferenceSize.w =
        static_cast<float>(run.guides.motion_subrect_height);
    constants.temporal.GuideBaseSize.x =
        static_cast<float>(run.guides.guide_base_x);
    constants.temporal.GuideBaseSize.y =
        static_cast<float>(run.guides.guide_base_y);
    constants.temporal.GuideBaseSize.z =
        static_cast<float>(run.guides.guide_subrect_width);
    constants.temporal.GuideBaseSize.w =
        static_cast<float>(run.guides.guide_subrect_height);
    if (!run.codec->Dispatch("TemporalReprojectEdit", list, constants,
                             reproject_srvs, candidate_uavs)) {
      self->temporal_history_valid_ = false;
      self->temporal_feedback_valid_ = false;
      run.temporal_active = false;
      TRACE_EVT(self, "temporal fallback pass=%u reason=reproject", self->pass_id_);
      return PipelineStatus::Ok;
    }
    self->command_.UavBarrier(list, candidate);
    self->command_.Transition(list, candidate,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12Resource* validate_srvs[kSrvCount] = {
        candidate, run.guides.motion, run.guides.depth, previous_depth};
    ID3D12Resource* validated_uavs[kUavCount] = {validated, stats};
    if (!run.codec->Dispatch("TemporalValidateDepth", list, constants,
                             validate_srvs, validated_uavs)) {
      self->temporal_history_valid_ = false;
      self->temporal_feedback_valid_ = false;
      run.temporal_active = false;
      TRACE_EVT(self, "temporal fallback pass=%u reason=depth_validate", self->pass_id_);
      return PipelineStatus::Ok;
    }
    self->command_.UavBarrier(list, validated);
    self->command_.UavBarrier(list, stats);
    self->command_.Transition(list, validated,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    run.temporal_candidate_ready = true;
  }

  // Enqueue the current validation sample for a later frame. The fence value
  // is predicted from the owning queue's next signal; no CPU synchronization is
  // introduced. A missing fence merely delays policy feedback and thus forces
  // fresh model evaluation.
  ID3D12Fence* copy_fence = feedback_fence;
  uint64_t copy_fence_value = 0;
  if (run.stage != nullptr && run.stage->owns_submit) {
    copy_fence = self->command_.GetFence();
    copy_fence_value = self->command_.GetNextFenceValue();
  } else {
    copy_fence_value = run.temporal_feedback_fence_value;
  }
  if (copy_fence != nullptr && copy_fence_value != 0 &&
      self->temporal_readback_.Ensure(run.device, 2u * sizeof(uint32_t))) {
    self->command_.Transition(list, stats, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!self->temporal_readback_.EnqueueCopy(
            list, stats, 2u * sizeof(uint32_t), copy_fence, copy_fence_value)) {
      TRACE_EVT(self, "temporal feedback copy deferred pass=%u", self->pass_id_);
    }
    self->command_.Transition(list, stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  const uint64_t age = run.frame_id >= self->temporal_last_refresh_frame_
      ? run.frame_id - self->temporal_last_refresh_frame_ : UINT64_MAX;
  uint64_t cadence = 4;
  if (mode == 2) cadence = 2;
  // Adaptive and Every 4 both have a hard four-frame maximum age. Adaptive's
  // refresh is driven by the same validation threshold; Every 4 adds the
  // deterministic cadence floor, while failed validation always refreshes.
  float max_invalid_percent = run.pass_state.temporal_max_invalid_percent;
  max_invalid_percent = std::clamp(max_invalid_percent, 1.0f, 50.0f);
  const float max_invalid_fraction = max_invalid_percent * 0.01f;
  const bool can_skip = have_candidate && !duplicate_frame && run.frame_id != 0 &&
      self->temporal_feedback_valid_ &&
      self->temporal_invalid_fraction_ < max_invalid_fraction &&
      self->temporal_last_refresh_frame_ != 0 && age > 0 && age < cadence;
  if (can_skip) {
    self->command_.Transition(list, validated,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* apply_srvs[kSrvCount] = {
        run.model_color, validated, validated, validated};
    ID3D12Resource* output_uavs[kUavCount] = {run.model_output};
    self->command_.Transition(list, run.model_output,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (run.codec->Dispatch("TemporalApplyEdit", list, constants,
                             apply_srvs, output_uavs)) {
      self->command_.UavBarrier(list, run.model_output);
      run.temporal_reuse = true;
      run.eval_result = NVSDK_NGX_Result_Success;
      TRACE_EVT(self, "temporal reuse pass=%u age=%llu invalid=%.4f mode=%d",
                self->pass_id_, static_cast<unsigned long long>(age),
                self->temporal_invalid_fraction_, mode);
    } else {
      self->temporal_history_valid_ = false;
      self->temporal_feedback_valid_ = false;
      run.temporal_reuse = false;
      TRACE_EVT(self, "temporal refresh pass=%u reason=apply_dispatch", self->pass_id_);
    }
  }

  if (!run.temporal_reuse && run.frame_id != 0 && !duplicate_frame) {
    self->temporal_last_refresh_frame_ = run.frame_id;
  }
  return PipelineStatus::Ok;
}

// Builds the full-resolution local SDR exposure scale for debug visualization.
// HDR inverse composition recovers the same multiplier from the source/carrier
// pair and does not use this extra texture in its descriptor table.
PipelineStatus PassSession::StepBuildEffectiveLocalExposure(PassRun& run) {
  PassSession* self = run.session;
  if (self == nullptr) return PipelineStatus::Ok;

  const bool selected_active_view = run.settings != nullptr &&
      run.settings->active_pass_id == self->pass_id_ &&
      DebugViewNeedsEffectiveExposureMap(run.debug_view);
  const bool local_hdr_active = run.codec != nullptr && run.hdr != nullptr &&
      run.hdr_uniforms_ready && run.hdr->ResolvedFormat() != HdrFormat::Sdr &&
      run.model_input != nullptr &&
      run.hdr_uniforms.local_exposure_strength > 0.0f;
  if (!selected_active_view || !local_hdr_active) {
    self->resources_.ReleaseEffectiveExposureMap();
    return PipelineStatus::Ok;
  }

  if (!self->resources_.EnsureEffectiveExposureMap(
          run.device, run.width, run.height)) {
    self->FailForStep("failed to create HDR effective exposure map");
    return PipelineStatus::Fail;
  }

  ComputeConstants constants = {};
  constants.Width = run.width;
  constants.Height = run.height;
  constants.hdr.Format = run.hdr_uniforms.format;
  constants.hdr.Exposure = run.hdr_uniforms.exposure;
  constants.hdr.Bt2446K1 = run.hdr_uniforms.bt2446_k1;
  constants.hdr.Bt2446K2 = run.hdr_uniforms.bt2446_k2;
  constants.hdr.Bt2446K4 = run.hdr_uniforms.bt2446_k4;
  constants.hdr.Crosstalk = run.hdr_uniforms.bt2446_crosstalk;
  constants.hdr.GainMinLog2 = run.hdr_uniforms.gain_min_log2;
  constants.hdr.GainMaxLog2 = run.hdr_uniforms.gain_max_log2;
  constants.hdr.EffectiveSceneWhiteNits =
      run.hdr_uniforms.effective_scene_white_nits;
  constants.hdr.LocalExposure.x = run.hdr_uniforms.local_exposure_strength;
  constants.hdr.LocalExposure.y = run.hdr_uniforms.local_max_brighten;
  constants.hdr.LocalExposure.z = run.hdr_uniforms.local_max_darken;
  constants.hdr.LocalExposure.w = run.hdr_uniforms.midtone;

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  ID3D12Resource* srvs[kSrvCount] = {};
  BindLocalExposureInputs(run, constants, run.model_input, srvs);
  ID3D12Resource* exposure_map = self->resources_.GetEffectiveExposureMap();
  ID3D12Resource* uavs[kUavCount] = {exposure_map};
  self->command_.Transition(list, run.model_input,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, exposure_map,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (!run.codec->Dispatch("BuildEffectiveLocalExposure", list, constants,
                           srvs, uavs)) {
    self->FailForStep("failed to build HDR effective local exposure map");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(list, exposure_map);
  self->command_.Transition(list, exposure_map,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return PipelineStatus::Ok;
}

// Fill the real NGX parameter block with tuning values, resources and subrects
// for this evaluation.
PipelineStatus PassSession::StepFillEvaluateParams(PassRun& run) {
  PassSession* self = run.session;
  if (run.temporal_reuse) return PipelineStatus::Ok;
  if (run.runtime == nullptr || !run.pass_bound) {
    return PipelineStatus::SkipFrame;
  }
  if (!self->FillEvaluateParams(*run.runtime, run.pass_state, run.guides,
                                run.model_color, run.model_output,
                                run.eval_width, run.eval_height, run.width,
                                run.height)) {
    self->FailForStep(self->reason_);
    return PipelineStatus::Fail;
  }
  return PipelineStatus::Ok;
}

// Builds one line describing exactly what NGX is being asked to do.
// EvaluateFeature rejects on a format or dimension mismatch with no other
// clue, and the two stages feed it different resources (a swapchain backbuffer
// for Present, the game's DLSS output resource for PostDLSS), so recording the
// concrete descriptors is what makes a failure attributable to one of them.
//
// Logged through the ReShade log, not the trace: the trace is opt-in via
// DLSSNR_TRACE, and this has to be visible to someone who just enabled the
// pass and saw it fail.
//
// The caller emits it on first use and when the descriptor contract changes,
// plus always when a resource is null or an evaluation fails. Resource pointer
// identity is intentionally not treated as a contract change: the backbuffer
// rotates through swapchain images and makes that pointer change every frame.
// Logging those expected rotations caused measurable Present-path overhead
// without adding diagnostic information.
void PassSession::FormatEvalLine(const PassRun& run, const EvalLogKey& key,
                                 char* line, size_t line_size) {
  char color_part[128] = "color=null";
  char out_part[128] = "out=null";
  if (key.color != nullptr) {
    snprintf(color_part, sizeof(color_part), "color=%ux%u/%s", key.color_width,
             key.color_height,
             FormatName(static_cast<DXGI_FORMAT>(key.color_format)));
  }
  if (key.output != nullptr) {
    snprintf(out_part, sizeof(out_part), "out=%ux%u/%s", key.output_width,
             key.output_height,
             FormatName(static_cast<DXGI_FORMAT>(key.output_format)));
  }
  snprintf(line, line_size,
           "DLSSNR evaluate st=%s %s %s eval=%ux%u frame=%ux%u "
           "in=%ux%u work=%ux%u guides=%d pass=%u hdl=%p params=%p",
           run.stage != nullptr ? run.stage->name : "?", color_part, out_part,
           key.eval_width, key.eval_height, key.frame_width, key.frame_height,
           key.input_width, key.input_height, key.work_width, key.work_height,
           key.guides_valid, key.pass_index, key.feature_handle, key.params);
}

// Fills `key` from the current run. Split out so the comparison and the
// formatting read the same values.
void PassSession::BuildEvalKey(const PassRun& run, void* feature_handle,
                               EvalLogKey* key) {
  *key = EvalLogKey{};
  key->stage = run.stage != nullptr ? run.stage->name : "?";
  key->color = run.model_color;
  key->output = run.model_output;
  if (run.model_color != nullptr) {
    const D3D12_RESOURCE_DESC d = run.model_color->GetDesc();
    key->color_width = static_cast<unsigned int>(d.Width);
    key->color_height = static_cast<unsigned int>(d.Height);
    key->color_format = static_cast<int>(d.Format);
  }
  if (run.model_output != nullptr) {
    const D3D12_RESOURCE_DESC d = run.model_output->GetDesc();
    key->output_width = static_cast<unsigned int>(d.Width);
    key->output_height = static_cast<unsigned int>(d.Height);
    key->output_format = static_cast<int>(d.Format);
  }
  key->eval_width = run.eval_width;
  key->eval_height = run.eval_height;
  key->frame_width = run.width;
  key->frame_height = run.height;
  key->input_width = static_cast<unsigned int>(run.desc.Width);
  key->input_height = static_cast<unsigned int>(run.desc.Height);
  key->work_width = run.work_width;
  key->work_height = run.work_height;
  key->guides_valid = run.guides_valid ? 1 : 0;
  key->pass_index = run.pass_id;
  key->feature_handle = feature_handle;
  key->params = run.runtime != nullptr
                    ? static_cast<void*>(run.runtime->Parameters().Get())
                    : nullptr;
}

bool PassSession::EvalKeysDiffer(const EvalLogKey& a, const EvalLogKey& b) {
  // This is a per-session diagnostic, not a per-evaluation trace. Only compare
  // stable descriptor-contract values. Swapchain resource identity naturally
  // rotates, guide availability can fluctuate while the game refreshes its
  // capture, and NGX can change/recreate handles independently of the resource
  // descriptor. None of those warrants reprinting the full INFO line every
  // frame. Feature rebuilds and failed evaluations have their own diagnostics.
  const std::string a_stage = CStrToString(a.stage);
  const std::string b_stage = CStrToString(b.stage);
  return a_stage != b_stage ||
         ((a.color == nullptr) != (b.color == nullptr)) ||
         ((a.output == nullptr) != (b.output == nullptr)) ||
         a.color_width != b.color_width || a.color_height != b.color_height ||
         a.output_width != b.output_width ||
         a.output_height != b.output_height ||
         a.color_format != b.color_format || a.output_format != b.output_format ||
         a.eval_width != b.eval_width || a.eval_height != b.eval_height ||
         a.frame_width != b.frame_width || a.frame_height != b.frame_height ||
         a.input_width != b.input_width || a.input_height != b.input_height ||
         a.work_width != b.work_width || a.work_height != b.work_height;
}

// Invoke NGX.
//
// Backbuffer captures the result and reports a failure after the frame has been
// submitted (see StepSubmitBackbuffer); PostDLSS ignores the result entirely,
// which is what the pre-refactor code did — deliberately or not, it is the
// behaviour this refactor preserves. The Stage 5 bug list records it.
PipelineStatus PassSession::StepEvaluateFeature(PassRun& run) {
  PassSession* self = run.session;
  if (run.temporal_reuse) {
    run.eval_result = NVSDK_NGX_Result_Success;
    TRACE_EVT(self, "temporal skip NGX pass=%u", self->pass_id_);
    return PipelineStatus::Ok;
  }
  if (run.runtime == nullptr) return PipelineStatus::SkipFrame;

  void* feature_handle = static_cast<void*>(self->feature_.FeatureHandle());
  EvalLogKey key = {};
  BuildEvalKey(run, feature_handle, &key);

  // Log on first use and on a descriptor/nullness transition. A persistent
  // null input is reported once as a diagnostic, not once per frame; the
  // evaluation failure path carries the actionable error if it persists.
  const bool first = !self->eval_log_key_valid_;
  const bool changed = first || EvalKeysDiffer(key, self->eval_log_key_);

  self->eval_log_written_this_frame_ = false;
  if (changed) {
    char line[384] = {};
    FormatEvalLine(run, key, line, sizeof(line));
    reshade::log::message(reshade::log::level::info, line);
    TRACE_EVT(self, "%s", line);
    self->eval_log_written_this_frame_ = true;
  }

  // Keep the latest pointers as well as the latest descriptor values even
  // when the diagnostic is suppressed. If NGX fails later in this same frame,
  // the error path can report the current evaluation rather than a stale
  // swapchain resource from a previous frame.
  self->eval_log_key_ = key;
  self->eval_log_key_valid_ = true;

  if (run.stage != nullptr && !run.stage->owns_submit) {
    run.eval_result = run.runtime->EvaluateFeature(
        self->StageList(run, self->command_), self->feature_.FeatureHandle(),
        run.runtime->Parameters().Get(), nullptr);
    return PipelineStatus::Ok;
  }

  run.eval_result = run.runtime->EvaluateFeature(
      self->StageList(run, self->command_), self->feature_.FeatureHandle(),
      run.runtime->Parameters().Get(), nullptr);
  return PipelineStatus::Ok;
}

// NGX writes to the model output through a UAV. A state transition alone does
// not guarantee those writes are visible to the subsequent reads, so insert an
// explicit UAV barrier before composition reads it back.
PipelineStatus PassSession::StepModelOutputBarrier(PassRun& run) {
  PassSession* self = run.session;
  if (run.model_output == nullptr) return PipelineStatus::Ok;
  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  self->command_.UavBarrier(list, run.model_output);

  if (!run.temporal_active) return PipelineStatus::Ok;
  if (run.temporal_reuse) {
    self->command_.Transition(list, run.model_output,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (!run.temporal_store_history || run.guides.depth == nullptr ||
      run.guides.motion == nullptr ||
      (!run.temporal_reuse && run.eval_result != NVSDK_NGX_Result_Success)) {
    self->temporal_history_valid_ = false;
    return PipelineStatus::Ok;
  }

  ID3D12Resource* previous_input = self->resources_.GetTemporalPreviousInput();
  ID3D12Resource* previous_edit = self->resources_.GetTemporalPreviousEdit();
  ID3D12Resource* previous_depth = self->resources_.GetTemporalPreviousDepth();
  ID3D12Resource* validated = self->resources_.GetTemporalValidatedCandidate();
  if (previous_input == nullptr || previous_edit == nullptr ||
      previous_depth == nullptr || validated == nullptr || run.model_color == nullptr ||
      run.codec == nullptr) {
    self->temporal_history_valid_ = false;
    return PipelineStatus::Ok;
  }

  ComputeConstants constants = {};
  constants.Width = run.work_width;
  constants.Height = run.work_height;
  self->command_.Transition(list, run.model_color,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, run.model_output,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (run.temporal_reuse) {
    self->command_.Transition(list, validated,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  self->command_.Transition(list, run.guides.depth,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, previous_edit,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, previous_depth,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  constants.temporal.GuideBaseSize.x =
      static_cast<float>(run.guides.guide_base_x);
  constants.temporal.GuideBaseSize.y =
      static_cast<float>(run.guides.guide_base_y);
  constants.temporal.GuideBaseSize.z =
      static_cast<float>(run.guides.guide_subrect_width);
  constants.temporal.GuideBaseSize.w =
      static_cast<float>(run.guides.guide_subrect_height);
  constants.temporal.HistoryControl.x = run.temporal_reuse ? 1.0f : 0.0f;
  ID3D12Resource* history_srvs[kSrvCount] = {
      run.model_color, run.model_output, validated, run.guides.depth};
  ID3D12Resource* history_uavs[kUavCount] = {previous_edit, previous_depth};
  if (!run.codec->Dispatch("TemporalCommitHistory", list, constants,
                           history_srvs, history_uavs)) {
    self->temporal_history_valid_ = false;
    return PipelineStatus::Ok;
  }
  self->command_.UavBarrier(list, previous_edit);
  self->command_.UavBarrier(list, previous_depth);

  // Advance the photometric reference on every processed frame. The source is
  // exactly the input seen by NGX (after preparation and input grade).
  const D3D12_RESOURCE_STATES input_state =
      self->command_.GetResourceState(run.model_color);
  self->command_.Transition(list, run.model_color,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
  self->command_.Transition(list, previous_input,
                            D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(previous_input, run.model_color);
  self->command_.Transition(list, run.model_color,
                            input_state == D3D12_RESOURCE_STATE_COMMON
                                ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                : input_state);
  self->command_.Transition(list, previous_input,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  self->temporal_history_valid_ = true;
  if (!run.temporal_reuse) {
    self->temporal_last_refresh_frame_ = run.frame_id;
  }
  TRACE_EVT(self, "temporal history committed pass=%u reuse=%d frame=%llu",
            self->pass_id_, run.temporal_reuse ? 1 : 0,
            static_cast<unsigned long long>(run.frame_id));
  return PipelineStatus::Ok;
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

// Compose the model output into PassResources::GetCompositeOutput(). HDR uses
// the packed carrier for model-domain values and the original source resource
// for full-precision HDR composition.
PipelineStatus PassSession::StepComposeModelOutput(PassRun& run) {
  PassSession* self = run.session;
  if (run.codec == nullptr || !run.pass_bound) {
    return PipelineStatus::SkipFrame;
  }

  // Backbuffer composes the model edit against the backbuffer; PostDLSS composes
  // it against the model input, which is the DLSS output or a snapshot of it.
  const bool hdr_active = run.hdr_uniforms_ready &&
                          run.hdr != nullptr &&
                          run.hdr->ResolvedFormat() != HdrFormat::Sdr;
  ID3D12Resource* original = hdr_active
                                 ? self->resources_.GetPackedOutput()
                                 : (run.stage != nullptr && run.stage->owns_submit
                                        ? run.backbuffer
                                        : run.model_input);

  const model_pipeline::HdrUniforms* hdr_ptr =
      hdr_active ? &run.hdr_uniforms : nullptr;

  // Intensity == 0 means the model edit is explicitly disabled. Use the
  // exact model input as the compose reference so NGX/evaluation noise cannot
  // create a residual when the model output is intentionally bypassed.
  ID3D12Resource* enhanced_for_compose =
      run.pass_state.intensity <= 0.0f ? run.model_color : run.model_output;

  ID3D12Resource* compose_input = run.model_color;
  if (hdr_active && run.pass_state.intensity > 0.0f &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      self->resources_.GetUnliftedWorkingInput() != nullptr) {
    compose_input = self->resources_.GetUnliftedWorkingInput();
  }

  if (!model_pipeline::ComposeOutput(
          self->command_, self->StageList(run, self->command_),
          self->resources_, run.codec, original, run.model_input,
          compose_input, enhanced_for_compose, run.width, run.height,
          run.pass_state.strength, run.pass_state.soft_compression,
          run.pass_state.residual, run.pass_state.rcas_enabled,
          run.pass_state.rcas_strength, &run.grade_input, hdr_ptr)) {
    self->FailForStep("failed to compose model output");
    return PipelineStatus::Fail;
  }
  return PipelineStatus::Ok;
}


// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------


// Backbuffer: publish the composed image on the swapchain backbuffer.
PipelineStatus PassSession::StepWriteToBackbuffer(PassRun& run) {
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
  PassSession* self = run.session;
  self->CleanupFrameState(self->command_.GetCommandList());
  self->SubmitBackbuffer();

  // Backbuffer reports the NGX result after the frame is on the GPU, matching the
  // pre-refactor ordering: the frame is never dropped because the model
  // failed, but the pass is marked so the user sees it.
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
    self->FailForStep(reason);
    return PipelineStatus::Fail;
  }
  return PipelineStatus::Ok;
}

// ---------------------------------------------------------------------------
// Step table
// ---------------------------------------------------------------------------

// Prerequisites are deliberately empty for now. The runner checks them and
// logs at error level when one is missing, so populating them is a genuine
// safety net rather than decoration — but it has to be done against a running
// pipeline to be worth anything, which is Stage 4. The flat, depth-ordered
// PipelineNodeId enum is what makes a cycle impossible to express when that
// happens.
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
// would be wrong — AcquireBackbuffer writes both, and the steps that read them
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
// `requires_hdr_aware`: the runner would skip it silently. This is the guard
// for a bug that shipped — PrepareModelInput and ComposeModelOutput were
// flagged HDR-only, got skipped in Backbuffer, and left the model to be evaluated
// with null inputs (NGX: FAIL_InvalidParameter).
//
// Those two steps are not HDR-only: they pick the HDR shader when
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
              "with the extra Importance Mask build dispatch - raise it, and kDescriptorRingSize with it");

const PipelineStepDef* SessionStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kSessionSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr
