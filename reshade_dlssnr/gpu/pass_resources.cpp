#include "pass_resources.h"
#include "profiling/profile.h"

#include "temporal/stats.h"

namespace reshade_dlssnr {

namespace {

// The two history copies are only ever copied to and sampled; deliberately
// omit UAV flags so sRGB game formats remain legal.
D3D12_RESOURCE_DESC TextureCopyDesc(const D3D12_RESOURCE_DESC& source) {
  D3D12_RESOURCE_DESC desc = Texture2DDesc(
      static_cast<uint32_t>(source.Width),
      static_cast<uint32_t>(source.Height), source.Format);
  return desc;
}

}  // namespace

bool PassResources::EnsureOutput(ID3D12Device* device,
                                 const D3D12_RESOURCE_DESC& desc) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureOutput");
  return output_.EnsureUav(device, desc, "output");
}

void PassResources::ReleaseOutput() { output_.Reset(); }

bool PassResources::EnsureCompositeOutput(ID3D12Device* device,
                                          const D3D12_RESOURCE_DESC& desc) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureCompositeOutput");
  return composite_output_.EnsureUav(device, desc, "composite_output");
}

bool PassResources::EnsurePackedOutput(ID3D12Device* device,
                                       const D3D12_RESOURCE_DESC& desc,
                                       DXGI_FORMAT format) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsurePackedOutput");
  D3D12_RESOURCE_DESC packed_desc = desc;
  packed_desc.Format = format;
  packed_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  return packed_hdr_sdr_.Ensure(
      device, packed_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      "packed_hdr_sdr");
}

bool PassResources::EnsureGainStats(ID3D12Device* device) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureGainStats");
  return gain_stats_.EnsureUav(device, 4 * sizeof(uint32_t), "gain_stats");
}

bool PassResources::EnsureEffectiveExposureMap(ID3D12Device* device,
                                               unsigned int width,
                                               unsigned int height) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureEffectiveExposureMap");
  if (device == nullptr || width == 0 || height == 0) return false;
  return effective_exposure_map_.EnsureUav(
      device, Texture2DDesc(width, height, DXGI_FORMAT_R32_FLOAT),
      "effective_exposure_map");
}

void PassResources::ReleaseEffectiveExposureMap() {
  effective_exposure_map_.Reset();
}

bool PassResources::EnsureLocalExposureRestoreResources(ID3D12Device* device,
                                                        unsigned int width,
                                                        unsigned int height) {
  DLSSNR_PROFILE_RESOURCE_L2(
      "PassResources::EnsureLocalExposureRestoreResources");
  if (device == nullptr || width == 0 || height == 0) return false;

  const unsigned int partial_width = (width + 7u) / 8u;
  const unsigned int partial_height = (height + 7u) / 8u;
  const bool matches =
      local_exposure_partial_sums_.Matches(partial_width, partial_height,
                                           DXGI_FORMAT_R32_FLOAT) &&
      local_exposure_global_scale_.Matches(1, 1, DXGI_FORMAT_R32_FLOAT);
  if (matches) return true;

  local_exposure_partial_sums_.Reset();
  local_exposure_global_scale_.Reset();

  if (!local_exposure_partial_sums_.EnsureUav(
          device, Texture2DDesc(partial_width, partial_height,
                                DXGI_FORMAT_R32_FLOAT),
          "local_exposure_tile_log_scales")) {
    return false;
  }
  if (!local_exposure_global_scale_.EnsureUav(
          device, Texture2DDesc(1, 1, DXGI_FORMAT_R32_FLOAT),
          "local_exposure_global_scale")) {
    local_exposure_partial_sums_.Reset();
    return false;
  }
  return true;
}

bool PassResources::EnsureWorkingResources(ID3D12Device* device,
                                           const D3D12_RESOURCE_DESC& desc,
                                           unsigned int work_width,
                                           unsigned int work_height,
                                           bool need_input,
                                           bool need_graded_input) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureWorkingResources");
  const bool output_matches =
      working_output_.Matches(work_width, work_height, desc.Format);
  const bool input_matches = !need_input || working_input_ != nullptr;
  const bool graded_matches =
      !need_graded_input || graded_working_input_ != nullptr;
  if (output_matches && input_matches && graded_matches) {
    if (!need_input) working_input_.Reset();
    if (!need_graded_input) graded_working_input_.Reset();
    return true;
  }

  working_input_.Reset();
  working_output_.Reset();
  graded_working_input_.Reset();

  D3D12_RESOURCE_DESC working_desc = desc;
  working_desc.Width = work_width;
  working_desc.Height = work_height;

  if (need_input &&
      !working_input_.EnsureUav(device, working_desc, "working_input")) {
    return false;
  }
  if (need_graded_input && need_input &&
      !graded_working_input_.EnsureUav(device, working_desc,
                                              "graded_working_input")) {
    working_input_.Reset();
    return false;
  }
  if (!working_output_.EnsureUav(device, working_desc,
                                        "working_output")) {
    working_input_.Reset();
    graded_working_input_.Reset();
    return false;
  }

  working_width_ = work_width;
  working_height_ = work_height;
  working_format_ = desc.Format;
  return true;
}

