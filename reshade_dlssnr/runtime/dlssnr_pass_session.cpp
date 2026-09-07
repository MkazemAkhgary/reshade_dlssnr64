#include "dlssnr_pass_session.h"

#include <algorithm>
#include <mutex>

#include <reshade.hpp>

#include "dlss_guides.h"
#include "dlssnr_stages_debug.h"
#include "dlssnr_stages_final_composite.h"
#include "dlssnr_stages_ui_mask.h"
#include "dlssnr_stages_working_scale.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_session_mutex;
}

void DlssNrPassSession::SetPendingSelectedPass(ID3D12Resource* resource) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
  }

  pending_selected_pass_ = resource;
}

bool DlssNrPassSession::IsReady() const {
  return feature_.HasFeature() && !failed_;
}

const char* DlssNrPassSession::GetLastError() const {
  return failed_ ? reason_ : "";
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

  IDXGISwapChain3* swapchain3 = nullptr;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
      swapchain3 == nullptr) {
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return;
  }

  if (!runtime.Initialize(device) || !command_.Ensure(device)) {
    failed_ = true;
    reason_ = runtime.GetLastError();
    reshade::log::message(reshade::log::level::error, reason_);
    swapchain3->Release();
    device->Release();
    return;
  }

  command_queue_ = command_queue;

  const UINT back_buffer_index = swapchain3->GetCurrentBackBufferIndex();
  ID3D12Resource* backbuffer = nullptr;

  if (FAILED(swapchain3->GetBuffer(
          back_buffer_index,
          IID_PPV_ARGS(&backbuffer))) || backbuffer == nullptr) {
    swapchain3->Release();
    device->Release();
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

  if (!resources_.EnsureOutput(device, desc) ||
      !resources_.EnsureUiMask(device, desc) ||
      !resources_.EnsureCompositeOutput(device, desc)) {
    failed_ = true;
    reason_ = "failed to create DLSSNR pass resources";
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (use_working_scale &&
      !resources_.EnsureWorkingResources(device, desc, work_width, work_height)) {
    failed_ = true;
    reason_ = "failed to create DLSSNR working resources";
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  command_.Reset();

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().CaptureSelectedPassIfNeeded(
        command_.GetCommandList(),
        pending_selected_pass_);

    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

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
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    command_.GetCommandList()->Close();
    ID3D12CommandList* lists[] = {command_.GetCommandList()};
    command_queue_->ExecuteCommandLists(1, lists);
    command_.WaitGpu(command_queue_);

    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created; skipping evaluation this frame");
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  const DlssGuideState guides = DlssGuides::Get().GetState();

  const bool selected_pass_size_ok =
      guides.selected_pass_color != nullptr &&
      guides.selected_width == width &&
      guides.selected_height == height;

  const bool use_selected_input =
      settings.use_selected_pass_as_input &&
      selected_pass_size_ok;

  ID3D12Resource* model_input =
      use_selected_input ? guides.selected_pass_color : backbuffer;

  ID3D12Resource* pre_ui_source =
      selected_pass_size_ok ? guides.selected_pass_color : nullptr;

  if (debug_codec_ == nullptr) {
    debug_codec_ = std::make_unique<DlssNrCodec>();
  }

  if (!debug_codec_->IsInit() && !debug_codec_->Init(device)) {
    failed_ = true;
    reason_ = "failed to initialize debug codec";
    reshade::log::message(reshade::log::level::error, reason_);
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (settings.debug_view == 1 ||
      ((settings.debug_view == 2 || settings.debug_view == 3 ||
        settings.debug_view == 5 || settings.debug_view == 6) &&
       (guides.valid || settings.debug_view == 6))) {
    if (dlssnr_stages_debug::Run(
            command_,
            resources_,
            debug_codec_.get(),
            device,
            command_queue_,
            backbuffer,
            guides,
            model_input,
            width,
            height,
            settings.debug_view)) {
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (settings.debug_view == 4 && guides.valid &&
      pre_ui_source != nullptr && resources_.ui_mask != nullptr) {
    if (!debug_codec_->InitMaskShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize UI mask shader";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants mask_constants = {};
    mask_constants.Mode = DlssNrMode_Encode;
    mask_constants.WhitePoint = 1.0f;
    mask_constants.Width = width;
    mask_constants.Height = height;
    mask_constants.Passthrough = 1u;
    mask_constants.UiMaskThreshold = settings.ui_mask_threshold;
    mask_constants.UiMaskSoftness = settings.ui_mask_softness;

    debug_codec_->DispatchMask(
        command_.GetCommandList(),
        mask_constants,
        pre_ui_source,
        backbuffer,
        resources_.ui_mask);

    command_.Barrier(command_.GetCommandList(), resources_.ui_mask,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer,
                     D3D12_RESOURCE_STATE_PRESENT,
                     D3D12_RESOURCE_STATE_COPY_DEST);
    command_.GetCommandList()->CopyResource(backbuffer, resources_.ui_mask);
    command_.Barrier(command_.GetCommandList(), backbuffer,
                     D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_PRESENT);
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask,
                     D3D12_RESOURCE_STATE_COPY_SOURCE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_.GetCommandList()->Close();
    ID3D12CommandList* lists[] = {command_.GetCommandList()};
    command_queue_->ExecuteCommandLists(1, lists);
    command_.WaitGpu(command_queue_);

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (settings.debug_view == 4) {
    if (pre_ui_source == nullptr || resources_.ui_mask == nullptr ||
        !guides.valid) {
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }
  }

  bool ui_mask_generated = false;

  if (guides.valid && pre_ui_source != nullptr && resources_.ui_mask != nullptr) {
    ui_mask_generated = dlssnr_stages_ui_mask::Generate(
        command_,
        resources_,
        debug_codec_.get(),
        device,
        pre_ui_source,
        backbuffer,
        width,
        height,
        settings.ui_mask_threshold,
        settings.ui_mask_softness);
  }

  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = resources_.output;
  unsigned int eval_width = width;
  unsigned int eval_height = height;

  if (use_working_scale) {
    if (model_input == backbuffer) {
      command_.Barrier(command_.GetCommandList(), backbuffer,
                       D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (!dlssnr_stages_working_scale::PrepareInput(
            command_,
            resources_,
            debug_codec_.get(),
            device,
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
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
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

  if (ui_mask_generated && !use_selected_input && resources_.ui_mask != nullptr) {
    parameter_block.SetResource("DLSSNR.UIAlpha", resources_.ui_mask);
    parameter_block.SetResource("DLSSNR.Backbuffer", backbuffer);
  }

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

  if (resources_.ui_mask != nullptr) {
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  if (!use_working_scale) {
    command_.Barrier(command_.GetCommandList(), backbuffer,
                     D3D12_RESOURCE_STATE_PRESENT,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  const NVSDK_NGX_Result result =
      runtime.EvaluateFeature(
          command_.GetCommandList(),
          feature_.FeatureHandle(),
          params,
          nullptr);

  if (resources_.ui_mask != nullptr) {
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  if (settings.debug_view == 7 || settings.debug_view == 8 ||
      settings.debug_view == 9) {
    if (!dlssnr_stages_working_scale::RunDebugView(
            command_,
            resources_,
            debug_codec_.get(),
            device,
            command_queue_,
            backbuffer,
            model_color,
            model_output,
            width,
            height,
            settings.debug_view)) {
      failed_ = true;
      reason_ = "failed to run working debug view";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (use_working_scale) {
    if (!dlssnr_stages_working_scale::Apply(
            command_,
            resources_,
            debug_codec_.get(),
            device,
            backbuffer,
            width,
            height,
            work_width,
            work_height,
            true,
            pass.luma_strength,
            pass.chroma_strength,
            pass.residual_suppression,
            pass.residual_threshold,
            pass.white_point_scale,
            settings.debug_view)) {
      failed_ = true;
      reason_ = "failed to apply working scale";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    command_.GetCommandList()->Close();
    ID3D12CommandList* lists[] = {command_.GetCommandList()};
    command_queue_->ExecuteCommandLists(1, lists);
    command_.WaitGpu(command_queue_);

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (!dlssnr_stages_final_composite::Apply(
          command_,
          resources_,
          debug_codec_.get(),
          device,
          backbuffer,
          width,
          height,
          use_selected_input,
          ui_mask_generated,
          pass.luma_strength,
          pass.chroma_strength,
          pass.white_point_scale)) {
    failed_ = true;
    reason_ = "failed to apply final composite";
    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  command_.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {command_.GetCommandList()};
  command_queue_->ExecuteCommandLists(1, lists);
  command_.WaitGpu(command_queue_);

  if (result != NVSDK_NGX_Result_Success) {
    failed_ = true;
    reason_ = "DLSSNR EvaluateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }

  backbuffer->Release();
  swapchain3->Release();
  device->Release();
}

void DlssNrPassSession::Shutdown(DlssNrRuntime& runtime) {
  std::lock_guard<std::mutex> lock(g_session_mutex);

  feature_.Shutdown(runtime);
  resources_.Shutdown();
  command_.Shutdown();

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  debug_codec_.reset();
  command_queue_ = nullptr;
  failed_ = false;
  reason_ = "";
}

}  // namespace reshade_dlssnr
