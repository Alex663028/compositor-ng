// AI生成
//
// shells/ohos/src/napi_init.cpp — 鸿蒙外壳（M4）的 NAPI 桥接层。
//
// 层次边界（与 compositor-ng 七层一致）：
//   本文件属于「平台外壳层」，只允许 include C ABI 头（compositor/comp_*.h）
//   与 OHOS NDK 头；不得 include engine 内部头，不得在壳内做像素处理。
//   所有图像能力一律经 comp_* ABI 下沉到引擎核。
//
// 桥接职责（2026-10-25 起，PS 基础功能批）：
//   1. 引擎生命周期：startup(backend) / shutdown() / backendName()
//   2. 文档与图层：newDocument / closeDocument / addSolidLayer / setTopOpacity /
//      setTopBlend / layerCount —— 经 comp_document_* / comp_layer_* ABI
//   3. 滤镜与撤销：applyVignette（comp_filter_apply）/ undo / redo / canUndo / canRedo
//   4. 导出：exportPng(path)（comp_io_export）
//   5. 呈现面：XComponent 的 surface 回调（OHNativeWindow）几何采集，暴露 surfaceInfo()
//   6. 上屏（OPT-21 CPU 通路）：present() 用 OH_NativeWindow_RequestBuffer 取窗口
//      缓冲，把 comp_render_flatten 的输出**直写**进 BufferHandle（按窗口 stride），
//      再 FlushBuffer 提交——不经引擎的 GPU 表面路径（Vulkan 交换链 = OPT-24 未落地，
//      CPU 通路先把画面真正显示出来；引擎零改动）。
//   7. flatten(w, h) 保留（临时文档离屏合成，诊断用）。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>

#define LOG_DOMAIN 0xD003F0
#define LOG_TAG "compositor-ohos"
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>

#include <napi/native_api.h>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_filter.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_undo.h"

namespace {

/* hilog 观测：fprintf(stderr) 在应用里不进 hilog，用真 hilog 才能定位卡点。 */
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

// ---------------------------------------------------------------------------
// 模块级状态：一个 ArkTS 进程对应一个外壳实例。
// ---------------------------------------------------------------------------

comp_engine g_engine = nullptr;
comp_document g_document = nullptr;
comp_layer_id g_top_layer = 0;
/*
 * 当前选中图层（「活动层」）。
 *
 * 为什么壳要自己记一份：引擎的 comp_layer_set_active 只有**写**没有**读**
 * （ABI 里没有 getter，activeLayerID 只在 .comp 序列化时用）。壳侧所有
 * 「作用于选中层」的操作（调整、vignette、改名…）都要一个 id，所以壳自己
 * 保管，同时写回引擎让 .comp 里的 activeLayerID 正确。
 */
comp_layer_id g_active_layer = 0;

/*
 * 图层树的**显示序缓存**（树形前序：组后面紧跟它的子层）。
 *
 * 为什么要缓存而不是每次现算：树形排布要 O(n²) 找父级，而壳侧画一行要取
 * 7 个字段（id / 名字 / 显隐 / 透明度 / 是否组 / 父级 / 层级）——每取一个
 * 都重算一遍树，层一多就明显卡。build 一次、逐字段取，是同一份数据的
 * 两个视图。
 *
 * 为什么不用 JSON 一把给：ArkTS 侧 `JSON.parse(raw) as Row[]` 会把
 * **interface 里靠后的字段抹成 0**（实测：同一个字符串，直接 stringify 出来
 * 是 `"pp":4`，as 转换后读到的却是 0；`isGroup` 之前的字段都正常）。
 * 标量接口没有这个坑。
 */
struct LayerRowEntry {
  comp_layer_id id = 0;
  int64_t parent = 0;
  int32_t level = 0;
  int32_t visible = 1;
  int32_t is_group = 0;
  double opacity = 1.0;
  std::string name;
  /*
   * 混合模式与剪贴基底（2026-10-27）。
   *
   * 两者都是**已参与像素求值**的属性（composite_ops 真的按它们算），面板上不显示
   * 的话，用户看到的是「画面不对，但每一层的透明度都对」——最难查的那一类。
   * 剪贴基底为 0 表示不剪贴。
   */
  int32_t blend = 0;
  int64_t clipping = 0;
};
std::vector<LayerRowEntry> g_layer_rows;
uint32_t g_doc_width = 0;
uint32_t g_doc_height = 0;
/* 上次导入的返回码（诊断用：设备上那行小字直接显示，不用翻 hilog）。 */
int32_t g_last_import_rc = -100;

std::atomic<uint64_t> g_surface_generation{0};  // surface 重建计数（present 防野指针）
uint32_t g_configured_width = 0;                // 窗口已配置的缓冲几何（只在变化时下发）
uint32_t g_configured_height = 0;
OH_NativeXComponent* g_xcomponent = nullptr;
OHNativeWindow* g_native_window = nullptr;
uint64_t g_surface_width = 0;
uint64_t g_surface_height = 0;
bool g_surface_ready = false;

void RefreshSurfaceGeometry(OH_NativeXComponent* component, void* window) {
  g_xcomponent = component;
  g_native_window = reinterpret_cast<OHNativeWindow*>(window);
  g_surface_width = 0;
  g_surface_height = 0;
  if (component != nullptr && window != nullptr) {
    OH_NativeXComponent_GetXComponentSize(component, window, &g_surface_width,
                                          &g_surface_height);
  }
  g_surface_ready = (g_native_window != nullptr);
}

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
  RefreshSurfaceGeometry(component, window);
  g_surface_generation.fetch_add(1);  // 新 surface：在途 present 按旧代数作废
  LOGI("surface created %{public}llux%{public}llu",
       (unsigned long long)g_surface_width, (unsigned long long)g_surface_height);
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
  RefreshSurfaceGeometry(component, window);
  /* surface 变化（尺寸/重建，如 2030x456 -> 2038x570）会重建 BufferQueue——
   * 已下发的 format/usage 随之失效，不重配则 RequestBuffer 拒绝（40601000，
   * 2026-10-26 模拟器实测）。重置标志，下一帧 present 重新配置。 */
  g_configured_width = 0;
  g_configured_height = 0;
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* window) {
  (void)component;
  (void)window;
  g_surface_generation.fetch_add(1);  // 在途 present 的 window 指针从此作废
  g_native_window = nullptr;
  g_surface_ready = false;
  g_configured_width = 0;
  g_configured_height = 0;
}



napi_value MakeInt32(napi_env env, int32_t value) {
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out;
}

napi_value MakeString(napi_env env, const char* text) {
  napi_value out = nullptr;
  napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &out);
  return out;
}

/*
 * 图层 id 走 double 而不是 int32：comp_layer_id 是 uint64，ArkTS 的 number 是
 * double（精确到 2^53，够用）。用 int32 返回会把大 id 截断成负数——壳侧再拿
 * 这个 id 去操作就「层不存在」。
 */
napi_value MakeDouble(napi_env env, double value) {
  napi_value out = nullptr;
  napi_create_double(env, value, &out);
  return out;
}

napi_value MakeNull(napi_env env) {
  napi_value out = nullptr;
  napi_get_null(env, &out);
  return out;
}

namespace {

/*
 * JSON 字符串转义。
 *
 * 图层名是用户可改的任意 UTF-8，里面可能有引号、反斜杠、换行——直接拼进
 * JSON 会让 ArkTS 侧的 JSON.parse 失败，整个图层面板都出不来。所以这里
 * 必须转义，而不是「假设名字里没有奇怪字符」。
 */
void json_escape_into(std::string& out, const char* text) {
  for (const char* p = text; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8] = {0};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          /* UTF-8 多字节原样保留（JSON 允许非 ASCII 直接出现）。 */
          out += static_cast<char>(c);
        }
        break;
    }
  }
}

/* 从 NAPI 参数里取一个 uint64（图层 id）。 */
bool read_layer_id(napi_env env, napi_value value, comp_layer_id* out) {
  if (value == nullptr) return false;
  double number = 0.0;
  if (napi_get_value_double(env, value, &number) != napi_ok) return false;
  if (number < 0.0) return false;
  *out = static_cast<comp_layer_id>(number);
  return true;
}

}  // namespace

const char* BackendName(comp_backend backend) {
  switch (backend) {
    case (comp_backend)1:
      return "Metal";
    case (comp_backend)2:
      return "D3D12";
    case (comp_backend)3:
      return "Vulkan";
    case (comp_backend)4:
      return "CPU";
    case (comp_backend)0:
    default:
      return "Auto";
  }
}

/* 确保引擎在（文档/图层等入口先走这里，UI 不必显式 startup）。 */
int32_t EnsureEngine() {
  if (g_engine != nullptr) return 0;
  comp_engine_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.app_name = "Compositor OHOS";
  desc.version = "0.5.0";
  desc.backend = COMP_BACKEND_AUTO;
  desc.worker_threads = 0;
  return static_cast<int32_t>(comp_engine_create(&desc, &g_engine));
}

// ---------------------------------------------------------------------------
// JS 导出：引擎生命周期
// ---------------------------------------------------------------------------

napi_value JsStartup(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t requested = 0;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_int32(env, argv[0], &requested);
  }
  if (g_engine != nullptr) {
    if (g_document != nullptr) {
      comp_document_destroy(g_document);
      g_document = nullptr;
      g_top_layer = 0;
    }
    comp_engine_destroy(g_engine);
    g_engine = nullptr;
  }
  comp_engine_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.app_name = "Compositor OHOS";
  desc.version = "0.5.0";
  desc.backend = static_cast<comp_backend>(requested);
  desc.worker_threads = 0;
  comp_result result = comp_engine_create(&desc, &g_engine);
  if (result != 0) g_engine = nullptr;
  return MakeInt32(env, static_cast<int32_t>(result));
}

napi_value JsShutdown(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document != nullptr) {
    comp_document_destroy(g_document);
    g_document = nullptr;
    g_top_layer = 0;
  }
  if (g_engine != nullptr) {
    comp_engine_destroy(g_engine);
    g_engine = nullptr;
  }
  return MakeInt32(env, 0);
}

napi_value JsBackendName(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_engine == nullptr) return MakeString(env, "None");
  comp_backend actual = COMP_BACKEND_AUTO;
  if (comp_engine_backend(g_engine, &actual) != 0) return MakeString(env, "Unknown");
  return MakeString(env, BackendName(actual));
}

// ---------------------------------------------------------------------------
// JS 导出：文档与图层（PS 基础：新建 / 纯色层 / 不透明度 / 混合模式 / 计数）
// ---------------------------------------------------------------------------

napi_value JsNewDocument(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[0], &width);
    napi_get_value_int32(env, argv[1], &height);
  }
  if (width <= 0 || height <= 0 || width > 30000 || height > 30000) {
    return MakeInt32(env, 1);  // COMP_ERR_INVALID_ARGUMENT
  }
  const int32_t engine_rc = EnsureEngine();
  if (engine_rc != 0) return MakeInt32(env, engine_rc);

  if (g_document != nullptr) comp_document_destroy(g_document);
  g_document = nullptr;
  g_top_layer = 0;

  comp_document_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.width = static_cast<uint32_t>(width);
  desc.height = static_cast<uint32_t>(height);
  desc.dpi = 72.0f;
  desc.color_space = "sRGB";
  comp_document document = nullptr;
  const comp_result rc = comp_document_create(g_engine, &desc, &document);
  if (rc != 0 || document == nullptr) return MakeInt32(env, static_cast<int32_t>(rc));
  g_document = document;
  g_doc_width = desc.width;
  g_doc_height = desc.height;
  return MakeInt32(env, 0);
}

napi_value JsCloseDocument(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document != nullptr) {
    comp_document_destroy(g_document);
    g_document = nullptr;
  }
  g_top_layer = 0;
  return MakeInt32(env, 0);
}

