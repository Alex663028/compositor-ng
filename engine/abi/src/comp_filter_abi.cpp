// AI生成
/*
 * comp_filter_abi.cpp — Filter 菜单 C ABI（P1 滤镜批次）。
 *
 * 只做「句柄校验 -> 描述符拷贝 -> 领域调用 -> 错误码映射」：
 *   - 描述符逐字段拷贝成 domain 的 FilterSettings，量程校验留给领域层
 *     （ABI 不做第二套闸，免得两处口径漂移）；
 *   - 领域层自己开/提交事务（一步撤销），这里**不能**再包 ImplicitTxn，
 *     否则会叠出两条历史；
 *   - 领域层在异常路径上已 abort 自己的事务，所以本入口可以安全地用
 *     COMP_ABI_TRY / COMP_ABI_CATCH。
 */
#include "compositor/comp_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "comp_abi_internal.h"
#include "domain/filters.h"
#include "domain/layer.h"
#include "domain/utf8_text.h"

/* 头文件宏与领域常量必须同值：两处漂移会让宿主按旧上限截断，落进 `.comp`
 * 的名字与 ABI 接受的名字于是分叉。 */
static_assert(compositor::kMaxSmartFilterNameBytes == COMP_SMART_FILTER_NAME_MAX_BYTES,
              "smart filter name limit must match COMP_SMART_FILTER_NAME_MAX_BYTES");

