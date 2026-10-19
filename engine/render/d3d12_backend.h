// AI生成
#ifndef COMPOSITOR_RENDER_D3D12_BACKEND_H
#define COMPOSITOR_RENDER_D3D12_BACKEND_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "render/rhi.h"

namespace compositor {

/*
 * D3D12 渲染后端（Windows）。
 *
 * 离屏路径：为每个可见图层建立一张源纹理，用 gpu_kernel.h 的 HLSL 计算着色器
 * 逐像素混合进目标纹理，最后回读到调用方缓冲——与 CPU 后端共用同一套像素语义。
 * 计算着色器由 d3dcompiler_47.dll 在运行期编译，D3D12 入口由 d3d12.dll 动态获取，
 * 因此不依赖 d3d12.lib / dxgi.lib 导入库。
 *
 * S13 起设备上下文跨渲染复用（device / queue / root signature / PSO / 纹理 /
 * 上传与回读缓冲只在画布尺寸变化时重建），不再每次渲染重新创建设备与管线；
 * 那正是此前「GPU 单帧比 CPU 还慢」的主因。
 *
 * 能力闸门：只接受 can_render_document 认可的简单文档（全画布 RGBA8、无组/
 * 蒙版/剪贴/调整/效果/变换）。ABI 层据此按文档回落 CPU，不会输出错误画面。
 *
 * S16 起支持窗口直呈：target.kind == COMP_SURFACE_HWND 时合成结果直接拷进
 * DXGI flip-model 交换链后备缓冲，comp_render_present 才真正 Present——不再整幅
 * 回读。离屏（CPU_BUFFER）路径保持原语义不变。
 */
class D3d12RenderBackend final : public RenderBackend {
 public:
  explicit D3d12RenderBackend(Engine* engine);
  ~D3d12RenderBackend() override;

  comp_backend kind() const override { return COMP_BACKEND_D3D12; }
  const char* name() const override { return "d3d12"; }
  bool usable() const override;
  BackendCaps caps() const override;

  bool can_render_document(const Document& doc, std::string* error) const override;

  bool render_document(Document& doc, const comp_render_target& target,
                       std::string* error) override;
  /* 仅当「上传 → GPU dispatch → （回读 | 拷进后备缓冲）」整链成功时为 true。 */
  bool last_render_offloaded() const override { return offloaded_.load(); }
  bool present(Document& doc, std::string* error) override;
  bool wait(Document& doc, std::string* error) override;

  /* ---------------- 内部证据（不进 C ABI，仅供测试/外壳基准使用） ---------------- */

  /* 成功提交到窗口表面的帧数（每次 comp_render_present 真正 Present 后 +1）。 */
  unsigned long long presented_frames() const { return presented_frames_.load(); }
  /* 走「纹理 -> 回读缓冲 -> CPU」离屏路径的帧数（窗口路径必须不增长）。 */
  unsigned long long readback_frames() const { return readback_frames_.load(); }

  /*
   * 仅供测试：打开后，窗口渲染会在后备缓冲交给 Present **之前**多拷一份到回读
   * 缓冲，可用 take_window_readback() 取回该帧像素与 CPU 基准对拍。
   * 默认关闭——它不是渲染路径的一部分，也不改变上屏语义。
   */
  void set_window_readback_verification(bool enabled);
  bool take_window_readback(std::vector<std::uint8_t>* rgba, int* out_width, int* out_height);

 private:
  struct GpuContext;

  Engine* engine_ = nullptr;
  std::unique_ptr<GpuContext> gpu_;
  mutable std::mutex mutex_;
  std::atomic<bool> offloaded_{false};
  std::atomic<unsigned long long> presented_frames_{0};
  std::atomic<unsigned long long> readback_frames_{0};
  bool verify_window_readback_ = false; /* 仅 set_window_readback_verification 可改 */
  bool window_readback_ready_ = false;  /* 上一次窗口渲染是否留下了可取的验证回读 */
};

/* 运行时探测：能否取得 D3D12 设备（含 WARP 软件适配器）。 */
bool probe_d3d12(std::string* why_not);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_D3D12_BACKEND_H */
