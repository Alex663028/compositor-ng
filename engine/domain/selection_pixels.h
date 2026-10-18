// AI生成
/*
 * selection_pixels.h — 选区内的像素搬移（S5）。
 *
 * 对齐上游 README「Selections」第三条的后半句：*move and duplicate the pixels
 * inside* a selection。选区本身是文档分辨率的蒙版（见 selection.h），像素搬移
 * 发生在**目标图层的像素空间**里，两者靠 `Layer::effective_bounds()` 对齐。
 *
 * 口径：
 *   - coverage 取 `selection_effective_coverage`（羽化已生效），按 coverage 把
 *     预乘像素拆成「搬走的部分」与「留下的部分」：搬走 = px × cov，留下 =
 *     px × (1 - cov)。这与形状叠加的 subtract 同一口径。
 *   - 只支持 1:1 未旋转/未翻转的图层：选区像素搬移是像素级操作，缩放或旋转
 *     图层时「同一个像素」没有良定义的落点，宁可在参数闸拒绝也不做重采样。
 *   - 只支持 8 位图层：16 位/HDR 要用 16 位缓冲区承载搬移结果，当前 ABI 没有
 *     对应入口，先按 `COMP_ERR_STATE` 拒绝并写清楚原因。
 *   - 目标位置越出图层现有像素矩形时**扩层**（原点随之外移，像素数据按新原点
 *     重新落位），越出画布的部分丢弃并写 note；蒙版随像素矩形同步平移。
 *   - 区域为空 / 选区为空 / 源像素全透明时 changed = false（不产生撤销步）。
 */
#ifndef COMPOSITOR_DOMAIN_SELECTION_PIXELS_H
#define COMPOSITOR_DOMAIN_SELECTION_PIXELS_H

#include <string>

#include "domain/domain_state.h"
#include "domain/layer.h"
#include "domain/selection.h"

namespace compositor {

/*
 * 把目标图层在选区内的像素按 (dx, dy) 搬移（duplicate 非 0 时保留原像素）。
 *
 * 成功返回 true（无论有没有实际改动，看 out_changed）；参数/状态不满足返回
 * false，原因写入 error。note 可为 nullptr，写入非致命说明（扩层、画布裁切）。
 */
bool selection_move_pixels(Layer* layer, int32_t canvas_width, int32_t canvas_height,
                           const SelectionState& selection, int32_t dx, int32_t dy,
                           bool duplicate, bool* out_changed, std::string* note,
                           std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_SELECTION_PIXELS_H */
