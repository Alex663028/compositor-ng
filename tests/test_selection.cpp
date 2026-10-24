// AI生成
/*
 * test_selection.cpp — F1 选区几何：形状栅格化 / 叠加 / 全选反选 / 羽化扩缩。
 *
 * 判据是**端到端事实**，不是「返回 COMP_OK」：
 *   - 整像素矩形的蒙版逐像素精确（面积 = 255 × 宽 × 高），包围盒与画布裁剪正确；
 *   - add / subtract 的并集与差集落在具体像素上；
 *   - 椭圆走抗锯齿路径（端点像素是中间覆盖率），面积逼近 πr²；
 *   - 反向子路径在非零缠绕下是洞（同向自交不该互相抵消）；
 *   - 贝塞尔曲线被真正扁平化（弦外的点也被选中，不是把曲线当直线）；
 *   - 羽化只改 coverage 不改 base，按 sqrt 叠加，换形状时归零；
 *   - expand / contract 对整像素选区落在整像素边界上，外扩是圆角、被画布裁掉；
 *   - 无变化不产生撤销步；显式事务内多次选区操作只留一条撤销步，abort 回滚；
 *   - 参数闸在事务之前：所有失败调用都不留事务、不动历史。
 *
 * 本文件只使用 comp_*.h 的 C 符号，与三端外壳走同一条集成路径。
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_selection.h"
#include "compositor/comp_undo.h"
#include "test_framework.h"

namespace {

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 48;

struct Fixture {
  comp_engine engine = nullptr;
  comp_document document = nullptr;

  Fixture() {
    comp_engine_desc desc{};
    desc.app_name = "compositor-selection-tests";
    desc.version = "1.0.0";
    desc.backend = COMP_BACKEND_CPU;
    desc.worker_threads = 1;
    if (comp_engine_create(&desc, &engine) != COMP_OK) return;
    comp_document_desc doc_desc{};
    doc_desc.width = kWidth;
    doc_desc.height = kHeight;
    doc_desc.dpi = 72.0f;
    comp_document_create(engine, &doc_desc, &document);
  }

  ~Fixture() {
    if (document != nullptr) comp_document_destroy(document);
    if (engine != nullptr) comp_engine_destroy(engine);
  }
};

/* 取有效 coverage（羽化已经算进去）。无选区时返回空缓冲。 */
std::vector<uint8_t> coverage(comp_document document) {
  std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0u);
  comp_mask mask{};
  mask.width = kWidth;
  mask.height = kHeight;
  mask.stride = kWidth;
  mask.pixels = pixels.data();
  if (comp_selection_get_coverage(document, &mask) != COMP_OK) return {};
  return pixels;
}

uint8_t at(const std::vector<uint8_t>& mask, int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return 0;
  return mask[static_cast<size_t>(y) * kWidth + x];
}

long mask_sum(const std::vector<uint8_t>& mask) {
  long total = 0;
  for (uint8_t value : mask) total += value;
  return total;
}

comp_result rect(comp_document document, float x0, float y0, float x1, float y1,
                 comp_selection_combine combine, int32_t* out_changed) {
  const float points[4] = {x0, y0, x1, y1};
  return comp_selection_apply_shape(document, COMP_SELECTION_RECT, points, 2, combine, 1,
                                    out_changed);
}

int32_t active_of(comp_document document) {
  int32_t active = -1;
  if (comp_selection_get_active(document, &active) != COMP_OK) return -1;
  return active;
}

float feather_of(comp_document document) {
  float feather = -1.0f;
  if (comp_selection_get_feather(document, &feather) != COMP_OK) return -1.0f;
  return feather;
}

size_t undo_depth_of(comp_document document) {
  size_t depth = static_cast<size_t>(-1);
  if (comp_document_history_stats(document, &depth, nullptr) != COMP_OK) return 0;
  return depth;
}

/* 直线段的等价三次贝塞尔（控制点取三等分点），用于拼矩形/多边形路径。 */
comp_selection_cubic line(float x0, float y0, float x1, float y1, bool new_subpath) {
  comp_selection_cubic segment{};
  segment.start[0] = x0;
  segment.start[1] = y0;
  segment.control1[0] = x0 + (x1 - x0) / 3.0f;
  segment.control1[1] = y0 + (y1 - y0) / 3.0f;
  segment.control2[0] = x0 + (x1 - x0) * 2.0f / 3.0f;
  segment.control2[1] = y0 + (y1 - y0) * 2.0f / 3.0f;
  segment.end[0] = x1;
  segment.end[1] = y1;
  segment.new_subpath = new_subpath ? 1 : 0;
  return segment;
}

}  // namespace

COMP_TEST(selection_rect_hits_exact_pixels) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  const size_t depth_before = undo_depth_of(fixture.document);
  int32_t changed = -1;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(feather_of(fixture.document) == 0.0f);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_before + 1);

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(mask.size() == static_cast<size_t>(kWidth) * kHeight);
  COMP_CHECK(at(mask, 8, 8) == 255);
  COMP_CHECK(at(mask, 23, 19) == 255);
  COMP_CHECK(at(mask, 7, 12) == 0);
  COMP_CHECK(at(mask, 24, 12) == 0);
  COMP_CHECK(at(mask, 16, 7) == 0);
  COMP_CHECK(at(mask, 16, 20) == 0);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * 16L * 12L, "integer rect has exact area");

  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 8 && bounds.y == 8 && bounds.w == 16 && bounds.h == 12);

  /* 同值 replace：changed = 0，不产生撤销步（上游 setSelection 的等值短路）。 */
  changed = -1;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_before + 1);

  /* 一步撤销回到「无选区」，重做后蒙版逐像素一致。 */
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(active_of(fixture.document) == 0);
  COMP_CHECK(comp_document_redo(fixture.document) == COMP_OK);
  COMP_CHECK(active_of(fixture.document) == 1);
  const std::vector<uint8_t> restored = coverage(fixture.document);
  COMP_CHECK(restored.size() == mask.size() && std::equal(mask.begin(), mask.end(), restored.begin()));
}

COMP_TEST(selection_combine_add_and_subtract) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  changed = 0;
  COMP_CHECK(rect(fixture.document, 20, 12, 32, 24, COMP_SELECTION_ADD, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 9, 9) == 255);   /* 原矩形独有部分 */
  COMP_CHECK(at(mask, 30, 22) == 255); /* 新矩形独有部分 */
  COMP_CHECK(at(mask, 22, 16) == 255); /* 重叠部分 */
  COMP_CHECK(at(mask, 30, 9) == 0);
  COMP_CHECK(at(mask, 9, 22) == 0);

  /* 差集：挖掉 4×4 的方块。 */
  changed = 0;
  COMP_CHECK(rect(fixture.document, 10, 10, 14, 14, COMP_SELECTION_SUBTRACT, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 12, 12) == 0);
  COMP_CHECK(at(mask, 9, 9) == 255);
  COMP_CHECK(at(mask, 14, 12) == 255);

  /* 没有选区时 subtract 无操作（上游 guard let current = selection）。 */
  COMP_CHECK(comp_selection_deselect(fixture.document, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(rect(fixture.document, 0, 0, 8, 8, COMP_SELECTION_SUBTRACT, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(active_of(fixture.document) == 0);
}

COMP_TEST(selection_ellipse_is_antialiased) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  const float points[4] = {22.0f, 14.0f, 42.0f, 34.0f}; /* 圆心 (32,24)，半径 10 */
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_ELLIPSE, points, 2,
                                        COMP_SELECTION_REPLACE, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 32, 24) == 255); /* 圆心实心 */
  COMP_CHECK(at(mask, 21, 24) == 0);   /* 半径之外 */
  COMP_CHECK(at(mask, 22, 14) == 0);   /* 包围盒角落在圆外 */
  /* 左端点像素是部分覆盖 —— 走的是抗锯齿路径，不是硬边二值化。 */
  COMP_CHECK_MSG(at(mask, 22, 24) > 180 && at(mask, 22, 24) < 255, "left cap is partial coverage");

  const double area = static_cast<double>(mask_sum(mask)) / 255.0;
  COMP_CHECK_MSG(area > 300.0 && area < 330.0, "ellipse area tracks pi*r^2");
}

COMP_TEST(selection_path_holes_follow_nonzero_winding) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  /* 外框顺时针、内框逆时针：非零缠绕下内框是洞。 */
  std::vector<comp_selection_cubic> segments;
  segments.push_back(line(8, 8, 24, 8, true));
  segments.push_back(line(24, 8, 24, 24, false));
  segments.push_back(line(24, 24, 8, 24, false));
  segments.push_back(line(8, 24, 8, 8, false));
  segments.push_back(line(12, 12, 12, 20, true));
  segments.push_back(line(12, 20, 20, 20, false));
  segments.push_back(line(20, 20, 20, 12, false));
  segments.push_back(line(20, 12, 12, 12, false));

  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_path(fixture.document, segments.data(),
                                       static_cast<int32_t>(segments.size()),
                                       COMP_SELECTION_REPLACE, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 16, 16) == 0); /* 洞 */
  COMP_CHECK(at(mask, 10, 10) == 255);
  COMP_CHECK(at(mask, 22, 16) == 255);
  COMP_CHECK(at(mask, 16, 10) == 255);
  COMP_CHECK(at(mask, 16, 22) == 255);
  COMP_CHECK(at(mask, 7, 16) == 0);
}

