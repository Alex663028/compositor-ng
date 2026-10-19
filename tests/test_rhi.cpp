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
#include <cstdint>
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
#include "render/d3d12_backend.h"
#include "render/gpu_kernel.h"
#include "render/gpu_plan.h"
#include "render/rhi.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace compositor;

namespace {

#if defined(_WIN32)
/* 窗口直呈用例需要真实 HWND：建一个不显示的普通窗口即可（swapchain 不要求可见）。 */
HWND create_hidden_test_window(int width, int height) {
  static const wchar_t kClass[] = L"CompositorRhiTestWindow";
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClass;
    if (RegisterClassExW(&wc) == 0) return nullptr;
    registered = true;
  }
  return CreateWindowExW(0, kClass, L"compositor-rhi-test", WS_OVERLAPPEDWINDOW, 0, 0, width,
                         height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
#endif

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

/*
 * S16：DXGI 窗口直呈（COMP_SURFACE_HWND）。
 *
 * 单靠像素比对证明不了「没回读」——ABI 层随时可以静默回落到 CPU 回读路径而画面
 * 依旧正确。所以这里直接持有 D3D12 后端实例，断言四件事：
 *   1. 合法子集文档 + HWND 目标真的在设备上跑（last_render_offloaded）；
 *   2. 窗口路径不产生整幅回读（readback_frames 不增长），且 present 之前没上屏；
 *   3. 进 Present 之前那一帧后备缓冲像素与 CPU 基准一致（±1 LSB，预乘契约内）；
 *   4. 离屏路径仍照旧回读——两条路径的差异是可观测的，不是自说自话。
 * 未编入 D3D12 / 本机无设备 / 建窗失败 → 跳过（不算失败）。
 */
COMP_TEST(rhi_gpu_presents_eligible_document_to_hwnd_without_readback) {
#if defined(_WIN32)
  const int32_t size = 32;
  HWND hwnd = create_hidden_test_window(320, 240);
  COMP_CHECK_MSG(hwnd != nullptr, "create the hidden test window");
  if (hwnd == nullptr) return;
  struct WindowGuard {
    HWND handle;
    ~WindowGuard() {
      if (handle != nullptr) DestroyWindow(handle);
    }
  } window_guard{hwnd};

  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);

  std::string why;
  comp_backend actual = COMP_BACKEND_AUTO;
  std::unique_ptr<RenderBackend> gpu = create_backend(&engine, COMP_BACKEND_D3D12, &why, &actual);
  if (gpu == nullptr || gpu->kind() != COMP_BACKEND_D3D12) return; /* 本构建没有 D3D12 */
  if (!gpu->caps().window_present) return; /* 本机没有可用的窗口上屏路径 */

  /* 与既有 GPU 用例同一形态：整幅画布 RGBA8、预乘、无组/蒙版/调整。 */
  Document document(&engine, size, size, 72.0f, "sRGB");
  comp_layer_desc layer_desc{};
  layer_desc.name = "canvas";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.transform = comp_rect{0, 0, size, size};
  comp_layer_id id = 0;
  Layer* layer = document.create_layer(layer_desc, &id);
  COMP_CHECK_MSG(layer != nullptr, "create a full-canvas layer");
  if (layer == nullptr) return;
  layer->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 40, 80, 120, 200);

  /* CPU 基准：同一个 Document，交给 CPU 后端渲染到缓冲（逐字节可比）。 */
  std::unique_ptr<RenderBackend> cpu = create_backend(&engine, COMP_BACKEND_CPU, &why, &actual);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
  comp_render_target cpu_target{};
  cpu_target.size = comp_size{size, size};
  cpu_target.format = COMP_PIXEL_RGBA8;
  cpu_target.kind = COMP_SURFACE_CPU_BUFFER;
  cpu_target.stride = static_cast<size_t>(size) * 4;
  cpu_target.pixels = cpu_pixels.data();
  COMP_CHECK(cpu->render_document(document, cpu_target, &why));

  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  d3d12->set_window_readback_verification(true);

  comp_render_target window_target{};
  window_target.size = comp_size{size, size};
  window_target.format = COMP_PIXEL_RGBA8;
  window_target.kind = COMP_SURFACE_HWND;
  window_target.native_handle = hwnd;

  why.clear();
  COMP_CHECK_MSG(gpu->render_document(document, window_target, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(),
                 "an eligible window target must run on the device");
  COMP_CHECK_MSG(d3d12->readback_frames() == 0,
                 "the window surface path must not read the frame back to the CPU");
  COMP_CHECK(d3d12->presented_frames() == 0);

  /* 测试专用回读：拿到的是「即将 Present」的那一帧后备缓冲内容。 */
  std::vector<uint8_t> presented;
  int presented_width = 0;
  int presented_height = 0;
  COMP_CHECK_MSG(d3d12->take_window_readback(&presented, &presented_width, &presented_height),
                 "the verification readback of the presented frame must be available");
  COMP_CHECK(presented_width == size && presented_height == size);

  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, presented, &max_diff, &diff_count, &first_diff);
  std::string message = "presented back buffer parity (max_diff=" + std::to_string(max_diff) +
                        ", diffs=" + std::to_string(diff_count) +
                        ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!presented.empty() && max_diff <= 1, message.c_str());

  why.clear();
  COMP_CHECK_MSG(gpu->present(document, &why), why.c_str());
  COMP_CHECK_MSG(d3d12->presented_frames() == 1, "present must actually submit a frame");
  COMP_CHECK_MSG(d3d12->readback_frames() == 0, "presenting must not introduce a readback");

  /*
   * 多帧轮转守卫：flip-model 下每帧必须换用另一块后备缓冲。早先的实现每帧
   * GetBuffer(0) 再 Release，第二帧实际写进「正在扫描输出」的表面，第二帧
   * ExecuteCommandLists 之后设备直接被移除（0x887A0005，原因 0x887A002B），
   * 表现为「只能上屏一帧」。这里连续再上屏 3 帧，每帧都对拍内容。
   */
  for (int frame = 0; frame < 3; ++frame) {
    why.clear();
    COMP_CHECK_MSG(gpu->render_document(document, window_target, &why), why.c_str());
    COMP_CHECK_MSG(gpu->last_render_offloaded(),
                   "every presented frame must still run on the device");
    std::vector<uint8_t> frame_pixels;
    int frame_width = 0;
    int frame_height = 0;
    COMP_CHECK_MSG(
        d3d12->take_window_readback(&frame_pixels, &frame_width, &frame_height),
        "the verification readback of every presented frame must be available");
    COMP_CHECK(frame_width == size && frame_height == size);
    int frame_max_diff = 255;
    size_t frame_diffs = 0;
    size_t frame_first_diff = 0;
    compare_pixels(cpu_pixels, frame_pixels, &frame_max_diff, &frame_diffs, &frame_first_diff);
    const std::string frame_message =
        "presented frame " + std::to_string(frame + 2) + " parity (max_diff=" +
        std::to_string(frame_max_diff) + ", diffs=" + std::to_string(frame_diffs) +
        ", first_byte=" + std::to_string(frame_first_diff) + ")";
    COMP_CHECK_MSG(!frame_pixels.empty() && frame_max_diff <= 1, frame_message.c_str());
    why.clear();
    COMP_CHECK_MSG(gpu->present(document, &why), why.c_str());
  }
  COMP_CHECK_MSG(d3d12->presented_frames() == 4,
                 "four window frames must have been submitted without a device loss");
  COMP_CHECK_MSG(d3d12->readback_frames() == 0,
                 "presenting further frames must not introduce a readback");

  /* 离屏路径仍走回读：两条路径的差异可观测，窗口直呈不是把回读改名了事。 */
  std::vector<uint8_t> offscreen_pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4,
                                        0);
  comp_render_target offscreen_target{};
  offscreen_target.size = comp_size{size, size};
  offscreen_target.format = COMP_PIXEL_RGBA8;
  offscreen_target.kind = COMP_SURFACE_CPU_BUFFER;
  offscreen_target.stride = static_cast<size_t>(size) * 4;
  offscreen_target.pixels = offscreen_pixels.data();
  why.clear();
  COMP_CHECK_MSG(gpu->render_document(document, offscreen_target, &why), why.c_str());
  COMP_CHECK_MSG(d3d12->readback_frames() == 1,
                 "the offscreen path must still count exactly one readback");
