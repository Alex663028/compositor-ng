// AI生成
/*
 * test_font.cpp — 字体加载（P5-1）领域与 ABI 契约。
 *
 * 覆盖：
 *   - 随仓 subset 字体能从文件 / 内存两条入口加载，身份名与度量符合资源清单；
 *   - 内存入口**拷贝**字节（宿主缓冲销毁后字体仍可用）；
 *   - 空路径 / 负面序号 / 文件不存在 / 非字体字节 / 越界面序号各自的失败码；
 *   - 度量字段自洽（height = ascender - descender + line_gap）与中文门槛 has_cjk；
 *   - ABI 句柄的加载 / 取名 / 度量 / 释放，以及错误码映射（IO / DECODE / ARG）。
 *
 * 断言只依赖 resources/fonts/README.md 里登记的资源清单（字形数、upem、字体名），
 * 不钉具体字形成分——那是 P5-2 栅格化用例的事。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_font.h"
#include "domain/font_face.h"
#include "test_framework.h"

namespace {

using compositor::FontFace;
using compositor::FontLoadStatus;
using compositor::FontMetricsInfo;

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
