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
 *   High Pass       adjustment_eval.h blur_rgba8_copy + 本文件 high_pass_pixels
 *   Minimum        本文件 morphology_pixels（方形窗口腐蚀，单调队列）
 *   Maximum        本文件 morphology_pixels（方形窗口膨胀，单调队列）
 *   Offset         本文件 offset_pixels（整像素平移，三种露出处理）
 *   Custom         本文件 custom_pixels（5×5 卷积，边缘延展）
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

bool high_pass_valid(const HighPassSettings& s, std::string* error) {
  /* 0 = 恒等；>0 时按上游量程 0.1..250。中间那段 (0, 0.1) 显式拒绝。 */
  if (s.radius != 0.0 && !in_range(s.radius, 0.1, 250.0)) {
    return set_error(error, "high pass radius must be 0 or in 0.1..250");
  }
  return true;
}

bool denoise_valid(const DenoiseSettings& s, int32_t max_threshold, std::string* error) {
  if (!(s.radius == 0.0 || (std::isfinite(s.radius) && s.radius >= 1.0 && s.radius <= 50.0))) {
    return set_error(error, "denoise radius must be 0 or in 1..50");
  }
  if (s.threshold < 0 || s.threshold > max_threshold) {
    return set_error(error, "denoise threshold is out of range");
  }
  return true;
}

bool minimum_valid(const MinimumSettings& s, std::string* error) {
  /* 0 = 恒等；>0 时按上游量程 1..100。中间那段 (0, 1) 显式拒绝（零半径无意义）。 */
  if (s.radius != 0.0 && !in_range(s.radius, 1.0, 100.0)) {
    return set_error(error, "minimum radius must be 0 or in 1..100");
  }
  return true;
}

bool maximum_valid(const MaximumSettings& s, std::string* error) {
  if (s.radius != 0.0 && !in_range(s.radius, 1.0, 100.0)) {
    return set_error(error, "maximum radius must be 0 or in 1..100");
  }
  return true;
}

bool offset_valid(const OffsetSettings& s, std::string* error) {
  if (!in_range(s.horizontal, -10000.0, 10000.0)) {
    return set_error(error, "offset horizontal out of -10000..10000");
  }
  if (!in_range(s.vertical, -10000.0, 10000.0)) {
    return set_error(error, "offset vertical out of -10000..10000");
  }
  if (s.wrap < 0 || s.wrap > 2) {
    return set_error(error, "offset wrap must be 0 (transparent), 1 (edge) or 2 (wrap-around)");
  }
  return true;
}

/* 核项 / 除数 / 偏移都按上游对话框的 ±999 量程；越界拒绝，不静默夹取。 */
bool custom_valid(const CustomSettings& s, std::string* error) {
  for (int i = 0; i < 25; ++i) {
    if (!in_range(s.kernel[i], -999.0, 999.0)) {
      return set_error(error, "custom kernel entry out of -999..999");
    }
  }
  if (!in_range(s.scale, -999.0, 999.0)) {
    return set_error(error, "custom scale out of -999..999");
  }
  if (!in_range(s.offset, -999.0, 999.0)) {
    return set_error(error, "custom offset out of -999..999");
  }
  return true;
}

/* 恒等核 + 不缩放 + 不偏移。scale 0 与 1 同义（都为除数 1），两者都算恒等。 */
bool custom_identity(const CustomSettings& s) {
  for (int i = 0; i < 25; ++i) {
    if (s.kernel[i] != (i == 12 ? 1.0 : 0.0)) return false;
  }
  return (s.scale == 0.0 || s.scale == 1.0) && s.offset == 0.0;
}

bool custom_equal(const CustomSettings& a, const CustomSettings& b) {
  for (int i = 0; i < 25; ++i) {
    if (a.kernel[i] != b.kernel[i]) return false;
  }
  return a.scale == b.scale && a.offset == b.offset;
}

/* A46：扭曲 / 像素化 / 风格化三组的参数闸（0 = 恒等；非 0 按上游量程，越界拒绝）。 */
bool spherize_valid(const SpherizeSettings& s, std::string* error) {
  if (s.amount != 0.0 && !in_range(s.amount, -100.0, 100.0)) {
    return set_error(error, "spherize amount must be 0 or in -100..100");
  }
  return true;
}

bool pinch_valid(const PinchSettings& s, std::string* error) {
  if (s.amount != 0.0 && !in_range(s.amount, -100.0, 100.0)) {
    return set_error(error, "pinch amount must be 0 or in -100..100");
  }
  return true;
}

bool twirl_valid(const TwirlSettings& s, std::string* error) {
  if (s.angle != 0.0 && !in_range(s.angle, -999.0, 999.0)) {
    return set_error(error, "twirl angle must be 0 or in -999..999");
  }
  return true;
}

bool mosaic_valid(const MosaicSettings& s, std::string* error) {
  /* 0 = 恒等；(0,2) 之间没有可用的方格（PS 最小 2），显式拒绝而不是夹到 2。 */
  if (s.cell_size != 0.0 && !in_range(s.cell_size, 2.0, 200.0)) {
    return set_error(error, "mosaic cell size must be 0 or in 2..200");
  }
  return true;
}

bool emboss_valid(const EmbossSettings& s, std::string* error) {
  if (s.amount != 0.0 && !in_range(s.amount, 1.0, 500.0)) {
    return set_error(error, "emboss amount must be 0 or in 1..500");
  }
  if (!in_range(s.angle, -360.0, 360.0)) {
    return set_error(error, "emboss angle out of -360..360");
  }
  if (!in_range(s.height, 0.0, 10.0)) {
    return set_error(error, "emboss height out of 0..10");
  }
  return true;
}

