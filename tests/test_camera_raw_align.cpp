// AI生成
/*
 * test_camera_raw_align.cpp — 上游 1.5.3 Align 批：AdjustPixels 四个函数的护栏。
 *
 * 背景（Pi 审计 AUDIT9 + hermes 完成度对标）：
 *   上游 v1.5.2/v1.5.3 在 Camera Raw 上持续加码，新增/修改了 4 个函数：
 *     - camera_raw_lookup_is_valid  —— Metal lookup 的受支持表域闸（grid==33、全部有限且 0..1）
 *     - camera_raw_repair_lookup    —— GPU 舍入边界像素回落双精度 composed_lookup
 *     - box_mean_plane              —— 边缘收缩窗口的盒式均值（不是 box_blur_plane，两者窗口不同）
 *     - adjust_camera_raw_shadows   —— 按画面分区自适应的 Shadows（log2 亮度域 + 引导滤波）
 *   ng 侧此前一个都没有，README §6.4「另 4 组差异仅注释」对 AdjustPixels 已不成立。
 *
 * 口径：移植后 AdjustPixels.c 与上游 origin/main 365b574 的 diff 只剩
 *   「去 Apple GCD」三处（dispatch_apply → 顺序 for）与 include 差异。
 *   本文件钉住这四个函数的行为边界，不复制上游的数值实现细节断言。
 */
#include <cmath>
#include <cstring>
#include <vector>

#include "imaging/comp_imaging.h"
#include "test_framework.h"

namespace {

/* 33³ × 3 float 的合法 Metal lookup 表（全部 0.5，恒等到 0.5 灰）。 */
std::vector<float> uniform_table(int grid, float value) {
  return std::vector<float>(static_cast<size_t>(grid) * grid * grid * 3, value);
}

/* 预乘 RGBA 缓冲：全不透明实心。 */
std::vector<uint8_t> solid(size_t width, size_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
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

/* ── 1. camera_raw_lookup_is_valid：表域闸 ──────────────────────────────
 * 上游语义：只支持 grid==33（33³×3 个 float）的表，且每个分量必须有限且落在 0..1。
 * 域外（其它 grid、NaN/Inf、越界）一律返回 0，调用方退回原 CPU 路径。
 */
COMP_TEST(camera_raw_lookup_is_valid_accepts_only_the_metal_table_domain) {
  std::vector<float> ok = uniform_table(33, 0.5f);
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(ok.data(), 33) == 1, "33\u00b3 \u0c3e\u5168\u6709\u9650\u4e14\u5728 0..1 \u5e94\u8ba4\u53ef");

  /* 两端取值仍合法（闸是闭区间，不是「必须严格内缩」）。 */
  std::vector<float> edges = uniform_table(33, 0.0f);
  COMP_CHECK(camera_raw_lookup_is_valid(edges.data(), 33) == 1);
  for (auto& v : edges) v = 1.0f;
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(edges.data(), 33) == 1, "1.0 \u662f\u4e0a\u6e38\u8ba4\u53ef\u7684\u4e0a\u754c");

  /* 越界：-0.001 与 1.001 都要被拒。 */
  std::vector<float> low = uniform_table(33, -0.001f);
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(low.data(), 33) == 0, "\u8d8a\u4e0b\u754c\u5e94\u62d2\u7edd");
  std::vector<float> high = uniform_table(33, 1.001f);
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(high.data(), 33) == 0, "\u8d8a\u4e0a\u754c\u5e94\u62d2\u7edd");

  /* 非有限：NaN / +Inf / -Inf。 */
  std::vector<float> nan_table = uniform_table(33, 0.5f);
  nan_table[0] = std::nanf("");
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(nan_table.data(), 33) == 0, "NaN \u5e94\u62d2\u7edd");
  std::vector<float> inf_table = uniform_table(33, 0.5f);
  inf_table[7] = HUGE_VALF;
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(inf_table.data(), 33) == 0, "+Inf \u5e94\u62d2\u7edd");
  std::vector<float> ninf_table = uniform_table(33, 0.5f);
  ninf_table[9] = -HUGE_VALF;
  COMP_CHECK(camera_raw_lookup_is_valid(ninf_table.data(), 33) == 0);

  /* grid \u2260 33 \u4e00\u5f8b\u62d2\u7edd\uff08\u5305\u62ec\u66f4\u5c0f\u7684\u5408\u6cd5\u8868\uff09\u3002 */
  std::vector<float> grid17 = uniform_table(17, 0.5f);
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(grid17.data(), 17) == 0, "grid=17 \u5e94\u62d2\u7edd");
  std::vector<float> grid65 = uniform_table(65, 0.5f);
  COMP_CHECK(camera_raw_lookup_is_valid(grid65.data(), 65) == 0);

  /* \u7a7a\u8868\u4e0d\u80fd\u5f53\u4f5c\u5408\u6cd5\u3002 */
  COMP_CHECK_MSG(camera_raw_lookup_is_valid(nullptr, 33) == 0, "\u7a7a\u6307\u9488\u5e94\u62d2\u7edd");
  COMP_CHECK(camera_raw_lookup_is_valid(ok.data(), 0) == 0);
}

