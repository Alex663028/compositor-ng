// AI生成
#include "domain/document.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compositing/compositor.h"
#include "compositing/effect_eval.h"
#include "compositing/resample.h"
#include "core/comp_live_handles.h"
#include "domain/history.h"
#include "domain/layer_merge.h"
#include "domain/utf8_text.h"
#include "engine.h"

namespace compositor {

/*
 * 文档实例号发号器（S19 / R2）：进程内原子递增、永不复用。
 * 只给 GPU 逐层纹理缓存做「换文档了要整体丢弃」的判据，不参与任何文档语义。
 */
uint64_t Document::allocate_instance_serial() noexcept {
  static std::atomic<uint64_t> next_serial{1};
  return next_serial.fetch_add(1, std::memory_order_relaxed);
}

Document::Document(Engine* engine, int32_t width, int32_t height, float dpi,
                   std::string color_space)
    : engine_(engine) {
  state_.width = width;
  state_.height = height;
  state_.dpi = dpi > 0.0f ? dpi : 72.0f;
  state_.color_space = color_space.empty() ? std::string("sRGB") : std::move(color_space);
  /* 历史的内存记账按「独占字节」口径：活文档资产不计入（见 history.h）。 */
  history_.set_live(&state_);
  /* 登记为存活对象：as_document 解引用前先查本表，野句柄被挡成错误码而不是段错误。
   * 放在构造体最后：此前任何抛异常的路径都不会留下「登记了却没对象」的脏表项。 */
  register_live_handle(this, LiveHandleKind::kDocument);
}

Document::~Document() {
  /*
   * 先从存活登记表注销：此后该指针被宿主误传进来时 is_live_handle 查不到，
   * as_document 不会解引用已释放的内存（与 ~FontFace / ~Engine 同一套约定）。
   */
  unregister_live_handle(this, LiveHandleKind::kDocument);
  /*
   * 作废所有活跃事务句柄：句柄内持有的 document 裸指针即将失效，
   * 不清的话宿主在文档销毁后再 commit/abort 就是 use-after-free。
   * 槽位是 comp_txn_handle::document 字段的地址，置 nullptr 即为失效标记。
   */
  for (void* slot : txn_slots_) {
    *static_cast<Document**>(slot) = nullptr;
  }
  txn_slots_.clear();

  /* 从引擎的登记表摘掉自己（引擎可能已先析构，那时列表已空，erase 无操作）。 */
  if (engine_ != nullptr) {
    engine_->detach_document(this);
  }

  /* 句柄 poison：此后任何 as_document 命中本对象的调用都会被拒绝。 */
  handle_magic_ = kHandlePoisoned;
}

Document* Document::create(Engine* engine, int32_t width, int32_t height, float dpi,
                           std::string color_space) {
  const Allocator& alloc =
      engine != nullptr ? engine->allocator() : Allocator{};
  void* memory = alloc.allocate(sizeof(Document));
  if (memory == nullptr) throw std::bad_alloc();
  try {
    return new (memory) Document(engine, width, height, dpi, std::move(color_space));
  } catch (...) {
    alloc.release(memory);
    throw;
  }
}

void Document::destroy(Document* document) {
  if (document == nullptr) return;
  Engine* engine = document->engine();
  const Allocator alloc = engine != nullptr ? engine->allocator() : Allocator{};
  document->~Document();
  alloc.release(document);
}

void Document::register_txn_slot(void* slot) {
  if (slot == nullptr) return;
  txn_slots_.push_back(slot);
}

void Document::unregister_txn_slot(void* slot) {
  txn_slots_.erase(std::remove(txn_slots_.begin(), txn_slots_.end(), slot), txn_slots_.end());
}

Layer* Document::find(comp_layer_id id) {
  if (id == 0) return nullptr;
  for (Layer& layer : state_.layers) {
    if (layer.id == id) return &layer;
  }
  return nullptr;
}

const Layer* Document::find(comp_layer_id id) const {
  if (id == 0) return nullptr;
  for (const Layer& layer : state_.layers) {
    if (layer.id == id) return &layer;
  }
  return nullptr;
}

uint64_t Document::touch_layer(comp_layer_id id) noexcept {
  Layer* layer = find(id);
  if (layer == nullptr) return 0;
  layer->content_revision = allocate_revision();
  return layer->content_revision;
}

void Document::touch_all_layers() noexcept {
  for (Layer& layer : state_.layers) layer.content_revision = allocate_revision();
}

Layer* Document::create_layer(const comp_layer_desc& desc, comp_layer_id* out_id) {
  Layer layer;
  /*
   * 父级校验必须在消耗 id 之前完成：校验失败时既没有事务可回滚
   * （ABI 侧直接 fail，ImplicitTxn 不 commit），next_layer_id 却已经自增，
   * 造成「不可见的 id 跳号」漂移。
   */
  if (desc.parent != 0) {
    const Layer* parent = find(desc.parent);
    if (parent == nullptr || !parent->is_group) return nullptr;
  }

  layer.id = state_.next_layer_id++;
  /* S18：新图层的修订号在同一个「不可见中间态」里一起分配（id 与号都只增）。 */
  layer.content_revision = allocate_revision();
  layer.name = (desc.name != nullptr && desc.name[0] != '\0') ? desc.name : std::string();
  layer.blend = desc.blend;
  layer.opacity = std::min(1.0f, std::max(0.0f, desc.opacity));
  layer.transform = desc.transform;
  layer.rotation = desc.rotation;
  layer.flip_x = desc.flip_x != 0;
  layer.flip_y = desc.flip_y != 0;
  layer.sampling = desc.sampling;
  layer.is_group = desc.is_group != 0;
  layer.parent = desc.parent;

  if (layer.name.empty()) {
    layer.name = layer.is_group ? "Group" : "Layer";
  }

  if (!layer.is_group) {
    /* 新像素层默认铺满画布且全透明，避免出现“尺寸不确定”的中间态。
     * P0-1：格式跟随文档缺省位深——16 位文档里新建的空图层必须是 16 位，
     * 否则「文档 16 位」只是一句声明：加一个图层、落一笔就变 8 位了。 */
    const comp_pixel_format format =
        state_.pixel_format == static_cast<int32_t>(COMP_PIXEL_RGBA16) ? COMP_PIXEL_RGBA16
                                                                      : COMP_PIXEL_RGBA8;
    layer.pixels.resize(state_.width, state_.height, format);
  }

  if (layer.parent != 0) {
    Layer* parent = find(layer.parent);
    if (parent == nullptr || !parent->is_group) return nullptr;
    parent->children.push_back(layer.id);
  }

  const comp_layer_id id = layer.id;
  state_.layers.push_back(std::move(layer));
  if (state_.active_layer == 0) state_.active_layer = id;
  invalidate_composite();

  if (out_id != nullptr) *out_id = id;
  return find(id);
}

bool Document::remove_subtree(comp_layer_id id) {
  Layer* layer = find(id);
  if (layer == nullptr) return false;

  /*
   * 递归 erase 会让 state_.layers 的元素整体移动，`layer` 指针随之失效
   * （被删元素之后的槽位被前移元素覆盖，`layer->parent` 会读到别的图层）。
   * 因此先把要用到的字段按值取出，再递归。
   */
  const std::vector<comp_layer_id> children = layer->children;
  const comp_layer_id parent = layer->parent;

  for (comp_layer_id child : children) {
    remove_subtree(child);
  }

  if (parent != 0) {
    Layer* p = find(parent);
    if (p != nullptr) {
      p->children.erase(std::remove(p->children.begin(), p->children.end(), id),
                        p->children.end());
    }
  }

  for (Layer& other : state_.layers) {
    if (other.clipping_source == id) other.clipping_source = 0;
  }

  state_.layers.erase(
      std::remove_if(state_.layers.begin(), state_.layers.end(),
                     [id](const Layer& l) { return l.id == id; }),
      state_.layers.end());

  if (state_.active_layer == id) {
    state_.active_layer = state_.layers.empty() ? 0 : state_.layers.back().id;
  }
  invalidate_composite();
  return true;
}

bool Document::remove_layer(comp_layer_id id) { return remove_subtree(id); }

bool Document::reorder_layer(comp_layer_id id, comp_layer_id before) {
  Layer* layer = find(id);
  if (layer == nullptr) return false;
  if (before == id) return true;

  const comp_layer_id parent = layer->parent;

  /*
   * 锚点必须与被移动层同级。跨级锚点若不拒绝，会把层插到另一棵树内部的
   * 向量位置上，等级与堆叠序同时被破坏且无任何报错。
   */
  if (before != 0) {
    const Layer* anchor = find(before);
    if (anchor == nullptr) return false;
    if (anchor->parent != parent) return false;
  }

  /*
   * 堆叠序的权威是 state_.layers 的向量序——comp_layer.h 写明「数组序即堆叠序」，
   * 合成器（filter_graph 的 children_of）按 parent 过滤该向量决定兄弟顺序。
   * 因此组内重排也必须重排 state_.layers：只改 parent->children 簿记向量的话，
   * 命令返回成功、进历史、children 也变了，唯独画面一点不变（渲染无操作）。
   * children 向量在末尾按新序重建，保持簿记与权威序一致。
   */
  auto it = std::find_if(state_.layers.begin(), state_.layers.end(),
                         [id](const Layer& l) { return l.id == id; });
  if (it == state_.layers.end()) return false;
  Layer moved = std::move(*it);
  state_.layers.erase(it);

  auto pos = state_.layers.end();
  if (before != 0) {
    pos = std::find_if(state_.layers.begin(), state_.layers.end(),
                       [before](const Layer& l) { return l.id == before; });
    /* 锚点已被同 parent 校验保证存在；find 落 end 只可能是 before==id（已提前返回）。 */
    if (pos == state_.layers.end()) return false;
  } else if (parent != 0) {
    /*
     * before == 0 表示移到同级末尾：插到向量中最后一个同级层之后。
     * 插在中间不影响其它父级的相对次序（插入保持其余元素的相对顺序），
     * Children_of(parent) 只关心同级层之间的相对次序。
     */
    for (auto scan = state_.layers.begin(); scan != state_.layers.end(); ++scan) {
      if (scan->parent == parent) pos = scan + 1;
    }
  }
  /* before == 0 且 parent == 0（根级）：pos 保持 end()，即移到数组末尾。 */
  state_.layers.insert(pos, std::move(moved));

  /* 簿记同步：父级 children 按新的向量序重建（根级没有 children 向量）。 */
  if (parent != 0) {
    Layer* p = find(parent);
    if (p == nullptr) return false;
    p->children.clear();
    for (const Layer& l : state_.layers) {
      if (l.parent == parent) p->children.push_back(l.id);
    }
  }
  invalidate_composite();
  return true;
}

bool Document::rename_layer(comp_layer_id id, const std::string& name) {
  Layer* layer = find(id);
  if (layer == nullptr) return false;
  if (name.empty() || name.size() > kMaxLayerNameBytes) return false;
  if (!utf8_is_valid(name)) return false;
  /* 控制字符（含内嵌 NUL）一律拒绝：名字要单行显示、要进 JSON 字符串，
   * 换行或 NUL 只会让面板与 .comp 各自截断出两个不同的名字。 */
  if (!utf8_is_single_line(name)) return false;
  layer->name = name;
  /* 名字只进面板与 .comp，不参与像素求值——不动内容修订号，也不让合成缓存
   * 失效（否则改个名字就要重绘整篇）。 */
  dirty_ = true;
  return true;
}

bool Document::move_layer(comp_layer_id id, comp_layer_id parent, comp_layer_id before) {
  Layer* layer = find(id);
  if (layer == nullptr) return false;
  if (before == id) return false;
  if (parent == id) return false;

  if (parent != 0) {
    const Layer* target = find(parent);
    if (target == nullptr || !target->is_group) return false;
    /* 不许把组塞进自己的子树：从目标父级沿 parent 链上溯，遇到 id 即拒绝。
     * 不挡这一步会造出「自己是自己的祖先」的环，之后 remove/合成都会死循环。 */
    const Layer* probe = target;
    size_t guard = 0;
    while (probe != nullptr) {
      if (probe->id == id) return false;
      probe = probe->parent != 0 ? find(probe->parent) : nullptr;
      if (++guard > state_.layers.size()) return false; /* 已经是环（防御） */
    }
  }
  if (before != 0) {
    const Layer* anchor = find(before);
    if (anchor == nullptr || anchor->parent != parent) return false;
  }

  if (layer->parent == parent) {
    /* 同级移动与 reorder 同义；交给它保持两处口径一致。 */
    return reorder_layer(id, before);
  }

  const comp_layer_id old_parent = layer->parent;
  auto it = std::find_if(state_.layers.begin(), state_.layers.end(),
                         [id](const Layer& l) { return l.id == id; });
  if (it == state_.layers.end()) return false;
  Layer moved = std::move(*it);
  state_.layers.erase(it);
  moved.parent = parent;

  auto pos = state_.layers.end();
  bool positioned = false;
  if (before != 0) {
    pos = std::find_if(state_.layers.begin(), state_.layers.end(),
                       [before](const Layer& l) { return l.id == before; });
    if (pos == state_.layers.end()) return false;
    positioned = true;
  } else if (parent != 0) {
    /* 目标组末尾：插到最后一个同父兄弟之后；组还没有子层时插在组自身后面。 */
    for (auto scan = state_.layers.begin(); scan != state_.layers.end(); ++scan) {
      if (scan->parent == parent) {
        pos = scan + 1;
        positioned = true;
      }
    }
    if (!positioned) {
      auto group = std::find_if(state_.layers.begin(), state_.layers.end(),
                                [parent](const Layer& l) { return l.id == parent; });
      if (group != state_.layers.end()) {
        pos = group + 1;
        positioned = true;
      }
    }
  }
  state_.layers.insert(pos, std::move(moved));

  /* 新旧父级的 children 按权威向量序重建（同 reorder_layer 的理由）。 */
  auto rebuild = [&](comp_layer_id owner) {
    if (owner == 0) return true;
    Layer* p = find(owner);
    if (p == nullptr) return false;
    p->children.clear();
    for (const Layer& l : state_.layers) {
      if (l.parent == owner) p->children.push_back(l.id);
    }
    return true;
  };
  if (!rebuild(old_parent) || !rebuild(parent)) return false;
  invalidate_composite();
  return true;
}

bool Document::duplicate_layer(comp_layer_id id, comp_layer_id* out_id) {
  const Layer* original = find(id);
  if (original == nullptr) return false;

  /*
   * 收集整棵子树（BFS，按 children 簿记序）。组的副本必须带上全部后代：
   * 只复制组壳会得到一个空组，画面直接少掉一层——比拒绝更糟。
   */
  std::vector<comp_layer_id> subtree;
  subtree.push_back(id);
  for (size_t i = 0; i < subtree.size(); ++i) {
    const Layer* node = find(subtree[i]);
    if (node == nullptr) continue;
    for (comp_layer_id child : node->children) subtree.push_back(child);
  }

  /*
   * 先分配全部新 id / 新修订号再做引用重映射。id 在 state_.next_layer_id 上只增，
   * 修订号同理——副本是独立内容，与原件共享修订号会让 GPU 缓存把原件纹理当成
   * 副本的（见 layer.h 的 content_revision 不变量）。
   */
  std::unordered_map<comp_layer_id, comp_layer_id> remap;
  std::vector<Layer> clones;
  clones.reserve(subtree.size());
  for (comp_layer_id old_id : subtree) {
    const Layer* source = find(old_id);
    if (source == nullptr) return false;
    Layer clone = *source;
    clone.id = state_.next_layer_id++;
    clone.content_revision = allocate_revision();
    /* 链接不随副本走：复制出来的层应能单独移动（PS 同款）。 */
    clone.link_group = 0;
    remap.emplace(old_id, clone.id);
    clones.push_back(std::move(clone));
  }
  for (Layer& clone : clones) {
    auto parent_hit = remap.find(clone.parent);
    if (parent_hit != remap.end()) clone.parent = parent_hit->second;
    auto clipping_hit = remap.find(clone.clipping_source);
    if (clipping_hit != remap.end()) clone.clipping_source = clipping_hit->second;
    for (comp_layer_id& child : clone.children) {
      auto child_hit = remap.find(child);
      if (child_hit != remap.end()) child = child_hit->second;
    }
  }

  /* 副本名字追加 " copy"，并按 UTF-8 字符边界截到长度上限内（不能切半个汉字）。 */
  std::string copy_name = clones.front().name + " copy";
  if (copy_name.size() > kMaxLayerNameBytes) {
    size_t limit = kMaxLayerNameBytes;
    while (limit > 0 && (static_cast<unsigned char>(copy_name[limit]) & 0xC0u) == 0x80u) {
      --limit;
    }
    copy_name.resize(limit);
  }
  clones.front().name = copy_name;

  const comp_layer_id root_parent = clones.front().parent;
  /* 落点：原子树之后的第一个位置（同级、紧贴原件之上）。 */
  size_t insert_at = state_.layers.size();
  for (size_t i = 0; i < state_.layers.size(); ++i) {
    if (remap.find(state_.layers[i].id) != remap.end()) insert_at = i + 1;
  }
  const comp_layer_id new_root = clones.front().id;
  state_.layers.insert(state_.layers.begin() + static_cast<std::ptrdiff_t>(insert_at),
                       std::make_move_iterator(clones.begin()),
                       std::make_move_iterator(clones.end()));

  /* 父级 children 簿记：新根紧跟在原件之后（组内复制的叠放次序与原层一致）。 */
  if (root_parent != 0) {
    Layer* parent = find(root_parent);
    if (parent == nullptr) return false;
    for (size_t i = 0; i < parent->children.size(); ++i) {
      if (parent->children[i] == id) {
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                new_root);
        break;
      }
    }
  }