COMP_TEST(selection_path_flattens_real_curves) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  /* P0(8,40) → P3(40,40)，控制点 (8,0)/(40,0)：曲线向上鼓到 y≈10。
   * 直边闭合后选区 = 曲线与 y=40 之间的区域；如果曲线被当成直线，
   * 这块区域会整个消失。 */
  comp_selection_cubic curve{};
  curve.start[0] = 8.0f;
  curve.start[1] = 40.0f;
  curve.control1[0] = 8.0f;
  curve.control1[1] = 0.0f;
  curve.control2[0] = 40.0f;
  curve.control2[1] = 0.0f;
  curve.end[0] = 40.0f;
  curve.end[1] = 40.0f;
  curve.new_subpath = 1;

  int32_t changed = 0;
  COMP_CHECK(comp_selection_apply_path(fixture.document, &curve, 1, COMP_SELECTION_REPLACE, 1,
                                       &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 24, 12) == 255); /* 弧下、远离边界 */
  COMP_CHECK(at(mask, 24, 8) == 0);    /* 弧顶（y≈10）之上 */
  COMP_CHECK(at(mask, 13, 30) == 255); /* 左侧陡峭段的下方 */
  COMP_CHECK(at(mask, 13, 20) == 255); /* y=20.5 处的左边界在 x≈11.5 */
  COMP_CHECK(at(mask, 9, 20) == 0);    /* 同一条线上、边界之外 */
  COMP_CHECK(at(mask, 24, 39) == 255); /* 贴着闭合边 */
  COMP_CHECK(at(mask, 24, 40) == 0);
  COMP_CHECK(at(mask, 7, 20) == 0); /* 曲线起点左侧 */

  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 8 && bounds.w == 32);
  COMP_CHECK(bounds.y <= 10 && bounds.y + bounds.h > 39);
}

COMP_TEST(selection_select_all_invert_deselect) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = -1;
  COMP_CHECK(comp_selection_select_all(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(mask.size() == static_cast<size_t>(kWidth) * kHeight);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * kWidth * kHeight, "select all covers the canvas");
  COMP_CHECK(comp_selection_get_bounds(fixture.document, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  changed = -1;
  COMP_CHECK(comp_selection_select_all(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);

  /* 反选「全选」= 无选区（不是显式空选区）。 */
  changed = -1;
  COMP_CHECK(comp_selection_invert(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 0);
  COMP_CHECK(coverage(fixture.document).empty());

  /* 无选区时反选无操作。 */
  changed = -1;
  COMP_CHECK(comp_selection_invert(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);

  /* 矩形 → 反选 → 再反选，逐像素回到原状。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const std::vector<uint8_t> original = coverage(fixture.document);
  COMP_CHECK(comp_selection_invert(fixture.document, &changed) == COMP_OK && changed == 1);
  mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 0, 0) == 255);
  COMP_CHECK(at(mask, 12, 12) == 0);
  COMP_CHECK(comp_selection_invert(fixture.document, &changed) == COMP_OK && changed == 1);
  const std::vector<uint8_t> round_trip = coverage(fixture.document);
  COMP_CHECK(round_trip.size() == original.size() &&
             std::equal(original.begin(), original.end(), round_trip.begin()));

  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 8 && bounds.w == 16);

  changed = -1;
  COMP_CHECK(comp_selection_deselect(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 0);
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 0 && bounds.y == 0 && bounds.w == 0 && bounds.h == 0);

  changed = -1;
  COMP_CHECK(comp_selection_deselect(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
}

COMP_TEST(selection_feather_softens_coverage_only) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 4, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 4.0f) < 1e-4f);

  const std::vector<uint8_t> soft = coverage(fixture.document);
  COMP_CHECK(at(soft, 8, 14) < 255);  /* 硬边两侧都变成中间覆盖率 */
  COMP_CHECK(at(soft, 7, 14) > 0);
  COMP_CHECK(at(soft, 7, 14) < at(soft, 8, 14));
  COMP_CHECK(at(soft, 16, 14) >= 250); /* 中心仍接近全选（σ = 2） */
  COMP_CHECK(at(soft, 0, 14) == 0);    /* 8 像素外衰减到 0 */

  /* base 没被羽化改写：包围盒按 base 的 bbox 外扩 ceil(2*feather) = 8。 */
  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 0 && bounds.y == 0 && bounds.w == 32 && bounds.h == 28);
  COMP_CHECK(comp_selection_deselect(fixture.document, &changed) == COMP_OK);

  /* 羽化按 sqrt 叠加：4 之后再来 3 → 5。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 4, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 3, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 5.0f) < 1e-4f);

  /* 任何成功的形状操作都把 feather 归零。 */
  changed = -1;
  COMP_CHECK(rect(fixture.document, 40, 8, 50, 20, COMP_SELECTION_ADD, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(feather_of(fixture.document) == 0.0f);

  /* 上限 250：叠加后钳到 250。 */
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 250, &changed) == COMP_OK);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 250.0f) < 1e-4f);
}

COMP_TEST(selection_expand_and_contract_keep_pixel_edges) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  /* 外扩 2 → 直边落在 [6,26) × [6,22)，拐角是圆角。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_EXPAND, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 6, 12) == 255);
  COMP_CHECK(at(mask, 5, 12) == 0);
  COMP_CHECK(at(mask, 25, 12) == 255);
  COMP_CHECK(at(mask, 26, 12) == 0);
  COMP_CHECK(at(mask, 12, 6) == 255);
  COMP_CHECK(at(mask, 12, 5) == 0);
  COMP_CHECK(at(mask, 12, 21) == 255);
  COMP_CHECK(at(mask, 12, 22) == 0);
  COMP_CHECK_MSG(at(mask, 6, 6) > 0 && at(mask, 6, 6) < 255, "outer corner is rounded");

  /* 内缩 2 → 直边落在 [10,22) × [10,18)。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_CONTRACT, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 10, 12) == 255);
  COMP_CHECK(at(mask, 9, 12) == 0);
  COMP_CHECK(at(mask, 21, 12) == 255);
  COMP_CHECK(at(mask, 22, 12) == 0);
  COMP_CHECK(at(mask, 12, 10) == 255);
  COMP_CHECK(at(mask, 12, 9) == 0);
  COMP_CHECK(at(mask, 12, 17) == 255);
  COMP_CHECK(at(mask, 12, 18) == 0);
  COMP_CHECK_MSG(mask_sum(mask) == 255L * 12L * 8L, "contract keeps integer edges");

  /* 内缩超过半宽 → 显式空选区：active 仍是 1，但没有像素被选中。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_CONTRACT, 500, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 0);
  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 0 && bounds.y == 0 && bounds.w == 0 && bounds.h == 0);

  /* 显式空选区上的 expand / feather 一律无操作。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_EXPAND, 5, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 3, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
}

COMP_TEST(selection_expand_is_clipped_to_canvas) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 0, 0, 16, 16, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_EXPAND, 5, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 0, 0) == 255);   /* 画布边缘不内缩（expand 只往外长） */
  COMP_CHECK(at(mask, 20, 8) == 255);  /* 16 + 5 = 21，像素 20 完整覆盖 */
  COMP_CHECK(at(mask, 21, 8) == 0);
  COMP_CHECK(at(mask, 63, 8) == 0);    /* 被画布裁掉，不会绕回来 */
  COMP_CHECK(at(mask, 8, 20) == 255);
  COMP_CHECK(at(mask, 8, 21) == 0);
}

COMP_TEST(selection_border_keeps_a_ring_on_both_sides) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  /* 边界 2 = 环带总宽 2 px（外扩 1 减内缩 1，奇数半径时外侧多 1 px）。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_BORDER, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 7, 12) == 255);   /* 原选区外一像素 */
  COMP_CHECK(at(mask, 8, 12) == 255);   /* 原选区最外一列被算进环带 */
  COMP_CHECK(at(mask, 9, 12) == 0);     /* 内缩 1 之后的部分被减掉 */
  COMP_CHECK(at(mask, 12, 12) == 0);    /* 中心必须是空的 */
  COMP_CHECK(at(mask, 22, 12) == 0);
  COMP_CHECK(at(mask, 23, 12) == 255);
  COMP_CHECK(at(mask, 24, 12) == 255);
  COMP_CHECK(at(mask, 25, 12) == 0);
  COMP_CHECK(at(mask, 12, 7) == 255);
  COMP_CHECK(at(mask, 12, 8) == 255);
  COMP_CHECK(at(mask, 12, 9) == 0);
  COMP_CHECK(at(mask, 12, 19) == 255);
  COMP_CHECK(at(mask, 12, 20) == 255);
  COMP_CHECK_MSG(at(mask, 7, 7) > 0 && at(mask, 7, 7) < 255, "border corner is antialiased");
  /* 边界 1 = 只保留外沿 1 px（内缩为 0，不碰原选区）。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_BORDER, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 7, 12) == 255);
  COMP_CHECK(at(mask, 8, 12) == 0);
  COMP_CHECK(at(mask, 24, 12) == 255);
  COMP_CHECK(at(mask, 23, 12) == 0);
  COMP_CHECK(at(mask, 12, 12) == 0);

  /* 边界 3 = 外沿 2 px + 内沿 1 px。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_BORDER, 3, &changed) == COMP_OK);
  mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 6, 12) == 255);
  COMP_CHECK(at(mask, 5, 12) == 0);
  COMP_CHECK(at(mask, 8, 12) == 255);
  COMP_CHECK(at(mask, 9, 12) == 0);
  COMP_CHECK(at(mask, 10, 12) == 0);

  /* 边界不动羽化半径（与扩缩同口径）。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 4, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_BORDER, 2, &changed) == COMP_OK);
  COMP_CHECK(feather_of(fixture.document) == 4.0f);

  /* 越界半径与无选区：参数闸在引擎里，且不产生撤销步。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_BORDER, 501, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(changed == -1);
  COMP_CHECK(comp_selection_deselect(fixture.document, nullptr) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_BORDER, 3, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
}

COMP_TEST(selection_smooth_rounds_corners_and_keeps_straight_edges) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const std::vector<uint8_t> before = coverage(fixture.document);

  /* 规整矩形做平滑：直边逐像素保留，四个直角被磨圆（开运算的必然结果，
   * 这正是「平滑」要找的东西——不是把直边也糊掉）。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_SMOOTH, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> rounded = coverage(fixture.document);
  COMP_CHECK(at(rounded, 8, 12) == 255);
  COMP_CHECK(at(rounded, 23, 12) == 255);
  COMP_CHECK(at(rounded, 12, 8) == 255);
  COMP_CHECK(at(rounded, 12, 19) == 255);
  COMP_CHECK(at(rounded, 12, 12) == 255);
  COMP_CHECK_MSG(at(rounded, 8, 8) < 255, "convex corner is rounded");
  COMP_CHECK(mask_sum(rounded) < mask_sum(before));

  /* 在规整矩形顶上长一根 2 px 宽的刺（x 12..13, y 4..7）。 */
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const float spike[8] = {12.0f, 4.0f, 14.0f, 4.0f, 14.0f, 8.0f, 12.0f, 8.0f};
  changed = 0;
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_POLYGON, spike, 4,
                                       COMP_SELECTION_ADD, 0, &changed) == COMP_OK);
  std::vector<uint8_t> spiked = coverage(fixture.document);
  COMP_CHECK(at(spiked, 12, 5) == 255);
  COMP_CHECK(at(spiked, 13, 5) == 255);

  /* 半径 2 的开运算削掉比 4 px 窄的凸起，闭运算把直边还原。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_SMOOTH, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> smoothed = coverage(fixture.document);
  COMP_CHECK(at(smoothed, 12, 5) == 0);
  COMP_CHECK(at(smoothed, 13, 5) == 0);
  COMP_CHECK(at(smoothed, 12, 4) == 0);
  COMP_CHECK_MSG(at(smoothed, 8, 12) == 255 && at(smoothed, 23, 12) == 255 &&
                     at(smoothed, 12, 8) == 255 && at(smoothed, 12, 19) == 255,
                 "smooth must keep straight edges byte-identical");
  COMP_CHECK(at(smoothed, 12, 12) == 255);
  COMP_CHECK(mask_sum(smoothed) < mask_sum(spiked));

  /* 平滑半径上限 100（PS 同量程）。 */
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_SMOOTH, 101, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(changed == -1);

  /* 无选区时无操作。 */
  COMP_CHECK(comp_selection_deselect(fixture.document, nullptr) == COMP_OK);
  changed = -1;
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_SMOOTH, 2, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
}

