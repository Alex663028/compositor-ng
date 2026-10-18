// AI生成
/*
 * test_hdr_import.cpp — C3「浮点解码器」的行为护栏：Radiance .hdr 进来到底落成什么。
 *
 * 上游口径（compositor/Compositor/…）：
 *   IO/ImageImporter.swift:112-127  浮点来源（EXR / Radiance / float TIFF）走
 *                                   `.floatComponents`，解出来是**线性光浮点**，
 *                                   不是 8 位量化后的近似；
 *   Document/DeepLayers.swift:164-178  浮点像素过 `peak > 1.001` 闸：超白 -> HDR 层
 *                                   （half floats in extended sRGB）；否则 -> 16 位层。
 *
 * 本文件钉死三件事：
 *   1. Radiance 解码确实是线性光（RGBE 的 IEEE754 取值），不是某种"看起来像"的近似；
 *   2. 非 Radiance 文件不许从浮点通道混进来——`stbi_loadf` 对 8 位输入会静默走
 *      「8 位解码 + 转浮点」，把一个 .png 改名成 .hdr 就得到一份假的"浮点"像素；
 *      `stbi_is_hdr_from_memory` 必须在门口拦住它；
 *   3. 1.001 闸按**编码后（extended sRGB）**的值判，且预乘/反预乘的次序不能错
 *      （峰值是在编码值上量、反预乘后比）。
 *
 * 断言一律按可确定性判定写；半精度与 RGBE 尾数都有固有精度损失，取 1%~1.5% 容差。
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "domain/layer.h"
#include "format/codec_lib.h"
#include "format/comp_png.h"
#include "imaging/DeepPixels.h"
#include "test_framework.h"

namespace {

comp_engine_desc hdr_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-hdr-import-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

std::string hdr_workspace_file(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories("build/codec", ec);
  return std::string("build/codec/") + name;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const size_t written =
      bytes.empty() ? 0u : std::fwrite(bytes.data(), 1u, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

/* ------------------------------------------------------------ 参考实现 */

/* 线性光 -> extended sRGB 编码值（与内核 deep_encoded 同一口径，但独立写一遍：
 * 期望值绝不能来自被测实现）。 */
float ref_encode(float linear) {
  const float magnitude = std::fabs(linear);
  const float encoded = magnitude <= 0.0031308f
                            ? magnitude * 12.92f
                            : 1.055f * std::pow(magnitude, 1.0f / 2.4f) - 0.055f;
  return linear < 0.0f ? -encoded : encoded;
}

/* IEEE 754 binary16 -> float。 */
float half_to_float(uint16_t bits) {
  const uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31;
  const uint32_t exponent = (bits >> 10) & 0x1fu;
  const uint32_t mantissa = bits & 0x3ffu;
  union {
    uint32_t u;
    float f;
  } out;
  if (exponent == 0u) {
    const float value = static_cast<float>(mantissa) * (1.0f / 16777216.0f);
    return sign != 0u ? -value : value;
  }
  if (exponent == 31u) {
    out.u = sign | 0x7f800000u | (mantissa << 13);
  } else {
    out.u = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  }
  return out.f;
}

bool close_to(float got, float want, float tolerance) {
  return std::fabs(got - want) <= tolerance;
}

/* ---------------------------------------------------- Radiance 构造器 */

/*
 * 线性光浮点 -> Radiance 的共享指数 RGBE。
 * 解码侧（stb_image.h 的 stbi__hdr_convert）是
 *     value = mantissa * 2^(exponent - 136)
 * 且三通道**共用一个指数**，所以指数取三通道最大值那一档，尾数按比例给：
 * 最大值落在 [128,255]，其余通道同比例缩小。
 */
void rgbe_encode(float r, float g, float b, uint8_t out[4]) {
  const float max = std::fmax(r, std::fmax(g, b));
  if (!(max > 1e-32f)) {
    out[0] = out[1] = out[2] = out[3] = 0u;
    return;
  }
  int exponent = 0;
  std::frexp(max, &exponent); /* max = m * 2^exponent, m ∈ [0.5, 1) */
  const float scale = std::ldexp(1.0f, 8 - exponent);
  float mantissa[3] = {r * scale, g * scale, b * scale};
  for (int c = 0; c < 3; ++c) {
    if (!(mantissa[c] > 0.0f)) mantissa[c] = 0.0f;
    if (mantissa[c] > 255.0f) mantissa[c] = 255.0f;
    out[c] = static_cast<uint8_t>(mantissa[c] + 0.5f);
  }
  out[3] = static_cast<uint8_t>(exponent + 128);
}

/*
 * 平铺（非 RLE）Radiance 文件。宽度必须 < 8：stb 只在这个宽度区间走平铺读法
 * （stb_image.h:7216 `if (width < 8 || width >= 32768)`），宽度再大就要写真 RLE。
 * 测试用图都在这个区间内，构造器也就不必实现 RLE。
 */
std::vector<uint8_t> radiance_flat(int width, int height, const std::vector<float>& linear_rgba) {
  const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) +
                             " +X " + std::to_string(width) + "\n";
  std::vector<uint8_t> out(header.begin(), header.end());
  for (size_t i = 0; i + 3u < linear_rgba.size(); i += 4u) {
    uint8_t rgbe[4];
    rgbe_encode(linear_rgba[i + 0u], linear_rgba[i + 1u], linear_rgba[i + 2u], rgbe);
    out.insert(out.end(), rgbe, rgbe + 4);
  }
  return out;
}

/* 只写文件头、不写像素的 Radiance 文件（伪造尺寸用）。 */
std::vector<uint8_t> radiance_header_only(int width, int height) {
  return radiance_flat(width, height, std::vector<float>());
}

/* 一张合法的不透明 PNG（用于「改名成 .hdr 的 8 位图」）。 */
std::vector<uint8_t> small_png(int width, int height) {
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255u);
  std::vector<uint8_t> png;
  std::string error;
  if (!compositor::png_encode_rgba8(rgba.data(), width, height, &png, &error)) return {};
  return png;
}

void free_engine(comp_engine engine) { comp_engine_destroy(engine); }

}  // namespace

/* ------------------------------------------------------------ 解码口径 */

