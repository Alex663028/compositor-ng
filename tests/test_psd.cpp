// AI生成
/*
 * test_psd.cpp — M1「PSD 常用子集分层读写」的验收测试。
 *
 * 全部不依赖第三方库：fixture 由本文件按 PSD 规范**手写字节流**，因此每条断言的
 * 期望值都能在源码里直接核对，测试跑在 CI 上也不会有「psd-tools 版本变了」的漂移。
 *
 * 四层验证：
 *   1. 解码：图层表 / 位置 / 不透明度 / 混合 / 组 / 剪贴 / 蒙版展开 / 预乘 / 16 位；
 *   2. ABI：comp_io_import_document 暴露的图层树与引擎口径（穿透组 = 不隔离）；
 *   3. `.comp` 往返：保存 → 加载 → 再保存，整个目录包**逐文件逐字节**一致；
 *   4. 导出：comp_io_export_document(PSD) 写出的文件读回来与原件同构；表示不了的
 *      文档一律 COMP_ERR_UNSUPPORTED（宁可打不开，也不静默降级成另一张图）。
 *
 * 蒙版口径（与 format/psd_codec.h 的契约一致）：PSD 的蒙版有自己的矩形，解码时
 * 按 `layer_rect - mask_rect` 展开到图层像素空间，矩形外取默认色，invert 取反。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "format/psd_codec.h"
#include "test_framework.h"

namespace {

using compositor::ImageBuffer;
using compositor::PsdDocument;
using compositor::PsdLayerData;
using compositor::PsdStatus;

/* ===================================================== 大端字节流小工具 */

void push8(std::vector<uint8_t>* out, uint8_t value) { out->push_back(value); }

void push16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

void push_i16(std::vector<uint8_t>* out, int16_t value) {
  push16(out, static_cast<uint16_t>(value));
}

void push32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value >> 24));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

void push_i32(std::vector<uint8_t>* out, int32_t value) {
  push32(out, static_cast<uint32_t>(value));
}

