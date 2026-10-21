// AI生成
/*
 * test_canvas_ops.cpp — S25「图像」菜单三件套（comp_canvas.h）+ 色彩范围
 * （comp_selection.h 的 S25 追加口）。
 *
 * 四个入口：
 *   comp_document_rotate_canvas        Image > Image Rotation（quarterTurned）
 *   comp_document_trim                 Image > Trim（calculateTrimRect + 裁剪提交）
 *   comp_layer_invert                  Image > Adjustments > Invert（PixelInvert）
 *   comp_selection_apply_color_range   Select > Color Range（ColorRangeSelection）
 *
 * 判据是端到端事实（与 test_crop / test_selection 同口径），不是「返回 COMP_OK」：
 *   - 旋转/修边**不重采样**：图层像素逐字节不动，只动 origin / rotation / 画布尺寸，
 *     合成结果等于「把原合成图按几何变换整体转置」；
 *   - 旋转后的选区按 90°/180° 精确随动（不是丢弃）；
 *   - 修边边框由逐行扫描算法给出（行中间的不匹配像素不改变边界），颜色基准是
 *     角点像素 + 逐通道容差；
 *   - 反相是预乘语义 c' = alpha - c（透明像素仍是透明），选区按文档坐标裁剪；
 *   - 色彩范围全局匹配、不要求连续（与魔棒 flood fill 的本质区别），include/exclude
 *     双表、fuzziness 与 invert 的组合都用具体像素钉死；
 *   - 一步撤销（显式事务内多步合并），参数闸在事务之前，失败调用无副作用。
 *
 * 大多数用例走 C ABI（与三端外壳同一条集成路径）；16 位 / HDR 图层与显式空选区
 * 用域层 Document 直接构造再 reinterpret_cast 成句柄（test_filters.cpp 同款）。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "compositor/comp_canvas.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_selection.h"
#include "compositor/comp_undo.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "test_framework.h"

namespace {

/* ---------------------------------------------------------------- helpers */

comp_engine_desc canvas_engine_desc(const char* app_name) {
  comp_engine_desc desc{};
  desc.app_name = app_name;
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* C ABI 文档（宽高可配）。 */
struct Doc {
  comp_engine engine = nullptr;
  comp_document document = nullptr;
  int32_t width = 0;
  int32_t height = 0;

  Doc(int32_t w, int32_t h, const char* app_name = "compositor-canvas-ops-tests") : width(w), height(h) {
    comp_engine_desc desc = canvas_engine_desc(app_name);
    if (comp_engine_create(&desc, &engine) != COMP_OK) return;
    comp_document_desc doc_desc{};
    doc_desc.width = w;
    doc_desc.height = h;
    doc_desc.dpi = 72.0f;
    comp_document_create(engine, &doc_desc, &document);
  }

  ~Doc() {
    if (document != nullptr) comp_document_destroy(document);
    if (engine != nullptr) comp_engine_destroy(engine);
  }

  /* 句柄转域层指针（16 位 / HDR / 空选区用例需要直接读像素）。 */
  compositor::Document* domain() { return reinterpret_cast<compositor::Document*>(document); }

  Doc(const Doc&) = delete;
  Doc& operator=(const Doc&) = delete;
};

/* 铺一张任意字节的 RGBA8 图层（stride == w * 4）。bytes.size() 必须是 w*h*4。 */
comp_layer_id add_rgba8_layer(comp_document document, comp_rect transform,
                              const std::vector<uint8_t>& bytes) {
  comp_layer_desc desc{};
  desc.name = "pattern";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = transform;
  desc.sampling = COMP_SAMPLING_NEAREST;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  comp_image image{};
  image.width = transform.w;
  image.height = transform.h;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(transform.w) * 4u;
  image.pixels = const_cast<uint8_t*>(bytes.data());
  if (comp_layer_set_pixels(document, id, &image) != COMP_OK) return 0;
  return id;
}

/* 铺满画布的单色 RGBA8 图层。 */
comp_layer_id add_solid_layer(comp_document document, int32_t w, int32_t h, uint8_t r, uint8_t g,
                              uint8_t b, uint8_t a = 255) {
  std::vector<uint8_t> bytes(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  for (size_t i = 0; i + 3 < bytes.size(); i += 4u) {
    bytes[i] = r;
    bytes[i + 1] = g;
    bytes[i + 2] = b;
    bytes[i + 3] = a;
  }
  return add_rgba8_layer(document, comp_rect{0, 0, w, h}, bytes);
}

/* x/y 编码的探针图（任何位移都改变字节，与 test_crop 的 RampImage 同款）。 */
std::vector<uint8_t> ramp_bytes(int32_t w, int32_t h) {
  std::vector<uint8_t> bytes(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
      bytes[i] = static_cast<uint8_t>((x * 7 + 3) & 0xFF);
      bytes[i + 1] = static_cast<uint8_t>((y * 11 + 5) & 0xFF);
      bytes[i + 2] = static_cast<uint8_t>((x * 5 + y * 3) & 0xFF);
      bytes[i + 3] = 255;
    }
  }
  return bytes;
}

std::vector<uint8_t> render_rgba(comp_document document, int32_t w, int32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  if (comp_render_flatten(document, pixels.data(), COMP_PIXEL_RGBA8,
                          static_cast<size_t>(w) * 4u) != COMP_OK) {
    return {};
  }
  return pixels;
}

size_t undo_depth(comp_document document) {
  size_t depth = 0;
  if (comp_document_history_stats(document, &depth, nullptr) != COMP_OK) return 0;
  return depth;
}

bool size_is(comp_document document, int32_t w, int32_t h) {
  comp_size size{};
  if (comp_document_size(document, &size) != COMP_OK) return false;
  return size.width == w && size.height == h;
}

bool transform_is(comp_document document, comp_layer_id id, comp_rect expect, float rotation,
                  float epsilon = 0.01f) {
  comp_rect transform{};
  float actual_rotation = 0.0f;
  if (comp_layer_get_transform(document, id, &transform, &actual_rotation) != COMP_OK) return false;
  return transform.x == expect.x && transform.y == expect.y && transform.w == expect.w &&
         transform.h == expect.h && std::fabs(actual_rotation - rotation) <= epsilon;
}

/* 画布分辨率选区 coverage（无选区时全 0）。 */
std::vector<uint8_t> coverage_of(comp_document document, int32_t w, int32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
  comp_mask mask{};
  mask.width = w;
  mask.height = h;
  mask.stride = static_cast<size_t>(w);
  mask.pixels = pixels.data();
  if (comp_selection_get_coverage(document, &mask) != COMP_OK) return {};
  return pixels;
}

uint8_t at(const std::vector<uint8_t>& mask, int32_t w, int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= w) return 0;
  const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
  if (i >= mask.size()) return 0;
  return mask[i];
}

long mask_sum(const std::vector<uint8_t>& mask) {
  long total = 0;
  for (uint8_t value : mask) total += value;
  return total;
}

int32_t active_of(comp_document document) {
  int32_t active = -1;
  if (comp_selection_get_active(document, &active) != COMP_OK) return -1;
  return active;
}

comp_result rect_select(comp_document document, float x0, float y0, float x1, float y1,
                        comp_selection_combine combine, int32_t* out_changed) {
  const float points[4] = {x0, y0, x1, y1};
  return comp_selection_apply_shape(document, COMP_SELECTION_RECT, points, 2, combine, 1,
                                    out_changed);
}

bool bounds_are(comp_document document, comp_rect expect) {
  comp_rect bounds{};
  if (comp_selection_get_bounds(document, &bounds) != COMP_OK) return false;
  return bounds.x == expect.x && bounds.y == expect.y && bounds.w == expect.w && bounds.h == expect.h;
}

