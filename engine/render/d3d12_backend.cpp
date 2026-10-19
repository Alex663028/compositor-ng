// AI生成
#include "render/d3d12_backend.h"

#include <string>
#include <vector>

#include "core/comp_geometry.h"
#include "render/gpu_kernel.h"

#if defined(COMP_HAS_D3D12)

#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

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
typedef HRESULT(WINAPI* PFN_D3D12GetDebugInterface)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                        ID3DBlob**);

struct D3D12Api {
  HMODULE core = nullptr;
  HMODULE compiler = nullptr;
  PFN_CreateDevice create_device = nullptr;
  PFN_SerializeVersionedRootSignature serialize_root = nullptr;
  PFN_D3D12GetDebugInterface get_debug_interface = nullptr;
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
      a.get_debug_interface = reinterpret_cast<PFN_D3D12GetDebugInterface>(
          reinterpret_cast<void*>(::GetProcAddress(a.core, "D3D12GetDebugInterface")));
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

std::string hresult_text(const char* what, HRESULT hr) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "d3d12 backend: %s (hresult 0x%08lx)", what,
                static_cast<unsigned long>(hr));
  return std::string(buffer);
}

/*
 * GPU 能力闸门：把「这个文档能不能交给 D3D12 的逐层混合」说清楚。
 *
 * 目前的着色器只做「整张画布、RGBA8、逐层 blend」；组语义、蒙版、剪贴、
 * 调整/效果、变换（缩放/旋转/翻转/裁剪）都还在 CPU 的 FilterGraph 里。
 * 任何一项不满足都必须整篇回落 CPU——绝不能只画一部分层就交出画面。
 *
 * paint 为 nullptr 时只判定不收集。
 */
bool gpu_document_ready(const Document& doc, std::vector<const Layer*>* paint,
                        std::string* why) {
  const DocumentState& state = doc.state();
  if (state.width <= 0 || state.height <= 0) {
    if (why != nullptr) *why = "gpu backend: empty canvas";
    return false;
  }
  for (const Layer& layer : state.layers) {
    if (layer.parent != 0) {
      if (why != nullptr) *why = "gpu backend: nested/group layers are not supported yet";
      return false;
    }
    if (!layer.visible) continue; /* 不可见图层不参与合成（CPU 同口径） */
    if (layer.is_group) {
      if (why != nullptr) *why = "gpu backend: layer groups are not supported yet";
      return false;
    }
    if (layer.clipping_source != 0) {
      if (why != nullptr) *why = "gpu backend: clipping masks are not supported yet";
      return false;
    }
    if (!layer.mask.empty()) {
      if (why != nullptr) *why = "gpu backend: layer masks are not baked yet";
      return false;
    }
    if (!layer.adjustments.empty()) {
      if (why != nullptr) *why = "gpu backend: adjustments are not evaluated yet";
      return false;
    }
    if (!layer.effects.empty()) {
      if (why != nullptr) *why = "gpu backend: effects are not evaluated yet";
      return false;
    }
    if (layer.rotation != 0.0f || layer.flip_x || layer.flip_y) {
      if (why != nullptr) *why = "gpu backend: layer transforms are not supported yet";
      return false;
    }
    if (!layer.has_pixels()) continue; /* 无像素 = 不贡献画面（CPU 同口径） */
    if (layer.pixels.width != state.width || layer.pixels.height != state.height ||
        layer.pixels.format != COMP_PIXEL_RGBA8) {
      if (why != nullptr) {
        *why = "gpu backend: needs full-canvas RGBA8 layers (no crop/resample yet)";
      }
      return false;
    }
    /* 变换必须是「整幅画布、无偏移」：采样/裁剪/缩放都在 CPU 路径里。 */
    const comp_rect bounds = layer.effective_bounds();
    if (bounds.x != 0 || bounds.y != 0 || bounds.w != state.width ||
        bounds.h != state.height) {
      if (why != nullptr) *why = "gpu backend: layer bounds must equal the canvas";
      return false;
    }
    if (paint != nullptr) paint->push_back(&layer);
  }
  return true;
}

}  // namespace

/* ------------------------------------------------------------------
 * 设备上下文：跨渲染复用。
 *
 * 每次渲染重新 CreateDevice / 编译 HLSL / 建 PSO 是本机实测里单帧最贵的部分
 * （上一轮「每帧重建设备」的路径正是这样把 GPU 白白浪费掉的）；这里把与画布尺寸
 * 无关的对象全部留下，只有画布尺寸变化时才重建纹理与上传/回读缓冲。
 * ------------------------------------------------------------------ */
struct D3d12RenderBackend::GpuContext {
  ID3D12Device* device = nullptr;
  ID3D12InfoQueue* info_queue = nullptr; /* 仅 COMP_D3D12_DEBUG 时非空 */
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12RootSignature* root = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12DescriptorHeap* uav_heap = nullptr;
  D3D12_GPU_DESCRIPTOR_HANDLE uav_table{};
  ID3D12Fence* fence = nullptr;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;

  UINT width = 0;
  UINT height = 0;
  UINT aligned_row = 0;
  ID3D12Resource* src_tex = nullptr;
  ID3D12Resource* dst_tex = nullptr;
  ID3D12Resource* upload = nullptr;
  ID3D12Resource* readback = nullptr;
  uint8_t* upload_mapped = nullptr;
  /* 上传缓冲按「块」计：1 块 = aligned_row * height（一层像素 / 一次清屏）。 */
  UINT upload_blocks = 0;
  /* 跨渲染保持的资源状态：每帧的 barrier 必须从真实状态出发。 */
  D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_COMMON;

  ~GpuContext() {
    if (upload != nullptr && upload_mapped != nullptr) upload->Unmap(0, nullptr);
    upload_mapped = nullptr;
    release(info_queue);
    release(readback);
    release(upload);
    release(dst_tex);
    release(src_tex);
    release(fence);
    if (fence_event != nullptr) ::CloseHandle(fence_event);
    fence_event = nullptr;
    release(pso);
    release(root);
    release(uav_heap);
    release(list);
    release(allocator);
    release(queue);
    release(device);
  }

  bool ensure(std::string* error) {
    if (device != nullptr) return true;
    const D3D12Api& api = d3d12_api();
    if (!api.ok()) {
      if (error != nullptr) *error = "d3d12 backend: d3d12.dll / d3dcompiler_47.dll unavailable";
      return false;
    }

    /* 诊断用：COMP_D3D12_DEBUG=1 打开 D3D12 校验层（默认关闭，零开销）。 */
    const bool debug_layer = std::getenv("COMP_D3D12_DEBUG") != nullptr;
    if (debug_layer && api.get_debug_interface != nullptr) {
      ID3D12Debug* debug = nullptr;
      if (SUCCEEDED(api.get_debug_interface(__uuidof(ID3D12Debug),
                                            reinterpret_cast<void**>(&debug))) &&
          debug != nullptr) {
        debug->EnableDebugLayer();
        debug->Release();
      }
    }

    HRESULT hr = create_device(&device);
    if (FAILED(hr) || device == nullptr) {
      if (error != nullptr) *error = hresult_text("create device failed", hr);
      return false;
    }
    if (debug_layer) {
      device->QueryInterface(__uuidof(ID3D12InfoQueue),
                             reinterpret_cast<void**>(&info_queue));
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create queue failed", hr);
      return false;
    }
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create allocator failed", hr);
      return false;
    }

    /*
     * 根签名：b0 根常量（8 个 32 位）+ 一张 2 项 UAV 描述符表（u0=目标，u1=源）。
     *
     * 这里**不能**用根 UAV 描述符：D3D12 的根 SRV/UAV 只能描述 buffer，纹理必须
     * 走描述符表。早期版本用根 UAV 绑 2D 纹理，AMD 驱动在 CreateComputePipelineState
     * 阶段直接返回 E_INVALIDARG（PSO 构建失败 → 每帧回落 CPU），改成描述符表后
     * 才真正跑在 GPU 上。
     */
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 1;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC sig_desc{};
    sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    sig_desc.Desc_1_1.NumParameters = 2;
    sig_desc.Desc_1_1.pParameters = params;
    sig_desc.Desc_1_1.NumStaticSamplers = 0;
    sig_desc.Desc_1_1.pStaticSamplers = nullptr;
    sig_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* sig_err = nullptr;
    hr = api.serialize_root(&sig_desc, &sig_blob, &sig_err);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("serialize root signature failed", hr);
      release(sig_err);
      return false;
    }
    hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                     IID_PPV_ARGS(&root));
    release(sig_blob);
    release(sig_err);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create root signature failed", hr);
      return false;
    }

    /* 2 项 shader-visible UAV 描述符堆：u0 = 目标纹理，u1 = 源纹理。 */
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&uav_heap));
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create descriptor heap failed", hr);
      return false;
    }
    uav_table = uav_heap->GetGPUDescriptorHandleForHeapStart();

    const char* hlsl = gpu_kernel_hlsl();
    ID3DBlob* cs_blob = nullptr;
    ID3DBlob* cs_err = nullptr;
    hr = api.compile(hlsl, std::strlen(hlsl), "gpu_kernel.hlsl", nullptr, nullptr, "main", "cs_5_0",
                     0, 0, &cs_blob, &cs_err);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("compile HLSL failed", hr);
      release(cs_err);
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root;
    pso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength = cs_blob->GetBufferSize();
    pso_desc.NodeMask = 0;
    pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    release(cs_blob);
    release(cs_err);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create compute PSO failed", hr);
      return false;
    }

    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso,
                                   IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create command list failed", hr);
      return false;
    }
    /* 新建的 command list 处于 recording 状态：立刻关闭，渲染时再 Reset。 */
    list->Close();

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create fence failed", hr);
      return false;
    }
    fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr) {
      if (error != nullptr) *error = "d3d12 backend: create fence event failed";
      return false;
    }
    return true;
  }

  void dump_debug_messages(const char* when) {
    if (info_queue == nullptr) return;
    const UINT64 count = info_queue->GetNumStoredMessages();
    std::fprintf(stderr, "[d3d12-debug] %s: %llu message(s)\n", when,
                 static_cast<unsigned long long>(count));
    for (UINT64 i = 0; i < count && i < 12; ++i) {
      SIZE_T length = 0;
      if (FAILED(info_queue->GetMessage(i, nullptr, &length)) || length == 0) continue;
      std::vector<char> buffer(length);
      D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
      if (SUCCEEDED(info_queue->GetMessage(i, message, &length)) &&
          message->pDescription != nullptr) {
        std::fprintf(stderr, "[d3d12-debug]   %s\n", message->pDescription);
      }
    }
    info_queue->ClearStoredMessages();
  }

  void drop_targets() {
    if (upload != nullptr && upload_mapped != nullptr) upload->Unmap(0, nullptr);
    upload_mapped = nullptr;
    release(readback);
    release(upload);
    release(dst_tex);
    release(src_tex);
    width = height = aligned_row = 0;
    upload_blocks = 0;
    src_state = D3D12_RESOURCE_STATE_COMMON;
    dst_state = D3D12_RESOURCE_STATE_COMMON;
  }

  /*
   * 上传缓冲必须在提交前装下**所有**图层的像素：命令是录制后统一执行的，
   * 共用一块缓冲会让先录的 CopyTextureRegion 读到最后一层的数据（原实现的
   * 多图层画面错误即由此而来）。这里按「层数 + 1 块清屏」扩容。
   */
  bool ensure_upload_blocks(UINT blocks, std::string* error) {
    if (blocks == 0) blocks = 1;
    if (upload != nullptr && upload_blocks >= blocks) return true;
    if (upload != nullptr && upload_mapped != nullptr) upload->Unmap(0, nullptr);
    upload_mapped = nullptr;
    release(upload);
    upload_blocks = blocks;

    const UINT64 buffer_size =
        static_cast<UINT64>(aligned_row) * static_cast<UINT64>(height) * blocks;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = buffer_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload));
    if (FAILED(hr)) {
      upload_blocks = 0;
      if (error != nullptr) *error = hresult_text("create upload buffer failed", hr);
      return false;
    }
    hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_mapped));
    if (FAILED(hr) || upload_mapped == nullptr) {
      upload_blocks = 0;
      if (error != nullptr) *error = hresult_text("map upload buffer failed", hr);
      return false;
    }
    return true;
  }

  bool ensure_targets(UINT w, UINT h, std::string* error) {
    if (device == nullptr) {
      if (error != nullptr) *error = "d3d12 backend: context is not initialized";
      return false;
    }
    if (w == width && h == height && src_tex != nullptr && dst_tex != nullptr) return true;
    drop_targets();

    width = w;
    height = h;
    aligned_row = (w * 4u + 255u) & ~255u;

    auto make_texture = [&](ID3D12Resource** out) -> HRESULT {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      heap.CreationNodeMask = 1;
      heap.VisibleNodeMask = 1;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = w;
      desc.Height = h;
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
    HRESULT hr = make_texture(&src_tex);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create source texture failed", hr);
      drop_targets();
      return false;
    }
    hr = make_texture(&dst_tex);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create target texture failed", hr);
      drop_targets();
      return false;
    }

    /*
     * 纹理建好就把 UAV 写进描述符堆：u0 = 目标、u1 = 源。
     * 堆与表地址跨帧稳定；只有画布尺寸变化（纹理重建）时才需要重写。
     */
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
      uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      uav_desc.Texture2D.MipSlice = 0;
      uav_desc.Texture2D.PlaneSlice = 0;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = uav_heap->GetCPUDescriptorHandleForHeapStart();
      const UINT increment =
          device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      device->CreateUnorderedAccessView(dst_tex, nullptr, &uav_desc, cpu);
      cpu.ptr += increment;
      device->CreateUnorderedAccessView(src_tex, nullptr, &uav_desc, cpu);
    }

    const UINT64 buffer_size = static_cast<UINT64>(aligned_row) * h;
    auto make_buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                           ID3D12Resource** out) -> HRESULT {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = type;
      heap.CreationNodeMask = 1;
      heap.VisibleNodeMask = 1;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = buffer_size;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_UNKNOWN;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(out));
    };
    hr = make_buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create upload buffer failed", hr);
      drop_targets();
      return false;
    }
    hr = make_buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("create readback buffer failed", hr);
      drop_targets();
      return false;
    }
    hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_mapped));
    if (FAILED(hr) || upload_mapped == nullptr) {
      if (error != nullptr) *error = hresult_text("map upload buffer failed", hr);
      drop_targets();
      return false;
    }
    return true;
  }

  bool wait_for_gpu(std::string* error) {
    const UINT64 value = ++fence_value;
    HRESULT hr = queue->Signal(fence, value);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("gpu signal failed", hr);
      return false;
    }
    hr = fence->SetEventOnCompletion(value, fence_event);
    if (FAILED(hr)) {
      if (error != nullptr) *error = hresult_text("gpu fence wait setup failed", hr);
      return false;
    }
    if (::WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0) {
      if (error != nullptr) *error = "d3d12 backend: gpu wait timed out";
      return false;
    }
    return true;
  }
};

