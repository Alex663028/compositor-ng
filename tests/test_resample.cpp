// AI生成
/*
 * test_resample.cpp — 仿射重采样与采样模式（对齐上游 LayerTransform）。
 *
 * 覆盖：
 *   - 恒等变换逐字节精确（默认 High quality 在整数对齐下不产生任何漂移）；
 *   - 整数倍放大（Nearest 象限映射）；
 *   - 90 度顺时针旋转（像素级精确映射）；
 *   - 水平翻转；
 *   - Smooth 与 Nearest 在非整数倍缩放下的差异；
 *   - 采样模式的 .comp manifest 往返（键名 sampling）；
 *   - 采样模式的 ABI 校验；
 *   - Catmull-Rom 上冲被夹回预乘不变量（rgb <= alpha）。
 */
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compositing/resample.h"
#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "domain/layer.h"
#include "test_framework.h"

using namespace compositor;

namespace {

using Rgba = std::array<uint8_t, 4>;

const Rgba kRed{255, 0, 0, 255};
const Rgba kGreen{0, 255, 0, 255};
const Rgba kBlue{0, 0, 255, 255};
const Rgba kWhite{255, 255, 255, 255};

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-resample-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

/* 行优先 RGBA8 图像，生存期由 bytes 持有。 */
struct RawImage {
  std::vector<uint8_t> bytes;
  comp_image image{};

  RawImage(int32_t width, int32_t height, const std::vector<Rgba>& px)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
    for (size_t i = 0; i < px.size() && i * 4 + 3 < bytes.size(); ++i) {
      bytes[i * 4 + 0] = px[i][0];
      bytes[i * 4 + 1] = px[i][1];
      bytes[i * 4 + 2] = px[i][2];
      bytes[i * 4 + 3] = px[i][3];
    }
    image.width = width;
    image.height = height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(width) * 4u;
    image.pixels = bytes.data();
  }
};

int abs_diff(int a, int b) { return a > b ? a - b : b - a; }

bool matches(const std::vector<uint8_t>& canvas, int32_t width, int32_t x, int32_t y,
             const Rgba& c, int tolerance) {
  const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)) * 4u;
  if (index + 3 >= canvas.size()) return false;
  return abs_diff(canvas[index + 0], c[0]) <= tolerance &&
         abs_diff(canvas[index + 1], c[1]) <= tolerance &&
         abs_diff(canvas[index + 2], c[2]) <= tolerance &&
         abs_diff(canvas[index + 3], c[3]) <= tolerance;
}

std::vector<uint8_t> flatten(comp_document document, int32_t width, int32_t height) {
  std::vector<uint8_t> canvas(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  const comp_result result = comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8,
                                                 static_cast<size_t>(width) * 4u);
  COMP_CHECK_MSG(result == COMP_OK, "comp_render_flatten");
  return canvas;
}

comp_layer_id background_layer(comp_document document) {
  comp_layer_id ids[8] = {0};
  size_t count = 0;
  comp_layer_list(document, ids, 8, &count);
  return count > 0 ? ids[0] : 0;
}

/* 新建文档 + 单背景层，画布为 width x height。 */
comp_document make_document(comp_engine engine, int32_t width, int32_t height) {
  comp_document_desc desc{};
  desc.width = width;
  desc.height = height;
  comp_document document = nullptr;
  COMP_CHECK_MSG(comp_document_create(engine, &desc, &document) == COMP_OK, "create document");
  return document;
}

}  // namespace

