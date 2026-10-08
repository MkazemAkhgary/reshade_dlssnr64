#include "hdr/hdr_state.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <bit>

#include <reshade.hpp>

#include "gpu/d3d12_utils.h"
#include "pipeline/pipeline_graph.h"
#include "skin/skin_detect.h"
#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {

HdrFormat DetectFormatFromDxgi(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return HdrFormat::ScRgb;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return HdrFormat::Hdr10;
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
constexpr uint32_t kMaskWeightScale = 16;
constexpr uint32_t kHistogramBytes = kHistogramBins * sizeof(uint32_t);
constexpr uint32_t kCombinedHistogramBytes = 2 * kHistogramBytes;
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
    default: return 0.0f;
  }
}

float PercentileBin(const uint32_t* bins, uint32_t total, float fraction) {
  if (bins == nullptr || total == 0) return 0.0f;
  const float f = std::clamp(fraction, 0.0f, 1.0f);
  const uint32_t target = static_cast<uint32_t>(f * total);
  uint32_t cumulative = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    cumulative += bins[i];
    if (cumulative >= target) return static_cast<float>(i);
  }
  return static_cast<float>(kHistogramBins - 1);
}

float BinToLinear(float bin) {
  const float t = bin / 255.0f;
  const float l2 = kLumaMinLog2 + t * (kLumaMaxLog2 - kLumaMinLog2);
  return std::pow(2.0f, l2);
}

// Log-average the histogram over the configured percentile interval. This is
// the histogram analogue of average-log-luminance: bright outliers above the
// selected interval do not dictate the global eye-adaptation target, while
// the full frame remains the population being measured.
float HistogramBandLuma(const uint32_t* bins, uint32_t total,
                        float low_fraction, float high_fraction) {
  if (bins == nullptr || total == 0 || high_fraction <= low_fraction)
    return 0.0f;

  const double low_target =
      std::clamp(static_cast<double>(low_fraction), 0.0, 1.0) * total;
  const double high_target =
      std::clamp(static_cast<double>(high_fraction), 0.0, 1.0) * total;
  if (high_target <= low_target) return 0.0f;

  const double span = kLumaMaxLog2 - kLumaMinLog2;
  const double denom = kHistogramBins - 1.0;
  double weighted_log2 = 0.0;
  double weight = 0.0;
  double cumulative = 0.0;

  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    const double count = static_cast<double>(bins[i]);
    const double begin = cumulative;
    const double end = cumulative + count;
    cumulative = end;
    if (count <= 0.0 || end <= low_target || begin >= high_target)
      continue;

    const double used = std::min(end, high_target) -
                        std::max(begin, low_target);
    if (used <= 0.0) continue;

    const double t = static_cast<double>(i) / denom;
    const double log2_luma = kLumaMinLog2 + t * span;
    weighted_log2 += used * log2_luma;
    weight += used;
  }

  return weight > 0.0
             ? std::pow(2.0f, static_cast<float>(weighted_log2 / weight))
             : 0.0f;
}

}  // namespace

void ExposureTracker::Reset() { *this = ExposureTracker{}; }

