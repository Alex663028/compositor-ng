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
  return make_report(TypeRasterStatus::kOk, "");
}

}  // namespace compositor