void push_bytes(std::vector<uint8_t>* out, const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

void push_str(std::vector<uint8_t>* out, const char* text) {
  push_bytes(out, text, std::strlen(text));
}

void patch16(std::vector<uint8_t>* out, size_t at, uint16_t value) {
  (*out)[at + 0] = static_cast<uint8_t>(value >> 8);
  (*out)[at + 1] = static_cast<uint8_t>(value & 0xFFu);
}

void patch32(std::vector<uint8_t>* out, size_t at, uint32_t value) {
  (*out)[at + 0] = static_cast<uint8_t>(value >> 24);
  (*out)[at + 1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  (*out)[at + 2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  (*out)[at + 3] = static_cast<uint8_t>(value & 0xFFu);
}

void pad_to(std::vector<uint8_t>* out, size_t align) {
  while ((out->size() % align) != 0u) out->push_back(0u);
}

/* ===================================================== PSD fixture 构造器 */

/* 一个通道块。samples 是**文件字节序**的行紧排样本（16 位已经是 big-endian）。 */
struct FixtureChannel {
  int16_t id = 0;
  uint16_t compression = compositor::kPsdCompressionRaw;
  std::vector<uint8_t> samples;
};

FixtureChannel uniform_channel(int16_t id, int32_t width, int32_t height, uint8_t value) {
  FixtureChannel channel;
  channel.id = id;
  const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
  channel.samples.assign(count, value);
  return channel;
}

/* 16 位通道：samples 用 native uint16 给，函数转成 big-endian 字节。 */
FixtureChannel channel16(int16_t id, const std::vector<uint16_t>& samples) {
  FixtureChannel channel;
  channel.id = id;
  channel.samples.reserve(samples.size() * 2u);
  for (uint16_t sample : samples) {
    channel.samples.push_back(static_cast<uint8_t>(sample >> 8));
    channel.samples.push_back(static_cast<uint8_t>(sample & 0xFFu));
  }
  return channel;
}

struct FixtureRecord {
  int32_t top = 0;
  int32_t left = 0;
  int32_t bottom = 0;
  int32_t right = 0;
  std::vector<FixtureChannel> channels;
  std::string blend_key = "norm";
  uint8_t opacity = 255;
  uint8_t clipping = 0;
  uint8_t flags = 0x08; /* 0x02 = 隐藏，可见时写 0x08（与 Photoshop 一致） */
  std::string name = "Layer";

  /* lsct：0 = 普通层，3 = 边界分隔（组在它上面开始），1|2 = 文件夹（收口）。 */
  int32_t section_kind = 0;
  std::string section_key = "norm";

  /* 蒙版块（-2 通道必须同时存在，否则解析器会按「结构自相矛盾」拒绝）。 */
  bool has_mask_block = false;
  uint32_t mask_block_length = 20;
  int32_t mask_top = 0;
  int32_t mask_left = 0;
  int32_t mask_bottom = 0;
  int32_t mask_right = 0;
  uint8_t mask_default = 0;
  uint8_t mask_flags = 0;

  /* 额外 tag（key + body），用于注入 TySh 之类的拒绝用例。 */
  std::vector<std::pair<std::string, std::vector<uint8_t>>> extra_tags;

  int32_t width() const { return right - left; }
  int32_t height() const { return bottom - top; }
  int32_t mask_width() const { return mask_right - mask_left; }
  int32_t mask_height() const { return mask_bottom - mask_top; }
};

struct PsdFixture {
  int32_t width = 32;
  int32_t height = 24;
  uint16_t version = 1;
  uint16_t channels = 4;
  uint16_t depth = 8;
  uint16_t mode = 3; /* 3 = RGB */
  bool flat = false; /* true：layer/mask 段长度写 0（已拍平文件） */
  std::vector<FixtureRecord> records;
};

std::vector<uint8_t> build_extra(const FixtureRecord& record) {
  std::vector<uint8_t> extra;
  if (record.has_mask_block) {
    push32(&extra, record.mask_block_length);
    push32(&extra, static_cast<uint32_t>(record.mask_top));
    push32(&extra, static_cast<uint32_t>(record.mask_left));
    push32(&extra, static_cast<uint32_t>(record.mask_bottom));
    push32(&extra, static_cast<uint32_t>(record.mask_right));
    push8(&extra, record.mask_default);
    push8(&extra, record.mask_flags);
    push8(&extra, 0);
    push8(&extra, 0);
    /* 负面用例可能把 length 撑到 36 以上；多出来的字节解析器会整体跳过。 */
    while (extra.size() < 4u + record.mask_block_length) push8(&extra, 0);
  } else {
    push32(&extra, 0);
  }

  /* 混合范围：10 对 (0, 65535)，共 40 字节（Photoshop 的默认写法）。 */
  push32(&extra, 40u);
  for (int i = 0; i < 10; ++i) {
    push16(&extra, 0u);
    push16(&extra, 65535u);
  }

  /* Pascal 名字：对齐是相对字符串自身起点算的。 */
  std::string ascii = record.name.size() > 255u ? record.name.substr(0, 255u) : record.name;
  const size_t pascal_start = extra.size();
  push8(&extra, static_cast<uint8_t>(ascii.size()));
  push_bytes(&extra, ascii.data(), ascii.size());
  while (((extra.size() - pascal_start) % 4u) != 0u) push8(&extra, 0);

  /* luni：真名（UTF-16BE）。 */
  {
    std::vector<uint8_t> body;
    push32(&body, static_cast<uint32_t>(record.name.size()));
    for (char c : record.name) push16(&body, static_cast<uint16_t>(static_cast<unsigned char>(c)));
    push_str(&extra, "8BIM");
    push_str(&extra, "luni");
    push32(&extra, static_cast<uint32_t>(body.size()));
    push_bytes(&extra, body.data(), body.size());
  }

  /* lsct：组信息（kind + 8BIM + 混合键）。 */
  if (record.section_kind != 0) {
    std::vector<uint8_t> body;
    push32(&body, static_cast<uint32_t>(record.section_kind));
    push_str(&body, "8BIM");
    push_bytes(&body, record.section_key.data(), 4);
    push_str(&extra, "8BIM");
    push_str(&extra, "lsct");
    push32(&extra, static_cast<uint32_t>(body.size()));
    push_bytes(&extra, body.data(), body.size());
  }

  for (const auto& tag : record.extra_tags) {
    push_str(&extra, "8BIM");
    push_str(&extra, tag.first.c_str());
    push32(&extra, static_cast<uint32_t>(tag.second.size()));
    push_bytes(&extra, tag.second.data(), tag.second.size());
  }

  pad_to(&extra, 2);
  return extra;
}

/* 一个通道块：2 字节压缩标记 + 数据。
 *   raw(0)：平面字节直贴；
 *   RLE(1)：u16 行长度表 + 每行**只走直串**的 PackBits（合法且可手算）；
 *   其它：按负面用例用——块体只填同尺寸的零，长度自洽即可。 */
std::vector<uint8_t> encode_block(const FixtureChannel& channel, int32_t width, int32_t height,
                                  size_t sample_bytes) {
  std::vector<uint8_t> out;
  push16(&out, channel.compression);
  if (width <= 0 || height <= 0) return out;
  const size_t row_bytes = static_cast<size_t>(width) * sample_bytes;
  if (channel.compression == compositor::kPsdCompressionRaw) {
    out.insert(out.end(), channel.samples.begin(), channel.samples.end());
    return out;
  }
  if (channel.compression == compositor::kPsdCompressionRle) {
    std::vector<uint8_t> packed;
    std::vector<uint16_t> counts;
    for (int32_t y = 0; y < height; ++y) {
      const uint8_t* row = channel.samples.data() + static_cast<size_t>(y) * row_bytes;
      const size_t before = packed.size();
      size_t i = 0;
      while (i < row_bytes) {
        const size_t run = std::min<size_t>(128u, row_bytes - i);
        packed.push_back(static_cast<uint8_t>(run - 1u)); /* 直串控制字 */
        packed.insert(packed.end(), row + i, row + i + run);
        i += run;
      }
      counts.push_back(static_cast<uint16_t>(packed.size() - before));
    }
    for (uint16_t count : counts) push16(&out, count);
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
  }
  out.resize(out.size() + row_bytes * static_cast<size_t>(height), 0u);
  return out;
}

std::vector<uint8_t> build_psd(const PsdFixture& fixture) {
  const size_t sample_bytes = fixture.depth == 16 ? 2u : 1u;

  std::vector<uint8_t> records;
  std::vector<uint8_t> channel_data;
  for (const FixtureRecord& record : fixture.records) {
    const std::vector<uint8_t> extra = build_extra(record);
    push_i32(&records, record.top);
    push_i32(&records, record.left);
    push_i32(&records, record.bottom);
    push_i32(&records, record.right);
    push16(&records, static_cast<uint16_t>(record.channels.size()));
    for (const FixtureChannel& channel : record.channels) {
      const bool is_mask = channel.id == -2;
      const int32_t channel_w = is_mask ? record.mask_width() : record.width();
      const int32_t channel_h = is_mask ? record.mask_height() : record.height();
      /* 蒙版通道恒为 8 位。 */
      const std::vector<uint8_t> block =
          encode_block(channel, channel_w, channel_h, is_mask ? 1u : sample_bytes);
      push_i16(&records, channel.id);
      push32(&records, static_cast<uint32_t>(block.size()));
      channel_data.insert(channel_data.end(), block.begin(), block.end());
    }
    push_str(&records, "8BIM");
    push_bytes(&records, record.blend_key.data(), 4);
    push8(&records, record.opacity);
    push8(&records, record.clipping);
    push8(&records, record.flags);
    push8(&records, 0);
    push32(&records, static_cast<uint32_t>(extra.size()));
    records.insert(records.end(), extra.begin(), extra.end());
  }

  std::vector<uint8_t> layer_info;
  push_i16(&layer_info, static_cast<int16_t>(-static_cast<int32_t>(fixture.records.size())));
  layer_info.insert(layer_info.end(), records.begin(), records.end());
  layer_info.insert(layer_info.end(), channel_data.begin(), channel_data.end());
  pad_to(&layer_info, 4);

  std::vector<uint8_t> out;
  push_str(&out, "8BPS");
  push16(&out, fixture.version);
  push32(&out, 0); /* 6 字节保留（4 + 2），别写成 2 字节 */
  push16(&out, 0);
  push16(&out, fixture.channels);
  push32(&out, static_cast<uint32_t>(fixture.height));
  push32(&out, static_cast<uint32_t>(fixture.width));
  push16(&out, fixture.depth);
  push16(&out, fixture.mode);
  push32(&out, 0); /* 色彩模式数据 */
  push32(&out, 0); /* 图像资源 */

  const size_t layer_mask_pos = out.size();
  push32(&out, 0);

  if (fixture.flat) {
    patch32(&out, layer_mask_pos, 0u);
  } else if (fixture.depth == 16) {
    /* 16 位：主 layer-info 长度写 0，记录放进 Lr16 附加块（块内不带外层长度）。 */
    push32(&out, 0);
    push32(&out, 0); /* 全局图层蒙版 */
    push_str(&out, "8BIM");
    push_str(&out, "Lr16");
    push32(&out, static_cast<uint32_t>(layer_info.size()));
    push_bytes(&out, layer_info.data(), layer_info.size());
    patch32(&out, layer_mask_pos, static_cast<uint32_t>(out.size() - layer_mask_pos - 4u));
  } else {
    push32(&out, static_cast<uint32_t>(layer_info.size()));
    push_bytes(&out, layer_info.data(), layer_info.size());
    push32(&out, 0); /* 全局图层蒙版 */
    patch32(&out, layer_mask_pos, static_cast<uint32_t>(out.size() - layer_mask_pos - 4u));
  }

  /* 合并图像：raw 全零。解码器不读它，但段必须存在。 */
  push16(&out, compositor::kPsdCompressionRaw);
  out.resize(out.size() + static_cast<size_t>(fixture.width) * static_cast<size_t>(fixture.height) *
                              static_cast<size_t>(fixture.channels) * sample_bytes,
             0u);
  return out;
}

/* ------------------------------------------- 验收用的分层 fixture（32×24）

 * 图层表（下标 0 在最底下）：
 *   0 Background  32×24 @ (0,0)   R=10 G=20 B=30 A=200
 *   1 Clip         8×8  @ (4,4)   剪贴到 Background，R=255 G=0 B=0 A=128
 *   2 Group 1     （穿透组，lsct 键 pass）
 *   3   Inner     18×10 @ (6,6)   Multiply / 不透明度 200 / 蒙版 18×10 @ (8,7)
 *   4 Iso Group   （隔离组，lsct 键 "mul "，不透明度 128）
 *   5   Iso Layer  8×6  @ (2,18)  Screen / 不透明度 77
 *   6 Hidden       6×6  @ (12,2)  隐藏，无 alpha 通道
 */
FixtureChannel inner_mask_channel() {
  FixtureChannel channel;
  channel.id = -2;
  const int32_t width = 18;
  const int32_t height = 10;
  channel.samples.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t value = 255;
      if (y == 0) {
        value = 0;
      } else if (y == 1) {
        value = (x >= 2 && x <= 8) ? 255 : 0;
      }
      channel.samples[static_cast<size_t>(y) * static_cast<size_t>(width) +
                      static_cast<size_t>(x)] = value;
    }
  }
  return channel;
}

PsdFixture staged_fixture() {
  PsdFixture fixture;
  fixture.width = 32;
  fixture.height = 24;

  { /* 0 Background */
    FixtureRecord record;
    record.name = "Background";
    record.bottom = 24;
    record.right = 32;
    record.channels = {uniform_channel(0, 32, 24, 10u), uniform_channel(1, 32, 24, 20u),
                       uniform_channel(2, 32, 24, 30u), uniform_channel(-1, 32, 24, 200u)};
    fixture.records.push_back(record);
  }
  { /* 1 Clip */
    FixtureRecord record;
    record.name = "Clip";
    record.top = 4;
    record.left = 4;
    record.bottom = 12;
    record.right = 12;
    record.clipping = 1;
    record.channels = {uniform_channel(0, 8, 8, 255u), uniform_channel(1, 8, 8, 0u),
                       uniform_channel(2, 8, 8, 0u), uniform_channel(-1, 8, 8, 128u)};
    fixture.records.push_back(record);
  }
  { /* 2 Group 1 的边界分隔记录（隐藏，Photoshop 的惯例） */
    FixtureRecord record;
    record.name = "</Layer group>";
    record.flags = 0x0A;
    record.section_kind = 3;
    record.section_key = "norm";
    fixture.records.push_back(record);
  }
  { /* 3 Inner（蒙版矩形比图层小：left 8 vs 6，top 7 vs 6） */
    FixtureRecord record;
    record.name = "Inner";
    record.top = 6;
    record.left = 6;
    record.bottom = 16;
    record.right = 24;
    record.blend_key = "mul ";
    record.opacity = 200;
    record.has_mask_block = true;
    record.mask_top = 7;
    record.mask_left = 8;
    record.mask_bottom = 17;
    record.mask_right = 26;
    record.channels = {uniform_channel(0, 18, 10, 100u), uniform_channel(1, 18, 10, 150u),
                       uniform_channel(2, 18, 10, 200u), uniform_channel(-1, 18, 10, 255u),
                       inner_mask_channel()};
    fixture.records.push_back(record);
  }
  { /* 4 Group 1 的文件夹记录：穿透 */
    FixtureRecord record;
    record.name = "Group 1";
    record.section_kind = 1;
    record.section_key = "pass";
    fixture.records.push_back(record);
  }
  { /* 5 Iso Group 的边界分隔记录 */
    FixtureRecord record;
    record.name = "</Layer group>";
    record.flags = 0x0A;
    record.section_kind = 3;
    record.section_key = "norm";
    fixture.records.push_back(record);
  }
  { /* 6 Iso Layer */
    FixtureRecord record;
    record.name = "Iso Layer";
    record.top = 18;
    record.left = 2;
    record.bottom = 24;
    record.right = 10;
    record.blend_key = "scrn";
    record.opacity = 77;
    record.channels = {uniform_channel(0, 8, 6, 50u), uniform_channel(1, 8, 6, 60u),
                       uniform_channel(2, 8, 6, 70u), uniform_channel(-1, 8, 6, 255u)};
    fixture.records.push_back(record);
  }
  { /* 7 Iso Group 的文件夹记录：隔离 + Multiply */
    FixtureRecord record;
    record.name = "Iso Group";
    record.section_kind = 1;
    record.section_key = "mul ";
    record.opacity = 128;
    fixture.records.push_back(record);
  }
  { /* 8 Hidden */
    FixtureRecord record;
    record.name = "Hidden";
    record.top = 2;
    record.left = 12;
    record.bottom = 8;
    record.right = 18;
    record.flags = 0x0A;
    record.channels = {uniform_channel(0, 6, 6, 7u), uniform_channel(1, 6, 6, 200u),
                       uniform_channel(2, 6, 6, 9u)};
    fixture.records.push_back(record);
  }
  return fixture;
}

/* ===================================================== 引擎 / 工作目录 */

comp_engine_desc psd_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-psd-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

std::string psd_workspace(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/psd", ec);
  return std::string("build/psd/") + name;
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
  std::vector<uint8_t> bytes;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return bytes;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<size_t>(size));
    const size_t read = std::fread(bytes.data(), 1u, bytes.size(), file);
    bytes.resize(read);
  }
  std::fclose(file);
  return bytes;
}

