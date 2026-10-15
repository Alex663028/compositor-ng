// AI生成
#include "render/vulkan_backend.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#include "render/gpu_kernel.h"

/*
 * 说明：本文件自带 Vulkan 的**最小声明子集**，而非官方 vulkan.h。
 *
 * 原因：本机既无 Vulkan SDK（无 vulkan.h / vulkan-1.lib），外网下载亦不可达
 * （GitHub raw 被阻断，见 README §6.3）。因此这里只声明所需类型与入口，全部按
 * Vulkan 规范的 C ABI 布局书写；一旦引入官方头，本段声明应整体替换，调用点无需改动。
 *
 * 分层：
 *   - 探测层（始终编入）：动态加载 loader（vulkan-1.dll / libvulkan.so.1）、创建实例、
 *     枚举物理设备，供 probe_backend / AUTO 择优使用；
 *   - 计算管线层（opt-in，定义 COMPOSITOR_ENABLE_VULKAN_COMPUTE 才编入）：
 *     设备与队列 → SPIR-V 字模块 → 描述符集（两个 rgba8 storage image）→ 推送常量 →
 *     vkCmdDispatch → 回读。逐像素语义与 CPU 基准一致（同一套 W3C 公式 + 预乘 RGBA8）。
 *
 * 描述符 / 推送常量布局不在本文件里手写数字，而由 scripts/gen_vulkan_bindings.py 从
 * GLSL 抽取成 render/spirv/vulkan_compute_bindings.h（单一真源），避免宿主与着色器错配。
 *
 * 能力边界（不伪造）：本机无 Vulkan 设备，设备侧代码在本机无法运行验证；默认构建不编入
 * 设备侧代码，render_document 显式返回 UNSUPPORTED，AUTO 回落 CPU。
 */

#if defined(COMPOSITOR_ENABLE_VULKAN_COMPUTE)
#  define COMPOSITOR_VULKAN_COMPUTE 1
#else
#  define COMPOSITOR_VULKAN_COMPUTE 0
#endif

#if defined(__has_include)
#  if __has_include("render/spirv/vulkan_compute_bindings.h")
#    include "render/spirv/vulkan_compute_bindings.h"
#    define COMPOSITOR_HAS_VULKAN_BINDINGS 1
#  endif
#  if __has_include("render/spirv/gpu_kernel_spv.h")
#    include "render/spirv/gpu_kernel_spv.h"
#    define COMPOSITOR_HAS_SPIRV_BLOB 1
#  endif
#endif
#ifndef COMPOSITOR_HAS_VULKAN_BINDINGS
#  define COMPOSITOR_HAS_VULKAN_BINDINGS 0
#endif
#ifndef COMPOSITOR_HAS_SPIRV_BLOB
#  define COMPOSITOR_HAS_SPIRV_BLOB 0
#endif

#if COMPOSITOR_VULKAN_COMPUTE && COMPOSITOR_HAS_SPIRV_BLOB && COMPOSITOR_HAS_VULKAN_BINDINGS
#  define COMPOSITOR_VULKAN_COMPUTE_READY 1
#else
#  define COMPOSITOR_VULKAN_COMPUTE_READY 0
#endif

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace compositor {

namespace {

typedef void* VkInstance;
typedef void* VkPhysicalDevice;

const int32_t kVkStructureTypeApplicationInfo = 0;
const int32_t kVkStructureTypeInstanceCreateInfo = 1;
const uint32_t kVkSuccess = 0;
const uint32_t kVkApiVersion10 = 0x00400000u;

struct VkApplicationInfo {
  int32_t sType;
  const void* pNext;
  const char* pApplicationName;
  uint32_t applicationVersion;
  const char* pEngineName;
  uint32_t engineVersion;
  uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  const VkApplicationInfo* pApplicationInfo;
  uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};

typedef uint32_t (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef uint32_t (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void*);

struct VulkanApi {
  void* library = nullptr;
  PFN_vkCreateInstance create_instance = nullptr;
  PFN_vkEnumeratePhysicalDevices enumerate_devices = nullptr;
  PFN_vkDestroyInstance destroy_instance = nullptr;

  bool loader_ok() const { return library != nullptr; }
  bool ok() const {
    return library != nullptr && create_instance != nullptr && enumerate_devices != nullptr;
  }
};

void* open_library(const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(::LoadLibraryA(name));
#else
  return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* load_symbol(void* library, const char* name) {
  if (library == nullptr) return nullptr;
#if defined(_WIN32)
  return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
  return ::dlsym(library, name);
#endif
}

const VulkanApi& vulkan_api() {
  static VulkanApi api = [] {
    VulkanApi a;
#if defined(_WIN32)
    a.library = open_library("vulkan-1.dll");
#else
    a.library = open_library("libvulkan.so.1");
#endif
    a.create_instance = reinterpret_cast<PFN_vkCreateInstance>(load_symbol(a.library, "vkCreateInstance"));
    a.enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        load_symbol(a.library, "vkEnumeratePhysicalDevices"));
    a.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        load_symbol(a.library, "vkDestroyInstance"));
    return a;
  }();
  return api;
}

}  // namespace

bool probe_vulkan(std::string* why_not) {
  const VulkanApi& api = vulkan_api();
  if (!api.loader_ok()) {
    if (why_not != nullptr) {
#if defined(_WIN32)
      *why_not = "vulkan-1.dll not found";
#else
      *why_not = "libvulkan.so.1 not found";
#endif
    }
    return false;
  }
  if (!api.ok()) {
    if (why_not != nullptr) *why_not = "vulkan loader exports missing (vkCreateInstance)";
    return false;
  }

  VkApplicationInfo app{};
  app.sType = kVkStructureTypeApplicationInfo;
  app.pApplicationName = "compositor-ng";
  app.applicationVersion = 1;
  app.pEngineName = "compositor-ng";
  app.engineVersion = 1;
  app.apiVersion = kVkApiVersion10;

  VkInstanceCreateInfo info{};
  info.sType = kVkStructureTypeInstanceCreateInfo;
  info.pApplicationInfo = &app;

  VkInstance instance = nullptr;
  const uint32_t created = api.create_instance(&info, nullptr, &instance);
  if (created != kVkSuccess || instance == nullptr) {
    if (why_not != nullptr) *why_not = "vkCreateInstance failed";
    return false;
  }

  uint32_t count = 0;
  const uint32_t enumerated = api.enumerate_devices(instance, &count, nullptr);
  const bool has_device = enumerated == kVkSuccess && count > 0;
  if (api.destroy_instance != nullptr) api.destroy_instance(instance, nullptr);
  if (!has_device) {
    if (why_not != nullptr) *why_not = "no Vulkan physical device";
    return false;
  }
  return true;
}

/* --------------------------------------------------------- 计算管线（opt-in） */

#if COMPOSITOR_VULKAN_COMPUTE_READY

namespace vkc {

typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkDeviceMemory;
typedef void* VkBuffer;
typedef void* VkImage;
typedef void* VkImageView;
typedef void* VkShaderModule;
typedef void* VkPipelineLayout;
typedef void* VkPipeline;
typedef void* VkDescriptorSetLayout;
typedef void* VkDescriptorPool;
typedef void* VkDescriptorSet;
typedef void* VkCommandPool;
typedef void* VkCommandBuffer;
typedef void* VkFence;
typedef void* VkPhysicalDeviceLocal;

/* 结构体 sType 常量（Vulkan 1.0 规范值）。 */
const int32_t kTypeDeviceQueueCreateInfo = 2;
const int32_t kTypeDeviceCreateInfo = 3;
const int32_t kTypeSubmitInfo = 4;
const int32_t kTypeMemoryAllocateInfo = 5;
const int32_t kTypeFenceCreateInfo = 8;
const int32_t kTypeBufferCreateInfo = 12;
const int32_t kTypeImageCreateInfo = 14;
const int32_t kTypeImageViewCreateInfo = 15;
const int32_t kTypeShaderModuleCreateInfo = 16;
const int32_t kTypePipelineShaderStageCreateInfo = 18;
const int32_t kTypeComputePipelineCreateInfo = 29;
const int32_t kTypePipelineLayoutCreateInfo = 30;
const int32_t kTypeDescriptorSetLayoutCreateInfo = 32;
const int32_t kTypeDescriptorPoolCreateInfo = 33;
const int32_t kTypeDescriptorSetAllocateInfo = 34;
const int32_t kTypeWriteDescriptorSet = 35;
const int32_t kTypeCommandPoolCreateInfo = 39;
const int32_t kTypeCommandBufferAllocateInfo = 40;
const int32_t kTypeCommandBufferBeginInfo = 42;
const int32_t kTypeBufferImageCopy = 43;
const int32_t kTypeImageMemoryBarrier = 45;

/* 枚举常量。 */
const uint32_t kQueueComputeBit = 0x00000002u;
const int32_t kImageType2D = 1;
const int32_t kImageViewType2D = 1;
const int32_t kFormatR8G8B8A8Unorm = 37;
const int32_t kSampleCount1 = 1;
const int32_t kImageTilingOptimal = 0;
const int32_t kSharingModeExclusive = 0;
const int32_t kLayoutUndefined = 0;
const int32_t kLayoutGeneral = 1;
const int32_t kLayoutTransferSrcOptimal = 6;
const int32_t kLayoutTransferDstOptimal = 7;
const uint32_t kImageUsageTransferSrc = 0x00000001u;
const uint32_t kImageUsageTransferDst = 0x00000002u;
const uint32_t kImageUsageStorage = 0x00000008u; /* 原误写 0x20（实为 DEPTH_STENCIL_ATTACHMENT），导致 rgba8 无存储用法、计算写入被丢弃 */
const uint32_t kBufferUsageTransferSrc = 0x00000001u;
const uint32_t kBufferUsageTransferDst = 0x00000002u;
const uint32_t kMemoryPropertyDeviceLocal = 0x00000001u;
const uint32_t kMemoryPropertyHostVisible = 0x00000002u;
const uint32_t kMemoryPropertyHostCoherent = 0x00000004u;
const uint32_t kDescriptorTypeStorageImage = 3;
const uint32_t kShaderStageCompute = 0x00000020u;
const int32_t kPipelineBindPointCompute = 1;
const uint32_t kAccessShaderRead = 0x00000020u;
const uint32_t kAccessShaderWrite = 0x00000040u;
const uint32_t kAccessTransferRead = 0x00000800u;
const uint32_t kAccessTransferWrite = 0x00001000u;
const uint32_t kStageTopOfPipe = 0x00000001u;
const uint32_t kStageComputeShader = 0x00000800u;
const uint32_t kStageTransfer = 0x00001000u;
const uint32_t kCommandPoolTransient = 0x00000001u;
const uint32_t kQueueFamilyIgnored = 0xFFFFFFFFu;
const uint32_t kTrue = 1;

struct VkExtent3D {
  uint32_t width;
  uint32_t height;
  uint32_t depth;
};

struct VkQueueFamilyProperties {
  uint32_t queueFlags;
  uint32_t queueCount;
  uint32_t timestampValidBits;
  VkExtent3D minImageTransferGranularity;
};

struct VkMemoryType {
  uint32_t propertyFlags;
  uint32_t heapIndex;
};

struct VkMemoryHeap {
  VkDeviceSize size;
  uint32_t flags;
};

struct VkPhysicalDeviceMemoryProperties {
  uint32_t memoryTypeCount;
  VkMemoryType memoryTypes[32];
  uint32_t memoryHeapCount;
  VkMemoryHeap memoryHeaps[16];
};

struct VkDeviceQueueCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t queueFamilyIndex;
  uint32_t queueCount;
  const float* pQueuePriorities;
};

struct VkDeviceCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t queueCreateInfoCount;
  const VkDeviceQueueCreateInfo* pQueueCreateInfos;
  uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
  const void* pEnabledFeatures;
};

