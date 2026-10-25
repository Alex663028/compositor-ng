// AI生成
/*
 * filters.cpp — Filter 菜单领域层实现（见 filters.h 的文件头）。
 *
 * 像素内核全部复用 engine/imaging：
 *   Vignette        AdjustPixels.h  adjust_colored_vignette
 *   Tonal Contrast  adjustment_eval.h blur_rgba8_copy + AdjustPixels.h adjust_tonal_contrast
 *   Bloom           本文件（Upstream 是 Core Image CIBloom，内部不公开；见 bloom_pixels）
 *   Lens Correction LensPixels.h    lens_distort
 *   Gaussian Blur   adjustment_eval.h blur_rgba8_copy（与 kind 7 调整层同一实现）
 *   Add Noise       NoisePixels.h   noise_add_at
 *   Grain           AdjustPixels.h  adjust_grain（与 kind 5 调整层同一内核）
 *   Motion Blur     本文件（上游是 Core Image CIMotionBlur，见 motion_blur_pixels）
 *   Progressive Blur 本文件（上游是 Core Image CIMaskedVariableBlur，见
 *                   progressive_blur_pixels：分档三角混合逼近逐像素半径）
 *   Sharpen         adjustment_eval.h blur_rgba8_copy + 本文件 sharpen_pixels（USM）
 *   Reduce Noise    本文件 reduce_noise_pixels（YCoCg 双边 + 色度平滑）
 *
 * 本文件只做「校验 -> 装配 -> 调用 -> 选区混合 -> 事务」，不重新定义内核语义。
 */
#include "domain/filters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "compositing/adjustment_eval.h"
#include "domain/document.h"
#include "domain/selection.h"
#include "imaging/comp_imaging.h"

