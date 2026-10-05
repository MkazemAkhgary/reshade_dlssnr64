#include "skin/skin_detect.h"

#include <algorithm>

#include <reshade.hpp>

#include "dlss_guides.h"
#include "hdr/hdr_state.h"
#include "model/model_pipeline.h"
#include "model/ngx_model_params.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"
#include "model/runtime.h"

namespace reshade_dlssnr {

namespace {

// Tuning for the detection pass. Built entirely from the dedicated
// skin_detect_* settings; nothing here inherits from or shares with any
// PassSettings.
PassSettings BuildDetectionTuning(const Settings& s) {
  PassSettings pass;
  pass.preset = std::clamp(s.skin.preset, 0, 2);
  pass.intensity = s.skin.intensity;
  pass.local_tone = s.skin.local_tone;
  pass.local_structure = s.skin.local_structure;
  pass.skin_structure = s.skin.skin_structure;
  pass.auto_mask = true;
  pass.ui_correction = s.skin.ui_correction;
  return pass;
}

void RetireToGuides(ID3D12Resource* resource) {
  if (resource == nullptr) return;
  resource->AddRef();
  DlssGuides::Get().RetireGameResource(resource);
  resource->Release();
}

bool RetireAndCreate(ID3D12Device* device,
                     Microsoft::WRL::ComPtr<ID3D12Resource>& slot,
                     const D3D12_RESOURCE_DESC& desc) {
  if (slot != nullptr) {
    ID3D12Resource* old = slot.Detach();
    RetireToGuides(old);
  }
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = desc;
  d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  Microsoft::WRL::ComPtr<ID3D12Resource> created;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &d,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&created)))) {
    return false;
  }
  slot = std::move(created);
  return true;
}

}  // namespace

void SkinDetect::Shutdown(NgxRuntime& runtime) {
  feature_.Shutdown(runtime);
  working_input_.Reset();
  working_output_.Reset();
  mask_[0].Reset();
  mask_[1].Reset();
  mask_index_ = 0;
  mask_valid_ = false;
  width_ = 0;
  height_ = 0;
  format_ = DXGI_FORMAT_UNKNOWN;
  output_ = {};
}

bool SkinDetect::EnsureResources(ID3D12Device* device,
                                 unsigned int width,
                                 unsigned int height,
                                 DXGI_FORMAT format) {
  if (device == nullptr) return false;
  if (working_input_ != nullptr && width_ == width && height_ == height &&
      format_ == format) {
    return true;
  }

  D3D12_RESOURCE_DESC color_desc = {};
  color_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  color_desc.Width = width;
  color_desc.Height = height;
  color_desc.DepthOrArraySize = 1;
  color_desc.MipLevels = 1;
  color_desc.Format = format;
  color_desc.SampleDesc.Count = 1;
  color_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  if (!RetireAndCreate(device, working_input_, color_desc)) return false;
  if (!RetireAndCreate(device, working_output_, color_desc)) return false;

  D3D12_RESOURCE_DESC mask_desc = color_desc;
  mask_desc.Format = DXGI_FORMAT_R8_UNORM;
  if (!RetireAndCreate(device, mask_[0], mask_desc)) return false;
  if (!RetireAndCreate(device, mask_[1], mask_desc)) return false;

  // Fresh pair, no history. First frame after this writes the vote fresh.
  mask_index_ = 0;
  mask_valid_ = false;
  width_ = width;
  height_ = height;
  format_ = format;
  return true;
}