std::string read_text_file(const std::string& path) {
  const std::vector<uint8_t> bytes = read_file_bytes(path);
  return std::string(bytes.begin(), bytes.end());
}

/* 一个目录包的全部文件（相对路径 + 内容），按相对路径排序。 */
struct PackageFile {
  std::string relative_path;
  std::vector<uint8_t> bytes;
};

std::vector<PackageFile> read_package(const std::string& root) {
  std::vector<PackageFile> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    PackageFile file;
    file.relative_path = entry.path().lexically_relative(root).generic_string();
    file.bytes = read_file_bytes(entry.path().string());
    files.push_back(std::move(file));
  }
  std::sort(files.begin(), files.end(), [](const PackageFile& a, const PackageFile& b) {
    return a.relative_path < b.relative_path;
  });
  return files;
}

/* ===================================================== 图层只读快照（ABI） */

struct LayerView {
  comp_layer_id id = 0;
  std::string name;
  comp_rect transform{0, 0, 0, 0};
  float rotation = 0.0f;
  comp_blend_mode blend = COMP_BLEND_NORMAL;
  float opacity = 1.0f;
  int32_t visible = 1;
  int32_t isolated = 0;
  int32_t is_group = 0;
  comp_layer_id parent = 0;
  comp_layer_id clipping_source = 0;
  int32_t has_mask = 0;
  int32_t mask_width = 0;
  int32_t mask_height = 0;
  int32_t pixel_width = 0;
  int32_t pixel_height = 0;
  comp_pixel_format pixel_format = COMP_PIXEL_RGBA8;
  std::vector<uint8_t> pixels; /* 8 位工作副本 */
};

bool read_layer_view(comp_document document, comp_layer_id id, LayerView* view) {
  view->id = id;

  size_t required = 0;
  if (comp_layer_get_name(document, id, nullptr, 0, &required) != COMP_OK || required == 0u) {
    return false;
  }
  std::vector<char> name(required + 1u, '\0');
  if (comp_layer_get_name(document, id, name.data(), name.size(), &required) != COMP_OK) {
    return false;
  }
  view->name = std::string(name.data());

  if (comp_layer_get_transform(document, id, &view->transform, &view->rotation) != COMP_OK) {
    return false;
  }
  if (comp_layer_get_blend(document, id, &view->blend) != COMP_OK) return false;
  if (comp_layer_get_opacity(document, id, &view->opacity) != COMP_OK) return false;
  if (comp_layer_get_visible(document, id, &view->visible) != COMP_OK) return false;
  if (comp_layer_get_isolated(document, id, &view->isolated) != COMP_OK) return false;
  if (comp_layer_get_group_info(document, id, &view->is_group, &view->parent,
                                &view->clipping_source) != COMP_OK) {
    return false;
  }
  if (comp_layer_get_mask(document, id, &view->has_mask, &view->mask_width,
                          &view->mask_height) != COMP_OK) {
    return false;
  }

  if (comp_layer_get_pixels(document, id, nullptr, 0u, &view->pixel_width, &view->pixel_height,
                            &view->pixel_format) != COMP_OK) {
    return false;
  }
  if (view->pixel_width > 0 && view->pixel_height > 0) {
    const size_t stride = static_cast<size_t>(view->pixel_width) * 4u;
    view->pixels.assign(stride * static_cast<size_t>(view->pixel_height), 0u);
    if (comp_layer_get_pixels(document, id, view->pixels.data(), stride, &view->pixel_width,
                              &view->pixel_height, &view->pixel_format) != COMP_OK) {
      return false;
    }
  }
  return true;
}

bool read_layer_tree(comp_document document, std::vector<LayerView>* out) {
  size_t count = 0;
  if (comp_layer_count(document, &count) != COMP_OK) return false;
  std::vector<comp_layer_id> ids(count > 0 ? count : 1u, 0);
  size_t listed = 0;
  if (comp_layer_list(document, ids.data(), ids.size(), &listed) != COMP_OK) return false;
  if (listed != count) return false;
  ids.resize(listed);
  out->clear();
  out->reserve(ids.size());
  for (comp_layer_id id : ids) {
    LayerView view;
    if (!read_layer_view(document, id, &view)) return false;
    out->push_back(std::move(view));
  }
  return true;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

/* ===================================================== 断言用的文案辅助 */

std::string layer_tag(size_t index) { return "layer " + std::to_string(index); }

}  // namespace

/* ============================================================ 1. 解码 */