D3d12RenderBackend::D3d12RenderBackend(Engine* engine) : engine_(engine) {}

D3d12RenderBackend::~D3d12RenderBackend() = default;

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
  /*
   * 没有设备就不能宣称「能离屏计算」：AUTO 的闸门读的正是这一位，
   * 谎报会让 AUTO 选中一个立刻失败的后端。设备在 = 离屏计算可用（本后端只有
   * 这一条渲染路径，窗口上屏本来就没有）。
   */
  c.offscreen_compute = c.device_available;
  c.window_present = false;
  c.note = c.device_available
               ? "D3D12 offscreen compute + readback (persistent device/PSO, HLSL compiled at "
                 "runtime; falls back to CPU for documents outside the simple-stack subset)"
               : "D3D12 device unavailable on this machine";
  return c;
}

bool D3d12RenderBackend::can_render_document(const Document& doc, std::string* error) const {
  return gpu_document_ready(doc, nullptr, error);
}

bool D3d12RenderBackend::render_document(Document& doc, const comp_render_target& target,
                                         std::string* error) {
  /* 先把「文档是否属于 GPU 子集」判掉：不满足时连设备都不必初始化。 */
  std::vector<const Layer*> paint;
  if (!gpu_document_ready(doc, &paint, error)) return false;

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

  std::lock_guard<std::mutex> lock(mutex_);
  offloaded_.store(false);
  if (gpu_ == nullptr) gpu_.reset(new GpuContext());
  if (!gpu_->ensure(error)) {
    gpu_.reset();
    return false;
  }
  if (!gpu_->ensure_targets(static_cast<UINT>(state.width), static_cast<UINT>(state.height),
                            error)) {
    return false;
  }

  const UINT width = gpu_->width;
  const UINT height = gpu_->height;
  const UINT aligned_row = gpu_->aligned_row;
  const UINT row_bytes = width * 4u;
  const UINT64 block_size = static_cast<UINT64>(aligned_row) * height;

  /*
   * 先把所有图层像素铺进上传缓冲（1 块清屏 + 每层 1 块），再录制拷贝命令。
   * 命令是执行时才读缓冲的，共用一块会让先录的拷贝读到最后一层的数据。
   */
  if (!gpu_->ensure_upload_blocks(static_cast<UINT>(paint.size()) + 1u, error)) {
    return false;
  }

  HRESULT hr = gpu_->allocator->Reset();
  if (FAILED(hr)) {
    gpu_.reset();
    if (error != nullptr) *error = hresult_text("reset allocator failed", hr);
    return false;
  }
  hr = gpu_->list->Reset(gpu_->allocator, gpu_->pso);
  if (FAILED(hr)) {
    gpu_.reset();
    if (error != nullptr) *error = hresult_text("reset command list failed", hr);
    return false;
  }

  ID3D12GraphicsCommandList* list = gpu_->list;
  /* 描述符堆绑定必须在 Reset 之后重新设置（command list 重置会清掉）。 */
  ID3D12DescriptorHeap* heaps[1] = {gpu_->uav_heap};
  list->SetDescriptorHeaps(1, heaps);
  auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                     D3D12_RESOURCE_STATES after) {
    if (before == after) return; /* 同状态转换是退化屏障，显式跳过 */
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
  };
  auto copy_into = [&](ID3D12Resource* tex, ID3D12Resource* buffer, UINT64 offset) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = aligned_row;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  };
  /*
   * 反向拷贝（纹理 -> 回读缓冲）必须单独写：缓冲在 D3D12 里既可能是拷贝目标
   * 也可能是拷贝源，状态要求不同（目标需 COPY_DEST，源需 COPY_SOURCE/COMMON）。
   * 回读缓冲是 COPY_DEST，所以这里它必须是 dst；早先复用 copy_into 把它当 src，
   * Close 阶段就会报 E_INVALIDARG。
   */
  auto copy_out = [&](ID3D12Resource* tex, ID3D12Resource* buffer, UINT64 offset) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = offset;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = aligned_row;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  };

  std::string build_error;
  bool built = true;

  /* 目标纹理先清为全透明（第 0 块已预先清零）。 */
  std::memset(gpu_->upload_mapped, 0, static_cast<size_t>(block_size));
  barrier(gpu_->dst_tex, gpu_->dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
  copy_into(gpu_->dst_tex, gpu_->upload, 0);
  barrier(gpu_->dst_tex, D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu_->dst_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  /* 逐层：上传源像素 -> UAV -> 计算混合 -> 回到可再次上传的状态。 */
  const UINT groups_x = (width + 7u) / 8u;
  const UINT groups_y = (height + 7u) / 8u;
  for (size_t i = 0; i < paint.size(); ++i) {
    const Layer* layer = paint[i];
    const uint8_t* pixels = layer->pixels.pixel_data().data();
    if (pixels == nullptr) {
      build_error = "d3d12 backend: layer pixel buffer inconsistent";
      built = false;
      break;
    }
    const UINT64 offset = block_size * static_cast<UINT64>(i + 1);
    uint8_t* upload_target = gpu_->upload_mapped + static_cast<size_t>(offset);
    for (UINT y = 0; y < height; ++y) {
      std::memcpy(upload_target + static_cast<size_t>(y) * aligned_row,
                  pixels + static_cast<size_t>(y) * row_bytes, row_bytes);
    }
    barrier(gpu_->src_tex, gpu_->src_state, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_into(gpu_->src_tex, gpu_->upload, offset);
    barrier(gpu_->src_tex, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu_->src_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    list->SetComputeRootSignature(gpu_->root);
    list->SetPipelineState(gpu_->pso);
    UINT constants[8] = {static_cast<UINT>(layer->blend), width, height, 0, 0, 0, 0, 0};
    float alpha = layer->opacity;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    std::memcpy(&constants[4], &alpha, sizeof(float));
    list->SetComputeRoot32BitConstants(0, 8, constants, 0);
    /* 一张表两项：u0 = 目标纹理，u1 = 源纹理（描述符在 ensure_targets 里写好）。 */
    list->SetComputeRootDescriptorTable(1, gpu_->uav_table);
    list->Dispatch(groups_x, groups_y, 1);

    barrier(gpu_->src_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
    gpu_->src_state = D3D12_RESOURCE_STATE_COPY_DEST;
  }

  if (built) {
    barrier(gpu_->dst_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu_->dst_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    /* 目标纹理 -> 回读缓冲：方向是纹理为源、缓冲为目标。 */
    copy_out(gpu_->dst_tex, gpu_->readback, 0);
  }

  hr = list->Close();
  if (!built || FAILED(hr)) {
    if (FAILED(hr)) gpu_->dump_debug_messages("close command list failed");
    /* 命令流状态不再可信：整块上下文丢弃，下一次渲染从头建立。 */
    gpu_.reset();
    if (error != nullptr) {
      *error = !built ? build_error : hresult_text("close command list failed", hr);
    }
    return false;
  }

  ID3D12CommandList* lists[1] = {list};
  gpu_->queue->ExecuteCommandLists(1, lists);
  if (!gpu_->wait_for_gpu(error)) {
    gpu_.reset();
    return false;
  }

  void* mapped_read = nullptr;
  D3D12_RANGE read_range{0, static_cast<SIZE_T>(block_size)};
  hr = gpu_->readback->Map(0, &read_range, &mapped_read);
  if (FAILED(hr) || mapped_read == nullptr) {
    gpu_.reset();
    if (error != nullptr) *error = hresult_text("map readback buffer failed", hr);
    return false;
  }
  const uint8_t* read = static_cast<const uint8_t*>(mapped_read);
  const size_t out_row_bytes = static_cast<size_t>(width) * 4;
  const size_t out_stride = target.stride >= out_row_bytes ? target.stride : out_row_bytes;
  uint8_t* out_base = static_cast<uint8_t*>(target.pixels);
  for (UINT y = 0; y < height; ++y) {
    std::memcpy(out_base + static_cast<size_t>(y) * out_stride,
                read + static_cast<size_t>(y) * aligned_row, out_row_bytes);
  }
  gpu_->readback->Unmap(0, nullptr);
  /* 到这里整条设备链路（上传 → dispatch → 回读）都成功了，才算真正用了 GPU。 */
  offloaded_.store(true);
  return true;
}

bool D3d12RenderBackend::present(Document& doc, std::string* error) {
  (void)doc;
  (void)error;
  /*
   * 本后端是离屏后端（RHI 契约：离屏后端 present 为无操作）。
   * 窗口上屏走 COMP_SURFACE_HWND 是后续独立切片，caps().window_present 如实为 false。
   */
  return true;
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

/* 未编入后端时也给 GpuContext 一个完整定义：unique_ptr 成员析构需要它。 */
struct D3d12RenderBackend::GpuContext {};

D3d12RenderBackend::D3d12RenderBackend(Engine* engine) : engine_(engine) {}

D3d12RenderBackend::~D3d12RenderBackend() = default;

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

bool D3d12RenderBackend::can_render_document(const Document&, std::string* error) const {
  if (error != nullptr) *error = "d3d12 backend not compiled into this build";
  return false;
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
