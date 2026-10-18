// AI生成
/*
 * exr_codec.cpp — 自研 OpenEXR scanline 读写器（C 方案 C3b）。
 *
 * 参考 OpenEXR 的文件布局（OpenEXRFileLayout）与 ImfZip/ImfRle 的压缩口径。
 * 两处**容易写错、写错就静默出坏像素**的地方，在这里先记清楚：
 *
 *   1. ZIP / ZIPS / RLE 三种压缩都先做同一套前处理：
 *        重排（偶数下标字节放前半、奇数下标放后半）
 *        -> 预测（每个字节减去前一字节再加 384，按字节截断）
 *      然后再 deflate（ZIP/ZIPS）或 RLE 编码。解码必须**逆序**：
 *      先解压，再累加还原预测，最后交织还原重排。少做任何一步都还是一串
 *      长度正确的字节——没有任何自校验会替你发现。
 *   2. 块数据的**未压缩大小**由 dataWindow 与通道表精确算出。当块头里的
 *      dataSize 恰好等于它时，块内是**原样像素**（OpenEXR 在压缩无收益时
 *      直接存原文），不做任何前处理；只有 dataSize 更小时才是压缩流。
 *
 * 读的边界：scanline、单部件，压缩 NONE/RLE/ZIPS/ZIP，通道 HALF/FLOAT，
 * 采样率 1。其余一律带原因失败（PIZ/PXR24/B44/DWA、tile、deep、multipart、
 * UINT、子采样、RANDOM_Y）。尺寸闸（is_valid_canvas）在**分配之前**执行。
 */
#include "format/exr_codec.h"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/comp_geometry.h"
#include "imaging/DeepPixels.h"

#if defined(__has_include)
#  if __has_include("zlib.h")
#    define COMPOSITOR_EXR_ZLIB 1
#  endif
#endif

#ifdef COMPOSITOR_EXR_ZLIB
#  include "zlib.h"
#endif

