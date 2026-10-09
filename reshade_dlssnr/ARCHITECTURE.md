# ReShade DLSSNR Addon

## Purpose

The addon runs NVIDIA DLSS Neural Rendering (NGX feature 18) through `nvngx_dlssnr.dll` on top of a game's existing DLSS Super Sampling path. It supports two injection points: **Post-DLSS**, where work is recorded inline on the game's command list immediately after the game's DLSS `EvaluateFeature`, and **Present**, where work is recorded on an addon-owned command list against the final swapchain backbuffer. The two paths share the same `PassSession` pipeline and differ mainly in their input resource, command-list ownership, and output publication point.

HDR is handled in the Post-DLSS path. The source is converted into a lossless SDR carrier plus a log2 gain map, the model operates on the SDR carrier, and the compose stage reconstructs the original HDR range and applies the model's residual directly in HDR. The final backbuffer grade can also operate in SDR, scRGB, or HDR10 according to the detected/selected format.

## Repository layout

The source tree is organized by ownership rather than by pipeline stage.

```text
reshade_dlssnr/
  addon.cpp                         ReShade addon entry points and event adapters
  reshade_dlssnr.sln                Visual Studio solution
  reshade_dlssnr.vcxproj            MSBuild project, shader-generation targets, linker settings
  shadertoolsconfig.json            HLSL editor include paths

  generated/
    shader_sources.h                Embedded shader-source table declaration
    shader_sources.cpp              Embedded shader-source table definition
    shader_bytecode.h               Optional embedded-bytecode table declaration
    shader_bytecode.cpp             Optional embedded-bytecode table definition/stub

  tools/
    generate_shaders.ps1            Generates embedded shader sources and the default bytecode stub
    generate_shaders.py             Equivalent source generator for tooling/CI use
    verify_shaders.ps1              FXC build-time shader verification
    precompile_shaders.ps1          Optional FXC bytecode generation

  shaders/common/
    params.h                        Shared C++/HLSL cbuffer layout
    cbuffer.hlsli                   Includes the shared cbuffer declaration
    bindings.hlsli                  Named aliases for cbuffer fields
    registers.hlsli                 Root-signature and register documentation
    color.hlsli                     Transfer functions, PQ, common decoding, gamut fitting
    color_constants.h               Shared C++/HLSL colorimetry constants (reference whites, PQ peak, model gamut fit)
    hdr_transfer.hlsli              BT.2446 forward/inverse, crosstalk, HDR/SDR carrier reconstruction
    hdr_local_exposure.hlsli        Shared local-exposure base/detail operator for split and exposure map
    luma.hlsli                      BT.709/BT.2020 luma weights
    math.hlsli                      Shared scalar/vector math helpers
    grade.hlsli                     Linear and SDR grade transforms
    fsr.hlsli                       FSR 1.0 EASU/RCAS implementation
    box_filter.hlsli                Exact footprint Box Area Average
    compose_common.hlsli            Residual shaping, ratio compose, YCbCr blend

  shaders/compose/
    compose_model_output.hlsl       SDR compose with FSR upsampling
    compose_model_output_hdr.hlsl   HDR compose with FSR upsampling and gain-map reconstruction
    compose_model_output_hdr_inverse.hlsl
                                    HDR inverse-tone-map compose with FSR upsampling and effective-exposure reconstruction
  shaders/debug/
    debug_display.hlsl              Debug-view renderer
    debug_hdr_roundtrip.hlsl        HDR round-trip, error, and local-exposure diagnostic renderer
  shaders/grade/
    apply_grade.hlsl                In-place color grade compute pass
  shaders/hdr/
    zero_histogram.hlsl             Clears the full-frame 256-bin histogram buffer
    build_histogram.hlsl            Builds the full-frame log2-luma histogram
    gain_map_split.hlsl             Builds the full-resolution SDR carrier + log2 gain map
    build_effective_local_exposure.hlsl
                                    Builds the full-resolution local SDR exposure scale for HDR inverse compose
    zero_gain_map_stats.hlsl        Clears gain-map diagnostic counters
    build_gain_map_stats.hlsl       Builds gain-map diagnostic statistics
  shaders/model/
    prepare_model_input.hlsl        Box-area resize into the model working buffer
  shaders/skin/
    skin_mask_diff.hlsl             Builds the temporal model-difference skin mask

  gpu/
    command_context.h               Command allocator/list/fence interface
    command_context.cpp             Command allocator/list/fence implementation
    compute_constants.h             C++ size/offset assertions for the shared shader cbuffer
    compute_engine.h                Compute engine interface and descriptor contract
    compute_engine.cpp              Root signature, shader compilation and dispatch
    compute_passes.h                Compute-pass registry declaration
    compute_passes.cpp              Compute-pass registry definitions
    d3d12_utils.h                   Resource-state inference fallback
    gpu_readback.h                  Fence-delayed GPU-to-CPU readback interface
    gpu_readback.cpp                Fence-delayed GPU-to-CPU readback implementation
    pass_resources.h                Per-pass resources and scratch ownership
    pass_resources.cpp              Per-pass resource creation and retirement
    shader_params.h                 Dynamic UI shader parameter table

  compose/
    compose.h                       Model residual composition interface
    compose.cpp                     SDR/HDR composition dispatch and uniforms
    settings.h                      Residual-edit settings structure
    settings.cpp                    Residual-edit persistence
  debug/
    debug_view.h                    Debug-view definitions and snapshot interface
    debug_view.cpp                  Debug-view selection and compute rendering
    frame_debug_context.h           Per-frame debug output container
    steps.cpp                       Pipeline registration for debug-view orchestration
  grade/
    grade.h                         Grade orchestration interface
    grade.cpp                       Grade dispatch and uniforms
    settings.h                      Grade settings structure
    settings.cpp                    Grade persistence
    steps.cpp                       Grade pipeline-step implementation
  hdr/
    hdr_state.h                     HDR measurement state and snapshot interface
    hdr_state.cpp                   Format detection, histogram, EMA and gain diagnostics
    settings.h                      HDR settings and format definitions
    settings.cpp                    HDR persistence
    steps.cpp                       HDR pipeline-step implementation
  hooks/
    d3d12_hooks.h                   D3D12 vtable hook interface
    d3d12_hooks.cpp                 D3D12 hooks and post-pass state restoration
    d3d12_state.h                   Captured command-list state
    d3d12_state.cpp                 Command-list state capture/restore implementation
    dlss_guides.h                   DLSS depth/motion/output guide ownership
    dlss_guides.cpp                 Guide capture, cloning and retirement
    local_vtable.h                  Lightweight vtable hook helpers
    nvngx_hook_compat.h             NGX CreateFeature/EvaluateFeature hook bridge
    proxy_unwrap.h                  ReShade/Streamline native-object unwrapping
    streamline_compat.h              Streamline interposer detection and unwrapping
  model/
    feature_lifecycle.h             Feature creation/rebuild lifetime
    feature_lifecycle.cpp           Full-resolution feature lifetime implementation
    model_input.h                   HDR model-input uniform definitions
    model_input.cpp                 Shared model input preparation helper
    model_pipeline.h                Model pipeline API surface
    ngx_model_params.h              Per-frame NGX tuning parameter interface
    ngx_model_params.cpp            Per-frame NGX tuning parameter writes
    ngx_parameter_block.h            Driver capability block interface
    ngx_parameter_block.cpp          Capability block access and float-slot probing
    runtime.h                       NGX runtime interface
    runtime.cpp                     nvngx_dlssnr.dll loading, import patching and initialization
  pipeline/
    addon_controller.h              Top-level owner and frame entry points
    addon_controller.cpp            Controller implementation and release-stage work
    crash_trace.h                   Raw Win32 first-chance crash reporting
    pass_session.h                  Per-pass frame/session interface
    pass_session.cpp                Pass step implementations and stage execution
    pipeline_graph.h                Stage/phase/step model and compile-time guards
    pipeline_graph.cpp              Stage tables and runner implementation
    pipeline_trace.h                Optional pipeline trace interface
    pipeline_trace.cpp              Trace writer and frame stream tracking
    status.h                        Runtime status snapshot definitions
  skin/
    settings.h                      Structure/skin detection settings
    settings.cpp                    Structure/skin detection persistence
    skin_detect.h                   Model-driven analysis mask interface
    skin_detect.cpp                 Model evaluation, exposure preparation and temporal mask generation
  state/
    pass.h                          Pass status/runtime support
    pass.cpp                        Pass state implementation
    settings.h                      Persistent settings and resolved inheritance
    settings.cpp                    Settings load/save/inheritance resolution
    trace.h                         Settings trace interface
    trace.cpp                       Settings change tracing
    version.h                       Addon version constant
  ui/
    ui.h                            Overlay entry point
    ui.cpp                          Main ImGui overlay and pass selection
    ui_debug.cpp                    Shared debug-view controls
    ui_grade.cpp                    Grade controls
    ui_hdr.cpp                      HDR controls and diagnostics
    ui_pass.cpp                     Per-pass controls
    ui_sections.h                   Section-drawer declarations
    ui_skin.cpp                     Structure/skin detection controls
    ui_status.cpp                   Status and retry controls
    ui_widgets.h                    Shared widget declarations
    ui_widgets.cpp                  Shared widget implementation

  third_party/
    reshade/                        ReShade SDK and ImGui source
    FidelityFX-FSR/                 FSR 1.0 source used by the addon/toolchain
    nvngx_dlss_sdk/                 NVIDIA NGX SDK headers
    d3dx/                           D3DX12 helper header
    detours/                        Detours headers and import library
```

`generated/` is build output produced from `shaders/`; it is not a place to edit shader source by hand.

## Runtime flow

There are two independent frame streams: the game's DLSS evaluate path and the ReShade present path. The trace system tracks them separately because a game can call DLSS more than once per presented frame.

### Addon startup

`AddonInit` registers the ReShade addon, loads persisted settings, installs the crash trace, registers `init_device` and `present`, registers the overlay, and attempts the NGX hook. `init_device` installs the early D3D12 hooks. `OnPresent` retries the NGX hook and D3D12 hook installation for a bounded number of presents because both NVIDIA modules and the device may not be ready at addon initialization.

### Post-DLSS injection

The NGX hook receives the game's `EvaluateFeature` call.

```text
Game -> NVSDK_NGX_D3D12_EvaluateFeature
  -> hooked_NVSDK_NGX_D3D12_EvaluateFeature
     -> real NGX EvaluateFeature
     -> resolve ReShade/Streamline command-list proxies to the native list
     -> identify the feature as DLSS Super Sampling
     -> DlssGuides::Capture(depth, motion, DLSS output color)
     -> AddonController::OnPostDlssEvaluate(native_list, output_resource)
        -> one PassSession::RunStage(PostDLSS) per enabled PostDLSS pass
```

The hook gives NGX exactly the command list the game supplied. The addon uses the unwrapped native interface only for its own D3D12 work. D3D12 state tracking is disabled while the real NGX call runs, then restored only if a Post-DLSS pass actually recorded work.

The Post-DLSS stage never closes, submits, or waits on the game's command list. It records its work inline and returns control to the game. Each pass writes its final result back to the supplied DLSS output resource so a later Post-DLSS pass can consume the previous pass's result.

The current Post-DLSS step order is:

```text
ResolveDevice
EnsureHdr
EnsureComputeEngine
HdrBeginFrame
SkinDetect
ResolveWorkingScale
EnsureFrameResources
SeedResourceStates
SeedGuideStates
ResolveModelInput
EnsureFeature
SnapshotGuides
BuildHdrUniforms
GainMapSplit
PrepareModelInput
GradeModelInput
FillEvaluateParams
EvaluateFeature
ModelOutputBarrier
ComposeModelOutput
GradeModelOutput
RenderDebugView
WriteToGameResource
```

`HdrBeginFrame` and `SkinDetect` are effectively first-pass operations even though they are present in the stage table for every Post-DLSS pass. The controller marks only the first enabled Post-DLSS pass as `is_first_pass`; subsequent passes reuse the HDR measurement state and persistent structure mask. Each exposure layer has a separate histogram population using that layer's enabled structure-mask, depth, and percentile filters. Because `HdrBeginFrame` precedes `SkinDetect`, the histogram uses the previous valid mask; carrier splitting later in the frame uses the newly produced mask.

### Present injection

At ReShade present:

```text
ReShade present
  -> AddonController::OnPresent(queue, swapchain)
     -> refresh backbuffer format/debug target format
     -> one PassSession::RunStage(Present) per enabled Present pass
     -> RunSingleStage(Release)
        -> GradeBackbuffer
        -> WriteDebugToBackbuffer
        -> SignalHdrAtPresent
     -> release retired resources
```

The Present stage runs on the session's private command list and owns its submit/wait tail. Its current step order is:

```text
AcquireBackbuffer
ResolveDevice
ResolveWorkingScale
EnsureFrameResources
SeedResourceStates
EnsureFeature
SnapshotGuides
EnsureComputeEngine
ResolveModelInput
PrepareModelInput
GradeModelInput
FillEvaluateParams
EvaluateFeature
ModelOutputBarrier
ComposeModelOutput
GradeModelOutput
RenderDebugView
WriteToBackbuffer
SubmitAndWait
```

The Release stage uses a controller-owned command list because it runs after the normal pass stage and has no pass session to own that list. Its order is deliberately fixed:

```text
GradeBackbuffer
WriteDebugToBackbuffer
SignalHdrAtPresent
```

`GradeBackbuffer` must return the swapchain image to `PRESENT` even when grading fails. The debug copy happens after the grade so a debug view replaces the backbuffer contents rather than showing a second graded image.

## Shader build and dispatch

### Source hierarchy and embedding

Every compute root is an on-disk `.hlsl` file under `shaders/`. Shared functions and data live in `.hlsli` or the shared C/C++ layout header under `shaders/common/`.

Includes are written relative to `shaders/`, for example:

```hlsl
#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
```

Root `.hlsl` files use shader-root-relative includes such as `common/color.hlsli`. Includes inside a shared `.hlsli` or `.h` may use sibling-relative paths such as `"color.hlsli"` or `"params.h"`; nested includes are resolved relative to the including file first, then relative to `shaders/`. Shared headers include the other headers they depend on, and the order of each include directive is preserved.

`tools/generate_shaders.ps1` and `tools/generate_shaders.py` embed every `.hlsl`, `.hlsli`, and `.h` file into `generated/shader_sources.cpp`. For each runtime `.hlsl` root, generation recursively expands local includes at the point where they occur, honors `#pragma once`, detects missing includes and unguarded include cycles, and adds `#line` markers so diagnostics can identify the original source file. Authored `.hlsli` and `.h` entries remain embedded in their original form for include-handler fallback. FXC verification and optional bytecode precompilation still compile the on-disk roots and use the same nested include layout. Generated files should never be hand-edited.

`shadertoolsconfig.json` supplies `shaders` and `shaders/common` as HLSL editor include directories.

### Runtime compilation

`ComputePassDef` is the shader registry contract:

```cpp
struct ComputePassDef {
  const char* name;
  const char* root;
  const unsigned char* bytecode;
  std::size_t bytecode_size;
};
```

`kComputePasses[]` maps the trace/dispatch name to the root shader path. `ComputeEngine::CompilePipeline` uses embedded bytecode when it is present; otherwise it resolves the root from the embedded source table and calls `D3DCompile` with:

- source = embedded root source
- source name = the root path, so compiler diagnostics identify the shader file
- include handler = `EmbeddedInclude`, which resolves both sibling-relative and shader-root-relative paths when a compile-time include is needed
- entry point = `CSMain`
- target = `cs_5_1`
- optimization = `D3DCOMPILE_OPTIMIZATION_LEVEL3`

Runtime compilation is the default. `PrecompileShaders=false` keeps the generated bytecode table as an empty-count sentinel. Setting `PrecompileShaders=true` runs `tools/precompile_shaders.ps1`, generates `.cso` data and rewrites `generated/shader_bytecode.cpp`, and `CompilePipeline` uses those bytes instead of calling `D3DCompile`.

### Build-time verification

`VerifyShaders=true` is the default. `tools/verify_shaders.ps1` locates `fxc.exe` automatically from the installed Windows SDK when `FxcPath` is not supplied, compiles every `.hlsl` root as `cs_5_1`, and forwards actual compiler warnings/errors. It writes objects to a temporary directory and deletes them after each verification pass. Successful FXC object-save messages are intentionally not part of the build output.

The project selects the latest installed Windows 10 SDK using `ToolLocationHelper::GetLatestSDKTargetPlatformVersion('Windows', '10.0')`. The `FxcPath` property is derived from `WindowsSdkDir` and that SDK version after the normal MSVC property import; it remains overrideable from MSBuild.

