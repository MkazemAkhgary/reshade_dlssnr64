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
    pipeline_graph.h/.cpp        // declarative pipeline: stages, step tables, runner
    pipeline_trace.h/.cpp        // DLSSNR_TRACE command-stream trace (regression oracle)
    runtime.h/.cpp               // model loading, patching, NGX init, parameter block
    command_context.h/.cpp       // command allocator/list/fence/barrier/state helpers
    feature_lifecycle.h/.cpp     // feature handle creation and rebuild state
    model_pipeline.h/.cpp        // prepare input, compose output, HDR variants
    pass_resources.h/.cpp        // per-pass textures + scratch pool
    pass_session.h/.cpp          // step implementations + RunStage entry point
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

The frame is described declaratively in `runtime/pipeline_graph.h/.cpp` as a
list of **stages**, each holding an ordered table of **steps**. Stages and
their step tables are data; a step is a named function plus a status. Adding,
removing or reordering a step is a one-line table edit — see *The Pipeline
Graph* below before changing anything.

```text
Game frame:
  ...
  -> game calls NVSDK_NGX_D3D12_EvaluateFeature (DLSS upscaling)
      -> hooked_NVSDK_NGX_D3D12_EvaluateFeature
          -> real EvaluateFeature
          -> DlssGuides::Capture           // clones depth/motion/DLSS output
          -> AddonController::OnPostDlssEvaluate
              -> for each enabled pass at DLSS Output:
                  PassSession::RunStage(PostDLSS stage, ...)
                    // EnsureHdr / HdrBeginFrame / ... / WriteToGameResource
  ...

ReShade present:
  -> AddonController::OnPresent
      -> for each enabled pass at Backbuffer:
          PassSession::RunStage(Present stage, ...)
      -> RunSingleStage(Release stage)         // SignalHdrAtPresent
      -> if a debug view was rendered this frame:
          PresentDebugOutput             // copy frame_debug.output to backbuffer
```

`PassSession::RunStage` is the single entry point for every stage. It builds a
`PassRun` (the per-frame inputs and resolved resources), then hands off to
`PipelineRunner::RunStage`, which walks the stage's step table in order,
checks prerequisites, and interprets each step's `PipelineStatus`. The old
`PassSession::OnPresent` and `OnPostDlssEvaluate` methods no longer exist.

The two injection points run the same step *functions* but not the same step
*order*: Post-DLSS builds its feature after seeding guide states, Present
before. That asymmetry is a property of the existing code and is deliberately
preserved — see the FIDELITY note in `pipeline_graph.cpp`.

---

## The Pipeline Graph

`runtime/pipeline_graph.h` defines the model; `pipeline_graph.cpp` holds the
tables. Read the header comments first — they state the invariants.

**Nodes.** `PipelineNodeId` is a flat enum ordered by dependency depth. A step
may only declare prerequisites that appear *earlier* in the enum, which makes
a dependency cycle impossible to express rather than something to debug later.

**Prerequisites** are the mechanism that makes ordering explicit rather than
relying on table position. `hdr_state.cpp` populates real ones —
`HdrBeginFrame` declares `EnsureComputeEngine`, because it dispatches the
histogram through that engine — and the ordering in `kPostDlssSteps` exists to
satisfy them. The session and controller steps still use `kNoDeps`, because
their ordering is already fixed by the frame's data flow.

The distinction matters: a prerequisite that can never be met does not fail
loudly, it skips the step *silently for the rest of the run*. Bug 1 in the
refactor was exactly this — `HdrBeginFrame` listed before the
`EnsureComputeEngine` it depends on, so HDR was skipped every frame. Hence the
guards below.

**Frame inputs.** A step that dereferences a frame input the caller did not
supply crashes — and because the *same step function* is listed in more than
one stage, while the call sites differ, moving one line between step tables is
enough to create it. Present's call site has a swapchain and no command list;
PostDLSS has the game's command list and no swapchain. Two declarations make
that checkable:

- `PipelineStepDef::required_inputs` — the `StageInput` bits this step
  dereferences.
- `PipelineStageDef::required_inputs` — what this stage's call site promises to
  supply.

`StageInput` is a bitmask (`CommandList`, `Device`, `Swapchain`,
`CommandQueue`, `OutputResource`, `Backbuffer`, `Stage`) with `HasInput` and
`InputsCover` helpers. The `Stage` bit is listed wherever steps read
`run.stage`; the runner sets it itself, so it is documentation of intent rather
than a real precondition.

