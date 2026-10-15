# ReShade DLSSNR Addon

## Purpose

The addon runs NVIDIA DLSS Neural Rendering (NGX feature 18) through `nvngx_dlssnr.dll` on top of a game's existing DLSS Super Sampling path. Each pass independently targets one of three injection points: **Pre-DLSS**, where work runs on the game's `Color` input before its real DLSS `EvaluateFeature`; **Post-DLSS**, where work runs inline immediately after the real evaluation on the upscaled output; or **Backbuffer**, where work runs on an addon-owned command list against the final swapchain image. The stages share the same `PassSession` pipeline and differ mainly in input resource, command-list ownership, and output publication point.

HDR is handled by the Pre-DLSS and Post-DLSS paths. The source is converted into an SDR carrier plus a full-precision carrier-to-HDR reconstruction multiplier, and the model operates on the SDR carrier. Composition reads the untouched HDR source directly and applies the model residual in HDR. The final backbuffer grade can also operate in SDR, scRGB, or HDR10 according to the detected/selected format.

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
    expanded_shaders/                Pragma-free, fully expanded FXC inputs (generated)

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
    hdr_local_exposure.hlsli        Shared block-aware, depth-continuous local exposure operator
    luma.hlsli                      BT.709/BT.2020 luma weights
    math.hlsli                      Shared scalar/vector math helpers
    grade.hlsli                     Linear and SDR grade transforms
    fsr.hlsli                       FSR 1.0 EASU/RCAS implementation
    box_filter.hlsli                Exact footprint Box Area Average
    compose_common.hlsli            Residual shaping, ratio compose, YCbCr blend

  shaders/compose/
    compose_model_output.hlsl       SDR compose with FSR upsampling
    compose_model_output_hdr.hlsl   Gain-Map Residual HDR compose with FSR upsampling and source-preserving HDR composition
    compose_model_output_hdr_inverse.hlsl
                                    Retained HDR inverse-compose shader root (verified, not registered for runtime dispatch)
  shaders/debug/
    debug_display.hlsl              Debug-view renderer
    debug_hdr_roundtrip.hlsl        HDR round-trip, error, and local-exposure diagnostic renderer
  shaders/grade/
    apply_grade.hlsl                In-place color grade compute pass
  shaders/hdr/
    zero_histogram.hlsl             Clears the full-frame 256-bin histogram buffer
    build_histogram.hlsl            Builds the full-frame log2-luma histogram
    gain_map_split.hlsl             Builds the full-resolution SDR carrier + HDR multiplier
    build_effective_local_exposure.hlsl
                                    Builds the full-resolution local SDR exposure scale for HDR diagnostics
    zero_gain_map_stats.hlsl        Clears gain-map diagnostic counters
    build_gain_map_stats.hlsl       Builds gain-map diagnostic statistics
    read_game_exposure.hlsl         Reads the game's 1x1 NGX exposure texture asynchronously
  shaders/model/
    prepare_model_input.hlsl        Box-area resize into the model working buffer
  shaders/importance_mask/
    build_importance_mask.hlsl             Builds the per-frame Importance Mask from model input/output difference
  shaders/temporal/
    extract_edit.hlsl               Extracts the current model's multiplicative edit
    reproject_edit.hlsl             Motion-reprojects the previous edit and tests photometric agreement
    validate_depth.hlsl              Rejects reprojections across depth discontinuities
    count_invalid.hlsl               Counts rejected/mismatched pixels for asynchronous reuse policy
    apply_edit.hlsl                  Applies a validated edit to the current model input
    commit_edit.hlsl                 Publishes the edit used as next-frame history
    store_depth.hlsl                 Stores the current depth history
    clear_stats.hlsl                 Clears temporal feedback counters

  ctl/                             Declarative settings framework
    ctl.h                           Framework umbrella header
    types.h/.cpp                   Values and setting metadata
    schema.h/.cpp                  Section, setting, hook, and schema declarations
    registry.h/.cpp                Setting-type registry
    values.h/.cpp                  Instance store
    serialize.h/.cpp               Schema-driven persistence
    draw.h/.cpp                    Generic section renderer
    builtin_settings.h/.cpp        Float, Int, Bool, Enum, and String settings
    usage/                          ReShade config and UI-store adapters
    ctl_acceptance.cpp              Headless acceptance harness
  feature/                         Feature-owned setting schemas and typed views
    register_all.cpp                Central feature registration
    preferences/                    Shared preferences schema and setting keys
    debug_log/                      Debug selection schema and view
    grade/                          Grade schema and view
    hdr/                            HDR schema, view, and UI hooks
    importance_mask/                Importance Mask schema and view
    pass/                           Per-pass schema, stable IDs, order, and view
  fragments/                       Reusable schema row fragments
    grade_rows/                     Shared grade declarations
    hdr_filter_rows/                Shared HDR filter declarations

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

  compose/
    compose.h                       Model residual composition interface
    compose.cpp                     SDR/HDR composition dispatch and uniforms
    settings.h                      Residual-edit and reconstruction settings structures
  debug/
    debug_view.h                    Debug-view definitions and snapshot interface
    debug_view.cpp                  Debug-view selection and compute rendering
    frame_debug_context.h           Per-frame debug output container
    steps.cpp                       Pipeline registration for debug-view orchestration
  grade/
    grade.h                         Grade orchestration interface
    grade.cpp                       Grade dispatch and uniforms
    settings.h                      Grade settings structure
    steps.cpp                       Grade pipeline-step implementation
  hdr/
    hdr_state.h                     HDR measurement state and snapshot interface
    hdr_state.cpp                   Format detection, histogram, EMA and gain diagnostics
    settings.h                      HDR runtime settings and format definitions
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
  importance_mask/
    settings.h                      Importance Mask runtime settings snapshot
    importance_mask.h               Importance Mask resource and dispatch interface
    importance_mask.cpp             Importance Mask generation and frame publication
  state/
    settings.h                      Top-level settings, profile metadata, and UI state
    settings.cpp                    Top-level/profile config and legacy-setting migration
    trace.h                         Settings trace interface
    trace.cpp                       Settings change tracing
    version.h                       Addon version constant
  ui/
    ui.h                            Overlay entry point
    ui.cpp                          Main overlay, section orchestration, and pass selection
    ui_pass.cpp                     Pass toolbar and reset controls
    ui_sections.h                   Section-drawer declarations
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

