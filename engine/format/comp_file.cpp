// AI生成
#include "format/comp_file.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "compositing/blend.h"
#include "compositing/resample.h"
#include "core/comp_geometry.h"
#include "domain/document.h"
#include "engine.h"
#include "format/comp_json.h"
#include "format/comp_png.h"
#include "format/comp_uuid.h"

namespace fs = std::filesystem;

namespace compositor {

namespace {

constexpr uint32_t kManifestVersion = 13;
constexpr const char* kFormatId = "com.compositor.project";
constexpr const char* kColorSpace = "sRGB";
constexpr int64_t kMaxTransformSize = 300000;   /* 上游 LayerTransform.isValid */
constexpr int64_t kMaxTransformOrigin = 1000000;
constexpr int64_t kMaxLayers = 10000;
constexpr int64_t kMaxManifestBytes = 4 * 1024 * 1024;

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
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error != nullptr) *error = "cannot open for read: " + path.u8string();
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *text = buffer.str();
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

/* 上游把图层资产放在 images/ 子目录，且文件名必须由该图层自己的 uuid 构成。 */
std::string pixel_file_name(const std::string& uuid) { return uuid + ".png"; }
std::string mask_file_name(const std::string& uuid) { return uuid + ".mask.png"; }

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

void transform_from_json(const JsonValue& entry, Layer* layer) {
  const JsonValue* transform = entry.find("transform");
  if (transform == nullptr || !transform->is_object()) return;
  const JsonValue* origin = transform->find("origin");
  if (origin != nullptr && origin->is_array() && origin->as_array().size() >= 2) {
    layer->transform.x = static_cast<int32_t>(origin->as_array()[0].as_integer(0));
    layer->transform.y = static_cast<int32_t>(origin->as_array()[1].as_integer(0));
  }
  const JsonValue* size = transform->find("size");
  if (size != nullptr && size->is_array() && size->as_array().size() >= 2) {
    layer->transform.w = static_cast<int32_t>(size->as_array()[0].as_integer(0));
    layer->transform.h = static_cast<int32_t>(size->as_array()[1].as_integer(0));
  }
  layer->rotation = static_cast<float>(transform->number_or("rotation", 0.0));
  layer->flip_x = transform->bool_or("flipX", false);
  layer->flip_y = transform->bool_or("flipY", false);
  layer->sampling = sampling_to_abi(
      sampling_from_manifest_name(transform->string_or("sampling", "High quality")));
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
    j.set("imageFile", JsonValue::string(pixel_file_name(uuid)));
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

  JsonValue layers = JsonValue::array({});
  for (const Layer& layer : state.layers) {
    const std::string uuid = layer_uuid[layer.id];

    if (!layer.pixels.empty()) {
      if (layer.pixels.format != COMP_PIXEL_RGBA8) {
        if (error != nullptr) {
          *error = "only RGBA8 layers can be written as PNG (layer id " +
                   std::to_string(static_cast<unsigned long long>(layer.id)) + ")";
        }
        return false;
      }
      std::vector<uint8_t> png;
      if (!png_encode_rgba8(layer.pixels.pixel_data().data(), layer.pixels.width, layer.pixels.height,
                            &png, error)) {
        return false;
      }
      if (!write_bytes_file(images_dir / fs::u8path(pixel_file_name(uuid)), png.data(), png.size(),
                            error)) {
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

/* 读取一个图层的像素（PNG）。uuid 为该层 UUID，用于定位资产文件。 */
bool read_layer_pixels(const JsonValue& entry, const fs::path& root, const std::string& uuid,
                       Layer* layer, std::string* error) {
  const std::string image_file = entry.string_or("imageFile", "");
  if (image_file.empty()) return true;
  /* 上游规定 imageFile 只能是 "<uuid>.png"（version 13 亦可为 .exr，但内核不支持 EXR）。 */
  if (image_file.size() >= 4 && image_file.compare(image_file.size() - 4, 4, ".exr") == 0) {
    if (error != nullptr) *error = "EXR layers are not supported by this build: " + image_file;
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_bytes_file(images_dir_of(root) / fs::u8path(image_file), &bytes, error)) return false;

  PngImage decoded;
  if (!png_decode_rgba8(bytes.data(), bytes.size(), &decoded, error)) return false;

  layer->pixels.resize(decoded.width, decoded.height, COMP_PIXEL_RGBA8);
  if (!decoded.rgba.empty()) {
    std::memcpy(layer->pixels.writable_pixels().data(), decoded.rgba.data(), decoded.rgba.size());
  }
  return true;
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

bool load_project(const std::string& path, Engine* engine, std::unique_ptr<Document>* out,
                  std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out document pointer is null";
    return false;
  }

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

  auto doc = std::unique_ptr<Document>(new Document(engine, width, height,
                                                   static_cast<float>(clamp_resolution(
                                                       manifest.number_or("resolution", 72.0))),
                                                   color_space));
  DocumentState& state = doc->state();
  state.layers.clear();
  state.document_id = document_id;

  const JsonValue* layers = manifest.find("layers");
  if (layers != nullptr && layers->is_array()) {
    if (static_cast<int64_t>(layers->as_array().size()) > kMaxLayers) {
      if (error != nullptr) *error = "too many layers in manifest";
      return false;
    }

    std::unordered_map<std::string, comp_layer_id> uuid_to_id;
    std::vector<std::string> uuid_of_layer;
    uuid_of_layer.reserve(layers->as_array().size());

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
      transform_from_json(entry, &layer);
      layer.mask_source_uuid = entry.string_or("maskSourceID", "");

      if (!read_layer_pixels(entry, root, uuid, &layer, error)) return false;
      if (!read_layer_mask(entry, root, &layer, error)) return false;

      /* 引擎扩展：格式、调整、效果。 */
      const long long pixel_format = entry.integer_or("enginePixelFormat", COMP_PIXEL_RGBA8);
      /*
       * 像素格式必须先校验再写入：非法枚举会让 bytes_per_pixel() 返回 0，
       * stride() 与 byte_size() 随之全零，composite_layer 按 format 计算的
       * 步长与实际缓冲不符——一个畸形文件就能让读路径越界写堆。
       * 识别得出但不支持的格式明确报错，不静默按 RGBA8 近似。
       */
      if (!layer.pixels.empty() &&
          pixel_format != static_cast<long long>(COMP_PIXEL_RGBA8)) {
        if (pixel_format != static_cast<long long>(COMP_PIXEL_RGBA16) &&
            pixel_format != static_cast<long long>(COMP_PIXEL_RGBA16F)) {
          if (error != nullptr) *error = "layer " + layer.name + ": unknown enginePixelFormat";
          return false;
        }
        layer.pixels.format = static_cast<comp_pixel_format>(pixel_format);
      }
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

      uuid_to_id[uuid] = layer.id;
      uuid_of_layer.push_back(uuid);
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

  doc->mark_dirty(false);
  doc->history().clear();
  *out = std::move(doc);
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
