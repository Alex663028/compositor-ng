// AI生成
/*
 * filters.h — Filter 菜单的领域层（P1 滤镜批次）。
 *
 * 背景：Vignette / Tonal Contrast / Bloom / Lens Correction / Gaussian Blur /
 * Add Noise / Grain / Motion Blur / Progressive Blur 在上游是 FilterSettings +
 * PixelFilter.run 的一条通路（Filters.swift:63、:312-356）。这九个算子里，前七个
 * 的像素内核在本仓早已就位（AdjustPixels.h 的 adjust_colored_vignette /
 * adjust_tonal_contrast / adjust_grain、LensPixels.h 的 lens_distort、
 * NoisePixels.h 的 noise_add_at、adjustment_eval.h 的 blur_rgba8_copy），
 * Motion Blur 与 Progressive Blur 没有对应内核，由本文件按上游
 * `CIMotionBlur` / `CIMaskedVariableBlur` 的可观察口径补一份（盒式线积分 /
 * 分档三角混合）；缺的都是**产品入口**：参数契约、与图层几何的装配、
 * 一步撤销、与选区的混合。本文件把那条通路收口成一个领域入口。
 *
 * 尾部追加（kind 9 / 10）：Sharpen（Unsharp Mask）复用高斯内核，
 * Reduce Noise（双边滤波）是本文件新增的边沿保留平滑。两者同样受下面三条
 * 口径与 8 位工作副本边界约束。
 *
 * 尾部再追加（kind 11..14，Filter > Other 组）：High Pass / Minimum / Maximum /
 * Offset。前三个复用共享的模糊与邻域极值，Offset 是整像素平移；它们同样受
 * 三条口径约束，且都不改图层几何（变换/边框/alpha 的语义各自见结构体注释）。
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
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"

namespace compositor {

class Document;

/* 只需要声明：`filter_render_stack` 的两个像素参数都是引用/指针（实现在
 * filters.cpp，完整类型见 domain/layer.h）。 */
struct ImageBuffer;

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

/*
 * Gaussian Blur（上游 FilterSettings.radius / preserveTransparency）。
 *
 * `radius` 是图层像素的高斯半径（0.1..250，上游量程）。**0 = 恒等**（零初始化
 * 口径），(0, 0.1) 是越界，不静默夹到 0.1——与既有的「越界拒绝」同规矩。
 *
 * `preserve_transparency`：只模糊颜色，轮廓与原 alpha 不动（上游
 * `unpremultiplyingAlpha` + `settingAlphaOne` + alpha 掩码的等价做法：
 * 用模糊后的 alpha 反预乘取得颜色，再乘回**原** alpha）。关掉时整份预乘像素
 * 一起模糊，alpha 会向外扩散（上游默认行为）。
 */
struct GaussianBlurSettings {
  double radius = 0.0;
  bool preserve_transparency = false;
};

/*
 * Add Noise（上游 FilterSettings.amount / gaussian / monochromatic / seed）。
 *
 * `amount` 是 Photoshop 百分比，0.1..400；**0 = 恒等**。`gaussian` 为真时噪声
 * 按高斯分布（更密），否则均匀分布；`monochromatic` 为真时三通道加同一个值，
 * 只改亮度。`seed` 决定噪声场，同一 seed + 同一原点逐字节可复现。
 *
 * 噪声场按**文档坐标**锚定（`noise_add_at` 的 origin = 图层 effective_bounds
 * 原点取整），图层移动后图案不会在像素上「游动」——与实时调整层的
 * `noiseOrigin` 口径一致。
 */
struct AddNoiseSettings {
  double amount = 0.0;
  bool gaussian = false;
  bool monochromatic = false;
  uint32_t seed = 0;
};

/*
 * Grain（上游 FilterSettings.grain / GrainSettings）。
 *
 * `amount` 0..100（0 = 恒等）；`size` 0.5..20 是颗粒尺度（文档像素）；
 * `roughness` 0..100 控制小尺度不规则细节。`seed` 决定图案。
 *
 * 与调整层的 Grain 走**同一个内核**（`adjust_grain`），差别只在入口：
 * 调整层作用于合成后的 backdrop，滤镜作用于目标图层自身的像素。
 * 采样在图层像素空间（origin 0，unitsPerPixel = 1 / scale），与上游
 * `grain.apply(job.image, unitsPerPixel: 1 / job.scale, seed: job.seed)` 一致。
 */
