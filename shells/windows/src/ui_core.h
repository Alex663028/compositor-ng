// AI生成
/*
 * ui_core.h — Windows 外壳的「平台无关 UI 核心」。
 *
 * 为什么单独一层：Win32 消息循环、GDI 绘制和 C ABI 调用都难以在无窗口环境
 * 回归；但「画布视图变换 / 面板布局 / 命中测试 / 工具与快捷键表」是纯算术与
 * 纯数据，可以在 tests/test_ui_core.cpp 里逐条钉死。壳（main.cpp）只做
 * 「把消息翻译成这里的函数调用，再把结果画出来」。
 *
 * 设计约束：
 *   1. 不 include 任何 Windows 头、不 include 任何 engine/ 头；
 *   2. 不分配内存、不抛异常（全部 inline、无状态或值语义）；
 *   3. 工具键位逐字对齐上游 `UI/KeyboardShortcuts.swift` 的 ShortcutDefinition.all；
 *   4. 滤镜参数表与 `comp_filter.h` 的量程逐项一致，`tests/test_ui_core.cpp`
 *      把两侧数值钉死（头文件之间不能互相 include，只能靠测试对齐）。
 *
 * 坐标系（三种，壳内必须分清）：
 *   - 文档像素坐标：图层/工具算子的口径，原点在画布左上；
 *   - 客户区坐标：Win32 消息与绘制口径；
 *   - 画布视图坐标：屏幕 = 画布中心 + pan + (文档 - 文档中心) * zoom。
 */
#ifndef COMPOSITOR_SHELL_UI_CORE_H
#define COMPOSITOR_SHELL_UI_CORE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace comp_ui {

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(double px, double py) const {
    return px >= static_cast<double>(x) && py >= static_cast<double>(y) &&
           px < static_cast<double>(x + w) && py < static_cast<double>(y + h);
  }
};

struct Point {
  double x = 0.0;
  double y = 0.0;
};

inline bool near_point(Point a, Point b, double epsilon = 1e-9) {
  return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon;
}

/* ------------------------------------------------------------ 画布视图 */

/*
 * 视图变换：screen = canvas_center + pan + (doc - doc_center) * zoom。
 * 以「文档中心」为缩放锚点的好处：pan = 0 时无论缩放多少，文档始终居中。
 */
struct CanvasView {
  static constexpr double kMinZoom = 0.02; /* 2%：整张 8K 图也能塞进窗口 */
  static constexpr double kMaxZoom = 32.0; /* 3200%：与上游放大上限同量级 */

  double zoom = 1.0;
  double pan_x = 0.0;
  double pan_y = 0.0;

  static double clamp_zoom(double value) {
    if (!(value > 0.0)) return kMinZoom; /* NaN / 0 / 负数一律回落到下限 */
    return std::min(std::max(value, kMinZoom), kMaxZoom);
  }

  static Point canvas_center(const Rect& canvas) {
    return Point{static_cast<double>(canvas.x) + static_cast<double>(canvas.w) * 0.5,
                 static_cast<double>(canvas.y) + static_cast<double>(canvas.h) * 0.5};
  }

  Point doc_to_screen(double doc_x, double doc_y, const Rect& canvas, int doc_w,
                      int doc_h) const {
    const Point center = canvas_center(canvas);
    return Point{center.x + pan_x + (doc_x - static_cast<double>(doc_w) * 0.5) * zoom,
                 center.y + pan_y + (doc_y - static_cast<double>(doc_h) * 0.5) * zoom};
  }

  Point screen_to_doc(double screen_x, double screen_y, const Rect& canvas, int doc_w,
                      int doc_h) const {
    const Point center = canvas_center(canvas);
    const double safe_zoom = zoom > 0.0 ? zoom : kMinZoom;
    return Point{(screen_x - center.x - pan_x) / safe_zoom + static_cast<double>(doc_w) * 0.5,
                 (screen_y - center.y - pan_y) / safe_zoom + static_cast<double>(doc_h) * 0.5};
  }

  /*
   * 以屏幕点 (screen_x, screen_y) 为锚缩放：缩放前后该屏幕点下方的文档像素
   * 保持不动。new_zoom 会先夹到 [kMinZoom, kMaxZoom]，夹取后的值参与计算，
   * 因此「已在上限再放大」不会偷偷挪动画布。
   */
  void zoom_at(double new_zoom, double screen_x, double screen_y, const Rect& canvas) {
    const double target = clamp_zoom(new_zoom);
    if (!(zoom > 0.0)) {
      zoom = target;
      pan_x = 0.0;
      pan_y = 0.0;
      return;
    }
    const double ratio = target / zoom;
    const Point center = canvas_center(canvas);
    pan_x = pan_x * ratio + (screen_x - center.x) * (1.0 - ratio);
    pan_y = pan_y * ratio + (screen_y - center.y) * (1.0 - ratio);
    zoom = target;
  }

  void zoom_by(double factor, double screen_x, double screen_y, const Rect& canvas) {
    if (!(factor > 0.0)) return;
    zoom_at(zoom * factor, screen_x, screen_y, canvas);
  }

  /* 适配窗口：留 margin 边距，保持宽高比，回到居中。 */
  void fit(int doc_w, int doc_h, const Rect& canvas, double margin = 0.92) {
    if (doc_w <= 0 || doc_h <= 0 || canvas.empty()) {
      zoom = 1.0;
      pan_x = 0.0;
      pan_y = 0.0;
      return;
    }
    const double scale_x = static_cast<double>(canvas.w) * margin / static_cast<double>(doc_w);
    const double scale_y = static_cast<double>(canvas.h) * margin / static_cast<double>(doc_h);
    zoom = clamp_zoom(std::min(scale_x, scale_y));
    pan_x = 0.0;
    pan_y = 0.0;
  }

  /*
   * 缩放到文档矩形（Z 拖框）：矩形按 margin 边距铺满画布，矩形中心移到画布
   * 中心（与 fit 的区别是只关心这一块，而不是整幅文档）。
   * 传入的两点先按左上/右下归一化；宽或高不为正（单点点击、零宽拖动）返回
   * false，调用方回落到「点一下放大 20%」。缩放会夹到 [kMinZoom, kMaxZoom]，
   * 夹取后仍然居中矩形中心。
   */
  bool zoom_to_doc_rect(double x0, double y0, double x1, double y1, const Rect& canvas,
                        int doc_w, int doc_h, double margin = 0.98) {
    if (canvas.empty()) return false;
    const double left = std::min(x0, x1);
    const double top = std::min(y0, y1);
    const double right = std::max(x0, x1);
    const double bottom = std::max(y0, y1);
    const double rect_w = right - left;
    const double rect_h = bottom - top;
    if (!(rect_w > 0.0) || !(rect_h > 0.0)) return false;
    const double scale_x = static_cast<double>(canvas.w) * margin / rect_w;
    const double scale_y = static_cast<double>(canvas.h) * margin / rect_h;
    zoom = clamp_zoom(std::min(scale_x, scale_y));
    pan_x = -((left + right) * 0.5 - static_cast<double>(doc_w) * 0.5) * zoom;
    pan_y = -((top + bottom) * 0.5 - static_cast<double>(doc_h) * 0.5) * zoom;
    return true;
  }

  void actual_pixels() {
    zoom = 1.0;
    pan_x = 0.0;
    pan_y = 0.0;
  }

  void pan_by(double dx, double dy) {
    pan_x += dx;
    pan_y += dy;
  }
};

/* ------------------------------------------------------------ 布局 */

/*
 * 布局：顶部工具条、左侧图层面板、右侧调整面板、底部状态栏，其余为画布。
 * 窄窗口优先收起右侧面板，再收起左侧面板，保证画布至少 kMinCanvasWidth，
 * 任何尺寸下矩形都不出现负宽高。
 *
 * 面板宽度可由用户拖动（§6.60）：`LayoutPrefs` 装用户偏好，`compute_layout`
 * 仍然负责「放不放得下」。收起规则与固定宽度时代同一套口径——按**偏好宽度**
 * 判断放不放得下，放不下就整块收起，不做「挤成 120px 的面板」：宽度不对的
 * 面板比没有面板更难用（层名截断到 4 个字、滑杆只剩刻度）。
 */
struct Layout {
  static constexpr int kToolbarHeight = 46;
  /*
   * §5.49 工具选项栏（P0-10）：工具条下的一条参数横栏。参数只有「当前工具」这一维，
   * 所以它放在工具条正下方而不是右栏——改画笔大小不该先切面板。
   */
  static constexpr int kOptionsHeight = 32;
  /*
   * 选项栏的收起阈值：窗口还得给「面板/画布主体」留够这么多高度才显示选项栏。
   * 不留阈值的话，矮窗口里选项栏会先于面板被挤扁——面板至少还能滚，32px 的
   * 一条横栏挤扁后连一行字都放不下，只会把画布往下推。
   */
  static constexpr int kMinBodyHeight = 96;
  static constexpr int kStatusHeight = 26;
  static constexpr int kLayerPanelWidth = 248;
  static constexpr int kAdjustPanelWidth = 330;
  static constexpr int kMinCanvasWidth = 180;
  /* 拖动范围（§6.60）：下限=还能看清层名/滑杆，上限=不能让画布消失。 */
  static constexpr int kLayerPanelMinWidth = 168;
  static constexpr int kLayerPanelMaxWidth = 520;
  static constexpr int kAdjustPanelMinWidth = 240;
  static constexpr int kAdjustPanelMaxWidth = 560;
  /*
   * 分界线命中带的宽度：整条带子落在**面板内侧**（图层面板取最右 5px，调整
   * 面板取最左 5px），不吃画布像素。画布边缘是用户最常落笔的地方之一，把
   * 那里的 3px 抢去做缩放会让「画到边上」变成「面板变宽」——这类误触找起来
   * 比点不准行更难。代价是面板最后 5px 内的点击用于拉宽，面板内容不会画到那里。
   */
  static constexpr int kPanelSplitterWidth = 5;
  /*
   * §5.43 面板整块收起（停靠最小版）：收起后只剩一条 24px 的竖条，点一下展开。
   * 收起条比任何面板下限都窄，但**仍占布局宽度**——它必须和展开态走同一套
   * 「三块横向铺满」的算式，否则收起/展开时画布宽度会跳 24px，而 pan 补偿
   * 是按画布中心算的，中心与宽度同时变会让画面看起来「闪一下」。
   */
  static constexpr int kCollapsedPanelWidth = 24;
  /*
   * 展开态的收起按钮：落在面板标题行右端（18×18），离右边缘 8px——分界线占
   * 最右 5px，两者留 3px 间隔，命中区永不重叠（重叠时「拖宽」会吃掉「收起」）。
   */
  static constexpr int kPanelToggleSize = 18;
  static constexpr int kPanelToggleRightMargin = 8;
  static constexpr int kPanelToggleTop = 6;
  /*
   * S22 直方图面板：叠在图层面板顶部（不新增一栏，否则 248px 的图层栏会被
   * 压到无法读层名）。高度是排版密度，不参与命中测试——它纯展示、不可点。
   */
  static constexpr int kHistogramHeight = 64;
  /* 绘图区比整块高度小一圈，留给标题行；折线本身画在这块里。 */
  static constexpr int kHistogramPlotHeight = 40;
  /*
   * ROI #3 标尺条：画在**画布内侧**（上边 / 左边各 18px），不从布局里切走一条带。
   * 画布矩形是缩放居中、pan 补偿与文档像素换算的唯一锚点，切走 18px 会让所有
   * 既有命中测试与视觉基线整体偏移 18px。代价是标尺盖住画布最外 18px（放大时
   * 正好是文档边缘），所以标尺默认关闭，用户要看的时候才开。
   */
  static constexpr int kRulerThickness = 18;

  Rect toolbar{};
  Rect options{}; /* §5.49 工具选项栏；高度不够时为空矩形 */
  Rect layer_panel{};
  Rect adjust_panel{};
  Rect canvas{};
  Rect status{};
  Rect histogram{}; /* S22：图层面板顶部的直方图区域；未启用时为空矩形 */
  /* 面板分界线命中带（§6.60）：面板收起时为空矩形，命中测试因此自动失效。 */
  Rect layer_splitter{};
  Rect adjust_splitter{};
  /*
   * §5.43 收起/展开按钮。展开态 = 标题行右端的小方块；整块收起时 = 整条竖条
   * （点条上任意位置都展开，不必去瞄 18px 的小方块）。面板未显示时为空矩形。
   */
  Rect layer_toggle{};
  Rect adjust_toggle{};
  bool layer_collapsed = false;
  bool adjust_collapsed = false;
};

/* 面板宽度偏好（§6.60）：用户拖到哪儿就记到哪儿，跨窗口尺寸沿用。 */
struct LayoutPrefs {
  int layer_panel_width = Layout::kLayerPanelWidth;
  int adjust_panel_width = Layout::kAdjustPanelWidth;
  /*
   * §5.43：整块收起。收起不改偏好宽度——展开回来必须是用户拖出来的宽度，
   * 而不是缺省值（宽度偏好是会话状态，收起只是它的一个显示开关）。
   */
  bool layer_panel_collapsed = false;
  bool adjust_panel_collapsed = false;
};

/* 宽度是整数像素，不存在 NaN 输入，夹取只需一次 min/max。 */
inline int clamp_layer_panel_width(int width) {
  return std::min(std::max(width, Layout::kLayerPanelMinWidth), Layout::kLayerPanelMaxWidth);
}

inline int clamp_adjust_panel_width(int width) {
  return std::min(std::max(width, Layout::kAdjustPanelMinWidth), Layout::kAdjustPanelMaxWidth);
}

/*
 * 拖动换算：图层面板在画布左侧，向右拖 = 变宽；调整面板在右侧，向左拖 = 变宽。
 * 取整后再夹取，夹取只在**结果**上做一次——先夹基准再加位移会让拖动到边界后
 * 反向拖回时出现一段「死区」（位移得先抵消掉溢出量，面板才开始动）。
 */
inline int layer_panel_width_from_drag(int base_width, double dx) {
  return clamp_layer_panel_width(base_width + static_cast<int>(std::lround(dx)));
}

inline int adjust_panel_width_from_drag(int base_width, double dx) {
  return clamp_adjust_panel_width(base_width - static_cast<int>(std::lround(dx)));
}

/* 展开态切换按钮的几何：标题行右端，与分界线留 3px（见 Layout 注释）。 */
inline Rect panel_toggle_rect(const Rect& panel) {
  return Rect{panel.right() - Layout::kPanelToggleRightMargin - Layout::kPanelToggleSize,
              panel.y + Layout::kPanelToggleTop, Layout::kPanelToggleSize,
              Layout::kPanelToggleSize};
}

inline Layout compute_layout(int width, int height, const LayoutPrefs& prefs) {
  if (width < 0) width = 0;
  if (height < 0) height = 0;

  const int toolbar_h = std::min(height, Layout::kToolbarHeight);
  const int status_h = std::min(height - toolbar_h, Layout::kStatusHeight);
  /*
   * §5.49：选项栏只在「工具栏 + 选项栏 + 主体下限 + 状态栏」都放得下时占位。
   * 收起时高度直接记 0 并返回空矩形，命中测试因此自动失效（与分界线同一口径），
   * 画布回收这 32px，不会留一条空白带。
   */
  const int remainder_h = height - toolbar_h - status_h;
  const int options_h = remainder_h >= Layout::kOptionsHeight + Layout::kMinBodyHeight
                            ? Layout::kOptionsHeight
                            : 0;
  const int body_top = toolbar_h + options_h;
  const int body_h = remainder_h - options_h;

  /*
   * §5.43：收起态只把「想要的宽度」换成 24px 竖条，**收起口径不变**——仍按
   * 「想要的宽度 + 画布下限」判断放不放得下，放不下就整块不给。这样窄窗不会
   * 出现「一条 24px 的竖条把画布挤到 179px」的破例。
   */
  const int layer_wanted = prefs.layer_panel_collapsed
                               ? Layout::kCollapsedPanelWidth
                               : clamp_layer_panel_width(prefs.layer_panel_width);
  int layer_w = 0;
  if (width >= layer_wanted + Layout::kMinCanvasWidth) {
    layer_w = layer_wanted;
  }
  const int adjust_wanted = prefs.adjust_panel_collapsed
                                ? Layout::kCollapsedPanelWidth
                                : clamp_adjust_panel_width(prefs.adjust_panel_width);
  int adjust_w = 0;
  if (width - layer_w >= adjust_wanted + Layout::kMinCanvasWidth) {
    adjust_w = adjust_wanted;
  }

  Layout out;
  out.toolbar = Rect{0, 0, width, toolbar_h};
  out.options = Rect{0, toolbar_h, width, options_h};
  out.layer_panel = Rect{0, body_top, layer_w, body_h};
  out.adjust_panel = Rect{width - adjust_w, body_top, adjust_w, body_h};
  out.canvas = Rect{layer_w, body_top, width - layer_w - adjust_w, body_h};
  out.status = Rect{0, body_top + body_h, width, status_h};
  out.layer_collapsed = layer_w > 0 && prefs.layer_panel_collapsed;
  out.adjust_collapsed = adjust_w > 0 && prefs.adjust_panel_collapsed;
  /*
   * 收起条不提供拖宽：它只有 24px，拖动命中带（最右 5px）几乎占满整条，用户
   * 想在哪儿点「展开」都会变成「拉宽」。展开后分界线立刻恢复。
   */
  if (layer_w > 0 && !out.layer_collapsed) {
    out.layer_splitter = Rect{layer_w - Layout::kPanelSplitterWidth, body_top,
                              Layout::kPanelSplitterWidth, body_h};
  }
  if (adjust_w > 0 && !out.adjust_collapsed) {
    out.adjust_splitter = Rect{width - adjust_w, body_top, Layout::kPanelSplitterWidth, body_h};
  }
  if (layer_w > 0) {
    out.layer_toggle = out.layer_collapsed ? out.layer_panel : panel_toggle_rect(out.layer_panel);
  }
  if (adjust_w > 0) {
    out.adjust_toggle =
        out.adjust_collapsed ? out.adjust_panel : panel_toggle_rect(out.adjust_panel);
  }
  return out;
}

inline Layout compute_layout(int width, int height) {
  return compute_layout(width, height, LayoutPrefs{});
}

/* 分界线命中：0 = 未命中，1 = 图层面板分界线，2 = 调整面板分界线。 */
inline int panel_splitter_at(const Layout& layout, double x, double y) {
  if (layout.layer_splitter.contains(x, y)) return 1;
  if (layout.adjust_splitter.contains(x, y)) return 2;
  return 0;
}

/* 收起/展开命中：0 = 未命中，1 = 图层面板，2 = 调整面板（§5.43）。 */
inline int panel_toggle_at(const Layout& layout, double x, double y) {
  if (layout.layer_toggle.contains(x, y)) return 1;
  if (layout.adjust_toggle.contains(x, y)) return 2;
  return 0;
}

/* ------------------------------------------------------------ 标尺 / 吸附 */

inline Rect ruler_top_rect(const Rect& canvas) {
  return Rect{canvas.x, canvas.y, canvas.w, Layout::kRulerThickness};
}

inline Rect ruler_left_rect(const Rect& canvas) {
  return Rect{canvas.x, canvas.y, Layout::kRulerThickness, canvas.h};
}

/* 左上角交叉块：两把尺子共用，画在最上层，盖住刻度起点。 */
inline Rect ruler_corner_rect(const Rect& canvas) {
  return Rect{canvas.x, canvas.y, Layout::kRulerThickness, Layout::kRulerThickness};
}

/*
 * 主刻度步长（文档像素）：取 1-2-5 序列里第一个「屏幕间距 >= 56px」的值。
 * 固定 56px 是标签宽度（最多 7 位数 + 边距）的经验下限：只要屏幕间距不小于它，
 * 标签就永不相撞；缩放越大，命中的步长越细，读数精度自动跟着变。
 */
inline double ruler_major_step(double zoom) {
  const double safe = (zoom > 0.0) ? zoom : CanvasView::kMinZoom;
  static const double kSteps[] = {1.0,   2.0,   5.0,    10.0,   20.0,   50.0,   100.0,
                                  200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0};
  for (double step : kSteps) {
    if (step * safe >= 56.0) return step;
  }
  return kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
}

/*
 * 标尺命中：0 = 未命中，1 = 上尺（拖出水平参考线），2 = 左尺（拖出垂直参考线）。
 * 标尺没显示时整块失效——看不见的尺子不能抢画布最外 18px 的点击。
 * 左上角落在上尺之内，因此从角上拖出的是水平参考线（与上游一致）。
 */
inline int ruler_hit(const Layout& layout, double x, double y, bool rulers_visible) {
  if (!rulers_visible || layout.canvas.empty()) return 0;
  if (ruler_top_rect(layout.canvas).contains(x, y)) return 1;
  if (ruler_left_rect(layout.canvas).contains(x, y)) return 2;
  return 0;
}

/*
 * 吸附半径：以**设备像素**给出再换算回文档像素。若直接用文档像素，800% 时
 * 8 文档像素在屏幕上是 64px（隔着半个画布都在吸），2% 时又不到 1 个设备像素
 * （永远吸不上）。按设备像素给，各缩放级手感一致。
 */
inline double snap_tolerance_doc(double zoom, double device_px = 8.0) {
  const double safe = (zoom > 0.0) ? zoom : CanvasView::kMinZoom;
  return device_px / safe;
}

/* ------------------------------------------------------------ 工具条 */

inline constexpr int kToolButtonSize = 32;
inline constexpr int kToolButtonGap = 6;
inline constexpr int kToolButtonLeft = 10;

inline Rect tool_button_rect(const Rect& toolbar, int index) {
  const int stride = kToolButtonSize + kToolButtonGap;
  return Rect{toolbar.x + kToolButtonLeft + index * stride,
              toolbar.y + (toolbar.h - kToolButtonSize) / 2, kToolButtonSize,
              kToolButtonSize};
}

/* 命中工具按钮；返回按钮序号，未命中返回 -1。 */
inline int tool_button_at(const Rect& toolbar, int button_count, double x, double y) {
  if (!toolbar.contains(x, y)) return -1;
  for (int i = 0; i < button_count; ++i) {
    if (tool_button_rect(toolbar, i).contains(x, y)) return i;
  }
  return -1;
}

/* ------------------------------------------------------------ 图层面板 */

inline constexpr int kLayerHeaderHeight = 30;
inline constexpr int kLayerRowHeight = 22;

/*
 * 顶部偏移版（S22）：直方图面板打开时，图层行整体下移 kHistogramHeight。
 * 偏移必须与绘制共用（layer_row_rect 的 row_top 参数），否则「点中的行」与
 * 「画出来的行」会错开 kHistogramHeight——这类偏移不一致是最典型的「点不准」缺陷。
 */
inline int layer_row_at(const Rect& panel, int layer_count, double x, double y, int row_top) {
  if (layer_count <= 0 || !panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kLayerHeaderHeight + row_top);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kLayerRowHeight)));
  if (row < 0 || row >= layer_count) return -1;
  return row;
}

/* 面板显示序：第 0 行 = 最上层图层（与引擎数组序相反，由调用方换算）。 */
inline int layer_row_at(const Rect& panel, int layer_count, double x, double y) {
  return layer_row_at(panel, layer_count, x, y, 0);
}

inline Rect layer_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kLayerHeaderHeight + row * kLayerRowHeight, panel.w,
              kLayerRowHeight};
}

inline Rect layer_row_rect(const Rect& panel, int row, int row_top) {
  return Rect{panel.x, panel.y + kLayerHeaderHeight + row_top + row * kLayerRowHeight, panel.w,
              kLayerRowHeight};
}

/* ------------------------------------------------------------ 工具与快捷键 */

/* 工具集合与顺序 = 工具条从左到右的显示顺序。 */
enum class ToolId : int {
  Select = 0,
  Move,
  Marquee,
  Lasso,
  Pen, /* 钢笔路径选区：点击落锚点、拖动拉控制柄 */
  Wand,
  Crop,
  Eyedropper,
  Brush,
  Eraser,
  Heal,
  Clone,
  Retouch, /* 上游 "Blur / Smudge / Liquify"，同一个 R 键循环 */
  Tone,    /* 上游 "Dodge / Burn / Sponge"，同一个 O 键循环 */
  Gradient,
  Shape,
  Type,
  Hand,
  Zoom,
  HistoryBrush, /* P1：从历史面板的某个状态取样，随笔迹刷回当前层 */
  Count
};

struct ToolInfo {
  ToolId id;
  const char* name; /* 英文名，与上游菜单一致；外壳负责转宽字符 */
  char shortcut;    /* 小写 ASCII；0 = 无默认键 */
};

inline const ToolInfo* tool_table(size_t* out_count) {
  static const ToolInfo kTools[] = {
      {ToolId::Select, "Select", 'a'},
      {ToolId::Move, "Move/Transform", 'v'},
      {ToolId::Marquee, "Marquee", 'm'},
      {ToolId::Lasso, "Lasso", 'l'},
      {ToolId::Pen, "Pen", 'p'},
      {ToolId::Wand, "Magic Wand", 'w'},
      {ToolId::Crop, "Crop", 'c'},
      {ToolId::Eyedropper, "Eyedropper", 'i'},
      {ToolId::Brush, "Brush", 'b'},
      {ToolId::Eraser, "Eraser", 'e'},
      {ToolId::Heal, "Spot Healing", 'j'},
      {ToolId::Clone, "Clone Stamp", 's'},
      {ToolId::Retouch, "Blur/Smudge/Liquify", 'r'},
      {ToolId::Tone, "Dodge/Burn/Sponge", 'o'},
      {ToolId::Gradient, "Gradient", 'g'},
      {ToolId::Shape, "Shape", 'u'},
      {ToolId::Type, "Type", 't'},
      {ToolId::Hand, "Hand", 'h'},
      {ToolId::Zoom, "Zoom", 'z'},
      /* P1：Y 与上游 History Brush 一致；追加在末尾，旧工具的下标不移动。 */
      {ToolId::HistoryBrush, "History Brush", 'y'},
  };
  if (out_count != nullptr) {
    *out_count = sizeof(kTools) / sizeof(kTools[0]);
  }
  return kTools;
}

inline int tool_count() {
  size_t count = 0;
  tool_table(&count);
  return static_cast<int>(count);
}

inline const ToolInfo* tool_info_at(int index) {
  size_t count = 0;
  const ToolInfo* table = tool_table(&count);
  if (index < 0 || static_cast<size_t>(index) >= count) return nullptr;
  return &table[index];
}

