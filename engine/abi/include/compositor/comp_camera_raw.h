// AI生成
/*
 * comp_camera_raw.h — Camera Raw（F4）的 C ABI。
 *
 * 语义唯一真源见 `domain/camera_raw.h` 的文件头：设置结构、JSON、曲线与管线。
 * 本头文件只暴露外壳需要的三件事：
 *
 *   comp_engine_load_camera_raw_tables   引擎级加载实测表（CRT4）
 *   comp_engine_camera_raw_tables_loaded 查询是否已加载
 *   comp_camera_raw_apply                解析设置并对一个图层跑完整管线，**一步撤销**
 *
 * 表是跨文档共享的只读资源：一个引擎加载一次，所有文档复用；加载失败时
 * 旧表保持不动。设置 JSON 用上游 Codable 的字段名（camelCase），完整清单与
 * 取值范围见 `docs/camera-raw.md`。
 *
 * 错误码：
 *   settings_json 为 NULL / 非 UTF-8 JSON / 未知键 / 类型不符 / 标量越界
 *       -> COMP_ERR_INVALID_ARGUMENT（解析器拒绝，不静默 clamp）
 *   未加载表 -> COMP_ERR_STATE
 *   非默认几何（Upright / 投影 / 变换） -> COMP_ERR_UNSUPPORTED
 *   图层不存在 -> COMP_ERR_NOT_FOUND；组图层或 HDR 图层 -> COMP_ERR_STATE
 * 恒等设置（全部字段取上游缺省）不动像素、不开事务：out_changed = 0。
 */
#ifndef COMPOSITOR_COMP_CAMERA_RAW_H
#define COMPOSITOR_COMP_CAMERA_RAW_H

#include <stddef.h>
#include <stdint.h>

#include "comp_core.h"
#include "comp_layer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 加载 Camera Raw 实测表（CRT4 容器：10 字节头 + zlib 流）。`data` 只需在
 * 本次调用期间有效——解码结果由引擎持有；失败时引擎原有的表保持不变。
 *
 * 头部/压缩流/解压长度任一不符 -> COMP_ERR_DECODE，last_error 写明原因。
 * data 为 NULL -> COMP_ERR_INVALID_ARGUMENT；本构建没有 zlib -> COMP_ERR_UNSUPPORTED。
 */
COMP_API comp_result comp_engine_load_camera_raw_tables(comp_engine engine, const void* data,
                                                        size_t size);

/* 是否已成功加载过表：out_loaded 写 1/0。只读，不改引擎状态。 */
COMP_API comp_result comp_engine_camera_raw_tables_loaded(comp_engine engine, int32_t* out_loaded);

/*
 * 对目标图层跑 Camera Raw（上游 `CameraRawSettings.apply` 的像素部分）。
 *
 * `settings_json` 是 UTF-8 JSON 对象；空串等价 "{}"（恒等设置）。`scale` 是
 * 预览像素 / 图层像素（<= 0 视为 1，用于颗粒与半径的缩放），`seed` 供颗粒定相。
 * 16 位图层先落 8 位工作副本（与工具层同一约定）；HDR 图层拒绝。
 *
 * note 可为 NULL；非 NULL 时写入 UTF-8 非致命说明（走哪条通路），两段式：
 * note_capacity 为 0 时只回报所需长度，out_note_size 写入含 '\0' 的长度。
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_camera_raw_apply(comp_document document, comp_layer_id target,
                                           const char* settings_json, double scale, uint32_t seed,
                                           int32_t* out_changed, char* note, size_t note_capacity,
                                           size_t* out_note_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_CAMERA_RAW_H */
