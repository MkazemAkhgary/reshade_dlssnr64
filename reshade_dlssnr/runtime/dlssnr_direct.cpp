#include "dlssnr_direct.h"

#include <mutex>

#include "dlss_guides.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_direct_mutex;
}

DlssNrDirect& DlssNrDirect::Get() {
  static DlssNrDirect direct;
  return direct;
}

void DlssNrDirect::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (failed_ || command_queue == nullptr || swapchain == nullptr) return;

  pass1_.OnPresent(
      runtime_,
      settings.pass1,
      settings,
      command_queue,
      swapchain);

  if (settings.second_pass_enabled) {
    pass2_.OnPresent(
        runtime_,
        settings.pass2,
        settings,
        command_queue,
        swapchain);
  }
}

void DlssNrDirect::SetDebugSelectedResource(uint64_t handle) {
}

void DlssNrDirect::ClearSelectedPass() {
  pass1_.SetPendingSelectedPass(nullptr);
}

void DlssNrDirect::SetPendingSelectedPass(ID3D12Resource* resource) {
  pass1_.SetPendingSelectedPass(resource);
}

void DlssNrDirect::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  pass1_.Shutdown(runtime_);
  pass2_.Shutdown(runtime_);
  runtime_.Shutdown();

  failed_ = false;
  reason_ = "";
}

bool DlssNrDirect::IsReady() const {
  return pass1_.IsReady() && !failed_;
}

const char* DlssNrDirect::GetLastError() const {
  return failed_ ? reason_ : "";
}

}  // namespace reshade_dlssnr
