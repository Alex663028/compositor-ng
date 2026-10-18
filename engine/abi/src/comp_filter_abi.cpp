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