napi_value JsAddSolidLayer(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t rgba[4] = {0, 0, 0, 255};
  for (size_t i = 0; i < argc && i < 4; ++i) {
    napi_get_value_int32(env, argv[i], &rgba[i]);
  }
  if (g_document == nullptr) return MakeInt32(env, -1);

  comp_layer_desc layer_desc;
  std::memset(&layer_desc, 0, sizeof(layer_desc));
  layer_desc.name = "solid";
  layer_desc.parent = 0;
  /*
   * opacity 必须显式给 1：memset 0 之后它是**完全透明**的层。
   *
   * 这个坑一直没暴露，是因为模拟器上画布上不了屏（OPT-21b）——建层看起来
   * 「正常」，真机上一进来就是「建了层但什么都看不到」。
   */
  layer_desc.opacity = 1.0f;
  comp_layer_id id = 0;
  comp_result rc = comp_layer_add(g_document, &layer_desc, &id);
  if (rc != 0) return MakeInt32(env, static_cast<int32_t>(rc));

  const size_t stride = static_cast<size_t>(g_doc_width) * 4u;
  std::vector<uint8_t> pixels(stride * g_doc_height);
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<uint8_t>(rgba[0]);
    pixels[i + 1] = static_cast<uint8_t>(rgba[1]);
    pixels[i + 2] = static_cast<uint8_t>(rgba[2]);
    pixels[i + 3] = static_cast<uint8_t>(rgba[3]);
  }
  comp_image image;
  std::memset(&image, 0, sizeof(image));
  image.width = static_cast<int32_t>(g_doc_width);
  image.height = static_cast<int32_t>(g_doc_height);
  image.format = COMP_PIXEL_RGBA8;
  image.stride = stride;
  image.pixels = pixels.data();
  rc = comp_layer_set_pixels(g_document, id, &image);
  if (rc != 0) return MakeInt32(env, static_cast<int32_t>(rc));
  g_top_layer = id;
  /*
   * 新建的层同时设成**活动层**：与 PS 一致（新建层立刻被选中），也让
   * 「选中即作用对象」的调整操作有目标——否则用户新建一层、点暗角，
   * 会因为「什么都没选」而打在上一次的层上。
   */
  g_active_layer = id;
  comp_layer_set_active(g_document, id);
  return MakeDouble(env, static_cast<double>(id));
}

napi_value JsSetTopOpacity(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  double opacity = 1.0;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_double(env, argv[0], &opacity);
  }
  if (g_document == nullptr || g_top_layer == 0) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(
                            comp_layer_set_opacity(g_document, g_top_layer,
                                                   static_cast<float>(opacity))));
}

napi_value JsSetTopBlend(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t blend = 0;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_int32(env, argv[0], &blend);
  }
  if (g_document == nullptr || g_top_layer == 0) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(
                            comp_layer_set_blend(g_document, g_top_layer,
                                                 static_cast<comp_blend_mode>(blend))));
}

napi_value JsLayerCount(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeInt32(env, -1);
  size_t count = 0;
  if (comp_layer_count(g_document, &count) != 0) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(count));
}

/* ------------------------------------------------ 混合模式 / 剪贴蒙版 */

namespace {

/*
 * 引擎名 -> 界面名：`LINEAR_DODGE` -> `Linear Dodge`。
 *
 * 刻意**不维护一份名字表**：Windows 壳有一份手写表（blend_mode_name），引擎加
 * 一个档位就得两边一起改，漏了就静默缺项。这里只做格式变换，名字的真相始终在
 * 引擎（comp_blend_mode_name），枚举长度也由「名字变成 UNKNOWN」自动定界。
 */
std::string blend_display_name(const char* engine_name) {
  std::string out;
  bool start_of_word = true;
  for (const char* p = engine_name; p != nullptr && *p != '\0'; ++p) {
    char ch = *p;
    if (ch == '_') {
      out += ' ';
      start_of_word = true;
      continue;
    }
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (start_of_word && ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
    start_of_word = false;
    out += ch;
  }
  return out;
}

}  // namespace

/*
 * 全部混合模式：`mode\x1fname`，行间 `\x1e`。
 *
 * 为什么不是 24 个常量：档位数由引擎枚举决定（名字变成 UNKNOWN 为止）。壳里
 * 写死数字的话，引擎加「Vivid Light」之外的档位时下拉会少一项，而界面上看不
 * 出少了什么。
 */
napi_value JsBlendModesText(napi_env env, napi_callback_info info) {
  (void)info;
  std::string out;
  for (int mode = 0; mode < 256; ++mode) {
    const char* name = comp_blend_mode_name(static_cast<comp_blend_mode>(mode));
    if (name == nullptr || std::strcmp(name, "UNKNOWN") == 0) break;
    if (!out.empty()) out += '\x1e';
    out += std::to_string(mode);
    out += '\x1f';
    out += blend_display_name(name);
  }
  return MakeString(env, out.c_str());
}

/* 某层的混合模式；失败返回 -1。 */
napi_value JsLayerBlend(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr || argc < 1) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, -1);
  comp_blend_mode blend = COMP_BLEND_NORMAL;
  if (comp_layer_get_blend(g_document, id, &blend) != COMP_OK) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(blend));
}

/* 设置某层的混合模式（id, mode）。 */
napi_value JsSetLayerBlend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr || argc < 2) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  int32_t mode = 0;
  if (!read_layer_id(env, argv[0], &id) ||
      napi_get_value_int32(env, argv[1], &mode) != napi_ok || mode < 0) {
    return MakeInt32(env, -1);
  }
  return MakeInt32(env, static_cast<int32_t>(comp_layer_set_blend(
                            g_document, id, static_cast<comp_blend_mode>(mode))));
}

/* 某层的剪贴基底 id（0 = 不剪贴）；失败返回 -1。 */
napi_value JsLayerClipping(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr || argc < 1) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, -1);
  int32_t is_group = 0;
  comp_layer_id parent = 0;
  comp_layer_id clipping = 0;
  if (comp_layer_get_group_info(g_document, id, &is_group, &parent, &clipping) != COMP_OK) {
    return MakeInt32(env, -1);
  }
  /* id 是 uint64，走 double（int32 会把大 id 截断成负数）。 */
  return MakeDouble(env, static_cast<double>(clipping));
}

/*
 * 切换剪贴蒙版。
 *
 * 语义与 Windows 壳的 panel_toggle_clipping 一致（那份判定在 ui_core.h 的
 * plan_clipping_base_index，有表驱动单测）：
 *   - 已剪贴 → 取消（一步撤销）；
 *   - 未剪贴 → 挂到**下方紧邻的同父图层**。引擎的成栈条件是「基底在前、成员紧跟
 *     其后、clipping_source 指向基底」，所以下方那一层才是基底。取上方会得到
 *     「面板说有剪贴、画面毫无变化」的假成功（Windows 侧踩过）。
 *   - 下方那层自己已经是成员时，直接挂到**同一个基底**上——并入已有剪贴栈，
 *     而不是形成一个指向成员的错栈。
 *
 * 返回：0 成功；-2 下方没有同组图层；-3 下方图层的父级不同（跨组不剪贴）。
 */
napi_value JsToggleLayerClipping(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr || argc < 1) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, -1);

  int32_t self_is_group = 0;
  comp_layer_id self_parent = 0;
  comp_layer_id self_clipping = 0;
  if (comp_layer_get_group_info(g_document, id, &self_is_group, &self_parent,
                                &self_clipping) != COMP_OK) {
    return MakeInt32(env, -1);
  }
  if (self_clipping != 0) {
    return MakeInt32(env, static_cast<int32_t>(
                              comp_layer_set_clipping_source(g_document, id, 0)));
  }

  size_t count = 0;
  if (comp_layer_count(g_document, &count) != COMP_OK || count < 2) return MakeInt32(env, -2);
  std::vector<comp_layer_id> ids(count);
  size_t listed = 0;
  if (comp_layer_list(g_document, ids.data(), ids.size(), &listed) != COMP_OK) {
    return MakeInt32(env, -1);
  }
  ids.resize(listed);
  size_t self_index = listed;
  for (size_t i = 0; i < listed; ++i) {
    if (ids[i] == id) {
      self_index = i;
      break;
    }
  }
  if (self_index >= listed) return MakeInt32(env, -1);
  /* 数组序自下而上：下标 0 是最底层，它没有「下方」可挂。 */
  if (self_index == 0) return MakeInt32(env, -2);

  const comp_layer_id neighbor = ids[self_index - 1];
  int32_t neighbor_is_group = 0;
  comp_layer_id neighbor_parent = 0;
  comp_layer_id neighbor_clipping = 0;
  if (comp_layer_get_group_info(g_document, neighbor, &neighbor_is_group, &neighbor_parent,
                                &neighbor_clipping) != COMP_OK) {
    return MakeInt32(env, -1);
  }
  if (neighbor_parent != self_parent) return MakeInt32(env, -3);
  /* 邻居自己也是成员：并入它所在的那个栈，基底往下追一层。 */
  const comp_layer_id base = neighbor_clipping != 0 ? neighbor_clipping : neighbor;
  return MakeInt32(env, static_cast<int32_t>(
                            comp_layer_set_clipping_source(g_document, id, base)));
}

/* ---------------------------------------------------------------- 图层面板 */

/*
 * 图层列表 -> JSON 数组。
 *
 * 格式：[{"id":3,"name":"背景","visible":1,"opacity":1.0}, ...]
 * 顺序 = 引擎的堆叠序（comp_layer_list 给的顺序：上层在前）。
 *
 * 为什么一次返回整份 JSON 而不是逐层取：ArkTS 侧要画**列表**，逐层跨 NAPI
 * 边界取会变成 N 次往返，层一多就明显卡。一次取一份，ArkTS 侧 JSON.parse
 * 一次即可。
 *
 * 图层名走 JSON 转义（见 json_escape_into）——名字是用户可改的，里面可能
 * 有引号/反斜杠，不转义会让整份 JSON 解析失败、图层面板整个出不来。
 */
/* 重算图层显示序（树形前序）。返回行数；没文档返回 0。 */
int32_t build_layer_rows() {
  g_layer_rows.clear();
  if (g_document == nullptr) return 0;
  size_t count = 0;
  if (comp_layer_count(g_document, &count) != COMP_OK || count == 0) return 0;
  std::vector<comp_layer_id> ids(count);
  size_t listed = 0;
  if (comp_layer_list(g_document, ids.data(), ids.size(), &listed) != COMP_OK || listed == 0) {
    return 0;
  }
  ids.resize(listed);

  std::vector<int64_t> parents(listed, 0);
  std::vector<uint8_t> groups(listed, 0);
  std::vector<int64_t> clippings(listed, 0);
  for (size_t i = 0; i < listed; ++i) {
    int32_t is_group = 0;
    comp_layer_id parent = 0;
    comp_layer_id clipping = 0;
    const comp_result group_rc =
        comp_layer_get_group_info(g_document, ids[i], &is_group, &parent, &clipping);
    if (group_rc == COMP_OK) {
      parents[i] = static_cast<int64_t>(parent);
      groups[i] = static_cast<uint8_t>(is_group != 0 ? 1 : 0);
      clippings[i] = static_cast<int64_t>(clipping);
    }
  }

  std::vector<std::vector<size_t>> children(listed);
  std::vector<size_t> roots;
  for (size_t i = 0; i < listed; ++i) {
    size_t parent_index = listed;
    for (size_t j = 0; j < listed; ++j) {
      if (ids[j] == static_cast<comp_layer_id>(parents[i])) {
        parent_index = j;
        break;
      }
    }
    if (parent_index == listed) {
      roots.push_back(i);
    } else {
      children[parent_index].push_back(i);
    }
  }

  std::vector<uint8_t> visited(listed, 0);
  auto emit = [&](auto&& self, size_t index, int32_t level) -> void {
    if (index >= listed || level > 64 || visited[index] != 0) return;
    visited[index] = 1;
    LayerRowEntry row;
    row.id = ids[index];
    row.parent = parents[index];
    row.level = level;
    row.is_group = groups[index];
    row.clipping = clippings[index];
    char name[256] = {0};
    size_t required = 0;
    if (comp_layer_get_name(g_document, row.id, name, sizeof(name), &required) != COMP_OK) {
      std::snprintf(name, sizeof(name), "%llu", static_cast<unsigned long long>(row.id));
    }
    row.name = name;
    int32_t visible = 1;
    comp_layer_get_visible(g_document, row.id, &visible);
    row.visible = visible != 0 ? 1 : 0;
    float opacity = 1.0f;
    comp_layer_get_opacity(g_document, row.id, &opacity);
    row.opacity = static_cast<double>(opacity);
    comp_blend_mode blend = COMP_BLEND_NORMAL;
    if (comp_layer_get_blend(g_document, row.id, &blend) == COMP_OK) {
      row.blend = static_cast<int32_t>(blend);
    }
    g_layer_rows.push_back(row);
    for (size_t child : children[index]) self(self, child, level + 1);
  };
  for (size_t root : roots) emit(emit, root, 0);
  /* 环 / 断链剩下的补在末尾：宁可顺序怪，也不能让层在面板上消失。 */
  for (size_t i = 0; i < listed; ++i) {
    if (visited[i] == 0) emit(emit, i, 0);
  }
  return static_cast<int32_t>(g_layer_rows.size());
}


