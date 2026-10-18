// AI生成
/*
 * comp_io_abi.cpp — comp_io.h 的 C ABI 实现。
 *
 * 分工：
 *   .comp 工程读写     自有格式（无第三方依赖），见 comp_file / comp_png；
 *   外部图像导入/导出  由 codec_lib 分派到 vendor 的真实库（PNG=libpng+zlib，
 *                      JPEG=stb_image/stb_image_write，PSD=stb_image 只读）；
 *                      第三方缺失时 PNG 回落到自研 comp_png，其余如实 UNSUPPORTED。
 *   浮点来源（C3）     Radiance .hdr/.pic 走 stb_image 的浮点通道，解出的是
 *                      **线性光浮点**；进 HDR 图层还是 16 位图层由 wideAsset
 *                      判决（peak > 1.001），与上游 ImageImporter.swift:114 同口径。
 *
 * 失败语义（可解释、可测试）：
 *   文件不存在 / 内容损坏 / 写盘失败 -> COMP_ERR_IO
 *   该格式在本构建中不可读或不可写 -> COMP_ERR_UNSUPPORTED（原因由登记表生成）
 */
#include "compositor/comp_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "comp_abi_internal.h"
#include "core/comp_geometry.h"
#include "domain/layer.h"
#include "format/codec_lib.h"
#include "format/codec_registry.h"
#include "format/comp_file.h"
#include "format/comp_json.h"
#include "format/comp_png.h"

namespace {

std::string lower_ascii(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string extension_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  if (slash != std::string::npos && dot < slash) return std::string();
  return lower_ascii(path.substr(dot + 1));
}

bool read_file_bytes(const char* path, std::vector<uint8_t>* out, std::string* error) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    if (error != nullptr) *error = std::string("cannot open file for reading: ") + path;
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long length = std::ftell(file);
  if (length < 0) {
    std::fclose(file);
    if (error != nullptr) *error = "cannot determine file size";
    return false;
  }
  std::fseek(file, 0, SEEK_SET);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  const size_t read = bytes.empty() ? 0u : std::fread(bytes.data(), 1u, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    if (error != nullptr) *error = "short read";
    return false;
  }
  if (out != nullptr) *out = std::move(bytes);
  return true;
}

bool write_file_bytes(const char* path, const uint8_t* data, size_t size, std::string* error) {
  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    if (error != nullptr) *error = std::string("cannot open file for writing: ") + path;
    return false;
  }
  const size_t written = size == 0u ? 0u : std::fwrite(data, 1u, size, file);
  std::fclose(file);
  if (written != size) {
    if (error != nullptr) *error = "short write";
    return false;
  }
  return true;
}

/* 本构建能否读 / 写该格式：真实库优先，PNG 另有自研兜底。 */
bool can_decode(comp_image_format format) {
  if (compositor::codec_lib_can_decode(static_cast<uint32_t>(format))) return true;
  return format == COMP_FORMAT_PNG;
}

bool can_encode(comp_image_format format) {
  if (compositor::codec_lib_can_encode(static_cast<uint32_t>(format))) return true;
  return format == COMP_FORMAT_PNG;
}

/* 由登记表生成可解释的失败原因；不再写死「M2 scope」之类会过期的文案。 */
std::string unsupported_reason(comp_image_format format) {
  const compositor::CodecEntry* entry =
      compositor::codec_entry_for(static_cast<uint32_t>(format));
  if (entry == nullptr) return "unknown image format: no codec registered";
  return std::string(entry->name) + " is not available in this build (backend: " +
         entry->backend + "): " + entry->note;
}

/* 写侧不可用（如 PSD 只读）与整体不可用要给出不同原因。 */
std::string unsupported_write_reason(comp_image_format format) {
  const compositor::CodecEntry* entry =
      compositor::codec_entry_for(static_cast<uint32_t>(format));
  if (entry == nullptr) return "unknown image format: no codec registered";
  if (!entry->can_write && entry->can_read) {
    return std::string(entry->name) + " export is unsupported: " + entry->note;
  }
  return unsupported_reason(format);
}

/* 从 options JSON 读质量参数（1..100，缺省 92）；仅 JPEG 等有损格式使用。 */
int quality_from_options(const char* options_json_utf8) {
  const int kDefault = 92;
  if (options_json_utf8 == nullptr || *options_json_utf8 == '\0') return kDefault;
  std::string parse_error;
  const compositor::JsonValue root = compositor::json_parse(options_json_utf8, &parse_error);
  if (!root.is_object()) return kDefault;
  const double value = root.number_or("quality", static_cast<double>(kDefault));
  /* inf/NaN 走 static_cast<int> 是 UB（钳位来不及救）：先挡非有限值。 */
  if (!std::isfinite(value)) return kDefault;
  int quality = static_cast<int>(value + 0.5);
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;
  return quality;
}

/* 统一解码（8 位通道）：真实库优先，PNG 回落到自研实现。失败即视为 IO/内容错误。 */
bool decode_raster_bytes(const uint8_t* data, size_t size, comp_image_format format,
                         compositor::CodecImage* out, std::string* error) {
  /*
   * 浮点来源（Radiance / 将来的 EXR、float TIFF）不许走这条 8 位通道：
   * 这里解出来的像素已经量化到 8 位，白点以上的光被抹平——正是 HDR 素材
   * 唯一要保住的东西。结构上堵死，而不是靠调用点记得先判浮点。
   */
  if (compositor::codec_lib_can_decode_float(static_cast<uint32_t>(format))) {
    if (error != nullptr) *error = "float image source must be decoded through the float channel";
    return false;
  }
  if (compositor::codec_lib_can_decode(static_cast<uint32_t>(format))) {
    return compositor::codec_lib_decode(data, size, static_cast<uint32_t>(format), out, error);
  }
  if (format == COMP_FORMAT_PNG) {
    compositor::PngImage png;
    if (!compositor::png_decode_rgba8(data, size, &png, error) || png.empty()) {
      if (error != nullptr && error->empty()) *error = "PNG decode failed";
      return false;
    }
    out->width = png.width;
    out->height = png.height;
    out->rgba = std::move(png.rgba);
    return true;
  }
  if (error != nullptr) *error = "no decoder for this format";
  return false;
}

/* 从 data[*offset] 起取一行（不含结尾 '\n'，容忍 '\r'）；没有完整行时返回 false。 */
bool next_line(const uint8_t* data, size_t size, size_t* offset, std::string* line) {
  if (*offset >= size) return false;
  size_t end = *offset;
  while (end < size && data[end] != '\n') ++end;
  if (end >= size) return false; /* 末行没有换行：不算完整行，交由解码器判错 */
  size_t length = end - *offset;
  if (length > 0u && data[*offset + length - 1u] == '\r') --length;
  line->assign(reinterpret_cast<const char*>(data + *offset), length);
  *offset = end + 1u;
  return true;
}

/*
 * Radiance 文件头里声明的尺寸（"-Y <h> +X <w>"）。只读头部，不碰像素。
 *
 * 为什么要在交给 stb 之前自己看一眼：stb 会**先**按声明尺寸分配
 * w*h*16 字节的浮点缓冲，之后我们才可能拒绝它——伪造一个巨大尺寸的文件头
 * 即可让尺寸闸形同虚设（浮点通道比 8 位通道危险的地方：libpng 侧有等价于
 * user_limits 的预检，stb 侧什么都没有）。
 *
 * 判定是**尽力而为**的：认不出头就返回 false，仍由 stb 做权威解析——我们
 * 不冒充解析器，也就不可能把合法文件误拒；认出来且超界则当场拒绝。
 */
