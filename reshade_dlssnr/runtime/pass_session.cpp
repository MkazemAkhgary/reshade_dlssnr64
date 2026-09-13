#include "pass_session.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

#include <reshade.hpp>

#include "d3d12_utils.h"
#include "dlss_guides.h"
#include "model_pipeline.h"
#include "ngx_model_params.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_session_mutex;
}

void PassSession::Fail(const char* reason) {
  failed_ = true;
  reason_ = reason;
  reshade::log::message(reshade::log::level::error, reason);
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
    // DlssGuides::RetireGameResource adds a reference and queues the
    // resource for release after the next GPU wait. We already own a
    // reference here, so AddRef first so the queue's eventual Release()
    // balances out.
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
    unsigned int work_height) {
  if (!resources_.EnsureOutput(device, desc) ||
      !resources_.EnsureCompositeOutput(device, desc)) {
    reason_ = "failed to create DLSSNR pass resources";
    return false;
  }

  if (!resources_.EnsureWorkingResources(device, desc, work_width, work_height)) {
    reason_ = "failed to create DLSSNR working resources";
    return false;
  }

  return true;
}

bool PassSession::FillEvaluateParams(
    NgxRuntime& runtime,
    const PassSettings& pass,
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

  params->Reset();

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
    block.SetUInt("DLSSNR.MVecSubrectWidth", guides.guide_width);
    block.SetUInt("DLSSNR.MVecSubrectHeight", guides.guide_height);
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
}

void PassSession::SubmitAndWait() {
  command_.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {command_.GetCommandList()};
  command_queue_->ExecuteCommandLists(1, lists);
  command_.WaitGpu(command_queue_);
  guides_->ReleaseRetiredGameResources();
}