#endif /* _WIN32 */
}

/*
 * 没有 GPU 时 HWND 表面必须被**明确拒绝**：CPU 后端不能假装上屏，也不能悄悄只画
 * 一半。拒绝的是表面类型，不是文档——同一文档改走 CPU 缓冲照旧成功。
 */
COMP_TEST(rhi_hwnd_target_is_rejected_without_gpu_backend) {
  const int32_t size = 16;
  comp_engine cpu_engine = make_abi_engine(COMP_BACKEND_CPU);
  COMP_CHECK(cpu_engine != nullptr);
  if (cpu_engine == nullptr) return;
  comp_document doc = build_scene(cpu_engine, size, false);
  COMP_CHECK(doc != nullptr);
  if (doc != nullptr) {
    comp_render_target window_target{};
    window_target.size = comp_size{size, size};
    window_target.format = COMP_PIXEL_RGBA8;
    window_target.kind = COMP_SURFACE_HWND;
    /* 伪句柄：CPU 后端按表面类型就该拒绝，绝不该解引用它。 */
    window_target.native_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    COMP_CHECK_MSG(comp_render_document(doc, &window_target) != COMP_OK,
                   "the CPU backend must refuse a window surface");

    std::vector<uint8_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
    comp_render_target cpu_target{};
    cpu_target.size = comp_size{size, size};
    cpu_target.format = COMP_PIXEL_RGBA8;
    cpu_target.kind = COMP_SURFACE_CPU_BUFFER;
    cpu_target.stride = static_cast<size_t>(size) * 4;
    cpu_target.pixels = pixels.data();
    COMP_CHECK_MSG(comp_render_document(doc, &cpu_target) == COMP_OK,
                   "the same document must still render to a CPU buffer");
    /* 离屏后端 present 仍是合法无操作。 */
    COMP_CHECK(comp_render_present(doc) == COMP_OK);
    COMP_CHECK(comp_render_wait(doc) == COMP_OK);
  }
  comp_document_destroy(doc);
  comp_engine_destroy(cpu_engine);
}

/* ==================================================================
 * S17：GPU 子集分层（按合成单元分类 + CPU 烘焙）
 * ================================================================== */

namespace {

/* 生成一张「预乘合法」的测试图案（颜色分量不超过 alpha）。 */
void fill_test_pattern(ImageBuffer* buffer, int32_t seed) {
  if (buffer == nullptr || buffer->empty()) return;
  for (int32_t y = 0; y < buffer->height; ++y) {
    uint8_t* row = buffer->writable_row(static_cast<size_t>(y));
    for (int32_t x = 0; x < buffer->width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      const uint32_t alpha =
          (static_cast<uint32_t>(x * 9 + y * 5 + seed * 17) % 180u) + 60u;
      p[0] = static_cast<uint8_t>(((x * 21 + seed * 13) % 255) * alpha / 255);
      p[1] = static_cast<uint8_t>(((y * 19 + seed * 7) % 255) * alpha / 255);
      p[2] = static_cast<uint8_t>((((x + y) * 11 + seed * 3) % 255) * alpha / 255);
      p[3] = static_cast<uint8_t>(alpha);
    }
  }
}

Layer* add_test_layer(Document& doc, const char* name, comp_blend_mode blend, float opacity,
                      comp_rect bounds, comp_layer_id parent, bool group) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = blend;
  desc.opacity = opacity;
  desc.transform = bounds;
  desc.is_group = group ? 1 : 0;
  desc.parent = parent;
  comp_layer_id id = 0;
  return doc.create_layer(desc, &id);
}

std::unique_ptr<RenderBackend> make_backend(Engine* engine, comp_backend kind,
                                            std::string* error) {
  comp_backend actual = COMP_BACKEND_AUTO;
  return create_backend(engine, kind, error, &actual);
}

bool render_to_buffer(RenderBackend& backend, Document& doc, int32_t size,
                      std::vector<uint8_t>* pixels, std::string* error) {
  if (pixels == nullptr) return false;
  pixels->assign(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
  comp_render_target target{};
  target.size = comp_size{size, size};
  target.format = COMP_PIXEL_RGBA8;
  target.kind = COMP_SURFACE_CPU_BUFFER;
  target.stride = static_cast<size_t>(size) * 4;
  target.pixels = pixels->data();
  return backend.render_document(doc, target, error);
}

/*
 * S17 场景：一个把「旧闸门会拒绝的全部形态」塞在一起的文档。
 * 期望：build_gpu_plan 不再整篇拒绝，而是把它拆成原生/烘焙单元。
 */
void build_layered_scene(Document& document, int32_t size) {
  Layer* background =
      add_test_layer(document, "bg", COMP_BLEND_NORMAL, 1.0f, comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(background != nullptr);
  background->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 24, 96, 168, 255);

  /* 裁剪 + 偏移：像素只有 8x6，落在 (7,9)。 */
  Layer* cropped =
      add_test_layer(document, "crop", COMP_BLEND_MULTIPLY, 0.75f, comp_rect{7, 9, 8, 6}, 0, false);
  COMP_CHECK(cropped != nullptr);
  cropped->pixels.resize(8, 6, COMP_PIXEL_RGBA8);
  fill_test_pattern(&cropped->pixels, 3);

  /* 蒙版：整幅画布像素 + 同尺寸渐变蒙版。 */
  Layer* masked = add_test_layer(document, "masked", COMP_BLEND_SCREEN, 0.8f,
                                 comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(masked != nullptr);
  masked->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&masked->pixels, 5);
  masked->mask.resize(size, size, 255);
  for (int32_t y = 0; y < size; ++y) {
    for (int32_t x = 0; x < size; ++x) {
      masked->mask.writable_pixels()[static_cast<size_t>(y) * size + x] =
          static_cast<uint8_t>((x * 8) & 0xFF);
    }
  }

  /* 穿透组：两个整幅子层；组不透明度 0.8 乘进每个子层（0.9 x 0.8 = 0.72）。 */
  Layer* pass_group =
      add_test_layer(document, "pass-through", COMP_BLEND_NORMAL, 0.8f, comp_rect{0, 0, 0, 0}, 0, true);
  COMP_CHECK(pass_group != nullptr);
  for (int32_t i = 0; i < 2; ++i) {
    Layer* child = add_test_layer(document, "pass-child",
                                  i == 0 ? COMP_BLEND_MULTIPLY : COMP_BLEND_NORMAL, 0.9f,
                                  comp_rect{0, 0, size, size}, pass_group->id, false);
    COMP_CHECK(child != nullptr);
    child->pixels.resize(size, size, COMP_PIXEL_RGBA8);
    fill_test_pattern(&child->pixels, 9 + i);
  }

  /* 隔离组：子树烘焙成单个单元，组自身的混合模式与不透明度作用在单元上。 */
  Layer* isolated_group = add_test_layer(document, "isolated", COMP_BLEND_SCREEN, 0.6f,
                                         comp_rect{0, 0, 0, 0}, 0, true);
  COMP_CHECK(isolated_group != nullptr);
  isolated_group->isolated = true;
  for (int32_t i = 0; i < 2; ++i) {
    Layer* child = add_test_layer(document, "iso-child", COMP_BLEND_NORMAL, 0.85f,
                                  comp_rect{0, 0, size, size}, isolated_group->id, false);
    COMP_CHECK(child != nullptr);
    child->pixels.resize(size, size, COMP_PIXEL_RGBA8);
    fill_test_pattern(&child->pixels, 13 + i);
  }

  /* 剪贴栈：基底 + 紧跟的被剪成员。 */
  Layer* clip_base = add_test_layer(document, "clip-base", COMP_BLEND_NORMAL, 0.9f,
                                    comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(clip_base != nullptr);
  clip_base->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&clip_base->pixels, 21);
  Layer* clipped = add_test_layer(document, "clipped", COMP_BLEND_MULTIPLY, 1.0f,
                                  comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(clipped != nullptr);
  clipped->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&clipped->pixels, 23);
  clipped->clipping_source = clip_base->id;
}

}  // namespace

