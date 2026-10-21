// AI生成
/*
 * test_ui_core.cpp — Windows 外壳 UI 核心的回归护栏。
 *
 * 被测对象是 shells/windows/src/ui_core.h（平台无关、无 Win32、无引擎头）。
 * 目标不是「画得对不对」（那需要人工看窗口），而是把可判定的三件事钉死：
 *   1. 视图变换：缩放夹取、光标锚定不漂移、文档/屏幕坐标往返一致；
 *   2. 布局与命中：任何窗口尺寸下画布/面板不重叠、行列命中边界明确；
 *   3. 工具表：键位与上游 KeyboardShortcuts.swift 一致，退化或笔刷步进有界。
 *
 * 这些用例随 `python scripts/build.py` 与全量回归一起跑，不依赖窗口环境。
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "../shells/windows/src/ui_core.h"
#include "test_framework.h"

namespace {

using comp_ui::CanvasView;
using comp_ui::Layout;
using comp_ui::Point;
using comp_ui::Rect;
using comp_ui::ToolId;

bool near_value(double a, double b, double epsilon = 1e-6) {
  return std::fabs(a - b) <= epsilon;
}

bool same_point(Point a, Point b, double epsilon = 1e-6) {
  return near_value(a.x, b.x, epsilon) && near_value(a.y, b.y, epsilon);
}

bool rects_overlap(const Rect& a, const Rect& b) {
  if (a.empty() || b.empty()) return false;
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

}  // namespace

COMP_TEST(ui_core_zoom_is_clamped_and_cursor_anchored) {
  const Rect canvas{248, 46, 702, 648};
  const int doc_w = 1280;
  const int doc_h = 720;

  /* 夹取边界：0、负数、NaN 一律下限；超过上限夹到上限。 */
  COMP_CHECK(near_value(CanvasView::clamp_zoom(0.0), CanvasView::kMinZoom));
  COMP_CHECK(near_value(CanvasView::clamp_zoom(-3.0), CanvasView::kMinZoom));
  COMP_CHECK(near_value(CanvasView::clamp_zoom(std::nan("")), CanvasView::kMinZoom));
  COMP_CHECK(near_value(CanvasView::clamp_zoom(1e9), CanvasView::kMaxZoom));

  /* 锚定不变量：缩放前后，光标下的文档像素不变（含夹到上限的那一次）。 */
  const double anchors[][2] = {{canvas.x + 5.0, canvas.y + 5.0},
                               {canvas.x + canvas.w * 0.5, canvas.y + canvas.h * 0.5},
                               {canvas.x + canvas.w - 1.0, canvas.y + canvas.h - 1.0},
                               {canvas.x + 120.0, canvas.y + 500.0}};
  const double targets[] = {0.75, 2.0, 8.0, 1e9};
  for (const auto& anchor : anchors) {
    for (double target : targets) {
      CanvasView view;
      view.zoom = 1.35;
      view.pan_x = 31.0;
      view.pan_y = -17.0;
      const Point before =
          view.screen_to_doc(anchor[0], anchor[1], canvas, doc_w, doc_h);
      view.zoom_at(target, anchor[0], anchor[1], canvas);
      const Point after =
          view.screen_to_doc(anchor[0], anchor[1], canvas, doc_w, doc_h);
      COMP_CHECK_MSG(same_point(before, after), "cursor-anchored zoom must not drift");
      COMP_CHECK(view.zoom >= CanvasView::kMinZoom && view.zoom <= CanvasView::kMaxZoom);
    }
  }

  /* 已在上限时继续放大：缩放值与 pan 都不应再变化。 */
  CanvasView capped;
  capped.zoom = CanvasView::kMaxZoom;
  capped.pan_x = 12.0;
  capped.pan_y = 9.0;
  capped.zoom_by(2.0, canvas.x + 10.0, canvas.y + 10.0, canvas);
  COMP_CHECK(near_value(capped.zoom, CanvasView::kMaxZoom));
  COMP_CHECK(near_value(capped.pan_x, 12.0));
  COMP_CHECK(near_value(capped.pan_y, 9.0));
}

COMP_TEST(ui_core_doc_screen_round_trip_holds_at_any_view) {
  const Rect canvas{0, 0, 900, 700};
  const int doc_w = 640;
  const int doc_h = 480;

  const double zooms[] = {0.02, 0.25, 1.0, 3.7, 32.0};
  const double pans[][2] = {{0.0, 0.0}, {123.0, -45.0}, {-800.0, 640.0}};
  for (double zoom : zooms) {
    for (const auto& pan : pans) {
      CanvasView view;
      view.zoom = zoom;
      view.pan_x = pan[0];
      view.pan_y = pan[1];
      const double points[][2] = {{0.0, 0.0}, {doc_w * 0.5, doc_h * 0.5},
                                  {doc_w - 0.5, doc_h - 0.5}, {17.25, 333.75}};
      for (const auto& point : points) {
        const Point screen =
            view.doc_to_screen(point[0], point[1], canvas, doc_w, doc_h);
        const Point back =
            view.screen_to_doc(screen.x, screen.y, canvas, doc_w, doc_h);
        COMP_CHECK_MSG(same_point(Point{point[0], point[1]}, back, 1e-6),
                       "doc->screen->doc round trip");
      }
    }
  }

  /* pan = 0 时文档中心必须落在画布中心（各缩放级别都成立）。 */
  for (double zoom : zooms) {
    CanvasView view;
    view.zoom = zoom;
    const Point center =
        view.doc_to_screen(doc_w * 0.5, doc_h * 0.5, canvas, doc_w, doc_h);
    COMP_CHECK_MSG(same_point(center, CanvasView::canvas_center(canvas)),
                   "zero pan keeps document centered");
  }
}

COMP_TEST(ui_core_fit_and_actual_pixels_are_predictable) {
  const Rect canvas{0, 0, 800, 600};
  CanvasView view;
  view.zoom = 7.0;
  view.pan_x = 50.0;
  view.pan_y = -20.0;

  view.fit(1600, 1200, canvas, 0.5);
  /* 宽高同比例：400/1600 = 300/1200 = 0.25（margin 0.5）。 */
  COMP_CHECK(near_value(view.zoom, 0.25));
  COMP_CHECK(near_value(view.pan_x, 0.0));
  COMP_CHECK(near_value(view.pan_y, 0.0));

  /* 非等比文档取较小的一边，保证整幅都可见。 */
  view.fit(1600, 300, canvas, 1.0);
  COMP_CHECK(near_value(view.zoom, 0.5)); /* min(800/1600, 600/300) = 0.5 */

  view.actual_pixels();
  COMP_CHECK(near_value(view.zoom, 1.0));
  COMP_CHECK(near_value(view.pan_x, 0.0));
  COMP_CHECK(near_value(view.pan_y, 0.0));

  /* 退化输入不产生 NaN / 除零。 */
  view.zoom = 3.0;
  view.fit(0, 0, canvas);
  COMP_CHECK(near_value(view.zoom, 1.0));
  COMP_CHECK(std::isfinite(view.pan_x) && std::isfinite(view.pan_y));
}

COMP_TEST(ui_core_layout_never_overlaps_at_any_size) {
  const int widths[] = {0, 1, 180, 427, 428, 429, 510, 800, 1280, 1920, 3840};
  const int heights[] = {0, 1, 40, 71, 72, 200, 720, 1080};

  for (int width : widths) {
    for (int height : heights) {
      const Layout layout = comp_ui::compute_layout(width, height);

      COMP_CHECK_MSG(layout.canvas.w >= 0 && layout.canvas.h >= 0,
                     "canvas rect must never go negative");
      COMP_CHECK_MSG(layout.layer_panel.w >= 0 && layout.adjust_panel.w >= 0,
                     "panel widths must never go negative");
      COMP_CHECK_MSG(layout.toolbar.x == 0 && layout.toolbar.w == width,
                     "toolbar spans the client width");

      COMP_CHECK_MSG(!rects_overlap(layout.canvas, layout.layer_panel),
                     "canvas/layer panel overlap");
      COMP_CHECK_MSG(!rects_overlap(layout.canvas, layout.adjust_panel),
                     "canvas/adjust panel overlap");
      COMP_CHECK_MSG(!rects_overlap(layout.canvas, layout.toolbar),
                     "canvas/toolbar overlap");
      COMP_CHECK_MSG(!rects_overlap(layout.canvas, layout.status),
                     "canvas/status overlap");

      /* 三块横向拼接后必须正好铺满客户区。 */
      COMP_CHECK_MSG(layout.layer_panel.w + layout.canvas.w + layout.adjust_panel.w == width,
                     "body row must tile the full width");

      /* 宽窗口保留两侧面板；窄窗口收起右侧、再收起左侧。 */
      if (width >= Layout::kLayerPanelWidth + Layout::kAdjustPanelWidth +
                       Layout::kMinCanvasWidth) {
        COMP_CHECK_MSG(layout.layer_panel.w == Layout::kLayerPanelWidth,
                       "wide window keeps layer panel");
        COMP_CHECK_MSG(layout.adjust_panel.w == Layout::kAdjustPanelWidth,
                       "wide window keeps adjust panel");
      }
      if (width < Layout::kLayerPanelWidth + Layout::kMinCanvasWidth) {
        COMP_CHECK_MSG(layout.layer_panel.w == 0, "narrow window collapses layer panel");
      }
    }
  }

  const Layout wide = comp_ui::compute_layout(1280, 720);
  COMP_CHECK(wide.canvas.w == 1280 - Layout::kLayerPanelWidth - Layout::kAdjustPanelWidth);
  COMP_CHECK(wide.status.h == Layout::kStatusHeight);
  COMP_CHECK(wide.canvas.y == Layout::kToolbarHeight);
  COMP_CHECK(wide.canvas.bottom() == 720 - Layout::kStatusHeight);
}

COMP_TEST(ui_core_layer_rows_hit_test_top_down) {
  const Rect panel{0, Layout::kToolbarHeight, Layout::kLayerPanelWidth, 600};
  const int count = 3;

  /* 第 0 行 = 面板里最上面的一行。 */
  COMP_CHECK(comp_ui::layer_row_at(panel, count, 10.0,
                                   panel.y + comp_ui::kLayerHeaderHeight + 1.0) == 0);
  COMP_CHECK(comp_ui::layer_row_at(panel, count, 10.0,
                                   panel.y + comp_ui::kLayerHeaderHeight +
                                       comp_ui::kLayerRowHeight * 1.5) == 1);
  COMP_CHECK(comp_ui::layer_row_at(panel, count, 10.0,
                                   panel.y + comp_ui::kLayerHeaderHeight +
                                       comp_ui::kLayerRowHeight * 2.0) == 2);

  /* 表头、最后一行下方、行数之外、面板外都必须返回 -1。 */
  COMP_CHECK(comp_ui::layer_row_at(panel, count, 10.0, panel.y + 1.0) == -1);
  COMP_CHECK(comp_ui::layer_row_at(panel, count, 10.0,
                                   panel.y + comp_ui::kLayerHeaderHeight +
                                       comp_ui::kLayerRowHeight * 3.0) == -1);
  /* 同样位置，若面板里只有 2 行，第 2 行处的空白不得命中。 */
  COMP_CHECK(comp_ui::layer_row_at(panel, 2, 10.0,
                                   panel.y + comp_ui::kLayerHeaderHeight +
                                       comp_ui::kLayerRowHeight * 2.0 + 1.0) == -1);
  COMP_CHECK(comp_ui::layer_row_at(panel, count, -1.0,
                                   panel.y + comp_ui::kLayerHeaderHeight + 1.0) == -1);
  COMP_CHECK(comp_ui::layer_row_at(panel, 0, 10.0,
                                   panel.y + comp_ui::kLayerHeaderHeight + 1.0) == -1);

  /* 行矩形与命中口径一致：第 n 行矩形内任一 y 都必须命中第 n 行。 */
  for (int row = 0; row < count; ++row) {
    const Rect rect = comp_ui::layer_row_rect(panel, row);
    COMP_CHECK(rect.w == panel.w);
    COMP_CHECK(comp_ui::layer_row_at(panel, count, rect.x + 5.0, rect.y) == row);
    COMP_CHECK(comp_ui::layer_row_at(panel, count, rect.x + 5.0,
                                     rect.bottom() - 1.0) == row);
  }
}

COMP_TEST(ui_core_tool_shortcuts_match_upstream) {
  size_t count = 0;
  comp_ui::tool_table(&count);
  COMP_CHECK(count == 18);
  COMP_CHECK(static_cast<int>(ToolId::Count) == static_cast<int>(count));

  /* 逐条对齐上游 KeyboardShortcuts.swift 的工具默认键。 */
  const struct {
    char key;
    ToolId id;
  } expected[] = {{'a', ToolId::Select},     {'v', ToolId::Move},
                  {'h', ToolId::Hand},       {'z', ToolId::Zoom},
                  {'b', ToolId::Brush},      {'e', ToolId::Eraser},
                  {'j', ToolId::Heal},       {'s', ToolId::Clone},
                  {'t', ToolId::Type},       {'g', ToolId::Gradient},
                  {'u', ToolId::Shape},      {'i', ToolId::Eyedropper},
                  {'m', ToolId::Marquee},    {'w', ToolId::Wand},
                  {'l', ToolId::Lasso},      {'r', ToolId::Retouch},
                  {'c', ToolId::Crop},       {'p', ToolId::Pen}};
  COMP_CHECK(sizeof(expected) / sizeof(expected[0]) == count);
  for (const auto& entry : expected) {
    COMP_CHECK_MSG(comp_ui::tool_for_shortcut(entry.key) == entry.id,
                   "tool shortcut mapping");
    /* 大写字母必须等价（Win32 的 WM_KEYDOWN 不区分大小写）。 */
    const char upper = static_cast<char>(entry.key - 'a' + 'A');
    COMP_CHECK_MSG(comp_ui::tool_for_shortcut(upper) == entry.id,
                   "uppercase shortcut must map the same tool");
  }

  /* 默认键不得重复，否则一条快捷键会指向两个工具。 */
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      COMP_CHECK_MSG(comp_ui::tool_info_at(static_cast<int>(i))->shortcut !=
                         comp_ui::tool_info_at(static_cast<int>(j))->shortcut,
                     "duplicate tool shortcut");
    }
  }

  /* 未占用键必须显式返回 Count，不得静默回落到第一个工具。 */
  COMP_CHECK(comp_ui::tool_for_shortcut('q') == ToolId::Count);
  COMP_CHECK(comp_ui::tool_for_shortcut('\0') == ToolId::Count);
  COMP_CHECK(comp_ui::tool_info_at(-1) == nullptr);
  COMP_CHECK(comp_ui::tool_info_at(static_cast<int>(count)) == nullptr);

  /* 工具名与枚举的往返查询。 */
  COMP_CHECK(comp_ui::tool_index(ToolId::Clone) == 11);
  COMP_CHECK(comp_ui::tool_index(ToolId::Pen) == 4);
  COMP_CHECK(std::string(comp_ui::tool_info(ToolId::Clone)->name) == "Clone Stamp");
  COMP_CHECK(std::string(comp_ui::tool_info(ToolId::Pen)->name) == "Pen");
}

COMP_TEST(ui_core_tool_buttons_hit_test_without_overlap) {
  const Rect toolbar = comp_ui::compute_layout(1280, 720).toolbar;
  const int count = comp_ui::tool_count();

  for (int i = 0; i < count; ++i) {
    const Rect rect = comp_ui::tool_button_rect(toolbar, i);
    COMP_CHECK(rect.w == comp_ui::kToolButtonSize);
    COMP_CHECK(rect.h == comp_ui::kToolButtonSize);
    COMP_CHECK_MSG(toolbar.contains(rect.x + 1.0, rect.y + 1.0),
                   "tool button must stay inside the toolbar");
    COMP_CHECK_MSG(comp_ui::tool_button_at(toolbar, count, rect.x + 1.0, rect.y + 1.0) == i,
                   "tool button center must hit itself");
    if (i + 1 < count) {
      const Rect next = comp_ui::tool_button_rect(toolbar, i + 1);
      COMP_CHECK(next.x >= rect.right());
    }
  }

  /* 工具条空白处与工具条外都不得命中按钮。 */
  COMP_CHECK(comp_ui::tool_button_at(toolbar, count, 2.0, 2.0) == -1);
  COMP_CHECK(comp_ui::tool_button_at(toolbar, count,
                                     comp_ui::tool_button_rect(toolbar, count - 1).right() + 4.0,
                                     toolbar.y + toolbar.h * 0.5) == -1);
  COMP_CHECK(comp_ui::tool_button_at(toolbar, count, 10.0, -5.0) == -1);
}

COMP_TEST(ui_core_filter_table_matches_documented_ranges) {
  size_t count = 0;
  const comp_ui::FilterSpec* table = comp_ui::filter_table(&count);
  COMP_CHECK(count == 4);
  COMP_CHECK(comp_ui::filter_count() == static_cast<int>(count));

  /* 枚举值必须与 ABI `comp_filter_kind` 的 0..3 一一对齐（main.cpp 里也有 static_assert）。 */
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Vignette) == 0);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::TonalContrast) == 1);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Bloom) == 2);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::LensCorrection) == 3);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Count) == 4);

  /* 面板行数：docs/filters.md §3 各表的数据行数。 */
  const int row_counts[] = {5, 5, 2, 1};
  for (size_t i = 0; i < count; ++i) {
    COMP_CHECK_MSG(table[i].param_count == row_counts[i], "panel row count");
    COMP_CHECK_MSG(table[i].param_count > 0 &&
                       table[i].param_count <= comp_ui::FilterSpec::kMaxParams,
                   "param_count must fit the fixed array");
    for (int row = 0; row < table[i].param_count; ++row) {
      const comp_ui::FilterParamSpec& spec = table[i].params[row];
      COMP_CHECK_MSG(spec.label != nullptr && spec.label[0] != '\0', "row needs a label");
      COMP_CHECK_MSG(spec.min_value < spec.max_value, "row range must be ordered");
      COMP_CHECK_MSG(spec.step > 0.0, "row step must advance");
      COMP_CHECK_MSG(spec.default_value >= spec.min_value &&
                         spec.default_value <= spec.max_value,
                     "default must lie inside the range");
    }
    /* 未使用行的量程必须保持零（绘制层靠 param_count 截断，不读到垃圾值）。 */
    for (int row = table[i].param_count; row < comp_ui::FilterSpec::kMaxParams; ++row) {
      COMP_CHECK(table[i].params[row].label != nullptr && table[i].params[row].label[0] == '\0');
      COMP_CHECK(near_value(table[i].params[row].min_value, 0.0));
      COMP_CHECK(near_value(table[i].params[row].max_value, 0.0));
    }
  }

  /* 缺省值逐项对齐上游面板（docs/filters.md §3）。 */
  const comp_ui::FilterSpec* vignette = comp_ui::filter_spec(comp_ui::FilterId::Vignette);
  COMP_CHECK(vignette != nullptr && std::string(vignette->name) == "Vignette");
  COMP_CHECK(near_value(vignette->params[0].default_value, 0.0));   /* Amount */
  COMP_CHECK(near_value(vignette->params[1].default_value, 50.0));  /* Midpoint */
  COMP_CHECK(near_value(vignette->params[2].default_value, 100.0)); /* Roundness */
  COMP_CHECK(near_value(vignette->params[3].default_value, 60.0));  /* Feather */
  COMP_CHECK(near_value(vignette->params[4].default_value, 25.0));  /* Highlights */

  const comp_ui::FilterSpec* tonal = comp_ui::filter_spec(comp_ui::FilterId::TonalContrast);
  COMP_CHECK(tonal != nullptr && near_value(tonal->params[1].min_value, 1.0));
  COMP_CHECK(near_value(tonal->params[1].default_value, 16.0)); /* Radius */
  COMP_CHECK(near_value(tonal->params[2].default_value, 40.0)); /* Shadows */
  COMP_CHECK(near_value(tonal->params[3].default_value, 60.0)); /* Midtones */
  COMP_CHECK(near_value(tonal->params[4].default_value, 30.0)); /* Highlights */

  const comp_ui::FilterSpec* bloom = comp_ui::filter_spec(comp_ui::FilterId::Bloom);
  COMP_CHECK(bloom != nullptr && near_value(bloom->params[1].max_value, 150.0));
  COMP_CHECK(near_value(bloom->params[1].default_value, 24.0)); /* Radius */

  const comp_ui::FilterSpec* lens = comp_ui::filter_spec(comp_ui::FilterId::LensCorrection);
  COMP_CHECK(lens != nullptr && near_value(lens->params[0].min_value, -100.0) &&
             near_value(lens->params[0].max_value, 100.0));
  COMP_CHECK(near_value(lens->params[0].default_value, 0.0)); /* 恒等 */

  COMP_CHECK(comp_ui::filter_spec_at(-1) == nullptr);
  COMP_CHECK(comp_ui::filter_spec_at(static_cast<int>(count)) == nullptr);
}

