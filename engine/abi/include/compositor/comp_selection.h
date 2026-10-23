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
  COMP_SELECTION_CONTRACT = 2, /* 内缩（画布边缘也一起内缩） */
  COMP_SELECTION_BORDER   = 3, /* 边界：沿轮廓内外总宽 amount 像素的环带 */
  COMP_SELECTION_SMOOTH   = 4  /* 平滑：开+闭形态学，圆角但保留直边 */
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
 * 注意 point_count 数的是**点**（一个点 = x,y 一对 float），不是 float 个数：
 * 传一个矩形必须给 points_count = 2（4 个 float），传 4 会被判参数非法。
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
 * op = COMP_SELECTION_FEATHER / EXPAND / CONTRACT / BORDER / SMOOTH。
 * amount 范围：feather 1..250，expand/contract/border 1..500，smooth 1..100。
 * 没有选区或显式空选区时一律无操作（out_changed = 0）。
 * 扩缩是精确的欧氏距离场（不是方框），边缘按整像素中心计算：
 * 对整像素选区，「外扩 N」与「内缩 N」后的边界落在整像素上。
 *
 * BORDER 沿选区轮廓保留一条**总宽 amount 像素**的环带（外扩 ceil(amount/2)
 * 减内缩 floor(amount/2)，奇数时外侧多 1 px），覆盖率按 8 位乘法相减，羽化过
 * 的边缘相减后仍是软边；
 * SMOOTH 走开+闭的形态学，圆化拐角与毛刺但逐像素保留直边，本来就规整的选区
 * 是恒等变换（out_changed = 0，不产生撤销步）。两者都只改基础蒙版、不动
 * feather 半径。
 */
COMP_API comp_result comp_selection_modify(comp_document document,
                                           comp_selection_modify_op op,
                                           int32_t amount,
                                           int32_t* out_changed);

/*
 * 绝对设置羽化半径（0..250 像素）。
 *
 * 与 comp_selection_modify(COMP_SELECTION_FEATHER) 的 sqrt(f1² + f2²) **叠加**
 * 语义明确区分：本入口是**覆盖**——直接把当前选区的羽化半径设成 feather，
 * 不做累积。feather = 0 清除羽化（有效 coverage 回到硬边蒙版）。
 *
 * 只改半径、不动基础蒙版；没有选区或显式空选区时无操作（out_changed = 0，
 * 不产生撤销步）。feather 越界 -> COMP_ERR_INVALID_ARGUMENT（事务之前拒绝）。
 */
