// AI生成
/*
 * test_hdr_semantics.cpp — C0 口径冻结：6 个 hdr_* 内核的行为护栏。
 *
 * 背景：6 个 hdr_* 内核在 DeepPixels.c:257-363 早已实现，但直到本文件之前
 * **一行测试都没有**（`grep -rn "deep_hdr" tests/` 零命中）。口径没对着测试冻结，
 * 等于没冻结——C1~C5 会建在从未被执行过的代码上。本文件按上游
 * DeepLayers.swift 的文档口径逐条钉死行为。
 *
 * 上游口径来源（compositor/Compositor/Document/DeepLayers.swift）：
 *   - :81-84  HDR 层：half floats in extended sRGB（白 = 1，高光在 1 以上）、预乘、
 *             top-left rows、8 字节/像素（与 16 位层同宽）
 *   - :97-99  8 位操作走 normalize/restore：高光各自除下来落回白内，再乘回去
 *   - :122   「Highlights take the operation's way with color, and dim with it」
 *   - :153   Exposure on HDR 精确执行，白点不裁切，so turning it down brings
 *             highlights into view
 *   - :174   `peak > 1.001` 才算 HDR 层（"a file brushing white through rounding
 *             isn't HDR"）
 *
 * 断言一律按「可确定性判定」写；半精度末位舍入允许容差，不做逐位比对。
 */
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "imaging/DeepPixels.h"
#include "test_framework.h"

namespace {

/* float → half 的参考实现（IEEE 754 binary16，最近偶数舍入），与内核内部同一套
 * 算法，用于构造期望值；放在测试侧而不是头文件里，避免测试依赖被测实现。 */
uint16_t ref_half(float value) {
  union {
    float f;
    uint32_t u;
  } in = {value};
  uint32_t sign = (in.u >> 16) & 0x8000, exponent = (in.u >> 23) & 0xff, mantissa = in.u & 0x7fffff;
  if (exponent == 0xff) return static_cast<uint16_t>(sign | 0x7c00 | (mantissa ? 0x200 : 0));
  int e = static_cast<int>(exponent) - 127 + 15;
  if (e >= 31) return static_cast<uint16_t>(sign | 0x7c00);
  if (e <= 0) {
    if (e < -10) return static_cast<uint16_t>(sign);
    mantissa |= 0x800000;
    uint32_t shift = static_cast<uint32_t>(14 - e);
    uint32_t half = mantissa >> shift, rest = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
    if (rest > halfway || (rest == halfway && (half & 1))) half += 1;
    return static_cast<uint16_t>(sign | half);
  }
  uint32_t half = sign | (static_cast<uint32_t>(e) << 10) | (mantissa >> 13), rest = mantissa & 0x1fff;
  if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) half += 1;
  return static_cast<uint16_t>(half);
}

float half_to_float(uint16_t bits) {
  uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31, exponent = (bits >> 10) & 0x1f, mantissa = bits & 0x3ff;
  union {
    uint32_t u;
    float f;
  } out;
  if (exponent == 0) {
    float v = static_cast<float>(mantissa) * (1.0f / 16777216.0f);
    return sign ? -v : v;
  }
  if (exponent == 31) out.u = sign | 0x7f800000u | (mantissa << 13);
  else out.u = sign | ((exponent + 112) << 23) | (mantissa << 13);
  return out.f;
}

/* 半精度预乘 RGBA 的浮点视图。 */
struct HalfPixel {
  float r, g, b, a;
};

HalfPixel read_half(const uint16_t *p) {
  return {half_to_float(p[0]), half_to_float(p[1]), half_to_float(p[2]), half_to_float(p[3])};
}

/* 写入一个半精度预乘像素。 */
void write_half(uint16_t *p, float r, float g, float b, float a) {
  p[0] = ref_half(r);
  p[1] = ref_half(g);
  p[2] = ref_half(b);
  p[3] = ref_half(a);
}

bool close_to(float got, float want, float tolerance) { return std::fabs(got - want) <= tolerance; }

}  // namespace

/*
 * deep_half_peak：HDR 判定闸。上游 :171-174 用它决定一个资产是 HDR 层还是
 * 16 位层，阈值 `peak > 1.001`。
 *
 * 口径要点：峰值是**反预乘后**的颜色通道值，不是预乘值；alpha 为 0 的像素跳过；
 * 上游代码里 `value < INFINITY` 的判断意味着 inf 不参与（否则一层全 inf 会让
 * 闸门通过，然后所有像素变成 NaN）。
 */