COMP_TEST(psd_decode_reads_layered_document) {
  const PsdFixture fixture = staged_fixture();
  const std::vector<uint8_t> bytes = build_psd(fixture);

  int32_t width = 0;
  int32_t height = 0;
  uint16_t depth = 0;
  std::string error;
  COMP_CHECK(compositor::psd_declared_size(bytes.data(), bytes.size(), &width, &height, &depth,
                                           &error));
  COMP_CHECK_MSG(width == 32 && height == 24 && depth == 8, "declared size 32x24 depth 8");

  PsdDocument document;
  const PsdStatus status =
      compositor::psd_decode_document(bytes.data(), bytes.size(), &document, &error);
  COMP_CHECK_MSG(status == PsdStatus::kOk, "decode staged psd | " + error);
  COMP_CHECK(document.width == 32 && document.height == 24 && document.depth == 8);
  COMP_CHECK_MSG(document.layers.size() == 7u,
                 "7 layers (two groups, one divider each) | got " +
                     std::to_string(document.layers.size()));

  /* ---- 图层表 ---- */
  COMP_CHECK(document.layers[0].name == "Background");
  COMP_CHECK(document.layers[1].name == "Clip");
  COMP_CHECK(document.layers[2].name == "Group 1");
  COMP_CHECK(document.layers[3].name == "Inner");
  COMP_CHECK(document.layers[4].name == "Iso Group");
  COMP_CHECK(document.layers[5].name == "Iso Layer");
  COMP_CHECK(document.layers[6].name == "Hidden");

  /* 组：边界分隔记录产生条目，文件夹记录给它补名字/混合/可见性。 */
  COMP_CHECK(document.layers[2].is_group);
  COMP_CHECK(document.layers[2].pass_through);
  COMP_CHECK(document.layers[2].blend == COMP_BLEND_NORMAL);
  COMP_CHECK(document.layers[4].is_group);
  COMP_CHECK(!document.layers[4].pass_through);
  COMP_CHECK(document.layers[4].blend == COMP_BLEND_MULTIPLY);
  COMP_CHECK(document.layers[4].opacity == 128u);

  /* 父级：Inner 在 Group 1 里，Iso Layer 在 Iso Group 里。 */
  COMP_CHECK(document.layers[3].parent == 2);
  COMP_CHECK(document.layers[5].parent == 4);
  COMP_CHECK(document.layers[0].parent == -1);

  /* 混合 / 不透明度 / 可见性。 */
  COMP_CHECK(document.layers[3].blend == COMP_BLEND_MULTIPLY);
  COMP_CHECK(document.layers[3].opacity == 200u);
  COMP_CHECK(document.layers[5].blend == COMP_BLEND_SCREEN);
  COMP_CHECK(document.layers[5].opacity == 77u);
  COMP_CHECK(!document.layers[6].visible);
  COMP_CHECK(document.layers[0].visible);

  /* 位置。 */
  COMP_CHECK(document.layers[0].bounds.x == 0 && document.layers[0].bounds.y == 0 &&
             document.layers[0].bounds.w == 32 && document.layers[0].bounds.h == 24);
  COMP_CHECK(document.layers[1].bounds.x == 4 && document.layers[1].bounds.y == 4 &&
             document.layers[1].bounds.w == 8 && document.layers[1].bounds.h == 8);
  COMP_CHECK(document.layers[3].bounds.x == 6 && document.layers[3].bounds.y == 6 &&
             document.layers[3].bounds.w == 18 && document.layers[3].bounds.h == 10);
  COMP_CHECK(document.layers[5].bounds.x == 2 && document.layers[5].bounds.y == 18 &&
             document.layers[5].bounds.w == 8 && document.layers[5].bounds.h == 6);
  COMP_CHECK(document.layers[6].bounds.x == 12 && document.layers[6].bounds.y == 2 &&
             document.layers[6].bounds.w == 6 && document.layers[6].bounds.h == 6);
  /* 组没有矩形。 */
  COMP_CHECK(document.layers[2].bounds.w == 0 && document.layers[2].bounds.h == 0);

  /* 剪贴：Clip 贴到它下方最近的非剪贴同级层（Background，下标 0）。 */
  COMP_CHECK(document.layers[1].clipping);
  COMP_CHECK(document.layers[1].clipping_source == 0);
  COMP_CHECK(!document.layers[0].clipping);
  COMP_CHECK(document.layers[0].clipping_source == -1);

  /* 像素：解码即**预乘**。10/20/30 配 alpha 200 -> 8/16/24。 */
  {
    const ImageBuffer& pixels = document.layers[0].pixels;
    COMP_CHECK(pixels.format == COMP_PIXEL_RGBA8);
    const uint8_t* row = pixels.row(0);
    COMP_CHECK_MSG(row[0] == 8 && row[1] == 16 && row[2] == 24 && row[3] == 200,
                   "premultiplied background pixel");
  }
  /* Clip：255 * 128 / 255 = 128。 */
  {
    const uint8_t* row = document.layers[1].pixels.row(0);
    COMP_CHECK(row[0] == 128 && row[1] == 0 && row[2] == 0 && row[3] == 128);
  }
  /* 没有 alpha 通道时按完全不透明收（Hidden 只有 0/1/2 三个通道）。 */
  {
    const uint8_t* row = document.layers[6].pixels.row(0);
    COMP_CHECK_MSG(row[3] == 255, "missing alpha channel means opaque");
    COMP_CHECK(row[0] == 7 && row[1] == 200 && row[2] == 9);
  }

  /* ---- 蒙版：从蒙版矩形 (8,7) 展开到图层矩形 (6,6)，outside 取默认色 0 ---- */
  {
    const PsdLayerData& inner = document.layers[3];
    COMP_CHECK(!inner.mask.empty());
    COMP_CHECK(inner.mask.width == 18 && inner.mask.height == 10);
    /* 逐像素按「layer(x,y) <- mask(x-2, y-1)」的语义核对。 */
    size_t mismatches = 0;
    for (int32_t y = 0; y < inner.mask.height; ++y) {
      for (int32_t x = 0; x < inner.mask.width; ++x) {
        const int32_t mx = x - 2;
        const int32_t my = y - 1;
        uint8_t expected = 0; /* 蒙版矩形外 = default_color */
        if (mx >= 0 && mx < 18 && my >= 0 && my < 10) {
          if (my == 0) {
            expected = 0;
          } else if (my == 1) {
            expected = (mx >= 2 && mx <= 8) ? 255 : 0;
          } else {
            expected = 255;
          }
        }
        if (inner.mask.row(y)[x] != expected) ++mismatches;
      }
    }
    COMP_CHECK_MSG(mismatches == 0, "mask expanded into layer pixel space");
    /* 几个手算的点，防止上面的公式与解码器一起写错。 */
    COMP_CHECK(inner.mask.row(0)[0] == 0);    /* mask(-2,-1) 在矩形外 */
    COMP_CHECK(inner.mask.row(2)[5] == 255);  /* mask(3,1) 落在 255 段 */
    COMP_CHECK(inner.mask.row(2)[0] == 0);    /* mask(-2,1) 在矩形外 */
    COMP_CHECK(inner.mask.row(2)[10] == 255); /* mask(8,1) 是 255 段的右端 */
    COMP_CHECK(inner.mask.row(2)[11] == 0);   /* mask(9,1) 已经出 255 段 */
    COMP_CHECK(inner.mask.row(9)[13] == 255); /* mask(11,8)：整行 255 */
  }
  /* 没有蒙版的层不带蒙版缓冲。 */
  COMP_CHECK(document.layers[0].mask.empty());
  COMP_CHECK(document.layers[1].mask.empty());
}

COMP_TEST(psd_decode_premultiplies_straight_alpha) {
  /* 2×1：R=[200,100] G=[50,0] B=[0,255] A=[128,0]。
   * 引擎内部是预乘，PSD 是直通，解码必须乘起来：
   *   (200*128+127)/255 = 100， (50*128+127)/255 = 25， (0*128+127)/255 = 0
   *   alpha = 0 的像素整体归零（预乘后颜色无意义）。 */
  PsdFixture fixture;
  fixture.width = 2;
  fixture.height = 1;
  FixtureRecord record;
  record.name = "Straight";
  record.bottom = 1;
  record.right = 2;
  record.channels = {uniform_channel(0, 2, 1, 0u), uniform_channel(1, 2, 1, 0u),
                     uniform_channel(2, 2, 1, 0u), uniform_channel(-1, 2, 1, 0u)};
  record.channels[0].samples = {200u, 100u};
  record.channels[1].samples = {50u, 0u};
  record.channels[2].samples = {0u, 255u};
  record.channels[3].samples = {128u, 0u};
  fixture.records.push_back(record);

  const std::vector<uint8_t> bytes = build_psd(fixture);
  PsdDocument document;
  std::string error;
  const PsdStatus status =
      compositor::psd_decode_document(bytes.data(), bytes.size(), &document, &error);
  COMP_CHECK_MSG(status == PsdStatus::kOk, "decode straight alpha psd | " + error);
  if (document.layers.size() != 1u) {
    COMP_CHECK_MSG(false, "expected exactly one layer");
    return;
  }
  const uint8_t* row = document.layers[0].pixels.row(0);
  COMP_CHECK_MSG(row[0] == 100 && row[1] == 25 && row[2] == 0 && row[3] == 128,
                 "pixel 0 premultiplied to (100,25,0,128)");
  COMP_CHECK_MSG(row[4] == 0 && row[5] == 0 && row[6] == 0 && row[7] == 0,
                 "fully transparent pixel stays zero");
}

COMP_TEST(psd_decode_16bit_uses_lr16) {
  /* 16 位文档：主 layer-info 长度 0，记录在 Lr16 附加块里。
   * 2×1，alpha = 32768（正好一半）：
   *   (20000*32768+32767)/65535 = 10000
   *   (10000*32768+32767)/65535 = 5000
   *   (  500*32768+32767)/65535 = 250
   * 第二个像素 alpha 65535 -> 原样。 */
  PsdFixture fixture;
  fixture.width = 2;
  fixture.height = 1;
  fixture.depth = 16;
  FixtureRecord record;
  record.name = "Deep";
  record.bottom = 1;
  record.right = 2;
  record.channels = {channel16(0, {20000u, 65535u}), channel16(1, {10000u, 0u}),
                     channel16(2, {500u, 65535u}), channel16(-1, {32768u, 65535u})};
  /* 第一条通道走 RLE，证明 16 位 RLE 也能解（行长度表按字节算）。 */
  record.channels[0].compression = compositor::kPsdCompressionRle;
  fixture.records.push_back(record);

  const std::vector<uint8_t> bytes = build_psd(fixture);
  PsdDocument document;
  std::string error;
  const PsdStatus status =
      compositor::psd_decode_document(bytes.data(), bytes.size(), &document, &error);
  COMP_CHECK_MSG(status == PsdStatus::kOk, "decode 16-bit psd | " + error);
  COMP_CHECK(document.depth == 16);
  if (document.layers.size() != 1u) {
    COMP_CHECK_MSG(false, "expected exactly one layer");
    return;
  }
  const ImageBuffer& pixels = document.layers[0].pixels;
  COMP_CHECK(pixels.format == COMP_PIXEL_RGBA16);
  COMP_CHECK(pixels.width == 2 && pixels.height == 1);
  const uint16_t* row = reinterpret_cast<const uint16_t*>(pixels.row(0));
  COMP_CHECK_MSG(row[0] == 10000u && row[1] == 5000u && row[2] == 250u && row[3] == 32768u,
                 "16-bit premultiplied pixel");
  COMP_CHECK_MSG(row[4] == 65535u && row[5] == 0u && row[6] == 65535u && row[7] == 65535u,
                 "opaque 16-bit pixel is untouched");
}

/* ============================================================ 2. ABI 导入 */

