#include "hdr_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <reshade.hpp>

#include "d3d12_utils.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

constexpr uint32_t kHistogramBins = 256;
constexpr uint32_t kHistogramBytes = kHistogramBins * sizeof(uint32_t);
constexpr float kLumaMinLog2 = -12.0f;
constexpr float kLumaMaxLog2 = 12.0f;

HdrFormat DetectFormatFromResource(ID3D12Resource* resource,
                                   HdrFormat override) {
  if (override != HdrFormat::Auto) return override;
  if (resource == nullptr) return HdrFormat::Sdr;

  const DXGI_FORMAT fmt = resource->GetDesc().Format;
  switch (fmt) {
    // Linear float formats — unambiguously HDR.
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return HdrFormat::ScRgb;

    // 10-bit UNORM. Historically treated as ambiguous (HDR10 vs 10-bit
    // SDR), but true 10-bit SDR is essentially unused in games. When this
    // format appears as a DLSS output, it is HDR10 in practice. Users
    // with a genuine 10-bit SDR setup can override to SDR.
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return HdrFormat::Hdr10;

    // 8-bit formats are SDR.
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return HdrFormat::Sdr;

    default:
      return HdrFormat::Sdr;
  }
}

float FormatToShaderValue(HdrFormat format) {
  switch (format) {
    case HdrFormat::ScRgb: return 1.0f;
    case HdrFormat::Hdr10: return 2.0f;
    case HdrFormat::Sdr:
    case HdrFormat::Auto:
    default:               return 0.0f;
  }
}

// Returns the bin index below which `fraction` of samples lie.
// `bins` is a 256-entry histogram; `total` is the sum of all bins.
float PercentileBin(const uint32_t* bins, uint32_t total, float fraction) {
  if (total == 0) return 0.0f;
  const uint32_t target =
      static_cast<uint32_t>(fraction * static_cast<float>(total));
  uint32_t cum = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    cum += bins[i];
    if (cum >= target) return static_cast<float>(i);
  }
  return static_cast<float>(kHistogramBins - 1);
}

// Maps a bin index to a linear luminance value (the value at the low edge
// of the bin).
float BinToLinear(float bin) {
  const float t = bin / 255.0f;
  const float l2 = kLumaMinLog2 + t * (kLumaMaxLog2 - kLumaMinLog2);
  return std::pow(2.0f, l2);
}

float Lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

}  // namespace

bool HdrState::Ensure(ID3D12Device* device) {
  if (device == nullptr) return false;
  if (device_.Get() == device && histogram_ != nullptr && fence_ != nullptr) {
    return true;
  }

  Shutdown();

  // Histogram buffer: 256 R32_UINT, UAV + COPY_SOURCE.
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kHistogramBytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(histogram_.GetAddressOf())))) {
    Shutdown();
    return false;
  }

  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(fence_.GetAddressOf())))) {
    Shutdown();
    return false;
  }

  if (!readback_.Ensure(device, kHistogramBytes)) {
    Shutdown();
    return false;
  }

  device_ = device;
  return true;
}

void HdrState::Shutdown() {
  readback_.Shutdown();
  histogram_.Reset();
  fence_.Reset();
  fence_value_ = 0;
  pending_fence_value_ = 0;
  pending_measurement_ = false;
  device_.Reset();

  enabled_ = false;
  have_measurement_ = false;
  exposure_ema_ = 1.0f;
  white_point_ema_ = 1.0f;
  frame_counter_ = 0;
  frames_until_measure_ = 0;
  warmup_frames_ = 0;
  frames_since_measure_ = 0;
  have_logged_format_ = false;

  have_histogram_ = false;
  for (uint32_t i = 0; i < kHistogramBins; ++i) last_histogram_[i] = 0;
  p50_luma_ = 0.0f;
  p_hi_luma_ = 0.0f;
  target_exposure_ = 1.0f;
  last_samples_ = 0;
}

HdrSnapshot HdrState::GetSnapshot() const {
  HdrSnapshot s;
  s.enabled = enabled_;
  s.ready = have_measurement_;
  s.resolved_format = resolved_format_;
  s.exposure = exposure_ema_;
  s.white_point = white_point_ema_;
  s.frames_since_measure = frames_since_measure_;

  s.has_histogram = have_histogram_;
  if (have_histogram_) {
    for (uint32_t i = 0; i < kHistogramBins; ++i) {
      s.histogram[i] = last_histogram_[i];
    }
  }
  s.p50_luma = p50_luma_;
  s.p_hi_luma = p_hi_luma_;
  s.target_exposure = target_exposure_;
  s.samples = last_samples_;
  return s;
}

