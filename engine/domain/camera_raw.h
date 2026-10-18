// AI生成
/*
 * camera_raw.h — Camera Raw 滤镜：设置、JSON、曲线与完整管线（F4）。
 *
 * 语义唯一真源是上游：
 *   - `Compositor/Document/CameraRaw.swift`（设置结构、isValid / normalized、apply 管线）；
 *   - `Compositor/Document/CameraRawColor.swift`（曲线 / 混合器 / 颜色分级）；
 *   - `Compositor/Document/CameraRawDetailOptics.swift`（细节 / 光学）；
 *   - `Compositor/Document/CameraRawGeometryCalibration.swift`（几何 / 校准）；
 *   - `Compositor/Document/Curves.swift`（保形三次 Hermite 求值）。
 *
 * 分层：
 *   - `camera_raw_tables.{h,cpp}` 管实测表：解码、取表、stage 装配、合成、亮度；
 *   - 本文件管**设置**：结构、JSON、归一化、曲线 LUT、管线顺序、文档事务；
 *   - `engine/imaging/AdjustPixels.c` 已有机型承担全部像素运算，本文件不重写公式。
 *
 * 与上游的两类收敛（README §6.9 与交付报告里都有记录）：
 *   1. 上游 `normalized` 对越界值一律 clamp；本仓 JSON 解析对**标量越界与非有限值
 *      直接拒绝**（`COMP_ERR_INVALID_ARGUMENT`），但对上游本身属于“结构修复”的
 *      部分（曲线点排序/端点/间距、分隔线先后、去紫/去绿色相端点对调、
 *      点颜色上限 8 个）照旧修复。理由：越界数字几乎都是调用方算错，
 *      静默 clamp 会把错误藏进像素；结构修复则是上游文档自身的既有语义。
 *   2. 上游把预览参数（`clipping` / `sharpenMask` / `visualizePointColor`）混在
 *      `apply` 里；本仓不暴露它们——它们会把预览效果烘进图层像素。
 *
 * 像素口径与全部 kernel 一致：RGBA8 预乘、4 字节/像素、行优先、左上原点。
 * 15/16 位图层先落 8 位工作副本（与工具层同一约定）；HDR 图层明确拒绝。
 */
#ifndef COMPOSITOR_DOMAIN_CAMERA_RAW_H
#define COMPOSITOR_DOMAIN_CAMERA_RAW_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "domain/camera_raw_tables.h"
#include "domain/document.h"
#include "domain/layer.h"

namespace compositor {

/* 上游枚举的 rawValue 逐字保留，JSON 里就是这些字符串。 */
enum class CameraRawWhiteBalance { kCustom = 0, kAuto = 1 };
enum class CameraRawGlowStyle { kDiffusion = 0, kBloom = 1, kHalation = 2 };
enum class CameraRawVignetteStyle { kHighlightPriority = 0, kColorPriority = 1, kPaintOverlay = 2 };
enum class CameraRawUpright { kOff = 0, kGuided = 1 };
enum class CameraRawProjection { kPerspective = 0, kRectilinear = 1 };
enum class CameraRawProcess {
  kVersion1 = 1,
  kVersion2 = 2,
  kVersion3 = 3,
  kVersion4 = 4,
  kVersion5 = 5,
  kVersion6 = 6,
};

/* 曲线点，0..1；上游 `CurvePoint`。 */
struct CameraRawCurvePoint {
  double x = 0.0;
  double y = 0.0;
};

/*
 * 上游 `CurvesSettings.value(_:channel:0)` 的逐字移植：保形三次 Hermite。
 * 定义域与值域都是 0..1（上游在 0..255 域里算，两个轴同比例，结果等价）。
 * `points` 少于 2 个时返回 x（上游 `point()` 的 guard）。
 */
double camera_raw_curve_value(const std::vector<CameraRawCurvePoint>& points, double x);

/* 参数曲线 + 四条点曲线（上游 `CameraRawCurveSettings`）。 */
struct CameraRawCurveSettings {
  double shadows = 0.0;
  double darks = 0.0;
  double lights = 0.0;
  double highlights = 0.0;
  /* 分隔线，0..100；上游缺省 25 / 50 / 75。 */
  double shadow_split = 25.0;
  double dark_split = 50.0;
  double light_split = 75.0;
  double refine_saturation = 0.0;
  std::vector<CameraRawCurvePoint> rgb;
  std::vector<CameraRawCurvePoint> red;
  std::vector<CameraRawCurvePoint> green;
  std::vector<CameraRawCurvePoint> blue;

