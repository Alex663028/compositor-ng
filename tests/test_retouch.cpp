// AI生成
/*
 * test_retouch.cpp — F3 修饰类工具（Clone Stamp / Smudge / Liquify）的回归护栏。
 *
 * 为什么需要这组用例：这三个算子是 F3 新增的产品通路（`comp_tool_apply` 的
 * kind = 9/10/11），像素算法在 domain/retouch.cpp。它们与既有 Stroke 的区别
 * 不在「画一笔」，而在**源从哪来**与**写回的依据**：
 *   - Clone 读的是「一笔开始时的快照」，快照可以是当前层或整篇合成；
 *   - Smudge 读的是跨 dab 携带的 carried 缓冲；
 *   - Liquify 读的是「本次 dab 之前的 scratch」，并且会移动像素。
 * 这三条都可能悄悄退化成「复制当前像素」或「原地自反馈」，肉眼难查，所以
 * 逐条冻结在 ABI 边界上。
 *
 * 本文件只用 C ABI，不引入引擎内部命名空间。
 */
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

/*
 * F3 的 kind 数值必须从 9 起：7/8 已被域层的 scanlines/chunky 占用但未进
 * ABI。这条 static_assert 是防回归的闸——若有人为了「补齐枚举」把 CLONE
 * 改成 7，域层与 ABI 就会在同一数值上指向两个不同算子。
 */
static_assert(COMP_TOOL_DEEP == 6, "F2 之前的 ABI kind 数值不得变动");
static_assert(COMP_TOOL_CLONE == 9, "clone must not reuse the domain-only 7/8 kinds");
static_assert(COMP_TOOL_SMUDGE == 10, "smudge must not reuse the domain-only 7/8 kinds");
static_assert(COMP_TOOL_LIQUIFY == 11, "liquify must not reuse the domain-only 7/8 kinds");

namespace {

constexpr int32_t kW = 80;
constexpr int32_t kH = 40;
constexpr size_t kStride = static_cast<size_t>(kW) * 4u;

struct Fixture {
  comp_engine engine = nullptr;
  comp_document doc = nullptr;
  comp_layer_id background = 0;
  comp_layer_id layer = 0;

  Fixture() {
    comp_engine_desc desc{};
    desc.app_name = "compositor-retouch-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    comp_document_desc doc_desc{};
    doc_desc.width = kW;
    doc_desc.height = kH;
    doc_desc.dpi = 72.0f;
    comp_engine_create(&desc, &engine);
    comp_document_create(engine, &doc_desc, &doc);

    comp_layer_id ids[8] = {};
    size_t count = 0;
    comp_layer_list(doc, ids, 8, &count);
    background = count > 0 ? ids[0] : 0;

    comp_layer_desc layer_desc{};
    layer_desc.name = "retouch";
    layer_desc.blend = COMP_BLEND_NORMAL;
    layer_desc.opacity = 1.0f;
    layer_desc.transform = comp_rect{0, 0, kW, kH};
    layer_desc.parent = 0;
    layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    comp_layer_add(doc, &layer_desc, &layer);
  }
  ~Fixture() {
    if (doc != nullptr) comp_document_destroy(doc);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
};

std::vector<uint8_t> solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4u, 0);
  for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
    pixels[i * 4 + 0] = r;
    pixels[i * 4 + 1] = g;
    pixels[i * 4 + 2] = b;
    pixels[i * 4 + 3] = a;
  }
  return pixels;
}

/* 在像素缓冲上填一个矩形（左闭右开）。 */
void fill_rect(std::vector<uint8_t>& pixels, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
               uint8_t r, uint8_t g, uint8_t b) {
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kW + x) * 4u;
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = 255;
    }
  }
}

void set_pixels(comp_document doc, comp_layer_id id, const std::vector<uint8_t>& pixels) {
  comp_image image{};
  image.width = kW;
  image.height = kH;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kStride;
  image.pixels = const_cast<uint8_t*>(pixels.data());
  COMP_CHECK(comp_layer_set_pixels(doc, id, &image) == COMP_OK);
}

std::vector<uint8_t> flatten(comp_document doc) {
  std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4u);
  COMP_CHECK(comp_render_flatten(doc, out.data(), COMP_PIXEL_RGBA8, kStride) == COMP_OK);
  return out;
}

const uint8_t* pixel(const std::vector<uint8_t>& buf, int32_t x, int32_t y) {
  return buf.data() + (static_cast<size_t>(y) * kW + x) * 4u;
}

comp_result apply(comp_document doc, comp_layer_id id, const comp_tool_op_desc& desc) {
  return comp_tool_apply(doc, id, &desc, nullptr, 0, nullptr);
}