/* \u2500\u2500 2. camera_raw_repair_lookup\uff1a\u53ea\u91cd\u7b97 uncertain \u6807\u8bb0\u7684\u50cf\u7d20 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
 * \u4e0a\u6e38\u8bed\u4e49\uff1aresult \u63a5\u6536\u53ea\u91cd\u5199 uncertain[i]!=0 \u7684\u50cf\u7d20\uff08\u672a\u6807\u8bb0\u7684\u4fdd\u6301\u539f\u503c\uff0c\u56e0\u4e3a result \u53ef\u80fd\u5df2\u88ab GPU \u5199\u8fc7\uff09\uff0c
 * \u91cd\u5199\u7528\u539f\u59cb\u53cc\u7cbe\u5ea6 composed_lookup\u3002uncertain \u4e0d\u5141\u8bb8\u8d8a\u754c\uff08\u6309 width \u7d27\u51d1\u7d22\u5f15\uff09\u3002
 */
COMP_TEST(camera_raw_repair_lookup_touches_only_the_marked_pixels) {
  const size_t width = 4, height = 3, stride = width * 4;
  std::vector<float> table = uniform_table(33, 1.0f);  /* \u5168 1 \u2192 \u8f93\u51fa\u767d */
  std::vector<uint8_t> source = solid(width, height, 40, 80, 120, 255);
  std::vector<uint8_t> result = solid(width, height, 7, 9, 11, 255);
  std::vector<uint8_t> uncertain(width * height, 0);

  /* \u53ea\u6807\u8bb0\u4e2d\u95f4\u90a3\u4e00\u50cf\u7d20\u3002 */
  uncertain[width * 1 + 1] = 1;
  camera_raw_repair_lookup(result.data(), source.data(), uncertain.data(), width, height,
                           stride, table.data(), 33);

  /* \u88ab\u6807\u8bb0\u7684\uff1a\u5168 1 \u7684\u8868\u628a\u989c\u8272\u63a8\u5230\u767d\u3002 */
  const size_t marked = width * 1 + 1;
  COMP_CHECK_MSG(result[marked * 4] == 255 && result[marked * 4 + 1] == 255 &&
                     result[marked * 4 + 2] == 255,
                 "\u88ab\u6807\u8bb0\u50cf\u7d20\u5e94\u8d70 composed_lookup");
  COMP_CHECK_MSG(result[marked * 4 + 3] == 255, "alpha \u4fdd\u6301");
  /* \u672a\u88ab\u6807\u8bb0\u7684\uff1aresult \u539f\u503c\u4e00\u4e2a\u5b57\u8282\u4e0d\u52a8\u3002 */
  bool untouched = true;
  for (size_t i = 0; i < result.size(); i += 4) {
    if (i == marked * 4) continue;
    if (result[i] != 7 || result[i + 1] != 9 || result[i + 2] != 11 || result[i + 3] != 255) {
      untouched = false;
    }
  }
  COMP_CHECK_MSG(untouched, "\u672a\u6807\u8bb0\u50cf\u7d20\u5e94\u539f\u6837\u4fdd\u7559");
}