  CameraRawCurveSettings();

  bool adjusts() const;
  /* 分隔线仍在 Photoshop 实测位置：参数曲线走表，否则走逐像素曲线。 */
  bool has_measured_dividers() const;

  /* 上游 `parametric(_:)`：33 个锚点的折线，`strength = 1.66`。 */
  double parametric(double tone) const;
  /* 256 项 LUT，0..1；上游 `toneTable()` / `channelTable(_:)`。 */
  std::array<float, 256> tone_table() const;
  static std::array<float, 256> channel_table(const std::vector<CameraRawCurvePoint>& points);

  /* 结构修复（排序、端点、间距）；越界标量由解析器拒绝，不在这里 clamp。 */
  void normalize();
};

/* 点颜色（上游 `CameraRawPointColor`）。 */
struct CameraRawPointColor {
  double hue = 0.0;                /* 0..360 */
  double saturation = 0.0;         /* 0..1 */
  double luminance = 0.0;          /* 0..1 */
  double hue_shift = 0.0;          /* −100..100 */
  double saturation_shift = 0.0;   /* −100..100 */
  double luminance_shift = 0.0;    /* −100..100 */
  double hue_range = 30.0;         /* 5..180 */
  double saturation_range = 0.4;   /* 0.05..1 */
  double luminance_range = 0.4;    /* 0.05..1 */
  bool visualize = false;
};

/* 颜色混合器（上游 `CameraRawMixerSettings`）：八个色族的色相/饱和/明度。 */
struct CameraRawMixerSettings {
  std::array<double, 8> hue{};
  std::array<double, 8> saturation{};
  std::array<double, 8> luminance{};
  std::vector<CameraRawPointColor> points;

  bool adjusts() const;
  /* 24 个 Float：hue、saturation、luminance 各八个，除以 100。 */
  std::array<float, 24> mixer_floats() const;
  /* 每个点 9 个 Float；上游 `pointFloats`。 */
  std::vector<float> point_floats() const;
  void normalize();
};

/* 一个颜色轮（上游 `CameraRawGradeWheel`）。 */
struct CameraRawGradeWheel {
  double hue = 0.0;         /* 0..360 */
  double saturation = 0.0;  /* 0..100 */
  double luminance = 0.0;   /* −100..100 */
};

/* 颜色分级（上游 `CameraRawGradingSettings`）。 */
struct CameraRawGradingSettings {
  CameraRawGradeWheel shadows;
  CameraRawGradeWheel midtones;
  CameraRawGradeWheel highlights;
  CameraRawGradeWheel global;
  double blending = 50.0; /* 0..100 */
  double balance = 0.0;   /* −100..100 */

  bool adjusts() const;
  std::array<CameraRawGradeWheel, 4> wheels() const;
  void normalize();
};

/* 细节（上游 `CameraRawDetailSettings`）。 */
struct CameraRawDetailSettings {
  double sharpen_amount = 0.0;          /* 0..150 */
  double sharpen_radius = 10.0;         /* 0..100 */
  double sharpen_detail = 25.0;         /* 0..100 */
  double sharpen_masking = 0.0;         /* 0..100 */
  double noise_luminance = 0.0;         /* 0..100 */
  double noise_luminance_detail = 50.0; /* 0..100 */
  double noise_luminance_contrast = 0.0;/* 0..100 */
  double noise_color = 0.0;             /* 0..100 */
  double noise_color_detail = 50.0;     /* 0..100 */
  double noise_color_smoothness = 50.0; /* 0..100 */

