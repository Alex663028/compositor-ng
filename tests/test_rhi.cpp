// AI生成
/*
 * test_rhi.cpp — M2 «D3D12 / Vulkan 接入同一 RHI» 的一致性护栏。
 *
 * 覆盖三件事：
 *   1. 后端选择与回落：CPU 可用、METAL 在本平台回落并给出原因、AUTO 的实际后端可解释；
 *   2. GPU 合成内核与 CPU 基准逐字节一致（24 种模式 × 多组不透明度 × 多组像素），
 *      用 gpu_kernel_blend_pixel（着色器 main 的 C++ 镜像）在没有 GPU 的机器上也能锁定语义；
 *   3. ABI 层端到端冒烟：AUTO 引擎的文档能真实合成出结果，后端枚举合法。
 */
#include "test_framework.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <compositor/comp_core.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_render.h>

#include "compositing/blend.h"
#include "compositing/composite_ops.h"
#include "render/gpu_kernel.h"
#include "render/rhi.h"

using namespace compositor;

namespace {

bool valid_backend(comp_backend backend) {
  return backend == COMP_BACKEND_CPU || backend == COMP_BACKEND_D3D12 ||
         backend == COMP_BACKEND_VULKAN || backend == COMP_BACKEND_METAL ||
         backend == COMP_BACKEND_AUTO;
}

}  // namespace

COMP_TEST(rhi_cpu_backend_caps_are_honest) {
  const BackendCaps cpu = probe_backend(COMP_BACKEND_CPU);
  COMP_CHECK(cpu.compiled);
  COMP_CHECK(cpu.device_available);
  COMP_CHECK(cpu.offscreen_compute);
  COMP_CHECK(!cpu.window_present);
  COMP_CHECK(cpu.note != nullptr);

  /* 非本平台后端不得谎称可用。 */
  const BackendCaps metal = probe_backend(COMP_BACKEND_METAL);
  COMP_CHECK(!metal.device_available);
}

/*
 * note 的地址稳定性：BackendCaps::note 是 const char*，调用方会一直持有；
 * 后续探测（哪怕是另一条文本）不得让先前返回的指针失效或改指到别的文本。
 */
COMP_TEST(rhi_probe_notes_survive_later_probes) {
  const BackendCaps metal = probe_backend(COMP_BACKEND_METAL);
  COMP_CHECK(metal.note != nullptr);
  const std::string metal_note(metal.note);

  for (int i = 0; i < 32; ++i) {
    const BackendCaps other = probe_backend(COMP_BACKEND_AUTO);
    COMP_CHECK(other.note != nullptr);
  }

  COMP_CHECK_MSG(std::string(metal.note) == metal_note,
                 "caps.note must stay valid and unchanged after later probes");
}

