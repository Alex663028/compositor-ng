// AI生成
/*
 * test_filters.cpp — P1 滤镜批次（Vignette / Tonal Contrast / Bloom /
 * Lens Correction / Gaussian Blur / Add Noise / Grain）的回归护栏。
 *
 * 为什么需要这组用例：这七个算子在上游是 Filter 菜单的一条通路
 * （Filters.swift + PixelFilter.run），本仓的像素内核早已就位，缺的是产品入口。
 * 本组用例冻结入口落地之后的契约：
 *   1. 恒等设置是**真 no-op**：不动像素、不开事务、out_changed = 0；
 *   2. 参数闸在开事务之前：越界参数 -> INVALID_ARGUMENT，不进撤销栈；
 *   3. 七个算子分别可观察地改变像素，且各恰好一步撤销；
 *   4. Bloom 只加光、不改 alpha（预乘不变式：颜色不超过 alpha）；
 *   5. 选区委剪按**文档坐标**混合；图层带偏移时用 effective_bounds() 的原点；
 *   6. 16 位图层落 8 位工作副本并在 note 里写明，撤销恢复 16 位；
 *   7. 组图层 / 无像素 / HDR -> COMP_ERR_STATE；未知图层 -> COMP_ERR_NOT_FOUND；
 *   8. Lens Correction 的 k = distortion/100*0.35 与 COMP_TOOL_LENS 的 k 等价
 *      （同一条内核，不该有第二套口径）；
 *   9. C ABI 的 note 两段式与 out_changed 契约。
 *
 * 断言只看「方向 / 是否改变 / 逐字节相等」，不重算内核公式——内核自身的
 * 数值口径由 imaging 那批用例钉住，这里测的是接线与事务边界。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_filter.h"
#include "compositor/comp_layer.h"
#include "domain/document.h"
#include "domain/filters.h"
#include "domain/layer.h"
#include "domain/selection.h"
#include "domain/tool_ops.h"
#include "engine.h"
#include "test_framework.h"

using namespace compositor;

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-filter-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_layer_id add_solid_layer(Document* document, uint8_t r, uint8_t g, uint8_t b) {
  comp_layer_desc desc{};
  desc.name = "solid";
  desc.transform = comp_rect{0, 0, document->state().width, document->state().height};
  comp_layer_id id = 0;
  Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels = ImageBuffer::solid(document->state().width, document->state().height,
                                     COMP_PIXEL_RGBA8, r, g, b, 255);
  return id;
}

/* 铺一块指定字节的 RGBA8 图层（尺寸必须等于画布）。 */
comp_layer_id add_layer_with(Document* document, const std::vector<uint8_t>& bytes,
                             comp_rect transform) {
  comp_layer_desc desc{};
  desc.name = "pattern";
  desc.transform = transform;
  comp_layer_id id = 0;
  Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels.resize(static_cast<int32_t>(transform.w), static_cast<int32_t>(transform.h),
                       COMP_PIXEL_RGBA8);
  if (layer->pixels.byte_size() == bytes.size()) {
    std::memcpy(layer->pixels.writable_pixels().data(), bytes.data(), bytes.size());
  }
  return id;
}

/* 预乘 RGBA16 图层：alpha 与三通道随位置变化（与 test_tool_ops_domain 同款）。 */
comp_layer_id add_wide_layer(Document* document) {
  const int32_t w = document->state().width;
  const int32_t h = document->state().height;
  comp_layer_desc desc{};
  desc.name = "wide";
  desc.transform = comp_rect{0, 0, w, h};
  comp_layer_id id = 0;
  Layer* layer = document->create_layer(desc, &id);
  if (layer == nullptr) return 0;
  layer->pixels.resize(w, h, COMP_PIXEL_RGBA16);
  uint16_t* wide = reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data());
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      uint16_t* px = wide + (static_cast<size_t>(y) * static_cast<size_t>(w) +
                             static_cast<size_t>(x)) * 4u;
      const uint16_t alpha = static_cast<uint16_t>(0x4000 + 0x1000 * (x + y));
      px[0] = static_cast<uint16_t>(alpha / 2u);
      px[1] = static_cast<uint16_t>(alpha / 3u);
      px[2] = static_cast<uint16_t>(alpha / 4u);
      px[3] = alpha;
    }
  }
  return id;
}

comp_layer_id add_hdr_layer(Document* document) {
  comp_layer_desc desc{};
  desc.name = "hdr";
  desc.transform = comp_rect{0, 0, document->state().width, document->state().height};
  comp_layer_id id = 0;
  Layer* layer = document->create_layer(desc, &id);
  if (layer != nullptr) {
    layer->pixels.resize(document->state().width, document->state().height, COMP_PIXEL_RGBA16F);
  }
  return id;
}

std::vector<uint8_t> pixels_of(const Document& document, comp_layer_id id) {
  const Layer* layer = document.find(id);
  if (layer == nullptr) return {};
  return layer->pixels.pixel_data();
}

size_t changed_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return 0;
  size_t changed = 0;
  for (size_t i = 0; i + 3 < a.size(); i += 4) {
    if (std::memcmp(a.data() + i, b.data() + i, 4) != 0) ++changed;
  }
  return changed;
}

double luma_at(const std::vector<uint8_t>& rgba, int32_t width, int32_t x, int32_t y) {
  const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                    static_cast<size_t>(x)) * 4u;
  return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

