# HDR Pipeline — Design

## Purpose

Make the model work on a bright, display-referred SDR image regardless of
the source's format (SDR, linear scRGB, HDR10 PQ). Preserve HDR detail on
the round trip: with the model as a no-op, output == input.

Auto-expose dark scenes and compress bright ones. Drive exposure from a
GPU histogram with temporal smoothing. Expose all knobs as sliders.

## Scope

**In scope:** the Post-DLSS path only. The Present path already runs on a
display-referred SDR backbuffer and is unchanged.

**Starting point:** one curve type (Extended Reinhard) with an invertible
closed form. More curves can be added later without changing the pipeline.

## Module layout

### New: `runtime/hdr_state.{h,cpp}`

Owns everything HDR-specific. One instance lives on `AddonController`.

```cpp
enum class HdrFormat : int {
  Auto  = 0,   // detect from DXGI format
  Sdr   = 1,   // sRGB-encoded, 0..1
  ScRgb = 2,   // linear, 1.0 == 80 nits
  Hdr10 = 3,   // PQ (ST.2084), 0..1 represents 0..10000 nits
};

enum class ChromaEditMode : int {
  Additive = 0,   // YCbCr delta
  Ratio    = 1,   // per-channel chroma ratio
};

class HdrState {
 public:
  bool Ensure(ID3D12Device* device);
  void Reset();

  // Called once per frame by AddonController before the pass loop.
  void BeginFrame(CommandContext& cmd,
                  ID3D12GraphicsCommandList* list,
                  ID3D12Resource* source,
                  unsigned int width,
                  unsigned int height,
                  const Settings& settings);

  bool Enabled() const;
  HdrFormat ResolvedFormat() const;
  float Exposure() const;
  float WhitePoint() const;

 private:
  void UpdateExposure(const uint32_t* bins, uint32_t total,
                      const Settings& cfg);

  HdrFormat resolved_format_ = HdrFormat::Sdr;
  float exposure_ema_ = 1.0f;
  float white_point_ema_ = 1.0f;
  bool  have_measurement_ = false;
  uint32_t frame_counter_ = 0;
  uint32_t warmup_frames_ = 0;

  GpuReadback readback_;
  Microsoft::WRL::ComPtr<ID3D12Resource> histogram_;
};
```

### New compute passes (`compute_passes.cpp`)

- `ZeroHistogram` — clears the 256-bin buffer
- `BuildHistogram` — 256-bin log2-luminance histogram with atomics
- `TonemapForward` — HDR source → SDR intermediate
- `ComposeModelOutputHdr` — HDR-aware composition (replaces the SDR path
  when HDR is enabled)

### Extended

- `PassResources` — one 256 × u32 histogram buffer (UAV). `kScratchCount`
  grows from 4 to 6 with two slots reserved for HDR SDR intermediates.
- `PassSession::OnPostDlssEvaluate` — branches between SDR (existing) and
  HDR (new) paths.

## Settings

Config lives in `Settings` (shared across passes):

```cpp
struct Settings {
  bool     hdr_enable = false;
  HdrFormat hdr_format = HdrFormat::Auto;
  ChromaEditMode hdr_chroma_mode = ChromaEditMode::Ratio;
  float    hdr_responsiveness = 0.50f;   // [0,1] — see below
  float    hdr_midtone = 0.18f;          // scene key target
  float    hdr_white_percentile = 0.99f;
  float    hdr_highlight_headroom = 8.0f;
  float    hdr_edit_strength = 1.0f;
};
```

### Responsiveness slider semantics

Single slider `hdr_responsiveness ∈ [0, 1]`. Low value means slow response
and low average cost; high value means fast response and higher average
cost. Both the measurement frequency and the EMA rate derive from it:

| Slider | Measure every N frames | EMA α (per measurement) | Avg. cost / frame |
|--------|------------------------|--------------------------|--------------------|
| 0.00   | 32                     | 0.02                     | 1/32 of a histogram |
| 0.25   | 24                     | 0.12                     | 1/24 |
| 0.50   | 16                     | 0.22                     | 1/16 |
| 0.75   | 8                      | 0.32                     | 1/8 |
| 1.00   | 1                      | 0.40                     | full |

Formula:
```
measure_every = round(32 - 31 * responsiveness)   // 32 → 1
ema_alpha     = 0.02 + responsiveness * 0.38
```

The two effects compound: at low responsiveness both the update rate and
the blend rate are slow; at high responsiveness both are fast.

The first 30 frames after HDR is enabled use an accelerated warm-up
(`ema_alpha = 0.5`) to converge quickly.

## Per-frame flow (Post-DLSS, HDR enabled)

```
AddonController::OnPostDlssEvaluate
  │
  ├─ if (hdr.enabled && first pass in chain)
  │    hdr.BeginFrame(cmd, list, dlss_output, w, h, settings)
  │      ├─ if (frame_counter % measure_every == 0)
  │      │    issue ZeroHistogram
  │      │    issue BuildHistogram(source → histogram)
  │      │    enqueue readback copy (histogram → readback ring slot)
  │      ├─ TryGet previous histogram
  │      │    └─ if data: UpdateExposure() → EMA update
  │      └─ frame_counter++
  │
  └─ for each enabled Post-DLSS pass:
       PassSession::OnPostDlssEvaluate(..., hdr)
         ├─ if (!hdr.enabled) → existing SDR path
         └─ else →
              TonemapForward(source_hdr → sdr_intermediate)
              EvaluateFeature(sdr_intermediate → sdr_model_output)
              ComposeModelOutputHdr(...)
              → write to DLSS output resource
```

Only the first Post-DLSS pass in the chain runs `BeginFrame`; subsequent
passes reuse the same exposure / white point.

## Shader specs

### `ZeroHistogram`

```
dispatch: 1 × 1 × 1  (256 threads)
output:   gHistogram : RWBuffer<uint>  (u0)

if (tid.x < 256) gHistogram[tid.x] = 0;
```

### `BuildHistogram`

```
dispatch: (W + 7) / 8 × (H + 7) / 8
inputs:   gSource    : Texture2D<float4>  (t0)
outputs:  gHistogram : RWBuffer<uint>     (u0)

params:
  gWidth, gHeight
  gLumaMinLog2 = -12.0
  gLumaMaxLog2 = +12.0
  gFormat      (0 SDR sRGB, 1 scRGB, 2 HDR10 PQ)

algorithm:
  - shared-memory 256-bin histogram per workgroup
  - each thread samples every 2nd pixel (1/4 total)
  - decode source to linear luminance (PQ decode if gFormat == 2)
  - bin = (log2(luma) - gLumaMinLog2) / (gLumaMaxLog2 - gLumaMinLog2) * 255
  - merge shared into global with InterlockedAdd
```

### `TonemapForward`

```
dispatch: (W + 7) / 8 × (H + 7) / 8
inputs:   gSource : Texture2D<float4>   (t0)
outputs:  gSdr    : RWTexture2D<float4> (u0)

params:
  gWidth, gHeight
  gExposure
  gWhitePoint
  gFormat (same as above)

algorithm:
  c = decode(source, gFormat)          // → linear
  x = c * gExposure
  s = ReinhardForward(x, gWhitePoint)  // → linear SDR
  s = LinearToSrgb(s)                  // → display-referred
  write(s)
```

### `ComposeModelOutputHdr`

```
dispatch: (W + 7) / 8 × (H + 7) / 8
inputs:
  t0: gOriginal : full-res HDR source
  t1: gSdrIn    : SDR intermediate (what the model saw)
  t2: gSdrOut   : SDR model output
outputs:
  u0: gResult   : full-res HDR result

params:
  gWidth, gHeight
  gExposure
  gWhitePoint
  gEditStrength
  gLumaStrength
  gChromaStrength
  gFormat
  gChromaMode  (0 additive, 1 ratio)

algorithm:
  hdrOrig = original[tid]
  sdrIn   = sample gSdrIn  (bilinear)
  sdrOut  = sample gSdrOut (bilinear)

  // Luma edit in SDR
  lumaIn    = dot(sdrIn,  kLuma709)
  lumaOut   = dot(sdrOut, kLuma709)
  lumaDelta = (lumaOut - lumaIn) * gEditStrength
  lumaFinal = saturate(lumaIn + lumaDelta)

  // Chroma edit — one of two modes
  chromaIn  = RgbToYcbcr(sdrIn)
  chromaOut = RgbToYcbcr(sdrOut)

  if (gChromaMode == 0) {
    // Additive: preserve hue by adding the delta
    chromaFinal.yz = chromaIn.yz +
                     (chromaOut.yz - chromaIn.yz) * gEditStrength;
  } else {
    // Ratio: match the model's chroma direction, keep the input's
    // magnitude shape
    chromaFinal.yz = chromaIn.yz *
                     (chromaOut.yz / max(abs(chromaIn.yz), 1e-4)) ;
    chromaFinal.yz = lerp(chromaIn.yz, chromaFinal.yz, gEditStrength);
  }

  sdrEdited = YcbcrToRgb(lumaFinal, chromaFinal)

  // Inverse tonemap → HDR that would have produced this SDR pixel
  hdrEdited = ReinhardInverse(SrgbToLinear(sdrEdited), gWhitePoint)
              / gExposure

  // Blend with original using luma/chroma strengths (existing pattern)
  finalLuma = lerp(dot(hdrOrig, kLuma709),
                   dot(hdrEdited, kLuma709), gLumaStrength)
  ... apply chroma blend in YCbCr space ...
  write(encode(finalResult, gFormat))
```

Round-trip property: if the model is a no-op (`sdrOut == sdrIn`), then
`hdrEdited == hdrOrig` exactly (from the exact inverse), and the blend
reduces to `hdrOrig`. No drift.

## Curve math (Extended Reinhard)

```
forward:  s = x · (1 + x / W²) / (1 + x)
inverse:  x = (-(1 - s) · W² + sqrt((1 - s)² · W⁴ + 4 · s · W²)) / 2
```

`W = 1` gives classic Reinhard (`x / (1 + x)`). Larger W preserves more of
the range linearly. `W → ∞` makes both directions identity.

## Format detection

Auto-detect from the DLSS output's DXGI format:

| DXGI format                  | Detected   |
|------------------------------|------------|
| R16G16B16A16_FLOAT           | scRGB      |
| R11G11B10_FLOAT              | scRGB      |
| R10G10B10A2_UNORM            | HDR10      |
| R8G8B8A8_UNORM / SRGB        | SDR        |
| B8G8R8A8_UNORM / SRGB        | SDR        |
| other                        | SDR        |

Float formats are unambiguously linear HDR. `R10G10B10A2_UNORM` is
detected as HDR10 — historically we treated it as ambiguous, but true
10-bit SDR is essentially unused in games; when this format appears as a
DLSS output it is HDR10 in practice. Users with a genuine 10-bit SDR setup
can override to SDR.

The override combo in the UI is always visible so the user can force a
format when auto-detection guesses wrong. The raw DXGI format value is
logged once per change, so any remaining detection issues are easy to
diagnose.

## Exposure computation (CPU)

Runs when a readback arrives:

```cpp
void UpdateExposure(const uint32_t* bins, uint32_t total,
                    const Settings& cfg) {
  const float p_mid = Percentile(bins, total, 0.50f);
  const float p_hi  = Percentile(bins, total, cfg.hdr_white_percentile);

  const float luma_mid = Log2BinToLinear(p_mid);
  const float luma_hi  = Log2BinToLinear(p_hi);

  const float target = (luma_mid > 1e-6f)
                           ? cfg.hdr_midtone / luma_mid
                           : exposure_ema_;

  const float alpha = warmup_frames_ < 30
                          ? 0.5f
                          : (0.02f + cfg.hdr_responsiveness * 0.38f);

  exposure_ema_ = Lerp(exposure_ema_, target, alpha);

  const float W_target = std::max(1.0f, luma_hi * exposure_ema_);
  white_point_ema_ = Lerp(white_point_ema_, W_target, alpha);

  have_measurement_ = true;
  warmup_frames_++;
}
```

Percentile lookup: 256-entry cumulative scan, trivial on CPU.

## UI

Shared section above the pass tabs (HDR is not per-pass):

```
▼ HDR
   ☐ Enable HDR Handling
       Normalize HDR input for the model. Only affects passes running
       at DLSS Output.

   Format             [Auto ▾]      // Auto / SDR / scRGB / HDR10
   Chroma Edit        [Ratio ▾]     // Additive / Ratio
   Responsiveness     [====●====]  R
   Midtone Target     [====●====]  R
   White Percentile   [====●====]  R
   Highlight Headroom [====●====]  R
   Edit Strength      [====●====]  R

   ▼ Diagnostics
       Samples, scene median, white point source, target vs current
       exposure, and a plot of the last measured histogram.
```

