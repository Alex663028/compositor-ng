// AI生成
/*
 * comp_layer_abi.cpp — comp_layer.h 的 C ABI 实现。
 *
 * 约定：所有写操作都落在事务内。显式事务（comp_txn_begin）由调用方管理；
 * 否则每次调用自动构成一个隐式撤销步（见 abi::ImplicitTxn）。
 */
#include "compositor/comp_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "comp_abi_internal.h"

#include "compositing/effect_eval.h"
#include "compositing/blend.h"
#include "core/comp_geometry.h"
#include "domain/adjustment_table.h"
#include "domain/curves_settings.h"
#include "domain/effect_table.h"
#include "domain/text_layer.h"
#include "domain/tool_ops.h"
#include "domain/utf8_text.h"
#include "format/comp_json.h"

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

/* ------------------------------------------------ 图层链接的仿射联动（P1-2 ③） */

constexpr double kPi = 3.14159265358979323846;

/*
 * 把「目标层从旧矩形+旋转 -> 新矩形+旋转」这张映射，套到同组另一层上：
 *
 *   p' = C_new + R(θ_new) · S(sx, sy) · R(-θ_old) · (p - C_old)
 *
 * 尺寸按层自身坐标系缩放；旋转层与目标的相对角不是 90° 整数倍又遇上
 * 非等比缩放时，结果会带剪切——本仓的 origin/size/rotation 模型表示不了，
 * 明确拒绝（返回 false + error），不做静默近似。
 */
struct LinkAffine {
  double sx = 1.0;
  double sy = 1.0;
  double old_cx = 0.0;
  double old_cy = 0.0;
  double new_cx = 0.0;
  double new_cy = 0.0;
  double old_rotation = 0.0;
  double new_rotation = 0.0;
};

bool apply_link_affine(const LinkAffine& m, const Layer& mate, comp_rect* out_rect,
                       float* out_rotation, std::string* error) {
  const double theta_old = m.old_rotation * kPi / 180.0;
  const double theta_new = m.new_rotation * kPi / 180.0;

  /* 相对角归一化到 [0, 360)，用来判断「缩放是否能在该层的轴向上表示」。 */
  double rel = std::fmod(mate.rotation - m.old_rotation, 360.0);
  if (rel < 0.0) rel += 360.0;
  const double k = std::round(rel / 90.0);
  const double residual = std::fabs(rel - k * 90.0);
  const bool uniform = std::fabs(m.sx - m.sy) <= 1e-9;
  if (!uniform && residual > 0.01) {
    if (error != nullptr) {
      *error = "non-uniform scale across linked layers with differing rotation "
               "(shear is not representable)";
    }
    return false;
  }
  const bool swap = (static_cast<int32_t>(std::llround(k)) % 2) != 0;
  const double scale_w = swap ? m.sy : m.sx;
  const double scale_h = swap ? m.sx : m.sy;

  const comp_rect bounds = mate.effective_bounds();
  const bool degenerate = bounds.w <= 0 || bounds.h <= 0;
  const double cx = bounds.x + bounds.w / 2.0;
  const double cy = bounds.y + bounds.h / 2.0;

  /* R(-θ_old)·(p - C_old) */
  const double qx = cx - m.old_cx;
  const double qy = cy - m.old_cy;
  const double ux = qx * std::cos(theta_old) + qy * std::sin(theta_old);
  const double uy = -qx * std::sin(theta_old) + qy * std::cos(theta_old);
  /* S */
  const double vx = ux * (degenerate ? 1.0 : m.sx);
  const double vy = uy * (degenerate ? 1.0 : m.sy);
  /* R(θ_new) + C_new */
  const double nx = m.new_cx + vx * std::cos(theta_new) - vy * std::sin(theta_new);
  const double ny = m.new_cy + vx * std::sin(theta_new) + vy * std::cos(theta_new);

  /* 显式尺寸才缩放；w/h <= 0 的「按像素尺寸」分量保持未指定语义（不偷偷改成
   * 显式），撤空图层（clipped_empty，包围盒为空）也不放大。 */
  const bool has_w = mate.transform.w > 0;
  const bool has_h = mate.transform.h > 0;
  double new_w = static_cast<double>(mate.transform.w);
  double new_h = static_cast<double>(mate.transform.h);
  double actual_w = new_w;
  double actual_h = new_h;
  if (!degenerate) {
    if (has_w) {
      new_w = std::round(bounds.w * scale_w);
      actual_w = new_w;
    } else {
      actual_w = bounds.w;
    }
    if (has_h) {
      new_h = std::round(bounds.h * scale_h);
      actual_h = new_h;
    } else {
      actual_h = bounds.h;
    }
  }

  const comp_rect rect{
      static_cast<int32_t>(std::llround(nx - actual_w / 2.0)),
      static_cast<int32_t>(std::llround(ny - actual_h / 2.0)),
      static_cast<int32_t>(new_w),
      static_cast<int32_t>(new_h),
  };
  const float rotation = static_cast<float>(mate.rotation + (m.new_rotation - m.old_rotation));
  if (!compositor::is_valid_transform(rect, rotation)) {
    if (error != nullptr) *error = "linked layer geometry out of range";
    return false;
  }
  *out_rect = rect;
  *out_rotation = rotation;
  return true;
}

/* comp_text_desc -> 域层 TextObject。只做拷贝与**廉价的**字段闸；文本/样式/
 * 字体的完整校验交给 text_layer_render（它已经握着字体与排版内核）。
 * 所有检查都在开事务之前完成：失败不建层、不改旧像素、不进撤销栈。 */
