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

COMP_TEST(ui_core_zoom_to_rect_fills_the_view_and_centers_the_rect) {
  const Rect canvas{0, 0, 800, 600};
  const int doc_w = 1600;
  const int doc_h = 1200;
  CanvasView view;

  /* 200x100 的框：min(800*0.98/200, 600*0.98/100) = 3.92。 */
  COMP_CHECK(view.zoom_to_doc_rect(200.0, 300.0, 400.0, 400.0, canvas, doc_w, doc_h, 0.98));
  COMP_CHECK(near_value(view.zoom, 3.92));
  /* 框中心 (300, 350) -> 画布中心；pan = -(中心 - 文档中心) * zoom。 */
  COMP_CHECK(near_value(view.pan_x, -(300.0 - 800.0) * 3.92));
  COMP_CHECK(near_value(view.pan_y, -(350.0 - 600.0) * 3.92));
  const Point center = view.doc_to_screen(300.0, 350.0, canvas, doc_w, doc_h);
  COMP_CHECK_MSG(same_point(center, CanvasView::canvas_center(canvas), 1e-6),
                 "rect center lands on canvas center");

  /* 反序拖框（右下 -> 左上）结果一致。 */
  CanvasView reversed;
  COMP_CHECK(reversed.zoom_to_doc_rect(400.0, 400.0, 200.0, 300.0, canvas, doc_w, doc_h, 0.98));
  COMP_CHECK(near_value(reversed.zoom, view.zoom));
  COMP_CHECK(near_value(reversed.pan_x, view.pan_x));
  COMP_CHECK(near_value(reversed.pan_y, view.pan_y));

  /* 缩小框同样可用；超大框夹到 2% 下限，不会出现 0 / 负数。 */
  CanvasView tiny;
  COMP_CHECK(tiny.zoom_to_doc_rect(0.0, 0.0, 4.0, 4.0, canvas, doc_w, doc_h, 1.0));
  COMP_CHECK(near_value(tiny.zoom, CanvasView::kMaxZoom));
  CanvasView huge;
  COMP_CHECK(huge.zoom_to_doc_rect(-1e6, -1e6, 1e6, 1e6, canvas, doc_w, doc_h, 1.0));
  COMP_CHECK(near_value(huge.zoom, CanvasView::kMinZoom));

  /* 退化输入：单点点击 / 零宽 / 空画布一律返回 false 且不改动视图。 */
  CanvasView untouched;
  untouched.zoom = 2.5;
  untouched.pan_x = 11.0;
  untouched.pan_y = -7.0;
  COMP_CHECK(!untouched.zoom_to_doc_rect(300.0, 300.0, 300.0, 300.0, canvas, doc_w, doc_h));
  COMP_CHECK(!untouched.zoom_to_doc_rect(100.0, 200.0, 300.0, 200.0, canvas, doc_w, doc_h));
  COMP_CHECK(!untouched.zoom_to_doc_rect(0.0, 0.0, 10.0, 10.0, Rect{}, doc_w, doc_h));
  COMP_CHECK(near_value(untouched.zoom, 2.5));
  COMP_CHECK(near_value(untouched.pan_x, 11.0));
  COMP_CHECK(near_value(untouched.pan_y, -7.0));
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
      COMP_CHECK_MSG(!rects_overlap(layout.canvas, layout.options),
                     "canvas/options overlap");
      COMP_CHECK_MSG(!rects_overlap(layout.options, layout.status),
                     "options/status overlap");
      /* 选项栏要么整条存在（贴工具栏下沿、满宽），要么整条收起（h == 0）。 */
      COMP_CHECK_MSG(layout.options.h == 0 ||
                         (layout.options.h == Layout::kOptionsHeight &&
                          layout.options.y == layout.toolbar.bottom() &&
                          layout.options.w == width),
                     "options bar must be either absent or full width under the toolbar");

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
  COMP_CHECK(wide.options.h == Layout::kOptionsHeight);
  COMP_CHECK(wide.options.y == Layout::kToolbarHeight);
  COMP_CHECK(wide.canvas.y == Layout::kToolbarHeight + Layout::kOptionsHeight);
  COMP_CHECK(wide.canvas.bottom() == 720 - Layout::kStatusHeight);

  /* §5.49：矮窗口先收起选项栏，画布回收这 32px；阈值两侧各验一次。 */
  const int shown_at =
      Layout::kToolbarHeight + Layout::kOptionsHeight + Layout::kMinBodyHeight +
      Layout::kStatusHeight;
  COMP_CHECK(comp_ui::compute_layout(1280, shown_at).options.h == Layout::kOptionsHeight);
  const Layout short_window = comp_ui::compute_layout(1280, shown_at - 1);
  COMP_CHECK(short_window.options.h == 0);
  COMP_CHECK(short_window.canvas.y == Layout::kToolbarHeight);
  COMP_CHECK(short_window.canvas.bottom() == shown_at - 1 - Layout::kStatusHeight);
}

COMP_TEST(ui_core_layout_respects_panel_width_prefs) {
  /* §6.60：偏好宽度生效时仍必须三块横向铺满，且收起口径按偏好宽度判断。 */
  const int prefs_layer[] = {Layout::kLayerPanelMinWidth, 200, Layout::kLayerPanelWidth, 400,
                             Layout::kLayerPanelMaxWidth};
  const int prefs_adjust[] = {Layout::kAdjustPanelMinWidth, 300, Layout::kAdjustPanelWidth, 470,
                              Layout::kAdjustPanelMaxWidth};
  const int widths[] = {300, 427, 660, 700, 800, 1000, 1280, 1920};

  for (int layer_pref : prefs_layer) {
    for (int adjust_pref : prefs_adjust) {
      comp_ui::LayoutPrefs prefs;
      prefs.layer_panel_width = layer_pref;
      prefs.adjust_panel_width = adjust_pref;
      for (int width : widths) {
        const Layout layout = comp_ui::compute_layout(width, 720, prefs);

        COMP_CHECK_MSG(layout.layer_panel.w + layout.canvas.w + layout.adjust_panel.w == width,
                       "prefs layout must still tile the full width");
        COMP_CHECK_MSG(layout.canvas.w >= 0, "canvas must never go negative with prefs");

        /* 宽度只能是 0（放不下）或偏好值本身——不存在「挤成一半」的中间态。 */
        COMP_CHECK_MSG(layout.layer_panel.w == 0 || layout.layer_panel.w == layer_pref,
                       "layer panel width must be the pref or collapsed");
        COMP_CHECK_MSG(layout.adjust_panel.w == 0 || layout.adjust_panel.w == adjust_pref,
                       "adjust panel width must be the pref or collapsed");

        if (width >= layer_pref + Layout::kMinCanvasWidth) {
          COMP_CHECK_MSG(layout.layer_panel.w == layer_pref,
                         "layer panel shown when pref + min canvas fits");
        } else {
          COMP_CHECK_MSG(layout.layer_panel.w == 0, "layer panel collapsed when it cannot fit");
        }
      }
    }
  }

  /* 面板变宽必然压缩画布，且画布左边界跟着面板右边界走。 */
  comp_ui::LayoutPrefs wide_prefs;
  wide_prefs.layer_panel_width = 400;
  const Layout wide = comp_ui::compute_layout(1280, 720, wide_prefs);
  const Layout standard = comp_ui::compute_layout(1280, 720);
  COMP_CHECK(wide.layer_panel.w == 400);
  COMP_CHECK(wide.canvas.x == 400);
  COMP_CHECK(wide.canvas.w == standard.canvas.w - (400 - Layout::kLayerPanelWidth));
}

COMP_TEST(ui_core_tool_options_bar) {
  /* 行集合是纯函数：绘制与命中共用 tool_option_count / tool_option，不能两处各写一张表。 */
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Brush) == 5);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Eraser) == 5);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Retouch) == 2);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Clone) == 4);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Heal) == 3);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Wand) == 2);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Marquee) == 1);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Gradient) == 4);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Shape) == 2);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Hand) == 0);
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Move) == 0);
  /* 文字的字号/行距/字体在右栏；段落框与对齐在选项栏（P0-6③，右栏四行已冻结）。 */
  COMP_CHECK(comp_ui::tool_option_count(comp_ui::ToolId::Type) == 3);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Type, 0).id ==
             comp_ui::ToolOptionId::TypeBoxWidth);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Type, 1).id ==
             comp_ui::ToolOptionId::TypeBoxHeight);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Type, 2).id ==
             comp_ui::ToolOptionId::TypeAlign);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Type, 2).kind ==
             comp_ui::ToolOptionKind::Choice);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Hand, 0).id == comp_ui::ToolOptionId::None);

  const comp_ui::ToolOptionSpec size = comp_ui::tool_option(comp_ui::ToolId::Brush, 0);
  COMP_CHECK(size.id == comp_ui::ToolOptionId::BrushSize);
  COMP_CHECK(size.kind == comp_ui::ToolOptionKind::Stepper);
  COMP_CHECK(size.min == static_cast<int>(comp_ui::kMinBrushSize));
  COMP_CHECK(size.max == static_cast<int>(comp_ui::kMaxBrushSize));

  /*
   * P1-4：画笔在「大小 / 硬度」**之后**追加预设/流量/压感。0/1 的下标是对外
   * 契约——visual_check 的 options_bar 场景与上面的 size 断言都按这个顺序取，
   * 以后新增组也只能继续往尾部加。
   */
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 1).id ==
             comp_ui::ToolOptionId::BrushHardness);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 2).id ==
             comp_ui::ToolOptionId::BrushPreset);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 3).id ==
             comp_ui::ToolOptionId::BrushFlow);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 4).id ==
             comp_ui::ToolOptionId::BrushPressure);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 2).kind ==
             comp_ui::ToolOptionKind::Choice);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 3).kind ==
             comp_ui::ToolOptionKind::Stepper);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Brush, 5).id == comp_ui::ToolOptionId::None);
  COMP_CHECK(comp_ui::tool_option(comp_ui::ToolId::Retouch, 2).id == comp_ui::ToolOptionId::None);

  const comp_ui::ToolId with_options[] = {
      comp_ui::ToolId::Brush,   comp_ui::ToolId::Eraser, comp_ui::ToolId::Heal,
      comp_ui::ToolId::Clone,   comp_ui::ToolId::Retouch, comp_ui::ToolId::Tone,
      comp_ui::ToolId::Marquee, comp_ui::ToolId::Lasso,  comp_ui::ToolId::Wand,
      comp_ui::ToolId::Gradient, comp_ui::ToolId::Shape, comp_ui::ToolId::HistoryBrush,
      comp_ui::ToolId::Type};
  for (comp_ui::ToolId tool : with_options) {
    const int count = comp_ui::tool_option_count(tool);
    COMP_CHECK_MSG(count > 0, "tool with options must expose at least one row");
    for (int i = 0; i < count; ++i) {
      const comp_ui::ToolOptionSpec spec = comp_ui::tool_option(tool, i);
      COMP_CHECK_MSG(spec.id != comp_ui::ToolOptionId::None, "row must name an option id");
      COMP_CHECK_MSG(spec.label != nullptr && spec.label[0] != '\0', "row must have a label");
      COMP_CHECK_MSG(spec.min <= spec.max, "row range must be ordered");
      if (spec.kind == comp_ui::ToolOptionKind::Choice) {
        COMP_CHECK_MSG(spec.choice_count == spec.max - spec.min + 1,
                       "choice row must span its enum range");
        COMP_CHECK_MSG(spec.choice_count >= 2, "choice row needs at least two choices");
      } else {
        COMP_CHECK_MSG(spec.step >= 1, "stepper row must advance");
      }
      /* 同一工具下不重复同一参数（重复行 = 两行互相打架）。 */
      for (int j = 0; j < i; ++j) {
        COMP_CHECK_MSG(comp_ui::tool_option(tool, j).id != spec.id, "duplicate option row");
      }
    }
    COMP_CHECK(comp_ui::tool_option(tool, count).id == comp_ui::ToolOptionId::None);
  }

  /* 几何：组从左往右排、放不下整组不画；控件盒在标签右侧。 */
  const comp_ui::Rect bar{0, 46, 1280, comp_ui::Layout::kOptionsHeight};
  const comp_ui::Rect group0 = comp_ui::option_group_rect(bar, 0);
  const comp_ui::Rect group1 = comp_ui::option_group_rect(bar, 1);
  COMP_CHECK(group0.x == comp_ui::kOptionLeftPad);
  COMP_CHECK(group0.w == comp_ui::kOptionGroupWidth);
  COMP_CHECK(group1.x == group0.right() + comp_ui::kOptionGroupGap);
  COMP_CHECK(comp_ui::option_group_rect(bar, -1).empty());
  COMP_CHECK(comp_ui::option_group_rect(comp_ui::Rect{}, 0).empty());
  const comp_ui::Rect narrow{0, 46, 300, comp_ui::Layout::kOptionsHeight};
  COMP_CHECK(!comp_ui::option_group_rect(narrow, 0).empty());
  COMP_CHECK(comp_ui::option_group_rect(narrow, 1).empty()); /* 170..320 越界 */

  const comp_ui::Rect control = comp_ui::option_control_rect(group0);
  COMP_CHECK(control.x == group0.x + comp_ui::kOptionLabelWidth);
  COMP_CHECK(control.w == comp_ui::kOptionArrowWidth * 2 + comp_ui::kOptionValueWidth);
  COMP_CHECK(control.h <= group0.h);
  COMP_CHECK(control.y >= group0.y && control.bottom() <= group0.bottom());
  COMP_CHECK(comp_ui::option_control_rect(comp_ui::Rect{}).empty());

  /*
   * 命中：Stepper 两侧箭头 = -1 / +1，**数值区 = 0**（P0-10 数字直输的入口）；
   * Choice 整块响应。
   *
   * 这一条的旧口径是「数值区点了不动参数」（返回 -1）——P0-10 把它改成输入框，
   * 所以断言跟着改；两侧箭头与 Choice 的行为一字未动。
   */
  int delta = 99;
  const double mid_y = static_cast<double>(control.y + control.h / 2);
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Brush, control.x + 4.0, mid_y, &delta) == 0);
  COMP_CHECK(delta == -1);
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Brush, control.right() - 4.0, mid_y,
                                 &delta) == 0);
  COMP_CHECK(delta == 1);
  delta = 99;
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Brush, control.x + control.w * 0.5, mid_y,
                                 &delta) == 0);
  COMP_CHECK(delta == 0); /* 数值区：进键入态 */
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Brush, group0.right() + 4.0, mid_y, &delta) ==
             -1);
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Hand, control.x + 4.0, mid_y, &delta) == -1);
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Brush, control.x + 4.0, 20.0, &delta) == -1);
  delta = 0;
  COMP_CHECK(comp_ui::option_hit(bar, comp_ui::ToolId::Shape, control.x + 4.0, mid_y, &delta) == 0);
  COMP_CHECK(delta == 1); /* 形状是 Choice：整块点都是下一档 */
  COMP_CHECK(comp_ui::option_hit(comp_ui::Rect{}, comp_ui::ToolId::Brush, 100.0, 60.0, &delta) ==
             -1);

  /* Choice 绕环：向后到底回 0，向前到 0 回尾；非法档位回落到 0。 */
  COMP_CHECK(comp_ui::option_cycle_choice(2, 3, 1) == 0);
  COMP_CHECK(comp_ui::option_cycle_choice(0, 3, -1) == 2);
  COMP_CHECK(comp_ui::option_cycle_choice(1, 3, 1) == 2);
  COMP_CHECK(comp_ui::option_cycle_choice(1, 3, 0) == 1);
  COMP_CHECK(comp_ui::option_cycle_choice(9, 3, 1) == 1);
  COMP_CHECK(comp_ui::option_cycle_choice(1, 0, 1) == 0);
}

COMP_TEST(ui_core_panel_width_clamp_and_drag) {
  /* 夹取：低于下限抬到下限，高于上限压到上限，范围内原样。 */
  COMP_CHECK(comp_ui::clamp_layer_panel_width(0) == Layout::kLayerPanelMinWidth);
  COMP_CHECK(comp_ui::clamp_layer_panel_width(-999) == Layout::kLayerPanelMinWidth);
  COMP_CHECK(comp_ui::clamp_layer_panel_width(200) == 200);
  COMP_CHECK(comp_ui::clamp_layer_panel_width(9999) == Layout::kLayerPanelMaxWidth);
  COMP_CHECK(comp_ui::clamp_adjust_panel_width(0) == Layout::kAdjustPanelMinWidth);
  COMP_CHECK(comp_ui::clamp_adjust_panel_width(300) == 300);
  COMP_CHECK(comp_ui::clamp_adjust_panel_width(9999) == Layout::kAdjustPanelMaxWidth);

  /* 图层面板在左：向右拖变宽。 */
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(248, 0.0) == 248);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(248, 40.0) == 288);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(248, -40.0) == 208);
  /* 右侧面板在右：向左拖变宽（同一屏位移，符号相反）。 */
  COMP_CHECK(comp_ui::adjust_panel_width_from_drag(330, 0.0) == 330);
  COMP_CHECK(comp_ui::adjust_panel_width_from_drag(330, -40.0) == 370);
  COMP_CHECK(comp_ui::adjust_panel_width_from_drag(330, 40.0) == 290);

  /*
   * 拖到边界后立刻反向：位移从**基准**算，不存在死区——夹取只作用在结果上。
   * 若先夹基准再加位移，回拖时得先抵消掉溢出量面板才开始动。
   */
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(500, 200.0) == Layout::kLayerPanelMaxWidth);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(500, 10.0) == 510);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(180, -100.0) == Layout::kLayerPanelMinWidth);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(180, 4.0) == 184);

  /* 位移取整：.5 向上取整，保证来回拖回原点能精确回到原宽度。 */
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(248, 2.5) == 251);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(248, 2.4) == 250);
  COMP_CHECK(comp_ui::layer_panel_width_from_drag(248, 40.0) ==
             comp_ui::layer_panel_width_from_drag(248, 40.4));
}

COMP_TEST(ui_core_panel_splitter_hit_test) {
  const Layout layout = comp_ui::compute_layout(1280, 720);
  const double mid_y = layout.canvas.y + layout.canvas.h / 2.0;

  /* 图层面板：命中带是面板最后 5px，整条落在面板内，不占画布。 */
  COMP_CHECK(layout.layer_splitter.w == Layout::kPanelSplitterWidth);
  COMP_CHECK(layout.layer_splitter.right() == layout.layer_panel.right());
  COMP_CHECK(layout.layer_splitter.x >= layout.layer_panel.x);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.layer_panel.w - 1.0, mid_y) == 1);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.layer_panel.w - 5.0, mid_y) == 1);
  /* 画布第一列像素不属于分界线：画到边上不应变成拉宽。 */
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.layer_panel.w, mid_y) == 0);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.layer_panel.w - 6.0, mid_y) == 0);

  /* 调整面板：命中带是面板最左 5px。 */
  COMP_CHECK(layout.adjust_splitter.w == Layout::kPanelSplitterWidth);
  COMP_CHECK(layout.adjust_splitter.x == layout.adjust_panel.x);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.adjust_panel.x + 0.0, mid_y) == 2);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.adjust_panel.x + 4.0, mid_y) == 2);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.adjust_panel.x - 1.0, mid_y) == 0);
  /* 画布右边界内侧一像素同样不是分界线。 */
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.adjust_panel.x - 1.0, mid_y) == 0);

  /* 纵向：只在内容行生效，工具条与状态栏里不命中。 */
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.layer_panel.w - 1.0,
                                        static_cast<double>(layout.toolbar.bottom()) - 1.0) == 0);
  COMP_CHECK(comp_ui::panel_splitter_at(layout, layout.layer_panel.w - 1.0,
                                        static_cast<double>(layout.status.y) + 1.0) == 0);

  /* 面板收起时命中带为空矩形，任何点都不得命中——否则窄窗口里拖不到画布边。 */
  const Layout narrow = comp_ui::compute_layout(300, 720);
  COMP_CHECK(narrow.layer_panel.w == 0);
  COMP_CHECK(narrow.adjust_panel.w == 0);
  COMP_CHECK(narrow.layer_splitter.empty());
  COMP_CHECK(narrow.adjust_splitter.empty());
  COMP_CHECK(comp_ui::panel_splitter_at(narrow, 0.0, mid_y) == 0);
  COMP_CHECK(comp_ui::panel_splitter_at(narrow, 299.0, mid_y) == 0);
  COMP_CHECK(comp_ui::panel_splitter_at(narrow, narrow.canvas.x, mid_y) == 0);
}

/*
 * §5.43 面板整块收起（§5.11「面板缩放 / 停靠」的另一半）：
 *   1. 收起态走同一套「三块铺满」算式，画布恰好让出 (偏好宽度 - 24) 的差额；
 *   2. 收起条上没有分界线——否则「点条展开」会被最右 5px 的「拖宽」吃掉；
 *   3. 展开态按钮落在标题行内，既不吃图层行，也不与分界线/色阶通道格重叠；
 *   4. 窄窗回落口径不变：按「想要的宽度（收起 = 24px）+ 画布下限」判断。
 */
