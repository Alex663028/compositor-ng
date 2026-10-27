// AI生成
/*
 * type_raster.cpp — 文字工具（S11）的位图字体与栅格化内核。
 *
 * 字体口径（见 type_raster.h）：5 列 × 7 行的点阵字体，95 个可打印 ASCII
 * （0x20..0x7E）。表是**本仓自带的**：不链接 FreeType/CoreText，也不从宿主
 * 灌字形图集——宿主给的字形图集属于「另一套字体」，内核说了不算（与 Dither
 * 的 GLYPHS 缺图集时如实回落同一条原则：不假装自己有宿主资源）。
 *
 * 位序：每字符 5 字节 = 5 列（列优先），字节的 bit0 是最上一行、bit6 是第七行。
 * 上行与下行之间留 1 列空隙（步进 6 列），与经典 5×7 点阵字体一致。
 *
 * 覆盖率口径（抗锯齿）：
 *   对每个源行带（7 行 × 缩放）与每个「连续置位列段」，把它的矩形面积按
 *   目标像素切开累加——即**精确面积**，不是 4×4 超采样。前提是这些矩形
 *   互不重叠：同一行带内的合并段互不相交，行带之间垂直不相交，行与行之间
 *   因为 line_height >= 1（参数闸保证）也不相交。因此逐像素累加不会超过 1，
 *   末了饱和到 255 即可（无需 float 累加缓冲）。
 *   antialias 关闭时退化为像素中心一票（硬边）。
 */
#include "domain/type_raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "domain/font_face.h"

namespace compositor {
namespace {

/*
 * 5×7 ASCII 点阵字体（0x20..0x7E）。列优先，bit0 = 最上一行。
 * 字形是「方正、无下伸」的经典点阵写法：基线在第 7 行（下标 6），小写字母的
 * 降部（g/j/p/q/y）也收在行内——7 行装不下真正的降部，这是字体的已知取舍
 * （见 type_raster.h 的「明确不支持」）。
 */
const uint8_t kTypeFont5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 0x20 ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* 0x21 '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 0x22 '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* 0x23 '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* 0x24 '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 0x25 '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 0x26 '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 0x27 '\'' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* 0x28 '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* 0x29 ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* 0x2A '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* 0x2B '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 0x2C ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 0x2D '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 0x2E '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 0x2F '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0x30 '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 0x31 '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 0x32 '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 0x33 '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 0x34 '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 0x35 '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 0x36 '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 0x37 '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 0x38 '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 0x39 '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 0x3A ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 0x3B ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 0x3C '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 0x3D '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 0x3E '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 0x3F '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* 0x40 '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 0x41 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 0x42 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 0x43 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 0x44 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 0x45 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 0x46 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 0x47 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 0x48 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 0x49 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 0x4A 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 0x4B 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 0x4C 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 0x4D 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 0x4E 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 0x4F 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 0x50 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 0x51 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 0x52 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 0x53 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 0x54 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 0x55 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 0x56 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 0x57 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 0x58 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 0x59 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 0x5A 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* 0x5B '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* 0x5C '\\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* 0x5D ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* 0x5E '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* 0x5F '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* 0x60 '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 0x61 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 0x62 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 0x63 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 0x64 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 0x65 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 0x66 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 0x67 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 0x68 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 0x69 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 0x6A 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 0x6B 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 0x6C 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 0x6D 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 0x6E 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 0x6F 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 0x70 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 0x71 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 0x72 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 0x73 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 0x74 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 0x75 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 0x76 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 0x77 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 0x78 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 0x79 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 0x7A 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* 0x7B '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* 0x7C '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* 0x7D '}' */
    {0x08, 0x04, 0x08, 0x10, 0x08}, /* 0x7E '~' */
};

/* 单字符步进下限：再小就不是「排字」，是同一位置叠印。 */
constexpr double kMinTypeAdvance = 0.5;

/* 排版结果：字形顺序 + 每行的起止下标（line_begin 多一个尾哨兵）。 */
struct TypeLayout {
  std::vector<char> glyphs;
  std::vector<size_t> line_begin;
  TypeMeasure measure;
  double scale = 1.0;
  double advance = 0.0;
  double line_advance = 0.0;
};

bool finite_range(double value, double low, double high) {
  return std::isfinite(value) && value >= low && value <= high;
}

bool validate_style(const TypeRasterStyle& style, double* scale, double* advance, const char** error) {
  if (!finite_range(style.font_size, kMinTypeFontSize, kMaxTypeFontSize)) {
    *error = "type font size must be within 6..2048 document px";
    return false;
  }
  if (!finite_range(style.line_height, kMinTypeLineHeight, kMaxTypeLineHeight)) {
    *error = "type line height must be within 1..8 (lines may not overlap)";
    return false;
  }
  if (!std::isfinite(style.letter_spacing) ||
      std::fabs(style.letter_spacing) > kMaxTypeLetterSpacing) {
    *error = "type letter spacing is not finite or out of range";
    return false;
  }
  *scale = style.font_size / static_cast<double>(kTypeFontRows);
  *advance = kTypeFontAdvanceColumns * *scale + style.letter_spacing;
  if (!(*advance >= kMinTypeAdvance)) {
    *error = "type letter spacing makes the glyph advance smaller than 0.5 document px";
    return false;
  }
  return true;
}

/* 逐字节校验文本；只认 0x20..0x7E 与 '\n'。 */
bool scan_text(const char* text, size_t size, const char** error) {
  if (text == nullptr) {
    *error = "type text pointer is null";
    return false;
  }
  if (size == 0) {
    *error = "type text is empty";
    return false;
  }
  int64_t glyphs = 0;
  for (size_t i = 0; i < size; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch == '\n') continue;
    if (ch < 0x20 || ch > 0x7E) {
      /* UTF-8 多字节、Tab 等控制字符统统拒绝：位图字体没有对应字形，
       * 静默画成问号/方框会让宿主以为字符被支持了。 */
      *error = "type text contains a byte outside printable ASCII 0x20..0x7E (UTF-8/CJK and "
               "control characters are not supported)";
      return false;
    }
    ++glyphs;
  }
  if (glyphs == 0) {
    *error = "type text has no printable glyphs";
    return false;
  }
  return true;
}