### Root signature and descriptor contract

`ComputeEngine::CreateRootSignature` is the single root-signature definition for all compute passes. It has one descriptor table containing:

| Resource | Registers | Count |
|---|---|---:|
| SRV | `t0..t3` | 4 |
| UAV | `u0` | 1 |
| CBV | `b0` | 1 |
| Static sampler | `s0` | 1 |

The static sampler is linear-filtered and clamp-addressed. There is no `s1`.

Every `Dispatch` provides exactly `kSrvCount` SRVs and `kUavCount` UAVs. A pass that uses fewer resources repeats a valid resource in unused descriptor slots. `BindAndDispatch` owns the descriptor and constant-buffer ring mechanics; individual passes only choose the resources and constants.

The descriptor table shape and register assignment are fixed. A shader must not invent a new register or sampler contract locally.

### Shared cbuffer

`shaders/common/params.h` is shared between HLSL and C++. `gpu/compute_constants.h` checks the C++ side of the layout.

The 256-byte CBV is:

| Offset | Block | Size |
|---:|---|---:|
| 0 | `Width`, `Height`, header padding | 16 |
| 16 | `ShaderBlock` | 32 |
| 48 | `GradeBlock` | 48 |
| 96 | `ResidualBlock` | 32 |
| 128 | `HdrBlock` | 48 |
| 176 | `EasuBlock` | 48 |
| 224 | `RcasBlock` | 16 |
| 240 | alignment padding | 16 |

The resulting `ComputeConstants` size is exactly 256 bytes, matching the CBV stride hardcoded by `BindAndDispatch`.

The shader-side aliases in `bindings.hlsli` turn fields such as `hdr.Exposure` and `easu.Con0` into the existing semantic names used by shader code (`kExposure`, `kEasuCon0`, and so on). The dynamic UI shader parameters remain defined by `gpu/shader_params.h`; the table is copied into `ShaderBlock.Params0` and `Params1`.

The layout intentionally avoids `float2`, `float3`, `packoffset`, platform headers and HLSL-only utility code in `params.h`. Every nested block is explicitly 16-byte aligned and padded.

### Shared filtering and reconstruction

The model input downsample is `BoxAreaAverage` from `shaders/common/box_filter.hlsli`. It uses the exact source footprint covered by each destination pixel, including fractional edge coverage, and fetches with `Texture2D.Load`. It does not rely on the global linear sampler.

The non-HDR `PrepareModelInput` path therefore treats the source as already in the model's target sRGB space and passes the averaged RGB through unchanged. The HDR carrier path averages the linear carrier, applies the reversible model highlight/gamut fit, and applies `LinearToSrgb` before the model sees it. Values at or below 0.90 remain unchanged. The fit maps the bright SDR range and super-whites up to a linear carrier value of 1.5 into a logarithmic headroom region below 1.0 to preserve more distinct values in the FP16 model texture. Higher carrier values are scaled into that supported model range while the packed HDR carrier remains untouched. The inverse clamps EASU/RCAS overshoot to the supported ceiling with a neutral channel scale, avoiding a spurious HDR spike or highlight hue shift.

Both SDR and HDR compose paths use the same FSR 1.0 EASU and optional RCAS implementation from `shaders/common/fsr.hlsli` to reconstruct full resolution from the working-scale model images. The EASU constants and RCAS controls come from the shared cbuffer.

The common helpers are also consolidated. `math.hlsli` owns the single `safe_log`/`safe_log3` implementation; `color.hlsli` owns `NitsToPq` and `DecodeToLinear`; and `color_constants.h` is the shared C++/HLSL source of truth for color-domain constants.

## The pipeline graph

`pipeline/pipeline_graph.h/.cpp` defines the declarative frame pipeline. A stage is an ordered list of step nodes grouped into phases. A step definition supplies its node id, name, implementation function, prerequisites, required inputs, and whether it is HDR-aware.

### Node ordering

`PipelineNodeId` is a single enum ordered by increasing pipeline dependency depth. A step may declare only prerequisites that occur earlier in the stage list. The ordering is load-bearing because the prerequisite checks are based on completed node ids.

The runner does not infer ordering from function dependencies. The step table is the source of truth for execution order.

### Stage contracts

The three current stages are:

| Stage | Injection | Submit owner | HDR capable | Required caller inputs |
|---|---|---|---|---|
| `PostDLSS` | NGX `EvaluateFeature` hook | game owns submit | yes | command list, output resource, stage |
| `Present` | ReShade present | pass session | no | swapchain, command queue, stage |
| `Release` | controller-owned post-present work | controller | no | swapchain, command queue, stage |

`FailurePolicy` is stage-level policy for unhandled non-OK step results: `Fail`, `Transient`, or `Ignore`.

### Graph invariants

- `NodeListsEqual` guarantees the stage step list in `pipeline_graph.cpp` matches the corresponding node list published in `pipeline_graph.h`.
- `PrerequisitesPrecedeSteps` rejects a prerequisite that is listed after its dependent step.
- `NoHdrAwareSteps` rejects an HDR-only step in a non-HDR-capable stage.
- `StageInputsCoverSteps` proves that every stage declaration covers the inputs claimed by the steps it contains.
- `PhasesCover` requires each stage's phase ranges to tile its step list with no gap or overlap.
- The runtime runner also checks `StageInputsMet` against the live `PassRun` before executing a stage.
- `required_inputs` documents reads, not writes. A step that creates `run.device` or `run.backbuffer` must not declare those as incoming prerequisites.
- The compiler cannot inspect arbitrary function bodies to discover missing `required_inputs`; a forgotten declaration remains the weak point of that safety mechanism.
- The static checks validate coverage and prerequisites, not semantic ordering beyond declared prerequisites. Reordering a table still requires reading the resulting sequence as a whole.

Pipeline step definitions can live in the file that owns their concern. `pass_session.cpp` owns most steps; `hdr_state.cpp`, `skin_detect.cpp`, and `addon_controller.cpp` contribute their own registrations. `FindPipelineStep` builds a node-indexed registry and detects duplicate ownership or a definition whose id does not match the lookup slot.

## Pipeline invariants and hard-won rules

### D3D12 / GPU

**Own every replaced resource until the GPU is finished with it.** `PassResources` transfers replaced resources into the retire callback rather than releasing them immediately; the callback is drained after the appropriate queue synchronization. Direct `ComPtr::Reset()` on an in-flight resource is unsafe.

**Seed states for resources the addon owns.** `InferResourceState` is only a fallback for resources whose exact state is not otherwise known. Pass-owned resources are seeded at frame boundaries and cleaned back to their expected states before submission.

**The NGX model output needs a UAV barrier before the addon reads it.** A state transition is not sufficient to synchronize the external UAV write performed by NGX.

**Post-DLSS work must stay on the game's command list and must not submit it.** The addon records inline and lets the game's queue submit that work.

**Present/Release command contexts own their own fence.** `CommandContext::WaitGpu` is the synchronization point for addon-owned lists and for deferred resource release that depends on them.

**Abandoned Present recordings must be closed before allocator reuse.** `CommandContext::Abort` closes a recording that exited before the normal `SubmitAndWait` tail; `Reset` must never reset an allocator while its command list is still recording.

**The descriptor/constant ring cannot be reused ahead of the GPU.** Descriptor views and the 256-byte constant slot are written by the CPU while commands are recorded but consumed later by the GPU. The ring depth therefore has to cover the maximum CPU lead and worst-case dispatch count.

**Always return the swapchain image to `PRESENT`.** `GradeBackbuffer` keeps that transition even when grading fails, because leaving the backbuffer in an intermediate state turns a grade error into a later present failure.

**The debug layer's object chain is not a safe assumption for the vtable hooks.** The addon hooks native D3D12 interfaces; SDK-layer or proxy vtables can point at different thunk implementations than the hooks expect.

### Streamline

**Do not record addon-owned work through a Streamline proxy.** Resolve the ReShade wrapper and Streamline interposer to the native object before the addon creates resources, records barriers, or restores state.

**Pass the game's command list to NGX unchanged.** Proxy unwrapping is for the addon's own work; the command list handed back to Streamline/NGX remains the one the host supplied.

**Track state using the same native command-list identity that the hooks see.** A wrapper pointer and the underlying native list are different keys; mixing them makes `RestoreCommandList` silently miss the captured state.

**Keep references to state objects used for restoration.** Root signatures, PSOs, descriptor heaps and related objects can be released and recreated by the game, especially around swapchain changes; storing raw pointers is unsafe.

**The NGX capability parameter block is process-global.** Do not call `Reset()` on it to clear only DLSSNR keys; Streamline may share the same block. The addon writes the keys it owns without clearing the entire object.

### NGX / feature lifecycle

**Hook NGX before the first DLSSNR `CreateFeature`.** `TryHookNgx()` loads `nvngx_dlss.dll` if necessary so the hook is in place before feature creation. `CreateFeature` records handle-to-feature-id mappings used by `EvaluateFeature`.

**The DLSSNR feature is rebuilt only when its full-resolution build size changes.** Working scale changes only the per-frame evaluation size; they do not recreate the feature.

**Model tuning is applied at feature creation and again for evaluation.** Preset, intensity, local structure/tone, skin structure, auto-mask and UI-correction controls are not feature-rebuild triggers.

**The feature creation frame is skipped for evaluation.** `FeatureLifecycle::ConsumeJustRebuilt()` gives the caller one frame to let a new feature initialize before the next evaluation.

**The model's build dimensions are full-frame dimensions.** `working_scale` belongs to the model's per-frame input/output resolution and NGX evaluation parameters, not feature creation.

**NGX feature creation is feature 18.** The addon uses the Super Sampling feature id for DLSSNR and maintains its own parameter keys under the `DLSSNR.*` namespace.

### Model loading and parameter block

`model/runtime.cpp` owns direct loading of `nvngx_dlssnr.dll`, patches the model module's import of `KERNEL32!GetModuleFileNameW` to satisfy its caller check, resolves `NVSDK_NGX_D3D12_Init_Ext`, `CreateFeature`, `EvaluateFeature`, and `ReleaseFeature`, then initializes the model against the addon module directory.

`NgxParameterBlock` obtains the driver NGX capability parameter block from `_nvngx.dll`. The float setter slot is probed from candidate vtable slots at runtime because the binary interface does not expose a stable named C++ method for the required setter. The block is not reset or cleared wholesale.

### HLSL

**Keep `groupshared` variables at global scope.** HLSL rejects function-local `groupshared` declarations.

**Avoid reserved or ambiguous identifiers.** HLSL's parser can report misleading errors for names that collide with language keywords or intrinsics.

**Be explicit with color types.** Functions that operate on colors take and return `float3`; do not rely on HLSL scalar-to-vector coercion.

**Use explicit matrix/vector multiplication.** `mul(matrix, vector)` expresses the intended linear algebra; componentwise operators do not.

**Keep shared cbuffer math-free.** `shaders/common/params.h` is a layout contract for two languages, not a utility header.

**Share local-exposure math between split and compose support.** `shaders/common/hdr_local_exposure.hlsli` is the single implementation of the local-exposure field calculation used by both the HDR gain-map split and the full-resolution exposure map. This prevents the map from drifting away from the exposure field that produced the model input.

