// AI生成
/*
 * psd_codec.cpp — 自研 PSD 分层读写器（M1）。能力边界与拒绝清单见 psd_codec.h。
 *
 * 这里把「读的时候最容易写成静默出错」的几处先说清楚，免得下一个人踩：
 *
 *   1. 组的**混合模式不在记录头的 4 字符键里**，而在 `lsct` 块里
 *      （`00 00 00 01` + `8BIM` + `pass`）。记录头永远是 `norm`。只读记录头
 *      会把所有穿透组当成普通组——画面上看不出差别，直到组里有混合模式不同
 *      的子层，合成结果整块变色。
 *   2. 一个组在文件里是**两条记录**：底下一条 `lsct=3` 的边界分隔（矩形为空），
 *      上面一条 `lsct=1|2` 的文件夹记录（名字 / 不透明度 / 可见性 / 混合都在它
 *      上面）。只认文件夹记录就找不到组的下边界，子层会全部跑到根上。
 *   3. 16 位文档的**主 layer-info 段长度是 0**，记录与通道数据在 `Lr16`
 *      附加块里，而且那块**不带外层长度字段**（记录从 `i16 count` 直接开始）。
 *      读侧两种形态都认（先按 u32 试长度，对不上就退回无长度形态）。
 *   4. 通道数据的长度**包含**它自己的 2 字节压缩标记；RLE 的每行字节数表在
 *      打包数据**之前**，且行数按该通道自己的矩形高算（蒙版通道的矩形可以与
 *      图层矩形不同）。
 *   5. PSD 的 alpha 是**直通**的，引擎是**预乘**的。解码时预乘、编码时反预乘；
 *      少做这一步，半透明边缘会亮一圈。
 *   6. 图层额外的 tag 块之间**不保证偶数对齐**（psd-tools 写的时候 padding=1，
 *      Adobe 规范写的是补到偶数）。读侧按 `12 + len` 前进，若 len 为奇数且
 *      下一个 4 字节不是 `8BIM`/`8B64` 再跳 1 字节——两种都吃得下。
 */
#include "format/psd_codec.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/comp_geometry.h"

namespace compositor {
namespace {

using std::size_t;

/* ---------------------------------------------------------------- 常量 */

constexpr uint16_t kPsdVersionPsb = 2;
constexpr uint16_t kColorModeRgb = 3;

/* 一个文件的图层记录数上限：超过就不是我们能合理处理的文档了。 */
constexpr int32_t kMaxRecordCount = 30000;

/* 通道 id 的合法区间（0..53 是彩色/专色通道，-1..-3 是 alpha 与蒙版）。 */
constexpr int16_t kMinChannelId = -3;
constexpr int16_t kMaxChannelId = 63;

/* 读缓冲的上界：单个通道声明长度超过它只可能是坏文件。 */
constexpr size_t kMaxChannelBytes = size_t(1) << 31; /* 2 GiB */

/* 一节 layer-info 的合理性下限：至少 2 字节（count）。 */
constexpr size_t kMinLayerInfoBytes = 2;

/* ---------------------------------------------------------------- 基本类型 */

uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

int16_t be16s(const uint8_t* p) { return static_cast<int16_t>(be16(p)); }

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int32_t be32s(const uint8_t* p) { return static_cast<int32_t>(be32(p)); }

void put16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

void put32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value >> 24));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

void put32_at(std::vector<uint8_t>* out, size_t at, uint32_t value) {
  (*out)[at + 0] = static_cast<uint8_t>(value >> 24);
  (*out)[at + 1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  (*out)[at + 2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  (*out)[at + 3] = static_cast<uint8_t>(value & 0xFFu);
}

void put_bytes(std::vector<uint8_t>* out, const void* data, size_t count) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  out->insert(out->end(), p, p + count);
}

void pad_to(std::vector<uint8_t>* out, size_t alignment) {
  while (alignment > 1 && (out->size() % alignment) != 0) out->push_back(0);
}

/* ---------------------------------------------------------------- 字符串 */

/* UTF-8 -> UTF-16BE（追加到 out）。非法序列写成 U+FFFD，绝不产生半个代理项。 */
void utf8_to_utf16be(const std::string& text, std::vector<uint8_t>* out) {
  size_t i = 0;
  while (i < text.size()) {
    const uint8_t lead = static_cast<uint8_t>(text[i]);
    uint32_t code = 0;
    size_t extra = 0;
    if (lead < 0x80u) {
      code = lead;
    } else if ((lead & 0xE0u) == 0xC0u) {
      code = lead & 0x1Fu;
      extra = 1;
    } else if ((lead & 0xF0u) == 0xE0u) {
      code = lead & 0x0Fu;
      extra = 2;
    } else if ((lead & 0xF8u) == 0xF0u) {
      code = lead & 0x07u;
      extra = 3;
    } else {
      code = 0xFFFDu;
    }
    ++i;
    bool ok = i + extra <= text.size();
    for (size_t k = 0; ok && k < extra; ++k) {
      const uint8_t cont = static_cast<uint8_t>(text[i + k]);
      if ((cont & 0xC0u) != 0x80u) {
        ok = false;
        break;
      }
      code = (code << 6) | (cont & 0x3Fu);
    }
    if (ok) {
      i += extra;
    } else {
      code = 0xFFFDu;
    }
    if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) code = 0xFFFDu;
    if (code >= 0x10000u) {
      const uint32_t rest = code - 0x10000u;
      put16(out, static_cast<uint16_t>(0xD800u + (rest >> 10)));
      put16(out, static_cast<uint16_t>(0xDC00u + (rest & 0x3FFu)));
    } else {
      put16(out, static_cast<uint16_t>(code));
    }
  }
}

void append_utf8(std::string* out, uint32_t code) {
  if (code < 0x80u) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800u) {
    out->push_back(static_cast<char>(0xC0u | (code >> 6)));
    out->push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  } else if (code < 0x10000u) {
    out->push_back(static_cast<char>(0xE0u | (code >> 12)));
    out->push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  } else {
    out->push_back(static_cast<char>(0xF0u | (code >> 18)));
    out->push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  }
}

std::string utf16be_to_utf8(const uint8_t* data, size_t units) {
  std::string out;
  size_t i = 0;
  while (i < units) {
    uint32_t code = be16(data + i * 2);
    ++i;
    if (code >= 0xD800u && code <= 0xDBFFu) {
      if (i < units) {
        const uint32_t low = be16(data + i * 2);
        if (low >= 0xDC00u && low <= 0xDFFFu) {
          code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
          ++i;
        } else {
          code = 0xFFFDu;
        }
      } else {
        code = 0xFFFDu;
      }
    } else if (code >= 0xDC00u && code <= 0xDFFFu) {
      code = 0xFFFDu;
    }
    append_utf8(&out, code);
  }
  return out;
}

/* ---------------------------------------------------------------- PackBits */

/* 解一行 RLE。必须正好解出 dst_len 字节；多解/少解都是坏文件。 */
bool unpack_row(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len,
                std::string* error) {
  size_t sp = 0;
  size_t dp = 0;
  while (dp < dst_len) {
    if (sp >= src_len) {
      if (error != nullptr) *error = "PSD: RLE 行数据比声明的短";
      return false;
    }
    const int8_t control = static_cast<int8_t>(src[sp]);
    ++sp;
    if (control >= 0) {
      const size_t count = static_cast<size_t>(control) + 1;
      if (sp + count > src_len || dp + count > dst_len) {
        if (error != nullptr) *error = "PSD: RLE 直串越界";
        return false;
      }
      std::memcpy(dst + dp, src + sp, count);
      sp += count;
      dp += count;
    } else if (control != -128) {
      const size_t count = static_cast<size_t>(1 - static_cast<int>(control));
      if (sp >= src_len || dp + count > dst_len) {
        if (error != nullptr) *error = "PSD: RLE 重复串越界";
        return false;
      }
      std::memset(dst + dp, src[sp], count);
      ++sp;
      dp += count;
    }
    /* -128：规范里的空操作，跳过。 */
  }
  return true;
}

/* 打包一行（PackBits）。重复 >=3 走重复串，其余走直串。 */
void pack_row(const uint8_t* src, size_t count, std::vector<uint8_t>* out) {
  size_t i = 0;
  while (i < count) {
    size_t run = 1;
    while (i + run < count && run < 128 && src[i + run] == src[i]) ++run;
    if (run >= 3) {
      out->push_back(static_cast<uint8_t>(257 - run));
      out->push_back(src[i]);
      i += run;
      continue;
    }
    const size_t start = i;
    size_t literal = 0;
    while (i < count && literal < 128) {
      if (i + 2 < count && src[i] == src[i + 1] && src[i] == src[i + 2]) break;
      ++i;
      ++literal;
    }
    out->push_back(static_cast<uint8_t>(literal - 1));
    out->insert(out->end(), src + start, src + start + literal);
  }
}

/* ---------------------------------------------------------------- 混合模式 */

struct BlendEntry {
  char key[5];
  comp_blend_mode mode;
};

