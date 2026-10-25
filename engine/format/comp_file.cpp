// AI生成
#include "format/comp_file.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "compositing/blend.h"
#include "compositing/resample.h"
#include "core/comp_geometry.h"
#include "domain/document.h"
#include "domain/guide.h"
#include "domain/selection.h"
#include "domain/selection_channel.h"
#include "domain/text_layer.h"
#include "engine.h"
#include "format/comp_json.h"
#include "format/comp_png.h"
#include "format/comp_uuid.h"
#include "format/exr_codec.h"
#include "imaging/DeepPixels.h"

namespace fs = std::filesystem;

namespace compositor {

namespace {

constexpr uint32_t kManifestVersion = 13;
constexpr const char* kFormatId = "com.compositor.project";
constexpr const char* kColorSpace = "sRGB";
constexpr int64_t kMaxLayers = 10000;
constexpr int64_t kMaxManifestBytes = 4 * 1024 * 1024;

/* transform 边界不再在此各写一份：改读 compositor::limits 单一真源
 * （engine/core/comp_geometry.h），与 ABI 写入侧的 is_valid_transform 同源。
 * 此前本文件的两份常量与 resample.cpp 的 30000/1e8 互不相识，是 AUDIT4 §2
 * 那条「三处口径不一致」的根源。 */
constexpr int64_t kMaxTransformSize = compositor::limits::kMaxTransformSize;
constexpr int64_t kMaxTransformOrigin = compositor::limits::kMaxTransformOrigin;

/* ------------------------------------------------------------ 文件助手 */

bool write_text_file(const fs::path& path, const std::string& text, std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error != nullptr) *error = "cannot open for write: " + path.u8string();
    return false;
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    if (error != nullptr) *error = "write failed: " + path.u8string();
    return false;
  }
  return true;
}

bool read_text_file(const fs::path& path, std::string* text, std::string* error) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (error != nullptr) *error = "cannot open for read: " + path.u8string();
    return false;
  }
  /*
   * 先取大小再读：manifest 有 4 MiB 上界（load / probe 都查）。若像从前那样
   * 用 rdbuf 流式读入，一个数 GB 的假 manifest 会先把进程内存吃光才走到
   * 大小检查——检查形同虚设。ate 模式 tellg 即字节数，超限在读前就拒。
   */
  const std::streamoff size = in.tellg();
  if (size < 0) {
    if (error != nullptr) *error = "cannot stat: " + path.u8string();
    return false;
  }
  if (static_cast<int64_t>(size) > kMaxManifestBytes) {
    if (error != nullptr) *error = "manifest exceeds 4 MiB";
    return false;
  }
  text->resize(static_cast<size_t>(size));
  in.seekg(0, std::ios::beg);
  if (size > 0) {
    in.read(text->data(), size);
    if (!in) {
      if (error != nullptr) *error = "read failed: " + path.u8string();
      return false;
    }
  }
  return true;
}

bool write_bytes_file(const fs::path& path, const void* data, size_t bytes, std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error != nullptr) *error = "cannot open for write: " + path.u8string();
    return false;
  }
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  if (!out) {
    if (error != nullptr) *error = "write failed: " + path.u8string();
    return false;
  }
  return true;
}

bool read_bytes_file(const fs::path& path, std::vector<uint8_t>* bytes, std::string* error) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (error != nullptr) *error = "cannot open for read: " + path.u8string();
    return false;
  }
  const std::streamoff size = in.tellg();
  if (size < 0) {
    if (error != nullptr) *error = "cannot stat: " + path.u8string();
    return false;
  }
  bytes->resize(static_cast<size_t>(size));
  in.seekg(0, std::ios::beg);
  if (size > 0) {
    in.read(reinterpret_cast<char*>(bytes->data()), size);
    if (!in) {
      if (error != nullptr) *error = "read failed: " + path.u8string();
      return false;
    }
  }
  return true;
}

/* ------------------------------------------------------- 资产路径与命名 */

/* 上游把图层资产放在 images/ 子目录，且文件名必须由该图层自己的 uuid 构成。
 *
 * 像素资产的扩展名由**像素格式**决定（不是「有什么编码器就用什么」）：
 *   RGBA8      -> .png  自研 PNG 解码器，无需第三方；
 *   RGBA16/16F -> .exr  树内自研 scanline 编解码器，像素口径是线性光直通 alpha。
 * 二者不互换：PNG 存不了 16 位预乘的精度，EXR 的浮点口径也不是 8 位层要的。
 * 蒙版始终是 8 位灰度 PNG（掩码是覆盖率，不是宽位深颜色）。 */
std::string pixel_file_name(const std::string& uuid, comp_pixel_format format) {
  if (format == COMP_PIXEL_RGBA16 || format == COMP_PIXEL_RGBA16F) return uuid + ".exr";
  return uuid + ".png";
}
std::string mask_file_name(const std::string& uuid) { return uuid + ".mask.png"; }

/* 选区通道的资产名**由文档 ID + 数组下标派生**，manifest 里不存路径：
 * 载入时不接受任何来自文件的路径字符串，也就不存在「manifest 指到 images/
 * 之外」这类输入。重排/删除通道会让后续下标前移，因此保存时必须整篇重写
 * 全部通道文件（下面就是这样做的），不能只补写新增的那一个。 */
std::string channel_file_name(const std::string& document_id, size_t index) {
  return document_id + ".channel" + std::to_string(index) + ".png";
}

bool ends_with(const std::string& text, const char* suffix) {
  const size_t length = std::strlen(suffix);
  return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

fs::path manifest_path_of(const fs::path& root) { return root / "manifest.json"; }
fs::path images_dir_of(const fs::path& root) { return root / "images"; }

/* ------------------------------------------------------------ JSON 助手 */

bool is_blank(const std::string& value) {
  for (char c : value) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  }
  return true;
}

/* 上游要求图层名非空：无名的引擎图层在落盘时补一个确定性的占位名。 */
std::string manifest_name_of(const Layer& layer) {
  if (!is_blank(layer.name)) return layer.name;
  return "Layer " + std::to_string(static_cast<unsigned long long>(layer.id));
}

/*
 * 混合模式按上游 LayerBlendMode 的规范字符串落盘（键名 blendMode），
 * 而不是内部枚举数值：.comp 是跨端交换格式，写入方必须与上游一致。
 */
JsonValue blend_to_json(comp_blend_mode mode) {
  const char* name = blend_mode_manifest_name(mode);
  return JsonValue::string(name != nullptr ? name : "Normal");
}

bool blend_from_json(const JsonValue& j, comp_blend_mode* out, std::string* error) {
  const std::string name = j.string_or("blendMode", "");
  if (name.empty()) {
    *out = COMP_BLEND_NORMAL;
    return true;
  }
  if (!blend_mode_from_manifest_name(name.data(), name.size(), out)) {
    if (error != nullptr) *error = "unknown blendMode: " + name;
    return false;
  }
  return true;
}