**Include shared helpers before using their aliases.** `bindings.hlsli` assumes the `params.h` field names exist. `color.hlsli`, `grade.hlsli`, and other helper files should only assume the functions they explicitly include or receive through earlier root includes.

### HDR

**Format detection is explicit and overridable.** Auto detection treats `R16G16B16A16_FLOAT` and `R11G11B10_FLOAT` as scRGB, `R10G10B10A2_UNORM` as HDR10, and 8-bit RGBA/BGRA formats as SDR. `HdrSettings::format` can override auto detection.

**Exposure is measurement state, not compose state.** Histogram-derived exposure changes what the model sees; HDR compose reconstructs the source from its own gain map and applies the model residual directly in HDR.

**Game Exposure, Adaptive Exposure, and Local Exposure are independent stackable layers.** Game Exposure samples the game's NGX `ExposureTexture` before `EvaluateFeature` and uses asynchronous GPU readback. Its scalar remains the global anchor, with an optional manual bias and optional bounded adaptive tuning; unavailable game exposure falls back to Adaptive Exposure when that layer is enabled. Adaptive Exposure independently tracks a target from its own filtered histogram and can apply a clamped, adjustable correction over the game value. Local Exposure applies the base/detail spatial operator after the global transfer. The normal layer order is Game base, Adaptive correction, then Local spatial correction.

**Every layer owns its filter combination.** Structure Mask, depth interval, and percentile range can be enabled independently for each layer. Game-layer filters affect the optional bounded-tuning population because the game's exposure scalar itself is not spatially metered. Adaptive filters select the population used to estimate its exposure target. Local filters affect where local correction is applied: the structure mask weights correction spatially, depth restricts correction to an interval, and percentile limits are derived from that layer's independently filtered luminance histogram. If a required resource is unavailable, the corresponding filter is skipped. Percentile range on Game/Adaptive controls the histogram band used for the target; on Local it defines a softly feathered luminance band.

**Adaptive Exposure uses a dedicated histogram population.** Full-frame, Game tuning, Adaptive, and Local populations are accumulated separately in the same measurement dispatch sequence. Mask-conditioned histogram weight follows the persistent mask; depth is normalized to a selected near/far interval; percentile ranges are applied to the population on CPU when estimating the histogram target. If an available filter combination selects no pixels, that layer holds its last valid estimate instead of silently reverting to full-frame metering. The mask is intentionally one-frame-latched for `HdrBeginFrame` because it is produced later in the stage, while Local Exposure uses the current pass's available mask.

**Local exposure is part of the HDR round-trip contract.** GainMapSplit applies the base/detail adjustment after the global BT.2446 Method C mapping. BuildEffectiveLocalExposure records the exact per-pixel local scale used by that transform, with the same filter settings. Inverse Tone Map reuses that map rather than re-estimating local exposure from the edited image.


**HDR round-trip diagnostic.** The debug menu also exposes `HDR Roundtrip Diagnostic`, `HDR Roundtrip Error`, and `HDR Local Exposure`. These are test-only views for the HDR compose path. With local exposure enabled, the round-trip path uses the same full-resolution exposure map as inverse compose. The diagnostic applies the same EASU and RCAS processing to its model endpoints as composition, then compares the model output with the composed HDR converted back through the same HDR-to-SDR transform. For a clean transfer/gamut-fit test, disable input/output grades and RCAS and use working scale 1.0. At lower working scales, edge error also includes high-frequency full-resolution source detail that compose deliberately preserves but the upsampled model endpoint cannot represent; RCAS changes that comparison because it sharpens the model endpoints.

**Render debug images at pass time.** The resource state is trustworthy while the producing pass still owns the command list and has just produced the resource. `MaybeRenderDebug` renders into `FrameDebugContext::output`; the Release stage only copies that already-rendered output to the backbuffer.

**The debug target uses the swapchain's format.** Post-DLSS sources can be HDR, but the final debug copy targets the actual backbuffer format.

**Debug selection is global, rendering is pass-local.** `Settings::debug_view` is shared, while only the active pass renders the selected view.

### Settings and UI

**Inheritance is resolved only when settings change.** `RecomputeResolved()` runs on load and save; runtime code reads `GetResolvedPass()` directly and never recomputes inheritance per frame.

**Only explicitly inheritable fields are resolved.** Working scale, intensity, local tone, local structure, skin structure and dynamic shader parameters can inherit. Grade settings, residual settings and RCAS controls are intentionally per-pass.

**HDR composition is a per-pass UI setting.** The Residual Edit section exposes `HDR Composition` with `Ratio (Current)` and `Inverse Tone Map (Scene-Referred)`. The choice is persisted as `PassN.Residual.HdrComposeMode` and only affects HDR composition; the default remains Ratio. Legacy value `2` is migrated to Inverse Tone Map.

**HDR exposure is configured as a layer stack.** The HDR section exposes tabbed `Game Exposure`, `Adaptive Exposure`, and `Local Exposure` layers, each with its own enable state, parameters, and independently configured combination of Structure Mask, depth interval, and percentile-range filters. Settings are persisted under `Hdr.Layer.Game`, `Hdr.Layer.Adaptive`, and `Hdr.Layer.Local`. Existing exclusive exposure settings are migrated to the closest current layer-stack setup; the old Median implementation is not retained as a runtime mode. A fresh configuration keeps Local Exposure disabled until selected so Game Exposure remains unchanged by default.

**Structure Detection uses the same resolved HDR exposure stack.** The detection input follows the current Game/Adaptive/Local settings, including Local filter configuration. Its mask-based filter uses the previous mask during mask generation for temporal stability. Dedicated detector exposure methods are no longer exposed or tracked.

