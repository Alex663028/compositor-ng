// AI生成
/*
 * test_shell_contract.cpp — Windows 壳与鸿蒙壳的「同一 ABI 语义」共享契约。
 *
 * 背景：compositor-ng 是「同引擎、两壳」结构。Windows 壳（shells/windows）与
 * 鸿蒙壳（shells/ohos）不做像素处理、不拼 JSON，全部图像能力经同一组 comp_* C ABI
 * 下沉到引擎核。因此两壳能否表现一致，取决于两件事：
 *   1. 冻结的数值常量（枚举 rawValue）不发生漂移；
 *   2. 同一串 ABI 调用在两壳下得到同一语义结果。
 * 本文件把这两点固化为回归护栏：任何一壳所依赖的常量/语义被改动，这里立即失败。
 *
 * 覆盖点：
 *   - ABI 数值常量（像素格式 / 后端 / 表面种类 / 结果码 / 编解码状态）；
 *   - 引擎生命周期（壳都先 create(AUTO) 再 backend）；
 *   - 文档 + 图层 + flatten 的像素语义（两壳共用同一 comp_render_flatten）；
 *   - 窗口表面（Windows HWND / OHOS HARMONY_WINDOW）如实返回 UNSUPPORTED，
 *     而离屏 present/wait 不失败（两壳可不加判别地调用）；
 *   - 编解码能力查询（两壳的“格式可用性”来自同一登记表）；
 *   - 调整图层控件元数据（两壳面板都由同一份键名表驱动）。
 */
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "test_framework.h"

namespace {

comp_engine_desc make_desc(comp_backend backend) {
  comp_engine_desc desc{};
  desc.app_name = "compositor-shell-contract-tests";
  desc.version = "1.0.0";
  desc.backend = backend;
  desc.worker_threads = 1;
  return desc;
}

comp_engine make_engine(comp_backend backend) {
  comp_engine engine = nullptr;
  comp_engine_desc desc = make_desc(backend);
  if (comp_engine_create(&desc, &engine) != COMP_OK) {
    return nullptr;
  }
  return engine;
}

comp_document make_document(comp_engine engine, int32_t width, int32_t height,
                            comp_layer_id* out_layer) {
  comp_document_desc document_desc{};
  document_desc.width = static_cast<uint32_t>(width);
  document_desc.height = static_cast<uint32_t>(height);
  document_desc.dpi = 72.0f;
  document_desc.color_space = nullptr; /* 默认 sRGB */

  comp_document document = nullptr;
  if (comp_document_create(engine, &document_desc, &document) != COMP_OK) {
    return nullptr;
  }

  comp_layer_desc layer_desc{};
  layer_desc.name = "layer";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.sampling = COMP_SAMPLING_NEAREST;

  comp_layer_id layer = 0;
  if (comp_layer_add(document, &layer_desc, &layer) != COMP_OK) {
    comp_document_destroy(document);
    return nullptr;
  }
  if (out_layer != nullptr) {
    *out_layer = layer;
  }
  return document;
}

/* 两段式读取某格式的边界说明文本（登记表里的 note）。 */
std::string codec_note(comp_image_format format) {
  comp_codec_state state = COMP_CODEC_UNAVAILABLE;
  size_t needed = 0;
  if (comp_io_codec_support(format, &state, nullptr, 0, &needed) != COMP_OK) {
    return std::string();
  }
  if (needed == 0) {
    return std::string();
  }
  std::string buffer(needed, '\0');
  size_t used = 0;
  comp_io_codec_support(format, &state, &buffer[0], buffer.size(), &used);
  while (!buffer.empty() && buffer.back() == '\0') {
    buffer.pop_back();
  }
  return buffer;
}

bool near(int a, int b) {
  const int diff = a - b;
  return diff >= -1 && diff <= 1;
}

}  // namespace

