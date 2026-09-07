#include "render_pass_tracker.h"

#include <d3d12.h>

#include "dlssnr_direct.h"

namespace reshade_dlssnr {

RenderPassTracker& RenderPassTracker::Get() {
  static RenderPassTracker tracker;
  return tracker;
}

void RenderPassTracker::RecordBeginRenderPass(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::render_pass_render_target_desc* render_targets) {
  if (command_list == nullptr || count == 0 || render_targets == nullptr) return;

  reshade::api::device* const device = command_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return;

  const reshade::api::resource target_resource =
      device->get_resource_from_view(render_targets[0].view);

  if (target_resource == 0) return;

  RenderPassInfo info = CreatePassInfo(command_list, count, render_targets);

  if (has_backbuffer_desc_) {
    info.is_backbuffer_size =
        info.width == backbuffer_desc_.texture.width &&
        info.height == backbuffer_desc_.texture.height;
  }

  info.is_backbuffer_resource =
      has_backbuffer_native_handle_ &&
      info.target_handle == backbuffer_native_handle_;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t pass_index = render_passes_.size();
    render_passes_.push_back(info);
    current_pass_for_command_list_[command_list] = pass_index;
  }
}

void RenderPassTracker::RecordBindRenderTargets(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::resource_view* render_targets) {
  if (command_list == nullptr || count == 0 || render_targets == nullptr) return;

  reshade::api::device* const device = command_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return;

  const reshade::api::resource target_resource =
      device->get_resource_from_view(render_targets[0]);

  if (target_resource == 0) return;

  RenderPassInfo info = CreatePassInfo(command_list, count, render_targets);

  if (has_backbuffer_desc_) {
    info.is_backbuffer_size =
        info.width == backbuffer_desc_.texture.width &&
        info.height == backbuffer_desc_.texture.height;
  }

  info.is_backbuffer_resource =
      has_backbuffer_native_handle_ &&
      info.target_handle == backbuffer_native_handle_;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t pass_index = render_passes_.size();
    render_passes_.push_back(info);
    current_pass_for_command_list_[command_list] = pass_index;
  }

  uint64_t selected_handle = 0;
  bool notify_selected = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_selected =
        has_selected_pass_target_ &&
        info.target_handle == selected_pass_target_handle_;
    selected_handle = info.target_handle;
  }

  if (notify_selected) {
    auto* native_target = reinterpret_cast<ID3D12Resource*>(selected_handle);
    if (native_target != nullptr) {
      native_target->AddRef();
      DlssNrDirect::Get().SetPendingSelectedPass(native_target);
    }
  }
}

void RenderPassTracker::RecordViewports(
    reshade::api::command_list* command_list,
    uint32_t first,
    uint32_t count,
    const reshade::api::viewport* viewports) {
  if (command_list == nullptr || count == 0 || viewports == nullptr) return;

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = current_pass_for_command_list_.find(command_list);
  if (it == current_pass_for_command_list_.end()) return;
  if (it->second >= render_passes_.size()) return;

  RenderPassInfo& pass = render_passes_[it->second];
  pass.viewport_width = viewports[first].width;
  pass.viewport_height = viewports[first].height;
}

void RenderPassTracker::RecordDraw(reshade::api::command_list* command_list) {
  if (command_list == nullptr) return;

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = current_pass_for_command_list_.find(command_list);
  if (it == current_pass_for_command_list_.end()) return;
  if (it->second >= render_passes_.size()) return;

  render_passes_[it->second].draw_count++;
}

void RenderPassTracker::RecordDrawIndexed(reshade::api::command_list* command_list) {
  if (command_list == nullptr) return;

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = current_pass_for_command_list_.find(command_list);
  if (it == current_pass_for_command_list_.end()) return;
  if (it->second >= render_passes_.size()) return;

  render_passes_[it->second].draw_count++;
}

void RenderPassTracker::CaptureBackbuffer(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr) return;

  reshade::api::device* const device = swapchain->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return;

  const auto backbuffer_handle = swapchain->get_current_back_buffer();
  if (backbuffer_handle == 0) return;

  std::lock_guard<std::mutex> lock(mutex_);

  backbuffer_desc_ = device->get_resource_desc(backbuffer_handle);
  has_backbuffer_desc_ = true;
  backbuffer_native_handle_ = backbuffer_handle.handle;
  has_backbuffer_native_handle_ = true;
}

void RenderPassTracker::SwapForNextFrame() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (freeze_render_pass_list_) return;

  display_passes_.swap(render_passes_);
  render_passes_.clear();
  current_pass_for_command_list_.clear();
  current_pass_index_ = 0;
}

bool RenderPassTracker::HasBackbufferDescription() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_backbuffer_desc_;
}

const reshade::api::resource_desc& RenderPassTracker::BackbufferDescription() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return backbuffer_desc_;
}

uint64_t RenderPassTracker::BackbufferNativeHandle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return backbuffer_native_handle_;
}

bool RenderPassTracker::IsSelectedPassSet() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_selected_pass_target_;
}

uint64_t RenderPassTracker::SelectedPassHandle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selected_pass_target_handle_;
}

void RenderPassTracker::SelectPass(uint64_t handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  selected_pass_target_handle_ = handle;
  has_selected_pass_target_ = handle != 0;
}

void RenderPassTracker::DeselectPass() {
  std::lock_guard<std::mutex> lock(mutex_);
  selected_pass_target_handle_ = 0;
  has_selected_pass_target_ = false;
}

const std::vector<RenderPassInfo>& RenderPassTracker::DisplayPasses() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return display_passes_;
}

RenderPassInfo RenderPassTracker::CreatePassInfo(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::render_pass_render_target_desc* render_targets) {
  reshade::api::device* const device = command_list->get_device();
  const reshade::api::resource target_resource =
      device->get_resource_from_view(render_targets[0].view);
  const reshade::api::resource_desc desc = device->get_resource_desc(target_resource);

  RenderPassInfo info;
  info.index = current_pass_index_++;
  info.width = desc.texture.width;
  info.height = desc.texture.height;
  info.format = static_cast<uint32_t>(desc.texture.format);
  info.target_handle = target_resource.handle;
  return info;
}

RenderPassInfo RenderPassTracker::CreatePassInfo(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::resource_view* render_targets) {
  reshade::api::device* const device = command_list->get_device();
  const reshade::api::resource target_resource =
      device->get_resource_from_view(render_targets[0]);
  const reshade::api::resource_desc desc = device->get_resource_desc(target_resource);

  RenderPassInfo info;
  info.index = current_pass_index_++;
  info.width = desc.texture.width;
  info.height = desc.texture.height;
  info.format = static_cast<uint32_t>(desc.texture.format);
  info.target_handle = target_resource.handle;
  return info;
}

}  // namespace reshade_dlssnr
