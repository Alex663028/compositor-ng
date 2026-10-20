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