bool SkinDetect::Run(CommandContext& cmd,
                     NgxRuntime& runtime,
                     ComputeEngine& codec,
                     ID3D12GraphicsCommandList* command_list,
                     const DlssGuideState& guides,
                     HdrState* hdr,
                     const Settings& settings) {
  output_.valid = false;

  if (command_list == nullptr) return false;
  if (guides.dlss_output_color == nullptr) return false;
  if (guides.output_width == 0 || guides.output_height == 0) return false;

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
      device == nullptr) {
    return false;
  }

  const float scale = std::clamp(settings.skin.scale, 0.1f, 1.0f);
  const unsigned int work_w = std::max(
      1u, static_cast<unsigned int>(guides.output_width * scale + 0.5f));
  const unsigned int work_h = std::max(
      1u, static_cast<unsigned int>(guides.output_height * scale + 0.5f));
  const DXGI_FORMAT source_format = guides.dlss_output_color->GetDesc().Format;

  if (!EnsureResources(device.Get(), work_w, work_h, source_format)) {
    return false;
  }

  tuning_ = BuildDetectionTuning(settings);

  const uint32_t cur = mask_index_;
  const uint32_t prev = 1u - mask_index_;
  const bool warmup = !mask_valid_;

  cmd.SetResourceState(guides.dlss_output_color,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.SetResourceState(working_input_.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.SetResourceState(working_output_.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.SetResourceState(mask_[cur].Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // Previous mask is read this frame; seed it as SRV. On warmup it is not
  // read by the shader (alpha is forced to 1), but the binding is still
  // valid so the same dispatch shape works for both cases.
  cmd.SetResourceState(mask_[prev].Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  cmd.Transition(command_list, guides.dlss_output_color,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.Transition(command_list, working_input_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.Transition(command_list, working_output_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.Transition(command_list, mask_[cur].Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.Transition(command_list, mask_[prev].Get(),
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // 1. Tonemap + resize into working_input_.
  ComputeConstants constants = {};
  constants.Width = work_w;
  constants.Height = work_h;
  const char* pass_name = "PrepareModelInput";
  if (hdr != nullptr && hdr->Enabled()) {
    // Fallback EMA: no skin slice, so the mask this pass produces cannot
    // drive its own input exposure.
    pass_name = "TonemapForward";
    constants.params[0] = hdr->FallbackExposure();
    constants.params[1] = hdr->FallbackSceneWhite() *
                          std::max(settings.hdr.highlight_headroom, 0.01f);
    constants.params[2] = static_cast<float>(HdrFormatCode(hdr->ResolvedFormat()));
  }

  ID3D12Resource* prep_srvs[kSrvCount] = {guides.dlss_output_color,
                                          guides.dlss_output_color,
                                          guides.dlss_output_color};
  ID3D12Resource* prep_uavs[kUavCount] = {working_input_.Get()};
  if (!codec.Dispatch(pass_name, command_list, constants, prep_srvs,
                      prep_uavs)) {
    return false;
  }
  cmd.Transition(command_list, working_input_.Get(),
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // 2. Build at full frame resolution; working scale only shapes the eval.
  if (!feature_.EnsureFeature(runtime, tuning_, command_list,
                              guides.output_width, guides.output_height)) {
    return false;
  }
  if (feature_.ConsumeJustRebuilt()) {
    return false;
  }

  // 3. Parameter block.
  NgxParameterBlock& block = runtime.Parameters();
  NVSDK_NGX_Parameter* params = block.Get();
  if (params == nullptr) return false;

  block.SetResource("DLSSNR.Color", working_input_.Get());
  block.SetResource("DLSSNR.Output", working_output_.Get());
  block.SetUInt("DLSSNR.Width", work_w);
  block.SetUInt("DLSSNR.Height", work_h);
  block.SetUInt("DLSSNR.InputWidth", guides.output_width);
  block.SetUInt("DLSSNR.InputHeight", guides.output_height);
  block.SetUInt("DLSSNR.OutputWidth", work_w);
  block.SetUInt("DLSSNR.OutputHeight", work_h);
  ApplyModelTuningParams(block, tuning_);

  if (guides.valid) {
    block.SetResource("DLSSNR.Depth", guides.depth);
    block.SetResource("DLSSNR.MVec", guides.motion);
    block.SetUInt("DLSSNR.DepthInverted", guides.depth_inverted ? 1u : 0u);
    block.SetFloat("DLSSNR.MVecScaleX", guides.mv_scale_x);
    block.SetFloat("DLSSNR.MVecScaleY", guides.mv_scale_y);
    block.SetUInt("DLSSNR.ColorSubrectBaseX", 0u);
    block.SetUInt("DLSSNR.ColorSubrectBaseY", 0u);
    block.SetUInt("DLSSNR.ColorSubrectWidth", work_w);
    block.SetUInt("DLSSNR.ColorSubrectHeight", work_h);
    block.SetUInt("DLSSNR.OutputSubrectBaseX", 0u);
    block.SetUInt("DLSSNR.OutputSubrectBaseY", 0u);
    block.SetUInt("DLSSNR.OutputSubrectWidth", work_w);
    block.SetUInt("DLSSNR.OutputSubrectHeight", work_h);
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

  // 4. Evaluate.
  runtime.EvaluateFeature(command_list, feature_.FeatureHandle(), params,
                          nullptr);

  // 5. Barrier so the diff reads the write.
  cmd.UavBarrier(command_list, working_output_.Get());
  cmd.Transition(command_list, working_output_.Get(),
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // 6. Threshold the difference into the mask.
  ComputeConstants mask_constants = {};
  mask_constants.Width = work_w;
  mask_constants.Height = work_h;
  // Response 0..1 -> alpha 0.02..1.0. Warmup forces 1.0 (write fresh, no
  // blend) so the first frame after a resource (re)creation is not a fade.
  float alpha = 0.02f + settings.skin.mask_response * 0.98f;
  if (warmup) alpha = 1.0f;

  mask_constants.params[0] = settings.skin.luma_threshold;
  mask_constants.params[1] = settings.skin.luma_relative;
  mask_constants.params[2] = settings.skin.chroma_threshold;
  mask_constants.params[3] = settings.skin.clip_threshold;
  mask_constants.params[4] = settings.skin.edit_strength;
  mask_constants.params[5] = settings.skin.mask_smoothing;
  mask_constants.params[6] = alpha;

  ID3D12Resource* mask_srvs[kSrvCount] = {working_input_.Get(),
                                          working_output_.Get(),
                                          mask_[prev].Get()};
  ID3D12Resource* mask_uavs[kUavCount] = {mask_[cur].Get()};
  if (!codec.Dispatch("SkinMaskDiff", command_list, mask_constants, mask_srvs,
                      mask_uavs)) {
    return false;
  }

  mask_index_ = prev;
  mask_valid_ = true;
  output_.mask = mask_[cur].Get();
  output_.detect_input = working_input_.Get();
  output_.detect_output = working_output_.Get();
  output_.width = work_w;
  output_.height = work_h;
  output_.valid = true;
  return true;
}

// ---------------------------------------------------------------------------
// Pipeline step registration
// ---------------------------------------------------------------------------

namespace {

PipelineStatus RunSkinDetect(PassRun& run) {
  if (!run.is_first_pass) return PipelineStatus::Ok;
  if (run.skin_detect == nullptr || run.hdr == nullptr ||
      run.settings == nullptr) {
    return PipelineStatus::Ok;
  }
  if (!run.settings->skin.enable) return PipelineStatus::Ok;
  if (!run.hdr->Enabled()) return PipelineStatus::Ok;
  if (run.guides.dlss_output_color == nullptr) return PipelineStatus::Ok;

  if (!run.skin_detect->Run(*run.cmd, *run.runtime, *run.codec, run.list,
                            run.guides, run.hdr, *run.settings)) {
    TRACE_EVT(run.session, "skin_detect SKIP");
  }
  return PipelineStatus::Ok;
}

constexpr PipelineNodeId kSkinDetectRequires[] = {
    PipelineNodeId::EnsureComputeEngine, PipelineNodeId::Count};

constexpr PipelineStepDef kSkinDetectSteps[] = {
    {PipelineNodeId::SkinDetect, "SkinDetect", &RunSkinDetect,
     kSkinDetectRequires, /*required_inputs=*/StageInput::CommandList,
     /*requires_hdr_aware=*/true},
};

static_assert(
    PrerequisitesPrecedeSteps(kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes),
                              kSkinDetectSteps, ArrayCount(kSkinDetectSteps)),
    "SkinDetect declares a prerequisite the PostDLSS stage lists after it");

static_assert(
    StageInputsCoverSteps(kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes),
                          kSkinDetectSteps, ArrayCount(kSkinDetectSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "SkinDetect needs an input the PostDLSS call site does not supply");

}  // namespace

const PipelineStepDef* SkinDetectStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kSkinDetectSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr
