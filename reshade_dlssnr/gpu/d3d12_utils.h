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

inline DXGI_FORMAT TypedViewFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
      return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
      return DXGI_FORMAT_R16G16_FLOAT;
    default:
      return format;
  }
}

// Short stable name for a DXGI format, for log lines. Only the formats this
// addon can plausibly meet are named; anything else prints as a number so an
// unexpected format is still identifiable rather than silently blank.
inline const char* FormatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_UNKNOWN:
      return "UNKNOWN";
    case DXGI_FORMAT_R8_UNORM:
      return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM:
      return "R8G8_UNORM";
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
    case DXGI_FORMAT_R32_FLOAT:
      return "R32_FLOAT";
    case DXGI_FORMAT_R32_UINT:
      return "R32_UINT";
    case DXGI_FORMAT_R32G32_FLOAT:
      return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return "R32G32B32A32_FLOAT";
    default:
      return "other";
  }
}

}  // namespace reshade_dlssnr