COMP_TEST(selection_rejects_bad_arguments) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const size_t depth = undo_depth_of(fixture.document);

  const float points[4] = {0.0f, 0.0f, 10.0f, 10.0f};
  const float three_points[6] = {0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f};
  const float nan_points[4] = {0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 10.0f};

  COMP_CHECK(comp_selection_apply_shape(fixture.document, static_cast<comp_selection_shape>(9),
                                        points, 2, COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_RECT, points, 2,
                                        static_cast<comp_selection_combine>(9), 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_RECT, nullptr, 2,
                                        COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_RECT, points, 0,
                                        COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_RECT, three_points, 3,
                                        COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_POLYGON, points, 2,
                                        COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_RECT, nan_points, 2,
                                        COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_path(fixture.document, nullptr, 1, COMP_SELECTION_REPLACE, 1,
                                       &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_modify(fixture.document, static_cast<comp_selection_modify_op>(7), 4,
                                   &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 0,
                                   &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 251,
                                   &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_EXPAND, 501,
                                   &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_get_active(fixture.document, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_get_feather(fixture.document, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_get_bounds(fixture.document, nullptr) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_get_coverage(fixture.document, nullptr) == COMP_ERR_INVALID_ARGUMENT);

  /* 无效句柄：全部在 guard 处拒绝。 */
  COMP_CHECK(comp_selection_select_all(nullptr, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_deselect(nullptr, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_invert(nullptr, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_modify(nullptr, COMP_SELECTION_FEATHER, 4,
                                   &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_get_active(nullptr, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_apply_shape(nullptr, COMP_SELECTION_RECT, points, 2,
                                        COMP_SELECTION_REPLACE, 1,
                                        &changed) == COMP_ERR_INVALID_ARGUMENT);

  /* 参数校验失败无副作用：既不留事务，也不动历史与选区。 */
  int32_t txn_active = -1;
  COMP_CHECK(comp_txn_active(fixture.document, &txn_active) == COMP_OK);
  COMP_CHECK(txn_active == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth);
  COMP_CHECK(active_of(fixture.document) == 1);
}

COMP_TEST(selection_coverage_validates_buffer) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0xABu);
  comp_mask mask{};
  mask.width = kWidth;
  mask.height = kHeight;
  mask.stride = kWidth;
  mask.pixels = pixels.data();

  /* 无选区：先查 active，再取 coverage（宿主契约）。 */
  COMP_CHECK(comp_selection_get_coverage(fixture.document, &mask) == COMP_ERR_STATE);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  mask.width = kWidth - 1;
  COMP_CHECK(comp_selection_get_coverage(fixture.document, &mask) == COMP_ERR_INVALID_ARGUMENT);
  mask.width = kWidth;
  mask.height = kHeight - 1;
  COMP_CHECK(comp_selection_get_coverage(fixture.document, &mask) == COMP_ERR_INVALID_ARGUMENT);
  mask.height = kHeight;
  mask.stride = kWidth - 1;
  COMP_CHECK(comp_selection_get_coverage(fixture.document, &mask) == COMP_ERR_INVALID_ARGUMENT);
  mask.stride = kWidth;
  mask.pixels = nullptr;
  COMP_CHECK(comp_selection_get_coverage(fixture.document, &mask) == COMP_ERR_INVALID_ARGUMENT);

  /* stride > width：逐行拷贝，padding 不被踩。 */
  const size_t wide_stride = static_cast<size_t>(kWidth) * 2u;
  std::vector<uint8_t> wide(static_cast<size_t>(kHeight) * wide_stride, 0x5Au);
  mask.pixels = wide.data();
  mask.stride = wide_stride;
  COMP_CHECK(comp_selection_get_coverage(fixture.document, &mask) == COMP_OK);
  COMP_CHECK(wide[static_cast<size_t>(12) * wide_stride + 12] == 255);
  COMP_CHECK(wide[static_cast<size_t>(12) * wide_stride + 8] == 255);
  COMP_CHECK(wide[static_cast<size_t>(12) * wide_stride + 7] == 0);
  COMP_CHECK(wide[static_cast<size_t>(12) * wide_stride + kWidth] == 0x5Au);
  COMP_CHECK(wide[static_cast<size_t>(12) * wide_stride + wide_stride - 1] == 0x5Au);
}

COMP_TEST(selection_transactions_group_and_roll_back) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  /* abort：整段选区操作回滚到事务前状态。 */
  const size_t base_depth = undo_depth_of(fixture.document);
  comp_txn txn = nullptr;
  COMP_CHECK(comp_txn_begin(fixture.document, "Selection", &txn) == COMP_OK);
  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(comp_txn_abort(txn) == COMP_OK);
  COMP_CHECK(active_of(fixture.document) == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == base_depth);

  /* 显式事务内多次改动只留一条撤销步。 */
  COMP_CHECK(comp_txn_begin(fixture.document, "Selection", &txn) == COMP_OK);
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_EXPAND, 3, &changed) == COMP_OK);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(comp_txn_commit(txn) == COMP_OK);
  COMP_CHECK(undo_depth_of(fixture.document) == base_depth + 1);
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(active_of(fixture.document) == 0);
}

/* ============================================ S5: 载入 / 平移 / 像素搬移 */

namespace {

comp_layer_id add_source_layer(comp_document document, comp_rect transform, int32_t group = 0) {
  comp_layer_desc desc{};
  desc.name = "s5-source";
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.transform = transform;
  desc.is_group = static_cast<uint8_t>(group);
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(document, &desc, &id) == COMP_OK);
  return id;
}

void set_layer_pixels(comp_document document, comp_layer_id id, int32_t width, int32_t height,
                      std::vector<uint8_t>& pixels) {
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(width) * 4u;
  image.pixels = pixels.data();
  COMP_CHECK(comp_layer_set_pixels(document, id, &image) == COMP_OK);
}

/* 8 x 6 的图层：左半 alpha 255、右半 alpha 128。 */
std::vector<uint8_t> split_alpha_pixels() {
  std::vector<uint8_t> pixels(8u * 6u * 4u, 0u);
  for (int32_t y = 0; y < 6; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u;
      px[0] = 200;
      px[3] = x < 4 ? 255 : 128;
    }
  }
  return pixels;
}

std::vector<uint8_t> flatten_document(comp_document document) {
  std::vector<uint8_t> out(static_cast<size_t>(kWidth) * kHeight * 4u);
  COMP_CHECK(comp_render_flatten(document, out.data(), COMP_PIXEL_RGBA8,
                                 static_cast<size_t>(kWidth) * 4u) == COMP_OK);
  return out;
}

uint8_t channel_at(const std::vector<uint8_t>& buf, int32_t x, int32_t y, int32_t channel) {
  return buf[(static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4u +
             static_cast<size_t>(channel)];
}

}  // namespace

COMP_TEST(selection_load_layer_pixels_and_threshold) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = split_alpha_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  const size_t base_depth = undo_depth_of(fixture.document);
  int32_t changed = -1;
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, 0,
                                       &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == base_depth + 1);

  /* 图层按 transform 映射到文档坐标：图层像素 (0,0) 就是文档 (10,6)。 */
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 10, 6) == 255, "the layer origin maps to its transform origin");
  COMP_CHECK_MSG(at(mask, 13, 11) == 255, "opaque half keeps its alpha");
  COMP_CHECK_MSG(at(mask, 14, 6) == 128, "semi-transparent half keeps its alpha");
  COMP_CHECK_MSG(at(mask, 9, 6) == 0, "left of the layer is unselected");
  COMP_CHECK_MSG(at(mask, 18, 6) == 0, "right of the layer is unselected");

  /* threshold 只抬高下限：128 被丢掉，255 保留。 */
  changed = -1;
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, 200,
                                       &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> thresholded = coverage(fixture.document);
  COMP_CHECK(at(thresholded, 10, 6) == 255);
  COMP_CHECK(at(thresholded, 14, 6) == 0);

  /* subtract 是「从现有选区里减掉载入的形状」。 */
  changed = -1;
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, 0,
                                       &changed) == COMP_OK);
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_SUBTRACT, 200,
                                       &changed) == COMP_OK);
  const std::vector<uint8_t> subtracted = coverage(fixture.document);
  COMP_CHECK_MSG(at(subtracted, 10, 6) == 0, "the subtracted opaque half is gone");
  COMP_CHECK_MSG(at(subtracted, 14, 6) == 128,
                 "pixels below the threshold keep their original coverage under subtract");
}

