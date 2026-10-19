// AI生成
#include "render/d3d12_backend.h"

#include <string>
#include <vector>

#include "core/comp_geometry.h"
#include "render/gpu_kernel.h"
#include "render/gpu_plan.h"

#if defined(COMP_HAS_D3D12)

#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h> /* IDXGISwapChain3::GetCurrentBackBufferIndex（S16 窗口直呈必需） */

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

/* ------------------------------------------------------------------
 * 动态入口：dxgi.dll 的 CreateDXGIFactory2。
 *
 * 与 d3d12 同一口径：不链接 dxgi.lib，工厂入口运行期解析；工厂/交换链对象都是
 * COM，vtable 调用不需要导入库。
 * ------------------------------------------------------------------ */

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);

struct DxgiApi {
  HMODULE module = nullptr;
  PFN_CreateDXGIFactory2 create_factory2 = nullptr;

  bool ok() const { return create_factory2 != nullptr; }
};

const DxgiApi& dxgi_api() {
  static DxgiApi api = [] {
    DxgiApi a;
    a.module = ::LoadLibraryA("dxgi.dll");
    if (a.module != nullptr) {
      a.create_factory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
          reinterpret_cast<void*>(::GetProcAddress(a.module, "CreateDXGIFactory2")));
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
 * GPU 能力判定与单元划分已迁到 render/gpu_plan.{h,cpp}（S17）。
 *
 * 旧版这里是 gpu_document_ready：只要有一层带蒙版 / 效果 / 组 / 剪贴 / 调整 /
 * 非全画布，就把整篇文档回落 CPU。现在由 build_gpu_plan 按合成单元分类：
 * 能直接上传的层走原生纹理，裁剪/蒙版/效果/变换/隔离组/剪贴栈在 CPU 上烘焙成
 * 画布单元后照常交给 GPU 混合；只有「调整层」和 HDR 表面继续整篇回落。
 */

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

  /* S16：窗口直呈。交换链只在 HWND 目标出现时创建，并按窗口/画布尺寸重建。 */
  IDXGIFactory2* factory = nullptr;
  IDXGISwapChain1* swapchain = nullptr;
  IDXGISwapChain3* swapchain3 = nullptr;
  /* flip-model 的后备缓冲必须**创建时取一次并长期持有**：每帧 GetBuffer(0)
     会拿到刚 Present 的那块缓冲，写它等于写正在扫描输出的表面（本机实测：
     第二帧 ExecuteCommandLists 后设备被移除，原因 0x887A002B ACCESS_DENIED）。
     正确做法是按 GetCurrentBackBufferIndex() 轮转使用固定持有的缓冲。 */
  ID3D12Resource* window_buffers[2] = {nullptr, nullptr};
  HWND swap_hwnd = nullptr;
  UINT swap_width = 0;
  UINT swap_height = 0;
  bool window_frame_ready = false; /* render_document(HWND) 成功、尚未 Present */

  ~GpuContext() {
    if (upload != nullptr && upload_mapped != nullptr) upload->Unmap(0, nullptr);
    upload_mapped = nullptr;
    for (ID3D12Resource*& buffer : window_buffers) release(buffer);
    release(swapchain3);
    release(swapchain);
    release(factory);
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

  void drop_swapchain() {
    for (ID3D12Resource*& buffer : window_buffers) release(buffer);
    release(swapchain3);
    release(swapchain);
    release(factory);
    swap_hwnd = nullptr;
    swap_width = 0;
    swap_height = 0;
    window_frame_ready = false;
  }

  /*
   * 为 HWND 建 DXGI flip-model 交换链。
   *
   * 关键点（本机实测过，见 report/S16）：
   *   - D3D12 的 CreateSwapChainForHwnd 第一个参数必须是 **direct 命令队列**，
   *     不是 ID3D12Device；
   *   - 后备缓冲格式必须与合成用的 dst_tex 一致（R8G8B8A8_UNORM），
   *     CopyResource 不做格式转换；RGBA8 的 flip-discard 在 Win10+ 可用；
   *   - 尺寸/窗口变化一律重建（本实现只在画布尺寸或目标窗口变化时重建，
   *     交互式缩放会走这条，代价是重建设备侧交换链而不是 ResizeBuffers，
   *     语义更简单且不会带进旧后备缓冲）。
   */
  bool ensure_swapchain(HWND hwnd, UINT w, UINT h, std::string* error) {
    if (device == nullptr || queue == nullptr) {
      if (error != nullptr) *error = "d3d12 backend: context is not initialized";
      return false;
    }
    if (hwnd == nullptr) {
      if (error != nullptr) *error = "d3d12 backend: window surface needs a non-null HWND";
      return false;
    }
    if (swapchain != nullptr && swap_hwnd == hwnd && swap_width == w && swap_height == h) {
      return true;
    }
    drop_swapchain();

    const DxgiApi& dxgi = dxgi_api();
    if (!dxgi.ok()) {
      if (error != nullptr) *error = "d3d12 backend: dxgi.dll CreateDXGIFactory2 unavailable";
      return false;
    }
    HRESULT hr = dxgi.create_factory2(0, __uuidof(IDXGIFactory2),
                                      reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
      if (error != nullptr) *error = hresult_text("create DXGI factory failed", hr);
      release(factory);
      return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swapchain);
    if (FAILED(hr) || swapchain == nullptr) {
      if (error != nullptr) {
        *error = hresult_text("create flip-model swap chain for hwnd failed (RGBA8)", hr);
      }
      release(swapchain);
      release(factory);
      return false;
    }
    /* 不让 DXGI 把 Alt+Enter 变成独占全屏（编辑器窗口不该被它接管）。 */
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    /* 需要 IDXGISwapChain3 拿「当前后备缓冲索引」：flip-model 下每帧必须轮转
       缓冲，固定往 GetBuffer(0) 写会与正在显示的表面冲突（见上面 window_buffers 注释）。 */
    HRESULT qi = swapchain->QueryInterface(__uuidof(IDXGISwapChain3),
                                           reinterpret_cast<void**>(&swapchain3));
    if (FAILED(qi) || swapchain3 == nullptr) {
      if (error != nullptr) *error = hresult_text("swap chain3 interface missing", qi);
      drop_swapchain();
      return false;
    }
    for (UINT i = 0; i < 2u; ++i) {
      hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&window_buffers[i]));
      if (FAILED(hr) || window_buffers[i] == nullptr) {
        if (error != nullptr) *error = hresult_text("acquire swap chain back buffer failed", hr);
        drop_swapchain();
        return false;
      }
    }
    swap_hwnd = hwnd;
    swap_width = w;
    swap_height = h;
    return true;
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
  /*
   * 窗口直呈（S16）：设备可用且 dxgi.dll 的 CreateDXGIFactory2 可解析。
   * 交换链本身要等调用方给出 HWND 才建，成立性由 render_document 如实报错；
   * 这里只回答「这条上屏路径在本机是否被支持」。
   */
  c.window_present = c.device_available && dxgi_api().ok();
  if (c.device_available && c.window_present) {
    c.note =
        "D3D12 offscreen compute + readback, and HWND present via DXGI flip-model swap "
        "chain (persistent device/PSO, HLSL compiled at runtime; layer units are uploaded "
        "natively or CPU-baked for crop/mask/effects/transforms/groups/clipping, adjustment "
        "layers and HDR surfaces still fall back to CPU)";
  } else if (c.device_available) {
    c.note =
        "D3D12 offscreen compute + readback (persistent device/PSO, HLSL compiled at "
        "runtime; dxgi.dll unavailable, so window present is not offered)";
  } else {
    c.note = "D3D12 device unavailable on this machine";
  }
  return c;
}

bool D3d12RenderBackend::can_render_document(const Document& doc, std::string* error) const {
  /* 只做分类与可行性判定，不分配烘焙缓冲（S17：粒度见 gpu_plan.h）。 */
  return build_gpu_plan(doc, nullptr, nullptr, error);
}

bool D3d12RenderBackend::render_document(Document& doc, const comp_render_target& target,
                                         std::string* error) {
  /*
   * 先把文档拆成合成单元：不满足 GPU 子集时连设备都不必初始化。
   * 单元 = 一张画布尺寸的预乘源 + 混合模式 + 不透明度；原生单元直接引用
   * 图层像素，其余（裁剪/蒙版/效果/变换/隔离组/剪贴栈）已由 CPU 烘焙。
   */
  std::vector<GpuUnit> units;
  GpuPlanStats plan_stats;
  if (!build_gpu_plan(doc, &units, &plan_stats, error)) return false;

  const DocumentState& state = doc.state();
  /*
   * 两种目标：
   *   - CPU_BUFFER：离屏合成 + 整幅回读（S13 起的既有路径，字节基准不变）；
   *   - HWND：合成后直接拷进交换链后备缓冲，等 comp_render_present 上屏（S16）。
   * 其他表面（Metal layer / Harmony window）仍明确拒绝，不做半张图。
   */
  const bool window_target = target.kind == COMP_SURFACE_HWND;
  if (!window_target && target.kind != COMP_SURFACE_CPU_BUFFER) {
    if (error != nullptr) {
      *error =
          "d3d12 backend supports COMP_SURFACE_CPU_BUFFER and COMP_SURFACE_HWND only";
    }
    return false;
  }
  if (window_target) {
    if (target.native_handle == nullptr) {
      if (error != nullptr) *error = "d3d12 backend: window surface needs native_handle = HWND";
      return false;
    }
    if (target.format != COMP_PIXEL_RGBA8) {
      if (error != nullptr) *error = "d3d12 backend: window surface only supports RGBA8";
      return false;
    }
  } else if (target.pixels == nullptr || target.format != COMP_PIXEL_RGBA8) {
    if (error != nullptr) *error = "d3d12 backend needs a non-null RGBA8 target buffer";
    return false;
  }
  if (target.size.width != state.width || target.size.height != state.height) {
    if (error != nullptr) *error = "render target size does not match canvas";
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  offloaded_.store(false);
  window_readback_ready_ = false;
  const bool verify_window_readback = verify_window_readback_;
  if (gpu_ == nullptr) gpu_.reset(new GpuContext());
  if (!gpu_->ensure(error)) {
    gpu_.reset();
    return false;
  }
  if (!gpu_->ensure_targets(static_cast<UINT>(state.width), static_cast<UINT>(state.height),
                            error)) {
    return false;
  }
  if (window_target) {
    if (!gpu_->ensure_swapchain(static_cast<HWND>(target.native_handle), gpu_->width, gpu_->height,
                                error)) {
      return false;
    }
  }

  const UINT width = gpu_->width;
  const UINT height = gpu_->height;
  const UINT aligned_row = gpu_->aligned_row;
  const UINT row_bytes = width * 4u;
  const UINT64 block_size = static_cast<UINT64>(aligned_row) * height;

  /*
   * 先把所有单元源铺进上传缓冲（1 块清屏 + 每单元 1 块），再录制拷贝命令。
   * 命令是执行时才读缓冲的，共用一块会让先录的拷贝读到最后一个单元的数据。
   */
  if (!gpu_->ensure_upload_blocks(static_cast<UINT>(units.size()) + 1u, error)) {
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

  /* 逐单元：上传源像素 -> UAV -> 计算混合 -> 回到可再次上传的状态。 */
  const UINT groups_x = (width + 7u) / 8u;
  const UINT groups_y = (height + 7u) / 8u;
  for (size_t i = 0; i < units.size(); ++i) {
    const GpuUnit& unit = units[i];
    const UINT64 offset = block_size * static_cast<UINT64>(i + 1);
    uint8_t* upload_target = gpu_->upload_mapped + static_cast<size_t>(offset);
    /*
     * 原生单元：图层像素就是整幅画布（gpu_plan 已保证 RGBA8 + 恒等几何）；
     * 烘焙单元：画布尺寸的预乘 RGBA8，逐行拷进上传块。
     */
    if (unit.is_native()) {
      const uint8_t* pixels = unit.native_layer->pixels.pixel_data().data();
      if (pixels == nullptr) {
        build_error = "d3d12 backend: layer pixel buffer inconsistent";
        built = false;
        break;
      }
      for (UINT y = 0; y < height; ++y) {
        std::memcpy(upload_target + static_cast<size_t>(y) * aligned_row,
                    pixels + static_cast<size_t>(y) * row_bytes, row_bytes);
      }
    } else {
      if (unit.baked.empty() || unit.baked.width != static_cast<int32_t>(width) ||
          unit.baked.height != static_cast<int32_t>(height) ||
          unit.baked.format != COMP_PIXEL_RGBA8) {
        build_error = "d3d12 backend: baked unit has an inconsistent surface";
        built = false;
        break;
      }
      for (UINT y = 0; y < height; ++y) {
        std::memcpy(upload_target + static_cast<size_t>(y) * aligned_row,
                    unit.baked.row(static_cast<size_t>(y)), row_bytes);
      }
    }
    barrier(gpu_->src_tex, gpu_->src_state, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_into(gpu_->src_tex, gpu_->upload, offset);
    barrier(gpu_->src_tex, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu_->src_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    list->SetComputeRootSignature(gpu_->root);
    list->SetPipelineState(gpu_->pso);
    UINT constants[8] = {static_cast<UINT>(unit.blend), width, height, 0, 0, 0, 0, 0};
    float alpha = unit.opacity;
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

  ID3D12Resource* window_buffer = nullptr; /* 借用 swapchain 持有的缓冲，不能 Release */
  if (built) {
    barrier(gpu_->dst_tex, gpu_->dst_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu_->dst_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (window_target) {
      /*
       * 窗口路径：合成结果 -> 交换链后备缓冲，**不回读**。
       *
       * flip 模型下后备缓冲必须按 GetCurrentBackBufferIndex() 轮转，且缓冲引用在
       * 交换链生命周期内常驻（见 window_buffers 上的注释）。每次 GetBuffer(0) 再
       * Release 会让第二帧写入「正在显示的那块」，DXGI 直接把设备判为移除
       * （0x887A0005 + 原因 0x887A002B）。缓冲状态由上一帧 Present 归一为 PRESENT，
       * 这里显式 PRESENT -> COPY_DEST，拷完再转回 PRESENT 交给 Present。
       */
      const UINT window_index = gpu_->swapchain3->GetCurrentBackBufferIndex();
      window_buffer = window_index < 2u ? gpu_->window_buffers[window_index] : nullptr;
      if (window_buffer == nullptr) {
        build_error = "d3d12 backend: swap chain back buffer index out of range";
        built = false;
      } else {
        barrier(window_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(window_buffer, gpu_->dst_tex);
        if (verify_window_readback) {
          /* 测试专用：把「即将 Present 的这一帧」再拷一份到回读缓冲对拍。
             只在 set_window_readback_verification(true) 时录制，默认零开销。 */
          barrier(window_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
          copy_out(window_buffer, gpu_->readback, 0);
          barrier(window_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_PRESENT);
        } else {
          barrier(window_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PRESENT);
        }
      }
    } else {
      /* 目标纹理 -> 回读缓冲：方向是纹理为源、缓冲为目标。 */
      copy_out(gpu_->dst_tex, gpu_->readback, 0);
    }
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

  if (window_target) {
    /* 到这里「上传 → dispatch → 拷后备缓冲」整链都成功了：等 comp_render_present 上屏。 */
    gpu_->window_frame_ready = true;
    window_readback_ready_ = verify_window_readback;
    offloaded_.store(true);
    return true;
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
  readback_frames_.fetch_add(1);
  /* 到这里整条设备链路（上传 → dispatch → 回读）都成功了，才算真正用了 GPU。 */
  offloaded_.store(true);
  return true;
}

bool D3d12RenderBackend::present(Document& doc, std::string* error) {
  (void)doc;
  std::lock_guard<std::mutex> lock(mutex_);
  if (gpu_ == nullptr || gpu_->swapchain == nullptr || !gpu_->window_frame_ready) {
    /* 离屏路径没有可上屏的帧：保持 RHI 的「离屏后端 present 为无操作」语义。 */
    return true;
  }
  const HRESULT hr = gpu_->swapchain->Present(0, 0);
  gpu_->window_frame_ready = false;
  if (FAILED(hr)) {
    if (error != nullptr) {
      std::string text = hresult_text("swap chain present failed", hr);
      /* 设备丢失时把驱动的真实原因一并带出来：只报 0x887A0005 无法判断是
         驱动 TDR、非法资源状态还是别的原因，读这一行的人会卡在这里。 */
      if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) &&
          gpu_->device != nullptr) {
        char extra[80];
        std::snprintf(extra, sizeof(extra), " (device removed reason 0x%08lx)",
                      static_cast<unsigned long>(gpu_->device->GetDeviceRemovedReason()));
        text += extra;
      }
      *error = text;
    }
    /* 设备没了：整块上下文丢弃，下一帧重新建（与渲染路径同一收口）。 */
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) gpu_.reset();
    return false;
  }
  /* DXGI_STATUS_OCCLUDED 也是成功（窗口被遮挡时 Present 会返回它）。 */
  presented_frames_.fetch_add(1);
  return true;
}

void D3d12RenderBackend::set_window_readback_verification(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  verify_window_readback_ = enabled;
}

bool D3d12RenderBackend::take_window_readback(std::vector<std::uint8_t>* rgba, int* out_width,
                                              int* out_height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!window_readback_ready_ || gpu_ == nullptr || gpu_->readback == nullptr ||
      gpu_->width == 0 || gpu_->height == 0) {
    return false;
  }
  const UINT width = gpu_->width;
  const UINT height = gpu_->height;
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  const UINT64 block_size = static_cast<UINT64>(gpu_->aligned_row) * height;

  void* mapped = nullptr;
  D3D12_RANGE read_range{0, static_cast<SIZE_T>(block_size)};
  if (FAILED(gpu_->readback->Map(0, &read_range, &mapped)) || mapped == nullptr) return false;
  const uint8_t* read = static_cast<const uint8_t*>(mapped);
  if (rgba != nullptr) {
    rgba->resize(row_bytes * height);
    for (UINT y = 0; y < height; ++y) {
      std::memcpy(rgba->data() + static_cast<size_t>(y) * row_bytes,
                  read + static_cast<size_t>(y) * gpu_->aligned_row, row_bytes);
    }
  }
  gpu_->readback->Unmap(0, nullptr);
  if (out_width != nullptr) *out_width = static_cast<int>(width);
  if (out_height != nullptr) *out_height = static_cast<int>(height);
  window_readback_ready_ = false;
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

void D3d12RenderBackend::set_window_readback_verification(bool enabled) {
  verify_window_readback_ = enabled;
}

bool D3d12RenderBackend::take_window_readback(std::vector<std::uint8_t>*, int*, int*) {
  return window_readback_ready_;
}

std::unique_ptr<RenderBackend> create_d3d12_backend(Engine*, std::string* error) {
  if (error != nullptr) *error = "d3d12 backend not compiled into this build";
  return nullptr;
}

}  // namespace compositor

#endif /* COMP_HAS_D3D12 */
