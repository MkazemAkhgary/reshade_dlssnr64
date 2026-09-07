# DLSSNR Refactor TODO

Refactor the addon into smaller domain files while keeping builds and runtime behavior working after every step.

- [x] Create `render_pass_tracker.h/.cpp` and move render-pass collection, display-pass swap, backbuffer identity, selected-pass state, and deselection out of `addon.cpp`; keep `addon.cpp` callbacks as thin adapters. Verify build and overlay pass list/selection still work.
- [x] Create `ui.h/.cpp` and move all ImGui drawing, tabs, pass settings controls, reset buttons, and render-pass debug UI out of `addon.cpp`; expose a single `DrawOverlay` entry point. Verify build and overlay interaction still works. Verified.
- [x] Extract `ngx_parameter_block.h/.cpp` from `dlssnr_direct.cpp` so NGX capability parameter handling is isolated. Verify build and model tuning still works. Verified.
- [ ] Split codec shader implementations out of `codec.cpp` into `codec_mask.cpp`, `codec_ui_composite.cpp`, `codec_luma_color.cpp`, and `codec_working_scale.cpp`, leaving core codec/root-signature/descriptor setup in `codec.cpp`. Verify build and all shaders still function.
- [ ] Create `dlssnr_runtime.h/.cpp` for shared model loading, IAT patching, `Init_Ext`, function pointers, and parameter block ownership. Verify build and direct model execution still works.
- [ ] Create `dlssnr_pass_session.h/.cpp` containing one pass's feature handle, resources, working scale state, and evaluation logic. Verify build and pass 1 behavior still works.
- [ ] Refactor `dlssnr_direct` into a controller/facade owning one `DlssNrRuntime` and two `DlssNrPassSession` instances; keep selected-pass/UI options only on pass 1. Verify build and both pass behaviors still work.
- [ ] Update `reshade_dlssnr.vcxproj` with all new source/header files and remove old monolithic references if any. Verify full build succeeds.
- [ ] Run full manual smoke test: overlay, model selection, tuning, selected pass input, UI mask, working scale, second pass, and shutdown.
- [ ] Remove this TODO file after all tasks are complete.