COMP_TEST(selection_load_layer_mask_ignores_pixel_alpha) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels(8u * 6u * 4u, 0u);
  for (size_t i = 0; i < 8u * 6u; ++i) pixels[i * 4u + 3] = 255; /* 像素全不透明 */
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  std::vector<uint8_t> mask_bytes(8u * 6u, 0u);
  for (int32_t y = 0; y < 6; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      mask_bytes[static_cast<size_t>(y) * 8u + static_cast<size_t>(x)] = x < 4 ? 200 : 100;
    }
  }
  comp_mask mask{};
  mask.width = 8;
  mask.height = 6;
  mask.stride = 8;
  mask.pixels = mask_bytes.data();
  COMP_CHECK(comp_layer_set_mask(fixture.document, id, &mask) == COMP_OK);

  int32_t changed = 0;
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_MASK,
                                       COMP_SELECTION_REPLACE, 150,
                                       &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  /* mask 的灰度是选区来源：200 保留、100 被 threshold 丢掉——与像素 alpha 无关。 */
  const std::vector<uint8_t> selection = coverage(fixture.document);
  COMP_CHECK(at(selection, 10, 6) == 200);
  COMP_CHECK(at(selection, 13, 11) == 200);
  COMP_CHECK(at(selection, 14, 6) == 0);
}

COMP_TEST(selection_load_layer_rejects_bad_arguments) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = split_alpha_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);
  const size_t base_depth = undo_depth_of(fixture.document);

  int32_t changed = 0;
  COMP_CHECK(comp_selection_load_layer(fixture.document, id,
                                       static_cast<comp_selection_load_source>(9),
                                       COMP_SELECTION_REPLACE, 0,
                                       &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       static_cast<comp_selection_combine>(9), 0,
                                       &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, -1,
                                       &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_load_layer(fixture.document, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, 256,
                                       &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_load_layer(fixture.document, 424242, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, 0,
                                       &changed) == COMP_ERR_NOT_FOUND);
  COMP_CHECK(comp_selection_load_layer(nullptr, id, COMP_SELECTION_LOAD_PIXELS,
                                       COMP_SELECTION_REPLACE, 0,
                                       &changed) == COMP_ERR_INVALID_ARGUMENT);

  int32_t active = -1;
  COMP_CHECK(comp_selection_get_active(fixture.document, &active) == COMP_OK);
  COMP_CHECK_MSG(active == 0, "rejected loads never install a selection");
  COMP_CHECK(undo_depth_of(fixture.document) == base_depth);
}

COMP_TEST(selection_translate_moves_outline_and_clips_to_canvas) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 4, &changed) == COMP_OK);

  const size_t depth = undo_depth_of(fixture.document);
  COMP_CHECK(comp_selection_translate(fixture.document, 5, -3, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK_MSG(feather_of(fixture.document) == 4.0f, "translation keeps the feather");
  COMP_CHECK(undo_depth_of(fixture.document) == depth + 1);

  const std::vector<uint8_t> moved = coverage(fixture.document);
  /* feather = 4 ⇒ 高斯 sigma = 2、支撑半径 = ceil(3σ) = 6；距边 5 像素处仍有约 2 级衰减，
   * 所以这里判「接近满值」而不是逐字节 255。 */
  COMP_CHECK_MSG(at(moved, 18, 10) >= 250, "the interior followed the outline");
  COMP_CHECK_MSG(at(moved, 6, 12) == 0,
                 "beyond the 3-sigma support of the feather the old outline is gone");

  /* 原地平移：无变化、不产生撤销步。 */
  const size_t before_noop = undo_depth_of(fixture.document);
  COMP_CHECK(comp_selection_translate(fixture.document, 0, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == before_noop);

  /* 负向平移（仍有重叠）：目标列从 0 开始，不能把负偏移直接转 size_t。 */
  COMP_CHECK(comp_selection_translate(fixture.document, -3, -3, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);

  /* 反向推出画布：重叠区为空，负数长度不能交给 memcpy。 */
  COMP_CHECK(comp_selection_translate(fixture.document, -1000, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 0);
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(mask_sum(coverage(fixture.document)) > 0);

  /* 推出画布：选区仍是「有选区」状态，但内容空。 */
  COMP_CHECK(comp_selection_translate(fixture.document, 1000, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 0);

  /* 一次平移一步撤销。 */
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(mask_sum(coverage(fixture.document)) > 0);
}

COMP_TEST(selection_move_pixels_cuts_and_expands_the_layer) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, 16, 16});
  std::vector<uint8_t> pixels(16u * 16u * 4u, 0u);
  for (int32_t y = 2; y < 6; ++y) {
    for (int32_t x = 2; x < 6; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
      px[0] = 255;
      px[3] = 255;
    }
  }
  set_layer_pixels(fixture.document, id, 16, 16, pixels);

  const std::vector<uint8_t> before = flatten_document(fixture.document);
  COMP_CHECK(channel_at(before, 3, 3, 0) == 255);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 0, 0, 8, 8, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const size_t depth = undo_depth_of(fixture.document);
  COMP_CHECK(comp_selection_move_pixels(fixture.document, id, 4, 0, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == depth + 1);

  const std::vector<uint8_t> cut = flatten_document(fixture.document);
  COMP_CHECK_MSG(channel_at(cut, 6, 3, 0) == 255, "the pixels landed at the destination");
  COMP_CHECK_MSG(channel_at(cut, 3, 3, 3) == 0, "the cut cleared the source pixels");

  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK_MSG(flatten_document(fixture.document) == before, "one undo restores pixels and geometry");

  /* 落点在图层矩形之外：扩层，像素仍然可见。 */
  COMP_CHECK(comp_selection_move_pixels(fixture.document, id, 20, 0, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> expanded = flatten_document(fixture.document);
  COMP_CHECK_MSG(channel_at(expanded, 22, 3, 0) == 255, "moving past the old bounds expands the layer");
  COMP_CHECK_MSG(channel_at(expanded, 3, 3, 3) == 0, "the source was cleared by the expansion move");
}

COMP_TEST(selection_move_pixels_duplicate_keeps_the_source) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, 16, 16});
  std::vector<uint8_t> pixels(16u * 16u * 4u, 0u);
  for (int32_t y = 2; y < 6; ++y) {
    for (int32_t x = 2; x < 6; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 16u + static_cast<size_t>(x)) * 4u;
      px[1] = 255;
      px[3] = 255;
    }
  }
  set_layer_pixels(fixture.document, id, 16, 16, pixels);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 0, 0, 8, 8, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_move_pixels(fixture.document, id, 4, 0, 1, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);

  const std::vector<uint8_t> after = flatten_document(fixture.document);
  COMP_CHECK_MSG(channel_at(after, 3, 3, 1) == 255, "duplicate keeps the source pixels");
  COMP_CHECK_MSG(channel_at(after, 7, 3, 1) == 255, "duplicate also paints the destination");
}

COMP_TEST(selection_move_pixels_rejects_unsupported_layers) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  /* 没有选区：成功但什么都没改。 */
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, 16, 16});
  std::vector<uint8_t> pixels(16u * 16u * 4u, 0u);
  for (size_t i = 0; i < 16u * 16u; ++i) pixels[i * 4u + 3] = 255;
  set_layer_pixels(fixture.document, id, 16, 16, pixels);
  int32_t changed = -1;
  COMP_CHECK(comp_selection_move_pixels(fixture.document, id, 2, 2, 0, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);

  /* 组图层 / 缺图层。 */
  const comp_layer_id group = add_source_layer(fixture.document, comp_rect{0, 0, 8, 8}, 1);
  COMP_CHECK(comp_selection_move_pixels(fixture.document, group, 2, 2, 0, &changed) ==
             COMP_ERR_STATE);
  COMP_CHECK(comp_selection_move_pixels(fixture.document, 424242, 2, 2, 0, &changed) ==
             COMP_ERR_NOT_FOUND);

  /* 旋转图层：拒绝而不是悄悄重采样。 */
  COMP_CHECK(comp_layer_set_transform(fixture.document, id, comp_rect{0, 0, 16, 16}, 30.0f) ==
             COMP_OK);
  COMP_CHECK(comp_selection_move_pixels(fixture.document, id, 2, 2, 0, &changed) ==
             COMP_ERR_STATE);

  /* 16 位图层：拒绝（8 位通路）。 */
  const comp_layer_id deep = add_source_layer(fixture.document, comp_rect{0, 0, 16, 16});
  std::vector<uint16_t> wide(16u * 16u * 4u, 0u);
  for (size_t i = 0; i < 16u * 16u; ++i) wide[i * 4u + 3] = 65535u;
  comp_image image{};
  image.width = 16;
  image.height = 16;
  image.format = COMP_PIXEL_RGBA16;
  image.stride = static_cast<size_t>(16u) * 4u * sizeof(uint16_t);
  image.pixels = wide.data();
  COMP_CHECK(comp_layer_set_pixels(fixture.document, deep, &image) == COMP_OK);
  COMP_CHECK(comp_selection_move_pixels(fixture.document, deep, 2, 2, 0, &changed) ==
             COMP_ERR_STATE);
}