/* 期望图：把 src（w×h）整体旋转。y 向下的文档坐标（视觉方向）。 */
std::vector<uint8_t> rotated_cw(const std::vector<uint8_t>& src, int32_t w, int32_t h) {
  std::vector<uint8_t> dst(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  /* dst(x, y) = src(x_s = y, y_s = h - 1 - x)；dst 尺寸 h×w（宽 h 高 w）。 */
  for (int32_t y = 0; y < w; ++y) {
    for (int32_t x = 0; x < h; ++x) {
      const uint8_t* s =
          src.data() + (static_cast<size_t>(h - 1 - x) * static_cast<size_t>(w) +
                        static_cast<size_t>(y)) * 4u;
      uint8_t* d = dst.data() + (static_cast<size_t>(y) * static_cast<size_t>(h) +
                                 static_cast<size_t>(x)) * 4u;
      std::memcpy(d, s, 4u);
    }
  }
  return dst;
}

std::vector<uint8_t> rotated_ccw(const std::vector<uint8_t>& src, int32_t w, int32_t h) {
  std::vector<uint8_t> dst(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  /* dst(x, y) = src(x_s = w - 1 - y, y_s = x)；dst 尺寸 h×w。 */
  for (int32_t y = 0; y < w; ++y) {
    for (int32_t x = 0; x < h; ++x) {
      const uint8_t* s = src.data() + (static_cast<size_t>(x) * static_cast<size_t>(w) +
                                       static_cast<size_t>(w - 1 - y)) * 4u;
      uint8_t* d = dst.data() + (static_cast<size_t>(y) * static_cast<size_t>(h) +
                                 static_cast<size_t>(x)) * 4u;
      std::memcpy(d, s, 4u);
    }
  }
  return dst;
}

std::vector<uint8_t> rotated_180(const std::vector<uint8_t>& src, int32_t w, int32_t h) {
  std::vector<uint8_t> dst(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const uint8_t* s = src.data() + (static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) +
                                       static_cast<size_t>(w - 1 - x)) * 4u;
      uint8_t* d = dst.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) +
                                 static_cast<size_t>(x)) * 4u;
      std::memcpy(d, s, 4u);
    }
  }
  return dst;
}

/* ------------------------------------------------------- 域层混合 fixture */

comp_engine_desc domain_engine_desc(const char* app_name) {
  comp_engine_desc desc{};
  desc.app_name = app_name;
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

/* 域层文档（需要直接访问 Layer::pixels / SelectionState 的用例）。 */
struct DomainDoc {
  compositor::Engine engine;
  compositor::Document document;
  int32_t width;
  int32_t height;

  DomainDoc(int32_t w, int32_t h, const char* app_name = "compositor-canvas-ops-domain")
      : engine(domain_engine_desc(app_name)),
        document(&engine, w, h, 72.0f, "sRGB"),
        width(w),
        height(h) {}

  comp_document handle() { return reinterpret_cast<comp_document>(&document); }

  std::vector<uint8_t> pixels_of(comp_layer_id id) {
    const compositor::Layer* layer = document.find(id);
    if (layer == nullptr) return {};
    return layer->pixels.pixel_data();
  }
};

comp_layer_id domain_solid(compositor::Document* document, int32_t w, int32_t h,
                                        uint8_t r, uint8_t g, uint8_t b) {
  comp_layer_desc desc{};
  desc.name = "solid";
  desc.transform = comp_rect{0, 0, w, h};
  comp_layer_id id = 0;
  compositor::Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels = compositor::ImageBuffer::solid(w, h, COMP_PIXEL_RGBA8, r, g, b, 255);
  return id;
}

comp_layer_id domain_wide_layer(compositor::Document* document) {
  const int32_t w = document->state().width;
  const int32_t h = document->state().height;
  comp_layer_desc desc{};
  desc.name = "wide";
  desc.transform = comp_rect{0, 0, w, h};
  comp_layer_id id = 0;
  compositor::Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels.resize(w, h, COMP_PIXEL_RGBA16);
  return id;
}

comp_layer_id domain_hdr_layer(compositor::Document* document) {
  const int32_t w = document->state().width;
  const int32_t h = document->state().height;
  comp_layer_desc desc{};
  desc.name = "hdr";
  desc.transform = comp_rect{0, 0, w, h};
  comp_layer_id id = 0;
  compositor::Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels.resize(w, h, COMP_PIXEL_RGBA16F);
  return id;
}

}  // namespace

/* ============================================================ 画布旋转 */

COMP_TEST(rotate_canvas_cw_swaps_dims_and_moves_layer_without_resampling) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  const std::vector<uint8_t> bytes = ramp_bytes(16, 8);
  const comp_layer_id id = add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, bytes);
  COMP_CHECK(id != 0);

  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 8);
  const size_t depth_before = undo_depth(doc.document);
  int32_t changed = -1;

  COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(size_is(doc.document, 8, 16), "90° CW swaps canvas width/height");

  /* 图层像素不重采样：落位换成旋转后的轴对齐包围盒 (0,0,8,16)，rotation 累加 90°。 */
  COMP_CHECK_MSG(transform_is(doc.document, id, comp_rect{0, 0, 8, 16}, 90.0f),
                 "layer origin/bounds follow the quarter turn");

  /* 整篇合成 = 原合成图整体顺时针 90°（逐字节）。 */
  const std::vector<uint8_t> after = render_rgba(doc.document, 8, 16);
  COMP_CHECK_MSG(after == rotated_cw(before, 16, 8),
                 "composite is the source image rotated 90° clockwise");

  /* 图层像素本身一个字节都不动（不重采样原则的硬约束）。 */
  const std::vector<uint8_t> layer_pixels = doc.domain()->find(id)->pixels.pixel_data();
  COMP_CHECK_MSG(layer_pixels == bytes, "rotation never resamples layer pixels");

  COMP_CHECK(undo_depth(doc.document) == depth_before + 1);
  COMP_CHECK(comp_document_undo(doc.document) == COMP_OK);
  COMP_CHECK_MSG(size_is(doc.document, 16, 8), "undo restores the canvas size");
  COMP_CHECK_MSG(render_rgba(doc.document, 16, 8) == before, "undo restores the composite");

  COMP_CHECK(comp_document_redo(doc.document) == COMP_OK);
  COMP_CHECK(size_is(doc.document, 8, 16));
  COMP_CHECK(render_rgba(doc.document, 8, 16) == rotated_cw(before, 16, 8));
}

COMP_TEST(rotate_canvas_ccw_is_the_inverse_of_cw) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  const comp_layer_id id = add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ramp_bytes(16, 8));
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 8);

  int32_t changed = -1;
  COMP_CHECK(comp_document_rotate_canvas(doc.document, -1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(size_is(doc.document, 8, 16));
  COMP_CHECK_MSG(transform_is(doc.document, id, comp_rect{0, 0, 8, 16}, -90.0f),
                 "CCW gives the same swapped bounds and a negative rotation");
  COMP_CHECK_MSG(render_rgba(doc.document, 8, 16) == rotated_ccw(before, 16, 8),
                 "composite is the source rotated 90° counter-clockwise");
}

COMP_TEST(rotate_canvas_180_keeps_dims_and_flips_every_pixel) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  const comp_layer_id id = add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ramp_bytes(16, 8));
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 8);
  const size_t depth_before = undo_depth(doc.document);

  int32_t changed = -1;
  COMP_CHECK(comp_document_rotate_canvas(doc.document, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(size_is(doc.document, 16, 8), "180° keeps the canvas size");
  COMP_CHECK_MSG(transform_is(doc.document, id, comp_rect{0, 0, 16, 8}, 180.0f),
                 "a full-size layer stays put, only the rotation accumulates");
  COMP_CHECK_MSG(render_rgba(doc.document, 16, 8) == rotated_180(before, 16, 8),
                 "composite is the source rotated 180°");
  COMP_CHECK(undo_depth(doc.document) == depth_before + 1);

  /* -2 与 2 同为 180°，两次效果一致（图层数据不动，重复落位幂等）。 */
  COMP_CHECK(comp_document_rotate_canvas(doc.document, -2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(size_is(doc.document, 16, 8));
  COMP_CHECK_MSG(render_rgba(doc.document, 16, 8) == before, "two 180° turns cancel out");
}

COMP_TEST(rotate_canvas_four_quarter_turns_close_the_loop) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ramp_bytes(16, 8)) != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 8);

  int32_t changed = 0;
  for (int32_t step = 0; step < 4; ++step) {
    COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_OK);
    COMP_CHECK(changed == 1);
    COMP_CHECK(size_is(doc.document, step % 2 == 0 ? 8 : 16, step % 2 == 0 ? 16 : 8));
  }
  COMP_CHECK_MSG(render_rgba(doc.document, 16, 8) == before,
                 "four CW turns reproduce the original composite byte for byte");
}