/*
 * 整棵图层树，**一次给完**，用分隔符而不是 JSON。
 *
 * 格式：每行 `id\x1fname\x1fvisible\x1fopacity\x1fisGroup\x1fparent\x1flevel`，
 * 行间用 `\x1e`。
 *
 * 为什么不用 JSON：ArkTS 侧 `JSON.parse(raw) as Row[]` 会把 interface 里靠后的
 * 字段**抹成 0**（实测：同一个字符串直接显示是 `"pp":4`，as 转换后读到的却是 0，
 * 靠前的字段却正常）。为什么不用「build 一次 + 逐字段取」：多次 NAPI 调用之间
 * 那份缓存会与调用方的渲染节奏错位（实测不同行读到不同批次的值）。
 *
 * 字符串本身是可靠的（同一份 raw 直接显示从来没错过），所以就在字符串里
 * 把字段分好，壳侧一次 split 到底。
 */
napi_value JsLayerTreeText(napi_env env, napi_callback_info info) {
  (void)info;
  build_layer_rows();
  std::string out;
  for (size_t i = 0; i < g_layer_rows.size(); ++i) {
    const LayerRowEntry& row = g_layer_rows[i];
    if (i > 0) out += '\x1e';
    out += std::to_string(static_cast<unsigned long long>(row.id));
    out += '\x1f';
    out += row.name;
    out += '\x1f';
    out += row.visible != 0 ? "1" : "0";
    out += '\x1f';
    char opacity[32] = {0};
    std::snprintf(opacity, sizeof(opacity), "%.3f", row.opacity);
    out += opacity;
    out += '\x1f';
    out += row.is_group != 0 ? "1" : "0";
    out += '\x1f';
    out += std::to_string(static_cast<long long>(row.parent));
    out += '\x1f';
    out += std::to_string(row.level);
    out += '\x1f';
    out += std::to_string(row.blend);
    out += '\x1f';
    out += std::to_string(static_cast<long long>(row.clipping));
  }
  return MakeString(env, out.c_str());
}

/*
 * 新建图层组（name: 可选，layerId: 可选 = 把这一层移进去）。
 *
 * 返回新组的 id（double）；失败返回负的错误码。
 *
 * 「原位建组」= 组落在原层的**位置**上、原层成为组的第一个子层。顺序很重要：
 * 先把组移到原层之前（同级），再把原层移进组——反过来的话组会跑到原层下面，
 * 用户看到「点了建组，图层跑到组上面去了」。
 */
napi_value JsCreateGroup(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeInt32(env, -1);

  char name[128] = "组";
  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype type = napi_undefined;
    napi_typeof(env, argv[0], &type);
    if (type == napi_string) {
      size_t length = 0;
      napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length);
      if (length > 0 && length < sizeof(name)) {
        napi_get_value_string_utf8(env, argv[0], name, sizeof(name), nullptr);
      }
    }
  }
  /*
   * 作用对象 = **当前活动层**（不靠第二个参数）。
   *
   * 为什么不用参数：从 ArkTS 传第二个 number 参数过来，NAPI 侧读到的恒为 0
   * （实测：ArkTS 侧 selectedLayerId 明明是 3，createGroup 里读 argv[1] 却是 0，
   * 于是「建组」只建了个空组、什么也没收进去）。而活动层本来就是壳侧维护的
   * 单一真源（点选即写回），直接读它不会错位。
   *
   * layerId 参数保留但仅作覆盖用：传了非 0 就优先用它。
   */
  comp_layer_id layer_id = g_active_layer;
  if (argc >= 2 && argv[1] != nullptr) {
    comp_layer_id explicit_id = 0;
    if (read_layer_id(env, argv[1], &explicit_id) && explicit_id != 0) layer_id = explicit_id;
  }

  comp_layer_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.name = name;
  desc.is_group = 1;
  desc.opacity = 1.0f;
  comp_layer_id group_id = 0;
  comp_result rc = comp_layer_add(g_document, &desc, &group_id);
  if (rc != COMP_OK) return MakeInt32(env, static_cast<int32_t>(rc));
  if (layer_id != 0) {
    comp_layer_move(g_document, group_id, 0, layer_id);
    comp_layer_move(g_document, layer_id, group_id, 0);
  }
  return MakeDouble(env, static_cast<double>(group_id));
}

/*
 * 移动图层：换父级 + 定位（拖行 / 拖进组 / 拖出组）。
 * parent = 0 表示根级；before = 0 表示同级末尾。
 * 引擎侧拒绝成环、拒绝把组拖进自己（INVALID_ARGUMENT）。
 */
napi_value JsMoveLayer(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  comp_layer_id parent = 0;
  comp_layer_id before = 0;
  if (argc < 1 || !read_layer_id(env, argv[0], &id) || id == 0) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  if (argc >= 2 && argv[1] != nullptr) read_layer_id(env, argv[1], &parent);
  if (argc >= 3 && argv[2] != nullptr) read_layer_id(env, argv[2], &before);
  return MakeInt32(env,
                   static_cast<int32_t>(comp_layer_move(g_document, id, parent, before)));
}

/*
 * 设置当前选中层（图层面板点选即写回）。
 *
 * 同时写引擎（comp_layer_set_active）：.comp 的 activeLayerID 就是它，重开文档
 * 时选中的还是这一层。引擎侧 id 不存在返回 NOT_FOUND——壳不把它当错误上报，
 * 因为选中是个纯 UI 动作，闪一下不该弹错。
 */
napi_value JsSetActiveLayer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  if (argc < 1 || argv[0] == nullptr || !read_layer_id(env, argv[0], &id)) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  g_active_layer = id;
  comp_layer_set_active(g_document, id);
  return MakeInt32(env, 0);
}

/* 当前选中层 id（0 = 没选）。 */
napi_value JsActiveLayer(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeDouble(env, static_cast<double>(g_active_layer));
}

/* 设置图层可见性（id, visible: boolean）。 */
napi_value JsSetLayerVisible(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) return MakeInt32(env, 1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, 1);
  bool visible = false;
  napi_get_value_bool(env, argv[1], &visible);
  if (g_document == nullptr) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(
                            comp_layer_set_visible(g_document, id, visible ? 1 : 0)));
}

/* 设置图层不透明度（id, opacity: 0..1）。 */
napi_value JsSetLayerOpacity(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) return MakeInt32(env, 1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, 1);
  double opacity = 1.0;
  napi_get_value_double(env, argv[1], &opacity);
  if (opacity < 0.0) opacity = 0.0;
  if (opacity > 1.0) opacity = 1.0;
  if (g_document == nullptr) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(
                            comp_layer_set_opacity(g_document, id,
                                                   static_cast<float>(opacity))));
}

/* 删除图层（id）。 */
napi_value JsRemoveLayer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return MakeInt32(env, 1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, 1);
  if (g_document == nullptr) return MakeInt32(env, -1);
  const comp_result rc = comp_layer_remove(g_document, id);
  if (rc == COMP_OK && g_top_layer == id) g_top_layer = 0;
  return MakeInt32(env, static_cast<int32_t>(rc));
}

/* 重命名图层（id, name: UTF-8 文本）。 */
napi_value JsSetLayerName(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) return MakeInt32(env, 1);
  comp_layer_id id = 0;
  if (!read_layer_id(env, argv[0], &id)) return MakeInt32(env, 1);
  size_t length = 0;
  napi_get_value_string_utf8(env, argv[1], nullptr, 0, &length);
  if (length == 0) return MakeInt32(env, 1);
  std::vector<char> name(length + 1, 0);
  size_t copied = 0;
  napi_get_value_string_utf8(env, argv[1], name.data(), name.size(), &copied);
  if (copied == 0) return MakeInt32(env, 1);
  if (g_document == nullptr) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(comp_layer_set_name(g_document, id, name.data())));
}

/*
 * 图层面板自检：把「列表 / 显隐 / 透明度 / 改名 / 删除」这条链路在**引擎层**
 * 跑一遍，每一步都**读回**校验，返回可读的诊断串。
 *
 * 为什么要有这个：模拟器上 XComponent 上不了屏（OPT-21b：x86_64 模拟器不给
 * surface 的 CPU 可写映射），画布上看不到任何变化——「显隐有没有真的作用到
 * 引擎」光看屏幕是验不了的。唯一能证明的办法是**读回引擎状态**：设了 visible
 * 就得读回 0，设了 opacity 就得读回 0.5，改了名就得读回新名字。
 *
 * 测完把探针层删掉，文档回到测试前的层数（不留痕）。
 */
napi_value JsLayerPanelSelfTest(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeString(env, "no-doc");
  size_t before = 0;
  comp_layer_count(g_document, &before);

  comp_layer_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.name = "panel_probe";
  comp_layer_id probe = 0;
  if (comp_layer_add(g_document, &desc, &probe) != COMP_OK) {
    return MakeString(env, "add-failed");
  }

  /* 1) 列表：探针层要能查出来。 */
  size_t count = 0;
  comp_layer_count(g_document, &count);
  std::vector<comp_layer_id> ids(count);
  size_t listed = 0;
  comp_layer_list(g_document, ids.data(), ids.size(), &listed);
  bool in_list = false;
  for (size_t i = 0; i < listed; ++i) {
    if (ids[i] == probe) {
      in_list = true;
      break;
    }
  }

  /* 2) 显隐：设 0 读回 0。 */
  comp_layer_set_visible(g_document, probe, 0);
  int32_t visible = 1;
  comp_layer_get_visible(g_document, probe, &visible);

  /* 3) 透明度：设 0.5 读回 0.5。 */
  comp_layer_set_opacity(g_document, probe, 0.5f);
  float opacity = 0.0f;
  comp_layer_get_opacity(g_document, probe, &opacity);

  /* 4) 改名：设了新名读回新名。 */
  comp_layer_set_name(g_document, probe, "renamed_probe");
  char name[64] = {0};
  size_t required = 0;
  comp_layer_get_name(g_document, probe, name, sizeof(name), &required);

  /*
   * 6) 分组：新建组 + 把探针层移进去，读回 parent 必须是这个组。
   *
   * 验的是**引擎真的记住了父子关系**，而不是壳自己记账——壳记账的话，重开
   * 文档（父子关系从 .comp 读回）就会全丢。
   */
  comp_layer_desc group_desc;
  std::memset(&group_desc, 0, sizeof(group_desc));
  group_desc.name = "panel_group_probe";
  group_desc.is_group = 1;
  group_desc.opacity = 1.0f;
  comp_layer_id group = 0;
  int32_t grouped = 0;
  if (comp_layer_add(g_document, &group_desc, &group) == COMP_OK) {
    /* 原位建组：组落到探针层之前，探针层再移进组（与 createGroup 同一顺序）。 */
    comp_layer_move(g_document, group, 0, probe);
    comp_layer_move(g_document, probe, group, 0);
    int32_t is_group = 0;
    comp_layer_id parent = 0;
    comp_layer_id clipping = 0;
    comp_layer_get_group_info(g_document, probe, &is_group, &parent, &clipping);
    grouped = parent == group ? 1 : 0;
  }

  /* 7) 拖行（换父级）：把探针层移回根级，parent 必须变回 0。 */
  int32_t ungrouped = 0;
  if (group != 0) {
    comp_layer_move(g_document, probe, 0, 0);
    int32_t is_group = 0;
    comp_layer_id parent = 0;
    comp_layer_id clipping = 0;
    comp_layer_get_group_info(g_document, probe, &is_group, &parent, &clipping);
    ungrouped = parent == 0 ? 1 : 0;
  }

  /* 8) 活动层：设进去要能读回来（调整面板作用于它）。 */
  g_active_layer = probe;
  comp_layer_set_active(g_document, probe);
  const int32_t active_ok = g_active_layer == probe ? 1 : 0;

  /* 9) 删除：先删组（子树一并删），再删探针层；层数回到测试前。 */
  if (group != 0) comp_layer_remove(g_document, group);
  comp_layer_remove(g_document, probe);
  size_t after = 0;
  comp_layer_count(g_document, &after);

  char buffer[288];
  std::snprintf(buffer, sizeof(buffer),
                "list=%d hide=%d opacity=%.2f name=%s group=%d ungroup=%d active=%d "
                "del=%d layers=%zu->%zu",
                in_list ? 1 : 0, visible, static_cast<double>(opacity), name, grouped,
                ungrouped, active_ok, after == before ? 1 : 0, before, after);
  return MakeString(env, buffer);
}

