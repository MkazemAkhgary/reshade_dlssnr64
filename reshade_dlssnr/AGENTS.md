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
    pipeline_trace.h/.cpp        // DLSSNR_TRACE command-stream trace (debug aid)
    runtime.h/.cpp               // model loading, patching, NGX init, parameter block
    command_context.h/.cpp       // command allocator/list/fence/barrier/state helpers
    crash_trace.h                // first-chance fault log (faulting module + offset)
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
    proxy_unwrap.h               // ReShade + Streamline proxy unwrapping
    streamline_compat.h          // Streamline interposer proxy unwrapping

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

**Verifying an edit.** The step tables are the source of truth for order. The
`static_assert`s above check coverage, prerequisites and declared inputs — not
relative order, which nothing checks for you. When you reorder a table, read it
back as a sequence and satisfy yourself that each operation still sits where it
must run.

The two stages legitimately differ inside their early-exit guards — a frame
abandoned inside the method — and those live inside `EnsureFeature` rather than
as steps, so the two tables are not expected to mirror each other.

**Runtime tracing.** `pipeline_trace.cpp` writes a command-stream trace when
`DLSSNR_TRACE=<path>` is set. It is a debug aid, not a test harness: you reach
for it when something crashes or behaves unexpectedly and the ReShade log does
not explain why, because it shows the commands that were actually recorded and
in what order, rather than what the code intends to do.

Every line is flushed as it is written, so the last line in the file is the
last thing that happened and a crash cannot lose the tail. The cost is a
write() per line, which only applies while tracing is on. Resource pointers
are printed as stable first-seen indices, not raw addresses, so two captures
stay comparable even though COM heaps recycle addresses between runs.

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

Read a capture with:

```bash
awk '$2 == "cmd" || $2 == "evt" {print $3}' trace.log \
  | sort | uniq -c | sort -rn        # how many of each operation
```

Column 3 is the operation name, so that count is the quickest way to see what
a frame actually consisted of. To compare a working run against a broken one,
`diff` the two logs — but only the first divergence is meaningful, since
everything below it is shifted rather than wrong.

**Settings changes are traced too**, as `cfg` lines. The UI writes into
`GetSettings()` in place, so `SaveSettings()` is the one point where a change is
complete; `state/settings.cpp` serializes the settings there and emits the
fields that differ from the previous snapshot:

```bash
grep ' cfg ' trace.log                   # init baseline, then every change
```

Every field once at load (`init <name> <value>`), then `set <name> <old> ->
<new>` per save — one line per frame while a slider is dragged, which is what
makes the frame counters on those lines useful. `resolved[i].` fields are the
inheritance-applied values the runtime reads, so a change to pass 1 shows up on
pass 2 as well. UI-only state (collapsed sections, tooltips) is not traced;
`active_pass` and `debug_view` are, because they change what is rendered.

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

**The dispatch ring must cover every frame the CPU can be ahead.** A dispatch's
descriptors and constants are written by the CPU as the command is recorded and
read by the GPU when it executes, so a slot may only be rewritten once that
dispatch has run. The Post-DLSS passes record onto the game's command list and
nothing on that path waits on the GPU, so the ring depth is the only thing
between a deep CPU lead and a dispatch reading another dispatch's resources.
`kDescriptorRingSize` is derived from DXGI's maximum swapchain depth and the
worst-case dispatches per frame. At 16 slots it covered two passes (6 dispatches
per frame, 8 with the HDR histogram) and not three (9 and 11), which is what
made a third pass flicker.

### NVIDIA Streamline

The game may be running its DLSS through Streamline, which replaces the D3D
entry points the game links against and hands it **proxy** objects. Whether it
does depends on the version:

| Version | Device / command list proxies |
|---|---|
| 1.0.0 - 1.1.1 | Always. Command list state tracking is unconditional in 1.0.0 and on by default in 1.1.x. |
| 2.0.0+ | Only what a feature asks for. The 2.x default flags include `eDisableCLStateTracking`, so a default 2.x host gets no command list proxy. |