void ExposureTracker::Update(const uint32_t* bins,
                             const ExposureParams& params,
                             float delta_seconds) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) total += bins[i];
  if (total == 0) return;

  const bool auto_local = params.exposure_method == HdrExposureMethod::AutoLocal;
  float exposure_reference_luma = auto_local
      ? HistogramBandLuma(bins, total, params.auto_low_percent,
                          params.auto_high_percent)
      : BinToLinear(PercentileBin(bins, total, 0.50f));

  const float safe_reference = std::max(exposure_reference_luma, 1e-6f);
  const float raw_target = std::clamp(
      params.midtone / safe_reference, 0.01f, 4096.0f);

  const bool was_ready = have_measurement;
  const float dt = std::clamp(delta_seconds, 1.0f / 240.0f, 0.5f);
  float target = raw_target;

  if (auto_local) {
    if (was_ready) {
      const float log_current = std::log2(std::max(exposure, 1e-6f));
      float delta = std::log2(raw_target) - log_current;
      if (delta >= 0.0f) {
        delta = std::min(delta, std::max(params.speed_up, 0.0f) * dt);
      } else {
        delta = std::max(delta, -std::max(params.speed_down, 0.0f) * dt);
      }
      target = std::exp2(log_current + delta);
    }
  } else {
    const float max_delta = warmup_frames < 30 ? 0.5f : params.rate_cap;
    if (max_delta > 0.0f) {
      const float log_current = std::log2(std::max(exposure, 1e-6f));
      float delta = std::log2(raw_target) - log_current;
      delta = std::clamp(delta, -max_delta, max_delta);
      target = std::exp2(log_current + delta);
    }
  }

  exposure_reference_luma = std::max(exposure_reference_luma, 1e-6f);
  target_exposure = target;
  metering_source = MeteringSource::FullFrame;

  if (!was_ready) {
    exposure = target;
  } else if (auto_local) {
    exposure = target;
  } else {
    const uint32_t interval =
        std::clamp(params.measure_interval, 1u, 32u);
    const float resp =
        (32.0f - static_cast<float>(interval)) / 31.0f;
    const float alpha = 0.02f + resp * 0.38f;
    exposure = exposure + (target - exposure) * alpha;
  }

  const float white_fraction = auto_local
      ? std::clamp(params.auto_high_percent, 0.01f, 0.999f)
      : std::clamp(params.white_percentile, 0.90f, 0.999f);
  const float white_luma =
      BinToLinear(PercentileBin(bins, total, white_fraction));

  const float white_alpha = auto_local
      ? (was_ready ? 1.0f - std::exp(-6.0f * dt) : 1.0f)
      : (was_ready ? 0.08f : 1.0f);
  scene_white = scene_white +
                (std::max(white_luma, 1e-6f) - scene_white) * white_alpha;

  this->exposure_reference_luma = exposure_reference_luma;
  have_measurement = true;
  warmup_frames += 1.0f;
}

