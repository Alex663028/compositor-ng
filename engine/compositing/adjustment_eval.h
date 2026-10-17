// AI生成
/*
 * adjustment_eval.h — 调整图层的像素求值（对齐上游的「滤镜节点」语义）。
 *
 * 背景（本轮补齐的最大功能缺口）：
 *   调整图层此前只入库不求值——`comp_layer_add_adjustment` 写入成功、
 *   `comp_layer_adjustment_count` 也能读回，但合成路径完全无视它们
 *   （ctypes 实测：加 Invert / Hue/Sat / Add Noise 前后 `comp_render_flatten`
 *   输出逐字节相同）。engine/imaging 下 10 组像素内核已移植在手，却无任何调用点。
 *
 * 语义对齐上游（`Compositor/Rendering/EditorCanvas.swift:3092`）：
 *   "An adjustment re-colors what's under it, through its own mask and its
 *    folders' masks, at its opacity."
 *   即调整图层是作用于「其下方已合成结果」的滤镜节点，而不是对自身像素做变换：
 *     - 作用于 backdrop（下层合成结果），不影响其上方图层；
 *     - 经由自身蒙版与其所在组的蒙版裁剪；
 *     - 按自身不透明度与所在组的不透明度混合回 backdrop。
 *
 * 求值点：filter_graph 的 source 节点。带调整的图层不贡献自己的像素，
 * 而是把 backdrop 过一遍调整再写回——这正是上游的滤镜节点位置。
 *
 * 参数解析一律「读不到就用默认值」，不因 settings 缺键而失败：
 *   settings_json 由 .comp 互通而来，缺键是常态（上游默认即恒等）。
 *   无法识别的 kind / 无法解析的 JSON 会被跳过并计入 skipped，
 *   绝不静默产生错误像素，也不让一条坏参数毁掉整篇合成。
 */
#ifndef COMPOSITOR_COMPOSITING_ADJUSTMENT_EVAL_H
#define COMPOSITOR_COMPOSITING_ADJUSTMENT_EVAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "domain/layer.h"

namespace compositor {

struct AdjustmentEvalResult {
  bool changed = false; /* 是否真的改变了像素（恒等参数为 false） */
  size_t applied = 0;   /* 实际执行的调整个数 */
  size_t skipped = 0;   /* 因 kind 未知 / 参数无法解析而跳过的个数 */
};

/*
 * 就地把一层调整应用到像素缓冲上（画布大小的 RGBA8 预乘）。
 *
 * buf / stride 必须描述整幅画布（合成路径下即为 backdrop）。
 * 返回 false 仅当缓冲非法（空指针、尺寸非法）——此时 error 有值；
 * 单条调整的失败计入 result.skipped，不视为整体失败。
 */
bool apply_adjustments(const std::vector<Adjustment>& adjustments, uint8_t* buf,
                       int32_t width, int32_t height, size_t stride,
                       AdjustmentEvalResult* result, std::string* error);

/* 某 kind 是否有已落地的像素实现（供面板/文档如实汇报能力边界）。 */
bool adjustment_kind_is_evaluated(uint32_t kind);

/*
 * 把 RGBA8 预乘像素模糊成一份副本（sigma 以像素计），原图不变。
 *
 * 与 kind 7 的高斯模糊共用同一份三趟盒式实现（不抄第二遍）；sigma <= 0 或
 * 尺寸/步长非法时返回 false 而不产出半成品缓冲。扫描线的磷光晕（上游
 * Scanlines.swift glowing）走的是这条路：模糊线本身，再加回原图。
 */
bool blur_rgba8_copy(const uint8_t* src, int32_t width, int32_t height, size_t stride,
                     float sigma_px, std::vector<uint8_t>* out);

/*
 * 在 16 位（预乘 RGBA16，native-endian）像素上施加会混合通道的调整。
 *
 * 与 8 位的 `apply_adjustments` **共用同一张查找表**（build_hsv_cube），只把内核
 * 换成 `deep_cube_apply`（上游 DeepLayer.lookup 的口径）。恒等参数返回 false。
 * 当前覆盖 kind 0（Hue/Saturation）；其余 kind 要等它们也有明确的混合通道语义。
 */
bool apply_channel_mixing_sixteen_bit(uint16_t* pixels, size_t stride, int32_t width, int32_t height,
                                      const std::string& settings_json);

/* ---------------------------------------------------------------------------
 * C 方案 C2 的口径记录（**尚无实现，调用点随 C5 到来**）。
 *
 * 上游口径（compositor/Compositor/Rendering/LiveMaskRenderer.swift:43）：
 *   let deep = sixteenBit && original.bitsPerComponent == 16
 *   ... settings.applySixteenBit(original, ...)
 *   ——`hdr` 为真时同一段代码落在 half 像素上（`adjust` 的 context 是
 *   `DeepLayer.context(..., hdr: hdr)`）。
 *
 * 而 `LayerAdjustment.applySixteenBit`（LayerAdjustment.swift:156-160）只有三条路：
 *   1. 高斯/动感模糊 → CI 里按图像自身深度渲染（与像素格式无关）；
 *   2. Exposure 且是 HDR → **deep_hdr_exposure**，直接在 half 上，不经 normalize；
 *   3. 跨通道的调整（hsv / blackWhite / colorBalance / gradientMap）→ **deep_cube_apply**，
 *      它在 16 位 RGBA 布局上查表，于是 `DeepLayer.lookup` 先 deep_hdr_normalize
 *      把高光除回白内、查完 deep_hdr_restore 乘回去（DeepLayers.swift:240-244）；
 *   4. 其余 → **deep_round_trip**（deep_split/deep_merge），同样被
 *      `DeepLayer.run`（:196）包在 normalize/restore 之间。
 *
 * 为什么这里还没有 `apply_adjustments_hdr`：本仓的调整求值只有一个调用点
 * （filter_graph.cpp 的 kSource，作用在 RGBA8 的 backdrop 上），而生成路径的每一块
 * 表面都是 `COMP_PIXEL_RGBA8`。也就是说，即便照上面的分派表写出一个「作用在 half
 * 像素上」的求值函数，也**没有任何调用者**——它会成为第 7 族零通路内核，正是
 * README §6.6 与历轮审计反复点名的那种缺口。让调整求值真正落在 half 上，前提是
 * 合成表面能按文档深度携带 half（上游那个 `sixteenBit` 开关），那是 C5 的范围。
 *
 * C2 已落地的那一半（有真调用点）在 domain/tool_ops.cpp：`deep_hdr_round_trip`
 * 把任一 8 位操作包在 deep_hdr_normalize / deep_hdr_restore 之间跑 16 位往返，
 * 由 `COMP_TOOL_DEEP` 的 round trip 分支对 HDR 图层调用（上游 DeepLayers.swift:196
 * 的 `normalizing(deep) { run(...) }`）。上面的分派表 2/4 两条据此可以直接落，
 * 只要 C5 把调整求值的缓冲换成 half，第 1、3 条也已具备同一层原语。
 * --------------------------------------------------------------------------- */

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_ADJUSTMENT_EVAL_H */