**Declare reads, not writes.** A step that *assigns* `run.list` / `run.device`
/ `run.backbuffer` is producing those values for later steps, not consuming
them. `AcquireBackbuffer` writes `run.device` and `run.backbuffer`; requiring
either on any stage would be wrong, because no caller supplies them.

**Compile-time guards.** Four `static_assert` families back the tables, so the
mistakes that produced real bugs now fail the build instead of failing at
runtime:

| Guard | Rejects |
|---|---|
| `NodeListsEqual` | a stage's step list in `pipeline_graph.cpp` drifting from the copy in the header |
| `PrerequisitesPrecedeSteps` | a step declaring a prerequisite listed *after* it — the Bug 1 class. Asserted in each TU that owns a step table |
| `NoHdrAwareSteps` | a `requires_hdr_aware` step in a stage that is not `hdr_capable`, so it could never run — the Bug 3 class |
| `StageInputsCoverSteps` | a step listed in a stage whose call site does not supply an input it declares. Asserted in each TU that owns a step table |

`PhasesCover` additionally requires a stage's phases to tile its steps exactly,
with no gap or overlap. A table edit that renumbers steps without re-tiling
phases is a build error.

**The declaration is the weak link, and that is unavoidable.** The compiler
cannot see what a function body dereferences, so `StageInputsCoverSteps` proves
only that *no step is listed in a stage that cannot supply what it declared*
— it trusts `required_inputs` to be complete. A step that reads `run.list`
without declaring it passes the guard and still crashes. Declaring inputs
correctly is the part that takes care; the guard is what stops a *correct*
declaration from being invalidated by a table edit.

`StageInputsMet` is the matching runtime check: the runner evaluates the stage's
declaration against the live `PassRun` at stage entry and returns `SkipFrame`
with a named-input error log rather than letting a step null-dereference
part-way through the frame. It is not redundant with the compile-time guard —
the guard checks the *configuration*, this checks that the caller kept its
promise on the frame in question.

**Stages** carry policy as data rather than as inline branches:

| Field | Meaning |
|---|---|
| `is_post_dlss` | records on the game's command list instead of our own |
| `default_failure` | `Fail` / `Transient` / `Ignore` for an unhandled step status |
| `owns_submit` | whether this stage submits and waits |
| `hdr_capable` | a `requires_hdr_aware` step in a non-capable stage is skipped at runtime, and additionally rejected at build time by `NoHdrAwareSteps` |
| `reads_dlss_output` | input comes from the game's DLSS output |
| `required_inputs` | the frame inputs this stage's call site promises. Enforced at stage entry by `StageInputsMet` and at build time by `StageInputsCoverSteps` |

Current stages: **PostDLSS** (transient, not owning submit, HDR-capable,
supplied `CommandList \| OutputResource`), **Present** (fail, owns submit,
supplied `Swapchain \| CommandQueue`), **Release** (ignore, owns submit,
supplied `Swapchain \| CommandQueue`).

**Step ownership in practice.** Most steps live in `pass_session.cpp` as
static members and reach private state through `run.session`. Two other files
contribute steps for concerns that belong to them: the controller contributes
`EnsureHdr` / `WriteToDebugBackbuffer` / `SignalHdrAtPresent` (it owns those
resources), and `hdr_state.cpp` contributes `HdrBeginFrame`. A step can
therefore sit next to the code it drives without the table caring where it
lives.

`FindPipelineStep` resolves that by building an index over the four registries
(`SessionStepFor`, `HdrStepFor`, `DebugStepFor`, `ControllerStepFor`) on first
use, one slot per node. The scans it replaced were never a measurable cost —
about 120 lookups per frame — so speed is not the reason. The reason is that
a linear scan takes the first match and silently ignores a second, so two
registries claiming the same node produced a step that never ran with no
diagnostic anywhere. The index detects that case and logs the collision, naming
which registry won. It also rejects a registry returning a definition whose
`id` does not match the node it was looked up under, which would otherwise
dispatch the wrong function.

**Verifying an edit.** `tools/verify_pipeline_order.py` prints the step tables
beside the ordered operations of the pre-refactor methods, read from git. Use
it before and after any reordering:

```bash
python tools/verify_pipeline_order.py            # auto-detect the oracle
python tools/verify_pipeline_order.py <commit>   # pin a specific revision
```

It needs a revision whose `pass_session.cpp` still contains the old
`OnPresent` / `OnPostDlssEvaluate`. Since the refactor landed, `HEAD` no
longer qualifies, so with no argument the tool walks back from HEAD until it
finds one and reports which it chose. Passing a refactored revision explicitly
is rejected with a clear message rather than printing a meaningless
comparison.

