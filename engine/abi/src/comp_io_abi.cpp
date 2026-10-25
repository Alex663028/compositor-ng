// AI生成
/*
 * comp_io_abi.cpp — comp_io.h 的 C ABI 实现。
 *
 * 分工：
 *   .comp 工程读写     自有格式（无第三方依赖），见 comp_file / comp_png；
 *   外部图像导入/导出  由 codec_lib 分派到 vendor 的真实库（PNG=libpng+zlib，
 *                      JPEG=stb_image/stb_image_write，PSD=stb_image 只读）；
 *                      第三方缺失时 PNG 回落到自研 comp_png，其余如实 UNSUPPORTED。
 *   浮点来源（C3/C3b） Radiance .hdr/.pic 走 stb_image 的浮点通道，EXR 走树内
 *                      自研 scanline 读取器；两者解出的都是**线性光浮点**；进
 *                      HDR 图层还是 16 位图层由 wideAsset 判决（peak > 1.001），
 *                      与上游 ImageImporter.swift:114 同口径。
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
#include "format/comp_uuid.h"
#include "format/exr_codec.h"
#include "format/comp_json.h"
#include "format/comp_png.h"
#include "format/psd_codec.h"
#include "imaging/DeepPixels.h"

#include <memory>
#include <unordered_map>

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

/*
 * 写侧不可用与整体不可用要给出不同原因。
 *
 * 这里说的是**拍平写**（comp_io_export：一次合成、一张图）。分层格式还有另一条
 * 写入口（comp_io_export_document），所以文案必须点明 "flattened"——否则像 PSD
 * 这种「分层能写、拍平不能写」的格式，用户会读到「PSD 不能导出」这句错话。
 */
