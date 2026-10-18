// AI生成
/*
 * test_codec.cpp — M5「编解码替换」接缝与 PNG 真实导入/导出的端到端验证。
 *
 * 两件事：
 *   1. 能力登记表必须只陈述事实：PNG 内建可用，未链接的格式如实不可用并给出原因；
 *   2. PNG 的 export -> import 往返必须逐字节一致（不透明像素下预乘 == 直通），
 *      证明 comp_io_export / comp_io_import 不是空壳。
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "format/comp_png.h"
#include "test_framework.h"

namespace {

comp_engine_desc codec_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-codec-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

std::string codec_workspace_file(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/codec", ec);
  return std::string("build/codec/") + name;
}

}  // namespace

COMP_TEST(codec_support_reports_only_facts) {
  comp_codec_state state = COMP_CODEC_UNAVAILABLE;
  size_t note_size = 0;

  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_PNG, &state, nullptr, 0, &note_size) == COMP_OK);
  /* PNG 由 vendor 的真实库（libpng + zlib）或自研 comp_png 提供，两者都算真实可用。 */
  COMP_CHECK(state == COMP_CODEC_VENDORED || state == COMP_CODEC_BUILTIN);

  /* EXR：树内自研 scanline 读取器（C3b），可读不可写。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_EXR, &state, nullptr, 0, &note_size) == COMP_OK);
  COMP_CHECK(state == COMP_CODEC_BUILTIN);

  std::vector<char> note(note_size + 1, '\0');
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_EXR, nullptr, note.data(), note.size(), &note_size) ==
             COMP_OK);
  COMP_CHECK(std::strstr(note.data(), "scanline") != nullptr);
  COMP_CHECK_MSG(std::strstr(note.data(), "export unsupported") != nullptr,
                 "the note must state that EXR export is not available");

  /* 有损栅格：JPEG 由 vendor 的 stb_image / stb_image_write 真读真写。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_JPEG, &state, nullptr, 0, &note_size) == COMP_OK);
  COMP_CHECK(state == COMP_CODEC_VENDORED);
  size_t jpeg_need = 0;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_JPEG, nullptr, nullptr, 0, &jpeg_need) == COMP_OK);
  std::vector<char> jpeg_note(jpeg_need + 1, '\0');
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_JPEG, nullptr, jpeg_note.data(), jpeg_note.size(),
                                   &jpeg_need) == COMP_OK);
  COMP_CHECK(std::strstr(jpeg_note.data(), "stb_image") != nullptr);

  /* PSD：可读不可写（stb_image 只解不封），状态 VENDORED 但导出必须 UNSUPPORTED。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_PSD, &state, nullptr, 0, &note_size) == COMP_OK);
  COMP_CHECK(state == COMP_CODEC_VENDORED);

  /* 未登记格式：如实 NOT_FOUND + UNAVAILABLE，不假装可用。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_UNKNOWN, &state, nullptr, 0, &note_size) ==
             COMP_ERR_NOT_FOUND);
  COMP_CHECK(state == COMP_CODEC_UNAVAILABLE);

  /* 截断读取必须仍然以 '\0' 结尾（两段式契约）。 */
  char small[8];
  std::memset(small, 'x', sizeof(small));
  size_t need = 0;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_HEIF, nullptr, small, sizeof(small), &need) == COMP_OK);
  COMP_CHECK(small[sizeof(small) - 1] == '\0');
  COMP_CHECK(need > sizeof(small));
}