  invalidate_composite();
  if (out_id != nullptr) *out_id = new_root;
  return true;
}

namespace {

/* 合并前校验：携带「尚无像素求值」的图层在烘入前不参与合并（明确拒绝，
 * 不悄悄丢参数、不假装烘入——与 compositor.h 声明的能力边界一致）。
 *
 * 2026-10-16 变更：**调整（adjustments）不再阻止合并**。
 * 调整图层此前只入库不求值，因此当时按「无法烘入」拒绝；调整求值落地后
 * （compositing/adjustment_eval.cpp + filter_graph 的滤镜节点），
 * 整篇合成已经包含调整的效果，拼合/合并直接走合成结果就是正确的烘入，
 * 无需拒绝。
 *
 * 同日再变更：**效果（effects）也不再阻止合并**。效果求值落地后
 * （compositing/effect_eval.cpp），投影/发光/描边/叠色等已包含在合成结果中。
 * flatten 与 merge_down 都经由 composite_document 取结果（document.cpp:641、
 * layer_merge.cpp:54），因此效果此时确实被烘入，而不是被丢弃。
 *
 * 仅剩一种拒绝：**引擎不认识的 effect 值**。这类效果既不求值也不绘制，
 * 烘入会静默丢弃，正是 AUDIT5 §7.4 记的那类「报成功但丢参数」，必须拒绝。
 *
 * 本函数现在**只有一个拒绝条件**，保留下来因为调用点需要区分它与其它失败
 * （祖先穿透组不透明度不为 1 等，属 COMP_ERR_STATE）。ABI 层靠错误文案里是否含
 * `kUnevaluatedEffectMarker` 来区分（见 comp_layer_abi.cpp），因此该字面量在两边
 * 必须一致——它定义在 effect_eval.h，与本文件共用一处，避免文案改动后两边悄悄
 * 脱节（早期版本就是各写一个 "effects" 子串匹配，文案一改就错报错误码）。
 *
 * merge_subset_is_bakeable 保留为显式断言点：将来若引入无法烘入的特性（例如
 * GPU 独占资源、引用外部文件的链接层），这里是唯一的拦截位置。
 */
bool merge_subset_is_bakeable(const Document& doc,
                              const std::vector<comp_layer_id>& subset,
                              std::string* error) {
  for (comp_layer_id id : subset) {
    const Layer* layer = doc.find(id);
    if (layer == nullptr || layer->is_group) continue; /* 组本身不携带调整/效果 */
    if (layer->effects.empty()) continue;
    if (error != nullptr) {
      /*
       * 唯一的保留拒绝点：**引擎不认识的 effect 值**（engineEffects 里出现
       * 未来版本或第三方新增的 kind）。这类效果本引擎既不求值也不绘制，
       * 烘入合成结果会把它们静默丢弃——正是 AUDIT5 §7.4 记的那类「报成功但丢参数」。
       * 引擎已知的 kind（effect_kind_is_evaluated，当前 9 种）此时已由合成结果包含，放行。
       */
      bool all_known = true;
      for (const Effect& effect : layer->effects) {
        if (!effect_kind_is_evaluated(effect.effect)) {
          all_known = false;
          break;
        }
      }
      if (all_known) continue;
      *error = "layer '" + layer->name + "' carries an " + kUnevaluatedEffectMarker +
               "; baking it would silently drop it, merge refused";
    }
    return false;
  }
  return true;
}

bool contains_id(const std::vector<comp_layer_id>& ids, comp_layer_id id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/*
 * 合并精确性条件：id 上方（含直接父级链）的所有「穿透组」不透明度必须为 1。
 *
 * 原因：穿透组的不透明度按「每个子层」缩放（filter_graph.cpp 的 child_scale），
 * 两个子层合并为一个结果层后，组不透明度只能对结果整体应用一次——二者在数学上
 * 不等价（预乘 source-over 的结合律救不了 per-child 缩放）。隔离组无此限制：
 * 其不透明度在子树合成后整体应用，合并不改变该结构。
 *
 * 不满足时合并明确拒绝（COMP_ERR_STATE），不做「近似正确」的静默近似。
 */
bool pass_through_ancestors_are_neutral(const Document& doc, comp_layer_id id) {
  const Layer* layer = doc.find(id);
  while (layer != nullptr && layer->parent != 0) {
    const Layer* parent = doc.find(layer->parent);
    if (parent == nullptr || !parent->is_group) return false; /* 结构异常：拒绝 */
    if (!parent->isolated && parent->opacity != 1.0f) return false;
    layer = parent;
  }
  return true;
}

/* 递归收集 group 的全部后代（对 children 封闭）。 */
void collect_descendants(const Document& doc, comp_layer_id group,
                         std::vector<comp_layer_id>* out) {
  for (const Layer& layer : doc.state().layers) {
    if (layer.parent != group) continue;
    out->push_back(layer.id);
    if (layer.is_group) collect_descendants(doc, layer.id, out);
  }
}

/*
 * 「实际参与合成」= 自身可见，且整条祖先链上的每组都可见。
 *
 * 合成器对不可见的组直接返回空缓冲（filter_graph.cpp 的
 * build_isolated_group_surface），穿透组同理跳过子树——所以隐藏组里的
 * 「可见」子层不会贡献任何像素。合并可见（Merge Visible）的参与集合必须用
 * 这同一把尺子，否则会把看不见的层烘进结果。
 */
bool layer_is_effectively_visible(const Document& doc, const Layer& layer) {
  if (!layer.visible) return false;
  comp_layer_id parent = layer.parent;
  /* 结构异常（父级缺失/不是组）按不可见处理：宁可不合并，也不猜。 */
  for (int depth = 0; parent != 0; ++depth) {
    if (depth > 1024) return false;
    const Layer* group = doc.find(parent);
    if (group == nullptr || !group->is_group || !group->visible) return false;
    parent = group->parent;
  }
  return true;
}

}  // namespace

bool Document::can_merge_down(comp_layer_id id) const {
  const Layer* active = find(id);
  if (active == nullptr) return false;
  if (!pass_through_ancestors_are_neutral(*this, id)) return false;
  if (active->is_group) {
    std::vector<comp_layer_id> inside;
    collect_descendants(*this, id, &inside);
    bool has_pixels = false;
    for (comp_layer_id child : inside) {
      const Layer* layer = find(child);
      if (layer != nullptr && !layer->is_group && layer->has_pixels()) {
        has_pixels = true;
        break;
      }
    }
    if (!has_pixels) return false;
    /*
     * 与 merge_down 保持同一套判定：携带调整/效果的子集无法烘焙，
     * 若这里不查，壳层会把命令置灰为可用，执行却必然失败。
     */
    std::vector<comp_layer_id> subset{id};
    for (comp_layer_id child : inside) subset.push_back(child);
    return merge_subset_is_bakeable(*this, subset, nullptr);
  }
  const Layer* below = nullptr;
  for (const Layer& layer : state_.layers) {
    if (layer.id == id) break;
    if (layer.parent == active->parent) below = &layer;
  }
  if (below == nullptr || below->is_group) return false;
  const std::vector<comp_layer_id> subset{below->id, id};
  return merge_subset_is_bakeable(*this, subset, nullptr);
}

bool Document::merge_down(comp_layer_id id, std::string* error) {
  const Layer* active = find(id);
  if (active == nullptr) {
    if (error != nullptr) *error = "layer not found";
    return false;
  }

  /* ---- 合并计划（对齐上游 mergePlan） ---- */
  std::vector<comp_layer_id> merged; /* 被合并消失的图层（组合并时含组本身） */
  std::string name;
  std::string label;
  comp_layer_id parent = 0;

  if (active->is_group) {
    merged.push_back(id);
    collect_descendants(*this, id, &merged);
    bool has_pixels = false;
    for (comp_layer_id member : merged) {
      const Layer* layer = find(member);
      if (layer != nullptr && !layer->is_group && layer->has_pixels()) has_pixels = true;
    }
    if (!has_pixels) {
      if (error != nullptr) *error = "group has no pixel layers to merge";
      return false;
    }
    name = active->name;
    label = "Merge Group";
    parent = active->parent;
  } else {
    const Layer* below = nullptr;
    for (const Layer& layer : state_.layers) {
      if (layer.id == id) break;
      if (layer.parent == active->parent) below = &layer;
    }
    if (below == nullptr) {
      if (error != nullptr) *error = "no layer below to merge with";
      return false;
    }
    if (below->is_group) {
      if (error != nullptr) *error = "the layer below is a group; merge it via Merge Group first";
      return false;
    }
    merged = {below->id, id};
    name = below->name;
    label = "Merge Down";
    parent = active->parent;
  }

  if (!merge_subset_is_bakeable(*this, merged, error)) return false;
  if (!pass_through_ancestors_are_neutral(*this, id)) {
    if (error != nullptr) {
      *error = "a pass-through group above this layer has opacity != 100%; merging would "
               "change the canvas (per-child group opacity has no single-layer equivalent)";
    }
    return false;
  }

  /*
   * 子集 = 被合并图层本身（组合并时含组——组的缩放烘入结果，结果层落到组的
   * 父级）。父引用剪断为根：合成器拒绝孤儿节点；合并结果回填到原父级槽位，
   * 由预乘 source-over 的结合律保证「渲染不变」。
   */
  ImageBuffer canvas_composite;
  if (!composite_merge_subset(*this, merged, &canvas_composite, error)) return false;

  /* 内容裁剪（对齐上游 trimmed）；全透明回退整幅，避免退化尺寸。 */
  ImageBuffer result_pixels;
  comp_rect placement{0, 0, canvas_composite.width, canvas_composite.height};
  if (!trim_to_content(canvas_composite, &result_pixels, &placement)) {
    result_pixels = canvas_composite; /* 共享资产，无拷贝 */
  }

  Layer merged_layer;
  merged_layer.name = name;
  merged_layer.blend = COMP_BLEND_NORMAL; /* 混合已烘入 */
  merged_layer.opacity = 1.0f;            /* 不透明度已烘入 */
  merged_layer.transform = placement;
  merged_layer.visible = true;
  merged_layer.parent = parent;
  merged_layer.pixels = std::move(result_pixels);
  /* 蒙版已烘入（空蒙版 = 无约束）；调整/效果已被 bakeable 校验拒绝；
   * 剪贴引用不继承（结果层不再剪贴到任何层）。 */

  /*
   * 事务内落盘：一次合并 = 一步撤销。已有显式事务时加入该事务（由宿主提交），
   * 否则自开自提交——两种路径都只产生一条历史记录。
   *
   * id 分配放在 txn_begin 之后：否则 next_layer_id 在快照里已被自增，
   * 撤销无法把计数器退回去，且 before 快照不是真正的合并前状态。
   */
  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin(label, &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }
  merged_layer.id = state_.next_layer_id++;
  /* S18：结果层是一份新内容，必须拿新号（沿用锚点层的旧号会让 GPU 缓存把
   * 「合并前那一层」的纹理当成本层的）。同在 txn 内、commit 之前。 */
  merged_layer.content_revision = allocate_revision();

  std::vector<Layer> next;
  next.reserve(state_.layers.size());
  bool inserted = false;
  for (const Layer& layer : state_.layers) {
    if (contains_id(merged, layer.id)) {
      /* 结果层落在锚点（active/组）的槽位；被合并的其余层直接消失。 */
      if (layer.id == id && !inserted) {
        next.push_back(merged_layer);
        inserted = true;
      }
      continue;
    }
    next.push_back(layer);
  }
  if (!inserted) next.push_back(merged_layer); /* 理论不可达：锚点必在 merged 中 */

  /* 剪贴到被合并图层的其它层，改剪到结果层。 */
  for (Layer& layer : next) {
    if (layer.clipping_source != 0 && contains_id(merged, layer.clipping_source)) {
      layer.clipping_source = merged_layer.id;
    }
  }

  /*
   * 维护父级的 children 列表：被合并的孩子移除、结果层补到锚点槽位
   * （组内合并时父级是直接父组；组合并时父级是组的父级——锚点都是 id）。
   */
  if (parent != 0) {
    for (Layer& layer : next) {
      if (layer.id != parent) continue;
      std::vector<comp_layer_id> kids;
      kids.reserve(layer.children.size());
      bool placed = false;
      for (comp_layer_id child : layer.children) {
        if (contains_id(merged, child)) {
          if (child == id && !placed) {
            kids.push_back(merged_layer.id);
            placed = true;
          }
          continue;
        }
        kids.push_back(child);
      }
      if (!placed) kids.push_back(merged_layer.id);
      layer.children = std::move(kids);
    }
  }

  state_.layers = std::move(next);
  state_.active_layer = merged_layer.id;
  invalidate_composite();
  if (!owns_txn) return true;
  /*
   * commit 内部要拷贝两份整篇 DocumentState 进历史栈，分配失败时它已自行
   * 回滚到快照并返回错误码（见 txn_commit）——失败即未发生，无需再 abort。
   */
  return txn_commit(error) == COMP_OK;
}

namespace {

/* Image Size 的几何缩放：原点四舍五入；尺寸下限 1（上游 transform.isValid 同款）。 */
int32_t scaled_origin(int32_t value, double factor) {
  return static_cast<int32_t>(std::lround(static_cast<double>(value) * factor));
}

int32_t scaled_dimension(int32_t value, double factor) {
  const int32_t out = static_cast<int32_t>(std::lround(static_cast<double>(value) * factor));
  return out < 1 ? 1 : out;
}

}  // namespace

bool Document::resize_image(comp_size size, std::string* error) {
  if (!compositor::is_valid_canvas(size)) {
    if (error != nullptr) *error = "invalid canvas size";
    return false;
  }
  /* 尺寸不变：无操作，不进历史（对齐上游 ImageResizer 的同款短路）。 */
  if (size.width == state_.width && size.height == state_.height) return true;

  const double sx = static_cast<double>(size.width) / static_cast<double>(state_.width);
  const double sy = static_cast<double>(size.height) / static_cast<double>(state_.height);

  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin("Image Size", &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }

  /*
   * S18：换画布 = 换坐标系，所有层的几何/像素都可能变，整篇换号。
   * 放在循环之前而不是之后：中途重采样失败时要么 abort（快照把旧号整份恢复），
   * 要么在调用方显式事务里留下「改了一半」的状态——两种情况下新号都只会
   * 多失效一次缓存，绝不会漏失效。
   */
  touch_all_layers();

  for (Layer& layer : state_.layers) {
    if (layer.clipped_empty) continue; /* 已被裁空的层没有内容可缩放 */

    /* 原点始终按比例缩放；尺寸分量仅在显式指定时缩放（w/h<=0 = 按像素尺寸，
     * 跟随缩放后的像素自动变化，保持「未指定」语义）。 */
    layer.transform.x = scaled_origin(layer.transform.x, sx);
    layer.transform.y = scaled_origin(layer.transform.y, sy);
    if (layer.transform.w > 0) layer.transform.w = scaled_dimension(layer.transform.w, sx);
    if (layer.transform.h > 0) layer.transform.h = scaled_dimension(layer.transform.h, sy);

    /* 组与空层只有几何；children 簿记按 id 记录，不随尺寸变化。 */
    if (layer.is_group || !layer.has_pixels()) continue;

    const compositor::SamplingMode mode = compositor::sampling_from_abi(layer.sampling);
    /*
     * 本地快速路：无旋转且 transform 尺寸与像素一致（未拉伸）时，直接重采样
     * 像素缓冲——翻转是贴图几何、不影响像素内容，原样保留即可。此时
     * 「本地像素缩放 + transform 缩放」与上游「画布坐标光栅化」逐字节等价。
     */
    const bool local_path = std::fabs(layer.rotation) < 1e-9f &&
                            layer.transform.w == layer.pixels.width &&
                            layer.transform.h == layer.pixels.height;

    if (local_path) {
      const int32_t new_w = scaled_dimension(layer.pixels.width, sx);
      const int32_t new_h = scaled_dimension(layer.pixels.height, sy);
      compositor::ImageBuffer scaled;
      if (!compositor::resample_image(layer.pixels, new_w, new_h, mode, &scaled)) {
        if (owns_txn) txn_abort(nullptr);
        if (error != nullptr) *error = "layer " + layer.name + ": resample failed";
        return false;
      }
      layer.pixels = std::move(scaled);
      /* transform.w/h 已在上面按同一比例缩放，与新像素尺寸一致。 */

      /* 掩版：1x1 的 uniform 掩版分辨率无关，保持原样（上游同款）；其余按
       * 同一比例重采样，保留可编辑性（不烘入 alpha）。 */
      if (!layer.mask.empty() && !(layer.mask.width == 1 && layer.mask.height == 1)) {
        const int32_t mw = scaled_dimension(layer.mask.width, sx);
        const int32_t mh = scaled_dimension(layer.mask.height, sy);
        std::vector<uint8_t> scaled_mask;
        if (!compositor::resample_gray(layer.mask.pixel_data().data(), layer.mask.width,
                                       layer.mask.height, mw, mh, mode, &scaled_mask)) {
          if (owns_txn) txn_abort(nullptr);
          if (error != nullptr) *error = "layer " + layer.name + ": mask resample failed";
          return false;
        }
        layer.mask.width = mw;
        layer.mask.height = mh;
        layer.mask.pixels = std::make_shared<std::vector<uint8_t>>(std::move(scaled_mask));
      }
      continue;
    }

    /*
     * 光栅化路径（对齐上游 rasterize each transformed layer）：旋转层与拉伸层
     * 先按旧几何烘成「目标矩形空间」的像素单元（翻转/旋转/蒙版已烘入），再
     * 整幅重采样到新尺寸。新层是 identity 几何的纯像素层——旋转与翻转标志
     * 随之丢弃（上游对旋转层同样丢弃：新 transform 不带 rotation）。
     */
    compositor::ImageBuffer unit;
    if (!compositor::build_layer_unit(layer, mode, &unit, nullptr, error)) {
      if (owns_txn) txn_abort(nullptr);
      return false;
    }
    if (unit.empty()) continue; /* 无内容（has_pixels 已挡，防御性） */
    compositor::ImageBuffer scaled;
    if (!compositor::resample_image(unit, layer.transform.w, layer.transform.h, mode, &scaled)) {
      if (owns_txn) txn_abort(nullptr);
      if (error != nullptr) *error = "layer " + layer.name + ": resample failed";
      return false;
    }
    layer.pixels = std::move(scaled);
    layer.mask = MaskBuffer{}; /* 蒙版已烘入 alpha */
    layer.rotation = 0.0f;
    layer.flip_x = false;
    layer.flip_y = false;
  }

  state_.width = size.width;
  state_.height = size.height;
  /*
   * 换画布 = 换文档坐标系，选区蒙版（文档分辨率）随之失效。上游 ImageResizer
   * 直接构造一个新的 CanvasDocument，选区自然不存在；这里同样**丢弃**而不是
   * 缩放——缩放一张 0/255 的选区会把边缘糊成半选，而上游没有这条通路。
   * 与尺寸变更同属一步撤销：撤销后选区回来，重做后再次消失。
   */
  state_.selection = SelectionState{};
  /* 通道蒙版同样是文档分辨率，跟着旧画布一起失效（见 domain_state.h 的
   * SelectionChannel 注释）；上游换画布重建文档时 Alpha 通道也一并消失。 */
  state_.channels.clear();
  invalidate_composite();
  if (!owns_txn) return true;
  /* commit 失败时已自行回滚到快照（见 txn_commit），失败即未发生。 */
  return txn_commit(error) == COMP_OK;
}

bool Document::flatten_image(std::string* error) {
  if (state_.layers.empty()) {
    if (error != nullptr) *error = "document has no layers to flatten";
    return false;
  }
  std::vector<comp_layer_id> all;
  all.reserve(state_.layers.size());
  for (const Layer& layer : state_.layers) all.push_back(layer.id);
  if (!merge_subset_is_bakeable(*this, all, error)) return false;

  /* 拼合即整篇合成：优先复用画布级合成缓存（MERGED）。像素按共享快照接过来，
   * 之后对该层写像素会触发 COW，不会污染缓存。 */
  const ImageBuffer* view = nullptr;
  if (!composite_document_cached(*this, &view, error)) return false;
  ImageBuffer canvas = *view;

  Layer flat;
  flat.name = "Background";
  flat.blend = COMP_BLEND_NORMAL;
  flat.opacity = 1.0f;
  flat.transform = comp_rect{0, 0, canvas.width, canvas.height};
  flat.visible = true;
  flat.pixels = std::move(canvas);

  /* 事务内落盘：一次拼合 = 一步撤销（加入宿主显式事务或自开自提交）。
   * id 分配放在 txn_begin 之后，快照才是真正的拼合前状态。 */
  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin("Flatten Image", &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }
  flat.id = state_.next_layer_id++;
  /* S18：拼合结果是一份新内容 → 新号。 */
  flat.content_revision = allocate_revision();
  state_.layers.clear();
  state_.layers.push_back(std::move(flat));
  state_.active_layer = state_.layers.front().id;
  invalidate_composite();
  if (!owns_txn) return true;
  /* commit 失败（历史栈分配失败）时 txn_commit 已自行回滚，避免「报失败但文档已改」。 */
  return txn_commit(error) == COMP_OK;
}

/*
 * 合并可见（P1-2 第二批）。
 *
 * 与 merge_down 的关键区别：merge_down 只合并「相邻两层」，渲染不变靠
 * 「子集合成 == 原子集合成」；merge_visible 合并的是**全部有效可见层**，
 * 中间可能夹着隐藏层。隐藏层在合成里本就被跳过，所以把它们排除在合并集合
 * 之外不会改变画布——前提是可见层与隐藏层之间没有**剪贴**依赖：剪贴因数
 * 只看基底像素、不看可见性（composite_ops.cpp 的 clipping_factor），
 * 可见层剪贴到隐藏层时，把隐藏层排除会改变裁切结果，因此明确拒绝。
 */
bool Document::merge_visible(std::string* error, comp_layer_id* out_id, bool* out_changed) {
  if (out_id != nullptr) *out_id = 0;
  if (out_changed != nullptr) *out_changed = false;

  /*
   * 参与集合：
   *   - merged_ids：所有「有效可见」的层（组与子层都算）——合并后全部消失；
   *   - units：根级有效可见单元（可见组整体算一个），结果层落在最高单元的槽位；
   *   - subset：交给 composite_merge_subset 的图层。根级组要连同**全部后代**
   *     （含隐藏后代）一起给：隐藏后代在求值时被跳过，但剪贴因数仍要靠 find()
   *     找到基底，才与整篇合成一致。
   */
  std::vector<comp_layer_id> merged_ids;
  std::vector<comp_layer_id> units;
  std::vector<comp_layer_id> subset;
  size_t contributing = 0;
  for (const Layer& layer : state_.layers) {
    if (!layer_is_effectively_visible(*this, layer)) continue;
    merged_ids.push_back(layer.id);
    if (!layer.is_group) ++contributing;
    if (layer.parent != 0) continue; /* 由根级组单元覆盖 */
    units.push_back(layer.id);
    if (layer.is_group) {
      subset.push_back(layer.id);
      collect_descendants(*this, layer.id, &subset);
    } else {
      subset.push_back(layer.id);
    }
  }

  /* 只有一个（或零个）实际贡献像素的图层：合并没有任何可做的，按空操作处理。 */
  if (contributing < 2) return true;

  /*
   * 剪贴的参与集合校验：参与合并的层若剪贴到**不参与**的基底（唯一可能是
   * 根级可见层剪贴到根级隐藏层），合并会把剪贴关系剪断，画面随之改变——
   * 明确拒绝，不做近似。
   */
  for (comp_layer_id id : merged_ids) {
    const Layer* layer = find(id);
    if (layer == nullptr || layer->clipping_source == 0) continue;
    if (!contains_id(subset, layer->clipping_source)) {
      if (error != nullptr) {
        *error = "visible layer '" + layer->name +
                 "' is clipped to a hidden layer; merging would change the canvas";
      }
      return false;
    }
  }

  if (!merge_subset_is_bakeable(*this, merged_ids, error)) return false;

  ImageBuffer canvas_composite;
  if (!composite_merge_subset(*this, subset, &canvas_composite, error)) return false;

  /* 内容裁剪（与 merge_down 同款）；全透明回退整幅，避免退化尺寸。 */
  ImageBuffer result_pixels;
  comp_rect placement{0, 0, canvas_composite.width, canvas_composite.height};
  if (!trim_to_content(canvas_composite, &result_pixels, &placement)) {
    result_pixels = canvas_composite;
  }

  const Layer* anchor = find(units.back());
  if (anchor == nullptr) {
    if (error != nullptr) *error = "merge anchor layer vanished";
    return false;
  }

  Layer merged_layer;
  merged_layer.name = anchor->name;       /* PS 保留最高可见层的名字 */
  merged_layer.blend = COMP_BLEND_NORMAL; /* 混合已烘入 */
  merged_layer.opacity = 1.0f;            /* 不透明度已烘入 */
  merged_layer.transform = placement;
  merged_layer.visible = true;
  merged_layer.parent = 0;
  merged_layer.pixels = std::move(result_pixels);

  /* id 分配放在 txn_begin 之后：快照必须记的是合并前状态（与 merge_down 同款）。 */
  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin("Merge Visible", &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }
  merged_layer.id = state_.next_layer_id++;
  merged_layer.content_revision = allocate_revision();

  std::vector<Layer> next;
  next.reserve(state_.layers.size());
  bool inserted = false;
  for (const Layer& layer : state_.layers) {
    if (layer.id == anchor->id) {
      next.push_back(merged_layer);
      inserted = true;
      continue;
    }
    if (contains_id(merged_ids, layer.id)) continue;
    Layer keep = layer;
    /*
     * 隐藏后代原本挂在某个被合并的组下面：组消失后逐级上溯，挂到最近一个
     * 仍然存在的祖先（通常是根级）。它们不可见，所以只影响面板结构。
     */
    if (keep.parent != 0 && contains_id(merged_ids, keep.parent)) {
      comp_layer_id ancestor = keep.parent;
      while (ancestor != 0 && contains_id(merged_ids, ancestor)) {
        const Layer* group = find(ancestor);
        ancestor = group == nullptr ? 0 : group->parent;
      }
      keep.parent = ancestor;
    }
    next.push_back(std::move(keep));
  }
  if (!inserted) next.push_back(merged_layer); /* 理论不可达：anchor 必在 merged_ids 中 */

  /* 剪贴到被合并图层的其它层，改剪到结果层（与 merge_down 同款）。 */
  for (Layer& layer : next) {
    if (layer.clipping_source != 0 && contains_id(merged_ids, layer.clipping_source)) {
      layer.clipping_source = merged_layer.id;
    }
  }

  /*
   * children 簿记一次性重建：合并 + 隐藏后代提升会同时改动多个父级，
   * 增量维护（像 merge_down 那样逐父改）在这里容易漏掉被提升的那几支。
   * 向量序是权威，children 只是缓存。
   */
  for (Layer& layer : next) {
    if (layer.is_group) layer.children.clear();
  }
  for (const Layer& layer : next) {
    if (layer.parent == 0) continue;
    for (Layer& group : next) {
      if (group.id != layer.parent) continue;
      group.children.push_back(layer.id);
      break;
    }
  }

  state_.layers = std::move(next);
  state_.active_layer = merged_layer.id;
  invalidate_composite();
  if (out_id != nullptr) *out_id = merged_layer.id;
  if (out_changed != nullptr) *out_changed = true;
  if (!owns_txn) return true;
  return txn_commit(error) == COMP_OK;
}

bool Document::reverse_layers(comp_layer_id parent, std::string* error, bool* out_changed) {
  if (out_changed != nullptr) *out_changed = false;
  if (parent != 0) {
    const Layer* group = find(parent);
    if (group == nullptr) {
      if (error != nullptr) *error = "layer not found";
      return false;
    }
    if (!group->is_group) {
      if (error != nullptr) *error = "parent is not a layer group";
      return false;
    }
  }

  /* 向量序是权威：只翻转该父级兄弟所占的**槽位**，其它层的位置不动。 */
  std::vector<size_t> slots;
  for (size_t i = 0; i < state_.layers.size(); ++i) {
    if (state_.layers[i].parent == parent) slots.push_back(i);
  }
  if (slots.size() < 2) return true; /* 空操作，不进历史 */

  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin("Reverse Layer Order", &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }

  std::vector<Layer> picked;
  picked.reserve(slots.size());
  for (size_t slot : slots) picked.push_back(std::move(state_.layers[slot]));
  std::reverse(picked.begin(), picked.end());
  for (size_t k = 0; k < slots.size(); ++k) state_.layers[slots[k]] = std::move(picked[k]);

  /* 组的 children 缓存与向量序保持一致（根级没有 children 簿记）。 */
  if (parent != 0) {
    for (Layer& layer : state_.layers) {
      if (layer.id != parent) continue;
      std::reverse(layer.children.begin(), layer.children.end());
      break;
    }
  }

  invalidate_composite();
  if (out_changed != nullptr) *out_changed = true;
  if (!owns_txn) return true;
  return txn_commit(error) == COMP_OK;
}

bool Document::link_layers(const std::vector<comp_layer_id>& ids, uint32_t* out_group,
                           std::string* error, bool* out_changed) {
  if (out_changed != nullptr) *out_changed = false;
  if (out_group != nullptr) *out_group = 0;
  if (ids.size() < 2) {
    if (error != nullptr) *error = "linking requires at least two layers";
    return false;
  }

  /* 先全量校验再动状态：不给「一半改完才报错」留口子。 */
  bool same_group = true;
  uint32_t chosen = 0;                 /* 参与链接的既有组里编号最小的那个 */
  std::vector<uint32_t> involved;      /* 要被并进来的既有组 */
  for (size_t i = 0; i < ids.size(); ++i) {
    const Layer* layer = find(ids[i]);
    if (layer == nullptr) {
      if (error != nullptr) *error = "layer not found";
      return false;
    }
    if (layer->is_group) {
      if (error != nullptr) *error = "layer groups cannot be linked";
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (ids[j] == ids[i]) {
        if (error != nullptr) *error = "duplicate layer id";
        return false;
      }
    }
    if (layer->link_group == 0) {
      same_group = false;
      continue;
    }
    if (chosen == 0 || layer->link_group < chosen) chosen = layer->link_group;
    if (std::find(involved.begin(), involved.end(), layer->link_group) == involved.end()) {
      involved.push_back(layer->link_group);
    }
  }
  /* 只要牵涉两个及以上既有组（或还有未链接层），就不是「已在同一组」。 */
  if (involved.size() > 1) same_group = false;
  if (same_group) {
    /* 已在同一组：空操作，不进历史。 */
    if (out_group != nullptr) *out_group = chosen;
    return true;
  }

  /* 组号：优先复用参与组里最小的那个；全未链接则取现有最大值 +1
   * （不复用空洞，避免「旧引用误连新组」）。 */
  uint32_t target = chosen;
  if (target == 0) {
    target = 1;
    for (const Layer& layer : state_.layers) {
      if (layer.link_group >= target) target = layer.link_group + 1;
    }
  }

  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin("Link Layers", &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }
  /* 合并既有组：被卷入的其它组的原有成员一并改投 target，不把原组拆成两半。 */
  for (Layer& layer : state_.layers) {
    if (layer.link_group == 0 || layer.link_group == target) continue;
    if (std::find(involved.begin(), involved.end(), layer.link_group) == involved.end()) continue;
    layer.link_group = target;
  }
  for (comp_layer_id id : ids) {
    Layer* layer = find(id);
    if (layer != nullptr) layer->link_group = target;
  }
  /* 纯关系元数据：不 invalidate 合成、不 touch_layer。 */
  if (out_group != nullptr) *out_group = target;
  if (out_changed != nullptr) *out_changed = true;
  if (!owns_txn) return true;
  return txn_commit(error) == COMP_OK;
}

bool Document::unlink_layers(const std::vector<comp_layer_id>& ids, std::string* error,
                             bool* out_changed) {
  if (out_changed != nullptr) *out_changed = false;
  if (ids.empty()) {
    if (error != nullptr) *error = "no layers given";
    return false;
  }

  bool any_linked = false;
  for (size_t i = 0; i < ids.size(); ++i) {
    const Layer* layer = find(ids[i]);
    if (layer == nullptr) {
      if (error != nullptr) *error = "layer not found";
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (ids[j] == ids[i]) {
        if (error != nullptr) *error = "duplicate layer id";
        return false;
      }
    }
    if (layer->link_group != 0) any_linked = true;
  }
  if (!any_linked) return true; /* 空操作，不进历史 */

  const bool owns_txn = !txn_active_;
  std::string txn_error;
  if (owns_txn && txn_begin("Unlink Layers", &txn_error) != COMP_OK) {
    if (error != nullptr) *error = txn_error;
    return false;
  }
  for (comp_layer_id id : ids) {
    Layer* layer = find(id);
    if (layer != nullptr) layer->link_group = 0;
  }
  if (out_changed != nullptr) *out_changed = true;
  if (!owns_txn) return true;
  return txn_commit(error) == COMP_OK;
}

comp_result Document::txn_begin(std::string label, std::string* error) {
  if (txn_active_) {
    if (error != nullptr) *error = "transaction already active";
    return COMP_ERR_STATE;
  }
  /*
   * 先拷快照再置 active：快照拷贝（整篇 DocumentState）在内存压力下会抛
   * bad_alloc。若先置 active 再拷，异常逃出后 txn_active_ 永远为真——
   * 后续 txn_begin 全部被拒、undo/redo 全部被拒，宿主只能销毁文档。
   * 拷贝失败时零状态变动，异常由 ABI 护栏转成错误码即可。
   */
  DocumentState snapshot = state_;
  txn_active_ = true;
  txn_label_ = std::move(label);
  txn_snapshot_ = std::move(snapshot);
  /* 事务一开始缓存就不可再采信：事务内看到的是中间态，提交/回滚都可能换掉整份状态。 */
  invalidate_composite();
  return COMP_OK;
}

comp_result Document::txn_commit(std::string* error) {
  if (!txn_active_) {
    if (error != nullptr) *error = "no active transaction";
    return COMP_ERR_STATE;
  }
  txn_active_ = false;
  /* 提交后状态已定型，但缓存记的是事务前/中间态：作废，下次渲染重算。 */
  invalidate_composite();
  /*
   * 压历史可能抛 bad_alloc（两份整篇状态进撤销栈）。此前异常直接逃出：
   * 状态已改、事务已关闭、历史里却没有这条记录——「报失败但改动生效且撤不回」，
   * 与 P1-5 同型的半完成操作。这里回滚到事务前快照，保证「失败即未发生」。
   * 回滚用 std::swap（vector/string 的移动语义 noexcept），回滚路径本身不再抛。
   */
  try {
    history_.push(txn_label_.empty() ? std::string("Edit") : txn_label_, txn_snapshot_, state_);
  } catch (const std::bad_alloc&) {
    std::swap(state_, txn_snapshot_);
    txn_snapshot_ = DocumentState{};
    txn_label_.clear();
    if (error != nullptr) *error = "out of memory while recording history; transaction rolled back";
    return COMP_ERR_OUT_OF_MEMORY;
  } catch (const std::exception& ex) {
    std::swap(state_, txn_snapshot_);
    txn_snapshot_ = DocumentState{};
    txn_label_.clear();
    if (error != nullptr) *error = ex.what();
    return COMP_ERR_INTERNAL;
  }
  txn_snapshot_ = DocumentState{};
  txn_label_.clear();
  dirty_ = true;
  return COMP_OK;
}

comp_result Document::txn_abort(std::string* error) {
  if (!txn_active_) {
    if (error != nullptr) *error = "no active transaction";
    return COMP_ERR_STATE;
  }
  txn_active_ = false;
  /* 回滚 = 换掉整份状态，缓存同样作废。 */
  invalidate_composite();
  /*
   * 与 txn_commit 的回滚同一把尺子：swap 是 noexcept 的，且把「待回滚的新状态」
   * 装进 txn_snapshot_ 后随临时量销毁——不用可能抛 bad_alloc 的拷贝赋值。
   */
  std::swap(state_, txn_snapshot_);
  txn_snapshot_ = DocumentState{};
  txn_label_.clear();
  return COMP_OK;
}

void Document::commit_implicit(const std::string& label, DocumentState before) {
  history_.push(label, std::move(before), state_);
  dirty_ = true;
  invalidate_composite();
}

bool Document::undo() {
  /*
   * 活动事务内拒绝：undo()只换 state_，txn_snapshot_ 仍是事务前的旧状态，
   * 随后的 commit 会把「事务前快照 / 被撤销后的状态」压成一条历史，
   * 撤销链从此错位。事务整体回退请用 txn_abort。
   */
  if (txn_active_) return false;
  DocumentState restored;
  if (!history_.undo(&restored)) return false;
  state_ = std::move(restored);
  dirty_ = true;
  invalidate_composite();
  return true;
}

bool Document::redo() {
  if (txn_active_) return false;
  DocumentState restored;
  if (!history_.redo(&restored)) return false;
  state_ = std::move(restored);
  dirty_ = true;
  invalidate_composite();
  return true;
}

std::vector<comp_layer_id> Document::flatten_ids() const {
  std::vector<comp_layer_id> ids;
  ids.reserve(state_.layers.size());
  for (const Layer& layer : state_.layers) {
    ids.push_back(layer.id);
  }
  return ids;
}

}  // namespace compositor