bool PassResources::EnsureUnliftedWorkingInput(ID3D12Device* device,
                                               const D3D12_RESOURCE_DESC& desc,
                                               unsigned int work_width,
                                               unsigned int work_height) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureUnliftedWorkingInput");
  if (device == nullptr || work_width == 0 || work_height == 0) return false;

  if (unlifted_working_input_.Matches(work_width, work_height, desc.Format)) {
    return true;
  }

  D3D12_RESOURCE_DESC working_desc = desc;
  working_desc.Width = work_width;
  working_desc.Height = work_height;
  if (!unlifted_working_input_.EnsureUav(device, working_desc,
                                                "unlifted_working_input")) {
    return false;
  }
  unlifted_working_width_ = work_width;
  unlifted_working_height_ = work_height;
  unlifted_working_format_ = desc.Format;
  return true;
}

bool PassResources::EnsureSyntheticMotionResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& input_desc,
    unsigned int width,
    unsigned int height,
    bool* out_reset_history) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureSyntheticMotionResources");
  if (out_reset_history != nullptr) *out_reset_history = false;
  if (device == nullptr || width == 0 || height == 0) return false;

  const bool matches = synthetic_previous_input_.Matches(
                           input_desc.Width, input_desc.Height,
                           input_desc.Format) &&
                       synthetic_previous_depth_.Matches(width, height,
                                                         DXGI_FORMAT_R32_FLOAT) &&
                       synthetic_current_depth_.Matches(width, height,
                                                        DXGI_FORMAT_R32_FLOAT) &&
                       synthetic_motion_.Matches(width, height,
                                                 DXGI_FORMAT_R32G32_FLOAT) &&
                       synthetic_width_ == width && synthetic_height_ == height &&
                       synthetic_input_format_ == input_desc.Format;
  if (matches) return true;

  synthetic_previous_input_.Reset();
  synthetic_previous_depth_.Reset();
  synthetic_current_depth_.Reset();
  synthetic_motion_.Reset();

  const D3D12_RESOURCE_DESC input_copy_desc = TextureCopyDesc(input_desc);
  if (!synthetic_previous_input_.Ensure(
          device, input_copy_desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          "synthetic_previous_input") ||
      !synthetic_previous_depth_.EnsureUav(
          device, Texture2DDesc(width, height, DXGI_FORMAT_R32_FLOAT),
          "synthetic_previous_depth") ||
      !synthetic_current_depth_.EnsureUav(
          device, Texture2DDesc(width, height, DXGI_FORMAT_R32_FLOAT),
          "synthetic_current_depth") ||
      !synthetic_motion_.EnsureUav(
          device, Texture2DDesc(width, height, DXGI_FORMAT_R32G32_FLOAT),
          "synthetic_motion")) {
    synthetic_previous_input_.Reset();
    synthetic_previous_depth_.Reset();
    synthetic_current_depth_.Reset();
    synthetic_motion_.Reset();
    return false;
  }

  synthetic_width_ = width;
  synthetic_height_ = height;
  synthetic_input_format_ = input_desc.Format;
  if (out_reset_history != nullptr) *out_reset_history = true;
  return true;
}

bool PassResources::EnsureTemporalResources(ID3D12Device* device,
                                            const D3D12_RESOURCE_DESC& input_desc,
                                            unsigned int width,
                                            unsigned int height,
                                            bool use_fp16_history,
                                            bool* out_reset_history) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureTemporalResources");
  if (out_reset_history != nullptr) *out_reset_history = false;
  if (device == nullptr || width == 0 || height == 0) return false;

  const DXGI_FORMAT requested_edit_format = use_fp16_history
      ? DXGI_FORMAT_R16G16B16A16_FLOAT
      : DXGI_FORMAT_R32G32B32A32_FLOAT;
  const bool matches =
      temporal_previous_input_.Matches(input_desc.Width, input_desc.Height,
                                       input_desc.Format) &&
      temporal_previous_edit_.Matches(width, height, requested_edit_format) &&
      temporal_previous_depth_.Matches(width, height, DXGI_FORMAT_R32_FLOAT) &&
      temporal_candidate_.Matches(width, height, requested_edit_format) &&
      temporal_validated_candidate_.Matches(width, height,
                                            requested_edit_format) &&
      temporal_stats_.Matches(temporal_stats::kBytes) &&
      temporal_width_ == width && temporal_height_ == height &&
      temporal_input_format_ == input_desc.Format &&
      temporal_edit_format_ == requested_edit_format;
  if (matches) return true;

  temporal_previous_input_.Reset();
  temporal_previous_edit_.Reset();
  temporal_previous_depth_.Reset();
  temporal_candidate_.Reset();
  temporal_validated_candidate_.Reset();
  temporal_stats_.Reset();

  const auto reset_all = [this]() {
    temporal_previous_input_.Reset();
    temporal_previous_edit_.Reset();
    temporal_previous_depth_.Reset();
    temporal_candidate_.Reset();
    temporal_validated_candidate_.Reset();
    temporal_stats_.Reset();
  };

  const D3D12_RESOURCE_DESC input_copy_desc = TextureCopyDesc(input_desc);
  const D3D12_RESOURCE_DESC edit_desc =
      Texture2DDesc(width, height, requested_edit_format);
  const D3D12_RESOURCE_DESC depth_desc =
      Texture2DDesc(width, height, DXGI_FORMAT_R32_FLOAT);

  if (!temporal_previous_input_.Ensure(
          device, input_copy_desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          "temporal_previous_input") ||
      !temporal_previous_edit_.EnsureUav(device, edit_desc,
                                                "temporal_previous_edit") ||
      !temporal_candidate_.EnsureUav(device, edit_desc,
                                            "temporal_candidate") ||
      !temporal_validated_candidate_.EnsureUav(
          device, edit_desc, "temporal_validated_candidate") ||
      !temporal_previous_depth_.EnsureUav(device, depth_desc,
                                                 "temporal_previous_depth") ||
      !temporal_stats_.EnsureUav(device, temporal_stats::kBytes,
                                       "temporal_stats")) {
    reset_all();
    return false;
  }

  temporal_width_ = width;
  temporal_height_ = height;
  temporal_input_format_ = input_desc.Format;
  temporal_edit_format_ = requested_edit_format;
  if (out_reset_history != nullptr) *out_reset_history = true;
  return true;
}