COMP_TEST(hdr_peak_measures_unpremultiplied_brightness) {
  const size_t width = 3, height = 2, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> pixels(width * height * 4, 0);

  /* (0,0)：不透明，色 = 4.0（白点以上）。 */
  write_half(&pixels[0], 4.0f, 4.0f, 4.0f, 1.0f);
  /* (1,0)：半透明，**预乘**值 1.0 → 反预乘后是 2.0。若峰值按预乘值算，这里会是 1.0。 */
  write_half(&pixels[4], 1.0f, 1.0f, 1.0f, 0.5f);
  /* (2,0)：全 0。 */
  write_half(&pixels[8], 0.0f, 0.0f, 0.0f, 0.0f);

  const float peak = deep_half_peak(pixels.data(), stride, width, height);
  COMP_CHECK_MSG(close_to(peak, 4.0f, 0.01f), "peak must take the brightest channel, unpremultiplied");

  /* 只有那一个半透明像素贡献时，反预乘后的值是 2.0，不是预乘的 1.0。 */
  write_half(&pixels[0], 0.0f, 0.0f, 0.0f, 1.0f);
  const float one = deep_half_peak(pixels.data(), stride, width, height);
  COMP_CHECK_MSG(close_to(one, 2.0f, 0.01f), "peak must unpremultiply before comparing");
}

COMP_TEST(hdr_peak_ignores_transparent_and_infinite) {
  const size_t width = 2, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> pixels(width * height * 4, 0);

  /* (0,0)：alpha=0 但颜色很亮。上游 `if (!(alpha > 0)) continue` 直接跳过，
   * 否则一层全透明但通道残留的图会被误判为 HDR。 */
  write_half(&pixels[0], 8.0f, 8.0f, 8.0f, 0.0f);
  /* (1,0)：正无穷。上游 `value < INFINITY` 排除它。 */
  write_half(&pixels[4], INFINITY, INFINITY, INFINITY, 1.0f);

  const float peak = deep_half_peak(pixels.data(), stride, width, height);
  COMP_CHECK_MSG(peak == 0.0f, "transparent pixels and infinities must not raise the peak");
}

/*
 * 上游 :174 的判定「peak > 1.001 才算 HDR 层」——「擦着白亮一下的文件不算 HDR」。
 * 这条护栏把阈值钉死在 1.001，并把两侧都测到：1.0（刚好白）不算，
 * 明显超白才算。
 */
COMP_TEST(hdr_threshold_brushing_white_is_not_hdr) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);

  std::vector<uint16_t> at_white(4, 0);
  write_half(at_white.data(), 1.0f, 1.0f, 1.0f, 1.0f);
  COMP_CHECK_MSG(!(deep_half_peak(at_white.data(), stride, width, height) > 1.001f),
                 "a pixel exactly at white is not an HDR layer");

  /* 舍入擦到 1.0009765625（1/1024），仍在 1.001 以下——这正是那句注释说的
   * 「a file brushing white through rounding isn't HDR」。 */
  std::vector<uint16_t> brushing(4, 0);
  write_half(brushing.data(), 1.0009765625f, 1.0009765625f, 1.0009765625f, 1.0f);
  const float brushed = deep_half_peak(brushing.data(), stride, width, height);
  COMP_CHECK_MSG(!(brushed > 1.001f), "rounding that brushes white is not HDR");

  std::vector<uint16_t> bright(4, 0);
  write_half(bright.data(), 1.5f, 1.5f, 1.5f, 1.0f);
  COMP_CHECK_MSG(deep_half_peak(bright.data(), stride, width, height) > 1.001f, "1.5 is an HDR layer");
}

/*
 * deep_hdr_normalize / deep_hdr_restore：HDR 图穿过一个「工作在 16 位像素上的
 * 8 位操作」（上游 run/lookup）。每个比白亮的像素各自除以其最亮通道，落进
 * 16 位布局（色相保留），scales 记下除数（其余为 1），操作完再乘回去。
 *
 * 口径要点：
 *   - **normalize 的输出是 RGBA16，不是 half float**。上游
 *     DeepLayers.swift:101 的目标是 `context(width:height:)`（hdr 默认 false），
 *     :115 又把 restore 的输入取自 `canonical(operation(...))`——同样是 16 位。
 *     只有 normalize 的**输入**和 restore 的**输出**是 half floats。这一条很容易
 *     读反，写错会让 16 位布局的值被当浮点解释，读出 0 或 65535 倍的偏差。
 *   - 归一化后**必须** ≤ 1（落回白内），否则 8 位/16 位操作会把它裁掉；
 *   - 色相保留：1.5 倍白的橙应与 1 倍白的橙在归一化后落同一色；
 *   - scales 为每像素除数、width 个一行；
 *   - restore 走预乘：乘的是预乘值（内核注释「Premultiplied colors scale with
 *     their alpha」），alpha 自己不被 scale 影响。
 */