COMP_TEST(ui_core_filter_panel_clamps_steps_and_resets) {
  comp_ui::FilterPanel panel;

  /* 默认构造 = 零初始化：amount 0 因此是恒等，但中点不是上游缺省 50。 */
  COMP_CHECK(panel.id == comp_ui::FilterId::Vignette);
  COMP_CHECK(panel.param_count() == 5);
  COMP_CHECK(near_value(panel.value(0), 0.0));

  /* 打开面板走 select()：值回到该滤镜的上游缺省，焦点回到第一行。 */
  panel.select(comp_ui::FilterId::Vignette);
  COMP_CHECK(near_value(panel.value(1), 50.0));
  COMP_CHECK(near_value(panel.value(2), 100.0));
  COMP_CHECK(panel.focused == 0);

  panel.select(comp_ui::FilterId::Bloom);
  COMP_CHECK(panel.param_count() == 2);
  COMP_CHECK(near_value(panel.value(0), 0.0));
  COMP_CHECK(near_value(panel.value(1), 24.0));
  /* 超出本滤镜行数的槽位必须是 0，不能被别的滤镜的缺省值污染。 */
  COMP_CHECK(near_value(panel.value(2), 0.0));
  COMP_CHECK(near_value(panel.value(5), 0.0));
  COMP_CHECK(panel.value(-1) == 0.0 && panel.value(6) == 0.0);
  COMP_CHECK(panel.param_at(2) == nullptr);

  /* 步进：按 spec.step 加一档，越界夹住并报告「没变化」。 */
  panel.focus(0);
  COMP_CHECK(panel.focused == 0);
  COMP_CHECK(panel.step(1));
  COMP_CHECK(near_value(panel.value(0), 1.0));

  panel.set_value(0, 1e9);
  COMP_CHECK(near_value(panel.value(0), 100.0));
  COMP_CHECK(!panel.step(1));
  COMP_CHECK(near_value(panel.value(0), 100.0));

  panel.set_value(0, -5.0);
  COMP_CHECK(near_value(panel.value(0), 0.0));
  COMP_CHECK(!panel.step(-1));

  /* NaN 回落到该行的下限（Bloom Radius 下限 = 1，不是 0）。 */
  panel.set_value(1, std::nan(""));
  COMP_CHECK(near_value(panel.value(1), 1.0));

  /* 焦点夹在 [0, param_count-1]，不环绕。 */
  panel.focus(99);
  COMP_CHECK(panel.focused == 1);
  panel.focus(-99);
  COMP_CHECK(panel.focused == 0);

  panel.select(comp_ui::FilterId::LensCorrection);
  COMP_CHECK(panel.param_count() == 1);
  panel.focus(1);
  COMP_CHECK(panel.focused == 0);
  COMP_CHECK(panel.step(1));
  COMP_CHECK(near_value(panel.value(0), 1.0));
  COMP_CHECK(panel.step(-1));
  COMP_CHECK(near_value(panel.value(0), 0.0));

  /* reset_values() 可以把用户调乱的行恢复成缺省。 */
  panel.select(comp_ui::FilterId::Vignette);
  panel.set_value(0, 77.0);
  panel.focus(3);
  panel.reset_values();
  COMP_CHECK(near_value(panel.value(0), 0.0));
  COMP_CHECK(panel.focused == 0);
}

COMP_TEST(ui_core_filter_rows_hit_test_matches_geometry) {
  const Rect panel{248, Layout::kToolbarHeight, Layout::kAdjustPanelWidth, 600};
  const int param_count = 5;

  /* 行矩形：起点 = 面板顶 + kFilterRowsTop，等高 kFilterRowHeight。 */
  for (int row = 0; row < param_count; ++row) {
    const Rect rect = comp_ui::filter_param_row_rect(panel, row);
    COMP_CHECK(rect.x == panel.x && rect.w == panel.w);
    COMP_CHECK(rect.y == panel.y + comp_ui::kFilterRowsTop + row * comp_ui::kFilterRowHeight);
    COMP_CHECK(rect.h == comp_ui::kFilterRowHeight);
    /* 行矩形内首尾 y 都必须命中同一行（绘制与命中共用一套常量）。 */
    COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, rect.x + 4.0, rect.y) == row);
    COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, rect.x + 4.0,
                                            rect.bottom() - 1.0) == row);
  }

  /* 行区上方（标题 / 滤镜名）不命中。 */
  COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, panel.x + 4.0,
                                          panel.y + comp_ui::kFilterRowsTop - 1.0) == -1);
  /* 行数之外、面板外、param_count 为 0 都不命中。 */
  COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, panel.x + 4.0,
                                          panel.y + comp_ui::kFilterRowsTop +
                                              param_count * comp_ui::kFilterRowHeight) == -1);
  COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, panel.x - 1.0,
                                          panel.y + comp_ui::kFilterRowsTop + 1.0) == -1);
  COMP_CHECK(comp_ui::filter_param_row_at(panel, 0, panel.x + 4.0,
                                          panel.y + comp_ui::kFilterRowsTop + 1.0) == -1);
}

COMP_TEST(ui_core_retouch_modes_cycle_and_blur_radius_is_bounded) {
  /* R 键循环：Blur -> Smudge -> Liquify -> Blur（对齐上游 retouch 组）。 */
  COMP_CHECK(comp_ui::kRetouchModeCount == 3);
  COMP_CHECK(comp_ui::kRetouchBlur == 0);
  COMP_CHECK(comp_ui::next_retouch_mode(0) == 1);
  COMP_CHECK(comp_ui::next_retouch_mode(1) == 2);
  COMP_CHECK(comp_ui::next_retouch_mode(2) == 0);
  /* 越界/负数一律回到第一个模式，不越界读名字表。 */
  COMP_CHECK(comp_ui::next_retouch_mode(-1) == 0);
  COMP_CHECK(comp_ui::next_retouch_mode(7) == 0);

  COMP_CHECK(std::string(comp_ui::retouch_mode_name(0)) == "Blur");
  COMP_CHECK(std::string(comp_ui::retouch_mode_name(1)) == "Smudge");
  COMP_CHECK(std::string(comp_ui::retouch_mode_name(2)) == "Liquify");
  COMP_CHECK(std::string(comp_ui::retouch_mode_name(3)) == "?");
  COMP_CHECK(std::string(comp_ui::retouch_mode_name(-1)) == "?");

  /* 半径量程对齐 ABI comp_tool_blur.radius 的 0.5..50。 */
  COMP_CHECK(near_value(comp_ui::clamp_blur_radius(0.0), comp_ui::kMinBlurRadius));
  COMP_CHECK(near_value(comp_ui::clamp_blur_radius(-2.0), comp_ui::kMinBlurRadius));
  COMP_CHECK(near_value(comp_ui::clamp_blur_radius(std::nan("")), comp_ui::kMinBlurRadius));
  COMP_CHECK(near_value(comp_ui::clamp_blur_radius(1e9), comp_ui::kMaxBlurRadius));

  COMP_CHECK(near_value(comp_ui::step_blur_radius(10.0, 1), 12.0));
  COMP_CHECK(near_value(comp_ui::step_blur_radius(10.0, -1), 10.0 / 1.2));
  COMP_CHECK(near_value(comp_ui::step_blur_radius(10.0, 0), 10.0));

  double radius = comp_ui::kMinBlurRadius;
  for (int i = 0; i < 200; ++i) radius = comp_ui::step_blur_radius(radius, 1);
  COMP_CHECK(near_value(radius, comp_ui::kMaxBlurRadius));
  for (int i = 0; i < 200; ++i) radius = comp_ui::step_blur_radius(radius, -1);
  COMP_CHECK(near_value(radius, comp_ui::kMinBlurRadius));
}

COMP_TEST(ui_core_stroke_points_are_debounced) {
  /* 阈值内（含）不追加，避免每个像素事件都变成折点。 */
  COMP_CHECK(!comp_ui::stroke_point_should_append(10.0, 10.0, 10.0, 10.0));
  COMP_CHECK(!comp_ui::stroke_point_should_append(10.0, 10.0,
                                                  10.0 + comp_ui::kStrokeMinDistance * 0.5, 10.0));
  /* 恰好等于阈值即追加（>= 口径，边界不含混）。 */
  COMP_CHECK(comp_ui::stroke_point_should_append(10.0, 10.0,
                                                 10.0 + comp_ui::kStrokeMinDistance, 10.0));
  COMP_CHECK(comp_ui::stroke_point_should_append(10.0, 10.0, 10.0,
                                                 10.0 + comp_ui::kStrokeMinDistance * 2.0));
  /* 距离是欧氏距离，不按单轴判断。 */
  COMP_CHECK(!comp_ui::stroke_point_should_append(
      0.0, 0.0, comp_ui::kStrokeMinDistance * 0.7, comp_ui::kStrokeMinDistance * 0.7));
  COMP_CHECK(comp_ui::stroke_point_should_append(
      0.0, 0.0, comp_ui::kStrokeMinDistance * 0.8, comp_ui::kStrokeMinDistance * 0.8));
}

COMP_TEST(ui_core_selection_boundary_traces_a_rect_outline) {
  /* 8x8 蒙版，选中 [2,6) x [1,5)。边界应合并成 4 条整边，而不是 16 个像素段。 */
  const int width = 8;
  const int height = 8;
  std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0);
  for (int y = 1; y < 5; ++y) {
    for (int x = 2; x < 6; ++x) mask[static_cast<size_t>(y) * width + x] = 255;
  }

  comp_ui::BoundarySegment segments[16];
  const int count = comp_ui::selection_boundary_segments(
      mask.data(), width, height, width, 128, segments, 16);
  COMP_CHECK_MSG(count == 4, "a filled rectangle has exactly four boundary edges");

  /* 四条边逐条核对（顺序由扫描方向决定，不依赖实现顺序）。 */
  const bool has_top = [&] {
    for (int i = 0; i < count; ++i) {
      const auto& s = segments[i];
      if (near_value(s.y0, 1.0) && near_value(s.y1, 1.0) && near_value(s.x0, 2.0) &&
          near_value(s.x1, 6.0)) return true;
    }
    return false;
  }();
  const bool has_bottom = [&] {
    for (int i = 0; i < count; ++i) {
      const auto& s = segments[i];
      if (near_value(s.y0, 5.0) && near_value(s.y1, 5.0) && near_value(s.x0, 2.0) &&
          near_value(s.x1, 6.0)) return true;
    }
    return false;
  }();
  const bool has_left = [&] {
    for (int i = 0; i < count; ++i) {
      const auto& s = segments[i];
      if (near_value(s.x0, 2.0) && near_value(s.x1, 2.0) && near_value(s.y0, 1.0) &&
          near_value(s.y1, 5.0)) return true;
    }
    return false;
  }();
  const bool has_right = [&] {
    for (int i = 0; i < count; ++i) {
      const auto& s = segments[i];
      if (near_value(s.x0, 6.0) && near_value(s.x1, 6.0) && near_value(s.y0, 1.0) &&
          near_value(s.y1, 5.0)) return true;
    }
    return false;
  }();
  COMP_CHECK_MSG(has_top && has_bottom && has_left && has_right,
                 "the four edges sit on pixel boundaries, not pixel centres");

  /* 所有线段都必须是轴对齐的（蚂蚁线只沿像素网格走）。 */
  for (int i = 0; i < count; ++i) {
    const auto& s = segments[i];
    COMP_CHECK(near_value(s.x0, s.x1) || near_value(s.y0, s.y1));
  }

  /* 容量不足时返回真实总数，只写前 capacity 条（调用方据此判断截断）。 */
  comp_ui::BoundarySegment small[2];
  COMP_CHECK(comp_ui::selection_boundary_segments(mask.data(), width, height, width, 128,
                                                  small, 2) == 4);
}

COMP_TEST(ui_core_selection_boundary_handles_edges_and_threshold) {
  const int width = 4;
  const int height = 4;

  /* 全空没有边界；全选的边界是画布外框（与上游 Ctrl+A 后沿画布边爬蚂蚁一致）。 */
  std::vector<uint8_t> empty(static_cast<size_t>(width) * height, 0);
  comp_ui::BoundarySegment out[8];
  COMP_CHECK(comp_ui::selection_boundary_segments(empty.data(), width, height, width, 128,
                                                  out, 8) == 0);

  std::vector<uint8_t> full(static_cast<size_t>(width) * height, 255);
  COMP_CHECK(comp_ui::selection_boundary_segments(full.data(), width, height, width, 128,
                                                  out, 8) == 4);

  /* 贴住文档左上角的选区：边界必须落在 x=0 / y=0 这两条网格线上。 */
  std::vector<uint8_t> corner(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) corner[static_cast<size_t>(y) * width + x] = 255;
  }
  const int corner_count = comp_ui::selection_boundary_segments(corner.data(), width, height,
                                                                width, 128, out, 8);
  COMP_CHECK(corner_count == 4); /* 矩形在文档角上时仍只缺两条内侧边 -> 4 条（含 0 边） */

  /* 阈值：100 在 128 下算「未选中」，在 64 下算「选中」。 */
  std::vector<uint8_t> faint(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) faint[static_cast<size_t>(y) * width + x] = 100;
  }
  COMP_CHECK(comp_ui::selection_boundary_segments(faint.data(), width, height, width, 128,
                                                  out, 8) == 0);
  COMP_CHECK(comp_ui::selection_boundary_segments(faint.data(), width, height, width, 64,
                                                  out, 8) == 4);

  /* 空指针 / 非正尺寸 / 负容量一律 0，不越界读。 */
  COMP_CHECK(comp_ui::selection_boundary_segments(nullptr, width, height, width, 128, out,
                                                  8) == 0);
  COMP_CHECK(comp_ui::selection_boundary_segments(empty.data(), 0, height, width, 128, out,
                                                  8) == 0);
  COMP_CHECK(comp_ui::selection_boundary_segments(empty.data(), width, height, width, 128,
                                                  out, -1) == 0);
}

COMP_TEST(ui_core_dash_spans_alternate_and_wrap_phase) {
  comp_ui::DashSpan spans[16];

  /* 相位 0：20 px、4 实 4 空 -> [0,4] [8,12] [16,20]。 */
  COMP_CHECK(comp_ui::dash_spans(20.0, 4.0, 4.0, 0.0, spans, 16) == 3);
  COMP_CHECK(near_value(spans[0].begin, 0.0) && near_value(spans[0].end, 4.0));
  COMP_CHECK(near_value(spans[1].begin, 8.0) && near_value(spans[1].end, 12.0));
  COMP_CHECK(near_value(spans[2].begin, 16.0) && near_value(spans[2].end, 20.0));

  /* 相位 2：整体左移 2 px，第一段被裁成 [0,2]。 */
  COMP_CHECK(comp_ui::dash_spans(20.0, 4.0, 4.0, 2.0, spans, 16) == 3);
  COMP_CHECK(near_value(spans[0].begin, 0.0) && near_value(spans[0].end, 2.0));
  COMP_CHECK(near_value(spans[1].begin, 6.0) && near_value(spans[1].end, 10.0));
  COMP_CHECK(near_value(spans[2].begin, 14.0) && near_value(spans[2].end, 18.0));

  /* 相位按 period 取模：8 与 0 相同，负数也折回正区间。 */
  COMP_CHECK(comp_ui::dash_spans(20.0, 4.0, 4.0, 8.0, spans, 16) == 3);
  COMP_CHECK(near_value(spans[0].end, 4.0));
  COMP_CHECK(comp_ui::dash_spans(20.0, 4.0, 4.0, -8.0, spans, 16) == 3);
  COMP_CHECK(near_value(spans[0].end, 4.0));

  /* 容量不足：返回真实总数，只写前 capacity 段。 */
  COMP_CHECK(comp_ui::dash_spans(80.0, 4.0, 4.0, 0.0, spans, 2) == 10);

  /* 退化输入：不进入死循环、不写出 NaN。 */
  COMP_CHECK(comp_ui::dash_spans(0.0, 4.0, 4.0, 0.0, spans, 16) == 0);
  COMP_CHECK(comp_ui::dash_spans(20.0, 0.0, 4.0, 0.0, spans, 16) == 0);
  COMP_CHECK(comp_ui::dash_spans(20.0, 4.0, -1.0, 0.0, spans, 16) == 0);
  COMP_CHECK(comp_ui::dash_spans(20.0, 4.0, 4.0, std::nan(""), spans, 16) == 0);
  COMP_CHECK(comp_ui::dash_spans(20.0, 1e-9, 1e-9, 0.0, spans, 16) == 0);
}

COMP_TEST(ui_core_brush_tip_and_retouch_strength_are_bounded) {
  /* 文档像素直径 -> 客户区半径：24 px @100% = 12，@50% = 6。 */
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(24.0, 1.0), 12.0));
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(24.0, 0.5), 6.0));
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(1.0, 0.02), 1.0)); /* 下限 1 px */
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(500.0, 32.0), 8000.0));
  /* 非法输入回落到 1 px，不产生 NaN 圆。 */
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(0.0, 1.0), 1.0));
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(-5.0, 1.0), 1.0));
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(std::nan(""), 1.0), 1.0));
  COMP_CHECK(near_value(comp_ui::brush_tip_radius(24.0, 0.0), 1.0));

  /* 只有吃笔刷直径的工具画笔尖。 */
  COMP_CHECK(comp_ui::tool_has_brush_tip(comp_ui::ToolId::Brush));
  COMP_CHECK(comp_ui::tool_has_brush_tip(comp_ui::ToolId::Retouch));
  COMP_CHECK(comp_ui::tool_has_brush_tip(comp_ui::ToolId::Clone));
  COMP_CHECK(!comp_ui::tool_has_brush_tip(comp_ui::ToolId::Marquee));
  COMP_CHECK(!comp_ui::tool_has_brush_tip(comp_ui::ToolId::Hand));

  /* Smudge / Liquify 强度：0/NaN 回落到下限，越界夹取，步进 0.1。 */
  COMP_CHECK(near_value(comp_ui::clamp_retouch_strength(0.0), comp_ui::kMinRetouchStrength));
  COMP_CHECK(near_value(comp_ui::clamp_retouch_strength(std::nan("")),
                        comp_ui::kMinRetouchStrength));
  COMP_CHECK(near_value(comp_ui::clamp_retouch_strength(5.0), comp_ui::kMaxRetouchStrength));
  COMP_CHECK(near_value(comp_ui::step_retouch_strength(0.5, 1), 0.6));
  COMP_CHECK(near_value(comp_ui::step_retouch_strength(0.5, -1), 0.4));
  COMP_CHECK(near_value(comp_ui::step_retouch_strength(0.5, 0), 0.5));

  double strength = comp_ui::kMinRetouchStrength;
  for (int i = 0; i < 100; ++i) strength = comp_ui::step_retouch_strength(strength, 1);
  COMP_CHECK(near_value(strength, comp_ui::kMaxRetouchStrength));
  for (int i = 0; i < 100; ++i) strength = comp_ui::step_retouch_strength(strength, -1);
  COMP_CHECK(near_value(strength, comp_ui::kMinRetouchStrength));
}

COMP_TEST(ui_core_brush_steps_are_geometric_and_bounded) {
  COMP_CHECK(near_value(comp_ui::clamp_brush_size(0.0), comp_ui::kMinBrushSize));
  COMP_CHECK(near_value(comp_ui::clamp_brush_size(-4.0), comp_ui::kMinBrushSize));
  COMP_CHECK(near_value(comp_ui::clamp_brush_size(1e6), comp_ui::kMaxBrushSize));

  /* 向上一步 = x1.2，向下一步 = /1.2，且始终夹在范围内。 */
  COMP_CHECK(near_value(comp_ui::step_brush_size(50.0, 1), 60.0));
  COMP_CHECK(near_value(comp_ui::step_brush_size(50.0, -1), 50.0 / 1.2));

  double size = 1.0;
  for (int i = 0; i < 200; ++i) size = comp_ui::step_brush_size(size, 1);
  COMP_CHECK(near_value(size, comp_ui::kMaxBrushSize));
  for (int i = 0; i < 200; ++i) size = comp_ui::step_brush_size(size, -1);
  COMP_CHECK(near_value(size, comp_ui::kMinBrushSize));

  COMP_CHECK(near_value(comp_ui::clamp_brush_hardness(std::nan("")), 0.0));
  COMP_CHECK(near_value(comp_ui::clamp_brush_hardness(2.0), 1.0));
  COMP_CHECK(near_value(comp_ui::step_brush_hardness(0.95, 1), 1.0));
  COMP_CHECK(near_value(comp_ui::step_brush_hardness(0.05, -1), 0.0));
  COMP_CHECK(near_value(comp_ui::step_brush_hardness(0.5, 0), 0.5));
}

COMP_TEST(ui_core_tool_shortcut_e_toggles_brush_and_eraser) {
  /* E 在画笔/橡皮擦之间往返：两者共用笔形，只是合成方向不同。 */
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Brush, 'e') == ToolId::Eraser);
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Eraser, 'e') == ToolId::Brush);
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Select, 'e') == ToolId::Eraser);
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Eraser, 'E') == ToolId::Brush);

  /* 其余键位仍按 tool_table 走，未命中返回 Count。 */
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Eraser, 'b') == ToolId::Brush);
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Brush, 'r') == ToolId::Retouch);
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Brush, 'x') == ToolId::Count);
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Brush, '1') == ToolId::Count);
}

COMP_TEST(ui_core_color_rgb_hsv_round_trips) {
  using comp_ui::ColorHSV;
  using comp_ui::ColorRGB;

  /* 三原色的色相锚点：红 0、绿 120、蓝 240。 */
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{255, 0, 0}).h, 0.0));
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{0, 255, 0}).h, 120.0));
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{0, 0, 255}).h, 240.0));
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{255, 255, 0}).h, 60.0));
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{255, 0, 255}).h, 300.0));

  /* 灰阶：没有色相，饱和度 0，明度即灰度。 */
  const ColorHSV gray = comp_ui::rgb_to_hsv(ColorRGB{127, 127, 127});
  COMP_CHECK(near_value(gray.s, 0.0));
  COMP_CHECK(near_value(gray.v, 127.0 / 255.0));
  COMP_CHECK(near_value(gray.h, 0.0));
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{0, 0, 0}).v, 0.0));
  COMP_CHECK(near_value(comp_ui::rgb_to_hsv(ColorRGB{255, 255, 255}).v, 1.0));

  /* 任意颜色往返：8 位量化后逐通道相等（HSV 是同一色的另一种写法）。 */
  const ColorRGB samples[] = {{0, 0, 0},     {255, 255, 255}, {58, 123, 213},
                              {200, 40, 20}, {0, 160, 0},     {139, 69, 19}};
  for (const ColorRGB& sample : samples) {
    const ColorRGB back = comp_ui::hsv_to_rgb(comp_ui::rgb_to_hsv(sample));
    COMP_CHECK(comp_ui::color_equal(back, sample));
  }

  /* 越界输入夹取：h 环绕，s/v 夹到 0..1。 */
  COMP_CHECK(comp_ui::color_equal(comp_ui::hsv_to_rgb(ColorHSV{-60.0, 1.0, 1.0}),
                                  comp_ui::hsv_to_rgb(ColorHSV{300.0, 1.0, 1.0})));
  COMP_CHECK(comp_ui::color_equal(comp_ui::hsv_to_rgb(ColorHSV{0.0, 5.0, 5.0}),
                                  ColorRGB{255, 0, 0})); /* s/v 夹到 1 -> 纯红 */
  COMP_CHECK(comp_ui::color_equal(comp_ui::hsv_to_rgb(ColorHSV{0.0, -1.0, -1.0}),
                                  ColorRGB{0, 0, 0}));
  COMP_CHECK(comp_ui::color_equal(comp_ui::hsv_to_rgb(ColorHSV{0.0, std::nan(""), 1.0}),
                                  ColorRGB{255, 255, 255})); /* NaN 饱和度 -> 灰阶，v=1 为白 */
}

COMP_TEST(ui_core_color_hex_round_trips_and_rejects_junk) {
  using comp_ui::ColorRGB;

  char hex[8] = {0};
  comp_ui::color_to_hex(ColorRGB{58, 123, 213}, hex);
  COMP_CHECK(std::string(hex) == "#3A7BD5");

  ColorRGB parsed{};
  COMP_CHECK(comp_ui::color_parse_hex("#3A7BD5", &parsed));
  COMP_CHECK(comp_ui::color_equal(parsed, ColorRGB{58, 123, 213}));
  COMP_CHECK(comp_ui::color_parse_hex("3a7bd5", &parsed)); /* 不带 #，小写也认 */
  COMP_CHECK(comp_ui::color_equal(parsed, ColorRGB{58, 123, 213}));
  COMP_CHECK(comp_ui::color_parse_hex("#abc", &parsed)); /* 3 位缩写 */
  COMP_CHECK(comp_ui::color_equal(parsed, ColorRGB{0xAA, 0xBB, 0xCC}));
  COMP_CHECK(comp_ui::color_parse_hex("#000000", &parsed));
  COMP_CHECK(comp_ui::color_equal(parsed, ColorRGB{0, 0, 0}));

  /* 非法输入必须整体失败，不能把前缀当成颜色。 */
  COMP_CHECK(!comp_ui::color_parse_hex(nullptr, &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#3A7BD5", nullptr));
  COMP_CHECK(!comp_ui::color_parse_hex("", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#12345", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#1234567", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#GGGGGG", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex(" #123456", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#123456 ", &parsed));
  COMP_CHECK(!comp_ui::color_parse_hex("#12", &parsed));
}

COMP_TEST(ui_core_swatch_palette_is_fixed_and_cycles) {
  COMP_CHECK(comp_ui::swatch_count() == 12);
  COMP_CHECK(comp_ui::color_equal(comp_ui::swatch_at(0), comp_ui::ColorRGB{0, 0, 0}));
  COMP_CHECK(comp_ui::color_equal(comp_ui::swatch_at(1), comp_ui::ColorRGB{255, 255, 255}));
  COMP_CHECK(comp_ui::color_equal(comp_ui::swatch_at(3), comp_ui::ColorRGB{255, 0, 0}));

  /* 越界不返回野指针，也不越界读：回落到黑色。 */
  COMP_CHECK(comp_ui::color_equal(comp_ui::swatch_at(-1), comp_ui::ColorRGB{0, 0, 0}));
  COMP_CHECK(comp_ui::color_equal(comp_ui::swatch_at(12), comp_ui::ColorRGB{0, 0, 0}));

  /* 循环：正向到头回到 0，反向从 0 回到末项。 */
  int index = 0;
  for (int i = 0; i < comp_ui::swatch_count(); ++i) index = comp_ui::step_swatch(index, 1);
  COMP_CHECK(index == 0);
  COMP_CHECK(comp_ui::step_swatch(0, -1) == comp_ui::swatch_count() - 1);
  COMP_CHECK(comp_ui::step_swatch(comp_ui::swatch_count() - 1, 1) == 0);
}

COMP_TEST(ui_core_color_unit_conversion_clamps) {
  COMP_CHECK(comp_ui::color_equal(comp_ui::color_from_unit(1.0, 0.5, 0.0),
                                  comp_ui::ColorRGB{255, 128, 0}));
  COMP_CHECK(comp_ui::color_equal(comp_ui::color_from_unit(-1.0, 2.0, std::nan("")),
                                  comp_ui::ColorRGB{0, 255, 0}));
  COMP_CHECK(near_value(comp_ui::color_to_unit(255), 1.0));
  COMP_CHECK(near_value(comp_ui::color_to_unit(0), 0.0));

  /* 吸管的输出就是 0..1 直通 sRGB：这条换算是「吸到的颜色 -> 画笔色」的桥。 */
  const comp_ui::ColorRGB sampled = comp_ui::color_from_unit(200.0 / 255.0, 40.0 / 255.0,
                                                             20.0 / 255.0);
  COMP_CHECK(comp_ui::color_equal(sampled, comp_ui::ColorRGB{200, 40, 20}));
}

/* ============================================ S7: 选区工具（Marquee / Lasso / Wand） */

COMP_TEST(ui_core_marquee_shape_cycles_between_two_engine_shapes) {
  COMP_CHECK(comp_ui::marquee_shape_count() == 2);
  COMP_CHECK(comp_ui::marquee_shape_next(comp_ui::kMarqueeRect) == comp_ui::kMarqueeEllipse);
  COMP_CHECK(comp_ui::marquee_shape_next(comp_ui::kMarqueeEllipse) == comp_ui::kMarqueeRect);
  /* 未知值必须落到矩形：状态被外部写坏时也不能停在未定义形状上。 */
  COMP_CHECK(comp_ui::marquee_shape_next(99) == comp_ui::kMarqueeRect);
  COMP_CHECK(std::string(comp_ui::marquee_shape_name(comp_ui::kMarqueeRect)) == "Rect");
  COMP_CHECK(std::string(comp_ui::marquee_shape_name(comp_ui::kMarqueeEllipse)) == "Ellipse");
}

COMP_TEST(ui_core_lasso_needs_three_points_and_is_bounded) {
  COMP_CHECK(!comp_ui::lasso_should_close(0));
  COMP_CHECK(!comp_ui::lasso_should_close(2));
  COMP_CHECK(comp_ui::lasso_should_close(3));
  COMP_CHECK(comp_ui::lasso_can_append(comp_ui::kLassoMaxPoints - 1));
  COMP_CHECK(!comp_ui::lasso_can_append(comp_ui::kLassoMaxPoints));
  COMP_CHECK(comp_ui::kLassoMinPoints == 3);
}

COMP_TEST(ui_core_wand_tolerance_steps_are_clamped) {
  COMP_CHECK(comp_ui::clamp_wand_tolerance(-5) == 0);
  COMP_CHECK(comp_ui::clamp_wand_tolerance(999) == 255);
  COMP_CHECK(comp_ui::step_wand_tolerance(0, -1) == 0);
  COMP_CHECK(comp_ui::step_wand_tolerance(0, 1) == 1);
  COMP_CHECK(comp_ui::step_wand_tolerance(15, 1) == 16);
  COMP_CHECK(comp_ui::step_wand_tolerance(16, 1) == 20);
  COMP_CHECK(comp_ui::step_wand_tolerance(64, 1) == 72);
  COMP_CHECK(comp_ui::step_wand_tolerance(255, 1) == 255);
  COMP_CHECK(comp_ui::step_wand_tolerance(255, -1) == 247);
  COMP_CHECK(comp_ui::kDefaultWandTolerance == 32);
}

COMP_TEST(ui_core_selection_feather_steps_are_clamped) {
  COMP_CHECK(comp_ui::clamp_selection_feather(-1) == 0);
  COMP_CHECK(comp_ui::clamp_selection_feather(251) == 250);
  COMP_CHECK(comp_ui::step_selection_feather(0, -1) == 0);
  COMP_CHECK(comp_ui::step_selection_feather(0, 1) == 1);
  COMP_CHECK(comp_ui::step_selection_feather(10, 1) == 15);
  COMP_CHECK(comp_ui::step_selection_feather(50, 1) == 60);
  COMP_CHECK(comp_ui::step_selection_feather(250, 1) == 250);
  COMP_CHECK(comp_ui::step_selection_feather(250, -1) == 240);
}

/* ------------------------------------------- P0-4: 扩展 / 收缩半径 */

COMP_TEST(ui_core_selection_grow_clamps_to_engine_range) {
  /* 下限是 1：引擎把 0 当 INVALID_ARGUMENT，面板不能把 0 递下去。 */
  COMP_CHECK(comp_ui::clamp_selection_grow(0) == 1);
  COMP_CHECK(comp_ui::clamp_selection_grow(-7) == 1);
  COMP_CHECK(comp_ui::clamp_selection_grow(1) == 1);
  COMP_CHECK(comp_ui::clamp_selection_grow(500) == 500);
  COMP_CHECK(comp_ui::clamp_selection_grow(501) == 500);

  COMP_CHECK(comp_ui::step_selection_grow(1, -1) == 1);
  COMP_CHECK(comp_ui::step_selection_grow(1, 1) == 2);
  COMP_CHECK(comp_ui::step_selection_grow(9, 1) == 10);
  COMP_CHECK(comp_ui::step_selection_grow(10, 1) == 15);
  COMP_CHECK(comp_ui::step_selection_grow(50, 1) == 60);
  COMP_CHECK(comp_ui::step_selection_grow(500, 1) == 500);
  COMP_CHECK(comp_ui::step_selection_grow(500, -1) == 490);
  /* 步进不能把值推到下限以下（与羽化的 0 语义不同）：方向只决定加减。 */
  COMP_CHECK(comp_ui::step_selection_grow(3, -5) == 2);
  COMP_CHECK(comp_ui::step_selection_grow(1, -5) == 1);
}

/* ------------------------------------------ P0-4 第二批: 色彩范围面板 */

COMP_TEST(ui_core_color_range_fuzziness_is_zero_based) {
  /* 与扩展/收缩不同，**0 是合法容差**（只选完全同色），不能照抄 clamp_selection_grow。 */
  COMP_CHECK(comp_ui::kMinColorRangeFuzziness == 0);
  COMP_CHECK(comp_ui::kMaxColorRangeFuzziness == 200);
  COMP_CHECK(comp_ui::kDefaultColorRangeFuzziness == 40);
  COMP_CHECK(comp_ui::clamp_color_range_fuzziness(-1) == 0);
  COMP_CHECK(comp_ui::clamp_color_range_fuzziness(0) == 0);
  COMP_CHECK(comp_ui::clamp_color_range_fuzziness(200) == 200);
  COMP_CHECK(comp_ui::clamp_color_range_fuzziness(201) == 200);

  COMP_CHECK(comp_ui::step_color_range_fuzziness(0, -1) == 0);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(0, 1) == 1);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(19, 1) == 20);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(20, 1) == 25);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(99, 1) == 104);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(100, 1) == 110);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(200, 1) == 200);
  COMP_CHECK(comp_ui::step_color_range_fuzziness(95, -1) == 90);
}

COMP_TEST(ui_core_color_range_swatch_geometry_hits_what_it_paints) {
  const comp_ui::Rect panel{724, 46, 330, 560};
  /* 330 宽、左右各 12 px 边距 = 306 px 可用，每格 22 px => 13 个色块。 */
  COMP_CHECK(comp_ui::color_range_swatch_capacity(panel) == 13);
  COMP_CHECK(comp_ui::color_range_swatch_capacity(comp_ui::Rect{0, 0, 30, 560}) == 0);
  COMP_CHECK(comp_ui::color_range_swatch_capacity(comp_ui::Rect{0, 0, 332, 560}) == 14);

  const comp_ui::Rect include0 = comp_ui::color_range_swatch_rect(panel, false, 0);
  const comp_ui::Rect exclude0 = comp_ui::color_range_swatch_rect(panel, true, 0);
  COMP_CHECK(include0.x == 736 && include0.y == 96 && include0.w == 18);
  COMP_CHECK(exclude0.x == 736 && exclude0.y == 120);

  /* 命中值 = 取色行 -> 排除行连续编号；色块之间的 4 px 缝不算命中。 */
  COMP_CHECK(comp_ui::color_range_swatch_at(panel, 2, 1, 737.0, 97.0) == 0);
  COMP_CHECK(comp_ui::color_range_swatch_at(panel, 2, 1, 759.0, 97.0) == 1);
  COMP_CHECK(comp_ui::color_range_swatch_at(panel, 2, 1, 737.0, 121.0) == 2);
  COMP_CHECK(comp_ui::color_range_swatch_at(panel, 2, 1, 781.0, 97.0) == -1);
  COMP_CHECK(comp_ui::color_range_swatch_at(panel, 2, 1, 737.0, 140.0) == -1);
  /* 只认首屏画得出来的：15 个取色色块，第 14 个之后点不到（画不出就不给删）。 */
  COMP_CHECK(comp_ui::color_range_swatch_at(panel, 15, 0, 736.0 + 13 * 22, 97.0) == -1);

  /* 参数行：容差（0）/ 反相（1），再往下不是行。 */
  COMP_CHECK(comp_ui::color_range_row_at(panel, 800.0, 147.0) == 0);
  COMP_CHECK(comp_ui::color_range_row_at(panel, 800.0, 170.0) == 1);
  COMP_CHECK(comp_ui::color_range_row_at(panel, 800.0, 194.0) == -1);
  COMP_CHECK(comp_ui::color_range_row_at(panel, 800.0, 145.0) == -1);
  COMP_CHECK(comp_ui::color_range_row_at(comp_ui::Rect{724, 46, 0, 560}, 724.0, 147.0) == -1);
}

/*
 * 路径面板（P0-4 可选）：列表行的绘制与命中必须同源。这里用 330x560 的面板
 * （外壳右栏的常见尺寸）把行高、滚动、可见行数和「只认画得出来的行」钉住。
 */
COMP_TEST(ui_core_path_panel_rows_hit_what_they_paint) {
  const comp_ui::Rect panel{724, 46, 330, 560};
  /* 560 - 64（表头）- 96（页脚）= 400，每行 22 => 18 行。 */
  COMP_CHECK(comp_ui::path_visible_rows(panel) == 18);
  COMP_CHECK(comp_ui::path_visible_rows(comp_ui::Rect{724, 46, 330, 120}) == 0);

  const comp_ui::Rect first = comp_ui::path_row_rect(panel, 0);
  COMP_CHECK(first.x == 724 && first.y == 110 && first.w == 330 && first.h == 22);
  COMP_CHECK(comp_ui::path_row_rect(panel, 17).y == 110 + 17 * 22);

  /* 命中 = scroll + 可视行号；行内任意一点都算，行尾之外不算。 */
  COMP_CHECK(comp_ui::path_row_at(panel, 3, 0, 800.0, 111.0) == 0);
  COMP_CHECK(comp_ui::path_row_at(panel, 3, 0, 800.0, 132.0) == 1);
  COMP_CHECK(comp_ui::path_row_at(panel, 3, 0, 800.0, 154.0) == 2);
  COMP_CHECK(comp_ui::path_row_at(panel, 3, 0, 800.0, 176.0) == -1); /* 行号超出条数 */
  COMP_CHECK(comp_ui::path_row_at(panel, 3, 0, 800.0, 109.0) == -1); /* 表头区 */
  COMP_CHECK(comp_ui::path_row_at(panel, 3, 0, 100.0, 111.0) == -1); /* 面板外 */
  /* 滚到第 2 条之后：第一可视行对应下标 2。 */
  COMP_CHECK(comp_ui::path_row_at(panel, 5, 2, 800.0, 111.0) == 2);
  COMP_CHECK(comp_ui::path_row_at(panel, 5, 2, 800.0, 133.0) == 3);
  /* 画不出来的行不给命中（滚动值比最大滚动量还大也不越界）。 */
  COMP_CHECK(comp_ui::path_row_at(panel, 5, 99, 800.0, 500.0) == -1);

  COMP_CHECK(comp_ui::path_max_scroll(panel, 3) == 0);
  COMP_CHECK(comp_ui::path_max_scroll(panel, 30) == 12);
  COMP_CHECK(comp_ui::path_clamp_scroll(panel, 30, -5) == 0);
  COMP_CHECK(comp_ui::path_clamp_scroll(panel, 30, 99) == 12);
  COMP_CHECK(comp_ui::path_clamp_scroll(panel, 30, 4) == 4);

  /* ensure_visible：只滚到刚好够，已经可见就不动。 */
  COMP_CHECK(comp_ui::path_ensure_visible(panel, 30, 3, 5) == 3);
  COMP_CHECK(comp_ui::path_ensure_visible(panel, 30, 12, 5) == 5);
  COMP_CHECK(comp_ui::path_ensure_visible(panel, 30, 25, 5) == 8);
  COMP_CHECK(comp_ui::path_ensure_visible(panel, 30, 25, 99) == 12);
}

/*
 * P1 历史面板：与路径面板共用 list_* 几何，但指数的是「状态数」——index 0 是最初
 * 状态，所以行数 = 步数 + 1。这里只验几何（绘制与命中同源），跳转语义在 ABI 用例里。
 */
COMP_TEST(ui_core_history_panel_rows_hit_what_they_paint) {
  const comp_ui::Rect panel{724, 46, 330, 560};
  COMP_CHECK(comp_ui::history_visible_rows(panel) == 18);
  COMP_CHECK(comp_ui::history_visible_rows(comp_ui::Rect{724, 46, 330, 120}) == 0);

  const comp_ui::Rect first = comp_ui::history_row_rect(panel, 0);
  COMP_CHECK(first.x == 724 && first.y == 110 && first.w == 330 && first.h == 22);
  COMP_CHECK(comp_ui::history_row_rect(panel, 17).y == 110 + 17 * 22);

  /* 下标 0 = 最初状态（「新建/打开」行），下标 i = 第 i 步。 */
  COMP_CHECK(comp_ui::history_row_at(panel, 5, 0, 800.0, 111.0) == 0);
  COMP_CHECK(comp_ui::history_row_at(panel, 5, 0, 800.0, 133.0) == 1);
  COMP_CHECK(comp_ui::history_row_at(panel, 3, 0, 800.0, 176.0) == -1); /* 超出条数 */
  COMP_CHECK(comp_ui::history_row_at(panel, 5, 0, 800.0, 109.0) == -1); /* 表头区 */
  COMP_CHECK(comp_ui::history_row_at(panel, 5, 0, 100.0, 111.0) == -1); /* 面板外 */

  COMP_CHECK(comp_ui::history_clamp_scroll(panel, 30, -5) == 0);
  COMP_CHECK(comp_ui::history_clamp_scroll(panel, 30, 99) == 12);
  COMP_CHECK(comp_ui::history_ensure_visible(panel, 30, 25, 5) == 8);
  COMP_CHECK(comp_ui::history_ensure_visible(panel, 30, 12, 5) == 5);
}

/* ============================================ S8: 钢笔路径 / 魔棒取样窗口 */
COMP_TEST(ui_core_pen_anchors_build_symmetric_segments) {
  /* 容量语义：只受 kPenMaxAnchors 约束，负数/越界都不得写入。 */
  COMP_CHECK(comp_ui::pen_can_append(0));
  COMP_CHECK(comp_ui::pen_can_append(comp_ui::kPenMaxAnchors - 1));
  COMP_CHECK(!comp_ui::pen_can_append(comp_ui::kPenMaxAnchors));
  COMP_CHECK(!comp_ui::pen_can_append(-1));

  comp_ui::PenAnchor anchors[8] = {};
  int count = 0;
  COMP_CHECK(comp_ui::pen_append_anchor(anchors, &count,
                                        comp_ui::Point{0.0, 0.0}));
  COMP_CHECK(comp_ui::pen_append_anchor(anchors, &count,
                                        comp_ui::Point{100.0, 0.0}));
  COMP_CHECK(comp_ui::pen_append_anchor(anchors, &count,
                                        comp_ui::Point{100.0, 100.0}));
  COMP_CHECK(count == 3);
  /* 直角锚点：控制柄为零，段就是直线。 */
  COMP_CHECK(same_point(anchors[0].handle_in, comp_ui::Point{0.0, 0.0}));
  COMP_CHECK(same_point(anchors[0].handle_out, comp_ui::Point{0.0, 0.0}));

  /* 拖动最后一个锚点 = 对称平滑点：出向 = pointer - anchor，入向取反。 */
  comp_ui::pen_drag_handle(anchors, count, comp_ui::Point{110.0, 110.0});
  COMP_CHECK(same_point(anchors[2].handle_out, comp_ui::Point{10.0, 10.0}));
  COMP_CHECK(same_point(anchors[2].handle_in, comp_ui::Point{-10.0, -10.0}));
  /* 其它锚点不受影响。 */
  COMP_CHECK(same_point(anchors[0].handle_out, comp_ui::Point{0.0, 0.0}));

  /* 段数：闭合 n 段（含闭合段），开放 n-1 段，少于 2 个锚点没有段。 */
  COMP_CHECK(comp_ui::pen_segment_count(3, /*closed=*/true) == 3);
  COMP_CHECK(comp_ui::pen_segment_count(3, /*closed=*/false) == 2);
  COMP_CHECK(comp_ui::pen_segment_count(1, /*closed=*/true) == 0);
  COMP_CHECK(comp_ui::pen_segment_count(0, /*closed=*/false) == 0);

  comp_ui::PenSegment segments[8] = {};
  COMP_CHECK(comp_ui::pen_build_segments(anchors, 3, /*closed=*/true, segments, 8) == 3);
  COMP_CHECK(same_point(segments[0].start, comp_ui::Point{0.0, 0.0}));
  COMP_CHECK(same_point(segments[0].end, comp_ui::Point{100.0, 0.0}));
  /* 闭合段：末锚点回到首锚点，控制柄按各自相对量加回去。 */
  COMP_CHECK(same_point(segments[2].start, comp_ui::Point{100.0, 100.0}));
  COMP_CHECK(same_point(segments[2].control1, comp_ui::Point{110.0, 110.0}));
  COMP_CHECK(same_point(segments[2].end, comp_ui::Point{0.0, 0.0}));
  /* 容量截断：返回总段数，只写前 capacity 段。 */
  COMP_CHECK(comp_ui::pen_build_segments(anchors, 3, /*closed=*/true, segments, 2) == 3);
  /* 逐段取几何与整条装配一致；越界返回全零段。 */
  const comp_ui::PenSegment single = comp_ui::pen_segment_at(anchors, 3, 2);
  COMP_CHECK(same_point(single.start, comp_ui::Point{100.0, 100.0}));
  const comp_ui::PenSegment out_of_range = comp_ui::pen_segment_at(anchors, 3, 9);
  COMP_CHECK(same_point(out_of_range.start, comp_ui::Point{0.0, 0.0}));

  /* 首锚点闭合命中：2 个锚点起才允许闭合，半径按文档距离判。 */
  COMP_CHECK(!comp_ui::pen_hits_first_anchor(anchors, 1, comp_ui::Point{1.0, 1.0}, 5.0));
  COMP_CHECK(comp_ui::pen_hits_first_anchor(anchors, 3, comp_ui::Point{2.0, 2.0}, 5.0));
  COMP_CHECK(!comp_ui::pen_hits_first_anchor(anchors, 3, comp_ui::Point{20.0, 0.0}, 5.0));
  COMP_CHECK(!comp_ui::pen_hits_first_anchor(anchors, 3, comp_ui::Point{2.0, 2.0}, -1.0));
}

COMP_TEST(ui_core_pen_preview_samples_the_curve_not_the_chord) {
  comp_ui::PenAnchor anchors[2] = {};
  int count = 0;
  comp_ui::pen_append_anchor(anchors, &count, comp_ui::Point{0.0, 0.0});
  comp_ui::pen_append_anchor(anchors, &count, comp_ui::Point{100.0, 0.0});
  /* 拖动第二个锚点：出向 (0, 50) 意味着曲线先往下再回到锚点。 */
  comp_ui::pen_drag_handle(anchors, count, comp_ui::Point{100.0, 50.0});
  COMP_CHECK(same_point(anchors[1].handle_out, comp_ui::Point{0.0, 50.0}));
  COMP_CHECK(same_point(anchors[1].handle_in, comp_ui::Point{0.0, -50.0}));

  const comp_ui::PenSegment segment = comp_ui::pen_segment_at(anchors, 2, 0);
  /* 直线段中点是 (50, 0)；有控制柄后中点必须离开弦（y = -18.75）： */
  const comp_ui::Point mid = comp_ui::pen_cubic_at(segment, 0.5);
  COMP_CHECK(same_point(mid, comp_ui::Point{50.0, -18.75}));

  comp_ui::Point polyline[64] = {};
  const int total = comp_ui::pen_preview_polyline(anchors, 2, /*closed=*/false, polyline, 64,
                                                  /*samples_per_segment=*/4);
  COMP_CHECK(total == 1 + 1 * 4);
  COMP_CHECK(same_point(polyline[0], comp_ui::Point{0.0, 0.0}));
  COMP_CHECK(same_point(polyline[4], comp_ui::Point{100.0, 0.0}));
  /* 曲线中间的采样点确实离开了弦。 */
  COMP_CHECK(polyline[2].y < -5.0);

  /* 闭合路径的预览会走回首锚点，并多出闭合段。 */
  const int closed_total = comp_ui::pen_preview_polyline(anchors, 2, /*closed=*/true, polyline,
                                                         64, /*samples_per_segment=*/4);
  COMP_CHECK(closed_total == 1 + 2 * 4);
  COMP_CHECK(same_point(polyline[closed_total - 1], anchors[0].position));
  /* 容量不足：返回总点数，只写容量内的点。 */
  comp_ui::Point small[3] = {};
  COMP_CHECK(comp_ui::pen_preview_polyline(anchors, 2, /*closed=*/false, small, 3, 4) == 5);
}

COMP_TEST(ui_core_wand_radius_steps_are_clamped) {
  COMP_CHECK(comp_ui::kDefaultWandRadius == 0);
  COMP_CHECK(comp_ui::kMaxWandRadius == 512);
  COMP_CHECK(comp_ui::clamp_wand_radius(-5) == 0);
  COMP_CHECK(comp_ui::clamp_wand_radius(999) == 512);
  COMP_CHECK(comp_ui::step_wand_radius(0, -1) == 0);
  COMP_CHECK(comp_ui::step_wand_radius(0, 1) == 1);
  COMP_CHECK(comp_ui::step_wand_radius(1, 1) == 2);
  COMP_CHECK(comp_ui::step_wand_radius(2, 1) == 4);
  COMP_CHECK(comp_ui::step_wand_radius(256, 1) == 512);
  COMP_CHECK(comp_ui::step_wand_radius(512, 1) == 512);
  COMP_CHECK(comp_ui::step_wand_radius(512, -1) == 256);
  COMP_CHECK(comp_ui::step_wand_radius(2, -1) == 1);
  COMP_CHECK(comp_ui::step_wand_radius(1, -1) == 0);
}

COMP_TEST(ui_core_gradient_type_cycles_and_drag_has_a_minimum_length) {
  /* 数值是 ABI（comp_tool_gradient.type）：外壳的镜像不得漂移。 */
  COMP_CHECK(comp_ui::kGradientLinear == 0);
  COMP_CHECK(comp_ui::kGradientRadial == 1);
  COMP_CHECK(comp_ui::kGradientTypeCount == 2);
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientLinear)) == "Linear");
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientRadial)) == "Radial");
  /* 未知值如实报 "?"（不猜成 Linear，便于面板暴露状态错乱）。 */
  COMP_CHECK(std::string(comp_ui::gradient_type_name(7)) == "?");

  /* G 键循环：任意未知值都收敛回合法类型，不产生第三态。 */
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientLinear) == comp_ui::kGradientRadial);
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientRadial) == comp_ui::kGradientLinear);
  COMP_CHECK(comp_ui::next_gradient_type(7) == comp_ui::kGradientLinear);
  COMP_CHECK(comp_ui::next_gradient_type(-1) == comp_ui::kGradientLinear);

  /*
   * 拖拽下限：与引擎参数闸的「端点重合」互补——外壳先挡掉手抖，
   * 免得一次短拖变成 INVALID_ARGUMENT 弹窗。恰好 1 px 视为可用（>= 语义）。
   */
  COMP_CHECK(comp_ui::kGradientMinLength == 1.0);
  COMP_CHECK(!comp_ui::gradient_drag_is_usable(10.0, 10.0, 10.0, 10.0));
  COMP_CHECK(!comp_ui::gradient_drag_is_usable(10.0, 10.0, 10.5, 10.5));
  COMP_CHECK(comp_ui::gradient_drag_is_usable(10.0, 10.0, 11.0, 10.0));
  COMP_CHECK(comp_ui::gradient_drag_is_usable(10.0, 10.0, 10.0, 11.0));
  COMP_CHECK(comp_ui::gradient_drag_is_usable(0.0, 0.0, 300.0, 400.0));
}

COMP_TEST(ui_core_shape_kind_cycles_and_drag_box_matches_modifiers) {
  /* 种类数值是 ABI（comp_shape_kind）：外壳的镜像不得漂移。 */
  COMP_CHECK(comp_ui::kShapeRect == 0);
  COMP_CHECK(comp_ui::kShapeEllipse == 1);
  COMP_CHECK(comp_ui::kShapeLine == 2);
  COMP_CHECK(comp_ui::kShapePath == 3);
  COMP_CHECK(comp_ui::kShellShapeKindCount == 3);
  COMP_CHECK(std::string(comp_ui::shape_kind_name(comp_ui::kShapeRect)) == "Rectangle");
  COMP_CHECK(std::string(comp_ui::shape_kind_name(comp_ui::kShapeEllipse)) == "Ellipse");
  COMP_CHECK(std::string(comp_ui::shape_kind_name(comp_ui::kShapeLine)) == "Line");
  COMP_CHECK(std::string(comp_ui::shape_kind_name(comp_ui::kShapePath)) == "Path");
  COMP_CHECK(std::string(comp_ui::shape_kind_name(9)) == "?");

  /* U 键循环：矩形 -> 椭圆 -> 直线 -> 矩形；未知值收敛回矩形。 */
  COMP_CHECK(comp_ui::next_shell_shape_kind(comp_ui::kShapeRect) == comp_ui::kShapeEllipse);
  COMP_CHECK(comp_ui::next_shell_shape_kind(comp_ui::kShapeEllipse) == comp_ui::kShapeLine);
  COMP_CHECK(comp_ui::next_shell_shape_kind(comp_ui::kShapeLine) == comp_ui::kShapeRect);
  COMP_CHECK(comp_ui::next_shell_shape_kind(comp_ui::kShapePath) == comp_ui::kShapeRect);
  COMP_CHECK(comp_ui::next_shell_shape_kind(-5) == comp_ui::kShapeRect);

  /* 描边宽度：量程 0.5..50，比值步进；NaN/0/负数回落到下限。 */
  COMP_CHECK(comp_ui::kDefaultShapeStrokeWidth == 4.0);
  COMP_CHECK(near_value(comp_ui::clamp_shape_stroke_width(0.0), 0.5));
  COMP_CHECK(near_value(comp_ui::clamp_shape_stroke_width(-3.0), 0.5));
  COMP_CHECK(near_value(comp_ui::clamp_shape_stroke_width(std::nan("")), 0.5));
  COMP_CHECK(near_value(comp_ui::clamp_shape_stroke_width(2.0), 2.0));
  COMP_CHECK(near_value(comp_ui::clamp_shape_stroke_width(500.0), 50.0));
  COMP_CHECK(near_value(comp_ui::step_shape_stroke_width(10.0, 1), 12.0));
  COMP_CHECK(near_value(comp_ui::step_shape_stroke_width(10.0, -1), 10.0 / 1.2));
  COMP_CHECK(near_value(comp_ui::step_shape_stroke_width(49.0, 1), 50.0));
  COMP_CHECK(near_value(comp_ui::step_shape_stroke_width(0.5, -1), 0.5));
  COMP_CHECK(near_value(comp_ui::step_shape_stroke_width(10.0, 0), 10.0));

  /* 圆角半径：0 是合法值（直角），步进 2 px，上限 256。 */
  COMP_CHECK(comp_ui::kDefaultShapeCornerRadius == 0.0);
  COMP_CHECK(near_value(comp_ui::clamp_shape_corner_radius(-1.0), 0.0));
  COMP_CHECK(near_value(comp_ui::clamp_shape_corner_radius(std::nan("")), 0.0));
  COMP_CHECK(near_value(comp_ui::clamp_shape_corner_radius(300.0), 256.0));
  COMP_CHECK(near_value(comp_ui::step_shape_corner_radius(4.0, 1), 6.0));
  COMP_CHECK(near_value(comp_ui::step_shape_corner_radius(4.0, -1), 2.0));
  COMP_CHECK(near_value(comp_ui::step_shape_corner_radius(0.0, -1), 0.0));

  /* 无修饰键：按下点 -> 当前点。 */
  comp_ui::ShapeDragBox plain = comp_ui::shape_drag_box(10.0, 20.0, 40.0, 50.0, false, false);
  COMP_CHECK(near_value(plain.x0, 10.0) && near_value(plain.y0, 20.0));
  COMP_CHECK(near_value(plain.x1, 40.0) && near_value(plain.y1, 50.0));

  /* 反向拖拽保留方向（不归一化）：几何照旧、渲染顺序由引擎处理。 */
  comp_ui::ShapeDragBox reversed = comp_ui::shape_drag_box(40.0, 50.0, 10.0, 20.0, false, false);
  COMP_CHECK(near_value(reversed.x0, 40.0) && near_value(reversed.x1, 10.0));

  /* Shift = 正方/正圆：取较长边并保留符号。 */
  comp_ui::ShapeDragBox square = comp_ui::shape_drag_box(0.0, 0.0, 30.0, 10.0, true, false);
  COMP_CHECK(near_value(square.x1, 30.0) && near_value(square.y1, 30.0));
  comp_ui::ShapeDragBox square_negative = comp_ui::shape_drag_box(0.0, 0.0, -10.0, -30.0, true, false);
  COMP_CHECK(near_value(square_negative.x1, -30.0) && near_value(square_negative.y1, -30.0));

  /* Alt = 从中心向外：按下点是中心。 */
  comp_ui::ShapeDragBox centered = comp_ui::shape_drag_box(20.0, 20.0, 30.0, 26.0, false, true);
  COMP_CHECK(near_value(centered.x0, 10.0) && near_value(centered.y0, 14.0));
  COMP_CHECK(near_value(centered.x1, 30.0) && near_value(centered.y1, 26.0));

  /* Shift+Alt 叠加：先把拖拽量取成正方/正圆，再以按下点为中心。 */
  comp_ui::ShapeDragBox both = comp_ui::shape_drag_box(20.0, 20.0, 30.0, 26.0, true, true);
  COMP_CHECK(near_value(both.x0, 10.0) && near_value(both.y0, 10.0));
  COMP_CHECK(near_value(both.x1, 30.0) && near_value(both.y1, 30.0));

  /*
   * 拖拽下限：外壳先挡掉手抖，免得一次短拖变成引擎的 INVALID_ARGUMENT。
   * 直线看两点距离，矩形/椭圆看宽高都至少 1 px。
   */
  COMP_CHECK(comp_ui::kShapeMinExtent == 1.0);
  comp_ui::ShapeDragBox tiny = comp_ui::shape_drag_box(10.0, 10.0, 10.4, 10.4, false, false);
  COMP_CHECK(!comp_ui::shape_drag_is_usable(comp_ui::kShapeRect, tiny));
  COMP_CHECK(!comp_ui::shape_drag_is_usable(comp_ui::kShapeEllipse, tiny));
  COMP_CHECK(!comp_ui::shape_drag_is_usable(comp_ui::kShapeLine, tiny));
  comp_ui::ShapeDragBox flat = comp_ui::shape_drag_box(10.0, 10.0, 40.0, 10.4, false, false);
  COMP_CHECK(!comp_ui::shape_drag_is_usable(comp_ui::kShapeRect, flat));
  COMP_CHECK(comp_ui::shape_drag_is_usable(comp_ui::kShapeLine, flat));
  comp_ui::ShapeDragBox one_pixel = comp_ui::shape_drag_box(10.0, 10.0, 11.0, 11.0, false, false);
  COMP_CHECK(comp_ui::shape_drag_is_usable(comp_ui::kShapeRect, one_pixel));

  /* 圆角半径的有效值：夹到用户量程后再夹到短边一半。 */
  comp_ui::ShapeDragBox wide = comp_ui::shape_drag_box(0.0, 0.0, 40.0, 10.0, false, false);
  COMP_CHECK(near_value(comp_ui::shape_effective_corner_radius(4.0, wide), 4.0));
  COMP_CHECK(near_value(comp_ui::shape_effective_corner_radius(50.0, wide), 5.0));
  COMP_CHECK(near_value(comp_ui::shape_effective_corner_radius(-2.0, wide), 0.0));
}

