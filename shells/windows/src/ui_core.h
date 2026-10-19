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
  Pen, /* 钢笔路径选区：点击落锚点、拖动拉控制柄 */
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
      {ToolId::Pen, "Pen", 'p'},
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
  Retouch, /* COMP_TOOL_BLUR / SMUDGE / LIQUIFY */
  Paint    /* COMP_TOOL_STROKE，erase 字段区分画笔与橡皮擦 */
};

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

/* ------------------------------------------------------------ 渐变工具（S9） */

/* 渐变类型（镜像 ABI comp_tool_gradient.type；数值必须与 ABI 一致）。 */
inline constexpr int kGradientLinear = 0; /* 线性：沿两端点连线 */
inline constexpr int kGradientRadial = 1; /* 径向：以起点为圆心、两端点距离为半径 */
inline constexpr int kGradientTypeCount = 2;

inline const char* gradient_type_name(int type) {
  switch (type) {
    case kGradientLinear: return "Linear";
    case kGradientRadial: return "Radial";
    default: return "?";
  }
}

/* G 键：已在该工具时在线性/径向之间切换；未知值收敛回线性，不产生第三态。 */
inline int next_gradient_type(int type) {
  return type == kGradientLinear ? kGradientRadial : kGradientLinear;
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
 * 文字工具只产出 **ASCII**：本仓自带 5×7 点阵字体，锚定「零依赖、可交叉编译」，
 * 没有系统字体可借；UTF-8/CJK 由引擎参数闸如实拒绝（不画方框顶替）。
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
 * WM_KEYDOWN 的虚拟键码 -> 可键入 ASCII（0 = 该键不是文字输入）。
 *
 * 布局口径是 **US 键盘**：Win32 的 WM_KEYDOWN 给的是虚拟键码，不含字符本身；
 * 外壳不调用 ToUnicode（那会随输入法/键盘布局漂移，且中文输入法会产出
 * UTF-8，而字体只支持 ASCII），因此这里显式列出能产出的字符集。Shift 只
 * 影响同一键位上的第二个字符（与 US 布局一致）。
 *
 * 常量用数值写死以保持 ui_core.h 不 include <windows.h>：
 * 0x20=Space、0x30..0x39=0..9、0x41..0x5A=A..Z、0xBA..=OEM 标点（US 布局）。
 */
inline char type_ascii_from_vk(int vk, bool shift) {
  if (vk == 0x20) return ' '; /* VK_SPACE */
  if (vk >= '0' && vk <= '9') {
    static const char kShiftedDigits[] = ")!@#$%^&*(";
    return shift ? kShiftedDigits[vk - '0'] : static_cast<char>(vk);
  }
  if (vk >= 'A' && vk <= 'Z') {
    return shift ? static_cast<char>(vk) : static_cast<char>(vk - 'A' + 'a');
  }
  switch (vk) {
    case 0xBA: return shift ? ':' : ';';  /* VK_OEM_1 */
    case 0xBB: return shift ? '+' : '=';  /* VK_OEM_PLUS */
    case 0xBC: return shift ? '<' : ',';  /* VK_OEM_COMMA */
    case 0xBD: return shift ? '_' : '-';  /* VK_OEM_MINUS */
    case 0xBE: return shift ? '>' : '.';  /* VK_OEM_PERIOD */
    case 0xBF: return shift ? '?' : '/';  /* VK_OEM_2 */
    case 0xC0: return shift ? '~' : '`';  /* VK_OEM_3 */
    case 0xDB: return shift ? '{' : '[';  /* VK_OEM_4 */
    case 0xDC: return shift ? '|' : '\\'; /* VK_OEM_5 */
    case 0xDD: return shift ? '}' : ']';  /* VK_OEM_6 */
    case 0xDE: return shift ? '"' : '\''; /* VK_OEM_7 */
    default: return 0;                    /* 控制键 / 功能键：不是文字输入 */
  }
}

/* 插入点光标在屏幕上的高度（文档字号 × 缩放，至少 1 px 可见）。 */
inline double type_caret_height(double font_size, double zoom) {
  const double size = clamp_type_font_size(font_size);
  const double height = size * (zoom > 0.0 ? zoom : 0.0);
  return height < 1.0 ? 1.0 : height;
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

}  // namespace comp_ui

#endif /* COMPOSITOR_SHELL_UI_CORE_H */
