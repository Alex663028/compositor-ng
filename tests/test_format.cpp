// AI生成
/*
 * test_format.cpp — .comp 格式层：PNG 编解码、UUID、manifest 上游互通护栏。
 *
 * 这一组测试守的是「.comp 能否被上游 macOS 版直接打开」这条硬约束：
 *   1. 像素资产从无压缩 .rgba 改为 PNG 后，编解码必须无损（预乘约定往返）；
 *   2. 解码器必须能读「真实 deflate 压缩」的 PNG（上游 CoreGraphics 产物），
 *      而不只是自家产出的存储块 PNG；
 *   3. 图层 id / documentID 必须是合法大写 UUID，且 documentID 跨多次保存稳定；
 *   4. manifest 的键名、形状、取值域逐条对齐上游 ProjectStore.swift——
 *      尤其 transform 的六键形态与组层 blendMode 强制为 Normal。
 */
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
#include "format/comp_file.h"
#include "format/comp_json.h"
#include "format/comp_png.h"
#include "format/comp_uuid.h"
#include "test_framework.h"

namespace {

using compositor::JsonValue;
using compositor::PngImage;
using compositor::json_parse;
using compositor::png_decode_rgba8;
using compositor::png_encode_gray8;
using compositor::png_encode_rgba8;
using compositor::uuid_derive;
using compositor::uuid_from_string;
using compositor::uuid_is_wellformed;
using compositor::uuid_to_string;
using compositor::uuid_v4_new;

int abs_i(int a, int b) { return a > b ? a - b : b - a; }

/* 比较解码结果中第 index 个像素是否为 (r,g,b,a)（按容差）。 */
bool near_px(const std::vector<uint8_t>& v, size_t index, int r, int g, int b, int a, int tol) {
  const size_t i = index * 4u;
  if (i + 3 >= v.size()) return false;
  return abs_i(v[i + 0], r) <= tol && abs_i(v[i + 1], g) <= tol &&
         abs_i(v[i + 2], b) <= tol && abs_i(v[i + 3], a) <= tol;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
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

comp_engine make_engine() {
  comp_engine_desc desc{};
  desc.app_name = "format-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  return engine;
}

comp_document make_document(comp_engine engine, int32_t width, int32_t height, float dpi) {
  comp_document_desc desc{};
  desc.width = width;
  desc.height = height;
  desc.dpi = dpi;
  desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &desc, &document) == COMP_OK);
  return document;
}

comp_layer_id first_layer(comp_document document) {
  comp_layer_id ids[8] = {0};
  size_t count = 0;
  comp_layer_list(document, ids, 8, &count);
  return count > 0 ? ids[0] : 0;
}

/* 外部（Python zlib，真实 deflate 压缩）产生的 2x2 RGBA PNG。
 * 直通像素：(255,0,0,255) (0,255,0,255) / (0,0,255,255) (200,100,50,100)。
 * 解码器须预乘：最后一像素期望 (78,39,20,100)（容差 2）。 */
const unsigned char kExternalPngRgba[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x15, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0xf4, 0xff, 0x13, 0x29, 0x46, 0x29, 0x00, 0x44, 0xe6,
    0x07, 0xbd, 0xcc, 0x43, 0xf7, 0xbb, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

/* 外部产生的 3x2 灰度 PNG（色彩类型 0）：样本 0,64,128 / 192,255,77。 */
const unsigned char kExternalPngGray[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x1f, 0x39, 0xc6, 0x00, 0x00, 0x00,
    0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x70, 0x68, 0x60,
    0x38, 0xf0, 0xdf, 0x17, 0x00, 0x08, 0x93, 0x02, 0xcd, 0x24, 0x78, 0x28,
    0x38, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82};

}  // namespace

COMP_TEST(format_png_rgba_round_trip) {
  const int32_t width = 3;
  const int32_t height = 2;
  /* 预乘 RGBA8：每个通道都必须 <= alpha。 */
  const uint8_t src_pixels[6][4] = {
      {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
      {128, 0, 0, 128}, {60, 30, 15, 60}, {255, 255, 255, 255}};
  std::vector<uint8_t> src;
  for (const auto& px : src_pixels) {
    src.insert(src.end(), px, px + 4);
  }

  std::vector<uint8_t> png;
  std::string error;
  COMP_CHECK_MSG(png_encode_rgba8(src.data(), width, height, &png, &error), "encode rgba8");

  /* 产物必须是合法 PNG（签名 + IHDR 色彩类型 6）。 */
  COMP_CHECK(png.size() > 33);
  COMP_CHECK(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
  COMP_CHECK(png[12] == 'I' && png[13] == 'H' && png[14] == 'D' && png[15] == 'R');
  COMP_CHECK(png[25] == 6); /* 色彩类型：真彩 + Alpha */

  PngImage decoded;
  COMP_CHECK_MSG(png_decode_rgba8(png.data(), png.size(), &decoded, &error), "decode rgba8");
  COMP_CHECK(decoded.width == width && decoded.height == height);
  COMP_CHECK(decoded.rgba.size() == src.size());
  /* 预乘 -> 直通 -> 预乘 的往返：不透明像素逐字节相等，半透明允许 2 级舍入。 */
  for (size_t i = 0; i < 6; ++i) {
    COMP_CHECK_MSG(near_px(decoded.rgba, i, src_pixels[i][0], src_pixels[i][1], src_pixels[i][2],
                           src_pixels[i][3], 2),
                   "rgba round trip pixel");
  }
}

COMP_TEST(format_png_gray_round_trip) {
  const int32_t width = 3;
  const int32_t height = 2;
  const uint8_t gray[6] = {0, 64, 128, 192, 255, 77};

  std::vector<uint8_t> png;
  std::string error;
  COMP_CHECK_MSG(png_encode_gray8(gray, width, height, &png, &error), "encode gray8");
  COMP_CHECK(png[25] == 0); /* 色彩类型 0：灰度 */

  PngImage decoded;
  COMP_CHECK_MSG(png_decode_rgba8(png.data(), png.size(), &decoded, &error), "decode gray8");
  COMP_CHECK(decoded.width == width && decoded.height == height);
  /* 灰度像素不透明，预乘是恒等变换：三通道相等且 alpha = 255。 */
  for (size_t i = 0; i < 6; ++i) {
    COMP_CHECK(near_px(decoded.rgba, i, gray[i], gray[i], gray[i], 255, 0));
  }
}

COMP_TEST(format_png_decodes_externally_produced_files) {
  /* 真实 deflate（固定/动态 Huffman）——覆盖自家「存储块」编码器之外的路径。 */
  PngImage rgba;
  std::string error;
  COMP_CHECK_MSG(png_decode_rgba8(kExternalPngRgba, sizeof(kExternalPngRgba), &rgba, &error),
                 "decode external rgba png");
  COMP_CHECK(rgba.width == 2 && rgba.height == 2);
  COMP_CHECK(near_px(rgba.rgba, 0, 255, 0, 0, 255, 0));
  COMP_CHECK(near_px(rgba.rgba, 1, 0, 255, 0, 255, 0));
  COMP_CHECK(near_px(rgba.rgba, 2, 0, 0, 255, 255, 0));
  COMP_CHECK_MSG(near_px(rgba.rgba, 3, 78, 39, 20, 100, 2), "premultiplied semi-transparent pixel");

  PngImage gray;
  COMP_CHECK_MSG(png_decode_rgba8(kExternalPngGray, sizeof(kExternalPngGray), &gray, &error),
                 "decode external gray png");
  COMP_CHECK(gray.width == 3 && gray.height == 2);
  const uint8_t expect[6] = {0, 64, 128, 192, 255, 77};
  for (size_t i = 0; i < 6; ++i) {
    COMP_CHECK(near_px(gray.rgba, i, expect[i], expect[i], expect[i], 255, 0));
  }
}

COMP_TEST(format_png_rejects_corrupt_input) {
  PngImage image;
  std::string error;
  /* 非 PNG 签名：必须显式失败，不能返回看似成功的空图。 */
  const unsigned char garbage[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  COMP_CHECK(!png_decode_rgba8(garbage, sizeof(garbage), &image, &error));
  /* 截断的合法 PNG：CRC / 长度校验须拦住。 */
  COMP_CHECK(!png_decode_rgba8(kExternalPngRgba, 20, &image, &error));

  std::vector<uint8_t> png;
  COMP_CHECK(!png_encode_rgba8(nullptr, 2, 2, &png, &error));
  COMP_CHECK(!png_encode_rgba8(garbage, -1, 2, &png, &error));
}

COMP_TEST(format_uuid_text_and_derive) {
  const std::string sample = "E621E1F8-C36C-495A-93FC-0C247A3E6E5F";
  COMP_CHECK(uuid_is_wellformed(sample));

  uint8_t bytes[16];
  COMP_CHECK(uuid_from_string(sample, bytes));
  COMP_CHECK(uuid_to_string(bytes) == sample);

  /* 大小写不敏感，规范化输出为大写连字符形式。 */
  COMP_CHECK(uuid_from_string("e621e1f8-c36c-495a-93fc-0c247a3e6e5f", bytes));
  COMP_CHECK(uuid_to_string(bytes) == sample);

  /* 非 UUID 文本一律拒绝（上游 id 字段必须是 UUID）。 */
  COMP_CHECK(!uuid_is_wellformed("not-a-uuid"));
  COMP_CHECK(!uuid_is_wellformed("E621E1F8C36C495A93FC0C247A3E6E5F"));
  COMP_CHECK(!uuid_is_wellformed(""));
  COMP_CHECK(!uuid_is_wellformed("E621E1F8-C36C-495A-93FC-0C247A3E6E5"));

  /* v4 生成：版本位第 3 组首字符为 '4'，变体位首位为 8/9/A/B。 */
  const std::string v4 = compositor::uuid_v4_new();
  COMP_CHECK(uuid_is_wellformed(v4));
  COMP_CHECK(v4[14] == '4');
  COMP_CHECK(v4[19] == '8' || v4[19] == '9' || v4[19] == 'A' || v4[19] == 'B');
  COMP_CHECK(compositor::uuid_v4_new() != v4);

  /* 派生稳定且区分：同一 (document, key) 恒定，不同 key 不碰撞。 */
  const std::string doc = "E621E1F8-C36C-495A-93FC-0C247A3E6E5F";
  const std::string id1a = compositor::uuid_derive(doc, 1);
  const std::string id1b = compositor::uuid_derive(doc, 1);
  const std::string id2 = compositor::uuid_derive(doc, 2);
  COMP_CHECK(id1a == id1b);
  COMP_CHECK(id1a != id2);
  COMP_CHECK(uuid_is_wellformed(id1a) && uuid_is_wellformed(id2));
  COMP_CHECK(compositor::uuid_derive("00000000-0000-0000-0000-000000000000", 1) != id1a);
}

COMP_TEST(format_comp_manifest_matches_upstream_schema) {
  std::error_code ec;
  std::filesystem::create_directories("build", ec);
  const std::string path = "build/format_manifest.comp";

  comp_engine engine = make_engine();
  comp_document document = make_document(engine, 8, 6, 300.0f);

  const comp_layer_id background = first_layer(document);
  COMP_CHECK(background != 0);
  const SolidImage red(8, 6, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  /* 组层带非 Normal 混合：落盘必须被强制为 Normal（上游组是穿透的）。 */
  comp_layer_desc group_desc{};
  group_desc.name = "Group";
  group_desc.blend = COMP_BLEND_MULTIPLY;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 8, 6};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  /* 半透明像素层：Screen + 0.5 不透明度。 */
  comp_layer_desc layer_desc{};
  layer_desc.name = "Half";
  layer_desc.blend = COMP_BLEND_SCREEN;
  layer_desc.opacity = 0.5f;
  layer_desc.transform = comp_rect{0, 0, 8, 6};
  comp_layer_id half = 0;
  COMP_CHECK(comp_layer_add(document, &layer_desc, &half) == COMP_OK);
  const SolidImage half_pixels(8, 6, 128, 0, 0, 128);
  COMP_CHECK(comp_layer_set_pixels(document, half, &half_pixels.image) == COMP_OK);

  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);

  /* 像素资产必须落在 images/<uuid>.png，且不得残留 .rgba。 */
  const std::string manifest_text = read_text(path + "/manifest.json");
  COMP_CHECK(!manifest_text.empty());
  COMP_CHECK_MSG(manifest_text.find(".rgba") == std::string::npos, "no legacy .rgba keys");
  COMP_CHECK_MSG(std::filesystem::exists(path + "/images"), "images/ directory exists");

  std::string parse_error;
  const JsonValue manifest = compositor::json_parse(manifest_text, &parse_error);
  COMP_CHECK(manifest.is_object());
  COMP_CHECK_MSG(manifest.string_or("format", "") == "com.compositor.project", "format id");
  COMP_CHECK(manifest.integer_or("version", 0) == 13);
  COMP_CHECK(manifest.string_or("colorSpace", "") == "sRGB");
  COMP_CHECK_MSG(manifest.number_or("resolution", 0.0) > 299.9 &&
                     manifest.number_or("resolution", 0.0) < 300.1,
                 "resolution round trip");
  COMP_CHECK(manifest.integer_or("width", 0) == 8);
  COMP_CHECK(manifest.integer_or("height", 0) == 6);
  const std::string document_id = manifest.string_or("documentID", "");
  COMP_CHECK_MSG(uuid_is_wellformed(document_id), "documentID is a UUID");
  COMP_CHECK(uuid_is_wellformed(manifest.string_or("activeLayerID", "")));

  const JsonValue* layers = manifest.find("layers");
  COMP_CHECK(layers != nullptr && layers->is_array());
  COMP_CHECK(layers->as_array().size() == 3);

  bool saw_group = false;
  bool saw_half = false;
  size_t saw_pixels = 0;
  for (const JsonValue& entry : layers->as_array()) {
    COMP_CHECK(entry.is_object());
    const std::string id = entry.string_or("id", "");
    COMP_CHECK_MSG(uuid_is_wellformed(id), "layer id is a UUID");
    const std::string name = entry.string_or("name", "");
    COMP_CHECK_MSG(!name.empty(), "layer name is non-empty");

    /* transform 必须是上游 LayerTransform 的六键形态。 */
    const JsonValue* transform = entry.find("transform");
    COMP_CHECK(transform != nullptr && transform->is_object());
    const JsonValue* origin = transform->find("origin");
    const JsonValue* size = transform->find("size");
    COMP_CHECK(origin != nullptr && origin->is_array() && origin->as_array().size() == 2);
    COMP_CHECK(size != nullptr && size->is_array() && size->as_array().size() == 2);
    COMP_CHECK(transform->find("rotation") != nullptr);
    COMP_CHECK(transform->find("flipX") != nullptr);
    COMP_CHECK(transform->find("flipY") != nullptr);
    COMP_CHECK_MSG(transform->string_or("sampling", "") == "High quality", "sampling value");

    COMP_CHECK(entry.find("isVisible") != nullptr);
    COMP_CHECK(entry.find("blendMode") != nullptr);
    COMP_CHECK(entry.find("isGroup") != nullptr);

    const std::string image_file = entry.string_or("imageFile", "");
    if (!image_file.empty()) {
      ++saw_pixels;
      COMP_CHECK_MSG(image_file == id + ".png", "imageFile is <id>.png");
      COMP_CHECK(std::filesystem::exists(path + "/images/" + image_file));
    }

    if (name == "Group") {
      saw_group = true;
      COMP_CHECK(entry.bool_or("isGroup", false));
      COMP_CHECK_MSG(entry.string_or("blendMode", "") == "Normal", "group blendMode forced Normal");
      COMP_CHECK_MSG(image_file.empty(), "group has no imageFile");
    }
    if (name == "Half") {
      saw_half = true;
      COMP_CHECK(entry.string_or("blendMode", "") == "Screen");
      COMP_CHECK(entry.number_or("opacity", 0.0) > 0.49 && entry.number_or("opacity", 0.0) < 0.51);
    }
  }
  COMP_CHECK(saw_group);
  COMP_CHECK(saw_half);
  COMP_CHECK_MSG(saw_pixels == 2, "two pixel layers carry imageFile");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/*
 * 组隔离（engineIsolated）的 .comp 往返。
 *
 * 此前 isolated 只在引擎内存里存在：既没有 ABI 入口，也没有任何落盘路径，隔离组
 * 存盘后再打开就退回穿透（结果从绿变黑）。这里钉住三件事：
 *   1. 穿透（默认）不写键——缺键即穿透，既有工程文件因此逐字节不变；
 *   2. 隔离为真时写 engineIsolated 且能读回；
 *   3. 读回后的工程再次合成仍是隔离语义（存档 -> 打开不丢属性）。
 */
COMP_TEST(format_group_isolation_round_trip) {
  std::error_code ec;
  std::filesystem::create_directories("build", ec);
  const std::string path = "build/format_isolated.comp";
  std::filesystem::remove_all(path, ec);

  comp_engine engine = make_engine();
  comp_document document = make_document(engine, 4, 4, 72.0f);

  comp_layer_id base_ids[8] = {0};
  size_t base_count = 0;
  COMP_CHECK(comp_layer_list(document, base_ids, 8, &base_count) == COMP_OK);
  COMP_CHECK(base_count == 1);
  const comp_layer_id base = base_ids[0];
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, base, &red.image) == COMP_OK);

  comp_layer_desc group_desc{};
  group_desc.name = "Isolated";
  group_desc.blend = COMP_BLEND_NORMAL;
  group_desc.opacity = 1.0f;
  group_desc.transform = comp_rect{0, 0, 4, 4};
  group_desc.is_group = 1;
  comp_layer_id group = 0;
  COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);

  comp_layer_desc child_desc{};
  child_desc.name = "Child";
  child_desc.blend = COMP_BLEND_MULTIPLY;
  child_desc.opacity = 1.0f;
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.parent = group;
  comp_layer_id child = 0;
  COMP_CHECK(comp_layer_add(document, &child_desc, &child) == COMP_OK);
  const SolidImage green(4, 4, 0, 255, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, child, &green.image) == COMP_OK);

  /* 穿透组：不得落 engineIsolated 键（缺键即穿透）。 */
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string pass_through = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(pass_through.find("engineIsolated") == std::string::npos,
                 "pass-through groups write no engineIsolated key");

  /* 打开隔离后落盘：必须写键，且与上游约定一致地只影响引擎侧。 */
  COMP_CHECK(comp_layer_set_isolated(document, group, 1) == COMP_OK);
  COMP_CHECK(comp_document_save(document, path.c_str()) == COMP_OK);
  const std::string manifest = read_text(path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("\"engineIsolated\"") != std::string::npos,
                 "isolated group is persisted");
  COMP_CHECK_MSG(manifest.find("\"engineIsolated\": false") == std::string::npos,
                 "pass-through value is never written explicitly");

  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, path.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);

  comp_layer_id ids[8] = {0};
  size_t count = 0;
  COMP_CHECK(comp_layer_list(loaded, ids, 8, &count) == COMP_OK);
  COMP_CHECK(count == 3);
  size_t isolated_count = 0;
  comp_layer_id loaded_isolated = 0;
  for (size_t i = 0; i < count; ++i) {
    int32_t isolated = 0;
    COMP_CHECK(comp_layer_get_isolated(loaded, ids[i], &isolated) == COMP_OK);
    if (isolated != 0) {
      ++isolated_count;
      loaded_isolated = ids[i];
    }
  }
  COMP_CHECK_MSG(isolated_count == 1, "exactly the isolated group survives the round trip");
  COMP_CHECK(loaded_isolated != 0);
  /* 层 id 经 engineLayerID 往返稳定，故隔离的仍是原来那个组。 */
  COMP_CHECK_MSG(loaded_isolated == group, "isolation stayed on the same layer");

  /* 读回后的合成仍是隔离语义：绿（若退化为穿透则为黑）。 */
  std::vector<uint8_t> canvas(4u * 4u * 4u, 0);
  COMP_CHECK(comp_render_flatten(loaded, canvas.data(), COMP_PIXEL_RGBA8, 4u * 4u) == COMP_OK);
  COMP_CHECK_MSG(near_px(canvas, 1u * 4u + 1u, 0, 255, 0, 255, 0),
                 "isolation survives save/load in compositing");

  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(format_comp_document_id_stable_across_saves) {
  std::error_code ec;
  std::filesystem::create_directories("build", ec);
  const std::string first = "build/format_stable_a.comp";
  const std::string second = "build/format_stable_b.comp";

  comp_engine engine = make_engine();
  comp_document document = make_document(engine, 4, 4, 72.0f);
  const comp_layer_id background = first_layer(document);
  const SolidImage red(4, 4, 255, 0, 0, 255);
  COMP_CHECK(comp_layer_set_pixels(document, background, &red.image) == COMP_OK);

  COMP_CHECK(comp_document_save(document, first.c_str()) == COMP_OK);
  comp_document loaded = nullptr;
  COMP_CHECK(comp_io_load_project(engine, first.c_str(), &loaded) == COMP_OK);
  COMP_CHECK(loaded != nullptr);

  /** 再存一次：documentID 必须与首次一致，否则图层文件名会漂移。 */
  COMP_CHECK(comp_document_save(loaded, second.c_str()) == COMP_OK);

  std::string parse_error;
  const JsonValue a = compositor::json_parse(read_text(first + "/manifest.json"), &parse_error);
  const JsonValue b = compositor::json_parse(read_text(second + "/manifest.json"), &parse_error);
  COMP_CHECK(a.is_object() && b.is_object());
  const std::string id_a = a.string_or("documentID", "");
  COMP_CHECK(uuid_is_wellformed(id_a));
  COMP_CHECK_MSG(id_a == b.string_or("documentID", ""), "documentID stable across saves");

  comp_document_destroy(loaded);
  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(format_comp_rejects_exr_layers) {
  std::error_code ec;
  std::filesystem::create_directories("build/format_exr.comp/images", ec);
  const std::string root = "build/format_exr.comp";
  write_text(root + "/manifest.json",
             std::string("{\n"
                         "  \"format\": \"com.compositor.project\",\n"
                         "  \"version\": 13,\n"
                         "  \"colorSpace\": \"sRGB\",\n"
                         "  \"resolution\": 72,\n"
                         "  \"documentID\": \"E621E1F8-C36C-495A-93FC-0C247A3E6E5F\",\n"
                         "  \"width\": 4,\n"
                         "  \"height\": 4,\n"
                         "  \"layers\": [\n"
                         "    {\n"
                         "      \"id\": \"5F2B1C4A-1D3E-4A6B-8C9D-0E1F2A3B4C5D\",\n"
                         "      \"name\": \"Exr Layer\",\n"
                         "      \"isVisible\": true,\n"
                         "      \"transform\": {\"origin\":[0,0],\"size\":[4,4],\"rotation\":0,"
                         "\"flipX\":false,\"flipY\":false,\"sampling\":\"High quality\"},\n"
                         "      \"imageFile\": \"5F2B1C4A-1D3E-4A6B-8C9D-0E1F2A3B4C5D.exr\",\n"
                         "      \"isGroup\": false\n"
                         "    }\n"
                         "  ]\n"
                         "}\n"));

  comp_engine engine = make_engine();
  comp_document loaded = nullptr;
  /* 本构建不含 OpenEXR：必须显式失败，不能静默产出空白图层。 */
  COMP_CHECK(comp_io_load_project(engine, root.c_str(), &loaded) != COMP_OK);
  COMP_CHECK(loaded == nullptr);
  comp_engine_destroy(engine);
}
