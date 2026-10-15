// AI生成
/*
 * document.h — 文档领域模型（内部 C++ 类型）。
 */
#ifndef COMPOSITOR_DOMAIN_DOCUMENT_H
#define COMPOSITOR_DOMAIN_DOCUMENT_H

#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"
#include "domain/domain_state.h"
#include "domain/history.h"

namespace compositor {

class Engine;

class Document {
 public:
  Document(Engine* engine, int32_t width, int32_t height, float dpi, std::string color_space);

  /*
   * 析构时统一作废所有已登记的事务句柄槽位（comp_txn 持裸 Document*，
   * 文档先于事务句柄销毁时，句柄侧的 commit/abort 必须能被安全拒绝）。
   * ABI 层 Document 均为堆分配，故此析构本就是删除路径。
   */
  ~Document();

  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  Engine* engine() const noexcept { return engine_; }

  DocumentState& state() noexcept { return state_; }
  const DocumentState& state() const noexcept { return state_; }

  SelectionState& selection() noexcept { return selection_; }
  const SelectionState& selection() const noexcept { return selection_; }

  History& history() noexcept { return history_; }
  const History& history() const noexcept { return history_; }

  Layer* find(comp_layer_id id);
  const Layer* find(comp_layer_id id) const;

  /* 创建图层；成功时返回新图层指针并写入 out_id。 */
  Layer* create_layer(const comp_layer_desc& desc, comp_layer_id* out_id);

  /* 删除图层（含子树）。根级/子级引用一并清理。 */
  bool remove_layer(comp_layer_id id);

  /* 在同级中把 id 移到 before 之前（before = 0 表示移到同级末尾）。 */
  bool reorder_layer(comp_layer_id id, comp_layer_id before);

  /* ------------------------------------------------------- 合并 / 拼合 */

  /*
   * 合并计划是否可行（纯判定，不合成、不改状态）：
   *   - id 为组：组内至少一个持有像素的非组后代（Merge Group）；
   *   - id 非组：同级下方存在图层且该层不是组（Merge Down）。
   * 供外壳置灰菜单与 ABI 查询复用；携带调整/效果的图层不在此判定内
   * （见 merge_down 的边界说明）。
   */
  bool can_merge_down(comp_layer_id id) const;

  /*
   * 向下合并 / 组合并（对齐上游 ⌘E）：把 id 指向的图层按「画布呈现」合成一个
   * 像素层——混合、不透明度、蒙版、剪贴、变换烘入，结果裁剪到内容并落在锚点
   * 槽位；id 为组时合并其全部后代且组本身消失（Merge Group）。一步撤销。
   *
   * 精确性契约（渲染不变）：合并前后整篇合成逐字节一致。成立条件——
   * 被合并图层上方所有穿透组不透明度为 1（穿透组按子层逐个缩放，两层并一层
   * 后无等价单层表示；隔离组不受限）。不满足时明确失败，不做静默近似。
   * 边界：被合并图层携带调整/效果时明确失败（像素求值属滤镜图里程碑）。
   */
  bool merge_down(comp_layer_id id, std::string* error);

  /*
   * 拼合图像（对齐上游 Flatten Image）：整篇文档合成为铺满画布的单一像素层，
   * 一步撤销。边界同 merge_down。
   */
  bool flatten_image(std::string* error);

  bool is_dirty() const noexcept { return dirty_; }
  void mark_dirty(bool value) noexcept { dirty_ = value; }

  /* ------------------------------------------------------------- 事务 */

  bool txn_active() const noexcept { return txn_active_; }
  comp_result txn_begin(std::string label, std::string* error);
  comp_result txn_commit(std::string* error);
  comp_result txn_abort(std::string* error);

  /*
   * 活跃事务句柄登记（修复 txn 句柄悬垂）。
   *
   * comp_txn 句柄持有裸 Document*。若宿主在事务未结束时销毁文档，
   * 之后再 commit/abort 该句柄就会解引用已释放对象。这里让文档记住所��
   * 登记的句柄槽位，析构时统一置为失效，句柄侧即可安全拒绝。
   * 槽位类型是 void*，Document 不依赖 ABI 层的 comp_txn_handle 定义。
   */
  void register_txn_slot(void* slot);
  void unregister_txn_slot(void* slot);

  bool undo();
  bool redo();

  /* 变更辅助：在隐式事务内直接落盘新状态（供 ABI 层调用）。 */
  void commit_implicit(const std::string& label, DocumentState before);

  /* 拍平后的图层 id 列表（堆叠序）。 */
  std::vector<comp_layer_id> flatten_ids() const;

 private:
  bool remove_subtree(comp_layer_id id);

  Engine* engine_ = nullptr;
  DocumentState state_{};
  SelectionState selection_{};
  History history_{};
  bool dirty_ = false;

  bool txn_active_ = false;
  std::string txn_label_;
  DocumentState txn_snapshot_{};
  std::vector<void*> txn_slots_{};
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_DOCUMENT_H */
