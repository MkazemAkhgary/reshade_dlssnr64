// Captures D3D12 render passes and selected-pass/backbuffer identity for UI.
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace reshade_dlssnr {

struct RenderPassInfo {
  uint32_t index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t draw_count = 0;
  float viewport_width = 0.0f;
  float viewport_height = 0.0f;
  uint64_t target_handle = 0;
  bool is_backbuffer_size = false;
  bool is_backbuffer_resource = false;
};

class RenderPassTracker {
 public:
  static RenderPassTracker& Get();

  void RecordBeginRenderPass(
      reshade::api::command_list* command_list,
      uint32_t count,
      const reshade::api::render_pass_render_target_desc* render_targets);

  void RecordBindRenderTargets(
      reshade::api::command_list* command_list,
      uint32_t count,
      const reshade::api::resource_view* render_targets);

  void RecordViewports(
      reshade::api::command_list* command_list,
      uint32_t first,
      uint32_t count,
      const reshade::api::viewport* viewports);

  void RecordDraw(reshade::api::command_list* command_list);
  void RecordDrawIndexed(reshade::api::command_list* command_list);

  void CaptureBackbuffer(reshade::api::swapchain* swapchain);
  void SwapForNextFrame();

  bool HasBackbufferDescription() const;
  const reshade::api::resource_desc& BackbufferDescription() const;
  uint64_t BackbufferNativeHandle() const;

  bool IsSelectedPassSet() const;
  uint64_t SelectedPassHandle() const;
  void SelectPass(uint64_t handle);
  void DeselectPass();

  bool IsFreezeRenderPassList() const { return freeze_render_pass_list_; }
  void SetFreezeRenderPassList(bool freeze) { freeze_render_pass_list_ = freeze; }

  const std::vector<RenderPassInfo>& DisplayPasses() const;

 private:
  RenderPassInfo CreatePassInfo(
      reshade::api::command_list* command_list,
      uint32_t count,
      const reshade::api::render_pass_render_target_desc* render_targets);

  RenderPassInfo CreatePassInfo(
      reshade::api::command_list* command_list,
      uint32_t count,
      const reshade::api::resource_view* render_targets);

  mutable std::mutex mutex_;

  std::vector<RenderPassInfo> render_passes_;
  std::vector<RenderPassInfo> display_passes_;
  std::unordered_map<reshade::api::command_list*, size_t> current_pass_for_command_list_;
  uint32_t current_pass_index_ = 0;

  reshade::api::resource_desc backbuffer_desc_ = {};
  bool has_backbuffer_desc_ = false;
  uint64_t backbuffer_native_handle_ = 0;
  bool has_backbuffer_native_handle_ = false;

  uint64_t selected_pass_target_handle_ = 0;
  bool has_selected_pass_target_ = false;

  bool freeze_render_pass_list_ = false;
};

}  // namespace reshade_dlssnr
