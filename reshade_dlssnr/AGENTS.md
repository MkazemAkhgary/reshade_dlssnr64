# ReShade DLSSNR Addon — Agent Guide

This document explains the project structure, runtime architecture, dependencies, and critical implementation notes for agents working on this addon.

---

## Project Purpose

The addon runs NVIDIA DLSS Neural Rendering (feature 18) via the leaked
`nvngx_dlssnr.dll` on top of a game's existing DLSS upscaling.

The model runs at one of two points in the frame:

- **At DLSS Output** (Post-DLSS): inline on the game's command list,
  immediately after the game's own DLSS `EvaluateFeature`, before tone
  mapping and UI. This is the recommended path.
- **At Backbuffer** (Present): at the ReShade present event, on the final
  swapchain backbuffer.

Both hook points share the same model pipeline. They differ only in the
input resource and where the result is written.

For HDR content, the Post-DLSS path additionally normalizes the image to
SDR before the model sees it and remaps the model's edit back into the
source's HDR range on output. See `HDR_DESIGN.md`.

---

## Directory Layout

```
reshade_dlssnr/
  addon.cpp                      // ReShade addon entry/exit and event adapters
  reshade_dlssnr.vcxproj         // main addon project
  reshade_dlssnr.sln             // solution
  AGENTS.md                      // this guide
  HDR_DESIGN.md                  // HDR pipeline design and rationale

  compute/
    compute_engine.h/.cpp        // root signature, descriptor heap, generic dispatch
    compute_passes.h/.cpp        // HLSL pass registry (one table)
    shader_params.h              // dynamic shader parameter table

  runtime/
    addon_controller.h/.cpp      // controller facade; owns runtime + pass sessions + status
    runtime.h/.cpp               // model loading, patching, NGX init, parameter block
    command_context.h/.cpp       // command allocator/list/fence/barrier/state helpers
    feature_lifecycle.h/.cpp     // feature handle creation and rebuild state
    model_pipeline.h/.cpp        // prepare input, compose output, HDR variants
    pass_resources.h/.cpp        // per-pass textures + scratch pool
    pass_session.h/.cpp          // per-pass orchestrator
    frame_debug_context.h        // per-frame debug output container
    debug_view.h/.cpp            // debug view registry + render
    hdr_state.h/.cpp             // HDR measurement, exposure EMA, readback
    gpu_readback.h/.cpp          // N-frame-delayed GPU -> CPU readback ring
    ngx_model_params.h/.cpp      // shared NGX tuning parameter application
    ngx_parameter_block.h/.cpp   // real driver NGX parameter block vtable access
    d3d12_utils.h                // resource-state inference helper
    status.h                     // StatusSnapshot struct (header-only)

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
              -> HdrState::BeginFrame       // measure, if scheduled
              -> for each enabled pass at DLSS Output:
                  PassSession::OnPostDlssEvaluate
  ...

ReShade present:
  -> AddonController::OnPresent
      -> for each enabled pass at Backbuffer:
          PassSession::OnPresent
      -> if a debug view was rendered this frame:
          PresentDebugOutput             // copy frame_debug.output to backbuffer
      -> HdrState::SignalAtPresent       // signal readback fence
```

`PassSession::OnPresent` and `OnPostDlssEvaluate` run the same model pipeline; they differ in input, output target, and whether the game's command list is used (Post-DLSS) or our own (Present).

---

## Hard-Won Findings

These are things that took time to discover and are easy to break by accident.

### D3D12 / GPU

**ComPtr + Detach vs Reset.** `RetireResource` used to `ComPtr::Reset()` after
copying the raw pointer into a retire list, which releases the underlying
resource while the raw pointer is still queued for later Release — a double
free. It must `Detach()`, transferring ownership to the retire list.

**All replaced resources must go through the retire queue.** Even the Present
path can be wrong if a resource is replaced on a frame where the game's queue
still has work in flight from an earlier DLSS-mode frame. `PassResources`
requires a `RetireCallback` and logs an error if `Dispose` is called without
one.

**Resource state must be seeded, not inferred, for anything we own.** The
tracker's `InferResourceState` reads D3D12 resource flags. For buffers, it
returns GENERIC_READ; for textures, it maps flags to a plausible state. This
is wrong for anything we created as UAV or left in a known state after our
own passes. `SeedFrameState` / `CleanupFrameState` enforce the invariant
that pass-owned resources are in UNORDERED_ACCESS at frame boundaries.