struct GrainSettings {
  double amount = 0.0;
  double size = 1.5;
  double roughness = 50.0;
  uint32_t seed = 0;
};

/*
 * Motion Blur（上游 FilterSettings.angle / distance，CIMotionBlur）。
 *
 * `angle` -90..90 是方向角（Photoshop 口径：水平为 0，逆时针为正）；
 * `distance` 1..2000 是条纹长度（图层像素，预览再乘 scale）；**0 = 恒等**。
 * (0, 1) 与越界值显式拒绝，不静默夹到端点。
 *
 * 上游把 `distance / sqrt(12)` 传给 `CIMotionBlur` 的 radius——即让 Core Image
 * 的（近似）分布与长度为 distance 的均匀条纹同方差。本仓直接做**盒式线积分**，
 * 条纹长度就是 distance：方向与长度两条可观察口径一致，形状不声称逐像素等同。
 * 采样在预乘像素上做双线性平均（alpha 一起平均，条纹会向外扩散），坐标夹到
 * 图层边界（等效上游 `extending(...past canvasSides)` 的边缘延展）。
 */
struct MotionBlurSettings {
  double angle = 0.0;
  double distance = 0.0;
};

/*
 * Progressive Blur（上游 FilterSettings.progressive*，CIMaskedVariableBlur）。
 *
 * `from` / `to` 是渐变轴两端的模糊半径（图层像素，0..250，预览再乘 scale）：
 * 起点按 `from`、终点按 `to`，中间按轴上的位置线性插值。`inverted` 交换 from/to
 * （等价于把轴掉头）。**两端半径都为 0 = 恒等**（上游 `strongest > 0` 的 guard）。
 *
 * `start_*` / `end_*` 是**文档坐标**的轴端点（连续，不必落在图层内）。上游把两点
 * 经 `job.mapping.inverted()` 投到像素空间；本层走
 * `Layer::document_to_pixel_continuous`，同一套仿射（归一化 → 逆旋转 → 翻转），
 * 保留亚像素位置，缩放/旋转下渐变方向不会 ±1 px 抖动。
 *
 * `radial` 为真时轴变成以 start 为心、|end − start| 为半径的径向渐变（上游
 * `CIRadialGradient`，radius0 = 0、radius1 = 轴长），否则是沿轴的线性渐变。
 *
 * 退化轴（两点距离 < 1 px）：上游把终点改成 start + (0, -1)（CI 的 y 向上），
 * 本层在像素空间取 start + (0, +1)（本仓像素 y 向下），对渐变方向等价。
 *
 * 缺省轴：上游 `progressiveStart/End` 为 nil 时取画布中线（x = 画布中线、
 * y 从 minY 到 maxY）。`auto_axis = true`（零初始化口径）时本层不替宿主猜画布——
 * 按「图层自身像素矩形中线、自顶向下」落轴；宿主给了 true 以外的显式轴时置
 * `auto_axis = false` 并填 start/end。这样零初始化的 `(0,0)-(0,0)` 不会被误读成
 * 「原点上的退化轴」（径向档下那是一个有意义的输入，不是缺省值）。
 */
struct ProgressiveBlurSettings {
  double from = 0.0;
  double to = 0.0;
  double start_x = 0.0;
  double start_y = 0.0;
  double end_x = 0.0;
  double end_y = 0.0;
  bool radial = false;
  bool inverted = false;
  bool auto_axis = true;
};

/*
 * Sharpen / Unsharp Mask（上游 Filter > Sharpen > Unsharp Mask）。
 *
 * `amount` 0..500（百分比，0 = 恒等）；`radius` 0.1..250 是 detail 半径
 * （图层像素，预览再乘 scale）；`threshold` 0..255 是「低于它不动」的通道差
 * 阈值，用来避免把噪点一起放大。
 *
 * 口径 = 经典 USM：`src + amount/100 * (src - blur(src))`。`radius` 复用共享的
 * `blur_rgba8_copy`（与 Gaussian Blur / Tonal Contrast 同一实现），不做第二套
 * 高斯。运算落在**预乘**像素上、逐通道夹到 `[0, alpha]`，alpha 不变——图层轮廓
 * 不向外扩散（半透明边缘的颜色不会因为锐化越过自身 coverage）。
 */
