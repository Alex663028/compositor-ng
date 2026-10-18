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
#include <string>

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
  COMP_CHECK(count == 17);
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
                  {'c', ToolId::Crop}};
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
  COMP_CHECK(comp_ui::tool_index(ToolId::Clone) == 10);
  COMP_CHECK(std::string(comp_ui::tool_info(ToolId::Clone)->name) == "Clone Stamp");
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
