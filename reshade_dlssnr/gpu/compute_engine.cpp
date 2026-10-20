#include "compute_engine.h"
#include "profiling/profile.h"


#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include <reshade.hpp>

#include "compute_passes.h"
#include "ctl/types.h"
#include "generated/shader_sources.h"
#include "generated/shader_bytecode.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

std::string ShaderDiagnosticKey(const char* pass_name,
                                reshade::log::level level,
                                const std::string& text) {
  std::string key = CStrToString(pass_name);
  if (pass_name == nullptr) key = "<unknown>";
  key += '\n';
  key += level == reshade::log::level::error ? "error" : "warning";
  key += '\n';
  key += text;
  return key;
}

std::string TrimShaderDiagnostic(const void* data, size_t size) {
  if (data == nullptr || size == 0) return {};

  std::string text(static_cast<const char*>(data), size);
  while (!text.empty() && (text.back() == '\0' || text.back() == '\r' ||
                           text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}


}  // namespace

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

bool ComputeEngine::BindAndDispatch(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* pipeline_state,
    const ComputeConstants& constants,
    ID3D12Resource* const* srvs,
    ID3D12Resource* const* uavs,
    uint32_t threads_x,
    uint32_t threads_y,
    uint32_t threads_z) {
  DLSSNR_PROFILE_RENDER_L3("ComputeEngine::BindAndDispatch");
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
    ID3D12Resource* resource = uavs[i] != nullptr ? uavs[i] : uavs[0];
    CreateUnorderedAccessView(resource, handle);
  }

  std::memcpy(constants_mapped_ +
                  static_cast<size_t>(slot) * kConstantStride,
              &constants, sizeof(constants));

  // Thread-group grid. Callers can override for passes whose grid is not
  // tied to the frame size; the built-in 8×8 group passes use the default.
  const uint32_t grid_x =
      threads_x != 0 ? threads_x : (constants.Width + 7) / 8;
  const uint32_t grid_y =
      threads_y != 0 ? threads_y : (constants.Height + 7) / 8;
  const uint32_t grid_z = threads_z != 0 ? threads_z : 1;

  if (bound_command_list_ != command_list) {
    bound_command_list_ = command_list;
    descriptor_heap_bound_ = false;
    root_signature_bound_ = false;
  }
  if (!descriptor_heap_bound_) {
    ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    descriptor_heap_bound_ = true;
  }
  if (!root_signature_bound_) {
    command_list->SetComputeRootSignature(root_signature_.Get());
    root_signature_bound_ = true;
  }
  command_list->SetPipelineState(pipeline_state);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(grid_x, grid_y, grid_z);

  return true;
}