TypeRasterReport make_report(TypeRasterStatus status, const char* message) {
  TypeRasterReport report;
  report.status = status;
  report.message = message;
  return report;
}

/* 校验 + 排版（不落像素）。所有入口共用，保证「量」与「画」的口径一致。 */
TypeRasterReport build_layout(const char* text, size_t size, const TypeRasterStyle& style,
                              TypeLayout* out) {
  if (size > kMaxTypeCharacters) {
    return make_report(TypeRasterStatus::kTooLong, "type text exceeds 65536 characters");
  }
  const char* error = "";
  if (!scan_text(text, size, &error)) {
    return make_report(TypeRasterStatus::kBadText, error);
  }
  double scale = 1.0;
  double advance = 0.0;
  if (!validate_style(style, &scale, &advance, &error)) {
    return make_report(TypeRasterStatus::kBadStyle, error);
  }

  out->glyphs.clear();
  out->glyphs.reserve(size);
  out->line_begin.clear();
  out->line_begin.push_back(0);
  for (size_t i = 0; i < size; ++i) {
    if (text[i] == '\n') {
      out->line_begin.push_back(out->glyphs.size());
    } else {
      out->glyphs.push_back(text[i]);
    }
  }
  out->line_begin.push_back(out->glyphs.size());
  out->scale = scale;
  out->advance = advance;
  out->line_advance = style.font_size * style.line_height;

  const size_t line_count = out->line_begin.size() - 1;
  size_t widest = 0;
  for (size_t line = 0; line < line_count; ++line) {
    widest = std::max(widest, out->line_begin[line + 1] - out->line_begin[line]);
  }
  out->measure.line_count = static_cast<int32_t>(line_count);
  out->measure.glyph_count = static_cast<int64_t>(out->glyphs.size());
  out->measure.advance = advance;
  out->measure.width = static_cast<double>(widest) * advance;
  out->measure.height =
      style.font_size + static_cast<double>(line_count - 1) * out->line_advance;
  return make_report(TypeRasterStatus::kOk, "");
}

/* 图层局部窗口：把文本框裁到图层像素范围；返回 false = 完全不相交。 */
bool clip_window(double origin_x, double origin_y, double width, double height, int32_t layer_x,
                 int32_t layer_y, int32_t layer_width, int32_t layer_height, int64_t* x0,
                 int64_t* y0, int64_t* x1, int64_t* y1) {
  const int64_t layer_x0 = layer_x;
  const int64_t layer_y0 = layer_y;
  const int64_t layer_x1 = layer_x0 + layer_width;
  const int64_t layer_y1 = layer_y0 + layer_height;
  *x0 = std::max<int64_t>(static_cast<int64_t>(std::floor(origin_x)), layer_x0);
  *y0 = std::max<int64_t>(static_cast<int64_t>(std::floor(origin_y)), layer_y0);
  *x1 = std::min<int64_t>(static_cast<int64_t>(std::ceil(origin_x + width)), layer_x1);
  *y1 = std::min<int64_t>(static_cast<int64_t>(std::ceil(origin_y + height)), layer_y1);
  return *x1 > *x0 && *y1 > *y0;
}

/* ---------- 字体轨道（P5-2）：FreeType 字形排版 ---------- */

/* 排版结果：码位顺序 + 每行起止下标（line_begin 多一个尾哨兵）+ 字体度量。 */
struct FontLayout {
  std::vector<uint32_t> codepoints;
  std::vector<size_t> line_begin;
  TypeMeasure measure;
  double ascender = 0.0;     /* 文档像素（已乘 em 高） */
  double line_pitch = 0.0;   /* ascender - descender，文档像素 */
  double line_advance = 0.0; /* font_size × line_height */
  double letter_spacing = 0.0;
};

/*
 * 严格 UTF-8 解码：拒绝过长编码、代理对、> U+10FFFF 与孤立续字节。
 * '\n' 保留（排版按它断行），其余 C0/C1 控制字符拒绝——字体没有制表位语义，
 * 静默跳过会让宿主以为字符被排上了。
 *
 * `out_offsets`（可为 nullptr）逐码点记下起始字节偏移；段落折行只给宿主
 * 每一行的**字节**区间（图标外层的原文是字节串），没有这份映射就得二次扫串。
 */
bool decode_utf8(const char* text, size_t size, std::vector<uint32_t>* out,
                 std::vector<size_t>* out_offsets, const char** error) {
  size_t i = 0;
  while (i < size) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    uint32_t codepoint = 0;
    size_t extra = 0;
    uint32_t minimum = 0;
    if (lead < 0x80) {
      codepoint = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      codepoint = lead & 0x1Fu;
      extra = 1;
      minimum = 0x80u;
    } else if ((lead & 0xF0) == 0xE0) {
      codepoint = lead & 0x0Fu;
      extra = 2;
      minimum = 0x800u;
    } else if ((lead & 0xF8) == 0xF0) {
      codepoint = lead & 0x07u;
      extra = 3;
      minimum = 0x10000u;
    } else {
      *error = "type text is not valid UTF-8 (bad lead byte)";
      return false;
    }
    for (size_t k = 1; k <= extra; ++k) {
      if (i + k >= size) {
        *error = "type text is not valid UTF-8 (truncated sequence)";
        return false;
      }
      const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
      if ((continuation & 0xC0) != 0x80) {
        *error = "type text is not valid UTF-8 (bad continuation byte)";
        return false;
      }
      codepoint = (codepoint << 6) | static_cast<uint32_t>(continuation & 0x3F);
    }
    if (extra > 0 && (codepoint < minimum || codepoint > 0x10FFFF ||
                      (codepoint >= 0xD800 && codepoint <= 0xDFFF))) {
      *error = "type text is not valid UTF-8 (overlong or out-of-range sequence)";
      return false;
    }
    if (codepoint != '\n' && (codepoint < 0x20 || codepoint == 0x7F)) {
      *error = "type text contains a control character (only '\\n' is supported)";
      return false;
    }
    if (out_offsets != nullptr) out_offsets->push_back(i);
    out->push_back(codepoint);
    i += extra + 1;
  }
  return true;
}

