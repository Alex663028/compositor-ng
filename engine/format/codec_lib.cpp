// AI生成
/*
 * codec_lib.cpp — 真实编解码后端（M5「编解码替换」的落地层）。
 *
 * 通道划分见 codec_lib.h。三条硬性约定：
 *   1. **不假装成功**：third_party 缺失或解码失败一律返回 false 并写 error；
 *   2. **预乘口径与 comp_png 一致**：文件内直通颜色，内存中预乘 RGBA8；
 *   3. **版本号取自库自身 API**（png_get_libpng_ver / zlibVersion / STBI_VERSION），
 *      不写死文案，避免登记表里出现会过期的「事实」。
 */
#include "core/comp_geometry.h"
#include "format/codec_lib.h"

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__has_include)
#  if __has_include("png.h")
#    define COMPOSITOR_CODEC_LIBPNG 1
#  endif
#  if __has_include("stb_image.h")
#    define COMPOSITOR_CODEC_STB 1
#  endif
#endif

#ifdef COMPOSITOR_CODEC_LIBPNG
#  include "png.h"
#  include "zlib.h"
#endif

#ifdef COMPOSITOR_CODEC_STB
#  ifndef STBI_NO_STDIO
#    define STBI_NO_STDIO
#  endif
#  define STB_IMAGE_IMPLEMENTATION
#  include "stb_image.h"
#  ifndef STBI_WRITE_NO_STDIO
#    define STBI_WRITE_NO_STDIO
#  endif
#  define STB_IMAGE_WRITE_IMPLEMENTATION
#  include "stb_image_write.h"
#endif

namespace compositor {
namespace {

/* 与 comp_image_format 数值一致（此处不复用 ABI 头，保持 format 层不反向依赖 ABI）。 */
constexpr uint32_t kFormatPng = 1u;
constexpr uint32_t kFormatJpeg = 2u;
constexpr uint32_t kFormatPsd = 7u;

int g_png_compression_level = 6;

#ifdef COMPOSITOR_CODEC_LIBPNG
/*
 * P2-12：libpng 默认把具体原因只打到 stderr，调用方（Swift / ArkTS / C#）拿到的
 * 只有通用文案，与刚修好的 P0-2 属于同一类「吞原因」毛病。
 * 这里装一个错误回调：先把真实原因记进 error，再按 libpng 默认格式打到 stderr
 * （保持既有可观测行为与文案格式），最后长跳转回 setjmp 分支。
 */
struct PngErrorSink {
  std::string* message;
};

void png_error_sink(png_structp png, png_const_charp msg) {
  PngErrorSink* sink = static_cast<PngErrorSink*>(png_get_error_ptr(png));
  if (sink != nullptr && sink->message != nullptr) {
    *sink->message = msg != nullptr ? std::string(msg) : std::string("libpng error");
  }
  std::fprintf(stderr, "libpng error: %s\n", msg != nullptr ? msg : "");
  std::fflush(stderr);
  std::longjmp(png_jmpbuf(png), 1);
}
#endif

uint8_t premul_channel(uint8_t color, uint8_t alpha) {
  return static_cast<uint8_t>((static_cast<uint32_t>(color) * alpha + 127u) / 255u);
}

uint8_t unpremul_channel(uint8_t color, uint8_t alpha) {
  if (alpha == 0u) return 0u;
  if (alpha == 255u) return color;
  const uint32_t value = (static_cast<uint32_t>(color) * 255u + alpha / 2u) / alpha;
  return static_cast<uint8_t>(value > 255u ? 255u : value);
}

void premultiply_in_place(std::vector<uint8_t>* rgba) {
  std::vector<uint8_t>& p = *rgba;
  for (size_t i = 0; i + 3u < p.size(); i += 4u) {
    const uint8_t a = p[i + 3u];
    p[i + 0u] = premul_channel(p[i + 0u], a);
    p[i + 1u] = premul_channel(p[i + 1u], a);
    p[i + 2u] = premul_channel(p[i + 2u], a);
  }
}

void unpremultiply_in_place(std::vector<uint8_t>* rgba) {
  std::vector<uint8_t>& p = *rgba;
  for (size_t i = 0; i + 3u < p.size(); i += 4u) {
    const uint8_t a = p[i + 3u];
    p[i + 0u] = unpremul_channel(p[i + 0u], a);
    p[i + 1u] = unpremul_channel(p[i + 1u], a);
    p[i + 2u] = unpremul_channel(p[i + 2u], a);
  }
}

bool fail(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
  return false;
}

}  // namespace

/* ----------------------------------------------------------- 能力探测 */

bool codec_lib_has_png() {
#ifdef COMPOSITOR_CODEC_LIBPNG
  return true;
#else
  return false;
#endif
}

bool codec_lib_has_stb() {
#ifdef COMPOSITOR_CODEC_STB
  return true;
#else
  return false;
#endif
}

const char* codec_lib_png_backend() {
#ifdef COMPOSITOR_CODEC_LIBPNG
  static const std::string backend =
      std::string("libpng ") + png_get_libpng_ver(nullptr) + " + zlib " + zlibVersion();
  return backend.c_str();
#else
  return "libpng (not vendored)";
#endif
}

const char* codec_lib_stb_backend() {
#ifdef COMPOSITOR_CODEC_STB
  static const std::string backend =
      std::string("stb_image v") + std::to_string(STBI_VERSION / 100) + "." +
      std::to_string(STBI_VERSION % 100) + " + stb_image_write";
  return backend.c_str();
#else
  return "stb_image (not vendored)";
#endif
}

void codec_lib_set_png_compression(int level) {
  g_png_compression_level = (level < 0 || level > 9) ? 6 : level;
}

/* ------------------------------------------------------------- libpng */

#ifdef COMPOSITOR_CODEC_LIBPNG

namespace {

struct PngMemoryReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

void png_memory_read(png_structp png, png_bytep out, size_t count) {
  PngMemoryReader* reader = static_cast<PngMemoryReader*>(png_get_io_ptr(png));
  if (reader == nullptr || count > reader->size - reader->offset) {
    png_error(png, "png read past end of buffer");
    return;
  }
  std::memcpy(out, reader->data + reader->offset, count);
  reader->offset += count;
}

struct PngMemoryWriter {
  std::vector<uint8_t>* out = nullptr;
};

void png_memory_write(png_structp png, png_bytep data, size_t count) {
  PngMemoryWriter* writer = static_cast<PngMemoryWriter*>(png_get_io_ptr(png));
  if (writer == nullptr || writer->out == nullptr) {
    png_error(png, "png write without sink");
    return;
  }
  const uint8_t* bytes = data;
  writer->out->insert(writer->out->end(), bytes, bytes + count);
}

void png_memory_flush(png_structp /*png*/) {}

/*
 * 把任意合法 PNG 归一化为紧凑 RGBA8：
 *   调色板 -> RGB；1/2/4 位灰度 -> 8 位；tRNS -> alpha；16 位 -> 8 位；
 *   灰度 -> RGB；无 alpha 的补 255；Adam7 隔行由 libpng 处理。
 */
bool png_decode_impl(const uint8_t* data, size_t size, CodecImage* out, std::string* error) {
  if (data == nullptr || size == 0u) return fail(error, "png buffer is empty");
  if (size < 8u) return fail(error, "png buffer too small for signature");

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) return fail(error, "png_create_read_struct failed");
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    return fail(error, "png_create_info_struct failed");
  }

