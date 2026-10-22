// AI生成
/*
 * test_font.cpp — 字体加载（P5-1）与字体轨道文字栅格化（P5-2）的领域 / ABI 契约。
 *
 * P5-1 覆盖：
 *   - 随仓 subset 字体能从文件 / 内存两条入口加载，身份名与度量符合资源清单；
 *   - 内存入口**拷贝**字节（宿主缓冲销毁后字体仍可用）；
 *   - 空路径 / 负面序号 / 文件不存在 / 非字体字节 / 越界面序号各自的失败码；
 *   - 度量字段自洽（height = ascender - descender + line_gap）与中文门槛 has_cjk；
 *   - ABI 句柄的加载 / 取名 / 度量 / 释放，以及错误码映射（IO / DECODE / ARG）。
 *
 * P5-2 覆盖（字体轨道，`*_with_font`）：
 *   - 「你好，世界」@48px 的度量（行数 / 字形数 / 缺字 / 宽度 / em 高忽略行距）；
 *   - 同一文本栅格化出真实覆盖率窗口（有实心像素、有真实墨量）；
 *   - 缺字（U+0378，Unicode 未分配码位）：计入 missing_glyphs、只占步进、不画方框；
 *   - 非法 UTF-8 / 控制字符按 kBadText 拒绝，字体为空按 kFontError 拒绝；
 *   - ABI：`comp_tool_op_desc::type_font` 走字体轨道落像素、一步撤销、note 如实。
 *
 * 旧 5×7 轨道的逐字节断言在 tests/test_tool_ops.cpp，本文件不碰它。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_font.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"
#include "domain/font_face.h"
#include "domain/type_raster.h"
#include "test_framework.h"

namespace {

using compositor::FontFace;
using compositor::FontLoadStatus;
using compositor::FontMetricsInfo;
using compositor::TypeCoverage;
using compositor::TypeMeasure;
using compositor::TypeRasterStatus;
using compositor::TypeRasterStyle;

const char* kFontPath = "resources/fonts/NotoSansSC-Regular-Subset.ttf";

std::vector<uint8_t> read_font_bytes() {
  std::ifstream input(kFontPath, std::ios::binary);
  if (!input) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

bool close_enough(double a, double b, double epsilon = 1e-9) {
  return std::fabs(a - b) <= epsilon;
}

}  // namespace

COMP_TEST(font_load_file_reports_identity_and_metrics) {
  FontLoadStatus status = FontLoadStatus::kInternal;
  std::string message = "placeholder";
  FontFace* font = FontFace::load_file(kFontPath, 0, &status, &message);
  COMP_CHECK_MSG(font != nullptr, message.c_str());
  if (font == nullptr) return;

  COMP_CHECK(status == FontLoadStatus::kOk);
  COMP_CHECK_MSG(message.empty(), "成功时 message 应为空");
  COMP_CHECK(font->is_alive());

  /* 身份名由 make_font_subset.py 写入，见 resources/fonts/README.md。 */
  COMP_CHECK(font->family_name() == "Noto Sans SC Subset");
  COMP_CHECK(font->style_name() == "Regular");
  COMP_CHECK(font->postscript_name() == "NotoSansSCSubset-Regular");

  const FontMetricsInfo& metrics = font->metrics();
  COMP_CHECK(metrics.face_index == 0);
  COMP_CHECK(metrics.face_count == 1);
  COMP_CHECK_MSG(metrics.glyph_count == 4491, "字形数应与资源清单一致");
  COMP_CHECK(metrics.units_per_em == 1000u);
  COMP_CHECK(metrics.scalable);
  COMP_CHECK_MSG(metrics.has_cjk, "子集包含「你」，has_cjk 必须为真");

  COMP_CHECK(metrics.ascender > 0.7 && metrics.ascender < 1.4);
  COMP_CHECK_MSG(metrics.descender < 0.0, "descender 按 FreeType 口径为负");
  COMP_CHECK(metrics.height > 1.0);
  COMP_CHECK(metrics.max_advance > 0.0);
  COMP_CHECK(close_enough(metrics.height, metrics.ascender - metrics.descender + metrics.line_gap));

  delete font;
}

COMP_TEST(font_load_memory_copies_bytes) {
  std::vector<uint8_t> bytes = read_font_bytes();
  COMP_CHECK_MSG(!bytes.empty(), "仓库应随带 resources/fonts 下的 subset 字体");

  FontLoadStatus status = FontLoadStatus::kInternal;
  std::string message;
  FontFace* font = FontFace::load_memory(bytes.data(), bytes.size(), 0, &status, &message);
  COMP_CHECK_MSG(font != nullptr, message.c_str());
  if (font == nullptr) return;
  COMP_CHECK(status == FontLoadStatus::kOk);

  /* 引擎拷贝了字节：把宿主缓冲涂掉再读字体，名字与度量必须照旧。 */
  std::fill(bytes.begin(), bytes.end(), 0);
  bytes.clear();
  bytes.shrink_to_fit();

  COMP_CHECK(font->family_name() == "Noto Sans SC Subset");
  COMP_CHECK(font->metrics().units_per_em == 1000u);
  COMP_CHECK(font->metrics().has_cjk);
  delete font;
}

COMP_TEST(font_rejects_bad_input) {
  FontLoadStatus status = FontLoadStatus::kOk;
  std::string message;

  COMP_CHECK_MSG(FontFace::load_file(nullptr, 0, &status, &message) == nullptr,
                 "空路径必须失败");
  COMP_CHECK(status == FontLoadStatus::kBadArgument);

  COMP_CHECK_MSG(FontFace::load_file("", 0, &status, &message) == nullptr, "空串路径必须失败");
  COMP_CHECK(status == FontLoadStatus::kBadArgument);

  COMP_CHECK_MSG(FontFace::load_file(kFontPath, -1, &status, &message) == nullptr,
                 "负面序号必须失败");
  COMP_CHECK(status == FontLoadStatus::kBadArgument);

  COMP_CHECK_MSG(FontFace::load_file("resources/fonts/not-there-20261022.ttf", 0, &status,
                                     &message) == nullptr,
                 "不存在的文件必须失败");
  COMP_CHECK_MSG(status == FontLoadStatus::kNotFound, "缺文件应报 kNotFound");

  /* 64 字节 0：不是任何字体格式。 */
  const uint8_t junk[64] = {0};
  COMP_CHECK_MSG(FontFace::load_memory(junk, sizeof(junk), 0, &status, &message) == nullptr,
                 "非字体字节必须失败");
  COMP_CHECK_MSG(status == FontLoadStatus::kNotAFont, "非字体应报 kNotAFont");

  COMP_CHECK_MSG(FontFace::load_memory(nullptr, 0, 0, &status, &message) == nullptr,
                 "空数据必须失败");
  COMP_CHECK(status == FontLoadStatus::kBadArgument);

  /* 单面字体没有第 7 面。 */
  COMP_CHECK_MSG(FontFace::load_file(kFontPath, 7, &status, &message) == nullptr,
                 "越界面序号必须失败");
  COMP_CHECK_MSG(status == FontLoadStatus::kBadFaceIndex, "越界面序号应报 kBadFaceIndex");
}

COMP_TEST(comp_font_abi_surfaces_handles_and_errors) {
  comp_font font = nullptr;
  COMP_CHECK(comp_font_load_file(kFontPath, 0, &font) == COMP_OK);
  COMP_CHECK_MSG(font != nullptr, "成功时句柄必须非空");
  if (font == nullptr) return;

  const char* family = comp_font_name(font, COMP_FONT_NAME_FAMILY);
  COMP_CHECK(family != nullptr && std::string(family) == "Noto Sans SC Subset");
  const char* style = comp_font_name(font, COMP_FONT_NAME_STYLE);
  COMP_CHECK(style != nullptr && std::string(style) == "Regular");
  const char* postscript = comp_font_name(font, COMP_FONT_NAME_POSTSCRIPT);
  COMP_CHECK(postscript != nullptr && std::string(postscript) == "NotoSansSCSubset-Regular");
  COMP_CHECK_MSG(comp_font_name(font, static_cast<comp_font_name_kind>(99)) == nullptr,
                 "未知 name kind 应返回 NULL");

  comp_font_metrics metrics{};
  COMP_CHECK(comp_font_get_metrics(font, &metrics) == COMP_OK);
  COMP_CHECK(metrics.face_index == 0);
  COMP_CHECK(metrics.face_count == 1);
  COMP_CHECK(metrics.glyph_count == 4491);
  COMP_CHECK(metrics.units_per_em == 1000u);
  COMP_CHECK_MSG(metrics.scalable == 1, "ABI 用 int32 表达 bool");
  COMP_CHECK_MSG(metrics.has_cjk == 1, "子集字体应能排中文");
  COMP_CHECK(metrics.ascender > 0.7);
  COMP_CHECK(metrics.descender < 0.0);

  /* 参数校验。 */
  COMP_CHECK(comp_font_get_metrics(font, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_get_metrics(nullptr, &metrics) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_name(nullptr, COMP_FONT_NAME_FAMILY) == nullptr);
  comp_font_release(nullptr); /* 空操作，不崩 */

  COMP_CHECK(comp_font_load_file(kFontPath, 0, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  comp_font unused = nullptr;
  COMP_CHECK(comp_font_load_file(nullptr, 0, &unused) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_load_file("", 0, &unused) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_load_file("resources/fonts/not-there-20261022.ttf", 0, &unused) ==
             COMP_ERR_IO);
  COMP_CHECK(comp_font_load_file(kFontPath, 7, &unused) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_load_memory(nullptr, 0, 0, &unused) == COMP_ERR_INVALID_ARGUMENT);

  const uint8_t junk[64] = {0};
  COMP_CHECK(comp_font_load_memory(junk, sizeof(junk), 0, &unused) == COMP_ERR_DECODE);

  comp_font_release(font);
}

/* ==================================================== 字体轨道（P5-2） */

namespace {

/* 「你好，世界」的 UTF-8 字节（15 字节）。 */
const char* kCjkText = "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C";
constexpr size_t kCjkTextSize = 15;

/* U+0378 是 Unicode 未分配码位：任何真实字体都不会有它的字形。 */
const char* kUnassigned = "\xCD\xB8";

FontFace* load_test_font() {
  FontLoadStatus status = FontLoadStatus::kInternal;
  std::string message;
  FontFace* font = FontFace::load_file(kFontPath, 0, &status, &message);
  COMP_CHECK_MSG(font != nullptr, message.c_str());
  return font;
}

size_t ink_pixels(const TypeCoverage& coverage, uint8_t threshold) {
  size_t count = 0;
  for (uint8_t value : coverage.coverage) {
    if (value >= threshold) ++count;
  }
  return count;
}

}  // namespace

COMP_TEST(type_measure_with_font_lays_out_cjk_in_em_units) {
  FontFace* font = load_test_font();
  if (font == nullptr) return;

  TypeRasterStyle style; /* font_size = 32 默认，这里钉 48 px em 高 */
  style.font_size = 48.0;
  style.line_height = 1.25;

  TypeMeasure measure;
  const compositor::TypeRasterReport report =
      compositor::type_measure_text_with_font(kCjkText, kCjkTextSize, style, font, &measure);
  COMP_CHECK_MSG(report.status == TypeRasterStatus::kOk, report.message);
  COMP_CHECK_MSG(measure.line_count == 1, "one line of text");
  COMP_CHECK_MSG(measure.glyph_count == 5, "你 好 ， 世 界 = 5 glyphs ('\\n' not counted)");
  COMP_CHECK_MSG(measure.missing_glyphs == 0, "the subset font has all five glyphs");
  COMP_CHECK_MSG(close_enough(measure.advance, 0.0),
                 "the font track has no single glyph advance, unlike the 5x7 bitmap font");

  /* 五个全角字形（含全角逗号）≈ 5 em；只钉一个宽区间，不钉具体字形。 */
  const double em = style.font_size;
  COMP_CHECK_MSG(measure.width > 4.0 * em && measure.width < 6.5 * em,
                 "the text box width is roughly five em, not five 5x7 cells");

  /* 单行高度 = (ascender - descender) × em 高；行距不参与单行高度。 */
  const FontMetricsInfo& metrics = font->metrics();
  const double expected_height = (metrics.ascender - metrics.descender) * em;
  COMP_CHECK_MSG(close_enough(measure.height, expected_height, 1e-9),
                 "single-line height is the em-scaled ascender-descender span");

  /* 两行：行距 1.0 ⇒ 高度 = 行高 + 1 × em。 */
  TypeRasterStyle two_lines = style;
  two_lines.line_height = 1.0;
  TypeMeasure multiline;
  const compositor::TypeRasterReport two_report = compositor::type_measure_text_with_font(
      "\xE4\xBD\xA0\xE5\xA5\xBD\n\xE4\xB8\x96\xE7\x95\x8C", 13, two_lines, font, &multiline);
  COMP_CHECK(two_report.status == TypeRasterStatus::kOk);
  COMP_CHECK(multiline.line_count == 2);
  COMP_CHECK(multiline.glyph_count == 4);
  COMP_CHECK_MSG(close_enough(multiline.height, expected_height + em, 1e-9),
                 "the second line adds exactly one line advance");

  delete font;
}

COMP_TEST(type_rasterize_with_font_paints_real_coverage) {
  FontFace* font = load_test_font();
  if (font == nullptr) return;

  TypeRasterStyle style;
  style.font_size = 48.0;
  style.antialias = true;

  TypeCoverage coverage;
  int64_t missing = -1;
  const compositor::TypeRasterReport report = compositor::type_rasterize_with_font(
      kCjkText, kCjkTextSize, style, font, 0.0, 0.0, 0, 0, 256, 128, &coverage, &missing);
  COMP_CHECK_MSG(report.status == TypeRasterStatus::kOk, report.message);
  COMP_CHECK_MSG(!coverage.empty(), "a 48 px CJK line must produce a coverage window");
  COMP_CHECK_MSG(coverage.x == 0 && coverage.y == 0, "the box starts at the layer origin");
  COMP_CHECK_MSG(missing == 0, "no missing glyphs were reported while rasterizing");

  /* 真墨量：48 px 的五个汉字远不止几百个像素；这里刻意不钉精确值。 */
  COMP_CHECK_MSG(ink_pixels(coverage, 1) > 1500, "the coverage window contains real ink");
  COMP_CHECK_MSG(ink_pixels(coverage, 200) > 800, "CJK strokes reach near-solid coverage");
  uint8_t peak = 0;
  for (uint8_t value : coverage.coverage) peak = std::max(peak, value);
  COMP_CHECK_MSG(peak >= 250, "at least one pixel is (nearly) fully covered");

  /* 硬边：阈值 128，覆盖率只剩 0 / 255 两档。 */
  TypeCoverage hard;
  int64_t hard_missing = -1;
  TypeRasterStyle hard_style = style;
  hard_style.antialias = false;
  const compositor::TypeRasterReport hard_report = compositor::type_rasterize_with_font(
      kCjkText, kCjkTextSize, hard_style, font, 0.0, 0.0, 0, 0, 256, 128, &hard, &hard_missing);
  COMP_CHECK(hard_report.status == TypeRasterStatus::kOk);
  bool only_two_levels = true;
  for (uint8_t value : hard.coverage) {
    if (value != 0 && value != 255) only_two_levels = false;
  }
  COMP_CHECK_MSG(only_two_levels, "antialias=0 keeps only 0 and 255");

  /* 文本框完全落在图层外：成功但无像素要改（kEmpty）。 */
  TypeCoverage outside;
  int64_t outside_missing = -1;
  const compositor::TypeRasterReport outside_report = compositor::type_rasterize_with_font(
      kCjkText, kCjkTextSize, style, font, 4096.0, 4096.0, 0, 0, 256, 128, &outside,
      &outside_missing);
  COMP_CHECK_MSG(outside_report.status == TypeRasterStatus::kEmpty,
                 "a box outside the layer is a successful empty raster");
  COMP_CHECK_MSG(outside.empty(), "kEmpty clears the coverage window");
  COMP_CHECK(outside_missing == 0);

  delete font;
}

COMP_TEST(type_font_track_reports_missing_glyphs_without_painting_boxes) {
  FontFace* font = load_test_font();
  if (font == nullptr) return;

  /* 前提：U+0378 在这个子集里确实没有字形（否则下面的断言没有意义）。 */
  compositor::FontGlyph probe;
  COMP_CHECK_MSG(font->render_glyph(0x0378u, 48.0, &probe), "probing an unassigned codepoint");
  COMP_CHECK_MSG(probe.missing, "U+0378 must be missing from the subset font");
  COMP_CHECK_MSG(probe.empty(), "a missing glyph has no bitmap (no .notdef box)");
  COMP_CHECK_MSG(probe.advance > 0.0, "a missing glyph still advances the pen");

  TypeRasterStyle style;
  style.font_size = 48.0;

  TypeMeasure measure;
  const compositor::TypeRasterReport measure_report =
      compositor::type_measure_text_with_font(kUnassigned, 2, style, font, &measure);
  COMP_CHECK(measure_report.status == TypeRasterStatus::kOk);
  COMP_CHECK_MSG(measure.glyph_count == 1, "a missing codepoint still counts as one glyph");
  COMP_CHECK_MSG(measure.missing_glyphs == 1, "the missing glyph is reported honestly");
  COMP_CHECK_MSG(measure.width > 0.0, "the missing glyph still occupies an advance");

  TypeCoverage coverage;
  int64_t missing = -1;
  const compositor::TypeRasterReport raster_report = compositor::type_rasterize_with_font(
      kUnassigned, 2, style, font, 0.0, 0.0, 0, 0, 256, 128, &coverage, &missing);
  COMP_CHECK(raster_report.status == TypeRasterStatus::kOk);
  COMP_CHECK_MSG(missing == 1, "rasterization reports the same missing count");
  COMP_CHECK_MSG(ink_pixels(coverage, 1) == 0,
                 "a missing glyph paints nothing rather than drawing a box");

  delete font;
}

COMP_TEST(type_font_track_rejects_bad_text_and_missing_font) {
  FontFace* font = load_test_font();
  if (font == nullptr) return;

  TypeRasterStyle style;
  style.font_size = 48.0;
  TypeMeasure measure;

  /* 字体为空：kFontError（宿主态错误），绝不静默回落到 5×7。 */
  const compositor::TypeRasterReport no_font =
      compositor::type_measure_text_with_font("A", 1, style, nullptr, &measure);
  COMP_CHECK_MSG(no_font.status == TypeRasterStatus::kFontError,
                 "a null font handle is a state error, not a bad argument");
  COMP_CHECK(measure.glyph_count == 0 && measure.line_count == 0);

  /* 截断的 UTF-8 序列（「你」少一个续字节）。 */
  const compositor::TypeRasterReport truncated =
      compositor::type_measure_text_with_font("\xE4\xBD", 2, style, font, &measure);
  COMP_CHECK_MSG(truncated.status == TypeRasterStatus::kBadText, "a truncated UTF-8 sequence is rejected");

  /* 孤立续字节。 */
  const compositor::TypeRasterReport lone =
      compositor::type_measure_text_with_font("\xA0", 1, style, font, &measure);
  COMP_CHECK(lone.status == TypeRasterStatus::kBadText);

  /* 过长编码（C0 AF = U+002F 的 U+0000 之外的两字节写法）。 */
  const compositor::TypeRasterReport overlong =
      compositor::type_measure_text_with_font("\xC0\xAF", 2, style, font, &measure);
  COMP_CHECK(overlong.status == TypeRasterStatus::kBadText);

  /* Tab 等控制字符：字体没有制表位语义。 */
  const compositor::TypeRasterReport tab =
      compositor::type_measure_text_with_font("A\tB", 3, style, font, &measure);
  COMP_CHECK(tab.status == TypeRasterStatus::kBadText);

  /* 只有换行：没有可排的字形。 */
  const compositor::TypeRasterReport newlines =
      compositor::type_measure_text_with_font("\n\n", 2, style, font, &measure);
  COMP_CHECK(newlines.status == TypeRasterStatus::kBadText);

  /* 样式量程与旧轨道一致。 */
  TypeRasterStyle tiny = style;
  tiny.font_size = 5.0;
  COMP_CHECK(compositor::type_measure_text_with_font("A", 1, tiny, font, &measure).status ==
             TypeRasterStatus::kBadStyle);
  TypeRasterStyle tight = style;
  tight.line_height = 0.5;
  COMP_CHECK(compositor::type_measure_text_with_font("A", 1, tight, font, &measure).status ==
             TypeRasterStatus::kBadStyle);

  /* 合法参数必须成功——证明拒绝的是参数而不是整条通路。 */
  COMP_CHECK(compositor::type_measure_text_with_font("Hi", 2, style, font, &measure).status ==
             TypeRasterStatus::kOk);
  COMP_CHECK(measure.glyph_count == 2);

  delete font;
}

COMP_TEST(comp_tool_type_with_font_paints_cjk_and_undoes_in_one_step) {
  constexpr int32_t kWidth = 256;
  constexpr int32_t kHeight = 128;
  constexpr size_t kTestStride = static_cast<size_t>(kWidth) * 4;

  comp_engine engine = nullptr;
  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-font-track-tests";
  engine_desc.version = "1.0.0";
  engine_desc.backend = COMP_BACKEND_CPU;
  engine_desc.worker_threads = 1;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  if (engine == nullptr) return;

  comp_document doc = nullptr;
  comp_document_desc doc_desc{};
  doc_desc.width = kWidth;
  doc_desc.height = kHeight;
  doc_desc.dpi = 72.0f;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);
  if (doc == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = "text";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, kWidth, kHeight};
  layer_desc.parent = 0;
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  COMP_CHECK(comp_layer_add(doc, &layer_desc, &layer) == COMP_OK);

  std::vector<uint8_t> transparent(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  comp_image image{};
  image.width = kWidth;
  image.height = kHeight;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = kTestStride;
  image.pixels = transparent.data();
  COMP_CHECK(comp_layer_set_pixels(doc, layer, &image) == COMP_OK);

  std::vector<uint8_t> before(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  COMP_CHECK(comp_render_flatten(doc, before.data(), COMP_PIXEL_RGBA8, kTestStride) == COMP_OK);

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_before, nullptr) == COMP_OK);

  comp_font font = nullptr;
  COMP_CHECK(comp_font_load_file(kFontPath, 0, &font) == COMP_OK);
  if (font == nullptr) {
    comp_document_destroy(doc);
    comp_engine_destroy(engine);
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_TYPE;
  desc.type.text = kCjkText;
  desc.type.text_length = static_cast<int32_t>(kCjkTextSize);
  desc.type.origin_x = 8.0;
  desc.type.origin_y = 8.0;
  desc.type.style.font_size = 48.0; /* 字体轨道：em 高 */
  desc.type.style.line_height = 1.25;
  desc.type.style.letter_spacing = 0.0;
  desc.type.style.antialias = 1;
  desc.type.color[0] = 255;
  desc.type.color[1] = 40;
  desc.type.color[2] = 40;
  desc.type.color[3] = 255;
  desc.type.opacity = 1.0f;
  desc.type_font = font;

  char note[512] = {0};
  size_t note_size = 0;
  COMP_CHECK_MSG(comp_tool_apply(doc, layer, &desc, note, sizeof(note), &note_size) == COMP_OK,
                 "the font track accepts CJK text through the ABI");
  COMP_CHECK_MSG(std::strlen(note) == 0,
                 "a clean type op has nothing to warn about (note stays empty)");
  COMP_CHECK(note_size <= sizeof(note));

  std::vector<uint8_t> after(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  COMP_CHECK(comp_render_flatten(doc, after.data(), COMP_PIXEL_RGBA8, kTestStride) == COMP_OK);

  size_t changed_pixels = 0;
  bool saw_requested_color = false;
  for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
    const uint8_t* now = after.data() + i * 4;
    const uint8_t* was = before.data() + i * 4;
    if (std::memcmp(now, was, 4) == 0) continue;
    ++changed_pixels;
    if (now[0] >= 200 && now[1] <= 96 && now[2] <= 96) saw_requested_color = true;
  }
  COMP_CHECK_MSG(changed_pixels > 1000, "48 px CJK text changes a substantial number of pixels");
  COMP_CHECK_MSG(saw_requested_color, "the painted ink came from the requested text color");

  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before + 1, "a font-track type op adds exactly one undo step");

  /*
   * 缺字仍是一次成功的操作：有墨就落笔，note 如实说明「有 1 个字符没字形，
   * 没有画出来」——不画方框，也不因为缺字整条拒绝。
   */
  const char* with_missing = "\xE4\xBD\xA0\xE5\xA5\xBD\xCD\xB8"; /* 你好 + U+0378 */
  desc.type.text = with_missing;
  desc.type.text_length = 8;
  desc.type.origin_y = 72.0;
  note[0] = '\0';
  note_size = 0;
  COMP_CHECK_MSG(comp_tool_apply(doc, layer, &desc, note, sizeof(note), &note_size) == COMP_OK,
                 "a missing glyph does not fail the whole op");
  COMP_CHECK_MSG(std::string(note).find("without a glyph") != std::string::npos,
                 "the note reports the missing glyph honestly");
  COMP_CHECK_MSG(std::string(note).find(" 1 ") != std::string::npos,
                 "the note counts exactly one missing character");

  size_t undo_after_missing = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_after_missing, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after_missing == undo_before + 2,
                 "the op with a missing glyph is still exactly one undo step");

  COMP_CHECK(comp_document_undo(doc) == COMP_OK);
  COMP_CHECK(comp_document_undo(doc) == COMP_OK);

  std::vector<uint8_t> undone(static_cast<size_t>(kWidth) * kHeight * 4, 0);
  COMP_CHECK(comp_render_flatten(doc, undone.data(), COMP_PIXEL_RGBA8, kTestStride) == COMP_OK);
  COMP_CHECK_MSG(undone == before, "undoing both type ops restores the render byte for byte");

  comp_font_release(font);
  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}

/*
 * 野句柄 / 已释放句柄不得崩溃（as_font 先查存活登记表再解引用）。
 *
 * 刻意用一个保证落在用户态寻址范围之外的地址（2^48，Windows 与 Linux x64 的
 * 用户态上限都是 2^47）：一旦 as_font 退回「直接解引用读 magic」的老路径，
 * 这个测试会段错误而不是断言失败——这正是要钉死的行为。
 */
COMP_TEST(font_abi_wild_and_released_handles_never_crash) {
  /* 2^48：所有受支持平台的用户态地址空间之外，保证未映射。 */
  const comp_font kWild = reinterpret_cast<comp_font>(0x1000000000000ULL);

  comp_font_metrics metrics{};
  COMP_CHECK(comp_font_get_metrics(kWild, &metrics) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_name(kWild, COMP_FONT_NAME_FAMILY) == nullptr);
  comp_font_release(kWild); /* 野句柄释放也必须是空操作，不崩。 */

  /* 释放后的句柄：先正常加载、释放，再复用。 */
  comp_font font = nullptr;
  COMP_CHECK(comp_font_load_file(kFontPath, 0, &font) == COMP_OK);
  if (font == nullptr) return;
  comp_font_release(font);

  comp_font_metrics released_metrics{};
  COMP_CHECK(comp_font_get_metrics(font, &released_metrics) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_font_name(font, COMP_FONT_NAME_FAMILY) == nullptr);
  comp_font_release(font); /* 重复释放：安全空操作。 */

  /* 工具调用里传野句柄 / 已释放句柄 -> COMP_ERR_STATE（不改画布、不加撤销步）。 */
  comp_engine engine = nullptr;
  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-font-handle-guard-tests";
  engine_desc.version = "1.0.0";
  engine_desc.backend = COMP_BACKEND_CPU;
  engine_desc.worker_threads = 1;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  if (engine == nullptr) return;

  comp_document doc = nullptr;
  comp_document_desc doc_desc{};
  doc_desc.width = 64;
  doc_desc.height = 32;
  doc_desc.dpi = 72.0f;
  COMP_CHECK(comp_document_create(engine, &doc_desc, &doc) == COMP_OK);
  if (doc == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  comp_layer_id layer = 0;
  comp_layer_desc layer_desc{};
  layer_desc.name = "text";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, 64, 32};
  COMP_CHECK(comp_layer_add(doc, &layer_desc, &layer) == COMP_OK);

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_TYPE;
  desc.type.text = "Hi";
  desc.type.text_length = 2;
  desc.type.origin_x = 4.0;
  desc.type.origin_y = 4.0;
  desc.type.style.font_size = 48.0;
  desc.type.style.line_height = 1.25;
  desc.type.style.letter_spacing = 0.0;
  desc.type.style.antialias = 1;
  desc.type.color[0] = 255;
  desc.type.color[1] = 255;
  desc.type.color[2] = 255;
  desc.type.color[3] = 255;
  desc.type.opacity = 1.0f;

  size_t undo_before = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_before, nullptr) == COMP_OK);

  char note[256] = {0};
  size_t note_size = 0;
  for (const comp_font bad : {kWild, font}) {
    note[0] = '\0';
    note_size = 0;
    desc.type_font = bad;
    COMP_CHECK_MSG(comp_tool_apply(doc, layer, &desc, note, sizeof(note), &note_size) ==
                       COMP_ERR_STATE,
                   "a wild or released font handle is a state error, never a crash");
  }

  size_t undo_after = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_after == undo_before, "a rejected type op adds no undo step");

  comp_document_destroy(doc);
  comp_engine_destroy(engine);
}