  bool adjusts() const;
};

/* 光学（上游 `CameraRawOpticsSettings`）。 */
struct CameraRawOpticsSettings {
  bool remove_chromatic_aberration = false;
  bool enable_lens_profile = false;
  double profile_distortion = 100.0; /* 0..100 */
  double profile_vignetting = 100.0; /* 0..100 */
  double distortion = 0.0;           /* −100..100 */
  double purple_amount = 0.0;        /* 0..100 */
  double purple_hue_low = 270.0;     /* 0..360 */
  double purple_hue_high = 310.0;    /* 0..360 */
  double green_amount = 0.0;         /* 0..100 */
  double green_hue_low = 60.0;       /* 0..360 */
  double green_hue_high = 120.0;     /* 0..360 */
  double vignette_amount = 0.0;      /* −100..100 */
  double vignette_midpoint = 50.0;   /* 0..100 */

  bool adjusts() const;
  /* 上游 `distortionK(profileStrength:)`。 */
  double distortion_k(double profile_strength) const;
  void normalize();
};

/* 几何引导线，归一化图像坐标（上游 `CameraRawGeometryGuide`）。 */
struct CameraRawGeometryGuide {
  double start_x = 0.0;
  double start_y = 0.0;
  double end_x = 0.0;
  double end_y = 0.0;
};

/*
 * 几何（上游 `CameraRawGeometrySettings`）。F4 不做 Core Image 透视变换：
 * 非默认几何由管线返回 `COMP_ERR_UNSUPPORTED`，设置仍能被解析与回读。
 */
struct CameraRawGeometrySettings {
  CameraRawUpright upright = CameraRawUpright::kOff;
  CameraRawProjection projection = CameraRawProjection::kPerspective;
  double vertical = 0.0;  /* −100..100 */
  double horizontal = 0.0;/* −100..100 */
  double rotate = 0.0;    /* −45..45 */
  double aspect = 0.0;    /* −100..100 */
  double scale = 0.0;     /* −100..100 */
  double offset_x = 0.0;  /* −100..100 */
  double offset_y = 0.0;  /* −100..100 */
  bool constrain_crop = false;
  std::vector<CameraRawGeometryGuide> guides;

  bool adjusts() const;
  void normalize();
};

/* 校准（上游 `CameraRawCalibrationSettings`）。 */
struct CameraRawCalibrationSettings {
  CameraRawProcess process = CameraRawProcess::kVersion6;
  double shadow_tint = 0.0;
  double red_hue = 0.0;
  double red_saturation = 0.0;
  double green_hue = 0.0;
  double green_saturation = 0.0;
  double blue_hue = 0.0;
  double blue_saturation = 0.0;

  bool adjusts() const;
};

/* 完整设置（上游 `CameraRawSettings`，预览专用参数未暴露）。 */
struct CameraRawSettings {
  CameraRawWhiteBalance white_balance = CameraRawWhiteBalance::kCustom;
  double temperature = 0.0; /* −100..100；相对偏移，不是开尔文。 */
  double tint = 0.0;
  double exposure = 0.0;    /* −5..5，线性光档位。 */
  double contrast = 0.0;
  double highlights = 0.0;
  double shadows = 0.0;
  double whites = 0.0;
  double blacks = 0.0;
  double vibrance = 0.0;
  double saturation = 0.0;
  double texture = 0.0;
  double clarity = 0.0;
  double dehaze = 0.0;
  double glow = 0.0;        /* 0..100 */
  CameraRawGlowStyle glow_style = CameraRawGlowStyle::kDiffusion;
  double glow_range = 0.0;  /* −100..100 */
  double glow_spread = 0.0;
  double glow_warmth = 0.0;
  double vignette_amount = 0.0;   /* −100..100 */
  CameraRawVignetteStyle vignette_style = CameraRawVignetteStyle::kHighlightPriority;
  double vignette_midpoint = 50.0;/* 0..100 */
  double vignette_roundness = 0.0;/* −100..100 */
  double vignette_feather = 50.0; /* 0..100 */
  double vignette_highlights = 0.0;/* 0..100 */
  double grain_amount = 0.0;      /* 0..100 */
  double grain_size = 25.0;       /* 0..100 */
  double grain_roughness = 50.0;  /* 0..100 */
  CameraRawCurveSettings curve;
  CameraRawMixerSettings mixer;
  CameraRawGradingSettings grading;
  CameraRawDetailSettings detail;
  CameraRawOpticsSettings optics;
  CameraRawGeometrySettings geometry;
  CameraRawCalibrationSettings calibration;

