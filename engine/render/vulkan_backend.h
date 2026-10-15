// AI生成
#ifndef COMPOSITOR_RENDER_VULKAN_BACKEND_H
#define COMPOSITOR_RENDER_VULKAN_BACKEND_H

#include "render/rhi.h"

namespace compositor {

/*
 * Vulkan 渲染后端（HarmonyOS / 通用）。
 *
 * 本轮完成「接入同一 RHI」所需的部分：
 *   - 运行期动态加载 Vulkan loader（vulkan-1.dll / libvulkan.so.1，含 OHOS 的 libvulkan.so.1），
 *     不依赖任何链接期导入库与 Khronos 头（本仓自带最小声明子集，见 .cpp 顶部说明）；
 *   - 实例创建与物理设备枚举探测，用于 probe_backend / AUTO 择优；
 *   - 计算管线所需资源（image2D 目标/源 + push constant）与 gpu_kernel 的 GLSL 一一对应。
 *
 * 尚未落地的部分（显式返回 UNSUPPORTED，绝不静默成功）：
 *   计算管线需要 SPIR-V 字节码，而 GLSL -> SPIR-V 需在构建期由 glslangValidator 生成。
 *   本机无 glslang，故 render_document 明确报 UNSUPPORTED；接入 glslang 或预编译 blob
 *   后即可打开（管线创建代码已就位）。
 */
class VulkanRenderBackend final : public RenderBackend {
 public:
  explicit VulkanRenderBackend(Engine* engine) : engine_(engine) {}

  comp_backend kind() const override { return COMP_BACKEND_VULKAN; }
  const char* name() const override { return "vulkan"; }
  bool usable() const override;
  BackendCaps caps() const override;

  bool render_document(Document& doc, const comp_render_target& target,
                       std::string* error) override;
  bool present(Document& doc, std::string* error) override;
  bool wait(Document& doc, std::string* error) override;

 private:
  Engine* engine_ = nullptr;
};

/* 运行时探测：能否加载 Vulkan loader 并枚举到物理设备。 */
bool probe_vulkan(std::string* why_not);

/*
 * SPIR-V 字模块状态（构建期由 scripts/build_spirv.sh 生成，见 render/spirv/）：
 *   vulkan_spirv_embedded()  —— 本构建是否把字模块编入二进制？
 *                              只有 opt-in 构建（COMPOSITOR_ENABLE_VULKAN_COMPUTE）且字模块
 *                              就位时才为 true，其他情况为 false（不虚构能力）。
 *   vulkan_spirv_byte_count() —— 字模块字节数（0 表示尚未生成），用于诊断与报告。
 */
bool vulkan_spirv_embedded();
size_t vulkan_spirv_byte_count();

/*
 * Vulkan 计算内核的离屏混合入口（设备侧全链路）：
 *   dst = blend(dst, src, blend_mode, src_alpha)
 * 逐像素语义与 CPU 基准一致（同一套 W3C 公式 + 预乘 RGBA8），使用 SPIR-V 字模块、
 * 两个 rgba8 storage image 描述符与推送常量；布局常量由 GLSL 抽取而来
 * （render/spirv/vulkan_compute_bindings.h），保证宿主与着色器不错配。
 *
 * src_rgba8 / dst_rgba8 均为预乘 RGBA8、按行紧密排布，长度 = width * height * 4。
 * blend_mode 取 comp_blend_mode（0..23）；src_alpha 为该层有效不透明度。
 *
 * 默认构建（未定义 COMPOSITOR_ENABLE_VULKAN_COMPUTE）不编入设备侧实现：
 * 返回 false 并在 error 说明，绝不静默成功。
 */
bool vulkan_compute_blend(const uint8_t* src_rgba8, int32_t width, int32_t height,
                          uint32_t blend_mode, float src_alpha, uint8_t* dst_rgba8,
                          std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_VULKAN_BACKEND_H */