COMP_TEST(resample_identity_is_bit_exact) {
  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 4, 4);

  std::vector<Rgba> expected;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      expected.push_back(Rgba{static_cast<uint8_t>(x * 50 + 10), static_cast<uint8_t>(y * 50 + 20),
                              static_cast<uint8_t>((x + y) * 30 + 5), 255});
    }
  }
  const comp_layer_id layer = background_layer(document);
  RawImage image(4, 4, expected);
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, layer, comp_rect{0, 0, 4, 4}, 0.0f) == COMP_OK);

  /* 默认采样为 High quality：恒等变换下必须逐字节等于源像素。 */
  comp_sampling read = COMP_SAMPLING_NEAREST;
  COMP_CHECK(comp_layer_get_sampling(document, layer, &read) == COMP_OK);
  COMP_CHECK_MSG(read == COMP_SAMPLING_HIGH_QUALITY, "new layer default sampling");

  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  bool exact = canvas.size() == image.bytes.size();
  if (exact) {
    for (size_t i = 0; i < canvas.size(); ++i) {
      if (canvas[i] != image.bytes[i]) { exact = false; break; }
    }
  }
  COMP_CHECK_MSG(exact, "identity resample is bit-exact");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(resample_scale_up_nearest_quadrants) {
  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 4, 4);

  const comp_layer_id layer = background_layer(document);
  /* 源 2x2：(0,0)=红,(1,0)=绿,(0,1)=蓝,(1,1)=白。 */
  RawImage image(2, 2, {kRed, kGreen, kBlue, kWhite});
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, layer, comp_rect{0, 0, 4, 4}, 0.0f) == COMP_OK);
  COMP_CHECK(comp_layer_set_sampling(document, layer, COMP_SAMPLING_NEAREST) == COMP_OK);

  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 0, 0, kRed, 0), "top-left quadrant = red");
  COMP_CHECK_MSG(matches(canvas, 4, 3, 0, kGreen, 0), "top-right quadrant = green");
  COMP_CHECK_MSG(matches(canvas, 4, 0, 3, kBlue, 0), "bottom-left quadrant = blue");
  COMP_CHECK_MSG(matches(canvas, 4, 3, 3, kWhite, 0), "bottom-right quadrant = white");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(resample_rotate_90_clockwise) {
  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 2, 2);

  const comp_layer_id layer = background_layer(document);
  RawImage image(2, 2, {kRed, kGreen, kBlue, kWhite});
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, layer, comp_rect{0, 0, 2, 2}, 90.0f) == COMP_OK);
  COMP_CHECK(comp_layer_set_sampling(document, layer, COMP_SAMPLING_NEAREST) == COMP_OK);

  /* 顺时针 90 度：(0,0)->(1,0), (1,0)->(1,1), (0,1)->(0,0), (1,1)->(0,1)。 */
  const std::vector<uint8_t> canvas = flatten(document, 2, 2);
  COMP_CHECK_MSG(matches(canvas, 2, 0, 0, kBlue, 0), "rotated (0,0) = source (0,1)");
  COMP_CHECK_MSG(matches(canvas, 2, 1, 0, kRed, 0), "rotated (1,0) = source (0,0)");
  COMP_CHECK_MSG(matches(canvas, 2, 0, 1, kWhite, 0), "rotated (0,1) = source (1,1)");
  COMP_CHECK_MSG(matches(canvas, 2, 1, 1, kGreen, 0), "rotated (1,1) = source (1,0)");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(resample_flip_x_mirrors_columns) {
  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 2, 2);

  const comp_layer_id layer = background_layer(document);
  RawImage image(2, 2, {kRed, kGreen, kBlue, kWhite});
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, layer, comp_rect{0, 0, 2, 2}, 0.0f) == COMP_OK);
  COMP_CHECK(comp_layer_set_sampling(document, layer, COMP_SAMPLING_NEAREST) == COMP_OK);

  comp_layer_desc flip_desc{};
  flip_desc.name = "Flipped";
  flip_desc.blend = COMP_BLEND_NORMAL;
  flip_desc.opacity = 1.0f;
  flip_desc.transform = comp_rect{0, 0, 2, 2};
  flip_desc.flip_x = 1;
  flip_desc.sampling = COMP_SAMPLING_NEAREST;
  comp_layer_id flipped = 0;
  COMP_CHECK(comp_layer_add(document, &flip_desc, &flipped) == COMP_OK);
  COMP_CHECK(comp_layer_set_pixels(document, flipped, &image.image) == COMP_OK);

  const std::vector<uint8_t> canvas = flatten(document, 2, 2);
  COMP_CHECK_MSG(matches(canvas, 2, 0, 0, kGreen, 0), "flipX (0,0) = source (1,0)");
  COMP_CHECK_MSG(matches(canvas, 2, 1, 0, kRed, 0), "flipX (1,0) = source (0,0)");
  COMP_CHECK_MSG(matches(canvas, 2, 0, 1, kWhite, 0), "flipX (0,1) = source (1,1)");
  COMP_CHECK_MSG(matches(canvas, 2, 1, 1, kBlue, 0), "flipX (1,1) = source (0,1)");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(resample_smooth_differs_from_nearest) {
  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 3, 3);

  const comp_layer_id layer = background_layer(document);
  RawImage image(2, 2, {kRed, kGreen, kBlue, kWhite});
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_transform(document, layer, comp_rect{0, 0, 3, 3}, 0.0f) == COMP_OK);

  COMP_CHECK(comp_layer_set_sampling(document, layer, COMP_SAMPLING_NEAREST) == COMP_OK);
  const std::vector<uint8_t> nearest = flatten(document, 3, 3);

  COMP_CHECK(comp_layer_set_sampling(document, layer, COMP_SAMPLING_SMOOTH) == COMP_OK);
  const std::vector<uint8_t> smooth = flatten(document, 3, 3);

  bool differs = false;
  for (size_t i = 0; i < nearest.size() && i < smooth.size(); ++i) {
    if (nearest[i] != smooth[i]) { differs = true; break; }
  }
  COMP_CHECK_MSG(differs, "Smooth interpolates on non-integer scaling");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * 预乘不变量回归：rgba 缓冲全引擎按预乘存放（rgb <= alpha），下游 blend_pixels
 * 与跨端校验（tools/comp_crosscheck.cpp）都依赖它。最近邻是直接取样、双线性是
 * 凸组合，二者天然保持；只有 Catmull-Rom 的四点权重带负瓣，颜色能在低 alpha 处
 * 上冲越过 alpha。
 *
 * 图案取自搜索出的最小违反样例：源 3x1 行 [(0,0,0,0), (96,96,96,96), (0,0,0,255)]
 * 放大到 9x3。断言既检查全图不变量，也钉住被夹回后的精确字节——实测表明这两列
 * 在加护栏前是 (32,32,32,23) 与 (75,75,75,56)，因此本用例在护栏缺失时必然失败，
 * 不会因为「违反根本没发生」而空过。
 */