There are three trace streams: Pre-DLSS evaluate work, Post-DLSS evaluate work, and ReShade Present/Backbuffer work. The two evaluate streams are counted separately because a game can call DLSS more than once per presented frame.

### Addon startup

`AddonInit` registers the ReShade addon, loads persisted settings, installs the crash trace, registers `init_device` and `present`, registers the overlay, and attempts the NGX hook. `init_device` installs the early D3D12 hooks. `OnPresent` retries the NGX hook and D3D12 hook installation for a bounded number of presents because both NVIDIA modules and the device may not be ready at addon initialization.

### Pre-DLSS injection

The NGX hook resolves the feature handle and identifies DLSS Super Sampling before the real `EvaluateFeature` call. When any enabled pass targets Pre-DLSS, it reads `NVSDK_NGX_Parameter_Color` and accepts only a full-resource, single-sample 2D texture whose NGX render dimensions match the resource. Non-zero Color subrects and mismatched extents are skipped safely rather than overwriting unrelated pixels.

```text
Game -> hooked NVSDK_NGX_D3D12_EvaluateFeature
  -> capture the current NGX ExposureTexture pointer and Color input resource
  -> AddonController::OnPreDlssEvaluate(native_list, color, exposure_texture)
     -> shared HDR metering on Color using the published previous-frame Importance Mask
     -> one PassSession::RunStage(PreDLSS) per enabled Pre-DLSS pass
        -> write the composed result back to the NGX Color input
  -> restore the game's tracked command-list bindings
  -> real NGX EvaluateFeature (game upscales the modified input)
```

Pre-DLSS records inline on the game's command list and never closes, submits, or waits on it. Its output resource is the game's input Color texture, and `WriteToGameResource` restores it to `NON_PIXEL_SHADER_RESOURCE` before NGX consumes it. At hook entry the game has already populated the current evaluation's depth and motion resources, so `CaptureGuides` publishes those exact guides before Pre-DLSS runs. With the shared advanced **Clone Game Resources** option off (default), the addon retains direct references and performs no guide copies; when on, it copies current depth/motion into reusable addon-owned textures. The option is latched at the next NGX evaluation and applies to all stages. When Pre-DLSS is the first active target, the shared preparation stage meters the current input Color using only the published previous-frame Importance Mask. The first successful model pass builds the new mask after model evaluation.

### Post-DLSS injection

Depth and motion are captured before the real NGX evaluation so every stage sees current-frame guides. When a Pre-DLSS pass is active, a shared preparation stage runs on the current NGX Color input before any Pre-DLSS pass. Otherwise, when a Post-DLSS pass is active, the hook captures the current DLSS output and metering runs before the first Post-DLSS pass. The new Importance Mask is not generated until that pass has evaluated, and the previous-frame mask remains the only mask visible during this frame. Backbuffer-only configurations do not schedule it. With cloning off, Post-DLSS preparation reads the game's output resource directly; with cloning on, it uses the reusable output clone. Pre-DLSS preparation reads the current Color input rather than a stale prior DLSS output.

```text
Game -> NVSDK_NGX_D3D12_EvaluateFeature
  -> hooked_NVSDK_NGX_D3D12_EvaluateFeature
     -> Pre-DLSS pass group (if configured)
     -> real NGX EvaluateFeature
     -> capture the DLSS output when a Post-DLSS pass needs it
     -> AddonController::OnPostDlssEvaluate(native_list, output_resource)
        -> if no Pre-DLSS stage ran: shared HDR measurement using previous-frame Importance Mask
        -> one PassSession::RunStage(PostDLSS) per enabled Post-DLSS pass
           -> first successful pass builds the current frame Importance Mask after ModelOutputBarrier
```

The hook gives NGX exactly the command list the game supplied. The addon uses the unwrapped native interface only for its own D3D12 work. D3D12 state tracking is disabled around addon recording and the real NGX call; tracked command-list bindings are restored before the real evaluation and again after Post-DLSS recording when an addon stage ran.

The Post-DLSS stage never closes, submits, or waits on the game's command list. It records inline and returns control to the game. Each pass writes its final result back to the supplied DLSS output resource so the next Post-DLSS pass can consume the prior result.

