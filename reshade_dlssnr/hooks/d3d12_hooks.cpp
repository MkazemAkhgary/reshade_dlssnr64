#include "d3d12_hooks.h"

#include <cstdio>
#include <cstdlib>

#include <wrl/client.h>

#include <detours.h>

#include <reshade.hpp>

#include "d3d12_state.h"
#include "streamline_compat.h"

namespace reshade_dlssnr {

namespace {

bool g_logged_interposer_state = false;

// Names the module owning a hooked slot, once. A crash inside a hook that
// called into this module is otherwise attributed to "the game" or "the
// model", and the module is the whole explanation.
void LogHookedModuleOnce(const void* address) {
  static bool logged = false;
  if (logged || address == nullptr) return;
  logged = true;

  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &module) == 0 ||
      module == nullptr) {
    return;
  }

  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return;

  const wchar_t* name = path;
  for (const wchar_t* p = path; *p != L'\0'; ++p) {
    if (*p == L'\\') name = p + 1;
  }

  char message[MAX_PATH + 64];
  int at = std::snprintf(message, sizeof(message),
                         "D3D12 command list hooks installed on ");
  for (const wchar_t* p = name; *p != L'\0' && at > 0 &&
                               static_cast<size_t>(at) + 1 < sizeof(message);
       ++p) {
    message[at++] = (*p < 128) ? static_cast<char>(*p) : '?';
  }
  message[at] = '\0';

  reshade::log::message(reshade::log::level::info, message);
}

// One line at install time, so a Streamline build gone wrong is visible in
// the log without adding anything to the per-frame path.
void LogInterposerStateOnce(bool device_is_proxy) {
  if (g_logged_interposer_state) return;
  g_logged_interposer_state = true;

  if (!sl_compat::InterposerPresent()) {
    reshade::log::message(
        reshade::log::level::info,
        "NVIDIA Streamline not present; hooking the game's D3D12 interfaces directly");
  } else if (device_is_proxy) {
    reshade::log::message(
        reshade::log::level::info,
        "NVIDIA Streamline proxies the D3D12 device; hooks installed on the native interfaces");
  } else {
    reshade::log::message(
        reshade::log::level::info,
        "NVIDIA Streamline present without D3D12 proxies; hooking the game's D3D12 interfaces directly");
  }
}

using PFN_SetDescriptorHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_SetPipelineState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_SetPipelineState1 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, ID3D12StateObject*);
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_SetComputeRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_SetGraphicsRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_SetComputeRootDescriptorTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_SetComputeRoot32BitConstant = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_SetComputeRoot32BitConstants = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using PFN_SetComputeRootConstantBufferView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_SetComputeRootShaderResourceView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_SetComputeRootUnorderedAccessView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_CreateRootSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);

struct RootRestoreHook {
  void* original = nullptr;
};

PFN_SetDescriptorHeaps g_original_set_descriptor_heaps = nullptr;
PFN_SetPipelineState g_original_set_pipeline_state = nullptr;
PFN_SetPipelineState1 g_original_set_pipeline_state1 = nullptr;
PFN_Reset g_original_reset = nullptr;
PFN_SetComputeRootSignature g_original_set_compute_root_signature = nullptr;
PFN_SetGraphicsRootSignature g_original_set_graphics_root_signature = nullptr;
PFN_SetComputeRootDescriptorTable g_original_set_compute_root_descriptor_table = nullptr;
PFN_SetComputeRoot32BitConstant g_original_set_compute_root_32bit_constant = nullptr;
PFN_SetComputeRoot32BitConstants g_original_set_compute_root_32bit_constants = nullptr;
PFN_SetComputeRootConstantBufferView g_original_set_compute_root_cbv = nullptr;
PFN_SetComputeRootShaderResourceView g_original_set_compute_root_srv = nullptr;
PFN_SetComputeRootUnorderedAccessView g_original_set_compute_root_uav = nullptr;
PFN_CreateRootSignature g_original_create_root_signature = nullptr;

bool g_hooked_command_lists = false;
bool g_hooked_device = false;
bool g_tracking_enabled = true;

