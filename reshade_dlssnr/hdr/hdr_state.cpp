#include "hdr/hdr_state.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <bit>

#include <reshade.hpp>

#include "gpu/d3d12_utils.h"
#include "pipeline/pipeline_graph.h"
#include "state/settings.h"
#include "importance_mask/importance_mask.h"
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
constexpr uint32_t kHistogramBytes = kHistogramBins * sizeof(uint32_t);
constexpr uint32_t kHistogramLayerCount = 4;
constexpr uint32_t kCombinedHistogramBytes = kHistogramLayerCount * kHistogramBytes;
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
  const uint32_t target = std::max(1u, static_cast<uint32_t>(std::ceil(f * total)));
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
  if (bins == nullptr) return;
  uint32_t total = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) total += bins[i];
  if (total == 0) return;

  const float low = params.use_percentile_range ? params.percentile_low : 0.0f;
  const float high = params.use_percentile_range ? params.percentile_high : 1.0f;
  const float reference = HistogramBandLuma(bins, total, low, high);
  const float raw_target = std::clamp(
      params.midtone / std::max(reference, 1e-6f), 0.01f, 4096.0f);

  const bool was_ready = have_measurement;
  const float dt = std::clamp(delta_seconds, 1.0f / 240.0f, 0.5f);
  float target = raw_target;
  if (was_ready) {
    const float current_log = std::log2(std::max(exposure, 1e-6f));
    float delta = std::log2(raw_target) - current_log;
    if (delta >= 0.0f)
      delta = std::min(delta, std::max(params.speed_up, 0.0f) * dt);
    else
      delta = std::max(delta, -std::max(params.speed_down, 0.0f) * dt);
    target = std::exp2(current_log + delta);
  }

  target_exposure = target;
  exposure = target;
  exposure_reference_luma = std::max(reference, 1e-6f);
  const float white_fraction = params.use_percentile_range
      ? std::clamp(params.percentile_high, 0.01f, 0.999f) : 0.99f;
  const float white_luma = BinToLinear(PercentileBin(bins, total, white_fraction));
  const float white_alpha = was_ready ? 1.0f - std::exp(-6.0f * dt) : 1.0f;
  scene_white += (std::max(white_luma, 1e-6f) - scene_white) * white_alpha;
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

  D3D12_RESOURCE_DESC exposure_desc = {};
  exposure_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  exposure_desc.Width = sizeof(uint32_t);
  exposure_desc.Height = 1;
  exposure_desc.DepthOrArraySize = 1;
  exposure_desc.MipLevels = 1;
  exposure_desc.Format = DXGI_FORMAT_UNKNOWN;
  exposure_desc.SampleDesc.Count = 1;
  exposure_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  exposure_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &exposure_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(game_exposure_value_buffer_.GetAddressOf())))) {
    Shutdown();
    return false;
  }

  if (!readback_.Ensure(device, kCombinedHistogramBytes) ||
      !gain_readback_.Ensure(device, kGainStatsBytes) ||
      !game_exposure_readback_.Ensure(device, sizeof(uint32_t))) {
    Shutdown();
    return false;
  }

  device_ = device;
  return true;
}

void HdrState::Shutdown() {
  readback_.Shutdown();
  gain_readback_.Shutdown();
  game_exposure_readback_.Shutdown();
  game_exposure_value_buffer_.Reset();
  histogram_.Reset();
  fence_.Reset();
  fence_value_ = 0;
  pending_fence_value_ = 0;
  pending_measurement_ = false;
  pending_gain_fence_value_ = 0;
  pending_gain_stats_ = false;
  pending_game_exposure_fence_value_ = 0;
  pending_game_exposure_ = false;
  game_exposure_value_ = 1.0f;
  game_exposure_available_ = false;
  game_exposure_width_ = 0;
  game_exposure_height_ = 0;
  game_exposure_format_ = DXGI_FORMAT_UNKNOWN;
  device_.Reset();

  enabled_ = false;
  regular_.Reset();
  adaptive_.Reset();
  game_tuning_.Reset();
  frame_counter_ = 0;
  frames_until_measure_ = 0;
  frames_since_measure_ = 0;
  measurement_generation_ = 0;
  have_logged_format_ = false;
  have_histogram_ = false;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    last_histogram_[i] = 0;
    last_full_histogram_[i] = 0;
    last_game_histogram_[i] = 0;
    last_local_histogram_[i] = 0;
  }
  p_hi_luma_ = 0.0f;
  local_luma_min_ = 0.0f;
  local_luma_max_ = 1.0e8f;
  last_samples_ = 0;
  last_full_samples_ = 0;
  last_game_samples_ = 0;
  last_local_samples_ = 0;
  have_measurement_time_ = false;
  last_measurement_time_ = {};
  have_gain_stats_ = false;
  gain_min_log2_ = 0.0f;
  gain_max_log2_ = 0.0f;
  gain_sdr_excursion_pixels_ = 0;
  gain_out_of_range_pixels_ = 0;
}

