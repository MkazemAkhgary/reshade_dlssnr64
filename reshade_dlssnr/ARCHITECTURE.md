# ReShade DLSSNR Addon

## Purpose

The addon runs NVIDIA DLSS Neural Rendering (NGX feature 18) through `nvngx_dlssnr.dll` on top of a game's existing DLSS Super Sampling path. Each pass independently targets one of three injection points: **Pre-DLSS**, where work runs on the game's `Color` input before its real DLSS `EvaluateFeature`; **Post-DLSS**, where work runs inline immediately after the real evaluation on the upscaled output; or **Backbuffer**, where work runs on an addon-owned command list against the final swapchain image. The stages share the same `PassSession` pipeline and differ mainly in input resource, command-list ownership, and output publication point.

HDR is handled by the Pre-DLSS and Post-DLSS paths. The source is converted into an SDR carrier plus a full-precision carrier-to-HDR reconstruction multiplier, and the model operates on the SDR carrier. Composition reads the untouched HDR source directly and applies the model residual in HDR. The final backbuffer grade can also operate in SDR, scRGB, or HDR10 according to the detected/selected format.

## Repository layout

The source tree is organized by ownership rather than by pipeline stage. `ctl` is domain-agnostic: it defines schemas, values, persistence, generic drawing, and tracing, but does not include or register application features. `feature/register_all.cpp` owns the application schema composition, and `feature/settings_store.cpp` builds the live store from that registration. This dependency runs from the feature layer into `ctl`, never the reverse. The only host-specific `ctl/usage` adapter is the ReShade config reader/writer.

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
    shader_bytecode.cpp             Runtime-compile sentinel table definition
    shader_bytecode_precompiled.cpp Precompiled table, written only by PrecompileShaders
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
    hdr_local_exposure.hlsli        Shared pivot-aware local exposure curve and detail protection
    luma.hlsli                      BT.709/BT.2020 luma weights
    math.hlsli                      Shared scalar/vector math helpers
    grade.hlsli                     Linear and SDR grade transforms
    box_filter.hlsli                Exact footprint Box Area Average
    compose_common.hlsli            Residual shaping, ratio compose, YCbCr blend
    compose_upsample.hlsli          Shared SDR/HDR joint-bilateral upsampling

  shaders/compose/
    compose_model_output.hlsl       SDR compose with fixed joint-bilateral 3×3 upsampling
    compose_model_output_hdr.hlsl   Gain-Map Residual HDR compose with fixed joint-bilateral 3×3 upsampling and source-preserving HDR composition
  shaders/debug/
    debug_display.hlsl              Debug-view renderer
    debug_hdr_roundtrip.hlsl        HDR round-trip, error, and local-exposure diagnostic renderer
  shaders/grade/
    apply_grade.hlsl                In-place color grade and edge-aware softening
    apply_cas.hlsl                  FidelityFX CAS sharpening pass
  shaders/hdr/
    zero_histogram.hlsl             Clears the full-frame 256-bin histogram buffer
    build_histogram.hlsl            Builds the full-frame log2-luma histogram
    gain_map_split.hlsl             Builds the full-resolution SDR carrier + HDR multiplier
    build_effective_local_exposure.hlsl
                                    Builds the full-resolution local SDR exposure scale for HDR diagnostics
    reduce_local_exposure_tiles.hlsl
                                    Reduces local/global carrier ratios to per-tile mean log exposure
    reduce_local_exposure_global.hlsl
                                    Reduces tile log exposures to one global geometric-mean multiplier
    zero_gain_map_stats.hlsl        Clears gain-map diagnostic counters
    build_gain_map_stats.hlsl       Builds gain-map diagnostic statistics
    read_game_exposure.hlsl         Reads the game's 1x1 NGX exposure texture asynchronously
  shaders/model/
    prepare_model_input.hlsl        Box-area resize into the model working buffer
  shaders/importance_mask/
    build_importance_mask.hlsl             Builds the per-frame Importance Mask from model input/output difference
  shaders/temporal/
    reproject_edit.hlsl             Motion-reprojects the previous edit, samples it with Catmull-Rom, and scores its confidence
    validate_depth.hlsl             Folds the depth-discontinuity test into the confidence and accumulates reuse statistics
    apply_edit.hlsl                 Applies a validated edit to the current model input
    commit_history.hlsl             Accumulates the fresh edit into the stored history and stores the current depth
    reconstruct_motion_vectors.hlsl Estimates a motion field from depth when the game supplies none
    clear_stats.hlsl                Clears temporal feedback counters

  ctl/                             Declarative settings framework
    ctl.h                           Framework umbrella header
    types.h/.cpp                   Values and setting metadata
    schema.h/.cpp                  Section, setting, hook, and schema declarations
    registry.h/.cpp                Setting-type registry
    values.h/.cpp                  Instance store
    serialize.h/.cpp               Schema-driven persistence
    draw.h/.cpp                    Generic section renderer
    builtin_settings.h/.cpp        Float, Int, Bool, Enum, String, and IntList settings
    trace.h/.cpp                   Generic tracing for all registered settings
    usage/                          ReShade config adapter
    ctl_acceptance.cpp              Headless acceptance harness
  feature/                         Feature-owned setting schemas and typed views
    register_all.cpp                Central feature registration
    settings_store.h/.cpp            Application-owned schema composition and live store
    preferences/                    Shared preferences schema and setting keys
    debug_log/                      Debug selection schema and view
    grade/                          Grade schema and view
    hdr/                            HDR schema, view, and UI hooks
    importance_mask/                Importance Mask schema and view
    pass/                           Per-pass schema, stable IDs, order, and view
    ui_state/                       Global-only schema for persisted overlay open/closed state
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
    settings.h                      Runtime flags, active pass, and profile metadata
    settings.cpp                    Top-level/profile config and legacy-setting migration
    trace.h                         Settings trace interface
    trace.cpp                       Settings change tracing
    version.h                       Addon version constant
  temporal/
    steps.cpp                       Temporal prepare/commit steps and reuse policy
    stats.h                         Live reprojection snapshot published to the overlay
  ui/
    ui.h                            Overlay entry point
    ui.cpp                          Main overlay, section orchestration, and pass selection
    ui_pass.cpp                     Pass toolbar and reset controls
    ui_sections.h                   Section-drawer declarations
    ui_status.cpp                   Status, temporal readout, and retry controls
    ui_widgets.h                    Shared widget declarations
    ui_widgets.cpp                  Shared widget implementation

  third_party/
    reshade/                        ReShade SDK and ImGui source
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

The NGX hook resolves the feature handle and identifies DLSS Super Sampling before the real `EvaluateFeature` call. When any enabled pass targets Pre-DLSS, it reads the active Color region from the NGX render-subrect dimensions (falling back to Width/Height) and Color subrect base. A full-resource, single-sample, single-mip texture can be processed directly. Mip chains, multisampled textures, nonzero subrect origins, or render extents smaller than the allocation are normalized into a compact single-sample texture containing only the active region. Mip zero is used because NGX does not expose a Color mip selector here. MSAA Color is region-resolved to one sample before processing. If a nonzero subrect origin is supplied without a usable active width or height, the hook skips that input instead of guessing that the rest of the allocation is rendered content. The compact resource and temporary NGX parameter overrides remain valid through the real evaluation, and the original parameter block is restored immediately afterward. The original allocation is preserved for adapted inputs; when MSAA is resolved, the original per-sample contents cannot be reconstructed from the averaged image.

