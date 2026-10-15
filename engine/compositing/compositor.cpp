// AI生成
#include "compositing/compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "compositing/composite_ops.h"
#include "compositing/filter_graph.h"

namespace compositor {

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

  out->resize(state.width, state.height, COMP_PIXEL_RGBA8);
  out->clear_transparent();

  /* 若存在剪贴基底，先合成基底再合成本层，保证结果与整篇合成一致。 */
  if (layer->clipping_source != 0) {
    const Layer* base = doc.find(layer->clipping_source);
    if (base != nullptr && !blend_layer_into(out, doc, *base, 1.0)) return false;
  }
  Layer standalone = *layer;
  standalone.clipping_source = 0; /* 基底已单独合成，避免重复裁剪 */
  return blend_layer_into(out, doc, standalone, 1.0);
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
