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
  constexpr size_t kBytes = 3 * sizeof(uint32_t);
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

bool PassResources::EnsureWorkingResources(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    unsigned int work_width,
    unsigned int work_height) {
  if (working_input != nullptr &&
      working_width == work_width &&
      working_height == work_height &&
      working_format == desc.Format) {
    return true;
  }

  // These are the resources most likely to be replaced on every slider
  // drag, so they must go through the retire path rather than being
  // released immediately (the GPU may still be reading them).
  Dispose(working_input);
  Dispose(working_output);

  D3D12_RESOURCE_DESC working_desc = desc;
  working_desc.Width = work_width;
  working_desc.Height = work_height;

  ID3D12Resource* raw_input = nullptr;
  if (!CreateUavTexture(device, working_desc, &raw_input)) {
    return false;
  }
  working_input.Attach(raw_input);

  ID3D12Resource* raw_output = nullptr;
  if (!CreateUavTexture(device, working_desc, &raw_output)) {
    Dispose(working_input);
    return false;
  }
  working_output.Attach(raw_output);

  working_width = work_width;
  working_height = work_height;
  working_format = desc.Format;
  trace::NameResource(working_input.Get(), "working_input");
  trace::NameResource(working_output.Get(), "working_output");
  TRACE_CMD(this, "create working res=%d/%d %ux%u",
            trace::ResourceId(working_input.Get()),
            trace::ResourceId(working_output.Get()),
            working_width, working_height);
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
  working_width = 0;
  working_height = 0;
  working_format = DXGI_FORMAT_UNKNOWN;
}

}  // namespace reshade_dlssnr
