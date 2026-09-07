#include "dlssnr_direct.h"

#include <algorithm>
#include <filesystem>
#include <mutex>

#include <reshade.hpp>

#include "codec.h"
#include "dlss_guides.h"
#include "dlssnr_stages_debug.h"
#include "dlssnr_stages_ui_mask.h"
#include "dlssnr_stages_working_scale.h"
#include "ngx_parameter_block.h"
#include "settings.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_direct_mutex;
}

DlssNrDirect& DlssNrDirect::Get() {
  static DlssNrDirect direct;
  direct.pass_index_ = 0;
  return direct;
}

DlssNrDirect& DlssNrDirect::GetPass2() {
  static DlssNrDirect direct;
  direct.pass_index_ = 1;
  return direct;
}

void DlssNrDirect::SetDebugSelectedResource(uint64_t handle) {
  debug_selected_resource_handle_ = handle;
}

void DlssNrDirect::ClearSelectedPass() {
  debug_selected_resource_handle_ = 0;

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }
}

void DlssNrDirect::SetPendingSelectedPass(ID3D12Resource* resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
  }

  pending_selected_pass_ = resource;
}

bool DlssNrDirect::IsReady() const {
  return feature_.FeatureHandle() != nullptr && !failed_;
}

const char* DlssNrDirect::GetLastError() const {
  return failed_ ? reason_ : "";
}

