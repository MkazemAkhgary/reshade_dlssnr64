#include "pipeline/pass_session.h"
#include "profiling/profile.h"

#include "hdr/hdr_state.h"
#include "pipeline/pipeline_graph.h"

namespace reshade_dlssnr {

// Builds the per-pixel local-exposure multiplier for a display-referred source,
// and the image lifted by it.
//
// The HDR path evaluates the same operator inside the gain-map split, which
// already carries the tone curve and the exposure that define its display
// luminance. Pivot, radius, taps, protections and retention are shared, so both
// paths produce the same multiplier for the same displayed luminance.
//
// The map is built at the model input's resolution, which is the grid the lift
// and the restore both work in. The lift goes into the model input only: the
// compose reference stays on the image without it, so the residual carries the
// lift onto the output and Restore Model Exposure can take it back out.
//
// The step runs before the input grade, on the ungraded image, which is the
// order the HDR path uses: its split lifts the carrier and the grade runs
// afterwards.
PipelineStatus PassSession::StepLocalExposureScale(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepLocalExposureScale");
  PassSession* self = run.session;
  if (self == nullptr) return PipelineStatus::Ok;
  self->local_exposure_.scale_valid = false;

  model_pipeline::SetLocalExposureUniforms(run.hdr_uniforms,
                                           run.local_exposure);
  if (run.hdr_uniforms.local_exposure_strength <= 0.0f) {
    return PipelineStatus::Ok;
  }

  // The split has already applied the operator to the HDR carrier.
  const bool hdr_path = run.hdr_uniforms_ready && run.hdr != nullptr &&
                        run.hdr->ResolvedFormat() != HdrFormat::Sdr;
  if (hdr_path) return PipelineStatus::Ok;
  if (run.codec == nullptr || run.device == nullptr) return PipelineStatus::Ok;

  ID3D12Resource* source = run.model_color;
  if (source == nullptr) return PipelineStatus::Ok;
  const D3D12_RESOURCE_DESC desc = source->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = desc.Height;

  if (!self->resources_.EnsureLocalExposureScale(run.device, width, height)) {
    return self->FailStep("failed to create the local exposure scale map");
  }
  if (!self->resources_.EnsureLiftedWorkingInput(run.device, desc, width,
                                                 height)) {
    return self->FailStep("failed to create the lifted model input");
  }
  ID3D12Resource* scale = self->resources_.GetLocalExposureScale();
  ID3D12Resource* lifted = self->resources_.GetLiftedWorkingInput();

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.shader.Params0.x = 1.0f;
  model_pipeline::SetLocalExposureConstants(constants, run.hdr_uniforms);
  // The radius setting is in source pixels and the split evaluates it at full
  // resolution. This pass reads the model input, so the radius has to be
  // converted or the neighbourhood widens with the working scale and the
  // correction spreads across detail it should follow.
  if (run.width > 0) {
    constants.hdr.LocalExposureParams.x =
        run.hdr_uniforms.local_radius_px *
        static_cast<float>(width) / static_cast<float>(run.width);
  }

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  ID3D12Resource* srvs[kSrvCount] = {source, source, source, source};
  ID3D12Resource* uavs[kUavCount] = {scale, lifted};
  self->command_.Transition(list, source,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, scale,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, lifted,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!run.codec->Dispatch("LocalExposureScale", list, constants, srvs, uavs)) {
    return self->FailStep("failed to build the local exposure scale map");
  }
  self->command_.UavBarrier(list, scale);
  self->command_.UavBarrier(list, lifted);
  self->command_.Transition(list, scale,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, lifted,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  run.model_color = lifted;

  self->local_exposure_.scale_valid = true;
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr
