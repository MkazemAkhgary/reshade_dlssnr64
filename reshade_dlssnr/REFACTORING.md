# Refactoring Plan

## Status: In Progress

### 1. Remove blob dependency from `shaders/dlssnr_cso.h`
**Goal:** Replace the precompiled CSO blob from OptiScaler with runtime HLSL compilation, like the other codec shaders.
**Benefits:** Removes GPL-3.0 license contamination, enables shader modifications, reduces binary size, improves maintainability.
**Analysis:** The blob is ONLY used for its root signature. The main `Dispatch()` method that uses the blob pipeline state is NEVER called in the current codebase. All actual compute work uses the other shaders (mask, composite, luma_color, debug_residual, downsample, upsample_residual) compiled from HLSL source at runtime. The blob's constants layout also doesn't match OptiScaler's current `dlssnr.hlsl` — it was compiled from an older/simpler version.
**Task:** Extract root signature creation to be independent of the blob, remove blob PSO from `Init()`, delete `shaders/dlssnr_cso.h`.
**Status:** Complete

### 2. Extract descriptor binding into a reusable helper
**Goal:** Create a shared descriptor management class to eliminate the duplicated binding logic across all `codec_*.cpp` files.
**Benefits:** Reduces code duplication by ~6x, simplifies future shader additions.
**Task:** Create `DlssNrDescriptorHelper` or similar that handles SRV/UAV/CBV binding, heap slot management.
**Status:** Not started

### 3. Unify resource state management in `DlssNrCommandContext`
**Goal:** Auto-track resource states and transition them automatically instead of manual `Barrier()` calls everywhere.
**Benefits:** Fewer bugs from missed transitions, cleaner stage code.
**Task:** Add state tracking to `DlssNrCommandContext`, replace manual barriers.
**Status:** Not started

### 4. Consolidate codec files into fewer files with shared infrastructure
**Goal:** Merge the 6 codec files into 1-2 files with a common pipeline state creation helper.
**Benefits:** Easier to understand, less boilerplate.
**Task:** After descriptor helper is done, consolidate.
**Status:** Not started

### 5. Add RAII wrappers for COM pointers
**Goal:** Replace raw `ID3D12Resource*` with `Microsoft::WRL::ComPtr` or custom RAII wrapper.
**Benefits:** Prevents memory leaks on early returns, simplifies cleanup.
**Task:** Replace raw pointers throughout codebase.
**Status:** Not started

### 6. Simplify singleton usage where possible
**Goal:** Reduce global state, make dependencies explicit.
**Benefits:** Easier to test, clearer lifecycle.
**Task:** Review each singleton, inject dependencies where possible.
**Status:** Not started

---

## Notes
- ~~The blob in `shaders/dlssnr_cso.h` is sourced from `OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl` (GPL-3.0)~~ — removed, blob file deleted
- Our `DlssNrConstants` struct is a simplified subset of OptiScaler's full shader interface
- We only use modes 0 (encode), 1 (resolve), 2 (downsample) — the comparison modes (4+) are unused