bool radiance_declared_size(const uint8_t* data, size_t size, int32_t* out_width,
                            int32_t* out_height) {
  if (data == nullptr || out_width == nullptr || out_height == nullptr) return false;
  size_t offset = 0;
  std::string line;
  if (!next_line(data, size, &offset, &line)) return false;
  if (line != "#?RADIANCE" && line != "#?RGBE") return false;

  /* 头部若干行，空行结束；其中必须声明 FORMAT=32-bit_rle_rgbe（与 stb 同款检查）。 */
  bool has_format = false;
  for (;;) {
    if (!next_line(data, size, &offset, &line)) return false;
    if (line.empty()) break;
    if (line == "FORMAT=32-bit_rle_rgbe") has_format = true;
  }
  if (!has_format) return false;

  if (!next_line(data, size, &offset, &line)) return false;
  if (line.compare(0, 3u, "-Y ") != 0) return false;
  const char* cursor = line.c_str() + 3u;
  char* stop = nullptr;
  const long long height = std::strtoll(cursor, &stop, 10);
  if (stop == cursor) return false;
  cursor = stop;
  while (*cursor == ' ') ++cursor;
  if (std::strncmp(cursor, "+X ", 3u) != 0) return false;
  cursor += 3u;
  const long long width = std::strtoll(cursor, &stop, 10);
  if (stop == cursor) return false;
  if (width <= 0 || height <= 0) return false;
  /* 超出 int32 的声明值一律钳到上限：它必然过不了尺寸闸，不需要保真。 */
  const long long kMax = static_cast<long long>(INT32_MAX);
  *out_width = static_cast<int32_t>(width > kMax ? kMax : width);
  *out_height = static_cast<int32_t>(height > kMax ? kMax : height);
  return true;
}

/*
 * 浮点来源的声明尺寸（进解码器之前的预检用）。目前只有 Radiance；
 * C3b 的 EXR 接入后在此补它自己的头解析（attribute 里的 dataWindow）。
 */
bool float_source_declared_size(comp_image_format format, const uint8_t* data, size_t size,
                               int32_t* out_width, int32_t* out_height) {
  if (format == COMP_FORMAT_HDR) return radiance_declared_size(data, size, out_width, out_height);
  return false;
}

/*
 * 一次导入的解码结果（C3 起有两条形态）：
 *   - 8 位：`rgba8`，预乘 RGBA8（老路，PNG/JPEG/PSD 走这里）；
 *   - 宽位深：`wide`，预乘 RGBA16 或 RGBA16F，由 `wide_asset_from_linear` 判决
 *     （浮点来源走这里）。
 * 调用点只搬 `format/stride/pixels` 三样，不必知道它是哪条形态——否则
 * comp_io_import 与 comp_io_import_layer 会各写一遍搬运，两处逻辑迟早分叉。
 */
struct DecodedRaster {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> rgba8;
  compositor::ImageBuffer wide;

  bool wide_depth() const { return !wide.empty(); }
  bool empty() const { return width <= 0 || height <= 0 || (rgba8.empty() && wide.empty()); }
  comp_pixel_format format() const { return wide_depth() ? wide.format : COMP_PIXEL_RGBA8; }
  size_t stride() const {
    return wide_depth() ? wide.stride() : static_cast<size_t>(width) * 4u;
  }
  void* pixels() {
    return wide_depth() ? static_cast<void*>(wide.writable_row(0)) : static_cast<void*>(rgba8.data());
  }
};

/*
 * 统一解码入口：浮点来源先走浮点通道，其余走 8 位通道。
 *
 * 浮点这一支就是上游 ImageImporter.swift:114-117 那条 `.floatComponents` 路径：
 * 解成线性光浮点 -> `DeepLayer.wideAsset` 判决（`wide_asset_from_linear`，
 * peak > 1.001 则为 HDR 图层，否则 16 位图层）。文件**不回落**到 8 位通道：
 * 「浮点解不出就退回 8 位」会让同一文件在不同机器上得到两种画面，
 * 而且悄悄丢掉 HDR 属性——那正是本项目禁止的假装成功。
 */
