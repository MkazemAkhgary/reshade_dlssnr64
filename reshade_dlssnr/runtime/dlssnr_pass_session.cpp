#include "dlssnr_pass_session.h"

#include <algorithm>
#include <mutex>

#include <reshade.hpp>

#include "dlss_guides.h"
#include "dlssnr_stages_working_scale.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_session_mutex;
}

void DlssNrPassSession::OnPresent(
    DlssNrRuntime& runtime,
    const PassSettings& pass,
    const Settings& settings,
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  if (!settings.enabled) return;
  if (failed_ || command_queue == nullptr || swapchain == nullptr) return;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
      swapchain3 == nullptr) {
    return;
  }

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return;
  }

  if (!runtime.Initialize(device.Get()) || !command_.Ensure(device.Get())) {
    failed_ = true;
    reason_ = runtime.GetLastError();
    reshade::log::message(reshade::log::level::error, reason_);
    return;
  }

  command_queue_ = command_queue;

  const UINT back_buffer_index = swapchain3->GetCurrentBackBufferIndex();
  Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;

  if (FAILED(swapchain3->GetBuffer(
          back_buffer_index,
          IID_PPV_ARGS(&backbuffer))) || backbuffer == nullptr) {
    return;
  }

  const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = static_cast<unsigned int>(desc.Height);

  const float working_scale = std::clamp(pass.working_scale, 0.25f, 1.0f);
  const unsigned int work_width =
      std::max(1u, static_cast<unsigned int>(width * working_scale + 0.5f));
  const unsigned int work_height =
      std::max(1u, static_cast<unsigned int>(height * working_scale + 0.5f));
  const bool use_working_scale =
      work_width != width || work_height != height;

  if (!resources_.EnsureOutput(device.Get(), desc) ||
      !resources_.EnsureCompositeOutput(device.Get(), desc)) {
    failed_ = true;
    reason_ = "failed to create DLSSNR pass resources";
    return;
  }

  if (use_working_scale &&
      !resources_.EnsureWorkingResources(device.Get(), desc, work_width, work_height)) {
    failed_ = true;
    reason_ = "failed to create DLSSNR working resources";
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

    command_.GetCommandList()->Close();
    ID3D12CommandList* lists[] = {command_.GetCommandList()};
    command_queue_->ExecuteCommandLists(1, lists);
    command_.WaitGpu(command_queue_);
    guides_->ReleaseRetiredGameResources();

    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created; skipping evaluation this frame");
    return;
  }

  const DlssGuideState guides = guides_->GetState();

  ID3D12Resource* model_input = backbuffer.Get();

  if (debug_codec_ == nullptr) {
    debug_codec_ = std::make_unique<DlssNrCodec>();
  }

  if (!debug_codec_->IsInit() && !debug_codec_->Init(device.Get())) {
    failed_ = true;
    reason_ = "failed to initialize debug codec";
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
            debug_codec_.get(),
            device.Get(),
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

  auto* params = runtime.Parameters().Get();
  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return;
  }

  params->Reset();

  NgxParameterBlock& parameter_block = runtime.Parameters();

  parameter_block.SetResource("DLSSNR.Color", model_color);
  parameter_block.SetResource("DLSSNR.Output", model_output);
  parameter_block.SetUInt("DLSSNR.Enabled", 1u);
  parameter_block.SetUInt("DLSSNR.Width", eval_width);
  parameter_block.SetUInt("DLSSNR.Height", eval_height);
  parameter_block.SetUInt("DLSSNR.InputWidth", width);
  parameter_block.SetUInt("DLSSNR.InputHeight", height);
  parameter_block.SetUInt("DLSSNR.OutputWidth", eval_width);
  parameter_block.SetUInt("DLSSNR.OutputHeight", eval_height);
  parameter_block.SetUInt("DLSSNR.Upscaling", 1u);
  parameter_block.SetFloat("DLSSNR.ScalingRatio", 1.0f);
  parameter_block.SetFloat("DLSSNR.Scale", 1.0f);
  parameter_block.SetUInt(
      "DLSSNR.Hint.Render.Preset",
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2)) + 1u);
  parameter_block.SetFloat("DLSSNR.Intensity", pass.intensity);
  parameter_block.SetUInt(
      "DLSSNR.Style",
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2)));
  parameter_block.SetFloat(
      "DLSSNR.LocalStructureStrength", pass.local_structure);
  parameter_block.SetFloat("DLSSNR.LocalToneStrength", pass.local_tone);
  parameter_block.SetFloat(
      "DLSSNR.SkinStructureStrength",
      pass.auto_mask ? pass.skin_structure : -1.0f);
  parameter_block.SetUInt(
      "DLSSNR.UseAutoMask", pass.auto_mask ? 1u : 0u);
  parameter_block.SetUInt(
      "DLSSNR.UICorrection", pass.ui_correction ? 1u : 0u);

  // UI mask capability is intentionally unused for now. The model supports:
  //   parameter_block.SetResource("DLSSNR.UIAlpha", <ui_mask_resource>);
  //   parameter_block.SetResource("DLSSNR.Backbuffer", <backbuffer>);
  // which will be re-enabled when a reliable UI mask source is available.

  if (guides.valid) {
    parameter_block.SetResource("DLSSNR.Depth", guides.depth);
    parameter_block.SetResource("DLSSNR.MVec", guides.motion);
    parameter_block.SetUInt(
        "DLSSNR.DepthInverted", guides.depth_inverted ? 1u : 0u);
    parameter_block.SetFloat("DLSSNR.MVecScaleX", guides.mv_scale_x);
    parameter_block.SetFloat("DLSSNR.MVecScaleY", guides.mv_scale_y);

    parameter_block.SetUInt("DLSSNR.ColorSubrectBaseX", 0u);
    parameter_block.SetUInt("DLSSNR.ColorSubrectBaseY", 0u);
    parameter_block.SetUInt("DLSSNR.ColorSubrectWidth", eval_width);
    parameter_block.SetUInt("DLSSNR.ColorSubrectHeight", eval_height);
    parameter_block.SetUInt("DLSSNR.OutputSubrectBaseX", 0u);
    parameter_block.SetUInt("DLSSNR.OutputSubrectBaseY", 0u);
    parameter_block.SetUInt("DLSSNR.OutputSubrectWidth", eval_width);
    parameter_block.SetUInt("DLSSNR.OutputSubrectHeight", eval_height);
    parameter_block.SetUInt("DLSSNR.DepthSubrectBaseX", 0u);
    parameter_block.SetUInt("DLSSNR.DepthSubrectBaseY", 0u);
    parameter_block.SetUInt("DLSSNR.DepthSubrectWidth", guides.guide_width);
    parameter_block.SetUInt("DLSSNR.DepthSubrectHeight", guides.guide_height);
    parameter_block.SetUInt("DLSSNR.MVecSubrectBaseX", 0u);
    parameter_block.SetUInt("DLSSNR.MVecSubrectBaseY", 0u);
    parameter_block.SetUInt("DLSSNR.MVecSubrectWidth", guides.guide_width);
    parameter_block.SetUInt("DLSSNR.MVecSubrectHeight", guides.guide_height);
  }

  parameter_block.SetUInt("DLSSNR.Reset", feature_.ShouldReset() ? 1u : 0u);
  feature_.MarkEvaluated();

  if (!use_working_scale) {
    command_.Transition(command_.GetCommandList(), backbuffer.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  const NVSDK_NGX_Result result =
      runtime.EvaluateFeature(
          command_.GetCommandList(),
          feature_.FeatureHandle(),
          params,
          nullptr);

  if (!dlssnr_stages_working_scale::Apply(
          command_,
          resources_,
          debug_codec_.get(),
          device.Get(),
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

  command_.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {command_.GetCommandList()};
  command_queue_->ExecuteCommandLists(1, lists);
  command_.WaitGpu(command_queue_);
  guides_->ReleaseRetiredGameResources();

  if (result != NVSDK_NGX_Result_Success) {
    failed_ = true;
    reason_ = "DLSSNR EvaluateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }
}

void DlssNrPassSession::Shutdown(DlssNrRuntime& runtime) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  feature_.Shutdown(runtime);
  resources_.Shutdown();
  command_.Shutdown();

  debug_codec_.reset();
  command_queue_ = nullptr;
  failed_ = false;
  reason_ = "";
}

}  // namespace reshade_dlssnr
