// AI生成
#ifndef COMPOSITOR_DOMAIN_PATH_RASTER_H
#define COMPOSITOR_DOMAIN_PATH_RASTER_H

#include <cstdint>
#include <vector>

namespace compositor {

/*
 * 路径 -> 覆盖率栅格化（选区与形状工具共用的唯一实现）。
 *
 * 为什么单独抽出来：选区（S7 的 Marquee/Lasso/Pen）与形状工具（S10 的
 * Rect/Ellipse/Line/Path）要的是**同一件事**——把闭合折线填成抗锯齿覆盖率。
 * 两份复制品迟早会在容差、采样行数、填充规则上分叉，于是「椭圆选区」与
 * 「椭圆形状」在同一个像素上给出不同 coverage，而宿主完全看不出原因。
 *
 * 坐标空间由调用方决定：选区传文档像素，形状传图层局部像素。本模块不关心
 * 画布尺寸以外的语义，也不碰蒙版/图层的任何状态。
 */

/* 一条闭合轮廓：x,y 交替的扁平 float 序列。 */
using PathContour = std::vector<float>;
using PathContours = std::vector<PathContour>;

/* 扁平化容差：0.1 像素的弦高误差折算到 255 级覆盖率不足 0.3 级。 */
inline constexpr double kPathFlattenTolerance = 0.1;
inline constexpr int kPathFlattenMaxDepth = 20;

struct PathBounds {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  bool any = false;
};

/* 矩形轮廓（四角，逆时针/顺时针由参数顺序决定；非零缠绕不受影响）。 */
void path_add_rect_contour(double x0, double y0, double x1, double y1, PathContours* contours);

/* 椭圆轮廓：按弦高误差取段数（32..8192 段）。 */
void path_add_ellipse_contour(double x0, double y0, double x1, double y1, PathContours* contours);

/* 圆角矩形轮廓：半径夹到 min(宽, 高) / 2；半径 <= 0 时退化成直角矩形。
 * 四角各用四段三次贝塞尔（与 CoreGraphics 的圆角矩形同口径 k=0.5522847498）。 */
void path_add_rounded_rect_contour(double x0, double y0, double x1, double y1, double radius,
                                   PathContours* contours);

/* 单段三次贝塞尔的 de Casteljau 自适应细分，把终点追加到 contour（起点由调用方压入）。
 * max_points 是硬上限：到达后只追加终点不再细分，内存有界；调用方按顶点数判定拒绝。 */
void path_flatten_cubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3,
                        double y3, int depth, size_t max_points, PathContour* contour);

/* 非零缠绕 + 4 行超采样 + 水平解析覆盖率的栅格化（抗锯齿）；非抗锯齿时取像素中心一票。 */
std::vector<uint8_t> path_rasterize_contours(const PathContours& contours, int32_t width,
                                             int32_t height, bool antialias);

/* 全部顶点的包围盒；空路径 any = false。 */
PathBounds path_contour_bounds(const PathContours& contours);

}  // namespace compositor

#endif  // COMPOSITOR_DOMAIN_PATH_RASTER_H