/*
 * 计划分类：裁剪/蒙版/组/剪贴不再让整篇回落，而是按单元分类。
 *
 * 这里不依赖 GPU：分类与 CPU 烘焙都是平台无关的，所以默认构建也能钉住
 * 「回落粒度已拆细」这件事。额外用「单元序列 + 同一条 CPU 混合公式」重组出
 * 一张图，与 CPU 合成器整篇结果对拍——证明计划没有漏掉或重复任何贡献。
 */
COMP_TEST(rhi_gpu_plan_classifies_units_instead_of_rejecting_documents) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  Engine engine(desc);
  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  build_layered_scene(document, size);

  std::string error;
  GpuPlanStats stats{};
  COMP_CHECK_MSG(build_gpu_plan(document, nullptr, &stats, &error), error.c_str());
  COMP_CHECK_MSG(stats.native_units == 3, "background + two pass-through children upload natively");
  COMP_CHECK_MSG(stats.baked_units == 2, "cropped and masked layers must be baked, not rejected");
  COMP_CHECK_MSG(stats.isolated_groups == 1, "the isolated group is one baked unit");
  COMP_CHECK_MSG(stats.clipping_stacks == 1, "the clipping stack is one baked unit");
  COMP_CHECK_MSG(stats.pass_through_groups == 1, "the pass-through group is expanded");

  std::vector<GpuUnit> units;
  GpuPlanStats built{};
  error.clear();
  COMP_CHECK_MSG(build_gpu_plan(document, &units, &built, &error), error.c_str());
  COMP_CHECK(units.size() == 7u);
  COMP_CHECK(built.native_units == 3 && built.baked_units == 2);

  int32_t baked_count = 0;
  bool saw_pass_through_opacity = false;
  for (const GpuUnit& unit : units) {
    if (unit.is_native()) {
      COMP_CHECK(unit.native_layer != nullptr);
      if (std::abs(unit.opacity - 0.72f) < 1e-4f) saw_pass_through_opacity = true;
      continue;
    }
    ++baked_count;
    COMP_CHECK(unit.native_layer == nullptr);
    COMP_CHECK(!unit.baked.empty());
    COMP_CHECK(unit.baked.width == size && unit.baked.height == size);
    COMP_CHECK(unit.baked.format == COMP_PIXEL_RGBA8);
  }
  COMP_CHECK(baked_count == 4);
  COMP_CHECK_MSG(saw_pass_through_opacity,
                 "pass-through group opacity must multiply into each child unit");

  /*
   * 用计划自己的单元序列重组整篇：原生单元 = 图层像素，烘焙单元 = 预乘贡献；
   * 每个单元都用同一套 blend_buffer_into 叠加。与 CPU 合成器整篇结果对比，
   * 差异只允许来自烘焙的 8 位量化（≤1 LSB）。
   */
  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &error);
  COMP_CHECK_MSG(cpu != nullptr, error.c_str());
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &error), error.c_str());

  ImageBuffer assembled;
  assembled.resize(size, size, COMP_PIXEL_RGBA8);
  assembled.clear_transparent();
  for (const GpuUnit& unit : units) {
    const ImageBuffer& source = unit.is_native() ? unit.native_layer->pixels : unit.baked;
    COMP_CHECK(blend_buffer_into(&assembled, source, unit.blend, unit.opacity));
  }
  std::vector<uint8_t> planned_pixels(static_cast<size_t>(size) * size * 4, 0);
  for (int32_t y = 0; y < size; ++y) {
    std::memcpy(planned_pixels.data() + static_cast<size_t>(y) * size * 4,
                assembled.row(static_cast<size_t>(y)), static_cast<size_t>(size) * 4);
  }
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, planned_pixels, &max_diff, &diff_count, &first_diff);
  const std::string message = "plan units must reproduce the CPU composite (max_diff=" +
                              std::to_string(max_diff) + ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!cpu_pixels.empty() && max_diff <= 1, message.c_str());
}

/*
 * 剩余回落必须具名：调整层（作用于已合成画布）与 HDR 表面仍然整篇回落 CPU。
 * 这两条是「拆细」之后的真实边界，不能悄悄近似。
 */
COMP_TEST(rhi_gpu_plan_names_remaining_fallbacks) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.backend = COMP_BACKEND_CPU;
  Engine engine(desc);
  const int32_t size = 16;

  Document adjusted(&engine, size, size, 72.0f, "sRGB");
  Layer* background =
      add_test_layer(adjusted, "bg", COMP_BLEND_NORMAL, 1.0f, comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(background != nullptr);
  background->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 10, 20, 30, 255);
  Layer* top =
      add_test_layer(adjusted, "top", COMP_BLEND_NORMAL, 1.0f, comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(top != nullptr);
  top->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 40, 50, 60, 200);
  Adjustment exposure{};
  exposure.kind = COMP_ADJUSTMENT_EXPOSURE;
  exposure.settings_json = "{\"exposureSettings\":{\"exposure\":0.2}}";
  top->adjustments.push_back(exposure);

  std::string error;
  COMP_CHECK(!build_gpu_plan(adjusted, nullptr, nullptr, &error));
  COMP_CHECK_MSG(error.find("adjustment") != std::string::npos,
                 "the rejection must name the adjustment layer");

  Document hdr(&engine, size, size, 72.0f, "sRGB");
  Layer* hdr_layer =
      add_test_layer(hdr, "hdr", COMP_BLEND_NORMAL, 1.0f, comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(hdr_layer != nullptr);
  hdr_layer->pixels.resize(size, size, COMP_PIXEL_RGBA16F);
  error.clear();
  COMP_CHECK(!build_gpu_plan(hdr, nullptr, nullptr, &error));
  COMP_CHECK_MSG(error.find("HDR") != std::string::npos,
                 "the rejection must name the half-float surface");
}

/*
 * S17 GPU 一致性：裁剪 + 蒙版图层（旧闸门整篇回落的形态）在真实设备上必须
 * 走 GPU 且与 CPU 基准一致（±1 LSB）。未编入 D3D12 / 无设备 → 跳过。
 */