/* 字体轨道的样式闸：量程与旧轨道一致，但步进要逐字形才知道（见 build_font_layout）。 */
bool validate_font_style(const TypeRasterStyle& style, const char** error) {
  if (!finite_range(style.font_size, kMinTypeFontSize, kMaxTypeFontSize)) {
    *error = "type font size must be within 6..2048 document px";
    return false;
  }
  if (!finite_range(style.line_height, kMinTypeLineHeight, kMaxTypeLineHeight)) {
    *error = "type line height must be within 1..8 (lines may not overlap)";
    return false;
  }
  if (!std::isfinite(style.letter_spacing) ||
      std::fabs(style.letter_spacing) > kMaxTypeLetterSpacing) {
    *error = "type letter spacing is not finite or out of range";
    return false;
  }
  return true;
}

TypeRasterReport build_font_layout(const char* text, size_t size, const TypeRasterStyle& style,
                                   FontFace* font, FontLayout* out) {
  if (font == nullptr) {
    return make_report(TypeRasterStatus::kFontError, "type font handle is null");
  }
  if (size > kMaxTypeCharacters) {
    return make_report(TypeRasterStatus::kTooLong, "type text exceeds 65536 characters");
  }
  if (text == nullptr) {
    return make_report(TypeRasterStatus::kBadText, "type text pointer is null");
  }
  const char* error = "";
  if (!validate_font_style(style, &error)) {
    return make_report(TypeRasterStatus::kBadStyle, error);
  }

  out->codepoints.clear();
  out->line_begin.clear();
  if (!decode_utf8(text, size, &out->codepoints, nullptr, &error)) {
    return make_report(TypeRasterStatus::kBadText, error);
  }

  const FontMetricsInfo& metrics = font->metrics();
  if (!metrics.scalable || metrics.units_per_em == 0) {
    return make_report(TypeRasterStatus::kFontError, "the loaded font is not scalable");
  }
  out->ascender = metrics.ascender * style.font_size;
  const double descender = metrics.descender * style.font_size;
  out->line_pitch = out->ascender - descender;
  if (!(out->line_pitch > 0.0)) {
    return make_report(TypeRasterStatus::kFontError, "the loaded font has no usable vertical metrics");
  }
  out->line_advance = style.font_size * style.line_height;
  out->letter_spacing = style.letter_spacing;

  out->line_begin.push_back(0);
  for (size_t i = 0; i < out->codepoints.size(); ++i) {
    if (out->codepoints[i] == '\n') out->line_begin.push_back(i + 1);
  }
  /*
   * 行尾哨兵：每行都靠 [begin, end) 取码位，end 来自下一行的起点。没有这个尾哨兵，
   * 最后一行永远落不进循环（单行文本会量出 line_count = 0、宽度 0，且一个字都不画）。
   * 文本以 '\n' 结尾时尾哨兵等于码位数，产生一个合法的空尾行——与常见文本编辑器
   * 的口径一致。
   */
  out->line_begin.push_back(out->codepoints.size());
  size_t glyphs = 0;
  for (uint32_t codepoint : out->codepoints) {
    if (codepoint != '\n') ++glyphs;
  }
  if (glyphs == 0) {
    return make_report(TypeRasterStatus::kBadText, "type text has no printable glyphs");
  }

  /*
   * 逐字形量宽度：字体里每个码位的步进都不同，不能像 5×7 那样用一个常数。
   * 这里顺带数缺字并把「字距把步进压成 0」「字形栅格化失败」在度量阶段就查出来，
   * 保证「量」与「画」用的是同一次排版结果。
   */
  int64_t missing = 0;
  double widest = 0.0;
  for (size_t line = 0; line + 1 < out->line_begin.size(); ++line) {
    const size_t begin = out->line_begin[line];
    const size_t end = out->line_begin[line + 1];
    bool first = true;
    double line_width = 0.0;
    for (size_t i = begin; i < end; ++i) {
      const uint32_t codepoint = out->codepoints[i];
      if (codepoint == '\n') continue;
      FontGlyph glyph;
      if (!font->render_glyph(codepoint, style.font_size, &glyph)) {
        return make_report(TypeRasterStatus::kFontError,
                           "the loaded font failed to rasterize a glyph");
      }
      if (glyph.missing) ++missing;
      const double step = glyph.advance + (first ? 0.0 : out->letter_spacing);
      if (!(step >= kMinTypeAdvance)) {
        return make_report(TypeRasterStatus::kBadStyle,
                          "type letter spacing makes the glyph advance smaller than 0.5 document px");
      }
      if (!first) line_width += out->letter_spacing;
      line_width += glyph.advance;
      first = false;
    }
    widest = std::max(widest, line_width);
  }

  out->measure.line_count = static_cast<int32_t>(out->line_begin.size() - 1);
  out->measure.glyph_count = static_cast<int64_t>(glyphs);
  out->measure.advance = 0.0;
  out->measure.width = widest;
  out->measure.height =
      out->line_pitch + static_cast<double>(out->measure.line_count - 1) * out->line_advance;
  out->measure.missing_glyphs = missing;
  return make_report(TypeRasterStatus::kOk, "");
}