```text
Game -> hooked NVSDK_NGX_D3D12_EvaluateFeature
  -> capture the current NGX ExposureTexture pointer and guide resources
  -> resolve the active Color region and adapt it when needed
     -> copy mip zero/subrect into a compact texture, or resolve MSAA into one sample
     -> temporarily bind the compact texture and zero-based region to NGX parameters
  -> AddonController::OnPreDlssEvaluate(native_list, active_color, exposure_texture)
     -> one PassSession::RunStage(PreDLSS) per enabled Pre-DLSS pass
        -> meter the current chained Color using that pass's prior-frame mask
        -> write composed output into the active-region texture NGX will consume
  -> restore the game's tracked command-list bindings
  -> real NGX EvaluateFeature (game upscales the processed active region)
  -> restore the original NGX parameter values and retire the adapter after a queue fence
```

Pre-DLSS records inline on the game's command list and never closes, submits, or waits on it. For the direct-compatible case, its output resource is the game's input Color texture. For mip-chain, MSAA, or subrect allocations, it operates on a compact single-sample adapter sized exactly to the active region; no pixels outside that region enter the model. The game resource is left unchanged in this adapted path, and the real NGX call consumes the temporary processed input. Adapter resources are retired behind the present-queue fence rather than released while the game's command list may still be in flight. `WriteToGameResource` leaves the direct input or compact adapter in `NON_PIXEL_SHADER_RESOURCE` before NGX consumes it. At hook entry the game has already populated the current evaluation's depth and motion resources, so `CaptureGuides` publishes those exact guides before Pre-DLSS runs. With the shared advanced **Clone Game Resources** option off (default), the addon retains direct references and performs no guide copies; when on, it copies current depth/motion into reusable addon-owned textures. The option is latched at the next NGX evaluation and applies to all stages. Each Pre-DLSS pass meters the current input Color using only its own published previous-frame Importance Mask. Each successful pass builds its own new mask after model evaluation.

### Post-DLSS injection

Depth and motion are captured before the real NGX evaluation so every stage sees current-frame guides. Every Pre-DLSS pass meters its live Color target before model preparation; every Post-DLSS pass meters its live upscaled output target. Each pass builds its own Importance Mask after its model output barrier and uses only its prior-frame mask during the current frame. Backbuffer-only configurations do not schedule HDR metering or mask generation.

```text
Game -> NVSDK_NGX_D3D12_EvaluateFeature
  -> hooked_NVSDK_NGX_D3D12_EvaluateFeature
     -> Pre-DLSS pass group (if configured)
     -> real NGX EvaluateFeature
     -> capture the DLSS output when a Post-DLSS pass needs it
     -> AddonController::OnPostDlssEvaluate(native_list, output_resource)
        -> one PassSession::RunStage(PostDLSS) per enabled Post-DLSS pass
           -> meter live output with that pass's previous-frame mask
           -> build its own mask after ModelOutputBarrier
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

`HdrBeginFrame` runs in each Pre-DLSS and Post-DLSS pass before model-input preparation and meters that pass's live target resource: the game's HDR source for the first pass, then the previous pass's composed HDR result. HDR exposure state and Importance Mask history are owned by the pass. Each pass consumes its own published previous-frame mask, generates its own new mask after model output, and promotes it at Present. `Reuse Previous HDR Pass Metering` skips a later pass's histogram layer and copies the previous pass's CPU measurement result within the same Pre- or Post-DLSS chain; it never crosses the actual DLSS evaluation boundary. Game Exposure and Adaptive Exposure can filter histogram metering; Local Exposure uses only its spatial neighborhood weights and has no mask, depth, or percentile filters.

The two grades are symmetric around the model: `PrepareModelInput` fuses Input Grade into the downsample/preparation dispatch when the pre-grade debug view is not selected; otherwise `GradeModelInput` retains the separate snapshot-and-grade path. `GradeModelOutput` grades the model output in place before compose. Both operate in the model's sRGB-encoded SDR domain, so every later step consumes them as if the model had produced the graded SDR. `Apply Edit to Pre-Grade Image` compares model output against the actual post-`GradeModelInput` image, then applies that residual to the image before the grade section. It remains active at model intensity zero, where the input grade itself does not leak into the composed image. In HDR, local exposure is preserved in this option path by reconstructing the base with the global-only inverse carrier scale. Ordering constraint: `BuildImportanceMask` runs **before** `GradeModelOutput`, because the mask must compare the graded model input against the *ungraded* model output — it measures the model's own edits, never the pipeline's grading. No grade is applied to the composite or the backbuffer inside a pass; the separate `grade_backbuffer` setting still grades the swapchain image in the Release stage.

### Backbuffer injection

At ReShade present, enabled Backbuffer passes run independently of NGX hook availability. When a game does not use DLSS, Pre-DLSS and Post-DLSS receive no evaluation callbacks, but Backbuffer passes can still process the swapchain image:

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

The generator resolves include order and include-once behavior while producing each root's fragment list, but it stores source segments only once across roots. `generated/expanded_shaders` is a build-tooling output used by FXC verification and optional precompilation; its expanded shader bodies are not embedded into the runtime source table. Runtime compilation is the default. `PrecompileShaders=false` keeps the generated bytecode table as an empty-count sentinel in `generated/shader_bytecode.cpp`. Setting `PrecompileShaders=true` runs `tools/precompile_shaders.ps1`, generates `.cso` data and writes `generated/shader_bytecode_precompiled.cpp`, which the project compiles instead of the sentinel; `CompilePipeline` then uses those bytes instead of calling `D3DCompile`.

### Build-time verification

`VerifyShaders=true` is the default. `tools/verify_shaders.ps1` locates `fxc.exe` automatically from the installed Windows SDK when `FxcPath` is not supplied, compiles every `.hlsl` root as `cs_5_1`, and forwards actual compiler warnings/errors. The generator expands local includes while honoring `#pragma once`, then writes pragma-free root sources under `generated/expanded_shaders`; verification and optional precompilation use those expanded files so FXC never sees its unsupported `#pragma once` directive. It writes objects to a temporary directory and deletes them after each verification pass. Successful FXC object-save messages are intentionally not part of the build output.

The project selects the latest installed Windows 10 SDK using `ToolLocationHelper::GetLatestSDKTargetPlatformVersion('Windows', '10.0')`. The `FxcPath` property is derived from `WindowsSdkDir` and that SDK version after the normal MSVC property import; it remains overrideable from MSBuild.

### Root signature and descriptor contract

`ComputeEngine::CreateRootSignature` is the single root-signature definition for all compute passes. It has one descriptor table containing:

  | Resource | Registers | Count |
|---|---|---:|
  | SRV | `t0..t3` | 4 |
| UAV | `u0..u1` | 2 |
| CBV | `b0` | 1 |
| Static sampler | `s0` | 1 |