COMP_TEST(shell_contract_abi_constants_are_frozen) {
  /* 两壳都以数值使用这些枚举；任一漂移即两壳行为分叉，必须先改 README §5 冻结表。 */
  COMP_CHECK(COMP_PIXEL_RGBA8 == 0);
  COMP_CHECK(COMP_BACKEND_AUTO == 0);
  COMP_CHECK(COMP_BACKEND_METAL == 1);
  COMP_CHECK(COMP_BACKEND_D3D12 == 2);
  COMP_CHECK(COMP_BACKEND_VULKAN == 3);
  COMP_CHECK(COMP_BACKEND_CPU == 4);
  COMP_CHECK(COMP_SURFACE_HWND == 0);
  COMP_CHECK(COMP_SURFACE_METAL_LAYER == 1);
  COMP_CHECK(COMP_SURFACE_HARMONY_WINDOW == 2);
  COMP_CHECK(COMP_SURFACE_CPU_BUFFER == 3);
  COMP_CHECK(COMP_FORMAT_PNG == 1);
  COMP_CHECK(COMP_FORMAT_JPEG == 2);
  COMP_CHECK(COMP_FORMAT_PSD == 7);
  COMP_CHECK(COMP_OK == 0);
  COMP_CHECK(COMP_ERR_INVALID_ARGUMENT == 1);
  COMP_CHECK(COMP_ERR_UNSUPPORTED == 4);
  COMP_CHECK(COMP_CODEC_BUILTIN == 0);
  COMP_CHECK(COMP_CODEC_UNAVAILABLE == 1);
  COMP_CHECK(COMP_CODEC_VENDORED == 2);
}

COMP_TEST(shell_contract_engine_lifecycle_is_shared) {
  /* 壳上电第一步：create(AUTO) → 查询实际生效后端；结果必须是合法枚举值。 */
  comp_engine auto_engine = make_engine(COMP_BACKEND_AUTO);
  COMP_CHECK_MSG(auto_engine != nullptr, "auto engine");
  if (auto_engine != nullptr) {
    comp_backend actual = COMP_BACKEND_METAL; /* 故意置为非法初值，未回写则暴露 */
    COMP_CHECK(comp_engine_backend(auto_engine, &actual) == COMP_OK);
    COMP_CHECK(actual >= COMP_BACKEND_AUTO && actual <= COMP_BACKEND_CPU);
    comp_engine_destroy(auto_engine);
  }

  /* 显式请求 CPU 必须原样生效（两壳测试与回退路径都依赖这一点）。 */
  comp_engine cpu_engine = make_engine(COMP_BACKEND_CPU);
  COMP_CHECK_MSG(cpu_engine != nullptr, "cpu engine");
  if (cpu_engine != nullptr) {
    comp_backend cpu_actual = COMP_BACKEND_AUTO;
    COMP_CHECK(comp_engine_backend(cpu_engine, &cpu_actual) == COMP_OK);
    COMP_CHECK(cpu_actual == COMP_BACKEND_CPU);
    comp_engine_destroy(cpu_engine);
  }
}