inline int tool_index(ToolId id) {
  size_t count = 0;
  const ToolInfo* table = tool_table(&count);
  for (size_t i = 0; i < count; ++i) {
    if (table[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

inline const ToolInfo* tool_info(ToolId id) {
  return tool_info_at(tool_index(id));
}

/*
 * 哪些工具需要画「笔尖轮廓」。Brush / Eraser / Spot Healing / Clone Stamp 与
 * Retouch 族（Blur / Smudge / Liquify）、Tone 族（Dodge / Burn / Sponge）、
 * 历史记录画笔都吃 `brush_size`，画同一个圆；选区类工具不吃笔刷直径，不画
 * （避免误导）。
 */
inline bool tool_has_brush_tip(ToolId id) {
  switch (id) {
    case ToolId::Brush:
    case ToolId::Eraser:
    case ToolId::Heal:
    case ToolId::Clone:
    case ToolId::Retouch:
    case ToolId::Tone:
    case ToolId::HistoryBrush:
      return true;
    default:
      return false;
  }
}

/* 键位 -> 工具；key 必须是小写 ASCII，未命中返回 ToolId::Count。 */
inline ToolId tool_for_shortcut(char key) {
  if (key >= 'A' && key <= 'Z') key = static_cast<char>(key - 'A' + 'a');
  size_t count = 0;
  const ToolInfo* table = tool_table(&count);
  for (size_t i = 0; i < count; ++i) {
    if (table[i].shortcut != 0 && table[i].shortcut == key) return table[i].id;
  }
  return ToolId::Count;
}

/*
 * 工具键位的实际落点：E 在 Brush / Eraser 之间来回切换。
 *
 * 上游画笔与橡皮擦共用同一支笔的参数（直径/硬度/压力/流量），只有合成方向相反，
 * 因此同一个键位在两个模式间切换；当前不是这两者时一律落到 Eraser（与
 * tool_table 的键位一致）。其余键位直接查表，本函数是唯一入口。
 */
inline ToolId tool_shortcut_target(ToolId current, char key) {
  if (key >= 'A' && key <= 'Z') key = static_cast<char>(key - 'A' + 'a');
  if (key == 'e') {
    if (current == ToolId::Brush) return ToolId::Eraser;
    if (current == ToolId::Eraser) return ToolId::Brush;
    return ToolId::Eraser;
  }
  return tool_for_shortcut(key);
}

/*
 * 画布上进行中的笔迹种类。Retouch 族（Blur / Smudge / Liquify）与画笔/橡皮擦
 * 共用同一条「鼠标折线 -> comp_tool_apply」通路，但提交时装配的参数不同，
 * 因此状态里必须记住这一笔属于哪一族。
 */
enum class StrokeKind : int {
  None = 0,
  Retouch,      /* COMP_TOOL_BLUR / SMUDGE / LIQUIFY */
  Tone,         /* COMP_TOOL_DODGE / BURN / SPONGE */
  Paint,        /* COMP_TOOL_STROKE，erase 字段区分画笔与橡皮擦 */
  HistoryBrush, /* COMP_TOOL_HISTORY_BRUSH，取样状态来自历史面板 */
  Clone,        /* COMP_TOOL_CLONE，源点/对齐偏移由壳自己保存 */
  Heal          /* COMP_TOOL_HEAL，coverage 由壳按笔迹折线自己光栅化 */
};

/*
 * 仿制图章的首笔偏移（文档像素）。
 *
 * 引擎在 `has_offset == 0` 时按 `round(source - first_point)` 现算，并把结果
 * 用在这一笔里；ABI 的 `comp_tool_apply` 是 const desc，宿主拿不到回填值，
 * 所以宿主必须自己算出同一组整数，才能在后续笔画里以 `has_offset = 1` 复用
 * （这就是「对齐」：同一会话的源点相对位置固定）。
 *
 * 必须用 `std::lround`：它四舍五入且 .5 远离零，与引擎的 `round(...)` 一致；
 * `static_cast<int>` 是截断，正负半像素都会差一格，症状是后续笔画整体错半像素
 * 且很难看出是哪一笔开始错的。
 */
inline void clone_stroke_offset(double source_x, double source_y, double first_x,
                                double first_y, double* offset_x, double* offset_y) {
  if (offset_x != nullptr) {
    *offset_x = static_cast<double>(std::lround(source_x - first_x));
  }
  if (offset_y != nullptr) {
    *offset_y = static_cast<double>(std::lround(source_y - first_y));
  }
}

/* ------------------------------------------------------------ 笔刷状态 */

/* 直径范围对齐上游笔刷滑杆的量级；[ / ] 用几何步进（手感等比）。 */
inline constexpr double kMinBrushSize = 1.0;
inline constexpr double kMaxBrushSize = 500.0;

inline double clamp_brush_size(double value) {
  if (!(value > 0.0)) return kMinBrushSize;
  return std::min(std::max(value, kMinBrushSize), kMaxBrushSize);
}

inline double clamp_brush_hardness(double value) {
  if (!(value >= 0.0)) return 0.0; /* NaN 也回落到 0 */
  return std::min(std::max(value, 0.0), 1.0);
}

inline double step_brush_size(double current, int direction) {
  const double base = clamp_brush_size(current);
  if (direction > 0) return clamp_brush_size(base * 1.2);
  if (direction < 0) return clamp_brush_size(base / 1.2);
  return base;
}

inline double step_brush_hardness(double current, int direction) {
  const double base = clamp_brush_hardness(current);
  const double next = base + (direction > 0 ? 0.1 : (direction < 0 ? -0.1 : 0.0));
  return clamp_brush_hardness(next);
}

/*
 * 流量（P1-4）：笔迹算子把 `flow` 当每 dab 的**覆盖面峰值**（与 PS 的「流量」
 * 同义）。`comp_tool_op_desc` 顶层还有 opacity，但 COMP_TOOL_STROKE 不读它，
 * 所以画笔/橡皮擦只暴露流量，不造一个改了没反应的滑杆。
 *
 * 下限 0.01 而不是 0：flow = 0 时整笔一个像素都不落，用户会当成「画笔坏了」；
 * PS 的流量滑杆也停在一个可见的最小档。
 */
inline constexpr double kMinBrushFlow = 0.01;
inline constexpr double kMaxBrushFlow = 1.0;

inline double clamp_brush_flow(double value) {
  if (!(value >= 0.0)) return kMinBrushFlow; /* NaN / 负数 -> 下限 */
  return std::min(std::max(value, kMinBrushFlow), kMaxBrushFlow);
}

inline double step_brush_flow(double current, int direction) {
  const double base = clamp_brush_flow(current);
  const double next = base + (direction > 0 ? 0.05 : (direction < 0 ? -0.05 : 0.0));
  return clamp_brush_flow(next);
}

/*
 * 画笔预设（P1-4）：与渐变预设同一做法——外壳没有预设管理器，只有一张
 * **只读表**：选中即把三个值写回 ShellState，不保存副本（因此手改任一值后
 * 预设名会退回「自定义」，见 brush_preset_is_custom 的调用方）。
 * 0 号是「自定义」：它不覆盖任何值，只是当前状态的显示名。
 */
inline constexpr int kBrushPresetCount = 5;

struct BrushPresetSpec {
  const char* name = "";
  double size = 24.0;
  double hardness = 0.75;
  double flow = 1.0;
};

inline BrushPresetSpec brush_preset_spec(int preset) {
  switch (preset) {
    case 1: return BrushPresetSpec{"硬边圆", 24.0, 1.0, 1.0};
    case 2: return BrushPresetSpec{"软边圆", 80.0, 0.0, 1.0};
    case 3: return BrushPresetSpec{"喷枪", 160.0, 0.25, 0.35};
    case 4: return BrushPresetSpec{"细节", 5.0, 1.0, 1.0};
    default: return BrushPresetSpec{"自定义", 24.0, 0.75, 1.0}; /* 0 与越界 */
  }
}

inline const char* brush_preset_name(int preset) { return brush_preset_spec(preset).name; }

inline bool brush_preset_is_custom(int preset) { return preset <= 0; }

/*
 * 笔压（P1-4）：Win32 的 POINTER_PEN_INFO.pressure 是 0..1024 的整数，且只有
 * `penMask` 里带 PEN_MASK_PRESSURE 时才有效。没有该位（鼠标、不支持压感的笔、
 * 远程会话）一律按满笔压 1.0——鼠标路径因此**逐字节不变**。
 */
inline double pen_pressure_to_unit(uint32_t raw, bool has_pressure) {
  if (!has_pressure) return 1.0;
  const double value = static_cast<double>(raw) / 1024.0;
  return std::min(std::max(value, 0.0), 1.0);
}

/*
 * 一笔只带一个 brush.pressure（ABI 是常量，不是逐点数组），所以整笔取
 * WM_POINTER 采样的平均；一个采样都没有 = 1.0。
 */
inline double average_stroke_pressure(double sum, int count) {
  if (count <= 0 || !(sum >= 0.0)) return 1.0;
  return std::min(std::max(sum / static_cast<double>(count), 0.0), 1.0);
}

/* ------------------------------------------------------------ 前景色 */

/*
 * 前景色：8 位直通 sRGB，与 `comp_tool_brush.color` 同口径（不是预乘，
 * 也不是浮点）。色相面板用 HSV（上游叫 HSB）：h 0..360 度，s/v 0..1。
 *
 * 三条表示之间的换算都在这里做完，壳（main.cpp）只做取值与显示：
 *   - RGB <-> HSV：画笔面板的角度/百分比读数；
 *   - RGB <-> hex：状态栏显示与 Shift+X 的键入；
 *   - 色板：固定 12 色的常用锚点，X 键循环。
 */
struct ColorRGB {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

struct ColorHSV {
  double h = 0.0; /* 0..360；0 与 360 都是红 */
  double s = 0.0; /* 0..1 */
  double v = 0.0; /* 0..1 */
};

inline bool color_equal(ColorRGB a, ColorRGB b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* 0..255 的任意实数 -> 通道字节；NaN 回落到 0，越界夹取。 */
inline uint8_t color_channel(double value) {
  if (!(value > 0.0)) return 0;
  if (value >= 255.0) return 255;
  return static_cast<uint8_t>(std::lround(value));
}

inline ColorRGB make_rgb(int r, int g, int b) {
  return ColorRGB{color_channel(static_cast<double>(r)), color_channel(static_cast<double>(g)),
                  color_channel(static_cast<double>(b))};
}

/* 直通 sRGB 0..1 -> 8 位。吸管（comp_eyedropper_sample）的输出就是前者的口径。 */
inline ColorRGB color_from_unit(double r, double g, double b) {
  return ColorRGB{color_channel(r * 255.0), color_channel(g * 255.0),
                  color_channel(b * 255.0)};
}

inline double color_to_unit(uint8_t channel) {
  return static_cast<double>(channel) / 255.0;
}

inline ColorHSV rgb_to_hsv(ColorRGB color) {
  const double r = color_to_unit(color.r);
  const double g = color_to_unit(color.g);
  const double b = color_to_unit(color.b);
  const double high = std::max(std::max(r, g), b);
  const double low = std::min(std::min(r, g), b);
  const double delta = high - low;

  ColorHSV out;
  out.v = high;
  out.s = high > 0.0 ? delta / high : 0.0;
  if (!(delta > 0.0)) return out; /* 灰阶没有色相：约定为 0 */

  double h = 0.0;
  if (high == r) {
    h = 60.0 * ((g - b) / delta);
  } else if (high == g) {
    h = 60.0 * ((b - r) / delta + 2.0);
  } else {
    h = 60.0 * ((r - g) / delta + 4.0);
  }
  if (h < 0.0) h += 360.0;
  out.h = h;
  return out;
}

inline ColorRGB hsv_to_rgb(ColorHSV hsv) {
  double h = std::fmod(hsv.h, 360.0);
  if (h < 0.0) h += 360.0;
  const double s = clamp_brush_hardness(hsv.s); /* 复用 0..1 夹取（NaN -> 0） */
  const double v = clamp_brush_hardness(hsv.v);

  const double c = v * s;
  const double sector = h / 60.0;
  const double x = c * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
  double r = 0.0, g = 0.0, b = 0.0;
  switch (static_cast<int>(sector)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break; /* 300..360 度：品红 -> 红 */
  }
  const double m = v - c;
  return color_from_unit(r + m, g + m, b + m);
}

/* 写 "#RRGGBB"（7 字符 + '\0'）。大写十六进制，状态栏与键入回显共用。 */
inline void color_to_hex(ColorRGB color, char out[8]) {
  static const char kDigits[] = "0123456789ABCDEF";
  out[0] = '#';
  out[1] = kDigits[(color.r >> 4) & 0x0F];
  out[2] = kDigits[color.r & 0x0F];
  out[3] = kDigits[(color.g >> 4) & 0x0F];
  out[4] = kDigits[color.g & 0x0F];
  out[5] = kDigits[(color.b >> 4) & 0x0F];
  out[6] = kDigits[color.b & 0x0F];
  out[7] = '\0';
}

/*
 * 解析 "#RRGGBB" / "RRGGBB"（大小写皆可），并接受 3 位缩写 "#abc" -> "#AABBCC"。
 * 只接受整串：前后有空白或多余字符都算失败（键入路径宁可让用户改，不静默截断）。
 */
inline bool color_parse_hex(const char* text, ColorRGB* out) {
  if (text == nullptr || out == nullptr) return false;
  if (text[0] == '#') ++text;
  size_t length = 0;
  while (text[length] != '\0') ++length;
  if (length != 3 && length != 6) return false;

  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const auto pair = [&](size_t index, int* value) -> bool {
    int high = 0;
    int low = 0;
    if (length == 3) {
      high = digit(text[index]);
      low = high;
    } else {
      high = digit(text[index * 2]);
      low = digit(text[index * 2 + 1]);
    }
    if (high < 0 || low < 0) return false;
    *value = high * 16 + low;
    return true;
  };

  int r = 0, g = 0, b = 0;
  if (!pair(0, &r) || !pair(1, &g) || !pair(2, &b)) return false;
  *out = ColorRGB{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
  return true;
}

/* 色板：黑、白、灰、红、橙、黄、绿、青、蓝、紫、品红、褐。 */
inline const ColorRGB* swatch_table(size_t* out_count) {
  static const ColorRGB kSwatches[] = {
      {0, 0, 0},       {255, 255, 255}, {127, 127, 127}, {255, 0, 0},
      {255, 128, 0},   {255, 255, 0},   {0, 160, 0},     {0, 255, 255},
      {0, 90, 255},    {128, 0, 255},   {255, 0, 255},   {139, 69, 19},
  };
  if (out_count != nullptr) *out_count = sizeof(kSwatches) / sizeof(kSwatches[0]);
  return kSwatches;
}

inline int swatch_count() {
  size_t count = 0;
  swatch_table(&count);
  return static_cast<int>(count);
}

/* 越界回落到黑色（与 ColorRGB 的缺省一致），不返回 nullptr。 */
inline ColorRGB swatch_at(int index) {
  size_t count = 0;
  const ColorRGB* table = swatch_table(&count);
  if (index < 0 || static_cast<size_t>(index) >= count) return ColorRGB{};
  return table[index];
}

/* 色板循环（X / Shift+X）：返回 [0, count) 内的下一个索引。 */
inline int step_swatch(int index, int direction) {
  const int count = swatch_count();
  if (count <= 0) return 0;
  int next = index + (direction >= 0 ? 1 : -1);
  next %= count;
  if (next < 0) next += count;
  return next;
}

/* ------------------------------------------------------------ 滤镜面板 */

/*
 * 滤镜种类。数值必须与 ABI `comp_filter_kind` 一致（0..14，尾部追加）；
 * 头文件之间不互相 include，这条约束由 test_ui_core 的 static_assert 钉死。
 */
enum class FilterId : int {
  Vignette = 0,
  TonalContrast = 1,
  Bloom = 2,
  LensCorrection = 3,
  GaussianBlur = 4,
  AddNoise = 5,
  Grain = 6,
  MotionBlur = 7,
  ProgressiveBlur = 8,
  Sharpen = 9,
  ReduceNoise = 10,
  HighPass = 11,
  Minimum = 12,
  Maximum = 13,
  Offset = 14,
  Count
};

/* 面板上一行参数。量程 / 步长 / 缺省值逐项对齐 docs/filters.md §3 的表。 */
struct FilterParamSpec {
  const char* label; /* 英文名；外壳负责转宽字符 */
  double min_value;
  double max_value;
  double step;
  double default_value;
};

/*
 * 一个滤镜的面板定义。`params` 顺序 = 面板行序，也是壳层把 values[] 搬进
 * `comp_filter_desc` 的顺序（映射写在 main.cpp，一处）。
 *
 * 能力边界：Vignette 的边缘色 `color[3]` 不在面板上（v1 固定 0,0,0），
 * 这不是遗漏——面板行模型只承载标量，颜色选择器属后续 UI 轮次。
 */
struct FilterSpec {
  static constexpr int kMaxParams = 6;
  FilterId id;
  const char* name;
  int param_count;
  FilterParamSpec params[kMaxParams];
};

inline const FilterSpec* filter_table(size_t* out_count) {
  static const FilterSpec kFilters[] = {
      {FilterId::Vignette,
       "Vignette",
       5,
       {{"Amount", 0.0, 100.0, 1.0, 0.0},
        {"Midpoint", 0.0, 100.0, 1.0, 50.0},
        {"Roundness", -100.0, 100.0, 1.0, 100.0},
        {"Feather", 0.0, 100.0, 1.0, 60.0},
        {"Highlights", 0.0, 100.0, 1.0, 25.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::TonalContrast,
       "Tonal Contrast",
       5,
       {{"Amount", 0.0, 100.0, 1.0, 0.0},
        {"Radius", 1.0, 100.0, 1.0, 16.0},
        {"Shadows", -100.0, 100.0, 1.0, 40.0},
        {"Midtones", -100.0, 100.0, 1.0, 60.0},
        {"Highlights", -100.0, 100.0, 1.0, 30.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::Bloom,
       "Bloom / Glow",
       2,
       {{"Amount", 0.0, 100.0, 1.0, 0.0},
        {"Radius", 1.0, 150.0, 1.0, 24.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::LensCorrection,
       "Lens Correction",
       1,
       {{"Distortion", -100.0, 100.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::GaussianBlur,
       "Gaussian Blur",
       2,
       {{"Radius", 0.0, 250.0, 1.0, 0.0},
        {"Preserve 0/1", 0.0, 1.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::AddNoise,
       "Add Noise",
       4,
       {{"Amount", 0.0, 400.0, 1.0, 0.0},
        {"Gaussian 0/1", 0.0, 1.0, 1.0, 0.0},
        {"Mono 0/1", 0.0, 1.0, 1.0, 0.0},
        {"Seed", 0.0, 4294967295.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::Grain,
       "Grain",
       4,
       {{"Amount", 0.0, 100.0, 1.0, 0.0},
        {"Size", 0.5, 20.0, 0.5, 1.5},
        {"Roughness", 0.0, 100.0, 1.0, 50.0},
        {"Seed", 0.0, 4294967295.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::MotionBlur,
       "Motion Blur",
       2,
       {{"Angle", -90.0, 90.0, 1.0, 0.0},
        {"Distance", 0.0, 2000.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      /* 渐变轴不走面板行：在画布上拖一条线来定（见 main.cpp 的
       * progressive_begin/extend/end）。面板只承载两端半径与两个开关。 */
      {FilterId::ProgressiveBlur,
       "Progressive Blur",
       4,
       {{"From", 0.0, 250.0, 1.0, 0.0},
        {"To", 0.0, 250.0, 1.0, 20.0},
        {"Radial 0/1", 0.0, 1.0, 1.0, 0.0},
        {"Inverted 0/1", 0.0, 1.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::Sharpen,
       "Sharpen",
       3,
       {{"Amount", 0.0, 500.0, 1.0, 0.0},
        {"Radius", 0.1, 250.0, 0.5, 1.0},
        {"Threshold", 0.0, 255.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::ReduceNoise,
       "Reduce Noise",
       3,
       {{"Strength", 0.0, 10.0, 1.0, 0.0},
        {"Preserve", 0.0, 100.0, 5.0, 50.0},
        {"Color Noise", 0.0, 100.0, 5.0, 25.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      /* Filter > Other 组（kind 11..14）：高反差保留 / 最小值 / 最大值 / 位移。 */
      {FilterId::HighPass,
       "High Pass",
       1,
       {{"Radius", 0.0, 250.0, 0.5, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::Minimum,
       "Minimum",
       1,
       {{"Radius", 0.0, 100.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::Maximum,
       "Maximum",
       1,
       {{"Radius", 0.0, 100.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
      {FilterId::Offset,
       "Offset",
       3,
       {{"Horizontal", -10000.0, 10000.0, 1.0, 0.0},
        {"Vertical", -10000.0, 10000.0, 1.0, 0.0},
        {"Wrap 0/1/2", 0.0, 2.0, 1.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0},
        {"", 0.0, 0.0, 0.0, 0.0}}},
  };
  if (out_count != nullptr) {
    *out_count = sizeof(kFilters) / sizeof(kFilters[0]);
  }
  return kFilters;
}

inline int filter_count() {
  size_t count = 0;
  filter_table(&count);
  return static_cast<int>(count);
}

inline const FilterSpec* filter_spec_at(int index) {
  size_t count = 0;
  const FilterSpec* table = filter_table(&count);
  if (index < 0 || static_cast<size_t>(index) >= count) return nullptr;
  return &table[index];
}

inline const FilterSpec* filter_spec(FilterId id) {
  return filter_spec_at(static_cast<int>(id));
}

/*
 * 参数夹取：NaN / 无穷回落到下限（与 clamp_zoom 的 NaN 口径一致），
 * 越界夹到区间。面板只在键入文本时用它；A-BI 侧仍然是「越界拒绝」，
 * 面板保证自己永远不产生越界值。
 */
inline double clamp_filter_param(const FilterParamSpec& spec, double value) {
  if (!(value == value)) return spec.min_value; /* NaN */
  if (value < spec.min_value) return spec.min_value;
  if (value > spec.max_value) return spec.max_value;
  return value;
}

inline double step_filter_param(const FilterParamSpec& spec, double current, int direction) {
  const double base = clamp_filter_param(spec, current);
  if (direction == 0) return base;
  return clamp_filter_param(spec, base + spec.step * static_cast<double>(direction));
}

/*
 * 滤镜面板状态：值语义、定长数组、无堆分配。壳层只把它画出来，
 * 并把 values[] 按固定顺序搬进 comp_filter_desc。
 */
struct FilterPanel {
  static constexpr int kMaxParams = FilterSpec::kMaxParams;

  FilterId id = FilterId::Vignette;
  int focused = 0;
  double values[kMaxParams] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

  const FilterSpec* spec() const { return filter_spec(id); }

  int param_count() const {
    const FilterSpec* definition = spec();
    return definition != nullptr ? definition->param_count : 0;
  }

  const FilterParamSpec* param_at(int row) const {
    const FilterSpec* definition = spec();
    if (definition == nullptr || row < 0 || row >= definition->param_count) return nullptr;
    return &definition->params[row];
  }

  double value(int row) const {
    return (row >= 0 && row < kMaxParams) ? values[row] : 0.0;
  }

  /* 切到某个滤镜：值回到该滤镜的缺省值，焦点回到第一行。 */
  void select(FilterId next) {
    id = next;
    reset_values();
  }

  void reset_values() {
    focused = 0;
    const FilterSpec* definition = spec();
    for (int i = 0; i < kMaxParams; ++i) {
      values[i] = (definition != nullptr && i < definition->param_count)
                      ? definition->params[i].default_value
                      : 0.0;
    }
  }

  /* 焦点移动：夹在 [0, param_count-1]，不环绕（与面板观感一致）。 */
  void focus(int delta) {
    const int count = param_count();
    if (count <= 0) {
      focused = 0;
      return;
    }
    focused += delta;
    if (focused < 0) focused = 0;
    if (focused >= count) focused = count - 1;
  }

  /* 调整焦点行的值；返回是否真的变化。 */
  bool step(int direction) {
    const FilterParamSpec* parameter = param_at(focused);
    if (parameter == nullptr) return false;
    const double next = step_filter_param(*parameter, values[focused], direction);
    if (next == values[focused]) return false;
    values[focused] = next;
    return true;
  }

  /* 键入提交：先夹取再落值；非法文本由调用方在解析阶段拒绝。 */
  void set_value(int row, double value) {
    const FilterParamSpec* parameter = param_at(row);
    if (parameter == nullptr) return;
    values[row] = clamp_filter_param(*parameter, value);
  }
};

/* 面板参数行几何：与 draw_filter_panel 共用，绘制与命中共用一套常量。 */
inline constexpr int kFilterRowHeight = 20;
inline constexpr int kFilterRowsTop = 54; /* 标题 + 滤镜名两行 + 间距 */

inline Rect filter_param_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kFilterRowsTop + row * kFilterRowHeight, panel.w,
              kFilterRowHeight};
}

inline int filter_param_row_at(const Rect& panel, int param_count, double x, double y) {
  if (param_count <= 0 || !panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kFilterRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kFilterRowHeight)));
  if (row < 0 || row >= param_count) return -1;
  return row;
}

/* ------------------------------------------------------------ 修边面板（图像 > 修边） */

/*
 * 上游 TrimOptions 的壳侧编辑模型（PS Image > Trim 对话框的等价物）。
 * 六行：基准 / 上 / 下 / 左 / 右 / 容差。值语义、无堆分配；提交时按固定行序
 * 搬进 comp_trim_options（映射只在 main.cpp 写一处）。
 *
 * 与滤镜面板同一套键盘规矩：Tab / Shift+Tab 切行、- / = 改值、容差行可数字键入、
 * 回车提交、Esc 取消。基准行循环三档；四条边是开关（- / = 都切换）。
 */
inline constexpr int kTrimRowCount = 6;
inline constexpr int kTrimBasedOnCount = 3;

inline const char* trim_based_on_name(int based_on) {
  switch (based_on) {
    case 0: return "Transparent Pixels";
    case 1: return "Top Left Pixel Color";
    case 2: return "Bottom Right Pixel Color";
    default: return "?";
  }
}

inline const char* trim_row_label(int row) {
  switch (row) {
    case 0: return "Based On";
    case 1: return "Top";
    case 2: return "Bottom";
    case 3: return "Left";
    case 4: return "Right";
    case 5: return "Tolerance";
    default: return "";
  }
}

/* 边开关的显示文字（绘制与状态栏共用）。 */
inline const char* trim_edge_text(int on) { return on ? "on" : "off"; }

struct TrimPanel {
  int focused = 0;
  int based_on = 0;           /* comp_trim_based_on 数值 0..2 */
  int edge[4] = {1, 1, 1, 1}; /* top / bottom / left / right；1 = 这一边参与裁切 */
  int tolerance = 0;          /* 0..255（comp_trim_options.tolerance 是 uint8_t） */

  void reset() {
    focused = 0;
    based_on = 0;
    edge[0] = 1;
    edge[1] = 1;
    edge[2] = 1;
    edge[3] = 1;
    tolerance = 0;
  }

  int row_count() const { return kTrimRowCount; }

  /* 焦点夹在 [0, kTrimRowCount-1]，不环绕（与滤镜 / 效果面板一致）。 */
  void focus(int delta) {
    focused += delta;
    if (focused < 0) focused = 0;
    if (focused >= kTrimRowCount) focused = kTrimRowCount - 1;
  }

  /* - / = 改当前行；返回是否真的变化。 */
  bool step(int direction) {
    if (direction == 0) return false;
    switch (focused) {
      case 0: {
        int next = based_on + (direction > 0 ? 1 : -1);
        if (next < 0) next = kTrimBasedOnCount - 1;
        if (next >= kTrimBasedOnCount) next = 0;
        if (next == based_on) return false;
        based_on = next;
        return true;
      }
      case 1:
      case 2:
      case 3:
      case 4: {
        int& value = edge[focused - 1];
        const int next = value ? 0 : 1;
        value = next;
        return true;
      }
      case 5: {
        int next = tolerance + (direction > 0 ? 1 : -1);
        if (next < 0) next = 0;
        if (next > 255) next = 255;
        if (next == tolerance) return false;
        tolerance = next;
        return true;
      }
      default:
        return false;
    }
  }

  void set_tolerance(int value) { tolerance = value < 0 ? 0 : (value > 255 ? 255 : value); }

  bool any_edge() const { return (edge[0] | edge[1] | edge[2] | edge[3]) != 0; }
};

/* 行几何：与 draw_trim_panel 共用，绘制与命中同源。 */
inline constexpr int kTrimRowHeight = 20;
inline constexpr int kTrimRowsTop = 54;

inline Rect trim_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kTrimRowsTop + row * kTrimRowHeight, panel.w, kTrimRowHeight};
}

inline int trim_row_at(const Rect& panel, int row_count, double x, double y) {
  if (row_count <= 0 || !panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kTrimRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kTrimRowHeight)));
  if (row < 0 || row >= row_count) return -1;
  return row;
}

/* ------------------------------------------------------------ Retouch 工具模式 */

/*
 * 上游把 Blur / Smudge / Liquify 收在同一个 R 键上循环
 * （`KeyboardShortcuts.swift` 的 retouch 组）。本仓用同一模型：
 * 重复按 R 依次切换，模式名进状态栏。
 */
inline constexpr int kRetouchModeCount = 3;
inline constexpr int kRetouchBlur = 0;     /* 走 COMP_TOOL_BLUR，调 blur.radius */
inline constexpr int kRetouchSmudge = 1;   /* 走 COMP_TOOL_SMUDGE，调 warp.strength */
inline constexpr int kRetouchLiquify = 2;  /* 走 COMP_TOOL_LIQUIFY，调 warp.strength */

inline const char* retouch_mode_name(int mode) {
  switch (mode) {
    case kRetouchBlur: return "Blur";
    case kRetouchSmudge: return "Smudge";
    case kRetouchLiquify: return "Liquify";
    default: return "?";
  }
}

inline int next_retouch_mode(int mode) {
  if (mode < 0 || mode >= kRetouchModeCount - 1) return 0;
  return mode + 1;
}

/* 模糊半径（comp_tool_blur.radius 的 0.5..50 与上游 blurRadius 同量程）。 */
inline constexpr double kMinBlurRadius = 0.5;
inline constexpr double kMaxBlurRadius = 50.0;

inline double clamp_blur_radius(double value) {
  if (!(value > 0.0)) return kMinBlurRadius; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinBlurRadius), kMaxBlurRadius);
}

inline double step_blur_radius(double current, int direction) {
  const double base = clamp_blur_radius(current);
  if (direction > 0) return clamp_blur_radius(base * 1.2);
  if (direction < 0) return clamp_blur_radius(base / 1.2);
  return base;
}

/* ------------------------------------------------------------ 色调工具（Dodge / Burn / Sponge） */

/*
 * 上游工具箱把减淡 / 加深 / 海绵收在同一个 O 键上循环（对齐 Photoshop 的
 * Dodge 默认键 O）。本仓用一个工具 + 8 个状态：
 *
 *   状态 0..2 = Dodge（中间调 / 阴影 / 高光）
 *   状态 3..5 = Burn （中间调 / 阴影 / 高光）
 *   状态 6..7 = Sponge（去色 / 加色）
 *
 * 状态是壳侧唯一的真值：ABI 侧的 (算子, mode) 由 tone_state_tool() /
 * tone_state_mode() 换算，壳内不另存一份，避免「状态栏说阴影、实际按中间调算」。
 */
inline constexpr int kToneStateCount = 8;
inline constexpr int kToneDodgeMidtone = 0;
inline constexpr int kToneDodgeShadow = 1;
inline constexpr int kToneDodgeHighlight = 2;
inline constexpr int kToneBurnMidtone = 3;
inline constexpr int kToneBurnShadow = 4;
inline constexpr int kToneBurnHighlight = 5;
inline constexpr int kToneSpongeDesaturate = 6;
inline constexpr int kToneSpongeSaturate = 7;

/* 算子：0 = 减淡、1 = 加深、2 = 海绵（壳侧编号，不是 ABI 枚举值）。 */
inline constexpr int kToneToolDodge = 0;
inline constexpr int kToneToolBurn = 1;
inline constexpr int kToneToolSponge = 2;

/* 状态 -> 算子。越界按 Dodge 处理（与 next_* 的「不越界读表」同口径）。 */
inline int tone_state_tool(int state) {
  if (state < kToneBurnMidtone) return kToneToolDodge;
  if (state < kToneSpongeDesaturate) return kToneToolBurn;
  return kToneToolSponge;
}

/*
 * 状态 -> ABI `comp_tool_tone.mode`：Dodge/Burn 0 = 中间调、1 = 阴影、2 = 高光；
 * Sponge 0 = 去色、1 = 加色。状态里的三档与这里的 mode 同序，减掉各段起点即可。
 */
inline int tone_state_mode(int state) {
  if (state < 0 || state >= kToneStateCount) return 0;
  if (state < kToneBurnMidtone) return state;
  if (state < kToneSpongeDesaturate) return state - kToneBurnMidtone;
  return state - kToneSpongeDesaturate;
}

inline const char* tone_state_name(int state) {
  switch (state) {
    case kToneDodgeMidtone: return "Dodge / Midtones";
    case kToneDodgeShadow: return "Dodge / Shadows";
    case kToneDodgeHighlight: return "Dodge / Highlights";
    case kToneBurnMidtone: return "Burn / Midtones";
    case kToneBurnShadow: return "Burn / Shadows";
    case kToneBurnHighlight: return "Burn / Highlights";
    case kToneSpongeDesaturate: return "Sponge / Desaturate";
    case kToneSpongeSaturate: return "Sponge / Saturate";
    default: return "?";
  }
}

/* 重复按 O：Dodge 中间调 -> ... -> Sponge 加色 -> 回到 Dodge 中间调。 */
inline int next_tone_state(int state) {
  if (state < 0 || state >= kToneStateCount - 1) return kToneDodgeMidtone;
  return state + 1;
}

/* 强度口径对齐 comp_tool_tone.strength（0..1，0 被引擎的闸拒绝）。 */
inline constexpr double kMinToneStrength = 0.01;
inline constexpr double kMaxToneStrength = 1.0;

inline double clamp_tone_strength(double value) {
  if (!(value > 0.0)) return kMinToneStrength; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinToneStrength), kMaxToneStrength);
}

inline double step_tone_strength(double current, int direction) {
  const double base = clamp_tone_strength(current);
  if (direction > 0) return clamp_tone_strength(base + 0.1);
  if (direction < 0) return clamp_tone_strength(base - 0.1);
  return base;
}

/*
 * 历史记录画笔的不透明度（口径对齐 comp_tool_history_brush.opacity：0..1，
 * 0 会被引擎的闸拒掉）。Alt+[ / ] 以 10% 一步，下限就取按住前进得到的最低位
 * 10%——键盘不该能把参数步进到一个必然被拒的值上。
 */
inline constexpr double kMinHistoryBrushOpacity = 0.1;
inline constexpr double kMaxHistoryBrushOpacity = 1.0;

inline double clamp_history_brush_opacity(double value) {
  if (!(value > 0.0)) return kMinHistoryBrushOpacity; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinHistoryBrushOpacity), kMaxHistoryBrushOpacity);
}

inline double step_history_brush_opacity(double current, int direction) {
  const double base = clamp_history_brush_opacity(current);
  if (direction > 0) return clamp_history_brush_opacity(base + 0.1);
  if (direction < 0) return clamp_history_brush_opacity(base - 0.1);
  return base;
}

/* ------------------------------------------------------------ Spot Healing 模式 */

/*
 * 上游 Spot Healing 的三种修补算法（comp_tool_heal.mode 的 0..2）。
 * 重复按 J 依次切换，模式名进状态栏——与 R / O 的循环口径一致。
 */
inline constexpr int kHealModeCount = 3;
inline constexpr int kHealContentAware = 0; /* PatchMatch，最慢也最像 PS 的内容识别 */
inline constexpr int kHealCreateTexture = 1; /* 只借纹理，不借结构 */
inline constexpr int kHealProximity = 2;    /* 邻近取色，最快 */

inline const char* heal_mode_name(int mode) {
  switch (mode) {
    case kHealContentAware: return "Content-Aware";
    case kHealCreateTexture: return "Create Texture";
    case kHealProximity: return "Proximity Match";
    default: return "?";
  }
}

inline int next_heal_mode(int mode) {
  if (mode < 0 || mode >= kHealModeCount - 1) return 0;
  return mode + 1;
}

/* 修补不透明度（comp_tool_heal.opacity 的 0..1；0 被引擎的闸拒绝）。10% 一步。 */
inline constexpr double kMinHealOpacity = 0.1;
inline constexpr double kMaxHealOpacity = 1.0;

inline double clamp_heal_opacity(double value) {
  if (!(value > 0.0)) return kMinHealOpacity; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinHealOpacity), kMaxHealOpacity);
}

inline double step_heal_opacity(double current, int direction) {
  const double base = clamp_heal_opacity(current);
  if (direction > 0) return clamp_heal_opacity(base + 0.1);
  if (direction < 0) return clamp_heal_opacity(base - 0.1);
  return base;
}

/* ------------------------------------------------------------ 仿制不透明度 */
/*
 * 仿制图章的笔迹总强度（comp_tool_clone.opacity）。引擎的闸允许 0，但 0 = 什么都不画，
 * 对外壳用户是无反馈的死调节，因此与修补同口径取 10% 下限。
 */
inline constexpr double kMinCloneOpacity = 0.1;
inline constexpr double kMaxCloneOpacity = 1.0;

inline double clamp_clone_opacity(double value) {
  if (!(value > 0.0)) return kMinCloneOpacity; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinCloneOpacity), kMaxCloneOpacity);
}

inline double step_clone_opacity(double current, int direction) {
  const double base = clamp_clone_opacity(current);
  if (direction > 0) return clamp_clone_opacity(base + 0.1);
  if (direction < 0) return clamp_clone_opacity(base - 0.1);
  return base;
}

/* ------------------------------------------------------------ 渐变工具（S9） */

/* 渐变类型（镜像 ABI comp_tool_gradient.type；数值必须与 ABI 一致）。 */
inline constexpr int kGradientLinear = 0;    /* 线性：沿两端点连线 */
inline constexpr int kGradientRadial = 1;    /* 径向：以起点为圆心、两端点距离为半径 */
inline constexpr int kGradientAngle = 2;     /* 角度（锥形）：绕起点的方位角 / 360° */
inline constexpr int kGradientReflected = 3; /* 对称：起点两侧镜像 */
inline constexpr int kGradientDiamond = 4;   /* 菱形：|投影| + |横向| */
inline constexpr int kGradientTypeCount = 5;

inline const char* gradient_type_name(int type) {
  switch (type) {
    case kGradientLinear: return "Linear";
    case kGradientRadial: return "Radial";
    case kGradientAngle: return "Angle";
    case kGradientReflected: return "Reflected";
    case kGradientDiamond: return "Diamond";
    default: return "?";
  }
}

/* G 键：已在该工具时循环五种类型；未知值收敛回线性，不产生第六态。 */
inline int next_gradient_type(int type) {
  if (type < kGradientLinear || type >= kGradientTypeCount - 1) return kGradientLinear;
  return type + 1;
}

/* ---------------------------------------------------------- 渐变预设（P1-7） */

/*
 * 外壳的多色标来源。引擎支持最多 32 个色标；外壳给三张**只读预设表**，外加
 * 第 4 档「自定义」——由 `DocDialogKind::GradientStops` 的数值面板编辑（增删、
 * 位置、中点、RGBA）。预设表与自定义表共用 `GradientPresetStop` 这一种元素类型，
 * 落笔时走同一条展开路径（见 shell_tools.cpp 的 gradient_end）。
 */
inline constexpr int kGradientPresetCustom = 3;
inline constexpr int kGradientPresetCount = 4;
/* 与 ABI 的 COMP_TOOL_GRADIENT_MAX_STOPS 同值；引擎会拒绝超过它的表。 */
inline constexpr int kGradientMaxStops = 32;

struct GradientPresetStop {
  double position = 0.0;
  double midpoint = 0.5;
  uint8_t color[4] = {0, 0, 0, 255};
};

inline const char* gradient_preset_name(int preset) {
  switch (preset) {
    case 0: return "前景→透明";
    case 1: return "黑→白";
    case 2: return "彩虹";
    case kGradientPresetCustom: return "自定义";
    default: return "?";
  }
}

/* 预设 0 用前景色，其余预设是固定色标表（与前景色无关）；自定义没有内置表。 */
inline int gradient_preset_stop_count(int preset, int* out_is_foreground) {
  if (out_is_foreground != nullptr) *out_is_foreground = preset == 0 ? 1 : 0;
  if (preset == kGradientPresetCustom) return 0;
  return preset == 2 ? 5 : 2;
}

/*
 * 展开预设到 `out[0..count)`，返回写入个数（capacity 不足返回 0，不写半张表）。
 * `foreground` 是直通 RGBA（预设 0 的第一色标用它自己的 RGB + 不透明，第二色标
 * 同 RGB + alpha 0）。`reverse` = 色标顺序整体翻转（与旧的两色标 Shift 反向同一语义）；
 * 翻转时段中点也要跟着翻：反向段的中点 = 1 - 原段中点。
 */
inline int gradient_preset_stops(int preset, const uint8_t foreground[4], bool reverse,
                                 GradientPresetStop* out, int capacity) {
  if (out == nullptr) return 0;
  /* 自定义表不是预设：调用方必须走自己的色标数组（见 kGradientPresetCustom）。 */
  if (preset < 0 || preset > 2) return 0;
  const uint8_t black[4] = {0, 0, 0, 255};
  const uint8_t white[4] = {255, 255, 255, 255};
  const uint8_t red[4] = {255, 0, 0, 255};
  const uint8_t yellow[4] = {255, 255, 0, 255};
  const uint8_t green[4] = {0, 255, 0, 255};
  const uint8_t cyan[4] = {0, 255, 255, 255};
  const uint8_t blue[4] = {0, 0, 255, 255};
  GradientPresetStop table[5];
  int count = 0;
  auto push = [&](double position, double midpoint, const uint8_t color[4]) {
    GradientPresetStop& stop = table[count++];
    stop.position = position;
    stop.midpoint = midpoint;
    for (int c = 0; c < 4; ++c) stop.color[c] = color[c];
  };
  if (preset == 2) {
    /* 彩虹：五个等距色标，其中两段的中点刻意偏离 0.5（检验中点重映射真的生效）。 */
    push(0.0, 0.6, red);
    push(0.25, 0.5, yellow);
    push(0.5, 0.4, green);
    push(0.75, 0.5, cyan);
    push(1.0, 0.5, blue);
    count = 5;
  } else if (preset == 1) {
    push(0.0, 0.5, black);
    push(1.0, 0.5, white);
    count = 2;
  } else {
    uint8_t solid[4] = {0, 0, 0, 255};
    uint8_t clear[4] = {0, 0, 0, 0};
    if (foreground != nullptr) {
      for (int c = 0; c < 3; ++c) {
        solid[c] = foreground[c];
        clear[c] = foreground[c];
      }
    }
    push(0.0, 0.5, solid);
    push(1.0, 0.5, clear);
    count = 2;
  }
  if (count > capacity) return 0;
  if (!reverse) {
    for (int i = 0; i < count; ++i) out[i] = table[i];
    return count;
  }
  for (int i = 0; i < count; ++i) {
    const GradientPresetStop& source = table[count - 1 - i];
    GradientPresetStop& target = out[i];
    target.position = 1.0 - source.position;
    /* 反向段 (i, i+1) ↔ 原段 (count-2-i, count-1-i)，中点取原段左端的中点。 */
    target.midpoint = i + 1 < count ? 1.0 - table[count - 2 - i].midpoint : 0.5;
    for (int c = 0; c < 4; ++c) target.color[c] = source.color[c];
  }
  return count;
}

/* ------------------------------------------------------ 可编辑色标表（P1-7） */

/*
 * 色标位置在面板里按**千分位整数（‰）**编辑，显示成「50.0%」。
 * 为什么不用整数百分比：1% 在 1280 px 画布上是 12.8 px，定位太粗；千分位是
 * PS 的 0.1% 档，足够精确，又不必让整数输入框学会浮点。
 */
inline int gradient_position_permille(double position) {
  if (!std::isfinite(position)) return 0;
  long value = std::lround(position * 1000.0);
  if (value < 0) value = 0;
  if (value > 1000) value = 1000;
  return static_cast<int>(value);
}

/*
 * 翻转一张任意色标表（2..32 个）。与 gradient_preset_stops 的 reverse 分支同一
 * 语义：位置取 1 - p，反向段 (i, i+1) 的中点取原段左端的中点，末色标的中点没有
 * 下一段，写回 0.5。表先整体拷进栈上副本再写出，允许 src == out。
 */
inline int reverse_gradient_stops_copy(const GradientPresetStop* src, int count,
                                       GradientPresetStop* out, int capacity) {
  if (src == nullptr || out == nullptr) return 0;
  if (count < 0 || count > kGradientMaxStops || count > capacity) return 0;
  GradientPresetStop table[kGradientMaxStops];
  for (int i = 0; i < count; ++i) table[i] = src[i];
  for (int i = 0; i < count; ++i) {
    const GradientPresetStop& source = table[count - 1 - i];
    GradientPresetStop& target = out[i];
    target.position = 1.0 - source.position;
    target.midpoint = i + 1 < count ? 1.0 - table[count - 2 - i].midpoint : 0.5;
    for (int c = 0; c < 4; ++c) target.color[c] = source.color[c];
  }
  return count;
}

/*
 * 第 index 个色标的可编辑位置区间（‰，闭区间）。首末色标钉在 0 / 1000：
 * 引擎允许端点内缩，但面板若也允许，用户会把渐变拖成「左边一段纯色」——
 * 那在画布上看起来像没生效。中间色标与邻居至少隔 1‰，保证严格升序。
 */
inline void gradient_stop_position_bounds_permille(const GradientPresetStop* stops, int count,
                                                   int index, int* out_min, int* out_max) {
  int low = 0;
  int high = 1000;
  if (stops != nullptr && count >= 2 && index >= 0 && index < count) {
    if (index == 0) {
      high = 0;
    } else if (index == count - 1) {
      low = 1000;
    } else {
      low = gradient_position_permille(stops[index - 1].position) + 1;
      high = gradient_position_permille(stops[index + 1].position) - 1;
    }
  }
  if (low > high) low = high;
  if (out_min != nullptr) *out_min = low;
  if (out_max != nullptr) *out_max = high;
}

inline void gradient_set_stop_position_permille(GradientPresetStop* stops, int count, int index,
                                                int permille) {
  if (stops == nullptr || count < 2 || index < 0 || index >= count) return;
  int low = 0;
  int high = 1000;
  gradient_stop_position_bounds_permille(stops, count, index, &low, &high);
  if (permille < low) permille = low;
  if (permille > high) permille = high;
  stops[index].position = permille / 1000.0;
}

/* 中点只属于本段，与邻居无关；引擎要求严格落在 (0,1)，所以夹到 1..999。 */
inline void gradient_set_stop_midpoint_permille(GradientPresetStop* stops, int count, int index,
                                                int permille) {
  if (stops == nullptr || count <= 0 || index < 0 || index >= count) return;
  if (permille < 1) permille = 1;
  if (permille > 999) permille = 999;
  stops[index].midpoint = permille / 1000.0;
}

/*
 * 在第 selected 个色标旁边插入一个色标，位置取相邻两色标的中点，颜色复制左端
 * 那个。selected 是末色标时用左侧那段（保证插进去的位置仍在它左边、表仍升序）。
 * 返回新表长度；表满 32、位置退化或参数越界时不改表，返回 0。
 */
inline int gradient_insert_stop(GradientPresetStop* stops, int count, int selected) {
  if (stops == nullptr || count < 2 || count >= kGradientMaxStops) return 0;
  if (selected < 0 || selected >= count) return 0;
  const int segment = selected < count - 1 ? selected : count - 2;
  const double left = stops[segment].position;
  const double right = stops[segment + 1].position;
  const double position = (left + right) * 0.5;
  if (!(position > left && position < right)) return 0;
  for (int i = count; i > segment + 1; --i) stops[i] = stops[i - 1];
  GradientPresetStop& added = stops[segment + 1];
  added.position = position;
  added.midpoint = 0.5;
  for (int c = 0; c < 4; ++c) added.color[c] = stops[segment].color[c];
  return count + 1;
}

/*
 * 删除第 selected 个色标。首末固定在 0% / 100%，不能删；少于 3 个时也没有
 * 可删的中间项（引擎至少要 2 个色标）。返回新表长度，未改动返回 0。
 */
inline int gradient_delete_stop(GradientPresetStop* stops, int count, int selected) {
  if (stops == nullptr || count <= 2) return 0;
  if (selected <= 0 || selected >= count - 1) return 0;
  for (int i = selected; i + 1 < count; ++i) stops[i] = stops[i + 1];
  return count - 1;
}

/*
 * 落笔前的整表校验，逐条对齐引擎的参数闸（tool_ops.cpp）：
 *   2..32 个、位置在 0..1 且严格升序、中点严格在 (0,1)、至少一个色标不透明。
 * 外壳先拦一次，是为了把「引擎返回 -2」翻译成面板里能看懂的一句话；这不是
 * 重复校验——引擎那一份仍是唯一权威，外壳只是提前给出可操作的原因。
 */
inline bool gradient_stops_valid(const GradientPresetStop* stops, int count) {
  if (stops == nullptr || count < 2 || count > kGradientMaxStops) return false;
  if (!std::isfinite(stops[0].position) || stops[0].position < 0.0 ||
      stops[0].position > 1.0) {
    return false;
  }
  bool any_visible = false;
  double previous = -1.0;
  for (int i = 0; i < count; ++i) {
    const GradientPresetStop& stop = stops[i];
    if (!std::isfinite(stop.position) || stop.position < 0.0 || stop.position > 1.0) {
      return false;
    }
    if (stop.position <= previous) return false;
    if (!std::isfinite(stop.midpoint) || stop.midpoint <= 0.0 || stop.midpoint >= 1.0) {
      return false;
    }
    previous = stop.position;
    if (stop.color[3] != 0) any_visible = true;
  }
  return any_visible;
}

/*
 * 最小拖拽长度（文档像素）。短于它的拖拽不是可定义的方向/半径：引擎参数闸会
 * 以「端点重合」拒绝，外壳先在这里判定并给出提示，避免「拖了一下没反应」变成
 * 一次错误弹窗。阈值取 1 px，与手抖容差同量级。
 */
inline constexpr double kGradientMinLength = 1.0;

inline bool gradient_drag_is_usable(double start_x, double start_y, double end_x,
                                    double end_y) {
  const double dx = end_x - start_x;
  const double dy = end_y - start_y;
  return dx * dx + dy * dy >= kGradientMinLength * kGradientMinLength;
}

/* ------------------------------------------------------------ 形状工具（S10） */

/* 形状种类（镜像 ABI `comp_shape_kind` / 域层 `ShapeKind`；数值必须一致）。 */
inline constexpr int kShapeRect = 0;
inline constexpr int kShapeEllipse = 1;
inline constexpr int kShapeLine = 2;
inline constexpr int kShapePath = 3;
/* 外壳能从鼠标拖拽产出的只有前三种；Path 只经 ABI（钢笔路径提交选区，不建形状）。 */
inline constexpr int kShellShapeKindCount = 3;

inline const char* shape_kind_name(int kind) {
  switch (kind) {
    case kShapeRect: return "Rectangle";
    case kShapeEllipse: return "Ellipse";
    case kShapeLine: return "Line";
    case kShapePath: return "Path";
    default: return "?";
  }
}

/* U 键：已在该工具时循环 Rectangle -> Ellipse -> Line（与 M/G/R 同款习惯）。 */
inline int next_shell_shape_kind(int kind) {
  if (kind < kShapeRect || kind >= kShellShapeKindCount - 1) return kShapeRect;
  return kind + 1;
}

/* 描边宽度（comp_shape_style.stroke_width，文档像素全宽）。量程与上游线宽一致。 */
inline constexpr double kMinShapeStrokeWidth = 0.5;
inline constexpr double kMaxShapeStrokeWidth = 50.0;
inline constexpr double kDefaultShapeStrokeWidth = 4.0;

inline double clamp_shape_stroke_width(double value) {
  if (!(value > 0.0)) return kMinShapeStrokeWidth; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinShapeStrokeWidth), kMaxShapeStrokeWidth);
}

inline double step_shape_stroke_width(double current, int direction) {
  const double base = clamp_shape_stroke_width(current);
  if (direction > 0) return clamp_shape_stroke_width(base * 1.2);
  if (direction < 0) return clamp_shape_stroke_width(base / 1.2);
  return base;
}

/* 圆角半径（矩形专用，文档像素；0 = 直角。引擎再夹到短边一半）。 */
inline constexpr double kMinShapeCornerRadius = 0.0;
inline constexpr double kMaxShapeCornerRadius = 256.0;
inline constexpr double kDefaultShapeCornerRadius = 0.0;

inline double clamp_shape_corner_radius(double value) {
  if (!(value > 0.0)) return kMinShapeCornerRadius;
  return std::min(std::max(value, kMinShapeCornerRadius), kMaxShapeCornerRadius);
}

inline double step_shape_corner_radius(double current, int direction) {
  const double base = clamp_shape_corner_radius(current);
  if (direction > 0) return clamp_shape_corner_radius(base + 2.0);
  if (direction < 0) return clamp_shape_corner_radius(base - 2.0);
  return base;
}

/* 拖拽盒（文档坐标，四个值任意顺序）。 */
struct ShapeDragBox {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

/*
 * 把「按下点 -> 当前点」变成落笔盒：Shift = 正方形/正圆（取较长边、保留方向），
 * Alt = 从中心向外（上游 ShapeTool 同款修饰键；两者可叠加）。
 */
inline ShapeDragBox shape_drag_box(double start_x, double start_y, double current_x,
                                   double current_y, bool square, bool from_center) {
  double dx = current_x - start_x;
  double dy = current_y - start_y;
  if (square) {
    const double extent = std::max(std::fabs(dx), std::fabs(dy));
    dx = dx < 0.0 ? -extent : extent;
    dy = dy < 0.0 ? -extent : extent;
  }
  ShapeDragBox box{};
  if (from_center) {
    box.x0 = start_x - dx;
    box.y0 = start_y - dy;
    box.x1 = start_x + dx;
    box.y1 = start_y + dy;
  } else {
    box.x0 = start_x;
    box.y0 = start_y;
    box.x1 = start_x + dx;
    box.y1 = start_y + dy;
  }
  return box;
}

/*
 * 拖拽是否够大：短于 1 文档像素的盒/线不可定义（引擎参数闸会以「零尺寸/端点重合」
 * 拒绝，外壳先判定并给提示，避免一次误拖变成错误弹窗）。
 */
inline constexpr double kShapeMinExtent = 1.0;

inline bool shape_drag_is_usable(int kind, const ShapeDragBox& box) {
  if (kind == kShapeLine) {
    const double dx = box.x1 - box.x0;
    const double dy = box.y1 - box.y0;
    return dx * dx + dy * dy >= kShapeMinExtent * kShapeMinExtent;
  }
  return std::fabs(box.x1 - box.x0) >= kShapeMinExtent &&
         std::fabs(box.y1 - box.y0) >= kShapeMinExtent;
}

/* 矩形圆角半径的上限（引擎夹到短边一半；外壳提示的是用户设定值）。 */
inline double shape_effective_corner_radius(double radius, const ShapeDragBox& box) {
  const double limit =
      std::min(std::fabs(box.x1 - box.x0), std::fabs(box.y1 - box.y0)) * 0.5;
  const double clamped = clamp_shape_corner_radius(radius);
  return clamped > limit ? limit : clamped;
}

/* ------------------------------------------------------------ 文字工具（S11） */

/*
 * 文字工具有两条轨道（P5）：
 *   - 默认 5×7 点阵：锚定「零依赖、可交叉编译」，只收 0x20..0x7E 与 '\n'；
 *   - 字体轨道：宿主传入已加载的 comp_font，font_size 变成 **em 高**，文本是
 *     UTF-8（CJK 由输入法直接送进来）。没有字体就老老实实走点阵，不猜系统字体。
 *
 * 这里的三组量程必须与 `engine/domain/type_raster.h` 的 TypeRasterStyle 逐项一致
 * （字号 6..2048、行距 1..8、字距 ±4096，且步进宽度 >= 0.5 px）：
 * 外壳先按同一套规则夹取，避免「能调出引擎一定拒绝的值」。
 */
inline constexpr double kMinTypeFontSize = 6.0;
inline constexpr double kMaxTypeFontSize = 2048.0;
inline constexpr double kDefaultTypeFontSize = 32.0;

inline double clamp_type_font_size(double value) {
  if (!(value > 0.0)) return kMinTypeFontSize; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinTypeFontSize), kMaxTypeFontSize);
}

/* Alt+[ / Alt+]：乘性步进（与笔刷直径、形状线宽同款手感）。 */
inline double step_type_font_size(double current, int direction) {
  const double base = clamp_type_font_size(current);
  if (direction > 0) return clamp_type_font_size(base * 1.25);
  if (direction < 0) return clamp_type_font_size(base / 1.25);
  return base;
}

inline constexpr double kMinTypeLineHeight = 1.0;
inline constexpr double kMaxTypeLineHeight = 8.0;
inline constexpr double kDefaultTypeLineHeight = 1.25;

inline double clamp_type_line_height(double value) {
  if (!(value > 0.0)) return kDefaultTypeLineHeight;
  return std::min(std::max(value, kMinTypeLineHeight), kMaxTypeLineHeight);
}

/*
 * 字距（字母间距，文档像素）。负值是合法排版（收紧），但底限由「字形步进
 * 6*scale + spacing 必须 >= 0.5 px」决定：字号 6 时步进本来只有 ~5.14 px，
 * 再收紧超过 4.64 px 引擎就会拒绝，所以下限跟着字号走。
 */
inline constexpr double kMinTypeLetterSpacing = -4096.0;
inline constexpr double kMaxTypeLetterSpacing = 4096.0;
inline constexpr double kMinTypeAdvance = 0.5;

inline double type_letter_spacing_min(double font_size) {
  const double size = clamp_type_font_size(font_size);
  const double base_advance = 6.0 * size / 7.0;
  const double limit = kMinTypeAdvance - base_advance;
  return limit < kMinTypeLetterSpacing ? kMinTypeLetterSpacing : limit;
}

inline double clamp_type_letter_spacing(double value, double font_size) {
  if (!(value > 0.0) && !(value < 0.0)) return 0.0; /* NaN 归零 */
  return std::min(std::max(value, type_letter_spacing_min(font_size)), kMaxTypeLetterSpacing);
}

/* Shift+Alt+[ / ]：线性步进 1 px（字距是排版值，乘性步进在 0 附近没手感）。 */
inline double step_type_letter_spacing(double current, double font_size, int direction) {
  const double base = clamp_type_letter_spacing(current, font_size);
  if (direction > 0) return clamp_type_letter_spacing(base + 1.0, font_size);
  if (direction < 0) return clamp_type_letter_spacing(base - 1.0, font_size);
  return base;
}

/*
 * 文本框度量（文档像素，镜像 engine/domain/type_raster.cpp 的 build_layout）：
 *   - 步进 = 6 * (font_size/7) + letter_spacing；
 *   - 宽 = 最长一行的字形数 × 步进（含最后一个字形后的步进侧距）；
 *   - 高 = font_size + (行数-1) * font_size * line_height。
 * 只用于「预览框 + 状态栏数字」，不是像素判定的第二套真值——像素在提交时
 * 由引擎一次写出。text 允许为 nullptr / 空串（此时宽 0、高一个字号）。
 */
struct TypeBlockMetrics {
  double width = 0.0;
  double height = 0.0;
  double advance = 0.0;
  int line_count = 1;
  int max_line_glyphs = 0;
  int last_line_glyphs = 0;
};

inline TypeBlockMetrics type_block_metrics(double font_size, double line_height,
                                           double letter_spacing, const char* text) {
  TypeBlockMetrics out{};
  const double size = clamp_type_font_size(font_size);
  const double spacing = clamp_type_letter_spacing(letter_spacing, size);
  const double lh = clamp_type_line_height(line_height);
  const double scale = size / 7.0;
  out.advance = 6.0 * scale + spacing;
  if (out.advance < kMinTypeAdvance) out.advance = kMinTypeAdvance;

  int glyphs = 0;
  if (text != nullptr) {
    for (const char* p = text; *p != '\0'; ++p) {
      if (*p == '\n') {
        if (glyphs > out.max_line_glyphs) out.max_line_glyphs = glyphs;
        out.last_line_glyphs = glyphs;
        glyphs = 0;
        ++out.line_count;
      } else {
        ++glyphs;
      }
    }
  }
  out.last_line_glyphs = glyphs;
  if (glyphs > out.max_line_glyphs) out.max_line_glyphs = glyphs;
  out.width = static_cast<double>(out.max_line_glyphs) * out.advance;
  out.height = size + static_cast<double>(out.line_count - 1) * size * lh;
  return out;
}

/*
 * 段落框与对齐（P0-6 ③）。量纲是文档像素：
 *   - 0 = 自动（点文字：不折行，框随内容）；
 *   - > 0 = 段落框：框宽决定折行点，框高只负责裁切（超出的行不画）。
 * 引擎允许到 1e6（那是给宿主渲染的量程），外壳只放到 8192：选项栏里按一下
 * 涨 25%，再大的框在几十次点击内也到不了，收窄上限只是不让误触调到无意义的数。
 *
 * 从「自动」起跳给一个看得见折行的 128 px，之后乘性增长；向下退到最小值再
 * 退一步就回到「自动」——与字号 Alt+[ / ] 的乘性手感一致。量化到 16 px 是
 * 为了让读数是整数、也让面板/状态栏不需要显示小数。
 */
inline constexpr double kTypeBoxMax = 8192.0;
inline constexpr double kTypeBoxMin = 16.0;
inline constexpr double kTypeBoxStart = 128.0;
inline constexpr double kTypeBoxStep = 16.0;

inline double clamp_type_box(double value) {
  if (!(value > 0.0)) return 0.0; /* NaN / 0 / 负数 = 自动 */
  return std::min(std::max(value, kTypeBoxMin), kTypeBoxMax);
}

inline double step_type_box(double current, int direction) {
  const double base = clamp_type_box(current);
  if (direction > 0) {
    if (base <= 0.0) return kTypeBoxStart;
    return clamp_type_box(std::ceil(base * 1.25 / kTypeBoxStep) * kTypeBoxStep);
  }
  if (direction < 0) {
    if (base <= kTypeBoxStart) return 0.0;
    return clamp_type_box(std::floor(base / 1.25 / kTypeBoxStep) * kTypeBoxStep);
  }
  return base;
}

/* 对齐档位与引擎 comp_text_align 同值（0 左 / 1 居中 / 2 右）。 */
inline constexpr int kTypeAlignLeft = 0;
inline constexpr int kTypeAlignCenter = 1;
inline constexpr int kTypeAlignRight = 2;
inline constexpr int kTypeAlignCount = 3;

inline const char* type_align_name(int align) {
  switch (align) {
    case kTypeAlignCenter: return "居中";
    case kTypeAlignRight: return "右对齐";
    case kTypeAlignLeft: return "左对齐";
    default: return "左对齐"; /* 越界值按默认档显示，不制造第四种状态 */
  }
}

/*
 * UTF-8 缓冲的逐码点编辑（P5-3）。
 *
 * 字体轨道的文本来自 WM_CHAR（含输入法），一个汉字是 3 字节；退格如果按字节删，
 * 留下的是半个码点，引擎会如实报 kBadText——用户看到的是「退格把整段文字弄没了」。
 * 所以编辑一律按码点：先认出首字节长度，再整段删除/追加。
 *
 * 这里不做「容错解码」：缓冲只由本文件的追加函数写入，出现非完整 UTF-8 只可能是
 * 本文件自己的 bug，那时返回 0 并保持原样，比猜一个字符更诚实。
 */

/* UTF-8 首字节 -> 该码点字节数；续字节 / 非法首字节返回 0。 */
inline int type_utf8_lead_bytes(unsigned char lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 0;
}

/* 追加一个码点。调用方保证 cp 不是代理、且 >= 0x20（控制字符在入口就挡掉了）。 */
inline void type_utf8_append(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

/* 删掉最后一个码点，返回删掉的字节数；缓冲不是完整 UTF-8 时返回 0 且不动。 */
inline size_t type_utf8_erase_last(std::string& text) {
  if (text.empty()) return 0;
  size_t start = text.size() - 1;
  size_t continuation = 0;
  while (start > 0 && continuation < 3 &&
         (static_cast<unsigned char>(text[start]) & 0xC0u) == 0x80u) {
    --start;
    ++continuation;
  }
  const int bytes = type_utf8_lead_bytes(static_cast<unsigned char>(text[start]));
  if (bytes <= 0 || static_cast<size_t>(bytes) != text.size() - start) return 0;
  text.erase(start);
  return static_cast<size_t>(bytes);
}

/*
 * 取下一个码点，返回消耗的字节数（0 = 文本结束或不是完整/合法 UTF-8）。
 * 只在预览度量里用；判定口径与引擎的 decode_utf8 一致（拒绝过长编码、代理对、
 * > U+10FFFF、孤立续字节），这样「预览能排、引擎拒收」不会出现。
 */
inline int type_utf8_next(const char* text, uint32_t* out_codepoint) {
  if (text == nullptr || out_codepoint == nullptr) return 0;
  const unsigned char lead = static_cast<unsigned char>(text[0]);
  const int bytes = type_utf8_lead_bytes(lead);
  if (bytes <= 0) return 0;
  for (int i = 1; i < bytes; ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) return 0;
  }
  uint32_t cp = 0;
  if (bytes == 1) {
    cp = lead;
  } else if (bytes == 2) {
    cp = (static_cast<uint32_t>(lead & 0x1Fu) << 6) |
         static_cast<uint32_t>(text[1] & 0x3F);
  } else if (bytes == 3) {
    cp = (static_cast<uint32_t>(lead & 0x0Fu) << 12) |
         (static_cast<uint32_t>(text[1] & 0x3F) << 6) |
         static_cast<uint32_t>(text[2] & 0x3F);
  } else {
    cp = (static_cast<uint32_t>(lead & 0x07u) << 18) |
         (static_cast<uint32_t>(text[1] & 0x3F) << 12) |
         (static_cast<uint32_t>(text[2] & 0x3F) << 6) |
         static_cast<uint32_t>(text[3] & 0x3F);
  }
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
  if (cp < 0x20u) return 0; /* 控制字符（含 Tab）：引擎拒收，预览也不认 */
  *out_codepoint = cp;
  return bytes;
}

/*
 * 字体轨道的预览宽度估计（em 比例）。外壳没有「只度量不落墨」的 ABI 入口，
 * 而每个字形的真实步进又依赖字体本身，所以预览框只能估：
 *   - 全角（CJK / 假名 / 谚文 / 全角标点）按 1 em（CJK 字体的实际步进就是 1 em）；
 *   - 其余（拉丁、希腊、符号）按 0.55 em（Noto Sans SC 的拉丁平均步进量级）。
 * 偏差在 ±5% 量级，只影响虚线预览框与状态栏的「≈」数字；像素永远以引擎为准。
 */
inline bool type_font_codepoint_is_wide(uint32_t cp) {
  return (cp >= 0x1100u && cp <= 0x115Fu) ||   /* 谚文字母 */
         (cp >= 0x2E80u && cp <= 0x9FFFu) ||   /* CJK 部首..统一表意文字 */
         (cp >= 0xAC00u && cp <= 0xD7A3u) ||   /* 谚文音节 */
         (cp >= 0xF900u && cp <= 0xFAFFu) ||   /* CJK 兼容表意文字 */
         (cp >= 0xFE30u && cp <= 0xFE4Fu) ||   /* CJK 兼容形式 */
         (cp >= 0xFF00u && cp <= 0xFF60u) ||   /* 全角 ASCII / 全角标点 */
         (cp >= 0xFFE0u && cp <= 0xFFE6u) ||   /* 全角符号 */
         (cp >= 0x20000u && cp <= 0x3FFFDdu);  /* 扩展 B 及以后 */
}

inline double type_font_estimate_advance_em(uint32_t cp) {
  return type_font_codepoint_is_wide(cp) ? 1.0 : 0.55;
}

/*
 * 字体轨道的文本框度量（镜像 build_font_layout 的**结构**，宽度是估计值）：
 *   - font_size 是 em 高（与 5×7 轨道的「格子高」不是同一个量纲）；
 *   - 行高 = (ascender - descender) * font_size（字号 48、Noto 约 0.65 时≈ 65 px）；
 *   - 多行行距 = font_size * line_height；
 *   - 字距只加在「非首字形」之间，与引擎一致。
 * ascender_em / descender_em 来自 comp_font_metrics（em 比例，descender 通常为负）。
 */
inline TypeBlockMetrics type_block_metrics_font(double font_size, double line_height,
                                                double letter_spacing, const char* text,
                                                double ascender_em, double descender_em) {
  TypeBlockMetrics out{};
  out.line_count = 1; /* 空文本也是「一行空行」，与内核 build_layout 同口径 */
  const double size = clamp_type_font_size(font_size);
  const double spacing = clamp_type_letter_spacing(letter_spacing, size);
  const double lh = clamp_type_line_height(line_height);
  const double ascender = (ascender_em > 0.0) ? ascender_em : 0.8;
  const double descender = (descender_em < 0.0) ? descender_em : -0.2;

  double widest = 0.0;
  double line_width = 0.0;
  int codepoints = 0;
  if (text != nullptr) {
    const char* cursor = text;
    uint32_t cp = 0;
    for (;;) {
      const int bytes = type_utf8_next(cursor, &cp);
      if (bytes <= 0) {
        /* '\n' 让 type_utf8_next 返回 0（控制字符），这里单独认一下换行。 */
        if (*cursor == '\n') {
          if (line_width > widest) widest = line_width;
          line_width = 0.0;
          if (codepoints > out.max_line_glyphs) out.max_line_glyphs = codepoints;
          out.last_line_glyphs = codepoints;
          codepoints = 0;
          ++out.line_count;
          ++cursor;
          continue;
        }
        break;
      }
      const double advance = type_font_estimate_advance_em(cp) * size;
      line_width += advance + (codepoints > 0 ? spacing : 0.0);
      ++codepoints;
      cursor += bytes;
    }
  }
  out.last_line_glyphs = codepoints;
  if (codepoints > out.max_line_glyphs) out.max_line_glyphs = codepoints;
  if (line_width > widest) widest = line_width;
  /* max_line_glyphs 在字体轨道按码点数统计（一个码点一个字形，缺字只占步进）。 */
  out.width = widest;
  out.advance = 0.0; /* 字体轨道没有单一「步距」：每个字形自己的 advance */
  out.height = (ascender - descender) * size +
               static_cast<double>(out.line_count - 1) * size * lh;
  return out;
}

/* 插入点光标在屏幕上的高度（文档字号 × 缩放，至少 1 px 可见）。 */
inline double type_caret_height(double font_size, double zoom) {
  const double size = clamp_type_font_size(font_size);
  const double height = size * (zoom > 0.0 ? zoom : 0.0);
  return height < 1.0 ? 1.0 : height;
}

/* ------------------------------------------------------------ 文字属性面板（S14） */

/*
 * 文字属性面板四行：字号 / 行距 / 字距 / 字体。
 *
 * 前三维是数值行（面板、快捷键、引擎三处同一套量程）；第 4 行是**开关**行
 * （5×7 点阵 <-> 已加载字体），没有数值量纲，- / = 在两种字体之间切换。
 * 行号只追加不改：既有三行的几何与命中顺序保持不变（追加型 ABI 的同一条规矩）。
 */
inline constexpr int kTypePanelRowCount = 4;
inline constexpr int kTypePanelFontSizeRow = 0;
inline constexpr int kTypePanelLineHeightRow = 1;
inline constexpr int kTypePanelLetterSpacingRow = 2;
inline constexpr int kTypePanelFontRow = 3;

/* 行距是倍率（1.0..8.0，界面显示为百分比）；面板一次步进 5%。 */
inline constexpr double kTypeLineHeightStep = 0.05;

inline const char* type_panel_row_label(int row) {
  switch (row) {
    case kTypePanelFontSizeRow: return "Font Size";
    case kTypePanelLineHeightRow: return "Line Height";
    case kTypePanelLetterSpacingRow: return "Letter Spacing";
    case kTypePanelFontRow: return "Typeface";
    default: return "";
  }
}

/* 行距按百分比显示，字号/字距按像素显示。 */
inline bool type_panel_row_is_percent(int row) { return row == kTypePanelLineHeightRow; }

/* 字体行是开关（数值量纲无意义）：数字键不进入输入态，- / = 直接切换。 */
inline bool type_panel_row_is_toggle(int row) { return row == kTypePanelFontRow; }

/* 按行夹取；字距的下限依赖当前字号，所以 font_size 必须一起传。 */
inline double type_panel_clamp_value(int row, double value, double font_size) {
  switch (row) {
    case kTypePanelFontSizeRow: return clamp_type_font_size(value);
    case kTypePanelLineHeightRow: return clamp_type_line_height(value);
    case kTypePanelLetterSpacingRow: return clamp_type_letter_spacing(value, font_size);
    default: return value;
  }
}

/* Alt+[ / ]：行距线性步进 5%（面板的主路径，键入态也可用）。 */
inline double step_type_line_height(double current, int direction) {
  const double base = clamp_type_line_height(current);
  if (direction > 0) return clamp_type_line_height(base + kTypeLineHeightStep);
  if (direction < 0) return clamp_type_line_height(base - kTypeLineHeightStep);
  return base;
}

inline double type_panel_step_value(int row, double value, double font_size, int direction) {
  switch (row) {
    case kTypePanelFontSizeRow: return step_type_font_size(value, direction);
    case kTypePanelLineHeightRow: return step_type_line_height(value, direction);
    case kTypePanelLetterSpacingRow:
      return step_type_letter_spacing(value, font_size, direction);
    default: return value;
  }
}

/* 面板行几何：与 draw_type_panel 共用，绘制与命中共用一套常量（同滤镜面板）。 */
inline constexpr int kTypeRowHeight = 20;
inline constexpr int kTypeRowsTop = 54; /* 标题 + 工具名两行 + 间距 */

inline Rect type_param_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kTypeRowsTop + row * kTypeRowHeight, panel.w, kTypeRowHeight};
}

inline int type_param_row_at(const Rect& panel, double x, double y) {
  if (!panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kTypeRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kTypeRowHeight)));
  if (row < 0 || row >= kTypePanelRowCount) return -1;
  return row;
}

/* ------------------------------------------------------------ 笔迹采样 */

/*
 * 笔迹去抖：文档坐标下两点距离小于 kStrokeMinDistance 时不追加，
 * 避免鼠标每像素事件都变成折点（一笔几千点会拖慢内核）。
 */
inline constexpr double kStrokeMinDistance = 0.75;

inline bool stroke_point_should_append(double last_x, double last_y, double x, double y) {
  const double dx = x - last_x;
  const double dy = y - last_y;
  return dx * dx + dy * dy >= kStrokeMinDistance * kStrokeMinDistance;
}

/*
 * 笔尖半径（客户区像素）：文档像素直径 x 缩放 / 2，下限 1 px。
 * 上限 1 px 只是为了保证「极小缩放时」圆仍然可见；放大时不做人为夹取
 * （32 倍下 500 px 笔刷的圆本来就该很大）。
 */
inline double brush_tip_radius(double diameter, double zoom) {
  if (!(diameter > 0.0) || !(zoom > 0.0)) return 1.0;
  const double radius = diameter * zoom * 0.5;
  return radius < 1.0 ? 1.0 : radius;
}

/* Smudge / Liquify 强度（comp_tool_warp.strength，0..1，0 = 逐字节不动）。 */
inline constexpr double kMinRetouchStrength = 0.01;
inline constexpr double kMaxRetouchStrength = 1.0;

inline double clamp_retouch_strength(double value) {
  if (!(value > 0.0)) return kMinRetouchStrength; /* NaN / 0 / 负数回落到下限 */
  return std::min(std::max(value, kMinRetouchStrength), kMaxRetouchStrength);
}

inline double step_retouch_strength(double current, int direction) {
  const double base = clamp_retouch_strength(current);
  if (direction > 0) return clamp_retouch_strength(base + 0.1);
  if (direction < 0) return clamp_retouch_strength(base - 0.1);
  return base;
}

/* ------------------------------------------------------------ 选区蚂蚁线 */

/*
 * 文档坐标（像素网格）上的边界线段。坐标是「像素边界」而不是像素中心：
 * 选中 [10,20) x [20,40) 的矩形，输出 x=10/30、y=20/40 四条边。
 */
struct BoundarySegment {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
};

/*
 * 从 8 位 coverage 蒙版提取「选中 / 未选中」之间的边界线段。
 *
 *   - mask 是文档分辨率灰度图（comp_selection_get_coverage 的输出），
 *     `stride` 是字节/行，必须 >= width；
 *   - `threshold` 是 1..255 的「算作选中」阈值，0 或越界按 1 处理；
 *   - 共线且连续的边界像素会被**合并**成一条线段（矩形只出 4 条），
 *     因此线段数的量级是「边界拐点」而不是「边界像素」。
 *
 * 返回值是**总线段数**（可能大于 capacity）；out 只写前 capacity 条。
 * 调用方据此判断是否被截断（复杂选区应放大缓冲或降采样蒙版）。
 * mask 为空 / 尺寸非正 / capacity < 0 时返回 0。
 */
inline int selection_boundary_segments(const uint8_t* mask, int width, int height,
                                       std::size_t stride, int threshold,
                                       BoundarySegment* out, int capacity) {
  if (mask == nullptr || width <= 0 || height <= 0 || capacity < 0) return 0;
  if (threshold < 1) threshold = 1;
  if (threshold > 255) threshold = 255;

  const auto inside = [&](int x, int y) {
    return mask[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)] >=
           static_cast<uint8_t>(threshold);
  };
  const auto emit = [&](double x0, double y0, double x1, double y1, int& count) {
    if (out != nullptr && count < capacity) {
      out[count] = BoundarySegment{x0, y0, x1, y1};
    }
    ++count;
  };

  int count = 0;
  /* 水平边：行 y 的上方与下方归属不同 -> 沿 x 合并成一条线段。 */
  for (int y = 0; y <= height; ++y) {
    int run_start = -1;
    for (int x = 0; x < width; ++x) {
      const bool above = y > 0 && inside(x, y - 1);
      const bool below = y < height && inside(x, y);
      if (above != below) {
        if (run_start < 0) run_start = x;
      } else if (run_start >= 0) {
        emit(static_cast<double>(run_start), static_cast<double>(y),
             static_cast<double>(x), static_cast<double>(y), count);
        run_start = -1;
      }
    }
    if (run_start >= 0) {
      emit(static_cast<double>(run_start), static_cast<double>(y),
           static_cast<double>(width), static_cast<double>(y), count);
    }
  }
  /* 垂直边：列 x 的左侧与右侧归属不同 -> 沿 y 合并成一条线段。 */
  for (int x = 0; x <= width; ++x) {
    int run_start = -1;
    for (int y = 0; y < height; ++y) {
      const bool left = x > 0 && inside(x - 1, y);
      const bool right = x < width && inside(x, y);
      if (left != right) {
        if (run_start < 0) run_start = y;
      } else if (run_start >= 0) {
        emit(static_cast<double>(x), static_cast<double>(run_start),
             static_cast<double>(x), static_cast<double>(y), count);
        run_start = -1;
      }
    }
    if (run_start >= 0) {
      emit(static_cast<double>(x), static_cast<double>(run_start),
           static_cast<double>(x), static_cast<double>(height), count);
    }
  }
  return count;
}

/* 虚线段在「沿线段长度」上的一段 [begin, end]，begin < end。 */
struct DashSpan {
  double begin = 0.0;
  double end = 0.0;
};

/*
 * 把长度 length 的线段切成 dash/gap 交替的虚线段，phase 是图案的相位
 * （像素，随时间递增得到「蚂蚁在爬」的观感）。phase 会被折到 [0, period)。
 *
 * 返回值是总段数（可能大于 capacity），out 只写前 capacity 段——
 * 与 selection_boundary_segments 同一套「两段式」口径。
 *
 * 退化输入一律返回 0：length <= 0、dash <= 0、gap < 0、NaN，以及
 * period 小于 1e-3 px（否则循环次数会失控）。
 */
inline int dash_spans(double length, double dash, double gap, double phase,
                      DashSpan* out, int capacity) {
  if (!(length > 0.0) || !(dash > 0.0) || !(gap >= 0.0) || !(phase == phase)) return 0;
  if (capacity < 0) return 0;
  const double period = dash + gap;
  if (!(period >= 1e-3)) return 0;

  double offset = std::fmod(phase, period);
  if (offset < 0.0) offset += period;

  int count = 0;
  for (double start = -offset; start < length; start += period) {
    const double begin = start > 0.0 ? start : 0.0;
    const double end = std::min(start + dash, length);
    if (end <= begin) continue;
    if (out != nullptr && count < capacity) {
      out[count] = DashSpan{begin, end};
    }
    ++count;
  }
  return count;
}

/* ------------------------------------------------------------ 选区工具（S7） */

/*
 * Marquee 的形状：M 键在矩形 / 椭圆之间循环。
 *
 * 上游 Marquee 有四角形与椭圆两种主形状（另有单行/单列），引擎的
 * comp_selection_apply_shape 直接支持 COMP_SELECTION_RECT / COMP_SELECTION_ELLIPSE，
 * 因此这里只暴露这两种——不做引擎不支持、只能「画个样子」的形状。
 */
enum MarqueeShape : int { kMarqueeRect = 0, kMarqueeEllipse = 1 };

inline int marquee_shape_count() { return 2; }

inline int marquee_shape_next(int shape) {
  return shape == kMarqueeRect ? kMarqueeEllipse : kMarqueeRect;
}

inline const char* marquee_shape_name(int shape) {
  return shape == kMarqueeEllipse ? "Ellipse" : "Rect";
}

/* 套索：自由手绘折线在松开时闭合成多边形，至少 3 点才有多边形语义。 */
inline constexpr int kLassoMinPoints = 3;

/*
 * 壳侧折线缓冲上限：只防「一笔几万点」把状态撑爆；引擎侧多边形上限是
 * kMaxSelectionPoints（262144），因此这个上限是壳的绘制/内存约束，不是语义约束。
 */
inline constexpr int kLassoMaxPoints = 8192;

inline bool lasso_should_close(int point_count) { return point_count >= kLassoMinPoints; }

inline bool lasso_can_append(int point_count) { return point_count < kLassoMaxPoints; }

/* 魔棒容差（0..255，逐通道距离，alpha 参与）：[ / ] 调，默认 32。 */
inline constexpr int kMinWandTolerance = 0;
inline constexpr int kMaxWandTolerance = 255;
inline constexpr int kDefaultWandTolerance = 32;

inline int clamp_wand_tolerance(int value) {
  if (value < kMinWandTolerance) return kMinWandTolerance;
  if (value > kMaxWandTolerance) return kMaxWandTolerance;
  return value;
}

/* 容差分档步进：小容差细调（1），中段 4，大容差 8——一档按一下手感够用。 */
inline int step_wand_tolerance(int tolerance, int direction) {
  const int current = clamp_wand_tolerance(tolerance);
  const int step = current < 16 ? 1 : (current < 64 ? 4 : 8);
  return clamp_wand_tolerance(current + (direction > 0 ? step : -step));
}

/* 新选区的羽化半径：0..250，与引擎 kMaxSelectionFeather 同口径。 */
inline constexpr int kMinSelectionFeather = 0;
inline constexpr int kMaxSelectionFeather = 250;

inline int clamp_selection_feather(int value) {
  if (value < kMinSelectionFeather) return kMinSelectionFeather;
  if (value > kMaxSelectionFeather) return kMaxSelectionFeather;
  return value;
}

/* 羽化步进：小半径 ±1、中段 ±5、大半径 ±10。 */
inline int step_selection_feather(int feather, int direction) {
  const int current = clamp_selection_feather(feather);
  const int step = current < 10 ? 1 : (current < 50 ? 5 : 10);
  return clamp_selection_feather(current + (direction > 0 ? step : -step));
}

/*
 * 选区扩展/收缩半径（P0-4）：1..500，与引擎 kMaxSelectionGrow 同口径。
 * 下限是 **1** 不是 0——`comp_selection_modify` 把 0 当 INVALID_ARGUMENT 拒掉
 * （羽化那边 0 有意义 = 清除羽化，扩展/收缩的 0 没有语义），所以面板要先把
 * 0 拦成提示，而不是把盖然无效果的调用打到引擎上。
 */
inline constexpr int kMinSelectionGrow = 1;
inline constexpr int kMaxSelectionGrow = 500;

inline int clamp_selection_grow(int value) {
  if (value < kMinSelectionGrow) return kMinSelectionGrow;
  if (value > kMaxSelectionGrow) return kMaxSelectionGrow;
  return value;
}

/* 扩展/收缩步进：小半径 ±1、中段 ±5、大半径 ±10（与羽化同一手感）。 */
inline int step_selection_grow(int radius, int direction) {
  const int current = clamp_selection_grow(radius);
  const int step = current < 10 ? 1 : (current < 50 ? 5 : 10);
  const int next = current + (direction > 0 ? step : -step);
  return next < kMinSelectionGrow ? kMinSelectionGrow : (next > kMaxSelectionGrow ? kMaxSelectionGrow : next);
}

/*
 * 选区平滑半径：1..100，与引擎 kMaxSelectionSmooth 同口径。
 *
 * 四条「修改」命令共用同一套键入态，但量程不同（扩展/收缩/边界到 500，
 * 平滑到 100），所以命令入口各自钳自己的上限（clamp_selection_grow /
 * clamp_selection_smooth），键入态只记用户敲的数字。
 */
inline constexpr int kMinSelectionSmooth = 1;
inline constexpr int kMaxSelectionSmooth = 100;

inline int clamp_selection_smooth(int value) {
  if (value < kMinSelectionSmooth) return kMinSelectionSmooth;
  if (value > kMaxSelectionSmooth) return kMaxSelectionSmooth;
  return value;
}

/* ------------------------------------------------------------ 工具选项栏（P0-10） */

/*
 * 选项栏只表达**当前工具**的参数，因此这里是一张「工具 -> 行」的静态映射，
 * 而不是可停靠的通用面板：
 *
 *   - 行集合必须是纯函数（tool_option_count / tool_option），绘制与命中都从它
 *     取数——两边各写一份表是「点中的行和画出来的行不是同一行」的来源。
 *   - 不重复右栏已有的入口：文字（字号/行距/字距/字体）仍在右栏，缩放的显示
 *     比例仍在工具条右侧；这里只放「改了要立刻看见画布变化」的手感参数。
 *     文字工具例外：右栏那四行是冻结的（kTypePanelRowCount == 4），段落框与
 *     对齐没有别的位置，只能放在选项栏（P0-6③）。
 *   - 行数少的工具（0–4 行）刻意不做换行/滚动：一行放不下就整组不画，点击
 *     自然落到空处（命中测试先看命中矩形，空矩形永不命中）。
 */
enum class ToolOptionId : int {
  None = 0,
  BrushSize,
  BrushHardness, /* 0..100 整数百分比；引擎侧是 0..1 的 double，由外壳换算 */
  WandTolerance,
  SelectionFeather,
  GradientType,
  GradientPreset, /* P1-7：多色标预设（只读表）+ 第 4 档「自定义」 */
  GradientDither, /* P1-7：确定性抖动开关，压 8 位渐变带 */
  GradientEdit,   /* P1-7：点开可编辑色标表（右栏面板，不是值） */
  ShapeKind,
  ShapeStrokeWidth,
  CloneSample, /* 0 = 当前图层，1 = 整篇合成 */
  CloneAligned,
  HealMode,
  TypeBoxWidth,  /* P0-6③：0 = 自动（点文字） */
  TypeBoxHeight, /* P0-6③：0 = 自动；> 0 只裁切不折行 */
  TypeAlign,     /* P0-6③：0 左 / 1 居中 / 2 右 */
  BrushPreset,   /* P1-4：只读预设表下标；0 = 自定义（不覆盖当前值） */
  BrushFlow,     /* P1-4：1..100 整数百分比；引擎侧是 0..1 的 double */
  BrushPressure  /* P1-4：0 关 / 1 开；开时整笔取笔压平均，鼠标恒 1.0 */
};

enum class ToolOptionKind : int {
  Stepper = 0, /* 左箭头 / 值 / 右箭头 */
  Choice = 1   /* 值本身就是按钮，点一下切下一档 */
};

struct ToolOptionSpec {
  ToolOptionId id = ToolOptionId::None;
  const char* label = "";
  ToolOptionKind kind = ToolOptionKind::Stepper;
  int min = 0;
  int max = 0;
  int step = 1;
  int choice_count = 0; /* 仅 Choice 有意义 */
};

/* 一行放几组、每组多宽：整数像素写死，绘制与命中共用同一组常量。 */
inline constexpr int kOptionLeftPad = 12;
inline constexpr int kOptionGroupWidth = 150;
inline constexpr int kOptionGroupGap = 8;
inline constexpr int kOptionLabelWidth = 56;
inline constexpr int kOptionArrowWidth = 18;
inline constexpr int kOptionValueWidth = 54;
inline constexpr int kOptionControlHeight = 22;

inline ToolOptionSpec tool_option(ToolId tool, int index) {
  const ToolOptionSpec size{ToolOptionId::BrushSize, "大小", ToolOptionKind::Stepper,
                            static_cast<int>(kMinBrushSize), static_cast<int>(kMaxBrushSize), 1, 0};
  const ToolOptionSpec hardness{ToolOptionId::BrushHardness, "硬度", ToolOptionKind::Stepper, 0,
                                100, 10, 0};
  /* P1-4：画笔/橡皮擦在「大小 / 硬度」之后追加三组（顺序是对外契约：
   * 0/1 的下标被 visual_check 的 options_bar 与 tests 钉死，只能尾部追加）。 */
  const ToolOptionSpec preset{ToolOptionId::BrushPreset, "预设", ToolOptionKind::Choice, 0,
                              kBrushPresetCount - 1, 1, kBrushPresetCount};
  const ToolOptionSpec flow{ToolOptionId::BrushFlow, "流量", ToolOptionKind::Stepper, 1, 100, 5, 0};
  const ToolOptionSpec pressure{ToolOptionId::BrushPressure, "压感", ToolOptionKind::Choice, 0, 1, 1,
                                2};
  switch (tool) {
    case ToolId::Brush:
    case ToolId::Eraser:
      if (index == 0) return size;
      if (index == 1) return hardness;
      if (index == 2) return preset;
      if (index == 3) return flow;
      if (index == 4) return pressure;
      break;
    case ToolId::Retouch:
    case ToolId::Tone:
    case ToolId::HistoryBrush:
      if (index == 0) return size;
      if (index == 1) return hardness;
      break;
    case ToolId::Heal:
      if (index == 0) return size;
      if (index == 1) return hardness;
      if (index == 2) {
        return ToolOptionSpec{ToolOptionId::HealMode, "模式", ToolOptionKind::Choice, 0,
                              kHealModeCount - 1, 1, kHealModeCount};
      }
      break;
    case ToolId::Clone:
      if (index == 0) return size;
      if (index == 1) return hardness;
      if (index == 2) {
        return ToolOptionSpec{ToolOptionId::CloneSample, "取样", ToolOptionKind::Choice, 0, 1, 1, 2};
      }
      if (index == 3) {
        return ToolOptionSpec{ToolOptionId::CloneAligned, "对齐", ToolOptionKind::Choice, 0, 1, 1, 2};
      }
      break;
    case ToolId::Marquee:
    case ToolId::Lasso:
      if (index == 0) {
        return ToolOptionSpec{ToolOptionId::SelectionFeather, "羽化", ToolOptionKind::Stepper,
                              kMinSelectionFeather, kMaxSelectionFeather, 1, 0};
      }
      break;
    case ToolId::Wand:
      if (index == 0) {
        return ToolOptionSpec{ToolOptionId::WandTolerance, "容差", ToolOptionKind::Stepper,
                              kMinWandTolerance, kMaxWandTolerance, 1, 0};
      }
      if (index == 1) {
        return ToolOptionSpec{ToolOptionId::SelectionFeather, "羽化", ToolOptionKind::Stepper,
                              kMinSelectionFeather, kMaxSelectionFeather, 1, 0};
      }
      break;
    case ToolId::Gradient:
      if (index == 0) {
        return ToolOptionSpec{ToolOptionId::GradientType, "类型", ToolOptionKind::Choice, 0,
                              kGradientTypeCount - 1, 1, kGradientTypeCount};
      }
      if (index == 1) {
        return ToolOptionSpec{ToolOptionId::GradientPreset, "色标", ToolOptionKind::Choice, 0,
                              kGradientPresetCount - 1, 1, kGradientPresetCount};
      }
      if (index == 2) {
        return ToolOptionSpec{ToolOptionId::GradientDither, "抖动", ToolOptionKind::Choice, 0, 1, 1,
                              2};
      }
      if (index == 3) {
        /*
         * 点一下打开色标表面板；值文本只做提示，不显示状态（表在右栏里）。
         * 包成 Stepper 而不是 Choice：它不是枚举值，Choice 的两项下限也套不上；
         * 左右箭头都落到同一个「打开」动作上，delta 被忽略。
         */
        return ToolOptionSpec{ToolOptionId::GradientEdit, "色标表", ToolOptionKind::Stepper, 0, 0, 1,
                              0};
      }
      break;
    case ToolId::Shape:
      if (index == 0) {
        return ToolOptionSpec{ToolOptionId::ShapeKind, "形状", ToolOptionKind::Choice, 0,
                              kShellShapeKindCount - 1, 1, kShellShapeKindCount};
      }
      if (index == 1) {
        return ToolOptionSpec{ToolOptionId::ShapeStrokeWidth, "线宽", ToolOptionKind::Stepper, 1,
                              50, 1, 0};
      }
      break;
    case ToolId::Type:
      /* 字号/行距/字距/字体在右栏；这里只放右栏放不下的段落参数。 */
      if (index == 0) {
        return ToolOptionSpec{ToolOptionId::TypeBoxWidth, "框宽", ToolOptionKind::Stepper, 0,
                              static_cast<int>(kTypeBoxMax),
                              static_cast<int>(kTypeBoxStep), 0};
      }
      if (index == 1) {
        return ToolOptionSpec{ToolOptionId::TypeBoxHeight, "框高", ToolOptionKind::Stepper, 0,
                              static_cast<int>(kTypeBoxMax),
                              static_cast<int>(kTypeBoxStep), 0};
      }
      if (index == 2) {
        return ToolOptionSpec{ToolOptionId::TypeAlign, "对齐", ToolOptionKind::Choice, 0,
                              kTypeAlignCount - 1, 1, kTypeAlignCount};
      }
      break;
    default:
      break;
  }
  return ToolOptionSpec{};
}

inline int tool_option_count(ToolId tool) {
  switch (tool) {
    case ToolId::Brush:
    case ToolId::Eraser:
      return 5; /* 大小 + 硬度 + 预设 + 流量 + 压感（P1-4） */
    case ToolId::Retouch:
    case ToolId::Tone:
    case ToolId::HistoryBrush:
      return 2;
    case ToolId::Heal:
      return 3;
    case ToolId::Clone:
      return 4;
    case ToolId::Marquee:
    case ToolId::Lasso:
      return 1;
    case ToolId::Wand:
      return 2;
    case ToolId::Gradient:
      return 4; /* 类型 + 色标预设 + 抖动 + 色标表 */
    case ToolId::Shape:
      return 2; /* 形状 + 线宽 */
    case ToolId::Type:
      return 3; /* 框宽 + 框高 + 对齐 */
    default:
      return 0;
  }
}

/*
 * 组矩形：从左往右排，**放不下就整组不画**（返回空矩形）。不做半宽裁切——
 * 半截的「大小 +」比没有更糟：用户会点在只剩 20px 的箭头上，以为在调参数。
 */
inline Rect option_group_rect(const Rect& bar, int index) {
  if (bar.empty() || index < 0) return Rect{};
  const int x = bar.x + kOptionLeftPad + index * (kOptionGroupWidth + kOptionGroupGap);
  if (x + kOptionGroupWidth > bar.right()) return Rect{};
  return Rect{x, bar.y, kOptionGroupWidth, bar.h};
}

/* 控件盒：标签右侧的「− 值 +」，垂直居中于整条高度（上下各留 5px 手感边距）。 */
inline Rect option_control_rect(const Rect& group) {
  if (group.empty()) return Rect{};
  const int height = std::min(kOptionControlHeight, group.h - 10);
  if (height <= 0) return Rect{};
  return Rect{group.x + kOptionLabelWidth, group.y + (group.h - height) / 2,
              kOptionArrowWidth * 2 + kOptionValueWidth, height};
}

/*
 * 命中：返回组下标（无命中 -1），*delta 给方向（-1 / +1 / 0）。
 *   - Stepper：只有左右两个箭头响应；点在数值上不动参数（PS 里数值区是文本输入，
 *     本仓的数值编辑在右栏，横栏里误触改参数比「点了没反应」更糟）。
 *   - Choice：整块响应，一律 direction = +1（循环到下一档）。
 */
inline int option_hit(const Rect& bar, ToolId tool, double x, double y, int* delta) {
  if (delta != nullptr) *delta = 0;
  const int count = tool_option_count(tool);
  for (int i = 0; i < count; ++i) {
    const Rect group = option_group_rect(bar, i);
    if (group.empty()) break; /* 后面的组更放不下 */
    const Rect control = option_control_rect(group);
    if (!control.contains(x, y)) continue;
    const ToolOptionSpec spec = tool_option(tool, i);
    if (spec.kind == ToolOptionKind::Choice) {
      if (delta != nullptr) *delta = 1;
      return i;
    }
    if (x < static_cast<double>(control.x + kOptionArrowWidth)) {
      if (delta != nullptr) *delta = -1;
      return i;
    }
    if (x >= static_cast<double>(control.right() - kOptionArrowWidth)) {
      if (delta != nullptr) *delta = 1;
      return i;
    }
    return -1;
  }
  return -1;
}

/* Choice 值的循环：direction > 0 向后、< 0 向前，绕环走（与右栏选择行同一手感）。 */
inline int option_cycle_choice(int current, int choice_count, int direction) {
  if (choice_count <= 0) return 0;
  int value = current;
  if (value < 0 || value >= choice_count) value = 0;
  if (direction > 0) return (value + 1) % choice_count;
  if (direction < 0) return (value + choice_count - 1) % choice_count;
  return value;
}


/*
 * 把工具 coverage 与选区蒙版逐像素相乘（255 = 全保留，0 = 全丢弃）。
 * 修补（Heal）的 coverage 由外壳自己光栅化，引擎的选区闸不覆盖这条路径，
 * 所以壳侧必须自己叠一次，否则「有选区时画笔受限、修补却涂满全画布」。
 * count 是像素数，调用方保证两个缓冲等长；任一为空指针返回 false 且不写字节。
 */
inline bool intersect_coverage_with_selection(uint8_t* coverage, const uint8_t* mask,
                                              size_t count) {
  if (coverage == nullptr || mask == nullptr) return false;
  for (size_t i = 0; i < count; ++i) {
    const unsigned value =
        static_cast<unsigned>(coverage[i]) * static_cast<unsigned>(mask[i]);
    coverage[i] = static_cast<uint8_t>((value + 127u) / 255u); /* 四舍五入，255*255 仍是 255 */
  }
  return true;
}

/*
 * 文档坐标 (x, y) 是否落在选区**内部**（coverage >= threshold，与蚂蚁线同一口径 128）。
 *
 * Move 工具用它分流：有选区时，落点在选区内 = 搬选区像素；落在选区外 = 搬整层
 * （上游 V 的语义）。坐标是连续值，取整用 floor——面板/画布的变换算出的小数点
 * 不应让边界像素来回跳。
 *
 * 越界（点在画布外）一律算「不在选区内」：画布外没有选中像素可搬，而用户在画布
 * 边缘外拖动时想动的是整层。mask 为空 / 尺寸非正时也返回 false，但调用方必须先
 * 自己判断「蒙版是不是根本读不到」——那种情况下 false 的含义是「不知道」，应退回
 * 旧行为而不是改走整层拖动。
 */
inline bool selection_contains_point(const uint8_t* mask, int width, int height,
                                     size_t stride, double x, double y, int threshold) {
  if (mask == nullptr || width <= 0 || height <= 0) return false;
  if (threshold < 1) threshold = 1;
  if (threshold > 255) threshold = 255;
  /* 写成 !(x >= 0) 而不是 x < 0：NaN 两个都比较为假，必须显式判成「不在选区内」。 */
  if (!(x >= 0.0) || !(y >= 0.0)) return false;
  if (!(x < static_cast<double>(width)) || !(y < static_cast<double>(height))) return false;
  const int px = static_cast<int>(x);
  const int py = static_cast<int>(y);
  return mask[static_cast<size_t>(py) * stride + static_cast<size_t>(px)] >=
         static_cast<uint8_t>(threshold);
}

/* ------------------------------------------------------- 选区手柄（S46） */

/*
 * 八个手柄的下标：0=左上 1=上 2=右上 3=右 4=右下 5=下 6=左下 7=左。
 * **偶数都是角、奇数都是边**——拖拽的等比锁定只对偶数生效，绘制与命中表
 * 都按这个顺序排列，不要在中途插入新值。
 */
inline constexpr int kSelectionHandleCount = 8;

/* 手柄在**文档坐标**里的锚点：四角 + 四边中点。矩形是左闭右开的像素网格，
 * 所以右/下边取 x + w / y + h（与 CanvasView::doc_to_screen 同一口径）。
 * 索引越界返回矩形中心，调用方不该依赖这个兜底。 */
inline Point selection_handle_point(const Rect& rect, int index) {
  const double x0 = static_cast<double>(rect.x);
  const double y0 = static_cast<double>(rect.y);
  const double x1 = static_cast<double>(rect.x + rect.w);
  const double y1 = static_cast<double>(rect.y + rect.h);
  const double cx = (x0 + x1) * 0.5;
  const double cy = (y0 + y1) * 0.5;
  switch (index) {
    case 0: return Point{x0, y0};
    case 1: return Point{cx, y0};
    case 2: return Point{x1, y0};
    case 3: return Point{x1, cy};
    case 4: return Point{x1, y1};
    case 5: return Point{cx, y1};
    case 6: return Point{x0, y1};
    case 7: return Point{x0, cy};
    default: return Point{cx, cy};
  }
}

/*
 * 屏幕坐标命中：centers 由调用方用 doc_to_screen 把手柄锚点换算好。
 * 手柄大小是**屏幕像素**（不随缩放变），所以命中也在屏幕空间做；
 * 返回最近的、距离不超过 tolerance 的手柄，都没有则 -1。
 */
inline int selection_handle_hit(const Point* centers, int count, Point p, double tolerance) {
  if (centers == nullptr || count <= 0 || !(tolerance > 0.0)) return -1;
  if (!(p.x == p.x) || !(p.y == p.y)) return -1; /* NaN：绝不命中 */
  int best = -1;
  double best_d2 = tolerance * tolerance;
  for (int i = 0; i < count; ++i) {
    const double dx = p.x - centers[i].x;
    const double dy = p.y - centers[i].y;
    const double d2 = dx * dx + dy * dy;
    if (d2 <= best_d2) {
      best = i;
      best_d2 = d2;
    }
  }
  return best;
}

/*
 * 手柄拖拽的结果矩形（**文档坐标**，整数像素，可以落在画布外——裁切是引擎的事）。
 *
 * 规则与 PS 的手柄一致：被拖的那条边/那个角跟着鼠标走，对边不动；
 * lock_aspect 只对**角**生效（保持源矩形宽高比，锚点是对角线另一端），
 * 边手柄忽略它（PS 里边手柄也不等比）。
 *
 * 拖过头（越过对边）夹到 1 像素并停住，**不做镜像翻转**——引擎的
 * comp_selection_set_bounds 也只有「收成空」这一种退化语义。
 * 传 NaN 或 index 越界时原样返回 source（调用方应当已经命中过手柄）。
 */
inline Rect selection_drag_rect(const Rect& source, int index, Point point, bool lock_aspect) {
  if (source.empty() || index < 0 || index >= kSelectionHandleCount) return source;
  if (!(point.x == point.x) || !(point.y == point.y)) return source;

  const bool moves_left = index == 0 || index == 6 || index == 7;
  const bool moves_right = index == 2 || index == 3 || index == 4;
  const bool moves_top = index == 0 || index == 1 || index == 2;
  const bool moves_bottom = index == 4 || index == 5 || index == 6;

  double left = static_cast<double>(source.x);
  double top = static_cast<double>(source.y);
  double right = static_cast<double>(source.x + source.w);
  double bottom = static_cast<double>(source.y + source.h);

  if (lock_aspect && (index % 2) == 0) {
    const double aspect =
        static_cast<double>(source.w) / static_cast<double>(source.h);
    const double anchor_x = moves_left ? right : left;
    const double anchor_y = moves_top ? bottom : top;
    const double dx = std::fabs(point.x - anchor_x);
    const double dy = std::fabs(point.y - anchor_y);
    double width = std::max(dx, dy * aspect);
    if (!(width > 1.0)) width = 1.0;
    const double height = width / aspect;
    if (moves_left) {
      left = anchor_x - width;
      right = anchor_x;
    } else {
      left = anchor_x;
      right = anchor_x + width;
    }
    if (moves_top) {
      top = anchor_y - height;
      bottom = anchor_y;
    } else {
      top = anchor_y;
      bottom = anchor_y + height;
    }
  } else {
    if (moves_left) left = point.x;
    if (moves_right) right = point.x;
    if (moves_top) top = point.y;
    if (moves_bottom) bottom = point.y;
  }

  /* 夹到至少 1 像素：先夹再取整，round 的单调性保证结果宽度 >= 1。 */
  if (!(right - left >= 1.0)) {
    if (moves_left) {
      left = right - 1.0;
    } else {
      right = left + 1.0;
    }
  }
  if (!(bottom - top >= 1.0)) {
    if (moves_top) {
      top = bottom - 1.0;
    } else {
      bottom = top + 1.0;
    }
  }

  const long x0 = std::lround(left);
  const long y0 = std::lround(top);
  const long x1 = std::lround(right);
  const long y1 = std::lround(bottom);
  return Rect{static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
              static_cast<int>(y1 - y0)};
}

/*
 * 色彩范围容差（P0-4 第二批）：0..200，与引擎 kMaxColorRangeFuzziness 同口径。
 * 缺省 40 抄自上游 `ColorRangeEdit.fuzziness`；**0 是合法值**（只选完全同色），
 * 与扩展/收缩的下限 1 不同，别把两个夹取函数合并。
 */
inline constexpr int kMinColorRangeFuzziness = 0;
inline constexpr int kMaxColorRangeFuzziness = 200;
inline constexpr int kDefaultColorRangeFuzziness = 40;

inline int clamp_color_range_fuzziness(int value) {
  if (value < kMinColorRangeFuzziness) return kMinColorRangeFuzziness;
  if (value > kMaxColorRangeFuzziness) return kMaxColorRangeFuzziness;
  return value;
}

/* 容差步进：<20 走 1、<100 走 5、其余走 10（同一手感，量程 200 不至于走不动）。 */
inline int step_color_range_fuzziness(int fuzziness, int direction) {
  const int current = clamp_color_range_fuzziness(fuzziness);
  const int step = current < 20 ? 1 : (current < 100 ? 5 : 10);
  return clamp_color_range_fuzziness(current + (direction > 0 ? step : -step));
}

/* ------------------------------------------------------------ 钢笔路径（S8） */

/*
 * 钢笔工具（P）：点击落锚点、按住拖动拉出对称控制柄，点第一个锚点闭合。
 *
 * 引擎 comp_selection_apply_path 收的是三次贝塞尔段，所以壳侧只维护锚点与
 * **相对锚点**的控制柄（装配段时再加回去）。本层不分配内存：锚点数组由
 * 调用方持有（壳里是 ShellState 的 std::vector），所有函数带容量参数。
 *
 * 段语义与引擎一致：选中路径按**闭合**子路径解释（钢笔选区总是闭合）。
 */
inline constexpr int kPenMaxAnchors = 2048;
inline constexpr int kPenPreviewSamples = 12; /* 每段折线采样数（绘制预览用） */

struct PenAnchor {
  Point position{};   /* 文档坐标 */
  Point handle_in{};  /* 入向控制柄，相对 position */
  Point handle_out{}; /* 出向控制柄，相对 position */
};

struct PenSegment {
  Point start{};
  Point control1{};
  Point control2{};
  Point end{};
};

inline bool pen_can_append(int anchor_count) {
  return anchor_count >= 0 && anchor_count < kPenMaxAnchors;
}

/* 追加一个直角锚点（无控制柄）；容量满时不改写、返回 false。 */
inline bool pen_append_anchor(PenAnchor* anchors, int* anchor_count, Point position) {
  if (anchors == nullptr || anchor_count == nullptr) return false;
  if (!pen_can_append(*anchor_count)) return false;
  PenAnchor& anchor = anchors[*anchor_count];
  anchor.position = position;
  anchor.handle_in = Point{0.0, 0.0};
  anchor.handle_out = Point{0.0, 0.0};
  ++(*anchor_count);
  return true;
}

/*
 * 拖动最后一个锚点的控制柄：pointer 是当前文档坐标。
 * Photoshop 口径是**对称平滑点**：出向 = pointer - anchor，入向 = -出向。
 */
inline void pen_drag_handle(PenAnchor* anchors, int anchor_count, Point pointer) {
  if (anchors == nullptr || anchor_count <= 0) return;
  PenAnchor& anchor = anchors[anchor_count - 1];
  anchor.handle_out = Point{pointer.x - anchor.position.x, pointer.y - anchor.position.y};
  anchor.handle_in = Point{-anchor.handle_out.x, -anchor.handle_out.y};
}

/* 闭合判定：光标落在第一个锚点的 radius 文档半径内（至少要 2 个锚点才闭合）。 */
inline bool pen_hits_first_anchor(const PenAnchor* anchors, int anchor_count, Point pointer,
                                  double radius) {
  if (anchors == nullptr || anchor_count < 2 || !(radius >= 0.0)) return false;
  const double dx = pointer.x - anchors[0].position.x;
  const double dy = pointer.y - anchors[0].position.y;
  return dx * dx + dy * dy <= radius * radius;
}

/* 段数：闭合路径 n 个锚点 => n 段（含末锚点回首锚点的闭合段）；开放路径 => n-1 段。 */
inline int pen_segment_count(int anchor_count, bool closed) {
  if (anchor_count < 2) return 0;
  return closed ? anchor_count : anchor_count - 1;
}

/*
 * 取第 index 段的几何（index 允许等于 anchor_count-1 表示闭合段）。
 * 越界返回全零段；供绘制预览时逐段取用，避免为整条路径开大缓冲。
 */
inline PenSegment pen_segment_at(const PenAnchor* anchors, int anchor_count, int index) {
  PenSegment segment{};
  if (anchors == nullptr || anchor_count < 2 || index < 0 || index >= anchor_count) {
    return segment;
  }
  const PenAnchor& from = anchors[index];
  const PenAnchor& to = anchors[(index + 1) % anchor_count];
  segment.start = from.position;
  segment.control1 =
      Point{from.position.x + from.handle_out.x, from.position.y + from.handle_out.y};
  segment.control2 = Point{to.position.x + to.handle_in.x, to.position.y + to.handle_in.y};
  segment.end = to.position;
  return segment;
}

/*
 * 装配三次贝塞尔段。返回**总段数**（容量不足时只写前 capacity 段），
 * 计数语义与 selection_boundary_segments 一致，调用方按返回值判断截断。
 */
inline int pen_build_segments(const PenAnchor* anchors, int anchor_count, bool closed,
                              PenSegment* out, int capacity) {
  const int total = pen_segment_count(anchor_count, closed);
  if (anchors == nullptr || out == nullptr || capacity <= 0 || total <= 0) return total;
  const int written = std::min(total, capacity);
  for (int i = 0; i < written; ++i) {
    out[i] = pen_segment_at(anchors, anchor_count, i);
  }
  return total;
}

inline Point pen_cubic_at(const PenSegment& segment, double t) {
  const double u = 1.0 - t;
  const double w0 = u * u * u;
  const double w1 = 3.0 * u * u * t;
  const double w2 = 3.0 * u * t * t;
  const double w3 = t * t * t;
  return Point{
      segment.start.x * w0 + segment.control1.x * w1 + segment.control2.x * w2 +
          segment.end.x * w3,
      segment.start.y * w0 + segment.control1.y * w1 + segment.control2.y * w2 +
          segment.end.y * w3};
}

/*
 * 预览折线：每段等分采样（不含 t=0 的重复起点，首点单独写入），
 * 闭合时把闭合段一起采样。返回**总点数**（容量不足时截断写入）。
 */
inline int pen_preview_polyline(const PenAnchor* anchors, int anchor_count, bool closed,
                                Point* out, int capacity,
                                int samples_per_segment = kPenPreviewSamples) {
  const int segments = pen_segment_count(anchor_count, closed);
  if (anchors == nullptr || out == nullptr || capacity <= 0 || segments <= 0) return 0;
  if (samples_per_segment < 1) samples_per_segment = 1;

  const int total = 1 + segments * samples_per_segment;
  int written = 0;
  if (written < capacity) out[written++] = anchors[0].position;
  for (int i = 0; i < segments && written < capacity; ++i) {
    const PenAnchor& from = anchors[i];
    const PenAnchor& to = anchors[(i + 1) % anchor_count];
    PenSegment segment{};
    segment.start = from.position;
    segment.control1 = Point{from.position.x + from.handle_out.x,
                             from.position.y + from.handle_out.y};
    segment.control2 = Point{to.position.x + to.handle_in.x, to.position.y + to.handle_in.y};
    segment.end = to.position;
    for (int sample = 1; sample <= samples_per_segment && written < capacity; ++sample) {
      const double t = static_cast<double>(sample) / static_cast<double>(samples_per_segment);
      out[written++] = pen_cubic_at(segment, t);
    }
  }
  return total;
}

/* ------------------------------------------------------------ 魔棒取样窗口（S8） */

/*
 * 参考色邻域半径（0..512）：引擎按 (2r+1)×(2r+1) 窗口取均值当参考色再泛洪，
 * 壳层只负责档位与状态栏。0 = 只取种子像素（S7 的行为）。
 */
inline constexpr int kMinWandRadius = 0;
inline constexpr int kMaxWandRadius = 512;
inline constexpr int kDefaultWandRadius = 0;

inline int clamp_wand_radius(int value) {
  if (value < kMinWandRadius) return kMinWandRadius;
  if (value > kMaxWandRadius) return kMaxWandRadius;
  return value;
}

/* 半径分档：0,1,2,4,8,16,32,64,128,256,512——每次按键跨一档，够粗也够细。 */
inline int step_wand_radius(int radius, int direction) {
  const int current = clamp_wand_radius(radius);
  int next = current;
  if (direction > 0) {
    if (current < 1) {
      next = 1;
    } else if (current < 2) {
      next = 2;
    } else {
      next = current * 2;
    }
  } else if (direction < 0) {
    if (current <= 1) {
      next = 0;
    } else if (current <= 2) {
      next = 1;
    } else {
      next = current / 2;
    }
  }
  return clamp_wand_radius(next);
}

/* ------------------------------------------------------------ 混合模式（S21） */

/*
 * 混合模式档位与名称：数值即 comp_blend_mode 枚举（0..23，ABI 冻结表），
 * 顺序逐字对齐上游 LayerAppearance.swift 的 `LayerBlendMode.allCases` 声明序——
 * 上游 Shift++ / Shift+− 就是按这个序循环、一步一撤销，可直接移植。
 *
 * 名称取上游 rawValue 的规范拼写（含 "Linear Dodge (Add)" 的全称）；分组沿用
 * 上游 `LayerBlendMode.groups`（Normal / 加深 / 减淡 / 对比 / 比较 / 分量），
 * 状态栏与菜单据此显示分组归属。
 */
inline constexpr int kBlendNormal = 0;
inline constexpr int kBlendCount = 24;

inline int clamp_blend_mode(int mode) {
  if (mode < 0) return 0;
  if (mode >= kBlendCount) return kBlendCount - 1;
  return mode;
}

/* 菜单序步进并回绕（对齐上游 cycleBlendMode 的取模）。 */
inline int step_blend_mode(int mode, int direction) {
  if (direction == 0) return clamp_blend_mode(mode);
  const int count = kBlendCount;
  /* 先归一再取模，避免负数与来回多步时的回绕偏差。 */
  int next = clamp_blend_mode(mode) + (direction > 0 ? 1 : -1);
  next %= count;
  if (next < 0) next += count;
  return next;
}

inline const char* blend_mode_name(int mode) {
  switch (clamp_blend_mode(mode)) {
    case 0: return "Normal";
    case 1: return "Darken";
    case 2: return "Multiply";
    case 3: return "Color Burn";
    case 4: return "Linear Burn";
    case 5: return "Lighten";
    case 6: return "Screen";
    case 7: return "Color Dodge";
    case 8: return "Linear Dodge (Add)";
    case 9: return "Overlay";
    case 10: return "Soft Light";
    case 11: return "Hard Light";
    case 12: return "Vivid Light";
    case 13: return "Linear Light";
    case 14: return "Pin Light";
    case 15: return "Hard Mix";
    case 16: return "Difference";
    case 17: return "Exclusion";
    case 18: return "Subtract";
    case 19: return "Divide";
    case 20: return "Hue";
    case 21: return "Saturation";
    case 22: return "Color";
    case 23: return "Luminosity";
    default: return "Normal";
  }
}

/*
 * 上游分组编号（同 LayerBlendMode.groups）：0 Normal / 1 加深 / 2 减淡 /
 * 3 对比 / 4 比较 / 5 分量。菜单在同一组内不画分隔线。
 */
inline constexpr int kBlendGroups[7][8] = {
    {0, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 3, 4, -1, -1, -1, -1},
    {5, 6, 7, 8, -1, -1, -1, -1},
    {9, 10, 11, 12, 13, 14, 15, -1},
    {16, 17, 18, 19, -1, -1, -1, -1},
    {20, 21, 22, 23, -1, -1, -1, -1},
};
inline constexpr int kBlendGroupCount = 6;

inline int blend_group_of(int mode) {
  const int value = clamp_blend_mode(mode);
  for (int group = 0; group < kBlendGroupCount; ++group) {
    for (int slot = 0; slot < 8; ++slot) {
      if (kBlendGroups[group][slot] == value) return group;
      if (kBlendGroups[group][slot] < 0) break;
    }
  }
  return 0;
}

inline const char* blend_group_name(int group) {
  switch (group) {
    case 0: return "Normal";
    case 1: return "加深";
    case 2: return "减淡";
    case 3: return "对比";
    case 4: return "比较";
    case 5: return "分量";
    default: return "";
  }
}

/* ------------------------------------------------------------ 图层变换（S21） */

/*
 * 图层变换的按键步进量（对齐上游 Free Transform 的微调粒度，均为文档像素/度）：
 *   Shift+方向  = 平移 1 px（微调），方向键单独 = 平移 10 px（对齐上游方向键步长）
 *   Ctrl+左右   = 旋转 15 度（Shift 再加速为 1 度精调）
 *   Alt+[ / ]   = 缩放 ±1 档（保持宽高比，锚点为图层中心）
 *
 * 只做「步进量」的纯函数，真正的 ABI 写入在壳侧：变换受 is_valid_transform 约束
 * （size 1..300000、|origin| <= 1000000、rotation 有限），壳必须夹取后再写入，
 * 否则脏值会被引擎拒绝，面板与文档状态就会分叉。
 */
inline constexpr int kLayerTransformMoveStep = 1;   /* Shift+方向：1 px 微调 */
inline constexpr int kLayerTransformMoveCoarse = 10; /* 方向键：10 px */
inline constexpr int kLayerTransformRotateStep = 15; /* Ctrl+左右：15 度 */
inline constexpr double kLayerTransformScaleStep = 0.01; /* Alt+[ ]：每档 1% */
inline constexpr double kLayerTransformScaleMin = 0.01;  /* 1% */
inline constexpr double kLayerTransformScaleMax = 32.0;  /* 3200% */

inline int layer_transform_move_step(bool coarse) {
  return coarse ? kLayerTransformMoveCoarse : kLayerTransformMoveStep;
}

/*
 * 旋转步进并夹取到 (-360, 360) 之外不收窄：超过 360 维持单调，读取时再归一化。
 * 上游 LayerTransform.isValid 只要求 finite，不限制圈数，这里不收口以保持连续性。
 */
inline double layer_transform_rotate_step(double rotation, bool fine) {
  if (!(rotation > -1e18 && rotation < 1e18)) return 0.0; /* NaN / inf 归一化 */
  const double step = fine ? 1.0 : static_cast<double>(kLayerTransformRotateStep);
  return rotation + step;
}

inline double clamp_layer_scale(double scale) {
  if (!(scale == scale) || scale < kLayerTransformScaleMin) {
    return kLayerTransformScaleMin;
  }
  if (scale > kLayerTransformScaleMax) return kLayerTransformScaleMax;
  return scale;
}

/*
 * 图层矩形（可旋转）是否还与画布有交集。给 Move 拖动的「拖出画布」提示用：
 * 图层被拖到完全离开文档后，画面上一点痕迹都没有，不提示就像把图层拖丢了。
 *
 * 先把旋转后的矩形取 AABB（保守上界）再与 [0, doc_w) × [0, doc_h) 求交。
 * 保守 = AABB 还相交就不提示，宁可少报也不误报（旋转 45° 的大图 AABB 明明还在
 * 画布上时，报「已拖出」会比不报更让人困惑）。
 * 宽高或画布不为正（读不到有效 transform）、角度非有限时返回 true：判断不了就不提示。
 */
inline bool layer_rect_touches_canvas(int32_t x, int32_t y, int32_t w, int32_t h,
                                      double rotation_deg, int doc_w, int doc_h) {
  if (w <= 0 || h <= 0 || doc_w <= 0 || doc_h <= 0) return true;
  if (!std::isfinite(rotation_deg)) return true; /* 脏角度：判断不了就不提示 */
  double half_w = static_cast<double>(w) * 0.5;
  double half_h = static_cast<double>(h) * 0.5;
  if (std::fabs(rotation_deg) > 1e-9) {
    constexpr double kPi = 3.14159265358979323846;
    const double rad = rotation_deg * kPi / 180.0;
    const double c = std::fabs(std::cos(rad));
    const double s = std::fabs(std::sin(rad));
    const double rotated_w = half_w * c + half_h * s;
    const double rotated_h = half_w * s + half_h * c;
    half_w = rotated_w;
    half_h = rotated_h;
  }
  /* transform.x/y 是图层左上角（heal_layer_point 用同一口径），中心 = 左上 + 半宽高。 */
  const double center_x = static_cast<double>(x) + static_cast<double>(w) * 0.5;
  const double center_y = static_cast<double>(y) + static_cast<double>(h) * 0.5;
  const bool right_of_canvas = center_x - half_w >= static_cast<double>(doc_w);
  const bool left_of_canvas = center_x + half_w <= 0.0;
  const bool below_canvas = center_y - half_h >= static_cast<double>(doc_h);
  const bool above_canvas = center_y + half_h <= 0.0;
  return !(right_of_canvas || left_of_canvas || below_canvas || above_canvas);
}

/*
 * 缩放步进：按当前宽高的等比例增量，锚点为图层中心（origin 随之收缩/展开，
 * 保持中心不动——上游拖角缩放的行为，比左上角固定更符合直觉）。
 */
inline void layer_transform_scale_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                                       double factor) {
  if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) return;
  if (*w <= 0 || *h <= 0) return;
  const double scale = clamp_layer_scale(factor);
  const int32_t new_w = static_cast<int32_t>(std::lround(static_cast<double>(*w) * scale));
  const int32_t new_h = static_cast<int32_t>(std::lround(static_cast<double>(*h) * scale));
  if (new_w < 1 || new_h < 1) return; /* 尺寸下限由引擎校验，壳不越界 */
  /* 中心保持：新原点 = 旧中心 - 新尺寸/2。 */
  const double cx = static_cast<double>(*x) + static_cast<double>(*w) / 2.0;
  const double cy = static_cast<double>(*y) + static_cast<double>(*h) / 2.0;
  *x = static_cast<int32_t>(std::lround(cx - static_cast<double>(new_w) / 2.0));
  *y = static_cast<int32_t>(std::lround(cy - static_cast<double>(new_h) / 2.0));
  *w = new_w;
  *h = new_h;
}

/* ------------------------------------------------- 图层变换手柄（S39） */

/*
 * 变换框：8 个缩放手柄（顺时针：左上、上、右上、右、右下、下、左下、左）
 * + 1 个旋转旋钮（上边中点外侧）。索引与绘制顺序一致，壳与单测共用一套定义。
 *
 * 坐标系：rect 是**未旋转**的文档矩形，rotation 为度（顺时针，与 ABI 同向），
 * 旋转中心恒为矩形中心——引擎 Layer::map_document_to_pixel 先归一化到中心再逆
 * 旋转，所以手柄必须绕中心转；绕左上角转出来的框会与画面错位。
 *
 * 缩放保持「对角锚点不动」：拖右边时左边固定，拖左上角时右下角固定。旋转过的
 * 图层锚点也会跟着转，因此算完新局部矩形后还要把中心回移
 * (anchor_doc - R(rotation)·anchor_local_new)，只改 x/y 不够。
 */
inline constexpr int kTransformHandleCount = 8;
inline constexpr int kTransformHandleNone = -1;
inline constexpr int kTransformHandleRotate = kTransformHandleCount; /* 8 */
inline constexpr int kTransformHandleMinSize = 1;                   /* 文档像素 */

inline Point rect_center_point(const Rect& rect) {
  return Point{static_cast<double>(rect.x) + static_cast<double>(rect.w) * 0.5,
               static_cast<double>(rect.y) + static_cast<double>(rect.h) * 0.5};
}

/* 绕 center 旋转 degrees（y 向下 = 顺时针）。 */
inline Point rotate_doc_point(Point point, Point center, double degrees) {
  if (!std::isfinite(degrees) || std::fabs(degrees) <= 1e-12) return point;
  constexpr double kPi = 3.14159265358979323846;
  const double rad = degrees * kPi / 180.0;
  const double cos_t = std::cos(rad);
  const double sin_t = std::sin(rad);
  const double dx = point.x - center.x;
  const double dy = point.y - center.y;
  return Point{center.x + dx * cos_t - dy * sin_t, center.y + dx * sin_t + dy * cos_t};
}

/* 手柄在未旋转矩形上的位置（文档坐标）。 */
inline Point transform_local_point(const Rect& rect, int handle) {
  const double left = static_cast<double>(rect.x);
  const double top = static_cast<double>(rect.y);
  const double right = static_cast<double>(rect.right());
  const double bottom = static_cast<double>(rect.bottom());
  const double mid_x = (left + right) * 0.5;
  const double mid_y = (top + bottom) * 0.5;
  switch (handle) {
    case 0: return Point{left, top};
    case 1: return Point{mid_x, top};
    case 2: return Point{right, top};
    case 3: return Point{right, mid_y};
    case 4: return Point{right, bottom};
    case 5: return Point{mid_x, bottom};
    case 6: return Point{left, bottom};
    case 7: return Point{left, mid_y};
    default: return Point{mid_x, mid_y};
  }
}

/* 手柄绕矩形中心旋转后的实际落点（画面上看到的那个小方块）。 */
inline Point transform_handle_point(const Rect& rect, double rotation, int handle) {
  return rotate_doc_point(transform_local_point(rect, handle), rect_center_point(rect),
                          rotation);
}

/* 旋转旋钮：上边中点向局部 -y 方向偏移 offset 后再整体旋转。 */
inline Point transform_rotate_knob_point(const Rect& rect, double rotation, double offset) {
  const Point center = rect_center_point(rect);
  const Point local{center.x, static_cast<double>(rect.y) - offset};
  return rotate_doc_point(local, center, rotation);
}

/* 半径内最近的手柄；没有命中返回 kTransformHandleNone。 */
inline int transform_handle_at(const Rect& rect, double rotation, Point doc, double radius) {
  if (rect.empty() || !std::isfinite(rotation) || !(radius > 0.0)) {
    return kTransformHandleNone;
  }
  int best = kTransformHandleNone;
  double best_distance = radius * radius;
  for (int handle = 0; handle < kTransformHandleCount; ++handle) {
    const Point point = transform_handle_point(rect, rotation, handle);
    const double dx = point.x - doc.x;
    const double dy = point.y - doc.y;
    const double distance = dx * dx + dy * dy;
    if (distance <= best_distance) {
      best_distance = distance;
      best = handle;
    }
  }
  return best;
}

inline bool transform_knob_at(const Rect& rect, double rotation, Point doc, double offset,
                              double radius) {
  if (rect.empty() || !std::isfinite(rotation) || !(radius > 0.0)) return false;
  const Point knob = transform_rotate_knob_point(rect, rotation, offset);
  const double dx = knob.x - doc.x;
  const double dy = knob.y - doc.y;
  return dx * dx + dy * dy <= radius * radius;
}

/*
 * 拖手柄缩放：两步——
 *   1. 把指针逆旋转回局部系，按手柄改对应的边，再把宽高压到 min_size 以上；
 *   2. 以「对角锚点」在文档里的旧位置为准，回移新矩形原点，补偿中心变化。
 *
 * 每帧都从按下时的 base 重新求，不在上一帧结果上叠加：来回拖动不会因取整漂移。
 * handle 非法 / rect 为空 / 角度非有限时原样返回。
 *
 * uniform = Shift 等比（S43）：只对**角**手柄生效（0/2/4/6）。先按自由拖拽算出
 * 两个方向各自想要的新尺寸，取较大的缩放倍率（主方向决定），再由 base 的宽高比
 * 反推另一方向，所以比例永远是按下那一刻的比例，而且松手再拖也不会逐帧漂。
 * 对角锚点与旋转补偿沿用同一套：等比只改两个尺寸，不改锚点语义。
 * 边手柄忽略 uniform——PS 里 Shift+边手柄是「以中心等比」，本壳没有中心锚点
 * 这条路径，静默按自由缩放处理比偷偷换个锚点更好解释。
 */
inline Rect transform_scale_rect(const Rect& base, double rotation, int handle, Point pointer,
                                 int min_size, bool uniform = false) {
  if (base.empty() || !std::isfinite(rotation)) return base;
  if (handle < 0 || handle >= kTransformHandleCount) return base;
  const int minimum = min_size < 1 ? 1 : min_size;
  const Point center = rect_center_point(base);
  const Point local = rotate_doc_point(pointer, center, -rotation);

  double left = static_cast<double>(base.x);
  double top = static_cast<double>(base.y);
  double right = static_cast<double>(base.right());
  double bottom = static_cast<double>(base.bottom());
  switch (handle) {
    case 0: left = local.x; top = local.y; break;
    case 1: top = local.y; break;
    case 2: right = local.x; top = local.y; break;
    case 3: right = local.x; break;
    case 4: right = local.x; bottom = local.y; break;
    case 5: bottom = local.y; break;
    case 6: left = local.x; bottom = local.y; break;
    default: left = local.x; break;
  }
  const double base_w = static_cast<double>(base.w);
  const double base_h = static_cast<double>(base.h);
  const bool corner = (handle % 2) == 0;
  if (uniform && corner && base_w > 0.0 && base_h > 0.0) {
    /*
     * 自由拖拽给出的 (right-left, bottom-top) 就是指针要求的两个新尺寸；
     * 取相对变化更大的那个方向定倍率，另一方向按 base 比例跟随。
     * 倍率下限同时满足两个方向的最小尺寸，否则「宽够 1px、高只有 0.2px」。
     */
    const double want_w = right - left;
    const double want_h = bottom - top;
    double scale = want_w / base_w;
    if (want_h / base_h > scale) scale = want_h / base_h;
    const double floor_w = static_cast<double>(minimum) / base_w;
    const double floor_h = static_cast<double>(minimum) / base_h;
    if (scale < floor_w) scale = floor_w;
    if (scale < floor_h) scale = floor_h;
    const double width = base_w * scale;
    const double height = base_h * scale;
    switch (handle) {
      case 0: /* 拖左上：右/下边是 base 的，钉住右下角 */
        left = right - width;
        top = bottom - height;
        break;
      case 2: /* 拖右上：钉住左下角 */
        right = left + width;
        top = bottom - height;
        break;
      case 4: /* 拖右下：钉住左上角 */
        right = left + width;
        bottom = top + height;
        break;
      default: /* 6：拖左下，钉住右上角 */
        left = right - width;
        bottom = top + height;
        break;
    }
  } else {
    if (right - left < static_cast<double>(minimum)) {
      const bool moves_left = handle == 0 || handle == 6 || handle == 7;
      if (moves_left) {
        left = right - static_cast<double>(minimum);
      } else {
        right = left + static_cast<double>(minimum);
      }
    }
    if (bottom - top < static_cast<double>(minimum)) {
      const bool moves_top = handle == 0 || handle == 1 || handle == 2;
      if (moves_top) {
        top = bottom - static_cast<double>(minimum);
      } else {
        bottom = top + static_cast<double>(minimum);
      }
    }
  }

  /* 对角锚点在 base 局部系里的位置：拖角是角、拖边是对边的中点。 */
  const double mid_x = (static_cast<double>(base.x) + static_cast<double>(base.right())) * 0.5;
  const double mid_y = (static_cast<double>(base.y) + static_cast<double>(base.bottom())) * 0.5;
  Point anchor{};
  switch (handle) {
    case 0: anchor = Point{right, bottom}; break;
    case 1: anchor = Point{mid_x, bottom}; break;
    case 2: anchor = Point{left, bottom}; break;
    case 3: anchor = Point{left, mid_y}; break;
    case 4: anchor = Point{left, top}; break;
    case 5: anchor = Point{mid_x, top}; break;
    case 6: anchor = Point{right, top}; break;
    default: anchor = Point{right, mid_y}; break;
  }
  const Point anchor_doc = rotate_doc_point(anchor, center, rotation);
  const Point new_center{(left + right) * 0.5, (top + bottom) * 0.5};
  const Point anchor_doc_new = rotate_doc_point(anchor, new_center, rotation);

  Rect result{};
  result.x = static_cast<int>(std::lround(left + (anchor_doc.x - anchor_doc_new.x)));
  result.y = static_cast<int>(std::lround(top + (anchor_doc.y - anchor_doc_new.y)));
  result.w = static_cast<int>(std::lround(right - left));
  result.h = static_cast<int>(std::lround(bottom - top));
  if (result.w < minimum) result.w = minimum;
  if (result.h < minimum) result.h = minimum;
  return result;
}

/* 拖旋钮的角度增量（度），归一化到 (-180, 180]，避免跨 ±180° 时跳一圈。 */
inline double transform_rotate_delta(const Rect& base, Point press, Point now) {
  const Point center = rect_center_point(base);
  const double from = std::atan2(press.y - center.y, press.x - center.x);
  const double to = std::atan2(now.y - center.y, now.x - center.x);
  constexpr double kPi = 3.14159265358979323846;
  double degrees = (to - from) * 180.0 / kPi;
  while (degrees <= -180.0) degrees += 360.0;
  while (degrees > 180.0) degrees -= 360.0;
  return degrees;
}

/* 与壳侧 Ctrl+左右 同一口径：多圈累加后归一到 (-360, 360]。 */
inline double transform_normalize_rotation(double degrees) {
  if (!std::isfinite(degrees)) return 0.0;
  while (degrees > 360.0) degrees -= 360.0;
  while (degrees <= -360.0) degrees += 360.0;
  return degrees;
}

/* ------------------------------------------------------- 采样模式（S22） */

/*
 * 采样模式档位表：数值即 comp_sampling 枚举（0 High quality / 1 Nearest /
 * 2 Smooth，ABI 冻结表），名称逐字取 ABI 注释里的上游拼写。
 *
 * 为什么值得在平台无关层钉一张表：引擎真的按它改变缩放手感
 * （resample.cpp 的 sampling_from_abi 把它译成内核的 SamplingMode），不是
 * 纯展示开关；而壳此前一次都没调过 setter / getter，用户看不到也改不了。
 */
inline constexpr int kSamplingHighQuality = 0;
inline constexpr int kSamplingNearest = 1;
inline constexpr int kSamplingSmooth = 2;
inline constexpr int kSamplingCount = 3;

inline int clamp_sampling_mode(int mode) {
  if (mode < 0) return kSamplingHighQuality;
  if (mode >= kSamplingCount) return kSamplingCount - 1;
  return mode;
}

inline const char* sampling_mode_name(int mode) {
  switch (clamp_sampling_mode(mode)) {
    case 0: return "High quality";
    case 1: return "Nearest";
    case 2: return "Smooth";
    default: return "High quality";
  }
}

/*
 * 图层面板行索引夹取（S22）：selected_layer 是**面板显示序**索引（0 = 最上层
 * 那一行），不是文档数组下标。重排、增删图层后都必须重新夹取，否则越界索引会让
 * selected_row 返回空指针，后续快捷键静默失效——「选了但按了没反应」最难排查。
 */
inline int32_t clamp_selected_row(int32_t row, int32_t count) {
  if (count <= 0) return 0;
  if (row < 0) return 0;
  if (row >= count) return count - 1;
  return row;
}

/*
 * 面板行 <-> 数组下标（S23）：两个方向的翻转只允许写在这一处。
 *
 * 理由不是抽象洁癖：S22 把 `count - 1 - row` 手写了四遍（重排锚点、选中态跟随、
 * 剪贴基底、直方图取层），只要有两处按相反方向理解，就会得到「层动了但选中态
 * 跳到邻居」这种自洽却不报错的缺陷——它不抛异常、自检也过。抽成纯函数后可以
 * 表驱动穷举，让方向错误在提交前就暴露。
 */
inline int32_t layer_array_index(int32_t count, int32_t row) {
  if (count <= 0) return -1;
  return count - 1 - clamp_selected_row(row, count);
}

inline int32_t layer_row_of_array_index(int32_t count, int32_t array_index) {
  if (count <= 0 || array_index < 0 || array_index >= count) return -1;
  return count - 1 - array_index;
}

/*
 * 图层重排计划（S23）：把「面板行 + 方向」翻译成 comp_layer_reorder 的锚点。
 *
 * 抽成纯函数的理由不是洁癖：S22 的这段换算写在 panel_reorder_layer 里，而
 * 那层没有任何测试，于是「上移 / 下移整对方向写反、且选中态按相反的假设跟随」
 * 这种自洽的错误一路进了主干——两个错误互相抵消，肉眼看菜单也不报错，只有
 * 真去拖图层的人才发现层跑反了。把换算搬到纯函数后可以表驱动穷举。
 *
 * 语义（engine/domain/document.cpp: `reorder_layer`）：先 erase 再把 id 插到
 * `before` **之前**；数组序即堆叠序，**尾 = 顶层**；`before == 0` = 挪到同级末尾
 * （顶层）。面板行序与之相反：行 0 = 顶层。
 *
 * 于是 row -> array_index = count-1-row，而「面板上移一行」= 堆叠序里往上 = 数组
 * 下标 **+1**，锚点必须是**新位置之上那一层**（插到它前面才落在下面一档）……
 * 注意插入是在锚点之前，所以要让 id 停在数组下标 k，锚点就必须是**现在**下标 k+1
 * 的那层（依次递推，见下）。
 */
struct ReorderPlan {
  bool ok = false;           /* false = 已经到头，界面上应如实提示而不是静默无操作。 */
  bool at_top = true;        /* ok=false 时的原因是「已是同级顶层」还是「已是同级底层」。 */
  int32_t before_index = -1; /* 锚点的数组下标；-1 表示 before = 0（同级末尾）。 */
};
/* S44：实现在 LayerStackEntry 之后——同一级兄弟要按 parent 找，不能只看数组相邻。 */

/*
 * 剪贴基底规划（S23）：输入是「面板所见的一层」（数组序、自下而上）。
 *
 * 为什么必须做成纯函数：S22 的 panel_toggle_clipping 把「下方一层」取成了
 * `self_index + 1`（数组上方那一层），守卫却拒绝的是顶层、提示还写「最底层无法
 * 剪贴」。后果不是「没效果」而是**两条渲染路径给出不同画面**：
 *   - FilterGraph 要求成员紧跟在基底之后（siblings[i+1+run]->clipping_source ==
 *     base），取反后剪贴栈根本不成立，画面完全不变；
 *   - compositor.cpp 的单层视图只要 clipping_source != 0 就按基底求值，画面变了。
 * 这种「某些视图下看起来是对的」最难查，所以把选择规则搬到这里穷举。
 */
struct LayerStackEntry {
  /*
   * id / parent / clipping 都是 comp_layer_id（ABI 里是 uint64_t）的数值形式。
   * 这里刻意用 int64_t 而不是 int32_t：窄化之后两个不同的 id 可能撞成同一个值，
   * 于是「基底是不是这一层」的判断会在极端情况下给出错误答案，而那种错误只在
   * 图层数很多时偶发——正是最难查的一类。ui_core 不包含引擎头，故不用精确类型。
   */
  int64_t id;       /* 图层 id */
  int64_t parent;   /* 父级 id；0 = 根级 */
  int64_t clipping; /* 该层当前剪贴到的基底 id；0 = 不是剪贴成员 */
};

/*
 * direction: -2 置顶, -1 上移一层, +1 下移一层, 0 置底。
 *
 * S44：面板变成树之后，「一层」必须理解为**同一父级里的相邻兄弟**，而不是
 * 数组里相邻的那个下标——组内层往上一格不该跳进组外的层，也不该被引擎的跨父
 * 校验拒绝。兄弟按数组序自下而上，越靠后越靠上：
 *   上移（-1）：目标 = 自己之上第一个兄弟；锚点 = 它再上一个兄弟，没有则末尾（before=0）。
 *   下移（+1）：目标 = 自己之下第一个兄弟；锚点 = 那个兄弟（erase 之后它就在目标位）。
 *   置顶（-2）：before = 0（同级末尾）。
 *   置底（0） ：锚点 = 最低的那个兄弟。
 */
inline ReorderPlan plan_layer_reorder(const LayerStackEntry* layers, int32_t count,
                                      int32_t index, int32_t direction) {
  ReorderPlan plan{};
  if (layers == nullptr || count <= 0 || index < 0 || index >= count) return plan;
  const int64_t parent = layers[index].parent;
  int32_t position = 0; /* 自己在兄弟里的序号（自下而上，0 开始） */
  int32_t siblings = 0;
  for (int32_t i = 0; i < count; ++i) {
    if (layers[i].parent != parent) continue;
    if (i < index) ++position;
    ++siblings;
  }

  /* 第 ordinal 个兄弟（自下而上，0 开始）的数组下标；越界返回 -1。 */
  const auto sibling_at = [&](int32_t ordinal) -> int32_t {
    int32_t seen = 0;
    for (int32_t i = 0; i < count; ++i) {
      if (layers[i].parent != parent) continue;
      if (seen == ordinal) return i;
      ++seen;
    }
    return -1;
  };

  if (direction == -2) { /* 置顶 */
    if (position == siblings - 1) {
      plan.at_top = true;
      return plan;
    }
    plan.ok = true;
    plan.before_index = -1;
    return plan;
  }
  if (direction == 0) { /* 置底 */
    if (position == 0) {
      plan.at_top = false;
      return plan;
    }
    plan.ok = true;
    plan.before_index = sibling_at(0);
    return plan;
  }
  if (direction < 0) { /* 上移一层 */
    if (position + 1 >= siblings) {
      plan.at_top = true;
      return plan;
    }
    plan.ok = true;
    plan.before_index = sibling_at(position + 2); /* -1 = 已经是同级顶层 */
    return plan;
  }
  if (direction > 0) { /* 下移一层 */
    if (position == 0) {
      plan.at_top = false;
      return plan;
    }
    plan.ok = true;
    plan.before_index = sibling_at(position - 1);
    return plan;
  }
  return plan;
}

/* --------------------------------------------------- 树形图层面板（S44） */

/*
 * 面板行的缩进与折叠三角。
 *
 * 缩进有上限：248px 的栏宽、12 字的层名，一直往右推会把名字挤没。超过 6 级的
 * 层仍然可折叠，只是不再继续右移——「看不见的缩进」比「看不到的名字」好。
 */
inline constexpr int kLayerIndentStep = 12;
inline constexpr int kLayerIndentLimit = 72;
/* 三角槽位对所有行都留出，同一级的名字才会对齐（只有组行真的画三角）。 */
inline constexpr int kLayerTreeMarker = 14;

inline int layer_row_indent(int32_t depth) {
  if (depth <= 0) return 0;
  const int indent = depth * kLayerIndentStep;
  return indent > kLayerIndentLimit ? kLayerIndentLimit : indent;
}

inline Rect layer_triangle_rect(const Rect& row, int32_t depth) {
  const int x = row.x + 6 + layer_row_indent(depth);
  const int y = row.y + (kLayerRowHeight - 10) / 2;
  return Rect{x, y, 10, 10};
}

namespace detail {

/*
 * 递归铺设一层显示：同一父级的子层按**数组逆序**（顶层先）排，组行先于它的
 * 子层。emit=false 表示这一支在折叠组里：照样往下走（把整支标记掉，免得后面
 * 被当成孤儿补出来），但不写进输出。
 */
inline void layer_display_walk(const LayerStackEntry* layers, int32_t count,
                               const uint8_t* collapsed, int64_t parent, int32_t level,
                               bool emit, uint8_t* seen, int32_t* display, int32_t* depth,
                               int32_t capacity, int32_t* written) {
  if (level > count) return; /* 数据有环时的护栏：宁可少画几层也不死循环 */
  for (int32_t i = count - 1; i >= 0; --i) {
    if (seen[i] != 0 || layers[i].parent != parent) continue;
    seen[i] = 1;
    if (emit && *written < capacity) {
      display[*written] = i;
      depth[*written] = level;
      ++(*written);
    }
    const bool folded = collapsed != nullptr && collapsed[i] != 0;
    layer_display_walk(layers, count, collapsed, layers[i].id, level + 1, emit && !folded,
                       seen, display, depth, capacity, written);
  }
}

}  // namespace detail

/* 父链能不能走到根（true = 会被根级那一轮铺进显示序）。环 / 断链一律 false。 */
inline bool layer_reaches_root(const LayerStackEntry* layers, int32_t count, int32_t index) {
  if (layers == nullptr || count <= 0 || index < 0 || index >= count) return false;
  int64_t current = layers[index].parent;
  for (int32_t guard = 0; guard <= count; ++guard) {
    if (current == 0) return true;
    int32_t next = -1;
    for (int32_t i = 0; i < count; ++i) {
      if (layers[i].id == current) {
        next = i;
        break;
      }
    }
    if (next < 0) return false; /* 父 id 不在表里：断链 */
    current = layers[next].parent;
  }
  return false; /* 环 */
}

/*
 * 显示序（S44）：面板是树，不再是「数组逆序」。
 *
 * 数组序自下而上，组的子层紧跟在组自身之后（引擎的落位口径）。直接取数组逆序
 * 会把子层画在组行**上面**，与 PS 相反；这里改成：
 *   - 根级层按数组逆序（顶层在第一行）；
 *   - 每个组的子层按数组逆序排在组行**下面**，再递归；
 *   - 折叠的组不展开子层（子层与更深层都不显示）。
 *
 * 输出 display[row] = 数组下标、depth[row] = 缩进层级，两者等长；缓冲不足按容量
 * 截断。父 id 不在表里的层（孤儿 / 环）不会被丢掉——从面板上消失比画错位置更糟，
 * 它们按数组逆序补到根级。
 *
 * 返回值 = 行数。
 */
inline int32_t build_layer_display(const LayerStackEntry* layers, int32_t count,
                                   const uint8_t* collapsed, int32_t* display, int32_t* depth,
                                   int32_t capacity) {
  if (layers == nullptr || count <= 0 || display == nullptr || depth == nullptr ||
      capacity <= 0) {
    return 0;
  }
  /* seen 是局部缓冲：ui_core 的约定是不分配内存，这里借调用方的输出缓冲又不合适
     （output 与 seen 长度无关），所以用静态上限的栈数组。图层数超过这个量级时
     面板本身也已经不能用了。 */
  static constexpr int32_t kMaxLayers = 4096;
  if (count > kMaxLayers) {
    /* 极端图层数：退回旧的「数组逆序」显示。树/折叠失效，但面板不会空。 */
    const int32_t rows = count < capacity ? count : capacity;
    for (int32_t i = 0; i < rows; ++i) {
      display[i] = count - 1 - i;
      depth[i] = 0;
    }
    return rows;
  }
  uint8_t seen[kMaxLayers];
  for (int32_t i = 0; i < count; ++i) seen[i] = 0;

  int32_t written = 0;
  detail::layer_display_walk(layers, count, collapsed, 0, 0, true, seen, display, depth,
                             capacity, &written);
  /* 断链 / 环：补到根级，避免「层在文档里、面板上看不见」。 */
  for (int32_t i = count - 1; i >= 0 && written < capacity; --i) {
    if (seen[i] != 0) continue;
    if (layer_reaches_root(layers, count, i)) continue;
    detail::layer_display_walk(layers, count, collapsed, layers[i].parent, 0, true, seen,
                               display, depth, capacity, &written);
  }
  return written;
}

/* 行 -> 数组下标；越界 -1。树形面板里这就是唯一合法的换算。 */
inline int32_t layer_index_at_row(const int32_t* display, int32_t rows, int32_t row) {
  if (display == nullptr || row < 0 || row >= rows) return -1;
  return display[row];
}

/* 数组下标 -> 行；不在显示序里（被折叠 / 越界）返回 -1。 */
inline int32_t layer_row_of_index(const int32_t* display, int32_t rows, int32_t index) {
  if (display == nullptr || index < 0) return -1;
  for (int32_t row = 0; row < rows; ++row) {
    if (display[row] == index) return row;
  }
  return -1;
}

enum class ClippingTarget {
  kBase,           /* 成功，base_index 可用 */
  kNoSiblingBelow, /* 下方没有同父图层：如实拒绝，不静默无操作 */
  kBrokenStack,    /* 下方邻居是剪贴成员，但它的基底解析不出（越级 / 反向引用） */
  kInvalid,        /* count / index 不合法 */
};

struct ClippingPlan {
  ClippingTarget target = ClippingTarget::kInvalid;
  int32_t base_index = -1;           /* 数组下标（base_row = count-1-base_index 仅扁平时成立） */
  bool joins_existing_stack = false; /* true = 并入下方邻居已有的剪贴栈 */
  bool ok() const { return target == ClippingTarget::kBase; }
};

/*
 * 整组移动（§6.61）：node 是否落在 root 的子树里（root 自己不算成员）。
 *
 * 组的 transform 不参与合成（filter_graph 只烘焙成员），所以「移动组」= 把子树里
 * 每个成员各自平移同样的量，而不是写组自己的 transform。成员关系必须沿 parent 链
 * 走——面板是扁平列表，视觉上的相邻与树上的父子没有任何关系，按数组相邻来搬会动到
 * 别的组里的层。
 *
 * 链最多走 count 步：数据有环时返回 false，宁可少移一层也不死循环。
 */
inline bool layer_in_subtree(const LayerStackEntry* layers, int32_t count, int64_t node,
                             int64_t root) {
  if (layers == nullptr || count <= 0 || root == 0 || node == root) return false;
  int64_t current = node;
  for (int32_t guard = 0; guard <= count; ++guard) {
    int64_t parent = -1;
    for (int32_t i = 0; i < count; ++i) {
      if (layers[i].id == current) {
        parent = layers[i].parent;
        break;
      }
    }
    if (parent < 0) return false; /* node 不在表里 */
    if (parent == root) return true;
    if (parent == 0) return false;
    current = parent;
  }
  return false; /* 环：当作不属于，绝不死循环 */
}

inline ClippingPlan plan_clipping_base_index(const LayerStackEntry* layers, int32_t count,
                                             int32_t index) {
  ClippingPlan plan{};
  if (layers == nullptr || count <= 0 || index < 0 || index >= count) return plan;
  plan.target = ClippingTarget::kInvalid;

  const int64_t parent = layers[index].parent;
  /*
   * 「下方一层」= 同父的紧邻前一个（数组序自下而上）。必须跳过夹在中间的别组层：
   * filter_graph 的 siblings 只含同父层，「全局数组相邻」不等于「同父相邻」——
   * S22 直接取 self_index-1，只要有图层组就会取到别的组里那一层。
   */
  int32_t below = index - 1;
  while (below >= 0 && layers[below].parent != parent) --below;
  if (below < 0) {
    plan.target = ClippingTarget::kNoSiblingBelow;
    return plan;
  }

  int32_t base = below;
  if (layers[below].clipping != 0) {
    /*
     * 下方邻居本身已是剪贴成员：基底必须是**它所在栈的基底**，不能是它本人。
     * 写成链式（C 剪到 B、B 又剪到 A）会让 C 落在栈外——FilterGraph 完全忽略
     * C 的 clipping_source，而单层视图仍然按它求值，两条路径画面不一致。
     */
    int32_t existing = -1;
    for (int32_t scan = below - 1; scan >= 0; --scan) {
      if (layers[scan].id == layers[below].clipping) {
        existing = scan;
        break;
      }
    }
    /* 只认「在下方且同父」的基底；越级 / 反向引用一律拒绝而不是退化成链。 */
    if (existing < 0 || existing >= index || layers[existing].parent != parent) {
      plan.target = ClippingTarget::kBrokenStack;
      return plan;
    }
    base = existing;
    plan.joins_existing_stack = true;
  }
  plan.target = ClippingTarget::kBase;
  plan.base_index = base;
  return plan;
}

/*
 * 行号版本（旧接口，扁平面板的调用点用它）。S44 的树形面板没有「行号 = 数组逆序」
 * 这条等式了，壳必须先把行号经 display[] 换成数组下标，再调 _index 版本；保留这个
 * 包装是为了既有用例与「剪贴只看同父相邻」的语义不被改写。
 */
inline ClippingPlan plan_clipping_base(const LayerStackEntry* layers, int32_t count,
                                       int32_t shown) {
  if (layers == nullptr || count <= 0) return ClippingPlan{};
  return plan_clipping_base_index(layers, count, layer_array_index(count, shown));
}

/* ------------------------------------------------- 直方图几何（S23） */

/*
 * 直方图块紧贴图层标题行**下方**：块顶 = panel.y + kLayerHeaderHeight，块高
 * kHistogramHeight，于是块底与图层行起点（panel.y + kLayerHeaderHeight +
 * rows_top）严格相接——既不压标题、也不留空档。
 *
 * S22 的块顶直接取 panel.y（块内标签落在 panel.y+10），和标题的 panel.y+8 叠字；
 * 行起点却在 +94，中间空出 30px。根因是「块的坐标」「标签的坐标」「行的偏移」
 * 三处各算一套。这里统一成唯一来源，绘制与命中测试都从这里取。
 */
inline Rect histogram_rect(const Rect& panel) {
  return Rect{panel.x, panel.y + kLayerHeaderHeight, panel.w, Layout::kHistogramHeight};
}

inline Rect histogram_label_rect(const Rect& panel) {
  const Rect block = histogram_rect(panel);
  return Rect{block.x + 8, block.y + 2, block.w - 16, 14};
}

inline Rect histogram_plot_rect(const Rect& panel) {
  const Rect block = histogram_rect(panel);
  return Rect{block.x + 6, block.y + Layout::kHistogramHeight - Layout::kHistogramPlotHeight,
              block.w - 12, Layout::kHistogramPlotHeight};
}

/* 图层行相对标题行的额外下移量：直方图打开时整体让出块高。 */
inline int layer_rows_top(bool histogram_open) {
  return histogram_open ? Layout::kHistogramHeight : 0;
}

/* ============================================ S24：菜单态 / 拖行 / 直方图区间 */

/* -------------------------------------------------------- 菜单勾选态（S24） */

/*
 * 为什么把「该勾哪几项」做成纯函数：菜单只在启动时建一次（build_menu 交给
 * CreateWindowW），勾选态只能在 WM_INITMENU 里刷。Win32 的 CheckMenuItem 本身
 * 没有可测的地方，可测的是**判定规则**——无选中层时不能勾、非组的 isolated 置 1
 * 也不算、采样/混合必须落在合法档位。规则搬到这里就能表驱动穷举，否则「勾错了」
 * 只能靠肉眼开菜单看（自检与单元测试都覆盖不到）。
 */
struct LayerMenuState {
  bool has_layer = false; /* 有选中图层 */
  bool visible = false;
  bool clipping = false;
  bool is_group = false;
  bool isolated = false;
  int32_t sampling = 0;
  int32_t blend = 0;
  bool histogram_open = false;

  /* ---- S41：置灰用的前置条件（每项都由调用方从引擎/壳读实况填入） ---- */
  bool has_mask = false;          /* 选中层带蒙版 */
  bool has_selection = false;     /* 有活动选区（含 coverage） */
  bool can_undo = false;
  bool can_redo = false;
  bool can_merge_down = false;    /* comp_layer_can_merge_down 判定 */
  bool can_move_up = false;       /* 重排纯函数：上移一层可行 */
  bool can_move_down = false;     /* 重排纯函数：下移一层可行 */
  bool clipping_available = false; /* 剪贴计划成立，或已处于剪贴态（可取消） */
  bool multi_selected = false;    /* 面板多选集合 > 1 */
  int32_t effect_count = 0;       /* 选中层已有效果数 */
  int32_t layer_count = 0;
  /*
   * P0-6 ①：选中层是否可编辑文字图层（由调用方用 comp_layer_get_text 查询：
   * 返回 COMP_OK 即文字层，COMP_ERR_STATE 即普通像素层）。栅格化只对文字层有动作。
   */
  bool is_text = false;
};

struct LayerMenuChecks {
  bool visible = false;
  bool clipping = false;
  bool isolated = false;
  bool histogram = false;
  int32_t sampling = -1; /* -1 = 不勾任何一项（无选中层时的显式「无」） */
  int32_t blend = -1;
};

inline LayerMenuChecks layer_menu_checks(const LayerMenuState& state) {
  LayerMenuChecks checks{};
  /* 直方图开关是文档级视图状态，与有没有选中层无关。 */
  checks.histogram = state.histogram_open;
  if (!state.has_layer) return checks;
  checks.visible = state.visible;
  checks.clipping = state.clipping;
  /* isolated 只对图层组有意义：非组层置 1 也不参与合成，勾上即误导。 */
  checks.isolated = state.is_group && state.isolated;
  checks.sampling = clamp_sampling_mode(state.sampling);
  checks.blend = clamp_blend_mode(state.blend);
  return checks;
}

/*
 * 菜单可用态（S41）。与勾选态同一套理由：Win32 的 EnableMenuItem 不可测，
 * 可测的是「哪一项在什么前提下有动作」。判定集中在这里，外壳只按命令 id 置灰。
 *
 * 口径：**只有调用会如实拒绝（或纯属无操作）的项才置灰**。凡是有副作用、
 * 只是结果可能为空（如按像素载入选区选中 0 个像素）的，一律保持可用——
 * 置灰等于替用户判断「你不想要」，比给一句状态栏提示更武断。
 */
struct LayerMenuEnabled {
  bool undo = false;
  bool redo = false;
  bool layer_present = false; /* 需要选中图层的整组项（复制/删除/改名/属性/变换…） */
  bool layer_merge_down = false;
  bool layer_flatten = false;
  bool layer_step = false;    /* 选择上一/下一图层：至少两层才有换的意义 */
  bool move_up = false;       /* 上移一层 / 置于顶层 */
  bool move_down = false;     /* 下移一层 / 置于底层 */
  bool effect_toggle = false; /* 效果 kEffectKindCount（10）档：组只能通过多选批量跳过它 */
  bool effect_edit = false;
  bool effect_clear = false;
  bool mask_from_selection = false;
  bool mask_to_selection = false;
  bool mask_remove = false;
  bool clipping = false;
  bool isolated = false;
  bool selection_modify = false; /* 取消选择/反选/扩展/收缩：都要先有选区 */
  bool selection_load_pixels = false;
  bool selection_save_channel = false; /* 存储选区到通道：没有选区时引擎会拒 */
  bool layer_rasterize_text = false; /* 栅格化文字：只对可编辑文字图层有动作 */
};

inline LayerMenuEnabled layer_menu_enabled(const LayerMenuState& state) {
  LayerMenuEnabled enabled{};
  enabled.undo = state.can_undo;
  enabled.redo = state.can_redo;
  enabled.layer_present = state.has_layer;
  enabled.layer_merge_down = state.has_layer && state.can_merge_down;
  /* 只有一层时拼合/换层都是空操作（还会白记一步历史）。 */
  enabled.layer_flatten = state.layer_count >= 2;
  enabled.layer_step = state.layer_count >= 2;
  enabled.move_up = state.has_layer && state.can_move_up;
  enabled.move_down = state.has_layer && state.can_move_down;
  /* 组的 effects 不参与求值（filter_graph 的 kGroup 分支只合成子树）；多选时
     主选中层是组也仍有意义——批量会跳过组、作用于其余成员。 */
  enabled.effect_toggle = state.has_layer && (!state.is_group || state.multi_selected);
  enabled.effect_edit = enabled.effect_toggle;
  enabled.effect_clear = state.has_layer && state.effect_count > 0;
  enabled.mask_from_selection = state.has_layer && state.has_selection;
  enabled.mask_to_selection = state.has_layer && state.has_mask;
  enabled.mask_remove = state.has_layer && state.has_mask;
  enabled.clipping = state.has_layer && state.clipping_available;
  enabled.isolated = state.has_layer && state.is_group;
  enabled.selection_modify = state.has_selection;
  enabled.selection_load_pixels = state.has_layer;
  /* 存储通道要一份**当前选区**：没有选区时 comp_selection_channel_save 只会
     回 COMP_ERR_STATE，提前置灰比让用户白按一次诚实。 */
  enabled.selection_save_channel = state.has_selection;
  /* 普通图层上「栅格化文字」是纯空操作（引擎幂等返回 OK、不记撤销步），置灰。 */
  enabled.layer_rasterize_text = state.has_layer && state.is_text;
  return enabled;
}

/* ------------------------------------------------------------ 效果档位（S24） */

/*
 * 数值即 .comp 的 engineEffects[].effect，与 compositing/effect_eval.h 的
 * kEffectStroke..kEffectPatternOverlay（0..9）逐字对应；顺序也是引擎的落笔判定顺序。
 * ui_core 不包含引擎头（纯几何/规则库，可脱壳编译），所以这里只镜像数值与名称。
 */
inline constexpr int kEffectKindCount = 10;

inline int clamp_effect_kind(int kind) {
  if (kind < 0) return 0;
  if (kind >= kEffectKindCount) return kEffectKindCount - 1;
  return kind;
}

inline const char* effect_kind_name(int kind) {
  switch (clamp_effect_kind(kind)) {
    case 0: return "Stroke";
    case 1: return "Drop Shadow";
    case 2: return "Color Overlay";
    case 3: return "Inner Shadow";
    case 4: return "Outer Glow";
    case 5: return "Inner Glow";
    case 6: return "Bevel & Emboss";
    case 7: return "Satin";
    case 8: return "Gradient Overlay";
    case 9: return "Pattern Overlay";
    default: return "Stroke";
  }
}

/* 该层是否已经带了某个效果（菜单勾选态 / 面板标记共用同一个判定）。 */
inline bool effect_list_contains(const uint32_t* kinds, int32_t count, int kind) {
  if (kinds == nullptr || count <= 0) return false;
  const uint32_t target = static_cast<uint32_t>(clamp_effect_kind(kind));
  for (int32_t i = 0; i < count; ++i) {
    if (kinds[i] == target) return true;
  }
  return false;
}

/* ---------------------------------------------------- 效果参数面板（P0-2） */

/*
 * 行几何与滤镜面板同源：标题 + 效果名两行（54 px）之后是参数行，每行 20 px。
 * 绘制与命中都用下面两个函数，避免「看得见点不中」。
 */
inline constexpr int kEffectRowHeight = 20;
inline constexpr int kEffectRowsTop = 54;

inline Rect effect_param_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kEffectRowsTop + row * kEffectRowHeight, panel.w,
              kEffectRowHeight};
}

inline int effect_param_row_at(const Rect& panel, int param_count, double x, double y) {
  if (param_count <= 0 || !panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kEffectRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kEffectRowHeight)));
  if (row < 0 || row >= param_count) return -1;
  return row;
}

/* 夹取口径与 clamp_filter_param 相同：NaN / 无穷夹到区间，绝不产生越界值。 */
inline double clamp_effect_param(double value, double min_value, double max_value) {
  if (!(value == value)) return min_value; /* NaN */
  if (value < min_value) return min_value;
  if (value > max_value) return max_value;
  return value;
}

/*
 * 步长按量程决定，而不是给每个参数再抄一份 `step` 字段：
 * 0..1 的归一化量（不透明度、RGB）走 0.05，其余（平方像素、角度）走 1。
 * 阈值取「跨度 <= 1」——当前表里没有跨度刚好为 1 的非归一量，若将来出现，
 * 补 `step` 字段比在这里加特例更诚实。
 */
inline double effect_param_step(double min_value, double max_value) {
  return (max_value - min_value) <= 1.0 ? 0.05 : 1.0;
}

inline double step_effect_param(double current, int direction, double min_value,
                                double max_value) {
  const double base = clamp_effect_param(current, min_value, max_value);
  if (direction == 0) return base;
  return clamp_effect_param(base + effect_param_step(min_value, max_value) *
                                         static_cast<double>(direction),
                            min_value, max_value);
}

/* ---------------------------------------------- 色彩范围面板（P0-4 第二批） */

/*
 * 排版：标题（8+22+20）之后是取样色块两行（取色 / 排除），再往下是容差与反相
 * 两行参数。色块行与参数行都只画**首屏**能放下的数量——面板固定宽 330，
 * 超出部分不影响命中（点不到就删不掉，但 Alt+点画布可以少加一个颜色，
 * 所以不存在「加错了没法撤销」的死角）。
 */
inline constexpr int kColorRangeSwatchSize = 18;
inline constexpr int kColorRangeSwatchGap = 4;
inline constexpr int kColorRangeSwatchLeft = 12;
inline constexpr int kColorRangeIncludeTop = 50;
inline constexpr int kColorRangeExcludeTop = 74;
inline constexpr int kColorRangeRowHeight = 24;
inline constexpr int kColorRangeRowsTop = 100;

inline int color_range_swatch_capacity(const Rect& panel) {
  const int usable = panel.w - 2 * kColorRangeSwatchLeft;
  if (usable <= 0) return 0;
  return usable / (kColorRangeSwatchSize + kColorRangeSwatchGap);
}

inline Rect color_range_swatch_rect(const Rect& panel, bool exclude, int index) {
  return Rect{panel.x + kColorRangeSwatchLeft +
                  index * (kColorRangeSwatchSize + kColorRangeSwatchGap),
              panel.y + (exclude ? kColorRangeExcludeTop : kColorRangeIncludeTop),
              kColorRangeSwatchSize, kColorRangeSwatchSize};
}

/*
 * 命中取样色块：返回 0..include_count-1 = 取色行第 i 个色块，
 * include_count.. = 排除行第 (i - include_count) 个；未命中返回 -1。
 * 只认首屏画得出来的那些，避免点到空白处删掉看不见的颜色。
 */
inline int color_range_swatch_at(const Rect& panel, int include_count, int exclude_count,
                                 double x, double y) {
  const int capacity = color_range_swatch_capacity(panel);
  if (capacity <= 0) return -1;
  const int shown_include = include_count < capacity ? include_count : capacity;
  const int shown_exclude = exclude_count < capacity ? exclude_count : capacity;
  for (int i = 0; i < shown_include; ++i) {
    if (color_range_swatch_rect(panel, false, i).contains(x, y)) return i;
  }
  for (int i = 0; i < shown_exclude; ++i) {
    if (color_range_swatch_rect(panel, true, i).contains(x, y)) return shown_include + i;
  }
  return -1;
}

inline Rect color_range_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kColorRangeRowsTop + row * kColorRangeRowHeight, panel.w,
              kColorRangeRowHeight};
}

inline int color_range_row_at(const Rect& panel, double x, double y) {
  if (!panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kColorRangeRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kColorRangeRowHeight)));
  if (row < 0 || row > 1) return -1;
  return row;
}

/* -------------------------------------------- 路径面板（P0-4 可选，S26） */

/*
 * 引擎没有持久化路径实体：路径在本壳里就是「已闭合的钢笔锚点」，唯一用途是
 * 重新落成选区。所以面板只列名字与锚点数，排版也按列表来做——标题（8+22）
 * 之后一行摘要，列表从 y+64 起，底部留 3 行提示。
 * 可见行数随面板高度变化；命中只认画得出来的行，与列表滚动的口径一致。
 */
inline constexpr int kPathRowHeight = 22;
inline constexpr int kPathRowsTop = 64;
inline constexpr int kPathPanelFooter = 96;
/* 上限按「够用且不无限涨内存」取整：超出后丢最旧一条（FIFO），状态栏会说。 */
inline constexpr int kPathMaxEntries = 64;

/*
 * 通用「右栏列表」几何：标题/摘要在 top 之内，底部 footer 留给提示，中间按
 * row_height 切行。路径面板与历史面板共用这一组，差别只在常量。
 * 命中只认画得出来的行（visible_rows 的同一口径），避免滚到底时点空白命中行尾。
 */
inline int list_visible_rows(const Rect& panel, int top, int footer, int row_height) {
  const int space = panel.h - top - footer;
  if (space <= 0 || row_height <= 0) return 0;
  return space / row_height;
}

inline int list_max_scroll(const Rect& panel, int count, int top, int footer, int row_height) {
  const int visible = list_visible_rows(panel, top, footer, row_height);
  if (visible <= 0 || count <= visible) return 0;
  return count - visible;
}

inline int list_clamp_scroll(const Rect& panel, int count, int scroll, int top, int footer,
                             int row_height) {
  if (scroll < 0) scroll = 0;
  const int max_scroll = list_max_scroll(panel, count, top, footer, row_height);
  return scroll > max_scroll ? max_scroll : scroll;
}

/* 让 index 落进可视区（↑/↓ 选行、新项入列时用）：只滚到刚好够，不居中。 */
inline int list_ensure_visible(const Rect& panel, int count, int index, int scroll, int top,
                               int footer, int row_height) {
  const int clamped = list_clamp_scroll(panel, count, scroll, top, footer, row_height);
  if (index < 0 || index >= count) return clamped;
  const int visible = list_visible_rows(panel, top, footer, row_height);
  if (visible <= 0) return clamped;
  if (index < clamped) return index;
  if (index >= clamped + visible) return index - visible + 1;
  return clamped;
}

inline Rect list_row_rect(const Rect& panel, int row, int top, int row_height) {
  return Rect{panel.x, panel.y + top + row * row_height, panel.w, row_height};
}

/* 返回面板内列表下标（= scroll + 可视行号）；空白/超出行尾返回 -1。 */
inline int list_row_at(const Rect& panel, int count, int scroll, double x, double y, int top,
                       int footer, int row_height) {
  if (!panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + top);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(row_height)));
  if (row < 0 || row >= list_visible_rows(panel, top, footer, row_height)) return -1;
  const int index = scroll + row;
  return index >= 0 && index < count ? index : -1;
}