/* 零初始化 = 各子结构缺省「什么都不改」；用例负责显式打开需要的字段。 */
comp_tool_op_desc base(comp_tool_op kind) {
  comp_tool_op_desc desc{};
  desc.kind = kind;
  desc.brush.diameter = 8.0f;
  desc.brush.pressure = 1.0f;
  desc.brush.hardness = 1.0f;
  desc.brush.flow = 1.0f;
  return desc;
}

/* 仿制图章：单点/折线笔迹 + 源点。 */
comp_tool_op_desc clone_desc(const float* points, int32_t count, double source_x, double source_y) {
  comp_tool_op_desc desc = base(COMP_TOOL_CLONE);
  desc.stroke_points = points;
  desc.stroke_point_count = count;
  desc.clone.source_x = source_x;
  desc.clone.source_y = source_y;
  desc.clone.has_source = 1;
  desc.clone.opacity = 1.0f;
  return desc;
}

comp_tool_op_desc warp_desc(comp_tool_op kind, const float* points, int32_t count, float strength) {
  comp_tool_op_desc desc = base(kind);
  desc.stroke_points = points;
  desc.stroke_point_count = count;
  desc.warp.strength = strength;
  return desc;
}

bool nearly(const uint8_t* px, int r, int g, int b) {
  return std::abs(static_cast<int>(px[0]) - r) <= 24 &&
         std::abs(static_cast<int>(px[1]) - g) <= 24 &&
         std::abs(static_cast<int>(px[2]) - b) <= 24;
}

}  // namespace

/* ================================================== 1. Clone Stamp */

COMP_TEST(retouch_clone_copies_the_source_under_the_brush) {
  Fixture fx;
  /* 蓝底 + 一处红标记作为源。 */
  std::vector<uint8_t> pixels = solid(20, 20, 220);
  fill_rect(pixels, 8, 8, 14, 14, 220, 20, 20);
  set_pixels(fx.doc, fx.layer, pixels);

  const float points[2] = {60.0f, 10.0f};
  comp_tool_op_desc desc = clone_desc(points, 1, 10.0, 10.0);
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_OK, "a valid clone stroke applies");

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(nearly(pixel(after, 60, 10), 220, 20, 20),
                 "the pixel under the brush takes the source colour");
  COMP_CHECK_MSG(nearly(pixel(after, 10, 10), 220, 20, 20), "the source itself is untouched");
  COMP_CHECK_MSG(nearly(pixel(after, 70, 30), 20, 20, 220),
                 "pixels outside the brush radius are untouched");
}

COMP_TEST(retouch_clone_aligned_keeps_the_first_offset_and_unaligned_recomputes) {
  Fixture fx;
  /* 绿标记在 8..13，红标记在 14..19；蓝底。 */
  std::vector<uint8_t> pixels = solid(20, 20, 220);
  fill_rect(pixels, 8, 8, 14, 14, 20, 200, 20);
  fill_rect(pixels, 14, 8, 20, 14, 220, 20, 20);
  set_pixels(fx.doc, fx.layer, pixels);

  /* 第一笔：源 (10,10)、落点 (60,10) -> 偏移 (-50,0)，采到绿色。 */
  const float first[2] = {60.0f, 10.0f};
  comp_tool_op_desc desc = clone_desc(first, 1, 10.0, 10.0);
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  {
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(nearly(pixel(after, 60, 10), 20, 200, 20),
                   "the first stroke samples source - first point");
  }

  /* 第二笔：aligned + 带回偏移 -> 落点 (66,10) 仍用 (-50,0)，采到红色。 */
  const float second[2] = {66.0f, 10.0f};
  desc = clone_desc(second, 1, 10.0, 10.0);
  desc.clone.aligned = 1;
  desc.clone.has_offset = 1;
  desc.clone.offset_x = -50.0;
  desc.clone.offset_y = 0.0;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  {
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(nearly(pixel(after, 66, 10), 220, 20, 20),
                   "aligned keeps the first stroke's offset (upstream -45,0 semantics)");
  }

  /* 第三笔：has_offset 仍在但 aligned=0 -> 按 source - 新落点重算，采到绿色。 */
  const float third[2] = {50.0f, 10.0f};
  desc = clone_desc(third, 1, 10.0, 10.0);
  desc.clone.aligned = 0;
  desc.clone.has_offset = 1;
  desc.clone.offset_x = -50.0;
  desc.clone.offset_y = 0.0;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  {
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(nearly(pixel(after, 50, 10), 20, 200, 20),
                   "without aligned the offset is recomputed from the new first point");
  }
}

