// AI生成
#include "render/d3d12_backend.h"

#include <string>
#include <vector>

#include "core/comp_geometry.h"
#include "render/gpu_kernel.h"

#if defined(COMP_HAS_D3D12)

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstring>

namespace compositor {

namespace {

/* ------------------------------------------------------------------
 * 动态入口：d3d12.dll / d3dcompiler_47.dll。
 * 本仓不依赖 d3d12.lib / dxgi.lib 导入库，入口全部运行期解析；
 * 接口方法调用走 COM vtable，无需导入库。
 * ------------------------------------------------------------------ */

typedef HRESULT(WINAPI* PFN_CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT(WINAPI* PFN_SerializeVersionedRootSignature)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                        ID3DBlob**);

struct D3D12Api {
  HMODULE core = nullptr;
  HMODULE compiler = nullptr;
  PFN_CreateDevice create_device = nullptr;
  PFN_SerializeVersionedRootSignature serialize_root = nullptr;
  PFN_D3DCompile compile = nullptr;

  bool ok() const { return create_device != nullptr && serialize_root != nullptr && compile != nullptr; }
};

template <typename T>
void release(T*& ptr) {
  if (ptr != nullptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

const D3D12Api& d3d12_api() {
  static D3D12Api api = [] {
    D3D12Api a;
    a.core = ::LoadLibraryA("d3d12.dll");
    if (a.core != nullptr) {
      a.create_device = reinterpret_cast<PFN_CreateDevice>(
          reinterpret_cast<void*>(::GetProcAddress(a.core, "D3D12CreateDevice")));
      a.serialize_root = reinterpret_cast<PFN_SerializeVersionedRootSignature>(
          reinterpret_cast<void*>(::GetProcAddress(a.core, "D3D12SerializeVersionedRootSignature")));
    }
    a.compiler = ::LoadLibraryA("d3dcompiler_47.dll");
    if (a.compiler != nullptr) {
      a.compile = reinterpret_cast<PFN_D3DCompile>(
          reinterpret_cast<void*>(::GetProcAddress(a.compiler, "D3DCompile")));
    }
    return a;
  }();
  return api;
}

/* 取得设备（优先硬件适配器，失败则 WARP 由驱动自动兜底）。 */
HRESULT create_device(ID3D12Device** out) {
  const D3D12Api& api = d3d12_api();
  if (api.create_device == nullptr) return E_FAIL;
  return api.create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                           reinterpret_cast<void**>(out));
}

bool layer_is_gpu_ready(const Layer& layer, int32_t width, int32_t height, std::string* why) {
  if (layer.is_group) {
    if (why != nullptr) *why = "gpu backend does not handle layer groups yet";
    return false;
  }
  if (!layer.has_pixels()) {
    if (why != nullptr) *why = "layer has no pixels";
    return false;
  }
  if (layer.pixels.width != width || layer.pixels.height != height ||
      layer.pixels.format != COMP_PIXEL_RGBA8) {
    if (why != nullptr) *why = "gpu backend needs full-canvas RGBA8 layers (no crop/resample yet)";
    return false;
  }
  if (!layer.mask.empty()) {
    if (why != nullptr) *why = "gpu backend does not bake layer masks yet";
    return false;
  }
  if (layer.clipping_source != 0) {
    if (why != nullptr) *why = "gpu backend does not handle clipping masks yet";
    return false;
  }
  return true;
}

}  // namespace

bool probe_d3d12(std::string* why_not) {
  const D3D12Api& api = d3d12_api();
  if (api.core == nullptr) {
    if (why_not != nullptr) *why_not = "d3d12.dll not found";
    return false;
  }
  if (!api.ok()) {
    if (why_not != nullptr) *why_not = "d3d12.dll / d3dcompiler_47.dll exports missing";
    return false;
  }
  ID3D12Device* device = nullptr;
  const HRESULT hr = create_device(&device);
  if (FAILED(hr) || device == nullptr) {
    if (why_not != nullptr) *why_not = "D3D12CreateDevice failed (no hardware or WARP adapter)";
    return false;
  }
  device->Release();
  return true;
}

bool D3d12RenderBackend::usable() const { return probe_d3d12(nullptr); }

BackendCaps D3d12RenderBackend::caps() const {
  BackendCaps c;
  c.compiled = true;
  c.device_available = probe_d3d12(nullptr);
  c.offscreen_compute = true;
  c.window_present = false;
  c.note = c.device_available
               ? "D3D12 offscreen compute + readback (HLSL compiled at runtime)"
               : "D3D12 device unavailable on this machine";
  return c;
}

bool D3d12RenderBackend::render_document(Document& doc, const comp_render_target& target,
                                         std::string* error) {
  const DocumentState& state = doc.state();
  if (target.kind != COMP_SURFACE_CPU_BUFFER) {
    if (error != nullptr) {
      *error = "d3d12 backend currently renders offscreen only (COMP_SURFACE_CPU_BUFFER)";
    }
    return false;
  }
  if (target.pixels == nullptr || target.format != COMP_PIXEL_RGBA8) {
    if (error != nullptr) *error = "d3d12 backend needs a non-null RGBA8 target buffer";
    return false;
  }
  if (target.size.width != state.width || target.size.height != state.height) {
    if (error != nullptr) *error = "render target size does not match canvas";
    return false;
  }

  /* 收集绘制序（自底向上）并做能力体检；任一层不满足即回落，不静默出错。 */
  std::vector<const Layer*> paint;
  for (const Layer& layer : state.layers) {
    if (layer.parent != 0 || !layer.visible) continue;
    std::string why;
    if (!layer_is_gpu_ready(layer, state.width, state.height, &why)) {
      if (error != nullptr) *error = "d3d12 backend: " + why;
      return false;
    }
    paint.push_back(&layer);
  }

  const D3D12Api& api = d3d12_api();
  if (!api.ok()) {
    if (error != nullptr) *error = "d3d12 backend: d3d12.dll / d3dcompiler_47.dll unavailable";
    return false;
  }

  const UINT width = static_cast<UINT>(state.width);
  const UINT height = static_cast<UINT>(state.height);
  const UINT row_bytes = width * 4;
  const UINT aligned_row = (row_bytes + 255u) & ~255u; /* 纹理上传行距须 256 对齐 */
  const std::string hresult_note = "d3d12 backend: hresult 0x";

  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12RootSignature* root = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12Resource* src_tex = nullptr;
  ID3D12Resource* dst_tex = nullptr;
  ID3D12Resource* upload = nullptr;
  ID3D12Resource* readback = nullptr;
  ID3D12Fence* fence = nullptr;
  ID3DBlob* sig_blob = nullptr;
  ID3DBlob* sig_err = nullptr;
  ID3DBlob* cs_blob = nullptr;
  ID3DBlob* cs_err = nullptr;
  HANDLE event_handle = nullptr;
  bool ok = false;

  auto fail = [&](const std::string& what) {
    if (error != nullptr) *error = hresult_note + what;
  };

  do {
    if (FAILED(create_device(&device))) { fail("create device failed"); break; }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
      fail("create queue failed");
      break;
    }
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)))) {
      fail("create allocator failed");
      break;
    }

    /* 根签名：b0 根常量（8 个 32 位）+ u0 目标 UAV + u1 源 UAV。 */
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].Descriptor.RegisterSpace = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC sig_desc{};
    sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    sig_desc.Desc_1_1.NumParameters = 3;
    sig_desc.Desc_1_1.pParameters = params;
    sig_desc.Desc_1_1.NumStaticSamplers = 0;
    sig_desc.Desc_1_1.pStaticSamplers = nullptr;
    sig_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    if (FAILED(api.serialize_root(&sig_desc, &sig_blob, &sig_err))) {
      fail("serialize root signature failed");
      break;
    }
    if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                           sig_blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
      fail("create root signature failed");
      break;
    }

    const char* hlsl = gpu_kernel_hlsl();
    if (FAILED(api.compile(hlsl, std::strlen(hlsl), "gpu_kernel.hlsl", nullptr, nullptr, "main",
                           "cs_5_0", 0, 0, &cs_blob, &cs_err))) {
      fail("compile HLSL failed");
      break;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root;
    pso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength = cs_blob->GetBufferSize();
    pso_desc.NodeMask = 0;
    pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)))) {
      fail("create compute PSO failed");
      break;
    }

    auto make_texture = [&](ID3D12Resource** out) {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      heap.CreationNodeMask = 1;
      heap.VisibleNodeMask = 1;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = width;
      desc.Height = height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(out));
    };
    if (FAILED(make_texture(&src_tex)) || FAILED(make_texture(&dst_tex))) {
      fail("create texture failed");
      break;
    }

    /* 上传缓冲：一次容纳一层的对齐像素数据。 */
    const UINT64 upload_size = static_cast<UINT64>(aligned_row) * height;
    {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_UPLOAD;
      heap.CreationNodeMask = 1;
      heap.VisibleNodeMask = 1;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = upload_size;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_UNKNOWN;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload)))) {
        fail("create upload buffer failed");
        break;
      }
    }
    {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_READBACK;
      heap.CreationNodeMask = 1;
      heap.VisibleNodeMask = 1;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = static_cast<UINT64>(aligned_row) * height;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_UNKNOWN;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readback)))) {
        fail("create readback buffer failed");
        break;
      }
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                         IID_PPV_ARGS(&list)))) {
      fail("create command list failed");
      break;
    }

    auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      b.Transition.pResource = res;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = before;
      b.Transition.StateAfter = after;
      list->ResourceBarrier(1, &b);
    };
    auto copy_into = [&](ID3D12Resource* tex, ID3D12Resource* buffer) {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = tex;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = buffer;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = 0;
      src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      src.PlacedFootprint.Footprint.Width = width;
      src.PlacedFootprint.Footprint.Height = height;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = aligned_row;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };

    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || mapped == nullptr) {
      fail("map upload buffer failed");
      break;
    }

    /* 目标纹理先清为全透明。 */
    std::memset(mapped, 0, static_cast<size_t>(upload_size));
    barrier(dst_tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_into(dst_tex, upload);
    barrier(dst_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    /* 逐层：上传源像素 -> UAV -> 计算混合 -> 回到可再次上传的状态。 */
    const UINT groups_x = (width + 7u) / 8u;
    const UINT groups_y = (height + 7u) / 8u;
    /*
     * src_tex 跨层复用，其真实状态随迭代变化：纹理创建于 COMMON，首轮 barrier 之后
     * 每轮结束都被留在 COPY_DEST。StateBefore 必须跟着走，否则除首轮外的每次转换都在
     * 谎报前置状态——调试层直接报错，发行版则是未定义行为。
     */
    D3D12_RESOURCE_STATES src_before = D3D12_RESOURCE_STATE_COMMON;
    for (const Layer* layer : paint) {
      const uint8_t* pixels = layer->pixels.pixel_data().data();
      if (layer->has_pixels() && layer->pixels.empty()) {
        fail("layer pixel buffer inconsistent");
        break;
      }
      for (UINT y = 0; y < height; ++y) {
        std::memcpy(mapped + static_cast<size_t>(y) * aligned_row,
                    pixels + static_cast<size_t>(y) * row_bytes, row_bytes);
      }
      barrier(src_tex, src_before, D3D12_RESOURCE_STATE_COPY_DEST);
      copy_into(src_tex, upload);
      barrier(src_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      list->SetComputeRootSignature(root);
      list->SetPipelineState(pso);
      UINT constants[8] = {static_cast<UINT>(layer->blend), width, height, 0, 0, 0, 0, 0};
      float alpha = layer->opacity;
      if (alpha < 0.0f) alpha = 0.0f;
      if (alpha > 1.0f) alpha = 1.0f;
      std::memcpy(&constants[4], &alpha, sizeof(float));
      list->SetComputeRoot32BitConstants(0, 8, constants, 0);
      list->SetComputeRootUnorderedAccessView(1, dst_tex->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(2, src_tex->GetGPUVirtualAddress());
      list->Dispatch(groups_x, groups_y, 1);

      barrier(src_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      src_before = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    barrier(dst_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_into(readback, dst_tex);

    if (FAILED(list->Close())) {
      fail("close command list failed");
      break;
    }
    ID3D12CommandList* lists[1] = {list};
    queue->ExecuteCommandLists(1, lists);

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
      fail("create fence failed");
      break;
    }
    event_handle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle == nullptr || FAILED(queue->Signal(fence, 1)) ||
        FAILED(fence->SetEventOnCompletion(1, event_handle))) {
      fail("fence wait setup failed");
      break;
    }
    if (::WaitForSingleObject(event_handle, 10000) != WAIT_OBJECT_0) {
      fail("gpu wait timed out");
      break;
    }

    upload->Unmap(0, nullptr);
    mapped = nullptr;

    void* mapped_read = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped_read)) || mapped_read == nullptr) {
      fail("map readback buffer failed");
      break;
    }
    const uint8_t* read = static_cast<const uint8_t*>(mapped_read);
    const size_t out_row_bytes = static_cast<size_t>(width) * 4;
    const size_t out_stride = target.stride >= out_row_bytes ? target.stride : out_row_bytes;
    uint8_t* out_base = static_cast<uint8_t*>(target.pixels);
    for (UINT y = 0; y < height; ++y) {
      std::memcpy(out_base + static_cast<size_t>(y) * out_stride,
                  read + static_cast<size_t>(y) * aligned_row, out_row_bytes);
    }
    readback->Unmap(0, nullptr);
    ok = true;
  } while (false);

  if (event_handle != nullptr) ::CloseHandle(event_handle);
  release(sig_blob);
  release(sig_err);
  release(cs_blob);
  release(cs_err);
  release(readback);
  release(upload);
  release(dst_tex);
  release(src_tex);
  release(pso);
  release(root);
  release(list);
  release(allocator);
  release(queue);
  release(device);
  return ok;
}

