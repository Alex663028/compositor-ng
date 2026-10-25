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
#include <cstring>
#include <string>

#include "comp_abi_internal.h"
#include "domain/filters.h"

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

