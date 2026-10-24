// AI生成
/*
 * test_layer_locks.cpp — P1-2 第一批：图层锁 + 颜色标签的端到端测试。
 *
 * 判据不是「API 返回 COMP_OK」，而是三件可观测事实：
 *   1. 锁真的**挡住**对应的写入口（返回 COMP_ERR_STATE，且像素/文档状态逐字节不变）；
 *   2. 锁**不**挡不该挡的入口（图像锁不拦几何/蒙版，位置锁不拦层序重排）；
 *   3. 锁与颜色标签随撤销/重做与 `.comp` 往返，同时不改变画面。
 *
 * 透明锁额外验证事后 alpha 还原的像素口径：全透明处整份还原、半透明处按
 * old_alpha/new_alpha 重新预乘、整步无可见改动时不产生空撤销。
 *
 * 本文件只使用 comp_*.h 中的 C 符号，与三端外壳的集成路径一致。
 */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_canvas.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_filter.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

comp_engine_desc lock_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-layer-lock-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* 4 字节/像素的 RGBA8 缓冲：按行优先、自上而下。 */
struct Rgba {
  std::vector<uint8_t> bytes;
  comp_image image{};

  Rgba(int32_t width, int32_t height)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u) {
    image.width = width;
    image.height = height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(width) * 4u;
    image.pixels = bytes.data();
  }

  void set(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
                      static_cast<size_t>(x)) * 4u;
    bytes[i] = r;
    bytes[i + 1] = g;
    bytes[i + 2] = b;
    bytes[i + 3] = a;
  }

  bool at(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) const {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
                      static_cast<size_t>(x)) * 4u;
    if (i + 3 >= bytes.size()) return false;
    return bytes[i] == r && bytes[i + 1] == g && bytes[i + 2] == b && bytes[i + 3] == a;
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

comp_layer_id add_layer(comp_document document, const char* name, int32_t width, int32_t height) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, width, height};
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  return id;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/layer_locks", ec);
  return std::string("build/layer_locks/") + name;
}

/* 一次覆盖整层的蓝色纯色填充（形状工具，硬边、无抗锯齿）。 */
comp_result paint_blue_over_layer(comp_document document, comp_layer_id id, int32_t width,
                                  int32_t height) {
  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_SHAPE;
  desc.shape.kind = COMP_SHAPE_RECT;
  desc.shape.x0 = -1.0;
  desc.shape.y0 = -1.0;
  desc.shape.x1 = static_cast<double>(width) + 1.0;
  desc.shape.y1 = static_cast<double>(height) + 1.0;
  desc.shape.style.fill_enabled = 1;
  desc.shape.style.fill_color[0] = 0;
  desc.shape.style.fill_color[1] = 0;
  desc.shape.style.fill_color[2] = 255;
  desc.shape.style.fill_color[3] = 255;
  desc.shape.style.opacity = 1.0f;
  desc.shape.style.antialias = 0;
  return comp_tool_apply(document, id, &desc, nullptr, 0u, nullptr);
}

comp_result vignette(comp_document document, comp_layer_id id, int32_t* out_changed) {
  comp_filter_desc desc{};
  desc.kind = COMP_FILTER_VIGNETTE;
  desc.vignette.amount = 30.0;
  desc.vignette.midpoint = 50.0;
  desc.vignette.feather = 50.0;
  return comp_filter_apply(document, id, &desc, 1.0, out_changed, nullptr, 0u, nullptr);
}

}  // namespace

/* ---------------------------------------------------------- 锁：读写与撤销 */

