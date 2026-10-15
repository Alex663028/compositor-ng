// AI生成
/*
 * comp_undo.h — 事务与撤销/重做。
 *
 * 模型：所有修改类 API 都在事务内生效。
 *   - 调用方显式 begin/commit/abort 时使用显式事务；
 *   - 未开启事务就调用修改 API 时，引擎自动补一个隐式事务（一次调用 = 一个撤销步）。
 * 撤销栈为文档级；栈深由引擎限制（M1 默认 200 步，超出丢弃最旧记录），
 * 另设内存上限（默认 512 MiB）：超限时先丢最旧的撤销记录、再丢最旧的重做记录，
 * 但始终保留最近一条撤销记录。
 */
#ifndef COMPOSITOR_COMP_UNDO_H
#define COMPOSITOR_COMP_UNDO_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 开启显式事务；label 可为 NULL（用于 UI 展示，如「合并图层」）。 */
COMP_API comp_result comp_txn_begin(comp_document document,
                                    const char* label_utf8,
                                    comp_txn* out_txn);

/* 提交事务：压入撤销栈。成功后 txn 句柄失效。 */
COMP_API comp_result comp_txn_commit(comp_txn txn);

/* 放弃事务：回滚到开启前状态。成功后 txn 句柄失效。 */
COMP_API comp_result comp_txn_abort(comp_txn txn);

/* 查询是否处于显式事务中。 */
COMP_API comp_result comp_txn_active(comp_document document, int32_t* out_active);

/* --------------------------------------------------------- 撤销 / 重做 */

COMP_API comp_result comp_document_undo(comp_document document);
COMP_API comp_result comp_document_redo(comp_document document);

COMP_API comp_result comp_document_can_undo(comp_document document, int32_t* out_can);
COMP_API comp_result comp_document_can_redo(comp_document document, int32_t* out_can);

/* 撤销栈统计：undo_depth / redo_depth 可为 NULL。 */
COMP_API comp_result comp_document_history_stats(comp_document document,
                                                 size_t* out_undo_depth,
                                                 size_t* out_redo_depth);

/* 设置撤销栈上限（0 = 使用默认 200）。 */
COMP_API comp_result comp_document_set_history_limit(comp_document document,
                                                     size_t limit);

/*
 * 设置撤销栈内存上限（字节；0 = 默认 512 MiB）。整篇快照式撤销的占用与文档规模成正比，
 * 大画布下先撞到的是内存而非步数；超限时按「先最旧撤销记录、后最旧重做记录」淘汰，
 * 且始终保留最近一条撤销记录（撤销能力不归零）。
 */
COMP_API comp_result comp_document_set_history_budget(comp_document document,
                                                      size_t budget_bytes);

/* 查询撤销栈 + 重做栈当前保留的估算字节数；out_bytes 可为 NULL。 */
COMP_API comp_result comp_document_history_bytes(comp_document document, size_t* out_bytes);

/*
 * 查询撤销历史是否已被淘汰过（步数上限或内存上限触发 trim）。
 * 1 = 已丢弃过旧记录，即「撤销不到更早状态」，外壳应据此给出提示；0 = 完整。
 * 标志在新文档或 History::clear() 后复位。
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_document_history_truncated(comp_document document,
                                                     int32_t* out_truncated);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_UNDO_H */
