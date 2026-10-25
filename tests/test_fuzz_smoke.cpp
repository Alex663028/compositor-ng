// AI生成
/*
 * test_fuzz_smoke.cpp — OPT-14：编解码器坏输入的确定性变异冒烟。
 *
 * 背景：JPEG/HDR 长期外包 stb_image 而仓内零坏输入用例（PSD/EXR 有定向拒绝
 * 用例，但都从自建合法基座出发，无随机损坏形态）。本文件补最小闭环：
 *   1. 用引擎自己的编码器生成合法 JPEG / PNG（内存缓冲，无样本文件依赖）；
 *   2. 截断变异：每源 8 个截断点，解码必须不崩；断言至少一次被拒绝
 *      （合法编码被截断大概率损坏；不断言全部——编码器容错口径留给实现）；
 *   3. 字节翻转：固定 LCG（种子 20261025）做 240 次单字节翻转，解码不得崩、
 *      不得越界写（ASan/日常双重意义）；接受/拒绝只计数不断言。
 * 全部确定性（同种子同序列），回归可复现。到测试跑完没崩即为通过——这正是
 * 对「最经典攻击面」的最小钉子；更全面的样本集见任务卡 OPT-14 后续。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_io.h"
#include "format/codec_lib.h"
#include "test_framework.h"

namespace {

std::vector<uint8_t> solid_premultiplied(int32_t w, int32_t h) {
  /* 预乘口径的半透明青紫棋盘：不透明像素 + 半透明像素都有，给编码器真实负载。 */
  std::vector<uint8_t> p(static_cast<size_t>(w) * h * 4);
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      const bool dark = ((x / 8) + (y / 8)) % 2 == 0;
      const uint8_t a = dark ? 255u : 160u;
      const uint8_t r = static_cast<uint8_t>((dark ? 40u : 200u) * a / 255u);
      const uint8_t g = static_cast<uint8_t>((dark ? 120u : 60u) * a / 255u);
      const uint8_t b = static_cast<uint8_t>((dark ? 180u : 200u) * a / 255u);
      p[i + 0] = r;
      p[i + 1] = g;
      p[i + 2] = b;
      p[i + 3] = a;
    }
  }
  return p;
}

uint32_t lcg(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

}  // namespace

COMP_TEST(fuzz_smoke_truncated_and_flipped_inputs_never_crash) {
  std::string error;
  std::vector<uint8_t> pixels = solid_premultiplied(96, 64);
  std::vector<uint8_t> jpeg;
  std::vector<uint8_t> png;
  COMP_CHECK_MSG(compositor::codec_lib_encode(pixels.data(), 96, 64,
                                              static_cast<uint32_t>(COMP_FORMAT_JPEG), 90,
                                              &jpeg, &error),
                 "engine encodes a valid JPEG baseline");
  COMP_CHECK_MSG(compositor::codec_lib_encode(pixels.data(), 96, 64,
                                              static_cast<uint32_t>(COMP_FORMAT_PNG), 6, &png,
                                              &error),
                 "engine encodes a valid PNG baseline");
  COMP_CHECK(jpeg.size() > 200);
  COMP_CHECK(png.size() > 200);

  const uint32_t kFormatJpeg = static_cast<uint32_t>(COMP_FORMAT_JPEG);
  const uint32_t kFormatPng = static_cast<uint32_t>(COMP_FORMAT_PNG);

  /* --- 截断：8 个截断点 × 两格式，解码只许「成功或拒绝」，不许崩。 --- */
  int rejected = 0;
  int tried = 0;
  for (const std::vector<uint8_t>* source : {&jpeg, &png}) {
    const uint32_t format = source == &jpeg ? kFormatJpeg : kFormatPng;
    for (size_t cut = 1; cut < source->size(); cut += source->size() / 8) {
      std::vector<uint8_t> truncated(source->begin(), source->begin() + static_cast<long>(cut));
      compositor::CodecImage image;
      std::string decode_error;
      ++tried;
      if (!compositor::codec_lib_decode(truncated.data(), truncated.size(), format, &image,
                                        &decode_error)) {
        ++rejected;
      }
    }
  }
  COMP_CHECK(tried >= 12);
  COMP_CHECK_MSG(rejected > 0, "at least one truncation is rejected (mostly-damaged files)");

  /* --- 单字节翻转：固定 LCG 240 次（JPEG/PNG 交替），跑完没崩即通过。 --- */
  uint32_t state = 20261025u;  /* 固定种子：同序列可复现 */
  int flips = 0;
  for (int i = 0; i < 240; ++i) {
    std::vector<uint8_t> mutated = (i % 2 == 0) ? jpeg : png;
    const uint32_t format = (i % 2 == 0) ? kFormatJpeg : kFormatPng;
    mutated[lcg(state) % mutated.size()] = static_cast<uint8_t>(lcg(state) & 0xFFu);
    compositor::CodecImage image;
    std::string decode_error;
    compositor::codec_lib_decode(mutated.data(), mutated.size(), format, &image, &decode_error);
    ++flips;
  }
  COMP_CHECK(flips == 240);

  /* 基线未损坏仍应可解（守卫：上面的失败不是「编码器坏了」）。 */
  compositor::CodecImage sanity;
  COMP_CHECK(compositor::codec_lib_decode(png.data(), png.size(), kFormatPng, &sanity, &error));
  COMP_CHECK(sanity.width == 96 && sanity.height == 64);
}