// ---------------------------------------------------------------------------
// 可编辑调整层（P1-2 收尾）：加 / 列 / 改 / 删
//
// 引擎侧接口早就齐了（comp_layer_add_adjustment / adjustment_at /
// update_adjustment_at / remove_adjustment，外加参数元数据与 build_settings /
// read_value）。壳这边**不写 JSON**：参数扁平值交给引擎拼，回填也走引擎。
//
// 传值一律用分隔符字符串而不是 JSON 数组：跨 NAPI 传数组在 ArkTS 侧类型不稳，
// 而 parse 成 interface 数组还会把靠后的字段抹成 0（本轮踩过，见 layerTreeText）。
// ---------------------------------------------------------------------------

/* 所有合法的调整类型：`kind\x1fname`，行间 `\x1e`。 */
napi_value JsAdjustKindsText(napi_env env, napi_callback_info info) {
  (void)info;
  std::string out;
  for (uint32_t kind = 0; kind < COMP_ADJUSTMENT_KIND_COUNT; ++kind) {
    const char* name = nullptr;
    if (comp_layer_adjustment_kind_name(kind, &name) != COMP_OK || name == nullptr) continue;
    if (!out.empty()) out += '\x1e';
    out += std::to_string(kind);
    out += '\x1f';
    out += name;
  }
  return MakeString(env, out.c_str());
}

/*
 * 某个调整类型的可编辑参数：`json_path\x1flabel\x1ftype\x1fmin\x1fmax\x1fdefault`。
 *
 * type 0 = 数值（滑杆）/ 1 = 布尔（开关）。壳据此生成控件——引擎给了范围与缺省，
 * 壳不需要（也不该）自己维护一份参数表。
 */
napi_value JsAdjustParamsText(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return MakeString(env, "");
  int32_t kind = -1;
  if (napi_get_value_int32(env, argv[0], &kind) != napi_ok || kind < 0) return MakeString(env, "");

  size_t count = 0;
  if (comp_layer_adjustment_param_count(static_cast<uint32_t>(kind), &count) != COMP_OK) {
    return MakeString(env, "");
  }
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    comp_adjustment_param param;
    std::memset(&param, 0, sizeof(param));
    if (comp_layer_adjustment_param_at(static_cast<uint32_t>(kind), i, &param) != COMP_OK) continue;
    if (!out.empty()) out += '\x1e';
    out += param.json_path != nullptr ? param.json_path : "";
    out += '\x1f';
    out += param.label != nullptr ? param.label : "";
    out += '\x1f';
    out += std::to_string(static_cast<int>(param.type));
    out += '\x1f';
    char number[40] = {0};
    std::snprintf(number, sizeof(number), "%.4f", param.min_value);
    out += number;
    out += '\x1f';
    std::snprintf(number, sizeof(number), "%.4f", param.max_value);
    out += number;
    out += '\x1f';
    std::snprintf(number, sizeof(number), "%.4f", param.default_value);
    out += number;
  }
  return MakeString(env, out.c_str());
}

/* 图层上已有的调整：`index\x1fkind\x1fname`，行间 `\x1e`。 */
napi_value JsLayerAdjustmentsText(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeString(env, "");
  comp_layer_id id = 0;
  if (argc < 1 || argv[0] == nullptr || !read_layer_id(env, argv[0], &id)) {
    return MakeString(env, "");
  }
  size_t count = 0;
  if (comp_layer_adjustment_count(g_document, id, &count) != COMP_OK) return MakeString(env, "");
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t size = 0;
    if (comp_layer_adjustment_at(g_document, id, i, &kind, nullptr, 0, &size) != COMP_OK) continue;
    const char* name = nullptr;
    if (comp_layer_adjustment_kind_name(kind, &name) != COMP_OK || name == nullptr) name = "?";
    if (!out.empty()) out += '\x1e';
    out += std::to_string(i);
    out += '\x1f';
    out += std::to_string(kind);
    out += '\x1f';
    out += name;
  }
  return MakeString(env, out.c_str());
}

/* 把 `\x1f` 分隔的扁平值拆成字符串数组（空串 = 沿用缺省）。 */
static std::vector<std::string> split_values(const char* text) {
  std::vector<std::string> values;
  if (text == nullptr) return values;
  std::string current;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '\x1f') {
      values.push_back(current);
      current.clear();
    } else {
      current += *p;
    }
  }
  values.push_back(current);
  return values;
}

/*
 * 给图层加一条调整（id, kind, values 用 `\x1f` 分隔，按 param 顺序）。
 *
 * JSON 由引擎的 comp_layer_adjustment_build_settings 组装——壳拼 JSON 的话，
 * 键名/转义/范围校验都得再实现一遍，迟早与引擎分叉。
 */
napi_value JsAddLayerAdjustment(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  int32_t kind = -1;
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr ||
      !read_layer_id(env, argv[0], &id) ||
      napi_get_value_int32(env, argv[1], &kind) != napi_ok || kind < 0) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  char values_text[512] = {0};
  if (argc >= 3 && argv[2] != nullptr) {
    size_t copied = 0;
    napi_get_value_string_utf8(env, argv[2], values_text, sizeof(values_text), &copied);
  }
  const std::vector<std::string> values = split_values(values_text);
  std::vector<const char*> pointers;
  pointers.reserve(values.size());
  for (const std::string& value : values) pointers.push_back(value.c_str());

  size_t need = 0;
  comp_layer_adjustment_build_settings(static_cast<uint32_t>(kind), pointers.data(),
                                       pointers.size(), nullptr, 0, &need);
  if (need == 0) return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  /*
   * 缓冲区要给 NUL 留一位：引擎的 out_size **不含** NUL，而写回时按
   * `capacity - 1` 截断。按 need 分配的话 JSON 会少最后一个字符（`}`），
   * 后续 add 直接报 INVALID_ARGUMENT——症状是「点新增永远是 rc1」。
   */
  std::string json(need + 1, '\0');
  if (comp_layer_adjustment_build_settings(static_cast<uint32_t>(kind), pointers.data(),
                                           pointers.size(), &json[0], json.size(),
                                           &need) != COMP_OK) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  return MakeInt32(env, static_cast<int32_t>(
                            comp_layer_add_adjustment(g_document, id,
                                                      static_cast<uint32_t>(kind),
                                                      json.c_str())));
}

/* 按索引替换一条调整（kind 与 settings 一并替换）。 */
napi_value JsUpdateLayerAdjustmentAt(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  int32_t index = -1;
  int32_t kind = -1;
  if (argc < 3 || argv[0] == nullptr || !read_layer_id(env, argv[0], &id) ||
      napi_get_value_int32(env, argv[1], &index) != napi_ok || index < 0 ||
      napi_get_value_int32(env, argv[2], &kind) != napi_ok || kind < 0) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  char values_text[512] = {0};
  if (argc >= 4 && argv[3] != nullptr) {
    size_t copied = 0;
    napi_get_value_string_utf8(env, argv[3], values_text, sizeof(values_text), &copied);
  }
  const std::vector<std::string> values = split_values(values_text);
  std::vector<const char*> pointers;
  pointers.reserve(values.size());
  for (const std::string& value : values) pointers.push_back(value.c_str());

  size_t need = 0;
  comp_layer_adjustment_build_settings(static_cast<uint32_t>(kind), pointers.data(),
                                       pointers.size(), nullptr, 0, &need);
  if (need == 0) return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  /* 同 add：out_size 不含 NUL，缓冲区要多一位。 */
  std::string json(need + 1, '\0');
  if (comp_layer_adjustment_build_settings(static_cast<uint32_t>(kind), pointers.data(),
                                           pointers.size(), &json[0], json.size(),
                                           &need) != COMP_OK) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  return MakeInt32(env, static_cast<int32_t>(comp_layer_update_adjustment_at(
                            g_document, id, static_cast<size_t>(index),
                            static_cast<uint32_t>(kind), json.c_str())));
}

/* 删掉第 index 条调整。 */
napi_value JsRemoveLayerAdjustment(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeInt32(env, -1);
  comp_layer_id id = 0;
  int32_t index = -1;
  if (argc < 2 || argv[0] == nullptr || !read_layer_id(env, argv[0], &id) ||
      napi_get_value_int32(env, argv[1], &index) != napi_ok || index < 0) {
    return MakeInt32(env, static_cast<int32_t>(COMP_ERR_INVALID_ARGUMENT));
  }
  return MakeInt32(env, static_cast<int32_t>(comp_layer_remove_adjustment(
                            g_document, id, static_cast<size_t>(index))));
}

/*
 * 回填某条调整的当前值：按 param 顺序输出 `value\x1e...`。
 *
 * 走引擎的 comp_layer_adjustment_read_value（按 json_path 取值），壳不解析 JSON。
 * 路径缺失时给缺省值——面板打开时滑块必须有个合理位置，不能是 0。
 */
napi_value JsLayerAdjustmentValuesText(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (g_document == nullptr) return MakeString(env, "");
  comp_layer_id id = 0;
  int32_t index = -1;
  int32_t kind = -1;
  if (argc < 3 || argv[0] == nullptr || !read_layer_id(env, argv[0], &id) ||
      napi_get_value_int32(env, argv[1], &index) != napi_ok || index < 0 ||
      napi_get_value_int32(env, argv[2], &kind) != napi_ok || kind < 0) {
    return MakeString(env, "");
  }
  size_t size = 0;
  uint32_t stored_kind = 0;
  if (comp_layer_adjustment_at(g_document, id, static_cast<size_t>(index), &stored_kind,
                              nullptr, 0, &size) != COMP_OK ||
      size == 0) {
    return MakeString(env, "");
  }
  /* 同 build_settings：out_size 不含 NUL，缓冲区多一位。 */
  std::string json(size + 1, '\0');
  if (comp_layer_adjustment_at(g_document, id, static_cast<size_t>(index), &stored_kind,
                              &json[0], json.size(), &size) != COMP_OK) {
    return MakeString(env, "");
  }

  size_t count = 0;
  if (comp_layer_adjustment_param_count(static_cast<uint32_t>(kind), &count) != COMP_OK) {
    return MakeString(env, "");
  }
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    comp_adjustment_param param;
    std::memset(&param, 0, sizeof(param));
    if (comp_layer_adjustment_param_at(static_cast<uint32_t>(kind), i, &param) != COMP_OK) continue;
    double number = param.default_value;
    int32_t boolean = param.default_value != 0.0 ? 1 : 0;
    /*
     * used_default：文档里**没有**这个键（旧版本存的 settings、或引擎新加了参数），
     * 于是退回缺省值。壳侧要把这件事显式说出来——否则用户看到的是一个「明明是
     * 我调过的值，回填回来却变了」的滑块，没有任何解释。
     */
    int32_t used_default = 1;
    if (param.json_path != nullptr) {
      double read_number = 0.0;
      int32_t read_boolean = 0;
      if (comp_layer_adjustment_read_value(json.c_str(), param.json_path, &read_number,
                                           &read_boolean) == COMP_OK) {
        number = read_number;
        boolean = read_boolean;
        used_default = 0;
      }
    }
    if (!out.empty()) out += '\x1e';
    /*
     * 每行带 **json_path**：壳按路径匹配回填，而不是按下标。
     * 按下标的坑是隐性的——引擎调整参数顺序、或在中间插一个参数，滑块就会整体
     * 错位一格，而每个值单看都「在范围内」，界面上看不出来。
     */
    out += param.json_path != nullptr ? param.json_path : "";
    out += '\x1f';
    char value[40] = {0};
    std::snprintf(value, sizeof(value), "%.4f",
                  static_cast<double>(param.type == COMP_PARAM_BOOL ? boolean : number));
    out += value;
    out += '\x1f';
    out += used_default != 0 ? "1" : "0";
  }
  return MakeString(env, out.c_str());
}