COMP_TEST(rhi_gpu_renders_cropped_and_masked_layers_like_cpu) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::string error;
  std::unique_ptr<RenderBackend> gpu = make_backend(&engine, COMP_BACKEND_D3D12, &error);
  if (gpu == nullptr || gpu->kind() != COMP_BACKEND_D3D12 || !gpu->usable()) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  Layer* background =
      add_test_layer(document, "bg", COMP_BLEND_NORMAL, 1.0f, comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(background != nullptr);
  background->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 24, 96, 168, 255);

  Layer* cropped =
      add_test_layer(document, "crop", COMP_BLEND_MULTIPLY, 0.75f, comp_rect{7, 9, 8, 6}, 0, false);
  COMP_CHECK(cropped != nullptr);
  cropped->pixels.resize(8, 6, COMP_PIXEL_RGBA8);
  fill_test_pattern(&cropped->pixels, 3);

  Layer* masked = add_test_layer(document, "masked", COMP_BLEND_SCREEN, 0.8f,
                                 comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(masked != nullptr);
  masked->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&masked->pixels, 5);
  /* 蒙版比画布小：蒙版坐标要经文档逆映射采样，烘焙路径必须沿用 CPU 的同一套。 */
  masked->mask.resize(16, 16, 255);
  for (int32_t y = 0; y < 16; ++y) {
    for (int32_t x = 0; x < 16; ++x) {
      masked->mask.writable_pixels()[static_cast<size_t>(y) * 16 + x] =
          static_cast<uint8_t>((x * 16) & 0xFF);
    }
  }

  std::string why;
  COMP_CHECK_MSG(gpu->can_render_document(document, &why), why.c_str());

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &error);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  std::vector<uint8_t> gpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  why.clear();
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &gpu_pixels, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(),
                 "cropped/masked layers must reach the device (baked unit), not fall back");

  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, gpu_pixels, &max_diff, &diff_count, &first_diff);
  /*
   * 烘焙单元与原生单元的口径不同：原生单元逐位上传像素，GPU 混合结果与 CPU 基准
   * 只差舍入（≤1 LSB）；烘焙单元要先把「蒙版 x 不透明度」乘进预乘色并量化成 8 位，
   * 再做一次混合，量化误差经 MULTIPLY/SCREEN 的颜色乘积项最多放大到 2 LSB。
   * 实测（32x32、裁剪 MULTIPLY 0.75 + 蒙版 SCREEN 0.8）最大差 2、约 13% 字节不同。
   * 这是「少一次上传 vs 一次量化」的显式取舍，不是画面错误，故这里按 2 断言。
   */
  const std::string message = "crop+mask GPU parity (max_diff=" + std::to_string(max_diff) +
                              ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!gpu_pixels.empty() && max_diff <= 2, message.c_str());
}

/*
 * S17 GPU 一致性：隔离组 / 穿透组 / 剪贴栈在真实设备上与 CPU 基准一致（±1 LSB）。
 * 未编入 D3D12 / 无设备 → 跳过。
 */
COMP_TEST(rhi_gpu_renders_group_and_clipping_units_like_cpu) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::string error;
  std::unique_ptr<RenderBackend> gpu = make_backend(&engine, COMP_BACKEND_D3D12, &error);
  if (gpu == nullptr || gpu->kind() != COMP_BACKEND_D3D12 || !gpu->usable()) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  Layer* background =
      add_test_layer(document, "bg", COMP_BLEND_NORMAL, 1.0f, comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(background != nullptr);
  background->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 24, 96, 168, 255);

  Layer* pass_group =
      add_test_layer(document, "pass-through", COMP_BLEND_NORMAL, 0.8f, comp_rect{0, 0, 0, 0}, 0, true);
  COMP_CHECK(pass_group != nullptr);
  for (int32_t i = 0; i < 2; ++i) {
    Layer* child = add_test_layer(document, "pass-child",
                                  i == 0 ? COMP_BLEND_MULTIPLY : COMP_BLEND_NORMAL, 0.9f,
                                  comp_rect{0, 0, size, size}, pass_group->id, false);
    COMP_CHECK(child != nullptr);
    child->pixels.resize(size, size, COMP_PIXEL_RGBA8);
    fill_test_pattern(&child->pixels, 9 + i);
  }

  Layer* isolated_group = add_test_layer(document, "isolated", COMP_BLEND_SCREEN, 0.6f,
                                         comp_rect{0, 0, 0, 0}, 0, true);
  COMP_CHECK(isolated_group != nullptr);
  isolated_group->isolated = true;
  for (int32_t i = 0; i < 2; ++i) {
    Layer* child = add_test_layer(document, "iso-child", COMP_BLEND_NORMAL, 0.85f,
                                  comp_rect{0, 0, size, size}, isolated_group->id, false);
    COMP_CHECK(child != nullptr);
    child->pixels.resize(size, size, COMP_PIXEL_RGBA8);
    fill_test_pattern(&child->pixels, 13 + i);
  }

  Layer* clip_base = add_test_layer(document, "clip-base", COMP_BLEND_NORMAL, 0.9f,
                                    comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(clip_base != nullptr);
  clip_base->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&clip_base->pixels, 21);
  Layer* clipped = add_test_layer(document, "clipped", COMP_BLEND_MULTIPLY, 1.0f,
                                  comp_rect{0, 0, size, size}, 0, false);
  COMP_CHECK(clipped != nullptr);
  clipped->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&clipped->pixels, 23);
  clipped->clipping_source = clip_base->id;

  std::string why;
  COMP_CHECK_MSG(gpu->can_render_document(document, &why), why.c_str());

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &error);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  std::vector<uint8_t> gpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  why.clear();
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &gpu_pixels, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(),
                 "group/clipping units must reach the device, not fall back to CPU");

  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, gpu_pixels, &max_diff, &diff_count, &first_diff);
  const std::string message = "group/clipping GPU parity (max_diff=" +
                              std::to_string(max_diff) + ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!gpu_pixels.empty() && max_diff <= 1, message.c_str());
}

/* ==================================================================
 * S19 / R2：D3D12 持久逐层纹理 + texture_uploads() 计数
 * ================================================================== */

namespace {

/* 追加一个整幅画布 RGBA8 的「原生单元」图层（gpu_plan 的 native 分支）。 */
comp_layer_id add_native_layer(Document& document, int32_t size, comp_blend_mode blend,
                               float opacity, int32_t seed) {
  Layer* layer =
      add_test_layer(document, "native", blend, opacity, comp_rect{0, 0, size, size}, 0, false);
  if (layer == nullptr) return 0;
  layer->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&layer->pixels, seed);
  return layer->id;
}

/*
 * 4 个原生单元（1 个不透明底 + 3 个混合层），与 --present-bench 的 1024² 场景同形态。
 * 全部是「整幅画布 + 无蒙版/效果/变换」→ 每个单元一张逐层纹理，预期一次渲染 4 次上传。
 * seed_offset 用来让两个文档的像素内容不同（跨文档缓存隔离用例要它）。
 */
bool build_native_scene(Document& document, int32_t size, std::vector<comp_layer_id>* ids,
                        int32_t seed_offset = 0) {
  if (ids == nullptr) return false;
  ids->clear();
  Layer* background = add_test_layer(document, "bg", COMP_BLEND_NORMAL, 1.0f,
                                     comp_rect{0, 0, size, size}, 0, false);
  if (background == nullptr) return false;
  background->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 24, 96, 168, 255);
  ids->push_back(background->id);
  const comp_blend_mode blends[3] = {COMP_BLEND_NORMAL, COMP_BLEND_MULTIPLY, COMP_BLEND_SCREEN};
  const float opacities[3] = {1.0f, 0.75f, 0.5f};
  for (int32_t i = 0; i < 3; ++i) {
    const comp_layer_id id =
        add_native_layer(document, size, blends[i], opacities[i], seed_offset + 1 + i);
    if (id == 0) return false;
    ids->push_back(id);
  }
  return true;
}

/* 当前文档里原生单元的数量 = 一次全量重传应有的上传次数。 */
size_t native_unit_count(const Document& document) {
  std::vector<GpuUnit> units;
  if (!build_gpu_plan(document, &units, nullptr, nullptr)) return 0;
  size_t natives = 0;
  for (const GpuUnit& unit : units) {
    if (unit.is_native()) ++natives;
  }
  return natives;
}

/* 打开 D3D12 后端；不可用（未编入 / 无设备）时返回 nullptr，用例据此跳过。 */
std::unique_ptr<RenderBackend> open_gpu_backend(Engine* engine) {
  std::string error;
  std::unique_ptr<RenderBackend> gpu = make_backend(engine, COMP_BACKEND_D3D12, &error);
  if (gpu == nullptr || gpu->kind() != COMP_BACKEND_D3D12 || !gpu->usable()) return nullptr;
  return gpu;
}

}  // namespace

/*
 * R2 验收 1（设计稿 §4.2）：文档没变时连渲两帧，第二帧不得再上传任何逐层纹理，
 * 且两帧逐字节一致、与 CPU 基准差 ≤1 LSB（缓存没有把内容改坏）。
 */
COMP_TEST(rhi_gpu_layer_textures_are_reused_across_identical_frames) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return; /* 本构建没有 D3D12 / 本机没有可用设备 */

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_native_scene(document, size, &ids), "build four native units");
  COMP_CHECK(ids.size() == 4u);
  COMP_CHECK_MSG(native_unit_count(document) == 4u,
                 "the scene must consist of native units only (one texture each)");

  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  std::string why;
  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  const unsigned long long before = d3d12->texture_uploads();
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &first, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the first frame must reach the device");
  const unsigned long long after_first = d3d12->texture_uploads();
  COMP_CHECK_MSG(after_first - before == ids.size(),
                 "the first frame must upload exactly one texture per native unit");

  why.clear();
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &second, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the second frame must still reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() == after_first,
                 "an unchanged document must not upload any layer texture again");
  COMP_CHECK_MSG(first == second, "two identical frames must be byte-identical");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, second, &max_diff, &diff_count, &first_diff);
  const std::string message = "cached-frame CPU parity (max_diff=" + std::to_string(max_diff) +
                              ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!second.empty() && max_diff <= 1, message.c_str());
}

/*
 * R2 验收 2（设计稿 §4.2）：只改一层并标脏，重渲时必须**恰好 +1** 次上传，
 * 且画面反映改动（不是「省了上传但贴旧图」）。
 */
COMP_TEST(rhi_gpu_layer_texture_cache_uploads_only_changed_layers) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_native_scene(document, size, &ids), "build four native units");

  std::string why;
  std::vector<uint8_t> warm;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &warm, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the warm-up frame must reach the device");
  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long after_warm = d3d12->texture_uploads();

  Layer* changed = document.find(ids[2]);
  COMP_CHECK_MSG(changed != nullptr, "the edited layer must exist");
  if (changed == nullptr) return;
  /* 直接改像素必须显式标脏（S18 的修订号才是缓存键）；颜色分量 ≤ alpha，保持预乘合法。 */
  changed->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 100, 20, 60, 160);
  COMP_CHECK_MSG(document.touch_layer(ids[2]) != 0, "touch_layer must allocate a new revision");

  why.clear();
  std::vector<uint8_t> edited;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &edited, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the frame after the edit must reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() - after_warm == 1u,
                 "changing one layer must upload exactly that one layer texture");
  COMP_CHECK_MSG(edited != warm, "the new frame must reflect the edited layer content");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, edited, &max_diff, &diff_count, &first_diff);
  const std::string message = "edited-frame CPU parity (max_diff=" + std::to_string(max_diff) +
                              ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!edited.empty() && max_diff <= 1, message.c_str());
}

/*
 * R2 验收 3（设计稿 §4.2）：画布尺寸一变，逐层纹理的尺寸/内容键全部作废，
 * 下一帧必须按当前原生单元数整份重传（而不是拿旧尺寸的纹理硬贴）。
 */
COMP_TEST(rhi_gpu_layer_texture_cache_rebuilds_after_canvas_resize) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  const int32_t resized = size * 2;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_native_scene(document, size, &ids), "build four native units");

  std::string why;
  std::vector<uint8_t> initial;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &initial, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the pre-resize frame must reach the device");
  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long after_warm = d3d12->texture_uploads();

  std::string resize_error;
  COMP_CHECK_MSG(document.resize_image(comp_size{resized, resized}, &resize_error),
                 resize_error.c_str());
  const size_t natives = native_unit_count(document);
  COMP_CHECK_MSG(natives == 4u, "full-canvas layers must stay native after the resize");

  why.clear();
  std::vector<uint8_t> rebuilt;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, resized, &rebuilt, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the post-resize frame must reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() - after_warm == natives,
                 "a canvas size change must rebuild every layer texture");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, resized, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, rebuilt, &max_diff, &diff_count, &first_diff);
  const std::string message = "post-resize CPU parity (max_diff=" + std::to_string(max_diff) +
                              ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!rebuilt.empty() && max_diff <= 1, message.c_str());
}

/*
 * R2 验收 4（设计稿 §4.2）：活跃事务期间整段旁路缓存——每帧都重传，绝不采信
 * 事务前的缓存，也绝不把中间态写进缓存；提交后只有真正改过的层需要重传一次。
 */
COMP_TEST(rhi_gpu_layer_texture_cache_is_bypassed_inside_a_transaction) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_native_scene(document, size, &ids), "build four native units");

  std::string why;
  std::vector<uint8_t> warm;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &warm, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the pre-transaction frame must reach the device");
  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long after_warm = d3d12->texture_uploads();
  const size_t natives = native_unit_count(document);
  COMP_CHECK(natives == 4u);

  std::string txn_error;
  COMP_CHECK_MSG(document.txn_begin("R2 cache bypass", &txn_error) == COMP_OK, txn_error.c_str());
  COMP_CHECK_MSG(document.txn_active(), "the transaction must be active");
  Layer* edited = document.find(ids[1]);
  COMP_CHECK_MSG(edited != nullptr, "the edited layer must exist");
  if (edited == nullptr) return;
  edited->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 40, 120, 200, 220);
  document.touch_layer(ids[1]);

  for (int round = 0; round < 2; ++round) {
    const unsigned long long before_round = d3d12->texture_uploads();
    why.clear();
    std::vector<uint8_t> in_txn;
    COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &in_txn, &why), why.c_str());
    COMP_CHECK_MSG(gpu->last_render_offloaded(),
                   "a frame inside a transaction must still reach the device");
    COMP_CHECK_MSG(d3d12->texture_uploads() - before_round == natives,
                   "transaction frames must bypass the layer texture cache entirely");
  }
  COMP_CHECK_MSG(d3d12->texture_uploads() - after_warm == natives * 2u,
                 "two transaction frames must have uploaded every native unit twice");

  COMP_CHECK_MSG(document.txn_commit(&txn_error) == COMP_OK, txn_error.c_str());
  COMP_CHECK(!document.txn_active());

  const unsigned long long after_commit = d3d12->texture_uploads();
  why.clear();
  std::vector<uint8_t> committed;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &committed, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the post-commit frame must reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() - after_commit == 1u,
                 "after the commit only the touched layer may be uploaded again");
  COMP_CHECK_MSG(committed != warm, "the committed frame must reflect the edited layer");

  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(warm, committed, &max_diff, &diff_count, &first_diff);
  COMP_CHECK_MSG(max_diff > 0, "the committed frame must differ from the pre-transaction one");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  int parity_max_diff = 255;
  size_t parity_diffs = 0;
  size_t parity_first_diff = 0;
  compare_pixels(cpu_pixels, committed, &parity_max_diff, &parity_diffs, &parity_first_diff);
  const std::string message = "post-commit CPU parity (max_diff=" +
                              std::to_string(parity_max_diff) + ", diffs=" +
                              std::to_string(parity_diffs) +
                              ", first_byte=" + std::to_string(parity_first_diff) + ")";
  COMP_CHECK_MSG(!committed.empty() && parity_max_diff <= 1, message.c_str());
}

/*
 * R2 补充用例：逐层纹理缓存不得跨文档串用。
 *
 * 两个文档的图层 id 与内容修订号**完全相同**（都从新建状态开始发号），但像素不同。
 * 缓存键若只有 (id, revision)，同一后端切文档就会把 A 的纹理贴到 B 上——静默错图。
 * `Document::instance_serial()` 就是为此加的：实例号变 → 整体丢弃。
 */
