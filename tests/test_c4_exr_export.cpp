// AI生成
/*
 * test_c4_exr_export.cpp — C4 验收：**合成结果**导出 EXR。
 *
 * 与 C3b 的资产级 `<uuid>.exr` 读写器不同，这里钉的是 `comp_io_export(EXR)`：
 *   - 导出必须拿**合成表面本身**（有 HDR 图层时是 half），不能先降到 8 位；
 *     先降位再写 EXR 会把白点以上的光截掉——文件写得出来、内容是错的。
 *   - 文件口径与 `.comp` 资产一致：**线性光、直通 alpha**，写 HALF 通道。
 *   - 8 位文档也能导出（CPU 后端用 rgba8_to_hdr 扩成 half），不另开一条路径。
 *   - 能力登记表必须如实说「可写」，失败原因仍由登记表生成。
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "format/codec_lib.h"
#include "format/exr_codec.h"
#include "test_framework.h"

namespace {

comp_engine_desc c4_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-c4-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

std::string c4_file(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/c4", ec);
  return std::string("build/c4/") + name;
}

std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/* 独立于被测实现的 extended sRGB 解码（期望值不能来自 deep_linear）。 */
float ref_linear(float encoded) {
  const float magnitude = std::fabs(encoded);
  const float linear = magnitude <= 0.04045f
                           ? magnitude / 12.92f
                           : std::pow((magnitude + 0.055f) / 1.055f, 2.4f);
  return encoded < 0.0f ? -linear : linear;
}

bool add_layer(comp_document doc, const char* name, int32_t w, int32_t h, comp_layer_id* out_id) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = comp_rect{0, 0, w, h};
  return comp_layer_add(doc, &desc, out_id) == COMP_OK;
}

}  // namespace

/* 1. 登记表如实说 EXR 可写；Radiance 仍只读。 */
COMP_TEST(c4_exr_export_is_advertised_writable) {
  comp_codec_state state = COMP_CODEC_UNAVAILABLE;
  char note[512] = {0};
  size_t note_size = 0;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_EXR, &state, note, sizeof(note), &note_size) ==
             COMP_OK);
  const std::string exr_note(note);
  COMP_CHECK_MSG(state == COMP_CODEC_BUILTIN, "EXR is the in-tree codec, not a vendored library");
  COMP_CHECK_MSG(exr_note.find("export writes scanline NONE + HALF") != std::string::npos,
                 "the registry must advertise the EXR write path");
  COMP_CHECK_MSG(exr_note.find("export unsupported") == std::string::npos,
                 "EXR export is no longer unsupported");

  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_HDR, &state, note, sizeof(note), &note_size) ==
             COMP_OK);
  COMP_CHECK_MSG(std::string(note).find("export unsupported") != std::string::npos,
                 "Radiance stays read-only in this build");
}

/* 2. HDR 合成结果导出 EXR：白点以上的光仍是线性光下 > 1 的值。 */
COMP_TEST(c4_exr_export_preserves_hdr_highlights) {
  comp_engine engine = nullptr;
  comp_engine_desc desc = c4_engine_desc();
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc doc_desc{};
  doc_desc.width = 2;
  doc_desc.height = 1;
  doc_desc.dpi = 72.0f;
  comp_document doc = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);

  comp_layer_id layer = 0;
  COMP_CHECK(add_layer(doc, "hdr", 2, 1, &layer));

  std::vector<uint16_t> pixels(2u * 4u, 0);
  /*
   * 像素 0：预乘 2.0、alpha 1（直通 2.0 的高光）；
   * 像素 1：预乘 0.25/0.5/1.0、alpha 0.5 —— 直通色是 0.5/1.0/2.0，
   * 因此它同时检验「除 alpha」与「再走 sRGB 曲线」，而不是只检验曲线。
   */
  const uint16_t high[4] = {0x4000, 0x4000, 0x4000, 0x3c00};
  const uint16_t mid[4] = {0x3400, 0x3800, 0x3c00, 0x3800};
  std::memcpy(pixels.data(), high, sizeof(high));
  std::memcpy(pixels.data() + 4u, mid, sizeof(mid));

  comp_image image{};
  image.width = 2;
  image.height = 1;
  image.format = COMP_PIXEL_RGBA16F;
  image.stride = 2u * 8u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, layer, &image) == COMP_OK);

  const std::string path = c4_file("c4_hdr_export.exr");
  COMP_CHECK_MSG(comp_io_export(doc, path.c_str(), COMP_FORMAT_EXR, nullptr) == COMP_OK,
                 "exporting a document with an HDR layer to EXR must succeed");

  const std::vector<uint8_t> bytes = read_bytes(path);
  COMP_CHECK_MSG(!bytes.empty(), "the exported EXR must exist and be non-empty");

  compositor::CodecFloatImage decoded;
  std::string error;
  COMP_CHECK(compositor::exr_decode_float(bytes.data(), bytes.size(), &decoded, &error));
  COMP_CHECK(decoded.width == 2 && decoded.height == 1);
  if (decoded.width == 2 && decoded.height == 1) {
    const float* p0 = decoded.rgba.data();
    const float* p1 = decoded.rgba.data() + 4u;
    /*
     * 期望值由测试内独立公式给出：2.0 的 extended sRGB 线性化 ≈ 4.95。
     * 关键断言是 > 1：若导出先落到 8 位栅格，2.0 会被截到白，线性值只会到 1。
     */
    const float want_high = ref_linear(2.0f);
    COMP_CHECK_MSG(std::fabs(p0[0] - want_high) < 1e-3f,
                   "a premultiplied 2.0 reaches the EXR as linear light, not clipped to white");
    COMP_CHECK_MSG(p0[0] > 1.0f, "the exported highlight must stay above linear white");
    COMP_CHECK_MSG(std::fabs(p0[3] - 1.0f) < 1e-4f, "alpha stays 1.0");
    const float want_mid_r = ref_linear(0.5f);
    const float want_mid_g = ref_linear(1.0f);
    const float want_mid_b = ref_linear(2.0f);
    COMP_CHECK_MSG(std::fabs(p1[0] - want_mid_r) < 1e-3f &&
                       std::fabs(p1[1] - want_mid_g) < 1e-3f,
                   "the EXR divides alpha out before the sRGB curve (straight-alpha linear light)");
    COMP_CHECK_MSG(std::fabs(p1[2] - want_mid_b) < 1e-3f && p1[2] > 1.0f,
                   "a half-alpha highlight stays above linear white after un-premultiplying");
    COMP_CHECK_MSG(std::fabs(p1[3] - 0.5f) < 1e-4f, "the second pixel keeps its own alpha");
  }

  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}

/* 3. 8 位文档也能导出：后端把 8 位合成扩成 half，颜色曲线只施加一次。 */
COMP_TEST(c4_exr_export_of_an_eight_bit_document) {
  comp_engine engine = nullptr;
  comp_engine_desc desc = c4_engine_desc();
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc doc_desc{};
  doc_desc.width = 2;
  doc_desc.height = 1;
  doc_desc.dpi = 72.0f;
  comp_document doc = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);

  comp_layer_id layer = 0;
  COMP_CHECK(add_layer(doc, "eight", 2, 1, &layer));

  /* 预乘不透明色 (100,150,200)，第二像素全透明。 */
  std::vector<uint8_t> pixels = {100, 150, 200, 255, 0, 0, 0, 0};
  comp_image image{};
  image.width = 2;
  image.height = 1;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = 2u * 4u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, layer, &image) == COMP_OK);

  const std::string path = c4_file("c4_eight_export.exr");
  COMP_CHECK_MSG(comp_io_export(doc, path.c_str(), COMP_FORMAT_EXR, nullptr) == COMP_OK,
                 "an 8-bit document must export to EXR through the same path");

  const std::vector<uint8_t> bytes = read_bytes(path);
  compositor::CodecFloatImage decoded;
  std::string error;
  COMP_CHECK(compositor::exr_decode_float(bytes.data(), bytes.size(), &decoded, &error));
  if (decoded.width == 2 && decoded.height == 1) {
    const float* p0 = decoded.rgba.data();
    const float* p1 = decoded.rgba.data() + 4u;
    COMP_CHECK_MSG(std::fabs(p0[0] - ref_linear(100.0f / 255.0f)) < 1e-3f &&
                       std::fabs(p0[1] - ref_linear(150.0f / 255.0f)) < 1e-3f &&
                       std::fabs(p0[2] - ref_linear(200.0f / 255.0f)) < 1e-3f,
                   "8-bit sRGB values become linear light in the EXR");
    COMP_CHECK_MSG(std::fabs(p1[3]) < 1e-6f && std::fabs(p1[0]) < 1e-6f,
                   "a transparent pixel exports as zero alpha with no colour");
  }

  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}

