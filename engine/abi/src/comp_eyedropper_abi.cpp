// AI生成
/*
 * comp_eyedropper_abi.cpp — 吸管取色 C ABI（S5）。
 *
 * 只做句柄校验与枚举映射；坐标/尺寸/图层状态由领域层统一校验，
 * 失败原因写进 comp_engine_last_error（错误码再映射一次，见头文件）。
 */
#include "compositor/comp_eyedropper.h"

#include <string>

#include "comp_abi_internal.h"
#include "domain/eyedropper.h"
#include "domain/layer.h"

comp_result comp_eyedropper_sample(comp_document document, comp_layer_id layer, double x, double y,
                                   int32_t sample_size, comp_eyedropper_source source,
                                   float out_srgb[4]) {
  compositor::Document* doc = nullptr;
  compositor::Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (out_srgb == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_srgb is null");
  }
  if (source != COMP_EYEDROPPER_LAYER && source != COMP_EYEDROPPER_MERGED) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "unknown eyedropper source");
  }

  /* 领域层用同一个错误字符串区分「参数/状态」；这里按前置条件再判一次，
   * 让宿主拿到准确的错误码（而不是一律 INTERNAL）。 */
  if (source == COMP_EYEDROPPER_LAYER) {
    const compositor::Layer* target = doc->find(layer);
    if (target == nullptr) {
      return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "eyedropper layer was not found");
    }
    if (target->is_group || target->pixels.empty()) {
      return compositor::abi::fail(engine, COMP_ERR_STATE, "eyedropper needs a pixel layer");
    }
  }
  if (sample_size < 1 || sample_size > compositor::kMaxEyeDropperSample ||
      (sample_size & 1) == 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "sample size must be odd in 1..101");
  }

  std::string error;
  float srgb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!compositor::eyedropper_sample(*doc, layer, x, y, sample_size, source, srgb, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
  }
  for (int i = 0; i < 4; ++i) out_srgb[i] = srgb[i];
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}
