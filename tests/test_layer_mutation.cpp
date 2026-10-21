// AI生成
/*
 * test_layer_mutation.cpp — P1-7：为 5 个此前零单测覆盖的已交付 ABI 补端到端测试。
 *
 * 覆盖：comp_layer_set_mask / comp_layer_set_clipping_source /
 *       comp_layer_set_effect + comp_layer_remove_effect /
 *       comp_layer_reorder / comp_layer_remove（子树与 clipping_source 级联清理）。
 *
 * 判据不是「API 返回 COMP_OK」，而是端到端事实：合成结果变化 + .comp manifest 里
 * 真的出现了 maskFile / maskEnabled / engineClippingSourceID / engineEffects，
 * 且经过一次 save -> load -> save 之后仍然成立（往返保真）。
 *
 * 本文件只使用 comp_*.h 中的 C 符号，与三端外壳的集成路径一致。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

comp_engine_desc mutation_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-layer-mutation-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* 生存期与 comp_image.pixels 绑定的实心图像。 */
struct SolidImage {
  std::vector<uint8_t> bytes;
  comp_image image{};

  SolidImage(int32_t width, int32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
    for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
      bytes[i] = r;
      bytes[i + 1] = g;
      bytes[i + 2] = b;
      bytes[i + 3] = a;
    }
    image.width = width;
    image.height = height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(width) * 4u;
    image.pixels = bytes.data();
  }
};

struct SolidMask {
  std::vector<uint8_t> bytes;
  comp_mask mask{};

  SolidMask(int32_t width, int32_t height, uint8_t value)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height), value) {
    mask.width = width;
    mask.height = height;
    mask.stride = static_cast<size_t>(width);
    mask.pixels = bytes.data();
  }
};

comp_layer_id background_layer(comp_document document) {
  comp_layer_id id = 0;
  size_t count = 0;
  if (comp_layer_list(document, &id, 1u, &count) != COMP_OK || count < 1u) return 0;
  return id;
}

/* 新建一个铺满画布、给定颜色的普通图层。 */
comp_layer_id add_solid_layer(comp_document document, int32_t width, int32_t height, uint8_t r,
                              uint8_t g, uint8_t b, uint8_t a) {
  comp_layer_desc desc{};
  desc.name = "solid";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, width, height};
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  const SolidImage image(width, height, r, g, b, a);
  if (comp_layer_set_pixels(document, id, &image.image) != COMP_OK) return 0;
  return id;
}

std::vector<uint8_t> flatten(comp_document document, int32_t width, int32_t height) {
  std::vector<uint8_t> canvas(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
  comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8, static_cast<size_t>(width) * 4u);
  return canvas;
}

bool matches(const std::vector<uint8_t>& canvas, int32_t width, int32_t x, int32_t y, uint8_t r,
             uint8_t g, uint8_t b, uint8_t a) {
  const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)) * 4u;
  if (index + 3 >= canvas.size()) return false;
  return canvas[index] == r && canvas[index + 1] == g && canvas[index + 2] == b &&
         canvas[index + 3] == a;
}

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/layer_mutation", ec);
  return std::string("build/layer_mutation/") + name;
}

std::string read_manifest(const std::string& comp_path) {
  std::ifstream in(comp_path + "/manifest.json", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

/* ------------------------------------------------------------------ reorder */

COMP_TEST(layer_reorder_changes_stacking_order_and_is_undoable) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  /* 新图层在数组尾部 = 堆叠最上层，故蓝盖红。 */
  const comp_layer_id top = add_solid_layer(document, 4, 4, 0, 0, 255, 255);
  COMP_CHECK(top != 0);
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255), "new layer stacks on top");

  /* reorder(id, before)：把 id 放到 before 之下（数组更靠前 = 更靠下）。 */
  COMP_CHECK(comp_layer_reorder(document, top, background) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255), "reorder puts target below `before`");

  /* before = 0 表示移到同级末尾（最上层）。 */
  COMP_CHECK(comp_layer_reorder(document, top, 0) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255), "before=0 moves to the top");

  /* 重排同样进撤销栈：撤销一次回到「蓝在下」。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255), "reorder is undoable");

  /* 参数校验：不存在的目标图层。 */
  COMP_CHECK(comp_layer_reorder(document, 999999, 0) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_reorder(document, top, 999999) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------------- remove（级联清理） */

COMP_TEST(layer_remove_cascades_subtree_and_clears_clipping_source) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const comp_layer_id clipper = add_solid_layer(document, 8, 8, 0, 255, 0, 255);
  COMP_CHECK(clipper != 0);

  comp_layer_desc group_desc{};
  group_desc.name = "Group";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 8, 8};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  comp_layer_desc child_desc{};
  child_desc.name = "Child";
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 8, 8};
  child_desc.parent = group;
  comp_layer_id child = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);

  size_t before = 0;
  COMP_CHECK(comp_layer_count(document, &before) == COMP_OK);
  COMP_CHECK_MSG(before == 4u, "background + clipper + group + child");

  /* 让 clipper 以 child 为剪贴来源，稍后验证级联清理会把该引用置 0。 */
  COMP_CHECK(comp_layer_set_clipping_source(document, clipper, child) == COMP_OK);

  /* 删组必须连子树一起删（-2），且 clipper 的剪贴来源被清掉。 */
  COMP_CHECK(comp_layer_remove(document, group) == COMP_OK);
  size_t after = 0;
  COMP_CHECK(comp_layer_count(document, &after) == COMP_OK);
  COMP_CHECK_MSG(after == before - 2u, "removing a group also removes its subtree");

  comp_layer_id ids[8];
  size_t listed = 0;
  COMP_CHECK(comp_layer_list(document, ids, 8u, &listed) == COMP_OK);
  bool group_gone = true;
  bool child_gone = true;
  for (size_t i = 0; i < listed; ++i) {
    if (ids[i] == group) group_gone = false;
    if (ids[i] == child) child_gone = false;
  }
  COMP_CHECK_MSG(group_gone && child_gone, "group and child are really gone");

  /* 级联置 0 的可观测证据：落盘后不再出现 engineClippingSourceID。 */
  const std::string path = work_path("remove_cascade.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_manifest(path);
  COMP_CHECK_MSG(!contains(manifest, "engineClippingSourceID"),
                 "dangling clipping source is cleared on cascade removal");

  /* 删除可撤销：撤销一次把子树整棵复活。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  size_t restored = 0;
  COMP_CHECK(comp_layer_count(document, &restored) == COMP_OK);
  COMP_CHECK_MSG(restored == before, "removal is undoable as one step");

  COMP_CHECK(comp_layer_remove(document, 999999) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* --------------------------------------------------------------- set_mask */

COMP_TEST(layer_mask_round_trips_through_comp) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);

  /* 掩码会改变合成结果：全 0 掩码把背景整层抹掉。 */
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 1, 1, 255, 0, 0, 255));

  const SolidMask mask(4, 4, 0);
  COMP_CHECK(comp_layer_set_mask(document, background, &mask.mask) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 0, 0), "zero mask hides the layer");

  const std::string path = work_path("mask_round_trip.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  std::string manifest = read_manifest(path);
  COMP_CHECK_MSG(contains(manifest, "\"maskFile\""), "maskFile is written to the manifest");
  COMP_CHECK_MSG(contains(manifest, "\"maskEnabled\""), "maskEnabled is written to the manifest");
  COMP_CHECK_MSG(contains(manifest, ".mask.png"), "mask asset is stored as <uuid>.mask.png");

  /* 加载还原：读回后掩码仍然生效，且再次落盘仍带 maskFile。 */
  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, path.c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    canvas = flatten(loaded, 4, 4);
    COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 0, 0), "mask survives a .comp round trip");

    const std::string path2 = work_path("mask_round_trip_2.comp");
    COMP_CHECK(comp_document_save(loaded, path2.c_str()) == COMP_OK);
    COMP_CHECK_MSG(contains(read_manifest(path2), "\"maskFile\""),
                   "re-saved project still carries maskFile");
    comp_document_destroy(loaded);
  }

  /* mask = NULL 移除掩码。 */
  COMP_CHECK(comp_layer_set_mask(document, background, nullptr) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255), "NULL mask removes the mask");

  COMP_CHECK(comp_layer_set_mask(document, 999999, &mask.mask) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ----------------------------------------------------- set_clipping_source */

COMP_TEST(layer_clipping_source_round_trips_through_comp) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const comp_layer_id clipper = add_solid_layer(document, 4, 4, 0, 0, 255, 255);
  COMP_CHECK(clipper != 0);

  /* 自引用与非存在来源都必须被拒。 */
  COMP_CHECK(comp_layer_set_clipping_source(document, clipper, clipper) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_set_clipping_source(document, clipper, 999999) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_set_clipping_source(document, 999999, background) == COMP_ERR_NOT_FOUND);

  COMP_CHECK(comp_layer_set_clipping_source(document, clipper, background) == COMP_OK);

  const std::string path = work_path("clipping_round_trip.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  std::string manifest = read_manifest(path);
  COMP_CHECK_MSG(contains(manifest, "\"engineClippingSourceID\""),
                 "clipping source is written to the manifest");

  /* 加载还原：读回后再落盘，引用仍在（即 ID 映射被正确重建）。 */
  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, path.c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    const std::string path2 = work_path("clipping_round_trip_2.comp");
    COMP_CHECK(comp_document_save(loaded, path2.c_str()) == COMP_OK);
    COMP_CHECK_MSG(contains(read_manifest(path2), "\"engineClippingSourceID\""),
                   "clipping source survives a .comp round trip");
    comp_document_destroy(loaded);
  }

  /* source = 0 取消剪贴。 */
  COMP_CHECK(comp_layer_set_clipping_source(document, clipper, 0) == COMP_OK);
  const std::string path3 = work_path("clipping_cleared.comp");
  COMP_CHECK(comp_document_save(document, path3.c_str()) == COMP_OK);
  COMP_CHECK_MSG(!contains(read_manifest(path3), "engineClippingSourceID"),
                 "clearing the clipping source drops the manifest key");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ---------------------------------------------------------------- effects */

COMP_TEST(layer_effects_add_remove_round_trip) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);

  const char* kSettings = "{\"radius\":3}";
  COMP_CHECK(comp_layer_set_effect(document, background, 1u, kSettings) == COMP_OK);
  /* 同一 effect 重复设置是「更新」而非追加。 */
  COMP_CHECK(comp_layer_set_effect(document, background, 1u, kSettings) == COMP_OK);

  const std::string path = work_path("effects_round_trip.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  std::string manifest = read_manifest(path);
  COMP_CHECK_MSG(contains(manifest, "\"engineEffects\""), "engineEffects key is written");
  COMP_CHECK_MSG(contains(manifest, "radius"), "effect settings are persisted");
  COMP_CHECK_MSG(contains(manifest, "\"effect\": 1"), "effect kind is persisted");

  /* 加载还原。 */
  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, path.c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    const std::string path2 = work_path("effects_round_trip_2.comp");
    COMP_CHECK(comp_document_save(loaded, path2.c_str()) == COMP_OK);
    COMP_CHECK_MSG(contains(read_manifest(path2), "radius"),
                   "effects survive a .comp round trip");
    comp_document_destroy(loaded);
  }

  COMP_CHECK(comp_layer_remove_effect(document, background, 999u) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_remove_effect(document, background, 1u) == COMP_OK);
  const std::string path3 = work_path("effects_removed.comp");
  COMP_CHECK(comp_document_save(document, path3.c_str()) == COMP_OK);
  const std::string cleared = read_manifest(path3);
  COMP_CHECK_MSG(!contains(cleared, "radius"), "removed effect is gone from the manifest");
  COMP_CHECK_MSG(contains(cleared, "engineEffects"), "engineEffects key stays present (empty)");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
