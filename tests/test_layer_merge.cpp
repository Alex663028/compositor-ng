// AI生成
/*
 * test_layer_merge.cpp — 图层合并 / 拼合（对齐上游 LayerMerge.swift 的 ⌘E 家族）。
 *
 * 核心不变量：合并是「渲染不变」变换——合并前后的整篇合成结果必须逐字节一致
 * （上游语义：被合并图层按画布呈现烘入结果层）。结构断言（层数/名称/撤销）、
 * 像素断言（混合算术）与边界断言（拒绝路径）并重。
 *
 * 注：域构造的 Document 从零图层开始（背景层是 ABI 创建协议的产物），
 * 因此域测试显式建层；ABI 测试则从自带 Background 层的文档开始。
 */
#include <cstring>
#include <string>
#include <vector>

#include "compositing/compositor.h"
#include "compositing/effect_eval.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "test_framework.h"

using namespace compositor;

/* ABI 层测试只用 C 符号（「壳的视角」）。 */
#include "compositor/comp_command.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-merge-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

/* 追加一个铺满画布的实心像素层（后添加 = 堆叠序在上）。 */
comp_layer_id add_solid_layer(Document& doc, const char* name, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t a, float opacity = 1.0f,
                              comp_blend_mode blend = COMP_BLEND_NORMAL) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = blend;
  desc.opacity = opacity;
  desc.transform = comp_rect{0, 0, doc.state().width, doc.state().height};
  comp_layer_id id = 0;
  doc.create_layer(desc, &id);
  Layer* layer = doc.find(id);
  if (layer != nullptr) {
    layer->pixels =
        ImageBuffer::solid(doc.state().width, doc.state().height, COMP_PIXEL_RGBA8, r, g, b, a);
  }
  return id;
}

/* 整篇合成到向量（渲染不变量的「前」/「后」快照）。 */
bool flatten_copy(const Document& doc, std::vector<uint8_t>* out, std::string* error) {
  ImageBuffer canvas;
  if (!composite_document(doc, &canvas, error)) return false;
  out->resize(canvas.byte_size());
  if (!canvas.pixel_data().empty()) {
    std::memcpy(out->data(), canvas.pixel_data().data(), canvas.byte_size());
  }
  return true;
}

void pixel_at(const ImageBuffer& img, int32_t x, int32_t y, uint8_t out[4]) {
  const uint8_t* px = img.row(y) + static_cast<size_t>(x) * 4;
  out[0] = px[0];
  out[1] = px[1];
  out[2] = px[2];
  out[3] = px[3];
}

}  // namespace

COMP_TEST(merge_down_bakes_blend_and_opacity) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id below = add_solid_layer(document, "Red", 255, 0, 0, 255);
  const comp_layer_id above = add_solid_layer(document, "Blue", 0, 0, 255, 255, 0.5f);

  /* 渲染不变量：合并前快照。 */
  std::vector<uint8_t> before;
  std::string error;
  COMP_CHECK(flatten_copy(document, &before, &error));

  COMP_CHECK(document.merge_down(above, &error));
  COMP_CHECK_MSG(document.state().layers.size() == 1, "two layers must become one");
  const Layer* merged = document.find(document.state().layers.front().id);
  COMP_CHECK(merged != nullptr);
  COMP_CHECK(merged->name == "Red"); /* 合并结果沿用下层名（对齐上游） */
  COMP_CHECK(merged->blend == COMP_BLEND_NORMAL && merged->opacity == 1.0f);
  COMP_CHECK(merged->transform.w == 4 && merged->transform.h == 4);
  COMP_CHECK(document.state().active_layer == merged->id);

  /*
   * 混合算术钉死：蓝(0,0,255,255) @ 不透明度 0.5 叠到红(255,0,0,255)：
   * out_a = 0.5 + 1*(1-0.5) = 1；out_rgb = 0.5*(0,0,255) + 0.5*(255,0,0)
   *        = (127.5, 0, 127.5) -> 预乘存取 (128, 0, 128, 255)。
   */
  uint8_t px[4] = {0, 0, 0, 0};
  pixel_at(merged->pixels, 1, 1, px);
  COMP_CHECK_MSG(px[0] == 128 && px[1] == 0 && px[2] == 128 && px[3] == 255,
                 "merged pixel must be the baked blend (128,0,128,255)");

  /* 渲染不变量：合并后快照必须逐字节一致。 */
  std::vector<uint8_t> after;
  COMP_CHECK(flatten_copy(document, &after, &error));
  COMP_CHECK_MSG(before == after, "merge must not change the rendered canvas");

  /* 一步撤销 / 重做。 */
  COMP_CHECK(document.history().undo_depth() == 1);
  COMP_CHECK(document.undo());
  COMP_CHECK(document.state().layers.size() == 2);
  COMP_CHECK(document.find(below) != nullptr && document.find(above) != nullptr);
  COMP_CHECK(document.redo());
  COMP_CHECK(document.state().layers.size() == 1);
}