inline int path_visible_rows(const Rect& panel) {
  return list_visible_rows(panel, kPathRowsTop, kPathPanelFooter, kPathRowHeight);
}

inline int path_max_scroll(const Rect& panel, int count) {
  return list_max_scroll(panel, count, kPathRowsTop, kPathPanelFooter, kPathRowHeight);
}

inline int path_clamp_scroll(const Rect& panel, int count, int scroll) {
  return list_clamp_scroll(panel, count, scroll, kPathRowsTop, kPathPanelFooter,
                           kPathRowHeight);
}

inline int path_ensure_visible(const Rect& panel, int count, int index, int scroll) {
  return list_ensure_visible(panel, count, index, scroll, kPathRowsTop, kPathPanelFooter,
                             kPathRowHeight);
}

inline Rect path_row_rect(const Rect& panel, int row) {
  return list_row_rect(panel, row, kPathRowsTop, kPathRowHeight);
}

inline int path_row_at(const Rect& panel, int count, int scroll, double x, double y) {
  return list_row_at(panel, count, scroll, x, y, kPathRowsTop, kPathPanelFooter,
                     kPathRowHeight);
}

/* ---------------------------------------- 历史面板（P1，2026-10-21） */

/*
 * 与路径面板同一套列表排版：index 0 = 最初状态，行 i = 第 i 步。行数 = 步数 + 1。
 * 底部留 3 行提示（回车/双击回跳、Ctrl+Z/Y、Esc 关闭），所以 footer 与路径同值。
 */
