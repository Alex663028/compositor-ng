// AI生成
#include "comp_abi_internal.h"

#include <cstring>

#include "core/comp_geometry.h"

namespace compositor {
namespace abi {

Engine* as_engine(comp_engine handle) { return reinterpret_cast<Engine*>(handle); }

Document* as_document(comp_document handle) { return reinterpret_cast<Document*>(handle); }

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