COMP_TEST(merge_down_trims_to_content) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 4, 72.0f, "sRGB");

  /* 两层都只覆盖左半 (0,0,4,4)：合并结果应裁剪到 4x4、原点 (0,0)。 */
  comp_layer_desc desc{};
  desc.name = "Base";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id below = 0;
  document.create_layer(desc, &below);
  document.find(below)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 255, 0, 0, 255);

  desc.name = "Top";
  comp_layer_id top = 0;
  document.create_layer(desc, &top);
  document.find(top)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 255, 255);

  std::string error;
  COMP_CHECK(document.merge_down(top, &error));
  const Layer* merged = document.find(document.state().layers.front().id);
  COMP_CHECK(merged != nullptr);
  /* 内容只在左半：裁剪到 4x4，原点 (0,0)（对齐上游 trimmed）。 */
  COMP_CHECK_MSG(merged->transform.x == 0 && merged->transform.y == 0, "trim origin");
  COMP_CHECK_MSG(merged->transform.w == 4 && merged->transform.h == 4, "trim size");
  COMP_CHECK(merged->pixels.width == 4 && merged->pixels.height == 4);
  uint8_t px[4] = {0, 0, 0, 0};
  pixel_at(merged->pixels, 1, 1, px);
  COMP_CHECK_MSG(px[0] == 0 && px[2] == 255 && px[3] == 255, "trimmed result bakes blue over red");
}

COMP_TEST(merge_down_inside_neutral_group_is_render_invariant) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc group_desc{};
  group_desc.name = "Group";
  group_desc.is_group = 1;
  group_desc.opacity = 1.0f; /* 中性组：合并精确性条件成立 */
  group_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id group = 0;
  document.create_layer(group_desc, &group);

  comp_layer_desc child_desc{};
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id red = 0;
  document.create_layer(child_desc, &red);
  comp_layer_id blue = 0;
  document.create_layer(child_desc, &blue);
  document.find(red)->name = "Red";
  document.find(blue)->name = "Blue";
  document.find(red)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  document.find(blue)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 255, 255);

  std::vector<uint8_t> before;
  std::string error;
  COMP_CHECK(flatten_copy(document, &before, &error));

  COMP_CHECK(document.merge_down(blue, &error));
  /* 组保留，组内只剩一个孩子（合并结果）。 */
  COMP_CHECK(document.state().layers.size() == 2);
  const Layer* group_layer = document.find(group);
  COMP_CHECK(group_layer != nullptr && group_layer->children.size() == 1);
  const Layer* merged = document.find(group_layer->children.front());
  COMP_CHECK(merged != nullptr);
  COMP_CHECK(merged->parent == group);
  uint8_t px[4] = {0, 0, 0, 0};
  pixel_at(merged->pixels, 1, 1, px);
  COMP_CHECK_MSG(px[0] == 0 && px[2] == 255 && px[3] == 255,
                 "neutral group: merged pixels bake blue over red at full opacity");

  std::vector<uint8_t> after;
  COMP_CHECK(flatten_copy(document, &after, &error));
  COMP_CHECK_MSG(before == after, "merge inside a neutral group must not change the canvas");
}

COMP_TEST(merge_down_refuses_inside_scaled_pass_through_group) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc group_desc{};
  group_desc.name = "Half";
  group_desc.is_group = 1;
  group_desc.opacity = 0.5f; /* 穿透组按子层逐个缩放：合并会改变画布呈现 */
  group_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id group = 0;
  document.create_layer(group_desc, &group);

  comp_layer_desc child_desc{};
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id red = 0;
  document.create_layer(child_desc, &red);
  comp_layer_id blue = 0;
  document.create_layer(child_desc, &blue);
  document.find(red)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  document.find(blue)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 255, 255);

  std::string error;
  COMP_CHECK(!document.merge_down(blue, &error));
  COMP_CHECK_MSG(error.find("pass-through group") != std::string::npos,
                 "merge inside a scaled pass-through group must be refused");
  COMP_CHECK(!document.can_merge_down(blue));
  /* 状态未被改动。 */
  COMP_CHECK(document.state().layers.size() == 3);
  COMP_CHECK(document.history().undo_depth() == 0);
  (void)red;
}