COMP_TEST(camera_raw_repair_lookup_skips_fully_transparent_pixels) {
  const size_t width = 2, height = 2, stride = width * 4;
  std::vector<float> table = uniform_table(33, 1.0f);
  std::vector<uint8_t> source = solid(width, height, 40, 80, 120, 255);
  source[3] = 0;  /* \u7b2c 0 \u50cf\u7d20\u900f\u660e */
  std::vector<uint8_t> result = solid(width, height, 200, 210, 220, 255);
  std::vector<uint8_t> uncertain(width * height, 1);  /* \u5168\u90e8\u6807\u8bb0 */

  camera_raw_repair_lookup(result.data(), source.data(), uncertain.data(), width, height,
                           stride, table.data(), 33);

  COMP_CHECK_MSG(result[3] == 255 && result[0] == 200 && result[1] == 210 && result[2] == 220,
                 "\u900f\u660e\u50cf\u7d20\u4e0d\u5e94\u88ab\u4fee\u590d\uff08alpha=0 \u65e0\u989c\u8272\u53ef\u53d6\uff0c\u4e0a\u6e38\u76f4\u63a5 continue\uff0c\u7559\u4e0b GPU \u5df2\u5199\u7684\u503c\uff09");
  /* \u4e0d\u900f\u660e\u7684\u88ab\u91cd\u5199\u5230\u767d\u3002 */
  COMP_CHECK(result[4] == 255);
}

/* \u2500\u2500 3. box_mean_plane \u4e0e box_blur_plane \u7684\u7a97\u53e3\u5dee\u5f02 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
 * \u4e24\u8005\u90fd\u662f\u7f3a\u5c11\u91cd\u590d\u8fb9\u7f18\u7684\u76d2\u5f0f\u5e73\u6ed1\uff0c\u4f46\u8ba1\u7b97\u5e73\u5747\u65f6\u5206\u6bcd\u4e0d\u540c\uff1a
 *   - box_blur_plane \u2014\u2014 \u8fb9\u7f18\u91cd\u590d\u53d6\u8fb9\u7f18\u503c\uff0c\u53ca\u7a97\u957f\u6052\u5b9a 2r+1\uff08\u6240\u4ee5\u8fb9\u7f18\u5904\u4f1a\u53d8\u6697\uff09\uff1b
 *   - box_mean_plane   \u2014\u2014 \u7a97\u5728\u8fb9\u7f18\u6536\u7f29\uff08\u5c11\u4e00\u4fa7\u5c31\u53ea\u7d2f\u52a0\u90a3\u4e00\u4fa7\u5b58\u5728\u7684\u50cf\u7d20\uff09\uff0c\u53ca\u771f\u6b63\u7684\u53ef\u7528\u8ba1\u6570\u3002
 * Shadows \u7528\u540e\u8005\uff08\u5bfc\u81f4\u5f71\u50cf\u8fb9\u7f18\u4e0e\u5185\u90e8\u4e00\u81f4\u5730\u91c7\u6837\u8ba1\u7b97\uff09\u3002
 * \u672c\u4f53\u662f file-static\uff0c\u901a\u8fc7 adjust_camera_raw_shadows \u7684\u8fb9\u7f18\u884c\u4e3a\u95f4\u63a5\u53cd\u63a8\u3002
 */
