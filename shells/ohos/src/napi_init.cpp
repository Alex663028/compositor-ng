// AI生成
//
// shells/ohos/src/napi_init.cpp — 鸿蒙外壳（M4）的 NAPI 桥接层。
//
// 层次边界（与 compositor-ng 七层一致）：
//   本文件属于「平台外壳层」，只允许 include C ABI 头（compositor/comp_*.h）
//   与 OHOS NDK 头；不得 include engine 内部头，不得在壳内做像素处理。
//   所有图像能力一律经 comp_* ABI 下沉到引擎核。
//
// 桥接职责：
//   1. 引擎生命周期：startup(backend) / shutdown() / backendName()
//   2. 离屏合成：flatten(w, h) → ArrayBuffer（RGBA8），走 comp_render_flatten
//   3. 呈现面：XComponent 的 surface 回调（OHNativeWindow）几何采集，暴露 surfaceInfo()
//   4. 上屏接线：presentSurface(w, h) 构造 COMP_SURFACE_HARMONY_WINDOW 渲染目标，
//      经 comp_render_document 把合成结果提交给设备侧表面
//
// 诚实边界：present 的 ABI 路径已完整接线（surface 回调 → 目标构造 → comp_render_document），
// 但设备侧真正上屏依赖 Vulkan 交换链（vkCreateSwapchainKHR / vkQueuePresentKHR，尚未落地），
// 故 presentSurface 当前会如实返回 COMP_ERR_UNSUPPORTED，而非伪造上屏成功。
// 一旦 Vulkan 交换链落地，本桥无需改动即可获得真实上屏。

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <napi/native_api.h>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <native_window/external_window.h>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"

namespace {

// ---------------------------------------------------------------------------
// 模块级状态
//
// 一个 ArkTS 进程对应一个外壳实例，引擎句柄与呈现面在此单例保存。
// 句柄类型来自 ABI 冻结面（comp_engine_handle / comp_document_handle）。
// ---------------------------------------------------------------------------

comp_engine g_engine = nullptr;

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

// ---------------------------------------------------------------------------
// XComponent 回调
//
// 旧式 native XComponent 接口：window 参数即 OHNativeWindow*，
// 无需额外 getter，直接持有并转交后续呈现路径。
// ---------------------------------------------------------------------------

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
  RefreshSurfaceGeometry(component, window);
  std::fprintf(stderr, "[compositor-ohos] surface created %llux%llu\n",
               static_cast<unsigned long long>(g_surface_width),
               static_cast<unsigned long long>(g_surface_height));
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
  RefreshSurfaceGeometry(component, window);
  std::fprintf(stderr, "[compositor-ohos] surface changed %llux%llu\n",
               static_cast<unsigned long long>(g_surface_width),
               static_cast<unsigned long long>(g_surface_height));
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* window) {
  (void)component;
  (void)window;
  g_native_window = nullptr;
  g_surface_ready = false;
  std::fprintf(stderr, "[compositor-ohos] surface destroyed\n");
}

void DispatchTouchEventCB(OH_NativeXComponent* component, void* window) {
  // 触摸事件当前不上报给引擎（输入层未接线），保留空实现以满足回调表完整性。
  (void)component;
  (void)window;
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

// ---------------------------------------------------------------------------
// JS 导出：引擎生命周期
// ---------------------------------------------------------------------------

napi_value JsStartup(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t requested = 0;  // comp_backend 0 = AUTO（冻结表）
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_int32(env, argv[0], &requested);
  }

  if (g_engine != nullptr) {
    comp_engine_destroy(g_engine);
    g_engine = nullptr;
  }

  comp_engine_desc desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.app_name = "Compositor OHOS";
  desc.version = "0.4.0";
  desc.backend = static_cast<comp_backend>(requested);
  desc.worker_threads = 0;  // 0 = 引擎自选（band 并行默认不下压线程）

  comp_result result = comp_engine_create(&desc, &g_engine);
  if (result != 0) {
    std::fprintf(stderr, "[compositor-ohos] engine create failed: %d\n", result);
    g_engine = nullptr;
  }
  return MakeInt32(env, static_cast<int32_t>(result));
}

napi_value JsShutdown(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_engine != nullptr) {
    comp_engine_destroy(g_engine);
    g_engine = nullptr;
  }
  return MakeInt32(env, 0);
}

napi_value JsBackendName(napi_env env, napi_callback_info info) {
  (void)info;
  if (g_engine == nullptr) {
    return MakeString(env, "None");
  }
  comp_backend actual = COMP_BACKEND_AUTO;
  if (comp_engine_backend(g_engine, &actual) != 0) {
    return MakeString(env, "Unknown");
  }
  return MakeString(env, BackendName(actual));
}

// ---------------------------------------------------------------------------
// JS 导出：离屏合成
//
// flatten(width, height) 建立临时文档并调用 comp_render_flatten，
// 参数用数值 cast 而非枚举名，避免因 ABI 常量拼写差异影响编译，
// 数值语义以 README §5 的冻结表为准（RGBA8 = 0）。
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
    napi_value invalid = nullptr;
    napi_get_null(env, &invalid);
    return invalid;
  }

  comp_document_desc document_desc;
  std::memset(&document_desc, 0, sizeof(document_desc));
  document_desc.width = static_cast<uint32_t>(width);
  document_desc.height = static_cast<uint32_t>(height);
  document_desc.dpi = 72.0f;
  document_desc.color_space = "sRGB";  // 显式 sRGB（NULL 亦可，引擎默认）

  comp_document document = nullptr;
  if (comp_document_create(g_engine, &document_desc, &document) != 0 ||
      document == nullptr) {
    napi_value invalid = nullptr;
    napi_get_null(env, &invalid);
    return invalid;
  }

  const size_t stride = static_cast<size_t>(width) * 4u;
  const size_t bytes = stride * static_cast<size_t>(height);

  napi_value array_buffer = nullptr;
  void* pixels = nullptr;
  napi_status status =
      napi_create_arraybuffer(env, bytes, &pixels, &array_buffer);
  if (status != napi_ok || pixels == nullptr) {
    comp_document_destroy(document);
    napi_value invalid = nullptr;
    napi_get_null(env, &invalid);
    return invalid;
  }

  comp_pixel_format format = COMP_PIXEL_RGBA8;
  comp_result result = comp_render_flatten(document, pixels, format, stride);
  comp_document_destroy(document);

  if (result != 0) {
    napi_value invalid = nullptr;
    napi_get_null(env, &invalid);
    return invalid;
  }
  return array_buffer;
}

// ---------------------------------------------------------------------------
// JS 导出：呈现面几何
// ---------------------------------------------------------------------------

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
// JS 导出：呈现上屏
//
// presentSurface(width, height)：把合成结果提交给当前 XComponent 的表面。
//   步骤：取 g_native_window → 建临时文档 → 构造 COMP_SURFACE_HARMONY_WINDOW 目标 →
//         调 comp_render_document（引擎按后端能力决定能否上屏）。
// 返回值（int32）：
//   >= 0  comp_result 数值（0 = OK；1 = 参数非法；4 = UNSUPPORTED，当前无后端支持上屏；
//                          6 = 后端内部错误）
//   -1    前置条件缺失（引擎未启动，或 XComponent 表面未就绪）
// 参数用数值 cast 而非枚举名，与 flatten 保持同一约定（数值语义见 README §5 冻结表）。
// ---------------------------------------------------------------------------

napi_value JsPresentSurface(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[0], &width);
    napi_get_value_int32(env, argv[1], &height);
  }

  if (g_engine == nullptr || g_native_window == nullptr) {
    return MakeInt32(env, -1);  // 无引擎或无呈现面
  }
  if (width <= 0 || height <= 0 || width > 30000 || height > 30000) {
    return MakeInt32(env, 1);  // COMP_ERR_INVALID_ARGUMENT
  }

  comp_document_desc document_desc;
  std::memset(&document_desc, 0, sizeof(document_desc));
  document_desc.width = static_cast<uint32_t>(width);
  document_desc.height = static_cast<uint32_t>(height);
  document_desc.dpi = 72.0f;
  document_desc.color_space = "sRGB";  // 显式 sRGB（NULL 亦可，引擎默认）

  comp_document document = nullptr;
  if (comp_document_create(g_engine, &document_desc, &document) != 0 ||
      document == nullptr) {
    return MakeInt32(env, 6);  // COMP_ERR_BACKEND
  }

  comp_render_target target;
  std::memset(&target, 0, sizeof(target));
  target.size = comp_size{width, height};
  target.format = COMP_PIXEL_RGBA8;
  target.kind = COMP_SURFACE_HARMONY_WINDOW;  // OHNativeWindow*
  target.native_handle = g_native_window;             // OHNativeWindow*
  target.stride = 0;
  target.pixels = nullptr;

  comp_result result = comp_render_document(document, &target);
  comp_document_destroy(document);
  return MakeInt32(env, static_cast<int32_t>(result));
}

// ---------------------------------------------------------------------------
// 模块注册
// ---------------------------------------------------------------------------

napi_value Init(napi_env env, napi_value exports) {
  // XComponent 实例由 ArkTS 侧以 libraryname="compositor" 创建后，
  // 框架会把它以 __NATIVE_XCOMPONENT_OBJ__ 属性挂在 exports 上。
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
      {"startup", nullptr, JsStartup, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"shutdown", nullptr, JsShutdown, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"backendName", nullptr, JsBackendName, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"flatten", nullptr, JsFlatten, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"presentSurface", nullptr, JsPresentSurface, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"surfaceInfo", nullptr, JsSurfaceInfo, nullptr, nullptr, nullptr,
       napi_default, nullptr},
  };
  napi_define_properties(env, exports,
                         sizeof(properties) / sizeof(properties[0]),
                         properties);
  return exports;
}

napi_module g_compositor_module = {
    1,                      // nm_version
    0,                      // nm_flags
    nullptr,                // nm_filename
    Init,                   // nm_register_func
    "compositor",           // nm_modname（须与 ArkTS libraryname 一致）
    nullptr,                // nm_priv
    {nullptr, nullptr, nullptr, nullptr},
};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterCompositorOHOSModule() {
  napi_module_register(&g_compositor_module);
}