HdrSnapshot HdrState::GetSnapshot() const {
  HdrSnapshot snapshot;
  snapshot.enabled = enabled_;
  snapshot.ready = regular_.have_measurement;
  snapshot.resolved_format = resolved_format_;
  snapshot.exposure = regular_.exposure;
  snapshot.scene_white = regular_.scene_white;
  snapshot.frames_since_measure = frames_since_measure_;
  snapshot.has_histogram = have_histogram_;
  if (have_histogram_) {
    for (uint32_t i = 0; i < kHistogramBins; ++i)
      snapshot.histogram[i] = last_histogram_[i];
  }
  snapshot.exposure_reference_luma = regular_.exposure_reference_luma;
  snapshot.p_hi_luma = p_hi_luma_;
  snapshot.target_exposure = regular_.target_exposure;
  snapshot.samples = last_samples_;
  snapshot.local_luma_min = local_luma_min_;
  snapshot.local_luma_max = local_luma_max_;
  snapshot.measurement_generation = measurement_generation_;
  snapshot.game_exposure_available = game_exposure_available_;
  snapshot.game_exposure = game_exposure_value_;
  snapshot.game_exposure_width = game_exposure_width_;
  snapshot.game_exposure_height = game_exposure_height_;
  snapshot.game_exposure_format = static_cast<int>(game_exposure_format_);
  snapshot.metering_source = static_cast<int>(regular_.metering_source);
  snapshot.gain_map_ready = have_gain_stats_;
  snapshot.gain_min_log2 = gain_min_log2_;
  snapshot.gain_max_log2 = gain_max_log2_;
  snapshot.gain_sdr_excursion_pixels = gain_sdr_excursion_pixels_;
  snapshot.gain_out_of_range_pixels = gain_out_of_range_pixels_;
  return snapshot;
}