thread_local bool g_in_set_descriptor_heaps = false;
thread_local bool g_in_set_pipeline_state = false;
thread_local bool g_in_set_pipeline_state1 = false;
thread_local bool g_in_set_compute_root_signature = false;
thread_local bool g_in_set_graphics_root_signature = false;
thread_local bool g_in_set_compute_root_descriptor_table = false;
thread_local bool g_in_set_compute_root_32bit_constant = false;
thread_local bool g_in_set_compute_root_32bit_constants = false;
thread_local bool g_in_set_compute_root_cbv = false;
thread_local bool g_in_set_compute_root_srv = false;
thread_local bool g_in_set_compute_root_uav = false;

void STDMETHODCALLTYPE HookedSetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  if (g_tracking_enabled && !g_in_set_descriptor_heaps && command_list != nullptr) {
    D3D12StateTracker::Get().SetDescriptorHeaps(command_list, count, heaps);
  }
  g_original_set_descriptor_heaps(command_list, count, heaps);
}

void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
  if (g_tracking_enabled && !g_in_set_pipeline_state && command_list != nullptr) {
    D3D12StateTracker::Get().SetPipelineState(command_list, pipeline_state);
  }
  g_original_set_pipeline_state(command_list, pipeline_state);
}

void STDMETHODCALLTYPE HookedSetPipelineState1(ID3D12GraphicsCommandList4* command_list, ID3D12StateObject* state_object) {
  if (g_tracking_enabled && !g_in_set_pipeline_state1 && command_list != nullptr) {
    D3D12StateTracker::Get().SetPipelineState1(
        reinterpret_cast<ID3D12GraphicsCommandList*>(command_list), state_object);
  }
  g_original_set_pipeline_state1(command_list, state_object);
}

HRESULT STDMETHODCALLTYPE HookedReset(ID3D12GraphicsCommandList* command_list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial_state) {
  // A reset command list has no state, and everything the tracker holds for it
  // belongs to the recording that just ended. Dropping it here does two things:
  // it keeps the held references from accumulating for the life of the
  // process, and it stops a later restore from replaying a previous
  // recording's objects.
  if (command_list != nullptr) {
    D3D12StateTracker::Get().UntrackCommandList(command_list);
  }
  return g_original_reset(command_list, allocator, initial_state);
}

void STDMETHODCALLTYPE HookedSetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (g_tracking_enabled && !g_in_set_compute_root_signature && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootSignature(command_list, root_signature);
  }
  g_original_set_compute_root_signature(command_list, root_signature);
}

void STDMETHODCALLTYPE HookedSetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (g_tracking_enabled && !g_in_set_graphics_root_signature && command_list != nullptr) {
    D3D12StateTracker::Get().SetGraphicsRootSignature(command_list, root_signature);
  }
  g_original_set_graphics_root_signature(command_list, root_signature);
}

void STDMETHODCALLTYPE HookedSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  if (g_tracking_enabled && !g_in_set_compute_root_descriptor_table && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootDescriptorTable(command_list, index, base_descriptor);
  }
  g_original_set_compute_root_descriptor_table(command_list, index, base_descriptor);
}

void STDMETHODCALLTYPE HookedSetComputeRoot32BitConstant(ID3D12GraphicsCommandList* command_list, UINT index, UINT value, UINT offset) {
  if (g_tracking_enabled && !g_in_set_compute_root_32bit_constant && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRoot32BitConstant(command_list, index, value, offset);
  }
  g_original_set_compute_root_32bit_constant(command_list, index, value, offset);
}

void STDMETHODCALLTYPE HookedSetComputeRoot32BitConstants(ID3D12GraphicsCommandList* command_list, UINT index, UINT count, const void* data, UINT offset) {
  if (g_tracking_enabled && !g_in_set_compute_root_32bit_constants && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRoot32BitConstants(command_list, index, count, data, offset);
  }
  g_original_set_compute_root_32bit_constants(command_list, index, count, data, offset);
}

void STDMETHODCALLTYPE HookedSetComputeRootConstantBufferView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (g_tracking_enabled && !g_in_set_compute_root_cbv && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootConstantBufferView(command_list, index, address);
  }
  g_original_set_compute_root_cbv(command_list, index, address);
}