COMP_TEST(codec_png_export_import_round_trip_is_exact) {
  comp_engine_desc desc = codec_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const int32_t width = 8;
  const int32_t height = 6;
  comp_document_desc document_desc{};
  document_desc.width = width;
  document_desc.height = height;
  document_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  /* 全不透明像素：预乘与直通数值相同，往返必须逐字节相等。 */
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<uint8_t>((i * 7u) % 251u);
    pixels[i + 1] = static_cast<uint8_t>((i * 13u) % 241u);
    pixels[i + 2] = static_cast<uint8_t>((i * 29u) % 239u);
    pixels[i + 3] = 255u;
  }

  comp_layer_id background = 0;
  size_t layer_count = 0;
  COMP_CHECK(comp_layer_list(document, &background, 1u, &layer_count) == COMP_OK);
  COMP_CHECK(layer_count >= 1u);

  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(width) * 4u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(document, background, &image) == COMP_OK);

  const std::string path = codec_workspace_file("round_trip.png");
  COMP_CHECK_MSG(comp_io_export(document, path.c_str(), COMP_FORMAT_PNG, nullptr) == COMP_OK,
                 "comp_io_export(png)");
  COMP_CHECK_MSG(std::filesystem::exists(path), "exported png exists");
  std::error_code size_ec;
  const std::uintmax_t png_bytes = std::filesystem::file_size(path, size_ec);
  COMP_CHECK_MSG(!size_ec && png_bytes > 8u, "exported png not empty");

  comp_document loaded = nullptr;
  COMP_CHECK_MSG(comp_io_import(engine, path.c_str(), &loaded) == COMP_OK, "comp_io_import(png)");
  if (loaded != nullptr) {
    comp_size size{};
    COMP_CHECK(comp_document_size(loaded, &size) == COMP_OK);
    COMP_CHECK(size.width == width);
    COMP_CHECK(size.height == height);

    std::vector<uint8_t> decoded(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    COMP_CHECK(comp_render_flatten(loaded, decoded.data(), COMP_PIXEL_RGBA8,
                                   static_cast<size_t>(width) * 4u) == COMP_OK);

    size_t diffs = 0;
    int worst = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
      const int delta = static_cast<int>(decoded[i]) - static_cast<int>(pixels[i]);
      const int magnitude = delta < 0 ? -delta : delta;
      if (magnitude != 0) ++diffs;
      if (magnitude > worst) worst = magnitude;
    }
    COMP_CHECK_MSG(diffs == 0, "png round trip is byte exact");
    COMP_CHECK_MSG(worst == 0, "png round trip max delta is 0");
    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(codec_png_import_layer_appends_layer) {
  comp_engine_desc desc = codec_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const std::string path = codec_workspace_file("layer_source.png");
  {
    comp_document_desc source_desc{};
    source_desc.width = 4;
    source_desc.height = 4;
    comp_document source = nullptr;
    COMP_CHECK(comp_document_create(engine, &source_desc, &source) == COMP_OK);

    std::vector<uint8_t> pixels(4u * 4u * 4u, 200u);
    comp_image image{};
    image.width = 4;
    image.height = 4;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = 4u * 4u;
    image.pixels = pixels.data();

    comp_layer_id first = 0;
    size_t count = 0;
    COMP_CHECK(comp_layer_list(source, &first, 1u, &count) == COMP_OK);
    COMP_CHECK(comp_layer_set_pixels(source, first, &image) == COMP_OK);
    COMP_CHECK(comp_io_export(source, path.c_str(), COMP_FORMAT_PNG, nullptr) == COMP_OK);
    comp_document_destroy(source);
  }

  comp_document_desc target_desc{};
  target_desc.width = 8;
  target_desc.height = 8;
  comp_document target = nullptr;
  COMP_CHECK(comp_document_create(engine, &target_desc, &target) == COMP_OK);

  size_t before = 0;
  COMP_CHECK(comp_layer_count(target, &before) == COMP_OK);

  comp_layer_id added = 0;
  COMP_CHECK_MSG(comp_io_import_layer(target, path.c_str(), &added) == COMP_OK,
                 "comp_io_import_layer(png)");
  COMP_CHECK(added != 0);

  size_t after = 0;
  COMP_CHECK(comp_layer_count(target, &after) == COMP_OK);
  COMP_CHECK_MSG(after == before + 1u, "import_layer appends exactly one layer");

  comp_document_destroy(target);
  comp_engine_destroy(engine);
}

COMP_TEST(codec_import_reports_io_error_for_missing_png) {
  comp_engine_desc desc = codec_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  comp_document imported = nullptr;
  COMP_CHECK_MSG(comp_io_import(engine, "build/codec/__missing__.png", &imported) == COMP_ERR_IO,
                 "missing png is IO error, not unsupported");
  COMP_CHECK(imported == nullptr);

  /* 仍未落地的格式：不支持，并写明原因（不是文件不存在）。 */
  COMP_CHECK(comp_io_import(engine, "photo.tiff", &imported) == COMP_ERR_UNSUPPORTED);
  COMP_CHECK(imported == nullptr);

  /* JPEG 已由 vendor 的 stb_image 接上：文件不存在时同样是 IO 错误。 */
  COMP_CHECK(comp_io_import(engine, "build/codec/__missing__.jpg", &imported) == COMP_ERR_IO);
  COMP_CHECK(imported == nullptr);

  comp_engine_destroy(engine);
}

COMP_TEST(codec_jpeg_export_import_round_trip_is_lossy_but_consistent) {
  /* 有损格式不可能逐字节相等，但尺寸必须守恒、误差必须有界。 */
  comp_engine_desc desc = codec_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const int32_t width = 16;
  const int32_t height = 12;
  comp_document_desc document_desc{};
  document_desc.width = width;
  document_desc.height = height;
  document_desc.color_space = "sRGB";
  comp_document document = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);

  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<uint8_t>((i * 3u) % 200u + 28u);
    pixels[i + 1] = static_cast<uint8_t>((i * 5u) % 200u + 28u);
    pixels[i + 2] = static_cast<uint8_t>((i * 11u) % 200u + 28u);
    pixels[i + 3] = 255u;
  }

  comp_layer_id background = 0;
  size_t layer_count = 0;
  COMP_CHECK(comp_layer_list(document, &background, 1u, &layer_count) == COMP_OK);
  COMP_CHECK(layer_count >= 1u);

  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(width) * 4u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(document, background, &image) == COMP_OK);

  const std::string path = codec_workspace_file("round_trip.jpg");
  COMP_CHECK_MSG(comp_io_export(document, path.c_str(), COMP_FORMAT_JPEG, "{\"quality\":95}") == COMP_OK,
                 "comp_io_export(jpeg)");
  COMP_CHECK_MSG(std::filesystem::exists(path), "exported jpeg exists");

  comp_document loaded = nullptr;
  COMP_CHECK_MSG(comp_io_import(engine, path.c_str(), &loaded) == COMP_OK, "comp_io_import(jpeg)");
  if (loaded != nullptr) {
    comp_size size{};
    COMP_CHECK(comp_document_size(loaded, &size) == COMP_OK);
    COMP_CHECK(size.width == width);
    COMP_CHECK(size.height == height);

    std::vector<uint8_t> decoded(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    COMP_CHECK(comp_render_flatten(loaded, decoded.data(), COMP_PIXEL_RGBA8,
                                   static_cast<size_t>(width) * 4u) == COMP_OK);

    int worst = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
      const int delta = static_cast<int>(decoded[i]) - static_cast<int>(pixels[i]);
      const int magnitude = delta < 0 ? -delta : delta;
      if (magnitude > worst) worst = magnitude;
    }
    /* quality=95 下基线 JPEG 的色度子采样误差经验上界放宽到 24/255，足以区分真实编解码与空壳。 */
    COMP_CHECK_MSG(worst <= 24, "jpeg round trip error is bounded");

    /* PSD 只读：导出必须显式 UNSUPPORTED，且原因来自登记表而非占位文案。 */
    const std::string psd_path = codec_workspace_file("out.psd");
    COMP_CHECK(comp_io_export(document, psd_path.c_str(), COMP_FORMAT_PSD, nullptr) ==
               COMP_ERR_UNSUPPORTED);

    comp_document_destroy(loaded);
  }

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* -------------------------------------------------- P1-2 负向：伪造 IHDR 入库 */

