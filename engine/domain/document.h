// AI生成
/*
 * document.h — 文档领域模型（内部 C++ 类型）。
 */
#ifndef COMPOSITOR_DOMAIN_DOCUMENT_H
#define COMPOSITOR_DOMAIN_DOCUMENT_H

#include <cstdint>
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

  /*
   * 创建图层；成功时返回新图层指针并写入 out_id。
   *
   * 指针有效期：Layer 对象存在 state_.layers 向量里，返回的是**内部指针**。
   * 任何增删图层的操作（create_layer / remove_layer / reorder_layer 之外的
   * undo / redo / 载入工程等整体替换 state_ 的路径）都可能让向量重新分配，
   * 此前拿到的 Layer* 随即失效。规范是：**不要跨这类操作持有 Layer***，
   * 需要时按 id 调 find() 重取。ABI 层本来就用 id，只有内部代码需要留意。
   */
  Layer* create_layer(const comp_layer_desc& desc, comp_layer_id* out_id);

  /* 删除图层（含子树）。根级/子级引用一并清理。 */
  bool remove_layer(comp_layer_id id);

  /* 在同级中把 id 移到 before 之前（before = 0 表示移到同级末尾）。 */
  bool reorder_layer(comp_layer_id id, comp_layer_id before);

  /*
   * 改图层名（对齐上游图层面板的双击重命名）。name 是 UTF-8，必须非空、
   * 每字符合法且总长（字节）不超过 kMaxLayerNameBytes——名字会被 .comp
   * 原样落盘，超长/坏编码一旦入库就再也无法在宿主侧显示成一行。
   * 只改名字：不动像素、不改堆叠序、不使合成缓存失效。
   */
  static constexpr size_t kMaxLayerNameBytes = 1024;
  bool rename_layer(comp_layer_id id, const std::string& name);

  /*
   * 跨父级移动（图层面板拖拽：拖进/拖出组）。parent = 0 表示根级，
   * before = 0 表示移到目标父级的**末尾**。校验：
   *   - parent 必须存在且是组（0 例外）；
   *   - before 必须存在且与 parent 同级（与 reorder_layer 同一把尺子）；
   *   - 不许把组移进自己的子树（含 parent == id）。
   * 与 reorder_layer 的区别只有一个：允许换 parent。堆叠序的权威仍是
   * state_.layers 的向量序，成功后按新序重建新旧父级的 children 簿记。
   */
  bool move_layer(comp_layer_id id, comp_layer_id parent, comp_layer_id before);

  /*
   * 复制图层（对齐上游 Duplicate Layer，⌘J）：
   *   - 像素层：像素 / 蒙版 / 调整 / 效果 / 全部几何与混合属性深拷贝；
   *   - 组：整棵子树深拷贝（父子关系、组内剪贴引用一并重映射）。
   * 副本落在原层（组为原组）之上，同级；名字追加 " copy"（UTF-8 边界安全地
   * 截到 1024 字节上限内）。新 layer id 与内容修订号都重新分配——副本是独立内容，
   * 不能共享修订号（否则 GPU 会把原层纹理当成副本的）。一步撤销。
   */
  bool duplicate_layer(comp_layer_id id, comp_layer_id* out_id);

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
   * 合并可见（对齐上游 Merge Visible）：把所有「实际参与合成」的图层（自身可见、
   * 祖先组全部可见）合成一个像素层，落在最高参与单元的槽位；隐藏图层/隐藏组
   * 原样保留。可见组按整体烘焙，组内**隐藏**后代保留并提升到组原来的父级
   * （结构变化、渲染不变）。
   *
   * 边界：
   *   - 参与层剪贴到的基底不在参与集合里 -> 明确拒绝（剪贴因数只看基底像素、
   *     不看可见性，剪断会把画布改成另一种样子）；
   *   - 携带引擎不认识的 effect -> 明确拒绝（与 merge_down 同一口径）；
   *   - 实际贡献像素的非组图层少于 2 个 -> 空操作（out_changed = false，
   *     不进历史；返回 true）。
   * 一步撤销（加入宿主显式事务或自开自提交）。
   */
  bool merge_visible(std::string* error, comp_layer_id* out_id, bool* out_changed);

  /*
   * 反向排列：反转 parent 下的直接兄弟顺序（parent == 0 = 根级）。只重排，
   * 不改像素/几何/混合；兄弟少于 2 个为空操作（out_changed = false，不进历史）。
   * 一步撤销。
   */
  bool reverse_layers(comp_layer_id parent, std::string* error, bool* out_changed);


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
  void mark_dirty(bool value) noexcept {
    /* mark_dirty(true) 是「宿主直接改了像素但没走事务」的兜底信号，缓存随之作废；
       mark_dirty(false) 只是存盘后清标记，像素没动，没必要让缓存重算。 */
    if (value) invalidate_composite();
    dirty_ = value;
  }

  /* ------------------------------------------- 合成缓存（MERGED，S12 追加） */
  /*
   * 画布级合成结果缓存。它是**派生数据**，不属于文档状态：
   *   - 不进事务快照、不进历史、不参与 .comp 序列化；
   *   - 随时可以丢弃，丢了只会多算一次合成，不影响任何可观察结果。
   *
   * 键 = content_serial()。任何可能改变合成结果的写入都必须让序列号 +1
   * （事务提交/回滚、隐式提交、撤销/重做、图层结构增删挪、画布重采样、
   * 显式 mark_dirty(true)）——加错了只多算一次，漏加才会读到旧画面。
   *
   * 活跃事务期间缓存不得被采信（事务是中间态），这条由取值方
   * composite_document_cached 负责，不在这里强制。
   */
  uint64_t content_serial() const noexcept { return content_serial_; }
  void invalidate_composite() noexcept {
    ++content_serial_;
    composite_cache_valid_ = false;
  }
  bool composite_cache_valid() const noexcept { return composite_cache_valid_; }
  uint64_t composite_cache_serial() const noexcept { return composite_cache_serial_; }
  ImageBuffer& composite_buffer() noexcept { return composite_cache_; }
  const ImageBuffer& composite_buffer() const noexcept { return composite_cache_; }
  /* 调用方刚把「当前状态的合成结果」写进 composite_buffer()，登记缓存键。 */
  void mark_composite_cache_valid() noexcept {
    composite_cache_serial_ = content_serial_;
    composite_cache_valid_ = true;
  }

  /* ------------------------------------- 逐层内容修订号（S18 / R1 追加） */
  /*
   * 单调分配器：返回一个文档内唯一、永不复用的修订号。
   * 只增不减——回滚/撤销不让它回落（历史快照恢复的是**旧号**，见 Layer::content_revision）。
   */
  uint64_t allocate_revision() noexcept { return next_revision_++; }

  /*
   * 把某一层的修订号推进到新值并返回它（图层不存在返回 0）。
   *
   * 调用规则（与设计稿 §3.2 对应）：**在事务提交之前调用**。写路径推进的是
   * 「当前 state_ 里的这个图层」，历史栈随后压入的 after 快照才会带上新号；
   * 若在 commit 之后推进，重做恢复的 after 快照仍带旧号，缓存会读到旧内容。
   *
   * 纯结构变更（add / remove / reorder / clipping_source）不调它：单层纹理可以留，
   * 合成计划每帧从文档重建（gpu_plan），不存在需要失效的计划缓存。
   */
  uint64_t touch_layer(comp_layer_id id) noexcept;

  /* 整篇所有层换新号：画布尺寸变化（Image Size / Canvas Size / Crop / 加载）用。 */
  void touch_all_layers() noexcept;

  /*
   * 句柄有效性（与 Engine 同一套双闸门，见 engine.h 文件头）：
   * 构造期置 kDocumentMagic 并登记进共享存活表（类型标签 kDocument），
   * 析构期注销并 poison。abi 层 as_document 先查表（含类型）、再读 magic，
   * 把「销毁后使用 / 类型混淆 / 野指针」挡成错误码而不是静默解引用。
   * 类型必须在解引用之前查：handle_magic_ 在对象尾部（约 +472），拿 160 字节的
   * Engine 当 Document 读就是越界读，鸿蒙上会直接段错误（F2）。
   */
  bool is_alive() const noexcept { return handle_magic_ == kDocumentMagic; }

  /*
   * 会话内唯一的文档实例号（S19 / R2 追加）。
   *
   * 用途只有一个：GPU 的逐层纹理缓存键是 (图层 id, content_revision)，而
   * `comp_layer_id` 与修订号都只是**文档内**唯一——同一个 GPU 后端先后服务两个
   * 文档时，两张表的 (id, revision) 可能撞在一起，缓存会跨文档串纹理。后端用
   * 这个实例号判断「换文档了，整体丢弃」，比用对象地址安全（地址会被复用）。
   *
   * 进程内原子递增、永不复用；不序列化、不进 DocumentState / 历史 / 事务比较。
   */
  uint64_t instance_serial() const noexcept { return instance_serial_; }

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

  /*
   * 逐层内容修订号发号器（S18 / R1）。从 1 起，0 保留给「尚未分配」。
   * 不进 .comp、不进 DocumentState 的语义比较：它是会话内的派生数据。
   */
  uint64_t next_revision_ = 1;

  /* 进程内唯一的文档实例号（见 public 区说明）。构造函数里分配，Document 不可复制。 */
  static uint64_t allocate_instance_serial() noexcept;
  uint64_t instance_serial_ = allocate_instance_serial();

  /* 合成缓存（派生数据，见 public 区的说明）。 */
  uint64_t content_serial_ = 0;
  uint64_t composite_cache_serial_ = 0;
  bool composite_cache_valid_ = false;
  ImageBuffer composite_cache_{};

  uint32_t handle_magic_ = kDocumentMagic;
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_DOCUMENT_H */
