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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <compositor/comp_core.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_render.h>

#include "compositing/blend.h"
#include "compositing/composite_ops.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "render/gpu_kernel.h"
#include "render/rhi.h"

using namespace compositor;

namespace {

bool valid_backend(comp_backend backend) {
  return backend == COMP_BACKEND_CPU || backend == COMP_BACKEND_D3D12 ||
         backend == COMP_BACKEND_VULKAN || backend == COMP_BACKEND_METAL ||
         backend == COMP_BACKEND_AUTO;
}

comp_engine make_abi_engine(comp_backend backend) {
  comp_engine engine = nullptr;
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.backend = backend;
  if (comp_engine_create(&desc, &engine) != COMP_OK) return nullptr;
  return engine;
}

/* 在文档顶层追加一个「整幅画布 RGBA8」图层——GPU 子集的合法形态。 */
comp_layer_id add_canvas_layer(comp_document document, int32_t size, comp_blend_mode blend,
                               float opacity, int seed) {
  comp_layer_desc desc{};
  desc.name = "layer";
  desc.blend = blend;
  desc.opacity = opacity;
  desc.transform = comp_rect{0, 0, size, size};
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  if (comp_layer_add(document, &desc, &id) != COMP_OK) return 0;

  std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
  for (int32_t y = 0; y < size; ++y) {
    for (int32_t x = 0; x < size; ++x) {
      uint8_t* p = &pixels[(static_cast<size_t>(y) * static_cast<size_t>(size) + x) * 4];
      const uint32_t alpha = (static_cast<uint32_t>(x * 11 + y * 7 + seed * 29) % 200u) + 40u;
      /* 预乘：颜色分量不得超过 alpha。 */
      p[0] = static_cast<uint8_t>(((x * 17 + seed * 31) % 255) * alpha / 255);
      p[1] = static_cast<uint8_t>(((y * 13 + seed * 47) % 255) * alpha / 255);
      p[2] = static_cast<uint8_t>((((x + y) * 7 + seed * 11) % 255) * alpha / 255);
      p[3] = static_cast<uint8_t>(alpha);
    }
  }
  comp_image image{};
  image.width = size;
  image.height = size;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(size) * 4;
  image.pixels = pixels.data();
  if (comp_layer_set_pixels(document, id, &image) != COMP_OK) return 0;
  return id;
}

/*
 * 场景：背景层（不透明底色）+ 三层不同混合模式/不透明度的整幅画布图层。
 * with_adjustment = true 时给顶层挂一个曝光调整——它不在 GPU 子集内，
 * 用来验证「按文档回落 CPU」。
 */
comp_document build_scene(comp_engine engine, int32_t size, bool with_adjustment) {
  comp_document document = nullptr;
  comp_document_desc desc{};
  desc.width = size;
  desc.height = size;
  if (comp_document_create(engine, &desc, &document) != COMP_OK) return nullptr;

  size_t count = 0;
  if (comp_layer_count(document, &count) != COMP_OK || count == 0) return document;
  std::vector<comp_layer_id> ids(count);
  if (comp_layer_list(document, ids.data(), ids.size(), &count) != COMP_OK) return document;

  std::vector<uint8_t> background(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
  for (size_t i = 0; i + 3 < background.size(); i += 4) {
    background[i + 0] = 24;
    background[i + 1] = 96;
    background[i + 2] = 168;
    background[i + 3] = 255;
  }
  comp_image background_image{};
  background_image.width = size;
  background_image.height = size;
  background_image.format = COMP_PIXEL_RGBA8;
  background_image.stride = static_cast<size_t>(size) * 4;
  background_image.pixels = background.data();
  comp_layer_set_pixels(document, ids.front(), &background_image);

  add_canvas_layer(document, size, COMP_BLEND_NORMAL, 1.0f, 1);
  add_canvas_layer(document, size, COMP_BLEND_MULTIPLY, 0.75f, 2);
  const comp_layer_id top = add_canvas_layer(document, size, COMP_BLEND_SCREEN, 0.5f, 3);
  if (with_adjustment && top != 0) {
    /* 曝光调整的 JSON 键见 docs/adjustment-json-keys.md；键写错会返回
       COMP_ERR_INVALID_ARGUMENT，而「加了却没加上」会让这个用例悄悄退化成
       「GPU 合法文档」，所以这里必须断言结果。 */
    COMP_CHECK_MSG(comp_layer_add_adjustment(document, top, COMP_ADJUSTMENT_EXPOSURE,
                                            "{\"exposureSettings\":{\"exposure\":0.2}}") == COMP_OK,
                   "the adjustment that makes the document leave the GPU subset must apply");
  }
  return document;
}

/* 返回 (max_diff, diff_count)；两图必须同尺寸。 */
void compare_pixels(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual,
                    int* out_max_diff, size_t* out_diff_count, size_t* out_first_diff) {
  int max_diff = 0;
  size_t diff_count = 0;
  size_t first_diff = 0;
  const size_t limit = expected.size() < actual.size() ? expected.size() : actual.size();
  for (size_t i = 0; i < limit; ++i) {
    const int diff = std::abs(static_cast<int>(expected[i]) - static_cast<int>(actual[i]));
    if (diff != 0) {
      if (diff_count == 0) first_diff = i;
      ++diff_count;
    }
    if (diff > max_diff) max_diff = diff;
  }
  /* 长度不同时按「最大差异」记一次不合规。 */
  if (expected.size() != actual.size()) {
    max_diff = 255;
    ++diff_count;
  }
  if (out_max_diff != nullptr) *out_max_diff = max_diff;
  if (out_diff_count != nullptr) *out_diff_count = diff_count;
  if (out_first_diff != nullptr) *out_first_diff = first_diff;
}

std::vector<uint8_t> flatten_document(comp_document document, int32_t size) {
  std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0xCD);
  if (comp_render_flatten(document, pixels.data(), COMP_PIXEL_RGBA8,
                          static_cast<size_t>(size) * 4) != COMP_OK) {
    return std::vector<uint8_t>();
  }
  return pixels;
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

/* ---------------------------------------------------------------- S13：GPU 接主渲染路径 */

/*
 * 引擎持有唯一后端实例并如实报告实际生效者：ABI 报的后端必须与 AUTO 的
 * create_backend 选择一致（同一机器、同一构建两种解析方式不能各说各话）。
 */
COMP_TEST(rhi_engine_owns_backend_and_reports_actual) {
  comp_engine auto_engine = make_abi_engine(COMP_BACKEND_AUTO);
  COMP_CHECK(auto_engine != nullptr);
  if (auto_engine == nullptr) return;

  comp_backend reported = COMP_BACKEND_AUTO;
  COMP_CHECK(comp_engine_backend(auto_engine, &reported) == COMP_OK);
  COMP_CHECK(valid_backend(reported));
  COMP_CHECK_MSG(reported != COMP_BACKEND_AUTO, "engine must resolve AUTO to a concrete backend");

  std::string error;
  comp_backend picked = COMP_BACKEND_AUTO;
  std::unique_ptr<RenderBackend> probe =
      create_backend(nullptr, COMP_BACKEND_AUTO, &error, &picked);
  COMP_CHECK(probe != nullptr);
  COMP_CHECK_MSG(reported == picked,
                 "engine's reported backend must match the backend AUTO actually creates");
  comp_engine_destroy(auto_engine);

  /* 显式 CPU 引擎必须原样报告 CPU（字节基准口径不变）。 */
  comp_engine cpu_engine = make_abi_engine(COMP_BACKEND_CPU);
  COMP_CHECK(cpu_engine != nullptr);
  if (cpu_engine != nullptr) {
    comp_backend cpu_reported = COMP_BACKEND_AUTO;
    COMP_CHECK(comp_engine_backend(cpu_engine, &cpu_reported) == COMP_OK);
    COMP_CHECK(cpu_reported == COMP_BACKEND_CPU);
    comp_engine_destroy(cpu_engine);
  }
}

/*
 * GPU 真机路径与 CPU 基准对齐：同一份场景分别用显式 CPU 引擎和 AUTO 引擎
 * 合成，逐像素比对。若 AUTO 选中 GPU，允许 ±1 LSB 的浮点舍入差（CPU 用
 * double、GPU 用 float）；回落 CPU 时必须逐字节一致。
 */
COMP_TEST(rhi_gpu_render_matches_cpu_when_eligible) {
  const int32_t size = 16;
  comp_engine cpu_engine = make_abi_engine(COMP_BACKEND_CPU);
  comp_engine auto_engine = make_abi_engine(COMP_BACKEND_AUTO);
  COMP_CHECK(cpu_engine != nullptr);
  COMP_CHECK(auto_engine != nullptr);
  if (cpu_engine == nullptr || auto_engine == nullptr) {
    comp_engine_destroy(cpu_engine);
    comp_engine_destroy(auto_engine);
    return;
  }

  comp_document cpu_doc = build_scene(cpu_engine, size, false);
  comp_document auto_doc = build_scene(auto_engine, size, false);
  COMP_CHECK(cpu_doc != nullptr);
  COMP_CHECK(auto_doc != nullptr);
  if (cpu_doc != nullptr && auto_doc != nullptr) {
    const std::vector<uint8_t> cpu_pixels = flatten_document(cpu_doc, size);
    const std::vector<uint8_t> auto_pixels = flatten_document(auto_doc, size);
    COMP_CHECK_MSG(!cpu_pixels.empty(), "cpu reference render must succeed");
    COMP_CHECK_MSG(!auto_pixels.empty(), "auto backend render must succeed");

    comp_backend auto_backend = COMP_BACKEND_AUTO;
    COMP_CHECK(comp_engine_backend(auto_engine, &auto_backend) == COMP_OK);
    const bool gpu_path = auto_backend != COMP_BACKEND_CPU;

    int max_diff = 255;
    size_t diff_count = 0;
    size_t first_diff = 0;
    compare_pixels(cpu_pixels, auto_pixels, &max_diff, &diff_count, &first_diff);

    const int tolerance = gpu_path ? 1 : 0;
    std::string message = "gpu/cpu pixel parity (max_diff=" + std::to_string(max_diff) +
                          ", diffs=" + std::to_string(diff_count) + ", first_byte=" +
                          std::to_string(first_diff) + ")";
    COMP_CHECK_MSG(max_diff <= tolerance, message.c_str());
  }

  comp_document_destroy(cpu_doc);
  comp_document_destroy(auto_doc);
  comp_engine_destroy(cpu_engine);
  comp_engine_destroy(auto_engine);
}

/*
 * 半透明整幅画布叠加（SCREEN + 不透明度 0.75）的 GPU/CPU 对拍——外壳自检用的正是
 * 这个场景。前提是**预乘**输入（颜色分量 <= alpha）：S13 曾用非预乘像素
 * （200,64,32,128）做这条对拍，GPU 浮点与 CPU 整数在饱和钳制上分叉到 34 级；
 * 那已超出 `comp_layer.h` 的预乘契约，是**非法输入**，不是两后端分歧。
 * 这里用合法预乘数据把「半透明底 + 不透明度 + 非 NORMAL 混合」锁进 ±1 LSB。
 */
COMP_TEST(rhi_gpu_matches_cpu_for_translucent_screen_stack) {
  const int32_t size = 16;
  comp_engine cpu_engine = make_abi_engine(COMP_BACKEND_CPU);
  comp_engine auto_engine = make_abi_engine(COMP_BACKEND_AUTO);
  COMP_CHECK(cpu_engine != nullptr);
  COMP_CHECK(auto_engine != nullptr);
  if (cpu_engine == nullptr || auto_engine == nullptr) {
    comp_engine_destroy(cpu_engine);
    comp_engine_destroy(auto_engine);
    return;
  }

  const auto build_translucent_stack = [size](comp_engine engine) -> comp_document {
    comp_document document = nullptr;
    comp_document_desc desc{};
    desc.width = size;
    desc.height = size;
    if (comp_document_create(engine, &desc, &document) != COMP_OK) return nullptr;

    size_t count = 0;
    if (comp_layer_count(document, &count) != COMP_OK || count == 0) return document;
    std::vector<comp_layer_id> ids(count);
    if (comp_layer_list(document, ids.data(), ids.size(), &count) != COMP_OK) return document;

    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
    comp_image image{};
    image.width = size;
    image.height = size;
    image.format = COMP_PIXEL_RGBA8;
    image.stride = static_cast<size_t>(size) * 4;
    image.pixels = pixels.data();

    /* 底层：合法的半透明预乘底色（颜色分量都 <= 192）。 */
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      pixels[i + 0] = 96;
      pixels[i + 1] = 72;
      pixels[i + 2] = 48;
      pixels[i + 3] = 192;
    }
    if (comp_layer_set_pixels(document, ids.front(), &image) != COMP_OK) return document;

    comp_layer_desc top_desc{};
    top_desc.name = "translucent-top";
    top_desc.blend = COMP_BLEND_SCREEN;
    top_desc.opacity = 0.75f;
    top_desc.transform = comp_rect{0, 0, size, size};
    top_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
    comp_layer_id top = 0;
    if (comp_layer_add(document, &top_desc, &top) != COMP_OK || top == 0) return document;

    /* 顶层：直通色 (200,64,32) @ alpha 128 → 预乘 (100,32,16,128)。 */
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      pixels[i + 0] = 100;
      pixels[i + 1] = 32;
      pixels[i + 2] = 16;
      pixels[i + 3] = 128;
    }
    comp_layer_set_pixels(document, top, &image);
    return document;
  };