/* 并发探测：多线程同时探测不同后端，各自的 note 都必须是本次探测的文本。 */
COMP_TEST(rhi_probe_is_thread_safe) {
  const int kThreads = 4;
  const int kRounds = 64;
  std::atomic<int> bad{0};

  /* 基线在单线程下取得：并发轮次必须与之逐字一致（文本内容随环境而变，故不作硬编码）。 */
  const BackendCaps metal_probe = probe_backend(COMP_BACKEND_METAL);
  const BackendCaps auto_probe = probe_backend(COMP_BACKEND_AUTO);
  COMP_CHECK(metal_probe.note != nullptr);
  COMP_CHECK(auto_probe.note != nullptr);
  const std::string metal_baseline(metal_probe.note != nullptr ? metal_probe.note : "");
  const std::string auto_baseline(auto_probe.note != nullptr ? auto_probe.note : "");

  std::vector<std::thread> pool;
  pool.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([&bad, &metal_baseline, &auto_baseline]() {
      for (int i = 0; i < kRounds; ++i) {
        const BackendCaps metal = probe_backend(COMP_BACKEND_METAL);
        const BackendCaps automatic = probe_backend(COMP_BACKEND_AUTO);
        if (metal.note == nullptr || automatic.note == nullptr) {
          bad.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        /* 同一后端的诊断文本在同一次运行中恒定；被别处的探测改写或重分配即判失败。 */
        if (metal_baseline != metal.note) bad.fetch_add(1, std::memory_order_relaxed);
        if (auto_baseline != automatic.note) bad.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& worker : pool) worker.join();

  COMP_CHECK_MSG(bad.load() == 0, "concurrent probes must not corrupt each other's notes");
}

/*
 * AUTO 探测必须与实际选择一致：本构建造出 GPU 后端就报可用，回落 CPU 才报不可用。
 * 原实现拿 actual == backend 判定，AUTO 下恒不成立，把「已选中 GPU」谎报为不可用。
 */
COMP_TEST(rhi_auto_probe_agrees_with_selection) {
  std::string error;
  comp_backend actual = COMP_BACKEND_AUTO;
  std::unique_ptr<RenderBackend> backend =
      create_backend(nullptr, COMP_BACKEND_AUTO, &error, &actual);
  COMP_CHECK(backend != nullptr);
  const BackendCaps caps = probe_backend(COMP_BACKEND_AUTO);
  COMP_CHECK(caps.note != nullptr);
  const bool gpu_selected = actual != COMP_BACKEND_CPU;
  COMP_CHECK_MSG(caps.compiled == gpu_selected,
                 "AUTO probe must not contradict the backend AUTO actually selects");
  /* 回落 CPU 时 note 必须说明原因，不能空手报不可用。 */
  if (!gpu_selected) {
    COMP_CHECK_MSG(std::strstr(caps.note, "falling back to cpu") != nullptr,
                   "auto fallback reports its reason");
  }
}

COMP_TEST(rhi_backend_selection_and_fallback) {
  std::string error;
  comp_backend actual = COMP_BACKEND_AUTO;
  std::unique_ptr<RenderBackend> cpu = create_backend(nullptr, COMP_BACKEND_CPU, &error, &actual);
  COMP_CHECK(cpu != nullptr);
  COMP_CHECK(actual == COMP_BACKEND_CPU);
  COMP_CHECK(cpu->usable());
  COMP_CHECK(std::string(cpu->name()) == "cpu");

  /* METAL 在 Windows/HarmonyOS 上不可用：必须回落 CPU 并写明原因。 */
  error.clear();
  std::unique_ptr<RenderBackend> metal = create_backend(nullptr, COMP_BACKEND_METAL, &error, &actual);
  COMP_CHECK(metal != nullptr);
  COMP_CHECK(actual == COMP_BACKEND_CPU);
  COMP_CHECK(!error.empty());

  /* AUTO：按平台择优，实际后端必须落在合法集合内且与实例自称一致。 */
  error.clear();
  std::unique_ptr<RenderBackend> automatic =
      create_backend(nullptr, COMP_BACKEND_AUTO, &error, &actual);
  COMP_CHECK(automatic != nullptr);
  COMP_CHECK(valid_backend(actual));
  COMP_CHECK(actual != COMP_BACKEND_AUTO);
  COMP_CHECK(automatic->kind() == actual);
  COMP_CHECK(automatic->caps().compiled);
}

COMP_TEST(rhi_shaders_match_abi_blend_table) {
  std::string error;
  const bool valid = gpu_kernel_validate(&error);
  COMP_CHECK_MSG(valid, error.c_str());
  COMP_CHECK(error.empty());
  COMP_CHECK(gpu_kernel_blend_case_count() == 24);

  const std::string hlsl(gpu_kernel_hlsl());
  const std::string glsl(gpu_kernel_glsl());
  COMP_CHECK(hlsl.find("SV_DispatchThreadID") != std::string::npos);
  COMP_CHECK(hlsl.find("Linear Dodge (Add)") != std::string::npos);
  COMP_CHECK(glsl.find("gl_GlobalInvocationID") != std::string::npos);
  COMP_CHECK(glsl.find("Luminosity") != std::string::npos);
}

COMP_TEST(rhi_gpu_kernel_matches_cpu_blend) {
  const double alphas[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  const uint8_t dst_pixels[][4] = {
      {0, 0, 0, 0}, {255, 255, 255, 255}, {64, 128, 192, 255}, {200, 100, 50, 128}, {10, 20, 30, 0}};
  const uint8_t src_pixels[][4] = {
      {255, 0, 0, 255}, {0, 255, 0, 128}, {40, 60, 200, 200}, {255, 255, 255, 64}, {128, 128, 128, 255}};

  int mismatches = 0;
  int comparisons = 0;
  for (int mode = COMP_BLEND_NORMAL; mode <= COMP_BLEND_LUMINOSITY; ++mode) {
    for (double alpha : alphas) {
      for (const uint8_t* dst : dst_pixels) {
        for (const uint8_t* src : src_pixels) {
          uint8_t cpu_dst[4];
          uint8_t gpu_dst[4];
          std::memcpy(cpu_dst, dst, 4);
          std::memcpy(gpu_dst, dst, 4);

          const double pixel_alpha = static_cast<double>(src[3]) / 255.0;
          blend_pixels(cpu_dst, src, static_cast<comp_blend_mode>(mode), pixel_alpha * alpha);
          gpu_kernel_blend_pixel(gpu_dst, src, static_cast<comp_blend_mode>(mode), alpha);

          ++comparisons;
          if (std::memcmp(cpu_dst, gpu_dst, 4) != 0) ++mismatches;
        }
      }
    }
  }
  COMP_CHECK_MSG(comparisons == 24 * 5 * 5 * 5, "expected full 24x5x5x5 sweep");
  COMP_CHECK_MSG(mismatches == 0, "gpu kernel must mirror cpu blend bit-exactly");
}

COMP_TEST(rhi_abi_render_smoke) {
  comp_engine engine = nullptr;
  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-tests";
  engine_desc.backend = COMP_BACKEND_AUTO;
  COMP_CHECK(comp_engine_create(&engine_desc, &engine) == COMP_OK);
  COMP_CHECK(engine != nullptr);

  comp_document document = nullptr;
  comp_document_desc document_desc{};
  document_desc.width = 8;
  document_desc.height = 8;
  COMP_CHECK(comp_document_create(engine, &document_desc, &document) == COMP_OK);
  COMP_CHECK(document != nullptr);

  comp_backend actual = COMP_BACKEND_AUTO;
  COMP_CHECK(comp_render_backend(document, &actual) == COMP_OK);
  COMP_CHECK(valid_backend(actual));

  std::vector<uint8_t> pixels(static_cast<size_t>(8) * 8 * 4, 0xAB);
  COMP_CHECK(comp_render_flatten(document, pixels.data(), COMP_PIXEL_RGBA8, 8 * 4) == COMP_OK);
  bool all_zero = true;
  for (uint8_t value : pixels) {
    if (value != 0) {
      all_zero = false;
      break;
    }
  }
  COMP_CHECK_MSG(all_zero, "empty document flattens to a fully transparent canvas");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}
