// AI生成
/*
 * crop.h — Crop 工具的几何与提交（F2）。
 *
 * 语义唯一真源是上游：
 *   - `Compositor/Document/Crop.swift`（CropGeometry + EditorSession 的裁剪命令）；
 *   - `Compositor/IO/CanvasResizer.swift`（commitCrop 最终调用的画布变更）。
 *
 * 分层与本仓其它模块一致：
 *   - 本文件只做**纯几何**（取整、拖框、比例）与**状态提交**（平移图层 + 换画布）；
 *   - 拖拽手柄、蚂蚁线、吸附参考线属于外壳，不在这里。
 *
 * 与上游逐条对齐的口径：
 *   - 取整一律 `round` 到最近的整数（Swift `.rounded()` = 半边远离零，即
 *     std::llround），两个对角边**各自**取整后再求宽高，最小 1 像素；
 *   - `create` 先按可选宽高比修正拖拽向量，再按 symmetric 展开成以起点为中心的
 *     矩形，最后取整；
 *   - `change_ratio` **保持宽度**，高度 = 宽度 / 比例，垂直居中，取整后仍须合法，
 *     不合法则原样保留（上游只在不合法时放弃赋值）；
 *   - 合法性：宽高 1..30000、原点绝对值 <= 1e6、且像素总数不超过画布预算
 *     （最后一条是本仓既有口径，见 core/comp_geometry.h 的 kMaxSurfacePixels）；
 *   - 提交：画布换成裁剪矩形尺寸，**所有图层的 transform 原点平移
 *     (-rect.x, -rect.y)**，不裁剪图层几何（渲染时自然裁到画布）；
 *   - 提交后丢弃选区：上游 commitCrop → applyDocumentSize 重新构造
 *     CanvasDocument，选区不随之带过去（换画布 = 换坐标系）。
 */
#ifndef COMPOSITOR_DOMAIN_CROP_H
#define COMPOSITOR_DOMAIN_CROP_H

#include <cstdint>

#include "compositor/comp_core.h"
#include "domain/domain_state.h"

namespace compositor {

/* 浮点裁剪框（文档像素，左上原点）。与 ABI 的 comp_crop_rect 字段同义。 */
struct CropFrame {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

/*
 * 取整成整像素裁剪框（上游 CropGeometry.snapped）：
 *   1. 先标准化（负宽高翻正）；
 *   2. 左/上/右/下四条边各自 round；
 *   3. 宽高 = 对边之差，最小 1。
 *
 * 坐标非有限、或绝对值大到无法安全落在 int32 内（> 1e7）时返回 false 且不写
 * out_rect；这类输入只可能来自宿主传坏了指针/未初始化内存，不是正常拖拽。
 */
bool crop_snap_frame(const CropFrame& frame, comp_rect* out_rect);

/*
 * 由拖拽的起止点生成裁剪框（上游 CropGeometry.create）：
 *   - ratio > 0：按宽高比修正向量。|dx| > |dy| * ratio 时修正 dy，否则修正 dx；
 *   - ratio <= 0：视为无比例约束（上游 ratio 为 nil）；
 *   - symmetric：以起点为中心向两侧展开（Option 拖拽），否则取起止点围成的矩形；
 *   - 最后取整（crop_snap_frame）。
 *
 * 无穷/NaN 坐标或比例返回 false 且不写 out_rect（非有限比例只可能是坏输入，
 * 与「没有比例」的 0 区分开）。
 */
bool crop_create_frame(double start_x, double start_y, double end_x, double end_y,
                       double ratio, bool symmetric, comp_rect* out_rect);

/* 裁剪框是否合法（上游 CropGeometry.valid + 本仓的像素预算）：
 *   - 宽、高都在 1..kMaxSide；
 *   - |x|、|y| <= kMaxTransformOrigin（1e6）；
 *   - 宽 * 高 <= kMaxSurfacePixels（1 亿；上游是 2 亿，本仓既有分歧）。 */
bool crop_is_valid(const comp_rect& rect);

/*
 * 按新宽高比调整裁剪框（上游 changeCropRatio）：
 *   - 宽度保持不变，高度 = 宽度 / ratio，垂直居中；
 *   - 取整后不合法则 out_rect = rect（上游放弃赋值）。
 *
 * ratio 非有限或 <= 0 返回 false 且不写 out_rect（上游比例来自 UI，恒为正）。
 * changed 可为 NULL；成功时写 1/0 表示 out_rect 是否与 rect 不同。
 */
bool crop_change_ratio(const comp_rect& rect, double ratio, comp_rect* out_rect, bool* changed);

/*
 * 提交裁剪（上游 commitCrop + CanvasResizer.resize(contentOffset:)）：
 *   - rect 必须是整像素且合法（先过 crop_is_valid）；
 *   - rect 恰好等于「原点 (0,0) + 当前画布尺寸」时无操作（changed = false，
 *     不产生撤销步；对齐 CanvasResizer 的尺寸与偏移都未变的短路）；
 *   - 否则所有图层 transform 原点平移 (-rect.x, -rect.y)（图层组与蒙版位置
 *     同属 transform，自然一起走），画布换成 rect 的宽高，选区丢弃；
 *   - **不裁剪图层几何**：图层可以伸出新画布，渲染时由画布边界裁掉。
 *
 * 平移后若有图层的 transform 越出 is_valid_transform 的上限（原点 1e6），
 * 返回 false 且**不改任何状态**——本仓在写入侧拒绝非法 transform，而不是等到
 * 合成/存盘时才炸（见 core/comp_geometry.h）。
 *
 * changed 可为 NULL；返回 true 时写 1/0。
 */
bool crop_apply(DocumentState* state, const comp_rect& rect, bool* changed);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_CROP_H */
