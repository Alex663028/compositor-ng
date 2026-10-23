// AI生成
/*
 * guide.h — 画布参考线（标尺拖出的对齐线）的文档模型。
 *
 * 参考线是**文档状态**，不是壳层设置：它要进 `.comp`、要随撤销回退，
 * 因此落在 DomainState 里（与选区同一条理由，见 domain_state.h）。
 * 外壳负责画标尺、拖出参考线、吸附与显示开关；本文件只做保存与校验。
 *
 * 坐标口径：
 *   - 水平参考线（axis = kGuideHorizontal）只有 y，垂直参考线只有 x；
 *   - 位置是**文档像素**、连续值（可带小数）——缩放不为 1 时，一个设备像素
 *     对应 1/zoom 个文档像素，取整发生在交互层，不在这里；
 *   - 允许落在画布外（负数或大于宽高）：PS 允许把参考线拖到画布边缘之外，
 *     画布区域会裁掉可见部分，但位置本身仍然有效。
 *
 * 与上游的关系：上游 manifest 里也有一个 `guides` 键（ProjectStore），但其
 * 结构未在本仓文档化。本仓不猜它的形状，改用引擎自有键 `engineGuides`
 * （遵循 comp_file.h 的「引擎扩展字段加 engine 前缀」约定），保证
 * 「本引擎 → 上游 → 本引擎」不会把上游的键误读成自己的。
 */
#ifndef COMPOSITOR_DOMAIN_GUIDE_H
#define COMPOSITOR_DOMAIN_GUIDE_H

#include <cstddef>
#include <cstdint>

#include "domain/domain_state.h"

namespace compositor {

enum GuideAxis : uint32_t {
  kGuideHorizontal = 0, /* 横线：position 是 y */
  kGuideVertical = 1,   /* 竖线：position 是 x */
};

/* 合法轴的枚举（与 comp_guide.h 的 COMP_GUIDE_* 数值一致）。 */
inline bool guide_axis_valid(uint32_t axis) {
  return axis == kGuideHorizontal || axis == kGuideVertical;
}

/* 一条文档最多保存多少条参考线。128 足够日常排版，超出在写入侧拒绝，
 * 而不是静默丢弃——参考线看不见的丢失会让「为什么吸不上」无从排查。 */
constexpr size_t kMaxGuides = 128;

/* 位置绝对值上限，与图层 transform 原点同一把尺子（core/comp_geometry.h
 * 的 kMaxTransformOrigin = 1e6）。 */
constexpr double kMaxGuidePosition = 1.0e6;

/* 轴合法、位置有限且在上限内。NaN / Inf / 越界一律 false。 */
bool guide_is_valid(uint32_t axis, double position);

/* 追加一条参考线。成功写 out_index（若提供）。失败条件：
 *   - 轴非法、位置非法；
 *   - 已满 kMaxGuides。
 * 失败不修改 state。 */
bool guide_add(DocumentState* state, uint32_t axis, double position, size_t* out_index);

/* 改第 index 条的位置（轴不变）。索引越界或位置非法返回 false。
 * 位置与原来相同 ⇒ changed = 0（调用方据此不产生撤销步）。 */
bool guide_move(DocumentState* state, size_t index, double position, bool* changed);

/* 删除第 index 条。越界返回 false；成功时 changed = 1。 */
bool guide_remove(DocumentState* state, size_t index, bool* changed);

/* 清空。原本为空 ⇒ changed = 0。 */
bool guide_clear(DocumentState* state, bool* changed);

/*
 * 把 position 吸附到候选位置里最近的一个（纯几何，外壳共用）。
 * 候选由调用方给（参考线 + 画布边界 + 网格线）；tolerance 以调用方坐标单位
 * 为准（外壳通常传「8 个设备像素换算成的文档像素」）。
 * 距离相等时取先出现的候选——候选按固定顺序拼装，结果逐次可复现。
 * 返回是否有候选落在 tolerance 内；out_position 只在返回 true 时写入。
 */
bool guide_snap_to(double position, const double* candidates, size_t count, double tolerance,
                   double* out_position);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_GUIDE_H */