namespace {

using compositor::Document;
using compositor::Engine;
using compositor::FilterSettings;

/* 两段式 note 拷贝：容量 0 时只写所需长度（含 '\0'）。 */
void copy_note(const std::string& text, char* note, size_t note_capacity, size_t* out_note_size) {
  if (out_note_size != nullptr) *out_note_size = text.size() + 1;
  if (note == nullptr || note_capacity == 0) return;
  const size_t copy = std::min(note_capacity - 1, text.size());
  if (copy > 0) std::memcpy(note, text.data(), copy);
  note[copy] = '\0';
}

/* 逐字段拷贝。只有 kind 对应的子结构被读取（与 domain 的约定一致）。 */
FilterSettings to_domain(const comp_filter_desc& desc) {
  FilterSettings settings;
  settings.kind = static_cast<compositor::FilterKind>(desc.kind);

  settings.vignette.amount = desc.vignette.amount;
  settings.vignette.midpoint = desc.vignette.midpoint;
  settings.vignette.roundness = desc.vignette.roundness;
  settings.vignette.feather = desc.vignette.feather;
  settings.vignette.highlights = desc.vignette.highlights;
  settings.vignette.color[0] = desc.vignette.color[0];
  settings.vignette.color[1] = desc.vignette.color[1];
  settings.vignette.color[2] = desc.vignette.color[2];

  settings.tonal_contrast.amount = desc.tonal_contrast.amount;
  settings.tonal_contrast.radius = desc.tonal_contrast.radius;
  settings.tonal_contrast.shadows = desc.tonal_contrast.shadows;
  settings.tonal_contrast.midtones = desc.tonal_contrast.midtones;
  settings.tonal_contrast.highlights = desc.tonal_contrast.highlights;

  settings.bloom.amount = desc.bloom.amount;
  settings.bloom.radius = desc.bloom.radius;

  settings.lens_correction.distortion = desc.lens_correction.distortion;

  settings.gaussian_blur.radius = desc.gaussian_blur.radius;
  settings.gaussian_blur.preserve_transparency = desc.gaussian_blur.preserve_transparency != 0;

  settings.add_noise.amount = desc.add_noise.amount;
  settings.add_noise.gaussian = desc.add_noise.gaussian != 0;
  settings.add_noise.monochromatic = desc.add_noise.monochromatic != 0;
  settings.add_noise.seed = desc.add_noise.seed;

  settings.grain.amount = desc.grain.amount;
  settings.grain.size = desc.grain.size;
  settings.grain.roughness = desc.grain.roughness;
  settings.grain.seed = desc.grain.seed;

  settings.motion_blur.angle = desc.motion_blur.angle;
  settings.motion_blur.distance = desc.motion_blur.distance;

  settings.progressive_blur.from = desc.progressive_blur.from;
  settings.progressive_blur.to = desc.progressive_blur.to;
  settings.progressive_blur.start_x = desc.progressive_blur.start_x;
  settings.progressive_blur.start_y = desc.progressive_blur.start_y;
  settings.progressive_blur.end_x = desc.progressive_blur.end_x;
  settings.progressive_blur.end_y = desc.progressive_blur.end_y;
  settings.progressive_blur.radial = desc.progressive_blur.radial != 0;
  settings.progressive_blur.inverted = desc.progressive_blur.inverted != 0;
  /* explicit_axis = 0（零初始化）用缺省轴，与域层 auto_axis = true 对齐。 */
  settings.progressive_blur.auto_axis = desc.progressive_blur.explicit_axis == 0;

  settings.sharpen.amount = desc.sharpen.amount;
  settings.sharpen.radius = desc.sharpen.radius;
  settings.sharpen.threshold = desc.sharpen.threshold;

  settings.reduce_noise.strength = desc.reduce_noise.strength;
  settings.reduce_noise.preserve_details = desc.reduce_noise.preserve_details;
  settings.reduce_noise.reduce_color_noise = desc.reduce_noise.reduce_color_noise;

  settings.high_pass.radius = desc.high_pass.radius;

  settings.minimum.radius = desc.minimum.radius;
  settings.maximum.radius = desc.maximum.radius;

  settings.offset.horizontal = desc.offset.horizontal;
  settings.offset.vertical = desc.offset.vertical;
  settings.offset.wrap = desc.offset.wrap;

  /* Custom：desc 全零 = 调用方没填 -> 保留域层的恒等核（零初始化 = 恒等，
   * 与 desc 其余字段的口径一致；见 comp_filter_custom 的注释）。 */
  bool custom_touched = desc.custom.scale != 0.0 || desc.custom.offset != 0.0;
  for (int i = 0; i < 25; ++i) {
    if (desc.custom.kernel[i] != 0.0) custom_touched = true;
  }
  if (custom_touched) {
    for (int i = 0; i < 25; ++i) settings.custom.kernel[i] = desc.custom.kernel[i];
    settings.custom.scale = desc.custom.scale;
    settings.custom.offset = desc.custom.offset;
  }

  /* A45：去噪族两个 kind 的参数（尾部追加，老宿主零初始化 = 半径 0 = 恒等）。
   * 注意：这两行**不在**上面的 custom 分支里——早前误缩进把 median/dust 的参数
   * 拷贝关进了 `if (custom_touched)`，导致「只调 Median 不碰 Custom」时半径被丢
   * 成 0、滤镜静默变恒等。A46 修正为先闭合 custom 分支再无条件拷贝。 */
  settings.median.radius = desc.median.radius;
  settings.median.threshold = desc.median.threshold;
  settings.dust_scratches.radius = desc.dust_scratches.radius;
  settings.dust_scratches.threshold = desc.dust_scratches.threshold;

  /* A46：扭曲 / 像素化 / 风格化组的参数（尾部追加，老宿主零初始化 = 恒等）。 */
  settings.spherize.amount = desc.spherize.amount;
  settings.pinch.amount = desc.pinch.amount;
  settings.twirl.angle = desc.twirl.angle;
  settings.mosaic.cell_size = desc.mosaic.cell_size;
  settings.emboss.amount = desc.emboss.amount;
  settings.emboss.angle = desc.emboss.angle;
  settings.emboss.height = desc.emboss.height;

  /* A47：渲染组三项的参数（尾部追加，老宿主零初始化 = 恒等）。 */
  settings.clouds.amount = desc.clouds.amount;
  settings.clouds.size = desc.clouds.size;
  settings.clouds.seed = desc.clouds.seed;
  settings.clouds.color_low[0] = desc.clouds.color_low[0];
  settings.clouds.color_low[1] = desc.clouds.color_low[1];
  settings.clouds.color_low[2] = desc.clouds.color_low[2];
  settings.clouds.color_high[0] = desc.clouds.color_high[0];
  settings.clouds.color_high[1] = desc.clouds.color_high[1];
  settings.clouds.color_high[2] = desc.clouds.color_high[2];
  settings.difference_clouds.amount = desc.difference_clouds.amount;
  settings.difference_clouds.size = desc.difference_clouds.size;
  settings.difference_clouds.seed = desc.difference_clouds.seed;
  settings.picture_frame.width = desc.picture_frame.width;
  settings.picture_frame.color[0] = desc.picture_frame.color[0];
  settings.picture_frame.color[1] = desc.picture_frame.color[1];
  settings.picture_frame.color[2] = desc.picture_frame.color[2];
  return settings;
}

}  // namespace

