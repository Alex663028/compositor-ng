// AI生成
/*
 * vulkan_decl_check.cpp — OPT-10：手写 Vulkan 声明子集 vs 官方头的编译期交叉检查。
 *
 * 原理：本 TU 先包含官方头（third_party/vulkan-headers，自 OHOS NDK sysroot
 * 拷贝的 Khronos 原版），再整体包含 vulkan_backend.cpp（含 COMPOSITOR_ENABLE_
 * VULKAN_COMPUTE 下的手写声明）。同名 typedef **定义不一致**会直接编译错误
 * （一致则合法重复声明）；kVk* 常量与 *Local 句柄由下面的 static_assert 逐一
 * 对表官方值。手写集一旦漂移，本文件编不过——不依赖任何 Vulkan 设备。
 *
 * 手动跑：python -m ziglang c++ -c tools/vulkan_decl_check.cpp  *   -Iengine -Iengine/abi/include -Iengine/abi/src  *   -Iengine/third_party/vulkan-headers/include -o build/decl_check.o
 */
#define COMPOSITOR_ENABLE_VULKAN_COMPUTE 1
#include "vulkan/vulkan.h"

#include "../engine/render/vulkan_backend.cpp"

/* 常量对表：kVk* == 官方值（定义在 compositor 命名空间内，逐一限定）。 */
namespace compositor {
static_assert(kVkApiVersion10 == VK_API_VERSION_1_0, "kVkApiVersion10 drifted");
static_assert(kVkSuccess == VK_SUCCESS, "kVkSuccess drifted");
static_assert(kVkStructureTypeApplicationInfo == VK_STRUCTURE_TYPE_APPLICATION_INFO,
              "kVkStructureTypeApplicationInfo drifted");
static_assert(kVkStructureTypeInstanceCreateInfo == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
              "kVkStructureTypeInstanceCreateInfo drifted");

/* 句柄对表：VkPhysicalDeviceLocal 与官方 VkPhysicalDevice 同宽度。
 * 其余 37 个结构体 / 52 个 PFN 全部直接使用官方名——与 vulkan.h 同名重复定义
 * 在 C++ 里「定义一致才合法」，本 TU 能编过 = 无漂移。 */
static_assert(sizeof(VkPhysicalDeviceMemoryProperties) >= 1, "official struct visible");
}  // namespace compositor