struct SharpenSettings {
  double amount = 0.0;
  double radius = 1.0;
  double threshold = 0.0;
};

/*
 * Reduce Noise（上游 Filter > Noise > Reduce Noise 的常用子集）。
 *
 * `strength` 0..10（0 = 恒等），`preserve_details` 0..100（越高越保留细节，
 * 双边滤波的值域 sigma 越小），`reduce_color_noise` 0..100（只平滑色度：
 * 亮度按双边结果、色度按窗口均值混合）。
 *
 * 实现：5×5 的双边滤波，工作在**直通（unpremultiply 后）**颜色上，先转 YCoCg，
 * 窗口权重取 Y 轴（≈亮度）之差（亮度边沿保留），色度两轴按窗口均值平滑；
 * alpha 逐字节不变。
 * 上游的算法内部不公开（细节层 + 小波一类的多尺度），本仓只保证三条可观察
 * 口径：噪声方差下降、边沿对比保留、色噪按键平滑；不声称逐像素等同。
 *
 * 边界：`sharpen_details`（上游对话框第四行）不在本结构里——锐化已有独立
 * 滤镜，不把两条锐化通路塞进同一个算子；见 docs/filters.md §3.11。
 */
struct ReduceNoiseSettings {
  double strength = 0.0;
  double preserve_details = 50.0;
  double reduce_color_noise = 25.0;
};

/*
 * High Pass（上游 Filter > Other > High Pass）。
 *
 * `radius` 0.1..250 是模糊半径（图层像素，预览再乘 scale）；**0 = 恒等**。
 * (0, 0.1) 与越界值显式拒绝，不静默夹到 0.1（与 Gaussian Blur / Sharpen 同规矩）。
 *
 * 口径 = `128 + (src - blur(src))`，在**直通（unpremultiply 后）**颜色上算，
 * 再乘回原 alpha；模糊复用共享的 `blur_rgba8_copy`（与 Gaussian Blur / Sharpen
 * 同一实现）。结果是「中灰底的边缘图」：平坦区域落 128，alpha 逐字节不变——
 * High Pass 常被用作叠加/柔光混合的底图，轮廓不能因为滤镜自己扩散。
 */
struct HighPassSettings {
  double radius = 0.0;
};

/*
 * Minimum / Maximum（上游 Filter > Other > Minimum / Maximum）。
 *
 * `radius` 是方形窗口半径（整数像素，1..100；预览再乘 scale 后取整，至少 1）；
 * **0 = 恒等**。窗口边长 = 2 * r + 1，逐通道在窗口内取最小 / 最大，等效于对
 * 亮部做**腐蚀 / 膨胀**：Minimum 让亮细节收缩、暗部扩张，Maximum 反之。
 *
 * 四个通道一起做（alpha 也参与），所以半透明轮廓会跟着形状一起收缩/扩张——
 * 与 Photoshop 在透明图层上的可观察行为一致；不做「只改颜色不动 alpha」的
 * 特殊处理。窗口在图层边界处裁剪（不复制边缘像素），与共享盒式模糊的边界口径
 * 相同：贴边的窗口只看落在图层内的像素。
 */
struct MinimumSettings {
  double radius = 0.0;
};

struct MaximumSettings {
  double radius = 0.0;
};

/*
 * Median / Dust & Scratches（A45，2026-10-27）。
 *   radius     0 = 恒等；1..50 像素
 *   threshold  仅 Dust & Scratches：0 = 等同 Median；1..255 = 超阈值才替换
 */
struct DenoiseSettings {
  double radius = 0.0;
  int32_t threshold = 0;
};

/*
 * Offset（上游 Filter > Other > Offset）。
 *
 * `horizontal` / `vertical` 是图层像素的整数位移（-10000..10000），四舍五入到
 * 整像素；两者取整后都为 0 = 恒等。
 *
 * `wrap` 是露出区域的处理方式（数值与 ABI `comp_offset_wrap` 一致）：
 *   0 = 透明（露出部分留空，图层内容不环绕）；
 *   1 = 边缘延展（用最外一圈像素填满露出区域）；
 *   2 = 环绕（移出去的内容从对侧卷回来）。
 * 其余值越界拒绝，不静默当成 0。
 *
 * 实现是**整像素平移**：目标像素读源 `(x - dx, y - dy)`，不做重采样，
 * 因此位移为整数时逐字节可逆，不引入插值模糊。
 */