COMP_TEST(hdr_normalize_divides_highlights_into_white_keeping_hue) {
  const size_t width = 2, height = 1, hdr_stride = width * 4 * sizeof(uint16_t), norm_stride = hdr_stride;

  std::vector<uint16_t> hdr(width * height * 4, 0);
  /* (0,0)：3 倍白的纯红 → 归一化后应是白（1,0,0 的色，落在白内）。 */
  write_half(&hdr[0], 3.0f, 0.0f, 0.0f, 1.0f);
  /* (1,0)：2 倍白的绿 → 落在 (0,1,0)。 */
  write_half(&hdr[4], 0.0f, 2.0f, 0.0f, 1.0f);

  std::vector<uint16_t> normal(hdr.size(), 0);
  std::vector<float> scales(width * height, 0.0f);
  deep_hdr_normalize(hdr.data(), hdr_stride, normal.data(), norm_stride, scales.data(), width, height);

  COMP_CHECK_MSG(close_to(scales[0], 3.0f, 0.001f), "each highlight divides by its own brightest channel");
  COMP_CHECK_MSG(close_to(scales[1], 2.0f, 0.001f), "scales is per pixel, one row of width values");

  /* 全部落回白内——这是 normalize 的全部意义。normalize 的输出是 16 位布局。 */
  for (size_t x = 0; x < width * height; ++x) {
    for (int c = 0; c < 3; ++c) {
      const float v = static_cast<float>(normal[x * 4 + c]) / 65535.0f;
      COMP_CHECK_MSG(v <= 1.0001f, "a normalized highlight must fit within white");
    }
  }
  /* 色相保留：纯红仍是纯红，纯绿仍是纯绿。16 位布局下的读数。 */
  const float first_r = static_cast<float>(normal[0]) / 65535.0f;
  const float second_g = static_cast<float>(normal[5]) / 65535.0f;
  COMP_CHECK_MSG(close_to(first_r, 1.0f, 0.01f) && close_to(normal[1], 0.0f, 0.01f) &&
                     close_to(normal[2], 0.0f, 0.01f),
                 "hue survives: a pure red highlight normalizes to pure red");
  COMP_CHECK_MSG(close_to(normal[4], 0.0f, 0.01f) && close_to(second_g, 1.0f, 0.01f),
                 "hue survives: a pure green highlight normalizes to pure green");
}

COMP_TEST(hdr_restore_reverses_normalize) {
  const size_t width = 2, height = 2, hdr_stride = width * 4 * sizeof(uint16_t), norm_stride = hdr_stride;

  /* 构造一张有高光、半透明与全透明的图。 */
  std::vector<uint16_t> hdr(width * height * 4, 0);
  const float r[] = {3.0f, 0.0f, 0.5f, 1.25f};
  const float g[] = {0.0f, 2.0f, 0.25f, 0.0f};
  const float b[] = {1.5f, 0.0f, 0.0f, 4.0f};
  const float a[] = {1.0f, 0.5f, 0.25f, 1.0f};
  for (size_t x = 0; x < width * height; ++x) write_half(&hdr[x * 4], r[x], g[x], b[x], a[x]);
  const std::vector<uint16_t> original = hdr;

  std::vector<uint16_t> normal(hdr.size(), 0);
  std::vector<float> scales(width * height, 0.0f);
  deep_hdr_normalize(hdr.data(), hdr_stride, normal.data(), norm_stride, scales.data(), width, height);

  /* 原样放回：normalize + restore 必须回到原值（半精度容差内）。 */
  deep_hdr_restore(normal.data(), norm_stride, scales.data(), hdr.data(), hdr_stride, width, height);
  for (size_t i = 0; i < width * height; ++i) {
    const HalfPixel before = read_half(&original[i * 4]);
    const HalfPixel after = read_half(&hdr[i * 4]);
    COMP_CHECK_MSG(close_to(before.r, after.r, 0.002f) && close_to(before.g, after.g, 0.002f) &&
                       close_to(before.b, after.b, 0.002f) && close_to(before.a, after.a, 0.002f),
                   "normalize then restore must be the identity");
  }
}

COMP_TEST(hdr_restore_multiplies_premultiplied_colors_not_alpha) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);

  /* 不透明的 4 倍白，归一化落在白内，restore 必须把颜色乘回 4 倍白，而 alpha 仍是 1。 */
  std::vector<uint16_t> hdr(4, 0);
  write_half(hdr.data(), 4.0f, 2.0f, 1.0f, 1.0f);

  std::vector<uint16_t> normal(4, 0);
  std::vector<float> scales(1, 0.0f);
  deep_hdr_normalize(hdr.data(), stride, normal.data(), stride, scales.data(), width, height);
  COMP_CHECK_MSG(close_to(scales[0], 4.0f, 0.001f), "brightest channel is the divisor");

  deep_hdr_restore(normal.data(), stride, scales.data(), hdr.data(), stride, width, height);
  const HalfPixel p = read_half(hdr.data());
  COMP_CHECK_MSG(close_to(p.r, 4.0f, 0.01f), "the brightest channel comes back brighter than white");
  COMP_CHECK_MSG(close_to(p.g, 2.0f, 0.01f), "other channels keep their proportion");
  COMP_CHECK_MSG(close_to(p.b, 1.0f, 0.01f), "other channels keep their proportion");
  COMP_CHECK_MSG(close_to(p.a, 1.0f, 0.002f), "alpha is coverage and is never scaled");
}

