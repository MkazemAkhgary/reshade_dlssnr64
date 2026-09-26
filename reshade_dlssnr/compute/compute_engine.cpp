#include "compute_engine.h"

#include <cstdio>

#include <reshade.hpp>

#include "compute_passes.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

DXGI_FORMAT ComputeEngine::TypedFormat(DXGI_FORMAT format) const {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      return format;
  }
}

void ComputeEngine::CreateShaderResourceView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const D3D12_RESOURCE_DESC res_desc = resource->GetDesc();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

  if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    // Typed R32_UINT buffer. HLSL: `Buffer<uint>`. NumElements is in
    // 32-bit elements, so the byte size is divided by 4.
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = static_cast<UINT>(res_desc.Width / 4);
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  } else {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Format = TypedFormat(res_desc.Format);
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.PlaneSlice = 0;
    desc.Texture2D.ResourceMinLODClamp = 0.0f;
  }

  device_->CreateShaderResourceView(resource, &desc, handle);
}

void ComputeEngine::CreateUnorderedAccessView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const D3D12_RESOURCE_DESC res_desc = resource->GetDesc();
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};

  if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    // Typed R32_UINT buffer. HLSL: `RWBuffer<uint>`. Supports atomics via
    // `InterlockedAdd`. NumElements is in 32-bit elements.
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = static_cast<UINT>(res_desc.Width / 4);
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.CounterOffsetInBytes = 0;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
  } else {
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Format = TypedFormat(res_desc.Format);
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = 0;
  }

  device_->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void ComputeEngine::CreateConstantBufferView(ID3D12Resource* resource, const void* data, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  void* mapped = nullptr;
  D3D12_RANGE read_range = {};
  if (SUCCEEDED(resource->Map(0, &read_range, &mapped))) {
    memcpy(mapped, data, size);
    D3D12_RANGE write_range = {0, size};
    resource->Unmap(0, &write_range);
  }

  D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
  desc.BufferLocation = resource->GetGPUVirtualAddress();
  desc.SizeInBytes = 256;
  device_->CreateConstantBufferView(&desc, handle);
}

