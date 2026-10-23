// AI生成
/*
 * comp_canvas_abi.cpp —「图像」菜单操作 C ABI（S25）。
 *
 * 约定与其它 ABI 文件一致：
 *   - 句柄/输出指针先校验，再做事；失败无副作用、不 commit；
 *   - 三个写操作的唯一事务来源是「显式事务加入 / 隐式事务自成一步」，
 *     整次操作恰好一步撤销（与 comp_crop_apply 同款 ImplicitTxn）；
 *   - rotate / trim 的领域实现在 domain/canvas_ops.cpp，ABI 只做校验与
 *     事务包装；invert 自带事务（与 filter_apply 同款，因为要先取 8 位
 *     副本、再落墨、再混合选区）。
 */
#include "compositor/comp_canvas.h"

#include "comp_abi_internal.h"
#include "domain/canvas_ops.h"
#include "domain/document.h"

namespace {

using compositor::Document;
using compositor::Engine;

compositor::TrimOptions to_domain(const comp_trim_options& options) {
  compositor::TrimOptions out;
  switch (options.based_on) {
    case COMP_TRIM_TOP_LEFT_PIXEL_COLOR:
      out.based_on = compositor::TrimBasedOn::TopLeftPixelColor;
      break;
    case COMP_TRIM_BOTTOM_RIGHT_PIXEL_COLOR:
      out.based_on = compositor::TrimBasedOn::BottomRightPixelColor;
      break;
    default:
      out.based_on = compositor::TrimBasedOn::TransparentPixels;
      break;
  }
  out.top = options.top != 0;
  out.bottom = options.bottom != 0;
  out.left = options.left != 0;
  out.right = options.right != 0;
  out.tolerance = options.tolerance;
  return out;
}

}  // namespace

comp_result comp_document_rotate_canvas(comp_document document, int32_t quarter_turns,
                                        int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (quarter_turns != 1 && quarter_turns != -1 && quarter_turns != 2 && quarter_turns != -2) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "quarter_turns must be 1, -1, 2 or -2");
  }
  compositor::abi::ImplicitTxn txn(doc, "Rotate Canvas");
  bool changed = false;
  if (!compositor::rotate_canvas(&doc->state(), quarter_turns, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "canvas rotation overflowed a layer transform");
  }
  /* S18：画布级几何变换 = 整篇换号（每层 origin/rotation 都可能变了）。
   * changed = 0 时（空文档正方形 90° / 180°）什么都没动，不 commit、
   * 不换号。 */
  if (changed) doc->touch_all_layers();
  if (changed) txn.commit();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_document_trim(comp_document document, comp_trim_options options,
                               int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (options.based_on < COMP_TRIM_TRANSPARENT_PIXELS ||
      options.based_on > COMP_TRIM_BOTTOM_RIGHT_PIXEL_COLOR) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "unknown trim based_on enum value");
  }
  if (options.top == 0 && options.bottom == 0 && options.left == 0 && options.right == 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "trim needs at least one enabled edge");
  }

  compositor::abi::ImplicitTxn txn(doc, "Trim");
  int32_t changed = 0;
  std::string error;
  const comp_result code = compositor::document_trim(doc, to_domain(options), &changed, &error);
  if (code != COMP_OK) {
    return compositor::abi::fail(engine, code, error);
  }
  /* document_trim 内部已按 crop 口径换号（changed 时 touch_all_layers）。 */
  if (changed) txn.commit();
  if (out_changed != nullptr) *out_changed = changed;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_invert(comp_document document, comp_layer_id layer, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  int32_t changed = 0;
  std::string error;
  const comp_result code = compositor::invert_layer(doc, layer, &changed, &error);
  if (code != COMP_OK) {
    return compositor::abi::fail(engine, code, error);
  }
  if (out_changed != nullptr) *out_changed = changed;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_layer_apply_adjustment(comp_document document, comp_layer_id layer, uint32_t kind,
                                        const char* settings_json_utf8, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  /*
   * settings 的校验在领域层做（同一份 adjustment_table，add/update 用的也是它），
   * ABI 只负责把 NULL 视作 "{}"——与 comp_layer_add_adjustment 的入参口径一致。
   */
  const std::string settings = settings_json_utf8 != nullptr ? std::string(settings_json_utf8) : std::string();
  int32_t changed = 0;
  std::string error;
  const comp_result code = compositor::apply_layer_adjustment(doc, layer, kind, settings, &changed, &error);
  if (code != COMP_OK) {
    return compositor::abi::fail(engine, code, error);
  }
  if (out_changed != nullptr) *out_changed = changed;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}
