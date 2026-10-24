// AI生成
/*
 * test_domain.cpp — 领域层：图像缓冲、图层、文档、历史/撤销。
 */
#include <algorithm>
#include <string>
#include <vector>

#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "test_framework.h"

using namespace compositor;

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

int index_of(const std::vector<comp_layer_id>& ids, comp_layer_id id) {
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

COMP_TEST(image_buffer_layout) {
  ImageBuffer buffer = ImageBuffer::solid(3, 2, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  COMP_CHECK(buffer.width == 3 && buffer.height == 2);
  COMP_CHECK(buffer.stride() == 12);
  COMP_CHECK(buffer.byte_size() == 24);
  COMP_CHECK(buffer.pixel_data().size() == 24);
  COMP_CHECK(buffer.row(1)[0] == 255 && buffer.row(1)[3] == 255);

  buffer.clear_transparent();
  COMP_CHECK(buffer.row(0)[0] == 0 && buffer.row(0)[3] == 0);

  /*
   * fill 的入参是**直通色**，缓冲是**预乘**的：半透明必须预乘。
   * (10*40+127)/255=2、(20*40+127)/255=3、(30*40+127)/255=5。
   * 不预乘会得到 rgb=(10,20,30) alpha=40 的「比 alpha 还亮」的预乘像素，
   * 剪贴栈的 extract/restore 与非 normal 混合都会因此偏亮。
   */
  buffer.fill(10, 20, 30, 40);
  COMP_CHECK(buffer.row(1)[0] == 2 && buffer.row(1)[1] == 3);
  COMP_CHECK(buffer.row(1)[2] == 5 && buffer.row(1)[3] == 40);

  /* 不透明像素不受影响（预乘系数为 255）。 */
  buffer.fill(10, 20, 30, 255);
  COMP_CHECK(buffer.row(1)[0] == 10 && buffer.row(1)[1] == 20);
  COMP_CHECK(buffer.row(1)[2] == 30 && buffer.row(1)[3] == 255);

  buffer.resize(0, 0, COMP_PIXEL_RGBA8);
  COMP_CHECK(buffer.empty());
}

COMP_TEST(layer_effective_bounds) {
  Layer layer;
  layer.pixels = ImageBuffer::solid(8, 4, COMP_PIXEL_RGBA8, 0, 0, 0, 0);
  /* transform 的 w/h <= 0 时回落到像素尺寸。 */
  layer.transform = comp_rect{0, 0, 0, 0};
  comp_rect bounds = layer.effective_bounds();
  COMP_CHECK(bounds.w == 8 && bounds.h == 4);

  layer.transform = comp_rect{5, 6, 0, 0};
  bounds = layer.effective_bounds();
  COMP_CHECK(bounds.x == 5 && bounds.y == 6 && bounds.w == 8 && bounds.h == 4);

  layer.transform = comp_rect{1, 2, 3, 3};
  bounds = layer.effective_bounds();
  COMP_CHECK(bounds.x == 1 && bounds.y == 2 && bounds.w == 3 && bounds.h == 3);
}

COMP_TEST(document_layer_management) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");

  const size_t base_count = document.state().layers.size();

  comp_layer_desc desc{};
  desc.name = "Layer A";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 16, 16};
  comp_layer_id a = 0;
  Layer* layer_a = document.create_layer(desc, &a);
  COMP_CHECK(layer_a != nullptr);
  COMP_CHECK(a != 0);
  COMP_CHECK(document.state().layers.size() == base_count + 1);

  desc.name = "Layer B";
  comp_layer_id b = 0;
  Layer* layer_b = document.create_layer(desc, &b);
  COMP_CHECK(layer_b != nullptr && b != a);

  /*
   * 上面这次 create 往 state_.layers 尾部追加，可能触发向量重新分配：先前拿到的
   * layer_a 已经指向被释放的旧块（ASan 在鸿蒙模拟器上抓到的 heap-use-after-free，
   * 写位置在旧块 +144 的 ImageBuffer 成员）。图层指针不跨「增删图层」持有，
   * 按 id 重取——这也是宿主侧该遵守的同一条规则（见 document.h 的 create_layer）。
   */
  layer_a = document.find(a);
  COMP_CHECK(layer_a != nullptr);

  /* 新图层默认继承画布尺寸的像素缓冲（由 create_layer 保证），此处显式写入。 */
  layer_a->pixels = ImageBuffer::solid(16, 16, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  layer_b->pixels = ImageBuffer::solid(16, 16, COMP_PIXEL_RGBA8, 0, 0, 255, 255);

  std::vector<comp_layer_id> order = document.flatten_ids();
  COMP_CHECK(index_of(order, a) >= 0 && index_of(order, b) >= 0);
  COMP_CHECK(index_of(order, a) < index_of(order, b));

  /* 同层重排：把 b 移到 a 之前。 */
  COMP_CHECK(document.reorder_layer(b, a));
  order = document.flatten_ids();
  COMP_CHECK(index_of(order, b) < index_of(order, a));

  COMP_CHECK(document.remove_layer(a));
  order = document.flatten_ids();
  COMP_CHECK(index_of(order, a) < 0);
  COMP_CHECK(index_of(order, b) >= 0);
}

COMP_TEST(document_group_subtree_removal) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");

  comp_layer_desc group_desc{};
  group_desc.name = "Group";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.is_group = 1;
  comp_layer_id group_id = 0;
  Layer* group = document.create_layer(group_desc, &group_id);
  COMP_CHECK(group != nullptr && group->is_group);

  comp_layer_desc child_desc{};
  child_desc.name = "Child";
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.parent = group_id;
  comp_layer_id child_id = 0;
  Layer* child = document.create_layer(child_desc, &child_id);
  COMP_CHECK(child != nullptr);
  COMP_CHECK(document.find(group_id)->children.size() == 1);

  const size_t before = document.state().layers.size();
  COMP_CHECK(document.remove_layer(group_id));
  COMP_CHECK(document.state().layers.size() == before - 2);
  COMP_CHECK(document.find(child_id) == nullptr);
}