uint8_t channel_at(const std::vector<uint8_t>& rgba, int32_t width, int32_t x, int32_t y,
                   int32_t channel) {
  return rgba[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u +
              static_cast<size_t>(channel)];
}

/* 4 像素一格的棋盘格：给 Tonal Contrast 制造细节（规则场才有确定的方向）。 */
std::vector<uint8_t> checkerboard(int32_t width, int32_t height) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t value = (((x / 4) + (y / 4)) & 1) ? 235 : 20;
      uint8_t* p = out.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                 static_cast<size_t>(x)) * 4u;
      p[0] = p[1] = p[2] = value;
    }
  }
  return out;
}

/* 黑底上的一块 16x16 白光：给 Bloom 一个可观察的亮部。 */
std::vector<uint8_t> dot_on_black(int32_t width, int32_t height) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = out.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                 static_cast<size_t>(x)) * 4u;
      p[3] = 255;
      if (x >= 24 && x < 40 && y >= 24 && y < 40) {
        p[0] = p[1] = p[2] = 255;
      }
    }
  }
  return out;
}

void select_rect(Document* document, float x0, float y0, float x1, float y1) {
  const float points[4] = {x0, y0, x1, y1};
  bool changed = false;
  COMP_CHECK_MSG(selection_apply_shape(&document->state().selection, document->state().width,
                                       document->state().height, kSelectionRect, points, 2,
                                       kSelectionReplace, false, &changed),
                 "the test can install a rectangular selection");
  COMP_CHECK(changed);
}

FilterSettings vignette_settings(double amount) {
  FilterSettings settings;
  settings.kind = kFilterVignette;
  settings.vignette.amount = amount;
  settings.vignette.midpoint = 50.0;
  settings.vignette.roundness = 100.0;
  settings.vignette.feather = 60.0;
  settings.vignette.highlights = 0.0;
  return settings;
}

size_t undo_depth(const Document& document) { return document.history().undo_depth(); }

}  // namespace

/* ================================================== 1. 恒等与参数闸 */

COMP_TEST(filters_identity_is_a_true_noop) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 40, 80, 120);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = undo_depth(document);

  FilterSettings settings = vignette_settings(0.0);
  /* 恒等判据只看 amount；其余字段是「非默认但无效」也要保持 no-op。 */
  settings.vignette.roundness = -100.0;
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = true;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, settings, 1.0, &code, &changed, &note, &error),
                 "an identity filter is a successful call");
  COMP_CHECK_MSG(code == COMP_OK, "success writes COMP_OK to out_code");
  COMP_CHECK_MSG(!changed, "an identity filter reports out_changed = false");
  COMP_CHECK_MSG(!document.txn_active(), "an identity filter opens no transaction");
  COMP_CHECK_MSG(undo_depth(document) == undo_before, "an identity filter adds no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "an identity filter leaves pixels byte-identical");
  COMP_CHECK_MSG(note.find("identity") != std::string::npos,
                 "the note says why nothing happened");
}

COMP_TEST(filters_new_kinds_are_identity_at_zero_initialisation) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 40, 80, 120);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = undo_depth(document);

  const FilterKind kinds[] = {kFilterGaussianBlur, kFilterAddNoise, kFilterGrain};
  for (FilterKind kind : kinds) {
    FilterSettings settings;
    settings.kind = kind; /* 其余字段零初始化：radius / amount 为 0 = 恒等 */
    comp_result code = COMP_ERR_INTERNAL;
    bool changed = true;
    std::string note;
    std::string error;
    COMP_CHECK_MSG(filter_apply(&document, id, settings, 1.0, &code, &changed, &note, &error),
                   "zero-initialised settings are a successful call");
    COMP_CHECK_MSG(code == COMP_OK, "success writes COMP_OK to out_code");
    COMP_CHECK_MSG(!changed, "zero-initialised settings report out_changed = false");
    COMP_CHECK_MSG(note.find("identity") != std::string::npos, "the note says why nothing happened");
  }
  COMP_CHECK_MSG(!document.txn_active(), "identity filters open no transaction");
  COMP_CHECK_MSG(undo_depth(document) == undo_before, "identity filters add no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "identity filters leave pixels byte-identical");
}

