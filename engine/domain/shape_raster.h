// AI生成
#ifndef COMPOSITOR_DOMAIN_SHAPE_RASTER_H
#define COMPOSITOR_DOMAIN_SHAPE_RASTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "domain/path_raster.h"

namespace compositor {

/*
 * 形状工具（S10）的栅格化内核：几何 -> 覆盖率。
 *
 * 与上游 ShapeTool.swift 的关系（只复刻语义，不搬 Swift 代码）：
 *   - 三种几何：矩形（含圆角，半径夹到短边一半）、椭圆、直线；
 *   - 直线是**描边**成图（圆头端点），矩形/椭圆是**填充**成图；
 *   - 另外提供通用贝塞尔路径（路径扁平化后填充 + 可选描边），
 *     这是「形状」类工具在宿主里被复用的一般情形（上游 Pen 工具画的就是它）。
 *
 * 本模块只产 coverage，不碰图层、不碰选区、不开事务：合成（直通 sRGB +
 * source-over 预乘、选区掩码、16 位工作副本）在 domain/tool_ops.cpp 里做，
 * 与渐变/落笔族同一处口径。
 *
 * 坐标：几何是**文档像素**，origin 是图层 `effective_bounds()` 的原点；
 * 内核在图层局部坐标里做栅格化，输出的窗口坐标系也是图层局部坐标。
 * 先把原点减掉再落 float，避免 1e6 量级坐标在 float 里损失亚像素精度。
 */

/* 形状种类（数值与 ABI comp_shape_kind 一一对应，尾部追加）。 */
enum ShapeRasterKind {
  kShapeRasterRect = 0,
  kShapeRasterEllipse = 1,
  kShapeRasterLine = 2,
  kShapeRasterPath = 3,
};

/* 三次贝塞尔路径段（与 ABI comp_shape_cubic 同布局；坐标是文档像素）。 */
struct ShapeCubic {
  double start[2] = {0.0, 0.0};
  double control1[2] = {0.0, 0.0};
  double control2[2] = {0.0, 0.0};
  double end[2] = {0.0, 0.0};
  bool new_subpath = false;
};

struct ShapeGeometry {
  int32_t kind = kShapeRasterRect;
  /* rect / ellipse：对角点；line：两个端点。 */
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  /* rect 的圆角半径（文档像素，<= 0 为直角；夹到短边一半）。 */
  double corner_radius = 0.0;
  /* path 专用。 */
  const ShapeCubic* segments = nullptr;
  int32_t segment_count = 0;
  /* path 描边是否闭合（填充总是隐式闭合）。rect / ellipse 忽略，line 恒为开放。 */
  bool closed = true;
};

/* 窗口：图层局部像素坐标；fill / stroke 为 w×h 覆盖率（0..255），空 = 不画。 */
struct ShapeCoverage {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  std::vector<uint8_t> fill;
  std::vector<uint8_t> stroke;

  bool empty() const { return w <= 0 || h <= 0; }
};

enum class ShapeRasterStatus {
  kOk = 0,
  kBadGeometry = 1, /* 空路径 / 退化轮廓：调用方按参数错误处理 */
  kTooComplex = 2,  /* 扁平化后顶点超过上限：调用方按参数错误处理（不静默截断） */
  kEmpty = 3,       /* 形状不与该图层相交：成功但没有任何像素要改 */
};

struct ShapeRasterReport {
  ShapeRasterStatus status = ShapeRasterStatus::kOk;
  const char* message = "";
};

/* 扁平化后允许的总顶点数（与选区 kMaxSelectionPoints 同量级）。 */
inline constexpr size_t kMaxShapePoints = 1u << 18;

/*
 * 把几何栅格化成覆盖率窗口。
 *   - fill_enabled：矩形/椭圆/路径的填充；line 恒不填充；
 *   - stroke_enabled + stroke_width：沿全部轮廓的圆头描边（矩形/椭圆描边的是轮廓）；
 *   - antialias：4 行超采样 + 水平解析覆盖率；关闭时取像素中心一票。
 * 返回 kEmpty 时窗口为空且 out 保持空状态，宿主应当「成功且无操作」。
 */
ShapeRasterReport shape_rasterize(const ShapeGeometry& geometry, int32_t origin_x,
                                  int32_t origin_y, int32_t layer_width, int32_t layer_height,
                                  bool fill_enabled, bool stroke_enabled, double stroke_width,
                                  bool antialias, ShapeCoverage* out);

}  // namespace compositor

#endif  // COMPOSITOR_DOMAIN_SHAPE_RASTER_H
