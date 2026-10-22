// Shared D3D12 resource-state inference helper.
#pragma once

#include <d3d12.h>
#include <cstdint>

namespace reshade_dlssnr {

inline D3D12_RESOURCE_STATES InferResourceState(ID3D12Resource* resource) {
  if (resource == nullptr) return D3D12_RESOURCE_STATE_COMMON;

  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  switch (desc.Dimension) {
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return D3D12_RESOURCE_STATE_GENERIC_READ;
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
      if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
      } else if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
      } else if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      }
      return D3D12_RESOURCE_STATE_COMMON;
    default:
      return D3D12_RESOURCE_STATE_COMMON;
  }
}

// Short stable name for a DXGI format, for log lines. Only the formats this
// addon can plausibly meet are named; anything else prints as a number so an
// unexpected format is still identifiable rather than silently blank.
inline const char* FormatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_UNKNOWN:
      return "UNKNOWN";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return "R8G8B8A8_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return "B8G8R8A8_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT:
      return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM:
      return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return "R32G32B32A32_FLOAT";
    default:
      return "other";
  }
}

// Creates a default-heap Texture2D with D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
// and initial state UNORDERED_ACCESS. The descriptor's existing flags are kept;
// the UAV flag is added. Returns false on any failure.
inline bool CreateUavTexture(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& desc,
    ID3D12Resource** out_resource) {
  if (device == nullptr || out_resource == nullptr) return false;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC resource_desc = desc;
  resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
      IID_PPV_ARGS(out_resource)));
}

// Creates a default-heap UAV buffer of `byte_width` bytes, typed R32_UINT.
// Returns false on any failure or a zero width.
inline bool CreateUavBuffer(
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

}  // namespace reshade_dlssnr
