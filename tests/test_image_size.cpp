// AI生成
/*
 * test_image_size.cpp — Image Size 整篇重采样（对齐上游 ImageResizer.swift）。
 *
 * 覆盖：本地重采样（像素/掩版/几何）、光栅化路径（旋转层、拉伸层）、
 *       uniform 掩版保持、组几何缩放、一步撤销、无操作短路、非法尺寸拒绝、
 *       .comp 往返保真。
 *
 * 判据是端到端事实：合成结果的像素颜色、.comp manifest 里的 transform 尺寸、
 * save->load 往返后的保持——不是「API 返回 COMP_OK」。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
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

comp_engine_desc image_size_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-image-size-tests";
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

/* 上下双色的 4×4 图案：上半 red、下半 blue（旋转语义探针）。 */
struct HalvesImage {
  std::vector<uint8_t> bytes;
  comp_image image{};

  HalvesImage(int32_t width, int32_t height)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
        const bool top = y < height / 2;
        bytes[i] = top ? 255 : 0;
        bytes[i + 1] = 0;
        bytes[i + 2] = top ? 0 : 255;
        bytes[i + 3] = 255;
      }
    }
    image.width = width;
    image.height = height;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(width) * 4u;
    image.pixels = bytes.data();
  }
};

/* 左右双色的 4×4 掩版：左半 0（遮罩）、右半 255（显形）。 */
struct HalvesMask {
  std::vector<uint8_t> bytes;
  comp_mask mask{};

  HalvesMask(int32_t width, int32_t height)
      : bytes(static_cast<size_t>(width) * static_cast<size_t>(height)) {
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        bytes[static_cast<size_t>(y) * width + x] = x < width / 2 ? 0 : 255;
      }
    }
    mask.width = width;
    mask.height = height;
    mask.stride = static_cast<size_t>(width);
    mask.pixels = bytes.data();
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

comp_layer_id add_solid_layer(comp_document document, comp_rect transform, uint8_t r,
                              uint8_t g, uint8_t b) {
  comp_layer_desc desc{};
  desc.name = "solid";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = transform;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;
  const SolidImage image(transform.w, transform.h, r, g, b, 255);
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

comp_size document_size(comp_document document) {
  comp_size size{0, 0};
  if (comp_document_size(document, &size) != COMP_OK) return comp_size{0, 0};
  return size;
}

size_t undo_depth(comp_document document) {
  size_t depth = 0;
  if (comp_document_history_stats(document, &depth, nullptr) != COMP_OK) return 0;
  return depth;
}

std::string work_path(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/image_size", ec);
  return std::string("build/image_size/") + name;
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

/* manifest 是 pretty-print（键值带空格、数组多行），尺寸/布尔断言走正则。 */
bool has_json(const std::string& text, const char* pattern) {
  return std::regex_search(text, std::regex(pattern));
}

}  // namespace

/* ------------------------------------------------ 本地重采样：像素与几何同步缩放 */

COMP_TEST(image_size_doubles_layer_pixels_and_canvas) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 2×2 红层贴在 (1,1)：放大 2 倍后应为 4×4 贴在 (2,2)。 */
  COMP_CHECK(add_solid_layer(document, comp_rect{1, 1, 2, 2}, 255, 0, 0) != 0);

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);
  COMP_CHECK(document_size(document).width == 8);
  COMP_CHECK(document_size(document).height == 8);

  /* 实色块双线性/双三次放大都是精确的：放大后目标区仍为纯红。 */
  const std::vector<uint8_t> canvas = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(canvas, 8, 2, 2, 255, 0, 0, 255), "scaled origin lands at (2,2)");
  COMP_CHECK_MSG(matches(canvas, 8, 5, 5, 255, 0, 0, 255), "scaled size covers 4x4");
  COMP_CHECK_MSG(matches(canvas, 8, 1, 1, 0, 0, 0, 0), "outside the layer stays transparent");

  /* manifest 里的 transform 尺寸同步缩放（读取侧事实，不是内部状态）。 */
  const std::string root = work_path("double.comp");
  COMP_CHECK(comp_io_save_project(document, root.c_str()) == COMP_OK);
  const std::string manifest = read_manifest(root);
  COMP_CHECK_MSG(has_json(manifest, "\"size\": \\[\\s*4,\\s*4\\s*\\]"),
                 "manifest records the scaled transform");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_shrink_scales_layers_down) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 4×4 红层贴在 (2,2)：缩半后应为 2×2 贴在 (1,1)。 */
  COMP_CHECK(add_solid_layer(document, comp_rect{2, 2, 4, 4}, 255, 0, 0) != 0);

  COMP_CHECK(comp_document_resize_image(document, comp_size{4, 4}) == COMP_OK);
  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 1, 1, 255, 0, 0, 255), "shrunk origin lands at (1,1)");
  COMP_CHECK_MSG(matches(canvas, 4, 2, 2, 255, 0, 0, 255), "shrunk layer still covers 2x2");
  COMP_CHECK_MSG(matches(canvas, 4, 0, 0, 0, 0, 0, 0), "outside the shrunk layer is transparent");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------ 光栅化路径：旋转层与拉伸层 */