/* 4. 失败路径仍由登记表给出具名原因（不能悄悄写一份 8 位内容冒充 EXR）。 */
COMP_TEST(c4_exr_export_failure_names_the_supported_formats) {
  comp_engine engine = nullptr;
  comp_engine_desc desc = c4_engine_desc();
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc doc_desc{};
  doc_desc.width = 1;
  doc_desc.height = 1;
  doc_desc.dpi = 72.0f;
  comp_document doc = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);

  const std::string path = c4_file("c4_unsupported.tiff");
  const comp_result result = comp_io_export(doc, path.c_str(), COMP_FORMAT_TIFF, nullptr);
  COMP_CHECK_MSG(result == COMP_ERR_UNSUPPORTED,
                 "TIFF export stays UNSUPPORTED instead of writing a mislabeled file");
  COMP_CHECK_MSG(std::string(comp_engine_last_error(engine)).find("TIFF") != std::string::npos,
                 "the failure names the format");

  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}

/* 5. 写的是**合成结果**，不是图层资产：图层只盖住右半，左半必须是透明黑。 */
COMP_TEST(c4_exr_export_writes_the_composite_not_the_layer_asset) {
  comp_engine engine = nullptr;
  comp_engine_desc desc = c4_engine_desc();
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc doc_desc{};
  doc_desc.width = 2;
  doc_desc.height = 1;
  doc_desc.dpi = 72.0f;
  comp_document doc = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);

  comp_layer_desc layer_desc{};
  layer_desc.name = "right";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{1, 0, 1, 1};
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(doc, &layer_desc, &layer) == COMP_OK);

  /* 1x1 不透明白（half 预乘 1.0）。 */
  std::vector<uint16_t> pixels(4u, 0x3c00);
  comp_image image{};
  image.width = 1;
  image.height = 1;
  image.format = COMP_PIXEL_RGBA16F;
  image.stride = 8u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(doc, layer, &image) == COMP_OK);

  const std::string path = c4_file("c4_composite_only.exr");
  COMP_CHECK(comp_io_export(doc, path.c_str(), COMP_FORMAT_EXR, nullptr) == COMP_OK);

  const std::vector<uint8_t> bytes = read_bytes(path);
  compositor::CodecFloatImage decoded;
  std::string error;
  COMP_CHECK(compositor::exr_decode_float(bytes.data(), bytes.size(), &decoded, &error));
  if (decoded.width == 2 && decoded.height == 1) {
    const float* left = decoded.rgba.data();
    const float* right = decoded.rgba.data() + 4u;
    COMP_CHECK_MSG(std::fabs(left[0]) < 1e-6f && std::fabs(left[3]) < 1e-6f,
                   "the uncovered left pixel is transparent black, not the layer asset");
    COMP_CHECK_MSG(std::fabs(right[0] - ref_linear(1.0f)) < 1e-3f &&
                       std::fabs(right[3] - 1.0f) < 1e-4f,
                   "the covered right pixel carries the composited linear colour");
  }

  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}

/* 6. 空文档导出仍是合法 EXR：全透明黑，不是未初始化内存。 */
COMP_TEST(c4_exr_export_of_an_empty_document_is_transparent_black) {
  comp_engine engine = nullptr;
  comp_engine_desc desc = c4_engine_desc();
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document_desc doc_desc{};
  doc_desc.width = 2;
  doc_desc.height = 1;
  doc_desc.dpi = 72.0f;
  comp_document doc = nullptr;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);

  const std::string path = c4_file("c4_empty.exr");
  COMP_CHECK_MSG(comp_io_export(doc, path.c_str(), COMP_FORMAT_EXR, nullptr) == COMP_OK,
                 "a document with no layers still exports");

  const std::vector<uint8_t> bytes = read_bytes(path);
  compositor::CodecFloatImage decoded;
  std::string error;
  COMP_CHECK(compositor::exr_decode_float(bytes.data(), bytes.size(), &decoded, &error));
  if (decoded.width == 2 && decoded.height == 1) {
    bool all_zero = true;
    for (float value : decoded.rgba) {
      if (std::fabs(value) > 1e-6f) all_zero = false;
    }
    COMP_CHECK_MSG(all_zero, "an empty composite is all-zero RGBA, including alpha");
  }

  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}
