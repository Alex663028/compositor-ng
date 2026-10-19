// AI生成
/*
 * rhi.h — 渲染后端抽象（Render Hardware Interface）。
 *
 * 目的：把「渲染调度」与「具体图形 API」解耦。引擎核只依赖本接口，
 * 各平台后端（D3D12 / Vulkan / CPU）实现同一组方法。
 *
 * 一致性契约：全部后端实现同一套逐像素语义（compositing/blend.h 的 W3C 公式 +
 * 预乘 RGBA8 布局）。CPU 后端是像素一致性基准，GPU 后端必须与其对齐；
 * GPU 计算内核的着色器源码见 render/gpu_kernel.h，对齐验证见 tests/test_rhi.cpp。
 */
#ifndef COMPOSITOR_RENDER_RHI_H
#define COMPOSITOR_RENDER_RHI_H

#include <memory>
#include <string>

#include "compositor/comp_core.h"
#include "domain/document.h"

namespace compositor {

class Engine;

/* 后端能力：让调用方在创建设备前判断「本构建是否含该后端」「本机是否有设备」。 */
struct BackendCaps {
  bool compiled = false;          /* 本构建是否编入该后端 */
  bool device_available = false;  /* 运行时探测：能否取得设备 */
  bool offscreen_compute = false; /* 支持离屏合成 + 回读（与 CPU 对齐的路径） */
  bool window_present = false;    /* 支持窗口表面直接上屏 */
  const char* note = "";          /* 人类可读补充说明（静态字符串，勿释放） */
};

class RenderBackend {
 public:
  virtual ~RenderBackend() = default;

  virtual comp_backend kind() const = 0;
  virtual const char* name() const = 0;

  /* 该后端实例当前是否可用于实际渲染。 */
  virtual bool usable() const = 0;

  virtual BackendCaps caps() const = 0;

  /*
   * 本后端能否按「当前文档内容」产出与 CPU 基准一致的像素。
   *
   * 这是 GPU 后端的能力闸门：CPU 后端恒为 true；D3D12 按**合成单元**判定
   * （S17，见 render/gpu_plan.h）——整幅 RGBA8 像素层直接上传，裁剪/蒙版/效果/
   * 变换/隔离组/剪贴栈在 CPU 上烘焙成画布单元后照常合成，只有调整层与 HDR
   * 表面整篇回落；Vulkan 的文档路径尚未接线，恒为 false。返回 false 时 error
   * 写明首个不满足的条件。
   *
   * ABI 层用它在每次渲染前判定「走 GPU 还是按文档回落 CPU」，因此 GPU 后端
   * 不需要（也不允许）为了不支持的文档静默输出错误画面。
   */
  virtual bool can_render_document(const Document& doc, std::string* error) const = 0;

  /* 把文档合成到 target 指定的表面。 */
  virtual bool render_document(Document& doc, const comp_render_target& target,
                               std::string* error) = 0;

  /*
   * 上一次 render_document 是否真的在设备上执行（上传 → dispatch → 回读全链路）。
   *
   * 为什么需要这个标志：像素相等**不能**证明 GPU 参与——ABI 层在 GPU 渲染失败时
   * 会回落 CPU 并照样输出正确像素。测试与诊断靠它区分「GPU 真的算了」与「悄悄
   * 回落了 CPU」。CPU 后端恒为 false；GPU 后端只在整条设备链路成功后置 true。
   */
  virtual bool last_render_offloaded() const { return false; }

  /* 提交上屏（窗口后端使用；离屏后端为无操作）。 */
  virtual bool present(Document& doc, std::string* error) = 0;

  /* 等待渲染工作完成。 */
  virtual bool wait(Document& doc, std::string* error) = 0;
};

/* 能力探测：不创建实例，直接查询某后端在本构建/本机的可用性。 */
BackendCaps probe_backend(comp_backend backend);

/* 各后端工厂：本机不可用（未编入或无设备）时返回 nullptr，并在 error 中说明。 */
std::unique_ptr<RenderBackend> create_cpu_backend(Engine* engine);
std::unique_ptr<RenderBackend> create_d3d12_backend(Engine* engine, std::string* error);
std::unique_ptr<RenderBackend> create_vulkan_backend(Engine* engine, std::string* error);

/*
 * 按期望后端创建实例：
 *   - AUTO：按平台择优（Windows → D3D12，其它 → Vulkan），都不可用则回落 CPU；
 *   - 显式 GPU 后端：不可用则回落 CPU，并在 error 中说明原因；
 *   - CPU：始终可用。
 * actual（可为 nullptr）回写实际生效的后端；error 只在发生回落或异常时写入。
 */
std::unique_ptr<RenderBackend> create_backend(Engine* engine, comp_backend requested,
                                              std::string* error, comp_backend* actual = nullptr);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_RHI_H */