/* A47：渲染组三项的参数闸（0 = 恒等；非 0 按上游量程，越界拒绝）。 */
bool clouds_valid(const CloudsSettings& s, std::string* error) {
  if (!in_range(s.amount, 0.0, 100.0)) {
    return set_error(error, "clouds amount out of 0..100");
  }
  if (!in_range(s.size, 2.0, 1000.0)) {
    return set_error(error, "clouds size out of 2..1000");
  }
  for (int c = 0; c < 3; ++c) {
    if (!in_range(s.color_low[c], 0.0, 1.0)) {
      return set_error(error, "clouds color_low out of 0..1");
    }
    if (!in_range(s.color_high[c], 0.0, 1.0)) {
      return set_error(error, "clouds color_high out of 0..1");
    }
  }
  return true;
}

bool difference_clouds_valid(const DifferenceCloudsSettings& s, std::string* error) {
  if (!in_range(s.amount, 0.0, 100.0)) {
    return set_error(error, "difference clouds amount out of 0..100");
  }
  if (!in_range(s.size, 2.0, 1000.0)) {
    return set_error(error, "difference clouds size out of 2..1000");
  }
  return true;
}

bool picture_frame_valid(const PictureFrameSettings& s, std::string* error) {
  if (s.width != 0.0 && !in_range(s.width, 1.0, 500.0)) {
    return set_error(error, "picture frame width must be 0 or in 1..500");
  }
  for (int c = 0; c < 3; ++c) {
    if (!in_range(s.color[c], 0.0, 1.0)) {
      return set_error(error, "picture frame color out of 0..1");
    }
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

/*
 * High Pass：`128 + (src - blur(src))`，直通色上算、乘回**原** alpha。
 *
 * 与 Sharpen / Gaussian Blur 共用 blur_rgba8_copy（sigma = radius * scale），
 * 不另起一套高斯。上游 High Pass 的结果就是「中灰 + 细节」，之后通常靠叠加 /
 * 柔光混合回原图，所以三条可观察口径是：
 *   - 平坦区域落 128（无细节即无偏移）；
 *   - alpha 逐字节不变（图层轮廓不因滤镜扩散）；
 *   - 半透明边缘按直通色比较，不会被透明邻域拉暗。
 * 模糊后 alpha 为 0 的邻域没有颜色可比，按 128 写（差值 0）。
 */
comp_result high_pass_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                             const HighPassSettings& s, double scale) {
  if (s.radius <= 0.0) return COMP_OK;
  const double sigma = std::max(0.1, s.radius * scale);
  std::vector<uint8_t> blurred;
  if (!blur_rgba8_copy(rgba, width, height, stride, static_cast<float>(sigma), &blurred)) {
    return COMP_ERR_OUT_OF_MEMORY;
  }
  const size_t blurred_stride = static_cast<size_t>(width) * 4u;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* soft = blurred.data() + static_cast<size_t>(y) * blurred_stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      const double alpha = p[3];
      if (alpha == 0.0) continue;
      const uint8_t* b = soft + static_cast<size_t>(x) * 4;
      for (int c = 0; c < 3; ++c) {
        double value = 128.0;
        if (b[3] != 0) {
          const double straight = std::min(255.0, static_cast<double>(p[c]) * 255.0 / alpha);
          const double soft_straight = std::min(
              255.0, static_cast<double>(b[c]) * 255.0 / static_cast<double>(b[3]));
          value = 128.0 + (straight - soft_straight);
        }
        const double clamped = value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value);
        const double premultiplied = std::lround(clamped * alpha / 255.0);
        p[c] = static_cast<uint8_t>(std::min(premultiplied, alpha));
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

/*
 * 一维滑动窗口极值（单调队列，O(n)）。窗口是 [i - r, i + r] 裁到 [0, count-1]：
 * 与共享盒式模糊的边界口径一致——不做边缘复制，窗口只看落在图层内的样本。
 */
void morphology_line(const uint8_t* values, int32_t count, int32_t radius, bool dilate,
                     uint8_t* out) {
  if (count <= 0) return;
  std::vector<int32_t> deque(static_cast<size_t>(count));
  int32_t head = 0;
  int32_t tail = 0; /* [head, tail) 是单调队列里的下标 */
  int32_t next = 0; /* 下一个待入队下标 */
  for (int32_t i = 0; i < count; ++i) {
    const int32_t high = std::min(count - 1, i + radius);
    while (next <= high) {
      const uint8_t value = values[next];
      while (tail > head) {
        const uint8_t back = values[deque[tail - 1]];
        /* 求最小值时丢掉不小于新值的尾部；求最大值时丢掉不大于新值的尾部。 */
        const bool drop = dilate ? back <= value : back >= value;
        if (!drop) break;
        --tail;
      }
      deque[tail++] = next;
      ++next;
    }
    const int32_t low = i - radius;
    while (tail > head && deque[head] < low) ++head;
    out[i] = values[deque[head]];
  }
}

/*
 * Minimum / Maximum：方形窗口的形态学腐蚀 / 膨胀，四个通道一起做。
 *
 * 作用在**预乘** RGBA8 上是安全的：逐像素颜色不超过 alpha（预乘不变式），
 * 于是窗口内颜色最大值不会超过 alpha 最大值、最小值不会小于 alpha 最小值——
 * 结果仍是合法预乘像素，不会出现「颜色越界」。
 *
 * 半径从图层像素换成预览像素（radius * scale，四舍五入，至少 1）；窗口大于
 * 图像时等价于覆盖全图，因此每轴再裁到 (维度 - 1)。横向一趟写进临时缓冲，
 * 纵向一趟写回图层，两趟都是 O(w * h)，不随半径变慢。
 */
/*
 * Median（中间值）与 Dust & Scratches（蒙尘与划痕）——A45（2026-10-27）。
 *
 * 两者都是**去噪**族：Median 取窗口内中位数，Dust & Scratches 只在「中心与
 * 中位数的差超过阈值」时才用中位数替换（PS 的口径：threshold=0 时两者等价）。
 * 实现上共用一份窗口采样，避免把同一段邻域遍历写两遍。
 *
 * 与 Minimum/Maximum 的关系：那两个是**排序的两端**（单调队列可 O(1) 摊销），
 * 中位数必须真正取中（需要选择算法），所以不能复用 morphology_line。这里用
 * 「复制窗口 + nth_element」：半径上限压到 50（PS 的中间值最大 100，但那是
 * 直径口径；本仓半径口径下 50 已等价），单像素代价 O(r² log r) 但 r 很小。
 */
namespace {

/* 逐通道取窗口中位数；threshold > 0 时只在超阈值处替换（Dust & Scratches）。 */
comp_result median_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                          double radius, double scale, int32_t threshold) {
  if (width <= 0 || height <= 0) return COMP_OK;
  int64_t requested = static_cast<int64_t>(std::llround(radius * scale));
  if (requested < 1) requested = 1;
  const int32_t rx = static_cast<int32_t>(std::min<int64_t>(requested, width - 1));
  const int32_t ry = static_cast<int32_t>(std::min<int64_t>(requested, height - 1));
  if (rx < 1 && ry < 1) return COMP_OK;

  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }

  /* 窗口最多 (2rx+1)*(2ry+1) 个样本；用固定容量向量避免每像素分配。 */
  const size_t window_capacity = static_cast<size_t>(2 * rx + 1) * static_cast<size_t>(2 * ry + 1);
  std::vector<uint8_t> samples(window_capacity);

  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const size_t center = static_cast<size_t>(y) * static_cast<size_t>(width) +
                            static_cast<size_t>(x);
      for (int c = 0; c < 4; ++c) {
        size_t count = 0;
        /* 边界用「夹取」而不是「跳过」：跳过会让靠边的像素样本数变少、中位数
         * 偏移；夹取等同边缘延展，与 Minimum/Maximum 的处理一致。 */
        for (int32_t dy = -ry; dy <= ry; ++dy) {
          int32_t sy = y + dy;
          if (sy < 0) sy = 0;
          if (sy >= height) sy = height - 1;
          const uint8_t* row = source.data() + static_cast<size_t>(sy) * row_bytes;
          for (int32_t dx = -rx; dx <= rx; ++dx) {
            int32_t sx = x + dx;
            if (sx < 0) sx = 0;
            if (sx >= width) sx = width - 1;
            samples[count++] = row[static_cast<size_t>(sx) * 4u + static_cast<size_t>(c)];
          }
        }
        const size_t mid = count / 2;
        std::nth_element(samples.begin(), samples.begin() + static_cast<ptrdiff_t>(mid),
                         samples.begin() + static_cast<ptrdiff_t>(count));
        const uint8_t median = samples[mid];
        const uint8_t original = source[center * 4u + static_cast<size_t>(c)];
        uint8_t value = median;
        if (threshold > 0) {
          const int32_t diff = static_cast<int32_t>(original) - static_cast<int32_t>(median);
          /* 超阈值才替换：这就是 Dust & Scratches 与 Median 的唯一差别。 */
          if (diff <= threshold && diff >= -threshold) value = original;
        }
        rgba[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u +
             static_cast<size_t>(c)] = value;
      }
    }
  }
  return COMP_OK;
}

}  // namespace