COMP_TEST(ui_core_collapsed_panels_layout) {
  const double mid_y = comp_ui::compute_layout(1280, 720).canvas.y + 100.0;

  /* 1 + 2：左侧整块收起。 */
  comp_ui::LayoutPrefs folded_prefs;
  folded_prefs.layer_panel_collapsed = true;
  const Layout folded = comp_ui::compute_layout(1280, 720, folded_prefs);
  COMP_CHECK(folded.layer_panel.w == Layout::kCollapsedPanelWidth);
  COMP_CHECK(folded.layer_collapsed);
  COMP_CHECK(!folded.adjust_collapsed);
  COMP_CHECK(folded.layer_panel.w + folded.canvas.w + folded.adjust_panel.w == 1280);
  COMP_CHECK(folded.canvas.w ==
             comp_ui::compute_layout(1280, 720).canvas.w +
                 (Layout::kLayerPanelWidth - Layout::kCollapsedPanelWidth));
  COMP_CHECK(folded.layer_splitter.empty());
  COMP_CHECK(comp_ui::panel_splitter_at(folded, folded.layer_panel.right() - 1.0, mid_y) == 0);
  /* 整条竖条都是展开按钮：不要求用户去瞄 18px 的小方块。 */
  COMP_CHECK(folded.layer_toggle.x == folded.layer_panel.x);
  COMP_CHECK(folded.layer_toggle.y == folded.layer_panel.y);
  COMP_CHECK(folded.layer_toggle.w == folded.layer_panel.w);
  COMP_CHECK(folded.layer_toggle.h == folded.layer_panel.h);
  COMP_CHECK(comp_ui::panel_toggle_at(folded, folded.layer_panel.x + 1.0,
                                      folded.layer_panel.y + 1.0) == 1);
  COMP_CHECK(comp_ui::panel_toggle_at(folded, folded.layer_panel.right() - 2.0,
                                      folded.layer_panel.bottom() - 2.0) == 1);

  /* 右侧同样：收起后不吃画布像素，分界线消失。 */
  comp_ui::LayoutPrefs folded_right;
  folded_right.adjust_panel_collapsed = true;
  const Layout right = comp_ui::compute_layout(1280, 720, folded_right);
  COMP_CHECK(right.adjust_panel.w == Layout::kCollapsedPanelWidth);
  COMP_CHECK(right.adjust_collapsed);
  COMP_CHECK(right.adjust_splitter.empty());
  COMP_CHECK(right.adjust_toggle.x == right.adjust_panel.x);
  COMP_CHECK(right.adjust_toggle.w == right.adjust_panel.w);
  COMP_CHECK(comp_ui::panel_toggle_at(right, right.adjust_panel.x + 2.0,
                                      right.adjust_panel.y + 40.0) == 2);

  /* 3：展开态按钮几何。 */
  const Layout open_layout = comp_ui::compute_layout(1280, 720);
  COMP_CHECK(open_layout.layer_toggle.w == Layout::kPanelToggleSize);
  COMP_CHECK(open_layout.layer_toggle.h == Layout::kPanelToggleSize);
  COMP_CHECK(open_layout.layer_toggle.x >= open_layout.layer_panel.x);
  /* 与分界线留出间隔：否则「收起」与「拖宽」命中区重叠，先命中的那条通吃。 */
  COMP_CHECK(!rects_overlap(open_layout.layer_toggle, open_layout.layer_splitter));
  COMP_CHECK(!rects_overlap(open_layout.adjust_toggle, open_layout.adjust_splitter));
  /* 不侵入图层行：标题行高 30，按钮底边必须还在标题行内。 */
  COMP_CHECK(open_layout.layer_toggle.bottom() <=
             open_layout.layer_panel.y + comp_ui::kLayerHeaderHeight);
  COMP_CHECK(comp_ui::panel_toggle_at(open_layout, open_layout.layer_toggle.x + 1.0,
                                      open_layout.layer_toggle.y + 1.0) == 1);
  COMP_CHECK(comp_ui::panel_toggle_at(open_layout, open_layout.layer_panel.x + 3.0, mid_y) == 0);
  COMP_CHECK(comp_ui::panel_toggle_at(open_layout, open_layout.adjust_toggle.x + 1.0,
                                      open_layout.adjust_toggle.y + 1.0) == 2);
  /* 色阶通道格必须让开按钮：被盖住的那一页就再也点不到了。 */
  for (int c = 0; c < 4; ++c) {
    const Rect chip = comp_ui::adjust_channel_chip_rect(open_layout.adjust_panel, c, 4);
    COMP_CHECK(!rects_overlap(chip, open_layout.adjust_toggle));
  }

  /* 4：窄窗回落——按收起后的 24px 判断，面板与画布都不出现负宽高。 */
  const int widths[] = {0, 24, 203, 204, 205, 227, 228, 229, 400};
  for (int width : widths) {
    comp_ui::LayoutPrefs both;
    both.layer_panel_collapsed = true;
    both.adjust_panel_collapsed = true;
    const Layout layout = comp_ui::compute_layout(width, 720, both);
    COMP_CHECK_MSG(layout.layer_panel.w + layout.canvas.w + layout.adjust_panel.w == width,
                   "collapsed layout must tile the full width");
    COMP_CHECK_MSG(layout.canvas.w >= 0, "collapsed canvas must never go negative");
    COMP_CHECK_MSG(layout.layer_panel.w == 0 ||
                       layout.layer_panel.w == Layout::kCollapsedPanelWidth,
                   "collapsed layer strip is either shown at 24 px or fully absent");
    COMP_CHECK_MSG(layout.adjust_panel.w == 0 ||
                       layout.adjust_panel.w == Layout::kCollapsedPanelWidth,
                   "collapsed adjust strip is either shown at 24 px or fully absent");
    /* 面板未显示时按钮必须一起消失，否则会在画布上留一个看不见的命中区。 */
    if (layout.layer_panel.w == 0) COMP_CHECK(layout.layer_toggle.empty());
    if (layout.adjust_panel.w == 0) COMP_CHECK(layout.adjust_toggle.empty());
  }
  /* 204 = 24 + 180：图层面板刚好放得下；右侧条只剩 180px 画布，放不下。 */
  comp_ui::LayoutPrefs both;
  both.layer_panel_collapsed = true;
  both.adjust_panel_collapsed = true;
  const Layout tight = comp_ui::compute_layout(204, 720, both);
  COMP_CHECK(tight.layer_panel.w == Layout::kCollapsedPanelWidth);
  COMP_CHECK(tight.adjust_panel.w == 0);
  COMP_CHECK(tight.canvas.w == Layout::kMinCanvasWidth);
}

COMP_TEST(ui_core_layer_in_subtree) {
  /*
   * 1 是根级，2 是 1 的子层，3 是 2 的子层，4 是另一个根级（1 的兄弟），
   * 5 自引用，6/7 互指成环，8 也是 1 的子层。数组成员按面板顺序排列——数组相邻
   * 与父子关系无关，正是不能靠下标判断、「必须沿 parent 链走」的原因。
   */
  const comp_ui::LayerStackEntry stack[] = {
      {1, 0, 0}, {2, 1, 0}, {3, 2, 0}, {4, 0, 0},
      {5, 5, 0}, {6, 7, 0}, {7, 6, 0}, {8, 1, 0},
  };
  const int32_t count = static_cast<int32_t>(sizeof(stack) / sizeof(stack[0]));

  /* 直接子层与孙层都算成员；root 自己不算成员（它只是容器）。 */
  COMP_CHECK(comp_ui::layer_in_subtree(stack, count, 2, 1));
  COMP_CHECK(comp_ui::layer_in_subtree(stack, count, 3, 1));
  COMP_CHECK(comp_ui::layer_in_subtree(stack, count, 8, 1));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 1, 1));

  /* 兄弟层、以及「方向反了」的查询（组在子层下面时）都不算成员。 */
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 4, 1));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 2, 3));

  /* 表里不存在的 id：宁可少移一层，也不能当成成员。 */
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 99, 1));

  /* 环：自引用与互引用都返回 false 而不是死循环。 */
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 5, 4));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 6, 4));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 7, 4));

  /* 非法入参：空表、非正 count、root 为 0（不是图层）。 */
  COMP_CHECK(!comp_ui::layer_in_subtree(nullptr, count, 2, 1));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, 0, 2, 1));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, -3, 2, 1));
  COMP_CHECK(!comp_ui::layer_in_subtree(stack, count, 2, 0));
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
  /* 20 个带默认键的工具 + 3 个无默认键（Quick Selection / Magic Eraser /
   * Background Eraser，S17/P18 追加，shortcut 0）= 23。 */
  COMP_CHECK(count == 23);
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
                  {'o', ToolId::Tone},
                  {'c', ToolId::Crop},       {'p', ToolId::Pen},
                  {'y', ToolId::HistoryBrush}};
  /* expected[] 只列**带默认键**的工具；无键的三项不在其列。 */
  size_t keyed_count = 0;
  for (size_t i = 0; i < count; ++i) {
    if (comp_ui::tool_info_at(static_cast<int>(i))->shortcut != 0) ++keyed_count;
  }
  COMP_CHECK(sizeof(expected) / sizeof(expected[0]) == keyed_count);
  for (const auto& entry : expected) {
    COMP_CHECK_MSG(comp_ui::tool_for_shortcut(entry.key) == entry.id,
                   "tool shortcut mapping");
    /* 大写字母必须等价（Win32 的 WM_KEYDOWN 不区分大小写）。 */
    const char upper = static_cast<char>(entry.key - 'a' + 'A');
    COMP_CHECK_MSG(comp_ui::tool_for_shortcut(upper) == entry.id,
                   "uppercase shortcut must map the same tool");
  }

  /* 默认键不得重复，否则一条快捷键会指向两个工具；无默认键（shortcut 0）
   * 的工具不参与（多个 0 不算冲突）。 */
  for (size_t i = 0; i < count; ++i) {
    if (comp_ui::tool_info_at(static_cast<int>(i))->shortcut == 0) continue;
    for (size_t j = i + 1; j < count; ++j) {
      if (comp_ui::tool_info_at(static_cast<int>(j))->shortcut == 0) continue;
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
  COMP_CHECK(count == 31);
  COMP_CHECK(comp_ui::filter_count() == static_cast<int>(count));

  /* 枚举值必须与 ABI `comp_filter_kind` 的 0..30 一一对齐（shell_internal.h 里也有 static_assert）。 */
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Vignette) == 0);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::TonalContrast) == 1);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Bloom) == 2);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::LensCorrection) == 3);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::GaussianBlur) == 4);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::AddNoise) == 5);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Grain) == 6);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::MotionBlur) == 7);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::ProgressiveBlur) == 8);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Sharpen) == 9);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::ReduceNoise) == 10);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::HighPass) == 11);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Minimum) == 12);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Maximum) == 13);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Offset) == 14);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Custom) == 15);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Median) == 16);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::DustScratches) == 17);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Spherize) == 18);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Pinch) == 19);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Twirl) == 20);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Mosaic) == 21);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Emboss) == 22);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Clouds) == 23);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::DifferenceClouds) == 24);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::PictureFrame) == 25);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::FieldBlur) == 26);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::IrisBlur) == 27);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::TiltShift) == 28);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::SpinBlur) == 29);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::PathBlur) == 30);
  COMP_CHECK(static_cast<int>(comp_ui::FilterId::Count) == 31);

  /* 面板行数：docs/filters.md §3 各表的数据行数。 */
  const int row_counts[] = {5, 5, 2, 1, 2, 4, 4, 2, 4, 3, 3, 1, 1, 1, 3, 27, 1, 2,
                            1, 1, 1, 1, 3, 3, 3, 1, 1, 4, 3, 1, 2};
  for (size_t i = 0; i < count; ++i) {
    /* 只有 Custom 是网格面板（5 列）；其余滤镜必须保持整宽单列，绘制不变。 */
    if (table[i].id == comp_ui::FilterId::Custom) {
      COMP_CHECK(table[i].columns == 5);
      COMP_CHECK(table[i].grid_count == 25);
    } else {
      COMP_CHECK_MSG(table[i].columns == 1, "only Custom uses a grid layout");
      COMP_CHECK_MSG(table[i].grid_count == 0, "only Custom uses a grid layout");
    }
    COMP_CHECK_MSG(table[i].grid_count <= table[i].param_count, "grid must fit in the rows");
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

  const comp_ui::FilterSpec* blur = comp_ui::filter_spec(comp_ui::FilterId::GaussianBlur);
  COMP_CHECK(blur != nullptr && std::string(blur->name) == "Gaussian Blur");
  COMP_CHECK(near_value(blur->params[0].min_value, 0.0));
  COMP_CHECK(near_value(blur->params[0].max_value, 250.0));
  COMP_CHECK(near_value(blur->params[0].default_value, 0.0)); /* 恒等 */
  COMP_CHECK(near_value(blur->params[1].default_value, 0.0)); /* Preserve */

  const comp_ui::FilterSpec* noise = comp_ui::filter_spec(comp_ui::FilterId::AddNoise);
  COMP_CHECK(noise != nullptr && std::string(noise->name) == "Add Noise");
  COMP_CHECK(near_value(noise->params[0].max_value, 400.0));
  COMP_CHECK(near_value(noise->params[0].default_value, 0.0)); /* 恒等 */
  COMP_CHECK(near_value(noise->params[3].max_value, 4294967295.0)); /* Seed */

  const comp_ui::FilterSpec* grain = comp_ui::filter_spec(comp_ui::FilterId::Grain);
  COMP_CHECK(grain != nullptr && std::string(grain->name) == "Grain");
  COMP_CHECK(near_value(grain->params[0].default_value, 0.0)); /* 恒等 */
  COMP_CHECK(near_value(grain->params[1].default_value, 1.5)); /* Size */
  COMP_CHECK(near_value(grain->params[1].step, 0.5));
  COMP_CHECK(near_value(grain->params[2].default_value, 50.0)); /* Roughness */

  const comp_ui::FilterSpec* motion = comp_ui::filter_spec(comp_ui::FilterId::MotionBlur);
  COMP_CHECK(motion != nullptr && std::string(motion->name) == "Motion Blur");
  COMP_CHECK(near_value(motion->params[0].min_value, -90.0)); /* Angle */
  COMP_CHECK(near_value(motion->params[0].max_value, 90.0));
  COMP_CHECK(near_value(motion->params[0].default_value, 0.0));
  COMP_CHECK(near_value(motion->params[1].min_value, 0.0)); /* Distance：0 = 恒等 */
  COMP_CHECK(near_value(motion->params[1].max_value, 2000.0));
  COMP_CHECK(near_value(motion->params[1].default_value, 0.0));

  const comp_ui::FilterSpec* progressive = comp_ui::filter_spec(comp_ui::FilterId::ProgressiveBlur);
  COMP_CHECK(progressive != nullptr && std::string(progressive->name) == "Progressive Blur");
  COMP_CHECK(progressive->param_count == 4);
  COMP_CHECK(near_value(progressive->params[0].min_value, 0.0)); /* From */
  COMP_CHECK(near_value(progressive->params[0].max_value, 250.0));
  COMP_CHECK(near_value(progressive->params[0].default_value, 0.0)); /* 恒等 */
  COMP_CHECK(near_value(progressive->params[1].min_value, 0.0));      /* To */
  COMP_CHECK(near_value(progressive->params[1].max_value, 250.0));
  COMP_CHECK(near_value(progressive->params[1].default_value, 20.0));
  /* 两个开关都是 0/1 的整数行，缺省关（同 Preserve / Gaussian 的口径）。 */
  COMP_CHECK(near_value(progressive->params[2].min_value, 0.0)); /* Radial */
  COMP_CHECK(near_value(progressive->params[2].max_value, 1.0));
  COMP_CHECK(near_value(progressive->params[2].default_value, 0.0));
  COMP_CHECK(near_value(progressive->params[3].min_value, 0.0)); /* Inverted */
  COMP_CHECK(near_value(progressive->params[3].max_value, 1.0));
  COMP_CHECK(near_value(progressive->params[3].default_value, 0.0));

  const comp_ui::FilterSpec* sharpen = comp_ui::filter_spec(comp_ui::FilterId::Sharpen);
  COMP_CHECK(sharpen != nullptr && std::string(sharpen->name) == "Sharpen");
  COMP_CHECK(sharpen->param_count == 3);
  COMP_CHECK(near_value(sharpen->params[0].min_value, 0.0)); /* Amount：0 = 恒等 */
  COMP_CHECK(near_value(sharpen->params[0].max_value, 500.0));
  COMP_CHECK(near_value(sharpen->params[0].default_value, 0.0));
  COMP_CHECK(near_value(sharpen->params[1].min_value, 0.1)); /* Radius */
  COMP_CHECK(near_value(sharpen->params[1].max_value, 250.0));
  COMP_CHECK(near_value(sharpen->params[1].step, 0.5));
  COMP_CHECK(near_value(sharpen->params[1].default_value, 1.0));
  COMP_CHECK(near_value(sharpen->params[2].min_value, 0.0)); /* Threshold */
  COMP_CHECK(near_value(sharpen->params[2].max_value, 255.0));
  COMP_CHECK(near_value(sharpen->params[2].default_value, 0.0));

  const comp_ui::FilterSpec* denoise = comp_ui::filter_spec(comp_ui::FilterId::ReduceNoise);
  COMP_CHECK(denoise != nullptr && std::string(denoise->name) == "Reduce Noise");
  COMP_CHECK(denoise->param_count == 3);
  COMP_CHECK(near_value(denoise->params[0].min_value, 0.0)); /* Strength：0 = 恒等 */
  COMP_CHECK(near_value(denoise->params[0].max_value, 10.0));
  COMP_CHECK(near_value(denoise->params[0].default_value, 0.0));
  COMP_CHECK(near_value(denoise->params[1].min_value, 0.0)); /* Preserve details */
  COMP_CHECK(near_value(denoise->params[1].max_value, 100.0));
  COMP_CHECK(near_value(denoise->params[1].default_value, 50.0));
  COMP_CHECK(near_value(denoise->params[2].min_value, 0.0)); /* Color noise */
  COMP_CHECK(near_value(denoise->params[2].max_value, 100.0));
  COMP_CHECK(near_value(denoise->params[2].default_value, 25.0));

  const comp_ui::FilterSpec* high_pass = comp_ui::filter_spec(comp_ui::FilterId::HighPass);
  COMP_CHECK(high_pass != nullptr && std::string(high_pass->name) == "High Pass");
  COMP_CHECK(high_pass->param_count == 1);
  COMP_CHECK(near_value(high_pass->params[0].min_value, 0.0));  /* Radius：0 = 恒等 */
  COMP_CHECK(near_value(high_pass->params[0].max_value, 250.0));
  COMP_CHECK(near_value(high_pass->params[0].step, 0.5));
  COMP_CHECK(near_value(high_pass->params[0].default_value, 0.0));

  const comp_ui::FilterSpec* minimum = comp_ui::filter_spec(comp_ui::FilterId::Minimum);
  COMP_CHECK(minimum != nullptr && std::string(minimum->name) == "Minimum");
  COMP_CHECK(minimum->param_count == 1);
  COMP_CHECK(near_value(minimum->params[0].min_value, 0.0)); /* Radius：0 = 恒等 */
  COMP_CHECK(near_value(minimum->params[0].max_value, 100.0));
  COMP_CHECK(near_value(minimum->params[0].default_value, 0.0));

  const comp_ui::FilterSpec* maximum = comp_ui::filter_spec(comp_ui::FilterId::Maximum);
  COMP_CHECK(maximum != nullptr && std::string(maximum->name) == "Maximum");
  COMP_CHECK(maximum->param_count == 1);
  COMP_CHECK(near_value(maximum->params[0].min_value, 0.0)); /* Radius：0 = 恒等 */
  COMP_CHECK(near_value(maximum->params[0].max_value, 100.0));
  COMP_CHECK(near_value(maximum->params[0].default_value, 0.0));

  const comp_ui::FilterSpec* offset = comp_ui::filter_spec(comp_ui::FilterId::Offset);
  COMP_CHECK(offset != nullptr && std::string(offset->name) == "Offset");
  COMP_CHECK(offset->param_count == 3);
  COMP_CHECK(near_value(offset->params[0].min_value, -10000.0)); /* Horizontal */
  COMP_CHECK(near_value(offset->params[0].max_value, 10000.0));
  COMP_CHECK(near_value(offset->params[0].default_value, 0.0));
  COMP_CHECK(near_value(offset->params[1].min_value, -10000.0)); /* Vertical */
  COMP_CHECK(near_value(offset->params[1].max_value, 10000.0));
  COMP_CHECK(near_value(offset->params[1].default_value, 0.0));
  COMP_CHECK(near_value(offset->params[2].min_value, 0.0)); /* Wrap：0/1/2 */
  COMP_CHECK(near_value(offset->params[2].max_value, 2.0));
  COMP_CHECK(near_value(offset->params[2].step, 1.0));
  COMP_CHECK(near_value(offset->params[2].default_value, 0.0));

  /* Custom：5×5 核（27 行里前 25 行按 5 列铺网格）+ 除数 + 偏移。
   * 缺省必须是恒等核（中心 1，其余 0）+ 除数 1 + 偏移 0。 */
  const comp_ui::FilterSpec* custom = comp_ui::filter_spec(comp_ui::FilterId::Custom);
  COMP_CHECK(custom != nullptr && std::string(custom->name) == "Custom");
  COMP_CHECK(custom->param_count == 27);
  COMP_CHECK(custom->columns == 5);
  COMP_CHECK(custom->grid_count == 25);
  for (int row = 0; row < 25; ++row) {
    COMP_CHECK_MSG(near_value(custom->params[row].min_value, -999.0), "kernel min");
    COMP_CHECK_MSG(near_value(custom->params[row].max_value, 999.0), "kernel max");
    COMP_CHECK_MSG(near_value(custom->params[row].default_value, row == 12 ? 1.0 : 0.0),
                   "kernel default must be the identity kernel");
  }
  COMP_CHECK(std::string(custom->params[12].label) == "K13"); /* 中心 = 第 3 行第 3 列 */
  COMP_CHECK(near_value(custom->params[25].default_value, 1.0)); /* Scale */
  COMP_CHECK(near_value(custom->params[26].default_value, 0.0)); /* Offset */
  COMP_CHECK(std::string(custom->params[25].label) == "Scale");
  COMP_CHECK(std::string(custom->params[26].label) == "Offset");

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

COMP_TEST(ui_core_filter_grid_layout_and_navigation) {
  const Rect panel{248, Layout::kToolbarHeight, Layout::kAdjustPanelWidth, 600};
  const int columns = 5;
  const int grid = 25;        /* 5×5 卷积核 */
  const int param_count = 27; /* + Scale + Offset */

  /* 网格：第 row 格在第 row/5 视觉行、第 row%5 列，列宽 = 面板宽 / 列数。 */
  const int cell_w = panel.w / columns;
  for (int row = 0; row < grid; ++row) {
    const Rect rect = comp_ui::filter_param_row_rect(panel, row, columns, grid);
    COMP_CHECK(rect.x == panel.x + (row % columns) * cell_w);
    COMP_CHECK(rect.w == cell_w);
    COMP_CHECK(rect.y == panel.y + comp_ui::kFilterRowsTop +
                           (row / columns) * comp_ui::kFilterRowHeight);
  }
  /* 网格后的两行回到整宽，接在网格下面。 */
  for (int row = grid; row < param_count; ++row) {
    const Rect rect = comp_ui::filter_param_row_rect(panel, row, columns, grid);
    COMP_CHECK(rect.x == panel.x && rect.w == panel.w);
    COMP_CHECK(rect.y == panel.y + comp_ui::kFilterRowsTop +
                           (grid / columns + row - grid) * comp_ui::kFilterRowHeight);
  }

  /* 命中：每格中心 + 顶端都回同一行；网格与整宽行不串。 */
  for (int row = 0; row < param_count; ++row) {
    const Rect rect = comp_ui::filter_param_row_rect(panel, row, columns, grid);
    COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, rect.x + rect.w / 2.0,
                                            rect.y + 1.0, columns, grid) == row);
    COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, rect.x + 1.0,
                                            rect.bottom() - 1.0, columns, grid) == row);
  }
  /* 最后一条整宽行下方不命中。 */
  COMP_CHECK(comp_ui::filter_param_row_at(panel, param_count, panel.x + 4.0,
                                          panel.y + comp_ui::kFilterRowsTop +
                                              (grid / columns + 2) * comp_ui::kFilterRowHeight,
                                          columns, grid) == -1);

  /* 方向键导航：上下挪一整行、左右一格，撞边停住不折行。 */
  comp_ui::FilterPanel nav;
  nav.select(comp_ui::FilterId::Custom);
  COMP_CHECK(nav.columns() == 5 && nav.grid_count() == 25);
  COMP_CHECK(nav.focused == 0);
  nav.focus_horizontal(1);
  COMP_CHECK(nav.focused == 1);
  nav.focus_vertical(1);
  COMP_CHECK(nav.focused == 6);
  nav.focus_vertical(-1);
  COMP_CHECK(nav.focused == 1);
  nav.focus_horizontal(-1);
  COMP_CHECK(nav.focused == 0);
  nav.focus_horizontal(-1); /* 行首：停住，不折到上一行 */
  COMP_CHECK(nav.focused == 0);
  nav.focus_vertical(-1); /* 顶行再往上：停在本列首格 */
  COMP_CHECK(nav.focused == 0);

  /* 从最后一行网格往下：落到 Scale，之后按单列逐行并夹在末尾。 */
  nav.select(comp_ui::FilterId::Custom);
  for (int i = 0; i < 4; ++i) nav.focus_vertical(1);
  COMP_CHECK(nav.focused == 20);
  nav.focus_vertical(1);
  COMP_CHECK(nav.focused == 25); /* Scale */
  nav.focus_vertical(1);
  COMP_CHECK(nav.focused == 26); /* Offset */
  nav.focus_vertical(1);
  COMP_CHECK(nav.focused == 26);
  nav.focus_horizontal(1); /* 网格外的行：不是网格，左右也不折 */
  COMP_CHECK(nav.focused == 26);

  /* 单列面板：columns()/grid_count() 回落 1 / 0，上下等于普通 focus。 */
  nav.select(comp_ui::FilterId::Vignette);
  COMP_CHECK(nav.columns() == 1 && nav.grid_count() == 0);
  nav.focus_vertical(1);
  COMP_CHECK(nav.focused == 1);
  nav.focus_vertical(-1);
  COMP_CHECK(nav.focused == 0);
}