inline constexpr int kHistoryRowHeight = 22;
inline constexpr int kHistoryRowsTop = 64;
inline constexpr int kHistoryPanelFooter = 96;

inline int history_visible_rows(const Rect& panel) {
  return list_visible_rows(panel, kHistoryRowsTop, kHistoryPanelFooter, kHistoryRowHeight);
}

inline int history_clamp_scroll(const Rect& panel, int count, int scroll) {
  return list_clamp_scroll(panel, count, scroll, kHistoryRowsTop, kHistoryPanelFooter,
                           kHistoryRowHeight);
}

inline int history_ensure_visible(const Rect& panel, int count, int index, int scroll) {
  return list_ensure_visible(panel, count, index, scroll, kHistoryRowsTop,
                             kHistoryPanelFooter, kHistoryRowHeight);
}

inline Rect history_row_rect(const Rect& panel, int row) {
  return list_row_rect(panel, row, kHistoryRowsTop, kHistoryRowHeight);
}

inline int history_row_at(const Rect& panel, int count, int scroll, double x, double y) {
  return list_row_at(panel, count, scroll, x, y, kHistoryRowsTop, kHistoryPanelFooter,
                     kHistoryRowHeight);
}

/* ---------------------------------------- 通道面板（ROI #5，2026-10-23） */

