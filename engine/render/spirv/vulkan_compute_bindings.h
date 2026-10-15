// AI生成（由 scripts/gen_vulkan_bindings.py 从 GLSL 抽取，请勿手工修改）
/*
 * vulkan_compute_bindings.h — Vulkan 计算管线的描述符 / 推送常量布局常量。
 * 单一真源：engine/render/gpu_kernel.cpp 的 GLSL 字面量。
 * 重建：bash scripts/build_spirv.sh && python scripts/gen_vulkan_bindings.py
 */
#ifndef COMPOSITOR_RENDER_SPIRV_VULKAN_COMPUTE_BINDINGS_H
#define COMPOSITOR_RENDER_SPIRV_VULKAN_COMPUTE_BINDINGS_H

#include <cstdint>

namespace compositor {
namespace vulkan_compute {

inline constexpr uint32_t kDescriptorSet = 0;
inline constexpr uint32_t kBindingDst = 0; /* g_dst : image2D */
inline constexpr uint32_t kBindingSrc = 1; /* g_src : image2D */

inline constexpr uint32_t kLocalSizeX = 8;
inline constexpr uint32_t kLocalSizeY = 8;
inline constexpr uint32_t kLocalSizeZ = 1;

inline constexpr uint32_t kPushConstantSize = 20;
inline constexpr uint32_t kPushConstantOffset = 0;

inline constexpr uint32_t kSpirvByteCount = 19052;

}  // namespace vulkan_compute
}  // namespace compositor

#endif /* COMPOSITOR_RENDER_SPIRV_VULKAN_COMPUTE_BINDINGS_H */
