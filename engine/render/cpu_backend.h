// AI生成
#ifndef COMPOSITOR_RENDER_CPU_BACKEND_H
#define COMPOSITOR_RENDER_CPU_BACKEND_H

#include "render/rhi.h"

namespace compositor {

/* CPU 渲染后端：出屏/离屏均经内存缓冲，是跨后端像素一致性的基准实现。 */
class CpuRenderBackend final : public RenderBackend {
 public:
  explicit CpuRenderBackend(Engine* engine) : engine_(engine) {}

  comp_backend kind() const override { return COMP_BACKEND_CPU; }
  const char* name() const override { return "cpu"; }
  bool usable() const override { return true; }
  BackendCaps caps() const override;

  bool render_document(Document& doc, const comp_render_target& target,
                       std::string* error) override;
  bool present(Document& doc, std::string* error) override;
  bool wait(Document& doc, std::string* error) override;

 private:
  Engine* engine_ = nullptr;
};

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_CPU_BACKEND_H */