void HdrState::BeginFrame(CommandContext& cmd,
                          ID3D12GraphicsCommandList* command_list,
                          ID3D12Resource* source,
                          unsigned int width,
                          unsigned int height,
                          const HdrSettings& hdr_settings,
                          ComputeEngine& codec,
                          ID3D12Resource* exposure_mask,
                          ID3D12Resource* game_exposure_texture,
                          ID3D12Resource* depth_texture,
                          bool depth_inverted,
                          unsigned int exposure_mask_width,
                          unsigned int exposure_mask_height) {
  enabled_ = hdr_settings.enable;
  if (!enabled_ || command_list == nullptr || source == nullptr || width == 0 || height == 0)
    return;

  ++frames_since_measure_;
  PollReadback(fence_.Get(), hdr_settings);

  const HdrFormat old_format = resolved_format_;
  resolved_format_ = DetectFormatFromResource(source, hdr_settings.format);
  if (resolved_format_ != old_format || !have_logged_format_) {
    char message[160];
    std::snprintf(message, sizeof(message),
                  "HDR: DLSS output format=%d detected=%d (0=SDR,1=scRGB,2=HDR10)",
                  static_cast<int>(source->GetDesc().Format),
                  HdrFormatCode(resolved_format_));
    reshade::log::message(reshade::log::level::info, message);
    have_logged_format_ = true;
  }

  if (hdr_settings.game_exposure.enabled && !pending_game_exposure_ &&
      game_exposure_texture != nullptr && game_exposure_value_buffer_ != nullptr &&
      codec.IsReady() && codec.IsPassCompiled("ReadGameExposure")) {
    const D3D12_RESOURCE_DESC desc = game_exposure_texture->GetDesc();
    game_exposure_width_ = static_cast<uint32_t>(desc.Width);
    game_exposure_height_ = desc.Height;
    game_exposure_format_ = desc.Format;
    if (desc.Width == 1 && desc.Height == 1) {
      ComputeConstants constants = {};
      constants.Width = 1;
      constants.Height = 1;
      ID3D12Resource* srvs[kSrvCount] = {
          game_exposure_texture, game_exposure_texture,
          game_exposure_texture, game_exposure_texture};
      ID3D12Resource* uavs[kUavCount] = {game_exposure_value_buffer_.Get()};
      cmd.SetResourceState(game_exposure_texture,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      cmd.SetResourceState(game_exposure_value_buffer_.Get(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      cmd.Transition(command_list, game_exposure_texture,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      cmd.Transition(command_list, game_exposure_value_buffer_.Get(),
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (codec.Dispatch("ReadGameExposure", command_list, constants, srvs, uavs,
                         1, 1, 1)) {
        cmd.UavBarrier(command_list, game_exposure_value_buffer_.Get());
        cmd.Transition(command_list, game_exposure_value_buffer_.Get(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        const uint64_t next = fence_value_ + 1u;
        if (game_exposure_readback_.EnqueueCopy(
                command_list, game_exposure_value_buffer_.Get(), sizeof(uint32_t),
                fence_.Get(), next)) {
          pending_game_exposure_fence_value_ = next;
          pending_game_exposure_ = true;
        }
        cmd.Transition(command_list, game_exposure_value_buffer_.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
    }
  }

  if (pending_measurement_) {
    ++frame_counter_;
    return;
  }
  if (frames_until_measure_ > 0) {
    --frames_until_measure_;
    ++frame_counter_;
    return;
  }
  frames_until_measure_ = static_cast<uint32_t>(
      std::clamp(hdr_settings.measure_interval, 1, 32)) - 1u;
  if (!codec.IsReady()) {
    ++frame_counter_;
    return;
  }

  const bool have_mask = exposure_mask != nullptr &&
                         exposure_mask_width != 0 && exposure_mask_height != 0;
  const bool have_depth = depth_texture != nullptr;
  ID3D12Resource* mask_srv = have_mask ? exposure_mask : source;
  ID3D12Resource* depth_srv = have_depth ? depth_texture : source;
  ID3D12Resource* srvs[kSrvCount] = {source, mask_srv, depth_srv, source};
  ID3D12Resource* uavs[kUavCount] = {histogram_.Get()};
  cmd.SetResourceState(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.SetResourceState(histogram_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd.Transition(command_list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  cmd.Transition(command_list, histogram_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (have_mask) {
    cmd.SetResourceState(exposure_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd.Transition(command_list, exposure_mask,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (have_depth) {
    cmd.SetResourceState(depth_texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd.Transition(command_list, depth_texture,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.shader.Params0.x = FormatToShaderValue(resolved_format_);
  constants.shader.Params0.y = kLumaMinLog2;
  constants.shader.Params0.z = kLumaMaxLog2;
  constants.shader.Params0.w = 0.0f;
  const ExposureFilterSettings* filters[kHistogramLayerCount] = {
      nullptr, &hdr_settings.game_exposure.filters,
      &hdr_settings.adaptive_exposure.filters,
      &hdr_settings.local_exposure.filters};

  for (uint32_t layer = 0; layer < kHistogramLayerCount; ++layer) {
    constants.shader.Params0.w = 0.0f;
    constants.shader.Params1.x = 0.0f;
    constants.shader.Params1.y = 1.0f;
    constants.shader.Params1.z = 0.0f;
    constants.shader.Params1.w = static_cast<float>(layer * kHistogramBins);
    if (!codec.Dispatch("ZeroHistogram", command_list, constants, srvs, uavs,
                        1, 1, 1)) {
      ++frame_counter_;
      return;
    }
    if (layer + 1 < kHistogramLayerCount)
      cmd.UavBarrier(command_list, histogram_.Get());
  }

  for (uint32_t layer = 0; layer < kHistogramLayerCount; ++layer) {
    const ExposureFilterSettings* filter = filters[layer];
    uint32_t flags = 0;
    if (filter != nullptr && filter->importance_mask && have_mask) flags |= 1u;
    if (filter != nullptr && filter->depth && have_depth) flags |= 2u;
    constants.shader.Params0.w = static_cast<float>(flags);
    constants.shader.Params1.x = filter != nullptr ? filter->depth_near : 0.0f;
    constants.shader.Params1.y = filter != nullptr ? filter->depth_far : 1.0f;
    constants.shader.Params1.z = depth_inverted ? 1.0f : 0.0f;
    constants.shader.Params1.w = static_cast<float>(layer * kHistogramBins);
    if (!codec.Dispatch("BuildHistogram", command_list, constants, srvs, uavs)) {
      ++frame_counter_;
      return;
    }
    if (layer + 1 < kHistogramLayerCount)
      cmd.UavBarrier(command_list, histogram_.Get());
  }

  const uint64_t next = fence_value_ + 1u;
  cmd.UavBarrier(command_list, histogram_.Get());
  cmd.Transition(command_list, histogram_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (readback_.EnqueueCopy(command_list, histogram_.Get(), kCombinedHistogramBytes,
                            fence_.Get(), next)) {
    pending_fence_value_ = next;
    pending_measurement_ = true;
  }
  cmd.Transition(command_list, histogram_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ++frame_counter_;
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
      (!pending_measurement_ && !pending_gain_stats_ && !pending_game_exposure_))
    return;
  uint64_t signal_value = fence_value_;
  if (pending_measurement_)
    signal_value = std::max(signal_value, pending_fence_value_);
  if (pending_gain_stats_)
    signal_value = std::max(signal_value, pending_gain_fence_value_);
  if (pending_game_exposure_)
    signal_value = std::max(signal_value, pending_game_exposure_fence_value_);
  queue->Signal(fence_.Get(), signal_value);
  fence_value_ = signal_value;
}

void HdrState::PollReadback(ID3D12Fence* fence, const HdrSettings& settings) {
  if (fence == nullptr) return;
  const uint64_t completed = fence->GetCompletedValue();

  if (pending_game_exposure_ && completed >= pending_game_exposure_fence_value_) {
    uint32_t bytes = 0;
    const void* mapped = game_exposure_readback_.TryGet(fence, &bytes);
    pending_game_exposure_ = false;
    if (mapped != nullptr && bytes >= sizeof(uint32_t)) {
      const float value = std::bit_cast<float>(
          *static_cast<const uint32_t*>(mapped));
      if (std::isfinite(value) && value > 1e-5f && value < 4096.0f) {
        game_exposure_value_ = value;
        game_exposure_available_ = true;
      }
    }
    ResolveExposureStack(settings);
  }

  if (pending_measurement_ && completed >= pending_fence_value_) {
    uint32_t bytes = 0;
    const void* mapped = readback_.TryGet(fence, &bytes);
    pending_measurement_ = false;
    if (mapped != nullptr && bytes >= kCombinedHistogramBytes) {
      const uint32_t* bins = static_cast<const uint32_t*>(mapped);
      UpdateExposure(bins, bins + kHistogramBins,
                     bins + 2 * kHistogramBins,
                     bins + 3 * kHistogramBins, settings);
      ++measurement_generation_;
      frames_since_measure_ = 0;
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
            ? (sortable ^ 0x80000000u) : ~sortable;
      };
      gain_min_log2_ = std::bit_cast<float>(unsort(min_sortable));
      gain_max_log2_ = std::bit_cast<float>(unsort(max_sortable));
      gain_sdr_excursion_pixels_ = stats[2];
      gain_out_of_range_pixels_ = stats[3];
      if (std::isfinite(gain_min_log2_) && std::isfinite(gain_max_log2_))
        have_gain_stats_ = true;
    }
  }
}

void HdrState::UpdateExposure(const uint32_t* full_bins,
                              const uint32_t* game_bins,
                              const uint32_t* adaptive_bins,
                              const uint32_t* local_bins,
                              const HdrSettings& settings) {
  const auto now = std::chrono::steady_clock::now();
  const float dt = have_measurement_time_
      ? std::chrono::duration<float>(now - last_measurement_time_).count()
      : 1.0f / 60.0f;
  last_measurement_time_ = now;
  have_measurement_time_ = true;

  auto total_of = [](const uint32_t* source) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < kHistogramBins; ++i) total += source[i];
    return total;
  };
  const uint32_t full_total = total_of(full_bins);
  const uint32_t game_total = total_of(game_bins);
  const uint32_t adaptive_total = total_of(adaptive_bins);
  const uint32_t local_total = total_of(local_bins);
  last_full_samples_ = full_total;
  last_game_samples_ = game_total;
  last_local_samples_ = local_total;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    last_full_histogram_[i] = full_bins[i];
    last_game_histogram_[i] = game_bins[i];
    last_local_histogram_[i] = local_bins[i];
  }

  const auto& adaptive_settings = settings.adaptive_exposure;
  ExposureParams adaptive_params;
  adaptive_params.midtone = adaptive_settings.midtone;
  adaptive_params.percentile_low = adaptive_settings.filters.percentile_low;
  adaptive_params.percentile_high = adaptive_settings.filters.percentile_high;
  adaptive_params.use_percentile_range = adaptive_settings.filters.percentile_range;
  adaptive_params.speed_up = adaptive_settings.speed_up;
  adaptive_params.speed_down = adaptive_settings.speed_down;
  if (adaptive_total != 0) {
    adaptive_.Update(adaptive_bins, adaptive_params, dt);
    const bool adaptive_uses_filters =
        adaptive_settings.filters.importance_mask || adaptive_settings.filters.depth ||
        adaptive_settings.filters.percentile_range;
    adaptive_.metering_source = adaptive_uses_filters
        ? MeteringSource::FilteredPopulation : MeteringSource::FullFrame;
  }

  const auto& game_settings = settings.game_exposure;
  if (game_settings.bounded_tuning) {
    ExposureParams game_params;
    game_params.midtone = game_settings.tuning_midtone;
    game_params.percentile_low = game_settings.filters.percentile_low;
    game_params.percentile_high = game_settings.filters.percentile_high;
    game_params.use_percentile_range = game_settings.filters.percentile_range;
    game_params.speed_up = game_settings.tuning_speed_up;
    game_params.speed_down = game_settings.tuning_speed_down;
    if (game_total != 0)
      game_tuning_.Update(game_bins, game_params, dt);
  }

  local_luma_min_ = 0.0f;
  local_luma_max_ = 1.0e8f;
  if (settings.local_exposure.filters.percentile_range && local_total != 0) {
    const auto& filters = settings.local_exposure.filters;
    local_luma_min_ = BinToLinear(PercentileBin(
        local_bins, local_total, filters.percentile_low));
    local_luma_max_ = BinToLinear(PercentileBin(
        local_bins, local_total, filters.percentile_high));
    if (local_luma_max_ < local_luma_min_)
      std::swap(local_luma_min_, local_luma_max_);
  }

  const uint32_t chosen_total = adaptive_total != 0 ? adaptive_total : full_total;
  const uint32_t* chosen = adaptive_total != 0 ? adaptive_bins : full_bins;
  for (uint32_t i = 0; i < kHistogramBins; ++i) last_histogram_[i] = chosen[i];
  last_samples_ = chosen_total;
  have_histogram_ = chosen_total != 0;
  p_hi_luma_ = adaptive_.scene_white;
  ResolveExposureStack(settings);
}

void HdrState::ResolveExposureStack(const HdrSettings& settings) {
  const bool use_game = settings.game_exposure.enabled && game_exposure_available_;
  const bool use_adaptive = settings.adaptive_exposure.enabled && adaptive_.have_measurement;
  float exposure = 1.0f;
  float scene_white = 1.0f;
  MeteringSource source = MeteringSource::FullFrame;

  if (use_game) {
    exposure = game_exposure_value_ * std::exp2(settings.game_exposure.bias_stops);
    if (settings.game_exposure.bounded_tuning && game_tuning_.have_measurement) {
      const float delta = std::log2(std::max(game_tuning_.exposure, 1e-6f) /
                                    std::max(exposure, 1e-6f));
      const float correction = std::clamp(
          delta * settings.game_exposure.tuning_strength,
          -settings.game_exposure.max_tuning_stops,
          settings.game_exposure.max_tuning_stops);
      exposure *= std::exp2(correction);
    }
    if (use_adaptive) {
      const float delta = std::log2(std::max(adaptive_.exposure, 1e-6f) /
                                    std::max(exposure, 1e-6f));
      const float correction = std::clamp(
          delta * settings.adaptive_exposure.strength_over_game,
          -settings.adaptive_exposure.max_correction_stops,
          settings.adaptive_exposure.max_correction_stops);
      exposure *= std::exp2(correction);
    }
    scene_white = 1.0f;
    source = MeteringSource::GameExposureBuffer;
  } else if (use_adaptive) {
    exposure = adaptive_.exposure;
    scene_white = adaptive_.scene_white;
    source = adaptive_.metering_source;
  }

  regular_.exposure = std::clamp(exposure, 1e-5f, 4096.0f);
  regular_.target_exposure = regular_.exposure;
  regular_.scene_white = std::max(scene_white, 1e-6f);
  regular_.exposure_reference_luma = adaptive_.exposure_reference_luma;
  regular_.have_measurement = use_game || use_adaptive;
  regular_.metering_source = source;
  regular_.warmup_frames = std::max(regular_.warmup_frames, adaptive_.warmup_frames);
}

namespace {

PipelineStatus RunHdrBeginFrame(PassRun& run) {
  if (!run.is_first_pass) return PipelineStatus::Ok;
  if (run.hdr == nullptr || run.hdr_cmd == nullptr)
    return PipelineStatus::Ok;

  const bool pre_dlss = run.stage != nullptr && run.stage->is_pre_dlss;
  ID3D12Resource* source = pre_dlss ? run.output_resource
                                    : run.guides.dlss_output_color;
  if (source == nullptr) {
    TRACE_EVT(run.session, "hdr_begin_frame SKIP (no source color)");
    return PipelineStatus::Ok;
  }
  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  const unsigned int source_width = static_cast<unsigned int>(source_desc.Width);
  const unsigned int source_height = source_desc.Height;
  if (source_width == 0 || source_height == 0) {
    TRACE_EVT(run.session, "hdr_begin_frame SKIP (empty source color)");
    return PipelineStatus::Ok;
  }

  ID3D12Resource* exposure_mask = nullptr;
  unsigned int exposure_mask_width = 0;
  unsigned int exposure_mask_height = 0;
  if (run.mask_state.enable && run.importance_mask != nullptr) {
    const ImportanceMask::Output detection = run.importance_mask->GetFrameOutput();
    if (detection.valid && detection.mask != nullptr && detection.width != 0 &&
        detection.height != 0) {
      exposure_mask = detection.mask;
      exposure_mask_width = detection.width;
      exposure_mask_height = detection.height;
    }
  }

  TRACE_EVT(run.session, "hdr_begin_frame stage=%s %ux%u mask=%d",
            run.stage != nullptr ? run.stage->name : "?", source_width,
            source_height, exposure_mask != nullptr ? 1 : 0);
  run.hdr->BeginFrame(*run.hdr_cmd, run.list, source, source_width,
                      source_height, run.hdr_state, *run.codec, exposure_mask,
                      run.game_exposure_texture, run.guides.depth,
                      run.guides.depth_inverted, exposure_mask_width,
                      exposure_mask_height);
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
                  kHdrPreparationStepNodes,
                  ArrayCount(kHdrPreparationStepNodes), kHdrSteps,
                  ArrayCount(kHdrSteps)),
              "an HDR step in HdrPreparation declares a prerequisite "
              "that the stage lists after it");

static_assert(
    StageInputsCoverSteps(kPreDlssStepNodes, ArrayCount(kPreDlssStepNodes),
                          kHdrSteps, ArrayCount(kHdrSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "an HDR step in PreDLSS needs an input the stage call site does not supply");
static_assert(
    StageInputsCoverSteps(kHdrPreparationStepNodes,
                          ArrayCount(kHdrPreparationStepNodes), kHdrSteps,
                          ArrayCount(kHdrSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "an HDR step in HdrPreparation needs an input the stage call site "
    "does not supply");

}  // namespace

const PipelineStepDef* HdrStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kHdrSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr
