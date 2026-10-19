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
 * 能力边界（S17 起按合成单元划分，见 render/gpu_plan.h）：整幅 RGBA8、无蒙版/
 * 效果/变换的像素层直接上传；裁剪/偏移、蒙版、效果、旋转翻转缩放、16 位源图层、
 * 隔离组与剪贴栈在 CPU 上烘焙成画布单元后照常交给 GPU 混合。仍整篇回落 CPU 的
 * 只有「调整层」（作用于已合成画布）与 HDR（RGBA16F）表面。ABI 层据此按文档
 * 回落，不会输出错误画面。
 *
 * S16 起支持窗口直呈：target.kind == COMP_SURFACE_HWND 时合成结果直接拷进
 * DXGI flip-model 交换链后备缓冲，comp_render_present 才真正 Present——不再整幅
 * 回读。离屏（CPU_BUFFER）路径保持原语义不变。
 *
 * S19 / R2 起原生单元走**持久逐层纹理**：缓存键 = (layer.id, layer.content_revision,
 * 画布尺寸, 格式)，命中时整块 CPU→GPU 上传都省掉；烘焙单元仍逐帧上传（其内容键
 * 需要子树修订集合 + 组结构签名，留给 v2）。活跃事务期间整段旁路缓存（事务是中间
 * 态，不采信也不写入）；画布尺寸变化 / 设备上下文重建时整体丢弃。缓存是派生数据，
 * 不进历史、事务快照或 .comp。
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
   * 仅供测试/诊断（S19 / R2，设计稿 §3.3）：累计「源纹理上传」次数。
   * 每创建一张逐层纹理或重传一次计 1（烘焙单元与事务期间的逐帧上传同样计数）。
   * 不进 C ABI、不进 BackendCaps——它不是能力，只是逐层缓存是否真的生效的证据。
   */
  unsigned long long texture_uploads() const { return texture_uploads_.load(); }

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
  /* 逐层纹理上传计数（派生数据，见 texture_uploads 的说明）。 */
  std::atomic<unsigned long long> texture_uploads_{0};
  bool verify_window_readback_ = false; /* 仅 set_window_readback_verification 可改 */
  bool window_readback_ready_ = false;  /* 上一次窗口渲染是否留下了可取的验证回读 */
};

/* 运行时探测：能否取得 D3D12 设备（含 WARP 软件适配器）。 */
bool probe_d3d12(std::string* why_not);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_D3D12_BACKEND_H */