/*
 * 与路径/历史同一套列表排版：标题（8+22）之后一行摘要，列表从 y+64 起，
 * 底部留 3 行提示。上限与引擎的 kMaxSelectionChannels 对齐（16）：引擎才是
 * 真值，这里只用来画「n / 16」和提前把「满」说清楚。
 */
inline constexpr int kChannelRowHeight = 22;
inline constexpr int kChannelRowsTop = 64;
inline constexpr int kChannelPanelFooter = 96;
inline constexpr int kChannelMaxEntries = 16;

inline int channel_visible_rows(const Rect& panel) {
  return list_visible_rows(panel, kChannelRowsTop, kChannelPanelFooter, kChannelRowHeight);
}

inline int channel_clamp_scroll(const Rect& panel, int count, int scroll) {
  return list_clamp_scroll(panel, count, scroll, kChannelRowsTop, kChannelPanelFooter,
                           kChannelRowHeight);
}

inline int channel_ensure_visible(const Rect& panel, int count, int index, int scroll) {
  return list_ensure_visible(panel, count, index, scroll, kChannelRowsTop,
                             kChannelPanelFooter, kChannelRowHeight);
}

inline Rect channel_row_rect(const Rect& panel, int row) {
  return list_row_rect(panel, row, kChannelRowsTop, kChannelRowHeight);
}

