// AI生成
/*
 * crop.cpp — 裁剪框几何与提交。语义说明见 domain/crop.h。
 *
 * 这里刻意不碰像素：裁剪不重采样、不重排缓冲，只平移图层原点并把画布换小
 * （上游 CanvasResizer 正是这么做的——渲染时画布边界自然裁掉多余像素）。
 */
#include "domain/crop.h"

#include <algorithm>
#include <cmath>

#include "core/comp_geometry.h"

namespace compositor {
namespace {

/* 取整前的坐标闸：超过这个量级的输入只可能来自未初始化的内存或坏的调用方，
 * 继续算下去既无意义又可能让 llround 溢出（UB）。合法裁剪框的坐标上限是
 * kMaxTransformOrigin = 1e6，留四个数量级的余量。 */
constexpr double kMaxFrameCoordinate = 1.0e7;

bool frame_is_finite(const CropFrame& frame) {
  return std::isfinite(frame.x) && std::isfinite(frame.y) && std::isfinite(frame.width) &&
         std::isfinite(frame.height);
}

}  // namespace

bool crop_snap_frame(const CropFrame& frame, comp_rect* out_rect) {
  if (out_rect == nullptr || !frame_is_finite(frame)) return false;

  /* 标准化：负宽高的拖拽（从右往左拉）先翻正，再对四条边分别取整。 */
  const double x0 = std::min(frame.x, frame.x + frame.width);
  const double x1 = std::max(frame.x, frame.x + frame.width);
  const double y0 = std::min(frame.y, frame.y + frame.height);
  const double y1 = std::max(frame.y, frame.y + frame.height);
  if (std::fabs(x0) > kMaxFrameCoordinate || std::fabs(x1) > kMaxFrameCoordinate ||
      std::fabs(y0) > kMaxFrameCoordinate || std::fabs(y1) > kMaxFrameCoordinate) {
    return false;
  }

  /* std::llround 是「半边远离零」，与 Swift 的 .rounded() 同口径。 */
  const long long left = std::llround(x0);
  const long long top = std::llround(y0);
  long long width = std::llround(x1) - left;
  long long height = std::llround(y1) - top;
  if (width < 1) width = 1;
  if (height < 1) height = 1;

  out_rect->x = static_cast<int32_t>(left);
  out_rect->y = static_cast<int32_t>(top);
  out_rect->w = static_cast<int32_t>(width);
  out_rect->h = static_cast<int32_t>(height);
  return true;
}

bool crop_create_frame(double start_x, double start_y, double end_x, double end_y, double ratio,
                       bool symmetric, comp_rect* out_rect) {
  if (out_rect == nullptr) return false;
  if (!std::isfinite(start_x) || !std::isfinite(start_y) || !std::isfinite(end_x) ||
      !std::isfinite(end_y) || !std::isfinite(ratio)) {
    return false;
  }

  double dx = end_x - start_x;
  double dy = end_y - start_y;
  if (ratio > 0.0) {
    /* 上游：|dx| > |dy| * ratio 时高度由宽度决定，否则宽度由高度决定。 */
    if (std::fabs(dx) > std::fabs(dy) * ratio) {
      dy = (dy < 0.0 ? -1.0 : 1.0) * std::fabs(dx) / ratio;
    } else {
      dx = (dx < 0.0 ? -1.0 : 1.0) * std::fabs(dy) * ratio;
    }
  }

  CropFrame frame;
  if (symmetric) {
    /* Option 拖拽：起点为中心，四个方向等距展开。 */
    frame.x = start_x - std::fabs(dx);
    frame.y = start_y - std::fabs(dy);
    frame.width = std::fabs(dx) * 2.0;
    frame.height = std::fabs(dy) * 2.0;
  } else {
    frame.x = std::min(start_x, start_x + dx);
    frame.y = std::min(start_y, start_y + dy);
    frame.width = std::fabs(dx);
    frame.height = std::fabs(dy);
  }
  return crop_snap_frame(frame, out_rect);
}

bool crop_is_valid(const comp_rect& rect) {
  if (rect.w <= 0 || rect.h <= 0) return false;
  if (rect.w > limits::kMaxSide || rect.h > limits::kMaxSide) return false;
  if (rect.x > static_cast<int32_t>(limits::kMaxTransformOrigin) ||
      rect.x < -static_cast<int32_t>(limits::kMaxTransformOrigin)) {
    return false;
  }
  if (rect.y > static_cast<int32_t>(limits::kMaxTransformOrigin) ||
      rect.y < -static_cast<int32_t>(limits::kMaxTransformOrigin)) {
    return false;
  }
  /* 上游只查单边，本仓额外守住画布像素预算（kMaxSurfacePixels，见
   * core/comp_geometry.h 的已知分歧）。 */
  return is_valid_canvas(comp_size{rect.w, rect.h});
}

bool crop_change_ratio(const comp_rect& rect, double ratio, comp_rect* out_rect, bool* changed) {
  if (out_rect == nullptr || !std::isfinite(ratio) || ratio <= 0.0) return false;
  if (changed != nullptr) *changed = false;

  /* 保持宽度，高度由比例决定，垂直居中（上游 changeCropRatio）。 */
  const double height = static_cast<double>(rect.w) / ratio;
  const double mid_y = static_cast<double>(rect.y) + static_cast<double>(rect.h) / 2.0;
  const CropFrame frame{static_cast<double>(rect.x), mid_y - height / 2.0,
                        static_cast<double>(rect.w), height};

  comp_rect next{};
  if (!crop_snap_frame(frame, &next) || !crop_is_valid(next)) {
    /* 取整后不合法：保持原框（上游 `if CropGeometry.valid(next) { cropRect = next }`）。 */
    *out_rect = rect;
    return true;
  }
  *out_rect = next;
  if (changed != nullptr) {
    *changed = next.x != rect.x || next.y != rect.y || next.w != rect.w || next.h != rect.h;
  }
  return true;
}

bool crop_apply(DocumentState* state, const comp_rect& rect, bool* changed) {
  if (state == nullptr || !crop_is_valid(rect)) return false;
  if (changed != nullptr) *changed = false;

  /* 尺寸与原点都没变 = 没有裁剪（上游 CanvasResizer 的短路）。 */
  if (rect.x == 0 && rect.y == 0 && rect.w == state->width && rect.h == state->height) {
    return true;
  }

  /*
   * 两遍扫描：先把每个图层的**新** transform 校验完，再真正写入。
   * 单遍「边改边校验」在某个图层越界时要回滚前面已改的图层，徒增出错面。
   */
  for (const Layer& layer : state->layers) {
    const long long x = static_cast<long long>(layer.transform.x) - rect.x;
    const long long y = static_cast<long long>(layer.transform.y) - rect.y;
    if (x > limits::kMaxTransformOrigin || x < -limits::kMaxTransformOrigin ||
        y > limits::kMaxTransformOrigin || y < -limits::kMaxTransformOrigin) {
      return false;
    }
    const comp_rect moved{static_cast<int32_t>(x), static_cast<int32_t>(y), layer.transform.w,
                          layer.transform.h};
    if (!is_valid_transform(moved, layer.rotation)) return false;
  }

  for (Layer& layer : state->layers) {
    layer.transform.x -= rect.x;
    layer.transform.y -= rect.y;
  }
  state->width = rect.w;
  state->height = rect.h;
  /* 换画布 = 换坐标系：选区蒙版按旧画布分辨率存的，留下就是错位的。
   * 上游 commitCrop → applyDocumentSize 重建 CanvasDocument，选区自然消失。 */
  state->selection = SelectionState{};
  if (changed != nullptr) *changed = true;
  return true;
}

}  // namespace compositor
