#include "pass_resources.h"

#include <cstdio>

#include <reshade.hpp>

#include "pipeline_trace.h"

namespace reshade_dlssnr {

void PassResources::Dispose(Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
  if (resource == nullptr) return;

  if (retire_cb_ != nullptr) {
    // Transfer the reference; the callback releases it later.
    retire_cb_(resource.Detach(), retire_user_data_);
  } else {
    // No retire callback means we may be about to release a resource the GPU
    // is still using. Whether that actually faults depends on timing — if the
    // resource was bound by in-flight GPU work, releasing it will hang the
    // device. This path should never run; if it does, log it so the cause is
    // visible rather than manifesting as an unexplained hang.
    reshade::log::message(
        reshade::log::level::error,
        "PassResources::Dispose called without a retire callback; "
        "releasing synchronously (unsafe)");
    resource.Reset();
  }
}

bool PassResources::CreateUavTexture(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    ID3D12Resource** out_resource) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC resource_desc = desc;
  resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  return SUCCEEDED(device->CreateCommittedResource(
      &heap,
      D3D12_HEAP_FLAG_NONE,
      &resource_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr,
      IID_PPV_ARGS(out_resource)));
}

bool PassResources::EnsureOutput(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc) {
  if (output != nullptr &&
      output_width == desc.Width &&
      output_height == desc.Height &&
      output->GetDesc().Format == desc.Format) {
    return true;
  }

  Dispose(output);

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, desc, &raw)) {
    return false;
  }
  output.Attach(raw);

  output_width = static_cast<unsigned int>(desc.Width);
  output_height = desc.Height;
  trace::NameResource(output.Get(), "output");
  TRACE_CMD(this, "create output res=%d %ux%u", trace::ResourceId(output.Get()),
            output_width, output_height);
  return true;
}

void PassResources::ReleaseOutput() {
  Dispose(output);
  output_width = 0;
  output_height = 0;
}

bool PassResources::EnsureCompositeOutput(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc) {
  if (composite_output != nullptr &&
      composite_output_width == desc.Width &&
      composite_output_height == desc.Height &&
      composite_output_format == desc.Format) {
    return true;
  }

  Dispose(composite_output);

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, desc, &raw)) {
    return false;
  }
  composite_output.Attach(raw);

  composite_output_width = static_cast<unsigned int>(desc.Width);
  composite_output_height = desc.Height;
  composite_output_format = desc.Format;
  trace::NameResource(composite_output.Get(), "composite_output");
  TRACE_CMD(this, "create composite_output res=%d %ux%u",
            trace::ResourceId(composite_output.Get()),
            composite_output_width, composite_output_height);
  return true;
}

bool PassResources::CreateUavBuffer(
    ID3D12Device* device,
    uint64_t byte_width,
    ID3D12Resource** out_resource) {
  if (device == nullptr || out_resource == nullptr || byte_width == 0) {
    return false;
  }

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = byte_width;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
      IID_PPV_ARGS(out_resource)));
}

bool PassResources::EnsurePackedOutput(
    ID3D12Device* device, const D3D12_RESOURCE_DESC& desc) {
  if (packed_hdr_sdr != nullptr &&
      packed_width == desc.Width &&
      packed_height == desc.Height &&
      packed_hdr_sdr->GetDesc().Format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
    return true;
  }

  Dispose(packed_hdr_sdr);

  D3D12_RESOURCE_DESC packed_desc = desc;
  packed_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  packed_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* raw = nullptr;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &packed_desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&raw)))) {
    return false;
  }
  packed_hdr_sdr.Attach(raw);
  packed_width = static_cast<unsigned int>(desc.Width);
  packed_height = desc.Height;
  trace::NameResource(packed_hdr_sdr.Get(), "packed_hdr_sdr");
  TRACE_CMD(this, "create packed_hdr_sdr res=%d %ux%u",
            trace::ResourceId(packed_hdr_sdr.Get()), packed_width, packed_height);
  return true;
}

bool PassResources::EnsureGainStats(ID3D12Device* device) {
  constexpr size_t kBytes = 4 * sizeof(uint32_t);
  if (gain_stats != nullptr && gain_stats_bytes == kBytes) return true;

  Dispose(gain_stats);
  ID3D12Resource* raw = nullptr;
  if (!CreateUavBuffer(device, kBytes, &raw)) return false;
  gain_stats.Attach(raw);
  gain_stats_bytes = kBytes;
  trace::NameResource(gain_stats.Get(), "gain_stats");
  TRACE_CMD(this, "create gain_stats res=%d bytes=%zu",
            trace::ResourceId(gain_stats.Get()), gain_stats_bytes);
  return true;
}