COMP_TEST(ui_core_trim_panel_cycles_and_clamps) {
  comp_ui::TrimPanel panel;
  COMP_CHECK(panel.row_count() == comp_ui::kTrimRowCount);
  COMP_CHECK(comp_ui::kTrimRowCount == 6);
  COMP_CHECK(panel.focused == 0);
  COMP_CHECK(panel.based_on == 0);
  COMP_CHECK(panel.edge[0] == 1 && panel.edge[1] == 1 && panel.edge[2] == 1 &&
             panel.edge[3] == 1);
  COMP_CHECK(panel.tolerance == 0);
  COMP_CHECK(panel.any_edge());

  /* Based On：三档循环，方向只决定前后（越界绕回另一头）。 */
  COMP_CHECK(panel.step(1));
  COMP_CHECK(panel.based_on == 1);
  COMP_CHECK(panel.step(1));
  COMP_CHECK(panel.based_on == 2);
  COMP_CHECK(panel.step(1));
  COMP_CHECK(panel.based_on == 0);
  COMP_CHECK(panel.step(-1));
  COMP_CHECK(panel.based_on == 2);
  /* direction == 0 不是「切换」而是「无动作」。 */
  COMP_CHECK(!panel.step(0));

  /* 四条边：- / = 都是翻转（不是 ±1 数值），关掉一条后 any_edge 仍为真。 */
  panel.focus(1);
  COMP_CHECK(panel.focused == 1);
  COMP_CHECK(panel.step(1));
  COMP_CHECK(panel.edge[0] == 0);
  COMP_CHECK(panel.step(-1));
  COMP_CHECK(panel.edge[0] == 1);

  /* 四边全关：引擎会拒（trimsAny guard），面板要能自己报告。 */
  panel.edge[0] = panel.edge[1] = panel.edge[2] = panel.edge[3] = 0;
  COMP_CHECK(!panel.any_edge());
  panel.reset();
  COMP_CHECK(panel.any_edge());

  /* Tolerance：逐通道 0..255，夹取且到边界报「没变化」。 */
  panel.focus(99);
  COMP_CHECK(panel.focused == 5);
  panel.set_tolerance(-7);
  COMP_CHECK(panel.tolerance == 0);
  COMP_CHECK(!panel.step(-1));
  panel.set_tolerance(999);
  COMP_CHECK(panel.tolerance == 255);
  COMP_CHECK(!panel.step(1));
  panel.set_tolerance(80);
  COMP_CHECK(panel.step(1) && panel.tolerance == 81);
  COMP_CHECK(panel.step(-1) && panel.tolerance == 80);

  /* 焦点夹在 [0, row_count-1]，不环绕。 */
  panel.focus(-99);
  COMP_CHECK(panel.focused == 0);
  panel.focus(99);
  COMP_CHECK(panel.focused == 5);

  /* 名表：三种基准与六个行名，越界给空串 / "?"（不越界读表）。 */
  COMP_CHECK(std::string(comp_ui::trim_based_on_name(0)) == "Transparent Pixels");
  COMP_CHECK(std::string(comp_ui::trim_based_on_name(1)) == "Top Left Pixel Color");
  COMP_CHECK(std::string(comp_ui::trim_based_on_name(2)) == "Bottom Right Pixel Color");
  COMP_CHECK(std::string(comp_ui::trim_based_on_name(3)) == "?");
  COMP_CHECK(std::string(comp_ui::trim_based_on_name(-1)) == "?");
  COMP_CHECK(std::string(comp_ui::trim_row_label(0)) == "Based On");
  COMP_CHECK(std::string(comp_ui::trim_row_label(5)) == "Tolerance");
  COMP_CHECK(std::string(comp_ui::trim_row_label(6)).empty());
  COMP_CHECK(std::string(comp_ui::trim_row_label(-1)).empty());
  COMP_CHECK(std::string(comp_ui::trim_edge_text(1)) == "on");
  COMP_CHECK(std::string(comp_ui::trim_edge_text(0)) == "off");
}

COMP_TEST(ui_core_trim_rows_hit_test_matches_geometry) {
  const Rect panel{248, Layout::kToolbarHeight, Layout::kAdjustPanelWidth, 600};
  const int row_count = comp_ui::kTrimRowCount;

  for (int row = 0; row < row_count; ++row) {
    const Rect rect = comp_ui::trim_row_rect(panel, row);
    COMP_CHECK(rect.x == panel.x && rect.w == panel.w);
    COMP_CHECK(rect.y == panel.y + comp_ui::kTrimRowsTop + row * comp_ui::kTrimRowHeight);
    COMP_CHECK(rect.h == comp_ui::kTrimRowHeight);
    COMP_CHECK(comp_ui::trim_row_at(panel, row_count, rect.x + 4.0, rect.y) == row);
    COMP_CHECK(comp_ui::trim_row_at(panel, row_count, rect.x + 4.0, rect.bottom() - 1.0) ==
               row);
  }

  /* 行区上方（标题 / 副标题）不命中；行数外、面板外、row_count 0 都不命中。 */
  COMP_CHECK(comp_ui::trim_row_at(panel, row_count, panel.x + 4.0,
                                  panel.y + comp_ui::kTrimRowsTop - 1.0) == -1);
  COMP_CHECK(comp_ui::trim_row_at(panel, row_count, panel.x + 4.0,
                                  panel.y + comp_ui::kTrimRowsTop +
                                      row_count * comp_ui::kTrimRowHeight) == -1);
  COMP_CHECK(comp_ui::trim_row_at(panel, row_count, panel.x - 1.0,
                                  panel.y + comp_ui::kTrimRowsTop + 1.0) == -1);
  COMP_CHECK(comp_ui::trim_row_at(panel, 0, panel.x + 4.0,
                                  panel.y + comp_ui::kTrimRowsTop + 1.0) == -1);
}

COMP_TEST(ui_core_clone_stroke_offset_matches_engine_rounding) {
  /*
   * 引擎在 has_offset == 0 时按 round(source - first_point) 定首笔偏移；宿主
   * 必须算出同一组整数，否则「首笔正确、后续每一笔都错位」——而且这种错位
   * 看起来像引擎的锅，实际上是宿主截断/取整口径不同。
   */
  double ox = 0.0;
  double oy = 0.0;
  comp_ui::clone_stroke_offset(100.0, 50.0, 10.4, 20.6, &ox, &oy);
  COMP_CHECK(ox == 90.0); /* 89.6 -> 90 */
  COMP_CHECK(oy == 29.0); /* 29.4 -> 29 */

  /* .5 远离零（std::lround）：正负都要对，static_cast<int> 会两边都变 0。 */
  comp_ui::clone_stroke_offset(0.0, 0.0, 0.5, -0.5, &ox, &oy);
  COMP_CHECK(ox == -1.0);
  COMP_CHECK(oy == 1.0);

  /* 负偏移（源在落点左上）：-2.5 -> -3、-2.4 -> -2，不能 floor 成 -3/-3。 */
  comp_ui::clone_stroke_offset(10.0, 10.0, 12.5, 12.4, &ox, &oy);
  COMP_CHECK(ox == -3.0);
  COMP_CHECK(oy == -2.0);

  /* 两个分量可以只取一个（壳里没有这种调用，但空指针必须安全）。 */
  ox = 123.0;
  comp_ui::clone_stroke_offset(5.0, 5.0, 1.0, 1.0, &ox, nullptr);
  COMP_CHECK(ox == 4.0);

  /* 笔迹种类互不相同：提交分支按等值分发，撞值会把仿制送进别的算子。 */
  COMP_CHECK(comp_ui::StrokeKind::Clone != comp_ui::StrokeKind::None);
  COMP_CHECK(comp_ui::StrokeKind::Clone != comp_ui::StrokeKind::Paint);
  COMP_CHECK(comp_ui::StrokeKind::Clone != comp_ui::StrokeKind::Retouch);
  COMP_CHECK(comp_ui::StrokeKind::Clone != comp_ui::StrokeKind::HistoryBrush);
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
  COMP_CHECK(comp_ui::tool_has_brush_tip(comp_ui::ToolId::Tone));
  COMP_CHECK(comp_ui::tool_has_brush_tip(comp_ui::ToolId::Clone));
  COMP_CHECK(comp_ui::tool_has_brush_tip(comp_ui::ToolId::HistoryBrush));
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

COMP_TEST(ui_core_brush_preset_flow_and_pen_pressure) {
  /* 预设表只读：0 号是「自定义」，它不覆盖任何值，只当当前状态的显示名。 */
  COMP_CHECK(comp_ui::kBrushPresetCount == 5);
  COMP_CHECK(std::string(comp_ui::brush_preset_name(0)) == "自定义");
  COMP_CHECK(comp_ui::brush_preset_is_custom(0));
  COMP_CHECK(comp_ui::brush_preset_is_custom(-3));
  COMP_CHECK(!comp_ui::brush_preset_is_custom(1));
  for (int i = 0; i < comp_ui::kBrushPresetCount; ++i) {
    const comp_ui::BrushPresetSpec spec = comp_ui::brush_preset_spec(i);
    COMP_CHECK(spec.name != nullptr && spec.name[0] != '\0');
    /* 预设值必须自身就在夹取范围内——否则点一下就得到一个引擎会拒的笔。 */
    COMP_CHECK(spec.size >= comp_ui::kMinBrushSize && spec.size <= comp_ui::kMaxBrushSize);
    COMP_CHECK(spec.hardness >= 0.0 && spec.hardness <= 1.0);
    COMP_CHECK(spec.flow >= comp_ui::kMinBrushFlow && spec.flow <= comp_ui::kMaxBrushFlow);
  }
  /* 越界下标读 0 号，不读表外内存。 */
  COMP_CHECK(std::string(comp_ui::brush_preset_name(99)) == "自定义");

  /* 流量：0..1；0 与 NaN 都回落到下限（flow = 0 会让整笔一个像素都不落）。 */
  COMP_CHECK(near_value(comp_ui::clamp_brush_flow(0.5), 0.5));
  COMP_CHECK(near_value(comp_ui::clamp_brush_flow(0.0), comp_ui::kMinBrushFlow));
  COMP_CHECK(near_value(comp_ui::clamp_brush_flow(-2.0), comp_ui::kMinBrushFlow));
  COMP_CHECK(near_value(comp_ui::clamp_brush_flow(std::nan("")), comp_ui::kMinBrushFlow));
  COMP_CHECK(near_value(comp_ui::clamp_brush_flow(7.5), comp_ui::kMaxBrushFlow));
  COMP_CHECK(near_value(comp_ui::step_brush_flow(1.0, 1), comp_ui::kMaxBrushFlow));
  COMP_CHECK(near_value(comp_ui::step_brush_flow(1.0, -1), 0.95));
  COMP_CHECK(near_value(comp_ui::step_brush_flow(0.03, -1), comp_ui::kMinBrushFlow));
  COMP_CHECK(near_value(comp_ui::step_brush_flow(0.5, 0), 0.5));

  /* 笔压：0..1024 -> 0..1；没有 PEN_MASK_PRESSURE（鼠标）一律满笔压 1.0。 */
  COMP_CHECK(near_value(comp_ui::pen_pressure_to_unit(1024, true), 1.0));
  COMP_CHECK(near_value(comp_ui::pen_pressure_to_unit(512, true), 0.5));
  COMP_CHECK(near_value(comp_ui::pen_pressure_to_unit(0, true), 0.0));
  COMP_CHECK(near_value(comp_ui::pen_pressure_to_unit(700, false), 1.0));

  /* 整笔取采样平均；没有采样（鼠标路径）必须回落到 1.0，像素才与 P1-4 前一致。 */
  COMP_CHECK(near_value(comp_ui::average_stroke_pressure(0.0, 0), 1.0));
  COMP_CHECK(near_value(comp_ui::average_stroke_pressure(1.5, 3), 0.5));
  COMP_CHECK(near_value(comp_ui::average_stroke_pressure(-1.0, 2), 1.0));
}

COMP_TEST(ui_core_tone_states_map_to_abi_operators_and_modes) {
  /* O 键循环：Dodge 中间调 -> 阴影 -> 高光 -> Burn 三档 -> Sponge 两档 -> 回到起点。 */
  COMP_CHECK(comp_ui::kToneStateCount == 8);
  COMP_CHECK(comp_ui::next_tone_state(comp_ui::kToneDodgeMidtone) == comp_ui::kToneDodgeShadow);
  COMP_CHECK(comp_ui::next_tone_state(comp_ui::kToneBurnHighlight) ==
             comp_ui::kToneSpongeDesaturate);
  COMP_CHECK(comp_ui::next_tone_state(comp_ui::kToneSpongeSaturate) ==
             comp_ui::kToneDodgeMidtone);
  /* 越界不读表外内存：一律回到起点。 */
  COMP_CHECK(comp_ui::next_tone_state(-1) == comp_ui::kToneDodgeMidtone);
  COMP_CHECK(comp_ui::next_tone_state(99) == comp_ui::kToneDodgeMidtone);

  /* 状态 -> (算子, mode)：状态栏文案与实际下发的参数必须同源。 */
  COMP_CHECK(comp_ui::tone_state_tool(comp_ui::kToneDodgeMidtone) == comp_ui::kToneToolDodge);
  COMP_CHECK(comp_ui::tone_state_tool(comp_ui::kToneDodgeHighlight) == comp_ui::kToneToolDodge);
  COMP_CHECK(comp_ui::tone_state_tool(comp_ui::kToneBurnShadow) == comp_ui::kToneToolBurn);
  COMP_CHECK(comp_ui::tone_state_tool(comp_ui::kToneSpongeSaturate) == comp_ui::kToneToolSponge);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneDodgeMidtone) == 0);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneDodgeShadow) == 1);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneDodgeHighlight) == 2);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneBurnMidtone) == 0);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneBurnHighlight) == 2);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneSpongeDesaturate) == 0);
  COMP_CHECK(comp_ui::tone_state_mode(comp_ui::kToneSpongeSaturate) == 1);

  /* 文案逐条冻结：状态栏、笔迹提示、撤销步标签都用它。 */
  COMP_CHECK(std::string(comp_ui::tone_state_name(comp_ui::kToneDodgeMidtone)) ==
             "Dodge / Midtones");
  COMP_CHECK(std::string(comp_ui::tone_state_name(comp_ui::kToneBurnShadow)) == "Burn / Shadows");
  COMP_CHECK(std::string(comp_ui::tone_state_name(comp_ui::kToneSpongeDesaturate)) ==
             "Sponge / Desaturate");
  COMP_CHECK(std::string(comp_ui::tone_state_name(-1)) == "?");
  COMP_CHECK(std::string(comp_ui::tone_state_name(8)) == "?");

  /* 强度：0 / 负数 / NaN 回落到 1%（引擎闸会拒绝 0），上限 100%，步进 0.1。 */
  COMP_CHECK(near_value(comp_ui::clamp_tone_strength(0.0), comp_ui::kMinToneStrength));
  COMP_CHECK(near_value(comp_ui::clamp_tone_strength(-1.0), comp_ui::kMinToneStrength));
  COMP_CHECK(near_value(comp_ui::clamp_tone_strength(std::nan("")), comp_ui::kMinToneStrength));
  COMP_CHECK(near_value(comp_ui::clamp_tone_strength(5.0), comp_ui::kMaxToneStrength));
  COMP_CHECK(near_value(comp_ui::step_tone_strength(0.5, 1), 0.6));
  COMP_CHECK(near_value(comp_ui::step_tone_strength(0.5, -1), 0.4));
  COMP_CHECK(near_value(comp_ui::step_tone_strength(0.5, 0), 0.5));

  double strength = comp_ui::kMinToneStrength;
  for (int i = 0; i < 100; ++i) strength = comp_ui::step_tone_strength(strength, 1);
  COMP_CHECK(near_value(strength, comp_ui::kMaxToneStrength));
  for (int i = 0; i < 100; ++i) strength = comp_ui::step_tone_strength(strength, -1);
  COMP_CHECK(near_value(strength, comp_ui::kMinToneStrength));
}

COMP_TEST(ui_core_history_brush_opacity_steps_stay_inside_the_engine_gate) {
  /* 追加式工具表：历史记录画笔排最后，旧工具下标不移动。 */
  COMP_CHECK(comp_ui::tool_index(comp_ui::ToolId::HistoryBrush) == 19);
  COMP_CHECK(static_cast<int>(comp_ui::StrokeKind::HistoryBrush) == 4);

  /* 引擎的闸只接受 (0, 1]，键盘步进不得停在会被拒的 0 上。 */
  COMP_CHECK(comp_ui::kMinHistoryBrushOpacity > 0.0);
  COMP_CHECK(near_value(comp_ui::clamp_history_brush_opacity(0.0),
                        comp_ui::kMinHistoryBrushOpacity));
  COMP_CHECK(near_value(comp_ui::clamp_history_brush_opacity(-0.5),
                        comp_ui::kMinHistoryBrushOpacity));
  COMP_CHECK(near_value(comp_ui::clamp_history_brush_opacity(std::nan("")),
                        comp_ui::kMinHistoryBrushOpacity));
  COMP_CHECK(near_value(comp_ui::clamp_history_brush_opacity(7.0),
                        comp_ui::kMaxHistoryBrushOpacity));
  COMP_CHECK(near_value(comp_ui::step_history_brush_opacity(0.5, 1), 0.6));
  COMP_CHECK(near_value(comp_ui::step_history_brush_opacity(0.5, -1), 0.4));
  COMP_CHECK(near_value(comp_ui::step_history_brush_opacity(0.5, 0), 0.5));

  double opacity = comp_ui::kMinHistoryBrushOpacity;
  for (int i = 0; i < 100; ++i) opacity = comp_ui::step_history_brush_opacity(opacity, 1);
  COMP_CHECK(near_value(opacity, comp_ui::kMaxHistoryBrushOpacity));
  for (int i = 0; i < 100; ++i) opacity = comp_ui::step_history_brush_opacity(opacity, -1);
  COMP_CHECK(near_value(opacity, comp_ui::kMinHistoryBrushOpacity));
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
  COMP_CHECK(comp_ui::tool_shortcut_target(ToolId::Brush, 'o') == ToolId::Tone);
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

/* ------------------------------------------- P0-5: 边界 / 平滑半径 */

COMP_TEST(ui_core_selection_smooth_clamps_to_engine_range) {
  /* 边界复用扩展/收缩的 1..500；平滑是 PS 同量程的 1..100。 */
  COMP_CHECK(comp_ui::clamp_selection_smooth(0) == 1);
  COMP_CHECK(comp_ui::clamp_selection_smooth(-3) == 1);
  COMP_CHECK(comp_ui::clamp_selection_smooth(1) == 1);
  COMP_CHECK(comp_ui::clamp_selection_smooth(100) == 100);
  COMP_CHECK(comp_ui::clamp_selection_smooth(101) == 100);
  COMP_CHECK(comp_ui::clamp_selection_smooth(5000) == 100);
  COMP_CHECK(comp_ui::kMinSelectionSmooth == 1);
  COMP_CHECK(comp_ui::kMaxSelectionSmooth == 100);
}

COMP_TEST(ui_core_coverage_intersects_selection_mask) {
  /* 255 旁路、0 全丢、中间值按比例并四舍五入。 */
  uint8_t coverage[] = {255, 255, 255, 200, 0, 0};
  const uint8_t mask[] = {255, 128, 0, 64, 255, 0};
  COMP_CHECK(comp_ui::intersect_coverage_with_selection(coverage, mask, 6));
  COMP_CHECK(coverage[0] == 255);
  COMP_CHECK(coverage[1] == 128); /* 255*128/255 */
  COMP_CHECK(coverage[2] == 0);
  COMP_CHECK(coverage[3] == 50);  /* 200*64/255 = 50.2 -> 50 */
  COMP_CHECK(coverage[4] == 0);    /* coverage 为 0 时无论蒙版多大都留 0 */
  COMP_CHECK(coverage[5] == 0);

  /* 四舍五入不能把 255*255 溢出成 254：全选必须原样不动。 */
  uint8_t full[] = {255, 128};
  const uint8_t full_mask[] = {255, 255};
  COMP_CHECK(comp_ui::intersect_coverage_with_selection(full, full_mask, 2));
  COMP_CHECK(full[0] == 255);
  COMP_CHECK(full[1] == 128);

  /* 空指针是调用方缺陷：返回 false 且不改字节。 */
  uint8_t untouched[] = {77};
  COMP_CHECK(!comp_ui::intersect_coverage_with_selection(untouched, nullptr, 1));
  COMP_CHECK(!comp_ui::intersect_coverage_with_selection(nullptr, untouched, 1));
  COMP_CHECK(untouched[0] == 77);
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
 * 智能滤镜栈面板（P1-2 收尾 ⑨）：列表行绘制与命中同源。几何与路径/历史/通道
 * 共用 list_* 家族，这里钉住标题（64）+ 页脚（5 行提示 96）之后的可用行数、
 * 行高 22、滚动夹取和「只认画得出来的行」——面板绘制与 WM_LBUTTONDOWN 都读这组
 * 函数，任何一侧改常量另一侧必须一起变。
 */
COMP_TEST(ui_core_smart_panel_rows_hit_what_they_paint) {
  const comp_ui::Rect panel{724, 46, 330, 560};
  /* 560 - 64（标题 + 摘要）- 96（5 行提示）= 400，每行 22 => 18 行。 */
  COMP_CHECK(comp_ui::smart_visible_rows(panel) == 18);
  COMP_CHECK(comp_ui::smart_visible_rows(comp_ui::Rect{724, 46, 330, 150}) == 0);

  const comp_ui::Rect first = comp_ui::smart_row_rect(panel, 0);
  COMP_CHECK(first.x == 724 && first.y == 110 && first.w == 330 && first.h == 22);
  COMP_CHECK(comp_ui::smart_row_rect(panel, 17).y == 110 + 17 * 22);

  COMP_CHECK(comp_ui::smart_row_at(panel, 1, 0, 800.0, 111.0) == 0);
  COMP_CHECK(comp_ui::smart_row_at(panel, 2, 0, 800.0, 132.0) == 1);
  COMP_CHECK(comp_ui::smart_row_at(panel, 2, 0, 800.0, 154.0) == -1); /* 行号超出条数 */
  COMP_CHECK(comp_ui::smart_row_at(panel, 2, 0, 800.0, 109.0) == -1); /* 标题/摘要区 */
  COMP_CHECK(comp_ui::smart_row_at(panel, 2, 0, 100.0, 111.0) == -1); /* 面板外 */

  COMP_CHECK(comp_ui::smart_max_scroll(panel, 3) == 0);
  COMP_CHECK(comp_ui::smart_max_scroll(panel, 30) == 12);
  COMP_CHECK(comp_ui::smart_clamp_scroll(panel, 30, -1) == 0);
  COMP_CHECK(comp_ui::smart_clamp_scroll(panel, 30, 99) == 12);
  COMP_CHECK(comp_ui::smart_clamp_scroll(panel, 30, 4) == 4);

  COMP_CHECK(comp_ui::smart_ensure_visible(panel, 30, 3, 5) == 3);
  COMP_CHECK(comp_ui::smart_ensure_visible(panel, 30, 25, 5) == 8);
  COMP_CHECK(comp_ui::smart_ensure_visible(panel, 30, 25, 99) == 12);
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
  COMP_CHECK(comp_ui::kGradientAngle == 2);
  COMP_CHECK(comp_ui::kGradientReflected == 3);
  COMP_CHECK(comp_ui::kGradientDiamond == 4);
  COMP_CHECK(comp_ui::kGradientTypeCount == 5);
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientLinear)) == "Linear");
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientRadial)) == "Radial");
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientAngle)) == "Angle");
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientReflected)) == "Reflected");
  COMP_CHECK(std::string(comp_ui::gradient_type_name(comp_ui::kGradientDiamond)) == "Diamond");
  /* 未知值如实报 "?"（不猜成 Linear，便于面板暴露状态错乱）。 */
  COMP_CHECK(std::string(comp_ui::gradient_type_name(7)) == "?");

  /* G 键循环：五种类型首尾相接；任意未知值都收敛回线性。 */
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientLinear) == comp_ui::kGradientRadial);
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientRadial) == comp_ui::kGradientAngle);
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientAngle) ==
             comp_ui::kGradientReflected);
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientReflected) == comp_ui::kGradientDiamond);
  COMP_CHECK(comp_ui::next_gradient_type(comp_ui::kGradientDiamond) == comp_ui::kGradientLinear);
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

