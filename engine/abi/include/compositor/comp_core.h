// AI生成
/*
 * comp_core.h — Compositor 引擎核 C ABI：基础类型、句柄、错误码与公共结构。
 *
 * 契约：
 *   1. 本头文件是唯一跨语言/跨编译器（Swift / ArkTS / C# / C++）的稳定边界；
 *      只暴露 POD 与 C 符号，禁止出现 STL 类型、类、模板、异常。
 *   2. 所有字符串一律 UTF-8 且以 '\0' 结束，所有权归调用方，除显式声明外。
 *   3. 所有函数返回 comp_result，0 表示成功；失败时不保证输出参数被写入。
 *   4. 结构体新增字段只能在尾部追加，并通过 size 或版本字段区分（M1 暂不启用）。
 */
#ifndef COMPOSITOR_COMP_CORE_H
#define COMPOSITOR_COMP_CORE_H

#include <stddef.h>
#include <stdint.h>

/* 版本：与 comp_engine_version() 返回一致 */
#define COMP_VERSION_MAJOR 1
#define COMP_VERSION_MINOR 0
#define COMP_VERSION_PATCH 0

/* 动态库导出宏：静态库构建下为空 */
#ifndef COMP_API
#  if defined(_WIN32) && defined(COMP_SHARED)
#    if defined(COMP_SHARED_BUILD)
#      define COMP_API __declspec(dllexport)
#    else
#      define COMP_API __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) && defined(COMP_SHARED)
#    define COMP_API __attribute__((visibility("default")))
#  else
#    define COMP_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 错误码 */

typedef uint32_t comp_result;

enum {
  COMP_OK                    = 0,  /* 成功 */
  COMP_ERR_INVALID_ARGUMENT  = 1,  /* 参数非法（空指针、越界、枚举非法） */
  COMP_ERR_OUT_OF_MEMORY     = 2,  /* 内存不足 */
  COMP_ERR_IO                = 3,  /* 文件/目录读写失败 */
  COMP_ERR_UNSUPPORTED       = 4,  /* 当前后端或构建不支持该能力 */
  COMP_ERR_STATE             = 5,  /* 对象状态不允许该操作 */
  COMP_ERR_BACKEND           = 6,  /* 渲染后端内部错误 */
  COMP_ERR_NOT_FOUND         = 7,  /* 指定对象不存在 */
  COMP_ERR_ALREADY_EXISTS    = 8,  /* 指定对象已存在 */
  COMP_ERR_DECODE            = 9,  /* 解码/解析失败（格式损坏或不兼容） */
  COMP_ERR_CANCELLED         = 10, /* 操作被取消 */
  COMP_ERR_INTERNAL          = 11  /* 未归类的内部错误 */
};

/* ------------------------------------------------------------ 不透明句柄 */

/*
 * 不透明句柄。结构体 tag 名刻意与 typedef 名不同（..._handle）：
 * 在 C++ 下 `typedef struct comp_engine* comp_engine;` 会与注入普通命名空间的
 * 类名 comp_engine 冲突（clang 报 typedef redefinition with different types）。
 * tag 名不属于 ABI——调用方只使用 typedef，故对 ABI 零影响。
 */
typedef struct comp_engine_handle*   comp_engine;
typedef struct comp_document_handle* comp_document;
typedef struct comp_txn_handle*      comp_txn;

/* 图层标识：0 表示无效。取值稳定，写入 .comp 后不得复用。 */
typedef uint64_t comp_layer_id;

/* ---------------------------------------------------------------- 像素 */

typedef enum comp_pixel_format {
  COMP_PIXEL_RGBA8    = 0, /* 8 位/通道，预乘 Alpha，4 字节/像素 */
  COMP_PIXEL_RGBA16   = 1, /* 16 位/通道，非预乘，8 字节/像素 */
  COMP_PIXEL_RGBA16F  = 2  /* 半精度浮点，8 字节/像素 */
} comp_pixel_format;

/* 内存布局：行优先、自上而下、stride 以字节计。 */
typedef struct comp_image {
  int32_t           width;
  int32_t           height;
  comp_pixel_format format;
  size_t            stride;
  void*             pixels;
} comp_image;

/* 8 位灰度蒙版：1 字节/像素，0 = 完全透明，255 = 完全不透明。 */
typedef struct comp_mask {
  int32_t width;
  int32_t height;
  size_t  stride;
  uint8_t* pixels;
} comp_mask;

/* ------------------------------------------------------------ 渲染后端 */

