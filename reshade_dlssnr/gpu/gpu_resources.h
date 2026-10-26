// The one place addon GPU resources are created, replaced, and released.
//
// Two rules hold everywhere in the addon:
//   1. Allocations go through CreateTexture2D()/CreateBuffer() (or the
//      CreateUav* wrappers) - nothing else calls CreateCommittedResource.
//   2. A resource that is replaced while the GPU may still be reading it is
//      handed to the retire queue, never released inline. The queue binds
//      each entry to a queue-ordered fence and frees the completed prefix.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace reshade_dlssnr {

// Every addon texture is a single-mip, single-slice Texture2D. This is the
// descriptor builder for that shape; callers override Format and Flags.
D3D12_RESOURCE_DESC Texture2DDesc(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes);

// -- Creation ---------------------------------------------------------------

// Creates the texture `desc` describes. `desc` is used verbatim, including its
// flags, so a caller that needs the UAV bit must set it.
bool CreateTexture2D(ID3D12Device* device,
                     const D3D12_RESOURCE_DESC& desc,
                     D3D12_RESOURCE_STATES initial_state,
                     ID3D12Resource** out_resource);

bool CreateBuffer(ID3D12Device* device,
                  uint64_t bytes,
                  D3D12_HEAP_TYPE heap_type,
                  D3D12_RESOURCE_STATES initial_state,
                  D3D12_RESOURCE_FLAGS flags,
                  ID3D12Resource** out_resource);

// Default-heap UAV allocations, which is what almost every addon resource is.
// The UAV flag is added to the descriptor; the resource starts in
// UNORDERED_ACCESS.
bool CreateUavTexture2D(ID3D12Device* device,
                        const D3D12_RESOURCE_DESC& desc,
                        ID3D12Resource** out_resource);
bool CreateUavBuffer(ID3D12Device* device,
                     uint64_t bytes,
                     ID3D12Resource** out_resource);

// -- Deferred release -------------------------------------------------------
//
// One queue per process, gated by the release command context's fence.
//
// The type is public so the surface reads as a service, the way
// AddonController::Get() and DlssGuides::Get() do, and so the device the fence
// belongs to can live next to the entries it gates.
//
// Retire() is still private, and the three RAII types below are its only
// friends. There is therefore no way to retire a bare ID3D12Resource* from
// outside this module: the argument type has to be one that owns a reference.
// Retiring something you hold no reference to is not expressible.
class ResourceRetireQueue final {
 public:
  static ResourceRetireQueue& Get();

  // Bind every untagged entry to `fence_value`. Zero is ignored.
  void TagAll(uint64_t fence_value);

  // Frees entries tagged at or below `completed_fence_value`. Returns how many
  // were released.
  size_t ReleaseCompleted(uint64_t completed_fence_value);

  // Unconditional release, with no fence check. Only legal once the GPU is
  // known to be idle and will never signal again - shutdown and restart, both
  // of which wait for a final queue marker. The name states the precondition
  // because it is the only thing standing between this call and a
  // use-after-free.
  void ReleaseAllAfterGpuIdle();

  // True when an entry is waiting for a fence that has not been signaled yet.
  // A marker is only useful for those: an entry already bound to a fence frees
  // when that fence completes, and signaling another one would not help.
  bool HasUntagged() const;

  // How many entries are waiting for their fence. Shown in the status panel.
  size_t Count() const;

 private:
  ResourceRetireQueue() = default;

  struct Entry {
    ID3D12Resource* resource = nullptr;
    uint64_t fence_value = 0;
  };

  // Takes ownership of `resource`.
  void Retire(ID3D12Resource* resource, uint64_t fence_value = 0);

  friend class OwnedTexture;
  friend class OwnedBuffer;
  friend class BorrowedRef;

  mutable std::mutex lock_;
  std::vector<Entry> entries_;
  uint64_t last_completed_fence_ = 0;
  bool depth_warned_ = false;
};

// -- Owned resources ---------------------------------------------------------
//
// These own the resource. Never hand them a game resource: they retire what
// they replace, and a game resource must not be released by the addon.
// Borrowed game resources are held by BorrowedRef, which owns exactly one
// counted reference and cannot be built without AddRef'ing.
//
// There is deliberately no Attach()/Adopt(): an adoption path would be the one
// way to smuggle a borrowed resource into the owned list.