/*
 * transform 必须是上游 LayerTransform 的六键形态。缺任一键都会让上游解码失败
 * （Swift 合成 Codable 对非 Optional 属性一律 decode(_:forKey:)），因此这里逐键写全。
 *
 * 写侧钳制（kMaxTransformOrigin / kMaxTransformSize）同时是读侧防线：
 * manifest 是跨端交换格式，恶意/损坏文件可以写任意大的整数。裸 static_cast<int32_t>
 * 截断后，effective_bounds 与 clamp_to_canvas 里的 `a.x + a.w` 就是有符号溢出（UB）。
 * 读侧按同一套边界钳制，与上游 LayerTransform.isValid 对齐。
 */
JsonValue transform_to_json(const Layer& layer) {
  int64_t width = layer.transform.w;
  int64_t height = layer.transform.h;
  if (width <= 0) width = layer.pixels.empty() ? 1 : layer.pixels.width;
  if (height <= 0) height = layer.pixels.empty() ? 1 : layer.pixels.height;
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  if (width > kMaxTransformSize) width = kMaxTransformSize;
  if (height > kMaxTransformSize) height = kMaxTransformSize;

  int64_t origin_x = layer.transform.x;
  int64_t origin_y = layer.transform.y;
  if (origin_x > kMaxTransformOrigin) origin_x = kMaxTransformOrigin;
  if (origin_x < -kMaxTransformOrigin) origin_x = -kMaxTransformOrigin;
  if (origin_y > kMaxTransformOrigin) origin_y = kMaxTransformOrigin;
  if (origin_y < -kMaxTransformOrigin) origin_y = -kMaxTransformOrigin;

  JsonValue origin = JsonValue::array({});
  origin.push(JsonValue::number(static_cast<double>(origin_x)));
  origin.push(JsonValue::number(static_cast<double>(origin_y)));

  JsonValue size = JsonValue::array({});
  size.push(JsonValue::number(static_cast<double>(width)));
  size.push(JsonValue::number(static_cast<double>(height)));

  JsonValue transform = JsonValue::object({});
  transform.set("origin", std::move(origin));
  transform.set("size", std::move(size));
  transform.set("rotation", JsonValue::number(static_cast<double>(layer.rotation)));
  transform.set("flipX", JsonValue::boolean(layer.flip_x));
  transform.set("flipY", JsonValue::boolean(layer.flip_y));
  transform.set("sampling",
                JsonValue::string(sampling_manifest_name(sampling_from_abi(layer.sampling))));
  return transform;
}

/* 读侧钳制：与 transform_to_json 同一套边界（上游 LayerTransform.isValid）。 */
int32_t clamp_transform_origin(double value) {
  if (!(value >= -kMaxTransformOrigin)) return static_cast<int32_t>(-kMaxTransformOrigin);
  if (value > kMaxTransformOrigin) return static_cast<int32_t>(kMaxTransformOrigin);
  return static_cast<int32_t>(value);
}

int32_t clamp_transform_size(double value) {
  if (!(value >= 1.0)) return 1;
  if (value > kMaxTransformSize) return static_cast<int32_t>(kMaxTransformSize);
  return static_cast<int32_t>(value);
}

void transform_from_json(const JsonValue& entry, Layer* layer) {
  const JsonValue* transform = entry.find("transform");
  if (transform == nullptr || !transform->is_object()) return;
  const JsonValue* origin = transform->find("origin");
  if (origin != nullptr && origin->is_array() && origin->as_array().size() >= 2) {
    layer->transform.x = clamp_transform_origin(origin->as_array()[0].as_number(0.0));
    layer->transform.y = clamp_transform_origin(origin->as_array()[1].as_number(0.0));
  }
  const JsonValue* size = transform->find("size");
  if (size != nullptr && size->is_array() && size->as_array().size() >= 2) {
    layer->transform.w = clamp_transform_size(size->as_array()[0].as_number(0.0));
    layer->transform.h = clamp_transform_size(size->as_array()[1].as_number(0.0));
  }
  layer->rotation = static_cast<float>(transform->number_or("rotation", 0.0));
  layer->flip_x = transform->bool_or("flipX", false);
  layer->flip_y = transform->bool_or("flipY", false);
  layer->sampling = sampling_to_abi(
      sampling_from_manifest_name(transform->string_or("sampling", "High quality")));
}

/* ------------------------------------------------------------ 可编辑文字（P0-6 ①） */

/*
 * engineText：文字对象的整份可编辑状态。**像素不进这里**——文字图层的像素照旧
 * 走 imageFile，是 `text` 的渲染缓存；重新打开工程后改字号只需重渲染这一层。
 *
 * 为什么存 font_path 而不是字体句柄：字体句柄属于宿主进程，`.comp` 里没有它的
 * 位置；上游同样以「字体名/文件引用」往返。空路径 = 内置 5×7 点阵。
 */
JsonValue text_to_json(const TextObject& text) {
  JsonValue j = JsonValue::object({});
  j.set("text", JsonValue::string(text.text));
  j.set("origin", JsonValue::array({JsonValue::number(text.origin_x),
                                    JsonValue::number(text.origin_y)}));
  j.set("fontSize", JsonValue::number(text.style.font_size));
  j.set("lineHeight", JsonValue::number(text.style.line_height));
  j.set("letterSpacing", JsonValue::number(text.style.letter_spacing));
  j.set("antialias", JsonValue::boolean(text.style.antialias));
  JsonValue color = JsonValue::array({});
  for (int i = 0; i < 4; ++i) color.push(JsonValue::integer(text.color[i]));
  j.set("color", std::move(color));
  j.set("opacity", JsonValue::number(static_cast<double>(text.opacity)));
  if (!text.font_path.empty()) {
    j.set("fontPath", JsonValue::string(text.font_path));
    j.set("fontFaceIndex", JsonValue::integer(text.font_face_index));
  }
  if (!text.font_family.empty()) j.set("fontFamily", JsonValue::string(text.font_family));
  /* 段落框（P0-6 ③）：只在非默认时写，老工程的 engineText 逐字节不变。 */
  if (text.box_width > 0.0) j.set("boxWidth", JsonValue::number(text.box_width));
  if (text.box_height > 0.0) j.set("boxHeight", JsonValue::number(text.box_height));
  if (text.align != 0) j.set("align", JsonValue::integer(text.align));
  return j;
}

/*
 * 读回 engineText。任何让对象**不可渲染**的字段组合都整体丢弃（图层退回普通
 * 像素图层，画面不变），而不是把半个文字对象塞进图层——半截对象会让下一次
 * 「改字号」把它渲染成另一张图，用户看到的是静默的内容变化。
 */