/*
 * 上游 :122-124：「Highlights take the operation's way with color, and dim with it:
 * a Levels or Curves pulling white down pulls them down in proportion.」
 * 这条护栏模拟「Levels 把白点从 1.0 拉到 0.5」这个操作：normalize → 操作 → restore。
 * 操作把白内的 1.0 压到 0.5，于是 4 倍白的高光应落在 2.0——按比例变暗，
 * 而不是被裁到 1.0。
 */
COMP_TEST(hdr_levels_op_on_highlights_dims_in_proportion) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);

  std::vector<uint16_t> hdr(4, 0);
  write_half(hdr.data(), 4.0f, 4.0f, 4.0f, 1.0f);

  std::vector<uint16_t> normal(4, 0);
  std::vector<float> scales(1, 0.0f);
  deep_hdr_normalize(hdr.data(), stride, normal.data(), stride, scales.data(), width, height);

  /* 「Levels 拉白点」：对 16 位缓冲里的每个通道乘 0.5（走 16 位整数，
   * 与 16 位操作的实际精度一致）。 */
  for (int c = 0; c < 3; ++c) normal[c] = static_cast<uint16_t>(normal[c] / 2);

  deep_hdr_restore(normal.data(), stride, scales.data(), hdr.data(), stride, width, height);
  const HalfPixel p = read_half(hdr.data());
  COMP_CHECK_MSG(close_to(p.r, 2.0f, 0.02f), "a highlight dims with the operation instead of being clipped at white");
  COMP_CHECK_MSG(p.r > 1.0f, "the highlight stays brighter than white");
}

/*
 * deep_hdr_exposure：上游 :153-159 与 :159 的实参。HDR 上精确执行，不在白点
 * 裁切，「so turning it down brings highlights into view」。
 *
 * 口径要点：
 *   - 顺序是「解码到线性光 → 乘 scale → 加 offset → 取 inverseGamma 次幂 → 编码回去」
 *     （内核 deep_linear/deep_encoded 是 extended sRGB 的曲线，白点以上按同一曲线
 *     延伸、黑点以下镜像），与 8 位 `ExposureSettings.table`
 *     （ImageAdjustments.swift:57-63）同序。
 *   - **inverseGamma 是 `1/gamma`，而 gamma 默认 1.0**（ImageAdjustments.swift:45），
 *     所以上游正常路径传进来的就是 1.0。写成 1/2.2 会多套一层伽马——这是一个
 *     很容易踩的坑：内核不校验这个值，错了也照样跑出结果。
 *   - 因为在**线性光**里缩放再编码回来，编码后的数值并不等于「原值除以 2 的幂」。
 *     8 的两档下压在线性域是 /4，编码回来是 4.46 而不是 2。
 */
COMP_TEST(hdr_exposure_round_trips_a_stop_up_and_down) {
  const size_t width = 2, height = 1, stride = width * 4 * sizeof(uint16_t);
  const float inverse_gamma = 1.0f; /* 上游传 Float(1 / settings.gamma)，gamma 默认 1 */

  std::vector<uint16_t> pixels(8, 0);
  write_half(&pixels[0], 0.5f, 0.25f, 0.8f, 1.0f);
  write_half(&pixels[4], 4.0f, 2.0f, 0.1f, 1.0f); /* 一个比白亮的像素 */
  const std::vector<uint16_t> original = pixels;

  deep_hdr_exposure(pixels.data(), stride, width, height, 2.0f, 0.0f, inverse_gamma);
  /* 提一档后，4.0 编码值的高光必须**高于** 4——不能在白点被裁。 */
  COMP_CHECK_MSG(half_to_float(pixels[4]) > 4.0f, "exposure up must not clip at white");

  deep_hdr_exposure(pixels.data(), stride, width, height, 0.5f, 0.0f, inverse_gamma);
  for (size_t i = 0; i < 8; ++i) {
    COMP_CHECK_MSG(close_to(half_to_float(original[i]), half_to_float(pixels[i]), 0.01f),
                   "a stop up then a stop down returns the pixel");
  }
}