The current Post-DLSS step order is:

```
ResolveDevice
EnsureHdr
EnsureComputeEngine
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
BuildEffectiveLocalExposure
GradeModelInput
TemporalPrepare
FillEvaluateParams
EvaluateFeature
ModelOutputBarrier
BuildImportanceMask
GradeModelOutput
ComposeModelOutput
RenderDebugView
WriteToGameResource
```

`HdrBeginFrame` runs once in the shared preparation stage before the first active Pre-DLSS or Post-DLSS pass. It consumes the published previous-frame Importance Mask snapshot. The first successful user pass generates a new mask after its model output barrier; that mask is promoted only at Present. All passes and exposure computations within one frame therefore observe the same mask. Game Exposure and Adaptive Exposure can filter histogram metering; Local Exposure uses only its spatial neighborhood weights and has no mask, depth, or percentile filters.

The two grades are symmetric around the model: `GradeModelInput` grades the model input in place before evaluation, `GradeModelOutput` grades the model output in place before compose. Both operate in the model's sRGB-encoded SDR domain, so every later step consumes them as if the model had produced the graded SDR. Ordering constraint: `BuildImportanceMask` runs **before** `GradeModelOutput`, because the mask must compare the graded model input against the *ungraded* model output — it measures the model's own edits, never the pipeline's grading. No grade is applied to the composite or the backbuffer inside a pass; the separate `grade_backbuffer` setting still grades the swapchain image in the Release stage.

### Backbuffer injection

At ReShade present, enabled Backbuffer passes run after all NGX evaluate hooks for the frame:

```text
ReShade present
  -> AddonController::OnPresent(queue, swapchain)
     -> refresh backbuffer format/debug target format
     -> one PassSession::RunStage(Backbuffer) per enabled Backbuffer pass
     -> RunSingleStage(Release)
        -> GradeBackbuffer
        -> WriteDebugToBackbuffer
        -> SignalHdrAtPresent
     -> release retired resources
```

The Backbuffer stage runs on the session's private command list. It submits and queues a fence signal without waiting on every Present; the 16-slot allocator/list ring waits only if a slot is reused before its fence completes. Its current step order is:

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
TemporalPrepare
FillEvaluateParams
EvaluateFeature
ModelOutputBarrier
GradeModelOutput
ComposeModelOutput
RenderDebugView
WriteToBackbuffer
SubmitBackbuffer
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

`tools/generate_shaders.ps1` stores each non-include source segment as a reusable string constant in `generated/shader_sources.cpp`. For every `.hlsl` root, the generator emits an ordered list of fragment references by walking source includes and honoring `#pragma once`. Runtime compilation concatenates that fragment list into a temporary source string; it does not process includes or `#pragma once`. The generated C++ therefore shares common source fragments instead of embedding a separately expanded copy per shader root. Generated files should never be hand-edited.

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

`kComputePasses[]` maps the trace/dispatch name to the root shader path. `ComputeEngine::CompilePipeline` uses embedded bytecode when present; otherwise it concatenates the root's ordered `EmbeddedShaderFragment` references and calls `D3DCompile` with:

- source = the assembled fragment string
- source name = the root path, so compiler diagnostics identify the shader root
- include handler = `nullptr`; the assembled source contains no `#include` or `#pragma once` directives
- entry point = `CSMain`
- target = `cs_5_1`
- optimization = `D3DCOMPILE_OPTIMIZATION_LEVEL3`

The generator resolves include order and include-once behavior while producing each root's fragment list, but it stores source segments only once across roots. `generated/expanded_shaders` is a build-tooling output used by FXC verification and optional precompilation; its expanded shader bodies are not embedded into the runtime source table. Runtime compilation is the default. `PrecompileShaders=false` keeps the generated bytecode table as an empty-count sentinel. Setting `PrecompileShaders=true` runs `tools/precompile_shaders.ps1`, generates `.cso` data and rewrites `generated/shader_bytecode.cpp`, and `CompilePipeline` uses those bytes instead of calling `D3DCompile`.

### Build-time verification

`VerifyShaders=true` is the default. `tools/verify_shaders.ps1` locates `fxc.exe` automatically from the installed Windows SDK when `FxcPath` is not supplied, compiles every `.hlsl` root as `cs_5_1`, and forwards actual compiler warnings/errors. The generator expands local includes while honoring `#pragma once`, then writes pragma-free root sources under `generated/expanded_shaders`; verification and optional precompilation use those expanded files so FXC never sees its unsupported `#pragma once` directive. It writes objects to a temporary directory and deletes them after each verification pass. Successful FXC object-save messages are intentionally not part of the build output.

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

The 512-byte CBV is:

| Offset | Block | Size |
|---:|---|---:|
| 0 | `Width`, `Height`, header padding | 16 |
| 16 | `ShaderBlock` | 32 |
| 48 | `GradeBlock` | 48 |
| 96 | `ResidualBlock` | 32 |
| 128 | `HdrBlock` | 80 |
| 208 | `EasuBlock` | 48 |
| 256 | `RcasBlock` | 16 |
| 272 | `TemporalBlock` | 80 |
| 352 | alignment padding | 160 |