void rasterize_antialiased(const TypeLayout& layout, double origin_x, double origin_y,
                           int64_t win_x0, int64_t win_y0, int64_t win_x1, int64_t win_y1,
                           TypeCoverage* out) {
  const double scale = layout.scale;
  const double advance = layout.advance;
  const double line_advance = layout.line_advance;
  const double left = static_cast<double>(win_x0);
  const double top = static_cast<double>(win_y0);
  const double right = static_cast<double>(win_x1);
  const double bottom = static_cast<double>(win_y1);
  const int32_t width = out->w;
  uint8_t* pixels = out->coverage.data();

  /* 复用同一块 run 缓冲：每行带最多 5×该行字形数 段。 */
  std::vector<std::pair<double, double>> runs;
  const size_t line_count = layout.line_begin.size() - 1;
  for (size_t line = 0; line < line_count; ++line) {
    const size_t begin = layout.line_begin[line];
    const size_t end = layout.line_begin[line + 1];
    if (begin == end) continue;
    const double line_top = origin_y + static_cast<double>(line) * line_advance;
    for (int32_t row = 0; row < kTypeFontRows; ++row) {
      const double band0 = line_top + static_cast<double>(row) * scale;
      const double band1 = band0 + scale;
      if (band1 <= top || band0 >= bottom) continue;
      runs.clear();
      for (size_t g = begin; g < end; ++g) {
        const uint8_t* glyph =
            kTypeFont5x7[static_cast<size_t>(static_cast<unsigned char>(layout.glyphs[g]) - 0x20)];
        if (glyph[row] == 0) continue;
        const double glyph_left = origin_x + static_cast<double>(g - begin) * advance;
        for (int32_t column = 0; column < kTypeFontColumns; ++column) {
          if ((glyph[column] & (1u << row)) == 0) continue;
          const double run0 = glyph_left + static_cast<double>(column) * scale;
          const double run1 = run0 + scale;
          if (run1 <= left || run0 >= right) continue;
          runs.emplace_back(run0, run1);
        }
      }
      if (runs.empty()) continue;
      /*
       * 字距为负时相邻字形会重叠，run 的生成顺序按「字形序 × 列序」，不是全局
       * x 序；先排序再合并区间，保证同一行带内每个像素只被加一次。
       */
      std::sort(runs.begin(), runs.end());
      size_t merged = 0;
      for (size_t i = 0; i < runs.size(); ++i) {
        if (merged > 0 && runs[i].first <= runs[merged - 1].second) {
          if (runs[i].second > runs[merged - 1].second) runs[merged - 1].second = runs[i].second;
        } else {
          runs[merged++] = runs[i];
        }
      }

      const int64_t first_row = static_cast<int64_t>(std::floor(std::max(band0, top)));
      const int64_t last_row = static_cast<int64_t>(std::ceil(std::min(band1, bottom)));
      for (int64_t y = first_row; y < last_row; ++y) {
        const double vertical =
            std::min(band1, static_cast<double>(y) + 1.0) - std::max(band0, static_cast<double>(y));
        if (vertical <= 0.0) continue;
        uint8_t* row_pixels = pixels + static_cast<size_t>(y - win_y0) * static_cast<size_t>(width);
        for (size_t i = 0; i < merged; ++i) {
          const double run0 = runs[i].first;
          const double run1 = runs[i].second;
          const int64_t first_column = static_cast<int64_t>(std::floor(std::max(run0, left)));
          const int64_t last_column = static_cast<int64_t>(std::ceil(std::min(run1, right)));
          for (int64_t x = first_column; x < last_column; ++x) {
            const double horizontal = std::min(run1, static_cast<double>(x) + 1.0) -
                                      std::max(run0, static_cast<double>(x));
            if (horizontal <= 0.0) continue;
            const long add = std::lround(vertical * horizontal * 255.0);
            if (add <= 0) continue;
            uint8_t& slot = row_pixels[x - win_x0];
            const int value = static_cast<int>(slot) + static_cast<int>(add);
            slot = static_cast<uint8_t>(value > 255 ? 255 : value);
          }
        }
      }
    }
  }
}

void rasterize_hard_edged(const TypeLayout& layout, double origin_x, double origin_y,
                          int64_t win_x0, int64_t win_y0, int64_t win_x1, int64_t win_y1,
                          TypeCoverage* out) {
  const double scale = layout.scale;
  const double advance = layout.advance;
  const double line_advance = layout.line_advance;
  const double total_width = layout.measure.width;
  const double total_height = layout.measure.height;
  const size_t line_count = layout.line_begin.size() - 1;
  const int32_t width = out->w;
  uint8_t* pixels = out->coverage.data();
  for (int64_t y = win_y0; y < win_y1; ++y) {
    const double center_y = static_cast<double>(y) + 0.5;
    const double local_y = center_y - origin_y;
    if (local_y < 0.0 || local_y >= total_height) continue;
    const size_t line = static_cast<size_t>(std::floor(local_y / line_advance));
    if (line >= line_count) continue;
    const double within_line = local_y - static_cast<double>(line) * line_advance;
    const int32_t row = static_cast<int32_t>(std::floor(within_line / scale));
    if (row < 0 || row >= kTypeFontRows) continue;
    const size_t begin = layout.line_begin[line];
    const size_t end = layout.line_begin[line + 1];
    uint8_t* row_pixels = pixels + static_cast<size_t>(y - win_y0) * static_cast<size_t>(width);
    for (int64_t x = win_x0; x < win_x1; ++x) {
      const double center_x = static_cast<double>(x) + 0.5;
      const double local_x = center_x - origin_x;
      if (local_x < 0.0 || local_x >= total_width) continue;
      const double glyph_index = std::floor(local_x / advance);
      const size_t offset = static_cast<size_t>(glyph_index);
      if (offset >= end - begin) continue; /* 短行右端 = 空白 */
      const double within_glyph = local_x - glyph_index * advance;
      const int32_t column = static_cast<int32_t>(std::floor(within_glyph / scale));
      if (column < 0 || column >= kTypeFontColumns) continue;
      const uint8_t* glyph = kTypeFont5x7[static_cast<size_t>(
          static_cast<unsigned char>(layout.glyphs[begin + offset]) - 0x20)];
      if ((glyph[column] & (1u << row)) != 0) row_pixels[x - win_x0] = 255;
    }
  }
}

}  // namespace

