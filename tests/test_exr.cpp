// AI生成
/*
 * test_exr.cpp — C3b：树内 EXR scanline 读取器与 .comp 宽位深资产往返。
 *
 * 这一组测试守的是两条「沉默损坏」通道，以及格式能力边界的诚实性：
 *   1. OpenEXR 的 RLE/ZIPS/ZIP 块先做**偶奇字节重排**、再做**差分预测**
 *      （`d = 当前 - 前一 + 384`），读侧必须以相反顺序撤掉两步。只做其中一步、
 *      或顺序做反，解出来的都是「能跑但画面不对」的像素，不会报错。
 *   2. 块头的 dataSize 恰好等于该块扫描行的原始字节数时，块内就是**原样像素**
 *      （OpenEXR 在压缩无收益时直接存原文），不能再去解压/逆预测。
 * 3. 不支持的东西必须报名字：PIZ/PXR24/B44/B44A/DWAA/DWAB、tile、deep、
 *      multipart、UINT、子采样、RANDOM_Y，以及伪造的超大 dataWindow。
 *
 * 夹具全部在本文件里手工构造（仓库没有 EXR 样本，也没有 Python EXR 工具）：
 * 期望值来自测试自己的编码器，绝不从被测实现里取。压缩夹具的「前处理」也
 * 由测试独立实现一遍（重排 + 预测 + RLE 字面量 / zlib），与被测解码器互证。
 */
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "domain/document.h"
#include "format/comp_file.h"
#include "format/exr_codec.h"
#include "imaging/DeepPixels.h"
#include "test_framework.h"

#if defined(__has_include)
#  if __has_include("zlib.h")
#    include "zlib.h"
#    define TEST_EXR_ZLIB 1
#  endif
#endif

namespace {

constexpr int32_t kCompressionNone = 0;
constexpr int32_t kCompressionRle = 1;
constexpr int32_t kCompressionZips = 2;
constexpr int32_t kCompressionZip = 3;
constexpr int32_t kTypeHalf = 1;
constexpr int32_t kTypeFloat = 2;

constexpr uint32_t kFlagTiled = 0x200u;
constexpr uint32_t kFlagDeep = 0x800u;
constexpr uint32_t kFlagMultipart = 0x1000u;

/* ------------------------------------------------------------ 字节工具 */

void put_u32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xffu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

void put_i32(std::vector<uint8_t>* out, int32_t value) {
  put_u32(out, static_cast<uint32_t>(value));
}

void put_f32(std::vector<uint8_t>* out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(out, bits);
}

void put_cstr(std::vector<uint8_t>* out, const char* text) {
  while (*text != '\0') out->push_back(static_cast<uint8_t>(*text++));
  out->push_back(0u);
}

void put_attribute(std::vector<uint8_t>* out, const char* name, const char* type,
                   const std::vector<uint8_t>& value) {
  put_cstr(out, name);
  put_cstr(out, type);
  put_i32(out, static_cast<int32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

/*
 * 只对**能被 half 精确表示**的值做转换：符号 + 5 位指数 + 10 位尾数。
 * 尾数低 13 位不为 0 的值返回 0xFFFF（half 的 NaN 位型）当哨兵，测试里会先
 * 断言夹具没踩到它。这样期望值不依赖被测实现的 half 舍入。
 */
uint16_t exact_half_bits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t raw_exp = (bits >> 23) & 0xffu;
  const uint32_t mantissa = bits & 0x7fffffu;
  if (raw_exp == 0u && mantissa == 0u) return static_cast<uint16_t>(sign);
  if (raw_exp == 0xffu) return 0xffffu;
  if ((mantissa & 0x1fffu) != 0u) return 0xffffu;
  const int32_t exponent = static_cast<int32_t>(raw_exp) - 127;
  const int32_t half_exp = exponent + 15;
  if (half_exp <= 0 || half_exp >= 31) return 0xffffu;
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(half_exp) << 10) |
                               (mantissa >> 13));
}

/* --------------------------------------------------------- 夹具构造器 */

struct ExrChannelSpec {
  std::string name;
  int32_t type = kTypeHalf;
};

struct ExrSpec {
  std::vector<ExrChannelSpec> channels;
  /* samples[c] 按行紧排，长度应为 width * height；越界读返回 0，避免夹具
   * 尺寸写错时在测试里先崩一次（尺寸闸测试会故意让声明与样例不一致）。 */
  std::vector<std::vector<float>> samples;
  int32_t width = 1;
  int32_t height = 1;
  int32_t compression = kCompressionNone;
  int32_t line_order = 0; /* 0 = INCREASING_Y, 1 = DECREASING_Y */
  uint32_t flags = 0u;
  bool raw_blocks = false; /* 压缩格式但块内直接放原样像素 */
  bool subsampled = false;
  int32_t window_min_x = 0;
  int32_t window_min_y = 0;
  int32_t window_max_x = -1; /* -1 = width - 1 */
  int32_t window_max_y = -1;
};

float sample_at(const ExrSpec& spec, size_t channel, int32_t x, int32_t y) {
  if (channel >= spec.samples.size()) return 0.0f;
  const size_t index = static_cast<size_t>(y) * static_cast<size_t>(spec.width) +
                       static_cast<size_t>(x);
  if (index >= spec.samples[channel].size()) return 0.0f;
  return spec.samples[channel][index];
}