COMP_TEST(hdr_exposure_turning_down_brings_highlights_into_view) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);
  const float inverse_gamma = 1.0f;

  std::vector<uint16_t> pixels(4, 0);
  write_half(pixels.data(), 8.0f, 8.0f, 8.0f, 1.0f);

  /* 降两档：线性域 /4。编码回来是 4.46，不是 2——因为缩放发生在线性光里。 */
  deep_hdr_exposure(pixels.data(), stride, width, height, 0.25f, 0.0f, inverse_gamma);
  const float dimmed = half_to_float(pixels[0]);
  COMP_CHECK_MSG(close_to(dimmed, 4.46f, 0.1f), "two stops down divides the highlight by four in linear light");
  COMP_CHECK_MSG(dimmed > 1.0f, "the highlight is still brighter than white after coming down");
  COMP_CHECK_MSG(dimmed < 8.0f, "it really did come down");
}

/* inverseGamma 确实被应用：传 1/2.2 与传 1.0 必须给出不同的结果（口径不是死的）。 */
COMP_TEST(hdr_exposure_applies_the_gamma_it_is_given) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);

  std::vector<uint16_t> a(4, 0), b(4, 0);
  write_half(a.data(), 0.5f, 0.5f, 0.5f, 1.0f);
  write_half(b.data(), 0.5f, 0.5f, 0.5f, 1.0f);
  deep_hdr_exposure(a.data(), stride, width, height, 1.0f, 0.0f, 1.0f);
  deep_hdr_exposure(b.data(), stride, width, height, 1.0f, 0.0f, 1.0f / 2.2f);
  COMP_CHECK_MSG(!close_to(half_to_float(a[0]), half_to_float(b[0]), 0.02f),
                 "inverseGamma is applied as given; upstream's default is 1, not 1/2.2");
}

/* Exposure 不动 alpha：alpha 是 coverage。 */
COMP_TEST(hdr_exposure_leaves_alpha_alone) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> pixels(4, 0);
  write_half(pixels.data(), 0.5f, 0.5f, 0.5f, 0.5f);

  deep_hdr_exposure(pixels.data(), stride, width, height, 4.0f, 0.1f, 1.0f);
  COMP_CHECK_MSG(close_to(half_to_float(pixels[3]), 0.5f, 0.002f), "exposure never touches alpha");
}

/*
 * deep_extract_alpha_hdr / deep_restore_alpha_hdr：剪贴栈的表面在上游是
 * 「抽出 alpha → 颜色置为不透明 → 被剪贴的层满覆盖混合 → alpha 放回」
 * （LiveMaskRenderer.drawComposite）。上游 :84 的实现对 alpha 为 0 的像素同样
 * 把颜色置 0（不是除以 0）。
 */
COMP_TEST(hdr_extract_alpha_makes_colors_opaque_and_restores) {
  const size_t width = 2, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> pixels(width * height * 4, 0);
  write_half(&pixels[0], 0.5f, 0.25f, 0.125f, 0.5f); /* 半透明，预乘 */
  write_half(&pixels[4], 0.25f, 0.5f, 0.75f, 0.25f); /* 更半透明 */
  const std::vector<uint16_t> original = pixels;

  std::vector<float> alpha(width * height, 0.0f);
  deep_extract_alpha_hdr(pixels.data(), stride, alpha.data(), width, height);

  COMP_CHECK_MSG(close_to(alpha[0], 0.5f, 0.002f) && close_to(alpha[1], 0.25f, 0.002f),
                 "each pixel's alpha is saved as a float, one row of width values");
  for (size_t x = 0; x < width; ++x) {
    const HalfPixel p = read_half(&pixels[x * 4]);
    COMP_CHECK_MSG(close_to(p.a, 1.0f, 0.002f),
                   "the surface becomes opaque so the clipped layers blend at full coverage");
  }
  /* 颜色反预乘回不透明的值（上游 :84），而不是按 1/alpha 缩放。 */
  COMP_CHECK_MSG(close_to(read_half(&pixels[0]).r, 1.0f, 0.01f), "colors are un-premultiplied, not scaled by 1/alpha");

  deep_restore_alpha_hdr(pixels.data(), stride, alpha.data(), width, height);
  for (size_t x = 0; x < width; ++x) {
    const HalfPixel before = read_half(&original[x * 4]);
    const HalfPixel after = read_half(&pixels[x * 4]);
    COMP_CHECK_MSG(close_to(before.r, after.r, 0.002f) && close_to(before.g, after.g, 0.002f) &&
                       close_to(before.b, after.b, 0.002f) && close_to(before.a, after.a, 0.002f),
                   "extract then restore returns the pixel");
  }
}