COMP_TEST(ui_core_type_metrics_and_ascii_key_mapping_match_the_kernel) {
  /*
   * 量程是跨层的「第二套真值」：ui_core 不 include engine（零依赖/可交叉编译），
   * engine/domain/type_raster.h 的量程只能靠这条测试逐项钉死。改了任何一侧，
   * 这里先红。
   */
  COMP_CHECK(comp_ui::kMinTypeFontSize == 6.0);
  COMP_CHECK(comp_ui::kMaxTypeFontSize == 2048.0);
  COMP_CHECK(comp_ui::kDefaultTypeFontSize == 32.0);
  COMP_CHECK(comp_ui::kMinTypeLineHeight == 1.0);
  COMP_CHECK(comp_ui::kMaxTypeLineHeight == 8.0);
  COMP_CHECK(comp_ui::kDefaultTypeLineHeight == 1.25);
  COMP_CHECK(comp_ui::kMinTypeLetterSpacing == -4096.0);
  COMP_CHECK(comp_ui::kMaxTypeLetterSpacing == 4096.0);
  COMP_CHECK(comp_ui::kMinTypeAdvance == 0.5);

  /* 字号：乘性步进 1.25；NaN/0/负数回落到下限；上界 2048。 */
  COMP_CHECK(near_value(comp_ui::clamp_type_font_size(0.0), 6.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_font_size(-10.0), 6.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_font_size(std::nan("")), 6.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_font_size(5000.0), 2048.0));
  COMP_CHECK(near_value(comp_ui::step_type_font_size(32.0, 1), 40.0));
  COMP_CHECK(near_value(comp_ui::step_type_font_size(32.0, -1), 25.6));
  COMP_CHECK(near_value(comp_ui::step_type_font_size(2040.0, 1), 2048.0));
  COMP_CHECK(near_value(comp_ui::step_type_font_size(6.0, -1), 6.0));
  COMP_CHECK(near_value(comp_ui::step_type_font_size(32.0, 0), 32.0));

  /* 行距：1..8（小于 1 会让两行重叠，引擎拒绝，外壳不给出这个值）。 */
  COMP_CHECK(near_value(comp_ui::clamp_type_line_height(0.5), 1.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_line_height(20.0), 8.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_line_height(2.0), 2.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_line_height(std::nan("")), 1.25));

  /*
   * 字距：线性步进 1 px；负值下限跟着字号走（步进 6*size/7 + spacing 必须 >= 0.5），
   * 这条下限正是 engine 的 kMinTypeAdvance。
   */
  COMP_CHECK(near_value(comp_ui::type_letter_spacing_min(6.0), 0.5 - 6.0 * 6.0 / 7.0));
  COMP_CHECK(near_value(comp_ui::type_letter_spacing_min(2048.0), 0.5 - 6.0 * 2048.0 / 7.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_letter_spacing(-1000.0, 6.0), 0.5 - 6.0 * 6.0 / 7.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_letter_spacing(-1000.0, 32.0), 0.5 - 6.0 * 32.0 / 7.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_letter_spacing(5000.0, 32.0), 4096.0));
  COMP_CHECK(near_value(comp_ui::clamp_type_letter_spacing(std::nan(""), 32.0), 0.0));
  COMP_CHECK(near_value(comp_ui::step_type_letter_spacing(0.0, 32.0, 1), 1.0));
  COMP_CHECK(near_value(comp_ui::step_type_letter_spacing(0.0, 32.0, -1), -1.0));
  COMP_CHECK(near_value(comp_ui::step_type_letter_spacing(4096.0, 32.0, 1), 4096.0));
  COMP_CHECK(near_value(comp_ui::step_type_letter_spacing(0.0, 32.0, 0), 0.0));

  /*
   * 文本框度量镜像内核 build_layout：字号 7 = 缩放 1、步进 6；
   * 宽 = 最长行字形数 × 步进，高 = font_size + (行数-1) × font_size × line_height。
   */
  comp_ui::TypeBlockMetrics metrics =
      comp_ui::type_block_metrics(7.0, 1.0, 0.0, "A\nBB");
  COMP_CHECK(near_value(metrics.advance, 6.0));
  COMP_CHECK(metrics.line_count == 2);
  COMP_CHECK(metrics.max_line_glyphs == 2);
  COMP_CHECK(metrics.last_line_glyphs == 2);
  COMP_CHECK(near_value(metrics.width, 12.0));
  COMP_CHECK(near_value(metrics.height, 14.0));

  metrics = comp_ui::type_block_metrics(7.0, 1.5, 0.0, "A\nB");
  COMP_CHECK(near_value(metrics.height, 7.0 + 7.0 * 1.5));

  metrics = comp_ui::type_block_metrics(7.0, 1.0, 2.0, "AB");
  COMP_CHECK(near_value(metrics.advance, 8.0));
  COMP_CHECK(near_value(metrics.width, 16.0));

  /* 空文本 / nullptr：宽 0、高一个字号（插入点光标只画竖线）。 */
  metrics = comp_ui::type_block_metrics(32.0, 1.25, 0.0, "");
  COMP_CHECK(near_value(metrics.width, 0.0));
  COMP_CHECK(near_value(metrics.height, 32.0));
  metrics = comp_ui::type_block_metrics(32.0, 1.25, 0.0, nullptr);
  COMP_CHECK(near_value(metrics.width, 0.0));
  COMP_CHECK(metrics.line_count == 1);

  /* 尾随换行 = 多一个空行（与内核语义一致）。 */
  metrics = comp_ui::type_block_metrics(7.0, 1.0, 0.0, "A\n");
  COMP_CHECK(metrics.line_count == 2);
  COMP_CHECK(near_value(metrics.height, 14.0));

  /*
   * WM_KEYDOWN -> ASCII：US 布局映射；控制键/功能键返回 0（外壳据此把键让给
   * 工具快捷键而不是塞进文本）。
   */
  COMP_CHECK(comp_ui::type_ascii_from_vk('A', false) == 'a');
  COMP_CHECK(comp_ui::type_ascii_from_vk('A', true) == 'A');
  COMP_CHECK(comp_ui::type_ascii_from_vk('Z', false) == 'z');
  COMP_CHECK(comp_ui::type_ascii_from_vk('0', false) == '0');
  COMP_CHECK(comp_ui::type_ascii_from_vk('9', true) == '(');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x20, false) == ' ');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x20, true) == ' ');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBA, false) == ';');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBA, true) == ':');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBB, true) == '+');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBC, true) == '<');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBD, true) == '_');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBE, false) == '.');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xBF, true) == '?');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xC0, true) == '~');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xDB, true) == '{');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xDC, true) == '|');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xDD, true) == '}');
  COMP_CHECK(comp_ui::type_ascii_from_vk(0xDE, true) == '"');
  /* 回车/退格/Tab/方向键/F 键都不是可键入字符。 */
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x0D, false) == 0);
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x08, false) == 0);
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x09, false) == 0);
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x25, false) == 0);
  COMP_CHECK(comp_ui::type_ascii_from_vk(0x70, false) == 0);

  /* 插入点光标：文档字号 × 缩放，至少 1 px。 */
  COMP_CHECK(near_value(comp_ui::type_caret_height(32.0, 0.5), 16.0));
  COMP_CHECK(near_value(comp_ui::type_caret_height(32.0, 2.0), 64.0));
  COMP_CHECK(comp_ui::type_caret_height(6.0, 0.0) == 1.0);

  /* 工具表：T 落在文字工具上（表本身是 ui_core 的唯一键位来源）。 */
  COMP_CHECK(comp_ui::tool_for_shortcut('t') == comp_ui::ToolId::Type);
  COMP_CHECK(comp_ui::tool_for_shortcut('T') == comp_ui::ToolId::Type);
  const comp_ui::ToolInfo* type_info = comp_ui::tool_info(comp_ui::ToolId::Type);
  COMP_CHECK(type_info != nullptr && std::string(type_info->name) == "Type" &&
             type_info->shortcut == 't');
}

/*
 * S14 文字属性面板：行表、量纲映射、步进、命中几何。面板与壳共用这些函数，
 * 因此这里钉住的是「面板显示的值 = 引擎接受的值」这条口径。
 */
COMP_TEST(ui_core_type_panel_rows_steps_and_hit_testing) {
  COMP_CHECK(comp_ui::kTypePanelRowCount == 3);
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelFontSizeRow)) ==
             "Font Size");
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLineHeightRow)) ==
             "Line Height");
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLetterSpacingRow)) ==
             "Letter Spacing");
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(-1)).empty());
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(3)).empty());
  COMP_CHECK(comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelLineHeightRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelFontSizeRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelLetterSpacingRow));

  /* 行距 5% 一步，夹在 1.0..8.0（面板与 Alt+[ / ] 共用）。 */
  COMP_CHECK(near_value(comp_ui::step_type_line_height(1.25, 1), 1.30));
  COMP_CHECK(near_value(comp_ui::step_type_line_height(1.25, -1), 1.20));
  COMP_CHECK(comp_ui::step_type_line_height(1.0, -1) == 1.0);
  COMP_CHECK(comp_ui::step_type_line_height(8.0, 1) == 8.0);
  /* 非正值不是合法行距：回落到默认 1.25（与 clamp_type_line_height 同口径）。 */
  COMP_CHECK(near_value(comp_ui::step_type_line_height(0.0, 0),
                        comp_ui::kDefaultTypeLineHeight));

  /* 字号行：乘性步进（×/÷1.25）；字距行：线性 1 px；行距行：5%。 */
  COMP_CHECK(near_value(comp_ui::type_panel_step_value(comp_ui::kTypePanelFontSizeRow, 32.0,
                                                       32.0, 1),
                        comp_ui::step_type_font_size(32.0, 1)));
  COMP_CHECK(near_value(comp_ui::type_panel_step_value(comp_ui::kTypePanelLetterSpacingRow,
                                                       0.0, 32.0, 1),
                        1.0));
  COMP_CHECK(near_value(comp_ui::type_panel_step_value(comp_ui::kTypePanelLineHeightRow, 1.25,
                                                       32.0, 1),
                        1.30));

  /* 夹取：字号 6..2048；字距下限随字号变化（步距 >= 0.5 px）。 */
  COMP_CHECK(comp_ui::type_panel_clamp_value(comp_ui::kTypePanelFontSizeRow, 1.0, 32.0) == 6.0);
  COMP_CHECK(comp_ui::type_panel_clamp_value(comp_ui::kTypePanelFontSizeRow, 5000.0, 32.0) ==
             2048.0);
  COMP_CHECK(comp_ui::type_panel_clamp_value(comp_ui::kTypePanelLineHeightRow, 0.1, 32.0) == 1.0);
  COMP_CHECK(near_value(comp_ui::type_panel_clamp_value(comp_ui::kTypePanelLetterSpacingRow,
                                                        -999.0, 32.0),
                        comp_ui::type_letter_spacing_min(32.0)));
  /* 同一 -10 px：小字号下被下限抬起，大字号下原样保留。 */
  COMP_CHECK(comp_ui::type_panel_clamp_value(comp_ui::kTypePanelLetterSpacingRow, -10.0, 7.0) >
             -10.0);
  COMP_CHECK(comp_ui::type_panel_clamp_value(comp_ui::kTypePanelLetterSpacingRow, -10.0,
                                             2048.0) == -10.0);

  /* 命中几何与绘制常量同源：行高 20、首行距面板顶 54。 */
  const comp_ui::Rect panel{1000, 46, 330, 600};
  COMP_CHECK(comp_ui::kTypeRowHeight == 20 && comp_ui::kTypeRowsTop == 54);
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 101.0) == 0);
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 121.0) == 1);
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 141.0) == 2);
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 161.0) == -1); /* 第四行不存在 */
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 99.0) == -1);  /* 标题区 */
  COMP_CHECK(comp_ui::type_param_row_at(panel, 500.0, 121.0) == -1);  /* 面板外 */
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1330.0, 121.0) == -1); /* 右边界开区间 */

  const comp_ui::Rect row_rect = comp_ui::type_param_row_rect(panel, 2);
  COMP_CHECK(row_rect.x == 1000 && row_rect.y == 46 + 54 + 2 * 20 && row_rect.w == 330 &&
             row_rect.h == 20);
}

/* ---------------------------------------------------------- 混合模式（S21） */

/*
 * 表逐字对齐上游 LayerAppearance.swift：LayerBlendMode.allCases 的声明序
 * （Shift++ / Shift+− 就按这个序循环）与 LayerBlendMode.groups 的分组。
 * 数值同时是 comp_blend_mode 枚举，ABI 冻结，不能重排。
 */
COMP_TEST(ui_core_blend_table_matches_upstream_all_cases) {
  /* 24 档：0 = Normal .. 23 = Luminosity（ABI comp_blend_mode 冻结表）。 */
  COMP_CHECK(comp_ui::kBlendCount == 24);
  COMP_CHECK(comp_ui::kBlendNormal == 0);

  const struct {
    int mode;
    const char* name; /* 上游 rawValue 的规范拼写 */
    int group;        /* 上游 groups[] 的组号：见 kBlendGroupName */
  } expected[] = {
      {0, "Normal", 0},
      {1, "Darken", 1},
      {2, "Multiply", 1},
      {3, "Color Burn", 1},
      {4, "Linear Burn", 1},
      {5, "Lighten", 2},
      {6, "Screen", 2},
      {7, "Color Dodge", 2},
      {8, "Linear Dodge (Add)", 2},
      {9, "Overlay", 3},
      {10, "Soft Light", 3},
      {11, "Hard Light", 3},
      {12, "Vivid Light", 3},
      {13, "Linear Light", 3},
      {14, "Pin Light", 3},
      {15, "Hard Mix", 3},
      {16, "Difference", 4},
      {17, "Exclusion", 4},
      {18, "Subtract", 4},
      {19, "Divide", 4},
      {20, "Hue", 5},
      {21, "Saturation", 5},
      {22, "Color", 5},
      {23, "Luminosity", 5},
  };
  COMP_CHECK(sizeof(expected) / sizeof(expected[0]) == comp_ui::kBlendCount);

  for (const auto& entry : expected) {
    COMP_CHECK_MSG(std::string(comp_ui::blend_mode_name(entry.mode)) == entry.name,
                   comp_ui::blend_mode_name(entry.mode));
    COMP_CHECK_MSG(comp_ui::blend_group_of(entry.mode) == entry.group,
                   "blend group assignment");
  }

  /* 分组表本身必须自洽：组号连续、组内档位严格递增、全体恰好覆盖 0..23 一次。 */
  bool seen[comp_ui::kBlendCount] = {};
  for (int group = 0; group < comp_ui::kBlendGroupCount; ++group) {
    int previous = -1;
    for (int slot = 0; slot < 8; ++slot) {
      const int mode = comp_ui::kBlendGroups[group][slot];
      if (mode < 0) break;
      COMP_CHECK(mode > previous); /* 组内按 allCases 顺序排列 */
      COMP_CHECK(!seen[mode]);     /* 不得重复覆盖 */
      seen[mode] = true;
      previous = mode;
    }
  }
  for (int mode = 0; mode < comp_ui::kBlendCount; ++mode) {
    COMP_CHECK(seen[mode]);
  }
  /* 分组名非空，供状态栏显示。 */
  for (int group = 0; group < comp_ui::kBlendGroupCount; ++group) {
    COMP_CHECK(std::string(comp_ui::blend_group_name(group)).empty() == false);
  }
}

COMP_TEST(ui_core_blend_mode_steps_and_wraps_like_upstream) {
  /* clamp：负值与越界一律折回合法区间，不得把 999 这类脏值传给 ABI。 */
  COMP_CHECK(comp_ui::clamp_blend_mode(-5) == 0);
  COMP_CHECK(comp_ui::clamp_blend_mode(0) == 0);
  COMP_CHECK(comp_ui::clamp_blend_mode(23) == 23);
  COMP_CHECK(comp_ui::clamp_blend_mode(24) == 23);
  COMP_CHECK(comp_ui::clamp_blend_mode(999) == 23);
  COMP_CHECK(comp_ui::clamp_blend_mode(comp_ui::kBlendNormal) == comp_ui::kBlendNormal);

  /* 步进回绕（对齐上游 cycleBlendMode 的取模）：Normal 后退到 Luminosity。 */
  COMP_CHECK(comp_ui::step_blend_mode(0, -1) == 23);
  COMP_CHECK(comp_ui::step_blend_mode(0, 1) == 1);
  COMP_CHECK(comp_ui::step_blend_mode(23, 1) == 0);
  COMP_CHECK(comp_ui::step_blend_mode(23, -1) == 22);

  /* direction = 0 是幂等查询，不应改动档位。 */
  COMP_CHECK(comp_ui::step_blend_mode(7, 0) == 7);
  COMP_CHECK(comp_ui::step_blend_mode(0, 0) == 0);

  /* 连续多步必须与单步逐次等价（回绕不能有偏差）。 */
  int stepped = comp_ui::kBlendNormal;
  for (int i = 0; i < comp_ui::kBlendCount; ++i) {
    stepped = comp_ui::step_blend_mode(stepped, 1);
  }
  COMP_CHECK(stepped == comp_ui::kBlendNormal); /* 走完一轮回到起点 */
  for (int i = 0; i < comp_ui::kBlendCount + 3; ++i) {
    stepped = comp_ui::step_blend_mode(stepped, 1);
  }
  COMP_CHECK(stepped == 3); /* 再过三轮 + 3 */
}

/* ---------------------------------------------------------- 图层变换（S21） */

/*
 * 步进量与夹取的纯函数层：壳侧据此写入 comp_layer_set_transform，非法值由引擎拒。
 * 这里只保证「壳算出来的值一定是引擎肯收的合法值」与「中心锚定」两条口径。
 */
COMP_TEST(ui_core_layer_transform_move_steps) {
  COMP_CHECK(comp_ui::layer_transform_move_step(false) == 1);
  COMP_CHECK(comp_ui::layer_transform_move_step(true) == 10);
  /* 步进量必须是常量，不随任何状态变化。 */
  COMP_CHECK(comp_ui::kLayerTransformMoveStep == 1);
  COMP_CHECK(comp_ui::kLayerTransformMoveCoarse == 10);
}

COMP_TEST(ui_core_layer_transform_rotate_steps_and_stays_finite) {
  /* 15 度一档（对齐上游方向/修饰键的旋转微调粒度）。 */
  COMP_CHECK(comp_ui::layer_transform_rotate_step(0.0, false) == 15.0);
  COMP_CHECK(comp_ui::layer_transform_rotate_step(15.0, false) == 30.0);
  /* 精调档 1 度。 */
  COMP_CHECK(near_value(comp_ui::layer_transform_rotate_step(0.0, true), 1.0));
  /* 连续性：可以超过一圈，上游 isValid 只要求 finite。 */
  double rotated = 0.0;
  for (int i = 0; i < 24; ++i) rotated = comp_ui::layer_transform_rotate_step(rotated, false);
  COMP_CHECK(near_value(rotated, 360.0));
  COMP_CHECK(rotated > -1e18 && rotated < 1e18); /* 保持有限，引擎才肯收 */

  /* NaN / inf 一律归一化到 0：脏值不进 ABI（否则 is_valid_transform 拒，面板分叉）。 */
  COMP_CHECK(comp_ui::layer_transform_rotate_step(std::numeric_limits<double>::quiet_NaN(),
                                                  false) == 0.0);
  COMP_CHECK(comp_ui::layer_transform_rotate_step(
                 std::numeric_limits<double>::infinity(), false) == 0.0);
  COMP_CHECK(comp_ui::layer_transform_rotate_step(
                 -std::numeric_limits<double>::infinity(), false) == 0.0);
}

