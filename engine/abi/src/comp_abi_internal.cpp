// AI生成
#include "comp_abi_internal.h"

#include <cstring>

#include "core/comp_geometry.h"
#include "core/comp_live_handles.h"

namespace compositor {
namespace abi {

/*
 * 句柄校验（双闸门）：
 *   1. 先查「存活对象登记表」（is_live_handle）：表里只有构造成功过、且尚未
 *      析构的对象指针，**并带类型标签**。查询只哈希指针值、不解引用，因此
 *      完全野的句柄（宿主传了个未映射地址）在这里就会被挡下来。
 *      类型标签是 F2 的修复关键：magic 是各类对象的尾部成员，偏移随类型变化
 *      （Document 的 handle_magic_ 在约 +472，而 Engine 对象总共才 160 字节）。
 *      若只查「活着」再按 Document 布局读 magic，就是把 comp_engine 当一个
 *      472+4 字节的对象读——Windows 上读进相邻堆块（侥幸返回错误码），
 *      鸿蒙上可能读进未映射页（SIGSEGV / ASan heap-buffer-overflow）。
 *      类型不对在这一步就返回，一次解引用都不会发生。
 *   2. 再读对象内置 magic：挡住销毁后使用（dtor 把 magic 改成 kHandlePoisoned），
 *      以及与登记表标签不一致的极端情形（JNI/宿主内存被踩坏等）。
 * 失败一律返回 nullptr，由调用方转成 COMP_ERR_INVALID_ARGUMENT。
 */
Engine* as_engine(comp_engine handle) {
  Engine* engine = reinterpret_cast<Engine*>(handle);
  if (engine == nullptr || !is_live_handle(engine, LiveHandleKind::kEngine) ||
      !engine->is_alive()) {
    return nullptr;
  }
  return engine;
}

Document* as_document(comp_document handle) {
  Document* document = reinterpret_cast<Document*>(handle);
  if (document == nullptr || !is_live_handle(document, LiveHandleKind::kDocument) ||
      !document->is_alive()) {
    return nullptr;
  }
  return document;
}

comp_txn_handle* as_txn(comp_txn handle) {
  comp_txn_handle* txn = reinterpret_cast<comp_txn_handle*>(handle);
  if (txn == nullptr || !is_live_handle(txn, LiveHandleKind::kTxn) ||
      txn->handle_magic_ != kTxnMagic) {
    return nullptr;
  }
  return txn;
}

comp_result guard_document(comp_document handle, Document** out_document, Engine** out_engine) {
  Document* document = as_document(handle);
  if (document == nullptr) {
    if (out_engine != nullptr) *out_engine = nullptr;
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_document != nullptr) *out_document = document;
  if (out_engine != nullptr) *out_engine = document->engine();
  return COMP_OK;
}

comp_result fail(Engine* engine, comp_result code, const std::string& message) {
  if (engine != nullptr) engine->set_last_error(message);
  return code;
}

comp_result ok(Engine* engine) {
  if (engine != nullptr) engine->clear_error();
  return COMP_OK;
}

bool copy_image_into(const comp_image& source, ImageBuffer* destination, std::string* error) {
  if (destination == nullptr) {
    if (error != nullptr) *error = "destination buffer is null";
    return false;
  }
  if (source.width <= 0 || source.height <= 0 || source.pixels == nullptr) {
    if (error != nullptr) *error = "source image is empty";
    return false;
  }
  const int32_t bpp = bytes_per_pixel(source.format);
  if (bpp <= 0) {
    if (error != nullptr) *error = "unsupported pixel format";
    return false;
  }
  const comp_size size{source.width, source.height};
  if (!is_valid_canvas(size)) {
    if (error != nullptr) *error = "image dimensions out of range";
    return false;
  }

  destination->resize(source.width, source.height, source.format);
  const size_t row_bytes = static_cast<size_t>(source.width) * static_cast<size_t>(bpp);
  const size_t src_stride = source.stride >= row_bytes ? source.stride : row_bytes;
  const uint8_t* base = static_cast<const uint8_t*>(source.pixels);
  for (int32_t y = 0; y < source.height; ++y) {
    std::memcpy(destination->writable_row(y), base + static_cast<size_t>(y) * src_stride, row_bytes);
  }
  return true;
}

bool copy_mask_into(const comp_mask& source, MaskBuffer* destination, std::string* error) {
  if (destination == nullptr) {
    if (error != nullptr) *error = "destination mask is null";
    return false;
  }
  if (source.width <= 0 || source.height <= 0 || source.pixels == nullptr) {
    if (error != nullptr) *error = "source mask is empty";
    return false;
  }
  const comp_size size{source.width, source.height};
  if (!is_valid_canvas(size)) {
    if (error != nullptr) *error = "mask dimensions out of range";
    return false;
  }

  destination->resize(source.width, source.height);
  const size_t row_bytes = static_cast<size_t>(source.width);
  const size_t src_stride = source.stride >= row_bytes ? source.stride : row_bytes;
  for (int32_t y = 0; y < source.height; ++y) {
    std::memcpy(destination->writable_pixels().data() + static_cast<size_t>(y) * row_bytes,
                source.pixels + static_cast<size_t>(y) * src_stride, row_bytes);
  }
  return true;
}

}  // namespace abi
}  // namespace compositor
