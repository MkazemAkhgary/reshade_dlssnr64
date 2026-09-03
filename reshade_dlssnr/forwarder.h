#pragma once

#include <windows.h>
#include <d3d12.h>

namespace reshade_dlssnr {

using PFN_DlssnrSetFloatSlot = void(__cdecl*)(int slot);
using PFN_DlssnrProbeFloat = void(__cdecl*)(void* params, const char* name, float value, int slot);
using PFN_DlssnrCreate = void*(__cdecl*)(
    const wchar_t* snippet_path,
    const wchar_t* data_path,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    void* capability_params,
    unsigned int width,
    unsigned int height,
    int preset,
    float intensity,
    int style,
    float local_structure,
    float local_tone,
    float skin_structure,
    int use_auto_mask,
    int ui_correction);
using PFN_DlssnrEvaluate = int(__cdecl*)(
    ID3D12GraphicsCommandList* command_list,
    void* feature,
    void* capability_params,
    ID3D12Resource* color,
    ID3D12Resource* depth,
    ID3D12Resource* motion,
    ID3D12Resource* output,
    unsigned int width,
    unsigned int height,
    unsigned int guide_width,
    unsigned int guide_height,
    int depth_inverted,
    int reset,
    float intensity,
    int style,
    float local_structure,
    float local_tone,
    float skin_structure,
    int use_auto_mask,
    float mv_scale_x,
    float mv_scale_y);
using PFN_DlssnrRelease = void(__cdecl*)(void* feature);
using PFN_DlssnrSetExtras = void(__cdecl*)(
    void* capability_params,
    float global_tone,
    ID3D12Resource* ui,
    ID3D12Resource* ui_alpha,
    ID3D12Resource* backbuffer,
    unsigned int ui_width,
    unsigned int ui_height,
    unsigned int bb_width,
    unsigned int bb_height);

struct ForwarderApi {
  HMODULE module = nullptr;
  PFN_DlssnrSetFloatSlot set_float_slot = nullptr;
  PFN_DlssnrProbeFloat probe_float = nullptr;
  PFN_DlssnrCreate create = nullptr;
  PFN_DlssnrEvaluate evaluate = nullptr;
  PFN_DlssnrRelease release = nullptr;
  PFN_DlssnrSetExtras set_extras = nullptr;
  int* last_init = nullptr;
  int* last_create = nullptr;

  bool Load(const wchar_t* path);
  bool IsLoaded() const { return create != nullptr && evaluate != nullptr; }
};

const wchar_t* GetDefaultForwarderName();
const wchar_t* GetDefaultModelName();

}  // namespace reshade_dlssnr