  comp_document cpu_doc = build_translucent_stack(cpu_engine);
  comp_document auto_doc = build_translucent_stack(auto_engine);
  COMP_CHECK(cpu_doc != nullptr);
  COMP_CHECK(auto_doc != nullptr);
  if (cpu_doc != nullptr && auto_doc != nullptr) {
    const std::vector<uint8_t> cpu_pixels = flatten_document(cpu_doc, size);
    const std::vector<uint8_t> auto_pixels = flatten_document(auto_doc, size);
    COMP_CHECK_MSG(!cpu_pixels.empty(), "cpu reference render must succeed");
    COMP_CHECK_MSG(!auto_pixels.empty(), "auto backend render must succeed");

    comp_backend auto_backend = COMP_BACKEND_AUTO;
    COMP_CHECK(comp_engine_backend(auto_engine, &auto_backend) == COMP_OK);
    const bool gpu_path = auto_backend != COMP_BACKEND_CPU;

    int max_diff = 255;
    size_t diff_count = 0;
    size_t first_diff = 0;
    compare_pixels(cpu_pixels, auto_pixels, &max_diff, &diff_count, &first_diff);
    const int tolerance = gpu_path ? 1 : 0;
    std::string message = "translucent screen stack parity (max_diff=" +
                          std::to_string(max_diff) + ", diffs=" + std::to_string(diff_count) +
                          ", first_byte=" + std::to_string(first_diff) + ")";
    COMP_CHECK_MSG(max_diff <= tolerance, message.c_str());
  }