COMP_TEST(ui_core_layer_transform_scale_clamps_and_holds_center) {
  /* 夹取：NaN 与负值落到下限，超上限落到上限（对齐 is_valid_transform 的 size 1..300000）。 */
  COMP_CHECK(comp_ui::clamp_layer_scale(-5.0) == comp_ui::kLayerTransformScaleMin);
  COMP_CHECK(comp_ui::clamp_layer_scale(std::numeric_limits<double>::quiet_NaN()) ==
             comp_ui::kLayerTransformScaleMin);
  COMP_CHECK(comp_ui::clamp_layer_scale(1.0) == 1.0);
  COMP_CHECK(comp_ui::clamp_layer_scale(99.0) == comp_ui::kLayerTransformScaleMax);

  /* 中心锚定：缩放前后中心不变——这是拖动角缩放的行为，不是左上角固定。 */
  int32_t x = 100, y = 50, w = 200, h = 100;
  const double cx = 200.0, cy = 100.0;
  comp_ui::layer_transform_scale_rect(&x, &y, &w, &h, 1.5);
  COMP_CHECK(w == 300 && h == 150);
  COMP_CHECK(near_value(static_cast<double>(x) + w / 2.0, cx));
  COMP_CHECK(near_value(static_cast<double>(y) + h / 2.0, cy));
  COMP_CHECK(x == 50 && y == 25);

  /* 缩到下限附近：尺寸不得跌破 1 像素（引擎下限）。 */
  x = 100; y = 50; w = 10; h = 10;
  comp_ui::layer_transform_scale_rect(&x, &y, &w, &h, 0.001);
  COMP_CHECK(w >= 1 && h >= 1);
  COMP_CHECK(comp_ui::clamp_layer_scale(0.001) == comp_ui::kLayerTransformScaleMin);

  /* 非法尺寸本身就是空操作，不得写出退化矩形。 */
  x = 0; y = 0; w = 0; h = 0;
  comp_ui::layer_transform_scale_rect(&x, &y, &w, &h, 2.0);
  COMP_CHECK(w == 0 && h == 0); /* 非正尺寸原样返回，交给调用方拒绝 */

  /* 缩放档位与双向对称：+1% 再 -1% 回到原尺寸（取整造成的 1 px 误差可接受）。 */
  x = 100; y = 50; w = 200, h = 100;
  comp_ui::layer_transform_scale_rect(&x, &y, &w, &h, 1.0 + comp_ui::kLayerTransformScaleStep);
  comp_ui::layer_transform_scale_rect(&x, &y, &w, &h, 1.0 / (1.0 + comp_ui::kLayerTransformScaleStep));
  COMP_CHECK(std::abs(w - 200) <= 1 && std::abs(h - 100) <= 1);

  /* 空指针不得崩。 */
  comp_ui::layer_transform_scale_rect(nullptr, nullptr, nullptr, nullptr, 1.0);
}

/* ------------------------------------------------------- 采样模式（S22） */

/*
 * 采样档位表：名称与数量必须和 ABI 的 comp_sampling 冻结表一致。不一致的后果
 * 不是这里 FAIL，而是菜单写出一个引擎不认识的档位、被参数闸拒绝——而用户只在
 * 点下去之后才看到失败。
 */
COMP_TEST(ui_core_sampling_table_matches_abi_enum) {
  COMP_CHECK(comp_ui::kSamplingCount == 3);
  COMP_CHECK(comp_ui::kSamplingHighQuality == 0);
  COMP_CHECK(comp_ui::kSamplingNearest == 1);
  COMP_CHECK(comp_ui::kSamplingSmooth == 2);

  COMP_CHECK(std::string(comp_ui::sampling_mode_name(0)) == "High quality");
  COMP_CHECK(std::string(comp_ui::sampling_mode_name(1)) == "Nearest");
  COMP_CHECK(std::string(comp_ui::sampling_mode_name(2)) == "Smooth");

  /* 夹取：负值回落 High quality、越界回落最后一档。 */
  COMP_CHECK(comp_ui::clamp_sampling_mode(-7) == 0);
  COMP_CHECK(comp_ui::clamp_sampling_mode(99) == comp_ui::kSamplingCount - 1);
  COMP_CHECK(comp_ui::sampling_mode_name(-7) == std::string("High quality"));
  COMP_CHECK(comp_ui::sampling_mode_name(99) == std::string("Smooth"));
}

/* ---------------------------------------------------- 面板行索引与偏移（S22） */

/*
 * clamp_selected_row：重排 / 删层后 selected_layer 是面板序索引，越界会让
 * selected_row 返回空指针，后续快捷键静默失效——「选了但按了没反应」最难排查。
 */
COMP_TEST(ui_core_clamp_selected_row_never_leaves_the_panel) {
  COMP_CHECK(comp_ui::clamp_selected_row(5, 0) == 0); /* 空面板不崩、不给负数 */
  COMP_CHECK(comp_ui::clamp_selected_row(-1, 0) == 0);

  const int32_t count = 4;
  COMP_CHECK(comp_ui::clamp_selected_row(-3, count) == 0);
  COMP_CHECK(comp_ui::clamp_selected_row(100, count) == count - 1);
  COMP_CHECK(comp_ui::clamp_selected_row(0, count) == 0);
  COMP_CHECK(comp_ui::clamp_selected_row(2, count) == 2);
  COMP_CHECK(comp_ui::clamp_selected_row(count - 1, count) == count - 1);

  /* 单层面板：0 是唯一合法值。盯「删到只剩一层后索引没收口」。 */
  COMP_CHECK(comp_ui::clamp_selected_row(3, 1) == 0);
}

/*
 * 直方图打开时图层行整体下移：绘制与命中共用同一偏移量。两处不一致的症状是
 * 「点不中要的那一层」，且只在面板开着直方图时出现，极难复现——所以这里同时
 * 正向断言（同一屏幕点命中下一行）与反面断言（面板外/表头区不命中）。
 */
COMP_TEST(ui_core_layer_rows_hit_test_respects_histogram_offset) {
  const Rect panel{0, 46, 248, 400};
  const int header = comp_ui::kLayerHeaderHeight;
  const int row = comp_ui::kLayerRowHeight;
  const int row_top = comp_ui::Layout::kHistogramHeight;

  const double y_row0 = panel.y + header + row / 2;
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, y_row0, 0) == 0);
  /*
   * 同一屏幕点在直方图打开时落到表头区里（y_row0 距顶只有 header + row/2，
   * 而 row_top 把第一个可命中位置压到 header + kHistogramHeight 处）——
   * 于是它不再是任何一行。这正是偏移真的生效的证据：不是「照样命中第 0 行」。
   */
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, y_row0, row_top) == -1);

  /*
   * 有偏移时，第 0 行的中心点下移了整整一个直方图高度，仍然命中第 0 行。
   * 这条与上面那条合起来才构成「偏移生效且只影响起点」的完整判据。
   */
  const double y_row0_shifted = panel.y + header + row_top + row / 2;
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, y_row0_shifted, row_top) == 0);
  /*
   * 无偏移时同一个点落在第 4 行位置——面板只有 3 层，所以不在任何行内（-1）。
   * 这一条同时盯住「偏移让起点后移」与「界外不得回绕到第 0 行」两件事。
   */
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, y_row0_shifted, 0) == -1);

  /* 偏移下第 1 行的中心点，仍然命中第 1 行（不是整体平移后串行）。 */
  const double y_row1_shifted = panel.y + header + row_top + row + row / 2;
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, y_row1_shifted, row_top) == 1);

  const double y_row1 = panel.y + header + row + row / 2;
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, y_row1, 0) == 1);

  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, panel.y + 2, 0) == -1);
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x - 5, y_row0, 0) == -1);
  COMP_CHECK(comp_ui::layer_row_at(panel, 0, panel.x + 10, y_row0, 0) == -1);

  /*
   * 正面钉「同一几何」：layer_row_rect(panel, row, row_top) 的中心点必须被
   * layer_row_at(..., row_top) 判回同一个 row。上面的断言只证明偏移存在，
   * 这一条证明绘制与命中用的是同一个偏移。
   */
  for (int row_index = 0; row_index < 3; ++row_index) {
    const Rect rect = comp_ui::layer_row_rect(panel, row_index, row_top);
    const double mid = rect.y + comp_ui::kLayerRowHeight / 2;
    COMP_CHECK(comp_ui::layer_row_at(panel, 3, rect.x + rect.w / 2, mid, row_top) ==
               row_index);
  }
}

/* ---------------------------------------------------------------- S23 */

/*
 * 按引擎语义重演一次重排：engine/domain/document.cpp 的 reorder_layer 是
 * 「先在 state_.layers 里 erase 自己，再把 id 插到锚点 **之前**」，数组序自下而上、
 * 尾 = 顶层，before == 0 表示同级末尾。
 *
 * 测试里必须**独立重演**这一步，而不是复述 plan 的输出：只断言 plan 自己算出来的
 * 数字，等于用被测对象证明被测对象。这里按文档化语义另写一份最小实现，再拿
 * 「用户在面板上看到什么」去比。
 */
std::vector<int64_t> simulate_engine_reorder(const std::vector<int64_t>& bottom_to_top,
                                             int32_t shown, int32_t direction) {
  std::vector<int64_t> layers = bottom_to_top;
  const int32_t count = static_cast<int32_t>(layers.size());
  const comp_ui::ReorderPlan plan = comp_ui::plan_layer_reorder(count, shown, direction);
  if (!plan.ok) return layers; /* 已在端头：不产生任何变更 */

  const int32_t index = comp_ui::layer_array_index(count, shown);
  const int64_t self = layers[static_cast<size_t>(index)];
  int64_t before = 0;
  if (plan.before_row >= 0) {
    before = layers[static_cast<size_t>(comp_ui::layer_array_index(count, plan.before_row))];
  }
  layers.erase(layers.begin() + index);
  if (before == 0) {
    layers.push_back(self);
  } else {
    auto pos = std::find(layers.begin(), layers.end(), before);
    layers.insert(pos, self);
  }
  return layers;
}

std::vector<int64_t> panel_order_of(const std::vector<int64_t>& bottom_to_top) {
  return std::vector<int64_t>(bottom_to_top.rbegin(), bottom_to_top.rend());
}

/*
 * 重排计划 = 用户可见的「上移 / 下移一行」，表驱动穷举 2~3 层 × 4 个方向。
 *
 * 这一条正是 S22 漏掉的那层：panel_* 里没有任何测试，于是「上移 / 下移整对写反」
 * 加「选中态按相反假设跟随」两个错误互相抵消着进了主干——不报错、自检也过，只有
 * 真去排序的人才发现层往反方向跑。现在两个方向都必须与「面板上挪一行 / 下挪一行」
 * 一致，且 next_row 必须指向被移动的那一层。
 */
COMP_TEST(ui_core_layer_reorder_matches_engine_semantics) {
  const int32_t directions[4] = {-2, -1, 1, 0}; /* 置顶 / 上移 / 下移 / 置底 */
  for (int32_t count = 2; count <= 3; ++count) {
    for (int32_t shown = 0; shown < count; ++shown) {
      for (int32_t direction : directions) {
        /* 数组序自下而上：10 是底层；面板行序相反（行 0 是顶层）。 */
        std::vector<int64_t> bottom = {10, 11, 12};
        bottom.resize(static_cast<size_t>(count));
        const std::vector<int64_t> panel = panel_order_of(bottom);
        const int64_t self = panel[static_cast<size_t>(shown)];

        /* 期望直接按「这一层在面板上应该落到第几行」写，不经过数组下标换算。 */
        int32_t target = shown;
        if (direction == -2) {
          target = 0;
        } else if (direction == 0) {
          target = count - 1;
        } else if (direction < 0) {
          target = shown - 1;
        } else {
          target = shown + 1;
        }
        if (target < 0 || target >= count) target = shown; /* 端头不动 */

        std::vector<int64_t> expected = panel;
        expected.erase(expected.begin() + shown);
        expected.insert(expected.begin() + target, self);

        const std::vector<int64_t> actual =
            panel_order_of(simulate_engine_reorder(bottom, shown, direction));
        COMP_CHECK(actual == expected);

        const comp_ui::ReorderPlan plan =
            comp_ui::plan_layer_reorder(count, shown, direction);
        if (target == shown) {
          COMP_CHECK(!plan.ok); /* 端头必须如实拒绝，不静默无操作 */
        } else {
          COMP_CHECK(plan.ok);
          COMP_CHECK(plan.next_row == target); /* 选中态跟着这一层走 */
        }
      }
    }
  }

  /* 空文档不产生计划。 */
  COMP_CHECK(!comp_ui::plan_layer_reorder(0, 0, -1).ok);
  COMP_CHECK(!comp_ui::plan_layer_reorder(0, 0, 0).ok);

  /* 选中行越界时按夹取后的行算（99 → 最后一行 2，即数组最底层），上移落在行 1。 */
  const comp_ui::ReorderPlan clamped = comp_ui::plan_layer_reorder(3, 99, -1);
  COMP_CHECK(clamped.ok);
  COMP_CHECK(clamped.next_row == 1);
}

/*
 * 剪贴基底必须是「下方紧邻的同父图层」——引擎成栈的唯一条件
 * （filter_graph.cpp: siblings[i+1+clip_run]->clipping_source == layer->id）。
 *
 * S22 取成了数组上方一层（self_index + 1），守卫却拒绝顶层、提示却写「最底层无法
 * 剪贴」。取反之后剪贴栈永远不成形：FilterGraph 画面不变，而 compositor.cpp 的单层
 * 视图只看 clipping_source != 0 就按基底求值——同一份文档在不同视图下画得不一样。
 */
COMP_TEST(ui_core_clipping_base_is_the_sibling_below) {
  /* 数组序自下而上：100(底) / 101 / 102(顶)；面板行 0 = 102。 */
  const comp_ui::LayerStackEntry flat[3] = {{100, 0, 0}, {101, 0, 0}, {102, 0, 0}};

  /* 选中顶层（行 0）→ 基底是下方那层 101（数组下标 1），绝不是它上方或它自己。 */
  comp_ui::ClippingPlan plan = comp_ui::plan_clipping_base(flat, 3, 0);
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.base_index == 1);
  COMP_CHECK(!plan.joins_existing_stack);

  /* 中间层（行 1）→ 基底是下方那层 100。 */
  plan = comp_ui::plan_clipping_base(flat, 3, 1);
  COMP_CHECK(plan.ok() && plan.base_index == 0);

  /* 最底层（行 count-1）下方没有图层：如实拒绝，原因必须是「下方没有」而非顶层。 */
  plan = comp_ui::plan_clipping_base(flat, 3, 2);
  COMP_CHECK(!plan.ok());
  COMP_CHECK(plan.target == comp_ui::ClippingTarget::kNoSiblingBelow);

  /* 退化输入。 */
  COMP_CHECK(!comp_ui::plan_clipping_base(nullptr, 3, 0).ok());
  COMP_CHECK(!comp_ui::plan_clipping_base(flat, 0, 0).ok());
}

/*
 * 有图层组时，「下方一层」必须是**同父**的下方一层：filter_graph 的 siblings 只含
 * 同父层，全数组相邻不等于同父相邻。这里同时覆盖「并入已有剪贴栈」与「异常链」。
 */
COMP_TEST(ui_core_clipping_base_respects_groups_and_stack) {
  /*
   * 全数组序（自下而上）：
   *   下标 0: 100 根级
   *   下标 1: 200 根级（组）
   *   下标 2: 201 parent=200
   *   下标 3: 202 parent=200
   *   下标 4: 103 根级
   * 面板行：行 0 = 103，行 1 = 202，行 2 = 201，行 3 = 200，行 4 = 100。
   */
  const comp_ui::LayerStackEntry mixed[5] = {
      {100, 0, 0}, {200, 0, 0}, {201, 200, 0}, {202, 200, 0}, {103, 0, 0}};

  /*
   * 选中 103（行 0）：同父下方最近的是**组图层 200**（下标 1，parent=0），必须跳过
   * 夹在中间的组内层 201 / 202（下标 2、3）。200 本身是根级层，剪到它是合法的，
   * 所以正确基底是 1 而不是 0——这一条同时钉住「跳过别组层」与「组本身可作为基底」。
   */
  comp_ui::ClippingPlan plan = comp_ui::plan_clipping_base(mixed, 5, 0);
  COMP_CHECK(plan.ok() && plan.base_index == 1);
  /* 反证：全局数组里紧邻 103 下方的是组内层 202（下标 3），取它就会跨父成栈失败。 */
  COMP_CHECK(plan.base_index != 3);

  /* 选中组内 202（行 1）：同父下方是 201（下标 2），不是全局相邻的 200（下标 1）。 */
  plan = comp_ui::plan_clipping_base(mixed, 5, 1);
  COMP_CHECK(plan.ok() && plan.base_index == 2);

  /* 选中组内 201（行 2）：它的同父下方没有别的组内层 → 拒。 */
  plan = comp_ui::plan_clipping_base(mixed, 5, 2);
  COMP_CHECK(!plan.ok() && plan.target == comp_ui::ClippingTarget::kNoSiblingBelow);

  /*
   * 并入已有剪贴栈：C 已剪到 A（数组 A,B,C,D），此时选 D 应把基底解析成 A，
   * 而不是链式地剪到 C——链式会让 D 落在 FilterGraph 的栈外，而单层视图仍按它求值。
   */
  const comp_ui::LayerStackEntry chained[4] = {{10, 0, 0}, {11, 0, 0}, {12, 0, 10}, {13, 0, 0}};
  plan = comp_ui::plan_clipping_base(chained, 4, 0); /* 行 0 = 13（下标 3） */
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.base_index == 0);
  COMP_CHECK(plan.joins_existing_stack);

  /* 异常：B 的剪贴基底指向它上方的 C（越级 / 反向引用）→ 拒绝，不退化成链。 */
  const comp_ui::LayerStackEntry backward[3] = {{20, 0, 0}, {21, 0, 22}, {22, 0, 0}};
  plan = comp_ui::plan_clipping_base(backward, 3, 0); /* 行 0 = 22（下标 2） */
  COMP_CHECK(!plan.ok());
  COMP_CHECK(plan.target == comp_ui::ClippingTarget::kBrokenStack);

  /* 基底 id 根本不存在同样算异常。 */
  const comp_ui::LayerStackEntry missing[3] = {{30, 0, 0}, {31, 0, 999}, {32, 0, 0}};
  plan = comp_ui::plan_clipping_base(missing, 3, 0);
  COMP_CHECK(!plan.ok() && plan.target == comp_ui::ClippingTarget::kBrokenStack);

  /*
   * id 必须按 64 位处理（comp_layer_id 是 uint64_t）：若在接口上窄化成 int32_t，
   * 高位 id 会被截断，两个不同图层可能撞成同一个值，'基底是哪一层'就会答错。
   * 这里用 2^40 量级的 id 走一遍「并入已有栈」的路径，把位宽钉死。
   */
  const int64_t wide = static_cast<int64_t>(1) << 40;
  const comp_ui::LayerStackEntry wide_ids[3] = {
      {wide, 0, 0}, {wide + 1, 0, wide}, {wide + 2, 0, 0}};
  plan = comp_ui::plan_clipping_base(wide_ids, 3, 0); /* 行 0 = wide+2（下标 2） */
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.base_index == 0);
  COMP_CHECK(plan.joins_existing_stack);
}

/*
 * 直方图几何：块贴标题行下方，块底与第一行图层严丝合缝。
 *
 * S22 的块顶取 panel.y，块内标签落在 panel.y+10，和标题的 panel.y+8 叠字；行起点
 * 却在 +94，中间空出 30px。根因是三处各算一套坐标，这里钉死唯一来源。
 */
COMP_TEST(ui_core_histogram_geometry_clears_title_and_rows) {
  const Rect panel{0, 46, 248, 600};
  const Rect block = comp_ui::histogram_rect(panel);
  const Rect label = comp_ui::histogram_label_rect(panel);
  const Rect plot = comp_ui::histogram_plot_rect(panel);
  const int rows_top = comp_ui::layer_rows_top(true);

  /* 不压标题：标题行占 panel.y .. panel.y + kLayerHeaderHeight。 */
  COMP_CHECK(block.y >= panel.y + comp_ui::kLayerHeaderHeight);
  COMP_CHECK(label.y >= panel.y + 24); /* 标题文字在 +8，高约一行 */

  /* 块内不叠：标签与绘图区互不重叠，且都落在块内。 */
  COMP_CHECK(!rects_overlap(label, plot));
  COMP_CHECK(label.bottom() <= plot.y);
  COMP_CHECK(label.y >= block.y && label.bottom() <= block.bottom());
  COMP_CHECK(plot.bottom() == block.bottom());

  /* 不留空档：块底 == 图层行起点；关掉直方图时行偏移归零。 */
  COMP_CHECK(block.bottom() == panel.y + comp_ui::kLayerHeaderHeight + rows_top);
  COMP_CHECK(comp_ui::layer_rows_top(false) == 0);

  /* 绘制与命中同一几何：第一行中心点判回第 0 行，块内任意点不落到任何行。 */
  const Rect row0 = comp_ui::layer_row_rect(panel, 0, rows_top);
  const double mid0 = row0.y + comp_ui::kLayerRowHeight / 2.0;
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, row0.x + row0.w / 2.0, mid0, rows_top) == 0);
  COMP_CHECK(comp_ui::layer_row_at(panel, 3, panel.x + 10, block.y + 1, rows_top) == -1);
}

/* ============================================================ S24 用例 */

