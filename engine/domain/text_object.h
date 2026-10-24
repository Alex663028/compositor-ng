// AI生成
/*
 * text_object.h — 可编辑文字对象（P0-6 ①）。
 *
 * 与上游 TypeLayer 的关系：上游把文字图层建成「文字对象 + 渲染结果」两层——对象
 * 是唯一真源，画布上的像素是它的栅格化产物。本仓在此之前只有一次性栅格化
 * （domain/tool_ops.h 的 `TypeParams` 注释第 1 条如实记录了这条边界），
 * 本文件起把文字对象挂到 `Layer` 上，改文本/字号不再需要撤销重画。
 *
 * 存放位置与口径：
 *   - 对象挂在 `Layer::text`（`std::optional`），**随 DocumentState 快照一起进
 *     事务/历史**，因此撤销/重做天然一致，不需要单独的历史记录类型；
 *   - 图层像素仍是「对象当前渲染结果的缓存」：合成、缩略图、合并、拼贴、GPU
 *     纹理全走既有路径，合成内核**一行不改**（这是本批刻意的取舍：先把可编辑性
 *     做对，再谈「按需栅格化」的省内存优化）；
 *   - 任何像素级写入（`comp_layer_set_pixels` / 工具算子）遇到文字图层都必须
 *     先显式栅格化，绝不静默把文字对象和手绘像素混在一起——那会让「改字号」
 *     抹掉用户的手绘笔迹。与 Photoshop 拒绝在文字图层上直接落笔同口径。
 *
 * 字体：`font_path` 是**持久化的文件路径**而不是句柄。文字对象要进 `.comp`
 * 并在重新打开后仍可编辑，运行期句柄（comp_font）在存盘之后就失效了；路径 +
 * face_index 才能跨会话还原。空路径 = 本仓内置的 5×7 点阵 ASCII 轨道
 * （domain/type_raster.h 的旧轨道），此时文本只接受 0x20..0x7E 与 '\n'。
 */
#ifndef COMPOSITOR_DOMAIN_TEXT_OBJECT_H
#define COMPOSITOR_DOMAIN_TEXT_OBJECT_H

#include <cstdint>
#include <string>

#include "domain/type_raster.h"

namespace compositor {

struct TextObject {
  /* UTF-8；'\n' 断行。上限见 type_raster.h 的 kMaxTypeCharacters。 */
  std::string text;

  /* 文本框左上角（文档像素，可带小数）。字形按这里的亚像素偏移落位。 */
  double origin_x = 0.0;
  double origin_y = 0.0;

  /* 字号/行距/字距/抗锯齿。字体轨道下 font_size 是 em 高，旧轨道是 5×7 单元高。 */
  TypeRasterStyle style{};

  /* 直通 sRGB + alpha（非预乘）；铺进图层时转预乘。 */
  uint8_t color[4] = {0, 0, 0, 255};

  /* 0..1，再乘进逐像素覆盖率。 */
  float opacity = 1.0f;

  /* 空 = 内置 5×7 点阵 ASCII；非空 = 该路径的字体 + face_index。 */
  std::string font_path;
  int32_t font_face_index = 0;

  /*
   * 字体族名（可空）。**不参与渲染**，只用于面板回显与 .comp 的可读性；
   * 真正的字体仍以 font_path + font_face_index 为准（族名在不同机器上可能
   * 指向不同文件，拿它当键会在跨机打开时静默换字体）。
   */
  std::string font_family;

  /*
   * 段落框（P0-6 ③）：0 = 按内容自动（点文字），> 0 = 段落文字框。
   *
   * 只有 `box_width > 0` 才折行；`box_height > 0` 只负责裁切（超出的行不画）。
   * 三个字段全为默认值时渲染走的是**与 P0-6 之前逐字节相同**的单次栅格化
   * 路径——折行实现不碰老路径，老工程的像素不会有任何漂移。
   */
  double box_width = 0.0;
  double box_height = 0.0;
  /* 0 = 左 / 1 = 居中 / 2 = 右；以 box_width 为基准，没有框宽时三种落点相同。 */
  int32_t align = 0;
};

}  // namespace compositor

#endif  // COMPOSITOR_DOMAIN_TEXT_OBJECT_H