bool type_text_is_supported(const char* text, size_t size, const char** out_error) {
  const char* error = "";
  const bool ok = size <= kMaxTypeCharacters && scan_text(text, size, &error);
  if (out_error != nullptr) *out_error = ok ? "" : error;
  return ok;
}

TypeRasterReport type_measure_text(const char* text, size_t size, const TypeRasterStyle& style,
                                   TypeMeasure* out) {
  TypeLayout layout;
  const TypeRasterReport report = build_layout(text, size, style, &layout);
  if (out != nullptr) *out = report.status == TypeRasterStatus::kOk ? layout.measure : TypeMeasure{};
  return report;
}

/*
 * T14（2026-10-27）：合成粗体 / 斜体 / 基线偏移的后处理。
 *
 * 为什么做成**后处理**而不是改光栅化内核：两条渲染路径（点阵 AA / 字体轨道）
 * 各有自己的覆盖度累加循环，改内核等于把「字形几何」的知识复制两份。而粗体
 * （双描向右偏一点点再叠一次最大值）与斜体（逐行横向剪切）本质上都是**对已渲染
 * 覆盖度图的整体变换**，一次后处理即可覆盖两条路径，且 bold=italic=offset=0 时
 * 可以整段跳过——护栏断言「三者全 0 时逐字节等同 T14 之前」因此天然成立。
 *
 * 代价（如实记）：斜体是逐行整数像素剪切，边缘会有 1px 台阶，不是真斜体字形；
 * 粗体是双描，笔画端点比真粗体钝。两者与 PS 的「合成」口径同级，不是替代真字体。
 */
namespace {

/* 把覆盖度图按「逐行横向剪切」重采样：x' = x + round(-shear * (y - anchor))。
 * shear 取 1/4（约 14°），与 PS 合成斜体的观感接近。 */
void shear_coverage(TypeCoverage& coverage, double shear, int64_t anchor_y) {
  if (coverage.w <= 0 || coverage.h <= 0) return;
  std::vector<uint8_t> source = coverage.coverage;
  std::fill(coverage.coverage.begin(), coverage.coverage.end(), static_cast<uint8_t>(0));
  for (int32_t row = 0; row < coverage.h; ++row) {
    const int64_t absolute_y = static_cast<int64_t>(coverage.y) + row + anchor_y;
    const int64_t shift = static_cast<int64_t>(std::llround(-shear * static_cast<double>(absolute_y)));
    const size_t src_base = static_cast<size_t>(row) * static_cast<size_t>(coverage.w);
    for (int32_t column = 0; column < coverage.w; ++column) {
      const uint8_t value = source[src_base + static_cast<size_t>(column)];
      if (value == 0) continue;
      const int64_t target = static_cast<int64_t>(column) + shift;
      if (target < 0 || target >= coverage.w) continue;
      uint8_t& slot = coverage.coverage[src_base + static_cast<size_t>(target)];
      if (value > slot) slot = value;
    }
  }
}

/* 合成粗体：整幅向右平移 `step` 像素再按最大值叠回去（双描）。 */
void embolden_coverage(TypeCoverage& coverage, int32_t step) {
  if (step <= 0 || coverage.w <= 0 || coverage.h <= 0) return;
  std::vector<uint8_t> source = coverage.coverage;
  for (int32_t row = 0; row < coverage.h; ++row) {
    const size_t base = static_cast<size_t>(row) * static_cast<size_t>(coverage.w);
    for (int32_t column = 0; column + step < coverage.w; ++column) {
      const uint8_t value = source[base + static_cast<size_t>(column)];
      if (value == 0) continue;
      uint8_t& slot = coverage.coverage[base + static_cast<size_t>(column) + static_cast<size_t>(step)];
      if (value > slot) slot = value;
    }
  }
}

/* T14 后处理入口：两条渲染路径（点阵 / 字体轨道）都在最后调用它。
 *
 * 为什么必须抽出来：字体轨道有**自己的**字形绘制循环（type_rasterize_with_font），
 * 我第一版只在点阵路径的 return 前接了后处理，结果「用了随仓字体就完全没效果」——
 * 测试里 bold 前后像素逐字节相同。这正是「两条路径各写一份」的经典陷阱。 */
void apply_synthetic_style(TypeCoverage& coverage, const TypeRasterStyle& style,
                           double origin_y) {
  if (style.italic) {
    shear_coverage(coverage, 0.25, static_cast<int64_t>(std::llround(origin_y)));
  }
  if (style.bold) {
    /* 双描位移按字号取 1/12 em，至少 1 像素（太小看不出，太大变胖）。 */
    const int32_t step = std::max(1, static_cast<int32_t>(std::llround(style.font_size / 12.0)));
    embolden_coverage(coverage, step);
  }
  if (style.baseline_offset != 0.0 && coverage.h > 0 && coverage.w > 0) {
    const int32_t shift = static_cast<int32_t>(std::llround(style.baseline_offset));
    if (shift != 0) {
      std::vector<uint8_t> source = coverage.coverage;
      std::fill(coverage.coverage.begin(), coverage.coverage.end(), static_cast<uint8_t>(0));
      const size_t row_bytes = static_cast<size_t>(coverage.w);
      for (int32_t row = 0; row < coverage.h; ++row) {
        const int32_t target = row - shift;
        if (target < 0 || target >= coverage.h) continue;
        std::copy(source.begin() + static_cast<size_t>(row) * row_bytes,
                  source.begin() + static_cast<size_t>(row + 1) * row_bytes,
                  coverage.coverage.begin() + static_cast<size_t>(target) * row_bytes);
      }
    }
  }
}

}  // namespace

TypeRasterReport type_rasterize(const char* text, size_t size, const TypeRasterStyle& style,
                                double origin_x, double origin_y, int32_t layer_x, int32_t layer_y,
                                int32_t layer_width, int32_t layer_height, TypeCoverage* out) {
  if (out == nullptr) {
    return make_report(TypeRasterStatus::kBadStyle, "type coverage output is null");
  }
  *out = TypeCoverage{};
  TypeLayout layout;
  const TypeRasterReport report = build_layout(text, size, style, &layout);
  if (report.status != TypeRasterStatus::kOk) return report;
  if (!std::isfinite(origin_x) || !std::isfinite(origin_y)) {
    return make_report(TypeRasterStatus::kBadStyle, "type origin is not finite");
  }

  int64_t win_x0 = 0;
  int64_t win_y0 = 0;
  int64_t win_x1 = 0;
  int64_t win_y1 = 0;
  if (!clip_window(origin_x, origin_y, layout.measure.width, layout.measure.height, layer_x,
                   layer_y, layer_width, layer_height, &win_x0, &win_y0, &win_x1, &win_y1)) {
    return make_report(TypeRasterStatus::kEmpty, "type box does not intersect the layer");
  }
  out->x = static_cast<int32_t>(win_x0 - layer_x);
  out->y = static_cast<int32_t>(win_y0 - layer_y);
  out->w = static_cast<int32_t>(win_x1 - win_x0);
  out->h = static_cast<int32_t>(win_y1 - win_y0);
  out->coverage.assign(static_cast<size_t>(out->w) * static_cast<size_t>(out->h), 0);

  if (style.antialias) {
    rasterize_antialiased(layout, origin_x, origin_y, win_x0, win_y0, win_x1, win_y1, out);
  } else {
    rasterize_hard_edged(layout, origin_x, origin_y, win_x0, win_y0, win_x1, win_y1, out);
  }
  /* T14：粗体 / 斜体 / 基线偏移的后处理（三条全 0 时内部整段跳过）。 */
  apply_synthetic_style(*out, style, origin_y);
  return make_report(TypeRasterStatus::kOk, "");
}

TypeRasterReport type_measure_text_with_font(const char* text, size_t size,
                                             const TypeRasterStyle& style, FontFace* font,
                                             TypeMeasure* out) {
  FontLayout layout;
  const TypeRasterReport report = build_font_layout(text, size, style, font, &layout);
  if (out != nullptr) {
    *out = report.status == TypeRasterStatus::kOk ? layout.measure : TypeMeasure{};
  }
  return report;
}

TypeRasterReport type_rasterize_with_font(const char* text, size_t size,
                                          const TypeRasterStyle& style, FontFace* font,
                                          double origin_x, double origin_y, int32_t layer_x,
                                          int32_t layer_y, int32_t layer_width,
                                          int32_t layer_height, TypeCoverage* out,
                                          int64_t* out_missing_glyphs) {
  if (out_missing_glyphs != nullptr) *out_missing_glyphs = 0;
  if (out == nullptr) {
    return make_report(TypeRasterStatus::kBadStyle, "type coverage output is null");
  }
  *out = TypeCoverage{};

  FontLayout layout;
  const TypeRasterReport report = build_font_layout(text, size, style, font, &layout);
  if (report.status != TypeRasterStatus::kOk) return report;
  if (!std::isfinite(origin_x) || !std::isfinite(origin_y)) {
    return make_report(TypeRasterStatus::kBadStyle, "type origin is not finite");
  }

  /*
   * 字形会越过「步进箱」：斜体、负左侧距、重音都可能伸出字体盒。窗口四周各放
   * 一 em 余量再裁到图层，既不会漏画，也不会在正常字体上多占太多像素。
   */
  const double margin = style.font_size;
  int64_t win_x0 = 0;
  int64_t win_y0 = 0;
  int64_t win_x1 = 0;
  int64_t win_y1 = 0;
  if (!clip_window(origin_x - margin, origin_y - margin, layout.measure.width + 2.0 * margin,
                   layout.measure.height + 2.0 * margin, layer_x, layer_y, layer_width,
                   layer_height, &win_x0, &win_y0, &win_x1, &win_y1)) {
    return make_report(TypeRasterStatus::kEmpty, "type box does not intersect the layer");
  }
  out->x = static_cast<int32_t>(win_x0 - layer_x);
  out->y = static_cast<int32_t>(win_y0 - layer_y);
  out->w = static_cast<int32_t>(win_x1 - win_x0);
  out->h = static_cast<int32_t>(win_y1 - win_y0);
  out->coverage.assign(static_cast<size_t>(out->w) * static_cast<size_t>(out->h), 0);

  const bool antialias = style.antialias;
  int64_t missing = 0;
  for (size_t line = 0; line + 1 < layout.line_begin.size(); ++line) {
    const size_t begin = layout.line_begin[line];
    const size_t end = layout.line_begin[line + 1];
    /* 首行基线 = 文本框顶 + ascender；ascender 已按字号换算成文档像素。 */
    const double baseline =
        origin_y + layout.ascender + static_cast<double>(line) * layout.line_advance;
    double pen_x = origin_x;
    bool first = true;
    for (size_t i = begin; i < end; ++i) {
      const uint32_t codepoint = layout.codepoints[i];
      if (codepoint == '\n') continue;
      FontGlyph glyph;
      if (!font->render_glyph(codepoint, style.font_size, &glyph)) {
        *out = TypeCoverage{};
        return make_report(TypeRasterStatus::kFontError,
                           "the loaded font failed to rasterize a glyph");
      }
      if (!first) pen_x += layout.letter_spacing;
      if (glyph.missing) ++missing;

      if (!glyph.empty()) {
        /*
         * 位图是 FreeType 已栅格化好的像素网格；这里把字形原点取整落到整数像素
         * （不做亚像素插值）。字距/原点的小数部分因此会带来最多半像素的横向
         * 抖动，这是本批已知取舍，不为它引入二次重采样。
         */
        const int64_t glyph_x = std::llround(pen_x + static_cast<double>(glyph.bearing_x));
        const int64_t glyph_y = std::llround(baseline - static_cast<double>(glyph.bearing_y));
        for (int32_t row = 0; row < glyph.height; ++row) {
          const int64_t pixel_y = glyph_y + row;
          if (pixel_y < win_y0 || pixel_y >= win_y1) continue;
          uint8_t* target = out->coverage.data() +
                            static_cast<size_t>(pixel_y - win_y0) * static_cast<size_t>(out->w);
          for (int32_t column = 0; column < glyph.width; ++column) {
            const int64_t pixel_x = glyph_x + column;
            if (pixel_x < win_x0 || pixel_x >= win_x1) continue;
            uint8_t value =
                glyph.bitmap[static_cast<size_t>(row) * static_cast<size_t>(glyph.width) +
                             static_cast<size_t>(column)];
            if (!antialias) value = value >= 128 ? 255 : 0;
            if (value == 0) continue;
            uint8_t& slot = target[pixel_x - win_x0];
            if (value > slot) slot = value; /* 字形重叠取最大值，不叠亮度 */
          }
        }
      }
      pen_x += glyph.advance;
      first = false;
    }
  }

  if (out_missing_glyphs != nullptr) *out_missing_glyphs = missing;
  /* T14：字体轨道走自己的绘制循环，后处理必须在这里再调一次——
   * 只在点阵路径接会得到「换成随仓字体后粗体完全没反应」。 */
  apply_synthetic_style(*out, style, origin_y);
  return make_report(TypeRasterStatus::kOk, "");
}

/* ---------- 段落排版（P0-6 ③）：折行 + 对齐 ---------- */

namespace {

/* 宽字符（CJK / 全角 / Emoji）：这些文字没有词间空格，必须允许在字与字之间断行，
 * 否则一整句中文会被当成一个「不可断的词」横向冲出段落框。 */
bool is_wide_codepoint(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F000 && cp <= 0x1FAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFD);
}

/* 区间 [begin, end) 的步进宽度；字距只在字形之间加（与 measure 同一口径）。 */
double slice_width(const std::vector<double>& advances, double spacing, size_t begin,
                   size_t end) {
  double width = 0.0;
  size_t count = 0;
  for (size_t i = begin; i < end; ++i) {
    width += advances[i] + (count > 0 ? spacing : 0.0);
    ++count;
  }
  return width;
}

/*
 * 一段（两个 '\n' 之间）的贪心装行。
 *
 * 断点三档，优先级从高到低：
 *   1. 显式 '\n'（由调用方切开，到这里已经不存在）；
 *   2. 行内最后一个「可断位置」——空格之后、或宽字符边界；
 *   3. 没有任何可断位置（超长英文单词/URL）：在溢出点按字符硬断。
 * 每行至少前进一个字形，保证循环终止；行尾空格不计宽也不进渲染区间。
 */
void wrap_paragraph(const std::vector<uint32_t>& codepoints,
                    const std::vector<size_t>& byte_offsets,
                    const std::vector<double>& advances, double spacing, size_t paragraph_begin,
                    size_t paragraph_end, double box_width, size_t text_size,
                    std::vector<TypeWrapLine>* lines, double* content_width) {
  const size_t kNoBreak = static_cast<size_t>(-1);
  size_t i = paragraph_begin;
  while (i < paragraph_end) {
    while (i < paragraph_end && codepoints[i] == ' ') ++i; /* 行首空格不占位 */
    if (i >= paragraph_end) break;

    double width = 0.0;
    size_t count = 0;
    size_t j = i;
    size_t last_break = kNoBreak;
    while (j < paragraph_end) {
      if (count > 0 &&
          (codepoints[j - 1] == ' ' || is_wide_codepoint(codepoints[j]) ||
           is_wide_codepoint(codepoints[j - 1]))) {
        last_break = j;
      }
      const double add = advances[j] + (count > 0 ? spacing : 0.0);
      if (box_width > 0.0 && count > 0 && width + add > box_width) break;
      width += add;
      ++count;
      ++j;
    }

    size_t end = paragraph_end;
    if (j < paragraph_end) {
      end = last_break != kNoBreak ? last_break : j;
      /* 断点落在行首（整行放不下且没有可断位置）时按字符前进一格。 */
      if (end <= i) end = i + 1;
      if (end > paragraph_end) end = paragraph_end;
    }

    size_t trimmed = end;
    while (trimmed > i && codepoints[trimmed - 1] == ' ') --trimmed;
    TypeWrapLine line;
    line.byte_begin = byte_offsets[i];
    line.byte_end = trimmed < byte_offsets.size() ? byte_offsets[trimmed] : text_size;
    line.width = slice_width(advances, spacing, i, trimmed);
    lines->push_back(line);
    if (line.width > *content_width) *content_width = line.width;

    i = end;
  }
}

}  // namespace

TypeRasterReport type_layout_paragraph(const char* text, size_t size,
                                       const TypeRasterStyle& style, FontFace* font,
                                       double box_width, int32_t align,
                                       std::vector<TypeWrapLine>* out_lines,
                                       TypeParagraphMetrics* out_metrics) {
  if (out_lines == nullptr || out_metrics == nullptr) {
    return make_report(TypeRasterStatus::kBadStyle, "paragraph output is null");
  }
  *out_lines = {};
  *out_metrics = {};
  if (!std::isfinite(box_width) || box_width < 0.0 || box_width > kMaxTypeParagraphBox) {
    return make_report(TypeRasterStatus::kBadStyle, "paragraph box width is not finite or out of range");
  }
  if (align < kTypeAlignLeft || align >= kTypeAlignCount) {
    return make_report(TypeRasterStatus::kBadStyle, "paragraph alignment must be 0..2");
  }
  if (text == nullptr) return make_report(TypeRasterStatus::kBadText, "type text pointer is null");
  if (size == 0) return make_report(TypeRasterStatus::kBadText, "type text is empty");
  if (size > kMaxTypeCharacters) {
    return make_report(TypeRasterStatus::kTooLong, "type text exceeds 65536 characters");
  }

  std::vector<uint32_t> codepoints;
  std::vector<size_t> byte_offsets;
  std::vector<double> advances;
  const char* error = "";
  double spacing = 0.0;
  double first_line_height = 0.0;
  double line_advance = 0.0;
  int64_t missing = 0;

  if (font == nullptr) {
    /* 旧轨道：逐字节 ASCII，步进是常数（已含字距，spacing 必须为 0）。 */
    if (!scan_text(text, size, &error)) return make_report(TypeRasterStatus::kBadText, error);
    double scale = 1.0;
    double advance = 0.0;
    if (!validate_style(style, &scale, &advance, &error)) {
      return make_report(TypeRasterStatus::kBadStyle, error);
    }
    codepoints.resize(size);
    byte_offsets.resize(size);
    for (size_t i = 0; i < size; ++i) {
      codepoints[i] = static_cast<uint32_t>(static_cast<unsigned char>(text[i]));
      byte_offsets[i] = i;
    }
    advances.assign(size, advance);
    spacing = 0.0;
    first_line_height = style.font_size;
    line_advance = style.font_size * style.line_height;
  } else {
    if (!validate_font_style(style, &error)) {
      return make_report(TypeRasterStatus::kBadStyle, error);
    }
    if (!decode_utf8(text, size, &codepoints, &byte_offsets, &error)) {
      return make_report(TypeRasterStatus::kBadText, error);
    }
    const FontMetricsInfo& metrics = font->metrics();
    if (!metrics.scalable || metrics.units_per_em == 0) {
      return make_report(TypeRasterStatus::kFontError, "the loaded font is not scalable");
    }
    const double ascender = metrics.ascender * style.font_size;
    const double descender = metrics.descender * style.font_size;
    if (!(ascender - descender > 0.0)) {
      return make_report(TypeRasterStatus::kFontError,
                         "the loaded font has no usable vertical metrics");
    }
    first_line_height = ascender - descender;
    line_advance = style.font_size * style.line_height;
    spacing = style.letter_spacing;
    advances.assign(codepoints.size(), 0.0);
    int64_t glyphs = 0;
    for (size_t i = 0; i < codepoints.size(); ++i) {
      if (codepoints[i] == '\n') continue;
      FontGlyph glyph;
      if (!font->render_glyph(codepoints[i], style.font_size, &glyph)) {
        return make_report(TypeRasterStatus::kFontError,
                           "the loaded font failed to rasterize a glyph");
      }
      if (glyph.missing) ++missing;
      /*
       * 折行会让任意字形成为某一行**行首**（不吃字距），所以这里比
       * build_font_layout 多判一条「字形自身步进 >= 0.5」——否则带负字距的
       * 极小步进字形在排版阶段通过、到逐行渲染时被内核拒绝。
       */
      if (!(glyph.advance >= kMinTypeAdvance)) {
        return make_report(TypeRasterStatus::kBadStyle,
                           "type letter spacing makes the glyph advance smaller than 0.5 document px");
      }
      advances[i] = glyph.advance;
      ++glyphs;
    }
    if (glyphs == 0) {
      return make_report(TypeRasterStatus::kBadText, "type text has no printable glyphs");
    }
  }

  /*
   * 行内非首字形的步进底限（首字形不吃字距，已在两轨各自的样式闸里判过）：
   * 空格也算——步进被压成 0 时居中/右对齐会把整行算短，且渲染会叠字。
   */
  for (size_t i = 0; i < advances.size(); ++i) {
    if (codepoints[i] == '\n') continue;
    if (!(advances[i] + spacing >= kMinTypeAdvance)) {
      return make_report(TypeRasterStatus::kBadStyle,
                         "type letter spacing makes the glyph advance smaller than 0.5 document px");
    }
  }

  double content_width = 0.0;
  size_t paragraph_begin = 0;
  for (size_t i = 0; i <= codepoints.size(); ++i) {
    if (i != codepoints.size() && codepoints[i] != '\n') continue;
    const size_t paragraph_end = i;
    const size_t before = out_lines->size();
    wrap_paragraph(codepoints, byte_offsets, advances, spacing, paragraph_begin, paragraph_end,
                   box_width, size, out_lines, &content_width);
    if (out_lines->size() == before) {
      /* 空段落（连续换行 / 纯空格行）：保留一个占位的空行，行高照算。 */
      TypeWrapLine empty;
      const size_t byte = paragraph_begin < byte_offsets.size() ? byte_offsets[paragraph_begin] : size;
      empty.byte_begin = byte;
      empty.byte_end = byte;
      out_lines->push_back(empty);
    }
    paragraph_begin = i + 1;
  }

  const double block_width = box_width > 0.0 ? box_width : content_width;
  const double factor =
      align == kTypeAlignCenter ? 0.5 : (align == kTypeAlignRight ? 1.0 : 0.0);
  for (TypeWrapLine& line : *out_lines) {
    double offset = (block_width - line.width) * factor;
    line.x_offset = offset > 0.0 ? offset : 0.0;
  }

  out_metrics->line_count = static_cast<int32_t>(out_lines->size());
  out_metrics->line_advance = line_advance;
  out_metrics->content_width = content_width;
  out_metrics->content_height =
      first_line_height + static_cast<double>(out_metrics->line_count - 1) * line_advance;
  out_metrics->missing_glyphs = missing;
  return make_report(TypeRasterStatus::kOk, "");
}

}  // namespace compositor
