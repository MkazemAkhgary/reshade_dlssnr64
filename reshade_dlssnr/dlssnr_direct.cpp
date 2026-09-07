#include "dlssnr_direct.h"

#include <algorithm>
#include <filesystem>
#include <mutex>

#include <reshade.hpp>

#include "codec.h"
#include "dlss_guides.h"
#include "ngx_parameter_block.h"
#include "settings.h"

namespace reshade_dlssnr {

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
  return feature_handle_ != nullptr && !failed_;
}

const char* DlssNrDirect::GetLastError() const {
  return failed_ ? reason_ : "";
}

bool DlssNrDirect::EnsureFeature(
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  const PassSettings& pass = pass_index_ == 1 ? GetSettings().pass2
                                              : GetSettings().pass1;

  const unsigned int model_value =
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2));
  const bool tuning_changed =
      built_preset_ != model_value ||
      built_intensity_ != pass.intensity ||
      built_local_structure_ != pass.local_structure ||
      built_local_tone_ != pass.local_tone ||
      built_skin_structure_ != pass.skin_structure ||
      built_auto_mask_ != pass.auto_mask ||
      built_ui_correction_ != pass.ui_correction;

  if (feature_handle_ != nullptr && built_width_ == width && built_height_ == height &&
      !tuning_changed) {
    return true;
  }

  if (feature_handle_ != nullptr) {
    runtime_.ReleaseFeature(feature_handle_);
    feature_handle_ = nullptr;
    first_evaluate_ = true;
  }

  auto* params = runtime_.Parameters().Get();
  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return false;
  }

  params->Reset();

  NgxParameterBlock& parameter_block = runtime_.Parameters();

  parameter_block.SetUInt("DLSSNR.Enabled", 1u);
  parameter_block.SetUInt("DLSSNR.Width", width);
  parameter_block.SetUInt("DLSSNR.Height", height);
  parameter_block.SetUInt("DLSSNR.InputWidth", width);
  parameter_block.SetUInt("DLSSNR.InputHeight", height);
  parameter_block.SetUInt("DLSSNR.OutputWidth", width);
  parameter_block.SetUInt("DLSSNR.OutputHeight", height);
  parameter_block.SetUInt("DLSSNR.Upscaling", 1u);
  parameter_block.SetFloat("DLSSNR.ScalingRatio", 1.0f);
  parameter_block.SetFloat("DLSSNR.Scale", 1.0f);
  parameter_block.SetUInt("DLSSNR.Hint.Render.Preset", model_value + 1u);
  parameter_block.SetUInt(NVSDK_NGX_Parameter_PerfQualityValue, 2u);
  parameter_block.SetFloat("DLSSNR.Intensity", pass.intensity);
  parameter_block.SetUInt("DLSSNR.Style", model_value);
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
  parameter_block.SetUInt("CreationNodeMask", 1u);
  parameter_block.SetUInt("VisibilityNodeMask", 1u);

  const NVSDK_NGX_Result result = runtime_.CreateFeature(
      command_list,
      static_cast<NVSDK_NGX_Feature>(18),
      params,
      &feature_handle_);

  if (result != NVSDK_NGX_Result_Success || feature_handle_ == nullptr) {
    feature_handle_ = nullptr;
    reason_ = "DLSSNR CreateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  built_width_ = width;
  built_height_ = height;
  built_working_scale_ = pass.working_scale;
  built_preset_ = model_value;
  built_intensity_ = pass.intensity;
  built_local_structure_ = pass.local_structure;
  built_local_tone_ = pass.local_tone;
  built_skin_structure_ = pass.skin_structure;
  built_auto_mask_ = pass.auto_mask;
  built_ui_correction_ = pass.ui_correction;

  std::string msg = "Direct DLSSNR feature created at ";
  msg += std::to_string(width) + "x" + std::to_string(height);
  reshade::log::message(reshade::log::level::info, msg.c_str());

  return true;
}

void DlssNrDirect::Barrier(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  if (before == after || resource == nullptr) return;

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);
}