bool text_from_json(const JsonValue& j, TextObject* out) {
  TextObject text;
  text.text = j.string_or("text", "");
  const JsonValue* origin = j.find("origin");
  if (origin != nullptr && origin->is_array() && origin->as_array().size() >= 2) {
    text.origin_x = origin->as_array()[0].as_number(0.0);
    text.origin_y = origin->as_array()[1].as_number(0.0);
  }
  text.style.font_size = j.number_or("fontSize", 0.0);
  text.style.line_height = j.number_or("lineHeight", 0.0);
  text.style.letter_spacing = j.number_or("letterSpacing", 0.0);
  text.style.antialias = j.bool_or("antialias", true);
  const JsonValue* color = j.find("color");
  if (color != nullptr && color->is_array() && color->as_array().size() >= 4) {
    for (int i = 0; i < 4; ++i) {
      long long value = color->as_array()[i].as_integer(0);
      if (value < 0) value = 0;
      if (value > 255) value = 255;
      text.color[i] = static_cast<uint8_t>(value);
    }
  }
  text.opacity = static_cast<float>(j.number_or("opacity", 0.0));
  text.font_path = j.string_or("fontPath", "");
  text.font_face_index = static_cast<int32_t>(j.integer_or("fontFaceIndex", 0));
  text.font_family = j.string_or("fontFamily", "");
  text.box_width = j.number_or("boxWidth", 0.0);
  text.box_height = j.number_or("boxHeight", 0.0);
  text.align = static_cast<int32_t>(j.integer_or("align", 0));

  TextLayerReport report;
  text_layer_validate(text, &report);
  if (report.status != TextLayerStatus::kOk) return false;
  *out = std::move(text);
  return true;
}

/* ------------------------------------------------------------ 写：单个图层 */

/* 返回该图层在 manifest 中的记录；uuid 为该图层派生出的 UUID 文本。 */
JsonValue layer_to_json(const Layer& layer, const std::string& uuid,
                        const std::unordered_map<comp_layer_id, std::string>& layer_uuid) {
  JsonValue j = JsonValue::object({});
  j.set("id", JsonValue::string(uuid));
  j.set("name", JsonValue::string(manifest_name_of(layer)));
  j.set("isVisible", JsonValue::boolean(layer.visible));
  j.set("transform", transform_to_json(layer));
  /* 只有像素图层写 imageFile；上游规定「组层不得带 imageFile」。 */
  if (!layer.pixels.empty() && !layer.is_group) {
    j.set("imageFile", JsonValue::string(pixel_file_name(uuid, layer.pixels.format)));
  }
  j.set("isGroup", JsonValue::boolean(layer.is_group));

  const double opacity = std::min(1.0, std::max(0.0, static_cast<double>(layer.opacity)));
  j.set("opacity", JsonValue::number(opacity));
  /* 上游规定：组层（文件夹）是穿透的，blendMode 必须为 Normal。 */
  j.set("blendMode", layer.is_group ? JsonValue::string("Normal") : blend_to_json(layer.blend));

  if (layer.parent != 0) {
    const auto it = layer_uuid.find(layer.parent);
    if (it != layer_uuid.end()) j.set("parentID", JsonValue::string(it->second));
  }
  if (!layer.mask.empty()) {
    j.set("maskFile", JsonValue::string(mask_file_name(uuid)));
    j.set("maskEnabled", JsonValue::boolean(true));
  }
  if (!layer.mask_source_uuid.empty()) {
    j.set("maskSourceID", JsonValue::string(layer.mask_source_uuid));
  }

  /* ---- 引擎扩展键（全部带 engine 前缀，上游忽略，往返保真）---- */
  j.set("engineLayerID", JsonValue::integer(static_cast<long long>(layer.id)));
  if (layer.clipping_source != 0) {
    const auto it = layer_uuid.find(layer.clipping_source);
    if (it != layer_uuid.end()) {
      j.set("engineClippingSourceID", JsonValue::string(it->second));
    }
  }
  j.set("enginePixelFormat", JsonValue::integer(static_cast<long long>(layer.pixels.format)));

  JsonValue adjustments = JsonValue::array({});
  for (const Adjustment& adjustment : layer.adjustments) {
    JsonValue a = JsonValue::object({});
    a.set("kind", JsonValue::integer(static_cast<long long>(adjustment.kind)));
    a.set("settings", JsonValue::string(adjustment.settings_json));
    adjustments.push(std::move(a));
  }
  j.set("engineAdjustments", std::move(adjustments));

  JsonValue effects = JsonValue::array({});
  for (const Effect& effect : layer.effects) {
    JsonValue e = JsonValue::object({});
    e.set("effect", JsonValue::integer(static_cast<long long>(effect.effect)));
    e.set("settings", JsonValue::string(effect.settings_json));
    effects.push(std::move(e));
  }
  j.set("engineEffects", std::move(effects));

  /*
   * 组隔离（engineIsolated）：只在为真时落盘——默认的「穿透」即缺省值，缺键即穿透，
   * 这样既有工程文件与不含隔离组的落盘结果保持逐字节不变。键值原样往返（与
   * isVisible / opacity 一致），是否参与合成由合成侧按 is_group && isolated 决定。
   */
  if (layer.isolated) {
    j.set("engineIsolated", JsonValue::boolean(true));
  }
  /*
   * 组自己的混合模式（engineBlendMode）：上游规定组层的 blendMode 必须是
   * Normal，组真正的混合模式（隔离组可以有自己的混合模式，如 Multiply）只能走
   * 引擎键。缺键 = Normal，所以不含此类组的工程落盘结果逐字节不变。
   */
  if (layer.is_group && layer.blend != COMP_BLEND_NORMAL) {
    j.set("engineBlendMode", blend_to_json(layer.blend));
  }
  /*
   * 可编辑文字（engineText）：只在是文字图层时落盘，缺键 = 普通图层——与
   * engineIsolated 同一条「默认值不写」的规矩，普通工程的 manifest 因此逐字节不变。
   */
  if (layer.text) {
    j.set("engineText", text_to_json(*layer.text));
  }
  /*
   * 图层锁（engineLock）与颜色标签（engineColorLabel）：同一条「默认值不写」。
   * 锁是整数字节掩码（comp_layer_lock），缺键 = 未锁；颜色标签 0 = 无。
   * 两者都是纯文档状态：不进合成、不推进内容修订号，但随撤销/重做与工程往返。
   */
  if (layer.lock_flags != 0u) {
    j.set("engineLock", JsonValue::integer(static_cast<long long>(layer.lock_flags)));
  }
  if (layer.color_label != 0u) {
    j.set("engineColorLabel", JsonValue::integer(static_cast<long long>(layer.color_label)));
  }
  /* 图层链接（engineLink）：0 = 未链接，缺键即未链接。组号 >0 才是有效状态。 */
  if (layer.link_group != 0u) {
    j.set("engineLink", JsonValue::integer(static_cast<long long>(layer.link_group)));
  }
  return j;
}