namespace compositor {
namespace {

constexpr uint32_t kExrMagic = 0x01312F76u;

constexpr uint32_t kFlagTiled = 0x200u;
constexpr uint32_t kFlagNonImage = 0x800u;
constexpr uint32_t kFlagMultipart = 0x1000u;

constexpr int32_t kCompressionNone = 0;
constexpr int32_t kCompressionRle = 1;
constexpr int32_t kCompressionZips = 2;
constexpr int32_t kCompressionZip = 3;

constexpr int32_t kPixelTypeUint = 0;
constexpr int32_t kPixelTypeHalf = 1;
constexpr int32_t kPixelTypeFloat = 2;

constexpr int32_t kLineOrderIncreasing = 0;
constexpr int32_t kLineOrderDecreasing = 1;

/* 单个通道名 / 属性名 / 属性类型名的上界：OpenEXR 自己的上限是 255（long names）。 */
constexpr size_t kMaxNameLength = 255;

bool fail(std::string* error, const std::string& text) {
  if (error != nullptr) *error = text;
  return false;
}

std::string compression_name(int32_t compression) {
  switch (compression) {
    case kCompressionNone: return "NONE";
    case kCompressionRle: return "RLE";
    case kCompressionZips: return "ZIPS";
    case kCompressionZip: return "ZIP";
    case 4: return "PIZ";
    case 5: return "PXR24";
    case 6: return "B44";
    case 7: return "B44A";
    case 8: return "DWAA";
    case 9: return "DWAB";
    default: return "unknown";
  }
}

uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p) {
  return static_cast<uint64_t>(read_u32(p)) | (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

int32_t read_i32(const uint8_t* p) { return static_cast<int32_t>(read_u32(p)); }

float read_f32(const uint8_t* p) {
  const uint32_t bits = read_u32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/* 带边界检查的顺序读取器：所有失败都落成一句可读的 error，绝不越界。 */
struct ByteReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
  std::string* error = nullptr;

  bool fail(const std::string& text) {
    if (error != nullptr && error->empty()) *error = text;
    return false;
  }

  bool remaining(size_t count) const {
    return offset <= size && count <= size - offset;
  }

  bool take(size_t count, const uint8_t** out) {
    if (!remaining(count)) return fail("EXR: file is truncated");
    if (out != nullptr) *out = data + offset;
    offset += count;
    return true;
  }

  bool skip(size_t count) { return take(count, nullptr); }

  bool u8(uint8_t* out) {
    const uint8_t* p = nullptr;
    if (!take(1, &p)) return false;
    *out = p[0];
    return true;
  }

  bool i32(int32_t* out) {
    const uint8_t* p = nullptr;
    if (!take(4, &p)) return false;
    *out = read_i32(p);
    return true;
  }

  bool u32(uint32_t* out) {
    const uint8_t* p = nullptr;
    if (!take(4, &p)) return false;
    *out = read_u32(p);
    return true;
  }

  bool u64(uint64_t* out) {
    const uint8_t* p = nullptr;
    if (!take(8, &p)) return false;
    *out = read_u64(p);
    return true;
  }

  bool cstr(std::string* out, size_t max_length) {
    const size_t start = offset;
    while (offset < size && data[offset] != 0u) ++offset;
    if (offset >= size) return fail("EXR: header string is not terminated");
    const size_t length = offset - start;
    if (length > max_length) return fail("EXR: header string is too long");
    out->assign(reinterpret_cast<const char*>(data + start), length);
    ++offset;
    return true;
  }
};

struct ExrChannel {
  std::string name;
  int32_t pixel_type = kPixelTypeHalf;
  size_t byte_offset = 0; /* 一个扫描行内该通道数据的起始偏移 */
  size_t pixel_size = 0;
};

struct ExrHeader {
  int32_t min_x = 0;
  int32_t min_y = 0;
  int32_t max_x = 0;
  int32_t max_y = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t compression = kCompressionNone;
  int32_t line_order = kLineOrderIncreasing;
  std::vector<ExrChannel> channels;
  size_t scanline_bytes = 0; /* 一行（全部通道）的原始字节数 */
  size_t header_end = 0;     /* 头部结束位置，即偏移表起点 */
};

bool parse_channels(const uint8_t* value, size_t value_size, ExrHeader* header,
                    std::string* error) {
  ByteReader reader{value, value_size, 0, error};
  for (;;) {
    std::string name;
    if (!reader.cstr(&name, kMaxNameLength)) return false;
    if (name.empty()) break;
    if (header->channels.size() >= 4096u) {
      return reader.fail("EXR: header declares too many channels");
    }
    ExrChannel channel;
    channel.name = std::move(name);
    uint8_t p_linear = 0;
    if (!reader.i32(&channel.pixel_type)) return false;
    if (!reader.u8(&p_linear)) return false;
    if (!reader.skip(3)) return false; /* 3 字节保留位 */
    int32_t x_sampling = 1;
    int32_t y_sampling = 1;
    if (!reader.i32(&x_sampling)) return false;
    if (!reader.i32(&y_sampling)) return false;
    if (channel.pixel_type == kPixelTypeUint) {
      return reader.fail("EXR: UINT channels are not supported");
    }
    if (channel.pixel_type != kPixelTypeHalf && channel.pixel_type != kPixelTypeFloat) {
      return reader.fail("EXR: unknown channel pixel type " +
                         std::to_string(channel.pixel_type));
    }
    if (x_sampling != 1 || y_sampling != 1) {
      return reader.fail("EXR: subsampled channels are not supported");
    }
    channel.pixel_size = channel.pixel_type == kPixelTypeHalf ? 2u : 4u;
    header->channels.push_back(std::move(channel));
  }
  if (header->channels.empty()) return reader.fail("EXR: channel list is empty");
  return true;
}

/* 头部解析：magic / version / flags / attributes，并做尺寸闸。
 * 只读头部，不分配与图像面积相关的内存。 */
bool parse_header(const uint8_t* data, size_t size, ExrHeader* header, std::string* error) {
  if (data == nullptr || size < 8u) return fail(error, "EXR: file is too small");
  ByteReader reader{data, size, 0, error};

  uint32_t magic = 0;
  if (!reader.u32(&magic)) return false;
  if (magic != kExrMagic) return fail(error, "EXR: bad magic number");

  uint32_t version = 0;
  if (!reader.u32(&version)) return false;
  const uint32_t version_number = version & 0xffu;
  const uint32_t flags = version & ~0xffu;
  if (version_number < 1u || version_number > 2u) {
    return fail(error, "EXR: unsupported file version " + std::to_string(version_number));
  }
  if ((flags & kFlagTiled) != 0u) return fail(error, "EXR: tiled files are not supported");
  if ((flags & kFlagNonImage) != 0u) {
    return fail(error, "EXR: deep (non-image) files are not supported");
  }
  if ((flags & kFlagMultipart) != 0u) {
    return fail(error, "EXR: multipart files are not supported");
  }

  bool have_channels = false;
  bool have_compression = false;
  bool have_data_window = false;
  bool have_line_order = false;
  int attribute_count = 0;

  for (;;) {
    std::string name;
    if (!reader.cstr(&name, kMaxNameLength)) return false;
    if (name.empty()) break; /* 头部结束 */
    if (++attribute_count > 1024) return fail(error, "EXR: header has too many attributes");

    std::string type;
    if (!reader.cstr(&type, kMaxNameLength)) return false;
    int32_t value_size = 0;
    if (!reader.i32(&value_size)) return false;
    if (value_size < 0) return fail(error, "EXR: attribute has a negative size");
    const uint8_t* value = nullptr;
    if (!reader.take(static_cast<size_t>(value_size), &value)) return false;

    if (name == "channels") {
      if (have_channels) return fail(error, "EXR: duplicate channels attribute");
      if (!parse_channels(value, static_cast<size_t>(value_size), header, error)) return false;
      have_channels = true;
    } else if (name == "compression") {
      if (value_size < 1) return fail(error, "EXR: compression attribute is empty");
      header->compression = value[0];
      have_compression = true;
    } else if (name == "dataWindow") {
      if (value_size < 16) return fail(error, "EXR: dataWindow attribute is too small");
      header->min_x = read_i32(value);
      header->min_y = read_i32(value + 4);
      header->max_x = read_i32(value + 8);
      header->max_y = read_i32(value + 12);
      have_data_window = true;
    } else if (name == "lineOrder") {
      if (value_size < 1) return fail(error, "EXR: lineOrder attribute is empty");
      header->line_order = value[0];
      have_line_order = true;
    }
  }

  if (!have_channels) return fail(error, "EXR: missing channels attribute");
  if (!have_compression) return fail(error, "EXR: missing compression attribute");
  if (!have_data_window) return fail(error, "EXR: missing dataWindow attribute");
  if (!have_line_order) return fail(error, "EXR: missing lineOrder attribute");

  switch (header->compression) {
    case kCompressionNone:
    case kCompressionRle:
      break;
    case kCompressionZips:
    case kCompressionZip:
#ifndef COMPOSITOR_EXR_ZLIB
      return fail(error, "EXR: ZIP compression needs zlib, which is not vendored in this build");
#endif
      break;
    default:
      return fail(error, "EXR: " + compression_name(header->compression) +
                             " compression is not supported (NONE/RLE/ZIPS/ZIP only)");
  }

  if (header->line_order != kLineOrderIncreasing &&
      header->line_order != kLineOrderDecreasing) {
    return fail(error, "EXR: RANDOM_Y line order is not supported");
  }

  /* dataWindow：闭区间，先按 int64 算宽高，避免 max - min + 1 有符号溢出。 */
  const int64_t width = static_cast<int64_t>(header->max_x) - header->min_x + 1;
  const int64_t height = static_cast<int64_t>(header->max_y) - header->min_y + 1;
  if (width <= 0 || height <= 0) return fail(error, "EXR: dataWindow is empty");
  if (width > std::numeric_limits<int32_t>::max() ||
      height > std::numeric_limits<int32_t>::max()) {
    return fail(error, "EXR: dataWindow exceeds the canvas limit");
  }
  header->width = static_cast<int32_t>(width);
  header->height = static_cast<int32_t>(height);
  /* 尺寸闸在**任何**面积相关分配之前：伪造 dataWindow 不能换到一次大分配。 */
  if (!is_valid_canvas(comp_size{header->width, header->height})) {
    return fail(error, "EXR: image exceeds maximum canvas size");
  }

  /* 每个扫描行内各通道的布局：按通道表顺序紧排，宽度 × 该通道像素字节数。 */
  size_t scanline_bytes = 0;
  for (ExrChannel& channel : header->channels) {
    channel.byte_offset = scanline_bytes;
    const size_t add = static_cast<size_t>(header->width) * channel.pixel_size;
    if (add > std::numeric_limits<size_t>::max() - scanline_bytes) {
      return fail(error, "EXR: scanline is too large");
    }
    scanline_bytes += add;
  }
  header->scanline_bytes = scanline_bytes;
  header->header_end = reader.offset;
  return true;
}

const ExrChannel* find_channel(const ExrHeader& header, const std::string& name) {
  for (const ExrChannel& channel : header.channels) {
    if (channel.name == name) return &channel;
  }
  return nullptr;
}

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* 选通道：优先精确的 R/G/B；否则找一组同前缀的 "<prefix>.R/G/B"；
 * 再否则退到 Y（灰度 EXR），按亮度复制到 RGB。alpha 同理，缺省 1。 */
struct ChannelPick {
  const ExrChannel* r = nullptr;
  const ExrChannel* g = nullptr;
  const ExrChannel* b = nullptr;
  const ExrChannel* a = nullptr;
  const ExrChannel* y = nullptr;
};

bool select_channels(const ExrHeader& header, ChannelPick* pick, std::string* error) {
  const ExrChannel* r = find_channel(header, "R");
  const ExrChannel* g = find_channel(header, "G");
  const ExrChannel* b = find_channel(header, "B");
  if (r != nullptr && g != nullptr && b != nullptr) {
    pick->r = r;
    pick->g = g;
    pick->b = b;
    pick->a = find_channel(header, "A");
    return true;
  }

  /* 分层 EXR（beauty.R / diffuse.R ...）：取通道表里第一组完整的三元组。 */
  for (const ExrChannel& channel : header.channels) {
    if (!ends_with(channel.name, ".R") || channel.name.size() <= 2u) continue;
    const std::string prefix = channel.name.substr(0, channel.name.size() - 2u);
    const ExrChannel* group_g = find_channel(header, prefix + ".G");
    const ExrChannel* group_b = find_channel(header, prefix + ".B");
    if (group_g == nullptr || group_b == nullptr) continue;
    pick->r = &channel;
    pick->g = group_g;
    pick->b = group_b;
    pick->a = find_channel(header, prefix + ".A");
    return true;
  }

  const ExrChannel* y = find_channel(header, "Y");
  std::string y_prefix;
  if (y == nullptr) {
    for (const ExrChannel& channel : header.channels) {
      if (ends_with(channel.name, ".Y") && channel.name.size() > 2u) {
        y = &channel;
        y_prefix = channel.name.substr(0, channel.name.size() - 2u);
        break;
      }
    }
  }
  if (y == nullptr) return fail(error, "EXR: no R/G/B or Y channels");
  pick->y = y;
  pick->a = find_channel(header, "A");
  if (pick->a == nullptr && !y_prefix.empty()) pick->a = find_channel(header, y_prefix + ".A");
  return true;
}

float sample_channel(const ExrChannel& channel, const uint8_t* scanline, int32_t x) {
  const uint8_t* p = scanline + channel.byte_offset + static_cast<size_t>(x) * channel.pixel_size;
  if (channel.pixel_type == kPixelTypeHalf) return deep_half_value(read_u16(p));
  return read_f32(p);
}

/* 逆预测：OpenEXR 的预测器把「当前字节 - 前一字节 + 384」写回当前字节。 */
void undo_predictor(uint8_t* buffer, size_t size) {
  if (size <= 1u) return;
  int previous = buffer[0];
  for (size_t i = 1; i < size; ++i) {
    const int delta = static_cast<int>(buffer[i]) - (128 + 256);
    previous = (previous + delta) & 0xff;
    buffer[i] = static_cast<uint8_t>(previous);
  }
}

/* 逆重排：压缩前偶数下标字节写在前半、奇数下标写在后半，这里交织还原。 */
void undo_reorder(uint8_t* buffer, size_t size, std::vector<uint8_t>* scratch) {
  if (size <= 1u) return;
  const size_t half = (size + 1u) / 2u;
  scratch->assign(buffer, buffer + size);
  const uint8_t* reordered = scratch->data();
  for (size_t i = 0; i < size; ++i) {
    buffer[i] = (i & 1u) == 0u ? reordered[i / 2u] : reordered[half + i / 2u];
  }
}

/* OpenEXR 的 RLE：负数计数后跟一段原文字节；非负计数表示「下一字节重复 count+1 次」。 */
bool rle_decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size,
                size_t* produced, std::string* error) {
  size_t in_pos = 0;
  size_t out_pos = 0;
  while (in_pos < in_size) {
    int control = static_cast<int>(in[in_pos++]);
    if (control >= 128) control -= 256; /* 有符号计数（不依赖 int8_t 的窄化语义） */
    if (control < 0) {
      const size_t count = static_cast<size_t>(-control);
      if (count > in_size - in_pos) return fail(error, "EXR: RLE literal run is truncated");
      if (count > out_size - out_pos) return fail(error, "EXR: RLE output overflows");
      std::memcpy(out + out_pos, in + in_pos, count);
      in_pos += count;
      out_pos += count;
    } else {
      const size_t count = static_cast<size_t>(control) + 1u;
      if (in_pos >= in_size) return fail(error, "EXR: RLE repeat run is truncated");
      if (count > out_size - out_pos) return fail(error, "EXR: RLE output overflows");
      std::memset(out + out_pos, in[in_pos], count);
      ++in_pos;
      out_pos += count;
    }
  }
  *produced = out_pos;
  return true;
}

bool decompress_block(int32_t compression, const uint8_t* in, size_t in_size, uint8_t* out,
                      size_t raw_size, std::vector<uint8_t>* scratch, std::string* error) {
  switch (compression) {
    case kCompressionNone:
      /* 走到这里说明 dataSize != rawSize：NONE 的块必须是原样像素。 */
      return fail(error, "EXR: uncompressed block size does not match its scanlines");
    case kCompressionRle: {
      size_t produced = 0;
      if (!rle_decode(in, in_size, out, raw_size, &produced, error)) return false;
      if (produced != raw_size) return fail(error, "EXR: RLE block has the wrong size");
      undo_predictor(out, raw_size);
      undo_reorder(out, raw_size, scratch);
      return true;
    }
    case kCompressionZips:
    case kCompressionZip: {
#ifdef COMPOSITOR_EXR_ZLIB
      size_t skip = 0;
      size_t payload = in_size;
      /*
       * 兼容带 16 字节前缀的 ZIP 变体（uint64 未压缩大小 + uint64 压缩大小）。
       * 只有在两个字段都精确对上时才跳过；真正的 zlib 流恰好满足这两个等式的
       * 概率可以忽略，所以不会把普通块误判成带前缀的块。
       */
      if (in_size >= 16u) {
        const uint64_t declared_raw = read_u64(in);
        const uint64_t declared_compressed = read_u64(in + 8);
        if (declared_raw == static_cast<uint64_t>(raw_size) &&
            declared_compressed == static_cast<uint64_t>(in_size - 16u)) {
          skip = 16u;
          payload = in_size - 16u;
        }
      }
      uLongf out_size = static_cast<uLongf>(raw_size);
      const int status = ::uncompress(out, &out_size, in + skip, static_cast<uLong>(payload));
      if (status != Z_OK || out_size != static_cast<uLongf>(raw_size)) {
        return fail(error, "EXR: zlib block failed to decompress");
      }
      undo_predictor(out, raw_size);
      undo_reorder(out, raw_size, scratch);
      return true;
#else
      return fail(error, "EXR: ZIP compression needs zlib, which is not vendored in this build");
#endif
    }
    default:
      return fail(error, "EXR: " + compression_name(compression) + " compression is not supported");
  }
}

}  // namespace

/* ----------------------------------------------------------- 能力探测 */

bool exr_codec_available() { return true; }

bool exr_codec_has_zlib() {
#ifdef COMPOSITOR_EXR_ZLIB
  return true;
#else
  return false;
#endif
}

const char* exr_codec_backend() {
  static const std::string backend = [] {
    std::string text = "in-tree OpenEXR scanline codec (NONE/RLE/ZIPS/ZIP, HALF/FLOAT)";
#ifdef COMPOSITOR_EXR_ZLIB
    text += std::string(" + zlib ") + zlibVersion();
#else
    text += " without zlib (ZIP/ZIPS unavailable)";
#endif
    return text;
  }();
  return backend.c_str();
}

bool exr_declared_size(const uint8_t* data, size_t size, int32_t* width, int32_t* height,
                       std::string* error) {
  if (width == nullptr || height == nullptr) return fail(error, "EXR: null size output");
  ExrHeader header;
  if (!parse_header(data, size, &header, error)) return false;
  *width = header.width;
  *height = header.height;
  return true;
}

/* ---------------------------------------------------------------- 读 */

bool exr_decode_float(const uint8_t* data, size_t size, CodecFloatImage* out, std::string* error) {
  if (out == nullptr) return fail(error, "EXR: out is null");

  ExrHeader header;
  if (!parse_header(data, size, &header, error)) return false;

  ChannelPick pick;
  if (!select_channels(header, &pick, error)) return false;

  const int32_t lines_per_block = header.compression == kCompressionZip ? 16 : 1;
  const int64_t block_count = (static_cast<int64_t>(header.height) + lines_per_block - 1) /
                              lines_per_block;

  /* 偏移表：每块一个 uint64。表本身也必须在文件内。 */
  ByteReader reader{data, size, header.header_end, error};
  if (static_cast<uint64_t>(block_count) >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 8u)) {
    return fail(error, "EXR: too many blocks");
  }
  const size_t offset_table_bytes = static_cast<size_t>(block_count) * 8u;
  const uint8_t* offset_table = nullptr;
  if (!reader.take(offset_table_bytes, &offset_table)) return false;

