# ReShade DLSSNR Addon — Agent Guide

This document explains the project structure, runtime architecture, dependencies, and critical implementation notes for agents working on this addon.

---

## Project Purpose

The addon runs NVIDIA DLSS Neural Rendering (feature 18) via the leaked `nvngx_dlssnr.dll` on top of a game's existing DLSS upscaling.

The active execution path is at **Present** on the final swapchain backbuffer. A planned feature will add execution at the **DLSS output stage** before UI and tone mapping.

---

## Directory Layout

```
reshade_dlssnr/
  addon.cpp                      // ReShade addon entry/exit and event adapters
  reshade_dlssnr.vcxproj         // main addon project
  reshade_dlssnr.sln             // solution
  AGENTS.md                      // this guide
  TODO.md                        // current feature plan

  compute/
    compute_engine.h/.cpp        // root signature, descriptor heap, generic dispatch
    compute_passes.h/.cpp        // HLSL pass registry
    shader_params.h              // dynamic shader parameter table

  runtime/
    addon_controller.h/.cpp      // controller facade, owns runtime + pass sessions
    runtime.h/.cpp               // shared model loading, patching, NGX init, parameter block
    command_context.h/.cpp       // command allocator/list/fence/barrier helpers
    feature_lifecycle.h/.cpp     // feature creation/rebuild/tuning state
    model_pipeline.h/.cpp        // prepare model input + compose model output
    pass_resources.h/.cpp        // per-pass output/composite/working/scratch textures
    pass_session.h/.cpp          // per-pass orchestrator
    ngx_model_params.h/.cpp      // shared NGX tuning parameter application
    ngx_parameter_block.h/.cpp   // real driver NGX parameter block vtable access
    d3d12_utils.h                // resource-state inference helpers

  hooks/
    d3d12_hooks.h/.cpp           // early D3D12 command-list state hooking
    d3d12_state.h/.cpp           // D3D12 state tracker
    dlss_guides.h/.cpp           // DLSS depth/motion guide capture
    local_vtable.h               // minimal Detours vtable hook wrapper
    nvngx_hook_compat.h          // hooks NVSDK_NGX Create/Evaluate

  state/
    settings.h/.cpp              // persistent settings, two passes
    version.h                    // addon version constant

  ui/
    ui.h/.cpp                    // all ImGui overlay drawing

  third_party/
    detours/                     // local Detours headers + lib
    nvngx_dlss_sdk/              // NGX SDK headers
    d3dx/d3dx12.h                // D3DX12 helper header
```

---

## Active Runtime Flow

```text
ReShade present
  -> AddonController::Get().OnPresent
      -> runtime_.Initialize(device)
      -> pass1_.OnPresent(runtime_, settings.pass1, ...)
      -> if enabled pass2_.OnPresent(runtime_, settings.pass2, ...)
```

Each `PassSession::OnPresent`:

1. Ensures D3D12 command context and pass resources.
2. Creates/rebuilds DLSSNR feature through `FeatureLifecycle`.
3. Gets DLSS guide state (depth/motion) from `DlssGuides`.
4. Handles working-scale debug views.
5. Prepares working-scale input if enabled.
6. Fills real NGX parameter block via `NgxParameterBlock`.
7. Evaluates the model.
8. Applies working-scale upsample or final luma/chroma composite.
9. Copies result to the swapchain backbuffer.

---

## Critical Notes

### DLSSNR model and parameter block

- The model is loaded directly from `nvngx_dlssnr.dll`.
- The addon patches the model's IAT `GetModuleFileNameW` to satisfy its caller check.
- The model expects tuning values through the **real NGX capability parameter block**, not a custom parameter object.
- `NgxParameterBlock` probes the correct float setter vtable slot at runtime.

### Feature lifecycle

- Tuning values are set at **feature creation**; most changes require a feature rebuild.
- `FeatureLifecycle` tracks built tuning and only rebuilds when width/height/tuning change.
- Do **not** evaluate a newly created feature on the same frame; skip evaluation on creation frame.

### Model controls

The real model selectors are:

- `DLSSNR.Style` = 0/1/2 selects Model A/B/C.
- `DLSSNR.Hint.Render.Preset` is only a compatibility hint.

### Resource and command safety

- ReShade `resource` handles are valid only during the current event callback; add `AddRef` before storing.
- Use owned command list/fence to avoid interfering with game D3D12 state.
- Release retired game resources only after GPU idle to avoid `DXGI_ERROR_DEVICE_HUNG`.

### UI mask (intentionally unused)

- The model supports `DLSSNR.UIAlpha` and `DLSSNR.Backbuffer` resources for UI masking.
- These are intentionally not bound for now. See the comment in `pass_session.cpp`.

### White point and passthrough

- `White Point Scale` and `Passthrough` have been removed from the present path.
- The present path operates on a display-referred backbuffer, so no HDR normalization is applied.
- The planned DLSS-stage feature will reintroduce automatic white point handling.

---

## Dependencies

- ReShade SDK
- Local `third_party/`:
  - Detours
  - NGX SDK headers
  - D3DX12 helper header
- `nvngx_dlssnr.dll` must be beside the game executable.

No renodx or OptiScaler build dependency remains.

---

## Build

- Project: `reshade_dlssnr.vcxproj`
- Platform: `Release|x64`
- Toolset: `v145`
- Output: `reshade_dlssnr64.addon64`
- Link: `d3d12.lib;dxgi.lib;detours.lib;d3dcompiler.lib`

---

## Recent Changes

- Removed `White Point Scale` and `Passthrough` from settings, shaders, stages, and UI.
- Fixed addon lifecycle registration and unhook cleanly in `AddonUninit`.
- Extracted hook/local vtable, runtime, stages, codec, state, and UI into domain folders.
- Replaced renodx hook dependency with local `local_vtable`.
- Added version `1.0.0` display.

---

## Planned Work

See `TODO.md` for the DLSS-stage feature and automatic white point implementation.
