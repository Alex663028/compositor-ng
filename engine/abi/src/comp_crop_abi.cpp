// AI生成
/*
 * comp_crop_abi.cpp — Crop 工具 C ABI（F2）。
 *
 * 约定与其它 ABI 文件一致：
 *   - 句柄/输出指针先校验，再做几何；失败无副作用、不 commit；
 *   - 纯几何函数（snap / create / change_ratio / is_valid）不碰文档，只需要
 *     engine 句柄来上报 last_error；
 *   - comp_crop_apply 是唯一写操作：未处于显式事务时补一个隐式事务，
 *     整次裁剪（含选区丢弃）恰好一步撤销。
 */
#include "compositor/comp_crop.h"

#include "comp_abi_internal.h"
#include "domain/crop.h"

namespace {

using compositor::CropFrame;
using compositor::Document;
using compositor::DocumentState;
using compositor::Engine;

CropFrame to_frame(const comp_crop_rect& rect) {
  return CropFrame{rect.x, rect.y, rect.width, rect.height};
}

comp_crop_rect to_abi(const comp_rect& rect) {
  return comp_crop_rect{static_cast<double>(rect.x), static_cast<double>(rect.y),
                        static_cast<double>(rect.w), static_cast<double>(rect.h)};
}

}  // namespace

comp_result comp_crop_snap_rect(comp_engine engine_handle, comp_crop_rect frame,
                                comp_crop_rect* out_rect) {
  Engine* engine = compositor::abi::as_engine(engine_handle);
  if (engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(engine)
  if (out_rect == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_rect is null");
  }
  comp_rect snapped{};
  if (!compositor::crop_snap_frame(to_frame(frame), &snapped)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid crop frame");
  }
  *out_rect = to_abi(snapped);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_crop_create_frame(comp_engine engine_handle, double start_x, double start_y,
                                   double end_x, double end_y, double ratio, int32_t symmetric,
                                   comp_crop_rect* out_rect) {
  Engine* engine = compositor::abi::as_engine(engine_handle);
  if (engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(engine)
  if (out_rect == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_rect is null");
  }
  comp_rect created{};
  if (!compositor::crop_create_frame(start_x, start_y, end_x, end_y, ratio, symmetric != 0,
                                     &created)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "invalid crop drag (non-finite coordinate or ratio)");
  }
  *out_rect = to_abi(created);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_crop_change_ratio(comp_engine engine_handle, comp_crop_rect rect, double ratio,
                                   comp_crop_rect* out_rect, int32_t* out_changed) {
  Engine* engine = compositor::abi::as_engine(engine_handle);
  if (engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(engine)
  if (out_rect == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_rect is null");
  }
  /* 输入框来自上一轮 create/snap，正常已是整像素；这里仍按「取整后的框」处理，
   * 免得宿主传入未取整的数值时算出半像素边界。 */
  comp_rect current{};
  if (!compositor::crop_snap_frame(to_frame(rect), &current)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid crop frame");
  }
  comp_rect next{};
  bool changed = false;
  if (!compositor::crop_change_ratio(current, ratio, &next, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "crop ratio must be finite and positive");
  }
  *out_rect = to_abi(next);
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_crop_is_valid(comp_engine engine_handle, comp_crop_rect rect,
                               int32_t* out_valid) {
  Engine* engine = compositor::abi::as_engine(engine_handle);
  if (engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(engine)
  if (out_valid == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_valid is null");
  }
  /* 与 comp_crop_apply 的判定完全一致：先取整再校验。非有限输入不算错误，
   * 只是「不能提交」（out_valid = 0）——这是查询，不是写操作。 */
  comp_rect snapped{};
  const bool valid =
      compositor::crop_snap_frame(to_frame(rect), &snapped) && compositor::crop_is_valid(snapped);
  *out_valid = valid ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_crop_apply(comp_document document, comp_crop_rect rect, int32_t* out_changed) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  comp_rect snapped{};
  if (!compositor::crop_snap_frame(to_frame(rect), &snapped)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid crop frame");
  }

  compositor::abi::ImplicitTxn txn(doc, "Crop");
  DocumentState& state = doc->state();
  bool changed = false;
  if (!compositor::crop_apply(&state, snapped, &changed)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "invalid crop rect or layer transform overflow");
  }
  /* S18：换画布 = 换坐标系（每层 transform 都平移了），整篇换号。changed=0 时
   * 上面什么都没动、也不会 commit，此时不换号。 */
  if (changed) doc->touch_all_layers();
  if (changed) txn.commit();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}