COMP_TEST(psd_import_document_exposes_layer_tree) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const std::string path = psd_workspace("staged.psd");
  {
    const std::vector<uint8_t> bytes = build_psd(staged_fixture());
    std::FILE* file = std::fopen(path.c_str(), "wb");
    COMP_CHECK(file != nullptr);
    if (file != nullptr) {
      const size_t written = std::fwrite(bytes.data(), 1u, bytes.size(), file);
      COMP_CHECK(written == bytes.size());
      std::fclose(file);
    }
  }

  comp_document document = nullptr;
  const comp_result imported = comp_io_import_document(engine, path.c_str(), &document);
  COMP_CHECK_MSG(imported == COMP_OK, "comp_io_import_document(psd)");
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  comp_size size{};
  COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
  COMP_CHECK(size.width == 32 && size.height == 24);
  int32_t dirty = 1;
  COMP_CHECK(comp_document_is_dirty(document, &dirty) == COMP_OK);
  COMP_CHECK_MSG(dirty == 0, "importing a file is not an edit");

  std::vector<LayerView> layers;
  COMP_CHECK_MSG(read_layer_tree(document, &layers), "read layer tree through the ABI");
  COMP_CHECK_MSG(layers.size() == 7u, "7 layers through the ABI");
  if (layers.size() != 7u) {
    comp_document_destroy(document);
    comp_engine_destroy(engine);
    return;
  }

  /* id 按图层表顺序 1..7 分配（导入是新建文档，没有历史 id）。 */
  for (size_t i = 0; i < layers.size(); ++i) {
    COMP_CHECK_MSG(layers[i].id == static_cast<comp_layer_id>(i + 1u),
                   layer_tag(i) + " id is table index + 1");
  }

  /* 名字 / 组 / 父级 / 剪贴。 */
  COMP_CHECK(layers[0].name == "Background");
  COMP_CHECK(layers[3].name == "Inner");
  COMP_CHECK(layers[2].is_group == 1);
  COMP_CHECK(layers[4].is_group == 1);
  COMP_CHECK(layers[3].parent == 3);
  COMP_CHECK(layers[5].parent == 5);
  COMP_CHECK(layers[1].clipping_source == 1);
  COMP_CHECK(layers[0].clipping_source == 0);
  COMP_CHECK(layers[2].parent == 0);

  /* **M1 的第 3 条**：穿透组 -> 不隔离，隔离组 -> isolated=1。
   * 这条如果错了，「打开带组的 PSD 再存回去」合成结果就变了。 */
  COMP_CHECK_MSG(layers[2].isolated == 0, "pass-through group is not isolated");
  COMP_CHECK_MSG(layers[4].isolated == 1, "non-pass folder is an isolated group");
  COMP_CHECK(layers[4].blend == COMP_BLEND_MULTIPLY);

  /* 位置 / 不透明度 / 混合 / 可见性。 */
  COMP_CHECK(layers[0].transform.x == 0 && layers[0].transform.y == 0 &&
             layers[0].transform.w == 32 && layers[0].transform.h == 24);
  COMP_CHECK(layers[3].transform.x == 6 && layers[3].transform.y == 6 &&
             layers[3].transform.w == 18 && layers[3].transform.h == 10);
  COMP_CHECK(layers[2].transform.w == 0 && layers[2].transform.h == 0);
  COMP_CHECK_MSG(near(layers[3].opacity, 200.0f / 255.0f), "opacity 200/255 survives the ABI");
  COMP_CHECK_MSG(near(layers[5].opacity, 77.0f / 255.0f), "opacity 77/255 survives the ABI");
  COMP_CHECK(layers[3].blend == COMP_BLEND_MULTIPLY);
  COMP_CHECK(layers[5].blend == COMP_BLEND_SCREEN);
  COMP_CHECK(layers[6].visible == 0);
  COMP_CHECK(layers[5].visible == 1);

  /* 蒙版尺寸走 ABI 之后仍在图层像素空间。 */
  COMP_CHECK(layers[3].has_mask == 1);
  COMP_CHECK(layers[3].mask_width == 18 && layers[3].mask_height == 10);
  COMP_CHECK(layers[0].has_mask == 0);

  /* 像素只读接口：组报 0×0，像素层报自己的尺寸与**预乘**内容。 */
  COMP_CHECK(layers[2].pixel_width == 0 && layers[2].pixel_height == 0);
  COMP_CHECK(layers[3].pixel_width == 18 && layers[3].pixel_height == 10);
  COMP_CHECK(layers[0].pixel_width == 32 && layers[0].pixel_height == 24);
  COMP_CHECK(layers[0].pixel_format == COMP_PIXEL_RGBA8);
  COMP_CHECK_MSG(!layers[0].pixels.empty() && layers[0].pixels[0] == 8 && layers[0].pixels[1] == 16 &&
                     layers[0].pixels[2] == 24 && layers[0].pixels[3] == 200,
                 "premultiplied pixel survives the ABI");
  COMP_CHECK_MSG(layers[6].pixels.size() == 6u * 6u * 4u && layers[6].pixels[3] == 255,
                 "hidden layer keeps opaque alpha through the ABI");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

/* ==================================================== 3. .comp 往返回合 */

COMP_TEST(psd_comp_round_trip_is_byte_exact) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const std::string psd_path = psd_workspace("round_trip.psd");
  {
    const std::vector<uint8_t> bytes = build_psd(staged_fixture());
    std::FILE* file = std::fopen(psd_path.c_str(), "wb");
    COMP_CHECK(file != nullptr);
    if (file != nullptr) {
      std::fwrite(bytes.data(), 1u, bytes.size(), file);
      std::fclose(file);
    }
  }

  const std::string first_path = psd_workspace("round_trip_a.comp");
  const std::string second_path = psd_workspace("round_trip_b.comp");
  std::error_code ec;
  std::filesystem::remove_all(first_path, ec);
  std::filesystem::remove_all(second_path, ec);

  comp_document imported = nullptr;
  COMP_CHECK_MSG(comp_io_import_document(engine, psd_path.c_str(), &imported) == COMP_OK,
                 "import psd before .comp round trip");
  if (imported == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  std::vector<LayerView> before;
  COMP_CHECK(read_layer_tree(imported, &before));
  std::vector<uint8_t> flatten_before(32u * 24u * 4u, 0u);
  COMP_CHECK(comp_render_flatten(imported, flatten_before.data(), COMP_PIXEL_RGBA8, 32u * 4u) ==
             COMP_OK);

  COMP_CHECK_MSG(comp_document_save(imported, first_path.c_str()) == COMP_OK, "save A");
  comp_document_destroy(imported);

  comp_document reloaded = nullptr;
  COMP_CHECK_MSG(comp_document_load(engine, first_path.c_str(), &reloaded) == COMP_OK, "load A");
  if (reloaded == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  /* 往返后图层数 / 位置 / 不透明度 / 蒙版 / 组关系逐项一致。 */
  std::vector<LayerView> after;
  COMP_CHECK(read_layer_tree(reloaded, &after));
  COMP_CHECK_MSG(after.size() == before.size(), "layer count survives .comp");
  if (after.size() == before.size()) {
    for (size_t i = 0; i < before.size(); ++i) {
      const std::string tag = layer_tag(i) + " '" + before[i].name + "'";
      COMP_CHECK_MSG(after[i].id == before[i].id, tag + " id");
      COMP_CHECK_MSG(after[i].name == before[i].name, tag + " name");
      /* 组没有矩形（PSD 的分隔记录也是 0 矩形），.comp 的 transform 尺寸把 <=0
       * 规范化成 1（clamp_transform_size），所以组的 w/h 不是可往返的量——只有
       * 原点有意义。像素层必须逐字段一致。 */
      if (before[i].is_group == 1) {
        COMP_CHECK_MSG(after[i].transform.x == before[i].transform.x &&
                           after[i].transform.y == before[i].transform.y,
                       tag + " origin");
      } else {
        COMP_CHECK_MSG(after[i].transform.x == before[i].transform.x &&
                           after[i].transform.y == before[i].transform.y &&
                           after[i].transform.w == before[i].transform.w &&
                           after[i].transform.h == before[i].transform.h,
                       tag + " transform");
      }
      COMP_CHECK_MSG(near(after[i].opacity, before[i].opacity), tag + " opacity");
      COMP_CHECK_MSG(after[i].blend == before[i].blend, tag + " blend");
      COMP_CHECK_MSG(after[i].visible == before[i].visible, tag + " visible");
      COMP_CHECK_MSG(after[i].isolated == before[i].isolated, tag + " isolated");
      COMP_CHECK_MSG(after[i].is_group == before[i].is_group, tag + " is_group");
      COMP_CHECK_MSG(after[i].parent == before[i].parent, tag + " parent");
      COMP_CHECK_MSG(after[i].clipping_source == before[i].clipping_source, tag + " clipping");
      COMP_CHECK_MSG(after[i].has_mask == before[i].has_mask, tag + " has_mask");
      COMP_CHECK_MSG(after[i].mask_width == before[i].mask_width &&
                         after[i].mask_height == before[i].mask_height,
                     tag + " mask size");
      COMP_CHECK_MSG(after[i].pixel_width == before[i].pixel_width &&
                         after[i].pixel_height == before[i].pixel_height,
                     tag + " pixel size");
      COMP_CHECK_MSG(after[i].pixels == before[i].pixels, tag + " pixel bytes");
    }
  }

  /* 合成结果逐字节一致（蒙版像素的间接比对：蒙版错一个像素，flatten 就变）。 */
  std::vector<uint8_t> flatten_after(32u * 24u * 4u, 0u);
  COMP_CHECK(comp_render_flatten(reloaded, flatten_after.data(), COMP_PIXEL_RGBA8, 32u * 4u) ==
             COMP_OK);
  COMP_CHECK_MSG(flatten_before == flatten_after, "flatten is byte-exact after .comp round trip");

  COMP_CHECK_MSG(comp_document_save(reloaded, second_path.c_str()) == COMP_OK, "save B");
  comp_document_destroy(reloaded);

  /* 整个目录包逐文件逐字节一致：manifest 与每张 PNG（含蒙版 PNG）都必须相同。
   * 这同时钉住「保存是确定性的」——不写时间戳、图层 uuid 由 documentID 派生。 */
  const std::vector<PackageFile> package_a = read_package(first_path);
  const std::vector<PackageFile> package_b = read_package(second_path);
  COMP_CHECK_MSG(!package_a.empty(), "package A has files");
  COMP_CHECK_MSG(package_a.size() == package_b.size(),
                 "package file count: " + std::to_string(package_a.size()) + " vs " +
                     std::to_string(package_b.size()));
  if (package_a.size() == package_b.size()) {
    for (size_t i = 0; i < package_a.size(); ++i) {
      COMP_CHECK_MSG(package_a[i].relative_path == package_b[i].relative_path,
                     "package entry name " + std::to_string(i) + ": " + package_a[i].relative_path +
                         " vs " + package_b[i].relative_path);
      COMP_CHECK_MSG(package_a[i].bytes == package_b[i].bytes,
                     "package entry bytes: " + package_a[i].relative_path);
    }
  }

  comp_engine_destroy(engine);
}

/* -------------------------------- M1 第 3 条：组的混合模式与「穿透」要能落盘 */

COMP_TEST(comp_file_round_trips_group_blend_mode) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const std::string psd_path = psd_workspace("group_blend.psd");
  {
    const std::vector<uint8_t> bytes = build_psd(staged_fixture());
    std::FILE* file = std::fopen(psd_path.c_str(), "wb");
    COMP_CHECK(file != nullptr);
    if (file != nullptr) {
      std::fwrite(bytes.data(), 1u, bytes.size(), file);
      std::fclose(file);
    }
  }

  const std::string comp_path = psd_workspace("group_blend.comp");
  std::error_code ec;
  std::filesystem::remove_all(comp_path, ec);

  comp_document document = nullptr;
  COMP_CHECK(comp_io_import_document(engine, psd_path.c_str(), &document) == COMP_OK);
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }
  COMP_CHECK(comp_document_save(document, comp_path.c_str()) == COMP_OK);

  /* manifest 里必须有引擎扩展键：隔离组的混合模式 + 组隔离状态。
   * 少了它们，「穿透的 Multiply 组」读回来就变成「隔离的 Normal 组」。 */
  const std::string manifest = read_text_file(comp_path + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("engineIsolated") != std::string::npos,
                 "manifest carries engineIsolated for the isolated group");
  COMP_CHECK_MSG(manifest.find("engineBlendMode") != std::string::npos,
                 "manifest carries engineBlendMode for the group blend");
  COMP_CHECK_MSG(manifest.find("Multiply") != std::string::npos,
                 "manifest records the group's Multiply blend");

  comp_document_destroy(document);

  comp_document loaded = nullptr;
  COMP_CHECK(comp_document_load(engine, comp_path.c_str(), &loaded) == COMP_OK);
  if (loaded == nullptr) {
    comp_engine_destroy(engine);
    return;
  }
  std::vector<LayerView> layers;
  COMP_CHECK(read_layer_tree(loaded, &layers));
  COMP_CHECK(layers.size() == 7u);
  if (layers.size() == 7u) {
    COMP_CHECK_MSG(layers[2].isolated == 0, "pass-through group is still pass-through");
    COMP_CHECK_MSG(layers[2].blend == COMP_BLEND_NORMAL, "pass-through group keeps Normal blend");
    COMP_CHECK_MSG(layers[4].isolated == 1, "isolated group is still isolated");
    COMP_CHECK_MSG(layers[4].blend == COMP_BLEND_MULTIPLY, "isolated group keeps Multiply");
  }
  comp_document_destroy(loaded);

  /* 普通新文档不带这两个键（不写无意义的默认值，manifest 与上游保持同形）。 */
  const std::string plain_path = psd_workspace("plain.comp");
  std::filesystem::remove_all(plain_path, ec);
  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  document_desc.color_space = "sRGB";
  comp_document plain = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &plain) == COMP_OK);
  if (plain != nullptr) {
    COMP_CHECK(comp_document_save(plain, plain_path.c_str()) == COMP_OK);
    const std::string plain_manifest = read_text_file(plain_path + "/manifest.json");
    COMP_CHECK_MSG(plain_manifest.find("engineIsolated") == std::string::npos,
                   "plain document has no engineIsolated");
    COMP_CHECK_MSG(plain_manifest.find("engineBlendMode") == std::string::npos,
                   "plain document has no engineBlendMode");
    comp_document_destroy(plain);
  }

  comp_engine_destroy(engine);
}

/* ============================================================ 4. 导出 */

COMP_TEST(psd_export_round_trip_preserves_layers) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  const std::string psd_path = psd_workspace("export_source.psd");
  const std::vector<uint8_t> fixture_bytes = build_psd(staged_fixture());
  {
    std::FILE* file = std::fopen(psd_path.c_str(), "wb");
    COMP_CHECK(file != nullptr);
    if (file != nullptr) {
      std::fwrite(fixture_bytes.data(), 1u, fixture_bytes.size(), file);
      std::fclose(file);
    }
  }

  comp_document document = nullptr;
  COMP_CHECK(comp_io_import_document(engine, psd_path.c_str(), &document) == COMP_OK);
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  const std::string exported_path = psd_workspace("export_round_trip.psd");
  const comp_result exported =
      comp_io_export_document(document, exported_path.c_str(), COMP_FORMAT_PSD, nullptr);
  COMP_CHECK_MSG(exported == COMP_OK, "comp_io_export_document(PSD)");
  comp_document_destroy(document);
  if (exported != COMP_OK) {
    comp_engine_destroy(engine);
    return;
  }

  /* 原件与导出件都用同一个解码器读，逐字段比对（不借用 ABI，避免双份解释）。 */
  PsdDocument original;
  PsdDocument written;
  std::string error;
  COMP_CHECK(compositor::psd_decode_document(fixture_bytes.data(), fixture_bytes.size(), &original,
                                             &error) == PsdStatus::kOk);
  const std::vector<uint8_t> written_bytes = read_file_bytes(exported_path);
  COMP_CHECK(!written_bytes.empty());
  COMP_CHECK_MSG(compositor::psd_decode_document(written_bytes.data(), written_bytes.size(), &written,
                                                 &error) == PsdStatus::kOk,
                 "re-decode the exported psd | " + error);

  COMP_CHECK(written.width == original.width && written.height == original.height &&
             written.depth == original.depth);
  COMP_CHECK_MSG(written.layers.size() == original.layers.size(),
                 "exported layer count " + std::to_string(written.layers.size()) + " vs " +
                     std::to_string(original.layers.size()));

  if (written.layers.size() == original.layers.size()) {
    int worst_delta = 0;
    for (size_t i = 0; i < original.layers.size(); ++i) {
      const PsdLayerData& before = original.layers[i];
      const PsdLayerData& after = written.layers[i];
      const std::string tag = layer_tag(i) + " '" + before.name + "'";
      COMP_CHECK_MSG(after.name == before.name, tag + " name");
      COMP_CHECK_MSG(after.bounds.x == before.bounds.x && after.bounds.y == before.bounds.y &&
                         after.bounds.w == before.bounds.w && after.bounds.h == before.bounds.h,
                     tag + " bounds");
      COMP_CHECK_MSG(after.opacity == before.opacity, tag + " opacity");
      COMP_CHECK_MSG(after.blend == before.blend, tag + " blend");
      COMP_CHECK_MSG(after.visible == before.visible, tag + " visible");
      COMP_CHECK_MSG(after.is_group == before.is_group, tag + " is_group");
      COMP_CHECK_MSG(after.pass_through == before.pass_through, tag + " pass_through");
      COMP_CHECK_MSG(after.parent == before.parent, tag + " parent");
      COMP_CHECK_MSG(after.clipping == before.clipping, tag + " clipping flag");
      COMP_CHECK_MSG(after.clipping_source == before.clipping_source, tag + " clipping source");
      COMP_CHECK_MSG(after.pixels.width == before.pixels.width &&
                         after.pixels.height == before.pixels.height,
                     tag + " pixel size");
      COMP_CHECK_MSG(after.mask.width == before.mask.width &&
                         after.mask.height == before.mask.height,
                     tag + " mask size");

      /* 像素：PSD 写的是直通 alpha，导出时反预乘、读回来再预乘。
       * 8 位下的这一对取整最多差 1（BG 的蓝通道就是 24 -> 31 -> 24 这条路径）。 */
      if (after.pixels.byte_size() == before.pixels.byte_size()) {
        for (size_t k = 0; k < before.pixels.byte_size(); ++k) {
          const int delta = static_cast<int>(after.pixels.pixel_data()[k]) -
                            static_cast<int>(before.pixels.pixel_data()[k]);
          const int magnitude = delta < 0 ? -delta : delta;
          if (magnitude > worst_delta) worst_delta = magnitude;
        }
      } else {
        COMP_CHECK_MSG(false, tag + " pixel byte size differs");
      }

      /* 蒙版必须逐字节一致（导出写成「与图层同矩形」，读回展开是恒等映射）。 */
      if (after.mask.empty() != before.mask.empty()) {
        COMP_CHECK_MSG(false, tag + " mask presence differs");
      } else if (!before.mask.empty()) {
        bool same_mask = after.mask.width == before.mask.width &&
                         after.mask.height == before.mask.height;
        for (int32_t y = 0; same_mask && y < before.mask.height; ++y) {
          same_mask = std::memcmp(after.mask.row(y), before.mask.row(y),
                                  static_cast<size_t>(before.mask.width)) == 0;
        }
        COMP_CHECK_MSG(same_mask, tag + " mask bytes");
      }
    }
    COMP_CHECK_MSG(worst_delta <= 1, "export round trip max pixel delta " +
                                         std::to_string(worst_delta) + " (must be <= 1)");
  }

  comp_engine_destroy(engine);
}

