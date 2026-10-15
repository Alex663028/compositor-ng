// AI生成
/*
 * vulkan_real_test.cpp — Vulkan 计算管线的**真机**验证宿主（非默认构建的一部分）。
 *
 * 背景：此前的证据边界是「设备侧只到可编译」（当时判断本机无 GPU / 无 Vulkan SDK）。
 * 后经探测推翻：本机有 AMD Radeon RX 6800（Vulkan 1.4.315，AMD 专有驱动）与
 * `vulkan-1.dll` 加载器。本用例因此把「编译器证据」升级为「真机运行证据」：
 *
 *   1) 真实创建 VkInstance / 选取计算队列族 / 创建设备与存储映像；
 *   2) 上传 SPIR-V 字模块、dispatch 计算管线、回读结果；
 *   3) 与 CPU 标量基准 `compositor_blend_pixel_scalar` 逐像素比对，要求 <= 1 LSB。
 *
 * 语义对齐（关键）：Vulkan 侧推送常量 alpha 对应着色器 `g_alpha`（图层不透明度），
 * 着色器内部再乘像素自身 alpha；CPU 标量入口的 `src_alpha` 是**有效 alpha**。
 * 故对照时 CPU 传 `像素alpha * src_alpha`，两侧才等价（见 render/gpu_kernel.h 的契约说明）。
 *
 * 运行：python scripts/run_vulkan_real.py [宽] [高]
 * 退出码：0 = 全部用例在 1 LSB 内且 GPU 路径确实跑过；2 = 设备侧未编入；1 = 超出容差。
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "compositing/composite_ops.h"
#include "render/vulkan_backend.h"

using namespace compositor;

namespace {

uint32_t g_state = 0;

uint32_t next_rand() {
  g_state = g_state * 1664525u + 1013904223u;
  return g_state >> 8;
}

/* 生成合法的预乘 RGBA8：先定 alpha，再取 0..alpha 的颜色分量。 */
void fill_premultiplied(std::vector<uint8_t>& buffer, uint32_t seed) {
  g_state = seed;
  for (size_t i = 0; i < buffer.size(); i += 4) {
    const uint32_t a = 24 + next_rand() % 232;
    buffer[i + 0] = static_cast<uint8_t>(next_rand() % (a + 1));
    buffer[i + 1] = static_cast<uint8_t>(next_rand() % (a + 1));
    buffer[i + 2] = static_cast<uint8_t>(next_rand() % (a + 1));
    buffer[i + 3] = static_cast<uint8_t>(a);
  }
}

struct CaseSummary {
  int max_delta = 0;
  size_t over_tolerance = 0;
  size_t pixels = 0;
};

CaseSummary compare_cpu(const std::vector<uint8_t>& gpu, const uint8_t* src,
                        const std::vector<uint8_t>& base, int32_t width, int32_t height,
                        uint32_t mode, float alpha) {
  CaseSummary summary;
  summary.pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  std::vector<uint8_t> cpu = base;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                            static_cast<size_t>(x)) * 4;
      const double pixel_alpha = static_cast<double>(src[index + 3]) / 255.0;
      compositor_blend_pixel_scalar(&cpu[index], &src[index], static_cast<comp_blend_mode>(mode),
                                   pixel_alpha * static_cast<double>(alpha));
    }
  }
  for (size_t i = 0; i < gpu.size(); ++i) {
    const int delta = static_cast<int>(gpu[i]) - static_cast<int>(cpu[i]);
    const int magnitude = delta < 0 ? -delta : delta;
    if (magnitude > summary.max_delta) summary.max_delta = magnitude;
    if (magnitude > 1) ++summary.over_tolerance;
  }

  /* 只对首个失败用例做像素级诊断，避免刷屏。 */
  static bool diag_printed = false;
  if (summary.over_tolerance > 0 && !diag_printed) {
    diag_printed = true;
    size_t zero_like = 0;
    size_t base_like = 0;
    for (size_t p = 0; p < summary.pixels; ++p) {
      const size_t index = p * 4;
      if (gpu[index] == 0 && gpu[index + 1] == 0 && gpu[index + 2] == 0 && gpu[index + 3] == 0) {
        ++zero_like;
      }
      bool same_as_base = true;
      for (int k = 0; k < 4; ++k) {
        if (gpu[index + static_cast<size_t>(k)] != base[index + static_cast<size_t>(k)]) {
          same_as_base = false;
          break;
        }
      }
      if (same_as_base) ++base_like;
    }
    std::printf("       [diag] 全零像素=%zu  等于基底像素=%zu  总像素=%zu\n", zero_like,
                base_like, summary.pixels);
    size_t printed = 0;
    for (size_t p = 0; p < summary.pixels && printed < 4; ++p) {
      const size_t index = p * 4;
      int dmax = 0;
      for (int k = 0; k < 4; ++k) {
        int d = static_cast<int>(gpu[index + static_cast<size_t>(k)]) -
                static_cast<int>(cpu[index + static_cast<size_t>(k)]);
        if (d < 0) d = -d;
        if (d > dmax) dmax = d;
      }
      if (dmax <= 1) continue;
      std::printf(
          "       px%zu src=(%u,%u,%u,%u) base=(%u,%u,%u,%u) gpu=(%u,%u,%u,%u) "
          "cpu=(%u,%u,%u,%u)\n",
          p, src[index], src[index + 1], src[index + 2], src[index + 3], base[index],
          base[index + 1], base[index + 2], base[index + 3], gpu[index], gpu[index + 1],
          gpu[index + 2], gpu[index + 3], cpu[index], cpu[index + 1], cpu[index + 2],
          cpu[index + 3]);
      ++printed;
    }
  }
  return summary;
}