/* OpenEXR 压缩前处理：先偶奇分半重排，再差分预测（+384）。 */
std::vector<uint8_t> forward_layout(const std::vector<uint8_t>& raw) {
  const size_t size = raw.size();
  std::vector<uint8_t> out(size, 0u);
  if (size == 0u) return out;
  const size_t half = (size + 1u) / 2u;
  size_t even = 0;
  size_t odd = half;
  for (size_t i = 0; i < size; ++i) {
    if ((i & 1u) == 0u) {
      out[even++] = raw[i];
    } else {
      out[odd++] = raw[i];
    }
  }
  int previous = out[0];
  for (size_t i = 1; i < size; ++i) {
    const int current = out[i];
    out[i] = static_cast<uint8_t>(current - previous + (128 + 256));
    previous = current;
  }
  return out;
}

/*
 * 真 RLE 编码（与 OpenEXR 同一口径）：连续 3 个以上相同字节用「重复游程」，
 * 其余用字面量游程（负数计数后跟原文）。
 *
 * 这里**不能**只发字面量：那样 payload 永远比原文大，build_exr 会按 OpenEXR
 * 的「压缩无收益就存原样」规则退回 raw 块，夹具就绕过了 RLE 解码路径——测试
 * 看起来绿，实际什么都没测。RLE 用例的样本是逐行常量，预测后整块几乎都是
 * 0x80，重复游程能把 payload 压到远小于 raw，压缩分支才真的被走到。
 */
std::vector<uint8_t> rle_compress(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  const size_t size = in.size();
  size_t position = 0;
  while (position < size) {
    size_t run = 1;
    while (position + run < size && in[position + run] == in[position] && run < 128u) ++run;
    if (run >= 3u) {
      out.push_back(static_cast<uint8_t>(run - 1u)); /* 非负计数：下一字节重复 count+1 次 */
      out.push_back(in[position]);
      position += run;
      continue;
    }

    const size_t literal_start = position;
    position += run;
    while (position < size) {
      size_t next = 1;
      while (position + next < size && in[position + next] == in[position] && next < 128u) {
        ++next;
      }
      if (next >= 3u) break; /* 让外层发重复游程 */
      if (position - literal_start + next > 127u) break;
      position += next;
    }
    const size_t count = position - literal_start;
    out.push_back(static_cast<uint8_t>(256u - count)); /* -count 的有符号字节 */
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(literal_start),
               in.begin() + static_cast<std::ptrdiff_t>(position));
  }
  return out;
}

#ifdef TEST_EXR_ZLIB
std::vector<uint8_t> deflate_bytes(const std::vector<uint8_t>& in) {
  uLongf out_size = compressBound(static_cast<uLong>(in.size()));
  std::vector<uint8_t> out(out_size, 0u);
  const int status = compress2(out.data(), &out_size, in.data(), static_cast<uLong>(in.size()),
                               Z_DEFAULT_COMPRESSION);
  if (status != Z_OK) return std::vector<uint8_t>();
  out.resize(out_size);
  return out;
}
#endif