COMP_TEST(layer_lock_set_get_undo_and_validation) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = background_layer(document);
  COMP_CHECK(layer != 0);
  uint32_t flags = 99;
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK_MSG(flags == 0u, "new layers are unlocked");

  COMP_CHECK(comp_layer_set_lock(document, layer,
                                 COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_POSITION) ==
             COMP_OK);
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK_MSG(flags == (COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_POSITION),
                 "combined lock bits round-trip");

  /* 同值重复设置是幂等的：不产生撤销步（撤销一次应回到未锁）。 */
  COMP_CHECK(comp_layer_set_lock(document, layer,
                                 COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_POSITION) ==
             COMP_OK);
  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK_MSG(flags == 0u, "an idempotent set did not push an empty undo step");

  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK(flags == (COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_POSITION));

  /* 未知位一律拒绝，旧值不丢。 */
  COMP_CHECK(comp_layer_set_lock(document, layer, 0x10u) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK_MSG(flags == (COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_POSITION),
                 "rejected flags leave the old value intact");

  COMP_CHECK(comp_layer_set_lock(document, layer, COMP_LAYER_LOCK_NONE) == COMP_OK);
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK(flags == 0u);

  /* 参数与目标校验。 */
  uint32_t ignored = 0;
  COMP_CHECK(comp_layer_get_lock(document, layer, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_lock(document, 999999, &ignored) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_set_lock(document, 999999, 1u) == COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------------ 颜色标签：读写与撤销 */

COMP_TEST(layer_color_label_set_get_undo_and_validation) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = background_layer(document);
  COMP_CHECK(layer != 0);
  uint32_t label = 99;
  COMP_CHECK(comp_layer_get_color_label(document, layer, &label) == COMP_OK);
  COMP_CHECK_MSG(label == COMP_LAYER_COLOR_NONE, "new layers have no color label");

  COMP_CHECK(comp_layer_set_color_label(document, layer, COMP_LAYER_COLOR_VIOLET) == COMP_OK);
  COMP_CHECK(comp_layer_get_color_label(document, layer, &label) == COMP_OK);
  COMP_CHECK(label == COMP_LAYER_COLOR_VIOLET);

  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(comp_layer_get_color_label(document, layer, &label) == COMP_OK);
  COMP_CHECK_MSG(label == COMP_LAYER_COLOR_NONE, "color label is undoable");

  /* 越界不写、不丢旧值。 */
  COMP_CHECK(comp_layer_set_color_label(document, layer, COMP_LAYER_COLOR_COUNT) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_get_color_label(document, layer, &label) == COMP_OK);
  COMP_CHECK(label == COMP_LAYER_COLOR_NONE);

  COMP_CHECK(comp_layer_get_color_label(document, layer, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_set_color_label(document, 999999, COMP_LAYER_COLOR_RED) ==
             COMP_ERR_NOT_FOUND);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------- 图像锁：挡像素写入口，不挡几何/蒙版 */

COMP_TEST(layer_pixels_lock_blocks_pixel_writes_but_not_geometry) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = background_layer(document);
  COMP_CHECK(layer != 0);
  Rgba red(4, 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) red.set(x, y, 255, 0, 0, 255);
  }
  COMP_CHECK(comp_layer_set_pixels(document, layer, &red.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_lock(document, layer, COMP_LAYER_LOCK_PIXELS) == COMP_OK);

  /* 四个像素写入口全部拒绝。 */
  COMP_CHECK(paint_blue_over_layer(document, layer, 4, 4) == COMP_ERR_STATE);
  int32_t changed = -1;
  COMP_CHECK(vignette(document, layer, &changed) == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_invert(document, layer, &changed) == COMP_ERR_STATE);
  Rgba blue(4, 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) blue.set(x, y, 0, 0, 255, 255);
  }
  COMP_CHECK(comp_layer_set_pixels(document, layer, &blue.image) == COMP_ERR_STATE);

  /* 拒绝是「什么都没发生」：像素仍是红，且没有留下半条历史。 */
  uint32_t flags = 0;
  COMP_CHECK(comp_layer_get_lock(document, layer, &flags) == COMP_OK);
  COMP_CHECK(flags == COMP_LAYER_LOCK_PIXELS);

  /* 几何与蒙版不受图像锁影响（PS 口径）。 */
  COMP_CHECK(comp_layer_set_transform(document, layer, comp_rect{0, 0, 4, 4}, 0.0f) == COMP_OK);
  COMP_CHECK(comp_layer_set_flip(document, layer, 0, 0) == COMP_OK);
  const SolidMask mask(4, 4, 255);
  COMP_CHECK(comp_layer_set_mask(document, layer, &mask.mask) == COMP_OK);

  /* 解锁后同一批入口恢复正常：证明闸是锁，而不是入口本身坏了。 */
  COMP_CHECK(comp_layer_set_lock(document, layer, COMP_LAYER_LOCK_NONE) == COMP_OK);
  COMP_CHECK(paint_blue_over_layer(document, layer, 4, 4) == COMP_OK);
  changed = -1;
  COMP_CHECK(vignette(document, layer, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "filter reports a change once unlocked");
  COMP_CHECK(comp_layer_set_pixels(document, layer, &red.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, layer, &blue.image) == COMP_OK);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------- 位置锁：挡几何，不挡层序/混合参数 */

COMP_TEST(layer_position_lock_blocks_transform_but_not_stack_order) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const comp_layer_id top = add_layer(document, "top", 4, 4);
  COMP_CHECK(background != 0 && top != 0);
  COMP_CHECK(comp_layer_set_lock(document, top, COMP_LAYER_LOCK_POSITION) == COMP_OK);

  COMP_CHECK(comp_layer_set_transform(document, top, comp_rect{1, 1, 2, 2}, 45.0f) ==
             COMP_ERR_STATE);
  COMP_CHECK(comp_layer_set_flip(document, top, 1, 0) == COMP_ERR_STATE);

  /* 堆叠次序不是位置：reorder / move 仍允许（PS 同口径）。 */
  COMP_CHECK(comp_layer_reorder(document, top, background) == COMP_OK);
  COMP_CHECK(comp_layer_move(document, top, 0, background) == COMP_OK);

  /* 混合模式与不透明度也不是位置。 */
  COMP_CHECK(comp_layer_set_blend(document, top, COMP_BLEND_MULTIPLY) == COMP_OK);
  COMP_CHECK(comp_layer_set_opacity(document, top, 0.5f) == COMP_OK);

  COMP_CHECK(comp_layer_set_lock(document, top, COMP_LAYER_LOCK_NONE) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, top, comp_rect{1, 1, 2, 2}, 45.0f) == COMP_OK);
  COMP_CHECK(comp_layer_set_flip(document, top, 1, 0) == COMP_OK);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------- 全锁：还挡删除/合并/蒙版/调整/效果 */

COMP_TEST(layer_all_lock_covers_delete_merge_mask_adjustment_effect) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id background = background_layer(document);
  const comp_layer_id top = add_layer(document, "top", 4, 4);
  COMP_CHECK(background != 0 && top != 0);
  Rgba red(4, 4);
  Rgba blue(4, 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      red.set(x, y, 255, 0, 0, 255);
      blue.set(x, y, 0, 0, 255, 255);
    }
  }
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, top, &blue.image) == COMP_OK);
  /* update / remove 的闸排在「有没有调整」之后，所以先挂一条再上锁。 */
  COMP_CHECK(comp_layer_add_adjustment(document, top, 0u, "{}") == COMP_OK);
  COMP_CHECK(comp_layer_set_lock(document, top, COMP_LAYER_LOCK_ALL) == COMP_OK);

  COMP_CHECK(comp_layer_remove(document, top) == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_merge_down(document, top) == COMP_ERR_STATE);
  const SolidMask mask(4, 4, 128);
  COMP_CHECK(comp_layer_set_mask(document, top, &mask.mask) == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_add_adjustment(document, top, 0u, "{}") == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_update_adjustment(document, top, "{}") == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_remove_adjustment(document, top, 0u) == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_set_effect(document, top, 0u, "{}") == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_remove_effect(document, top, 0u) == COMP_ERR_STATE);

  /* 魔法棒写蒙版，也归全锁管。 */
  comp_tool_op_desc wand{};
  wand.kind = COMP_TOOL_SELECT;
  wand.select.seed_x = 0;
  wand.select.seed_y = 0;
  COMP_CHECK(comp_tool_apply(document, top, &wand, nullptr, 0u, nullptr) == COMP_ERR_STATE);

  /* 目标层没被删掉，锁还是全锁。 */
  size_t count = 0;
  COMP_CHECK(comp_layer_count(document, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 2u, "locked delete really removed nothing");
  uint32_t flags = 0;
  COMP_CHECK(comp_layer_get_lock(document, top, &flags) == COMP_OK);
  COMP_CHECK(flags == COMP_LAYER_LOCK_ALL);

  /* 解锁后删除/调整恢复正常。 */
  COMP_CHECK(comp_layer_set_lock(document, top, COMP_LAYER_LOCK_NONE) == COMP_OK);
  COMP_CHECK(comp_layer_remove_adjustment(document, top, 0u) == COMP_OK);
  COMP_CHECK(comp_layer_merge_down(document, top) == COMP_OK);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------- 透明锁：事后 alpha 还原的像素口径 */

COMP_TEST(layer_transparent_lock_preserves_alpha) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = background_layer(document);
  COMP_CHECK(layer != 0);
  /*
   * 四列分别覆盖三种 alpha 口径：
   *   x=0 全透明（0,0,0,0）
   *   x=1 半透明红（预乘后 128,0,0,128）
   *   x=2/3 不透明红
   */
  Rgba base(4, 4);
  for (int y = 0; y < 4; ++y) {
    base.set(0, y, 0, 0, 0, 0);
    base.set(1, y, 128, 0, 0, 128);
    base.set(2, y, 255, 0, 0, 255);
    base.set(3, y, 255, 0, 0, 255);
  }
  COMP_CHECK(comp_layer_set_pixels(document, layer, &base.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_lock(document, layer, COMP_LAYER_LOCK_TRANSPARENT) == COMP_OK);

  /* 整层刷成不透明蓝：透明列必须整份还原，半透明列必须按原 alpha 重新预乘。 */
  COMP_CHECK(paint_blue_over_layer(document, layer, 4, 4) == COMP_OK);

  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  COMP_CHECK(comp_layer_get_pixels(document, layer, nullptr, 0u, &width, &height, &format) ==
             COMP_OK);
  COMP_CHECK(width == 4 && height == 4 && format == COMP_PIXEL_RGBA8);
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> out(static_cast<size_t>(height) * stride, 0xCDu);
  COMP_CHECK(comp_layer_get_pixels(document, layer, out.data(), stride, &width, &height, &format) ==
             COMP_OK);
  auto at = [&out, width](int x, int y, int c) -> int {
    return static_cast<int>(
        out[static_cast<size_t>(y) * static_cast<size_t>(width) * 4u + static_cast<size_t>(x) * 4u +
            static_cast<size_t>(c)]);
  };
  COMP_CHECK_MSG(at(0, 0, 3) == 0 && at(0, 0, 2) == 0 && at(0, 0, 0) == 0,
                 "transparent pixels stay fully transparent");
  COMP_CHECK_MSG(at(1, 0, 3) == 128, "semi-transparent alpha is restored");
  COMP_CHECK_MSG(at(1, 0, 2) == 128 && at(1, 0, 0) == 0,
                 "semi-transparent colour is re-premultiplied by old_alpha/new_alpha");
  COMP_CHECK_MSG(at(2, 0, 3) == 255 && at(2, 0, 2) == 255,
                 "opaque pixels keep their new colour and alpha");

  /*
   * 反相不是「绘画类工具」，透明锁不管它（要拦它用 PIXELS）；而反相本身只改
   * RGB、不动 alpha，所以透明像素仍是空像素、不透明像素仍不透明。
   */
  int32_t changed = 0;
  COMP_CHECK(comp_layer_invert(document, layer, &changed) == COMP_OK);
  COMP_CHECK(comp_layer_get_pixels(document, layer, out.data(), stride, &width, &height, &format) ==
             COMP_OK);
  COMP_CHECK_MSG(at(0, 0, 3) == 0, "invert is not blocked by a transparency lock");
  COMP_CHECK_MSG(at(2, 0, 3) == 255 && at(2, 0, 0) == 255 && at(2, 0, 1) == 255 &&
                     at(2, 0, 2) == 0,
                 "invert turns opaque blue into opaque yellow");

  comp_layer_set_lock(document, layer, COMP_LAYER_LOCK_NONE);

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* 透明锁下的空笔迹（全落在透明处 / 全被还原）不产生撤销步。 */
COMP_TEST(layer_transparent_lock_empty_stroke_leaves_no_undo_step) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = background_layer(document);
  COMP_CHECK(layer != 0);
  Rgba empty(4, 4); /* 全透明 */
  COMP_CHECK(comp_layer_set_pixels(document, layer, &empty.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_lock(document, layer, COMP_LAYER_LOCK_TRANSPARENT) == COMP_OK);

  /* 涂满整层的形状：每一个像素都会被 alpha 还原，整步应被撤回。 */
  size_t undo_before = 0;
  size_t redo_before = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_before, &redo_before) == COMP_OK);
  COMP_CHECK(paint_blue_over_layer(document, layer, 4, 4) == COMP_OK);

  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  COMP_CHECK(comp_layer_get_pixels(document, layer, nullptr, 0u, &width, &height, &format) ==
             COMP_OK);
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> out(static_cast<size_t>(height) * stride, 0xCDu);
  COMP_CHECK(comp_layer_get_pixels(document, layer, out.data(), stride, &width, &height, &format) ==
             COMP_OK);
  bool all_empty = true;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] != 0u) all_empty = false;
  }
  COMP_CHECK_MSG(all_empty, "a stroke on transparent pixels is discarded entirely");

  /*
   * 没有撤销步：栈深不变（用 depth 而不是「撤销必须失败」——设锁本身也是一步）。
   */
  size_t undo_after = 0;
  size_t redo_after = 0;
  COMP_CHECK(comp_document_history_stats(document, &undo_after, &redo_after) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before,
                 "a fully-restored stroke must not push an undo step");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* --------------------------------------- .comp 往返：engineLock / engineColorLabel */