inline int channel_row_at(const Rect& panel, int count, int scroll, double x, double y) {
  return list_row_at(panel, count, scroll, x, y, kChannelRowsTop, kChannelPanelFooter,
                     kChannelRowHeight);
}

/* ----------------------------------- 智能滤镜栈面板（P1-2 收尾 ⑨，2026-10-25） */

/*
 * 与路径 / 历史 / 通道同一套右栏列表排版：标题（8+22）之后一行摘要，列表从
 * y+64 起，底部留 5 行提示（选择+调序+改名 / 开关+编辑 / 拖动+删除 / 撤销+只改名 /
 * 关闭+同步）。一行 = 引擎里的一条智能滤镜；壳里不缓存 desc，画一行就问一次
 * comp_layer_smart_filter_at，撤销 / 重做之后列表不会与文档漂移。
 */
inline constexpr int kSmartRowHeight = 22;
inline constexpr int kSmartRowsTop = 64;
inline constexpr int kSmartPanelFooter = 98; /* 5 行提示（90）+ 8 px 留白 */

inline int smart_visible_rows(const Rect& panel) {
  return list_visible_rows(panel, kSmartRowsTop, kSmartPanelFooter, kSmartRowHeight);
}

inline int smart_max_scroll(const Rect& panel, int count) {
  return list_max_scroll(panel, count, kSmartRowsTop, kSmartPanelFooter, kSmartRowHeight);
}

