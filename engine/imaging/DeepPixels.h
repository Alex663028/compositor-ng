// AI生成
#ifndef DeepPixels_h
#define DeepPixels_h
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// 16-bit layers kept through 8-bit operations. Each color gets an 8-bit `low` a little below it (0 to 2 steps, rounded
// as dither rounds, so where bands would start falls at random and comes out as fine noise). An 8-bit operation is run
// on `low`, and on `low` one, two and four steps up, and the result at 16 bits is read off between them by where the
// color lies above `low` (see `deep_change` in DeepPixels.c):
//
// - where one step changes the result by two or more (the operation stretches tones there), between the steps the
//   color lies between: exact wherever the operation is straight across one step, sharp bends included;
// - where it changes it by one or nothing (the operation holds or compresses tones there), its results being 8-bit
//   too say too little across one step, so the slope is read across four instead.
//
// An operation whose channels mix (each output channel depends on all three inputs) steps each input channel on its own
// and adds up how each moves each output: ten runs rather than four.
//
// All buffers are premultiplied RGBA. 16-bit ones are native-endian uint16, 4 a pixel; 8-bit ones are 4 bytes a
// pixel. Strides are in bytes. Alpha is carried at 8 bits (it's coverage), so all inputs share it.

// The 8-bit image `step` steps above a 16-bit one's `low` (0 for `low` itself), kept within alpha: in every
// color channel, or with `channel` 0–2 only in that one (-1 for all).
void deep_split(const uint16_t *deep, size_t deepStride, uint8_t *out, size_t stride, size_t width, size_t height,
                int step, int channel);

// The 16-bit result from an operation's results on `low` and one, two and four steps up (`deep_split` with steps 0, 1,
// 2 and 4). Without `mixes`, only the first of each three is read, stepped in every channel; with it, the three are
// red, green and blue stepped alone.
void deep_merge(const uint16_t *deep, size_t deepStride, const uint8_t *lowResult, const uint8_t *const nearResults[3],
                const uint8_t *const midResults[3], const uint8_t *const farResults[3], size_t stride, uint16_t *out,
                size_t outStride, size_t width, size_t height, int mixes);

// A painted 16-bit layer made whole: where its 8-bit pixels (`painted`, the base with the strokes over it) differ from
// its 8-bit base alone (`base`), paint went there and its value is taken; elsewhere the 16-bit pixels in `deep` stay.
// The strokes' patches are whole tiles, untouched pixels included, so laying them over the 16-bit pixels outright
// would round everything they cover back to 8 bits.
void deep_take_painted(uint16_t *deep, size_t deepStride, const uint8_t *painted, const uint8_t *base, size_t stride,
                       size_t width, size_t height);

// A clipping stack's surface at 16 bits, as `layer_extract_alpha` and `layer_unpremultiply_opaque` do at 8: saves each
// pixel's alpha into `alpha` (`width` values a row) and makes the colors opaque, so the layers clipped to it blend at
// full coverage; `deep_restore_alpha` puts the alpha back.
void deep_extract_alpha(uint16_t *rgba, size_t stride, uint16_t *alpha, size_t width, size_t height);
void deep_restore_alpha(uint16_t *rgba, size_t stride, const uint16_t *alpha, size_t width, size_t height);

// A color lookup at 16 bits, as `cube_apply` at 8: `cube` is `dimension`³ RGBA floats, red fastest, as Core Image's
// color cube. Operations whose channels mix (Hue/Saturation, Black & White) go this way rather than through the three
// runs, which read each channel's result off that channel's own input alone.
void deep_cube_apply(uint16_t *pixels, size_t stride, size_t width, size_t height, const float *cube, int dimension);

// The brightest color channel in a half-float RGBA image (premultiplied, native-endian, 4 a pixel), unpremultiplied:
// above 1 where it holds light brighter than white (an HDR image).
float deep_half_peak(const uint16_t *pixels, size_t stride, size_t width, size_t height);

// HDR layers (half floats in extended sRGB, premultiplied, native-endian, 4 a pixel) through 8-bit operations: each
// pixel brighter than white is divided by its brightest channel, into the 16-bit layout, so its color fits within white
// with its hue; `scales` keeps each pixel's divisor (1 for the rest), `width` a row. The operation runs as on any
// 16-bit layer, and `deep_hdr_restore` multiplies its result back into half floats. Highlights take the operation's
// way with color, and dim with it: a Levels or Curves pulling white down pulls them down in proportion.
void deep_hdr_normalize(const uint16_t *hdr, size_t hdrStride, uint16_t *out, size_t outStride, float *scales,
                        size_t width, size_t height);