std::string unsupported_write_reason(comp_image_format format) {
  const compositor::CodecEntry* entry =
      compositor::codec_entry_for(static_cast<uint32_t>(format));
  if (entry == nullptr) return "unknown image format: no codec registered";
  if (!entry->can_write && entry->can_read) {
    return std::string(entry->name) + " flattened export is unsupported: " + entry->note;
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
 * 浮点来源的声明尺寸（进解码器之前的预检用）：Radiance 读 "-Y <h> +X <w>"，
 * EXR 读头部 attribute 里的 dataWindow（exr_declared_size 只解析头部，不分配像素）。
 */
bool float_source_declared_size(comp_image_format format, const uint8_t* data, size_t size,
                               int32_t* out_width, int32_t* out_height) {
  if (format == COMP_FORMAT_HDR) return radiance_declared_size(data, size, out_width, out_height);
  if (format == COMP_FORMAT_EXR) {
    /* 原因由后面真正的解码器给出（同一份头解析），这里只需要尺寸。 */
    std::string detail;
    return compositor::exr_declared_size(data, size, out_width, out_height, &detail);
  }
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

/* ------------------------------------------------- PSD 整篇文档（M1） */

comp_result psd_status_to_result(compositor::PsdStatus status) {
  switch (status) {
    case compositor::PsdStatus::kOk: return COMP_OK;
    case compositor::PsdStatus::kUnsupported: return COMP_ERR_UNSUPPORTED;
    case compositor::PsdStatus::kIo:
    case compositor::PsdStatus::kDecode: return COMP_ERR_IO;
  }
  return COMP_ERR_INTERNAL;
}

/*
 * PSD -> 引擎文档。直接建 state.layers（与 comp_file 的 load_project 同路），
 * 不走 comp_layer_add 的逐层事务：导入是「新建文档」而不是对既有文档的一步编辑，
 * 每层一个事务只会让撤销栈长出一串与用户操作无关的步骤。
 *
 * 口径：
 *   - 表序即堆叠序（index 0 = 最底层），与 PSD 的图层记录序一致；
 *   - `parent`/`clipping_source` 在 psd_codec 里都是**表内下标**，这里换成内部 id；
 *   - 组的「穿透」= !isolated，PSD 的 lsct 已把 pass / 非 pass 读成 pass_through；
 *   - 像素与蒙版都是预乘、RGBA8 或 RGBA16，蒙版在图层像素空间——与引擎口径相同，
 *     直接转移所有权，不做任何换算。
 */
compositor::PsdStatus import_psd_document(compositor::Engine* engine, const uint8_t* data,
                                          size_t size, comp_document* out_document,
                                          std::string* error) {
  compositor::PsdDocument psd;
  const compositor::PsdStatus status =
      compositor::psd_decode_document(data, size, &psd, error);
  if (status != compositor::PsdStatus::kOk) return status;

  /*
   * 没有图层记录的 PSD（拍平文件）不是「0 层的文档」：它的内容全在那份我们不读的
   * 合并图里。交给分层入口会得到一个空文档——那是最坏的失败形态（看起来成功）。
   */
  if (psd.layers.empty()) {
    if (error != nullptr) {
      *error = "PSD: 文件没有图层记录（已拍平），请用 comp_io_import 导入合成图";
    }
    return compositor::PsdStatus::kUnsupported;
  }

  auto doc = std::unique_ptr<compositor::Document, void (*)(compositor::Document*)>(
      compositor::Document::create(engine, psd.width, psd.height, 72.0f, "sRGB"),
      compositor::Document::destroy);
  compositor::DocumentState& state = doc->state();
  state.layers.clear();
  /* 与 comp_document_create 一致：文档一创建就有 documentID（.comp 包的文件名派生自它）。 */
  state.document_id = compositor::uuid_v4_new();

  std::vector<comp_layer_id> ids(psd.layers.size(), 0);
  comp_layer_id next_id = 1;
  state.layers.reserve(psd.layers.size());
  for (size_t i = 0; i < psd.layers.size(); ++i) {
    const compositor::PsdLayerData& source = psd.layers[i];
    compositor::Layer layer;
    layer.id = next_id++;
    ids[i] = layer.id;
    layer.name = source.name.empty() ? "Layer " + std::to_string(i + 1u) : source.name;
    layer.blend = source.blend;
    float opacity = static_cast<float>(source.opacity) / 255.0f;
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    layer.opacity = opacity;
    layer.visible = source.visible;
    layer.is_group = source.is_group;
    /* 穿透组 = 不隔离。PSD 的「非 pass 文件夹记录」就是一个隔离组。 */
    layer.isolated = source.is_group && !source.pass_through;
    layer.transform = source.bounds;
    layer.pixels = std::move(source.pixels);
    layer.mask = std::move(source.mask);
    state.layers.push_back(std::move(layer));
  }

  /* 第二趟：跨图层引用（父级 / 剪贴来源）——此时所有 id 都已分配。 */
  for (size_t i = 0; i < psd.layers.size(); ++i) {
    const compositor::PsdLayerData& source = psd.layers[i];
    compositor::Layer& layer = state.layers[i];
    if (source.parent >= 0) {
      if (static_cast<size_t>(source.parent) >= ids.size()) {
        if (error != nullptr) *error = "PSD: 图层父级下标越界";
        return compositor::PsdStatus::kIo;
      }
      layer.parent = ids[static_cast<size_t>(source.parent)];
    }
    if (source.clipping_source >= 0) {
      if (static_cast<size_t>(source.clipping_source) >= ids.size()) {
        if (error != nullptr) *error = "PSD: 剪贴来源下标越界";
        return compositor::PsdStatus::kIo;
      }
      layer.clipping_source = ids[static_cast<size_t>(source.clipping_source)];
    }
  }

  for (compositor::Layer& layer : state.layers) layer.children.clear();
  for (compositor::Layer& layer : state.layers) {
    if (layer.parent == 0) continue;
    compositor::Layer* parent = doc->find(layer.parent);
    if (parent != nullptr && parent->is_group) parent->children.push_back(layer.id);
  }

  state.next_layer_id = next_id;
  /* 最上层为活动层（Photoshop 打开文件后的默认选中项）。 */
  state.active_layer = ids.back();
  doc->touch_all_layers();
  doc->mark_dirty(false);
  doc->history().clear();
  /*
   * 与 comp_document_create / comp_document_load 同一步：先交给引擎登记，
   * 再把所有权转成不透明句柄。登记失败（bad_alloc）时 unique_ptr 仍会 destroy，
   * 不会留下既没 attach 也没释放的文档。
   */
  engine->attach_document(doc.get());
  *out_document = reinterpret_cast<comp_document>(doc.release());
  return compositor::PsdStatus::kOk;
}

/* 8 位 -> 16 位无损扩位（v*257），PSD 一个文件只有一种位深。 */
compositor::ImageBuffer widen_rgba16(const compositor::ImageBuffer& source) {
  compositor::ImageBuffer out;
  out.resize(source.width, source.height, COMP_PIXEL_RGBA16);
  const size_t samples = static_cast<size_t>(source.width) * 4u;
  for (int32_t y = 0; y < source.height; ++y) {
    const uint8_t* in = source.row(y);
    uint16_t* dst = reinterpret_cast<uint16_t*>(out.writable_row(y));
    for (size_t x = 0; x < samples; ++x) dst[x] = static_cast<uint16_t>(in[x]) * 257u;
  }
  return out;
}

/*
 * 引擎文档 -> PSD。**只做能忠实表示的那一部分**：任何会被写成「另一个样子」的
 * 属性都在这里明说失败，由 ABI 层翻成 COMP_ERR_UNSUPPORTED。
 */
bool build_psd_document(const compositor::Document& doc, compositor::PsdDocument* psd,
                        std::string* error) {
  const compositor::DocumentState& state = doc.state();
  psd->width = state.width;
  psd->height = state.height;
  psd->depth = 8;
  for (const compositor::Layer& layer : state.layers) {
    if (layer.pixels.empty()) continue;
    if (layer.pixels.format == COMP_PIXEL_RGBA16F) {
      if (error != nullptr) {
        *error = "PSD: 图层 '" + layer.name +
                 "' 是 HDR（半精度）图层；PSD 子集只支持 8/16 位整数通道，"
                 "写成浮点会丢掉白点以上的高光";
      }
      return false;
    }
    if (layer.pixels.format == COMP_PIXEL_RGBA16) psd->depth = 16;
  }

  std::unordered_map<comp_layer_id, int32_t> index_of;
  index_of.reserve(state.layers.size() * 2u);
  for (size_t i = 0; i < state.layers.size(); ++i) {
    index_of[state.layers[i].id] = static_cast<int32_t>(i);
  }

  psd->layers.resize(state.layers.size());
  for (size_t i = 0; i < state.layers.size(); ++i) {
    const compositor::Layer& layer = state.layers[i];
    compositor::PsdLayerData& out = psd->layers[i];
    out.name = layer.name;
    out.blend = layer.blend;
    const float opacity = layer.opacity < 0.0f ? 0.0f : (layer.opacity > 1.0f ? 1.0f : layer.opacity);
    out.opacity = static_cast<uint8_t>(std::lround(opacity * 255.0f));
    out.visible = layer.visible;
    out.is_group = layer.is_group;
    out.pass_through = layer.is_group && !layer.isolated;

    if (layer.parent != 0) {
      const auto parent = index_of.find(layer.parent);
      if (parent == index_of.end()) {
        if (error != nullptr) *error = "PSD: 图层 '" + layer.name + "' 的父级不存在";
        return false;
      }
      out.parent = parent->second;
    } else {
      out.parent = -1;
    }
    out.clipping_source = -1;

    if (layer.is_group) {
      /* 穿透组在 PSD 里没有自己的混合模式（lsct 的 blend 键永远是 pass）：
       * 写别的键等于把一个「穿透 + Multiply」的组读回来变成隔离组，画面会变。 */
      if (out.pass_through && layer.blend != COMP_BLEND_NORMAL) {
        if (error != nullptr) {
          *error = "PSD: 组 '" + layer.name +
                   "' 是穿透组却带着自己的混合模式；PSD 的穿透组不支持混合模式";
        }
        return false;
      }
      if (!layer.pixels.empty() || !layer.mask.empty()) {
        if (error != nullptr) *error = "PSD: 组 '" + layer.name + "' 带像素或蒙版，无法写成 PSD";
        return false;
      }
      continue;
    }

    /* ---- 像素层：先过一遍「PSD 表示不了」的属性（宁可失败，不可改名换姓地写） ---- */
    if (layer.has_text()) {
      if (error != nullptr) *error = "PSD: 文字图层 '" + layer.name + "' 无法写成 PSD（PSD 子集不含文字图层）";
      return false;
    }
    /* 智能对象在 PSD 里是 Placed Layer：本子集只写像素层，写成一个普通像素层
     * 就等于静默丢掉「原始像素 + 滤镜栈」这份可编辑性。宁可失败。 */
    if (layer.has_smart_object()) {
      if (error != nullptr) {
        *error = "PSD: 智能对象图层 '" + layer.name + "' 无法写成 PSD（PSD 子集不含智能对象）";
      }
      return false;
    }
    if (!layer.adjustments.empty()) {
      if (error != nullptr) *error = "PSD: 图层 '" + layer.name + "' 带调整图层参数，无法写成 PSD";
      return false;
    }
    if (!layer.effects.empty()) {
      if (error != nullptr) *error = "PSD: 图层 '" + layer.name + "' 带图层效果，无法写成 PSD";
      return false;
    }
    if (layer.rotation != 0.0f || layer.flip_x || layer.flip_y) {
      if (error != nullptr) *error = "PSD: 图层 '" + layer.name + "' 有旋转/翻转，无法写成 PSD";
      return false;
    }

    comp_rect bounds = layer.transform;
    if (bounds.w <= 0 || bounds.h <= 0) {
      bounds = comp_rect{0, 0, layer.pixels.width, layer.pixels.height};
    } else if (!layer.pixels.empty() &&
               (layer.pixels.width != bounds.w || layer.pixels.height != bounds.h)) {
      if (error != nullptr) {
        *error = "PSD: 图层 '" + layer.name +
                 "' 的变换尺寸与像素尺寸不一致（缩放图层），PSD 子集不能表示";
      }
      return false;
    }
    out.bounds = bounds;

    if (!layer.pixels.empty() && layer.pixels.format == COMP_PIXEL_RGBA16) {
      out.pixels = layer.pixels;
    } else if (!layer.pixels.empty() && psd->depth == 16) {
      out.pixels = widen_rgba16(layer.pixels);
    } else {
      out.pixels = layer.pixels;
    }
    out.mask = layer.mask;
  }

  /* ---- 剪贴关系：PSD 用「相邻上方 + clipping 标志」表达，来源必须是正下方最近的
   *      非剪贴同级层。engine 允许指向更远的层，那种关系写出去会读回另一个意思。 ---- */
  for (size_t i = 0; i < state.layers.size(); ++i) {
    const compositor::Layer& layer = state.layers[i];
    if (layer.clipping_source == 0) continue;
    if (layer.is_group) {
      if (error != nullptr) *error = "PSD: 组 '" + layer.name + "' 不能被剪贴";
      return false;
    }
    const auto source = index_of.find(layer.clipping_source);
    if (source == index_of.end()) {
      if (error != nullptr) *error = "PSD: 图层 '" + layer.name + "' 的剪贴来源不存在";
      return false;
    }
    const int32_t source_index = source->second;
    const compositor::Layer& base = state.layers[static_cast<size_t>(source_index)];
    bool adjacent = source_index < static_cast<int32_t>(i) && !base.is_group &&
                    base.parent == layer.parent;
    for (int32_t j = static_cast<int32_t>(i) - 1; adjacent && j >= 0; --j) {
      const compositor::Layer& between = state.layers[static_cast<size_t>(j)];
      if (between.parent != layer.parent) continue;
      if (j == source_index) break; /* 走到了基底：关系成立 */
      if (between.clipping_source != layer.clipping_source) adjacent = false;
    }
    if (!adjacent) {
      if (error != nullptr) {
        *error = "PSD: 图层 '" + layer.name +
                 "' 的剪贴来源不是正下方最近的非剪贴同级层，PSD 无法表达该关系";
      }
      return false;
    }
    psd->layers[i].clipping = true;
    psd->layers[i].clipping_source = source_index;
  }
  return true;
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

  /*
   * C4：EXR 是浮点容器，导出必须拿**合成表面本身**，不能先走 8 位栅格。
   * 先降到 8 位再写 EXR 会把白点以上的光截掉——文件写得出来、内容是错的，比失败更坏。
   *
   * 这里按 CPU 后端支持的 RGBA16F 表面 flatten（8 位文档由后端用 rgba8_to_hdr
   * 扩成 half，不额外丢信息），再用与 `.comp` 资产同一对曲线做
   * 预乘 extended sRGB -> 线性光直通 alpha，最后写 HALF 通道。
   * 若当前后端不支持 half 目标，flatten 会带名字失败，绝不悄悄降回 8 位。
   */
  if (format == COMP_FORMAT_EXR) {
    const size_t half_stride = static_cast<size_t>(size.width) * 8u;
    std::vector<uint8_t> half(static_cast<size_t>(size.height) * half_stride, 0u);
    result = comp_render_flatten(document, half.data(), COMP_PIXEL_RGBA16F, half_stride);
    if (result != COMP_OK) return result;

    const size_t pixels = static_cast<size_t>(size.width) * static_cast<size_t>(size.height);
    const size_t float_stride = static_cast<size_t>(size.width) * 4u * sizeof(float);
    std::vector<float> linear(pixels * 4u);
    deep_hdr_to_linear(reinterpret_cast<const uint16_t*>(half.data()), half_stride, linear.data(),
                       float_stride, static_cast<size_t>(size.width),
                       static_cast<size_t>(size.height));

    std::vector<uint8_t> encoded;
    std::string error;
    if (!compositor::exr_encode_float(linear.data(), size.width, size.height,
                                      compositor::kExrSampleHalf, &encoded, &error)) {
      return compositor::abi::fail(engine, COMP_ERR_IO, error);
    }
    if (!write_file_bytes(path_utf8, encoded.data(), encoded.size(), &error)) {
      return compositor::abi::fail(engine, COMP_ERR_IO, error);
    }
    return COMP_OK;
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

comp_result comp_io_import_document(comp_engine engine, const char* path_utf8,
                                    comp_document* out_document) {
  if (out_document == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_document = nullptr;
  compositor::Engine* e = compositor::abi::as_engine(engine);
  COMP_ABI_TRY(e)
  if (e == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  if (path_utf8 == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_INVALID_ARGUMENT, "path is null");
  }

  const comp_image_format format = comp_io_format_from_path(path_utf8);
  if (format != COMP_FORMAT_PSD) {
    /* 单图层格式没有「整篇」语义：与 comp_io_import 完全同义（不做第二套解码）。 */
    return comp_io_import(engine, path_utf8, out_document);
  }

  std::vector<uint8_t> bytes;
  std::string error;
  if (!read_file_bytes(path_utf8, &bytes, &error)) {
    return compositor::abi::fail(e, COMP_ERR_IO, error.c_str());
  }
  const compositor::PsdStatus status =
      import_psd_document(e, bytes.data(), bytes.size(), out_document, &error);
  if (status != compositor::PsdStatus::kOk) {
    /* 失败时不留半成品文档：import_psd_document 只在最后一步交出所有权。 */
    *out_document = nullptr;
    const comp_result failure = psd_status_to_result(status);
    return compositor::abi::fail(e, failure,
                                 error.empty() ? "PSD document import failed" : error.c_str());
  }
  return COMP_OK;
  COMP_ABI_CATCH(e)
}

comp_result comp_io_export_document(comp_document document, const char* path_utf8,
                                    comp_image_format format, const char* options_json_utf8) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (path_utf8 == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "path is null");
  }
  if (format != COMP_FORMAT_PSD) {
    /* 非分层格式：与 comp_io_export 同义（一次合成、一张图）。 */
    return comp_io_export(document, path_utf8, format, options_json_utf8);
  }

  compositor::PsdDocument psd;
  std::string error;
  if (!build_psd_document(*doc, &psd, &error)) {
    /* 全是「这个文档 PSD 表示不了」：具名原因走 last_error，码用 UNSUPPORTED。 */
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, error.c_str());
  }
  if (psd.width <= 0 || psd.height <= 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "document has no canvas size");
  }

  /*
   * 合并图像（PSD 末尾那份预览）= 当前合成结果，8 位 RGBA；16 位文档由编码器按
   * v*257 扩位。它是预览，布局里的图层数据才是权威——但也不能留一张全透明的
   * 假图，否则没开图层的查看器看到的是一张空图。
   */
  const size_t stride = static_cast<size_t>(psd.width) * 4u;
  std::vector<uint8_t> merged(static_cast<size_t>(psd.height) * stride, 0u);
  const comp_result flatten =
      comp_render_flatten(document, merged.data(), COMP_PIXEL_RGBA8, stride);
  if (flatten != COMP_OK) return flatten;

  std::vector<uint8_t> encoded;
  if (!compositor::psd_encode_document(psd, merged.data(), COMP_PIXEL_RGBA8, stride, &encoded,
                                       &error)) {
    return compositor::abi::fail(engine, COMP_ERR_IO, error.c_str());
  }
  if (!write_file_bytes(path_utf8, encoded.data(), encoded.size(), &error)) {
    return compositor::abi::fail(engine, COMP_ERR_IO, error.c_str());
  }
  return COMP_OK;
  COMP_ABI_CATCH(engine)
}

} /* extern "C" */
