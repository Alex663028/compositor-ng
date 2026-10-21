// AI生成
#include "domain/history.h"

#include <set>
#include <utility>

namespace compositor {

namespace {

/* 字符串的估算堆占用：容量即实际分配量。 */
std::size_t string_bytes(const std::string& s) { return s.capacity() + sizeof(std::string); }

/* 单个像素/蒙版资产的估算占用：数据容量 + vector 头 + shared_ptr 控制块。 */
constexpr std::size_t kAssetOverheadBytes = sizeof(std::vector<uint8_t>) + 48;

void account_asset(const std::shared_ptr<std::vector<uint8_t>>& asset,
                   std::set<const void*>* seen, std::size_t* bytes) {
  if (asset == nullptr || asset->empty()) return;
  /* bytes 为 nullptr 时仅做「已见」标记（用于先登记活文档资产）。 */
  if (seen->insert(asset.get()).second && bytes != nullptr) {
    *bytes += asset->capacity() + kAssetOverheadBytes;
  }
}

void collect_state_assets(const DocumentState& state, std::set<const void*>* seen,
                          std::size_t* bytes) {
  for (const Layer& layer : state.layers) {
    account_asset(layer.pixels.pixels, seen, bytes);
    account_asset(layer.mask.pixels, seen, bytes);
  }
  /*
   * 选区蒙版（文档分辨率的 8 位灰度）与图层资产走同一本账：一次形状操作
   * 产出一张新蒙版，旧蒙版随历史快照保留——它是画布级缓冲，漏记的话
   * 一张 30000×30000 的选区就是约 900 MB 不计入预算。
   */
  account_asset(state.selection.base.pixels, seen, bytes);
}

}  // namespace

std::size_t document_state_bytes(const DocumentState& state) {
  std::size_t total = sizeof(DocumentState);
  total += string_bytes(state.color_space);
  total += string_bytes(state.document_id);
  total += state.layers.capacity() * sizeof(Layer);
  for (const Layer& layer : state.layers) {
    total += string_bytes(layer.name);
    total += string_bytes(layer.mask_source_uuid);
    /* 深拷贝口径：按像素/蒙版容量计（共享后各份 vector 仍持有数据时成立）。 */
    total += layer.pixels.pixel_data().capacity();
    total += layer.mask.pixel_data().capacity();
    total += layer.children.capacity() * sizeof(comp_layer_id) + sizeof(std::vector<comp_layer_id>);
    total += layer.adjustments.capacity() * sizeof(Adjustment);
    for (const Adjustment& adjustment : layer.adjustments) {
      total += string_bytes(adjustment.settings_json);
    }
    total += layer.effects.capacity() * sizeof(Effect);
    for (const Effect& effect : layer.effects) {
      total += string_bytes(effect.settings_json);
    }
  }
  /* 选区蒙版（深拷贝口径，与图层像素/蒙版一致）。 */
  total += state.selection.base.pixel_data().capacity();
  return total;
}

std::size_t retained_history_bytes(const DocumentState* live,
                                   const std::deque<HistoryEntry>& undo_stack,
                                   const std::deque<HistoryEntry>& redo_stack) {
  std::set<const void*> seen;
  std::size_t bytes = 0;
  /* 活文档持有的资产先登记为「已见」：后续历史中同一资产不计入（上游同口径）。 */
  if (live != nullptr) collect_state_assets(*live, &seen, nullptr);
  for (const HistoryEntry& entry : undo_stack) {
    collect_state_assets(entry.before, &seen, &bytes);
    collect_state_assets(entry.after, &seen, &bytes);
  }
  for (const HistoryEntry& entry : redo_stack) {
    collect_state_assets(entry.before, &seen, &bytes);
    collect_state_assets(entry.after, &seen, &bytes);
  }
  return bytes;
}

void History::clear() {
  undo_stack_.clear();
  redo_stack_.clear();
  truncated_ = false;
}

void History::trim() {
  /* 步数上限：只丢最旧记录（现有语义，保持不变）。 */
  while (undo_stack_.size() > limit_) {
    undo_stack_.pop_front();
    truncated_ = true;
  }

  /*
   * 内存上限（独占口径）：先淘汰最旧的撤销记录，但保留最近一条
   * （撤销能力不能归零）；撤销栈已见底仍超预算时，再淘汰最旧的重做记录。
   */
  while (bytes() > budget_bytes_ && undo_stack_.size() > 1) {
    undo_stack_.pop_front();
    truncated_ = true;
  }
  while (bytes() > budget_bytes_ && !redo_stack_.empty()) {
    redo_stack_.pop_front();
    truncated_ = true;
  }
}

void History::push(std::string label, DocumentState before, DocumentState after) {
  HistoryEntry entry;
  entry.label = std::move(label);
  entry.before = std::move(before);
  entry.after = std::move(after);

  undo_stack_.push_back(std::move(entry));

  /* 新记录使重做链失效。 */
  redo_stack_.clear();

  trim();
}

bool History::undo(DocumentState* out) {
  if (undo_stack_.empty()) return false;
  HistoryEntry entry = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  if (out != nullptr) *out = entry.before;
  redo_stack_.push_back(std::move(entry));
  return true;
}

bool History::redo(DocumentState* out) {
  if (redo_stack_.empty()) return false;
  HistoryEntry entry = std::move(redo_stack_.back());
  redo_stack_.pop_back();
  if (out != nullptr) *out = entry.after;
  undo_stack_.push_back(std::move(entry));
  trim();
  return true;
}

const std::string* History::step_label(std::size_t step) const {
  if (step == 0 || step > step_count()) return nullptr;
  if (step <= undo_stack_.size()) return &undo_stack_[step - 1].label;
  /*
   * 重做栈是 LIFO：undo() 把记录 push_back 进来，redo() 从 back 取。
   * 因此 back 是「下一步会重做的记录」，即第 undo_depth + 1 步。
   */
  const std::size_t from_back = step - undo_stack_.size() - 1;
  return &redo_stack_[redo_stack_.size() - 1 - from_back].label;
}

void History::set_limit(std::size_t limit) {
  limit_ = limit == 0 ? 200 : limit;
  trim();
}

void History::set_budget_bytes(std::size_t budget) {
  budget_bytes_ = budget == 0 ? kDefaultHistoryBudgetBytes : budget;
  trim();
}

std::size_t History::bytes() const {
  return retained_history_bytes(live_, undo_stack_, redo_stack_);
}

}  // namespace compositor