**Once the first Present pass exists, later passes are forced to Present.** This keeps the pass chain monotonic: Post-DLSS can feed later Post-DLSS work, while a Present pass cannot hand its output back into the earlier DLSS injection point.

**The debug selection is not per-pass.** The active UI tab selects which pass renders the shared debug view.

**Persisted section state is shared unless explicitly per-pass.** `PersistentHeader` owns the common open/closed UI state for the overlay sections.

## HDR model path

The HDR path is gain-map based and exists only in Post-DLSS processing.

1. `HdrState` detects the source format and resolves the exposure layer stack. Game Exposure uses the game's NGX exposure scalar with optional bounded tuning; when unavailable or disabled, Adaptive Exposure can supply the base. The independent layer histograms use each layer's own structure-mask, depth, and percentile settings. Game exposure keeps scene white at 1.0.
2. `SkinDetect` (shown as `Structure Detection` in the UI), when enabled, prepares a working-scale model input using the same resolved exposure stack and Local Exposure filters as the main path, evaluates a dedicated model feature built at full output resolution, and builds a temporal mask from the input/output difference. The `Skin Range` target excludes pixels near black or clipped white; it is a range gate, not a skin-hue classifier. `All Structure` accepts any detected model edit, including dark and clipped-highlight pixels. These target modes control mask eligibility and are separate from the individual model-strength sliders. The detection pass has three additional compute dispatches in its HDR worst case, and the descriptor/constant ring is sized for those dispatches in addition to the normal frame budget.
3. `GainMapSplit` converts the full-resolution HDR source into an RGBA32F carrier: RGB is the linear SDR base and A is log2 gain. BT.2446-C parameters, gain-map offsets and scene-white control the split.
4. `PrepareModelInput` performs exact Box Area downsampling. For the HDR carrier path it applies the reversible model highlight/gamut fit and sRGB encoding. The packed carrier is not modified.
5. The model feature evaluates at the selected working scale while being built at full frame resolution.
6. `ComposeModelOutputHdr` uses FSR EASU and optional RCAS to reconstruct the model's working-scale images at full resolution. The default `Ratio` mode uses the existing linear-SDR ratio transfer. `Inverse Tone Map` selects `ComposeModelOutputHdrInverse`, which inverts the same BT.2446 transfer used by the HDR split. When local exposure is enabled, it uses the full-resolution local SDR exposure map; otherwise it falls back to the global/packed-carrier exposure recovery. It reconstructs both model endpoints in scene-referred HDR, transfers the model-output-minus-model-input edit onto the original full-resolution HDR, and therefore preserves source detail when Working Scale is below 1.0.
7. `EncodeHdr` writes the reconstructed result back into scRGB or HDR10 as required by the source format. The inverse compose path first reverses the model highlight/gamut fit so super-white model edits can reconstruct their HDR magnitude.

The SDR compose uses the same FSR path but does not reconstruct an HDR carrier; it applies the residual in the source SDR space and uses the shared grade/compose helpers.

## Logging, tracing, status, and crash trace

### Log

The ReShade log is for lifecycle events and actionable failures: NGX hook installation, format changes, feature creation, shader compilation diagnostics, pass failure reasons and unrecoverable command-context errors.

Shader diagnostics are identified by pass name, root shader path and severity. Compiler warnings/errors are deduplicated by pass + severity + exact diagnostic text, so background compilation and a post-ready fallback compile do not flood the log with the same message.

Pass failures are represented by `PassSession::Fail()` and surfaced through the status snapshot. Startup conditions such as the compute worker not being ready are skipped rather than reported as shader failures.

### Status

`StatusSnapshot` is the UI-facing current state: addon enabled state, runtime initialization state, guide availability, HDR/gain-map measurements, post-DLSS block reason, and per-pass runtime status. It is refreshed by `AddonController` after frame processing.

Use Status for information that must remain visible as current state. Do not replace it with a per-frame log line.

### Pipeline trace

Set `DLSSNR_TRACE=<path>` to enable the command/event trace. The trace maintains separate counters for `PostDLSS` and `Present` streams, assigns stable resource ids in first-seen order, and records frame boundaries. `TRACE_CMD` covers resource transitions, barriers, copies and dispatches; `TRACE_EVT` records control-flow decisions such as skips and failures.

Because trace lines are intentionally flushed immediately, the file is suitable for diagnosing the final commands leading into a crash. The trace should still be bounded to a deterministic test scene because a full frame contains many transitions and dispatches.

Settings changes are recorded separately as `cfg` lines when tracing is enabled. Load-time state is emitted as `init`; changes at save time are emitted as `set` transitions. Resolved pass values are represented separately from raw inherited settings.

### Crash trace

`crash_trace.h` installs a vectored exception handler and a terminate handler at addon attach time. It writes `%TEMP%\dlssnr_crash.txt`, including fault code, faulting module and offset, a short stack, x64 register context and thread id. The handler is deliberately raw Win32 and allocation-free enough for a broken process, and it chains to the game's own exception handling.

## Extending

### Add a shader pass

1. Add one root `.hlsl` under the appropriate `shaders/<module>/` directory with one `CSMain`.
2. Include the shared headers it actually needs (`common/cbuffer.hlsli`, `common/bindings.hlsli`, `common/registers.hlsli`, plus functional helpers such as `color.hlsli` or `fsr.hlsli`).
3. Add the root path to `kComputePasses[]` in `gpu/compute_passes.cpp`.
4. Register the pass in the appropriate pipeline step if it is part of frame orchestration.
5. Bind exactly three SRVs and one UAV at the call site; repeat unused SRV resources.
6. Use the shared `ComputeConstants` block for common parameters. If the pass truly needs a private parameter convention, document it locally rather than changing the global layout casually.