/* resolution 的上游取值域为 1...9600 的有限数，越界会令上游拒绝整个工程。 */
double clamp_resolution(double value) {
  if (!(value == value) || value < 1.0 || value > 9600.0) return 72.0;
  return value;
}

}  // namespace

uint32_t project_manifest_version() { return kManifestVersion; }

bool save_project(Document& doc, const std::string& path, std::string* error) {
  DocumentState& state = doc.state();
  if (!is_valid_canvas(comp_size{state.width, state.height})) {
    if (error != nullptr) *error = "canvas size out of range";
    return false;
  }
  if (static_cast<int64_t>(state.layers.size()) > kMaxLayers) {
    if (error != nullptr) *error = "too many layers for .comp";
    return false;
  }

  /* documentID 必须非空且是合法 UUID；为空或非法时在此补齐并写回文档状态，
   * 从而保证「同一文档多次保存」的图层文件名不漂移。 */
  uint8_t id_bytes[16];
  if (state.document_id.empty() || !uuid_from_string(state.document_id, id_bytes)) {
    state.document_id = uuid_v4_new();
  }

  /* 图层内部 id -> 该层 UUID（同一文档恒定）。 */
  std::unordered_map<comp_layer_id, std::string> layer_uuid;
  for (const Layer& layer : state.layers) {
    layer_uuid[layer.id] = uuid_derive(state.document_id, layer.id);
  }

  const fs::path root = fs::u8path(path);
  const fs::path images_dir = images_dir_of(root);
  std::error_code ec;
  fs::create_directories(images_dir, ec);
  if (ec) {
    if (error != nullptr) *error = "cannot create project directory: " + ec.message();
    return false;
  }

  JsonValue manifest = JsonValue::object({});
  manifest.set("format", JsonValue::string(kFormatId));
  manifest.set("version", JsonValue::integer(static_cast<long long>(kManifestVersion)));
  manifest.set("colorSpace", JsonValue::string(kColorSpace));
  manifest.set("resolution", JsonValue::number(clamp_resolution(static_cast<double>(state.dpi))));
  manifest.set("documentID", JsonValue::string(state.document_id));
  manifest.set("width", JsonValue::integer(state.width));
  manifest.set("height", JsonValue::integer(state.height));
  if (state.active_layer != 0) {
    const auto it = layer_uuid.find(state.active_layer);
    if (it != layer_uuid.end()) manifest.set("activeLayerID", JsonValue::string(it->second));
  }
  /* 参考线：空表不写键，保证「没有参考线的文档」存出的 manifest 与旧版逐字节
   * 一致。键名走 engine 前缀（engineGuides）——上游 manifest 另有同名 `guides`
   * 键但其结构未文档化，不能拿它当自己的格式（见 comp_file.h / domain/guide.h）。 */
  if (!state.guides.empty()) {
    JsonValue guides = JsonValue::array({});
    for (const Guide& guide : state.guides) {
      JsonValue entry = JsonValue::object({});
      entry.set("axis", JsonValue::integer(static_cast<long long>(guide.axis)));
      entry.set("position", JsonValue::number(guide.position));
      guides.push(std::move(entry));
    }
    manifest.set("engineGuides", std::move(guides));
  }

  /* 存储通道（engineChannels）：同样空表不写键。每条只落名字与羽化半径，
   * 蒙版写到 images/<documentID>.channel<N>.png（名字由数组下标派生）。 */
  if (!state.channels.empty()) {
    JsonValue channels = JsonValue::array({});
    for (size_t i = 0; i < state.channels.size(); ++i) {
      const SelectionChannel& channel = state.channels[i];
      if (channel.mask.empty() || channel.mask.width != state.width ||
          channel.mask.height != state.height) {
        if (error != nullptr) *error = "selection channel mask size mismatch";
        return false;
      }
      std::vector<uint8_t> png;
      if (!png_encode_gray8(channel.mask.pixel_data().data(), channel.mask.width,
                            channel.mask.height, &png, error)) {
        return false;
      }
      if (!write_bytes_file(images_dir / fs::u8path(channel_file_name(state.document_id, i)),
                            png.data(), png.size(), error)) {
        return false;
      }

      double feather = static_cast<double>(channel.feather);
      if (!(feather >= 0.0)) feather = 0.0; /* 同时挡住 NaN */
      if (feather > static_cast<double>(kMaxSelectionFeather)) {
        feather = static_cast<double>(kMaxSelectionFeather);
      }
      JsonValue entry = JsonValue::object({});
      entry.set("name", JsonValue::string(channel.name));
      entry.set("feather", JsonValue::number(feather));
      channels.push(std::move(entry));
    }
    manifest.set("engineChannels", std::move(channels));
  }

  JsonValue layers = JsonValue::array({});
  for (const Layer& layer : state.layers) {
    const std::string uuid = layer_uuid[layer.id];

    if (!layer.pixels.empty()) {
      const comp_pixel_format format = layer.pixels.format;
      const int32_t pixel_width = layer.pixels.width;
      const int32_t pixel_height = layer.pixels.height;
      if (!is_valid_canvas(comp_size{pixel_width, pixel_height})) {
        if (error != nullptr) *error = "layer pixel size out of range";
        return false;
      }

      if (format == COMP_PIXEL_RGBA8) {
        std::vector<uint8_t> png;
        if (!png_encode_rgba8(layer.pixels.pixel_data().data(), pixel_width, pixel_height, &png,
                              error)) {
          return false;
        }
        if (!write_bytes_file(images_dir / fs::u8path(pixel_file_name(uuid, format)), png.data(),
                              png.size(), error)) {
          return false;
        }
      } else if (format == COMP_PIXEL_RGBA16 || format == COMP_PIXEL_RGBA16F) {
        /*
         * 宽位深图层落成 EXR：文件里是**线性光、直通 alpha** 的浮点（外部
         * 看图软件按 EXR 惯例读到的就是对的颜色），图层内存里是 extended sRGB
         * 预乘的 16 位整数或 half。转换必须与读入侧同一对曲线：
         *   RGBA16F -> deep_hdr_to_linear；RGBA16 -> deep_u16_to_linear。
         * 采样类型：RGBA16F 用 HALF（与图层同精度，往返逐位一致）；RGBA16 用
         * FLOAT——half 只有 11 位尾数，存不下 16 位整数的低 5 位。
         */
        const size_t pixels = static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height);
        const size_t float_stride = static_cast<size_t>(pixel_width) * 4u * sizeof(float);
        std::vector<float> linear(pixels * 4u);
        if (format == COMP_PIXEL_RGBA16F) {
          deep_hdr_to_linear(reinterpret_cast<const uint16_t*>(layer.pixels.pixel_data().data()),
                             layer.pixels.stride(), linear.data(), float_stride, pixel_width,
                             pixel_height);
        } else {
          deep_u16_to_linear(reinterpret_cast<const uint16_t*>(layer.pixels.pixel_data().data()),
                             layer.pixels.stride(), linear.data(), float_stride, pixel_width,
                             pixel_height);
        }
        const ExrSampleType sample_type =
            format == COMP_PIXEL_RGBA16F ? kExrSampleHalf : kExrSampleFloat;
        std::vector<uint8_t> exr;
        if (!exr_encode_float(linear.data(), pixel_width, pixel_height, sample_type, &exr, error)) {
          return false;
        }
        if (!write_bytes_file(images_dir / fs::u8path(pixel_file_name(uuid, format)), exr.data(),
                              exr.size(), error)) {
          return false;
        }
      } else {
        if (error != nullptr) {
          *error = "layer id " +
                   std::to_string(static_cast<unsigned long long>(layer.id)) +
                   " has a pixel format .comp cannot store (RGBA8/RGBA16/RGBA16F only)";
        }
        return false;
      }
    }

    if (!layer.mask.empty()) {
      std::vector<uint8_t> png;
      if (!png_encode_gray8(layer.mask.pixel_data().data(), layer.mask.width, layer.mask.height, &png,
                            error)) {
        return false;
      }
      if (!write_bytes_file(images_dir / fs::u8path(mask_file_name(uuid)), png.data(), png.size(),
                            error)) {
        return false;
      }
    }

    layers.push(layer_to_json(layer, uuid, layer_uuid));
  }
  manifest.set("layers", std::move(layers));
  manifest.set("engineVersion", JsonValue::string("1.0.0"));

  const std::string text = json_dump(manifest, 2) + "\n";
  if (text.size() > kMaxManifestBytes) {
    if (error != nullptr) *error = "manifest exceeds 4 MiB";
    return false;
  }
  return write_text_file(manifest_path_of(root), text, error);
}