/* ============================================ S7: 魔棒选区 */

namespace {

/* 8 x 6：左半 (layer x<4) 不透明灰 200，右半不透明黑；alpha 全 255。 */
std::vector<uint8_t> two_tone_pixels() {
  std::vector<uint8_t> pixels(8u * 6u * 4u, 0u);
  for (int32_t y = 0; y < 6; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u;
      const uint8_t value = x < 4 ? 200 : 0;
      px[0] = value;
      px[1] = value;
      px[2] = value;
      px[3] = 255;
    }
  }
  return pixels;
}

/* 8 x 6：列 x 的灰度 = min(255, x * 32)，用来验证 tolerance 的匹配半径。 */
std::vector<uint8_t> ramp_pixels() {
  std::vector<uint8_t> pixels(8u * 6u * 4u, 0u);
  for (int32_t y = 0; y < 6; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      const uint8_t value = static_cast<uint8_t>(x * 32 > 255 ? 255 : x * 32);
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u;
      px[0] = value;
      px[1] = value;
      px[2] = value;
      px[3] = 255;
    }
  }
  return pixels;
}

comp_result wand(comp_document document, comp_layer_id layer, comp_selection_wand_source source,
                 double x, double y, int32_t tolerance, int32_t contiguous, int32_t radius,
                 comp_selection_combine combine, int32_t* out_changed) {
  return comp_selection_apply_wand(document, layer, source, x, y, tolerance, contiguous, radius,
                                   combine, out_changed);
}

/* 全画布灰阶斜坡：列 x 的灰度 = x * 4（0..252），行内恒定。
 * 用来验证「扩大选取沿梯度一路漫水」与「选取相似在颜色距离上全局匹配」。 */
std::vector<uint8_t> full_ramp_pixels() {
  std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
  for (int32_t y = 0; y < kHeight; ++y) {
    for (int32_t x = 0; x < kWidth; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
      const uint8_t value = static_cast<uint8_t>(x * 4);
      px[0] = value;
      px[1] = value;
      px[2] = value;
      px[3] = 255;
    }
  }
  return pixels;
}

}  // namespace

COMP_TEST(selection_wand_selects_a_contiguous_region) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = two_tone_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);
  const size_t base_depth = undo_depth_of(fixture.document);

  int32_t changed = -1;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(active_of(fixture.document) == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == base_depth + 1);

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 10, 6) == 255, "the seed pixel is selected");
  COMP_CHECK_MSG(at(mask, 13, 11) == 255, "the whole grey half is selected");
  COMP_CHECK_MSG(at(mask, 14, 6) == 0, "the black half is a different colour");
  COMP_CHECK_MSG(at(mask, 9, 6) == 0, "outside the layer stays unselected");

  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.x == 10 && bounds.y == 6 && bounds.w == 4 && bounds.h == 6);
}

COMP_TEST(selection_wand_global_mode_reaches_disjoint_pixels) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  /* 灰-黑-灰三带：左灰带与右灰带颜色相同但互不连通。 */
  std::vector<uint8_t> pixels(8u * 6u * 4u, 0u);
  for (int32_t y = 0; y < 6; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u;
      const uint8_t value = (x < 2 || x >= 6) ? 200 : 0;
      px[0] = value;
      px[1] = value;
      px[2] = value;
      px[3] = 255;
    }
  }
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  int32_t changed = 0;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const std::vector<uint8_t> contiguous_mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(contiguous_mask, 10, 6) == 255, "contiguous keeps the seed run");
  COMP_CHECK_MSG(at(contiguous_mask, 16, 6) == 0, "contiguous stops at the black band");

  changed = 0;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 0, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const std::vector<uint8_t> global_mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(global_mask, 10, 6) == 255, "global keeps the left grey band");
  COMP_CHECK_MSG(at(global_mask, 17, 6) == 255, "global reaches the disjoint right grey band");
  COMP_CHECK_MSG(at(global_mask, 13, 6) == 0, "the black band still never matches");
}

COMP_TEST(selection_wand_tolerance_widens_the_match) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = ramp_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  /* 种子列灰 64：容差 0 只选自己；容差 32 还要吃 32 与 96。 */
  int32_t changed = 0;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 12.0, 6.0, 0, 0, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 12, 6) == 255);
  COMP_CHECK(at(mask, 11, 6) == 0);
  COMP_CHECK(at(mask, 13, 6) == 0);

  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 12.0, 6.0, 32, 0, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 11, 6) == 255, "tolerance 32 reaches the 32 grey column");
  COMP_CHECK_MSG(at(mask, 13, 6) == 255, "tolerance 32 reaches the 96 grey column");
  COMP_CHECK_MSG(at(mask, 14, 6) == 0, "tolerance 32 stops before the 128 grey column");
}

COMP_TEST(selection_grow_diffuses_along_the_colour_gradient) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, kWidth, kHeight});
  std::vector<uint8_t> pixels = full_ramp_pixels();
  set_layer_pixels(fixture.document, id, kWidth, kHeight, pixels);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 20.0f, 0.0f, 22.0f, static_cast<float>(kHeight),
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const size_t depth_before = undo_depth_of(fixture.document);
  comp_rect before{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &before) == COMP_OK);

  /* 相邻列差恰好 4：容差 4 能一路漫到两端，总色差 252 也挡不住。 */
  COMP_CHECK(comp_selection_grow(fixture.document, 4, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "grow reports a change");
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 0, 5) == 255, "grow reaches the left edge");
  COMP_CHECK_MSG(at(mask, kWidth - 1, 5) == 255, "grow reaches the right edge");
  COMP_CHECK(mask_sum(mask) == static_cast<long>(kWidth) * kHeight * 255);
  comp_rect after{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &after) == COMP_OK);
  COMP_CHECK_MSG(after.x <= before.x && after.y <= before.y &&
                     after.x + after.w >= before.x + before.w &&
                     after.y + after.h >= before.y + before.h,
                 "grown bounds contain the previous bounds");
  COMP_CHECK_MSG(undo_depth_of(fixture.document) == depth_before + 1, "grow is one undo step");

  /* 再长一次没有任何新像素：不进撤销栈。 */
  COMP_CHECK(comp_selection_grow(fixture.document, 4, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "an idempotent grow reports no change");
  COMP_CHECK_MSG(undo_depth_of(fixture.document) == depth_before + 1, "no undo step when nothing changes");
}

COMP_TEST(selection_grow_stops_at_the_tolerance_gap) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, kWidth, kHeight});
  std::vector<uint8_t> pixels = full_ramp_pixels();
  set_layer_pixels(fixture.document, id, kWidth, kHeight, pixels);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 20.0f, 0.0f, 22.0f, static_cast<float>(kHeight),
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  const size_t depth_before = undo_depth_of(fixture.document);

  /* 容差 3 < 列间差 4：一格都过不去。 */
  COMP_CHECK(comp_selection_grow(fixture.document, 3, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "grow cannot cross a gap wider than the tolerance");
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 2L * kHeight * 255);
  COMP_CHECK_MSG(undo_depth_of(fixture.document) == depth_before, "no undo step when the gap blocks");
}