void STDMETHODCALLTYPE HookedSetComputeRootShaderResourceView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (g_tracking_enabled && !g_in_set_compute_root_srv && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootShaderResourceView(command_list, index, address);
  }
  g_original_set_compute_root_srv(command_list, index, address);
}

void STDMETHODCALLTYPE HookedSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (g_tracking_enabled && !g_in_set_compute_root_uav && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootUnorderedAccessView(command_list, index, address);
  }
  g_original_set_compute_root_uav(command_list, index, address);
}

HRESULT STDMETHODCALLTYPE HookedCreateRootSignature(
    ID3D12Device* device,
    UINT node_mask,
    const void* blob_with_root_signature,
    SIZE_T blob_length_in_bytes,
    REFIID riid,
    void** ppv_root_signature) {
  HRESULT result = g_original_create_root_signature(
      device, node_mask, blob_with_root_signature, blob_length_in_bytes, riid, ppv_root_signature);

  if (SUCCEEDED(result) && ppv_root_signature != nullptr && *ppv_root_signature != nullptr) {
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(
            blob_with_root_signature,
            blob_length_in_bytes,
            IID_PPV_ARGS(&deserializer)))) {
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc =
          deserializer->GetUnconvertedRootSignatureDesc();

      UINT parameter_count = 0;
      if (desc != nullptr) {
        switch (desc->Version) {
          case D3D_ROOT_SIGNATURE_VERSION_1_0:
            parameter_count = desc->Desc_1_0.NumParameters;
            break;
          case D3D_ROOT_SIGNATURE_VERSION_1_1:
            parameter_count = desc->Desc_1_1.NumParameters;
            break;
          case D3D_ROOT_SIGNATURE_VERSION_1_2:
            parameter_count = desc->Desc_1_2.NumParameters;
            break;
          default:
            break;
        }
      }

      if (parameter_count > 0) {
        D3D12StateTracker::Get().SetRootParameterCount(
            static_cast<ID3D12RootSignature*>(*ppv_root_signature),
            parameter_count);
      }

      deserializer->Release();
    }
  }

  return result;
}

}  // namespace

D3D12Hooks& D3D12Hooks::Get() {
  static D3D12Hooks hooks;
  return hooks;
}