  CodecFloatImage image;
  image.width = header.width;
  image.height = header.height;
  const size_t pixel_count = static_cast<size_t>(header.width) * static_cast<size_t>(header.height);
  image.rgba.assign(pixel_count * 4u, 0.0f);

  std::vector<uint8_t> scratch;
  std::vector<uint8_t> reorder_scratch;

  for (int64_t block = 0; block < block_count; ++block) {
    const uint64_t chunk_offset = read_u64(offset_table + static_cast<size_t>(block) * 8u);
    if (chunk_offset > static_cast<uint64_t>(size)) {
      return fail(error, "EXR: chunk offset is past the end of the file");
    }
    ByteReader chunk{data, size, static_cast<size_t>(chunk_offset), error};
    int32_t chunk_y = 0;
    int32_t data_size = 0;
    if (!chunk.i32(&chunk_y)) return false;
    if (!chunk.i32(&data_size)) return false;
    if (data_size < 0) return fail(error, "EXR: chunk has a negative size");
    const uint8_t* payload = nullptr;
    if (!chunk.take(static_cast<size_t>(data_size), &payload)) return false;

    if (chunk_y < header.min_y || chunk_y > header.max_y) {
      return fail(error, "EXR: chunk scanline is outside the dataWindow");
    }
    const int64_t available = static_cast<int64_t>(header.max_y) - chunk_y + 1;
    const int32_t lines = static_cast<int32_t>(available < lines_per_block ? available
                                                                          : lines_per_block);
    const size_t raw_size = header.scanline_bytes * static_cast<size_t>(lines);

    const uint8_t* raw = nullptr;
    if (static_cast<size_t>(data_size) == raw_size) {
      raw = payload; /* 压缩无收益时 OpenEXR 直接存原样像素 */
    } else if (static_cast<size_t>(data_size) > raw_size) {
      return fail(error, "EXR: chunk is larger than its scanlines");
    } else {
      scratch.resize(raw_size);
      if (!decompress_block(header.compression, payload, static_cast<size_t>(data_size),
                            scratch.data(), raw_size, &reorder_scratch, error)) {
        return false;
      }
      raw = scratch.data();
    }

    for (int32_t line = 0; line < lines; ++line) {
      const int32_t row_y = chunk_y + line;
      if (row_y > header.max_y) return fail(error, "EXR: chunk runs past the dataWindow");
      const uint8_t* scanline = raw + static_cast<size_t>(line) * header.scanline_bytes;
      float* row = image.rgba.data() +
                   static_cast<size_t>(row_y - header.min_y) * static_cast<size_t>(header.width) *
                       4u;
      for (int32_t x = 0; x < header.width; ++x, row += 4) {
        if (pick.y != nullptr) {
          const float value = sample_channel(*pick.y, scanline, x);
          row[0] = value;
          row[1] = value;
          row[2] = value;
        } else {
          row[0] = sample_channel(*pick.r, scanline, x);
          row[1] = sample_channel(*pick.g, scanline, x);
          row[2] = sample_channel(*pick.b, scanline, x);
        }
        row[3] = pick.a != nullptr ? sample_channel(*pick.a, scanline, x) : 1.0f;
      }
    }
  }