comp_result comp_filter_apply(comp_document document_handle, comp_layer_id target,
                              const comp_filter_desc* desc, double scale, int32_t* out_changed,
                              char* note, size_t note_capacity, size_t* out_note_size) {
  Document* document = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &document, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (desc == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "filter desc is null");
  }
  const FilterSettings settings = to_domain(*desc);
  /*
   * 图像锁的闸（P1-2）：滤镜整幅改写像素，与画笔同属「图像像素」写入口。
   * 闸放在领域调用之前——领域层自己开会提交事务，锁在这里拒绝就不会留下
   * 任何历史步；锁定透明像素（位 1）不拦滤镜，它只约束 alpha 的变化面，
   * 本轮滤镜不走那条还原路径（见 comp_layer.h 的能力边界）。
   */
  const compositor::Layer* target_layer = document->find(target);
  if (target_layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  const comp_result locked =
      compositor::abi::lock_gate(engine, target_layer, COMP_LAYER_LOCK_PIXELS, "apply a filter");
  if (locked != COMP_OK) return locked;
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note_text;
  std::string error;
  if (!compositor::filter_apply(document, target, settings, scale, &code, &changed, &note_text,
                                &error)) {
    return compositor::abi::fail(engine, code, error);
  }
  copy_note(note_text, note, note_capacity, out_note_size);
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* ========================================= 智能对象 / 智能滤镜（P1-2 收尾 ⑦） */

namespace {

using compositor::FilterSettings;
using compositor::ImageBuffer;
using compositor::Layer;
using compositor::SmartFilter;
using compositor::SmartObject;

/* to_domain 的逆：把整份设置写回 ABI 描述符（comp_layer_smart_filter_at 用）。
 * 与 to_domain 同一条「只有 kind 对应的子结构有意义」，这里把全部字段都填上，
 * 宿主不必按 kind 分支就知道「读到的就是存的那一份」。 */
comp_filter_desc from_domain(const FilterSettings& s) {
  comp_filter_desc desc{};
  desc.kind = static_cast<comp_filter_kind>(s.kind);

  desc.vignette.amount = s.vignette.amount;
  desc.vignette.midpoint = s.vignette.midpoint;
  desc.vignette.roundness = s.vignette.roundness;
  desc.vignette.feather = s.vignette.feather;
  desc.vignette.highlights = s.vignette.highlights;
  desc.vignette.color[0] = s.vignette.color[0];
  desc.vignette.color[1] = s.vignette.color[1];
  desc.vignette.color[2] = s.vignette.color[2];

  desc.tonal_contrast.amount = s.tonal_contrast.amount;
  desc.tonal_contrast.radius = s.tonal_contrast.radius;
  desc.tonal_contrast.shadows = s.tonal_contrast.shadows;
  desc.tonal_contrast.midtones = s.tonal_contrast.midtones;
  desc.tonal_contrast.highlights = s.tonal_contrast.highlights;

  desc.bloom.amount = s.bloom.amount;
  desc.bloom.radius = s.bloom.radius;

  desc.lens_correction.distortion = s.lens_correction.distortion;

  desc.gaussian_blur.radius = s.gaussian_blur.radius;
  desc.gaussian_blur.preserve_transparency = s.gaussian_blur.preserve_transparency ? 1 : 0;

  desc.add_noise.amount = s.add_noise.amount;
  desc.add_noise.gaussian = s.add_noise.gaussian ? 1 : 0;
  desc.add_noise.monochromatic = s.add_noise.monochromatic ? 1 : 0;
  desc.add_noise.seed = s.add_noise.seed;

  desc.grain.amount = s.grain.amount;
  desc.grain.size = s.grain.size;
  desc.grain.roughness = s.grain.roughness;
  desc.grain.seed = s.grain.seed;

  desc.motion_blur.angle = s.motion_blur.angle;
  desc.motion_blur.distance = s.motion_blur.distance;

  desc.progressive_blur.from = s.progressive_blur.from;
  desc.progressive_blur.to = s.progressive_blur.to;
  desc.progressive_blur.start_x = s.progressive_blur.start_x;
  desc.progressive_blur.start_y = s.progressive_blur.start_y;
  desc.progressive_blur.end_x = s.progressive_blur.end_x;
  desc.progressive_blur.end_y = s.progressive_blur.end_y;
  desc.progressive_blur.radial = s.progressive_blur.radial ? 1 : 0;
  desc.progressive_blur.inverted = s.progressive_blur.inverted ? 1 : 0;
  desc.progressive_blur.explicit_axis = s.progressive_blur.auto_axis ? 0 : 1;

  desc.sharpen.amount = s.sharpen.amount;
  desc.sharpen.radius = s.sharpen.radius;
  desc.sharpen.threshold = s.sharpen.threshold;

  desc.reduce_noise.strength = s.reduce_noise.strength;
  desc.reduce_noise.preserve_details = s.reduce_noise.preserve_details;
  desc.reduce_noise.reduce_color_noise = s.reduce_noise.reduce_color_noise;

  desc.high_pass.radius = s.high_pass.radius;

  desc.minimum.radius = s.minimum.radius;
  desc.maximum.radius = s.maximum.radius;

  desc.offset.horizontal = s.offset.horizontal;
  desc.offset.vertical = s.offset.vertical;
  desc.offset.wrap = s.offset.wrap;

  desc.median.radius = s.median.radius;
  desc.median.threshold = s.median.threshold;
  desc.dust_scratches.radius = s.dust_scratches.radius;
  desc.dust_scratches.threshold = s.dust_scratches.threshold;

  desc.spherize.amount = s.spherize.amount;
  desc.pinch.amount = s.pinch.amount;
  desc.twirl.angle = s.twirl.angle;
  desc.mosaic.cell_size = s.mosaic.cell_size;
  desc.emboss.amount = s.emboss.amount;
  desc.emboss.angle = s.emboss.angle;
  desc.emboss.height = s.emboss.height;

  /* A47：渲染组三项。Clouds 的 color_low / color_high 零初始化时域层缺省是黑/白，
   * 但这里是**回读**（from_domain）：把域层当前值原样写回，不重新造缺省。 */
  desc.clouds.amount = s.clouds.amount;
  desc.clouds.size = s.clouds.size;
  desc.clouds.seed = s.clouds.seed;
  desc.clouds.color_low[0] = s.clouds.color_low[0];
  desc.clouds.color_low[1] = s.clouds.color_low[1];
  desc.clouds.color_low[2] = s.clouds.color_low[2];
  desc.clouds.color_high[0] = s.clouds.color_high[0];
  desc.clouds.color_high[1] = s.clouds.color_high[1];
  desc.clouds.color_high[2] = s.clouds.color_high[2];
  desc.difference_clouds.amount = s.difference_clouds.amount;
  desc.difference_clouds.size = s.difference_clouds.size;
  desc.difference_clouds.seed = s.difference_clouds.seed;
  desc.picture_frame.width = s.picture_frame.width;
  desc.picture_frame.color[0] = s.picture_frame.color[0];
  desc.picture_frame.color[1] = s.picture_frame.color[1];
  desc.picture_frame.color[2] = s.picture_frame.color[2];
  return desc;
}

/* 把整条栈（只取启用的条目）重新求值到 out。失败不写 out。 */
bool evaluate_stack(const SmartObject& object, const std::vector<SmartFilter>& stack,
                    ImageBuffer* out, comp_result* out_code, std::string* error) {
  std::vector<FilterSettings> enabled;
  enabled.reserve(stack.size());
  for (const SmartFilter& filter : stack) {
    if (filter.enabled) enabled.push_back(filter.settings);
  }
  return compositor::filter_render_stack(object.source, enabled, 1.0, out, out_code, error);
}

/* 智能滤镜写入口的公共前置：图层存在且确实是智能对象。返回 nullptr 时
 * `out` 已是可返回的错误（调用方直接 return）。 */
Layer* smart_layer_or_error(compositor::Document* doc, comp_layer_id id,
                            compositor::Engine* engine, comp_result* out) {
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    *out = compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
    return nullptr;
  }
  if (!layer->has_smart_object()) {
    *out = compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "layer is not a smart object (see comp_layer_smart_object_create)");
    return nullptr;
  }
  return layer;
}