COMP_TEST(document_history_implicit_commit) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc desc{};
  desc.name = "History Probe";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 4, 4};

  const DocumentState before = document.state();
  comp_layer_id id = 0;
  document.create_layer(desc, &id);
  document.commit_implicit("Add Layer", before);

  COMP_CHECK(document.history().undo_depth() == 1);
  COMP_CHECK(document.history().can_undo());
  COMP_CHECK(!document.history().can_redo());

  COMP_CHECK(document.undo());
  COMP_CHECK(document.find(id) == nullptr);
  COMP_CHECK(document.history().can_redo());

  COMP_CHECK(document.redo());
  COMP_CHECK(document.find(id) != nullptr);
}

COMP_TEST(document_history_explicit_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc desc{};
  desc.name = "Txn Probe";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id id = 0;
  Layer* layer = document.create_layer(desc, &id);
  COMP_CHECK(layer != nullptr);

  const float original_opacity = layer->opacity;

  std::string error;
  COMP_CHECK(document.txn_begin("Set Opacity", &error) == COMP_OK);
  COMP_CHECK(document.txn_active());
  document.find(id)->opacity = 0.25f;
  COMP_CHECK(document.txn_commit(&error) == COMP_OK);
  COMP_CHECK(!document.txn_active());
  COMP_CHECK(document.history().undo_depth() == 1);

  COMP_CHECK(document.undo());
  COMP_CHECK_MSG(document.find(id)->opacity == original_opacity, "undo should restore opacity");

  COMP_CHECK(document.redo());
  COMP_CHECK_MSG(document.find(id)->opacity == 0.25f, "redo should reapply opacity");
}

COMP_TEST(document_transaction_abort_restores_state) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc desc{};
  desc.name = "Abort Probe";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id id = 0;
  document.create_layer(desc, &id);

  const size_t layers_before = document.state().layers.size();
  const float opacity_before = document.find(id)->opacity;

  std::string error;
  COMP_CHECK(document.txn_begin("Abandon", &error) == COMP_OK);
  document.find(id)->opacity = 0.1f;
  comp_layer_desc extra{};
  extra.name = "Discarded";
  extra.blend = COMP_BLEND_NORMAL;
  extra.opacity = 1.0f;
  comp_layer_id discarded = 0;
  document.create_layer(extra, &discarded);
  COMP_CHECK(document.txn_abort(&error) == COMP_OK);

  COMP_CHECK(document.state().layers.size() == layers_before);
  COMP_CHECK(document.find(id)->opacity == opacity_before);
  COMP_CHECK(document.find(discarded) == nullptr);
  COMP_CHECK(document.history().undo_depth() == 0);
}