The resulting `ComputeConstants` size and `BindAndDispatch` stride are both 512 bytes, rounded to the D3D12 constant-buffer alignment. `TemporalBlock` owns the photometric/depth thresholds, maximum motion displacement, motion-vector coordinate metadata, and guide subrect metadata. Temporal kernels use a dedicated `TemporalBlock`; they do not borrow HDR or generic shader-parameter slots. `HdrBlock.LocalExposureParams` separately carries the local-exposure neighborhood diameter and tap count.

The shader-side aliases in `bindings.hlsli` turn fields such as `hdr.Exposure` and `easu.Con0` into the existing semantic names used by shader code (`kExposure`, `kEasuCon0`, and so on). The Strength/Soft-Compression settings (`PassSettings::strength` / `::soft_compression`, compose/settings.h) are written field-by-field into `ShaderBlock.Params0` and `Params1` by `ComposeOutput` in compose/compose.cpp.

The layout intentionally avoids `float2`, `float3`, `packoffset`, platform headers and HLSL-only utility code in `params.h`. Every nested block is explicitly 16-byte aligned and padded.

### Shared filtering and reconstruction

The model input downsample is `BoxAreaAverage` from `shaders/common/box_filter.hlsli`. It uses the exact source footprint covered by each destination pixel, including fractional edge coverage, and fetches with `Texture2D.Load`. It does not rely on the global linear sampler.

The non-HDR `PrepareModelInput` path therefore treats the source as already in the model's target sRGB space and passes the averaged RGB through unchanged. The HDR carrier path averages the linear carrier, applies the reversible model highlight/gamut fit, and applies `LinearToSrgb` before the model sees it. Normal colors are unchanged; super-whites are smoothly compressed toward white so emissive highlights keep a white/yellow core.

Both SDR and HDR compose paths support FSR 1.0 EASU or joint-bilateral upsampling. Optional RCAS from `shaders/common/fsr.hlsli` sharpens both the reconstructed model input and output after either upsampling path. RCAS enable/strength are registered per pass and are not gated to Backbuffer targets or EASU mode; all three paths use the shared cbuffer controls.

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
| `PreDLSS` | before real NGX `EvaluateFeature` | game owns submit | yes | command list, Color input resource, stage |
| `PostDLSS` | after real NGX `EvaluateFeature` | game owns submit | yes | command list, output resource, stage |
| `Backbuffer` | ReShade present | pass session | no | swapchain, command queue, stage |
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

Pipeline step definitions can live in the file that owns their concern. `pass_session.cpp` owns most steps; `hdr_state.cpp`, `importance_mask.cpp`, and `addon_controller.cpp` contribute their own registrations. `FindPipelineStep` builds a node-indexed registry and detects duplicate ownership or a definition whose id does not match the lookup slot.

## Pipeline invariants and hard-won rules

### D3D12 / GPU

**Own every replaced resource until the GPU is finished with it.** `PassResources` transfers replaced resources into the retire callback rather than releasing them immediately; Importance Mask and resized backbuffer-grade scratch textures use the same deferred-release queues. Resources are tagged with a queue-ordered Release fence and released on a later Present only after that fence completes. Direct `ComPtr::Reset()` on an in-flight resource is unsafe.

**Seed states for resources the addon owns.** `InferResourceState` is only a fallback for resources whose exact state is not otherwise known. Pass-owned resources are seeded at frame boundaries and cleaned back to their expected states before submission.

**The NGX model output needs a UAV barrier before the addon reads it.** A state transition is not sufficient to synchronize the external UAV write performed by NGX.

**Pre-DLSS and Post-DLSS work must stay on the game's command list and must not submit it.** Pre-DLSS records before the real NGX evaluation and writes the result to its Color input; Post-DLSS records afterwards and writes the result to the upscaled output. The game's queue submits both. Backbuffer work uses an addon-owned command list.

**Present/Release command contexts own their own fence, but the frame path is asynchronous.** Backbuffer and Release submissions signal the command queue without an unconditional CPU wait. A 16-slot allocator/list ring waits only before reusing an allocator whose signal has not completed. When either retired-resource queue is non-empty, `OnPresent` still submits a Release queue marker even if grading and debug output are disabled; retired entries are tagged with that fence and released only after its value completes. `WaitGpu` remains available for shutdown or other callers that genuinely require completion immediately.

**Abandoned Present recordings must be closed before allocator reuse.** `CommandContext::Abort` closes a recording that exited before the normal `SubmitBackbuffer` tail; `Reset` must never reset an allocator while its command list is still recording.

**Removed pass sessions are retired after a queue fence.** Removing a pass drops it from the live ID map immediately, but its `PassSession`, NGX feature, and resources remain owned until a Release-queue marker behind prior pass work completes. `PreDLSS` and `PostDLSS` record on the game's command list, so erasing a session must not synchronously release their feature/resources while that list may still be in flight.

**The descriptor/constant ring cannot be reused ahead of the GPU.** Descriptor views and the 512-byte constant slot are written by the CPU while commands are recorded but consumed later by the GPU. The ring depth therefore has to cover the maximum CPU lead and worst-case dispatch count.

**Importance Mask keeps the published snapshot immutable across resolution changes.** The destination ping-pong texture is resized independently; the currently published texture is retained until Present. Importance-mask temporal history samples the prior mask in normalized coordinates if its resolution differs; this is separate from temporal model-edit reprojection. Resource replacement uses the deferred-retirement path, and a successful generated mask is promoted only at Present.