COMP_TEST(image_size_rasterizes_rotated_layer) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 上下双色层顺时针旋转 90°：上边转到右边 -> 右半红、左半蓝。 */
  comp_layer_desc layer_desc{};
  layer_desc.name = "halves";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, 4, 4};
  layer_desc.rotation = 90.0f;
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);
  const HalvesImage halves(4, 4);
  COMP_CHECK(comp_layer_set_pixels(document, layer, &halves.image) == COMP_OK);

  /* 先钉住旋转语义本身：放大前的合成就必须是右红左蓝。 */
  const std::vector<uint8_t> before = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(before, 4, 3, 1, 255, 0, 0, 255), "clockwise 90 puts red on the right");
  COMP_CHECK_MSG(matches(before, 4, 0, 2, 0, 0, 255, 255), "clockwise 90 puts blue on the left");

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);

  /* 光栅化 + 整幅缩放保持同一几何关系：放大后依然右红左蓝。 */
  const std::vector<uint8_t> after = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(after, 8, 6, 2, 255, 0, 0, 255), "rasterized+scaled keeps red on the right");
  COMP_CHECK_MSG(matches(after, 8, 1, 5, 0, 0, 255, 255), "rasterized+scaled keeps blue on the left");

  /* 旋转被烘入像素后丢弃（上游同策略）：manifest 里 rotation 归 0。 */
  const std::string root = work_path("rotated.comp");
  COMP_CHECK(comp_io_save_project(document, root.c_str()) == COMP_OK);
  const std::string manifest = read_manifest(root);
  COMP_CHECK_MSG(has_json(manifest, "\"rotation\": 0"),
                 "rotation is baked away after rasterize");
  COMP_CHECK(!contains(manifest, "\"rotation\": 90"));

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_keeps_stretched_layer_filling_canvas) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 2×2 像素被 transform 拉伸铺满 4×4 画布（尺寸不一致 = 拉伸层）。 */
  comp_layer_desc layer_desc{};
  layer_desc.name = "stretched";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, 4, 4};
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &layer) == COMP_OK);
  const SolidImage pixels(2, 2, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, layer, &pixels.image) == COMP_OK);

  const std::vector<uint8_t> before = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(before, 4, 3, 3, 0, 255, 0, 255), "stretched layer fills the canvas");

  /* 放大 2 倍：光栅化路径保证「铺满」不变量跟随画布，而不是把像素缩成 4×4。 */
  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);
  const std::vector<uint8_t> after = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(after, 8, 7, 7, 0, 255, 0, 255), "still fills the enlarged canvas");
  COMP_CHECK_MSG(matches(after, 8, 0, 0, 0, 255, 0, 255), "top-left corner of the fill");

  const std::string root = work_path("stretched.comp");
  COMP_CHECK(comp_io_save_project(document, root.c_str()) == COMP_OK);
  COMP_CHECK_MSG(has_json(read_manifest(root), "\"size\": \\[\\s*8,\\s*8\\s*\\]"),
                 "rasterized layer transform matches the new canvas");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------ 掩版：uniform 保持 / 非 uniform 重采样 */

COMP_TEST(image_size_keeps_uniform_mask) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = add_solid_layer(document, comp_rect{0, 0, 4, 4}, 255, 0, 0);
  COMP_CHECK(layer != 0);
  /* 1×1 uniform 掩版（半显）：分辨率无关，缩放后必须保持原值。 */
  SolidMask uniform(1, 1, 128);
  COMP_CHECK(comp_layer_set_mask(document, layer, &uniform.mask) == COMP_OK);

  const std::vector<uint8_t> before = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(before, 4, 2, 2, 128, 0, 0, 128), "half-revealed before resize");

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);
  const std::vector<uint8_t> after = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(after, 8, 4, 4, 128, 0, 0, 128),
                 "uniform mask stays half-revealed after resize");
  COMP_CHECK_MSG(matches(after, 8, 0, 0, 128, 0, 0, 128), "uniform mask covers the whole layer");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_resamples_layer_mask) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  const comp_layer_id layer = add_solid_layer(document, comp_rect{0, 0, 4, 4}, 255, 0, 0);
  COMP_CHECK(layer != 0);
  const HalvesMask halves(4, 4);
  COMP_CHECK(comp_layer_set_mask(document, layer, &halves.mask) == COMP_OK);

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);

  /* 掩版随层重采样：左半仍遮罩、右半仍显形（纯色区内双线性精确）。 */
  const std::vector<uint8_t> canvas = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(canvas, 8, 1, 4, 0, 0, 0, 0), "masked left half stays hidden");
  COMP_CHECK_MSG(matches(canvas, 8, 6, 4, 255, 0, 0, 255), "unmasked right half stays red");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ------------------------------------------------ 组几何 / 撤销 / 短路 / 拒绝 / 往返 */

