// AI生成
/*
 * comp_filter.h — Filter 菜单的 C ABI（P1 滤镜批次）。
 *
 * 语义唯一真源见 `domain/filters.h` 的文件头：九个算子（Vignette / Tonal
 * Contrast / Bloom / Lens Correction / Gaussian Blur / Add Noise / Grain /
 * Motion Blur / Progressive Blur）的像素口径、选区混合与能力边界都写在那里。
 *
 * 本头文件只暴露一件事：一次「整层」滤镜调用。
 *
 *   - 参数是面板设置（量程对齐上游 `FilterSettings.normalized`）；
 *   - **零初始化 = 恒等**：各子结构的 amount / distortion 缺省为 0，此时不动
 *     像素、不开事务（out_changed = 0）。宿主需要面板缺省值时自己填（上游缺省
 *     见 docs/filters.md），ABI 不替宿主决定「用户想要什么效果」；
 *   - 越界参数拒绝而不是静默 clamp（与 camera_raw 同规矩；上游是 clamp，
 *     这里刻意更严格，避免宿主传错量程却静默生效半个效果）；
 *   - 一步撤销：加入调用方已开启的显式事务，或自成一个隐式步；
 *   - 有选区时结果按 coverage 与选区外的原像素混合（文档坐标）。
 *
 * 能力边界（与 domain/filters.h 一致）：
 *   16 位图层先落 8 位工作副本（note 写明），HDR 图层 -> COMP_ERR_STATE；
 *   组图层 / 无像素图层 -> COMP_ERR_STATE。
 */
#ifndef COMPOSITOR_COMP_FILTER_H
#define COMPOSITOR_COMP_FILTER_H

#include <stddef.h>
#include <stdint.h>

#include "comp_core.h"
#include "comp_layer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 滤镜种类。数值与域层 `FilterKind` 一一对应（尾部追加，不改既有值）。 */
typedef enum comp_filter_kind {
  COMP_FILTER_VIGNETTE        = 0,
  COMP_FILTER_TONAL_CONTRAST  = 1,
  COMP_FILTER_BLOOM           = 2,
  COMP_FILTER_LENS_CORRECTION = 3,
  COMP_FILTER_GAUSSIAN_BLUR   = 4,
  COMP_FILTER_ADD_NOISE       = 5,
  COMP_FILTER_GRAIN           = 6,
  COMP_FILTER_MOTION_BLUR     = 7,
  COMP_FILTER_PROGRESSIVE_BLUR = 8
} comp_filter_kind;

/*
 * Vignette（上游 FilterSettings.vignette*）。
 *   amount      0..100，0 = 恒等
 *   midpoint    0..100（羽化起点）
 *   roundness   -100..100（负数 = 方角）
 *   feather     0..100
 *   highlights  0..100（高光保护）
 *   color       直通 sRGB 0..1 的边缘色
 */
typedef struct comp_filter_vignette {
  double amount;
  double midpoint;
  double roundness;
  double feather;
  double highlights;
  double color[3];
} comp_filter_vignette;

/*
 * Tonal Contrast（上游 FilterSettings.tonal*）。
 *   amount      0..100（= 内核 strength * 50）
 *   radius      1..100（高斯 detail 半径，图层像素；预览再乘 scale）
 *   shadows / midtones / highlights  -100..100
 */
typedef struct comp_filter_tonal_contrast {
  double amount;
  double radius;
  double shadows;
  double midtones;
  double highlights;
} comp_filter_tonal_contrast;

/* Bloom / Glow（上游 FilterSettings.bloom*）。amount 0..100，radius 1..150。 */
typedef struct comp_filter_bloom {
  double amount;
  double radius;
} comp_filter_bloom;

/* Lens Correction（上游 FilterSettings.distortion）。-100..100，0 = 恒等。 */
typedef struct comp_filter_lens_correction {
  double distortion;
} comp_filter_lens_correction;

/*
 * Gaussian Blur（上游 FilterSettings.radius / preserveTransparency）。
 *   radius                0 = 恒等；否则 0.1..250（图层像素，预览再乘 scale）
 *   preserve_transparency 1 = 只模糊颜色，轮廓与原 alpha 不动
 */
typedef struct comp_filter_gaussian_blur {
  double radius;
  int32_t preserve_transparency;
} comp_filter_gaussian_blur;