COMP_TEST(rhi_gpu_layer_texture_cache_is_not_shared_between_documents) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  Document first(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> first_ids;
  COMP_CHECK_MSG(build_native_scene(first, size, &first_ids, 0), "build the first document");
  std::string why;
  std::vector<uint8_t> first_pixels;
  COMP_CHECK_MSG(render_to_buffer(*gpu, first, size, &first_pixels, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the first document must reach the device");
  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long after_first = d3d12->texture_uploads();

  Document second(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> second_ids;
  COMP_CHECK_MSG(build_native_scene(second, size, &second_ids, 10), "build the second document");
  COMP_CHECK_MSG(first_ids == second_ids, "both documents must use the same layer ids");
  /* 前提确认：两个文档的 (id, 修订号) 完全一致，不带文档号就必然撞键。 */
  for (const comp_layer_id id : first_ids) {
    const Layer* a = first.find(id);
    const Layer* b = second.find(id);
    COMP_CHECK_MSG(a != nullptr && b != nullptr, "both layers must exist");
    if (a == nullptr || b == nullptr) return;
    COMP_CHECK_MSG(a->content_revision == b->content_revision,
                   "the two documents must have colliding revisions for this test to mean anything");
  }

  why.clear();
  std::vector<uint8_t> second_pixels;
  COMP_CHECK_MSG(render_to_buffer(*gpu, second, size, &second_pixels, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the second document must reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() - after_first == second_ids.size(),
                 "switching documents must discard the cache and upload every layer again");
  COMP_CHECK_MSG(second_pixels != first_pixels,
                 "the second document must not show the first document's textures");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, second, size, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, second_pixels, &max_diff, &diff_count, &first_diff);
  const std::string message = "second-document CPU parity (max_diff=" +
                              std::to_string(max_diff) + ", diffs=" +
                              std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!second_pixels.empty() && max_diff <= 1, message.c_str());
}

/* ==================================================================
 * S20 / v2：烘焙单元缓存（CPU 表面缓存 + D3D12 烘焙纹理缓存）
 * ================================================================== */

namespace {

/* 追加一个「整幅画布 + 蒙版」的图层：蒙版让 is_native_layer 为假 → 烘焙单元。 */
comp_layer_id add_baked_layer(Document& document, int32_t size, comp_blend_mode blend,
                              float opacity, int32_t seed) {
  Layer* layer =
      add_test_layer(document, "baked", blend, opacity, comp_rect{0, 0, size, size}, 0, false);
  if (layer == nullptr) return 0;
  layer->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&layer->pixels, seed);
  layer->mask.resize(size, size, 255);
  for (int32_t y = 0; y < size; ++y) {
    for (int32_t x = 0; x < size; ++x) {
      layer->mask.writable_pixels()[static_cast<size_t>(y) * size + x] =
          static_cast<uint8_t>((x * 8 + seed * 3) & 0xFF);
    }
  }
  return layer->id;
}

/* 1 个原生底 + 2 个烘焙蒙版层：一次全量渲染 = 3 次源纹理上传。 */
bool build_baked_scene(Document& document, int32_t size, std::vector<comp_layer_id>* ids,
                       int32_t seed_offset = 0) {
  if (ids == nullptr) return false;
  ids->clear();
  Layer* background = add_test_layer(document, "bg", COMP_BLEND_NORMAL, 1.0f,
                                     comp_rect{0, 0, size, size}, 0, false);
  if (background == nullptr) return false;
  background->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 24, 96, 168, 255);
  ids->push_back(background->id);
  const comp_blend_mode blends[2] = {COMP_BLEND_NORMAL, COMP_BLEND_MULTIPLY};
  const float opacities[2] = {1.0f, 0.75f};
  for (int32_t i = 0; i < 2; ++i) {
    const comp_layer_id id =
        add_baked_layer(document, size, blends[i], opacities[i], seed_offset + 3 + i);
    if (id == 0) return false;
    ids->push_back(id);
  }
  return true;
}

/* 逐行拼成一维字节序列（忽略 stride 差异，只比较有效像素）。 */
std::vector<uint8_t> buffer_bytes(const ImageBuffer& buffer) {
  std::vector<uint8_t> out;
  if (buffer.empty()) return out;
  out.reserve(buffer.byte_size());
  const size_t row_bytes = static_cast<size_t>(buffer.width) * 4u;
  for (int32_t y = 0; y < buffer.height; ++y) {
    const uint8_t* row = buffer.row(static_cast<size_t>(y));
    out.insert(out.end(), row, row + row_bytes);
  }
  return out;
}

std::vector<uint64_t> baked_keys(const std::vector<GpuUnit>& units) {
  std::vector<uint64_t> keys;
  for (const GpuUnit& unit : units) {
    if (!unit.is_native()) keys.push_back(unit.cache_key);
  }
  return keys;
}

}  // namespace

/*
 * v2 验收 1：文档没变时重建计划，每个烘焙单元都必须命中 CPU 烘焙缓存，
 * 且缓存里取回的像素与首次烘焙逐字节一致（缓存没有把内容改坏）。
 */
COMP_TEST(gpu_bake_cache_reuses_surfaces_across_identical_plans) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_baked_scene(document, size, &ids), "build the baked scene");

  GpuBakeCache cache;
  std::string error;
  std::vector<GpuUnit> first;
  GpuPlanStats stats{};
  COMP_CHECK_MSG(build_gpu_plan(document, &first, &stats, &error, &cache), error.c_str());
  COMP_CHECK_MSG(stats.native_units == 1 && stats.baked_units == 2,
                 "the scene must contain one native and two baked units");
  COMP_CHECK_MSG(cache.hits() == 0 && cache.misses() == 2,
                 "the first plan must miss the cache once per baked unit");
  COMP_CHECK_MSG(cache.entries() == 2u, "both baked surfaces must be stored");
  for (const GpuUnit& unit : first) {
    if (unit.is_native()) {
      COMP_CHECK_MSG(unit.cache_key == 0, "native units carry no bake key");
    } else {
      COMP_CHECK_MSG(unit.cache_key != 0, "baked units must carry a cache key");
      COMP_CHECK(!unit.baked.empty());
    }
  }

  std::vector<GpuUnit> second;
  error.clear();
  COMP_CHECK_MSG(build_gpu_plan(document, &second, &stats, &error, &cache), error.c_str());
  COMP_CHECK_MSG(cache.hits() == 2 && cache.misses() == 2,
                 "an unchanged plan must hit the bake cache for every baked unit");
  COMP_CHECK_MSG(second.size() == first.size(), "the plan shape must be unchanged");
  COMP_CHECK_MSG(baked_keys(first) == baked_keys(second), "cache keys must be stable");
  for (size_t i = 0; i < first.size(); ++i) {
    COMP_CHECK_MSG(buffer_bytes(first[i].baked) == buffer_bytes(second[i].baked),
                   "a cache hit must return pixel-identical surfaces");
  }
}

/*
 * v2 验收 2：只改一层并标脏，重建计划时恰好只有该单元未命中；其余单元命中，
 * 且取回像素逐字节不变。
 */
