// Debug views: display pipeline resources by role.
//
// This module is a pure observer — it reads what the pipeline already
// produced, and never recomputes anything. The pipeline populates a
// DebugSnapshot (raw pointers to its live resources) as it runs. If the
// pipeline changes (e.g. a new stage is added), the snapshot reflects it
// automatically because it holds role-to-resource mappings, not copies.
//
// Adding or removing a view = editing one table entry in debug_view.cpp.
#pragma once

#include <d3d12.h>

#include <cstdint>

#include "gpu/command_context.h"
#include "gpu/compute_engine.h"
#include "gpu/pass_resources.h"

namespace reshade_dlssnr {

// Pointers captured during a pass. Only raw pointers are held; the caller
// guarantees the resources stay alive until the pass finishes.
struct DebugSnapshot {
  // Model input before any post-processing (source frame or DLSS output).
  ID3D12Resource* source = nullptr;
  unsigned int source_width = 0;
  unsigned int source_height = 0;

  // Model input after HDR normalization and optional resize — what the
  // model actually sees.
  ID3D12Resource* prepared_input = nullptr;
  unsigned int prepared_input_width = 0;
  unsigned int prepared_input_height = 0;

  // Model output as returned by NGX, before composition.
  ID3D12Resource* model_output = nullptr;
  unsigned int model_output_width = 0;
  unsigned int model_output_height = 0;

  // Model output after composition (residual, strength, HDR remap).
  ID3D12Resource* composed_output = nullptr;
  unsigned int composed_output_width = 0;
  unsigned int composed_output_height = 0;

  // DLSS guides captured from the game.
  ID3D12Resource* motion = nullptr;
  unsigned int motion_width = 0;
  unsigned int motion_height = 0;

  ID3D12Resource* depth = nullptr;
  unsigned int depth_width = 0;
  unsigned int depth_height = 0;

  // Skin detection, when the pass ran this frame. Input and output are at
  // detect scale (working scale of the detection pass); the mask is at the
  // same scale and format R8_UNORM.
  ID3D12Resource* skin_detect_input = nullptr;
  ID3D12Resource* skin_detect_output = nullptr;
  ID3D12Resource* skin_mask = nullptr;
  unsigned int skin_detect_width = 0;
  unsigned int skin_detect_height = 0;
};

// How the display shader should interpret the selected resource.
enum class DebugDisplayMode : uint32_t {
  Color = 0,   // pass through as RGB
  Motion = 1,  // remap [-1,1] xy to [0,1] and show as R=x, G=y
  Depth = 2,   // replicate the red channel
  Mask = 3,    // replicate the red channel (skin mask)
};

struct DebugViewDef {
  const char* name;
  const char* tooltip;
  DebugDisplayMode display_mode;
  // Returns the selected resource and its content size. Returns nullptr if
  // the resource is unavailable this frame. The resource may be larger than
  // the content (e.g. NGX writes into the top-left corner of its output);
  // the display shader accounts for that.
  ID3D12Resource* (*select)(const DebugSnapshot& snapshot,
                            unsigned int* out_content_width,
                            unsigned int* out_content_height);
};

// The debug view table. Terminated by an entry whose `name` is nullptr;
// iterate until then. Adding or removing a view is a single-line edit here.
extern const DebugViewDef kDebugViews[];

// Renders the selected debug view into `target`, which the caller owns and
// must have sized to `target_width × target_height`. `debug_view` follows
// the settings convention: 0 = off, N = kDebugViews[N-1].
// Returns true on success.
bool RenderDebugView(
    CommandContext& state,
    ComputeEngine& codec,
    ID3D12GraphicsCommandList* command_list,
    const DebugSnapshot& snapshot,
    int debug_view,
    ID3D12Resource* target,
    unsigned int target_width,
    unsigned int target_height);

}  // namespace reshade_dlssnr
