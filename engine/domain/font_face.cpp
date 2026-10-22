// AI生成
#include "font_face.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <system_error>
#include <mutex>

/* FreeType 的规矩：先 ft2build.h（它定义 FT_*_H 这组包含宏），再按宏包含具体头。 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H

#include "../core/comp_handle_magic.h"
#include "../core/comp_live_handles.h"

namespace compositor {
namespace {

/* ---------- UTF-8 工具（只在本文件用，不导出） ---------- */

void append_utf8(std::string* out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

/* SFNT 的 Unicode 名记录是 UTF-16BE；坏代理对替换成 U+FFFD，不编造字符。 */
std::string utf16be_to_utf8(const FT_Byte* data, FT_UInt length) {
  std::string out;
  for (FT_UInt i = 0; i + 1 < length; i += 2) {
    const uint32_t unit = (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
    uint32_t codepoint = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (i + 3 < length) {
        const uint32_t low = (static_cast<uint32_t>(data[i + 2]) << 8) | data[i + 3];
        if (low >= 0xDC00 && low <= 0xDFFF) {
          codepoint = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
          i += 2;
        } else {
          codepoint = 0xFFFD;
        }
      } else {
        codepoint = 0xFFFD;
      }
    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
      codepoint = 0xFFFD;
    }
    append_utf8(&out, codepoint);
  }
  return out;
}

/*
 * 从 name 表取一条名字。优先 MS 平台 Unicode（英文 0x409 加分），
 * 其次 Apple Unicode、Mac Roman；同分取靠前记录（字体通常第一组是英文）。
 * 取不到返回空串，由调用方决定回落。
 */
std::string sfnt_name(FT_Face face, FT_UShort name_id) {
  const FT_UInt count = FT_Get_Sfnt_Name_Count(face);
  int best_score = -1;
  FT_UInt best_index = 0;
  for (FT_UInt i = 0; i < count; ++i) {
    FT_SfntName name;
    if (FT_Get_Sfnt_Name(face, i, &name) != 0) continue;
    if (name.name_id != name_id) continue;
    int score = -1;
    if (name.platform_id == TT_PLATFORM_MICROSOFT &&
        (name.encoding_id == TT_MS_ID_UNICODE_CS || name.encoding_id == TT_MS_ID_UCS_4)) {
      score = (name.language_id == 0x0409) ? 100 : 50;
    } else if (name.platform_id == TT_PLATFORM_APPLE_UNICODE) {
      score = 40;
    } else if (name.platform_id == TT_PLATFORM_MACINTOSH && name.encoding_id == TT_MAC_ID_ROMAN) {
      score = 30;
    }
    if (score > best_score) {
      best_score = score;
      best_index = i;
    }
  }
  if (best_score < 0) return std::string();

  FT_SfntName name;
  if (FT_Get_Sfnt_Name(face, best_index, &name) != 0) return std::string();
  if (best_score >= 40) return utf16be_to_utf8(name.string, name.string_len);
  /* Mac Roman：ASCII 范围字节即 UTF-8，非 ASCII 极少见，原样传递并在上层按需替换。 */
  return std::string(reinterpret_cast<const char*>(name.string), name.string_len);
}

/*
 * 把 FreeType 的错误码翻成本模块的枚举。
 *
 * 格式/流/表一级的失败都归到 kNotAFont：站在「能不能用这份字体排版」的角度，
 * 「不是 sfnt」「表目录坏了」「流读到一半断了」是同一件事——文件不可用。
 * Invalid_Argument 两种来源：face_index 越界（可以确定是参数问题）或坏数据
 * （由调用方按 face_index 是否 > 0 区分）。
 */
FontLoadStatus map_error(FT_Error error, int32_t face_index) {
  switch (error) {
    case FT_Err_Unknown_File_Format:
    case FT_Err_Invalid_File_Format:
    case FT_Err_Invalid_Table:
    case FT_Err_Invalid_Offset:
    case FT_Err_Array_Too_Large:
    case FT_Err_Cannot_Open_Stream:
    case FT_Err_Invalid_Stream_Seek:
    case FT_Err_Invalid_Stream_Skip:
    case FT_Err_Invalid_Stream_Read:
    case FT_Err_Invalid_Stream_Operation:
      return FontLoadStatus::kNotAFont;
    case FT_Err_Invalid_Argument:
    case FT_Err_Invalid_Face_Handle:
      return (face_index > 0) ? FontLoadStatus::kBadFaceIndex : FontLoadStatus::kNotAFont;
    case FT_Err_Out_Of_Memory:
      return FontLoadStatus::kOutOfMemory;
    default:
      return FontLoadStatus::kInternal;
  }
}

void set_message(std::string* message, const char* text) {
  if (message != nullptr) *message = text;
}

/* 100 MB 上限：字体再大也不该整块读进内存（.ttc 里确实有这种怪物）。 */
constexpr size_t kMaxFontBytes = 100u * 1024u * 1024u;

/* ---------- 存活字体登记表：转发共享表（core/comp_live_handles.h） ---------- */

}  // namespace