bool PassSession::MaybeRenderDebug(
    const Settings& settings,
    ID3D12GraphicsCommandList* command_list,
    FrameDebugContext& frame_debug,
    const DebugSnapshot& snapshot,
    unsigned int width,
    unsigned int height,
    DXGI_FORMAT format) {
  // Debug view is a shared setting; only the active pass renders it.
  if (settings.debug_view == 0) return false;
  if (static_cast<int>(pass_index_) != settings.active_pass) return false;
  if (command_list == nullptr || codec_ == nullptr) return false;

  // The debug output is displayed on the backbuffer, so prefer the
  // backbuffer's format. Fall back to the pass's own format only when
  // the controller hasn't yet cached the backbuffer format.
  const DXGI_FORMAT target_format =
      frame_debug.preferred_format != DXGI_FORMAT_UNKNOWN
          ? frame_debug.preferred_format
          : format;

  // (Re)create the shared debug output when its shape changes. The output
  // is owned by FrameDebugContext and lives across frames.
  if (frame_debug.output == nullptr ||
      frame_debug.width != width ||
      frame_debug.height != height ||
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
    desc.Width = width;
    desc.Height = height;
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
    frame_debug.width = width;
    frame_debug.height = height;
    frame_debug.format = target_format;
  }

  // Seed the pass tracker: at this point the debug output is always UAV
  // (it was left UAV by the previous frame's copy, or just created UAV).
  command_.SetResourceState(frame_debug.output.Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (!RenderDebugView(command_, *codec_, command_list, snapshot,
                       settings.debug_view, frame_debug.output.Get(),
                       width, height)) {
    return false;
  }

  frame_debug.valid = true;
  return true;
}

void PassSession::OnPresent(
    NgxRuntime& runtime,
    const PassSettings& pass,
    const Settings& settings,
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain,
    FrameDebugContext& frame_debug) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  if (!settings.enabled) return;
  if (failed_ || command_queue == nullptr || swapchain == nullptr) return;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
  D3D12_RESOURCE_DESC desc = {};

  if (!AcquireBackbuffer(swapchain, command_queue, swapchain3, device,
                         backbuffer, desc)) {
    return;
  }

  if (!runtime.Initialize(device.Get())) {
    Fail(runtime.GetLastError());
    return;
  }
  if (!command_.Ensure(device.Get())) {
    Fail("failed to create DLSSNR command list");
    return;
  }

  command_queue_ = command_queue;

  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = static_cast<unsigned int>(desc.Height);

  const float working_scale = std::clamp(pass.working_scale, 0.10f, 1.0f);
  const unsigned int work_width =
      std::max(1u, static_cast<unsigned int>(width * working_scale + 0.5f));
  const unsigned int work_height =
      std::max(1u, static_cast<unsigned int>(height * working_scale + 0.5f));

  // Always retire replaced resources rather than releasing them immediately.
  // Even in Present mode, a resource replaced this frame might still be read
  // by the game's queue from a previous DLSS-mode frame (the game's present
  // is queued, not necessarily flushed). Retiring hands ownership to a
  // deferred queue that is drained only after we synchronize with the GPU.
  resources_.SetRetireCallback(&PassSession::RetireResourceCallback, guides_);

  if (!EnsureFrameResources(device.Get(), desc, work_width, work_height)) {
    Fail(reason_ != nullptr && reason_[0] != '\0'
             ? reason_
             : "failed to create pass resources");
    return;
  }

  command_.Reset();
  command_.ClearState();
  SeedFrameState(backbuffer.Get());

  // Build (or rebuild) the feature at the full frame resolution. Working
  // scale only affects the per-frame evaluation size, which FillEvaluateParams
  // sets on every frame, so changing working scale never triggers a rebuild.
  // A rebuild happens only when the swapchain resolution itself changes.
  if (!feature_.EnsureFeature(
          runtime,
          pass,
          command_.GetCommandList(),
          width,
          height)) {
    Fail(feature_.GetLastError());
    command_.GetCommandList()->Close();
    return;
  }

  if (feature_.ConsumeJustRebuilt()) {
    SubmitAndWait();
    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created/rebuilt; skipping evaluation this frame");
    return;
  }

  const DlssGuideState guides = guides_->GetState();

  // Guides are owned by DlssGuides and always left in SRV. The pass tracker
  // was just reset, so seed that state explicitly; otherwise inference may
  // pick a different (incorrect) "before" state for the debug render.
  if (guides.valid) {
    command_.SetResourceState(guides.depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.SetResourceState(guides.motion,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.SetResourceState(guides.dlss_output_color,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  ID3D12Resource* model_input = backbuffer.Get();

  if (codec_ == nullptr) {
    codec_ = std::make_unique<ComputeEngine>();
  }

  if (!codec_->IsInit() && !codec_->Init(device.Get())) {
    Fail("failed to initialize compute engine");
    return;
  }

  if (model_input == backbuffer.Get()) {
    command_.Transition(command_.GetCommandList(), backbuffer.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = resources_.GetOutput();
  unsigned int eval_width = width;
  unsigned int eval_height = height;

  if (!model_pipeline::PrepareInput(
          command_,
          command_.GetCommandList(),
          resources_,
          codec_.get(),
          model_input,
          &model_color,
          &model_output,
          &eval_width,
          &eval_height,
          width,
          height,
          work_width,
          work_height)) {
    Fail("failed to prepare model input");
    return;
  }

  if (!FillEvaluateParams(runtime, pass, guides, model_color, model_output,
                          eval_width, eval_height, width, height)) {
    Fail(reason_ != nullptr && reason_[0] != '\0'
             ? reason_
             : "failed to fill NGX evaluation parameters");
    return;
  }

  const NVSDK_NGX_Result result =
      runtime.EvaluateFeature(
          command_.GetCommandList(),
          feature_.FeatureHandle(),
          runtime.Parameters().Get(),
          nullptr);

  // NGX writes to the model output through a UAV. A state transition alone
  // does not guarantee those writes are visible to the subsequent reads, so
  // insert an explicit UAV barrier before ComposeOutput reads it back.
  command_.UavBarrier(command_.GetCommandList(), model_output);

  if (!model_pipeline::ComposeOutput(
          command_,
          command_.GetCommandList(),
          resources_,
          codec_.get(),
          backbuffer.Get(),
          model_color,
          model_output,
          width,
          height,
          pass.params)) {
    Fail("failed to compose model output");
    return;
  }

  DebugSnapshot debug;
  debug.source = model_input;
  debug.source_width = width;
  debug.source_height = height;
  debug.prepared_input = model_color;
  debug.prepared_input_width = eval_width;
  debug.prepared_input_height = eval_height;
  debug.model_output = model_output;
  debug.model_output_width = eval_width;
  debug.model_output_height = eval_height;
  debug.composed_output = resources_.GetCompositeOutput();
  debug.composed_output_width = width;
  debug.composed_output_height = height;
  if (guides.valid) {
    debug.motion = guides.motion;
    debug.motion_width = guides.guide_width;
    debug.motion_height = guides.guide_height;
    debug.depth = guides.depth;
    debug.depth_width = guides.guide_width;
    debug.depth_height = guides.guide_height;
  }

  // If this is the active pass, render the debug view now — while the
  // source resources are in the states this pass left them in. It goes
  // into our own resource (frame_debug.output), never the backbuffer.
  MaybeRenderDebug(settings, command_.GetCommandList(), frame_debug,
                   debug, width, height, desc.Format);

  // Always write the real composed output; the debug override happens at
  // Present and does not affect the game's resources.
  command_.CopyResourceToBackbuffer(
      command_.GetCommandList(), resources_.GetCompositeOutput(),
      backbuffer.Get());
  command_.Transition(command_.GetCommandList(), model_output,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  CleanupFrameState(command_.GetCommandList());
  SubmitAndWait();

  if (result != NVSDK_NGX_Result_Success) {
    Fail("DLSSNR EvaluateFeature failed");
  }
}

void PassSession::OnPostDlssEvaluate(
    NgxRuntime& runtime,
    const PassSettings& pass,
    const Settings& settings,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource,
    bool is_first_pass,
    FrameDebugContext& frame_debug,
    HdrState* hdr) {
  if (!settings.enabled) return;
  if (failed_ || command_list == nullptr || output_resource == nullptr) return;

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
      device == nullptr) {
    // Transient: don't fail the pass permanently. This can happen on
    // frames where ReShade hasn't fully wired up the command list yet.
    return;
  }

  if (!runtime.Initialize(device.Get())) {
    // Not fatal here either — the game may not have loaded the DLSS
    // model yet this frame. Log once and keep trying.
    if (runtime.GetLastError() != nullptr &&
        runtime.GetLastError()[0] != '\0') {
      reshade::log::message(reshade::log::level::warning,
                            runtime.GetLastError());
    }
    return;
  }

  if (codec_ == nullptr) {
    codec_ = std::make_unique<ComputeEngine>();
  }

  if (!codec_->IsInit() && !codec_->Init(device.Get())) {
    Fail("failed to initialize compute engine");
    return;
  }

  const DlssGuideState guides = guides_->GetState();

  const D3D12_RESOURCE_DESC desc = output_resource->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = static_cast<unsigned int>(desc.Height);

  const float working_scale = std::clamp(pass.working_scale, 0.10f, 1.0f);
  const unsigned int work_width =
      std::max(1u, static_cast<unsigned int>(width * working_scale + 0.5f));
  const unsigned int work_height =
      std::max(1u, static_cast<unsigned int>(height * working_scale + 0.5f));

  // Post-DLSS runs on the game's command list and does not submit or wait.
  // Resources replaced mid-frame (e.g. when the working-scale slider is
  // dragged) may still be read by in-flight GPU work, so hand them to the
  // retire queue instead of releasing them immediately.
  resources_.SetRetireCallback(&PassSession::RetireResourceCallback, guides_);

  if (!EnsureFrameResources(device.Get(), desc, work_width, work_height)) {
    Fail(reason_ != nullptr && reason_[0] != '\0'
             ? reason_
             : "failed to create pass resources");
    return;
  }

  // Fresh state tracking per pass: no cross-pass aliasing to rely on.
  command_.ClearState();
  SeedFrameState(nullptr);
  command_.SetResourceState(output_resource, InferResourceState(output_resource));

  // Seed guide states: guides are owned by DlssGuides and always left in
  // SRV. Without this, the tracker would infer a state from the resource
  // descriptor, which may be wrong (typeless depth, etc.).
  if (guides.valid) {
    command_.SetResourceState(guides.depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.SetResourceState(guides.motion,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.SetResourceState(guides.dlss_output_color,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Determine input: first pass reads the captured DLSS output; subsequent
  // passes snapshot the previous result into scratch slot 0.
  ID3D12Resource* model_input = nullptr;
  if (is_first_pass) {
    if (guides.dlss_output_color == nullptr) return;
    model_input = guides.dlss_output_color;
    command_.SetResourceState(model_input,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  } else {
    constexpr uint32_t kInputSnapshotSlot = 0;
    if (!resources_.EnsureScratch(device.Get(), desc, width, height,
                                  kInputSnapshotSlot)) {
      failed_ = true;
      return;
    }
    ID3D12Resource* scratch = resources_.GetScratch(kInputSnapshotSlot);
    command_.Transition(command_list, output_resource,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Transition(command_list, scratch,
                        D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->CopyResource(scratch, output_resource);
    command_.Transition(command_list, scratch,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    model_input = scratch;
  }

  // Build at full frame resolution; working scale is applied per-evaluation
  // via FillEvaluateParams. Matches the Present path.
  if (!feature_.EnsureFeature(runtime, pass, command_list, width, height)) {
    Fail(feature_.GetLastError());
    return;
  }
  if (feature_.ConsumeJustRebuilt()) {
    // Skip evaluation this frame; the feature was just created/rebuilt.
    return;
  }

  // Build HDR tonemap uniforms if HDR is enabled. The tonemap shader
  // doubles as the resize into working scale, so passing these uniforms
  // is all that's needed to switch from a plain resample to a full
  // HDR → SDR normalize.
  //
  // `Highlight Headroom` scales the auto-detected white point. Default 1.0
  // keeps the detected value; larger values push the compression knee
  // higher, preserving more highlight headroom.
  model_pipeline::HdrUniforms hdr_uniforms;
  const model_pipeline::HdrUniforms* hdr_ptr = nullptr;
  if (hdr != nullptr && hdr->Enabled()) {
    hdr_uniforms.exposure = hdr->Exposure();
    hdr_uniforms.white_point =
        hdr->WhitePoint() * std::max(settings.hdr_highlight_headroom, 0.01f);
    hdr_uniforms.edit_strength = settings.hdr_edit_strength;
    hdr_uniforms.chroma_mode =
        static_cast<uint32_t>(settings.hdr_chroma_mode);
    switch (hdr->ResolvedFormat()) {
      case HdrFormat::ScRgb: hdr_uniforms.format = 1; break;
      case HdrFormat::Hdr10: hdr_uniforms.format = 2; break;
      case HdrFormat::Sdr:
      case HdrFormat::Auto:
      default:               hdr_uniforms.format = 0; break;
    }
    hdr_ptr = &hdr_uniforms;
  }

  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = resources_.GetOutput();
  unsigned int eval_width = width;
  unsigned int eval_height = height;

  if (!model_pipeline::PrepareInput(
          command_,
          command_list,
          resources_,
          codec_.get(),
          model_input,
          &model_color,
          &model_output,
          &eval_width,
          &eval_height,
          width,
          height,
          work_width,
          work_height,
          hdr_ptr)) {
    Fail("failed to prepare model input");
    return;
  }

  if (!FillEvaluateParams(runtime, pass, guides, model_color, model_output,
                          eval_width, eval_height, width, height)) {
    Fail(reason_ != nullptr && reason_[0] != '\0'
             ? reason_
             : "failed to fill NGX evaluation parameters");
    return;
  }

  runtime.EvaluateFeature(
      command_list,
      feature_.FeatureHandle(),
      runtime.Parameters().Get(),
      nullptr);

  // NGX writes to the model output through a UAV; barrier it before we
  // read it back (a state transition alone does not synchronize UAVs).
  command_.UavBarrier(command_list, model_output);

  if (!model_pipeline::ComposeOutput(
          command_,
          command_list,
          resources_,
          codec_.get(),
          model_input,
          model_color,
          model_output,
          width,
          height,
          pass.params,
          hdr_ptr)) {
    Fail("failed to compose model output");
    return;
  }

  DebugSnapshot debug;
  debug.source = model_input;
  debug.source_width = width;
  debug.source_height = height;
  debug.prepared_input = model_color;
  debug.prepared_input_width = eval_width;
  debug.prepared_input_height = eval_height;
  debug.model_output = model_output;
  debug.model_output_width = eval_width;
  debug.model_output_height = eval_height;
  debug.composed_output = resources_.GetCompositeOutput();
  debug.composed_output_width = width;
  debug.composed_output_height = height;
  if (guides.valid) {
    debug.motion = guides.motion;
    debug.motion_width = guides.guide_width;
    debug.motion_height = guides.guide_height;
    debug.depth = guides.depth;
    debug.depth_width = guides.guide_width;
    debug.depth_height = guides.guide_height;
  }

  // If this is the active pass, render the debug view now — while the
  // source resources are in the states this pass left them in. It goes
  // into our own resource, never the game's.
  MaybeRenderDebug(settings, command_list, frame_debug,
                   debug, width, height, desc.Format);

  // Always write the real composed output to the game's resource. The debug
  // image is applied to the backbuffer at Present only.
  command_.Transition(command_list, resources_.GetCompositeOutput(),
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_RESOURCE_STATES out_state = InferResourceState(output_resource);
  command_.Transition(command_list, output_resource,
                      D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(output_resource, resources_.GetCompositeOutput());
  command_.Transition(command_list, output_resource, out_state);
  command_.Transition(command_list, resources_.GetCompositeOutput(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // Leave pass-owned resources in the state SeedFrameState() expects.
  CleanupFrameState(command_list);
}

void PassSession::Shutdown(NgxRuntime& runtime) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  feature_.Shutdown(runtime);
  resources_.Shutdown();
  command_.Shutdown();

  codec_.reset();
  command_queue_ = nullptr;
  failed_ = false;
  reason_ = "";
}

}  // namespace reshade_dlssnr