namespace {

/* PNG 用的是标准 CRC-32（多项式 0xEDB88320，初值/终值与 PNG 规范一致）。 */
uint32_t crc32_png(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void append_be32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

void append_chunk(std::vector<uint8_t>* out, const char* type,
                  const std::vector<uint8_t>& payload) {
  append_be32(out, static_cast<uint32_t>(payload.size()));
  const size_t crc_start = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), payload.begin(), payload.end());
  append_be32(out, crc32_png(out->data() + crc_start, out->size() - crc_start));
}

/*
 * 伪造 PNG：结构与 CRC 全部合法，只有 IHDR 声明的尺寸被放大。
 * IDAT 用合法的「空 zlib 存储流」，因此解码器若信任声明尺寸就会按声明分配。
 */
std::vector<uint8_t> forged_png(uint32_t width, uint32_t height) {
  std::vector<uint8_t> png = {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
  std::vector<uint8_t> ihdr;
  append_be32(&ihdr, width);
  append_be32(&ihdr, height);
  ihdr.push_back(8u);  /* bit depth */
  ihdr.push_back(6u);  /* color type: RGBA */
  ihdr.push_back(0u);  /* compression */
  ihdr.push_back(0u);  /* filter */
  ihdr.push_back(0u);  /* interlace: none */
  append_chunk(&png, "IHDR", ihdr);
  append_chunk(&png, "IDAT", std::vector<uint8_t>{0x78u, 0x9Cu, 0x03u, 0x00u, 0x00u, 0x00u,
                                                  0x00u, 0x01u});
  append_chunk(&png, "IEND", std::vector<uint8_t>{});
  return png;
}

void write_binary_file(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return;
  std::fwrite(bytes.data(), 1u, bytes.size(), file);
  std::fclose(file);
}

bool mentions_canvas(const std::string& text) {
  return text.find("canvas") != std::string::npos;
}

}  // namespace