COMP_TEST(radiance_float_decode_returns_linear_light_floats) {
  /* 三通道共用一个指数：这一格最大值 4.0，它的尾数是 128（= 4.0 * 2^-5），
   * 于是 1.0 -> 32，0.25 -> 8，解码回来分别是 4 / 1 / 0.25。
   * RGBE 是**非负**容器（负光没有表示法），所以这里只放非负样本；负值走
   * 内核侧的 deep_hdr_take_linear（见下面那条测试）。 */
  const std::vector<float> pixels = {
      4.0f, 1.0f, 0.25f, 1.0f, /* 超白 */
      1.0f, 1.0f, 1.0f, 1.0f,  /* 刚好白 */
      0.5f, 0.25f, 0.125f, 1.0f,
      0.001f, 0.001f, 0.001f, 1.0f /* 很小的值：指数为负，尾数仍要给对 */
  };
  const std::vector<uint8_t> file = radiance_flat(4, 1, pixels);
  COMP_CHECK(!file.empty());

  compositor::CodecFloatImage image;
  std::string error;
  COMP_CHECK_MSG(compositor::codec_lib_decode_float(file.data(), file.size(),
                                                   static_cast<uint32_t>(COMP_FORMAT_HDR), &image,
                                                   &error),
                 "a valid Radiance file must decode through the float channel");
  COMP_CHECK(image.width == 4 && image.height == 1);
  COMP_CHECK(image.rgba.size() == 4u * 4u);
  if (image.rgba.size() == 4u * 4u) {
    /* 线性光、直通 alpha：Radiance 没有 alpha 通道，补 1.0。 */
    for (size_t i = 0; i < 4u; ++i) {
      COMP_CHECK_MSG(close_to(image.rgba[i * 4u + 3u], 1.0f, 0.0f),
                     "Radiance has no alpha; the float channel must fill 1.0");
    }
    const float tolerance = 0.02f;
    COMP_CHECK(close_to(image.rgba[0u], 4.0f, 4.0f * tolerance));
    COMP_CHECK(close_to(image.rgba[1u], 1.0f, tolerance));
    COMP_CHECK(close_to(image.rgba[2u], 0.25f, 0.25f * tolerance));
    COMP_CHECK(close_to(image.rgba[4u], 1.0f, tolerance));
    COMP_CHECK(close_to(image.rgba[8u], 0.5f, tolerance));
    COMP_CHECK(close_to(image.rgba[9u], 0.25f, 0.25f * tolerance));
    COMP_CHECK(close_to(image.rgba[10u], 0.125f, 0.125f * tolerance));
    COMP_CHECK(close_to(image.rgba[12u], 0.001f, 0.001f * tolerance));
    COMP_CHECK(close_to(image.rgba[13u], 0.001f, 0.001f * tolerance));

    /* 「线性光」的反面证据：如果这一层被 8 位 sRGB 化过，4.0 会被压到 1.0，1.0 会变成
     * 约 0.0039 —— 上面那些等值断言已经排除了这种量化。下面这条把「超白」单独钉一次。 */
    COMP_CHECK_MSG(image.rgba[0u] > 3.9f, "values above white must survive the decode");
  }

  /* 能力口径：Radiance 只有浮点通道，两条 API 必须说同一件事（单一真源）。 */
  COMP_CHECK(compositor::codec_lib_can_decode_float(static_cast<uint32_t>(COMP_FORMAT_HDR)));
  COMP_CHECK(compositor::codec_lib_can_decode(static_cast<uint32_t>(COMP_FORMAT_HDR)) ==
             compositor::codec_lib_can_decode_float(static_cast<uint32_t>(COMP_FORMAT_HDR)));
  COMP_CHECK(!compositor::codec_lib_can_decode_float(static_cast<uint32_t>(COMP_FORMAT_PNG)));
  COMP_CHECK(!compositor::codec_lib_can_decode_float(static_cast<uint32_t>(COMP_FORMAT_JPEG)));
}

/*
 * stb 的浮点入口对 8 位输入会静默降级：`stbi_loadf` 内部走
 * `stbi__load_and_postprocess_8bit` + `stbi__ldr_to_hdr`（stb_image.h:1472-1474），
 * 于是一张 .png 改名成 .hdr 就得到一份"看起来合法、实则 8 位量化"的浮点像素。
 * 浮点通道必须用 stbi_is_hdr_from_memory 在门口拦住它——这是本项目禁止的假装成功。
 */
COMP_TEST(radiance_float_channel_refuses_a_file_that_is_not_radiance) {
  const std::vector<uint8_t> png = small_png(4, 2);
  COMP_CHECK(!png.empty());

  /* 前提：这份字节本身是**合法图像**（8 位通道解得开），所以下面那次拒绝
   * 只可能来自「这不是 Radiance」这个判定，而不是文件坏了。 */
  compositor::CodecImage eight_bit;
  std::string error;
  COMP_CHECK(compositor::codec_lib_decode(png.data(), png.size(),
                                          static_cast<uint32_t>(COMP_FORMAT_PNG), &eight_bit,
                                          &error));

  compositor::CodecFloatImage floats;
  error.clear();
  COMP_CHECK_MSG(!compositor::codec_lib_decode_float(png.data(), png.size(),
                                                     static_cast<uint32_t>(COMP_FORMAT_HDR), &floats,
                                                     &error),
                 "a non-Radiance payload must not decode through the float channel");
  COMP_CHECK(std::strstr(error.c_str(), "not a Radiance HDR file") != nullptr);
  COMP_CHECK(floats.empty());

  /* 走 ABI：.hdr 扩展名 + 8 位内容 -> IO 错误，绝不产出文档。 */
  comp_engine_desc desc = hdr_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);
  const std::string path = hdr_workspace_file("mislabeled.hdr");
  COMP_CHECK(write_bytes(path, png));

  comp_document document = nullptr;
  COMP_CHECK(comp_io_import(engine, path.c_str(), &document) == COMP_ERR_IO);
  COMP_CHECK(document == nullptr);
  COMP_CHECK(std::strstr(comp_engine_last_error(engine), "not a Radiance HDR file") != nullptr);
  free_engine(engine);
}