COMP_TEST(merge_down_inside_scaled_isolated_group_is_exact) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  /*
   * 隔离组的不透明度在子树合成后整体应用，合并不改变该结构——
   * 因此隔离组内合并任意不透明度都精确（与穿透组相反）。
   */
  comp_layer_desc group_desc{};
  group_desc.name = "Iso";
  group_desc.is_group = 1;
  group_desc.opacity = 0.5f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id group = 0;
  document.create_layer(group_desc, &group);
  document.find(group)->isolated = true; /* 隔离开关走域属性（desc 无该字段） */

  comp_layer_desc child_desc{};
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id red = 0;
  document.create_layer(child_desc, &red);
  comp_layer_id blue = 0;
  document.create_layer(child_desc, &blue);
  document.find(red)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  document.find(blue)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 255, 255);

  std::vector<uint8_t> before;
  std::string error;
  COMP_CHECK(flatten_copy(document, &before, &error));

  COMP_CHECK(document.merge_down(blue, &error));
  COMP_CHECK(document.state().layers.size() == 2);
  const Layer* group_layer = document.find(group);
  COMP_CHECK(group_layer != nullptr && group_layer->children.size() == 1);

  std::vector<uint8_t> after;
  COMP_CHECK(flatten_copy(document, &after, &error));
  COMP_CHECK_MSG(before == after, "merge inside an isolated group must not change the canvas");
}

COMP_TEST(merge_group_flattens_subtree) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc group_desc{};
  group_desc.name = "Folder";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f; /* 零初始化会把不透明度留成 0——显式写全 */
  group_desc.is_group = 1;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id group = 0;
  document.create_layer(group_desc, &group);

  comp_layer_desc child_desc{};
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id red = 0;
  document.create_layer(child_desc, &red);
  comp_layer_id blue = 0;
  document.create_layer(child_desc, &blue);
  document.find(red)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  document.find(blue)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 255, 255);

  std::vector<uint8_t> before;
  std::string error;
  COMP_CHECK(flatten_copy(document, &before, &error));

  COMP_CHECK(document.merge_down(group, &error)); /* 组合并 */
  COMP_CHECK(document.state().layers.size() == 1);
  const Layer* merged = document.find(document.state().layers.front().id);
  COMP_CHECK(merged != nullptr);
  COMP_CHECK(!merged->is_group);
  COMP_CHECK(merged->name == "Folder"); /* 结果沿用组名 */
  uint8_t px[4] = {0, 0, 0, 0};
  pixel_at(merged->pixels, 1, 1, px);
  COMP_CHECK_MSG(px[0] == 0 && px[2] == 255 && px[3] == 255,
                 "merged group must bake blue over red");

  std::vector<uint8_t> after;
  COMP_CHECK(flatten_copy(document, &after, &error));
  COMP_CHECK_MSG(before == after, "merge group must not change the canvas");
}

COMP_TEST(merge_down_rejects_unmergeable_plans) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  /* 孤立图层：下方无事可合并。 */
  const comp_layer_id lonely = add_solid_layer(document, "Lonely", 1, 2, 3, 255);
  std::string error;
  COMP_CHECK(!document.merge_down(lonely, &error));
  COMP_CHECK_MSG(error.find("no layer below") != std::string::npos, "lonely layer must be refused");
  COMP_CHECK(!document.can_merge_down(lonely));

  /* 下方是组：拒绝（对齐上游 !below.isGroup 规则）。 */
  comp_layer_desc group_desc{};
  group_desc.name = "Group";
  group_desc.is_group = 1;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id group = 0;
  document.create_layer(group_desc, &group);
  const comp_layer_id above_group = add_solid_layer(document, "Above", 9, 9, 9, 255);
  COMP_CHECK(!document.merge_down(above_group, &error));
  COMP_CHECK_MSG(error.find("below is a group") != std::string::npos,
                 "merging onto a group must be refused");

  /* 空组：组合并不允许。 */
  comp_layer_desc empty_group_desc{};
  empty_group_desc.name = "Empty";
  empty_group_desc.is_group = 1;
  empty_group_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id empty_group = 0;
  document.create_layer(empty_group_desc, &empty_group);
  COMP_CHECK(!document.merge_down(empty_group, &error));
  COMP_CHECK_MSG(error.find("no pixel layers") != std::string::npos, "empty group must be refused");

  /* 不存在的图层。 */
  COMP_CHECK(!document.merge_down(12345, &error));
  COMP_CHECK_MSG(error.find("not found") != std::string::npos, "missing layer must be refused");
  (void)group;
}

