// D3D12 command-list state hooking used for early integration points.
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <mutex>

namespace reshade_dlssnr {

class D3D12Hooks {
 public:
  static D3D12Hooks& Get();

  // Installs the command-list and device vtable hooks. Idempotent; returns
  // true once both are in place.
  bool InstallEarlyHooks(ID3D12Device* device);

  // True once the command-list hooks are live. Nothing is tracked before
  // that, so there is nothing for RestoreCommandList to put back.
  bool Installed() const;

  void SetTrackingEnabled(bool enabled);
  void RestoreCommandList(ID3D12GraphicsCommandList* command_list);

 private:
  D3D12Hooks() = default;
  ~D3D12Hooks() = default;

  D3D12Hooks(const D3D12Hooks&) = delete;
  D3D12Hooks& operator=(const D3D12Hooks&) = delete;
};

}  // namespace reshade_dlssnr