/* ==================================================================
 * 本轮审计修复的回归护栏。每条对应一个已修复的缺陷，防止复发。
 * ================================================================== */

/* 修复 1：remove_subtree 递归 erase 后读 layer->parent 的悬垂指针。 */

COMP_TEST(remove_subtree_keeps_siblings_intact_when_deleting_a_group) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  /* 组 G 内两个子层；G 之后还有一个兄弟层 S。删除 G 时 S 的父子关系必须完好，
     否则悬垂读到的 parent 会把 S 从错误的父组里摘掉（孤儿 -> 合成整篇拒绝）。 */
  comp_layer_desc group_desc{};
  group_desc.name = "G";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  for (int i = 0; i < 2; ++i) {
    comp_layer_desc child_desc{};
    child_desc.name = "child";
    child_desc.blend = COMP_BLEND_NORMAL;
    child_desc.opacity = 1.0f;
    child_desc.transform = comp_rect{0, 0, 4, 4};
    child_desc.parent = group;
    comp_layer_id child = 0;
    COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);
  }

  const comp_layer_id sibling = add_solid_layer(document, 4, 4, 0, 0, 255, 255);
  COMP_CHECK(sibling != 0);

  COMP_CHECK(comp_layer_remove(document, group) == COMP_OK);

  /* 兄弟层仍在，且仍可正常参与合成（未被误挂到已删除的组下）。 */
  size_t count = 0;
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 2u, "background + sibling survive the group removal");

  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255),
                 "sibling still composites after group removal (no dangling parent)");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 修复 2：reorder 的跨父级锚点必须拒绝，否则层级与堆叠序同时被破坏。 */

COMP_TEST(reorder_rejects_cross_parent_anchor) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_layer_desc group_desc{};
  group_desc.name = "G";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  comp_layer_desc child_desc{};
  child_desc.name = "child";
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id child = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);

  const comp_layer_id root_sibling = add_solid_layer(document, 4, 4, 0, 0, 255, 255);
  COMP_CHECK(root_sibling != 0);

  /* 把根级层放到组内子层之前：跨级锚点，必须被拒绝而不是静默改树。 */
  COMP_CHECK_MSG(comp_layer_reorder(document, root_sibling, child) == COMP_ERR_INVALID_ARGUMENT,
                 "cross-parent anchor is rejected instead of corrupting the tree");

  /* 树未被动过：子层仍挂在组下，合成照常。 */
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255), "canvas unchanged after rejection");

  /* 同级锚点仍然可用（修复不能把合法路径一起堵死）。 */
  COMP_CHECK(comp_layer_reorder(document, root_sibling, background) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255), "same-level reorder still works");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 修复 3：活动事务内 undo/redo 必须被拒绝，否则提交后历史链错位。 */

COMP_TEST(undo_is_rejected_while_a_transaction_is_active) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(document, "batch", &txn) == COMP_OK);

  const comp_layer_id top = add_solid_layer(document, 4, 4, 0, 0, 255, 255);
  COMP_CHECK(top != 0);

  /* 事务内 undo/redo 必须拒绝（要走回退请用 comp_txn_abort）。 */
  COMP_CHECK_MSG(comp_document_undo(document) == COMP_ERR_STATE,
                 "undo inside an active transaction is rejected");
  COMP_CHECK_MSG(comp_document_redo(document) == COMP_ERR_STATE,
                 "redo inside an active transaction is rejected");

  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);

  /* 事务正常提交：一次批量编辑 = 一步撤销。 */
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255), "committed transaction applied");
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255),
                 "one undo reverts the whole transaction");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 修复 4：文档先于事务句柄销毁时，句柄必须被安全作废（不能解引用野指针）。 */

