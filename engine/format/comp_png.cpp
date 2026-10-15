// AI生成
/*
 * comp_png.cpp — 最小 PNG 编解码实现（零第三方依赖）。
 *
 * 参见 comp_png.h 的契约说明。实现分三层：
 *   1) 字节级助手：CRC-32 / Adler-32 / 大端写入 / 分块组装
 *   2) zlib 流：编码走「存储块」，解码支持存储块 + 固定 Huffman + 动态 Huffman
 *   3) PNG 容器：IHDR/PLTE/tRNS/IDAT/IEND 解析、行过滤器反变换、位深与色彩类型归一
 */
#include "core/comp_geometry.h"
#include "format/comp_png.h"

#include <algorithm>
#include <cstring>

namespace compositor {

namespace {

/* ------------------------------------------------------------ 字节级助手 */

constexpr uint8_t kPngSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

uint32_t crc32_of(const uint8_t* data, size_t size) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    ready = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32_of(const uint8_t* data, size_t size) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void append_be32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value >> 24));
  out->push_back(static_cast<uint8_t>(value >> 16));
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value));
}

uint32_t read_be32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

/* 写入一个 PNG 分块：长度 + 类型 + 数据 + CRC(类型+数据)。 */
void append_chunk(std::vector<uint8_t>* out, const char type[5],
                  const std::vector<uint8_t>& payload) {
  append_be32(out, static_cast<uint32_t>(payload.size()));
  const size_t start = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), payload.begin(), payload.end());
  append_be32(out, crc32_of(out->data() + start, out->size() - start));
}

/* ------------------------------------------------ zlib：存储块编码 */

void append_zlib_stored(std::vector<uint8_t>* out, const std::vector<uint8_t>& raw) {
  out->push_back(0x78); /* CM=8, CINFO=7 */
  out->push_back(0x01); /* FCHECK：使 (0x78<<8|0x01) % 31 == 0 */
  size_t pos = 0;
  const size_t total = raw.size();
  if (total == 0) {
    out->push_back(0x01); /* BFINAL=1, BTYPE=00 */
    out->push_back(0x00);
    out->push_back(0x00);
    out->push_back(0xFF);
    out->push_back(0xFF);
  }
  while (pos < total) {
    const size_t block = std::min<size_t>(65535u, total - pos);
    const bool last = pos + block >= total;
    out->push_back(last ? 0x01 : 0x00);
    out->push_back(static_cast<uint8_t>(block & 0xFFu));
    out->push_back(static_cast<uint8_t>((block >> 8) & 0xFFu));
    const uint16_t nlen = static_cast<uint16_t>(~static_cast<uint16_t>(block));
    out->push_back(static_cast<uint8_t>(nlen & 0xFFu));
    out->push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));
    out->insert(out->end(), raw.begin() + static_cast<long>(pos),
                raw.begin() + static_cast<long>(pos + block));
    pos += block;
  }
  append_be32(out, adler32_of(raw.data(), total));
}

/* ------------------------------------------------ zlib：解压（inflate） */

struct BitReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  int bit = 0;
  bool ok = true;

  int getbit() {
    if (pos >= size) {
      ok = false;
      return 0;
    }
    const int value = (data[pos] >> bit) & 1;
    if (++bit == 8) {
      bit = 0;
      ++pos;
    }
    return value;
  }

  uint32_t getbits(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      value |= static_cast<uint32_t>(getbit()) << i;
    }
    return value;
  }

  void align_byte() {
    if (bit != 0) {
      bit = 0;
      ++pos;
    }
  }
};

struct Huffman {
  uint16_t count[16] = {0};
  uint16_t symbol[288] = {0};
  int max_bits = 0;
};

bool huffman_build(Huffman* h, const uint8_t* lengths, int count) {
  for (int i = 0; i < 16; ++i) h->count[i] = 0;
  for (int i = 0; i < count; ++i) h->count[lengths[i]]++;
  if (h->count[0] == count) {
    h->max_bits = 0; /* 全部长度为零：该表无码字 */
    return true;
  }
  int left = 1;
  for (int len = 1; len <= 15; ++len) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return false; /* 过订阅 */
  }
  h->max_bits = 15;
  int offsets[16] = {0};
  for (int len = 1; len < 15; ++len) offsets[len + 1] = offsets[len] + h->count[len];
  for (int i = 0; i < count; ++i) {
    if (lengths[i] != 0) h->symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
  }
  return true;
}

int huffman_decode(BitReader* br, const Huffman& h) {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int len = 1; len <= h.max_bits; ++len) {
    code |= br->getbit();
    if (!br->ok) return -1;
    const int count = h.count[len];
    if (code - first < count) return h.symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                    33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};