comp_result morphology_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                              double radius, double scale, bool dilate) {
  if (width <= 0 || height <= 0) return COMP_OK;
  int64_t requested = static_cast<int64_t>(std::llround(radius * scale));
  if (requested < 1) requested = 1;
  const int32_t row_bytes = width * 4;
  const int32_t rx = static_cast<int32_t>(std::min<int64_t>(requested, width - 1));
  const int32_t ry = static_cast<int32_t>(std::min<int64_t>(requested, height - 1));
  const size_t longest = static_cast<size_t>(std::max(width, height));
  std::vector<uint8_t> line_in(longest);
  std::vector<uint8_t> line_out(longest);
  std::vector<uint8_t> temp(static_cast<size_t>(row_bytes) * static_cast<size_t>(height));

  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + static_cast<size_t>(y) * stride;
    uint8_t* dst = temp.data() + static_cast<size_t>(y) * static_cast<size_t>(row_bytes);
    for (int c = 0; c < 4; ++c) {
      for (int32_t x = 0; x < width; ++x) {
        line_in[static_cast<size_t>(x)] = src[static_cast<size_t>(x) * 4u + c];
      }
      morphology_line(line_in.data(), width, rx, dilate, line_out.data());
      for (int32_t x = 0; x < width; ++x) {
        dst[static_cast<size_t>(x) * 4u + c] = line_out[static_cast<size_t>(x)];
      }
    }
  }

  for (int32_t x = 0; x < width; ++x) {
    const size_t column = static_cast<size_t>(x) * 4u;
    for (int c = 0; c < 4; ++c) {
      for (int32_t y = 0; y < height; ++y) {
        line_in[static_cast<size_t>(y)] =
            temp[static_cast<size_t>(y) * static_cast<size_t>(row_bytes) + column + c];
      }
      morphology_line(line_in.data(), height, ry, dilate, line_out.data());
      for (int32_t y = 0; y < height; ++y) {
        rgba[static_cast<size_t>(y) * stride + column + c] = line_out[static_cast<size_t>(y)];
      }
    }
  }
  return COMP_OK;
}

/*
 * Offset：整像素平移，目标像素读源 `(x - dx, y - dy)`，不重采样。
 *
 * 位移按**图层像素**设定，预览再乘 scale 后四舍五入（与其它半径同一口径：
 * 降采样预览的观感与全尺寸一致）。wrap 三种露出处理（数值见 filters.h）：
 * 0 透明、1 边缘延展、2 环绕。先整份拷出源像素再写回，源与目标重叠不会互相污染。
 */
comp_result offset_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                          const OffsetSettings& s, double scale) {
  if (width <= 0 || height <= 0) return COMP_OK;
  const int64_t dx = std::llround(s.horizontal * scale);
  const int64_t dy = std::llround(s.vertical * scale);
  if (dx == 0 && dy == 0) return COMP_OK;

  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }

  const int32_t last_x = width - 1;
  const int32_t last_y = height - 1;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      int64_t sx = static_cast<int64_t>(x) - dx;
      int64_t sy = static_cast<int64_t>(y) - dy;
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      if (sx < 0 || sx >= width || sy < 0 || sy >= height) {
        if (s.wrap == 0) {
          p[0] = p[1] = p[2] = p[3] = 0;
          continue;
        }
        if (s.wrap == 1) {
          sx = std::min<int64_t>(std::max<int64_t>(sx, 0), last_x);
          sy = std::min<int64_t>(std::max<int64_t>(sy, 0), last_y);
        } else {
          sx %= width;
          if (sx < 0) sx += width;
          sy %= height;
          if (sy < 0) sy += height;
        }
      }
      std::memcpy(p, source.data() + static_cast<size_t>(sy) * row_bytes +
                         static_cast<size_t>(sx) * 4u, 4u);
    }
  }
  return COMP_OK;
}

/*
 * Custom：5×5 卷积，直通色上算、乘回原 alpha。
 *
 * 与 High Pass / Sharpen / Reduce Noise 同一套「直通色 -> 预乘回去」的口径：
 *   - alpha == 0 的像素原样跳过；邻居 alpha == 0 时按 0 参与累加（透明区没有色）；
 *   - 邻居坐标夹到图层内（边缘延展），所以边缘不会因为窗外像素被当成黑而发暗；
 *   - 结果 `sum / divisor + offset`，逐通道夹到 0..255 后再乘回本像素的 alpha。
 *
 * 先整份拷出源像素：卷积要读原图邻居，原地写会污染右侧/下方的采样。
 * 除数为 0 时按 1 处理（面板允许把 Scale 清成 0；引擎不为 0 除）。
 */
comp_result custom_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                          const CustomSettings& s) {
  if (width <= 0 || height <= 0) return COMP_OK;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }

  const double divisor = s.scale == 0.0 ? 1.0 : s.scale;
  const int32_t last_x = width - 1;
  const int32_t last_y = height - 1;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      const double alpha = p[3];
      if (alpha == 0.0) continue;
      for (int c = 0; c < 3; ++c) {
        double sum = 0.0;
        for (int ky = -2; ky <= 2; ++ky) {
          const double* weights = s.kernel + (ky + 2) * 5;
          int32_t sy = y + ky;
          if (sy < 0) sy = 0;
          if (sy > last_y) sy = last_y;
          const uint8_t* src_row = source.data() + static_cast<size_t>(sy) * row_bytes;
          for (int kx = -2; kx <= 2; ++kx) {
            const double weight = weights[kx + 2];
            if (weight == 0.0) continue;
            int32_t sx = x + kx;
            if (sx < 0) sx = 0;
            if (sx > last_x) sx = last_x;
            const uint8_t* q = src_row + static_cast<size_t>(sx) * 4u;
            if (q[3] == 0) continue;
            const double straight =
                std::min(255.0, static_cast<double>(q[c]) * 255.0 / static_cast<double>(q[3]));
            sum += weight * straight;
          }
        }
        const double value = sum / divisor + s.offset;
        const double clamped = value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value);
        const double premultiplied = std::lround(clamped * alpha / 255.0);
        p[c] = static_cast<uint8_t>(std::min(premultiplied, alpha));
      }
    }
  }
  return COMP_OK;
}

/* ============================================================ A46：扭曲 / 像素化 / 风格化
 *
 * 扭曲三兄弟（Spherize / Pinch / Twirl）共用同一套「以图层中心为原点、半短边
 * 归一」的径向重映射 + 双线性采样；Mosaic 是格内预乘均值；Emboss 是方向导数。
 * 重采样口径与 lens_distort 一致：源坐标落在图像外按**透明**参与（不复制边缘），
 * 输出是预乘 RGBA8、满足 out_color <= out_alpha。
 */

/* 双线性采样：返回**直通** RGB（0..255）与覆盖率 alpha（0..255）。
 * 越界格子按透明（alpha 0）计入权重，所以贴边像素不会把窗外当成黑。 */
void sample_bilinear_raw(const uint8_t* src, int32_t width, int32_t height, size_t row_bytes,
                         double px, double py, double rgb[3], double* out_alpha) {
  const int32_t x0 = static_cast<int32_t>(std::floor(px));
  const int32_t y0 = static_cast<int32_t>(std::floor(py));
  const double fx = px - static_cast<double>(x0);
  const double fy = py - static_cast<double>(y0);
  double acc_rgb[3] = {0.0, 0.0, 0.0};
  double acc_a = 0.0;
  double acc_w = 0.0;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const double w = (i ? fx : 1.0 - fx) * (j ? fy : 1.0 - fy);
      acc_w += w;
      const int32_t sx = x0 + i;
      const int32_t sy = y0 + j;
      if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
      const uint8_t* q = src + static_cast<size_t>(sy) * row_bytes + static_cast<size_t>(sx) * 4u;
      const double a = static_cast<double>(q[3]);
      acc_a += a * w;
      for (int c = 0; c < 3; ++c) {
        const double straight =
            a > 0.0 ? std::min(255.0, static_cast<double>(q[c]) * 255.0 / a) : 0.0;
        acc_rgb[c] += straight * a * w;
      }
    }
  }
  for (int c = 0; c < 3; ++c) rgb[c] = acc_a > 0.0 ? acc_rgb[c] / acc_a : 0.0;
  *out_alpha = acc_w > 0.0 ? acc_a / acc_w : 0.0;
}

/* 通用径向重映射。map 把目标像素的归一坐标 (dx, dy) 映射到源归一坐标 (sx, sy)；
 * 归一坐标以图层中心为原点、半短边为 1。Spherize / Pinch / Twirl 共用。 */
template <typename MapFn>
comp_result distort_remap(uint8_t* rgba, int32_t width, int32_t height, size_t stride, MapFn map) {
  if (width <= 0 || height <= 0) return COMP_OK;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }
  const double cx = (static_cast<double>(width) - 1.0) * 0.5;
  const double cy = (static_cast<double>(height) - 1.0) * 0.5;
  const double half = std::max(1.0, static_cast<double>(std::min(width, height)) * 0.5);
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const double dy = (static_cast<double>(y) - cy) / half;
    for (int32_t x = 0; x < width; ++x) {
      const double dx = (static_cast<double>(x) - cx) / half;
      double sx = dx;
      double sy = dy;
      map(dx, dy, &sx, &sy);
      double rgb[3];
      double a = 0.0;
      sample_bilinear_raw(source.data(), width, height, row_bytes, cx + sx * half, cy + sy * half,
                          rgb, &a);
      if (a < 0.0) a = 0.0;
      if (a > 255.0) a = 255.0;
      const uint8_t aq = static_cast<uint8_t>(std::lround(a));
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      for (int c = 0; c < 3; ++c) {
        double premul = rgb[c] * a / 255.0;
        if (premul < 0.0) premul = 0.0;
        if (premul > a) premul = a;
        p[c] = static_cast<uint8_t>(std::lround(premul));
      }
      p[3] = aq;
    }
  }
  return COMP_OK;
}

comp_result spherize_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                            const SpherizeSettings& s) {
  if (s.amount == 0.0) return COMP_OK;
  const double k = s.amount / 100.0;
  return distort_remap(
      rgba, width, height, stride,
      [k](double dx, double dy, double* sx, double* sy) {
        const double r2 = dx * dx + dy * dy;
        const double scale = 1.0 - k * (1.0 - r2);
        *sx = dx * scale;
        *sy = dy * scale;
      });
}

comp_result pinch_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const PinchSettings& s) {
  if (s.amount == 0.0) return COMP_OK;
  const double k = s.amount / 100.0;
  return distort_remap(
      rgba, width, height, stride,
      [k](double dx, double dy, double* sx, double* sy) {
        const double r2 = dx * dx + dy * dy;
        const double scale = 1.0 + k * (1.0 - r2);
        *sx = dx * scale;
        *sy = dy * scale;
      });
}

comp_result twirl_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const TwirlSettings& s) {
  if (s.angle == 0.0) return COMP_OK;
  const double radians = s.angle * 3.14159265358979323846 / 180.0;
  return distort_remap(
      rgba, width, height, stride,
      [radians](double dx, double dy, double* sx, double* sy) {
        const double r = std::sqrt(dx * dx + dy * dy);
        double falloff = 1.0 - r;
        if (falloff < 0.0) falloff = 0.0;
        const double theta = std::atan2(dy, dx) - radians * falloff;
        *sx = r * std::cos(theta);
        *sy = r * std::sin(theta);
      });
}

/* Mosaic：格内预乘分量的算术均值（满足 out_color <= out_alpha）。 */
comp_result mosaic_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                          const MosaicSettings& s, double scale) {
  if (s.cell_size <= 0.0 || width <= 0 || height <= 0) return COMP_OK;
  int64_t cell = std::llround(s.cell_size * scale);
  if (cell < 2) cell = 2;
  const int32_t step = static_cast<int32_t>(cell);
  for (int32_t by = 0; by < height; by += step) {
    for (int32_t bx = 0; bx < width; bx += step) {
      const int32_t ex = static_cast<int32_t>(std::min<int64_t>(bx + cell, width));
      const int32_t ey = static_cast<int32_t>(std::min<int64_t>(by + cell, height));
      uint64_t sum[4] = {0u, 0u, 0u, 0u};
      int64_t n = 0;
      for (int32_t y = by; y < ey; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
        for (int32_t x = bx; x < ex; ++x) {
          const uint8_t* p = row + static_cast<size_t>(x) * 4u;
          for (int c = 0; c < 4; ++c) sum[c] += p[c];
          ++n;
        }
      }
      if (n == 0) continue;
      uint8_t avg[4];
      const uint64_t denom = static_cast<uint64_t>(n);
      for (int c = 0; c < 4; ++c) {
        avg[c] = static_cast<uint8_t>((sum[c] + denom / 2u) / denom);
      }
      for (int32_t y = by; y < ey; ++y) {
        uint8_t* row = rgba + static_cast<size_t>(y) * stride;
        for (int32_t x = bx; x < ex; ++x) {
          uint8_t* p = row + static_cast<size_t>(x) * 4u;
          p[0] = avg[0];
          p[1] = avg[1];
          p[2] = avg[2];
          p[3] = avg[3];
        }
      }
    }
  }
  return COMP_OK;
}

/* Emboss：`128 + amount/100 * (A - B)`，A / B 沿 angle 方向 ±step 处采样的直通色。
 * 平坦区域落 128 底灰；alpha 逐字节不变。 */
comp_result emboss_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                          const EmbossSettings& s, double scale) {
  if (s.amount == 0.0 || width <= 0 || height <= 0) return COMP_OK;
  const double strength = s.amount / 100.0;
  const double radians = s.angle * 3.14159265358979323846 / 180.0;
  double step = std::round(s.height * scale);
  if (step < 1.0) step = 1.0;
  /* 逆时针为正、y 向下：方向 = (cos, -sin)。 */
  const double ox = std::cos(radians) * step;
  const double oy = -std::sin(radians) * step;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      const double alpha = p[3];
      if (alpha <= 0.0) continue;
      double a_rgb[3];
      double a_a = 0.0;
      double b_rgb[3];
      double b_a = 0.0;
      sample_bilinear_raw(source.data(), width, height, row_bytes, x + ox, y + oy, a_rgb, &a_a);
      sample_bilinear_raw(source.data(), width, height, row_bytes, x - ox, y - oy, b_rgb, &b_a);
      for (int c = 0; c < 3; ++c) {
        double value = 128.0 + strength * (a_rgb[c] - b_rgb[c]);
        if (value < 0.0) value = 0.0;
        if (value > 255.0) value = 255.0;
        double premul = std::lround(value * alpha / 255.0);
        if (premul > alpha) premul = alpha;
        p[c] = static_cast<uint8_t>(premul);
      }
    }
  }
  return COMP_OK;
}

/* ============================================================ A47：渲染组
 *
 * 确定性值噪声（fBm）——Clouds / Difference Clouds 的唯一随机源。不用 rand()：
 * 同 seed + 同坐标必须逐字节可复现（智能滤镜重求值、撤销/重做都依赖这一点）。
 * 哈希把 (ix, iy, seed) 混成一个 32 位数，值噪声在格点间用 smoothstep 插值，
 * 5 个八度按振幅折半叠加。
 */
uint32_t cloud_hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

double cloud_lattice(int32_t ix, int32_t iy, uint32_t seed) {
  const uint32_t hx = cloud_hash(static_cast<uint32_t>(ix) * 0x9e3779b9u ^ seed);
  const uint32_t h = cloud_hash(hx ^ (static_cast<uint32_t>(iy) * 0x85ebca6bu));
  return static_cast<double>(h) * (1.0 / 4294967296.0);
}

double cloud_smooth(double t) { return t * t * (3.0 - 2.0 * t); }

double cloud_value(double x, double y, uint32_t seed) {
  const double fx = std::floor(x);
  const double fy = std::floor(y);
  const int32_t ix = static_cast<int32_t>(fx);
  const int32_t iy = static_cast<int32_t>(fy);
  const double tx = cloud_smooth(x - fx);
  const double ty = cloud_smooth(y - fy);
  const double v00 = cloud_lattice(ix, iy, seed);
  const double v10 = cloud_lattice(ix + 1, iy, seed);
  const double v01 = cloud_lattice(ix, iy + 1, seed);
  const double v11 = cloud_lattice(ix + 1, iy + 1, seed);
  const double lo = v00 + (v10 - v00) * tx;
  const double hi = v01 + (v11 - v01) * tx;
  return lo + (hi - lo) * ty;
}

/* 5 个八度的分形叠加，返回 0..1。 */
double cloud_fbm(double x, double y, uint32_t seed) {
  double sum = 0.0;
  double amp = 0.5;
  double freq = 1.0;
  double norm = 0.0;
  for (int octave = 0; octave < 5; ++octave) {
    sum += amp * cloud_value(x * freq, y * freq, seed + static_cast<uint32_t>(octave) * 0x9e37u);
    norm += amp;
    amp *= 0.5;
    freq *= 2.0;
  }
  return norm > 0.0 ? sum / norm : 0.0;
}

/* Clouds：云纹端色按噪声值在 color_low / color_high 之间线性插值，再按 amount
 * 与原像素做预乘混合（alpha 逐字节不变）。噪声锚在文档坐标（origin = 图层
 * effective_bounds 原点），图层移动后图案不游动（同 Add Noise）。 */
comp_result clouds_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                          const CloudsSettings& s, double scale, int64_t origin_x,
                          int64_t origin_y) {
  if (s.amount <= 0.0 || width <= 0 || height <= 0) return COMP_OK;
  const double feature = std::max(1.0, s.size * (scale > 0.0 ? scale : 1.0));
  const double weight = s.amount / 100.0;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const double ny = static_cast<double>(origin_y + y) / feature;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      const double alpha = p[3];
      if (alpha <= 0.0) continue;
      const double nx = static_cast<double>(origin_x + x) / feature;
      const double c = cloud_fbm(nx, ny, s.seed);
      const uint8_t* src = source.data() + static_cast<size_t>(y) * row_bytes +
                           static_cast<size_t>(x) * 4u;
      for (int ch = 0; ch < 3; ++ch) {
        const double cloud =
            (s.color_low[ch] + (s.color_high[ch] - s.color_low[ch]) * c) * 255.0;
        double value = static_cast<double>(src[ch]) * (1.0 - weight) +
                       cloud * (alpha / 255.0) * weight;
        if (value < 0.0) value = 0.0;
        if (value > alpha) value = alpha;
        p[ch] = static_cast<uint8_t>(std::lround(value));
      }
    }
  }
  return COMP_OK;
}

/* Difference Clouds：灰度云纹 c（0..255），逐通道 out = src*(1-a) + |c - src| * a
 * （src 为直通色），alpha 逐字节不变。 */
comp_result difference_clouds_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                                     const DifferenceCloudsSettings& s, double scale,
                                     int64_t origin_x, int64_t origin_y) {
  if (s.amount <= 0.0 || width <= 0 || height <= 0) return COMP_OK;
  const double feature = std::max(1.0, s.size * (scale > 0.0 ? scale : 1.0));
  const double weight = s.amount / 100.0;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(row_bytes * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<size_t>(y) * row_bytes,
                rgba + static_cast<size_t>(y) * stride, row_bytes);
  }
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const double ny = static_cast<double>(origin_y + y) / feature;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      const double alpha = p[3];
      if (alpha <= 0.0) continue;
      const double nx = static_cast<double>(origin_x + x) / feature;
      const double cloud = cloud_fbm(nx, ny, s.seed) * 255.0;
      const uint8_t* src = source.data() + static_cast<size_t>(y) * row_bytes +
                           static_cast<size_t>(x) * 4u;
      for (int ch = 0; ch < 3; ++ch) {
        const double straight = std::min(255.0, static_cast<double>(src[ch]) * 255.0 / alpha);
        const double diff = std::fabs(cloud - straight);
        const double straight_out = straight * (1.0 - weight) + diff * weight;
        double value = std::lround(straight_out * alpha / 255.0);
        if (value < 0.0) value = 0.0;
        if (value > alpha) value = alpha;
        p[ch] = static_cast<uint8_t>(value);
      }
    }
  }
  return COMP_OK;
}

/* Picture Frame：沿四边落一圈等厚色带（到最近边界的像素数 < t），alpha 不变。 */
comp_result picture_frame_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                                 const PictureFrameSettings& s, double scale) {
  if (s.width <= 0.0 || width <= 0 || height <= 0) return COMP_OK;
  int64_t t = std::llround(s.width * (scale > 0.0 ? scale : 1.0));
  if (t < 1) t = 1;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const int64_t dy = std::min<int64_t>(y, static_cast<int64_t>(height) - 1 - y);
    for (int32_t x = 0; x < width; ++x) {
      const int64_t dx = std::min<int64_t>(x, static_cast<int64_t>(width) - 1 - x);
      if (std::min(dx, dy) >= t) continue;
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      const double alpha = p[3];
      if (alpha <= 0.0) continue;
      for (int ch = 0; ch < 3; ++ch) {
        double value = std::lround(s.color[ch] * 255.0 * alpha / 255.0);
        if (value < 0.0) value = 0.0;
        if (value > alpha) value = alpha;
        p[ch] = static_cast<uint8_t>(value);
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
    case kFilterHighPass:
      return high_pass_pixels(rgba, width, height, stride, settings.high_pass, scale);
    case kFilterMedian:
      return median_pixels(rgba, width, height, stride, settings.median.radius, scale, 0);
    case kFilterDustScratches:
      return median_pixels(rgba, width, height, stride, settings.dust_scratches.radius, scale,
                           settings.dust_scratches.threshold);
    case kFilterMinimum:
      return morphology_pixels(rgba, width, height, stride, settings.minimum.radius, scale, false);
    case kFilterMaximum:
      return morphology_pixels(rgba, width, height, stride, settings.maximum.radius, scale, true);
    case kFilterOffset:
      return offset_pixels(rgba, width, height, stride, settings.offset, scale);
    case kFilterCustom:
      /* 核没有连续的「半径」语义：预览不缩放核（见 filters.h 的边界说明）。 */
      return custom_pixels(rgba, width, height, stride, settings.custom);
    case kFilterSpherize:
      return spherize_pixels(rgba, width, height, stride, settings.spherize);
    case kFilterPinch:
      return pinch_pixels(rgba, width, height, stride, settings.pinch);
    case kFilterTwirl:
      return twirl_pixels(rgba, width, height, stride, settings.twirl);
    case kFilterMosaic:
      return mosaic_pixels(rgba, width, height, stride, settings.mosaic, scale);
    case kFilterEmboss:
      return emboss_pixels(rgba, width, height, stride, settings.emboss, scale);
    case kFilterClouds:
      return clouds_pixels(rgba, width, height, stride, settings.clouds, scale, origin_x,
                           origin_y);
    case kFilterDifferenceClouds:
      return difference_clouds_pixels(rgba, width, height, stride, settings.difference_clouds,
                                      scale, origin_x, origin_y);
    case kFilterPictureFrame:
      return picture_frame_pixels(rgba, width, height, stride, settings.picture_frame, scale);
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
    case kFilterHighPass: return high_pass_valid(settings.high_pass, error);
    case kFilterMedian: return denoise_valid(settings.median, 0, error);
    case kFilterDustScratches: return denoise_valid(settings.dust_scratches, 255, error);
    case kFilterMinimum: return minimum_valid(settings.minimum, error);
    case kFilterMaximum: return maximum_valid(settings.maximum, error);
    case kFilterOffset: return offset_valid(settings.offset, error);
    case kFilterCustom: return custom_valid(settings.custom, error);
    case kFilterSpherize: return spherize_valid(settings.spherize, error);
    case kFilterPinch: return pinch_valid(settings.pinch, error);
    case kFilterTwirl: return twirl_valid(settings.twirl, error);
    case kFilterMosaic: return mosaic_valid(settings.mosaic, error);
    case kFilterEmboss: return emboss_valid(settings.emboss, error);
    case kFilterClouds: return clouds_valid(settings.clouds, error);
    case kFilterDifferenceClouds: return difference_clouds_valid(settings.difference_clouds, error);
    case kFilterPictureFrame: return picture_frame_valid(settings.picture_frame, error);
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
    case kFilterHighPass: return settings.high_pass.radius == 0.0;
    case kFilterMedian: return settings.median.radius == 0.0;
    case kFilterDustScratches: return settings.dust_scratches.radius == 0.0;
    case kFilterMinimum: return settings.minimum.radius == 0.0;
    case kFilterMaximum: return settings.maximum.radius == 0.0;
    case kFilterOffset:
      return std::llround(settings.offset.horizontal) == 0 &&
             std::llround(settings.offset.vertical) == 0;
    case kFilterCustom: return custom_identity(settings.custom);
    case kFilterSpherize: return settings.spherize.amount == 0.0;
    case kFilterPinch: return settings.pinch.amount == 0.0;
    case kFilterTwirl: return settings.twirl.angle == 0.0;
    case kFilterMosaic: return settings.mosaic.cell_size == 0.0;
    case kFilterEmboss: return settings.emboss.amount == 0.0;
    case kFilterClouds: return settings.clouds.amount == 0.0;
    case kFilterDifferenceClouds: return settings.difference_clouds.amount == 0.0;
    case kFilterPictureFrame: return settings.picture_frame.width == 0.0;
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
    case kFilterHighPass: return a.high_pass.radius == b.high_pass.radius;
    case kFilterMedian:
      return a.median.radius == b.median.radius;
    case kFilterDustScratches:
      return a.dust_scratches.radius == b.dust_scratches.radius &&
             a.dust_scratches.threshold == b.dust_scratches.threshold;
    case kFilterMinimum: return a.minimum.radius == b.minimum.radius;
    case kFilterMaximum: return a.maximum.radius == b.maximum.radius;
    case kFilterOffset:
      return a.offset.horizontal == b.offset.horizontal &&
             a.offset.vertical == b.offset.vertical && a.offset.wrap == b.offset.wrap;
    case kFilterCustom: return custom_equal(a.custom, b.custom);
    case kFilterSpherize: return a.spherize.amount == b.spherize.amount;
    case kFilterPinch: return a.pinch.amount == b.pinch.amount;
    case kFilterTwirl: return a.twirl.angle == b.twirl.angle;
    case kFilterMosaic: return a.mosaic.cell_size == b.mosaic.cell_size;
    case kFilterEmboss:
      return a.emboss.amount == b.emboss.amount && a.emboss.angle == b.emboss.angle &&
             a.emboss.height == b.emboss.height;
    case kFilterClouds: {
      const CloudsSettings& x = a.clouds;
      const CloudsSettings& y = b.clouds;
      return x.amount == y.amount && x.size == y.size && x.seed == y.seed &&
             x.color_low[0] == y.color_low[0] && x.color_low[1] == y.color_low[1] &&
             x.color_low[2] == y.color_low[2] && x.color_high[0] == y.color_high[0] &&
             x.color_high[1] == y.color_high[1] && x.color_high[2] == y.color_high[2];
    }
    case kFilterDifferenceClouds:
      return a.difference_clouds.amount == b.difference_clouds.amount &&
             a.difference_clouds.size == b.difference_clouds.size &&
             a.difference_clouds.seed == b.difference_clouds.seed;
    case kFilterPictureFrame:
      return a.picture_frame.width == b.picture_frame.width &&
             a.picture_frame.color[0] == b.picture_frame.color[0] &&
             a.picture_frame.color[1] == b.picture_frame.color[1] &&
             a.picture_frame.color[2] == b.picture_frame.color[2];
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
    case kFilterHighPass: return "High Pass";
    case kFilterMinimum: return "Minimum";
    case kFilterMaximum: return "Maximum";
    case kFilterOffset: return "Offset";
    case kFilterCustom: return "Custom";
    case kFilterMedian: return "Median";
    case kFilterDustScratches: return "Dust & Scratches";
    case kFilterSpherize: return "Spherize";
    case kFilterPinch: return "Pinch";
    case kFilterTwirl: return "Twirl";
    case kFilterMosaic: return "Mosaic";
    case kFilterEmboss: return "Emboss";
    case kFilterClouds: return "Clouds";
    case kFilterDifferenceClouds: return "Difference Clouds";
    case kFilterPictureFrame: return "Picture Frame";
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
    if (settings.kind == kFilterHighPass) {
      *note += " as 128 + (src - blurred) over the shared box-blur approximation (sigma = radius * "
               "scale; alpha untouched, flat areas become mid-grey)";
    }
    if (settings.kind == kFilterMinimum || settings.kind == kFilterMaximum) {
      *note += settings.kind == kFilterMinimum
                   ? " as a square-window erosion over all four channels (windows clipped at the "
                     "layer edge, so alpha follows the shrinking shape)"
                   : " as a square-window dilation over all four channels (windows clipped at the "
                     "layer edge, so alpha follows the growing shape)";
    }
    if (settings.kind == kFilterOffset) {
      *note += " as an integer-pixel shift with ";
      *note += settings.offset.wrap == 0   ? "transparent uncovered areas"
               : settings.offset.wrap == 1 ? "edge extension in the uncovered areas"
                                           : "wrap-around in the uncovered areas";
    }
    if (settings.kind == kFilterCustom) {
      *note += " as a 5x5 convolution on straight colour with edge-clamped neighbours "
               "(alpha untouched; previews do not rescale the kernel)";
    }
    if (settings.kind == kFilterClouds || settings.kind == kFilterDifferenceClouds) {
      *note += " as a five-octave value-noise fractal (CIRandomGenerator's field is not public; "
               "the pattern is reproducible per seed but not pixel-identical)";
    }
    if (settings.kind == kFilterPictureFrame) {
      *note += " as a flat-colour ring of equal thickness (the upstream frame is a shaded picture "
               "frame; only the thickness and colour are matched)";
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