/* ------------------------------------------------------- 入库口径（内核） */

/*
 * deep_hdr_take_linear：文件里的**线性光**浮点进 HDR 图层的唯一入口。
 * 上游 DeepLayers.swift:81-84 的 HDR 层是 extended sRGB 的 half floats，
 * 所以这里必须过编码曲线，且先编颜色、再乘 alpha（预乘）。
 */
COMP_TEST(hdr_take_linear_encodes_through_extended_srgb) {
  const size_t width = 6, height = 1;
  const std::vector<float> linear = {
      1.0f,   1.0f,   1.0f,  1.0f, /* 白 */
      1.05f,  1.05f,  1.05f, 1.0f, /* 擦着白 */
      4.0f,   4.0f,   4.0f,  1.0f, /* 超白 */
      0.001f, 0.001f, 0.001f, 1.0f, /* 曲线线性段以下 */
      -0.5f,  -0.5f,  -0.5f, 1.0f, /* 黑以下：extended sRGB 镜像，不裁到 0 */
      0.5f,   0.5f,   0.5f,  0.5f /* 半透明：颜色按 alpha 预乘 */
  };
  std::vector<uint16_t> out(width * height * 4u, 0u);
  deep_hdr_take_linear(linear.data(), width * 4u * sizeof(float), out.data(),
                       width * 4u * sizeof(uint16_t), width, height);

  const float tolerance = 0.002f;
  COMP_CHECK(close_to(half_to_float(out[0]), 1.0f, tolerance));
  COMP_CHECK_MSG(close_to(half_to_float(out[4]), ref_encode(1.05f), tolerance),
                 "1.05 must land on the extended-sRGB encoding of 1.05 (about 1.0217)");
  COMP_CHECK(close_to(half_to_float(out[8]), ref_encode(4.0f), 0.01f));
  COMP_CHECK_MSG(half_to_float(out[8]) > 1.8f, "4.0 must stay well above white after encoding");
  COMP_CHECK(close_to(half_to_float(out[12]), 0.001f * 12.92f, 0.0001f));
  COMP_CHECK_MSG(half_to_float(out[16]) < -0.7f,
                 "below black the curve mirrors instead of clamping (extended sRGB)");

  /* 预乘：颜色 = 编码值 * alpha；alpha 通道本身是覆盖度，不编码。 */
  const float alpha = half_to_float(out[23]);
  COMP_CHECK_MSG(close_to(alpha, 0.5f, 0.001f), "alpha must stay coverage, unencoded");
  for (int c = 0; c < 3; ++c) {
    COMP_CHECK_MSG(close_to(half_to_float(out[20 + c]), ref_encode(0.5f) * 0.5f, tolerance),
                   "colour must be encoded then premultiplied by alpha");
  }
  COMP_CHECK(close_to(half_to_float(out[3]), 1.0f, 0.0f));
}