COMP_TEST(rotate_canvas_odd_dims_rounds_the_origin_away_from_zero) {
  Doc doc(5, 3);
  COMP_CHECK(doc.document != nullptr);
  const comp_layer_id id = add_rgba8_layer(doc.document, comp_rect{0, 0, 5, 3}, ramp_bytes(5, 3));
  COMP_CHECK(id != 0);

  int32_t changed = -1;
  COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(size_is(doc.document, 3, 5));
  /* 中心 (2.5, 1.5) → middle (1.5, 2.5) → 落位包围盒 (0, 0, 3, 5)（3×5 竖矩形），
   * 奇数边长按 .5 远离零取整（不重采样原则下的已知半像素代价）。 */
  COMP_CHECK_MSG(transform_is(doc.document, id, comp_rect{0, 0, 3, 5}, 90.0f),
                 "odd side lengths round the new bounds away from zero");
}

COMP_TEST(rotate_canvas_rejects_unknown_turns_without_side_effects) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ramp_bytes(16, 8)) != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 8);
  const size_t depth_before = undo_depth(doc.document);

  const int32_t bad[] = {0, 3, -3, 5, -4};
  for (int32_t turns : bad) {
    int32_t changed = 7;
    COMP_CHECK(comp_document_rotate_canvas(doc.document, turns, &changed) ==
               COMP_ERR_INVALID_ARGUMENT);
    /* 失败路径不写 out_changed（契约只保证成功时写入），changed 仍是 7。 */
    COMP_CHECK(changed == 7);
  }
  COMP_CHECK(size_is(doc.document, 16, 8));
  COMP_CHECK(undo_depth(doc.document) == depth_before);
  COMP_CHECK(render_rgba(doc.document, 16, 8) == before);
}

COMP_TEST(rotate_canvas_empty_document_is_a_noop_only_when_dims_hold) {
  /* comp_document_create 自带一个默认背景层（PS 同款），真空文档要先删掉它。 */
  Doc square(8, 8, "compositor-rotate-square");
  COMP_CHECK(square.document != nullptr);
  while (!square.domain()->state().layers.empty()) {
    const comp_layer_id front = square.domain()->state().layers.front().id;
    COMP_CHECK(comp_layer_remove(square.document, front) == COMP_OK);
  }
  const size_t depth_square = undo_depth(square.document);
  int32_t changed = -1;
  COMP_CHECK(comp_document_rotate_canvas(square.document, 1, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "rotating an empty square canvas changes nothing");
  COMP_CHECK(undo_depth(square.document) == depth_square);

  /* 空文档但尺寸互换：状态变了（width/height 是 DocumentState 的一部分），进历史。 */
  Doc wide(16, 8, "compositor-rotate-wide");
  COMP_CHECK(wide.document != nullptr);
  while (!wide.domain()->state().layers.empty()) {
    const comp_layer_id front = wide.domain()->state().layers.front().id;
    COMP_CHECK(comp_layer_remove(wide.document, front) == COMP_OK);
  }
  const size_t depth_wide = undo_depth(wide.document);
  COMP_CHECK(comp_document_rotate_canvas(wide.document, 1, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "an empty canvas still swaps its dimensions");
  COMP_CHECK(size_is(wide.document, 8, 16));
  COMP_CHECK(undo_depth(wide.document) == depth_wide + 1);
}

COMP_TEST(rotate_canvas_carries_the_selection_by_exact_transposition) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_solid_layer(doc.document, 16, 8, 120, 120, 120) != 0);

  /* 选区 x∈[4,10), y∈[2,6)。CW 后 dst(x, y) = src(y, 7 - x)：新画布上非零区是
   * x∈[2,6), y∈[4,10)，即矩形 (2,4)-(6,10)。蒙版是文档分辨率的衍生数据，必须
   * 精确转置（无插值），而不是被丢弃。 */
  int32_t changed = 0;
  COMP_CHECK(rect_select(doc.document, 4.0f, 2.0f, 10.0f, 6.0f, COMP_SELECTION_REPLACE, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);

  COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(size_is(doc.document, 8, 16));

  const std::vector<uint8_t> mask = coverage_of(doc.document, 8, 16);
  COMP_CHECK(at(mask, 8, 2, 4) == 255);
  COMP_CHECK(at(mask, 8, 5, 9) == 255);
  COMP_CHECK(at(mask, 8, 2, 3) == 0);
  COMP_CHECK(at(mask, 8, 1, 4) == 0);
  COMP_CHECK(at(mask, 8, 6, 9) == 0);
  COMP_CHECK(at(mask, 8, 2, 10) == 0);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * 6L * 4L, "transposed selection keeps its exact area");
  COMP_CHECK_MSG(bounds_are(doc.document, comp_rect{2, 4, 4, 6}),
                 "selection bounds follow the quarter turn");

  /* 一步撤销：画布与选区一起回来。 */
  COMP_CHECK(comp_document_undo(doc.document) == COMP_OK);
  COMP_CHECK(size_is(doc.document, 16, 8));
  COMP_CHECK(active_of(doc.document) == 1);
  COMP_CHECK_MSG(bounds_are(doc.document, comp_rect{4, 2, 6, 4}),
                 "undo restores the selection to its original bounds");
}

COMP_TEST(rotate_canvas_drops_an_unalignable_empty_selection) {
  /* 两件不同的事，旧用例只看到第一件的一半：
   *   A. 显式空选区但蒙版网格**已分配**（全 0）——网格能转置，选区保持 active，
   *      coverage 仍全 0。这不是「幽灵选区」：显式空选区在滤镜/反相里本来就是
   *      no-op 语义，丢掉它反而会抹掉 feather / 撤销语义。
   *   B. active 但蒙版**网格为空**（零尺寸）——换坐标系后无法对齐，保守丢弃
   *      （与 crop 同口径），否则会留下一个尺寸对不上新画布的蒙版。 */
  {
    Doc doc(16, 8);
    COMP_CHECK(doc.document != nullptr);
    COMP_CHECK(add_solid_layer(doc.document, 16, 8, 120, 120, 120) != 0);

    int32_t changed = 0;
    COMP_CHECK(rect_select(doc.document, 4.0f, 4.0f, 12.0f, 12.0f, COMP_SELECTION_REPLACE,
                           &changed) == COMP_OK);
    COMP_CHECK(comp_selection_modify(doc.document, COMP_SELECTION_CONTRACT, 500, &changed) ==
               COMP_OK);
    COMP_CHECK(active_of(doc.document) == 1);
    COMP_CHECK(mask_sum(coverage_of(doc.document, 16, 8)) == 0);

    COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_OK);
    COMP_CHECK(changed == 1);
    /* 全 0 蒙版转置仍是全 0：没有像素被选中，不会凭空造出幽灵选区。 */
    COMP_CHECK_MSG(mask_sum(coverage_of(doc.document, 8, 16)) == 0,
                   "rotating an explicitly empty selection selects no pixel");
    COMP_CHECK_MSG(active_of(doc.document) == 1,
                   "an allocated empty mask survives the turn (it is still alignable)");
  }

  {
    Doc doc(16, 8);
    COMP_CHECK(doc.document != nullptr);
    COMP_CHECK(add_solid_layer(doc.document, 16, 8, 120, 120, 120) != 0);
    /* 域层直接摆一个「active 但蒙版缓冲是空的」状态（C ABI 造不出来）。 */
    compositor::Document* domain = doc.domain();
    COMP_CHECK(domain != nullptr);
    domain->state().selection.active = true;
    COMP_CHECK(domain->state().selection.base.empty());

    int32_t changed = 0;
    COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_OK);
    COMP_CHECK(changed == 1);
    COMP_CHECK_MSG(active_of(doc.document) == 0,
                   "an unalignable empty grid is dropped, not carried into the new canvas");
  }
}