COMP_TEST(history_limit_and_clear) {
  History history;
  COMP_CHECK(history.limit() == 200);
  history.set_limit(3);
  COMP_CHECK(history.limit() == 3);

  DocumentState state{};
  for (int i = 0; i < 5; ++i) {
    history.push("step", state, state);
  }
  COMP_CHECK(history.undo_depth() == 3);

  history.clear();
  COMP_CHECK(history.undo_depth() == 0);
  COMP_CHECK(!history.can_undo());
}

/*
 * P1 历史面板：记录在撤销/重做之间「步号 → 标签」的对应关系必须稳定。
 * 重做栈是 LIFO，所以这里重点验撤销两步后第 1..3 步仍报 A/B/C（而不是倒序）。
 */
COMP_TEST(history_step_labels_follow_undo_and_redo) {
  History history;
  DocumentState state{};
  history.push("A", state, state);
  history.push("B", state, state);
  history.push("C", state, state);

  COMP_CHECK(history.step_count() == 3);
  COMP_CHECK(history.current_index() == 3);
  /* index 0 是最初状态，没有标签；越界也不给。 */
  COMP_CHECK(history.step_label(0) == nullptr);
  COMP_CHECK(history.step_label(4) == nullptr);
  COMP_CHECK(history.step_label(1) != nullptr && *history.step_label(1) == "A");
  COMP_CHECK(history.step_label(2) != nullptr && *history.step_label(2) == "B");
  COMP_CHECK(history.step_label(3) != nullptr && *history.step_label(3) == "C");

  COMP_CHECK(history.undo(nullptr));
  COMP_CHECK(history.undo(nullptr));
  COMP_CHECK(history.current_index() == 1);
  COMP_CHECK(history.step_count() == 3);
  /* 撤销不改记录顺序：B、C 现在在重做栈里，但步号不变。 */
  COMP_CHECK(history.step_label(2) != nullptr && *history.step_label(2) == "B");
  COMP_CHECK(history.step_label(3) != nullptr && *history.step_label(3) == "C");

  COMP_CHECK(history.redo(nullptr));
  COMP_CHECK(history.current_index() == 2);
  COMP_CHECK(history.step_label(3) != nullptr && *history.step_label(3) == "C");

  /* 新记录丢弃重做链：步数收缩，越界标签消失。 */
  history.push("D", state, state);
  COMP_CHECK(history.step_count() == 3);
  COMP_CHECK(history.step_label(3) != nullptr && *history.step_label(3) == "D");
  COMP_CHECK(history.step_label(4) == nullptr);

  history.clear();
  COMP_CHECK(history.step_count() == 0);
  COMP_CHECK(history.step_label(1) == nullptr);
}

COMP_TEST(history_byte_budget_evicts_oldest) {
  History history;
  COMP_CHECK(history.budget_bytes() == kDefaultHistoryBudgetBytes);

  /*
   * 口径说明（2026-10-15 资产化改造后重写）：
   * History::bytes() 只统计像素/蒙版的「独占资产」——按指针去重，
   * 同一份资产被 before/after 或多条记录共享时只计一次
   * （对齐上游 DocumentHistory.swift 的独占字节账本）。
   * 空状态没有任何资产 ⇒ 字节为 0 ⇒ 预算算术退化成 0（会被重置为
   * 默认值），所以本用例必须注入真实像素才有记账意义。
   */
  auto make_state = [](uint8_t seed) {
    DocumentState state{};
    Layer layer;
    layer.id = 1;
    layer.name = "bg";
    layer.pixels = ImageBuffer::solid(64, 64, COMP_PIXEL_RGBA8, seed, seed, seed, 255);
    state.layers.push_back(layer);
    return state;
  };
  const DocumentState first = make_state(10);
  const DocumentState second = make_state(200);

  history.push("first", first, first);
  const std::size_t entry_bytes = history.bytes();
  /* before/after 是同一状态的拷贝：共享资产只计一份，但必须非零。 */
  COMP_CHECK(entry_bytes > 0);

  /* 预算只容得下一条：新记录（不同资产）挤掉最旧记录，但撤销能力不归零。 */
  history.set_budget_bytes(entry_bytes + entry_bytes / 2);
  history.push("second", second, second);
  COMP_CHECK(history.undo_depth() == 1);
  COMP_CHECK(history.bytes() == entry_bytes);

  /* 预算过小：保留最近一条撤销记录，先丢重做栈。 */
  history.set_budget_bytes(entry_bytes / 2);
  COMP_CHECK(history.undo_depth() == 1);
  COMP_CHECK(history.can_undo());
  COMP_CHECK(history.bytes() == entry_bytes);

  /* 0 恢复为默认预算。 */
  history.set_budget_bytes(0);
  COMP_CHECK(history.budget_bytes() == kDefaultHistoryBudgetBytes);
  COMP_CHECK(history.undo_depth() == 1);
}