The static sampler is linear-filtered and clamp-addressed. There is no `s1`.

Every `Dispatch` provides exactly `kSrvCount` SRVs and `kUavCount` UAV slots. A pass that uses fewer resources repeats a valid SRV; an unused UAV slot may be null and is bound to the same resource as `u0`. `BindAndDispatch` owns the descriptor and constant-buffer ring mechanics; individual passes only choose the resources and constants. The 256-byte-aligned constant upload buffer remains persistently mapped, and one CBV per ring slot is created during initialization rather than mapping/unmapping and recreating the CBV on every dispatch.

The descriptor table shape and register assignment are fixed. A shader must not invent a new register or sampler contract locally. `GainMapSplit` uses both UAVs to generate the local and global HDR carriers in one dispatch.

### Shared cbuffer

`shaders/common/params.h` is shared between HLSL and C++. `gpu/compute_constants.h` checks the C++ side of the layout.

The 512-byte CBV is:

| Offset | Block | Size |
|---:|---|---:|
| 0 | `Width`, `Height`, header padding | 16 |
| 16 | `ShaderBlock` | 32 |
| 48 | `GradeBlock` | 96 |
| 144 | `ResidualBlock` | 32 |
| 176 | `HdrBlock` | 80 |
| 256 | `HistogramBlock` | 32 |
| 288 | `TemporalBlock` | 96 |
| 384 | `LocalExposureControlBlock` | 32 |
| 416 | alignment padding | 96 |

The resulting `ComputeConstants` size and `BindAndDispatch` stride are both 512 bytes, rounded to the D3D12 constant-buffer alignment. `TemporalBlock` owns the photometric/depth thresholds, maximum motion displacement, motion-vector coordinate metadata, and guide subrect metadata. `TemporalBlock.HistoryControl.x` selects whether history is committed from the validated candidate or from a fresh model evaluation; `.y` is the weight of the fresh edit when it is accumulated into the stored one. Temporal kernels use a dedicated `TemporalBlock`; they do not borrow HDR or generic shader-parameter slots. `HdrBlock.LocalExposureParams.x/y` carry the local-exposure radius and sample count; `.z` is reserved and kept zero. The `LocalExposureControlBlock` occupies previously unused trailing cbuffer space and carries the directional curve, detail-retention, and protection controls without changing existing block offsets.

The shader-side aliases in `bindings.hlsli` turn fields such as `hdr.Exposure` into the semantic names used by shader code (`kExposure` and so on). The strength settings (`PassSettings::strength`, compose/settings.h) are written field-by-field into `ShaderBlock.Params0` by `ComposeOutput` in compose/compose.cpp. `Params1.w` carries the per-dispatch `Apply Edit to Pre-Grade Image` flag for composition. In HDR mode, when local exposure is active, that option transfers the edit onto the local-exposed pre-grade base by undoing only the global carrier scale; the normal path continues to use the original HDR source and packed carrier-to-HDR multiplier.

The same option means the model is fed the graded image and the edit it produces lands on the pre-grade image. `compose/steps.cpp` selects the graded endpoint as the residual reference, and the SDR compose evaluates highlight/shadow region protection against that endpoint rather than the pre-grade base, because protection has to be weighted in the domain the edit was measured in. The residual stays a ratio in the graded domain: the model reconstructs a plausible image, so its edit has to land as the model produced it, and inverting the grade first would move the result away from the model's intent rather than toward it.

The layout intentionally avoids `float2`, `float3`, `packoffset`, platform headers and HLSL-only utility code in `params.h`. Every nested block is explicitly 16-byte aligned and padded.

### Shared filtering and reconstruction

Each grade section can apply joint-bilateral 3x3 edge-aware softening before its grade curve and optional AMD FidelityFX CAS sharpening afterwards. CAS is deliberately limited to linear SDR inputs because the official filter expects normalized linear values; it is skipped for PQ/scRGB. CAS headers are expected at `third_party/FidelityFX-CAS/ffx-cas/ffx_a.h` and `ffx_cas.h`. The shader generator permits external includes only from that allowlisted directory. Add the FidelityFX-CAS repository as a submodule before building this feature; the addon repository does not vendor or update the dependency. Grade tone controls operate on OKLab lightness, while saturation and vibrance scale OKLab chroma. SDR contrast uses a bounded S-curve anchored at black and white. When a grade leaves the output gamut, a short chroma-reduction search preserves perceptual lightness and hue better than clipping individual channels. HDR10 grading converts between Rec.2020 and the OKLab reference primaries, while retaining over-white HDR values.

Local Exposure uses an adjustable radius and 4–48 spatial taps. Its base/detail operator adapts log luminance toward the midtone pivot, with independent shadow/highlight amounts and transitions, protections for bright and dark details, and a detail-retention control. Local Exposure samples the source and evaluates the SDR luminance transfer at each neighborhood tap, preserving the established sampling order.

The model input downsample is `BoxAreaAverage` from `shaders/common/box_filter.hlsli`. It uses exact source footprints and `Texture2D.Load`. Common exact integer ratios up to 4:1 use fixed equal-weight loads; arbitrary scales and fractional edge coverage retain the general footprint path. The integer path is mathematically equivalent, with only summation-order rounding differences.

The non-HDR `PrepareModelInput` path therefore treats the source as already in the model's target sRGB space and passes the averaged RGB through unchanged. The HDR carrier path averages the linear carrier, applies the reversible model highlight/gamut fit, and applies `LinearToSrgb` before the model sees it. Normal colors are unchanged; super-whites are smoothly compressed toward white so emissive highlights keep a white/yellow core.

Both SDR and HDR compose paths use a fixed joint-bilateral 3×3 reconstruction guided by the full-resolution source. The per-pass Upsample Sharpness setting controls guide-luma similarity; the kernel size is fixed to avoid expensive larger neighborhoods.

Every grade section can apply joint-bilateral 3×3 edge-aware softening before the grade curve and optional AMD FidelityFX CAS sharpening afterwards. CAS runs as a separate dispatch so it sharpens the graded result without recomputing the softening filter per CAS tap. It is deliberately limited to SDR inputs because the official filter expects normalized linear color; it is skipped for PQ/scRGB. CAS headers are expected at `third_party/FidelityFX-CAS/ffx-cas/ffx_a.h` and `ffx_cas.h`. The shader generator permits external includes only from that allowlisted directory. Add the FidelityFX-CAS repository as a submodule before building this feature; the addon repository does not vendor or update the dependency.

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

Temporal reprojection runs after `GradeModelInput` and before `FillEvaluateParams`/NGX evaluation on each pass. It expresses the model's per-channel edit as bounded log gain, reprojects the stored edit with the current motion vectors, and scores how far that reprojected value can be trusted.

**Confidence is continuous, and each term is a deadzone.** The score combines agreement with the stored edit's own neighbourhood in YCoCg, photometric agreement between the current and reprojected previous input, and a depth-discontinuity test. Each term is full confidence inside half its tolerance and falls to zero at the tolerance. A term that leaks weight on every pixel becomes a periodic gain step, because a reuse frame sits between two full-weight fresh frames. Depth is a disocclusion gate, not a confidence measure, so it is weighted the same way rather than scaling linearly with the depth error.