COMP_TEST(rotate_canvas_rejects_origin_overflow_without_partial_state) {
  Doc doc(16, 8);
  COMP_CHECK(doc.document != nullptr);
  /* 中心 (1000008, 4)：CW 后新 origin 越出 ±1e6 上限（与 crop 的两遍校验同口径）。 */
  COMP_CHECK(add_solid_layer(doc.document, 16, 8, 10, 10, 10) != 0);
  COMP_CHECK(comp_document_set_size(doc.document, comp_size{16, 8}) == COMP_OK);

  comp_layer_desc far_desc{};
  far_desc.name = "far";
  /* 落位口径下 CW 的 origin 映射是 (old_h - y - h, x)：
   * (1000000, -1000000, 4, 4) 旋出后 x 分量 1000004 越出 ±1e6（与 crop 的
   * 两遍校验同口径）。 */
  far_desc.transform = comp_rect{1000000, -1000000, 4, 4};
  comp_layer_id far_id = 0;
  const comp_result added = comp_layer_add(doc.document, &far_desc, &far_id);
  if (added == COMP_OK) {
    const size_t depth_before = undo_depth(doc.document);
    int32_t changed = 7;
    COMP_CHECK(comp_document_rotate_canvas(doc.document, 1, &changed) == COMP_ERR_INVALID_ARGUMENT);
    COMP_CHECK(changed == 7);
    COMP_CHECK(size_is(doc.document, 16, 8));
    COMP_CHECK(undo_depth(doc.document) == depth_before);
  }
}

/* ================================================================ 修边 */

COMP_TEST(trim_transparent_pixels_crops_to_the_content_box) {
  Doc doc(16, 16);
  COMP_CHECK(doc.document != nullptr);
  /* 内容只有 (4,6)-(12,14) 一块不透明红，四周是透明画布。 */
  std::vector<uint8_t> bytes(static_cast<size_t>(16) * 16 * 4u, 0u);
  for (int32_t y = 6; y < 14; ++y) {
    for (int32_t x = 4; x < 12; ++x) {
      uint8_t* p = bytes.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      p[0] = 200;
      p[1] = 40;
      p[2] = 40;
      p[3] = 255;
    }
  }
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 16}, bytes) != 0);

  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 16);
  const size_t depth_before = undo_depth(doc.document);

  comp_trim_options options{};
  options.based_on = COMP_TRIM_TRANSPARENT_PIXELS;
  options.top = options.bottom = options.left = options.right = 1;
  options.tolerance = 0;
  int32_t changed = -1;
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(size_is(doc.document, 8, 8), "trim crops to the opaque content box");
  COMP_CHECK(undo_depth(doc.document) == depth_before + 1);

  /* 修边不重采样：新画布逐字节等于原图 (4,6) 处的 8x8 窗口。 */
  const std::vector<uint8_t> trimmed = render_rgba(doc.document, 8, 8);
  for (int32_t y = 0; y < 8; ++y) {
    const uint8_t* source = before.data() + ((static_cast<size_t>(y) + 6) * 16 + 4) * 4u;
    COMP_CHECK(std::memcmp(source, trimmed.data() + static_cast<size_t>(y) * 8 * 4u, 8 * 4u) == 0);
  }

  COMP_CHECK(comp_document_undo(doc.document) == COMP_OK);
  COMP_CHECK(size_is(doc.document, 16, 16));
  COMP_CHECK(render_rgba(doc.document, 16, 16) == before);
}

COMP_TEST(trim_respects_per_edge_switches) {
  Doc doc(16, 16);
  COMP_CHECK(doc.document != nullptr);
  std::vector<uint8_t> bytes(static_cast<size_t>(16) * 16 * 4u, 0u);
  for (int32_t y = 4; y < 12; ++y) {
    for (int32_t x = 4; x < 12; ++x) {
      uint8_t* p = bytes.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      p[0] = p[1] = p[2] = 90;
      p[3] = 255;
    }
  }
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 16}, bytes) != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 16);

  comp_trim_options options{};
  options.based_on = COMP_TRIM_TRANSPARENT_PIXELS;
  options.tolerance = 0;

  /* 只裁顶：x 保持 0..16，y 从 4 起。 */
  options.top = 1;
  options.bottom = options.left = options.right = 0;
  int32_t changed = -1;
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(size_is(doc.document, 16, 12), "only the top edge is trimmed");
  for (int32_t y = 0; y < 12; ++y) {
    COMP_CHECK(std::memcmp(before.data() + (static_cast<size_t>(y) + 4) * 16 * 4u,
                           render_rgba(doc.document, 16, 12).data() + static_cast<size_t>(y) * 16 * 4u,
                           16 * 4u) == 0);
  }

  /* 只裁左：y 保持 0..16，x 从 4 起。 */
  COMP_CHECK(comp_document_undo(doc.document) == COMP_OK);
  options.top = 0;
  options.left = 1;
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(size_is(doc.document, 12, 16), "only the left edge is trimmed");
  for (int32_t y = 0; y < 16; ++y) {
    COMP_CHECK(std::memcmp(before.data() + (static_cast<size_t>(y) * 16 + 4) * 4u,
                           render_rgba(doc.document, 12, 16).data() + static_cast<size_t>(y) * 12 * 4u,
                           12 * 4u) == 0);
  }
}

