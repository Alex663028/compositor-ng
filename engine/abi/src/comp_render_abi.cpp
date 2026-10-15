// AI生成
/*
 * comp_render_abi.cpp — comp_render.h 的 C ABI 实现。
 *
 * M1：全部走 CPU 后端（离屏合成）。窗口类表面返回 COMP_ERR_UNSUPPORTED，
 * 由 M2 的后端实现接管；接口签名不再变动。
 */
#include "compositor/comp_render.h"

#include <cstring>
#include <memory>

#include "comp_abi_internal.h"
#include "compositing/compositor.h"
#include "core/comp_geometry.h"
#include "render/rhi.h"

using compositor::Document;
using compositor::Engine;
using compositor::ImageBuffer;
using compositor::RenderBackend;

namespace {

RenderBackend* make_backend(Engine* engine, std::string* error) {
  return compositor::create_backend(engine, engine->backend(), error).release();
}

}  // namespace

extern "C" {

comp_result comp_render_document(comp_document document, const comp_render_target* target) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (target == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "render target is null");
  }

  std::string error;
  std::unique_ptr<RenderBackend> backend(make_backend(engine, &error));
  if (backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, "cannot create render backend");
  }
  if (!backend->render_document(*doc, *target, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_render_present(comp_document document) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  std::string error;
  std::unique_ptr<RenderBackend> backend(make_backend(engine, &error));
  if (backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, "cannot create render backend");
  }
  if (!backend->present(*doc, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_render_wait(comp_document document) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  std::string error;
  std::unique_ptr<RenderBackend> backend(make_backend(engine, &error));
  if (backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, "cannot create render backend");
  }
  if (!backend->wait(*doc, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, error);
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_render_flatten(comp_document document, void* pixels, comp_pixel_format format,
                               size_t stride) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (pixels == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "pixels is null");
  }
  /*
   * stride 必须由调用方按实际行宽给出。后端虽会把过小的 stride 兜底成
   * row_bytes，但那会写到调用方分配范围之外——宁可在此明确拒绝。
   */
  const int32_t bpp = compositor::bytes_per_pixel(format);
  if (bpp <= 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid pixel format");
  }
  const size_t row_bytes = static_cast<size_t>(doc->state().width) * static_cast<size_t>(bpp);
  if (stride < row_bytes) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "stride is smaller than one row of pixels");
  }

  comp_render_target target{};
  target.size = comp_size{doc->state().width, doc->state().height};
  target.format = format;
  target.kind = COMP_SURFACE_CPU_BUFFER;
  target.native_handle = nullptr;
  target.stride = stride;
  target.pixels = pixels;
  return comp_render_document(document, &target);
  COMP_ABI_CATCH(engine)
}

comp_result comp_render_backend(comp_document document, comp_backend* out_backend) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_backend is null");
  }
  *out_backend = engine->backend();
  return compositor::abi::ok(engine);
}

comp_result comp_render_layer_thumbnail(comp_document document, comp_layer_id id, int32_t max_size,
                                       void* pixels, size_t stride, comp_size* out_size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (pixels == nullptr || out_size == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "pixels or out_size is null");
  }
  if (max_size <= 0) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "max_size must be positive");
  }
  if (doc->find(id) == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_NOT_FOUND, "layer not found");
  }
  /* 缩略图行宽随内容尺寸而定，只能在算出尺寸后校验；此时先把调用方给的
     stride 视为下界，downsample 之后再复核（见下方）。 */
  if (stride != 0 && stride < 4) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "stride is smaller than one RGBA8 pixel");
  }

  ImageBuffer composite;
  std::string error;
  if (!compositor::composite_layer(*doc, id, &composite, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, error);
  }
  ImageBuffer thumbnail;
  if (!compositor::downsample_rgba8(composite, max_size, &thumbnail)) {
    return compositor::abi::fail(engine, COMP_ERR_INTERNAL, "thumbnail downsample failed");
  }

  const size_t row_bytes = static_cast<size_t>(thumbnail.width) * 4;
  /* 与 flatten 同一把尺子：引擎不替调用方"猜"行宽，过小即拒绝。 */
  if (stride < row_bytes) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT,
                                 "stride is smaller than one row of the thumbnail");
  }
  const size_t target_stride = stride;
  uint8_t* base = static_cast<uint8_t*>(pixels);
  for (int32_t y = 0; y < thumbnail.height; ++y) {
    std::memcpy(base + static_cast<size_t>(y) * target_stride, thumbnail.row(y), row_bytes);
  }
  *out_size = comp_size{thumbnail.width, thumbnail.height};
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

} /* extern "C" */