COMP_TEST(history_byte_budget_drops_redo_before_undo) {
  History history;
  auto make_state = [](uint8_t seed) {
    DocumentState state{};
    Layer layer;
    layer.id = 1;
    layer.name = "bg";
    layer.pixels = ImageBuffer::solid(64, 64, COMP_PIXEL_RGBA8, seed, seed, seed, 255);
    state.layers.push_back(layer);
    return state;
  };
  const DocumentState state_a = make_state(10);
  const DocumentState state_b = make_state(200);

  history.push("a", state_a, state_b);
  history.push("b", state_b, state_a);
  COMP_CHECK(history.undo_depth() == 2);

  DocumentState out{};
  COMP_CHECK(history.undo(&out));
  COMP_CHECK(history.undo_depth() == 1);
  COMP_CHECK(history.redo_depth() == 1);
  const std::size_t total = history.bytes();
  COMP_CHECK(total > 0);

  /* 预算不足以同时保留撤销与重做记录：重做栈被清空，撤销记录保留。 */
  history.set_budget_bytes(total / 2);
  COMP_CHECK(history.redo_depth() == 0);
  COMP_CHECK(!history.can_redo());
  COMP_CHECK(history.undo_depth() == 1);
  COMP_CHECK(history.can_undo());

  /*
   * 重做能力已丢弃，字节计数与栈内容保持一致：剩下的撤销记录「a」
   * 同时持有两份资产（before = state_a、after = state_b），账本仍是 total。
   * 这正是共享模型的收益——丢重做不释放任何像素，因为没有独占副本。
   */
  COMP_CHECK(history.bytes() == total);
}

/*
 * 像素资产化后的核心正确性属性（2026-10-15 增补）：
 * 历史快照与活文档共享像素资产（shared_ptr + COW）。快照入历史后
 * 再对活图层写像素，必须触发 copy-on-write——否则写入会污染历史里的
 * 「旧状态」，undo 恢复出来的会是被改过的值。本用例钉住这条隔离语义。
 */
COMP_TEST(history_snapshot_isolated_from_pixel_writes) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc desc{};
  desc.name = "COW Probe";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id id = 0;
  document.create_layer(desc, &id);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);

  /* 写入一个已知像素，然后拍快照。 */
  layer->pixels.writable_row(0)[0] = 111;
  const DocumentState before = document.state();
  COMP_CHECK(before.layers.size() == 1);

  /* 编辑：改同一个像素（走 COW，不得动到 before 持有的资产）。 */
  layer->pixels.writable_row(0)[0] = 222;
  document.commit_implicit("Pixel Edit", before);

  COMP_CHECK(document.find(id)->pixels.row(0)[0] == 222);
  COMP_CHECK(document.history().undo_depth() == 1);

  /* undo 必须恢复到快照值 111；若 COW 失效，这里会是 222。 */
  COMP_CHECK(document.undo());
  COMP_CHECK(document.find(id) != nullptr);
  COMP_CHECK_MSG(document.find(id)->pixels.row(0)[0] == 111,
                 "undo must restore the pre-edit pixel (COW isolation broken?)");

  /* redo 再改回 222。 */
  COMP_CHECK(document.redo());
  COMP_CHECK_MSG(document.find(id)->pixels.row(0)[0] == 222,
                 "redo must reapply the edited pixel");
}
