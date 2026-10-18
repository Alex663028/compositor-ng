// AI生成
/*
 * comp_file.h — .comp 工程目录包读写。
 *
 * 布局（对齐上游 macOS 版的 .comp 目录包）：
 *   <project>.comp/
 *     manifest.json               格式标识 / 版本 / 画布 / 图层元数据
 *     images/<uuid>.png           图层像素（RGBA8 图层；PNG，8 位预乘 RGBA）
 *     images/<uuid>.exr           图层像素（RGBA16 / RGBA16F 图层；未压缩 scanline
 *                                 EXR，文件内为线性光、直通 alpha；RGBA16 用 FLOAT
 *                                 通道保 16 位精度，RGBA16F 用 HALF 通道）
 *     images/<uuid>.mask.png      图层蒙版（PNG，8 位灰度，可选）
 *
 * 互通约定（M2 收口，硬约束，均由测试护栏固定）：
 *   - manifest 顶层与图层记录的键名、取值域与上游 ProjectStore.swift 一致：
 *     format="com.compositor.project"、version=13、colorSpace="sRGB"、
 *     documentID / layers[].id / parentID 均为大写 UUID 文本；
 *     imageFile 只能是 "<该层 uuid>.png"（RGBA8）或 "<该层 uuid>.exr"
 *     （RGBA16/RGBA16F，格式由 enginePixelFormat 声明），
 *     maskFile 只能是 "<该层 uuid>.mask.png"。
 *   - transform 必须是上游 LayerTransform 的六键形态：
 *     {"origin":[x,y],"size":[w,h],"rotation":0,"flipX":false,"flipY":false,
 *      "sampling":"High quality"}；写成 {x,y,w,h} 会让上游解码失败。
 *   - 引擎自有的扩展字段一律加 "engine" 前缀（engineLayerID / engineEffects /
 *     engineAdjustments / engineClippingSourceID），既不与上游键冲突，也保证
 *     「本引擎 → 上游 → 本引擎」往返不丢信息。
 *
 * 显式不建模（保真边界，见 README）：上游 guides（对齐参考线）、live-mask 的
 * 启用状态、文本/形状/效果的上游语义字段——读取时忽略。
 */
#ifndef COMPOSITOR_FORMAT_COMP_FILE_H
#define COMPOSITOR_FORMAT_COMP_FILE_H

#include <cstdint>
#include <memory>
#include <string>

namespace compositor {

class Document;
class Engine;

struct ProjectInfo {
  std::string format;
  uint32_t version = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t layer_count = 0;
  float dpi = 72.0f;
  std::string color_space;
};

/* 保存文档到 .comp 目录包（整体覆盖）。
 * doc 非 const：documentID 缺失或非法时会在保存前补齐并写回文档状态，
 * 以保证「同一文档多次保存」的图层文件名（由 UUID 派生）不漂移。 */
bool save_project(Document& doc, const std::string& path, std::string* error);

/* 从 .comp 目录包加载，成功时创建 Document（engine 可为 nullptr）。
 * 出参为裸指针：文档对象经引擎分配器创建，调用方必须用 Document::destroy
 * 释放（而不是 delete）——与 comp_document_create 的分配路径保持一致。 */
bool load_project(const std::string& path, Engine* engine,
                  Document** out, std::string* error);

/* 只读 manifest 元信息，不加载像素。 */
bool probe_project(const std::string& path, ProjectInfo* info, std::string* error);

/* 引擎写入 .comp 时使用的 manifest 版本。 */
uint32_t project_manifest_version();

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_COMP_FILE_H */