/*
 * 读智能滤镜展示名（P1-2 收尾 ⑩）。
 *
 * 与 read_layer_comp_name 同一读法：不信「内存里一定有 NUL」，先按上界扫再校验，
 * 免得宿主传进来一段没终止的缓冲时越界读。**这里允许空**：NULL / 空串 = 清除名字
 * （回退到滤镜类型名），与图层名「必须非空」不同——条目本身已由 kind 命名。
 */
bool read_smart_filter_name(const char* utf8, std::string* out, std::string* why) {
  out->clear();
  if (utf8 == nullptr) return true;
  size_t length = 0;
  while (length <= compositor::kMaxSmartFilterNameBytes && utf8[length] != '\0') ++length;
  if (length > compositor::kMaxSmartFilterNameBytes) {
    *why = "smart filter name exceeds COMP_SMART_FILTER_NAME_MAX_BYTES";
    return false;
  }
  if (length == 0) return true;
  std::string name(utf8, length);
  if (!compositor::utf8_is_valid(name)) {
    *why = "smart filter name is not valid UTF-8";
    return false;
  }
  if (!compositor::utf8_is_single_line(name)) {
    *why = "smart filter name must be single-line (no control characters)";
    return false;
  }
  *out = std::move(name);
  return true;
}

}  // namespace