**Always return the swapchain image to `PRESENT`.** `GradeBackbuffer` keeps that transition even when grading fails, because leaving the backbuffer in an intermediate state turns a grade error into a later present failure.

**The debug layer's object chain is not a safe assumption for the vtable hooks.** The addon hooks native D3D12 interfaces; SDK-layer or proxy vtables can point at different thunk implementations than the hooks expect.

### Temporal edit reprojection

Temporal reprojection runs after `GradeModelInput` and before `FillEvaluateParams`/NGX evaluation on each pass. It extracts the model's per-channel multiplicative edit as bounded log gain, reprojects the previous edit with current motion vectors, then rejects history that fails coordinate/motion, photometric, or depth validation. Accepted history is applied to the current model input to synthesize the model output; any unavailable history, failed GPU step, missing fresh guides, or feedback above the configured invalid percentage fails open to a fresh NGX evaluation. A fresh model result replaces the stored edit/depth/input history. The feedback readback is fence-delayed and asynchronous; the render path does not wait for the CPU to learn the invalid fraction.

The mode selector offers `Off`, `Adaptive`, `Every 2`, and `Every 4`. The photometric test compares current and motion-reprojected previous input luma against an absolute-plus-relative allowance. Depth validation compares current depth against the previous depth at the reprojected coordinate using the larger of the absolute and relative limits. Motion beyond the maximum displacement is rejected. The default limits are absolute/relative photometric `0.06`/`0.10`, absolute/relative depth `0.005`/`0.03`, maximum displacement `40` working-resolution pixels, and maximum invalid/mismatched fraction `5%`. Lower thresholds reject more history and increase fresh model evaluations; higher thresholds trade validation strictness for reuse.

Debug views show the pre-depth candidate, post-depth validation result, and per-channel reprojected log edit. Candidate/validation status is encoded by alpha in the temporal working textures; debug views translate it to readable colors. They are rendered on the pass command list while the produced resources are still in known states.

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

**Exposure is measurement state, not compose state.** Histogram-derived exposure changes what the model sees; HDR compose reads the untouched source directly and applies the model residual in HDR.

**Game Exposure is one layer in the exposure stack.** When enabled, the addon captures the game's NGX `ExposureTexture` before `EvaluateFeature`. If it is a usable 1x1 texture, a compute pass samples its scalar and an asynchronous GPU readback updates HDR state without waiting on the CPU. Game Exposure remains the base when available; its optional bias and bounded tuning use that value as their anchor. Adaptive Exposure can add a bounded histogram-derived correction, and Local Exposure can apply a spatial correction afterward. If game exposure is unavailable, Adaptive Exposure can provide the global exposure on its own. The Status panel reports availability and the last sampled value; `Debug View -> Game Exposure` shows the sampled value in stops.

**Exposure filters use the previous frame's Importance Mask snapshot.** When enabled and valid, the mask constrains only those histogram computations whose filter options select it. Local Exposure has no Importance Mask, Depth, or Percentile filter settings. The mask generated after the current frame's first successful Pre/Post-DLSS model evaluation is not exposed until Present, so it cannot affect metering or later passes in the same frame.

**Adaptive and Local Exposure are independent layers.** Adaptive Exposure maps the filtered histogram to its midtone target and can provide global exposure by itself or a bounded correction on top of Game Exposure. Local Exposure computes a continuous per-pixel correction from a configurable block-sized low-frequency neighborhood while restoring high-frequency detail. Neighborhood diameter ranges from 4 to 512 source pixels, and the number of taps is configurable from 4 to 48. More taps fill spatial gaps within the same radius; they do not increase the radius and cost additional GPU time. Local Exposure uses distance weighting only; it deliberately has no mask/depth/percentile filters or edge-aware controls. `Restore Global Exposure` reduces the local exposure multiplier texture to its arithmetic mean, then applies the reciprocal uniformly to the model output in linear light before the Output Grade. It retains local shading from the model instead of undoing the local field pixel by pixel, while bringing average exposure roughly back toward the global level. The Game and Adaptive layers retain their histogram-filter options.

**The HDR carrier and source have separate responsibilities.** `GainMapSplit` writes RGB linear SDR carrier plus a direct reciprocal of the RGB carrier scale to `R32G32B32A32_FLOAT`. The multiplier is stored linearly in alpha to avoid an unnecessary log2/exp2 precision cycle; gain statistics and the debug view convert it to stops. The paired pre-desaturation/inverse operations are reduced algebraically to a scalar RGB multiply so saturated super-whites do not suffer cancellation noise in near-zero channels. No additive offsets or fixed super-white ceiling are applied. HDR composition reads the untouched source resource and decodes it into float shader values, then transfers the model residual onto that source; it never reconstructs the original from the SDR carrier. The HDR model input/output and EASU/RCAS working textures use `R32G32B32A32_FLOAT` to reduce precision loss in the reversible highlight transform; SDR continues to use the source-compatible model format. The published composite remains in the game's native format, whose representable range still applies. The model-gamut mapping keeps valid source super-whites below model-domain white with a piecewise logarithmic mapping; its inverse also handles model-output overrange. This mapping is only for the model-facing carrier. The original source and its full carrier-to-HDR multiplier remain separate, so model-gamut fitting never caps the final HDR result.