COMP_TEST(camera_raw_shadows_reaches_the_edges_of_a_small_image) {
  /* 5\u00d73 \u5f88\u5c0f\u7684\u753b\u9762\uff1alw/lh \u4e3a\u539f\u5c3a\u5bf8\uff08scale=1\uff09\uff0cradius=1\u3002 */
  const size_t width = 5, height = 3, stride = width * 4;
  std::vector<uint8_t> rgba = solid(width, height, 30, 30, 30, 255);

  /* \u4e2d\u95f4\u884c\u4eae\u4e00\u683c\uff0c\u8ba9\u533a\u57df\u5e73\u6ed1\u6709\u53ef\u6d4b\u7684\u63d0\u5347\u3002 */
  for (size_t x = 2; x < 3; ++x) rgba[(1 * width + x) * 4] = 150;

  /* \u6807\u5e8f\u5316\u7684\u7cfb\u6570\uff1a\u4e09\u4e2a\u5e73\u9762\u3001\u884c\u5217 xCount\u3001\u6b65\u957f\u3002 */
  const int xCount = 8, kCount = 4;
  const float xLow = -6.0f, xStep = 1.0f, kLow = -6.0f, kStep = 1.0f;
  const size_t plane = static_cast<size_t>(xCount) * kCount;
  /* \u6052\u5b9a\u63d0\u5347 +0.25 log2 \u2014\u2014\u4e00\u4e2a\u53ef\u9884\u671f\u7684\u5b9e\u8d28\u9879\uff0c\u8bed\u4e49\u6e05\u6670\u3002 */
  std::vector<float> coefficients(3 * plane, 0.0f);
  for (size_t i = 0; i < plane; ++i) coefficients[i] = 0.25f;

  adjust_camera_raw_shadows(rgba.data(), width, height, stride, coefficients.data(), xCount,
                            xLow, xStep, kCount, kLow, kStep);

  /* \u975e\u900f\u660e\u3001\u975e\u96f6\u7684\u50cf\u7d20\u5e94\u88ab\u63d0\u5347\uff08alpha \u4fdd\u6301\uff09\u3002 */
  for (size_t i = 0; i < width * height; ++i) {
    COMP_CHECK_MSG(rgba[i * 4 + 3] == 255, "Shadows \u4fdd\u7559 alpha");
    COMP_CHECK_MSG(rgba[i * 4] >= 30, "\u6697\u533a\u5e94\u88ab\u63d0\u4eae\uff08\u63d0\u5347\u9879\u4e3a\u6b63\uff09");
  }
  /* \u63d0\u5347\u4e0d\u5e94\u8d85\u8fc7\u53ef\u80fd\u7684\u4e0a\u9650\uff1a\u989c\u8272\u4ecd\u5728 0..255\u3001\u4e14\u4e0d\u5f97\u56de\u7ed5\u5230\u9ed1\u3002 */
  bool any_changed = false;
  for (size_t i = 0; i < width * height; ++i) {
    COMP_CHECK(rgba[i * 4 + 1] <= 255);
    if (rgba[i * 4] > 30) any_changed = true;
  }
  COMP_CHECK_MSG(any_changed, "\u63d0\u5347\u9879\u5fc5\u987b\u771f\u7684\u6539\u52a8\u753b\u9762");
}

/* Shadows \u4e0d\u80fd\u52a8 alpha==0 \u7684\u50cf\u7d20\u3001\u4e0d\u80fd\u6539\u9884\u7b97 alpha\u3002 */
COMP_TEST(camera_raw_shadows_leaves_transparent_pixels_alone) {
  const size_t width = 8, height = 8, stride = width * 4;
  std::vector<uint8_t> rgba = solid(width, height, 20, 20, 20, 255);
  for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 0;  /* \u5168\u900f\u660e */
  std::vector<uint8_t> before = rgba;

  const int xCount = 8, kCount = 4;
  const size_t plane = static_cast<size_t>(xCount) * kCount;
  std::vector<float> coefficients(3 * plane, 1.0f);  /* \u6781\u7aef\u63d0\u5347\uff0c\u82e5\u6f0f\u5199\u5c31\u4f1a\u9732\u51fa */
  adjust_camera_raw_shadows(rgba.data(), width, height, stride, coefficients.data(), xCount,
                            -6.0f, 1.0f, kCount, -6.0f, 1.0f);

  COMP_CHECK_MSG(rgba == before, "\u5168\u900f\u660e\u753b\u9762\u5e94\u539f\u6837\u4e0d\u53d8");
}

/* \u53c2\u6570\u5b88\u536b\uff1a\u8f93\u5165\u4e0d\u5408\u6cd5\u65f6\u76f4\u63a5\u8fd4\u56de\uff0c\u4e00\u5b57\u4e0d\u6539\u3002 */
COMP_TEST(camera_raw_shadows_guards_its_parameters) {
  const size_t width = 4, height = 4, stride = width * 4;
  std::vector<uint8_t> rgba = solid(width, height, 40, 40, 40, 255);
  std::vector<uint8_t> before = rgba;
  const int xCount = 8, kCount = 4;
  const size_t plane = static_cast<size_t>(xCount) * kCount;
  std::vector<float> coefficients(3 * plane, 0.5f);

  /* xCount < 2 \u65e0\u6cd5\u5728 x \u4e0a\u63d2\u503c \u2192 \u8fd4\u56de\u3002 */
  adjust_camera_raw_shadows(rgba.data(), width, height, stride, coefficients.data(), 1,
                            -6.0f, 1.0f, kCount, -6.0f, 1.0f);
  /* kCount < 1 \u65e0 K \u884c\u53ef\u53d6 \u2192 \u8fd4\u56de\u3002 */
  adjust_camera_raw_shadows(rgba.data(), width, height, stride, coefficients.data(), xCount,
                            -6.0f, 1.0f, 0, -6.0f, 1.0f);
  /* \u96f6\u5c3a\u5bf8\u3002 */
  adjust_camera_raw_shadows(rgba.data(), 0, 0, 0, coefficients.data(), xCount, -6.0f, 1.0f,
                            kCount, -6.0f, 1.0f);

  COMP_CHECK_MSG(rgba == before, "\u975e\u6cd5\u53c2\u6570\u5e94\u539f\u6837\u8fd4\u56de");
}