  /*
   * P2-12：错误回调必须在 setjmp 之前装好，真实原因（如 "png exceeds maximum canvas
   * size" / "Invalid IHDR data"）才会被记进 error 而不再被吞成通用文案。
   * 注意：libpng 用 longjmp 报错，setjmp 与 longjmp 之间不做堆分配即可安全返回。
   */
  PngErrorSink sink{error};
  png_set_error_fn(png, &sink, png_error_sink, nullptr);
  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_read_struct(&png, &info, nullptr);
    if (error != nullptr && !error->empty()) return false;
    return fail(error, "libpng decode failed (malformed or unsupported PNG)");
  }

  PngMemoryReader reader;
  reader.data = data;
  reader.size = size;
  png_set_read_fn(png, &reader, png_memory_read);
  png_read_info(png, info);

  png_uint_32 width = 0;
  png_uint_32 height = 0;
  int bit_depth = 0;
  int color_type = 0;
  int interlace = 0;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace, nullptr, nullptr);
  if (width == 0u || height == 0u) png_error(png, "png has zero extent");
  /* P1-2：libpng 默认信任声明尺寸；此处按 .comp 规范上界预检，
     避免 :226/:238 的连续分配被伪造 IHDR 打爆（也等价于 png_set_user_limits）。 */
  if (!is_valid_canvas(comp_size{static_cast<int32_t>(width), static_cast<int32_t>(height)})) {
    png_error(png, "png exceeds maximum canvas size");
  }

  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  const bool has_trns = png_get_valid(png, info, PNG_INFO_tRNS) != 0u;
  if (has_trns) png_set_tRNS_to_alpha(png);
  if (bit_depth == 16) png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }
  const bool has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || has_trns;
  if (!has_alpha) png_set_add_alpha(png, 0xffu, PNG_FILLER_AFTER);
  if (interlace != PNG_INTERLACE_NONE) png_set_interlace_handling(png);

  png_read_update_info(png, info);
  const size_t row_bytes = png_get_rowbytes(png, info);
  const size_t packed_bytes = static_cast<size_t>(width) * 4u;
  if (row_bytes < packed_bytes) png_error(png, "unexpected decoded row size");

  std::vector<uint8_t> scratch(static_cast<size_t>(height) * row_bytes, 0u);
  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (size_t y = 0; y < static_cast<size_t>(height); ++y) {
    rows[y] = scratch.data() + y * row_bytes;
  }
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);

  CodecImage image;
  image.width = static_cast<int32_t>(width);
  image.height = static_cast<int32_t>(height);
  image.rgba.resize(packed_bytes * static_cast<size_t>(height));
  for (size_t y = 0; y < static_cast<size_t>(height); ++y) {
    std::memcpy(image.rgba.data() + y * packed_bytes, scratch.data() + y * row_bytes, packed_bytes);
  }
  premultiply_in_place(&image.rgba);
  *out = std::move(image);
  return true;
}

bool png_encode_impl(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                     std::vector<uint8_t>* out, std::string* error) {
  if (premultiplied_rgba == nullptr || out == nullptr) return fail(error, "null buffer");
  if (width <= 0 || height <= 0) return fail(error, "non-positive extent");

  const size_t packed_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> straight(premultiplied_rgba,
                                premultiplied_rgba + packed_bytes * static_cast<size_t>(height));
  unpremultiply_in_place(&straight);

  out->clear();
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) return fail(error, "png_create_write_struct failed");
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return fail(error, "png_create_info_struct failed");
  }
  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_write_struct(&png, &info);
    out->clear();
    return fail(error, "libpng encode failed");
  }

  PngMemoryWriter writer;
  writer.out = out;
  png_set_write_fn(png, &writer, png_memory_write, png_memory_flush);
  png_set_IHDR(png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 8,
               PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_set_compression_level(png, g_png_compression_level);
  png_write_info(png, info);

  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (size_t y = 0; y < static_cast<size_t>(height); ++y) {
    rows[y] = const_cast<png_bytep>(straight.data() + y * packed_bytes);
  }
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return !out->empty();
}

}  // namespace

#endif /* COMPOSITOR_CODEC_LIBPNG */

/* ---------------------------------------------------------------- stb */

#ifdef COMPOSITOR_CODEC_STB

namespace {

bool stb_decode_impl(const uint8_t* data, size_t size, CodecImage* out, std::string* error) {
  if (data == nullptr || size == 0u) return fail(error, "image buffer is empty");
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
  if (pixels == nullptr) {
    const char* reason = stbi_failure_reason();
    if (error != nullptr) {
      *error = std::string("stb_image: ") + (reason != nullptr ? reason : "decode failed");
    }
    return false;
  }
  CodecImage image;
  image.width = width;
  image.height = height;
  image.rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  stbi_image_free(pixels);
  premultiply_in_place(&image.rgba);
  *out = std::move(image);
  return true;
}

void stb_write_sink(void* context, void* data, int size) {
  std::vector<uint8_t>* out = static_cast<std::vector<uint8_t>*>(context);
  if (out == nullptr || data == nullptr || size <= 0) return;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + static_cast<size_t>(size));
}