COMP_TEST(gpu_bake_cache_invalidates_only_the_changed_layer) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_baked_scene(document, size, &ids), "build the baked scene");
  COMP_CHECK(ids.size() == 3u);

  GpuBakeCache cache;
  std::string error;
  std::vector<GpuUnit> before;
  COMP_CHECK_MSG(build_gpu_plan(document, &before, nullptr, &error, &cache), error.c_str());
  COMP_CHECK(cache.misses() == 2);

  Layer* changed = document.find(ids[2]);
  COMP_CHECK_MSG(changed != nullptr, "the edited layer must exist");
  if (changed == nullptr) return;
  changed->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 100, 20, 60, 160);
  COMP_CHECK_MSG(document.touch_layer(ids[2]) != 0, "touch_layer must allocate a new revision");

  error.clear();
  std::vector<GpuUnit> after;
  COMP_CHECK_MSG(build_gpu_plan(document, &after, nullptr, &error, &cache), error.c_str());
  COMP_CHECK_MSG(cache.hits() == 1 && cache.misses() == 3,
                 "only the changed baked layer may miss the bake cache");

  const std::vector<uint64_t> before_keys = baked_keys(before);
  const std::vector<uint64_t> after_keys = baked_keys(after);
  COMP_CHECK(before_keys.size() == 2u && after_keys.size() == 2u);
  if (before_keys.size() != 2u || after_keys.size() != 2u) return;
  size_t changed_units = 0;
  for (size_t i = 0; i < before_keys.size(); ++i) {
    if (before_keys[i] != after_keys[i]) ++changed_units;
  }
  COMP_CHECK_MSG(changed_units == 1u, "exactly one bake key may change");
  /* 未变的那一层：缓存取回像素必须与首次烘焙完全一致。 */
  size_t baked_index = 0;
  for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
    /* 计划形状不变：同一下标在两轮里必须是同一类单元。 */
    COMP_CHECK(before[i].is_native() == after[i].is_native());
    if (before[i].is_native()) continue;
    COMP_CHECK(baked_index < before_keys.size() && baked_index < after_keys.size());
    if (baked_index >= before_keys.size() || baked_index >= after_keys.size()) return;
    if (before_keys[baked_index] == after_keys[baked_index]) {
      COMP_CHECK_MSG(buffer_bytes(before[i].baked) == buffer_bytes(after[i].baked),
                     "the untouched baked unit must come back byte-identical");
    }
    ++baked_index;
  }
  COMP_CHECK(baked_index == 2u);
}

/*
 * v2 验收 3：不改任何图层内容、只重排组内顺序（S18 明确不给结构变更发修订号），
 * 子树键仍必须变化——否则缓存会把旧顺序的画面留在屏上。
 */
COMP_TEST(gpu_bake_cache_invalidates_when_group_structure_changes) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");

  Layer* group = add_test_layer(document, "isolated", COMP_BLEND_NORMAL, 1.0f,
                                comp_rect{0, 0, 0, 0}, 0, true);
  COMP_CHECK(group != nullptr);
  if (group == nullptr) return;
  group->isolated = true;
  Layer* first_child = add_test_layer(document, "child-a", COMP_BLEND_NORMAL, 1.0f,
                                      comp_rect{0, 0, size, size}, group->id, false);
  Layer* second_child = add_test_layer(document, "child-b", COMP_BLEND_NORMAL, 1.0f,
                                       comp_rect{0, 0, size, size}, group->id, false);
  COMP_CHECK(first_child != nullptr && second_child != nullptr);
  if (first_child == nullptr || second_child == nullptr) return;
  first_child->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&first_child->pixels, 31);
  second_child->pixels.resize(size, size, COMP_PIXEL_RGBA8);
  fill_test_pattern(&second_child->pixels, 41);

  GpuBakeCache cache;
  std::string error;
  std::vector<GpuUnit> before;
  GpuPlanStats stats{};
  COMP_CHECK_MSG(build_gpu_plan(document, &before, &stats, &error, &cache), error.c_str());
  COMP_CHECK(stats.isolated_groups == 1);
  COMP_CHECK(before.size() == 1u);
  COMP_CHECK(cache.hits() == 0 && cache.misses() == 1);
  const uint64_t key_before = before[0].cache_key;
  COMP_CHECK(key_before != 0);

  /*
   * reorder_layer 会 erase/insert state_.layers，先前拿到的 Layer* 随即失效；
   * 之后一律按 id 重新查，避免读悬垂指针。
   */
  const comp_layer_id child_a = first_child->id;
  const comp_layer_id child_b = second_child->id;
  const uint64_t revision_a = first_child->content_revision;
  const uint64_t revision_b = second_child->content_revision;
  COMP_CHECK_MSG(document.reorder_layer(child_b, child_a), "reordering siblings must succeed");
  const Layer* moved_a = document.find(child_a);
  const Layer* moved_b = document.find(child_b);
  COMP_CHECK(moved_a != nullptr && moved_b != nullptr);
  if (moved_a == nullptr || moved_b == nullptr) return;
  COMP_CHECK_MSG(moved_a->content_revision == revision_a &&
                     moved_b->content_revision == revision_b,
                 "reorder must not bump layer revisions (this is the point of the structure key)");

  error.clear();
  std::vector<GpuUnit> after;
  COMP_CHECK_MSG(build_gpu_plan(document, &after, nullptr, &error, &cache), error.c_str());
  COMP_CHECK(after.size() == 1u);
  COMP_CHECK_MSG(after[0].cache_key != key_before,
                 "a group structure change must change the subtree bake key");
  COMP_CHECK_MSG(cache.hits() == 0 && cache.misses() == 2,
                 "the reordered subtree must be rebaked, not served from the cache");
}

/*
 * v2 补充：两个文档的图层 id 与修订号完全相同、像素不同；同一个缓存不得把
 * A 的烘焙表面供给 B（键里必须带文档实例号）。
 */
COMP_TEST(gpu_bake_cache_is_not_shared_between_documents) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  const int32_t size = 32;
  Document first(&engine, size, size, 72.0f, "sRGB");
  Document second(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> first_ids;
  std::vector<comp_layer_id> second_ids;
  COMP_CHECK_MSG(build_baked_scene(first, size, &first_ids, 0), "build the first document");
  COMP_CHECK_MSG(build_baked_scene(second, size, &second_ids, 50), "build the second document");
  COMP_CHECK_MSG(first_ids == second_ids, "both documents must use the same layer ids");
  for (const comp_layer_id id : first_ids) {
    const Layer* a = first.find(id);
    const Layer* b = second.find(id);
    COMP_CHECK_MSG(a != nullptr && b != nullptr, "both layers must exist");
    if (a == nullptr || b == nullptr) return;
    COMP_CHECK_MSG(a->content_revision == b->content_revision,
                   "the two documents must have colliding revisions for this test to mean anything");
  }

  GpuBakeCache cache;
  std::string error;
  std::vector<GpuUnit> first_units;
  COMP_CHECK_MSG(build_gpu_plan(first, &first_units, nullptr, &error, &cache), error.c_str());
  COMP_CHECK(cache.misses() == 2);

  error.clear();
  std::vector<GpuUnit> second_units;
  COMP_CHECK_MSG(build_gpu_plan(second, &second_units, nullptr, &error, &cache), error.c_str());
  COMP_CHECK_MSG(cache.hits() == 0 && cache.misses() == 4,
                 "switching documents must miss the bake cache for every baked unit");
  const std::vector<uint64_t> first_keys = baked_keys(first_units);
  const std::vector<uint64_t> second_keys = baked_keys(second_units);
  COMP_CHECK(first_keys.size() == 2u && second_keys.size() == 2u);
  for (size_t i = 0; i < first_keys.size() && i < second_keys.size(); ++i) {
    COMP_CHECK_MSG(first_keys[i] != second_keys[i],
                   "the document instance serial must change every bake key");
  }
}

/*
 * v2 补充：单个表面比预算还大时永不入缓存（否则缓存永远超预算），
 * 每帧都重新烘焙但结果仍然正确。
 */
COMP_TEST(gpu_bake_cache_respects_budget) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_baked_scene(document, size, &ids), "build the baked scene");

  GpuBakeCache cache(1); /* 1 字节预算：任何画布表面都放不下 */
  std::string error;
  for (int round = 0; round < 2; ++round) {
    std::vector<GpuUnit> units;
    error.clear();
    COMP_CHECK_MSG(build_gpu_plan(document, &units, nullptr, &error, &cache), error.c_str());
  }
  COMP_CHECK_MSG(cache.entries() == 0u, "oversized surfaces must never be stored");
  COMP_CHECK_MSG(cache.bytes() == 0u, "the cache must not retain any bytes");
  COMP_CHECK_MSG(cache.hits() == 0 && cache.misses() == 4,
                 "an unbudgetable surface must miss on every frame");
}

/* ------------------------------------------------------------------
 * D3D12：烘焙纹理缓存（真机；未编入 / 无设备 → 跳过）
 * ------------------------------------------------------------------ */