/*
 * alpha 为 0 的像素：上游 :84 是 `a > 0 ? deep_half(p[c]) / a : 0`——
 * 颜色**置零**，不是除以 0。一层里只要有一个 alpha=0 而通道有残留的像素，
 * 少了这个分支就会把整张图变成 NaN，然后静默渲成全透明且不报任何错。
 * 这是本文件里唯一一条「输入本身不合规」的口径，单独钉死。
 */
COMP_TEST(hdr_extract_alpha_zeroes_transparent_pixels_instead_of_dividing_by_zero) {
  const size_t width = 1, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> pixels(4, 0);
  write_half(pixels.data(), 2.0f, 4.0f, 6.0f, 0.0f); /* alpha=0 却有高光残留 */

  std::vector<float> alpha(1, 0.0f);
  deep_extract_alpha_hdr(pixels.data(), stride, alpha.data(), width, height);
  for (int c = 0; c < 3; ++c) {
    const float v = half_to_float(pixels[c]);
    COMP_CHECK_MSG(!std::isnan(v) && close_to(v, 0.0f, 0.01f),
                   "a transparent pixel's colors go to zero rather than dividing by zero");
  }

  deep_restore_alpha_hdr(pixels.data(), stride, alpha.data(), width, height);
  for (int c = 0; c < 4; ++c) COMP_CHECK_MSG(!std::isnan(half_to_float(pixels[c])), "no NaN survives");
}

/*
 * deep_take_painted_hdr：上游 BrushStroke.swift:1116-1117 与 DeepLayers.swift:68。
 * 笔刷画的 8 位像素（painted）盖到 HDR 图上时，只在 painted 与 base 不同的地方
 * 取 8 位值，其余保持半精度——否则笔刷整块 tile 覆盖的区域会把半精度全抹回 8 位。
 */
COMP_TEST(hdr_take_painted_only_touches_painted_pixels) {
  const size_t width = 3, height = 1, hdr_stride = width * 4 * sizeof(uint16_t), stride = width * 4;

  std::vector<uint16_t> hdr(width * height * 4, 0);
  write_half(&hdr[0], 3.0f, 1.0f, 0.5f, 1.0f);    /* 像素 0：未上笔刷的高光 */
  write_half(&hdr[4], 2.0f, 2.0f, 2.0f, 1.0f);    /* 像素 1：未上笔刷 */
  write_half(&hdr[8], 1.5f, 1.0f, 0.25f, 0.5f);   /* 像素 2：上笔刷 */
  const std::vector<uint16_t> original = hdr;

  /* 只有像素 2 的四个通道与 base 不同。 */
  const uint8_t base[12] = {0, 0, 0, 255, 128, 128, 128, 255, 64, 32, 16, 128};
  const uint8_t painted[12] = {0, 0, 0, 255, 128, 128, 128, 255, 200, 100, 50, 200};
  deep_take_painted_hdr(hdr.data(), hdr_stride, painted, base, stride, width, height);

  /* 未上笔刷的两个像素（各 4 个 uint16，共 16 字节）必须逐字节不变。 */
  COMP_CHECK_MSG(std::memcmp(hdr.data(), original.data(), 8 * sizeof(uint16_t)) == 0,
                 "a pixel the brush never touched keeps its half floats");
  /* 上笔刷的那像素取 8 位的值（按 /255 进半精度）。 */
  const HalfPixel p = read_half(&hdr[8]);
  COMP_CHECK_MSG(close_to(p.r, 200.0f / 255.0f, 0.002f) && close_to(p.a, 200.0f / 255.0f, 0.002f),
                 "a painted pixel takes the 8-bit value");
  COMP_CHECK_MSG(close_to(p.g, 100.0f / 255.0f, 0.002f) && close_to(p.b, 50.0f / 255.0f, 0.002f),
                 "every channel of a painted pixel is taken");
}

/* ===========================================================================
 * C1：半精度 ↔ 16 位整数的两个转换内核（deep_half_to_u16 / deep_u16_to_half）。
 *
 * 这两个内核是 HDR 通路的**地基**：RGBA16F 图层能被接受、能合成，靠的就是
 * hdr_to_rgba8 里先走 deep_half_to_u16 再 dither。没有它们，C1 只能是「放开了
 * 入口但画面是噪声」——正是 AUDIT3 P2-13 当初要拒绝 RGBA16F 的理由。
 *
 * 上游口径（DeepLayers.swift）：
 *   - :24 canonical / :31 image(_:)：把 HDR 图画进 16 位 premultiplied sRGB
 *     context —— 白点以上的光被**截断**到 65535，NaN/Inf 落不进来。
 *   - :37 dithered(_:)：在这份 16 位布局上加约一阶噪声取整成 8 位（由 hdr_to_rgba8
 *     调用，不在本文件测）。
 * =========================================================================== */