/*
 * 合并烘入与求值口径一致（2026-10-16 调整求值落地后的行为）：
 *   - 调整（adjustments）已被合成路径求值，因此合并 **允许** 且烘入结果正确；
 *   - 效果（effects）仍无求值点，仍然明确拒绝。
 */
COMP_TEST(merge_down_bakes_adjustments_and_effects) {
  /* (1) 携带调整的图层：合并成功，且烘入结果与「先渲染再合并」逐字节一致。 */
  {
    Engine engine(test_engine_desc());
    Document document(&engine, 4, 4, 72.0f, "sRGB");
    add_solid_layer(document, "Red", 255, 0, 0, 255);
    const comp_layer_id above = add_solid_layer(document, "Blue", 0, 0, 255, 255);

    Layer* blue = document.find(above);
    COMP_CHECK(blue != nullptr);
    Adjustment adjustment;
    adjustment.kind = COMP_ADJUSTMENT_INVERT;
    blue->adjustments.push_back(adjustment);

    /* 烘入前的整篇合成结果（含调整）作为期望值。 */
    std::vector<uint8_t> expected;
    std::string error;
    COMP_CHECK(flatten_copy(document, &expected, &error));

    COMP_CHECK_MSG(document.merge_down(above, &error),
                   "adjustment-carrying layers are now evaluable and must bake, not be refused");
    COMP_CHECK(document.state().layers.size() == 1);

    std::vector<uint8_t> after;
    COMP_CHECK(flatten_copy(document, &after, &error));
    COMP_CHECK_MSG(after == expected,
                   "baked pixels must be byte-identical to the pre-merge composite");
    /* 合并后调整不再残留（已被烘进像素）。 */
    COMP_CHECK(document.state().layers.front().adjustments.empty());
  }

  /* (2) 携带效果的图层：效果求值落地后同样烘入，不再拒绝。 */
  {
    Engine engine(test_engine_desc());
    Document document(&engine, 4, 4, 72.0f, "sRGB");
    add_solid_layer(document, "Red", 255, 0, 0, 255);
    const comp_layer_id above = add_solid_layer(document, "Blue", 0, 0, 255, 255);

    Layer* blue = document.find(above);
    COMP_CHECK(blue != nullptr);
    Effect effect;
    effect.effect = 0;
    blue->effects.push_back(effect);

    std::string error;
    std::vector<uint8_t> expected;
    COMP_CHECK(flatten_copy(document, &expected, &error));

    COMP_CHECK_MSG(document.merge_down(above, &error),
                   "effect-carrying layers are now evaluated and must bake, not be refused");
    COMP_CHECK(document.state().layers.size() == 1);

    std::vector<uint8_t> after;
    COMP_CHECK(flatten_copy(document, &after, &error));
    COMP_CHECK_MSG(after == expected,
                   "baked pixels must be byte-identical to the pre-merge composite");
    /* 合并后效果不再残留（已烘进像素）。 */
    COMP_CHECK(document.state().layers.front().effects.empty());
  }

  /* (3) 引擎不认识的 effect 值：仍拒绝，否则烘入会静默丢弃它。 */
  {
    Engine engine(test_engine_desc());
    Document document(&engine, 4, 4, 72.0f, "sRGB");
    add_solid_layer(document, "Red", 255, 0, 0, 255);
    const comp_layer_id above = add_solid_layer(document, "Blue", 0, 0, 255, 255);

    Layer* blue = document.find(above);
    COMP_CHECK(blue != nullptr);
    Effect effect;
    effect.effect = 999; /* 远超 kEffectKindCount */
    blue->effects.push_back(effect);

    std::string error;
    COMP_CHECK(!document.merge_down(above, &error));
    COMP_CHECK_MSG(error.find(kUnevaluatedEffectMarker) != std::string::npos,
                   "unknown effect kind must be refused with the shared reason marker");
    /* 状态未被改动。 */
    COMP_CHECK(document.state().layers.size() == 2);
    COMP_CHECK(document.history().undo_depth() == 0);
  }
}

