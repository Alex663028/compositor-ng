// AI生成
#include "render/cpu_backend.h"

#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "compositing/compositor.h"
#include "core/comp_geometry.h"

namespace compositor {

bool CpuRenderBackend::render_document(Document& doc, const comp_render_target& target,
                                       std::string* error) {
  if (target.kind != COMP_SURFACE_CPU_BUFFER) {
    if (error != nullptr) {
      *error = "cpu backend only supports COMP_SURFACE_CPU_BUFFER";
    }
    return false;
  }
  if (target.pixels == nullptr) {
    if (error != nullptr) *error = "target.pixels is null";
    return false;
  }
  if (target.format != COMP_PIXEL_RGBA8 && target.format != COMP_PIXEL_RGBA16F) {
    if (error != nullptr) {
      *error = "cpu backend only supports RGBA8 and RGBA16F render targets";
    }
    return false;
  }

  const DocumentState& state = doc.state();
  if (target.size.width != state.width || target.size.height != state.height) {
    if (error != nullptr) *error = "render target size does not match canvas";
    return false;
  }

  /*
   * MERGED：走画布级合成缓存。同一文档在事务外连续渲染（外壳每次 WM_PAINT、
   * 自检连续拍快照）只合成一次；缓存失效后由这里重算并回填。
   * 注意返回的是文档持有的只读视图，下面不再修改它，只做深度转换与拷贝。
   */
  const ImageBuffer* composite = nullptr;
  if (!composite_document_cached(doc, &composite, error)) return false;
  if (composite == nullptr) {
    if (error != nullptr) *error = "composite view is null";
    return false;
  }

  /*
   * C5：目标深度与合成表面深度不一致时显式转换一次。
   *
   *   - half 合成 -> 8 位目标：走 hdr_to_rgba8（上游 `image(_:)` 的显示口径，
   *     白点以上按白显示），不是按位强转；
   *   - 8 位合成 -> half 目标：走 rgba8_to_hdr（扩展 sRGB 编码、预乘），
   *     这样 8 位文档也能请求 half 表面而不必另写一条合成路径。
   *
   * 同深度时零拷贝直通：8 位文档的既有输出逐字节不变。
   */
  ImageBuffer converted;
  const ImageBuffer* source = composite;
  if (target.format == COMP_PIXEL_RGBA8 && composite->format != COMP_PIXEL_RGBA8) {
    if (!hdr_to_rgba8(*composite, &converted)) {
      if (error != nullptr) *error = "failed to build an 8-bit view of the HDR composite";
      return false;
    }
    source = &converted;
  } else if (target.format == COMP_PIXEL_RGBA16F && composite->format != COMP_PIXEL_RGBA16F) {
    ImageBuffer hdr;
    hdr.resize(composite->width, composite->height, COMP_PIXEL_RGBA16F);
    if (!rgba8_to_hdr(*composite, &hdr)) {
      if (error != nullptr) *error = "failed to build a half view of the 8-bit composite";
      return false;
    }
    converted = std::move(hdr);
    source = &converted;
  }

  const size_t row_bytes =
      static_cast<size_t>(state.width) * static_cast<size_t>(bytes_per_pixel(target.format));
  const size_t stride = target.stride >= row_bytes ? target.stride : row_bytes;
  uint8_t* base = static_cast<uint8_t*>(target.pixels);

  for (int32_t y = 0; y < state.height; ++y) {
    std::memcpy(base + static_cast<size_t>(y) * stride, source->row(y), row_bytes);
  }
  return true;
}

bool CpuRenderBackend::present(Document& doc, std::string* error) {
  (void)doc;
  (void)error;
  /* 离屏后端无上屏动作。 */
  return true;
}

bool CpuRenderBackend::wait(Document& doc, std::string* error) {
  (void)doc;
  (void)error;
  /* CPU 路径为同步执行，无需等待。 */
  return true;
}

BackendCaps CpuRenderBackend::caps() const {
  BackendCaps c;
  c.compiled = true;
  c.device_available = true;
  c.offscreen_compute = true;
  c.window_present = false;
  c.note = "CPU reference backend: always available, defines cross-backend pixel parity";
  return c;
}

std::unique_ptr<RenderBackend> create_cpu_backend(Engine* engine) {
  return std::unique_ptr<RenderBackend>(new CpuRenderBackend(engine));
}

namespace {

std::unique_ptr<RenderBackend> make_gpu(Engine* engine, comp_backend which,
                                        std::string* error) {
  if (which == COMP_BACKEND_D3D12) return create_d3d12_backend(engine, error);
  return create_vulkan_backend(engine, error);
}

}  // namespace

std::unique_ptr<RenderBackend> create_backend(Engine* engine, comp_backend requested,
                                              std::string* error, comp_backend* actual) {
  /*
   * 统一收口：任何一条路径都经过这里回写「实际生效的后端」，避免调用方拿到
   * 与实例不符的 kind。回落 CPU 时把原因写进 error（仅回落时写）。
   */
  auto finish = [actual](std::unique_ptr<RenderBackend> backend) {
    if (actual != nullptr && backend != nullptr) *actual = backend->kind();
    return backend;
  };
  auto fallback = [&](const std::string& why) {
    if (error != nullptr) *error = why + "; falling back to cpu";
    return finish(create_cpu_backend(engine));
  };

  switch (requested) {
    case COMP_BACKEND_CPU:
      return finish(create_cpu_backend(engine));

    case COMP_BACKEND_D3D12:
    case COMP_BACKEND_VULKAN: {
      std::string why;
      std::unique_ptr<RenderBackend> backend = make_gpu(engine, requested, &why);
      if (backend != nullptr) return finish(std::move(backend));
      if (why.empty()) why = "requested gpu backend unavailable";
      return fallback(why);
    }

    case COMP_BACKEND_AUTO: {
#if defined(_WIN32)
      const comp_backend order[2] = {COMP_BACKEND_D3D12, COMP_BACKEND_VULKAN};
#else
      const comp_backend order[2] = {COMP_BACKEND_VULKAN, COMP_BACKEND_D3D12};
#endif
      std::string why;
      for (comp_backend candidate : order) {
        std::string local;
        std::unique_ptr<RenderBackend> backend = make_gpu(engine, candidate, &local);
        if (backend != nullptr) return finish(std::move(backend));
        if (!local.empty()) why = local;
      }
      if (why.empty()) why = "auto: no gpu backend available on this platform";
      return fallback(why);
    }

    case COMP_BACKEND_METAL:
      return fallback("metal backend is not part of this build");

    default:
      return fallback("unknown backend enum");
  }
}

}  // namespace compositor