bool ComputeEngine::CreateRootSignature(ID3D12Device* device) {
  DLSSNR_PROFILE_RESOURCE_L2("ComputeEngine::CreateRootSignature");
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
    const ComputePassDef& def,
    ID3D12PipelineState** out_pso) {
  DLSSNR_PROFILE_ASYNC_L2("ComputeEngine::CompilePipeline");
  if (device_ == nullptr || out_pso == nullptr) return false;

  *out_pso = nullptr;

  const char* pass_name = def.name != nullptr ? def.name : "<unknown>";
  const EmbeddedShader* source =
      def.root != nullptr ? FindEmbeddedShader(def.root) : nullptr;

  const unsigned char* bytecode = def.bytecode;
  std::size_t bytecode_size = def.bytecode_size;
  if (bytecode == nullptr || bytecode_size == 0) {
    const EmbeddedShaderBytecode* embedded_bytecode =
        def.root != nullptr ? FindEmbeddedShaderBytecode(def.root) : nullptr;
    if (embedded_bytecode != nullptr) {
      bytecode = embedded_bytecode->data;
      bytecode_size = embedded_bytecode->size;
    }
  }

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* diagnostic_blob = nullptr;

  if (bytecode == nullptr || bytecode_size == 0) {
    if (source == nullptr || source->fragments == nullptr ||
        source->fragment_count == 0) {
      char message[384];
      std::snprintf(message, sizeof(message),
                    "DLSSNR shader source missing or empty: pass=%s root=%s",
                    pass_name, def.root != nullptr ? def.root : "<null>");
      const std::string key =
          ShaderDiagnosticKey(pass_name, reshade::log::level::error, message);
      bool duplicate = false;
      {
        std::lock_guard<std::mutex> lock(diagnostic_mutex_);
        duplicate = !logged_shader_diagnostics_.insert(key).second;
      }
      if (!duplicate)
        reshade::log::message(reshade::log::level::error, message);
      return false;
    }

    std::size_t source_size = 0;
    for (std::size_t i = 0; i < source->fragment_count; ++i) {
      const EmbeddedShaderFragment* fragment = source->fragments[i];
      if (fragment == nullptr || fragment->source == nullptr ||
          fragment->size == 0 ||
          fragment->size > std::numeric_limits<std::size_t>::max() - source_size) {
        char message[384];
        std::snprintf(message, sizeof(message),
                      "DLSSNR shader source has an invalid fragment: pass=%s root=%s index=%zu",
                      pass_name, def.root != nullptr ? def.root : "<null>", i);
        const std::string key =
            ShaderDiagnosticKey(pass_name, reshade::log::level::error, message);
        bool duplicate = false;
        {
          std::lock_guard<std::mutex> lock(diagnostic_mutex_);
          duplicate = !logged_shader_diagnostics_.insert(key).second;
        }
        if (!duplicate)
          reshade::log::message(reshade::log::level::error, message);
        return false;
      }
      source_size += fragment->size;
    }

    std::string assembled_source;
    assembled_source.reserve(source_size);
    for (std::size_t i = 0; i < source->fragment_count; ++i) {
      const EmbeddedShaderFragment* fragment = source->fragments[i];
      assembled_source.append(fragment->source, fragment->size);
    }

    const HRESULT compile_hr = D3DCompile(
        assembled_source.data(),
        assembled_source.size(),
        def.root,
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shader_blob,
        &diagnostic_blob);

    if (diagnostic_blob != nullptr) {
      const std::string diagnostic =
          TrimShaderDiagnostic(diagnostic_blob->GetBufferPointer(),
                               diagnostic_blob->GetBufferSize());
      if (!diagnostic.empty()) {
        const reshade::log::level level =
            FAILED(compile_hr) ? reshade::log::level::error
                               : reshade::log::level::warning;
        const std::string key = ShaderDiagnosticKey(pass_name, level, diagnostic);
        bool duplicate = false;
        {
          std::lock_guard<std::mutex> lock(diagnostic_mutex_);
          duplicate = !logged_shader_diagnostics_.insert(key).second;
        }
        if (!duplicate) {
          char header[384];
          std::snprintf(header, sizeof(header),
                        "DLSSNR shader %s: pass=%s root=%s",
                        level == reshade::log::level::error ? "error" : "warning",
                        pass_name,
                        def.root != nullptr ? def.root : "<null>");
          reshade::log::message(level, header);
          reshade::log::message(level, diagnostic.c_str());
        }
      }
      diagnostic_blob->Release();
      diagnostic_blob = nullptr;
    }

    if (FAILED(compile_hr) || shader_blob == nullptr) {
      char message[384];
      std::snprintf(message, sizeof(message),
                    "DLSSNR shader compile failed: pass=%s root=%s hr=0x%08lX",
                    pass_name,
                    def.root != nullptr ? def.root : "<null>",
                    static_cast<unsigned long>(compile_hr));
      const std::string key =
          ShaderDiagnosticKey(pass_name, reshade::log::level::error, message);
      bool duplicate = false;
      {
        std::lock_guard<std::mutex> lock(diagnostic_mutex_);
        duplicate = !logged_shader_diagnostics_.insert(key).second;
      }
      if (!duplicate)
        reshade::log::message(reshade::log::level::error, message);
      if (shader_blob != nullptr) shader_blob->Release();
      return false;
    }
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  if (shader_blob != nullptr) {
    pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();
  } else {
    pso_desc.CS.pShaderBytecode = bytecode;
    pso_desc.CS.BytecodeLength = bytecode_size;
  }
  pso_desc.NodeMask = 0;

  const HRESULT pso_hr = device_->CreateComputePipelineState(
      &pso_desc, IID_PPV_ARGS(out_pso));
  if (shader_blob != nullptr) shader_blob->Release();

  if (FAILED(pso_hr) || *out_pso == nullptr) {
    char message[384];
    std::snprintf(message, sizeof(message),
                  "DLSSNR shader pipeline creation failed: pass=%s root=%s hr=0x%08lX",
                  pass_name,
                  def.root != nullptr ? def.root : "<null>",
                  static_cast<unsigned long>(pso_hr));
    const std::string key =
        ShaderDiagnosticKey(pass_name, reshade::log::level::error, message);
    bool duplicate = false;
    {
      std::lock_guard<std::mutex> lock(diagnostic_mutex_);
      duplicate = !logged_shader_diagnostics_.insert(key).second;
    }
    if (!duplicate)
      reshade::log::message(reshade::log::level::error, message);
    return false;
  }

  return true;
}

bool ComputeEngine::IsPassCompiled(const char* pass_name) {
  if (pass_name == nullptr) return false;
  std::lock_guard<std::mutex> lock(passes_mutex_);
  return passes_.find(pass_name) != passes_.end();
}

bool ComputeEngine::IsPassFailed(const char* pass_name) {
  if (pass_name == nullptr) return false;
  std::lock_guard<std::mutex> lock(passes_mutex_);
  return failed_passes_.find(pass_name) != failed_passes_.end();
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
  DLSSNR_PROFILE_RENDER_L2("ComputeEngine::Dispatch");
  DLSSNR_PROFILE_RENDER_TAG_L2("ComputePass", pass_name != nullptr ? pass_name : "<null>");

  if (device_ == nullptr || pass_name == nullptr ||
      command_list == nullptr || srvs == nullptr || uavs == nullptr) {
    return false;
  }

  // Every descriptor slot is consumed unconditionally below. Reject a null
  // resource before CreateShaderResourceView/CreateUnorderedAccessView can
  // dereference it. Callers should repeat an otherwise-unused valid resource
  // to satisfy the fixed descriptor-table contract.
  for (uint32_t i = 0; i < kSrvCount; ++i) {
    if (srvs[i] == nullptr) return false;
  }
  if (uavs[0] == nullptr) return false;

  ID3D12PipelineState* pso = nullptr;

  {
    std::lock_guard<std::mutex> lock(passes_mutex_);

    auto it = passes_.find(pass_name);
    if (it == passes_.end()) {
      // Never compile on the render thread while the background compiler is
      // still active. After it is ready, a missing pass gets one fallback
      // attempt unless the worker already recorded it as failed.
      if (!IsReady()) return false;
      if (failed_passes_.find(pass_name) != failed_passes_.end()) {
        return false;
      }

      const ComputePassDef* def = FindComputePassDef(pass_name);
      if (def == nullptr) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "DLSSNR shader dispatch requested unknown pass: %s",
                      pass_name);
        failed_passes_.insert(pass_name);
        reshade::log::message(reshade::log::level::error, message);
        return false;
      }

      CachedPass pass;
      if (!CompilePipeline(*def, pass.pso.GetAddressOf())) {
        failed_passes_.insert(pass_name);
        return false;
      }

      it = passes_.emplace(pass_name, std::move(pass)).first;
    }

    pso = it->second.pso.Get();
  }

  // Logged before BindAndDispatch so the pass name and constants line up with
  // the transitions that precede and follow it in the trace.
  if (trace::Enabled()) {
    const float* params = &constants.shader.Params0.x;
    constexpr uint32_t kParamCapacity =
        (sizeof(ShaderBlock) + sizeof(GradeBlock) + sizeof(ResidualBlock) +
         sizeof(HdrBlock) + sizeof(HistogramBlock) + sizeof(TemporalBlock)) / sizeof(float);
    uint32_t param_count = 0;
    for (uint32_t i = 0; i < kParamCapacity; ++i) {
      if (params[i] != 0.0f) param_count = i + 1;
    }

    char params_text[48 * 10] = {};
    int used = 0;
    for (uint32_t i = 0;
         i < param_count && used < static_cast<int>(sizeof(params_text)) - 12; ++i) {
      const int n = std::snprintf(params_text + used, sizeof(params_text) - used,
                                  "%s%.6g", i == 0 ? "" : ",",
                                  static_cast<double>(params[i]));
      if (n <= 0) break;
      used += n;
    }
    trace::Line("cmd", "Dispatch", this,
                "dispatch pass=%s w=%u h=%u grid=%u,%u,%u "
                "srv=[%d,%d,%d,%d] uav=[%d] params=[%s]",
                pass_name, constants.Width, constants.Height,
                threads_x != 0 ? threads_x : (constants.Width + 7) / 8,
                threads_y != 0 ? threads_y : (constants.Height + 7) / 8,
                threads_z != 0 ? threads_z : 1,
                trace::ResourceId(srvs[0]),
                kSrvCount > 1 ? trace::ResourceId(srvs[1]) : -1,
                kSrvCount > 2 ? trace::ResourceId(srvs[2]) : -1,
                kSrvCount > 3 ? trace::ResourceId(srvs[3]) : -1,
                trace::ResourceId(uavs[0]), params_text);
  }

  DLSSNR_PROFILE_GPU_CONTEXT_L2(command_list);
  DLSSNR_PROFILE_GPU_L2("Compute Dispatch");
  return BindAndDispatch(
      command_list, pso, constants, srvs, uavs,
      threads_x, threads_y, threads_z);
}

