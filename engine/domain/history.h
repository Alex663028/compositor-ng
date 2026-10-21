// AI生成
/*
 * history.h — 文档级撤销/重做栈。
 *
 * 像素资产模型（对齐上游 DocumentHistory.swift）：
 * DocumentState 内的 ImageBuffer / MaskBuffer 由 shared_ptr 持有像素，
 * 因此快照只增引用、**不复制像素**。内存上限按「历史独占字节」计——
 * 活文档仍持有的资产不计入（retained_history_bytes 用资产指针去重），
 * 与上游 retainedBytes(current:) 同口径。
 *
 * 单步成本与文档规模成正比的部分只剩 Layer 结构（几十字节/层）；
 * 步数上限之外保留内存上限作为兜底（异常大的资产集合仍会截断）。
 */
#ifndef COMPOSITOR_DOMAIN_HISTORY_H
#define COMPOSITOR_DOMAIN_HISTORY_H

#include <cstddef>
#include <deque>
#include <string>

#include "domain/domain_state.h"

namespace compositor {

/* 撤销栈默认内存上限：512 MiB（0 视为该默认值）。 */
constexpr std::size_t kDefaultHistoryBudgetBytes = 512ull * 1024ull * 1024ull;

/* 估算一份文档状态的堆占用（深拷贝口径：像素 + 蒙版 + 字符串 + 容器容量开销）。
 * 供测试与文档说明使用；撤销栈的实际记账用 retained_history_bytes（独占口径）。 */
std::size_t document_state_bytes(const DocumentState& state);

/*
 * 历史「独占字节」：undo + redo 栈中持有的像素/蒙版资产，扣除活文档
 * （live）仍在引用的部分后，按资产指针去重累加（同一资产被多步引用只算一次）。
 * 与上游 DocumentHistory.retainedBytes(current:) 同口径。
 */
std::size_t retained_history_bytes(const DocumentState* live,
                                   const std::deque<struct HistoryEntry>& undo_stack,
                                   const std::deque<struct HistoryEntry>& redo_stack);

struct HistoryEntry {
  std::string label; /* 面向 UI，如「合并图层」 */
  DocumentState before;
  DocumentState after;
};

class History {
 public:
  void clear();

  /* 绑定活文档（History 不持有它；Document 构造时设置，生命周期内地址稳定）。 */
  void set_live(const DocumentState* live) { live_ = live; }

  /* 记录一步：before 为变更前状态，after 为变更后状态。 */
  void push(std::string label, DocumentState before, DocumentState after);

  bool can_undo() const { return !undo_stack_.empty(); }
  bool can_redo() const { return !redo_stack_.empty(); }

  /* 成功时把目标状态写入 out。 */
  bool undo(DocumentState* out);
  bool redo(DocumentState* out);

  std::size_t undo_depth() const { return undo_stack_.size(); }
  std::size_t redo_depth() const { return redo_stack_.size(); }

  /* ------------------------------------------------- 历史面板（P1，2026-10-21） */
  /*
   * 面板口径：index 0 = 文档最初状态（打开/新建），index i (1..step_count())
   * = 第 i 步之后的状态。current_index() = 当前落在哪个状态上。
   * 撤销/重做不改变「第几步 = 哪条记录」的对应关系，只移动当前下标；因此面板
   * 可以拿着同一份列表在回跳前后继续用（下标只在 push 或 trim 淘汰时变化）。
   */
  std::size_t step_count() const { return undo_stack_.size() + redo_stack_.size(); }
  std::size_t current_index() const { return undo_stack_.size(); }

  /* 第 step 步（1..step_count()）的标签；step == 0 或越界返回 nullptr。 */
  const std::string* step_label(std::size_t step) const;

  /* 设置步数上限（0 视为默认 200）；超限丢弃最旧记录。 */
  void set_limit(std::size_t limit);
  std::size_t limit() const { return limit_; }

  /* 设置内存上限（0 视为默认 512 MiB）；超限按上述顺序淘汰。 */
  void set_budget_bytes(std::size_t budget);
  std::size_t budget_bytes() const { return budget_bytes_; }

  /* 当前撤销栈 + 重做栈的「独占字节」（活文档持有的资产不计；供壳层提示）。 */
  std::size_t bytes() const;

  /*
   * 是否因「步数上限」或「内存上限」淘汰过历史记录（P1-3）。
   * 一旦为真即锁定（撤销能力已不完整，撤销不到更早状态），History::clear() 复位。
   * 供壳层在状态栏如实提示「撤销历史已截断」，而不是让用户遇到莫名的撤销失效。
   */
  bool truncated() const { return truncated_; }

 private:
  /* 按步数上限与内存上限淘汰；两步都只动队首（最旧记录）。 */
  void trim();

  std::deque<HistoryEntry> undo_stack_;
  std::deque<HistoryEntry> redo_stack_;
  const DocumentState* live_ = nullptr;
  std::size_t limit_ = 200;
  std::size_t budget_bytes_ = kDefaultHistoryBudgetBytes;
  bool truncated_ = false;
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_HISTORY_H */