COMP_API comp_result comp_selection_set_feather(comp_document document, int32_t feather,
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

/* ------------------------------------------------------------ 魔棒（S7） */

/* 魔棒取样来源：0 = 指定图层的像素，1 = 整篇合成（与吸管「合并」同源）。 */
typedef enum comp_selection_wand_source {
  COMP_SELECTION_WAND_LAYER  = 0,
  COMP_SELECTION_WAND_MERGED = 1
} comp_selection_wand_source;

/*
 * 魔棒选区：在文档像素 seed 处按逐通道容差匹配，把结果按 combine 并入选区
 * （上游 MagicWand 的工具语义）：tolerance 0..255（alpha 参与比较）；
 * contiguous != 0 时从种子做 4-连通泛洪，0 时选中全图所有匹配像素；
 * radius 0..512 是参考色的 (2r+1)² 邻域半边长。
 *
 * 与 `comp_tool_apply(COMP_TOOL_SELECT)` 的区别：那次把结果**累积写进图层蒙版**
 * （会覆写既有蒙版，宿主还要再 load 一次才得到选区），本入口直接产出文档选区、
 * 不动图层蒙版，因此可以安全地反复点选。
 *
 * 语义与错误码：
 *   - 0 个匹配像素时**不改选区**（out_changed = 0），不会静默清掉既有选区；
 *   - 图层来源要求 1:1 变换（未缩放/旋转/翻转），否则 COMP_ERR_STATE；
 *   - 图层不存在 -> COMP_ERR_NOT_FOUND；组图层/无像素 -> COMP_ERR_STATE；
 *   - 枚举、容差、半径、contiguous 非法或 seed 落在画布/取样面之外 ->
 *     COMP_ERR_INVALID_ARGUMENT；
 *   - 一步撤销（加入调用方已开启的显式事务，或自成一个隐式步）。
 */
COMP_API comp_result comp_selection_apply_wand(comp_document document, comp_layer_id layer,
                                               comp_selection_wand_source source,
                                               double seed_x, double seed_y,
                                               int32_t tolerance, int32_t contiguous,
                                               int32_t radius, comp_selection_combine combine,
                                               int32_t* out_changed);

/* ------------------------------------------------------------ 色彩范围（S25） */

/*
 * 色彩范围选区（PS Select > Color Range；上游 ColorRangeSelection.swift）。
 *
 * 在**整篇可见合成**上按颜色匹配**全部**像素（不要求连续，这是与魔棒
 * flood fill 的本质区别）。一个像素被选中 = 与某个 include 颜色的 RGB
 * 三通道绝对差都 <= fuzziness，**且**与任何 exclude 颜色都不满足；
 * invert != 0 时把匹配结果取反再写入选区。
 *
 * 参数：
 *   - include / include_count：扁平 RGB 三元组数组（include_count 是
 *     **颜色数**，不是字节数；include 为 NULL 时按无匹配处理）。可传
 *     NULL + 0 表示不含任何取样色；
 *   - exclude / exclude_count：同上，被排除色覆盖（上游面板里 Option
 *     点选减去的颜色）；
 *   - fuzziness 0..200（上游 fuzzinessRange）；
 *   - invert：!= 0 时选「匹配之外」的像素。
 *
 * 语义与错误码：
 *   - 距离度量与魔棒同家族（RGB 三通道 Chebyshev），直接在合成 RGBA8
 *     字节上比较，不反预乘；
 *   - 0 个匹配像素时**不改选区**（out_changed = 0，与魔棒同口径）；
 *   - combine 语义与形状/魔棒叠加一致（replace / add / subtract）；
 *   - 数组为 NULL 但 count > 0、fuzziness 越界、枚举非法 ->
 *     COMP_ERR_INVALID_ARGUMENT；
 *   - 一步撤销（加入调用方已开启的显式事务，或自成一个隐式步）。
 *
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_selection_apply_color_range(comp_document document,
                                                      const uint8_t* include, size_t include_count,
                                                      const uint8_t* exclude, size_t exclude_count,
                                                      int32_t fuzziness, int32_t invert,
                                                      comp_selection_combine combine,
                                                      int32_t* out_changed);

/* ------------------------------------------------------- 手柄重采样（S46） */

/*
 * 基础蒙版的紧包围盒（非零像素的包围盒，**不含**羽化外扩）。
 * 没有选区或显式空选区时写 {0, 0, 0, 0}。
 *
 * 与 comp_selection_get_bounds 的区别：那个口径是「可见 coverage 的范围」，
 * 羽化会把包围盒外扩 ceil(2 * feather)；选区手柄要贴在**真正被变换的那张
 * 蒙版**上，所以用这一个。宿主画手柄、算拖拽起点都用它。
 */
COMP_API comp_result comp_selection_get_base_bounds(comp_document document, comp_rect* out_bounds);

/*
 * 把选区整体重采样到绝对矩形 bounds（画在选区边上的扩缩手柄）。
 *
 * 源矩形恒为当前 `comp_selection_get_base_bounds`，因此
 * `bounds == get_base_bounds()` 是**恒等变换**：逐字节不变、out_changed = 0，
 * 不会因为手柄没动就多一条撤销步。这是 comp_selection_modify（相对扩缩）
 * 无法表达的语义：手柄给的是**绝对落点**，不是「再扩几像素」。
 *
 * 采样是双线性（保留抗锯齿边缘）；bounds 越出画布的部分直接丢弃（画布外
 * 没有选区）。feather 半径原样保留——羽化是选区参数，不是几何的一部分
 * （与 comp_selection_translate 同口径）。宽或高 <= 0 的退化 bounds 等价于
 * 「把选区收成空」：写入显式空选区（active 仍为 1）。不做镜像翻转，手柄
 * 拖过头时的夹取由宿主负责。
 *
 * 没有选区 / 基础蒙版为空时无操作（out_changed = 0）；一步撤销
 * （加入调用方已开启的显式事务，或自成一个隐式步）。
 * out_bounds 为 NULL -> COMP_ERR_INVALID_ARGUMENT。
 */
COMP_API comp_result comp_selection_set_bounds(comp_document document, comp_rect bounds,
                                               int32_t* out_changed);

/* ------------------------------------------------- 存储通道（ROI #5） */

/*
 * 存储的选区通道 = PS 的 Alpha 通道（Select > Save Selection / Load Selection）。
 *
 * 通道是**文档状态**：随 `.comp` 保存、随撤销/重做回退，因此归文档句柄。
 * 每条通道存「硬边选区蒙版 + 羽化半径」：载回时逐字节恢复硬边并恢复同一个
 * 羽化参数，保存/载入往返相等（不存烘焙过羽化的 coverage，否则二次模糊）。
 * 蒙版恒为**文档分辨率**；换画布（Image Size / Canvas Size / Crop）会连同
 * 选区一起丢弃全部通道，旋转画布则逐通道转置。
 *
 * 上限：每条文档 16 个通道（kMaxSelectionChannels）。容量满 -> COMP_ERR_STATE。
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */

/* 通道数。从未保存过的文档返回 0。只读：不开事务、不进撤销栈。 */
COMP_API comp_result comp_selection_channel_count(comp_document document, size_t* out_count);

/*
 * 读取第 index 个通道的名字（UTF-8，含结尾 NUL）。
 *
 * 与 comp_layer_get_name 同一把两段式尺子：
 *   - `out_required` 写所需字节数（含 NUL），即使 buffer 不够也照写；
 *   - `buffer == NULL` 或 `capacity == 0` 是查询模式：只回长度，返回 COMP_OK；
 *   - 缓冲区不够时拒绝（COMP_ERR_INVALID_ARGUMENT），不静默截断；
 *   - 索引越界 -> COMP_ERR_INVALID_ARGUMENT；只读，不开事务。
 */
COMP_API comp_result comp_selection_channel_get_name(comp_document document, size_t index,
                                                     char* buffer, size_t capacity,
                                                     size_t* out_required);

/*
 * 把**当前选区**存成一个新通道（追加到末尾），`out_index` 写新下标（可为 NULL）。
 *
 *   - name_utf8 必须非空、严格 UTF-8（拒绝超长编码、代理区、截断）、无控制字符、
 *     不超过 255 字节；否则 COMP_ERR_INVALID_ARGUMENT；
 *   - 没有选区（active = 0）-> COMP_ERR_STATE；
 *   - 已达 16 个 -> COMP_ERR_STATE；
 *   - 成功是一步撤销（加入调用方的显式事务，或自成一个隐式步）；蒙版按 COW
 *     共享，不复制像素。
 */
COMP_API comp_result comp_selection_channel_save(comp_document document, const char* name_utf8,
                                                 size_t* out_index);

/*
 * 把第 index 个通道载入选区，combine 取 comp_selection_combine：
 *   - replace：基础蒙版 = 通道蒙版，羽化 = 通道羽化；
 *   - add：逐像素取最大值（没有选区时蒙版等价于 replace，羽化仍归 0）；
 *   - subtract：逐像素相减（没有选区时无操作，羽化不动）；
 *   - add / subtract 把羽化归 0（与形状叠加同口径）。
 * 结果与当前选区逐字节相同时 out_changed = 0（不产生空撤销步）。
 * 索引越界、combine 非法 -> COMP_ERR_INVALID_ARGUMENT；一步撤销；out_changed 可为 NULL。
 */
COMP_API comp_result comp_selection_channel_load(comp_document document, size_t index,
                                                 comp_selection_combine combine,
                                                 int32_t* out_changed);

/* 删除第 index 个通道。越界 -> COMP_ERR_INVALID_ARGUMENT；成功 out_changed = 1。
 * 一步撤销；out_changed 可为 NULL。 */
COMP_API comp_result comp_selection_channel_remove(comp_document document, size_t index,
                                                   int32_t* out_changed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_SELECTION_H */
