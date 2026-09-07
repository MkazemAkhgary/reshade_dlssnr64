// Minimal Detours vtable hook wrapper with no logging.
#pragma once

#include <windows.h>

#include <detours.h>

#include <tuple>
#include <vector>

namespace reshade_dlssnr::local_vtable {

using HookItem = std::tuple<const char*, void**, void*>;

static bool Hook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return false;

  bool any_hooked = false;
  bool transaction_started = false;

  for (const auto& [name, real, hook] : hooks) {
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) continue;

    *real = reinterpret_cast<void*>(proc);

    if (!transaction_started) {
      if (DetourTransactionBegin() != NO_ERROR) continue;
      if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort();
        continue;
      }
      transaction_started = true;
    }

    if (DetourAttach(reinterpret_cast<void**>(real), hook) == NO_ERROR) {
      any_hooked = true;
    } else {
      DetourTransactionAbort();
      transaction_started = false;
    }
  }

  if (transaction_started) {
    if (DetourTransactionCommit() != NO_ERROR) {
      DetourTransactionAbort();
      any_hooked = false;
    }
  }

  return any_hooked;
}

static void Unhook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return;

  for (const auto& [name, real, hook] : hooks) {
    if (*real == nullptr || hook == nullptr) continue;

    if (DetourTransactionBegin() != NO_ERROR) continue;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
      DetourTransactionAbort();
      continue;
    }

    if (DetourDetach(reinterpret_cast<void**>(real), hook) != NO_ERROR) {
      DetourTransactionAbort();
      continue;
    }

    if (DetourTransactionCommit() == NO_ERROR) {
      *real = nullptr;
    } else {
      DetourTransactionAbort();
    }
  }
}

}  // namespace reshade_dlssnr::local_vtable