  comp_document_destroy(cpu_doc);
  comp_document_destroy(auto_doc);
  comp_engine_destroy(cpu_engine);
  comp_engine_destroy(auto_engine);
}

/*
 * 文档超出 GPU 子集（这里用图层调整构造）时必须按文档回落 CPU：
 * 结果与 CPU 基准逐字节一致，且调用仍返回 COMP_OK——绝不为了「用 GPU」而
 * 输出少算一层的错误画面。
 */
/*
 * GPU 真的参与了渲染的**证据**测试。
 *
 * 为什么单靠像素比对不够：ABI 层在 GPU 渲染失败时会按文档回落 CPU，输出照样正确，
 * 于是「GPU 已接入」可以长期看起来成立而设备根本没跑过（S13 就抓到过一次：根 UAV
 * 绑纹理导致 PSO 创建失败，每帧回落 CPU，而所有像素测试全绿）。这里直接持有
 * D3D12 后端实例渲染一个合法子集文档，用 last_render_offloaded() 证明
 * 「上传 → dispatch → 回读」整条设备链路真的走通。
 * 本构建未编入 D3D12 或本机无设备时跳过（不算失败）。
 */
COMP_TEST(rhi_gpu_backend_really_offloads_an_eligible_document) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);

  std::string create_error;
  comp_backend actual = COMP_BACKEND_AUTO;
  std::unique_ptr<RenderBackend> gpu =
      create_backend(&engine, COMP_BACKEND_D3D12, &create_error, &actual);
  if (gpu == nullptr || gpu->kind() != COMP_BACKEND_D3D12 || !gpu->caps().offscreen_compute) {
    return; /* 本机没有可验证的 D3D12 设备 */
  }

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  comp_layer_desc layer_desc{};
  layer_desc.name = "canvas";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f; /* 零初始化的 desc 里 opacity = 0，必须显式给 1 */
  layer_desc.transform = comp_rect{0, 0, size, size};
  comp_layer_id id = 0;
  Layer* layer = document.create_layer(layer_desc, &id);
  COMP_CHECK_MSG(layer != nullptr, "create a full-canvas layer");
  if (layer == nullptr) return;
  layer->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 40, 80, 120, 200);

  std::string why;
  COMP_CHECK_MSG(gpu->can_render_document(document, &why), why.c_str());

  std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
  comp_render_target target{};
  target.size = comp_size{size, size};
  target.format = COMP_PIXEL_RGBA8;
  target.kind = COMP_SURFACE_CPU_BUFFER;
  target.stride = static_cast<size_t>(size) * 4;
  target.pixels = pixels.data();
  COMP_CHECK_MSG(gpu->render_document(document, target, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(),
                 "an eligible document must run on the device instead of silently falling back");
  COMP_CHECK_MSG(!gpu->last_render_offloaded() || pixels[3] > 0,
                 "the offloaded render must have written pixels through readback");
}