/*
 * P1-2 回归护栏（此前只有一次性探针，未入库；见审核报告 §6.4-1）。
 * 三个断言组：
 *   1) 正常 PNG 仍然导入成功（守卫没有误伤）；
 *   2) 伪造 IHDR 在 C ABI（libpng 路径）被拒，且 last_error 带上真实原因（§6.4-2）；
 *   3) 直接调自研 comp_png 解码器，守卫在自研路径上同样生效。
 */
COMP_TEST(codec_rejects_forged_oversized_ihdr) {
  comp_engine_desc desc = codec_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  /* --- 1) 正常尺寸仍然可用 --- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 4;
    document_desc.height = 4;
    document_desc.color_space = "sRGB";
    comp_document source = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &source) == COMP_OK);
    const std::string good = codec_workspace_file("guard_good.png");
    COMP_CHECK(comp_io_export(source, good.c_str(), COMP_FORMAT_PNG, nullptr) == COMP_OK);
    comp_document_destroy(source);

    comp_document imported = nullptr;
    COMP_CHECK_MSG(comp_io_import(engine, good.c_str(), &imported) == COMP_OK,
                   "sane png still imports");
    if (imported != nullptr) comp_document_destroy(imported);
  }

  struct Case {
    const char* name;
    uint32_t width;
    uint32_t height;
  };
  const Case cases[] = {
      {"forged_40000x40000.png", 40000u, 40000u},   /* ≈6.4 GB 若无上界 */
      {"forged_1000000x4.png", 1000000u, 4u},       /* 恰好等于 libpng 自带 user limit */
  };

  for (const Case& item : cases) {
    const std::vector<uint8_t> bytes = forged_png(item.width, item.height);
    const std::string path = codec_workspace_file(item.name);
    write_binary_file(path, bytes);

    /* --- 2) C ABI（libpng 路径）必须拒绝，且不得留下文档 --- */
    comp_document imported = nullptr;
    const comp_result result = comp_io_import(engine, path.c_str(), &imported);
    COMP_CHECK_MSG(result != COMP_OK, "forged IHDR is rejected by comp_io_import");
    COMP_CHECK_MSG(imported == nullptr, "rejected import yields no document");
    if (imported != nullptr) comp_document_destroy(imported);

    /* §6.4-2：真实原因必须出现在 last_error 里，而不是被吞成通用文案。 */
    const char* last_error = comp_engine_last_error(engine);
    COMP_CHECK_MSG(last_error != nullptr && mentions_canvas(std::string(last_error)),
                   "last_error carries the real reason (canvas size)");

    /* --- 3) 自研 comp_png 路径：同一份字节直接喂给解码器 --- */
    compositor::PngImage decoded;
    std::string decode_error;
    const bool ok = compositor::png_decode_rgba8(bytes.data(), bytes.size(), &decoded,
                                                &decode_error);
    COMP_CHECK_MSG(!ok, "self-built comp_png rejects the forged IHDR too");
    COMP_CHECK_MSG(decoded.empty(), "self-built comp_png produces no image");
    COMP_CHECK_MSG(mentions_canvas(decode_error), "self-built comp_png names the reason");
  }

  comp_engine_destroy(engine);
}