**`HDR Roundtrip Error` isolates the carrier transfer.** It compares the original decoded full-resolution HDR source directly with HDR reconstructed from the RGBA32F carrier and multiplier. It does not run EASU, RCAS, model processing, input grade, or output-format encoding. Alpha stores the reciprocal of the scalar used to create RGB from the original HDR source; it is the carrier-to-HDR reconstruction multiplier. Gain statistics and the gain-map debug view display its log2 value in stops. HDR composition itself uses the untouched source and therefore does not inherit carrier reconstruction error. `HDR Roundtrip Diagnostic` is a different comparison: it uses the actual prepared model input and compares the composed HDR mapped back to model SDR with the model output in the ungraded model-linear domain. Nonzero edge differences can be legitimate because HDR composition preserves full-resolution source detail; Working Scale and RCAS change the comparison. It is not an isolated carrier-transfer test.

**Local exposure is kept out of the HDR residual reference.** When Local Exposure is active, GainMapSplit produces both the normal locally adjusted carrier and a global-only carrier with the same BT.2446 mapping. The global-only carrier is downsampled and model-gamut-fitted through the same preparation path as the actual model input, and receives the same input grade. HDR composition compares the model output against this unlifted reference, so local lifting does not flatten the measured edit. It is constructed before downsampling rather than by dividing a filtered image afterward; this preserves the exact sampling footprint and avoids detail loss. The normal carrier still stores the reciprocal of its complete global × local scale, which cancels that scale when transferring the relative edit onto the untouched HDR source.

**HDR round-trip diagnostics.** `HDR Roundtrip Error` checks only source-to-carrier-to-HDR reconstruction, independent of Working Scale, RCAS, model processing, and input grade. A uniform neutral image means the carrier reconstruction itself is accurate. `HDR Roundtrip Diagnostic` compares model and composition domains, including EASU/RCAS and the preservation of full-resolution source detail. Its bottom-right panel is a composition difference, not a pure transfer error; input grade is removed before the model output is compared. Use Error to test carrier math and Diagnostic to inspect composition behavior.

**Render debug images at pass time.** The resource state is trustworthy while the producing pass still owns the command list and has just produced the resource. `MaybeRenderDebug` renders into `FrameDebugContext::output`; the Release stage only copies that already-rendered output to the backbuffer.

**The debug target uses the swapchain's format and dimensions.** Pre-DLSS and Post-DLSS sources may have a different format or render extent. `DebugDisplay` renders into a backbuffer-sized debug texture and samples the selected source over its normalized extent, so the Release stage can copy it directly to the swapchain.

**Debug selection is global, rendering is pass-local.** The Debug section's `ctl` value is snapshotted into each `PassRun`; only the active pass renders the selected view.

### Settings and UI

**Guide/output cloning is one shared advanced option.** `CloneResources` defaults to false and is read atomically by the NGX hook at the next evaluation boundary. Off uses current game resources directly; on clones current depth/motion and, only for Post-DLSS, the post-evaluation color output. The same toggle governs Pre-DLSS, Post-DLSS, and the guides available to Backbuffer, so it can compare copy-path costs without reverting to stale guides.

**Working scale 1 avoids identity scaling work.** For SDR inputs with no input grade and equal input/working dimensions, `PrepareModelInput` passes the original resource to the model instead of dispatching an identity downsample. SDR/HDR composition still dispatches because it applies the model residual and writes the final image, but the shaders skip upsampling when the working and output sizes are equal; optional RCAS remains active. The HDR carrier preparation is not bypassed because it performs required color-domain conversion.

**Brightness mode `Exposure` operates in linear light.** It multiplies all three linear RGB channels by `2^stops` before output encoding, which changes scene intensity while preserving chromaticity until gamut limits/clipping. It replaces the former Lift mode; the shared mode index is retained so profile serialization remains stable. Gain, Gamma, and Soft keep their separate luminance-shaped curves.


**Saved profiles use stable `ProfileId` keys.** `UserPresetOrder` is the comma-separated display order and `UserPresetNextId` preserves the allocation high-water mark. A profile's name, enabled state, and pass order live at `UserPreset.<id>.Name`, `.Enabled`, and `.PassOrder`; its `ctl` block is under `UserPreset.<id>.Data`. Save and delete pass that profile's pass order as the instance-key source, so unrelated live pass instances are not copied into or erased from its block. Deletion removes one id from the order and erases its schema-defined keys; it never shifts another profile's data. Legacy positional profile blocks are not imported Rename edits the selected profile name in place after validation; Enter saves and Escape cancels.


**HDR composition uses Gain-Map Residual exclusively.** HDR Pre-DLSS and Post-DLSS passes use the packed per-pixel carrier-to-HDR multiplier to transfer model edits back to the untouched full-resolution HDR source. The former HDR Ratio mode and selector are removed. SDR composition, including Present/backbuffer passes, retains its existing log-ratio residual path and settings.

