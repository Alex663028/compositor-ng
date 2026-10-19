// AI生成
/*
 * gpu_plan.h — GPU 合成计划：把「文档能否交给 GPU」从整篇布尔判定拆成按合成单元。
 *
 * S13/S16 的 D3D12 后端只有一条整篇闸门（gpu_document_ready）：文档里只要有一层
 * 带蒙版 / 效果 / 组 / 剪贴 / 调整 / 非全画布，就把**整篇**回落 CPU。S17 起改成
 * 两步：
 *
 *   1. 按 filter_graph 的绘制序把文档拆成「合成单元」序列；
 *   2. 每个单元分类：
 *      - 原生（native）：整幅画布、RGBA8、无蒙版/效果/旋转翻转、几何为恒等
 *        —— 直接把图层像素上传成一张源纹理；
 *      - 烘焙（baked）：其余**独立可求值**的单元（裁剪/偏移图层、带蒙版、
 *        带效果、旋转/缩放、16 位/HDR 源图层、隔离组、剪贴栈）—— 在 CPU 上
 *        用与 CPU 合成器完全相同的函数烘焙成画布尺寸的预乘 RGBA8，再上传。
 *
 * 烘焙单元之后仍以「原混合模式 + 不透明度」走 GPU 的逐像素混合，因此像素语义
 * 与 CPU 路径同源。差别只在精度：原生单元逐位上传、GPU 混合与 CPU 基准差 ≤1 LSB；
 * 烘焙单元把「蒙版 x 不透明度 x 效果」乘进预乘色时量化一次 8 位，经 MULTIPLY/SCREEN
 * 的颜色乘积项最多放大到 2 LSB（`tests/test_rhi.cpp` 里按 2 断言并写明原因）。
 * 这样做不改变 ABI、不新增导出函数，只在引擎内部把回落粒度从「整篇」降到「单元」。
 *
 * 明确仍不支持（整篇回落，错误信息具名）：
 *   - 像素源图层带 adjustments：调整层作用于「已合成画布」，不是图层自身的
 *     贡献，无法作为独立单元烘焙（需要分段回读，留待后续）；
 *   - 文档表面为 RGBA16F（HDR）：D3D12 路径只实现 8 位目标；
 *   - 不支持的混合模式：CPU 会跳过该层，这里同样跳过（不是回落）。
 */
#ifndef COMPOSITOR_RENDER_GPU_PLAN_H
#define COMPOSITOR_RENDER_GPU_PLAN_H

#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "domain/document.h"
#include "domain/layer.h"

namespace compositor {

/* 一个合成单元：画布尺寸的预乘源 + 混合模式 + 不透明度乘数。 */
struct GpuUnit {
  comp_blend_mode blend = COMP_BLEND_NORMAL;
  /*
   * GPU 内核的 alpha 乘数：
   *   - 原生单元 = 图层不透明度 × 穿透组累积系数；
   *   - 烘焙单元 = 组 / 剪贴栈基底的不透明度 × 累积系数；烘焙的**像素源图层**
   *     已经把不透明度折进预乘像素（bake_layer_contribution），所以为 1.0。
   */
  float opacity = 1.0f;
  /* 原生单元：直接读该图层的整幅 RGBA8 像素；烘焙单元为 nullptr。 */
  const Layer* native_layer = nullptr;
  /* 烘焙单元：画布尺寸的预乘 RGBA8；原生单元为空。 */
  ImageBuffer baked;

  bool is_native() const { return native_layer != nullptr; }
};

/* 计划里的分类计数，供测试/诊断证明「回落粒度已经拆细」。 */
struct GpuPlanStats {
  int32_t native_units = 0;      /* 直接上传的整幅 RGBA8 图层数 */
  int32_t baked_units = 0;       /* CPU 烘焙的像素源图层数（裁剪/蒙版/效果/变换…） */
  int32_t pass_through_groups = 0; /* 展开的穿透组数（不产生单元） */
  int32_t isolated_groups = 0;   /* 烘焙成单元的隔离组数 */
  int32_t clipping_stacks = 0;   /* 烘焙成单元的剪贴栈数 */
};

/*
 * 构建合成计划。
 *
 * units 非空时生成单元序列（会做 CPU 烘焙，按画布尺寸分配缓冲）；
 * units 为空时只做分类与可行性判定（can_render_document 用它，不分配像素）。
 * stats 可为空。失败返回 false 并写 error——ABI 层据此整篇回落 CPU。
 */
bool build_gpu_plan(const Document& doc, std::vector<GpuUnit>* units, GpuPlanStats* stats,
                    std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_RENDER_GPU_PLAN_H */