typedef enum comp_backend {
  COMP_BACKEND_AUTO    = 0, /* 按平台择优 */
  COMP_BACKEND_METAL   = 1, /* Apple（既有工程） */
  COMP_BACKEND_D3D12   = 2, /* Windows */
  COMP_BACKEND_VULKAN  = 3, /* HarmonyOS / 通用 */
  COMP_BACKEND_CPU     = 4  /* 通用 CPU 回退，始终可用 */
} comp_backend;

/* ------------------------------------------------------------ 混合模式 */

/* 与 .comp 规范、原 LayerBlendMode 一一对应；数值为 ABI 的一部分。 */
typedef enum comp_blend_mode {
  COMP_BLEND_NORMAL        = 0,
  COMP_BLEND_DARKEN        = 1,
  COMP_BLEND_MULTIPLY      = 2,
  COMP_BLEND_COLOR_BURN    = 3,
  COMP_BLEND_LINEAR_BURN   = 4,
  COMP_BLEND_LIGHTEN       = 5,
  COMP_BLEND_SCREEN        = 6,
  COMP_BLEND_COLOR_DODGE   = 7,
  COMP_BLEND_LINEAR_DODGE  = 8,
  COMP_BLEND_OVERLAY       = 9,
  COMP_BLEND_SOFT_LIGHT    = 10,
  COMP_BLEND_HARD_LIGHT    = 11,
  COMP_BLEND_VIVID_LIGHT   = 12,
  COMP_BLEND_LINEAR_LIGHT  = 13,
  COMP_BLEND_PIN_LIGHT     = 14,
  COMP_BLEND_HARD_MIX      = 15,
  COMP_BLEND_DIFFERENCE    = 16,
  COMP_BLEND_EXCLUSION     = 17,
  COMP_BLEND_SUBTRACT      = 18,
  COMP_BLEND_DIVIDE        = 19,
  COMP_BLEND_HUE           = 20,
  COMP_BLEND_SATURATION    = 21,
  COMP_BLEND_COLOR         = 22,
  COMP_BLEND_LUMINOSITY    = 23
} comp_blend_mode;

/* ------------------------------------------------------------ 采样模式 */

/*
 * 与上游 .comp manifest 的 transform.sampling 一一对应（rawValue 逐字对齐）。
 * 数值为 ABI 一部分：缺省 High quality 刻意取 0，使 comp_layer_desc 零初始化
 * 即得到上游默认值，避免调用方遗漏设置时静默退化为 Nearest。
 */
typedef enum comp_sampling {
  COMP_SAMPLING_HIGH_QUALITY = 0, /* "High quality"（缺省） */
  COMP_SAMPLING_NEAREST      = 1, /* "Nearest" */
  COMP_SAMPLING_SMOOTH       = 2  /* "Smooth" */
} comp_sampling;

/* ------------------------------------------------------------ 几何 */

typedef struct comp_rect { int32_t x, y, w, h; } comp_rect;
typedef struct comp_size { int32_t width, height; } comp_size;

/* ------------------------------------------------------------ 分配器 */

/* 调用方注入的分配器；全零表示使用引擎内置默认分配器。
 * realloc 允许为空（引擎将以 alloc+copy+free 替代）。 */
typedef struct comp_allocator {
  void* (*alloc)(size_t bytes, void* user);
  void* (*realloc)(void* p, size_t bytes, void* user);
  void  (*free)(void* p, void* user);
  void* user;
} comp_allocator;

/* ------------------------------------------------------------ 渲染目标 */

typedef enum comp_surface_kind {
  COMP_SURFACE_HWND           = 0, /* Windows HWND */
  COMP_SURFACE_METAL_LAYER    = 1, /* CAMetalLayer* */
  COMP_SURFACE_HARMONY_WINDOW = 2, /* OHNativeWindow* */
  COMP_SURFACE_CPU_BUFFER     = 3  /* 调用方提供的像素缓冲（无窗口/离屏） */
} comp_surface_kind;

typedef struct comp_render_target {
  comp_size          size;
  comp_pixel_format  format;
  comp_surface_kind  kind;
  void*              native_handle; /* 语义由 kind 决定 */
  size_t             stride;        /* 仅 CPU_BUFFER 使用，字节/行 */
  void*              pixels;        /* 仅 CPU_BUFFER 使用 */
} comp_render_target;

/* ------------------------------------------------------------ 杂项 */

/* 返回引擎核版本字符串，如 "1.0.0"。永不为空。 */
COMP_API const char* comp_version(void);

/* 返回 comp_result 的稳定英文名，如 "COMP_OK"。未知值返回 "COMP_ERR_UNKNOWN"。 */
COMP_API const char* comp_result_name(comp_result result);

/* 返回 comp_blend_mode 的稳定英文名，如 "NORMAL"。未知值返回 "UNKNOWN"。 */
COMP_API const char* comp_blend_mode_name(comp_blend_mode mode);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_CORE_H */