**HDR exposure is configured as a layer stack.** The HDR UI has separate `Game Exposure`, `Adaptive Exposure`, and `Local Exposure` tabs. The default stack order is Game Exposure base, Adaptive Exposure correction, then Local Exposure. Game and Adaptive Exposure retain their Importance Mask, Depth, and Percentile Range filter combinations. Local Exposure exposes neighborhood block size, a 4-48 sample count, and `Restore Global Exposure`; its correction remains continuous per pixel and uses no filters. Restoration computes the arithmetic mean of the local exposure multiplier on the GPU and applies its reciprocal uniformly to linear RGB, rather than reversing the per-pixel field. The exclusive Median and combined Auto Exposure + Local modes are removed; old settings are migrated to the closest layer configuration.

**Importance Mask is derived from the pass result, not a separate model evaluation.** It compares the first successful Pre/Post-DLSS pass's raw model output with the same-resolution model input actually fed to NGX, including input grading and Local Exposure. The mask reads that input directly instead of making a separate ungraded/unlifted copy. The previous-frame mask is used by importance-mask temporal filtering and exposure only; it is separate from the model-edit reprojection history. The new mask is published at Present.

**Pass targets are independent per-pass settings.** Runtime execution groups enabled passes in frame order: all Pre-DLSS passes before the real NGX evaluation, all Post-DLSS passes after it, then all Backbuffer passes at ReShade Present. Within each target group, passes run in pass-tab order. Each pass target is persisted as `PassN.TargetStage`; the prior two-value `PassN.InjectionStage` key is read only for migration. This means a pass with a higher tab index can run in an earlier stage than a lower-index pass; chaining applies within each target group.

**The debug selection is not per-pass.** The active UI tab selects which pass renders the shared debug view. Temporal reprojection adds `Temporal Candidate Status`, `Temporal Validation Status`, and `Temporal Reprojected Edit`; unavailable history is blue, accepted pixels are green, photometric mismatches are yellow, depth/guide rejection is magenta, and rejected/no-candidate pixels are red or dark in the edit view.

**Temporal reprojection is configured per pass and sits before Advanced/model grading.** The `Temporal Reprojection` section follows Strength and precedes the Advanced settings and grade-boundary sections. Mode defaults to Off. Its controls expose absolute/relative photometric thresholds, absolute/relative depth thresholds, maximum working-resolution motion displacement, and the maximum invalid-pixel percentage that permits reuse. Defaults preserve the existing acceptance limits: 0.06/0.10 photometric, 0.005/0.03 depth, 40 working pixels, and 5% invalid/mismatched pixels. Threshold changes are included in the temporal-history signature and invalidate stale history. The pass-tab row wraps when buttons no longer fit beside the toolbar.

**ImGui IDs are scoped by section identity and instance identity.** `ctl::DrawSection` pushes a scope derived from the schema section name and `InstanceKey`, so equal visible labels in different sections or instances do not share collapsing-header IDs. Persistent open/closed state remains governed by the overlay callbacks; `PersistentHeader` owns the common non-schema section state.

## HDR model path

The HDR path is gain-map based and runs on the Pre-DLSS and Post-DLSS targets. Backbuffer passes use the existing SDR composition path.

1. `HdrState` detects the source format and resolves the enabled exposure layers. Game Exposure is the base when its game-provided value is valid; Adaptive Exposure contributes its filtered histogram correction or supplies global exposure when Game Exposure is unavailable. Local Exposure computes a continuous spatial scale from its configured neighborhood block size and 4-48 tap sample count using spatial-distance weights only. The same helper and controls are used by the gain-map split and effective-exposure diagnostic map. The optional `Restore Global Exposure` step reduces the generated local exposure multiplier texture to an arithmetic mean, then applies the reciprocal as one uniform linear-RGB exposure correction before output grading. It does not undo each pixel's local lift, preserving the model's spatial shading while bringing average exposure roughly back toward the global level. Remaining differences depend on the image content and the model's response; this adjustment is not intended to reverse the model's own edits. Importance Mask compares the actual model input after input grading and Local Exposure with the raw model output.
2. `Importance Mask` is generated from the first successful Pre/Post-DLSS pass's actual model input (after input grading and Local Exposure, when enabled) and its raw model output. It reads the same model-input resource that NGX evaluated, so there is no separate ungraded/unlifted mask reference. The mask is not consumed by any stage until the next Present boundary; backbuffer passes are excluded. It does not evaluate a separate model feature.
3. `GainMapSplit` converts the full-resolution HDR source into an RGBA32F carrier: RGB is the linear SDR base and A is the reciprocal of the complete carrier scale. BT.2446-C parameters and scene-white control the carrier; no additive gain offsets or gain ceiling are applied. If Local Exposure is enabled, a second invocation with Local Exposure disabled writes a global-only carrier for the residual reference.
4. `PrepareModelInput` performs exact Box Area downsampling. For the HDR carrier path it applies the reversible model highlight/gamut fit and sRGB encoding. When Local Exposure is active, the global-only carrier uses this same path to create a separate unlifted working-resolution reference for HDR composition. Importance Mask instead reads the actual prepared model input after input grading; it never uses the composition reference.
5. The model feature evaluates at the selected working scale while being built at full frame resolution.
6. `ComposeModelOutputHdr` uses FSR EASU and optional RCAS to reconstruct model-domain images at full resolution. With Local Exposure enabled, the composition residual uses the unlifted reference; otherwise it uses the prepared model input. HDR composition retains the original Gain-Map Residual math: the packed per-pixel carrier-to-HDR multiplier transfers the relative model edit onto the untouched full-resolution HDR source. It does not invert the nonlinear super-white fit to derive an absolute endpoint delta. Present/backbuffer passes continue to use the existing SDR log-ratio compose path.
7. `EncodeHdr` writes the reconstructed result back into scRGB or HDR10 as required by the source format. The gain-map residual compose path measures the model edit in the model domain, then uses the packed carrier-to-HDR multiplier to transfer that edit into the original source's scene-referred range without inverting the highlight/gamut fit.

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