comp_result comp_layer_smart_object_create(comp_document document_handle, comp_layer_id id,
                                           int32_t* out_changed) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 幂等：已经是智能对象时什么也不做（宿主不必先判断）。 */
  if (layer->has_smart_object()) {
    if (out_changed != nullptr) *out_changed = 0;
    return compositor::abi::ok(engine);
  }
  if (layer->is_group) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "group layer has no pixels");
  }
  /* 文字对象已经是可编辑真源：两层真源会让「改字号」与「删滤镜」互相抹掉对方
   * 的结果。先 rasterize 文字（那一步本来就会丢可编辑性）。 */
  if (layer->has_text()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "layer is an editable text layer; rasterize it first "
                                 "(comp_layer_rasterize_text)");
  }
  /* 填充层同理：参数已是真源，再叠一层 source 会让「改渐变颜色」与「删滤镜」
   * 互相抹掉对方的结果。先 rasterize 把画面固定下来。 */
  if (layer->has_fill()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "layer is a fill layer; rasterize it first "
                                 "(comp_layer_rasterize_fill)");
  }
  if (!layer->has_pixels()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "layer has no pixels");
  }
  if (layer->pixels.format == COMP_PIXEL_RGBA16F) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "smart objects need an 8-bit or 16-bit layer (an HDR layer has no "
                                 "8-bit filter path)");
  }
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "convert to a smart object");
    if (locked != COMP_OK) return locked;
  }
  /* source 与图层像素共享同一份缓冲（COW 保护）：转换本身不复制像素。 */
  ImageBuffer source = layer->pixels;
  if (source.format == COMP_PIXEL_RGBA16) {
    ImageBuffer eight;
    if (!compositor::deep_to_rgba8(source, &eight)) {
      return compositor::abi::fail(engine, COMP_ERR_OUT_OF_MEMORY,
                                   "smart object could not build the 8-bit working copy");
    }
    source = std::move(eight);
  }
  compositor::abi::ImplicitTxn txn(doc, "Convert to Smart Object");
  SmartObject object;
  object.source = source;
  layer->pixels = std::move(source);
  layer->smart = std::move(object);
  doc->touch_layer(id);
  txn.commit();
  if (out_changed != nullptr) *out_changed = 1;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_object_is_smart(comp_document document_handle, comp_layer_id id,
                                             int32_t* out_is_smart) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_is_smart == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_is_smart is null");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_is_smart = layer->has_smart_object() ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_object_rasterize(comp_document document_handle, comp_layer_id id) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 幂等：普通图层已经是栅格，不记撤销步。 */
  if (!layer->has_smart_object()) return compositor::abi::ok(engine);
  {
    const comp_result locked = compositor::abi::lock_gate(
        engine, layer, COMP_LAYER_LOCK_ALL, "rasterize the smart object");
    if (locked != COMP_OK) return locked;
  }
  compositor::abi::ImplicitTxn txn(doc, "Rasterize Smart Object");
  /* 只丢真源：pixels 是最后一次求值的结果，正是要留下的那份画面。 */
  layer->smart.reset();
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_count(comp_document document_handle, comp_layer_id id,
                                          size_t* out_count) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  *out_count = layer->smart->filters.size();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_at(comp_document document_handle, comp_layer_id id,
                                       size_t index, comp_filter_desc* out_desc,
                                       int32_t* out_enabled) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_desc == nullptr && out_enabled == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "no output requested (out_desc and out_enabled are both null)");
  }
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  if (index >= layer->smart->filters.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "smart filter index out of range");
  }
  const SmartFilter& filter = layer->smart->filters[index];
  if (out_desc != nullptr) *out_desc = from_domain(filter.settings);
  if (out_enabled != nullptr) *out_enabled = filter.enabled ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_add(comp_document document_handle, comp_layer_id id,
                                        const comp_filter_desc* desc) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (desc == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "filter desc is null");
  }
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  const FilterSettings settings = to_domain(*desc);
  std::string why;
  if (!compositor::filter_settings_valid(settings, &why)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, why);
  }
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS, "add a smart filter");
    if (locked != COMP_OK) return locked;
  }
  std::vector<SmartFilter> stack = layer->smart->filters;
  SmartFilter entry;
  entry.settings = settings;
  stack.push_back(entry);
  /* 先求值（无副作用），成功后才开事务：失败不留撤销步、不动像素。 */
  ImageBuffer rendered;
  std::string failure;
  if (!evaluate_stack(*layer->smart, stack, &rendered, &error, &failure)) {
    return compositor::abi::fail(engine, error, failure);
  }
  compositor::abi::ImplicitTxn txn(doc, "Add Smart Filter");
  layer->smart->filters = std::move(stack);
  layer->pixels = std::move(rendered);
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_set(comp_document document_handle, comp_layer_id id,
                                        size_t index, const comp_filter_desc* desc,
                                        int32_t enabled) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (desc == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "filter desc is null");
  }
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  if (index >= layer->smart->filters.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "smart filter index out of range");
  }
  const FilterSettings settings = to_domain(*desc);
  std::string why;
  if (!compositor::filter_settings_valid(settings, &why)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, why);
  }
  const bool enabled_flag = enabled != 0;
  const SmartFilter& current = layer->smart->filters[index];
  /* 同值重设：不重新求值、不落撤销步（宿主可以无脑「设为当前面板值」）。 */
  if (current.enabled == enabled_flag &&
      compositor::filter_settings_equal(current.settings, settings)) {
    return compositor::abi::ok(engine);
  }
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS, "update a smart filter");
    if (locked != COMP_OK) return locked;
  }
  std::vector<SmartFilter> stack = layer->smart->filters;
  stack[index].settings = settings;
  stack[index].enabled = enabled_flag;
  ImageBuffer rendered;
  std::string failure;
  if (!evaluate_stack(*layer->smart, stack, &rendered, &error, &failure)) {
    return compositor::abi::fail(engine, error, failure);
  }
  compositor::abi::ImplicitTxn txn(doc, "Update Smart Filter");
  layer->smart->filters = std::move(stack);
  layer->pixels = std::move(rendered);
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_remove(comp_document document_handle, comp_layer_id id,
                                           size_t index) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  if (index >= layer->smart->filters.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "smart filter index out of range");
  }
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS, "remove a smart filter");
    if (locked != COMP_OK) return locked;
  }
  std::vector<SmartFilter> stack = layer->smart->filters;
  stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
  ImageBuffer rendered;
  std::string failure;
  if (!evaluate_stack(*layer->smart, stack, &rendered, &error, &failure)) {
    return compositor::abi::fail(engine, error, failure);
  }
  compositor::abi::ImplicitTxn txn(doc, "Remove Smart Filter");
  layer->smart->filters = std::move(stack);
  layer->pixels = std::move(rendered);
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* ------------------------------------ 栈序与命名（P1-2 收尾 ⑩，尾部追加） */

