#include "pass_session.h"

#include <algorithm>
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

bool PassSession::AcquireBackbuffer(
    IDXGISwapChain* swapchain,
    ID3D12CommandQueue* command_queue,
    Microsoft::WRL::ComPtr<IDXGISwapChain3>& out_swapchain3,
    Microsoft::WRL::ComPtr<ID3D12Device>& out_device,
    Microsoft::WRL::ComPtr<ID3D12Resource>& out_backbuffer,
    D3D12_RESOURCE_DESC& out_desc) {
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&out_swapchain3))) ||
      out_swapchain3 == nullptr) {
    return false;
  }

  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&out_device))) ||
      out_device == nullptr) {
    return false;
  }

  const UINT back_buffer_index = out_swapchain3->GetCurrentBackBufferIndex();
  if (FAILED(out_swapchain3->GetBuffer(
          back_buffer_index, IID_PPV_ARGS(&out_backbuffer))) ||
      out_backbuffer == nullptr) {
    return false;
  }

  out_desc = out_backbuffer->GetDesc();
  return true;
}

bool PassSession::EnsureFrameResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int work_width,
    unsigned int work_height) {
  if (!resources_.EnsureOutput(device, desc) ||
      !resources_.EnsureCompositeOutput(device, desc) ||
      !resources_.EnsureWhitePointBuffer(device)) {
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
  command_.SetResourceState(resources_.GetWhitePointBuffer(),
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
  command_.Transition(command_list, resources_.GetWhitePointBuffer(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void PassSession::SubmitAndWait() {
  command_.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {command_.GetCommandList()};
  command_queue_->ExecuteCommandLists(1, lists);
  command_.WaitGpu(command_queue_);
  guides_->ReleaseRetiredGameResources();
}

void PassSession::OnPresent(
    NgxRuntime& runtime,
    const PassSettings& pass,
    const Settings& settings,
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
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

  if (!runtime.Initialize(device.Get()) || !command_.Ensure(device.Get())) {
    failed_ = true;
    reason_ = runtime.GetLastError();
    reshade::log::message(reshade::log::level::error, reason_);
    return;
  }

  command_queue_ = command_queue;

  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = static_cast<unsigned int>(desc.Height);

  const float working_scale = std::clamp(pass.working_scale, 0.25f, 1.0f);
  const unsigned int work_width =
      std::max(1u, static_cast<unsigned int>(width * working_scale + 0.5f));
  const unsigned int work_height =
      std::max(1u, static_cast<unsigned int>(height * working_scale + 0.5f));
  const bool use_working_scale =
      work_width != width || work_height != height;

  if (!EnsureFrameResources(device.Get(), desc, work_width, work_height)) {
    failed_ = true;
    return;
  }

  command_.Reset();
  command_.ClearState();
  SeedFrameState(backbuffer.Get());

  if (!feature_.HasFeature()) {
    const unsigned int create_width = use_working_scale ? work_width : width;
    const unsigned int create_height = use_working_scale ? work_height : height;

    if (!feature_.EnsureFeature(
            runtime,
            pass,
            command_.GetCommandList(),
            create_width,
            create_height)) {
      failed_ = true;
      command_.GetCommandList()->Close();
      return;
    }

    SubmitAndWait();

    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created; skipping evaluation this frame");
    return;
  }

  const DlssGuideState guides = guides_->GetState();

  ID3D12Resource* model_input = backbuffer.Get();

  if (codec_ == nullptr) {
    codec_ = std::make_unique<ComputeEngine>();
  }

  if (!codec_->IsInit() && !codec_->Init(device.Get())) {
    failed_ = true;
    reason_ = "failed to initialize codec";
    reshade::log::message(reshade::log::level::error, reason_);
    return;
  }

  if (model_input == backbuffer.Get()) {
    command_.Transition(command_.GetCommandList(), backbuffer.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Measure the frame's white point once. The result is shared by
  // PrepareInput (HDR -> SDR) and ComposeOutput (SDR -> HDR).
  if (!model_pipeline::MeasureWhitePoint(
          command_,
          command_.GetCommandList(),
          resources_,
          codec_.get(),
          model_input,
          width,
          height)) {
    failed_ = true;
    reason_ = "failed to measure white point";
    return;
  }
  command_.Transition(command_.GetCommandList(),
                      resources_.GetWhitePointBuffer(),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

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
    failed_ = true;
    reason_ = "failed to prepare model input";
    return;
  }
  (void)use_working_scale;

  if (!FillEvaluateParams(runtime, pass, guides, model_color, model_output,
                          eval_width, eval_height, width, height)) {
    return;
  }

  const NVSDK_NGX_Result result =
      runtime.EvaluateFeature(
          command_.GetCommandList(),
          feature_.FeatureHandle(),
          runtime.Parameters().Get(),
          nullptr);

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
    failed_ = true;
    reason_ = "failed to compose model output";
    return;
  }

  ID3D12Resource* final_output = resources_.GetCompositeOutput();

  if (pass.debug_view != 0) {
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

    ID3D12Resource* debug_out = DisplayDebugView(
        command_, *codec_, command_.GetCommandList(), resources_, debug,
        pass.debug_view, width, height);
    if (debug_out != nullptr) final_output = debug_out;
  }

  command_.CopyResourceToBackbuffer(
      command_.GetCommandList(), final_output, backbuffer.Get());
  command_.Transition(command_.GetCommandList(), model_output,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  CleanupFrameState(command_.GetCommandList());
  SubmitAndWait();

  if (result != NVSDK_NGX_Result_Success) {
    failed_ = true;
    reason_ = "DLSSNR EvaluateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }
}

void PassSession::OnPostDlssEvaluate(
    NgxRuntime& runtime,
    const PassSettings& pass,
    const Settings& settings,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource,
    bool is_first_pass) {
  if (!settings.enabled) return;
  if (failed_ || command_list == nullptr || output_resource == nullptr) return;

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
      device == nullptr) {
    return;
  }

  if (!runtime.Initialize(device.Get())) return;

  if (codec_ == nullptr) {
    codec_ = std::make_unique<ComputeEngine>();
  }

  if (!codec_->IsInit() && !codec_->Init(device.Get())) {
    failed_ = true;
    reason_ = "failed to initialize compute engine";
    return;
  }

  const DlssGuideState guides = guides_->GetState();

  const D3D12_RESOURCE_DESC desc = output_resource->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = static_cast<unsigned int>(desc.Height);

  const float working_scale = std::clamp(pass.working_scale, 0.25f, 1.0f);
  const unsigned int work_width =
      std::max(1u, static_cast<unsigned int>(width * working_scale + 0.5f));
  const unsigned int work_height =
      std::max(1u, static_cast<unsigned int>(height * working_scale + 0.5f));
  const bool use_working_scale =
      work_width != width || work_height != height;

  if (!EnsureFrameResources(device.Get(), desc, work_width, work_height)) {
    failed_ = true;
    return;
  }

  // Fresh state tracking per pass: no cross-pass aliasing to rely on.
  command_.ClearState();
  SeedFrameState(nullptr);
  command_.SetResourceState(output_resource, InferResourceState(output_resource));

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

  if (!feature_.HasFeature()) {
    const unsigned int create_width = use_working_scale ? work_width : width;
    const unsigned int create_height = use_working_scale ? work_height : height;
    feature_.EnsureFeature(runtime, pass, command_list, create_width, create_height);
    return;
  }

  if (!model_pipeline::MeasureWhitePoint(
          command_,
          command_list,
          resources_,
          codec_.get(),
          model_input,
          width,
          height)) {
    return;
  }
  command_.Transition(command_list, resources_.GetWhitePointBuffer(),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

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
          work_height)) {
    return;
  }
  (void)use_working_scale;

  if (!FillEvaluateParams(runtime, pass, guides, model_color, model_output,
                          eval_width, eval_height, width, height)) {
    return;
  }

  runtime.EvaluateFeature(
      command_list,
      feature_.FeatureHandle(),
      runtime.Parameters().Get(),
      nullptr);

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
          pass.params)) {
    return;
  }

  ID3D12Resource* final_output = resources_.GetCompositeOutput();

  if (pass.debug_view != 0) {
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

    ID3D12Resource* debug_out = DisplayDebugView(
        command_, *codec_, command_list, resources_, debug,
        pass.debug_view, width, height);
    if (debug_out != nullptr) final_output = debug_out;
  }

  command_.Transition(command_list, final_output,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_RESOURCE_STATES out_state = InferResourceState(output_resource);
  command_.Transition(command_list, output_resource,
                      D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(output_resource, final_output);
  command_.Transition(command_list, output_resource, out_state);
  command_.Transition(command_list, final_output,
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