namespace {

/* 按引擎「先 erase 后插到锚点之前」语义复算一次拖拽，返回落定后的数组（自下而上）。 */
std::vector<int64_t> simulate_layer_drag(const std::vector<int64_t>& array,
                                         const comp_ui::DragPlan& plan, int32_t from_shown) {
  if (!plan.ok()) return array;
  const int32_t from_index = static_cast<int32_t>(array.size()) - 1 - from_shown;
  if (from_index < 0 || from_index >= static_cast<int32_t>(array.size())) return array;
  const int64_t dragged = array[static_cast<size_t>(from_index)];
  const int64_t anchor =
      plan.before_index < 0 ? 0 : array[static_cast<size_t>(plan.before_index)];
  std::vector<int64_t> result = array;
  result.erase(result.begin() + from_index);
  if (anchor == 0) {
    /* comp_layer_reorder 的 before == 0 = 同级末尾（栈顶）。 */
    result.push_back(dragged);
  } else {
    const auto it = std::find(result.begin(), result.end(), anchor);
    result.insert(it == result.end() ? result.end() : it, dragged);
  }
  return result;
}

int32_t shown_row_of(const std::vector<int64_t>& array, int64_t id) {
  for (size_t i = 0; i < array.size(); ++i) {
    if (array[i] == id) return static_cast<int32_t>(array.size() - 1 - i);
  }
  return -1;
}

}  // namespace

/*
 * 菜单勾选态（S24）。
 *
 * Win32 的 CheckMenuItem 本身不可测，可测的是「该勾哪几项」的判定规则：
 * 无选中层不能勾、非组的 isolated 置 1 也不算、越界档位必须先夹。规则搬进
 * ui_core 就是为了能在这里表驱动穷举——否则「勾错了」只能靠肉眼开菜单看。
 */
COMP_TEST(ui_core_layer_menu_checks_follow_state_rules) {
  /* 1. 无选中层：除直方图（文档级视图态）外一律不勾，采样/混合显式 -1。 */
  comp_ui::LayerMenuState state;
  comp_ui::LayerMenuChecks checks = comp_ui::layer_menu_checks(state);
  COMP_CHECK(!checks.visible && !checks.clipping && !checks.isolated && !checks.histogram);
  COMP_CHECK_MSG(checks.sampling == -1, "无选中层时采样不得勾任何一项");
  COMP_CHECK_MSG(checks.blend == -1, "无选中层时混合不得勾任何一项");

  state.histogram_open = true;
  checks = comp_ui::layer_menu_checks(state);
  COMP_CHECK(checks.histogram);
  COMP_CHECK(!checks.visible);

  /* 2. 有选中层：可见/剪贴如实透传；isolated 只在「是组」时才有意义。 */
  state.has_layer = true;
  state.visible = true;
  state.clipping = true;
  state.isolated = true; /* 非组层置 1：引擎不参与合成，勾上即误导 */
  state.sampling = 2;
  state.blend = 7;
  checks = comp_ui::layer_menu_checks(state);
  COMP_CHECK(checks.visible && checks.clipping);
  COMP_CHECK_MSG(!checks.isolated, "非组层的 isolated 不得勾");
  COMP_CHECK(checks.sampling == 2 && checks.blend == 7);

  state.is_group = true;
  checks = comp_ui::layer_menu_checks(state);
  COMP_CHECK(checks.isolated);

  /* 3. 越界档位先夹再勾：绝不能把不存在的档位勾上（勾错档 = 用户以为模式变了）。 */
  state.sampling = -5;
  state.blend = 999;
  checks = comp_ui::layer_menu_checks(state);
  COMP_CHECK(checks.sampling == comp_ui::clamp_sampling_mode(-5));
  COMP_CHECK(checks.blend == comp_ui::clamp_blend_mode(999));
  COMP_CHECK(checks.sampling >= 0 && checks.sampling < comp_ui::kSamplingCount);
  COMP_CHECK(checks.blend >= 0 && checks.blend < comp_ui::kBlendCount);
  COMP_CHECK(checks.sampling == 0);
  COMP_CHECK(checks.blend == comp_ui::kBlendCount - 1);
}

/*
 * 效果档位表（S24）。
 *
 * 数值 0..5 是 .comp 的 engineEffects[].effect，顺序必须与引擎落笔判定顺序
 * （effect_eval.cpp 的 kEffectStroke..kEffectInnerGlow）逐字一致：菜单 id 用
 * kCmdEffectBase + kind 拼出来，表错一位就是「点描边结果加了投影」。
 */
COMP_TEST(ui_core_effect_kind_table_matches_engine_order) {
  const char* expected[] = {"Stroke",  "Drop Shadow", "Color Overlay",
                            "Inner Shadow", "Outer Glow", "Inner Glow"};
  COMP_CHECK(comp_ui::kEffectKindCount == 6);
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    COMP_CHECK_MSG(std::string(comp_ui::effect_kind_name(kind)) == expected[kind],
                   "效果档位顺序必须与引擎一致");
  }

  /* 越界夹取：负 -> 首项，超界 -> 末项（菜单 id 永不指向不存在的档位）。 */
  COMP_CHECK(comp_ui::clamp_effect_kind(-3) == 0);
  COMP_CHECK(comp_ui::clamp_effect_kind(comp_ui::kEffectKindCount) ==
             comp_ui::kEffectKindCount - 1);
  COMP_CHECK(std::string(comp_ui::effect_kind_name(-1)) == "Stroke");
  COMP_CHECK(std::string(comp_ui::effect_kind_name(99)) == "Inner Glow");

  /* 勾选判定：null / 空表必须 false，命中与未命中都要对。 */
  const uint32_t kinds[] = {2u, 5u};
  COMP_CHECK(!comp_ui::effect_list_contains(nullptr, 3, 0));
  COMP_CHECK(!comp_ui::effect_list_contains(kinds, 0, 2));
  COMP_CHECK(comp_ui::effect_list_contains(kinds, 2, 2));
  COMP_CHECK(comp_ui::effect_list_contains(kinds, 2, 5));
  COMP_CHECK(!comp_ui::effect_list_contains(kinds, 2, 1));
  /* 负 kind 被夹到 0，但 0 不在表里 —— 夹取不得变成「误命中」。 */
  COMP_CHECK(!comp_ui::effect_list_contains(kinds, 2, -1));
}

/*
 * 效果参数面板（P0-2）的行几何与步长。
 *
 * 绘制和命中都用 effect_param_row_rect：这里逐行对账，防「看得见点不中」；
 * 步长按量程决定（归一量 0.05 / 像素与角度 1），夹取必须永不产生越界值——
 * 面板夹取是 ABI 拒绝之前的最后一道，越界值会让状态栏只报一句「参数不合法」。
 */
COMP_TEST(ui_core_effect_param_panel_geometry_and_steps) {
  const comp_ui::Rect panel{1000, 100, 220, 600};
  COMP_CHECK(comp_ui::kEffectRowsTop == 54);
  COMP_CHECK(comp_ui::kEffectRowHeight == 20);

  const comp_ui::Rect first = comp_ui::effect_param_row_rect(panel, 0);
  COMP_CHECK(first.x == panel.x && first.w == panel.w && first.h == comp_ui::kEffectRowHeight);
  COMP_CHECK(first.y == panel.y + comp_ui::kEffectRowsTop);
  const comp_ui::Rect third = comp_ui::effect_param_row_rect(panel, 2);
  COMP_CHECK(third.y == first.y + 2 * comp_ui::kEffectRowHeight);

  /* 行首 / 行中 / 行尾都必须命中同一行。 */
  for (int row = 0; row < 4; ++row) {
    const comp_ui::Rect rect = comp_ui::effect_param_row_rect(panel, row);
    const double x = static_cast<double>(rect.x + 2);
    COMP_CHECK(comp_ui::effect_param_row_at(panel, 7, x, static_cast<double>(rect.y)) == row);
    COMP_CHECK(comp_ui::effect_param_row_at(panel, 7, x, static_cast<double>(rect.y) +
                                                             rect.h / 2.0) == row);
    COMP_CHECK(comp_ui::effect_param_row_at(panel, 7, x,
                                            static_cast<double>(rect.bottom()) - 1.0) == row);
  }
  /* 标题区 / 面板外 / 参数行数之外都不是参数行。 */
  COMP_CHECK(comp_ui::effect_param_row_at(panel, 7, static_cast<double>(first.x + 2),
                                          static_cast<double>(panel.y + 10)) == -1);
  COMP_CHECK(comp_ui::effect_param_row_at(panel, 7, static_cast<double>(panel.right() + 4),
                                          static_cast<double>(first.y)) == -1);
  COMP_CHECK(comp_ui::effect_param_row_at(panel, 0, static_cast<double>(first.x + 2),
                                          static_cast<double>(first.y)) == -1);
  COMP_CHECK(comp_ui::effect_param_row_at(panel, 7, static_cast<double>(first.x + 2),
                                          static_cast<double>(first.y) +
                                              7 * comp_ui::kEffectRowHeight + 2.0) == -1);

  /* 步长：归一量 0.05，像素/角度 1（不是给每个参数再抄一份 step 字段）。 */
  COMP_CHECK(comp_ui::effect_param_step(0.0, 1.0) == 0.05);
  COMP_CHECK(comp_ui::effect_param_step(0.0, 500.0) == 1.0);
  COMP_CHECK(comp_ui::effect_param_step(-360.0, 360.0) == 1.0);
  COMP_CHECK(comp_ui::step_effect_param(4.0, 1, 0.0, 500.0) == 5.0);
  COMP_CHECK(comp_ui::step_effect_param(4.0, -1, 0.0, 500.0) == 3.0);
  /* 两端夹取：0.98 + 0.05 不得变成 1.03，0.02 - 0.05 不得变成负数。 */
  COMP_CHECK(comp_ui::step_effect_param(0.98, 1, 0.0, 1.0) == 1.0);
  COMP_CHECK(comp_ui::step_effect_param(0.02, -1, 0.0, 1.0) == 0.0);
  /* direction 0 = 只夹取（回显用），合法值必须原样返回。 */
  COMP_CHECK(comp_ui::step_effect_param(0.37, 0, 0.0, 1.0) == 0.37);
  COMP_CHECK(comp_ui::clamp_effect_param(1e9, 0.0, 1.0) == 1.0);
  COMP_CHECK(comp_ui::clamp_effect_param(-1e9, 0.0, 500.0) == 0.0);
  COMP_CHECK(comp_ui::clamp_effect_param(std::nan(""), 0.0, 500.0) == 0.0);
}

/*
 * 拖行重排（S24）：plan_layer_drag 必须与引擎 comp_layer_reorder 的语义逐位对齐。
 *
 * 这里不「相信」实现里的注释，而是用 erase+insert 独立复算一遍落点，再和
 * plan.next_row 对账——注释写错、下标差一、拖到顶时 anchor=-1 的口径弄反，
 * 都会在这里露出来。跨父级必须如实拒绝（面板不画缩进，静默改父子关系等于「层没了」）。
 */
COMP_TEST(ui_core_plan_layer_drag_keeps_engine_semantics) {
  /* 5 个同父图层，数组自下而上；面板行 0 = 105（栈顶）。 */
  const comp_ui::LayerStackEntry same_parent[5] = {
      {101, 0, 0}, {102, 0, 0}, {103, 0, 0}, {104, 0, 0}, {105, 0, 0}};
  const std::vector<int64_t> array = {101, 102, 103, 104, 105};
  const int32_t count = 5;

  for (int32_t from = 0; from < count; ++from) {
    for (int32_t to = 0; to < count; ++to) {
      const comp_ui::DragPlan plan = comp_ui::plan_layer_drag(same_parent, count, from, to);
      if (from == to) {
        COMP_CHECK_MSG(!plan.ok(), "落点即原行：不得产生写入");
        COMP_CHECK(plan.target == comp_ui::DragTarget::kSameRow);
        COMP_CHECK(plan.before_index == -1);
        COMP_CHECK(plan.next_row == from);
        continue;
      }
      COMP_CHECK(plan.ok());
      const int64_t dragged = array[static_cast<size_t>(count - 1 - from)];
      const std::vector<int64_t> after = simulate_layer_drag(array, plan, from);
      COMP_CHECK_MSG(shown_row_of(after, dragged) == to, "落点行必须是 to_shown");
      COMP_CHECK_MSG(shown_row_of(after, dragged) == plan.next_row,
                     "选中态必须跟到 next_row（否则拖完选错层）");
      /* 未参与拖拽的元素相对顺序不得变化（拖一层不能顺手重排别层）。 */
      std::vector<int64_t> others_before;
      for (int64_t id : array) {
        if (id != dragged) others_before.push_back(id);
      }
      std::vector<int64_t> others_after;
      for (int64_t id : after) {
        if (id != dragged) others_after.push_back(id);
      }
      COMP_CHECK(others_before == others_after);
    }
  }

  /* 拖到最顶 = 锚点取「同级末尾」(-1)；其余方向锚点是当前数组里的真实下标。 */
  comp_ui::DragPlan plan = comp_ui::plan_layer_drag(same_parent, count, 4, 0);
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.before_index == -1);
  COMP_CHECK(plan.next_row == 0);

  /* 跨父级：如实拒绝，next_row 停原位（不回 0、不乱跳）。 */
  const comp_ui::LayerStackEntry mixed[4] = {
      {201, 0, 0}, {202, 0, 0}, {203, 77, 0}, {204, 77, 0}};
  /* 面板行 0 = 204(77)、1 = 203(77)、2 = 202(根)、3 = 201(根)。 */
  plan = comp_ui::plan_layer_drag(mixed, 4, 0, 2);
  COMP_CHECK(!plan.ok());
  COMP_CHECK(plan.target == comp_ui::DragTarget::kCrossParent);
  COMP_CHECK(plan.next_row == 0);
  plan = comp_ui::plan_layer_drag(mixed, 4, 0, 1); /* 同组内 204 -> 203 上面 */
  COMP_CHECK(plan.ok());
  plan = comp_ui::plan_layer_drag(mixed, 4, 2, 3); /* 根级相邻 */
  COMP_CHECK(plan.ok());

  /* 非法输入：空指针 / 空表 / 越界下标一律 kInvalid，不得越界读。 */
  COMP_CHECK(!comp_ui::plan_layer_drag(nullptr, 4, 0, 1).ok());
  COMP_CHECK(comp_ui::plan_layer_drag(nullptr, 4, 0, 1).target == comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, 0, 0, 0).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, -1, 0).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, 0, -1).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, 0, count).target ==
             comp_ui::DragTarget::kInvalid);
}

/*
 * 拖行几何（S24）：指示线 / 落点行必须与行矩形同源。
 *
 * 历史上这类「两处各算一套坐标」的 bug 已经出现过（S23 直方图块与行起点错位 30px），
 * 症状都不报错、只在特定面板状态下偏移。这里把三条不变式钉死：
 *   1. slot 行指示线 = 该行上边缘 - 1px；slot == count 时 = 末行下边缘；
 *   2. 直方图开关只整体平移同样的 rows_top；
 *   3. 鼠标 y 落在行中心必判回本行，越界夹到首末行。
 */
COMP_TEST(ui_core_layer_drag_geometry_tracks_row_top) {
  const Rect panel{0, 46, 248, 600};
  const int32_t count = 4;
  const int rows_top = comp_ui::layer_rows_top(true);

  for (int32_t slot = 0; slot <= count; ++slot) {
    const Rect line = comp_ui::layer_insert_line_rect(panel, rows_top, slot, count);
    const int expected_y =
        panel.y + comp_ui::kLayerHeaderHeight + rows_top + slot * comp_ui::kLayerRowHeight - 1;
    COMP_CHECK(line.y == expected_y);
    COMP_CHECK(line.h == 2);
    COMP_CHECK(line.x >= panel.x && line.right() <= panel.right());
    if (slot < count) {
      const Rect row = comp_ui::layer_row_rect(panel, slot, rows_top);
      COMP_CHECK_MSG(line.y + 1 == row.y, "指示线必须贴目标行上边缘");
    }
  }
  const Rect last_row = comp_ui::layer_row_rect(panel, count - 1, rows_top);
  const Rect bottom_line = comp_ui::layer_insert_line_rect(panel, rows_top, count, count);
  COMP_CHECK(bottom_line.y + 1 == last_row.bottom());

  /* 直方图开关只平移同一偏移量（不是各算一套）。 */
  const int rows_top_off = comp_ui::layer_rows_top(false);
  const Rect no_hist = comp_ui::layer_insert_line_rect(panel, rows_top_off, 2, count);
  const Rect with_hist = comp_ui::layer_insert_line_rect(panel, rows_top, 2, count);
  COMP_CHECK(with_hist.y - no_hist.y == rows_top - rows_top_off);
  COMP_CHECK(rows_top_off == 0);

  /* 插槽换算：向上拖画上边缘、向下拖画下边缘。 */
  COMP_CHECK(comp_ui::layer_drag_insert_slot(3, 1) == 1);
  COMP_CHECK(comp_ui::layer_drag_insert_slot(1, 3) == 4);

  /* 鼠标 y -> 行：行中心必判回本行；上下越界夹到首末行；无层 -> -1。 */
  for (int32_t row = 0; row < count; ++row) {
    const Rect rect = comp_ui::layer_row_rect(panel, row, rows_top);
    const double mid = rect.y + comp_ui::kLayerRowHeight * 0.5;
    COMP_CHECK(comp_ui::layer_drag_target_row(panel, count, mid, rows_top) == row);
  }
  COMP_CHECK(comp_ui::layer_drag_target_row(panel, count, panel.y - 500.0, rows_top) == 0);
  COMP_CHECK(comp_ui::layer_drag_target_row(panel, count, panel.bottom() + 500.0, rows_top) ==
             count - 1);
  COMP_CHECK(comp_ui::layer_drag_target_row(panel, 0, 100.0, rows_top) == -1);
}

/*
 * 直方图可视窗口（S24）：夹取 / 缩放 / 映射三件套。
 *
 * 夹取是防崩的那一层：跨度 0 会让 histogram_bin_x 除以 0，NaN 坐标交给 GDI 的结果
 * 是一整块空白且不报错。缩放要保证「光标下的那根柱不跑」，映射要与命中测试共用，
 * 否则放大后曲线和鼠标位置对不上。框选还要容忍反向拖与单点框。
 */
