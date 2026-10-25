// AI生成
/*
 * comp_fill_abi.cpp — 填充图层（comp_fill_* / comp_layer_*_fill）的 C ABI 实现。
 *
 * 语义与文字图层、智能对象同族（见 domain/fill_layer.h）：`kind + settings` 是
 * 唯一真源，图层像素是它们渲出的缓存。因此：
 *   - 所有「改参数」的入口都先校验、再渲染、最后才开事务——渲染失败时什么都不
 *     发生（不建层、不消耗图层 id、不进撤销栈），与 comp_layer_add_text 同一条
 *     「参数闸先于副作用」契约；
 *   - 直接从参数表驱动的读接口（kind_name / param_count / param_at /
 *     read_value / build_settings）都是纯函数，不需要文档，也不开事务。
 */
#include "compositor/comp_layer.h"

#include <cstring>
#include <string>
#include <vector>

#include "comp_abi_internal.h"

#include "core/comp_geometry.h"
#include "domain/fill_layer.h"

using compositor::Document;
using compositor::Engine;
using compositor::ImageBuffer;
using compositor::Layer;

namespace {

/* 两段式写文本：out_size 恒写正文长度（不含 NUL），缓冲不够时截断写入
 * （与 comp_layer_adjustment_at / build_settings 同口径）。 */
void write_text(const std::string& text, char* buffer, size_t capacity, size_t* out_size) {
  if (out_size != nullptr) *out_size = text.size();
  if (buffer == nullptr || capacity == 0) return;
  const size_t copy = text.size() < capacity - 1 ? text.size() : capacity - 1;
  std::memcpy(buffer, text.data(), copy);
  buffer[copy] = '\0';
}

/* set_fill 的重渲尺寸：沿用图层当前的像素缓存尺寸（缩画布会把它重采样到新画布
 * 尺寸；用户对图层做过缩放时也得按缓存尺寸重渲，否则画面会跳变）。缓存为空时
 * 回落到文档画布。 */
void render_target_size(const Document& doc, const Layer& layer, int32_t* out_width,
                        int32_t* out_height) {
  if (!layer.pixels.empty()) {
    *out_width = layer.pixels.width;
    *out_height = layer.pixels.height;
    return;
  }
  *out_width = doc.state().width;
  *out_height = doc.state().height;
}

}  // namespace

/* -------------------------------------------------- 参数表（只读纯函数） */

