// AI生成
/*
 * comp_crop.h — Crop 工具（F2）：裁剪框几何 + 提交。
 *
 * 语义唯一真源是上游 `Compositor/Document/Crop.swift` 与
 * `Compositor/IO/CanvasResizer.swift`。这里的五个函数正好是外壳需要的全部：
 *
 *   comp_crop_create_frame   拖拽中：由起止点（+ 可选比例 / Option 对称）生成框
 *   comp_crop_change_ratio   切换宽高比：保持宽度、高度由比例决定、垂直居中
 *   comp_crop_snap_rect      松手/提交前：四边取整、宽高至少 1 像素
 *   comp_crop_is_valid       框是否可提交（尺寸 / 坐标 / 像素预算）
 *   comp_crop_apply          提交：画布换尺寸 + 图层平移，**一步撤销**
 *
 * 拖拽手柄、命中测试、蚂蚁线与吸附参考线属于外壳职责，不在本 ABI 内。
 *
 * 贯穿全篇的口径：
 *   1. 坐标是**文档像素**、左上原点，与 comp_selection_* 一致；dragging 的
 *      浮点值在取整前保留亚像素，取整规则与 Swift `.rounded()` 相同（.5 远离 0）。
 *   2. 提交不重采样、不裁剪图层像素：画布换小后，画布外的图层内容仍在，
 *      只是渲染时被画布边界裁掉；再次扩大画布（Crop 向负方向扩）会露出来。
 *   3. 提交丢弃选区：换画布 = 换坐标系（上游 applyDocumentSize 重建
 *      CanvasDocument，选区不随行）。这一步与尺寸变更同属**一步**撤销。
 *   4. 提交无变化（框恰好等于当前画布）时 out_changed = 0，不产生撤销步。
 */
#ifndef COMPOSITOR_COMP_CROP_H
#define COMPOSITOR_COMP_CROP_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 浮点裁剪框。字段与 comp_rect 同义，只是拖拽阶段尚未取整，因此用 double
 * （与上游 CGFloat 同宽，避免在 1e6 量级上因 float 精度把边界挪掉半像素）。
 */
typedef struct comp_crop_rect {
  double x;
  double y;
  double width;
  double height;
} comp_crop_rect;

/*
 * 取整成整像素裁剪框（上游 CropGeometry.snapped）：
 *   1. 负宽高先标准化；
 *   2. 四条边各自 round 到最近整数；
 *   3. 宽高 = 对边之差，最小 1。
 *
 * 坐标非有限（NaN/Inf）或绝对值超过 1e7 时返回 COMP_ERR_INVALID_ARGUMENT，
 * out_rect 不被写入。
 */
COMP_API comp_result comp_crop_snap_rect(comp_engine engine, comp_crop_rect frame,
                                         comp_crop_rect* out_rect);

/*
 * 由拖拽起止点生成裁剪框（上游 CropGeometry.create）：
 *   - ratio > 0 时按宽高比修正向量；ratio <= 0 表示不约束比例；
 *   - symmetric != 0：以起点为中心向两侧等距展开（Option 拖拽），否则取
 *     起止点围成的矩形；
 *   - 输出已取整（同 comp_crop_snap_rect）。
 *
 * 坐标或比例非有限返回 COMP_ERR_INVALID_ARGUMENT。
 */
COMP_API comp_result comp_crop_create_frame(comp_engine engine, double start_x, double start_y,
                                            double end_x, double end_y, double ratio,
                                            int32_t symmetric, comp_crop_rect* out_rect);

/*
 * 切换宽高比（上游 changeCropRatio）：**保持宽度**，高度 = 宽度 / ratio，
 * 垂直居中；取整后若不再合法，则原样保留 rect（上游只在不合法时放弃赋值），
 * 此时 out_changed = 0。
 *
 * ratio 必须是有限正数，否则返回 COMP_ERR_INVALID_ARGUMENT。
 */
COMP_API comp_result comp_crop_change_ratio(comp_engine engine, comp_crop_rect rect, double ratio,
                                            comp_crop_rect* out_rect, int32_t* out_changed);

/*
 * 框是否可提交（上游 CropGeometry.valid + 本仓像素预算）：
 *   - 宽、高 1..30000；
 *   - |x|、|y| <= 1e6；
 *   - 宽 * 高 <= 1 亿像素（本仓既有画布预算，上游为 2 亿）。
 * out_valid 写 1/0。注意：本函数对“不合法”返回 COMP_OK + 0，而不是错误码。
 */
COMP_API comp_result comp_crop_is_valid(comp_engine engine, comp_crop_rect rect,
                                        int32_t* out_valid);

/*
 * 提交裁剪（上游 commitCrop → CanvasResizer.resize(contentOffset:)）：
 *   1. 先取整（同 comp_crop_snap_rect），因此可以直接传入拖拽中的浮点框；
 *   2. 校验取整后的框，不合法 → COMP_ERR_INVALID_ARGUMENT，文档不变；
 *   3. 框恰好等于当前画布（原点 0、尺寸相同）→ out_changed = 0，不产生撤销步；
 *   4. 否则所有图层 transform 原点平移 (-x, -y)，画布换成框的宽高，选区丢弃。
 *
 * 平移后若有图层 transform 越出引擎上限（原点 1e6），返回
 * COMP_ERR_INVALID_ARGUMENT 且**不改任何状态**（本仓在写入侧拒绝非法 transform）。
 *
 * 未处于显式事务时自动补一个隐式事务：整个裁剪（含选区丢弃）是一步撤销。
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_crop_apply(comp_document document, comp_crop_rect rect,
                                     int32_t* out_changed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_CROP_H */
