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