  *out = std::move(image);
  return true;
}

/* ---------------------------------------------------------------- 写 */

namespace {

void put_u32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xffu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

void put_i32(std::vector<uint8_t>* out, int32_t value) { put_u32(out, static_cast<uint32_t>(value)); }

void put_u64(std::vector<uint8_t>* out, uint64_t value) {
  put_u32(out, static_cast<uint32_t>(value & 0xffffffffu));
  put_u32(out, static_cast<uint32_t>(value >> 32));
}

void put_f32(std::vector<uint8_t>* out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(out, bits);
}

void put_cstr(std::vector<uint8_t>* out, const char* text) {
  const size_t length = std::strlen(text);
  out->insert(out->end(), text, text + length);
  out->push_back(0u);
}

void put_attribute(std::vector<uint8_t>* out, const char* name, const char* type,
                   const std::vector<uint8_t>& value) {
  put_cstr(out, name);
  put_cstr(out, type);
  put_i32(out, static_cast<int32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void put_channel(std::vector<uint8_t>* out, const char* name, int32_t pixel_type) {
  put_cstr(out, name);
  put_i32(out, pixel_type);
  out->push_back(0u); /* pLinear */
  out->push_back(0u);
  out->push_back(0u);
  out->push_back(0u);
  put_i32(out, 1); /* xSampling */
  put_i32(out, 1); /* ySampling */
}

}  // namespace

bool exr_encode_float(const float* rgba, int32_t width, int32_t height, ExrSampleType sample_type,
                      std::vector<uint8_t>* out, std::string* error) {
  if (rgba == nullptr || out == nullptr) return fail(error, "EXR: null buffer");
  if (width <= 0 || height <= 0) return fail(error, "EXR: non-positive extent");
  if (!is_valid_canvas(comp_size{width, height})) {
    return fail(error, "EXR: image exceeds maximum canvas size");
  }
  if (sample_type != kExrSampleHalf && sample_type != kExrSampleFloat) {
    return fail(error, "EXR: unknown channel sample type");
  }
  const int32_t pixel_type = static_cast<int32_t>(sample_type);
  const size_t pixel_bytes = sample_type == kExrSampleHalf ? 2u : 4u;

  /* 通道顺序必须与数据布局一致：OpenEXR 惯例是字母序 A/B/G/R。 */
  const char* const channel_names[4] = {"A", "B", "G", "R"};

  std::vector<uint8_t> header;
  put_u32(&header, kExrMagic);
  put_u32(&header, 2u); /* version 2, single-part scanline */

  std::vector<uint8_t> channels;
  for (const char* name : channel_names) put_channel(&channels, name, pixel_type);
  channels.push_back(0u); /* 通道表结束 */
  put_attribute(&header, "channels", "chlist", channels);

  put_attribute(&header, "compression", "compression",
                std::vector<uint8_t>{static_cast<uint8_t>(kCompressionNone)});

  std::vector<uint8_t> window;
  put_i32(&window, 0);
  put_i32(&window, 0);
  put_i32(&window, width - 1);
  put_i32(&window, height - 1);
  put_attribute(&header, "dataWindow", "box2i", window);
  put_attribute(&header, "displayWindow", "box2i", window);

  put_attribute(&header, "lineOrder", "lineOrder",
                std::vector<uint8_t>{static_cast<uint8_t>(kLineOrderIncreasing)});

  std::vector<uint8_t> aspect;
  put_f32(&aspect, 1.0f);
  put_attribute(&header, "pixelAspectRatio", "float", aspect);

  std::vector<uint8_t> center;
  put_f32(&center, 0.0f);
  put_f32(&center, 0.0f);
  put_attribute(&header, "screenWindowCenter", "v2f", center);

  std::vector<uint8_t> window_width;
  put_f32(&window_width, 1.0f);
  put_attribute(&header, "screenWindowWidth", "float", window_width);

  header.push_back(0u); /* 头部结束 */

  const size_t row_bytes = static_cast<size_t>(width) * 4u * pixel_bytes;
  const size_t chunk_bytes = 8u + row_bytes;
  const size_t offset_table = header.size();
  const size_t first_chunk = offset_table + static_cast<size_t>(height) * 8u;

  out->clear();
  out->reserve(first_chunk + static_cast<size_t>(height) * chunk_bytes);
  out->insert(out->end(), header.begin(), header.end());
  for (int32_t y = 0; y < height; ++y) {
    put_u64(out, static_cast<uint64_t>(first_chunk + static_cast<size_t>(y) * chunk_bytes));
  }

  for (int32_t y = 0; y < height; ++y) {
    put_i32(out, y);
    put_i32(out, static_cast<int32_t>(row_bytes));
    const float* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
    /* A、B、G、R 四个通道各占一整行；通道内是 little-endian 的 half 或 float。 */
    for (int channel = 0; channel < 4; ++channel) {
      for (int32_t x = 0; x < width; ++x) {
        const float value = row[static_cast<size_t>(x) * 4u + 3u - channel];
        if (sample_type == kExrSampleHalf) {
          const uint16_t bits = deep_half_bits(value);
          out->push_back(static_cast<uint8_t>(bits & 0xffu));
          out->push_back(static_cast<uint8_t>(bits >> 8));
        } else {
          put_f32(out, value);
        }
      }
    }
  }
  return true;
}

}  // namespace compositor
