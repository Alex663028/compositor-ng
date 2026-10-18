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
 *   3. 工具键位逐字对齐上游 `UI/KeyboardShortcuts.swift` 的 ShortcutDefinition.all。
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
 */
struct Layout {
  static constexpr int kToolbarHeight = 46;
  static constexpr int kStatusHeight = 26;
  static constexpr int kLayerPanelWidth = 248;
  static constexpr int kAdjustPanelWidth = 330;
  static constexpr int kMinCanvasWidth = 180;

  Rect toolbar{};
  Rect layer_panel{};
  Rect adjust_panel{};
  Rect canvas{};
  Rect status{};
};

inline Layout compute_layout(int width, int height) {
  if (width < 0) width = 0;
  if (height < 0) height = 0;

  const int toolbar_h = std::min(height, Layout::kToolbarHeight);
  const int status_h = std::min(height - toolbar_h, Layout::kStatusHeight);
  const int body_top = toolbar_h;
  const int body_h = height - toolbar_h - status_h;

  int layer_w = 0;
  if (width >= Layout::kLayerPanelWidth + Layout::kMinCanvasWidth) {
    layer_w = Layout::kLayerPanelWidth;
  }
  int adjust_w = 0;
  if (width - layer_w >= Layout::kAdjustPanelWidth + Layout::kMinCanvasWidth) {
    adjust_w = Layout::kAdjustPanelWidth;
  }

  Layout out;
  out.toolbar = Rect{0, 0, width, toolbar_h};
  out.layer_panel = Rect{0, body_top, layer_w, body_h};
  out.adjust_panel = Rect{width - adjust_w, body_top, adjust_w, body_h};
  out.canvas = Rect{layer_w, body_top, width - layer_w - adjust_w, body_h};
  out.status = Rect{0, body_top + body_h, width, status_h};
  return out;
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

/* 面板显示序：第 0 行 = 最上层图层（与引擎数组序相反，由调用方换算）。 */
inline int layer_row_at(const Rect& panel, int layer_count, double x, double y) {
  if (layer_count <= 0 || !panel.contains(x, y)) return -1;
  const double local = y - static_cast<double>(panel.y + kLayerHeaderHeight);
  if (local < 0.0) return -1;
  const int row = static_cast<int>(std::floor(local / static_cast<double>(kLayerRowHeight)));
  if (row < 0 || row >= layer_count) return -1;
  return row;
}

inline Rect layer_row_rect(const Rect& panel, int row) {
  return Rect{panel.x, panel.y + kLayerHeaderHeight + row * kLayerRowHeight, panel.w,
              kLayerRowHeight};
}

/* ------------------------------------------------------------ 工具与快捷键 */

/* 工具集合与顺序 = 工具条从左到右的显示顺序。 */
enum class ToolId : int {
  Select = 0,
  Move,
  Marquee,
  Lasso,
  Wand,
  Crop,
  Eyedropper,
  Brush,
  Eraser,
  Heal,
  Clone,
  Retouch, /* 上游 "Blur / Smudge / Liquify"，同一个 R 键循环 */
  Gradient,
  Shape,
  Type,
  Hand,
  Zoom,
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
      {ToolId::Wand, "Magic Wand", 'w'},
      {ToolId::Crop, "Crop", 'c'},
      {ToolId::Eyedropper, "Eyedropper", 'i'},
      {ToolId::Brush, "Brush", 'b'},
      {ToolId::Eraser, "Eraser", 'e'},
      {ToolId::Heal, "Spot Healing", 'j'},
      {ToolId::Clone, "Clone Stamp", 's'},
      {ToolId::Retouch, "Blur/Smudge/Liquify", 'r'},
      {ToolId::Gradient, "Gradient", 'g'},
      {ToolId::Shape, "Shape", 'u'},
      {ToolId::Type, "Type", 't'},
      {ToolId::Hand, "Hand", 'h'},
      {ToolId::Zoom, "Zoom", 'z'},
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

}  // namespace comp_ui

#endif /* COMPOSITOR_SHELL_UI_CORE_H */