/* NaN / 越界 alpha 的收口：一个坏像素不许变成亮点，也不许把 alpha 带出 [0,1]。 */
COMP_TEST(hdr_take_linear_folds_bad_samples_onto_safe_values) {
  const float nan = std::nanf("");
  const float inf = INFINITY;
  const size_t width = 3, height = 1;
  const std::vector<float> linear = {
      nan,  nan,  nan,  1.0f,   /* 颜色 NaN -> 0 */
      1.0f, 1.0f, 1.0f,  nan,   /* alpha NaN -> 0，颜色随之归 0（透明） */
      1.0f, 1.0f, 1.0f,  inf    /* alpha 无穷 -> 钳到 1 */
  };
  std::vector<uint16_t> out(width * height * 4u, 0u);
  deep_hdr_take_linear(linear.data(), width * 4u * sizeof(float), out.data(),
                       width * 4u * sizeof(uint16_t), width, height);

  COMP_CHECK(half_to_float(out[0]) == 0.0f && half_to_float(out[1]) == 0.0f &&
             half_to_float(out[2]) == 0.0f);
  COMP_CHECK_MSG(half_to_float(out[3]) == 1.0f, "a NaN colour must not touch alpha");

  COMP_CHECK_MSG(half_to_float(out[7]) == 0.0f, "a NaN alpha must land on 0, not travel on");
  for (int c = 0; c < 3; ++c) {
    COMP_CHECK(half_to_float(out[4 + c]) == 0.0f);
  }

  COMP_CHECK_MSG(close_to(half_to_float(out[11]), 1.0f, 0.0f), "alpha is clamped to coverage");
  COMP_CHECK(close_to(half_to_float(out[8]), 1.0f, 0.002f));
}

/* ------------------------------------------------- 1.001 闸与 16 位分支 */

/*
 * wide_asset_from_linear：上游 DeepLayers.swift:164-178 的判决。
 * 「a file brushing white through rounding isn't HDR」——闸是对着**编码后**的
 * 峰值量的，所以 1.05 这种线性值编码后只有 1.0217，仍然过闸。
 *
 * 闸落在哪里很重要：白以上的 half 台阶是 2^-10，1.0 之后依次是
 * 1.000977 / 1.001953 / 1.002930——而 1.001 闸恰好卡在**前两级之间**。
 * 也就是说这道闸真正能分辨的输入必须落在 half 空间里：随手写个 1.0005
 * （编码 1.00022，half 取整又回到 1.0）既不区分 1.001 与 1.0，也不区分
 * 1.001 与 1.01。下面两例专门钉住闸的两侧。
 */
