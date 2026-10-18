// AI生成
/*
 * comp_camera_raw_abi.cpp — Camera Raw C ABI（F4）。
 *
 * 三个入口都只做「句柄校验 -> 领域调用 -> 错误码映射」：
 *   - 表加载：解码 CRT4 成功后整体替换引擎里的表；失败时旧表原样保留，
 *     所以一次坏 blob 不会让已加载的文档失去 Camera Raw。
 *   - 表查询：只读，不碰文档事务。
 *   - 应用：领域层自己开/提交事务（一步撤销），这里**不能**再包 ImplicitTxn，
 *     否则会叠出两条历史。像素失败时领域层已回滚，这里只负责上报。
 */
#include "compositor/comp_camera_raw.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "comp_abi_internal.h"
#include "domain/camera_raw.h"
#include "domain/camera_raw_tables.h"

namespace {

using compositor::CameraRawSettings;
using compositor::CameraRawTableSet;
using compositor::Document;
using compositor::Engine;

/* 两段式 note 拷贝：容量 0 时只写所需长度（含 '\0'）。 */
void copy_note(const std::string& text, char* note, size_t note_capacity, size_t* out_note_size) {
  if (out_note_size != nullptr) *out_note_size = text.size() + 1;
  if (note == nullptr || note_capacity == 0) return;
  const size_t copy = std::min(note_capacity - 1, text.size());
  if (copy > 0) std::memcpy(note, text.data(), copy);
  note[copy] = '\0';
}

}  // namespace

comp_result comp_engine_load_camera_raw_tables(comp_engine engine_handle, const void* data,
                                               size_t size) {
  Engine* engine = compositor::abi::as_engine(engine_handle);
  if (engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(engine)
  if (data == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "camera raw table blob is null");
  }
  std::string error;
  std::shared_ptr<const CameraRawTableSet> decoded = CameraRawTableSet::decode(data, size, &error);
  if (decoded == nullptr) {
    /* 没有 zlib 是构建能力问题，不是数据坏了：单独报 UNSUPPORTED。 */
    const bool no_zlib = error.find("zlib is not available") != std::string::npos;
    return compositor::abi::fail(engine, no_zlib ? COMP_ERR_UNSUPPORTED : COMP_ERR_DECODE, error);
  }
  engine->set_camera_raw_tables(std::move(decoded));
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_engine_camera_raw_tables_loaded(comp_engine engine_handle, int32_t* out_loaded) {
  Engine* engine = compositor::abi::as_engine(engine_handle);
  if (engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  COMP_ABI_TRY(engine)
  if (out_loaded == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_loaded is null");
  }
  *out_loaded = engine->camera_raw_tables_loaded() ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_camera_raw_apply(comp_document document_handle, comp_layer_id target,
                                  const char* settings_json, double scale, uint32_t seed,
                                  int32_t* out_changed, char* note, size_t note_capacity,
                                  size_t* out_note_size) {
  Document* document = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document_handle, &document, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (settings_json == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "camera raw settings json is null");
  }
  CameraRawSettings settings;
  std::string error;
  if (!compositor::camera_raw_settings_from_json(settings_json, &settings, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error);
  }
  comp_result code = COMP_ERR_INTERNAL;
  bool changed = false;
  std::string note_text;
  if (!compositor::camera_raw_apply(document, target, settings, scale, seed, &code, &changed,
                                    &note_text, &error)) {
    return compositor::abi::fail(engine, code, error);
  }
  copy_note(note_text, note, note_capacity, out_note_size);
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}
