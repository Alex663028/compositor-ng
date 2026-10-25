// AI生成
/*
 * adjustment_table.cpp — 调整图层参数键名表与校验（键名逐字对齐上游 LayerAdjustment）。
 *
 * 取值范围与缺省值取自上游：
 *   ExposureSettings.exposureRange -20...20 / offsetRange -0.5...0.5 / gammaRange 0.01...9.99
 *   GradientMapSettings 颜色分量 0...1
 *   GrainSettings amountRange 0...100 / sizeRange 0.5...20 / roughnessRange 0...100
 *   BlackWhiteSettings.range -200...300（tintHue 0...360 / tintSaturation 0...100）
 *   ColorBalanceSettings.range -100...100
 *   LayerAdjustment.gaussianRadius 0.1...250 / motionAngle -90...90 / motionDistance 1...2000
 *   LayerAdjustment.resolvedNoiseAmount 0.1...400
 *   LayerAdjustment.hue |<=360| / saturation |<=100| / lightness |<=100|
 */
#include "domain/adjustment_table.h"

#include <cmath>
#include <cstring>

#include "domain/curves_settings.h"
#include "domain/settings_json_util.h"
#include "format/comp_json.h"

namespace compositor {
namespace {

/* 路径工具与填充图层共用（见 domain/settings_json_util.h）。 */
using settings_json::locate_leaf;
using settings_json::number_to_text;
using settings_json::parse_bool_text;
using settings_json::segment_index;
using settings_json::set_path;
using settings_json::split_path;

#define COMP_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

const char* const kNoRoots = "";

/* 0 Hue/Saturation */
const AdjustmentParam kHsvParams[] = {
    {"hue", "Hue", kAdjustmentParamNumber, -360.0, 360.0, 0.0},
    {"saturation", "Saturation", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"lightness", "Lightness", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorize", "Colorize", kAdjustmentParamBool, 0.0, 1.0, 0.0},
};

/* 1 Levels：四个通道（RGB 合成 + 红/绿/蓝）各 5 个 LevelRange 字段。
 *
 * `ranges` 是数组，json_path 因此带数字段（`levels.ranges.1.black`）。数字段按数组下标
 * 解析（见 locate_leaf / set_path），数组下标语义与上游 `LevelsSettings.ranges` 一致。
 * 量程取自上游 `LevelRange.normalized`：black 0..254、white 1..255、gamma 0.1..9.99、
 * 输出两端 0..255。white 与 black 的相对顺序由引擎求值侧 normalize() 保证（同上游）。
 */
const AdjustmentParam kLevelsParams[] = {
    {"levels.ranges.0.black", "RGB Black", kAdjustmentParamNumber, 0.0, 254.0, 0.0},
    {"levels.ranges.0.gamma", "RGB Gamma", kAdjustmentParamNumber, 0.1, 9.99, 1.0},
    {"levels.ranges.0.white", "RGB White", kAdjustmentParamNumber, 1.0, 255.0, 255.0},
    {"levels.ranges.0.outputBlack", "RGB OutLo", kAdjustmentParamNumber, 0.0, 255.0, 0.0},
    {"levels.ranges.0.outputWhite", "RGB OutHi", kAdjustmentParamNumber, 0.0, 255.0, 255.0},
    {"levels.ranges.1.black", "R Black", kAdjustmentParamNumber, 0.0, 254.0, 0.0},
    {"levels.ranges.1.gamma", "R Gamma", kAdjustmentParamNumber, 0.1, 9.99, 1.0},
    {"levels.ranges.1.white", "R White", kAdjustmentParamNumber, 1.0, 255.0, 255.0},
    {"levels.ranges.1.outputBlack", "R OutLo", kAdjustmentParamNumber, 0.0, 255.0, 0.0},
    {"levels.ranges.1.outputWhite", "R OutHi", kAdjustmentParamNumber, 0.0, 255.0, 255.0},
    {"levels.ranges.2.black", "G Black", kAdjustmentParamNumber, 0.0, 254.0, 0.0},
    {"levels.ranges.2.gamma", "G Gamma", kAdjustmentParamNumber, 0.1, 9.99, 1.0},
    {"levels.ranges.2.white", "G White", kAdjustmentParamNumber, 1.0, 255.0, 255.0},
    {"levels.ranges.2.outputBlack", "G OutLo", kAdjustmentParamNumber, 0.0, 255.0, 0.0},
    {"levels.ranges.2.outputWhite", "G OutHi", kAdjustmentParamNumber, 0.0, 255.0, 255.0},
    {"levels.ranges.3.black", "B Black", kAdjustmentParamNumber, 0.0, 254.0, 0.0},
    {"levels.ranges.3.gamma", "B Gamma", kAdjustmentParamNumber, 0.1, 9.99, 1.0},
    {"levels.ranges.3.white", "B White", kAdjustmentParamNumber, 1.0, 255.0, 255.0},
    {"levels.ranges.3.outputBlack", "B OutLo", kAdjustmentParamNumber, 0.0, 255.0, 0.0},
    {"levels.ranges.3.outputWhite", "B OutHi", kAdjustmentParamNumber, 0.0, 255.0, 255.0},
};

/* 无面板参数的 kind（Invert）共用空表。 */
const AdjustmentParam kNoParams[] = {};

/* 3 Exposure */
const AdjustmentParam kExposureParams[] = {
    {"exposureSettings.exposure", "Exposure", kAdjustmentParamNumber, -20.0, 20.0, 0.0},
    {"exposureSettings.offset", "Offset", kAdjustmentParamNumber, -0.5, 0.5, 0.0},
    {"exposureSettings.gamma", "Gamma", kAdjustmentParamNumber, 0.01, 9.99, 1.0},
};

/* 4 Gradient Map */
const AdjustmentParam kGradientMapParams[] = {
    {"gradientMapSettings.shadows.red", "Shadow R", kAdjustmentParamNumber, 0.0, 1.0, 0.0},
    {"gradientMapSettings.shadows.green", "Shadow G", kAdjustmentParamNumber, 0.0, 1.0, 0.0},
    {"gradientMapSettings.shadows.blue", "Shadow B", kAdjustmentParamNumber, 0.0, 1.0, 0.0},
    {"gradientMapSettings.highlights.red", "Highlight R", kAdjustmentParamNumber, 0.0, 1.0, 1.0},
    {"gradientMapSettings.highlights.green", "Highlight G", kAdjustmentParamNumber, 0.0, 1.0, 1.0},
    {"gradientMapSettings.highlights.blue", "Highlight B", kAdjustmentParamNumber, 0.0, 1.0, 1.0},
    {"gradientMapSettings.reversed", "Reversed", kAdjustmentParamBool, 0.0, 1.0, 0.0},
};

/* 5 Grain */
const AdjustmentParam kGrainParams[] = {
    {"grainSettings.amount", "Amount", kAdjustmentParamNumber, 0.0, 100.0, 25.0},
    {"grainSettings.size", "Size", kAdjustmentParamNumber, 0.5, 20.0, 1.5},
    {"grainSettings.roughness", "Roughness", kAdjustmentParamNumber, 0.0, 100.0, 50.0},
    {"grainSettings.seed", "Seed", kAdjustmentParamNumber, 0.0, 4294967295.0, 0.0},
};

/* 6 Add Noise（上游为 LayerAdjustment 顶层可选字段） */
const AdjustmentParam kAddNoiseParams[] = {
    {"noiseAmount", "Amount", kAdjustmentParamNumber, 0.1, 400.0, 10.0},
    {"noiseGaussian", "Gaussian", kAdjustmentParamBool, 0.0, 1.0, 0.0},
    {"noiseMonochromatic", "Monochromatic", kAdjustmentParamBool, 0.0, 1.0, 0.0},
    {"noiseSeed", "Seed", kAdjustmentParamNumber, 0.0, 4294967295.0, 0.0},
};

/* 7 Gaussian Blur（上游 LayerAdjustment.blurRadius） */
const AdjustmentParam kGaussianBlurParams[] = {
    {"blurRadius", "Radius", kAdjustmentParamNumber, 0.1, 250.0, 10.0},
};

/* 8 Motion Blur（上游 motionAngle / motionDistance） */
const AdjustmentParam kMotionBlurParams[] = {
    {"motionAngle", "Angle", kAdjustmentParamNumber, -90.0, 90.0, 0.0},
    {"motionDistance", "Distance", kAdjustmentParamNumber, 1.0, 2000.0, 10.0},
};

/* 10 Black & White */
const AdjustmentParam kBlackWhiteParams[] = {
    {"blackWhiteSettings.reds", "Reds", kAdjustmentParamNumber, -200.0, 300.0, 40.0},
    {"blackWhiteSettings.yellows", "Yellows", kAdjustmentParamNumber, -200.0, 300.0, 60.0},
    {"blackWhiteSettings.greens", "Greens", kAdjustmentParamNumber, -200.0, 300.0, 40.0},
    {"blackWhiteSettings.cyans", "Cyans", kAdjustmentParamNumber, -200.0, 300.0, 60.0},
    {"blackWhiteSettings.blues", "Blues", kAdjustmentParamNumber, -200.0, 300.0, 20.0},
    {"blackWhiteSettings.magentas", "Magentas", kAdjustmentParamNumber, -200.0, 300.0, 80.0},
    {"blackWhiteSettings.tint", "Tint", kAdjustmentParamBool, 0.0, 1.0, 0.0},
    {"blackWhiteSettings.tintHue", "Tint Hue", kAdjustmentParamNumber, 0.0, 360.0, 40.0},
    {"blackWhiteSettings.tintSaturation", "Tint Sat", kAdjustmentParamNumber, 0.0, 100.0, 20.0},
};

/* 11 Color Balance */
const AdjustmentParam kColorBalanceParams[] = {
    {"colorBalanceSettings.shadowCyanRed", "Shadow C/R", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.shadowMagentaGreen", "Shadow M/G", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.shadowYellowBlue", "Shadow Y/B", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.midCyanRed", "Mid C/R", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.midMagentaGreen", "Mid M/G", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.midYellowBlue", "Mid Y/B", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.highlightCyanRed", "High C/R", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.highlightMagentaGreen", "High M/G", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.highlightYellowBlue", "High Y/B", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"colorBalanceSettings.preserveLuminosity", "Preserve Lum", kAdjustmentParamBool, 0.0, 1.0, 1.0},
};

/*
 * 12..15 是本仓 S27 追加的破坏性调整，键名沿上游 `XxxSettings` 的命名风格自定；
 * 它们不参与 .comp ← 上游的 kind 互操作，但作为调整图层写入 .comp 时同样合法。
 */

/* 12 Brightness/Contrast（Photoshop 旧版公式，见 AdjustPixels.c） */
const AdjustmentParam kBrightnessContrastParams[] = {
    {"brightnessContrastSettings.brightness", "Brightness", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"brightnessContrastSettings.contrast", "Contrast", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
};

/* 13 Desaturate：无参数（等价于红/绿/蓝混合器全 0）。 */
const AdjustmentParam kDesaturateParams[] = {};

/* 14 Threshold */
const AdjustmentParam kThresholdParams[] = {
    {"thresholdSettings.level", "Level", kAdjustmentParamNumber, 0.0, 255.0, 128.0},
};

/* 15 Posterize：级数 2..255，255 即恒等。 */
const AdjustmentParam kPosterizeParams[] = {
    {"posterizeSettings.levels", "Levels", kAdjustmentParamNumber, 2.0, 255.0, 4.0},
};

/*
 * 16..24：把 Photoshop「Image > Adjustments」的其余条目补齐。键名仍沿 `XxxSettings`
 * 的命名风格自定，因此同样只在本仓内往返，不参与上游 kind 白名单互操作。
 */

/* 16 Vibrance（自然饱和度）。 */
const AdjustmentParam kVibranceParams[] = {
    {"vibranceSettings.vibrance", "Vibrance", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"vibranceSettings.saturation", "Saturation", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
};

/*
 * 17 Selective Color（可选颜色）：9 个色系 × 青/洋红/黄/黑。`relative` 与 PS 一样放在
 * 首位；36 个滑块在面板里滚动显示（PS 用色系下拉，本仓面板没有下拉控件）。
 */
const AdjustmentParam kSelectiveColorParams[] = {
    {"selectiveColorSettings.relative", "Relative", kAdjustmentParamBool, 0.0, 1.0, 0.0},
    {"selectiveColorSettings.ranges.0.cyan", "Reds C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.0.magenta", "Reds M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.0.yellow", "Reds Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.0.black", "Reds K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.1.cyan", "Yellows C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.1.magenta", "Yellows M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.1.yellow", "Yellows Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.1.black", "Yellows K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.2.cyan", "Greens C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.2.magenta", "Greens M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.2.yellow", "Greens Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.2.black", "Greens K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.3.cyan", "Cyans C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.3.magenta", "Cyans M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.3.yellow", "Cyans Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.3.black", "Cyans K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.4.cyan", "Blues C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.4.magenta", "Blues M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.4.yellow", "Blues Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.4.black", "Blues K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.5.cyan", "Magentas C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.5.magenta", "Magentas M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.5.yellow", "Magentas Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.5.black", "Magentas K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.6.cyan", "Whites C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.6.magenta", "Whites M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.6.yellow", "Whites Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.6.black", "Whites K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.7.cyan", "Neutrals C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.7.magenta", "Neutrals M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.7.yellow", "Neutrals Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.7.black", "Neutrals K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.8.cyan", "Blacks C", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.8.magenta", "Blacks M", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.8.yellow", "Blacks Y", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"selectiveColorSettings.ranges.8.black", "Blacks K", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
};

/* 18 Photo Filter（照片滤镜）。颜色分量 0…1，密度 0…100（PS 同量程）。 */
const AdjustmentParam kPhotoFilterParams[] = {
    {"photoFilterSettings.color.red", "Filter R", kAdjustmentParamNumber, 0.0, 1.0, 0.93},
    {"photoFilterSettings.color.green", "Filter G", kAdjustmentParamNumber, 0.0, 1.0, 0.66},
    {"photoFilterSettings.color.blue", "Filter B", kAdjustmentParamNumber, 0.0, 1.0, 0.35},
    {"photoFilterSettings.density", "Density", kAdjustmentParamNumber, 0.0, 100.0, 25.0},
    {"photoFilterSettings.preserveLuminosity", "Preserve Lum", kAdjustmentParamBool, 0.0, 1.0, 1.0},
};

/* 19 Channel Mixer（通道混合器）：3 个输出行 × (R, G, B, 常数)，PS 的 −200%…200%。 */
const AdjustmentParam kChannelMixerParams[] = {
    {"channelMixerSettings.redRed", "Red: R", kAdjustmentParamNumber, -2.0, 2.0, 1.0},
    {"channelMixerSettings.redGreen", "Red: G", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.redBlue", "Red: B", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.redConstant", "Red: C", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.greenRed", "Green: R", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.greenGreen", "Green: G", kAdjustmentParamNumber, -2.0, 2.0, 1.0},
    {"channelMixerSettings.greenBlue", "Green: B", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.greenConstant", "Green: C", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.blueRed", "Blue: R", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.blueGreen", "Blue: G", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
    {"channelMixerSettings.blueBlue", "Blue: B", kAdjustmentParamNumber, -2.0, 2.0, 1.0},
    {"channelMixerSettings.blueConstant", "Blue: C", kAdjustmentParamNumber, -2.0, 2.0, 0.0},
};

/* 20 Color Lookup（查找颜色）：只做内置外观，`look` 是外观序号，`.cube` 文件不读。 */
const AdjustmentParam kColorLookupParams[] = {
    {"colorLookupSettings.look", "Look", kAdjustmentParamNumber, 0.0, 4.0, 0.0},
    {"colorLookupSettings.intensity", "Intensity", kAdjustmentParamNumber, 0.0, 100.0, 100.0},
};

/* 21 Shadows/Highlights（阴影/高光）：两个量、两个色调宽度、一个半径。 */
const AdjustmentParam kShadowsHighlightsParams[] = {
    {"shadowsHighlightsSettings.shadows", "Shadows", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"shadowsHighlightsSettings.shadowWidth", "Shadow Width", kAdjustmentParamNumber, 0.0, 100.0, 50.0},
    {"shadowsHighlightsSettings.highlights", "Highlights", kAdjustmentParamNumber, -100.0, 100.0, 0.0},
    {"shadowsHighlightsSettings.highlightWidth", "Highlight Width", kAdjustmentParamNumber, 0.0, 100.0, 50.0},
    {"shadowsHighlightsSettings.radius", "Radius", kAdjustmentParamNumber, 1.0, 100.0, 30.0},
};

/* 22..24 Auto Tone / Contrast / Color：读图自算，没有参数（与去色同一类「点即应用」）。 */

/* kind 顺序必须与上游 `AdjustmentKind` 的 CaseIterable 声明顺序一致（.comp 的 kind 即序号）。 */
const AdjustmentKindInfo kKinds[kAdjustmentKindCount] = {
    {0, "Hue/Saturation", "Hue, saturation and lightness shifts", kHsvParams,
     COMP_ARRAY_LEN(kHsvParams), true, "hsvSettings"},
    {1, "Levels", "Input and output levels per channel", kLevelsParams,
     COMP_ARRAY_LEN(kLevelsParams), true, kNoRoots},
    {2, "Curves", "Tone curve control points per channel", kNoParams, 0, false, "curves"},
    {3, "Exposure", "Exposure, offset and gamma", kExposureParams,
     COMP_ARRAY_LEN(kExposureParams), true, kNoRoots},
    {4, "Gradient Map", "Map shadows and highlights to colors", kGradientMapParams,
     COMP_ARRAY_LEN(kGradientMapParams), true, kNoRoots},
    {5, "Grain", "Film grain amount, size and roughness", kGrainParams,
     COMP_ARRAY_LEN(kGrainParams), true, kNoRoots},
    {6, "Add Noise", "Uniform or gaussian noise", kAddNoiseParams,
     COMP_ARRAY_LEN(kAddNoiseParams), true, kNoRoots},
    {7, "Gaussian Blur", "Gaussian blur radius", kGaussianBlurParams,
     COMP_ARRAY_LEN(kGaussianBlurParams), true, kNoRoots},
    {8, "Motion Blur", "Motion blur angle and distance", kMotionBlurParams,
     COMP_ARRAY_LEN(kMotionBlurParams), true, kNoRoots},
    {9, "Invert", "Invert has no settings", kNoParams, 0, false, kNoRoots},
    {10, "Black & White", "Channel weights for the gray conversion", kBlackWhiteParams,
     COMP_ARRAY_LEN(kBlackWhiteParams), true, kNoRoots},
    {11, "Color Balance", "Color balance for shadows, midtones and highlights",
     kColorBalanceParams, COMP_ARRAY_LEN(kColorBalanceParams), true, kNoRoots},
    {12, "Brightness/Contrast", "Legacy brightness and contrast", kBrightnessContrastParams,
     COMP_ARRAY_LEN(kBrightnessContrastParams), true, kNoRoots},
    {13, "Desaturate", "Desaturate has no settings", kDesaturateParams,
     COMP_ARRAY_LEN(kDesaturateParams), false, kNoRoots},
    {14, "Threshold", "Split the image into black and white at a level", kThresholdParams,
     COMP_ARRAY_LEN(kThresholdParams), true, kNoRoots},
    {15, "Posterize", "Reduce the number of tonal levels per channel", kPosterizeParams,
     COMP_ARRAY_LEN(kPosterizeParams), true, kNoRoots},
    {16, "Vibrance", "Saturation that favors the quiet colors", kVibranceParams,
     COMP_ARRAY_LEN(kVibranceParams), true, kNoRoots},
    {17, "Selective Color", "Cyan/magenta/yellow/black per color family", kSelectiveColorParams,
     COMP_ARRAY_LEN(kSelectiveColorParams), true, kNoRoots},
    {18, "Photo Filter", "A colored filter in front of the lens", kPhotoFilterParams,
     COMP_ARRAY_LEN(kPhotoFilterParams), true, kNoRoots},
    {19, "Channel Mixer", "Each output channel from a mix of the inputs", kChannelMixerParams,
     COMP_ARRAY_LEN(kChannelMixerParams), true, kNoRoots},
    {20, "Color Lookup", "One of the built-in looks, blended by intensity", kColorLookupParams,
     COMP_ARRAY_LEN(kColorLookupParams), true, kNoRoots},
    {21, "Shadows/Highlights", "Open the shadows and recover the highlights", kShadowsHighlightsParams,
     COMP_ARRAY_LEN(kShadowsHighlightsParams), true, kNoRoots},
    {22, "Auto Tone", "Stretch each channel on the image's own range", kNoParams, 0, false, kNoRoots},
    {23, "Auto Contrast", "Stretch all channels on the luminance range", kNoParams, 0, false, kNoRoots},
    {24, "Auto Color", "Auto Tone plus a gray-world cast removal", kNoParams, 0, false, kNoRoots},
};

/* 顶层键白名单 = 各参数路径的根段 + extra_roots。 */
bool root_allowed(const AdjustmentKindInfo& info, const std::string& key) {
  for (size_t i = 0; i < info.param_count; ++i) {
    const char* path = info.params[i].json_path;
    const char* dot = std::strchr(path, '.');
    const size_t length = dot != nullptr ? static_cast<size_t>(dot - path) : std::strlen(path);
    if (key.size() == length && key.compare(0, length, path, length) == 0) return true;
  }
  if (info.extra_roots != nullptr && info.extra_roots[0] != '\0') {
    size_t start = 0;
    const std::string roots(info.extra_roots);
    while (start < roots.size()) {
      const size_t space = roots.find(' ', start);
      const std::string token = roots.substr(start, space == std::string::npos ? std::string::npos
                                                                              : space - start);
      if (!token.empty() && token == key) return true;
      if (space == std::string::npos) break;
      start = space + 1;
    }
  }
  return false;
}

/*
 * 由参数表构造完整的缺省 settings。
 *
 * 每个参数的叶子经 set_path 深合并进 root：同一顶层段（如 gradientMapSettings）下的
 * 多个参数会逐叶写入既有子树，而不是只有第一个参数存活。
 * 单段路径（如 blurRadius / hue）由 set_path 的 index+1==size 分支落在顶层。
 */
JsonValue build_defaults(const AdjustmentKindInfo& info) {
  JsonValue root = JsonValue::object({});
  for (size_t i = 0; i < info.param_count; ++i) {
    const AdjustmentParam& param = info.params[i];
    const std::vector<std::string> segments = split_path(param.json_path);
    JsonValue leaf = param.type == kAdjustmentParamBool
                         ? JsonValue::boolean(param.default_value != 0.0)
                         : JsonValue::number(param.default_value);
    root = set_path(root, segments, 0, std::move(leaf));
  }
  return root;
}

}  // namespace

const AdjustmentKindInfo* adjustment_kind_info(uint32_t kind) {
  if (kind >= kAdjustmentKindCount) return nullptr;
  return &kKinds[kind];
}

bool adjustment_kind_is_destructive(uint32_t kind) {
  switch (kind) {
    case 0:   /* Hue/Saturation */
    case 1:   /* Levels */
    case 2:   /* Curves */
    case 3:   /* Exposure */
    case 4:   /* Gradient Map */
    case 10:  /* Black & White */
    case 11:  /* Color Balance */
    case 12:  /* Brightness/Contrast */
    case 13:  /* Desaturate */
    case 14:  /* Threshold */
    case 15:  /* Posterize */
    case 16:  /* Vibrance */
    case 17:  /* Selective Color */
    case 18:  /* Photo Filter */
    case 19:  /* Channel Mixer */
    case 20:  /* Color Lookup */
    case 21:  /* Shadows/Highlights */
    case 22:  /* Auto Tone */
    case 23:  /* Auto Contrast */
    case 24:  /* Auto Color */
      return true;
    default:
      /*
       * 5..8（Grain / Add Noise / Gaussian Blur / Motion Blur）需要邻域或随机源，
       * 不是逐像素映射，破坏性入口一律拒绝，调用方应走 `comp_filter_apply`；
       * 9 Invert 有专用入口 `comp_layer_invert`，避免同一语义两条实现。
       */
      return false;
  }
}

bool adjustment_validate_settings(uint32_t kind, const std::string& settings_json, std::string* error) {
  const AdjustmentKindInfo* info = adjustment_kind_info(kind);
  if (info == nullptr) {
    if (error != nullptr) *error = "unknown adjustment kind";
    return false;
  }
  const std::string text = settings_json.empty() ? std::string("{}") : settings_json;

  std::string parse_error;
  const JsonValue root = json_parse(text, &parse_error);
  if (root.is_null() && !parse_error.empty()) {
    if (error != nullptr) *error = "invalid settings json: " + parse_error;
    return false;
  }
  if (!root.is_object()) {
    if (error != nullptr) *error = "adjustment settings must be a json object";
    return false;
  }

  for (const auto& entry : root.as_object()) {
    if (!root_allowed(*info, entry.first)) {
      if (error != nullptr) *error = "unknown settings key: " + entry.first;
      return false;
    }
  }

  /* Levels / Curves 只做浅层对象校验（结构复杂，需专用编辑器）。 */
  if (info->extra_roots != nullptr && info->extra_roots[0] != '\0') {
    size_t start = 0;
    const std::string roots(info->extra_roots);
    while (start < roots.size()) {
      const size_t space = roots.find(' ', start);
      const std::string token =
          roots.substr(start, space == std::string::npos ? std::string::npos : space - start);
      if (!token.empty()) {
        const JsonValue* value = root.find(token);
        if (value != nullptr && !value->is_object() && !value->is_null()) {
          if (error != nullptr) *error = token + " must be a json object";
          return false;
        }
      }
      if (space == std::string::npos) break;
      start = space + 1;
    }
  }

  /* Curves 的控制点数组表达不进静态参数表，交给专用校验。 */
  if (kind == 2u && !curves_validate_settings(text, error)) return false;

  for (size_t i = 0; i < info->param_count; ++i) {
    const AdjustmentParam& param = info->params[i];
    bool malformed = false;
    const JsonValue* leaf = locate_leaf(root, param.json_path, &malformed);
    if (malformed) {
      if (error != nullptr) *error = std::string("settings path is not an object: ") + param.json_path;
      return false;
    }
    if (leaf == nullptr) continue;
    if (param.type == kAdjustmentParamBool) {
      if (!leaf->is_bool()) {
        if (error != nullptr) *error = std::string(param.json_path) + " must be a boolean";
        return false;
      }
      continue;
    }
    if (!leaf->is_number()) {
      if (error != nullptr) *error = std::string(param.json_path) + " must be a number";
      return false;
    }
    const double value = leaf->as_number();
    if (!std::isfinite(value) || value < param.min_value || value > param.max_value) {
      if (error != nullptr) {
        *error = std::string(param.json_path) + " out of range [" + number_to_text(param.min_value) +
                 ", " + number_to_text(param.max_value) + "]: " + number_to_text(value);
      }
      return false;
    }
  }
  return true;
}

std::string adjustment_default_settings_json(uint32_t kind) {
  const AdjustmentKindInfo* info = adjustment_kind_info(kind);
  if (info == nullptr || info->param_count == 0) return "{}";
  return json_dump(build_defaults(*info), 0);
}

bool adjustment_read_value(const std::string& settings_json, const char* json_path,
                          double* out_number, int32_t* out_boolean) {
  if (json_path == nullptr) return false;
  std::string parse_error;
  const JsonValue root = json_parse(settings_json.empty() ? std::string("{}") : settings_json,
                                    &parse_error);
  if (!root.is_object()) return false;
  bool malformed = false;
  const JsonValue* leaf = locate_leaf(root, json_path, &malformed);
  if (leaf == nullptr || malformed) return false;
  if (leaf->is_number()) {
    if (out_number != nullptr) *out_number = leaf->as_number();
    return true;
  }
  if (leaf->is_bool()) {
    if (out_boolean != nullptr) *out_boolean = leaf->as_bool() ? 1 : 0;
    return true;
  }
  /* 对象/数组叶子不视为可回填的标量。 */
  return false;
}

bool adjustment_build_settings(uint32_t kind, const std::vector<std::string>& values,
                              std::string* out_json, std::string* error) {
  const AdjustmentKindInfo* info = adjustment_kind_info(kind);
  if (info == nullptr) {
    if (error != nullptr) *error = "unknown adjustment kind";
    return false;
  }
  if (values.size() != info->param_count) {
    if (error != nullptr) *error = "value count does not match parameter count";
    return false;
  }

  JsonValue root = build_defaults(*info);
  for (size_t i = 0; i < info->param_count; ++i) {
    const AdjustmentParam& param = info->params[i];
    if (values[i].empty()) continue; /* 空串 = 沿用缺省值，不写入该键。 */
    JsonValue leaf;
    if (param.type == kAdjustmentParamBool) {
      bool truthy = false;
      if (!parse_bool_text(values[i], &truthy)) {
        if (error != nullptr) *error = std::string(param.json_path) + ": expected boolean text";
        return false;
      }
      leaf = JsonValue::boolean(truthy);
    } else {
      const char* begin = values[i].c_str();
      char* end = nullptr;
      const double number = std::strtod(begin, &end);
      if (end == begin || end == nullptr || *end != '\0' || !std::isfinite(number)) {
        if (error != nullptr) *error = std::string(param.json_path) + ": expected numeric text";
        return false;
      }
      if (number < param.min_value || number > param.max_value) {
        if (error != nullptr) {
          *error = std::string(param.json_path) + " out of range [" +
                   number_to_text(param.min_value) + ", " + number_to_text(param.max_value) + "]";
        }
        return false;
      }
      leaf = JsonValue::number(number);
    }
    root = set_path(root, split_path(param.json_path), 0, std::move(leaf));
  }

  const std::string dumped = json_dump(root, 0);
  /* 自检：构造结果必须能通过同一套校验，避免面板造出引擎拒收的 settings。 */
  if (!adjustment_validate_settings(kind, dumped, error)) return false;
  if (out_json != nullptr) *out_json = dumped;
  return true;
}

}  // namespace compositor