COMP_TEST(selection_similar_matches_across_disjoint_regions) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels(8u * 6u * 4u, 0u);
  for (int32_t y = 0; y < 6; ++y) {
    for (int32_t x = 0; x < 8; ++x) {
      uint8_t* px = pixels.data() + (static_cast<size_t>(y) * 8 + x) * 4u;
      const bool grey = x < 2 || x >= 6;
      px[0] = grey ? 200 : 0;
      px[1] = grey ? 200 : 0;
      px[2] = grey ? 200 : 0;
      px[3] = 255;
    }
  }
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  int32_t changed = 0;
  /* 只选左边那条灰带（文档 x 10..11）。 */
  COMP_CHECK(rect(fixture.document, 10.0f, 6.0f, 12.0f, 12.0f, COMP_SELECTION_REPLACE, &changed) ==
             COMP_OK);

  /* Grow 是连通的：中间隔着黑带，走不到右边那条灰带。 */
  COMP_CHECK(comp_selection_grow(fixture.document, 0, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "grow stays inside the connected band");
  COMP_CHECK_MSG(at(coverage(fixture.document), 16, 6) == 0, "grow never jumps the black band");

  /* Similar 不看连通性：右边那条同色灰带要选中，黑带与非画布区都不选。 */
  COMP_CHECK(comp_selection_similar(fixture.document, 0, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "similar reaches a disjoint region");
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 10, 6) == 255, "the seed band stays selected");
  COMP_CHECK_MSG(at(mask, 17, 6) == 255, "the right grey band is selected");
  COMP_CHECK_MSG(at(mask, 13, 6) == 0, "the black band never matches");
  COMP_CHECK_MSG(at(mask, 5, 5) == 0, "transparent canvas pixels never match");
  COMP_CHECK_MSG(at(mask, 17, 11) == 255, "the whole band height matches");
  COMP_CHECK_MSG(at(mask, 17, 12) == 0, "nothing below the layer matches");
}

COMP_TEST(selection_similar_uses_the_colour_distance) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, kWidth, kHeight});
  std::vector<uint8_t> pixels = full_ramp_pixels();
  set_layer_pixels(fixture.document, id, kWidth, kHeight, pixels);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 20.0f, 0.0f, 22.0f, static_cast<float>(kHeight),
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  /* 种子列灰 80 / 84，容差 4：再吃掉 76 与 88，正好 19..22 四列。 */
  COMP_CHECK(comp_selection_similar(fixture.document, 4, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 19, 10) == 255, "one step below the seed still matches");
  COMP_CHECK_MSG(at(mask, 22, 10) == 255, "one step above the seed still matches");
  COMP_CHECK_MSG(at(mask, 18, 10) == 0, "two steps below is out of range");
  COMP_CHECK_MSG(at(mask, 23, 10) == 0, "two steps above is out of range");
  COMP_CHECK_MSG(mask_sum(mask) == 4L * kHeight * 255, "exactly four whole columns match");

  /* 容差 255 直接全选。 */
  COMP_CHECK(comp_selection_similar(fixture.document, 255, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 1, "tolerance 255 selects everything");
  COMP_CHECK(mask_sum(coverage(fixture.document)) == static_cast<long>(kWidth) * kHeight * 255);
}

COMP_TEST(selection_grow_similar_validate_and_reset_feather) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{0, 0, kWidth, kHeight});
  std::vector<uint8_t> pixels = full_ramp_pixels();
  set_layer_pixels(fixture.document, id, kWidth, kHeight, pixels);

  int32_t changed = 1;
  /* 没有选区：两条命令都只是空跑，不算一次改动。 */
  COMP_CHECK(comp_selection_grow(fixture.document, 16, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "grow without a selection does nothing");
  COMP_CHECK(comp_selection_similar(fixture.document, 16, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "similar without a selection does nothing");

  COMP_CHECK(rect(fixture.document, 20.0f, 0.0f, 22.0f, static_cast<float>(kHeight),
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_set_feather(fixture.document, 10, &changed) == COMP_OK);
  const size_t depth_before = undo_depth_of(fixture.document);
  const long sum_before = mask_sum(coverage(fixture.document));

  /* 越界容差：参数错误，选区与历史都不动。 */
  COMP_CHECK_MSG(comp_selection_grow(fixture.document, -1, &changed) == COMP_ERR_INVALID_ARGUMENT,
                 "negative grow tolerance is rejected");
  COMP_CHECK_MSG(comp_selection_grow(fixture.document, 256, &changed) == COMP_ERR_INVALID_ARGUMENT,
                 "grow tolerance above 255 is rejected");
  COMP_CHECK_MSG(comp_selection_similar(fixture.document, -1, &changed) == COMP_ERR_INVALID_ARGUMENT,
                 "negative similar tolerance is rejected");
  COMP_CHECK_MSG(comp_selection_similar(fixture.document, 256, &changed) == COMP_ERR_INVALID_ARGUMENT,
                 "similar tolerance above 255 is rejected");
  COMP_CHECK(mask_sum(coverage(fixture.document)) == sum_before);
  COMP_CHECK_MSG(undo_depth_of(fixture.document) == depth_before,
                 "rejected arguments leave the undo stack alone");

  /* 真长一次：羽化要按约定清零，out_changed 可以为空指针。 */
  COMP_CHECK(comp_selection_grow(fixture.document, 4, nullptr) == COMP_OK);
  COMP_CHECK_MSG(feather_of(fixture.document) == 0.0f, "grow resets the feather");
  COMP_CHECK(mask_sum(coverage(fixture.document)) == static_cast<long>(kWidth) * kHeight * 255);
}

COMP_TEST(selection_wand_merged_source_samples_the_composite) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  /* 全画布不透明白底 + 一块红补丁；合并取样应只看到「白底 + 红块」。 */
  const comp_layer_id backdrop = add_source_layer(fixture.document, comp_rect{0, 0, kWidth, kHeight});
  std::vector<uint8_t> white(static_cast<size_t>(kWidth) * kHeight * 4u);
  for (size_t i = 0; i < white.size(); i += 4) {
    white[i + 0] = 255;
    white[i + 1] = 255;
    white[i + 2] = 255;
    white[i + 3] = 255;
  }
  set_layer_pixels(fixture.document, backdrop, kWidth, kHeight, white);

  const comp_layer_id patch = add_source_layer(fixture.document, comp_rect{20, 20, 8, 8});
  std::vector<uint8_t> red(8u * 8u * 4u);
  for (size_t i = 0; i < red.size(); i += 4) {
    red[i + 0] = 255;
    red[i + 1] = 0;
    red[i + 2] = 0;
    red[i + 3] = 255;
  }
  set_layer_pixels(fixture.document, patch, 8, 8, red);

  int32_t changed = 0;
  COMP_CHECK(wand(fixture.document, patch, COMP_SELECTION_WAND_MERGED, 2.0, 2.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 2, 2) == 255, "the white backdrop is selected");
  COMP_CHECK_MSG(at(mask, 24, 24) == 0, "the red patch on top is not white");
}

COMP_TEST(selection_wand_combines_with_the_existing_selection) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = two_tone_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 10.0f, 6.0f, 18.0f, 12.0f, COMP_SELECTION_REPLACE,
                  &changed) == COMP_OK);
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_SUBTRACT, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 10, 6) == 0, "the wand subtracted the grey half");
  COMP_CHECK_MSG(at(mask, 14, 6) == 255, "the black half of the rect survives");
}

COMP_TEST(selection_wand_zero_matches_keeps_the_previous_selection) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = two_tone_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 10.0f, 6.0f, 18.0f, 12.0f, COMP_SELECTION_REPLACE,
                  &changed) == COMP_OK);
  const size_t depth_before = undo_depth_of(fixture.document);

  /*
   * 种子取两色交界 (14,6)、radius 1：参考色是 (200+0+0)/3≈67，种子像素是 0，
   * 容差 0 下窗口里没有像素命中参考色 -> 选中 0 个；此时既不能清空选区，
   * 也不能留下撤销步（壳层靠 changed==0 回滚事务）。
   */
  int32_t no_match_changed = -1;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 14.0, 6.0, 0, 1, 1,
                  COMP_SELECTION_REPLACE, &no_match_changed) == COMP_OK);
  COMP_CHECK(no_match_changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_before);
  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK_MSG(at(mask, 10, 6) == 255, "the previous selection is untouched");
}