bool PassResources::EnsureEffectiveExposureMap(
    ID3D12Device* device, unsigned int width, unsigned int height) {
  if (device == nullptr || width == 0 || height == 0) return false;

  if (effective_exposure_map != nullptr &&
      effective_exposure_width == width &&
      effective_exposure_height == height &&
      effective_exposure_map->GetDesc().Format == DXGI_FORMAT_R32_FLOAT) {
    return true;
  }

  Dispose(effective_exposure_map);

  D3D12_RESOURCE_DESC map_desc = {};
  map_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  map_desc.Width = width;
  map_desc.Height = height;
  map_desc.DepthOrArraySize = 1;
  map_desc.MipLevels = 1;
  map_desc.Format = DXGI_FORMAT_R32_FLOAT;
  map_desc.SampleDesc.Count = 1;
  map_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  map_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, map_desc, &raw)) {
    return false;
  }
  effective_exposure_map.Attach(raw);
  effective_exposure_width = width;
  effective_exposure_height = height;
  trace::NameResource(effective_exposure_map.Get(), "effective_exposure_map");
  TRACE_CMD(this, "create effective_exposure_map res=%d %ux%u",
            trace::ResourceId(effective_exposure_map.Get()),
            width, height);
  return true;
}

void PassResources::ReleaseEffectiveExposureMap() {
  Dispose(effective_exposure_map);
  effective_exposure_width = 0;
  effective_exposure_height = 0;
}

bool PassResources::EnsureLocalExposureRestoreResources(
    ID3D12Device* device, unsigned int width, unsigned int height) {
  if (device == nullptr || width == 0 || height == 0) return false;

  const unsigned int partial_width = (width + 7u) / 8u;
  const unsigned int partial_height = (height + 7u) / 8u;
  const D3D12_RESOURCE_DESC existing_global_desc =
      local_exposure_global_scale != nullptr
          ? local_exposure_global_scale->GetDesc() : D3D12_RESOURCE_DESC{};
  const bool matches = local_exposure_partial_sums != nullptr &&
      local_exposure_global_scale != nullptr &&
      local_exposure_partial_width == partial_width &&
      local_exposure_partial_height == partial_height &&
      existing_global_desc.Width == 1 && existing_global_desc.Height == 1 &&
      existing_global_desc.Format == DXGI_FORMAT_R32_FLOAT;
  if (matches) return true;

  Dispose(local_exposure_partial_sums);
  Dispose(local_exposure_global_scale);

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = partial_width;
  desc.Height = partial_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  ID3D12Resource* partial_raw = nullptr;
  if (!CreateUavTexture(device, desc, &partial_raw)) return false;
  local_exposure_partial_sums.Attach(partial_raw);

  D3D12_RESOURCE_DESC global_desc = desc;
  global_desc.Width = 1;
  global_desc.Height = 1;
  ID3D12Resource* global_raw = nullptr;
  if (!CreateUavTexture(device, global_desc, &global_raw)) {
    Dispose(local_exposure_partial_sums);
    return false;
  }
  local_exposure_global_scale.Attach(global_raw);

  local_exposure_partial_width = partial_width;
  local_exposure_partial_height = partial_height;
  trace::NameResource(local_exposure_partial_sums.Get(),
                      "local_exposure_tile_log_scales");
  trace::NameResource(local_exposure_global_scale.Get(),
                      "local_exposure_global_scale");
  TRACE_CMD(this, "create local exposure restore res=%d/%d tiles=%ux%u",
            trace::ResourceId(local_exposure_partial_sums.Get()),
            trace::ResourceId(local_exposure_global_scale.Get()),
            partial_width, partial_height);
  return true;
}

bool PassResources::EnsureWorkingResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int work_width,
    unsigned int work_height,
    bool need_input) {
  const bool output_matches = working_output != nullptr &&
      working_width == work_width && working_height == work_height &&
      working_format == desc.Format;
  const bool input_matches = !need_input || working_input != nullptr;
  if (output_matches && input_matches) {
    if (!need_input && working_input != nullptr) {
      Dispose(working_input);
    }
    return true;
  }

  Dispose(working_input);
  Dispose(working_output);

  D3D12_RESOURCE_DESC working_desc = desc;
  working_desc.Width = work_width;
  working_desc.Height = work_height;

  if (need_input) {
    ID3D12Resource* raw_input = nullptr;
    if (!CreateUavTexture(device, working_desc, &raw_input)) {
      return false;
    }
    working_input.Attach(raw_input);
  }

  ID3D12Resource* raw_output = nullptr;
  if (!CreateUavTexture(device, working_desc, &raw_output)) {
    Dispose(working_input);
    return false;
  }
  working_output.Attach(raw_output);

  working_width = work_width;
  working_height = work_height;
  working_format = desc.Format;
  if (working_input != nullptr) {
    trace::NameResource(working_input.Get(), "working_input");
  }
  trace::NameResource(working_output.Get(), "working_output");
  TRACE_CMD(this, "create working res=%d/%d %ux%u input=%d",
            working_input != nullptr ? trace::ResourceId(working_input.Get()) : -1,
            trace::ResourceId(working_output.Get()), working_width,
            working_height, working_input != nullptr ? 1 : 0);
  return true;
}