COMP_TEST(hdr_wide_asset_threshold_is_measured_on_encoded_values) {
  const int32_t width = 1, height = 1;

  struct Case {
    float linear;
    float alpha;
    comp_pixel_format want;
    const char* why;
  };
  const Case cases[] = {
      {0.0f, 1.0f, COMP_PIXEL_RGBA16, "black is not HDR"},
      {1.0f, 1.0f, COMP_PIXEL_RGBA16, "exactly white is not HDR"},
      {1.0005f, 1.0f, COMP_PIXEL_RGBA16, "a file brushing white through rounding isn't HDR"},
      {1.0023f, 1.0f, COMP_PIXEL_RGBA16,
       "the first half step above white (1.000977) still sits below the gate"},
      {1.0046f, 1.0f, COMP_PIXEL_RGBA16F,
       "the next half step (1.001953) is the first one the gate admits"},
      {1.05f, 1.0f, COMP_PIXEL_RGBA16F, "1.05 encodes to 1.0217, which is over the gate"},
      {4.0f, 1.0f, COMP_PIXEL_RGBA16F, "well over white is HDR"},
  };

  /*
   * 上面两例的说辞靠的是 half 取整落点，不能只写在注释里：编码值必须真的落在
   * 对应台阶的取整区间内（1.000977 的区间是 [1.0004883, 1.0014648)，
   * 1.001953 的是 [1.0014648, 1.0024414)）。这一断言若漂了，说明我写的输入
   * 已经不是想验的那个边界，闸的两侧也就没被真正钉住。
   */
  COMP_CHECK_MSG(ref_encode(1.0023f) >= 1.0004883f && ref_encode(1.0023f) < 1.0014648f,
                 "1.0023 must encode into the half step that rounds back to 1.000977");
  COMP_CHECK_MSG(ref_encode(1.0046f) >= 1.0014648f && ref_encode(1.0046f) < 1.0024414f,
                 "1.0046 must encode into the half step 1.001953");

  for (const Case& item : cases) {
    const float linear[4] = {item.linear, item.linear, item.linear, item.alpha};
    compositor::ImageBuffer out;
    COMP_CHECK_MSG(compositor::wide_asset_from_linear(linear, width, height, &out), item.why);
    if (out.empty()) continue;
    COMP_CHECK_MSG(out.format == item.want, item.why);
    COMP_CHECK(out.width == width && out.height == height);
    COMP_CHECK_MSG(out.stride() == static_cast<size_t>(width) * 8u,
                   "wide assets are 8 bytes per pixel");
    if (out.format == COMP_PIXEL_RGBA16) {
      const uint16_t* p = reinterpret_cast<const uint16_t*>(out.pixel_data().data());
      const float stored = static_cast<float>(p[0]) / 65535.0f;
      COMP_CHECK_MSG(close_to(stored, ref_encode(item.linear), 0.002f),
                     "a 16-bit asset stores the encoded value, not the linear one");
      /* 16 位层里「擦着白」必须真的落在 65535：向上取整到 16 位即饱和白。 */
      if (item.linear >= 1.0f) COMP_CHECK(p[0] == 65535u);
    } else {
      const uint16_t* p = reinterpret_cast<const uint16_t*>(out.pixel_data().data());
      COMP_CHECK_MSG(close_to(half_to_float(p[0]), ref_encode(item.linear), 0.01f),
                     "an HDR asset stores the encoded value in half precision");
      COMP_CHECK(close_to(half_to_float(p[3]), item.alpha, 0.002f));
    }
  }

  /* 峰值必须**反预乘**后比：半透明 + 超白，预乘值是 1.8248*0.5 = 0.9124（在白以内），
   * 反预乘回来才是 1.8248。按预乘值判会把这一层错判成 16 位层。 */
  {
    const float linear[4] = {4.0f, 4.0f, 4.0f, 0.5f};
    compositor::ImageBuffer out;
    COMP_CHECK(compositor::wide_asset_from_linear(linear, 1, 1, &out));
    COMP_CHECK_MSG(out.format == COMP_PIXEL_RGBA16F,
                   "the gate is measured on unpremultiplied brightness");
    if (out.format == COMP_PIXEL_RGBA16F && !out.empty()) {
      const uint16_t* p = reinterpret_cast<const uint16_t*>(out.pixel_data().data());
      const float alpha = half_to_float(p[3]);
      COMP_CHECK(close_to(alpha, 0.5f, 0.002f));
      COMP_CHECK_MSG(close_to(half_to_float(p[0]) / alpha, ref_encode(4.0f), 0.02f),
                     "colour stays premultiplied inside the asset");
    }
  }
}

/* ------------------------------------------------------- 尺寸闸与 ABI */

/*
 * 尺寸闸必须在**解码器之前**：stb 会先按文件头声明的尺寸分配 w*h*16 字节的浮点
 * 缓冲，之后再拒绝就已经晚了（伪造一个 40000x40000 的头，stb 连自己的上界都过不去，
 * 我们看到的会是它的 "too large"）。所以护栏分两层断言：
 *   a) 一个 stb 能接受的尺寸（4x40000，超出引擎的 kMaxSide）必须被**我们**拒掉；
 *   b) 一个 stb 自己都拒的尺寸（40000x40000）也必须报**我们的**尺寸闸文案，
 *      而不是 stb 的 "too large" —— 这一条才真正证明「分配前」那一层存在。
 */