bool decode_raster_asset(const char* path, comp_image_format format, DecodedRaster* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out is null";
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_file_bytes(path, &bytes, error)) return false;

  if (compositor::codec_lib_can_decode_float(static_cast<uint32_t>(format))) {
    /*
     * 分配前预检：尺寸闸必须在解码器**之前**（见 radiance_declared_size 的说明）。
     * 认不出头时这里放过，由下面的解码器给出它自己的判错——不抢权威判定权。
     */
    int32_t declared_width = 0;
    int32_t declared_height = 0;
    if (float_source_declared_size(format, bytes.data(), bytes.size(), &declared_width,
                                   &declared_height) &&
        !compositor::is_valid_canvas(comp_size{declared_width, declared_height})) {
      if (error != nullptr) *error = "image exceeds maximum canvas size";
      return false;
    }

    compositor::CodecFloatImage floats;
    if (!compositor::codec_lib_decode_float(bytes.data(), bytes.size(),
                                            static_cast<uint32_t>(format), &floats, error) ||
        floats.empty()) {
      if (error != nullptr && error->empty()) *error = "float image decode failed";
      return false;
    }
    /*
     * 尺寸闸（第二道，按**解码器实际报告的**尺寸）：wide_asset_from_linear 会为
     * 整幅图连开两份 (w*h*8) 字节的中间缓冲，伪造尺寸的文件头能把它变成 OOM。
     * 8 位通道侧由 libpng 自己的 is_valid_canvas 守住，浮点通道侧必须在这里守
     * （stb 不设上界）。上面按文件头的预检挡的是「分配前」那一半；这里挡的是
     * 解码器可能报出与声明不同的尺寸那一半。
     */
    if (!compositor::is_valid_canvas(
            comp_size{static_cast<int32_t>(floats.width), static_cast<int32_t>(floats.height)})) {
      if (error != nullptr) *error = "image exceeds maximum canvas size";
      return false;
    }
    if (!compositor::wide_asset_from_linear(floats.rgba.data(), floats.width, floats.height,
                                            &out->wide)) {
      if (error != nullptr) *error = "wide asset conversion failed";
      return false;
    }
    out->width = floats.width;
    out->height = floats.height;
    return true;
  }

  compositor::CodecImage image;
  if (!decode_raster_bytes(bytes.data(), bytes.size(), format, &image, error) || image.empty()) {
    if (error != nullptr && error->empty()) *error = "image decode failed";
    return false;
  }
  out->width = image.width;
  out->height = image.height;
  out->rgba8 = std::move(image.rgba);
  return true;
}

/* 把解码结果搬进 comp_image（两条形态共用；不复制像素，只交出缓冲区首址）。 */
void fill_image_from_raster(DecodedRaster* raster, comp_image* image) {
  image->width = raster->width;
  image->height = raster->height;
  image->format = raster->format();
  image->stride = raster->stride();
  image->pixels = raster->pixels();
}

/* 统一编码：真实库优先，PNG 回落到自研实现。 */
bool encode_raster_file(const uint8_t* canvas, int32_t width, int32_t height,
                        comp_image_format format, int quality, std::vector<uint8_t>* out,
                        std::string* error) {
  if (compositor::codec_lib_can_encode(static_cast<uint32_t>(format))) {
    if (format == COMP_FORMAT_PNG) compositor::codec_lib_set_png_compression(6);
    return compositor::codec_lib_encode(canvas, width, height, static_cast<uint32_t>(format),
                                       quality, out, error);
  }
  if (format == COMP_FORMAT_PNG) {
    return compositor::png_encode_rgba8(canvas, width, height, out, error);
  }
  if (error != nullptr) *error = "no encoder for this format";
  return false;
}

}  // namespace