COMP_TEST(filters_reject_out_of_range_settings_before_the_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 40, 80, 120);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = undo_depth(document);

  struct Case {
    const char* why;
    FilterSettings settings;
  };
  std::vector<Case> cases;
  {
    FilterSettings s = vignette_settings(100.0);
    s.vignette.midpoint = 101.0;
    cases.push_back({"vignette midpoint above 100", s});
  }
  {
    FilterSettings s = vignette_settings(100.0);
    s.vignette.color[2] = 1.5;
    cases.push_back({"vignette color above 1", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterTonalContrast;
    s.tonal_contrast.amount = 50.0;
    s.tonal_contrast.radius = 0.0; /* 0 不是「不模糊」，而是越界 */
    cases.push_back({"tonal contrast radius below 1", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterBloom;
    s.bloom.amount = 50.0;
    s.bloom.radius = 151.0;
    cases.push_back({"bloom radius above 150", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterLensCorrection;
    s.lens_correction.distortion = -101.0;
    cases.push_back({"lens distortion below -100", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterGaussianBlur;
    s.gaussian_blur.radius = 0.05; /* (0, 0.1) 是越界，不是「四舍五入到 0」 */
    cases.push_back({"gaussian blur radius between 0 and 0.1", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterGaussianBlur;
    s.gaussian_blur.radius = 250.1;
    cases.push_back({"gaussian blur radius above 250", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterAddNoise;
    s.add_noise.amount = 0.05;
    cases.push_back({"add noise amount between 0 and 0.1", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterAddNoise;
    s.add_noise.amount = 400.1;
    cases.push_back({"add noise amount above 400", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterGrain;
    s.grain.amount = 40.0;
    s.grain.size = 0.4;
    cases.push_back({"grain size below 0.5", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterGrain;
    s.grain.amount = 40.0;
    s.grain.size = 20.1;
    cases.push_back({"grain size above 20", s});
  }
  {
    FilterSettings s;
    s.kind = kFilterGrain;
    s.grain.amount = 40.0;
    s.grain.roughness = 101.0;
    cases.push_back({"grain roughness above 100", s});
  }

  for (const Case& item : cases) {
    comp_result code = COMP_OK;
    bool changed = true;
    std::string note;
    std::string error;
    COMP_CHECK_MSG(!filter_apply(&document, id, item.settings, 1.0, &code, &changed, &note, &error),
                   item.why);
    COMP_CHECK_MSG(code == COMP_ERR_INVALID_ARGUMENT, "out-of-range settings are INVALID_ARGUMENT");
    COMP_CHECK_MSG(!changed, "a rejected call never reports changed");
    COMP_CHECK_MSG(!error.empty(), "a rejected call explains itself in error");
  }

  /* 非有限的 scale 也是参数错误，不能落进内核。 */
  {
    comp_result code = COMP_OK;
    std::string error;
    COMP_CHECK_MSG(!filter_apply(&document, id, vignette_settings(50.0),
                                 std::numeric_limits<double>::quiet_NaN(), &code, nullptr,
                                 nullptr, &error),
                   "a NaN scale is rejected");
    COMP_CHECK(code == COMP_ERR_INVALID_ARGUMENT);
  }

  COMP_CHECK_MSG(!document.txn_active(), "the gate runs before txn_begin: no transaction is left open");
  COMP_CHECK_MSG(undo_depth(document) == undo_before, "rejected filters add no undo step");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "rejected filters leave pixels byte-identical");
}

/* ================================================== 2. 七个算子的像素通路 */

COMP_TEST(filters_vignette_darkens_the_edges_and_is_one_step_undo) {
  Engine engine(test_engine_desc());
  Document document(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 200, 200, 200);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = undo_depth(document);

  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, vignette_settings(100.0), 1.0, &code, &changed, &note,
                              &error),
                 "a vignette is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(changed, "a non-identity vignette reports changed");
  COMP_CHECK_MSG(note.find("Vignette applied") != std::string::npos, "the note names the filter");

  const std::vector<uint8_t> after = pixels_of(document, id);
  COMP_CHECK_MSG(luma_at(after, 32, 0, 0) < luma_at(before, 32, 0, 0) - 50.0,
                 "the corner is pulled toward the vignette color");
  COMP_CHECK_MSG(after[(static_cast<size_t>(16) * 32 + 16) * 4] ==
                     before[(static_cast<size_t>(16) * 32 + 16) * 4],
                 "the middle (inside the midpoint) is untouched");
  COMP_CHECK_MSG(channel_at(after, 32, 0, 0, 3) == 255,
                 "a non-fillsClear vignette keeps coverage as it was");

  COMP_CHECK_MSG(undo_depth(document) == undo_before + 1, "the vignette is exactly one undo step");
  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the pixels byte for byte");
}

COMP_TEST(filters_tonal_contrast_accents_local_detail) {
  Engine engine(test_engine_desc());
  Document document(&engine, 32, 32, 72.0f, "sRGB");
  const std::vector<uint8_t> pattern = checkerboard(32, 32);
  const comp_layer_id id = add_layer_with(&document, pattern, comp_rect{0, 0, 32, 32});
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(before == pattern);

  FilterSettings settings;
  settings.kind = kFilterTonalContrast;
  settings.tonal_contrast.amount = 60.0;
  settings.tonal_contrast.radius = 2.0;
  settings.tonal_contrast.shadows = 40.0;
  settings.tonal_contrast.midtones = 60.0;
  settings.tonal_contrast.highlights = 30.0;

  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, settings, 1.0, &code, &changed, nullptr, &error),
                 "tonal contrast is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(changed, "tonal contrast reports changed");

  const std::vector<uint8_t> after = pixels_of(document, id);
  COMP_CHECK_MSG(changed_pixels(before, after) > 0,
                 "tonal contrast actually moves the checkerboard");
  /*
   * 每个像素都带细节，所以「改了多少像素」几乎总是全部——不能拿它当强度上限。
   * 真正要冻结的是「局部对比被强调、但没有把棋盘抹成一片」：相邻两格
   * （20 / 235）在结果里仍是明确的暗格与亮格，且间隔没有被压平。
   */
  const double dark_luma = luma_at(after, 32, 0, 0);
  const double light_luma = luma_at(after, 32, 4, 0);
  COMP_CHECK_MSG(dark_luma < 128.0, "the dark square stays on the dark side");
  COMP_CHECK_MSG(light_luma > 128.0, "the light square stays on the light side");
  COMP_CHECK_MSG(light_luma - dark_luma > 100.0,
                 "local contrast survives instead of flattening to a single tone");

  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the checkerboard byte for byte");
}

COMP_TEST(filters_bloom_adds_light_without_touching_alpha) {
  Engine engine(test_engine_desc());
  Document document(&engine, 64, 64, 72.0f, "sRGB");
  const std::vector<uint8_t> pattern = dot_on_black(64, 64);
  const comp_layer_id id = add_layer_with(&document, pattern, comp_rect{0, 0, 64, 64});
  const std::vector<uint8_t> before = pixels_of(document, id);

  FilterSettings settings;
  settings.kind = kFilterBloom;
  settings.bloom.amount = 100.0;
  settings.bloom.radius = 4.0;

  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, settings, 1.0, &code, &changed, &note, &error),
                 "bloom is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(changed, "bloom reports changed");
  COMP_CHECK_MSG(note.find("approximation") != std::string::npos,
                 "the note is honest about CIBloom's closed curve");

  const std::vector<uint8_t> after = pixels_of(document, id);
  COMP_CHECK_MSG(luma_at(after, 64, 41, 32) > luma_at(before, 64, 41, 32),
                 "the glow spreads just outside the bright block");
  COMP_CHECK_MSG(luma_at(after, 64, 0, 0) == luma_at(before, 64, 0, 0),
                 "the far corner is outside the glow radius");
  bool alpha_untouched = true;
  bool premultiplied = true;
  for (size_t i = 0; i + 3 < after.size(); i += 4) {
    if (after[i + 3] != before[i + 3]) alpha_untouched = false;
    for (size_t c = 0; c < 3; ++c) {
      if (after[i + c] > after[i + 3]) premultiplied = false;
    }
  }
  COMP_CHECK_MSG(alpha_untouched, "bloom never changes coverage (the layer outline stays put)");
  COMP_CHECK_MSG(premultiplied, "the added light is clamped to each pixel's own alpha");

  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the glow-free pixels");
}

COMP_TEST(filters_lens_correction_matches_the_lens_tool_kernel) {
  /*
   * 同一份畸变有两个入口：Filter 菜单（distortion -100..100）与工具层
   * （COMP_TOOL_LENS，k -1..1）。两者必须是同一条内核、同一个 k 换算
   * （distortion/100*0.35），否则同一张图从两个菜单得到两种畸变。
   */
  Engine engine(test_engine_desc());
  Document via_filter(&engine, 32, 32, 72.0f, "sRGB");
  Document via_tool(&engine, 32, 32, 72.0f, "sRGB");
  const std::vector<uint8_t> pattern = checkerboard(32, 32);
  const comp_layer_id filter_layer = add_layer_with(&via_filter, pattern, comp_rect{0, 0, 32, 32});
  const comp_layer_id tool_layer = add_layer_with(&via_tool, pattern, comp_rect{0, 0, 32, 32});
  const std::vector<uint8_t> before = pixels_of(via_filter, filter_layer);

  FilterSettings settings;
  settings.kind = kFilterLensCorrection;
  settings.lens_correction.distortion = -100.0;
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&via_filter, filter_layer, settings, 1.0, &code, &changed, nullptr,
                              &error),
                 "lens correction is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);

  ToolOpParams params;
  params.kind = kToolOpLens;
  params.lens.k = -0.35;
  comp_result tool_code = COMP_ERR_INTERNAL;
  std::string tool_error;
  COMP_CHECK_MSG(run_tool_op(&via_tool, tool_layer, params, &tool_code, &tool_error, nullptr),
                 "the lens tool still runs");
  COMP_CHECK(tool_code == COMP_OK);

  COMP_CHECK_MSG(pixels_of(via_filter, filter_layer) == pixels_of(via_tool, tool_layer),
                 "filter distortion -100 and tool k -0.35 are the same kernel and the same k");

  const std::vector<uint8_t> after = pixels_of(via_filter, filter_layer);
  COMP_CHECK_MSG(after != before, "the lens correction actually moves pixels");

  via_filter.undo();
  COMP_CHECK_MSG(pixels_of(via_filter, filter_layer) == before, "one undo restores the source");
}

/* ================================================== 2b. P1 滤镜扩容：模糊 / 噪声 / 颗粒 */

namespace {

/* 透明底上的一个双色实心盘：给 Gaussian Blur 一个可观察的 alpha 边界与内部颜色。 */
std::vector<uint8_t> disc_on_transparent(int32_t width, int32_t height) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  const double cx = static_cast<double>(width) / 2.0;
  const double cy = static_cast<double>(height) / 2.0;
  const double radius = static_cast<double>(width) / 4.0;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const double dx = static_cast<double>(x) + 0.5 - cx;
      const double dy = static_cast<double>(y) + 0.5 - cy;
      if (dx * dx + dy * dy > radius * radius) continue;
      uint8_t* p = out.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                 static_cast<size_t>(x)) * 4u;
      /* 左半红、右半蓝：纯色盘在预乘空间里模糊后反预乘仍是同一色，
       * 只有内部有颜色落差时 blur 才有可观察的颜色变化。 */
      const bool left = x < width / 2;
      p[0] = left ? 255 : 0;
      p[1] = 0;
      p[2] = left ? 0 : 255;
      p[3] = 255;
    }
  }
  return out;
}

bool alpha_channel_identical(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 3; i < a.size(); i += 4) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

}  // namespace

COMP_TEST(filters_gaussian_blur_softens_and_preserve_keeps_the_outline) {
  Engine engine(test_engine_desc());
  const std::vector<uint8_t> pattern = disc_on_transparent(64, 64);

  Document plain(&engine, 64, 64, 72.0f, "sRGB");
  const comp_layer_id plain_id = add_layer_with(&plain, pattern, comp_rect{0, 0, 64, 64});
  const size_t undo_before = undo_depth(plain);

  FilterSettings settings;
  settings.kind = kFilterGaussianBlur;
  settings.gaussian_blur.radius = 12.0;
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&plain, plain_id, settings, 1.0, &code, &changed, &note, &error),
                 "gaussian blur is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);
  COMP_CHECK_MSG(undo_depth(plain) == undo_before + 1, "one blur is exactly one undo step");
  COMP_CHECK_MSG(note.find("box approximation") != std::string::npos,
                 "the note records the approximation instead of claiming CI parity");

  const std::vector<uint8_t> blurred = pixels_of(plain, plain_id);
  COMP_CHECK_MSG(changed_pixels(blurred, pattern) > 0, "the blur actually moves pixels");
  /* 关掉 preserve：整份预乘像素一起模糊，alpha 会向外扩散（上游默认行为）。 */
  COMP_CHECK_MSG(channel_at(blurred, 64, 32, 20, 3) > 0,
                 "without preserve the alpha spreads outside the original disc");

  Document preserved(&engine, 64, 64, 72.0f, "sRGB");
  const comp_layer_id preserved_id = add_layer_with(&preserved, pattern, comp_rect{0, 0, 64, 64});
  settings.gaussian_blur.preserve_transparency = true;
  COMP_CHECK_MSG(filter_apply(&preserved, preserved_id, settings, 1.0, &code, &changed, &note,
                              &error),
                 "gaussian blur with preserve_transparency runs");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);
  const std::vector<uint8_t> kept = pixels_of(preserved, preserved_id);
  COMP_CHECK_MSG(alpha_channel_identical(kept, pattern),
                 "preserve_transparency keeps every original alpha byte");
  COMP_CHECK_MSG(changed_pixels(kept, pattern) > 0, "the preserved run still blurs the colour");
  COMP_CHECK_MSG(changed_pixels(kept, blurred) > 0,
                 "preserve_transparency is a different result from the default path");

  plain.undo();
  COMP_CHECK_MSG(pixels_of(plain, plain_id) == pattern, "one undo restores the source");
}

COMP_TEST(filters_add_noise_is_seeded_and_anchored_in_document_coordinates) {
  Engine engine(test_engine_desc());
  std::vector<uint8_t> flat(32 * 32 * 4, 128);
  for (size_t i = 3; i < flat.size(); i += 4) flat[i] = 255;

  FilterSettings settings;
  settings.kind = kFilterAddNoise;
  settings.add_noise.amount = 50.0;
  settings.add_noise.seed = 7;

  Document first(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id first_id = add_layer_with(&first, flat, comp_rect{0, 0, 32, 32});
  const size_t undo_before = undo_depth(first);
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&first, first_id, settings, 1.0, &code, &changed, nullptr, &error),
                 "add noise is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);
  COMP_CHECK_MSG(undo_depth(first) == undo_before + 1, "one noise run is exactly one undo step");
  const std::vector<uint8_t> noisy = pixels_of(first, first_id);
  COMP_CHECK_MSG(noisy != flat, "the noise actually moves pixels");

  /* 同 seed + 同图层原点：逐字节复现。 */
  Document second(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id second_id = add_layer_with(&second, flat, comp_rect{0, 0, 32, 32});
  COMP_CHECK(filter_apply(&second, second_id, settings, 1.0, &code, &changed, nullptr, &error));
  COMP_CHECK_MSG(pixels_of(second, second_id) == noisy,
                 "the same seed and origin reproduce the noise byte for byte");

  /* 换 seed：同一张图必须得到不同的噪声场。 */
  settings.add_noise.seed = 8;
  Document third(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id third_id = add_layer_with(&third, flat, comp_rect{0, 0, 32, 32});
  COMP_CHECK(filter_apply(&third, third_id, settings, 1.0, &code, &changed, nullptr, &error));
  COMP_CHECK_MSG(pixels_of(third, third_id) != noisy, "a different seed produces a different field");

  /* 噪声场锚在文档坐标：同一张图、缩进 16 px 的同样图层，重叠处逐字节一致。 */
  settings.add_noise.seed = 7;
  Document offset(&engine, 32, 32, 72.0f, "sRGB");
  std::vector<uint8_t> slice(16 * 32 * 4, 128);
  for (size_t i = 3; i < slice.size(); i += 4) slice[i] = 255;
  const comp_layer_id offset_id = add_layer_with(&offset, slice, comp_rect{16, 0, 16, 32});
  COMP_CHECK(filter_apply(&offset, offset_id, settings, 1.0, &code, &changed, nullptr, &error));
  const std::vector<uint8_t> shifted = pixels_of(offset, offset_id);
  bool anchored = true;
  for (int32_t y = 0; y < 32 && anchored; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      const size_t at = (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
      const size_t full = (static_cast<size_t>(y) * 32u + static_cast<size_t>(x + 16)) * 4u;
      if (std::memcmp(shifted.data() + at, noisy.data() + full, 4) != 0) {
        anchored = false;
        break;
      }
    }
  }
  COMP_CHECK_MSG(anchored, "noise is anchored in document coordinates, not layer pixels");

  /* monochromatic：三通道加同一个值，只改亮度。 */
  Document mono(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id mono_id = add_layer_with(&mono, flat, comp_rect{0, 0, 32, 32});
  settings.add_noise.monochromatic = true;
  COMP_CHECK(filter_apply(&mono, mono_id, settings, 1.0, &code, &changed, nullptr, &error));
  const std::vector<uint8_t> mono_pixels = pixels_of(mono, mono_id);
  bool monochrome = true;
  for (size_t i = 0; i + 3 < mono_pixels.size(); i += 4) {
    if (mono_pixels[i] != mono_pixels[i + 1] || mono_pixels[i] != mono_pixels[i + 2]) {
      monochrome = false;
      break;
    }
  }
  COMP_CHECK_MSG(monochrome, "monochromatic noise keeps the three channels equal");

  first.undo();
  COMP_CHECK_MSG(pixels_of(first, first_id) == flat, "one undo restores the source");
}

COMP_TEST(filters_grain_is_seeded_and_is_one_step_undo) {
  Engine engine(test_engine_desc());
  std::vector<uint8_t> flat(32 * 32 * 4, 128);
  for (size_t i = 3; i < flat.size(); i += 4) flat[i] = 255;

  FilterSettings settings;
  settings.kind = kFilterGrain;
  settings.grain.amount = 60.0;
  settings.grain.size = 1.5;
  settings.grain.roughness = 50.0;
  settings.grain.seed = 3;

  Document first(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id first_id = add_layer_with(&first, flat, comp_rect{0, 0, 32, 32});
  const size_t undo_before = undo_depth(first);
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&first, first_id, settings, 1.0, &code, &changed, nullptr, &error),
                 "grain is a real product path");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);
  COMP_CHECK_MSG(undo_depth(first) == undo_before + 1, "one grain run is exactly one undo step");
  const std::vector<uint8_t> grained = pixels_of(first, first_id);
  COMP_CHECK_MSG(grained != flat, "the grain actually moves pixels");

  Document second(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id second_id = add_layer_with(&second, flat, comp_rect{0, 0, 32, 32});
  COMP_CHECK(filter_apply(&second, second_id, settings, 1.0, &code, &changed, nullptr, &error));
  COMP_CHECK_MSG(pixels_of(second, second_id) == grained,
                 "the same seed reproduces the grain byte for byte");

  settings.grain.seed = 4;
  Document third(&engine, 32, 32, 72.0f, "sRGB");
  const comp_layer_id third_id = add_layer_with(&third, flat, comp_rect{0, 0, 32, 32});
  COMP_CHECK(filter_apply(&third, third_id, settings, 1.0, &code, &changed, nullptr, &error));
  COMP_CHECK_MSG(pixels_of(third, third_id) != grained,
                 "a different seed produces a different grain field");

  first.undo();
  COMP_CHECK_MSG(pixels_of(first, first_id) == flat, "one undo restores the source");
}

/* ================================================== 3. 选区 / 深度 / 拒绝 */

COMP_TEST(filters_clip_to_the_document_selection_in_document_coordinates) {
  /*
   * 图层带偏移：transform = {8,0,16,8}，画布 32x8。选区是文档 x ∈ [0,16)，
   * 因此只覆盖图层的**左半**（图层 x ∈ [0,8)）。右半必须逐字节不变——
   * 这同时钉住「图层像素 + effective_bounds().x = 文档坐标」这条平移约定。
   */
  Engine engine(test_engine_desc());
  Document document(&engine, 32, 8, 72.0f, "sRGB");
  const std::vector<uint8_t> pattern = checkerboard(16, 8);
  const comp_layer_id id = add_layer_with(&document, pattern, comp_rect{8, 0, 16, 8});
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(before == pattern);

  select_rect(&document, 0.0f, 0.0f, 16.0f, 8.0f);

  FilterSettings settings;
  settings.kind = kFilterLensCorrection;
  /* k < 0：角点采样到图层外 -> 透明。左半一定有可观察变化。 */
  settings.lens_correction.distortion = -100.0;
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, settings, 1.0, &code, &changed, &note, &error),
                 "a filter with a selection runs");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);
  COMP_CHECK_MSG(note.find("clipped to the document selection") != std::string::npos,
                 "the note says the result was clipped");

  const std::vector<uint8_t> after = pixels_of(document, id);
  /* 未选中的右半（图层 x >= 8）逐字节不变。 */
  for (int32_t y = 0; y < 8; ++y) {
    for (int32_t x = 8; x < 16; ++x) {
      const size_t at = (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
      COMP_CHECK_MSG(std::memcmp(after.data() + at, before.data() + at, 4) == 0,
                     "pixels outside the document selection keep their bytes");
    }
  }
  /* 选中的左半确实变了（角点处的 alpha 被 k<0 的重采样清掉）。 */
  COMP_CHECK_MSG(channel_at(after, 16, 0, 0, 3) < channel_at(before, 16, 0, 0, 3),
                 "the selected half really took the lens correction");

  document.undo();
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the clipped result");
}

COMP_TEST(filters_take_a_sixteen_bit_layer_through_its_working_copy) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_wide_layer(&document);
  const std::vector<uint8_t> before = pixels_of(document, id);
  COMP_CHECK(document.find(id)->pixels.format == COMP_PIXEL_RGBA16);

  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, vignette_settings(100.0), 1.0, &code, &changed, &note,
                              &error),
                 "a filter accepts a 16-bit layer through its working copy");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK(changed);
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA8,
                 "the layer is 8-bit afterward (DeepLayers.swift:5, same as the tools)");
  COMP_CHECK_MSG(note.find("8-bit working copy") != std::string::npos,
                 "the note records the downgrade instead of doing it silently");
  COMP_CHECK_MSG(pixels_of(document, id) != before, "the working copy really took the filter");

  document.undo();
  COMP_CHECK_MSG(document.find(id)->pixels.format == COMP_PIXEL_RGBA16,
                 "undo restores the 16-bit asset, not an 8-bit copy");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "undo restores the wide pixels byte for byte");
}

COMP_TEST(filters_refuse_group_empty_and_hdr_layers) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id target = add_solid_layer(&document, 10, 20, 30);
  const std::vector<uint8_t> before = pixels_of(document, target);
  const size_t undo_before = undo_depth(document);

  comp_layer_desc group_desc{};
  group_desc.name = "group";
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(document.create_layer(group_desc, &group) != nullptr);

  comp_layer_desc empty_desc{};
  empty_desc.name = "empty";
  empty_desc.transform = comp_rect{0, 0, 8, 8};
  comp_layer_id empty = 0;
  Layer* empty_layer = document.create_layer(empty_desc, &empty);
  COMP_CHECK(empty_layer != nullptr);
  /*
   * create_layer 会给非组图层铺一张全透明缓冲区（document.cpp:125-128），
   * 那仍然是「有像素」。要覆盖 has_pixels() == false 的分支，必须显式清掉
   * 像素资产——例如导入失败或裁剪后留下的占位层。
   */
  empty_layer->pixels = ImageBuffer{};

  const comp_layer_id hdr = add_hdr_layer(&document);

  for (comp_layer_id target : {group, empty, hdr}) {
    comp_result code = COMP_OK;
    bool changed = true;
    std::string error;
    COMP_CHECK_MSG(!filter_apply(&document, target, vignette_settings(50.0), 1.0, &code, &changed,
                                 nullptr, &error),
                   "a group / empty / HDR layer is refused");
    COMP_CHECK_MSG(code == COMP_ERR_STATE, "all three are state errors, not argument errors");
    COMP_CHECK_MSG(!changed, "a refused filter never reports changed");
  }

  comp_result code = COMP_OK;
  std::string error;
  COMP_CHECK_MSG(!filter_apply(&document, 99999, vignette_settings(50.0), 1.0, &code, nullptr,
                               nullptr, &error),
                 "an unknown layer id is refused");
  COMP_CHECK_MSG(code == COMP_ERR_NOT_FOUND, "an unknown id is NOT_FOUND, not STATE");

  COMP_CHECK_MSG(!document.txn_active(), "all refusals happen before txn_begin");
  COMP_CHECK_MSG(undo_depth(document) == undo_before, "refusals record no history");
  COMP_CHECK_MSG(pixels_of(document, target) == before,
                 "refusals leave the pixel layer untouched");
}