struct VkShaderModuleCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  size_t codeSize;
  const uint32_t* pCode;
};

struct VkDescriptorSetLayoutBinding {
  uint32_t binding;
  uint32_t descriptorType;
  uint32_t descriptorCount;
  uint32_t stageFlags;
  const void* pImmutableSamplers;
};

struct VkDescriptorSetLayoutCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t bindingCount;
  const VkDescriptorSetLayoutBinding* pBindings;
};

struct VkPushConstantRange {
  uint32_t stageFlags;
  uint32_t offset;
  uint32_t size;
};

struct VkPipelineLayoutCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t setLayoutCount;
  const VkDescriptorSetLayout* pSetLayouts;
  uint32_t pushConstantRangeCount;
  const VkPushConstantRange* pPushConstantRanges;
};

struct VkPipelineShaderStageCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t stage;
  VkShaderModule module;
  const char* pName;
  const void* pSpecializationInfo;
};

struct VkComputePipelineCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  VkPipelineShaderStageCreateInfo stage;
  VkPipelineLayout layout;
  VkPipeline basePipelineHandle;
  int32_t basePipelineIndex;
};

struct VkDescriptorPoolSize {
  uint32_t type;
  uint32_t descriptorCount;
};

struct VkDescriptorPoolCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t maxSets;
  uint32_t poolSizeCount;
  const VkDescriptorPoolSize* pPoolSizes;
};

struct VkDescriptorSetAllocateInfo {
  int32_t sType;
  const void* pNext;
  VkDescriptorPool descriptorPool;
  uint32_t descriptorSetCount;
  const VkDescriptorSetLayout* pSetLayouts;
};

struct VkDescriptorImageInfo {
  void* sampler;
  VkImageView imageView;
  int32_t imageLayout;
};

struct VkWriteDescriptorSet {
  int32_t sType;
  const void* pNext;
  VkDescriptorSet dstSet;
  uint32_t dstBinding;
  uint32_t dstArrayElement;
  uint32_t descriptorCount;
  uint32_t descriptorType;
  const VkDescriptorImageInfo* pImageInfo;
  const void* pBufferInfo;
  const void* pTexelBufferView;
};

struct VkComponentMapping {
  int32_t r;
  int32_t g;
  int32_t b;
  int32_t a;
};

struct VkImageSubresourceRange {
  uint32_t aspectMask;
  uint32_t baseMipLevel;
  uint32_t levelCount;
  uint32_t baseArrayLayer;
  uint32_t layerCount;
};

struct VkImageCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  int32_t imageType;
  int32_t format;
  VkExtent3D extent;
  uint32_t mipLevels;
  uint32_t arrayLayers;
  int32_t samples;
  int32_t tiling;
  uint32_t usage;
  int32_t sharingMode;
  uint32_t queueFamilyIndexCount;
  const uint32_t* pQueueFamilyIndices;
  int32_t initialLayout;
};

