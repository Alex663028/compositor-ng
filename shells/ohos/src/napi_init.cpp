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
uint32_t g_doc_width = 0;
uint32_t g_doc_height = 0;

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

napi_value MakeNull(napi_env env) {
  napi_value out = nullptr;
  napi_get_null(env, &out);
  return out;
}

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
  return MakeInt32(env, 0);
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
  if (g_document == nullptr || g_top_layer == 0) return MakeInt32(env, -1);

  comp_filter_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.kind = COMP_FILTER_VIGNETTE;
  desc.vignette.amount = amount;
  desc.vignette.midpoint = 0.5;
  desc.vignette.feather = 0.5;
  int32_t changed = 0;
  size_t note_size = 0;
  const comp_result rc = comp_filter_apply(g_document, g_top_layer, &desc, 1.0, &changed,
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

float g_stroke_color[3] = {255.0f, 220.0f, 40.0f};  /* 黄色，深底上醒目 */
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
  op.brush.color[0] = (uint8_t)g_stroke_color[0];
  op.brush.color[1] = (uint8_t)g_stroke_color[1];
  op.brush.color[2] = (uint8_t)g_stroke_color[2];
  op.brush.diameter = 70.0f;
  op.brush.pressure = 1.0f;
  op.brush.hardness = 0.85f;
  op.brush.flow = 1.0f;
  op.brush.seed = 1;
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
      {"applyVignette", nullptr, JsApplyVignette, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"undo", nullptr, JsUndo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"redo", nullptr, JsRedo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"canUndo", nullptr, JsCanUndo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"canRedo", nullptr, JsCanRedo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"exportPng", nullptr, JsExportPng, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"present", nullptr, JsPresent, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"presentResult", nullptr, JsPresentResult, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"presentDiag", nullptr, JsPresentDiag, nullptr, nullptr, nullptr, napi_default, nullptr},
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
