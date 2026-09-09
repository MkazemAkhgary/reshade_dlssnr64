// Shared D3D12 resource-state inference helper.
#pragma once

#include <d3d12.h>

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

}  // namespace reshade_dlssnr