**The model output after NGX EvaluateFeature needs a UAV barrier.** A state
transition alone does not synchronize UAV writes; the model output will read
as stale. `CommandContext::UavBarrier` handles this.

**Post-DLSS passes must not close / submit / wait on the game's command list.**
They only record commands. This is why the retry path releases the NGX feature
rather than trying to synchronize.

### HLSL

**`groupshared` declarations must be at global scope**, not inside the shader
entry point. Compilation fails with "local variables cannot be declared
'groupshared'".

**Some identifiers are reserved** and silently break compilation with vague
errors. `linear` is one; naming a local `linear` produces "unexpected token
'linear'".

**HLSL silently truncates a `float3` argument to a `float` parameter by taking
`.x`**, then broadcasts the scalar result back to `float3` on return. Any
function that operates on a color must take and return `float3`. The extended
Reinhard forward and inverse both had this bug and produced grayscale output
until the signatures were corrected.

**A 3-channel x 3-channel matrix multiply must be explicit.** `mul(m, v)` with
a `float3x3` and `float3` is correct; `m * v` componentwise is a silent error.

### Pipeline

**The feature must be built at full frame resolution**, never at working-scale
resolution. Working scale only affects the per-frame evaluation size, which
`FillEvaluateParams` sets every frame. Building at working scale causes a
feature rebuild every time the working-scale slider changes.

**Tuning changes must not trigger a feature rebuild.** The pre-refactor code
rebuilt on every tuning change and made the UI sluggish. Tuning is re-applied
every frame via `ApplyModelTuningParams`; only a resolution change rebuilds.

**Two frames must pass between a feature creation and its first evaluation.**
`FeatureLifecycle::ConsumeJustRebuilt()` returns true for exactly one frame
after a rebuild, and the pass session skips evaluation that frame.

**The luma / chroma strength blend must interpolate chroma in normalized
space** (`Cb/Y`, `Cr/Y`), not absolute Cb/Cr. Absolute chroma blended
independently of luma produces over-saturation when luma alone moves and
paleness when chroma alone moves.

**Extended Reinhard maps `x = W` to SDR 1.0.** Any pixel above W clips,
and the clip flattens highlights on the round trip. The default
`Highlight Headroom` multiplier is 8× the auto-detected P99, which is
generous but preserves details. Values below ~2 will visibly flatten
highlights.

### HDR detection

**`R10G10B10A2_UNORM` should be treated as HDR10, not SDR.** True 10-bit SDR
is essentially unused in games; when this format appears as a DLSS output it
is almost always HDR10. The auto-detector reflects this; users with genuine
10-bit SDR can override.

**`R11G11B10_FLOAT` is scRGB.** It was missing from the original detector.

### Debug views

**Debug views must be rendered at pass time**, not at Present time. The
source resource's state is only known during the pass that produced it; by
the time we reach Present, states may have changed and the copy is wrong.
`MaybeRenderDebug` runs on the pass's own command list, writing into
`FrameDebugContext::output`. `PresentDebugOutput` at Present just copies it.

**Debug output must be created at the backbuffer's format**, even for
Post-DLSS passes whose source is HDR. Otherwise the copy to the backbuffer
fails on format mismatch. `FrameDebugContext::preferred_format` carries this.

**Debug view selection is shared across passes**, not per-pass. `Settings::debug_view`
is a single value; only the currently-open tab renders it.

### UI

**The pass tab's open/close state is per-pass**; the HDR and Status sections
are shared. `PersistentHeader` reads and writes the persisted flag through
`SetNextItemOpen`.

**The debug view combo must not be inside the disabled pass tab scope.**
Moving it out of `DrawPassSettingsBody` was necessary so a user can select a
debug view even when the current pass is disabled.

**Button groups** share a helper (`DrawGroupButtons`). The `selected` entry is
highlighted across all three button states via `PushStyleColor` on `Button`,
`ButtonHovered`, and `ButtonActive`. Per-item tooltips use
`IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)` so disabled entries still
explain themselves.

### Feature failures

**A failed pass is retryable.** `PassSession::Retry` clears the failure and
releases the NGX feature, forcing a clean rebuild. The Status UI shows a
Retry button only when the pass is failed. Otherwise the only way to recover
was to toggle the master switch.

---

## Critical Notes

### Model loading and parameter block

- The model is loaded directly from `nvngx_dlssnr.dll`.
- The addon patches the model's IAT `GetModuleFileNameW` to satisfy its caller check.
- Tuning values are passed through the **real NGX capability parameter block**
  obtained from `_nvngx.dll`.