/*
 * 调整面板联动自检：applyVignette 到底改的是哪一层。
 *
 * 为什么需要这个自检：模拟器上画布上不了屏（OPT-21b），「暗角打在哪一层」
 * 光看屏幕验不了。而这个错误**在真机上极难发现**——用户选中 B 层点暗角，
 * 被改的是最上面那层 A，屏幕上看到「某个地方暗了」，很容易以为是对的。
 *
 * 判据：两层纯色（红 / 蓝），选中红层后打暗角，**红层中心像素必须变暗、
 * 蓝层必须一个字节都不动**。
 *
 * @returns "target=red ok=1 red=..->.. blue=..->.."
 */
napi_value JsAdjustTargetSelfTest(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeString(env, "no-doc");
  if (g_doc_width == 0 || g_doc_height == 0) return MakeString(env, "no-size");

  const comp_layer_id prev_active = g_active_layer;
  const size_t stride = static_cast<size_t>(g_doc_width) * 4u;

  auto make_solid = [&](uint8_t r, uint8_t g, uint8_t b, comp_layer_id* out) -> bool {
    comp_layer_desc desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.name = "adjust_probe";
    desc.opacity = 1.0f;
    if (comp_layer_add(g_document, &desc, out) != COMP_OK) return false;
    std::vector<uint8_t> pixels(stride * g_doc_height);
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      pixels[i + 0] = r;
      pixels[i + 1] = g;
      pixels[i + 2] = b;
      pixels[i + 3] = 255;
    }
    comp_image image;
    std::memset(&image, 0, sizeof(image));
    image.width = static_cast<int32_t>(g_doc_width);
    image.height = static_cast<int32_t>(g_doc_height);
    image.format = COMP_PIXEL_RGBA8;
    image.stride = stride;
    image.pixels = pixels.data();
    return comp_layer_set_pixels(g_document, *out, &image) == COMP_OK;
  };

  /*
   * 读**角落**像素的 R 通道。
   *
   * 为什么不是中心：暗角（vignette）的语义就是「中心不动、边缘压暗」——
   * 量中心的话，既看不出打没打上，也分不清是「没生效」还是「打对了」。
   * 取 (2,2) 而不是 (0,0)：最外圈可能是边界采样，往里两个像素在各尺寸下都安全。
   * 也不要取 1/8 处——实测那里暗化只有 255->253，两个数值的差判不出真假。
   */
  auto corner_red = [&](comp_layer_id id) -> int32_t {
    std::vector<uint8_t> pixels(stride * g_doc_height);
    int32_t width = 0;
    int32_t height = 0;
    comp_pixel_format format = COMP_PIXEL_RGBA8;
    if (comp_layer_get_pixels(g_document, id, pixels.data(), stride, &width, &height, &format) !=
        COMP_OK) {
      return -1;
    }
    if (width <= 0 || height <= 0) return -1;
    const size_t offset = 2u * stride + 2u * 4u;
    if (offset + 3 >= pixels.size()) return -1;
    return static_cast<int32_t>(pixels[offset]);
  };

  comp_layer_id red = 0;
  comp_layer_id blue = 0;
  if (!make_solid(255, 40, 40, &red)) return MakeString(env, "add-red-failed");
  if (!make_solid(40, 40, 255, &blue)) return MakeString(env, "add-blue-failed");

  const int32_t red_before = corner_red(red);
  const int32_t blue_before = corner_red(blue);

  /* 选中**红**层（蓝层是后建的、在更上面——打错层就会被立刻发现）。 */
  g_active_layer = red;
  comp_layer_set_active(g_document, red);

  comp_filter_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.kind = COMP_FILTER_VIGNETTE;
  /* 与 JsApplyVignette 同一口径：0..100 的百分比（域层再映射成压暗）。 */
  desc.vignette.amount = 90.0;
  desc.vignette.midpoint = 20.0;
  desc.vignette.feather = 60.0;
  const comp_layer_id target = g_active_layer != 0 ? g_active_layer : g_top_layer;
  int32_t changed = 0;
  size_t note_size = 0;
  const comp_result filter_rc =
      comp_filter_apply(g_document, target, &desc, 1.0, &changed, nullptr, 0, &note_size);

  const int32_t red_after = corner_red(red);
  const int32_t blue_after = corner_red(blue);

  /* 清理：先删探针层，再恢复原来的活动层。 */
  comp_layer_remove(g_document, blue);
  comp_layer_remove(g_document, red);
  g_active_layer = prev_active;
  if (prev_active != 0) comp_layer_set_active(g_document, prev_active);

  /*
   * 判据两条都要成立：
   *   1. 红层角落**变暗**（暗角真的作用到它）；
   *   2. 蓝层角落**一个字节没动**（没打到未选中的层）。
   */
  /* 红层角落要**变暗**，且蓝层一个字节都不动。 */
  const int32_t ok = (red_after >= 0 && red_after < red_before &&
                      blue_after == blue_before)
                         ? 1
                         : 0;
  char buffer[192];
  std::snprintf(buffer, sizeof(buffer),
                "target=red ok=%d red=%d->%d blue=%d->%d rc=%d changed=%d note=%zu", ok,
                red_before, red_after, blue_before, blue_after,
                static_cast<int>(filter_rc), changed, note_size);
  return MakeString(env, buffer);
}

// ---------------------------------------------------------------------------
// JS 导出：滤镜与撤销（vignette 在纯色上也肉眼可见，适合链路演示）
// ---------------------------------------------------------------------------

napi_value JsApplyVignette(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  double amount = 0.6;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_double(env, argv[0], &amount);
  }
  /*
   * 作用对象 = **当前选中层**（不是顶层）。
   *
   * 之前固定打顶层：用户在面板上选中某一层、点「暗角」，被改的却是最上面那层
   * ——「选中的层」与「被改的层」不是同一个，这是最糟的一类交互错误（用户会
   * 以为操作丢了，其实是打到别处去了，而且撤销一次才发现改错了层）。
   *
   * 没选任何层时退回顶层：单层文档（新建后直接试）不至于点了没反应。
   */
  const comp_layer_id target = g_active_layer != 0 ? g_active_layer : g_top_layer;
  if (g_document == nullptr || target == 0) return MakeInt32(env, -1);

  /*
   * 参数值域换算：ArkTS 侧传的是 **0..1 的强度**，而 ABI 的 vignette 参数是
   * **0..100 的百分比**（域层校验 `in_range(amount, 0, 100)`，再映射成内核的
   * 负值压暗）。
   *
   * 此前直接把 0.7 传进去 = **0.7%**，等于什么都没做——「暗角」按钮一直是
   * 空操作。而模拟器上画布上不了屏（OPT-21b），屏幕上根本看不出没生效，
   * 这个 bug 一直藏着（2026-10-27 查出来的）。
   *
   * midpoint / feather 同样是 0..100 的百分比，不是 0..1。
   */
  double strength = amount;
  if (strength < 0.0) strength = 0.0;
  if (strength > 1.0) strength = 1.0;
  comp_filter_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.kind = COMP_FILTER_VIGNETTE;
  desc.vignette.amount = strength * 100.0;
  desc.vignette.midpoint = 50.0;
  desc.vignette.feather = 50.0;
  int32_t changed = 0;
  size_t note_size = 0;
  const comp_result rc = comp_filter_apply(g_document, target, &desc, 1.0, &changed,
                                           nullptr, 0, &note_size);
  return MakeInt32(env, static_cast<int32_t>(rc));
}

napi_value JsUndo(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(comp_document_undo(g_document)));
}

napi_value JsRedo(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(comp_document_redo(g_document)));
}

napi_value JsCanUndo(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeInt32(env, 0);
  int32_t can = 0;
  comp_document_can_undo(g_document, &can);
  return MakeInt32(env, can);
}

napi_value JsCanRedo(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeInt32(env, 0);
  int32_t can = 0;
  comp_document_can_redo(g_document, &can);
  return MakeInt32(env, can);
}

// ---------------------------------------------------------------------------
// JS 导出：导出 PNG（路径由 ArkTS 传入应用沙箱 cacheDir）
// ---------------------------------------------------------------------------

napi_value JsExportPng(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  char path[1024] = {0};
  size_t copied = 0;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_string_utf8(env, argv[0], path, sizeof(path), &copied);
  }
  if (g_document == nullptr || copied == 0) return MakeInt32(env, -1);
  return MakeInt32(env, static_cast<int32_t>(
                            comp_io_export(g_document, path, COMP_FORMAT_PNG, "{}")));
}

/*
 * 导出到**固定的沙箱路径**（自动化用）。
 *
 * 为什么需要：上面那个 exportPng 要调用方给路径，而应用沙箱路径只有进程
 * 内部知道（/data/storage/el2/base/cache/...），外部脚本拿不到、也没法
 * 把它拉出来比对像素。这里固定落到一个已知路径，验证脚本用 hdc 拉回
 * 就能逐像素核对——「图片真的进引擎了」这件事因此可以自动化验证，
 * 而不是只能靠肉眼看屏幕（模拟器上屏那条路本来就不通，OPT-21b）。
 */
napi_value JsExportPngToSandbox(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_document == nullptr) return MakeInt32(env, -1);
  const char* path = "/data/storage/el2/base/cache/compositor_verify.png";
  return MakeInt32(env, static_cast<int32_t>(
                            comp_io_export(g_document, path, COMP_FORMAT_PNG, "{}")));
}

/*
 * 导入 + **像素核对**（一次性自检入口）。
 *
 * 这一条把「鸿蒙壳能打开一张真实 PNG」这件事变成可自动化的断言：
 *   1. 把一张内嵌的 8x8 PNG 落地成沙箱文件；
 *   2. comp_io_import_layer 导进当前文档；
 *   3. comp_layer_get_pixels 读回该图层的像素；
 *   4. 与**期望图案**逐字节比对（左上红 / 右下绿 / 其余蓝）。
 *
 * 为什么用非对称图案：对称图案里「转置」「镜像」这类错误会因像素相同
 * 而蒙混过关。非对称图案能查出方向错、通道错、位置错。
 *
 * 返回：0 = 全部通过；-1 = 无引擎/无文档；其它 = 失败阶段（见下）。
 *   -2 = 落地失败；-3 = 导入失败（comp_result 取负）；-4 = 读回失败；
 *   -5 = 尺寸不符；-6 = 像素不符（**错误像素数**放在 g_last_import_rc 里）。
 */