COMP_TEST(retouch_clone_sample_all_layers_uses_the_composite_snapshot) {
  Fixture fx;
  /* 背景绿底 + 红块；目标层全透明。 */
  std::vector<uint8_t> background = solid(20, 200, 20);
  fill_rect(background, 4, 4, 10, 10, 220, 20, 20);
  set_pixels(fx.doc, fx.background, background);
  set_pixels(fx.doc, fx.layer, solid(0, 0, 0, 0));

  const float points[2] = {60.0f, 10.0f};
  comp_tool_op_desc desc = clone_desc(points, 1, 6.0, 6.0);
  desc.clone.sample_all_layers = 1;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  {
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(nearly(pixel(after, 60, 10), 220, 20, 20),
                   "sampleAllLayers reads the live composite, not just the target layer");
  }

  /* 撤销后同一点改成只取当前层：目标层透明 => 无像素可画，结果仍是背景绿。 */
  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  desc.clone.sample_all_layers = 0;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  {
    const std::vector<uint8_t> after = flatten(fx.doc);
    COMP_CHECK_MSG(nearly(pixel(after, 60, 10), 20, 200, 20),
                   "without sampleAllLayers a transparent source paints nothing");
  }
}

COMP_TEST(retouch_clone_without_source_is_a_state_error_and_leaves_no_history) {
  Fixture fx;
  set_pixels(fx.doc, fx.layer, solid(20, 20, 220));
  const std::vector<uint8_t> before = flatten(fx.doc);

  int32_t can_undo = 0;
  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  const int32_t undo_before = can_undo;

  const float points[2] = {60.0f, 10.0f};
  comp_tool_op_desc desc = base(COMP_TOOL_CLONE);
  desc.stroke_points = points;
  desc.stroke_point_count = 1;
  desc.clone.opacity = 1.0f; /* 有笔刷参数，但没有源 */
  COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_STATE,
                 "nothing to copy until a source is set is a state error");

  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  COMP_CHECK_MSG(can_undo == undo_before, "the rejected clone must not add an undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "the rejected clone must not touch pixels");
}

/* ================================================== 2. Smudge */

COMP_TEST(retouch_smudge_drags_colour_across_a_boundary) {
  Fixture fx;
  std::vector<uint8_t> pixels = solid(20, 20, 220);
  fill_rect(pixels, 0, 0, 40, kH, 220, 20, 20);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const float points[4] = {30.0f, 20.0f, 60.0f, 20.0f};
  comp_tool_op_desc desc = warp_desc(COMP_TOOL_SMUDGE, points, 2, 1.0f);
  desc.brush.diameter = 16.0f;
  desc.brush.hardness = 0.5f;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(nearly(pixel(after, 50, 20), 220, 20, 20),
                 "the carried colour is dragged forward past the boundary");
  COMP_CHECK_MSG(nearly(pixel(after, 75, 20), 20, 20, 220),
                 "pixels beyond the last dab's radius keep their colour");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "smudge is exactly one undo step");
}

COMP_TEST(retouch_smudge_single_point_only_picks_up_colour) {
  Fixture fx;
  std::vector<uint8_t> pixels = solid(20, 20, 220);
  fill_rect(pixels, 0, 0, 40, kH, 220, 20, 20);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const float points[2] = {30.0f, 20.0f};
  comp_tool_op_desc desc = warp_desc(COMP_TOOL_SMUDGE, points, 1, 1.0f);
  desc.brush.diameter = 16.0f;
  desc.brush.hardness = 0.5f;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before,
                 "a click only picks up colour; the first point produces no dab (upstream)");
}

/* ================================================== 3. Liquify (push) */

COMP_TEST(retouch_liquify_pushes_pixels_forward) {
  Fixture fx;
  std::vector<uint8_t> pixels = solid(0, 0, 0);
  fill_rect(pixels, 38, 0, 47, kH, 255, 255, 255);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const float points[4] = {30.0f, 20.0f, 50.0f, 20.0f};
  comp_tool_op_desc desc = warp_desc(COMP_TOOL_LIQUIFY, points, 2, 1.0f);
  desc.brush.diameter = 40.0f;
  desc.brush.hardness = 0.9f;
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);

  const std::vector<uint8_t> after = flatten(fx.doc);
  COMP_CHECK_MSG(nearly(pixel(after, 40, 20), 0, 0, 0),
                 "the white stripe is pulled away from x=40");
  COMP_CHECK_MSG(nearly(pixel(after, 60, 20), 255, 255, 255),
                 "the stripe arrives downstream (pixels are resampled, not stamped)");
  COMP_CHECK_MSG(nearly(pixel(after, 75, 20), 0, 0, 0),
                 "pixels outside the brush are untouched");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "liquify is exactly one undo step");
}