COMP_TEST(hdr_declared_size_is_gated_before_the_decoder_allocates) {
  comp_engine_desc desc = hdr_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  /* a) 4 x 40000：最长边超 kMaxSide（30000），像素数却很小，stb 完全解得开。 */
  {
    const std::vector<uint8_t> tall =
        radiance_flat(4, 40000, std::vector<float>(4u * 40000u * 4u, 0.5f));
    compositor::CodecFloatImage floats;
    std::string error;
    COMP_CHECK_MSG(compositor::codec_lib_decode_float(tall.data(), tall.size(),
                                                      static_cast<uint32_t>(COMP_FORMAT_HDR), &floats,
                                                      &error),
                   "stb accepts this file; the refusal below must be ours, not the decoder's");
    COMP_CHECK(floats.width == 4 && floats.height == 40000);

    const std::string path = hdr_workspace_file("too_tall.hdr");
    COMP_CHECK(write_bytes(path, tall));
    comp_document document = nullptr;
    COMP_CHECK(comp_io_import(engine, path.c_str(), &document) == COMP_ERR_IO);
    COMP_CHECK(document == nullptr);
    COMP_CHECK(std::strstr(comp_engine_last_error(engine), "exceeds maximum canvas size") != nullptr);
  }

  /* b) 40000 x 40000：只有文件头，没有任何像素数据。 */
  {
    const std::vector<uint8_t> huge = radiance_header_only(40000, 40000);
    const std::string path = hdr_workspace_file("too_huge.hdr");
    COMP_CHECK(write_bytes(path, huge));
    comp_document document = nullptr;
    COMP_CHECK(comp_io_import(engine, path.c_str(), &document) == COMP_ERR_IO);
    COMP_CHECK(document == nullptr);
    const std::string message = comp_engine_last_error(engine);
    COMP_CHECK_MSG(message.find("exceeds maximum canvas size") != std::string::npos,
                   "the size gate must refuse the declared size before any allocation");
    COMP_CHECK_MSG(message.find("too large") == std::string::npos,
                   "a forged header must be caught by our gate, not by the decoder's own limit");
  }

  free_engine(engine);
}

/*
 * 端到端：一张超白的 .hdr 进 comp_io_import 之后，
 *   - 扩展名（.hdr/.pic，大小写无关）映射到 COMP_FORMAT_HDR；
 *   - 登记表如实说「可读不可写」；
 *   - 导出该格式必须 UNSUPPORTED（Radiance 只读），不是静默写个 8 位图；
 *   - 文档确实建成了（画布 = 图像尺寸），且这一层**不是 RGBA8**：
 *     .comp 把它存成 images/<uuid>.exr（C3b），manifest 声明 RGBA16F，
 *     再加载回来尺寸不变——存盘环闭合，而不是"保存必须失败"。
 *     对比组：同样大小的一张 8 位 PNG 仍然走 PNG 资产路径。
 */