comp_result comp_fill_kind_name(uint32_t kind, const char** out_name) {
  COMP_ABI_TRY(nullptr)
  const char* name = compositor::fill_kind_name(kind);
  if (name == nullptr || out_name == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_name = name;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_fill_kind_param_count(uint32_t kind, size_t* out_count) {
  COMP_ABI_TRY(nullptr)
  const char* name = compositor::fill_kind_name(kind); /* 越界闸 */
  if (name == nullptr || out_count == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  const compositor::FillKindInfo* info = compositor::fill_kind_info(kind);
  *out_count = info != nullptr ? info->param_count : 0u;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_fill_kind_param_at(uint32_t kind, size_t index, comp_fill_param* out_param) {
  COMP_ABI_TRY(nullptr)
  const compositor::FillKindInfo* info = compositor::fill_kind_info(kind);
  if (info == nullptr || out_param == nullptr || index >= info->param_count) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  const compositor::FillParam& param = info->params[index];
  out_param->json_path = param.json_path;
  out_param->label = param.label;
  out_param->type = static_cast<comp_param_type>(param.type);
  out_param->min_value = param.min_value;
  out_param->max_value = param.max_value;
  out_param->default_value = param.default_value;
  out_param->flags = 0;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_fill_validate(uint32_t kind, const char* settings_json_utf8) {
  COMP_ABI_TRY(nullptr)
  std::string error;
  const bool ok = compositor::fill_validate_settings(
      kind, settings_json_utf8 != nullptr ? settings_json_utf8 : "{}", &error);
  return ok ? COMP_OK : COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_fill_default_settings(uint32_t kind, char* settings_json_utf8, size_t capacity,
                                       size_t* out_size) {
  COMP_ABI_TRY(nullptr)
  if (compositor::fill_kind_name(kind) == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  write_text(compositor::fill_default_settings_json(kind), settings_json_utf8, capacity, out_size);
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_fill_read_value(const char* settings_json_utf8, const char* json_path,
                                 double* out_number, int32_t* out_boolean) {
  if (json_path == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(nullptr)
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  if (!compositor::fill_read_value(settings, json_path, out_number, out_boolean)) {
    return COMP_ERR_NOT_FOUND;
  }
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_fill_build_settings(uint32_t kind, const char* const* values, size_t value_count,
                                     char* settings_json_utf8, size_t capacity, size_t* out_size) {
  COMP_ABI_TRY(nullptr)
  const compositor::FillKindInfo* info = compositor::fill_kind_info(kind);
  if (info == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  std::vector<std::string> flat;
  flat.reserve(info->param_count);
  for (size_t i = 0; i < info->param_count; ++i) {
    const char* text = (values != nullptr && i < value_count) ? values[i] : nullptr;
    flat.emplace_back(text != nullptr ? text : "");
  }

  std::string json;
  std::string error;
  if (!compositor::fill_build_settings(kind, flat, &json, &error)) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  write_text(json, settings_json_utf8, capacity, out_size);
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

/* -------------------------------------------------------- 图层写入口 */

comp_result comp_layer_add_fill(comp_document document, uint32_t kind,
                                const char* settings_json_utf8, comp_layer_id* out_id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  const compositor::FillKindInfo* info = compositor::fill_kind_info(kind);
  if (info == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "fill kind must be 1 (solid), 2 (gradient) or 3 (pattern)");
  }
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";

  /* 渲染在事务之前：参数非法/尺寸非法/分配失败时什么都不发生。 */
  ImageBuffer buffer;
  std::string error;
  if (!compositor::fill_render(kind, settings, doc->state().width, doc->state().height, &buffer,
                               &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 error.empty() ? "fill render failed" : error);
  }
  const comp_rect bounds{0, 0, doc->state().width, doc->state().height};
  if (!compositor::is_valid_transform(bounds, 0.0f)) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                 "fill layer exceeds the maximum layer size");
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = compositor::fill_layer_default_name(kind);
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = bounds;
  layer_desc.rotation = 0.0f;
  layer_desc.flip_x = 0;
  layer_desc.flip_y = 0;
  layer_desc.is_group = 0;
  layer_desc._reserved = 0;
  layer_desc.parent = 0;
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;

  compositor::abi::ImplicitTxn txn(doc, "Add Fill Layer");
  comp_layer_id id = 0;
  Layer* layer = doc->create_layer(layer_desc, &id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "failed to create the fill layer");
  }
  /* create_layer 铺的是一张全透明缓冲；填充层直接整体替换成渲染结果。 */
  layer->pixels = std::move(buffer);
  layer->fill = compositor::FillLayer{kind, settings};
  layer->clipped_empty = false;
  doc->touch_layer(id);
  txn.commit();
  if (out_id != nullptr) *out_id = id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_fill(comp_document document, comp_layer_id id, uint32_t kind,
                                const char* settings_json_utf8, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (!layer->has_fill()) {
    /* 不把普通图层的像素悄悄换成填充：会抹掉用户已有内容，宿主也看不出发生了什么。
     * 要新建填充层用 comp_layer_add_fill。 */
    return compositor::abi::fail(
        engine, COMP_ERR_STATE,
        "layer is not a fill layer (use comp_layer_add_fill to create one)");
  }
  if (compositor::fill_kind_info(kind) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "fill kind must be 1 (solid), 2 (gradient) or 3 (pattern)");
  }
  /* 改参数 = 重写整层像素缓存：归「锁定图像像素」管（LOCK_ALL 经蕴含规则同样命中）。 */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS, "change the fill");
    if (locked != COMP_OK) return locked;
  }
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";

  /* 幂等：参数一字未变就不重渲、不落撤销步（面板连续回调同一值时很有用）。 */
  if (layer->fill->kind == kind && layer->fill->settings_json == settings) {
    if (out_changed != nullptr) *out_changed = 0;
    return compositor::abi::ok(engine);
  }

  int32_t width = 0;
  int32_t height = 0;
  render_target_size(*doc, *layer, &width, &height);
  ImageBuffer buffer;
  std::string error;
  if (!compositor::fill_render(kind, settings, width, height, &buffer, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 error.empty() ? "fill render failed" : error);
  }

  compositor::abi::ImplicitTxn txn(doc, "Set Fill");
  layer->pixels = std::move(buffer);
  layer->fill = compositor::FillLayer{kind, settings};
  /* 重渲后这一层确实有内容：解掉缩画布留下的空矩形标记（与 set_text 同口径）。 */
  layer->clipped_empty = false;
  doc->touch_layer(id);
  txn.commit();
  if (out_changed != nullptr) *out_changed = 1;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_fill(comp_document document, comp_layer_id id, uint32_t* out_kind,
                                char* settings_json_utf8, size_t capacity, size_t* out_size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (!layer->has_fill()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "layer is not a fill layer");
  }
  if (out_kind != nullptr) *out_kind = layer->fill->kind;
  write_text(layer->fill->settings_json, settings_json_utf8, capacity, out_size);
  return compositor::abi::ok(engine);
}

comp_result comp_layer_is_fill(comp_document document, comp_layer_id id, int32_t* out_is_fill) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (out_is_fill != nullptr) *out_is_fill = layer->has_fill() ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_layer_rasterize_fill(comp_document document, comp_layer_id id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 幂等：本来就不是填充层 -> 成功且不落撤销步（与 rasterize_text 同口径）。 */
  if (!layer->has_fill()) return compositor::abi::ok(engine);
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "rasterize the fill layer");
    if (locked != COMP_OK) return locked;
  }

  compositor::abi::ImplicitTxn txn(doc, "Rasterize Fill");
  /* 像素缓存就是渲染结果，原样保留；只丢掉「参数是真源」这层关系。 */
  layer->fill.reset();
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}