**The reprojected edit is applied at full weight above the confidence knee.** Linear attenuation would dim the edit on every reuse frame while the fresh frames apply it fully, which is a gain step at the cadence rather than a smoother result. Above the knee the edit lands unchanged; below it, the pixel fades to the current input.

**Variance clipping scores the edit, it never rewrites it.** The stored edit is written back as history on reuse frames, so clipping the value feeds back and decays the edit toward its local mean, then snaps back on the fresh frame. An edit field is piecewise smooth with hard breaks at object edges; mean-clamping it dims the correction exactly where it matters.

**History is sampled with 5-tap Catmull-Rom, clamped to the min/max of its taps.** The spline is C1, which removes the derivative step bilinear leaves at texel boundaries under sub-pixel motion. It also overshoots at edit-field discontinuities, and that overshoot is a per-frame gain spike, so the result is clamped to the range of the five taps it was built from.

**Motion vectors are read from the centre texel and widened only when the centre has no data.** The widened pick takes the largest-magnitude neighbour: a hole beside a moving object belongs to that object, and taking the smallest vector there snaps the silhouette back to the background motion.

**The fresh edit is accumulated into the stored edit, and every frame renders `current * exp(stored edit)`.** A refresh frame runs NGX, extracts the fresh edit, and blends it in; a reuse frame reprojects the stored edit. A hard replace makes the applied edit jump to the new value in one frame, and the jump grows with the reuse window because the model's own temporal history has been starved for that long. Blending bounds the jump to a fraction of the difference per refresh. Where the reprojected prediction has low confidence it is not a usable reference, so the accumulation falls back to a full replace. The current frame is always the base; the edit only steers it.

**The cadence sets the period of the frame-time step, not its size.** A refresh frame costs substantially more than a reuse frame, and no image-side treatment changes that. `Adaptive` and `Every 2` use the short window because a step every other frame beats faster than the eye resolves judder; `Every 4` is the explicit maximum-reuse option. The invalid fraction does not gate reuse: it is a population statistic, and gating on it disables reuse whenever the scene moves, which is most of the time. It only shortens the reuse window, and that shortening latches with hysteresis so a wobbling fraction cannot flip the cadence every frame.

Any unavailable history, failed GPU step, missing fresh guides, or feedback above the configured invalid percentage fails open to a fresh NGX evaluation. The feedback readback is fence-delayed and asynchronous; the render path does not wait for the CPU to learn the invalid fraction.

The photometric test compares current and motion-reprojected previous input luma against an absolute-plus-relative allowance. Depth validation compares current depth against the previous depth at the reprojected coordinate using the larger of the absolute and relative limits. Motion beyond the maximum displacement is rejected. The default limits are absolute/relative photometric `0.06`/`0.10`, absolute/relative depth `0.005`/`0.03`, maximum displacement `40` working-resolution pixels, and maximum invalid/mismatched fraction `15%`. Lower thresholds reject more history and increase fresh model evaluations; higher thresholds trade validation strictness for reuse.

Debug views show the pre-depth candidate, post-depth validation result, and per-channel reprojected log edit. Confidence is stored in alpha in the temporal working textures; debug views translate it to readable colors. They are rendered on the pass command list while the produced resources are still in known states.

**The Temporal Reprojection category ends with a live readout.** It reports reuse or refresh for the current frame with its age over the effective cadence, the reuse ratio since load, the invalid fraction from the GPU readback, mean confidence and mean absolute log gain of the reprojected edit, and the smoothed frame interval for each class with the step between them. That step is the judder amplitude, and nothing else in the addon exposes it. `temporal/stats.h` holds the snapshot; the temporal stage publishes it once per frame and the overlay reads it. The GPU side accumulates the invalid count, a marker, the confidence sum, and the edit-luma sum in one small buffer that the reproject dispatch clears and the validate dispatch fills.

### Streamline

**Do not record addon-owned work through a Streamline proxy.** Resolve the ReShade wrapper and Streamline interposer to the native object before the addon creates resources, records barriers, or restores state.

**Pass the game's command list to NGX unchanged.** Proxy unwrapping is for the addon's own work; the command list handed back to Streamline/NGX remains the one the host supplied.

**Track state using the same native command-list identity that the hooks see.** A wrapper pointer and the underlying native list are different keys; mixing them makes `RestoreCommandList` silently miss the captured state.

**Keep references to state objects used for restoration.** Root signatures, PSOs, descriptor heaps and related objects can be released and recreated by the game, especially around swapchain changes; storing raw pointers is unsafe.

**The NGX capability parameter block is process-global.** Do not call `Reset()` on it to clear only DLSSNR keys; Streamline may share the same block. The addon writes the keys it owns without clearing the entire object. `Restart Addon` is also deferred: it stops recording addon work and guide-clone copies, queues a marker on the game queue, and releases NGX features and addon resources only after that marker completes. A UI callback must never synchronously release a feature that an in-flight command list may still reference.

### NGX / feature lifecycle

**Hook NGX before the first DLSSNR `CreateFeature`.** `TryHookNgx()` loads `nvngx_dlss.dll` if necessary so the hook is in place before feature creation. `CreateFeature` records handle-to-feature-id mappings used by `EvaluateFeature`.

**The DLSSNR feature is rebuilt only when its full-resolution build size changes.** Working scale changes only the per-frame evaluation size; they do not recreate the feature.

**Model tuning is applied at feature creation and again for evaluation.** Preset, intensity, local structure/tone, skin structure, auto-mask and UI-correction controls are not feature-rebuild triggers.

**The FP16 pipeline preference never releases the feature.** It selects the format of the addon's own intermediate buffers and is not passed to NGX, so the feature does not depend on it. Releasing it from the preference path runs while the frame that still references the feature is in flight, which takes the streamline v1 runtime down; the buffers are recreated on their own, and the next evaluation resets the model history.

**The feature creation frame is skipped for evaluation.** `FeatureLifecycle::ConsumeJustRebuilt()` gives the caller one frame to let a new feature initialize before the next evaluation.

**The model's build dimensions are full-frame dimensions.** `working_scale` belongs to the model's per-frame input/output resolution and NGX evaluation parameters, not feature creation.

**NGX feature creation is feature 18.** The addon uses the Super Sampling feature id for DLSSNR and maintains its own parameter keys under the `DLSSNR.*` namespace.

### Model loading and parameter block

`model/runtime.cpp` owns direct loading of `nvngx_dlssnr.dll`, patches the model module's import of `KERNEL32!GetModuleFileNameW` to satisfy its caller check, resolves `NVSDK_NGX_D3D12_Init_Ext`, `CreateFeature`, `EvaluateFeature`, and `ReleaseFeature`, then initializes the model against the addon module directory.

`NgxParameterBlock` obtains the driver NGX capability parameter block from `_nvngx.dll`. The float setter slot is probed from candidate vtable slots at runtime because the binary interface does not expose a stable named C++ method for the required setter. The block is not reset or cleared wholesale. `FillEvaluateParams` must write every owned guide field on every evaluation, using null resources and zeroed guide metadata when no guide is valid; leaving a field untouched can preserve a dangling resource pointer in NGX’s persistent shared parameter block.