std::vector<uint8_t> build_exr(const ExrSpec& spec) {
  std::vector<uint8_t> out;
  put_u32(&out, 0x01312F76u);
  put_u32(&out, 2u | spec.flags);

  std::vector<uint8_t> channel_list;
  for (size_t i = 0; i < spec.channels.size(); ++i) {
    put_cstr(&channel_list, spec.channels[i].name.c_str());
    put_i32(&channel_list, spec.channels[i].type);
    channel_list.push_back(0u); /* pLinear */
    channel_list.push_back(0u);
    channel_list.push_back(0u);
    channel_list.push_back(0u);
    const int32_t sampling = spec.subsampled && i == 0u ? 2 : 1;
    put_i32(&channel_list, sampling);
    put_i32(&channel_list, sampling);
  }
  channel_list.push_back(0u); /* 通道表结束 */
  put_attribute(&out, "channels", "chlist", channel_list);

  put_attribute(&out, "compression", "compression",
                std::vector<uint8_t>{static_cast<uint8_t>(spec.compression)});

  const int32_t max_x = spec.window_max_x >= 0 ? spec.window_max_x : spec.width - 1;
  const int32_t max_y = spec.window_max_y >= 0 ? spec.window_max_y : spec.height - 1;
  std::vector<uint8_t> window;
  put_i32(&window, spec.window_min_x);
  put_i32(&window, spec.window_min_y);
  put_i32(&window, max_x);
  put_i32(&window, max_y);
  put_attribute(&out, "dataWindow", "box2i", window);

  put_attribute(&out, "lineOrder", "lineOrder",
                std::vector<uint8_t>{static_cast<uint8_t>(spec.line_order)});

  out.push_back(0u); /* 头部结束 */

  const size_t lines_per_block = spec.compression == kCompressionZip ? 16u : 1u;
  std::vector<int32_t> block_starts;
  for (int32_t y = 0; y < spec.height; y += static_cast<int32_t>(lines_per_block)) {
    block_starts.push_back(y);
  }
  /* DECREASING_Y 的偏移表从最上面一块开始；块内的 y 仍是该块**最低**那一行，
   * 行号自下而上排（与 OpenEXR 的 numLines 计算一致）。 */
  if (spec.line_order == 1) std::reverse(block_starts.begin(), block_starts.end());

  const size_t table_at = out.size();
  out.resize(table_at + block_starts.size() * 8u, 0u);

  for (size_t block = 0; block < block_starts.size(); ++block) {
    const int32_t start_y = block_starts[block];
    const size_t lines = std::min<size_t>(lines_per_block,
                                          static_cast<size_t>(spec.height - start_y));

    std::vector<uint8_t> raw;
    for (size_t line = 0; line < lines; ++line) {
      const int32_t y = start_y + static_cast<int32_t>(line);
      for (size_t c = 0; c < spec.channels.size(); ++c) {
        for (int32_t x = 0; x < spec.width; ++x) {
          const float value = sample_at(spec, c, x, y);
          if (spec.channels[c].type == kTypeHalf) {
            const uint16_t bits = exact_half_bits(value);
            raw.push_back(static_cast<uint8_t>(bits & 0xffu));
            raw.push_back(static_cast<uint8_t>((bits >> 8) & 0xffu));
          } else {
            put_f32(&raw, value);
          }
        }
      }
    }

    std::vector<uint8_t> payload;
    if (spec.compression == kCompressionNone || spec.raw_blocks) {
      payload = raw;
    } else {
      const std::vector<uint8_t> laid = forward_layout(raw);
      if (spec.compression == kCompressionRle) {
        payload = rle_compress(laid);
      } else {
#ifdef TEST_EXR_ZLIB
        payload = deflate_bytes(laid);
#endif
      }
      /*
       * 与 OpenEXR 写侧同一规则：压缩没有收益时块内直接放原样像素
       * （dataSize == 该块原始字节数）。夹具必须照做，否则构造出的
       * 「压缩流」比原文还大，会被读侧按 dataSize > rawSize 如实拒绝。
       */
      if (payload.empty() || payload.size() >= raw.size()) payload = raw;
    }

    const uint64_t offset = static_cast<uint64_t>(out.size());
    for (size_t byte = 0; byte < 8u; ++byte) {
      out[table_at + block * 8u + byte] = static_cast<uint8_t>((offset >> (byte * 8u)) & 0xffu);
    }
    put_i32(&out, start_y);
    put_i32(&out, static_cast<int32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
  }
  return out;
}

bool decode(const std::vector<uint8_t>& bytes, compositor::CodecFloatImage* image,
            std::string* error) {
  return compositor::exr_decode_float(bytes.data(), bytes.size(), image, error);
}

bool decode_rejected_with(const std::vector<uint8_t>& bytes, const char* needle) {
  compositor::CodecFloatImage image;
  std::string error;
  if (decode(bytes, &image, &error)) return false;
  return error.find(needle) != std::string::npos;
}

/* 常见形状：w*h 的 R/G/B/A，值由回调给（便于每个测试构造独立样例）。 */
ExrSpec rgb_spec(int32_t width, int32_t height, int32_t sample_type, int32_t compression) {
  ExrSpec spec;
  spec.width = width;
  spec.height = height;
  spec.compression = compression;
  spec.channels = {{"R", sample_type}, {"G", sample_type}, {"B", sample_type},
                   {"A", sample_type}};
  spec.samples.resize(spec.channels.size());
  return spec;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

/* 无 enginePixelFormat 的上游风格 manifest（本引擎的宽位深扩展键缺席）。 */
std::string plain_exr_manifest(const std::string& uuid, const std::string& image_file) {
  return std::string("{\n"
                     "  \"format\": \"com.compositor.project\",\n"
                     "  \"version\": 13,\n"
                     "  \"colorSpace\": \"sRGB\",\n"
                     "  \"resolution\": 72,\n"
                     "  \"documentID\": \"E621E1F8-C36C-495A-93FC-0C247A3E6E5F\",\n"
                     "  \"width\": 4,\n"
                     "  \"height\": 4,\n"
                     "  \"layers\": [\n"
                     "    {\n"
                     "      \"id\": \"") +
         uuid +
         "\",\n"
         "      \"name\": \"Extern\",\n"
         "      \"isVisible\": true,\n"
         "      \"transform\": {\"origin\":[0,0],\"size\":[4,4],\"rotation\":0,"
         "\"flipX\":false,\"flipY\":false,\"sampling\":\"High quality\"},\n"
         "      \"imageFile\": \"" +
         image_file +
         "\",\n"
         "      \"isGroup\": false\n"
         "    }\n"
         "  ]\n"
         "}\n";
}

}  // namespace

/* ------------------------------------------------------- 未压缩往返 */

COMP_TEST(exr_none_float_channels_round_trip_exactly) {
  ExrSpec spec = rgb_spec(4, 3, kTypeFloat, kCompressionNone);
  const float palette[] = {0.0f, 0.1f, 1.0f, 1234.5f, -2.25f, 1e-8f,
                           65504.0f, 0.5f, 3.5f};
  const size_t palette_size = sizeof(palette) / sizeof(palette[0]);
  for (size_t c = 0; c < spec.channels.size(); ++c) {
    spec.samples[c].resize(static_cast<size_t>(spec.width) * spec.height);
    for (size_t i = 0; i < spec.samples[c].size(); ++i) {
      spec.samples[c][i] = palette[(i * (c + 3u)) % palette_size];
    }
  }

  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  COMP_CHECK(image.width == spec.width);
  COMP_CHECK(image.height == spec.height);
  COMP_CHECK(image.rgba.size() == static_cast<size_t>(spec.width) * spec.height * 4u);

  size_t diffs = 0;
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < spec.channels.size(); ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        /* 通道顺序 R,G,B,A 与像素顺序一致。 */
        if (image.rgba[pixel + c] != sample_at(spec, c, x, y)) ++diffs;
      }
    }
  }
  COMP_CHECK_MSG(diffs == 0, "FLOAT NONE round trip is bit exact");

  int32_t declared_w = 0;
  int32_t declared_h = 0;
  COMP_CHECK(compositor::exr_declared_size(bytes.data(), bytes.size(), &declared_w, &declared_h,
                                           &error));
  COMP_CHECK(declared_w == spec.width && declared_h == spec.height);
}