/* ------------------------------------------------------------ 读：单个图层 */

namespace {

/* 读取一个图层的像素。uuid 为该层 UUID，用于定位资产文件。
 *
 * 资产格式由 manifest 的 enginePixelFormat 决定，**不由扩展名猜**：
 *   RGBA8 + .png        -> 自研 PNG 解码（预乘 RGBA8，原样入层）；
 *   RGBA16 + .exr       -> 线性光浮点 -> deep_u16_take_linear（不经 half，保 16 位精度）；
 *   RGBA16F + .exr      -> 线性光浮点 -> deep_hdr_take_linear（half，逐位往返）；
 *   无 enginePixelFormat + .exr -> wide_asset_from_linear（按内容判 HDR/16 位，
 *                          与外部导入同一口径；上游风格的工程没有本引擎的扩展键）。
 * 声明与扩展名不符、或声明了本引擎不存的格式，一律带原因失败——绝不按
 * 另一种格式「近似读一下」。
 */
bool read_layer_pixels(const JsonValue& entry, const fs::path& root, const std::string& uuid,
                       long long pixel_format, bool has_pixel_format, Layer* layer,
                       std::string* error) {
  const std::string image_file = entry.string_or("imageFile", "");
  if (image_file.empty()) return true;

  const bool is_exr = ends_with(image_file, ".exr");
  const bool is_png = ends_with(image_file, ".png");
  if (!is_exr && !is_png) {
    if (error != nullptr) *error = "unsupported layer asset type: " + image_file;
    return false;
  }

  /* 声明与资产打架是 manifest 契约问题，必须先于 IO 判定：否则资产恰好缺失时，
   * IO 错误会盖掉真正的错因（守卫就只测到了「文件不存在」）。 */
  if (is_exr && has_pixel_format && pixel_format == static_cast<long long>(COMP_PIXEL_RGBA8)) {
    if (error != nullptr) {
      *error = "layer " + uuid + ": imageFile is EXR but enginePixelFormat is RGBA8";
    }
    return false;
  }
  if (!is_exr && has_pixel_format && pixel_format != static_cast<long long>(COMP_PIXEL_RGBA8)) {
    if (error != nullptr) {
      *error = "layer " + uuid + ": enginePixelFormat " + std::to_string(pixel_format) +
               " needs an EXR asset, but imageFile is " + image_file;
    }
    return false;
  }

  std::vector<uint8_t> bytes;
  if (!read_bytes_file(images_dir_of(root) / fs::u8path(image_file), &bytes, error)) return false;

  if (!is_exr) {
    PngImage decoded;
    if (!png_decode_rgba8(bytes.data(), bytes.size(), &decoded, error)) return false;
    if (!is_valid_canvas(comp_size{decoded.width, decoded.height})) {
      if (error != nullptr) *error = "layer pixel size out of range";
      return false;
    }

    layer->pixels.resize(decoded.width, decoded.height, COMP_PIXEL_RGBA8);
    if (!decoded.rgba.empty()) {
      std::memcpy(layer->pixels.writable_pixels().data(), decoded.rgba.data(), decoded.rgba.size());
    }
    return true;
  }

  CodecFloatImage floats;
  if (!exr_decode_float(bytes.data(), bytes.size(), &floats, error) || floats.empty()) {
    if (error != nullptr && error->empty()) *error = "EXR layer decode failed";
    return false;
  }
  /* 解码器已按 dataWindow 做过尺寸闸；这里仍按**实际出图**再核一次，
   * 因为下面的转换会按声明尺寸分配整幅缓冲。 */
  if (!is_valid_canvas(comp_size{floats.width, floats.height})) {
    if (error != nullptr) *error = "layer pixel size out of range";
    return false;
  }
  const int32_t width = floats.width;
  const int32_t height = floats.height;
  const size_t float_stride = static_cast<size_t>(width) * 4u * sizeof(float);

  if (!has_pixel_format) {
    ImageBuffer wide;
    if (!wide_asset_from_linear(floats.rgba.data(), width, height, &wide)) {
      if (error != nullptr) *error = "wide asset conversion failed";
      return false;
    }
    layer->pixels = std::move(wide);
    return true;
  }

  if (pixel_format == static_cast<long long>(COMP_PIXEL_RGBA16)) {
    layer->pixels.resize(width, height, COMP_PIXEL_RGBA16);
    deep_u16_take_linear(floats.rgba.data(), float_stride,
                         reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data()),
                         layer->pixels.stride(), width, height);
    return true;
  }
  if (pixel_format == static_cast<long long>(COMP_PIXEL_RGBA16F)) {
    layer->pixels.resize(width, height, COMP_PIXEL_RGBA16F);
    deep_hdr_take_linear(floats.rgba.data(), float_stride,
                         reinterpret_cast<uint16_t*>(layer->pixels.writable_pixels().data()),
                         layer->pixels.stride(), width, height);
    return true;
  }

