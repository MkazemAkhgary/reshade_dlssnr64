// Minimal Detours vtable hook wrapper.
//
// Derived from renodx/src/utils/vtable.hpp.
// SPDX-License-Identifier: MIT
// Copyright (C) 2025 Carlos Lopez
#pragma once

#include <windows.h>

#include <detours.h>

#include <cassert>
#include <tuple>
#include <vector>

namespace reshade_dlssnr::local_vtable {

using HookItem = std::tuple<const char*, void**, void*>;

static bool Hook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return false;

  bool any_hooked = false;
  bool transaction_started = false;
  bool commit_failed = false;

  for (const auto& [name, real, hook] : hooks) {
    if (real == nullptr || hook == nullptr) continue;

    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) continue;

    assert(*real == nullptr);

    *real = reinterpret_cast<void*>(proc);

    if (!transaction_started) {
      if (DetourTransactionBegin() != NO_ERROR) {
        *real = nullptr;
        continue;
      }
      if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort();
        *real = nullptr;
        continue;
      }
      transaction_started = true;
    }

    if (DetourAttach(reinterpret_cast<void**>(real), hook) == NO_ERROR) {
      any_hooked = true;
    } else {
      DetourTransactionAbort();
      *real = nullptr;
      transaction_started = false;
      commit_failed = true;
      break;
    }
  }

  if (transaction_started && !commit_failed) {
    if (DetourTransactionCommit() != NO_ERROR) {
      DetourTransactionAbort();
      commit_failed = true;
    }
  }

  if (commit_failed) {
    // Rollback any hooks attached in this transaction.
    for (const auto& [name, real, hook] : hooks) {
      if (real == nullptr || *real == nullptr || hook == nullptr) continue;

      if (DetourTransactionBegin() != NO_ERROR) continue;
      if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort();
        continue;
      }
      if (DetourDetach(reinterpret_cast<void**>(real), hook) == NO_ERROR) {
        DetourTransactionCommit();
        *real = nullptr;
      } else {
        DetourTransactionAbort();
      }
    }
    any_hooked = false;
  }

  return any_hooked;
}

static void Unhook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return;

  for (const auto& [name, real, hook] : hooks) {
    if (real == nullptr || *real == nullptr || hook == nullptr) continue;

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