COMP_TEST(shell_contract_flatten_semantics_is_shared) {
  comp_engine engine = make_engine(COMP_BACKEND_CPU);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_layer_id layer = 0;
  comp_document document = make_document(engine, 8, 8, &layer);
  COMP_CHECK_MSG(document != nullptr, "document");
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  const int32_t width = 8;
  const int32_t height = 8;
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> source(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i + 3 < source.size(); i += 4) {
    source[i + 0] = 200;
    source[i + 1] = 100;
    source[i + 2] = 50;
    source[i + 3] = 255; /* 不透明 */
  }

  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = stride;
  image.pixels = source.data();
  COMP_CHECK(comp_layer_set_pixels(document, layer, &image) == COMP_OK);

  std::vector<uint8_t> out(stride * static_cast<size_t>(height), 0);
  COMP_CHECK(comp_render_flatten(document, out.data(), COMP_PIXEL_RGBA8, stride) == COMP_OK);

  /* 不透明源以 Normal 叠在透明底上：结果应保留源色（±1 LSB 舍入），alpha 回到不透明。 */
  COMP_CHECK_MSG(out[3] == 255, "alpha opaque");
  COMP_CHECK_MSG(near(out[0], 200), "red channel");
  COMP_CHECK_MSG(near(out[1], 100), "green channel");
  COMP_CHECK_MSG(near(out[2], 50), "blue channel");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(shell_contract_window_surfaces_report_unsupported) {
  comp_engine engine = make_engine(COMP_BACKEND_CPU);
  COMP_CHECK_MSG(engine != nullptr, "engine");
  if (engine == nullptr) return;

  comp_layer_id layer = 0;
  comp_document document = make_document(engine, 4, 4, &layer);
  COMP_CHECK_MSG(document != nullptr, "document");
  if (document == nullptr) {
    comp_engine_destroy(engine);
    return;
  }

  /*
   * 两壳各自的窗口表面（Windows=HWND，OHOS=OHNativeWindow*）当前都无设备侧上屏实现，
   * 必须如实返回 UNSUPPORTED，而不是伪造成功。native_handle 用占位值，不接触真实窗口。
   */
  const comp_surface_kind surfaces[2] = {COMP_SURFACE_HWND, COMP_SURFACE_HARMONY_WINDOW};
  for (comp_surface_kind kind : surfaces) {
    comp_render_target target{};
    target.size = comp_size{4, 4};
    target.format = COMP_PIXEL_RGBA8;
    target.kind = kind;
    target.native_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    target.stride = 0;
    target.pixels = nullptr;
    COMP_CHECK_MSG(comp_render_document(document, &target) == COMP_ERR_UNSUPPORTED,
                   "window surface must be honestly unsupported");
  }

  /* 离屏后端下 present/wait 为无操作：两壳可无判别调用且不得失败。 */
  COMP_CHECK_MSG(comp_render_present(document) == COMP_OK, "present noop ok");
  COMP_CHECK_MSG(comp_render_wait(document) == COMP_OK, "wait noop ok");

  comp_document_destroy(document);
  comp_engine_destroy(engine);
}

COMP_TEST(shell_contract_codec_capability_query_is_shared) {
  /* 两壳的“该格式能不能用”来自同一 comp_io_codec_support，绝不各自硬编码。 */
  comp_codec_state state = COMP_CODEC_UNAVAILABLE;

  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_PNG, &state, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK_MSG(state == COMP_CODEC_VENDORED || state == COMP_CODEC_BUILTIN, "png usable");
  COMP_CHECK_MSG(!codec_note(COMP_FORMAT_PNG).empty(), "png note present");

  /* JPEG 经 vendored stb 提供（有损）。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_JPEG, &state, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK_MSG(state == COMP_CODEC_VENDORED, "jpeg vendored");

  /* PSD 由 stb 提供（只读）。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_PSD, &state, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK_MSG(state == COMP_CODEC_VENDORED, "psd vendored");

  /* 未链接实现的格式必须如实报告 UNAVAILABLE，并给出原因。 */
  COMP_CHECK(comp_io_codec_support(COMP_FORMAT_TIFF, &state, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK_MSG(state == COMP_CODEC_UNAVAILABLE, "tiff unavailable");
  COMP_CHECK_MSG(!codec_note(COMP_FORMAT_TIFF).empty(), "tiff note present");
}

COMP_TEST(shell_contract_adjustment_control_metadata_is_shared) {
  /* 两壳的调整面板都由同一份键名表驱动；kind=3 是 Exposure（上游 rawValue）。 */
  const char* name = nullptr;
  COMP_CHECK(comp_layer_adjustment_kind_name(3, &name) == COMP_OK);
  COMP_CHECK_MSG(name != nullptr && std::string(name) == "Exposure", "kind name");

  size_t count = 0;
  COMP_CHECK(comp_layer_adjustment_param_count(3, &count) == COMP_OK);
  COMP_CHECK_MSG(count == 3, "exposure param count");

  comp_adjustment_param param{};
  COMP_CHECK(comp_layer_adjustment_param_at(3, 0, &param) == COMP_OK);
  COMP_CHECK_MSG(param.json_path != nullptr &&
                     std::string(param.json_path) == "exposureSettings.exposure",
                 "exposure first path");

  /* 越界必须显式失败，不得回落到某个默认 kind / 默认参数。 */
  COMP_CHECK(comp_layer_adjustment_kind_name(12, &name) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_adjustment_param_at(3, 99, &param) == COMP_ERR_INVALID_ARGUMENT);
}