### HLSL

**Keep `groupshared` variables at global scope.** HLSL rejects function-local `groupshared` declarations.

**Initialize HLSL result/output variables at function entry and return once to avoid FXC X4000 warnings.**

**Avoid reserved or ambiguous identifiers.** HLSL's parser can report misleading errors for names that collide with language keywords or intrinsics.

**Be explicit with color types.** Functions that operate on colors take and return `float3`; do not rely on HLSL scalar-to-vector coercion.

**Use explicit matrix/vector multiplication.** `mul(matrix, vector)` expresses the intended linear algebra; componentwise operators do not.

**Keep shared cbuffer math-free.** `shaders/common/params.h` is a layout contract for two languages, not a utility header.

**Share local-exposure math between split and compose support.** `shaders/common/hdr_local_exposure.hlsli` is the single implementation of the local-exposure field calculation used by both the HDR gain-map split and the full-resolution exposure map. This prevents the map from drifting away from the exposure field that produced the model input.

**Include shared helpers before using their aliases.** `bindings.hlsli` assumes the `params.h` field names exist. `color.hlsli`, `grade.hlsli`, and other helper files should only assume the functions they explicitly include or receive through earlier root includes.

### HDR

**Format detection is explicit and overridable.** Auto detection treats `R16G16B16A16_FLOAT` and `R11G11B10_FLOAT` as scRGB, `R10G10B10A2_UNORM` as HDR10, and 8-bit RGBA/BGRA formats as SDR. `HdrSettings::format` can override auto detection.

**Depth falls back to ReShade Generic Depth only when NGX supplies none.** The addon reads Generic Depth's published `selected_shader_resource` at `reshade_begin_effects`, retains the underlying D3D12 resource, and makes it available as a fallback guide. `Invert Fallback Depth` affects only this substitute; it does not override DLSS's own inverted-depth flag. As Generic Depth publishes its selection during effects setup, selection changes are consumed at the next addon pass boundary.

**Exposure is measurement state, not compose state.** Histogram-derived exposure changes what the model sees; HDR compose reads the untouched source directly and applies the model residual in HDR.

**Game Exposure is one layer in the exposure stack.** When enabled, the addon captures the game's NGX `ExposureTexture` before `EvaluateFeature`. If it is a usable 1x1 texture, a compute pass samples its scalar and an asynchronous GPU readback updates HDR state without waiting on the CPU. Game Exposure remains the base when available; its optional bias and bounded tuning use that value as their anchor. Adaptive Exposure can add a bounded histogram-derived correction, and Local Exposure can apply a spatial correction afterward. If game exposure is unavailable, Adaptive Exposure can provide the global exposure on its own. The Status panel reports availability and the last sampled value; `Debug View -> Game Exposure` shows the sampled value in stops.

**Exposure filters use the same pass's previous-frame Importance Mask snapshot.** When enabled and valid, that mask constrains only histogram computations whose filter options select it. Local Exposure has no Importance Mask, Depth, or Percentile filter settings. Each pass generates its own mask after its model evaluation and publishes it only at Present, so it cannot affect that pass's current-frame metering.

**Adaptive and Local Exposure are independent layers.** Adaptive Exposure maps the filtered histogram to its midtone target and can provide global exposure by itself or a bounded correction on top of Game Exposure. Local Exposure computes a log-luminance base/detail correction using a configurable radius and 4–48 spatial taps. The target midtone is a pivot: shadow lift and highlight reduction have independent strengths and transition ranges, while bright-detail and dark-detail protection prevent one side of the local contrast from being dragged along with the other. Detail Retention controls the amount of within-neighborhood log-luminance detail preserved. Local Exposure still has no mask, depth, or percentile filters. `Restore Model Exposure` is a separate opt-in checkbox. Its `Restore Scale` slider interpolates in log exposure between the exact local multiplier at 0 and a uniform geometric-mean multiplier at 1. The local endpoint reads the multiplier directly from the carrier pair without a blurred map; the global endpoint uses a tile reduction followed by a single-group reduction. Intermediate values blend the two spatial scales. The Game and Adaptive layers retain their histogram-filter options.

**The HDR carrier and source have separate responsibilities.** `GainMapSplit` writes RGB linear SDR carrier plus a direct reciprocal of the RGB carrier scale to `R32G32B32A32_FLOAT`. The multiplier is stored linearly in alpha to avoid an unnecessary log2/exp2 precision cycle; gain statistics and the debug view convert it to stops. The paired pre-desaturation/inverse operations are reduced algebraically to a scalar RGB multiply so saturated super-whites do not suffer cancellation noise in near-zero channels. No additive offsets or fixed super-white ceiling are applied. HDR composition reads the untouched source resource and decodes it into float shader values, then transfers the model residual onto that source; it never reconstructs the original from the SDR carrier. The HDR model input/output working textures use `R32G32B32A32_FLOAT` to reduce precision loss in the reversible highlight transform. The opt-in `FP16 Reference` setting changes only the unlifted working-resolution composition reference to `R16G16B16A16_FLOAT`; SDR continues to use the source-compatible model format. The published composite remains in the game's native format, whose representable range still applies. The model-gamut mapping keeps valid source super-whites below model-domain white with a piecewise logarithmic mapping; its inverse also handles model-output overrange. This mapping is only for the model-facing carrier. The original source and its full carrier-to-HDR multiplier remain separate, so model-gamut fitting never caps the final HDR result.

**`HDR Roundtrip Error` isolates the carrier transfer.** It compares the original decoded full-resolution HDR source directly with HDR reconstructed from the RGBA32F carrier and multiplier. It does not run model processing, input grade, or output-format encoding. Alpha stores the reciprocal of the scalar used to create RGB from the original HDR source; it is the carrier-to-HDR reconstruction multiplier. Gain statistics and the gain-map debug view display its log2 value in stops. HDR composition itself uses the untouched source and therefore does not inherit carrier reconstruction error. `HDR Roundtrip Diagnostic` is a different comparison: it uses the actual prepared model input and compares the composed HDR mapped back to model SDR with the model output in the ungraded model-linear domain. Nonzero edge differences can be legitimate because HDR composition preserves full-resolution source detail; Working Scale changes the comparison. It is not an isolated carrier-transfer test.

**Local exposure is kept out of the HDR residual reference.** When Local Exposure is active, GainMapSplit produces both the normal locally adjusted carrier and a global-only carrier with the same BT.2446 mapping. The global-only carrier is downsampled and model-gamut-fitted through the same preparation path as the actual model input, and receives the same input grade. HDR composition compares the model output against this unlifted reference, so local lifting does not flatten the measured edit. It is constructed before downsampling rather than by dividing a filtered image afterward; this preserves the exact sampling footprint and avoids detail loss. The normal carrier still stores the reciprocal of its complete global × local scale, which cancels that scale when transferring the relative edit onto the untouched HDR source.