  if (error != nullptr) {
    *error = "layer " + uuid + ": enginePixelFormat " + std::to_string(pixel_format) +
             " is not a pixel format this build stores";
  }
  return false;
}

bool read_layer_mask(const JsonValue& entry, const fs::path& root, Layer* layer,
                     std::string* error) {
  const std::string mask_file = entry.string_or("maskFile", "");
  if (mask_file.empty()) return true;
  std::vector<uint8_t> bytes;
  if (!read_bytes_file(images_dir_of(root) / fs::u8path(mask_file), &bytes, error)) return false;

  /* 掩码是 8 位灰度 PNG：解码成 RGBA 后取 R 通道即可（灰阶三通道相等，alpha = 255）。 */
  PngImage decoded;
  if (!png_decode_rgba8(bytes.data(), bytes.size(), &decoded, error)) return false;
  layer->mask.resize(decoded.width, decoded.height);
  std::vector<uint8_t>& mask_px = layer->mask.writable_pixels();
  for (int32_t y = 0; y < decoded.height; ++y) {
    for (int32_t x = 0; x < decoded.width; ++x) {
      const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(decoded.width) +
                            static_cast<size_t>(x)) * 4u;
      mask_px[static_cast<size_t>(y) * static_cast<size_t>(decoded.width) +
              static_cast<size_t>(x)] = decoded.rgba[index];
    }
  }
  return true;
}

}  // namespace