constexpr BlendEntry kBlendTable[] = {
    {"norm", COMP_BLEND_NORMAL},       {"dark", COMP_BLEND_DARKEN},
    {"mul ", COMP_BLEND_MULTIPLY},     {"idiv", COMP_BLEND_COLOR_BURN},
    {"lbrn", COMP_BLEND_LINEAR_BURN},  {"lite", COMP_BLEND_LIGHTEN},
    {"scrn", COMP_BLEND_SCREEN},       {"div ", COMP_BLEND_COLOR_DODGE},
    {"lddg", COMP_BLEND_LINEAR_DODGE}, {"over", COMP_BLEND_OVERLAY},
    {"sLit", COMP_BLEND_SOFT_LIGHT},   {"hLit", COMP_BLEND_HARD_LIGHT},
    {"vLit", COMP_BLEND_VIVID_LIGHT},  {"lLit", COMP_BLEND_LINEAR_LIGHT},
    {"pLit", COMP_BLEND_PIN_LIGHT},    {"hMix", COMP_BLEND_HARD_MIX},
    {"diff", COMP_BLEND_DIFFERENCE},   {"smud", COMP_BLEND_EXCLUSION},
    {"fsub", COMP_BLEND_SUBTRACT},     {"fdiv", COMP_BLEND_DIVIDE},
    {"hue ", COMP_BLEND_HUE},          {"sat ", COMP_BLEND_SATURATION},
    {"colr", COMP_BLEND_COLOR},        {"lum ", COMP_BLEND_LUMINOSITY},
};

bool blend_key_is(const uint8_t* key, const char* text) {
  return std::memcmp(key, text, 4) == 0;
}

const char* blend_key_of(comp_blend_mode mode) {
  for (const BlendEntry& entry : kBlendTable) {
    if (entry.mode == mode) return entry.key;
  }
  return "norm";
}

/* 读：4 字符键 -> 引擎枚举。pass 单独返回（只有组能用）。 */
bool blend_from_key(const uint8_t* key, comp_blend_mode* mode, bool* pass, std::string* reason) {
  if (pass != nullptr) *pass = false;
  if (blend_key_is(key, "pass")) {
    if (pass != nullptr) *pass = true;
    if (mode != nullptr) *mode = COMP_BLEND_NORMAL;
    return true;
  }
  for (const BlendEntry& entry : kBlendTable) {
    if (std::memcmp(key, entry.key, 4) == 0) {
      if (mode != nullptr) *mode = entry.mode;
      return true;
    }
  }
  if (reason != nullptr) {
    reason->assign("PSD: 不支持的混合模式 '");
    reason->append(reinterpret_cast<const char*>(key), 4);
    reason->push_back('\'');
    if (blend_key_is(key, "diss")) reason->append("（溶解依赖随机抖动，无法逐像素复现）");
    else if (blend_key_is(key, "dkCl")) reason->append("（深色颜色模式未实现）");
    else if (blend_key_is(key, "lgCl")) reason->append("（浅色颜色模式未实现）");
  }
  return false;
}

/* ---------------------------------------------------------------- 拒绝清单 */

/* 这些 tag 出现就意味着该图层承载了我们不支持的内容。**必须拒绝**：
 * 静默忽略等于在用户不知情的情况下把文字层/智能对象变成一张普通像素层。 */
struct RejectTag {
  char key[5];
  const char* reason;
};

constexpr RejectTag kRejectTags[] = {
    {"TySh", "文字层"},
    {"tySh", "文字层"},
    {"SoLd", "智能对象"},
    {"SoLE", "智能对象"},
    {"PlLd", "置入的智能对象"},
    {"vmsk", "矢量蒙版"},
    {"vsms", "矢量蒙版"},
    {"lmfx", "图层特效"},
    {"lrFX", "图层特效"},
    {"lfx2", "图层特效"},
    {"brit", "亮度/对比度调整图层"},
    {"levl", "色阶调整图层"},
    {"curv", "曲线调整图层"},
    {"expA", "曝光调整图层"},
    {"vibA", "自然饱和度调整图层"},
    {"hue2", "色相/饱和度调整图层"},
    {"hue ", "色相/饱和度调整图层"},
    {"blnc", "色彩平衡调整图层"},
    {"blwh", "黑白调整图层"},
    {"phfl", "照片滤镜调整图层"},
    {"mixr", "通道混合器调整图层"},
    {"clrL", "颜色查找调整图层"},
    {"nvrt", "反相调整图层"},
    {"post", "色调分离调整图层"},
    {"thrs", "阈值调整图层"},
    {"grdm", "渐变映射调整图层"},
    {"selc", "可选颜色调整图层"},
    {"CgEd", "调整图层的内容附加数据"},
    {"SoCo", "纯色填充图层"},
    {"GdFl", "渐变填充图层"},
    {"PtFl", "图案填充图层"},
};

const char* rejected_tag_reason(const uint8_t* key) {
  for (const RejectTag& entry : kRejectTags) {
    if (std::memcmp(key, entry.key, 4) == 0) return entry.reason;
  }
  return nullptr;
}

/* ---------------------------------------------------------------- 读游标 */

struct Cursor {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  std::string* error = nullptr;
  PsdStatus* status = nullptr;

  bool fail(PsdStatus code, const std::string& text) {
    if (status != nullptr) *status = code;
    if (error != nullptr && error->empty()) *error = text;
    return false;
  }

  bool io(const std::string& text) { return fail(PsdStatus::kIo, text); }
  bool unsupported(const std::string& text) { return fail(PsdStatus::kUnsupported, text); }
  bool bad(const std::string& text) { return fail(PsdStatus::kDecode, text); }

  bool remaining(size_t count) const { return pos <= size && count <= size - pos; }

  /* 剩余字节数。 */
  size_t left() const { return pos <= size ? size - pos : 0; }

  bool take(size_t count, const uint8_t** out) {
    if (!remaining(count)) return io("PSD: 文件被截断");
    if (out != nullptr) *out = data + pos;
    pos += count;
    return true;
  }

  bool skip(size_t count) { return take(count, nullptr); }

  bool u8(uint8_t* out) {
    const uint8_t* p = nullptr;
    if (!take(1, &p)) return false;
    *out = p[0];
    return true;
  }

  bool u16(uint16_t* out) {
    const uint8_t* p = nullptr;
    if (!take(2, &p)) return false;
    *out = be16(p);
    return true;
  }

  bool i16(int16_t* out) {
    const uint8_t* p = nullptr;
    if (!take(2, &p)) return false;
    *out = be16s(p);
    return true;
  }

  bool u32(uint32_t* out) {
    const uint8_t* p = nullptr;
    if (!take(4, &p)) return false;
    *out = be32(p);
    return true;
  }

  bool i32(int32_t* out) {
    const uint8_t* p = nullptr;
    if (!take(4, &p)) return false;
    *out = be32s(p);
    return true;
  }

  /* 4 个 int32（PSD 的 top/left/bottom/right）。 */
  bool rect(int32_t out[4]) {
    for (int i = 0; i < 4; ++i) {
      if (!i32(&out[i])) return false;
    }
    return true;
  }
};

/* ---------------------------------------------------------------- 解码中间态 */

struct MaskInfo {
  bool present = false;
  int32_t top = 0;
  int32_t left = 0;
  int32_t bottom = 0;
  int32_t right = 0;
  uint8_t default_color = 0;
  bool invert = false;

  int32_t width() const { return std::max(0, right - left); }
  int32_t height() const { return std::max(0, bottom - top); }
};

struct ParsedChannel {
  int16_t id = 0;
  uint32_t length = 0;
};

struct ParsedRecord {
  std::string name;
  int32_t top = 0;
  int32_t left = 0;
  int32_t bottom = 0;
  int32_t right = 0;
  std::vector<ParsedChannel> channels;

  comp_blend_mode blend = COMP_BLEND_NORMAL;
  bool blend_pass = false;
  uint8_t opacity = 255;
  uint8_t clipping = 0;
  uint8_t flags = 0;

  MaskInfo mask;
  bool has_mask_channel = false;

  /* lsct / lsdk：0 = 普通层，1/2 = 文件夹，3 = 边界分隔。 */
  int32_t section_kind = 0;
  comp_blend_mode section_blend = COMP_BLEND_NORMAL;
  bool section_pass = false;

  int32_t width() const { return std::max(0, right - left); }
  int32_t height() const { return std::max(0, bottom - top); }
  bool visible() const { return (flags & 0x02u) == 0; }
};

/* 一个通道解出来的平面：8 位用 b8，16 位用 b16。 */
struct Plane {
  int32_t width = 0;
  int32_t height = 0;
  bool wide = false;
  std::vector<uint8_t> b8;
  std::vector<uint16_t> b16;

  bool empty() const { return width <= 0 || height <= 0; }

  uint32_t at(int32_t x, int32_t y) const {
    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) +
                         static_cast<size_t>(x);
    return wide ? static_cast<uint32_t>(b16[index]) : static_cast<uint32_t>(b8[index]);
  }
};

/* ---------------------------------------------------------------- 通道解码 */