**HDR round-trip diagnostics.** `HDR Roundtrip Error` checks only source-to-carrier-to-HDR reconstruction, independent of Working Scale, model processing, and input grade. A uniform neutral image means the carrier reconstruction itself is accurate. `HDR Roundtrip Diagnostic` compares model and composition domains, including the preservation of full-resolution source detail. Its bottom-right panel is a composition difference, not a pure transfer error; input grade is removed before the model output is compared. Use Error to test carrier math and Diagnostic to inspect composition behavior.

**Render debug images at pass time.** The resource state is trustworthy while the producing pass still owns the command list and has just produced the resource. `MaybeRenderDebug` renders into `FrameDebugContext::output`; the Release stage only copies that already-rendered output to the backbuffer.

**The debug target uses the swapchain's format and dimensions.** Pre-DLSS and Post-DLSS sources may have a different format or render extent. `DebugDisplay` renders into a backbuffer-sized debug texture and samples the selected source over its normalized extent, so the Release stage can copy it directly to the swapchain.

**Debug selection is global, rendering is pass-local.** The Debug section's `ctl` value is snapshotted into each `PassRun`; only the active pass renders the selected view.

**The debug image lives for an evaluation, not for a presentation.** Each evaluate entry point clears it before its passes run; Present never does. A frame can be presented more than once per evaluation — frame generation presents twice — and clearing at Present strips the overlay from every presentation after the first.

### Settings and UI

**Guide/output cloning is one shared advanced option.** `CloneResources` defaults to false and is read atomically by the NGX hook at the next evaluation boundary. Off uses current game resources directly; on clones current depth/motion and, only for Post-DLSS, the post-evaluation color output. The same toggle governs Pre-DLSS, Post-DLSS, and the guides available to Backbuffer, so it can compare copy-path costs without reverting to stale guides.

**Working scale 1 avoids identity scaling work.** For SDR inputs with no input grade and equal input/working dimensions, `PrepareModelInput` passes the original resource to the model instead of dispatching an identity downsample. SDR/HDR composition still dispatches because it applies the model residual and writes the final image; the shaders bypass reconstruction when working and output sizes match. The HDR carrier preparation is not bypassed because it performs required color-domain conversion.

**Brightness mode `Exposure` operates in linear light.** It multiplies all three linear RGB channels by `2^stops` before output encoding, which changes scene intensity while preserving chromaticity until gamut limits/clipping. It replaces the former Lift mode; the shared mode index is retained so profile serialization remains stable. Gain, Gamma, and Soft keep their separate luminance-shaped curves.

**Level ranges are bounded by the curve, not by the control.** Black Level spans `-0.40..0.20` and White Level `0.70..2.00`: past roughly `0.2` the black point consumes the whole shadow range, and below roughly `0.7` the white point has nothing left to compress. The clamps in `shaders/common/grade.hlsli` mirror `grade/settings.h` and move with it; a wider UI range with narrower shader clamps silently does nothing.


**Saved profiles use stable `ProfileId` keys.** `UserPresetOrder` is the comma-separated display order and `UserPresetNextId` preserves the allocation high-water mark. A profile's name and enabled state live at `UserPreset.<id>.Name` and `.Enabled`; its `ctl` settings, including the typed `Pass Order.ids` integer list, live under `UserPreset.<id>.Data`. The pass-order setting is loaded before instanced sections, and its stable IDs supply the instance keys for pass settings. Profile persistence excludes settings marked global-only; loading a profile preserves the current global overlay UI state. Rename edits the selected profile name in place and confirms only after validation, while Enter saves and Escape cancels. Deletion removes one ID and its schema-defined keys without shifting another profile's data.


**HDR composition uses Gain-Map Residual exclusively.** HDR Pre-DLSS and Post-DLSS passes use the packed per-pixel carrier-to-HDR multiplier to transfer model edits back to the untouched full-resolution HDR source. The former HDR Ratio mode and selector are removed. SDR composition, including Present/backbuffer passes, retains its existing log-ratio residual path and settings.

**HDR exposure is configured as a layer stack.** The HDR UI has separate `Game Exposure`, `Adaptive Exposure`, and `Local Exposure` tabs. The default stack order is Game Exposure base, Adaptive Exposure correction, then Local Exposure. Game and Adaptive Exposure retain their Importance Mask, Depth, and Percentile Range filter combinations. Local Exposure defaults to Strength 0.5 and has a configurable source-pixel radius, 4–48 spatial taps, separate shadow-lift/highlight-reduction strengths and transition ranges, bright/dark detail protection, edge-aware log-luminance weighting, and a detail-retention control. The midtone pivot is stable: with the default full protection, shadow lifting does not raise pixels above the pivot and highlight reduction does not darken pixels below it. Restoration is disabled by default and has its own checkbox. Its spatial-scale slider is 0 for exact per-pixel cancellation of the local multiplier and 1 for a uniform global correction; intermediate values blend the multipliers in stops. The global endpoint uses a GPU tile reduction followed by a single-group reduction, and the old blurred tile map is removed. The exclusive Median and combined Auto Exposure + Local modes are removed.

**Importance Mask is derived per pass from the pass result, not a separate model evaluation.** It compares that pass's raw model output with the same-resolution model input actually fed to NGX, including input grading and Local Exposure. The mask reads that input directly instead of making a separate ungraded/unlifted copy. The pass's previous-frame mask is used by mask temporal filtering and exposure only; it is separate from model-edit reprojection history. The new mask is published at Present.

**HDR and Importance Mask settings and runtime state are per-pass.** The stable `PassId` is the instance key for those schemas, exposure trackers, histogram/readback resources, and mask history. Reordering tabs never moves those histories to another pass. **Pass targets are independent per-pass settings.** Runtime execution groups enabled passes in frame order: all Pre-DLSS passes before the real NGX evaluation, all Post-DLSS passes after it, then all Backbuffer passes at ReShade Present. Within each target group, passes run in pass-tab order. Each pass target is persisted as `PassN.TargetStage`; the prior two-value `PassN.InjectionStage` key is read only for migration. This means a pass with a higher tab index can run in an earlier stage than a lower-index pass; chaining applies within each target group.

**The debug selection is not per-pass.** The active UI tab selects which pass renders the shared debug view. Temporal reprojection adds `Temporal Candidate Status`, `Temporal Validation Status`, and `Temporal Reprojected Edit`; unavailable history is blue, accepted pixels are green, photometric mismatches are yellow, depth/guide rejection is magenta, and rejected/no-candidate pixels are red or dark in the edit view.

**Synthetic Motion Guides are optional and pass-scoped.** When enabled and no native motion texture is available, the pass searches the previous image at working resolution and scores candidates against current/previous depth to reject cross-surface matches. The generated current-to-previous vector field is supplied to NGX and temporal reprojection; ambiguous/disoccluded pixels fall back to zero motion. The extra optical-flow dispatch is opt-in because of its GPU cost, and requires a usable depth guide.

**Temporal reprojection is configured per pass and sits before Advanced/model grading.** The `Temporal Reprojection` section follows Strength and precedes the Advanced settings and grade-boundary sections. Mode defaults to Off. Its controls expose absolute/relative photometric thresholds, absolute/relative depth thresholds, maximum working-resolution motion displacement, and the maximum invalid-pixel percentage that shortens the reuse window. Defaults are 0.06/0.10 photometric, 0.005/0.03 depth, 40 working pixels, and 15% invalid/mismatched pixels. Threshold changes are included in the temporal-history signature and invalidate stale history. The category ends with the live reprojection readout, drawn as a schema hook row rather than a separate section, so its collapsible is the category header. The pass-tab row wraps when buttons no longer fit beside the toolbar.