COMP_TEST(txn_handle_is_invalidated_when_document_is_destroyed_first) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(document, "dangling", &txn) == COMP_OK);

  /* 宿主常见误用：文档先销毁，事务句柄后提交。~Document 已把句柄置为失效，
     此处必须返回错误而不是 use-after-free。 */
  comp_document_destroy(document);

  COMP_CHECK_MSG(comp_txn_commit(txn) == COMP_ERR_INVALID_ARGUMENT,
                 "commit after document destruction is rejected, not dereferenced");
  COMP_CHECK_MSG(comp_txn_abort(txn) == COMP_ERR_INVALID_ARGUMENT,
                 "abort after document destruction is rejected, not dereferenced");

  comp_engine_destroy(engine);
}

/* 修复 5：sampling / _reserved 必须在 comp_layer_add 入口就校验。 */

COMP_TEST(add_layer_rejects_invalid_sampling_and_reserved) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  comp_layer_desc bad_sampling{};
  bad_sampling.name = "bad";
  bad_sampling.blend = COMP_BLEND_NORMAL;
  bad_sampling.opacity = 1.0f;
  bad_sampling.transform = comp_rect{0, 0, 4, 4};
  bad_sampling.sampling = static_cast<comp_sampling>(99);
  comp_layer_id id = 0;
  COMP_CHECK_MSG(comp_layer_add(document, &bad_sampling, &id) == COMP_ERR_INVALID_ARGUMENT,
                 "invalid sampling is rejected at the ABI boundary");

  comp_layer_desc bad_reserved{};
  bad_reserved.name = "bad";
  bad_reserved.blend = COMP_BLEND_NORMAL;
  bad_reserved.opacity = 1.0f;
  bad_reserved.transform = comp_rect{0, 0, 4, 4};
  bad_reserved.sampling = COMP_SAMPLING_HIGH_QUALITY;
  bad_reserved._reserved = 7;
  COMP_CHECK_MSG(comp_layer_add(document, &bad_reserved, &id) == COMP_ERR_INVALID_ARGUMENT,
                 "non-zero _reserved is rejected");

  /* 合法值仍然放行。 */
  comp_layer_desc good{};
  good.name = "good";
  good.blend = COMP_BLEND_NORMAL;
  good.opacity = 1.0f;
  good.transform = comp_rect{0, 0, 4, 4};
  good.sampling = COMP_SAMPLING_NEAREST;
  COMP_CHECK(comp_layer_add(document, &good, &id) == COMP_OK);
  COMP_CHECK(id != 0);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 修复 6：stride 过小必须被拒绝（引擎替调用方兜底会写到其缓冲之外）。 */

COMP_TEST(render_rejects_stride_smaller_than_a_row) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  std::vector<uint8_t> canvas(4u * 4u * 4u, 0u);
  COMP_CHECK_MSG(comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8, 15u) ==
                     COMP_ERR_INVALID_ARGUMENT,
                 "stride < row_bytes is rejected instead of overrunning the host buffer");
  COMP_CHECK_MSG(comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8, 16u) == COMP_OK,
                 "exact row_bytes stride still works");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 修复 7：缩画布对完全离屏图层的裁剪必须真的生效（不能被尺寸回落撤销）。 */

COMP_TEST(canvas_shrink_clips_layers_fully_outside_the_canvas) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 16;
  document_desc.height = 16;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(16, 16, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  /* 完全落在新画布之外的图层：裁剪后包围盒应为空，不参与合成。 */
  comp_layer_desc far_desc{};
  far_desc.name = "far";
  far_desc.blend = COMP_BLEND_NORMAL;
  far_desc.opacity = 1.0f;
  far_desc.transform = comp_rect{100, 100, 16, 16};
  far_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id far_layer = 0;
  COMP_CHECK(comp_layer_add(document, &far_desc, &far_layer) == COMP_OK);
  const SolidImage green(16, 16, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, far_layer, &green.image) == COMP_OK);

  /* 缩到 4x4：离屏图层应被裁掉，合成只剩红色背景。 */
  COMP_CHECK(comp_document_set_size(document, comp_size{4, 4}) == COMP_OK);
  std::vector<uint8_t> canvas(4u * 4u * 4u, 0u);
  COMP_CHECK(comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8, 16u) == COMP_OK);
  COMP_CHECK_MSG(canvas[0] == 255 && canvas[1] == 0 && canvas[2] == 0,
                 "layer fully outside the shrunk canvas is clipped away, not kept at full size");

  /* 撤销后画布尺寸复原（一步撤销）。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  comp_size restored{};
  COMP_CHECK(comp_document_size(document, &restored) == COMP_OK);
  COMP_CHECK_MSG(restored.width == 16 && restored.height == 16, "canvas resize is undoable");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}/* ==================================================================
 * 格式层与效果层修复的回归护栏。
 * ================================================================== */

/*
 * 修复 8：.comp 的 enginePixelFormat 未校验就写入。
 * 非法枚举让 bytes_per_pixel() 返回 0，stride()/byte_size() 全零，
 * 一个畸形文件就能让读路径按format 算出的步长与真实缓冲不符。
 * 现在解析期拒绝未知格式，ImageBuffer::empty() 也把未知格式视为空。
 */