namespace compositor {
namespace {

/* 上游 Filters.swift:238：Remove Distortion ±100 把角点移动半对角线距离的这个比例。 */
constexpr double kLensStrength = 0.35;

/*
 * Bloom 的亮部阈值。CIBloom 不公开这个参数；取 0.5 与 Photoshop 的
 * 「Glow 只吃高光」口径一致，并在文档里显式标注为近似。
 */
constexpr double kBloomThreshold = 0.5;

bool finite(double value) { return std::isfinite(value); }

bool in_range(double value, double low, double high) {
  return finite(value) && value >= low && value <= high;
}

double clamp01(double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

/* rec.709 亮度（与 AdjustPixels.c 的 rec709 同系数，避免两处口径漂移）。 */
double luminance(double r, double g, double b) {
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

bool set_error(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
  return false;
}

/* Progressive Blur 的渐变轴，已经投到图层像素坐标（连续、可越界）。 */
struct ProgressiveLine {
  bool valid = false;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

/* ------------------------------------------------------------ 参数闸 */

bool vignette_valid(const VignetteSettings& s, std::string* error) {
  if (!in_range(s.amount, 0.0, 100.0)) return set_error(error, "vignette amount out of 0..100");
  if (!in_range(s.midpoint, 0.0, 100.0)) return set_error(error, "vignette midpoint out of 0..100");
  if (!in_range(s.roundness, -100.0, 100.0)) {
    return set_error(error, "vignette roundness out of -100..100");
  }
  if (!in_range(s.feather, 0.0, 100.0)) return set_error(error, "vignette feather out of 0..100");
  if (!in_range(s.highlights, 0.0, 100.0)) {
    return set_error(error, "vignette highlights out of 0..100");
  }
  for (int c = 0; c < 3; ++c) {
    if (!in_range(s.color[c], 0.0, 1.0)) {
      return set_error(error, "vignette color channel out of 0..1");
    }
  }
  return true;
}

bool tonal_valid(const TonalContrastSettings& s, std::string* error) {
  if (!in_range(s.amount, 0.0, 100.0)) {
    return set_error(error, "tonal contrast amount out of 0..100");
  }
  if (!in_range(s.radius, 1.0, 100.0)) {
    return set_error(error, "tonal contrast radius out of 1..100");
  }
  if (!in_range(s.shadows, -100.0, 100.0)) {
    return set_error(error, "tonal contrast shadows out of -100..100");
  }
  if (!in_range(s.midtones, -100.0, 100.0)) {
    return set_error(error, "tonal contrast midtones out of -100..100");
  }
  if (!in_range(s.highlights, -100.0, 100.0)) {
    return set_error(error, "tonal contrast highlights out of -100..100");
  }
  return true;
}

bool bloom_valid(const BloomSettings& s, std::string* error) {
  if (!in_range(s.amount, 0.0, 100.0)) return set_error(error, "bloom amount out of 0..100");
  if (!in_range(s.radius, 1.0, 150.0)) return set_error(error, "bloom radius out of 1..150");
  return true;
}

bool lens_valid(const LensCorrectionSettings& s, std::string* error) {
  if (!in_range(s.distortion, -100.0, 100.0)) {
    return set_error(error, "lens correction distortion out of -100..100");
  }
  return true;
}

bool gaussian_blur_valid(const GaussianBlurSettings& s, std::string* error) {
  /* 0 = 恒等；>0 时按上游量程 0.1..250。中间那段 (0, 0.1) 显式拒绝。 */
  if (s.radius != 0.0 && !in_range(s.radius, 0.1, 250.0)) {
    return set_error(error, "gaussian blur radius must be 0 or in 0.1..250");
  }
  return true;
}

bool add_noise_valid(const AddNoiseSettings& s, std::string* error) {
  /* 0 = 恒等；>0 时按上游量程 0.1..400。 */
  if (s.amount != 0.0 && !in_range(s.amount, 0.1, 400.0)) {
    return set_error(error, "add noise amount must be 0 or in 0.1..400");
  }
  return true;
}

bool grain_valid(const GrainSettings& s, std::string* error) {
  if (!in_range(s.amount, 0.0, 100.0)) return set_error(error, "grain amount out of 0..100");
  if (!in_range(s.size, 0.5, 20.0)) return set_error(error, "grain size out of 0.5..20");
  if (!in_range(s.roughness, 0.0, 100.0)) {
    return set_error(error, "grain roughness out of 0..100");
  }
  return true;
}

bool motion_blur_valid(const MotionBlurSettings& s, std::string* error) {
  if (!in_range(s.angle, -90.0, 90.0)) {
    return set_error(error, "motion blur angle out of -90..90");
  }
  /* 0 = 恒等；>0 时按上游量程 1..2000。中间那段 (0, 1) 显式拒绝。 */
  if (s.distance != 0.0 && !in_range(s.distance, 1.0, 2000.0)) {
    return set_error(error, "motion blur distance must be 0 or in 1..2000");
  }
  return true;
}

bool progressive_blur_valid(const ProgressiveBlurSettings& s, std::string* error) {
  if (!in_range(s.from, 0.0, 250.0)) {
    return set_error(error, "progressive blur from radius out of 0..250");
  }
  if (!in_range(s.to, 0.0, 250.0)) {
    return set_error(error, "progressive blur to radius out of 0..250");
  }
  /* 显式轴的端点必须有限；落在画布/图层外是合法的（渐变轴常常横跨整幅画布）。 */
  if (!finite(s.start_x) || !finite(s.start_y) || !finite(s.end_x) || !finite(s.end_y)) {
    return set_error(error, "progressive blur axis endpoint is not finite");
  }
  return true;
}

bool sharpen_valid(const SharpenSettings& s, std::string* error) {
  /* 0 = 恒等；>0 时按上游量程 0.1..500。中间那段 (0, 0.1) 显式拒绝。 */
  if (s.amount != 0.0 && !in_range(s.amount, 0.1, 500.0)) {
    return set_error(error, "sharpen amount must be 0 or in 0.1..500");
  }
  if (!in_range(s.radius, 0.1, 250.0)) {
    return set_error(error, "sharpen radius out of 0.1..250");
  }
  if (!in_range(s.threshold, 0.0, 255.0)) {
    return set_error(error, "sharpen threshold out of 0..255");
  }
  return true;
}

bool reduce_noise_valid(const ReduceNoiseSettings& s, std::string* error) {
  /* 0 = 恒等；>0 时按上游量程 0.1..10。 */
  if (s.strength != 0.0 && !in_range(s.strength, 0.1, 10.0)) {
    return set_error(error, "reduce noise strength must be 0 or in 0.1..10");
  }
  if (!in_range(s.preserve_details, 0.0, 100.0)) {
    return set_error(error, "reduce noise preserve details out of 0..100");
  }
  if (!in_range(s.reduce_color_noise, 0.0, 100.0)) {
    return set_error(error, "reduce noise color noise out of 0..100");
  }
  return true;
}

/* ------------------------------------------------------------ 像素算子 */

/* Vignette：帧 = 图层自身像素矩形（上游非空图层 `job.canvas == nil` 的分支）。
 * 本仓图层是 top-down 行序，frameY 直接取 0；内核的椭圆对垂直翻转不敏感，
 * 但 top-down 与 bottom-up 的差异只在这一处，写清楚免得以后误改。 */
void vignette_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                     const VignetteSettings& s) {
  ::adjust_colored_vignette(rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
                           0.0, 0.0, static_cast<double>(width), static_cast<double>(height),
                           0 /* fillsClear：图层已有像素，只改色不铺底 */, s.amount, s.midpoint,
                           s.roundness, s.feather, s.highlights, s.color[0], s.color[1],
                           s.color[2]);
}

/* Tonal Contrast：先按 detail 半径做高斯模糊得到 base，再逐像素比较细节。
 * 高斯走共享的 blur_rgba8_copy（与 Gaussian Blur 调整层、扫描线辉光同一实现）。 */
comp_result tonal_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const TonalContrastSettings& s, double scale) {
  const double sigma = std::max(0.1, s.radius * scale);
  std::vector<uint8_t> blurred;
  if (!blur_rgba8_copy(rgba, width, height, stride, static_cast<float>(sigma), &blurred)) {
    return COMP_ERR_OUT_OF_MEMORY;
  }
  const size_t blurred_stride = static_cast<size_t>(width) * 4u;
  ::adjust_tonal_contrast(rgba, blurred.data(), static_cast<size_t>(width),
                          static_cast<size_t>(height), stride, blurred_stride, s.amount, s.shadows,
                          s.midtones, s.highlights);
  return COMP_OK;
}

/*
 * Bloom / Glow 的显式近似（CIBloom 内部不公开）：
 *
 *   1. 亮部提取：按 rec.709 亮度在 [0.5, 1] 线性起权，得到 highlight ∈ [0,1]；
 *   2. 预乘亮部副本（RGB 与 alpha 同乘 highlight，保持预乘不变式）；
 *   3. 高斯模糊（sigma = radius * scale，复用 blur_rgba8_copy）；
 *   4. 加光：rgb += glow * (amount / 50)（上游 intensity = bloomAmount / 50），
 *      再裁到该像素自己的 alpha —— **不改 alpha**，图层轮廓与合成几何不变。
 *
 * 与上游的差异（写进 note，不静默）：CIBloom 的具体阈值/混合曲线不可见，
 * 本仓只保证「亮部向外扩散、强度按 amount/50、半径按 radius」这三条可观察口径。
 */
comp_result bloom_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const BloomSettings& s, double scale) {
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  const size_t rows = static_cast<size_t>(height);
  std::vector<uint8_t> glow(row_bytes * rows, 0);
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + static_cast<size_t>(y) * stride;
    uint8_t* dst = glow.data() + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* p = src + static_cast<size_t>(x) * 4;
      if (p[3] == 0) continue;
      const double alpha = p[3] / 255.0;
      const double r = std::min(1.0, p[0] / (alpha * 255.0));
      const double g = std::min(1.0, p[1] / (alpha * 255.0));
      const double b = std::min(1.0, p[2] / (alpha * 255.0));
      const double highlight = clamp01((luminance(r, g, b) - kBloomThreshold) /
                                       (1.0 - kBloomThreshold));
      if (highlight <= 0.0) continue;
      uint8_t* q = dst + static_cast<size_t>(x) * 4;
      for (int c = 0; c < 3; ++c) {
        q[c] = static_cast<uint8_t>(std::lround(p[c] * highlight));
      }
      q[3] = static_cast<uint8_t>(std::lround(p[3] * highlight));
    }
  }
  const double sigma = std::max(0.1, s.radius * scale);
  std::vector<uint8_t> blurred;
  if (!blur_rgba8_copy(glow.data(), width, height, row_bytes, static_cast<float>(sigma),
                       &blurred)) {
    return COMP_ERR_OUT_OF_MEMORY;
  }
  const double gain = s.amount / 50.0;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* lit = blurred.data() + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      if (p[3] == 0) continue;
      const uint8_t* l = lit + static_cast<size_t>(x) * 4;
      for (int c = 0; c < 3; ++c) {
        const double added = l[c] * gain;
        const double value = clamp01((p[c] + added) / 255.0) * 255.0;
        /* 预乘不变式：颜色不得超过自身的 alpha（透明像素不会凭空发光）。 */
        p[c] = static_cast<uint8_t>(std::lround(std::min(value, static_cast<double>(p[3]))));
      }
    }
  }
  return COMP_OK;
}