COMP_TEST(filters_with_an_explicitly_empty_selection_are_a_noop) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 90, 90, 90);
  const std::vector<uint8_t> before = pixels_of(document, id);
  const size_t undo_before = undo_depth(document);

  /* 显式空选区：active 但 base 全 0（与「没有选区」可区分，上游 blend 结果相同）。 */
  document.state().selection.active = true;
  document.state().selection.feather = 0.0f;
  document.state().selection.base.resize(8, 8, 0);

  comp_result code = COMP_ERR_INTERNAL;
  bool changed = true;
  std::string note;
  std::string error;
  COMP_CHECK_MSG(filter_apply(&document, id, vignette_settings(100.0), 1.0, &code, &changed, &note,
                              &error),
                 "an explicitly empty selection is a successful no-op");
  COMP_CHECK(code == COMP_OK);
  COMP_CHECK_MSG(!changed, "nothing changes when the coverage is all zero");
  COMP_CHECK_MSG(note.find("empty") != std::string::npos, "the note explains the no-op");
  COMP_CHECK_MSG(!document.txn_active(), "the empty-selection shortcut opens no transaction");
  COMP_CHECK_MSG(undo_depth(document) == undo_before, "and records no history");
  COMP_CHECK_MSG(pixels_of(document, id) == before, "and leaves pixels byte-identical");
}

