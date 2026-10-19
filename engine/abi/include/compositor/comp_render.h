// AI生成
/*
 * comp_render.h — 渲染与合成输出。
 *
 * M1 范围：接口定稿 + CPU 后端落地（flatten 到内存）。
 * M2 范围：D3D12 / Vulkan 后端接入同一组接口，present 走各自上屏路径。
 *
 * 线程约定：渲染调用可来自任意线程；同一文档的渲染调用必须串行。
 */
#ifndef COMPOSITOR_COMP_RENDER_H
#define COMPOSITOR_COMP_RENDER_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 把整篇文档合成到给定目标。
 *   - COMP_SURFACE_CPU_BUFFER：直接写入 target->pixels（stride 为字节/行）
 *   - COMP_SURFACE_HWND：由支持窗口直呈的后端（当前是 D3D12，S16）写入窗口的
 *     DXGI 后备缓冲，需再调 comp_render_present 真正上屏；CPU 后端做不到，
 *     会返回 COMP_ERR_UNSUPPORTED，而不是假装成功或只画一半
 *   - 其他 surface：仍返回 COMP_ERR_UNSUPPORTED
 * 返回前不保证 GPU 工作已完成，需 comp_render_wait 同步。
 */
COMP_API comp_result comp_render_document(comp_document document,
                                          const comp_render_target* target);

/* 将已合成内容提交上屏（窗口后端使用）。 */
COMP_API comp_result comp_render_present(comp_document document);

/* 阻塞直到该文档的渲染工作全部完成。 */
COMP_API comp_result comp_render_wait(comp_document document);

/*
 * 合成整篇文档并输出到调用方缓冲（纯 CPU 路径，GPU 后端需回读）。
 * pixels 由调用方分配，大小需 >= height * stride。
 */
COMP_API comp_result comp_render_flatten(comp_document document,
                                         void* pixels,
                                         comp_pixel_format format,
                                         size_t stride);

/* 查询文档当前使用的渲染后端。 */
COMP_API comp_result comp_render_backend(comp_document document,
                                         comp_backend* out_backend);

/*
 * 合成单个图层（用于缩略图/图层预览）。
 * 输出尺寸取 min(canvas, max_size)，等比缩放；pixels 由调用方分配。
 */
COMP_API comp_result comp_render_layer_thumbnail(comp_document document,
                                                 comp_layer_id id,
                                                 int32_t max_size,
                                                 void* pixels,
                                                 size_t stride,
                                                 comp_size* out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_RENDER_H */