Set `DLSSNR_TRACE=<path>` to enable the command/event trace. The trace maintains separate counters for `PreDLSS`, `PostDLSS`, and `Present` streams, assigns stable resource ids in first-seen order, and records frame boundaries. `TRACE_CMD` covers resource transitions, barriers, copies and dispatches; `TRACE_EVT` records control-flow decisions such as skips and failures.

Because trace lines are intentionally flushed immediately, the file is suitable for diagnosing the final commands leading into a crash. The trace should still be bounded to a deterministic test scene because a full frame contains many transitions and dispatches.

Settings changes are recorded separately as `cfg` lines when tracing is enabled. Load-time state is emitted as `init`; changes at save time are emitted as `set` transitions.

### Crash trace

`crash_trace.h` installs a vectored exception handler and a terminate handler at addon attach time. It writes `%TEMP%\dlssnr_crash.txt`, including fault code, faulting module and offset, a short stack, x64 register context and thread id. The handler is deliberately raw Win32 and allocation-free enough for a broken process, and it chains to the game's own exception handling.

## Extending

### Add a shader pass

1. Add one root `.hlsl` under the appropriate `shaders/<module>/` directory with one `CSMain`.
2. Include the shared headers it actually needs (`common/cbuffer.hlsli`, `common/bindings.hlsli`, `common/registers.hlsli`, plus functional helpers such as `color.hlsli` or `fsr.hlsli`).
3. Add the root path to `kComputePasses[]` in `gpu/compute_passes.cpp`.
4. Register the pass in the appropriate pipeline step if it is part of frame orchestration.
5. Bind exactly four SRVs and one UAV at the call site; repeat unused SRV resources.
6. Use the shared `ComputeConstants` block for common parameters. If the pass truly needs a private parameter convention, document it locally rather than changing the global layout casually.

The build automatically embeds the new shader and the verification target compiles it before C++ compilation.

### Add a shader parameter

1. Add or update the runtime settings field in the owning header when the compose/grade code needs a typed snapshot, then declare the user-facing value in the owning `feature/<name>/schema.cpp`.
2. Map the stored value into the runtime snapshot in that feature's `view.cpp`; `ctl::Save` and `ctl::Load` own setting persistence. Do not add a parallel per-feature INI loader.
3. Add or reuse the corresponding field in `ShaderBlock` in `shaders/common/params.h` only when the parameter belongs to the shared cbuffer rather than a pass-specific constant convention.
4. Write the value through the typed cbuffer field instead of a numeric slot offset.
5. Add the HLSL alias in `shaders/common/bindings.hlsli` if the shader-facing semantic name is not already present.

`ShaderBlock.Params0` and `Params1` hold the Strength/Soft-Compression parameters; the cbuffer layout is asserted in `gpu/compute_constants.h`.

### Add a debug view

1. Add a selector to `kDebugViews` in `debug/debug_view.cpp`.
2. If the resource is not already in `DebugSnapshot`, add it to `debug/debug_view.h` and populate it from the step that produces the resource.
3. Keep the rendering on the producing pass's command list; do not defer the shader work to Present.

### Add a pipeline step

1. Add a `PipelineNodeId` entry in depth order.
2. Add a step function in the owner (`PassSession`, `AddonController`, `HdrState`, `ImportanceMask`, etc.).
3. Register a `PipelineStepDef` in that owner's registry.
4. Add the node to the appropriate stage table and mirror list.
5. Declare every prerequisite in `depends_on` and place the prerequisite earlier in the stage.
6. Declare every frame input the body reads in `required_inputs`; do not declare values the step itself creates.
7. Update the stage phase ranges.
8. Re-read the final stage order; the compile-time guards do not prove semantic ordering that has not been expressed as a prerequisite.

### Add a UI section

The overlay owns cross-feature layout and coordination. A feature's ordinary rows are declared in its schema and rendered by `ctl::DrawSection`; use hooks for feature-specific UI that must appear between rows. Pass-toolbar operations belong in `ui_pass.cpp`, while the pass's settings body is schema-driven.

### Add a setting

1. Add the setting key to the owning `feature/<name>/settings.h` and declare its type, default, metadata, and presentation in `feature/<name>/schema.cpp`.
2. Expose it through that feature's typed `view.h/.cpp` only when runtime code needs it, and update the runtime settings structure if required.
3. Use a schema hook for specialized UI; keep cross-feature enablement in the overlay rather than making one feature's schema depend on another feature's keys.
4. Persistence is handled by `ctl::Save`/`ctl::Load`; do not add per-feature hand-written save/load mappings.
5. Keep UI-only transient state in the overlay/settings UI flags rather than the frame-processing snapshot.

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