comp_result comp_layer_smart_filter_move(comp_document document_handle, comp_layer_id id,
                                         size_t from, size_t to) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  const size_t count = layer->smart->filters.size();
  if (from >= count || to >= count) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "smart filter index out of range");
  }
  /* 原地不动：不重新求值、不落撤销步（宿主可无脑「拖回原位」）。 */
  if (from == to) return compositor::abi::ok(engine);
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS, "reorder smart filters");
    if (locked != COMP_OK) return locked;
  }
  std::vector<SmartFilter> stack = layer->smart->filters;
  /* 「取出再插入」：from < to 时 erase 后目标下标已左移，insert 到 `to` 正好
   * 落到用户期望的最终位置（与 std::vector 的常用拖拽语义一致）。 */
  SmartFilter moved = std::move(stack[from]);
  stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(from));
  stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
  /* 顺序会改画面（非交换律的滤镜），必须整条重跑；失败先于事务返回。 */
  ImageBuffer rendered;
  std::string failure;
  if (!evaluate_stack(*layer->smart, stack, &rendered, &error, &failure)) {
    return compositor::abi::fail(engine, error, failure);
  }
  compositor::abi::ImplicitTxn txn(doc, "Reorder Smart Filters");
  layer->smart->filters = std::move(stack);
  layer->pixels = std::move(rendered);
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_set_name(comp_document document_handle, comp_layer_id id,
                                             size_t index, const char* name_utf8) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  if (index >= layer->smart->filters.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "smart filter index out of range");
  }
  std::string name;
  std::string why;
  if (!read_smart_filter_name(name_utf8, &name, &why)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, why);
  }
  /* 同值重设：不落撤销步（宿主可以每帧把输入框内容写回来）。 */
  if (layer->smart->filters[index].name == name) return compositor::abi::ok(engine);
  /*
   * 只改元数据：不重新求值、不过像素锁闸。名字与画面无关，锁「图像像素」的
   * 图层照样能改名（与色标/链接同一口径）。仍进快照，所以撤销会退回旧名字。
   */
  compositor::abi::ImplicitTxn txn(doc, "Rename Smart Filter");
  layer->smart->filters[index].name = std::move(name);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_smart_filter_name(comp_document document_handle, comp_layer_id id,
                                         size_t index, char* buffer, size_t capacity,
                                         size_t* out_required) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_required == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_required is null");
  }
  comp_result error = COMP_OK;
  Layer* layer = smart_layer_or_error(doc, id, engine, &error);
  if (layer == nullptr) return error;
  if (index >= layer->smart->filters.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "smart filter index out of range");
  }
  const std::string& name = layer->smart->filters[index].name;
  const size_t required = name.size() + 1;
  *out_required = required;
  /* 查询模式：buffer 为 NULL 或容量 0 时只回报长度（含 '\0'）。 */
  if (buffer == nullptr || capacity == 0) return compositor::abi::ok(engine);
  if (capacity < required) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "name buffer is too small (see out_required)");
  }
  std::memcpy(buffer, name.c_str(), required);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