/* [C1-1] 白点及以下线性映射：0.0→0、1.0→65535、0.5→32768（±1 取整）。
 * 预乘关系保持：分量与 alpha 各自独立换算，不做反预乘。 */
COMP_TEST(half_to_u16_maps_the_unit_range_linearly) {
  const size_t width = 4, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> half(width * 4, 0);
  write_half(&half[0], 1.0f, 1.0f, 1.0f, 1.0f);     /* 不透明白 → 65535 */
  write_half(&half[4], 0.0f, 0.0f, 0.0f, 0.0f);     /* 全透明 → 0 */
  write_half(&half[8], 0.5f, 0.25f, 0.5f, 0.5f);    /* 半不透明 → 一半 */
  /* 半透明像素按**预乘**给：rgb 0.5×0.5 = 0.25。 */
  write_half(&half[12], 0.25f, 0.125f, 0.0f, 0.5f);

  std::vector<uint16_t> out(width * 4, 0xaaaa);
  deep_half_to_u16(half.data(), stride, out.data(), stride, width, height);

  COMP_CHECK_MSG(out[3] == 65535, "half 1.0 maps to full scale");
  COMP_CHECK_MSG(out[7] == 0, "half 0.0 maps to zero");
  COMP_CHECK_MSG(std::abs(static_cast<int>(out[8]) - 32768) <= 1, "half 0.5 maps to mid scale");
  COMP_CHECK_MSG(std::abs(static_cast<int>(out[11]) - 32768) <= 1, "half alpha 0.5 maps to mid scale");
  COMP_CHECK_MSG(std::abs(static_cast<int>(out[12]) - 16384) <= 1,
                 "a premultiplied half colour is mapped as it is, not unpremultiplied");
  COMP_CHECK_MSG(std::abs(static_cast<int>(out[13]) - 8192) <= 1,
                 "a premultiplied quarter of a colour maps to a quarter scale");
  COMP_CHECK_MSG(out[14] == 0, "a premultiplied channel of zero stays zero");
}

/* [C1-2] 白点以上的光**饱和**而不是回绕。
 *
 * 这是本组最关键的一条：half 最大 65504，若直接 `(uint16_t)(v*65535)` 会被截成
 * 低 16 位 —— 65504×65535 的低 16 位是一个随机乱值，画面出现彩色噪点。
 * 上游是画进 16 位 context，被钳到白。 */
COMP_TEST(half_to_u16_saturates_above_white) {
  const size_t width = 4, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> half(width * 4, 0);
  write_half(&half[0], 2.0f, 2.0f, 2.0f, 1.0f);       /* 刚过白点 */
  write_half(&half[4], 65504.0f, 65504.0f, 65504.0f, 1.0f); /* half 的最大值 */
  write_half(&half[8], 1e30f, 1e30f, 1e30f, 1.0f);   /* 远超 half 范围 → +Inf */
  /* 真 +Inf 的位模式是 0x7C00（指数全 1、尾数全 0）；0x7FFF 是 NaN，落 0。 */
  half[12] = 0x7c00;
  half[13] = 0x7c00;
  half[14] = 0x7c00;
  half[15] = 0x3c00;

  std::vector<uint16_t> out(width * 4, 0);
  deep_half_to_u16(half.data(), stride, out.data(), stride, width, height);

  for (size_t i = 0; i < 4; ++i) {
    COMP_CHECK_MSG(out[i] == 65535, "half 2.0 clamps to full scale");
  }
  for (size_t i = 4; i < 8; ++i) {
    COMP_CHECK_MSG(out[i] == 65535, "half 65504 (its maximum) clamps instead of wrapping");
  }
  for (size_t i = 8; i < 12; ++i) {
    COMP_CHECK_MSG(out[i] == 65535, "a value past half's range becomes +Inf and clamps to full scale");
  }
  for (size_t i = 12; i < 15; ++i) {
    COMP_CHECK_MSG(out[i] == 65535, "+Inf clamps to full scale");
  }
}

/* [C1-3] 负值与 NaN 落 0，而 +Inf 落上限——两者刻意不同。
 *
 * 负光不是有效颜色，16 位 sRGB context 也存不下 → 0。NaN 同样落 0（不是白）：
 * 把一个坏像素映成白会在画面上打出刺眼亮斑，映成 0 最多是它透明，破坏面小得多。
 * +Inf 则是「光超过表示范围」，语义上就是白 → 上限。 */
