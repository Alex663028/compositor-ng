// AI生成
#include "compositing/resample.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/comp_geometry.h"

namespace compositor {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline int32_t clamp_index(int32_t v, int32_t limit) {
  if (v < 0) return 0;
  if (v >= limit) return limit - 1;
  return v;
}

inline const uint8_t* fetch(const ImageBuffer& src, int32_t x, int32_t y) {
  return src.row(clamp_index(y, src.height)) + static_cast<size_t>(clamp_index(x, src.width)) * 4;
}

/* 源坐标是否落在源图像内（以像素中心为基准，半像素容差）。 */
inline bool inside(double sx, double sy, int32_t sw, int32_t sh) {
  return sx >= -0.5 && sy >= -0.5 && sx <= static_cast<double>(sw) - 0.5 &&
         sy <= static_cast<double>(sh) - 0.5;
}

void sample_nearest(const ImageBuffer& src, double sx, double sy, uint8_t out[4]) {
  const int32_t xi = static_cast<int32_t>(std::lround(sx));
  const int32_t yi = static_cast<int32_t>(std::lround(sy));
  std::memcpy(out, fetch(src, xi, yi), 4);
}

void sample_bilinear(const ImageBuffer& src, double sx, double sy, uint8_t out[4]) {
  const double fx = std::floor(sx);
  const double fy = std::floor(sy);
  const double tx = sx - fx;
  const double ty = sy - fy;
  const int32_t x0 = static_cast<int32_t>(fx);
  const int32_t y0 = static_cast<int32_t>(fy);
  const uint8_t* p00 = fetch(src, x0, y0);
  const uint8_t* p10 = fetch(src, x0 + 1, y0);
  const uint8_t* p01 = fetch(src, x0, y0 + 1);
  const uint8_t* p11 = fetch(src, x0 + 1, y0 + 1);
  for (int c = 0; c < 4; ++c) {
    const double top = p00[c] * (1.0 - tx) + p10[c] * tx;
    const double bottom = p01[c] * (1.0 - tx) + p11[c] * tx;
    const double value = top * (1.0 - ty) + bottom * ty;
    out[c] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, value + 0.5)));
  }
}

/* Catmull-Rom（B=0, C=0.5）四点权重，f 为 [0,1) 内的小数部分。 */
void catmull_rom_weights(double f, double w[4]) {
  const double f2 = f * f;
  const double f3 = f2 * f;
  w[0] = -0.5 * f3 + f2 - 0.5 * f;
  w[1] = 1.5 * f3 - 2.5 * f2 + 1.0;
  w[2] = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
  w[3] = 0.5 * f3 - 0.5 * f2;
}

/*
 * 预乘不变量护栏：颜色通道不得超过 alpha。
 *
 * 本引擎的像素缓冲按预乘 RGBA8 存储（rgba <= alpha）——这是 blend_pixels 等下游
 * 代码的前提（见 tools/comp_crosscheck.cpp 与 tests/test_compositing.cpp 的不变量断言）。
 * 最近邻是直接取样、双线性是凸组合，二者都保持该不变量；只有 Catmull-Rom 的
 * 权重带负瓣（w[0]、w[3] 在 f>0 时为负），颜色能在低 alpha 处上冲越过 alpha。
 * 夹到 <= alpha 对已满足不变量的输入是空操作，因此不会改变逐字节结果。
 */
inline void enforce_premultiplied(uint8_t px[4]) {
  const uint8_t alpha = px[3];
  for (int c = 0; c < 3; ++c) {
    if (px[c] > alpha) px[c] = alpha;
  }
}

void sample_bicubic(const ImageBuffer& src, double sx, double sy, uint8_t out[4]) {
  const double fx = std::floor(sx);
  const double fy = std::floor(sy);
  const int32_t x0 = static_cast<int32_t>(fx);
  const int32_t y0 = static_cast<int32_t>(fy);
  double wx[4];
  double wy[4];
  catmull_rom_weights(sx - fx, wx);
  catmull_rom_weights(sy - fy, wy);
  double acc[4] = {0.0, 0.0, 0.0, 0.0};
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      const uint8_t* p = fetch(src, x0 - 1 + i, y0 - 1 + j);
      const double weight = wx[i] * wy[j];
      for (int c = 0; c < 4; ++c) acc[c] += static_cast<double>(p[c]) * weight;
    }
  }
  for (int c = 0; c < 4; ++c) {
    out[c] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, acc[c] + 0.5)));
  }
  enforce_premultiplied(out);
}

}  // namespace