struct OffsetSettings {
  double horizontal = 0.0;
  double vertical = 0.0;
  int32_t wrap = 0;
};

/*
 * Custom（上游 Filter > Other > Custom）。
 *
 * `kernel[25]` 是 5×5 卷积核，行优先（`kernel[row * 5 + column]`，第 0 行是
 * 最上面一行）。卷积在**直通（unpremultiply 后）**颜色上做，每通道：
 *
 *   out = sum(kernel[i] * 邻居直通色) / divisor + offset
 *   divisor = (scale == 0 ? 1 : scale)
 *
 * 再乘回**原** alpha；alpha 逐字节不变。alpha == 0 的像素原样跳过，邻域里的
 * 透明像素没有直通色、按 0 参与累加——除数是面板给的 `scale`，不按覆盖率归一，
 * 所以紧贴透明区的轮廓像素会因为少了那几格权重而偏暗（5×5 直通卷积的直接结果，
 * 不是 bug；docs/filters.md §3.16 记这条边界）。
 *
 * 参数闸：核项 / `scale` / `offset` 都在 -999..999（上游对话框的量程）。
 * `scale` 为 0 按 1 处理（= 不除），这样面板把「除数」清空时不会除零。
 *
 * 缺省 = 恒等核（中心 1，其余 0）+ scale 1 + offset 0：默认构造的
 * `FilterSettings{}` 因此仍是恒等，与其余滤镜的零初始化口径一致。
 *
 * 边界：邻居坐标夹到图层内（边缘延展），不复制/环绕；预览（scale < 1）**不缩放**
 * 核——卷积核没有连续的「半径」语义，降采样预览的观感会比全尺寸偏强，
 * 这条差异写在 docs/filters.md §3.16。
 */
struct CustomSettings {
  double kernel[25] = {0.0, 0.0, 0.0, 0.0, 0.0,
                       0.0, 0.0, 0.0, 0.0, 0.0,
                       0.0, 0.0, 1.0, 0.0, 0.0,
                       0.0, 0.0, 0.0, 0.0, 0.0,
                       0.0, 0.0, 0.0, 0.0, 0.0};
  double scale = 1.0;
  double offset = 0.0;
};

/*
 * Spherize（上游 Filter > Distort > Spherize 球面化）。
 *
 * `amount` -100..100，**0 = 恒等**：正值 = 球面（中心放大、像贴到球上），
 * 负值 = 凹面。半径按图层**半短边**归一（r = 到中心的距离 / 半短边），
 * 映射为 `r_src = r * (1 - s * (1 - r²))`（s = amount/100）：中心不动、
 * 边缘不动、中间按 s 向心或离心偏移，所以 0 之外都真的重采样。
 * 与 Lens Correction 共用双线性径向重采样口径，但映射不是同一个 k。
 */
struct SpherizeSettings {
  double amount = 0.0;
};

/*
 * Pinch（上游 Filter > Distort > Pinch 挤压）。
 *
 * `amount` -100..100，**0 = 恒等**：正值 = 向内挤压（Pinch），负值 = 向外
 * 扩张（Punch）。映射 `r_src = r * (1 + s * (1 - r²))`，与 Spherize 同族、
 * 符号相反（两者在 ±s 下互为对方的映射方向）。
 */
struct PinchSettings {
  double amount = 0.0;
};

/*
 * Twirl（上游 Filter > Distort > Twirl 旋转扭曲）。
 *
 * `angle` -999..999（度），**0 = 恒等**：绕图层中心旋转，离中心越近转角越大、
 * 边缘（r >= 1）不转：`θ_src = θ_dst - angle * max(0, 1 - r)`。转角超过 ±360
 * 会绕成多圈、出现径向条纹——这是旋转扭曲的正常观感，不做夹取。
 */
struct TwirlSettings {
  double angle = 0.0;
};

/*
 * Mosaic（上游 Filter > Pixelate > Mosaic 马赛克）。
 *
 * `cell_size` **0 = 恒等**；否则 2..200（方格边长，图层像素；预览再乘 scale、
 * 取整但至少 2）。方格取窗口内**预乘**分量的算术均值（与盒式下采样同一口径：
 * 预乘均值天然满足 out_color <= out_alpha，不会把透明区颜色渗进不透明格）。
 */
