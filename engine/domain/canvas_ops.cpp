// AI生成
/*
 * canvas_ops.cpp —「图像」菜单一次性操作的领域实现（S25）。
 *
 * 三个入口的语义真源见 canvas_ops.h 头注释。实现上刻意遵循本仓既有模式：
 *   - rotate_canvas 是纯 DocumentState 操作（与 crop_apply 同族，不碰像素）；
 *   - document_trim 先 flatten（只读）算框，再复用 crop_apply 提交；
 *   - invert_layer 完全照 filters.cpp 的事务 / 16 位 / HDR / 选区口径。
 */
#include "domain/canvas_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "compositing/compositor.h"
#include "core/comp_geometry.h"
#include "domain/crop.h"
#include "domain/document.h"
#include "domain/selection.h"

namespace compositor {
namespace {

/* 上游 quarterTurned 的中心点映射（y 向下的文档像素坐标）。
 *
 * 顺时针 90°：middle = (canvas.height - center.y, center.x)
 * 逆时针 90°：middle = (center.y, canvas.width - center.x)
 * 180°：     middle = (canvas.width - center.x, canvas.height - center.y)
 *
 * quarter_turns 规范化到 {1,-1,2}：-2 与 2 都是 180°。返回 0 表示输入非法。
 */
int32_t normalize_turns(int32_t quarter_turns) {
  switch (quarter_turns) {
    case 1:
    case -1:
    case 2:
    case -2:
      return quarter_turns == -2 ? 2 : quarter_turns;
    default:
      return 0;
  }
}

/* 把旋转后的中心点换算成左上角 origin 并取整。上游 position 是 Double，
 * 本仓 transform origin 是 int32_t：奇数边长按 llround（.5 远离零）取整，
 * 与 crop_snap_frame 同口径。返回新角度并归一化到 (-360, 360]。 */
float turned_transform(double center_x, double center_y, double w, double h,
                       int32_t quarter_turns, float rotation, int32_t* out_x, int32_t* out_y) {
  const double middle_x = center_x - w / 2.0;
  const double middle_y = center_y - h / 2.0;
  out_x[0] = static_cast<int32_t>(std::llround(middle_x));
  out_y[0] = static_cast<int32_t>(std::llround(middle_y));
  float turned = rotation + static_cast<float>(quarter_turns) * 90.0f;
  /* 归一化到 (-360, 360]（上游 truncatingRemainder(dividingBy: 360)）。 */
  while (turned > 360.0f) turned -= 360.0f;
  while (turned <= -360.0f) turned += 360.0f;
  return turned;
}

/* 8 位单通道蒙版的 90°/180° 精确转置（无插值：文档分辨率选区是衍生数据）。 */
bool rotate_mask_90(const MaskBuffer& src, MaskBuffer* out, int32_t quarter_turns) {
  if (out == nullptr) return false;
  const int32_t w = src.width;
  const int32_t h = src.height;
  if (w <= 0 || h <= 0) return false;
  const bool clock = quarter_turns == 1;
  out->resize(quarter_turns == 2 ? w : h, quarter_turns == 2 ? h : w, 0);
  const uint8_t* s = src.pixel_data().data();
  uint8_t* d = out->writable_pixels().data();
  const size_t src_stride = src.stride();
  const size_t dst_stride = out->stride();
  if (quarter_turns == 2) {
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        d[(static_cast<size_t>(h - 1 - y)) * dst_stride + (w - 1 - x)] =
            s[static_cast<size_t>(y) * src_stride + x];
      }
    }
    return true;
  }
  /* 90°：dst 尺寸为 (h, w)（宽 h 高 w）。视觉旋转必须同时换行列：源 (x_s, y_s)
   * 顺时针落到 dst(x_d = h - 1 - y_s, y_d = x_s)，逆时针落到 dst(x_d = y_s,
   * y_d = w - 1 - x_s)。旧的写法把源行/列直接当目标列/行用，目标窄于源时
   * 写进相邻行（非正方形蒙版结果全错）。 */
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const uint8_t value = s[static_cast<size_t>(y) * src_stride + x];
      if (clock) {
        d[static_cast<size_t>(x) * dst_stride + static_cast<size_t>(h - 1 - y)] = value;
      } else {
        d[static_cast<size_t>(w - 1 - x) * dst_stride + static_cast<size_t>(y)] = value;
      }
    }
  }
  return true;
}

}  // namespace

bool rotate_canvas(DocumentState* state, int32_t quarter_turns, bool* changed) {
  if (state == nullptr) return false;
  const int32_t turns = normalize_turns(quarter_turns);
  if (turns == 0) return false;
  if (changed != nullptr) *changed = false;

  const int32_t old_w = state->width;
  const int32_t old_h = state->height;
  const bool swap_dims = turns != 2; /* 180° 时画布尺寸不变 */
  const int32_t new_w = swap_dims ? old_h : old_w;
  const int32_t new_h = swap_dims ? old_w : old_h;

  /* 无变化的唯一情形：空文档（无图层、无选区）且画布尺寸不变。 */
  const bool empty_document = state->layers.empty() && !state->selection.active;
  if (empty_document && new_w == old_w && new_h == old_h) {
    return true;
  }

  /* 两遍：先校验新 origin / 落位尺寸，再写入（与 crop_apply 同款防半途回滚）。
   *
   * 落位口径（重要）：渲染器 build_layer_unit 的 rotation 是「绕 bounds 中心转，
   * 光栅窗口 = 轴对齐 bounds」——90° 旋转后图层实际占的是 bounds.h × bounds.w 的
   * 竖矩形，若 transform 仍是原横矩形，旋转出去的那一半内容落在光栅窗口之外，
   * 整页渲染就会缺一半。所以 90° 落位必须把 transform 换成旋转后的包围盒：
   * **图层像素一个字节不动**（不重采样原则），动的只是 origin / 落位尺寸 /
   * rotation。180° 的轴对齐包围盒不变，尺寸原样写回。 */
  for (const Layer& layer : state->layers) {
    const comp_rect bounds = layer.effective_bounds();
    if (bounds.w <= 0 || bounds.h <= 0) continue; /* 无效图层几何不动 */
    const double cx = static_cast<double>(bounds.x) + static_cast<double>(bounds.w) / 2.0;
    const double cy = static_cast<double>(bounds.y) + static_cast<double>(bounds.h) / 2.0;
    double middle_x = cx;
    double middle_y = cy;
    if (turns == 1) {
      middle_x = static_cast<double>(old_h) - cy;
      middle_y = cx;
    } else if (turns == -1) {
      middle_x = cy;
      middle_y = static_cast<double>(old_w) - cx;
    } else {
      middle_x = static_cast<double>(old_w) - cx;
      middle_y = static_cast<double>(old_h) - cy;
    }
    const double half_w =
        static_cast<double>(turns == 2 ? bounds.w : bounds.h) / 2.0;
    const double half_h =
        static_cast<double>(turns == 2 ? bounds.h : bounds.w) / 2.0;
    const double nx = middle_x - half_w;
    const double ny = middle_y - half_h;
    if (nx > static_cast<double>(limits::kMaxTransformOrigin) ||
        nx < -static_cast<double>(limits::kMaxTransformOrigin) ||
        ny > static_cast<double>(limits::kMaxTransformOrigin) ||
        ny < -static_cast<double>(limits::kMaxTransformOrigin)) {
      return false;
    }
  }

  for (Layer& layer : state->layers) {
    const comp_rect bounds = layer.effective_bounds();
    if (bounds.w <= 0 || bounds.h <= 0) continue;
    const double cx = static_cast<double>(bounds.x) + static_cast<double>(bounds.w) / 2.0;
    const double cy = static_cast<double>(bounds.y) + static_cast<double>(bounds.h) / 2.0;
    double middle_x = cx;
    double middle_y = cy;
    if (turns == 1) {
      middle_x = static_cast<double>(old_h) - cy;
      middle_y = cx;
    } else if (turns == -1) {
      middle_x = cy;
      middle_y = static_cast<double>(old_w) - cx;
    } else {
      middle_x = static_cast<double>(old_w) - cx;
      middle_y = static_cast<double>(old_h) - cy;
    }
    const int32_t nw = turns == 2 ? bounds.w : bounds.h;
    const int32_t nh = turns == 2 ? bounds.h : bounds.w;
    int32_t nx = 0;
    int32_t ny = 0;
    const float rotated = turned_transform(middle_x, middle_y, nw, nh, turns,
                                           layer.rotation, &nx, &ny);
    layer.transform.x = nx;
    layer.transform.y = ny;
    /* 90°：写旋转后的轴对齐落位包围盒（像素不动，见上方落位口径）；
     * 180°：包围盒不变，把尺寸显式写回（不依赖 transform.w/h<=0 的回落）。 */
    layer.transform.w = nw;
    layer.transform.h = nh;
    layer.rotation = rotated;
    /* clipped_empty 的图层在 effective_bounds() 里就是空矩形，已被上面的
     * bounds 闸跳过；这里不必（也走不到）清位。 */
  }

  state->width = new_w;
  state->height = new_h;

  /* 选区随动：有选区且网格非空时精确转置。 */
  if (state->selection.active && !state->selection.base.empty()) {
    MaskBuffer turned;
    if (rotate_mask_90(state->selection.base, &turned, turns)) {
      state->selection.base = std::move(turned);
    } else {
      /* 蒙版损坏（尺寸非法）：按上游「换画布丢选区」的保守口径丢弃。 */
      state->selection = SelectionState{};
    }
  } else if (state->selection.active) {
    /* 显式空选区：网格为空，换坐标系后无法对齐，丢弃（与 crop 同口径）。 */
    state->selection = SelectionState{};
  }

  if (changed != nullptr) *changed = true;
  return true;
}

bool trim_calculate_rect(const uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                         const TrimOptions& options, comp_rect* out_rect) {
  if (out_rect == nullptr) return false;
  if (rgba == nullptr || width <= 0 || height <= 0) return false;
  if (stride < static_cast<size_t>(width) * 4) return false;
  if (!options.top && !options.bottom && !options.left && !options.right) return false;

  const int32_t tol = static_cast<int32_t>(options.tolerance);

  if (options.based_on == TrimBasedOn::TransparentPixels) {
    /* alpha 边界：row 从左找首个 alpha>tol，从右找末个；col 同理（逐行扫描
     * 已能给出上下界，左右用每行首末汇总——与上游 brush_alpha_bounds 的
     * 四值汇合同口径，且保证行中间的透明不误判）。 */
    int32_t left = width, right = 0, top = height, bottom = 0;
    for (int32_t y = 0; y < height; ++y) {
      const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
      int32_t first = 0;
      while (first < width && static_cast<int32_t>(row[first * 4 + 3]) <= tol) ++first;
      if (first == width) continue;
      int32_t last = width;
      while (last > first && static_cast<int32_t>(row[(last - 1) * 4 + 3]) <= tol) --last;
      if (first < left) left = first;
      if (last > right) right = last;
      if (y < top) top = y;
      bottom = y + 1;
    }
    if (right <= 0) return false; /* 整图透明（上游 guard edges[2] > 0） */
    const int32_t min_x = options.left ? left : 0;
    const int32_t min_y = options.top ? top : 0;
    const int32_t max_x = options.right ? right : width;
    const int32_t max_y = options.bottom ? bottom : height;
    if (max_x <= min_x || max_y <= min_y) return false;
    out_rect->x = min_x;
    out_rect->y = min_y;
    out_rect->w = max_x - min_x;
    out_rect->h = max_y - min_y;
    return true;
  }

  /* 颜色基准：逐通道 abs 差 <= tol（RGBA 四通道都比），基准像素为角点。 */
  const int32_t sx = options.based_on == TrimBasedOn::BottomRightPixelColor ? width - 1 : 0;
  const int32_t sy = options.based_on == TrimBasedOn::BottomRightPixelColor ? height - 1 : 0;
  const uint8_t* sample = rgba + static_cast<size_t>(sy) * stride + static_cast<size_t>(sx) * 4;
  const int32_t target[4] = {sample[0], sample[1], sample[2], sample[3]};

  int32_t left = width, right = 0, top = height, bottom = 0;
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    int32_t first = 0;
    while (first < width) {
      const uint8_t* p = row + static_cast<size_t>(first) * 4;
      if (std::abs(static_cast<int32_t>(p[0]) - target[0]) <= tol &&
          std::abs(static_cast<int32_t>(p[1]) - target[1]) <= tol &&
          std::abs(static_cast<int32_t>(p[2]) - target[2]) <= tol &&
          std::abs(static_cast<int32_t>(p[3]) - target[3]) <= tol) {
        ++first;
      } else {
        break;
      }
    }
    if (first == width) continue;
    int32_t last = width;
    while (last > first) {
      const uint8_t* p = row + static_cast<size_t>(last - 1) * 4;
      if (std::abs(static_cast<int32_t>(p[0]) - target[0]) <= tol &&
          std::abs(static_cast<int32_t>(p[1]) - target[1]) <= tol &&
          std::abs(static_cast<int32_t>(p[2]) - target[2]) <= tol &&
          std::abs(static_cast<int32_t>(p[3]) - target[3]) <= tol) {
        --last;
      } else {
        break;
      }
    }
    if (first < left) left = first;
    if (last > right) right = last;
    if (y < top) top = y;
    bottom = y + 1;
  }
  if (right <= 0) return false; /* 整图都是基准色（上游 guard right > 0） */
  const int32_t min_x = options.left ? left : 0;
  const int32_t min_y = options.top ? top : 0;
  const int32_t max_x = options.right ? right : width;
  const int32_t max_y = options.bottom ? bottom : height;
  if (max_x <= min_x || max_y <= min_y) return false;
  out_rect->x = min_x;
  out_rect->y = min_y;
  out_rect->w = max_x - min_x;
  out_rect->h = max_y - min_y;
  return true;
}

comp_result document_trim(Document* doc, const TrimOptions& options, int32_t* out_changed,
                          std::string* error) {
  if (out_changed != nullptr) *out_changed = 0;
  auto fail = [error](comp_result code, const char* text) {
    if (error != nullptr) *error = text;
    return code;
  };
  if (doc == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "trim needs a document");

  /* 参数闸（trimsAny 由 trim_calculate_rect 内部守，此处重复是为了在 flatten
   * 之前就拒绝——不为一眼非法的输入花一次整篇合成）。 */
  if (!options.top && !options.bottom && !options.left && !options.right) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "trim needs at least one enabled edge");
  }
  if (doc->state().layers.empty()) {
    return fail(COMP_ERR_STATE, "empty document has nothing to trim");
  }

  /* flatten 是只读操作：在事务之外做，合成结果不受事务影响，也避免
   * 「事务中 trim」的中间态被算进边框。 */
  ImageBuffer composited;
  std::string compose_error;
  if (!composite_document(*doc, &composited, &compose_error)) {
    return fail(COMP_ERR_INTERNAL, "trim could not composite the document");
  }

  /* 合成可能是 16 位 / HDR：颜色匹配需要 8 位视图。复用 deep/hdr_to_rgba8
   * （与 selection 的归一化同一家族），只读，不落盘到文档。 */
  const uint8_t* rgba = nullptr;
  size_t stride = 0;
  ImageBuffer rgba8_scratch;
  const int32_t canvas_w = doc->state().width;
  const int32_t canvas_h = doc->state().height;
  if (composited.format == COMP_PIXEL_RGBA8) {
    rgba = composited.pixel_data().data();
    stride = composited.stride();
  } else {
    const bool converted = composited.format == COMP_PIXEL_RGBA16F
                               ? hdr_to_rgba8(composited, &rgba8_scratch)
                               : deep_to_rgba8(composited, &rgba8_scratch);
    if (!converted) {
      return fail(COMP_ERR_UNSUPPORTED, "trim could not build an 8-bit composite view");
    }
    rgba = rgba8_scratch.pixel_data().data();
    stride = rgba8_scratch.stride();
  }

  comp_rect trim_rect{};
  if (!trim_calculate_rect(rgba, stride, canvas_w, canvas_h, options, &trim_rect)) {
    return fail(COMP_ERR_STATE, "no content remains after trimming the edges");
  }

  bool changed = false;
  if (!crop_apply(&doc->state(), trim_rect, &changed)) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "trim rect overflowed a layer transform");
  }
  if (changed) doc->touch_all_layers();
  if (out_changed != nullptr) *out_changed = changed ? 1 : 0;
  return COMP_OK;
}

comp_result invert_layer(Document* doc, comp_layer_id target, int32_t* out_changed,
                         std::string* error) {
  if (out_changed != nullptr) *out_changed = 0;
  auto fail = [error](comp_result code, const std::string& text) {
    if (error != nullptr) *error = text;
    return code;
  };
  if (doc == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "invert needs a document");

  const Layer* probe = doc->find(target);
  if (probe == nullptr) return fail(COMP_ERR_NOT_FOUND, "invert target layer was not found");
  if (probe->is_group) return fail(COMP_ERR_STATE, "invert cannot run on a group layer");
  if (!probe->has_pixels()) return fail(COMP_ERR_STATE, "invert layer has no pixels");

  const SelectionState& selection = doc->state().selection;
  const bool clip = selection.active;
  if (clip && selection_is_empty(selection)) {
    /* 显式空选区（与 filter_apply 同口径）：什么都不改。 */
    if (out_changed != nullptr) *out_changed = 0;
    return COMP_OK;
  }

  const bool owns_txn = !doc->txn_active();
  std::string txn_error;
  if (owns_txn && doc->txn_begin("Image: Invert", &txn_error) != COMP_OK) {
    return fail(COMP_ERR_STATE, txn_error);
  }
  auto abort_txn = [&]() {
    if (owns_txn) doc->txn_abort(&txn_error);
  };

  Layer* layer = doc->find(target);
  if (layer == nullptr) {
    abort_txn();
    return fail(COMP_ERR_NOT_FOUND, "invert target layer vanished after the parameter gate");
  }

  comp_result status = COMP_OK;
  std::string failure;
  try {
    if (layer->pixels.format == COMP_PIXEL_RGBA16) {
      if (!deep_to_rgba8(layer->pixels, &layer->pixels)) {
        status = COMP_ERR_OUT_OF_MEMORY;
        failure = "invert could not build the 8-bit working copy";
      }
    } else if (layer->pixels.format != COMP_PIXEL_RGBA8) {
      status = COMP_ERR_UNSUPPORTED;
      failure = "invert needs an 8-bit or 16-bit layer (an HDR layer has no invert path)";
    }
    if (status == COMP_OK) {
      std::vector<uint8_t> original;
      if (clip) original = layer->pixels.pixel_data();
      uint8_t* pixels = layer->pixels.writable_pixels().data();
      const int32_t w = layer->pixels.width;
      const int32_t h = layer->pixels.height;
      const size_t stride = layer->pixels.stride();
      /* 预乘 RGBA：c' = a - c（上游 PixelInvert 的 4×4 矩阵），alpha 不动。
       * 反相**总是**改变颜色通道（0 ↔ a），恒等只在整层 alpha=0 时出现，
       * 那种层「无内容」已经被 has_pixels/ui 可见性挡住；此处不设恒等短路，
       * 与上游「反相就是一次落墨」一致。 */
      for (int32_t y = 0; y < h; ++y) {
        uint8_t* row = pixels + static_cast<size_t>(y) * stride;
        for (int32_t x = 0; x < w; ++x) {
          uint8_t* p = row + static_cast<size_t>(x) * 4;
          const uint8_t a = p[3];
          p[0] = static_cast<uint8_t>(a) - p[0];
          p[1] = static_cast<uint8_t>(a) - p[1];
          p[2] = static_cast<uint8_t>(a) - p[2];
        }
      }
      if (clip) {
        std::vector<uint8_t> coverage;
        selection_effective_coverage(selection, &coverage);
        blend_through_selection(pixels, stride, original.data(), w, h, coverage, *layer,
                                doc->state().width, doc->state().height);
      }
    }
  } catch (...) {
    abort_txn();
    throw;
  }
  if (status != COMP_OK) {
    abort_txn();
    return fail(status, failure);
  }
  doc->touch_layer(target); /* S18：commit 之前换号。 */
  if (owns_txn && doc->txn_commit(&txn_error) != COMP_OK) return fail(COMP_ERR_STATE, txn_error);
  if (out_changed != nullptr) *out_changed = 1;
  return COMP_OK;
}

}  // namespace compositor