bool text_desc_to_object(const comp_text_desc* desc, compositor::TextObject* out,
                         std::string* error) {
  if (desc == nullptr || out == nullptr) {
    *error = "text desc is null";
    return false;
  }
  if (desc->text == nullptr) {
    *error = "text is null";
    return false;
  }
  if (desc->text_length <= 0) {
    *error = "text_length must be positive";
    return false;
  }
  if (static_cast<size_t>(desc->text_length) > compositor::kMaxTypeCharacters) {
    *error = "text exceeds the character limit";
    return false;
  }
  if (desc->_reserved != 0) {
    *error = "_reserved must be 0";
    return false;
  }
  if (desc->_reserved2 != 0) {
    *error = "_reserved2 must be 0";
    return false;
  }
  /* 段落框的量程与 text_layer_validate 同一把尺子（这里是廉价闸，完整判定
   * 仍由渲染路径给出）——非法值一旦入库会被 .comp 持久化。 */
  if (!(desc->box_width >= 0.0) || !(desc->box_height >= 0.0) ||
      desc->box_width > compositor::kMaxTypeParagraphBox ||
      desc->box_height > compositor::kMaxTypeParagraphBox) {
    *error = "paragraph box must be finite and within 0..1e6";
    return false;
  }
  if (desc->align < compositor::kTypeAlignLeft || desc->align >= compositor::kTypeAlignCount) {
    *error = "align must be 0..2";
    return false;
  }
  if (desc->font_face_index < 0) {
    *error = "font_face_index must be >= 0";
    return false;
  }
  out->text.assign(desc->text, static_cast<size_t>(desc->text_length));
  out->origin_x = desc->origin_x;
  out->origin_y = desc->origin_y;
  out->style.font_size = desc->style.font_size;
  out->style.line_height = desc->style.line_height;
  out->style.letter_spacing = desc->style.letter_spacing;
  out->style.antialias = desc->style.antialias != 0;
  for (int i = 0; i < 4; ++i) out->color[i] = desc->color[i];
  out->opacity = desc->opacity;
  out->font_path = desc->font_path_utf8 != nullptr ? desc->font_path_utf8 : "";
  /* 内置点阵轨道只有一个面；把 face 归零可让同一个 desc（换字体轨道时）
   * 不会因残留的 face 号被误判成 kBadFaceIndex。 */
  out->font_face_index = out->font_path.empty() ? 0 : desc->font_face_index;
  out->box_width = desc->box_width;
  out->box_height = desc->box_height;
  out->align = desc->align;
  return true;
}

/* TextLayerStatus -> comp_result。参数类错误一律 INVALID_ARGUMENT（与工具算子同一口径），
 * 字体错误按 FontLoadStatus 细分，宿主据此区分「路径错」与「文件不是字体」。 */
/* ------------------------------------------------ 图层锁 / 颜色标签（P1-2 第一批） */

/* 锁位白名单：未知位一律拒绝，不静默丢弃——宿主写 0x10 若得到「成功」，
 * 会以为锁上了，而实际什么都没发生。 */
bool valid_lock_flags(uint32_t flags) { return (flags & ~COMP_LAYER_LOCK_MASK) == 0u; }

bool valid_color_label(uint32_t label) { return label < COMP_LAYER_COLOR_COUNT; }