struct VkImageViewCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  VkImage image;
  int32_t viewType;
  int32_t format;
  VkComponentMapping components;
  VkImageSubresourceRange subresourceRange;
};

struct VkMemoryRequirements {
  VkDeviceSize size;
  VkDeviceSize alignment;
  uint32_t memoryTypeBits;
};

struct VkMemoryAllocateInfo {
  int32_t sType;
  const void* pNext;
  VkDeviceSize allocationSize;
  uint32_t memoryTypeIndex;
};

struct VkBufferCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  VkDeviceSize size;
  uint32_t usage;
  int32_t sharingMode;
  uint32_t queueFamilyIndexCount;
  const uint32_t* pQueueFamilyIndices;
};

struct VkImageSubresourceLayers {
  uint32_t aspectMask;
  uint32_t mipLevel;
  uint32_t baseArrayLayer;
  uint32_t layerCount;
};

struct VkOffset3D {
  int32_t x;
  int32_t y;
  int32_t z;
};

struct VkBufferImageCopy {
  VkDeviceSize bufferOffset;
  uint32_t bufferRowLength;
  uint32_t bufferImageHeight;
  VkImageSubresourceLayers imageSubresource;
  VkOffset3D imageOffset;
  VkExtent3D imageExtent;
};

struct VkImageMemoryBarrier {
  int32_t sType;
  const void* pNext;
  uint32_t srcAccessMask;
  uint32_t dstAccessMask;
  int32_t oldLayout;
  int32_t newLayout;
  uint32_t srcQueueFamilyIndex;
  uint32_t dstQueueFamilyIndex;
  VkImage image;
  VkImageSubresourceRange subresourceRange;
};

struct VkCommandPoolCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
  int32_t sType;
  const void* pNext;
  VkCommandPool commandPool;
  int32_t level;
  uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
  const void* pInheritanceInfo;
};

struct VkSubmitInfo {
  int32_t sType;
  const void* pNext;
  uint32_t waitSemaphoreCount;
  const void* pWaitSemaphores;
  const uint32_t* pWaitDstStageMask;
  uint32_t commandBufferCount;
  const VkCommandBuffer* pCommandBuffers;
  uint32_t signalSemaphoreCount;
  const void* pSignalSemaphores;
};

struct VkFenceCreateInfo {
  int32_t sType;
  const void* pNext;
  uint32_t flags;
};

/* 推送常量：与 GLSL 的 Params 逐字段一致（mode,width,height,pad,alpha）。 */
struct PushConstants {
  uint32_t mode;
  uint32_t width;
  uint32_t height;
  uint32_t pad;
  float alpha;
};

typedef void* (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef uint32_t (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDeviceLocal, uint32_t*,
                                                                VkQueueFamilyProperties*);
typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDeviceLocal,
                                                       VkPhysicalDeviceMemoryProperties*);
typedef uint32_t (*PFN_vkCreateDevice)(VkPhysicalDeviceLocal, const VkDeviceCreateInfo*,
                                      const void*, VkDevice*);
typedef void (*PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef uint32_t (*PFN_vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const void*,
                                            VkShaderModule*);
typedef void (*PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, const void*);
typedef uint32_t (*PFN_vkCreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*,
                                                   const void*, VkDescriptorSetLayout*);
typedef void (*PFN_vkDestroyDescriptorSetLayout)(VkDevice, VkDescriptorSetLayout, const void*);
typedef uint32_t (*PFN_vkCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*,
                                              const void*, VkPipelineLayout*);
typedef void (*PFN_vkDestroyPipelineLayout)(VkDevice, VkPipelineLayout, const void*);
typedef uint32_t (*PFN_vkCreateComputePipelines)(VkDevice, VkPipeline, uint32_t,
                                                const VkComputePipelineCreateInfo*, const void*,
                                                VkPipeline*);
typedef void (*PFN_vkDestroyPipeline)(VkDevice, VkPipeline, const void*);
typedef uint32_t (*PFN_vkCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*,
                                              const void*, VkDescriptorPool*);
typedef void (*PFN_vkDestroyDescriptorPool)(VkDevice, VkDescriptorPool, const void*);
typedef uint32_t (*PFN_vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*,
                                                VkDescriptorSet*);
typedef void (*PFN_vkUpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t,
                                          const void*);
typedef uint32_t (*PFN_vkCreateImage)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*);
typedef void (*PFN_vkDestroyImage)(VkDevice, VkImage, const void*);
typedef void (*PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*);
typedef uint32_t (*PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
typedef uint32_t (*PFN_vkCreateImageView)(VkDevice, const VkImageViewCreateInfo*, const void*,
                                         VkImageView*);
typedef void (*PFN_vkDestroyImageView)(VkDevice, VkImageView, const void*);
typedef uint32_t (*PFN_vkCreateBuffer)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*);
typedef void (*PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const void*);
typedef void (*PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*);
typedef uint32_t (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef uint32_t (*PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*,
                                        VkDeviceMemory*);
typedef void (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);
typedef uint32_t (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t,
                                   void**);
typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef uint32_t (*PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*,
                                           VkCommandPool*);
typedef void (*PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);
typedef uint32_t (*PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*,
                                                VkCommandBuffer*);
typedef uint32_t (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
typedef uint32_t (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef void (*PFN_vkCmdPipelineBarrier)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t,
                                        const void*, uint32_t, const void*, uint32_t,
                                        const VkImageMemoryBarrier*);
typedef void (*PFN_vkCmdCopyBufferToImage)(VkCommandBuffer, VkBuffer, VkImage, int32_t, uint32_t,
                                          const VkBufferImageCopy*);
typedef void (*PFN_vkCmdCopyImageToBuffer)(VkCommandBuffer, VkImage, int32_t, VkBuffer, uint32_t,
                                          const VkBufferImageCopy*);
typedef void (*PFN_vkCmdBindPipeline)(VkCommandBuffer, int32_t, VkPipeline);
typedef void (*PFN_vkCmdBindDescriptorSets)(VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t,
                                           uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
typedef void (*PFN_vkCmdPushConstants)(VkCommandBuffer, VkPipelineLayout, uint32_t, uint32_t,
                                      uint32_t, const void*);
typedef void (*PFN_vkCmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
typedef uint32_t (*PFN_vkCreateFence)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*);
typedef uint32_t (*PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence*, uint32_t, uint64_t);
typedef uint32_t (*PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);
typedef void (*PFN_vkDestroyFence)(VkDevice, VkFence, const void*);

/*
 * 设备侧计算内核：一次 init 建好实例/设备/管线/描述符集，之后每次 blend 走完整
 * upload → dispatch → barrier → readback。为清晰起见不复用缓冲（正确优先于吞吐）。
 */
class ComputeKernel {
 public:
  bool init(std::string* error);
  bool blend(const uint8_t* src_rgba8, int32_t width, int32_t height, uint32_t blend_mode,
             float src_alpha, uint8_t* dst_rgba8, std::string* error);
  void shutdown();
  bool ready() const { return pipeline_ != nullptr; }

 private:
  bool create_device(std::string* error);
  bool create_pipeline(std::string* error);
  uint32_t find_memory_type(uint32_t type_bits, uint32_t required, std::string* error);
  struct Resource {
    VkImage image = nullptr;
    VkDeviceMemory memory = nullptr;
    VkImageView view = nullptr;
  };
  bool create_image(int32_t width, int32_t height, uint32_t usage, Resource* out,
                    std::string* error);
  bool create_staging(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped,
                      std::string* error);
  void destroy_image(Resource* resource);
  void destroy_staging(VkBuffer buffer, VkDeviceMemory memory);

  void* library_ = nullptr;
  PFN_vkGetInstanceProcAddr get_proc_ = nullptr;

  VkInstance instance_ = nullptr;
  VkPhysicalDeviceLocal physical_ = nullptr;
  VkDevice device_ = nullptr;
  VkQueue queue_ = nullptr;
  uint32_t queue_family_ = 0;
  VkPhysicalDeviceMemoryProperties memory_properties_{};

  VkShaderModule shader_ = nullptr;
  VkDescriptorSetLayout set_layout_ = nullptr;
  VkPipelineLayout pipeline_layout_ = nullptr;
  VkPipeline pipeline_ = nullptr;
  VkDescriptorPool descriptor_pool_ = nullptr;
  VkDescriptorSet descriptor_set_ = nullptr;
  VkCommandPool command_pool_ = nullptr;
  VkFence fence_ = nullptr;

  PFN_vkDestroyDevice destroy_device_ = nullptr;
  PFN_vkGetDeviceQueue get_device_queue_ = nullptr;
  PFN_vkCreateShaderModule create_shader_module_ = nullptr;
  PFN_vkDestroyShaderModule destroy_shader_module_ = nullptr;
  PFN_vkCreateDescriptorSetLayout create_set_layout_ = nullptr;
  PFN_vkDestroyDescriptorSetLayout destroy_set_layout_ = nullptr;
  PFN_vkCreatePipelineLayout create_pipeline_layout_ = nullptr;
  PFN_vkDestroyPipelineLayout destroy_pipeline_layout_ = nullptr;
  PFN_vkCreateComputePipelines create_compute_pipelines_ = nullptr;
  PFN_vkDestroyPipeline destroy_pipeline_ = nullptr;
  PFN_vkCreateDescriptorPool create_descriptor_pool_ = nullptr;
  PFN_vkDestroyDescriptorPool destroy_descriptor_pool_ = nullptr;
  PFN_vkAllocateDescriptorSets allocate_descriptor_sets_ = nullptr;
  PFN_vkUpdateDescriptorSets update_descriptor_sets_ = nullptr;
  PFN_vkCreateImage create_image_ = nullptr;
  PFN_vkDestroyImage destroy_image_fn_ = nullptr;
  PFN_vkGetImageMemoryRequirements image_memory_requirements_ = nullptr;
  PFN_vkBindImageMemory bind_image_memory_ = nullptr;
  PFN_vkCreateImageView create_image_view_ = nullptr;
  PFN_vkDestroyImageView destroy_image_view_ = nullptr;
  PFN_vkCreateBuffer create_buffer_ = nullptr;
  PFN_vkDestroyBuffer destroy_buffer_ = nullptr;
  PFN_vkGetBufferMemoryRequirements buffer_memory_requirements_ = nullptr;
  PFN_vkBindBufferMemory bind_buffer_memory_ = nullptr;
  PFN_vkAllocateMemory allocate_memory_ = nullptr;
  PFN_vkFreeMemory free_memory_ = nullptr;
  PFN_vkMapMemory map_memory_ = nullptr;
  PFN_vkUnmapMemory unmap_memory_ = nullptr;
  PFN_vkCreateCommandPool create_command_pool_ = nullptr;
  PFN_vkDestroyCommandPool destroy_command_pool_ = nullptr;
  PFN_vkAllocateCommandBuffers allocate_command_buffers_ = nullptr;
  PFN_vkBeginCommandBuffer begin_command_buffer_ = nullptr;
  PFN_vkEndCommandBuffer end_command_buffer_ = nullptr;
  PFN_vkCmdPipelineBarrier cmd_pipeline_barrier_ = nullptr;
  PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image_ = nullptr;
  PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer_ = nullptr;
  PFN_vkCmdBindPipeline cmd_bind_pipeline_ = nullptr;
  PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets_ = nullptr;
  PFN_vkCmdPushConstants cmd_push_constants_ = nullptr;
  PFN_vkCmdDispatch cmd_dispatch_ = nullptr;
  PFN_vkQueueSubmit queue_submit_ = nullptr;
  PFN_vkCreateFence create_fence_ = nullptr;
  PFN_vkWaitForFences wait_for_fences_ = nullptr;
  PFN_vkResetFences reset_fences_ = nullptr;
  PFN_vkDestroyFence destroy_fence_ = nullptr;
};

template <typename T>
void bind_device_fn(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, const char* name,
                    T* out) {
  *out = reinterpret_cast<T>(get_proc(instance, name));
}

bool ComputeKernel::init(std::string* error) {
  const VulkanApi& api = vulkan_api();
  if (!api.ok()) {
    if (error != nullptr) *error = "vulkan loader exports missing";
    return false;
  }
  library_ = api.library;
  get_proc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(load_symbol(library_, "vkGetInstanceProcAddr"));
  if (get_proc_ == nullptr) {
    if (error != nullptr) *error = "vkGetInstanceProcAddr missing";
    return false;
  }

  VkApplicationInfo app{};
  app.sType = kVkStructureTypeApplicationInfo;
  app.pApplicationName = "compositor-ng";
  app.applicationVersion = 1;
  app.pEngineName = "compositor-ng";
  app.engineVersion = 1;
  app.apiVersion = kVkApiVersion10;
  VkInstanceCreateInfo create_info{};
  create_info.sType = kVkStructureTypeInstanceCreateInfo;
  create_info.pApplicationInfo = &app;
  if (api.create_instance(&create_info, nullptr, &instance_) != kVkSuccess || instance_ == nullptr) {
    if (error != nullptr) *error = "vkCreateInstance failed";
    return false;
  }

  uint32_t device_count = 0;
  if (api.enumerate_devices(instance_, &device_count, nullptr) != kVkSuccess || device_count == 0) {
    if (error != nullptr) *error = "no Vulkan physical device";
    return false;
  }
  if (api.enumerate_devices(instance_, &device_count, &physical_) != kVkSuccess) {
    if (error != nullptr) *error = "vkEnumeratePhysicalDevices failed";
    return false;
  }
  if (!create_device(error)) return false;
  if (!create_pipeline(error)) return false;
  if (error != nullptr) error->clear();
  return true;
}

bool ComputeKernel::create_device(std::string* error) {
  PFN_vkGetPhysicalDeviceQueueFamilyProperties families_fn = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties memory_fn = nullptr;
  PFN_vkCreateDevice create_device_fn = nullptr;
  bind_device_fn(get_proc_, instance_, "vkGetPhysicalDeviceQueueFamilyProperties", &families_fn);
  bind_device_fn(get_proc_, instance_, "vkGetPhysicalDeviceMemoryProperties", &memory_fn);
  bind_device_fn(get_proc_, instance_, "vkCreateDevice", &create_device_fn);
  if (families_fn == nullptr || memory_fn == nullptr || create_device_fn == nullptr) {
    if (error != nullptr) *error = "device-level entry points missing";
    return false;
  }

  uint32_t family_count = 0;
  families_fn(physical_, &family_count, nullptr);
  if (family_count == 0) {
    if (error != nullptr) *error = "no queue family";
    return false;
  }
  std::vector<VkQueueFamilyProperties> families(family_count);
  families_fn(physical_, &family_count, families.data());
  bool found = false;
  for (uint32_t i = 0; i < family_count; ++i) {
    if ((families[i].queueFlags & kQueueComputeBit) != 0 && families[i].queueCount > 0) {
      queue_family_ = i;
      found = true;
      break;
    }
  }
  if (!found) {
    if (error != nullptr) *error = "no compute-capable queue family";
    return false;
  }
  memory_fn(physical_, &memory_properties_);

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = kTypeDeviceQueueCreateInfo;
  queue_info.queueFamilyIndex = queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{};
  device_info.sType = kTypeDeviceCreateInfo;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (create_device_fn(physical_, &device_info, nullptr, &device_) != kVkSuccess || device_ == nullptr) {
    if (error != nullptr) *error = "vkCreateDevice failed";
    return false;
  }

  bind_device_fn(get_proc_, instance_, "vkDestroyDevice", &destroy_device_);
  bind_device_fn(get_proc_, instance_, "vkGetDeviceQueue", &get_device_queue_);
  bind_device_fn(get_proc_, instance_, "vkCreateShaderModule", &create_shader_module_);
  bind_device_fn(get_proc_, instance_, "vkDestroyShaderModule", &destroy_shader_module_);
  bind_device_fn(get_proc_, instance_, "vkCreateDescriptorSetLayout", &create_set_layout_);
  bind_device_fn(get_proc_, instance_, "vkDestroyDescriptorSetLayout", &destroy_set_layout_);
  bind_device_fn(get_proc_, instance_, "vkCreatePipelineLayout", &create_pipeline_layout_);
  bind_device_fn(get_proc_, instance_, "vkDestroyPipelineLayout", &destroy_pipeline_layout_);
  bind_device_fn(get_proc_, instance_, "vkCreateComputePipelines", &create_compute_pipelines_);
  bind_device_fn(get_proc_, instance_, "vkDestroyPipeline", &destroy_pipeline_);
  bind_device_fn(get_proc_, instance_, "vkCreateDescriptorPool", &create_descriptor_pool_);
  bind_device_fn(get_proc_, instance_, "vkDestroyDescriptorPool", &destroy_descriptor_pool_);
  bind_device_fn(get_proc_, instance_, "vkAllocateDescriptorSets", &allocate_descriptor_sets_);
  bind_device_fn(get_proc_, instance_, "vkUpdateDescriptorSets", &update_descriptor_sets_);
  bind_device_fn(get_proc_, instance_, "vkCreateImage", &create_image_);
  bind_device_fn(get_proc_, instance_, "vkDestroyImage", &destroy_image_fn_);
  bind_device_fn(get_proc_, instance_, "vkGetImageMemoryRequirements", &image_memory_requirements_);
  bind_device_fn(get_proc_, instance_, "vkBindImageMemory", &bind_image_memory_);
  bind_device_fn(get_proc_, instance_, "vkCreateImageView", &create_image_view_);
  bind_device_fn(get_proc_, instance_, "vkDestroyImageView", &destroy_image_view_);
  bind_device_fn(get_proc_, instance_, "vkCreateBuffer", &create_buffer_);
  bind_device_fn(get_proc_, instance_, "vkDestroyBuffer", &destroy_buffer_);
  bind_device_fn(get_proc_, instance_, "vkGetBufferMemoryRequirements", &buffer_memory_requirements_);
  bind_device_fn(get_proc_, instance_, "vkBindBufferMemory", &bind_buffer_memory_);
  bind_device_fn(get_proc_, instance_, "vkAllocateMemory", &allocate_memory_);
  bind_device_fn(get_proc_, instance_, "vkFreeMemory", &free_memory_);
  bind_device_fn(get_proc_, instance_, "vkMapMemory", &map_memory_);
  bind_device_fn(get_proc_, instance_, "vkUnmapMemory", &unmap_memory_);
  bind_device_fn(get_proc_, instance_, "vkCreateCommandPool", &create_command_pool_);
  bind_device_fn(get_proc_, instance_, "vkDestroyCommandPool", &destroy_command_pool_);
  bind_device_fn(get_proc_, instance_, "vkAllocateCommandBuffers", &allocate_command_buffers_);
  bind_device_fn(get_proc_, instance_, "vkBeginCommandBuffer", &begin_command_buffer_);
  bind_device_fn(get_proc_, instance_, "vkEndCommandBuffer", &end_command_buffer_);
  bind_device_fn(get_proc_, instance_, "vkCmdPipelineBarrier", &cmd_pipeline_barrier_);
  bind_device_fn(get_proc_, instance_, "vkCmdCopyBufferToImage", &cmd_copy_buffer_to_image_);
  bind_device_fn(get_proc_, instance_, "vkCmdCopyImageToBuffer", &cmd_copy_image_to_buffer_);
  bind_device_fn(get_proc_, instance_, "vkCmdBindPipeline", &cmd_bind_pipeline_);
  bind_device_fn(get_proc_, instance_, "vkCmdBindDescriptorSets", &cmd_bind_descriptor_sets_);
  bind_device_fn(get_proc_, instance_, "vkCmdPushConstants", &cmd_push_constants_);
  bind_device_fn(get_proc_, instance_, "vkCmdDispatch", &cmd_dispatch_);
  bind_device_fn(get_proc_, instance_, "vkQueueSubmit", &queue_submit_);
  bind_device_fn(get_proc_, instance_, "vkCreateFence", &create_fence_);
  bind_device_fn(get_proc_, instance_, "vkWaitForFences", &wait_for_fences_);
  bind_device_fn(get_proc_, instance_, "vkResetFences", &reset_fences_);
  bind_device_fn(get_proc_, instance_, "vkDestroyFence", &destroy_fence_);

  const bool complete = get_device_queue_ && create_shader_module_ && create_set_layout_ &&
                        create_pipeline_layout_ && create_compute_pipelines_ &&
                        create_command_pool_ && allocate_command_buffers_ && cmd_dispatch_ &&
                        queue_submit_ && create_fence_ && wait_for_fences_ && create_image_ &&
                        create_image_view_ && allocate_memory_ && map_memory_;
  if (!complete) {
    if (error != nullptr) *error = "some Vulkan device entry points are missing";
    return false;
  }
  get_device_queue_(device_, queue_family_, 0, &queue_);
  return true;
}

bool ComputeKernel::create_pipeline(std::string* error) {
  using namespace vulkan_compute;

  VkShaderModuleCreateInfo shader_info{};
  shader_info.sType = kTypeShaderModuleCreateInfo;
  shader_info.codeSize = kGpuKernelSpirvByteCount;
  shader_info.pCode = kGpuKernelSpirv;
  if (create_shader_module_(device_, &shader_info, nullptr, &shader_) != kVkSuccess ||
      shader_ == nullptr) {
    if (error != nullptr) *error = "vkCreateShaderModule failed (SPIR-V rejected)";
    return false;
  }

  /* 描述符集布局：两个 rgba8 storage image，绑定号取自 GLSL（vulkan_compute_bindings.h）。 */
  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = kBindingDst;
  bindings[0].descriptorType = kDescriptorTypeStorageImage;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = kShaderStageCompute;
  bindings[1].binding = kBindingSrc;
  bindings[1].descriptorType = kDescriptorTypeStorageImage;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = kShaderStageCompute;

  VkDescriptorSetLayoutCreateInfo set_info{};
  set_info.sType = kTypeDescriptorSetLayoutCreateInfo;
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  if (create_set_layout_(device_, &set_info, nullptr, &set_layout_) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateDescriptorSetLayout failed";
    return false;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = kShaderStageCompute;
  push_range.offset = kPushConstantOffset;
  push_range.size = kPushConstantSize;
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = kTypePipelineLayoutCreateInfo;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (create_pipeline_layout_(device_, &layout_info, nullptr, &pipeline_layout_) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreatePipelineLayout failed";
    return false;
  }

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = kTypePipelineShaderStageCreateInfo;
  stage.stage = kShaderStageCompute;
  stage.module = shader_;
  stage.pName = "main";
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = kTypeComputePipelineCreateInfo;
  pipeline_info.stage = stage;
  pipeline_info.layout = pipeline_layout_;
  /* 无基管线时必须为 -1：与 basePipelineHandle=null 搭配才合法（0 属非法参数）。 */
  pipeline_info.basePipelineIndex = -1;
  if (create_compute_pipelines_(device_, nullptr, 1, &pipeline_info, nullptr, &pipeline_) !=
          kVkSuccess ||
      pipeline_ == nullptr) {
    if (error != nullptr) *error = "vkCreateComputePipelines failed";
    return false;
  }

  VkDescriptorPoolSize pool_size{};
  pool_size.type = kDescriptorTypeStorageImage;
  pool_size.descriptorCount = 2;
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = kTypeDescriptorPoolCreateInfo;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (create_descriptor_pool_(device_, &pool_info, nullptr, &descriptor_pool_) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateDescriptorPool failed";
    return false;
  }
  VkDescriptorSetAllocateInfo alloc_info{};
  alloc_info.sType = kTypeDescriptorSetAllocateInfo;
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &set_layout_;
  if (allocate_descriptor_sets_(device_, &alloc_info, &descriptor_set_) != kVkSuccess) {
    if (error != nullptr) *error = "vkAllocateDescriptorSets failed";
    return false;
  }

  VkCommandPoolCreateInfo cmd_info{};
  cmd_info.sType = kTypeCommandPoolCreateInfo;
  cmd_info.flags = kCommandPoolTransient;
  cmd_info.queueFamilyIndex = queue_family_;
  if (create_command_pool_(device_, &cmd_info, nullptr, &command_pool_) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateCommandPool failed";
    return false;
  }
  VkFenceCreateInfo fence_info{};
  fence_info.sType = kTypeFenceCreateInfo;
  if (create_fence_(device_, &fence_info, nullptr, &fence_) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateFence failed";
    return false;
  }

  if (error != nullptr) error->clear();
  return true;
}

uint32_t ComputeKernel::find_memory_type(uint32_t type_bits, uint32_t required,
                                         std::string* error) {
  for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
    const uint32_t flags = memory_properties_.memoryTypes[i].propertyFlags;
    if ((type_bits & (1u << i)) != 0 && (flags & required) == required) return i;
  }
  if (error != nullptr) *error = "no suitable memory type";
  return 0xFFFFFFFFu;
}

bool ComputeKernel::create_image(int32_t width, int32_t height, uint32_t usage, Resource* out,
                                 std::string* error) {
  VkImageCreateInfo info{};
  info.sType = kTypeImageCreateInfo;
  info.imageType = kImageType2D;
  info.format = kFormatR8G8B8A8Unorm;
  info.extent.width = static_cast<uint32_t>(width);
  info.extent.height = static_cast<uint32_t>(height);
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = kSampleCount1;
  info.tiling = kImageTilingOptimal;
  info.usage = usage;
  info.sharingMode = kSharingModeExclusive;
  info.initialLayout = kLayoutUndefined;
  if (create_image_(device_, &info, nullptr, &out->image) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateImage failed";
    return false;
  }

  VkMemoryRequirements requirements{};
  image_memory_requirements_(device_, out->image, &requirements);
  const uint32_t type_index = find_memory_type(requirements.memoryTypeBits, kMemoryPropertyDeviceLocal, error);
  if (type_index == 0xFFFFFFFFu) return false;
  VkMemoryAllocateInfo alloc{};
  alloc.sType = kTypeMemoryAllocateInfo;
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = type_index;
  if (allocate_memory_(device_, &alloc, nullptr, &out->memory) != kVkSuccess) {
    if (error != nullptr) *error = "vkAllocateMemory(image) failed";
    return false;
  }
  if (bind_image_memory_(device_, out->image, out->memory, 0) != kVkSuccess) {
    if (error != nullptr) *error = "vkBindImageMemory failed";
    return false;
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = kTypeImageViewCreateInfo;
  view_info.image = out->image;
  view_info.viewType = kImageViewType2D;
  view_info.format = kFormatR8G8B8A8Unorm;
  view_info.subresourceRange.aspectMask = 0x1; /* COLOR */
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  if (create_image_view_(device_, &view_info, nullptr, &out->view) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateImageView failed";
    return false;
  }
  if (error != nullptr) error->clear();
  return true;
}

bool ComputeKernel::create_staging(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory,
                                   void** mapped, std::string* error) {
  VkBufferCreateInfo info{};
  info.sType = kTypeBufferCreateInfo;
  info.size = size;
  info.usage = kBufferUsageTransferSrc | kBufferUsageTransferDst;
  info.sharingMode = kSharingModeExclusive;
  if (create_buffer_(device_, &info, nullptr, buffer) != kVkSuccess) {
    if (error != nullptr) *error = "vkCreateBuffer(staging) failed";
    return false;
  }
  VkMemoryRequirements requirements{};
  buffer_memory_requirements_(device_, *buffer, &requirements);
  const uint32_t type_index = find_memory_type(
      requirements.memoryTypeBits, kMemoryPropertyHostVisible | kMemoryPropertyHostCoherent, error);
  if (type_index == 0xFFFFFFFFu) return false;
  VkMemoryAllocateInfo alloc{};
  alloc.sType = kTypeMemoryAllocateInfo;
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = type_index;
  if (allocate_memory_(device_, &alloc, nullptr, memory) != kVkSuccess) {
    if (error != nullptr) *error = "vkAllocateMemory(staging) failed";
    return false;
  }
  if (bind_buffer_memory_(device_, *buffer, *memory, 0) != kVkSuccess) {
    if (error != nullptr) *error = "vkBindBufferMemory failed";
    return false;
  }
  if (map_memory_(device_, *memory, 0, requirements.size, 0, mapped) != kVkSuccess) {
    if (error != nullptr) *error = "vkMapMemory failed";
    return false;
  }
  if (error != nullptr) error->clear();
  return true;
}

void ComputeKernel::destroy_image(Resource* resource) {
  if (resource->view != nullptr && destroy_image_view_) destroy_image_view_(device_, resource->view, nullptr);
  if (resource->image != nullptr && destroy_image_fn_) destroy_image_fn_(device_, resource->image, nullptr);
  if (resource->memory != nullptr && free_memory_) free_memory_(device_, resource->memory, nullptr);
  *resource = Resource{};
}

void ComputeKernel::destroy_staging(VkBuffer buffer, VkDeviceMemory memory) {
  if (buffer != nullptr && destroy_buffer_) destroy_buffer_(device_, buffer, nullptr);
  if (memory != nullptr && free_memory_) free_memory_(device_, memory, nullptr);
}

bool ComputeKernel::blend(const uint8_t* src_rgba8, int32_t width, int32_t height,
                          uint32_t blend_mode, float src_alpha, uint8_t* dst_rgba8,
                          std::string* error) {
  if (!ready()) {
    if (error != nullptr) *error = "compute pipeline not initialized";
    return false;
  }
  if (src_rgba8 == nullptr || dst_rgba8 == nullptr || width <= 0 || height <= 0) {
    if (error != nullptr) *error = "invalid image size or null buffer";
    return false;
  }
  if (blend_mode >= 24u) {
    if (error != nullptr) *error = "blend mode out of range (0..23)";
    return false;
  }

  const VkDeviceSize byte_size =
      static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
  const uint32_t usage = kImageUsageStorage | kImageUsageTransferSrc | kImageUsageTransferDst;

  Resource dst{};
  Resource src{};
  VkBuffer in_buffer = nullptr;
  VkDeviceMemory in_memory = nullptr;
  VkBuffer out_buffer = nullptr;
  VkDeviceMemory out_memory = nullptr;
  void* in_mapped = nullptr;
  void* out_mapped = nullptr;
  VkCommandBuffer command = nullptr;
  bool ok = false;

  do {
    if (!create_image(width, height, usage, &dst, error)) break;
    if (!create_image(width, height, usage, &src, error)) break;
    if (!create_staging(byte_size, &in_buffer, &in_memory, &in_mapped, error)) break;
    if (!create_staging(byte_size, &out_buffer, &out_memory, &out_mapped, error)) break;

    /* 上传：dst 先装载当前目标像素（着色器读改写 g_dst），src 装载源图层像素。 */
    std::memcpy(in_mapped, dst_rgba8, static_cast<size_t>(byte_size));
    std::memcpy(out_mapped, src_rgba8, static_cast<size_t>(byte_size));

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = kTypeCommandBufferAllocateInfo;
    alloc.commandPool = command_pool_;
    alloc.level = 0; /* PRIMARY */
    alloc.commandBufferCount = 1;
    if (allocate_command_buffers_(device_, &alloc, &command) != kVkSuccess) {
      if (error != nullptr) *error = "vkAllocateCommandBuffers failed";
      break;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = kTypeCommandBufferBeginInfo;
    if (begin_command_buffer_(command, &begin) != kVkSuccess) {
      if (error != nullptr) *error = "vkBeginCommandBuffer failed";
      break;
    }

    const VkImageSubresourceRange range{0x1u, 0, 1, 0, 1};
    VkImageMemoryBarrier to_transfer[2]{};
    for (int i = 0; i < 2; ++i) {
      to_transfer[i].sType = kTypeImageMemoryBarrier;
      to_transfer[i].srcAccessMask = 0;
      to_transfer[i].dstAccessMask = kAccessTransferWrite;
      to_transfer[i].oldLayout = kLayoutUndefined;
      to_transfer[i].newLayout = kLayoutTransferDstOptimal;
      to_transfer[i].srcQueueFamilyIndex = kQueueFamilyIgnored;
      to_transfer[i].dstQueueFamilyIndex = kQueueFamilyIgnored;
      to_transfer[i].image = (i == 0) ? dst.image : src.image;
      to_transfer[i].subresourceRange = range;
    }
    cmd_pipeline_barrier_(command, kStageTopOfPipe, kStageTransfer, 0, 0, nullptr, 0, nullptr, 2,
                          to_transfer);

    VkBufferImageCopy copies[2]{};
    for (int i = 0; i < 2; ++i) {
      copies[i].bufferOffset = 0;
      copies[i].imageSubresource.aspectMask = 0x1u;
      copies[i].imageSubresource.layerCount = 1;
      copies[i].imageSubresource.layerCount = 1;
      copies[i].imageExtent.width = static_cast<uint32_t>(width);
      copies[i].imageExtent.height = static_cast<uint32_t>(height);
      copies[i].imageExtent.depth = 1;
    }
    cmd_copy_buffer_to_image_(command, in_buffer, dst.image, kLayoutTransferDstOptimal, 1, &copies[0]);
    cmd_copy_buffer_to_image_(command, out_buffer, src.image, kLayoutTransferDstOptimal, 1, &copies[1]);

    VkImageMemoryBarrier to_general[2]{};
    for (int i = 0; i < 2; ++i) {
      to_general[i] = to_transfer[i];
      to_general[i].srcAccessMask = kAccessTransferWrite;
      to_general[i].dstAccessMask = kAccessShaderRead | kAccessShaderWrite;
      to_general[i].oldLayout = kLayoutTransferDstOptimal;
      to_general[i].newLayout = kLayoutGeneral;
      to_general[i].image = (i == 0) ? dst.image : src.image;
    }
    cmd_pipeline_barrier_(command, kStageTransfer, kStageComputeShader, 0, 0, nullptr, 0, nullptr, 2,
                          to_general);

    VkDescriptorImageInfo dst_info{};
    dst_info.imageView = dst.view;
    dst_info.imageLayout = kLayoutGeneral;
    VkDescriptorImageInfo src_info{};
    src_info.imageView = src.view;
    src_info.imageLayout = kLayoutGeneral;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = kTypeWriteDescriptorSet;
    writes[0].dstSet = descriptor_set_;
    writes[0].dstBinding = vulkan_compute::kBindingDst;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = kDescriptorTypeStorageImage;
    writes[0].pImageInfo = &dst_info;
    writes[1].sType = kTypeWriteDescriptorSet;
    writes[1].dstSet = descriptor_set_;
    writes[1].dstBinding = vulkan_compute::kBindingSrc;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = kDescriptorTypeStorageImage;
    writes[1].pImageInfo = &src_info;
    update_descriptor_sets_(device_, 2, writes, 0, nullptr);

    PushConstants push{};
    push.mode = blend_mode;
    push.width = static_cast<uint32_t>(width);
    push.height = static_cast<uint32_t>(height);
    push.pad = 0;
    push.alpha = src_alpha;

    cmd_bind_pipeline_(command, kPipelineBindPointCompute, pipeline_);
    cmd_bind_descriptor_sets_(command, kPipelineBindPointCompute, pipeline_layout_,
                              vulkan_compute::kDescriptorSet, 1, &descriptor_set_, 0, nullptr);
    cmd_push_constants_(command, pipeline_layout_, kShaderStageCompute,
                        vulkan_compute::kPushConstantOffset, vulkan_compute::kPushConstantSize,
                        &push);
    const uint32_t groups_x = (static_cast<uint32_t>(width) + vulkan_compute::kLocalSizeX - 1u) /
                              vulkan_compute::kLocalSizeX;
    const uint32_t groups_y = (static_cast<uint32_t>(height) + vulkan_compute::kLocalSizeY - 1u) /
                              vulkan_compute::kLocalSizeY;
    cmd_dispatch_(command, groups_x, groups_y, 1);

    VkImageMemoryBarrier to_readback{};
    to_readback.sType = kTypeImageMemoryBarrier;
    to_readback.srcAccessMask = kAccessShaderWrite;
    to_readback.dstAccessMask = kAccessTransferRead;
    to_readback.oldLayout = kLayoutGeneral;
    to_readback.newLayout = kLayoutTransferSrcOptimal;
    to_readback.srcQueueFamilyIndex = kQueueFamilyIgnored;
    to_readback.dstQueueFamilyIndex = kQueueFamilyIgnored;
    to_readback.image = dst.image;
    to_readback.subresourceRange = range;
    cmd_pipeline_barrier_(command, kStageComputeShader, kStageTransfer, 0, 0, nullptr, 0, nullptr, 1,
                          &to_readback);
    cmd_copy_image_to_buffer_(command, dst.image, kLayoutTransferSrcOptimal, out_buffer, 1,
                              &copies[0]);

    if (end_command_buffer_(command) != kVkSuccess) {
      if (error != nullptr) *error = "vkEndCommandBuffer failed";
      break;
    }

    VkSubmitInfo submit{};
    submit.sType = kTypeSubmitInfo;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (queue_submit_(queue_, 1, &submit, fence_) != kVkSuccess) {
      if (error != nullptr) *error = "vkQueueSubmit failed";
      break;
    }
    wait_for_fences_(device_, 1, &fence_, kTrue, 0xFFFFFFFFFFFFFFFFull);
    reset_fences_(device_, 1, &fence_);

    /* 回读：dst 结果写回调用方缓冲（预乘 RGBA8，与 CPU 口径一致）。 */
    unmap_memory_(device_, out_memory);
    void* readback = nullptr;
    if (map_memory_(device_, out_memory, 0, byte_size, 0, &readback) != kVkSuccess) {
      if (error != nullptr) *error = "vkMapMemory(readback) failed";
      break;
    }
    std::memcpy(dst_rgba8, readback, static_cast<size_t>(byte_size));
    unmap_memory_(device_, out_memory);

    if (error != nullptr) error->clear();
    ok = true;
  } while (false);

  if (in_memory != nullptr && in_mapped != nullptr) unmap_memory_(device_, in_memory);
  destroy_staging(in_buffer, in_memory);
  destroy_staging(out_buffer, out_memory);
  destroy_image(&dst);
  destroy_image(&src);
  return ok;
}

void ComputeKernel::shutdown() {
  if (device_ != nullptr) {
    if (destroy_pipeline_ && pipeline_ != nullptr) destroy_pipeline_(device_, pipeline_, nullptr);
    if (destroy_pipeline_layout_ && pipeline_layout_ != nullptr)
      destroy_pipeline_layout_(device_, pipeline_layout_, nullptr);
    if (destroy_set_layout_ && set_layout_ != nullptr)
      destroy_set_layout_(device_, set_layout_, nullptr);
    if (destroy_shader_module_ && shader_ != nullptr)
      destroy_shader_module_(device_, shader_, nullptr);
    if (destroy_descriptor_pool_ && descriptor_pool_ != nullptr)
      destroy_descriptor_pool_(device_, descriptor_pool_, nullptr);
    if (destroy_fence_ && fence_ != nullptr) destroy_fence_(device_, fence_, nullptr);
    if (destroy_command_pool_ && command_pool_ != nullptr)
      destroy_command_pool_(device_, command_pool_, nullptr);
    if (destroy_device_ != nullptr) destroy_device_(device_, nullptr);
  }
  if (instance_ != nullptr && vulkan_api().destroy_instance != nullptr) {
    vulkan_api().destroy_instance(instance_, nullptr);
  }
  pipeline_ = nullptr;
  device_ = nullptr;
  instance_ = nullptr;
}

}  // namespace vkc

#endif /* COMPOSITOR_VULKAN_COMPUTE_READY */

/* ------------------------------------------------------------ 公共接口 */

bool vulkan_spirv_embedded() {
#if COMPOSITOR_VULKAN_COMPUTE_READY
  return kGpuKernelSpirvWordCount > 0;
#else
  return false;
#endif
}

size_t vulkan_spirv_byte_count() {
#if COMPOSITOR_HAS_VULKAN_BINDINGS
  return static_cast<size_t>(vulkan_compute::kSpirvByteCount);
#else
  return 0;
#endif
}

bool vulkan_compute_blend(const uint8_t* src_rgba8, int32_t width, int32_t height,
                          uint32_t blend_mode, float src_alpha, uint8_t* dst_rgba8,
                          std::string* error) {
#if COMPOSITOR_VULKAN_COMPUTE_READY
  static vkc::ComputeKernel kernel;
  static bool initialized = false;
  static std::string init_error;
  if (!initialized) {
    initialized = true;
    if (!kernel.init(&init_error)) {
      if (error != nullptr) *error = "vulkan compute init failed: " + init_error;
      return false;
    }
  }
  return kernel.blend(src_rgba8, width, height, blend_mode, src_alpha, dst_rgba8, error);
#else
  (void)src_rgba8;
  (void)width;
  (void)height;
  (void)blend_mode;
  (void)src_alpha;
  (void)dst_rgba8;
  if (error != nullptr) {
    *error =
        "vulkan compute pipeline host code is not compiled into this build"
        " (define COMPOSITOR_ENABLE_VULKAN_COMPUTE); SPIR-V blob is present but unusable without it";
  }
  return false;
#endif
}

bool VulkanRenderBackend::usable() const { return probe_vulkan(nullptr); }

BackendCaps VulkanRenderBackend::caps() const {
  BackendCaps c;
  c.compiled = true;
  c.device_available = probe_vulkan(nullptr);
  c.offscreen_compute = false; /* RHI 文档路径尚未接线到计算内核，如实置 false */
  c.window_present = false;

  static const std::string note = [] {
    std::string text;
    if (!probe_vulkan(nullptr)) {
      text = "Vulkan loader or device unavailable on this machine";
    } else {
      text = "Vulkan device present; SPIR-V blob embedded (";
      text += std::to_string(vulkan_spirv_byte_count());
      text += " bytes)";
    }
#if COMPOSITOR_VULKAN_COMPUTE_READY
    text += "; compute kernel host code compiled (vulkan_compute_blend available)";
#else
    text += "; compute kernel host code is opt-in (COMPOSITOR_ENABLE_VULKAN_COMPUTE)";
#endif
    text += "; RHI document path returns UNSUPPORTED";
    return text;
  }();
  c.note = note.c_str();
  return c;
}

bool VulkanRenderBackend::render_document(Document& doc, const comp_render_target& target,
                                          std::string* error) {
  (void)doc;
  (void)target;
  if (error != nullptr) {
#if COMPOSITOR_VULKAN_COMPUTE_READY
    *error =
        "vulkan backend: compute kernel is compiled (see vulkan_compute_blend), but the RHI "
        "document compositing path is not wired to it yet; use the CPU backend for documents";
#else
    *error =
        "vulkan backend: SPIR-V blob is embedded, but the compute pipeline host code is opt-in and "
        "not compiled into this build (define COMPOSITOR_ENABLE_VULKAN_COMPUTE)";
#endif
  }
  return false;
}

bool VulkanRenderBackend::present(Document& doc, std::string* error) {
  (void)doc;
  if (error != nullptr) *error = "vulkan backend window presentation is not implemented yet";
  return false;
}

bool VulkanRenderBackend::wait(Document& doc, std::string* error) {
  (void)doc;
  (void)error;
  return true;
}

std::unique_ptr<RenderBackend> create_vulkan_backend(Engine* engine, std::string* error) {
  std::string why;
  if (!probe_vulkan(&why)) {
    if (error != nullptr) *error = "vulkan unavailable: " + why;
    return nullptr;
  }
  if (error != nullptr) {
#if COMPOSITOR_VULKAN_COMPUTE_READY
    *error = "vulkan device present; compute kernel host code compiled";
#else
    *error =
        "vulkan device present; compute pipeline host code is opt-in "
        "(COMPOSITOR_ENABLE_VULKAN_COMPUTE)";
#endif
  }
  return std::unique_ptr<RenderBackend>(new VulkanRenderBackend(engine));
}

}  // namespace compositor