**Never operate on a Streamline proxy.** The 2.x guide, section 5.3, says it
plainly: "Third party libraries SHOULD NOT use SL proxies to avoid app
instability." Three concrete ways it breaks us:

1. The proxy's `ResourceBarrier` dispatches Streamline's registered
   after-hooks. Recording our barriers through it runs plugin code from the
   middle of Streamline's own evaluate.
2. The command list proxy's `GetDevice` returns the *proxy device*, so
   anything we allocate goes under Streamline's resource tracking.
3. A proxy we detour puts us inside Streamline's state tracking: its
   `restorePipeline` calls the very methods we patched.

**Unwrap instead.** `sl_compat::Native()` in `hooks/streamline_compat.h`
resolves any proxy to what it wraps, via QueryInterface with the fixed IID
`{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}`. That IID is handled by every
interposer proxy from 1.0.0 through 2.14.1, so one probe covers the whole
range; on anything that is not a proxy it fails harmlessly and the interface
comes back unchanged.

Two consequences worth remembering:

- **The vtable hooks are installed on the native device**, so the tracker is
  keyed by the *native* command list pointer. `RestoreCommandList` must be
  given the same native pointer the hooks saw, or the lookup misses and the
  game's state is never put back.
- **The `command_list` handed to NGX is passed through untouched.** Only our
  own work uses the unwrapped list. Swapping what Streamline passes to its own
  NGX call is not ours to change.

**The NGX capability block is not ours.** `NVSDK_NGX_D3D12_GetCapabilityParameters`
returns one process-global object, and `sl.common` holds the same one
(`ctx.ngxContext.params`) for the life of the process — the same call in v1.1.1
and v2.14.1. Calling `Reset()` on it to clear our own keys clears theirs too.

**Restoring command list state** (2.x guide section 7.0). The host puts back
what it changed after an evaluate. Ours is in `D3D12Hooks::RestoreCommandList`:

- Only the **compute** root signature and root entries. `SetComputeRoot*` and
  `SetGraphicsRoot*` address the same index numbers in two *different*
  signatures, so replaying compute entries after binding a graphics signature
  addresses parameters it does not have and faults in the driver.
- `SetPipelineState1` is on the list too, and lives on `ID3D12GraphicsCommandList4`
  (slot 75), not the base interface.
- Only after a pass actually recorded on the list. Otherwise it replays a record
  the game has moved on from.
- Held as references, not pointers: the game releases these objects whenever it
  likes, and a swapchain resize rebuilds them all.

**The D3D12 debug layer cannot be used with the addon.** With it enabled the
captured vtable slots hold `d3d12SDKLayers.dll` thunks, and driving those
through a hook chain faults inside the layer. An env var used to enable it for
diagnosis and has been removed, because a knob that cannot work is worse than
none.

**The NGX hook must be in place before the first `CreateFeature`.**
`nvngx_dlss.dll` is loaded on demand, and the feature is created once,
immediately after it is loaded. A hook that attaches later never sees that
call, and since the handle-to-feature map is built from it, every evaluate
afterwards is unrecognised and silently ignored — no log, no status, nothing.

`TryHookNgx()` therefore loads the module when nothing else has: it checks
`GetModuleHandle` first, so an instance Streamline has already loaded is never
shadowed by a second copy, and only then falls back to `LoadLibrary`.
`OnPresent` retries while the module is still absent.

The alternative — never loading, only polling from `OnPresent` — was tried and
is up to a frame too late whenever Streamline loads NGX and creates the feature
between two presents.

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

**Two pointers, one object — and the tracker only knows one of them.** This is
the mistake that made `RestoreCommandList` a no-op for its whole life.

The vtable hooks in `d3d12_hooks.cpp` are installed on a command list created
from the **unwrapped** device, so they intercept the unwrapped chain and record
state under unwrapped pointers. The game, however, hands us ReShade's
**wrapper**, and on a 1.x host that wrapper sits around a Streamline proxy.
Every lookup missed, so the game's descriptor heaps, root signature and PSO
stayed clobbered after each of our passes and nothing said so.