bool stb_encode_jpeg_impl(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                          int quality, std::vector<uint8_t>* out, std::string* error) {
  if (premultiplied_rgba == nullptr || out == nullptr) return fail(error, "null buffer");
  if (width <= 0 || height <= 0) return fail(error, "non-positive extent");
  const int clamped = (quality < 1 || quality > 100) ? 92 : quality;

  const size_t packed_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> straight(premultiplied_rgba,
                                premultiplied_rgba + packed_bytes * static_cast<size_t>(height));
  unpremultiply_in_place(&straight);

  out->clear();
  if (stbi_write_jpg_to_func(stb_write_sink, out, width, height, 4, straight.data(), clamped) == 0) {
    out->clear();
    return fail(error, "stb_image_write: jpeg encode failed");
  }
  return true;
}

}  // namespace

#endif /* COMPOSITOR_CODEC_STB */

/* ------------------------------------------------------------ 分派入口 */

bool codec_png_decode(const uint8_t* data, size_t size, CodecImage* out, std::string* error) {
#ifdef COMPOSITOR_CODEC_LIBPNG
  return png_decode_impl(data, size, out, error);
#else
  (void)data;
  (void)size;
  (void)out;
  return fail(error, "libpng is not vendored in this build");
#endif
}

bool codec_png_encode(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      std::vector<uint8_t>* out, std::string* error) {
#ifdef COMPOSITOR_CODEC_LIBPNG
  return png_encode_impl(premultiplied_rgba, width, height, out, error);
#else
  (void)premultiplied_rgba;
  (void)width;
  (void)height;
  (void)out;
  return fail(error, "libpng is not vendored in this build");
#endif
}

bool codec_stb_decode(const uint8_t* data, size_t size, uint32_t format, CodecImage* out,
                      std::string* error) {
#ifdef COMPOSITOR_CODEC_STB
  if (format != kFormatJpeg && format != kFormatPsd) {
    return fail(error, "stb channel handles JPEG and PSD only");
  }
  return stb_decode_impl(data, size, out, error);
#else
  (void)data;
  (void)size;
  (void)format;
  (void)out;
  return fail(error, "stb_image is not vendored in this build");
#endif
}

bool codec_stb_encode(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      uint32_t format, int quality, std::vector<uint8_t>* out, std::string* error) {
#ifdef COMPOSITOR_CODEC_STB
  if (format != kFormatJpeg) {
    return fail(error, "stb write channel handles JPEG only (PSD is read-only)");
  }
  return stb_encode_jpeg_impl(premultiplied_rgba, width, height, quality, out, error);
#else
  (void)premultiplied_rgba;
  (void)width;
  (void)height;
  (void)format;
  (void)quality;
  (void)out;
  return fail(error, "stb_image_write is not vendored in this build");
#endif
}

bool codec_lib_can_decode(uint32_t format) {
  switch (format) {
    case kFormatPng:
      return codec_lib_has_png();
    case kFormatJpeg:
    case kFormatPsd:
      return codec_lib_has_stb();
    default:
      return false;
  }
}

bool codec_lib_can_encode(uint32_t format) {
  switch (format) {
    case kFormatPng:
      return codec_lib_has_png();
    case kFormatJpeg:
      return codec_lib_has_stb();
    default:
      return false; /* PSD 只读 */
  }
}

bool codec_lib_decode(const uint8_t* data, size_t size, uint32_t format, CodecImage* out,
                      std::string* error) {
  if (out == nullptr) return fail(error, "out is null");
  switch (format) {
    case kFormatPng:
      return codec_png_decode(data, size, out, error);
    case kFormatJpeg:
    case kFormatPsd:
      return codec_stb_decode(data, size, format, out, error);
    default:
      return fail(error, "no vendored decoder for this format");
  }
}

bool codec_lib_encode(const uint8_t* premultiplied_rgba, int32_t width, int32_t height,
                      uint32_t format, int quality, std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) return fail(error, "out is null");
  switch (format) {
    case kFormatPng:
      return codec_png_encode(premultiplied_rgba, width, height, out, error);
    case kFormatJpeg:
      return codec_stb_encode(premultiplied_rgba, width, height, format, quality, out, error);
    default:
      return fail(error, "no vendored encoder for this format");
  }
}

}  // namespace compositor