COMP_TEST(layer_lock_and_label_round_trip_through_comp) {
  comp_engine_desc desc = lock_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id locked = background_layer(document);
  const comp_layer_id plain = add_layer(document, "plain", 4, 4);
  COMP_CHECK(locked != 0 && plain != 0);
  Rgba red(4, 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) red.set(x, y, 255, 0, 0, 255);
  }
  COMP_CHECK(comp_layer_set_pixels(document, locked, &red.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_lock(document, locked,
                                 COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_PIXELS) ==
             COMP_OK);
  COMP_CHECK(comp_layer_set_color_label(document, locked, COMP_LAYER_COLOR_BLUE) == COMP_OK);

  const std::string path = work_path("lock_round_trip.comp");
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(contains(manifest, "\"engineLock\""), "engineLock is written");
  COMP_CHECK_MSG(contains(manifest, "\"engineColorLabel\""), "engineColorLabel is written");
  /* 默认值不写：普通图层不该凭空多出这两个键。 */
  const size_t lock_key = manifest.find("\"engineLock\"");
  const size_t label_key = manifest.find("\"engineColorLabel\"");
  COMP_CHECK(lock_key != std::string::npos && label_key != std::string::npos);
  COMP_CHECK_MSG(manifest.find("\"engineLock\"", lock_key + 1u) == std::string::npos,
                 "only the locked layer writes engineLock");
  COMP_CHECK_MSG(manifest.find("\"engineColorLabel\"", label_key + 1u) == std::string::npos,
                 "only the labelled layer writes engineColorLabel");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, path.c_str(), &loaded) == COMP_OK);
  if (loaded != nullptr) {
    uint32_t flags = 0;
    uint32_t label = 0;
    comp_layer_id ids[4] = {0, 0, 0, 0};
    size_t listed = 0;
    COMP_CHECK(comp_layer_list(loaded, ids, 4u, &listed) == COMP_OK);
    comp_layer_id restored_locked = 0;
    comp_layer_id restored_plain = 0;
    for (size_t i = 0; i < listed; ++i) {
      uint32_t f = 0;
      if (comp_layer_get_lock(loaded, ids[i], &f) == COMP_OK && f ==
              (COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_PIXELS)) {
        restored_locked = ids[i];
      } else {
        restored_plain = ids[i];
      }
    }
    COMP_CHECK_MSG(restored_locked != 0, "lock survives a .comp round trip");
    COMP_CHECK(comp_layer_get_color_label(loaded, restored_locked, &label) == COMP_OK);
    COMP_CHECK_MSG(label == COMP_LAYER_COLOR_BLUE, "color label survives a .comp round trip");
    COMP_CHECK(comp_layer_get_lock(loaded, restored_locked, &flags) == COMP_OK);
    COMP_CHECK(flags == (COMP_LAYER_LOCK_TRANSPARENT | COMP_LAYER_LOCK_PIXELS));
    if (restored_plain != 0) {
      COMP_CHECK(comp_layer_get_lock(loaded, restored_plain, &flags) == COMP_OK);
      COMP_CHECK_MSG(flags == 0u, "a plain layer loads unlocked");
      COMP_CHECK(comp_layer_get_color_label(loaded, restored_plain, &label) == COMP_OK);
      COMP_CHECK_MSG(label == COMP_LAYER_COLOR_NONE, "a plain layer loads with no label");
    }

    /* 再存一次：键与值都保真（不因默认值规则被吞掉）。 */
    const std::string path2 = work_path("lock_round_trip_2.comp");
    COMP_CHECK(comp_document_save(loaded, path2.c_str()) == COMP_OK);
    const std::string manifest2 = read_text(path2 + "/manifest.json");
    COMP_CHECK_MSG(contains(manifest2, "\"engineLock\""), "lock is written again on save");
    COMP_CHECK_MSG(contains(manifest2, "\"engineColorLabel\""),
                   "color label is written again on save");
    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
