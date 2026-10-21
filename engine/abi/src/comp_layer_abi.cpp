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

#include "compositing/effect_eval.h"
#include "compositing/blend.h"
#include "core/comp_geometry.h"
#include "domain/adjustment_table.h"
#include "domain/curves_settings.h"
#include "domain/effect_table.h"
#include "domain/tool_ops.h"

using compositor::Document;
using compositor::DocumentState;
using compositor::Engine;
using compositor::Layer;
using compositor::ToolOpParams;

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
  /* transform 写入侧把关（与 comp_layer_set_transform 同一把尺子）：
   * 越界矩形一旦入库，就能存盘、能经 .comp 往返、到合成时才在
   * build_layer_unit 炸掉——宿主既拿不到达因，也无从恢复（整篇恒不可渲染）。
   * 上游同款做法是写入前 isValid 校验 + 不合法则回退原值
   * （LayerTransform.swift:213 / EditorSession.swift:506），此处按 ABI 惯例返回
   * COMP_ERR_INVALID_ARGUMENT（幂等写属性失败即报错，不静默改写调用方的值）。 */
  if (!compositor::is_valid_transform(desc->transform, desc->rotation)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "transform out of range (size <= 300000, "
                                 "|origin| <= 1000000, finite rotation)");
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
  /*
   * RGBA8 / RGBA16 / RGBA16F 均接受：RGBA8 走正常合成；RGBA16（预乘、native-endian）
   * 保留 16 位资产，合成侧按它的 8 位副本光栅化（domain/layer.h 的 deep_to_rgba8），
   * 并用于 deep 工具链（dither_quantize16 / deep_split / deep_merge）。
   * RGBA16F（C 方案 C1）是 HDR 资产：同样 8 字节/像素，但每个通道是 half float
   * （extended sRGB、预乘、native-endian）。它的 8 位工作副本走 hdr_to_rgba8
   * ——先 deep_half_to_u16（上游 `DeepLayer.image(_:)`，white 以上的光在此截断）
   * 再 dither_quantize16；直接按 16 位整数读会把 half 的位模式当整数，画面全错。
   * 未知格式仍然明确拒绝。
   */
  if (image != nullptr && image->format != COMP_PIXEL_RGBA8 &&
      image->format != COMP_PIXEL_RGBA16 && image->format != COMP_PIXEL_RGBA16F) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                 "comp_image format must be RGBA8, RGBA16 or RGBA16F; unsupported "
                                 "pixel format");
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
  /*
   * S18：内容变了 → 换修订号。**在 txn.commit() 之前**——历史 after 快照要带着
   * 新号一起入栈，否则重做恢复的是新像素 + 旧号，GPU 会静默读到旧纹理。
   * 以下所有写入口同此顺序，不再逐处重复说明。
   */
  doc->touch_layer(id);
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
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_mask(comp_document document, comp_layer_id id,
                                int32_t* out_has_mask, int32_t* out_width,
                                int32_t* out_height) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  /* 三个出参都必填：只要一半会让调用方把「未知」当成「没有蒙版」。 */
  if (out_has_mask == nullptr || out_width == nullptr || out_height == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "out_has_mask / out_width / out_height is null");
  }
  *out_has_mask = 0;
  *out_width = 0;
  *out_height = 0;
  COMP_ABI_TRY(engine)
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /*
   * 只读：不开事务、不进撤销栈。判据与域层空 MaskBuffer 同口径——空蒙版等于
   * 没有蒙版（comp_layer_set_mask(nullptr) 正是把 mask 置空），因此不区分
   * 「从未设置」与「已移除」，面板只关心「当前这一层受不受蒙版约束」。
   */
  if (layer->mask.empty()) {
    return compositor::abi::ok(engine);
  }
  *out_has_mask = 1;
  *out_width = layer->mask.width;
  *out_height = layer->mask.height;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_group_info(comp_document document, comp_layer_id id,
                                      int32_t* out_is_group, comp_layer_id* out_parent,
                                      comp_layer_id* out_clipping_source) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  /* 三个出参都必填：图层面板要么拿全「是不是组 + 挂在哪 + 剪贴谁」，要么不画。 */
  if (out_is_group == nullptr || out_parent == nullptr || out_clipping_source == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "out_is_group / out_parent / out_clipping_source is null");
  }
  *out_is_group = 0;
  *out_parent = 0;
  *out_clipping_source = 0;
  COMP_ABI_TRY(engine)
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /*
   * 只读：不开事务、不进撤销栈。这三个值都只可能是结构性的（.comp 读回或创建时
   * 给定），没有任何「壳侧可写」的入口会读到一半状态；回读即当前真相。
   */
  *out_is_group = layer->is_group ? 1 : 0;
  *out_parent = layer->parent;
  *out_clipping_source = layer->clipping_source;
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
  doc->touch_layer(id);
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
  doc->touch_layer(id);
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
  /* 与 comp_layer_add 同一把尺子：非法 transform 在入口就拒，不让文档进入
   * 「存得进、读得出、渲不了」的死状态。口径见 compositor::is_valid_transform
   * （对齐上游 LayerTransform.isValid）。
   * 注意必须放在 ImplicitTxn 之前：闸在事务内返回会留下一个「未 commit 的事务」，
   * 而 guard_document 随后会因 transaction already active 报 COMP_ERR_STATE，
   * 把达因掩盖掉。上游 EditorSession.swift:516 同样是先 guard 再 beginEdit。 */
  if (!compositor::is_valid_transform(transform, rotation)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "transform out of range (size <= 300000, "
                                 "|origin| <= 1000000, finite rotation)");
  }

  compositor::abi::ImplicitTxn txn(doc, "Set Transform");
  layer->transform = transform;
  /* 显式给回尺寸 = 用户把图层放回画布，清掉缩画布留下的「已裁空」标记，
   * 否则 effective_bounds 会一直返回空矩形。 */
  layer->clipped_empty = false;
  layer->rotation = rotation;
  doc->touch_layer(id);
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
  doc->touch_layer(id);
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

comp_result comp_layer_get_name(comp_document document, comp_layer_id id, char* buffer,
                                size_t capacity, size_t* out_required) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_required == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_required is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  const size_t required = layer->name.size() + 1; /* 含结尾 NUL */
  *out_required = required;
  /* 查询模式：只问长度（capacity 为 0 也算），不碰 buffer。 */
  if (buffer == nullptr || capacity == 0) return compositor::abi::ok(engine);
  if (capacity < required) {
    /* 不静默截断：截断可能切掉一个多字节字符的尾部，宿主拿到的是坏 UTF-8。 */
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "name buffer is too small (see out_required)");
  }
  std::memcpy(buffer, layer->name.c_str(), required);
  return compositor::abi::ok(engine);
}

comp_result comp_layer_set_name(comp_document document, comp_layer_id id,
                                const char* name_utf8) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (name_utf8 == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "name is null");
  }
  /* 有界扫描：调用方可能传未以 NUL 结尾的缓冲，不能直接交给 std::string。 */
  size_t length = 0;
  while (length <= Document::kMaxLayerNameBytes && name_utf8[length] != '\0') ++length;
  if (length > Document::kMaxLayerNameBytes) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "layer name exceeds 1024 bytes");
  }

  compositor::abi::ImplicitTxn txn(doc, "Rename Layer");
  if (!doc->rename_layer(id, std::string(name_utf8, length))) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "invalid layer name (empty, contains NUL, or malformed UTF-8)");
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_move(comp_document document, comp_layer_id id, comp_layer_id parent,
                            comp_layer_id before) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (parent != 0) {
    const Layer* target = doc->find(parent);
    if (target == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "parent layer not found");
    }
    if (!target->is_group) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "parent is not a group");
    }
  }
  if (before != 0 && doc->find(before) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "anchor layer not found");
  }

  compositor::abi::ImplicitTxn txn(doc, parent != 0 ? "Move Layer Into Group" : "Move Layer Out");
  if (!doc->move_layer(id, parent, before)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "illegal move (cycle, cross-parent anchor, or empty target)");
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_duplicate(comp_document document, comp_layer_id id,
                                 comp_layer_id* out_id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_id == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_id is null");
  }
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  compositor::abi::ImplicitTxn txn(doc, "Duplicate Layer");
  comp_layer_id copy_id = 0;
  if (!doc->duplicate_layer(id, &copy_id)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "duplicate failed");
  }
  txn.commit();
  *out_id = copy_id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_transform(comp_document document, comp_layer_id id,
                                     comp_rect* out_transform, float* out_rotation) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  /* 两个出参都必填：只回一半会让调用方拿着半份变换去做相对步进，必然漂移。 */
  if (out_transform == nullptr || out_rotation == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "out_transform / out_rotation is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_transform = layer->transform;
  *out_rotation = layer->rotation;
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
  doc->touch_layer(id);
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

comp_result comp_layer_set_flip(comp_document document, comp_layer_id id, int32_t flip_x,
                                int32_t flip_y) {
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

  /*
   * 规范化为 0/1：`Layer::flip_x` 是 bool，调用方传 -1 / 2 时若直接隐式转换，
   * 回读得到 1 而它自己记的是 2，往返就不保真了。这里一次夹死。
   */
  const bool new_flip_x = flip_x != 0;
  const bool new_flip_y = flip_y != 0;
  if (layer->flip_x == new_flip_x && layer->flip_y == new_flip_y) {
    /*
     * 无变化不 commit：空事务会在撤销栈里留下一条「点了没反应」的记录，
     * S23 修的就是这类空条目（撤销一次画面不动，用户以为撤销失效）。
     */
    return compositor::abi::ok(engine);
  }

  compositor::abi::ImplicitTxn txn(doc, "Flip Layer");
  layer->flip_x = new_flip_x;
  layer->flip_y = new_flip_y;
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_flip(comp_document document, comp_layer_id id, int32_t* out_flip_x,
                                int32_t* out_flip_y) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  /* 两个出参都必填：只回一半会让调用方拿旧值补另一半，翻转态直接错。 */
  if (out_flip_x == nullptr || out_flip_y == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "out_flip_x / out_flip_y is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_flip_x = layer->flip_x ? 1 : 0;
  *out_flip_y = layer->flip_y ? 1 : 0;
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
  doc->touch_layer(target);
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
  doc->touch_layer(id);
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
  doc->touch_layer(id);
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
  doc->touch_layer(id);
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
  doc->touch_layer(id);
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
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_effect_count(comp_document document, comp_layer_id id,
                                    size_t* out_count) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_count = layer->effects.size();
  return compositor::abi::ok(engine);
}

comp_result comp_layer_effect_at(comp_document document, comp_layer_id id, size_t index,
                                 uint32_t* out_effect, char* settings_json_utf8,
                                 size_t capacity, size_t* out_size) {
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
  if (index >= layer->effects.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "effect index out of range");
  }
  const compositor::Effect& effect = layer->effects[index];
  if (out_effect != nullptr) {
    *out_effect = effect.effect;
  }
  const std::string& settings = effect.settings_json;
  if (out_size != nullptr) {
    *out_size = settings.size();
  }
  /* 两段式：settings_json_utf8 为 nullptr / capacity 为 0 时只回报长度。 */
  if (settings_json_utf8 != nullptr && capacity > 0) {
    const size_t copy = settings.size() < capacity - 1 ? settings.size() : capacity - 1;
    std::memcpy(settings_json_utf8, settings.data(), copy);
    settings_json_utf8[copy] = '\0';
  }
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
  doc->touch_layer(id);
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
    /*
     * 错误码从文案反推：只有「引擎不认识这个 effect」属于能力边界
     * （UNSUPPORTED，宿主不应重试）；其余（祖先穿透组不透明度不为 1、
     * 图层不存在等）都是可修正的状态错误（STATE）。
     *
     * 匹配的是 kUnevaluatedEffectMarker 而不是早期那个 "effects" 子串——
     * 后者会让任何提到 effects 的 STATE 错误也被归成 UNSUPPORTED。
     * 标记定义在 effect_eval.h，与生成文案的一侧共用同一常量。
     */
    const comp_result code = error.find(compositor::kUnevaluatedEffectMarker) != std::string::npos
                                 ? COMP_ERR_UNSUPPORTED
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
    /* 与 comp_layer_merge_down 同一条映射规则（标记为共用常量）。 */
    const comp_result code = error.find(compositor::kUnevaluatedEffectMarker) != std::string::npos
                                 ? COMP_ERR_UNSUPPORTED
                                 : COMP_ERR_STATE;
    return compositor::abi::fail(engine, code, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_tool_apply(comp_document document, comp_layer_id target,
                            const comp_tool_op_desc* desc, char* note, size_t note_capacity,
                            size_t* out_note_size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (desc == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "tool op desc is null");
  }
  if (doc->find(target) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }

  /*
   * ABI 结构 -> 领域参数。纯搬运，**不写任何状态**：闸与执行都在域层，
   * 与 comp_layer_merge_down 同款（域方法自行管事务，再包 ImplicitTxn 会双记历史）。
   */
  ToolOpParams params;
  params.kind = static_cast<compositor::ToolOpKind>(desc->kind);
  params.use_color_range = desc->select.use_color_range;
  params.seed_x = desc->select.seed_x;
  params.seed_y = desc->select.seed_y;
  params.select.tolerance = desc->select.tolerance;
  params.select.radius = desc->select.radius;
  params.select.contiguous = desc->select.contiguous;
  params.select.invert = desc->select.invert;
  params.color_range.fuzziness = desc->select.fuzziness;
  params.color_range.invert = desc->select.invert;
  params.include = desc->select.include;
  params.include_count = desc->select.include_count;
  params.exclude = desc->select.exclude;
  params.exclude_count = desc->select.exclude_count;
  for (int i = 0; i < 3; ++i) {
    params.brush.color[i] = desc->brush.color[i];
    params.dither.dark[i] = desc->dither.dark[i];
    params.dither.light[i] = desc->dither.light[i];
  }
  params.brush.diameter = desc->brush.diameter;
  params.brush.pressure = desc->brush.pressure;
  params.brush.hardness = desc->brush.hardness;
  params.brush.flow = desc->brush.flow;
  params.brush.seed = desc->brush.seed;
  params.brush.grain_amount = desc->brush.grain_amount;
  params.brush.grain_gaussian = desc->brush.grain_gaussian;
  params.brush.grain_monochromatic = desc->brush.grain_monochromatic;
  params.brush.erase = desc->erase;
  params.points = desc->stroke_points;
  params.point_count = desc->stroke_point_count;
  params.heal.opacity = desc->heal.opacity;
  params.heal.mode = desc->heal.mode;
  params.heal.seed = desc->heal.seed;
  params.coverage = desc->heal_coverage;
  params.coverage_stride = desc->heal_coverage_stride;
  params.dither.style = desc->dither.style;
  params.dither.levels = desc->dither.levels;
  params.dither.diffusion = desc->dither.diffusion;
  params.dither.density = desc->dither.density;
  params.dither.contrast = desc->dither.contrast;
  params.dither.cell = desc->dither.cell;
  params.dither.angle = desc->dither.angle;
  params.dither.light_on_dark = desc->dither.light_on_dark;
  params.dither.original_colors = desc->dither.original_colors;
  params.lens.k = desc->lens.k;
  params.clone.source_x = desc->clone.source_x;
  params.clone.source_y = desc->clone.source_y;
  params.clone.has_source = desc->clone.has_source;
  params.clone.offset_x = desc->clone.offset_x;
  params.clone.offset_y = desc->clone.offset_y;
  params.clone.has_offset = desc->clone.has_offset;
  params.clone.aligned = desc->clone.aligned;
  params.clone.sample_all_layers = desc->clone.sample_all_layers;
  params.clone.opacity = desc->clone.opacity;
  params.warp.strength = desc->warp.strength;
  params.blur.radius = desc->blur.radius;
  params.gradient.type = desc->gradient.type;
  params.gradient.start_x = desc->gradient.start_x;
  params.gradient.start_y = desc->gradient.start_y;
  params.gradient.end_x = desc->gradient.end_x;
  params.gradient.end_y = desc->gradient.end_y;
  for (int i = 0; i < 4; ++i) {
    params.gradient.color0[i] = desc->gradient.color0[i];
    params.gradient.color1[i] = desc->gradient.color1[i];
  }
  params.gradient.opacity = desc->gradient.opacity;

  /*
   * S10：形状的路径段要从 ABI 的 float 布局转成域层/kernel 的 double 布局。
   * 这里只做**廉价的结构性守卫**（指针与段数范围，避免解引用空指针或巨量分配）；
   * 坐标的合法性（NaN/Inf/量级）交给域层参数闸，它同时保证「被拒绝时不留半成品」。
   */
  std::vector<compositor::ShapeCubic> shape_segments;
  if (desc->kind == COMP_TOOL_SHAPE && desc->shape.kind == COMP_SHAPE_PATH) {
    if (desc->shape.segments == nullptr || desc->shape.segment_count <= 0 ||
        desc->shape.segment_count > 65536) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                   "shape path needs 1..65536 segments");
    }
    shape_segments.resize(static_cast<size_t>(desc->shape.segment_count));
    for (int32_t i = 0; i < desc->shape.segment_count; ++i) {
      const comp_shape_cubic& source = desc->shape.segments[i];
      compositor::ShapeCubic& segment = shape_segments[static_cast<size_t>(i)];
      for (int c = 0; c < 2; ++c) {
        segment.start[c] = source.start[c];
        segment.control1[c] = source.control1[c];
        segment.control2[c] = source.control2[c];
        segment.end[c] = source.end[c];
      }
      segment.new_subpath = source.new_subpath != 0;
    }
  }
  params.shape.kind = desc->shape.kind;
  params.shape.x0 = desc->shape.x0;
  params.shape.y0 = desc->shape.y0;
  params.shape.x1 = desc->shape.x1;
  params.shape.y1 = desc->shape.y1;
  params.shape.corner_radius = desc->shape.corner_radius;
  params.shape.segments = shape_segments.empty() ? nullptr : shape_segments.data();
  params.shape.segment_count = static_cast<int32_t>(shape_segments.size());
  params.shape.closed = desc->shape.closed;
  params.shape.style.fill_enabled = desc->shape.style.fill_enabled;
  params.shape.style.stroke_enabled = desc->shape.style.stroke_enabled;
  for (int i = 0; i < 4; ++i) {
    params.shape.style.fill_color[i] = desc->shape.style.fill_color[i];
    params.shape.style.stroke_color[i] = desc->shape.style.stroke_color[i];
  }
  params.shape.style.stroke_width = desc->shape.style.stroke_width;
  params.shape.style.opacity = desc->shape.style.opacity;
  params.shape.style.antialias = desc->shape.style.antialias;

  /*
   * S11：文字的文本指针按原样搬运（零拷贝）。**不在这里读文本**：ASCII 白名单、
   * 字符数上限与排版量程都由域层/内核的参数闸统一校验（与形状路径段的分工
   * 一致），ABI 只做结构搬运，不制造第二份阈值。
   */
  params.type.origin_x = desc->type.origin_x;
  params.type.origin_y = desc->type.origin_y;
  params.type.text = desc->type.text;
  params.type.text_length = desc->type.text_length;
  params.type.style.font_size = desc->type.style.font_size;
  params.type.style.line_height = desc->type.style.line_height;
  params.type.style.letter_spacing = desc->type.style.letter_spacing;
  params.type.style.antialias = desc->type.style.antialias;
  for (int i = 0; i < 4; ++i) {
    params.type.color[i] = desc->type.color[i];
  }
  params.type.opacity = desc->type.opacity;

  /* P1 尾部追加：减淡 / 加深 / 海绵只多两个标量，笔迹与几何复用上面的字段。 */
  params.tone.mode = desc->tone.mode;
  params.tone.strength = desc->tone.strength;

  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  std::string note_text;
  if (!compositor::run_tool_op(doc, target, params, &code, &error, &note_text)) {
    /* 错误码由域层直接给出（不从文案反推，见 tool_ops.h 末尾）。 */
    return compositor::abi::fail(engine, code, error);
  }

  /* note 两段式：capacity 为 0（或 note 为 NULL）时只回报所需长度（含 '\0'）。 */
  if (out_note_size != nullptr) *out_note_size = note_text.size() + 1;
  if (note != nullptr && note_capacity > 0) {
    /*
     * 截断时给 '\0' 预留一个字节：copy 取 min(capacity-1, size)，
     * 写入 note[copy] 落在缓冲**内**。早期写成 min(capacity, size) 时，
     * 当 note 恰好装得下（size < capacity 但 size+1 == capacity）就会把终止符
     * 写到 note[capacity]，即缓冲区之后一字节——越界写。
     */
    const size_t copy = std::min(note_capacity - 1, note_text.size());
    if (copy > 0) std::memcpy(note, note_text.data(), copy);
    note[copy] = '\0';
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_histogram(comp_document document, comp_layer_id target,
                                 const uint8_t* coverage, size_t coverage_stride,
                                 double* bins) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (bins == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "histogram bins are null");
  }
  Layer* layer = doc->find(target);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /*
   * coverage 显式传入时尺寸必须容得下图层像素。宁可拒绝也不静默截断：宿主传
   * 一个按自己那层尺寸做的 coverage（层被换过），静默按较小值读会读出半张图的
   * 直方图而不报错。
   */
  if (coverage != nullptr) {
    /* 先挡非正尺寸再比较：把 int32_t 无符号转换放在后面，负宽度不会被折成巨值
     * 而让检查失效（-Wsign-compare 也在这里显式对齐）。 */
    if (layer->pixels.width <= 0 || layer->pixels.height == 0 ||
        coverage_stride < static_cast<size_t>(layer->pixels.width)) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                   "coverage stride is smaller than the layer width");
    }
  }
  comp_result code = COMP_ERR_INTERNAL;
  std::string error;
  if (!compositor::layer_histogram(*layer, coverage, coverage_stride, bins, &code, &error)) {
    return compositor::abi::fail(engine, code, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_tool_coverage_bounds(const uint8_t* coverage, size_t coverage_stride,
                                      int32_t width, int32_t height, int32_t bounds[4]) {
  /*
   * 纯计算：只读调用方给的 coverage，不碰任何引擎状态（无文档、无引擎槽）。
   * 因此不取引擎句柄、不写 last_error——这不是一个“引擎操作”，只是一个
   * 对像素内核算法的转发；错误码直接由参数校验给出。
   */
  if (bounds == nullptr || coverage == nullptr || width <= 0 || height <= 0) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  compositor::heal_coverage_bounds_of(coverage, coverage_stride, width, height, bounds,
                                       nullptr, nullptr);
  return COMP_OK;
}

/* ------------------------------------------- 效果参数元数据（P0-2） */

/*
 * 与调整面板的 param_count/param_at 同构：纯只读元数据，不开事务。
 * 参数表在 domain/effect_table.cpp，本层只做边界检查与结构体搬运；
 * 这样「面板有哪些行」不会在 ABI 与外壳里各写一份。
 */
comp_result comp_layer_effect_param_count(uint32_t effect, size_t* out_count) {
  COMP_ABI_TRY(nullptr)
  const compositor::EffectKindInfo* info = compositor::effect_kind_info(effect);
  if (info == nullptr || out_count == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_count = info->param_count;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_effect_param_at(uint32_t effect, size_t index,
                                       comp_effect_param* out_param) {
  COMP_ABI_TRY(nullptr)
  const compositor::EffectKindInfo* info = compositor::effect_kind_info(effect);
  if (info == nullptr || out_param == nullptr || index >= info->param_count) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  const compositor::EffectParam& param = info->params[index];
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

comp_result comp_layer_effect_read_value(const char* settings_json_utf8, const char* json_path,
                                         double* out_number, int32_t* out_boolean) {
  if (json_path == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  /* 无引擎上下文的纯函数：std::string 构造 + JSON 解析会抛，护栏传 nullptr。 */
  COMP_ABI_TRY(nullptr)
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  if (!compositor::effect_read_value(settings, json_path, out_number, out_boolean)) {
    return COMP_ERR_NOT_FOUND;
  }
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_effect_build_settings(uint32_t effect, const char* const* values,
                                              size_t value_count, const char* base_json_utf8,
                                              char* settings_json_utf8, size_t capacity,
                                              size_t* out_size) {
  COMP_ABI_TRY(nullptr)
  const compositor::EffectKindInfo* info = compositor::effect_kind_info(effect);
  if (info == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  std::vector<std::string> flat;
  flat.reserve(info->param_count);
  for (size_t i = 0; i < info->param_count; ++i) {
    const char* text = (values != nullptr && i < value_count) ? values[i] : nullptr;
    flat.emplace_back(text != nullptr ? text : "");
  }

  std::string json;
  std::string error;
  const std::string base = base_json_utf8 != nullptr ? base_json_utf8 : "";
  if (!compositor::effect_build_settings(effect, flat, base, &json, &error)) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_size != nullptr) *out_size = json.size();
  /* 与 adjustment_build_settings 同口径：容量按「含 NUL」计，不够则截断并补 NUL。 */
  if (settings_json_utf8 != nullptr && capacity > 0) {
    const size_t copy = json.size() < capacity - 1 ? json.size() : capacity - 1;
    std::memcpy(settings_json_utf8, json.data(), copy);
    settings_json_utf8[copy] = '\0';
  }
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

/* ------------------------------------------- 曲线控制点（P0-3 尾部追加） */

comp_result comp_layer_curves_channel_count(size_t* out_count) {
  COMP_ABI_TRY(nullptr)
  if (out_count == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_count = compositor::kCurveChannelCount;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_curves_point_count(const char* settings_json_utf8, uint32_t channel,
                                          size_t* out_count) {
  COMP_ABI_TRY(nullptr)
  if (channel >= compositor::kCurveChannelCount || out_count == nullptr) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  *out_count = compositor::curves_read_points(settings, channel).size();
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_curves_point_at(const char* settings_json_utf8, uint32_t channel,
                                       size_t index, double* out_x, double* out_y) {
  COMP_ABI_TRY(nullptr)
  if (channel >= compositor::kCurveChannelCount) return COMP_ERR_INVALID_ARGUMENT;
  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  const std::vector<std::pair<double, double>> points =
      compositor::curves_read_points(settings, channel);
  if (index >= points.size()) return COMP_ERR_NOT_FOUND;
  if (out_x != nullptr) *out_x = points[index].first;
  if (out_y != nullptr) *out_y = points[index].second;
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

comp_result comp_layer_curves_set_points(const char* settings_json_utf8, uint32_t channel,
                                         const double* points_xy, size_t point_count,
                                         char* out_json_utf8, size_t capacity, size_t* out_size) {
  COMP_ABI_TRY(nullptr)
  if (channel >= compositor::kCurveChannelCount) return COMP_ERR_INVALID_ARGUMENT;
  if (points_xy == nullptr && point_count != 0) return COMP_ERR_INVALID_ARGUMENT;

  std::vector<std::pair<double, double>> points;
  points.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    points.emplace_back(points_xy[2 * i], points_xy[2 * i + 1]);
  }

  const std::string settings = settings_json_utf8 != nullptr ? settings_json_utf8 : "{}";
  std::string json;
  std::string error;
  if (!compositor::curves_set_points(settings, channel, points, &json, &error)) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_size != nullptr) *out_size = json.size();
  /* 与 adjustment_build_settings 同口径：容量按「含 NUL」计，不够则截断并补 NUL。 */
  if (out_json_utf8 != nullptr && capacity > 0) {
    const size_t copy = json.size() < capacity - 1 ? json.size() : capacity - 1;
    std::memcpy(out_json_utf8, json.data(), copy);
    out_json_utf8[copy] = '\0';
  }
  return COMP_OK;
  COMP_ABI_CATCH(nullptr)
}

} /* extern "C" */