COMP_TEST(half_to_u16_clamps_negatives_and_nan_to_zero) {
  const size_t width = 4, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> half(width * 4, 0);
  half[0] = ref_half(-1.0f);            /* -1.0 → 0 */
  half[1] = ref_half(-0.5f);
  half[2] = ref_half(-0.0f);            /* -0.0 → 0 */
  half[3] = ref_half(-65504.0f);
  half[4] = 0x7fff;                     /* NaN（指数全 1、尾数非 0）→ 0，不是白 */
  half[5] = 0xffff;
  half[6] = 0x0001;                     /* 最小次正规数 → 0 */
  half[7] = ref_half(2.5f);

  std::vector<uint16_t> out(width * 4, 0xaaaa);
  deep_half_to_u16(half.data(), stride, out.data(), stride, width, height);

  COMP_CHECK_MSG(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0,
                 "negative halves clamp to zero rather than wrapping to 65535");
  COMP_CHECK_MSG(out[4] == 0 && out[5] == 0,
                 "NaN clamps to zero, not to white: one bad pixel must not become a bright spot");
  COMP_CHECK_MSG(out[6] == 0, "a subnormal half rounds to zero");
  COMP_CHECK_MSG(out[7] == 65535, "half 2.5 saturates rather than wrapping");
}

/* [C1-4] 逆方向：16 位整数 → half。0→0、65535→1.0，白点仍是 1.0。
 * 与 normalize 的区别是这里**不**缩放高光——它就是同一量纲的另一种写法。 */
COMP_TEST(u16_to_half_maps_the_full_range_to_unit_white) {
  const size_t width = 4, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> wide(width * 4, 0);
  wide[0] = 0; wide[1] = 0; wide[2] = 0; wide[3] = 0;
  wide[4] = 65535; wide[5] = 65535; wide[6] = 65535; wide[7] = 65535;
  wide[8] = 32768; wide[9] = 32768; wide[10] = 32768; wide[11] = 32768;

  std::vector<uint16_t> half(width * 4, 0xaaaa);
  deep_u16_to_half(wide.data(), stride, half.data(), stride, width, height);

  COMP_CHECK_MSG(half_to_float(half[3]) == 0.0f, "0 maps to half zero");
  COMP_CHECK_MSG(close_to(half_to_float(half[7]), 1.0f, 0.0005f), "65535 maps to half 1.0 (white)");
  COMP_CHECK_MSG(close_to(half_to_float(half[11]), 0.5f, 0.0005f), "32768 maps to half 0.5");
  COMP_CHECK_MSG(half[7] == ref_half(1.0f), "white is exactly half 1.0, not some other encoding");
}

/* [C1-5] 往返：white 以下的值往返后仍在 ±1 个 8 位步长内。
 * 白点以上**不**保往返（那是 normalize/restore 的职责，不是存储转换的）。 */
COMP_TEST(half_round_trip_preserves_values_within_white) {
  const size_t width = 4, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> half(width * 4, 0);
  const float values[4] = {0.0f, 0.25f, 0.5f, 1.0f};
  for (size_t i = 0; i < 4; ++i) {
    half[i * 4 + 0] = ref_half(values[i]);
    half[i * 4 + 1] = ref_half(values[i]);
    half[i * 4 + 2] = ref_half(values[i]);
    half[i * 4 + 3] = ref_half(values[i]);
  }

  std::vector<uint16_t> wide(width * 4, 0);
  std::vector<uint16_t> back(width * 4, 0);
  deep_half_to_u16(half.data(), stride, wide.data(), stride, width, height);
  deep_u16_to_half(wide.data(), stride, back.data(), stride, width, height);

  for (size_t i = 0; i < 4; ++i) {
    /* 往返一次的容差是 half 的精度（约 2⁻¹¹），远小于 1/255。 */
    COMP_CHECK_MSG(close_to(half_to_float(back[i * 4 + 0]), values[i], 0.001f),
                   "a value within white survives the round trip");
  }
}

/* [C1-6] 两趟的顺序不可交换：half 必须先按**值**换算。
 * 若把 half 的位模式直接交给 16 位整数路径，1.0（0x3C00 = 15360）会得 15360，
 * 是白点的 23 倍——这就是「RGBA16F 放行后画面全错」的具体形态。 */
COMP_TEST(half_bit_patterns_are_not_read_as_sixteen_bit_integers) {
  const size_t width = 2, height = 1, stride = width * 4 * sizeof(uint16_t);
  std::vector<uint16_t> half(width * 4, 0);
  const uint16_t one = ref_half(1.0f);
  for (size_t i = 0; i < 4; ++i) half[i] = one;
  COMP_CHECK_MSG(one == 0x3c00, "half 1.0 is the bit pattern 0x3C00");

  std::vector<uint16_t> out(width * 4, 0);
  deep_half_to_u16(half.data(), stride, out.data(), stride, width, height);

  COMP_CHECK_MSG(out[0] == 65535,
                 "half 1.0 converts to full scale, where the bit pattern alone would give 15360");
}
