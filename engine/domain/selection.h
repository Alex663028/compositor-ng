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
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "domain/domain_state.h"

namespace compositor {

class Document;

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
/* 色彩范围的 fuzziness 上限（上游 ColorRangeEdit.fuzzinessRange = 0...200）。 */
constexpr int32_t kMaxColorRangeFuzziness = 200;

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

/*
 * 把羽化半径**绝对设置**为 feather 像素（0..250）。
 *
 * 与 selection_modify(kSelectionFeather) 的 sqrt(f1² + f2²) 叠加明确区分：这里
 * 是覆盖当前半径，不是「再加一层」。feather = 0 清除羽化。
 * 只改半径、不动基础蒙版；没有选区或显式空选区时无操作（changed = false）。
 * 越界返回 false（参数闸与 ABI 同口径）。
 */
bool selection_set_feather(SelectionState* selection, int32_t feather, bool* changed);

/* 载入选区来源：0 = 图层像素（取其 alpha），1 = 图层蒙版。 */
enum SelectionLoadSource {
  kSelectionLoadPixels = 0,
  kSelectionLoadMask = 1
};

/*
 * 从图层生成文档分辨率选区（上游「Load a layer's pixels or a mask as a selection」）。
 *
 * 图层像素/蒙版都按 `Layer::effective_bounds()` 的放置映射回文档坐标：偏移、
 * 缩放、旋转与翻转全部反向求逆，采样用最近邻（选区是硬边蒙版，不做插值）。
 *   - source = kSelectionLoadPixels 取像素 alpha；kSelectionLoadMask 取蒙版灰度；
 *   - threshold 0..255：采样值 <= threshold 记为 0，其余保留原值（保留半透明边）；
 *   - combine 语义与形状叠加一致：replace / add（max）/ subtract（按 8 位乘法擦除）；
 *   - 与形状操作一样，成功的载入把 feather 归 0；源为空时 replace 得到显式空选区。
 *
 * 失败（画布非法、来源/叠加枚举未知、threshold 越界）返回 false 且不改状态。
 */
bool selection_load_layer(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                          const Layer& layer, int32_t source, int32_t combine, int32_t threshold,
                          bool* changed);

/*
 * 平移选区轮廓（像素，正数向右/向下）。画布外的部分被裁掉，feather 保留。
 * 结果与旧蒙版逐字节一致时 changed = false（例如空选区或 dx = dy = 0）。
 */
bool selection_translate(SelectionState* selection, int32_t dx, int32_t dy, bool* changed);

/* ------------------------------------------------------------ 魔棒（S7） */

/* 参考色邻域半边长的上限（与 tool_ops 的 select.radius 同口径）。 */
constexpr int32_t kMaxSelectionWandRadius = 512;

/* 魔棒取样来源：0 = 指定图层的像素，1 = 整篇合成（与吸管「合并」同源）。 */
enum SelectionWandSource {
  kSelectionWandLayer = 0,
  kSelectionWandMerged = 1
};

/* 魔棒参数。seed 是**文档像素**坐标（左上原点），tolerance 是逐通道距离阈值
 * 0..255（alpha 参与比较），radius 是参考色 (2r+1)² 邻域的半边长，
 * contiguous != 0 表示从种子做 4-连通泛洪、0 表示选中全图所有匹配像素。 */
struct SelectionWandParams {
  int32_t source = kSelectionWandLayer;
  double seed_x = 0.0;
  double seed_y = 0.0;
  int32_t tolerance = 32;
  int32_t contiguous = 1;
  int32_t radius = 0;
};

/*
 * 魔棒选区：用 wand_mask 内核在图层像素或整篇合成上匹配，再把结果按 combine
 * 并回文档选区（上游 MagicWand 的工具语义）。
 *
 * 与 `COMP_TOOL_SELECT` 的区别：那次把结果**累积写进图层蒙版**（会覆写既有
 * 蒙版，宿主还得再 load 一次才成选区），这里直接产出选区、不动图层蒙版。
 *
 * 返回 COMP_OK 时 changed 回报状态是否真的变了；0 个匹配像素时**不改选区**
 * （changed = false），与「点了个空处不留下空蒙版」同口径。失败返回具体错误码：
 *   - COMP_ERR_NOT_FOUND：图层来源的图层不存在；
 *   - COMP_ERR_STATE：图层是组 / 无像素 / 不是 1:1 变换（泛洪网格与文档网格
 *     之间必须是整数平移）；
 *   - COMP_ERR_INVALID_ARGUMENT：画布非法、来源/叠加枚举未知、容差/半径越界、
 *     contiguous 不是 0/1、seed 非有限或落在画布/取样面之外；
 *   - COMP_ERR_OUT_OF_MEMORY：内核工作内存申请失败。
 */
comp_result selection_apply_wand(SelectionState* selection, int32_t canvas_width,
                                 int32_t canvas_height, const Document& document,
                                 comp_layer_id layer, const SelectionWandParams& params,
                                 int32_t combine, bool* changed, std::string* error);

/*
 * 色彩范围选区（PS Select > Color Range；上游 ColorRangeSelection.swift）。
 *
 * 在**整篇合成**（sampleAllLayers）上按颜色匹配**全部**像素（不要求连续，
 * 这是与魔棒 flood fill 的本质区别）：一个像素被选中 = 与某个 include
 * 颜色的逐通道绝对差都 <= fuzziness，**且**与任何 exclude 颜色都不满足。
 * invert != 0 时把匹配结果取反再写入选区。
 *
 * include / exclude 是扁平 RGB 三元组数组（count 为颜色数，不是字节数）。
 * 可任一为空：include 为空按「无匹配」（invert 时全选）处理。
 *
 * 距离度量与魔棒 / trim 同一家族（RGB 三通道 Chebyshev），直接在合成图
 * 的 RGBA8 字节上比较，不做反预乘——与 wand_mask 同口径。fuzziness
 * 范围 0..200（上游 fuzzinessRange）；硬边选区（匹配 = 255，否则 0）。
 *
 * combine 语义与形状叠加一致（replace / add / subtract）。**0 个匹配像素
 * 不改选区**（changed = false），与魔棒「点了个空处不留下空蒙版」同口径。
 * 失败返回具体错误码（参数闸在一切写入之前）。
 */
comp_result selection_apply_color_range(SelectionState* selection, int32_t canvas_width,
                                        int32_t canvas_height, const Document& document,
                                        const uint8_t* include, size_t include_count,
                                        const uint8_t* exclude, size_t exclude_count,
                                        int32_t fuzziness, int32_t invert, int32_t combine,
                                        bool* changed, std::string* error);

/* 有效 coverage（画布分辨率，白 = 选中）：base 按 feather 高斯模糊。
 * selection 不活跃或基础蒙版为空时 out 清空（调用方必须先看 active）。 */
void selection_effective_coverage(const SelectionState& selection, std::vector<uint8_t>* out);

/*
 * 把一次像素操作的结果按选区与原像素混合（上游 PixelAdjust.blend）。
 *
 * rgba 是**已经改好**的图层像素（整层视图，stride 由调用方给），original
 * 是改之前的拷贝。coverage 是**文档分辨率**有效选区，图层像素按
 * effective_bounds() 的原点平移到文档坐标（与合成/工具同款约定）；
 * 图层落在画布外的部分 coverage 视为 0（选区之外），保持原像素。
 * coverage 为空 / 画布非法时整层回滚成 original（显式空选区在上游就是
 * 「什么都不改」）。
 *
 * S25 起从 filters.cpp 提取共享：filter_apply 与 invert_layer 共用同一条
 * 选区混合口径，避免两处各写一套互相漂移。
 */
void blend_through_selection(uint8_t* rgba, size_t stride, const uint8_t* original, int32_t width,
                             int32_t height, const std::vector<uint8_t>& coverage,
                             const comp_rect& bounds, int32_t canvas_width,
                             int32_t canvas_height);

/* 基础蒙版里非零像素的包围盒，按 ceil(2 * feather) 外扩后裁到画布。
 * 没有选区或显式空选区返回 {0, 0, 0, 0}。 */
comp_rect selection_bounds(const SelectionState& selection);

/* 基础蒙版是否没有任何非零像素（显式空选区 / 未初始化）。 */
bool selection_is_empty(const SelectionState& selection);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_SELECTION_H */