bool ComputeEngine::Init(ID3D12Device* device) {
  DLSSNR_PROFILE_RESOURCE_L1("ComputeEngine::Init");
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
  buffer_desc.Width = kDescriptorRingSize * kConstantStride;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  if (FAILED(device->CreateCommittedResource(
          &heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(constants_.GetAddressOf())))) {
    return false;
  }

  void* mapped_constants = nullptr;
  D3D12_RANGE read_range = {0, 0};
  if (FAILED(constants_->Map(0, &read_range, &mapped_constants))) {
    constants_.Reset();
    descriptor_heap_.Reset();
    root_signature_.Reset();
    descriptor_size_ = 0;
    return false;
  }
  constants_mapped_ = static_cast<uint8_t*>(mapped_constants);

  D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
      descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t slot = 0; slot < kDescriptorRingSize; ++slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = heap_start;
    cbv_handle.ptr += static_cast<SIZE_T>(slot) *
        kDescriptorsPerDispatch * descriptor_size_;
    cbv_handle.ptr += static_cast<SIZE_T>(kSrvCount + kUavCount) *
        descriptor_size_;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
    cbv.BufferLocation = constants_->GetGPUVirtualAddress() +
                         static_cast<uint64_t>(slot) * kConstantStride;
    cbv.SizeInBytes = kConstantStride;
    device->CreateConstantBufferView(&cbv, cbv_handle);
  }

  ready_.store(false, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(passes_mutex_);
    passes_.clear();
    failed_passes_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(diagnostic_mutex_);
    logged_shader_diagnostics_.clear();
  }

  device_ = device;

  // Kick off the background compile. The render thread never compiles while
  // the worker is active; a post-ready miss gets at most one fallback attempt.
  worker_stop_ = false;
  worker_ = std::thread(&ComputeEngine::WorkerCompileAll, this);

  return true;
}

