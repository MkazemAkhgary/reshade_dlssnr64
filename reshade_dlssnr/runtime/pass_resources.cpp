#include "pass_resources.h"

namespace reshade_dlssnr {

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

  output.Reset();

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, desc, &raw)) {
    return false;
  }
  output.Attach(raw);

  output_width = static_cast<unsigned int>(desc.Width);
  output_height = desc.Height;
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

  composite_output.Reset();

  ID3D12Resource* raw = nullptr;
  if (!CreateUavTexture(device, desc, &raw)) {
    return false;
  }
  composite_output.Attach(raw);

  composite_output_width = static_cast<unsigned int>(desc.Width);
  composite_output_height = desc.Height;
  composite_output_format = desc.Format;
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

  working_input.Reset();
  working_output.Reset();

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
    working_input.Reset();
    return false;
  }
  working_output.Attach(raw_output);

  working_width = work_width;
  working_height = work_height;
  working_format = desc.Format;
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

  scratch_[index].Reset();

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
  return true;
}

bool PassResources::EnsureWhitePointBuffer(ID3D12Device* device) {
  if (white_point_buffer != nullptr) return true;
  if (device == nullptr) return false;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = kWhitePointGridSize;
  desc.Height = kWhitePointGridSize;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  return SUCCEEDED(device->CreateCommittedResource(
      &heap,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      nullptr,
      IID_PPV_ARGS(white_point_buffer.GetAddressOf())));
}

void PassResources::Shutdown() {
  output.Reset();
  composite_output.Reset();
  working_input.Reset();
  working_output.Reset();
  white_point_buffer.Reset();

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
  working_width = 0;
  working_height = 0;
  working_format = DXGI_FORMAT_UNKNOWN;
}

}  // namespace reshade_dlssnr
