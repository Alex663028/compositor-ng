// AI生成
#include "domain/text_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "domain/font_face.h"
#include "domain/type_raster.h"

namespace compositor {
namespace {

/* 文字图层缓冲的像素数上限：与画布上限（16384²）同量级，防止一个误传的
 * 原点/字号把一次渲染变成几十 GB 的分配。超限如实报 kTooLarge。 */
constexpr int64_t kMaxTextLayerPixels = 268435456;

struct FontCacheKey {
  std::string path;
  int32_t face_index = 0;
  bool operator<(const FontCacheKey& other) const {
    if (path != other.path) return path < other.path;
    return face_index < other.face_index;
  }
};

std::mutex& font_cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<FontCacheKey, std::shared_ptr<FontFace>>& font_cache() {
  static std::map<FontCacheKey, std::shared_ptr<FontFace>> cache;
  return cache;
}

TextLayerReport make_report(TextLayerStatus status, std::string message) {
  TextLayerReport report;
  report.status = status;
  report.message = std::move(message);
  return report;
}

/* type_raster 的状态 -> 本模块状态（两者语义一一对应，只是错误面不同）。 */
TextLayerStatus map_raster_status(TypeRasterStatus status) {
  switch (status) {
    case TypeRasterStatus::kOk:
      return TextLayerStatus::kOk;
    case TypeRasterStatus::kBadText:
      return TextLayerStatus::kBadText;
    case TypeRasterStatus::kTooLong:
      return TextLayerStatus::kTooLarge;
    case TypeRasterStatus::kBadStyle:
      return TextLayerStatus::kBadStyle;
    case TypeRasterStatus::kEmpty:
      return TextLayerStatus::kOutOfCanvas;
    case TypeRasterStatus::kFontError:
      return TextLayerStatus::kFontMissing;
  }
  return TextLayerStatus::kInternal;
}

}  // namespace

void text_layer_clear_font_cache() {
  std::lock_guard<std::mutex> lock(font_cache_mutex());
  font_cache().clear();
}

std::shared_ptr<FontFace> text_layer_acquire_font(const std::string& path, int32_t face_index,
                                                  FontLoadStatus* out_status, std::string* error) {
  if (out_status != nullptr) *out_status = FontLoadStatus::kOk;
  if (path.empty()) {
    if (out_status != nullptr) *out_status = FontLoadStatus::kBadArgument;
    if (error != nullptr) *error = "font path is empty";
    return nullptr;
  }
  FontCacheKey key;
  key.path = path;
  key.face_index = face_index;
  {
    std::lock_guard<std::mutex> lock(font_cache_mutex());
    auto found = font_cache().find(key);
    if (found != font_cache().end()) return found->second;
  }
  /* 加载放在锁外：读 10 MB 字体不值得阻塞其它文字图层。并发首次加载同一份字体
   * 会各自建一个 FontFace，后到者覆盖缓存——重复一次加载是可接受的代价，
   * 换来的是不在锁内做 I/O。 */
  FontLoadStatus status = FontLoadStatus::kOk;
  std::string message;
  FontFace* raw = FontFace::load_file(path.c_str(), face_index, &status, &message);
  if (raw == nullptr) {
    if (out_status != nullptr) *out_status = status;
    if (error != nullptr) {
      *error = message.empty() ? "font could not be loaded" : message;
    }
    return nullptr;
  }
  std::shared_ptr<FontFace> face(raw);
  std::lock_guard<std::mutex> lock(font_cache_mutex());
  /* 并发下用先到的那份，保证同一个键在任何时刻只有一份被复用。 */
  auto inserted = font_cache().emplace(key, face);
  return inserted.first->second;
}

void text_layer_validate(const TextObject& text, TextLayerReport* report) {
  if (report == nullptr) return;
  *report = TextLayerReport{};
  if (text.text.empty()) {
    *report = make_report(TextLayerStatus::kBadArgument, "text is empty");
    return;
  }
  if (text.text.size() > kMaxTypeCharacters) {
    *report = make_report(TextLayerStatus::kTooLarge, "text exceeds the character limit");
    return;
  }
  if (!std::isfinite(text.origin_x) || !std::isfinite(text.origin_y) ||
      std::fabs(text.origin_x) > 1e6 || std::fabs(text.origin_y) > 1e6) {
    *report = make_report(TextLayerStatus::kBadArgument, "text origin is not finite or out of range");
    return;
  }
  if (!(text.opacity > 0.0f) || !(text.opacity <= 1.0f)) {
    *report = make_report(TextLayerStatus::kBadArgument, "opacity must be in (0, 1]");
    return;
  }
  if (!std::isfinite(text.box_width) || !std::isfinite(text.box_height) ||
      text.box_width < 0.0 || text.box_height < 0.0 ||
      text.box_width > kMaxTypeParagraphBox || text.box_height > kMaxTypeParagraphBox) {
    *report = make_report(TextLayerStatus::kBadArgument,
                          "paragraph box is not finite or out of range");
    return;
  }
  if (text.align < kTypeAlignLeft || text.align >= kTypeAlignCount) {
    *report = make_report(TextLayerStatus::kBadArgument, "paragraph alignment must be 0..2");
    return;
  }
  if (text.color[3] == 0) {
    *report = make_report(TextLayerStatus::kBadArgument, "text color is fully transparent");
    return;
  }
  if (text.font_face_index < 0) {
    *report = make_report(TextLayerStatus::kBadArgument, "font face index must be >= 0");
    return;
  }
  TypeRasterStyle style;
  style.font_size = text.style.font_size;
  style.line_height = text.style.line_height;
  style.letter_spacing = text.style.letter_spacing;
  style.antialias = text.style.antialias;
  /* 文本与样式的参数闸完全复用 type_raster 的判定（两轨同一把尺子），
   * 这里只借 measure 拿错误码，不关心量出来的宽高。 */
  /*
   * 旧轨道（内置 5×7）在这一步就能判死文本/样式；字体轨道不加载字体，
   * 由 render 在真正拿到 FontFace 之后再判（字体打不开是 kFontMissing，
   * 不是参数错误，两者不能混成一个错误码）。
   */
  if (text.font_path.empty()) {
    TypeMeasure measure;
    const TypeRasterReport raster_report =
        type_measure_text(text.text.c_str(), text.text.size(), style, &measure);
    if (raster_report.status != TypeRasterStatus::kOk) {
      *report = make_report(map_raster_status(raster_report.status),
                            raster_report.message != nullptr ? raster_report.message : "");
    }
  }
}

std::string text_layer_default_name(const std::string& text) {
  size_t begin = 0;
  size_t end = text.find('\n');
  if (end == std::string::npos) end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    --end;
  }
  if (begin == end) return "文字图层";
  /* 最多 32 字节，且不切断 UTF-8 续字节（0b10xxxxxx）。 */
  size_t length = std::min<size_t>(end - begin, 32);
  while (length > 0 && (static_cast<unsigned char>(text[begin + length]) & 0xC0u) == 0x80u) {
    --length;
  }
  if (length == 0) length = std::min<size_t>(end - begin, 32);
  return text.substr(begin, length);
}

TextLayerReport text_layer_render(const TextObject& text, int32_t canvas_width,
                                  int32_t canvas_height, ImageBuffer* out,
                                  comp_rect* out_bounds, int64_t* out_missing_glyphs) {
  if (out_missing_glyphs != nullptr) *out_missing_glyphs = 0;
  if (out == nullptr || out_bounds == nullptr) {
    return make_report(TextLayerStatus::kBadArgument, "output buffer or bounds is null");
  }
  if (canvas_width <= 0 || canvas_height <= 0) {
    return make_report(TextLayerStatus::kBadArgument, "canvas size must be positive");
  }

  TextLayerReport report;
  text_layer_validate(text, &report);
  if (report.status != TextLayerStatus::kOk) return report;

  std::shared_ptr<FontFace> font;
  if (!text.font_path.empty()) {
    std::string font_error;
    FontLoadStatus font_status = FontLoadStatus::kOk;
    font = text_layer_acquire_font(text.font_path, text.font_face_index, &font_status,
                                   &font_error);
    if (font == nullptr) {
      TextLayerReport missing = make_report(
          TextLayerStatus::kFontMissing,
          font_error.empty() ? "font could not be loaded" : font_error);
      missing.font_status = font_status;
      return missing;
    }
  }

  TypeRasterStyle style;
  style.font_size = text.style.font_size;
  style.line_height = text.style.line_height;
  style.letter_spacing = text.style.letter_spacing;
  style.antialias = text.style.antialias;

  /*
   * 段落模式（P0-6 ③）：三个字段任一非默认就按段落走；全默认时下面的代码与
   * P0-6 之前的单次栅格化**逐字节相同**（不折行、逐行合成不进入老路径）。
   */
  const bool paragraph_mode =
      text.box_width > 0.0 || text.box_height > 0.0 || text.align != kTypeAlignLeft;

  TypeMeasure measure;
  TypeParagraphMetrics paragraph;
  std::vector<TypeWrapLine> paragraph_lines;
  if (paragraph_mode) {
    const TypeRasterReport laid =
        type_layout_paragraph(text.text.c_str(), text.text.size(), style, font.get(),
                              text.box_width, text.align, &paragraph_lines, &paragraph);
    if (laid.status != TypeRasterStatus::kOk) {
      return make_report(map_raster_status(laid.status),
                         laid.message != nullptr ? laid.message : "");
    }
  } else {
    const TypeRasterReport measured =
        font != nullptr
            ? type_measure_text_with_font(text.text.c_str(), text.text.size(), style, font.get(),
                                          &measure)
            : type_measure_text(text.text.c_str(), text.text.size(), style, &measure);
    if (measured.status != TypeRasterStatus::kOk) {
      return make_report(map_raster_status(measured.status),
                         measured.message != nullptr ? measured.message : "");
    }
  }

  const double content_width = paragraph_mode ? paragraph.content_width : measure.width;
  const double content_height = paragraph_mode ? paragraph.content_height : measure.height;
  /* 框宽/框高为 0 的那一维退回内容尺寸（= 点文字）；两维都给才是完整段落框。 */
  const double box_width = text.box_width > 0.0 ? text.box_width : content_width;
  const double box_height = text.box_height > 0.0 ? text.box_height : content_height;

  /*
   * 文本框 -> 图层矩形。原点取 floor，亚像素部分留给栅格化内核（它把 origin
   * 减掉图层原点后按小数落位）——先把原点四舍五入会丢掉半个像素的字形偏移，
   * 与工具算子直接画进图层时的落墨位置就不再一致。
   *
   * 注意 comp_rect 是 int32：这里全是 double，先算完再转，避免 1e6 量级坐标
   * 在 int 上溢出。
   */
  const double box_left = text.origin_x;
  const double box_top = text.origin_y;
  const double box_right = box_left + box_width;
  const double box_bottom = box_top + box_height;
  const int64_t left = static_cast<int64_t>(std::floor(box_left));
  const int64_t top = static_cast<int64_t>(std::floor(box_top));
  const int64_t right = static_cast<int64_t>(std::ceil(box_right));
  const int64_t bottom = static_cast<int64_t>(std::ceil(box_bottom));
  const int64_t width = right - left;
  const int64_t height = bottom - top;
  if (width <= 0 || height <= 0) {
    return make_report(TextLayerStatus::kTooLarge, "text box has an empty extent");
  }
  if (width > 2147483647LL || height > 2147483647LL ||
      width * height > kMaxTextLayerPixels) {
    return make_report(TextLayerStatus::kTooLarge, "text box is too large");
  }
  if (right <= 0 || bottom <= 0 || left >= canvas_width || top >= canvas_height) {
    return make_report(TextLayerStatus::kOutOfCanvas, "text box does not intersect the canvas");
  }

  ImageBuffer buffer;
  buffer.resize(static_cast<int32_t>(width), static_cast<int32_t>(height), COMP_PIXEL_RGBA8);
  if (buffer.empty()) {
    return make_report(TextLayerStatus::kInternal, "failed to allocate the text layer buffer");
  }
  buffer.clear_transparent();

  TypeCoverage coverage;
  int64_t missing = paragraph_mode ? paragraph.missing_glyphs : 0;
  if (!paragraph_mode) {
    const TypeRasterReport rastered =
        font != nullptr
            ? type_rasterize_with_font(text.text.c_str(), text.text.size(), style, font.get(),
                                       text.origin_x - static_cast<double>(left),
                                       text.origin_y - static_cast<double>(top), 0, 0,
                                       buffer.width, buffer.height, &coverage, &missing)
            : type_rasterize(text.text.c_str(), text.text.size(), style,
                             text.origin_x - static_cast<double>(left),
                             text.origin_y - static_cast<double>(top), 0, 0, buffer.width,
                             buffer.height, &coverage);
    if (rastered.status != TypeRasterStatus::kOk) {
      return make_report(map_raster_status(rastered.status),
                         rastered.message != nullptr ? rastered.message : "");
    }
  } else {
    /*
     * 段落：逐视觉行栅格化，再按 max 合并到整层窗口。
     *
     * 为什么逐行调内核而不是改内核：每行的落点由折行结果决定（x_offset +
     * 行序 × line_advance），内核的「一次排版一整段」不知道折行；逐行调用
     * 复用的是同一条已验证的排版/栅格化路径，行与行只在像素上合成。max
     * 合并与「字形重叠取最大覆盖率」同口径，行距 < 1 时也不会叠亮。
     *
     * 框高/框宽就是 buffer 尺寸，超出的行由内核的窗口裁剪自然掉——不需要
     * 再写一段裁切逻辑。
     */
    std::vector<uint8_t> merged(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    for (size_t line = 0; line < paragraph_lines.size(); ++line) {
      const TypeWrapLine& wrap = paragraph_lines[line];
      if (wrap.byte_end <= wrap.byte_begin) continue; /* 空行不落墨 */
      const std::string slice =
          text.text.substr(wrap.byte_begin, wrap.byte_end - wrap.byte_begin);
      const double line_origin_x = text.origin_x + wrap.x_offset - static_cast<double>(left);
      const double line_origin_y =
          text.origin_y + static_cast<double>(line) * paragraph.line_advance -
          static_cast<double>(top);
      TypeCoverage line_coverage;
      int64_t line_missing = 0;
      const TypeRasterReport line_report =
          font != nullptr
              ? type_rasterize_with_font(slice.c_str(), slice.size(), style, font.get(),
                                         line_origin_x, line_origin_y, 0, 0, buffer.width,
                                         buffer.height, &line_coverage, &line_missing)
              : type_rasterize(slice.c_str(), slice.size(), style, line_origin_x, line_origin_y,
                               0, 0, buffer.width, buffer.height, &line_coverage);
      if (line_report.status == TypeRasterStatus::kEmpty) continue;
      if (line_report.status != TypeRasterStatus::kOk) {
        return make_report(map_raster_status(line_report.status),
                           line_report.message != nullptr ? line_report.message : "");
      }
      for (int32_t y = 0; y < line_coverage.h; ++y) {
        const int32_t local_y = line_coverage.y + y;
        if (local_y < 0 || local_y >= buffer.height) continue;
        uint8_t* target = merged.data() + static_cast<size_t>(local_y) * static_cast<size_t>(width);
        const uint8_t* source = line_coverage.coverage.data() +
                                static_cast<size_t>(y) * static_cast<size_t>(line_coverage.w);
        for (int32_t x = 0; x < line_coverage.w; ++x) {
          const int32_t local_x = line_coverage.x + x;
          if (local_x < 0 || local_x >= buffer.width) continue;
          uint8_t& slot = target[local_x];
          if (source[x] > slot) slot = source[x];
        }
      }
    }
    coverage.x = 0;
    coverage.y = 0;
    coverage.w = static_cast<int32_t>(width);
    coverage.h = static_cast<int32_t>(height);
    coverage.coverage = std::move(merged);
  }
  if (coverage.empty()) {
    /* 空文本框（纯空格）：成功但没有任何墨迹。图层保留为全透明，
     * 与「文本框与图层不相交」区分开——这是一个合法但看不见的文字图层。 */
    *out = std::move(buffer);
    out_bounds->x = static_cast<int32_t>(left);
    out_bounds->y = static_cast<int32_t>(top);
    out_bounds->w = static_cast<int32_t>(width);
    out_bounds->h = static_cast<int32_t>(height);
    if (out_missing_glyphs != nullptr) *out_missing_glyphs = missing;
    return TextLayerReport{};
  }

  /*
   * 覆盖率 -> 预乘 RGBA8。公式与 domain/tool_ops.cpp 的文字落墨逐字节同口径
   * （alpha = coverage/255 × color.a/255 × opacity，颜色先乘 alpha 再写），
   * 只是这里的目标缓冲初始全透明，source-over 退化成直接写入。
   */
  const double opacity = static_cast<double>(text.opacity);
  const size_t stride_bytes = buffer.stride();
  uint8_t* pixels = buffer.writable_pixels().data();
  for (int32_t y = 0; y < coverage.h; ++y) {
    const int32_t local_y = coverage.y + y;
    if (local_y < 0 || local_y >= buffer.height) continue;
    uint8_t* row = pixels + static_cast<size_t>(local_y) * stride_bytes;
    for (int32_t x = 0; x < coverage.w; ++x) {
      const uint8_t cover =
          coverage.coverage[static_cast<size_t>(y) * static_cast<size_t>(coverage.w) +
                            static_cast<size_t>(x)];
      if (cover == 0) continue;
      const int32_t local_x = coverage.x + x;
      if (local_x < 0 || local_x >= buffer.width) continue;
      const double alpha =
          (static_cast<double>(cover) / 255.0) * (static_cast<double>(text.color[3]) / 255.0) *
          opacity;
      if (alpha <= 0.0) continue;
      uint8_t* pixel = row + static_cast<size_t>(local_x) * 4u;
      for (int c = 0; c < 3; ++c) {
        const long value = std::lround(static_cast<double>(text.color[c]) * alpha);
        pixel[c] = static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
      }
      const long alpha_value = std::lround(alpha * 255.0);
      pixel[3] = static_cast<uint8_t>(alpha_value < 0 ? 0 : (alpha_value > 255 ? 255 : alpha_value));
    }
  }

  if (out_missing_glyphs != nullptr) *out_missing_glyphs = missing;
  *out = std::move(buffer);
  out_bounds->x = static_cast<int32_t>(left);
  out_bounds->y = static_cast<int32_t>(top);
  out_bounds->w = static_cast<int32_t>(width);
  out_bounds->h = static_cast<int32_t>(height);
  return TextLayerReport{};
}

}  // namespace compositor