COMP_TEST(image_size_scales_group_geometry) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  comp_layer_desc group_desc{};
  group_desc.name = "G";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  group_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  comp_layer_desc child_desc{};
  child_desc.name = "child";
  child_desc.blend = COMP_BLEND_NORMAL;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 2, 2};
  child_desc.parent = group;
  child_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id child = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);
  const SolidImage red(2, 2, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, child, &red.image) == COMP_OK);

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);

  /* 组与子层几何同步缩放：子层像素 4×4 铺在 (0,0)，组 8×8。 */
  const std::vector<uint8_t> canvas = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(canvas, 8, 3, 3, 255, 0, 0, 255), "child pixels scaled to 4x4");
  COMP_CHECK_MSG(matches(canvas, 8, 4, 4, 0, 0, 0, 0), "child still only covers its own rect");

  const std::string root = work_path("group.comp");
  COMP_CHECK(comp_io_save_project(document, root.c_str()) == COMP_OK);
  const std::string manifest = read_manifest(root);
  COMP_CHECK_MSG(has_json(manifest, "\"isGroup\": true"), "group survives the resize");
  COMP_CHECK_MSG(has_json(manifest, "\"size\": \\[\\s*8,\\s*8\\s*\\]"),
                 "group transform scaled to the canvas");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_is_one_undo_step) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_solid_layer(document, comp_rect{0, 0, 4, 4}, 255, 0, 0) != 0);
  const size_t before = undo_depth(document);

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);
  COMP_CHECK_MSG(undo_depth(document) == before + 1, "one resize = one history entry");

  COMP_CHECK(comp_document_undo(document) == COMP_OK);
  COMP_CHECK(document_size(document).width == 4);
  const std::vector<uint8_t> restored = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(restored, 4, 2, 2, 255, 0, 0, 255), "undo restores the old geometry");

  COMP_CHECK(comp_document_redo(document) == COMP_OK);
  COMP_CHECK(document_size(document).width == 8);
  const std::vector<uint8_t> redone = flatten(document, 8, 8);
  COMP_CHECK_MSG(matches(redone, 8, 5, 5, 255, 0, 0, 255), "redo re-applies the scaled geometry");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_no_op_when_size_unchanged) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 尺寸不变：成功但是无操作，不进历史（对齐上游短路）。 */
  COMP_CHECK(comp_document_resize_image(document, comp_size{4, 4}) == COMP_OK);
  COMP_CHECK_MSG(undo_depth(document) == 0, "no-op does not touch the history stack");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_rejects_invalid_size) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_solid_layer(document, comp_rect{0, 0, 4, 4}, 255, 0, 0) != 0);
  const size_t before = undo_depth(document);

  /* 零宽 / 超长边（.comp 规范界 30000）/ 超总像素：一律拒绝且文档不变。 */
  COMP_CHECK(comp_document_resize_image(document, comp_size{0, 8}) != COMP_OK);
  COMP_CHECK(comp_document_resize_image(document, comp_size{30001, 8}) != COMP_OK);
  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 0}) != COMP_OK);
  COMP_CHECK(document_size(document).width == 4);
  COMP_CHECK_MSG(undo_depth(document) == before, "rejected resize leaves no history entry");
  const std::vector<uint8_t> canvas = flatten(document, 4, 4);
  COMP_CHECK_MSG(matches(canvas, 4, 2, 2, 255, 0, 0, 255), "rejected resize changes nothing");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(image_size_survives_save_load_roundtrip) {
  comp_engine_desc desc = image_size_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc document_desc{};
  document_desc.width = 4;
  document_desc.height = 4;
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(add_solid_layer(document, comp_rect{1, 1, 2, 2}, 255, 0, 0) != 0);

  COMP_CHECK(comp_document_resize_image(document, comp_size{8, 8}) == COMP_OK);

  const std::string root = work_path("roundtrip.comp");
  COMP_CHECK(comp_io_save_project(document, root.c_str()) == COMP_OK);

  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, root.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(document_size(loaded).width == 8);
  COMP_CHECK(document_size(loaded).height == 8);

  /* 往返后合成等价：像素资产按新尺寸落盘并读回。 */
  const std::vector<uint8_t> canvas = flatten(loaded, 8, 8);
  COMP_CHECK_MSG(matches(canvas, 8, 3, 3, 255, 0, 0, 255), "round-tripped layer keeps its placement");
  COMP_CHECK_MSG(matches(canvas, 8, 0, 0, 0, 0, 0, 0), "round-tripped layer keeps its bounds");

  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