COMP_TEST(trim_rejects_empty_uniform_and_untrimmable_documents) {
  Doc doc(16, 16);
  COMP_CHECK(doc.document != nullptr);

  comp_trim_options options{};
  options.based_on = COMP_TRIM_TRANSPARENT_PIXELS;
  options.top = options.bottom = options.left = options.right = 1;
  options.tolerance = 0;

  /* 默认背景层是全透明画布：整篇没有不透明内容 -> STATE，不进历史。 */
  const size_t depth_before = undo_depth(doc.document);
  int32_t changed = 7;
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_ERR_STATE);
  COMP_CHECK(changed == 7);
  COMP_CHECK(undo_depth(doc.document) == depth_before);

  /* 整篇透明：STATE。 */
  std::vector<uint8_t> transparent(static_cast<size_t>(16) * 16 * 4u, 0u);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 16}, transparent) != 0);
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_ERR_STATE);
  COMP_CHECK(size_is(doc.document, 16, 16));

  /* 整篇同基准色（颜色模式）：STATE。 */
  Doc uniform(16, 16, "compositor-trim-uniform");
  COMP_CHECK(uniform.document != nullptr);
  COMP_CHECK(add_solid_layer(uniform.document, 16, 16, 77, 88, 99) != 0);
  comp_trim_options color_options{};
  color_options.based_on = COMP_TRIM_TOP_LEFT_PIXEL_COLOR;
  color_options.top = color_options.bottom = color_options.left = color_options.right = 1;
  COMP_CHECK(comp_document_trim(uniform.document, color_options, &changed) == COMP_ERR_STATE);
  COMP_CHECK(size_is(uniform.document, 16, 16));

  /* 四边全关：INVALID_ARGUMENT（上游 trimsAny guard）。 */
  comp_trim_options none = options;
  none.top = none.bottom = none.left = none.right = 0;
  COMP_CHECK(comp_document_trim(doc.document, none, &changed) == COMP_ERR_INVALID_ARGUMENT);
  /* 未知枚举：INVALID_ARGUMENT。 */
  comp_trim_options bad_enum = options;
  bad_enum.based_on = static_cast<comp_trim_based_on>(99);
  COMP_CHECK(comp_document_trim(doc.document, bad_enum, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(size_is(doc.document, 16, 16));
}

COMP_TEST(trim_color_based_uses_corner_samples_with_per_channel_tolerance) {
  const int32_t k = 16;
  /* 灰边框 + 红心：TopLeft 基准 = (0,0) 灰。保留 (1,1)-(15,15)。 */
  std::vector<uint8_t> gray_border(static_cast<size_t>(k) * k * 4u, 0u);
  for (int32_t y = 0; y < k; ++y) {
    for (int32_t x = 0; x < k; ++x) {
      uint8_t* p = gray_border.data() + (static_cast<size_t>(y) * k + static_cast<size_t>(x)) * 4u;
      const bool border = x == 0 || y == 0 || x == k - 1 || y == k - 1;
      p[0] = border ? 128 : 200;
      p[1] = border ? 128 : 50;
      p[2] = border ? 128 : 50;
      p[3] = 255;
    }
  }

  auto run_trim = [](comp_document document, comp_trim_options options, int32_t* changed) {
    return comp_document_trim(document, options, changed);
  };

  comp_trim_options top_left{};
  top_left.based_on = COMP_TRIM_TOP_LEFT_PIXEL_COLOR;
  top_left.top = top_left.bottom = top_left.left = top_left.right = 1;
  top_left.tolerance = 0;

  {
    Doc doc(k, k, "compositor-trim-topleft");
    COMP_CHECK(doc.document != nullptr);
    COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, k, k}, gray_border) != 0);
    int32_t changed = -1;
    COMP_CHECK(run_trim(doc.document, top_left, &changed) == COMP_OK);
    COMP_CHECK(changed == 1);
    COMP_CHECK_MSG(size_is(doc.document, 14, 14), "top-left color trim keeps the inner box");
  }

  /* tolerance 大到把红也判为基准色（逐通道 abs 差 <= tol）：整图匹配 -> 无内容可留。 */
  {
    Doc doc(k, k, "compositor-trim-wide-tol");
    COMP_CHECK(doc.document != nullptr);
    COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, k, k}, gray_border) != 0);
    comp_trim_options wide = top_left;
    wide.tolerance = 80; /* abs(50-128)=78 <= 80，红心也被吃掉 */
    int32_t changed = 7;
    COMP_CHECK(run_trim(doc.document, wide, &changed) == COMP_ERR_STATE);
    COMP_CHECK(size_is(doc.document, k, k));
  }
  {
    Doc doc(k, k, "compositor-trim-narrow-tol");
    COMP_CHECK(doc.document != nullptr);
    COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, k, k}, gray_border) != 0);
    comp_trim_options narrow = top_left;
    narrow.tolerance = 70; /* 78 > 70，红心仍算差异色 */
    int32_t changed = -1;
    COMP_CHECK(run_trim(doc.document, narrow, &changed) == COMP_OK);
    COMP_CHECK(changed == 1);
    COMP_CHECK(size_is(doc.document, 14, 14));
  }

  /* BottomRight 基准：红边框 + 灰心，(15,15) 是红 -> 同样保留灰心。 */
  std::vector<uint8_t> red_border(static_cast<size_t>(k) * k * 4u, 0u);
  for (int32_t y = 0; y < k; ++y) {
    for (int32_t x = 0; x < k; ++x) {
      uint8_t* p = red_border.data() + (static_cast<size_t>(y) * k + static_cast<size_t>(x)) * 4u;
      const bool border = x == 0 || y == 0 || x == k - 1 || y == k - 1;
      p[0] = border ? 200 : 128;
      p[1] = border ? 50 : 128;
      p[2] = border ? 50 : 128;
      p[3] = 255;
    }
  }
  {
    Doc doc(k, k, "compositor-trim-bottomright");
    COMP_CHECK(doc.document != nullptr);
    COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, k, k}, red_border) != 0);
    comp_trim_options bottom_right{};
    bottom_right.based_on = COMP_TRIM_BOTTOM_RIGHT_PIXEL_COLOR;
    bottom_right.top = bottom_right.bottom = bottom_right.left = bottom_right.right = 1;
    bottom_right.tolerance = 0;
    int32_t changed = -1;
    COMP_CHECK(run_trim(doc.document, bottom_right, &changed) == COMP_OK);
    COMP_CHECK(changed == 1);
    COMP_CHECK_MSG(size_is(doc.document, 14, 14), "bottom-right color trim keeps the inner box");
  }
}

COMP_TEST(trim_is_a_noop_when_content_fills_the_canvas) {
  Doc doc(16, 16);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_solid_layer(doc.document, 16, 16, 30, 60, 90) != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 16);
  const size_t depth_before = undo_depth(doc.document);

  comp_trim_options options{};
  options.based_on = COMP_TRIM_TRANSPARENT_PIXELS;
  options.top = options.bottom = options.left = options.right = 1;
  int32_t changed = -1;
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "a canvas without margins adds no history on trim");
  COMP_CHECK(undo_depth(doc.document) == depth_before);
  COMP_CHECK(render_rgba(doc.document, 16, 16) == before);
}

COMP_TEST(trim_transactions_group_into_one_undo_step) {
  Doc doc(16, 16);
  COMP_CHECK(doc.document != nullptr);
  std::vector<uint8_t> bytes(static_cast<size_t>(16) * 16 * 4u, 0u);
  for (int32_t y = 2; y < 10; ++y) {
    for (int32_t x = 2; x < 10; ++x) {
      uint8_t* p = bytes.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      p[0] = p[1] = p[2] = 60;
      p[3] = 255;
    }
  }
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 16}, bytes) != 0);
  const std::vector<uint8_t> before = render_rgba(doc.document, 16, 16);
  const size_t depth_before = undo_depth(doc.document);

  comp_trim_options options{};
  options.based_on = COMP_TRIM_TRANSPARENT_PIXELS;
  options.top = options.bottom = options.left = options.right = 1;

  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(doc.document, "Trim batch", &txn) == COMP_OK);
  int32_t changed = 0;
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(comp_document_trim(doc.document, options, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "nothing left to trim once the content fills the canvas");
  COMP_CHECK_MSG(undo_depth(doc.document) == depth_before,
                 "commits inside an explicit transaction are suppressed");
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);
  COMP_CHECK(undo_depth(doc.document) == depth_before + 1);

  /* 一次 trim 到位：内容 (2,2)-(10,10) 被修成 8x8 画布，第二次无变化。 */
  COMP_CHECK(size_is(doc.document, 8, 8));
  COMP_CHECK(comp_document_undo(doc.document) == COMP_OK);
  COMP_CHECK(size_is(doc.document, 16, 16));
  COMP_CHECK(render_rgba(doc.document, 16, 16) == before);
}

/* ================================================================ 反相 */

COMP_TEST(invert_subtracts_each_channel_from_alpha_and_keeps_transparency) {
  DomainDoc doc(16, 8);
  COMP_CHECK(doc.handle() != nullptr);
  /* 三个探针像素：不透明、半透明、全透明。 */
  std::vector<uint8_t> bytes(static_cast<size_t>(16) * 8 * 4u, 0u);
  auto set_pixel = [&bytes](int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* p = bytes.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
  };
  set_pixel(0, 0, 100, 150, 200, 255);
  set_pixel(1, 0, 5, 5, 5, 10);
  set_pixel(2, 0, 0, 0, 0, 0);
  set_pixel(3, 0, 40, 40, 40, 40);
  const comp_layer_id id = add_rgba8_layer(doc.handle(), comp_rect{0, 0, 16, 8}, bytes);
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = doc.pixels_of(id);
  const size_t depth_before = undo_depth(doc.handle());

  int32_t changed = -1;
  COMP_CHECK(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth(doc.handle()) == depth_before + 1);

  const std::vector<uint8_t> after = doc.pixels_of(id);
  COMP_CHECK(after.size() == before.size());
  /* 预乘语义 c' = alpha - c（上游 PixelInvert 的矩阵），alpha 一律不动。 */
  auto expect = [](uint8_t c, uint8_t a) { return static_cast<uint8_t>(a) - c; };
  for (size_t i = 0; i + 3 < after.size(); i += 4u) {
    COMP_CHECK(after[i] == expect(before[i], before[i + 3]));
    COMP_CHECK(after[i + 1] == expect(before[i + 1], before[i + 3]));
    COMP_CHECK(after[i + 2] == expect(before[i + 2], before[i + 3]));
    COMP_CHECK(after[i + 3] == before[i + 3]);
  }
  /* 三个探针的显式值：透明像素反相后仍透明。 */
  const size_t p0 = 0, p1 = 4, p2 = 8, p3 = 12;
  COMP_CHECK(after[p0] == 155 && after[p0 + 1] == 105 && after[p0 + 2] == 55 &&
             after[p0 + 3] == 255);
  COMP_CHECK(after[p1] == 5 && after[p1 + 1] == 5 && after[p1 + 2] == 5 && after[p1 + 3] == 10);
  COMP_CHECK(after[p2] == 0 && after[p2 + 1] == 0 && after[p2 + 2] == 0 && after[p2 + 3] == 0);
  COMP_CHECK(after[p3] == 0 && after[p3 + 1] == 0 && after[p3 + 2] == 0 && after[p3 + 3] == 40);

  COMP_CHECK(comp_document_undo(doc.handle()) == COMP_OK);
  COMP_CHECK_MSG(doc.pixels_of(id) == before, "undo restores the layer byte for byte");
}

