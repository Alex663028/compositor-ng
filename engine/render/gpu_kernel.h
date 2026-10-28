// AI生成
/*
 * gpu_kernel.h — GPU 合成内核的**唯一定义处**。
 *
 * 背景：D3D12 与 Vulkan 两条后端必须产出与 CPU 后端一致的像素。为了不出现
 * 「三个后端三套公式」，这里集中三件东西：
 *   1. 计算着色器源码（HLSL for D3D12 / GLSL for Vulkan），供后端在运行期创建管线；
 *   2. 着色器与 ABI 混合模式表的一致性校验（模式条数、顺序、上游规范名）；
 *   3. 着色器 main() 的 C++ 镜像（gpu_kernel_blend_pixel），使「GPU 语义 == CPU 语义」
 *      在没有 GPU 的机器上也能被单测锁定。
 *
 * 逐像素语义逐式镜像 compositing/composite_ops.cpp 的 blend_pixels：
 *   直通化 -> W3C 合成 -> 量化回预乘 RGBA8，四条分量与 CPU 逐字节一致。
 */
#ifndef COMPOSITOR_RENDER_GPU_KERNEL_H
#define COMPOSITOR_RENDER_GPU_KERNEL_H

#include <cstdint>
#include <string>

#include "compositor/comp_core.h"

namespace compositor {

/* D3D12 / cs_5_0 计算着色器源码（UTF-8，以 '\0' 结尾，静态存储）。 */
const char* gpu_kernel_hlsl();

/* Vulkan / GLSL 450 计算着色器源码（与 HLSL 语义一一对应）。 */
const char* gpu_kernel_glsl();

/* 着色器内混合模式分支数，应等于 comp_blend_mode 的取值个数（27）。 */
int gpu_kernel_blend_case_count();

/*
 * 校验两份着色器源码：非空、恰好 27 个 `case N:` 分支、编号 0..26 连续、
 * 且每个分支注释里的上游规范名与 blend_mode_manifest_name(N) 逐字相同。
 * 不一致时写入 error 并返回 false。
 */
bool gpu_kernel_validate(std::string* error);

/*
 * 着色器 main() 的 C++ 镜像：按同一套公式处理一个预乘 RGBA8 像素。
 * 仅用于测试把 GPU 内核语义钉在 CPU 后端上（见 tests/test_rhi.cpp）。
 *
 * 参数契约（与 CPU 标量入口 `compositor_blend_pixel_scalar` **不同**，勿混用）：
 *   layer_opacity 是**图层不透明度**，对应着色器的推送常量 g_alpha；
 *   函数内部按 `sa = src[3]/255` 再乘一次，得到有效 alpha（镜像 `s.a * g_alpha`）。
 *   CPU 标量入口的 `src_alpha` 则是**已含像素 alpha 的有效 alpha**。
 * 两侧语义等价，但入参口径不同；混用会让 alpha 被乘两次（结果被压暗）。
 *
 * A50：px/py 是**文档坐标**，仅 Dissolve 需要（镜像着色器 main() 里对 tid.xy 做同一
 * 套确定性哈希）。其余模式忽略；缺省 0,0 时 Dissolve 退化为「整幅同一判定」。
 */
void gpu_kernel_blend_pixel(uint8_t* dst_premultiplied, const uint8_t* src_premultiplied,
                            comp_blend_mode mode, double layer_opacity, int32_t px = 0,
                            int32_t py = 0);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_GPU_KERNEL_H */