bool PassResources::EnsureUnliftedWorkingInput(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int work_width,
    unsigned int work_height) {
  if (device == nullptr || work_width == 0 || work_height == 0) return false;

  if (unlifted_working_input != nullptr &&
      unlifted_working_width == work_width &&
      unlifted_working_height == work_height &&
      unlifted_working_format == desc.Format) {
    return true;
  }

  Dispose(unlifted_working_input);

  D3D12_RESOURCE_DESC working_desc = desc;
  working_desc.Width = work_width;
  working_desc.Height = work_height;

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, working_desc, &raw)) return false;
  unlifted_working_input.Attach(raw);
  unlifted_working_width = work_width;
  unlifted_working_height = work_height;
  unlifted_working_format = desc.Format;
  trace::NameResource(unlifted_working_input.Get(), "unlifted_working_input");
  TRACE_CMD(this, "create unlifted_working_input res=%d %ux%u",
            trace::ResourceId(unlifted_working_input.Get()),
            work_width, work_height);
  return true;
}

bool PassResources::EnsureTemporalResources(
    ID3D12Device* device, const D3D12_RESOURCE_DESC& input_desc,
    unsigned int width, unsigned int height, bool use_fp16_history,
    bool* out_reset_history) {
  if (out_reset_history != nullptr) *out_reset_history = false;
  if (device == nullptr || width == 0 || height == 0) return false;

  const DXGI_FORMAT requested_edit_format = use_fp16_history
      ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
  const bool matches = temporal_previous_input != nullptr &&
      temporal_previous_edit != nullptr && temporal_previous_depth != nullptr &&
      temporal_candidate != nullptr && temporal_validated_candidate != nullptr &&
      temporal_stats != nullptr && temporal_width == width &&
      temporal_height == height && temporal_input_format == input_desc.Format &&
      temporal_edit_format == requested_edit_format &&
      temporal_previous_input->GetDesc().Width == input_desc.Width &&
      temporal_previous_input->GetDesc().Height == input_desc.Height;
  if (matches) return true;

  Dispose(temporal_previous_input);
  Dispose(temporal_previous_edit);
  Dispose(temporal_previous_depth);
  Dispose(temporal_candidate);
  Dispose(temporal_validated_candidate);
  Dispose(temporal_stats);

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  // This resource is only ever copied to and sampled; deliberately omit UAV
  // flags so sRGB game formats remain legal.
  D3D12_RESOURCE_DESC input_copy_desc = {};
  input_copy_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  input_copy_desc.Width = input_desc.Width;
  input_copy_desc.Height = input_desc.Height;
  input_copy_desc.DepthOrArraySize = 1;
  input_copy_desc.MipLevels = 1;
  input_copy_desc.Format = input_desc.Format;
  input_copy_desc.SampleDesc.Count = 1;
  input_copy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  ID3D12Resource* input_copy = nullptr;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &input_copy_desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&input_copy)))) {
    return false;
  }
  temporal_previous_input.Attach(input_copy);

  D3D12_RESOURCE_DESC edit_desc = {};
  edit_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  edit_desc.Width = width;
  edit_desc.Height = height;
  edit_desc.DepthOrArraySize = 1;
  edit_desc.MipLevels = 1;
  edit_desc.Format = requested_edit_format;
  edit_desc.SampleDesc.Count = 1;
  edit_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  if (!CreateUavTexture(device, edit_desc, temporal_previous_edit.GetAddressOf()) ||
      !CreateUavTexture(device, edit_desc, temporal_candidate.GetAddressOf()) ||
      !CreateUavTexture(device, edit_desc, temporal_validated_candidate.GetAddressOf())) {
    Dispose(temporal_previous_input);
    Dispose(temporal_previous_edit);
    Dispose(temporal_candidate);
    Dispose(temporal_validated_candidate);
    return false;
  }

  D3D12_RESOURCE_DESC depth_desc = edit_desc;
  depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
  if (!CreateUavTexture(device, depth_desc, temporal_previous_depth.GetAddressOf())) {
    Dispose(temporal_previous_input);
    Dispose(temporal_previous_edit);
    Dispose(temporal_candidate);
    Dispose(temporal_validated_candidate);
    return false;
  }

  // Two uints: invalid-pixel count and a marker that distinguishes a real
  // reprojection sample from the deliberately empty first-history frame.
  if (!CreateUavBuffer(device, 2u * sizeof(uint32_t),
                       temporal_stats.GetAddressOf())) {
    Dispose(temporal_previous_input);
    Dispose(temporal_previous_edit);
    Dispose(temporal_previous_depth);
    Dispose(temporal_candidate);
    Dispose(temporal_validated_candidate);
    return false;
  }

  temporal_width = width;
  temporal_height = height;
  temporal_input_format = input_desc.Format;
  temporal_edit_format = requested_edit_format;
  trace::NameResource(temporal_previous_input.Get(), "temporal_previous_input");
  trace::NameResource(temporal_previous_edit.Get(), "temporal_previous_edit");
  trace::NameResource(temporal_previous_depth.Get(), "temporal_previous_depth");
  trace::NameResource(temporal_candidate.Get(), "temporal_candidate");
  trace::NameResource(temporal_validated_candidate.Get(), "temporal_validated_candidate");
  trace::NameResource(temporal_stats.Get(), "temporal_stats");
  if (out_reset_history != nullptr) *out_reset_history = true;
  return true;
}