bool ComputeEngine::BindAndDispatch(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* pipeline_state,
    const ComputeConstants& constants,
    ID3D12Resource* const* srvs,
    ID3D12Resource* const* uavs,
    uint32_t threads_x,
    uint32_t threads_y,
    uint32_t threads_z) {
  if (command_list == nullptr || pipeline_state == nullptr ||
      srvs == nullptr || uavs == nullptr) {
    return false;
  }

  const uint32_t slot = heap_index_;
  heap_index_ = (heap_index_ + 1) % kDescriptorRingSize;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
      descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu_handle.ptr += static_cast<SIZE_T>(slot) *
                    kDescriptorsPerDispatch * descriptor_size_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle =
      descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu_handle.ptr += static_cast<UINT64>(slot) *
                    kDescriptorsPerDispatch * descriptor_size_;

  for (uint32_t i = 0; i < kSrvCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(i) * descriptor_size_;
    CreateShaderResourceView(srvs[i], handle);
  }

  for (uint32_t i = 0; i < kUavCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(kSrvCount + i) *
                  descriptor_size_;
    CreateUnorderedAccessView(uavs[i], handle);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = cpu_handle;
  cbv_handle.ptr += static_cast<SIZE_T>(kSrvCount + kUavCount) *
                    descriptor_size_;
  CreateConstantBufferView(
      constant_buffers_[slot].Get(), &constants, sizeof(constants), cbv_handle);

  // Thread-group grid. Callers can override for passes whose grid is not
  // tied to the frame size; the built-in 8×8 group passes use the default.
  const uint32_t grid_x =
      threads_x != 0 ? threads_x : (constants.Width + 7) / 8;
  const uint32_t grid_y =
      threads_y != 0 ? threads_y : (constants.Height + 7) / 8;
  const uint32_t grid_z = threads_z != 0 ? threads_z : 1;

  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(pipeline_state);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(grid_x, grid_y, grid_z);

  return true;
}

bool ComputeEngine::CreateRootSignature(ID3D12Device* device) {
  D3D12_DESCRIPTOR_RANGE ranges[3] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = kSrvCount;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = kUavCount;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  ranges[2].NumDescriptors = kCbvCount;
  ranges[2].BaseShaderRegister = 0;
  ranges[2].RegisterSpace = 0;
  ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER root_params[1] = {};
  root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[0].DescriptorTable.NumDescriptorRanges = 3;
  root_params[0].DescriptorTable.pDescriptorRanges = ranges;

  D3D12_STATIC_SAMPLER_DESC static_sampler = {};
  static_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  static_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.MipLODBias = 0.0f;
  static_sampler.MaxAnisotropy = 0;
  static_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  static_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  static_sampler.MinLOD = 0.0f;
  static_sampler.MaxLOD = D3D12_FLOAT32_MAX;
  static_sampler.ShaderRegister = 0;
  static_sampler.RegisterSpace = 0;
  static_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 1;
  root_desc.pParameters = root_params;
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &static_sampler;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ID3DBlob* signature = nullptr;
  ID3DBlob* error = nullptr;
  if (FAILED(D3D12SerializeRootSignature(
          &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
    if (error != nullptr) {
      reshade::log::message(
          reshade::log::level::error,
          "DLSSNR codec root signature serialization failed");
      error->Release();
    }
    return false;
  }

  if (FAILED(device->CreateRootSignature(
          0,
          signature->GetBufferPointer(),
          signature->GetBufferSize(),
          IID_PPV_ARGS(root_signature_.GetAddressOf())))) {
    signature->Release();
    return false;
  }
  signature->Release();

  return true;
}

bool ComputeEngine::CompilePipeline(
    const char* hlsl,
    ID3D12PipelineState** out_pso) {
  if (device_ == nullptr || hlsl == nullptr || out_pso == nullptr) {
    return false;
  }

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          hlsl,
          strlen(hlsl),
          nullptr,
          nullptr,
          nullptr,
          "CSMain",
          "cs_5_1",
          D3DCOMPILE_OPTIMIZATION_LEVEL3,
          0,
          &shader_blob,
          &error_blob))) {
    if (error_blob != nullptr) {
      reshade::log::message(
          reshade::log::level::error,
          static_cast<const char*>(error_blob->GetBufferPointer()));
      error_blob->Release();
    } else {
      reshade::log::message(
          reshade::log::level::error,
          "D3DCompile failed without an error blob");
    }
    return false;
  }

  if (error_blob != nullptr) error_blob->Release();

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
  pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();
  pso_desc.NodeMask = 0;

  const HRESULT hr = device_->CreateComputePipelineState(
      &pso_desc, IID_PPV_ARGS(out_pso));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool ComputeEngine::Dispatch(
    const char* pass_name,
    ID3D12GraphicsCommandList* command_list,
    const ComputeConstants& constants,
    ID3D12Resource* const* srvs,
    ID3D12Resource* const* uavs,
    uint32_t threads_x,
    uint32_t threads_y,
    uint32_t threads_z) {
  if (device_ == nullptr || pass_name == nullptr ||
      command_list == nullptr || srvs == nullptr || uavs == nullptr) {
    return false;
  }

  ID3D12PipelineState* pso = nullptr;

  {
    std::lock_guard<std::mutex> lock(passes_mutex_);

    auto it = passes_.find(pass_name);
    if (it == passes_.end()) {
      // Refuse to compile synchronously while the worker is still running:
      // that would reintroduce the freeze this is designed to avoid. Once
      // the worker has finished, a miss means the pass genuinely failed to
      // compile (or was never registered), and the synchronous retry is
      // both cheap (D3DCompile on a bad shader fails fast) and informative
      // (CompilePipeline logs the error).
      if (!IsReady()) return false;

      const char* source = FindComputePassSource(pass_name);
      if (source == nullptr) return false;

      CachedPass pass;
      if (!CompilePipeline(source, pass.pso.GetAddressOf())) {
        std::string msg = "Failed to compile compute pass: ";
        msg += pass_name;
        reshade::log::message(reshade::log::level::error, msg.c_str());
        return false;
      }

      it = passes_.emplace(pass_name, std::move(pass)).first;
    }

    pso = it->second.pso.Get();
  }

  // Logged before BindAndDispatch so the pass name and constants line up with
  // the transitions that precede and follow it in the trace.
  if (trace::Enabled()) {
    // Only the used prefix carries information: every param past the last
    // non-zero one is zero, so formatting them is cost and noise.
    constexpr uint32_t kParamCapacity =
        sizeof(constants.params) / sizeof(constants.params[0]);
    uint32_t param_count = 0;
    for (uint32_t i = 0; i < kParamCapacity; ++i) {
      if (constants.params[i] != 0.0f) param_count = i + 1;
    }

    char params[48 * 10] = {};
    int used = 0;
    for (uint32_t i = 0;
         i < param_count && used < static_cast<int>(sizeof(params)) - 12; ++i) {
      const int n = std::snprintf(params + used, sizeof(params) - used,
                                  "%s%.6g", i == 0 ? "" : ",",
                                  static_cast<double>(constants.params[i]));
      if (n <= 0) break;
      used += n;
    }
    trace::Line("cmd", "Dispatch", this,
                "dispatch pass=%s w=%u h=%u grid=%u,%u,%u "
                "srv=[%d,%d,%d] uav=[%d] params=[%s]",
                pass_name, constants.Width, constants.Height,
                threads_x != 0 ? threads_x : (constants.Width + 7) / 8,
                threads_y != 0 ? threads_y : (constants.Height + 7) / 8,
                threads_z != 0 ? threads_z : 1,
                trace::ResourceId(srvs[0]),
                kSrvCount > 1 ? trace::ResourceId(srvs[1]) : -1,
                kSrvCount > 2 ? trace::ResourceId(srvs[2]) : -1,
                trace::ResourceId(uavs[0]), params);
  }

  return BindAndDispatch(
      command_list, pso, constants, srvs, uavs,
      threads_x, threads_y, threads_z);
}

bool ComputeEngine::Init(ID3D12Device* device) {
  if (device_ != nullptr) return true;
  if (device == nullptr) return false;

  if (!CreateRootSignature(device)) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kDescriptorRingSize * kDescriptorsPerDispatch;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  heap_desc.NodeMask = 0;

  if (FAILED(device->CreateDescriptorHeap(
          &heap_desc, IID_PPV_ARGS(descriptor_heap_.GetAddressOf())))) {
    return false;
  }

  descriptor_size_ =
      device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_HEAP_PROPERTIES heap_props = {};
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Alignment = 0;
  buffer_desc.Width = sizeof(ComputeConstants);
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  for (uint32_t i = 0; i < kDescriptorRingSize; ++i) {
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(constant_buffers_[i].GetAddressOf())))) {
      return false;
    }
  }

  ready_.store(false, std::memory_order_relaxed);

  device_ = device;

  // Kick off the background compile. Dispatch still compiles synchronously
  // on a miss, so a caller that arrives before the worker finishes sees the
  // old behaviour rather than a new failure mode.
  worker_stop_ = false;
  worker_ = std::thread(&ComputeEngine::WorkerCompileAll, this);

  return true;
}