COMP_TEST(psd_export_rejects_unrepresentable_documents) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  const std::string path = psd_workspace("unrepresentable.psd");

  const auto export_result = [&](comp_document document) {
    return comp_io_export_document(document, path.c_str(), COMP_FORMAT_PSD, nullptr);
  };

  comp_layer_desc layer_desc{};
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;

  /* ---- 旋转 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id id = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &id) == COMP_OK);
      COMP_CHECK(comp_layer_set_transform(document, id, comp_rect{0, 0, 8, 8}, 45.0f) == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "rotated layer must be rejected, not silently baked");
      comp_document_destroy(document);
    }
  }

  /* ---- 翻转 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id id = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &id) == COMP_OK);
      COMP_CHECK(comp_layer_set_flip(document, id, 1, 0) == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "flipped layer must be rejected");
      comp_document_destroy(document);
    }
  }

  /* ---- 缩放（变换尺寸与像素尺寸不一致） ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id id = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &id) == COMP_OK);
      COMP_CHECK(comp_layer_set_transform(document, id, comp_rect{0, 0, 4, 4}, 0.0f) == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "scaled layer must be rejected");
      comp_document_destroy(document);
    }
  }

  /* ---- 文字图层 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 32;
    document_desc.height = 32;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_text_desc text{};
      text.text = "Psd";
      text.text_length = 3;
      text.origin_x = 0.0;
      text.origin_y = 0.0;
      text.style.font_size = 8.0;
      text.style.line_height = 1.2;
      text.style.letter_spacing = 0.0;
      text.style.antialias = 1;
      text.color[0] = 255;
      text.color[1] = 255;
      text.color[2] = 255;
      text.color[3] = 255;
      text.opacity = 1.0f;
      text.font_path_utf8 = nullptr;
      text.font_face_index = 0;
      text._reserved = 0;
      comp_layer_id id = 0;
      const comp_result added = comp_layer_add_text(document, &text, &id);
      COMP_CHECK_MSG(added == COMP_OK, "create a text layer for the export test");
      if (added == COMP_OK) {
        COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                       "text layer must be rejected, not rasterized silently");
      }
      comp_document_destroy(document);
    }
  }

  /* ---- 调整图层参数 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id id = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &id) == COMP_OK);
      /* kind 0 的缺省 settings 就是 "{}"：settings 会按参数表校验，这里只关心
       * 「图层带了调整参数」这件事本身。 */
      COMP_CHECK(comp_layer_add_adjustment(document, id, 0u, nullptr) == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "adjustment parameters must be rejected");
      comp_document_destroy(document);
    }
  }

  /* ---- 图层效果 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id id = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &id) == COMP_OK);
      COMP_CHECK(comp_layer_set_effect(document, id, 0u, "{\"width\":2}") == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "layer effects must be rejected");
      comp_document_destroy(document);
    }
  }

  /* ---- 穿透组 + 非 Normal 混合（PSD 的透视组没有自己的混合模式） ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_desc group_desc = layer_desc;
      group_desc.is_group = 1;
      comp_layer_id group = 0;
      COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);
      COMP_CHECK(comp_layer_set_isolated(document, group, 0) == COMP_OK);
      COMP_CHECK(comp_layer_set_blend(document, group, COMP_BLEND_MULTIPLY) == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "pass-through group with a blend mode must be rejected");
      comp_document_destroy(document);
    }
  }

  /* ---- 组带像素 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_desc group_desc = layer_desc;
      group_desc.is_group = 1;
      comp_layer_id group = 0;
      COMP_CHECK(comp_layer_add(document, &group_desc, &group) == COMP_OK);
      std::vector<uint8_t> pixels(8u * 8u * 4u, 0u);
      comp_image image{};
      image.width = 8;
      image.height = 8;
      image.format = COMP_PIXEL_RGBA8;
      image.stride = 8u * 4u;
      image.pixels = pixels.data();
      const comp_result set = comp_layer_set_pixels(document, group, &image);
      /* 组不接受像素是更早的一道闸；这里只要确认「无论哪一层拦下，导出都不会
       * 悄悄把像素丢掉」。 */
      if (set == COMP_OK) {
        COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                       "group carrying pixels must be rejected");
      }
      comp_document_destroy(document);
    }
  }

  /* ---- 剪贴来源不相邻 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 8;
    document_desc.height = 8;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id base = 0;
      comp_layer_id middle = 0;
      comp_layer_id top = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &base) == COMP_OK);
      COMP_CHECK(comp_layer_add(document, &layer_desc, &middle) == COMP_OK);
      COMP_CHECK(comp_layer_add(document, &layer_desc, &top) == COMP_OK);
      COMP_CHECK(comp_layer_set_clipping_source(document, top, base) == COMP_OK);
      COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                     "non-adjacent clipping must be rejected");
      comp_document_destroy(document);
    }
  }

  /* ---- HDR（半精度）图层 ---- */
  {
    comp_document_desc document_desc{};
    document_desc.width = 4;
    document_desc.height = 4;
    document_desc.color_space = "sRGB";
    comp_document document = nullptr;
    COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
    if (document != nullptr) {
      comp_layer_id id = 0;
      COMP_CHECK(comp_layer_add(document, &layer_desc, &id) == COMP_OK);
      std::vector<uint8_t> pixels(4u * 4u * 8u, 0u);
      comp_image image{};
      image.width = 4;
      image.height = 4;
      image.format = COMP_PIXEL_RGBA16F;
      image.stride = 4u * 8u;
      image.pixels = pixels.data();
      const comp_result set = comp_layer_set_pixels(document, id, &image);
      if (set == COMP_OK) {
        COMP_CHECK_MSG(export_result(document) == COMP_ERR_UNSUPPORTED,
                       "HDR layer must be rejected, not truncated");
      }
      comp_document_destroy(document);
    }
  }

  comp_engine_destroy(engine);
}

/* ==================================================== 5. 明确的拒绝路径 */