bool PassResources::EnsureCheapTemporalResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& input_desc,
    unsigned int width,
    unsigned int height) {
  DLSSNR_PROFILE_RESOURCE_L2("PassResources::EnsureCheapTemporalResources");
  if (device == nullptr || width == 0 || height == 0) return false;

  if (temporal_cheap_input_.Matches(width, height, input_desc.Format) &&
      temporal_cheap_output_.Matches(width, height, input_desc.Format)) {
    return true;
  }

  temporal_cheap_input_.Reset();
  temporal_cheap_output_.Reset();

  D3D12_RESOURCE_DESC desc = input_desc;
  desc.Width = width;
  desc.Height = height;

  if (!temporal_cheap_input_.EnsureUav(device, desc,
                                              "temporal_cheap_input")) {
    return false;
  }
  if (!temporal_cheap_output_.EnsureUav(device, desc,
                                               "temporal_cheap_output")) {
    temporal_cheap_input_.Reset();
    return false;
  }
  return true;
}

bool PassResources::EnsureScratch(ID3D12Device* device,
                                  const D3D12_RESOURCE_DESC& desc,
                                  unsigned int width,
                                  unsigned int height,
                                  uint32_t index) {
  DLSSNR_PROFILE_RESOURCE_L3("PassResources::EnsureScratch");
  if (index >= kScratchCount) return false;
  if (scratch_[index].Matches(width, height, desc.Format)) return true;

  D3D12_RESOURCE_DESC scratch_desc = desc;
  scratch_desc.Width = width;
  scratch_desc.Height = height;

  static const char* const kNames[kScratchCount] = {
      "scratch[0]", "scratch[1]", "scratch[2]", "scratch[3]", "scratch[4]"};
  return scratch_[index].EnsureUav(device, scratch_desc,
                                          kNames[index]);
}

void PassResources::Shutdown() {
  DLSSNR_PROFILE_RESOURCE_L1("PassResources::Shutdown");
  output_.Reset();
  composite_output_.Reset();
  packed_hdr_sdr_.Reset();
  gain_stats_.Reset();
  working_input_.Reset();
  working_output_.Reset();
  graded_working_input_.Reset();
  unlifted_working_input_.Reset();
  effective_exposure_map_.Reset();
  local_exposure_partial_sums_.Reset();
  local_exposure_global_scale_.Reset();
  temporal_previous_input_.Reset();
  temporal_previous_edit_.Reset();
  temporal_previous_depth_.Reset();
  temporal_candidate_.Reset();
  temporal_validated_candidate_.Reset();
  temporal_stats_.Reset();
  temporal_cheap_input_.Reset();
  temporal_cheap_output_.Reset();
  synthetic_previous_input_.Reset();
  synthetic_previous_depth_.Reset();
  synthetic_current_depth_.Reset();
  synthetic_motion_.Reset();

  for (uint32_t i = 0; i < kScratchCount; ++i) scratch_[i].Reset();

  synthetic_width_ = 0;
  synthetic_height_ = 0;
  synthetic_input_format_ = DXGI_FORMAT_UNKNOWN;
  temporal_width_ = temporal_height_ = 0;
  temporal_input_format_ = DXGI_FORMAT_UNKNOWN;
  temporal_edit_format_ = DXGI_FORMAT_UNKNOWN;
  working_width_ = 0;
  working_height_ = 0;
  working_format_ = DXGI_FORMAT_UNKNOWN;
  unlifted_working_width_ = 0;
  unlifted_working_height_ = 0;
  unlifted_working_format_ = DXGI_FORMAT_UNKNOWN;
}

}  // namespace reshade_dlssnr