comp_result lens_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                        const LensCorrectionSettings& s) {
  const double k = s.distortion / 100.0 * kLensStrength;
  if (k == 0.0) return COMP_OK;
  std::vector<uint8_t> source(static_cast<size_t>(width) * 4u * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
                rgba + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
  }
  ::lens_distort(source.data(), rgba, static_cast<size_t>(width), static_cast<size_t>(height),
                 stride, k);
  return COMP_OK;
}

/*
 * Gaussian Blur：与 kind 7 调整层共用 blur_rgba8_copy（sigma 以图层像素计，
 * 预览时乘 scale——与 Tonal Contrast 的 detail 半径同一口径）。
 *
 * preserve_transparency 的等价做法：模糊后的 alpha 是周围像素的加权和，
 * 用 ` blurred_rgb * 255 / blurred_alpha` 把它反预乘回颜色，再乘**原** alpha。
 * 这样轮廓不向外扩散（alpha 不动），也不会因为透明邻域把颜色拉暗。
 * 模糊后 alpha 为 0 的像素没有颜色可取，按预乘口径写 0。
 */
comp_result gaussian_blur_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                                 const GaussianBlurSettings& s, double scale) {
  if (s.radius <= 0.0) return COMP_OK;
  const double sigma = std::max(0.1, s.radius * scale);
  std::vector<uint8_t> blurred;
  if (!blur_rgba8_copy(rgba, width, height, stride, static_cast<float>(sigma), &blurred)) {
    return COMP_ERR_OUT_OF_MEMORY;
  }
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  if (!s.preserve_transparency) {
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(rgba + static_cast<size_t>(y) * stride,
                  blurred.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
    }
    return COMP_OK;
  }
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* source = blurred.data() + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
      const uint8_t* sample = source + static_cast<size_t>(x) * 4u;
      const uint8_t alpha = pixel[3];
      if (alpha == 0 || sample[3] == 0) {
        pixel[0] = pixel[1] = pixel[2] = 0;
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        unsigned colour = (static_cast<unsigned>(sample[c]) * 255u + sample[3] / 2u) /
                          static_cast<unsigned>(sample[3]);
        if (colour > 255u) colour = 255u;
        pixel[c] = static_cast<uint8_t>((colour * static_cast<unsigned>(alpha) + 127u) / 255u);
      }
    }
  }
  return COMP_OK;
}

/* Add Noise：噪声场按文档坐标锚定（origin = 图层 effective_bounds 原点）。 */
comp_result add_noise_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                             const AddNoiseSettings& s, int64_t origin_x, int64_t origin_y) {
  ::noise_add_at(rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
                 static_cast<float>(s.amount), s.gaussian ? 1 : 0, s.monochromatic ? 1 : 0, s.seed,
                 origin_x, origin_y);
  return COMP_OK;
}

/* Grain：与 kind 5 调整层同一内核；采样在图层像素空间（origin 0）。 */
comp_result grain_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const GrainSettings& s, double scale) {
  const double units_per_pixel = 1.0 / (scale > 0.0 ? scale : 1.0);
  ::adjust_grain(rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride, s.amount,
                 s.size, s.roughness, s.seed, 0.0, 0.0, units_per_pixel);
  return COMP_OK;
}

/*
 * Motion Blur：沿 angle 方向的**盒式线积分**（distance = 条纹全长，图层像素，
 * 预览再乘 scale）。上游把 `distance / sqrt(12)` 传给 CIMotionBlur 的 radius，
 * 让 Core Image 的分布与长度 distance 的均匀条纹同方差；本仓直接对长度
 * distance 做平均，方向与长度两条可观察口径一致，形状是盒式而不是 CI 的近似
 * 高斯，note 如实注明。
 *
 * 采样在**预乘**像素上做双线性平均（含 alpha：条纹会向外扩散，与上游不保留
 * 透明的 motionBlur 一致）；越界坐标夹到边缘（等效上游
 * `extending(...past: canvasSides)` 的边缘延展）。每像素最多 257 个采样点，
 * 更长的条纹按步长重采样。
 */
comp_result motion_blur_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                               const MotionBlurSettings& s, double scale) {
  if (width <= 0 || height <= 0) return COMP_OK;
  const double length = s.distance * scale;
  if (!(length >= 0.5)) return COMP_OK; /* 亚像素条纹：不改动像素 */
  const double radians = s.angle * 3.14159265358979323846 / 180.0;
  /* 屏幕坐标 y 向下：Photoshop 的逆时针正角在这里 dy 取负。 */
  const double dx = std::cos(radians);
  const double dy = -std::sin(radians);

  int taps = static_cast<int>(std::ceil(length)) + 1;
  if (taps < 2) taps = 2;
  if (taps > 257) taps = 257;
  const double first = -length / 2.0;
  const double step = length / static_cast<double>(taps - 1);

  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }

  const double last_x = static_cast<double>(width - 1);
  const double last_y = static_cast<double>(height - 1);
  auto fetch = [&](double x, double y, double out[4]) {
    x = std::min(std::max(x, 0.0), last_x);
    y = std::min(std::max(y, 0.0), last_y);
    const int32_t x0 = static_cast<int32_t>(x);
    const int32_t y0 = static_cast<int32_t>(y);
    const int32_t x1 = x0 < width - 1 ? x0 + 1 : x0;
    const int32_t y1 = y0 < height - 1 ? y0 + 1 : y0;
    const double fx = x - static_cast<double>(x0);
    const double fy = y - static_cast<double>(y0);
    const uint8_t* row0 = source.data() + static_cast<size_t>(y0) * row_bytes;
    const uint8_t* row1 = source.data() + static_cast<size_t>(y1) * row_bytes;
    const uint8_t* p00 = row0 + static_cast<size_t>(x0) * 4u;
    const uint8_t* p10 = row0 + static_cast<size_t>(x1) * 4u;
    const uint8_t* p01 = row1 + static_cast<size_t>(x0) * 4u;
    const uint8_t* p11 = row1 + static_cast<size_t>(x1) * 4u;
    for (int c = 0; c < 4; ++c) {
      const double top = p00[c] + (p10[c] - p00[c]) * fx;
      const double bottom = p01[c] + (p11[c] - p01[c]) * fx;
      out[c] = top + (bottom - top) * fy;
    }
  };

  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      double sum[4] = {0.0, 0.0, 0.0, 0.0};
      for (int t = 0; t < taps; ++t) {
        const double offset = first + step * static_cast<double>(t);
        double sample[4];
        fetch(static_cast<double>(x) + dx * offset, static_cast<double>(y) + dy * offset, sample);
        for (int c = 0; c < 4; ++c) sum[c] += sample[c];
      }
      uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
      for (int c = 0; c < 4; ++c) {
        long value = std::lround(sum[c] / static_cast<double>(taps));
        pixel[c] = static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
      }
    }
  }
  return COMP_OK;
}

/*
 * Progressive Blur：半径沿一条轴渐变的模糊（上游 `CIMaskedVariableBlur` +
 * `CILinearGradient` / `CIRadialGradient` 遮罩）。
 *
 * 上游的遮罩按位置给出**半径比例**（mask = radius / strongest），
 * `inputRadius = strongest * scale` 是上限，Core Image 再逐像素用较小的半径
 * 模糊。本仓共享内核 `blur_rgba8_copy` 只接受单一 sigma，因此用**分档三角混合**
 * 逼近：
 *
 *   1. 把半径区间等分成 kProgressiveLevels 档，逐档跑一次均匀模糊；
 *   2. 每个像素按它在轴上的位置 t ∈ [0,1] 得到档位坐标 t * Levels，
 *      在相邻两档之间取三角权重（权重和恒为 1）；
 *   3. 把各档结果按权重累加进 float 缓冲，最后四舍五入回 8 位。
 *
 * 因此「轴向均匀」（from == to）时只有一档有权重，结果与单一 sigma 的
 * `blur_rgba8_copy` **逐字节相同**（权重恰为 1.0f）；渐变轴上半径分段线性。
 * 与上游的差异（note 如实写明）：CIMaskedVariableBlur 的半径逐像素连续变化，
 * 本仓是 8 档混合，过渡带内不声称逐像素等同。
 *
 * 轴向：`line` 已由 filter_apply 投到图层像素坐标。线性档取「点在轴上的投影 /
 * 轴长」，径向档取「点与 start 的距离 / 轴长」，都夹到 0..1。轴退化
 * （距离 < 1 px）时终点取 start + (0, 1)：本仓像素 y 向下，与上游在 CI 里
 * 取 start + (0, -1)（y 向上）是同一方向。
 */
constexpr int kProgressiveLevels = 8;

comp_result progressive_blur_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                                    const ProgressiveBlurSettings& s, double scale,
                                    const ProgressiveLine& line) {
  if (width <= 0 || height <= 0) return COMP_OK;
  if (!line.valid) return COMP_OK;
  double from = s.from;
  double to = s.to;
  if (s.inverted) std::swap(from, to);
  const double strongest = std::max(from, to);
  /* 上游 guard strongest > 0（Filters.swift:399）：两端都为 0 时原样返回。 */
  if (!(strongest > 0.0)) return COMP_OK;

  double sx = line.x0;
  double sy = line.y0;
  double ex = line.x1;
  double ey = line.y1;
  double dx = ex - sx;
  double dy = ey - sy;
  double length = std::hypot(dx, dy);
  if (length < 1.0) {
    ex = sx;
    ey = sy + 1.0;
    dx = 0.0;
    dy = 1.0;
    length = 1.0;
  }
  const double inv_length = 1.0 / length;
  const double inv_length2 = inv_length * inv_length;

  const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t row_bytes = static_cast<size_t>(width) * 4u;

  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }

  /* 每像素的档位坐标（0..kProgressiveLevels），只算一次。 */
  std::vector<float> position(pixel_count);
  for (int32_t y = 0; y < height; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int32_t x = 0; x < width; ++x) {
      const double px = static_cast<double>(x) - sx;
      const double py = static_cast<double>(y) - sy;
      const double t = s.radial ? std::hypot(px, py) * inv_length
                                : (px * dx + py * dy) * inv_length2;
      position[row + static_cast<size_t>(x)] =
          static_cast<float>(clamp01(t) * static_cast<double>(kProgressiveLevels));
    }
  }

  std::vector<float> accumulated(pixel_count * 4u, 0.0f);
  std::vector<uint8_t> blurred;
  for (int level = 0; level <= kProgressiveLevels; ++level) {
    const uint8_t* image = source.data();
    const double t = static_cast<double>(level) / static_cast<double>(kProgressiveLevels);
    const double radius = from + (to - from) * t;
    /* level 0 是轴起点的半径，不是「原图」：from > 0 时它同样要模糊，
     * 否则 from == to 的均匀档会在起点留一条不模糊的边，与「整层同一半径」
     * 的上游口径不符。from == 0 才用原图（半径 0 = 不动是 0.1 px 做不到的）。 */
    if (radius > 0.0) {
      const double sigma = std::max(0.1, radius * scale);
      if (!blur_rgba8_copy(source.data(), width, height, row_bytes, static_cast<float>(sigma),
                           &blurred)) {
        return COMP_ERR_OUT_OF_MEMORY;
      }
      image = blurred.data();
    }
    const float level_value = static_cast<float>(level);
    for (size_t i = 0; i < pixel_count; ++i) {
      const float weight = 1.0f - std::fabs(position[i] - level_value);
      if (weight <= 0.0f) continue;
      const uint8_t* p = image + i * 4u;
      float* a = accumulated.data() + i * 4u;
      a[0] += weight * static_cast<float>(p[0]);
      a[1] += weight * static_cast<float>(p[1]);
      a[2] += weight * static_cast<float>(p[2]);
      a[3] += weight * static_cast<float>(p[3]);
    }
  }

  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const size_t base = static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int32_t x = 0; x < width; ++x) {
      const float* a = accumulated.data() + (base + static_cast<size_t>(x)) * 4u;
      uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
      for (int c = 0; c < 4; ++c) {
        const long value = std::lround(a[c]);
        pixel[c] = static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
      }
    }
  }
  return COMP_OK;
}

/*
 * Sharpen / Unsharp Mask：`src + amount/100 * (src - blur(src))`。
 *
 * 与 Tonal Contrast / Gaussian Blur 共用 blur_rgba8_copy（sigma = radius * scale），
 * 不另起一套高斯。运算落在预乘像素上、逐通道夹到 [0, alpha]：
 *   - alpha 不变，图层轮廓不会因为锐化向外扩张；
 *   - 半透明像素的颜色不会越过自身 coverage（预乘不变式）。
 * threshold 按**逐通道**差值判：|src - blur| <= threshold 的通道保持原值，
 * 用来避免把平坦区的噪点一起放大（与 Photoshop 的 Threshold 同一用途）。
 */
comp_result sharpen_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                           const SharpenSettings& s, double scale) {
  if (s.amount <= 0.0) return COMP_OK;
  const double sigma = std::max(0.1, s.radius * scale);
  std::vector<uint8_t> blurred;
  if (!blur_rgba8_copy(rgba, width, height, stride, static_cast<float>(sigma), &blurred)) {
    return COMP_ERR_OUT_OF_MEMORY;
  }
  const size_t blurred_stride = static_cast<size_t>(width) * 4u;
  const double gain = s.amount / 100.0;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* soft = blurred.data() + static_cast<size_t>(y) * blurred_stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      const double alpha = p[3];
      if (alpha == 0.0) continue;
      const uint8_t* b = soft + static_cast<size_t>(x) * 4;
      for (int c = 0; c < 3; ++c) {
        const double diff = static_cast<double>(p[c]) - static_cast<double>(b[c]);
        if (std::fabs(diff) <= s.threshold) continue;
        const double value = static_cast<double>(p[c]) + gain * diff;
        const double clamped = value < 0.0 ? 0.0 : (value > alpha ? alpha : value);
        p[c] = static_cast<uint8_t>(std::lround(clamped));
      }
    }
  }
  return COMP_OK;
}

/* YCoCg：亮度与两个色度轴。用它做降噪的理由是「亮度边沿保留、色噪单独平滑」
 * 天然正交——RGB 逐通道双边在强色边两侧会把分量拉成灰边。 */
struct YCoCg {
  double y;
  double co;
  double cg;
};

YCoCg ycocg_of(const uint8_t* p) {
  const double r = static_cast<double>(p[0]);
  const double g = static_cast<double>(p[1]);
  const double b = static_cast<double>(p[2]);
  return YCoCg{(r + 2.0 * g + b) * 0.25, r - b, g - 0.5 * (r + b)};
}

/*
 * Reduce Noise：5×5 双边滤波（亮度）+ 色度均值混合。
 *
 * 输入是预乘 RGBA8，先落到**直通**颜色副本（transparent 像素不参与累加，
 * 否则边缘会向透明区发黑），每像素：
 *   1. 亮度 Y 按空间权重 × 亮度差权重累加（差越大权重越小 → 边沿保留）；
 *   2. 色度 Co/Cg 用同一组权重取均值，再按 reduce_color_noise 混回；
 *   3. 用 YCoCg 逆变换重建 RGB 并夹到 0..255，最后按**原** alpha 预乘写回。
 *
 * 值域 sigma：strength / 10 越大、preserve_details 越低，平滑越强
 * （`2 + strength01 * (1 - preserve) * 60`，单位是 0..255 的亮度差）。
 * alpha 逐字节不变；strength = 0 时在入口短路（恒等）。
 */
comp_result reduce_noise_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                                const ReduceNoiseSettings& s) {
  if (s.strength <= 0.0 || width <= 0 || height <= 0) return COMP_OK;

  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> straight(row_bytes * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + static_cast<size_t>(y) * stride;
    uint8_t* dst = straight.data() + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* p = src + static_cast<size_t>(x) * 4;
      uint8_t* q = dst + static_cast<size_t>(x) * 4;
      q[3] = p[3];
      if (p[3] == 0) continue;
      const double alpha = p[3] / 255.0;
      for (int c = 0; c < 3; ++c) {
        const double value = std::min(255.0, static_cast<double>(p[c]) / alpha);
        q[c] = static_cast<uint8_t>(std::lround(value));
      }
    }
  }

  constexpr int kRadius = 2;
  constexpr double kSpatialSigma = 1.2;
  constexpr size_t kWindow = static_cast<size_t>(2 * kRadius + 1);
  double spatial[kWindow][kWindow];
  for (int dy = -kRadius; dy <= kRadius; ++dy) {
    for (int dx = -kRadius; dx <= kRadius; ++dx) {
      const double distance = static_cast<double>(dx * dx + dy * dy);
      spatial[dy + kRadius][dx + kRadius] =
          std::exp(-distance / (2.0 * kSpatialSigma * kSpatialSigma));
    }
  }

  const double strength01 = s.strength / 10.0;
  const double preserve01 = s.preserve_details / 100.0;
  const double range_sigma = 2.0 + strength01 * (1.0 - preserve01) * 60.0;
  const double range_scale = 1.0 / (2.0 * range_sigma * range_sigma);
  const double color_weight = s.reduce_color_noise / 100.0;

  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* center_row = straight.data() + static_cast<size_t>(y) * row_bytes;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* p = center_row + static_cast<size_t>(x) * 4;
      if (p[3] == 0) continue;
      const YCoCg center = ycocg_of(p);
      double weight_sum = 0.0;
      double y_sum = 0.0;
      double co_sum = 0.0;
      double cg_sum = 0.0;
      for (int dy = -kRadius; dy <= kRadius; ++dy) {
        int ny = y + dy;
        if (ny < 0) ny = 0;
        if (ny >= height) ny = height - 1;
        const uint8_t* neighbor_row = straight.data() + static_cast<size_t>(ny) * row_bytes;
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
          int nx = x + dx;
          if (nx < 0) nx = 0;
          if (nx >= width) nx = width - 1;
          const uint8_t* q = neighbor_row + static_cast<size_t>(nx) * 4;
          if (q[3] == 0) continue;
          const YCoCg neighbor = ycocg_of(q);
          const double delta = neighbor.y - center.y;
          const double weight =
              spatial[dy + kRadius][dx + kRadius] * std::exp(-delta * delta * range_scale);
          weight_sum += weight;
          y_sum += weight * neighbor.y;
          co_sum += weight * neighbor.co;
          cg_sum += weight * neighbor.cg;
        }
      }
      if (weight_sum <= 0.0) continue;
      const double mean_y = y_sum / weight_sum;
      const double mean_co = co_sum / weight_sum;
      const double mean_cg = cg_sum / weight_sum;
      /* 亮度走双边结果（边沿保留）；色度按 reduce_color_noise 向窗口均值混合。 */
      const double co = center.co + color_weight * (mean_co - center.co);
      const double cg = center.cg + color_weight * (mean_cg - center.cg);
      const double r = mean_y + (co - cg) * 0.5;
      const double b = mean_y - (co + cg) * 0.5;
      const double g = mean_y + cg * 0.5;
      const double alpha = static_cast<double>(p[3]);
      const double channels[3] = {r, g, b};
      for (int c = 0; c < 3; ++c) {
        const double clamped = channels[c] < 0.0 ? 0.0 : (channels[c] > 255.0 ? 255.0 : channels[c]);
        const double premultiplied = std::lround(clamped * alpha / 255.0);
        row[static_cast<size_t>(x) * 4u + static_cast<size_t>(c)] =
            static_cast<uint8_t>(premultiplied);
      }
    }
  }
  return COMP_OK;
}

comp_result apply_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const FilterSettings& settings, double scale, int64_t origin_x,
                         int64_t origin_y, const ProgressiveLine& line) {
  switch (settings.kind) {
    case kFilterVignette:
      vignette_pixels(rgba, width, height, stride, settings.vignette);
      return COMP_OK;
    case kFilterTonalContrast:
      return tonal_pixels(rgba, width, height, stride, settings.tonal_contrast, scale);
    case kFilterBloom:
      return bloom_pixels(rgba, width, height, stride, settings.bloom, scale);
    case kFilterLensCorrection:
      return lens_pixels(rgba, width, height, stride, settings.lens_correction);
    case kFilterGaussianBlur:
      return gaussian_blur_pixels(rgba, width, height, stride, settings.gaussian_blur, scale);
    case kFilterAddNoise:
      return add_noise_pixels(rgba, width, height, stride, settings.add_noise, origin_x, origin_y);
    case kFilterGrain:
      return grain_pixels(rgba, width, height, stride, settings.grain, scale);
    case kFilterMotionBlur:
      return motion_blur_pixels(rgba, width, height, stride, settings.motion_blur, scale);
    case kFilterProgressiveBlur:
      return progressive_blur_pixels(rgba, width, height, stride, settings.progressive_blur, scale,
                                     line);
    case kFilterSharpen:
      return sharpen_pixels(rgba, width, height, stride, settings.sharpen, scale);
    case kFilterReduceNoise:
      return reduce_noise_pixels(rgba, width, height, stride, settings.reduce_noise);
  }
  return COMP_ERR_INVALID_ARGUMENT;
}

/*
 * 把一次像素操作的结果按选区与原像素混合（上游 PixelFilter.run 末尾的
 * PixelAdjust.blend；S25 起也服务 invert_layer，故从 filters.cpp 提取共享）。
 * coverage 是**文档分辨率**，图层像素按 effective_bounds() 的原点平移到
 * 文档坐标（与合成/工具同款约定）。图层落在画布外的部分 coverage 视为 0
 * （选区之外），保持原像素。
 */

}  // namespace

/* ============================================================ 公共接口 */

bool filter_settings_valid(const FilterSettings& settings, std::string* error) {
  switch (settings.kind) {
    case kFilterVignette: return vignette_valid(settings.vignette, error);
    case kFilterTonalContrast: return tonal_valid(settings.tonal_contrast, error);
    case kFilterBloom: return bloom_valid(settings.bloom, error);
    case kFilterLensCorrection: return lens_valid(settings.lens_correction, error);
    case kFilterGaussianBlur: return gaussian_blur_valid(settings.gaussian_blur, error);
    case kFilterAddNoise: return add_noise_valid(settings.add_noise, error);
    case kFilterGrain: return grain_valid(settings.grain, error);
    case kFilterMotionBlur: return motion_blur_valid(settings.motion_blur, error);
    case kFilterProgressiveBlur: return progressive_blur_valid(settings.progressive_blur, error);
    case kFilterSharpen: return sharpen_valid(settings.sharpen, error);
    case kFilterReduceNoise: return reduce_noise_valid(settings.reduce_noise, error);
  }
  return set_error(error, "unknown filter kind");
}

bool filter_settings_identity(const FilterSettings& settings) {
  switch (settings.kind) {
    case kFilterVignette: return settings.vignette.amount == 0.0;
    case kFilterTonalContrast:
      return settings.tonal_contrast.amount == 0.0 ||
             (settings.tonal_contrast.shadows == 0.0 && settings.tonal_contrast.midtones == 0.0 &&
              settings.tonal_contrast.highlights == 0.0);
    case kFilterBloom: return settings.bloom.amount == 0.0;
    case kFilterLensCorrection: return settings.lens_correction.distortion == 0.0;
    case kFilterGaussianBlur: return settings.gaussian_blur.radius == 0.0;
    case kFilterAddNoise: return settings.add_noise.amount == 0.0;
    case kFilterGrain: return settings.grain.amount == 0.0;
    case kFilterMotionBlur: return settings.motion_blur.distance == 0.0;
    case kFilterProgressiveBlur:
      /* 上游 guard strongest > 0：两端半径都为 0 时原样返回。 */
      return std::max(settings.progressive_blur.from, settings.progressive_blur.to) <= 0.0;
    case kFilterSharpen: return settings.sharpen.amount == 0.0;
    case kFilterReduceNoise: return settings.reduce_noise.strength == 0.0;
  }
  return true;
}

bool filter_settings_equal(const FilterSettings& a, const FilterSettings& b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case kFilterVignette: {
      const VignetteSettings& x = a.vignette;
      const VignetteSettings& y = b.vignette;
      return x.amount == y.amount && x.midpoint == y.midpoint && x.roundness == y.roundness &&
             x.feather == y.feather && x.highlights == y.highlights && x.color[0] == y.color[0] &&
             x.color[1] == y.color[1] && x.color[2] == y.color[2];
    }
    case kFilterTonalContrast: {
      const TonalContrastSettings& x = a.tonal_contrast;
      const TonalContrastSettings& y = b.tonal_contrast;
      return x.amount == y.amount && x.radius == y.radius && x.shadows == y.shadows &&
             x.midtones == y.midtones && x.highlights == y.highlights;
    }
    case kFilterBloom:
      return a.bloom.amount == b.bloom.amount && a.bloom.radius == b.bloom.radius;
    case kFilterLensCorrection:
      return a.lens_correction.distortion == b.lens_correction.distortion;
    case kFilterGaussianBlur:
      return a.gaussian_blur.radius == b.gaussian_blur.radius &&
             a.gaussian_blur.preserve_transparency == b.gaussian_blur.preserve_transparency;
    case kFilterAddNoise:
      return a.add_noise.amount == b.add_noise.amount &&
             a.add_noise.gaussian == b.add_noise.gaussian &&
             a.add_noise.monochromatic == b.add_noise.monochromatic &&
             a.add_noise.seed == b.add_noise.seed;
    case kFilterGrain:
      return a.grain.amount == b.grain.amount && a.grain.size == b.grain.size &&
             a.grain.roughness == b.grain.roughness && a.grain.seed == b.grain.seed;
    case kFilterMotionBlur:
      return a.motion_blur.angle == b.motion_blur.angle &&
             a.motion_blur.distance == b.motion_blur.distance;
    case kFilterProgressiveBlur: {
      const ProgressiveBlurSettings& x = a.progressive_blur;
      const ProgressiveBlurSettings& y = b.progressive_blur;
      return x.from == y.from && x.to == y.to && x.start_x == y.start_x && x.start_y == y.start_y &&
             x.end_x == y.end_x && x.end_y == y.end_y && x.radial == y.radial &&
             x.inverted == y.inverted && x.auto_axis == y.auto_axis;
    }
    case kFilterSharpen:
      return a.sharpen.amount == b.sharpen.amount && a.sharpen.radius == b.sharpen.radius &&
             a.sharpen.threshold == b.sharpen.threshold;
    case kFilterReduceNoise:
      return a.reduce_noise.strength == b.reduce_noise.strength &&
             a.reduce_noise.preserve_details == b.reduce_noise.preserve_details &&
             a.reduce_noise.reduce_color_noise == b.reduce_noise.reduce_color_noise;
  }
  return false;
}

const char* filter_kind_label(FilterKind kind) {
  switch (kind) {
    case kFilterVignette: return "Vignette";
    case kFilterTonalContrast: return "Tonal Contrast";
    case kFilterBloom: return "Bloom";
    case kFilterLensCorrection: return "Lens Correction";
    case kFilterGaussianBlur: return "Gaussian Blur";
    case kFilterAddNoise: return "Add Noise";
    case kFilterGrain: return "Grain";
    case kFilterMotionBlur: return "Motion Blur";
    case kFilterProgressiveBlur: return "Progressive Blur";
    case kFilterSharpen: return "Sharpen";
    case kFilterReduceNoise: return "Reduce Noise";
  }
  return "filter";
}

bool filter_apply(Document* document, comp_layer_id target, const FilterSettings& settings,
                  double scale, comp_result* out_code, bool* out_changed, std::string* note,
                  std::string* error) {
  auto fail = [&](comp_result code, const std::string& message) {
    if (error != nullptr) *error = message;
    if (out_code != nullptr) *out_code = code;
    if (out_changed != nullptr) *out_changed = false;
    return false;
  };
  if (document == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "filter needs a document");
  if (!finite(scale)) return fail(COMP_ERR_INVALID_ARGUMENT, "filter scale must be finite");
  std::string why;
  if (!filter_settings_valid(settings, &why)) {
    return fail(COMP_ERR_INVALID_ARGUMENT, why);
  }
  const char* label = filter_kind_label(settings.kind);
  if (filter_settings_identity(settings)) {
    if (out_code != nullptr) *out_code = COMP_OK;
    if (out_changed != nullptr) *out_changed = false;
    if (note != nullptr) *note += std::string(label) + " settings are the identity; layer left unchanged";
    return true;
  }
  /* <= 0 视为 1（与 camera_raw 的 scale 口径一致，见 comp_camera_raw.h）。 */
  const double effective_scale = scale > 0.0 ? scale : 1.0;

  Layer* layer = document->find(target);
  if (layer == nullptr) return fail(COMP_ERR_NOT_FOUND, "filter target layer was not found");
  if (layer->is_group) return fail(COMP_ERR_STATE, "filter cannot run on a group layer");
  /* 智能图层的像素是 source + 滤镜栈的求值缓存：直接烤进去会在下一次求值时
   * 整份丢掉（同文字图层的手绘像素）。要走 comp_layer_smart_filter_add。 */
  if (layer->has_smart_object()) {
    return fail(COMP_ERR_STATE,
                "layer is a smart object; add the filter to its smart filter stack instead");
  }
  /* 填充图层同理：pixels 由 kind + settings 渲出，烤进去会在下一次改参数时整份
   * 丢掉。要滤镜就把画面固定下来（comp_layer_rasterize_fill）。 */
  if (layer->has_fill()) {
    return fail(COMP_ERR_STATE,
                "layer is a fill layer; rasterize it before applying a filter");
  }
  if (!layer->has_pixels()) return fail(COMP_ERR_STATE, "filter layer has no pixels");

  const SelectionState& selection = document->state().selection;
  const bool clip = selection.active;
  if (clip && selection_is_empty(selection)) {
    /* 显式空选区：上游 blend 的 coverage 全 0，等价于什么都不改。 */
    if (out_code != nullptr) *out_code = COMP_OK;
    if (out_changed != nullptr) *out_changed = false;
    if (note != nullptr) *note += "document selection is empty; layer left unchanged";
    return true;
  }

  const bool owns_txn = !document->txn_active();
  std::string txn_error;
  if (owns_txn &&
      document->txn_begin(std::string("Filter: ") + label, &txn_error) != COMP_OK) {
    return fail(COMP_ERR_STATE, txn_error);
  }
  auto abort_txn = [&]() {
    if (owns_txn) document->txn_abort(&txn_error);
  };

  layer = document->find(target);
  if (layer == nullptr) {
    abort_txn();
    return fail(COMP_ERR_NOT_FOUND, "filter target layer vanished after the parameter gate");
  }

  comp_result status = COMP_OK;
  std::string failure;
  bool downgraded = false;
  try {
    /* 8 位直接跑；16 位先落 8 位工作副本（与工具层同一约定）；HDR 拒绝。 */
    if (layer->pixels.format == COMP_PIXEL_RGBA16) {
      if (!deep_to_rgba8(layer->pixels, &layer->pixels)) {
        status = COMP_ERR_OUT_OF_MEMORY;
        failure = "filter could not build the 8-bit working copy";
      } else {
        downgraded = true;
      }
    } else if (layer->pixels.format != COMP_PIXEL_RGBA8) {
      status = COMP_ERR_STATE;
      failure = "filter needs an 8-bit or 16-bit layer (an HDR layer has no 8-bit filter path)";
    }

    if (status == COMP_OK) {
      std::vector<uint8_t> original;
      /* 必须在 writable_pixels() 之前取只读视图：COW 之后原缓冲会被换掉。 */
      if (clip) original = layer->pixels.pixel_data();
      uint8_t* pixels = layer->pixels.writable_pixels().data();
      /* Add Noise 的噪声场锚在文档坐标（同上游 noiseOrigin）：原点取图层
       * effective_bounds()——图层移动后图案不会在像素上「游动」。 */
      const comp_rect bounds = layer->effective_bounds();
      /* Progressive Blur 的渐变轴：auto_axis 时取图层自身像素矩形中线、自顶向下
       * （上游 progressiveStart/End 为 nil 时的画布中线口径，本层不替宿主猜画布）；
       * 否则把文档坐标端点投到图层像素空间（连续、允许越界）。 */
      ProgressiveLine line;
      if (settings.kind == kFilterProgressiveBlur) {
        const ProgressiveBlurSettings& p = settings.progressive_blur;
        if (p.auto_axis) {
          line.x0 = line.x1 = (static_cast<double>(layer->pixels.width) - 1.0) * 0.5;
          line.y0 = 0.0;
          line.y1 = static_cast<double>(layer->pixels.height) - 1.0;
          line.valid = layer->pixels.width > 0 && layer->pixels.height > 0;
        } else {
          line.valid = layer->document_to_pixel_continuous(p.start_x, p.start_y, &line.x0,
                                                           &line.y0) &&
                       layer->document_to_pixel_continuous(p.end_x, p.end_y, &line.x1, &line.y1);
        }
      }
      status = apply_pixels(pixels, layer->pixels.width, layer->pixels.height,
                            layer->pixels.stride(), settings, effective_scale,
                            static_cast<int64_t>(bounds.x), static_cast<int64_t>(bounds.y), line);
      if (status != COMP_OK) {
        failure = status == COMP_ERR_OUT_OF_MEMORY ? "filter ran out of memory"
                                                   : "filter could not apply the settings";
      } else if (clip) {
        std::vector<uint8_t> coverage;
        selection_effective_coverage(selection, &coverage);
        blend_through_selection(pixels, layer->pixels.stride(), original.data(),
                                layer->pixels.width, layer->pixels.height, coverage, *layer,
                                document->state().width, document->state().height);
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
  /* S18：像素真的被改过 → 换号；**在 commit 之前**（历史 after 快照要带新号）。 */
  document->touch_layer(target);
  if (owns_txn) {
    std::string commit_error;
    if (document->txn_commit(&commit_error) != COMP_OK) {
      return fail(COMP_ERR_STATE, commit_error);
    }
  }
  document->mark_dirty(true);
  if (out_code != nullptr) *out_code = COMP_OK;
  if (out_changed != nullptr) *out_changed = true;
  if (note != nullptr) {
    *note += std::string(label) + " applied";
    if (downgraded) {
      *note += " on the 16-bit layer's 8-bit working copy (the layer is RGBA8 afterward)";
    }
    if (settings.kind == kFilterBloom) {
      *note += " with the bright-pass approximation (threshold 0.5; CIBloom's curve is not public)";
    }
    if (settings.kind == kFilterGaussianBlur) {
      *note += " via the shared three-pass box approximation of a Gaussian (sigma = radius * scale)";
    }
    if (settings.kind == kFilterMotionBlur) {
      *note += " as a box streak over distance * scale (CIMotionBlur's radius is the equivalent "
               "standard deviation, so the shape is not pixel-identical)";
    }
    if (settings.kind == kFilterProgressiveBlur) {
      *note += " as an 8-level blend of uniform box blurs (CIMaskedVariableBlur varies the radius "
               "per pixel, so the ramp is not pixel-identical)";
      if (settings.progressive_blur.auto_axis) {
        *note += ", axis defaulted to the layer's mid-line";
      }
    }
    if (settings.kind == kFilterSharpen) {
      *note += " as an unsharp mask over the shared box-blur approximation (sigma = radius * "
               "scale; alpha untouched)";
    }
    if (settings.kind == kFilterReduceNoise) {
      *note += " as a 5x5 bilateral on luma with chroma smoothing (alpha untouched; the upstream "
               "multi-scale algorithm is not public)";
    }
    if (clip) *note += ", clipped to the document selection";
  }
  return true;
}

/* ==================================================== 智能滤镜栈求值 */

namespace {

/* 对象像素空间里的 Progressive Blur 轴（见 filters.h 的坐标口径）：
 * auto_axis 取对象自身像素矩形中线、自顶向下；显式轴直接按对象像素坐标解释
 * ——不走 Layer::document_to_pixel_continuous，因为这里的「对象空间」就是
 * 像素空间，求值不依赖图层当前落在文档的哪里。 */
ProgressiveLine object_space_line(const FilterSettings& settings, int32_t width, int32_t height) {
  ProgressiveLine line;
  if (settings.kind != kFilterProgressiveBlur) return line;
  if (width <= 0 || height <= 0) return line;
  const ProgressiveBlurSettings& p = settings.progressive_blur;
  line.valid = true;
  if (p.auto_axis) {
    line.x0 = line.x1 = (static_cast<double>(width) - 1.0) * 0.5;
    line.y0 = 0.0;
    line.y1 = static_cast<double>(height) - 1.0;
  } else {
    line.x0 = p.start_x;
    line.y0 = p.start_y;
    line.x1 = p.end_x;
    line.y1 = p.end_y;
  }
  return line;
}

}  // namespace

bool filter_render_stack(const ImageBuffer& source, const std::vector<FilterSettings>& stack,
                         double scale, ImageBuffer* out, comp_result* out_code,
                         std::string* error) {
  auto fail = [&](comp_result code, const std::string& message) {
    if (error != nullptr) *error = message;
    if (out_code != nullptr) *out_code = code;
    return false;
  };
  if (out == nullptr) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "smart object render needs an output buffer");
  }
  if (!finite(scale)) return fail(COMP_ERR_INVALID_ARGUMENT, "filter scale must be finite");
  if (source.empty()) return fail(COMP_ERR_STATE, "smart object source has no pixels");
  /* 整栈参数闸在写任何缓冲之前：一条越界就整栈失败（不产生半成品）。 */
  for (const FilterSettings& settings : stack) {
    std::string why;
    if (!filter_settings_valid(settings, &why)) return fail(COMP_ERR_INVALID_ARGUMENT, why);
  }
  const double effective_scale = scale > 0.0 ? scale : 1.0;

  ImageBuffer working = source;
  if (working.format == COMP_PIXEL_RGBA16) {
    ImageBuffer eight;
    if (!deep_to_rgba8(working, &eight)) {
      return fail(COMP_ERR_OUT_OF_MEMORY, "smart object could not build the 8-bit working copy");
    }
    working = std::move(eight);
  } else if (working.format != COMP_PIXEL_RGBA8) {
    return fail(COMP_ERR_STATE,
                "smart filters need an 8-bit or 16-bit source (an HDR source has no 8-bit path)");
  }

  /* 独占一份缓冲再落笔：source 可能被历史快照或其它图层共享（COW 在这里生效）。 */
  std::vector<uint8_t>& bytes = working.writable_pixels();
  const int32_t width = working.width;
  const int32_t height = working.height;
  const size_t stride = working.stride();
  for (const FilterSettings& settings : stack) {
    if (filter_settings_identity(settings)) continue;
    const comp_result status =
        apply_pixels(bytes.data(), width, height, stride, settings, effective_scale, 0, 0,
                     object_space_line(settings, width, height));
    if (status != COMP_OK) {
      return fail(status, std::string(filter_kind_label(settings.kind)) +
                              ": smart filter could not be evaluated");
    }
  }
  *out = std::move(working);
  if (out_code != nullptr) *out_code = COMP_OK;
  return true;
}

}  // namespace compositor
