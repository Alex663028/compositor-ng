// AI生成
#ifndef COMPOSITOR_RENDER_D3D12_BACKEND_H
#define COMPOSITOR_RENDER_D3D12_BACKEND_H

#include "render/rhi.h"

namespace compositor {

/*
 * D3D12 渲染后端（Windows）。
 *
 * 离屏路径：为每个可见图层建立一张源纹理，用 gpu_kernel.h 的 HLSL 计算着色器
 * 逐像素混合进目标纹理，最后回读到调用方缓冲——与 CPU 后端共用同一套像素语义。
 * 计算着色器由 d3dcompiler_47.dll 在运行期编译，D3D12 入口由 d3d12.dll 动态获取，
 * 因此不依赖 d3d12.lib / dxgi.lib 导入库。
 */
class D3d12RenderBackend final : public RenderBackend {
 public:
  explicit D3d12RenderBackend(Engine* engine) : engine_(engine) {}

  comp_backend kind() const override { return COMP_BACKEND_D3D12; }
  const char* name() const override { return "d3d12"; }
  bool usable() const override;
  BackendCaps caps() const override;

  bool render_document(Document& doc, const comp_render_target& target,
                       std::string* error) override;
  bool present(Document& doc, std::string* error) override;
  bool wait(Document& doc, std::string* error) override;

 private:
  Engine* engine_ = nullptr;
};

/* 运行时探测：能否取得 D3D12 设备（含 WARP 软件适配器）。 */
bool probe_d3d12(std::string* why_not);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_D3D12_BACKEND_H */
