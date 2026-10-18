// AI生成
#include "compositing/compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "compositing/composite_ops.h"
#include "compositing/filter_graph.h"

namespace compositor {

namespace {

/*
 * half 表面 -> 缩略图 API 的 8 位显示副本。
 *
 * 上游 `DeepLayer.image(_:)` 的口径：白点以上按白显示（截断），不是丢弃高光
 * ——高光在合成途中已经按 half 保留，只有这份显示副本才降到 8 位。
 * 失败时保留原缓冲不动，调用方拿到 false 与具名 error。
 */
bool to_display_rgba8(ImageBuffer* buffer, std::string* error) {
  ImageBuffer view;
  if (buffer == nullptr || !hdr_to_rgba8(*buffer, &view)) {
    if (error != nullptr) *error = "failed to build an 8-bit display copy of an HDR layer view";
    return false;
  }
  *buffer = std::move(view);
  return true;
}

}  // namespace

comp_pixel_format composite_surface_format(const Document& doc) {
  /* 扁平存储 + parent 引用：扫一遍全部图层即可，无需递归。 */
  for (const Layer& layer : doc.state().layers) {
    if (layer.pixels.format == COMP_PIXEL_RGBA16F && !layer.pixels.empty()) {
      return COMP_PIXEL_RGBA16F;
    }
  }
  return COMP_PIXEL_RGBA8;
}

bool composite_document(const Document& doc, ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out buffer is null";
    return false;
  }
  const DocumentState& state = doc.state();
  if (state.width <= 0 || state.height <= 0) {
    if (error != nullptr) *error = "document canvas is empty";
    return false;
  }

  /* 线性叠加已升级为有向图求值：建图 -> 按拓扑序合成（详见 filter_graph.h）。 */
  FilterGraph graph;
  if (!build_filter_graph(doc, &graph, error)) return false;
  return execute_filter_graph(doc, graph, out, error);
}

bool composite_layer(const Document& doc, comp_layer_id id, ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out buffer is null";
    return false;
  }
  const DocumentState& state = doc.state();
  const Layer* layer = doc.find(id);
  if (layer == nullptr) {
    if (error != nullptr) *error = "layer not found";
    return false;
  }

  /*
   * C5：单层视图的深度跟图层本身走——half 图层在 half 表面上求值，再取 8 位
   * 显示副本返回（缩略图 API 的输出契约仍是 RGBA8）。这样高光在合成途中
   * 不会先被截到白；只有最终显示副本才按上游 `image(_:)` 的口径截断。
   * 剪贴基底是 half 时同一张表面也得是 half，否则基底高光在栈内就丢了。
   */
  comp_pixel_format surface_format =
      layer->pixels.format == COMP_PIXEL_RGBA16F ? COMP_PIXEL_RGBA16F : COMP_PIXEL_RGBA8;
  if (layer->clipping_source != 0) {
    const Layer* clip_base = doc.find(layer->clipping_source);
    if (clip_base != nullptr && clip_base->pixels.format == COMP_PIXEL_RGBA16F) {
      surface_format = COMP_PIXEL_RGBA16F;
    }
  }

  out->resize(state.width, state.height, surface_format);
  out->clear_transparent();

  /*
   * 若存在剪贴基底，走剪贴栈口径（与 FilterGraph 的 kClippingStack 节点同一条路）：
   * 基底 alpha 取出 → 子层在不透明基底上叠 → alpha 放回 → 整个栈作为一个单元参与。
   *
   * 此前这里是「先合成基底、再按基底 alpha 逐像素缩放本层」，alpha 会被叠成 0
   * （实测：软 alpha 基底 + MULTIPLY 子层得 a=0，上游口径应得 a=128），
   * 而且非 Normal 混合的语义完全不对。
   *
   * 单层视图（comp_render_composite_layer）只见到「基底 + 这一个成员」，
   * 而 FilterGraph 会见到整串成员；两处对「一层栈」的定义一致，结果也就一致。
   */
  if (layer->clipping_source != 0) {
    const Layer* base = doc.find(layer->clipping_source);
    if (base != nullptr && !base->pixels.empty()) {
      Layer base_standalone = *base;
      base_standalone.clipping_source = 0;
      Layer member = *layer;
      member.clipping_source = 0;

      /*
       * 表面是**画布大小**的：blend_layer_into 把 dst 的尺寸当画布边界
       * （clamp_to_canvas(padded, dst->size)），局部坐标缓冲会被当成画布而错位。
       * 栈内部是整幅画布的 extract/member/restore，缩小到 bounds 是 FilterGraph
       * 那边的做法；此处保持画布大小，两处求值结果仍然一致。
       */
      ImageBuffer surface;
      surface.resize(state.width, state.height, surface_format);
      surface.clear_transparent();
      std::string stack_error;
      if (!blend_layer_into(&surface, doc, base_standalone, 1.0, &stack_error)) {
        if (error != nullptr) *error = stack_error;
        return false;
      }
      /* 栈内 coverage 与表面同深度：half 用 float，其余用 16 位整数。 */
      const bool half_stack = surface_format == COMP_PIXEL_RGBA16F;
      std::vector<uint16_t> coverage;
      std::vector<float> coverage_hdr;
      const bool extracted = half_stack
                                 ? extract_stack_alpha(&surface, &coverage_hdr, &stack_error)
                                 : extract_stack_alpha(&surface, &coverage, &stack_error);
      if (!extracted) {
        if (error != nullptr) *error = stack_error;
        return false;
      }
      if (!blend_layer_into(&surface, doc, member, 1.0, &stack_error)) {
        if (error != nullptr) *error = stack_error;
        return false;
      }
      const bool restored = half_stack
                                ? restore_stack_alpha(&surface, coverage_hdr, &stack_error)
                                : restore_stack_alpha(&surface, coverage, &stack_error);
      if (!restored) {
        if (error != nullptr) *error = stack_error;
        return false;
      }
      if (!blend_buffer_into(out, surface, base->blend, static_cast<double>(layer->opacity))) {
        if (error != nullptr) *error = "failed to blend the clipping stack onto the layer view";
        return false;
      }
      return surface_format == COMP_PIXEL_RGBA16F ? to_display_rgba8(out, error) : true;
    }
  }
  Layer standalone = *layer;
  standalone.clipping_source = 0;
  if (!blend_layer_into(out, doc, standalone, 1.0, error)) return false;
  return surface_format == COMP_PIXEL_RGBA16F ? to_display_rgba8(out, error) : true;
}

bool downsample_rgba8(const ImageBuffer& src, int32_t max_edge, ImageBuffer* out) {
  if (out == nullptr || src.empty() || max_edge <= 0) return false;

  const int32_t src_max = std::max(src.width, src.height);
  if (src_max <= max_edge) {
    *out = src;
    return true;
  }

  const double scale = static_cast<double>(max_edge) / static_cast<double>(src_max);
  const int32_t dst_w = std::max(1, static_cast<int32_t>(std::lround(src.width * scale)));
  const int32_t dst_h = std::max(1, static_cast<int32_t>(std::lround(src.height * scale)));

  out->resize(dst_w, dst_h, COMP_PIXEL_RGBA8);
  for (int32_t y = 0; y < dst_h; ++y) {
    uint8_t* dst_row = out->writable_row(y);
    const int32_t sy = std::min(src.height - 1, static_cast<int32_t>(y / scale));
    for (int32_t x = 0; x < dst_w; ++x) {
      const int32_t sx = std::min(src.width - 1, static_cast<int32_t>(x / scale));
      const uint8_t* src_px = src.row(sy) + static_cast<size_t>(sx) * 4;
      std::memcpy(dst_row + static_cast<size_t>(x) * 4, src_px, 4);
    }
  }
  return true;
}

}  // namespace compositor