bool PassResources::EnsureScratch(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int width,
    unsigned int height,
    uint32_t index) {
  if (index >= kScratchCount) return false;

  if (scratch_[index] != nullptr &&
      scratch_width_[index] == width &&
      scratch_height_[index] == height &&
      scratch_format_[index] == desc.Format) {
    return true;
  }

  Dispose(scratch_[index]);

  D3D12_RESOURCE_DESC scratch_desc = desc;
  scratch_desc.Width = width;
  scratch_desc.Height = height;

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, scratch_desc, &raw)) {
    return false;
  }
  scratch_[index].Attach(raw);

  scratch_width_[index] = width;
  scratch_height_[index] = height;
  scratch_format_[index] = desc.Format;
  {
    char name[32];
    std::snprintf(name, sizeof(name), "scratch[%u]", index);
    trace::NameResource(scratch_[index].Get(), name);
  }
  TRACE_CMD(this, "create scratch[%u] res=%d %ux%u", index,
            trace::ResourceId(scratch_[index].Get()), width, height);
  return true;
}

void PassResources::Shutdown() {
  // Shutdown is only called after the GPU has been synchronized (or the
  // device is gone), so direct release is safe here.
  retire_cb_ = nullptr;
  retire_user_data_ = nullptr;

  output.Reset();
  composite_output.Reset();
  packed_hdr_sdr.Reset();
  gain_stats.Reset();
  working_input.Reset();
  working_output.Reset();
  unlifted_working_input.Reset();
  effective_exposure_map.Reset();
  local_exposure_partial_sums.Reset();
  local_exposure_global_scale.Reset();
  temporal_previous_input.Reset();
  temporal_previous_edit.Reset();
  temporal_previous_depth.Reset();
  temporal_candidate.Reset();
  temporal_validated_candidate.Reset();
  temporal_stats.Reset();

  for (uint32_t i = 0; i < kScratchCount; ++i) {
    scratch_[i].Reset();
    scratch_width_[i] = 0;
    scratch_height_[i] = 0;
    scratch_format_[i] = DXGI_FORMAT_UNKNOWN;
  }

  output_width = 0;
  output_height = 0;
  composite_output_width = 0;
  composite_output_height = 0;
  composite_output_format = DXGI_FORMAT_UNKNOWN;
  packed_width = 0;
  packed_height = 0;
  gain_stats_bytes = 0;
  effective_exposure_width = 0;
  effective_exposure_height = 0;
  local_exposure_partial_width = 0;
  local_exposure_partial_height = 0;
  temporal_width = temporal_height = 0;
  temporal_input_format = DXGI_FORMAT_UNKNOWN;
  temporal_edit_format = DXGI_FORMAT_UNKNOWN;
  working_width = 0;
  working_height = 0;
  working_format = DXGI_FORMAT_UNKNOWN;
  unlifted_working_width = 0;
  unlifted_working_height = 0;
  unlifted_working_format = DXGI_FORMAT_UNKNOWN;
}

}  // namespace reshade_dlssnr
