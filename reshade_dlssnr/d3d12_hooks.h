#pragma once

#include <d3d12.h>
#include <windows.h>

#include <mutex>

namespace reshade_dlssnr {

class D3D12Hooks {
 public:
  static D3D12Hooks& Get();

  void HookCommandList(ID3D12GraphicsCommandList* command_list);
  void UnhookCommandList(ID3D12GraphicsCommandList* command_list);
  void RestoreCommandList(ID3D12GraphicsCommandList* command_list);

 private:
  D3D12Hooks() = default;
  ~D3D12Hooks() = default;

  D3D12Hooks(const D3D12Hooks&) = delete;
  D3D12Hooks& operator=(const D3D12Hooks&) = delete;

  std::mutex mutex_;
};

}  // namespace reshade_dlssnr