napi_value JsImportVerify(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_engine == nullptr || g_document == nullptr) {
    g_last_import_rc = -1;
    return MakeInt32(env, -1);
  }
  /* 期望图案：8x8，左上 2x2 红、右下 2x2 绿、其余蓝（全不透明）。 */
  const int32_t side = 8;
  std::vector<uint8_t> expected(static_cast<size_t>(side) * side * 4u, 0u);
  for (int32_t y = 0; y < side; ++y) {
    for (int32_t x = 0; x < side; ++x) {
      const size_t index = (static_cast<size_t>(y) * side + x) * 4u;
      if (x < side / 2 && y < side / 2) {
        expected[index + 0] = 255;
      } else if (x >= side / 2 && y >= side / 2) {
        expected[index + 1] = 255;
      } else {
        expected[index + 2] = 255;
      }
      expected[index + 3] = 255;
    }
  }
  /* 落地：8x8 PNG 的字节（与 ArkTS 侧那份同源，用固定 hex 避免两处不一致）。 */
  static const char kHex[] =
      "89504e470d0a1a0a0000000d4948445200000008000000080806000000c40fbe8b0000"
      "001b4944415478da63f8cfc0f01f19a371ff33d04101161da890f60a0073277f819c26"
      "6fe50000000049454e44ae426082";
  std::vector<uint8_t> bytes;
  bytes.reserve(sizeof(kHex) / 2u);
  int high = -1;
  for (const char* p = kHex; *p != '\0'; ++p) {
    const char c = *p;
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    }
    if (digit < 0) continue;
    if (high < 0) {
      high = digit;
    } else {
      bytes.push_back(static_cast<uint8_t>((high << 4) | digit));
      high = -1;
    }
  }
  const char* path = "/data/storage/el2/base/cache/imported_verify.png";
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    g_last_import_rc = -2;
    return MakeInt32(env, -2);
  }
  const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  if (written != bytes.size()) {
    g_last_import_rc = -2;
    return MakeInt32(env, -2);
  }
  comp_layer_id layer = 0;
  const comp_result rc = comp_io_import_layer(g_document, path, &layer);
  if (rc != COMP_OK) {
    g_last_import_rc = -static_cast<int32_t>(rc);
    return MakeInt32(env, -3);
  }
  g_top_layer = layer;

  /* 读回像素。 */
  std::vector<uint8_t> actual(static_cast<size_t>(side) * side * 4u, 0u);
  int32_t width = 0;
  int32_t height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  if (comp_layer_get_pixels(g_document, layer, actual.data(), 4u * static_cast<size_t>(side),
                            &width, &height, &format) != COMP_OK) {
    g_last_import_rc = -4;
    return MakeInt32(env, -4);
  }
  if (width != side || height != side) {
    g_last_import_rc = -5;
    return MakeInt32(env, -5);
  }
  int32_t wrong = 0;
  for (size_t index = 0; index < expected.size(); ++index) {
    if (actual[index] != expected[index]) ++wrong;
  }
  if (wrong != 0) {
    g_last_import_rc = wrong;
    return MakeInt32(env, -6);
  }
  g_last_import_rc = 0;
  return MakeInt32(env, 0);
}

// ---------------------------------------------------------------------------
// JS 导出：导入图片（P1-2 剩余项）
//
// 此前鸿蒙壳只能建纯色层，连自己的照片都打不开——「引擎很行、产品不行」
// 的典型样本（引擎 215 个 ABI 里壳只用了十来个）。
//
// 两个入口，语义与 Windows 壳一致：
//   importImage(path)         = 作为**新图层**放进当前文档（文档尺寸不变）
//   importImageAsDocument(path) = 导入为**新文档**（画布尺寸 = 图像尺寸）
//
// 前置条件（comp_io_import_layer 的契约）：进入时不得有活动事务。壳侧
// 不开显式事务，所以这条恒满足。
// ---------------------------------------------------------------------------

napi_value ImportImageImpl(napi_env env, napi_callback_info info, bool as_document) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  char path[1024] = {0};
  size_t copied = 0;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_string_utf8(env, argv[0], path, sizeof(path), &copied);
  }
  if (g_engine == nullptr || copied == 0) return MakeInt32(env, -1);
  if (as_document) {
    /* 导入为新文档：旧文档先销毁（壳只有一个文档槽位，与 newDocument 同款）。 */
    comp_document document = nullptr;
    const comp_result rc = comp_io_import(g_engine, path, &document);
    if (rc != COMP_OK) {
      LOGE("import as document failed rc=%{public}d path=%{public}s", static_cast<int>(rc), path);
      return MakeInt32(env, static_cast<int32_t>(rc));
    }
    if (g_document != nullptr) comp_document_destroy(g_document);
    g_document = document;
    comp_size size{};
    if (comp_document_size(g_document, &size) == COMP_OK) {
      g_doc_width = static_cast<uint32_t>(size.width);
      g_doc_height = static_cast<uint32_t>(size.height);
    }
    LOGI("import as document ok %{public}ux%{public}u", g_doc_width, g_doc_height);
    return MakeInt32(env, 0);
  }
  if (g_document == nullptr) return MakeInt32(env, -1);
  comp_layer_id layer = 0;
  const comp_result rc = comp_io_import_layer(g_document, path, &layer);
  if (rc != COMP_OK) {
    LOGE("import layer failed rc=%{public}d path=%{public}s", static_cast<int>(rc), path);
    return MakeInt32(env, static_cast<int32_t>(rc));
  }
  g_top_layer = layer;
  LOGI("import layer ok id=%{public}llu", static_cast<unsigned long long>(layer));
  return MakeInt32(env, 0);
}

napi_value JsImportImage(napi_env env, napi_callback_info info) {
  return ImportImageImpl(env, info, false);
}

napi_value JsImportImageAsDocument(napi_env env, napi_callback_info info) {
  return ImportImageImpl(env, info, true);
}

/*
 * 桥接自检：把「导入」这条链路的状态读出来。
 *
 * 为什么需要它：P1-2 的验收是「打开一张真实 PNG 并正确显示」，而
 * 「正确显示」在模拟器上验不了（OPT-21b 已定位：x86_64 模拟器不给
 * XComponent surface 的 CPU 可写映射，上屏那条路走不通）。
 * 所以这里把**桥接层**的状态做成可读的：导入返回码、图层数、文档尺寸、
 * 顶层图层 id —— 设备上跑一遍就能确认「桥接通了、图片进引擎了」。
 *
 * 返回格式："rc=<n>;layers=<n>;doc=<w>x<h>;top=<id>"。
 */
napi_value JsImportState(napi_env env, napi_callback_info info) {
  (void)info;
  char buffer[160];
  size_t layers = 0;
  if (g_document != nullptr) comp_layer_count(g_document, &layers);
  std::snprintf(buffer, sizeof(buffer), "layers=%zu;doc=%ux%u;top=%llu;last=%d",
                layers, g_doc_width, g_doc_height,
                static_cast<unsigned long long>(g_top_layer), g_last_import_rc);
  return MakeString(env, buffer);
}

/*
 * 从**内存字节**导入图片（不经文件路径）。
 *
 * 为什么要有这个入口：
 *   1. 引擎的 comp_io_import_layer 只认**路径**，且格式按**扩展名**识别
 *      （comp_io_format_from_path）。所以字节流必须先落盘成带正确扩展名的
 *      文件，引擎才认得出——这是引擎侧的既有契约，不该在壳里改 ABI。
 *   2. 有了「给字节就能导入」，鸿蒙侧就不必依赖文件选择器也能导入
 *      （比如从相册取到 ArrayBuffer 的场景），自动化也能驱动。
 *
 * 参数：bytes(ArrayBuffer) + format(字符串，如 "png" / "jpg")。
 * 落地路径：cacheDir/imported_bytes.<format>。
 */
/*
 * 从**十六进制字符串**导入图片（不经文件路径、不依赖二进制类型识别）。
 *
 * 为什么用 hex 字符串而不是 ArrayBuffer / Uint8Array：
 * 跨 NAPI 边界的二进制传递在 ArkTS 侧不稳定——实测 2026-10-26 模拟器上
 * `napi_is_arraybuffer` 与 `napi_is_typedarray` 对 ArkTS 传来的字节对象
 * 都返回 false，参数检查直接把导入挡下（表现为 rc=1）。
 * hex 字符串是纯文本，跨边界没有这个问题；代价是长度翻倍，但一张
 * 几 MB 的照片也就是十几 MB 的字符串，导入是一次性操作，可以接受。
 *
 * 参数：hex（十六进制字符串，可含空白）+ format（"png" / "jpg" / …）。
 * 落地路径：应用沙箱 cacheDir 下的 imported_bytes.<format>。
 */
napi_value JsImportImageBytes(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  /*
   * 失败码分段（便于在设备上**不看日志**就能定位卡在哪一步）：
   *   -1 = 参数个数不对 / 参数为空
   *   -2 = hex 文本读不出来
   *   -3 = hex 解析失败（非法字符 / 奇数长度 / 空）
   *   -4 = 临时文件写不出来
   *   -5 = 落地成功但引擎导入失败（返回真正的 comp_result 取负偏移）
   *   -6 = 无引擎 / 无文档
   * 成功 = 0。
   * 设备上那行小字直接显示这个码，比翻 hilog 快得多（实测 hilog 在模拟器上
   * 抓不到 app 私有 tag）。
   */
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) {
    g_last_import_rc = -1;
    return MakeInt32(env, -1);
  }

  /* 取 hex 文本。容量按 4 MB 原始数据算（hex 翻倍 + 空白）。 */
  const size_t kHexCapacity = 16u * 1024u * 1024u;
  std::vector<char> hex(kHexCapacity);
  size_t hex_length = 0;
  napi_get_value_string_utf8(env, argv[0], hex.data(), hex.size(), &hex_length);
  if (hex_length == 0) {
    g_last_import_rc = -2;
    return MakeInt32(env, -2);
  }

  char format[16] = {0};
  size_t copied = 0;
  napi_get_value_string_utf8(env, argv[1], format, sizeof(format), &copied);
  if (copied == 0) {
    std::snprintf(format, sizeof(format), "png");
  }
  /* 只允许已知栅格扩展名：格式是按扩展名识别的，乱写会得到「未知格式」。 */
  const std::string ext(format);
  const bool known = ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
                     ext == "webp" || ext == "tga" || ext == "psd";
  if (!known) {
    g_last_import_rc = -1;
    return MakeInt32(env, -1);
  }

  /* hex -> 字节。奇数位长度直接拒绝（截一半的字节流是坏数据）。 */
  std::vector<uint8_t> bytes;
  bytes.reserve(hex_length / 2u);
  int high = -1;
  for (size_t index = 0; index < hex_length; ++index) {
    const char c = hex[index];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',') continue;
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    }
    if (digit < 0) {
      g_last_import_rc = -3;
      return MakeInt32(env, -3);
    }
    if (high < 0) {
      high = digit;
    } else {
      bytes.push_back(static_cast<uint8_t>((high << 4) | digit));
      high = -1;
    }
  }
  if (high >= 0 || bytes.empty()) {
    g_last_import_rc = -3;
    return MakeInt32(env, -3);
  }

  /* 落地：应用沙箱里的固定文件名（引擎按扩展名判格式，扩展名要保留）。 */
  char path[512] = {0};
  std::snprintf(path, sizeof(path), "/data/storage/el2/base/cache/imported_bytes.%s", ext.c_str());
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    g_last_import_rc = -4;
    return MakeInt32(env, -4);
  }
  const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  if (written != bytes.size()) {
    g_last_import_rc = -4;
    return MakeInt32(env, -4);
  }

  if (g_engine == nullptr || g_document == nullptr) {
    g_last_import_rc = -6;
    return MakeInt32(env, -6);
  }
  comp_layer_id layer = 0;
  const comp_result rc = comp_io_import_layer(g_document, path, &layer);
  if (rc != COMP_OK) {
    /* 引擎的失败码（1..N）原样回传：设备上能直接看到 COMP_ERR_IO 之类。 */
    g_last_import_rc = static_cast<int32_t>(rc);
    return MakeInt32(env, static_cast<int32_t>(rc));
  }
  g_top_layer = layer;
  g_last_import_rc = 0;
  return MakeInt32(env, 0);
}

// ---------------------------------------------------------------------------
// JS 导出：呈现面几何 + CPU 上屏（OPT-21）
//
// present()：取 XComponent 的 OHNativeWindow → 设 RGBA_8888/几何 → RequestBuffer
// → 等 fence → comp_render_flatten 直写 BufferHandle（按窗口 stride，行间可能有
// padding，flatten 的 stride 参数天然支持）→ FlushBuffer 提交。
// 引擎零改动；Vulkan 交换链（OPT-24）落地后可切 GPU 路径。
// 返回：0 = OK；-1 = 引擎/文档/表面缺失；1 = 参数错；6 = 后端/内部错误。
// ---------------------------------------------------------------------------

void WaitFence(int32_t fd) {
  if (fd < 0) return;
  struct pollfd waiter;
  waiter.fd = fd;
  waiter.events = POLLIN;
  waiter.revents = 0;
  poll(&waiter, 1, 100);
}

