// AI生成
/*
 * comp_render_abi.cpp — comp_render.h 的 C ABI 实现。
 *
 * M1 曾全部走 CPU 后端；S13 起引擎持有一个真实后端实例（AUTO 在 Windows 上
 * 解析为 D3D12），渲染入口借用该实例。GPU 后端只覆盖「简单堆叠」子集，文档
 * 越界时按**本次调用**回落 CPU——引擎报告的后端不变；回落成功时按 ABI 契约
 * 清空 last_error，GPU 是否真实参与由后端 last_render_offloaded() 证明。
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

/*
 * 选本次渲染用的后端。
 *
 *   - 用引擎持有的实例（设备/管线常驻，避免每次渲染重建 D3D12 设备）；
 *   - 文档不在 GPU 子集内（组/蒙版/剪贴/调整/效果/变换/非全画布 RGBA8…）时，
 *     本次调用改用临时 CPU 后端，结果与 CPU 基准逐字节一致。
 */
struct BackendPick {
  RenderBackend* backend = nullptr;
  std::unique_ptr<RenderBackend> cpu_fallback{};
};

BackendPick pick_backend(Engine* engine, const Document* doc) {
  BackendPick pick;
  pick.backend = engine->render_backend();
  if (pick.backend == nullptr) {
    pick.cpu_fallback = compositor::create_cpu_backend(engine);
    pick.backend = pick.cpu_fallback.get();
    return pick;
  }
  if (doc != nullptr) {
    std::string why;
    if (!pick.backend->can_render_document(*doc, &why)) {
      pick.cpu_fallback = compositor::create_cpu_backend(engine);
      pick.backend = pick.cpu_fallback.get();
      if (!why.empty()) engine->set_last_error(why + "; rendered with cpu fallback");
    }
  }
  return pick;
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
  BackendPick pick = pick_backend(engine, doc);
  if (pick.backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, "cannot create render backend");
  }
  if (!pick.backend->render_document(*doc, *target, &error)) {
    /*
     * 运行期才暴露的能力缺口（例如目标表面格式/类型不在 GPU 路径内）：
     * 本次调用用 CPU 重试一次。像素结果正确优先于「必须用 GPU」。
     */
    const std::string gpu_error = error;
    if (pick.cpu_fallback == nullptr && pick.backend->kind() != COMP_BACKEND_CPU) {
      std::unique_ptr<RenderBackend> cpu = compositor::create_cpu_backend(engine);
      std::string cpu_error;
      if (cpu != nullptr && cpu->render_document(*doc, *target, &cpu_error)) {
        /*
         * 回落成功：调用成功，按 ABI 契约 ok() 会清空 last_error，所以这里不再写
         * 回落原因（写了也会被清掉）。GPU 是否真的参与由后端实例的
         * last_render_offloaded() 与测试/shell 自检负责证明，而不是靠错误串。
         */
        return compositor::abi::ok(engine);
      }
      return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, cpu_error);
    }
    return compositor::abi::fail(engine, COMP_ERR_UNSUPPORTED, gpu_error);
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
  BackendPick pick = pick_backend(engine, nullptr);
  if (pick.backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, "cannot create render backend");
  }
  if (!pick.backend->present(*doc, &error)) {
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
  BackendPick pick = pick_backend(engine, nullptr);
  if (pick.backend == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_BACKEND, "cannot create render backend");
  }
  if (!pick.backend->wait(*doc, &error)) {
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
  const comp_result rendered = comp_render_document(document, &target);
  if (rendered != COMP_OK) return rendered;
  /*
   * P0-1 灰阶模式：契约是「宿主观测到的一切像素输出恒为 R=G=B」。
   *
   * 放在 flatten 而不是合成内核里，是因为 flatten 是所有输出口径的唯一收口
   * （上屏的 CPU 帧、PNG 导出、PSD 导出用的合成结果、16F 导出全都走这里），
   * 一处兜住就不存在「某条导出路径漏了」。代价是灰阶文档每帧多一遍全画布
   * 扫描——只在 color_mode 为 GRAYSCALE 时发生，RGB 文档零开销。
   *
   * GPU 后端回落 CPU 的情形无需额外处理：这里拿到的已经是回读后的缓冲。
   */
  if (compositor::abi::document_is_gray(doc->state())) {
    compositor::abi::desaturate_premultiplied(pixels, doc->state().width, doc->state().height,
                                              stride, format);
  }
  return rendered;
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