COMP_TEST(hdr_import_end_to_end_takes_a_radiance_file_as_a_hdr_layer) {
  COMP_CHECK(comp_io_format_from_path("a.hdr") == COMP_FORMAT_HDR);
  COMP_CHECK(comp_io_format_from_path("a.PIC") == COMP_FORMAT_HDR);
  COMP_CHECK(comp_io_format_from_path("dir.hdr/a.png") == COMP_FORMAT_PNG);
  COMP_CHECK(comp_io_format_from_path("a.png") == COMP_FORMAT_PNG);

  comp_engine_desc desc = hdr_engine_desc();
  comp_engine engine = nullptr;
  COMP_CHECK(comp_engine_create(&desc, &engine) == COMP_OK);

  /* 登记表：Radiance 由 vendored stb 的浮点通道提供，只读。 */
  comp_codec_state state = COMP_CODEC_UNAVAILABLE;
  size_t note_size = 0;
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_HDR, &state, nullptr, 0, &note_size) == COMP_OK);
  COMP_CHECK_MSG(state == COMP_CODEC_VENDORED,
                 "Radiance comes from the vendored float channel in this tree");
  std::vector<char> note(note_size + 1u, '\0');
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_HDR, nullptr, note.data(), note.size(), &note_size) ==
             COMP_OK);
  COMP_CHECK(std::strstr(note.data(), "Radiance") != nullptr);
  COMP_CHECK_MSG(std::strstr(note.data(), "READ-ONLY") != nullptr,
                 "the note must state that Radiance export is not available");

  /* 一张超白的 4x2 Radiance。 */
  const std::vector<float> pixels = {
      4.0f, 4.0f, 4.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 2.0f, 1.0f, 1.0f,
      1.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0.1f, 0.1f, 0.1f, 1.0f, 0.25f, 0.5f,
      0.75f, 1.0f};
  const std::vector<uint8_t> file = radiance_flat(4, 2, pixels);
  const std::string hdr_path = hdr_workspace_file("bright.hdr");
  COMP_CHECK(write_bytes(hdr_path, file));

  comp_document document = nullptr;
  COMP_CHECK(comp_io_import(engine, hdr_path.c_str(), &document) == COMP_OK);
  if (document != nullptr) {
    comp_size size{};
    COMP_CHECK(comp_document_size(document, &size) == COMP_OK);
    COMP_CHECK(size.width == 4 && size.height == 2);

    comp_layer_id first = 0;
    size_t count = 0;
    COMP_CHECK(comp_layer_list(document, &first, 1u, &count) == COMP_OK);
    COMP_CHECK(count == 1u);

    /* Radiance 只读：导出必须被登记表挡下，且给出可解释的原因。 */
    const std::string out_path = hdr_workspace_file("bright_out.hdr");
    COMP_CHECK(comp_io_export(document, out_path.c_str(), COMP_FORMAT_HDR, nullptr) ==
               COMP_ERR_UNSUPPORTED);
    COMP_CHECK(std::strstr(comp_engine_last_error(engine), "export is unsupported") != nullptr);

    /*
     * C3b：HDR 图层现在能存进 .comp 了——资产是 images/<uuid>.exr
     * （线性光、直通 alpha，half 通道），manifest 声明 enginePixelFormat=2。
     * 存盘与再加载必须双通；这里只验「存得下、读得回、尺寸对」，
     * 像素精度的逐项核对在 test_exr.cpp 里对着解码器做。
     */
    const std::string project = hdr_workspace_file("hdr_project.comp");
    COMP_CHECK_MSG(comp_io_save_project(document, project.c_str()) == COMP_OK,
                   "HDR layers must be stored as EXR assets in .comp");
    const std::string manifest = read_text(project + "/manifest.json");
    COMP_CHECK_MSG(manifest.find("\"enginePixelFormat\": 2") != std::string::npos,
                   "the manifest must declare the HDR layer as RGBA16F");
    COMP_CHECK_MSG(manifest.find(".exr") != std::string::npos,
                   "the manifest must point imageFile at the EXR asset");
    comp_document_destroy(document);

    comp_document reloaded = nullptr;
    COMP_CHECK_MSG(comp_io_load_project(engine, project.c_str(), &reloaded) == COMP_OK,
                   "a saved HDR document must load back");
    if (reloaded != nullptr) {
      comp_size reloaded_size{};
      COMP_CHECK(comp_document_size(reloaded, &reloaded_size) == COMP_OK);
      COMP_CHECK(reloaded_size.width == 4 && reloaded_size.height == 2);
      comp_document_destroy(reloaded);
    }
  }

  /* 对比组：8 位 PNG 走同一条保存路径必须成功（证明上面那次失败是图层位深引起的）。 */
  {
    const std::vector<uint8_t> png = small_png(4, 2);
    const std::string png_path = hdr_workspace_file("flat.png");
    COMP_CHECK(write_bytes(png_path, png));
    comp_document flat_document = nullptr;
    COMP_CHECK(comp_io_import(engine, png_path.c_str(), &flat_document) == COMP_OK);
    if (flat_document != nullptr) {
      const std::string project = hdr_workspace_file("flat_project.comp");
      COMP_CHECK_MSG(comp_io_save_project(flat_document, project.c_str()) == COMP_OK,
                     "the same save path works for an 8-bit document");
      comp_document_destroy(flat_document);
    }
  }

  free_engine(engine);
}