COMP_TEST(invert_rejects_groups_unknown_and_hdr_layers) {
  DomainDoc doc(16, 8);
  COMP_CHECK(doc.handle() != nullptr);
  const comp_layer_id solid = domain_solid(&doc.document, 16, 8, 90, 90, 90);
  COMP_CHECK(solid != 0);

  /* 组图层。 */
  comp_layer_desc group_desc{};
  group_desc.name = "group";
  group_desc.is_group = 1;
  comp_layer_id group_id = 0;
  COMP_CHECK(doc.document.create_layer(group_desc, &group_id) != 0);

  /* HDR 图层没有反相通路。 */
  const comp_layer_id hdr = domain_hdr_layer(&doc.document);
  COMP_CHECK(hdr != 0);

  /* 建层本身各记一步历史；基准取在所有创建之后。 */
  const size_t depth_before = undo_depth(doc.handle());

  int32_t changed = 7;
  COMP_CHECK(comp_layer_invert(doc.handle(), group_id, &changed) == COMP_ERR_STATE);
  COMP_CHECK(comp_layer_invert(doc.handle(), 999999ull, &changed) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_layer_invert(doc.handle(), hdr, &changed) == COMP_ERR_UNSUPPORTED);
  COMP_CHECK_MSG(undo_depth(doc.handle()) == depth_before,
                 "all refusals record no history");
}

