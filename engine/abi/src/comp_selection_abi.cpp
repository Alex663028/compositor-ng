// AI生成
/*
 * comp_selection_abi.cpp — 选区几何 C ABI（F1）。
 *
 * 约定（与其它 ABI 文件一致）：
 *   - 轻量参数闸（句柄 / 枚举 / 空输入 / 输出指针）在 ImplicitTxn 之前；点数、
 *     段数与坐标有限性交由域函数统一校验。任何失败路径都没有副作用、也不 commit，
 *     所以既不会在显式事务里留下半成品，也不会产生撤销步；
 *   - 域函数返回 changed = false 时不 commit：上游 setSelection 对相同值不
 *     产生撤销步，宿主也不该看到一条「点了但没动」的历史；
 *   - 域函数自己管事务的（merge/flatten/resize）不在这里包 ImplicitTxn；
 *     本文件的域函数都是纯状态改动，由 ABI 统一补隐式事务。
 */
#include "compositor/comp_selection.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "comp_abi_internal.h"
#include "domain/layer.h"
#include "domain/selection.h"
#include "domain/selection_pixels.h"

namespace {

using compositor::Document;
using compositor::DocumentState;
using compositor::Engine;
using compositor::SelectionCubic;

/* 写操作共用的收尾：changed 才落历史，然后回报给宿主。 */
comp_result finish(Engine* engine, compositor::abi::ImplicitTxn* txn, bool changed,
                   int32_t* out_changed) {
  if (changed) txn->commit();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
}

}  // namespace

comp_result comp_selection_apply_shape(comp_document document, comp_selection_shape shape,
                                       const float* points, int32_t point_count,
                                       comp_selection_combine combine, int32_t antialias,
                                       int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (shape < COMP_SELECTION_RECT || shape > COMP_SELECTION_POLYGON) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection shape");
  }
  if (combine < COMP_SELECTION_REPLACE || combine > COMP_SELECTION_SUBTRACT) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection combine");
  }
  if (points == nullptr || point_count <= 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "empty selection points");
  }

  const DocumentState& state = doc->state();
  compositor::abi::ImplicitTxn txn(doc, "Selection");
  bool changed = false;
  if (!compositor::selection_apply_shape(&doc->selection(), state.width, state.height, shape,
                                         points, point_count, combine, antialias != 0, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "invalid selection shape (point count or non-finite coordinate)");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_apply_path(comp_document document, const comp_selection_cubic* segments,
                                      int32_t segment_count, comp_selection_combine combine,
                                      int32_t antialias, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (combine < COMP_SELECTION_REPLACE || combine > COMP_SELECTION_SUBTRACT) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection combine");
  }
  if (segments == nullptr || segment_count <= 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "empty selection path");
  }

  /* ABI 结构是 POD（new_subpath 用 int32），这里按域类型重排一遍；
   * 坐标的有限性由域函数统一校验。 */
  std::vector<SelectionCubic> converted(static_cast<size_t>(segment_count));
  for (int32_t i = 0; i < segment_count; ++i) {
    const comp_selection_cubic& source = segments[i];
    SelectionCubic& target = converted[static_cast<size_t>(i)];
    target.start[0] = source.start[0];
    target.start[1] = source.start[1];
    target.control1[0] = source.control1[0];
    target.control1[1] = source.control1[1];
    target.control2[0] = source.control2[0];
    target.control2[1] = source.control2[1];
    target.end[0] = source.end[0];
    target.end[1] = source.end[1];
    target.new_subpath = source.new_subpath != 0;
  }

  const DocumentState& state = doc->state();
  compositor::abi::ImplicitTxn txn(doc, "Selection");
  bool changed = false;
  if (!compositor::selection_apply_path(&doc->selection(), state.width, state.height,
                                        converted.data(), segment_count, combine, antialias != 0,
                                        &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "invalid selection path (segment count or non-finite coordinate)");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_select_all(comp_document document, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  const DocumentState& state = doc->state();
  compositor::abi::ImplicitTxn txn(doc, "Select All");
  bool changed = false;
  if (!compositor::selection_select_all(&doc->selection(), state.width, state.height, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid canvas size");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_deselect(comp_document document, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  compositor::abi::ImplicitTxn txn(doc, "Deselect");
  bool changed = false;
  if (!compositor::selection_deselect(&doc->selection(), &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "deselect failed");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_invert(comp_document document, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  compositor::abi::ImplicitTxn txn(doc, "Invert Selection");
  bool changed = false;
  if (!compositor::selection_invert(&doc->selection(), &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "invert selection failed");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_modify(comp_document document, comp_selection_modify_op op,
                                  int32_t amount, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (op < COMP_SELECTION_FEATHER || op > COMP_SELECTION_CONTRACT) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection modify op");
  }
  const int32_t limit =
      op == COMP_SELECTION_FEATHER ? compositor::kMaxSelectionFeather : compositor::kMaxSelectionGrow;
  if (amount < 1 || amount > limit) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "selection amount out of range");
  }

  const char* label = op == COMP_SELECTION_FEATHER  ? "Feather Selection"
                      : op == COMP_SELECTION_EXPAND ? "Expand Selection"
                                                    : "Contract Selection";
  compositor::abi::ImplicitTxn txn(doc, label);
  bool changed = false;
  if (!compositor::selection_modify(&doc->selection(), op, amount, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "selection modify failed");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_get_active(comp_document document, int32_t* out_active) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_active == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_active is null");
  }
  *out_active = doc->selection().active ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_get_feather(comp_document document, float* out_feather) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_feather == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_feather is null");
  }
  *out_feather = doc->selection().feather;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_get_bounds(comp_document document, comp_rect* out_bounds) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_bounds == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_bounds is null");
  }
  *out_bounds = compositor::selection_bounds(doc->selection());
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_get_coverage(comp_document document, comp_mask* out_mask) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_mask == nullptr || out_mask->pixels == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "mask buffer is required");
  }

  const DocumentState& state = doc->state();
  if (out_mask->width != state.width || out_mask->height != state.height) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "mask size must equal document size");
  }
  if (out_mask->stride < static_cast<size_t>(out_mask->width)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "mask stride is too small");
  }
  if (!state.selection.active) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, "no selection");
  }

  std::vector<uint8_t> coverage;
  compositor::selection_effective_coverage(state.selection, &coverage);
  const size_t expected =
      static_cast<size_t>(state.width) * static_cast<size_t>(state.height);
  if (coverage.size() != expected) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "selection mask is corrupt");
  }
  for (int32_t y = 0; y < state.height; ++y) {
    std::memcpy(out_mask->pixels + static_cast<size_t>(y) * out_mask->stride,
                coverage.data() + static_cast<size_t>(y) * static_cast<size_t>(state.width),
                static_cast<size_t>(state.width));
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

/* ------------------------------------------- 载入 / 平移 / 像素搬移（S5） */

comp_result comp_selection_load_layer(comp_document document, comp_layer_id layer_id,
                                      comp_selection_load_source source,
                                      comp_selection_combine combine, int32_t threshold,
                                      int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (source != COMP_SELECTION_LOAD_PIXELS && source != COMP_SELECTION_LOAD_MASK) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection load source");
  }
  if (combine < COMP_SELECTION_REPLACE || combine > COMP_SELECTION_SUBTRACT) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection combine");
  }
  if (threshold < 0 || threshold > 255) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "threshold out of 0..255");
  }
  compositor::Layer* layer = doc->find(layer_id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "selection source layer was not found");
  }

  const DocumentState& state = doc->state();
  compositor::abi::ImplicitTxn txn(doc, "Load Selection");
  bool changed = false;
  if (!compositor::selection_load_layer(&doc->selection(), state.width, state.height, *layer,
                                        source, combine, threshold, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid selection load request");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_translate(comp_document document, int32_t dx, int32_t dy,
                                     int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (std::abs(static_cast<int64_t>(dx)) > 1000000 ||
      std::abs(static_cast<int64_t>(dy)) > 1000000) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "selection offset out of range");
  }
  compositor::abi::ImplicitTxn txn(doc, "Move Selection");
  bool changed = false;
  if (!compositor::selection_translate(&doc->selection(), dx, dy, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "selection translate failed");
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

comp_result comp_selection_move_pixels(comp_document document, comp_layer_id layer_id, int32_t dx,
                                       int32_t dy, int32_t duplicate, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  compositor::Layer* layer = doc->find(layer_id);
  if (layer == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "move pixels target layer was not found");
  }

  const DocumentState& state = doc->state();
  compositor::abi::ImplicitTxn txn(doc, duplicate != 0 ? "Duplicate Selection Pixels"
                                                       : "Move Selection Pixels");
  bool changed = false;
  std::string note;
  std::string error;
  if (!compositor::selection_move_pixels(layer, state.width, state.height, state.selection, dx, dy,
                                          duplicate != 0, &changed, &note, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_STATE, error);
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}

/* --------------------------------------------------------------- 魔棒（S7） */

comp_result comp_selection_apply_wand(comp_document document, comp_layer_id layer_id,
                                      comp_selection_wand_source source, double seed_x,
                                      double seed_y, int32_t tolerance, int32_t contiguous,
                                      int32_t radius, comp_selection_combine combine,
                                      int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  /* 轻量参数闸在 ImplicitTxn 之前：非法参数不留半成品、不进撤销栈。 */
  if (source != COMP_SELECTION_WAND_LAYER && source != COMP_SELECTION_WAND_MERGED) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown wand source");
  }
  if (combine < COMP_SELECTION_REPLACE || combine > COMP_SELECTION_SUBTRACT) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown selection combine");
  }
  if (tolerance < 0 || tolerance > 255) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "wand tolerance out of 0..255");
  }
  if (contiguous != 0 && contiguous != 1) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "wand contiguous must be 0 or 1");
  }
  if (radius < 0 || radius > compositor::kMaxSelectionWandRadius) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "wand radius out of range");
  }

  const DocumentState& state = doc->state();
  compositor::SelectionWandParams params;
  params.source = source == COMP_SELECTION_WAND_MERGED ? compositor::kSelectionWandMerged
                                                       : compositor::kSelectionWandLayer;
  params.seed_x = seed_x;
  params.seed_y = seed_y;
  params.tolerance = tolerance;
  params.contiguous = contiguous;
  params.radius = radius;

  compositor::abi::ImplicitTxn txn(doc, "Magic Wand Selection");
  bool changed = false;
  std::string error;
  const comp_result code = compositor::selection_apply_wand(
      &doc->selection(), state.width, state.height, *doc, layer_id, params, combine, &changed,
      &error);
  if (code != COMP_OK) {
    return compositor::abi::fail(
        engine, code, error.empty() ? "magic wand selection failed" : error);
  }
  return finish(engine, &txn, changed, out_changed);
  COMP_ABI_CATCH(engine)
}