COMP_TEST(exr_none_half_channels_round_trip_exactly) {
  ExrSpec spec = rgb_spec(5, 2, kTypeHalf, kCompressionNone);
  const float palette[] = {0.0f,  0.25f, 0.5f,   1.0f,  1.5f,  2.0f,
                           4.0f,  8.0f,  -1.5f,  -0.25f, 0.75f, 65504.0f};
  const size_t palette_size = sizeof(palette) / sizeof(palette[0]);
  for (float value : palette) {
    /* 夹具前置条件：值必须能被 half 精确表示，否则期望值没有意义。 */
    COMP_CHECK(exact_half_bits(value) != 0xffffu);
  }

  for (size_t c = 0; c < spec.channels.size(); ++c) {
    spec.samples[c].resize(static_cast<size_t>(spec.width) * spec.height);
    for (size_t i = 0; i < spec.samples[c].size(); ++i) {
      spec.samples[c][i] = palette[(i + c * 5u) % palette_size];
    }
  }

  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  COMP_CHECK(image.width == spec.width && image.height == spec.height);
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < spec.channels.size(); ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        COMP_CHECK(image.rgba[pixel + c] == sample_at(spec, c, x, y));
      }
    }
  }
}

/* ------------------------------------------------- 压缩：重排 + 预测 */

namespace {

/*
 * 压缩夹具的样例：**每一行内逐像素相同、行与行、通道与通道不同**。
 * 这样重排后偶数半区是整段相同字节、奇数半区是成串的通道常量，预测器又
 * 把它们变成成串 0x80 —— 压缩才有收益，块才会真的走压缩分支（OpenEXR
 * 在压缩没收益时直接写原样像素，那样就测不到前处理了）。
 * 行内常量仍能暴露任何一步做错：通道与行号都会解错位。
 */
void fill_rle_samples(ExrSpec* spec) {
  const float rows[3][3] = {{0.25f, 1.5f, 4.0f},
                            {0.5f, 2.0f, 8.0f},
                            {1.0f, 0.75f, 0.25f}};
  for (size_t c = 0; c < spec->channels.size(); ++c) {
    spec->samples[c].resize(static_cast<size_t>(spec->width) * spec->height);
    for (int32_t y = 0; y < spec->height; ++y) {
      const float value = c < 3u ? rows[c][static_cast<size_t>(y) % 3u] : 1.0f;
      for (int32_t x = 0; x < spec->width; ++x) {
        spec->samples[c][static_cast<size_t>(y) * spec->width + x] = value;
      }
    }
  }
}

}  // namespace

COMP_TEST(exr_rle_blocks_undo_reorder_and_predictor) {
  ExrSpec spec = rgb_spec(5, 3, kTypeHalf, kCompressionRle);
  fill_rle_samples(&spec);

  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  COMP_CHECK(image.width == spec.width && image.height == spec.height);
  size_t diffs = 0;
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < 4u; ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        if (image.rgba[pixel + c] != sample_at(spec, c, x, y)) ++diffs;
      }
    }
  }
  COMP_CHECK_MSG(diffs == 0, "RLE blocks must be de-predicted and de-interleaved");
}

COMP_TEST(exr_decreasing_line_order_places_rows_by_chunk_y) {
  ExrSpec spec = rgb_spec(3, 4, kTypeHalf, kCompressionRle);
  fill_rle_samples(&spec);
  spec.line_order = 1; /* DECREASING_Y */
  /* 行内容按 y 变化，行放错位置立刻能看出来。 */
  for (size_t c = 0; c < spec.channels.size(); ++c) {
    for (int32_t y = 0; y < spec.height; ++y) {
      for (int32_t x = 0; x < spec.width; ++x) {
        spec.samples[c][static_cast<size_t>(y) * spec.width + x] =
            static_cast<float>(y) * 0.5f + static_cast<float>(x) * 0.25f;
      }
    }
  }

  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  size_t diffs = 0;
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < 4u; ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        if (image.rgba[pixel + c] != sample_at(spec, c, x, y)) ++diffs;
      }
    }
  }
  COMP_CHECK_MSG(diffs == 0, "DECREASING_Y chunks are placed by their own y, not table index");
}

#ifdef TEST_EXR_ZLIB
COMP_TEST(exr_zip_blocks_span_sixteen_scanlines) {
  /* 17 行 = 一个 16 行块 + 一个 1 行残块：块行数算错会直接解压尺寸不符。 */
  ExrSpec spec = rgb_spec(3, 17, kTypeFloat, kCompressionZip);
  for (size_t c = 0; c < spec.channels.size(); ++c) {
    spec.samples[c].resize(static_cast<size_t>(spec.width) * spec.height);
    for (size_t i = 0; i < spec.samples[c].size(); ++i) {
      spec.samples[c][i] = static_cast<float>((i * (c + 2u)) % 97u) * 0.03125f;
    }
  }

  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  COMP_CHECK(image.width == spec.width && image.height == spec.height);
  size_t diffs = 0;
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < 4u; ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        if (image.rgba[pixel + c] != sample_at(spec, c, x, y)) ++diffs;
      }
    }
  }
  COMP_CHECK_MSG(diffs == 0, "ZIP blocks are laid out in 16-scanline units");
}

COMP_TEST(exr_zips_blocks_are_single_scanline) {
  ExrSpec spec = rgb_spec(8, 3, kTypeHalf, kCompressionZips);
  fill_rle_samples(&spec);
  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  COMP_CHECK(image.width == spec.width && image.height == spec.height);
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < 4u; ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        COMP_CHECK(image.rgba[pixel + c] == sample_at(spec, c, x, y));
      }
    }
  }
}