`proxy_unwrap::NativeChain()` resolves ReShade first, then Streamline, and the
NGX hook resolves the game's command list once and uses that pointer for
everything — guides, the Post-DLSS passes, and the restore. Both layers answer
the same way (a fixed IID for the object underneath), and the order is forced:
each layer only answers for the object it itself wraps.

If the once-logged `Command list resolved to the interface our D3D12 hooks
recorded` ever stops saying "resolved", the resolution has broken and every
restore is silently a no-op again.

**The tracker holds references, not pointers.** It remembers root signatures,
pipeline states, state objects and descriptor heaps so they can be put back
after our pass, and the game releases all of those whenever it likes — a
swapchain resize tears down and rebuilds the lot. Storing raw pointers meant
the restore could hand the driver one that was already gone, which reads as an
access violation at a small offset inside `nvwgf2umx.dll` and looks like a
driver bug rather than ours. `ReplaceCom` takes a reference on store and drops
the old one; `HookedReset` calls `UntrackCommandList` so the lifetime is one
recording rather than the whole process.

**Nothing here may create objects on a device other than the one the game
uses.** Streamline 1.x proxies the device, queue and command list
unconditionally; 2.x proxies only what a feature needs, and its default flags
disable command list state tracking entirely. So on a 1.x host
`command_queue->GetDevice()` hands back the *proxy* device, and a session that
builds its command list on it gets a proxy list — which then goes to NGX.
`PassSession::AcquireBackbuffer` and the PostDLSS branch of
`StepResolveDevice` resolve through `sl_compat::Native()` before anything is
created from the result.

`DLSSNR pass: Present device ..., command queue ...` is logged once; it says
"native" only in the sense of "not a Streamline proxy", which is a different
question from whether the object is a ReShade wrapper.

**A second DLSSNR feature is fine.** Two passes chained at the same stage, and
a third at the other stage, all create and evaluate their own feature. An
earlier note here claimed the second `CreateFeature` kills the process; that
was wrong — the crash it was describing was the pointer mismatch above.

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

**A crash inside a module we do not own leaves no log.** Everything logged is
written by code that ran *before* the fault, so a fault in `nvngx_dlssnr.dll`
or the NGX core produces a log that says what we were about to do and nothing
about where it died. `runtime/crash_trace.h` closes that gap: a vectored
exception handler that appends one line naming the faulting instruction's
module and offset, plus a short stack, to `%TEMP%\dlssnr_crash.txt`. It is
built on raw Win32 on purpose — no CRT, no `reshade::log`, no allocation —
because it runs while the process is already broken, and it returns
`EXCEPTION_CONTINUE_SEARCH` so the game's own crash handling is unchanged.
Reach for it before theorising about a fault that only reproduces in one game.

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

The CBV is `ComputeConstants`, and a pass that wants to read it declares the
whole thing:

```hlsl
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];   // 16-byte header + 48 floats = the 256-byte ceiling
};
```

Declaring a shorter cbuffer is legal — the shader just reads no parameters
(`kPrepareModelInput` is a pure copy and declares only the header). What you
must not do is declare `gParams` at a size that does not match, because the
offsets are what the region map below is about.

### A new shader parameter

Add one line to `kShaderParams` in `compute/shader_params.h`. It appears
automatically in the UI, persists to settings, and lands in the shader at
`gParams[i]` — written from `constants.params[kShaderParamSlotBase + i]`.

**Read the region map in `compute/compute_engine.h` before you add one.** It
pins which floats belong to whom:

| Region | Floats | Owner |
|---|---|---|
| `kShaderParamSlotBase` | 0 | the `kShaderParams` table |
| `kGradeSlotBase` | 8 | the colour grade block |
| `kResidualSlotBase` | 24 | the residual edit block |
| `kHdrSlotBase` | 32 | the HDR uniforms |

`static_assert`s prove the regions cannot overlap or run off the end of the
48-float array, so a parameter that would collide fails the build instead of
silently overwriting the exposure — which is what used to happen, and produced
a wrong image with nothing in the log.