bool DlssNrDirect::EnsureCommandInfrastructure(ID3D12Device* device) {
  if (command_allocator_ != nullptr) return true;
  if (device == nullptr) return false;

  if (FAILED(device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&command_allocator_)))) {
    reason_ = "failed to create DLSSNR command allocator";
    return false;
  }

  if (FAILED(device->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          command_allocator_,
          nullptr,
          IID_PPV_ARGS(&command_list_)))) {
    reason_ = "failed to create DLSSNR command list";
    return false;
  }

  command_list_->Close();

  if (FAILED(device->CreateFence(
          0,
          D3D12_FENCE_FLAG_NONE,
          IID_PPV_ARGS(&fence_)))) {
    reason_ = "failed to create DLSSNR fence";
    return false;
  }

  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    reason_ = "failed to create DLSSNR fence event";
    return false;
  }

  return true;
}

void DlssNrDirect::WaitGpu() {
  if (command_queue_ == nullptr || fence_ == nullptr) return;

  const UINT64 value = ++fence_value_;
  command_queue_->Signal(fence_, value);

  if (fence_->GetCompletedValue() < value) {
    fence_->SetEventOnCompletion(value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }

  DlssGuides::Get().ReleaseRetiredGameResources();
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

  if (!EnsureModelAndPatch() || !EnsureInitialized(device) ||
      !EnsureParameterBlock() || !EnsureCommandInfrastructure(device)) {
    failed_ = true;
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

  if (output_ == nullptr || output_width_ != width || output_height_ != height ||
      output_->GetDesc().Format != desc.Format) {
    if (output_ != nullptr) {
      output_->Release();
      output_ = nullptr;
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
            IID_PPV_ARGS(&output_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR output texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    output_width_ = width;
    output_height_ = height;
  }

  if (ui_mask_ == nullptr || ui_mask_width_ != width || ui_mask_height_ != height ||
      ui_mask_format_ != desc.Format) {
    if (ui_mask_ != nullptr) {
      ui_mask_->Release();
      ui_mask_ = nullptr;
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
            IID_PPV_ARGS(&ui_mask_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR UI mask texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    ui_mask_width_ = width;
    ui_mask_height_ = height;
    ui_mask_format_ = desc.Format;
  }

  if (composite_output_ == nullptr ||
      composite_output_width_ != width ||
      composite_output_height_ != height ||
      composite_output_format_ != desc.Format) {
    if (composite_output_ != nullptr) {
      composite_output_->Release();
      composite_output_ = nullptr;
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
            IID_PPV_ARGS(&composite_output_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR composite output texture";
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    composite_output_width_ = width;
    composite_output_height_ = height;
    composite_output_format_ = desc.Format;
  }

  if (use_working_scale &&
      (working_input_ == nullptr ||
       working_resource_width_ != work_width ||
       working_resource_height_ != work_height ||
       working_resource_format_ != desc.Format)) {
    if (working_input_ != nullptr) {
      working_input_->Release();
      working_input_ = nullptr;
    }
    if (working_output_ != nullptr) {
      working_output_->Release();
      working_output_ = nullptr;
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
            IID_PPV_ARGS(&working_input_)))) {
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
            IID_PPV_ARGS(&working_output_)))) {
      failed_ = true;
      reason_ = "failed to create DLSSNR working output texture";
      working_input_->Release();
      working_input_ = nullptr;
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    working_resource_width_ = work_width;
    working_resource_height_ = work_height;
    working_resource_format_ = desc.Format;
  }

  if (feature_handle_ != nullptr &&
      (built_width_ != work_width ||
       built_height_ != work_height ||
       built_working_scale_ != working_scale)) {
    release_feature_(feature_handle_);
    feature_handle_ = nullptr;
    first_evaluate_ = true;
  }

  command_allocator_->Reset();
  command_list_->Reset(command_allocator_, nullptr);

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().CaptureSelectedPassIfNeeded(
        command_list_,
        pending_selected_pass_);

    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  if (feature_handle_ == nullptr) {
    const unsigned int create_width = use_working_scale ? work_width : width;
    const unsigned int create_height = use_working_scale ? work_height : height;
    if (!EnsureFeature(command_list_, create_width, create_height)) {
      failed_ = true;
      command_list_->Close();
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

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
    command_allocator_->Reset();
    command_list_->Reset(command_allocator_, nullptr);

    ID3D12Resource* debug_input = nullptr;
    unsigned int debug_guide_width = width;
    unsigned int debug_guide_height = height;

    if (settings.debug_view == 1) {
      debug_input = model_input;
      debug_guide_width = width;
      debug_guide_height = height;
    } else if (settings.debug_view == 2) {
      debug_input = guides.depth;
      debug_guide_width = guides.guide_width;
      debug_guide_height = guides.guide_height;
    } else if (settings.debug_view == 3) {
      debug_input = guides.motion;
      debug_guide_width = guides.guide_width;
      debug_guide_height = guides.guide_height;
    } else if (settings.debug_view == 5) {
      debug_input = guides.pre_ui_color;
      debug_guide_width = width;
      debug_guide_height = height;
    } else if (settings.debug_view == 6) {
      debug_input = guides.selected_pass_color;
      debug_guide_width = guides.selected_width != 0 ? guides.selected_width : width;
      debug_guide_height = guides.selected_height != 0 ? guides.selected_height : height;
    }

    if (debug_input == nullptr) {
      backbuffer->Release();
      swapchain3->Release();
      device->Release();
      return;
    }

    DlssNrConstants debug_constants = {};
    debug_constants.Mode = DlssNrMode_Resolve;
    debug_constants.DebugView = 1;
    debug_constants.Passthrough = 1;
    debug_constants.WhitePoint = 1.0f;
    debug_constants.Width = width;
    debug_constants.Height = height;
    debug_constants.GuideWidth = debug_guide_width;
    debug_constants.GuideHeight = debug_guide_height;
    debug_constants.MvScaleX = 1.0f;
    debug_constants.MvScaleY = 1.0f;

    debug_codec_->Dispatch(
        command_list_,
        debug_constants,
        debug_input,
        debug_input,
        debug_input,
        nullptr,
        nullptr,
        output_,
        nullptr);

    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  if (settings.debug_view == 4 && guides.valid &&
      pre_ui_source != nullptr && ui_mask_ != nullptr) {
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
        command_list_,
        mask_constants,
        pre_ui_source,
        backbuffer,
        ui_mask_);

    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, ui_mask_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

    backbuffer->Release();
    swapchain3->Release();
    device->Release();
    return;
  }

  bool ui_mask_generated = false;

  if (guides.valid && pre_ui_source != nullptr && ui_mask_ != nullptr) {
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

    debug_codec_->DispatchMask(
        command_list_,
        mask_constants,
        pre_ui_source,
        backbuffer,
        ui_mask_);

    ui_mask_generated = true;
  }

  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = output_;
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
      Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    DlssNrConstants downsample_constants = {};
    downsample_constants.Mode = DlssNrMode_Downsample;
    downsample_constants.Width = work_width;
    downsample_constants.Height = work_height;

    debug_codec_->DispatchDownsample(
        command_list_,
        downsample_constants,
        model_input,
        working_input_);

    Barrier(command_list_, working_input_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    model_color = working_input_;
    model_output = working_output_;
    eval_width = work_width;
    eval_height = work_height;
  }

  auto* params = parameter_block_.Get();
  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return;
  }

  params->Reset();

  parameter_block_.SetResource("DLSSNR.Color", model_color);
  parameter_block_.SetResource("DLSSNR.Output", model_output);
  parameter_block_.SetUInt("DLSSNR.Enabled", 1u);
  parameter_block_.SetUInt("DLSSNR.Width", eval_width);
  parameter_block_.SetUInt("DLSSNR.Height", eval_height);
  parameter_block_.SetUInt("DLSSNR.InputWidth", width);
  parameter_block_.SetUInt("DLSSNR.InputHeight", height);
  parameter_block_.SetUInt("DLSSNR.OutputWidth", eval_width);
  parameter_block_.SetUInt("DLSSNR.OutputHeight", eval_height);
  parameter_block_.SetUInt("DLSSNR.Upscaling", 1u);
  parameter_block_.SetFloat("DLSSNR.ScalingRatio", 1.0f);
  parameter_block_.SetFloat("DLSSNR.Scale", 1.0f);
  parameter_block_.SetUInt(
      "DLSSNR.Hint.Render.Preset",
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2)) + 1u);
  parameter_block_.SetFloat("DLSSNR.Intensity", pass.intensity);
  parameter_block_.SetUInt(
      "DLSSNR.Style",
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2)));
  parameter_block_.SetFloat(
      "DLSSNR.LocalStructureStrength", pass.local_structure);
  parameter_block_.SetFloat("DLSSNR.LocalToneStrength", pass.local_tone);
  parameter_block_.SetFloat(
      "DLSSNR.SkinStructureStrength",
      pass.auto_mask ? pass.skin_structure : -1.0f);
  parameter_block_.SetUInt(
      "DLSSNR.UseAutoMask", pass.auto_mask ? 1u : 0u);
  parameter_block_.SetUInt(
      "DLSSNR.UICorrection", pass.ui_correction ? 1u : 0u);

  if (ui_mask_generated && !use_selected_input && ui_mask_ != nullptr) {
    parameter_block_.SetResource("DLSSNR.UIAlpha", ui_mask_);
    parameter_block_.SetResource("DLSSNR.Backbuffer", backbuffer);
  }

  if (guides.valid) {
    parameter_block_.SetResource("DLSSNR.Depth", guides.depth);
    parameter_block_.SetResource("DLSSNR.MVec", guides.motion);
    parameter_block_.SetUInt(
        "DLSSNR.DepthInverted", guides.depth_inverted ? 1u : 0u);
    parameter_block_.SetFloat("DLSSNR.MVecScaleX", guides.mv_scale_x);
    parameter_block_.SetFloat("DLSSNR.MVecScaleY", guides.mv_scale_y);

    parameter_block_.SetUInt("DLSSNR.ColorSubrectBaseX", 0u);
    parameter_block_.SetUInt("DLSSNR.ColorSubrectBaseY", 0u);
    parameter_block_.SetUInt("DLSSNR.ColorSubrectWidth", eval_width);
    parameter_block_.SetUInt("DLSSNR.ColorSubrectHeight", eval_height);
    parameter_block_.SetUInt("DLSSNR.OutputSubrectBaseX", 0u);
    parameter_block_.SetUInt("DLSSNR.OutputSubrectBaseY", 0u);
    parameter_block_.SetUInt("DLSSNR.OutputSubrectWidth", eval_width);
    parameter_block_.SetUInt("DLSSNR.OutputSubrectHeight", eval_height);
    parameter_block_.SetUInt("DLSSNR.DepthSubrectBaseX", 0u);
    parameter_block_.SetUInt("DLSSNR.DepthSubrectBaseY", 0u);
    parameter_block_.SetUInt("DLSSNR.DepthSubrectWidth", guides.guide_width);
    parameter_block_.SetUInt("DLSSNR.DepthSubrectHeight", guides.guide_height);
    parameter_block_.SetUInt("DLSSNR.MVecSubrectBaseX", 0u);
    parameter_block_.SetUInt("DLSSNR.MVecSubrectBaseY", 0u);
    parameter_block_.SetUInt("DLSSNR.MVecSubrectWidth", guides.guide_width);
    parameter_block_.SetUInt("DLSSNR.MVecSubrectHeight", guides.guide_height);
  }

  parameter_block_.SetUInt("DLSSNR.Reset", first_evaluate_ ? 1u : 0u);
  first_evaluate_ = false;

  if (ui_mask_ != nullptr) {
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  if (!use_working_scale) {
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  const NVSDK_NGX_Result result = evaluate_feature_(command_list_, feature_handle_, params, nullptr);

  if (ui_mask_ != nullptr) {
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  if (use_working_scale &&
      (settings.debug_view == 7 || settings.debug_view == 8 ||
       settings.debug_view == 9)) {
    const bool debug_residual = settings.debug_view == 9;

    if (!debug_residual) {
      ID3D12Resource* debug_source =
          settings.debug_view == 7 ? working_input_ : working_output_;

      if (debug_source != nullptr) {
        Barrier(command_list_, debug_source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        command_list_->CopyResource(backbuffer, debug_source);
        Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PRESENT);
        Barrier(command_list_, debug_source, D3D12_RESOURCE_STATE_COPY_SOURCE,
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

      Barrier(command_list_, working_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Barrier(command_list_, working_input_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      debug_codec_->DispatchUpsampleResidual(
          command_list_,
          residual_constants,
          working_input_,
          working_input_,
          working_output_,
          composite_output_);

      Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
      Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_COPY_DEST);
      command_list_->CopyResource(backbuffer, composite_output_);
      Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_PRESENT);
      Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Barrier(command_list_, working_output_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    command_list_->Close();
    ID3D12CommandList* const lists[] = {command_list_};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

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

    Barrier(command_list_, working_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(command_list_, working_input_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchUpsampleResidual(
        command_list_,
        upsample_constants,
        backbuffer,
        working_input_,
        working_output_,
        composite_output_);

    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, composite_output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(command_list_, working_output_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else if (use_selected_input && ui_mask_generated &&
      ui_mask_ != nullptr && composite_output_ != nullptr) {
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

    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchComposite(
        command_list_,
        composite_constants,
        output_,
        backbuffer,
        ui_mask_,
        composite_output_);

    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, composite_output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(command_list_, ui_mask_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else if ((pass.luma_strength != 1.0f || pass.chroma_strength != 1.0f) &&
             composite_output_ != nullptr) {
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

    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    debug_codec_->DispatchLumaColor(
        command_list_,
        luma_color_constants,
        output_,
        backbuffer,
        composite_output_);

    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, composite_output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, composite_output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    command_list_->CopyResource(backbuffer, output_);
    Barrier(command_list_, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(command_list_, output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  command_list_->Close();
  ID3D12CommandList* const lists[] = {command_list_};
  command_queue_->ExecuteCommandLists(1, lists);
  WaitGpu();

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

  if (feature_handle_ != nullptr && release_feature_ != nullptr) {
    release_feature_(feature_handle_);
    feature_handle_ = nullptr;
  }

  if (output_ != nullptr) {
    output_->Release();
    output_ = nullptr;
  }

  if (ui_mask_ != nullptr) {
    ui_mask_->Release();
    ui_mask_ = nullptr;
  }

  if (composite_output_ != nullptr) {
    composite_output_->Release();
    composite_output_ = nullptr;
  }

  if (working_input_ != nullptr) {
    working_input_->Release();
    working_input_ = nullptr;
  }

  if (working_output_ != nullptr) {
    working_output_->Release();
    working_output_ = nullptr;
  }

  ui_mask_width_ = 0;
  ui_mask_height_ = 0;
  ui_mask_format_ = DXGI_FORMAT_UNKNOWN;
  composite_output_width_ = 0;
  composite_output_height_ = 0;
  composite_output_format_ = DXGI_FORMAT_UNKNOWN;
  working_resource_width_ = 0;
  working_resource_height_ = 0;
  working_resource_format_ = DXGI_FORMAT_UNKNOWN;

  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
    fence_event_ = INVALID_HANDLE_VALUE;
  }

  if (fence_ != nullptr) {
    fence_->Release();
    fence_ = nullptr;
  }

  if (command_list_ != nullptr) {
    command_list_->Release();
    command_list_ = nullptr;
  }

  if (command_allocator_ != nullptr) {
    command_allocator_->Release();
    command_allocator_ = nullptr;
  }

  command_queue_ = nullptr;
  fence_value_ = 0;
  built_width_ = 0;
  built_height_ = 0;
  built_working_scale_ = -1.0f;
  built_preset_ = 0xFFFFFFFFu;
  built_intensity_ = -1.0f;
  built_local_structure_ = -1.0f;
  built_local_tone_ = -1.0f;
  built_skin_structure_ = -1.0f;
  built_auto_mask_ = false;
  built_ui_correction_ = false;
  output_width_ = 0;
  output_height_ = 0;
  first_evaluate_ = true;

  parameter_block_.Shutdown();

  if (pending_selected_pass_ != nullptr) {
    DlssGuides::Get().RetireGameResource(pending_selected_pass_);
    pending_selected_pass_ = nullptr;
  }

  failed_ = false;
  reason_ = "";
  pass_index_ = 0;
}

}  // namespace reshade_dlssnr
