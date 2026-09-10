#include "dlssnr_pass_session.h"

#include <algorithm>
#include <mutex>

#include <reshade.hpp>

#include "dlss_guides.h"
#include "dlssnr_stages_working_scale.h"
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
    unsigned int work_height,
    bool use_working_scale) {
  if (!resources_.EnsureOutput(device, desc) ||
      !resources_.EnsureCompositeOutput(device, desc)) {
    reason_ = "failed to create DLSSNR pass resources";
    return false;
  }

  if (use_working_scale &&
      !resources_.EnsureWorkingResources(device, desc, work_width, work_height)) {
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

  if (!EnsureFrameResources(device.Get(), desc, work_width, work_height,
                            use_working_scale)) {
    failed_ = true;
    return;
  }

  command_.Reset();
  command_.SetResourceState(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT);

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

  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = resources_.GetOutput();
  unsigned int eval_width = width;
  unsigned int eval_height = height;

  if (use_working_scale) {
    if (model_input == backbuffer.Get()) {
      command_.Transition(command_.GetCommandList(), backbuffer.Get(),
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (!dlssnr_stages_working_scale::PrepareInput(
            command_,
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
            true)) {
      failed_ = true;
      reason_ = "failed to prepare working scale input";
      return;
    }
  }

  if (!FillEvaluateParams(runtime, pass, guides, model_color, model_output,
                          eval_width, eval_height, width, height)) {
    return;
  }

  if (!use_working_scale) {
    command_.Transition(command_.GetCommandList(), backbuffer.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  const NVSDK_NGX_Result result =
      runtime.EvaluateFeature(
          command_.GetCommandList(),
          feature_.FeatureHandle(),
          runtime.Parameters().Get(),
          nullptr);

  if (!dlssnr_stages_working_scale::Apply(
          command_,
          resources_,
          codec_.get(),
          backbuffer.Get(),
          model_color,
          model_output,
          width,
          height,
          pass.params)) {
    failed_ = true;
    reason_ = "failed to apply post-model processing";
    return;
  }

  SubmitAndWait();

  if (result != NVSDK_NGX_Result_Success) {
    failed_ = true;
    reason_ = "DLSSNR EvaluateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }
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
