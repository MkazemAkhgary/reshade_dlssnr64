// Per-pass output, UI mask, composite, working-scale, and scratch resources.
#pragma once

#include <d3d12.h>

#include <cstdint>

#include "gpu/gpu_resources.h"

namespace reshade_dlssnr {

// Generic scratch texture pool for intermediate stages (pre-exposure,
// blend helpers, etc.). Slots are created lazily at whatever size/format
// is requested and reused across frames until the requested size changes.
//
// Slot 0 holds the Post-DLSS multi-pass input snapshot; slots 1 and 2 hold
// grade scratch; slots 3 and 4 hold the pre-grade model input/output debug
// snapshots.
//
// The frame debug output is deliberately not here. It lives in the
// controller's FrameDebugContext, because the Release stage copies it to the
// backbuffer after every pass has finished, so it has to outlive the pass
// session that rendered it.
constexpr uint32_t kScratchCount = 5;
constexpr uint32_t kInputSnapshotSlot = 0;
constexpr uint32_t kGradeScratchSlotA = 1;
constexpr uint32_t kGradeScratchSlotB = 2;
constexpr uint32_t kDebugPreGradeInputSlot = 3;
constexpr uint32_t kDebugPreGradeOutputSlot = 4;

// Every slot is an OwnedTexture or OwnedBuffer, so replacement always goes
// through the shared retire queue. A resource replaced this frame may still be
// read by in-flight GPU work on the game's queue; releasing it inline causes a
// use-after-free that manifests as DXGI_ERROR_DEVICE_HUNG.
class PassResources {
 public:
  bool EnsureOutput(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
  void ReleaseOutput();
  bool EnsureCompositeOutput(ID3D12Device* device,
                             const D3D12_RESOURCE_DESC& desc);
  bool EnsurePackedOutput(ID3D12Device* device,
                          const D3D12_RESOURCE_DESC& desc,
                          DXGI_FORMAT format);
  bool EnsureGainStats(ID3D12Device* device);
  bool EnsureWorkingResources(ID3D12Device* device,
                              const D3D12_RESOURCE_DESC& desc,
                              unsigned int work_width,
                              unsigned int work_height,
                              bool need_input,
                              bool need_graded_input);
  bool EnsureUnliftedWorkingInput(ID3D12Device* device,
                                  const D3D12_RESOURCE_DESC& desc,
                                  unsigned int work_width,
                                  unsigned int work_height);
  bool EnsureEffectiveExposureMap(ID3D12Device* device,
                                  unsigned int width,
                                  unsigned int height);
  void ReleaseEffectiveExposureMap();
  bool EnsureLocalExposureRestoreResources(ID3D12Device* device,
                                           unsigned int width,
                                           unsigned int height);
  // `input_desc` is the actual model-domain input (which may be the game
  // resource in identity-scale SDR mode). `out_reset_history` is true when
  // any temporal resource had to be recreated.
  bool EnsureSyntheticMotionResources(ID3D12Device* device,
                                      const D3D12_RESOURCE_DESC& input_desc,
                                      unsigned int width,
                                      unsigned int height,
                                      bool* out_reset_history);
  bool EnsureTemporalResources(ID3D12Device* device,
                               const D3D12_RESOURCE_DESC& input_desc,
                               unsigned int width,
                               unsigned int height,
                               bool use_fp16_history,
                               bool* out_reset_history);
  bool EnsureCheapTemporalResources(ID3D12Device* device,
                                    const D3D12_RESOURCE_DESC& input_desc,
                                    unsigned int width,
                                    unsigned int height);

  // Ensures scratch slot `index` matches the given size and format.
  bool EnsureScratch(ID3D12Device* device,
                     const D3D12_RESOURCE_DESC& desc,
                     unsigned int width,
                     unsigned int height,
                     uint32_t index);

  // Retires every slot. Call only after the GPU has been synchronized (or the
  // device is gone); the queue is drained by the controller afterwards.
  void Shutdown();

  ID3D12Resource* GetOutput() const { return output_.Get(); }
  // The otherwise-unused full-resolution output allocation holds the HDR
  // carrier with Local Exposure disabled, used to prepare a matching residual
  // reference at working resolution.
  ID3D12Resource* GetGlobalHdrCarrier() const { return output_.Get(); }
  ID3D12Resource* GetCompositeOutput() const { return composite_output_.Get(); }
  ID3D12Resource* GetPackedOutput() const { return packed_hdr_sdr_.Get(); }
  ID3D12Resource* GetGainStats() const { return gain_stats_.Get(); }
  ID3D12Resource* GetWorkingInput() const { return working_input_.Get(); }
  ID3D12Resource* GetWorkingOutput() const { return working_output_.Get(); }
  ID3D12Resource* GetGradedWorkingInput() const {
    return graded_working_input_.Get();
  }
  ID3D12Resource* GetUnliftedWorkingInput() const {
    return unlifted_working_input_.Get();
  }
  ID3D12Resource* GetEffectiveExposureMap() const {
    return effective_exposure_map_.Get();
  }
  ID3D12Resource* GetLocalExposurePartialSums() const {
    return local_exposure_partial_sums_.Get();
  }
  ID3D12Resource* GetLocalExposureGlobalScale() const {
    return local_exposure_global_scale_.Get();
  }
  unsigned int LocalExposurePartialWidth() const {
    return local_exposure_partial_sums_.Width();
  }
  unsigned int LocalExposurePartialHeight() const {
    return local_exposure_partial_sums_.Height();
  }
  ID3D12Resource* GetSyntheticPreviousInput() const {
    return synthetic_previous_input_.Get();
  }
  ID3D12Resource* GetSyntheticPreviousDepth() const {
    return synthetic_previous_depth_.Get();
  }
  ID3D12Resource* GetSyntheticCurrentDepth() const {
    return synthetic_current_depth_.Get();
  }
  ID3D12Resource* GetSyntheticMotion() const { return synthetic_motion_.Get(); }
  ID3D12Resource* GetTemporalPreviousInput() const {
    return temporal_previous_input_.Get();
  }
  ID3D12Resource* GetTemporalPreviousEdit() const {
    return temporal_previous_edit_.Get();
  }
  ID3D12Resource* GetTemporalPreviousDepth() const {
    return temporal_previous_depth_.Get();
  }
  ID3D12Resource* GetTemporalCandidate() const {
    return temporal_candidate_.Get();
  }
  ID3D12Resource* GetTemporalValidatedCandidate() const {
    return temporal_validated_candidate_.Get();
  }
  ID3D12Resource* GetTemporalStats() const { return temporal_stats_.Get(); }
  ID3D12Resource* GetCheapTemporalInput() const {
    return temporal_cheap_input_.Get();
  }
  ID3D12Resource* GetCheapTemporalOutput() const {
    return temporal_cheap_output_.Get();
  }
  unsigned int CheapTemporalWidth() const { return temporal_cheap_input_.Width(); }
  unsigned int CheapTemporalHeight() const {
    return temporal_cheap_input_.Height();
  }
  // True when slot `index` already holds a texture of the given shape on
  // `device`. Lets a caller pick between two slots instead of letting
  // EnsureScratch dispose a matching one in the other.
  bool ScratchMatches(ID3D12Device* device,
                      unsigned int width,
                      unsigned int height,
                      DXGI_FORMAT format,
                      uint32_t index) const {
    return index < kScratchCount &&
           scratch_[index].Matches(device, width, height, format);
  }

  ID3D12Resource* GetScratch(uint32_t index) const {
    return index < kScratchCount ? scratch_[index].Get() : nullptr;
  }

 private:
  // The full-resolution auxiliary target. In HDR frames with Local Exposure,
  // it holds the global-only carrier used to build the unlifted working-resolution
  // reference for HDR composition.
  OwnedTexture output_;
  OwnedTexture composite_output_;

  // Full-resolution packed HDR gain-map representation. RGB is the linear SDR
  // base; A is the linear carrier-to-HDR reconstruction multiplier. This is RGBA32F: no quantization
  // is acceptable in the round-trip path.
  OwnedTexture packed_hdr_sdr_;

  // [0] = sortable min gain, [1] = sortable max gain, [2] = out-of-range or
  // SDR gamut-fitted pixel count.
  OwnedBuffer gain_stats_;

  OwnedTexture working_input_;
  OwnedTexture working_output_;
  // Model-input domain after GradeModelInput. NGX consumes this buffer;
  // working_input stays ungraded and is the compose reference.
  OwnedTexture graded_working_input_;
  OwnedTexture unlifted_working_input_;
  // Full-resolution combined exposure used by HDR inverse compose. R32_FLOAT
  // stores global exposure x the exact local exposure factor applied by the
  // gain-map split to that source pixel.
  OwnedTexture effective_exposure_map_;
  OwnedTexture local_exposure_partial_sums_;
  // One R32_FLOAT scalar holding the geometric-mean local multiplier for
  // global restoration. The per-pixel multiplier is read directly from the
  // local/global carrier alpha channels in the grade shader.
  OwnedTexture local_exposure_global_scale_;

  // Temporal edit history. The history stores the current model input and a
  // bounded multiplicative log-gain field, never the previous output image.
  OwnedTexture temporal_previous_input_;
  OwnedTexture temporal_previous_edit_;
  OwnedTexture temporal_previous_depth_;
  OwnedTexture temporal_candidate_;
  OwnedTexture temporal_validated_candidate_;
  OwnedBuffer temporal_stats_;

  // Cheap model input/output at cheap_scale x working resolution, used on
  // reuse frames to fill low-confidence reprojection gaps.
  OwnedTexture temporal_cheap_input_;
  OwnedTexture temporal_cheap_output_;

  // Optional synthesized motion guides for stages with depth but no native MVec.
  // Motion is estimated at working resolution; current depth is reduced into
  // the same grid and committed as previous depth after model evaluation.
  OwnedTexture synthetic_previous_input_;
  OwnedTexture synthetic_previous_depth_;
  OwnedTexture synthetic_current_depth_;
  OwnedTexture synthetic_motion_;
  // Shape of the last synthetic/temporal set, so a size or format change can
  // force history invalidation.
  unsigned int synthetic_width_ = 0;
  unsigned int synthetic_height_ = 0;
  DXGI_FORMAT synthetic_input_format_ = DXGI_FORMAT_UNKNOWN;
  unsigned int temporal_width_ = 0;
  unsigned int temporal_height_ = 0;
  DXGI_FORMAT temporal_input_format_ = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT temporal_edit_format_ = DXGI_FORMAT_UNKNOWN;
  unsigned int working_width_ = 0;
  unsigned int working_height_ = 0;
  DXGI_FORMAT working_format_ = DXGI_FORMAT_UNKNOWN;
  unsigned int unlifted_working_width_ = 0;
  unsigned int unlifted_working_height_ = 0;
  DXGI_FORMAT unlifted_working_format_ = DXGI_FORMAT_UNKNOWN;

  OwnedTexture scratch_[kScratchCount];
};

}  // namespace reshade_dlssnr