COMP_TEST(resample_bicubic_keeps_premultiplied_invariant) {
  Layer layer;
  layer.name = "premultiplied";
  layer.transform = comp_rect{0, 0, 9, 3};
  layer.sampling = COMP_SAMPLING_HIGH_QUALITY;
  layer.pixels.resize(3, 1, COMP_PIXEL_RGBA8);
  const std::vector<Rgba> source{Rgba{0, 0, 0, 0}, Rgba{96, 96, 96, 96}, Rgba{0, 0, 0, 255}};
  for (size_t i = 0; i < source.size(); ++i) {
    std::memcpy(layer.pixels.writable_pixels().data() + i * 4, source[i].data(), 4);
  }

  ImageBuffer unit;
  comp_rect bounds{};
  std::string error;
  COMP_CHECK_MSG(build_layer_unit(layer, SamplingMode::kHighQuality, &unit, &bounds, &error),
                 "build_layer_unit");
  COMP_CHECK(bounds.w == 9 && bounds.h == 3);
  COMP_CHECK(unit.width == 9 && unit.height == 3);

  const std::vector<uint8_t>& unit_px = unit.pixel_data();
  bool premultiplied = true;
  for (size_t i = 0; i + 3 < unit_px.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      if (unit_px[i + c] > unit_px[i + 3]) premultiplied = false;
    }
  }
  COMP_CHECK_MSG(premultiplied, "bicubic output must stay premultiplied (rgb <= alpha)");

  /* 源中心映射到输出中心，仍是逐字节精确的原像素。 */
  COMP_CHECK_MSG(matches(unit.pixel_data(), 9, 4, 1, Rgba{96, 96, 96, 96}, 0), "center pixel untouched");
  /*
   * 上冲列：颜色被夹到 alpha。加护栏前的实测值为 (32,32,32,23) 与 (75,75,75,56)——
   * 两列的 rgb 都超过各自的 alpha，正是本条断言要防的回归。
   */
  COMP_CHECK_MSG(matches(unit.pixel_data(), 9, 2, 1, Rgba{23, 23, 23, 23}, 0),
                 "overshoot clamped to alpha 23");
  COMP_CHECK_MSG(matches(unit.pixel_data(), 9, 3, 1, Rgba{56, 56, 56, 56}, 0),
                 "overshoot clamped to alpha 56");
  /* 未越界的像素一个字节都不能变（夹取对合法输出必须是空操作）。 */
  COMP_CHECK_MSG(matches(unit.pixel_data(), 9, 5, 1, Rgba{75, 75, 75, 150}, 0),
                 "legal pixel left untouched by the guard");
  COMP_CHECK_MSG(matches(unit.pixel_data(), 9, 0, 1, Rgba{0, 0, 0, 0}, 0), "transparent stays transparent");
}