COMP_TEST(invert_flattens_a_16bit_layer_to_8bit_and_undo_restores) {
  DomainDoc doc(16, 8);
  COMP_CHECK(doc.handle() != nullptr);
  const comp_layer_id id = domain_wide_layer(&doc.document);
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = doc.pixels_of(id);
  const size_t depth_before = undo_depth(doc.handle());

  int32_t changed = -1;
  COMP_CHECK(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(doc.document.find(id)->pixels.format == COMP_PIXEL_RGBA8,
                 "an 8-bit tool leaves the 16-bit layer as an 8-bit working copy");
  COMP_CHECK(doc.pixels_of(id).size() == static_cast<size_t>(16) * 8 * 4u);

  COMP_CHECK(undo_depth(doc.handle()) == depth_before + 1);
  COMP_CHECK(comp_document_undo(doc.handle()) == COMP_OK);
  COMP_CHECK_MSG(doc.document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                 "undo restores the 16-bit asset");
  COMP_CHECK_MSG(doc.pixels_of(id) == before, "undo restores the original 16-bit bytes");
}

COMP_TEST(invert_clips_to_the_selection_in_document_coordinates) {
  DomainDoc doc(16, 8);
  COMP_CHECK(doc.handle() != nullptr);
  const comp_layer_id id = domain_solid(&doc.document, 16, 8, 100, 150, 200);
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = doc.pixels_of(id);

  /* 选区只盖左半 (0,0)-(8,8)：选区内反相，选区外逐字节保持。 */
  int32_t changed = 0;
  COMP_CHECK(rect_select(doc.handle(), 0.0f, 0.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);

  changed = -1;
  COMP_CHECK(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> after = doc.pixels_of(id);
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      const size_t i = (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      if (x < 8) {
        COMP_CHECK(after[i] == 155 && after[i + 1] == 105 && after[i + 2] == 55);
      } else {
        COMP_CHECK(after[i] == 100 && after[i + 1] == 150 && after[i + 2] == 200);
      }
      COMP_CHECK(after[i + 3] == 255);
    }
  }

  COMP_CHECK(comp_document_undo(doc.handle()) == COMP_OK);
  COMP_CHECK_MSG(doc.pixels_of(id) == before, "undo restores the un-inverted layer");
}

COMP_TEST(invert_with_an_offset_layer_uses_the_effective_bounds_origin) {
  DomainDoc doc(16, 8);
  COMP_CHECK(doc.handle() != nullptr);
  /* 图层只有 (4,6)-(12,14) 一块，选区 (0,0)-(8,8) 按文档坐标解释：
   * 图层像素里只有本地 x<4 且 y<2 的部分落在选区内。 */
  comp_layer_desc desc{};
  desc.name = "offset";
  desc.transform = comp_rect{4, 6, 8, 8};
  comp_layer_id id = 0;
  compositor::Layer* layer = doc.document.create_layer(desc, &id);
  COMP_CHECK(layer != nullptr);
  layer->pixels = compositor::ImageBuffer::solid(8, 8, COMP_PIXEL_RGBA8, 100, 150, 200,
                                                 255);
  const std::vector<uint8_t> before = doc.pixels_of(id);

  int32_t changed = 0;
  COMP_CHECK(rect_select(doc.handle(), 0.0f, 0.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE, &changed) ==
             COMP_OK);
  changed = -1;
  COMP_CHECK(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> after = doc.pixels_of(id);
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      const size_t i = (static_cast<size_t>(y) * 8 + static_cast<size_t>(x)) * 4u;
      const bool inside = x < 4 && y < 2;
      COMP_CHECK(after[i] == (inside ? 155 : 100));
      COMP_CHECK(after[i + 1] == (inside ? 105 : 150));
      COMP_CHECK(after[i + 2] == (inside ? 55 : 200));
    }
  }
  COMP_CHECK(comp_document_undo(doc.handle()) == COMP_OK);
  COMP_CHECK(doc.pixels_of(id) == before);
}

COMP_TEST(invert_maps_the_selection_through_layer_rotation_and_flip) {
  /* 8x8 画布、8x8 铺满图层，选区是文档左上四分块 (0,0)-(4,4)。
   * 旋转/翻转后要判的是「图层像素在文档里落在哪」，不是「图层像素自己的左边」：
   *   90°  → 图层 (x,y) 落在文档 (7-y, x)：选区内 ⇔ x<4 且 y>=4（左下块）
   *   180° → 落在文档 (7-x, 7-y)：选区内 ⇔ x>=4 且 y>=4（右下块）
   *   水平翻转 → 落在文档 (7-x, y)：选区内 ⇔ x>=4 且 y<4（右上块）
   * 旧实现只按 effective_bounds() 原点平移，三种情况都会把「左边」当成选区，
   * 选区外的像素被误改（S25 复测在 45°/90° 各测到 8 像素）。 */
  struct Case {
    float rotation;
    bool flip_x;
    const char* label;
    bool (*inside)(int32_t x, int32_t y);
  };
  const Case cases[] = {
      {90.0f, false, "90 degrees", [](int32_t x, int32_t y) { return x < 4 && y >= 4; }},
      {180.0f, false, "180 degrees", [](int32_t x, int32_t y) { return x >= 4 && y >= 4; }},
      {0.0f, true, "flip_x", [](int32_t x, int32_t y) { return x >= 4 && y < 4; }},
  };

  for (const Case& test : cases) {
    DomainDoc doc(8, 8);
    COMP_CHECK(doc.handle() != nullptr);
    const comp_layer_id id = domain_solid(&doc.document, 8, 8, 100, 150, 200);
    COMP_CHECK(id != 0);
    compositor::Layer* layer = doc.document.find(id);
    COMP_CHECK(layer != nullptr);
    layer->rotation = test.rotation;
    layer->flip_x = test.flip_x;
    const std::vector<uint8_t> before = doc.pixels_of(id);

    int32_t changed = 0;
    COMP_CHECK(rect_select(doc.handle(), 0.0f, 0.0f, 4.0f, 4.0f, COMP_SELECTION_REPLACE,
                           &changed) == COMP_OK);
    changed = -1;
    COMP_CHECK_MSG(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK, test.label);
    COMP_CHECK(changed == 1);

    const std::vector<uint8_t> after = doc.pixels_of(id);
    for (int32_t y = 0; y < 8; ++y) {
      for (int32_t x = 0; x < 8; ++x) {
        const size_t i = (static_cast<size_t>(y) * 8 + static_cast<size_t>(x)) * 4u;
        const bool inside = test.inside(x, y);
        /* 反相是 c' = 255 - c（alpha 不透明）：155/105/55 与 100/150/200 互判。 */
        COMP_CHECK_MSG(after[i] == (inside ? 155 : 100), test.label);
        COMP_CHECK_MSG(after[i + 1] == (inside ? 105 : 150), test.label);
        COMP_CHECK_MSG(after[i + 2] == (inside ? 55 : 200), test.label);
        COMP_CHECK_MSG(after[i + 3] == 255, test.label);
      }
    }

    COMP_CHECK(comp_document_undo(doc.handle()) == COMP_OK);
    COMP_CHECK_MSG(doc.pixels_of(id) == before, test.label);
  }
}

COMP_TEST(invert_maps_the_selection_through_an_arbitrary_rotation) {
  /* 16x16 图层，rotation = 45°，选区是文档右半 (x >= 8)。
   * 方形包围盒下图层像素 (x,y) 的文档落点是（cos45 = sin45 = 1/√2）
   *   doc_x = 7.5 + 0.7071 * (x - y)
   *   doc_y = 7.5 + 0.7071 * (x + y - 15)
   * 选区内 ⇔ lround(doc_x) >= 8，且落点仍要落在画布内（45° 旋转会把四角甩出
   * 画布，画布外的 coverage 定义为 0）。x == y 时 doc_x 恰好是 7.5，取整方向
   * 属于实现的 tie-break，跳过这条对角线。 */
  constexpr double kDiag = 0.7071067811865476;
  DomainDoc doc(16, 16);
  COMP_CHECK(doc.handle() != nullptr);
  const comp_layer_id id = domain_solid(&doc.document, 16, 16, 100, 150, 200);
  COMP_CHECK(id != 0);
  compositor::Layer* layer = doc.document.find(id);
  COMP_CHECK(layer != nullptr);
  layer->rotation = 45.0f;
  const std::vector<uint8_t> before = doc.pixels_of(id);

  int32_t changed = 0;
  COMP_CHECK(rect_select(doc.handle(), 8.0f, 0.0f, 16.0f, 16.0f, COMP_SELECTION_REPLACE,
                         &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> after = doc.pixels_of(id);
  int32_t inverted = 0;
  for (int32_t y = 0; y < 16; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      if (x == y) continue;
      const int32_t ix = static_cast<int32_t>(std::lround(7.5 + kDiag * (x - y)));
      const int32_t iy = static_cast<int32_t>(std::lround(7.5 + kDiag * (x + y - 15)));
      const bool on_canvas = ix >= 0 && ix < 16 && iy >= 0 && iy < 16;
      const bool inside = on_canvas && ix >= 8;
      if (inside) ++inverted;
      const size_t i = (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      COMP_CHECK_MSG(after[i] == (inside ? 155 : 100), "45 degrees maps the selection");
      COMP_CHECK(after[i + 1] == (inside ? 105 : 150));
      COMP_CHECK(after[i + 2] == (inside ? 55 : 200));
    }
  }
  /* 断言不是空转：45° 下确实有一片像素落在「文档右半但图层自己的左边」。 */
  COMP_CHECK_MSG(inverted > 0 && inverted < 16 * 16 - 16, "the 45-degree case has both halves");

  COMP_CHECK(comp_document_undo(doc.handle()) == COMP_OK);
  COMP_CHECK_MSG(doc.pixels_of(id) == before, "undo restores the rotated layer");
}

COMP_TEST(invert_with_an_explicitly_empty_selection_is_a_noop) {
  DomainDoc doc(16, 8);
  COMP_CHECK(doc.handle() != nullptr);
  const comp_layer_id id = domain_solid(&doc.document, 16, 8, 90, 90, 90);
  COMP_CHECK(id != 0);
  const std::vector<uint8_t> before = doc.pixels_of(id);
  const size_t depth_before = undo_depth(doc.handle());

  /* 显式空选区（active 但 base 全 0，与「没有选区」可区分）。 */
  doc.document.state().selection.active = true;
  doc.document.state().selection.feather = 0.0f;
  doc.document.state().selection.base.resize(16, 8, 0);

  int32_t changed = -1;
  COMP_CHECK(comp_layer_invert(doc.handle(), id, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "an explicitly empty selection inverts nothing");
  COMP_CHECK(undo_depth(doc.handle()) == depth_before);
  COMP_CHECK_MSG(doc.pixels_of(id) == before, "the layer stays byte-identical");
}

/* ============================================================ 色彩范围 */

/* 16x8 探针图：左半红、右半蓝，再在右半撒两个红点（验证全局匹配、不要求连续）。 */
struct ColorRangeImage {
  static constexpr int32_t kWidth = 16;
  static constexpr int32_t kHeight = 8;
  std::vector<uint8_t> bytes;

  static std::vector<uint8_t> build() {
    std::vector<uint8_t> out(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    for (int32_t y = 0; y < kHeight; ++y) {
      for (int32_t x = 0; x < kWidth; ++x) {
        uint8_t* p = out.data() + (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4u;
        const bool red = x < 8 || (x == 9 && y == 1) || (x == 14 && y == 6);
        p[0] = red ? 220 : 30;
        p[1] = red ? 30 : 30;
        p[2] = red ? 30 : 220;
        p[3] = 255;
      }
    }
    return out;
  }

  /* 红像素总数：左半 8x8 + 两个散点。 */
  static constexpr long kRedPixels = 8L * 8L + 2L;
  /* 蓝像素总数：右半 64 - 2 个红点。 */
  static constexpr long kBluePixels = 8L * 8L - 2L;
};

COMP_TEST(color_range_selects_every_matching_pixel_globally) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);
  const size_t depth_before = undo_depth(doc.document);

  const uint8_t include[3] = {220, 30, 30};
  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth(doc.document) == depth_before + 1);

  const std::vector<uint8_t> mask = coverage_of(doc.document, 16, 8);
  /* 左半整片 + 右半两个散点都要命中：色彩范围不做 flood fill。 */
  COMP_CHECK(at(mask, 16, 0, 0) == 255);
  COMP_CHECK(at(mask, 16, 7, 7) == 255);
  COMP_CHECK(at(mask, 16, 9, 1) == 255);
  COMP_CHECK(at(mask, 16, 14, 6) == 255);
  COMP_CHECK(at(mask, 16, 8, 0) == 0);
  COMP_CHECK(at(mask, 16, 10, 1) == 0);
  COMP_CHECK(at(mask, 16, 13, 6) == 0);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * ColorRangeImage::kRedPixels,
                 "every red pixel is selected, contiguous or not");
  /* 最右非零列是 x=14（右半只有两个散点），最底非零行是 y=7（左半整列）：半开包围盒
   * (0,0,15,8)，不是整画布。 */
  COMP_CHECK_MSG(bounds_are(doc.document, comp_rect{0, 0, 15, 8}),
                 "bounds are the tight box of the selected pixels");
}

COMP_TEST(color_range_invert_selects_the_complement) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);

  const uint8_t include[3] = {220, 30, 30};
  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 0, 1,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> mask = coverage_of(doc.document, 16, 8);
  COMP_CHECK(at(mask, 16, 8, 0) == 255);
  COMP_CHECK(at(mask, 16, 15, 7) == 255);
  COMP_CHECK(at(mask, 16, 9, 1) == 0);
  COMP_CHECK(at(mask, 16, 14, 6) == 0);
  COMP_CHECK(at(mask, 16, 0, 0) == 0);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * ColorRangeImage::kBluePixels,
                 "inverted range selects everything but the sampled color");
}