COMP_TEST(selection_wand_rejects_bad_arguments) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = two_tone_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);
  int32_t changed = 0;

  /* 种子落在画布外。 */
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, -1.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);
  /* 种子在画布内但落在图层外。 */
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 40.0, 30.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);
  /* NaN 种子。 */
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER,
                  std::numeric_limits<double>::quiet_NaN(), 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);
  /* tolerance / contiguous / radius / combine / source 的量程。 */
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 256, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 2, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 513,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  static_cast<comp_selection_combine>(9), &changed) == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(wand(fixture.document, id, static_cast<comp_selection_wand_source>(9), 10.0, 6.0, 0, 1,
                  0, COMP_SELECTION_REPLACE, &changed) == COMP_ERR_INVALID_ARGUMENT);

  /* 源图层不存在 / 是分组 / 带缩放（非 1:1）。 */
  int32_t* null_changed = nullptr;
  COMP_CHECK(wand(fixture.document, 999999, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, null_changed) == COMP_ERR_NOT_FOUND);
  const comp_layer_id group = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6}, 1);
  COMP_CHECK(wand(fixture.document, group, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_STATE);
  const comp_layer_id scaled = add_source_layer(fixture.document, comp_rect{10, 6, 4, 6});
  std::vector<uint8_t> scaled_pixels = two_tone_pixels();
  set_layer_pixels(fixture.document, scaled, 8, 6, scaled_pixels);
  COMP_CHECK(wand(fixture.document, scaled, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_ERR_STATE);
  /* 所有失败都不得动选区。 */
  COMP_CHECK(active_of(fixture.document) == 0);
}

/* ============================================ S8: 绝对羽化 / 魔棒取样窗口 */

COMP_TEST(selection_set_feather_is_absolute_not_additive) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8.0f, 8.0f, 40.0f, 32.0f, COMP_SELECTION_REPLACE,
                  &changed) == COMP_OK &&
             changed == 1);
  const std::vector<uint8_t> hard_edges = coverage(fixture.document);
  COMP_CHECK(feather_of(fixture.document) < 0.01f);
  const size_t depth_after_rect = undo_depth_of(fixture.document);

  /* modify(FEATHER) 是叠加：从 0 起算得到 30。 */
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 30, &changed) ==
                 COMP_OK &&
             changed == 1);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 30.0f) < 0.01f);
  const size_t depth_after_modify = undo_depth_of(fixture.document);
  COMP_CHECK(depth_after_modify == depth_after_rect + 1);

  /* 绝对设置 12：是覆盖，不是 sqrt(30² + 12²) ≈ 32.31。 */
  COMP_CHECK(comp_selection_set_feather(fixture.document, 12, &changed) == COMP_OK &&
             changed == 1);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 12.0f) < 0.01f);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_after_modify + 1);
  const std::vector<uint8_t> soft_12 = coverage(fixture.document);
  COMP_CHECK(soft_12 != hard_edges);
  bool has_partial = false;
  for (uint8_t value : soft_12) {
    if (value > 0 && value < 255) {
      has_partial = true;
      break;
    }
  }
  COMP_CHECK_MSG(has_partial, "feather 12 must produce a soft edge, not a hard rectangle");

  /* 等值设回 12：无变化、无撤销步。 */
  changed = 99;
  COMP_CHECK(comp_selection_set_feather(fixture.document, 12, &changed) == COMP_OK &&
             changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_after_modify + 1);

  /* 0 = 清除羽化：有效 coverage 必须逐字节回到硬边蒙版。 */
  COMP_CHECK(comp_selection_set_feather(fixture.document, 0, &changed) == COMP_OK &&
             changed == 1);
  COMP_CHECK(feather_of(fixture.document) < 0.01f);
  COMP_CHECK(coverage(fixture.document) == hard_edges);

  /* 撤销链：0 -> 12 -> 30（set_feather 与 modify 各自一步）。 */
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 12.0f) < 0.01f);
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(std::fabs(feather_of(fixture.document) - 30.0f) < 0.01f);
}

COMP_TEST(selection_set_feather_rejects_bad_input_and_no_selection) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  /* 没有选区：成功但无变化，也不产生撤销步。 */
  int32_t changed = 99;
  COMP_CHECK(comp_selection_set_feather(fixture.document, 8, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == 0);
  COMP_CHECK(active_of(fixture.document) == 0);

  /* 量程闸在事务之前：非法值返回 INVALID_ARGUMENT 且不写 out_changed。 */
  changed = 7;
  COMP_CHECK(comp_selection_set_feather(fixture.document, -1, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_selection_set_feather(fixture.document, 251, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(changed == 7);
  COMP_CHECK(undo_depth_of(fixture.document) == 0);

  /* 显式取消选区（无活动选区）同样无操作，也不产生撤销步。 */
  COMP_CHECK(comp_selection_deselect(fixture.document, &changed) == COMP_OK);
  COMP_CHECK(active_of(fixture.document) == 0);
  const size_t depth_after_empty = undo_depth_of(fixture.document);
  changed = 5;
  COMP_CHECK(comp_selection_set_feather(fixture.document, 40, &changed) == COMP_OK &&
             changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_after_empty);
}

COMP_TEST(selection_wand_radius_averages_the_reference_window) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);
  const comp_layer_id id = add_source_layer(fixture.document, comp_rect{10, 6, 8, 6});
  std::vector<uint8_t> pixels = ramp_pixels();
  set_layer_pixels(fixture.document, id, 8, 6, pixels);

  /* 半径 0：参考色 = 种子像素 0，容差 16 只匹配灰色 0 的那一列。 */
  int32_t changed = -1;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 16, 1, 0,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK &&
             changed == 1);
  comp_rect bounds{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.w == 1);

  /* 半径 1：参考色是 2×2 窗口均值 (0+32)/2 = 16，容差 16 同时吃到 0 与 32 两列。 */
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 16, 1, 1,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK &&
             changed == 1);
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK_MSG(bounds.w == 2, "radius 1 averages the reference over the sampling window");

  /* 半径 1 + 容差 0：参考色 16 与任何像素都不等 -> 0 匹配，保留旧选区且不加撤销步。 */
  const size_t depth_before = undo_depth_of(fixture.document);
  changed = 99;
  COMP_CHECK(wand(fixture.document, id, COMP_SELECTION_WAND_LAYER, 10.0, 6.0, 0, 1, 1,
                  COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth_before);
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &bounds) == COMP_OK);
  COMP_CHECK(bounds.w == 2);
}

/* --------------------------------------------------- 手柄重采样（S46） */

COMP_TEST(selection_base_bounds_ignores_feather) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  comp_rect base{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.x == 8 && base.y == 8 && base.w == 16 && base.h == 12);

  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 4, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK_MSG(base.x == 8 && base.y == 8 && base.w == 16 && base.h == 12,
                 "feather is a parameter, not part of the base bbox");

  comp_rect visible{};
  COMP_CHECK(comp_selection_get_bounds(fixture.document, &visible) == COMP_OK);
  COMP_CHECK_MSG(visible.w > base.w, "the visible bounds include the feather spread");

  /* 取消选择后没有可变换的蒙版：返回空矩形而不是上一次的坐标。 */
  COMP_CHECK(comp_selection_deselect(fixture.document, &changed) == COMP_OK);
  base = comp_rect{1, 2, 3, 4};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.x == 0 && base.y == 0 && base.w == 0 && base.h == 0);

  /* 空指针在事务之前拒绝。 */
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, nullptr) ==
             COMP_ERR_INVALID_ARGUMENT);
}

COMP_TEST(selection_transform_bounds_identity_is_byte_identical) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  /* 用椭圆拿一条抗锯齿边，恒等变换必须原样保留这些半透明像素。 */
  const float ellipse[4] = {10.3f, 11.7f, 21.4f, 18.2f};
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_ELLIPSE, ellipse, 2,
                                        COMP_SELECTION_ADD, 1, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 3, &changed) == COMP_OK);

  const std::vector<uint8_t> before = coverage(fixture.document);
  comp_rect base{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);

  const size_t depth = undo_depth_of(fixture.document);
  changed = -1;
  COMP_CHECK(comp_selection_set_bounds(fixture.document, base, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "handles that did not move must not create an undo step");
  COMP_CHECK(undo_depth_of(fixture.document) == depth);
  COMP_CHECK_MSG(feather_of(fixture.document) == 3.0f, "transform keeps the feather radius");

  const std::vector<uint8_t> after = coverage(fixture.document);
  COMP_CHECK(after.size() == before.size() &&
             std::equal(before.begin(), before.end(), after.begin()));
}

COMP_TEST(selection_transform_bounds_rescales_the_mask) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  const size_t depth = undo_depth_of(fixture.document);
  /* 16x12 -> 32x24，左上角落在 (0,0)：面积四倍、位置也变了。 */
  changed = -1;
  COMP_CHECK(comp_selection_set_bounds(fixture.document, comp_rect{0, 0, 32, 24}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == depth + 1);
  COMP_CHECK(active_of(fixture.document) == 1);

  const std::vector<uint8_t> scaled = coverage(fixture.document);
  COMP_CHECK_MSG(at(scaled, 16, 12) == 255, "the interior is fully selected");
  COMP_CHECK_MSG(at(scaled, 1, 1) == 255, "one pixel inside the new rect is already solid");
  COMP_CHECK_MSG(at(scaled, 0, 0) > 0 && at(scaled, 0, 0) < 255,
                 "bilinear sampling fades the outermost ring");
  COMP_CHECK_MSG(at(scaled, 40, 30) == 0, "the old outline is gone");
  comp_rect base{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.x == 0 && base.y == 0 && base.w == 32 && base.h == 24);

  /* 一次变换一步撤销：回到原来的 16x12。 */
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.x == 8 && base.y == 8 && base.w == 16 && base.h == 12);

  /* 缩到一半：内容仍在，只是采样成 8x6。 */
  COMP_CHECK(comp_selection_set_bounds(fixture.document, comp_rect{8, 8, 8, 6}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.w == 8 && base.h == 6);
  const std::vector<uint8_t> shrunk = coverage(fixture.document);
  COMP_CHECK_MSG(at(shrunk, 11, 10) >= 250, "downscaling keeps the interior selected");
  COMP_CHECK(at(shrunk, 20, 16) == 0);
}