void HdrState::BeginFrame(CommandContext& cmd,
                          ID3D12GraphicsCommandList* command_list,
                          ID3D12Resource* source,
                          unsigned int width,
                          unsigned int height,
                          const Settings& settings,
                          ComputeEngine& codec) {
  enabled_ = settings.hdr_enable;
  if (!enabled_) {
    // Keep existing EMA values around so toggling back on doesn't reset.
    return;
  }

  if (command_list == nullptr || source == nullptr || width == 0 ||
      height == 0) {
    return;
  }

  frames_since_measure_++;

  // Poll any completed readback first, so this frame's exposure reflects
  // the latest available data.
  PollReadback(fence_.Get(), settings);

  // Resolve format for this frame.
  const HdrFormat prev_format = resolved_format_;
  resolved_format_ = DetectFormatFromResource(source, settings.hdr_format);

  // Log the raw DXGI format the first time we see it (or whenever it
  // changes). Makes format-detection issues easy to diagnose.
  if (resolved_format_ != prev_format || !have_logged_format_) {
    const DXGI_FORMAT raw_fmt = source->GetDesc().Format;
    char msg[160];
    std::snprintf(
        msg, sizeof(msg),
        "HDR: DLSS output format=%d detected=%d (0=SDR,1=scRGB,2=HDR10)",
        static_cast<int>(raw_fmt), static_cast<int>(resolved_format_));
    reshade::log::message(reshade::log::level::info, msg);
    have_logged_format_ = true;
  }

  // If a measurement is already in flight, don't enqueue another.
  if (pending_measurement_) {
    frame_counter_++;
    return;
  }

  // Determine how often to measure. 1 = every frame.
  const uint32_t capped_every =
      static_cast<uint32_t>(std::clamp(settings.hdr_measure_interval, 1, 32));

  if (frames_until_measure_ > 0) {
    frames_until_measure_--;
    frame_counter_++;
    return;
  }
  frames_until_measure_ = capped_every - 1;

  // The histogram shaders are compiled on the worker like everything
  // else. Skipping here is safe: pending_measurement_ stays false, so
  // the next frame retries, and the readback ring is untouched.
  if (!codec.IsReady()) {
    frame_counter_++;
    return;
  }

  // Dispatch histogram on the game's command list.
  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.params[0] = FormatToShaderValue(resolved_format_);
  constants.params[1] = kLumaMinLog2;
  constants.params[2] = kLumaMaxLog2;

  // Seed the state tracker with the actual current states. Without this,
  // inference would guess from the resource descriptor, which is wrong for
  // these two (the buffer would be read as GENERIC_READ, the source as
  // COMMON), and the first transition would issue an invalid barrier.
  //
  // Both resources are owned by us, and their states across frames are
  // deterministic:
  //   - `source` is the DLSS output clone, left in SRV by DlssGuides.
  //   - `histogram_` is left in UAV at the end of every BeginFrame.
  cmd.SetResourceState(source,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.SetResourceState(histogram_.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {source, source, source};
  ID3D12Resource* uavs[kUavCount] = {histogram_.Get()};

  cmd.Transition(command_list, source,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // Clear then build.
  codec.Dispatch("ZeroHistogram", command_list, constants, srvs, uavs,
                 /*threads_x=*/1, /*threads_y=*/1, /*threads_z=*/1);
  codec.Dispatch("BuildHistogram", command_list, constants, srvs, uavs);

  // Copy into the readback ring. The fence value is signaled later at
  // Present, once the game's queue has advanced past this command list.
  const uint64_t next_value = fence_value_ + 1;
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_COPY_SOURCE);

  if (readback_.EnqueueCopy(command_list, histogram_.Get(),
                            kHistogramBytes, fence_.Get(), next_value)) {
    pending_fence_value_ = next_value;
    pending_measurement_ = true;
  }

  // Leave the histogram in UAV so SeedFrameState / inference matches.
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  frame_counter_++;
}

void HdrState::SignalAtPresent(ID3D12CommandQueue* queue) {
  if (!pending_measurement_ || queue == nullptr || fence_ == nullptr) return;

  // Signal the fence with the value we promised to the readback ring. The
  // game has already submitted its command list (which contained our
  // histogram + copy), so this signal marks the point at which the copy
  // is complete.
  queue->Signal(fence_.Get(), pending_fence_value_);
  fence_value_ = pending_fence_value_;
}

void HdrState::PollReadback(ID3D12Fence* fence, const Settings& settings) {
  if (!pending_measurement_ || fence == nullptr) return;

  if (fence->GetCompletedValue() < pending_fence_value_) return;

  uint32_t bytes = 0;
  const void* mapped = readback_.TryGet(fence, &bytes);
  pending_measurement_ = false;

  if (mapped == nullptr || bytes < kHistogramBytes) return;

  const uint32_t* bins = static_cast<const uint32_t*>(mapped);
  uint32_t total = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) total += bins[i];
  if (total == 0) return;

  UpdateExposure(bins, total, settings);

  // Copy the raw histogram into our diagnostics buffer.
  for (uint32_t i = 0; i < kHistogramBins; ++i) last_histogram_[i] = bins[i];
  have_histogram_ = true;
  last_samples_ = total;

  frames_since_measure_ = 0;
}

void HdrState::UpdateExposure(const uint32_t* bins, uint32_t total,
                              const Settings& settings) {
  const float p_mid_bin = PercentileBin(bins, total, 0.50f);
  const float p_hi_bin =
      PercentileBin(bins, total,
                    std::clamp(settings.hdr_white_percentile, 0.90f, 0.999f));

  const float luma_mid = BinToLinear(p_mid_bin);
  const float luma_hi = BinToLinear(p_hi_bin);

  const float target =
      (luma_mid > 1e-6f)
          ? std::clamp(settings.hdr_midtone / luma_mid, 0.01f, 128.0f)
          : exposure_ema_;

  // Publish to the diagnostics fields.
  p50_luma_ = luma_mid;
  p_hi_luma_ = luma_hi;
  target_exposure_ = target;

  // Warm-up: converge quickly for the first ~30 measurements, then blend
  // at a rate proportional to how often we measure. A short interval means
  // fresher data, so the EMA can move faster; a long one means stale data,
  // so it smooths more. Converted back to the [0,1] responsiveness scale
  // used below.
  const uint32_t interval =
      static_cast<uint32_t>(std::clamp(settings.hdr_measure_interval, 1, 32));
  const float resp = (32.0f - static_cast<float>(interval)) / 31.0f;
  const float alpha = warmup_frames_ < 30
                          ? 0.5f
                          : (0.02f + resp * 0.38f);

  exposure_ema_ = Lerp(exposure_ema_, target, alpha);

  const float W_target = std::max(1.0f, luma_hi * exposure_ema_);
  white_point_ema_ = Lerp(white_point_ema_, W_target, alpha);

  have_measurement_ = true;
  warmup_frames_++;
}

// ---------------------------------------------------------------------------
// Pipeline step registration
// ---------------------------------------------------------------------------
//
// The HDR histogram is a single stage-local unit: HdrState::BeginFrame emits
// the zero, the build, and the readback enqueue in one contiguous block, and
// splitting them would mean threading the histogram resource and constants
// through PassRun for no benefit. So HdrBeginFrame is the real step, and the
// three histogram nodes are declared in the graph as documentation of what it
// does. Splitting them later is a matter of moving code from BeginFrame into
// the three step bodies below and listing them in kPostDlssSteps; the graph's
// shape does not have to change.
namespace {

PipelineStatus RunHdrBeginFrame(PassRun& run) {
  // The controller called BeginFrame exactly once per frame, before the pass
  // loop. As a step it is listed once in the PostDLSS stage, which runs that
  // step per pass — so the gate moves here. Only the first pass in the chain
  // measures.
  if (!run.is_first_pass) return PipelineStatus::Ok;
  if (run.hdr == nullptr || run.hdr_cmd == nullptr) return PipelineStatus::Ok;
  if (run.guides.dlss_output_color == nullptr || run.guides.output_width == 0 ||
      run.guides.output_height == 0) {
    TRACE_EVT(run.session, "hdr_begin_frame SKIP (no guide color)");
    return PipelineStatus::Ok;
  }

  TRACE_EVT(run.session, "hdr_begin_frame %ux%u", run.guides.output_width,
            run.guides.output_height);

  // hdr_cmd is the controller-owned scratch context the histogram records
  // through. Carried on the run so the graph has no controller coupling.
  run.hdr->BeginFrame(*run.hdr_cmd, run.list, run.guides.dlss_output_color,
                      run.guides.output_width, run.guides.output_height,
                      *run.settings, *run.codec);
  return PipelineStatus::Ok;
}

// Placeholder steps: the work is inside RunHdrBeginFrame above. They exist so
// the graph names the histogram sub-steps explicitly.
PipelineStatus RunHistogramPlaceholder(PassRun&) { return PipelineStatus::Ok; }

const PipelineNodeId kHdrBeginRequires[] = {PipelineNodeId::EnsureComputeEngine,
                                            PipelineNodeId::Count};
const PipelineNodeId kHistogramRequires[] = {PipelineNodeId::HdrBeginFrame,
                                             PipelineNodeId::Count};

const PipelineStepDef kHdrSteps[] = {
    {PipelineNodeId::HdrBeginFrame, "HdrBeginFrame", &RunHdrBeginFrame,
     kHdrBeginRequires, /*requires_hdr_aware=*/true},
    {PipelineNodeId::HistogramZero, "HistogramZero", &RunHistogramPlaceholder,
     kHistogramRequires, /*requires_hdr_aware=*/true},
    {PipelineNodeId::HistogramBuild, "HistogramBuild", &RunHistogramPlaceholder,
     kHistogramRequires, /*requires_hdr_aware=*/true},
    {PipelineNodeId::HistogramReadback, "HistogramReadback",
     &RunHistogramPlaceholder, kHistogramRequires, /*requires_hdr_aware=*/true},
};

}  // namespace

const PipelineStepDef* HdrStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kHdrSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr
