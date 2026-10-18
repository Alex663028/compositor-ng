// AI生成
/*
 * filters.cpp — Filter 菜单领域层实现（见 filters.h 的文件头）。
 *
 * 像素内核全部复用 engine/imaging：
 *   Vignette        AdjustPixels.h  adjust_colored_vignette
 *   Tonal Contrast  adjustment_eval.h blur_rgba8_copy + AdjustPixels.h adjust_tonal_contrast
 *   Bloom           本文件（Upstream 是 Core Image CIBloom，内部不公开；见 bloom_pixels）
 *   Lens Correction LensPixels.h    lens_distort
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

comp_result apply_pixels(uint8_t* rgba, int32_t width, int32_t height, size_t stride,
                         const FilterSettings& settings, double scale) {
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
  }
  return COMP_ERR_INVALID_ARGUMENT;
}

/*
 * 把滤镜结果按选区与原像素混合（上游 PixelFilter.run 末尾的
 * PixelAdjust.blend）。coverage 是**文档分辨率**，图层像素按
 * effective_bounds() 的原点平移到文档坐标（与合成/工具同款约定）。
 * 图层落在画布外的部分 coverage 视为 0（选区之外），保持原像素。
 */
void blend_through_selection(uint8_t* rgba, size_t stride, const uint8_t* original, int32_t width,
                             int32_t height, const std::vector<uint8_t>& coverage,
                             const comp_rect& bounds, int32_t canvas_width,
                             int32_t canvas_height) {
  if (coverage.empty() || canvas_width <= 0 || canvas_height <= 0) {
    /* 没有有效 coverage：整层保持原样（显式空选区在上游就是「什么都不改」）。 */
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(rgba + static_cast<size_t>(y) * stride,
                  original + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
    }
    return;
  }
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* before = original + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      const uint8_t* o = before + static_cast<size_t>(x) * 4;
      const int32_t doc_x = bounds.x + x;
      const int32_t doc_y = bounds.y + y;
      int32_t cov = 0;
      if (doc_x >= 0 && doc_y >= 0 && doc_x < canvas_width && doc_y < canvas_height) {
        cov = coverage[static_cast<size_t>(doc_y) * static_cast<size_t>(canvas_width) +
                       static_cast<size_t>(doc_x)];
      }
      if (cov == 0) {
        for (int c = 0; c < 4; ++c) p[c] = o[c];
      } else if (cov < 255) {
        for (int c = 0; c < 4; ++c) {
          const int blended = (p[c] * cov + o[c] * (255 - cov) + 127) / 255;
          p[c] = static_cast<uint8_t>(std::min(255, blended));
        }
      }
    }
  }
}

}  // namespace

/* ============================================================ 公共接口 */

bool filter_settings_valid(const FilterSettings& settings, std::string* error) {
  switch (settings.kind) {
    case kFilterVignette: return vignette_valid(settings.vignette, error);
    case kFilterTonalContrast: return tonal_valid(settings.tonal_contrast, error);
    case kFilterBloom: return bloom_valid(settings.bloom, error);
    case kFilterLensCorrection: return lens_valid(settings.lens_correction, error);
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
  }
  return true;
}

const char* filter_kind_label(FilterKind kind) {
  switch (kind) {
    case kFilterVignette: return "Vignette";
    case kFilterTonalContrast: return "Tonal Contrast";
    case kFilterBloom: return "Bloom";
    case kFilterLensCorrection: return "Lens Correction";
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
      status = apply_pixels(pixels, layer->pixels.width, layer->pixels.height,
                            layer->pixels.stride(), settings, effective_scale);
      if (status != COMP_OK) {
        failure = status == COMP_ERR_OUT_OF_MEMORY ? "filter ran out of memory"
                                                   : "filter could not apply the settings";
      } else if (clip) {
        std::vector<uint8_t> coverage;
        selection_effective_coverage(selection, &coverage);
        blend_through_selection(pixels, layer->pixels.stride(), original.data(),
                                layer->pixels.width, layer->pixels.height, coverage,
                                layer->effective_bounds(), document->state().width,
                                document->state().height);
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
    if (clip) *note += ", clipped to the document selection";
  }
  return true;
}

}  // namespace compositor