COMP_TEST(rhi_gpu_falls_back_to_cpu_for_unsupported_documents) {
  const int32_t size = 16;
  comp_engine cpu_engine = make_abi_engine(COMP_BACKEND_CPU);
  comp_engine auto_engine = make_abi_engine(COMP_BACKEND_AUTO);
  COMP_CHECK(cpu_engine != nullptr);
  COMP_CHECK(auto_engine != nullptr);
  if (cpu_engine == nullptr || auto_engine == nullptr) {
    comp_engine_destroy(cpu_engine);
    comp_engine_destroy(auto_engine);
    return;
  }

  comp_document cpu_doc = build_scene(cpu_engine, size, true);
  comp_document auto_doc = build_scene(auto_engine, size, true);
  COMP_CHECK(cpu_doc != nullptr);
  COMP_CHECK(auto_doc != nullptr);
  if (cpu_doc != nullptr && auto_doc != nullptr) {
    const std::vector<uint8_t> cpu_pixels = flatten_document(cpu_doc, size);
    const std::vector<uint8_t> auto_pixels = flatten_document(auto_doc, size);
    COMP_CHECK_MSG(!cpu_pixels.empty(), "cpu reference render must succeed");
    COMP_CHECK_MSG(!auto_pixels.empty(), "auto backend must fall back, not fail");

    int max_diff = 255;
    size_t diff_count = 0;
    size_t first_diff = 0;
    compare_pixels(cpu_pixels, auto_pixels, &max_diff, &diff_count, &first_diff);
    std::string message = "cpu fallback must be byte-exact (max_diff=" +
                          std::to_string(max_diff) + ", first_byte=" +
                          std::to_string(first_diff) + ")";
    COMP_CHECK_MSG(max_diff == 0, message.c_str());

    /* 渲染本身仍成功；present/wait 对离屏后端是无操作。 */
    COMP_CHECK(comp_render_present(auto_doc) == COMP_OK);
    COMP_CHECK(comp_render_wait(auto_doc) == COMP_OK);
  }

  comp_document_destroy(cpu_doc);
  comp_document_destroy(auto_doc);
  comp_engine_destroy(cpu_engine);
  comp_engine_destroy(auto_engine);
}
