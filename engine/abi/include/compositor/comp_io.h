// AI生成
/*
 * comp_io.h — 导入 / 导出与 .comp 工程文件。
 *
 * 格式约定（与既有 macOS 工程保持互通）：
 *   .comp 为目录包，布局对齐上游 macOS 版：
 *     manifest.json                格式标识 / 版本 / 画布 / 图层元数据
 *     images/<uuid>.png            图层像素（PNG，8 位 RGBA）
 *     images/<uuid>.mask.png       图层蒙版（PNG，8 位灰度，可选）
 *   manifest.json 的 format 字段固定为 "com.compositor.project"，version = 13；
 *   documentID / layers[].id / parentID 均为大写 UUID 文本。
 */
#ifndef COMPOSITOR_COMP_IO_H
#define COMPOSITOR_COMP_IO_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 栅格图像格式 */
typedef enum comp_image_format {
  COMP_FORMAT_UNKNOWN = 0,
  COMP_FORMAT_PNG     = 1,
  COMP_FORMAT_JPEG    = 2,
  COMP_FORMAT_TIFF    = 3,
  COMP_FORMAT_HEIF    = 4,
  COMP_FORMAT_WEBP    = 5,
  COMP_FORMAT_EXR     = 6,
  COMP_FORMAT_PSD     = 7,
  /*
   * C 方案 C3 追加：Radiance（.hdr / .pic，RGBE，线性光）。
   * 追加在末尾而不是插入中间——0..7 的取值已经写进宿主（Swift / ArkTS / C#）
   * 与 .comp 的互操作约定，重排即为破坏性变更。
   * 它与 EXR/TIFF 同属上游的「浮点来源」（ImageImporter.swift:114
   * `.floatComponents` 那一支）：解出来是线性光浮点，进 HDR 图层还是 16 位图层
   * 由 `wideAsset` 的 peak > 1.001 判决（见 comp_io_import 的说明）。
   */
  COMP_FORMAT_HDR     = 8
} comp_image_format;

/* 按扩展名推断格式；无法识别返回 COMP_FORMAT_UNKNOWN。 */
COMP_API comp_image_format comp_io_format_from_path(const char* path_utf8);

/* 把图像文件导入为新文档（画布尺寸 = 图像尺寸）。 */
COMP_API comp_result comp_io_import(comp_engine engine,
                                    const char* path_utf8,
                                    comp_document* out_document);

/*
 * 把图像文件作为新图层放入现有文档，返回新图层 id。
 *
 * 前置条件（N4，P1-4 之后确立）：进入时**不得存在活动事务**（即 comp_txn_active == 0）。
 * 本入口会自开一个**整段显式事务**：成功后恰好占 1 步撤销；失败就地 abort，
 * 不会在撤销栈留下 add + remove 的两步残影。
 * 若宿主已开显式事务，本入口返回 COMP_ERR_STATE（实测 =5），
 * last_error = "transaction already active"，且不修改文档。
 * 需要并入宿主事务时，请改用 comp_layer_add + comp_layer_set_pixels 自行组合。
 * 注：此前置条件是该入口的显式例外，与 comp_layer.h 开头「显式事务由调用方管理」
 * 的一般写操作约定不同。
 */
COMP_API comp_result comp_io_import_layer(comp_document document,
                                          const char* path_utf8,
                                          comp_layer_id* out_id);

/*
 * 合成整篇文档并导出为图像文件。
 * options_json_utf8 可为 NULL；支持键：{"quality":92}（JPEG/HEIF/WEBP）、
 * {"flatten":true}（PSD 是否拍平，M2 生效）。
 * EXR 导出走**合成表面本身**（有 HDR 图层时是 RGBA16F，8 位文档由后端扩成 half），
 * 文件为线性光、直通 alpha 的 HALF scanline；不会先降到 8 位再写。
 * 本构建不支持的格式由登记表给出具名原因（COMP_ERR_UNSUPPORTED），不写冒名文件。
 */
COMP_API comp_result comp_io_export(comp_document document,
                                    const char* path_utf8,
                                    comp_image_format format,
                                    const char* options_json_utf8);

/* --------------------------------------- 整篇文档导入 / 导出（M1 尾部追加） */

/*
 * 整篇文档语义的导入：**分层格式**读成图层树，画布尺寸 = 文件声明尺寸。
 *
 * 与 comp_io_import 的分工：
 *   - 分层格式（当前只有 PSD：8/16 位 RGB(A)、RLE/raw、图层记录、蒙版、组、剪贴）
 *     走自研 psd_codec：组、蒙版、剪贴、不透明度、混合模式全部还原成图层树；
 *   - 其余格式（PNG/JPEG/EXR/…）与 comp_io_import 完全同义（单图层 + 背景层）。
 *
 * 能力边界（**明确拒绝，绝不悄悄降级**）：
 *   PSB、CMYK/灰度/索引色彩模式、32 位通道、ZIP 压缩、图层效果（lsct 之外的
 *   无法表示的标签）、真实蒙版（-3 通道）、智能对象/文字图层 —— 一律
 *   COMP_ERR_UNSUPPORTED / COMP_ERR_IO 并给出具名原因。宁可打不开，也不能
 *   把用户的图打开成「少了几个图层」的样子。
 *
 * 失败语义与 comp_io_import 一致：读不出 / 内容损坏 -> COMP_ERR_IO；
 * 本构建不支持的变体 -> COMP_ERR_UNSUPPORTED（last_error 给出原因）。
 */