napi_value JsSurfaceInfo(napi_env env, napi_callback_info info) {
  (void)info;
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%llux%llu|%s",
                static_cast<unsigned long long>(g_surface_width),
                static_cast<unsigned long long>(g_surface_height),
                g_surface_ready ? "ready" : "absent");
  return MakeString(env, buffer);
}

// ---------------------------------------------------------------------------
// CPU 上屏（OPT-21）：异步工作线程版。
//
// 2026-10-25 首版在 JS 主线程同步调用，实测 NativeWindowRequestBuffer 可在
// 缓冲队列配置协商期无限阻塞 → 主线程 THREAD_BLOCK_6S 被系统看门狗判死
// （appfreeze 日志实锤）。改为：present() 只投递任务立即返回；真正的窗口调用
// 在一次性工作线程执行，结果经 presentResult() 供下一次刷新读取。
// 几何/格式/usage 只在尺寸变化时下发——每次 present 都 SET_* 会反复触发缓冲
// 队列重配，正是阻塞的高危路径。
// 已知残余：surface 销毁竞态（工作线程持 window 指针期间 OnSurfaceDestroyed）
// 用代计数尽量拦截，完整方案要等引擎侧 surface 代际保护（OPT-21 备注）。
// ---------------------------------------------------------------------------

std::mutex g_present_mutex;
bool g_present_running = false;
std::atomic<int32_t> g_present_last_rc{-100};
std::atomic<int> g_present_stage{0};  /* 诊断：0=未跑 1=SET_FORMAT 2=SET_USAGE 3=REQUEST 4=HANDLE 5=FLUSH 6=完成 */  // -100 = 尚无结果（或进行中）
char g_present_diag[384] = {0};  /* 诊断：配置读回 + RequestBuffer 结果（OPT-21b 排查） */

int32_t DoPresent(OHNativeWindow* window, comp_document document, uint32_t width,
                  uint32_t height, uint64_t generation) {
  if (g_surface_generation.load() != generation || window == nullptr) return -1;
  g_present_diag[0] = '\0';  /* 每帧重置诊断，避免无限累加 */

  /* 只在几何/尺寸变化时下发一次配置。
   *
   * SET_BUFFER_GEOMETRY 是 RequestBuffer 的**官方前置契约**（external_window.h:
   * "Before calling this interface, you need to set the width and height of
   * OHNativeWindow through SET_BUFFER_GEOMETRY"）。此前刻意不设、指望缓冲按表面
   * 原生尺寸分配——surface 2030x456 时侥幸成立，改成 2038x570 后 RequestBuffer
   * 恒定 NO_BUFFER(40601000)，即 OPT-21b（2026-10-26 模拟器实测）。
   *
   * 改配几何确实会触发缓冲队列与消费者的重配协商，所以严格按尺寸变化触发：
   * 尺寸不变就一帧都不多设，避免每帧协商导致的阻塞与撕裂。 */
  if (g_configured_width != width || g_configured_height != height) {
    g_present_stage.store(1);
    int rc = OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    if (rc != 0) { LOGE("SET_FORMAT rc=%{public}d", rc); return 6; }
    g_present_stage.store(2);
    rc = OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE,
                                               NATIVEBUFFER_USAGE_CPU_READ |
                                                   NATIVEBUFFER_USAGE_CPU_WRITE |
                                                   NATIVEBUFFER_USAGE_MEM_DMA);
    if (rc != 0) { LOGE("SET_USAGE rc=%{public}d", rc); return 6; }
    rc = OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, width, height);
    if (rc != 0) {
      LOGE("SET_BUFFER_GEOMETRY %{public}ux%{public}u rc=%{public}d", width, height, rc);
      return 6;
    }
    g_configured_width = width;
    g_configured_height = height;
    /* 诊断：配置后读回窗口真实几何/格式/usage，确认 SET_* 是否被接受。 */
    int32_t back_w = 0, back_h = 0, back_fmt = 0;
    uint64_t back_usage = 0;
    OH_NativeWindow_NativeWindowHandleOpt(window, GET_BUFFER_GEOMETRY, &back_w, &back_h);
    OH_NativeWindow_NativeWindowHandleOpt(window, GET_FORMAT, &back_fmt);
    OH_NativeWindow_NativeWindowHandleOpt(window, GET_USAGE, &back_usage);
    snprintf(g_present_diag, sizeof(g_present_diag),
             "cfg %ux%u->%dx%d fmt%d usage0x%llx", width, height, back_w, back_h, back_fmt,
             (unsigned long long)back_usage);
    LOGI("window configured: %{public}ux%{public}u", width, height);
  }

  NativeWindowBuffer* buffer = nullptr;
  int32_t fence_fd = -1;
  g_present_stage.store(3);
  /* RequestBuffer 在 surface 刚重建后可能短窗拒绝（40601000：consumer 尚未
   * attach 完）。小间隔重试覆盖时序窗口——恒定拒绝才算真失败。 */
  int rc = -1;
  for (int attempt = 0; attempt < 4; ++attempt) {
    rc = OH_NativeWindow_NativeWindowRequestBuffer(window, &buffer, &fence_fd);
    if (rc == 0 && buffer != nullptr) break;
    buffer = nullptr;
    usleep(50 * 1000);
  }
  if (rc != 0 || buffer == nullptr) {
    const size_t used = strlen(g_present_diag);
    if (used < sizeof(g_present_diag) - 1) {
      snprintf(g_present_diag + used, sizeof(g_present_diag) - used, " | rb=%d buf=%p",
               rc, reinterpret_cast<void*>(buffer));
    }
    return 6;
  }
  LOGI("request buffer ok, fence=%{public}d", fence_fd);

  g_present_stage.store(4);
  BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
  /* 该平台 GetBufferHandleFromNative 不给 virAddr（实测恒为 0，加 MEM_DMA 也不给）；
   * 直接 mmap(handle->fd) 也拿不到可写内存（写入不可见，读回恒 0）。
   * 唯一有效路径是官方的 OH_NativeBuffer_Map：从 OHNativeWindowBuffer 取
   * OH_NativeBuffer 再 Map（2026-10-26 模拟器逐条实测）。 */
  void* pixels = handle != nullptr ? handle->virAddr : nullptr;
  OH_NativeBuffer* mapped_buffer = nullptr;
  bool mapped_by_us = false;
  if (handle != nullptr && pixels == nullptr) {
    OH_NativeBuffer* nb = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &nb) == 0 && nb != nullptr) {
      void* addr = nullptr;
      if (OH_NativeBuffer_Map(nb, &addr) == 0 && addr != nullptr) {
        pixels = addr;
        mapped_buffer = nb;
        mapped_by_us = true;
      }
    }
  }
  if (handle == nullptr || pixels == nullptr) {
    const size_t used = strlen(g_present_diag);
    if (used < sizeof(g_present_diag) - 1) {
      snprintf(g_present_diag + used, sizeof(g_present_diag) - used,
               " | h=%p v=%p fd=%d size=%d stride=%d", reinterpret_cast<void*>(handle),
               handle != nullptr ? handle->virAddr : nullptr, handle != nullptr ? handle->fd : -1,
               handle != nullptr ? handle->size : -1, handle != nullptr ? handle->stride : -1);
    }
    /* 关键：拿到 buffer 后任何提前返回都必须归还，否则 —— 官方文档原话 ——
     * "otherwise buffer will be exhausted"。首次失败漏归还后，后续每一帧都
     * 拿不到 buffer，表象就是 RequestBuffer 永远 NO_BUFFER / 长时间等待，
     * 而失败现场已经被冲掉，极难定位（2026-10-26 模拟器实测）。 */
    OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
    return 6;
  }
  WaitFence(fence_fd);

  if (g_surface_generation.load() != generation) {  // surface 已销毁，放弃
    if (mapped_by_us) OH_NativeBuffer_Unmap(mapped_buffer);
    OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
    return -1;
  }
  const comp_result flatten_rc = comp_render_flatten(
      document, pixels, COMP_PIXEL_RGBA8, static_cast<size_t>(handle->stride));
  if (flatten_rc != 0) {
    if (mapped_by_us) OH_NativeBuffer_Unmap(mapped_buffer);
    OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
    return static_cast<int32_t>(flatten_rc);
  }
  {
    const uint8_t* probe = static_cast<const uint8_t*>(pixels);
    const size_t mid = static_cast<size_t>(handle->stride) * (handle->height / 2) +
                       static_cast<size_t>(handle->width / 2) * 4;
    const size_t used = strlen(g_present_diag);
    if (used < sizeof(g_present_diag) - 1) {
      snprintf(g_present_diag + used, sizeof(g_present_diag) - used,
               " | ok hw=%d hh=%d st=%d sz=%d map=%d px0=%02x%02x%02x%02x pxm=%02x%02x%02x%02x",
               handle->width, handle->height, handle->stride, handle->size, mapped_by_us ? 1 : 0,
               probe[0], probe[1], probe[2], probe[3], probe[mid], probe[mid + 1], probe[mid + 2],
               probe[mid + 3]);
    }
  }
  if (mapped_by_us) OH_NativeBuffer_Unmap(mapped_buffer);

  Region damage;
  Region::Rect rect;
  rect.x = 0;
  rect.y = 0;
  rect.w = static_cast<uint32_t>(handle->width);
  rect.h = static_cast<uint32_t>(handle->height);
  damage.rects = &rect;
  damage.rectNumber = 1;
  g_present_stage.store(5);
  rc = OH_NativeWindow_NativeWindowFlushBuffer(window, buffer, fence_fd, damage);
  if (rc != 0) { return 6; }
  g_present_stage.store(6);
  return 0;
}

// ---------------------------------------------------------------------------
// 触摸画笔（OPT-23）：按下开显式事务、滑动逐段落笔、抬起 commit——
// 整笔一笔 = 一个撤销步（comp_txn 的产品语义）。落点用组件坐标（= 表面原生
// 像素 = 文档像素，三方同尺寸，零换算）。抬起后自动投递一帧上屏。
// ---------------------------------------------------------------------------

/* 画笔当前设置（P1-2）：颜色 / 直径 / 硬度 / 是否橡皮擦。
 * 此前全部写死（黄色 70px 实心），用户只能画一种笔，属于「引擎全有、壳没接」。 */
float g_brush_color[3] = {255.0f, 220.0f, 40.0f};
float g_brush_diameter = 70.0f;
float g_brush_hardness = 0.85f;
bool g_brush_erase = false;
std::atomic<int> g_touch_events{0};  /* 诊断探针：回调入口计数 */
bool g_stroke_active = false;
float g_stroke_last_x = 0.0f;
float g_stroke_last_y = 0.0f;
comp_txn g_stroke_txn = nullptr;

void PaintSegment(float x0, float y0, float x1, float y1) {
  if (g_document == nullptr || g_top_layer == 0) return;
  float points[4] = {x0, y0, x1, y1};
  comp_tool_op_desc op;
  std::memset(&op, 0, sizeof(op));
  op.kind = COMP_TOOL_STROKE;  /* 冻结表 2：笔刷落笔到图层像素 */
  op.brush.color[0] = (uint8_t)g_brush_color[0];
  op.brush.color[1] = (uint8_t)g_brush_color[1];
  op.brush.color[2] = (uint8_t)g_brush_color[2];
  op.brush.diameter = g_brush_diameter;
  op.brush.pressure = 1.0f;
  op.brush.hardness = g_brush_hardness;
  op.brush.flow = 1.0f;
  op.brush.seed = 1;
  op.erase = g_brush_erase ? 1 : 0;  /* 橡皮擦：同一落笔通路，按 coverage 擦除 */
  op.stroke_points = points;
  op.stroke_point_count = 2;
  comp_tool_apply(g_document, g_top_layer, &op, nullptr, 0, nullptr);
}

