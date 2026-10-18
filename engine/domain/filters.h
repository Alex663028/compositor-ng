// AI生成
/*
 * filters.h — Filter 菜单的领域层（P1 滤镜批次）。
 *
 * 背景：Vignette / Tonal Contrast / Bloom / Lens Correction 在上游是
 * FilterSettings + PixelFilter.run 的一条通路（Filters.swift:63、:312-356）。
 * 这四个算子的像素内核在本仓早已就位（AdjustPixels.h 的
 * adjust_colored_vignette / adjust_tonal_contrast、LensPixels.h 的
 * lens_distort），缺的是**产品入口**：参数契约、与图层几何的装配、一步撤销、
 * 与选区的混合。本文件把那条通路收口成一个领域入口。
 *
 * 与工具层（tool_ops.h）的分工：
 *   - 工具层按「笔迹 / coverage」工作，一次调用通常落一笔；
 *   - 本层按「整层一次」工作，参数是面板上的滤镜设置，语义对齐上游
 *     `FilterSettings.normalized` 的量程。
 *
 * 三条口径（与 camera_raw / tool_ops 一致）：
 *   1. 写操作一步撤销：加入调用方已开启的显式事务，或自成一个隐式步；
 *   2. 参数闸在开事务之前：非法参数不写半成品、不进撤销栈；
 *   3. 恒等设置不碰像素、不开事务：out_changed = 0。
 *
 * 能力边界（显式记录，不静默降级）：
 *   - 16 位图层先落 8 位工作副本（`deep_to_rgba8`，与工具层同一约定），
 *     之后图层就是 8 位；HDR（RGBA16F）拒绝。上游对 Bloom 有 16 位通路
 *     （`runsAtSixteenBits`），本仓的 8 位内核做不到逐位对齐，如实记入 note。
 *   - 空图层不做「Vignette 自动铺一张画布大小的透明底」（上游 Filters.swift
 *     的 `fillsCanvas`）：本层要求图层已有像素，与工具层同一条拒绝。
 *   - 选区：非空选区时结果按 coverage 与原像素混合（上游 PixelFilter.run
 *     末尾 `PixelAdjust.blend`）。选区按**文档坐标**解释，图层像素的文档
 *     坐标 = `effective_bounds()` 原点 + 像素坐标（与合成/工具同款平移约定）。
 */
#ifndef COMPOSITOR_DOMAIN_FILTERS_H
#define COMPOSITOR_DOMAIN_FILTERS_H

#include <cstdint>
#include <string>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"

namespace compositor {

class Document;

/* ------------------------------------------------------------ 设置 */

/*
 * Vignette（上游 FilterSettings.vignette*）。
 *
 * `amount` 0..100：0 = 恒等（上游 Filters.swift:856 的 no-op 判据）；
 * `midpoint` 0..100（羽化起点，50 = 画面中部开始压暗）；
 * `roundness` -100..100（负数 = 方角，100 = 正圆）；
 * `feather` 0..100；`highlights` 0..100（高光保护：越亮越不吃暗角）；
 * `color` 为直通 sRGB 0..1 的边缘色（上游 AdjustmentColor）。
 *
 * 作用面：图层自身的像素范围（上游非空图层 `job.canvas == nil` 时
 * frame = extent），按椭圆/超椭圆落权重。
 */
struct VignetteSettings {
  double amount = 0.0;
  double midpoint = 50.0;
  double roundness = 100.0;
  double feather = 60.0;
  double highlights = 25.0;
  double color[3] = {0.0, 0.0, 0.0};
};

/*
 * Tonal Contrast（上游 FilterSettings.tonal*）。
 *
 * `radius` 是高斯 detail 半径（图层像素，1..100，预览再乘 scale）；
 * 三个 gain -100..100 分别给暗部 / 中间调 / 高光。`amount` / 50 是整体强度
 * （与内核 adjust_tonal_contrast 的 strength 一致）。
 *
 * 恒等判据与上游 Filters.swift:858 相同：amount 为 0，或三个 gain 全 0。
 */
struct TonalContrastSettings {
  double amount = 0.0;
  double radius = 16.0;
  double shadows = 40.0;
  double midtones = 60.0;
  double highlights = 30.0;
};

/*
 * Bloom / Glow（上游 FilterSettings.bloom*，CIBloom 的 intensity = amount/50）。
 *
 * `radius` 1..150 是高斯半径（图层像素，预览再乘 scale）。上游走 Core Image
 * 的 `CIBloom`，其内部流程不公开；本仓用一份**显式近似**：
 *   亮部提取（阈值 0.5，按 rec.709 亮度线性过渡）-> 预乘模糊 -> 加光。
 * 只加光、不改 alpha：图层轮廓与合成几何不变（与 tool_ops 的合成不变性同款）。
 */
struct BloomSettings {
  double amount = 0.0;
  double radius = 24.0;
};

/*
 * Lens Correction（上游 FilterSettings.distortion）。
 *
 * -100..100 的 Remove Distortion；内核 k = distortion / 100 * 0.35
 * （上游 `lensStrength`，Filters.swift:238）。0 = 恒等。
 */
struct LensCorrectionSettings {
  double distortion = 0.0;
};

/* 滤镜种类。数值与 ABI `comp_filter_kind` 一一对应（尾部追加，不改既有值）。 */
enum FilterKind {
  kFilterVignette = 0,
  kFilterTonalContrast = 1,
  kFilterBloom = 2,
  kFilterLensCorrection = 3,
};

/*
 * 一次滤镜调用。只有 `kind` 对应的那个子结构被读取；其余忽略。
 * 零初始化 = Vignette + amount 0 = 恒等。
 */
struct FilterSettings {
  FilterKind kind = kFilterVignette;
  VignetteSettings vignette;
  TonalContrastSettings tonal_contrast;
  BloomSettings bloom;
  LensCorrectionSettings lens_correction;
};

/* 参数闸：逐项检查量程（越界拒绝而不是静默 clamp，与 camera_raw 同规矩）。 */
bool filter_settings_valid(const FilterSettings& settings, std::string* error);

/* 恒等判据（不碰像素、不开事务）。 */
bool filter_settings_identity(const FilterSettings& settings);

/* 滤镜的稳定名字（事务标签 / note 用）；未知 kind 返回 "filter"。 */
const char* filter_kind_label(FilterKind kind);

/* ============================================================ 执行 */

/*
 * 对目标图层跑一次滤镜。成功返回 true。
 *
 * `scale` 是预览像素 / 图层像素（<= 0 视为 1）：只影响高斯半径（Tonal
 * Contrast / Bloom），让降采样预览看起来与全尺寸一致；几何类（Vignette /
 * Lens Correction）本身就相对图层尺寸，无需缩放。
 *
 * 错误码（本层直接回报，调用方不从文案反推）：
 *   设置越界 / 非有限 -> COMP_ERR_INVALID_ARGUMENT
 *   文档为空        -> COMP_ERR_INVALID_ARGUMENT
 *   图层不存在      -> COMP_ERR_NOT_FOUND
 *   组图层 / 无像素 / HDR -> COMP_ERR_STATE
 *   分配失败        -> COMP_ERR_OUT_OF_MEMORY
 *
 * `out_code` 成功时也写 COMP_OK（理由同 tool_ops.h 末尾）。
 * `out_changed` 可为 NULL。`note` 可空，回传非致命说明。
 */
bool filter_apply(Document* document, comp_layer_id target, const FilterSettings& settings,
                  double scale, comp_result* out_code, bool* out_changed, std::string* note,
                  std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_FILTERS_H */