COMP_API comp_result comp_io_import_document(comp_engine engine,
                                             const char* path_utf8,
                                             comp_document* out_document);

/*
 * 整篇文档语义的导出：**分层格式**写图层树，其余格式与 comp_io_export 同义。
 *
 * PSD 写入子集（与 comp_io_import_document 对称）：
 *   - 8 位文档写 8 位通道；文档里只要有 RGBA16 图层就整体写 16 位通道（Lr16），
 *     8 位图层按 v*257 无损扩位（PSD 一个文件只有一种位深）；
 *   - 图层名 / 可见性 / 不透明度 / 混合模式 / 位置尺寸 / 蒙版 / 组（穿透或隔离）
 *     / 剪贴关系原样写出；组的混合模式与「穿透」语义走 lsct，
 *     隔离组 = 非 pass 的文件夹记录；
 *   - 合并图像（文件末尾那份预览）由合成结果填充（8 位 RGBA，16 位文档按
 *     v*257 扩展到 16 位；布局中的图层数据才是权威）。
 *
 * 无法忠实写成 PSD 的文档明确失败（COMP_ERR_UNSUPPORTED + 具名原因），不做
 * 「写出去看起来成功、读回来变了样」的事：HDR(half) 图层、文字图层、调整图层、
 * 图层效果（内核尚未求值的）、缩放/旋转/翻转的图层变换、不在下方相邻位置的
 * 剪贴来源、穿透组自带非 Normal 混合模式。
 *
 * options_json_utf8 目前不影响 PSD 输出（保留参数以便与 comp_io_export 同签名）；
 * 非 PSD 格式原样转交 comp_io_export。
 */
COMP_API comp_result comp_io_export_document(comp_document document,
                                             const char* path_utf8,
                                             comp_image_format format,
                                             const char* options_json_utf8);

/* ------------------------------------------------------------- .comp */

/* 保存/加载 .comp 工程目录包。语义与 comp_document_save/load 一致。 */
COMP_API comp_result comp_io_save_project(comp_document document, const char* comp_path_utf8);
COMP_API comp_result comp_io_load_project(comp_engine engine,
                                          const char* comp_path_utf8,
                                          comp_document* out_document);

/* 读取 .comp 的 manifest 元信息（不加载图层像素，用于快速探测）。 */
typedef struct comp_project_info {
  int32_t     width;
  int32_t     height;
  uint32_t    version;      /* manifest version */
  int32_t     layer_count;
  char        format[32];   /* 如 "com.compositor.project" */
} comp_project_info;

COMP_API comp_result comp_io_probe_project(const char* comp_path_utf8,
                                           comp_project_info* out_info);

/* ------------------------------------------------- 编解码能力查询（M5 接缝） */

/*
 * 编解码状态：
 *   COMP_CODEC_BUILTIN     = 本仓自研零依赖实现，import/export 可用；
 *   COMP_CODEC_UNAVAILABLE = 未实现或本机未链接第三方实现（原因见 note）；
 *   COMP_CODEC_VENDORED    = 由 vendor 在 engine/third_party 的真实库提供。
 * 该状态**只能反映事实**：未链接的格式一律不可用，不会「假装成功」。
 */
typedef enum comp_codec_state {
  COMP_CODEC_BUILTIN     = 0,
  COMP_CODEC_UNAVAILABLE = 1,
  COMP_CODEC_VENDORED    = 2
} comp_codec_state;

/*
 * 查询某格式的编解码能力与边界说明。
 *   out_state      —— 必填以外的可空出参，返回状态；
 *   note_utf8      —— 可空；两段式：先以容量 0 调用取 *out_note_size，再按该长度取文本
 *                     （长度含结尾 '\0'）；
 *   note_capacity  —— note_utf8 的字节容量；
 *   out_note_size  —— 可空；返回 note 所需字节数（含结尾 '\0'）。
 * 未登记的格式返回 COMP_ERR_NOT_FOUND，并以 COMP_CODEC_UNAVAILABLE 报告。
 */
COMP_API comp_result comp_io_codec_support(comp_image_format format,
                                           comp_codec_state* out_state,
                                           char* note_utf8,
                                           size_t note_capacity,
                                           size_t* out_note_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_IO_H */
