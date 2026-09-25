// Asking a proxy for the object underneath it.
//
// Two layers can sit between the addon and d3d12.dll, and they use the same
// trick -- a QueryInterface with a fixed IID that returns the original:
//
//   ReShade    wraps every D3D12 object it hands the game, and answers
//              IID_UnwrappedObject {7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}
//              with it. Handled by the command list, device, command queue,
//              descriptor heap, pipeline library and swapchain wrappers
//              (ReShade's com_utils.hpp and source/d3d12/*.cpp).
//   Streamline hands the game proxies on 1.x and answers its base-interface
//              IID with it. See streamline_compat.h.
//
// Both matter for the same reason. The vtable hooks in d3d12_hooks.cpp are
// installed on a command list created from the *unwrapped* device, so they
// intercept the unwrapped chain and record state under unwrapped pointers.
// The game, meanwhile, hands us its own wrappers. Resolving here is what makes
// those the same object -- without it every tracker lookup misses and the
// restore silently does nothing.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "streamline_compat.h"

namespace reshade_dlssnr::proxy_unwrap {

// {7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}
inline const GUID& ReShadeUnwrappedIid() {
  static constexpr GUID kIid = {
      0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
  return kIid;
}

// The object ReShade wrapped, or `iface` when it is not a ReShade proxy.
// `was_proxy` reports which. The result always owns a reference, so put it in
// a ComPtr.
template <typename T>
Microsoft::WRL::ComPtr<T> ReShadeUnwrapped(T* iface, bool* was_proxy = nullptr) {
  if (was_proxy != nullptr) *was_proxy = false;
  if (iface == nullptr) return nullptr;

  Microsoft::WRL::ComPtr<T> result;
  void* original = nullptr;
  if (SUCCEEDED(iface->QueryInterface(ReShadeUnwrappedIid(), &original)) &&
      original != nullptr) {
    result.Attach(static_cast<T*>(original));
    if (was_proxy != nullptr) *was_proxy = true;
    return result;
  }

  iface->AddRef();
  result.Attach(iface);
  return result;
}

// Both layers, ReShade first.
//
// The order is forced: a ReShade wrapper sits around a Streamline proxy, which
// sits around the real object, and each layer only answers for the object it
// itself wraps. On a game with no Streamline the second step is a no-op, and
// on a 2.x host whose interposer has nothing to proxy the first one is.
//
// This is the pointer our vtable hooks see, and therefore the only pointer the
// state tracker and the restore agree on.
template <typename T>
Microsoft::WRL::ComPtr<T> NativeChain(T* iface, bool* was_proxy = nullptr) {
  bool reshade_proxy = false;
  Microsoft::WRL::ComPtr<T> inner = ReShadeUnwrapped(iface, &reshade_proxy);

  bool streamline_proxy = false;
  Microsoft::WRL::ComPtr<T> native = sl_compat::Native(inner.Get(), &streamline_proxy);

  if (was_proxy != nullptr) *was_proxy = reshade_proxy || streamline_proxy;
  return native;
}

}  // namespace reshade_dlssnr::proxy_unwrap