struct MosaicSettings {
  double cell_size = 0.0;
};

/*
 * Emboss（上游 Filter > Stylize > Emboss 浮雕）。
 *
 * `amount` **0 = 恒等**；否则 1..500（= 上游百分比）。
 * `angle` -360..360（光照方向，度；0 = 正右、逆时针为正）。
 * `height` 0..10（采样间距，图层像素；预览再乘 scale 后至少 1）。
 *
 * 口径 = `128 + amount/100 * (A - B)`，A / B 是沿 angle 方向 ±height 处采到的
 * **直通**色；结果逐通道夹到 0..255、乘回原 alpha（alpha 逐字节不变）。
 * 平坦区域落 128 底灰，与 High Pass 同一「边缘以中灰为轴」的约定。
 */
struct EmbossSettings {
  double amount = 0.0;
  double angle = 0.0;
  double height = 0.0;
};

/*
 * Clouds / Difference Clouds（上游 Filter > Render > Clouds / Difference Clouds）。
 *
 * 生成式滤镜：用**确定性 fBm 值噪声**铺一层云纹，再按 `amount` 与原像素混合。
 *
 *   amount      0 = 恒等（不开事务、不动像素）；否则 0.1..100（混合权重 %），
 *               100 = 完全用云纹替换原色（保留原 alpha 覆盖率）。
 *   size        2..1000（云纹特征尺度，图层像素；预览再乘 scale）
 *   seed        噪声场种子；同 seed + 同图层原点逐字节可复现
 *   color_low / color_high  噪声 0 / 1 两端的**直通 sRGB** 0..1 颜色
 *
 * 口径（与上游的差异如实记录，见 docs/filters.md）：上游走 CIRandomGenerator +
 * 更复杂的退火，本仓是 5 个八度、光滑插值的值噪声，只保证「分形云纹、可复现、
 * 端色可控」三条可观察口径，不声称逐像素等同。噪声锚在**文档坐标**
 * （origin = 图层 effective_bounds 原点），图层移动后图案不游动（同 Add Noise）。
 * 逐字节保留原 alpha：透明区不上色（上游 Clouds 会铺满画布，本仓不铺底）。
 */
struct CloudsSettings {
  double amount = 0.0;
  double size = 200.0;
  uint32_t seed = 0;
  double color_low[3] = {0.0, 0.0, 0.0};
  double color_high[3] = {1.0, 1.0, 1.0};
};

/*
 * Difference Clouds：`|cloud - src|`。噪声取灰度云纹（0..255），逐通道
 *   `out = src*(1-a) + |cloud - src|*a`（a = amount/100），alpha 不变。
 * 与 Clouds 共用同一份噪声与端色约定；零初始化 = amount 0 = 恒等。
 */
struct DifferenceCloudsSettings {
  double amount = 0.0;
  double size = 200.0;
  uint32_t seed = 0;
};

/*
 * Picture Frame（上游 Filter > Render > Picture Frame）。
 *
 *   width  0 = 恒等；否则 1..500（画框厚度，图层像素；预览再乘 scale 后至少 1）
 *   color  画框**直通 sRGB** 0..1 颜色
 *
 * 沿图层四边落一圈等厚的纯色带（到最近边界的像素数 < width）；逐字节保留原
 * alpha（透明区不会被框上色）。上游按灰度图做立体相框，本仓是扁平色带，只保证
 * 「四边等厚、颜色可控」两条可观察口径（见 docs/filters.md）。
 */
struct PictureFrameSettings {
  double width = 0.0;
  double color[3] = {0.0, 0.0, 0.0};
};

