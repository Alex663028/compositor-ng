// AI生成
/*
 * comp_selection.h — 选区几何（F1）：形状、叠加、全选/取消/反选、羽化与扩缩。
 *
 * 语义唯一真源是上游 `Compositor/Document/Selection.swift`（DocumentSelection +
 * EditorSession 的选区命令）。三条贯穿全篇的口径：
 *   1. 内部存的是**文档分辨率的 8 位灰度蒙版**（0 = 不选中），不是路径；
 *      消费方要读「有效 coverage」请走 comp_selection_get_coverage
 *      （它把羽化算进去），不要自己拿 base 去当 coverage。
 *   2. 「无选区」与「显式空选区」是不同的状态：前者编辑不受限，后者是一个
 *      合法但落空的形状（比如画布外的矩形），编辑结果为空。用
 *      comp_selection_get_active 区分——active = 0 是无选区，active = 1 且
 *      coverage 全 0 是显式空选区。
 *   3. 写操作 = 一步撤销。函数在未处于显式事务时自动补一个隐式事务；
 *      **什么都没改**时（out_changed = 0）不产生撤销步。
 *
 * 参数闸（一致适用于所有写函数）：
 *   - comp_document 句柄必须有效，否则 COMP_ERR_INVALID_ARGUMENT；
 *   - 枚举值越界、点/段数为空或超过上限、坐标非有限（NaN/Inf）→
 *     COMP_ERR_INVALID_ARGUMENT；
 *   - 形状坐标是**文档像素**（左上原点），画布外部分按上游口径裁掉。
 *
 * 上限（防止一次请求吃掉几个 GB）：
 *   - 形状点数 2^18，路径段数 2^16（扁平化后的顶点总数同样受 2^18 约束）；
 *   - Feather 1..250；Expand / Contract 1..500。
 */
#ifndef COMPOSITOR_COMP_SELECTION_H
#define COMPOSITOR_COMP_SELECTION_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 形状种类。矩形/椭圆用**两个对角点**（顺序任意）；多边形按给定顺序闭合。 */
typedef enum comp_selection_shape {
  COMP_SELECTION_RECT    = 0,
  COMP_SELECTION_ELLIPSE = 1,
  COMP_SELECTION_POLYGON = 2
} comp_selection_shape;

/* 叠加模式（与上游 SelectionMode 一一对应）。 */
typedef enum comp_selection_combine {
  COMP_SELECTION_REPLACE  = 0, /* 替换 */
  COMP_SELECTION_ADD      = 1, /* 并集 */
  COMP_SELECTION_SUBTRACT = 2  /* 差集 */
} comp_selection_combine;

/* 选区修改种类。类型名带 _op 后缀：C 里 typedef 与函数名共用普通标识符
 * 命名空间，`comp_selection_modify` 已被函数占用。 */
typedef enum comp_selection_modify_op {
  COMP_SELECTION_FEATHER  = 0, /* 羽化（按 sqrt 叠加，上限 250） */
  COMP_SELECTION_EXPAND   = 1, /* 外扩（被画布裁掉） */
  COMP_SELECTION_CONTRACT = 2  /* 内缩（画布边缘也一起内缩） */
} comp_selection_modify_op;

/*
 * 三次贝塞尔路径段。start 只在 new_subpath 非 0（或这是第一段）时使用；
 * 否则曲线从上一段的终点继续（对齐 CGPath 的 current point 语义）。
 * 每段以 end 收尾，整条子路径按闭合处理。
 */
typedef struct comp_selection_cubic {
  float   start[2];
  float   control1[2];
  float   control2[2];
  float   end[2];
  int32_t new_subpath;
} comp_selection_cubic;

/* ----------------------------------------------------------- 形状与路径 */

/*
 * 用形状产生/叠加选区。points 是扁平的 (x, y) 序列：
 *   - rect / ellipse：恰好 2 个点；
 *   - polygon：至少 3 个点。
 * antialias 只影响**这一次**栅格化，已经存在的基础蒙版保持原样
 * （1 = 4 行超采样 + 水平解析覆盖率，0 = 每像素中心取一票的硬边）。
 *
 * 叠加语义（与上游一致）：
 *   - replace：新形状替换整个选区；
 *   - add：并集；
 *   - subtract：从现有选区里减去形状（没有选区时无操作）。
 * 任何成功的形状操作都把 feather 重置为 0（羽化不是选区的一部分，
 * 它是形状的一个参数）。退化形状（包围盒宽或高 <= 0）在 replace 下等价于
 * 取消选择，在其它模式下被忽略。
 *
 * out_changed 可为 NULL；返回 COMP_OK 时写 1/0 表示状态是否真的变了。
 */
COMP_API comp_result comp_selection_apply_shape(comp_document document,
                                                comp_selection_shape shape,
                                                const float* points,
                                                int32_t point_count,
                                                comp_selection_combine combine,
                                                int32_t antialias,
                                                int32_t* out_changed);