Two separate limits, and they are not the same number:

- The **table region** is floats `0..8`. There are seven parameters today, so
  one more fits and a ninth trips `kShaderParamCount <= kGradeSlotBase`. The
  message tells you to move `kGradeSlotBase` up, which is the intended fix.
- `kShaderParamCapacity` in `state/settings.h` is the size of the persisted
  `params` / `inherit_params` arrays. It is hand-written on purpose rather than
  derived from the table, so adding a parameter does not resize the saved
  struct.

Passes with a private layout — the histogram pair, `TonemapForward`,
`DebugDisplay` — start their own parameters at 0 and are not part of this map.

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
7. Re-read the stage's step table. The guards check coverage, prerequisites
   and declared inputs, but nothing checks that the new step sits at the right
   point in the sequence.

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
- The pipeline guards — `NodeListsEqual`, `PrerequisitesPrecedeSteps`,
  `NoHdrAwareSteps`, `StageInputsCoverSteps`, `PhasesCover` — hold, and the
  build fails on them rather than the runtime.

These are all static, and none of them checks relative order. When a step
table is reordered, that has to be re-read by hand; see "Verifying an edit"
above.

## Known gaps

**Resource states of game-owned resources are guessed.** `DlssGuides::Capture`
and `PassSession::SeedResourceStates` call `InferResourceState`, which reads a
resource's creation flags. For the game's depth, motion vectors and DLSS output
that is an approximation, and a wrong guess is a barrier whose `Before` does not
match — the debug layer reports it as `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH`.

The correct fix is to record the state from the game's own `ResourceBarrier`
calls. `D3D12Hooks` already has the command list hooked, so it is vtable slot 26
plus a small map of watched resources. Not done because it adds a lookup to
every barrier in the frame, and because no reported failure has been traced to
it — the debug layer's mismatches were mostly on the game's own passes, which
this addon never touches.

## Reported, not investigated

Three symptoms reported by the user, recorded so they are not lost. Two are
still undiagnosed: treat those as the observation only, and the guesses with
them as unverified. The flicker has a diagnosis and a fix that is waiting on a
live run to confirm.

**Flicker with three passes at DLSS Output.** Occurs in both TLOU1 and TLOU2,
and only when three enabled passes all target the DLSS Output stage — any other
configuration shows none. It is transient: about five flickers after the third
pass is enabled, and then it stops.

Diagnosed from a trace of the third pass being enabled. The dispatch ring was
sized for barely two frames: two passes record 6 dispatches per frame (8 with
the HDR histogram) and three record 9 (11 with it), against 16 slots. Fixed by
deriving `kDescriptorRingSize` from the swapchain depth and the worst-case
dispatches per frame (`64fdc60`) — see the ring note under *D3D12 / GPU*. Not
yet confirmed against a live run: if it recurs, the ring is not the whole story
and the next step is to gate slot reuse on the GPU's progress with a fence.

**Debug view frames skipped.** With a debug view selected, occasional frames
show the image with the debug view off. Both games. Intermittent, and only
after the addon has been reconfigured for a while. `RenderDebugView` has several
early returns that silently leave the frame un-debugged — `!codec.IsReady()`,
a null source, `content_w`/`content_h` of zero, or the debug output resource
being recreated by `FrameDebugContext` — and `MaybeRenderDebug` only renders for
the active pass. Any one of those skipping a single frame looks exactly like
this. "After fiddling for a while" points at a rebuild rather than a per-frame
race.

The dispatch-ring fix above is a candidate for this one too: a slot reused
before its dispatch ran hands that dispatch another dispatch's resources, which
renders into the wrong target — and a debug view whose output never lands looks
exactly like a frame with the view off.

**Grading the model output is too extreme in HDR.** The backbuffer grade is
fine; the model-output grade needs a conversion the addon does not currently
apply. `model_pipeline::ApplyGrade` takes an `HdrFormat` and the pipeline
already has `TonemapForward`/`TonemapInverse` around it, so the likely question
is which space the grade levels are applied in and whether the HDR remap runs
before or after them.