/* 三个 font 侧入口都是共享存活登记表的薄转发：表键是 const void*，
 * Engine / Document / FontFace 共用一张表；类型区分仍交给各自对象的 magic。 */
void register_live_font(FontFace* face) noexcept {
  register_live_handle(face);
}

void unregister_live_font(FontFace* face) noexcept {
  unregister_live_handle(face);
}

bool is_live_font(FontFace* face) noexcept {
  return is_live_handle(face);
}

FontFace::FontFace() = default;

FontFace::~FontFace() {
  /* 先从登记表注销：此后该指针即便是被 delete 释放后再被宿主误传进来，
   * is_live_font 也查不到，as_font 不会解引用已释放的内存。 */
  unregister_live_font(this);
  handle_magic_ = kHandlePoisoned;
  if (face_ != nullptr) {
    FT_Done_Face(static_cast<FT_Face>(face_));
    face_ = nullptr;
  }
  if (library_ != nullptr) {
    FT_Done_FreeType(static_cast<FT_Library>(library_));
    library_ = nullptr;
  }
}

bool FontFace::is_alive() const noexcept {
  return handle_magic_ == kFontMagic && face_ != nullptr;
}

FontFace* FontFace::load_file(const char* path_utf8, int32_t face_index, FontLoadStatus* status,
                              std::string* message) {
  if (status != nullptr) *status = FontLoadStatus::kBadArgument;
  if (path_utf8 == nullptr || path_utf8[0] == '\0' || face_index < 0) {
    set_message(message, "字体路径为空或 face_index 为负");
    return nullptr;
  }

  std::error_code ec;
  const std::filesystem::path path = std::filesystem::u8path(path_utf8);
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    if (status != nullptr) *status = FontLoadStatus::kNotFound;
    set_message(message, "字体文件不存在或不是普通文件");
    return nullptr;
  }

  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    if (status != nullptr) *status = FontLoadStatus::kNotFound;
    set_message(message, "字体文件打不开");
    return nullptr;
  }
  const std::streamoff size = stream.tellg();
  if (size <= 0) {
    if (status != nullptr) *status = FontLoadStatus::kNotAFont;
    set_message(message, "字体文件是空的");
    return nullptr;
  }
  /* 100 MB 上限：字体再大也不该整块读进内存（.ttc 里确实有这种怪物）。 */
  if (size > static_cast<std::streamoff>(kMaxFontBytes)) {
    if (status != nullptr) *status = FontLoadStatus::kBadArgument;
    set_message(message, "字体文件超过 100 MB 上限");
    return nullptr;
  }

  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  stream.seekg(0, std::ios::beg);
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
    if (status != nullptr) *status = FontLoadStatus::kNotFound;
    set_message(message, "字体文件读取失败");
    return nullptr;
  }
  stream.close();

  return create_from_bytes(std::move(bytes), face_index, status, message);
}

