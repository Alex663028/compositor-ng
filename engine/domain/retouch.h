// AI生成
/*
 * retouch.h — 修饰类工具的像素算法（F3：Clone Stamp / Smudge / Liquify）。
 *
 * 语义唯一真源是上游：
 *   - `Compositor/Document/CloneStamp.swift`（cloneStrokeOffset / 采样与落笔）；
 *   - `Compositor/Document/SmudgeLiquify.swift`（`WarpStroke` 的 weight / pickUp /
 *     smudge / push）。
 *
 * 分层与 tool_ops 一致：
 *   - 本文件只做**图层像素空间**的纯算法：入参是已平移好的浮点折线、已取好的
 *     源快照（Clone）或工作像素（Smudge/Liquify），不碰文档、不开事务、不报错；
 *   - 文档坐标 → 图层坐标、快照来源（当前层 / 整篇合成）、参数闸、事务与 note
 *     都在 `tool_ops.cpp`。这样算法可以单独验收，也不会把两种坐标空间混在
 *     一个文件里。
 *
 * 像素口径（与 engine/imaging 全部内核一致）：
 *   - RGBA8、**预乘**、4 字节/像素、行优先、左上原点；
 *   - 双线性采样与加权都在预乘值上做（预乘空间插值才是正确合成，反预乘再插值
 *     会在 alpha 边缘出现暗边）；
 *   - 写回一律四舍五入到最近整数并夹到 0..255。
 *
 * 与上游的两处显式收敛（README §6.9 与报告里都有记录）：
 *   1. 上游 Smudge/Liquify 的 carried/scratch 缓冲是 O(diameter²)，直径由 UI
 *      滑块给到数千时内存可到 GB 级。本仓在参数闸把 warp 直径上限设为
 *      `kMaxWarpDiameter`（tool_ops.cpp），算法本身不截断直径；
 *   2. 上游 warp 的采样点来自图层图像本身（含变换后的绘制），本仓按图层像素
 *      网格解释——与 Stroke 的既有口径一致（只做原点平移，不参与缩放/旋转）。
 */
#ifndef COMPOSITOR_DOMAIN_RETOUCH_H
#define COMPOSITOR_DOMAIN_RETOUCH_H

#include <cstddef>
#include <cstdint>

namespace compositor {

/*
 * Clone Stamp 落笔（上游 `cloneStroke` 的像素部分）。
 *
 *   - `source` 是与目标**同尺寸**的 RGBA8 预乘快照，图层像素空间；快照在
 *     一笔开始时取好（当前图层原始像素或整篇合成），笔迹因此不会吃自己
 *     刚画出的像素（上游 "snapshot before the stroke" 同款）；
 *   - 源像素 = 目标像素 + (offset_x, offset_y)，offset 为整数像素（上游对
 *     source - firstPoint 取 `.rounded()`）；
 *   - `coverage_peak` = pressure × flow × opacity，已由调用方乘好并夹到 0..1；
 *     coverage 的形状与 `stamp_polyline` 完全一致（折线距离 + smoothstep 羽化）；
 *   - 源越界（含 offset 把采样推出图层）按「没有可画的内容」跳过，不落透明
 *     ——与上游 `guard let source = ... else { return }` 的语义一致，也不会在
 *     越界处把图层擦出洞。
 *
 * 返回 false 仅表示内存不足；参数非法由调用方在闸里拒绝。
 */
bool clone_stamp_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                       const uint8_t* source, int32_t offset_x, int32_t offset_y,
                       const float* points, int32_t point_count, float diameter,
                       float hardness, float coverage_peak);

/*
 * Smudge（涂抹）落笔（上游 `WarpStroke.smudge(at:)`）。
 *
 *   - `points` 是图层像素空间的折线（浮点；宿主按 `spacing = max(1, d*0.005)`
 *     采样后传进来也可以，本函数会再按上游间距插值）；
 *   - 笔芯携带 carried 缓冲：开始时从第一个点拾色，之后每个 dab 把
 *     `under + (carried - under) * weight * strength` 写回，并把结果留在 carried；
 *   - `weight` = 上游 `weight(u)`：`u <= hardness` 时为 1，否则 smoothstep；
 *     hardness 按上游夹到 0.98（避免除以 0 的硬边界）；
 *   - strength 直接乘在混合系数上，0 时逐字节不变；
 *   - 只有一个点时只拾色、不落笔（上游 append 首点不产生 dab）。
 *
 * 返回 false 仅表示内存不足。
 */
bool smudge_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                  const float* points, int32_t point_count, float diameter, float hardness,
                  float strength);

/*
 * Liquify 的 Push（向前推动）落笔（上游 `WarpStroke.push(from:to:)`）。
 *
 *   - 每个 dab 先复制「半径 + 位移余量」的旧像素到 scratch，再从 scratch 双线性
 *     采样回写：`sx = x - move.x * weight`，`move = (to - from) * strength`；
 *   - 从 scratch 而不是原地采样，避免同一次 dab 内自己吃自己（上游同款）；
 *   - 采样坐标夹在 scratch 内（上游在图像边缘同样按边界处理）；
 *   - 只有一个点时不动（首点只定位）。
 *
 * 返回 false 仅表示内存不足。
 */
bool liquify_push_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                        const float* points, int32_t point_count, float diameter, float hardness,
                        float strength);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_RETOUCH_H */
