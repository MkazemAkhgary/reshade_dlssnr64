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

// Attaches every export in `hooks` in one transaction.
//
// All-or-nothing: a target that cannot be resolved, a DetourAttach that fails
// or a commit that fails all leave the module untouched and *real == nullptr.
// That matters because the callers retry -- the NGX hook is re-attempted on
// every Present until nvngx_dlss.dll shows up -- and a half-attached table
// would let a later call detour a target twice.
//
// An entry whose *real is already set is skipped rather than re-attached.
static bool Hook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return false;

  // Indices into `hooks` whose *real this call filled in, so a failure can
  // undo exactly what it did.
  std::vector<size_t> attached;

  auto begin_transaction = []() -> bool {
    if (DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) == NO_ERROR) return true;
    DetourTransactionAbort();
    return false;
  };

  auto rollback = [&hooks, &attached]() {
    for (size_t index : attached) {
      *std::get<1>(hooks[index]) = nullptr;
    }
  };

  for (size_t i = 0; i < hooks.size(); ++i) {
    const auto& [name, real, hook] = hooks[i];
    if (real == nullptr || hook == nullptr || *real != nullptr) continue;

    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) continue;

    if (attached.empty() && !begin_transaction()) return false;

    *real = reinterpret_cast<void*>(proc);
    attached.push_back(i);

    if (DetourAttach(real, hook) != NO_ERROR) {
      DetourTransactionAbort();
      rollback();
      return false;
    }
  }

  if (attached.empty()) return false;

  if (DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    rollback();
    return false;
  }

  return true;
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