/* \u2500\u2500 4. x \u57df\u7684\u53f3\u7aef\u5939\u53d6\u5fc5\u987b\u843d\u5728\u884c\u5185 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
 * \u4e0a\u6e38\u5199\u6cd5\uff1a\u4e09\u4e2a\u5e73\u9762\u5728 x \u4e0a\u53cc\u7ebf\u6027\u63d2\u503c\uff0c\u6240\u4ee5\u5217\u53f7\u5fc5\u987b\u6321\u5230 xCount-2\uff08\u624d\u80fd\u8bfb\u5230\u5b83\u53f3\u8fb9
 * \u90a3\u4e2a\u4e0d\u8d8a\u754c\u7684\u5217\uff09\u3002\u82e5\u653e\u677e\u6210 int(xp) \u672a\u5939\uff0c\u5f53 xp \u6070\u597d\u843d\u5728\u57df\u7684\u6700\u53f3\u5217\u65f6\uff0c
 * \u8bfb\u53d6\u4f1a\u8de8\u5230\u4e0b\u4e00\u4e2a K \u884c\u7684\u7b2c 0 \u5217\uff08\u672b\u884c\u65f6\u5219\u76f4\u63a5\u8d8a\u51fa\u6574\u4e2a coefficients \u7f13\u51b2\uff09\u3002
 *
 * \u6784\u9020\u53ef\u8fbe\u6761\u4ef6\uff1a\u5b8c\u5168\u5e73\u7684\u56fe\u7247\u2192 \u533a\u57df\u5c42\u7ea7\u6070\u597d\u7b49\u4e8e\u53c2\u8003\u4eae\u5ea6 key
 *   \u2192 xp = clamp((region - key - xLow) / xStep) = clamp(-xLow/xStep)\u3002\u53d6 xLow = -(xCount-1) - 0.5 \u5c31\u5f3a\u5236\u843d\u5230\u6700\u53f3\u5217\uff08
 *   \u7559 0.5 \u7684\u88d5\u5ea6\uff0c\u5bf9 float \u566a\u58f0\u5145\u5206\u5ffd\u7565\uff09\u3002
 * \u5e73\u56fe\u53c8\u8ba9 detail = level - region = 0\uff0c\u4e8c\u3001\u4e09\u4e24\u4e2a\u9879\u7684\u7cfb\u6570\u6570\u5b66\u4e0a\u4e3a 0\uff0c\u53ea\u7559 lift\uff08t=0\uff09\u2014\u2014
 *   \u6b63\u786e\u8def\u5f84\u53d6\u5230\u7684\u662f\u672c\u884c\u6700\u540e\u4e00\u5217\uff08\u8bbe 0 \u2192 \u753b\u9762\u4e0d\u53d8\uff09\uff0c\u8d8a\u754c\u8def\u5f84\u5219\u8bfb\u5230\u4e0b\u4e00 K \u884c\u7684
 *   \u7b2c 0 \u5217\uff08\u8bbe 9 \u2192 gain = 2\u2079 = 512 \u2192 \u767d\u573a\uff09\u3002\u4e24\u8005\u4e0d\u53ef\u80fd\u540c\u65f6\u6210\u7acb\u3002
 */