/*
 * v2 验收 4：文档没变时连渲两帧，第二帧不得再上传任何源纹理（原生 + 烘焙），
 * 且两帧逐字节一致、与 CPU 基准差 ≤2 LSB（烘焙量化口径同 S17）。
 */
COMP_TEST(rhi_gpu_baked_textures_are_reused_across_identical_frames) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_baked_scene(document, size, &ids), "build the baked scene");

  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long uploads_before = d3d12->texture_uploads();
  const unsigned long long bake_misses_before = d3d12->bake_cache_misses();
  std::string why;
  std::vector<uint8_t> first;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &first, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the first frame must reach the device");
  const unsigned long long uploads_after_first = d3d12->texture_uploads();
  COMP_CHECK_MSG(uploads_after_first - uploads_before == 3u,
                 "the first frame must create one texture per unit (1 native + 2 baked)");
  COMP_CHECK_MSG(d3d12->bake_cache_misses() - bake_misses_before == 2u,
                 "the first plan must bake both baked units");

  const unsigned long long bake_hits_before = d3d12->bake_cache_hits();
  why.clear();
  std::vector<uint8_t> second;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &second, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the second frame must still reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() == uploads_after_first,
                 "an unchanged document must not upload any source texture again");
  COMP_CHECK_MSG(d3d12->bake_cache_hits() - bake_hits_before == 2u,
                 "the second plan must hit the CPU bake cache for both baked units");
  COMP_CHECK_MSG(first == second, "two identical frames must be byte-identical");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, second, &max_diff, &diff_count, &first_diff);
  const std::string message = "baked-cache CPU parity (max_diff=" + std::to_string(max_diff) +
                              ", diffs=" + std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!second.empty() && max_diff <= 2, message.c_str());
}

/*
 * v2 验收 5：只改一个烘焙层，下一帧恰好 +1 次上传（该层的烘焙纹理），
 * 未变的烘焙层与原生层都不得重传；画面反映改动且与 CPU 一致。
 */
COMP_TEST(rhi_gpu_baked_texture_cache_uploads_only_changed_units) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_baked_scene(document, size, &ids), "build the baked scene");

  std::string why;
  std::vector<uint8_t> warm;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &warm, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the warm-up frame must reach the device");
  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long uploads_after_warm = d3d12->texture_uploads();
  const unsigned long long hits_after_warm = d3d12->bake_cache_hits();
  const unsigned long long misses_after_warm = d3d12->bake_cache_misses();

  Layer* changed = document.find(ids[2]);
  COMP_CHECK_MSG(changed != nullptr, "the edited layer must exist");
  if (changed == nullptr) return;
  changed->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 100, 20, 60, 160);
  COMP_CHECK_MSG(document.touch_layer(ids[2]) != 0, "touch_layer must allocate a new revision");

  why.clear();
  std::vector<uint8_t> edited;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &edited, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the edited frame must reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() - uploads_after_warm == 1u,
                 "only the changed baked unit may be uploaded again");
  COMP_CHECK_MSG(d3d12->bake_cache_hits() - hits_after_warm == 1u,
                 "the untouched baked unit must hit the CPU bake cache");
  COMP_CHECK_MSG(d3d12->bake_cache_misses() - misses_after_warm == 1u,
                 "the changed baked unit must be rebaked");
  COMP_CHECK_MSG(edited != warm, "the new frame must reflect the edited layer content");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, edited, &max_diff, &diff_count, &first_diff);
  const std::string message = "edited baked-frame CPU parity (max_diff=" +
                              std::to_string(max_diff) + ", diffs=" +
                              std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!edited.empty() && max_diff <= 2, message.c_str());
}

/*
 * v2 验收 6：活跃事务期间烘焙缓存整段旁路——每帧按单元数上传，且不读写
 * CPU 烘焙缓存；提交后只有真正改过的烘焙层重传一次。
 */
COMP_TEST(rhi_gpu_baked_texture_cache_is_bypassed_inside_a_transaction) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-tests";
  desc.version = "1.0.0";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  Engine engine(desc);
  std::unique_ptr<RenderBackend> gpu = open_gpu_backend(&engine);
  if (gpu == nullptr) return;

  const int32_t size = 32;
  Document document(&engine, size, size, 72.0f, "sRGB");
  std::vector<comp_layer_id> ids;
  COMP_CHECK_MSG(build_baked_scene(document, size, &ids), "build the baked scene");

  std::string why;
  std::vector<uint8_t> warm;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &warm, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the pre-transaction frame must reach the device");
  auto* d3d12 = static_cast<D3d12RenderBackend*>(gpu.get());
  const unsigned long long uploads_after_warm = d3d12->texture_uploads();
  const unsigned long long hits_after_warm = d3d12->bake_cache_hits();
  const unsigned long long misses_after_warm = d3d12->bake_cache_misses();

  std::string txn_error;
  COMP_CHECK_MSG(document.txn_begin("v2 bake cache bypass", &txn_error) == COMP_OK,
                 txn_error.c_str());
  COMP_CHECK_MSG(document.txn_active(), "the transaction must be active");
  Layer* edited = document.find(ids[2]);
  COMP_CHECK_MSG(edited != nullptr, "the edited layer must exist");
  if (edited == nullptr) return;
  edited->pixels = ImageBuffer::solid(size, size, COMP_PIXEL_RGBA8, 40, 120, 200, 220);
  document.touch_layer(ids[2]);

  for (int round = 0; round < 2; ++round) {
    const unsigned long long before_round = d3d12->texture_uploads();
    why.clear();
    std::vector<uint8_t> in_txn;
    COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &in_txn, &why), why.c_str());
    COMP_CHECK_MSG(gpu->last_render_offloaded(),
                   "a frame inside a transaction must still reach the device");
    COMP_CHECK_MSG(d3d12->texture_uploads() - before_round == 3u,
                   "transaction frames must bypass both texture caches entirely");
  }
  COMP_CHECK_MSG(d3d12->texture_uploads() - uploads_after_warm == 6u,
                 "two transaction frames must upload every unit twice");
  COMP_CHECK_MSG(d3d12->bake_cache_hits() == hits_after_warm &&
                     d3d12->bake_cache_misses() == misses_after_warm,
                 "transaction frames must not touch the CPU bake cache");

  COMP_CHECK_MSG(document.txn_commit(&txn_error) == COMP_OK, txn_error.c_str());
  COMP_CHECK(!document.txn_active());

  const unsigned long long uploads_after_commit = d3d12->texture_uploads();
  why.clear();
  std::vector<uint8_t> committed;
  COMP_CHECK_MSG(render_to_buffer(*gpu, document, size, &committed, &why), why.c_str());
  COMP_CHECK_MSG(gpu->last_render_offloaded(), "the post-commit frame must reach the device");
  COMP_CHECK_MSG(d3d12->texture_uploads() - uploads_after_commit == 1u,
                 "after the commit only the touched baked unit may be uploaded again");
  COMP_CHECK_MSG(committed != warm, "the committed frame must reflect the edited layer");

  std::unique_ptr<RenderBackend> cpu = make_backend(&engine, COMP_BACKEND_CPU, &why);
  COMP_CHECK(cpu != nullptr);
  if (cpu == nullptr) return;
  std::vector<uint8_t> cpu_pixels;
  COMP_CHECK_MSG(render_to_buffer(*cpu, document, size, &cpu_pixels, &why), why.c_str());
  int max_diff = 255;
  size_t diff_count = 0;
  size_t first_diff = 0;
  compare_pixels(cpu_pixels, committed, &max_diff, &diff_count, &first_diff);
  const std::string message = "post-commit baked CPU parity (max_diff=" +
                              std::to_string(max_diff) + ", diffs=" +
                              std::to_string(diff_count) +
                              ", first_byte=" + std::to_string(first_diff) + ")";
  COMP_CHECK_MSG(!committed.empty() && max_diff <= 2, message.c_str());
}
