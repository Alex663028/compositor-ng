// AI生成
/*
 * comp_layer_abi.cpp — comp_layer.h 的 C ABI 实现。
 *
 * 约定：所有写操作都落在事务内。显式事务（comp_txn_begin）由调用方管理；
 * 否则每次调用自动构成一个隐式撤销步（见 abi::ImplicitTxn）。
 */
#include "compositor/comp_layer.h"

#include <algorithm>
#include <cstring>

#include "comp_abi_internal.h"
#include "compositing/blend.h"
#include "core/comp_geometry.h"
#include "domain/adjustment_table.h"

using compositor::Document;
using compositor::DocumentState;
using compositor::Engine;
using compositor::Layer;

namespace {

bool valid_blend(comp_blend_mode mode) { return compositor::is_blend_supported(mode); }

/* sampling 枚举校验：comp_sampling 的数值是 ABI 契约的一部分，
   与 comp_layer_set_sampling 共用同一套白名单。 */
bool valid_sampling(comp_sampling sampling) {
  return sampling == COMP_SAMPLING_NEAREST || sampling == COMP_SAMPLING_SMOOTH ||
         sampling == COMP_SAMPLING_HIGH_QUALITY;
}

float clamp_opacity(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

}  // namespace

extern "C" {

comp_result comp_layer_add(comp_document document, const comp_layer_desc* desc,
                          comp_layer_id* out_id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (desc == nullptr || out_id == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "desc or out_id is null");
  }
  if (desc->parent != 0) {
    const Layer* parent = doc->find(desc->parent);
    if (parent == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "parent layer not found");
    }
    if (!parent->is_group) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "parent is not a group");
    }
  }
  if (!valid_blend(desc->blend)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid blend mode");
  }
  /* sampling 与 _reserved 同样在入口把关：非法枚举一旦入库会被 .comp 持久化，
     之后读路径无从判定它是哪一版 ABI 的值。 */
  if (!valid_sampling(desc->sampling)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid sampling mode");
  }
  if (desc->_reserved != 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "_reserved must be 0");
  }

  compositor::abi::ImplicitTxn txn(doc, "Add Layer");
  comp_layer_id id = 0;
  Layer* layer = doc->create_layer(*desc, &id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "failed to create layer");
  }
  txn.commit();
  *out_id = id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_remove(comp_document document, comp_layer_id id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }

  compositor::abi::ImplicitTxn txn(doc, "Remove Layer");
  if (!doc->remove_layer(id)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "remove layer failed");
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_reorder(comp_document document, comp_layer_id id, comp_layer_id before) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (before != 0 && doc->find(before) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "target layer not found");
  }

  compositor::abi::ImplicitTxn txn(doc, "Reorder Layer");
  if (!doc->reorder_layer(id, before)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "reorder layer failed");
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_pixels(comp_document document, comp_layer_id id,
                                 const comp_image* image) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Layer Pixels");
  if (image == nullptr) {
    /* 清空为全透明：保留既有尺寸；尚无缓冲则按画布尺寸建立。 */
    if (layer->pixels.empty()) {
      layer->pixels.resize(doc->state().width, doc->state().height, COMP_PIXEL_RGBA8);
    }
    layer->pixels.clear_transparent();
  } else {
    std::string error;
    if (!compositor::abi::copy_image_into(*image, &layer->pixels, &error)) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
    }
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_mask(comp_document document, comp_layer_id id, const comp_mask* mask) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Layer Mask");
  if (mask == nullptr) {
    layer->mask = compositor::MaskBuffer{};
  } else {
    std::string error;
    if (!compositor::abi::copy_mask_into(*mask, &layer->mask, &error)) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
    }
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_clipping_source(comp_document document, comp_layer_id id,
                                          comp_layer_id source) {
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
  if (source != 0) {
    if (source == id) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                   "clipping source cannot be the layer itself");
    }
    if (doc->find(source) == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "clipping source not found");
    }
  }

  compositor::abi::ImplicitTxn txn(doc, "Set Clipping Source");
  layer->clipping_source = source;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_blend(comp_document document, comp_layer_id id,
                                comp_blend_mode blend) {
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
  if (!valid_blend(blend)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid blend mode");
  }

  compositor::abi::ImplicitTxn txn(doc, "Set Blend Mode");
  layer->blend = blend;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_opacity(comp_document document, comp_layer_id id, float opacity) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Opacity");
  layer->opacity = clamp_opacity(opacity);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_transform(comp_document document, comp_layer_id id,
                                    comp_rect transform, float rotation) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Transform");
  layer->transform = transform;
  /* 显式给回尺寸 = 用户把图层放回画布，清掉缩画布留下的「已裁空」标记，
   * 否则 effective_bounds 会一直返回空矩形。 */
  layer->clipped_empty = false;
  layer->rotation = rotation;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_visible(comp_document document, comp_layer_id id, int32_t visible) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Visibility");
  layer->visible = visible != 0;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_visible(comp_document document, comp_layer_id id,
                                  int32_t* out_visible) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_visible == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_visible is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_visible = layer->visible ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_layer_set_sampling(comp_document document, comp_layer_id id,
                                    comp_sampling sampling) {
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
  if (!valid_sampling(sampling)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid sampling mode");
  }

  compositor::abi::ImplicitTxn txn(doc, "Set Sampling");
  layer->sampling = sampling;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_sampling(comp_document document, comp_layer_id id,
                                    comp_sampling* out_sampling) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_sampling == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_sampling is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_sampling = layer->sampling;
  return compositor::abi::ok(engine);
}

comp_result comp_layer_add_adjustment(comp_document document, comp_layer_id target, uint32_t kind,
                                     const char* settings_json_utf8) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  Layer* layer = doc->find(target);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }

  /* kind 与 settings 键名/取值范围都按键名表校验（键名逐字对齐上游 LayerAdjustment）。 */
  const char* settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  std::string settings_error;
  if (!compositor::adjustment_validate_settings(kind, settings, &settings_error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, settings_error.c_str());
  }

  compositor::abi::ImplicitTxn txn(doc, "Add Adjustment");
  compositor::Adjustment adjustment;
  adjustment.kind = kind;
  adjustment.settings_json = settings;
  layer->adjustments.push_back(std::move(adjustment));
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_update_adjustment(comp_document document, comp_layer_id id,
                                        const char* settings_json_utf8) {
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
  if (layer->adjustments.empty()) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer has no adjustment");
  }

  /* 沿用既有 kind，只换 settings（校验同 add）。 */
  const char* settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  std::string settings_error;
  if (!compositor::adjustment_validate_settings(layer->adjustments.front().kind, settings,
                                                &settings_error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, settings_error.c_str());
  }

  compositor::abi::ImplicitTxn txn(doc, "Update Adjustment");
  layer->adjustments.front().settings_json = settings;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_update_adjustment_at(comp_document document, comp_layer_id id, size_t index,
                                            uint32_t kind, const char* settings_json_utf8) {
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
  if (index >= layer->adjustments.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "adjustment index out of range");
  }

  const char* settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  std::string settings_error;
  if (!compositor::adjustment_validate_settings(kind, settings, &settings_error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, settings_error.c_str());
  }

  compositor::abi::ImplicitTxn txn(doc, "Update Adjustment");
  layer->adjustments[index].kind = kind;
  layer->adjustments[index].settings_json = settings;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_remove_adjustment(comp_document document, comp_layer_id id, size_t index) {
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
  if (index >= layer->adjustments.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "adjustment index out of range");
  }

  compositor::abi::ImplicitTxn txn(doc, "Remove Adjustment");
  layer->adjustments.erase(layer->adjustments.begin() + static_cast<std::ptrdiff_t>(index));
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_adjustment_read_value(const char* settings_json_utf8, const char* json_path,
                                             double* out_number, int32_t* out_boolean) {
  if (json_path == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  /* 无引擎上下文的纯函数：std::string 构造 + JSON 解析会抛，护栏传 nullptr。 */
  COMP_ABI_TRY(nullptr)
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  if (!compositor::adjustment_read_value(settings, json_path, out_number, out_boolean)) {
    return COMP_ERR_NOT_FOUND;
  }
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_adjustment_build_settings(uint32_t kind, const char* const* values,
                                                 size_t value_count, char* settings_json_utf8,
                                                 size_t capacity, size_t* out_size) {
  COMP_ABI_TRY(nullptr)
  const compositor::AdjustmentKindInfo* info = compositor::adjustment_kind_info(kind);
  if (info == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  std::vector<std::string> flat;
  flat.reserve(info->param_count);
  for (size_t i = 0; i < info->param_count; ++i) {
    const char* text = (values != nullptr && i < value_count) ? values[i] : nullptr;
    flat.emplace_back(text != nullptr ? text : "");
  }

  std::string json;
  std::string error;
  if (!compositor::adjustment_build_settings(kind, flat, &json, &error)) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_size != nullptr) *out_size = json.size();
  if (settings_json_utf8 != nullptr && capacity > 0) {
    const size_t copy = json.size() < capacity - 1 ? json.size() : capacity - 1;
    std::memcpy(settings_json_utf8, json.data(), copy);
    settings_json_utf8[copy] = '\0';
  }
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

/* ------------------------------------------------- 调整参数元数据（只读） */

comp_result comp_layer_adjustment_kind_name(uint32_t kind, const char** out_name) {
  COMP_ABI_TRY(nullptr)
  const compositor::AdjustmentKindInfo* info = compositor::adjustment_kind_info(kind);
  if (info == nullptr || out_name == nullptr) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  *out_name = info->name;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_adjustment_param_count(uint32_t kind, size_t* out_count) {
  COMP_ABI_TRY(nullptr)
  const compositor::AdjustmentKindInfo* info = compositor::adjustment_kind_info(kind);
  if (info == nullptr || out_count == nullptr) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  *out_count = info->param_count;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_adjustment_param_at(uint32_t kind, size_t index,
                                           comp_adjustment_param* out_param) {
  COMP_ABI_TRY(nullptr)
  const compositor::AdjustmentKindInfo* info = compositor::adjustment_kind_info(kind);
  if (info == nullptr || out_param == nullptr || index >= info->param_count) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  const compositor::AdjustmentParam& param = info->params[index];
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

comp_result comp_layer_adjustment_validate(uint32_t kind, const char* settings_json_utf8) {
  COMP_ABI_TRY(nullptr)
  std::string error;
  const bool ok = compositor::adjustment_validate_settings(
      kind, settings_json_utf8 != nullptr ? settings_json_utf8 : "{}", &error);
  return ok ? COMP_OK : COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_adjustment_count(comp_document document, comp_layer_id id,
                                        size_t* out_count) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_count = layer->adjustments.size();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_adjustment_at(comp_document document, comp_layer_id id, size_t index,
                                     uint32_t* out_kind, char* settings_json_utf8, size_t capacity,
                                     size_t* out_size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (index >= layer->adjustments.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "adjustment index out of range");
  }
  const compositor::Adjustment& adjustment = layer->adjustments[index];
  if (out_kind != nullptr) {
    *out_kind = adjustment.kind;
  }
  const std::string& settings = adjustment.settings_json;
  if (out_size != nullptr) {
    *out_size = settings.size();
  }
  if (settings_json_utf8 != nullptr && capacity > 0) {
    const size_t copy = settings.size() < capacity - 1 ? settings.size() : capacity - 1;
    std::memcpy(settings_json_utf8, settings.data(), copy);
    settings_json_utf8[copy] = '\0';
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_effect(comp_document document, comp_layer_id id, uint32_t effect,
                                 const char* settings_json_utf8) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Layer Effect");
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  bool updated = false;
  for (compositor::Effect& existing : layer->effects) {
    if (existing.effect == effect) {
      existing.settings_json = settings;
      updated = true;
      break;
    }
  }
  if (!updated) {
    compositor::Effect created;
    created.effect = effect;
    created.settings_json = settings;
    layer->effects.push_back(std::move(created));
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_remove_effect(comp_document document, comp_layer_id id, uint32_t effect) {
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

  const size_t before = layer->effects.size();
  compositor::abi::ImplicitTxn txn(doc, "Remove Layer Effect");
  /*
   * 先判定再改：effects 为空（无该效果）时不能留下任何改动。
   * 原来先 erase 再看size，失败路径虽然没 commit 历史，但文档状态已被
   * 动过——外部事务下连回滚都没有，成了「报失败但已改」的半完成操作。
   */
  const bool present = std::any_of(layer->effects.begin(), layer->effects.end(),
                                   [effect](const compositor::Effect& e) {
                                     return e.effect == effect;
                                   });
  if (!present) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "effect not found");
  }
  layer->effects.erase(
      std::remove_if(layer->effects.begin(), layer->effects.end(),
                     [effect](const compositor::Effect& e) { return e.effect == effect; }),
      layer->effects.end());
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_count(comp_document document, size_t* out_count) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }
  *out_count = doc->state().layers.size();
  return compositor::abi::ok(engine);
}

comp_result comp_layer_list(comp_document document, comp_layer_id* ids, size_t capacity,
                           size_t* out_count) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }

  const std::vector<comp_layer_id> flattened = doc->flatten_ids();
  *out_count = flattened.size();
  if (ids == nullptr) return compositor::abi::ok(engine);

  const size_t count = std::min(capacity, flattened.size());
  for (size_t i = 0; i < count; ++i) ids[i] = flattened[i];
  return compositor::abi::ok(engine);
}

comp_result comp_layer_get_opacity(comp_document document, comp_layer_id id, float* out_opacity) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_opacity == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_opacity is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_opacity = layer->opacity;
  return compositor::abi::ok(engine);
}

comp_result comp_layer_get_blend(comp_document document, comp_layer_id id,
                                comp_blend_mode* out_blend) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_blend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_blend is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_blend = layer->blend;
  return compositor::abi::ok(engine);
}

comp_result comp_layer_set_isolated(comp_document document, comp_layer_id id, int32_t isolated) {
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

  compositor::abi::ImplicitTxn txn(doc, "Set Isolation");
  layer->isolated = isolated != 0;
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_isolated(comp_document document, comp_layer_id id,
                                    int32_t* out_isolated) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_isolated == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_isolated is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_isolated = layer->isolated ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_layer_can_merge_down(comp_document document, comp_layer_id id,
                                      int32_t* out_can) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_can == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_can is null");
  }
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_can = doc->can_merge_down(id) ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_active(comp_document document, comp_layer_id id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 不入历史：选择变更是 UI 态，不是可撤销编辑。 */
  doc->state().active_layer = id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_merge_down(comp_document document, comp_layer_id id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /*
   * 不包 ImplicitTxn：域方法自行管理事务（加入调用方显式事务，或自开自提交），
   * 再包一层会双记历史。失败路径上域方法尚未改动任何状态（先算后改）。
   */
  std::string error;
  if (!doc->merge_down(id, &error)) {
    const comp_result code =
        error.find("adjustments/effects") != std::string::npos ? COMP_ERR_UNSUPPORTED
                                                               : COMP_ERR_STATE;
    return compositor::abi::fail(engine, code, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_document_flatten(comp_document document) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  std::string error;
  if (!doc->flatten_image(&error)) {
    const comp_result code =
        error.find("adjustments/effects") != std::string::npos ? COMP_ERR_UNSUPPORTED
                                                               : COMP_ERR_STATE;
    return compositor::abi::fail(engine, code, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

} /* extern "C" */
