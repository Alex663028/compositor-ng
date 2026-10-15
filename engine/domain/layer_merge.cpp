// AI生成
#include "domain/layer_merge.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#include "compositing/compositor.h"
#include "domain/document.h"

namespace compositor {

bool composite_merge_subset(const Document& doc, const std::vector<comp_layer_id>& subset,
                            ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out buffer is null";
    return false;
  }
  if (subset.empty()) {
    if (error != nullptr) *error = "merge subset is empty";
    return false;
  }

  /* 子集状态：与活文档同画布；Layer 拷贝只增 shared_ptr 引用，不复制像素。 */
  DocumentState state = doc.state();
  state.layers.clear();
  state.active_layer = 0;

  const std::set<comp_layer_id> keep(subset.begin(), subset.end());
  for (comp_layer_id id : subset) {
    const Layer* layer = doc.find(id);
    if (layer == nullptr) {
      if (error != nullptr) *error = "merge subset references a missing layer";
      return false;
    }
    Layer copy = *layer;
    /* 剪断集外父引用：合成器把挂靠不存在父级的图层判为孤儿并整篇拒绝。 */
    if (copy.parent != 0 && keep.find(copy.parent) == keep.end()) copy.parent = 0;
    /* 剪断集外剪贴基底：基底不在集内按无裁剪处理（composite_layer 同款宽容）。 */
    if (copy.clipping_source != 0 && keep.find(copy.clipping_source) == keep.end()) {
      copy.clipping_source = 0;
    }
    state.layers.push_back(std::move(copy));
  }

  /*
   * 临时文档只用于驱动合成器（与上游 CanvasDocument(layers: subset) 同思路）。
   * state 在构造后整体替换：DocumentState 对象地址不变，构造函数里登记的
   * history live 指针仍然有效。
   */
  Document temp(doc.engine(), state.width, state.height, state.dpi, state.color_space);
  temp.state() = std::move(state);
  return composite_document(temp, out, error);
}

bool trim_to_content(const ImageBuffer& src, ImageBuffer* out, comp_rect* out_placement) {
  if (out == nullptr || src.empty()) return false;

  /*
   * 步长与像素偏移一律按 src.format 的实际字节宽计算：写死 4 会在
   * 非 RGBA8 缓冲上按错误的字节位置判alpha、按错误的宽度拷行，
   * 得到错位且越界的裁剪结果。
   */
  const int32_t bpp = bytes_per_pixel(src.format);
  if (bpp <= 0) return false;

  int32_t min_x = src.width;
  int32_t min_y = src.height;
  int32_t max_x = -1;
  int32_t max_y = -1;
  for (int32_t y = 0; y < src.height; ++y) {
    const uint8_t* row = src.row(y);
    for (int32_t x = 0; x < src.width; ++x) {
      /* alpha 位于每像素最后一字节（RGBA8=a[3]，RGBA16/16F 的高字节）。 */
      const uint8_t* px = row + static_cast<size_t>(x) * static_cast<size_t>(bpp);
      if (px[bpp - 1] == 0) continue;
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
  }
  if (max_x < min_x || max_y < min_y) return false; /* 全透明：无内容可裁 */

  const int32_t w = max_x - min_x + 1;
  const int32_t h = max_y - min_y + 1;
  out->resize(w, h, src.format);
  const size_t copy_bytes = static_cast<size_t>(w) * static_cast<size_t>(bpp);
  for (int32_t y = 0; y < h; ++y) {
    std::memcpy(out->writable_row(y),
                src.row(min_y + y) + static_cast<size_t>(min_x) * static_cast<size_t>(bpp),
                copy_bytes);
  }
  if (out_placement != nullptr) *out_placement = comp_rect{min_x, min_y, w, h};
  return true;
}

}  // namespace compositor