ComputeEngine::~ComputeEngine() {
  Destroy();
}

void ComputeEngine::Destroy() {
  // Stop and join first: the worker reads device_ and root_signature_ and
  // writes passes_, all of which are torn down below.
  {
    std::lock_guard<std::mutex> lock(passes_mutex_);
    worker_stop_ = true;
  }
  if (worker_.joinable()) {
    worker_.join();
  }

  for (auto& buffer : constant_buffers_) {
    buffer.Reset();
  }

  {
    std::lock_guard<std::mutex> lock(passes_mutex_);
    passes_.clear();
  }
  root_signature_.Reset();
  descriptor_heap_.Reset();

  device_ = nullptr;
}

void ComputeEngine::WorkerCompileAll() {
  for (uint32_t i = 0; i < kComputePassCount; ++i) {
    const char* name = kComputePasses[i].name;

    CachedPass pass;
    if (!CompilePipeline(kComputePasses[i].hlsl_source,
                         pass.pso.GetAddressOf())) {
      // CompilePipeline already logged the HLSL error. Dispatch will
      // retry synchronously once ready_ is set, so a bad shader surfaces
      // through the normal failure path instead of silently.
      continue;
    }

    // Dispatch cannot insert into passes_ while !IsReady(), so the worker
    // is the only writer during its run. The lock guards the emplace
    // against a concurrent Destroy().
    std::lock_guard<std::mutex> lock(passes_mutex_);
    if (worker_stop_) return;
    passes_.emplace(name, std::move(pass));
  }

  ready_.store(true, std::memory_order_release);
}

}  // namespace reshade_dlssnr
