// AI生成
/*
 * selection.h — 选区几何（F1）：形状栅格化、叠加模式、羽化与扩展/收缩。
 *
 * 语义唯一真源是上游 `Compositor/Document/Selection.swift`（DocumentSelection +
 * EditorSession 的选区命令）。本层只做两件事：
 *   1. 把形状（矩形/椭圆/多边形/贝塞尔路径）栅格化成文档分辨率的 8 位蒙版；
 *   2. 在蒙版上执行 replace / add / subtract、Select All / Deselect / Inverse、
 *      Feather / Expand / Contract。
 *
 * 为什么存蒙版而不是路径：见 domain_state.h 的 SelectionState 注释。
 *
 * 与上游逐条对齐的口径：
 *   - 栅格化是**非零缠绕**（CGPath.fillPath(using: .winding)），因此反向子路径
 *     是洞、同向自交不会互相抵消；
 *   - 形状先与画布求交（画布外部分被裁掉）；空形状 replace ⇒ 显式空选区，
 *     add ⇒ 原选区不变，subtract ⇒ 原选区不变；
 *   - 退化形状（包围盒宽或高 <= 0）在 replace 模式等价于 deselect，其它模式
 *     忽略（上游 finishLasso 的 guard）；
 *   - **任何**形状操作都把 feather 重置为 0（applySelection 新建
 *     DocumentSelection 时没有传 feather）；
 *   - 反选保留 feather；全选/取消选择把 feather 归 0；
 *   - Feather 按 sqrt(f1² + f2²) 叠加、上限 250（featherSelection(by:)）；
 *   - Expand 外扩会被画布裁掉，Contract 连画布边缘也一起内缩，缩到没有就是
 *     **显式空选区**（contractSelection 的注释）；
 *   - 没有选区或显式空选区时，Feather / Expand / Contract 一律无操作
 *     （上游 canModifySelection == false）。
 *
 * 数值口径：
 *   - 形状坐标是文档像素（左上原点），与 ABI 一致；
 *   - 栅格化用 4 行超采样 + 水平方向解析覆盖率（非 AA 时取每像素中心一票）；
 *   - Expand / Contract 用**精确欧氏距离场**（Felzenszwalb & Huttenlocher），
 *     在选区包围盒外扩 amount+2 的窗口里计算，窗口外一律视为未选中；
 *   - Feather 的 sigma = feather / 2（上游 applyingGaussianBlur(sigma: feather/2)），
 *     边界按 clampedToExtent 钳到画布边缘（全选羽化不会在画布边缘变淡）。
 */
#ifndef COMPOSITOR_DOMAIN_SELECTION_H
#define COMPOSITOR_DOMAIN_SELECTION_H

#include <cstdint>
#include <vector>

#include "compositor/comp_core.h"
#include "domain/domain_state.h"

namespace compositor {

/* 与 ABI 的 comp_selection_shape / combine / modify 数值一一对应。 */
enum SelectionShape {
  kSelectionRect = 0,
  kSelectionEllipse = 1,
  kSelectionPolygon = 2
};

enum SelectionCombine {
  kSelectionReplace = 0,
  kSelectionAdd = 1,
  kSelectionSubtract = 2
};

enum SelectionModify {
  kSelectionFeather = 0,
  kSelectionExpand = 1,
  kSelectionContract = 2
};

/* 上限：先挡住荒谬输入，避免为「一个请求」分配几个 GB。
 * 多边形按「行 × 活跃边」扫描，26 万顶点已远超正常套索（0.25 像素采样）；
 * 路径段在扁平化后还会受同一个顶点上限约束。 */
constexpr int32_t kMaxSelectionPoints = 1 << 18;
constexpr int32_t kMaxSelectionSegments = 1 << 16;
constexpr int32_t kMaxSelectionFeather = 250;
constexpr int32_t kMaxSelectionGrow = 500;

/* 三次贝塞尔路径段。start 只在 new_subpath 为真（或这是第一段）时使用；
 * 否则曲线从上一段的终点继续（对齐 CGPath 的 current point 语义）。 */
struct SelectionCubic {
  float start[2] = {0.0f, 0.0f};
  float control1[2] = {0.0f, 0.0f};
  float control2[2] = {0.0f, 0.0f};
  float end[2] = {0.0f, 0.0f};
  bool new_subpath = false;
};

/*
 * 用形状产生/叠加选区。points 是扁平的 (x,y) 序列：
 *   - rect / ellipse：恰好 2 个点（两个对角点，顺序任意）；
 *   - polygon：至少 3 个点，按给定顺序闭合。
 * antialias 只影响**这一次**栅格化；已经存在的基础蒙版保持原样。
 *
 * 失败（画布尺寸非法、点数为空/超限/坐标非有限、枚举未知）返回 false 且
 * 不改 selection。成功时 changed 回报状态是否真的变了（宿主据此决定是否
 * 进历史——上游 setSelection 对相同值不产生撤销步）。
 */
bool selection_apply_shape(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                           int32_t shape, const float* points, int32_t point_count,
                           int32_t combine, bool antialias, bool* changed);

/* 同上，输入是贝塞尔路径段（钢笔/路径选区）。 */
bool selection_apply_path(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                          const SelectionCubic* segments, int32_t segment_count,
                          int32_t combine, bool antialias, bool* changed);

/* Select All：整张画布，feather = 0。 */
bool selection_select_all(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                          bool* changed);

/* Deselect：没有选区时 changed = false（上游 setSelection(nil) 前先 guard）。 */
bool selection_deselect(SelectionState* selection, bool* changed);

/* 反选：没有选区时无操作；反选「全选」得到**无选区**（上游 inverse.isEmpty ⇒ nil）。 */
bool selection_invert(SelectionState* selection, bool* changed);

/* op = kSelectionFeather / kSelectionExpand / kSelectionContract。
 * amount 范围：feather 1..250，expand/contract 1..500（上游同一组闸）。
 * 没有选区或显式空选区时无操作（changed = false）。 */
bool selection_modify(SelectionState* selection, int32_t op, int32_t amount, bool* changed);

/* 有效 coverage（画布分辨率，白 = 选中）：base 按 feather 高斯模糊。
 * selection 不活跃或基础蒙版为空时 out 清空（调用方必须先看 active）。 */
void selection_effective_coverage(const SelectionState& selection, std::vector<uint8_t>* out);

/* 基础蒙版里非零像素的包围盒，按 ceil(2 * feather) 外扩后裁到画布。
 * 没有选区或显式空选区返回 {0, 0, 0, 0}。 */
comp_rect selection_bounds(const SelectionState& selection);

/* 基础蒙版是否没有任何非零像素（显式空选区 / 未初始化）。 */
bool selection_is_empty(const SelectionState& selection);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_SELECTION_H */