inline int smart_clamp_scroll(const Rect& panel, int count, int scroll) {
  return list_clamp_scroll(panel, count, scroll, kSmartRowsTop, kSmartPanelFooter,
                           kSmartRowHeight);
}

inline int smart_ensure_visible(const Rect& panel, int count, int index, int scroll) {
  return list_ensure_visible(panel, count, index, scroll, kSmartRowsTop, kSmartPanelFooter,
                             kSmartRowHeight);
}

inline Rect smart_row_rect(const Rect& panel, int row) {
  return list_row_rect(panel, row, kSmartRowsTop, kSmartRowHeight);
}

inline int smart_row_at(const Rect& panel, int count, int scroll, double x, double y) {
  return list_row_at(panel, count, scroll, x, y, kSmartRowsTop, kSmartPanelFooter,
                     kSmartRowHeight);
}

/* ---------------------------------------------- 字体面板（P0-6②，S45） */

/*
 * 字体列表比路径/历史多一行：标题（8+22）之后是「扫描进度 / 命中数」（y+30）
 * 与过滤词（y+54），列表从 y+80 起。底部留 4 行快捷键提示——字体行的操作比
 * 「回车/双击」多（过滤、清空过滤、应用、关闭），少写一行用户就记不全。
 * 行几何全部走下面这组函数：画在哪一行，点中就应用哪一行。
 */
inline constexpr int kFontRowHeight = 22;
inline constexpr int kFontRowsTop = 80;
inline constexpr int kFontPanelFooter = 88;

inline int font_visible_rows(const Rect& panel) {
  return list_visible_rows(panel, kFontRowsTop, kFontPanelFooter, kFontRowHeight);
}

inline int font_clamp_scroll(const Rect& panel, int count, int scroll) {
  return list_clamp_scroll(panel, count, scroll, kFontRowsTop, kFontPanelFooter, kFontRowHeight);
}

inline int font_ensure_visible(const Rect& panel, int count, int index, int scroll) {
  return list_ensure_visible(panel, count, index, scroll, kFontRowsTop, kFontPanelFooter,
                             kFontRowHeight);
}

inline Rect font_row_rect(const Rect& panel, int row) {
  return list_row_rect(panel, row, kFontRowsTop, kFontRowHeight);
}

inline int font_row_at(const Rect& panel, int count, int scroll, double x, double y) {
  return list_row_at(panel, count, scroll, x, y, kFontRowsTop, kFontPanelFooter, kFontRowHeight);
}

/*
 * 过滤：ASCII 大小写不敏感的子串匹配，非 ASCII 码元按原样比较。
 * 字体族名里既有 "Noto Sans SC" 也有「思源黑体」这类本地化名，统一按 UTF-16
 * 码元逐个比——中文名不该因为「没有大小写概念」而被排除在匹配之外。
 */
inline bool font_label_matches(const std::wstring& label, const std::wstring& filter) {
  if (filter.empty()) return true;
  if (label.size() < filter.size()) return false;
  for (size_t start = 0; start + filter.size() <= label.size(); ++start) {
    size_t i = 0;
    for (; i < filter.size(); ++i) {
      wchar_t left = label[start + i];
      wchar_t right = filter[i];
      if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left - L'A' + L'a');
      if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right - L'A' + L'a');
      if (left != right) break;
    }
    if (i == filter.size()) return true;
  }
  return false;
}

/* ------------------------------------------------ 调整参数面板（P0-3） */

/*
 * 参数行 = 上 18 px「标签 + 数值」（点击聚焦 / 数字键键入）+ 下 16 px 滑杆。
 * 单行 20 px 放不下一个能用鼠标拖的滑道，这是 GDI 里还能拖得动的下限。
 * 行首偏移 = 标题（8 + 22）+ 类型行（20），与 draw_adjust_panel 的排版一致；
 * 绘制与命中都走下面这组函数，避免「看得见点不中」。
 */
inline constexpr int kAdjustParamRowHeight = 34;
inline constexpr int kAdjustParamRowsTop = 50;
inline constexpr int kAdjustRowLabelBand = 18;
/* 底部留给「已有调整」列表与快捷键提示的高度（列表头 + 3 行 + 5 行提示）。 */
inline constexpr int kAdjustPanelFooter = 180;

inline int adjust_param_visible_rows(const Rect& panel) {
  const int space = panel.h - kAdjustParamRowsTop - kAdjustPanelFooter;
  if (space <= 0) return 0;
  return space / kAdjustParamRowHeight;
}

inline int adjust_param_max_scroll(const Rect& panel, int param_count) {
  const int visible = adjust_param_visible_rows(panel);
  if (visible <= 0 || param_count <= visible) return 0;
  return param_count - visible;
}

inline int adjust_param_clamp_scroll(const Rect& panel, int param_count, int scroll) {
  if (scroll < 0) scroll = 0;
  const int max_scroll = adjust_param_max_scroll(panel, param_count);
  return scroll > max_scroll ? max_scroll : scroll;
}

/* 让 row 落进可视区（Tab 切参数时用）：只滚到刚好够，不做居中。 */
inline int adjust_param_ensure_visible(const Rect& panel, int param_count, int row,
                                       int scroll) {
  const int visible = adjust_param_visible_rows(panel);
  int next = scroll;
  if (visible > 0 && row >= 0) {
    if (row < next) next = row;
    if (row >= next + visible) next = row - visible + 1;
  }
  return adjust_param_clamp_scroll(panel, param_count, next);
}

/* row 是参数表里的绝对下标；scroll 是已经滚掉的行数。 */
inline Rect adjust_param_row_rect(const Rect& panel, int row, int scroll = 0) {
  return Rect{panel.x, panel.y + kAdjustParamRowsTop + (row - scroll) * kAdjustParamRowHeight,
              panel.w, kAdjustParamRowHeight};
}

/* 命中：返回绝对行号；落在滚动窗口外 / 行数之外返回 -1。 */
inline int adjust_param_row_at(const Rect& panel, int param_count, int scroll, double x,
                               double y) {
  if (param_count <= 0 || !panel.contains(x, y)) return -1;
  const int visible = adjust_param_visible_rows(panel);
  if (visible <= 0) return -1;
  const double local = y - static_cast<double>(panel.y + kAdjustParamRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kAdjustParamRowHeight)));
  if (row < 0 || row >= visible) return -1;
  const int absolute = row + scroll;
  return absolute < param_count ? absolute : -1;
}

inline Rect adjust_slider_track_rect(const Rect& row) {
  return Rect{row.x + 10, row.y + kAdjustRowLabelBand, row.w - 20,
              row.h - kAdjustRowLabelBand - 2};
}

/* 滑杆只占行的下半段：上半段仍是「聚焦 / 键入」区域，拖动不会误锁焦点。 */
inline bool adjust_row_track_hit(const Rect& panel, int row, int scroll, double x, double y) {
  const Rect rect = adjust_param_row_rect(panel, row, scroll);
  return adjust_slider_track_rect(rect).contains(x, y);
}