FontFace* FontFace::load_memory(const void* data, size_t size, int32_t face_index,
                                FontLoadStatus* status, std::string* message) {
  if (status != nullptr) *status = FontLoadStatus::kBadArgument;
  if (data == nullptr || size == 0 || face_index < 0) {
    set_message(message, "字体数据为空或 face_index 为负");
    return nullptr;
  }
  if (size > kMaxFontBytes) {
    set_message(message, "字体数据超过 100 MB 上限");
    return nullptr;
  }

  std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + size);
  return create_from_bytes(std::move(bytes), face_index, status, message);
}

FontFace* FontFace::create_from_bytes(std::vector<uint8_t> bytes, int32_t face_index,
                                      FontLoadStatus* status, std::string* message) {
  std::unique_ptr<FontFace> self(new (std::nothrow) FontFace());
  if (!self) {
    if (status != nullptr) *status = FontLoadStatus::kOutOfMemory;
    set_message(message, "内存不足");
    return nullptr;
  }
  if (!self->init_from_bytes(std::move(bytes), face_index, status, message)) return nullptr;
  /* 登记为存活字体：这是 as_font 在解引用前能查到的唯一凭据。 */
  register_live_font(self.get());
  return self.release();
}

bool FontFace::init_from_bytes(std::vector<uint8_t> bytes, int32_t face_index,
                               FontLoadStatus* status, std::string* message) {
  bytes_ = std::move(bytes);

  FT_Library library = nullptr;
  FT_Error error = FT_Init_FreeType(&library);
  if (error != 0 || library == nullptr) {
    if (status != nullptr) *status = FontLoadStatus::kOutOfMemory;
    set_message(message, "FreeType 初始化失败（多半是内存不足）");
    return false;
  }
  library_ = library;

  FT_Face face = nullptr;
  error = FT_New_Memory_Face(library, bytes_.data(), static_cast<FT_Long>(bytes_.size()),
                             face_index, &face);
  if (error != 0 || face == nullptr) {
    const FontLoadStatus failure = map_error(error, face_index);
    if (status != nullptr) *status = failure;
    switch (failure) {
      case FontLoadStatus::kNotAFont:
        set_message(message, "不是可识别的字体（本构建不支持 gzip / WOFF / WOFF2）");
        break;
      case FontLoadStatus::kBadFaceIndex:
        set_message(message, "字体里没有该 face_index");
        break;
      case FontLoadStatus::kOutOfMemory:
        set_message(message, "内存不足");
        break;
      default:
        set_message(message, "FreeType 打开字体失败");
        break;
    }
    return false;
  }
  face_ = face;

  read_identity();
  read_metrics();

  handle_magic_ = kFontMagic;
  if (status != nullptr) *status = FontLoadStatus::kOk;
  set_message(message, "");
  return true;
}

void FontFace::read_identity() {
  FT_Face face = static_cast<FT_Face>(face_);
  family_name_ = sfnt_name(face, TT_NAME_ID_FONT_FAMILY);
  if (family_name_.empty()) family_name_ = sfnt_name(face, TT_NAME_ID_TYPOGRAPHIC_FAMILY);
  style_name_ = sfnt_name(face, TT_NAME_ID_FONT_SUBFAMILY);
  if (style_name_.empty()) style_name_ = sfnt_name(face, TT_NAME_ID_TYPOGRAPHIC_SUBFAMILY);
  const char* postscript = FT_Get_Postscript_Name(face);
  postscript_name_ = (postscript != nullptr) ? postscript : "";
}