bool load_project(const std::string& path, Engine* engine, Document** out,
                  std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out document pointer is null";
    return false;
  }
  *out = nullptr;

  const fs::path root = fs::u8path(path);
  std::error_code ec;
  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
    if (error != nullptr) *error = "not a .comp directory: " + path;
    return false;
  }

  std::string text;
  if (!read_text_file(manifest_path_of(root), &text, error)) return false;
  if (text.size() > kMaxManifestBytes) {
    if (error != nullptr) *error = "manifest exceeds 4 MiB";
    return false;
  }

  JsonValue manifest = json_parse(text, error);
  if (!manifest.is_object()) {
    if (error != nullptr && error->empty()) *error = "manifest root is not an object";
    return false;
  }
  if (manifest.string_or("format", "") != kFormatId) {
    if (error != nullptr) *error = "unexpected project format";
    return false;
  }
  const long long version = manifest.integer_or("version", 0);
  if (version < 1 || version > static_cast<long long>(kManifestVersion)) {
    if (error != nullptr) *error = "unsupported manifest version";
    return false;
  }
  const std::string color_space = manifest.string_or("colorSpace", kColorSpace);
  if (color_space != kColorSpace) {
    if (error != nullptr) *error = "unsupported color space: " + color_space;
    return false;
  }
  const int32_t width = static_cast<int32_t>(manifest.integer_or("width", 0));
  const int32_t height = static_cast<int32_t>(manifest.integer_or("height", 0));
  if (!is_valid_canvas(comp_size{width, height})) {
    if (error != nullptr) *error = "manifest canvas size out of range";
    return false;
  }

  std::string document_id = manifest.string_or("documentID", "");
  uint8_t id_bytes[16];
  if (document_id.empty() || !uuid_from_string(document_id, id_bytes)) {
    document_id = uuid_v4_new();
  }

  /* 与 comp_document_create 走同一对工厂函数：文档对象经引擎分配器分配，
   * unique_ptr 的删除器必须用 Document::destroy 而不是默认 delete，
   * 否则定制分配器场景下「分配/释放」来自两座堆。 */
  auto doc = std::unique_ptr<Document, void (*)(Document*)>(
      Document::create(engine, width, height,
                       static_cast<float>(clamp_resolution(
                           manifest.number_or("resolution", 72.0))),
                       color_space),
      Document::destroy);
  DocumentState& state = doc->state();
  state.layers.clear();
  state.document_id = document_id;

  /* 参考线（engineGuides）。缺键 = 没有参考线（旧 manifest 直接兼容）；
   * 键存在就必须是合法数组——坏数据静默丢弃会让「存过参考线怎么没了」
   * 无从排查，与图层字段的严格读法一致。 */
  const JsonValue* guide_entries = manifest.find("engineGuides");
  if (guide_entries != nullptr && !guide_entries->is_null()) {
    if (!guide_entries->is_array()) {
      if (error != nullptr) *error = "engineGuides must be an array";
      return false;
    }
    if (guide_entries->as_array().size() > kMaxGuides) {
      if (error != nullptr) *error = "too many guides in manifest";
      return false;
    }
    for (const JsonValue& entry : guide_entries->as_array()) {
      if (!entry.is_object()) {
        if (error != nullptr) *error = "engineGuides must contain objects";
        return false;
      }
      const JsonValue* axis = entry.find("axis");
      const JsonValue* position = entry.find("position");
      if (axis == nullptr || position == nullptr || !axis->is_number() || !position->is_number()) {
        if (error != nullptr) *error = "guide must carry numeric axis and position";
        return false;
      }
      const long long axis_value = axis->as_integer(-1);
      const double position_value = position->as_number();
      if (axis_value < 0 || axis_value > 1 ||
          !guide_is_valid(static_cast<uint32_t>(axis_value), position_value)) {
        if (error != nullptr) *error = "guide axis or position is invalid";
        return false;
      }
      Guide guide;
      guide.axis = static_cast<uint32_t>(axis_value);
      guide.position = position_value;
      state.guides.push_back(guide);
    }
  }

  /* 存储通道（engineChannels）。缺键 = 没有通道；键存在则严格读：
   * 名字必须是合法通道名、feather 在 0..250、蒙版 PNG 必须恰好是文档尺寸。
   * 资产名由 (documentID, 下标) 派生，manifest 里的路径字段一律不认。 */
  const JsonValue* channel_entries = manifest.find("engineChannels");
  if (channel_entries != nullptr && !channel_entries->is_null()) {
    if (!channel_entries->is_array()) {
      if (error != nullptr) *error = "engineChannels must be an array";
      return false;
    }
    if (channel_entries->as_array().size() > kMaxSelectionChannels) {
      if (error != nullptr) *error = "too many selection channels in manifest";
      return false;
    }
    for (size_t i = 0; i < channel_entries->as_array().size(); ++i) {
      const JsonValue& entry = channel_entries->as_array()[i];
      if (!entry.is_object()) {
        if (error != nullptr) *error = "engineChannels must contain objects";
        return false;
      }
      const JsonValue* name = entry.find("name");
      const JsonValue* feather = entry.find("feather");
      if (name == nullptr || !name->is_string() || feather == nullptr ||
          !feather->is_number()) {
        if (error != nullptr) *error = "selection channel must carry string name and numeric feather";
        return false;
      }
      const std::string channel_name = name->as_string();
      if (!selection_channel_name_is_valid(channel_name)) {
        if (error != nullptr) *error = "selection channel name is invalid";
        return false;
      }
      const double feather_value = feather->as_number();
      if (!(feather_value >= 0.0) ||
          feather_value > static_cast<double>(kMaxSelectionFeather)) {
        if (error != nullptr) *error = "selection channel feather is out of range";
        return false;
      }

      std::vector<uint8_t> bytes;
      if (!read_bytes_file(images_dir_of(root) / fs::u8path(channel_file_name(document_id, i)),
                           &bytes, error)) {
        return false;
      }
      PngImage decoded;
      if (!png_decode_rgba8(bytes.data(), bytes.size(), &decoded, error)) return false;
      if (decoded.width != width || decoded.height != height) {
        if (error != nullptr) *error = "selection channel mask size does not match document";
        return false;
      }

      SelectionChannel channel;
      channel.name = channel_name;
      channel.feather = static_cast<float>(feather_value);
      channel.mask.resize(decoded.width, decoded.height, 0);
      std::vector<uint8_t>& mask_px = channel.mask.writable_pixels();
      for (int32_t y = 0; y < decoded.height; ++y) {
        for (int32_t x = 0; x < decoded.width; ++x) {
          const size_t index =
              (static_cast<size_t>(y) * static_cast<size_t>(decoded.width) +
               static_cast<size_t>(x)) * 4u;
          mask_px[static_cast<size_t>(y) * static_cast<size_t>(decoded.width) +
                  static_cast<size_t>(x)] = decoded.rgba[index];
        }
      }
      state.channels.push_back(std::move(channel));
    }
  }

    std::unordered_map<std::string, comp_layer_id> uuid_to_id;
  const JsonValue* layers = manifest.find("layers");
  if (layers != nullptr && layers->is_array()) {
    if (static_cast<int64_t>(layers->as_array().size()) > kMaxLayers) {
      if (error != nullptr) *error = "too many layers in manifest";
      return false;
    }

    /*
     * engineLayerID 是引擎内部 id 的持久化形态：两条不同 UUID 的层一旦声明同一
     * id，find()/parentID 解析全部别名错层，撤销栈按 id 寻的也会打到别人。
     * 显式 id 与顺序分配的 id 共用同一张去重表，两个方向的碰撞都堵。
     */
    std::unordered_set<comp_layer_id> used_ids;

    /* 第一趟：分配内部 id、解析基础字段与像素。 */
    comp_layer_id next_id = 1;
    for (const JsonValue& entry : layers->as_array()) {
      if (!entry.is_object()) {
        if (error != nullptr) *error = "layer entry is not an object";
        return false;
      }
      Layer layer;

      std::string uuid = entry.string_or("id", "");
      if (!uuid_is_wellformed(uuid)) {
        /* 非 UUID 的 id 无法与上游互通：用内容派生的稳定 UUID 兜底。 */
        uuid = uuid_derive(document_id, static_cast<uint64_t>(next_id));
      }
      if (!uuid_to_id.emplace(uuid, 0).second) {
        if (error != nullptr) *error = "duplicate layer id in manifest";
        return false;
      }

      /* 引擎自有内部 id 优先（保证往返后 id 与图层文件名都不变），否则顺序分配。 */
      const long long explicit_id = entry.integer_or("engineLayerID", 0);
      if (explicit_id > 0 && explicit_id < (1LL << 62)) {
        layer.id = static_cast<comp_layer_id>(explicit_id);
      } else {
        layer.id = next_id;
      }
      if (layer.id >= next_id) next_id = layer.id + 1;
      if (!used_ids.insert(layer.id).second) {
        if (error != nullptr) *error = "duplicate engineLayerID in manifest";
        return false;
      }

      layer.name = entry.string_or("name", "");
      layer.visible = entry.bool_or("isVisible", true);
      layer.is_group = entry.bool_or("isGroup", false);
      /*
       * 组隔离（engineIsolated）：缺键 = 穿透。原样回填（不按 is_group 过滤），
       * 是否真的隔离由合成侧 filter_graph 的 is_group && isolated 决定。
       */
      layer.isolated = entry.bool_or("engineIsolated", false);
      layer.opacity = static_cast<float>(entry.number_or("opacity", 1.0));
      if (!blend_from_json(entry, &layer.blend, error)) return false;
      /*
       * 组自己的混合模式（engineBlendMode，见 layer_to_json）：存在时覆盖
       * blendMode 那一份「上游兼容用的 Normal」。未知名字明确报错，不静默当
       * Normal——那等于改了用户的图。
       */
      if (layer.is_group) {
        const JsonValue* engine_blend = entry.find("engineBlendMode");
        if (engine_blend != nullptr) {
          const std::string engine_blend_name =
              engine_blend->is_string() ? engine_blend->as_string("") : std::string();
          if (!blend_mode_from_manifest_name(engine_blend_name.data(), engine_blend_name.size(),
                                             &layer.blend)) {
            if (error != nullptr) {
              *error = "layer " + uuid + ": unknown engineBlendMode " + engine_blend_name;
            }
            return false;
          }
        }
      }
      transform_from_json(entry, &layer);
      layer.mask_source_uuid = entry.string_or("maskSourceID", "");

      /*
       * 引擎扩展：像素格式、调整、效果。
       *
       * enginePixelFormat 必须在**解码之前**读到：它决定资产按哪条路读
       * （PNG 8 位 / EXR 16 位 / EXR half），以及缓冲要用哪种 stride 分配。
       * 从前是「先按 RGBA8 读，再把 format 覆写成 8 字节/像素」——那会让
       * stride()/byte_size() 翻倍而缓冲容量不变，row(y) 对 y >= h/2 越界读写。
       *
       * 缺键 = RGBA8（老工程的语义）；键存在时只接受本构建真能存的三种格式，
       * 其余明确报错，不静默按 RGBA8 近似。
       */
      const JsonValue* format_value = entry.find("enginePixelFormat");
      const bool has_pixel_format = format_value != nullptr;
      const long long pixel_format = entry.integer_or("enginePixelFormat", COMP_PIXEL_RGBA8);
      if (has_pixel_format && pixel_format != static_cast<long long>(COMP_PIXEL_RGBA8) &&
          pixel_format != static_cast<long long>(COMP_PIXEL_RGBA16) &&
          pixel_format != static_cast<long long>(COMP_PIXEL_RGBA16F)) {
        if (error != nullptr) {
          *error = "layer " + uuid + ": unknown enginePixelFormat " + std::to_string(pixel_format);
        }
        return false;
      }

      if (!read_layer_pixels(entry, root, uuid, pixel_format, has_pixel_format, &layer, error)) {
        return false;
      }
      if (!read_layer_mask(entry, root, &layer, error)) return false;

      const JsonValue* adjustments = entry.find("engineAdjustments");
      if (adjustments != nullptr && adjustments->is_array()) {
        for (const JsonValue& a : adjustments->as_array()) {
          Adjustment adjustment;
          adjustment.kind = static_cast<uint32_t>(a.integer_or("kind", 0));
          adjustment.settings_json = a.string_or("settings", "{}");
          layer.adjustments.push_back(std::move(adjustment));
        }
      }
      const JsonValue* effects = entry.find("engineEffects");
      if (effects != nullptr && effects->is_array()) {
        for (const JsonValue& e : effects->as_array()) {
          Effect effect;
          effect.effect = static_cast<uint32_t>(e.integer_or("effect", 0));
          effect.settings_json = e.string_or("settings", "{}");
          layer.effects.push_back(std::move(effect));
        }
      }

      /*
       * 图层锁 / 颜色标签（engineLock / engineColorLabel，见 layer_to_json）：
       * 缺键 = 未锁 / 无标签。取值域外的值明确报错，不静默截断——把未知位当
       * 「没锁」会让用户的图层突然可写，把 9 当灰色会把标签悄悄改名。
       */
      const long long lock_flags = entry.integer_or("engineLock", 0);
      if (lock_flags < 0 ||
          (static_cast<unsigned long long>(lock_flags) & ~static_cast<unsigned long long>(COMP_LAYER_LOCK_MASK)) != 0u) {
        if (error != nullptr) {
          *error = "layer " + uuid + ": unknown engineLock " + std::to_string(lock_flags);
        }
        return false;
      }
      layer.lock_flags = static_cast<uint32_t>(lock_flags);
      const long long color_label = entry.integer_or("engineColorLabel", 0);
      if (color_label < 0 || color_label >= static_cast<long long>(COMP_LAYER_COLOR_COUNT)) {
        if (error != nullptr) {
          *error =
              "layer " + uuid + ": unknown engineColorLabel " + std::to_string(color_label);
        }
        return false;
      }
      layer.color_label = static_cast<uint32_t>(color_label);

      /* engineLink：缺键 = 未链接。组号必须 >0；负数/0 显式写出属于非法状态
       * （0 是缺省值，写出来只说明生成方口径不一致），一律拒绝而不是静默归零。 */
      if (entry.find("engineLink") != nullptr) {
        const long long link_group = entry.integer_or("engineLink", 0);
        if (link_group <= 0 || link_group > 4294967295LL) {
          if (error != nullptr) {
            *error = "layer " + uuid + ": invalid engineLink " + std::to_string(link_group);
          }
          return false;
        }
        layer.link_group = static_cast<uint32_t>(link_group);
      }

      /* engineText：缺键 = 普通图层；解析出的对象不可渲染则整体忽略（像素照旧
       * 由 imageFile 读回，画面不变，只是失去可编辑性——好过让半个文字对象
       * 决定下一次重渲染的结果）。 */
      const JsonValue* text_entry = entry.find("engineText");
      if (text_entry != nullptr && text_entry->is_object()) {
        TextObject text;
        if (text_from_json(*text_entry, &text)) layer.text = std::move(text);

      uuid_to_id[uuid] = layer.id;
      state.layers.push_back(std::move(layer));
    }

    /* 第二趟：解析跨图层引用（parentID / engineClippingSourceID / maskSourceID）。 */
    for (size_t i = 0; i < state.layers.size(); ++i) {
      const JsonValue& entry = layers->as_array()[i];
      Layer& layer = state.layers[i];
      const std::string parent = entry.string_or("parentID", "");
      if (!parent.empty()) {
        const auto it = uuid_to_id.find(parent);
        if (it != uuid_to_id.end()) layer.parent = it->second;
      }
      const std::string clipping = entry.string_or("engineClippingSourceID", "");
      if (!clipping.empty()) {
        const auto it = uuid_to_id.find(clipping);
        if (it != uuid_to_id.end()) layer.clipping_source = it->second;
      }
    }

    comp_layer_id max_id = 0;
    for (const Layer& layer : state.layers) {
      if (layer.id > max_id) max_id = layer.id;
    }
    state.next_layer_id = max_id + 1;

    const std::string active = manifest.string_or("activeLayerID", "");
    if (!active.empty()) {
      const auto it = uuid_to_id.find(active);
      if (it != uuid_to_id.end()) state.active_layer = it->second;
    }
  }

  /* 重建组-子关系。 */
  for (Layer& layer : state.layers) layer.children.clear();
  for (Layer& layer : state.layers) {
    if (layer.parent == 0) continue;
    Layer* parent = doc->find(layer.parent);
    if (parent != nullptr && parent->is_group) parent->children.push_back(layer.id);
  }

  /*
   * S18：加载进来的每一层都要有内容修订号。修订号是**会话内**派生数据，不进
   * `.comp`——同一份工程两次加载得到不同的号是完全正常的（缓存只关心「本次会话
   * 里这份内容有没有变」）。这里必须发新号而不是留 0：留 0 会让所有层撞同一个
   * 缓存键，跨层串纹理。
   */
  doc->touch_all_layers();

  doc->mark_dirty(false);
  doc->history().clear();
  *out = doc.release(); /* 所有权移交调用方；失败路径由 unique_ptr 自动 destroy */
  return true;
}