COMP_TEST(exr_zip_raw_block_needs_no_decompression) {
  /* dataSize == 该块原始字节数：块内是原样像素，不得再走解压/逆预测。 */
  ExrSpec spec = rgb_spec(2, 4, kTypeHalf, kCompressionZip);
  fill_rle_samples(&spec);
  spec.raw_blocks = true;

  const std::vector<uint8_t> bytes = build_exr(spec);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(decode(bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return; /* 解码失败时不要继续按下标访问，免得崩溃盖住断言 */
  for (int32_t y = 0; y < spec.height; ++y) {
    for (int32_t x = 0; x < spec.width; ++x) {
      for (size_t c = 0; c < 4u; ++c) {
        const size_t pixel = (static_cast<size_t>(y) * spec.width + x) * 4u;
        COMP_CHECK(image.rgba[pixel + c] == sample_at(spec, c, x, y));
      }
    }
  }
}
#endif  /* TEST_EXR_ZLIB */

/* ------------------------------------------------------------ 能力边界 */

COMP_TEST(exr_rejects_compressions_it_does_not_decode) {
  struct Case {
    int32_t compression;
    const char* name;
  };
  const Case cases[] = {{4, "PIZ"},   {5, "PXR24"}, {6, "B44"},
                        {7, "B44A"},  {8, "DWAA"},  {9, "DWAB"}};
  for (const Case& item : cases) {
    ExrSpec spec = rgb_spec(2, 2, kTypeHalf, item.compression);
    fill_rle_samples(&spec);
    const std::vector<uint8_t> bytes = build_exr(spec);
    COMP_CHECK_MSG(decode_rejected_with(bytes, item.name),
                   "unsupported compression must be named in the error");
  }
}

COMP_TEST(exr_rejects_container_features_it_does_not_decode) {
  const auto bytes_with = [](const ExrSpec& spec) { return build_exr(spec); };

  ExrSpec tiled = rgb_spec(2, 2, kTypeHalf, kCompressionNone);
  fill_rle_samples(&tiled);
  tiled.flags = kFlagTiled;
  COMP_CHECK(decode_rejected_with(bytes_with(tiled), "tiled"));

  ExrSpec deep = rgb_spec(2, 2, kTypeHalf, kCompressionNone);
  fill_rle_samples(&deep);
  deep.flags = kFlagDeep;
  COMP_CHECK(decode_rejected_with(bytes_with(deep), "deep"));

  ExrSpec multipart = rgb_spec(2, 2, kTypeHalf, kCompressionNone);
  fill_rle_samples(&multipart);
  multipart.flags = kFlagMultipart;
  COMP_CHECK(decode_rejected_with(bytes_with(multipart), "multipart"));

  ExrSpec uint_channels = rgb_spec(2, 2, 0, kCompressionNone);
  fill_rle_samples(&uint_channels);
  COMP_CHECK(decode_rejected_with(bytes_with(uint_channels), "UINT"));

  ExrSpec subsampled = rgb_spec(2, 2, kTypeHalf, kCompressionNone);
  fill_rle_samples(&subsampled);
  subsampled.subsampled = true;
  COMP_CHECK(decode_rejected_with(bytes_with(subsampled), "subsampled"));

  ExrSpec random_y = rgb_spec(2, 2, kTypeHalf, kCompressionNone);
  fill_rle_samples(&random_y);
  random_y.line_order = 2;
  COMP_CHECK(decode_rejected_with(bytes_with(random_y), "RANDOM_Y"));

  /* 魔数损坏：必须是「不是 EXR」，而不是解出垃圾。 */
  std::vector<uint8_t> bad_magic = bytes_with(random_y);
  bad_magic[0] = 0u;
  COMP_CHECK(decode_rejected_with(bad_magic, "magic"));

  /* 截断到头部中间：越界读取必须以可读原因失败，不能读穿缓冲。 */
  std::vector<uint8_t> truncated = bytes_with(random_y);
  truncated.resize(20u);
  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK(!decode(truncated, &image, &error));
  COMP_CHECK(!error.empty());
}

COMP_TEST(exr_rejects_forged_oversized_data_window_before_allocating) {
  /* 30000×30000 = 9 亿像素，超过画布上限：头部就要拒绝，绝不按声明分配。 */
  ExrSpec huge = rgb_spec(4, 4, kTypeFloat, kCompressionNone);
  huge.samples.assign(4, std::vector<float>(16, 1.0f));
  huge.window_max_x = 29999;
  huge.window_max_y = 29999;
  const std::vector<uint8_t> huge_bytes = build_exr(huge);

  int32_t width = 0;
  int32_t height = 0;
  std::string error;
  COMP_CHECK_MSG(!compositor::exr_declared_size(huge_bytes.data(), huge_bytes.size(), &width,
                                                &height, &error),
                 "declared size probe rejects the forged window");
  COMP_CHECK(error.find("canvas") != std::string::npos);
  COMP_CHECK(decode_rejected_with(huge_bytes, "canvas"));

  /* 闭区间跨度超过 int32：宽度按 int64 算，必须报越界而不是回绕成小尺寸。 */
  ExrSpec overflow = rgb_spec(4, 4, kTypeFloat, kCompressionNone);
  overflow.samples.assign(4, std::vector<float>(16, 1.0f));
  overflow.window_min_x = -2000000000;
  overflow.window_max_x = 2000000000;
  const std::vector<uint8_t> overflow_bytes = build_exr(overflow);
  COMP_CHECK(!compositor::exr_declared_size(overflow_bytes.data(), overflow_bytes.size(), &width,
                                            &height, &error));
  COMP_CHECK(error.find("canvas") != std::string::npos);
}

COMP_TEST(exr_channel_selection_handles_y_and_layered_names) {
  /* 单通道 Y：灰度复制到 RGB，alpha 缺省 1。 */
  ExrSpec y_only;
  y_only.width = 3;
  y_only.height = 1;
  y_only.channels = {{"Y", kTypeHalf}};
  y_only.samples = {{0.75f, 0.5f, 0.25f}};
  compositor::CodecFloatImage image;
  std::string error;
  const std::vector<uint8_t> y_bytes = build_exr(y_only);
  COMP_CHECK_MSG(decode(y_bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return;
  COMP_CHECK(image.rgba[0] == 0.75f && image.rgba[1] == 0.75f && image.rgba[2] == 0.75f);
  COMP_CHECK(image.rgba[3] == 1.0f);
  COMP_CHECK(image.rgba[8] == 0.25f && image.rgba[11] == 1.0f);

  /* 分层命名：beauty.R/G/B/A 归成一组（前缀相同），按 R/G/B/A 取值。 */
  ExrSpec layered;
  layered.width = 2;
  layered.height = 1;
  layered.channels = {{"beauty.R", kTypeHalf},
                      {"beauty.G", kTypeHalf},
                      {"beauty.B", kTypeHalf},
                      {"beauty.A", kTypeHalf}};
  layered.samples = {{0.25f, 0.5f}, {0.5f, 1.0f}, {1.0f, 2.0f}, {0.5f, 1.0f}};
  const std::vector<uint8_t> layered_bytes = build_exr(layered);
  COMP_CHECK_MSG(decode(layered_bytes, &image, &error), error.c_str());
  if (image.rgba.empty()) return;
  COMP_CHECK(image.rgba[0] == 0.25f && image.rgba[1] == 0.5f && image.rgba[2] == 1.0f);
  COMP_CHECK(image.rgba[3] == 0.5f);
  COMP_CHECK(image.rgba[4] == 0.5f && image.rgba[5] == 1.0f && image.rgba[6] == 2.0f);
  COMP_CHECK(image.rgba[7] == 1.0f);
}

/* ---------------------------------------------------- 写侧与读侧互证 */

COMP_TEST(exr_encode_round_trips_what_it_writes) {
  const int32_t width = 3;
  const int32_t height = 2;
  const float half_values[] = {0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f,
                               4.0f, 8.0f,  -1.5f, 0.75f, 0.125f, 65504.0f};
  std::vector<float> rgba(static_cast<size_t>(width) * height * 4u, 0.0f);
  for (size_t i = 0; i < rgba.size(); ++i) {
    rgba[i] = half_values[i % (sizeof(half_values) / sizeof(half_values[0]))];
  }

  std::string error;
  std::vector<uint8_t> half_file;
  COMP_CHECK_MSG(compositor::exr_encode_float(rgba.data(), width, height,
                                              compositor::kExrSampleHalf, &half_file, &error),
                 error.c_str());
  compositor::CodecFloatImage decoded;
  COMP_CHECK_MSG(decode(half_file, &decoded, &error), error.c_str());
  if (decoded.rgba.empty()) return;
  COMP_CHECK(decoded.width == width && decoded.height == height);
  size_t half_diffs = 0;
  for (size_t i = 0; i < rgba.size(); ++i) {
    if (decoded.rgba[i] != rgba[i]) ++half_diffs;
  }
  COMP_CHECK_MSG(half_diffs == 0, "HALF writer round trips exact half values");

  /* FLOAT 通道必须保住 half 表示不了的尾数（这正是 RGBA16 资产要的类型）。 */
  std::vector<float> precise(static_cast<size_t>(width) * height * 4u, 1.0f);
  for (size_t i = 0; i < precise.size(); ++i) {
    precise[i] = 1.234567f + static_cast<float>(i) * 0.000123f;
  }
  std::vector<uint8_t> float_file;
  COMP_CHECK_MSG(compositor::exr_encode_float(precise.data(), width, height,
                                              compositor::kExrSampleFloat, &float_file, &error),
                 error.c_str());
  COMP_CHECK_MSG(decode(float_file, &decoded, &error), error.c_str());
  if (decoded.rgba.empty()) return;
  size_t float_diffs = 0;
  for (size_t i = 0; i < precise.size(); ++i) {
    if (decoded.rgba[i] != precise[i]) ++float_diffs;
  }
  COMP_CHECK_MSG(float_diffs == 0, "FLOAT writer is lossless");

  int32_t declared_w = 0;
  int32_t declared_h = 0;
  COMP_CHECK(compositor::exr_declared_size(float_file.data(), float_file.size(), &declared_w,
                                           &declared_h, &error));
  COMP_CHECK(declared_w == width && declared_h == height);
}

/* -------------------------------------------- .comp 宽位深资产往返 */

COMP_TEST(format_comp_round_trips_rgba16_layer_exactly) {
  const std::string root = "build/exr_rgba16.comp";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  compositor::Document* document =
      compositor::Document::create(nullptr, 4, 2, 72.0f, "sRGB");
  COMP_CHECK(document != nullptr);
  if (document == nullptr) return;

  compositor::Layer layer;
  layer.id = 1;
  layer.name = "wide16";
  layer.pixels.resize(4, 2, COMP_PIXEL_RGBA16);
  /* 低 5 位必须活下来：0x0001 / 0xABCD / 0xFFFE 的尾数 half 存不下，
   * 若资产被写成 HALF，往返就不可能逐字节相等。 */
  std::vector<uint8_t>& target = layer.pixels.writable_pixels();
  uint16_t* words = reinterpret_cast<uint16_t*>(target.data());
  /* 预乘约束：颜色不得超过 alpha；这里每 4 个值一组是 R,G,B,A。 */
  const uint16_t alpha0 = 0xFFFFu;
  const uint16_t alpha1 = 0x8001u;
  const uint16_t pixels16[8][4] = {
      {0x0001u, 0x1234u, 0xABCDu, alpha0},
      {0xFFFEu, 0x8001u, 0x00FFu, alpha0},
      {0x1234u, 0x0001u, 0x00FFu, alpha0},
      {0xABCDu, 0x4321u, 0x1111u, alpha0},
      {0x0001u, 0x1234u, 0x7FFFu, alpha1},
      {0x8001u, 0x7000u, 0x6000u, alpha1},
      {0x4321u, 0x3210u, 0x2100u, alpha1},
      {0x1111u, 0x0101u, 0x0001u, alpha1},
  };
  for (size_t i = 0; i < 8u * 4u; ++i) {
    words[i] = pixels16[i / 4u][i % 4u];
  }
  const std::vector<uint8_t> original = target;

  document->state().layers.push_back(std::move(layer));

  std::string error;
  COMP_CHECK_MSG(compositor::save_project(*document, root, &error), error.c_str());

  const std::string manifest = read_text(root + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("\"enginePixelFormat\": 1") != std::string::npos,
                 "manifest declares RGBA16 for the wide layer");
  COMP_CHECK_MSG(manifest.find(".exr") != std::string::npos,
                 "RGBA16 layer is stored as an EXR asset, not a PNG");

  compositor::Document* loaded = nullptr;
  COMP_CHECK_MSG(compositor::load_project(root, nullptr, &loaded, &error), error.c_str());
  COMP_CHECK(loaded != nullptr);
  if (loaded != nullptr) {
    COMP_CHECK(loaded->state().layers.size() == 1u);
    if (!loaded->state().layers.empty()) {
      const compositor::ImageBuffer& restored = loaded->state().layers[0].pixels;
      COMP_CHECK(restored.format == COMP_PIXEL_RGBA16);
      COMP_CHECK(restored.width == 4 && restored.height == 2);
      COMP_CHECK_MSG(restored.pixel_data() == original,
                     "RGBA16 layer survives save/load byte for byte");
    }
    compositor::Document::destroy(loaded);
  }

  compositor::Document::destroy(document);
}

COMP_TEST(format_comp_round_trips_rgba16f_layer_exactly) {
  const std::string root = "build/exr_rgba16f.comp";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  compositor::Document* document =
      compositor::Document::create(nullptr, 4, 2, 72.0f, "sRGB");
  COMP_CHECK(document != nullptr);
  if (document == nullptr) return;

  compositor::Layer layer;
  layer.id = 1;
  layer.name = "hdr";
  layer.pixels.resize(4, 2, COMP_PIXEL_RGBA16F);
  /* 全部是 half 精确值；其中 4.0 / 2.0 高于白点，必须原样存回。 */
  const float values[8][4] = {
      {1.0f, 0.5f, 0.25f, 1.0f},
      {4.0f, 2.0f, 1.0f, 1.0f},
      {1.5f, 0.75f, 0.5f, 1.0f},
      {0.0f, 0.0f, 0.0f, 0.0f},
      {0.25f, 0.125f, 0.0625f, 0.5f},
      {0.5f, 0.25f, 0.125f, 0.5f},
      {0.0625f, 0.03125f, 0.015625f, 0.0625f},
      {2.0f, 1.0f, 0.5f, 1.0f},
  };
  std::vector<uint8_t>& target = layer.pixels.writable_pixels();
  uint16_t* words = reinterpret_cast<uint16_t*>(target.data());
  for (size_t i = 0; i < 8u * 4u; ++i) {
    const uint16_t bits = exact_half_bits(values[i / 4u][i % 4u]);
    COMP_CHECK(bits != 0xffffu);
    words[i] = bits;
  }
  const std::vector<uint8_t> original = target;

  document->state().layers.push_back(std::move(layer));

  std::string error;
  COMP_CHECK_MSG(compositor::save_project(*document, root, &error), error.c_str());
  const std::string manifest = read_text(root + "/manifest.json");
  COMP_CHECK_MSG(manifest.find("\"enginePixelFormat\": 2") != std::string::npos,
                 "manifest declares RGBA16F for the HDR layer");
  COMP_CHECK_MSG(manifest.find(".exr") != std::string::npos,
                 "RGBA16F layer is stored as an EXR asset");

  compositor::Document* loaded = nullptr;
  COMP_CHECK_MSG(compositor::load_project(root, nullptr, &loaded, &error), error.c_str());
  COMP_CHECK(loaded != nullptr);
  if (loaded != nullptr) {
    COMP_CHECK(loaded->state().layers.size() == 1u);
    if (!loaded->state().layers.empty()) {
      const compositor::ImageBuffer& restored = loaded->state().layers[0].pixels;
      COMP_CHECK(restored.format == COMP_PIXEL_RGBA16F);
      COMP_CHECK(restored.width == 4 && restored.height == 2);
      COMP_CHECK_MSG(restored.pixel_data() == original,
                     "RGBA16F layer survives save/load bit for bit");
    }
    compositor::Document::destroy(loaded);
  }

  compositor::Document::destroy(document);
}

COMP_TEST(format_comp_reads_exr_asset_without_engine_pixel_format) {
  /* 上游风格的工程没有 enginePixelFormat：资产按内容判宽位深/HDR，
   * 口径与外部导入一致（peak > 1.001 才是 HDR）。 */
  const std::string uuid = "5F2B1C4A-1D3E-4A6B-8C9D-0E1F2A3B4C5D";
  const std::string hdr_root = "build/exr_import_hdr.comp";
  const std::string wide_root = "build/exr_import_wide.comp";
  std::error_code ec;
  std::filesystem::remove_all(hdr_root, ec);
  std::filesystem::remove_all(wide_root, ec);
  std::filesystem::create_directories(hdr_root + "/images", ec);
  std::filesystem::create_directories(wide_root + "/images", ec);

  const auto make_asset = [&](float peak) {
    ExrSpec spec = rgb_spec(4, 4, kTypeFloat, kCompressionNone);
    for (size_t c = 0; c < spec.channels.size(); ++c) {
      spec.samples[c].assign(16u, peak);
    }
    spec.samples[3].assign(16u, 1.0f); /* 直通 alpha */
    return build_exr(spec);
  };

  const std::vector<uint8_t> hdr_asset = make_asset(2.0f);   /* peak 2.0 > 1.001 */
  const std::vector<uint8_t> wide_asset = make_asset(0.8f);  /* peak 0.8 <= 1.001 */
  const std::string image_file = uuid + ".exr";
  write_bytes(hdr_root + "/images/" + image_file, hdr_asset);
  write_bytes(wide_root + "/images/" + image_file, wide_asset);
  {
    std::ofstream manifest(hdr_root + "/manifest.json", std::ios::binary | std::ios::trunc);
    const std::string text = plain_exr_manifest(uuid, image_file);
    manifest.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  {
    std::ofstream manifest(wide_root + "/manifest.json", std::ios::binary | std::ios::trunc);
    const std::string text = plain_exr_manifest(uuid, image_file);
    manifest.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  std::string error;
  compositor::Document* loaded = nullptr;
  COMP_CHECK_MSG(compositor::load_project(hdr_root, nullptr, &loaded, &error), error.c_str());
  if (loaded != nullptr) {
    COMP_CHECK(loaded->state().layers.size() == 1u);
    if (!loaded->state().layers.empty()) {
      COMP_CHECK_MSG(loaded->state().layers[0].pixels.format == COMP_PIXEL_RGBA16F,
                     "peak above white is imported as an HDR layer");
    }
    compositor::Document::destroy(loaded);
    loaded = nullptr;
  }

  COMP_CHECK_MSG(compositor::load_project(wide_root, nullptr, &loaded, &error), error.c_str());
  if (loaded != nullptr) {
    COMP_CHECK(loaded->state().layers.size() == 1u);
    if (!loaded->state().layers.empty()) {
      COMP_CHECK_MSG(loaded->state().layers[0].pixels.format == COMP_PIXEL_RGBA16,
                     "a non-HDR EXR asset is imported as a 16-bit layer");
    }
    compositor::Document::destroy(loaded);
  }
}

/* -------------------------------------------------------- 16 位内核 */

COMP_TEST(deep_u16_kernels_clamp_and_saturate) {
  const size_t width = 4;
  const size_t height = 1;
  const size_t float_stride = width * 4u * sizeof(float);
  std::vector<float> linear(static_cast<size_t>(width) * 4u);
  const float input[4][4] = {
      {2.0f, -1.0f, 1.0f, 1.0f},                     /* 过曝与负值 */
      {0.5f, 0.25f, 0.0f, 1.0f},                     /* 常规 */
      {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f},
      {1.0f, 1.0f, 1.0f, 0.0f},                      /* alpha 0：颜色归零 */
  };
  for (size_t i = 0; i < width * 4u; ++i) {
    linear[i] = input[i / 4u][i % 4u];
  }
  std::vector<uint16_t> wide(width * 4u, 0u);
  /* DeepPixels.h 是 extern "C" 全局声明，引擎内部同样不加命名空间限定。 */
  deep_u16_take_linear(linear.data(), float_stride, wide.data(),
                       width * 4u * sizeof(uint16_t), width, height);

  COMP_CHECK_MSG(wide[0] == 65535u, "above white saturates to 65535");
  COMP_CHECK_MSG(wide[1] == 0u, "negative light clamps to 0");
  COMP_CHECK_MSG(wide[4] < 65535u && wide[4] > 0u, "mid light stays inside the range");
  COMP_CHECK_MSG(wide[8] == 0u, "NaN light lands on 0, not on a bright value");
  COMP_CHECK_MSG(wide[12] == 0u, "alpha 0 writes no colour");
  COMP_CHECK_MSG(wide[15] == 0u, "alpha 0 is stored as 0");

  /* 反向：16 位预乘 -> 线性 -> 16 位，必须逐值稳定（曲线在 float 上的往返
   * 误差远小于半个 LSB）。 */
  std::vector<uint16_t> round_trip_input(width * 4u, 0u);
  const uint16_t seeds[] = {0x0001u, 0x1234u, 0xABCDu, 0xFFFEu};
  for (size_t i = 0; i < width; ++i) {
    round_trip_input[i * 4u + 0] = seeds[i];
    round_trip_input[i * 4u + 1] = static_cast<uint16_t>(seeds[i] / 2u);
    round_trip_input[i * 4u + 2] = 0u;
    round_trip_input[i * 4u + 3] = 0xFFFFu;
  }
  std::vector<float> back_linear(width * 4u);
  deep_u16_to_linear(round_trip_input.data(), width * 4u * sizeof(uint16_t),
                     back_linear.data(), float_stride, width, height);
  std::vector<uint16_t> restored(width * 4u, 0u);
  deep_u16_take_linear(back_linear.data(), float_stride, restored.data(),
                       width * 4u * sizeof(uint16_t), width, height);
  COMP_CHECK_MSG(restored == round_trip_input,
                 "u16 <-> linear kernels round trip every value on this grid");
}
