// NVIDIA Streamline interposer compatibility.
//
// Streamline's interposer replaces the D3D/DXGI entry points the game links
// against and hands the game *proxy* objects for ID3D12Device,
// ID3D12CommandQueue, ID3D12GraphicsCommandList and IDXGISwapChain. Whether
// it does so depends on the version and on the host's preferences:
//
//   1.0.0 - 1.1.1   Always proxies the device, the command queue and the
//                   command list. Command list state tracking is always on
//                   in 1.0.0 (Preferences carries no flags at all) and on by
//                   default in 1.1.x (ePreferenceFlagDisableCLStateTracking
//                   is opt-in). There is no manual hooking, no
//                   slGetNativeInterface and no sl_hooks.h.
//   2.0.0+          Proxies only what a feature needs, and skips the command
//                   list proxy entirely when the host sets
//                   PreferenceFlags::eDisableCLStateTracking -- which is part
//                   of the 2.x default flag set. Manual hooking
//                   (eUseManualHooking, slUpgradeInterface) is new here.
//
// So a game on 1.x gives us SL proxy command lists and an SL proxy device,
// and a game on 2.x usually does not. Everything below is written to work in
// both cases without asking which one it is.
//
// The 2.x programming guide, section 5.3, is explicit about what a third
// party is supposed to do about it:
//
//   "Third party libraries SHOULD NOT use SL proxies to avoid app
//    instability."
//
// The proxy is not opaque: QueryInterface with a fixed IID returns the
// interface it wraps. That IID is declared in sl.api/internal.h as
// StreamlineRetreiveBaseInterface (1.x) / StreamlineRetrieveBaseInterface
// (2.x) and is handled by every interposer proxy from v1.0.0 through
// v2.14.1, so one probe covers the whole range we care about.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include "profiling/profile.h"

namespace reshade_dlssnr::sl_compat {

// {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
// Returns the interface an SL proxy wraps. On anything that is not an SL
// proxy the query fails and the object is left alone.
inline const GUID& BaseInterfaceIid() {
  static constexpr GUID kIid = {
      0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
  return kIid;
}

// True when sl.interposer.dll is in the process. Cheap; not cached, because
// the DLL is loaded by the game at a point we do not control and callers use
// this before deciding whether a more expensive probe is worth it.
inline bool InterposerPresent() {
  return GetModuleHandleW(L"sl.interposer.dll") != nullptr;
}

// The native interface behind `iface`, or `iface` itself when it is not an SL
// proxy. `is_proxy` (optional) reports which happened; `false` also covers a
// null input.
//
// The returned pointer always owns a reference, so put it in a ComPtr. Going
// through the proxy's own QueryInterface means the proxy does the AddRef for
// us on one path and we do it on the other, and callers should not have to
// know which one they got.
template <typename T>
Microsoft::WRL::ComPtr<T> Native(T* iface, bool* is_proxy = nullptr) {
  if (is_proxy != nullptr) *is_proxy = false;
  if (iface == nullptr) return nullptr;

  Microsoft::WRL::ComPtr<T> result;
  void* base = nullptr;
  if (SUCCEEDED(iface->QueryInterface(BaseInterfaceIid(), &base)) && base != nullptr) {
    result.Attach(static_cast<T*>(base));  // already AddRef'd by the proxy
    if (is_proxy != nullptr) *is_proxy = true;
    return result;
  }

  // Not a proxy (or a proxy that refused). Hand back the caller's own
  // interface with a reference of our own.
  iface->AddRef();
  result.Attach(iface);
  return result;
}

// The native device behind a command queue or a command list, or null. Both
// expose GetDevice, and either may be a Streamline proxy whose device must not
// own addon objects. This is the resolver every creation path uses.
//
// Two QueryInterface calls: one for the device, one proxy probe that fails on a
// normal device.
template <typename T>
Microsoft::WRL::ComPtr<ID3D12Device> NativeDeviceOf(T* object) {
  DLSSNR_PROFILE_RESOURCE_L3("NativeDeviceOf");
  if (object == nullptr) return nullptr;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(object->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return nullptr;
  }
  return Native(device.Get());
}

}  // namespace reshade_dlssnr::sl_compat