comp_result text_layer_result(const compositor::TextLayerReport& report) {
  using compositor::TextLayerStatus;
  switch (report.status) {
    case TextLayerStatus::kOk:
      return COMP_OK;
    case TextLayerStatus::kBadArgument:
    case TextLayerStatus::kBadText:
    case TextLayerStatus::kBadStyle:
    case TextLayerStatus::kOutOfCanvas:
      return COMP_ERR_INVALID_ARGUMENT;
    case TextLayerStatus::kTooLarge:
      return COMP_ERR_UNSUPPORTED;
    case TextLayerStatus::kFontMissing:
      switch (report.font_status) {
        case compositor::FontLoadStatus::kNotFound:
          return COMP_ERR_IO;
        case compositor::FontLoadStatus::kOutOfMemory:
          return COMP_ERR_OUT_OF_MEMORY;
        case compositor::FontLoadStatus::kNotAFont:
        case compositor::FontLoadStatus::kBadFaceIndex:
          return COMP_ERR_DECODE;
        default:
          return COMP_ERR_INVALID_ARGUMENT;
      }
    case TextLayerStatus::kInternal:
      return COMP_ERR_INTERNAL;
  }
  return COMP_ERR_INTERNAL;
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
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 锁定全部连删除也拦（PS 同口径：全锁的层不能被删）。 */
  const comp_result locked =
      compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "delete");
  if (locked != COMP_OK) return locked;

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
   * 文字图层的像素是 `text` 的渲染缓存，不是独立真源：直接覆写会让 `text` 与画面
   * 分叉，下一次改字号又把它抹掉。要求宿主先 comp_layer_rasterize_text（与
   * Photoshop 的「先栅格化才能画」同一条规矩），而不是静默丢掉文字对象。
   */
  if (layer->has_text()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "layer is an editable text layer; rasterize it first "
                                 "(comp_layer_rasterize_text)");
  }
  /* 智能对象同理：pixels 是 source + 智能滤镜栈的求值缓存。要改画面就改滤镜栈
   * （comp_layer_smart_filter_*），要整份换像素就先 rasterize（丢掉可编辑性）。 */
  if (layer->has_smart_object()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "layer is a smart object; rasterize it first "
                                 "(comp_layer_smart_object_rasterize)");
  }
  /* 填充图层同理：pixels 是 kind + settings 的渲染缓存。要改画面就改参数
   * （comp_layer_set_fill），要保留当前画面再手绘就先 rasterize。 */
  if (layer->has_fill()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "layer is a fill layer; rasterize it first "
                                 "(comp_layer_rasterize_fill)");
  }
  /* 锁定图像像素：整层像素写入是 PIXELS 明令禁止的入口（含「贴一张新像素」）。
   * 锁定透明像素在这里也拒绝：整份替换无法按原 alpha 裁剪（尺寸/格式都可能变），
   * 与其静默改掉透明区，不如让宿主先解锁或改用绘画类工具。 */
  comp_result locked = compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS,
                                                  "whole-layer pixel write");
  if (locked != COMP_OK) return locked;
  locked = compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_TRANSPARENT,
                                      "whole-layer pixel write");
  if (locked != COMP_OK) return locked;
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
  /* 蒙版不是「图像像素」：锁定图像像素/透明像素都不拦蒙版编辑（PS 同口径）。
   * 锁定全部才拦（否则「全锁」形同虚设）。 */
  const comp_result locked =
      compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "edit the layer mask");
  if (locked != COMP_OK) return locked;

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
  /* 锁定位置：移动/缩放/旋转/翻转一律拒绝（堆叠次序不算位置，重排仍允许）。
   * 闸在事务之前，失败的调用不留撤销步。 */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_POSITION, "transform");
    if (locked != COMP_OK) return locked;
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

  /*
   * 图层链接：把同一链接组（link_group != 0）的其它层带上同样的仿射量。
   * 全部计算与锁闸都在事务之前完成——失败的调用零状态变动、不留撤销步；
   * 同组任一锁定位置就整次拒绝（不做「一部分动了」的半截提交）。
   */
  struct LinkMate {
    Layer* layer = nullptr;
    comp_layer_id id = 0;
    comp_rect rect{0, 0, 0, 0};
    float rotation = 0.0f;
  };
  std::vector<LinkMate> mates;
  if (layer->link_group != 0) {
    const comp_rect old_bounds = layer->effective_bounds();
    LinkAffine affine;
    affine.old_rotation = layer->rotation;
    affine.new_rotation = rotation;
    affine.old_cx = old_bounds.x + old_bounds.w / 2.0;
    affine.old_cy = old_bounds.y + old_bounds.h / 2.0;
    affine.new_cx = transform.x + transform.w / 2.0;
    affine.new_cy = transform.y + transform.h / 2.0;
    /* 旧尺寸为空（缩画布裁空）时无从谈缩放，只做平移/旋转。 */
    affine.sx = old_bounds.w > 0 ? static_cast<double>(transform.w) / old_bounds.w : 1.0;
    affine.sy = old_bounds.h > 0 ? static_cast<double>(transform.h) / old_bounds.h : 1.0;

    for (Layer& other : doc->state().layers) {
      if (other.id == id || other.link_group != layer->link_group) continue;
      const comp_result locked = compositor::abi::lock_gate(
          engine, &other, COMP_LAYER_LOCK_POSITION, "transform linked layers");
      if (locked != COMP_OK) return locked;
      LinkMate mate;
      mate.layer = &other;
      mate.id = other.id;
      std::string mate_error;
      if (!apply_link_affine(affine, other, &mate.rect, &mate.rotation, &mate_error)) {
        return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, mate_error.c_str());
      }
      mates.push_back(mate);
    }
  }

  compositor::abi::ImplicitTxn txn(doc, mates.empty() ? "Set Transform" : "Set Linked Transform");
  layer->transform = transform;
  /* 显式给回尺寸 = 用户把图层放回画布，清掉缩画布留下的「已裁空」标记，
   * 否则 effective_bounds 会一直返回空矩形。 */
  layer->clipped_empty = false;
  layer->rotation = rotation;
  doc->touch_layer(id);
  for (LinkMate& mate : mates) {
    mate.layer->transform = mate.rect;
    mate.layer->clipped_empty = false;
    mate.layer->rotation = mate.rotation;
    doc->touch_layer(mate.id);
  }
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
  /* 翻转是几何变换，归「锁定位置」管（PS 的 Free Transform 在全锁/位置锁下禁用）。 */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_POSITION, "flip");
    if (locked != COMP_OK) return locked;
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
  /* 锁定全部的层不接受调整层的增删改（PS 同口径；锁图像像素不拦调整参数）。 */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "add an adjustment");
    if (locked != COMP_OK) return locked;
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
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "update the adjustment");
    if (locked != COMP_OK) return locked;
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
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "update the adjustment");
    if (locked != COMP_OK) return locked;
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
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "remove the adjustment");
    if (locked != COMP_OK) return locked;
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
  /* 锁定全部：图层样式的增改拦掉（锁图像像素不拦样式参数）。 */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "change a layer style");
    if (locked != COMP_OK) return locked;
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

  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "remove a layer style");
    if (locked != COMP_OK) return locked;
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
  const Layer* merge_layer = doc->find(id);
  if (merge_layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 锁定全部连向下合并也拦（PS 同口径）：合并会把这一层并进下一层，属于「删改」。 */
  const comp_result merge_locked =
      compositor::abi::lock_gate(engine, merge_layer, COMP_LAYER_LOCK_ALL, "merge down");
  if (merge_locked != COMP_OK) return merge_locked;
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
  const Layer* target_layer = doc->find(target);
  if (target_layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /*
   * 工具落墨的锁闸：
   *   - 魔棒/色彩范围只写**蒙版**，归「锁定全部」管（PS 的图像锁不拦蒙版编辑）；
   *   - 其余算子都改图层像素，归「锁定图像像素」；
   *   - 锁定透明像素不在这里拒绝：绘画类算子由域层做 alpha 保真
   *     （ToolOpParams::lock_transparent），锁定本身仍允许落笔。
   */
  const bool mask_only = desc->kind == COMP_TOOL_SELECT;
  const comp_result tool_locked = compositor::abi::lock_gate(
      engine, target_layer,
      mask_only ? COMP_LAYER_LOCK_ALL : COMP_LAYER_LOCK_PIXELS,
      mask_only ? "edit the layer mask" : "paint");
  if (tool_locked != COMP_OK) return tool_locked;
  /*
   * Heal 的行数闸（`heal_coverage_rows` 尾部追加，0 = 旧宿主按图层高信任）。
   *
   * 域层的 heal 闸只校验 stride：它拿不到宿主缓冲到底有多少行，而执行时按
   * **图层像素高**逐行 memcpy。图层比画布高（导入层的像素尺寸可以是任意值）时，
   * 宿主按画布分配缓冲就会被读到尾外。行数由宿主显式报上来，这里在写入前拒绝。
   */
  if (desc->kind == COMP_TOOL_HEAL && desc->heal_coverage_rows != 0 &&
      desc->heal_coverage_rows < static_cast<size_t>(target_layer->pixels.height)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "heal coverage rows smaller than the layer height");
  }

  /*
   * ABI 结构 -> 领域参数。纯搬运，**不写任何状态**：闸与执行都在域层，
   * 与 comp_layer_merge_down 同款（域方法自行管事务，再包 ImplicitTxn 会双记历史）。
   */
  ToolOpParams params;
  params.kind = static_cast<compositor::ToolOpKind>(desc->kind);
  /*
   * 锁定透明像素只对**绘画类**算子有意义：SELECT 写蒙版、DEEP 换像素格式
   * （alpha 语义随之改变），两者都不参与「保住原 alpha」的还原。
   */
  if (!mask_only && desc->kind != COMP_TOOL_DEEP &&
      compositor::abi::lock_has(target_layer->lock_flags, COMP_LAYER_LOCK_TRANSPARENT)) {
    params.lock_transparent = 1;
  }
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
   * P1-7：多色标。这里只做**结构性守卫**（空指针 / 超上限），位置的升序、
   * midpoint 开区间、是否全透明交给域层参数闸——那里报错会回滚事务，这里
   * 提前拒绝只是避免解引用空指针与巨量分配。
   */
  if (desc->kind == COMP_TOOL_GRADIENT && desc->gradient_stop_count != 0) {
    if (desc->gradient_stops == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                   "gradient stops are null");
    }
    if (desc->gradient_stop_count > COMP_TOOL_GRADIENT_MAX_STOPS) {
      return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                   "gradient stop count exceeds 32");
    }
    params.gradient.stops.resize(desc->gradient_stop_count);
    for (size_t i = 0; i < desc->gradient_stop_count; ++i) {
      const comp_gradient_stop& source = desc->gradient_stops[i];
      compositor::GradientStop& stop = params.gradient.stops[i];
      stop.position = source.position;
      stop.midpoint = source.midpoint;
      for (int c = 0; c < 4; ++c) stop.color[c] = source.color[c];
    }
  }
  params.gradient.dither = desc->gradient_dither != 0;

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

  /*
   * P5-2 尾部追加：可选字体句柄。NULL = 旧轨道（5×7 位图，行为逐字节不变）；
   * 非 NULL 时必须是仍有效的 comp_font——失效/重复释放的句柄属于宿主态错误
   * （COMP_ERR_STATE），不能当成「没传字体」静默回落。引擎只在本次调用内读取
   * 该句柄，不接管生命周期。
   */
  params.type.font = compositor::abi::as_font(desc->type_font);
  if (desc->type_font != nullptr && params.type.font == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_STATE,
                                 "type font handle is invalid or already released");
  }

  /* P1 尾部追加：减淡 / 加深 / 海绵只多两个标量，笔迹与几何复用上面的字段。 */
  params.tone.mode = desc->tone.mode;
  params.tone.strength = desc->tone.strength;

  /* P1 尾部追加：历史记录画笔同样只多两个标量（取样下标 + 不透明度）。 */
  params.history_brush.state = desc->history.state;
  params.history_brush.opacity = desc->history.opacity;

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

comp_result comp_layer_get_pixels(comp_document document, comp_layer_id id, void* pixels,
                                 size_t stride, int32_t* out_width, int32_t* out_height,
                                 comp_pixel_format* out_format) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_width == nullptr || out_height == nullptr || out_format == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "out_width / out_height / out_format is null");
  }
  *out_width = 0;
  *out_height = 0;
  *out_format = COMP_PIXEL_RGBA8;
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 组与从未有像素的层是合法状态：如实回报 0x0，而不是伪装成错误。 */
  if (layer->is_group || layer->pixels.empty()) {
    return compositor::abi::ok(engine);
  }
  const compositor::ImageBuffer& src = layer->pixels;
  /* 先回报尺寸/原始格式：查询模式（pixels == nullptr 或 stride == 0）到此为止，
     宿主据此分配缓冲。写回格式是 8 位工作副本，但来源格式必须如实回报。 */
  *out_format = src.format;
  *out_width = src.width;
  *out_height = src.height;
  if (pixels == nullptr || stride == 0) {
    return compositor::abi::ok(engine);
  }
  const size_t row_bytes = static_cast<size_t>(src.width) * 4u;
  if (stride < row_bytes) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "stride is smaller than one row of RGBA8 pixels");
  }

  uint8_t* base = static_cast<uint8_t*>(pixels);
  if (src.format == COMP_PIXEL_RGBA8) {
    for (int32_t y = 0; y < src.height; ++y) {
      std::memcpy(base + static_cast<size_t>(y) * stride, src.row(y), row_bytes);
    }
  } else {
    /* 与合成/缩略图/工具层同一份 8 位工作副本：16 位整数与 HDR half 各有自己的
       降位口径（见 domain/layer.h），这里只做搬运，不另造一套。 */
    compositor::ImageBuffer rgba;
    const bool made = src.format == COMP_PIXEL_RGBA16F
                          ? compositor::hdr_to_rgba8(src, &rgba)
                          : compositor::deep_to_rgba8(src, &rgba);
    if (!made) {
      return compositor::abi::fail(engine, COMP_ERR_INTERNAL,
                                   "8-bit working copy conversion failed");
    }
    for (int32_t y = 0; y < rgba.height; ++y) {
      std::memcpy(base + static_cast<size_t>(y) * stride, rgba.row(y), row_bytes);
    }
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* --------------------------------------------- 可编辑文字图层（P0-6 ①） */

comp_result comp_layer_add_text(comp_document document, const comp_text_desc* desc,
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
  compositor::TextObject object;
  std::string error;
  if (!text_desc_to_object(desc, &object, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
  }
  /*
   * 渲染在**事务之前**：字体加载/排版失败时就什么都不发生（不建层、不消耗
   * 图层 id、不进撤销栈）。这是与 comp_layer_add 相同的「参数闸先于副作用」
   * 契约。
   */
  compositor::ImageBuffer buffer;
  comp_rect bounds{};
  int64_t missing_glyphs = 0;
  const compositor::TextLayerReport report = compositor::text_layer_render(
      object, doc->state().width, doc->state().height, &buffer, &bounds, &missing_glyphs);
  if (report.status != compositor::TextLayerStatus::kOk) {
    return compositor::abi::fail(engine, text_layer_result(report),
                                 report.message.empty() ? "text layer render failed"
                                                        : report.message);
  }
  /* 文本框即图层矩形，必须过 comp_layer_add 的同一把 transform 尺子；否则
   * create_layer 会拒绝它，而我们只能报一个「内部错误」掩盖真实原因（文本太长）。 */
  if (!compositor::is_valid_transform(bounds, 0.0f)) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                 "text box exceeds the maximum layer size");
  }

  const std::string name = compositor::text_layer_default_name(object.text);
  comp_layer_desc layer_desc{};
  layer_desc.name = name.c_str();
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

  compositor::abi::ImplicitTxn txn(doc, "Add Text Layer");
  comp_layer_id id = 0;
  Layer* layer = doc->create_layer(layer_desc, &id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "failed to create the text layer");
  }
  /*
   * create_layer 给普通图层铺了一张画布大小的全透明缓冲；文字图层只需要文本
   * 框那么大的缓冲（合成按 transform 取样，不要求整幅画布）。这里整体替换，
   * 顺带省掉一次画布级的分配。
   */
  layer->pixels = std::move(buffer);
  layer->text = std::move(object);
  doc->touch_layer(id);
  txn.commit();
  *out_id = id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_text(comp_document document, comp_layer_id id,
                                const comp_text_desc* desc) {
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
  if (!layer->has_text()) {
    /* 不把普通图层的像素悄悄换成文字：那会抹掉用户已有内容，而且宿主无法
     * 从返回值看出发生了什么。要新建文字层用 comp_layer_add_text。 */
    return compositor::abi::fail(
        engine, COMP_ERR_STATE,
        "layer is not a text layer (use comp_layer_add_text to create one)");
  }
  /* 改文本 = 重排版 = 重写整层像素缓存：归「锁定图像像素」管。
   * （锁定全部经 lock_has 的蕴含规则同样命中。） */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_PIXELS, "edit the text");
    if (locked != COMP_OK) return locked;
  }
  compositor::TextObject object;
  std::string error;
  if (!text_desc_to_object(desc, &object, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
  }
  compositor::ImageBuffer buffer;
  comp_rect bounds{};
  int64_t missing_glyphs = 0;
  const compositor::TextLayerReport report = compositor::text_layer_render(
      object, doc->state().width, doc->state().height, &buffer, &bounds, &missing_glyphs);
  if (report.status != compositor::TextLayerStatus::kOk) {
    return compositor::abi::fail(engine, text_layer_result(report),
                                 report.message.empty() ? "text layer render failed"
                                                        : report.message);
  }
  if (!compositor::is_valid_transform(bounds, 0.0f)) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                 "text box exceeds the maximum layer size");
  }

  compositor::abi::ImplicitTxn txn(doc, "Set Text");
  layer->text = std::move(object);
  layer->pixels = std::move(buffer);
  layer->transform = bounds;
  /* 文本框可能把图层移出「画布裁剪过的空矩形」状态：重新渲染后它确实有内容。 */
  layer->clipped_empty = false;
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_text(comp_document document, comp_layer_id id,
                                comp_text_desc* out_desc, char* text_buffer, size_t capacity,
                                size_t* out_required) {
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
  if (!layer->has_text()) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "layer is not a text layer");
  }
  const compositor::TextObject& text = *layer->text;
  const size_t required = text.text.size() + 1; /* 含结尾 NUL，与 comp_layer_get_name 同口径 */
  *out_required = required;
  if (out_desc != nullptr) {
    *out_desc = comp_text_desc{};
    out_desc->text = text_buffer;
    out_desc->text_length = static_cast<int32_t>(text.text.size());
    out_desc->origin_x = text.origin_x;
    out_desc->origin_y = text.origin_y;
    out_desc->style.font_size = text.style.font_size;
    out_desc->style.line_height = text.style.line_height;
    out_desc->style.letter_spacing = text.style.letter_spacing;
    out_desc->style.antialias = text.style.antialias ? 1 : 0;
    for (int i = 0; i < 4; ++i) out_desc->color[i] = text.color[i];
    out_desc->opacity = text.opacity;
    out_desc->font_path_utf8 = text.font_path.empty() ? nullptr : text.font_path.c_str();
    out_desc->font_face_index = text.font_face_index;
    out_desc->_reserved = 0;
    out_desc->box_width = text.box_width;
    out_desc->box_height = text.box_height;
    out_desc->align = text.align;
    out_desc->_reserved2 = 0;
  }
  if (text_buffer == nullptr || capacity == 0) return compositor::abi::ok(engine);
  if (capacity < required) {
    /* 不静默截断：截断可能切掉一个多字节字符的尾部（与 get_name 同一条理由）。 */
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "text buffer is too small (see out_required)");
  }
  std::memcpy(text_buffer, text.text.c_str(), required);
  return compositor::abi::ok(engine);
}

