# ReShade DLSSNR Addon — Agent Guide

This document explains the project structure, runtime architecture, dependencies, and critical implementation notes for agents working on this addon.

---

## Project Purpose

The addon runs NVIDIA DLSS Neural Rendering (feature 18) via the leaked `nvngx_dlssnr.dll` on top of a game's existing DLSS upscaling. It runs the model at one of two points in the frame:

- **At DLSS** (Post-DLSS): inline on the game's command list, immediately after the game's own DLSS `EvaluateFeature`, before tone mapping and UI.
- **At Present**: at the ReShade present event, on the final swapchain backbuffer.

Both hook points share the same model pipeline; they differ only in the input resource and where the result is written.

---

## Directory Layout

```
reshade_dlssnr/
  addon.cpp                      // ReShade addon entry/exit and event adapters
  reshade_dlssnr.vcxproj         // main addon project
  reshade_dlssnr.sln             // solution
  AGENTS.md                      // this guide

  compute/
    compute_engine.h/.cpp        // root signature, descriptor heap, generic dispatch
    compute_passes.h/.cpp        // HLSL pass registry (one table)
    shader_params.h              // dynamic shader parameter table

  runtime/
    addon_controller.h/.cpp      // controller facade, owns runtime + pass sessions
    runtime.h/.cpp               // model loading, patching, NGX init, parameter block
    command_context.h/.cpp       // command allocator/list/fence/barrier/state helpers
    feature_lifecycle.h/.cpp     // feature creation/rebuild state
    model_pipeline.h/.cpp        // white point, prepare input, compose output
    pass_resources.h/.cpp        // per-pass textures + scratch pool + white point buffer
    pass_session.h/.cpp          // per-pass orchestrator
    frame_debug_context.h        // per-frame debug output resource
    debug_view.h/.cpp            // debug view registry + render
    ngx_model_params.h/.cpp      // shared NGX tuning parameter application
    ngx_parameter_block.h/.cpp   // real driver NGX parameter block vtable access
    d3d12_utils.h                // resource-state inference helpers

  hooks/
    d3d12_hooks.h/.cpp           // early D3D12 command-list state hooking
    d3d12_state.h/.cpp           // D3D12 state tracker
    dlss_guides.h/.cpp           // DLSS depth/motion/output capture
    local_vtable.h               // minimal Detours vtable hook wrapper
    nvngx_hook_compat.h          // hooks NVSDK_NGX Create/Evaluate

  state/
    settings.h/.cpp              // persistent settings, up to kMaxPasses passes
    version.h                    // addon version constant

  ui/
    ui.h/.cpp                    // all ImGui overlay drawing

  third_party/
    detours/                     // local Detours headers + lib
    nvngx_dlss_sdk/              // NGX SDK headers
    d3dx/d3dx12.h                // D3DX12 helper header
```

---

## Runtime Flow

```text
Game frame:
  ...
  -> game calls NVSDK_NGX_D3D12_EvaluateFeature (DLSS upscaling)
      -> hooked_NVSDK_NGX_D3D12_EvaluateFeature
          -> real EvaluateFeature
          -> DlssGuides::Capture           // clones depth/motion/DLSS output
          -> AddonController::OnPostDlssEvaluate
              -> for each enabled pass at DLSS Output:
                  PassSession::OnPostDlssEvaluate
  ...

ReShade present:
  -> AddonController::OnPresent
      -> for each enabled pass at Backbuffer:
          PassSession::OnPresent
      -> if a debug view was rendered this frame:
          PresentDebugOutput             // copy frame_debug.output to backbuffer
```

`PassSession::OnPresent` and `PassSession::OnPostDlssEvaluate` run the same model pipeline; they differ in:

- **Input**: the backbuffer (Present) or the captured DLSS output clone (Post-DLSS).
- **Output target**: the backbuffer (Present) or the game's DLSS output resource (Post-DLSS, in place).

---

## Critical Notes

### Model loading and parameter block

- The model is loaded directly from `nvngx_dlssnr.dll`.
- The addon patches the model's IAT `GetModuleFileNameW` to satisfy its caller check.
- Tuning values are passed through the **real NGX capability parameter block** obtained from `_nvngx.dll`.
- `NgxParameterBlock` probes the correct float setter vtable slot at runtime.

### Feature lifecycle

- **Only a resolution change rebuilds the feature.** Tuning values (preset, intensity, structure strengths, UI correction, etc.) are re-applied every evaluation through `FillEvaluateParams` -> `ApplyModelTuningParams`, so they never trigger a rebuild.
- The feature is always built at **full frame resolution** — working scale affects only the per-evaluation size, never the feature handle.
- After a rebuild, `FeatureLifecycle::ConsumeJustRebuilt()` returns true for exactly one frame; the pass session skips evaluation that frame so NGX has time to initialize.

### Model controls

- `DLSSNR.Style` = 0/1/2 selects Model A/B/C.
- `DLSSNR.Hint.Render.Preset` is a compatibility hint.

### White point

The pipeline normalizes HDR input to SDR for the model and remaps the output back to HDR:

1. `MeasureWhitePoint` reduces the source to an 8×8 tile buffer of per-tile max luma.
2. `PrepareModelInput` divides the source by `W = max(tiles, 1.0)` — SDR content is unaffected.
3. `ComposeModelOutput` multiplies the result back by `W`.

With the model as a no-op, this round-trips exactly. Because `W >= max_luma`, the SDR intermediate never clips (no burn-in).

### Resource and command safety

- ReShade resource handles are valid only during the current event callback; `AddRef` before storing.
- The addon uses its own command list and fence for the Present path to avoid interfering with the game's D3D12 state.
- Retired game resources are released only after GPU idle (`PassSession::SubmitAndWait`).
- `CommandContext::Transition` tracks resource state per command list; `ClearState` is called at each pass boundary to prevent cross-pass aliasing.

### Post-DLSS specifics

- The Post-DLSS pass writes the composed result **in place** into the game's DLSS output resource, so the game continues from a modified image.
- Each Post-DLSS pass after the first snapshots the previous pass's result into scratch slot 0 (`kInputSnapshotSlot`) so chained passes don't alias the output resource.
- The evaluation runs on the **game's command list** inside the NVSDK hook, not on our own list.

### Debug views

- Debug views are declared in **one table** in `debug_view.cpp`. Adding or removing a view is a single-line edit.
- The debug view is rendered **at pass time** into `FrameDebugContext::output` — a resource owned by `AddonController` — never into the game's resources.
- The Present event then copies `frame_debug.output` to the backbuffer as the last touch before swapchain present.
- The debug output is created at the **backbuffer format**, so the final copy is always format-compatible.
- Only the **active pass** (matching `Settings::active_pass`, the currently open UI tab) renders its debug view; all other passes just run the pipeline.

### UI mask (intentionally unused)

- The model supports `DLSSNR.UIAlpha` and `DLSSNR.Backbuffer` resources for UI masking.
- These are intentionally not bound for now. See the comment in `pass_session.cpp`.

---

## Settings and Passes

- `kMaxPasses` (3) passes, each with its own `PassSettings`.
- Each pass has a `Target` (DLSS Output or Backbuffer) and a `Debug View`.
- Constraint: once a pass targets Backbuffer, all later passes must too. Enforced at load and in the UI.
- Dynamic shader params are driven by `compute/shader_params.h`; adding one entry propagates to the UI, settings persistence, and shader cbuffer automatically.

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
