// AI生成
/*
 * test_eyedropper.cpp — 吸管取色（S5）的回归护栏。
 *
 * 冻结的行为：
 *   1. 输出是**非预乘** sRGB 0..1：不透明红就是 {1,0,0,1}，不是 {1,0,0,255}；
 *   2. 方形窗口按预乘值平均后再反预乘——半透明边缘不会因 alpha 加权而发亮；
 *   3. 窗口越出画布/图层时只统计真实存在的像素，整窗落空返回透明；
 *   4. 图层源只读目标图层，合并源读整篇合成（隐藏目标图层就取不到它）；
 *   5. 16 位图层走既有的 8 位工作副本，取到的是屏幕上的颜色；
 *   6. 只读：不开事务、不产生撤销步、不改像素；
 *   7. 错误码按语义分派（NOT_FOUND / STATE / INVALID_ARGUMENT）。
 */
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_eyedropper.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

constexpr int32_t kSide = 16;

struct Fixture {
  comp_engine engine = nullptr;
  comp_document document = nullptr;
  comp_layer_id layer = 0;

  Fixture() {
    comp_engine_desc desc{};
    desc.app_name = "compositor-eyedropper-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    if (comp_engine_create(&desc, &engine) != COMP_OK) return;

    comp_document_desc doc_desc{};
    doc_desc.width = kSide;
    doc_desc.height = kSide;
    doc_desc.dpi = 72.0f;
    if (comp_document_create(engine, &doc_desc, &document) != COMP_OK) return;

    comp_layer_desc layer_desc{};
    layer_desc.name = "eyedropper";
    layer_desc.blend = COMP_BLEND_NORMAL;
    layer_desc.opacity = 1.0f;
    layer_desc.transform = comp_rect{0, 0, kSide, kSide};
    layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    if (comp_layer_add(document, &layer_desc, &layer) != COMP_OK) layer = 0;
  }

  ~Fixture() {
    if (document != nullptr) comp_document_destroy(document);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
};

void set_pixels(comp_document document, comp_layer_id id, std::vector<uint8_t>& pixels) {
  comp_image image{};
  image.width = kSide;
  image.height = kSide;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(kSide) * 4u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(document, id, &image) == COMP_OK);
}

/* 全透明底 + 一块不透明纯色方块 [x0, x1) x [y0, y1)。 */
std::vector<uint8_t> block_pixels(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r,
                                  uint8_t g, uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kSide) * kSide * 4u, 0u);
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      uint8_t* px =
          pixels.data() + (static_cast<size_t>(y) * kSide + static_cast<size_t>(x)) * 4u;
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = 255;
    }
  }
  return pixels;
}

bool near(float value, float expected, float tolerance = 0.01f) {
  return std::fabs(value - expected) <= tolerance;
}

}  // namespace

COMP_TEST(eyedropper_layer_source_returns_straight_srgb) {
  Fixture fx;
  COMP_CHECK(fx.document != nullptr);

  std::vector<uint8_t> pixels = block_pixels(0, 0, kSide, kSide, 255, 64, 32);
  set_pixels(fx.document, fx.layer, pixels);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.document, &undo_before, nullptr) == COMP_OK);

  float rgba[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.5, 8.5, 1, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_OK);
  COMP_CHECK_MSG(near(rgba[0], 1.0f), "red is 1.0, not 255");
  COMP_CHECK_MSG(near(rgba[1], 64.0f / 255.0f), "green is normalized");
  COMP_CHECK_MSG(near(rgba[2], 32.0f / 255.0f), "blue is normalized");
  COMP_CHECK_MSG(near(rgba[3], 1.0f), "alpha is 1.0 for an opaque pixel");

  /* 只读：不动历史、不挂事务。 */
  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.document, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before, "sampling records no undo step");
  int32_t txn_active = -1;
  COMP_CHECK(comp_txn_active(fx.document, &txn_active) == COMP_OK);
  COMP_CHECK_MSG(txn_active == 0, "sampling leaves no open transaction");
}

COMP_TEST(eyedropper_averages_premultiplied_coverage) {
  Fixture fx;
  COMP_CHECK(fx.document != nullptr);

  /* 只有一个不透明红像素，其余全透明。 */
  std::vector<uint8_t> pixels = block_pixels(0, 0, 1, 1, 255, 0, 0);
  set_pixels(fx.document, fx.layer, pixels);

  /*
   * 在 (0,0) 取 3x3：窗口被画布裁到 4 个像素，其中 1 个 alpha 255。
   * 预乘平均后 alpha = 0.25；反预乘的直通色仍是纯红（1,0,0）——
   * 若先按直通色平均再乘 alpha，颜色会被邻像素的透明黑拉暗。
   */
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 0.0, 0.0, 3, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_OK);
  COMP_CHECK_MSG(near(rgba[0], 1.0f), "the straight color is not darkened by transparent neighbors");
  COMP_CHECK_MSG(near(rgba[1], 0.0f), "no green");
  COMP_CHECK_MSG(near(rgba[3], 0.25f), "alpha is the coverage of the window");

  /* 整窗落在透明区域：返回全 0（透明）。 */
  float empty[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 12.0, 12.0, 5, COMP_EYEDROPPER_LAYER,
                                    empty) == COMP_OK);
  COMP_CHECK(empty[0] == 0.0f && empty[1] == 0.0f && empty[2] == 0.0f && empty[3] == 0.0f);
}

COMP_TEST(eyedropper_merged_source_reads_the_composite) {
  Fixture fx;
  COMP_CHECK(fx.document != nullptr);

  std::vector<uint8_t> pixels = block_pixels(0, 0, kSide, kSide, 0, 200, 0);
  set_pixels(fx.document, fx.layer, pixels);

  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, 0, 8.0, 8.0, 1, COMP_EYEDROPPER_MERGED, rgba) ==
             COMP_OK);
  COMP_CHECK_MSG(near(rgba[0], 0.0f) && near(rgba[1], 200.0f / 255.0f) && near(rgba[2], 0.0f),
                 "merged source returns the layer's composite color");

  /* 隐藏图层后合成里没有它：合并源应取到透明，图层源仍能取到颜色。 */
  COMP_CHECK(comp_layer_set_visible(fx.document, fx.layer, 0) == COMP_OK);
  float merged_hidden[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, 0, 8.0, 8.0, 1, COMP_EYEDROPPER_MERGED,
                                    merged_hidden) == COMP_OK);
  COMP_CHECK_MSG(merged_hidden[3] == 0.0f, "a hidden layer is absent from the composite");

  float layer_direct[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    layer_direct) == COMP_OK);
  COMP_CHECK_MSG(near(layer_direct[1], 200.0f / 255.0f),
                 "the layer source ignores visibility");
}

COMP_TEST(eyedropper_deep_layer_uses_the_8bit_working_copy) {
  Fixture fx;
  COMP_CHECK(fx.document != nullptr);

  std::vector<uint16_t> wide(static_cast<size_t>(kSide) * kSide * 4u, 0u);
  for (size_t i = 0; i < static_cast<size_t>(kSide) * kSide; ++i) {
    wide[i * 4u + 0] = 65535u;
    wide[i * 4u + 3] = 65535u;
  }
  comp_image image{};
  image.width = kSide;
  image.height = kSide;
  image.format = COMP_PIXEL_RGBA16;
  image.stride = static_cast<size_t>(kSide) * 4u * sizeof(uint16_t);
  image.pixels = wide.data();
  COMP_CHECK(comp_layer_set_pixels(fx.document, fx.layer, &image) == COMP_OK);

  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_OK);
  COMP_CHECK_MSG(rgba[0] > 0.9f, "a saturated 16-bit red reads as red");
  COMP_CHECK_MSG(rgba[1] < 0.1f && rgba[2] < 0.1f, "the other channels stay dark");
  COMP_CHECK_MSG(rgba[3] > 0.9f, "alpha survives the 8-bit working copy");
}

COMP_TEST(eyedropper_rejects_bad_arguments_and_bad_layers) {
  Fixture fx;
  COMP_CHECK(fx.document != nullptr);
  std::vector<uint8_t> pixels = block_pixels(0, 0, kSide, kSide, 10, 20, 30);
  set_pixels(fx.document, fx.layer, pixels);

  /* 组图层没有像素：STATE（与图层无像素同码）。 */
  comp_layer_desc group_desc{};
  group_desc.name = "group";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(fx.document, &group_desc, &group) == COMP_OK);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(fx.document, &undo_before, nullptr) == COMP_OK);

  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, -1.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, std::nan(""), 8.0, 1,
                                    COMP_EYEDROPPER_LAYER, rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0,
                                    std::numeric_limits<double>::infinity(), 1,
                                    COMP_EYEDROPPER_LAYER, rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, static_cast<double>(kSide), 1,
                                    COMP_EYEDROPPER_LAYER, rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 2, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 0, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 103, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, fx.layer, 8.0, 8.0, 1,
                                    static_cast<comp_eyedropper_source>(7),
                                    rgba) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_eyedropper_sample(fx.document, 424242, 8.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_eyedropper_sample(nullptr, fx.layer, 8.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_INVALID_ARGUMENT);

  COMP_CHECK(comp_eyedropper_sample(fx.document, group, 8.0, 8.0, 1, COMP_EYEDROPPER_LAYER,
                                    rgba) == COMP_ERR_STATE);

  /* 被拒绝的调用不产生撤销步。 */
  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(fx.document, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before, "rejected samples leave no history");
}