COMP_TEST(psd_decode_rejects_unsupported_variants) {
  const auto expect = [](const std::vector<uint8_t>& bytes, PsdStatus want, const char* keyword,
                         const std::string& label) {
    PsdDocument document;
    std::string error;
    const PsdStatus got =
        compositor::psd_decode_document(bytes.data(), bytes.size(), &document, &error);
    COMP_CHECK_MSG(got == want, label + " [status=" + compositor::psd_status_name(got) +
                                    ", error=" + error + "]");
    if (keyword != nullptr) {
      COMP_CHECK_MSG(error.find(keyword) != std::string::npos,
                     label + " [error should mention '" + keyword + "', got: " + error + "]");
    }
  };

  const PsdFixture staged = staged_fixture();

  /* ---- 文件头 ---- */
  { /* PSB */
    std::vector<uint8_t> bytes = build_psd(staged);
    patch16(&bytes, 4, 2);
    expect(bytes, PsdStatus::kUnsupported, "PSB", "PSB (version 2)");
  }
  { /* 32 位通道 */
    std::vector<uint8_t> bytes = build_psd(staged);
    patch16(&bytes, 22, 32);
    expect(bytes, PsdStatus::kUnsupported, "8 位与 16 位", "32-bit channels");
  }
  { /* 12 位通道 */
    std::vector<uint8_t> bytes = build_psd(staged);
    patch16(&bytes, 22, 12);
    expect(bytes, PsdStatus::kUnsupported, "8 位与 16 位", "12-bit channels");
  }
  { /* CMYK */
    std::vector<uint8_t> bytes = build_psd(staged);
    patch16(&bytes, 24, 4);
    expect(bytes, PsdStatus::kUnsupported, "CMYK", "CMYK color mode");
  }
  { /* 通道数不足 */
    std::vector<uint8_t> bytes = build_psd(staged);
    patch16(&bytes, 12, 2);
    expect(bytes, PsdStatus::kUnsupported, "小于 3", "two channels");
  }
  { /* 截断 */
    std::vector<uint8_t> bytes = build_psd(staged);
    bytes.resize(30);
    expect(bytes, PsdStatus::kIo, "截断", "truncated header sections");
  }

  /* ---- 通道层 ---- */
  {
    PsdFixture fixture = staged;
    fixture.records[0].channels[0].compression = 2;
    expect(build_psd(fixture), PsdStatus::kUnsupported, "ZIP", "ZIP channel compression");
  }
  {
    PsdFixture fixture = staged;
    FixtureChannel extra;
    extra.id = -3;
    extra.samples.assign(32u * 24u, 0u);
    fixture.records[0].channels.push_back(extra);
    expect(build_psd(fixture), PsdStatus::kUnsupported, "真实蒙版", "channel id -3");
  }
  {
    PsdFixture fixture = staged;
    FixtureChannel extra;
    extra.id = 3;
    extra.samples.assign(32u * 24u, 0u);
    fixture.records[0].channels.push_back(extra);
    expect(build_psd(fixture), PsdStatus::kUnsupported, "额外通道", "channel id 3");
  }

  /* ---- 记录层 ---- */
  {
    PsdFixture fixture = staged;
    fixture.records[0].blend_key = "diss";
    expect(build_psd(fixture), PsdStatus::kUnsupported, "混合模式", "diss blend mode");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[0].blend_key = "pass"; /* 组专用键写在普通层上 */
    expect(build_psd(fixture), PsdStatus::kIo, "pass", "pass key on a plain layer");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[0].flags = 0x18; /* pixel data irrelevant */
    expect(build_psd(fixture), PsdStatus::kUnsupported, "像素无关", "pixel-irrelevant flag");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[1].clipping = 2;
    expect(build_psd(fixture), PsdStatus::kIo, "剪贴", "clipping flag out of range");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[3].extra_tags.push_back({"TySh", {0, 0, 0, 0}});
    expect(build_psd(fixture), PsdStatus::kUnsupported, "文字层", "TySh text layer tag");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[3].extra_tags.push_back({"lmfx", {0, 0, 0, 0}});
    expect(build_psd(fixture), PsdStatus::kUnsupported, "图层特效", "layer effects tag");
  }

  /* ---- 蒙版一致性 ---- */
  {
    PsdFixture fixture = staged; /* 有蒙版块但没有 -2 通道 */
    auto& channels = fixture.records[3].channels;
    channels.erase(std::remove_if(channels.begin(), channels.end(),
                                  [](const FixtureChannel& channel) { return channel.id == -2; }),
                   channels.end());
    expect(build_psd(fixture), PsdStatus::kIo, "不一致", "mask block without -2 channel");
  }
  {
    PsdFixture fixture = staged; /* 有 -2 通道但没有蒙版块 */
    fixture.records[3].has_mask_block = false;
    expect(build_psd(fixture), PsdStatus::kIo, "不一致", "-2 channel without mask block");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[3].mask_flags = 0x10; /* MaskParameters：羽化/密度 */
    expect(build_psd(fixture), PsdStatus::kUnsupported, "羽化", "mask with feather/density");
  }
  {
    PsdFixture fixture = staged;
    fixture.records[3].mask_block_length = 36; /* 带未识别前缀 */
    expect(build_psd(fixture), PsdStatus::kUnsupported, "前缀", "oversized mask block");
  }

  /* ---- 组结构 ---- */
  {
    PsdFixture fixture = staged; /* 删掉 Group 1 的文件夹记录：只剩开始标记 */
    fixture.records.erase(fixture.records.begin() + 4);
    expect(build_psd(fixture), PsdStatus::kIo, "没有对应的结束标记", "unbalanced group");
  }
}

COMP_TEST(psd_import_document_rejects_flat_file) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  /* 已拍平的 PSD：layer/mask 段长度 0。codec 认为这是合法的「0 个图层」，
   * 但整篇导入必须拒绝——它的内容全在那份我们不读的合并图里。 */
  PsdFixture fixture;
  fixture.flat = true;
  const std::vector<uint8_t> bytes = build_psd(fixture);

  PsdDocument decoded;
  std::string error;
  COMP_CHECK_MSG(compositor::psd_decode_document(bytes.data(), bytes.size(), &decoded, &error) ==
                     PsdStatus::kOk,
                 "a flat psd is not a codec error");
  COMP_CHECK(decoded.layers.empty());

  const std::string path = psd_workspace("flat.psd");
  std::FILE* file = std::fopen(path.c_str(), "wb");
  COMP_CHECK(file != nullptr);
  if (file != nullptr) {
    std::fwrite(bytes.data(), 1u, bytes.size(), file);
    std::fclose(file);
  }

  comp_document document = reinterpret_cast<comp_document>(static_cast<intptr_t>(1));
  const comp_result imported = comp_io_import_document(engine, path.c_str(), &document);
  COMP_CHECK_MSG(imported == COMP_ERR_UNSUPPORTED, "flat psd import is UNSUPPORTED");
  COMP_CHECK_MSG(document == nullptr, "no half-built document is handed out");
  const char* last_error = comp_engine_last_error(engine);
  COMP_CHECK(last_error != nullptr);
  if (last_error != nullptr) {
    COMP_CHECK_MSG(std::string(last_error).find("已拍平") != std::string::npos,
                   std::string("last_error explains the flat file: ") + last_error);
  }

  comp_engine_destroy(engine);
}

COMP_TEST(psd_import_document_delegates_non_psd) {
  comp_engine_desc desc = psd_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  /* 单图层格式没有「整篇」语义：两个入口必须完全同义（不做第二套解码）。 */
  comp_document_desc document_desc{};
  document_desc.width = 6;
  document_desc.height = 4;
  document_desc.color_space = "sRGB";
  comp_document source = nullptr;
  COMP_CHECK(comp_document_create(engine, &document_desc, &source) == COMP_OK);

  const std::string png_path = psd_workspace("delegate.png");
  const comp_result exported = comp_io_export(source, png_path.c_str(), COMP_FORMAT_PNG, nullptr);
  COMP_CHECK(exported == COMP_OK);
  comp_document_destroy(source);

  if (exported != COMP_OK) {
    comp_engine_destroy(engine);
    return;
  }

  comp_document single = nullptr;
  comp_document whole = nullptr;
  COMP_CHECK(comp_io_import(engine, png_path.c_str(), &single) == COMP_OK);
  COMP_CHECK(comp_io_import_document(engine, png_path.c_str(), &whole) == COMP_OK);

  if (single != nullptr && whole != nullptr) {
    comp_size single_size{};
    comp_size whole_size{};
    COMP_CHECK(comp_document_size(single, &single_size) == COMP_OK);
    COMP_CHECK(comp_document_size(whole, &whole_size) == COMP_OK);
    COMP_CHECK(whole_size.width == single_size.width && whole_size.height == single_size.height);

    size_t single_count = 0;
    size_t whole_count = 0;
    COMP_CHECK(comp_layer_count(single, &single_count) == COMP_OK);
    COMP_CHECK(comp_layer_count(whole, &whole_count) == COMP_OK);
    COMP_CHECK_MSG(whole_count == single_count, "comp_io_import_document == comp_io_import for png");

    std::vector<uint8_t> a(static_cast<size_t>(single_size.width) *
                               static_cast<size_t>(single_size.height) * 4u,
                           0u);
    std::vector<uint8_t> b(a.size(), 0u);
    COMP_CHECK(comp_render_flatten(single, a.data(), COMP_PIXEL_RGBA8,
                                   static_cast<size_t>(single_size.width) * 4u) == COMP_OK);
    COMP_CHECK(comp_render_flatten(whole, b.data(), COMP_PIXEL_RGBA8,
                                   static_cast<size_t>(whole_size.width) * 4u) == COMP_OK);
    COMP_CHECK_MSG(a == b, "both import paths decode the same pixels");
  }

  comp_document_destroy(single);
  comp_document_destroy(whole);
  comp_engine_destroy(engine);
}