COMP_TEST(comp_file_rejects_unknown_pixel_format) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  const std::string path = work_path("unknown_pixel_format.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);

  /* 把合法RGBA8 篡改成引擎不认识的枚举值。 */
  std::string manifest = read_manifest(path);
  const size_t at = manifest.find("\"enginePixelFormat\"");
  COMP_CHECK_MSG(at != std::string::npos, "enginePixelFormat key is present in the manifest");
  if (at != std::string::npos) {
    const size_t colon = manifest.find(':', at);
    const size_t value_start = manifest.find_first_not_of(" \t\n\r", colon + 1);
    const size_t value_end = manifest.find_first_of(",\n}", value_start);
    manifest.replace(value_start, value_end - value_start, "9999");
    std::ofstream out(path + "/manifest.json", std::ios::binary | std::ios::trunc);
    out << manifest;
    out.close();
  }

  /* 必须明确拒绝加载，而不是带着 stride=0 的缓冲继续跑。 */
  comp_document loaded = nullptr;
  COMP_CHECK_MSG(comp_document_load(engine, path.c_str(), &loaded) != COMP_OK,
                 "unknown enginePixelFormat is rejected at parse time");
  if (loaded != nullptr) comp_document_destroy(loaded);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * 修复 9：comp_layer_remove_effect 的失败路径不应改动状态。
 * 原实现先 erase 再看 size，NOT_FOUND 时文档已被改过却没记历史。
 */
COMP_TEST(remove_effect_failure_leaves_document_untouched) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);

  /* 该图层没有任何效果，移除不存在的效果必须报 NOT_FOUND。 */
  COMP_CHECK(comp_layer_remove_effect(document, background, 42u) == COMP_ERR_NOT_FOUND);

  /* 失败不得留下历史：撤销栈仍应为空，撤销应报「无内容可撤」。 */
  int32_t can_undo = 1;
  COMP_CHECK(comp_document_can_undo(document, &can_undo) == COMP_OK);
  COMP_CHECK_MSG(can_undo == 0, "a failed remove must not push a history entry");
  COMP_CHECK_MSG(comp_document_undo(document) == COMP_ERR_STATE,
                 "nothing to undo after a failed remove");

  /* 成功路径仍然正常。 */
  COMP_CHECK(comp_layer_set_effect(document, background, 1u, "{\"radius\":3}") == COMP_OK);
  COMP_CHECK(comp_layer_remove_effect(document, background, 1u) == COMP_OK);
  const std::string path = work_path("effect_removed_clean.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  COMP_CHECK_MSG(!contains(read_manifest(path), "radius"), "effect really removed");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * 修复 10：comp_engine_threads 必须报告真实生效的并行度。
 * 并行度是进程级全局量，后建的引擎会改变既有引擎的实际行为。
 */
COMP_TEST(engine_threads_reports_the_effective_parallelism) {
  comp_engine_desc desc = mutation_engine_desc();
  desc.worker_threads = 2;
  comp_engine first = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &first) == COMP_OK);

  uint32_t reported = 0;
  COMP_CHECK(comp_engine_threads(first, &reported) == COMP_OK);
  COMP_CHECK_MSG(reported == 2u, "explicit worker_threads is reported back");

  /* 第二个引擎把全局并行度改成 4：第一个引擎的报告值必须跟着变，
     否则ABI 报告值与真实行为漂移。 */
  desc.worker_threads = 4;
  comp_engine second = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &second) == COMP_OK);

  COMP_CHECK(comp_engine_threads(first, &reported) == COMP_OK);
  COMP_CHECK_MSG(reported == 4u, "threads reflects the process-wide effective parallelism");

  comp_engine_destroy(second);
  comp_engine_destroy(first);
}
/* --------------------------------------- AUDIT3：组内重排必须改变堆叠序 */

/* P0-1 回归：组内 reorder 此前只改 parent->children 簿记向量，而合成器按
 * state.layers 权威序渲染——结果返回 COMP_OK、进历史、画面毫无变化。
 * 判据是端到端事实：组内两个子层交换后，合成结果必须翻转。 */
COMP_TEST(group_reorder_changes_stacking_order) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_layer_desc group_desc{};
  group_desc.name = "G";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  /* 组内两个实心子层：先蓝后绿 = 绿在上（数组更靠后 = 堆叠更靠上）。 */
  comp_layer_desc child_desc{};
  child_desc.name = "child";
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id child_a = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child_a) == COMP_OK);
  const SolidImage blue(4, 4, 0, 0, 255, 255);
  COMP_CHECK(comp_layer_set_pixels(document, child_a, &blue.image) == COMP_OK);

  child_desc.parent = group;
  comp_layer_id child_b = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child_b) == COMP_OK);
  const SolidImage green(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, child_b, &green.image) == COMP_OK);

  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 255, 0, 255),
                 "later child in the group stacks on top");

  /* 组内重排：child_b 移到 child_a 之前 -> 蓝在上。画面必须翻转，
     这正是旧实现只改 children 簿记、渲染毫无变化的那条路径。 */
  COMP_CHECK_MSG(comp_layer_reorder(document, child_b, child_a) == COMP_OK,
                 "in-group reorder is accepted");
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255),
                 "in-group reorder changes the rendered stacking order");

  /* before = 0：移到同级末尾（最上层）-> 绿复归。 */
  COMP_CHECK(comp_layer_reorder(document, child_b, 0) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 255, 0, 255),
                 "before=0 moves the child back to the top of its group");

  /* 组内重排同样进撤销栈：撤销一次回到「蓝在上」。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255),
                 "in-group reorder is undoable");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------ P0-1：图层名读写（get/set_name） */