COMP_TEST(camera_raw_shadows_clamps_the_x_domain_inside_its_row) {
  const size_t width = 5, height = 3, stride = width * 4;
  std::vector<uint8_t> rgba = solid(width, height, 30, 30, 30, 255);

  const int xCount = 8, kCount = 4;
  const float xLow = -7.5f, xStep = 1.0f, kLow = 0.0f, kStep = 1.0f;
  const size_t plane = static_cast<size_t>(xCount) * kCount;
  /* \u6b63\u786e\u8def\u5f84\u53ea\u770b coefficients[0*8+6] \u4e0e [0*8+7]\uff08xt=1 \u2192 \u53d6\u6700\u540e\u4e00\u5217 = 0\uff09\uff1b
   * \u8d8a\u754c\u8def\u5f84\u53d6 coefficients[0*8+7] \u4e0e [0*8+8]\uff08xt=0 \u2192 \u53d6\u4e0b\u4e00 K \u884c\u7684\u7b2c 0 \u5217\uff09\u3002 */
  std::vector<float> coefficients(3 * plane, 0.0f);
  coefficients[8] = 9.0f;  /* = K \u884c 1 \u7684\u7b2c 0 \u5217\uff1a\u8d8a\u754c\u624d\u4f1a\u8bfb\u5230\u5b83 */

  adjust_camera_raw_shadows(rgba.data(), width, height, stride, coefficients.data(), xCount,
                            xLow, xStep, kCount, kLow, kStep);

  for (size_t i = 0; i < width * height; ++i) {
    COMP_CHECK_MSG(rgba[i * 4] == 30 && rgba[i * 4 + 1] == 30 && rgba[i * 4 + 2] == 30,
                  "\u672c\u884c\u6700\u540e\u4e00\u5217\u7684 lift \u4e3a 0 \u2192 \u5e73\u56fe\u5e94\u539f\u6837\u4e0d\u53d8");
    COMP_CHECK_MSG(rgba[i * 4] != 255, "\u82e5\u8bfb\u8fc7\u884c\u5c3e\u5230\u4e0b\u4e00 K \u884c\uff0c\u753b\u9762\u4f1a\u88ab\u63a8\u6210\u767d\u573a");
  }
}

/* \u2500\u2500 5. box_mean_plane \u7684\u7a97\u5728\u8fb9\u7f18\u6536\u7f29\uff0c\u800c\u975e\u56fa\u5b9a 2r+1 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
 * box_blur_plane \u628a\u8fb9\u7f18\u503c\u91cd\u590d\u53d6\u5230\u7a97\u5185\u5e76\u4ee5\u56fa\u5b9a\u7a97\u957f 2r+1 \u9664\uff08\u8fb9\u7f18\u53d8\u6697\uff09\uff1b
 * box_mean_plane \u5219\u662f\u7a97\u771f\u6b63\u7f29\u5c0f\u3001\u53ea\u53d6\u5e73\u9762\u5185\u90e8\u90a3\u4e00\u6bb5\u3002\u4e24\u8005\u5728\u5e73\u53f0/\u5b9e\u6570\u8f93\u5165\u4e0b\u65e0\u6cd5\u533a\u5206\uff0c
 * \u53ea\u6709\u8fc7\u8fb9\u540e\u7684\u9636\u68af\u8fdb\u5165\u5e73\u6ed1\u533a\u65f6\u624d\u6709\u53ef\u6d4b\u5dee\u5f02\u3002
 *
 * \u53c2\u6570\u9009\u62e9\uff0833\u00d717\uff0cradius=4\uff0clw=33 \u76f4\u63a5\u7528\u539f\u5c3a\u5bf8\u5b9e\u73b0\uff0c\u65e0 scale \u62bd\u6837\u566a\u58f0\uff09\uff1a
 *   \u53ea\u7ed9 gain-above \u5e73\u9762\u975e\u96f6\uff08lift = gain \u4e0b \u4e24\u9879\u7ed9 0\uff09\u3002\u8fd9\u6837\u8f93\u51fa\u4f9d\u8d56 detail = level - region\uff0c
 *   \u800c region \u6b63\u662f\u88ab\u6ce8\u5165\u7684\u90a3\u4e2a\u5e73\u6ed1\u7ed3\u679c\u2014\u2014\u76f4\u63a5\u8bfb\u51fa\u5dee\u5f02\u3002
 *   \u82e5\u53ea\u7ed9 lift \u975e\u96f6\uff0c\u5219 detail \u4e58\u4e0a\u53bb\u4e3a 0\uff0c\u6d4b\u51fa\u7684\u53ea\u662f\u56fa\u5b9a\u7684 lift\uff08\u6ce8\u5165\u4e0d\u53ef\u89c1\uff09\u2014\u2014
 *   \u672c\u8f6e\u7b2c\u4e09\u6b21\u201c\u62a4\u680f\u521d\u62a5\u4e0d\u7ea2\u201d\u5c31\u662f\u8fd9\u4e2a\u5751\uff08\u524d\u4e24\u6b21\u5728 C0 \u4e0e C1\uff09\u3002
 */