SamplingMode sampling_from_manifest_name(const std::string& name) {
  if (name == "Nearest") return SamplingMode::kNearest;
  if (name == "Smooth") return SamplingMode::kSmooth;
  return SamplingMode::kHighQuality;
}

const char* sampling_manifest_name(SamplingMode mode) {
  switch (mode) {
    case SamplingMode::kNearest: return "Nearest";
    case SamplingMode::kSmooth: return "Smooth";
    case SamplingMode::kHighQuality:
    default: return "High quality";
  }
}

SamplingMode sampling_from_abi(comp_sampling value) {
  switch (value) {
    case COMP_SAMPLING_NEAREST: return SamplingMode::kNearest;
    case COMP_SAMPLING_SMOOTH: return SamplingMode::kSmooth;
    case COMP_SAMPLING_HIGH_QUALITY: return SamplingMode::kHighQuality;
    default: return SamplingMode::kHighQuality;
  }
}

comp_sampling sampling_to_abi(SamplingMode mode) {
  switch (mode) {
    case SamplingMode::kNearest: return COMP_SAMPLING_NEAREST;
    case SamplingMode::kSmooth: return COMP_SAMPLING_SMOOTH;
    case SamplingMode::kHighQuality: return COMP_SAMPLING_HIGH_QUALITY;
    default: return COMP_SAMPLING_HIGH_QUALITY;
  }
}

bool build_layer_unit(const Layer& layer, SamplingMode mode, ImageBuffer* out,
                      comp_rect* out_bounds, std::string* error, const comp_rect* clip) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out buffer is null";
    return false;
  }
  const comp_rect bounds = layer.effective_bounds();
  if (out_bounds != nullptr) *out_bounds = bounds;

  if (layer.pixels.empty() || is_empty(bounds)) {
    out->resize(0, 0, COMP_PIXEL_RGBA8);
    return true;
  }

  /*
   * 16 位图层按它的 8 位工作副本光栅化：上游同一处（DeepLayers.swift:4）
   * 「its `image` is a dithered 8-bit copy that the canvas, thumbnails and every
   * tool keep using unchanged」。少了这一步，采样内核会按 4 字节/像素读
   * 8 字节/像素的缓冲——不报错，画面是垃圾（AUDIT3 P2-13 的原观察）。
   */
  ImageBuffer rgba8_view;
  const ImageBuffer* pixels = &layer.pixels;
  if (layer.pixels.format != COMP_PIXEL_RGBA8) {
    /*
     * HDR（RGBA16F）走 hdr_to_rgba8 而不是 deep_to_rgba8：后者把缓冲按 16 位
     * 整数读，会把 half 的位模式当成数值（0x3C00 = 1.0 读成 15360），
     * 画面是噪声而不是「白点以上显示为白」。
     */
    const bool ok = layer.pixels.format == COMP_PIXEL_RGBA16F
                        ? hdr_to_rgba8(layer.pixels, &rgba8_view)
                        : deep_to_rgba8(layer.pixels, &rgba8_view);
    if (!ok) {
      if (error != nullptr) *error = "unsupported layer pixel format";
      return false;
    }
    pixels = &rgba8_view;
  }

  /*
   * 光栅窗口：默认整幅 bounds；传入 clip（合成路径给的是 clamp_to_canvas 后的
   * 可见矩形）时光栅化 **只覆盖裁剪后那一块**。
   *
   * 为什么必须裁剪：上游从不为一层实体化整幅光栅，而是按可见矩形分块绘制
   * （TiledLayerRenderer 分块 + LayerRenderer.reduced 缩到设备尺寸）。本引擎若
   * 整幅光栅，一个 transform 拉大到 200000×200000 的图层就会要 160 GB 中间缓冲，
   * 只能在 build_layer_unit 被尺寸闸拦下——于是「拉大图层」这个正常操作直接变成
   * 「整篇不可渲染」（AUDIT4 §2）。裁剪后中间缓冲以可见矩形为界，与上游同思路。
   *
   * 不变量：逆映射的分母始终用完整 bounds，窗口内每个像素算出的源坐标与
   * 「不裁剪时同一坐标处的像素」完全一致——因此裁剪只改变算多少像素，不改变
   * 任何像素值，恒等/缩放/旋转三条路径的逐字节不变性不受影响。
   */
  comp_rect window = bounds;
  if (clip != nullptr) {
    window = intersection(bounds, *clip);
    if (is_empty(window)) {
      out->resize(0, 0, COMP_PIXEL_RGBA8);
      return true;
    }
  }
  if (out_bounds != nullptr) *out_bounds = window;

  const int32_t sw = layer.pixels.width;
  const int32_t sh = layer.pixels.height;
  const int32_t ow = window.w;
  const int32_t oh = window.h;
  /*
   * 边界读单一真源（compositor::limits），不再本文件另写 30000 / 1e8。
   * 加闸后合成路径的窗口以画布为界（≤ 30000×30000 且交于本画布），
   * 这道闸实际拦的是「不传 clip 的整幅光栅」路径与不可信 manifest；
   * 它必须保留：绕过 ABI 直接构造的 Document 同样不受写入侧保护。
   * 内因写进 error —— 上游调用方据此报 ProjectError.tooLarge。
   */
  if (ow > compositor::limits::kMaxSide || oh > compositor::limits::kMaxSide ||
      static_cast<int64_t>(ow) * oh > compositor::limits::kMaxSurfacePixels) {
    if (error != nullptr) *error = "layer unit too large";
    return false;
  }

  out->resize(ow, oh, COMP_PIXEL_RGBA8);

  const bool rotate = std::fabs(layer.rotation) > 1e-9;
  const double rad = static_cast<double>(layer.rotation) * kPi / 180.0;
  const double cos_t = rotate ? std::cos(rad) : 1.0;
  const double sin_t = rotate ? std::sin(rad) : 0.0;

  const bool has_mask = !layer.mask.empty();
  const int32_t mw = layer.mask.width;
  const int32_t mh = layer.mask.height;

  for (int32_t y = 0; y < oh; ++y) {
    uint8_t* dst_row = out->writable_row(y);
    /* 归一化分母用完整 bounds（见上方不变量），窗口偏移只平移起点。 */
    const double doc_y = static_cast<double>(y + window.y - bounds.y);
    const double vd = (doc_y + 0.5) / bounds.h * 2.0 - 1.0;
    for (int32_t x = 0; x < ow; ++x) {
      const double doc_x = static_cast<double>(x + window.x - bounds.x);
      const double ud = (doc_x + 0.5) / bounds.w * 2.0 - 1.0;
      /* 逆旋转：顺时针 θ 的正向为 (u,v)->(u cosθ - v sinθ, u sinθ + v cosθ)。 */
      double us = rotate ? (ud * cos_t + vd * sin_t) : ud;
      double vs = rotate ? (-ud * sin_t + vd * cos_t) : vd;
      if (layer.flip_x) us = -us;
      if (layer.flip_y) vs = -vs;
      const double sx = (us + 1.0) * 0.5 * sw - 0.5;
      const double sy = (vs + 1.0) * 0.5 * sh - 0.5;

      uint8_t* dst_px = dst_row + static_cast<size_t>(x) * 4;
      if (!inside(sx, sy, sw, sh)) continue; /* 源外 -> 保持透明 */

      switch (mode) {
        case SamplingMode::kNearest: sample_nearest(*pixels, sx, sy, dst_px); break;
        case SamplingMode::kSmooth: sample_bilinear(*pixels, sx, sy, dst_px); break;
        case SamplingMode::kHighQuality:
        default: sample_bicubic(*pixels, sx, sy, dst_px); break;
      }

      if (has_mask) {
        const int32_t mx = clamp_index(static_cast<int32_t>(std::lround(sx)), mw);
        const int32_t my = clamp_index(static_cast<int32_t>(std::lround(sy)), mh);
        const double m = static_cast<double>(
                             layer.mask.pixel_data()[static_cast<size_t>(my) * mw + mx]) / 255.0;
        for (int c = 0; c < 4; ++c) {
          dst_px[c] = static_cast<uint8_t>(
              std::min(255.0, std::max(0.0, static_cast<double>(dst_px[c]) * m + 0.5)));
        }
      }
    }
  }
  return true;
}

bool resample_image(const ImageBuffer& src, int32_t dst_w, int32_t dst_h,
                    SamplingMode mode, ImageBuffer* out) {
  if (out == nullptr) return false;
  const int32_t sw = src.width;
  const int32_t sh = src.height;
  if (src.empty() || dst_w <= 0 || dst_h <= 0) return false;

  out->resize(dst_w, dst_h, COMP_PIXEL_RGBA8);
  for (int32_t y = 0; y < dst_h; ++y) {
    uint8_t* dst_row = out->writable_row(y);
    /* 与 build_layer_unit 同口径的像素中心逆映射；sw==dst_w 时退化为整数坐标。 */
    const double sy = (static_cast<double>(y) + 0.5) * sh / dst_h - 0.5;
    for (int32_t x = 0; x < dst_w; ++x) {
      const double sx = (static_cast<double>(x) + 0.5) * sw / dst_w - 0.5;
      uint8_t* dst_px = dst_row + static_cast<size_t>(x) * 4;
      switch (mode) {
        case SamplingMode::kNearest: sample_nearest(src, sx, sy, dst_px); break;
        case SamplingMode::kSmooth: sample_bilinear(src, sx, sy, dst_px); break;
        case SamplingMode::kHighQuality:
        default: sample_bicubic(src, sx, sy, dst_px); break;
      }
    }
  }
  return true;
}

namespace {

/* 灰度版取样：边缘夹取（整幅掩版缩放不引入透明）。 */
inline uint8_t gray_at(const uint8_t* src, int32_t sw, int32_t sh, int32_t x, int32_t y) {
  return src[static_cast<size_t>(clamp_index(y, sh)) * sw + clamp_index(x, sw)];
}

void gray_nearest(const uint8_t* src, int32_t sw, int32_t sh, double sx, double sy,
                  uint8_t* out) {
  *out = gray_at(src, sw, sh, static_cast<int32_t>(std::lround(sx)),
                 static_cast<int32_t>(std::lround(sy)));
}

void gray_bilinear(const uint8_t* src, int32_t sw, int32_t sh, double sx, double sy,
                   uint8_t* out) {
  const double fx = std::floor(sx);
  const double fy = std::floor(sy);
  const double tx = sx - fx;
  const double ty = sy - fy;
  const int32_t x0 = static_cast<int32_t>(fx);
  const int32_t y0 = static_cast<int32_t>(fy);
  const double p00 = gray_at(src, sw, sh, x0, y0);
  const double p10 = gray_at(src, sw, sh, x0 + 1, y0);
  const double p01 = gray_at(src, sw, sh, x0, y0 + 1);
  const double p11 = gray_at(src, sw, sh, x0 + 1, y0 + 1);
  const double top = p00 * (1.0 - tx) + p10 * tx;
  const double bottom = p01 * (1.0 - tx) + p11 * tx;
  *out = static_cast<uint8_t>(std::min(255.0, std::max(0.0, top * (1.0 - ty) + bottom * ty + 0.5)));
}

void gray_bicubic(const uint8_t* src, int32_t sw, int32_t sh, double sx, double sy,
                  uint8_t* out) {
  const double fx = std::floor(sx);
  const double fy = std::floor(sy);
  const int32_t x0 = static_cast<int32_t>(fx);
  const int32_t y0 = static_cast<int32_t>(fy);
  double wx[4];
  double wy[4];
  catmull_rom_weights(sx - fx, wx);
  catmull_rom_weights(sy - fy, wy);
  double acc = 0.0;
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      acc += static_cast<double>(gray_at(src, sw, sh, x0 - 1 + i, y0 - 1 + j)) * wx[i] * wy[j];
    }
  }
  *out = static_cast<uint8_t>(std::min(255.0, std::max(0.0, acc + 0.5)));
}

}  // namespace

bool resample_gray(const uint8_t* src, int32_t src_w, int32_t src_h,
                   int32_t dst_w, int32_t dst_h, SamplingMode mode,
                   std::vector<uint8_t>* out) {
  if (out == nullptr || src == nullptr) return false;
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return false;

  out->assign(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h), 0u);
  for (int32_t y = 0; y < dst_h; ++y) {
    const double sy = (static_cast<double>(y) + 0.5) * src_h / dst_h - 0.5;
    uint8_t* dst_row = out->data() + static_cast<size_t>(y) * dst_w;
    for (int32_t x = 0; x < dst_w; ++x) {
      const double sx = (static_cast<double>(x) + 0.5) * src_w / dst_w - 0.5;
      switch (mode) {
        case SamplingMode::kNearest:
          gray_nearest(src, src_w, src_h, sx, sy, &dst_row[x]);
          break;
        case SamplingMode::kSmooth:
          gray_bilinear(src, src_w, src_h, sx, sy, &dst_row[x]);
          break;
        case SamplingMode::kHighQuality:
        default:
          gray_bicubic(src, src_w, src_h, sx, sy, &dst_row[x]);
          break;
      }
    }
  }
  return true;
}

}  // namespace compositor