COMP_TEST(retouch_warp_strength_zero_is_a_byte_identical_no_op) {
  Fixture fx;
  std::vector<uint8_t> pixels = solid(20, 20, 220);
  fill_rect(pixels, 0, 0, 40, kH, 220, 20, 20);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const float points[4] = {30.0f, 20.0f, 60.0f, 20.0f};
  comp_tool_op_desc smudge = warp_desc(COMP_TOOL_SMUDGE, points, 2, 0.0f);
  smudge.brush.diameter = 16.0f;
  smudge.brush.hardness = 0.5f;
  COMP_CHECK(apply(fx.doc, fx.layer, smudge) == COMP_OK);

  comp_tool_op_desc liquify = warp_desc(COMP_TOOL_LIQUIFY, points, 2, 0.0f);
  liquify.brush.diameter = 40.0f;
  liquify.brush.hardness = 0.9f;
  COMP_CHECK(apply(fx.doc, fx.layer, liquify) == COMP_OK);

  COMP_CHECK_MSG(flatten(fx.doc) == before, "strength 0 must be a byte-identical no-op");
}

/* ================================================== 4. 参数闸与可重复性 */

COMP_TEST(retouch_rejects_bad_arguments_before_any_transaction) {
  Fixture fx;
  set_pixels(fx.doc, fx.layer, solid(20, 20, 220));
  const std::vector<uint8_t> before = flatten(fx.doc);

  int32_t can_undo = 0;
  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  const int32_t undo_before = can_undo;

  /* warp 直径超过内存安全上限（1024）。 */
  {
    const float points[2] = {30.0f, 20.0f};
    comp_tool_op_desc desc = warp_desc(COMP_TOOL_SMUDGE, points, 1, 1.0f);
    desc.brush.diameter = 2000.0f;
    COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                   "a warp diameter above the memory-safe cap is an argument error");
  }

  /* strength 越界（NaN 也走同一条闸）。 */
  {
    const float points[2] = {30.0f, 20.0f};
    comp_tool_op_desc desc = warp_desc(COMP_TOOL_LIQUIFY, points, 1, 2.0f);
    COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                   "warp strength above 1 is an argument error");
  }

  /* 坐标非有限 / 数量级离谱：取整会 UB，必须在闸里拒绝。 */
  {
    float points[2] = {30.0f, std::nanf("")};
    comp_tool_op_desc desc = clone_desc(points, 1, 10.0, 10.0);
    COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                   "a non-finite point is rejected");
  }
  {
    float points[2] = {1.0e7f, 20.0f};
    comp_tool_op_desc desc = clone_desc(points, 1, 10.0, 10.0);
    COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                   "a coordinate beyond the tool bound is rejected before rounding");
  }

  /* 指针为空。 */
  {
    comp_tool_op_desc desc = clone_desc(nullptr, 1, 10.0, 10.0);
    COMP_CHECK_MSG(apply(fx.doc, fx.layer, desc) == COMP_ERR_INVALID_ARGUMENT,
                   "a null point array is an argument error");
  }

  COMP_CHECK(comp_document_can_undo(fx.doc, &can_undo) == COMP_OK);
  COMP_CHECK_MSG(can_undo == undo_before, "rejected retouch ops must not add an undo step");
  COMP_CHECK_MSG(flatten(fx.doc) == before, "rejected retouch ops must leave pixels untouched");
}

COMP_TEST(retouch_clone_is_deterministic_across_replays) {
  Fixture fx;
  std::vector<uint8_t> pixels = solid(20, 20, 220);
  fill_rect(pixels, 4, 4, 20, 20, 220, 20, 20);
  set_pixels(fx.doc, fx.layer, pixels);
  const std::vector<uint8_t> before = flatten(fx.doc);

  const float points[4] = {55.0f, 20.0f, 65.0f, 20.0f};
  comp_tool_op_desc desc = clone_desc(points, 2, 10.0, 10.0);
  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  const std::vector<uint8_t> first = flatten(fx.doc);
  COMP_CHECK_MSG(first != before, "the clone stroke really changes pixels");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK(flatten(fx.doc) == before);

  COMP_CHECK(apply(fx.doc, fx.layer, desc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == first,
                 "the same clone parameters produce byte-identical output");

  COMP_CHECK(comp_document_undo(fx.doc) == COMP_OK);
  COMP_CHECK_MSG(flatten(fx.doc) == before, "clone is exactly one undo step");
}