bool probe_project(const std::string& path, ProjectInfo* info, std::string* error) {
  if (info == nullptr) {
    if (error != nullptr) *error = "out info pointer is null";
    return false;
  }
  const fs::path root = fs::u8path(path);
  std::string text;
  if (!read_text_file(manifest_path_of(root), &text, error)) return false;
  /* N3：与 load / save 保持同一上界（load 在 :449、save 在 :375 都查 4 MiB），
     否则超大 manifest 会在无上界的字符串分配上先炸。 */
  if (text.size() > kMaxManifestBytes) {
    if (error != nullptr) *error = "manifest exceeds 4 MiB";
    return false;
  }

  JsonValue manifest = json_parse(text, error);
  if (!manifest.is_object()) {
    if (error != nullptr && error->empty()) *error = "manifest root is not an object";
    return false;
  }
  if (manifest.string_or("format", "") != kFormatId) {
    if (error != nullptr) *error = "unexpected project format";
    return false;
  }

  info->format = kFormatId;
  info->version = static_cast<uint32_t>(manifest.integer_or("version", 0));
  info->width = static_cast<int32_t>(manifest.integer_or("width", 0));
  info->height = static_cast<int32_t>(manifest.integer_or("height", 0));
  info->dpi = static_cast<float>(clamp_resolution(manifest.number_or("resolution", 72.0)));
  info->color_space = manifest.string_or("colorSpace", kColorSpace);

  const JsonValue* layers = manifest.find("layers");
  info->layer_count = (layers != nullptr && layers->is_array())
                          ? static_cast<int32_t>(layers->as_array().size())
                          : 0;
  return true;
}

}  // namespace compositor