void deep_hdr_restore(const uint16_t *normal, size_t normalStride, const float *scales, uint16_t *hdr, size_t hdrStride,
                      size_t width, size_t height);

// Exposure on HDR pixels in place, as `ExposureSettings.table` at 8 bits but with nothing clipped at white: each channel
// to linear light, times `scale`, plus `offset`, to the power `inverseGamma`, and back.
void deep_hdr_exposure(uint16_t *hdr, size_t stride, size_t width, size_t height, float scale, float offset, float inverseGamma);

// `deep_extract_alpha` and `deep_restore_alpha` for a clipping stack's surface in half floats (a merge with an HDR
// layer), alpha kept as floats.
void deep_extract_alpha_hdr(uint16_t *rgba, size_t stride, float *alpha, size_t width, size_t height);
void deep_restore_alpha_hdr(uint16_t *rgba, size_t stride, const float *alpha, size_t width, size_t height);

// `deep_take_painted` for an HDR layer's half floats.
void deep_take_painted_hdr(uint16_t *hdr, size_t hdrStride, const uint8_t *painted, const uint8_t *base, size_t stride,
                           size_t width, size_t height);

/* C 方案 C1 新增：半精度像素与整数像素之间的两个转换。
 *
 * `deep_half_to_u16` 是上游 `DeepLayer.image(_:)`（DeepLayers.swift:31）那一步：
 * 把 half 的**值**按 0..65535 取整写进 RGBA16 布局。white 以上的光在此被截断
 * （"anything brighter than white shows as white"，DeepLayers.swift:175），
 * 但保持预乘关系（分量与 alpha 各自独立取整，不做反预乘）。
 *
 * `deep_u16_to_half` 是它的逆（DeepLayers.swift 写入侧同款）：0..65535 的整数
 * 按 extended sRGB 编码写成 half。
 *
 * 为什么不复用 deep_hdr_normalize/restore：那两个是给「8 位操作跑在 HDR 图层上」
 * 用的，除以各像素最亮通道并记 scales——显示副本不缩放高光，只截断。
 */

/* Half RGBA → 16-bit RGBA。两侧同为 4 个 uint16 一像素（half 与 16 位同宽），
 * 行分别按字节给。值域按 0..65535 取整后夹紧，NaN/Inf 按 +Inf 夹到 65535。 */
void deep_half_to_u16(const uint16_t *half, size_t halfStride, uint16_t *out, size_t outStride, size_t width, size_t height);

/* 16-bit RGBA → Half RGBA（同布局）。0..65535 映射为 extended sRGB 编码的 half，
 * 白点仍为 1.0——这是**存储/合成**用的，不是 normalize（后者会把高光除下来）。 */
void deep_u16_to_half(const uint16_t *wide, size_t wideStride, uint16_t *out, size_t outStride, size_t width, size_t height);

/* C 方案 C3 新增：文件里的浮点像素进 HDR 图层的入库通道。
 *
 * `deep_hdr_take_linear` 把**线性光**的直通 alpha 浮点 RGBA（EXR / Radiance 这类
 * 文件的原生取值）写进 HDR 图层的像素布局：每个颜色分量过 sRGB 曲线（`deep_encoded`，
 * 白点以上照常延伸），再乘自己的 alpha；alpha 夹进 0..1（它是覆盖率，不是亮度）。
 * 这一步对应上游 ImageImporter 里 `wideContext`（workingColorSpace =
 * extendedLinearSRGB，workingFormat = RGBAh）渲染进 `DeepLayer.hdrSpace`
 * （extendedSRGB，DeepLayers.swift:85）时 Core Image 做的那次色彩空间转换。
 *
 * 输入：`linear` 直通 alpha、线性光、4 个 float 一像素、行步长以**字节**计；
 *       `out` 预乘 half、extended sRGB、4 个 uint16 一像素、行步长以字节计。
 * NaN 落 0 而不是留在结果里（坏采样不该变成亮点）；±Inf 照常走曲线，
 * 由 `deep_half_peak`（跳过 Inf）与显示侧的夹白各自处理。
 *
 * 判决「这文件是不是 HDR」不在内核里：调用方对结果跑 `deep_half_peak`，
 * 与 `DeepLayer.wideAsset`（DeepLayers.swift:174）同口径地比 `peak > 1.001`。 */
void deep_hdr_take_linear(const float *linear, size_t linearStride, uint16_t *out, size_t outStride, size_t width, size_t height);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif
