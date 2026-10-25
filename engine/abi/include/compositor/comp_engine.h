// AI生成
/*
 * comp_engine.h — 引擎生命周期与文档生命周期。
 *
 * 生命周期约定：
 *   comp_engine_create  -> 进程内通常只创建一个引擎实例（跨文档共享线程池与后端）
 *   comp_engine_destroy -> 推荐在其创建的文档全部销毁之后调用；若宿主反序
 *                          （引擎先销毁），引擎会兜底销毁在册文档，不会泄漏
 *   comp_document_*     -> 文档句柄由引擎创建，destroy 后其下所有 comp_layer_id 失效
 *
 * 句柄有效性：销毁后的句柄继续使用会得到 COMP_ERR_INVALID_ARGUMENT 而不是崩溃
 * （对象内置 magic 在析构时擦除）；重复 destroy 为无操作。
 *
 * 错误信息：comp_engine_last_error 返回调用线程的错误槽（thread_local）——
 * 多线程共用一个引擎实例时，请在同一线程内「调用 + 读错误」。
 */
#ifndef COMPOSITOR_COMP_ENGINE_H
#define COMPOSITOR_COMP_ENGINE_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 引擎 */

typedef struct comp_engine_desc {
  const char*    app_name;       /* 必填，UTF-8，可为 NULL 表示 "compositor" */
  const char*    version;        /* 可选，宿主应用版本，用于日志与诊断 */
  comp_backend   backend;        /* 期望后端；AUTO 表示按平台择优 */
  comp_allocator alloc;          /* 全零 = 默认分配器 */
  uint32_t       worker_threads; /* 0 = 按 CPU 核数 */
} comp_engine_desc;

/* 创建引擎。desc 可为 NULL（等价于全零描述 + AUTO 后端）。 */
COMP_API comp_result comp_engine_create(const comp_engine_desc* desc, comp_engine* out_engine);

/* 销毁引擎。传入 NULL 为无操作。 */
COMP_API void comp_engine_destroy(comp_engine engine);

/*
 * 最近一次错误信息（UTF-8）。返回的指针由引擎持有，在同一次「调用 → 读取」
 * 的线程内有效；错误槽是线程局部的，另一个线程的调用不会覆盖它。
 */
COMP_API const char* comp_engine_last_error(comp_engine engine);

/* 查询实际生效的后端；写入 buf（含 '\0'）。 */
COMP_API comp_result comp_engine_backend(comp_engine engine, comp_backend* out_backend);

/* 查询实际生效的工作线程数。 */
COMP_API comp_result comp_engine_threads(comp_engine engine, uint32_t* out_threads);

/* ---------------------------------------------------------------- 文档 */

typedef struct comp_document_desc {
  int32_t     width;        /* 画布宽，像素，1..30000 */
  int32_t     height;       /* 画布高，像素，1..30000 */
  float       dpi;          /* 分辨率，0 表示默认 72 */
  const char* color_space;  /* 可选，如 "sRGB"；NULL 表示默认 sRGB */
} comp_document_desc;

/* 新建空文档（单背景层），返回文档句柄。 */
COMP_API comp_result comp_document_create(comp_engine engine,
                                          const comp_document_desc* desc,
                                          comp_document* out_document);

/* 从 .comp 目录包加载文档。 */
COMP_API comp_result comp_document_load(comp_engine engine,
                                        const char* comp_path_utf8,
                                        comp_document* out_document);

/* 保存到 .comp 目录包（整体覆盖写入）。 */
COMP_API comp_result comp_document_save(comp_document document, const char* comp_path_utf8);

/* 销毁文档。传入 NULL 为无操作。 */
COMP_API void comp_document_destroy(comp_document document);

/* 画布尺寸查询与调整；缩小时超出部分被裁剪。
 *
 * set_size 是 M1 的 Canvas Size：只改画布并把图层包围盒裁回画布范围，不做
 * 内容偏移、不重采样（Crop 请用 comp_crop_apply）。尺寸真的变化时选区被丢弃
 * ——上游 Canvas Size / Crop 都重建 CanvasDocument，选区不随新坐标系走。 */
COMP_API comp_result comp_document_size(comp_document document, comp_size* out_size);
COMP_API comp_result comp_document_set_size(comp_document document, comp_size size);

/*
 * 整篇重采样到新画布尺寸（对齐上游 Image Size）：每层像素/掩版按比例重采样、
 * transform 同步缩放；旋转层光栅化后转为 identity 几何（上游同策略——非均匀
 * 缩放的旋转矩形无法用 origin/size/rotation 表示）。1x1 的 uniform 掩版保持
 * 原样。尺寸不变为无操作。选区随之丢弃（换画布 = 换坐标系，上游同口径）。
 * 效果参数（JSON 透传、求值未实现）不随尺寸缩放，这是已记录的能力边界。
 * 一步撤销。
 */
COMP_API comp_result comp_document_resize_image(comp_document document, comp_size size);

/* 文档是否包含未提交的修改。 */
COMP_API comp_result comp_document_is_dirty(comp_document document, int32_t* out_dirty);

/* 只读查询：文档所属引擎。 */
COMP_API comp_result comp_document_engine(comp_document document, comp_engine* out_engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ------------------------------------------------- ABI 版本握手（OPT-8，尾部追加）
 *
 * comp_engine_desc 只能尾部追加字段；等第一次真正加字段时再启用握手，启用本身
 * 就是破坏性变更（老宿主传的短结构体会被引擎越界读）。create2 把握手做在**新
 * 入口**上、零迁移：宿主传自己编译时的 sizeof(comp_engine_desc)，引擎校验与
 * 自身一致才放行——不一致即宿主与引擎版本错配，返回 INVALID_ARGUMENT。
 * 老的 comp_engine_create 永封当前字段面。 */
COMP_API comp_result comp_engine_create2(const comp_engine_desc* desc,
                                         size_t desc_size,
                                         comp_engine* out_engine);

#endif /* COMPOSITOR_COMP_ENGINE_H */
