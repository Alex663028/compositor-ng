// AI生成
/*
 * comp_font.h — 字体加载 C ABI（P5-1）。
 *
 * 语义唯一真源见 domain/font_face.h 的文件头。要点：
 *   - 字体句柄**独立于引擎**：可在没有 engine 的情况下加载（外壳启动时先选字体），
 *     失败原因由返回码表达；没有 engine 也就没有 last_error 文本。
 *   - 路径是 UTF-8；引擎自己读文件后交给 FreeType 的内存入口，Windows 上含中文/
 *     空格的路径不会打不开。
 *   - 支持 .ttf / .otf / .ttc；不支持 gzip 压缩字体与 WOFF/WOFF2（本构建裁掉了
 *     zlib），这类文件如实返回 COMP_ERR_DECODE。
 *   - 度量全部是 em 比例（1.0 = 一个 em），乘字号即文档像素。
 *   - 未加载字体时，文字工具继续走内置 5×7 点阵（旧 ASCII 用例逐字节不变）。
 */
#ifndef COMPOSITOR_COMP_FONT_H
#define COMPOSITOR_COMP_FONT_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 字体名种类，配合 comp_font_name 使用。 */
typedef enum comp_font_name_kind {
  COMP_FONT_NAME_FAMILY     = 0, /* 字体族名，如 "Noto Sans SC Subset" */
  COMP_FONT_NAME_STYLE      = 1, /* 字重/样式，如 "Regular" */
  COMP_FONT_NAME_POSTSCRIPT = 2  /* PostScript 名；没有时返回空串，不返回 NULL */
} comp_font_name_kind;

/*
 * 字体度量。`scalable` / `has_cjk` 用 int32_t 而不是 C 的 bool：
 * 跨语言边界上 1 字节的 _Bool 没有稳定大小约定。
 *
 * 缩放字体（.ttf/.otf/.ttc 基本都可缩放）的 units_per_em 通常为 1000 或 2048；
 * 位图字体为 0，此时其余度量全为 0，文字排版应拒绝使用它。
 */
typedef struct comp_font_metrics {
  int32_t face_index;   /* 实际打开的 .ttc 面序号（0 起） */
  int32_t face_count;   /* 文件里总面数，单面字体为 1 */
  int32_t glyph_count;  /* 字形数（不等于字符数） */
  uint32_t units_per_em;/* 每 em 的字体单位（= em 的比例分母） */
  int32_t scalable;     /* 非 0 = 可缩放（能按任意字号渲染） */
  int32_t has_cjk;      /* 非 0 = U+4F60「你」有字形（能排中文的最低门槛） */
  double ascender;      /* em 比例，通常为正 */
  double descender;     /* em 比例，通常为负 */
  double line_gap;      /* em 比例，行间额外空隙 */
  double height;        /* em 比例，ascender - descender + line_gap */
  double max_advance;   /* em 比例，全字体最大步进宽度 */
} comp_font_metrics;

/*
 * 从文件加载。`face_index` 是 .ttc 内的面序号（0 = 第一面）。
 * 成功时 *out_font 写入新句柄（调用方负责 comp_font_release）。
 *
 * 错误码：
 *   path_utf8 为 NULL / 空串 / face_index < 0 / out_font 为 NULL
 *                                        -> COMP_ERR_INVALID_ARGUMENT
 *   文件不存在或不可读                    -> COMP_ERR_IO
 *   文件超过 100 MB                       -> COMP_ERR_INVALID_ARGUMENT
 *   不是可识别的字体（含 gzip/WOFF/WOFF2）-> COMP_ERR_DECODE
 *   .ttc 里没有该面                       -> COMP_ERR_INVALID_ARGUMENT
 *   内存不足                              -> COMP_ERR_OUT_OF_MEMORY
 *   其它                                  -> COMP_ERR_INTERNAL
 */
COMP_API comp_result comp_font_load_file(const char* path_utf8, int32_t face_index,
                                         comp_font* out_font);

/*
 * 从内存加载。引擎**拷贝**字节，返回后调用方即可释放自己的缓冲。
 * 错误码语义同 comp_font_load_file（没有文件，故不会有 COMP_ERR_IO），
 * 超过 100 MB 同样按 COMP_ERR_INVALID_ARGUMENT 拒绝。
 */
COMP_API comp_result comp_font_load_memory(const void* data, size_t size, int32_t face_index,
                                           comp_font* out_font);

/*
 * 释放句柄；NULL 是安全的空操作。
 *
 * 句柄一经释放即**不可再用**（与 comp_engine / comp_document 同一套约定）：
 * 释放后句柄指向的对象已经被 delete，再传给任何字体函数（包括第二次 release）
 * 都是宿主错误，契约外行为——不要在释放后保留或复用句柄。
 */
COMP_API void comp_font_release(comp_font font);

/*
 * 读度量。字体句柄无效 -> COMP_ERR_INVALID_ARGUMENT；out_metrics 为 NULL 同样。
 * 成功时整块写入，调用方不需要预填。
 *
 * 函数名带 get_ 是为了避开与结构体 typedef 的同名冲突（C 的普通标识符命名空间里
 * 函数和 typedef 不能同名）。
 */
COMP_API comp_result comp_font_get_metrics(comp_font font, comp_font_metrics* out_metrics);

/*
 * 读字体名（UTF-8，以 '\0' 结束）。返回的指针**归字体句柄所有**，有效期到
 * comp_font_release 为止，调用方不得释放或改写。
 * 句柄无效 / kind 未知 -> NULL；字体没有该名字 -> 空串（不是 NULL）。
 */
COMP_API const char* comp_font_name(comp_font font, comp_font_name_kind kind);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_FONT_H */