bool D3D12Hooks::InstallEarlyHooks(ID3D12Device* device) {
  if (device == nullptr) return false;
  if (g_hooked_command_lists && g_hooked_device) return true;

  // Streamline 1.x always hands the host a proxy device, and detouring the
  // proxy's vtable entries instead of d3d12.dll's puts us inside Streamline's
  // own command list bookkeeping: its restorePipeline calls the very methods
  // we would have patched, and a ResourceBarrier on the proxy runs
  // Streamline's registered after-hooks from the middle of its evaluate.
  // Streamline 2.x only proxies when a feature asks for it. Resolve to the
  // native device either way, so the hooks always land on d3d12.dll.
  bool device_is_proxy = false;
  Microsoft::WRL::ComPtr<ID3D12Device> native_device =
      sl_compat::Native(device, &device_is_proxy);
  if (native_device == nullptr) return false;
  LogInterposerStateOnce(device_is_proxy);

  if (!g_hooked_device) {
    void** device_vtable = *reinterpret_cast<void***>(native_device.Get());
    if (device_vtable != nullptr) {
      g_original_create_root_signature =
          reinterpret_cast<PFN_CreateRootSignature>(device_vtable[16]);

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      if (DetourAttach(
              reinterpret_cast<void**>(&g_original_create_root_signature),
              HookedCreateRootSignature) == NO_ERROR) {
        DetourTransactionCommit();
        g_hooked_device = true;
      } else {
        DetourTransactionAbort();
      }
    }
  }

  if (g_hooked_command_lists) return true;

  ID3D12CommandAllocator* command_allocator = nullptr;
  ID3D12GraphicsCommandList* command_list = nullptr;

  if (FAILED(native_device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&command_allocator)))) {
    return false;
  }

  if (FAILED(native_device->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          command_allocator,
          nullptr,
          IID_PPV_ARGS(&command_list)))) {
    command_allocator->Release();
    return false;
  }

  void** vtable = *reinterpret_cast<void***>(command_list);
  if (vtable == nullptr) {
    command_list->Release();
    command_allocator->Release();
    return false;
  }

  // ID3D12GraphicsCommandList vtable slots. ID3D12Object contributes seven
  // (the three IUnknown methods plus four Get/SetPrivateData and SetName),
  // so the command list methods start at 9, not 5.
  g_original_set_pipeline_state = reinterpret_cast<PFN_SetPipelineState>(vtable[25]);
  g_original_set_descriptor_heaps = reinterpret_cast<PFN_SetDescriptorHeaps>(vtable[28]);
  g_original_set_compute_root_signature = reinterpret_cast<PFN_SetComputeRootSignature>(vtable[29]);
  g_original_set_graphics_root_signature = reinterpret_cast<PFN_SetGraphicsRootSignature>(vtable[30]);
  g_original_set_compute_root_descriptor_table = reinterpret_cast<PFN_SetComputeRootDescriptorTable>(vtable[31]);
  g_original_set_compute_root_32bit_constant = reinterpret_cast<PFN_SetComputeRoot32BitConstant>(vtable[33]);
  g_original_set_compute_root_32bit_constants = reinterpret_cast<PFN_SetComputeRoot32BitConstants>(vtable[35]);
  g_original_set_compute_root_cbv = reinterpret_cast<PFN_SetComputeRootConstantBufferView>(vtable[37]);
  g_original_set_compute_root_srv = reinterpret_cast<PFN_SetComputeRootShaderResourceView>(vtable[39]);
  g_original_set_compute_root_uav = reinterpret_cast<PFN_SetComputeRootUnorderedAccessView>(vtable[41]);
  g_original_reset = reinterpret_cast<PFN_Reset>(vtable[10]);

  // SetPipelineState1 lives on ID3D12GraphicsCommandList4, whose vtable
  // continues the base one, so its slot has to be read from that interface:
  // 75, after the 60 base slots. Not every command list exposes GCL4, so a
  // failed query skips the hook rather than guessing a slot.
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> command_list4;
  if (SUCCEEDED(command_list->QueryInterface(IID_PPV_ARGS(&command_list4))) &&
      command_list4 != nullptr) {
    void** vtable4 = *reinterpret_cast<void***>(command_list4.Get());
    if (vtable4 != nullptr) {
      g_original_set_pipeline_state1 =
          reinterpret_cast<PFN_SetPipelineState1>(vtable4[75]);
    }
  }

  command_list->Close();
  command_list->Release();
  command_allocator->Release();

  // Names the module owning the captured slots. With the D3D12 debug layer on
  // it is a d3d12SDKLayers thunk, which cannot be driven through a hook chain.
  LogHookedModuleOnce(g_original_set_pipeline_state);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  DetourAttach(reinterpret_cast<void**>(&g_original_set_pipeline_state), HookedSetPipelineState);
  DetourAttach(reinterpret_cast<void**>(&g_original_reset), HookedReset);
  if (g_original_set_pipeline_state1 != nullptr) {
    DetourAttach(reinterpret_cast<void**>(&g_original_set_pipeline_state1), HookedSetPipelineState1);
  }
  DetourAttach(reinterpret_cast<void**>(&g_original_set_descriptor_heaps), HookedSetDescriptorHeaps);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_signature), HookedSetComputeRootSignature);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_graphics_root_signature), HookedSetGraphicsRootSignature);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_descriptor_table), HookedSetComputeRootDescriptorTable);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_32bit_constant), HookedSetComputeRoot32BitConstant);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_32bit_constants), HookedSetComputeRoot32BitConstants);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_cbv), HookedSetComputeRootConstantBufferView);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_srv), HookedSetComputeRootShaderResourceView);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_uav), HookedSetComputeRootUnorderedAccessView);

  if (DetourTransactionCommit() == NO_ERROR) {
    g_hooked_command_lists = true;
    return true;
  }

  return false;
}

bool D3D12Hooks::Installed() const {
  return g_hooked_command_lists;
}