COMP_TEST(selection_transform_bounds_collapses_to_an_explicit_empty_selection) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  const size_t depth = undo_depth_of(fixture.document);
  changed = -1;
  COMP_CHECK(comp_selection_set_bounds(fixture.document, comp_rect{8, 8, 0, 0}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == depth + 1);
  COMP_CHECK_MSG(active_of(fixture.document) == 1,
                 "a collapsed handle drag leaves an explicit empty selection");
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 0);

  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 255L * 16L * 12L);

  /* 没有选区 = 无操作，不产生撤销步。 */
  Fixture empty;
  COMP_CHECK(empty.document != nullptr);
  const size_t empty_depth = undo_depth_of(empty.document);
  changed = -1;
  COMP_CHECK(comp_selection_set_bounds(empty.document, comp_rect{4, 4, 8, 8}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth_of(empty.document) == empty_depth);
  COMP_CHECK(active_of(empty.document) == 0);

  /* 越出画布：只保留画布内的部分，越界不崩。 */
  Fixture clipped;
  COMP_CHECK(clipped.document != nullptr);
  COMP_CHECK(rect(clipped.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_set_bounds(clipped.document, comp_rect{-60, -48, 128, 96}, &changed) ==
             COMP_OK);
  COMP_CHECK(changed == 1);
  const std::vector<uint8_t> wide = coverage(clipped.document);
  COMP_CHECK_MSG(at(wide, 32, 24) == 255, "the interior is selected after clipping");
  comp_rect clipped_bounds{};
  COMP_CHECK(comp_selection_get_base_bounds(clipped.document, &clipped_bounds) == COMP_OK);
  COMP_CHECK(clipped_bounds.x == 0 && clipped_bounds.y == 0);
  COMP_CHECK(clipped_bounds.w == kWidth && clipped_bounds.h == kHeight);
}

/* ------------------------------------------------- 变换选区（P0-5 收尾） */

/*
 * 面板的种子值（宽/高 = 当前紧包围盒、角度 0）必须是**恒等**：
 * 逐字节不变、changed = 0、不落撤销步。这是「打开面板直接回车不该弄脏历史」
 * 的前提，也是把旋转矩形退回轴对齐路径时靠 masks_equal 短路的那一条。
 */
COMP_TEST(selection_transform_identity_is_byte_identical) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  /* 抗锯齿椭圆 + 羽化：恒等必须原样保留半透明像素与羽化半径。 */
  const float ellipse[4] = {10.3f, 11.7f, 21.4f, 18.2f};
  COMP_CHECK(comp_selection_apply_shape(fixture.document, COMP_SELECTION_ELLIPSE, ellipse, 2,
                                        COMP_SELECTION_ADD, 1, &changed) == COMP_OK);
  COMP_CHECK(comp_selection_modify(fixture.document, COMP_SELECTION_FEATHER, 3, &changed) == COMP_OK);

  comp_rect base{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  const std::vector<uint8_t> before = coverage(fixture.document);

  comp_selection_transform_desc transform{};
  transform.center_x = static_cast<double>(base.x) + base.w * 0.5;
  transform.center_y = static_cast<double>(base.y) + base.h * 0.5;
  transform.width = base.w;
  transform.height = base.h;
  transform.angle_degrees = 0.0;

  const size_t depth = undo_depth_of(fixture.document);
  changed = -1;
  COMP_CHECK(comp_selection_transform(fixture.document, transform, &changed) == COMP_OK);
  COMP_CHECK_MSG(changed == 0, "an unrotated box at the current bounds is the identity");
  COMP_CHECK(undo_depth_of(fixture.document) == depth);
  COMP_CHECK_MSG(feather_of(fixture.document) == 3.0f, "transform keeps the feather radius");

  const std::vector<uint8_t> after = coverage(fixture.document);
  COMP_CHECK(after.size() == before.size() &&
             std::equal(before.begin(), before.end(), after.begin()));
}

/*
 * 90° 顺时针：16×12 的矩形绕自己中心转四分之一圈 -> 12×16，包围盒从
 * {8,8,16,12} 变成 {10,6,12,16}（中心 (16,14) 不动）。这一步钉死的是
 * 「先逆旋转目标像素中心、再走同一把双线性尺子」的方向——转反了会得到镜像。
 */
COMP_TEST(selection_transform_rotates_a_quarter_turn) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  comp_rect base{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.x == 8 && base.y == 8 && base.w == 16 && base.h == 12);

  comp_selection_transform_desc transform{};
  transform.center_x = 16.0;
  transform.center_y = 14.0;
  transform.width = 16.0;
  transform.height = 12.0;
  transform.angle_degrees = 90.0;

  const size_t depth = undo_depth_of(fixture.document);
  changed = -1;
  COMP_CHECK(comp_selection_transform(fixture.document, transform, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == depth + 1);
  COMP_CHECK(active_of(fixture.document) == 1);

  comp_rect rotated{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &rotated) == COMP_OK);
  COMP_CHECK_MSG(rotated.x == 10 && rotated.y == 6 && rotated.w == 12 && rotated.h == 16,
                 "a quarter turn swaps the sides around the fixed centre");

  const std::vector<uint8_t> mask = coverage(fixture.document);
  COMP_CHECK(at(mask, 16, 14) == 255);
  COMP_CHECK_MSG(at(mask, 16, 7) == 255, "the long side now stands up");
  COMP_CHECK_MSG(at(mask, 8, 8) == 0, "the old top-left corner is gone");
  COMP_CHECK(at(mask, 24, 24) == 0);

  /* 一步撤销：回到原来的 16×12。 */
  COMP_CHECK(comp_document_undo(fixture.document) == COMP_OK);
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &base) == COMP_OK);
  COMP_CHECK(base.x == 8 && base.y == 8 && base.w == 16 && base.h == 12);
}

/* 宽或高 <= 0 = 把选区收成**显式空选区**（active 仍为 1），与退化手柄同口径。 */
COMP_TEST(selection_transform_degenerate_size_collapses_to_empty) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);

  comp_selection_transform_desc transform{};
  transform.center_x = 16.0;
  transform.center_y = 14.0;
  transform.width = 0.0;
  transform.height = 12.0;
  transform.angle_degrees = 30.0;

  const size_t depth = undo_depth_of(fixture.document);
  changed = -1;
  COMP_CHECK(comp_selection_transform(fixture.document, transform, &changed) == COMP_OK);
  COMP_CHECK(changed == 1);
  COMP_CHECK(undo_depth_of(fixture.document) == depth + 1);
  COMP_CHECK_MSG(active_of(fixture.document) == 1, "collapsing keeps an explicit empty selection");
  COMP_CHECK(mask_sum(coverage(fixture.document)) == 0);
}

/* 没有选区 = 无操作：不建事务、不落撤销步、active 仍是 0。 */
COMP_TEST(selection_transform_without_a_selection_is_a_noop) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  comp_selection_transform_desc transform{};
  transform.center_x = 16.0;
  transform.center_y = 14.0;
  transform.width = 8.0;
  transform.height = 8.0;
  transform.angle_degrees = 45.0;

  const size_t depth = undo_depth_of(fixture.document);
  int32_t changed = -1;
  COMP_CHECK(comp_selection_transform(fixture.document, transform, &changed) == COMP_OK);
  COMP_CHECK(changed == 0);
  COMP_CHECK(undo_depth_of(fixture.document) == depth);
  COMP_CHECK(active_of(fixture.document) == 0);
}

/* 参数闸在事务之前：NaN / Inf / 超范围一律拒绝，且不弄脏历史、不动蒙版。 */
COMP_TEST(selection_transform_rejects_non_finite_and_out_of_range) {
  Fixture fixture;
  COMP_CHECK(fixture.document != nullptr);

  int32_t changed = 0;
  COMP_CHECK(rect(fixture.document, 8, 8, 24, 20, COMP_SELECTION_REPLACE, &changed) == COMP_OK);
  comp_rect before{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &before) == COMP_OK);

  comp_selection_transform_desc transform{};
  transform.center_x = 16.0;
  transform.center_y = 14.0;
  transform.width = 16.0;
  transform.height = 12.0;
  transform.angle_degrees = 90.0;

  const size_t depth = undo_depth_of(fixture.document);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  comp_selection_transform_desc bad = transform;
  bad.center_x = nan;
  COMP_CHECK(comp_selection_transform(fixture.document, bad, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  bad = transform;
  bad.center_y = inf;
  COMP_CHECK(comp_selection_transform(fixture.document, bad, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  bad = transform;
  bad.angle_degrees = nan;
  COMP_CHECK(comp_selection_transform(fixture.document, bad, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);
  /* 1e6 是上限：2e6 的宽度在 int 之外的几何上是无意义的，直接拒。 */
  bad = transform;
  bad.width = 2.0e6;
  COMP_CHECK(comp_selection_transform(fixture.document, bad, &changed) ==
             COMP_ERR_INVALID_ARGUMENT);

  COMP_CHECK(undo_depth_of(fixture.document) == depth);
  comp_rect after{};
  COMP_CHECK(comp_selection_get_base_bounds(fixture.document, &after) == COMP_OK);
  COMP_CHECK(after.x == before.x && after.y == before.y && after.w == before.w &&
             after.h == before.h);

  /* 无效文档句柄也不崩。 */
  COMP_CHECK(comp_selection_transform(nullptr, transform, &changed) == COMP_ERR_INVALID_ARGUMENT);
}