comp_result comp_layer_rasterize_text(comp_document document, comp_layer_id id) {
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
  /* 幂等：普通图层已经是栅格，什么也不做、不记撤销步（宿主不必先判断类型）。 */
  if (!layer->has_text()) return compositor::abi::ok(engine);
  /* 栅格化会丢掉文字对象（不可逆的可编辑性损失）：锁定全部连这一步也拦。 */
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "rasterize the text");
    if (locked != COMP_OK) return locked;
  }
  compositor::abi::ImplicitTxn txn(doc, "Rasterize Text");
  layer->text.reset();
  doc->touch_layer(id);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* --------------------------------------------- 图层锁 / 颜色标签（P1-2 第一批） */

comp_result comp_layer_set_lock(comp_document document, comp_layer_id id, uint32_t flags) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  /* 参数闸在事务之前：非法位不产生任何状态（也就不产生撤销步）。 */
  if (!valid_lock_flags(flags)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "unknown lock bits (see COMP_LAYER_LOCK_MASK)");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 幂等：同值不落撤销步（宿主可以无脑「设为当前勾选态」）。 */
  if (layer->lock_flags == flags) return compositor::abi::ok(engine);
  compositor::abi::ImplicitTxn txn(doc, "Set Layer Lock");
  layer->lock_flags = flags;
  /* 不 touch_layer：锁不参与像素求值，不该让合成/GPU 缓存失效。 */
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_lock(comp_document document, comp_layer_id id, uint32_t* out_flags) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_flags == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_flags is null");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_flags = layer->lock_flags;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_set_color_label(comp_document document, comp_layer_id id, uint32_t label) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (!valid_color_label(label)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "label must be 0..COMP_LAYER_COLOR_COUNT-1");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (layer->color_label == label) return compositor::abi::ok(engine);
  compositor::abi::ImplicitTxn txn(doc, "Set Layer Color Label");
  layer->color_label = label;
  /* 与锁同理：纯 UI 元数据，不碰 content_revision。 */
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_color_label(comp_document document, comp_layer_id id, uint32_t* out_label) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_label == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_label is null");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_label = layer->color_label;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/*
 * 合并可见（P1-2 第二批，ChangeLog：ABI 171 -> 172）。
 *
 * 锁闸：不按「选中层」判，而按**参与集合**判——合并会删除每一个参与层，
 * 所以任意一个参与层带「锁定全部」（或图像像素锁）都应拒绝。错误文案里指出
 * 具体层名，宿主才能定位。域方法自行管事务，这里不再包 ImplicitTxn。
 */