All controls below the checkbox are disabled (greyed) when HDR is off.
The Format and Chroma Edit combos are always enabled.

Defaults:

| Control           | Default | Range          | Notes                            |
|-------------------|---------|----------------|----------------------------------|
| Enable            | off     |                |                                  |
| Format            | Auto    |                |                                  |
| Chroma Edit       | Ratio   |                |                                  |
| Responsiveness    | 0.50    | 0.00 – 1.00    | Slow + cheap ↔ fast + costly     |
| Midtone Target    | 0.18    | 0.05 – 0.50    | 18% grey                         |
| White Percentile  | 0.99    | 0.90 – 0.999   |                                  |
| Highlight Headroom| 8.00    | 0.25 – 16.00   | Multiplier on detected white point |
| Edit Strength     | 1.00    | 0.00 – 2.00    | Scales model edit before remap   |

## `PassResources` additions

```cpp
constexpr uint32_t kScratchCount = 6;
constexpr uint32_t kInputSnapshotSlot = 0;  // Post-DLSS multi-pass
constexpr uint32_t kDebugScratchSlot  = 1;  // frame debug output
constexpr uint32_t kSdrWorkingSlot    = 2;  // HDR SDR intermediate
constexpr uint32_t kSdrFullSlot       = 3;  // reserved

// Dedicated histogram buffer (256 × u32, UAV)
bool EnsureHistogram(ID3D12Device* device);
ID3D12Resource* GetHistogram() const;
```

## Edge cases and known limitations

1. **Readback latency.** 3 frames of ring buffer. With the Responsiveness
   slider at low values, changes to scene brightness take noticeable time
   to converge. Intended.

2. **First-frame convergence.** Before the first histogram arrives,
   exposure is 1.0. The 30-frame warm-up (alpha = 0.5) converges in ~0.5s
   at 60Hz.

3. **`R10G10B10A2` ambiguity.** Defaults to SDR. Users with true HDR10
   content should select the override.

4. **Working scale != 1.** The SDR intermediate is created at
   working-scale resolution. The composition samples it back up to full
   resolution with bilinear, matching the existing pipeline pattern.

5. **Backbuffer mode ignores HDR.** The backbuffer is already
   display-referred. Tooltip documents this.

6. **Multiple Post-DLSS passes.** Only the first runs `BeginFrame`. All
   passes share the same exposure. If pass 2 modifies the DLSS output
   significantly, pass 3 reads the modified image as its `hdrOrig` while
   using the exposure measured on the original. Acceptable for now.

7. **Chroma edit modes.**
   - `Additive`: the model's edit is expressed as an RGB delta in
     SDR sRGB space, applied to the reconstructed full-res SDR, then
     inverse-tonemapped back to HDR. Predictable; the edit's magnitude is
     bounded by the SDR delta. Safe everywhere.
   - `Ratio` (default): the model's edit is expressed as a per-channel multiplicative
     ratio in linear SDR, applied directly to the HDR linear. Preserves
     relative brightness changes exactly, at the cost of amplifying noise
     where the SDR input is near zero.

   Both modes flow through the same soft compression (when enabled) and the
   same luma/chroma strength blend. Ship both, default to Ratio, iterate
   with real content.

## Implementation phases

**Phase A — Histogram + exposure measurement**
- `ZeroHistogram` + `BuildHistogram` shaders
- `HdrState` module (detection, readback, EMA)
- Settings fields + UI section
- Temporary debug logging of exposure / white point to verify

**Phase B — Forward tonemap**
- `TonemapForward` shader
- `PassSession` HDR branch: measure → tonemap → feed SDR to model
- Model still uses the existing SDR → SDR composition (incorrect HDR
  colors, but visible progress on dark-scene brightness)

**Phase C — Inverse tonemap + exact-inverse composition**
- `ComposeModelOutputHdr` shader
- Full round trip. Verify no-op model produces bit-identical HDR output.

**Phase D — Chroma edit tuning**
- Iterate on both modes with real content

**Phase E — Polish**
- Defaults tuned across a few games
- Optional debug views for histogram / exposure