double ms_since(const std::chrono::steady_clock::time_point& start) {
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  const int32_t width = argc > 1 ? std::atoi(argv[1]) : 128;
  const int32_t height = argc > 2 ? std::atoi(argv[2]) : 64;

  std::printf("=== Vulkan 计算管线真机验证 ===\n");
  std::string why;
  const bool probed = probe_vulkan(&why);
  std::printf("[env] probe_vulkan=%s %s\n", probed ? "true" : "false",
              probed ? "(loader + 物理设备可用)" : why.c_str());
  std::printf("[env] vulkan_spirv_embedded=%s spirv_bytes=%zu\n",
              vulkan_spirv_embedded() ? "true" : "false", vulkan_spirv_byte_count());

  if (argc > 3 && std::strcmp(argv[3], "--probe") == 0) {
    /* 三值可区分探针：输入基底 / 暂存源 / 正确结果互不相同。 */
    const int32_t pw = 4;
    const int32_t ph = 4;
    const size_t pbytes = static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4;
    std::vector<uint8_t> psrc(pbytes, 0);
    std::vector<uint8_t> pdst(pbytes, 0);
    for (size_t i = 0; i < pbytes; i += 4) {
      psrc[i + 0] = 255; psrc[i + 1] = 0; psrc[i + 2] = 0; psrc[i + 3] = 128;
      pdst[i + 0] = 0;   pdst[i + 1] = 0; pdst[i + 2] = 0; pdst[i + 3] = 255;
    }
    std::vector<uint8_t> out = pdst;
    std::string perr;
    const bool pok = vulkan_compute_blend(psrc.data(), pw, ph, 0u, 1.0f, out.data(), &perr);
    std::printf("[probe] ok=%d err=%s\n", pok ? 1 : 0, perr.c_str());
    std::printf("        out=(%u,%u,%u,%u)\n", out[0], out[1], out[2], out[3]);
    std::printf("        正确=(128,0,0,255) | 输入基底=(0,0,0,255) | 暂存源=(255,0,0,128)\n");
    return 0;
  }

  if (!probed) {
    std::printf("[SKIP] 本机无 Vulkan 加载器或物理设备，真机验证不可执行\n");
    return 2;
  }
  if (!vulkan_spirv_embedded()) {
    std::printf("[SKIP] 设备侧未编入（需 -DCOMPOSITOR_ENABLE_VULKAN_COMPUTE 且 SPIR-V 就位）\n");
    return 2;
  }

  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  std::vector<uint8_t> src(bytes);
  std::vector<uint8_t> base(bytes);
  fill_premultiplied(src, 0x12345678u);
  fill_premultiplied(base, 0x9abcdef0u);

  /* 覆盖分离型/非分离型：Normal, Darken, Color Burn, Linear Dodge, Overlay, Hard Mix,
   * Difference, Divide, Saturation, Luminosity。 */
  const uint32_t modes[] = {0, 1, 3, 8, 9, 16, 17, 19, 21, 23};
  const char* const mode_names[] = {"Normal",  "Darken",     "Color Burn", "Linear Dodge (Add)",
                                    "Overlay", "Hard Mix",   "Difference", "Divide",
                                    "Saturation", "Luminosity"};
  const float alphas[] = {1.0f, 0.5f};

  const size_t mode_count = sizeof(modes) / sizeof(modes[0]);
  const size_t alpha_count = sizeof(alphas) / sizeof(alphas[0]);

  int failed_cases = 0;
  int worst_delta = 0;
  size_t total_over = 0;
  size_t total_pixels_compared = 0;

  std::printf("[case] 画布 %dx%d（%zu 像素）· %zu 模式 x %zu 不透明度\n", width, height,
              static_cast<size_t>(width) * static_cast<size_t>(height), mode_count, alpha_count);

  for (size_t m = 0; m < mode_count; ++m) {
    for (size_t a = 0; a < alpha_count; ++a) {
      std::vector<uint8_t> gpu = base;
      std::string error;
      if (!vulkan_compute_blend(src.data(), width, height, modes[m], alphas[a], gpu.data(),
                                &error)) {
        std::printf("[FAIL] mode=%s alpha=%.2f: GPU 路径失败: %s\n", mode_names[m], alphas[a],
                    error.c_str());
        ++failed_cases;
        continue;
      }
      const CaseSummary summary =
          compare_cpu(gpu, src.data(), base, width, height, modes[m], alphas[a]);
      total_pixels_compared += summary.pixels;
      total_over += summary.over_tolerance;
      if (summary.max_delta > worst_delta) worst_delta = summary.max_delta;
      const bool pass = summary.over_tolerance == 0;
      if (!pass) ++failed_cases;
      std::printf("[%s] mode=%-20s alpha=%.2f max_delta=%d 超容差通道=%zu / %zu\n",
                  pass ? " OK " : "FAIL", mode_names[m], alphas[a], summary.max_delta,
                  summary.over_tolerance, summary.pixels * 4);
    }
  }

  /* 真机耗时对照（同一模式的单次全画布 dispatch vs CPU 逐像素）。 */
  const int32_t perf_w = 1024;
  const int32_t perf_h = 1024;
  const size_t perf_bytes = static_cast<size_t>(perf_w) * perf_h * 4;
  std::vector<uint8_t> perf_src(perf_bytes);
  std::vector<uint8_t> perf_dst(perf_bytes);
  fill_premultiplied(perf_src, 0x5a5a5a5au);
  fill_premultiplied(perf_dst, 0xa5a5a5a5u);
  std::string perf_error;
  const auto gpu_start = std::chrono::steady_clock::now();
  bool perf_ok = true;
  const int gpu_iterations = 20;
  for (int i = 0; i < gpu_iterations; ++i) {
    std::vector<uint8_t> dst = perf_dst;
    if (!vulkan_compute_blend(perf_src.data(), perf_w, perf_h, 0u, 1.0f, dst.data(),
                              &perf_error)) {
      perf_ok = false;
      break;
    }
  }
  const double gpu_ms = ms_since(gpu_start) / gpu_iterations;

  const auto cpu_start = std::chrono::steady_clock::now();
  std::vector<uint8_t> perf_cpu = perf_dst;
  for (int32_t y = 0; y < perf_h; ++y) {
    for (int32_t x = 0; x < perf_w; ++x) {
      const size_t index = (static_cast<size_t>(y) * perf_w + static_cast<size_t>(x)) * 4;
      const double pixel_alpha = static_cast<double>(perf_src[index + 3]) / 255.0;
      compositor_blend_pixel_scalar(&perf_cpu[index], &perf_src[index], COMP_BLEND_NORMAL,
                                   pixel_alpha);
    }
  }
  const double cpu_ms = ms_since(cpu_start);

  if (perf_ok) {
    std::printf("[perf] 1024x1024 Normal：GPU %.2f ms/次（%d 次均值）· CPU %.2f ms/次 · 加速比 %.2fx\n",
                gpu_ms, gpu_iterations, cpu_ms, cpu_ms / (gpu_ms > 0.0 ? gpu_ms : 1.0));
  } else {
    std::printf("[perf] GPU 计时失败：%s\n", perf_error.c_str());
  }

  std::printf("[summary] 比对像素=%zu 最差偏差=%d LSB 超容差通道=%zu 失败用例=%d\n",
              total_pixels_compared, worst_delta, total_over, failed_cases);
  if (failed_cases == 0 && worst_delta <= 1) {
    std::printf("[PASS] GPU 与 CPU 基准在 <=1 LSB 内一致（真机 dispatch 已验证）\n");
    return 0;
  }
  std::printf("[FAIL] 存在超出 1 LSB 的用例\n");
  return 1;
}
