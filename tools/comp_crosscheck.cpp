// AI生成
/*
 * comp_crosscheck.cpp — .comp 与外部编解码器交叉验证宿主（只走 C ABI）。
 *
 * 目的：`.comp` 的 PNG 像素此前只被本仓自己的编码器/解码器「自证」。本宿主提供
 * 两个方向的原始像素导出，供 scripts/crosscheck_comp.py 用 **PIL（独立编解码器）**
 * 做双向交叉比对：
 *
 *   write <comp_dir> <raw_out>   建 16x16 文档 → 加确定性图案图层 → 保存 .comp，
 *                                并把「写入图层的原始像素」导出到 raw_out；
 *   read  <comp_dir> <raw_out>   load .comp → 用 comp_layer_thumbnail(1:1) 取回
 *                                「引擎解码后的像素」并导出到 raw_out。
 *
 * 用法：
 *   comp_crosscheck write build/crosscheck/sample.comp build/crosscheck/written.raw
 *   comp_crosscheck read  build/crosscheck/sample.comp build/crosscheck/decoded.raw
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <compositor/comp_core.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_io.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_render.h>

namespace {

constexpr int32_t kSize = 16;

/* 确定性图案（预乘不变式：rgb <= alpha），便于逐字节比对与失败定位。 */
void fill_pattern(std::vector<uint8_t>& buffer, uint32_t salt) {
  for (int32_t y = 0; y < kSize; ++y) {
    for (int32_t x = 0; x < kSize; ++x) {
      const size_t index = (static_cast<size_t>(y) * kSize + static_cast<size_t>(x)) * 4u;
      const uint32_t a = static_cast<uint32_t>(24 + ((x * 7 + y * 11 + static_cast<int32_t>(salt)) % 232));
      buffer[index + 0] = static_cast<uint8_t>((x * 13 + salt) % (a + 1));
      buffer[index + 1] = static_cast<uint8_t>((y * 17 + salt) % (a + 1));
      buffer[index + 2] = static_cast<uint8_t>((x * y + salt) % (a + 1));
      buffer[index + 3] = static_cast<uint8_t>(a);
    }
  }
}

bool write_raw(const char* path, const std::vector<uint8_t>& buffer) {
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  const size_t written = std::fwrite(buffer.data(), 1, buffer.size(), file);
  std::fclose(file);
  return written == buffer.size();
}

bool read_raw(const char* path, std::vector<uint8_t>* buffer) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    return false;
  }
  buffer->assign(static_cast<size_t>(size), 0);
  const size_t got = std::fread(buffer->data(), 1, buffer->size(), file);
  std::fclose(file);
  return got == buffer->size();
}

comp_engine make_engine() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-crosscheck";
  desc.version = "0.1.0";
  desc.backend = COMP_BACKEND_AUTO;
  comp_engine engine = nullptr;
  if (comp_engine_create(&desc, &engine) != COMP_OK) return nullptr;
  return engine;
}

int run_write(const char* comp_dir, const char* raw_out) {
  comp_engine engine = make_engine();
  if (engine == nullptr) {
    std::printf("[write] 引擎创建失败\n");
    return 1;
  }
  comp_document_desc doc_desc{};
  doc_desc.width = kSize;
  doc_desc.height = kSize;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  if (comp_document_create(engine, &doc_desc, &document) != COMP_OK || document == nullptr) {
    std::printf("[write] 文档创建失败\n");
    comp_engine_destroy(engine);
    return 1;
  }

  std::vector<uint8_t> pattern(static_cast<size_t>(kSize) * kSize * 4u, 0);
  fill_pattern(pattern, 3u);

  comp_image image{};
  image.width = kSize;
  image.height = kSize;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(kSize) * 4u;
  image.pixels = pattern.data();

  comp_layer_desc layer_desc{};
  layer_desc.name = "crosscheck-layer";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  if (comp_layer_add(document, &layer_desc, &layer) != COMP_OK ||
      comp_layer_set_pixels(document, layer, &image) != COMP_OK) {
    std::printf("[write] 图层写入失败\n");
    comp_document_destroy(document);
    comp_engine_destroy(engine);
    return 1;
  }

  const comp_result saved = comp_document_save(document, comp_dir);
  const bool raw_ok = write_raw(raw_out, pattern);
  std::printf("[write] save=%s layer_id=%llu raw=%s bytes=%zu\n",
              saved == COMP_OK ? "OK" : "FAIL", static_cast<unsigned long long>(layer),
              raw_ok ? "OK" : "FAIL", pattern.size());

  comp_document_destroy(document);
  comp_engine_destroy(engine);
  return (saved == COMP_OK && raw_ok) ? 0 : 1;
}

int run_read(const char* comp_dir, const char* raw_out) {
  comp_engine engine = make_engine();
  if (engine == nullptr) {
    std::printf("[read] 引擎创建失败\n");
    return 1;
  }
  comp_document document = nullptr;
  if (comp_document_load(engine, comp_dir, &document) != COMP_OK || document == nullptr) {
    std::printf("[read] load 失败: %s\n",
                comp_engine_last_error(engine) != nullptr ? comp_engine_last_error(engine) : "-");
    comp_engine_destroy(engine);
    return 1;
  }

  size_t count = 0;
  if (comp_layer_count(document, &count) != COMP_OK || count == 0) {
    std::printf("[read] 无图层\n");
    comp_document_destroy(document);
    comp_engine_destroy(engine);
    return 1;
  }
  std::vector<comp_layer_id> ids(count, 0);
  size_t listed = 0;
  if (comp_layer_list(document, ids.data(), ids.size(), &listed) != COMP_OK || listed == 0) {
    std::printf("[read] 图层枚举失败\n");
    comp_document_destroy(document);
    comp_engine_destroy(engine);
    return 1;
  }

  /* 取 layer_id 最大的图层（新建文档的背景层先建，追加层的 id 更大）。 */
  comp_layer_id target = ids[0];
  for (size_t i = 1; i < listed; ++i) {
    if (ids[i] > target) target = ids[i];
  }
  std::vector<uint8_t> decoded(static_cast<size_t>(kSize) * kSize * 4u, 0);
  comp_size out_size{};
  const comp_result thumb = comp_render_layer_thumbnail(document, target, kSize, decoded.data(),
                                                       static_cast<size_t>(kSize) * 4u, &out_size);
  const bool raw_ok = (thumb == COMP_OK) && write_raw(raw_out, decoded);
  std::printf("[read] layers=%zu target=%llu thumbnail=%s size=%dx%d raw=%s bytes=%zu\n", listed,
              static_cast<unsigned long long>(target), thumb == COMP_OK ? "OK" : "FAIL",
              out_size.width, out_size.height, raw_ok ? "OK" : "FAIL", decoded.size());

  comp_document_destroy(document);
  comp_engine_destroy(engine);
  return raw_ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("用法: comp_crosscheck <write|read> <comp_dir> <raw_out>\n");
    return 2;
  }
  if (std::strcmp(argv[1], "write") == 0) return run_write(argv[2], argv[3]);
  if (std::strcmp(argv[1], "read") == 0) return run_read(argv[2], argv[3]);
  std::printf("未知模式: %s\n", argv[1]);
  return 2;
}
