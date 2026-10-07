#include "hdr/hdr_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <bit>

#include <reshade.hpp>

#include "gpu/d3d12_utils.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"
#include "skin/skin_detect.h"

namespace reshade_dlssnr {

HdrFormat DetectFormatFromDxgi(DXGI_FORMAT format) {
  switch (format) {
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

namespace {

constexpr uint32_t kHistogramBins = 256;
// Three histograms packed into one buffer: [0, 256) full frame, [256, 512)
// depth-filtered, [512, 768) skin. The shader splits it at the same offsets.
constexpr uint32_t kHistogramSlices = 3;
constexpr uint32_t kHistogramTotalBins = kHistogramSlices * kHistogramBins;
constexpr uint32_t kHistogramBytes = kHistogramTotalBins * sizeof(uint32_t);
constexpr float kLumaMinLog2 = -12.0f;
constexpr float kLumaMaxLog2 = 12.0f;

HdrFormat DetectFormatFromResource(ID3D12Resource* resource,
                                   HdrFormat override) {
  if (override != HdrFormat::Auto) return override;
  if (resource == nullptr) return HdrFormat::Sdr;
  return DetectFormatFromDxgi(resource->GetDesc().Format);
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

void ExposureTracker::Reset() { *this = ExposureTracker{}; }

// Same EMA logic for both trackers; `params` and `use_skin` are the only
// differences between them. Headroom and exposure are not applied here.
// `scene_white` is the measured source-domain white percentile; the split
// applies the exposure to the image and keeps headroom as a separate peak
// control so exposure cannot cancel out of the tone-map normalization.
void ExposureTracker::Update(const uint32_t* bins, const ExposureParams& params,
                             bool use_skin) {
  const uint32_t* full_bins = bins;
  const uint32_t* depth_bins = bins + kHistogramBins;
  const uint32_t* skin_bins = bins + 2u * kHistogramBins;

  uint32_t total_full = 0;
  uint32_t total_depth = 0;
  uint32_t total_skin = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    total_full += full_bins[i];
    total_depth += depth_bins[i];
    total_skin += skin_bins[i];
  }
  if (total_full == 0) return;

  // Priority: skin > depth-filtered > full frame. The full frame keeps the
  // measurement alive when the other two are empty or too sparse.
  const float depth_survived =
      static_cast<float>(total_depth) / static_cast<float>(total_full);

  // Scene source: depth-filtered if usable, else the full frame.
  const uint32_t* scene_bins = full_bins;
  uint32_t scene_total = total_full;
  MeteringSource source = MeteringSource::FullFrame;

  const bool depth_usable =
      total_depth > 0u &&
      depth_survived >= params.depth_fallback_fraction;
  if (depth_usable) {
    scene_bins = depth_bins;
    scene_total = total_depth;
    source = MeteringSource::DepthFiltered;
  }

  const float p_mid_scene_bin = PercentileBin(scene_bins, scene_total, 0.50f);
  const float luma_mid_scene = BinToLinear(p_mid_scene_bin);
  float log_target = (luma_mid_scene > 1e-6f)
                         ? log2(params.midtone / luma_mid_scene)
                         : log2(std::max(exposure, 1e-6f));

  // Skin is a pull, not a takeover: blend toward the skin target in log2
  // space so the depth/full scene keeps a say. The gate above (fraction
  // threshold) decides whether skin is considered at all.
  const float skin_frac =
      static_cast<float>(total_skin) / static_cast<float>(total_full);
  bool skin_blended = false;
  if (use_skin && total_skin > 1000u &&
      skin_frac > params.skin_fraction_threshold) {
    const float p_mid_skin_bin = PercentileBin(skin_bins, total_skin, 0.50f);
    const float luma_mid_skin = BinToLinear(p_mid_skin_bin);
    if (luma_mid_skin > 1e-6f) {
      const float log_skin = log2(params.midtone / luma_mid_skin);
      const float t = std::clamp(params.skin_priority, 0.0f, 1.0f);
      log_target = Lerp(log_target, log_skin, t);
      skin_blended = (t > 0.0f);
      if (skin_blended) source = MeteringSource::Skin;
    }
  }

  const float p_hi_bin =
      PercentileBin(full_bins, total_full,
                    std::clamp(params.white_percentile, 0.90f, 0.999f));

  const float luma_hi = BinToLinear(p_hi_bin);
  const float luma_mid = params.midtone / exp2(log_target);

  // 4096 = 2^12, matching the histogram's own log2 range. Very dark scenes
  // need well above 2^7 to reach the default 0.18 midtone.
  const float raw_target =
      std::clamp(exp2(log_target), 0.01f, 4096.0f);

  // Rate cap in log2 space. Skin and warmup bypass it so they can lock
  // quickly; 0 disables it outright.
  const bool skin_drives = skin_blended;
  const float log_max_delta =
      warmup_frames < 30 ? 0.5f : params.rate_cap;
  float capped_target = raw_target;
  if (!skin_drives && log_max_delta > 0.0f) {
    const float log_current = log2(std::max(exposure, 1e-6f));
    float log_delta = log2(raw_target) - log_current;
    log_delta = std::clamp(log_delta, -log_max_delta, log_max_delta);
    capped_target = exp2(log_current + log_delta);
  }

  p50_luma = luma_mid;
  target_exposure = capped_target;
  skin_fraction = skin_frac;
  target_is_skin = (source == MeteringSource::Skin);
  depth_filter_survived = depth_survived;
  metering_source = source;

  // Warm-up and skin converge fast; otherwise the blend rate scales with
  // the measurement interval, so a longer interval smooths more.
  const uint32_t interval =
      static_cast<uint32_t>(std::clamp(params.measure_interval, 1u, 32u));
  const float resp = (32.0f - static_cast<float>(interval)) / 31.0f;
  const float alpha = (skin_drives || warmup_frames < 30)
                          ? 0.5f
                          : (0.02f + resp * 0.38f);

  exposure = Lerp(exposure, capped_target, alpha);
  // Keep the measured white point in source units. Exposure is applied to the
  // sampled HDR pixel by GainMapSplit; folding exposure into this white point
  // would cancel the very control that is meant to lift dark scenes.
  const float W_target = std::max(luma_hi, 1e-6f);
  scene_white = Lerp(scene_white, W_target, alpha);

  have_measurement = true;
  warmup_frames++;
}

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

  if (!readback_.Ensure(device, kHistogramBytes) ||
      !gain_readback_.Ensure(device, kGainStatsBytes)) {
    Shutdown();
    return false;
  }

  device_ = device;
  return true;
}

void HdrState::Shutdown() {
  readback_.Shutdown();
  gain_readback_.Shutdown();
  histogram_.Reset();
  fence_.Reset();
  fence_value_ = 0;
  pending_fence_value_ = 0;
  pending_measurement_ = false;
  pending_gain_fence_value_ = 0;
  pending_gain_stats_ = false;
  device_.Reset();

  enabled_ = false;
  regular_.Reset();
  fallback_.Reset();
  frame_counter_ = 0;
  frames_until_measure_ = 0;
  frames_since_measure_ = 0;
  have_logged_format_ = false;

  have_histogram_ = false;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    last_histogram_[i] = 0;
    last_histogram_skin_[i] = 0;
  }
  p_hi_luma_ = 0.0f;
  last_samples_ = 0;
  have_gain_stats_ = false;
  gain_min_log2_ = 0.0f;
  gain_max_log2_ = 0.0f;
  gain_sdr_clipped_pixels_ = 0;
  gain_out_of_range_pixels_ = 0;
}

HdrSnapshot HdrState::GetSnapshot() const {
  HdrSnapshot s;
  s.enabled = enabled_;
  s.ready = regular_.have_measurement;
  s.resolved_format = resolved_format_;
  s.exposure = regular_.exposure;
  s.scene_white = regular_.scene_white;
  s.fallback_exposure = fallback_.exposure;
  s.fallback_scene_white = fallback_.scene_white;
  s.fallback_metering_source = static_cast<int>(fallback_.metering_source);
  s.frames_since_measure = frames_since_measure_;

  s.has_histogram = have_histogram_;
  if (have_histogram_) {
    for (uint32_t i = 0; i < kHistogramBins; ++i) {
      s.histogram[i] = last_histogram_[i];
      s.histogram_skin[i] = last_histogram_skin_[i];
    }
  }
  s.p50_luma = regular_.p50_luma;
  s.p_hi_luma = p_hi_luma_;
  s.target_exposure = regular_.target_exposure;
  s.samples = last_samples_;
  s.skin_fraction = regular_.skin_fraction;
  s.target_is_skin = regular_.target_is_skin;
  s.depth_filter_survived = regular_.depth_filter_survived;
  s.metering_source = static_cast<int>(regular_.metering_source);
  s.gain_map_ready = have_gain_stats_;
  s.gain_min_log2 = gain_min_log2_;
  s.gain_max_log2 = gain_max_log2_;
  s.gain_sdr_clipped_pixels = gain_sdr_clipped_pixels_;
  s.gain_out_of_range_pixels = gain_out_of_range_pixels_;
  return s;
}

void HdrState::BeginFrame(CommandContext& cmd,
                          ID3D12GraphicsCommandList* command_list,
                          ID3D12Resource* source,
                          unsigned int width,
                          unsigned int height,
                          ID3D12Resource* depth,
                          bool depth_inverted,
                          ID3D12Resource* mask,
                          const Settings& settings,
                          ComputeEngine& codec) {
  enabled_ = settings.hdr.enable;
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
  resolved_format_ = DetectFormatFromResource(source, settings.hdr.format);

  // Log the raw DXGI format the first time we see it (or whenever it
  // changes). Makes format-detection issues easy to diagnose.
  if (resolved_format_ != prev_format || !have_logged_format_) {
    const DXGI_FORMAT raw_fmt = source->GetDesc().Format;
    char msg[160];
    std::snprintf(
        msg, sizeof(msg),
        "HDR: DLSS output format=%d detected=%d (0=SDR,1=scRGB,2=HDR10)",
        static_cast<int>(raw_fmt), HdrFormatCode(resolved_format_));
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
      static_cast<uint32_t>(std::clamp(settings.hdr.measure_interval, 1, 32));

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
  constants.shader.Params0.x = FormatToShaderValue(resolved_format_);
  constants.shader.Params0.y = kLumaMinLog2;
  constants.shader.Params0.z = kLumaMaxLog2;
  // shader.Params1 in the shader: depth availability, orientation, mask bound.
  constants.shader.Params1.x = depth != nullptr ? 1.0f : 0.0f;
  constants.shader.Params1.y = depth_inverted ? 1.0f : 0.0f;
  constants.shader.Params1.z = mask != nullptr ? 1.0f : 0.0f;

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
  if (depth != nullptr) {
    cmd.SetResourceState(depth,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  // The mask was left UAV by the skin-detect pass.
  if (mask != nullptr) {
    cmd.SetResourceState(mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  // The shader reads depth from t1 and the skin mask from t2 when each is
  // available; otherwise the kHasDepth / kHasMask flags skip the sample, so
  // binding source there is safe.
  ID3D12Resource* depth_srv = depth != nullptr ? depth : source;
  ID3D12Resource* mask_srv = mask != nullptr ? mask : source;
  ID3D12Resource* srvs[kSrvCount] = {source, depth_srv, mask_srv};
  ID3D12Resource* uavs[kUavCount] = {histogram_.Get()};

  cmd.Transition(command_list, source,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (depth != nullptr) {
    cmd.Transition(command_list, depth,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (mask != nullptr) {
    cmd.Transition(command_list, mask,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Clear then build.
  if (!codec.Dispatch("ZeroHistogram", command_list, constants, srvs, uavs,
                      /*threads_x=*/1, /*threads_y=*/1, /*threads_z=*/1)) {
    frame_counter_++;
    return;
  }
  cmd.UavBarrier(command_list, histogram_.Get());
  if (!codec.Dispatch("BuildHistogram", command_list, constants, srvs, uavs)) {
    frame_counter_++;
    return;
  }

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

bool HdrState::EnqueueGainMapStatsReadback(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* stats) {
  if (command_list == nullptr || stats == nullptr || fence_ == nullptr ||
      !device_ || pending_gain_stats_) {
    return false;
  }

  // If the histogram copy from BeginFrame is still pending, reuse its fence
  // value so both readback rings are guaranteed complete at the same queue
  // signal. Otherwise allocate the next value.
  const uint64_t next_value =
      pending_measurement_ ? pending_fence_value_ : (fence_value_ + 1u);
  if (!gain_readback_.EnqueueCopy(command_list, stats, kGainStatsBytes,
                                  fence_.Get(), next_value)) {
    return false;
  }

  pending_gain_fence_value_ = next_value;
  pending_gain_stats_ = true;
  return true;
}

void HdrState::SignalAtPresent(ID3D12CommandQueue* queue) {
  if (queue == nullptr || fence_ == nullptr ||
      (!pending_measurement_ && !pending_gain_stats_)) {
    return;
  }

  uint64_t signal_value = fence_value_;
  if (pending_measurement_) signal_value = std::max(signal_value, pending_fence_value_);
  if (pending_gain_stats_) signal_value = std::max(signal_value, pending_gain_fence_value_);

  queue->Signal(fence_.Get(), signal_value);
  fence_value_ = signal_value;
}

void HdrState::PollReadback(ID3D12Fence* fence, const Settings& settings) {
  if (fence == nullptr) return;

  const uint64_t completed = fence->GetCompletedValue();

  if (pending_measurement_ && completed >= pending_fence_value_) {
    uint32_t bytes = 0;
    const void* mapped = readback_.TryGet(fence, &bytes);
    pending_measurement_ = false;

    if (mapped != nullptr && bytes >= kHistogramBytes) {
      const uint32_t* bins = static_cast<const uint32_t*>(mapped);
      uint32_t total = 0;
      for (uint32_t i = 0; i < kHistogramTotalBins; ++i) total += bins[i];
      if (total != 0) {
        UpdateExposure(bins, settings);
        for (uint32_t i = 0; i < kHistogramBins; ++i) {
          last_histogram_[i] = bins[i];
          last_histogram_skin_[i] = bins[2u * kHistogramBins + i];
        }
        have_histogram_ = true;
        last_samples_ = total;
        frames_since_measure_ = 0;
      }
    }
  }

  if (pending_gain_stats_ && completed >= pending_gain_fence_value_) {
    uint32_t bytes = 0;
    const void* mapped = gain_readback_.TryGet(fence, &bytes);
    pending_gain_stats_ = false;

    if (mapped != nullptr && bytes >= kGainStatsBytes) {
      const uint32_t* stats = static_cast<const uint32_t*>(mapped);
      const uint32_t min_sortable = stats[0];
      const uint32_t max_sortable = stats[1];
      auto unsort = [](uint32_t sortable) {
        return (sortable & 0x80000000u) != 0u
            ? (sortable ^ 0x80000000u)
            : ~sortable;
      };

      gain_min_log2_ = std::bit_cast<float>(unsort(min_sortable));
      gain_max_log2_ = std::bit_cast<float>(unsort(max_sortable));
      gain_sdr_clipped_pixels_ = stats[2];
      gain_out_of_range_pixels_ = stats[3];
      if (std::isfinite(gain_min_log2_) && std::isfinite(gain_max_log2_)) {
        have_gain_stats_ = true;
      }
    }
  }
}

void HdrState::UpdateExposure(const uint32_t* bins, const Settings& settings) {
  // Same histogram, two independent EMAs with independent parameters. The
  // regular tracker may use the skin slice (which the mask gates); the
  // fallback tracker never does, so the skin-detect pass's exposure cannot
  // be driven by the mask it produced.
  ExposureParams regular_params;
  regular_params.midtone = settings.hdr.midtone;
  regular_params.white_percentile = settings.hdr.white_percentile;
  regular_params.rate_cap = settings.hdr.rate_cap;
  regular_params.skin_fraction_threshold =
      settings.hdr.skin_fraction_threshold;
  regular_params.skin_priority = settings.hdr.skin_priority;
  regular_params.depth_fallback_fraction =
      settings.hdr.depth_fallback_fraction;
  regular_params.measure_interval =
      static_cast<uint32_t>(settings.hdr.measure_interval);

  // The skin-detect tracker keeps the scene's white point (that describes
  // the scene, not the pass) but has its own target, response, and depth
  // fallback.
  ExposureParams fallback_params = regular_params;
  fallback_params.midtone = settings.skin.hdr_midtone;
  fallback_params.rate_cap = settings.skin.hdr_rate_cap;
  fallback_params.depth_fallback_fraction =
      settings.skin.hdr_depth_fallback_fraction;
  fallback_params.measure_interval =
      static_cast<uint32_t>(settings.skin.hdr_measure_interval);

  regular_.Update(bins, regular_params, /*use_skin=*/true);
  fallback_.Update(bins, fallback_params, /*use_skin=*/false);

  // Shared: the P99 of the full-frame histogram, for diagnostics. Both
  // trackers compute it internally; this copy is only for the UI.
  uint32_t total_full = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) total_full += bins[i];
  if (total_full == 0) return;
  const float p_hi_bin =
      PercentileBin(bins, total_full,
                    std::clamp(settings.hdr.white_percentile, 0.90f, 0.999f));
  p_hi_luma_ = BinToLinear(p_hi_bin);
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
  // Depth filtering removes the sky, which is the largest single source of
  // exposure flicker from camera movement. The guide is only valid together
  // with motion, so a valid guide implies a non-null depth clone.
  ID3D12Resource* depth = run.guides.valid ? run.guides.depth : nullptr;
  ID3D12Resource* mask = (run.skin_detect != nullptr)
                             ? run.skin_detect->GetOutput().mask
                             : nullptr;
  run.hdr->BeginFrame(*run.hdr_cmd, run.list, run.guides.dlss_output_color,
                      run.guides.output_width, run.guides.output_height,
                      depth, run.guides.depth_inverted, mask,
                      *run.settings, *run.codec);
  return PipelineStatus::Ok;
}

// Placeholder steps: the work is inside RunHdrBeginFrame above. They exist so
// the graph names the histogram sub-steps explicitly.
PipelineStatus RunHistogramPlaceholder(PassRun&) { return PipelineStatus::Ok; }

// `constexpr`, not `const`: a static_assert can only read an array whose
// initializer is a constant expression. `const` would compile fine for the
// runtime lookup in HdrStepFor below and still fail the assertion at the end
// of this namespace with C2131.
constexpr PipelineNodeId kHdrBeginRequires[] = {PipelineNodeId::EnsureComputeEngine,
                                                PipelineNodeId::SkinDetect,
                                                PipelineNodeId::Count};
constexpr PipelineNodeId kHistogramRequires[] = {PipelineNodeId::HdrBeginFrame,
                                                 PipelineNodeId::Count};

constexpr PipelineStepDef kHdrSteps[] = {
    // Passes run.list into HdrState::BeginFrame, so it needs the stage to
    // supply a command list. PostDLSS does; this step is listed there only.
    {PipelineNodeId::HdrBeginFrame, "HdrBeginFrame", &RunHdrBeginFrame,
     kHdrBeginRequires, /*required_inputs=*/StageInput::CommandList,
     /*requires_hdr_aware=*/true},
    {PipelineNodeId::HistogramZero, "HistogramZero", &RunHistogramPlaceholder,
     kHistogramRequires, /*required_inputs=*/StageInput::None,
     /*requires_hdr_aware=*/true},
    {PipelineNodeId::HistogramBuild, "HistogramBuild", &RunHistogramPlaceholder,
     kHistogramRequires, /*required_inputs=*/StageInput::None,
     /*requires_hdr_aware=*/true},
    {PipelineNodeId::HistogramReadback, "HistogramReadback",
     &RunHistogramPlaceholder, kHistogramRequires, /*required_inputs=*/StageInput::None,
     /*requires_hdr_aware=*/true},
};

// HdrBeginFrame declares EnsureComputeEngine as a prerequisite, and this is the
// assertion that keeps it satisfiable. kPostDlssStepNodes is the stage's
// ordered node list, published in pipeline_graph.h; if HdrBeginFrame is listed
// before the engine it depends on, the runner would log "unmet prerequisite"
// and skip the step every frame, silently disabling HDR measurement. Now the
// build fails instead.
//
// This exact mistake shipped once. See the ordering comment on kPostDlssSteps
// in pipeline_graph.cpp.
static_assert(PrerequisitesPrecedeSteps(kPostDlssStepNodes,
                                        ArrayCount(kPostDlssStepNodes),
                                        kHdrSteps, ArrayCount(kHdrSteps)),
              "an HDR step declares a prerequisite that the PostDLSS stage "
              "lists after it - reorder kPostDlssStepNodes, not this");

// The HDR steps are listed in PostDLSS alone, whose call site supplies the
// game's command list. HdrBeginFrame reads run.list, so this proves it is
// never reachable from a stage that passes none.
static_assert(
    StageInputsCoverSteps(kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes),
                          kHdrSteps, ArrayCount(kHdrSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "an HDR step in the PostDLSS stage needs an input the PostDLSS call site "
    "does not supply");

}  // namespace

const PipelineStepDef* HdrStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kHdrSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr
