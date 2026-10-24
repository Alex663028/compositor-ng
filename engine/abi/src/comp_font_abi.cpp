// AI生成
/*
 * comp_font_abi.cpp — 字体加载 C ABI 实现（P5-1）。
 *
 * 句柄就是 FontFace* 的 reinterpret_cast，靠对象内置的 kFontMagic 校验
 * （与 Engine/Document 同一套轻量方案，见 comp_abi_internal.h 的注释）；
 * 解引用前先查「存活字体登记表」（is_live_font，见 domain/font_face.h），
 * 使完全野的句柄也不会段错误。
 * 字体句柄不挂在引擎上，因此这里没有 last_error 文本，失败原因只由返回码表达；
 * 需要细粒度原因（例如「不是字体」vs「内存不足」）的宿主应看 comp_result。
 */
#include "compositor/comp_font.h"

#include "comp_abi_internal.h"
#include "domain/font_face.h"

namespace compositor {
namespace abi {

FontFace* as_font(comp_font handle) {
  FontFace* font = reinterpret_cast<FontFace*>(handle);
  /*
   * 先查存活登记表（带类型标签），命中且类型为 kFont 才允许解引用读 magic：
   *   - 完全野的句柄（如 0xDEADBEEF 落在未映射页）在表里查不到，直接判 nullptr，
   *     不会段错误；调用方据此返回 COMP_ERR_STATE / COMP_ERR_INVALID_ARGUMENT。
   *   - 已释放的句柄在 ~FontFace 里已注销，同样查不到。
   *   - 类型标签必须查：magic 是 FontFace 的尾部成员，按 FontFace 布局去读一个
   *     Engine/Document 指针会越界（F2）。命中且类型对上，读 magic 才是安全的；
   *     再核对 magic 是挡"注销与 poison 之间的极端时序"。
   * 查询只哈希指针值本身，不解引用，野指针查询本身也是安全的。
   */
  if (font == nullptr || !is_live_font(font)) return nullptr;
  if (!font->is_alive()) return nullptr;
  return font;
}

namespace {

comp_result map_status(FontLoadStatus status) {
  switch (status) {
    case FontLoadStatus::kOk:
      return COMP_OK;
    case FontLoadStatus::kBadArgument:
    case FontLoadStatus::kBadFaceIndex:
      return COMP_ERR_INVALID_ARGUMENT;
    case FontLoadStatus::kNotFound:
      return COMP_ERR_IO;
    case FontLoadStatus::kNotAFont:
      return COMP_ERR_DECODE;
    case FontLoadStatus::kOutOfMemory:
      return COMP_ERR_OUT_OF_MEMORY;
    default:
      return COMP_ERR_INTERNAL;
  }
}

}  // namespace
}  // namespace abi
}  // namespace compositor

using compositor::FontFace;
using compositor::FontLoadStatus;
using compositor::abi::as_font;

extern "C" {

COMP_API comp_result comp_font_load_file(const char* path_utf8, int32_t face_index,
                                         comp_font* out_font) {
  if (out_font == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_font = nullptr;
  if (path_utf8 == nullptr || path_utf8[0] == '\0' || face_index < 0) {
    return COMP_ERR_INVALID_ARGUMENT;
  }

  COMP_ABI_TRY(nullptr)
  FontLoadStatus status = FontLoadStatus::kBadArgument;
  std::string message;
  FontFace* font = FontFace::load_file(path_utf8, face_index, &status, &message);
  if (font == nullptr) return compositor::abi::fail(nullptr, compositor::abi::map_status(status),
                                                    message);
  *out_font = reinterpret_cast<comp_font>(font);
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

COMP_API comp_result comp_font_load_memory(const void* data, size_t size, int32_t face_index,
                                           comp_font* out_font) {
  if (out_font == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_font = nullptr;
  if (data == nullptr || size == 0 || face_index < 0) return COMP_ERR_INVALID_ARGUMENT;

  COMP_ABI_TRY(nullptr)
  FontLoadStatus status = FontLoadStatus::kBadArgument;
  std::string message;
  FontFace* font = FontFace::load_memory(data, size, face_index, &status, &message);
  if (font == nullptr) return compositor::abi::fail(nullptr, compositor::abi::map_status(status),
                                                    message);
  *out_font = reinterpret_cast<comp_font>(font);
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

COMP_API void comp_font_release(comp_font font) {
  FontFace* face = as_font(font);
  if (face == nullptr) return; /* NULL / 无效 / 已释放：安全空操作。 */
  /*
   * 句柄一经释放即不可再用（与 comp_engine / comp_document 同一套约定）。
   * as_font 先查存活登记表再读 magic，因此重复释放（把已 delete 的句柄再传
   * 进来）不会再解引用已释放的内存——判 nullptr 后原路返回，是安全的空操作；
   * 但这仍是宿主错误，契约外行为，不要依赖。
   */
  delete face; /* ~FontFace 会把自己从登记表注销。 */
}

COMP_API comp_result comp_font_get_metrics(comp_font font, comp_font_metrics* out_metrics) {
  if (out_metrics == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  const FontFace* face = as_font(font);
  if (face == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  const compositor::FontMetricsInfo& info = face->metrics();
  out_metrics->face_index = info.face_index;
  out_metrics->face_count = info.face_count;
  out_metrics->glyph_count = info.glyph_count;
  out_metrics->units_per_em = info.units_per_em;
  out_metrics->scalable = info.scalable ? 1 : 0;
  out_metrics->has_cjk = info.has_cjk ? 1 : 0;
  out_metrics->ascender = info.ascender;
  out_metrics->descender = info.descender;
  out_metrics->line_gap = info.line_gap;
  out_metrics->height = info.height;
  out_metrics->max_advance = info.max_advance;
  return COMP_OK;
}

COMP_API const char* comp_font_name(comp_font font, comp_font_name_kind kind) {
  const FontFace* face = as_font(font);
  if (face == nullptr) return nullptr;
  switch (kind) {
    case COMP_FONT_NAME_FAMILY:
      return face->family_name().c_str();
    case COMP_FONT_NAME_STYLE:
      return face->style_name().c_str();
    case COMP_FONT_NAME_POSTSCRIPT:
      return face->postscript_name().c_str();
    default:
      return nullptr;
  }
}

} /* extern "C" */