bool zlib_inflate(const uint8_t* data, size_t size, size_t max_out,
                  std::vector<uint8_t>* out, std::string* error) {
  auto fail = [&](const char* message) -> bool {
    if (error != nullptr) *error = message;
    return false;
  };
  if (size < 2) return fail("zlib stream too short");
  const uint8_t cmf = data[0];
  const uint8_t flg = data[1];
  if ((cmf & 0x0Fu) != 8) return fail("zlib: unsupported compression method");
  if (((static_cast<uint32_t>(cmf) << 8) | flg) % 31u != 0u) return fail("zlib: bad header check");
  if ((flg & 0x20u) != 0u) return fail("zlib: preset dictionary unsupported");

  BitReader br;
  br.data = data + 2;
  br.size = size - 2;
  out->clear();

  for (;;) {
    const int last = br.getbit();
    const int type = static_cast<int>(br.getbits(2));
    if (!br.ok) return fail("zlib: truncated block header");

    if (type == 0) {
      br.align_byte();
      if (br.pos + 4 > br.size) return fail("zlib: truncated stored block");
      const uint8_t* header = br.data + br.pos;
      const uint32_t len = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8);
      const uint32_t nlen =
          static_cast<uint32_t>(header[2]) | (static_cast<uint32_t>(header[3]) << 8);
      if ((len ^ 0xFFFFu) != nlen) return fail("zlib: stored block length mismatch");
      br.pos += 4;
      if (br.pos + len > br.size) return fail("zlib: stored block overruns stream");
      if (out->size() + len > max_out) return fail("zlib: output exceeds expected size");
      out->insert(out->end(), br.data + br.pos, br.data + br.pos + len);
      br.pos += len;
    } else if (type == 1 || type == 2) {
      Huffman literal;
      Huffman distance;
      if (type == 1) {
        uint8_t lengths[288];
        for (int i = 0; i < 144; ++i) lengths[i] = 8;
        for (int i = 144; i < 256; ++i) lengths[i] = 9;
        for (int i = 256; i < 280; ++i) lengths[i] = 7;
        for (int i = 280; i < 288; ++i) lengths[i] = 8;
        if (!huffman_build(&literal, lengths, 288)) return fail("zlib: bad fixed literal tree");
        uint8_t dist_lengths[30];
        for (int i = 0; i < 30; ++i) dist_lengths[i] = 5;
        if (!huffman_build(&distance, dist_lengths, 30)) return fail("zlib: bad fixed distance tree");
      } else {
        const int hlit = static_cast<int>(br.getbits(5)) + 257;
        const int hdist = static_cast<int>(br.getbits(5)) + 1;
        const int hclen = static_cast<int>(br.getbits(4)) + 4;
        if (!br.ok || hlit > 286 || hdist > 30) return fail("zlib: bad dynamic header");
        uint8_t code_lengths[19];
        std::memset(code_lengths, 0, sizeof(code_lengths));
        for (int i = 0; i < hclen; ++i) {
          code_lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(br.getbits(3));
        }
        Huffman code_tree;
        if (!huffman_build(&code_tree, code_lengths, 19)) return fail("zlib: bad code-length tree");

        uint8_t lengths[320];
        std::memset(lengths, 0, sizeof(lengths));
        const int total = hlit + hdist;
        int index = 0;
        while (index < total) {
          const int symbol = huffman_decode(&br, code_tree);
          if (symbol < 0) return fail("zlib: bad code-length symbol");
          if (symbol < 16) {
            lengths[index++] = static_cast<uint8_t>(symbol);
          } else if (symbol == 16) {
            if (index == 0) return fail("zlib: repeat with no previous length");
            const int repeat = 3 + static_cast<int>(br.getbits(2));
            const uint8_t previous = lengths[index - 1];
            for (int i = 0; i < repeat && index < total; ++i) lengths[index++] = previous;
          } else if (symbol == 17) {
            const int repeat = 3 + static_cast<int>(br.getbits(3));
            for (int i = 0; i < repeat && index < total; ++i) lengths[index++] = 0;
          } else {
            const int repeat = 11 + static_cast<int>(br.getbits(7));
            for (int i = 0; i < repeat && index < total; ++i) lengths[index++] = 0;
          }
          if (!br.ok) return fail("zlib: truncated code lengths");
        }
        if (!huffman_build(&literal, lengths, hlit)) return fail("zlib: bad literal tree");
        if (!huffman_build(&distance, lengths + hlit, hdist)) return fail("zlib: bad distance tree");
      }

      for (;;) {
        const int symbol = huffman_decode(&br, literal);
        if (symbol < 0) return fail("zlib: bad literal/length symbol");
        if (symbol < 256) {
          if (out->size() + 1 > max_out) return fail("zlib: output exceeds expected size");
          out->push_back(static_cast<uint8_t>(symbol));
        } else if (symbol == 256) {
          break;
        } else {
          const int length_index = symbol - 257;
          if (length_index >= 29) return fail("zlib: length symbol out of range");
          const int length = kLengthBase[length_index] +
                             (kLengthExtra[length_index] != 0
                                  ? static_cast<int>(br.getbits(kLengthExtra[length_index]))
                                  : 0);
          const int distance_symbol = huffman_decode(&br, distance);
          if (distance_symbol < 0 || distance_symbol >= 30) {
            return fail("zlib: distance symbol out of range");
          }
          const int distance = kDistanceBase[distance_symbol] +
                               (kDistanceExtra[distance_symbol] != 0
                                    ? static_cast<int>(br.getbits(kDistanceExtra[distance_symbol]))
                                    : 0);
          if (static_cast<size_t>(distance) > out->size()) return fail("zlib: distance too far back");
          if (out->size() + static_cast<size_t>(length) > max_out) {
            return fail("zlib: output exceeds expected size");
          }
          const size_t start = out->size() - static_cast<size_t>(distance);
          for (int i = 0; i < length; ++i) out->push_back((*out)[start + static_cast<size_t>(i)]);
        }
        if (!br.ok) return fail("zlib: truncated compressed data");
      }
    } else {
      return fail("zlib: reserved block type");
    }
    if (last != 0) break;
  }
  return true;
}

/* ------------------------------------------------------------ PNG 容器 */

int paeth_predictor(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

/* 从一行中取某像素某通道的 8 位采样值（位深 < 8 时仅单通道，按位打包）。 */
uint8_t sample8(const uint8_t* line, int32_t x, int channels, int depth, int channel) {
  if (depth == 8) return line[static_cast<size_t>(x) * static_cast<size_t>(channels) + channel];
  if (depth == 16) {
    return line[(static_cast<size_t>(x) * static_cast<size_t>(channels) + channel) * 2u];
  }
  (void)channel;
  switch (depth) {
    case 1:
      return static_cast<uint8_t>(((line[x >> 3] >> (7 - (x & 7))) & 0x01) * 255);
    case 2:
      return static_cast<uint8_t>(((line[x >> 2] >> (6 - 2 * (x & 3))) & 0x03) * 85);
    case 4:
      return static_cast<uint8_t>(((line[x >> 1] >> (4 * (1 - (x & 1)))) & 0x0F) * 17);
    default:
      return 0;
  }
}

uint8_t premultiply(uint8_t value, uint8_t alpha) {
  if (alpha == 255) return value;
  if (alpha == 0) return 0;
  return static_cast<uint8_t>((static_cast<int>(value) * alpha + 127) / 255);
}

}  // namespace

/* ------------------------------------------------------------------ 编码 */

bool png_encode_rgba8(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "png encode: out pointer is null";
    return false;
  }
  if (premultiplied_rgba == nullptr || width <= 0 || height <= 0) {
    if (error != nullptr) *error = "png encode: invalid image dimensions or pixels";
    return false;
  }

  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> raw;
  raw.reserve((row_bytes + 1u) * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    raw.push_back(0); /* 过滤器类型 None：解码端只需反变换 */
    const uint8_t* row = premultiplied_rgba + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t r = row[x * 4 + 0];
      const uint8_t g = row[x * 4 + 1];
      const uint8_t b = row[x * 4 + 2];
      const uint8_t a = row[x * 4 + 3];
      /* 反预乘：PNG 按行业惯例存直通颜色 */
      if (a == 0) {
        raw.push_back(0);
        raw.push_back(0);
        raw.push_back(0);
      } else if (a == 255) {
        raw.push_back(r);
        raw.push_back(g);
        raw.push_back(b);
      } else {
        raw.push_back(static_cast<uint8_t>(std::min(255, (static_cast<int>(r) * 255 + a / 2) / a)));
        raw.push_back(static_cast<uint8_t>(std::min(255, (static_cast<int>(g) * 255 + a / 2) / a)));
        raw.push_back(static_cast<uint8_t>(std::min(255, (static_cast<int>(b) * 255 + a / 2) / a)));
      }
      raw.push_back(a);
    }
  }

  out->clear();
  out->insert(out->end(), kPngSignature, kPngSignature + 8);

  std::vector<uint8_t> ihdr;
  append_be32(&ihdr, static_cast<uint32_t>(width));
  append_be32(&ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8); /* 位深 */
  ihdr.push_back(6); /* 色彩类型 RGBA */
  ihdr.push_back(0); /* 压缩方法 */
  ihdr.push_back(0); /* 过滤方法 */
  ihdr.push_back(0); /* 非隔行 */
  append_chunk(out, "IHDR", ihdr);

  std::vector<uint8_t> compressed;
  append_zlib_stored(&compressed, raw);
  append_chunk(out, "IDAT", compressed);
  append_chunk(out, "IEND", std::vector<uint8_t>());
  return true;
}

bool png_encode_gray8(const uint8_t* gray, int32_t width, int32_t height,
                      std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "png encode: out pointer is null";
    return false;
  }
  if (gray == nullptr || width <= 0 || height <= 0) {
    if (error != nullptr) *error = "png encode: invalid image dimensions or pixels";
    return false;
  }

  std::vector<uint8_t> raw;
  raw.reserve((static_cast<size_t>(width) + 1u) * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = gray + static_cast<size_t>(y) * static_cast<size_t>(width);
    raw.insert(raw.end(), row, row + width);
  }

  out->clear();
  out->insert(out->end(), kPngSignature, kPngSignature + 8);

  std::vector<uint8_t> ihdr;
  append_be32(&ihdr, static_cast<uint32_t>(width));
  append_be32(&ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8); /* 位深 */
  ihdr.push_back(0); /* 色彩类型 灰度 */
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  append_chunk(out, "IHDR", ihdr);

  std::vector<uint8_t> compressed;
  append_zlib_stored(&compressed, raw);
  append_chunk(out, "IDAT", compressed);
  append_chunk(out, "IEND", std::vector<uint8_t>());
  return true;
}

/* ------------------------------------------------------------------ 解码 */

bool png_decode_rgba8(const uint8_t* data, size_t size, PngImage* out, std::string* error) {
  auto fail = [&](const char* message) -> bool {
    if (error != nullptr) *error = message;
    return false;
  };
  if (out == nullptr) return fail("png decode: out pointer is null");
  if (data == nullptr || size < 8) return fail("png decode: too short");
  if (std::memcmp(data, kPngSignature, 8) != 0) return fail("png decode: bad signature");

  int32_t width = 0;
  int32_t height = 0;
  int bit_depth = 0;
  int color_type = -1;
  bool have_ihdr = false;
  std::vector<uint8_t> idat;
  std::vector<uint8_t> palette;
  std::vector<uint8_t> transparency;

  size_t pos = 8;
  while (pos + 8 <= size) {
    const uint32_t length = read_be32(data + pos);
    const char* type = reinterpret_cast<const char*>(data + pos + 4);
    if (pos + 12u + length > size) return fail("png decode: chunk overruns file");
    const uint8_t* payload = data + pos + 8;
    if (crc32_of(data + pos + 4, 4u + length) != read_be32(payload + length)) {
      return fail("png decode: chunk CRC mismatch");
    }

    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (length != 13) return fail("png decode: bad IHDR length");
      width = static_cast<int32_t>(read_be32(payload));
      height = static_cast<int32_t>(read_be32(payload + 4));
      bit_depth = payload[8];
      color_type = payload[9];
      const int compression = payload[10];
      const int filter_method = payload[11];
      const int interlace = payload[12];
      if (compression != 0 || filter_method != 0) return fail("png decode: unsupported method");
      if (interlace != 0) return fail("png decode: interlaced (Adam7) not supported");
      if (width <= 0 || height <= 0) return fail("png decode: invalid dimensions");
      /* P1-2：IHDR 声明的尺寸必须有上界，否则伪造的 40 亿像素头会触发 GB 级分配。 */
      if (!is_valid_canvas(comp_size{width, height})) {
        return fail("png decode: canvas exceeds maximum size");
      }
      const bool depth_ok =
          (color_type == 0 && (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                               bit_depth == 8 || bit_depth == 16)) ||
          (color_type == 2 && (bit_depth == 8 || bit_depth == 16)) ||
          (color_type == 3 && (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
                               bit_depth == 8)) ||
          (color_type == 4 && (bit_depth == 8 || bit_depth == 16)) ||
          (color_type == 6 && (bit_depth == 8 || bit_depth == 16));
      if (!depth_ok) return fail("png decode: unsupported bit depth / color type");
      have_ihdr = true;
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      palette.assign(payload, payload + length);
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      transparency.assign(payload, payload + length);
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), payload, payload + length);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    pos += 12u + length;
  }
  if (!have_ihdr) return fail("png decode: missing IHDR");
  if (idat.empty()) return fail("png decode: missing IDAT");

  int channels = 1;
  switch (color_type) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return fail("png decode: unsupported color type");
  }
  if (color_type == 3 && palette.empty()) return fail("png decode: palette color type without PLTE");

  const size_t stride = (static_cast<size_t>(width) * static_cast<size_t>(channels) *
                             static_cast<size_t>(bit_depth) + 7u) / 8u;
  const size_t expected = (stride + 1u) * static_cast<size_t>(height);
  std::vector<uint8_t> raw;
  if (!zlib_inflate(idat.data(), idat.size(), expected, &raw, error)) return false;
  if (raw.size() != expected) return fail("png decode: inflated size mismatch");

  /* 命名与comp_geometry.h 的 bytes_per_pixel() 同名但语义不同（PNG 样本字节宽
     可以是 1，非像素格式），这里改名避免遮蔽全局函数。 */
  size_t sample_bytes = (static_cast<size_t>(channels) * static_cast<size_t>(bit_depth) + 7u) / 8u;
  if (sample_bytes < 1) sample_bytes = 1;

  out->width = width;
  out->height = height;
  out->rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);

  std::vector<uint8_t> previous(stride, 0);
  std::vector<uint8_t> current(stride, 0);
  for (int32_t y = 0; y < height; ++y) {
    const size_t row_offset = static_cast<size_t>(y) * (stride + 1u);
    const int filter = raw[row_offset];
    const uint8_t* source = raw.data() + row_offset + 1u;
    std::memcpy(current.data(), source, stride);
    switch (filter) {
      case 0:
        break;
      case 1:
        for (size_t i = sample_bytes; i < stride; ++i) {
          current[i] = static_cast<uint8_t>(current[i] + current[i - sample_bytes]);
        }
        break;
      case 2:
        for (size_t i = 0; i < stride; ++i) {
          current[i] = static_cast<uint8_t>(current[i] + previous[i]);
        }
        break;
      case 3:
        for (size_t i = 0; i < stride; ++i) {
          const int left = i >= sample_bytes ? current[i - sample_bytes] : 0;
          current[i] = static_cast<uint8_t>(current[i] + ((left + previous[i]) / 2));
        }
        break;
      case 4:
        for (size_t i = 0; i < stride; ++i) {
          const int left = i >= sample_bytes ? current[i - sample_bytes] : 0;
          const int up = previous[i];
          const int up_left = i >= sample_bytes ? previous[i - sample_bytes] : 0;
          current[i] = static_cast<uint8_t>(current[i] + paeth_predictor(left, up, up_left));
        }
        break;
      default:
        return fail("png decode: unknown filter type");
    }

    for (int32_t x = 0; x < width; ++x) {
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;
      uint8_t a = 255;
      switch (color_type) {
        case 0: {
          const uint8_t gray = sample8(current.data(), x, channels, bit_depth, 0);
          r = g = b = gray;
          break;
        }
        case 2: {
          r = sample8(current.data(), x, channels, bit_depth, 0);
          g = sample8(current.data(), x, channels, bit_depth, 1);
          b = sample8(current.data(), x, channels, bit_depth, 2);
          break;
        }
        case 3: {
          const uint8_t index = sample8(current.data(), x, channels, bit_depth, 0);
          const size_t base = static_cast<size_t>(index) * 3u;
          if (base + 2u >= palette.size()) return fail("png decode: palette index out of range");
          r = palette[base];
          g = palette[base + 1];
          b = palette[base + 2];
          a = index < transparency.size() ? transparency[index] : 255;
          break;
        }
        case 4: {
          const uint8_t gray = sample8(current.data(), x, channels, bit_depth, 0);
          r = g = b = gray;
          a = sample8(current.data(), x, channels, bit_depth, 1);
          break;
        }
        case 6: {
          r = sample8(current.data(), x, channels, bit_depth, 0);
          g = sample8(current.data(), x, channels, bit_depth, 1);
          b = sample8(current.data(), x, channels, bit_depth, 2);
          a = sample8(current.data(), x, channels, bit_depth, 3);
          break;
        }
        default:
          return fail("png decode: unsupported color type");
      }
      const size_t target = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                             static_cast<size_t>(x)) * 4u;
      out->rgba[target + 0] = premultiply(r, a);
      out->rgba[target + 1] = premultiply(g, a);
      out->rgba[target + 2] = premultiply(b, a);
      out->rgba[target + 3] = a;
    }

    std::swap(previous, current);
  }
  return true;
}

}  // namespace compositor