bool HdrState::Ensure(ID3D12Device* device) {
  if (device == nullptr) return false;
  if (device_.Get() == device && histogram_ != nullptr && fence_ != nullptr)
    return true;

  Shutdown();

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kCombinedHistogramBytes;
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

  if (!readback_.Ensure(device, kCombinedHistogramBytes) ||
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
  detection_.Reset();
  frame_counter_ = 0;
  frames_until_measure_ = 0;
  frames_since_measure_ = 0;
  measurement_generation_ = 0;
  have_logged_format_ = false;
  have_histogram_ = false;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    last_histogram_[i] = 0;
    last_detection_histogram_[i] = 0;
  }
  p_hi_luma_ = 0.0f;
  last_samples_ = 0;
  last_detection_samples_ = 0;
  have_measurement_time_ = false;
  last_measurement_time_ = {};
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
  s.detection_exposure = detection_.exposure;
  s.detection_scene_white = detection_.scene_white;
  s.detection_samples = last_detection_samples_;
  if (last_detection_samples_ != 0) {
    for (uint32_t i = 0; i < kHistogramBins; ++i)
      s.detection_histogram[i] = last_detection_histogram_[i];
  }
  s.frames_since_measure = frames_since_measure_;
  s.has_histogram = have_histogram_;
  if (have_histogram_) {
    for (uint32_t i = 0; i < kHistogramBins; ++i)
      s.histogram[i] = last_histogram_[i];
  }
  s.exposure_reference_luma = regular_.exposure_reference_luma;
  s.p_hi_luma = p_hi_luma_;
  s.target_exposure = regular_.target_exposure;
  s.samples = last_samples_;
  s.measurement_generation = measurement_generation_;
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
                          const Settings& settings,
                          ComputeEngine& codec,
                          ID3D12Resource* exposure_mask,
                          unsigned int exposure_mask_width,
                          unsigned int exposure_mask_height) {
  enabled_ = settings.hdr.enable;
  if (!enabled_) return;

  if (command_list == nullptr || source == nullptr || width == 0 || height == 0)
    return;

  frames_since_measure_++;
  PollReadback(fence_.Get(), settings);

  const HdrFormat prev_format = resolved_format_;
  resolved_format_ = DetectFormatFromResource(source, settings.hdr.format);
  if (resolved_format_ != prev_format || !have_logged_format_) {
    const DXGI_FORMAT raw_fmt = source->GetDesc().Format;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "HDR: DLSS output format=%d detected=%d (0=SDR,1=scRGB,2=HDR10)",
                  static_cast<int>(raw_fmt), HdrFormatCode(resolved_format_));
    reshade::log::message(reshade::log::level::info, msg);
    have_logged_format_ = true;
  }

  if (pending_measurement_) {
    frame_counter_++;
    return;
  }

  const uint32_t capped_every =
      static_cast<uint32_t>(std::clamp(settings.hdr.measure_interval, 1, 32));
  if (frames_until_measure_ > 0) {
    frames_until_measure_--;
    frame_counter_++;
    return;
  }
  frames_until_measure_ = capped_every - 1;

  if (!codec.IsReady()) {
    frame_counter_++;
    return;
  }

  const bool use_mask =
      exposure_mask != nullptr && exposure_mask_width != 0 &&
      exposure_mask_height != 0;

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.shader.Params0.x = FormatToShaderValue(resolved_format_);
  constants.shader.Params0.y = kLumaMinLog2;
  constants.shader.Params0.z = kLumaMaxLog2;
  constants.shader.Params1 = {};
  constants.shader.Params1.x = static_cast<float>(exposure_mask_width);
  constants.shader.Params1.y = static_cast<float>(exposure_mask_height);
  constants.shader.Params1.z = use_mask ? 1.0f : 0.0f;

  ID3D12Resource* srvs[kSrvCount] = {
      source, exposure_mask != nullptr ? exposure_mask : source, source, source};
  ID3D12Resource* uavs[kUavCount] = {histogram_.Get()};

  cmd.SetResourceState(source,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.SetResourceState(histogram_.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.Transition(command_list, source,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (use_mask) {
    cmd.SetResourceState(exposure_mask,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd.Transition(command_list, exposure_mask,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Clear the full-frame half. The mask-conditioned half is cleared only when
  // a valid previous-frame detection mask is available.
  constants.shader.Params1.w = 0.0f;
  if (!codec.Dispatch("ZeroHistogram", command_list, constants, srvs, uavs,
                      /*threads_x=*/1, /*threads_y=*/1, /*threads_z=*/1)) {
    frame_counter_++;
    return;
  }
  if (use_mask) {
    constants.shader.Params1.w = static_cast<float>(kHistogramBins);
    if (!codec.Dispatch("ZeroHistogram", command_list, constants, srvs, uavs,
                        /*threads_x=*/1, /*threads_y=*/1, /*threads_z=*/1)) {
      frame_counter_++;
      return;
    }
  }

  cmd.UavBarrier(command_list, histogram_.Get());
  constants.shader.Params1.z = 0.0f;
  constants.shader.Params1.w = 0.0f;
  if (!codec.Dispatch("BuildHistogram", command_list, constants, srvs, uavs)) {
    frame_counter_++;
    return;
  }

  if (use_mask) {
    cmd.UavBarrier(command_list, histogram_.Get());
    constants.shader.Params1.z = 1.0f;
    constants.shader.Params1.w = static_cast<float>(kHistogramBins);
    if (!codec.Dispatch("BuildHistogram", command_list, constants, srvs, uavs)) {
      frame_counter_++;
      return;
    }
  }

  const uint64_t next_value = fence_value_ + 1;
  cmd.UavBarrier(command_list, histogram_.Get());
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (readback_.EnqueueCopy(command_list, histogram_.Get(),
                             use_mask ? kCombinedHistogramBytes
                                      : kHistogramBytes,
                             fence_.Get(), next_value)) {
    pending_fence_value_ = next_value;
    pending_measurement_ = true;
  }
  cmd.Transition(command_list, histogram_.Get(),
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  frame_counter_++;
}

bool HdrState::EnqueueGainMapStatsReadback(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* stats) {
  if (command_list == nullptr || stats == nullptr || fence_ == nullptr ||
      (!device_) || pending_gain_stats_)
    return false;

  const uint64_t next_value =
      pending_measurement_ ? pending_fence_value_ : (fence_value_ + 1u);
  if (!gain_readback_.EnqueueCopy(command_list, stats, kGainStatsBytes,
                                  fence_.Get(), next_value))
    return false;
  pending_gain_fence_value_ = next_value;
  pending_gain_stats_ = true;
  return true;
}

void HdrState::SignalAtPresent(ID3D12CommandQueue* queue) {
  if (queue == nullptr || fence_ == nullptr ||
      (!pending_measurement_ && !pending_gain_stats_))
    return;
  uint64_t signal_value = fence_value_;
  if (pending_measurement_)
    signal_value = std::max(signal_value, pending_fence_value_);
  if (pending_gain_stats_)
    signal_value = std::max(signal_value, pending_gain_fence_value_);
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
      const uint32_t* all_bins = static_cast<const uint32_t*>(mapped);
      const uint32_t* full_bins = all_bins;
      const uint32_t* mask_bins =
          bytes >= kCombinedHistogramBytes ? all_bins + kHistogramBins : nullptr;

      uint32_t full_total = 0;
      uint32_t mask_total = 0;
      for (uint32_t i = 0; i < kHistogramBins; ++i) {
        full_total += full_bins[i];
        if (mask_bins != nullptr) mask_total += mask_bins[i];
      }

      uint32_t effective_bins[kHistogramBins] = {};
      const uint32_t* bins = full_bins;
      bool used_mask = false;

      if (mask_bins != nullptr && full_total != 0 && mask_total != 0) {
        const float mask_fraction =
            std::clamp(static_cast<float>(mask_total) /
                           (static_cast<float>(full_total) *
                            static_cast<float>(kMaskWeightScale)),
                       0.0f, 1.0f);
        const float priority =
            std::clamp(settings.hdr.skin_priority, 0.0f, 1.0f);
        const float min_fraction =
            std::clamp(settings.hdr.skin_fraction_threshold, 0.0f, 0.5f);

        if (priority > 0.0f && mask_fraction >= min_fraction) {
          // Give the masked population a configurable share of the effective
          // metering histogram, independent of how large that mask happens
          // to be. The unmasked full-frame population supplies the rest.
          const float inverse_fraction =
              1.0f / std::max(mask_fraction, 1e-6f);
          const float unmasked_weight = 1.0f - priority;
          for (uint32_t i = 0; i < kHistogramBins; ++i) {
            const float masked_normalized =
                static_cast<float>(mask_bins[i]) *
                (inverse_fraction / static_cast<float>(kMaskWeightScale));
            const float blended =
                unmasked_weight * static_cast<float>(full_bins[i]) +
                priority * masked_normalized;
            effective_bins[i] =
                static_cast<uint32_t>(std::clamp(blended, 0.0f,
                                                 4294967295.0f));
          }
          bins = effective_bins;
          used_mask = true;
        }
      }

      uint32_t total = 0;
      for (uint32_t i = 0; i < kHistogramBins; ++i) total += bins[i];
      if (total != 0) {
        for (uint32_t i = 0; i < kHistogramBins; ++i)
          last_detection_histogram_[i] = full_bins[i];
        last_detection_samples_ = full_total;

        UpdateExposure(bins, full_bins, full_total, settings);
        for (uint32_t i = 0; i < kHistogramBins; ++i)
          last_histogram_[i] = bins[i];
        have_histogram_ = true;
        last_samples_ = total;
        regular_.metering_source = used_mask
            ? MeteringSource::SkinMaskPriority
            : MeteringSource::FullFrame;
        measurement_generation_ += 1;
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
      if (std::isfinite(gain_min_log2_) && std::isfinite(gain_max_log2_))
        have_gain_stats_ = true;
    }
  }
}

void HdrState::UpdateExposure(const uint32_t* bins, const uint32_t* full_bins,
                              uint32_t full_total,
                              const Settings& settings) {
  const auto now = std::chrono::steady_clock::now();
  const float delta_seconds = have_measurement_time_
      ? std::chrono::duration<float>(now - last_measurement_time_).count()
      : (1.0f / 60.0f);
  last_measurement_time_ = now;
  have_measurement_time_ = true;

  ExposureParams params;
  params.midtone = settings.hdr.midtone;
  params.white_percentile = settings.hdr.white_percentile;
  params.exposure_method = settings.hdr.exposure_method;
  params.auto_low_percent = settings.hdr.auto_low_percent;
  params.auto_high_percent = settings.hdr.auto_high_percent;
  params.speed_up = settings.hdr.speed_up;
  params.speed_down = settings.hdr.speed_down;
  params.rate_cap = settings.hdr.rate_cap;
  params.measure_interval = static_cast<uint32_t>(settings.hdr.measure_interval);

  regular_.Update(bins, params, delta_seconds);
  if (full_bins != nullptr && full_total != 0) {
    // Detection exposure is always based on the raw full-frame population.
    // The structure mask is intentionally excluded here so the mask cannot
    // perturb the exposure that is used to generate the next mask.
    detection_.Update(full_bins, params, delta_seconds);
  }
  p_hi_luma_ = regular_.scene_white;
}

namespace {

PipelineStatus RunHdrBeginFrame(PassRun& run) {
  if (!run.is_first_pass) return PipelineStatus::Ok;
  if (run.hdr == nullptr || run.hdr_cmd == nullptr) return PipelineStatus::Ok;
  if (run.guides.dlss_output_color == nullptr || run.guides.output_width == 0 ||
      run.guides.output_height == 0) {
    TRACE_EVT(run.session, "hdr_begin_frame SKIP (no guide color)");
    return PipelineStatus::Ok;
  }

  ID3D12Resource* exposure_mask = nullptr;
  unsigned int exposure_mask_width = 0;
  unsigned int exposure_mask_height = 0;
  if (run.settings->skin.enable && run.skin_detect != nullptr) {
    const SkinDetect::Output& detection = run.skin_detect->GetOutput();
    if (detection.valid && detection.mask != nullptr && detection.width != 0 &&
        detection.height != 0) {
      exposure_mask = detection.mask;
      exposure_mask_width = detection.width;
      exposure_mask_height = detection.height;
    }
  }

  TRACE_EVT(run.session, "hdr_begin_frame %ux%u mask=%d",
            run.guides.output_width, run.guides.output_height,
            exposure_mask != nullptr ? 1 : 0);
  run.hdr->BeginFrame(*run.hdr_cmd, run.list, run.guides.dlss_output_color,
                      run.guides.output_width, run.guides.output_height,
                      *run.settings, *run.codec, exposure_mask,
                      exposure_mask_width, exposure_mask_height);
  return PipelineStatus::Ok;
}

PipelineStatus RunHistogramPlaceholder(PassRun&) { return PipelineStatus::Ok; }

constexpr PipelineNodeId kHdrBeginRequires[] = {
    PipelineNodeId::EnsureComputeEngine,
    PipelineNodeId::Count};
constexpr PipelineNodeId kHistogramRequires[] = {
    PipelineNodeId::HdrBeginFrame,
    PipelineNodeId::Count};

constexpr PipelineStepDef kHdrSteps[] = {
    {PipelineNodeId::HdrBeginFrame, "HdrBeginFrame", &RunHdrBeginFrame,
     kHdrBeginRequires, StageInput::CommandList, true},
    {PipelineNodeId::HistogramZero, "HistogramZero", &RunHistogramPlaceholder,
     kHistogramRequires, StageInput::None, true},
    {PipelineNodeId::HistogramBuild, "HistogramBuild", &RunHistogramPlaceholder,
     kHistogramRequires, StageInput::None, true},
    {PipelineNodeId::HistogramReadback, "HistogramReadback",
     &RunHistogramPlaceholder, kHistogramRequires, StageInput::None, true},
};

static_assert(PrerequisitesPrecedeSteps(
                  kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes), kHdrSteps,
                  ArrayCount(kHdrSteps)),
              "an HDR step declares a prerequisite that the PostDLSS stage "
              "lists after it - reorder kPostDlssStepNodes, not this");

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
