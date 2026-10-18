// AI生成
/*
 * document.h — 文档领域模型（内部 C++ 类型）。
 */
#ifndef COMPOSITOR_DOMAIN_DOCUMENT_H
#define COMPOSITOR_DOMAIN_DOCUMENT_H

#include <stdexcept>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"
#include "core/comp_alloc.h"
#include "core/comp_handle_magic.h"
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

  /*
   * 经引擎分配器创建/销毁（placement new + 手动析构；失败抛 bad_alloc）。
   * comp_document_create 与 format 层 load_project 共用这一对函数，
   * 保证「谁分配谁释放」在两条创建路径上都成立（见 Engine::create）。
   */
  static Document* create(Engine* engine, int32_t width, int32_t height, float dpi,
                          std::string color_space);
  static void destroy(Document* document);

  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  Engine* engine() const noexcept { return engine_; }

  DocumentState& state() noexcept { return state_; }
  const DocumentState& state() const noexcept { return state_; }

  /* 选区是文档状态的一部分（见 SelectionState）：事务快照、撤销/重做与
   * 回滚都自动覆盖它。这里只做转发，不另存一份。 */
  SelectionState& selection() noexcept { return state_.selection; }
  const SelectionState& selection() const noexcept { return state_.selection; }

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

  /*
   * 整篇重采样到新画布尺寸（对齐上游 ImageResizer / Image Size）：
   *   - 每层像素与掩版按画布缩放比例重采样（层的采样模式），transform 的
   *     原点与尺寸同步缩放；w/h<=0 的「按像素尺寸」分量保持未指定语义；
   *   - 旋转层（或 transform 尺寸与像素不一致的拉伸层）走光栅化：翻转、
   *     旋转与掩版烘入像素后丢弃这些标志——非均匀缩放的旋转矩形在本仓
   *     origin/size/rotation 模型里无法表示（会引入剪切），上游对此同样
   *     光栅化（rasterize each transformed layer）；
   *   - 1x1 的 uniform 掩版分辨率无关，保持原样（上游同款）；
   *   - 尺寸不变为无操作（不进历史，对齐上游短路）；
   *   - 效果参数为 JSON 透传（像素求值属滤镜图里程碑），不随尺寸缩放——
   *     已记录的能力边界，上游此时按 effectScale 缩放。
   * 一步撤销（加入宿主显式事务或自开自提交）。
   */
  bool resize_image(comp_size size, std::string* error);

  bool is_dirty() const noexcept { return dirty_; }
  void mark_dirty(bool value) noexcept { dirty_ = value; }

  /*
   * 句柄有效性（与 Engine 同一套轻量方案，见 engine.h 文件头）：
   * 构造期置 kDocumentMagic，析构期 poison。abi 层 as_document 据此
   * 把「销毁后使用 / 类型混淆 / 野指针」挡成错误码而不是静默解引用。
   */
  bool is_alive() const noexcept { return handle_magic_ == kDocumentMagic; }

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
  History history_{};
  bool dirty_ = false;

  bool txn_active_ = false;
  std::string txn_label_;
  DocumentState txn_snapshot_{};
  std::vector<void*> txn_slots_{};
  uint32_t handle_magic_ = kDocumentMagic;
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_DOCUMENT_H */