COMP_TEST(color_range_exclude_overrides_the_include_table) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);

  /* include 同时给红蓝、exclude 把蓝去掉：结果应与只 include 红一致。 */
  const uint8_t include[6] = {220, 30, 30, 30, 30, 220};
  const uint8_t exclude[3] = {30, 30, 220};
  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 2, exclude, 1, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> mask = coverage_of(doc.document, 16, 8);
  COMP_CHECK(at(mask, 16, 0, 0) == 255);
  COMP_CHECK(at(mask, 16, 8, 0) == 0);
  COMP_CHECK(at(mask, 16, 9, 1) == 255);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * ColorRangeImage::kRedPixels,
                 " Exclude wins where a pixel matches both tables");

  /* include 与 exclude 同色：没有像素能入选 -> 不改选区（changed = 0）。 */
  const uint8_t same[3] = {220, 30, 30};
  int32_t empty_changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, same, 1, same, 1, 0, 0,
                                              COMP_SELECTION_REPLACE, &empty_changed) == COMP_OK);
  COMP_CHECK_MSG(empty_changed == 0, "a fully excluded range selects nothing");
  COMP_CHECK(active_of(doc.document) == 1);
  COMP_CHECK(mask_sum(coverage_of(doc.document, 16, 8)) == 255L * ColorRangeImage::kRedPixels);
}

COMP_TEST(color_range_fuzziness_widens_the_match) {
  Doc doc(16, 8, "compositor-color-range-fuzziness");
  COMP_CHECK(doc.document != nullptr);
  /* 左半 (220,30,30)，右半 (235,45,45)：逐通道差 15/15/15。 */
  std::vector<uint8_t> bytes(static_cast<size_t>(16) * 8 * 4u, 0u);
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      uint8_t* p = bytes.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      const bool left = x < 8;
      p[0] = left ? 220 : 235;
      p[1] = left ? 30 : 45;
      p[2] = 30;
      p[3] = 255;
    }
  }
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, bytes) != 0);
  const uint8_t include[3] = {220, 30, 30};

  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(mask_sum(coverage_of(doc.document, 16, 8)) == 255L * 64L,
                 "fuzziness 0 matches only the exact color");

  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 10, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(mask_sum(coverage_of(doc.document, 16, 8)) == 255L * 64L,
                 "fuzziness 10 still misses the 15-step neighbor");

  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 20, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(mask_sum(coverage_of(doc.document, 16, 8)) == 255L * 128L,
                 "fuzziness 20 swallows the whole canvas");
}

COMP_TEST(color_range_zero_match_leaves_selection_and_history_untouched) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);
  const size_t depth_before = undo_depth(doc.document);

  /* 画面里没有纯黑像素。 */
  const uint8_t black[3] = {0, 0, 0};
  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, black, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "no matching pixel leaves the selection alone");
  COMP_CHECK(active_of(doc.document) == 0);
  COMP_CHECK(mask_sum(coverage_of(doc.document, 16, 8)) == 0);
  COMP_CHECK(undo_depth(doc.document) == depth_before);
}

COMP_TEST(color_range_rejects_invalid_arguments_before_any_write) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);
  const size_t depth_before = undo_depth(doc.document);

  const uint8_t include[3] = {220, 30, 30};
  int32_t changed = 7;
  /* 数组 NULL 但 count > 0。 */
  COMP_CHECK(comp_selection_apply_color_range(doc.document, nullptr, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 1, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* fuzziness 越界（上游 0...200）。 */
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 201, 0,
                                              COMP_SELECTION_REPLACE, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, -1, 0,
                                              COMP_SELECTION_REPLACE, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* 未知 combine。 */
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 0, 0,
                                              static_cast<comp_selection_combine>(3), &changed) ==
             COMP_ERR_INVALID_ARGUMENT);

  COMP_CHECK(changed == 7);
  COMP_CHECK(active_of(doc.document) == 0);
  COMP_CHECK(undo_depth(doc.document) == depth_before);
}

COMP_TEST(color_range_combine_add_and_subtract_existing_selections) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);

  /* 起点：左半矩形选区。 */
  int32_t changed = 0;
  COMP_CHECK(rect_select(doc.document, 0.0f, 0.0f, 8.0f, 8.0f, COMP_SELECTION_REPLACE, &changed) ==
             COMP_OK);
  COMP_CHECK(at(coverage_of(doc.document, 16, 8), 16, 9, 0) == 0);

  /* ADD 蓝色范围：蓝区并入，原有左半保留（右半的两个红点不选）。 */
  const uint8_t blue[3] = {30, 30, 220};
  COMP_CHECK(comp_selection_apply_color_range(doc.document, blue, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_ADD, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> added = coverage_of(doc.document, 16, 8);
  COMP_CHECK(at(added, 16, 0, 0) == 255);  /* 原矩形保留 */
  COMP_CHECK(at(added, 16, 8, 0) == 255);  /* 蓝区并入 */
  COMP_CHECK(at(added, 16, 9, 1) == 0);    /* 红点不是蓝 */
  COMP_CHECK(at(added, 16, 14, 6) == 0);
  COMP_CHECK_MSG(mask_sum(added) == 255L * 64L + 255L * ColorRangeImage::kBluePixels,
                 "ADD takes the union with the existing selection");

  /* 重新全选，再 SUBTRACT 蓝色范围：蓝区被抠掉。 */
  COMP_CHECK(rect_select(doc.document, 0.0f, 0.0f, 16.0f, 8.0f, COMP_SELECTION_REPLACE, &changed) ==
             COMP_OK);
  COMP_CHECK(comp_selection_apply_color_range(doc.document, blue, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_SUBTRACT, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> subtracted = coverage_of(doc.document, 16, 8);
  COMP_CHECK(at(subtracted, 16, 0, 0) == 255);
  COMP_CHECK(at(subtracted, 16, 8, 0) == 0);
  COMP_CHECK(at(subtracted, 16, 9, 1) == 255);
  COMP_CHECK_MSG(mask_sum(subtracted) == 255L * (128L - ColorRangeImage::kBluePixels),
                 "SUBTRACT removes only the color range");
}

COMP_TEST(color_range_records_one_undo_step_and_restores) {
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, ColorRangeImage::build()) != 0);
  const size_t depth_before = undo_depth(doc.document);

  const uint8_t include[3] = {220, 30, 30};
  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, include, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(undo_depth(doc.document) == depth_before + 1);

  COMP_CHECK(comp_document_undo(doc.document) == COMP_OK);
  COMP_CHECK_MSG(active_of(doc.document) == 0, "undo clears the color-range selection");
  COMP_CHECK(undo_depth(doc.document) == depth_before);
}

COMP_TEST(color_range_runs_on_the_merged_composite) {
  /* 两层各画一半：色彩范围采样的是整篇可见合成（sampleAllLayers），不是活动层。 */
  Doc doc(ColorRangeImage::kWidth, ColorRangeImage::kHeight);
  COMP_CHECK(doc.document != nullptr);
  std::vector<uint8_t> left(static_cast<size_t>(16) * 8 * 4u, 0u);
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      uint8_t* p = left.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      if (x < 8) {
        p[0] = 220;
        p[1] = 30;
        p[2] = 30;
        p[3] = 255;
      }
    }
  }
  std::vector<uint8_t> right(static_cast<size_t>(16) * 8 * 4u, 0u);
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      uint8_t* p = right.data() + (static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4u;
      if (x >= 8) {
        p[0] = 30;
        p[1] = 30;
        p[2] = 220;
        p[3] = 255;
      }
    }
  }
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, left) != 0);
  COMP_CHECK(add_rgba8_layer(doc.document, comp_rect{0, 0, 16, 8}, right) != 0);

  const uint8_t red[3] = {220, 30, 30};
  int32_t changed = -1;
  COMP_CHECK(comp_selection_apply_color_range(doc.document, red, 1, nullptr, 0, 0, 0,
                                              COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> mask = coverage_of(doc.document, 16, 8);
  COMP_CHECK(at(mask, 16, 0, 0) == 255);
  COMP_CHECK(at(mask, 16, 8, 0) == 0);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * 64L,
                 "the match runs on the merged composite across layers");
}