COMP_TEST(ui_core_gradient_presets_expand_to_ascending_stop_tables) {
  /* 预设表是外壳喂给 ABI 的唯一多色标来源：位置必须严格升序、中点必须在 (0,1)。 */
  const uint8_t foreground[4] = {10, 20, 30, 255};
  comp_ui::GradientPresetStop stops[5];

  COMP_CHECK(comp_ui::kGradientPresetCount == 4);
  /*
   * 第 4 档「自定义」没有内置表：0..2 是预设，3 读 ShellState 里那张可编辑表
   * （见下面的 ui_core_gradient_custom_stop_table_editing）。
   */
  for (int preset = 0; preset < comp_ui::kGradientPresetCustom; ++preset) {
    int is_foreground = -1;
    const int expected = comp_ui::gradient_preset_stop_count(preset, &is_foreground);
    COMP_CHECK(expected >= 2 && expected <= 5);
    COMP_CHECK(is_foreground == (preset == 0 ? 1 : 0));
    const int count = comp_ui::gradient_preset_stops(preset, foreground, false, stops, 5);
    COMP_CHECK(count == expected);
    COMP_CHECK(stops[0].position == 0.0);
    COMP_CHECK(stops[count - 1].position == 1.0);
    for (int i = 0; i < count; ++i) {
      COMP_CHECK(stops[i].midpoint > 0.0 && stops[i].midpoint < 1.0);
      if (i > 0) COMP_CHECK(stops[i].position > stops[i - 1].position);
    }
  }

  /* 预设 0 用前景色：首色标不透明、末色标全透明。 */
  int foreground_flag = 0;
  COMP_CHECK(comp_ui::gradient_preset_stop_count(0, &foreground_flag) == 2);
  COMP_CHECK(comp_ui::gradient_preset_stops(0, foreground, false, stops, 5) == 2);
  COMP_CHECK(stops[0].color[0] == 10 && stops[0].color[1] == 20 && stops[0].color[2] == 30);
  COMP_CHECK(stops[0].color[3] == 255 && stops[1].color[3] == 0);

  /* 反向：位置镜像、颜色顺序翻转，段中点也镜像（反向段中点 = 1 - 原段中点）。 */
  comp_ui::GradientPresetStop forward[5];
  COMP_CHECK(comp_ui::gradient_preset_stops(2, foreground, false, forward, 5) == 5);
  COMP_CHECK(comp_ui::gradient_preset_stops(2, foreground, true, stops, 5) == 5);
  for (int i = 0; i < 3; ++i) {
    for (int c = 0; c < 4; ++c) {
      COMP_CHECK(stops[i].color[c] == forward[4 - i].color[c]);
    }
  }
  COMP_CHECK(stops[0].position == 0.0 && stops[4].position == 1.0);
  COMP_CHECK(std::fabs(stops[0].midpoint - (1.0 - forward[3].midpoint)) < 1e-12);
  COMP_CHECK(std::fabs(stops[1].midpoint - (1.0 - forward[2].midpoint)) < 1e-12);

  /* 容量不足时返回 0，且不写半张表（调用方据此走两色标旧路径）。 */
  comp_ui::GradientPresetStop small[2];
  COMP_CHECK(comp_ui::gradient_preset_stops(2, foreground, false, small, 2) == 0);

  /* 「自定义」是空档：没有内置表，也不假装有一张（空 = 0 个色标）。 */
  COMP_CHECK(comp_ui::kGradientPresetCustom == 3);
  COMP_CHECK(comp_ui::kGradientMaxStops == 32);
  COMP_CHECK(std::string(comp_ui::gradient_preset_name(comp_ui::kGradientPresetCustom)) == "自定义");
  int custom_foreground = -1;
  COMP_CHECK(comp_ui::gradient_preset_stop_count(comp_ui::kGradientPresetCustom,
                                                 &custom_foreground) == 0);
  COMP_CHECK(comp_ui::gradient_preset_stops(comp_ui::kGradientPresetCustom, foreground, false,
                                            stops, 5) == 0);
}

COMP_TEST(ui_core_gradient_custom_stop_table_editing) {
  /*
   * P1-7 收尾：可编辑色标表的纯逻辑（ui_core）。面板只是这些函数的键鼠外壳，
   * 所以数值边界全在这里钉死——首末固定、中间与邻居至少 1‰、增删只动中间项。
   */
  comp_ui::GradientPresetStop table[comp_ui::kGradientMaxStops];
  for (int i = 0; i < 3; ++i) {
    table[i].position = i * 0.5;
    table[i].midpoint = 0.5;
    table[i].color[0] = static_cast<uint8_t>(10 * (i + 1));
    table[i].color[1] = static_cast<uint8_t>(20 * (i + 1));
    table[i].color[2] = static_cast<uint8_t>(30 * (i + 1));
    table[i].color[3] = 255;
  }

  /* 位置 -> 千分位：四舍五入、夹到 0..1000、非有限值按 0（面板不显示 NaN）。 */
  COMP_CHECK(comp_ui::gradient_position_permille(0.0) == 0);
  COMP_CHECK(comp_ui::gradient_position_permille(1.0) == 1000);
  COMP_CHECK(comp_ui::gradient_position_permille(0.125) == 125);
  COMP_CHECK(comp_ui::gradient_position_permille(0.1234) == 123);
  COMP_CHECK(comp_ui::gradient_position_permille(0.9999) == 1000);
  COMP_CHECK(comp_ui::gradient_position_permille(-0.5) == 0);
  COMP_CHECK(comp_ui::gradient_position_permille(1.5) == 1000);
  COMP_CHECK(comp_ui::gradient_position_permille(std::nan("")) == 0);

  /* 可编辑区间：首 0、末 1000、中间是本邻居之间（各留 1‰ 保证严格升序）。 */
  int low = -1;
  int high = -1;
  comp_ui::gradient_stop_position_bounds_permille(table, 3, 0, &low, &high);
  COMP_CHECK(low == 0 && high == 0);
  comp_ui::gradient_stop_position_bounds_permille(table, 3, 1, &low, &high);
  COMP_CHECK(low == 1 && high == 999);
  comp_ui::gradient_stop_position_bounds_permille(table, 3, 2, &low, &high);
  COMP_CHECK(low == 1000 && high == 1000);
  comp_ui::gradient_stop_position_bounds_permille(table, 3, 9, &low, &high);
  COMP_CHECK(low == 0 && high == 1000); /* 越界下标：整段，调用方本来就不该写 */
  comp_ui::gradient_stop_position_bounds_permille(nullptr, 3, 1, &low, &high);
  COMP_CHECK(low == 0 && high == 1000);

  /* 写位置：夹到区间；端点写别的值也不动。 */
  comp_ui::gradient_set_stop_position_permille(table, 3, 1, 5);
  COMP_CHECK(table[1].position == 0.005);
  comp_ui::gradient_set_stop_position_permille(table, 3, 1, 5000);
  COMP_CHECK(table[1].position == 0.999);
  comp_ui::gradient_set_stop_position_permille(table, 3, 0, 700);
  COMP_CHECK(table[0].position == 0.0);
  comp_ui::gradient_set_stop_position_permille(table, 3, 2, 0);
  COMP_CHECK(table[2].position == 1.0);
  table[1].position = 0.5;

  /* 中点只属于本段，夹到 (0,1) 开区间——引擎要求严格。 */
  comp_ui::gradient_set_stop_midpoint_permille(table, 3, 1, 0);
  COMP_CHECK(table[1].midpoint == 0.001);
  comp_ui::gradient_set_stop_midpoint_permille(table, 3, 1, 1000);
  COMP_CHECK(table[1].midpoint == 0.999);
  comp_ui::gradient_set_stop_midpoint_permille(table, 3, 1, 420);
  COMP_CHECK(table[1].midpoint == 0.42);
  comp_ui::gradient_set_stop_midpoint_permille(table, 3, 7, 420); /* 越界：不改表 */
  COMP_CHECK(table[1].midpoint == 0.42);

  /* 插入：选中中间项 -> 与右邻居之间；颜色抄左端，段中点 0.5。 */
  COMP_CHECK(comp_ui::gradient_insert_stop(table, 3, 1) == 4);
  COMP_CHECK(table[2].position == 0.75);
  COMP_CHECK(table[2].midpoint == 0.5);
  COMP_CHECK(table[2].color[0] == table[1].color[0]);
  COMP_CHECK(table[2].color[3] == 255);
  COMP_CHECK(table[3].position == 1.0);
  /* 选中末项 -> 用左边那段（插在它前面）。 */
  COMP_CHECK(comp_ui::gradient_insert_stop(table, 4, 3) == 5);
  COMP_CHECK(table[3].position == 0.875);
  COMP_CHECK(table[4].position == 1.0);
  /* 参数闸：越界/满表/退化段都不改表、返回 0。 */
  COMP_CHECK(comp_ui::gradient_insert_stop(table, 5, -1) == 0);
  COMP_CHECK(comp_ui::gradient_insert_stop(table, 5, 5) == 0);
  COMP_CHECK(comp_ui::gradient_insert_stop(nullptr, 5, 0) == 0);
  COMP_CHECK(comp_ui::gradient_insert_stop(table, 1, 0) == 0);
  COMP_CHECK(comp_ui::gradient_insert_stop(table, comp_ui::kGradientMaxStops, 0) == 0);
  table[1].position = 0.5;
  table[2].position = 0.5;
  const double before = table[2].position;
  COMP_CHECK(comp_ui::gradient_insert_stop(table, 3, 1) == 0);
  COMP_CHECK(table[2].position == before);

  /* 删除：只有中间项能删（引擎至少 2 个，首末固定 0% / 100%）。 */
  for (int i = 0; i < 3; ++i) {
    table[i].position = i * 0.5;
    table[i].midpoint = 0.5;
  }
  COMP_CHECK(comp_ui::gradient_delete_stop(table, 3, 1) == 2);
  COMP_CHECK(table[0].position == 0.0 && table[1].position == 1.0);
  COMP_CHECK(comp_ui::gradient_delete_stop(table, 2, 0) == 0);
  COMP_CHECK(comp_ui::gradient_delete_stop(table, 2, 1) == 0);
  COMP_CHECK(comp_ui::gradient_delete_stop(table, 3, 0) == 0);
  COMP_CHECK(comp_ui::gradient_delete_stop(table, 3, 2) == 0);
  COMP_CHECK(comp_ui::gradient_delete_stop(nullptr, 3, 1) == 0);

  /* 反向：整表镜像，颜色跟着走，反向段中点 = 1 - 原段左中点，末项写 0.5。 */
  comp_ui::GradientPresetStop forward[4];
  const double positions[4] = {0.0, 0.25, 0.6, 1.0};
  const double midpoints[4] = {0.4, 0.3, 0.2, 0.5};
  for (int i = 0; i < 4; ++i) {
    forward[i].position = positions[i];
    forward[i].midpoint = midpoints[i];
    for (int c = 0; c < 4; ++c) forward[i].color[c] = static_cast<uint8_t>(i * 10 + c);
  }
  comp_ui::GradientPresetStop reversed[4];
  COMP_CHECK(comp_ui::reverse_gradient_stops_copy(forward, 4, reversed, 4) == 4);
  COMP_CHECK(reversed[0].position == 0.0 && reversed[3].position == 1.0);
  COMP_CHECK(std::fabs(reversed[1].position - 0.4) < 1e-12);
  COMP_CHECK(std::fabs(reversed[2].position - 0.75) < 1e-12);
  COMP_CHECK(reversed[0].color[0] == forward[3].color[0]);
  COMP_CHECK(reversed[3].color[0] == forward[0].color[0]);
  COMP_CHECK(std::fabs(reversed[0].midpoint - (1.0 - forward[2].midpoint)) < 1e-12);
  COMP_CHECK(reversed[3].midpoint == 0.5);
  /* src == out：走栈上副本，偶/奇长度都不会读到被覆盖的元素。 */
  comp_ui::GradientPresetStop alias[4];
  for (int i = 0; i < 4; ++i) alias[i] = forward[i];
  COMP_CHECK(comp_ui::reverse_gradient_stops_copy(alias, 4, alias, 4) == 4);
  COMP_CHECK(alias[0].color[0] == forward[3].color[0]);
  COMP_CHECK(std::fabs(alias[1].position - 0.4) < 1e-12);
  COMP_CHECK(comp_ui::reverse_gradient_stops_copy(forward, 4, reversed, 3) == 0);
  COMP_CHECK(comp_ui::reverse_gradient_stops_copy(nullptr, 4, reversed, 4) == 0);
  COMP_CHECK(comp_ui::reverse_gradient_stops_copy(forward, 33, reversed, 33) == 0);

  /* 整表校验：逐条对齐引擎，包括「至少一个不透明色标」。 */
  for (int i = 0; i < 3; ++i) {
    table[i].position = i * 0.5;
    table[i].midpoint = 0.5;
    table[i].color[3] = 255;
  }
  COMP_CHECK(comp_ui::gradient_stops_valid(table, 3));
  COMP_CHECK(!comp_ui::gradient_stops_valid(table, 1));
  COMP_CHECK(!comp_ui::gradient_stops_valid(nullptr, 3));
  table[1].position = 0.0; /* 与首色标重合：不严格升序 */
  COMP_CHECK(!comp_ui::gradient_stops_valid(table, 3));
  table[1].position = 0.6;
  COMP_CHECK(comp_ui::gradient_stops_valid(table, 3));
  table[0].midpoint = 1.0;
  COMP_CHECK(!comp_ui::gradient_stops_valid(table, 3));
  table[0].midpoint = 0.5;
  table[2].position = 1.2;
  COMP_CHECK(!comp_ui::gradient_stops_valid(table, 3));
  table[2].position = 1.0;
  for (int i = 0; i < 3; ++i) table[i].color[3] = 0;
  COMP_CHECK(!comp_ui::gradient_stops_valid(table, 3)); /* 全透明：引擎会拒，外壳先说 */
  table[0].color[3] = 1;
  COMP_CHECK(comp_ui::gradient_stops_valid(table, 3));
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
   * 字体轨道的文本是 UTF-8，字符只从 WM_CHAR/输入法进来（外壳不再自己把
   * VK 翻成 ASCII——那会吞掉输入法，也和 WM_CHAR 重复插一遍）。
   * 一个码点 1..4 字节，退格必须整段删：按字节删会留下半个码点，引擎如实
   * 拒收成 kBadText。
   */
  {
    std::string buffer;
    comp_ui::type_utf8_append(buffer, 'A');
    comp_ui::type_utf8_append(buffer, 0x4F60u);  /* 你：3 字节 */
    comp_ui::type_utf8_append(buffer, 0x1F600u); /* 😀：4 字节（代理对在壳里拼好） */
    COMP_CHECK(buffer.size() == 1 + 3 + 4);

    uint32_t cp = 0;
    COMP_CHECK(comp_ui::type_utf8_next(buffer.c_str(), &cp) == 1 && cp == 'A');
    COMP_CHECK(comp_ui::type_utf8_next(buffer.c_str() + 1, &cp) == 3 && cp == 0x4F60u);
    COMP_CHECK(comp_ui::type_utf8_next(buffer.c_str() + 4, &cp) == 4 && cp == 0x1F600u);

    COMP_CHECK(comp_ui::type_utf8_erase_last(buffer) == 4 && buffer.size() == 4);
    COMP_CHECK(comp_ui::type_utf8_erase_last(buffer) == 3 && buffer.size() == 1);
    COMP_CHECK(comp_ui::type_utf8_erase_last(buffer) == 1 && buffer.empty());
    COMP_CHECK(comp_ui::type_utf8_erase_last(buffer) == 0);
  }

  /* 拒绝：截断、孤立续字节、控制字符（含 Tab）、代理区、越界码点。 */
  {
    uint32_t cp = 0;
    COMP_CHECK(comp_ui::type_utf8_next("\xE4\xBD", &cp) == 0);
    COMP_CHECK(comp_ui::type_utf8_next("\xA0", &cp) == 0);
    COMP_CHECK(comp_ui::type_utf8_next("\n", &cp) == 0);
    COMP_CHECK(comp_ui::type_utf8_next("\x09", &cp) == 0);
    COMP_CHECK(comp_ui::type_utf8_next("\xED\xA0\x80", &cp) == 0); /* U+D800 代理 */
    COMP_CHECK(comp_ui::type_utf8_next("\xF4\x90\x80\x80", &cp) == 0); /* > U+10FFFF */
    COMP_CHECK(comp_ui::type_utf8_next(nullptr, &cp) == 0);
    /* 半个码点不动：宁可原样留着让引擎拒收，也不猜该丢哪个字节。 */
    std::string broken(1, static_cast<char>(0x80));
    COMP_CHECK(comp_ui::type_utf8_erase_last(broken) == 0 && broken.size() == 1);
  }

  /*
   * 字体轨道的预览度量：font_size 是 em 高，ascender/descender 是 em 比例。
   * 外壳没有只量的 ABI（不为此加导出），步进只能是估计值：CJK/全角 ≈1 em，
   * 其余 ≈0.55 em；advance 归 0 表示「不知道」，免得把估计值当引擎真值。
   */
  comp_ui::TypeBlockMetrics font_metrics =
      comp_ui::type_block_metrics_font(40.0, 1.25, 0.0, "\xE4\xBD\xA0\xE5\xA5\xBD", 0.8, -0.2);
  COMP_CHECK(near_value(font_metrics.width, 80.0));
  COMP_CHECK(near_value(font_metrics.height, 40.0));
  COMP_CHECK(font_metrics.line_count == 1);
  COMP_CHECK(font_metrics.max_line_glyphs == 2);
  COMP_CHECK(near_value(font_metrics.advance, 0.0));

  /* 行步进 = font_size × line_height；字距只加在码点之间，不进行尾。 */
  font_metrics = comp_ui::type_block_metrics_font(40.0, 1.5, 2.0, "AB\nA", 0.8, -0.2);
  COMP_CHECK(near_value(font_metrics.width, 2.0 * 0.55 * 40.0 + 2.0));
  COMP_CHECK(near_value(font_metrics.height, 40.0 + 40.0 * 1.5));
  COMP_CHECK(font_metrics.line_count == 2);
  COMP_CHECK(font_metrics.last_line_glyphs == 1);

  /* 空文本 / nullptr：宽 0、高一个 em（插入点光标只画竖线）。 */
  font_metrics = comp_ui::type_block_metrics_font(40.0, 1.25, 0.0, "", 0.8, -0.2);
  COMP_CHECK(near_value(font_metrics.width, 0.0));
  COMP_CHECK(near_value(font_metrics.height, 40.0));
  font_metrics = comp_ui::type_block_metrics_font(40.0, 1.25, 0.0, nullptr, 0.8, -0.2);
  COMP_CHECK(near_value(font_metrics.width, 0.0));
  COMP_CHECK(font_metrics.line_count == 1);

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
  COMP_CHECK(comp_ui::kTypePanelRowCount == 4);
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelFontSizeRow)) ==
             "Font Size");
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLineHeightRow)) ==
             "Line Height");
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelLetterSpacingRow)) ==
             "Letter Spacing");
  /* 字体行是开关不是数值：值文本由外壳填（字体名 / 「5×7 点阵」）。 */
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(comp_ui::kTypePanelFontRow)) == "Typeface");
  COMP_CHECK(comp_ui::type_panel_row_is_toggle(comp_ui::kTypePanelFontRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_toggle(comp_ui::kTypePanelFontSizeRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_toggle(comp_ui::kTypePanelLineHeightRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_toggle(comp_ui::kTypePanelLetterSpacingRow));
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(-1)).empty());
  COMP_CHECK(std::string(comp_ui::type_panel_row_label(4)).empty());
  COMP_CHECK(comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelLineHeightRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelFontSizeRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelLetterSpacingRow));
  COMP_CHECK(!comp_ui::type_panel_row_is_percent(comp_ui::kTypePanelFontRow));

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
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 161.0) == comp_ui::kTypePanelFontRow);
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 181.0) == -1); /* 第五行不存在 */
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1010.0, 99.0) == -1);  /* 标题区 */
  COMP_CHECK(comp_ui::type_param_row_at(panel, 500.0, 121.0) == -1);  /* 面板外 */
  COMP_CHECK(comp_ui::type_param_row_at(panel, 1330.0, 121.0) == -1); /* 右边界开区间 */

  const comp_ui::Rect row_rect = comp_ui::type_param_row_rect(panel, 2);
  COMP_CHECK(row_rect.x == 1000 && row_rect.y == 46 + 54 + 2 * 20 && row_rect.w == 330 &&
             row_rect.h == 20);
}

/*
 * P0-6③ 段落框：0 = 自动（点文字），从自动起跳给一个能看见折行的宽度，之后
 * 乘性增长并量化到 16 px；向下退到最小值再退一步回自动。对齐三档与
 * comp_text_align 同值。
 */