/* 解一个通道块（含 2 字节压缩标记）。w/h 是该通道自己的矩形尺寸。 */
bool decode_plane(const uint8_t* block, size_t block_len, int32_t width, int32_t height,
                  bool wide, Plane* out, Cursor* cursor) {
  out->width = std::max(0, width);
  out->height = std::max(0, height);
  out->wide = wide;
  out->b8.clear();
  out->b16.clear();
  if (out->width <= 0 || out->height <= 0) return true;

  if (block_len < 2) return cursor->bad("PSD: 通道数据缺少压缩标记");
  const uint16_t compression = be16(block);
  const size_t sample_bytes = wide ? 2u : 1u;
  const size_t row_bytes = static_cast<size_t>(out->width) * sample_bytes;
  const size_t plane_bytes = row_bytes * static_cast<size_t>(out->height);

  if (wide) {
    out->b16.assign(static_cast<size_t>(out->width) * static_cast<size_t>(out->height), 0u);
  } else {
    out->b8.assign(static_cast<size_t>(out->width) * static_cast<size_t>(out->height), 0u);
  }
  uint8_t* base = wide ? reinterpret_cast<uint8_t*>(out->b16.data()) : out->b8.data();

  if (compression == kPsdCompressionRaw) {
    if (block_len - 2 < plane_bytes) return cursor->bad("PSD: 未压缩通道数据不足");
    if (wide) {
      /* 文件是 big-endian，引擎要 native-endian。 */
      for (size_t i = 0; i < static_cast<size_t>(out->width) * static_cast<size_t>(out->height);
           ++i) {
        out->b16[i] = be16(block + 2 + i * 2);
      }
    } else {
      std::memcpy(base, block + 2, plane_bytes);
    }
    return true;
  }

  if (compression != kPsdCompressionRle) {
    if (compression == 2 || compression == 3) {
      return cursor->unsupported("PSD: 图层通道使用 ZIP 压缩（本读写器只支持 raw 与 RLE）");
    }
    return cursor->unsupported("PSD: 未知的通道压缩方式 " + std::to_string(compression));
  }

  /* RLE：先是 height 个 u16 的行字节数，然后是各行打包数据。 */
  const size_t count_table = static_cast<size_t>(out->height) * 2u;
  if (block_len - 2 < count_table) return cursor->bad("PSD: RLE 行长度表被截断");
  const uint8_t* counts = block + 2;
  const uint8_t* packed = counts + count_table;
  size_t packed_left = block_len - 2 - count_table;

  std::vector<uint8_t> row_storage;
  for (int32_t y = 0; y < out->height; ++y) {
    const size_t row_len = be16(counts + static_cast<size_t>(y) * 2u);
    if (row_len > packed_left) return cursor->bad("PSD: RLE 行数据越界");
    row_storage.resize(row_bytes);
    if (!unpack_row(packed, row_len, row_storage.data(), row_bytes, cursor->error)) {
      return cursor->fail(PsdStatus::kDecode, cursor->error != nullptr ? *cursor->error
                                                                      : "PSD: RLE 解码失败");
    }
    if (wide) {
      for (int32_t x = 0; x < out->width; ++x) {
        out->b16[static_cast<size_t>(y) * static_cast<size_t>(out->width) +
                 static_cast<size_t>(x)] = be16(row_storage.data() + static_cast<size_t>(x) * 2u);
      }
    } else {
      std::memcpy(base + static_cast<size_t>(y) * row_bytes, row_storage.data(), row_bytes);
    }
    packed += row_len;
    packed_left -= row_len;
  }
  return true;
}

/* ---------------------------------------------------------------- 记录解析 */

/* 图层额外的 mask 块。返回时 cursor 已越过该块。
 * 真实蒙版（-3 通道）在通道表里就已经拒绝，这里不必再判一次。 */
bool parse_mask_block(Cursor* cursor, int32_t layer_left, int32_t layer_top,
                      MaskInfo* mask) {
  uint32_t length = 0;
  if (!cursor->u32(&length)) return false;
  if (length == 0) {
    mask->present = false;
    return true;
  }
  if (length > cursor->left()) return cursor->io("PSD: 蒙版块长度越界");
  const size_t body_start = cursor->pos;
  const uint8_t* rect = nullptr;
  if (!cursor->take(16, &rect)) return false;
  int32_t values[4];
  for (int i = 0; i < 4; ++i) values[i] = be32s(rect + i * 4);
  uint8_t default_color = 0;
  uint8_t flags = 0;
  if (!cursor->u8(&default_color)) return false;
  if (!cursor->u8(&flags)) return false;

  if ((flags & 0x10u) != 0) {
    /* MaskParameters：羽化 / 密度参数。引擎的蒙版模型没有这两项，
     * 悄悄忽略会让蒙版边缘与用户在 Photoshop 里看到的完全不同。 */
    return cursor->unsupported("PSD: 蒙版带羽化/密度参数（MaskParameters），本读写器未实现");
  }
  if (length >= 36) {
    /* 带真实蒙版的块会长出这一截；到这里说明含我们没识别的前缀数据。 */
    return cursor->unsupported("PSD: 蒙版块含未识别的前缀数据");
  }

  mask->present = true;
  mask->top = values[0];
  mask->left = values[1];
  mask->bottom = values[2];
  mask->right = values[3];
  if ((flags & 0x01u) != 0) {
    /* 蒙版矩形相对图层矩形。 */
    mask->left += layer_left;
    mask->right += layer_left;
    mask->top += layer_top;
    mask->bottom += layer_top;
  }
  mask->default_color = default_color;
  mask->invert = (flags & 0x04u) != 0;
  if (mask->bottom < mask->top || mask->right < mask->left) {
    return cursor->io("PSD: 蒙版矩形为负");
  }

  /* 跳到块尾（块内可能有我们不关心的尾巴）。 */
  cursor->pos = body_start + length;
  return true;
}

/* 一个图层记录（不含通道数据）。 */
bool parse_record(Cursor* cursor, ParsedRecord* record) {
  int32_t geometry[4];
  if (!cursor->rect(geometry)) return false;
  record->top = geometry[0];
  record->left = geometry[1];
  record->bottom = geometry[2];
  record->right = geometry[3];
  if (record->bottom < record->top || record->right < record->left) {
    return cursor->io("PSD: 图层矩形为负");
  }

  uint16_t channel_count = 0;
  if (!cursor->u16(&channel_count)) return false;
  if (channel_count > 64) return cursor->io("PSD: 图层通道数异常");

  record->channels.clear();
  record->channels.reserve(channel_count);
  for (uint16_t i = 0; i < channel_count; ++i) {
    ParsedChannel channel;
    if (!cursor->i16(&channel.id)) return false;
    if (!cursor->u32(&channel.length)) return false;
    if (channel.id < kMinChannelId || channel.id > kMaxChannelId) {
      return cursor->unsupported("PSD: 未知通道 id " + std::to_string(channel.id));
    }
    if (channel.id == -3) {
      return cursor->unsupported("PSD: 图层带真实蒙版通道（-3，矢量+像素合成蒙版）");
    }
    if (channel.id >= 3) {
      return cursor->unsupported("PSD: 图层带额外通道（专色/第 4 个以上彩色通道）");
    }
    if (channel.length > kMaxChannelBytes) return cursor->io("PSD: 通道长度异常");
    if (channel.id == -2) record->has_mask_channel = true;
    record->channels.push_back(channel);
  }

  /* 混合模式：`8BIM` + 4 字符键。签名必须认，否则后面整条记录都错位。 */
  const uint8_t* blend_block = nullptr;
  if (!cursor->take(8, &blend_block)) return false;
  if (std::memcmp(blend_block, "8BIM", 4) != 0 && std::memcmp(blend_block, "8B64", 4) != 0) {
    return cursor->io("PSD: 图层记录混合模式签名不是 8BIM");
  }
  std::string blend_error;
  if (!blend_from_key(blend_block + 4, &record->blend, &record->blend_pass, &blend_error)) {
    return cursor->unsupported(blend_error);
  }
  if (!cursor->u8(&record->opacity)) return false;
  if (!cursor->u8(&record->clipping)) return false;
  if (record->clipping > 1) return cursor->io("PSD: 剪贴标志取值异常");
  if (!cursor->u8(&record->flags)) return false;
  if ((record->flags & 0x10u) != 0) {
    /* pixel data irrelevant：填充 / 调整图层的标记，其像素是缓存而非内容。 */
    return cursor->unsupported("PSD: 图层标记为「像素无关」（填充/调整图层）");
  }
  uint8_t filler = 0;
  if (!cursor->u8(&filler)) return false;

  uint32_t extra_len = 0;
  if (!cursor->u32(&extra_len)) return false;
  if (extra_len > cursor->left()) return cursor->io("PSD: 图层扩展数据长度越界");
  const size_t extra_end = cursor->pos + extra_len;

  if (!parse_mask_block(cursor, record->left, record->top, &record->mask)) {
    return false;
  }
  /* 蒙版块与 -2 通道必须同进同出：只有其中一个就说明文件的蒙版结构我们没读懂，
   * 悄悄按「没有蒙版」处理等于改了用户的图。 */
  if (record->mask.present != record->has_mask_channel) {
    return cursor->io("PSD: 蒙版块与 -2 蒙版通道不一致");
  }

  /* 混合范围：一律不读（引擎的混合模型没有它，读了也无处安放）。 */
  uint32_t ranges_len = 0;
  if (!cursor->u32(&ranges_len)) return false;
  if (!cursor->skip(ranges_len)) return false;

  /* Pascal 名字：长度字节 + 内容，共 (1 + len) 字节补到 4 的整数倍。
   * 对齐是相对**这个字符串自身**算的，不是相对文件偏移。 */
  uint8_t name_len = 0;
  if (!cursor->u8(&name_len)) return false;
  const uint8_t* name_bytes = nullptr;
  if (!cursor->take(name_len, &name_bytes)) return false;
  record->name.assign(reinterpret_cast<const char*>(name_bytes), name_len);
  {
    const size_t written = static_cast<size_t>(name_len) + 1u;
    const size_t pad = (4u - (written % 4u)) % 4u;
    if (cursor->pos + pad > extra_end) return cursor->io("PSD: 图层名字段越界");
    if (!cursor->skip(pad)) return false;
  }

  /* 带 tag 的扩展块。 */
  while (cursor->pos + 12 <= extra_end) {
    const uint8_t* tag = nullptr;
    if (!cursor->take(12, &tag)) return false;
    const bool known_sig = std::memcmp(tag, "8BIM", 4) == 0 || std::memcmp(tag, "8B64", 4) == 0;
    if (!known_sig) {
      /* 不是 tag：只剩对齐填补，收工。 */
      cursor->pos = extra_end;
      break;
    }
    const uint8_t* key = tag + 4;
    const uint32_t tag_len = be32(tag + 8);
    if (tag_len > extra_end - cursor->pos) return cursor->io("PSD: tag 块长度越界");
    const uint8_t* tag_body = nullptr;
    if (!cursor->take(tag_len, &tag_body)) return false;

    if (const char* reason = rejected_tag_reason(key)) {
      return cursor->unsupported(std::string("PSD: 图层含不支持的内容——") + reason);
    }

    if (std::memcmp(key, "luni", 4) == 0 && tag_len >= 4) {
      const uint32_t units = be32(tag_body);
      if (4u + static_cast<size_t>(units) * 2u <= tag_len) {
        record->name = utf16be_to_utf8(tag_body + 4, units);
      }
    } else if ((std::memcmp(key, "lsct", 4) == 0 || std::memcmp(key, "lsdk", 4) == 0) &&
               tag_len >= 4) {
      const uint32_t kind = be32(tag_body);
      if (kind == 1 || kind == 2 || kind == 3) {
        record->section_kind = static_cast<int32_t>(kind);
        /* 组混合模式就在这个块里（`8BIM` + 4 字符键）；`pass` = 穿透。 */
        if (tag_len >= 12 && std::memcmp(tag_body + 4, "8BIM", 4) == 0) {
          std::string section_error;
          if (!blend_from_key(tag_body + 8, &record->section_blend, &record->section_pass,
                              &section_error)) {
            return cursor->unsupported(section_error);
          }
        }
      }
    }

    /* tag 之间对齐：规范补到偶数，psd-tools 不补。两种都吃。 */
    if ((tag_len & 1u) != 0u) {
      if (cursor->pos + 4 <= extra_end && cursor->pos < extra_end) {
        const bool next_is_tag =
            std::memcmp(cursor->data + cursor->pos, "8BIM", 4) == 0 ||
            std::memcmp(cursor->data + cursor->pos, "8B64", 4) == 0;
        if (!next_is_tag && cursor->pos < extra_end) {
          if (!cursor->skip(1)) return false;
        }
      }
    }
  }

  if (cursor->pos > extra_end) return cursor->io("PSD: 图层扩展数据越界读取");
  cursor->pos = extra_end;
  return true;
}

/* ---------------------------------------------------------------- 通道合并 */

/* 把一个图层的各通道合成为引擎像素缓冲（**预乘**）。 */
bool compose_pixels(const ParsedRecord& record, bool wide, const std::vector<Plane>& planes,
                    ImageBuffer* out, Cursor* cursor) {
  const int32_t width = record.width();
  const int32_t height = record.height();
  const comp_pixel_format format = wide ? COMP_PIXEL_RGBA16 : COMP_PIXEL_RGBA8;
  if (width <= 0 || height <= 0) {
    out->resize(0, 0, format);
    return true;
  }
  out->resize(width, height, format);

  const Plane* red = nullptr;
  const Plane* green = nullptr;
  const Plane* blue = nullptr;
  const Plane* alpha = nullptr;
  for (size_t i = 0; i < record.channels.size(); ++i) {
    switch (record.channels[i].id) {
      case 0: red = &planes[i]; break;
      case 1: green = &planes[i]; break;
      case 2: blue = &planes[i]; break;
      case -1: alpha = &planes[i]; break;
      default: break;
    }
  }

  const uint64_t max_value = wide ? 65535u : 255u;
  const uint64_t half = max_value / 2u;

  if (wide) {
    uint16_t* dst = reinterpret_cast<uint16_t*>(out->writable_pixels().data());
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const uint32_t a = alpha != nullptr ? alpha->at(x, y) : 65535u;
        uint32_t r = red != nullptr ? red->at(x, y) : 0u;
        uint32_t g = green != nullptr ? green->at(x, y) : 0u;
        uint32_t b = blue != nullptr ? blue->at(x, y) : 0u;
        if (a != 65535u) {
          r = static_cast<uint32_t>((static_cast<uint64_t>(r) * a + half) / max_value);
          g = static_cast<uint32_t>((static_cast<uint64_t>(g) * a + half) / max_value);
          b = static_cast<uint32_t>((static_cast<uint64_t>(b) * a + half) / max_value);
        }
        const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                              static_cast<size_t>(x)) * 4u;
        dst[index + 0] = static_cast<uint16_t>(r);
        dst[index + 1] = static_cast<uint16_t>(g);
        dst[index + 2] = static_cast<uint16_t>(b);
        dst[index + 3] = static_cast<uint16_t>(a);
      }
    }
  } else {
    uint8_t* dst = out->writable_pixels().data();
    for (int32_t y = 0; y < height; ++y) {
      uint8_t* row = dst + static_cast<size_t>(y) * out->stride();
      for (int32_t x = 0; x < width; ++x) {
        const uint32_t a = alpha != nullptr ? alpha->at(x, y) : 255u;
        uint32_t r = red != nullptr ? red->at(x, y) : 0u;
        uint32_t g = green != nullptr ? green->at(x, y) : 0u;
        uint32_t b = blue != nullptr ? blue->at(x, y) : 0u;
        if (a != 255u) {
          r = (r * a + 127u) / 255u;
          g = (g * a + 127u) / 255u;
          b = (b * a + 127u) / 255u;
        }
        row[x * 4 + 0] = static_cast<uint8_t>(r);
        row[x * 4 + 1] = static_cast<uint8_t>(g);
        row[x * 4 + 2] = static_cast<uint8_t>(b);
        row[x * 4 + 3] = static_cast<uint8_t>(a);
      }
    }
  }
  (void)cursor;
  return true;
}

/* 把蒙版平面展开到**图层像素空间**（矩形外取默认色，按需取反）。 */
void expand_mask(const ParsedRecord& record, const Plane& plane, MaskBuffer* out) {
  const int32_t width = record.width();
  const int32_t height = record.height();
  if (width <= 0 || height <= 0) {
    out->width = 0;
    out->height = 0;
    out->pixels.reset();
    return;
  }
  out->resize(width, height, 0);
  const uint8_t outside = record.mask.default_color;
  /* 图层像素 (x,y) 对应蒙版平面 (x + dx, y + dy)。 */
  const int32_t dx = record.left - record.mask.left;
  const int32_t dy = record.top - record.mask.top;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = out->writable_row(y);
    const int32_t my = y + dy;
    for (int32_t x = 0; x < width; ++x) {
      const int32_t mx = x + dx;
      uint8_t value = outside;
      if (mx >= 0 && mx < plane.width && my >= 0 && my < plane.height) {
        value = static_cast<uint8_t>(plane.at(mx, my));
      }
      row[x] = record.mask.invert ? static_cast<uint8_t>(255u - value) : value;
    }
  }
}

/* ---------------------------------------------------------------- 顶层解析 */

/* layer-info 主体：i16 count + 记录 + 通道数据。 */
bool parse_layer_info(const uint8_t* body, size_t body_len, PsdDocument* doc, Cursor* outer) {
  Cursor cursor;
  cursor.data = body;
  cursor.size = body_len;
  cursor.pos = 0;
  cursor.error = outer->error;
  cursor.status = outer->status;
  if (body_len < kMinLayerInfoBytes) return true; /* 空段 */

  int16_t count = 0;
  if (!cursor.i16(&count)) return false;
  const int32_t record_count = count < 0 ? -static_cast<int32_t>(count) : count;
  if (record_count > kMaxRecordCount) return cursor.io("PSD: 图层记录数异常");

  std::vector<ParsedRecord> records(static_cast<size_t>(record_count));
  for (int32_t i = 0; i < record_count; ++i) {
    if (!parse_record(&cursor, &records[static_cast<size_t>(i)])) {
      /* 把内层状态交还给外层（Cursor 只借用 error/status 指针，这里显式同步）。 */
      return false;
    }
  }

  /* 建立图层表：边界分隔（lsct=3）开一个组，文件夹记录（lsct=1|2）收口。 */
  struct Scope {
    size_t layer_index = 0;
  };
  std::vector<Scope> stack;

  for (ParsedRecord& record : records) {
    const int32_t parent = stack.empty() ? -1 : static_cast<int32_t>(stack.back().layer_index);

    if (record.section_kind == 3) {
      PsdLayerData group;
      group.is_group = true;
      group.parent = parent;
      group.bounds = comp_rect{0, 0, 0, 0};
      group.visible = record.visible();
      group.opacity = record.opacity;
      doc->layers.push_back(std::move(group));
      stack.push_back(Scope{doc->layers.size() - 1u});
      continue;
    }

    if (record.section_kind == 1 || record.section_kind == 2) {
      if (stack.empty()) return cursor.io("PSD: 组结束标记没有对应的开始标记");
      const size_t index = stack.back().layer_index;
      stack.pop_back();
      PsdLayerData& group = doc->layers[index];
      group.name = record.name;
      group.blend = record.section_pass ? COMP_BLEND_NORMAL : record.section_blend;
      group.pass_through = record.section_pass;
      group.opacity = record.opacity;
      /* 组的可见性以文件夹记录为准（边界分隔记录在 Photoshop 里总是隐藏的）。 */
      group.visible = record.visible();
      continue;
    }

    PsdLayerData layer;
    layer.name = record.name;
    layer.blend = record.blend;
    /* pass 只对组有意义；普通层写成 pass 是坏文件。 */
    if (record.blend_pass) {
      return cursor.io("PSD: 普通图层使用了组专用的 pass 混合键");
    }
    layer.opacity = record.opacity;
    layer.visible = record.visible();
    layer.clipping = record.clipping == 1;
    layer.parent = parent;
    layer.bounds = comp_rect{record.left, record.top, record.width(), record.height()};
    if (!is_valid_canvas(comp_size{layer.bounds.w, layer.bounds.h}) &&
        (layer.bounds.w > 0 || layer.bounds.h > 0)) {
      if (layer.bounds.w > limits::kMaxSide || layer.bounds.h > limits::kMaxSide) {
        return cursor.io("PSD: 图层尺寸超过引擎上限");
      }
      if (static_cast<int64_t>(layer.bounds.w) * layer.bounds.h >
          limits::kMaxSurfacePixels) {
        return cursor.io("PSD: 图层像素数超过引擎上限");
      }
    }
    doc->layers.push_back(std::move(layer));
  }

  if (!stack.empty()) return cursor.io("PSD: 组开始标记没有对应的结束标记");

  /* 通道数据：按记录顺序、记录内按通道顺序依次排列。
   *
   * **每一条记录都要消费自己的通道块**，包括组的两条记录（它们的通道通常是
   * 0×0 矩形，长度 2，只够放压缩标记）——跳过它们会让后面所有通道块整体错位，
   * 而那不会报错，只会把像素读成垃圾。 */
  const bool wide = doc->depth == 16;
  size_t layer_index = 0;
  std::vector<Plane> planes;

  /* 建表时「边界分隔」产生一个组条目、「文件夹」不产生条目，这里必须按同样的
   * 规则推进 doc->layers 的下标。 */
  for (ParsedRecord& record : records) {
    /* 第一遍只定位：每个通道块从哪开始、多长（同时推进游标）。 */
    std::vector<std::pair<const uint8_t*, size_t>> blocks(record.channels.size());
    for (size_t i = 0; i < record.channels.size(); ++i) {
      const uint8_t* block = nullptr;
      if (!cursor.take(record.channels[i].length, &block)) return false;
      blocks[i] = std::make_pair(block, static_cast<size_t>(record.channels[i].length));
    }

    const bool produces_layer = record.section_kind != 1 && record.section_kind != 2;
    PsdLayerData* layer = nullptr;
    if (produces_layer) {
      if (layer_index >= doc->layers.size()) return cursor.io("PSD: 图层表与记录表不一致");
      layer = &doc->layers[layer_index];
      ++layer_index;
      if (record.section_kind == 3 && !layer->is_group) {
        return cursor.io("PSD: 图层表与记录表不一致");
      }
    }
    if (layer == nullptr || record.section_kind == 3) continue; /* 组的两条记录没有像素 */

    planes.clear();
    planes.resize(record.channels.size());

    const int32_t layer_h = record.height();
    const bool has_mask_plane = record.has_mask_channel && record.mask.present;
    Plane mask_plane;

    for (size_t i = 0; i < record.channels.size(); ++i) {
      const ParsedChannel& channel = record.channels[i];
      int32_t channel_w = record.width();
      int32_t channel_h = layer_h;
      bool wide_channel = wide;
      if (channel.id == -2) {
        if (!has_mask_plane) continue;
        channel_w = record.mask.width();
        channel_h = record.mask.height();
        wide_channel = false; /* PSD 的蒙版通道恒为 8 位 */
      }
      /* -2 的矩形可能和图层矩形不同（蒙版有自己的矩形）；其余通道恒等于图层矩形。 */
      Plane plane;
      if (!decode_plane(blocks[i].first, blocks[i].second, channel_w, channel_h, wide_channel,
                        &plane, &cursor)) {
        return false;
      }
      if (channel.id == -2) {
        mask_plane = std::move(plane);
      } else {
        planes[i] = std::move(plane);
      }
    }

    if (!compose_pixels(record, wide, planes, &layer->pixels, &cursor)) return false;

    if (has_mask_plane && !mask_plane.empty()) {
      expand_mask(record, mask_plane, &layer->mask);
    } else {
      layer->mask = MaskBuffer();
    }
  }

  return true;
}

/* 剪贴关系：剪贴层贴到同一父级下、它下方最近的非剪贴层。 */
void resolve_clipping(PsdDocument* doc) {
  for (size_t i = 0; i < doc->layers.size(); ++i) {
    PsdLayerData& layer = doc->layers[i];
    if (!layer.clipping) {
      layer.clipping_source = -1;
      continue;
    }
    int32_t source = -1;
    for (size_t j = i; j-- > 0;) {
      if (doc->layers[j].parent != layer.parent) continue;
      if (doc->layers[j].is_group) continue;
      if (doc->layers[j].clipping) continue;
      source = static_cast<int32_t>(j);
      break;
    }
    if (source < 0) {
      /* 没有可以贴的基底：按 Photoshop 的行为，该层就当成普通层。 */
      layer.clipping = false;
      layer.clipping_source = -1;
    } else {
      layer.clipping_source = source;
    }
  }
}

/* ---------------------------------------------------------------- 编码辅助 */

/* 一个图层记录待写出的两部分：记录头（含通道长度）与通道数据。 */
struct RecordBlob {
  std::vector<uint8_t> header;
  std::vector<uint8_t> channels;
};

/* 把一个通道的平面编码成 PSD 通道块（2 字节压缩标记 + RLE 数据）。 */
bool encode_channel_block(const uint8_t* samples, int32_t width, int32_t height,
                          size_t sample_bytes, std::vector<uint8_t>* out, std::string* error) {
  out->clear();
  put16(out, kPsdCompressionRle);
  if (width <= 0 || height <= 0) return true;

  const size_t row_bytes = static_cast<size_t>(width) * sample_bytes;
  std::vector<uint8_t> packed;
  std::vector<uint16_t> counts;
  counts.reserve(static_cast<size_t>(height));
  packed.reserve(row_bytes * static_cast<size_t>(height));

  for (int32_t y = 0; y < height; ++y) {
    const size_t before = packed.size();
    pack_row(samples + static_cast<size_t>(y) * row_bytes, row_bytes, &packed);
    const size_t row_len = packed.size() - before;
    if (row_len > 0xFFFFu) {
      if (error != nullptr) *error = "PSD: RLE 行长度超过 65535 字节";
      return false;
    }
    counts.push_back(static_cast<uint16_t>(row_len));
  }
  for (uint16_t value : counts) put16(out, value);
  out->insert(out->end(), packed.begin(), packed.end());
  return true;
}

/* 逐通道取样写进临时行缓冲（直通 alpha + big-endian 16 位）。 */
void extract_channel(const ImageBuffer& pixels, int32_t channel, int depth,
                     std::vector<uint8_t>* out) {
  const int32_t width = pixels.width;
  const int32_t height = pixels.height;
  const size_t sample_bytes = depth == 16 ? 2u : 1u;
  const size_t row_bytes = static_cast<size_t>(width) * sample_bytes;
  out->assign(row_bytes * static_cast<size_t>(height), 0);
  if (width <= 0 || height <= 0) return;
  const bool wide = pixels.format == COMP_PIXEL_RGBA16;

  for (int32_t y = 0; y < height; ++y) {
    uint8_t* dst = out->data() + static_cast<size_t>(y) * row_bytes;
    if (wide) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(
          pixels.pixel_data().data() + static_cast<size_t>(y) * pixels.stride());
      for (int32_t x = 0; x < width; ++x) {
        const uint32_t a = row[x * 4 + 3];
        uint32_t value = channel < 0 ? a : row[x * 4 + channel];
        if (channel >= 0 && a != 0u && a != 65535u) {
          value = static_cast<uint32_t>(
              (static_cast<uint64_t>(value) * 65535u + a / 2u) / a);
          if (value > 65535u) value = 65535u;
        }
        dst[x * 2 + 0] = static_cast<uint8_t>(value >> 8);
        dst[x * 2 + 1] = static_cast<uint8_t>(value & 0xFFu);
      }
    } else {
      const uint8_t* row = pixels.row(y);
      for (int32_t x = 0; x < width; ++x) {
        const uint32_t a = row[x * 4 + 3];
        uint32_t value = channel < 0 ? a : row[x * 4 + channel];
        if (channel >= 0 && a != 0u && a != 255u) {
          value = (value * 255u + a / 2u) / a;
          if (value > 255u) value = 255u;
        }
        dst[x] = static_cast<uint8_t>(value);
      }
    }
  }
}

/* 组记录 / 分隔记录共用的空矩形 + 空通道记录头。 */
void write_empty_record(RecordBlob* blob, const char* blend_key, uint8_t opacity, bool hidden,
                        const std::vector<uint8_t>& extra) {
  put32(&blob->header, 0);
  put32(&blob->header, 0);
  put32(&blob->header, 0);
  put32(&blob->header, 0);
  /*
   * 组/分隔记录也**必须**列出通道。此前这里写 channel count = 0，本仓库的解码器
   * 对此宽容，但第三方读取器不认：psd-tools 在合并图层时抛
   * `Channels mismatch: expected 3 != given 0`，整份结构随之被跳过——导出的 PSD
   * 在 Photoshop / psd-tools 里背景会凭空消失（合成全黑）。
   *
   * 写法与 Photoshop、psd-tools 一致：4 个通道（-1 alpha 与 R/G/B），每个都带一个
   * 「压缩方式 = 0、数据长度为 0」的 2 字节块。组本身没有像素，所以数据是空的，
   * 但长度字段必须存在——读取器按记录里声明的长度顺序跳过通道数据。
   */
  static const int16_t kEmptyChannelIds[4] = {-1, 0, 1, 2};
  put16(&blob->header, 4);
  for (int16_t id : kEmptyChannelIds) {
    put16(&blob->header, static_cast<uint16_t>(id));
    put32(&blob->header, 2u);
  }
  put_bytes(&blob->header, "8BIM", 4);
  put_bytes(&blob->header, blend_key, 4);
  blob->header.push_back(opacity);
  blob->header.push_back(0); /* clipping */
  blob->header.push_back(hidden ? 0x0Au : 0x08u);
  blob->header.push_back(0);
  put32(&blob->header, static_cast<uint32_t>(extra.size()));
  put_bytes(&blob->header, extra.data(), extra.size());
  /* 每个空通道 = 1 字节压缩方式(0=raw) + 0 字节数据。 */
  blob->channels.assign(8, 0u);
}

/* 图层扩展数据：蒙版块 + 混合范围 + Pascal 名字 + tag 块。
 *
 * emit_lsct：这个记录是否是组的组成部分（边界分隔 or 文件夹）。分隔记录 layer
 * 为 nullptr，此时它只需要 lsct=3 与占位名字。 */
void write_extra(std::vector<uint8_t>* extra, const PsdLayerData* layer, bool emit_lsct,
                 bool is_divider, bool has_mask, int32_t mask_left, int32_t mask_top,
                 int32_t mask_width, int32_t mask_height) {
  extra->clear();
  if (has_mask) {
    put32(extra, 18u + 2u); /* 18 字节内容 + 补到 4 */
    put32(extra, static_cast<uint32_t>(mask_top));
    put32(extra, static_cast<uint32_t>(mask_left));
    put32(extra, static_cast<uint32_t>(mask_top + mask_height));
    put32(extra, static_cast<uint32_t>(mask_left + mask_width));
    extra->push_back(0); /* default color */
    extra->push_back(0); /* flags */
    extra->push_back(0); /* pad */
    extra->push_back(0);
  } else {
    put32(extra, 0);
  }

  /* 混合范围：写 40 字节的全范围，与 Photoshop 一致（长度 0 也合法，但会被
   * 一些插件当成「没有混合范围」而跳过）。 */
  put32(extra, 40u);
  for (int i = 0; i < 10; ++i) {
    put16(extra, 0u);
    put16(extra, 65535u);
  }

  /* Pascal 名字：非 ASCII 一律换成 '?'，真正的名字走 luni。 */
  std::string name = layer != nullptr ? layer->name : std::string();
  if (name.empty()) name = is_divider ? "</Layer group>" : "Layer";
  std::string ascii;
  ascii.reserve(name.size());
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    ascii.push_back(u < 0x80u ? c : '?');
  }
  if (ascii.size() > 255u) ascii.resize(255u);
  const size_t pascal_start = extra->size();
  extra->push_back(static_cast<uint8_t>(ascii.size()));
  put_bytes(extra, ascii.data(), ascii.size());
  /* 对齐按 (1 + len) 相对字符串起点算，和 psd-tools 的 write_pascal_string 一致。 */
  while (((extra->size() - pascal_start) % 4u) != 0u) extra->push_back(0);

  /* tag：luni（真名）+ lsct（组信息）。 */
  {
    std::vector<uint8_t> body;
    std::vector<uint8_t> units;
    utf8_to_utf16be(name, &units);
    put32(&body, static_cast<uint32_t>(units.size() / 2u));
    put_bytes(&body, units.data(), units.size());
    put_bytes(extra, "8BIM", 4);
    put_bytes(extra, "luni", 4);
    put32(extra, static_cast<uint32_t>(body.size()));
    put_bytes(extra, body.data(), body.size());
  }

  if (emit_lsct) {
    std::vector<uint8_t> body;
    put32(&body, is_divider ? 3u : 1u);
    put_bytes(&body, "8BIM", 4);
    const char* key = "norm";
    if (!is_divider) {
      key = layer->pass_through ? "pass" : blend_key_of(layer->blend);
    }
    put_bytes(&body, key, 4);
    put_bytes(extra, "8BIM", 4);
    put_bytes(extra, "lsct", 4);
    put32(extra, static_cast<uint32_t>(body.size()));
    put_bytes(extra, body.data(), body.size());
  }

  pad_to(extra, 2);
}

}  // namespace

/* ================================================================ 公开接口 */

const char* psd_status_name(PsdStatus status) {
  switch (status) {
    case PsdStatus::kOk: return "ok";
    case PsdStatus::kIo: return "io";
    case PsdStatus::kUnsupported: return "unsupported";
    case PsdStatus::kDecode: return "decode";
  }
  return "unknown";
}

bool psd_declared_size(const uint8_t* data, size_t size, int32_t* width, int32_t* height,
                       uint16_t* depth, std::string* error) {
  if (data == nullptr || size < 26) {
    if (error != nullptr) *error = "PSD: 文件头不足 26 字节";
    return false;
  }
  if (std::memcmp(data, "8BPS", 4) != 0) {
    if (error != nullptr) *error = "PSD: 缺少 8BPS 签名";
    return false;
  }
  const uint16_t version = be16(data + 4);
  if (version == kPsdVersionPsb) {
    if (error != nullptr) *error = "PSD: 这是 PSB（大文档）格式，本读写器只支持 PSD";
    return false;
  }
  if (version != 1) {
    if (error != nullptr) *error = "PSD: 未知的版本号 " + std::to_string(version);
    return false;
  }
  const int32_t declared_height = be32s(data + 14);
  const int32_t declared_width = be32s(data + 18);
  const uint16_t declared_depth = be16(data + 22);
  if (declared_depth != 8 && declared_depth != 16) {
    if (error != nullptr) *error = "PSD: 只支持 8 位与 16 位通道";
    return false;
  }
  if (!is_valid_canvas(comp_size{declared_width, declared_height})) {
    if (error != nullptr) *error = "PSD: 画布尺寸超出引擎上限";
    return false;
  }
  if (width != nullptr) *width = declared_width;
  if (height != nullptr) *height = declared_height;
  if (depth != nullptr) *depth = declared_depth;
  return true;
}

PsdStatus psd_decode_document(const uint8_t* data, size_t size, PsdDocument* out,
                              std::string* error) {
  PsdStatus status = PsdStatus::kOk;
  if (out == nullptr) {
    if (error != nullptr) *error = "PSD: 输出参数为空";
    return PsdStatus::kIo;
  }
  out->layers.clear();
  out->width = 0;
  out->height = 0;
  out->depth = 8;

  int32_t width = 0;
  int32_t height = 0;
  uint16_t depth = 8;
  if (!psd_declared_size(data, size, &width, &height, &depth, error)) {
    return PsdStatus::kUnsupported;
  }
  if (error != nullptr) error->clear();

  Cursor cursor;
  cursor.data = data;
  cursor.size = size;
  cursor.pos = 0;
  cursor.error = error;
  cursor.status = &status;

  /* 文件头。 */
  if (!cursor.skip(4)) return status;
  const uint8_t* header = nullptr;
  if (!cursor.take(22, &header)) return status;
  /* header 从文件偏移 4 开始：+8 = 通道数（文件 12），+20 = 色彩模式（文件 24）。 */
  const uint16_t channels = be16(header + 8);
  const uint16_t mode = be16(header + 20);
  if (channels < 3) {
    if (error != nullptr) {
      *error = "PSD: 通道数 " + std::to_string(static_cast<int>(channels)) +
               " 小于 3，不是 RGB 分层文件";
    }
    return PsdStatus::kUnsupported;
  }
  if (mode != kColorModeRgb) {
    const char* name = "未知色彩模式";
    switch (mode) {
      case 0: name = "位图"; break;
      case 1: name = "灰度"; break;
      case 2: name = "索引色"; break;
      case 4: name = "CMYK"; break;
      case 7: name = "多通道"; break;
      case 8: name = "双色调"; break;
      case 9: name = "Lab"; break;
      default: break;
    }
    if (error != nullptr) {
      *error = std::string("PSD: 不支持的色彩模式（") + name +
               "），本读写器只支持 RGB";
    }
    return PsdStatus::kUnsupported;
  }

  /* 色彩模式数据段 + 图像资源段：跳过。 */
  uint32_t section = 0;
  if (!cursor.u32(&section)) return status;
  if (!cursor.skip(section)) return status;
  if (!cursor.u32(&section)) return status;
  if (!cursor.skip(section)) return status;

  /* 图层与蒙版段。 */
  uint32_t layer_mask_len = 0;
  if (!cursor.u32(&layer_mask_len)) return status;
  if (layer_mask_len == 0) {
    out->width = width;
    out->height = height;
    out->depth = depth;
    return PsdStatus::kOk;
  }
  if (layer_mask_len > cursor.left()) return PsdStatus::kIo;

  const size_t layer_mask_end = cursor.pos + layer_mask_len;
  PsdDocument document;
  document.width = width;
  document.height = height;
  document.depth = depth;

  /* 主 layer-info 段。 */
  uint32_t layer_info_len = 0;
  if (!cursor.u32(&layer_info_len)) return status;
  bool parsed = false;
  if (layer_info_len > 0) {
    if (layer_info_len > cursor.left()) return PsdStatus::kIo;
    const uint8_t* body = nullptr;
    if (!cursor.take(layer_info_len, &body)) return status;
    if (!parse_layer_info(body, layer_info_len, &document, &cursor)) return status;
    parsed = true;
  }

  /* 全局图层蒙版段。 */
  if (cursor.pos + 4 <= layer_mask_end) {
    uint32_t global_len = 0;
    if (!cursor.u32(&global_len)) return status;
    if (global_len > layer_mask_end - cursor.pos) return PsdStatus::kIo;
    if (!cursor.skip(global_len)) return status;
  }

  /* 附加图层信息：16 位文档的图层就在这里。 */
  while (cursor.pos + 12 <= layer_mask_end) {
    const uint8_t* tag = nullptr;
    if (!cursor.take(12, &tag)) return status;
    if (std::memcmp(tag, "8BIM", 4) != 0 && std::memcmp(tag, "8B64", 4) != 0) break;
    const uint8_t* key = tag + 4;
    const uint32_t tag_len = be32(tag + 8);
    if (tag_len > layer_mask_end - cursor.pos) return PsdStatus::kIo;
    if (std::memcmp(key, "Lr32", 4) == 0) {
      if (error != nullptr) *error = "PSD: 32 位文档的图层块（Lr32）不在支持范围内";
      return PsdStatus::kUnsupported;
    }
    if (!parsed && std::memcmp(key, "Lr16", 4) == 0) {
      const uint8_t* body = nullptr;
      if (!cursor.take(tag_len, &body)) return status;
      if (tag_len < 2) return PsdStatus::kIo;
      /* 两种形态都认：带外层长度（规范写法）与不带（Photoshop / psd-tools 的
       * 实际写法，记录从 i16 count 直接开始）。判据是「前 4 字节是否正好
       * 等于本块长度减 4」。 */
      size_t offset = 0;
      if (tag_len >= 6 && be32(body) + 4u == tag_len) {
        offset = 4;
      }
      if (!parse_layer_info(body + offset, tag_len - offset, &document, &cursor)) return status;
      parsed = true;
    } else {
      if (!cursor.skip(tag_len)) return status;
    }
    /* 附加块数据补到偶数。 */
    if ((tag_len & 1u) != 0u && cursor.pos < layer_mask_end) {
      if (!cursor.skip(1)) return status;
    }
  }

  resolve_clipping(&document);
  *out = std::move(document);
  return PsdStatus::kOk;
}

bool psd_encode_document(const PsdDocument& document, const uint8_t* merged,
                         comp_pixel_format merged_format, size_t merged_stride,
                         std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "PSD: 输出参数为空";
    return false;
  }
  if (document.depth != 8 && document.depth != 16) {
    if (error != nullptr) *error = "PSD: 只支持 8 位与 16 位";
    return false;
  }
  if (!is_valid_canvas(comp_size{document.width, document.height})) {
    if (error != nullptr) *error = "PSD: 画布尺寸超出引擎上限";
    return false;
  }
  if (merged_format != COMP_PIXEL_RGBA8 && merged_format != COMP_PIXEL_RGBA16) {
    if (error != nullptr) *error = "PSD: 合并图像只支持 RGBA8 / RGBA16";
    return false;
  }
  const size_t merged_bpp = merged_format == COMP_PIXEL_RGBA8 ? 4u : 8u;
  if (merged != nullptr && merged_stride < static_cast<size_t>(document.width) * merged_bpp) {
    if (error != nullptr) *error = "PSD: 合并图像行跨度过小";
    return false;
  }

  const size_t layer_count_limit = static_cast<size_t>(document.layers.size());
  const int32_t count = static_cast<int32_t>(layer_count_limit);

  /* ---- 结构校验：父级合法、子层连续 ---- */
  for (int32_t i = 0; i < count; ++i) {
    const PsdLayerData& layer = document.layers[static_cast<size_t>(i)];
    if (layer.parent < -1 || layer.parent >= count) {
      if (error != nullptr) *error = "PSD: 图层父级下标越界";
      return false;
    }
    if (layer.parent >= 0 && !document.layers[static_cast<size_t>(layer.parent)].is_group) {
      if (error != nullptr) *error = "PSD: 图层的父级不是组";
      return false;
    }
    if (layer.is_group) {
      if (!layer.pixels.empty() || !layer.mask.empty()) {
        if (error != nullptr) *error = "PSD: 组不能带像素或蒙版";
        return false;
      }
      if (layer.clipping) {
        if (error != nullptr) *error = "PSD: 组不能被剪贴";
        return false;
      }
    }
  }
  /* ---- 递归生成记录顺序 ---- */
  /* 一条待写出的记录。组的两个记录都指向图层表里的同一个条目：
   * 边界分隔（divider，lsx=3）在子层下方，文件夹记录（folder）在子层上方。 */
  struct Emitted {
    const PsdLayerData* layer = nullptr;
    bool divider = false;
  };
  std::vector<Emitted> emission;
  std::function<void(int32_t)> emit_scope;
  emit_scope = [&](int32_t parent) {
    for (int32_t i = 0; i < count; ++i) {
      if (document.layers[static_cast<size_t>(i)].parent != parent) continue;
      const PsdLayerData& layer = document.layers[static_cast<size_t>(i)];
      if (layer.is_group) {
        emission.push_back(Emitted{&layer, true});  /* lsct=3 边界分隔 */
        emit_scope(i);                              /* 子层 */
        emission.push_back(Emitted{&layer, false}); /* lsct=1 文件夹记录 */
      } else {
        emission.push_back(Emitted{&layer, false});
      }
    }
  };
  emit_scope(-1);

  /*
   * 记录顺序必须与图层表顺序一致：PSD 里组的边界分隔记录**就是**图层表里的那个
   * 组条目，文件夹记录不产出条目，所以读回来的表序 = 去掉文件夹记录后的展开序。
   * 若表序与展开序不同，写出去的 PSD 读回来图层表就变了（内存里的 z 序与文件里的
   * z 序不一致），这是硬约束而不是可修复的偏差。
   * 顺带也挡住了不可达的层（父级成环 / 父级指向自己）——它们不会出现在展开里。
   */
  {
    size_t next = 0;
    for (const Emitted& entry : emission) {
      if (!entry.divider && entry.layer->is_group) continue; /* 文件夹记录 */
      if (next >= static_cast<size_t>(count) || &document.layers[next] != entry.layer) {
        if (error != nullptr) {
          *error = "PSD: 图层表顺序与图层树不一致（第 " + std::to_string(next) +
                   " 项），无法写成 PSD 的连续分段结构";
        }
        return false;
      }
      ++next;
    }
    if (next != static_cast<size_t>(count)) {
      if (error != nullptr) *error = "PSD: 有图层不在图层树上（父级成环或不可达）";
      return false;
    }
  }

  if (emission.size() > static_cast<size_t>(kMaxRecordCount)) {
    if (error != nullptr) *error = "PSD: 图层记录数超过上限";
    return false;
  }

  /* ---- 记录头 + 通道数据 ---- */
  std::vector<RecordBlob> blobs;
  blobs.reserve(emission.size());
  for (const Emitted& entry : emission) {
    const PsdLayerData* layer = entry.layer;
    RecordBlob blob;
    if (layer->is_group) {
      std::vector<uint8_t> extra;
      const bool is_divider = entry.divider;
      write_extra(&extra, layer, /*emit_lsct=*/true, is_divider,
                  /*has_mask=*/false, 0, 0, 0, 0);
      if (is_divider) {
        write_empty_record(&blob, "norm", 255, true, extra);
      } else {
        const char* key = layer->pass_through ? "pass" : blend_key_of(layer->blend);
        write_empty_record(&blob, key, layer->opacity, !layer->visible, extra);
      }
      blobs.push_back(std::move(blob));
      continue;
    }

    const int32_t width = layer->bounds.w;
    const int32_t height = layer->bounds.h;
    if (width > limits::kMaxSide || height > limits::kMaxSide ||
        static_cast<int64_t>(width) * height > limits::kMaxSurfacePixels) {
      if (error != nullptr) *error = "PSD: 图层尺寸超出引擎上限";
      return false;
    }
    if (width > 0 && height > 0) {
      if (layer->pixels.empty()) {
        if (error != nullptr) *error = "PSD: 图层声明了尺寸却没有像素";
        return false;
      }
      if (layer->pixels.width != width || layer->pixels.height != height) {
        if (error != nullptr) *error = "PSD: 图层像素尺寸与矩形不一致";
        return false;
      }
      if (document.depth == 16 && layer->pixels.format != COMP_PIXEL_RGBA16) {
        if (error != nullptr) *error = "PSD: 16 位文档里的图层必须是 RGBA16";
        return false;
      }
      if (document.depth == 8 && layer->pixels.format != COMP_PIXEL_RGBA8) {
        if (error != nullptr) *error = "PSD: 8 位文档里的图层必须是 RGBA8";
        return false;
      }
    }
    const bool has_mask = !layer->mask.empty();
    if (has_mask && (layer->mask.width != width || layer->mask.height != height ||
                     width <= 0 || height <= 0)) {
      /* 引擎的蒙版恒在图层像素空间；尺寸不符说明上游违反了契约，不能悄悄丢掉。 */
      if (error != nullptr) *error = "PSD: 蒙版尺寸与图层矩形不一致";
      return false;
    }

    std::vector<uint8_t> extra;
    write_extra(&extra, layer, /*emit_lsct=*/false, /*is_divider=*/false, has_mask,
                layer->bounds.x, layer->bounds.y, width, height);

    const int16_t alpha_id = -1;
    const int16_t mask_id = -2;
    const int16_t channel_ids[5] = {alpha_id, 0, 1, 2, mask_id};
    const size_t channel_total = has_mask ? 5u : 4u;

    std::vector<std::pair<int16_t, uint32_t>> channel_lengths;
    channel_lengths.reserve(channel_total);

    const size_t sample_bytes = document.depth == 16 ? 2u : 1u;
    std::vector<uint8_t> plane;

    for (size_t i = 0; i < channel_total; ++i) {
      const int16_t id = channel_ids[i];
      if (id == mask_id) {
        /* 蒙版通道恒为 8 位，行紧排。 */
        plane.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
        for (int32_t y = 0; y < height; ++y) {
          std::memcpy(plane.data() + static_cast<size_t>(y) * static_cast<size_t>(width),
                      layer->mask.row(y), static_cast<size_t>(width));
        }
        std::vector<uint8_t> block;
        if (!encode_channel_block(plane.data(), width, height, 1u, &block, error)) return false;
        channel_lengths.push_back(std::make_pair(id, static_cast<uint32_t>(block.size())));
        blob.channels.insert(blob.channels.end(), block.begin(), block.end());
        continue;
      }
      if (width > 0 && height > 0) {
        extract_channel(layer->pixels, id, document.depth, &plane);
      } else {
        plane.clear();
      }
      std::vector<uint8_t> block;
      if (!encode_channel_block(plane.empty() ? nullptr : plane.data(), width, height,
                                sample_bytes, &block, error)) {
        return false;
      }
      channel_lengths.push_back(std::make_pair(id, static_cast<uint32_t>(block.size())));
      blob.channels.insert(blob.channels.end(), block.begin(), block.end());
    }

    put32(&blob.header, static_cast<uint32_t>(layer->bounds.y));
    put32(&blob.header, static_cast<uint32_t>(layer->bounds.x));
    put32(&blob.header, static_cast<uint32_t>(layer->bounds.y + height));
    put32(&blob.header, static_cast<uint32_t>(layer->bounds.x + width));
    put16(&blob.header, static_cast<uint16_t>(channel_lengths.size()));
    for (const std::pair<int16_t, uint32_t>& item : channel_lengths) {
      put16(&blob.header, static_cast<uint16_t>(item.first));
      put32(&blob.header, item.second);
    }
    put_bytes(&blob.header, "8BIM", 4);
    put_bytes(&blob.header, blend_key_of(layer->blend), 4);
    blob.header.push_back(layer->opacity);
    blob.header.push_back(layer->clipping ? 1u : 0u);
    blob.header.push_back(layer->visible ? 0x08u : 0x0Au);
    blob.header.push_back(0);
    put32(&blob.header, static_cast<uint32_t>(extra.size()));
    put_bytes(&blob.header, extra.data(), extra.size());

    blobs.push_back(std::move(blob));
  }

  /* ---- 组装文件 ---- */
  out->clear();
  put_bytes(out, "8BPS", 4);
  put16(out, 1);                              /* 版本 */
  put32(out, 0);                              /* 保留 6 字节（不是 2 字节，别省） */
  put16(out, 0);
  put16(out, 4);                              /* 通道数：R/G/B/A */
  put32(out, static_cast<uint32_t>(document.height));
  put32(out, static_cast<uint32_t>(document.width));
  put16(out, document.depth);
  put16(out, kColorModeRgb);
  put32(out, 0);                              /* 色彩模式数据 */
  put32(out, 0);                              /* 图像资源 */

  /* 图层与蒙版段。 */
  const size_t layer_mask_len_pos = out->size();
  put32(out, 0);

  std::vector<uint8_t> layer_info;
  {
    std::vector<uint8_t> records;
    std::vector<uint8_t> channel_data;
    for (const RecordBlob& blob : blobs) {
      records.insert(records.end(), blob.header.begin(), blob.header.end());
      channel_data.insert(channel_data.end(), blob.channels.begin(), blob.channels.end());
    }
    /* 记录数：负数表示合并图像的第 4 个通道是透明度。 */
    const int32_t signed_count = -static_cast<int32_t>(blobs.size());
    put16(&layer_info, static_cast<uint16_t>(static_cast<int16_t>(signed_count)));
    layer_info.insert(layer_info.end(), records.begin(), records.end());
    layer_info.insert(layer_info.end(), channel_data.begin(), channel_data.end());
    pad_to(&layer_info, 4);
  }

  if (document.depth == 16) {
    /* 16 位：主段长度写 0，记录放进 Lr16 附加块。
     * 块内**不带**外层长度字段——Photoshop 与 psd-tools 的实际写法都是这样
     * （psd-tools 的 LayerInfoBlock 直接按 i16 count 开始读）。 */
    put32(out, 0);
    put32(out, 0); /* 全局图层蒙版 */
    put_bytes(out, "8BIM", 4);
    put_bytes(out, "Lr16", 4);
    put32(out, static_cast<uint32_t>(layer_info.size()));
    put_bytes(out, layer_info.data(), layer_info.size());
  } else {
    put32(out, static_cast<uint32_t>(layer_info.size()));
    put_bytes(out, layer_info.data(), layer_info.size());
    put32(out, 0); /* 全局图层蒙版 */
  }
  put32_at(out, layer_mask_len_pos, static_cast<uint32_t>(out->size() - layer_mask_len_pos - 4));

  /* 合并图像数据：RLE，4 个通道。 */
  {
    const size_t sample_bytes = document.depth == 16 ? 2u : 1u;
    const size_t row_bytes = static_cast<size_t>(document.width) * sample_bytes;
    const int32_t height = document.height;
    std::vector<std::vector<uint8_t>> rows;
    rows.reserve(static_cast<size_t>(4) * static_cast<size_t>(height));

    for (int32_t channel = 0; channel < 4; ++channel) {
      for (int32_t y = 0; y < height; ++y) {
        std::vector<uint8_t> row(row_bytes, 0u);
        if (merged != nullptr) {
          const uint8_t* source = merged + static_cast<size_t>(y) * merged_stride;
          if (document.depth == 16) {
            for (int32_t x = 0; x < document.width; ++x) {
              uint32_t value = 0;
              uint32_t alpha = 0;
              if (merged_format == COMP_PIXEL_RGBA8) {
                alpha = static_cast<uint32_t>(source[x * 4 + 3]) * 257u;
              } else {
                const uint16_t* wide_row = reinterpret_cast<const uint16_t*>(source);
                alpha = wide_row[x * 4 + 3];
              }
              if (channel < 3) {
                if (merged_format == COMP_PIXEL_RGBA8) {
                  value = static_cast<uint32_t>(source[x * 4 + channel]) * 257u;
                } else {
                  const uint16_t* wide_row = reinterpret_cast<const uint16_t*>(source);
                  value = wide_row[x * 4 + channel];
                }
                if (alpha != 0u && alpha != 65535u) {
                  value = static_cast<uint32_t>(
                      (static_cast<uint64_t>(value) * 65535u + alpha / 2u) / alpha);
                  if (value > 65535u) value = 65535u;
                }
              } else {
                value = alpha;
              }
              row[x * 2 + 0] = static_cast<uint8_t>(value >> 8);
              row[x * 2 + 1] = static_cast<uint8_t>(value & 0xFFu);
            }
          } else {
            for (int32_t x = 0; x < document.width; ++x) {
              uint32_t alpha = 0;
              if (merged_format == COMP_PIXEL_RGBA8) {
                alpha = source[x * 4 + 3];
              } else {
                const uint16_t* wide_row = reinterpret_cast<const uint16_t*>(source);
                alpha = (wide_row[x * 4 + 3] + 128u) / 257u;
              }
              uint32_t value = 0;
              if (channel < 3) {
                if (merged_format == COMP_PIXEL_RGBA8) {
                  value = source[x * 4 + channel];
                } else {
                  const uint16_t* wide_row = reinterpret_cast<const uint16_t*>(source);
                  value = (wide_row[x * 4 + channel] + 128u) / 257u;
                }
                if (alpha != 0u && alpha != 255u) {
                  value = (value * 255u + alpha / 2u) / alpha;
                  if (value > 255u) value = 255u;
                }
              } else {
                value = alpha;
              }
              row[x] = static_cast<uint8_t>(value);
            }
          }
        }
        rows.push_back(std::move(row));
      }
    }

    put16(out, kPsdCompressionRle);
    std::vector<uint8_t> packed_rows;
    std::vector<uint16_t> counts;
    counts.reserve(rows.size());
    for (std::vector<uint8_t>& row : rows) {
      const size_t before = packed_rows.size();
      pack_row(row.data(), row.size(), &packed_rows);
      const size_t len = packed_rows.size() - before;
      if (len > 0xFFFFu) {
        if (error != nullptr) *error = "PSD: 合并图像某行压缩后超过 65535 字节";
        return false;
      }
      counts.push_back(static_cast<uint16_t>(len));
    }
    for (uint16_t value : counts) put16(out, value);
    out->insert(out->end(), packed_rows.begin(), packed_rows.end());
  }

  return true;
}

}  // namespace compositor