COMP_TEST(camera_raw_shadows_detail_gain_exposes_the_smoothing_window) {
  const size_t width = 33, height = 17, stride = width * 4;
  /* \u9636\u68af\u8fb9\u7f18\uff1a\u5de6\u534a\u6697 12\uff08\u7a84\uff09\u3001\u53f3\u534a\u4eae 200\uff08\u5bbd\uff09\u3002\u5206\u9694\u5904\u5c31\u662f\u88ab\u5e73\u6ed1\u7684\u533a\u57df\u3002 */
  std::vector<uint8_t> rgba(width * height * 4);
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      uint8_t* p = &rgba[(y * width + x) * 4];
      p[0] = p[1] = p[2] = x < width / 2 ? 12 : 200;
      p[3] = 255;
    }
  }

  const int xCount = 8, kCount = 4;
  const size_t plane = static_cast<size_t>(xCount) * kCount;
  std::vector<float> coefficients(3 * plane, 0.0f);
  for (size_t i = 0; i < plane; ++i) coefficients[plane + i] = 0.4f;  /* gain-above \u552f\u4e00\u975e\u96f6 */

  adjust_camera_raw_shadows(rgba.data(), width, height, stride, coefficients.data(), xCount,
                            -6.0f, 1.0f, kCount, -6.0f, 1.0f);

  /* \u5206\u9694\u4e4b\u540e\u7684\u8fc7\u6e21\u5e26\uff1a\u8fb9\u7f18\u4e00\u5b9a\u6bd4\u786c\u8fb9\u754c\u66f4\u5e73\u6ed1\u5730\u5411\u4eae\u4fa7\u6ea2\u51fa\u3002
   * \u6b63\u786e\u5b9e\u73b0\u5728\u672c\u8f93\u5165\u4e0b\u7ed9\u51fa 212,208,205,204,203,202,201,200\uff08\u9010\u70b9\u8870\u51cf\u540e\u5e73\u89c4\u5730\u5230 200\uff09\uff0c
   * \u800c\u56fa\u5b9a\u7a97\u957f\u7248\u4f1a\u5728\u8fb9\u7f18\u91cd\u590d\u53d6\u503c\u3001\u5e73\u5747\u88ab\u62c9\u56de\u5e73\u5747\u503c\u2192 \u5206\u9694\u5904\u76f4\u63a5\u8df3\u5230 200\u3002 */
  const size_t seam = width / 2;
  COMP_CHECK_MSG(rgba[seam * 4] > 200, "\u8fc7\u6e21\u5e26\u7b2c\u4e00\u683c\u5e94\u88ab\u63d0\u4eae\u5230\u8fb9\u7f18\u4e4b\u4e0a");
  COMP_CHECK_MSG(rgba[seam * 4] < 255, "\u8fc7\u6e21\u5e26\u4e0d\u5e94\u88ab\u63a8\u5230\u767d\u573a");
  /* \u9012\u51cf\uff1a\u8d8a\u9760\u8fd1\u4eae\u4fa7\u8fb9\u5dee\u5f83\u8d8a\u5c0f\u3002 */
  bool strictly_decreasing = true;
  for (size_t x = seam; x + 1 < width; ++x) {
    if (rgba[(x + 1) * 4] > rgba[x * 4]) strictly_decreasing = false;
  }
  COMP_CHECK_MSG(strictly_decreasing, "\u8fc7\u6e21\u5e26\u5e94\u9012\u51cf\uff08\u800c\u975e\u4e00\u6b65\u8df3\u5230\u5e73\u53f0\u503c\uff09");
  /* \u666e\u901a\u5185\u90e8\u533a\u4e0d\u53d7\u5f71\u54cd\u3002 */
  COMP_CHECK_MSG(rgba[0] == 12, "\u8fdc\u79bb\u8fb9\u7f18\u7684\u5e73\u53f0\u533a\u4e0d\u5e94\u88ab\u63d0\u5347");
}

