#include "dlssnr_pass_resources.h"

namespace reshade_dlssnr {

bool DlssNrPassResources::CreateUavTexture(
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

bool DlssNrPassResources::EnsureOutput(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc) {
  if (output != nullptr &&
      output_width == desc.Width &&
      output_height == desc.Height &&
      output->GetDesc().Format == desc.Format) {
    return true;
  }

  if (output != nullptr) {
    output->Release();
    output = nullptr;
  }

  if (!CreateUavTexture(device, desc, &output)) {
    return false;
  }

  output_width = static_cast<unsigned int>(desc.Width);
  output_height = desc.Height;
  return true;
}

bool DlssNrPassResources::EnsureUiMask(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc) {
  if (ui_mask != nullptr &&
      ui_mask_width == desc.Width &&
      ui_mask_height == desc.Height &&
      ui_mask_format == desc.Format) {
    return true;
  }

  if (ui_mask != nullptr) {
    ui_mask->Release();
    ui_mask = nullptr;
  }

  if (!CreateUavTexture(device, desc, &ui_mask)) {
    return false;
  }

  ui_mask_width = static_cast<unsigned int>(desc.Width);
  ui_mask_height = desc.Height;
  ui_mask_format = desc.Format;
  return true;
}

bool DlssNrPassResources::EnsureCompositeOutput(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc) {
  if (composite_output != nullptr &&
      composite_output_width == desc.Width &&
      composite_output_height == desc.Height &&
      composite_output_format == desc.Format) {
    return true;
  }

  if (composite_output != nullptr) {
    composite_output->Release();
    composite_output = nullptr;
  }

  if (!CreateUavTexture(device, desc, &composite_output)) {
    return false;
  }

  composite_output_width = static_cast<unsigned int>(desc.Width);
  composite_output_height = desc.Height;
  composite_output_format = desc.Format;
  return true;
}

bool DlssNrPassResources::EnsureWorkingResources(
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

  if (working_input != nullptr) {
    working_input->Release();
    working_input = nullptr;
  }

  if (working_output != nullptr) {
    working_output->Release();
    working_output = nullptr;
  }

  D3D12_RESOURCE_DESC working_desc = desc;
  working_desc.Width = work_width;
  working_desc.Height = work_height;

  if (!CreateUavTexture(device, working_desc, &working_input)) {
    return false;
  }

  if (!CreateUavTexture(device, working_desc, &working_output)) {
    working_input->Release();
    working_input = nullptr;
    return false;
  }

  working_width = work_width;
  working_height = work_height;
  working_format = desc.Format;
  return true;
}

void DlssNrPassResources::Shutdown() {
  if (output != nullptr) {
    output->Release();
    output = nullptr;
  }

  if (ui_mask != nullptr) {
    ui_mask->Release();
    ui_mask = nullptr;
  }

  if (composite_output != nullptr) {
    composite_output->Release();
    composite_output = nullptr;
  }

  if (working_input != nullptr) {
    working_input->Release();
    working_input = nullptr;
  }

  if (working_output != nullptr) {
    working_output->Release();
    working_output = nullptr;
  }

  output_width = 0;
  output_height = 0;
  ui_mask_width = 0;
  ui_mask_height = 0;
  ui_mask_format = DXGI_FORMAT_UNKNOWN;
  composite_output_width = 0;
  composite_output_height = 0;
  composite_output_format = DXGI_FORMAT_UNKNOWN;
  working_width = 0;
  working_height = 0;
  working_format = DXGI_FORMAT_UNKNOWN;
}

}  // namespace reshade_dlssnr