It reports early-exit guards (frames abandoned inside a step) separately from
sequential steps, because those legitimately differ between the two stages.

**Runtime tracing.** `pipeline_trace.cpp` writes a command-stream trace when
`DLSSNR_TRACE=<path>` is set. Resource pointers are printed as stable
first-seen indices, not raw addresses, because COM heaps recycle addresses and
raw pointers would make two runs incomparable. This is the regression oracle
for the refactor: build the pre-refactor tree, capture a trace, and diff.

**Every line names its frame.** A game frame is two streams, not one: the DLSS
evaluate happens mid-frame and the present happens at the end, so one counter
would label the other stream's lines with the wrong frame. `BeginFrame` /
`EndFrame` in `AddonController` therefore keep a counter per `FrameStream`,
printed on every line as `d=` (PostDlss) and `p=` (Present), and emit a `frm`
marker line whose END names the sequence range the frame spans:

```bash
grep ' frm ' trace.log                  # every boundary
grep 'frm.*PostDLSS.*BEGIN' trace.log   # every DLSS-evaluate frame start
```

`d` counts evaluate calls, not frames, so it advances more than once per frame
if the game calls DLSS more than once. `EndFrame` also flushes, so a frame that
completed is on disk even if the process then crashes.

Coverage is deliberately at the choke points rather than in the steps, so a
new step is traced without being instrumented: every `Transition`,
`UavBarrier` and copy goes through `CommandContext`, and every shader dispatch
through `ComputeEngine::Dispatch` — the latter logs the pass name, grid,
SRV/UAV resource ids and the used prefix of the shader params (the trailing
zeros carry no information). Steps add `TRACE_EVT` lines for decisions (a
skip, a failure reason) that produce no GPU command.

Diff two captures with:

```bash
python tools/diff_trace.py --summary before.log after.log   # counts per op
python tools/diff_trace.py before.log after.log             # first divergence
```

Run `--summary` first. It counts operations by type and flags the difference
immediately, which is far easier to read than a line diff. A line diff
desynchronises after the first difference, so only the first one is
meaningful — the tool says so and prints both sides in context.

Bound the capture. Traces grow fast — every transition, barrier and dispatch
is a line — so an unbounded capture from a menu screen is unreadable. Use a
fixed, short, deterministic scene: same save, same camera, same duration for
both runs.


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

**These names are compute shader passes, not pipeline stages.** In
`kComputePasses[]`, `TonemapForward` and `ComposeModelOutputHdr` are the
HDR-aware *alternatives* to `PrepareModelInput` and `ComposeModelOutput`
respectively. `model_pipeline.cpp` picks between them with a plain
`hdr != nullptr` test, so a frame runs one or the other, never both. The
pipeline *stage* that owns them is the PostDLSS stage; steps such as
`PrepareModelInput` and `ComposeModelOutput` in `pass_session.cpp` are the
orchestration around them — a name collision, not the same thing. Some earlier
revisions of this document described these as four separate chained passes;
they are not.

### Resource ownership

Command contexts and compute engines owned by `AddonController`:

| Member | Purpose |
|---|---|
| `debug_command_` | the copy that puts a debug view onto the backbuffer at Present |
| `pass1_command_` | the HDR histogram dispatch (records on the game's list) |
| `codec_` | one shared `ComputeEngine` for every pass **and** the histogram |

There is a single shared `codec_`, not one engine per concern. One engine
means one root signature, one descriptor heap, and one background compile of
`kComputePasses[]` instead of one per session. Earlier revisions had separate
`debug_codec_` and `hdr_codec_` members; those no longer exist.

Each `PassSession` owns all its pass-scoped resources and reaches the shared
engine through the `PassRun`.

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

**Diagnostics on the per-frame path must be transition-based.** A line that is
accurate every frame but informative only when it changes is a stutter source
and a log flood — and worse, a permanent condition repeated every frame buries
its own first occurrence, which is the only one that matters. Two patterns are
in use:

- **Change detection.** `StepEvaluateFeature` compares a cached
  `PassSession::EvalLogKey` (every field the line prints) against the current
  frame and only emits on the first evaluation, on any change, or when a
  resource is null. `Retry()` and `Shutdown()` clear the cache so a retry
  re-arms it.
- **Skip dedup.** The runner's unmet-prerequisite and non-HDR-capable skips log
  once per `(session, stage, node)` via `PipelineRunner::BeginSkipLogFrame`
  (called from `OnPresent`), dropping entries when a skip clears so a
  recurrence logs again.

The rule both follow: **the failure path must force its own emission.** A pass
failing identically every frame is a steady state, which is exactly what a
change check suppresses — so `StepSubmitAndWait` re-emits the cached descriptor
line at error level when the evaluation step didn't already write one that
frame. Without that, the state a persistent bug produces is the one the dedup
silences.

The remaining per-frame risk is the `TRACE_*` macros. They are compiled in but
gate on `trace::Enabled()`, which is a couple of atomic loads and a branch, so
a traced call site costs a branch when `DLSSNR_TRACE` is unset. The macro's
arguments sit *inside* that gate, so nothing they compute is paid while tracing
is off — but every argument is paid every frame while it is on, so keep
formatting out of the call.

Lines are batched rather than flushed individually. A flush per line is a
`write()` syscall per line and dominated the cost of tracing; instead the
writer flushes every `kFlushLineInterval` lines, so call `trace::Flush()` at a
frame boundary to make that frame durable. `NameResource` is a no-op when
tracing is off, because names are only ever read from trace arguments.

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
not in the snapshot, add a field for it and fill it in from the step that
produces it (`PassSession::Step*` in `pass_session.cpp`), not from a
stage-level method — the resource's state is only trustworthy at that point.

### A new pipeline step

1. Add the node to `PipelineNodeId` in `pipeline_graph.h`. Position it after
   everything it depends on: the enum is depth-ordered, and that ordering is
   what makes a cycle unrepresentable.
2. Write the step as a `static PipelineStatus Step*(PassRun& run);` on
   `PassSession` and start the body with `PassSession* self = run.session;`.
   Return `Ok`, `SkipFrame`, or `Fail` — the runner interprets these.
3. Register it in the right step table in `pipeline_graph.cpp`, inside the
   correct phase range, and add a name to `kNodeNames[]`. If the stage's step
   list is one of the mirrored ones, update the copy in `pipeline_graph.h` too —
   `NodeListsEqual` compares them element by element and fails the build
   otherwise.
4. If the step needs a new phase, add it to the phase table. `PhasesCover`
   is a `constexpr` check that the phases tile the step list exactly, so a
   mismatch fails the build rather than misbehaving at runtime.
5. Declare any prerequisite in the step's `depends_on` array. If you also add
   the step to a stage that could run it *before* that prerequisite — or flag it
   `requires_hdr_aware` in a stage that is not `hdr_capable` — the build fails:
   `PrerequisitesPrecedeSteps` and `NoHdrAwareSteps` exist precisely because
   both mistakes shipped once. Register the `static_assert` for your step table
   if it is a new one.
6. Set `required_inputs` to every frame input the body dereferences —
   `run.list`, `run.swapchain`, `run.command_queue`, `run.output_resource`,
   `run.device`, `run.backbuffer`, `run.stage`. Declare only reads: a step that
   *assigns* one of those fields is producing it, and requiring it would be
   wrong. If the step is listed in a stage whose call site does not supply what
   you declared, `StageInputsCoverSteps` fails the build and names the step.
   This is the guard against the null-dereference class of table edit, so it is
   only as good as this declaration — leaving a read undeclared is the one
   mistake it cannot catch.
7. Run `tools/verify_pipeline_order.py` to confirm nothing moved.

If the step belongs to a concern owned by another file, register it there
instead — see the controller and `hdr_state.cpp` registries, and
`FindPipelineStep` for how they are combined. Adding it to *two* registries is
not a silent no-op any more: the index logs which one won.

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
- Output: `build\Release\reshade_dlssnr64.addon64`
- Link: `d3d12.lib;dxgi.lib;detours.lib;d3dcompiler.lib`

`OutDir` is `$(SolutionDir)build\$(Configuration)\`. The target name is
`reshade_dlssnr64`, matching upstream builds; both names may be present in
`build\Release\` if an older fork build is still lying around.

Builds are done by hand in this workspace, not by an agent: the sandbox blocks
`MSBuild.exe` as an unrecognised executable, so MSBuild-based builds are not
something an agent can run here. Do not add scratch build scripts to work
around it.

## Verification

Static checks that pass today:

- 22 translation units compile clean under `/W3 /permissive- /std:c++20`.
- The addon links, with `AddonInit` / `AddonUninit` exported.
- `tools/verify_pipeline_order.py` confirms the step tables reproduce the
  original operation order.

These are all static. The one check that can catch a mistake they cannot is a
runtime trace diff: build the pre-refactor tree, run both with `DLSSNR_TRACE`
set, and compare. Resource pointers are emitted as stable first-seen indices
precisely so the two traces are comparable.