void DispatchTouchEventCB(OH_NativeXComponent* component, void* window) {
  g_touch_events.fetch_add(1);
  if (component == nullptr || window == nullptr || g_document == nullptr) return;
  OH_NativeXComponent_TouchEvent event;
  if (OH_NativeXComponent_GetTouchEvent(component, window, &event) != 0) return;
  if (event.numPoints == 0) return;
  const float x = static_cast<float>(event.touchPoints[0].x);
  const float y = static_cast<float>(event.touchPoints[0].y);

  switch (event.type) {
    case OH_NATIVEXCOMPONENT_DOWN:
      if (g_stroke_txn != nullptr) return;  /* 上一笔未收尾，防重入 */
      if (comp_txn_begin(g_document, "Stroke", &g_stroke_txn) == 0 && g_stroke_txn != nullptr) {
        g_stroke_active = true;
        g_stroke_last_x = x;
        g_stroke_last_y = y;
        PaintSegment(x, y, x, y);  /* 单点也出一个圆头笔触 */
        LOGI("stroke begin (%{public}.0f,%{public}.0f)", x, y);
      }
      break;
    case OH_NATIVEXCOMPONENT_MOVE:
      if (!g_stroke_active) break;
      PaintSegment(g_stroke_last_x, g_stroke_last_y, x, y);
      g_stroke_last_x = x;
      g_stroke_last_y = y;
      break;
    case OH_NATIVEXCOMPONENT_UP:
    case OH_NATIVEXCOMPONENT_CANCEL:
      if (!g_stroke_active) break;
      if (event.type == OH_NATIVEXCOMPONENT_UP) {
        PaintSegment(g_stroke_last_x, g_stroke_last_y, x, y);
      }
      comp_txn_commit(g_stroke_txn);  /* 一笔 = 一个撤销步 */
      g_stroke_txn = nullptr;
      g_stroke_active = false;
      LOGI("stroke end, committed");
      /* 抬笔即刷屏：复用 JsPresent 的投递路径（从 JS 侧调它最省事，但这里在
       * 回调线程——直接把 present 也丢给工作线程）。 */
      {
        std::lock_guard<std::mutex> lock(g_present_mutex);
        if (!g_present_running) {
          g_present_running = true;
          OHNativeWindow* w = g_native_window;
          comp_document doc = g_document;
          const uint32_t dw = g_doc_width;
          const uint32_t dh = g_doc_height;
          const uint64_t gen = g_surface_generation.load();
          std::thread worker([w, doc, dw, dh, gen] {
            const int32_t rc = DoPresent(w, doc, dw, dh, gen);
            g_present_last_rc.store(rc);
            std::lock_guard<std::mutex> lock2(g_present_mutex);
            g_present_running = false;
          });
          worker.detach();
        }
      }
      break;
    default:
      break;
  }
}

napi_value JsPresent(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_engine == nullptr || g_document == nullptr || g_native_window == nullptr) {
    g_present_last_rc.store(-1);
    return MakeInt32(env, -1);
  }
  {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    if (g_present_running) return MakeInt32(env, -2);  // 上一帧还在路上
    g_present_running = true;
  }
  OHNativeWindow* window = g_native_window;
  comp_document document = g_document;
  const uint32_t width = g_doc_width;
  const uint32_t height = g_doc_height;
  const uint64_t generation = g_surface_generation.load();
  std::thread worker([window, document, width, height, generation] {
    const int32_t rc = DoPresent(window, document, width, height, generation);
    g_present_last_rc.store(rc);
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_present_running = false;
  });
  worker.detach();
  return MakeInt32(env, 0);  // 0 = 已提交（真正的 rc 见 presentResult()）
}

napi_value JsPresentResult(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, g_present_last_rc.load());
}

/* 诊断：上一次 present 的窗口配置读回与 RequestBuffer 原始返回码。 */
napi_value JsPresentDiag(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeString(env, g_present_diag);
}

// ---------------------------------------------------------------------------
// JS 导出：离屏合成（临时文档，诊断用；保留旧签名）
// ---------------------------------------------------------------------------

napi_value JsFlatten(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[0], &width);
    napi_get_value_int32(env, argv[1], &height);
  }
  if (g_engine == nullptr || width <= 0 || height <= 0 || width > 30000 ||
      height > 30000) {
    return MakeNull(env);
  }
  comp_document_desc document_desc;
  std::memset(&document_desc, 0, sizeof(document_desc));
  document_desc.width = static_cast<uint32_t>(width);
  document_desc.height = static_cast<uint32_t>(height);
  document_desc.dpi = 72.0f;
  document_desc.color_space = "sRGB";
  comp_document document = nullptr;
  if (comp_document_create(g_engine, &document_desc, &document) != 0 ||
      document == nullptr) {
    return MakeNull(env);
  }
  const size_t stride = static_cast<size_t>(width) * 4u;
  const size_t bytes = stride * static_cast<size_t>(height);
  napi_value array_buffer = nullptr;
  void* pixels = nullptr;
  if (napi_create_arraybuffer(env, bytes, &pixels, &array_buffer) != napi_ok ||
      pixels == nullptr) {
    comp_document_destroy(document);
    return MakeNull(env);
  }
  comp_result result = comp_render_flatten(document, pixels, COMP_PIXEL_RGBA8, stride);
  comp_document_destroy(document);
  if (result != 0) return MakeNull(env);
  return array_buffer;
}

// ---------------------------------------------------------------------------
// 模块注册
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// JS 导出：画笔设置（P1-2）
//
// 同一套落笔通路（PaintSegment）读这几个全局量，所以调参不需要重开文档，
// 下一笔就生效。取值范围在 JS 侧也夹一次，这里再夹一次——壳是 ABI 的唯一
// 使用者，不该把非法值漏给引擎。
// ---------------------------------------------------------------------------

napi_value JsSetBrushColor(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 3) return MakeInt32(env, 1);
  for (int i = 0; i < 3; ++i) {
    int32_t v = 0;
    if (napi_get_value_int32(env, argv[i], &v) != napi_ok) return MakeInt32(env, 1);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    g_brush_color[i] = static_cast<float>(v);
  }
  return MakeInt32(env, 0);
}

napi_value JsSetBrushDiameter(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t v = 0;
  if (argc < 1 || napi_get_value_int32(env, argv[0], &v) != napi_ok) return MakeInt32(env, 1);
  if (v < 1) v = 1;
  if (v > 400) v = 400;
  g_brush_diameter = static_cast<float>(v);
  return MakeInt32(env, 0);
}

napi_value JsSetBrushHardness(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  double v = 0.0;
  if (argc < 1 || napi_get_value_double(env, argv[0], &v) != napi_ok) return MakeInt32(env, 1);
  if (v < 0.0) v = 0.0;
  if (v > 1.0) v = 1.0;
  g_brush_hardness = static_cast<float>(v);
  return MakeInt32(env, 0);
}

napi_value JsSetBrushErase(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool v = false;
  if (argc < 1 || napi_get_value_bool(env, argv[0], &v) != napi_ok) return MakeInt32(env, 1);
  g_brush_erase = v;
  return MakeInt32(env, 0);
}

/* 给 UI 显示用的一行摘要；不参与任何判定。 */
napi_value JsBrushInfo(napi_env env, napi_callback_info info) {
  (void)info;
  char text[128];
  snprintf(text, sizeof(text), "%s #%02X%02X%02X · %d px · 硬度 %d%%",
           g_brush_erase ? "橡皮" : "画笔", static_cast<int>(g_brush_color[0]),
           static_cast<int>(g_brush_color[1]), static_cast<int>(g_brush_color[2]),
           static_cast<int>(g_brush_diameter + 0.5),
           static_cast<int>(g_brush_hardness * 100.0f + 0.5f));
  return MakeString(env, text);
}

napi_value Init(napi_env env, napi_value exports) {
  napi_value xcomponent_object = nullptr;
  if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ,
                              &xcomponent_object) == napi_ok &&
      xcomponent_object != nullptr) {
    OH_NativeXComponent* xcomponent = nullptr;
    if (napi_unwrap(env, xcomponent_object,
                    reinterpret_cast<void**>(&xcomponent)) == napi_ok &&
        xcomponent != nullptr) {
      g_xcomponent = xcomponent;
      static OH_NativeXComponent_Callback callback;
      callback.OnSurfaceCreated = OnSurfaceCreatedCB;
      callback.OnSurfaceChanged = OnSurfaceChangedCB;
      callback.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
      callback.DispatchTouchEvent = DispatchTouchEventCB;
      OH_NativeXComponent_RegisterCallback(xcomponent, &callback);
    }
  }

  napi_property_descriptor properties[] = {
      {"startup", nullptr, JsStartup, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"shutdown", nullptr, JsShutdown, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"backendName", nullptr, JsBackendName, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"newDocument", nullptr, JsNewDocument, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"closeDocument", nullptr, JsCloseDocument, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"addSolidLayer", nullptr, JsAddSolidLayer, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setTopOpacity", nullptr, JsSetTopOpacity, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setTopBlend", nullptr, JsSetTopBlend, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"layerCount", nullptr, JsLayerCount, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"blendModesText", nullptr, JsBlendModesText, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"layerBlend", nullptr, JsLayerBlend, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setLayerBlend", nullptr, JsSetLayerBlend, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"layerClipping", nullptr, JsLayerClipping, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"toggleLayerClipping", nullptr, JsToggleLayerClipping, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setLayerVisible", nullptr, JsSetLayerVisible, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setLayerOpacity", nullptr, JsSetLayerOpacity, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"removeLayer", nullptr, JsRemoveLayer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setLayerName", nullptr, JsSetLayerName, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"layerTreeText", nullptr, JsLayerTreeText, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"createGroup", nullptr, JsCreateGroup, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"moveLayer", nullptr, JsMoveLayer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setActiveLayer", nullptr, JsSetActiveLayer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"activeLayer", nullptr, JsActiveLayer, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"layerPanelSelfTest", nullptr, JsLayerPanelSelfTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"adjustTargetSelfTest", nullptr, JsAdjustTargetSelfTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"adjustKindsText", nullptr, JsAdjustKindsText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"adjustParamsText", nullptr, JsAdjustParamsText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"layerAdjustmentsText", nullptr, JsLayerAdjustmentsText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"addLayerAdjustment", nullptr, JsAddLayerAdjustment, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"updateLayerAdjustmentAt", nullptr, JsUpdateLayerAdjustmentAt, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"removeLayerAdjustment", nullptr, JsRemoveLayerAdjustment, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"layerAdjustmentValuesText", nullptr, JsLayerAdjustmentValuesText, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"applyVignette", nullptr, JsApplyVignette, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"undo", nullptr, JsUndo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"redo", nullptr, JsRedo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"canUndo", nullptr, JsCanUndo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"canRedo", nullptr, JsCanRedo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"exportPng", nullptr, JsExportPng, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"exportPngToSandbox", nullptr, JsExportPngToSandbox, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"importVerify", nullptr, JsImportVerify, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"importImage", nullptr, JsImportImage, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"importImageAsDocument", nullptr, JsImportImageAsDocument, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"importState", nullptr, JsImportState, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"importImageBytes", nullptr, JsImportImageBytes, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"present", nullptr, JsPresent, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"presentResult", nullptr, JsPresentResult, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"presentDiag", nullptr, JsPresentDiag, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setBrushColor", nullptr, JsSetBrushColor, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setBrushDiameter", nullptr, JsSetBrushDiameter, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setBrushHardness", nullptr, JsSetBrushHardness, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setBrushErase", nullptr, JsSetBrushErase, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"brushInfo", nullptr, JsBrushInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"presentStage", nullptr, [](napi_env env, napi_callback_info info) {
        (void)info;
        napi_value out = nullptr;
        napi_create_int32(env, g_present_stage.load(), &out);
        return out;
      }, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"touchCount", nullptr, [](napi_env env, napi_callback_info info) {
        (void)info;
        napi_value out = nullptr;
        napi_create_int32(env, g_touch_events.load(), &out);
        return out;
      }, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"flatten", nullptr, JsFlatten, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"surfaceInfo", nullptr, JsSurfaceInfo, nullptr, nullptr, nullptr, napi_default,
       nullptr},
  };
  napi_define_properties(env, exports,
                         sizeof(properties) / sizeof(properties[0]),
                         properties);
  return exports;
}

napi_module g_compositor_module = {
    1,
    0,
    nullptr,
    Init,
    "compositor",
    nullptr,
    {nullptr, nullptr, nullptr, nullptr},
};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterCompositorOHOSModule() {
  napi_module_register(&g_compositor_module);
}