/* 同上，输入是贝塞尔路径（钢笔 / 路径选区）。 */
COMP_API comp_result comp_selection_apply_path(comp_document document,
                                               const comp_selection_cubic* segments,
                                               int32_t segment_count,
                                               comp_selection_combine combine,
                                               int32_t antialias,
                                               int32_t* out_changed);

/* ------------------------------------------------------- 全选/取消/反选 */

/* Select All：整张画布，feather = 0。 */
COMP_API comp_result comp_selection_select_all(comp_document document, int32_t* out_changed);

/* Deselect：没有选区时 out_changed = 0。 */
COMP_API comp_result comp_selection_deselect(comp_document document, int32_t* out_changed);

/* 反选：没有选区时无操作；反选「全选」得到**无选区**（不是空选区）。 */
COMP_API comp_result comp_selection_invert(comp_document document, int32_t* out_changed);

/* ----------------------------------------------------------- 羽化/扩缩 */

/*
 * op = COMP_SELECTION_FEATHER / EXPAND / CONTRACT。
 * amount 范围：feather 1..250，expand/contract 1..500。
 * 没有选区或显式空选区时一律无操作（out_changed = 0）。
 * 扩缩是精确的欧氏距离场（不是方框），边缘按整像素中心计算：
 * 对整像素选区，「外扩 N」与「内缩 N」后的边界落在整像素上。
 */
COMP_API comp_result comp_selection_modify(comp_document document,
                                           comp_selection_modify_op op,
                                           int32_t amount,
                                           int32_t* out_changed);

/* --------------------------------------------------------------- 查询 */

/* 是否有选区：1 = 有（含显式空选区），0 = 无。 */
COMP_API comp_result comp_selection_get_active(comp_document document, int32_t* out_active);

/* 当前羽化半径（像素）。 */
COMP_API comp_result comp_selection_get_feather(comp_document document, float* out_feather);

/*
 * 选区包围盒（裁剪到画布）：非零像素的包围盒外扩 ceil(2 * feather)。
 * 无选区或显式空选区返回 {0, 0, 0, 0}。
 */
COMP_API comp_result comp_selection_get_bounds(comp_document document, comp_rect* out_bounds);

/*
 * 有效 coverage（把 feather 算进去，白 = 选中），文档分辨率。
 * out_mask 由调用方提供尺寸与缓冲：width/height 必须等于文档尺寸，
 * stride >= width，pixels 非空。
 * 无选区时返回 COMP_ERR_STATE（先查 get_active 再决定要不要取）。
 */
COMP_API comp_result comp_selection_get_coverage(comp_document document, comp_mask* out_mask);

/* ------------------------------------------------- 载入 / 平移 / 像素搬移（S5） */

/* 载入来源：0 = 图层像素（alpha），1 = 图层蒙版。 */
typedef enum comp_selection_load_source {
  COMP_SELECTION_LOAD_PIXELS = 0,
  COMP_SELECTION_LOAD_MASK   = 1
} comp_selection_load_source;

/*
 * 把图层的像素 alpha 或蒙版载入为选区（上游「Load a layer's pixels or a mask
 * as a selection」）。图层按 transform 映射回文档坐标（偏移/缩放/旋转/翻转）；
 * threshold 0..255：采样值 <= threshold 记为 0，其余保留原值。
 * 叠加语义与形状操作一致（replace / add / subtract），成功后 feather 归 0。
 * 图层不存在 -> COMP_ERR_NOT_FOUND；来源/叠加枚举或 threshold 越界 ->
 * COMP_ERR_INVALID_ARGUMENT。
 */
COMP_API comp_result comp_selection_load_layer(comp_document document, comp_layer_id layer,
                                               comp_selection_load_source source,
                                               comp_selection_combine combine, int32_t threshold,
                                               int32_t* out_changed);

/*
 * 平移选区轮廓 (dx, dy) 像素：移出画布的部分被裁掉，羽化保留。
 * 宿主应在**一次拖拽结束时**调用一次（拖拽中的预览由宿主自己按原蒙版平移绘制），
 * 否则每次鼠标移动都会产生一条撤销步。起点为空选区或位移为 0 时 out_changed = 0。
 */
COMP_API comp_result comp_selection_translate(comp_document document, int32_t dx, int32_t dy,
                                              int32_t* out_changed);

/*
 * 搬移选区内的像素（上游「move and duplicate the pixels inside」）。
 *   - duplicate = 0：剪切语义（原位置按 coverage 擦除）；非 0：复制语义；
 *   - 目标越出图层像素矩形时扩层（transform 原点/尺寸随之外移），越出画布丢弃；
 *   - 仅支持 8 位、未旋转/翻转、1:1 缩放的图层；其它情况返回 COMP_ERR_STATE；
 *   - 一步撤销；没有可搬的不透明像素时 out_changed = 0。
 * 图层不存在 -> COMP_ERR_NOT_FOUND。
 */
COMP_API comp_result comp_selection_move_pixels(comp_document document, comp_layer_id layer,
                                                int32_t dx, int32_t dy, int32_t duplicate,
                                                int32_t* out_changed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_SELECTION_H */