  bool adjusts_light() const;
  bool adjusts_color() const;
  bool adjusts_effects() const;
  bool is_identity() const;
  /* 上游 `grainKernelSize`：0.5 + grainSize/100 × 19.5。 */
  double grain_kernel_size() const;
  /* 上游 `normalized()` 里本仓认可的结构修复部分。 */
  void normalize();
};

/*
 * 解析 JSON。字段名与上游 Codable 名逐字一致（camelCase）；未知键、类型不符、
 * 非有限数值、标量越界都返回 false 并写 `error`。缺省字段取上游缺省值。
 * 解析成功后设置已经过结构修复，可直接交给管线。
 */
bool camera_raw_settings_from_json(const std::string& text, CameraRawSettings* settings,
                                   std::string* error);

/*
 * 把设置归一化后按上游顺序拆成 stage（校准主色 → 曝光 → 白平衡 → 对比度 /
 * 高光 / 阴影 → 白/黑/饱和/自然饱和 → 参数曲线）。只返回非零项。
 */
std::vector<CameraRawStage> camera_raw_stages_for(const CameraRawSettings& settings,
                                                  const CameraRawTableSet& tables,
                                                  const CameraRawBrightness& brightness);

/* 颜色混合器 / 颜色分级各自的 stage（上游同名函数）。 */
std::vector<CameraRawStage> camera_raw_mixer_stages(const CameraRawMixerSettings& mixer,
                                                    const CameraRawTableSet& tables);
std::vector<CameraRawStage> camera_raw_grading_stages(const CameraRawGradingSettings& grading,
                                                      const CameraRawTableSet& tables);

/*
 * 曲线 + 混合器 + 分级折叠进合成表（上游 `foldingColor`）。`composed` 为空时
 * 从恒等网格开始；返回值总是合法网格。
 */
std::vector<float> camera_raw_folding_color(const CameraRawSettings& settings,
                                            const CameraRawTableSet& tables,
                                            std::vector<float> composed);

/* 逐像素曲线 + 点颜色（上游 `applyCurveColor`）。 */
void camera_raw_apply_curve_color(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                                  const CameraRawSettings& settings,
                                  const CameraRawTableSet& tables);

/*
 * 设置是否要求实测表：任何非恒等设置都要。表缺失时上游整条滤镜不可用，
 * 本仓返回 `COMP_ERR_STATE` 而不是悄悄降级成另一套观感。
 */
bool camera_raw_needs_tables(const CameraRawSettings& settings);

/*
 * 对一块 RGBA8 预乘像素跑完整管线（上游 `apply` 的像素部分）。设置必须已经
 * 解析/校验过；`scale` 是预览像素 / 图层像素（<= 0 视为 1），`seed` 供颗粒定相。
 * 几何非默认或表缺失时返回对应错误码，像素不被修改。
 */
comp_result camera_raw_apply_pixels(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                                    const CameraRawSettings& settings,
                                    const CameraRawTableSet* tables, double scale, uint32_t seed);

/*
 * 文档层入口：参数在事务开始前校验；一步撤销；成功时 `*out_changed = false`
 * 表示恒等设置（不开事务、不动像素）。`note` 追加一句实际走了哪条通路。
 * `scale` / `seed` 语义同 `camera_raw_apply_pixels`。
 */
bool camera_raw_apply(Document* document, comp_layer_id target,
                      const CameraRawSettings& settings, double scale, uint32_t seed,
                      comp_result* out_code, bool* out_changed, std::string* note,
                      std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_CAMERA_RAW_H */