/* 滤镜种类。数值与 ABI `comp_filter_kind` 一一对应（尾部追加，不改既有值）。 */
enum FilterKind {
  kFilterVignette = 0,
  kFilterTonalContrast = 1,
  kFilterBloom = 2,
  kFilterLensCorrection = 3,
  kFilterGaussianBlur = 4,
  kFilterAddNoise = 5,
  kFilterGrain = 6,
  kFilterMotionBlur = 7,
  kFilterProgressiveBlur = 8,
  kFilterSharpen = 9,
  kFilterReduceNoise = 10,
  kFilterHighPass = 11,
  kFilterMinimum = 12,
  kFilterMaximum = 13,
  kFilterOffset = 14,
  kFilterCustom = 15,
  kFilterMedian = 16,
  kFilterDustScratches = 17,
  /* A46（2026-10-28）尾部追加：扭曲 / 像素化 / 风格化三组各若干。 */
  kFilterSpherize = 18,
  kFilterPinch = 19,
  kFilterTwirl = 20,
  kFilterMosaic = 21,
  kFilterEmboss = 22,
  /* A47（2026-10-28）尾部追加：渲染组三项。 */
  kFilterClouds = 23,
  kFilterDifferenceClouds = 24,
  kFilterPictureFrame = 25,
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
  GaussianBlurSettings gaussian_blur;
  AddNoiseSettings add_noise;
  GrainSettings grain;
  MotionBlurSettings motion_blur;
  ProgressiveBlurSettings progressive_blur;
  SharpenSettings sharpen;
  ReduceNoiseSettings reduce_noise;
  HighPassSettings high_pass;
  MinimumSettings minimum;
  MaximumSettings maximum;
  OffsetSettings offset;
  CustomSettings custom;
  DenoiseSettings median;
  DenoiseSettings dust_scratches;
  SpherizeSettings spherize;
  PinchSettings pinch;
  TwirlSettings twirl;
  MosaicSettings mosaic;
  EmbossSettings emboss;
  CloudsSettings clouds;
  DifferenceCloudsSettings difference_clouds;
  PictureFrameSettings picture_frame;
};

/* 参数闸：逐项检查量程（越界拒绝而不是静默 clamp，与 camera_raw 同规矩）。 */
bool filter_settings_valid(const FilterSettings& settings, std::string* error);

/* 恒等判据（不碰像素、不开事务）。 */
bool filter_settings_identity(const FilterSettings& settings);

/* 同值判据（只比较 `kind` 对应的那个子结构）。
 * 用途：智能滤镜「同参数重设」不落撤销步（P1-2 收尾 ⑦）。 */
bool filter_settings_equal(const FilterSettings& a, const FilterSettings& b);

/* 滤镜的稳定名字（事务标签 / note 用）；未知 kind 返回 "filter"。 */
const char* filter_kind_label(FilterKind kind);

/* ============================================================ 执行 */

/*
 * 对目标图层跑一次滤镜。成功返回 true。
 *
 * `scale` 是预览像素 / 图层像素（<= 0 视为 1）：只影响以像素为单位的半径
 * （Tonal Contrast / Bloom / Gaussian Blur / Motion Blur / Progressive Blur），
 * 让降采样预览看起来与全尺寸一致；几何类（Vignette / Lens Correction）本身
 * 相对图层尺寸，无需缩放。
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

/* ==================================================== 智能滤镜栈求值 */

/*
 * 把一整个滤镜栈跑在**独立的像素缓冲**上，结果写 `out`（P1-2 收尾 ⑦）。
 *
 * 与 `filter_apply` 的差别（这是智能对象语义要求的，不是简化）：
 *   - 不碰文档、不开事务、不读选区：智能滤镜的结果不能因为「当前有选区」而变；
 *   - 恒等条目跳过；越界参数整栈失败（不写 out，一个字节都不动）；
 *   - 坐标口径是**对象像素空间**：Add Noise 的锚点取 (0,0)，Progressive Blur 的
 *     显式轴端点直接按对象像素坐标解释。图层之后被移动/缩放/旋转，同一份
 *     `source + filters` 仍渲出逐字节相同的结果；
 *   - 16 位 source 先落 8 位工作副本（`deep_to_rgba8`，与 filter_apply 同一条）；
 *     HDR（RGBA16F）拒绝——8 位内核没有它的通路。
 *
 * 错误码：out 为空 / scale 非有限 / 任一设置越界 -> COMP_ERR_INVALID_ARGUMENT；
 * source 为空 / HDR -> COMP_ERR_STATE；分配失败 -> COMP_ERR_OUT_OF_MEMORY。
 * 成功时 `out` 是**独占**缓冲（不会与 source 共享像素）。
 */
bool filter_render_stack(const ImageBuffer& source, const std::vector<FilterSettings>& stack,
                         double scale, ImageBuffer* out, comp_result* out_code,
                         std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_FILTERS_H */