The build automatically embeds the new shader and the verification target compiles it before C++ compilation.

### Add a shader parameter

1. Add an entry to `gpu/shader_params.h`.
2. Add or reuse the corresponding field in `ShaderBlock` in `shaders/common/params.h` only when the parameter belongs to the shared cbuffer rather than a pass-specific constant convention.
3. Write the value through the typed cbuffer field instead of a numeric slot offset.
4. Add the HLSL alias in `shaders/common/bindings.hlsli` if the shader-facing semantic name is not already present.
5. Keep the persistent `kShaderParamCapacity` limit in `state/settings.h` large enough for the UI/settings arrays.

The dynamic table is copied contiguously into `ShaderBlock.Params0/Params1`; `gpu/compute_constants.h` asserts that the table fits in those eight floats.

### Add a debug view

1. Add a selector to `kDebugViews` in `debug/debug_view.cpp`.
2. If the resource is not already in `DebugSnapshot`, add it to `debug/debug_view.h` and populate it from the step that produces the resource.
3. Keep the rendering on the producing pass's command list; do not defer the shader work to Present.

### Add a pipeline step

1. Add a `PipelineNodeId` entry in depth order.
2. Add a step function in the owner (`PassSession`, `AddonController`, `HdrState`, `SkinDetect`, etc.).
3. Register a `PipelineStepDef` in that owner's registry.
4. Add the node to the appropriate stage table and mirror list.
5. Declare every prerequisite in `depends_on` and place the prerequisite earlier in the stage.
6. Declare every frame input the body reads in `required_inputs`; do not declare values the step itself creates.
7. Update the stage phase ranges.
8. Re-read the final stage order; the compile-time guards do not prove semantic ordering that has not been expressed as a prerequisite.

### Add a UI section

Use the existing section helpers and persistent settings fields. Shared sections belong in the overlay-level drawing flow; per-pass controls belong in `ui_pass.cpp` and operate on the selected `PassSettings`/resolved pass values.

### Add a setting

1. Add the field to `Settings` or `PassSettings`.
2. Add load/save handling in `state/settings.cpp`.
3. Add the UI control in the relevant `ui_*.cpp` file.
4. If later passes may inherit it, add the corresponding inherit flag and handle it in `RecomputeResolved`.
5. If the setting is runtime-only UI state, keep it out of the frame-processing layer.

## Known gaps

**Game-owned resource states are inferred, not observed.** `D3D12Hooks` already sees the game's resource barriers, but the addon currently does not maintain a complete watched-resource state map for every game-owned input. `InferResourceState` is therefore an approximation for external resources such as DLSS output, depth and motion vectors. A sufficiently wrong state guess can produce an invalid barrier or a debug-layer before/after mismatch.

**Shader register compatibility is a convention, not a separate static analyzer.** The root signature and the HLSL register documentation agree by construction, but the project does not have a compile-time C++/HLSL reflection check that proves every shader declaration matches the descriptor counts. FXC and D3D12 pipeline creation remain the enforcement points.

**The optional precompiled shader path is not the default path.** Runtime `D3DCompile` is still the authoritative shipping path unless `PrecompileShaders=true` is explicitly enabled. That means runtime shader source, include resolution and compiler availability remain part of normal deployment.

## Reported but not investigated

These are retained as user-reported symptoms whose root causes are not established by the current code review.

**Occasional debug-view frames are skipped.** A selected debug view can be absent on an intermittent frame after reconfiguration. `RenderDebugView` intentionally returns without rendering when the codec is not ready, the selected source is unavailable, dimensions are zero, or the debug target has been recreated; the symptom may therefore be a benign skipped render, a resource-lifecycle problem, or a ring/resource reuse issue. It has not been conclusively isolated.

**Model-output grading can be too strong in HDR.** Backbuffer grading is separate and currently behaves differently from grading the model output inside the HDR compose path. The exact intended grade space for the model-output boundary in HDR has not been established as a user-visible contract and should be treated carefully before changing it.

## Build

The project is a Visual Studio x64 DLL/addon build.

- Solution: `reshade_dlssnr.sln`
- Project: `reshade_dlssnr.vcxproj`
- Configurations: `Debug|x64`, `Release|x64`
- Platform toolset: `v145`
- C++ standard: C++20
- Release output: `build\Release\reshade_dlssnr64.addon64`
- Debug output: `build\Debug\reshade_dlssnr64.addon64`
- Linked system/local libraries: `d3d12.lib`, `dxgi.lib`, `detours.lib`, `d3dcompiler.lib`

MSBuild shader properties:

| Property | Default | Meaning |
|---|---|---|
| `VerifyShaders` | `true` | Run FXC verification before the C++ compile |
| `PrecompileShaders` | `false` | Generate embedded shader bytecode instead of using the empty bytecode table |
| `FxcPath` | auto-detected | Optional explicit path to x64 `fxc.exe` |

The shader target order is:

```text
GenerateEmbeddedShaders
        |
        +--> VerifyShaders
        |        |
        |        +--> PrecompileShaders (only when PrecompileShaders=true)
        |
        +--> C++ compilation
```

The build selects the latest installed Windows 10 SDK with `ToolLocationHelper`. `verify_shaders.ps1` can independently locate `fxc.exe` from `WindowsSdkDir`/`WindowsSdkVerBinPath` when `FxcPath` is not passed explicitly.

The project expects the ReShade SDK/ImGui sources, FidelityFX-FSR sources, NVIDIA NGX headers and the local Detours/D3DX12 dependencies under `third_party/`. A clean checkout needs those repository dependencies present before building.