void FontFace::read_metrics() {
  FT_Face face = static_cast<FT_Face>(face_);
  FontMetricsInfo* info = &metrics_;
  info->face_index = static_cast<int32_t>(face->face_index);
  info->face_count = static_cast<int32_t>(face->num_faces);
  info->glyph_count = static_cast<int32_t>(face->num_glyphs);
  info->units_per_em = face->units_per_EM;
  info->scalable = FT_IS_SCALABLE(face) != 0;

  /* 非可缩放字体 units_per_EM 为 0；不做除法，度量留 0 让上层拒绝排版。 */
  if (info->units_per_em == 0) {
    info->ascender = 0.0;
    info->descender = 0.0;
    info->line_gap = 0.0;
    info->height = 0.0;
    info->max_advance = 0.0;
    info->has_cjk = false;
    return;
  }

  const double em = static_cast<double>(info->units_per_em);
  info->ascender = static_cast<double>(face->ascender) / em;
  info->descender = static_cast<double>(face->descender) / em;
  info->line_gap = static_cast<double>(face->height - face->ascender + face->descender) / em;
  info->height = static_cast<double>(face->height) / em;
  info->max_advance = static_cast<double>(face->max_advance_width) / em;

  /* U+4F60「你」有字形 = 能排中文的最低门槛（不做完整覆盖统计，见头文件）。 */
  info->has_cjk = FT_Get_Char_Index(face, 0x4F60) != 0;
}

bool FontFace::render_glyph(uint32_t codepoint, double pixel_size, FontGlyph* out) {
  if (out == nullptr) return false;
  *out = FontGlyph{};
  if (!is_alive() || !std::isfinite(pixel_size) || pixel_size < 1.0 || pixel_size > 4096.0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  FT_Face face = static_cast<FT_Face>(face_);
  /* 26.6 定点：小数字号（如 48.5 px）不被截断，字形随字号线性缩放。 */
  const FT_F26Dot6 size_26_6 = static_cast<FT_F26Dot6>(std::lround(pixel_size * 64.0));
  if (FT_Set_Char_Size(face, 0, size_26_6, 72, 72) != 0) return false;

  const FT_UInt index = FT_Get_Char_Index(face, codepoint);
  if (index == 0) {
    /* 缺字：不画 .notdef（那是字体的方框，不是用户的字），按最大步进留白。 */
    out->missing = true;
    const long fallback = (face->size != nullptr) ? face->size->metrics.max_advance : 0;
    out->advance = fallback > 0 ? static_cast<double>(fallback) / 64.0 : pixel_size * 0.5;
    return true;
  }

  if (FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING | FT_LOAD_RENDER) != 0) return false;
  const FT_GlyphSlot slot = face->glyph;
  if (slot == nullptr) return false;
  out->advance = static_cast<double>(slot->advance.x) / 64.0;

  const FT_Bitmap& bitmap = slot->bitmap;
  if (bitmap.width == 0 || bitmap.rows == 0) return true; /* 空格等无墨字形 */
  if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.buffer == nullptr) return false;

  out->width = static_cast<int32_t>(bitmap.width);
  out->height = static_cast<int32_t>(bitmap.rows);
  out->bearing_x = slot->bitmap_left;
  out->bearing_y = slot->bitmap_top;
  out->bitmap.resize(static_cast<size_t>(out->width) * static_cast<size_t>(out->height));

  /*
   * FreeType 的 pitch 是「加多少字节到指针上就往下走一行」，可为负（自下而上
   * 存的行）。负 pitch 时 buffer 指向最后一行，故按行号从尾往前走。
   */
  const ptrdiff_t pitch = static_cast<ptrdiff_t>(bitmap.pitch);
  for (int32_t row = 0; row < out->height; ++row) {
    const int32_t source_row = pitch >= 0 ? row : (out->height - 1 - row);
    const uint8_t* source = bitmap.buffer + static_cast<ptrdiff_t>(source_row) * pitch;
    std::memcpy(out->bitmap.data() + static_cast<size_t>(row) * static_cast<size_t>(out->width),
                source, static_cast<size_t>(out->width));
  }
  return true;
}

}  // namespace compositor
