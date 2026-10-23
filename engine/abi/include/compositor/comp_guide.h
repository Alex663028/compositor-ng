// AI生成
/*
 * comp_guide.h — 画布参考线（标尺拖出的对齐线）C ABI。
 *
 * 参考线是**文档状态**：它随 `.comp` 保存、随撤销/重做回退，因此归文档句柄
 * 而不是某个外壳会话。坐标是连续文档像素；轴 0 = 水平线（位置是 y），
 * 轴 1 = 垂直线（位置是 x）。参考线允许落在画布外。
 *
 * 契约：
 *   - 写操作（add / move / remove / clear）各自构成一步撤销（调用方已开显式
 *     事务时并入该事务）；值未变（changed = 0）时不产生撤销步；
 *   - 参数闸在开事务之前：轴非法 / 位置非有限 / 越界 → COMP_ERR_INVALID_ARGUMENT，
 *     不留半成品；
 *   - 读操作（count / get）纯只读，不开事务、不进撤销栈；
 *   - 索引越界 → COMP_ERR_INVALID_ARGUMENT；文档句柄无效 → COMP_ERR_INVALID_ARGUMENT；
 *   - 超过 kMaxGuides（128）→ COMP_ERR_STATE（这是容量状态，不是参数错误）。
 *
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
#ifndef COMPOSITOR_COMP_GUIDE_H
#define COMPOSITOR_COMP_GUIDE_H

#include "compositor/comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 轴。数值是 ABI 契约的一部分，不得重排。 */
#define COMP_GUIDE_HORIZONTAL 0u /* 横线：position 是 y */
#define COMP_GUIDE_VERTICAL 1u   /* 竖线：position 是 x */

/* 一条参考线（与 .comp 的 engineGuides 条目同义）。 */
typedef struct comp_guide {
  uint32_t axis;   /* COMP_GUIDE_HORIZONTAL / COMP_GUIDE_VERTICAL */
  double position; /* 文档像素，连续值，可为负或超出画布 */
} comp_guide;

/* 参考线条数。未设置过参考线的文档返回 0。 */
COMP_API comp_result comp_guide_count(comp_document document, size_t* out_count);

/* 按插入序取第 index 条（0 起）。顺序即 `.comp` 的 engineGuides 数组序。 */
COMP_API comp_result comp_guide_get(comp_document document, size_t index, comp_guide* out_guide);

/* 追加一条参考线，成功时 out_index 写新下标（可为 NULL）。 */
COMP_API comp_result comp_guide_add(comp_document document, uint32_t axis, double position,
                                    size_t* out_index);

/* 改第 index 条的位置（轴不变，重复位置合法）。 */
COMP_API comp_result comp_guide_move(comp_document document, size_t index, double position,
                                     int32_t* out_changed);

/* 删除第 index 条。 */
COMP_API comp_result comp_guide_remove(comp_document document, size_t index, int32_t* out_changed);

/* 清空全部参考线（原本为空 ⇒ out_changed = 0）。 */
COMP_API comp_result comp_guide_clear(comp_document document, int32_t* out_changed);

#ifdef __cplusplus
}
#endif

#endif /* COMPOSITOR_COMP_GUIDE_H */