extern "C" {

comp_image_format comp_io_format_from_path(const char* path_utf8) {
  if (path_utf8 == nullptr) return COMP_FORMAT_UNKNOWN;
  const std::string ext = extension_of(path_utf8);
  if (ext == "png") return COMP_FORMAT_PNG;
  if (ext == "jpg" || ext == "jpeg") return COMP_FORMAT_JPEG;
  if (ext == "tif" || ext == "tiff") return COMP_FORMAT_TIFF;
  if (ext == "heic" || ext == "heif") return COMP_FORMAT_HEIF;
  if (ext == "webp") return COMP_FORMAT_WEBP;
  if (ext == "exr") return COMP_FORMAT_EXR;
  if (ext == "psd") return COMP_FORMAT_PSD;
  /* C3：Radiance。`.pic` 是它的另一个常用扩展名（同一容器）。 */
  if (ext == "hdr" || ext == "pic") return COMP_FORMAT_HDR;
  return COMP_FORMAT_UNKNOWN;
}

comp_result comp_io_import(comp_engine engine, const char* path_utf8, comp_document* out_document) {
  if (out_document == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  compositor::Engine* e = compositor::abi::as_engine(engine);
  COMP_ABI_TRY(e)
  if (e == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  if (path_utf8 == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_INVALID_ARGUMENT, "path is null");
  }

  const comp_image_format format = comp_io_format_from_path(path_utf8);
  if (!can_decode(format)) {
    const std::string reason = unsupported_reason(format);
    return compositor::abi::fail(e, COMP_ERR_UNSUPPORTED, reason.c_str());
  }

  DecodedRaster raster;
  std::string error;
  if (!decode_raster_asset(path_utf8, format, &raster, &error) || raster.empty()) {
    return compositor::abi::fail(e, COMP_ERR_IO,
                                 error.empty() ? "image decode failed" : error.c_str());
  }

  /* 导入语义：画布尺寸 = 图像尺寸，像素落在背景层（不额外叠一层）。
   * HDR 素材（浮点来源且过 1.001 闸）在这里以 COMP_PIXEL_RGBA16F 入库，
   * 它的 8 位工作副本由合成侧按 hdr_to_rgba8 现取（C1）。 */
  comp_document_desc desc{};
  desc.width = raster.width;
  desc.height = raster.height;
  desc.dpi = 72.0f;
  desc.color_space = "sRGB";
  comp_document document = nullptr;
  comp_result result = comp_document_create(engine, &desc, &document);
  if (result != COMP_OK) return result;

  comp_layer_id background = 0;
  size_t layer_count = 0;
  /* P0-2：拆开失败分支。comp_layer_list 的错误码必须原样透传，不能再套 fail()，
     否则 last_error 会被覆盖成与真实原因无关的文案。 */
  const comp_result list_result = comp_layer_list(document, &background, 1u, &layer_count);
  if (list_result != COMP_OK) {
    comp_document_destroy(document);
    return list_result;
  }
  if (layer_count < 1u) {
    /* 新建文档在语义上必然带一个背景层；缺失说明文档状态与预期不一致。 */
    comp_document_destroy(document);
    return compositor::abi::fail(e, COMP_ERR_INTERNAL, "background layer missing");
  }

  comp_image pixels{};
  fill_image_from_raster(&raster, &pixels);
  const comp_result pixels_result = comp_layer_set_pixels(document, background, &pixels);
  if (pixels_result != COMP_OK) {
    comp_document_destroy(document);
    return compositor::abi::fail(e, pixels_result, "imported pixels were not applied");
  }
  *out_document = document;
  return COMP_OK;
  COMP_ABI_CATCH(e)
}

comp_result comp_io_import_layer(comp_document document, const char* path_utf8,
                                comp_layer_id* out_id) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_id == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_id is null");
  }
  if (path_utf8 == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "path is null");
  }

  const comp_image_format format = comp_io_format_from_path(path_utf8);
  if (!can_decode(format)) {
    const std::string reason = unsupported_reason(format);
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, reason.c_str());
  }

  DecodedRaster raster;
  std::string error;
  if (!decode_raster_asset(path_utf8, format, &raster, &error) || raster.empty()) {
    return compositor::abi::fail(engine, COMP_ERR_IO,
                                 error.empty() ? "image decode failed" : error.c_str());
  }

  /* P1-4：整段放进一个**显式事务**，失败路径走 txn_abort 而非「再删一层」。
     这样撤销栈只会留 0 步或 1 步，不会留下 add + remove 的两步残影
     （否则撤销一次会把那个空图层复活）。 */
  std::string txn_error;
  const comp_result begin = doc->txn_begin("Import Layer", &txn_error);
  if (begin != COMP_OK) return compositor::abi::fail(engine, begin, txn_error);

  comp_layer_desc layer_desc{};
  layer_desc.name = "Imported image";
  layer_desc.opacity = 1.0f;
  comp_layer_id id = 0;
  comp_result result = COMP_OK;

  /* 内层护栏：事务已开启后若抛异常，必须先 abort 再离开，否则文档会卡在 active 事务。
     txn_abort 自身也包在 try 内，绝不让异常再越出 extern "C"。 */
  try {
    result = comp_layer_add(document, &layer_desc, &id);
    if (result == COMP_OK) {
      comp_image pixels{};
      fill_image_from_raster(&raster, &pixels);
      result = comp_layer_set_pixels(document, id, &pixels);
    }
  } catch (...) {
    try { doc->txn_abort(&txn_error); } catch (...) {}
    throw;
  }

  if (result != COMP_OK) {
    std::string abort_error;
    doc->txn_abort(&abort_error);
    return compositor::abi::fail(engine, result, "imported layer was rolled back");
  }
  const comp_result commit = doc->txn_commit(&txn_error);
  if (commit != COMP_OK) return compositor::abi::fail(engine, commit, txn_error);
  *out_id = id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_io_export(comp_document document, const char* path_utf8,
                          comp_image_format format, const char* options_json_utf8) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  (void)doc;
  if (path_utf8 == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "path is null");
  }
  if (!can_encode(format)) {
    const std::string reason = unsupported_write_reason(format);
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, reason.c_str());
  }

  comp_size size{};
  comp_result result = comp_document_size(document, &size);
  if (result != COMP_OK) return result;
  if (size.width <= 0 || size.height <= 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "document has no canvas size");
  }

  const size_t stride = static_cast<size_t>(size.width) * 4u;
  std::vector<uint8_t> canvas(static_cast<size_t>(size.height) * stride, 0u);
  result = comp_render_flatten(document, canvas.data(), COMP_PIXEL_RGBA8, stride);
  if (result != COMP_OK) return result;

  std::vector<uint8_t> encoded;
  std::string error;
  const int quality = quality_from_options(options_json_utf8);
  if (!encode_raster_file(canvas.data(), size.width, size.height, format, quality, &encoded,
                          &error)) {
    return compositor::abi::fail(engine, COMP_ERR_IO, error.c_str());
  }
  if (!write_file_bytes(path_utf8, encoded.data(), encoded.size(), &error)) {
    return compositor::abi::fail(engine, COMP_ERR_IO, error.c_str());
  }
  return COMP_OK;
  COMP_ABI_CATCH(engine)
}