/* ================================================== 4. C ABI */

namespace {

comp_document as_document_handle(Document* document) {
  return reinterpret_cast<comp_document>(document);
}

}  // namespace

COMP_TEST(filters_abi_reports_changed_and_copies_the_note_in_two_stages) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 8, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 60, 70, 80);
  const std::vector<uint8_t> before = pixels_of(document, id);

  /* 零初始化 desc = Vignette + amount 0 = 恒等。 */
  comp_filter_desc identity{};
  size_t needed = 0;
  int32_t changed = -1;
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, &identity, 1.0, &changed,
                                   nullptr, 0, &needed) == COMP_OK,
                 "the ABI accepts a zero-initialized desc");
  COMP_CHECK_MSG(changed == 0, "identity reports out_changed = 0");
  COMP_CHECK_MSG(needed > 1, "capacity 0 still reports the note length (two-stage copy)");

  std::vector<char> note(needed, '\0');
  COMP_CHECK(comp_filter_apply(as_document_handle(&document), id, &identity, 1.0, &changed,
                               note.data(), note.size(), &needed) == COMP_OK);
  COMP_CHECK_MSG(std::string(note.data()).find("identity") != std::string::npos,
                 "the second stage copies the actual note text");

  /* 非恒等：changed = 1，note 点名滤镜。 */
  comp_filter_desc desc{};
  desc.kind = COMP_FILTER_VIGNETTE;
  desc.vignette.amount = 100.0;
  desc.vignette.midpoint = 50.0;
  desc.vignette.roundness = 100.0;
  desc.vignette.feather = 60.0;
  std::vector<char> applied(160, '\0');
  size_t applied_size = 0;
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, &desc, 1.0, &changed,
                                   applied.data(), applied.size(), &applied_size) == COMP_OK,
                 "a real vignette goes through the ABI");
  COMP_CHECK_MSG(changed == 1, "a real filter reports out_changed = 1");
  COMP_CHECK_MSG(std::string(applied.data()).find("Vignette applied") != std::string::npos,
                 "the note names the filter");
  COMP_CHECK_MSG(pixels_of(document, id) != before, "the ABI path changed the layer pixels");

  /* NULL desc：参数错误。 */
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, nullptr, 1.0, &changed,
                                   nullptr, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT,
                 "a null desc is an argument error");

  /* 越界量程同样在 ABI 边界被拒（域层负责闸，ABI 不做第二套口径）。 */
  desc.vignette.amount = 101.0;
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, &desc, 1.0, &changed,
                                   nullptr, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT,
                 "out-of-range amounts are rejected through the ABI too");
}