**ImGui IDs are scoped by section identity and instance identity.** `ctl::DrawSection` pushes a scope derived from the schema section name and `InstanceKey`, so equal visible labels in different sections or instances do not share collapsing-header IDs. All persisted overlay open/closed states are schema settings in `Overlay UI State`, marked global-only so profile loads cannot change them. The overlay callbacks still implement the same header interactions, but read and write the ctl store rather than manually maintained `Settings` booleans.

**Inline toggles belong to the target setting.** Chain `.InlineToggle(bool_setting)` on the target declaration. `ctl` validates the boolean source, draws it to the left of the target label, and hides its standalone row.

## HDR model path

The HDR path is gain-map based and runs on the Pre-DLSS and Post-DLSS targets. Backbuffer passes use the existing SDR composition path.

1. `HdrState` detects the source format and resolves the enabled exposure layers. Game Exposure is the base when its game-provided value is valid; Adaptive Exposure contributes its filtered histogram correction or supplies global exposure when Game Exposure is unavailable. Local Exposure estimates a weighted neighborhood base in log-luminance space using the configured radius and spatial taps. The midtone pivot defines the neutral point. The neighborhood uses a bilateral range weight in log-luminance space to reduce cross-edge halos, then selects a proposed lift or reduction. Pixel luminance gates the correction so bright outliers are not lifted with dark surroundings and shadow details are not darkened with bright surroundings unless the corresponding protection control permits it. `Detail Retention` controls how much log-luminance detail is retained around the adapted local base. The same local-exposure math is used by the gain-map split and effective-exposure diagnostic map. `Restore Model Exposure` is a separate opt-in operation after model evaluation: the per-pixel mode divides linear model-output RGB by the exact local multiplier derived from the carrier pair; the global mode derives the pixel-weighted geometric mean multiplier on the GPU and applies its reciprocal uniformly. Intermediate modes interpolate in log exposure. This preserves more model-reconstructed spatial brightness as the control approaches global mode. It preserves channel ratios, though the model and gamut mapping can still change the result. Importance Mask compares the actual model input after input grading and Local Exposure with the raw model output.
2. Each Pre/Post-DLSS pass generates its own `Importance Mask` from the actual model input (after input grading and Local Exposure, when enabled) and its raw model output. A per-pixel classifier is computed once, then a second dispatch performs neighborhood voting and temporal smoothing; this avoids recalculating each neighbor's photometric classifier for every output pixel. It reads the same model-input resource NGX evaluated, so there is no separate ungraded/unlifted mask reference. Each mask is not consumed until the next Present boundary; backbuffer passes are excluded. It does not evaluate a separate model feature.
3. `GainMapSplit` converts the full-resolution HDR source into an RGBA32F carrier: RGB is the linear SDR base and A is the reciprocal of the complete carrier scale. BT.2446-C source parameters are converted to derived `k2`/`k4` coefficients once on the CPU when the HDR uniform snapshot is built, then uploaded to every relevant shader pass. Scene-white controls the carrier; no additive gain offsets or gain ceiling are applied. If Local Exposure is enabled, a second invocation with Local Exposure disabled writes a global-only carrier for the residual reference.
4. `PrepareModelInput` performs exact Box Area downsampling. For the HDR carrier path it applies the reversible model highlight/gamut fit and sRGB encoding. When Local Exposure is active, the global-only carrier uses this same path to create a separate unlifted working-resolution reference for HDR composition. Importance Mask instead reads the actual prepared model input after input grading; it never uses the composition reference.
5. The model feature evaluates at the selected working scale while being built at full frame resolution.
6. `ComposeModelOutputHdr` uses fixed joint-bilateral 3×3 reconstruction to upsample model-domain images at full resolution. With Local Exposure enabled, the composition residual uses the unlifted reference; otherwise it uses the prepared model input. HDR composition retains the original Gain-Map Residual math: the packed per-pixel carrier-to-HDR multiplier transfers the relative model edit onto the untouched full-resolution HDR source. It does not invert the nonlinear super-white fit to derive an absolute endpoint delta. Present/backbuffer passes continue to use the existing SDR log-ratio compose path.
7. `EncodeHdr` writes the reconstructed result back into scRGB or HDR10 as required by the source format. The gain-map residual compose path measures the model edit in the model domain, then uses the packed carrier-to-HDR multiplier to transfer that edit into the original source's scene-referred range without inverting the highlight/gamut fit.

The SDR compose uses the same fixed joint-bilateral 3×3 reconstruction but does not reconstruct an HDR carrier; it applies the residual in the source SDR space and uses the shared grade/compose helpers.

## Logging, tracing, status, and crash trace

### Log

The ReShade log is for lifecycle events and actionable failures: NGX hook installation, format changes, feature creation, shader compilation diagnostics, pass failure reasons and unrecoverable command-context errors.

Shader diagnostics are identified by pass name, root shader path and severity. Compiler warnings/errors are deduplicated by pass + severity + exact diagnostic text, so background compilation and a post-ready fallback compile do not flood the log with the same message.

Pass failures are represented by `PassSession::Fail()` and surfaced through the status snapshot. Startup conditions such as the compute worker not being ready are skipped rather than reported as shader failures.

### Status

`StatusSnapshot` is the UI-facing current state: addon enabled state, runtime initialization state, guide availability, HDR/gain-map measurements, post-DLSS block reason, and per-pass runtime status. It is refreshed by `AddonController` after frame processing.

Use Status for information that must remain visible as current state. Do not replace it with a per-frame log line.

### Pipeline trace

Set `DLSSNR_TRACE=<path>` to enable the command/event trace. The macros and the writer are compiled in by default; `DlssnrTraceEnabled=false` removes them from the binary. The trace maintains separate counters for `PreDLSS`, `PostDLSS`, and `Present` streams, assigns stable resource ids in first-seen order, and records frame boundaries. `TRACE_CMD` covers resource transitions, barriers, copies and dispatches; `TRACE_EVT` records control-flow decisions such as skips and failures.

Because trace lines are intentionally flushed immediately, the file is suitable for diagnosing the final commands leading into a crash. The trace should still be bounded to a deterministic test scene because a full frame contains many transitions and dispatches.

When tracing is enabled, `ctl::TraceRecorder` enumerates registered sections, instances, and settings from the built schema and emits initial values plus changed/removed keys automatically. Setting names use `Section.setting` for single-instance sections and `Section[key].setting` for instanced sections. Adding a registered setting therefore automatically adds it to the trace without another hand-maintained snapshot list. `state/trace.cpp` is only an adapter that formats ctl events and traces the few runtime fields that are not settings (`enabled` and `active_pass_id`).

### Crash trace