- `NgxParameterBlock` probes the correct float setter vtable slot at runtime.

### Model controls

- `DLSSNR.Style` = 0/1/2 selects Model A/B/C.
- `DLSSNR.Hint.Render.Preset` is a compatibility hint.

### Inheritance

Pass inheritance (the `I` button in the UI) is resolved in the settings layer
on load and on save. `GetResolvedPass(i)` returns the resolved settings; the
runtime reads it directly. There is no per-frame resolution.

### HDR pipeline

See `HDR_DESIGN.md`. Short version:

1. `MeasureHistogram` reduces the source to a 256-bin log2-luminance
   histogram. The grid must be 1×1×1, not frame-size, because the shader
   only uses the first 64 threads (one 8×8 group).
2. `HdrState::UpdateExposure` reads the histogram, picks percentiles, and
   updates an exposure + white-point EMA.
3. `TonemapForward` decodes source (SDR / scRGB / HDR10 PQ) to linear,
   multiplies by exposure, applies extended Reinhard per channel, encodes
   to sRGB, and writes to the working input. The tonemap shader doubles as
   the working-scale resize.
4. The model sees the SDR image and returns an SDR output.
5. `ComposeModelOutputHdr` rebuilds the full-res SDR the original would have
   tonemapped to, applies the model's SDR residual, inverse-tonemaps back to
   HDR, and blends with the source using the luma / chroma strengths.

The no-op model round trip is exact (up to saturation at the white point).

### Resource ownership

Command contexts and compute engines owned by `AddonController`:

| Member | Purpose |
|---|---|
| `debug_command_` | the copy that puts a debug view onto the backbuffer at Present |
| `pass1_command_` | the histogram dispatch (records on the game's list) |
| `debug_codec_` (per `PassSession`) | dispatch engine for the pass's own stages |
| `hdr_codec_` | dispatch engine for the histogram passes |

Each `PassSession` owns its own `codec_` (a `ComputeEngine`) and all its
pass-scoped resources.

---

## Logging and Status

**Log** is a historical record: lifecycle events (addon attach/detach,
feature creation, format detection changes, first-time errors), not per-frame
chatter. Every failure that marks a pass as failed logs once, via
`PassSession::Fail`.

**Status** is the current state: `StatusSnapshot`, refreshed by
`AddonController::RefreshStatus` at the end of every frame. Rendered by the
Status section in the UI. It answers "is the addon working right now, and if
not, what is wrong" without requiring the user to read the log.

Do not add per-frame log messages during normal operation. If a value is
useful every frame, put it in Status.

---

## Adding Things

### A new shader pass

1. Add the HLSL source to `compute_passes.cpp` inside the anonymous namespace.
2. Add one entry to `kComputePasses[]`.
3. Dispatch it through a `ComputeEngine`. If the pass is not full-screen
   (e.g. a reduction), pass explicit `threads_x/y/z` to `Dispatch`.

The descriptor layout is fixed: `kSrvCount` SRVs, `kUavCount` UAVs, one CBV.
Fill unused slots with a repeated valid resource.

### A new shader parameter

Add one line to `kShaderParams` in `compute/shader_params.h`. It appears
automatically in the UI, persists to settings, and is available in the
shader cbuffer at `gParams[i]`.

### A new debug view

Add one entry to `kDebugViews` in `debug_view.cpp`. The pipeline populates
`DebugSnapshot` (see `debug_view.h`); if the resource you want to display is
not in the snapshot, add a field for it and fill it in at pass time in
`PassSession::OnPresent` / `OnPostDlssEvaluate`.

### A new UI section

Use `PersistentHeader(label, settings.ui_*_open, changed)` so the section
respects the persisted open/closed state. Shared sections live in
`DrawOverlay`; per-pass sections live in `DrawPassSettingsBody`.

### A new setting

Add a field to `Settings` or `PassSettings`, a load/save pair in
`settings.cpp`, and a UI control. If it should be inheritable in later
passes, add the corresponding `inherit_*` field and handle it in
`RecomputeResolved`.

---

## Dependencies

- ReShade SDK
- Local `third_party/`:
  - Detours
  - NGX SDK headers
  - D3DX12 helper header
- `nvngx_dlssnr.dll` must be beside the game executable.

---

## Build

- Project: `reshade_dlssnr.vcxproj`
- Platform: `Release|x64`
- Toolset: `v145`
- Output: `reshade_dlssnr64.addon64`
- Link: `d3d12.lib;dxgi.lib;detours.lib;d3dcompiler.lib`
