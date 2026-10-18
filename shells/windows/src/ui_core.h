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

/*
 * 哪些工具需要画「笔尖轮廓」。Brush / Eraser / Spot Healing / Clone Stamp 与
 * Retouch 族（Blur / Smudge / Liquify）都吃 `brush_size`，画同一个圆；
 * 选区类工具不吃笔刷直径，不画（避免误导）。
 */
inline bool tool_has_brush_tip(ToolId id) {
  switch (id) {
    case ToolId::Brush:
    case ToolId::Eraser:
    case ToolId::Heal:
    case ToolId::Clone:
    case ToolId::Retouch:
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

/* ------------------------------------------------------------ 滤镜面板 */

/*
 * 滤镜种类。数值必须与 ABI `comp_filter_kind` 一致（0..3，尾部追加）；
 * 头文件之间不互相 include，这条约束由 test_ui_core 的 static_assert 钉死。
 */
enum class FilterId : int {
  Vignette = 0,
  TonalContrast = 1,
  Bloom = 2,
  LensCorrection = 3,
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

}  // namespace comp_ui

#endif /* COMPOSITOR_SHELL_UI_CORE_H */