COMP_TEST(filters_abi_maps_the_p1_operator_fields) {
  Engine engine(test_engine_desc());
  Document document(&engine, 16, 16, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(&document, 128, 128, 128);

  char note[256] = {0};
  size_t note_size = 0;
  int32_t changed = 0;

  /* Gaussian Blur：radius + preserve_transparency 两个字段都要过 ABI。 */
  comp_filter_desc blur{};
  blur.kind = COMP_FILTER_GAUSSIAN_BLUR;
  blur.gaussian_blur.radius = 4.0;
  blur.gaussian_blur.preserve_transparency = 1;
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, &blur, 1.0, &changed, note,
                                   sizeof(note), &note_size) == COMP_OK,
                 "the ABI accepts the gaussian blur desc");
  COMP_CHECK_MSG(changed == 1, "the blur reports out_changed = 1");
  COMP_CHECK_MSG(std::string(note).find("Gaussian Blur applied") != std::string::npos,
                 "the note names the blur");
  COMP_CHECK_MSG(std::string(note).find("box approximation") != std::string::npos,
                 "the blur note keeps the approximation caveat");

  /* Add Noise：amount / gaussian / monochromatic / seed 四个字段。 */
  comp_filter_desc noise{};
  noise.kind = COMP_FILTER_ADD_NOISE;
  noise.add_noise.amount = 100.0;
  noise.add_noise.gaussian = 1;
  noise.add_noise.monochromatic = 1;
  noise.add_noise.seed = 9;
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, &noise, 1.0, &changed, note,
                                   sizeof(note), &note_size) == COMP_OK,
                 "the ABI accepts the add noise desc");
  COMP_CHECK_MSG(changed == 1, "the noise reports out_changed = 1");
  COMP_CHECK_MSG(std::string(note).find("Add Noise applied") != std::string::npos,
                 "the note names the noise");
  const std::vector<uint8_t> noisy = pixels_of(document, id);
  bool monochrome = true;
  for (size_t i = 0; i + 3 < noisy.size(); i += 4) {
    if (noisy[i] != noisy[i + 1] || noisy[i] != noisy[i + 2]) {
      monochrome = false;
      break;
    }
  }
  COMP_CHECK_MSG(monochrome, "monochromatic = 1 crosses the ABI as a real boolean");

  /* Grain：amount / size / roughness / seed。 */
  comp_filter_desc grain{};
  grain.kind = COMP_FILTER_GRAIN;
  grain.grain.amount = 50.0;
  grain.grain.size = 2.0;
  grain.grain.roughness = 40.0;
  grain.grain.seed = 5;
  COMP_CHECK_MSG(comp_filter_apply(as_document_handle(&document), id, &grain, 1.0, &changed, note,
                                   sizeof(note), &note_size) == COMP_OK,
                 "the ABI accepts the grain desc");
  COMP_CHECK_MSG(changed == 1, "the grain reports out_changed = 1");
  COMP_CHECK_MSG(std::string(note).find("Grain applied") != std::string::npos,
                 "the note names the grain");

  /* 三个新 kind 的越界量程同样在 ABI 边界被拒。 */
  blur.gaussian_blur.radius = 0.05;
  COMP_CHECK(comp_filter_apply(as_document_handle(&document), id, &blur, 1.0, &changed, nullptr, 0,
                               nullptr) == COMP_ERR_INVALID_ARGUMENT);
  noise.add_noise.amount = 0.05;
  COMP_CHECK(comp_filter_apply(as_document_handle(&document), id, &noise, 1.0, &changed, nullptr,
                               0, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  grain.grain.size = 0.4;
  COMP_CHECK(comp_filter_apply(as_document_handle(&document), id, &grain, 1.0, &changed, nullptr,
                               0, nullptr) == COMP_ERR_INVALID_ARGUMENT);
}