/* x -> 0..1，含 NaN / 越界夹取（拖出滑道即取端值，PS 同款）。 */
inline double adjust_slider_fraction(const Rect& track, double x) {
  if (track.w <= 0) return 0.0;
  const double t = (x - static_cast<double>(track.x)) / static_cast<double>(track.w);
  if (!(t == t)) return 0.0; /* NaN */
  return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

inline double adjust_value_fraction(double value, double min_value, double max_value) {
  if (!(max_value > min_value) || !(value == value)) return 0.0;
  const double t = (value - min_value) / (max_value - min_value);
  return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

inline double adjust_slider_value(const Rect& track, double x, double min_value,
                                  double max_value) {
  const double t = adjust_slider_fraction(track, x);
  return clamp_effect_param(min_value + (max_value - min_value) * t, min_value, max_value);
}

/*
 * 落文本前的归整：量程 >= 50 的表项（色阶 0..255、明暗/饱和 -100..100）都是
 * 「整数量」，而量程小的（±20 的曝光、0.1..9.99 的 gamma）必须留小数。
 * 阈值按当前参数表定，与 effect_param_step 的「跨度 <= 1」同一思路：
 * 表里出现新的量程类别时，补字段比在这里加特例更诚实。
 */
inline double adjust_snap_value(double value, double min_value, double max_value) {
  const double clamped = clamp_effect_param(value, min_value, max_value);
  const double scale = (max_value - min_value) >= 50.0 ? 1.0 : 100.0;
  return std::round(clamped * scale) / scale;
}

/* 数值在滑道上的像素位置（画滑块用；与 x -> 值严格互逆）。 */
inline double adjust_slider_knob_x(const Rect& track, double value, double min_value,
                                   double max_value) {
  const double t = adjust_value_fraction(value, min_value, max_value);
  return static_cast<double>(track.x) + t * static_cast<double>(track.w);
}

/* ------------------------------------------------------- 拖行重排（S24） */

enum class DragTarget {
  kOk = 0,
  kSameRow,     /* 落点就是原行：不产生写入（否则撤销栈里多一条空记录） */
  kCrossParent, /* 落点父级不同：如实拒绝，不静默改父子关系 */
  kInvalid,
};

struct DragPlan {
  DragTarget target = DragTarget::kInvalid;
  int32_t before_index = -1; /* 锚点在**当前数组**里的下标；-1 = 同级末尾（before = 0） */
  int32_t next_row = 0;      /* 放下后该图层应占的面板行（选中态跟随） */
  bool ok() const { return target == DragTarget::kOk; }
};

/*
 * 把面板第 from_row 行拖到第 to_row 行（0 = 最上层）。
 *
 * S44：行号先经 display[] 换算成数组下标（面板是树，行号不再等于 count-1-下标）。
 * 只允许同父拖拽：跨父会改 parent，跨父的两种合法路径（拖进组 / 拖出到根级）由
 * 壳按落点几何另行处理；其余跨父落点明确拒绝，「拖完层没了」比「拒绝」糟得多。
 *
 * 引擎语义是「erase 后插到锚点之前」。设 ps / ts 为拖拽层与目标层在**同一父级的
 * 兄弟序列**里的序号（自下而上）：
 *   ts == 兄弟数 - 1（落到同级最顶） -> 锚点 = 同级末尾（before = 0）；
 *   ts <  ps（往上拖）              -> 锚点 = 原序号 ts+1 的兄弟（erase 后它顶到 ts）；
 *   ts >  ps（往下拖）              -> 锚点 = 原序号 ts 的兄弟。
 * 扁平文档（全是根级层）时 ps/ts 与数组下标一一对应，行为与旧实现逐位相同。
 * 树形文档里兄弟之间可能夹着别的组，序号换算绕开它们——这正是旧实现会错的地方。
 *
 * next_row 只在「显示序 = 数组逆序」的扁平文档里精确；树形文档拖完行号会因展开
 * 的子树而变，调用方应按 id 重新选中（见 main.cpp panel_drop_layer）。
 */
inline DragPlan plan_layer_drag(const LayerStackEntry* layers, int32_t count,
                                const int32_t* display, int32_t rows, int32_t from_row,
                                int32_t to_row) {
  DragPlan plan{};
  if (layers == nullptr || display == nullptr || count <= 0 || rows <= 0) return plan;
  if (from_row < 0 || from_row >= rows || to_row < 0 || to_row >= rows) return plan;
  if (from_row == to_row) {
    plan.target = DragTarget::kSameRow;
    plan.next_row = from_row;
    return plan;
  }
  const int32_t from_index = display[from_row];
  const int32_t to_index = display[to_row];
  if (from_index < 0 || from_index >= count || to_index < 0 || to_index >= count) {
    return plan;
  }
  if (layers[from_index].parent != layers[to_index].parent) {
    plan.target = DragTarget::kCrossParent;
    plan.next_row = from_row;
    return plan;
  }
  const int64_t parent = layers[from_index].parent;
  int32_t ps = 0; /* 自己在兄弟里的序号 */
  int32_t ts = 0; /* 目标在兄弟里的序号 */
  int32_t siblings = 0;
  for (int32_t i = 0; i < count; ++i) {
    if (layers[i].parent != parent) continue;
    if (i < from_index) ++ps;
    if (i < to_index) ++ts;
    ++siblings;
  }
  /* 第 ordinal 个兄弟（自下而上）的数组下标；越界 -1。 */
  const auto sibling_at = [&](int32_t ordinal) -> int32_t {
    int32_t seen = 0;
    for (int32_t i = 0; i < count; ++i) {
      if (layers[i].parent != parent) continue;
      if (seen == ordinal) return i;
      ++seen;
    }
    return -1;
  };
  plan.target = DragTarget::kOk;
  plan.next_row = to_row;
  if (ts == siblings - 1) {
    plan.before_index = -1;
  } else if (ts < ps) {
    plan.before_index = sibling_at(ts);
  } else {
    plan.before_index = sibling_at(ts + 1);
  }
  return plan;
}

/* 拖拽指示线的插槽：向上拖画在目标行上边缘，向下拖画在下边缘（都表示「落在这里」）。 */
inline int32_t layer_drag_insert_slot(int32_t from_shown, int32_t to_shown) {
  return to_shown >= from_shown ? to_shown + 1 : to_shown;
}

/*
 * 拖行落点指示线：画在 to_shown 行的**上边缘**；to_shown == count 表示最后一行
 * 之后（底部）。row_top 必须与 layer_row_rect / draw_layer_panel 用同一个偏移，
 * 否则直方图一开指示线就整体上移 64px——「线在上一行、贴纸落到这一行」。
 */
inline Rect layer_insert_line_rect(const Rect& panel, int row_top, int32_t to_shown,
                                   int32_t count) {
  int32_t slot = to_shown;
  if (slot < 0) slot = 0;
  if (slot > count) slot = count;
  const int y = panel.y + kLayerHeaderHeight + row_top + slot * kLayerRowHeight;
  return Rect{panel.x + 4, y - 1, panel.w - 8, 2};
}

/* 落点行：把鼠标 y 换算成 0..count-1 的面板行（上/下越界夹到首/末行）。 */
inline int32_t layer_drag_target_row(const Rect& panel, int32_t count, double y, int row_top) {
  if (count <= 0) return -1;
  const double top = static_cast<double>(panel.y + kLayerHeaderHeight + row_top);
  const double row = std::floor((y - top) / static_cast<double>(kLayerRowHeight));
  if (row < 0.0) return 0;
  if (row >= static_cast<double>(count)) return count - 1;
  return static_cast<int32_t>(row);
}

/*
 * 拖到目标行的**中间带**（P0-1，拖进组）：行高 22px，上下各 30% 仍是插入位，
 * 中间 40% 才解释成「放进这个组」。同父重排的语义不受影响——只有落点是组、
 * 且指针在中间带时，才走跨父级 move；边缘拖拽仍是原来的同父插入。
 */
inline bool layer_drag_inner_band(const Rect& panel, int32_t count, int32_t row, double y,
                                  int row_top) {
  if (row < 0 || row >= count) return false;
  const double top = static_cast<double>(panel.y + kLayerHeaderHeight + row_top);
  const double local = y - top - static_cast<double>(row) * kLayerRowHeight;
  return local > kLayerRowHeight * 0.3 && local < kLayerRowHeight * 0.7;
}

/* -------------------------------------------------- 直方图区间缩放（S24） */

/*
 * 直方图可视窗口：亮度 [lo, hi] 铺满整条绘图区宽度，默认全幅 0…255。
 * 交互：绘图区横向拖拽 = 放大到拖出的区间（PS 直方图同款：框一段亮度看细节）；
 * 滚轮 = 以光标亮度为中心缩放；双击 = 复位全幅。
 *
 * 夹取规则也放在这里：最小跨度 kHistogramMinSpan 个 bin。允许 0 宽会让映射除以 0，
 * NaN 坐标传给 GDI 的结果是一整块空白——不报错、只在缩放到极限时偶现。
 */
struct HistogramView {
  double lo = 0.0;
  double hi = 255.0;
};

inline constexpr double kHistogramMinSpan = 8.0;
inline constexpr double kHistogramMaxBin = 255.0;

inline HistogramView clamp_histogram_view(double lo, double hi) {
  HistogramView view{};
  /* NaN 比较全 false，先挡掉再谈大小关系。 */
  if (!(lo == lo) || !(hi == hi)) return view;
  if (hi < lo) {
    const double swap = lo;
    lo = hi;
    hi = swap;
  }
  if (hi - lo < kHistogramMinSpan) {
    const double center = (lo + hi) * 0.5;
    lo = center - kHistogramMinSpan * 0.5;
    hi = center + kHistogramMinSpan * 0.5;
  }
  if (lo < 0.0) {
    hi -= lo;
    lo = 0.0;
  }
  if (hi > kHistogramMaxBin) {
    lo -= hi - kHistogramMaxBin;
    hi = kHistogramMaxBin;
  }
  if (lo < 0.0) lo = 0.0;
  if (hi > kHistogramMaxBin) hi = kHistogramMaxBin;
  /*
   * 跨度顶满全幅时只能落在 0..255，顺势归位。
   *
   * 不归位的话，先缩到全幅、再继续「缩小一档」会得到 lo = 1.8e-15、hi = 255：
   * 画面与全幅完全一样，但 histogram_view_is_full 判假，标签就会一直显示
   * 「直方图 0–255」这种自相矛盾的放大态。差一个浮点 ulp 的判定不该由 UI 承担。
   */
  if (hi - lo >= kHistogramMaxBin) {
    lo = 0.0;
    hi = kHistogramMaxBin;
  }
  view.lo = lo;
  view.hi = hi;
  return view;
}

inline double histogram_view_span(const HistogramView& view) { return view.hi - view.lo; }

inline bool histogram_view_is_full(const HistogramView& view) {
  return view.lo <= 0.0 && view.hi >= kHistogramMaxBin;
}

/* 以 focus（亮度）为中心按 factor 缩放：factor < 1 放大，factor > 1 缩小。 */
inline HistogramView zoom_histogram_view(const HistogramView& view, double focus,
                                         double factor) {
  if (!(factor > 0.0) || factor == 1.0) return clamp_histogram_view(view.lo, view.hi);
  const double current_lo = view.lo;
  const double current_hi = view.hi;
  const double span = current_hi - current_lo;
  if (!(span > 0.0)) return clamp_histogram_view(focus - kHistogramMinSpan * 0.5,
                                                 focus + kHistogramMinSpan * 0.5);
  double focus_bin = focus;
  if (focus_bin < current_lo) focus_bin = current_lo;
  if (focus_bin > current_hi) focus_bin = current_hi;
  double new_span = span * factor;
  if (new_span > kHistogramMaxBin) new_span = kHistogramMaxBin;
  /* 保持 focus 在窗口里的相对位置不变：光标下的那根柱不跑。 */
  const double t = (focus_bin - current_lo) / span;
  const double new_lo = focus_bin - t * new_span;
  return clamp_histogram_view(new_lo, new_lo + new_span);
}

/* bin（0…255，可带小数）-> 绘图区 x。 */
inline double histogram_bin_x(const Rect& plot, const HistogramView& view, double bin) {
  const double span = histogram_view_span(view);
  if (!(span > 0.0) || !(plot.w > 0.0)) return plot.x;
  return plot.x + ((bin - view.lo) / span) * plot.w;
}

/* 绘图区 x -> bin（未夹取，调用方按需夹到 0…255）。 */
inline double histogram_x_to_bin(const Rect& plot, const HistogramView& view, double x) {
  if (!(plot.w > 0.0)) return view.lo;
  const double span = histogram_view_span(view);
  const double t = (x - plot.x) / plot.w;
  return view.lo + t * span;
}

/* 拖拽区间 -> 新窗口：两端按 x 排序（允许反向拖），单端超界先夹再交给 clamp。 */
inline HistogramView histogram_window_from_drag(const Rect& plot, const HistogramView& view,
                                                double x0, double x1) {
  const double left = x0 < x1 ? x0 : x1;
  const double right = x0 < x1 ? x1 : x0;
  double a = histogram_x_to_bin(plot, view, left);
  double b = histogram_x_to_bin(plot, view, right);
  if (a < 0.0) a = 0.0;
  if (b > kHistogramMaxBin) b = kHistogramMaxBin;
  return clamp_histogram_view(a, b);
}

/* ------------------------------------- 调整面板：色阶通道条 / 曲线编辑器（P0-3） */

/*
 * 色阶 = 4 通道 × 5 参（`levels.ranges.N.*`，顺序由引擎表钉住）。面板按通道分页，
 * 通道条右对齐排在**标题行**（与「调整（Adjustment）」同高），不占参数行高度——
 * 参数行几何因此不用为色阶单独开一套，滚动/命中仍走 adjust_param_*。
 */
inline constexpr int kAdjustChannelChipWidth = 34;
inline constexpr int kAdjustChannelChipHeight = 18;
inline constexpr int kAdjustChannelChipGap = 4;

inline Rect adjust_channel_chip_rect(const Rect& panel, int channel, int channel_count) {
  if (channel_count < 1) channel_count = 1;
  if (channel < 0) channel = 0;
  if (channel >= channel_count) channel = channel_count - 1;
  const int total =
      channel_count * kAdjustChannelChipWidth + (channel_count - 1) * kAdjustChannelChipGap;
  /*
   * §5.43：标题行右端让给整块收起按钮（18px + 8px 边距），通道条整体左移同样的
   * 宽度。不左移的话按钮会盖住最后一个通道格（"B"），而通道格是能点的——被盖住
   * 的那一页就再也切不过去了。
   */
  const int x0 = panel.right() - 10 - Layout::kPanelToggleSize -
                 Layout::kPanelToggleRightMargin - total;
  return Rect{x0 + channel * (kAdjustChannelChipWidth + kAdjustChannelChipGap), panel.y + 6,
              kAdjustChannelChipWidth, kAdjustChannelChipHeight};
}

inline int adjust_channel_chip_at(const Rect& panel, int channel_count, double x, double y) {
  for (int c = 0; c < channel_count; ++c) {
    if (adjust_channel_chip_rect(panel, c, channel_count).contains(x, y)) return c;
  }
  return -1;
}

/*
 * 曲线编辑器：顶部一排通道条（RGB / R / G / B），下面是正方形绘图区。
 * 坐标系与引擎同口径：控制点是 0..255 的 (x, y)，y 向上为大——屏幕 y 要翻转。
 * 屏幕 <-> 值的换算只此一份，绘制折线、画控制点、命中、拖动全部复用它。
 */
inline constexpr int kCurveChannelBarHeight = 22;
inline constexpr int kCurveChannelChipWidth = 42;
inline constexpr int kCurveMinPlotSide = 48;

inline Rect curve_channel_chip_rect(const Rect& panel, int channel, int channel_count) {
  if (channel_count < 1) channel_count = 1;
  if (channel < 0) channel = 0;
  if (channel >= channel_count) channel = channel_count - 1;
  const int total =
      channel_count * kCurveChannelChipWidth + (channel_count - 1) * kAdjustChannelChipGap;
  const int x0 = panel.x + (panel.w - total) / 2;
  return Rect{x0 + channel * (kCurveChannelChipWidth + kAdjustChannelChipGap),
              panel.y + kAdjustParamRowsTop, kCurveChannelChipWidth, kAdjustChannelChipHeight};
}

inline int curve_channel_chip_at(const Rect& panel, int channel_count, double x, double y) {
  for (int c = 0; c < channel_count; ++c) {
    if (curve_channel_chip_rect(panel, c, channel_count).contains(x, y)) return c;
  }
  return -1;
}

/* 绘图区：通道条以下的可用区里取最大的正方形，水平居中；放不下就返回空矩形。 */
inline Rect adjust_curve_plot_rect(const Rect& panel) {
  const int top = panel.y + kAdjustParamRowsTop + kCurveChannelBarHeight;
  const int avail_w = panel.w - 24;
  const int avail_h = panel.bottom() - kAdjustPanelFooter - top - 8;
  if (avail_w < kCurveMinPlotSide || avail_h < kCurveMinPlotSide) return Rect{};
  const int side = avail_w < avail_h ? avail_w : avail_h;
  return Rect{panel.x + (panel.w - side) / 2, top, side, side};
}

inline double curve_plot_value_to_x(const Rect& plot, double value) {
  if (!(plot.w > 1.0)) return plot.x;
  if (value < 0.0) value = 0.0;
  if (value > 255.0) value = 255.0;
  return plot.x + (value / 255.0) * (plot.w - 1.0);
}

inline double curve_plot_value_to_y(const Rect& plot, double value) {
  if (!(plot.h > 1.0)) return plot.y;
  if (value < 0.0) value = 0.0;
  if (value > 255.0) value = 255.0;
  return plot.y + (1.0 - value / 255.0) * (plot.h - 1.0);
}

inline double curve_plot_x_to_value(const Rect& plot, double x) {
  if (!(plot.w > 1.0)) return 0.0;
  double t = (x - plot.x) / (plot.w - 1.0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * 255.0;
}

inline double curve_plot_y_to_value(const Rect& plot, double y) {
  if (!(plot.h > 1.0)) return 0.0;
  double t = 1.0 - (y - plot.y) / (plot.h - 1.0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * 255.0;
}

/* 控制点命中：返回半径内最近的点，没有则 -1。points_xy 是 (x, y) 交替存放。 */
inline int curve_point_hit(const Rect& plot, const double* points_xy, int count, double x,
                           double y, double radius) {
  if (points_xy == nullptr || radius <= 0.0) return -1;
  int best = -1;
  double best_d2 = radius * radius;
  for (int i = 0; i < count; ++i) {
    const double dx = curve_plot_value_to_x(plot, points_xy[2 * i]) - x;
    const double dy = curve_plot_value_to_y(plot, points_xy[2 * i + 1]) - y;
    const double d2 = dx * dx + dy * dy;
    if (d2 <= best_d2) {
      best_d2 = d2;
      best = i;
    }
  }
  return best;
}

/*
 * 空白处加点：取最近的整数 x，必须严格落在相邻两点之间且两侧各留 1 个单位
 * （引擎要求 x 严格递增）。没有空位返回 NaN，调用方据此不加点。
 */
inline double curve_insert_x(const double* points_xy, int count, double value_x) {
  if (points_xy == nullptr || count < 2) return std::numeric_limits<double>::quiet_NaN();
  const double target = std::floor(value_x + 0.5);
  for (int i = 0; i + 1 < count; ++i) {
    const double left = points_xy[2 * i];
    const double right = points_xy[2 * i + 2];
    if (target > left && target < right) {
      if (target - left < 1.0 || right - target < 1.0) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      return target;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

/*
 * 曲线求值（**只用于面板画折线**）：保形三次 Hermite / Fritsch–Carlson 斜率，
 * 与 engine/compositing/adjustment_eval.cpp 的 `Curve::apply` 同口径。面板画出来的
 * 形状必须等于画布上实际的亮度映射，否则用户拖的是 A、看到的是 B。
 */
inline double curve_eval(const double* points_xy, int count, double x) {
  if (points_xy == nullptr || count < 2) return x;
  if (count > 32) count = 32;
  if (x <= points_xy[0]) return points_xy[1];
  if (x >= points_xy[2 * count - 2]) return points_xy[2 * count - 1];

  double slopes[32];
  for (int i = 0; i + 1 < count; ++i) {
    const double span = points_xy[2 * i + 2] - points_xy[2 * i];
    slopes[i] = span > 1e-9 ? (points_xy[2 * i + 3] - points_xy[2 * i + 1]) / span : 0.0;
  }
  /* 端点用邻段斜率；中间点取调和平均，异号或为零则取 0（保形，不过冲）。 */
  auto slope_at = [&](int j) {
    if (j <= 0) return slopes[0];
    if (j >= count - 1) return slopes[count - 2];
    if (slopes[j - 1] * slopes[j] <= 0.0) return 0.0;
    return 2.0 / (1.0 / slopes[j - 1] + 1.0 / slopes[j]);
  };

  int i = 0;
  while (i + 2 < count && x > points_xy[2 * i + 2]) ++i;
  const double x0 = points_xy[2 * i];
  const double y0 = points_xy[2 * i + 1];
  const double x1 = points_xy[2 * i + 2];
  const double y1 = points_xy[2 * i + 3];
  const double h = x1 - x0;
  const double t = h > 1e-9 ? (x - x0) / h : 0.0;
  const double t2 = t * t;
  const double t3 = t2 * t;
  double y = (2.0 * t3 - 3.0 * t2 + 1.0) * y0 + (t3 - 2.0 * t2 + t) * h * slope_at(i) +
             (-2.0 * t3 + 3.0 * t2) * y1 + (t3 - t2) * h * slope_at(i + 1);
  if (y < 0.0) y = 0.0;
  if (y > 255.0) y = 255.0;
  return y;
}

/* ------------------------------------------------- 文档级对话框（P0 接线） */

/*
 * 新建 / 图像大小 / 画布大小 / 历史容量四个对话框共用一套行模型：数值行、
 * 枚举行、开关行。行几何、夹取、锚点与百分比换算都放在这里，壳只负责取值
 * 和调用既有 ABI——「面板上显示的数字」与「真正写进文档的数字」必须同源，
 * 否则一旦两边各写一份夹取，用户就会看到 30000 却得到 16384。
 *
 * 这四个对话框全都**不新增 ABI**：
 *   - 新建：comp_document_desc 的 width/height/dpi/color_space；
 *   - 图像大小：comp_document_resize_image（整篇重采样）；
 *   - 画布大小：comp_txn_begin + comp_layer_set_transform + comp_document_set_size；
 *   - 历史容量：comp_document_set_history_limit / set_history_budget。
 *
 * 关闭提示（ClosePrompt，P0-3）也吃这套行模型，但它只有一行「动作」，没有数值：
 * 目的不是填参数，而是**把模态确认放进右栏面板**，让自动化能按 D 回答。
 */
enum class DocDialogKind {
  None = 0,
  NewDocument,
  ImageSize,
  CanvasSize,
  HistoryLimit,
  /*
   * P1-5：Camera Raw 面板复用同一套行模型与键盘/命中通路，但它是**图层级**
   * 而非文档级的——回车把整层像素按设置跑一遍相机管线（一步撤销），Esc 不改。
   * 值本身不回读（ABI 没有 settings getter，且滤镜是破坏性的），每次打开都从
   * 上游缺省值起算；这是如实反映引擎契约，不是取巧。
   */
  CameraRaw,
  /*
   * P0-3：关闭 / 退出前的未保存提示。行模型里只有一个「动作」枚举行，
   * 三档 = 保存并关闭 / 不保存直接关闭 / 取消。
   *
   * **刻意不用 MessageBox**：自动化 `stop()` 是「post WM_CLOSE 后等一会儿，
   * 没退就补一个键」，系统模态弹窗既不吃注入的键、又会把每次目视跑拖成超时。
   * 放进右栏面板后它和别的文档级对话框共用同一套键位与命中，脚本按 D 即可。
   */
  ClosePrompt,
  /*
   * P0-5 收尾：变换选区（Select > Transform Selection 的数值/旋转部分）。
   * 选区手柄只能做轴对齐缩放；这里补上「宽 / 高 / 角度」三行数值输入，
   * 回车走 comp_selection_transform（ABI 尾部第 167 项）。中心点固定为
   * 当前基础蒙版的包围盒中心，一次命令内可预期；Esc 不改文档。
   */
  TransformSelection,
  /*
   * P1-3：数值变换（图层级）。选中图层已经能按 Ctrl+T 手柄缩放、按 15° 步进
   * 旋转，但**没有地方输入确定数**——「把它变成正好 512 px 宽」只能靠拖，
   * 拖完还得看状态栏猜。这里补「宽 / 高 / 角度」三行，中心锚定，回车走既有的
   * comp_layer_set_transform（零 ABI 新增）；再次变换（Ctrl+Shift+T）复读上
   * 一次的比例与角度增量。
   *
   * 组与空图层没有可变换像素：打开即拒绝并说明原因，不画一个改了也没用的面板。
   */
  TransformLayer,
  /*
   * P1-7 收尾：可编辑色标表。引擎早就收 32 个色标 + 每段中点，外壳却只有三张
   * 只读预设——「把彩虹的绿往左挪一点」在 UI 里没有入口。这里用同一套行模型
   * 补一张**数值色标表**（选中 / 位置 / 中点 / RGB / 不透明度 / 增删），回车把
   * 整表写进渐变工具的「自定义」档；零 ABI 新增，落笔仍走 comp_tool_apply 的
   * `gradient_stops` 尾部字段。
   *
   * 刻意不做拖拽手柄：右栏只有两百多像素，拖拽精度不如 0.1% 的数值输入，而且
   * 会与现有「面板内按下 = 点行」的命中规则打架。数值表是等价能力的更稳实现。
   */
  GradientStops
};

/* 可编辑色标面板的固定行序（下标是对外契约：测试与 visual_check 都按下标点）。 */
inline constexpr int kGradientStopsRowCount = 8;

enum class DocRowKind { Number = 0, Choice = 1, Toggle = 2 };

struct DocRowSpec {
  const char* label;
  DocRowKind kind;
  int min;
  int max;
  int step;         /* ← / → 与 - / = 的单步步长（数值行） */
  int choice_count; /* 枚举行档位数；数值/开关行为 0 */
};

/* 画布尺寸的合法区间，与 comp_document_desc 的注释（1..30000）同口径。 */
inline constexpr int kDocSizeMin = 1;
inline constexpr int kDocSizeMax = 30000;
/* dpi：0 表示引擎默认 72；对话框里给 1..2400 的可选区间。 */
inline constexpr int kDocDpiMin = 1;
inline constexpr int kDocDpiMax = 2400;
/* 历史步数：0 = 引擎默认 200，上限给到 2000（快照式撤销，再高内存吃不住）。 */
inline constexpr int kDocHistoryStepsMax = 2000;
/* 历史内存预算：0 = 引擎默认 512 MiB；界面上按 MiB 显示，上限 16 GiB。 */
inline constexpr int kDocHistoryBudgetMaxMb = 16384;
/*
 * Camera Raw 面板的行数（Basic 组十二项）。曝光用 0.01 EV 的整数近似，
 * -500..500 对应 docs/camera-raw.md §3.1 的 -5..5 档。
 */
inline constexpr int kCameraRawRowCount = 12;
inline constexpr int kCameraRawExposureMax = 500;

/* 变换选区的角度：-180..180 度（顺时针为正），←/→ 以 15° 为步长；
 * 数字键入不受步长限制，-180 这类值按行首负号输入。 */
inline constexpr int kTransformAngleMin = -180;
inline constexpr int kTransformAngleMax = 180;
inline constexpr int kTransformAngleStep = 15;

/*
 * 关闭提示的三个动作。行值直接就是这三个常量：面板显示、状态栏、执行分支读同
 * 一份数，不会出现「显示保存、实际丢弃」。缺省 = 保存（与 PS 的默认按钮同侧）。
 */
inline constexpr int kCloseActionSave = 0;
inline constexpr int kCloseActionDiscard = 1;
inline constexpr int kCloseActionCancel = 2;

inline const char* doc_close_action_name(int action) {
  switch (action) {
    case kCloseActionSave: return "保存并关闭";
    case kCloseActionDiscard: return "不保存，直接关闭";
    case kCloseActionCancel: return "取消（留在文档）";
    default: return "?";
  }
}

inline int clamp_doc_size(int value) {
  if (value < kDocSizeMin) return kDocSizeMin;
  if (value > kDocSizeMax) return kDocSizeMax;
  return value;
}

inline int clamp_doc_dpi(int value) {
  if (value < kDocDpiMin) return kDocDpiMin;
  if (value > kDocDpiMax) return kDocDpiMax;
  return value;
}

inline int clamp_history_steps(int value) {
  if (value < 0) return 0;
  if (value > kDocHistoryStepsMax) return kDocHistoryStepsMax;
  return value;
}

inline int clamp_history_budget_mb(int value) {
  if (value < 0) return 0;
  if (value > kDocHistoryBudgetMaxMb) return kDocHistoryBudgetMaxMb;
  return value;
}

/*
 * 图层旋转角 -> 面板角度行：先归一到 (-180, 180] 再四舍五入取整。
 * 用半开区间而不是 [-180, 180]，与旋转命令的 (-360, 360] 归一化同思路：
 * 端点只取一侧，lround 之后不会漂到域外。旋转角本身可能带小数（手柄拖动），
 * 这里只负责显示；数值变换在角度行没被改动时会原样写回带小数的角度
 * （见 shell_document.cpp 的 TransformLayer 分支），所以取整不会改坏图层。
 */
inline int doc_angle_from_rotation(double degrees) {
  if (!std::isfinite(degrees)) return 0;
  while (degrees <= -180.0) degrees += 360.0;
  while (degrees > 180.0) degrees -= 360.0;
  int value = static_cast<int>(std::lround(degrees));
  if (value < kTransformAngleMin) value = kTransformAngleMin;
  if (value > kTransformAngleMax) value = kTransformAngleMax;
  return value;
}

inline const char* doc_dialog_title(DocDialogKind kind) {
  switch (kind) {
    case DocDialogKind::NewDocument:
      return "新建文档（New Document）";
    case DocDialogKind::ImageSize:
      return "图像大小（Image Size）";
    case DocDialogKind::CanvasSize:
      return "画布大小（Canvas Size）";
    case DocDialogKind::HistoryLimit:
      return "历史记录容量（History）";
    case DocDialogKind::CameraRaw:
      return "Camera Raw 滤镜（Camera Raw Filter）";
    case DocDialogKind::ClosePrompt:
      return "关闭文档（Close）";
    case DocDialogKind::TransformSelection:
      return "变换选区（Transform Selection）";
    case DocDialogKind::TransformLayer:
      return "数值变换（Transform）";
    case DocDialogKind::GradientStops:
      return "可编辑色标（Gradient Stops）";
    default:
      return "";
  }
}

inline const char* doc_dialog_subtitle(DocDialogKind kind) {
  switch (kind) {
    case DocDialogKind::NewDocument:
      return "画布尺寸 / 分辨率 / 背景";
    case DocDialogKind::ImageSize:
      return "整篇重采样；选区会被丢弃";
    case DocDialogKind::CanvasSize:
      return "改画布并按锚点平移内容；裁剪不可恢复";
    case DocDialogKind::HistoryLimit:
      return "0 = 引擎默认（200 步 / 512 MiB）";
    case DocDialogKind::CameraRaw:
      return "对选中图层整层写入（Basic 组；一步撤销）";
    case DocDialogKind::ClosePrompt:
      return "文档有未保存的修改 · 关闭前请选择";
    case DocDialogKind::TransformSelection:
      return "中心固定为当前选区中心 · 角度顺时针为正";
    case DocDialogKind::TransformLayer:
      return "作用于选中图层 · 中心锚定 · Ctrl+Shift+T 再次变换";
    case DocDialogKind::GradientStops:
      return "改完回车应用为「自定义」色标表 · 首末固定在 0% / 100%";
    default:
      return "";
  }
}

inline const char* doc_dialog_hint(DocDialogKind kind) {
  switch (kind) {
    case DocDialogKind::NewDocument:
      return "Tab 切行 · 数字键输入 · 回车应用";
    case DocDialogKind::ImageSize:
      return "Tab 切行 · 空格关约束 · 回车应用";
    case DocDialogKind::CanvasSize:
      return "Tab 切行 · ←/→ 循环锚点 · 回车应用";
    case DocDialogKind::HistoryLimit:
      return "Tab 切行 · 0 = 默认 · 回车应用";
    case DocDialogKind::CameraRaw:
      return "Tab 切行 · 负值用 - 开头 · 回车应用";
    case DocDialogKind::ClosePrompt:
      return "不保存 = 这次修改连同撤销栈一起丢弃";
    case DocDialogKind::TransformSelection:
      return "Tab 切行 · 数字键输入 · 回车应用（Esc 不改）";
    case DocDialogKind::TransformLayer:
      return "Tab 切行 · 数字键输入 · 回车应用（Esc 不改）";
    case DocDialogKind::GradientStops:
      return "位置/中点按 % 值输入（可带 .5）· ←/→ = 5‰";
    default:
      return "";
  }
}

inline int doc_dialog_row_count(DocDialogKind kind) {
  switch (kind) {
    case DocDialogKind::NewDocument:
      return 4; /* 宽 / 高 / 分辨率 / 背景 */
    case DocDialogKind::ImageSize:
      return 4; /* 宽 / 高 / 约束比例 / 百分比 */
    case DocDialogKind::CanvasSize:
      return 3; /* 宽 / 高 / 锚点 */
    case DocDialogKind::HistoryLimit:
      return 2; /* 步数上限 / 内存上限 */
    case DocDialogKind::CameraRaw:
      return kCameraRawRowCount;
    case DocDialogKind::ClosePrompt:
      return 1; /* 动作 */
    case DocDialogKind::TransformSelection:
      return 3; /* 宽 / 高 / 角度 */
    case DocDialogKind::TransformLayer:
      return 3; /* 宽 / 高 / 角度 */
    case DocDialogKind::GradientStops:
      return kGradientStopsRowCount;
    default:
      return 0;
  }
}

inline DocRowSpec doc_dialog_row(DocDialogKind kind, int row) {
  switch (kind) {
    case DocDialogKind::NewDocument:
      switch (row) {
        case 0: return DocRowSpec{"宽度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 64, 0};
        case 1: return DocRowSpec{"高度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 64, 0};
        case 2: return DocRowSpec{"分辨率 (dpi)", DocRowKind::Number, kDocDpiMin, kDocDpiMax, 1, 0};
        case 3: return DocRowSpec{"背景", DocRowKind::Choice, 0, 2, 1, 3};
        default: break;
      }
      break;
    case DocDialogKind::ImageSize:
      switch (row) {
        case 0: return DocRowSpec{"宽度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 64, 0};
        case 1: return DocRowSpec{"高度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 64, 0};
        case 2: return DocRowSpec{"约束比例", DocRowKind::Toggle, 0, 1, 1, 0};
        case 3: return DocRowSpec{"百分比 (%)", DocRowKind::Number, 1, 10000, 5, 0};
        default: break;
      }
      break;
    case DocDialogKind::CanvasSize:
      switch (row) {
        case 0: return DocRowSpec{"宽度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 64, 0};
        case 1: return DocRowSpec{"高度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 64, 0};
        case 2: return DocRowSpec{"锚点", DocRowKind::Choice, 0, 8, 1, 9};
        default: break;
      }
      break;
    case DocDialogKind::HistoryLimit:
      switch (row) {
        case 0:
          return DocRowSpec{"步数上限", DocRowKind::Number, 0, kDocHistoryStepsMax, 10, 0};
        case 1:
          return DocRowSpec{"内存上限 (MiB)", DocRowKind::Number, 0,
                            kDocHistoryBudgetMaxMb, 64, 0};
        default: break;
      }
      break;
    case DocDialogKind::CameraRaw:
      switch (row) {
        /* 曝光以 0.01 EV 为整数单位（-500..500 即 -5.00..+5.00），面板只收整数，
         * 写 JSON 时才除以 100——与 docs/camera-raw.md §3.1 的量程逐项对齐。 */
        case 0:
          return DocRowSpec{"曝光 (0.01 EV)", DocRowKind::Number, -kCameraRawExposureMax,
                            kCameraRawExposureMax, 5, 0};
        case 1: return DocRowSpec{"色温", DocRowKind::Number, -100, 100, 5, 0};
        case 2: return DocRowSpec{"色调", DocRowKind::Number, -100, 100, 5, 0};
        case 3: return DocRowSpec{"对比", DocRowKind::Number, -100, 100, 5, 0};
        case 4: return DocRowSpec{"高光", DocRowKind::Number, -100, 100, 5, 0};
        case 5: return DocRowSpec{"阴影", DocRowKind::Number, -100, 100, 5, 0};
        case 6: return DocRowSpec{"白色", DocRowKind::Number, -100, 100, 5, 0};
        case 7: return DocRowSpec{"黑色", DocRowKind::Number, -100, 100, 5, 0};
        case 8: return DocRowSpec{"清晰度", DocRowKind::Number, -100, 100, 5, 0};
        case 9: return DocRowSpec{"鲜艳度", DocRowKind::Number, -100, 100, 5, 0};
        case 10: return DocRowSpec{"饱和度", DocRowKind::Number, -100, 100, 5, 0};
        case 11: return DocRowSpec{"去雾", DocRowKind::Number, -100, 100, 5, 0};
        default: break;
      }
      break;
    case DocDialogKind::ClosePrompt:
      if (row == 0) {
        return DocRowSpec{"动作", DocRowKind::Choice, 0, 2, 1, 3};
      }
      break;
    case DocDialogKind::TransformSelection:
      switch (row) {
        case 0: return DocRowSpec{"宽度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 8, 0};
        case 1: return DocRowSpec{"高度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 8, 0};
        case 2:
          return DocRowSpec{"角度 (°)", DocRowKind::Number, kTransformAngleMin,
                            kTransformAngleMax, kTransformAngleStep, 0};
        default: break;
      }
      break;
    case DocDialogKind::TransformLayer:
      /* 与选区那条同量程：宽度 1..30000（图层像素尺寸上限与文档同口径），
       * 角度 -180..180、步进 15°——和 图层 > 逆/顺时针旋转 15° 是同一个档位，
       * 键盘与菜单对同一图层给出可预期的相同结果。 */
      switch (row) {
        case 0: return DocRowSpec{"宽度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 8, 0};
        case 1: return DocRowSpec{"高度 (px)", DocRowKind::Number, kDocSizeMin, kDocSizeMax, 8, 0};
        case 2:
          return DocRowSpec{"角度 (°)", DocRowKind::Number, kTransformAngleMin,
                            kTransformAngleMax, kTransformAngleStep, 0};
        default: break;
      }
      break;
    case DocDialogKind::GradientStops:
      /*
       * 顺序：选中的色标 / 位置 / 中点 / R G B / 不透明度 / 增删。
       * 位置与中点按千分位存，输入框收百分数（可带一位小数），显示成 50.0%；
       * ←/→ 一次走 5‰（= 0.5%），与 PS 的色标微调档同量级。
       */
      switch (row) {
        case 0:
          return DocRowSpec{"色标 (#)", DocRowKind::Number, 1, kGradientMaxStops, 1, 0};
        case 1: return DocRowSpec{"位置 (%)", DocRowKind::Number, 0, 1000, 5, 0};
        case 2: return DocRowSpec{"中点 (%)", DocRowKind::Number, 1, 999, 5, 0};
        case 3: return DocRowSpec{"R", DocRowKind::Number, 0, 255, 1, 0};
        case 4: return DocRowSpec{"G", DocRowKind::Number, 0, 255, 1, 0};
        case 5: return DocRowSpec{"B", DocRowKind::Number, 0, 255, 1, 0};
        case 6: return DocRowSpec{"不透明度 (%)", DocRowKind::Number, 0, 100, 1, 0};
        case 7: return DocRowSpec{"增删", DocRowKind::Choice, 0, 2, 1, 3};
        default: break;
      }
      break;
    default:
      break;
  }
  return DocRowSpec{"", DocRowKind::Number, 0, 0, 1, 0};
}

inline const char* doc_new_background_name(int choice) {
  switch (choice) {
    case 0: return "白色";
    case 1: return "透明";
    case 2: return "前景色";
    default: return "?";
  }
}

/* 锚点按 3×3 行优先编号：0=左上 … 4=居中 … 8=右下。 */
inline const char* doc_anchor_name(int anchor) {
  static const char* names[9] = {"左上", "上中", "右上", "左中", "居中",
                                 "右中", "左下", "下中", "右下"};
  if (anchor < 0 || anchor > 8) return "?";
  return names[anchor];
}

/* 锚点 -> 内容位移。列号 = anchor % 3，行号 = anchor / 3，取 0 / 半个 / 一个增量。 */
inline int doc_anchor_offset(int delta, int index) {
  if (index <= 0) return 0;
  if (index >= 2) return delta;
  return delta >= 0 ? delta / 2 : -((-delta) / 2);
}

inline int doc_anchor_dx(int anchor, int delta_width) {
  if (anchor < 0 || anchor > 8) anchor = 4;
  return doc_anchor_offset(delta_width, anchor % 3);
}

inline int doc_anchor_dy(int anchor, int delta_height) {
  if (anchor < 0 || anchor > 8) anchor = 4;
  return doc_anchor_offset(delta_height, anchor / 3);
}

/* 百分比 -> 尺寸：四舍五入后夹取，和面板上显示的数字一致。 */
inline int doc_size_from_percent(int base, int percent) {
  if (base <= 0) return kDocSizeMin;
  if (percent <= 0) percent = 1;
  const double scaled = static_cast<double>(base) * static_cast<double>(percent) / 100.0;
  return clamp_doc_size(static_cast<int>(std::lround(scaled)));
}

/* 尺寸 -> 百分比（仅用于面板回显；不写回字段，避免来回取整漂移）。 */
inline int doc_percent_from_size(int base, int value) {
  if (base <= 0) return 100;
  const double percent = static_cast<double>(value) * 100.0 / static_cast<double>(base);
  if (percent < 1.0) return 1;
  if (percent > 10000.0) return 10000;
  return static_cast<int>(std::lround(percent));
}

/*
 * 约束比例联动：改宽按原比例推高，改高按原比例推宽。两个方向都从**原始尺寸**
 * 取比例，而不是从上一帧的结果——否则连续微调会一路累积取整误差。
 */
inline int doc_locked_height(int original_width, int original_height, int width) {
  if (original_width <= 0) return original_height;
  const double height = static_cast<double>(width) * static_cast<double>(original_height) /
                        static_cast<double>(original_width);
  return clamp_doc_size(static_cast<int>(std::lround(height)));
}

inline int doc_locked_width(int original_width, int original_height, int height) {
  if (original_height <= 0) return original_width;
  const double width = static_cast<double>(height) * static_cast<double>(original_width) /
                       static_cast<double>(original_height);
  return clamp_doc_size(static_cast<int>(std::lround(width)));
}

/* 行几何：绘制与命中同源（与 trim_row_rect 同一套约定）。 */
inline constexpr int kDocDialogRowHeight = 22;
inline constexpr int kDocDialogRowsTop = 56;

inline Rect doc_dialog_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kDocDialogRowsTop + row * kDocDialogRowHeight, panel.w,
              kDocDialogRowHeight};
}

inline int doc_dialog_row_at(const Rect& panel, int row_count, double x, double y) {
  if (row_count <= 0 || !panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kDocDialogRowsTop);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kDocDialogRowHeight)));
  if (row < 0 || row >= row_count) return -1;
  return row;
}

/*
 * Camera Raw 行的 JSON 键：与 `docs/camera-raw.md` §3.1 的字段名逐字一致。
 * 拼错一个键引擎就整条拒绝（COMP_ERR_INVALID_ARGUMENT，未知键不忽略），
 * 所以键表放在 UI 核里由单元测试钉住，而不是散落在壳的拼串代码里。
 */
inline const char* camera_raw_row_key(int row) {
  switch (row) {
    case 0: return "exposure";
    case 1: return "temperature";
    case 2: return "tint";
    case 3: return "contrast";
    case 4: return "highlights";
    case 5: return "shadows";
    case 6: return "whites";
    case 7: return "blacks";
    case 8: return "clarity";
    case 9: return "vibrance";
    case 10: return "saturation";
    case 11: return "dehaze";
    default: return nullptr;
  }
}

/* 曝光行在 JSON 里是小数：面板整数单位是 0.01 EV，写出去要除以 100。 */
inline bool camera_raw_row_is_exposure(int row) { return row == 0; }

/*
 * 面板值 -> 设置 JSON。**只写非零字段**：零就是上游缺省，写不写等价；
 * 少写几个键既缩短字符串，也让「恒等设置」自然得到 "{}"（引擎据此
 * 不开事务、不动像素，out_changed = 0）。
 *
 * 数值格式刻意固定两位小数（曝光）与十进制整数（其余），由单元测试逐字钉住
 * ——JSON 是跨语言边界，格式漂移比逻辑漂移更难在运行时发现。
 */
inline std::string camera_raw_settings_json(const int* values, int count) {
  std::string json = "{";
  bool first = true;
  for (int row = 0; row < count && row < kCameraRawRowCount; ++row) {
    if (values == nullptr || values[row] == 0) continue;
    const char* key = camera_raw_row_key(row);
    if (key == nullptr) continue;
    char number[32];
    if (camera_raw_row_is_exposure(row)) {
      std::snprintf(number, sizeof(number), "%.2f",
                    static_cast<double>(values[row]) / 100.0);
    } else {
      std::snprintf(number, sizeof(number), "%d", values[row]);
    }
    if (!first) json += ',';
    first = false;
    json += '"';
    json += key;
    json += "\":";
    json += number;
  }
  json += '}';
  return json;
}

}  // namespace comp_ui

#endif /* COMPOSITOR_SHELL_UI_CORE_H */