COMP_TEST(ui_core_histogram_view_clamp_zoom_and_mapping) {
  /* 1. 夹取：默认全幅、反向先排序、跨度不足补中心、越界整体平移。 */
  const comp_ui::HistogramView full = comp_ui::clamp_histogram_view(0.0, 255.0);
  COMP_CHECK(comp_ui::histogram_view_is_full(full));
  COMP_CHECK(near_value(comp_ui::histogram_view_span(full), 255.0));

  const comp_ui::HistogramView swapped = comp_ui::clamp_histogram_view(200.0, 50.0);
  COMP_CHECK(near_value(swapped.lo, 50.0) && near_value(swapped.hi, 200.0));

  const comp_ui::HistogramView tiny = comp_ui::clamp_histogram_view(100.0, 102.0);
  COMP_CHECK(near_value(comp_ui::histogram_view_span(tiny), comp_ui::kHistogramMinSpan));
  COMP_CHECK(near_value((tiny.lo + tiny.hi) * 0.5, 101.0)); /* 补跨度时中心不动 */

  const comp_ui::HistogramView low = comp_ui::clamp_histogram_view(-40.0, 40.0);
  COMP_CHECK(near_value(low.lo, 0.0));
  COMP_CHECK(near_value(comp_ui::histogram_view_span(low), 80.0)); /* 平移不缩跨度 */

  const comp_ui::HistogramView high = comp_ui::clamp_histogram_view(230.0, 300.0);
  COMP_CHECK(near_value(high.hi, comp_ui::kHistogramMaxBin));
  COMP_CHECK(near_value(comp_ui::histogram_view_span(high), 70.0));

  /* NaN 一律回落全幅（否则 NaN 会一路传到 GDI）。 */
  COMP_CHECK(comp_ui::histogram_view_is_full(comp_ui::clamp_histogram_view(std::nan(""), 100.0)));
  COMP_CHECK(comp_ui::histogram_view_is_full(comp_ui::clamp_histogram_view(100.0, std::nan(""))));

  /* 2. 缩放：focus 在窗口里的相对位置不变；极限处跨度锁死在最小跨度。 */
  const comp_ui::HistogramView zoomed = comp_ui::zoom_histogram_view(full, 128.0, 0.5);
  COMP_CHECK(near_value(comp_ui::histogram_view_span(zoomed), 127.5));
  COMP_CHECK(near_value(zoomed.lo, 64.0));
  COMP_CHECK(near_value(zoomed.hi, 191.5));
  const double t_before = (128.0 - full.lo) / comp_ui::histogram_view_span(full);
  const double t_after = (128.0 - zoomed.lo) / comp_ui::histogram_view_span(zoomed);
  COMP_CHECK_MSG(near_value(t_before, t_after), "缩放必须锚定 focus，光标下的柱不能跑");
  COMP_CHECK(!comp_ui::histogram_view_is_full(zoomed));

  comp_ui::HistogramView deepest = full;
  for (int i = 0; i < 64; ++i) {
    deepest = comp_ui::zoom_histogram_view(deepest, 128.0, 0.5);
  }
  COMP_CHECK(near_value(comp_ui::histogram_view_span(deepest), comp_ui::kHistogramMinSpan));

  comp_ui::HistogramView reset = deepest;
  for (int i = 0; i < 64; ++i) {
    reset = comp_ui::zoom_histogram_view(reset, 128.0, 2.0);
  }
  COMP_CHECK_MSG(comp_ui::histogram_view_is_full(reset), "连续缩小必须回到全幅且不越界");

  /* 3. 映射：两端严格对齐绘图区左右边，中间线性，bin->x->bin 往返稳定。 */
  const Rect plot{0, 100, 200, 64};
  const comp_ui::HistogramView view{64.0, 192.0};
  COMP_CHECK(near_value(comp_ui::histogram_bin_x(plot, view, view.lo), plot.x));
  COMP_CHECK(near_value(comp_ui::histogram_bin_x(plot, view, view.hi), plot.x + plot.w));
  const double xs[] = {0.0, 1.0, 37.5, 100.0, 199.0, 200.0};
  for (double x : xs) {
    const double bin = comp_ui::histogram_x_to_bin(plot, view, x);
    COMP_CHECK(near_value(comp_ui::histogram_bin_x(plot, view, bin), x));
  }
  COMP_CHECK(near_value(comp_ui::histogram_x_to_bin(plot, view, plot.x + plot.w * 0.5), 128.0));

  /* 4. 框选：反向拖取排序后两端；单点框不得产生 0 跨度；全幅拖出即全幅。 */
  const comp_ui::HistogramView selected =
      comp_ui::histogram_window_from_drag(plot, full, 150.0, 50.0);
  COMP_CHECK(near_value(selected.lo, 50.0 / 200.0 * 255.0));
  COMP_CHECK(near_value(selected.hi, 150.0 / 200.0 * 255.0));

  const comp_ui::HistogramView one_pixel =
      comp_ui::histogram_window_from_drag(plot, full, 90.0, 90.0);
  COMP_CHECK(comp_ui::histogram_view_span(one_pixel) >= comp_ui::kHistogramMinSpan);

  COMP_CHECK(comp_ui::histogram_view_is_full(
      comp_ui::histogram_window_from_drag(plot, full, -1000.0, 1000.0)));
}

/*
 * 调整参数面板（P0-3）：行 / 滑道 / 滚动 / 值映射。
 *
 * 这里钉的是「画出来的地方就是能点的地方」——滑杆的命中区、滑道的像素位置、
 * 拖动时的 x -> 值映射三者必须同源，否则会出现拖了半格没反应、或者松手后
 * 数值跳一下的老问题。滚动只保证：能滚到的行都完整落在可视区，滚不到的行
 * 命中返回 -1（而不是顺手改掉别人的参数）。
 */
COMP_TEST(ui_core_adjust_param_rows_and_slider_mapping) {
  const comp_ui::Rect panel{1000, 100, 330, 648};
  COMP_CHECK(comp_ui::kAdjustParamRowHeight == 34);
  COMP_CHECK(comp_ui::kAdjustParamRowsTop == 50);

  /* 1. 行几何：行首与 draw_adjust_panel 的标题 + 类型行对齐，行高含标签带与滑道。 */
  const comp_ui::Rect first = comp_ui::adjust_param_row_rect(panel, 0);
  COMP_CHECK(first.x == panel.x && first.w == panel.w && first.h == comp_ui::kAdjustParamRowHeight);
  COMP_CHECK(first.y == panel.y + comp_ui::kAdjustParamRowsTop);
  COMP_CHECK(comp_ui::adjust_param_row_rect(panel, 3).y ==
             first.y + 3 * comp_ui::kAdjustParamRowHeight);
  /* 滚动只平移，不改行尺寸。 */
  const comp_ui::Rect scrolled = comp_ui::adjust_param_row_rect(panel, 3, 2);
  COMP_CHECK(scrolled.y == comp_ui::adjust_param_row_rect(panel, 1).y);
  COMP_CHECK(scrolled.h == first.h && scrolled.w == first.w);

  /* 2. 可视行数 / 滚动上限：720 高的窗口（体区 648）放得下十余行，色阶 20 行要能滚。 */
  COMP_CHECK(comp_ui::adjust_param_visible_rows(panel) ==
             (panel.h - comp_ui::kAdjustParamRowsTop - comp_ui::kAdjustPanelFooter) /
                 comp_ui::kAdjustParamRowHeight);
  COMP_CHECK(comp_ui::adjust_param_max_scroll(panel, 5) == 0);
  COMP_CHECK(comp_ui::adjust_param_max_scroll(panel, 20) ==
             20 - comp_ui::adjust_param_visible_rows(panel));
  COMP_CHECK(comp_ui::adjust_param_clamp_scroll(panel, 20, -7) == 0);
  COMP_CHECK(comp_ui::adjust_param_clamp_scroll(panel, 20, 999) ==
             comp_ui::adjust_param_max_scroll(panel, 20));
  COMP_CHECK(comp_ui::adjust_param_clamp_scroll(panel, 5, 3) == 0);

  const int visible = comp_ui::adjust_param_visible_rows(panel);
  COMP_CHECK(visible > 0);

  /* Tab 切参数时「刚好滚够」：已经在窗口内就不动，越界只滚最小距离。 */
  COMP_CHECK(comp_ui::adjust_param_ensure_visible(panel, 20, 4, 2) == 2);
  COMP_CHECK(comp_ui::adjust_param_ensure_visible(panel, 20, visible - 1, 0) == 0);
  COMP_CHECK(comp_ui::adjust_param_ensure_visible(panel, 20, visible, 0) == 1);
  COMP_CHECK(comp_ui::adjust_param_ensure_visible(panel, 20, 19, 0) == 19 - visible + 1);
  COMP_CHECK(comp_ui::adjust_param_ensure_visible(panel, 20, 2, 5) == 2);
  COMP_CHECK(comp_ui::adjust_param_ensure_visible(panel, 5, 9, 0) == 0);

  /* 滚到底时最后一行仍然完整落在可视区内，不压到底部「已有调整」列表。 */
  const comp_ui::Rect last = comp_ui::adjust_param_row_rect(
      panel, 19, comp_ui::adjust_param_clamp_scroll(panel, 20, 999));
  COMP_CHECK(last.bottom() <= panel.bottom() - comp_ui::kAdjustPanelFooter);

  /* 3. 命中：行首 / 行中 / 行尾命中同一行；标题区、可视区外、行数外返回 -1。 */
  for (int row = 0; row < 3; ++row) {
    const comp_ui::Rect rect = comp_ui::adjust_param_row_rect(panel, row, 0);
    const double x = static_cast<double>(rect.x + 4);
    COMP_CHECK(comp_ui::adjust_param_row_at(panel, 5, 0, x, static_cast<double>(rect.y)) == row);
    COMP_CHECK(comp_ui::adjust_param_row_at(panel, 5, 0, x,
                                            static_cast<double>(rect.bottom()) - 1.0) == row);
  }
  COMP_CHECK(comp_ui::adjust_param_row_at(panel, 5, 0, static_cast<double>(first.x + 4),
                                          static_cast<double>(panel.y + 10)) == -1);
  COMP_CHECK(comp_ui::adjust_param_row_at(panel, 5, 0, static_cast<double>(panel.right() + 4),
                                          static_cast<double>(first.y)) == -1);
  COMP_CHECK(comp_ui::adjust_param_row_at(panel, 0, 0, static_cast<double>(first.x + 4),
                                          static_cast<double>(first.y)) == -1);
  /* 滚过 2 行后，屏幕第一行是绝对第 2 行；绝对第 0/1 行已不可点。 */
  COMP_CHECK(comp_ui::adjust_param_row_at(panel, 20, 2, static_cast<double>(first.x + 4),
                                          static_cast<double>(first.y)) == 2);
  /* 可视区以下（底部提示区）不是参数行。 */
  COMP_CHECK(comp_ui::adjust_param_row_at(panel, 20, 0, static_cast<double>(first.x + 4),
                                          static_cast<double>(panel.bottom()) - 4.0) == -1);

  /* 4. 滑道：只占行下半段，上沿留出聚焦/键入带；标签带点击不算拖滑杆。 */
  const comp_ui::Rect track = comp_ui::adjust_slider_track_rect(first);
  COMP_CHECK(track.x > first.x && track.right() < first.right());
  COMP_CHECK(track.y == first.y + comp_ui::kAdjustRowLabelBand);
  COMP_CHECK(track.bottom() <= first.bottom());
  COMP_CHECK(!comp_ui::adjust_row_track_hit(panel, 0, 0, static_cast<double>(track.x + 4),
                                            static_cast<double>(first.y + 4)));
  COMP_CHECK(comp_ui::adjust_row_track_hit(panel, 0, 0, static_cast<double>(track.x + 4),
                                           static_cast<double>(track.y + track.h / 2.0)));

  /* 5. x -> 值：两端严格对齐滑道左右，拖出滑道取端值，NaN 不产生脏值。 */
  COMP_CHECK(near_value(comp_ui::adjust_slider_value(track, track.x, 0.0, 255.0), 0.0));
  COMP_CHECK(near_value(comp_ui::adjust_slider_value(track, track.right(), 0.0, 255.0), 255.0));
  COMP_CHECK(near_value(comp_ui::adjust_slider_value(track, track.x - 500.0, 0.0, 255.0), 0.0));
  COMP_CHECK(near_value(comp_ui::adjust_slider_value(track, track.right() + 500.0, 0.0, 255.0),
                        255.0));
  COMP_CHECK(near_value(comp_ui::adjust_slider_fraction(track, std::nan("")), 0.0));
  COMP_CHECK(near_value(comp_ui::adjust_slider_value(track, std::nan(""), -100.0, 100.0), -100.0));

  /* 值 -> 像素 -> 值往返：滑块画在哪，拖到那就是哪个值。 */
  const double samples[] = {0.0, 40.0, 127.5, 200.0, 255.0};
  for (double value : samples) {
    const double x = comp_ui::adjust_slider_knob_x(track, value, 0.0, 255.0);
    COMP_CHECK(near_value(comp_ui::adjust_slider_value(track, x, 0.0, 255.0), value));
  }
  COMP_CHECK(near_value(comp_ui::adjust_slider_knob_x(track, -1e9, 0.0, 255.0), track.x));
  COMP_CHECK(near_value(comp_ui::adjust_slider_knob_x(track, 1e9, 0.0, 255.0), track.right()));

  /* 6. 归整：宽量程（色阶 0..255）取整，窄量程（gamma 0.1..9.99）留两位小数。 */
  COMP_CHECK(comp_ui::adjust_snap_value(127.6, 0.0, 255.0) == 128.0);
  COMP_CHECK(comp_ui::adjust_snap_value(39.4, 0.0, 255.0) == 39.0);
  COMP_CHECK(comp_ui::adjust_snap_value(-3.6, -100.0, 100.0) == -4.0);
  COMP_CHECK(comp_ui::adjust_snap_value(1.234, 0.1, 9.99) == 1.23);
  COMP_CHECK(comp_ui::adjust_snap_value(0.004, -20.0, 20.0) == 0.0);
  COMP_CHECK(comp_ui::adjust_snap_value(1e9, 0.0, 255.0) == 255.0);
  COMP_CHECK(comp_ui::adjust_snap_value(std::nan(""), 0.0, 255.0) == 0.0);
}

/*
 * 色阶通道条 + 曲线编辑器（P0-3）。
 *
 * 这里钉三件事：
 *   1. 通道条右对齐在类型行上，不压参数行；四个格子按 RGB/R/G/B 从左到右排，
 *      互不重叠且完整落在面板里（点得到的前提是画得下）。
 *   2. 曲线绘图区是正方形，屏幕 <-> 值双向映射一致（y 翻转）；控制点命中取最近
 *      且在半径内的点，空白处不加点的情况返回 -1。
 *   3. 折线求值与引擎同口径：恒等曲线不动、单调点上不过冲、端点夹住。
 */
COMP_TEST(ui_core_adjust_channel_chips_and_curve_editor) {
  const comp_ui::Rect panel{1000, 100, 330, 648};

  /* 1. 通道条：4 个格子，右对齐，不进入参数行区域。 */
  const int channels = 4;
  comp_ui::Rect previous{};
  for (int c = 0; c < channels; ++c) {
    const comp_ui::Rect chip = comp_ui::adjust_channel_chip_rect(panel, c, channels);
    COMP_CHECK(chip.w == comp_ui::kAdjustChannelChipWidth);
    COMP_CHECK(chip.h == comp_ui::kAdjustChannelChipHeight);
    COMP_CHECK(chip.x > panel.x && chip.right() <= panel.right() - 10);
    COMP_CHECK(chip.bottom() <= panel.y + comp_ui::kAdjustParamRowsTop);
    if (c > 0) COMP_CHECK(chip.x > previous.right());
    previous = chip;
    COMP_CHECK(comp_ui::adjust_channel_chip_at(panel, channels,
                                               chip.x + chip.w / 2.0,
                                               chip.y + chip.h / 2.0) == c);
  }
  COMP_CHECK(comp_ui::adjust_channel_chip_at(panel, channels, panel.x + 4.0,
                                             panel.y + 30.0) == -1);
  COMP_CHECK(comp_ui::adjust_channel_chip_at(panel, channels, panel.right() - 4.0,
                                             panel.y + 30.0) == -1);
  /* 通道格子不吞参数行：第一行起始 y 在通道条之下。 */
  const comp_ui::Rect first = comp_ui::adjust_param_row_rect(panel, 0);
  COMP_CHECK(comp_ui::adjust_channel_chip_rect(panel, 0, channels).bottom() <= first.y);

  /* 2. 曲线通道条 + 绘图区。 */
  const comp_ui::Rect plot = comp_ui::adjust_curve_plot_rect(panel);
  COMP_CHECK(!plot.empty());
  COMP_CHECK(plot.w == plot.h);
  COMP_CHECK(plot.x >= panel.x && plot.right() <= panel.right());
  COMP_CHECK(plot.bottom() <= panel.bottom() - comp_ui::kAdjustPanelFooter);
  COMP_CHECK(plot.y >= panel.y + comp_ui::kAdjustParamRowsTop + comp_ui::kCurveChannelBarHeight);
  for (int c = 0; c < channels; ++c) {
    const comp_ui::Rect chip = comp_ui::curve_channel_chip_rect(panel, c, channels);
    COMP_CHECK(chip.bottom() <= plot.y);
    COMP_CHECK(comp_ui::curve_channel_chip_at(panel, channels, chip.x + chip.w / 2.0,
                                              chip.y + chip.h / 2.0) == c);
  }
  /* 小面板放不下正方形时返回空矩形，绘制端据此跳过（不画半个编辑器）。 */
  COMP_CHECK(comp_ui::adjust_curve_plot_rect(comp_ui::Rect{0, 0, 200, 120}).empty());

  /* 3. 值 <-> 屏幕：0 在左下、255 在右上，往返一致。 */
  COMP_CHECK(near_value(comp_ui::curve_plot_value_to_x(plot, 0.0),
                        static_cast<double>(plot.x)));
  COMP_CHECK(near_value(comp_ui::curve_plot_value_to_x(plot, 255.0),
                        static_cast<double>(plot.right() - 1)));
  COMP_CHECK(near_value(comp_ui::curve_plot_value_to_y(plot, 0.0),
                        static_cast<double>(plot.bottom() - 1)));
  COMP_CHECK(near_value(comp_ui::curve_plot_value_to_y(plot, 255.0),
                        static_cast<double>(plot.y)));
  const double sample_values[] = {0.0, 40.0, 128.0, 200.0, 255.0};
  for (double value : sample_values) {
    COMP_CHECK(near_value(
        comp_ui::curve_plot_x_to_value(plot, comp_ui::curve_plot_value_to_x(plot, value)), value));
    COMP_CHECK(near_value(
        comp_ui::curve_plot_y_to_value(plot, comp_ui::curve_plot_value_to_y(plot, value)), value));
  }
  COMP_CHECK(near_value(comp_ui::curve_plot_x_to_value(plot, plot.x - 500.0), 0.0));
  COMP_CHECK(near_value(comp_ui::curve_plot_x_to_value(plot, plot.right() + 500.0), 255.0));

  /* 4. 控制点命中：取半径内最近的点；点外返回 -1。 */
  const double xy[] = {0.0, 0.0, 128.0, 160.0, 255.0, 255.0};
  const double mid_x = comp_ui::curve_plot_value_to_x(plot, 128.0);
  const double mid_y = comp_ui::curve_plot_value_to_y(plot, 160.0);
  COMP_CHECK(comp_ui::curve_point_hit(plot, xy, 3, mid_x + 2.0, mid_y - 2.0, 6.0) == 1);
  COMP_CHECK(comp_ui::curve_point_hit(plot, xy, 3, mid_x + 40.0, mid_y, 6.0) == -1);
  COMP_CHECK(comp_ui::curve_point_hit(plot, xy, 0, mid_x, mid_y, 6.0) == -1);
  /* 两个点重叠半径内取更近的那个。 */
  COMP_CHECK(comp_ui::curve_point_hit(plot, xy, 3, mid_x, mid_y + 3.0, 6.0) == 1);

  /* 5. 加点：只能落在相邻两点之间且两侧留 1 个单位。 */
  const double wide[] = {0.0, 0.0, 128.0, 128.0, 255.0, 255.0};
  COMP_CHECK(near_value(comp_ui::curve_insert_x(wide, 3, 64.2), 64.0));
  COMP_CHECK(near_value(comp_ui::curve_insert_x(wide, 3, 126.6), 127.0));
  COMP_CHECK(std::isnan(comp_ui::curve_insert_x(wide, 3, 128.0)));
  COMP_CHECK(std::isnan(comp_ui::curve_insert_x(wide, 3, 255.0)));
  COMP_CHECK(std::isnan(comp_ui::curve_insert_x(wide, 3, -4.0)));
  const double tight[] = {0.0, 0.0, 1.0, 1.0, 255.0, 255.0};
  COMP_CHECK(std::isnan(comp_ui::curve_insert_x(tight, 3, 0.4)));
  COMP_CHECK(near_value(comp_ui::curve_insert_x(tight, 3, 100.0), 100.0));

  /* 6. 折线求值：恒等、端点夹住、单调点上不过冲。 */
  const double identity[] = {0.0, 0.0, 255.0, 255.0};
  for (double value : sample_values) {
    COMP_CHECK(near_value(comp_ui::curve_eval(identity, 2, value), value));
  }
  const double lift[] = {0.0, 0.0, 128.0, 180.0, 255.0, 255.0};
  COMP_CHECK(near_value(comp_ui::curve_eval(lift, 3, 128.0), 180.0));
  COMP_CHECK(near_value(comp_ui::curve_eval(lift, 3, 0.0), 0.0));
  COMP_CHECK(near_value(comp_ui::curve_eval(lift, 3, 255.0), 255.0));
  /* 保形：中间调上提后整段单调不减；中段不冲过 180，末段不冲过 255。 */
  double previous_y = -1.0;
  for (int x = 0; x <= 255; ++x) {
    const double y = comp_ui::curve_eval(lift, 3, static_cast<double>(x));
    COMP_CHECK(y >= previous_y - 1e-9);
    COMP_CHECK(y >= -1e-9 && y <= 255.0 + 1e-9);
    if (x <= 128) COMP_CHECK(y <= 180.0 + 1e-9);
    previous_y = y;
  }
  /* 少于两点 = 恒等（引擎按容错读，面板也照此画）。 */
  COMP_CHECK(near_value(comp_ui::curve_eval(identity, 1, 77.0), 77.0));
}