/*
 * Add Noise（上游 FilterSettings.amount / gaussian / monochromatic / seed）。
 *   amount          0 = 恒等；否则 0.1..400（Photoshop 百分比）
 *   gaussian        1 = 高斯分布，0 = 均匀分布
 *   monochromatic   1 = 三通道加同一个值（只改亮度）
 *   seed            噪声场种子；同 seed + 同图层原点逐字节可复现
 */
typedef struct comp_filter_add_noise {
  double amount;
  int32_t gaussian;
  int32_t monochromatic;
  uint32_t seed;
} comp_filter_add_noise;

/*
 * Grain（上游 FilterSettings.grain / GrainSettings）。
 *   amount     0..100，0 = 恒等
 *   size       0.5..20（颗粒尺度，文档像素）
 *   roughness  0..100
 *   seed       图案种子
 */
typedef struct comp_filter_grain {
  double amount;
  double size;
  double roughness;
  uint32_t seed;
} comp_filter_grain;

/*
 * Motion Blur（上游 FilterSettings.angle / distance，CIMotionBlur）。
 *   angle     -90..90（度；水平为 0，逆时针为正，Photoshop 口径）
 *   distance  0 = 恒等；否则 1..2000（条纹长度，图层像素；预览再乘 scale）
 */
typedef struct comp_filter_motion_blur {
  double angle;
  double distance;
} comp_filter_motion_blur;

/*
 * Progressive Blur（上游 FilterSettings.progressive*，CIMaskedVariableBlur）。
 *   from / to   0..250（渐变轴两端的模糊半径，图层像素；预览再乘 scale）
 *   start_x/y   轴起点的文档坐标（连续；auto_axis = 1 时忽略）
 *   end_x/y     轴终点的文档坐标（同上）
 *   radial        1 = 以 start 为心、|end - start| 为半径的径向渐变
 *   inverted      1 = 交换 from/to（等价于把轴掉头）
 *   explicit_axis 0 = 用缺省轴（图层自身像素矩形中线、自顶向下）；
 *                 1 = 用 start/end（文档坐标）
 *
 * 零初始化（from = to = 0，explicit_axis = 0）= 恒等 + 缺省轴：C 侧与域层
 * （`auto_axis = true`）的零初始化口径一致，不把 (0,0)-(0,0) 误读成退化轴。
 */
typedef struct comp_filter_progressive_blur {
  double from;
  double to;
  double start_x;
  double start_y;
  double end_x;
  double end_y;
  int32_t radial;
  int32_t inverted;
  int32_t explicit_axis;
} comp_filter_progressive_blur;

/*
 * 一次滤镜调用。只有 `kind` 对应的那个子结构被读取；其余忽略。
 * 零初始化 = Vignette + amount 0 = 恒等。
 */
typedef struct comp_filter_desc {
  comp_filter_kind             kind;
  comp_filter_vignette         vignette;
  comp_filter_tonal_contrast   tonal_contrast;
  comp_filter_bloom            bloom;
  comp_filter_lens_correction  lens_correction;
  comp_filter_gaussian_blur    gaussian_blur;
  comp_filter_add_noise        add_noise;
  comp_filter_grain            grain;
  comp_filter_motion_blur      motion_blur;
  comp_filter_progressive_blur progressive_blur;
} comp_filter_desc;

/*
 * 对目标图层跑一次滤镜。成功返回 COMP_OK。
 *
 * `scale` 是预览像素 / 图层像素（<= 0 视为 1）：影响以像素为单位的半径
 * （Tonal Contrast / Bloom / Gaussian Blur / Motion Blur / Progressive Blur），
 * 让降采样预览看起来与全尺寸一致。
 *
 * 错误码：
 *   参数越界 / 非有限 / desc 为 NULL -> COMP_ERR_INVALID_ARGUMENT
 *   图层不存在                       -> COMP_ERR_NOT_FOUND
 *   组图层 / 无像素 / HDR 图层        -> COMP_ERR_STATE
 *   分配失败                         -> COMP_ERR_OUT_OF_MEMORY
 *
 * note 可为 NULL；非 NULL 时写入 UTF-8 非致命说明（16 位降位、选区裁切、
 * Bloom / Gaussian Blur / Motion Blur / Progressive Blur 的近似口径），两段式：
 * note_capacity 为 0 时只回报所需长度，out_note_size 写入含 '\0' 的长度。
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_filter_apply(comp_document document, comp_layer_id target,
                                       const comp_filter_desc* desc, double scale,
                                       int32_t* out_changed, char* note, size_t note_capacity,
                                       size_t* out_note_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_FILTER_H */