bool D3d12RenderBackend::present(Document& doc, std::string* error) {
  (void)doc;
  if (error != nullptr) *error = "d3d12 backend window presentation is not implemented yet";
  return false;
}

bool D3d12RenderBackend::wait(Document& doc, std::string* error) {
  (void)doc;
  (void)error;
  /* 每次 render_document 内部已同步等待 GPU 完成。 */
  return true;
}

std::unique_ptr<RenderBackend> create_d3d12_backend(Engine* engine, std::string* error) {
  std::string why;
  if (!probe_d3d12(&why)) {
    if (error != nullptr) *error = "d3d12 unavailable: " + why;
    return nullptr;
  }
  return std::unique_ptr<RenderBackend>(new D3d12RenderBackend(engine));
}

}  // namespace compositor

#else /* !COMP_HAS_D3D12 */

namespace compositor {

bool probe_d3d12(std::string* why_not) {
  if (why_not != nullptr) *why_not = "built without COMP_HAS_D3D12";
  return false;
}

bool D3d12RenderBackend::usable() const { return false; }

BackendCaps D3d12RenderBackend::caps() const {
  BackendCaps c;
  c.compiled = false;
  c.note = "d3d12 backend not compiled into this build";
  return c;
}

bool D3d12RenderBackend::render_document(Document&, const comp_render_target&, std::string* error) {
  if (error != nullptr) *error = "d3d12 backend not compiled into this build";
  return false;
}

bool D3d12RenderBackend::present(Document&, std::string* error) {
  if (error != nullptr) *error = "d3d12 backend not compiled into this build";
  return false;
}

bool D3d12RenderBackend::wait(Document&, std::string*) { return false; }

std::unique_ptr<RenderBackend> create_d3d12_backend(Engine*, std::string* error) {
  if (error != nullptr) *error = "d3d12 backend not compiled into this build";
  return nullptr;
}

}  // namespace compositor

#endif /* COMP_HAS_D3D12 */