COMP_TEST(ui_core_type_paragraph_box_steps_and_alignment) {
  /* 夹取：非正值 / NaN 一律 0（自动），上限 8192。 */
  COMP_CHECK(comp_ui::clamp_type_box(0.0) == 0.0);
  COMP_CHECK(comp_ui::clamp_type_box(-40.0) == 0.0);
  COMP_CHECK(comp_ui::clamp_type_box(std::nan("")) == 0.0);
  COMP_CHECK(comp_ui::clamp_type_box(1.0) == comp_ui::kTypeBoxMin);
  COMP_CHECK(comp_ui::clamp_type_box(1e9) == comp_ui::kTypeBoxMax);

  /* 从自动向上 = 128；之后 ×1.25 并向上量化到 16。 */
  COMP_CHECK(comp_ui::step_type_box(0.0, 1) == comp_ui::kTypeBoxStart);
  COMP_CHECK(comp_ui::step_type_box(comp_ui::kTypeBoxStart, 1) == 160.0);
  COMP_CHECK(comp_ui::step_type_box(160.0, 1) == 208.0);
  /* 向下：大于起点按 ÷1.25 向下量化；到起点再退一步回自动。 */
  COMP_CHECK(comp_ui::step_type_box(160.0, -1) == 128.0);
  COMP_CHECK(comp_ui::step_type_box(comp_ui::kTypeBoxStart, -1) == 0.0);
  COMP_CHECK(comp_ui::step_type_box(0.0, -1) == 0.0);
  /* 0 方向 = 原样夹取（不制造第四种状态）。 */
  COMP_CHECK(comp_ui::step_type_box(0.0, 0) == 0.0);
  COMP_CHECK(comp_ui::step_type_box(200.0, 0) == 200.0);
  /* 上限封顶：再往上也不越过 8192。 */
  COMP_CHECK(comp_ui::step_type_box(comp_ui::kTypeBoxMax, 1) == comp_ui::kTypeBoxMax);

  /* 对齐：三档、与引擎枚举同值、循环走环。 */
  COMP_CHECK(comp_ui::kTypeAlignLeft == 0 && comp_ui::kTypeAlignCenter == 1 &&
             comp_ui::kTypeAlignRight == 2 && comp_ui::kTypeAlignCount == 3);
  COMP_CHECK(std::string(comp_ui::type_align_name(comp_ui::kTypeAlignLeft)) == "左对齐");
  COMP_CHECK(std::string(comp_ui::type_align_name(comp_ui::kTypeAlignCenter)) == "居中");
  COMP_CHECK(std::string(comp_ui::type_align_name(comp_ui::kTypeAlignRight)) == "右对齐");
  COMP_CHECK(std::string(comp_ui::type_align_name(99)) == "左对齐");
  COMP_CHECK(comp_ui::option_cycle_choice(comp_ui::kTypeAlignLeft, comp_ui::kTypeAlignCount, 1) ==
             comp_ui::kTypeAlignCenter);
  COMP_CHECK(comp_ui::option_cycle_choice(comp_ui::kTypeAlignRight, comp_ui::kTypeAlignCount, 1) ==
             comp_ui::kTypeAlignLeft);
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

/*
 * 「拖出画布」提示的判定：只看矩形与画布的相交，不关心是否真被裁剪。
 * 最容易写错的两处：① 边界刚好贴住（left == doc_w）算拖出，不能被判成「还看得见」；
 * ② 旋转后必须用 AABB 求交——不旋转时 45° 的宽高会算错整整一倍。
 */
COMP_TEST(ui_core_layer_rect_touches_canvas) {
  /* 完全在画布内 / 部分伸出：都算「还看得见」，不提示。 */
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(10, 10, 100, 100, 0.0, 1280, 720));
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(-50, -50, 100, 100, 0.0, 1280, 720));
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(1279, 719, 100, 100, 0.0, 1280, 720));

  /* 整块跑到画布外侧：四个方向都要能判出来。 */
  COMP_CHECK(!comp_ui::layer_rect_touches_canvas(1280, 0, 100, 100, 0.0, 1280, 720));
  COMP_CHECK(!comp_ui::layer_rect_touches_canvas(-100, 0, 100, 100, 0.0, 1280, 720));
  COMP_CHECK(!comp_ui::layer_rect_touches_canvas(0, 720, 100, 100, 0.0, 1280, 720));
  COMP_CHECK(!comp_ui::layer_rect_touches_canvas(0, -100, 100, 100, 0.0, 1280, 720));
  /* 贴边（右边界与画布左边界重合）也算离开：像素一个都不在画布内。 */
  COMP_CHECK(!comp_ui::layer_rect_touches_canvas(-100, 10, 100, 100, 0.0, 1280, 720));

  /* 旋转：100×100 的正方形转 45° 后 AABB 半宽 = 50*(|cos|+|sin|) ≈ 70.7。 */
  const double diag = 50.0 * (std::fabs(std::cos(3.14159265358979323846 / 4.0)) +
                              std::fabs(std::sin(3.14159265358979323846 / 4.0)));
  COMP_CHECK(near_value(diag, 50.0 * std::sqrt(2.0)));
  /* 中心在 (-60, 50)：AABB 右边界 ≈ -60 + 70.71 > 0 ⇒ 还压在画布上。 */
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(-110, 0, 100, 100, 45.0, 1280, 720));
  /* 同尺寸不旋转时右边界 = -10 ⇒ 完全在画布外。 */
  COMP_CHECK(!comp_ui::layer_rect_touches_canvas(-110, 0, 100, 100, 0.0, 1280, 720));
  /* 转 90° 后 AABB 与未旋转一致（正方形），转轴不影响判断口径。 */
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(10, 10, 100, 100, 90.0, 1280, 720));

  /* 判断不了的输入一律「当作还在画布上」：宁可少提示，也不误报拖丢。 */
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(5000, 5000, 0, 100, 0.0, 1280, 720));
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(5000, 5000, 100, 0, 0.0, 1280, 720));
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(5000, 5000, 100, 100, 0.0, 0, 720));
  COMP_CHECK(comp_ui::layer_rect_touches_canvas(
      5000, 5000, 100, 100, std::numeric_limits<double>::quiet_NaN(), 1280, 720));
}

/*
 * Move 工具「拖选区外 = 移图层」的分流判定：只有 coverage >= 阈值的点才算在选区内。
 * 最容易错的两处：① 越界/NaN 必须判成「不在选区内」（否则画布外一拖就去搬像素）；
 * ② 羽化边上按阈值切，不能让 127 这种半透明边缘算成「里面」。
 */
COMP_TEST(ui_core_selection_contains_point) {
  /* 4×3 蒙版、stride 6（带填充垃圾）：步长写错会立刻读到别的值。 */
  uint8_t mask[3 * 6];
  std::fill(mask, mask + sizeof(mask), static_cast<uint8_t>(0xCD));
  const auto put = [&](int x, int y, uint8_t value) {
    mask[static_cast<std::size_t>(y) * 6 + static_cast<std::size_t>(x)] = value;
  };
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) put(x, y, 0);
  }
  /* 一块 255 的实心区（x 1..2、y 0..1），外加羽化边上的 128 与 127。 */
  put(1, 0, 255);
  put(2, 0, 255);
  put(1, 1, 255);
  put(2, 1, 255);
  put(0, 0, 128);
  put(3, 0, 127);

  COMP_CHECK(comp_ui::selection_contains_point(mask, 4, 3, 6, 1.0, 0.0, 128));
  COMP_CHECK(comp_ui::selection_contains_point(mask, 4, 3, 6, 1.9, 1.9, 128)); /* floor */
  COMP_CHECK(comp_ui::selection_contains_point(mask, 4, 3, 6, 0.5, 0.5, 128)); /* 恰为阈值 */
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, 3.0, 0.0, 128)); /* 127 不够 */
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, 0.0, 2.0, 128)); /* 未选中 */

  /* 阈值夹取：0/负值按 1，>255 按 255。 */
  COMP_CHECK(comp_ui::selection_contains_point(mask, 4, 3, 6, 3.0, 0.0, 0));
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, 3.0, 0.0, 400));

  /* 越界与 NaN 一律「不在选区内」：画布外拖动 = 移动整层。 */
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, -0.1, 0.0, 128));
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, 0.0, -0.1, 128));
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, 4.0, 0.0, 128));
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, 3, 6, 0.0, 3.0, 128));
  COMP_CHECK(!comp_ui::selection_contains_point(
      mask, 4, 3, 6, std::numeric_limits<double>::quiet_NaN(), 0.0, 128));

  /* 判断不了的输入：空指针 / 非正尺寸返回 false，且不读内存。 */
  COMP_CHECK(!comp_ui::selection_contains_point(nullptr, 4, 3, 6, 0.0, 0.0, 128));
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 0, 3, 6, 0.0, 0.0, 128));
  COMP_CHECK(!comp_ui::selection_contains_point(mask, 4, -1, 6, 0.0, 0.0, 128));
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
                                             const comp_ui::LayerStackEntry* layers,
                                             const int32_t* display, int32_t rows, int32_t row,
                                             int32_t direction) {
  std::vector<int64_t> array = bottom_to_top;
  const int32_t count = static_cast<int32_t>(array.size());
  const int32_t index = comp_ui::layer_index_at_row(display, rows, row);
  const comp_ui::ReorderPlan plan = comp_ui::plan_layer_reorder(layers, count, index, direction);
  if (!plan.ok) return array; /* 已在端头：不产生任何变更 */

  const int64_t self = array[static_cast<size_t>(index)];
  const int64_t parent = layers[index].parent;
  int32_t last_sibling = -1;
  for (int32_t i = 0; i < count; ++i) {
    if (i == index || layers[i].parent != parent) continue;
    last_sibling = i;
  }
  int64_t before = 0;
  if (plan.before_index >= 0) {
    before = array[static_cast<size_t>(plan.before_index)];
  }
  array.erase(array.begin() + index);
  if (before == 0) {
    /* 引擎（document.cpp）：before == 0 = 插到「同级最后一个」之后；根级即数组末尾。 */
    if (parent == 0 || last_sibling < 0) {
      array.push_back(self);
    } else {
      const size_t pos = last_sibling < index ? static_cast<size_t>(last_sibling + 1)
                                              : static_cast<size_t>(last_sibling);
      array.insert(array.begin() + pos, self);
    }
  } else {
    auto pos = std::find(array.begin(), array.end(), before);
    array.insert(pos, self);
  }
  return array;
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
        std::vector<comp_ui::LayerStackEntry> entries;
        for (int64_t id : bottom) entries.push_back(comp_ui::LayerStackEntry{id, 0, 0});
        /* 扁平文档：显示序仍是数组逆序，行 -> 下标 = count-1-row。 */
        int32_t display[3] = {0, 0, 0};
        for (int32_t i = 0; i < count; ++i) display[i] = count - 1 - i;

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

        const std::vector<int64_t> actual = panel_order_of(
            simulate_engine_reorder(bottom, entries.data(), display, count, shown, direction));
        COMP_CHECK(actual == expected);

        const comp_ui::ReorderPlan plan = comp_ui::plan_layer_reorder(
            entries.data(), count, count - 1 - shown, direction);
        if (target == shown) {
          COMP_CHECK(!plan.ok); /* 端头必须如实拒绝，不静默无操作 */
        } else {
          COMP_CHECK(plan.ok);
        }
      }
    }
  }

  /* 空文档 / 空表不产生计划。 */
  COMP_CHECK(!comp_ui::plan_layer_reorder(nullptr, 0, 0, -1).ok);
  COMP_CHECK(!comp_ui::plan_layer_reorder(nullptr, 0, 0, 0).ok);

  /* 越界下标不夹取：树形面板里「行 → 下标」不再单调，猜一个比拒绝更糟。 */
  const comp_ui::LayerStackEntry flat[3] = {{10, 0, 0}, {11, 0, 0}, {12, 0, 0}};
  COMP_CHECK(!comp_ui::plan_layer_reorder(flat, 3, 99, -1).ok);
  COMP_CHECK(!comp_ui::plan_layer_reorder(flat, 3, -1, -1).ok);
  COMP_CHECK(!comp_ui::plan_layer_reorder(flat, 3, 0, 7).ok);
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
                                         const comp_ui::DragPlan& plan,
                                         const comp_ui::LayerStackEntry* layers,
                                         const int32_t* display, int32_t rows, int32_t from_row) {
  if (!plan.ok()) return array;
  const int32_t from_index = comp_ui::layer_index_at_row(display, rows, from_row);
  if (from_index < 0 || from_index >= static_cast<int32_t>(array.size())) return array;
  const int64_t dragged = array[static_cast<size_t>(from_index)];
  const int64_t parent = layers[from_index].parent;
  const int32_t count = static_cast<int32_t>(array.size());
  int32_t last_sibling = -1;
  for (int32_t i = 0; i < count; ++i) {
    if (i == from_index || layers[i].parent != parent) continue;
    last_sibling = i;
  }
  const int64_t anchor =
      plan.before_index < 0 ? 0 : array[static_cast<size_t>(plan.before_index)];
  std::vector<int64_t> result = array;
  result.erase(result.begin() + from_index);
  if (anchor == 0) {
    /* comp_layer_reorder 的 before == 0 = 同级末尾；根级即数组末尾。 */
    if (parent == 0 || last_sibling < 0) {
      result.push_back(dragged);
    } else {
      const size_t pos = last_sibling < from_index ? static_cast<size_t>(last_sibling + 1)
                                                   : static_cast<size_t>(last_sibling);
      result.insert(result.begin() + pos, dragged);
    }
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
 * 菜单可用态（S41）。
 *
 * 与勾选态同一套路：Win32 的 EnableMenuItem 不可测，可测的是「什么前提下这
 * 一项有动作」。三条最容易被写错的规则在这里钉住：
 *  1. 无层时「需要选中层」的整组项全灭，但撤销/重做只跟历史走（无层也能撤销
 *     删除操作的历史步——那一步本身才把层删掉的）；
 *  2. 组图层的效果项只有多选时才可用（批量会跳过组）；
 *  3. 端头重排看 can_move_up/can_move_down，而不是「有层就点亮」。
 */
COMP_TEST(ui_core_layer_menu_enabled_follows_preconditions) {
  /* 1. 空状态：一切需要层/选区的项都不可用。 */
  comp_ui::LayerMenuState state;
  comp_ui::LayerMenuEnabled enabled = comp_ui::layer_menu_enabled(state);
  COMP_CHECK(!enabled.undo && !enabled.redo);
  COMP_CHECK(!enabled.layer_present && !enabled.layer_merge_down && !enabled.layer_flatten);
  COMP_CHECK(!enabled.layer_step && !enabled.move_up && !enabled.move_down);
  COMP_CHECK(!enabled.effect_toggle && !enabled.effect_edit && !enabled.effect_clear);
  COMP_CHECK(!enabled.mask_from_selection && !enabled.mask_to_selection && !enabled.mask_remove);
  COMP_CHECK(!enabled.clipping && !enabled.isolated);
  COMP_CHECK(!enabled.selection_modify && !enabled.selection_load_pixels);

  /* 撤销不依赖选中层：删掉最后一层之后仍然要能撤销回去。 */
  state.can_undo = true;
  enabled = comp_ui::layer_menu_enabled(state);
  COMP_CHECK(enabled.undo);
  COMP_CHECK(!enabled.redo && !enabled.layer_present);

  /* 2. 普通图层：属性项全开；蒙版/选区项各看自己的前置；单层不能拼合/换层。 */
  state = comp_ui::LayerMenuState{};
  state.has_layer = true;
  state.layer_count = 1;
  state.can_move_up = false; /* 单层：上下都是端头 */
  state.can_move_down = false;
  enabled = comp_ui::layer_menu_enabled(state);
  COMP_CHECK(enabled.layer_present);
  COMP_CHECK(!enabled.layer_merge_down);
  COMP_CHECK_MSG(!enabled.layer_flatten, "只有一层时拼合是空操作，应置灰");
  COMP_CHECK(!enabled.layer_step);
  COMP_CHECK(!enabled.move_up && !enabled.move_down);
  COMP_CHECK(enabled.effect_toggle && enabled.effect_edit);
  COMP_CHECK(!enabled.effect_clear);
  COMP_CHECK(!enabled.mask_to_selection && !enabled.mask_remove);
  COMP_CHECK(!enabled.mask_from_selection); /* 无选区 */
  COMP_CHECK(!enabled.clipping && !enabled.isolated);
  COMP_CHECK(enabled.selection_load_pixels);

  state.has_selection = true;
  state.has_mask = true;
  state.effect_count = 2;
  state.clipping_available = true;
  state.can_merge_down = true;
  state.can_move_up = false; /* 顶层：只能下移 */
  state.can_move_down = true;
  enabled = comp_ui::layer_menu_enabled(state);
  COMP_CHECK(enabled.mask_from_selection);
  COMP_CHECK(enabled.mask_to_selection);
  COMP_CHECK(enabled.mask_remove);
  COMP_CHECK(enabled.clipping);
  COMP_CHECK(enabled.effect_clear);
  COMP_CHECK(enabled.layer_merge_down);
  COMP_CHECK(enabled.selection_modify);
  COMP_CHECK(enabled.move_down && !enabled.move_up);
  COMP_CHECK_MSG(!enabled.isolated, "非组的 isolated 必须保持置灰（引擎忽略）");

  /* 3. 组图层：效果项默认灭；多选时可用（批量作用于组外成员）；隔离开。 */
  state = comp_ui::LayerMenuState{};
  state.has_layer = true;
  state.is_group = true;
  state.layer_count = 3;
  enabled = comp_ui::layer_menu_enabled(state);
  COMP_CHECK_MSG(!enabled.effect_toggle, "组图层的效果不参与求值，单选的档位应置灰");
  COMP_CHECK(!enabled.effect_edit);
  COMP_CHECK(enabled.isolated);

  state.multi_selected = true;
  enabled = comp_ui::layer_menu_enabled(state);
  COMP_CHECK_MSG(enabled.effect_toggle, "多选时批量会跳过组、作用于其余成员，档位应可用");
}

/*
 * P0-6 ①：栅格化文字只在「选中层确实是可编辑文字图层」时可点。
 *
 * 引擎对普通像素层的 comp_layer_rasterize_text 是幂等空操作（返回 OK、不记撤销
 * 步），所以不置灰不会丢数据，但会让菜单承诺一件不会发生的事——按 S41 的口径
 * （只有如实拒绝或纯空操作的项才置灰），它正是该置灰的一类。
 */
COMP_TEST(ui_core_layer_menu_rasterize_text_needs_text_layer) {
  comp_ui::LayerMenuState state;
  COMP_CHECK(!comp_ui::layer_menu_enabled(state).layer_rasterize_text);

  /* 有层但不是文字层：普通像素层上「栅格化文字」没有动作。 */
  state.has_layer = true;
  COMP_CHECK(!comp_ui::layer_menu_enabled(state).layer_rasterize_text);

  /* 文字图层：可用。 */
  state.is_text = true;
  COMP_CHECK(comp_ui::layer_menu_enabled(state).layer_rasterize_text);

  /* 没有选中层时即便 is_text 残留为真也不能亮（状态复位由调用方负责，规则兜底）。 */
  state.has_layer = false;
  COMP_CHECK(!comp_ui::layer_menu_enabled(state).layer_rasterize_text);
}

/*
 * 效果档位表（S24）。
 *
 * 数值 0..9 是 .comp 的 engineEffects[].effect，顺序必须与引擎落笔判定顺序
 * （effect_eval.cpp 的 kEffectStroke..kEffectPatternOverlay）逐字一致：菜单 id 用
 * kCmdEffectBase + kind 拼出来，表错一位就是「点描边结果加了投影」。
 */
COMP_TEST(ui_core_effect_kind_table_matches_engine_order) {
  const char* expected[] = {"Stroke",       "Drop Shadow",     "Color Overlay",
                            "Inner Shadow", "Outer Glow",      "Inner Glow",
                            "Bevel & Emboss", "Satin",         "Gradient Overlay",
                            "Pattern Overlay"};
  COMP_CHECK(comp_ui::kEffectKindCount == 10);
  for (int kind = 0; kind < comp_ui::kEffectKindCount; ++kind) {
    COMP_CHECK_MSG(std::string(comp_ui::effect_kind_name(kind)) == expected[kind],
                   "效果档位顺序必须与引擎一致");
  }

  /* 越界夹取：负 -> 首项，超界 -> 末项（菜单 id 永不指向不存在的档位）。 */
  COMP_CHECK(comp_ui::clamp_effect_kind(-3) == 0);
  COMP_CHECK(comp_ui::clamp_effect_kind(comp_ui::kEffectKindCount) ==
             comp_ui::kEffectKindCount - 1);
  COMP_CHECK(std::string(comp_ui::effect_kind_name(-1)) == "Stroke");
  COMP_CHECK(std::string(comp_ui::effect_kind_name(99)) == "Pattern Overlay");

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
  /* 扁平文档的显示序：行 -> 数组下标，逆序。 */
  const int32_t display[5] = {4, 3, 2, 1, 0};

  for (int32_t from = 0; from < count; ++from) {
    for (int32_t to = 0; to < count; ++to) {
      const comp_ui::DragPlan plan =
          comp_ui::plan_layer_drag(same_parent, count, display, count, from, to);
      if (from == to) {
        COMP_CHECK_MSG(!plan.ok(), "落点即原行：不得产生写入");
        COMP_CHECK(plan.target == comp_ui::DragTarget::kSameRow);
        COMP_CHECK(plan.before_index == -1);
        COMP_CHECK(plan.next_row == from);
        continue;
      }
      COMP_CHECK(plan.ok());
      const int64_t dragged = array[static_cast<size_t>(display[from])];
      const std::vector<int64_t> after =
          simulate_layer_drag(array, plan, same_parent, display, count, from);
      COMP_CHECK_MSG(shown_row_of(after, dragged) == to, "落点行必须是 to_row");
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
  comp_ui::DragPlan plan = comp_ui::plan_layer_drag(same_parent, count, display, count, 4, 0);
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.before_index == -1);
  COMP_CHECK(plan.next_row == 0);

  /* 跨父级：如实拒绝，next_row 停原位（不回 0、不乱跳）。 */
  const comp_ui::LayerStackEntry mixed[4] = {
      {201, 0, 0}, {202, 0, 0}, {203, 77, 0}, {204, 77, 0}};
  const int32_t mixed_display[4] = {3, 2, 1, 0};
  /* 面板行 0 = 204(77)、1 = 203(77)、2 = 202(根)、3 = 201(根)。 */
  plan = comp_ui::plan_layer_drag(mixed, 4, mixed_display, 4, 0, 2);
  COMP_CHECK(!plan.ok());
  COMP_CHECK(plan.target == comp_ui::DragTarget::kCrossParent);
  COMP_CHECK(plan.next_row == 0);
  plan = comp_ui::plan_layer_drag(mixed, 4, mixed_display, 4, 0, 1); /* 同组内 204 -> 203 上面 */
  COMP_CHECK(plan.ok());
  plan = comp_ui::plan_layer_drag(mixed, 4, mixed_display, 4, 2, 3); /* 根级相邻 */
  COMP_CHECK(plan.ok());

  /* 非法输入：空指针 / 空表 / 越界行号一律 kInvalid，不得越界读。 */
  COMP_CHECK(!comp_ui::plan_layer_drag(nullptr, 4, display, 4, 0, 1).ok());
  COMP_CHECK(comp_ui::plan_layer_drag(nullptr, 4, display, 4, 0, 1).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, 0, display, 0, 0, 0).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, nullptr, count, 0, 1).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, display, count, -1, 0).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, display, count, 0, -1).target ==
             comp_ui::DragTarget::kInvalid);
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, display, count, 0, count).target ==
             comp_ui::DragTarget::kInvalid);
  /* display 内容越界（被折叠的行数比显示行数少时可能出现）也必须拒绝。 */
  const int32_t bogus[5] = {4, 3, 2, 1, 99};
  COMP_CHECK(comp_ui::plan_layer_drag(same_parent, count, bogus, count, 0, 4).target ==
             comp_ui::DragTarget::kInvalid);
}

/*
 * 树形显示序（S44）。
 *
 * 旧面板直接按「数组逆序」画，于是组行总在它的子层**下面**（引擎把子层插在
 * 组自身下标 +1，逆序后跑到上面），与上游 PS 相反；用户看到的是「子层挂在组
 * 下面那层」而不是「组展开的成员」。这里钉死三件事：
 *   1. 组行先于它的子层，子层按数组逆序（顶层先）排在组行下方，缩进 +1；
 *   2. 折叠的组整支不显示（孙层也不显示），别的组不受影响；
 *   3. 父 id 不存在的孤儿 / 成环的层仍要出现在面板上（补到根级），绝不静默消失。
 */