void DlssNrDirect::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (failed_ || command_queue == nullptr || swapchain == nullptr) return;

  const PassSettings& pass = pass_index_ == 1 ? settings.pass2
                                              : settings.pass1;

  IDXGISwapChain3* swapchain3 = nullptr;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) || swapchain3 == nullptr) {
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;

  if (!runtime_.Initialize(device) || !command_.Ensure(device)) {
    failed_ = true;
    reason_ = runtime_.GetLastError();
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

  if (resources_.output == nullptr || resources_.output_width != width || resources_.output_height != height ||
      resources_.output->GetDesc().Format != desc.Format) {
    if (resources_.output != nullptr) {
      resources_.output->Release();
      resources_.output = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC output_desc = desc;
    output_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resources_.output)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR output texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    resources_.output_width = width;
    resources_.output_height = height;
  }

  if (resources_.ui_mask == nullptr || resources_.ui_mask_width != width || resources_.ui_mask_height != height ||
      resources_.ui_mask_format != desc.Format) {
    if (resources_.ui_mask != nullptr) {
      resources_.ui_mask->Release();
      resources_.ui_mask = nullptr;
    }

    D3D12_HEAP_PROPERTIES mask_heap = {};
    mask_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC mask_desc = desc;
    mask_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &mask_heap,
            D3D12_HEAP_FLAG_NONE,
            &mask_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resources_.ui_mask)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR UI mask texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    resources_.ui_mask_width = width;
    resources_.ui_mask_height = height;
    resources_.ui_mask_format = desc.Format;
  }

  if (resources_.composite_output == nullptr ||
      resources_.composite_output_width != width ||
      resources_.composite_output_height != height ||
      resources_.composite_output_format != desc.Format) {
    if (resources_.composite_output != nullptr) {
      resources_.composite_output->Release();
      resources_.composite_output = nullptr;
    }

    D3D12_HEAP_PROPERTIES composite_heap = {};
    composite_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC composite_desc = desc;
    composite_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &composite_heap,
            D3D12_HEAP_FLAG_NONE,
            &composite_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resources_.composite_output)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR composite output texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    resources_.composite_output_width = width;
    resources_.composite_output_height = height;
    resources_.composite_output_format = desc.Format;
  }

  if (use_working_scale &&
      (resources_.working_input == nullptr ||
       resources_.working_width != work_width ||
       resources_.working_height != work_height ||
       resources_.working_format != desc.Format)) {
    if (resources_.working_input != nullptr) {
      resources_.working_input->Release();
      resources_.working_input = nullptr;
    }
    if (resources_.working_output != nullptr) {
      resources_.working_output->Release();
      resources_.working_output = nullptr;
    }

    D3D12_HEAP_PROPERTIES working_heap = {};
    working_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC working_desc = desc;
    working_desc.Width = work_width;
    working_desc.Height = work_height;
    working_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(
            &working_heap,
            D3D12_HEAP_FLAG_NONE,
            &working_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resources_.working_input)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR working input texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    if (FAILED(device->CreateCommittedResource(
            &working_heap,
            D3D12_HEAP_FLAG_NONE,
            &working_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resources_.working_output)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR working output texture";
      resources_.working_input->Release();
      resources_.working_input = nullptr;
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    resources_.working_width = work_width;
    resources_.working_height = work_height;
    resources_.working_format = desc.Format;
  }

  command_.Reset();

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().CaptureSelectedPassIfNeeded(
        command_.GetCommandList(),
        pending_selected_pass_);

    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  if (feature_.FeatureHandle() == nullptr) {
    const unsigned int create_width = use_working_scale ? work_width : width;
    const unsigned int create_height = use_working_scale ? work_height : height;
    if (!feature_.EnsureFeature(runtime_, pass, command_.GetCommandList(), create_width, create_height)) {
      failed_ = true;
      command_.GetCommandList()->Close();
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    command_.GetCommandList()->Close();
    ID3D12CommandList* const lists[] = {command_.GetCommandList()};
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

  const bool use_selected_input =
      settings.use_selected_pass_as_input &&
      guides.selected_pass_color != nullptr;

  ID3D12Resource* model_input = use_selected_input
                                    ? guides.selected_pass_color
                                    : backbuffer;

  // The UI mask is always generated from the selected clean pass when one is
  // available. Without a selected pass, no UI mask is used at all.
  ID3D12Resource* pre_ui_source = guides.selected_pass_color;

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
      reshade::log::message(reshade::log::level::error, reason_);
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
    mask_constants.UiMaskThreshold = settings.ui_mask_threshold;
    mask_constants.UiMaskSoftness = settings.ui_mask_softness;

    debug_codec_->DispatchMask(
        command_.GetCommandList(),
        mask_constants,
        pre_ui_source,
        backbuffer,
        resources_.ui_mask);

    command_.Barrier(command_.GetCommandList(), resources_.ui_mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    command_.GetCommandList()->CopyResource(backbuffer, resources_.ui_mask);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_.GetCommandList()->Close();
    ID3D12CommandList* const lists[] = {command_.GetCommandList()};
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
    if (!debug_codec_->InitDownsampleShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize downsample shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    if (model_input == backbuffer) {
      command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_PRESENT,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    DlssNrConstants downsample_constants = {};
    downsample_constants.Mode = DlssNrMode_Downsample;
    downsample_constants.Width = work_width;
    downsample_constants.Height = work_height;

    debug_codec_->DispatchDownsample(
        command_.GetCommandList(),
        downsample_constants,
        model_input,
        resources_.working_input);

    command_.Barrier(command_.GetCommandList(), resources_.working_input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    model_color = resources_.working_input;
    model_output = resources_.working_output;
    eval_width = work_width;
    eval_height = work_height;
  }

  auto* params = runtime_.Parameters().Get();
  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return;
  }

  params->Reset();

  NgxParameterBlock& parameter_block = runtime_.Parameters();

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
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  if (!use_working_scale) {
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  const NVSDK_NGX_Result result =
      runtime_.EvaluateFeature(command_.GetCommandList(), feature_.FeatureHandle(), params, nullptr);

  if (resources_.ui_mask != nullptr) {
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  if (use_working_scale &&
      (settings.debug_view == 7 || settings.debug_view == 8 ||
       settings.debug_view == 9)) {
    const bool debug_residual = settings.debug_view == 9;

    if (!debug_residual) {
      ID3D12Resource* debug_source =
          settings.debug_view == 7 ? resources_.working_input : resources_.working_output;

      if (debug_source != nullptr) {
        command_.Barrier(command_.GetCommandList(), debug_source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        command_.GetCommandList()->CopyResource(backbuffer, debug_source);
        command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PRESENT);
        command_.Barrier(command_.GetCommandList(), debug_source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
    } else {
      if (!debug_codec_->InitUpsampleResidualShader(device)) {
        failed_ = true;
        reason_ = "failed to initialize residual debug shader";
        backbuffer->Release();
        swapchain3->Release();
        device->Release();
        return;
      }

      DlssNrConstants residual_constants = {};
      residual_constants.Mode = DlssNrMode_Resolve;
      residual_constants.Width = width;
      residual_constants.Height = height;
      residual_constants.LumaStrength = 1.0f;
      residual_constants.ChromaStrength = 1.0f;
      residual_constants.ResidualSuppression = pass.residual_suppression;
      residual_constants.ResidualThreshold = pass.residual_threshold;

      command_.Barrier(command_.GetCommandList(), resources_.working_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      command_.Barrier(command_.GetCommandList(), resources_.working_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      debug_codec_->DispatchUpsampleResidual(
          command_.GetCommandList(),
          residual_constants,
          resources_.working_input,
          resources_.working_input,
          resources_.working_output,
          resources_.composite_output);

      command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
      command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_COPY_DEST);
      command_.GetCommandList()->CopyResource(backbuffer, resources_.composite_output);
      command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_PRESENT);
      command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      command_.Barrier(command_.GetCommandList(), resources_.working_output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    command_.GetCommandList()->Close();
    ID3D12CommandList* const lists[] = {command_.GetCommandList()};
    command_queue_->ExecuteCommandLists(1, lists);
    command_.WaitGpu(command_queue_);

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (use_working_scale) {
    if (!debug_codec_->InitUpsampleResidualShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize upsample residual shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants upsample_constants = {};
    upsample_constants.Mode = DlssNrMode_Resolve;
    upsample_constants.Width = width;
    upsample_constants.Height = height;
    upsample_constants.LumaStrength = pass.luma_strength;
    upsample_constants.ChromaStrength = pass.chroma_strength;
    upsample_constants.ResidualSuppression = pass.residual_suppression;
    upsample_constants.ResidualThreshold = pass.residual_threshold;

    command_.Barrier(command_.GetCommandList(), resources_.working_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.Barrier(command_.GetCommandList(), resources_.working_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchUpsampleResidual(
        command_.GetCommandList(),
        upsample_constants,
        backbuffer,
        resources_.working_input,
        resources_.working_output,
        resources_.composite_output);

    command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    command_.GetCommandList()->CopyResource(backbuffer, resources_.composite_output);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PRESENT);
    command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_.Barrier(command_.GetCommandList(), resources_.working_output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else if (use_selected_input && ui_mask_generated &&
      resources_.ui_mask != nullptr && resources_.composite_output != nullptr) {
    if (!debug_codec_->InitCompositeShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize UI composite shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants composite_constants = {};
    composite_constants.Mode = DlssNrMode_Encode;
    composite_constants.WhitePoint = 1.0f;
    composite_constants.Width = width;
    composite_constants.Height = height;
    composite_constants.Passthrough = 1u;
    composite_constants.LumaStrength = pass.luma_strength;
    composite_constants.ChromaStrength = pass.chroma_strength;

    command_.Barrier(command_.GetCommandList(), resources_.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchComposite(
        command_.GetCommandList(),
        composite_constants,
        resources_.output,
        backbuffer,
        resources_.ui_mask,
        resources_.composite_output);

    command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_.GetCommandList()->CopyResource(backbuffer, resources_.composite_output);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_.Barrier(command_.GetCommandList(), resources_.ui_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_.Barrier(command_.GetCommandList(), resources_.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else if ((pass.luma_strength != 1.0f || pass.chroma_strength != 1.0f) &&
             resources_.composite_output != nullptr) {
    if (!debug_codec_->InitLumaColorShader(device)) {
      failed_ = true;
      reason_ = "failed to initialize luma color composite shader";
      reshade::log::message(reshade::log::level::error, reason_);
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants luma_color_constants = {};
    luma_color_constants.Mode = DlssNrMode_Encode;
    luma_color_constants.WhitePoint = 1.0f;
    luma_color_constants.Width = width;
    luma_color_constants.Height = height;
    luma_color_constants.Passthrough = 1u;
    luma_color_constants.LumaStrength = pass.luma_strength;
    luma_color_constants.ChromaStrength = pass.chroma_strength;

    command_.Barrier(command_.GetCommandList(), resources_.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchLumaColor(
        command_.GetCommandList(),
        luma_color_constants,
        resources_.output,
        backbuffer,
        resources_.composite_output);

    command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_.GetCommandList()->CopyResource(backbuffer, resources_.composite_output);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    command_.Barrier(command_.GetCommandList(), resources_.composite_output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_.Barrier(command_.GetCommandList(), resources_.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    command_.Barrier(command_.GetCommandList(), resources_.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_.GetCommandList()->CopyResource(backbuffer, resources_.output);
    command_.Barrier(command_.GetCommandList(), backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    command_.Barrier(command_.GetCommandList(), resources_.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  command_.GetCommandList()->Close();
  ID3D12CommandList* const lists[] = {command_.GetCommandList()};
  command_queue_->ExecuteCommandLists(1, lists);
  command_.WaitGpu(command_queue_);

  static bool reported_first_evaluate = false;
  if (!reported_first_evaluate) {
    reported_first_evaluate = true;
    std::string message = "Direct DLSSNR evaluate result: 0x";
    char buffer[16] = {};
    sprintf_s(buffer, "%X", static_cast<unsigned int>(result));
    message += buffer;
    reshade::log::message(reshade::log::level::info, message.c_str());
  }

  if (result != NVSDK_NGX_Result_Success) {
    failed_ = true;
    reason_ = "DLSSNR EvaluateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
  }

  backbuffer->Release();
  swapchain3->Release();
  device->Release();
}

void DlssNrDirect::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  feature_.Shutdown(runtime_);

  if (resources_.output != nullptr) {
    resources_.output->Release();
    resources_.output = nullptr;
  }

  if (resources_.ui_mask != nullptr) {
    resources_.ui_mask->Release();
    resources_.ui_mask = nullptr;
  }

  if (resources_.composite_output != nullptr) {
    resources_.composite_output->Release();
    resources_.composite_output = nullptr;
  }

  if (resources_.working_input != nullptr) {
    resources_.working_input->Release();
    resources_.working_input = nullptr;
  }

  if (resources_.working_output != nullptr) {
    resources_.working_output->Release();
    resources_.working_output = nullptr;
  }

  resources_.ui_mask_width = 0;
  resources_.ui_mask_height = 0;
  resources_.ui_mask_format = DXGI_FORMAT_UNKNOWN;
  resources_.composite_output_width = 0;
  resources_.composite_output_height = 0;
  resources_.composite_output_format = DXGI_FORMAT_UNKNOWN;
  resources_.working_width = 0;
  resources_.working_height = 0;
  resources_.working_format = DXGI_FORMAT_UNKNOWN;

  command_.Shutdown();

  command_queue_ = nullptr;
  resources_.output_width = 0;
  resources_.output_height = 0;

  runtime_.Shutdown();

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  failed_ = false;
  reason_ = "";
  pass_index_ = 0;
}

}  // namespace reshade_dlssnr