COMP_TEST(merge_down_repoints_clipping) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_solid_layer(document, "Base", 255, 0, 0, 255);
  const comp_layer_id clipped = add_solid_layer(document, "Clipped", 0, 255, 0, 255);
  const comp_layer_id watcher = add_solid_layer(document, "Watcher", 0, 0, 255, 255);
  document.find(clipped)->clipping_source = base;
  document.find(watcher)->clipping_source = base;

  std::vector<uint8_t> before;
  std::string error;
  COMP_CHECK(flatten_copy(document, &before, &error));

  /* 合并 Base + Clipped：Watcher 的剪贴引用改指结果层。 */
  COMP_CHECK(document.merge_down(clipped, &error));
  COMP_CHECK(document.state().layers.size() == 2);
  const comp_layer_id merged_id = document.state().active_layer;
  const Layer* watcher_layer = document.find(watcher);
  COMP_CHECK(watcher_layer != nullptr);
  COMP_CHECK_MSG(watcher_layer->clipping_source == merged_id,
                 "clipping references to merged layers must re-point to the result");
  const Layer* merged = document.find(merged_id);
  COMP_CHECK(merged != nullptr);
  COMP_CHECK(merged->clipping_source == 0); /* 结果层不剪贴到任何层 */

  std::vector<uint8_t> after;
  COMP_CHECK(flatten_copy(document, &after, &error));
  COMP_CHECK_MSG(before == after, "merge must not change the canvas (clipping preserved)");
}

COMP_TEST(flatten_image_composites_all_layers) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  add_solid_layer(document, "Red", 255, 0, 0, 255);
  add_solid_layer(document, "Green", 0, 255, 0, 255);
  add_solid_layer(document, "Blue", 0, 0, 255, 255);

  std::vector<uint8_t> before;
  std::string error;
  COMP_CHECK(flatten_copy(document, &before, &error));

  COMP_CHECK(document.flatten_image(&error));
  COMP_CHECK(document.state().layers.size() == 1);
  const Layer* flat = document.find(document.state().layers.front().id);
  COMP_CHECK(flat != nullptr);
  COMP_CHECK(flat->name == "Background");
  COMP_CHECK(flat->transform.w == 4 && flat->transform.h == 4);
  uint8_t px[4] = {0, 0, 0, 0};
  pixel_at(flat->pixels, 0, 0, px);
  COMP_CHECK_MSG(px[0] == 0 && px[2] == 255 && px[3] == 255, "flatten must bake blue on top");

  std::vector<uint8_t> after;
  COMP_CHECK(flatten_copy(document, &after, &error));
  COMP_CHECK_MSG(before == after, "flatten must not change the canvas");

  /* 空文档拒绝。 */
  Engine engine2(test_engine_desc());
  Document empty(&engine2, 4, 4, 72.0f, "sRGB");
  COMP_CHECK(!empty.flatten_image(&error));
  COMP_CHECK_MSG(error.find("no layers") != std::string::npos, "empty document must be refused");
}

/* ---------------------------------------------------------------- ABI 层 */

namespace {

comp_layer_id abi_add_solid_layer(comp_document document, int32_t width, int32_t height,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
    bytes[i] = r;
    bytes[i + 1] = g;
    bytes[i + 2] = b;
    bytes[i + 3] = a;
  }
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(width) * 4u;
  image.pixels = bytes.data();

  comp_layer_desc desc{};
  desc.name = "Solid";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, width, height};
  comp_layer_id id = 0;
  COMP_CHECK_MSG(comp_layer_add(document, &desc, &id) == COMP_OK, "comp_layer_add");
  COMP_CHECK_MSG(comp_layer_set_pixels(document, id, &image) == COMP_OK, "comp_layer_set_pixels");
  return id;
}

std::vector<uint8_t> abi_flatten(comp_document document, int32_t width, int32_t height) {
  std::vector<uint8_t> canvas(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  const comp_result result = comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8,
                                                 static_cast<size_t>(width) * 4u);
  COMP_CHECK_MSG(result == COMP_OK, "comp_render_flatten");
  return canvas;
}

}  // namespace