COMP_TEST(resample_sampling_manifest_round_trip) {
  std::error_code ec;
  std::filesystem::create_directories("build", ec);
  const std::string project_path = "build/align_resample.comp";

  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 4, 4);

  const comp_layer_id layer = background_layer(document);
  RawImage image(4, 4, {kRed, kGreen, kBlue, kWhite, kRed, kGreen, kBlue, kWhite, kRed, kGreen,
                       kBlue, kWhite, kRed, kGreen, kBlue, kWhite});
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image.image) == COMP_OK);
  COMP_CHECK(comp_layer_set_sampling(document, layer, COMP_SAMPLING_SMOOTH) == COMP_OK);
  COMP_CHECK(comp_document_save(document, project_path.c_str()) == COMP_OK);

  /* manifest 键名护栏：sampling 必须是上游 rawValue "Smooth"。 */
  const std::string manifest_path = project_path + "/manifest.json";
  std::ifstream in(manifest_path, std::ios::binary);
  COMP_CHECK_MSG(in.good(), "manifest.json exists");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string manifest = buffer.str();
  COMP_CHECK_MSG(manifest.find("\"sampling\"") != std::string::npos, "manifest has sampling key");
  COMP_CHECK_MSG(manifest.find("Smooth") != std::string::npos, "manifest stores Smooth");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, project_path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);

  comp_layer_id loaded_ids[8] = {0};
  size_t count = 0;
  COMP_CHECK(comp_layer_list(loaded, loaded_ids, 8, &count) == COMP_OK);
  COMP_CHECK(count >= 1);
  comp_sampling read = COMP_SAMPLING_NEAREST;
  COMP_CHECK(comp_layer_get_sampling(loaded, loaded_ids[0], &read) == COMP_OK);
  COMP_CHECK_MSG(read == COMP_SAMPLING_SMOOTH, "sampling survives save/load");

  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(resample_sampling_rejects_invalid) {
  comp_engine_desc engine_desc = test_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  comp_document document = make_document(engine, 2, 2);

  const comp_layer_id layer = background_layer(document);
  COMP_CHECK(comp_layer_set_sampling(document, layer, static_cast<comp_sampling>(7)) ==
             COMP_ERR_INVALID_ARGUMENT);
  comp_sampling out = COMP_SAMPLING_NEAREST;
  COMP_CHECK(comp_layer_get_sampling(document, layer, &out) == COMP_OK);
  COMP_CHECK_MSG(out == COMP_SAMPLING_HIGH_QUALITY, "invalid set left default unchanged");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
