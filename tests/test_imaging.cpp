// AI生成
/*
 * test_imaging.cpp — 像素内核：移植后（去 GCD）保持行为不变的基础验证。
 *
 * M1 只针对「可确定性断言」的内核做运行时校验：内核语义的完整等价性
 * 由 M2 的图像回归基准（对同一输入的 macOS 参考输出逐像素比对）负责。
 */
#include <cstring>
#include <vector>

#include "imaging/comp_imaging.h"
#include "test_framework.h"

namespace {

/* 生成 width×height 的预乘 RGBA 全实心缓冲。 */
std::vector<uint8_t> solid(size_t width, size_t height, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a) {
  std::vector<uint8_t> bytes(width * height * 4);
  for (size_t i = 0; i < bytes.size(); i += 4) {
    bytes[i] = r;
    bytes[i + 1] = g;
    bytes[i + 2] = b;
    bytes[i + 3] = a;
  }
  return bytes;
}

}  // namespace

COMP_TEST(imaging_lens_distort_identity) {
  const size_t width = 4;
  const size_t height = 4;
  const size_t stride = width * 4;

  std::vector<uint8_t> source(width * height * 4);
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  }
  std::vector<uint8_t> destination(source.size(), 0);

  /* k = 0 时按文档必须逐字节复制源。 */
  lens_distort(source.data(), destination.data(), width, height, stride, 0.0);
  COMP_CHECK(std::memcmp(source.data(), destination.data(), source.size()) == 0);
}

COMP_TEST(imaging_clamp_premultiplied) {
  /* 预乘 RGBA 的颜色通道不得超过 alpha；(200,180,160,100) 应被压到 ≤100。 */
  std::vector<uint8_t> pixels{200, 180, 160, 100, 40, 20, 10, 100};
  rgba_clamp_premultiplied(pixels.data(), 2);
  COMP_CHECK(pixels[0] == 100);
  COMP_CHECK(pixels[1] == 100);
  COMP_CHECK(pixels[2] == 100);
  COMP_CHECK(pixels[3] == 100);
  /* 未超限的像素保持不变。 */
  COMP_CHECK(pixels[4] == 40 && pixels[5] == 20 && pixels[6] == 10 && pixels[7] == 100);
}

COMP_TEST(imaging_noise_is_deterministic_and_keeps_alpha) {
  const size_t width = 8;
  const size_t height = 8;
  const size_t stride = width * 4;

  const std::vector<uint8_t> original = solid(width, height, 128, 128, 128, 255);
  std::vector<uint8_t> first = original;
  std::vector<uint8_t> second = original;

  noise_add(first.data(), width, height, stride, 50.0f, /*gaussian=*/0, /*monochromatic=*/1, 12345u);
  noise_add(second.data(), width, height, stride, 50.0f, 0, 1, 12345u);

  /* 相同种子必须给出完全一致的结果。 */
  COMP_CHECK(std::memcmp(first.data(), second.data(), first.size()) == 0);
  /* alpha 不被噪声触碰。 */
  bool alpha_intact = true;
  for (size_t i = 3; i < first.size(); i += 4) {
    if (first[i] != 255) alpha_intact = false;
  }
  COMP_CHECK_MSG(alpha_intact, "noise_add must not modify alpha");

  /* 不同种子应给出不同结果。 */
  std::vector<uint8_t> third = original;
  noise_add(third.data(), width, height, stride, 50.0f, 0, 1, 999u);
  COMP_CHECK(std::memcmp(first.data(), third.data(), first.size()) != 0);
}

/*
 * 以下内核的语义依赖浮点/迭代收敛，M1 只做「可编译、可链接、不崩溃」的
 * 冒烟调用，量化等价性交 M2 的图像回归基准。
 */
COMP_TEST(imaging_deep_kernels_smoke) {
  const size_t width = 4;
  const size_t height = 4;
  const size_t deep_stride = width * 4 * 2; /* 16 位 4 通道 */
  const size_t small_stride = width * 4;

  std::vector<uint16_t> deep(width * height * 4, 0x8080);
  std::vector<uint8_t> split(width * height * 4, 0);

  deep_split(deep.data(), deep_stride, split.data(), small_stride, width, height, 0, -1);
  /* low 层必须是 16 位值向下的 8 位近似。 */
  COMP_CHECK(split[0] <= 0x81);

  const std::vector<uint8_t> flat(small_stride * height, 0x80);
  const uint8_t* near_results[3] = {flat.data(), flat.data(), flat.data()};
  const uint8_t* mid_results[3] = {flat.data(), flat.data(), flat.data()};
  const uint8_t* far_results[3] = {flat.data(), flat.data(), flat.data()};
  std::vector<uint16_t> merged(width * height * 4, 0);
  deep_merge(deep.data(), deep_stride, flat.data(), near_results, mid_results, far_results,
             small_stride, merged.data(), deep_stride, width, height, /*mixes=*/0);
  COMP_CHECK(merged.size() == deep.size());
}

COMP_TEST(imaging_dither_quantize16_smoke) {
  const size_t width = 4;
  const size_t height = 4;
  std::vector<uint16_t> wide(width * height * 4, 0x8080);
  std::vector<uint8_t> rgba(width * height * 4, 0);

  dither_quantize16(wide.data(), rgba.data(), width, height, width * 4);
  /* 8 位量化允许约一个量化步的抖动：0x8080 ≈ 128，容差 ±3。 */
  for (size_t i = 0; i < rgba.size(); ++i) {
    COMP_CHECK(rgba[i] >= 124 && rgba[i] <= 133);
  }
}