// One owned texture plus its shape. Replacing it retires the previous texture
// instead of releasing it, which is the only safe order: the GPU may still be
// reading it from a command list submitted in an earlier frame.
class OwnedTexture final {
 public:
  OwnedTexture() = default;
  ~OwnedTexture() { Reset(); }
  OwnedTexture(const OwnedTexture&) = delete;
  OwnedTexture& operator=(const OwnedTexture&) = delete;
  OwnedTexture(OwnedTexture&& other) noexcept { MoveFrom(other); }
  OwnedTexture& operator=(OwnedTexture&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  // Creates the texture if the slot is empty or its shape differs. `desc` is
  // used verbatim, including its flags.
  bool Ensure(ID3D12Device* device,
              const D3D12_RESOURCE_DESC& desc,
              D3D12_RESOURCE_STATES initial_state,
              const char* name);

  // Same, adding ALLOW_UNORDERED_ACCESS and starting in UNORDERED_ACCESS.
  bool EnsureUav(ID3D12Device* device,
                 const D3D12_RESOURCE_DESC& desc,
                 const char* name);

  // Retires the current texture and clears the shape.
  void Reset();

  ID3D12Resource* Get() const { return resource_.Get(); }
  explicit operator bool() const { return resource_ != nullptr; }
  bool operator==(std::nullptr_t) const { return resource_ == nullptr; }
  bool operator!=(std::nullptr_t) const { return resource_ != nullptr; }

  bool Matches(uint64_t width, uint64_t height, DXGI_FORMAT format) const {
    return resource_ != nullptr && width_ == width && height_ == height &&
           format_ == format;
  }

  uint32_t Width() const { return width_; }
  uint32_t Height() const { return height_; }
  DXGI_FORMAT Format() const { return format_; }
  const char* Name() const { return name_; }

 private:
  void MoveFrom(OwnedTexture& other) noexcept {
    resource_ = std::move(other.resource_);
    width_ = other.width_;
    height_ = other.height_;
    format_ = other.format_;
    name_ = other.name_;
    other.width_ = 0;
    other.height_ = 0;
    other.format_ = DXGI_FORMAT_UNKNOWN;
    other.name_ = "";
  }

  Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  const char* name_ = "";
};

// One owned buffer plus its byte size. Same replacement rule as OwnedTexture.
class OwnedBuffer final {
 public:
  OwnedBuffer() = default;
  ~OwnedBuffer() { Reset(); }
  OwnedBuffer(const OwnedBuffer&) = delete;
  OwnedBuffer& operator=(const OwnedBuffer&) = delete;
  OwnedBuffer(OwnedBuffer&& other) noexcept { MoveFrom(other); }
  OwnedBuffer& operator=(OwnedBuffer&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  bool Ensure(ID3D12Device* device,
              uint64_t bytes,
              D3D12_HEAP_TYPE heap_type,
              D3D12_RESOURCE_STATES initial_state,
              D3D12_RESOURCE_FLAGS flags,
              const char* name);

  // Same, as a default-heap UAV buffer starting in UNORDERED_ACCESS.
  bool EnsureUav(ID3D12Device* device, uint64_t bytes, const char* name);

  void Reset();

  ID3D12Resource* Get() const { return resource_.Get(); }
  explicit operator bool() const { return resource_ != nullptr; }
  bool operator==(std::nullptr_t) const { return resource_ == nullptr; }
  bool operator!=(std::nullptr_t) const { return resource_ != nullptr; }

  bool Matches(uint64_t bytes) const {
    return resource_ != nullptr && bytes_ == bytes;
  }

  uint64_t Bytes() const { return bytes_; }
  const char* Name() const { return name_; }

 private:
  void MoveFrom(OwnedBuffer& other) noexcept {
    resource_ = std::move(other.resource_);
    bytes_ = other.bytes_;
    name_ = other.name_;
    other.bytes_ = 0;
    other.name_ = "";
  }

  Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  uint64_t bytes_ = 0;
  const char* name_ = "";
};

// A counted reference on a resource the addon does not own - a game texture
// held for a frame or two, or a fallback guide. The only way to obtain one is
// BorrowedRef::AddRef(), so "retire a resource I hold no reference to" cannot
// be written. Release goes through the shared retire queue like every other
// replacement.
class BorrowedRef final {
 public:
  BorrowedRef() = default;
  ~BorrowedRef() { Reset(); }
  BorrowedRef(const BorrowedRef&) = delete;
  BorrowedRef& operator=(const BorrowedRef&) = delete;
  BorrowedRef(BorrowedRef&& other) noexcept { MoveFrom(other); }
  BorrowedRef& operator=(BorrowedRef&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  // Takes a reference on `resource`. Null yields an empty ref.
  static BorrowedRef AddRef(ID3D12Resource* resource);

  // Retires the reference. `fence_value` binds it to a queue marker the caller
  // has already signaled; zero leaves it for the next Present to tag.
  void Reset(uint64_t fence_value = 0);

  ID3D12Resource* Get() const { return resource_; }
  explicit operator bool() const { return resource_ != nullptr; }
  bool operator==(std::nullptr_t) const { return resource_ == nullptr; }
  bool operator!=(std::nullptr_t) const { return resource_ != nullptr; }

 private:
  void MoveFrom(BorrowedRef& other) noexcept {
    resource_ = other.resource_;
    other.resource_ = nullptr;
  }

  ID3D12Resource* resource_ = nullptr;
};

}  // namespace reshade_dlssnr