namespace compositor {

namespace {

/*
 * 诊断文本池：BackendCaps::note 是 const char*，调用方会在探测返回后继续持有，
 * 因此文本必须在进程生命周期内地址稳定。
 *
 * 原实现用单个 static std::string 承接，存在两个问题：
 *   1) 并发探测时多个线程同时改写同一个 std::string，是数据竞态（UB），
 *      且一旦发生重分配，先前返回的 caps.note 立刻变成悬垂指针；
 *   2) 同一线程第二次探测也会覆盖第一次的结果。
 * 这里改为带互斥量的文本池：按内容去重、只增不减，池内元素地址稳定
 * （std::deque 的 push_back 不会使已有元素的引用失效）。
 * 池保持有界的依据：note 文本来自 create_backend 的固定失败原因集合。
 */
const char* intern_note(const std::string& text) {
  static std::mutex mutex;
  static std::deque<std::string> pool;
  std::lock_guard<std::mutex> lock(mutex);
  for (const std::string& existing : pool) {
    if (existing == text) return existing.c_str();
  }
  pool.push_back(text);
  return pool.back().c_str();
}

}  // namespace

/*
 * 运行期能力探测：以「能否创建出该后端且未被回落」为唯一判据。
 * 探测失败时返回 compiled=false 并附带原因，绝不谎报可用；反之也不谎报不可用。
 * note 指向进程级文本池，地址稳定且可跨线程持有（见 intern_note）。
 */
BackendCaps probe_backend(comp_backend backend) {
  std::string error;
  comp_backend actual = backend;
  std::unique_ptr<RenderBackend> candidate = create_backend(nullptr, backend, &error, &actual);
  /*
   * 判据分两种情形：
   *   - 显式请求具体后端：actual 与请求一致才算可用，不一致说明已回落 CPU；
   *   - AUTO：没有可比对的「请求后端」——actual 是被择优选中的那个。原实现直接拿
   *     actual == backend 判定，AUTO 下恒不成立，于是「AUTO 已选中 D3D12」会被谎报成
   *     compiled=false 且附上空原因兜底文案（本机实测：error 为空 = 确实建出了 GPU
   *     后端，note 却是 "backend not available in this build"）。改为「选中了 GPU 后端」
   *     即算可用；回落 CPU 才报不可用，并带上回落原因。
   */
  const bool usable = backend == COMP_BACKEND_AUTO ? actual != COMP_BACKEND_CPU
                                                   : actual == backend;
  if (candidate != nullptr && usable) {
    return candidate->caps();
  }
  BackendCaps caps;
  caps.compiled = false;
  caps.device_available = false;
  caps.offscreen_compute = false;
  caps.window_present = false;
  caps.note = intern_note(error.empty() ? std::string("backend not available in this build") : error);
  return caps;
}

}  // namespace compositor
