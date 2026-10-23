// AI生成
/*
 * text_layer.h — 把 `TextObject` 栅格化成图层像素（P0-6 ①）。
 *
 * 分工（与 type_raster.h 一致，本模块不重复实现排版）：
 *   - 量文本框 / 产覆盖率：`type_measure_text*` / `type_rasterize*`；
 *   - 本模块只做三件事：字体解析（路径 -> FontFace，带进程级缓存）、
 *     文本框 -> 图层矩形（含亚像素原点对齐与画布外裁剪）、
 *     覆盖率 -> 预乘 RGBA8（与 tool_ops.cpp 的落墨公式逐字节同口径）。
 *
 * 为什么要有字体缓存：每次改字号都会重渲染一次，而一份 CJK 字体动辄 3–10 MB。
 * 缓存按 (path, face_index) 持有已加载的 FontFace；FontFace 自带互斥锁，可在
 * 合成/编辑线程间共享。缓存只增不减，进程退出时随静态对象析构释放。
 *
 * 失败一律**不留半成品**：`out` 只在完整成功后才被写入（先渲染到局部缓冲，
 * 最后 move 出去）。调用方（ABI）据此保证「参数/字体错误不改旧像素、不进撤销」。
 */
#ifndef COMPOSITOR_DOMAIN_TEXT_LAYER_H
#define COMPOSITOR_DOMAIN_TEXT_LAYER_H

#include <cstdint>
#include <memory>
#include <string>

#include "domain/font_face.h"
#include "domain/layer.h"
#include "domain/text_object.h"

namespace compositor {

class FontFace;

enum class TextLayerStatus {
  kOk = 0,
  kBadArgument = 1,  /* 文本为空 / 颜色全透明 / opacity 越界 / 几何非有限 */
  kBadText = 2,      /* 文本含该轨道不支持的字节（旧轨道遇 UTF-8、Tab 等） */
  kBadStyle = 3,     /* 字号/行距/字距越界或非有限 */
  kTooLarge = 4,     /* 文本过长或文本框面积超过上限 */
  kFontMissing = 5,  /* font_path 指定的字体打不开 / 不支持 */
  kOutOfCanvas = 6,  /* 文本框与画布完全不相交 */
  kInternal = 7,     /* 内存不足等未归类失败 */
};

struct TextLayerReport {
  TextLayerStatus status = TextLayerStatus::kOk;
  std::string message;
  /* status == kFontMissing 时的字体加载原因（ABI 据此区分 IO / DECODE）。 */
  FontLoadStatus font_status = FontLoadStatus::kOk;
};

/*
 * 渲染文字对象到一张**独立**的 RGBA8 缓冲（预乘、行紧排），并给出它在文档
 * 空间里的矩形（`out_bounds`，整数像素；原点取 floor，亚像素偏移保留在渲染里）。
 *
 * 契约：
 *   - 成功时 `out->width/height` == `out_bounds->w/h`，且 `out_bounds` 与画布
 *     相交（完全在画布外 -> kOutOfCanvas，不分配缓冲）；
 *   - 字体轨道缺字只占步进不画 .notdef，个数写进 `out_missing_glyphs`（可空）；
 *   - 旧轨道（font_path 为空）文本只接受 0x20..0x7E 与 '\n'，UTF-8/CJK 如实
 *     报 kBadText（与 type_raster 同口径，不假装画上了）。
 *   - 文本在画布内的部分正常渲染；落在画布外的部分仍保留在图层缓冲里
 *     （图层本就可以部分出画布），由合成阶段裁剪。
 */
TextLayerReport text_layer_render(const TextObject& text, int32_t canvas_width,
                                  int32_t canvas_height, ImageBuffer* out,
                                  comp_rect* out_bounds, int64_t* out_missing_glyphs);

/* 只校验不渲染（ABI 在开事务前用它做参数闸）。成功时 report.status == kOk。 */
void text_layer_validate(const TextObject& text, TextLayerReport* report);

/*
 * 文字图层的默认名字：第一行去掉首尾空白后取前 32 字节（按 UTF-8 边界截断，
 * 不切坏多字节字符）；第一行空白则回落到「文字图层」。
 */
std::string text_layer_default_name(const std::string& text);

/* 字体缓存（path + face_index 为键）。失败返回 nullptr，并写 *out_status / *error。 */
std::shared_ptr<FontFace> text_layer_acquire_font(const std::string& path, int32_t face_index,
                                                  FontLoadStatus* out_status, std::string* error);
/* 测试用：清空缓存（不影响已经交出去的 shared_ptr）。 */
void text_layer_clear_font_cache();

}  // namespace compositor

#endif  // COMPOSITOR_DOMAIN_TEXT_LAYER_H