ComputeEngine::~ComputeEngine() {
  Destroy();
}

void ComputeEngine::Destroy() {
  DLSSNR_PROFILE_RESOURCE_L1("ComputeEngine::Destroy");
  // Stop and join first: the worker reads device_ and root_signature_ and
  // writes passes_, all of which are torn down below.
  {
    std::lock_guard<std::mutex> lock(passes_mutex_);
    worker_stop_ = true;
  }
  if (worker_.joinable()) {
    worker_.join();
  }

  if (constants_ != nullptr && constants_mapped_ != nullptr) {
    D3D12_RANGE written_range = {
        0, static_cast<SIZE_T>(kDescriptorRingSize) * kConstantStride};
    constants_->Unmap(0, &written_range);
    constants_mapped_ = nullptr;
  }
  constants_.Reset();

  {
    std::lock_guard<std::mutex> lock(passes_mutex_);
    passes_.clear();
    failed_passes_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(diagnostic_mutex_);
    logged_shader_diagnostics_.clear();
  }
  root_signature_.Reset();
  descriptor_heap_.Reset();
  InvalidateBindings();

  device_ = nullptr;
}

void ComputeEngine::WorkerCompileAll() {
  DLSSNR_PROFILE_ASYNC_L1("ComputeEngine::WorkerCompileAll");
  for (uint32_t i = 0; i < kComputePassCount; ++i) {
    const char* name = kComputePasses[i].name;

    CachedPass pass;
    if (!CompilePipeline(kComputePasses[i], pass.pso.GetAddressOf())) {
      std::lock_guard<std::mutex> lock(passes_mutex_);
      if (worker_stop_) return;
      failed_passes_.insert(name);
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