`crash_trace.h` installs a vectored exception handler and a terminate handler at addon attach time. It writes `%TEMP%\dlssnr_crash.txt`, including fault code, faulting module and offset, a short stack, x64 register context and thread id. The handler is deliberately raw Win32 and allocation-free enough for a broken process, and it chains to the game's own exception handling.

### CPU/GPU profiling

Profiling uses the external Tracy source tree at `third_party/tracy` (the user supplies the submodule; it is not vendored by this project), with `public` and `public/tracy` on the include path. CPU zones are emitted through the `ZoneScopedN` family; `FrameMark` marks the end of each Present. GPU zones are recorded with `TracyD3D12Zone` against a `TracyD3D12Context` created once for the native ReShade D3D12 device and graphics queue; the context is collected at Present via `TracyD3D12Collect`. Use the Tracy GUI to capture and inspect CPU/GPU timelines.

## Extending

### Add a shader pass

1. Add one root `.hlsl` under the appropriate `shaders/<module>/` directory with one `CSMain`.
2. Include the shared headers it actually needs (`common/cbuffer.hlsli`, `common/bindings.hlsli`, `common/registers.hlsli`, plus functional helpers such as `color.hlsli` or `compose_upsample.hlsli`). External shader includes are restricted to the FidelityFX CAS submodule's `ffx-cas` directory.
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

`ShaderBlock.Params0` and `Params1` hold the composition parameters; the cbuffer layout is asserted in `gpu/compute_constants.h`.

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

**Pre-DLSS is skipped under Ray Reconstruction.** RR reads Color through a subrect of a larger allocation and addresses its G-buffer inputs with the same subrect; the compact-Color adapter rewrites Color's geometry and not theirs. Running the stage in that configuration has produced GPU faults. The failure is timing dependent and its root cause is not established, so the stage is skipped rather than risking the device, and the reason is published as `pre_dlss_blocked`. Post-DLSS is unaffected.

The pass target row reflects this: a stage that reports blocked stays selectable but is drawn dimmed, with the reason in its tooltip. Availability is re-evaluated every frame, so the styling follows the game rather than a fixed capability table.

**The optional precompiled shader path is not the default path.** Runtime `D3DCompile` is still the authoritative shipping path unless `PrecompileShaders=true` is explicitly enabled. That means runtime shader source, include resolution and compiler availability remain part of normal deployment.

## Reported but not investigated

These are retained as user-reported symptoms whose root causes are not established by the current code review.

**Model-output grading can be too strong in HDR.** Backbuffer grading is separate and currently behaves differently from grading the model output inside the HDR compose path. The exact intended grade space for the model-output boundary in HDR has not been established as a user-visible contract and should be treated carefully before changing it.

## Build

The project is a Visual Studio x64 DLL/addon build. Release builds use speed optimization and whole-program optimization for every translation unit.

`pch.h` carries the SDK headers plus the addon headers that are both heavy and stable: `profiling/profile.h`, the `ctl` core (`schema`, `types`, `values`), and `d3dx12.h`. `ctl/builtin_settings.h`, `ctl/draw.h` and `feature/settings_store.h` stay out; too few TUs pull them to be worth the rebuild blast radius. Anything that churns costs more in PCH rebuilds than it saves in parse time.

- Solution: `reshade_dlssnr.sln`
- Project: `reshade_dlssnr.vcxproj`
- Configurations: `Debug|x64`, `Release|x64`
- Platform toolset: `v145`
- C++ standard: C++20
- Release output: `build\Release\reshade_dlssnr64.addon64`
- Debug output: `build\Debug\reshade_dlssnr64.addon64`
- Linked system/local libraries: `d3d12.lib`, `dxgi.lib`, `detours.lib`, `d3dcompiler.lib`

MSBuild switches. Every one is a project property with a default, so a build only has to pass the ones it wants to change (`/p:Name=value`); the defaults are what a plain build produces.

| Property | Default | Effect |
|---|---|---|
| `VerifyShaders` | `true` | Run FXC verification before the C++ compile |
| `PrecompileShaders` | `false` | Generate embedded shader bytecode instead of using the empty bytecode table |
| `FxcPath` | auto-detected | Optional explicit path to x64 `fxc.exe` |
| `DlssnrTracyEnabled` | `true` | Compile `third_party/tracy/public/TracyClient.cpp` in and define `TRACY_ENABLE`, `TRACY_ON_DEMAND`, `TRACY_NO_BROADCAST`. Off removes the profiler from the binary; `profiling/profile.h` then expands every zone macro to nothing. |
| `DlssnrProfileVerbosity` | `1` | `DLSSNR_PROFILE_VERBOSITY`. Zones above this level are compiled out; GPU zones need 2 or more. |
| `DlssnrProfileDomainCore` | `1` | One `DLSSNR_PROFILE_DOMAIN_*` per subsystem (`Core`, `Render`, `Resource`, `Async`, `Ui`, `Ctl`). Zero compiles that domain's zones out and leaves the rest. |
| `DlssnrProfileDomainGpu` | `0` | Same, for the GPU zones. |
| `DlssnrTraceEnabled` | `true` | `DLSSNR_TRACE_ENABLED`. Off compiles the pipeline trace out entirely: `TRACE_CMD`, `TRACE_EVT` and `TRACE_CFG` become `((void)0)` and the trace entry points in `pipeline_trace.cpp` become empty functions, so no format string or file writer survives. |
| `DlssnrMutexCompatibility` | `true` | `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`, described below. |

`DlssnrMutexCompatibility` exists because `std::mutex` construction is not ABI-neutral. The current toolset builds the critical section in situ and imports only `_Mtx_lock`/`_Mtx_unlock`; a game that ships an older `msvcp140.dll` (Alan Wake 2 ships 14.34) resolves those against a critical-section pointer that was never allocated, and the first lock faults. The switch makes the constructor call `_Mtx_init_in_situ` on the runtime that is actually loaded, so the layout matches whatever the game brought. Turning it off is safe only when every target runtime is at least the toolset's own version; leaving it on costs a heap allocation per `std::mutex` construction and makes the type non-`constexpr`-constructible.

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

The project expects the ReShade SDK/ImGui sources, NVIDIA NGX headers and the local Detours/D3DX12 dependencies under `third_party/`. A clean checkout needs those repository dependencies present before building.


### Runtime dispatch overhead

`ComputeEngine` retains root-signature and descriptor-heap binding state only within a known command-list recording interval. `PassSession::RunStage`, HDR shared-frame preparation, and every external NGX evaluation invalidate that cache, so a game/NGX mutation or command-list reset cannot inherit stale state. `CommandContext::TransitionBatch` updates the same tracked resource-state map as `Transition`, but submits adjacent transition barriers as one `ResourceBarrier` call; UAV barriers and per-resource before/after tracking remain explicit.


### Runtime redundancy cleanup

The SDR and HDR composition shaders share `JointBilateralUpsamplePair`; the older single-output helper has been removed so the common include contains only the paired path. Unconsumed full/game histogram copies and sample counters are no longer retained on the CPU. `ComputeEngine` uses transparent string lookup for registered and failed shader names, avoiding temporary owning-string construction during dispatch lookup. Trace parameter dumps include the Histogram and Temporal blocks. The shared constant-buffer table above reflects the complete current 512-byte contract.