comp_result comp_io_save_project(comp_document document, const char* comp_path_utf8) {
  return comp_document_save(document, comp_path_utf8);
}

comp_result comp_io_load_project(comp_engine engine, const char* comp_path_utf8,
                                comp_document* out_document) {
  return comp_document_load(engine, comp_path_utf8, out_document);
}

comp_result comp_io_probe_project(const char* comp_path_utf8, comp_project_info* out_info) {
  if (comp_path_utf8 == nullptr || out_info == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  /* N3：本入口没有 engine 句柄（fail(nullptr, …) 只回错误码，不写 last_error），
     但 probe_project 会整篇读入 manifest，故仍需护栏兜住 bad_alloc / 其它异常。
     manifest 的 4 MiB 上界在 probe_project 内与 load/save 对齐。 */
  COMP_ABI_TRY(nullptr)
  compositor::ProjectInfo info;
  std::string error;
  if (!compositor::probe_project(comp_path_utf8, &info, &error)) return COMP_ERR_IO;

  out_info->width = info.width;
  out_info->height = info.height;
  out_info->version = info.version;
  out_info->layer_count = info.layer_count;
  std::snprintf(out_info->format, sizeof(out_info->format), "%s", info.format.c_str());
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_io_codec_support(comp_image_format format, comp_codec_state* out_state,
                                 char* note_utf8, size_t note_capacity, size_t* out_note_size) {
  const compositor::CodecEntry* entry =
      compositor::codec_entry_for(static_cast<uint32_t>(format));
  const char* note = entry != nullptr ? entry->note : "unknown image format: no codec registered";

  if (out_state != nullptr) {
    comp_codec_state state = COMP_CODEC_UNAVAILABLE;
    if (entry != nullptr) {
      if (entry->state == static_cast<uint32_t>(compositor::kCodecBuiltIn)) {
        state = COMP_CODEC_BUILTIN;
      } else if (entry->state == static_cast<uint32_t>(compositor::kCodecVendored)) {
        state = COMP_CODEC_VENDORED;
      }
    }
    *out_state = state;
  }
  const size_t need = std::strlen(note) + 1u; /* 含结尾 '\0' */
  if (out_note_size != nullptr) *out_note_size = need;
  if (note_utf8 != nullptr && note_capacity > 0u) {
    const size_t copy = need < note_capacity ? need : note_capacity;
    std::memcpy(note_utf8, note, copy);
    note_utf8[copy - 1u] = '\0';
  }
  return entry != nullptr ? COMP_OK : COMP_ERR_NOT_FOUND;
}

} /* extern "C" */