COMP_TEST(abi_merge_down_and_flatten) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  COMP_CHECK(engine != nullptr);

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  document_desc.dpi = 72.0f;
  document_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(document != nullptr);

  /* ABI 创建协议自带 Background 层；再叠一个实心蓝层。 */
  const comp_layer_id blue = abi_add_solid_layer(document, 8, 8, 0, 0, 255, 255);

  /* 合并计划查询：蓝层可向下合并（与 Background 合并）。 */
  int32_t can = -1;
  COMP_CHECK(comp_layer_can_merge_down(document, blue, &can) == COMP_OK);
  COMP_CHECK(can == 1);

  const std::vector<uint8_t> before = abi_flatten(document, 8, 8);
  COMP_CHECK(comp_layer_merge_down(document, blue) == COMP_OK);

  /* 结构：两层并一层。 */
  size_t count = 0;
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 1);
  const std::vector<uint8_t> after = abi_flatten(document, 8, 8);
  COMP_CHECK_MSG(before == after, "ABI merge must not change the canvas");

  /* 一步撤销恢复两层。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 2);
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 1);

  /*
   * 命令入口：先把蓝层设为活动层（选择不入历史），再走
   * comp_command_invoke("layer.merge_down")——与外壳菜单路径一致。
   */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 2);
  COMP_CHECK(comp_layer_set_active(document, blue) == COMP_OK);
  COMP_CHECK(comp_command_invoke(document, "layer.merge_down", nullptr) == COMP_OK);
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 1);

  /* 命令入口：layer.flatten。 */
  int32_t enabled = -1;
  COMP_CHECK(comp_command_state(document, "layer.flatten", &enabled, &can) == COMP_OK);
  COMP_CHECK(enabled == 1);
  COMP_CHECK(comp_command_invoke(document, "layer.flatten", nullptr) == COMP_OK);
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 1);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(abi_merge_down_error_paths) {
  comp_engine_desc desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  document_desc.dpi = 72.0f;
  document_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 只有 Background 一层：它无事可合并 -> COMP_ERR_STATE。 */
  comp_layer_id lonely = 0;
  {
    comp_layer_id ids[8] = {0};
    size_t n = 0;
    comp_layer_list(document, ids, 8, &n);
    lonely = n > 0 ? ids[0] : 0;
  }
  COMP_CHECK(lonely != 0);
  int32_t can = -1;
  COMP_CHECK(comp_layer_can_merge_down(document, lonely, &can) == COMP_OK);
  COMP_CHECK(can == 0);
  COMP_CHECK(comp_layer_merge_down(document, lonely) == COMP_ERR_STATE);

  /* 命令入口同样拒绝（活动层不可合并）。 */
  int32_t enabled = -1;
  COMP_CHECK(comp_command_state(document, "layer.merge_down", &enabled, &can) == COMP_OK);
  COMP_CHECK(enabled == 0);

  /* 不存在的图层 -> COMP_ERR_NOT_FOUND。 */
  COMP_CHECK(comp_layer_merge_down(document, 9999) == COMP_ERR_NOT_FOUND);

  /*
   * 叠加两层，下层挂引擎**不认识**的 effect 值 -> COMP_ERR_UNSUPPORTED。
   *
   * 区分两种情形（这是本用例的要点）：
   *   - 引擎已知的效果（effect_kind_is_evaluated，当前 9 种）：已由合成求值包含，
   *     烘入即正确，允许合并（见 merge_down_bakes_adjustments_and_effects）；
   *   - 未知的 kind（如 999）：本引擎既不求值也不绘制，烘入会静默丢弃，
   *     所以必须拒绝。
   * 早期版本用 kind=0（已知的描边）来测这条拒绝路径，那是在“效果只入库不求值”
   * 的旧边界下写的；效果求值落地后 kind=0 已经可以正常烘入，故改用未知 kind，
   * 让拒绝断言继续守住真正需要守住的那条线。
   */
  const comp_layer_id top = abi_add_solid_layer(document, 4, 4, 0, 0, 255, 255);
  comp_layer_id below = 0;
  {
    comp_layer_id ids[8] = {0};
    size_t n = 0;
    comp_layer_list(document, ids, 8, &n);
    below = n >= 2 ? ids[0] : 0;
  }
  COMP_CHECK(below != 0);
  COMP_CHECK(comp_layer_set_effect(document, below, 999, "{}") == COMP_OK);
  COMP_CHECK(comp_layer_merge_down(document, top) == COMP_ERR_UNSUPPORTED);
  /* 状态未被改动。 */
  size_t count = 0;
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK(count == 2);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