COMP_TEST(ui_core_layer_tree_display_puts_group_above_children) {
  /* 数组自下而上：301 | 302(组) 303 304 | 305，其中 303/304 属于 302。 */
  const comp_ui::LayerStackEntry layers[5] = {
      {301, 0, 0}, {302, 0, 0}, {303, 302, 0}, {304, 302, 0}, {305, 0, 0}};
  int32_t display[8] = {0};
  int32_t depth[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

  int32_t rows = comp_ui::build_layer_display(layers, 5, nullptr, display, depth, 8);
  COMP_CHECK(rows == 5);
  const int32_t want_display[5] = {4, 1, 3, 2, 0}; /* 305 | 302 | 304 303 | 301 */
  const int32_t want_depth[5] = {0, 0, 1, 1, 0};
  for (int32_t i = 0; i < 5; ++i) {
    COMP_CHECK(display[i] == want_display[i]);
    COMP_CHECK(depth[i] == want_depth[i]);
  }

  /* 折叠 302：组行保留，两个子层消失，别的层不受影响。 */
  uint8_t collapsed[5] = {0, 1, 0, 0, 0};
  rows = comp_ui::build_layer_display(layers, 5, collapsed, display, depth, 8);
  COMP_CHECK(rows == 3);
  COMP_CHECK(display[0] == 4 && display[1] == 1 && display[2] == 0);
  COMP_CHECK(comp_ui::layer_row_of_index(display, rows, 2) == -1); /* 303 被折叠 */
  COMP_CHECK(comp_ui::layer_row_of_index(display, rows, 3) == -1); /* 304 被折叠 */
  COMP_CHECK(comp_ui::layer_row_of_index(display, rows, 4) == 0);
  COMP_CHECK(comp_ui::layer_index_at_row(display, rows, 1) == 1);
  COMP_CHECK(comp_ui::layer_index_at_row(display, rows, rows) == -1);

  /* 缩进有上限：极深的层不再右移，但行本身仍在（折叠三角照样能点）。 */
  COMP_CHECK(comp_ui::layer_row_indent(0) == 0);
  COMP_CHECK(comp_ui::layer_row_indent(2) == 2 * comp_ui::kLayerIndentStep);
  COMP_CHECK(comp_ui::layer_row_indent(99) == comp_ui::kLayerIndentLimit);
  const Rect row{10, 100, 248, comp_ui::kLayerRowHeight};
  const Rect tri0 = comp_ui::layer_triangle_rect(row, 0);
  const Rect tri2 = comp_ui::layer_triangle_rect(row, 2);
  COMP_CHECK(tri2.x - tri0.x == 2 * comp_ui::kLayerIndentStep);
  COMP_CHECK(tri0.y >= row.y && tri0.bottom() <= row.bottom());

  /* 孤儿 / 环：父 id 不在表里、或互相指成环，都必须补到根级显示出来。 */
  const comp_ui::LayerStackEntry broken[4] = {
      {401, 0, 0}, {402, 999, 0}, {403, 404, 0}, {404, 403, 0}};
  rows = comp_ui::build_layer_display(broken, 4, nullptr, display, depth, 8);
  COMP_CHECK(rows == 4);
  COMP_CHECK(display[0] == 0); /* 根级 401 第一行 */
  /* 剩下三个按数组逆序补：404、403、402。成环的两个会互相「嵌套」一层，
     这是数据损坏时的显示形态；这里只要求存在、不要求层级正确。 */
  COMP_CHECK(display[1] == 3 && display[2] == 2 && display[3] == 1);
  COMP_CHECK(depth[0] == 0);
  for (int32_t i = 0; i < 4; ++i) COMP_CHECK(depth[i] >= 0 && depth[i] < 4);

  /* 容量不足按容量截断，不写越界。 */
  int32_t small_display[2] = {-1, -1};
  int32_t small_depth[2] = {-1, -1};
  rows = comp_ui::build_layer_display(layers, 5, nullptr, small_display, small_depth, 2);
  COMP_CHECK(rows == 2);
  COMP_CHECK(small_display[0] == 4 && small_display[1] == 1);

  /* 空输入一律 0 行。 */
  COMP_CHECK(comp_ui::build_layer_display(nullptr, 5, nullptr, display, depth, 8) == 0);
  COMP_CHECK(comp_ui::build_layer_display(layers, 0, nullptr, display, depth, 8) == 0);
  COMP_CHECK(comp_ui::build_layer_display(layers, 5, nullptr, nullptr, depth, 8) == 0);
  COMP_CHECK(comp_ui::build_layer_display(layers, 5, nullptr, display, depth, 0) == 0);
}

/*
 * 同级重排（S44）：菜单的上移 / 下移只换**同一父级里相邻的兄弟**。
 *
 * 旧实现按数组相邻取锚点，树形文档里「组内层上移一格」会跨进组外的层——引擎的
 * 跨父校验会拒绝，用户看到的是「菜单点了没反应」；更糟的是有时恰好同父，于是
 * 子层一口气跳过整个别组，落点完全不对。
 */
COMP_TEST(ui_core_layer_tree_reorder_stays_within_siblings) {
  /* 301 | 302(组) 303 304 | 305 */
  const comp_ui::LayerStackEntry layers[5] = {
      {301, 0, 0}, {302, 0, 0}, {303, 302, 0}, {304, 302, 0}, {305, 0, 0}};

  /* 根级兄弟序（自下而上）= 301、302、305；301 上移一格应越过 302（不能只挪下标）。 */
  comp_ui::ReorderPlan plan = comp_ui::plan_layer_reorder(layers, 5, 0, -1);
  COMP_CHECK(plan.ok);
  COMP_CHECK(plan.before_index == 4); /* 锚点 305 */

  /* 301 置顶：before = 0（同级末尾），不是数组末尾的下标。 */
  plan = comp_ui::plan_layer_reorder(layers, 5, 0, -2);
  COMP_CHECK(plan.ok);
  COMP_CHECK(plan.before_index == -1);

  /* 302 下移：根级兄弟里 302 上面没有别的根级层？有 305，所以目标锚点是 301。 */
  plan = comp_ui::plan_layer_reorder(layers, 5, 1, 1);
  COMP_CHECK(plan.ok);
  COMP_CHECK(plan.before_index == 0);

  /* 组内 303 上移一格：只在 303/304 之间换，锚点绝不允许是根级的 302 或 305。 */
  plan = comp_ui::plan_layer_reorder(layers, 5, 2, -1);
  COMP_CHECK(plan.ok);
  COMP_CHECK(plan.before_index == -1); /* 304 之上没有兄弟了，落到组内末尾 */
  COMP_CHECK(comp_ui::plan_layer_reorder(layers, 5, 3, -1).ok == false); /* 304 已是组内顶层 */
  COMP_CHECK(comp_ui::plan_layer_reorder(layers, 5, 3, -2).ok == false);
  /* 304 下移一格：锚点是 303。 */
  plan = comp_ui::plan_layer_reorder(layers, 5, 3, 1);
  COMP_CHECK(plan.ok);
  COMP_CHECK(plan.before_index == 2);
  /* 304 置底：锚点是组内最低的 303。 */
  plan = comp_ui::plan_layer_reorder(layers, 5, 3, 0);
  COMP_CHECK(plan.ok);
  COMP_CHECK(plan.before_index == 2);

  /* 用引擎语义复算「303 上移」：它的落点必须在 304 之上、仍在 302 的子树里。 */
  const std::vector<int64_t> array = {301, 302, 303, 304, 305};
  const int32_t flat_display[5] = {4, 3, 2, 1, 0}; /* 行 2 = 下标 2 = 303 */
  const std::vector<int64_t> after =
      simulate_engine_reorder(array, layers, flat_display, 5, 2, -1);
  COMP_CHECK(after.size() == 5);
  auto pos_of = [&](int64_t id) {
    return static_cast<int32_t>(std::find(after.begin(), after.end(), id) - after.begin());
  };
  COMP_CHECK(pos_of(302) + 1 == pos_of(304)); /* 子树仍紧贴组行 */
  COMP_CHECK(pos_of(304) + 1 == pos_of(303)); /* 303 换到 304 之上 */
}

/*
 * 树形拖拽（S44）：落点行先经 display[] 换成数组下标，再按**兄弟序号**取锚点。
 *
 * 关键场景：两个根级兄弟之间夹着已展开的组，行号不相邻。旧实现拿行号直接做
 * count-1-row 换算，会把「拖到另一个根级层下面」算成组内的位置。
 */
COMP_TEST(ui_core_layer_tree_drag_uses_sibling_ordinals) {
  /* 数组：401 | 402(组) 403 404 | 405；显示：405 402 404 403 401 */
  const comp_ui::LayerStackEntry layers[5] = {
      {401, 0, 0}, {402, 0, 0}, {403, 402, 0}, {404, 402, 0}, {405, 0, 0}};
  int32_t display[8] = {0};
  int32_t depth[8] = {0};
  const int32_t rows = comp_ui::build_layer_display(layers, 5, nullptr, display, depth, 8);
  COMP_CHECK(rows == 5);
  COMP_CHECK(display[0] == 4 && display[1] == 1 && display[2] == 3 && display[3] == 2 &&
             display[4] == 0);

  /* 把 402（行 1）拖到根级最底（行 4）：ps=1、ts=0，锚点 = 401。 */
  comp_ui::DragPlan plan = comp_ui::plan_layer_drag(layers, 5, display, rows, 1, 4);
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.before_index == 0);
  std::vector<int64_t> array = {401, 402, 403, 404, 405};
  std::vector<int64_t> after = simulate_layer_drag(array, plan, layers, display, rows, 1);
  COMP_CHECK(after[0] == 402 && after[1] == 401);
  /* 组内的 403/404 必须仍然紧跟 402（子树整体跟着组走）。 */
  COMP_CHECK(std::find(after.begin(), after.end(), 403) - after.begin() == 2);
  COMP_CHECK(std::find(after.begin(), after.end(), 404) - after.begin() == 3);

  /* 组内 403（行 3）拖到 404（行 2）之上：同父，锚点 = 组内末尾（before = -1）。 */
  plan = comp_ui::plan_layer_drag(layers, 5, display, rows, 3, 2);
  COMP_CHECK(plan.ok());
  COMP_CHECK(plan.before_index == -1);
  after = simulate_layer_drag(array, plan, layers, display, rows, 3);
  COMP_CHECK(std::find(after.begin(), after.end(), 403) - after.begin() == 3);

  /* 组内层拖到根级层上 / 根级层拖进组内：跨父，如实拒绝。 */
  COMP_CHECK(comp_ui::plan_layer_drag(layers, 5, display, rows, 3, 4).target ==
             comp_ui::DragTarget::kCrossParent);
  COMP_CHECK(comp_ui::plan_layer_drag(layers, 5, display, rows, 0, 2).target ==
             comp_ui::DragTarget::kCrossParent);

  /* 折叠后行号变短，跨父判定也不能错位（折叠时组内层根本不在显示序里）。 */
  uint8_t collapsed[5] = {0, 1, 0, 0, 0};
  const int32_t folded_rows =
      comp_ui::build_layer_display(layers, 5, collapsed, display, depth, 8);
  COMP_CHECK(folded_rows == 3);
  COMP_CHECK(comp_ui::plan_layer_drag(layers, 5, display, folded_rows, 0, 2).ok());
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

/*
 * 变换手柄（S39）。
 *
 * 只钉可判定的几何，不碰 Win32：
 *   1. 8 个手柄绕矩形中心转，顺序与绘制一致（左上起顺时针），旋钮在上边外侧。
 *   2. 命中取半径内最近的手柄，空白处 / 空矩形 / 非法半径返回「无」。
 *   3. 拖手柄缩放时**对角锚点不动**——0° 与 90° 都要成立；最小尺寸把指针
 *      越过对边的情况夹住，而不是让宽高变成 0 或负数（引擎会拒绝这种 transform）。
 *   4. 旋钮角度取夹角增量并归一到 (-180, 180]，跨 ±180° 不跳圈。
 */
COMP_TEST(ui_core_transform_handles_track_rotation_and_anchor) {
  const Rect base{100, 200, 400, 200}; /* 中心 (300, 300) */
  const Point center = comp_ui::rect_center_point(base);

  /* 1. 0° 时的手柄位置。 */
  const Point expected[8] = {Point{100, 200}, Point{300, 200}, Point{500, 200},
                             Point{500, 300}, Point{500, 400}, Point{300, 400},
                             Point{100, 400}, Point{100, 300}};
  for (int handle = 0; handle < comp_ui::kTransformHandleCount; ++handle) {
    COMP_CHECK(same_point(comp_ui::transform_handle_point(base, 0.0, handle),
                          expected[handle]));
  }
  /* 90°：左上角 (-200,-100) 转到 (100,-200)，即 (400, 100)。 */
  COMP_CHECK(same_point(comp_ui::transform_handle_point(base, 90.0, 0),
                        Point{400, 100}));
  COMP_CHECK(same_point(comp_ui::transform_handle_point(base, 90.0, 4),
                        Point{200, 500}));

  /* 2. 命中。 */
  COMP_CHECK(comp_ui::transform_handle_at(base, 0.0, Point{500, 300}, 6.0) == 3);
  COMP_CHECK(comp_ui::transform_handle_at(base, 0.0, Point{503, 297}, 6.0) == 3);
  COMP_CHECK(comp_ui::transform_handle_at(base, 0.0, Point{340, 300}, 6.0) ==
             comp_ui::kTransformHandleNone);
  COMP_CHECK(comp_ui::transform_handle_at(base, 0.0, Point{500, 300}, 0.0) ==
             comp_ui::kTransformHandleNone);
  COMP_CHECK(comp_ui::transform_handle_at(Rect{}, 0.0, Point{0, 0}, 6.0) ==
             comp_ui::kTransformHandleNone);
  /* 旋转 45° 后手柄转到别处：命中按旋转后的点算，原角落不再命中。 */
  const Point rotated_br = comp_ui::transform_handle_point(base, 45.0, 4);
  COMP_CHECK(comp_ui::transform_handle_at(base, 45.0, rotated_br, 6.0) == 4);
  COMP_CHECK(comp_ui::transform_handle_at(base, 45.0, Point{500, 400}, 6.0) != 4);

  /* 旋钮：上边中点外 offset。 */
  COMP_CHECK(same_point(comp_ui::transform_rotate_knob_point(base, 0.0, 24.0),
                        Point{300, 176}));
  COMP_CHECK(same_point(comp_ui::transform_rotate_knob_point(base, 90.0, 24.0),
                        Point{424, 300}));
  COMP_CHECK(comp_ui::transform_knob_at(base, 0.0, Point{301, 175}, 24.0, 6.0));
  COMP_CHECK(!comp_ui::transform_knob_at(base, 0.0, Point{300, 200}, 24.0, 6.0));

  /* 3. 缩放：0° 下拖各边，锚点侧的边不动。 */
  const Rect right_drag =
      comp_ui::transform_scale_rect(base, 0.0, 3, Point{600, 300}, 1);
  COMP_CHECK(right_drag.x == 100 && right_drag.y == 200 && right_drag.w == 500 &&
             right_drag.h == 200);
  const Rect left_drag =
      comp_ui::transform_scale_rect(base, 0.0, 7, Point{0, 300}, 1);
  COMP_CHECK(left_drag.x == 0 && left_drag.w == 500 && left_drag.h == 200);
  const Rect bottom_drag =
      comp_ui::transform_scale_rect(base, 0.0, 5, Point{300, 500}, 1);
  COMP_CHECK(bottom_drag.y == 200 && bottom_drag.h == 300 && bottom_drag.w == 400);
  const Rect corner_drag =
      comp_ui::transform_scale_rect(base, 0.0, 0, Point{50, 150}, 1);
  COMP_CHECK(corner_drag.x == 50 && corner_drag.y == 150 && corner_drag.w == 450 &&
             corner_drag.h == 250);
  /* 指针越过对边：宽高压到下限，不出现 0 / 负数。 */
  const Rect clamped =
      comp_ui::transform_scale_rect(base, 0.0, 3, Point{50, 300}, 8);
  COMP_CHECK(clamped.x == 100 && clamped.w == 8);
  /* 非法手柄 / 空矩形原样返回。 */
  const Rect untouched =
      comp_ui::transform_scale_rect(base, 0.0, 9, Point{600, 300}, 1);
  COMP_CHECK(untouched.x == base.x && untouched.w == base.w);

  /*
   * 90° 下拖右边：局部 x 从 500 拉到 600，中心随之前移，锚点（局部左边中点）
   * 在文档里必须仍在 (300, 100)。结果 {50, 250, 500, 200}。
   */
  const Rect rotated_drag =
      comp_ui::transform_scale_rect(base, 90.0, 3, Point{300, 600}, 1);
  COMP_CHECK(rotated_drag.x == 50 && rotated_drag.y == 250 && rotated_drag.w == 500 &&
             rotated_drag.h == 200);
  const Point anchor_after = comp_ui::rotate_doc_point(
      Point{static_cast<double>(rotated_drag.x), comp_ui::rect_center_point(rotated_drag).y},
      comp_ui::rect_center_point(rotated_drag), 90.0);
  COMP_CHECK(same_point(anchor_after, Point{300, 100}, 1.5));

  /* 4. 旋钮角度：上 -> 右 = +90°；上 -> 左跨过 180° 后归到 -90°。 */
  COMP_CHECK(near_value(comp_ui::transform_rotate_delta(base, Point{300, 200},
                                                        Point{400, 300}), 90.0));
  COMP_CHECK(near_value(comp_ui::transform_rotate_delta(base, Point{300, 200},
                                                        Point{200, 300}), -90.0));
  COMP_CHECK(near_value(comp_ui::transform_rotate_delta(base, Point{300, 200},
                                                        Point{300, 200}), 0.0));
  COMP_CHECK(near_value(comp_ui::transform_normalize_rotation(400.0), 40.0));
  COMP_CHECK(near_value(comp_ui::transform_normalize_rotation(-400.0), -40.0));
  COMP_CHECK(near_value(comp_ui::transform_normalize_rotation(360.0), 360.0));
  COMP_CHECK(near_value(comp_ui::transform_normalize_rotation(-360.0), 0.0));
  /* 中心与实际手柄中心一致（旋钮角度以它为原点）。 */
  COMP_CHECK(same_point(center, Point{300, 300}));
}

/*
 * S43：Shift 拖角 = 等比缩放。比例锁在按下时的 base 上（不是上一帧的中间结果），
 * 对角锚点语义与自由缩放完全一致；边手柄忽略 Shift（没有中心锚点这条路径）。
 */
COMP_TEST(ui_core_transform_uniform_scale_locks_aspect) {
  const Rect base{100, 200, 400, 200}; /* 2:1 */
  /* 1. 左上角：宽度 450/400=1.125、高度 250/200=1.25，主方向是高度，取 1.25。 */
  const Rect top_left = comp_ui::transform_scale_rect(base, 0.0, 0, Point{50, 150}, 1, true);
  COMP_CHECK(top_left.x == 0 && top_left.y == 150 && top_left.w == 500 && top_left.h == 250);
  COMP_CHECK(top_left.right() == 500 && top_left.bottom() == 400); /* 右下角不动 */
  /* 2. 同一指针下自由缩放两个方向各走各的，比例被破坏——这正是要修的行为。 */
  const Rect free = comp_ui::transform_scale_rect(base, 0.0, 0, Point{50, 150}, 1);
  COMP_CHECK(free.w == 450 && free.h == 250);
  /* 3. 右下角：钉住左上角，1.5 倍。 */
  const Rect bottom_right =
      comp_ui::transform_scale_rect(base, 0.0, 4, Point{600, 500}, 1, true);
  COMP_CHECK(bottom_right.x == 100 && bottom_right.y == 200 && bottom_right.w == 600 &&
             bottom_right.h == 300);
  /* 4. 边手柄忽略 Shift：结果必须与自由缩放逐字段相同。 */
  const Rect edge_free = comp_ui::transform_scale_rect(base, 0.0, 3, Point{600, 300}, 1);
  const Rect edge_shift =
      comp_ui::transform_scale_rect(base, 0.0, 3, Point{600, 300}, 1, true);
  COMP_CHECK(edge_shift.x == edge_free.x && edge_shift.y == edge_free.y &&
             edge_shift.w == edge_free.w && edge_shift.h == edge_free.h);
  /* 5. 拖到 0 也不破坏比例：两边都压到下限之上（8 高 -> 16 宽）。 */
  const Rect minimal =
      comp_ui::transform_scale_rect(base, 0.0, 4, Point{100, 200}, 8, true);
  COMP_CHECK(minimal.w == 16 && minimal.h == 8);
  /* 6. 旋转 90°：比例仍锁住，局部左上角（锚点）在文档里的位置不动。 */
  const Rect rotated = comp_ui::transform_scale_rect(base, 90.0, 4, Point{300, 600}, 1, true);
  COMP_CHECK(rotated.w == 500 && rotated.h == 250);
  const Point anchor_before = comp_ui::rotate_doc_point(
      Point{100, 200}, comp_ui::rect_center_point(base), 90.0);
  const Point anchor_after = comp_ui::rotate_doc_point(
      Point{static_cast<double>(rotated.x), static_cast<double>(rotated.y)},
      comp_ui::rect_center_point(rotated), 90.0);
  COMP_CHECK(same_point(anchor_after, anchor_before, 1.5));
}

/* --------------------------------------------------- 选区手柄（S46） */

COMP_TEST(ui_core_selection_handle_points_and_hit) {
  const Rect bounds{10, 20, 100, 50};

  /* 1. 八个锚点：四角 + 四边中点；右/下边落在 x+w / y+h 上（像素网格口径）。 */
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 0), Point{10, 20}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 1), Point{60, 20}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 2), Point{110, 20}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 3), Point{110, 45}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 4), Point{110, 70}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 5), Point{60, 70}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 6), Point{10, 70}));
  COMP_CHECK(same_point(comp_ui::selection_handle_point(bounds, 7), Point{10, 45}));

  /* 2. 命中：容差内取最近的一个，容差外返回 -1。 */
  Point centers[comp_ui::kSelectionHandleCount];
  for (int i = 0; i < comp_ui::kSelectionHandleCount; ++i) {
    centers[i] = comp_ui::selection_handle_point(bounds, i);
  }
  COMP_CHECK(comp_ui::selection_handle_hit(centers, comp_ui::kSelectionHandleCount,
                                           Point{13, 23}, 6.0) == 0);
  COMP_CHECK(comp_ui::selection_handle_hit(centers, comp_ui::kSelectionHandleCount,
                                           Point{113, 46}, 6.0) == 3);
  COMP_CHECK(comp_ui::selection_handle_hit(centers, comp_ui::kSelectionHandleCount,
                                           Point{60, 68}, 6.0) == 5);
  COMP_CHECK(comp_ui::selection_handle_hit(centers, comp_ui::kSelectionHandleCount,
                                           Point{61, 45}, 6.0) == -1);

  /* 3. 精确落在两个手柄中点时按「距离相等取后者」的确定性结果，不随编译器。 */
  COMP_CHECK(comp_ui::selection_handle_hit(centers, comp_ui::kSelectionHandleCount,
                                           Point{35, 20}, 40.0) == 1);
  COMP_CHECK(comp_ui::selection_handle_hit(centers, comp_ui::kSelectionHandleCount,
                                           Point{std::numeric_limits<double>::quiet_NaN(), 20},
                                           6.0) == -1);
}