void D3D12Hooks::SetTrackingEnabled(bool enabled) {
  g_tracking_enabled = enabled;
}

void D3D12Hooks::RestoreCommandList(ID3D12GraphicsCommandList* command_list) {
  // Nothing was recorded if the hooks never went in, so there is nothing to
  // put back. Returning here rather than calling the unhooked originals also
  // keeps the tracker from being consulted on the Present path.
  if (command_list == nullptr || !g_hooked_command_lists) return;

  // DLSSNR_NO_RESTORE=1 separates "the replay is wrong" from "the pass left
  // something behind", which need different fixes.
  static const bool restore_disabled = [] {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "DLSSNR_NO_RESTORE") != 0 || value == nullptr) {
      return false;
    }
    const bool set = length > 1 && value[0] == '1';
    std::free(value);
    if (set) {
      reshade::log::message(
          reshade::log::level::info,
          "DLSSNR_NO_RESTORE is set: command list state is NOT being restored");
    }
    return set;
  }();
  if (restore_disabled) return;

  const auto heaps = D3D12StateTracker::Get().GetDescriptorHeaps(command_list);
  // Clamped to what was actually stored: the tracker records the real count but
  // only keeps the first two heaps, so passing the raw count through would hand
  // the runtime whatever follows the array.
  const UINT heap_count = heaps.NumDescriptorHeaps > 2 ? 2 : heaps.NumDescriptorHeaps;
  if (heap_count > 0 && heaps.Heaps[0] != nullptr) {
    g_original_set_descriptor_heaps(command_list, heap_count, heaps.Heaps);
  }

  // Only the compute signature: that is the slot our passes bind, and the
  // entries below belong to it. Replaying them against a graphics signature
  // addresses parameters it does not have and faults in the driver.
  const auto signatures = D3D12StateTracker::Get().GetSignatures(command_list);
  if (signatures.compute != nullptr) {
    g_original_set_compute_root_signature(command_list, signatures.compute);
  }

  const auto* root_state = D3D12StateTracker::Get().GetRootState(command_list);
  if (root_state != nullptr) {
    for (UINT i = 0; i < root_state->size(); ++i) {
      const RootState& state = (*root_state)[i];
      switch (state.type) {
        case RootEntryType::Table:
          g_original_set_compute_root_descriptor_table(command_list, i, state.rootDescriptorTable);
          break;
        case RootEntryType::Constant:
          // The recorded Data is what carries the value; an entry without it
          // is not something to hand the runtime a dangling pointer for.
          if (!state.Data.empty()) {
            g_original_set_compute_root_32bit_constant(command_list, i, state.Data[0], state.DestOffset);
          }
          break;
        case RootEntryType::Constants:
          if (!state.Data.empty()) {
            g_original_set_compute_root_32bit_constants(command_list, i, state.Num32BitValues, state.Data.data(), state.DestOffset);
          }
          break;
        case RootEntryType::CBV:
          g_original_set_compute_root_cbv(command_list, i, state.bufferLocation);
          break;
        case RootEntryType::SRV:
          g_original_set_compute_root_srv(command_list, i, state.bufferLocation);
          break;
        case RootEntryType::UAV:
          g_original_set_compute_root_uav(command_list, i, state.bufferLocation);
          break;
        default:
          break;
      }
    }
  }

  ID3D12PipelineState* pipeline_state = D3D12StateTracker::Get().GetPipelineState(command_list);
  if (pipeline_state != nullptr) {
    g_original_set_pipeline_state(command_list, pipeline_state);
  }

  // Last in the sequence, and last for a reason: a state object and a pipeline
  // state are alternatives for the same slot, so whichever was set most
  // recently is the one in force. Streamline's restore list puts it here too.
  ID3D12StateObject* state_object = D3D12StateTracker::Get().GetPipelineState1(command_list);
  if (state_object != nullptr && g_original_set_pipeline_state1 != nullptr) {
    g_original_set_pipeline_state1(
        reinterpret_cast<ID3D12GraphicsCommandList4*>(command_list), state_object);
  }
}

}  // namespace reshade_dlssnr