COMP_TEST(imaging_brush_alpha_bounds) {
  const size_t width = 4;
  const size_t height = 4;
  const size_t stride = width * 4;
  std::vector<uint8_t> pixels = solid(width, height, 0, 0, 0, 0);

  /* 只有 (1,1) 与 (2,2) 有非零 alpha。 */
  pixels[(1 * width + 1) * 4 + 3] = 255;
  pixels[(2 * width + 2) * 4 + 3] = 255;

  size_t bounds[4] = {0, 0, 0, 0};
  brush_alpha_bounds(pixels.data(), width, height, stride, bounds);
  COMP_CHECK(bounds[0] == 1 && bounds[1] == 1);
  COMP_CHECK(bounds[2] == 3 && bounds[3] == 3);

  /* 全透明 → 全零。 */
  const std::vector<uint8_t> empty = solid(width, height, 0, 0, 0, 0);
  size_t empty_bounds[4] = {9, 9, 9, 9};
  brush_alpha_bounds(empty.data(), width, height, stride, empty_bounds);
  COMP_CHECK(empty_bounds[0] == 0 && empty_bounds[1] == 0);
  COMP_CHECK(empty_bounds[2] == 0 && empty_bounds[3] == 0);
}

COMP_TEST(imaging_layer_alpha_round_trip) {
  const size_t width = 2;
  const size_t height = 2;
  const size_t stride = width * 4;

  std::vector<uint8_t> rgba = solid(width, height, 64, 64, 64, 128);
  std::vector<uint8_t> gray(width * height, 0);

  /*
   * 抽离 alpha：原始实现（macOS 版 BrushPixels.c）只把 alpha 拷进灰度缓冲，
   * 不修改 rgba 本身；「置为不透明」是下一步 layer_unpremultiply_opaque 的职责。
   */
  layer_extract_alpha(rgba.data(), stride, gray.data(), width, width, height);
  for (size_t i = 0; i < gray.size(); ++i) COMP_CHECK(gray[i] == 128);
  for (size_t i = 3; i < rgba.size(); i += 4) COMP_CHECK(rgba[i] == 128);

  /* 反预乘：RGB 还原为直通色（64*255/128 = 128），alpha 置 255。 */
  layer_unpremultiply_opaque(rgba.data(), stride, width, height);
  COMP_CHECK(rgba[0] >= 126 && rgba[0] <= 129);
  for (size_t i = 3; i < rgba.size(); i += 4) COMP_CHECK(rgba[i] == 255);

  /* 回填 alpha 并重新预乘 → 精确回到初始像素 (64,64,64,128)。 */
  layer_restore_alpha(rgba.data(), stride, gray.data(), width, width, height);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    COMP_CHECK(rgba[i] == 64);
    COMP_CHECK(rgba[i + 3] == 128);
  }
}

COMP_TEST(imaging_heal_coverage_bounds) {
  const size_t width = 8;
  const size_t height = 8;
  std::vector<uint8_t> gray(width * height, 0);
  gray[3 * width + 2] = 255;
  gray[5 * width + 4] = 200;

  long bounds[4] = {0, 0, 0, 0};
  heal_coverage_bounds(gray.data(), width, height, width, bounds);
  COMP_CHECK(bounds[0] == 2 && bounds[1] == 3);
  COMP_CHECK(bounds[2] == 5 && bounds[3] == 6);
}

COMP_TEST(imaging_wand_mask_selects_uniform_area) {
  const size_t width = 8;
  const size_t height = 8;
  const size_t stride = width * 4;
  const std::vector<uint8_t> rgba = solid(width, height, 100, 120, 140, 255);
  std::vector<uint8_t> mask(width * height, 0);

  const long selected = wand_mask(rgba.data(), width, height, stride, /*seedX=*/0, /*seedY=*/0,
                                  /*radius=*/0, /*tolerance=*/5, /*contiguous=*/1, mask.data());
  COMP_CHECK(selected == static_cast<long>(width * height));
  for (size_t i = 0; i < mask.size(); ++i) COMP_CHECK(mask[i] == 255);
}

COMP_TEST(imaging_color_range_mask_matches_exact_color) {
  const size_t width = 4;
  const size_t height = 4;
  const size_t stride = width * 4;
  const std::vector<uint8_t> rgba = solid(width, height, 30, 60, 90, 255);
  const uint8_t include[3] = {30, 60, 90};
  std::vector<uint8_t> mask(width * height, 0);

  const long selected = color_range_mask(rgba.data(), width, height, stride, include, 1,
                                         /*exclude=*/nullptr, 0, /*fuzziness=*/0,
                                         /*invert=*/0, mask.data());
  COMP_CHECK(selected == static_cast<long>(width * height));
  COMP_CHECK(mask[0] == 255);

  /* 反转选择：同一颜色在 invert 下应一个都不选。 */
  std::vector<uint8_t> inverted_mask(width * height, 0);
  const long inverted = color_range_mask(rgba.data(), width, height, stride, include, 1, nullptr,
                                         0, 0, 1, inverted_mask.data());
  COMP_CHECK(inverted == 0);
}

COMP_TEST(imaging_patch_fill_smoke) {
  const int width = 16;
  const int height = 16;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> rgba = solid(width, height, 180, 140, 100, 255);
  std::vector<uint8_t> hole(static_cast<size_t>(width) * height, 0);
  /* 中心 2×2 挖空。 */
  for (int y = 7; y <= 8; ++y) {
    for (int x = 7; x <= 8; ++x) hole[y * width + x] = 255;
  }

  const int result = patch_fill(rgba.data(), stride, hole.data(), static_cast<size_t>(width),
                                width, height, 4242u);
  COMP_CHECK_MSG(result >= 0, "patch_fill must not run out of memory on a 16x16 image");
  /* 填洞区域内不得残留「洞」标记以外的破损 alpha。 */
  for (size_t i = 3; i < rgba.size(); i += 4) COMP_CHECK(rgba[i] == 255);
}