COMP_TEST(ui_core_selection_drag_rect) {
  const Rect source{10, 20, 100, 50};
  /* Rect 没有 operator==（C++17 不合成）：统一走字段比较，避免各写一遍。 */
  const auto is = [](const Rect& r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
  };
  const auto drag = [&source](int index, double x, double y, bool lock) {
    return comp_ui::selection_drag_rect(source, index, Point{x, y}, lock);
  };

  /* 1. 每个手柄只动自己那条边（对边钉住）。 */
  COMP_CHECK(is(drag(3, 150, 200, false), 10, 20, 140, 50));
  COMP_CHECK(is(drag(7, 0, 200, false), 0, 20, 110, 50));
  COMP_CHECK(is(drag(5, 0, 100, false), 10, 20, 100, 80));
  COMP_CHECK(is(drag(1, 0, 40, false), 10, 40, 100, 30));
  COMP_CHECK(is(drag(0, 30, 40, false), 30, 40, 80, 30));
  COMP_CHECK(is(drag(4, 60, 45, false), 10, 20, 50, 25));

  /* 2. 拖过头：夹到 1 像素，不出现负宽高、也不镜像。 */
  COMP_CHECK(is(drag(3, 5, 0, false), 10, 20, 1, 50));
  COMP_CHECK(is(drag(7, 400, 0, false), 109, 20, 1, 50));
  COMP_CHECK(is(drag(1, 0, 400, false), 10, 69, 100, 1));

  /* 3. Shift 等比：角手柄保持源比例（100:50 = 2），锚点是对角。 */
  const Rect locked = drag(4, 210, 220, true);
  COMP_CHECK(is(locked, 10, 20, 400, 200));
  COMP_CHECK_MSG(locked.w == 2 * locked.h, "aspect lock keeps 2:1");
  /* 锚点东南角 (110,70)：dx = 200，dy = 150；等比按主导轴 dy*aspect = 300，
   * 所以宽 300（不是 dx 的 200），左边界 = 110 - 300 = -190。 */
  COMP_CHECK(is(drag(0, -90, -80, true), -190, -80, 300, 150));

  /* 4. 边手柄忽略 Shift（PS 同语义），只做普通单边拖拽。 */
  COMP_CHECK(is(drag(1, 999, 60, true), 10, 60, 100, 10));

  /* 5. 退化输入：空矩形 / 越界索引 / NaN 一律原样返回。 */
  const Rect empty_source{10, 20, 0, 50};
  COMP_CHECK(is(comp_ui::selection_drag_rect(empty_source, 3, Point{0, 0}, false), 10, 20, 0, 50));
  COMP_CHECK(is(drag(8, 0, 0, false), 10, 20, 100, 50));
  COMP_CHECK(is(comp_ui::selection_drag_rect(
                   source, 3, Point{std::numeric_limits<double>::quiet_NaN(), 0}, false),
               10, 20, 100, 50));
}

/*
 * P0 文档级对话框的行模型。这里钉的是「面板显示的数字 = 写进文档的数字」：
 * 行数与行序必须与壳里的取值 switch 一一对应，夹取口径必须与 ABI 注释同界，
 * 命中测试必须与绘制几何同源。
 */
COMP_TEST(ui_core_doc_dialog_rows) {
  using comp_ui::DocDialogKind;
  using comp_ui::DocRowKind;

  /* 1. 四种对话框的行数与行序（顺序是壳取值 switch 的约定，不能随便调）。 */
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::None) == 0);
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::NewDocument) == 6);
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::ImageSize) == 4);
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::CanvasSize) == 3);
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::HistoryLimit) == 2);

  const comp_ui::DocRowSpec new_w = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 0);
  const comp_ui::DocRowSpec new_h = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 1);
  const comp_ui::DocRowSpec new_dpi = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 2);
  const comp_ui::DocRowSpec new_bg = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 3);
  COMP_CHECK(new_w.kind == DocRowKind::Number && new_h.kind == DocRowKind::Number);
  /* 宽高同步长：用同一个 64 步，避免「按两下宽度跳 64、高度跳 1」的手感不一致。 */
  COMP_CHECK(new_w.step == new_h.step && new_w.min == new_h.min && new_w.max == new_h.max);
  COMP_CHECK(new_dpi.kind == DocRowKind::Number && new_dpi.min == comp_ui::kDocDpiMin);
  COMP_CHECK(new_bg.kind == DocRowKind::Choice && new_bg.choice_count == 3);

  /* P0-1：位深 / 颜色模式两行（各两档）。行序是壳取值 switch 的约定：
   * 4 = 位深、5 = 颜色模式，插在背景之后、不能挪到前面。 */
  const comp_ui::DocRowSpec new_depth = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 4);
  const comp_ui::DocRowSpec new_mode = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 5);
  COMP_CHECK(new_depth.kind == DocRowKind::Choice && new_depth.choice_count == 2);
  COMP_CHECK(new_mode.kind == DocRowKind::Choice && new_mode.choice_count == 2);
  COMP_CHECK(new_depth.min == 0 && new_depth.max == 1);
  COMP_CHECK(new_mode.min == 0 && new_mode.max == 1);
  /* 枚举行文案不能互相串（背景名/位深名/模式名三套）。 */
  COMP_CHECK(std::string(comp_ui::doc_new_bit_depth_name(0)) !=
             std::string(comp_ui::doc_new_color_mode_name(0)));
  COMP_CHECK(std::string(comp_ui::doc_new_bit_depth_name(1)).find("16") != std::string::npos);
  COMP_CHECK(std::string(comp_ui::doc_new_color_mode_name(1)).find("灰") != std::string::npos);

  const comp_ui::DocRowSpec image_lock = comp_ui::doc_dialog_row(DocDialogKind::ImageSize, 2);
  const comp_ui::DocRowSpec image_percent = comp_ui::doc_dialog_row(DocDialogKind::ImageSize, 3);
  COMP_CHECK(image_lock.kind == DocRowKind::Toggle);
  COMP_CHECK(image_percent.kind == DocRowKind::Number && image_percent.max == 10000);

  const comp_ui::DocRowSpec canvas_anchor = comp_ui::doc_dialog_row(DocDialogKind::CanvasSize, 2);
  COMP_CHECK(canvas_anchor.kind == DocRowKind::Choice && canvas_anchor.choice_count == 9);

  /* 2. 越界行返回空标签的安全默认值（绘制侧会照常打出空行，但不会越界读）。 */
  const comp_ui::DocRowSpec missing = comp_ui::doc_dialog_row(DocDialogKind::NewDocument, 99);
  COMP_CHECK(missing.label != nullptr && missing.label[0] == '\0');

  /* 3. 夹取：与 comp_document_desc 的 1..30000 同界，0 步历史 = 引擎默认。 */
  COMP_CHECK(comp_ui::clamp_doc_size(-5) == comp_ui::kDocSizeMin);
  COMP_CHECK(comp_ui::clamp_doc_size(0) == comp_ui::kDocSizeMin);
  COMP_CHECK(comp_ui::clamp_doc_size(30001) == comp_ui::kDocSizeMax);
  COMP_CHECK(comp_ui::clamp_doc_size(1920) == 1920);
  COMP_CHECK(comp_ui::clamp_doc_dpi(0) == comp_ui::kDocDpiMin);
  COMP_CHECK(comp_ui::clamp_doc_dpi(99999) == comp_ui::kDocDpiMax);
  /* 历史两项的 0 是「合法值 = 交回引擎默认」，不能被夹到 1。 */
  COMP_CHECK(comp_ui::clamp_history_steps(0) == 0);
  COMP_CHECK(comp_ui::clamp_history_steps(-1) == 0);
  COMP_CHECK(comp_ui::clamp_history_steps(99999) == comp_ui::kDocHistoryStepsMax);
  COMP_CHECK(comp_ui::clamp_history_budget_mb(0) == 0);
  COMP_CHECK(comp_ui::clamp_history_budget_mb(-8) == 0);
  COMP_CHECK(comp_ui::clamp_history_budget_mb(999999) == comp_ui::kDocHistoryBudgetMaxMb);

  /* 4. 行几何与命中：第 0 行紧贴标题下沿，行高整格，行间无缝隙、无重叠。 */
  const Rect panel{900, 60, 320, 640};
  const Rect row0 = comp_ui::doc_dialog_row_rect(panel, 0);
  const Rect row1 = comp_ui::doc_dialog_row_rect(panel, 1);
  COMP_CHECK(row0.y == panel.y + comp_ui::kDocDialogRowsTop);
  COMP_CHECK(row1.y == row0.bottom());
  COMP_CHECK(row0.h == comp_ui::kDocDialogRowHeight);

  const double first_y = panel.y + comp_ui::kDocDialogRowsTop + 4.0;
  COMP_CHECK(comp_ui::doc_dialog_row_at(panel, 4, panel.x + 10.0, first_y) == 0);
  COMP_CHECK(comp_ui::doc_dialog_row_at(
                 panel, 4, panel.x + 10.0,
                 first_y + comp_ui::kDocDialogRowHeight * 3) == 3);
  /* 超出末行 = 未命中（不是夹到末行：点空白不该改最后一行的值）。 */
  COMP_CHECK(comp_ui::doc_dialog_row_at(
                 panel, 4, panel.x + 10.0,
                 first_y + comp_ui::kDocDialogRowHeight * 4) == -1);
  /* 标题区与面板外一律未命中。 */
  COMP_CHECK(comp_ui::doc_dialog_row_at(panel, 4, panel.x + 10.0, panel.y + 10.0) == -1);
  COMP_CHECK(comp_ui::doc_dialog_row_at(panel, 4, panel.x - 5.0, first_y) == -1);
  COMP_CHECK(comp_ui::doc_dialog_row_at(panel, 0, panel.x + 10.0, first_y) == -1);

  /* 5. P0-5 收尾：变换选区（宽 / 高 / 角度）。行数 3，角度行独占负值量程。 */
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::TransformSelection) == 3);
  const comp_ui::DocRowSpec transform_w =
      comp_ui::doc_dialog_row(DocDialogKind::TransformSelection, 0);
  const comp_ui::DocRowSpec transform_h =
      comp_ui::doc_dialog_row(DocDialogKind::TransformSelection, 1);
  const comp_ui::DocRowSpec transform_a =
      comp_ui::doc_dialog_row(DocDialogKind::TransformSelection, 2);
  COMP_CHECK(transform_w.kind == DocRowKind::Number && transform_h.kind == DocRowKind::Number);
  /* 宽高同量程（与「图像大小」同界），否则面板能把选区拖到引擎拒绝的尺寸上。 */
  COMP_CHECK(transform_w.min == comp_ui::kDocSizeMin && transform_w.max == comp_ui::kDocSizeMax);
  COMP_CHECK(transform_h.min == transform_w.min && transform_h.max == transform_w.max);
  COMP_CHECK(transform_a.kind == DocRowKind::Number);
  COMP_CHECK(transform_a.min == -180 && transform_a.max == 180);
  /* 角度必须允许负号（doc_dialog_begin_input 只在 min < 0 的行收 '-'）。 */
  COMP_CHECK(transform_a.min < 0);
  COMP_CHECK(transform_a.step == 15);
  /* 标题/副标题/提示三处都必须有字，否则面板顶上是一片空白。 */
  COMP_CHECK(std::string(comp_ui::doc_dialog_title(DocDialogKind::TransformSelection)).size() > 0);
  COMP_CHECK(std::string(comp_ui::doc_dialog_subtitle(DocDialogKind::TransformSelection)).size() > 0);
  COMP_CHECK(std::string(comp_ui::doc_dialog_hint(DocDialogKind::TransformSelection)).size() > 0);

  /*
   * 6. P1-3：数值变换（图层级）。行序与选区那条一致（宽 / 高 / 角度），量程也必须
   * 一致——两套面板用同一个 kTransformAngle*，否则「同一个角度」在两边能填的数不同。
   */
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::TransformLayer) == 3);
  const comp_ui::DocRowSpec layer_w = comp_ui::doc_dialog_row(DocDialogKind::TransformLayer, 0);
  const comp_ui::DocRowSpec layer_h = comp_ui::doc_dialog_row(DocDialogKind::TransformLayer, 1);
  const comp_ui::DocRowSpec layer_a = comp_ui::doc_dialog_row(DocDialogKind::TransformLayer, 2);
  COMP_CHECK(layer_w.kind == DocRowKind::Number && layer_h.kind == DocRowKind::Number);
  COMP_CHECK(layer_a.kind == DocRowKind::Number);
  COMP_CHECK(layer_w.min == comp_ui::kDocSizeMin && layer_w.max == comp_ui::kDocSizeMax);
  COMP_CHECK(layer_h.min == layer_w.min && layer_h.max == layer_w.max);
  COMP_CHECK(layer_a.min == comp_ui::kTransformAngleMin);
  COMP_CHECK(layer_a.max == comp_ui::kTransformAngleMax);
  COMP_CHECK(layer_a.step == comp_ui::kTransformAngleStep);
  /* 宽高的步进不能是 1：几千像素的图层按一次改 1px 是没法用的。 */
  COMP_CHECK(layer_w.step > 1);
  COMP_CHECK(std::string(comp_ui::doc_dialog_title(DocDialogKind::TransformLayer)).size() > 0);
  COMP_CHECK(std::string(comp_ui::doc_dialog_subtitle(DocDialogKind::TransformLayer)).size() > 0);
  COMP_CHECK(std::string(comp_ui::doc_dialog_hint(DocDialogKind::TransformLayer)).size() > 0);
  /*
   * 角度回显：引擎给的是 float，面板只有整数行。归一化到 (-180, 180]，四舍五入，
   * 并夹在行量程内——否则 `%d 度` 会显示一个填不回去的数（-180.4 → -180 尚可，
   * 181 → 181 就越界了）。
   */
  COMP_CHECK(comp_ui::doc_angle_from_rotation(0.0) == 0);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(22.4) == 22);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(22.5) == 23);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(-22.5) == -23);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(180.0) == 180);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(-180.0) == 180);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(540.0) == 180);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(370.0) == 10);
  COMP_CHECK(comp_ui::doc_angle_from_rotation(-370.0) == -10);
}

/*
 * P1-7 收尾：可编辑色标面板（DocDialogKind::GradientStops）。
 *
 * 行序是面板、键盘步进与 visual_check 共用的契约：0 = 选中第几个（1 基）、
 * 1/2 = 位置/中点（‰）、3..5 = RGB、6 = 不透明度（%）、7 = 增删动作。
 * 这里只钉几何与量程；取值换算在 shell_document.cpp，靠窗口/Selftest 覆盖。
 */
COMP_TEST(ui_core_gradient_stops_dialog_rows) {
  using comp_ui::DocDialogKind;
  using comp_ui::DocRowKind;

  COMP_CHECK(comp_ui::kGradientStopsRowCount == 8);
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::GradientStops) ==
             comp_ui::kGradientStopsRowCount);
  /* 追加在枚举尾部：对话框编号是外部契约，不能插到中间。 */
  COMP_CHECK(static_cast<int>(DocDialogKind::GradientStops) >
             static_cast<int>(DocDialogKind::TransformLayer));

  const comp_ui::DocRowSpec index = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, 0);
  COMP_CHECK(index.kind == DocRowKind::Number);
  COMP_CHECK(index.min == 1 && index.max == comp_ui::kGradientMaxStops);
  const comp_ui::DocRowSpec position = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, 1);
  COMP_CHECK(position.kind == DocRowKind::Number && position.min == 0 && position.max == 1000);
  const comp_ui::DocRowSpec midpoint = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, 2);
  COMP_CHECK(midpoint.kind == DocRowKind::Number && midpoint.min == 1 && midpoint.max == 999);
  for (int row = 3; row <= 5; ++row) {
    const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, row);
    COMP_CHECK(spec.kind == DocRowKind::Number && spec.min == 0 && spec.max == 255);
  }
  const comp_ui::DocRowSpec alpha = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, 6);
  COMP_CHECK(alpha.kind == DocRowKind::Number && alpha.min == 0 && alpha.max == 100);
  const comp_ui::DocRowSpec action = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, 7);
  COMP_CHECK(action.kind == DocRowKind::Choice);
  COMP_CHECK(action.choice_count == 3 && action.min == 0 && action.max == 2);
  /* 越界行返回空行（空 label），绘制循环据此停。 */
  const comp_ui::DocRowSpec none = comp_ui::doc_dialog_row(DocDialogKind::GradientStops, 8);
  COMP_CHECK(none.label != nullptr && none.label[0] == '\0');
  COMP_CHECK(std::string(comp_ui::doc_dialog_title(DocDialogKind::GradientStops)).size() > 0);
  COMP_CHECK(std::string(comp_ui::doc_dialog_subtitle(DocDialogKind::GradientStops)).size() > 0);
  COMP_CHECK(std::string(comp_ui::doc_dialog_hint(DocDialogKind::GradientStops)).size() > 0);
}

/*
 * P1-5 Camera Raw 面板：行序 + JSON 生成。
 *
 * JSON 键名必须是**引擎认识的 camelCase**（未知键是 COMP_ERR_INVALID_ARGUMENT，
 * 不是忽略），所以这里把 12 个键逐字钉死；数值只写非零行、曝光除以 100 写成
 * 带两位小数的 EV，是全零 → "{}"（恒等设置、不产生撤销步）的前提。
 */
COMP_TEST(ui_core_camera_raw_rows) {
  using comp_ui::DocDialogKind;
  using comp_ui::DocRowKind;

  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::CameraRaw) ==
             comp_ui::kCameraRawRowCount);
  COMP_CHECK(comp_ui::kCameraRawRowCount == 12);

  /* 行 0 是曝光（0.01 EV 整数单位），其余 11 行都是 ±100 的整数档。 */
  const comp_ui::DocRowSpec exposure = comp_ui::doc_dialog_row(DocDialogKind::CameraRaw, 0);
  COMP_CHECK(exposure.kind == DocRowKind::Number);
  COMP_CHECK(exposure.min == -comp_ui::kCameraRawExposureMax);
  COMP_CHECK(exposure.max == comp_ui::kCameraRawExposureMax);
  COMP_CHECK(comp_ui::camera_raw_row_is_exposure(0));
  for (int row = 1; row < comp_ui::kCameraRawRowCount; ++row) {
    const comp_ui::DocRowSpec spec = comp_ui::doc_dialog_row(DocDialogKind::CameraRaw, row);
    COMP_CHECK(spec.kind == DocRowKind::Number);
    COMP_CHECK(spec.min == -100 && spec.max == 100);
    COMP_CHECK(!comp_ui::camera_raw_row_is_exposure(row));
  }

  /* 键名与行号一一对应，且互不重复（重复会写出两份同名键）。 */
  COMP_CHECK(std::string(comp_ui::camera_raw_row_key(0)) == "exposure");
  COMP_CHECK(std::string(comp_ui::camera_raw_row_key(1)) == "temperature");
  COMP_CHECK(std::string(comp_ui::camera_raw_row_key(11)) == "dehaze");
  COMP_CHECK(comp_ui::camera_raw_row_key(12) == nullptr);
  COMP_CHECK(comp_ui::camera_raw_row_key(-1) == nullptr);
  for (int row = 0; row < comp_ui::kCameraRawRowCount; ++row) {
    for (int other = row + 1; other < comp_ui::kCameraRawRowCount; ++other) {
      COMP_CHECK(std::string(comp_ui::camera_raw_row_key(row)) !=
                 std::string(comp_ui::camera_raw_row_key(other)));
    }
  }

  /* 全零 = 恒等设置：必须是空对象，而不是 12 个 "key":0。 */
  int values[comp_ui::kCameraRawRowCount] = {};
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, comp_ui::kCameraRawRowCount) == "{}");

  /* 曝光 35 = +0.35 EV；小数点在 JSON 里必须是 '.'，且只有两位。 */
  values[0] = 35;
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, comp_ui::kCameraRawRowCount) ==
             "{\"exposure\":0.35}");
  values[0] = -125;
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, comp_ui::kCameraRawRowCount) ==
             "{\"exposure\":-1.25}");
  values[0] = 0;

  /* 非零行按行序输出；零行被跳过（0 就是上游缺省值，写出来只会白跑一遍管线）。 */
  values[1] = -40;
  values[7] = 12;
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, comp_ui::kCameraRawRowCount) ==
             "{\"temperature\":-40,\"blacks\":12}");

  /* 行数上限由 count 与 kCameraRawRowCount 中较小者决定：多传不越界，少传截断。 */
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, 1) == "{}");
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, 64) ==
             "{\"temperature\":-40,\"blacks\":12}");
  COMP_CHECK(comp_ui::camera_raw_settings_json(nullptr, comp_ui::kCameraRawRowCount) == "{}");
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, 0) == "{}");
  COMP_CHECK(comp_ui::camera_raw_settings_json(values, -3) == "{}");
}

COMP_TEST(ui_core_close_prompt_rows) {
  using comp_ui::DocDialogKind;
  using comp_ui::DocRowKind;

  /*
   * 关闭提示（P0-3）也走同一套行模型：只有一行「动作」，三档循环。
   * 这里钉住的是壳的另一半约定——行值直接就是 kCloseAction*，面板显示、
   * 状态栏与执行分支读的是同一份数（显示「保存」却执行「丢弃」是最坏的失败）。
   */
  COMP_CHECK(comp_ui::doc_dialog_row_count(DocDialogKind::ClosePrompt) == 1);
  const comp_ui::DocRowSpec action = comp_ui::doc_dialog_row(DocDialogKind::ClosePrompt, 0);
  COMP_CHECK(action.kind == DocRowKind::Choice && action.choice_count == 3);
  COMP_CHECK(action.min == comp_ui::kCloseActionSave);
  COMP_CHECK(action.max == comp_ui::kCloseActionCancel);
  /* 缺省动作是「保存」：回车落在安全侧，误触不会丢像素。 */
  COMP_CHECK(comp_ui::kCloseActionSave == 0);
  COMP_CHECK(comp_ui::kCloseActionDiscard == 1);
  COMP_CHECK(comp_ui::kCloseActionCancel == 2);
  COMP_CHECK(std::string(comp_ui::doc_close_action_name(comp_ui::kCloseActionSave)) != "");
  COMP_CHECK(std::string(comp_ui::doc_close_action_name(comp_ui::kCloseActionDiscard)) !=
             comp_ui::doc_close_action_name(comp_ui::kCloseActionSave));
  COMP_CHECK(std::string(comp_ui::doc_close_action_name(comp_ui::kCloseActionCancel)) != "");
  /* 越界动作名不返回 nullptr（绘制侧直接 TextOut 它）。 */
  COMP_CHECK(comp_ui::doc_close_action_name(99) != nullptr);
  /* 标题/提示必须非空：面板靠它们告诉用户「这不是普通对话框」。 */
  COMP_CHECK(std::string(comp_ui::doc_dialog_title(DocDialogKind::ClosePrompt)) != "");
  COMP_CHECK(std::string(comp_ui::doc_dialog_hint(DocDialogKind::ClosePrompt)) != "");
  /* 越界行仍然是安全默认值，不越界读。 */
  const comp_ui::DocRowSpec out_of_range =
      comp_ui::doc_dialog_row(DocDialogKind::ClosePrompt, 3);
  COMP_CHECK(out_of_range.label != nullptr && out_of_range.label[0] == '\0');
}