/* 名字一直在文档里（desc 写入、.comp 落盘），此前没有读出口：宿主只能自己
 * 记一份，撤销/合并/读盘之后就和文档漂移。本用例钉住「文档是唯一真源」。 */
COMP_TEST(layer_name_round_trips_through_utf8_and_history) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);

  /* 新建文档的默认名是 Background（10 字节 + NUL）。查询模式只回长度。 */
  size_t required = 0;
  COMP_CHECK(comp_layer_get_name(document, background, nullptr, 0, &required) == COMP_OK);
  COMP_CHECK_MSG(required == 11u, "default layer name is 'Background' + NUL");
  char initial[16] = {};
  COMP_CHECK(comp_layer_get_name(document, background, initial, sizeof(initial), &required) ==
             COMP_OK);
  COMP_CHECK_MSG(std::strcmp(initial, "Background") == 0, "default name reads back verbatim");

  /* UTF-8 名字往返（中文 + 空格）。 */
  const char* chinese = "背景 副本";
  size_t undo_before = 0;
  size_t redo_before = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_before, &redo_before) == COMP_OK);
  COMP_CHECK(comp_layer_set_name(document, background, chinese) == COMP_OK);
  size_t undo_after = 0;
  size_t redo_after = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_after, &redo_after) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1u, "rename is exactly one undo step");

  COMP_CHECK(comp_layer_get_name(document, background, nullptr, 0, &required) == COMP_OK);
  COMP_CHECK_MSG(required == std::strlen(chinese) + 1u, "required bytes follow the UTF-8 length");
  char round_trip[32] = {};
  COMP_CHECK(comp_layer_get_name(document, background, round_trip, sizeof(round_trip), &required) ==
             COMP_OK);
  COMP_CHECK_MSG(std::strcmp(round_trip, chinese) == 0, "UTF-8 name round trips");

  /* 缓冲区不够：拒绝且不截断（out_required 仍然如实回报）。 */
  char too_small[4] = {'x', 'x', 'x', 0};
  size_t needed = 0;
  COMP_CHECK(comp_layer_get_name(document, background, too_small, sizeof(too_small), &needed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK_MSG(needed == required && too_small[0] == 'x',
                 "small buffer is rejected without truncation");

  /* 撤销回到旧名字，重做回到新名字。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  char undone[16] = {};
  COMP_CHECK(comp_layer_get_name(document, background, undone, sizeof(undone), &required) ==
             COMP_OK);
  COMP_CHECK_MSG(std::strcmp(undone, "Background") == 0, "undo restores the previous name");
  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  char redone[32] = {};
  COMP_CHECK(comp_layer_get_name(document, background, redone, sizeof(redone), &required) ==
             COMP_OK);
  COMP_CHECK_MSG(std::strcmp(redone, chinese) == 0, "redo restores the renamed value");

  /* 参数闸：空名 / 坏 UTF-8 / 控制字符 / 超长 / 不存在图层。 */
  COMP_CHECK(comp_layer_set_name(document, background, "") == COMP_ERR_INVALID_ARGUMENT);
  const char truncated_utf8[] = {'\xC3', '\x28', '\0'}; /* 两字节序列缺续字节 */
  COMP_CHECK(comp_layer_set_name(document, background, truncated_utf8) ==
             COMP_ERR_INVALID_ARGUMENT);
  const char surrogate[] = {'\xED', '\xA0', '\x80', '\0'}; /* U+D800 代理区 */
  COMP_CHECK(comp_layer_set_name(document, background, surrogate) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_set_name(document, background, "line\nbreak") ==
             COMP_ERR_INVALID_ARGUMENT);
  std::string too_long(1025u, 'x');
  COMP_CHECK(comp_layer_set_name(document, background, too_long.c_str()) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_set_name(document, 999999, "x") == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_set_name(document, background, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_name(document, 999999, nullptr, 0, &required) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_get_name(document, background, nullptr, 0, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);

  /* 被拒绝的写入没有改掉旧名字。 */
  char after_rejects[32] = {};
  COMP_CHECK(comp_layer_get_name(document, background, after_rejects, sizeof(after_rejects),
                                 &required) == COMP_OK);
  COMP_CHECK_MSG(std::strcmp(after_rejects, chinese) == 0,
                 "rejected names leave the previous name untouched");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ----------------------------------------------- P0-1：复制图层（Ctrl+J） */

COMP_TEST(duplicate_layer_deep_copies_and_is_one_undo_step) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_layer_desc child_desc{};
  child_desc.name = "blue";
  child_desc.blend = COMP_BLEND_NORMAL;
  /* 不透明度 1：副本叠在原件之上时画面逐字节不变（半透明层再叠一次会改变
   * 合成结果——那是正确的重复绘制，不是深拷贝的判据）。 */
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id blue = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &blue) == COMP_OK);
  const SolidImage blue_image(4, 4, 0, 0, 255, 255);
  COMP_CHECK(comp_layer_set_pixels(document, blue, &blue_image.image) == COMP_OK);

  size_t count_before = 0;
  COMP_CHECK(comp_layer_count(document, &count_before) == COMP_OK);
  size_t undo_before = 0;
  size_t redo_before = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_before, &redo_before) == COMP_OK);
  const std::vector<uint8_t> canvas_before_copy = flatten(document, 4, 4);

  comp_layer_id copy = 0;
  COMP_CHECK_MSG(comp_layer_duplicate(document, blue, &copy) == COMP_OK, "duplicate succeeds");
  COMP_CHECK_MSG(copy != 0 && copy != blue, "duplicate gets a fresh layer id");
  size_t count_after = 0;
  COMP_CHECK(comp_layer_count(document, &count_after) == COMP_OK);
  COMP_CHECK_MSG(count_after == count_before + 1u, "duplicate adds exactly one layer");
  size_t undo_after = 0;
  size_t redo_after = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_after, &redo_after) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1u, "duplicate is exactly one undo step");

  /* 属性与像素深拷贝：混合、不透明度、像素都跟原件一致。 */
  comp_blend_mode copy_blend = COMP_BLEND_NORMAL;
  COMP_CHECK(comp_layer_get_blend(document, copy, &copy_blend) == COMP_OK);
  COMP_CHECK_MSG(copy_blend == COMP_BLEND_NORMAL, "blend mode is copied");
  float copy_opacity = 1.0f;
  COMP_CHECK(comp_layer_get_opacity(document, copy, &copy_opacity) == COMP_OK);
  COMP_CHECK_MSG(std::fabs(copy_opacity - 1.0f) < 1e-6f, "opacity is copied");
  char copy_name[64] = {};
  size_t required = 0;
  COMP_CHECK(comp_layer_get_name(document, copy, copy_name, sizeof(copy_name), &required) ==
             COMP_OK);
  COMP_CHECK_MSG(std::strcmp(copy_name, "blue copy") == 0, "duplicate name gets a ' copy' suffix");

  /* 画面逐字节不变（同像素、同属性、落在原件之上）。 */
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(canvas == canvas_before_copy, "duplicate is render-invariant");

  /* 改副本不影响原件：深拷贝而不是共享像素。 */
  const SolidImage green(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, copy, &green.image) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(canvas != canvas_before_copy, "editing the copy changes the canvas");
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(canvas == canvas_before_copy,
                 "the original keeps its own pixels after undo of the copy edit");

  /* 撤销一次复制：图层消失。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  size_t count_undone = 0;
  COMP_CHECK(comp_layer_count(document, &count_undone) == COMP_OK);
  COMP_CHECK_MSG(count_undone == count_before, "undo removes the duplicated layer");
  comp_layer_id copy_after_undo = 0;
  COMP_CHECK(comp_layer_duplicate(document, blue, &copy_after_undo) == COMP_OK);

  /* 组的复制 = 整棵子树深拷贝。 */
  comp_layer_desc group_desc{};
  group_desc.name = "G";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);
  comp_layer_desc inner_desc{};
  inner_desc.name = "inner";
  inner_desc.blend = COMP_BLEND_NORMAL;
  inner_desc.opacity = 1.0f;
  inner_desc.transform = comp_rect{0, 0, 4, 4};
  inner_desc.parent = group;
  comp_layer_id inner = 0;
  COMP_CHECK(comp_layer_add(document, &inner_desc, &inner) == COMP_OK);
  const SolidImage yellow(4, 4, 255, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, inner, &yellow.image) == COMP_OK);

  size_t before_group_copy = 0;
  COMP_CHECK(comp_layer_count(document, &before_group_copy) == COMP_OK);
  comp_layer_id group_copy = 0;
  COMP_CHECK(comp_layer_duplicate(document, group, &group_copy) == COMP_OK);
  size_t after_group_copy = 0;
  COMP_CHECK(comp_layer_count(document, &after_group_copy) == COMP_OK);
  COMP_CHECK_MSG(after_group_copy == before_group_copy + 2u,
                 "duplicating a group deep copies its subtree");
  int32_t copy_is_group = 0;
  comp_layer_id copy_parent = 0;
  comp_layer_id copy_clipping = 0;
  COMP_CHECK(comp_layer_get_group_info(document, group_copy, &copy_is_group, &copy_parent,
                                       &copy_clipping) == COMP_OK);
  COMP_CHECK_MSG(copy_is_group != 0 && copy_parent == 0, "the group copy is a root-level group");

  /* 参数闸。 */
  COMP_CHECK(comp_layer_duplicate(document, 999999, &group_copy) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_duplicate(document, blue, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ---------------------------------- P0-1：跨父级移动（图层面板拖进/拖出组） */

COMP_TEST(move_layer_reparents_and_changes_stacking) {
  comp_engine_desc desc = mutation_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  comp_layer_desc group_desc{};
  group_desc.name = "G";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  /* 根级蓝层（在组之上）：画面是蓝。 */
  comp_layer_desc child_desc{};
  child_desc.name = "blue";
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id blue = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &blue) == COMP_OK);
  const SolidImage blue_image(4, 4, 0, 0, 255, 255);
  COMP_CHECK(comp_layer_set_pixels(document, blue, &blue_image.image) == COMP_OK);
  std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 1, 1, 0, 0, 255, 255));

  int32_t is_group = 0;
  comp_layer_id parent = 0;
  comp_layer_id clipping = 0;
  COMP_CHECK(comp_layer_get_group_info(document, blue, &is_group, &parent, &clipping) == COMP_OK);
  COMP_CHECK(parent == 0);

  /* 拖进组：parent 变成组，画面不变（组按子层逐个合成，属性全默认）。 */
  size_t undo_before = 0;
  size_t redo_before = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_before, &redo_before) == COMP_OK);
  COMP_CHECK_MSG(comp_layer_move(document, blue, group, 0) == COMP_OK, "move into group");
  size_t undo_after = 0;
  size_t redo_after = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_after, &redo_after) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1u, "move is exactly one undo step");
  COMP_CHECK(comp_layer_get_group_info(document, blue, &is_group, &parent, &clipping) == COMP_OK);
  COMP_CHECK_MSG(parent == group, "get_group_info reports the new parent");
  canvas = flatten(document, 4, 4);
  COMP_CHECK(matches(canvas, 4, 1, 1, 0, 0, 255, 255));

  /* 组内再加绿层（比蓝更靠上），然后把绿移到蓝之前：画面翻成蓝。 */
  child_desc.name = "green";
  child_desc.parent = group;
  comp_layer_id green = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &green) == COMP_OK);
  const SolidImage green_image(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, green, &green_image.image) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 255, 0, 255), "later sibling stacks on top");

  COMP_CHECK_MSG(comp_layer_move(document, green, group, blue) == COMP_OK,
                 "move before a sibling inside the group");
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 0, 255, 255),
                 "moving before the sibling changes the rendered order");

  /* 撤销一次回到「绿在上」。 */
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 255, 0, 255), "the move is undoable");

  /*
   * 回归：`before = 0` 是「移到组末尾（最上层）」，即使最后一个同父兄弟恰好是
   * 数组末元素也一样。旧写法用 `pos == end()` 判断「没找到兄弟」，那个情况下会
   * 误走「组还没有子层」分支，把新层插到组自身后面（= 组底），于是每拖一层进组
   * 都落到最下面——Ctrl+G 批量收层时整组堆叠序被静默反转。
   */
  comp_layer_desc cyan_desc{};
  cyan_desc.name = "cyan";
  cyan_desc.blend = COMP_BLEND_NORMAL;
  cyan_desc.opacity = 1.0f;
  cyan_desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id cyan = 0;
  COMP_CHECK(comp_layer_add(document, &cyan_desc, &cyan) == COMP_OK);
  const SolidImage cyan_image(4, 4, 0, 255, 255, 255);
  COMP_CHECK(comp_layer_set_pixels(document, cyan, &cyan_image.image) == COMP_OK);
  COMP_CHECK_MSG(comp_layer_move(document, cyan, group, 0) == COMP_OK,
                 "move into a non-empty group");
  canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 0, 255, 255, 255),
                 "before=0 lands on top of the group, not under its last child");

  /* 拖出组：parent = 0，root 末尾（最上层）仍是蓝；层级事实必须为 0。 */
  COMP_CHECK_MSG(comp_layer_move(document, blue, 0, 0) == COMP_OK, "move out of the group");
  COMP_CHECK(comp_layer_get_group_info(document, blue, &is_group, &parent, &clipping) == COMP_OK);
  COMP_CHECK_MSG(parent == 0 && is_group == 0, "moved layer is back at root level");

  /* 参数闸：非组父级 / 跨级锚点 / 成环 / 不存在的 id 与锚点。 */
  COMP_CHECK_MSG(comp_layer_move(document, blue, background, 0) == COMP_ERR_INVALID_ARGUMENT,
                 "parent must be a group");
  COMP_CHECK_MSG(comp_layer_move(document, blue, group, background) == COMP_ERR_INVALID_ARGUMENT,
                 "anchor must share the target parent");
  comp_layer_desc nested_desc{};
  nested_desc.name = "inner";
  nested_desc.blend = COMP_BLEND_NORMAL;
  nested_desc.opacity = 1.0f;
  nested_desc.transform = comp_rect{0, 0, 4, 4};
  nested_desc.is_group = 1;
  nested_desc.parent = group;
  comp_layer_id inner = 0;
  COMP_CHECK(comp_layer_add(document, &nested_desc, &inner) == COMP_OK);
  COMP_CHECK_MSG(comp_layer_move(document, group, inner, 0) == COMP_ERR_INVALID_ARGUMENT,
                 "a group cannot be moved into its own subtree");
  COMP_CHECK(comp_layer_move(document, group, group, 0) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_move(document, 999999, group, 0) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_move(document, group, 999999, 0) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_move(document, group, 0, 999999) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