comp_result comp_layer_merge_visible(comp_document document, comp_layer_id* out_layer_id,
                                     int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  /*
   * 先做空操作短路：没有足够可见层时，锁也不该把「本来什么都不做」变成报错
   * （PS 里该命令此时是置灰的，不是弹错误）。
   *
   * 参与集合 = 自身可见、且祖先组全部可见的非组图层（与 Document::merge_visible
   * 的 layer_is_effectively_visible 同一把尺子；隐藏组里的「可见」子层不贡献像素，
   * 既不该计入数量，也不该用它的锁拦下整条命令）。
   */
  std::vector<const Layer*> participating;
  for (const Layer& layer : doc->state().layers) {
    if (layer.is_group || !layer.visible) continue;
    bool ancestors_visible = true;
    for (comp_layer_id parent = layer.parent; parent != 0;) {
      const Layer* group = doc->find(parent);
      if (group == nullptr || !group->visible) {
        ancestors_visible = false;
        break;
      }
      parent = group->parent;
    }
    if (ancestors_visible) participating.push_back(&layer);
  }
  if (participating.size() < 2) {
    if (out_layer_id != nullptr) *out_layer_id = 0;
    if (out_changed != nullptr) *out_changed = 0;
    return compositor::abi::ok(engine);
  }

  /*
   * 参与集合的锁闸：锁是「删改」的闸，合并会让每一层消失，故锁定全部 /
   * 锁定图像像素都拒绝。锁定透明/位置不拦（合并不改用户对像素/位置的手工意图，
   * 与 merge_down 只拦 LOCK_ALL 的口径保持一致；这里更严一档，因为合并可见
   * 会一次删掉很多层，误操作代价更大）。
   */
  for (const Layer* layer : participating) {
    if (!compositor::abi::lock_has(layer->lock_flags, COMP_LAYER_LOCK_ALL) &&
        !compositor::abi::lock_has(layer->lock_flags, COMP_LAYER_LOCK_PIXELS)) {
      continue;
    }
    std::string message = "layer '" + layer->name + "' is locked (merge visible)";
    return compositor::abi::fail(engine, COMP_ERR_STATE, message);
  }

  std::string error;
  if (!doc->merge_visible(&error, out_layer_id, nullptr)) {
    const comp_result code = error.find(compositor::kUnevaluatedEffectMarker) != std::string::npos
                                 ? COMP_ERR_UNSUPPORTED
                                 : COMP_ERR_STATE;
    return compositor::abi::fail(engine, code, error);
  }
  if (out_changed != nullptr) *out_changed = 1;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/*
 * 反向排列（P1-2 第二批，ABI 172 -> 173）。
 *
 * 只重排，不动内容：锁不拦（PS 里锁定位置/图像后仍可在图层面板里重排）。
 * 域方法自行管事务。
 */
comp_result comp_layer_reverse(comp_document document, comp_layer_id parent,
                               int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (parent != 0) {
    const Layer* group = doc->find(parent);
    if (group == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "parent layer not found");
    }
    if (!group->is_group) {
      return compositor::abi::fail(engine, COMP_ERR_STATE, "parent is not a layer group");
    }
  }
  std::string error;
  bool changed = false;
  if (!doc->reverse_layers(parent, &error, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, error);
  }
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* --------------------------------------------- 图层链接（P1-2 第三批） */

static comp_result link_fail(Engine* engine, const std::string& error) {
  if (error == "layer not found") {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, error);
  }
  if (error == "layer groups cannot be linked") {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, error);
  }
  return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
}

comp_result comp_layer_link(comp_document document, const comp_layer_id* ids, size_t count,
                            uint32_t* out_group, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_changed != nullptr) *out_changed = 0;
  if (ids == nullptr || count < 2) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "linking requires at least two layer ids");
  }
  /* 逐层先给准确错误码（域方法只回字符串）；组图层明确不支持。 */
  for (size_t i = 0; i < count; ++i) {
    const Layer* layer = doc->find(ids[i]);
    if (layer == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
    }
    if (layer->is_group) {
      return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                   "layer groups cannot be linked");
    }
  }
  std::vector<comp_layer_id> list(ids, ids + count);
  std::string error;
  uint32_t group = 0;
  bool changed = false;
  if (!doc->link_layers(list, &group, &error, &changed)) {
    return link_fail(engine, error);
  }
  if (out_group != nullptr) *out_group = group;
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_unlink(comp_document document, const comp_layer_id* ids, size_t count,
                              int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_changed != nullptr) *out_changed = 0;
  if (ids == nullptr || count == 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "no layer ids given");
  }
  for (size_t i = 0; i < count; ++i) {
    if (doc->find(ids[i]) == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
    }
  }
  std::vector<comp_layer_id> list(ids, ids + count);
  std::string error;
  bool changed = false;
  if (!doc->unlink_layers(list, &error, &changed)) {
    return link_fail(engine, error);
  }
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_get_link(comp_document document, comp_layer_id id, uint32_t* out_group) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_group == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_group is null");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  *out_group = layer->link_group;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* --------------------------------------------- 图层样式拷贝 / 粘贴（P1-2 ④） */

namespace {

/* 本文件的实现体整体位于 extern "C" 区段内，匿名命名空间的辅助函数默认也会拿到
 * C 链接——返回 std::string 的函数会被 -Wreturn-type-c-linkage 点名。这里显式
 * 回到 C++ 链接（与下面图层复合块同一写法）。 */
extern "C++" {

/* 样式块与 .comp 的 engineEffects 同形；settings 保持为字符串（不解释）。 */
std::string dump_layer_style(const std::vector<compositor::Effect>& effects) {
  compositor::JsonValue list = compositor::JsonValue::array({});
  for (const compositor::Effect& effect : effects) {
    compositor::JsonValue item = compositor::JsonValue::object({});
    item.set("effect", compositor::JsonValue::integer(static_cast<long long>(effect.effect)));
    item.set("settings", compositor::JsonValue::string(effect.settings_json));
    list.push(std::move(item));
  }
  return compositor::json_dump(list, 0);
}

/* 本引擎求值不了的 effect kind（更高版本 .comp 读进来的）：不导出、不导入。 */
bool style_kind_supported(uint32_t kind) { return kind < COMP_EFFECT_KIND_COUNT; }

/*
 * 解析样式块。两类错误分开报：报文形状错 -> INVALID_ARGUMENT（宿主传错了），
 * 求值不了的 kind -> UNSUPPORTED（这里做不到），宿主才能给出不同的提示。
 */
bool parse_layer_style(const std::string& text, std::vector<compositor::Effect>* out,
                       comp_result* out_code, std::string* out_error) {
  std::string parse_error;
  compositor::JsonValue root = compositor::json_parse(text, &parse_error);
  if (!parse_error.empty() || !root.is_array()) {
    *out_code = COMP_ERR_INVALID_ARGUMENT;
    *out_error = parse_error.empty() ? std::string("style JSON must be an array") : parse_error;
    return false;
  }
  std::vector<compositor::Effect> parsed;
  for (const compositor::JsonValue& item : root.as_array()) {
    if (!item.is_object()) {
      *out_code = COMP_ERR_INVALID_ARGUMENT;
      *out_error = "each style entry must be an object";
      return false;
    }
    const compositor::JsonValue* kind_value = item.find("effect");
    const compositor::JsonValue* settings_value = item.find("settings");
    if (kind_value == nullptr || !kind_value->is_number() || settings_value == nullptr ||
        !settings_value->is_string()) {
      *out_code = COMP_ERR_INVALID_ARGUMENT;
      *out_error = "style entries need an integer 'effect' and a string 'settings'";
      return false;
    }
    /* 非整数 kind 不能被静默截断成相邻的合法 kind（2.9 不等于 2）。 */
    const double kind_number = kind_value->as_number(-1.0);
    if (kind_number != std::floor(kind_number) || kind_number < 0.0) {
      *out_code = COMP_ERR_INVALID_ARGUMENT;
      *out_error = "'effect' must be a non-negative integer";
      return false;
    }
    const uint32_t kind = static_cast<uint32_t>(kind_number);
    if (!style_kind_supported(kind)) {
      *out_code = COMP_ERR_UNSUPPORTED;
      *out_error = compositor::kUnevaluatedEffectMarker;
      return false;
    }
    for (const compositor::Effect& existing : parsed) {
      if (existing.effect == kind) {
        *out_code = COMP_ERR_INVALID_ARGUMENT;
        *out_error = "duplicate effect kind in style";
        return false;
      }
    }
    compositor::Effect effect;
    effect.effect = kind;
    effect.settings_json = settings_value->as_string("{}");
    parsed.push_back(std::move(effect));
  }
  *out = std::move(parsed);
  return true;
}

bool same_style(const std::vector<compositor::Effect>& a,
                const std::vector<compositor::Effect>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].effect != b[i].effect || a[i].settings_json != b[i].settings_json) return false;
  }
  return true;
}

}  /* extern "C++" */

}  // namespace

comp_result comp_layer_export_style(comp_document document, comp_layer_id id,
                                    char* style_json_utf8, size_t capacity, size_t* out_size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_size == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_size is null");
  }
  const Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (layer->is_group) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                 "layer groups do not render layer styles");
  }
  /* 先把整层检查完再生成报文：半份可用的剪贴板比拒绝更难排查。 */
  for (const compositor::Effect& effect : layer->effects) {
    if (!style_kind_supported(effect.effect)) {
      return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                   compositor::kUnevaluatedEffectMarker);
    }
  }
  const std::string blob = dump_layer_style(layer->effects);
  *out_size = blob.size();
  /* 两段式：pixels 风格的查询调用只回报长度，不碰缓冲。 */
  if (style_json_utf8 == nullptr || capacity == 0) return compositor::abi::ok(engine);
  if (capacity < blob.size() + 1) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "style buffer is too small (see out_size)");
  }
  std::memcpy(style_json_utf8, blob.c_str(), blob.size() + 1);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_import_style(comp_document document, comp_layer_id id,
                                    const char* style_json_utf8, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_changed != nullptr) *out_changed = 0;
  if (style_json_utf8 == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "style_json_utf8 is null");
  }
  Layer* layer = doc->find(id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  if (layer->is_group) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED,
                                 "layer groups do not render layer styles");
  }
  /* 参数闸全部先于状态改动：报文坏掉时连撤销步都不该出现。 */
  std::vector<compositor::Effect> parsed;
  comp_result parse_code = COMP_ERR_INVALID_ARGUMENT;
  std::string parse_error;
  if (!parse_layer_style(style_json_utf8, &parsed, &parse_code, &parse_error)) {
    return compositor::abi::fail(engine, parse_code, parse_error);
  }
  /* 幂等：同一样式不落撤销步（宿主可以无脑「粘贴到所有选中层」）。 */
  if (same_style(parsed, layer->effects)) return compositor::abi::ok(engine);
  {
    const comp_result locked =
        compositor::abi::lock_gate(engine, layer, COMP_LAYER_LOCK_ALL, "change a layer style");
    if (locked != COMP_OK) return locked;
  }
  compositor::abi::ImplicitTxn txn(doc, "Paste Layer Style");
  layer->effects = std::move(parsed);
  /* 样式参与像素求值：修订号必须换，否则合成/GPU 缓存会复用旧样式的结果。 */
  doc->touch_layer(id);
  txn.commit();
  if (out_changed != nullptr) *out_changed = 1;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* ------------------------------------------- 图层复合（P1-2 收尾 ⑤） */

namespace {

/* 本文件的实现体整体位于 extern "C" 区段内，匿名命名空间的辅助函数默认也会
 * 拿到 C 链接——返回 std::string / vector 的辅助函数会被 -Wreturn-type-c-linkage
 * 点名。这里显式回到 C++ 链接（与上面样式块同一写法）。 */
extern "C++" {

compositor::LayerComp* find_layer_comp(compositor::DocumentState& state, uint32_t id) {
  for (compositor::LayerComp& comp : state.layer_comps) {
    if (comp.id == id) return &comp;
  }
  return nullptr;
}

/*
 * 从 C 字符串取复合名。与 comp_layer_set_name 的读法一致：不信「内存里一定有
 * NUL」，先按上界扫，再校验编码与单行。长度**含 NUL 之外**的字节数判定。
 */
bool read_layer_comp_name(const char* utf8, std::string* out) {
  if (utf8 == nullptr || out == nullptr) return false;
  size_t length = 0;
  while (length <= compositor::kMaxLayerCompNameBytes && utf8[length] != '\0') ++length;
  if (length == 0 || length > compositor::kMaxLayerCompNameBytes) return false;
  std::string name(utf8, length);
  if (!compositor::utf8_is_valid(name) || !compositor::utf8_is_single_line(name)) return false;
  *out = std::move(name);
  return true;
}

/* 当前状态 -> 快照条目（顺序 = 文档堆叠序，与面板自上而下的读法相反，但与
 * state.layers 一致；.comp 落盘同样按这个序，往返不需要重排）。 */
std::vector<compositor::LayerCompEntry> snapshot_layer_comp(
    const compositor::DocumentState& state) {
  std::vector<compositor::LayerCompEntry> entries;
  entries.reserve(state.layers.size());
  for (const compositor::Layer& layer : state.layers) {
    compositor::LayerCompEntry entry;
    entry.layer = layer.id;
    entry.visible = layer.visible ? 1u : 0u;
    entry.opacity = layer.opacity;
    entries.push_back(entry);
  }
  return entries;
}

bool same_layer_comp_entries(const std::vector<compositor::LayerCompEntry>& a,
                             const std::vector<compositor::LayerCompEntry>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].layer != b[i].layer || a[i].visible != b[i].visible ||
        a[i].opacity != b[i].opacity) {
      return false;
    }
  }
  return true;
}

}  /* extern "C++" */

}  // namespace

comp_result comp_layer_comp_count(comp_document document, size_t* out_count) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_count == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_count is null");
  }
  *out_count = doc->state().layer_comps.size();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_id_at(comp_document document, size_t index,
                                  comp_layer_comp_id* out_id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_id == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_id is null");
  }
  const compositor::DocumentState& state = doc->state();
  if (index >= state.layer_comps.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "layer comp index out of range");
  }
  *out_id = state.layer_comps[index].id;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_name(comp_document document, comp_layer_comp_id comp, char* buffer,
                                 size_t capacity, size_t* out_required) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_required == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_required is null");
  }
  const compositor::LayerComp* found = find_layer_comp(doc->state(), static_cast<uint32_t>(comp));
  if (found == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  const size_t required = found->name.size() + 1;
  *out_required = required;
  if (buffer == nullptr || capacity == 0) return compositor::abi::ok(engine);
  if (capacity < required) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "name buffer is too small (see out_required)");
  }
  std::memcpy(buffer, found->name.c_str(), required);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_capture(comp_document document, const char* name_utf8,
                                    comp_layer_comp_id* out_id) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_id != nullptr) *out_id = 0;
  std::string name;
  if (!read_layer_comp_name(name_utf8, &name)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "layer comp name must be non-empty single-line UTF-8 within "
                                 "COMP_LAYER_COMP_NAME_MAX_BYTES bytes");
  }
  compositor::DocumentState& state = doc->state();
  if (state.layer_comps.size() >= compositor::kMaxLayerComps) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "too many layer comps");
  }
  compositor::abi::ImplicitTxn txn(doc, "Capture Layer Comp");
  compositor::LayerComp comp;
  comp.id = state.next_layer_comp_id++;
  comp.name = std::move(name);
  comp.entries = snapshot_layer_comp(state);
  const uint32_t captured_id = comp.id;
  state.layer_comps.push_back(std::move(comp));
  txn.commit();
  if (out_id != nullptr) *out_id = static_cast<comp_layer_comp_id>(captured_id);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_apply(comp_document document, comp_layer_comp_id comp,
                                  int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_changed != nullptr) *out_changed = 0;
  const compositor::LayerComp* found =
      find_layer_comp(doc->state(), static_cast<uint32_t>(comp));
  if (found == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  /* 先判幂等再开事务：全都一样时不该落撤销步，也不该推进修订号。 */
  bool changed = false;
  for (const compositor::LayerCompEntry& entry : found->entries) {
    const Layer* layer = doc->find(entry.layer);
    if (layer == nullptr) continue;
    if ((layer->visible ? 1u : 0u) != entry.visible || layer->opacity != entry.opacity) {
      changed = true;
      break;
    }
  }
  if (!changed) return compositor::abi::ok(engine);

  /* 事务期间不跨结构变更（不改 state.layers，但仍按条重新 find 一次：
   * 保持「不缓存图层指针」这条规矩）。 */
  compositor::abi::ImplicitTxn txn(doc, "Apply Layer Comp");
  for (const compositor::LayerCompEntry& entry : found->entries) {
    Layer* layer = doc->find(entry.layer);
    if (layer == nullptr) continue;
    const bool visible = entry.visible != 0;
    if (layer->visible == visible && layer->opacity == entry.opacity) continue;
    layer->visible = visible;
    layer->opacity = entry.opacity;
    /* 可见性与不透明度都参与合成求值：这两层缓存必须一起失效。 */
    doc->touch_layer(entry.layer);
  }
  txn.commit();
  if (out_changed != nullptr) *out_changed = 1;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_update(comp_document document, comp_layer_comp_id comp) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  compositor::DocumentState& state = doc->state();
  compositor::LayerComp* found = find_layer_comp(state, static_cast<uint32_t>(comp));
  if (found == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  std::vector<compositor::LayerCompEntry> entries = snapshot_layer_comp(state);
  if (same_layer_comp_entries(found->entries, entries)) return compositor::abi::ok(engine);
  compositor::abi::ImplicitTxn txn(doc, "Update Layer Comp");
  found->entries = std::move(entries);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_rename(comp_document document, comp_layer_comp_id comp,
                                   const char* name_utf8) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  std::string name;
  if (!read_layer_comp_name(name_utf8, &name)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "layer comp name must be non-empty single-line UTF-8 within "
                                 "COMP_LAYER_COMP_NAME_MAX_BYTES bytes");
  }
  compositor::DocumentState& state = doc->state();
  compositor::LayerComp* found = find_layer_comp(state, static_cast<uint32_t>(comp));
  if (found == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  if (found->name == name) return compositor::abi::ok(engine);
  compositor::abi::ImplicitTxn txn(doc, "Rename Layer Comp");
  found->name = std::move(name);
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_delete(comp_document document, comp_layer_comp_id comp) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  compositor::DocumentState& state = doc->state();
  const uint32_t id = static_cast<uint32_t>(comp);
  size_t index = state.layer_comps.size();
  for (size_t i = 0; i < state.layer_comps.size(); ++i) {
    if (state.layer_comps[i].id == id) {
      index = i;
      break;
    }
  }
  if (index >= state.layer_comps.size()) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  compositor::abi::ImplicitTxn txn(doc, "Delete Layer Comp");
  state.layer_comps.erase(state.layer_comps.begin() + static_cast<std::ptrdiff_t>(index));
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_entry_count(comp_document document, comp_layer_comp_id comp,
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
  const compositor::LayerComp* found =
      find_layer_comp(doc->state(), static_cast<uint32_t>(comp));
  if (found == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  *out_count = found->entries.size();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_comp_entry(comp_document document, comp_layer_comp_id comp, size_t index,
                                  comp_layer_id* out_layer, uint8_t* out_visible,
                                  float* out_opacity) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  const compositor::LayerComp* found =
      find_layer_comp(doc->state(), static_cast<uint32_t>(comp));
  if (found == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer comp not found");
  }
  if (index >= found->entries.size()) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "layer comp entry index out of range");
  }
  const compositor::LayerCompEntry& entry = found->entries[index];
  if (out_layer != nullptr) *out_layer = entry.layer;
  if (out_visible != nullptr) *out_visible = entry.visible;
  if (out_opacity != nullptr) *out_opacity = entry.opacity;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

} /* extern "C" */