COMP_TEST(ui_core_doc_dialog_anchor) {
  /* 1. 四角钉住不动、四边只动一个轴、居中两轴各动一半——这是画布大小的全部语义。 */
  const int dw = 100;
  const int dh = 60;
  COMP_CHECK(comp_ui::doc_anchor_dx(0, dw) == 0);   /* 左上 */
  COMP_CHECK(comp_ui::doc_anchor_dy(0, dh) == 0);
  COMP_CHECK(comp_ui::doc_anchor_dx(8, dw) == dw);  /* 右下 */
  COMP_CHECK(comp_ui::doc_anchor_dy(8, dh) == dh);
  COMP_CHECK(comp_ui::doc_anchor_dx(2, dw) == dw);  /* 右上 */
  COMP_CHECK(comp_ui::doc_anchor_dy(2, dh) == 0);
  COMP_CHECK(comp_ui::doc_anchor_dx(6, dw) == 0);   /* 左下 */
  COMP_CHECK(comp_ui::doc_anchor_dy(6, dh) == dh);
  COMP_CHECK(comp_ui::doc_anchor_dx(4, dw) == dw / 2); /* 居中 */
  COMP_CHECK(comp_ui::doc_anchor_dy(4, dh) == dh / 2);
  COMP_CHECK(comp_ui::doc_anchor_dx(1, dw) == dw / 2); /* 上中 */
  COMP_CHECK(comp_ui::doc_anchor_dy(1, dh) == 0);

  /* 2. 负增量（缩画布）同样按锚点分：居中向右上挪，右下整幅挪。 */
  COMP_CHECK(comp_ui::doc_anchor_dx(0, -40) == 0);
  COMP_CHECK(comp_ui::doc_anchor_dx(8, -40) == -40);
  COMP_CHECK(comp_ui::doc_anchor_dx(4, -40) == -20);

  /* 3. 半量取整朝零：-5/2 = -2（不是 -3），避免内容在奇偶差上抖动。 */
  COMP_CHECK(comp_ui::doc_anchor_offset(-5, 1) == -2);
  COMP_CHECK(comp_ui::doc_anchor_offset(5, 1) == 2);
  COMP_CHECK(comp_ui::doc_anchor_offset(1, 1) == 0);

  /* 4. 非法锚点回落到居中，而不是回落到「左上」——静默贴边比不动更意外。 */
  COMP_CHECK(comp_ui::doc_anchor_dx(-1, dw) == dw / 2);
  COMP_CHECK(comp_ui::doc_anchor_dy(99, dh) == dh / 2);

  /* 5. 九个名字互不相同，且顺序与 3×3 行优先一致。 */
  COMP_CHECK(std::string(comp_ui::doc_anchor_name(0)) == "左上");
  COMP_CHECK(std::string(comp_ui::doc_anchor_name(4)) == "居中");
  COMP_CHECK(std::string(comp_ui::doc_anchor_name(8)) == "右下");
  COMP_CHECK(std::string(comp_ui::doc_anchor_name(9)) == "?");
}

COMP_TEST(ui_core_doc_dialog_percent_and_aspect) {
  /* 1. 百分比换算 ±1 取整误差内往返（面板回显不会抖）。 */
  const int bases[] = {1, 37, 1280, 4096, 30000};
  for (const int base : bases) {
    COMP_CHECK(comp_ui::doc_size_from_percent(base, 100) == base);
    const int scaled = comp_ui::doc_size_from_percent(base, 50);
    COMP_CHECK(scaled <= base);
    /* 回显误差上界随基准尺寸缩小：base=1 时四舍五入本身就吃掉 50 个百分点，
     * 这不是漂移，是 1 px 的画布没有「半像素」。 */
    const int tolerance = 1 + 100 / base;
    const int back = comp_ui::doc_percent_from_size(base, scaled);
    COMP_CHECK(back >= 50 - tolerance && back <= 50 + tolerance);
  }

  /* 2. 百分比越界：0/负数当 1%，超上限按尺寸上限截断（不是溢出成负数）。 */
  COMP_CHECK(comp_ui::doc_size_from_percent(1280, 0) ==
             comp_ui::doc_size_from_percent(1280, 1));
  COMP_CHECK(comp_ui::doc_size_from_percent(1280, 10000) == comp_ui::kDocSizeMax);
  COMP_CHECK(comp_ui::doc_percent_from_size(0, 100) == 100);
  COMP_CHECK(comp_ui::doc_percent_from_size(1280, 30000) == 2344);
  COMP_CHECK(comp_ui::doc_percent_from_size(1280, 1) == 1);

  /* 3. 约束比例从**原始**尺寸推另一个轴：连续两次改宽不会累积取整误差。 */
  COMP_CHECK(comp_ui::doc_locked_height(1280, 720, 640) == 360);
  COMP_CHECK(comp_ui::doc_locked_width(1280, 720, 360) == 640);
  COMP_CHECK(comp_ui::doc_locked_height(300, 200, 150) == 100);
  COMP_CHECK(comp_ui::doc_locked_width(300, 200, 100) == 150);
  /* 第二次仍以原始 1280×720 为基准，结果与第一次相同。 */
  COMP_CHECK(comp_ui::doc_locked_height(1280, 720, 640) == 360);

  /* 4. 极端比例：放大到上限时另一轴同步夹取，仍保持 1..30000。 */
  const int huge_width = comp_ui::clamp_doc_size(comp_ui::kDocSizeMax);
  const int linked = comp_ui::doc_locked_height(1, 1, huge_width);
  COMP_CHECK(huge_width == comp_ui::kDocSizeMax);
  COMP_CHECK(linked == comp_ui::kDocSizeMax);

  /* 5. 退化输入（原始宽高为 0）不除零，原样返回。 */
  COMP_CHECK(comp_ui::doc_locked_height(0, 720, 640) == 720);
  COMP_CHECK(comp_ui::doc_locked_width(1280, 0, 360) == 1280);
}

/*
 * 字体面板（P0-6②）：行几何与过滤匹配。
 *
 * 面板比路径/历史多占一行（扫描进度 + 过滤词），列表从 y+80 起、底部留 4 行
 * 提示。这里钉的是「画在哪一行 = 点中哪一行」：几何公式、滚动夹取、命中范围
 * 一次对齐；再钉住过滤的语义（ASCII 不区分大小写、中文按码元逐字比、空过滤
 * 恒真），免得「输了个字母列表就空了」被当成扫描失败。
 */
COMP_TEST(ui_core_font_panel_rows) {
  const comp_ui::Rect panel{1000, 100, 330, 648};
  COMP_CHECK(comp_ui::kFontRowHeight == 22);
  COMP_CHECK(comp_ui::kFontRowsTop == 80);
  COMP_CHECK(comp_ui::kFontPanelFooter == 88);

  /* 1. 行几何：列表顶着过滤行，行高恒定，滚动只平移。 */
  const comp_ui::Rect first = comp_ui::font_row_rect(panel, 0);
  COMP_CHECK(first.x == panel.x && first.w == panel.w && first.h == comp_ui::kFontRowHeight);
  COMP_CHECK(first.y == panel.y + comp_ui::kFontRowsTop);
  COMP_CHECK(comp_ui::font_row_rect(panel, 3).y == first.y + 3 * comp_ui::kFontRowHeight);

  /* 2. 可视行数：648 高减去 80 的表头与 88 的提示区，整除 22。 */
  const int visible = comp_ui::font_visible_rows(panel);
  COMP_CHECK(visible == (panel.h - comp_ui::kFontRowsTop - comp_ui::kFontPanelFooter) /
                            comp_ui::kFontRowHeight);
  COMP_CHECK(visible > 0);

  /* 3. 滚动夹取：少于可视行数不滚；超出按上限截断，负数归零。 */
  COMP_CHECK(comp_ui::font_clamp_scroll(panel, 5, 3) == 0);
  COMP_CHECK(comp_ui::font_clamp_scroll(panel, 100, -7) == 0);
  COMP_CHECK(comp_ui::font_clamp_scroll(panel, 100, 999) == 100 - visible);

  /* 4. 选行跟随：窗口内不动，越界只滚最小距离。 */
  COMP_CHECK(comp_ui::font_ensure_visible(panel, 100, 3, 0) == 0);
  COMP_CHECK(comp_ui::font_ensure_visible(panel, 100, visible - 1, 0) == 0);
  COMP_CHECK(comp_ui::font_ensure_visible(panel, 100, visible, 0) == 1);
  COMP_CHECK(comp_ui::font_ensure_visible(panel, 100, 99, 0) == 99 - visible + 1);
  COMP_CHECK(comp_ui::font_ensure_visible(panel, 100, 2, 10) == 2);
  COMP_CHECK(comp_ui::font_ensure_visible(panel, 5, 9, 0) == 0);

  /* 滚到底时最后一行仍完整落在可视区内，不压到底部提示。 */
  const int tail_scroll = comp_ui::font_clamp_scroll(panel, 100, 999);
  COMP_CHECK(tail_scroll + visible == 100);
  const comp_ui::Rect last = comp_ui::font_row_rect(panel, visible - 1);
  COMP_CHECK(last.bottom() <= panel.bottom() - comp_ui::kFontPanelFooter);

  /* 5. 命中：行首/行尾同一行；表头、提示区、面板外、空列表都返回 -1。 */
  for (int row = 0; row < 3; ++row) {
    const comp_ui::Rect rect = comp_ui::font_row_rect(panel, row);
    const double x = static_cast<double>(rect.x + 4);
    COMP_CHECK(comp_ui::font_row_at(panel, 100, 0, x, static_cast<double>(rect.y)) == row);
    COMP_CHECK(comp_ui::font_row_at(panel, 100, 0, x, static_cast<double>(rect.bottom()) - 1.0) ==
               row);
  }
  COMP_CHECK(comp_ui::font_row_at(panel, 100, 0, static_cast<double>(first.x + 4),
                                  static_cast<double>(panel.y + 10)) == -1);
  COMP_CHECK(comp_ui::font_row_at(panel, 100, 0, static_cast<double>(panel.right() + 4),
                                  static_cast<double>(first.y)) == -1);
  COMP_CHECK(comp_ui::font_row_at(panel, 0, 0, static_cast<double>(first.x + 4),
                                  static_cast<double>(first.y)) == -1);
  COMP_CHECK(comp_ui::font_row_at(panel, 100, 0, static_cast<double>(first.x + 4),
                                  static_cast<double>(panel.bottom()) - 4.0) == -1);
  /* 滚过 2 行后屏幕第一行是绝对第 2 行。 */
  COMP_CHECK(comp_ui::font_row_at(panel, 100, 2, static_cast<double>(first.x + 4),
                                  static_cast<double>(first.y)) == 2);
}

COMP_TEST(ui_core_font_label_filter) {
  /* 空过滤恒真：列表不会因为「还没输入」而空。 */
  COMP_CHECK(comp_ui::font_label_matches(L"", L""));
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L""));
  /* 空标签配非空过滤必然不匹配（避免空行冒充命中）。 */
  COMP_CHECK(!comp_ui::font_label_matches(L"", L"a"));

  /* ASCII 大小写不敏感，两端都折叠；子串而非前缀——"Sans" 里也能找到 "ans"。 */
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L"noto"));
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L"NOTO"));
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L"Sans"));
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L"ans"));
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L"sc"));
  COMP_CHECK(comp_ui::font_label_matches(L"Noto Sans SC", L"Noto Sans SC"));
  COMP_CHECK(!comp_ui::font_label_matches(L"Noto Sans SC", L"Noto Serif"));
  /* 过滤词比标签长：直接不匹配，不越界读。 */
  COMP_CHECK(!comp_ui::font_label_matches(L"Abc", L"abcd"));

  /* 中文按码元逐字比：能匹配中间片段，不做拼音/分词。 */
  COMP_CHECK(comp_ui::font_label_matches(L"思源黑体", L"思源"));
  COMP_CHECK(comp_ui::font_label_matches(L"思源黑体", L"黑体"));
  COMP_CHECK(!comp_ui::font_label_matches(L"思源黑体", L"宋体"));
  /* 空格是标签的一部分，不会被忽略。 */
  COMP_CHECK(!comp_ui::font_label_matches(L"思源黑体", L"思源 黑体"));

  /* 混合标签：中文过滤词不会命中纯英文标签，反之亦然。 */
  COMP_CHECK(comp_ui::font_label_matches(L"思源黑体 Noto Sans SC", L"noto"));
  COMP_CHECK(comp_ui::font_label_matches(L"思源黑体 Noto Sans SC", L"黑体"));
  COMP_CHECK(!comp_ui::font_label_matches(L"Noto Sans SC", L"思源"));

  /* 真实条目名（内置点阵）里的 ASCII 也能被小写过滤词找到。 */
  COMP_CHECK(comp_ui::font_label_matches(L"5×7 点阵（内置，仅 ASCII）", L"ascii"));
}

/*
 * ROI #3：标尺几何 / 刻度步长 / 命中 / 吸附半径。
 *
 * 标尺是**画在画布内侧**的 18px 条带（不从布局里切走），所以这里钉的是：
 *   1. 三条矩形都贴画布左上角、宽度厚度恒定，且互不越界；
 *   2. 主刻度走 1-2-5 序列，屏幕间距永不小于 56px（标签不相撞），缩放越大越细；
 *   3. 命中顺序：上尺优先，左上角算上尺（拖出水平参考线），标尺隐藏时整块失效；
 *   4. 吸附半径按设备像素换算，缩放级之间手感一致，非法缩放回落到 2%。
 */
COMP_TEST(ui_core_rulers_and_snap_tolerance) {
  const Rect canvas{100, 50, 800, 600};

  /* 1. 三条矩形：厚度恒定、贴左上角，交叉块等于两把尺子的交集。 */
  COMP_CHECK(comp_ui::Layout::kRulerThickness == 18);
  const Rect top = comp_ui::ruler_top_rect(canvas);
  const Rect left = comp_ui::ruler_left_rect(canvas);
  const Rect corner = comp_ui::ruler_corner_rect(canvas);
  COMP_CHECK(top.x == canvas.x && top.y == canvas.y && top.w == canvas.w);
  COMP_CHECK(top.h == comp_ui::Layout::kRulerThickness);
  COMP_CHECK(left.x == canvas.x && left.y == canvas.y &&
             left.h == canvas.h && left.w == comp_ui::Layout::kRulerThickness);
  COMP_CHECK(corner.x == canvas.x && corner.y == canvas.y &&
             corner.w == comp_ui::Layout::kRulerThickness &&
             corner.h == comp_ui::Layout::kRulerThickness);
  /* 条带都在画布内侧（画布是缩放/平移的锚点，标尺不得让它变形）。 */
  COMP_CHECK(top.right() == canvas.right() && top.bottom() <= canvas.bottom());
  COMP_CHECK(left.bottom() == canvas.bottom() && left.right() <= canvas.right());

  /* 2. 刻度步长：1-2-5 序列 + 屏幕间距 >= 56px，且是满足条件的最小值。 */
  const double zooms[] = {0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 32.0};
  for (const double zoom : zooms) {
    const double step = comp_ui::ruler_major_step(zoom);
    COMP_CHECK(step * zoom >= 56.0 - 1e-9);
    /* 序列成员判定：首位 1/2/5 再乘 10 的幂。 */
    double mantissa = step;
    while (mantissa >= 10.0) mantissa /= 10.0;
    COMP_CHECK(near_value(mantissa, 1.0) || near_value(mantissa, 2.0) ||
               near_value(mantissa, 5.0));
    /* 最小性：退一档就不够了（否则标签会比需要的更稀，读数精度白丢）。 */
    double smaller = 0.0;
    if (near_value(mantissa, 2.0)) smaller = step * 0.5;
    if (near_value(mantissa, 5.0)) smaller = step * 0.4;
    if (near_value(mantissa, 1.0) && step > 1.0) smaller = step * 0.5;
    if (smaller > 0.0) COMP_CHECK(smaller * zoom < 56.0 + 1e-9);
  }
  COMP_CHECK(near_value(comp_ui::ruler_major_step(1.0), 100.0));
  COMP_CHECK(near_value(comp_ui::ruler_major_step(32.0), 2.0));
  COMP_CHECK(near_value(comp_ui::ruler_major_step(0.02), 5000.0));
  /* 非法缩放（0 / 负数 / NaN）回落到下限 2%，不返回 0 或 inf。 */
  COMP_CHECK(near_value(comp_ui::ruler_major_step(0.0), 5000.0));
  COMP_CHECK(near_value(comp_ui::ruler_major_step(-4.0), 5000.0));
  COMP_CHECK(near_value(comp_ui::ruler_major_step(
                            std::numeric_limits<double>::quiet_NaN()),
                        5000.0));

  /* 3. 命中：上尺 1、左尺 2、左上角 1、画布其余 0。 */
  const Layout fixed = comp_ui::compute_layout(1280, 800);
  COMP_CHECK(!fixed.canvas.empty());
  COMP_CHECK(comp_ui::ruler_hit(fixed, static_cast<double>(fixed.canvas.x + 200),
                                static_cast<double>(fixed.canvas.y + 6), true) == 1);
  COMP_CHECK(comp_ui::ruler_hit(fixed, static_cast<double>(fixed.canvas.x + 6),
                                static_cast<double>(fixed.canvas.y + 300), true) == 2);
  /* 左上角落在上尺里（顺序决定语义：从角上拖出水平参考线）。 */
  COMP_CHECK(comp_ui::ruler_hit(fixed, static_cast<double>(fixed.canvas.x + 6),
                                static_cast<double>(fixed.canvas.y + 6), true) == 1);
  /* 画布中央不命中；标尺隐藏时连条带也不命中。 */
  COMP_CHECK(comp_ui::ruler_hit(fixed, static_cast<double>(fixed.canvas.x + 200),
                                static_cast<double>(fixed.canvas.y + 200), true) == 0);
  COMP_CHECK(comp_ui::ruler_hit(fixed, static_cast<double>(fixed.canvas.x + 6),
                                static_cast<double>(fixed.canvas.y + 6), false) == 0);
  /* 半开区间：条带右/下边界外一像素就不算标尺。 */
  COMP_CHECK(comp_ui::ruler_hit(
                 fixed, static_cast<double>(fixed.canvas.x + 200),
                 static_cast<double>(fixed.canvas.y + comp_ui::Layout::kRulerThickness), true) == 0);
  COMP_CHECK(comp_ui::ruler_hit(
                 fixed, static_cast<double>(fixed.canvas.x + comp_ui::Layout::kRulerThickness),
                 static_cast<double>(fixed.canvas.y + 300), true) == 0);
  /* 空布局失效：不看尺寸为 0 的画布。 */
  const Layout empty{};
  COMP_CHECK(comp_ui::ruler_hit(empty, 0.0, 0.0, true) == 0);

  /* 4. 吸附半径：device_px / zoom，缩放级之间屏幕手感一致。 */
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(1.0), 8.0));
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(4.0), 2.0));
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(8.0), 1.0));
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(0.02), 400.0));
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(1.0, 4.0), 4.0));
  for (const double zoom : zooms) {
    /* 反算回设备像素必须还是 8：这就是「手感与缩放无关」的可判定口径。 */
    COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(zoom) * zoom, 8.0, 1e-9));
  }
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(0.0), 400.0));
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(-2.0), 400.0));
  COMP_CHECK(near_value(comp_ui::snap_tolerance_doc(
                            std::numeric_limits<double>::quiet_NaN()),
                        400.0));
}


/*
 * P0-3 标签栏 + P0-10 面板停靠：布局不变量。
 *
 * 这两条动的都是**全局布局**，一个写错就是「画布被面板压住」或「标签栏压住
 * 画布」，所以几何关系必须钉死，而不是靠目视。
 */
COMP_TEST(ui_core_tabbar_and_dock_layout) {
  /* 1. 单文档：标签栏高度 0，历史基线一个像素不动。 */
  comp_ui::LayoutPrefs single;
  single.tab_count = 1;
  const comp_ui::Layout one = comp_ui::compute_layout(1280, 720, single);
  COMP_CHECK(one.tabbar.h == 0);
  COMP_CHECK(one.tab_count == 0);
  COMP_CHECK(one.canvas.h == comp_ui::compute_layout(1280, 720).canvas.h);

  /* 2. 多文档：标签栏占 26px，画布高度相应变矮，且标签栏在选项栏之下。 */
  comp_ui::LayoutPrefs multi;
  multi.tab_count = 3;
  const comp_ui::Layout three = comp_ui::compute_layout(1280, 720, multi);
  COMP_CHECK(three.tabbar.h == comp_ui::Layout::kTabBarHeight);
  COMP_CHECK(three.tab_count == 3);
  COMP_CHECK(three.tabbar.y == three.options.bottom());
  COMP_CHECK(three.canvas.y == three.tabbar.bottom());
  COMP_CHECK(three.canvas.h == one.canvas.h - comp_ui::Layout::kTabBarHeight);

  /* 3. 标签矩形：等宽、互不重叠、全在标签栏内；命中与几何同源。 */
  for (int index = 0; index < 3; ++index) {
    const comp_ui::Rect tab = comp_ui::tab_rect(three, index, 3);
    COMP_CHECK(tab.w > 0);
    COMP_CHECK(tab.x >= three.tabbar.x && tab.right() <= three.tabbar.right());
    COMP_CHECK(comp_ui::tab_at(three, 3, tab.x + 1, tab.y + 2) == index);
  }
  COMP_CHECK(comp_ui::tab_rect(three, 3, 3).w == 0);
  COMP_CHECK(comp_ui::tab_at(three, 3, -5, 10) == -1);
  /* 标签栏之外（画布区）不命中。 */
  COMP_CHECK(comp_ui::tab_at(three, 3, three.canvas.x + 10, three.canvas.y + 10) == -1);

  /* 4. 停靠换边：两列交换位置，但画布宽度与位置不变（换边不该让画面跳）。 */
  comp_ui::LayoutPrefs left;
  left.adjust_dock_left = true;
  const comp_ui::Layout docked = comp_ui::compute_layout(1280, 720, left);
  const comp_ui::Layout normal = comp_ui::compute_layout(1280, 720);
  COMP_CHECK(docked.adjust_panel.x == 0);
  /* 停左 = 两列各贴一边：调整贴左缘、图层贴右缘（不是「挤在中间」）。 */
  COMP_CHECK(docked.layer_panel.right() == 1280);
  /* 画布**宽度**不变（两列宽度之和一样），但横向位置随列序移动——
   * 换边必然移动画布，否则面板会压到画布上。
   * 停左时两列各贴一边：调整贴左缘、图层贴右缘，画布在中间。 */
  COMP_CHECK(docked.canvas.w == normal.canvas.w);
  COMP_CHECK(docked.canvas.x == docked.adjust_panel.right());
  COMP_CHECK(docked.canvas.right() == docked.layer_panel.x);
  COMP_CHECK(docked.canvas.x > normal.canvas.x);
  /* 停左时分界线在面板**右**缘，停右时在左缘。 */
  COMP_CHECK(docked.adjust_splitter.right() <= docked.adjust_panel.right());
  COMP_CHECK(normal.adjust_splitter.x >= normal.adjust_panel.x);
  COMP_CHECK(docked.adjust_splitter.x != normal.adjust_splitter.x);
}

/*
 * P0-10 数字直输：选项栏的命中分区。
 *   左右箭头 = -1 / +1；中间数值区 = 0（进入键入态）。
 */
COMP_TEST(ui_core_option_value_area_hit) {
  const comp_ui::Rect bar{0, 46, 600, 32};
  const comp_ui::ToolId brush = comp_ui::ToolId::Brush;
  const int count = comp_ui::tool_option_count(brush);
  COMP_CHECK(count > 0);
  const comp_ui::Rect group = comp_ui::option_group_rect(bar, 0);
  const comp_ui::Rect control = comp_ui::option_control_rect(group);
  COMP_CHECK(control.w > 0);

  int delta = 99;
  const int left_hit = comp_ui::option_hit(bar, brush, control.x + 2, control.y + 4, &delta);
  COMP_CHECK(left_hit == 0 && delta == -1);
  delta = 99;
  const int right_hit = comp_ui::option_hit(bar, brush, control.right() - 2, control.y + 4, &delta);
  COMP_CHECK(right_hit == 0 && delta == 1);
  delta = 99;
  const int mid_hit = comp_ui::option_hit(bar, brush, control.x + control.w / 2, control.y + 4, &delta);
  COMP_CHECK(mid_hit == 0 && delta == 0);
  /* 数值区几何必须落在控制块内部（绘制高亮与命中同一处）。 */
  const comp_ui::Rect field = comp_ui::option_value_rect(control);
  COMP_CHECK(field.x > control.x && field.right() < control.right());
  COMP_CHECK(field.h == control.h);
}
