// AI生成
/*
 * main.cpp — Windows 外壳层（M3）最小可运行骨架。
 *
 * 对应七层统一架构中的职责边界：
 *   平台外壳层    : 本文件负责窗口、消息循环、菜单/快捷键、图层面板与画布上屏（GDI blit）；
 *   交互与命令层  : 键盘/菜单映射为 C ABI 调用（新建/打开/保存、撤销/重做、图层与调整、
 *                   命令枚举），不触碰引擎内部；
 *   引擎核 C ABI  : 唯一集成边界（compositor/comp_*.h），壳不包含任何像素处理逻辑。
 *
 * 设计约束：
 *   1. 外壳不得 #include 任何 engine/ 内部头（domain/ compositing/ render/），
 *      否则七层架构的「ABI 边界」立即失效；本文件只依赖 abi/include。
 *   2. 所有合成由引擎完成；本机无可用 GPU，comp_engine_desc.backend = AUTO 会回落 CPU。
 *   3. 画布像素为预乘 RGBA8，GDI 需要 BGRA 顺序，上屏前做一次 R/B 交换。
 *   4. 撤销语义完全由引擎承担（文档级撤销栈，comp_undo.h）；
 *      外壳只做「触发 + 展示深度」，不自己维护历史。
 *   5. 图层属性一律「查询→修改」走 ABI（含可见性：comp_layer_get_visible）；
 *      壳侧仍保留一份 visible 缓存，仅作 ABI 查询失败时的兜底。
 */
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <compositor/comp_command.h>
#include <compositor/comp_core.h>
#include <compositor/comp_engine.h>
#include <compositor/comp_eyedropper.h>
#include <compositor/comp_filter.h>
#include <compositor/comp_io.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_render.h>
#include <compositor/comp_selection.h>
#include <compositor/comp_undo.h>

#include "ui_core.h" /* 平台无关的视图变换 / 布局 / 命中 / 工具表 */

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;

/* 蚂蚁线：虚线 4 px / 间隔 4 px，每 120 ms 前进 1 px（一个周期 8 px）。 */
constexpr UINT_PTR kAntsTimerId = 1;
constexpr UINT kAntsTimerPeriodMs = 120;
constexpr int kAntsDashPx = 4;
constexpr int kAntsGapPx = 4;
constexpr int kAntsPhasePeriod = kAntsDashPx + kAntsGapPx;
/* 边界线段缓冲上限：常规选区的量级是几十条；超过即截断并如实报到状态栏。 */
constexpr int kMaxAntSegments = 20000;

/* 面板几何统一由 ui_core.h 提供：绘制与命中测试必须共用同一套常量。 */
using comp_ui::Layout;

/* 调整面板的行高（仅是排版密度，不参与命中测试）。 */
constexpr int kAdjustRowHeight = 20;

/* 菜单命令 id（与快捷键一一对应，见 run_shell_command）。 */
constexpr UINT kCmdNew = 1001;
constexpr UINT kCmdOpen = 1002;
constexpr UINT kCmdSave = 1003;
constexpr UINT kCmdExit = 1004;
constexpr UINT kCmdUndo = 1005;
constexpr UINT kCmdRedo = 1006;
constexpr UINT kCmdListCommands = 1007;
constexpr UINT kCmdLayerAdd = 1008;
constexpr UINT kCmdLayerSelectUp = 1009;
constexpr UINT kCmdLayerSelectDown = 1010;
constexpr UINT kCmdLayerToggleVisible = 1011;
constexpr UINT kCmdLayerOpacityDown = 1012;
constexpr UINT kCmdLayerOpacityUp = 1013;
constexpr UINT kCmdAdjKindPrev = 1014;
constexpr UINT kCmdAdjKindNext = 1015;
constexpr UINT kCmdAdjRowPrev = 1016;
constexpr UINT kCmdAdjRowNext = 1017;
constexpr UINT kCmdAdjParamPrev = 1018;
constexpr UINT kCmdAdjParamNext = 1019;
constexpr UINT kCmdAdjValueDown = 1020;
constexpr UINT kCmdAdjValueUp = 1021;
constexpr UINT kCmdAdjAdd = 1022;
constexpr UINT kCmdAdjApply = 1023;
constexpr UINT kCmdAdjRemove = 1024;
constexpr UINT kCmdLayerMergeDown = 1025;
constexpr UINT kCmdLayerFlatten = 1026;
constexpr UINT kCmdViewFit = 1027;
constexpr UINT kCmdViewActual = 1028;
constexpr UINT kCmdViewZoomIn = 1029;
constexpr UINT kCmdViewZoomOut = 1030;
constexpr UINT kCmdSelectAll = 1031;
constexpr UINT kCmdSelectNone = 1032;
constexpr UINT kCmdSelectInvert = 1033;
constexpr UINT kCmdToolBase = 1100; /* + 工具序号，范围 1100..1116 */
constexpr UINT kCmdFilterBase = 1200; /* + 滤镜序号 0..3 */
constexpr UINT kCmdFilterApply = 1210;
constexpr UINT kCmdFilterReset = 1211;
constexpr UINT kCmdFilterClose = 1212;

struct LayerRow {
  comp_layer_id id = 0;
  float opacity = 1.0f;
  int32_t blend = 0;   /* comp_blend_mode 数值，0..23（ABI 冻结表） */
  uint8_t visible = 1; /* 壳侧自持：ABI 无可见性 getter */
};

struct ShellState {
  comp_engine engine = nullptr;
  comp_document document = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint32_t> framebuffer;   /* BGRA，供 GDI 直接使用 */
  std::vector<uint8_t> rgba;           /* 引擎输出的预乘 RGBA8 */
  bool needs_render = true;
  std::wstring status;
  /* 图层面板状态 */
  std::vector<LayerRow> layers;
  std::vector<std::pair<comp_layer_id, uint8_t>> visibility; /* id -> visible */
  int32_t selected_layer = 0; /* 面板行索引（0 = 最上层显示行） */
  /* 调整面板状态：只经 C ABI 元数据/写入接口工作，壳内不解析也不拼装 JSON。 */
  std::vector<uint32_t> adj_kinds;       /* 选中图层已有调整的 kind（数组序） */
  std::vector<std::string> adj_settings; /* 与 adj_kinds 对齐的 settings JSON */
  int32_t selected_adjustment = -1;      /* 已有调整的选中索引；-1 = 未选（新增态） */
  int32_t adj_kind = 3;                  /* 当前编辑 kind，默认 3 = Exposure */
  int32_t adj_param = 0;                 /* 聚焦的可编辑参数序号 */
  std::vector<std::string> adj_values;   /* 与参数表对齐的 ASCII 编辑值 */
  std::wstring adj_input;                /* 键入缓冲；非空 = 输入态 */
  /* 画布视图与工具状态：几何/变换全部来自 ui_core.h，壳内不重复实现。 */
  comp_ui::CanvasView view;
  comp_ui::ToolId tool = comp_ui::ToolId::Select;
  double brush_size = 24.0;
  double brush_hardness = 0.75;
  /* Retouch 工具（R 循环）：0 = 模糊（走 COMP_TOOL_BLUR）/ 1 涂抹 / 2 液化。 */
  int retouch_mode = comp_ui::kRetouchBlur;
  double blur_radius = 8.0;
  double retouch_strength = 0.5; /* Smudge / Liquify 的 warp.strength */
  /* 滤镜面板：状态（种类/焦点/值）全在 ui_core.h，壳只做绘制与 ABI 搬运。 */
  comp_ui::FilterPanel filter;
  bool filter_open = false;
  std::wstring filter_input; /* 滤镜面板的键入缓冲；非空 = 输入态 */
  /* 前景色（S6）：画笔的笔画色；吸管与色板都写这里。 */
  comp_ui::ColorRGB foreground{};
  int32_t palette_index = 0; /* X 键循环到色板的哪一项 */
  std::wstring color_input;  /* Shift+X 的十六进制键入；非空 = 输入态 */
  /*
   * 进行中的画布笔迹（文档像素折线）。Retouch 族（Blur / Smudge / Liquify）与
   * 画笔/橡皮擦共用同一条折线通路与去抖规则，stroke_kind 记住松开时装配哪种算子；
   * stroke_erase 在落笔时冻结——拖拽中途按 E 不改变这一笔的合成方向。
   */
  comp_ui::StrokeKind stroke_kind = comp_ui::StrokeKind::None;
  std::vector<float> stroke_points;
  bool stroke_erase = false;
  /* 选区搬移（Move 工具，S6）：按下记起点，松开一次性提交（Alt = 复制）。 */
  bool move_active = false;
  comp_ui::Point move_start{};
  comp_ui::Point move_current{};
  bool move_duplicate = false;
  /* 吸管拖拽（S6）：按住拖动时连续取样；只读，不产生撤销步。 */
  bool eyedropper_drag = false;
  /*
   * 选区（comp_selection.h 的 F1 ABI）。mask 是文档分辨率的有效 coverage；
   * 蚂蚁线的线段由 ui_core 从 mask 现算，壳内不维护「选区形状」的第二种表示。
   */
  bool selection_active = false;
  std::vector<uint8_t> selection_mask;
  std::vector<comp_ui::BoundarySegment> selection_segments;
  int32_t selection_phase = 0;    /* 蚂蚁线相位 0..7 px */
  bool selection_truncated = false; /* 线段数超过缓冲上限（如实提示） */
  /* 框选（Marquee 工具）进行中的矩形，文档坐标。 */
  bool marquee_active = false;
  comp_ui::Point marquee_start{};
  comp_ui::Point marquee_current{};
  /* Marquee 形状（S7）：M 键在矩形 / 椭圆之间循环。 */
  int marquee_shape = comp_ui::kMarqueeRect;
  /* 套索（S7）：自由手绘折线（文档像素 x,y 交替），松开闭合成多边形。 */
  bool lasso_active = false;
  std::vector<float> lasso_points;
  /* 魔棒（S7）：容差、取样来源与连通性；Shift+W 切「合并/当前图层」。 */
  int wand_tolerance = comp_ui::kDefaultWandTolerance;
  bool wand_contiguous = true;
  bool wand_merged = false;
  /* 魔棒取样窗口半径（S8）：Alt+[ / Alt+] 调，0 = 只取种子像素（S7 行为）。 */
  int wand_radius = comp_ui::kDefaultWandRadius;
  /*
   * 渐变工具（S9）：G 切工具，已在该工具时再按 G 在线性/径向之间循环。
   * 拖拽的两个端点（文档像素）在松开时一次性提交 comp_tool_apply(COMP_TOOL_GRADIENT)；
   * 色标固定为「前景色 -> 透明」（Photoshop 的 Foreground to Transparent 预设），
   * 按住 Shift 反向（透明 -> 前景色）。
   */
  int gradient_type = comp_ui::kGradientLinear;
  bool gradient_active = false;
  comp_ui::Point gradient_start{};
  comp_ui::Point gradient_current{};
  /*
   * 形状工具（S10）：U 切工具，已在该工具时再按 U 在矩形/椭圆/直线之间循环。
   * 拖拽的对角点/两端点（文档像素）在松开时一次性提交 comp_tool_apply(COMP_TOOL_SHAPE)；
   * 矩形/椭圆填充前景色，直线用前景色描边（宽度 = shape_stroke_width）。
   */
  int shape_kind = comp_ui::kShapeRect;
  bool shape_active = false;
  comp_ui::Point shape_start{};
  comp_ui::Point shape_current{};
  double shape_stroke_width = comp_ui::kDefaultShapeStrokeWidth;
  double shape_corner_radius = comp_ui::kDefaultShapeCornerRadius;
  /*
   * 文字工具（S11）：T 切工具；在画布上点一下落插入点，随后直接键入 ASCII，
   * 回车提交一次 comp_tool_apply(COMP_TOOL_TYPE)，Esc / 失焦 / 切工具取消
   * （只丢预览，不产生撤销步）。字体是引擎内的 5×7 点阵，没有系统字体，
   * 因此只支持 ASCII；CJK 由引擎参数闸如实拒绝（不画方框顶替）。
   */
  bool type_active = false; /* 已落插入点，正在键入/等待提交 */
  comp_ui::Point type_origin{}; /* 插入点（文档像素，文本框左上角） */
  std::string type_text;        /* ASCII 键入缓冲，'\n' = 换行（Shift+Enter） */
  double type_font_size = comp_ui::kDefaultTypeFontSize;
  double type_line_height = comp_ui::kDefaultTypeLineHeight;
  double type_letter_spacing = 0.0;
  /* 新选区羽化半径（Shift+F 键入）：形状/套索/魔棒提交时同事务应用。 */
  int selection_feather = comp_ui::kMinSelectionFeather;
  std::wstring feather_input; /* 羽化键入缓冲；非空 = 输入态 */
  /*
   * 钢笔路径（S8）：锚点与控制柄都在文档坐标；pen_dragging = 按下后正在拖
   * 最后一个锚点的控制柄。路径本身不产生撤销步，Enter/点首锚点才提交选区。
   */
  std::vector<comp_ui::PenAnchor> pen_anchors;
  bool pen_dragging = false;
  /* 鼠标是否停在画布上（决定画不画笔尖轮廓），客户区坐标。 */
  bool cursor_valid = false;
  bool tracking_leave = false;
  int cursor_x = 0;
  int cursor_y = 0;
  bool space_down = false; /* 按住空格 = 临时抓手 */
  bool panning = false;
  POINT pan_anchor{};
  bool needs_fit = true; /* 新文档/打开文档后，下一次绘制时适配窗口 */
  HWND window = nullptr;
};

ShellState* g_shell = nullptr;  /* 单窗口外壳，生命周期与窗口一致 */

void set_status(ShellState& shell, const wchar_t* text) {
  shell.status = text;
  if (shell.document != nullptr) {
    const char* engine_error = comp_engine_last_error(shell.engine);
    if (engine_error != nullptr && engine_error[0] != '\0') {
      /* 引擎侧错误一并带上，避免壳层把失败静默掉。 */
      const std::string narrow(engine_error);
      shell.status += L" | ";
      shell.status.append(narrow.begin(), narrow.end());
    }
  }
}

/*
 * 撤销/重做后刷新状态栏：深度、内存占用、是否截断统统直接问引擎，
 * 壳内不记账（P1-3：撤销栈预算信号必须透到用户可见处）。
 * 注意：wsprintfW 不支持 %f，内存按 KiB 用整数输出。
 */
void refresh_history_status(ShellState& shell, const wchar_t* prefix) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  comp_document_history_stats(shell.document, &undo_depth, &redo_depth);

  size_t history_bytes = 0;
  if (comp_document_history_bytes(shell.document, &history_bytes) != COMP_OK) history_bytes = 0;
  int32_t truncated = 0;
  if (comp_document_history_truncated(shell.document, &truncated) != COMP_OK) truncated = 0;
  const unsigned kib = static_cast<unsigned>(history_bytes / 1024ull);

  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（撤销 %u / 重做 %u · 历史 %u KiB%s）", prefix,
            static_cast<unsigned>(undo_depth), static_cast<unsigned>(redo_depth), kib,
            truncated != 0 ? L"，已截断" : L"");
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 图层面板 */

uint8_t visible_of(ShellState& shell, comp_layer_id id) {
  for (const auto& entry : shell.visibility) {
    if (entry.first == id) return entry.second;
  }
  shell.visibility.emplace_back(id, static_cast<uint8_t>(1));
  return 1;
}

void set_visible_cache(ShellState& shell, comp_layer_id id, uint8_t value) {
  for (auto& entry : shell.visibility) {
    if (entry.first == id) {
      entry.second = value;
      return;
    }
  }
  shell.visibility.emplace_back(id, value);
}

/* 调整面板在下方定义；refresh_layers 需要它同步选中图层的调整列表。 */
void refresh_adjustments(ShellState& shell);

/* 两段式枚举图层 id，并逐个查询属性（只读，不引入隐式事务）。 */
void refresh_layers(ShellState& shell) {
  shell.layers.clear();
  if (shell.document == nullptr) return;

  size_t count = 0;
  if (comp_layer_count(shell.document, &count) != COMP_OK || count == 0) {
    shell.selected_layer = 0;
    return;
  }
  std::vector<comp_layer_id> ids(count, 0);
  size_t written = 0;
  if (comp_layer_list(shell.document, ids.data(), ids.size(), &written) != COMP_OK) return;
  if (written < ids.size()) ids.resize(written);

  for (comp_layer_id id : ids) {
    LayerRow row;
    row.id = id;
    float opacity = 1.0f;
    if (comp_layer_get_opacity(shell.document, id, &opacity) == COMP_OK) {
      row.opacity = opacity;
    }
    comp_blend_mode blend = static_cast<comp_blend_mode>(0);
    if (comp_layer_get_blend(shell.document, id, &blend) == COMP_OK) {
      row.blend = static_cast<int32_t>(blend);
    }
    int32_t visible = 0;
    if (comp_layer_get_visible(shell.document, id, &visible) == COMP_OK) {
      row.visible = visible != 0 ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      set_visible_cache(shell, id, row.visible);
    } else {
      /* 兜底：仅在 ABI 查询失败时沿用壳侧缓存。 */
      row.visible = visible_of(shell, id);
    }
    shell.layers.push_back(row);
  }

  if (shell.selected_layer < 0) shell.selected_layer = 0;
  if (shell.selected_layer >= static_cast<int32_t>(shell.layers.size())) {
    shell.selected_layer = static_cast<int32_t>(shell.layers.size()) - 1;
  }
  /* 选中图层变化后，调整面板必须跟着换数据源。 */
  refresh_adjustments(shell);
}

/* 面板显示序：数组序是自下而上，UI 需要自上而下（顶层在第一行）。 */
const LayerRow* selected_row(const ShellState& shell) {
  if (shell.layers.empty()) return nullptr;
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  if (shell.selected_layer < 0 || shell.selected_layer >= count) return nullptr;
  return &shell.layers[static_cast<size_t>(count - 1 - shell.selected_layer)];
}

void panel_move_selection(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.layers.size());
  if (count == 0) {
    set_status(shell, L"无图层");
    return;
  }
  int32_t next = shell.selected_layer + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.selected_layer = next;
  const LayerRow* row = selected_row(shell);
  if (row != nullptr) {
    wchar_t buffer[128];
    wsprintfW(buffer, L"已选中图层 #%I64u（混合#%d %d%%）",
              static_cast<unsigned long long>(row->id), row->blend,
              static_cast<int>(row->opacity * 100.0f + 0.5f));
    set_status(shell, buffer);
  }
}

void panel_toggle_visible(ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  const comp_layer_id id = row->id;
  const uint8_t next = row->visible != 0 ? static_cast<uint8_t>(0) : static_cast<uint8_t>(1);
  if (comp_layer_set_visible(shell.document, id, next) != COMP_OK) {
    set_status(shell, L"切换可见性失败");
    return;
  }
  set_visible_cache(shell, id, next);
  refresh_layers(shell);
  shell.needs_render = true;
  refresh_history_status(shell, next != 0 ? L"已显示图层" : L"已隐藏图层");
}

void panel_adjust_opacity(ShellState& shell, float delta) {
  const LayerRow* row = selected_row(shell);
  if (row == nullptr) {
    set_status(shell, L"无图层");
    return;
  }
  float next = row->opacity + delta;
  if (next < 0.0f) next = 0.0f;
  if (next > 1.0f) next = 1.0f;
  const comp_layer_id id = row->id;
  if (comp_layer_set_opacity(shell.document, id, next) != COMP_OK) {
    set_status(shell, L"设置不透明度失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  wchar_t buffer[96];
  wsprintfW(buffer, L"不透明度 %d%%", static_cast<int>(next * 100.0f + 0.5f));
  refresh_history_status(shell, buffer);
}

void panel_add_layer(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_layer_desc desc{};
  desc.name = nullptr; /* 交给引擎自动命名 */
  desc.blend = static_cast<comp_blend_mode>(0); /* 0 = Normal（冻结表） */
  desc.opacity = 1.0f;
  desc.is_group = 0;
  desc.parent = 0;
  desc.sampling = static_cast<comp_sampling>(0);
  comp_layer_id id = 0;
  if (comp_layer_add(shell.document, &desc, &id) != COMP_OK) {
    set_status(shell, L"新建图层失败");
    return;
  }
  set_visible_cache(shell, id, static_cast<uint8_t>(1));
  refresh_layers(shell);
  shell.needs_render = true;
  wchar_t buffer[96];
  wsprintfW(buffer, L"已新建图层 #%I64u", static_cast<unsigned long long>(id));
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 调整面板 */

comp_layer_id selected_layer_id(const ShellState& shell) {
  const LayerRow* row = selected_row(shell);
  return row != nullptr ? row->id : 0;
}

/*
 * 向下合并（Ctrl+E，对齐上游 ⌘E）：把选中图层与下方图层按「画布呈现」
 * 合并为一个像素层。失败原因（无可合并 / 上方穿透组不透明度非 100% /
 * 携带调整或效果）由 set_status 附带的引擎 last_error 如实透出。
 */
void panel_merge_down(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"无图层");
    return;
  }
  if (comp_layer_merge_down(shell.document, id) != COMP_OK) {
    set_status(shell, L"合并失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  refresh_history_status(shell, L"已向下合并");
}

/* 拼合图像（对齐上游 Flatten Image）：整篇文档合成为单一像素层。 */
void panel_flatten(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  if (comp_document_flatten(shell.document) != COMP_OK) {
    set_status(shell, L"拼合失败");
    return;
  }
  refresh_layers(shell);
  shell.needs_render = true;
  refresh_history_status(shell, L"已拼合图像");
}

/* 参数值在面板内用 ASCII 文本承载（避开 WideChar 转换噪声）。 */
std::string number_text(double value) {
  char buffer[32];
  if (std::fabs(value - std::floor(value)) < 1e-9) {
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%g", value);
  }
  return std::string(buffer);
}

std::wstring widen_ascii(const char* text) {
  std::wstring wide;
  if (text == nullptr) return wide;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
  }
  return wide;
}

/* 名称经 ABI 取自引擎键名表，外壳不复制任何键名。 */
const char* adjustment_kind_name(uint32_t kind) {
  const char* name = nullptr;
  if (comp_layer_adjustment_kind_name(kind, &name) != COMP_OK || name == nullptr) return "(unknown)";
  return name;
}

/* 重建编辑值：以缺省值为底；若选中了既有调整，则按 settings 回填（引擎侧解析 JSON）。 */
void reset_adjustment_values(ShellState& shell) {
  shell.adj_values.clear();
  shell.adj_param = 0;
  shell.adj_input.clear();

  size_t param_count = 0;
  if (comp_layer_adjustment_param_count(static_cast<uint32_t>(shell.adj_kind), &param_count) != COMP_OK) {
    param_count = 0;
  }
  const std::string* existing = nullptr;
  if (shell.selected_adjustment >= 0 &&
      shell.selected_adjustment < static_cast<int32_t>(shell.adj_settings.size())) {
    existing = &shell.adj_settings[static_cast<size_t>(shell.selected_adjustment)];
  }

  for (size_t i = 0; i < param_count; ++i) {
    comp_adjustment_param param{};
    if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), i, &param) != COMP_OK) {
      shell.adj_values.emplace_back();
      continue;
    }
    bool filled = false;
    std::string text;
    if (existing != nullptr) {
      if (param.type == COMP_PARAM_BOOL) {
        int32_t boolean = 0;
        if (comp_layer_adjustment_read_value(existing->c_str(), param.json_path, nullptr, &boolean) ==
            COMP_OK) {
          text = boolean != 0 ? "1" : "0";
          filled = true;
        }
      } else {
        double number = 0.0;
        if (comp_layer_adjustment_read_value(existing->c_str(), param.json_path, &number, nullptr) ==
            COMP_OK) {
          text = number_text(number);
          filled = true;
        }
      }
    }
    if (!filled) {
      text = param.type == COMP_PARAM_BOOL ? (param.default_value != 0.0 ? "1" : "0")
                                           : number_text(param.default_value);
    }
    shell.adj_values.push_back(text);
  }
}

/* 读取选中图层的调整列表（只读，不开事务），并把 kind 与编辑值同步过来。 */
void refresh_adjustments(ShellState& shell) {
  shell.adj_kinds.clear();
  shell.adj_settings.clear();

  const comp_layer_id id = shell.document != nullptr ? selected_layer_id(shell) : 0;
  if (id == 0) {
    shell.selected_adjustment = -1;
    reset_adjustment_values(shell);
    return;
  }
  size_t count = 0;
  if (comp_layer_adjustment_count(shell.document, id, &count) != COMP_OK) count = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t kind = 0;
    size_t need = 0;
    if (comp_layer_adjustment_at(shell.document, id, i, &kind, nullptr, 0, &need) != COMP_OK) break;
    std::vector<char> text(need + 1, '\0');
    if (comp_layer_adjustment_at(shell.document, id, i, &kind, text.data(), text.size(), &need) !=
        COMP_OK) {
      break;
    }
    shell.adj_kinds.push_back(kind);
    shell.adj_settings.emplace_back(text.data(), need);
  }
  if (shell.selected_adjustment >= static_cast<int32_t>(shell.adj_settings.size())) {
    shell.selected_adjustment = static_cast<int32_t>(shell.adj_settings.size()) - 1;
  }
  if (shell.selected_adjustment >= 0) {
    shell.adj_kind = static_cast<int32_t>(shell.adj_kinds[static_cast<size_t>(shell.selected_adjustment)]);
  }
  reset_adjustment_values(shell);
}

void panel_cycle_kind(ShellState& shell, int32_t delta) {
  const int32_t total = static_cast<int32_t>(COMP_ADJUSTMENT_KIND_COUNT);
  int32_t kind = shell.adj_kind + delta;
  if (kind < 0) kind = total - 1;
  if (kind >= total) kind = 0;
  shell.adj_kind = kind;
  shell.selected_adjustment = -1; /* 换类型即进入「新增」态 */
  reset_adjustment_values(shell);
  wchar_t buffer[160];
  wsprintfW(buffer, L"调整类型 %d/%d：%s", shell.adj_kind + 1, total,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(kind))).c_str());
  set_status(shell, buffer);
}

void panel_select_adjustment(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.adj_settings.size());
  if (count == 0) {
    set_status(shell, L"该图层没有调整（Ctrl+Shift+A 新增）");
    return;
  }
  int32_t next = shell.selected_adjustment + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.selected_adjustment = next;
  shell.adj_kind = static_cast<int32_t>(shell.adj_kinds[static_cast<size_t>(next)]);
  reset_adjustment_values(shell);
  wchar_t buffer[160];
  wsprintfW(buffer, L"已选中调整 #%d：%s", next + 1,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(shell.adj_kind))).c_str());
  set_status(shell, buffer);
}

void panel_move_param(ShellState& shell, int32_t delta) {
  const int32_t count = static_cast<int32_t>(shell.adj_values.size());
  if (count == 0) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  int32_t next = shell.adj_param + delta;
  if (next < 0) next = 0;
  if (next >= count) next = count - 1;
  shell.adj_param = next;
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), static_cast<size_t>(next),
                                     &param) != COMP_OK) {
    return;
  }
  wchar_t buffer[200];
  wsprintfW(buffer, L"参数 %d/%d：%s = %s", next + 1, count,
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.adj_values[static_cast<size_t>(next)].c_str()).c_str());
  set_status(shell, buffer);
  shell.adj_input.clear();
}

void panel_step_value(ShellState& shell, int32_t direction) {
  const size_t index = static_cast<size_t>(shell.adj_param);
  if (index >= shell.adj_values.size()) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), index, &param) != COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    shell.adj_values[index] = shell.adj_values[index] == "1" ? "0" : "1";
  } else {
    double value = std::atof(shell.adj_values[index].c_str());
    double step = (param.max_value - param.min_value) / 100.0;
    if (step <= 0.0) step = 1.0;
    value += static_cast<double>(direction) * step;
    if (value < param.min_value) value = param.min_value;
    if (value > param.max_value) value = param.max_value;
    shell.adj_values[index] = number_text(value);
  }
  shell.adj_input.clear();
  wchar_t buffer[220];
  wsprintfW(buffer, L"%s = %s （范围 [%s, %s]）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.adj_values[index].c_str()).c_str(),
            widen_ascii(number_text(param.min_value).c_str()).c_str(),
            widen_ascii(number_text(param.max_value).c_str()).c_str());
  set_status(shell, buffer);
}

/* 数字键开始输入：布尔参数不进入文本态，直接切换。 */
void panel_begin_input(ShellState& shell, wchar_t character) {
  const size_t index = static_cast<size_t>(shell.adj_param);
  if (index >= shell.adj_values.size()) {
    set_status(shell, L"当前调整没有可编辑参数");
    return;
  }
  comp_adjustment_param param{};
  if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), index, &param) != COMP_OK) {
    return;
  }
  if (param.type == COMP_PARAM_BOOL) {
    panel_step_value(shell, 1);
    return;
  }
  shell.adj_input.assign(1, character);
  set_status(shell, L"输入中：回车提交，Esc 取消");
}

void panel_backspace(ShellState& shell) {
  if (!shell.adj_input.empty()) shell.adj_input.pop_back();
}

void panel_commit_input(ShellState& shell) {
  if (shell.adj_input.empty()) {
    set_status(shell, L"未在输入状态（数字键开始输入）");
    return;
  }
  const size_t index = static_cast<size_t>(shell.adj_param);
  if (index >= shell.adj_values.size()) {
    shell.adj_input.clear();
    return;
  }
  const std::string text(shell.adj_input.begin(), shell.adj_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  shell.adj_values[index] = text;
  shell.adj_input.clear();
  wchar_t buffer[160];
  wsprintfW(buffer, L"已提交 %s", widen_ascii(text.c_str()).c_str());
  set_status(shell, buffer);
}

/* settings JSON 由引擎组装（壳内不拼 JSON、不解析 JSON）。 */
bool build_settings_json(const ShellState& shell, std::string* out_json) {
  std::vector<const char*> values;
  values.reserve(shell.adj_values.size());
  for (const std::string& text : shell.adj_values) values.push_back(text.c_str());

  const uint32_t kind = static_cast<uint32_t>(shell.adj_kind);
  const char* const* data = values.empty() ? nullptr : values.data();
  size_t need = 0;
  if (comp_layer_adjustment_build_settings(kind, data, values.size(), nullptr, 0, &need) != COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_layer_adjustment_build_settings(kind, data, values.size(), &json[0], json.size(), &need) !=
      COMP_OK) {
    return false;
  }
  json.resize(need);
  if (out_json != nullptr) *out_json = json;
  return true;
}

void panel_apply_adjustment(ShellState& shell, bool add) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  if (!add && shell.selected_adjustment < 0) {
    set_status(shell, L"未选中已有调整（, / . 选择；Ctrl+Shift+A 新增）");
    return;
  }
  std::string json;
  if (!build_settings_json(shell, &json)) {
    set_status(shell, add ? L"新增被拒：参数不合法" : L"应用被拒：参数不合法");
    return;
  }

  const uint32_t kind = static_cast<uint32_t>(shell.adj_kind);
  comp_result result = COMP_OK;
  if (add) {
    result = comp_layer_add_adjustment(shell.document, id, kind, json.c_str());
  } else {
    result = comp_layer_update_adjustment_at(
        shell.document, id, static_cast<size_t>(shell.selected_adjustment), kind, json.c_str());
  }

  shell.needs_render = true;
  if (result != COMP_OK && !add) shell.selected_adjustment = -1;
  refresh_adjustments(shell);
  if (add && !shell.adj_settings.empty()) {
    /* 新增成功后选中最后一条，便于继续微调。 */
    shell.selected_adjustment = static_cast<int32_t>(shell.adj_settings.size()) - 1;
    shell.adj_kind = static_cast<int32_t>(shell.adj_kinds[static_cast<size_t>(shell.selected_adjustment)]);
    reset_adjustment_values(shell);
  } else if (add) {
    reset_adjustment_values(shell);
  }

  wchar_t buffer[180];
  wsprintfW(buffer, L"%s：%s",
            result == COMP_OK ? (add ? L"已新增调整" : L"已应用调整")
                              : (add ? L"新增调整失败" : L"应用调整失败"),
            widen_ascii(adjustment_kind_name(kind)).c_str());
  refresh_history_status(shell, buffer);
}

void panel_remove_adjustment(ShellState& shell) {
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  if (shell.selected_adjustment < 0) {
    set_status(shell, L"未选中已有调整（, / . 选择）");
    return;
  }
  const comp_result result = comp_layer_remove_adjustment(
      shell.document, id, static_cast<size_t>(shell.selected_adjustment));
  shell.needs_render = true;
  refresh_adjustments(shell);

  wchar_t buffer[120];
  wsprintfW(buffer, result == COMP_OK ? L"已删除调整" : L"删除调整失败");
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 滤镜面板 */

/*
 * ui_core.h 出于「不 include 引擎头」的约束，只能自带一份滤镜枚举。
 * 两边数值必须一一对应；这里用编译期断言把漂移挡在构建阶段。
 */
static_assert(static_cast<int>(comp_ui::FilterId::Vignette) == COMP_FILTER_VIGNETTE,
              "FilterId::Vignette must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::TonalContrast) == COMP_FILTER_TONAL_CONTRAST,
              "FilterId::TonalContrast must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::Bloom) == COMP_FILTER_BLOOM,
              "FilterId::Bloom must match comp_filter_kind");
static_assert(static_cast<int>(comp_ui::FilterId::LensCorrection) == COMP_FILTER_LENS_CORRECTION,
              "FilterId::LensCorrection must match comp_filter_kind");

std::wstring param_status_text(const comp_ui::FilterPanel& panel) {
  const comp_ui::FilterParamSpec* spec = panel.param_at(panel.focused);
  if (spec == nullptr) return std::wstring();
  return widen_ascii(spec->label) + L" = " +
         widen_ascii(number_text(panel.value(panel.focused)).c_str());
}

void open_filter_panel(ShellState& shell, comp_ui::FilterId id) {
  shell.filter.select(id);
  shell.filter_open = true;
  shell.filter_input.clear();
  const comp_ui::FilterSpec* spec = shell.filter.spec();
  wchar_t buffer[192];
  wsprintfW(buffer, L"滤镜：%s（Tab 选参数 · -/= 调值 · 数字键入 · 回车应用 · Esc 关闭）",
            widen_ascii(spec != nullptr ? spec->name : "?").c_str());
  set_status(shell, buffer);
}

void close_filter_panel(ShellState& shell) {
  shell.filter_open = false;
  shell.filter_input.clear();
  set_status(shell, L"已关闭滤镜面板");
}

void filter_reset_values(ShellState& shell) {
  shell.filter_input.clear();
  shell.filter.reset_values();
  set_status(shell, L"滤镜参数已重置为默认值");
}

void filter_focus_row(ShellState& shell, int delta) {
  if (!shell.filter_open) return;
  shell.filter.focus(delta);
  const std::wstring text = param_status_text(shell.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"参数 %d/%d：%s", shell.filter.focused + 1, shell.filter.param_count(),
            text.c_str());
  set_status(shell, buffer);
}

void filter_step_value(ShellState& shell, int direction) {
  if (!shell.filter_open) return;
  const comp_ui::FilterParamSpec* spec = shell.filter.param_at(shell.filter.focused);
  if (spec == nullptr) return;
  shell.filter.step(direction);
  const std::wstring text = param_status_text(shell.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（步长 %s）", text.c_str(),
            widen_ascii(number_text(spec->step).c_str()).c_str());
  set_status(shell, buffer);
}

void filter_begin_input(ShellState& shell, wchar_t character) {
  if (!shell.filter_open) return;
  shell.filter_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.filter_input.c_str());
  set_status(shell, buffer);
}

void filter_backspace(ShellState& shell) {
  if (shell.filter_input.empty()) return;
  shell.filter_input.pop_back();
}

/* 键入值先夹到面板量程再落值：面板永不产生越界参数（ABI 仍会拒绝越界）。 */
void filter_commit_input(ShellState& shell) {
  if (shell.filter_input.empty()) return;
  const std::string text(shell.filter_input.begin(), shell.filter_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  shell.filter.set_value(shell.filter.focused, value);
  shell.filter_input.clear();
  const std::wstring shown = param_status_text(shell.filter);
  wchar_t buffer[192];
  wsprintfW(buffer, L"已设置 %s", shown.c_str());
  set_status(shell, buffer);
}

/* 面板值按固定行序搬进 ABI 结构（唯一映射点；顺序见 ui_core 的 filter_table）。 */
void build_filter_desc(const comp_ui::FilterPanel& panel, comp_filter_desc* out) {
  comp_filter_desc desc{};
  desc.kind = static_cast<comp_filter_kind>(static_cast<int>(panel.id));
  switch (panel.id) {
    case comp_ui::FilterId::Vignette:
      desc.vignette.amount = panel.value(0);
      desc.vignette.midpoint = panel.value(1);
      desc.vignette.roundness = panel.value(2);
      desc.vignette.feather = panel.value(3);
      desc.vignette.highlights = panel.value(4);
      /* 面板 v1 不暴露边缘色：固定黑（与上游默认色一致），已在文档记边界。 */
      desc.vignette.color[0] = 0.0;
      desc.vignette.color[1] = 0.0;
      desc.vignette.color[2] = 0.0;
      break;
    case comp_ui::FilterId::TonalContrast:
      desc.tonal_contrast.amount = panel.value(0);
      desc.tonal_contrast.radius = panel.value(1);
      desc.tonal_contrast.shadows = panel.value(2);
      desc.tonal_contrast.midtones = panel.value(3);
      desc.tonal_contrast.highlights = panel.value(4);
      break;
    case comp_ui::FilterId::Bloom:
      desc.bloom.amount = panel.value(0);
      desc.bloom.radius = panel.value(1);
      break;
    case comp_ui::FilterId::LensCorrection:
      desc.lens_correction.distortion = panel.value(0);
      break;
    default:
      break;
  }
  *out = desc;
}

void apply_filter(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"请先用 ↑↓ 选择一个图层");
    return;
  }
  const comp_ui::FilterSpec* spec = shell.filter.spec();
  if (spec == nullptr) return;

  comp_filter_desc desc{};
  build_filter_desc(shell.filter, &desc);

  int32_t changed = 0;
  char note[512] = {0};
  size_t note_size = 0;
  /* 全分辨率渲染，scale = 1：高斯半径不缩放。 */
  const comp_result result = comp_filter_apply(shell.document, id, &desc, 1.0, &changed, note,
                                               sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[768];
  const std::wstring name = widen_ascii(spec->name);
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s 失败", name.c_str());
    set_status(shell, buffer);
    return;
  }
  if (changed == 0) {
    /* 恒等（例如 Amount 仍为 0）：引擎不碰像素、不开事务，这里如实回报。 */
    wsprintfW(buffer, L"%s：参数为恒等，未改动", name.c_str());
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"%s：已应用｜%s", name.c_str(), widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"%s：已应用", name.c_str());
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 选区 */

/*
 * 从引擎取完整选区状态：active、有效 coverage（含羽化）、边界线段。
 * 无选区时清空全部缓存——「上一次的蚂蚁线」绝不能留在屏幕上。
 *
 * coverage 是文档分辨率的 8 位灰度图；1500 万像素的文档约 15 MB 副本，
 * 只在选区/文档变化时取一次（不随每帧绘制重取）。
 */
void refresh_selection(ShellState& shell) {
  shell.selection_active = false;
  shell.selection_mask.clear();
  shell.selection_segments.clear();
  shell.selection_truncated = false;
  if (shell.document == nullptr || shell.width <= 0 || shell.height <= 0) return;

  int32_t active = 0;
  if (comp_selection_get_active(shell.document, &active) != COMP_OK || active == 0) return;

  const std::size_t pixel_count = static_cast<std::size_t>(shell.width) *
                                  static_cast<std::size_t>(shell.height);
  shell.selection_mask.assign(pixel_count, 0);
  comp_mask mask{};
  mask.width = shell.width;
  mask.height = shell.height;
  mask.stride = static_cast<std::size_t>(shell.width);
  mask.pixels = shell.selection_mask.data();
  if (comp_selection_get_coverage(shell.document, &mask) != COMP_OK) {
    shell.selection_mask.clear();
    return;
  }
  shell.selection_active = true;

  shell.selection_segments.resize(kMaxAntSegments);
  const int needed = comp_ui::selection_boundary_segments(
      shell.selection_mask.data(), shell.width, shell.height,
      static_cast<std::size_t>(shell.width), 128, shell.selection_segments.data(),
      kMaxAntSegments);
  shell.selection_truncated = needed > kMaxAntSegments;
  if (!shell.selection_truncated) {
    shell.selection_segments.resize(static_cast<std::size_t>(needed));
  }
}

/* 选区命令的共同尾部：刷新缓存 + 撤销深度 + 截断提示（如有）。 */
void finish_selection_command(ShellState& shell, const wchar_t* prefix) {
  refresh_selection(shell);
  wchar_t buffer[192];
  if (shell.selection_truncated) {
    wsprintfW(buffer, L"%s（选区边界过多，蚂蚁线截断显示）", prefix);
    refresh_history_status(shell, buffer);
  } else {
    refresh_history_status(shell, prefix);
  }
}

void select_all(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_select_all(shell.document, &changed) != COMP_OK) {
    set_status(shell, L"全选失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已全选（Ctrl+D 取消）" : L"已全选：选区未变化");
}

void deselect_all(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_deselect(shell.document, &changed) != COMP_OK) {
    set_status(shell, L"取消选择失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已取消选择" : L"取消选择：本来就没有选区");
}

void invert_selection(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t changed = 0;
  if (comp_selection_invert(shell.document, &changed) != COMP_OK) {
    set_status(shell, L"反选失败");
    return;
  }
  finish_selection_command(shell, changed ? L"已反选" : L"反选：无选区，未变化");
}

/* ------------------------------------------------------- 选区工具共同尾部（S7） */

/*
 * 形状 / 套索 / 魔棒三个入口的共同尾部：一次选区操作只落**一个**撤销步。
 *
 * 引擎的 apply_shape / apply_wand 各自会自成一个隐式事务，而「新选区 + 羽化」是
 * 两次 ABI 调用；因此这里开显式事务把它们并成一步（隐式事务会加入已开启的显式
 * 事务，不会嵌套）。选区没有变化时回滚，避免空撤销步。
 */
void finish_new_selection(ShellState& shell, const char* label, const wchar_t* ok_text,
                          const wchar_t* noop_text, const wchar_t* fail_text,
                          const std::function<comp_result(int32_t*)>& apply) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.document, label, &txn) != COMP_OK) {
    set_status(shell, L"选区：无法开始事务");
    return;
  }
  int32_t changed = 0;
  comp_result result = apply(&changed);
  /* 新选区羽化只作用于**本次真正新建**的选区：形状/路径/魔棒把 feather 归零后
   * 再用 comp_selection_set_feather 按绝对值设一次（S8 新增的覆盖语义入口）。
   * changed == 0 表示这次点击什么都没选中（例如魔棒没匹配），此时绝不能去动
   * 用户已有选区的羽化——那会把「点空处」变成一次羽化修改。 */
  if (result == COMP_OK && changed != 0 && shell.selection_feather > 0) {
    int32_t feathered = 0;
    result = comp_selection_set_feather(shell.document, shell.selection_feather, &feathered);
    if (result == COMP_OK && feathered != 0) changed = 1;
  }
  if (result != COMP_OK) {
    /* 失败必须整体回滚：半成品选区比不动更坏。 */
    comp_txn_abort(txn);
    set_status(shell, fail_text);
    return;
  }
  if (changed == 0) {
    comp_txn_abort(txn);
    set_status(shell, noop_text);
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"选区：提交失败");
    return;
  }
  finish_selection_command(shell, ok_text);
}

/* --------------------------------------------------------------- 羽化（S7） */

void refresh_feather_input_status(ShellState& shell) {
  wchar_t buffer[192];
  wsprintfW(buffer, L"新选区羽化：%s px（回车应用 / Esc 取消，0..%d）",
            shell.feather_input.c_str(), comp_ui::kMaxSelectionFeather);
  set_status(shell, buffer);
}

void begin_feather_input(ShellState& shell) {
  shell.feather_input.clear();
  wchar_t buffer[192];
  wsprintfW(buffer, L"新选区羽化：键入 0..%d 后回车（当前 %d px）",
            comp_ui::kMaxSelectionFeather, shell.selection_feather);
  set_status(shell, buffer);
}

/*
 * 回车：写进「新选区羽化」默认值。若已有选区，用 S8 的
 * comp_selection_set_feather 按**绝对值覆盖**当前选区的羽化（不是叠加）——
 * 与新选区提交时同一口径：键入几次都只会得到键入的那个半径，0 即清除羽化。
 */
void commit_feather_input(ShellState& shell) {
  const int value =
      comp_ui::clamp_selection_feather(_wtoi(shell.feather_input.c_str()));
  shell.feather_input.clear();
  shell.selection_feather = value;

  wchar_t buffer[224];
  if (shell.document == nullptr || !shell.selection_active) {
    wsprintfW(buffer, L"新选区羽化：%d px（对之后新建的选区生效）", value);
    set_status(shell, buffer);
    return;
  }
  int32_t changed = 0;
  if (comp_selection_set_feather(shell.document, value, &changed) != COMP_OK) {
    set_status(shell, L"羽化失败：当前选区不支持羽化");
    return;
  }
  refresh_selection(shell);
  if (changed == 0) {
    wsprintfW(buffer, L"当前选区羽化已是 %d px（无变化）", value);
    refresh_history_status(shell, buffer);
    return;
  }
  wsprintfW(buffer, L"已把当前选区羽化设为 %d px（绝对值）", value);
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 套索（S7） */

bool lasso_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Lasso;
}

void lasso_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!lasso_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.lasso_active = true;
  shell.lasso_points.clear();
  shell.lasso_points.push_back(static_cast<float>(doc.x));
  shell.lasso_points.push_back(static_cast<float>(doc.y));
  SetCapture(window);
  set_status(shell, L"套索：按住拖出自由选区，松开闭合成选区（Shift 加选 / Alt 减选）");
}

void lasso_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.lasso_active) return;
  const int point_count = static_cast<int>(shell.lasso_points.size() / 2);
  if (!comp_ui::lasso_can_append(point_count)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  const size_t count = shell.lasso_points.size();
  const double last_x = count >= 2 ? shell.lasso_points[count - 2] : doc.x;
  const double last_y = count >= 2 ? shell.lasso_points[count - 1] : doc.y;
  if (!comp_ui::stroke_point_should_append(last_x, last_y, doc.x, doc.y)) return;
  shell.lasso_points.push_back(static_cast<float>(doc.x));
  shell.lasso_points.push_back(static_cast<float>(doc.y));
}

void lasso_cancel(ShellState& shell) {
  shell.lasso_active = false;
  shell.lasso_points.clear();
}

void lasso_end(HWND window, ShellState& shell) {
  if (!shell.lasso_active) return;
  shell.lasso_active = false;
  ReleaseCapture();
  if (shell.document == nullptr) return;

  std::vector<float> points;
  points.swap(shell.lasso_points);
  const int point_count = static_cast<int>(points.size() / 2);
  if (!comp_ui::lasso_should_close(point_count)) {
    set_status(shell, L"套索：至少需要拖出 3 个点才能闭合成选区");
    return;
  }

  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }

  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"套索：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"套索：已减选"
                                                                : L"套索：已应用";
  finish_new_selection(
      shell, "Lasso Selection", ok_text, L"套索：选区未变化", L"套索失败：多边形无效",
      [&](int32_t* changed) {
        return comp_selection_apply_shape(shell.document, COMP_SELECTION_POLYGON, points.data(),
                                          point_count, combine, /*antialias=*/1, changed);
      });
}

/* ------------------------------------------------------------ 魔棒（S7） */

bool wand_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Wand;
}

void wand_click(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!wand_available(shell)) return;
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"魔棒：请先选择一个图层");
    return;
  }
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const comp_selection_wand_source source =
      shell.wand_merged ? COMP_SELECTION_WAND_MERGED : COMP_SELECTION_WAND_LAYER;

  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"魔棒：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"魔棒：已减选"
                                                                : L"魔棒：已应用";
  /* 图层带缩放/旋转/翻转时引擎拒绝 1:1 假设：提示切到合并取样，而不是静默失败。 */
  const wchar_t* fail_text = L"魔棒失败：参数无效或该图层非 1:1 变换（Shift+W 切合并取样）";
  finish_new_selection(shell, "Magic Wand Selection", ok_text, L"魔棒：没有匹配的像素，选区未变",
                       fail_text, [&](int32_t* changed) {
                         return comp_selection_apply_wand(
                             shell.document, id, source, doc.x, doc.y, shell.wand_tolerance,
                             shell.wand_contiguous ? 1 : 0, shell.wand_radius, combine,
                             changed);
                       });
}

/* ------------------------------------------------------------ 钢笔（S8） */

bool pen_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Pen;
}

/* 闭合命中半径：屏幕上约 6 px，换算成文档半径，缩放变化不影响手感。 */
double pen_close_radius(const ShellState& shell) {
  const double zoom = shell.view.zoom > 1e-6 ? shell.view.zoom : 1.0;
  return 6.0 / zoom;
}

void pen_refresh_status(ShellState& shell) {
  wchar_t buffer[256];
  wsprintfW(buffer,
            L"钢笔：%d 个锚点（点击继续 / 拖动拉控制柄 / 点首锚点闭合 / Enter 完成 / Esc 取消）",
            static_cast<int>(shell.pen_anchors.size()));
  set_status(shell, buffer);
}

void pen_cancel_drag(ShellState& shell) { shell.pen_dragging = false; }

void pen_cancel(ShellState& shell) {
  const bool had_path = !shell.pen_anchors.empty();
  shell.pen_anchors.clear();
  shell.pen_dragging = false;
  if (had_path) set_status(shell, L"钢笔：已取消当前路径");
}

/*
 * 提交路径：把锚点 + 控制柄装配成 comp_selection_cubic 段，走
 * comp_selection_apply_path（与套索/框选共用 finish_new_selection 的事务与
 * 羽化口径）。钢笔选区总是闭合子路径——这是选区语义，不是图形路径语义。
 */
void commit_pen_path(ShellState& shell) {
  if (shell.document == nullptr) return;
  const int anchor_count = static_cast<int>(shell.pen_anchors.size());
  if (anchor_count < 2) {
    set_status(shell, L"钢笔：至少需要 2 个锚点才能闭合路径");
    return;
  }

  const int segment_count = comp_ui::pen_segment_count(anchor_count, /*closed=*/true);
  std::vector<comp_ui::PenSegment> segments(static_cast<size_t>(segment_count));
  const int built = comp_ui::pen_build_segments(shell.pen_anchors.data(), anchor_count,
                                                /*closed=*/true, segments.data(),
                                                segment_count);
  if (built <= 0) {
    set_status(shell, L"钢笔失败：路径没有段");
    return;
  }
  const int path_count = std::min(built, segment_count);
  std::vector<comp_selection_cubic> cubics(static_cast<size_t>(path_count));
  for (int i = 0; i < path_count; ++i) {
    const comp_ui::PenSegment& source = segments[static_cast<size_t>(i)];
    comp_selection_cubic& target = cubics[static_cast<size_t>(i)];
    target.start[0] = static_cast<float>(source.start.x);
    target.start[1] = static_cast<float>(source.start.y);
    target.control1[0] = static_cast<float>(source.control1.x);
    target.control1[1] = static_cast<float>(source.control1.y);
    target.control2[0] = static_cast<float>(source.control2.x);
    target.control2[1] = static_cast<float>(source.control2.y);
    target.end[0] = static_cast<float>(source.end.x);
    target.end[1] = static_cast<float>(source.end.y);
    /* start 只在第一段（new_subpath = 1）有意义，其余段接着上一段的 end。 */
    target.new_subpath = i == 0 ? 1 : 0;
  }

  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"钢笔：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"钢笔：已减选"
                                                                : L"钢笔：已应用闭合选区";

  /* 先清状态再提交：路径已经变成选区参数，失败也不该留下半截预览。 */
  shell.pen_anchors.clear();
  shell.pen_dragging = false;
  ReleaseCapture();
  finish_new_selection(
      shell, "Pen Selection", ok_text, L"钢笔：选区未变化", L"钢笔失败：路径无效",
      [&](int32_t* changed) {
        return comp_selection_apply_path(shell.document, cubics.data(), path_count, combine,
                                         /*antialias=*/1, changed);
      });
}

void pen_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!pen_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  const int anchor_count = static_cast<int>(shell.pen_anchors.size());
  /* 点第一个锚点 = 闭合（首锚点至少要有 2 个锚点才有闭合路径可言）。 */
  if (comp_ui::pen_hits_first_anchor(shell.pen_anchors.data(), anchor_count, doc,
                                     pen_close_radius(shell))) {
    commit_pen_path(shell);
    return;
  }
  const size_t index = static_cast<size_t>(anchor_count);
  shell.pen_anchors.resize(index + 1u);
  int next = anchor_count;
  if (!comp_ui::pen_append_anchor(shell.pen_anchors.data(), &next, doc)) {
    shell.pen_anchors.resize(index);
    wchar_t buffer[160];
    wsprintfW(buffer, L"钢笔：锚点数已达上限 %d", comp_ui::kPenMaxAnchors);
    set_status(shell, buffer);
    return;
  }
  shell.pen_anchors.resize(static_cast<size_t>(next));
  /* 按住不放就进入「拖控制柄」状态；只点不拖时松开即得到直角锚点。 */
  shell.pen_dragging = true;
  SetCapture(window);
  pen_refresh_status(shell);
}

void pen_drag(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.pen_dragging || shell.pen_anchors.empty()) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  comp_ui::pen_drag_handle(shell.pen_anchors.data(),
                           static_cast<int>(shell.pen_anchors.size()), doc);
}

void pen_up(HWND window, ShellState& shell) {
  if (!shell.pen_dragging) return;
  shell.pen_dragging = false;
  ReleaseCapture();
  pen_refresh_status(shell);
}

/* ------------------------------------------------------------ 渐变（S9） */

/*
 * 渐变工具（COMP_TOOL_GRADIENT）：窗口路径是「拖出两个端点 -> 一次 comp_tool_apply」。
 *
 * 为什么不在拖拽过程中反复提交：渐变是一次性铺满整层的像素算子，中途每一步都提交
 * 会在撤销栈里留下几十步。预览只画方向线（draw_canvas_overlays），松开才算一次操作。
 *
 * 色标固定为「前景色 -> 透明」：外壳只有前景色模型（S6），第二色标没有 UI 可调；
 * 与其编一个宿主不存在的背景色，不如用上游同样存在的这个预设，并如实写进状态栏。
 * 选区活跃时引擎按 coverage 混合，因此「先套选区再拉渐变」只改选区内。
 */
bool gradient_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Gradient;
}

void gradient_cancel(ShellState& shell) { shell.gradient_active = false; }

void gradient_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!gradient_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.gradient_active = true;
  shell.gradient_start = doc;
  shell.gradient_current = doc;
  SetCapture(window);
  wchar_t buffer[192];
  wsprintfW(buffer, L"渐变 / %s：拖出方向与长度后松开（再按 G 切换类型；Shift 反向）",
            widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
  set_status(shell, buffer);
}

void gradient_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.gradient_active) return;
  shell.gradient_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void gradient_end(HWND window, ShellState& shell) {
  if (!shell.gradient_active) return;
  shell.gradient_active = false;
  ReleaseCapture();

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"渐变：请先选择一个图层");
    return;
  }
  const comp_ui::Point start = shell.gradient_start;
  const comp_ui::Point end = shell.gradient_current;
  if (!comp_ui::gradient_drag_is_usable(start.x, start.y, end.x, end.y)) {
    set_status(shell, L"渐变：拖拽太短，已放弃（方向/半径需要一个可定义的长度）");
    return;
  }

  /*
   * Shift = 反向：色标顺序翻转成「透明 -> 前景色」。这是拖拽结束后才读的
   * 修饰键（与框选/套索的 Shift=加选同口径：提交时才决定组合方式）。
   */
  const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_GRADIENT;
  desc.gradient.type = shell.gradient_type;
  desc.gradient.start_x = start.x;
  desc.gradient.start_y = start.y;
  desc.gradient.end_x = end.x;
  desc.gradient.end_y = end.y;
  const uint8_t solid[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 255};
  const uint8_t clear[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 0};
  const uint8_t* color0 = reverse ? clear : solid;
  const uint8_t* color1 = reverse ? solid : clear;
  for (int i = 0; i < 4; ++i) {
    desc.gradient.color0[i] = color0[i];
    desc.gradient.color1[i] = color1[i];
  }
  desc.gradient.opacity = 1.0f;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"渐变失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"渐变 / %s：已应用｜%s",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"渐变 / %s：已应用（%s）",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str(),
              reverse ? L"透明 → 前景" : L"前景 → 透明");
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 形状（S10） */

/*
 * 形状工具（COMP_TOOL_SHAPE）：窗口路径是「拖出对角点 / 两端点 -> 一次 comp_tool_apply」。
 *
 * 与渐变同理，不在拖拽过程中反复提交：形状是一次性铺像素的算子，中途每一步都提交
 * 会在撤销栈里留下几十步。预览只画几何轮廓（draw_canvas_overlays），松开才算一次操作。
 *
 * 外壳能产出的几何：矩形 / 椭圆（填充前景色）与直线（前景色描边，圆头端点）；
 * 通用贝塞尔路径只经 ABI——本壳的钢笔用来提交选区，不造形状。圆角半径只对矩形
 * 有效（0 = 直角），引擎再把半径夹到短边一半。Shift = 正方/正圆、Alt = 从中心拉，
 * 与上游 ShapeTool 的修饰键一致。
 */
bool shape_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Shape;
}

void shape_cancel(ShellState& shell) { shell.shape_active = false; }

void shape_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!shape_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.shape_active = true;
  shell.shape_start = doc;
  shell.shape_current = doc;
  SetCapture(window);
  wchar_t buffer[224];
  wsprintfW(buffer,
            L"形状 / %s：拖出%s后松开（Shift = 正方/正圆，Alt = 从中心；再按 U 切种类）",
            widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
            shell.shape_kind == comp_ui::kShapeLine ? L"两端点" : L"对角点");
  set_status(shell, buffer);
}

void shape_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.shape_active) return;
  shell.shape_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void shape_end(HWND window, ShellState& shell) {
  if (!shell.shape_active) return;
  shell.shape_active = false;
  ReleaseCapture();

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"形状：请先选择一个图层");
    return;
  }

  /* 修饰键在松开时才读：拖动中按下/松开 Shift、Alt 都能改落笔盒（与框选同口径）。 */
  const bool square = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const comp_ui::ShapeDragBox box = comp_ui::shape_drag_box(
      shell.shape_start.x, shell.shape_start.y, shell.shape_current.x, shell.shape_current.y,
      square, from_center);
  const int kind = shell.shape_kind;
  if (!comp_ui::shape_drag_is_usable(kind, box)) {
    set_status(shell, kind == comp_ui::kShapeLine
                          ? L"形状：拖拽太短，已放弃（直线需要两个不同端点）"
                          : L"形状：拖拽太短，已放弃（宽高都至少需要 1 px）");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_SHAPE;
  desc.shape.kind = static_cast<comp_shape_kind>(kind);
  desc.shape.x0 = box.x0;
  desc.shape.y0 = box.y0;
  desc.shape.x1 = box.x1;
  desc.shape.y1 = box.y1;
  desc.shape.corner_radius =
      kind == comp_ui::kShapeRect
          ? comp_ui::shape_effective_corner_radius(shell.shape_corner_radius, box)
          : 0.0;
  const bool line = kind == comp_ui::kShapeLine;
  const uint8_t color[4] = {shell.foreground.r, shell.foreground.g, shell.foreground.b, 255};
  for (int i = 0; i < 4; ++i) {
    desc.shape.style.fill_color[i] = color[i];
    desc.shape.style.stroke_color[i] = color[i];
  }
  /* 直线只有描边（引擎会拒绝「填充直线」）：矩形/椭圆的外壳默认是纯填充，与上游一致。 */
  desc.shape.style.fill_enabled = line ? 0 : 1;
  desc.shape.style.stroke_enabled = line ? 1 : 0;
  desc.shape.style.stroke_width = comp_ui::clamp_shape_stroke_width(shell.shape_stroke_width);
  desc.shape.style.opacity = 1.0f;
  desc.shape.style.antialias = 1;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"形状失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    /* 引擎的 note 会如实说明「被选区遮罩」「与图层不相交」这类非致命情况。 */
    wsprintfW(buffer, L"形状 / %s：已应用｜%s",
              widen_ascii(comp_ui::shape_kind_name(kind)).c_str(), widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"形状 / %s：已应用", widen_ascii(comp_ui::shape_kind_name(kind)).c_str());
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 文字（S11） */

/*
 * 文字工具（COMP_TOOL_TYPE）：窗口路径是「点击落插入点 -> 键入 ASCII -> 回车提交」。
 *
 * 键盘模态：落点后普通字符不再走工具快捷键，而是进 type_text；文本框、插入点光标
 * 与度量数字只是预览，像素在回车时由引擎一次性写出（与渐变/形状「不在拖拽中反复
 * 提交」同口径）。Esc / 失焦 / 切工具取消 = 丢弃缓冲，不产生撤销步。
 *
 * 为什么没有拖拽：上游文字工具也是「点一下定位、键入、提交」，拖动不产生额外几何；
 * 位置就取点击处的文档像素（向下/向右排字）。
 */
bool type_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Type;
}

void type_cancel(ShellState& shell) {
  shell.type_active = false;
  shell.type_text.clear();
}

/* 状态栏：空缓冲时给快捷键提示，有内容时给行数/文本框尺寸/排版值。 */
void type_refresh_status(ShellState& shell) {
  const comp_ui::TypeBlockMetrics metrics =
      comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                  shell.type_letter_spacing, shell.type_text.c_str());
  const int size_px = static_cast<int>(shell.type_font_size + 0.5);
  const int line_pct = static_cast<int>(shell.type_line_height * 100.0 + 0.5);
  const int spacing_px = static_cast<int>(std::lround(shell.type_letter_spacing));
  const int width_px = static_cast<int>(std::lround(metrics.width));
  const int height_px = static_cast<int>(std::lround(metrics.height));
  wchar_t buffer[384];
  if (shell.type_text.empty()) {
    wsprintfW(buffer,
              L"文字：插入点 (%d, %d)｜字号 %d px，行距 %d%%，字距 %d px｜"
              L"键入 ASCII，Shift+Enter 换行，回车提交，Esc 取消；"
              L"[ / ] 调字号，Shift+[ / ] 调字距",
              static_cast<int>(std::floor(shell.type_origin.x)),
              static_cast<int>(std::floor(shell.type_origin.y)), size_px, line_pct, spacing_px);
  } else {
    /* 状态栏是单行：把换行显示成 '|'，不把控制字符塞进窗口标题。 */
    std::string display;
    display.reserve(shell.type_text.size());
    for (char ch : shell.type_text) display.push_back(ch == '\n' ? '|' : ch);
    wsprintfW(buffer, L"文字：%s｜%d 行 %d×%d px，字号 %d，行距 %d%%，字距 %d｜"
                      L"回车提交，Esc 取消",
              widen_ascii(display.c_str()).c_str(), metrics.line_count, width_px, height_px,
              size_px, line_pct, spacing_px);
  }
  set_status(shell, buffer);
}

/* 重新落点前要先提交上一段文字（定义在下方，先声明）。 */
void type_commit(ShellState& shell);

void type_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!type_available(shell)) return;
  (void)window;
  /* 已经在键入时再点一次 = 先提交上一段文字，再在新位置重新落点（不丢已键入内容）。 */
  if (shell.type_active && !shell.type_text.empty()) type_commit(shell);
  shell.type_origin = shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.type_active = true;
  shell.type_text.clear();
  type_refresh_status(shell);
}

/*
 * 回车提交：一次 comp_tool_apply = 一步撤销。空缓冲不提交（没有可画的字形，
 * 引擎会以「无字形」拒绝），外壳保持插入点等待输入。
 */
void type_commit(ShellState& shell) {
  if (!shell.type_active) return;
  if (shell.type_text.empty()) {
    set_status(shell, L"文字：还没有输入内容（键入 ASCII 后回车提交，Esc 取消）");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"文字：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_TYPE;
  desc.type.origin_x = shell.type_origin.x;
  desc.type.origin_y = shell.type_origin.y;
  desc.type.text = shell.type_text.c_str();
  desc.type.text_length = static_cast<int32_t>(shell.type_text.size());
  desc.type.style.font_size = shell.type_font_size;
  desc.type.style.line_height = shell.type_line_height;
  desc.type.style.letter_spacing = shell.type_letter_spacing;
  desc.type.style.antialias = 1;
  desc.type.color[0] = shell.foreground.r;
  desc.type.color[1] = shell.foreground.g;
  desc.type.color[2] = shell.foreground.b;
  desc.type.color[3] = 255;
  desc.type.opacity = 1.0f;

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  /* 无论成功与否都退出键入态：成功已提交，失败也没有可保留的预览语义。 */
  const int lines = comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                                shell.type_letter_spacing,
                                                shell.type_text.c_str())
                        .line_count;
  shell.type_active = false;
  shell.type_text.clear();

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"文字失败（%d）", static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"文字：已应用（%d 行）｜%s", lines, widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"文字：已应用（%d 行）", lines);
  }
  refresh_history_status(shell, buffer);
}

/* [ / ] 调字号，Shift+[ / ] 调字距；只在文字工具内生效（其余工具沿用笔刷/魔棒）。 */
void type_step_param(ShellState& shell, int direction, bool spacing) {
  if (spacing) {
    shell.type_letter_spacing =
        comp_ui::step_type_letter_spacing(shell.type_letter_spacing, shell.type_font_size,
                                          direction);
  } else {
    shell.type_font_size = comp_ui::step_type_font_size(shell.type_font_size, direction);
    /* 字号变小会让原字距超出「步进 >= 0.5 px」的底限：跟着重新夹取。 */
    shell.type_letter_spacing =
        comp_ui::clamp_type_letter_spacing(shell.type_letter_spacing, shell.type_font_size);
  }
  if (shell.type_active) {
    type_refresh_status(shell);
  } else {
    wchar_t buffer[192];
    wsprintfW(buffer, L"文字：字号 %d px，行距 %d%%，字距 %d px（[ / ] 调字号，Shift+[ / ] 调字距）",
              static_cast<int>(shell.type_font_size + 0.5),
              static_cast<int>(shell.type_line_height * 100.0 + 0.5),
              static_cast<int>(std::lround(shell.type_letter_spacing)));
    set_status(shell, buffer);
  }
}

/* ------------------------------------------------------------ 框选（Marquee） */

bool marquee_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Marquee;
}

void marquee_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!marquee_available(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.marquee_active = true;
  shell.marquee_start = doc;
  shell.marquee_current = doc;
  SetCapture(window);
  set_status(shell, L"框选：拖出矩形后松开（Shift 加选 / Alt 减选）");
}

void marquee_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.marquee_active) return;
  shell.marquee_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void marquee_cancel(ShellState& shell) { shell.marquee_active = false; }

void marquee_end(HWND window, ShellState& shell) {
  if (!shell.marquee_active) return;
  shell.marquee_active = false;
  ReleaseCapture();
  if (shell.document == nullptr) return;

  /* 框选的两个角点（顺序任意，引擎按包围盒解释）：矩形与椭圆都是 2 点语义。 */
  const float points[4] = {static_cast<float>(shell.marquee_start.x),
                           static_cast<float>(shell.marquee_start.y),
                           static_cast<float>(shell.marquee_current.x),
                           static_cast<float>(shell.marquee_current.y)};
  comp_selection_combine combine = COMP_SELECTION_REPLACE;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    combine = COMP_SELECTION_ADD;
  } else if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    combine = COMP_SELECTION_SUBTRACT;
  }
  const comp_selection_shape shape =
      shell.marquee_shape == comp_ui::kMarqueeEllipse ? COMP_SELECTION_ELLIPSE
                                                      : COMP_SELECTION_RECT;
  const wchar_t* ok_text = combine == COMP_SELECTION_ADD ? L"框选：已加选"
                           : combine == COMP_SELECTION_SUBTRACT ? L"框选：已减选"
                                                                : L"框选：已应用";
  finish_new_selection(
      shell, shell.marquee_shape == comp_ui::kMarqueeEllipse ? "Ellipse Marquee Selection"
                                                             : "Rect Marquee Selection",
      ok_text, L"框选：选区未变化", L"框选失败", [&](int32_t* changed) {
        return comp_selection_apply_shape(shell.document, shape, points, 2, combine,
                                          /*antialias=*/1, changed);
      });
}

/* ------------------------------------------------------------ 画布落笔（折线） */

/*
 * Retouch 与画笔/橡皮擦共用同一条「鼠标折线 -> comp_tool_apply」通路：
 * 折线按文档像素累积，松开鼠标时一次性提交，中间不产生事务。
 */
bool stroke_in_progress(const ShellState& shell) {
  return shell.stroke_kind != comp_ui::StrokeKind::None;
}

bool stroke_begin(ShellState& shell, int x, int y, const Layout& layout,
                  comp_ui::StrokeKind kind) {
  if (shell.document == nullptr || kind == comp_ui::StrokeKind::None) return false;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.stroke_points.clear();
  /* 单击也要留下一记 dab：单点折线在 COMP_TOOL_STROKE 里就是一个圆形笔尖。 */
  shell.stroke_points.push_back(static_cast<float>(doc.x));
  shell.stroke_points.push_back(static_cast<float>(doc.y));
  shell.stroke_kind = kind;
  return true;
}

void stroke_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!stroke_in_progress(shell)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  const size_t count = shell.stroke_points.size();
  const double last_x = count >= 2 ? shell.stroke_points[count - 2] : doc.x;
  const double last_y = count >= 2 ? shell.stroke_points[count - 1] : doc.y;
  if (!comp_ui::stroke_point_should_append(last_x, last_y, doc.x, doc.y)) return;
  shell.stroke_points.push_back(static_cast<float>(doc.x));
  shell.stroke_points.push_back(static_cast<float>(doc.y));
}

/* 失焦 / 捕获丢失 / Esc：两种笔迹共用同一个取消动作。 */
void stroke_cancel(ShellState& shell) {
  shell.stroke_kind = comp_ui::StrokeKind::None;
  shell.stroke_points.clear();
  shell.stroke_erase = false;
}

/* ------------------------------------------------------------ Retouch 笔迹 */

/* Blur / Smudge / Liquify 共用同一条「鼠标折线 -> comp_tool_apply」通路。 */
bool retouch_stroke_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Retouch;
}

void retouch_stroke_begin(HWND window, ShellState& shell, int x, int y,
                          const Layout& layout) {
  if (!retouch_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Retouch)) return;
  SetCapture(window);
  wchar_t buffer[128];
  wsprintfW(buffer, L"%s 笔迹…（松开鼠标应用）",
            widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  set_status(shell, buffer);
}

void retouch_stroke_end(HWND window, ShellState& shell) {
  if (shell.stroke_kind != comp_ui::StrokeKind::Retouch) return;
  shell.stroke_kind = comp_ui::StrokeKind::None;
  ReleaseCapture();

  std::vector<float> points;
  points.swap(shell.stroke_points);
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, L"Retouch：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  switch (shell.retouch_mode) {
    case comp_ui::kRetouchBlur:
      desc.kind = COMP_TOOL_BLUR;
      desc.blur.radius = static_cast<float>(shell.blur_radius);
      break;
    case comp_ui::kRetouchSmudge:
      desc.kind = COMP_TOOL_SMUDGE;
      desc.warp.strength = static_cast<float>(shell.retouch_strength);
      break;
    case comp_ui::kRetouchLiquify:
      desc.kind = COMP_TOOL_LIQUIFY;
      desc.warp.strength = static_cast<float>(shell.retouch_strength);
      break;
    default:
      return;
  }
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 三个算子都不用随机数；固定值保证同参数同结果 */

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  const char* mode_name = comp_ui::retouch_mode_name(shell.retouch_mode);
  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s 失败", widen_ascii(mode_name).c_str());
    set_status(shell, buffer);
    return;
  }
  if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"%s：已应用 %d 个折点｜%s", widen_ascii(mode_name).c_str(),
              desc.stroke_point_count, widen_ascii(note).c_str());
  } else if (shell.retouch_mode == comp_ui::kRetouchBlur) {
    wsprintfW(buffer, L"%s：已应用 %d 个折点（半径 %d px）",
              widen_ascii(mode_name).c_str(), desc.stroke_point_count,
              static_cast<int>(shell.blur_radius + 0.5));
  } else {
    wsprintfW(buffer, L"%s：已应用 %d 个折点（强度 %d%%）",
              widen_ascii(mode_name).c_str(), desc.stroke_point_count,
              static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 画笔 / 橡皮擦 */

/*
 * 画笔与橡皮擦是同一条 COMP_TOOL_STROKE：形状与 coverage 相同，只有合成方向
 * 由 desc.erase 翻转（预乘 destination-out）。取色一律用 shell.foreground——
 * 吸管与色板都写同一个字段，因此「吸管吸到色 -> 直接画」不需要额外状态。
 */
bool paint_stroke_available(const ShellState& shell) {
  return shell.document != nullptr &&
         (shell.tool == comp_ui::ToolId::Brush || shell.tool == comp_ui::ToolId::Eraser);
}

void paint_stroke_begin(HWND window, ShellState& shell, int x, int y,
                        const Layout& layout) {
  if (!paint_stroke_available(shell)) return;
  if (!stroke_begin(shell, x, y, layout, comp_ui::StrokeKind::Paint)) return;
  shell.stroke_erase = shell.tool == comp_ui::ToolId::Eraser;
  SetCapture(window);
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s：拖动画布，松开%s（直径 %d px / 硬度 %d%%）",
            shell.stroke_erase ? L"橡皮擦" : L"画笔",
            shell.stroke_erase ? L"擦除" : L"落色",
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
  set_status(shell, buffer);
}

void paint_stroke_end(HWND window, ShellState& shell) {
  if (shell.stroke_kind != comp_ui::StrokeKind::Paint) return;
  const bool erasing = shell.stroke_erase;
  shell.stroke_kind = comp_ui::StrokeKind::None;
  shell.stroke_erase = false;
  ReleaseCapture();

  std::vector<float> points;
  points.swap(shell.stroke_points);
  if (points.empty()) return;

  const comp_layer_id id = selected_layer_id(shell);
  if (shell.document == nullptr || id == 0) {
    set_status(shell, erasing ? L"橡皮擦：请先选择一个图层" : L"画笔：请先选择一个图层");
    return;
  }

  comp_tool_op_desc desc{};
  desc.kind = COMP_TOOL_STROKE;
  desc.brush.color[0] = shell.foreground.r;
  desc.brush.color[1] = shell.foreground.g;
  desc.brush.color[2] = shell.foreground.b; /* 抹色不透明：不透明度由 flow 表达（固定 1） */
  desc.brush.diameter = static_cast<float>(shell.brush_size);
  desc.brush.pressure = 1.0f; /* 鼠标无压感：按上游满笔压 */
  desc.brush.hardness = static_cast<float>(shell.brush_hardness);
  desc.brush.flow = 1.0f;
  desc.brush.seed = 0; /* 圆头笔是解析式的；固定值守住「同参数同结果」 */
  desc.erase = erasing ? 1 : 0;
  desc.stroke_points = points.data();
  desc.stroke_point_count = static_cast<int32_t>(points.size() / 2);

  char note[256] = {0};
  size_t note_size = 0;
  const comp_result result =
      comp_tool_apply(shell.document, id, &desc, note, sizeof(note), &note_size);
  shell.needs_render = true;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"%s失败（错误码 %d）", erasing ? L"橡皮擦" : L"画笔",
              static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  if (erasing) {
    wsprintfW(buffer, L"橡皮擦：已擦除 %d 个折点（直径 %d px）", desc.stroke_point_count,
              static_cast<int>(shell.brush_size + 0.5));
  } else if (note[0] != '\0' && note_size > 1) {
    wsprintfW(buffer, L"画笔：已落色 %d 个折点｜%s", desc.stroke_point_count,
              widen_ascii(note).c_str());
  } else {
    wsprintfW(buffer, L"画笔：已落色 %d 个折点（%s · 直径 %d px）", desc.stroke_point_count,
              widen_ascii(hex).c_str(), static_cast<int>(shell.brush_size + 0.5));
  }
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 吸管（只读取色） */

/*
 * 取样结果写进 shell.foreground（0..1 直通 sRGB -> 0..255）。
 * 吸管是只读工具：不进撤销栈、不改选区、不改图层。
 */
bool eyedropper_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Eyedropper;
}

void eyedropper_sample(ShellState& shell, int x, int y, const Layout& layout) {
  if (!eyedropper_available(shell)) return;
  if (!layout.canvas.contains(x, y)) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  /* 画布外不取样：ABI 对越界坐标返回 INVALID_ARGUMENT，那不是「吸管坏了」。 */
  if (doc.x < 0.0 || doc.y < 0.0 || doc.x >= static_cast<double>(shell.width) ||
      doc.y >= static_cast<double>(shell.height)) {
    return;
  }

  const bool merged = (GetKeyState(VK_MENU) & 0x8000) != 0;
  comp_layer_id source_layer = merged ? 0 : selected_layer_id(shell);
  comp_eyedropper_source source = COMP_EYEDROPPER_LAYER;
  if (merged || source_layer == 0) {
    /* 没有选中图层时取整篇合成：画布上看到什么就吸什么，比报错更符合预期。 */
    source = COMP_EYEDROPPER_MERGED;
    source_layer = 0;
  }

  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const comp_result result = comp_eyedropper_sample(
      shell.document, source_layer, doc.x, doc.y, 1, source, rgba);
  wchar_t buffer[256];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"吸管：取样失败（%s，错误码 %d）",
              source == COMP_EYEDROPPER_MERGED ? L"合并源" : L"当前图层",
              static_cast<int>(result));
    set_status(shell, buffer);
    return;
  }

  shell.foreground = comp_ui::color_from_unit(rgba[0], rgba[1], rgba[2]);
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  const comp_ui::ColorHSV hsv = comp_ui::rgb_to_hsv(shell.foreground);
  wsprintfW(buffer, L"吸管：%s · %s · H %d° S %d%% B %d%%（不透明度 %d%%）",
            widen_ascii(hex).c_str(),
            source == COMP_EYEDROPPER_MERGED ? L"合并源" : L"当前图层",
            static_cast<int>(hsv.h + 0.5), static_cast<int>(hsv.s * 100.0 + 0.5),
            static_cast<int>(hsv.v * 100.0 + 0.5),
            static_cast<int>(rgba[3] * 100.0f + 0.5f));
  set_status(shell, buffer);
}

void eyedropper_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!eyedropper_available(shell)) return;
  shell.eyedropper_drag = true;
  SetCapture(window);
  eyedropper_sample(shell, x, y, layout);
}

void eyedropper_cancel(ShellState& shell) { shell.eyedropper_drag = false; }

/* ------------------------------------------------------------ 选区搬移（Move） */

/*
 * 一次拖拽 = 一步撤销：像素搬移与轮廓平移写进同一个显式事务。
 * 两个 ABI 调用内部的 ImplicitTxn 会「加入」已开启的显式事务，
 * 因此这里不需要把两步合成一个新的算子。
 */
bool move_available(const ShellState& shell) {
  return shell.document != nullptr && shell.tool == comp_ui::ToolId::Move;
}

void move_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!move_available(shell)) return;
  if (!shell.selection_active) {
    set_status(shell, L"移动：当前没有选区（先用选框建立选区）");
    return;
  }
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  shell.move_active = true;
  shell.move_start = doc;
  shell.move_current = doc;
  shell.move_duplicate = (GetKeyState(VK_MENU) & 0x8000) != 0;
  SetCapture(window);
  set_status(shell, shell.move_duplicate
                        ? L"移动选区像素：Alt = 复制，松开提交"
                        : L"移动选区像素：松开提交（按住 Alt = 复制）");
}

void move_extend(ShellState& shell, int x, int y, const Layout& layout) {
  if (!shell.move_active) return;
  shell.move_current =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
}

void move_cancel(ShellState& shell) { shell.move_active = false; }

void move_end(HWND window, ShellState& shell) {
  if (!shell.move_active) return;
  shell.move_active = false;
  ReleaseCapture();
  if (shell.document == nullptr) return;

  const int32_t dx =
      static_cast<int32_t>(std::lround(shell.move_current.x - shell.move_start.x));
  const int32_t dy =
      static_cast<int32_t>(std::lround(shell.move_current.y - shell.move_start.y));
  if (dx == 0 && dy == 0) {
    set_status(shell, L"移动：位置未变化");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"移动：请先选择一个图层");
    return;
  }

  comp_txn txn = nullptr;
  if (comp_txn_begin(shell.document,
                     shell.move_duplicate ? "Duplicate Selection Pixels"
                                          : "Move Selection Pixels",
                     &txn) != COMP_OK) {
    set_status(shell, L"移动：无法开始事务");
    return;
  }

  /* 顺序不能反：move_pixels 按当前选区取像素，先平移轮廓会搬错地方。 */
  int32_t moved_pixels = 0;
  int32_t moved_outline = 0;
  comp_result result = comp_selection_move_pixels(
      shell.document, id, dx, dy, shell.move_duplicate ? 1 : 0, &moved_pixels);
  if (result == COMP_OK) {
    result = comp_selection_translate(shell.document, dx, dy, &moved_outline);
  }
  if (result != COMP_OK) {
    /* 失败必须整体回滚：半搬的像素比不搬更坏。 */
    comp_txn_abort(txn);
    set_status(shell, L"移动失败（16 位 / 带旋转或翻转的图层不支持像素搬移，轮廓已回滚）");
    return;
  }
  if (moved_pixels == 0 && moved_outline == 0) {
    comp_txn_abort(txn);
    set_status(shell, L"移动：选区内没有可搬移的像素");
    return;
  }
  if (comp_txn_commit(txn) != COMP_OK) {
    set_status(shell, L"移动：提交失败");
    return;
  }

  refresh_selection(shell);
  shell.needs_render = true;
  wchar_t buffer[224];
  wsprintfW(buffer, L"%s：位移 (%d, %d) px，%d 个像素",
            shell.move_duplicate ? L"已复制选区像素" : L"已移动选区像素", dx, dy,
            moved_pixels);
  refresh_history_status(shell, buffer);
}

/* ------------------------------------------------------------ 文件与命令 */

bool create_document(ShellState& shell) {
  comp_document_desc desc{};
  desc.width = kDefaultWidth;
  desc.height = kDefaultHeight;
  desc.color_space = "sRGB";

  comp_document document = nullptr;
  const comp_result result = comp_document_create(shell.engine, &desc, &document);
  if (result != COMP_OK || document == nullptr) {
    set_status(shell, L"新建文档失败");
    return false;
  }
  if (shell.document != nullptr) comp_document_destroy(shell.document);
  shell.document = document;

  comp_size size{};
  if (comp_document_size(shell.document, &size) == COMP_OK) {
    shell.width = size.width;
    shell.height = size.height;
  }
  const size_t pixel_count = static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
  shell.rgba.assign(pixel_count * 4, 0);
  shell.framebuffer.assign(pixel_count, 0);
  shell.needs_render = true;
  shell.needs_fit = true;
  shell.visibility.clear();
  /* 新文档没有「之前的滤镜面板/未完成笔迹」可言，一并复位，避免跨文档误应用。 */
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.filter.select(comp_ui::FilterId::Vignette);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已新建文档");
  return true;
}

bool load_document(ShellState& shell, const char* path_utf8) {
  comp_document document = nullptr;
  if (comp_document_load(shell.engine, path_utf8, &document) != COMP_OK || document == nullptr) {
    set_status(shell, L"打开 .comp 失败");
    return false;
  }
  if (shell.document != nullptr) comp_document_destroy(shell.document);
  shell.document = document;

  comp_size size{};
  if (comp_document_size(shell.document, &size) == COMP_OK) {
    shell.width = size.width;
    shell.height = size.height;
  }
  const size_t pixel_count = static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
  shell.rgba.assign(pixel_count * 4, 0);
  shell.framebuffer.assign(pixel_count, 0);
  shell.needs_render = true;
  shell.needs_fit = true;
  shell.visibility.clear();
  shell.filter_open = false;
  shell.filter_input.clear();
  shell.filter.select(comp_ui::FilterId::Vignette);
  stroke_cancel(shell);
  move_cancel(shell);
  eyedropper_cancel(shell);
  marquee_cancel(shell);
  lasso_cancel(shell);
  pen_cancel(shell);
  refresh_layers(shell);
  refresh_selection(shell);
  refresh_history_status(shell, L"已打开 .comp");
  return true;
}

bool save_document(ShellState& shell, const char* path_utf8) {
  if (shell.document == nullptr) return false;
  const bool ok = comp_document_save(shell.document, path_utf8) == COMP_OK;
  set_status(shell, ok ? L"已保存 .comp" : L"保存 .comp 失败");
  return ok;
}

/* ------------------------------------------------------------ 撤销 / 重做 */

void undo_document(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t can_undo = 0;
  if (comp_document_can_undo(shell.document, &can_undo) != COMP_OK || can_undo == 0) {
    set_status(shell, L"已到最早状态，无可撤销");
    return;
  }
  const comp_result result = comp_document_undo(shell.document);
  if (result != COMP_OK) {
    set_status(shell, L"撤销失败");
    return;
  }
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell); /* 选区操作也在撤销栈里：撤销可能改变蚂蚁线 */
  refresh_history_status(shell, L"已撤销");
}

void redo_document(ShellState& shell) {
  if (shell.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  int32_t can_redo = 0;
  if (comp_document_can_redo(shell.document, &can_redo) != COMP_OK || can_redo == 0) {
    set_status(shell, L"已到最新状态，无可重做");
    return;
  }
  const comp_result result = comp_document_redo(shell.document);
  if (result != COMP_OK) {
    set_status(shell, L"重做失败");
    return;
  }
  shell.needs_render = true;
  refresh_layers(shell);
  refresh_selection(shell); /* 同撤销：重做也可能恢复/撤掉选区 */
  refresh_history_status(shell, L"已重做");
}

/* 命令层冒烟：两段式枚举，只报数量，不猜测命令 id。 */
void list_commands(ShellState& shell) {
  if (shell.document == nullptr) return;
  size_t count = 0;
  /* count-only 模式不要求文档；带 desc 的枚举才需要按文档状态求值 enabled。 */
  if (comp_command_list(shell.document, nullptr, 0, &count) != COMP_OK) {
    set_status(shell, L"命令枚举失败");
    return;
  }
  wchar_t buffer[96];
  wsprintfW(buffer, L"引擎注册命令 %u 条", static_cast<unsigned>(count));
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 文件对话框 */

void open_document_dialog(HWND window, ShellState& shell) {
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = L"Compositor 项目 (*.comp)\0*.comp\0所有文件\0*.*\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  load_document(shell, utf8);
}

void save_document_dialog(HWND window, ShellState& shell) {
  if (shell.document == nullptr) return;
  wchar_t buffer[MAX_PATH] = L"";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = L"Compositor 项目 (*.comp)\0*.comp\0";
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrDefExt = L"comp";
  if (!GetSaveFileNameW(&dialog)) return;
  char utf8[MAX_PATH * 3] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  save_document(shell, utf8);
}

/* 视图 / 工具类命令的实现在窗口过程之后，这里先给出声明：
 * run_shell_command 是菜单与快捷键的唯一入口，不能被定义顺序卡住。 */
void set_tool(ShellState& shell, comp_ui::ToolId id);
void brush_step(ShellState& shell, int direction, bool hardness);
bool client_layout(const ShellState& shell, Layout* out);
void view_fit(ShellState& shell);
void view_actual(ShellState& shell);
void view_zoom_step(ShellState& shell, double factor);

/* 菜单与快捷键共用同一入口，保证两条路径行为不分叉。 */
void run_shell_command(HWND window, ShellState& shell, UINT id) {
  switch (id) {
    case kCmdNew:
      create_document(shell);
      break;
    case kCmdOpen:
      open_document_dialog(window, shell);
      break;
    case kCmdSave:
      save_document_dialog(window, shell);
      break;
    case kCmdUndo:
      undo_document(shell);
      break;
    case kCmdRedo:
      redo_document(shell);
      break;
    case kCmdListCommands:
      list_commands(shell);
      break;
    case kCmdSelectAll:
      select_all(shell);
      break;
    case kCmdSelectNone:
      deselect_all(shell);
      break;
    case kCmdSelectInvert:
      invert_selection(shell);
      break;
    case kCmdLayerAdd:
      panel_add_layer(shell);
      break;
    case kCmdLayerMergeDown:
      panel_merge_down(shell);
      break;
    case kCmdLayerFlatten:
      panel_flatten(shell);
      break;
    case kCmdViewFit:
      view_fit(shell);
      break;
    case kCmdViewActual:
      view_actual(shell);
      break;
    case kCmdViewZoomIn:
      view_zoom_step(shell, 1.2);
      break;
    case kCmdViewZoomOut:
      view_zoom_step(shell, 1.0 / 1.2);
      break;
    case kCmdLayerSelectUp:
      panel_move_selection(shell, 1); /* 面板自上而下：上移一行 = 索引 +1 */
      break;
    case kCmdLayerSelectDown:
      panel_move_selection(shell, -1);
      break;
    case kCmdLayerToggleVisible:
      panel_toggle_visible(shell);
      break;
    case kCmdLayerOpacityDown:
      panel_adjust_opacity(shell, -0.1f);
      break;
    case kCmdLayerOpacityUp:
      panel_adjust_opacity(shell, 0.1f);
      break;
    case kCmdAdjKindPrev:
      panel_cycle_kind(shell, -1);
      break;
    case kCmdAdjKindNext:
      panel_cycle_kind(shell, 1);
      break;
    case kCmdAdjRowPrev:
      panel_select_adjustment(shell, -1);
      break;
    case kCmdAdjRowNext:
      panel_select_adjustment(shell, 1);
      break;
    case kCmdAdjParamPrev:
      panel_move_param(shell, -1);
      break;
    case kCmdAdjParamNext:
      panel_move_param(shell, 1);
      break;
    case kCmdAdjValueDown:
      panel_step_value(shell, -1);
      break;
    case kCmdAdjValueUp:
      panel_step_value(shell, 1);
      break;
    case kCmdAdjAdd:
      panel_apply_adjustment(shell, true);
      break;
    case kCmdAdjApply:
      panel_apply_adjustment(shell, false);
      break;
    case kCmdAdjRemove:
      panel_remove_adjustment(shell);
      break;
    case kCmdFilterApply:
      apply_filter(shell);
      break;
    case kCmdFilterReset:
      filter_reset_values(shell);
      break;
    case kCmdFilterClose:
      close_filter_panel(shell);
      break;
    case kCmdExit:
      PostMessageW(window, WM_CLOSE, 0, 0);
      return;
    default:
      /* 滤镜菜单项：kCmdFilterBase + 滤镜序号。 */
      if (id >= kCmdFilterBase && id < kCmdFilterBase + comp_ui::filter_count()) {
        open_filter_panel(shell, static_cast<comp_ui::FilterId>(id - kCmdFilterBase));
        break;
      }
      return;
  }
  InvalidateRect(window, nullptr, FALSE);
}

void render_canvas(ShellState& shell) {
  if (shell.document == nullptr || shell.rgba.empty()) return;
  const size_t stride = static_cast<size_t>(shell.width) * 4;
  const comp_result result =
      comp_render_flatten(shell.document, shell.rgba.data(), COMP_PIXEL_RGBA8, stride);
  if (result == COMP_OK) {
    /* 预乘 RGBA8 -> BGRA：GDI 的 BI_RGB 32bpp 位图按 BGRA 解释。 */
    const size_t pixel_count = static_cast<size_t>(shell.width) * static_cast<size_t>(shell.height);
    for (size_t i = 0; i < pixel_count; ++i) {
      const uint8_t* src = shell.rgba.data() + i * 4;
      uint32_t bgra = (static_cast<uint32_t>(src[3]) << 24) |
                      (static_cast<uint32_t>(src[0]) << 16) |
                      (static_cast<uint32_t>(src[1]) << 8) |
                      static_cast<uint32_t>(src[2]);
      shell.framebuffer[i] = bgra;
    }
  }
  shell.needs_render = false;
}

/* 工具条：按钮几何与命中共享 ui_core.h，壳只负责把选中态画出来。 */
void draw_toolbar(HDC dc, const ShellState& shell, const comp_ui::Rect& bar) {
  RECT background_rect{bar.x, bar.y, bar.right(), bar.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(44, 46, 52));
  FillRect(dc, &background_rect, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  const int count = comp_ui::tool_count();
  for (int i = 0; i < count; ++i) {
    const comp_ui::ToolInfo* info = comp_ui::tool_info_at(i);
    if (info == nullptr) continue;
    const comp_ui::Rect rect = comp_ui::tool_button_rect(bar, i);
    RECT button{rect.x, rect.y, rect.right(), rect.bottom()};
    const bool selected = info->id == shell.tool;
    HBRUSH fill = CreateSolidBrush(selected ? RGB(70, 104, 168) : RGB(58, 60, 68));
    FillRect(dc, &button, fill);
    DeleteObject(fill);
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(198, 200, 208));
    const wchar_t letter[2] = {
        info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ', 0};
    DrawTextW(dc, letter, 1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  /* 右侧读数：缩放、笔刷，Retouch 工具再带上子模式与当前参数。 */
  wchar_t extra[96] = L"";
  if (shell.tool == comp_ui::ToolId::Retouch) {
    if (shell.retouch_mode == comp_ui::kRetouchBlur) {
      wsprintfW(extra, L"   %s / 半径 %d px",
                widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str(),
                static_cast<int>(shell.blur_radius + 0.5));
    } else {
      wsprintfW(extra, L"   %s / 强度 %d%%",
                widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str(),
                static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
    }
  }
  wchar_t info_text[192];
  wsprintfW(info_text, L"缩放 %d%%   笔刷 %d px / %d%%%s",
            static_cast<int>(shell.view.zoom * 100.0 + 0.5),
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5), extra);
  RECT info_rect{bar.right() - 460, bar.y, bar.right() - 10, bar.bottom()};
  SetTextColor(dc, RGB(172, 176, 188));
  DrawTextW(dc, info_text, -1, &info_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

/* 状态栏：工具 / 缩放 / 历史，加上壳层最近一条反馈。 */
void draw_status_bar(HDC dc, const ShellState& shell, const comp_ui::Rect& bar) {
  RECT background_rect{bar.x, bar.y, bar.right(), bar.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(26, 28, 32));
  FillRect(dc, &background_rect, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  const comp_ui::ToolInfo* info = comp_ui::tool_info(shell.tool);
  wchar_t tool_name[96];
  if (shell.tool == comp_ui::ToolId::Retouch) {
    /* R 键在同一工具内循环：状态栏必须显示当前子模式，否则用户不知道自己拿的是哪个。 */
    wsprintfW(tool_name, L"Retouch / %s",
              widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  } else {
    wsprintfW(tool_name, L"%s", info != nullptr ? widen_ascii(info->name).c_str() : L"?");
  }
  wchar_t extra[64] = L"";
  if (shell.tool == comp_ui::ToolId::Retouch) {
    if (shell.retouch_mode == comp_ui::kRetouchBlur) {
      wsprintfW(extra, L" · 半径 %d px", static_cast<int>(shell.blur_radius + 0.5));
    } else {
      wsprintfW(extra, L" · 强度 %d%%",
                static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
    }
  }
  wchar_t line[768];
  wsprintfW(line, L"%s（%c） · 缩放 %d%% · 笔刷 %d px / %d%%%s · %s", tool_name,
            info != nullptr && info->shortcut != 0
                ? static_cast<wchar_t>(info->shortcut - 'a' + 'A')
                : L' ',
            static_cast<int>(shell.view.zoom * 100.0 + 0.5),
            static_cast<int>(shell.brush_size + 0.5),
            static_cast<int>(shell.brush_hardness * 100.0 + 0.5), extra, shell.status.c_str());
  /* 右端留给前景色读数：色块 + 十六进制，左端长文本按省略号截断。 */
  RECT text_rect{bar.x + 12, bar.y, bar.right() - 200, bar.bottom()};
  SetTextColor(dc, RGB(220, 222, 228));
  DrawTextW(dc, line, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  if (bar.w >= 260) {
    char hex[8] = {0};
    comp_ui::color_to_hex(shell.foreground, hex);
    RECT swatch{bar.right() - 190, bar.y + 5, bar.right() - 166, bar.y + 21};
    HBRUSH foreground_brush =
        CreateSolidBrush(RGB(shell.foreground.r, shell.foreground.g, shell.foreground.b));
    FillRect(dc, &swatch, foreground_brush);
    DeleteObject(foreground_brush);
    HBRUSH swatch_border = CreateSolidBrush(RGB(96, 100, 112));
    FrameRect(dc, &swatch, swatch_border);
    DeleteObject(swatch_border);

    wchar_t foreground_text[64];
    wsprintfW(foreground_text, L"前景 %s", widen_ascii(hex).c_str());
    RECT foreground_rect{bar.right() - 158, bar.y, bar.right() - 10, bar.bottom()};
    SetTextColor(dc, RGB(220, 222, 228));
    DrawTextW(dc, foreground_text, -1, &foreground_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
}

/* 图层面板：GDI 文本绘制（外壳不读像素，只呈现引擎的属性查询结果）。 */
void draw_layer_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  RECT panel{panel_rect.x, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(24, 26, 30));
  FillRect(dc, &panel, background);
  DeleteObject(background);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(240, 240, 244));
  wchar_t title[64];
  wsprintfW(title, L"图层（%u）", static_cast<unsigned>(shell.layers.size()));
  TextOutW(dc, panel_rect.x + 10, panel_rect.y + 8, title, lstrlenW(title));

  const int32_t count = static_cast<int32_t>(shell.layers.size());
  for (int32_t line = 0; line < count; ++line) {
    const comp_ui::Rect row_rect = comp_ui::layer_row_rect(panel_rect, line);
    const LayerRow& row = shell.layers[static_cast<size_t>(count - 1 - line)];
    if (line == shell.selected_layer) {
      RECT highlight{row_rect.x + 2, row_rect.y + 1, row_rect.right() - 2,
                     row_rect.bottom() - 1};
      HBRUSH selected = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, selected);
      DeleteObject(selected);
      SetTextColor(dc, RGB(255, 255, 255));
    } else {
      SetTextColor(dc, RGB(206, 208, 216));
    }
    wchar_t line_text[128];
    wsprintfW(line_text, L"%s #%I64u  混合#%d  %d%%", row.visible != 0 ? L"[o]" : L"[-]",
              static_cast<unsigned long long>(row.id), row.blend,
              static_cast<int>(row.opacity * 100.0f + 0.5f));
    TextOutW(dc, row_rect.x + 10, row_rect.y + 3, line_text, lstrlenW(line_text));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hint = L"点击选中 · Ctrl+Shift+V 显隐 · Ctrl+[ ] 不透明度";
  TextOutW(dc, panel_rect.x + 10, panel_rect.bottom() - 46, hint, lstrlenW(hint));
  const wchar_t* hint2 = L"Ctrl+Shift+N 新建图层";
  TextOutW(dc, panel_rect.x + 10, panel_rect.bottom() - 26, hint2, lstrlenW(hint2));
}

/* 调整面板：控件完全由 ABI 元数据生成（外壳不复制键名表、不解析 JSON）。 */
void draw_adjust_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int height = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"调整（Adjustment）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;

  const int32_t total = static_cast<int32_t>(COMP_ADJUSTMENT_KIND_COUNT);
  wsprintfW(line, L"类型 %d/%d：%s", shell.adj_kind + 1, total,
            widen_ascii(adjustment_kind_name(static_cast<uint32_t>(shell.adj_kind))).c_str());
  SetTextColor(dc, RGB(150, 210, 255));
  TextOutW(dc, left + 10, y, line, lstrlenW(line));
  y += kAdjustRowHeight;

  size_t param_count = 0;
  comp_layer_adjustment_param_count(static_cast<uint32_t>(shell.adj_kind), &param_count);
  if (param_count == 0) {
    SetTextColor(dc, RGB(200, 170, 120));
    const wchar_t* note = L"（该类型无可面板编辑参数）";
    TextOutW(dc, left + 10, y, note, lstrlenW(note));
    y += kAdjustRowHeight;
  } else {
    for (size_t i = 0; i < param_count; ++i) {
      comp_adjustment_param param{};
      if (comp_layer_adjustment_param_at(static_cast<uint32_t>(shell.adj_kind), i, &param) != COMP_OK) {
        break;
      }
      const bool focused = static_cast<int32_t>(i) == shell.adj_param;
      if (focused) {
        RECT highlight{left + 4, y - 2, panel_rect.right() - 6, y + kAdjustRowHeight - 4};
        HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
        FillRect(dc, &highlight, brush);
        DeleteObject(brush);
      }
      SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
      const std::string value =
          i < shell.adj_values.size() ? shell.adj_values[i] : std::string("?");
      if (param.type == COMP_PARAM_BOOL) {
        wsprintfW(line, L"%s = %s", widen_ascii(param.label).c_str(),
                  value == "1" ? L"true" : L"false");
      } else {
        wsprintfW(line, L"%s = %s", widen_ascii(param.label).c_str(),
                  widen_ascii(value.c_str()).c_str());
      }
      TextOutW(dc, left + 12, y, line, lstrlenW(line));
      y += kAdjustRowHeight;
    }
  }

  y += 8;
  SetTextColor(dc, RGB(180, 184, 196));
  wsprintfW(line, L"该图层已有调整：%u", static_cast<unsigned>(shell.adj_settings.size()));
  TextOutW(dc, left + 10, y, line, lstrlenW(line));
  y += kAdjustRowHeight;
  for (size_t i = 0; i < shell.adj_settings.size() && y < height - 120; ++i) {
    const bool selected = static_cast<int32_t>(i) == shell.selected_adjustment;
    SetTextColor(dc, selected ? RGB(255, 226, 140) : RGB(196, 198, 208));
    wsprintfW(line, L"%s %u: %s", selected ? L">" : L" ", static_cast<unsigned>(i + 1),
              widen_ascii(adjustment_kind_name(shell.adj_kinds[i])).c_str());
    TextOutW(dc, left + 12, y, line, lstrlenW(line));
    y += kAdjustRowHeight;
  }

  if (!shell.adj_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    const std::string typed(shell.adj_input.begin(), shell.adj_input.end());
    wsprintfW(line, L"输入中：%s", widen_ascii(typed.c_str()).c_str());
    TextOutW(dc, left + 10, height - 108, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"Ctrl+Shift+K/J 切换类型",
      L", / . 选择已有调整",
      L"Tab 切参数 · - / = 调值",
      L"数字键输入 · 回车提交",
      L"Ctrl+Shift+A 新增 · Ctrl+Shift+U 应用 · Ctrl+Shift+D 删除",
  };
  int hint_y = height - 92;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 滤镜面板。行几何来自 ui_core.h 的 filter_param_row_rect——绘制与命中同源，
 * 避免「看得见点不中」。
 */
void draw_filter_panel(HDC dc, const ShellState& shell, const comp_ui::Rect& panel_rect) {
  const int left = panel_rect.x;
  const int bottom = panel_rect.bottom();
  RECT panel{left, panel_rect.y, panel_rect.right(), panel_rect.bottom()};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = panel_rect.y + 8;
  SetTextColor(dc, RGB(240, 240, 244));
  const wchar_t* title = L"滤镜（Filter）";
  TextOutW(dc, left + 10, y, title, lstrlenW(title));
  y += 22;

  const comp_ui::FilterSpec* spec = shell.filter.spec();
  SetTextColor(dc, RGB(150, 210, 255));
  wsprintfW(line, L"%d/%d：%s", static_cast<int>(shell.filter.id) + 1,
            comp_ui::filter_count(), spec != nullptr ? widen_ascii(spec->name).c_str() : L"?");
  TextOutW(dc, left + 10, y, line, lstrlenW(line));

  for (int row = 0; row < shell.filter.param_count(); ++row) {
    const comp_ui::FilterParamSpec* parameter = shell.filter.param_at(row);
    if (parameter == nullptr) break;
    const comp_ui::Rect row_rect = comp_ui::filter_param_row_rect(panel_rect, row);
    const bool focused = row == shell.filter.focused;
    if (focused) {
      RECT highlight{row_rect.x + 4, row_rect.y - 2, row_rect.right() - 6,
                     row_rect.bottom() + 2};
      HBRUSH brush = CreateSolidBrush(RGB(56, 76, 120));
      FillRect(dc, &highlight, brush);
      DeleteObject(brush);
    }
    SetTextColor(dc, focused ? RGB(255, 255, 255) : RGB(206, 208, 216));
    wsprintfW(line, L"%s = %s", widen_ascii(parameter->label).c_str(),
              widen_ascii(number_text(shell.filter.value(row)).c_str()).c_str());
    TextOutW(dc, left + 12, row_rect.y, line, lstrlenW(line));
  }

  if (!shell.filter_input.empty()) {
    SetTextColor(dc, RGB(255, 240, 160));
    wsprintfW(line, L"输入中：%s", shell.filter_input.c_str());
    TextOutW(dc, left + 10, bottom - 130, line, lstrlenW(line));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hints[] = {
      L"Tab / Shift+Tab 切参数",
      L"- / = 调值 · 数字键输入",
      L"回车应用 · Esc 关闭",
      L"Amount = 0 时滤镜为恒等（不改像素）",
  };
  int hint_y = bottom - 104;
  for (const wchar_t* hint : hints) {
    TextOutW(dc, left + 10, hint_y, hint, lstrlenW(hint));
    hint_y += 18;
  }
}

/*
 * 画布叠加层（选区蚂蚁线 / 框选矩形 / 笔迹预览 / 笔尖轮廓）。
 *
 * 只画在画布矩形内：先把轴对齐线段裁到画布，再生成虚线段，避免 32 倍缩放
 * 时一条边生成上万个屏幕虚线段。几何与分段规则全部来自 ui_core.h，这里只做
 * 「文档坐标 -> 屏幕坐标 -> GDI 调用」的搬运。
 */
bool clip_axis_aligned(const comp_ui::Rect& canvas, comp_ui::Point* a, comp_ui::Point* b) {
  const double tolerance = 1e-6;
  if (std::fabs(a->y - b->y) <= tolerance) { /* 水平线 */
    const double y = a->y;
    if (y < static_cast<double>(canvas.y) || y >= static_cast<double>(canvas.bottom())) {
      return false;
    }
    double x0 = std::min(a->x, b->x);
    double x1 = std::max(a->x, b->x);
    x0 = std::max(x0, static_cast<double>(canvas.x));
    x1 = std::min(x1, static_cast<double>(canvas.right()));
    if (x1 <= x0) return false;
    *a = comp_ui::Point{x0, y};
    *b = comp_ui::Point{x1, y};
    return true;
  }
  if (std::fabs(a->x - b->x) <= tolerance) { /* 垂直线 */
    const double x = a->x;
    if (x < static_cast<double>(canvas.x) || x >= static_cast<double>(canvas.right())) {
      return false;
    }
    double y0 = std::min(a->y, b->y);
    double y1 = std::max(a->y, b->y);
    y0 = std::max(y0, static_cast<double>(canvas.y));
    y1 = std::min(y1, static_cast<double>(canvas.bottom()));
    if (y1 <= y0) return false;
    *a = comp_ui::Point{x, y0};
    *b = comp_ui::Point{x, y1};
    return true;
  }
  return true; /* 非轴对齐线段（当前不产生）原样返回，不做错误裁剪 */
}

void draw_canvas_overlays(HDC dc, const ShellState& shell, const Layout& layout) {
  if (layout.canvas.empty()) return;
  const int saved = SaveDC(dc);
  IntersectClipRect(dc, layout.canvas.x, layout.canvas.y, layout.canvas.right(),
                    layout.canvas.bottom());
  SetBkMode(dc, TRANSPARENT);

  /*
   * 移动预览：拖动期间蚂蚁线整体跟着鼠标走，像素与轮廓在松开时一次性提交。
   * 偏移取整到文档像素，和 move_end 提交的 dx/dy 完全一致，预览不会「差一像素」。
   */
  int ants_dx = 0;
  int ants_dy = 0;
  if (shell.move_active) {
    ants_dx = static_cast<int>(std::lround(shell.move_current.x - shell.move_start.x));
    ants_dy = static_cast<int>(std::lround(shell.move_current.y - shell.move_start.y));
  }

  /* 选区蚂蚁线：白色底线 + 黑色虚线段；相位随时间前进 = 「蚂蚁在爬」。 */
  if (shell.selection_active && !shell.selection_segments.empty()) {
    HPEN base_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dash_pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, base_pen);
    for (const auto& segment : shell.selection_segments) {
      comp_ui::Point a = shell.view.doc_to_screen(segment.x0 + ants_dx, segment.y0 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      comp_ui::Point b = shell.view.doc_to_screen(segment.x1 + ants_dx, segment.y1 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      if (!clip_axis_aligned(layout.canvas, &a, &b)) continue;
      MoveToEx(dc, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)),
               nullptr);
      LineTo(dc, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    }

    SelectObject(dc, dash_pen);
    comp_ui::DashSpan spans[1024];
    for (const auto& segment : shell.selection_segments) {
      comp_ui::Point a = shell.view.doc_to_screen(segment.x0 + ants_dx, segment.y0 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      comp_ui::Point b = shell.view.doc_to_screen(segment.x1 + ants_dx, segment.y1 + ants_dy,
                                                 layout.canvas, shell.width, shell.height);
      if (!clip_axis_aligned(layout.canvas, &a, &b)) continue;
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double length = std::sqrt(dx * dx + dy * dy);
      if (!(length > 0.0)) continue;
      const int count = comp_ui::dash_spans(length, kAntsDashPx, kAntsGapPx,
                                            shell.selection_phase, spans, 1024);
      const int drawn = std::min(count, 1024);
      for (int i = 0; i < drawn; ++i) {
        const double t0 = spans[i].begin / length;
        const double t1 = spans[i].end / length;
        MoveToEx(dc, static_cast<int>(std::lround(a.x + dx * t0)),
                 static_cast<int>(std::lround(a.y + dy * t0)), nullptr);
        LineTo(dc, static_cast<int>(std::lround(a.x + dx * t1)),
               static_cast<int>(std::lround(a.y + dy * t1)));
      }
    }
    SelectObject(dc, old_pen);
    DeleteObject(base_pen);
    DeleteObject(dash_pen);
  }

  /* 框选预览：文档坐标两点 -> 屏幕矩形；白实线 + 黑虚线（M 键切椭圆）。 */
  if (shell.marquee_active) {
    const comp_ui::Point a = shell.view.doc_to_screen(shell.marquee_start.x,
                                                     shell.marquee_start.y, layout.canvas,
                                                     shell.width, shell.height);
    const comp_ui::Point b = shell.view.doc_to_screen(shell.marquee_current.x,
                                                     shell.marquee_current.y, layout.canvas,
                                                     shell.width, shell.height);
    const int left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (shell.marquee_shape == comp_ui::kMarqueeEllipse) {
      Ellipse(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Ellipse(dc, left, top, right, bottom);
    } else {
      Rectangle(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Rectangle(dc, left, top, right, bottom);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /*
   * 渐变预览（S9）：方向线 + 两个端点方块；径向额外画半径圆。只是几何提示，
   * 不预演颜色渐变——真正的像素在松开时由引擎一次性写出。
   */
  if (shell.gradient_active) {
    const comp_ui::Point a =
        shell.view.doc_to_screen(shell.gradient_start.x, shell.gradient_start.y,
                                 layout.canvas, shell.width, shell.height);
    const comp_ui::Point b =
        shell.view.doc_to_screen(shell.gradient_current.x, shell.gradient_current.y,
                                 layout.canvas, shell.width, shell.height);
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dashed = CreatePen(PS_DASH, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int ax = static_cast<int>(std::lround(a.x));
    const int ay = static_cast<int>(std::lround(a.y));
    const int bx = static_cast<int>(std::lround(b.x));
    const int by = static_cast<int>(std::lround(b.y));
    MoveToEx(dc, ax, ay, nullptr);
    LineTo(dc, bx, by);
    if (shell.gradient_type == comp_ui::kGradientRadial) {
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const int radius = static_cast<int>(std::lround(std::sqrt(dx * dx + dy * dy)));
      SelectObject(dc, dashed);
      Ellipse(dc, ax - radius, ay - radius, ax + radius, ay + radius);
      SelectObject(dc, solid);
    }
    /* 起点 = 空心方框（色标 0），终点 = 实心小方块（色标 1）。 */
    Rectangle(dc, ax - 3, ay - 3, ax + 4, ay + 4);
    HGDIOBJ old_fill = SelectObject(dc, GetStockObject(BLACK_BRUSH));
    Rectangle(dc, bx - 2, by - 2, bx + 3, by + 3);
    SelectObject(dc, old_fill);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dashed);
  }

  /*
   * 形状预览（S10）：矩形/椭圆画白实线 + 黑虚线双描边（圆角矩形用 RoundRect），
   * 直线画一条双线。只是几何提示，不预演颜色——真正的像素在松开时由引擎一次性写出
   * （与渐变预览同口径）。修饰键（Shift = 正方/正圆、Alt = 从中心）在预览里就生效，
   * 所见即落笔盒。
   */
  if (shell.shape_active) {
    const bool square = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const comp_ui::ShapeDragBox box = comp_ui::shape_drag_box(
        shell.shape_start.x, shell.shape_start.y, shell.shape_current.x, shell.shape_current.y,
        square, from_center);
    const comp_ui::Point a = shell.view.doc_to_screen(box.x0, box.y0, layout.canvas,
                                                      shell.width, shell.height);
    const comp_ui::Point b = shell.view.doc_to_screen(box.x1, box.y1, layout.canvas,
                                                      shell.width, shell.height);
    const int left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    const int top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    const int bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));

    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (shell.shape_kind == comp_ui::kShapeLine) {
      MoveToEx(dc, left, top, nullptr);
      LineTo(dc, right, bottom);
      SelectObject(dc, dotted);
      MoveToEx(dc, left, top, nullptr);
      LineTo(dc, right, bottom);
    } else if (shell.shape_kind == comp_ui::kShapeEllipse) {
      Ellipse(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Ellipse(dc, left, top, right, bottom);
    } else {
      /* 圆角半径按同样的屏幕缩放换算，预览与落笔盒一致。 */
      const double zoom = shell.view.zoom;
      const int radius = static_cast<int>(
          std::lround(comp_ui::shape_effective_corner_radius(shell.shape_corner_radius, box) *
                      (zoom > 0.0 ? zoom : 0.0)));
      if (radius > 0) {
        RoundRect(dc, left, top, right, bottom, radius * 2, radius * 2);
        SelectObject(dc, dotted);
        RoundRect(dc, left, top, right, bottom, radius * 2, radius * 2);
      } else {
        Rectangle(dc, left, top, right, bottom);
        SelectObject(dc, dotted);
        Rectangle(dc, left, top, right, bottom);
      }
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /*
   * 文字预览（S11）：插入点光标 + 未来文本框（白实线 + 黑虚线双描边）。
   * 只给位置与尺寸，不预演字形：外壳没有系统字体（字体是引擎内的 5×7 点阵），
   * 画一套 GDI 字只会与落笔结果不一致；像素在回车时由引擎一次写出（与渐变/
   * 形状预览同口径）。度量来自 ui_core，与内核 build_layout 同公式。
   */
  if (shell.type_active) {
    const comp_ui::TypeBlockMetrics metrics =
        comp_ui::type_block_metrics(shell.type_font_size, shell.type_line_height,
                                    shell.type_letter_spacing, shell.type_text.c_str());
    const comp_ui::Point origin = shell.view.doc_to_screen(
        shell.type_origin.x, shell.type_origin.y, layout.canvas, shell.width, shell.height);
    const comp_ui::Point corner = shell.view.doc_to_screen(
        shell.type_origin.x + metrics.width, shell.type_origin.y + metrics.height,
        layout.canvas, shell.width, shell.height);
    const int left = static_cast<int>(std::lround(std::min(origin.x, corner.x)));
    const int top = static_cast<int>(std::lround(std::min(origin.y, corner.y)));
    const int right = static_cast<int>(std::lround(std::max(origin.x, corner.x)));
    const int bottom = static_cast<int>(std::lround(std::max(origin.y, corner.y)));

    HPEN solid = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (right - left >= 1 && bottom - top >= 1) {
      Rectangle(dc, left, top, right, bottom);
      SelectObject(dc, dotted);
      Rectangle(dc, left, top, right, bottom);
      SelectObject(dc, solid);
    }
    /* 插入点光标：从原点向下画一个字号高（文档字号 × 缩放，至少 1 px）。 */
    const int caret = static_cast<int>(
        std::lround(comp_ui::type_caret_height(shell.type_font_size, shell.view.zoom)));
    MoveToEx(dc, left, top, nullptr);
    LineTo(dc, left, top + caret);
    SelectObject(dc, dotted);
    MoveToEx(dc, left + 1, top, nullptr);
    LineTo(dc, left + 1, top + caret);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  /* 套索预览：沿已采样的折线画一条高对比度细线（闭合边在松开时由引擎生成）。 */  if (shell.lasso_active && shell.lasso_points.size() >= 4) {
    HPEN trail = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, trail);
    bool started = false;
    for (size_t i = 0; i + 1 < shell.lasso_points.size(); i += 2) {
      const comp_ui::Point screen =
          shell.view.doc_to_screen(shell.lasso_points[i], shell.lasso_points[i + 1],
                                   layout.canvas, shell.width, shell.height);
      const int px = static_cast<int>(std::lround(screen.x));
      const int py = static_cast<int>(std::lround(screen.y));
      if (!started) {
        MoveToEx(dc, px, py, nullptr);
        started = true;
      } else {
        LineTo(dc, px, py);
      }
    }
    SelectObject(dc, old_pen);
    DeleteObject(trail);
  }

  /*
   * 钢笔预览（S8）：逐段采样折线 + 锚点方块 + 控制柄。闭合段用虚线提示
   * 「提交时会闭合成选区」——引擎 apply_path 对子路径按闭合解释。
   * 逐段取几何，避免给 2048 个锚点的整条路径开大缓冲。
   */
  if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
    const int anchor_count = static_cast<int>(shell.pen_anchors.size());
    const int segment_count = comp_ui::pen_segment_count(anchor_count, /*closed=*/false);
    HPEN path_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HPEN handle_pen = CreatePen(PS_DOT, 1, RGB(80, 200, 255));
    HGDIOBJ old_pen = SelectObject(dc, path_pen);

    comp_ui::Point samples[comp_ui::kPenPreviewSamples + 1];
    for (int i = 0; i < segment_count; ++i) {
      const comp_ui::PenSegment segment =
          comp_ui::pen_segment_at(shell.pen_anchors.data(), anchor_count, i);
      samples[0] = segment.start;
      for (int sample = 1; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        samples[sample] = comp_ui::pen_cubic_at(
            segment, static_cast<double>(sample) / static_cast<double>(comp_ui::kPenPreviewSamples));
      }
      bool started = false;
      for (int sample = 0; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        const comp_ui::Point screen =
            shell.view.doc_to_screen(samples[sample].x, samples[sample].y, layout.canvas,
                                     shell.width, shell.height);
        const int px = static_cast<int>(std::lround(screen.x));
        const int py = static_cast<int>(std::lround(screen.y));
        if (!started) {
          MoveToEx(dc, px, py, nullptr);
          started = true;
        } else {
          LineTo(dc, px, py);
        }
      }
    }

    /* 未闭合时用虚线画出「会补上的闭合边」：2 个锚点起就有意义。 */
    if (anchor_count >= 2) {
      SelectObject(dc, handle_pen);
      const comp_ui::PenSegment closing =
          comp_ui::pen_segment_at(shell.pen_anchors.data(), anchor_count, anchor_count - 1);
      bool started = false;
      for (int sample = 0; sample <= comp_ui::kPenPreviewSamples; ++sample) {
        const comp_ui::Point point =
            sample == 0 ? closing.start
                        : comp_ui::pen_cubic_at(
                              closing, static_cast<double>(sample) /
                                           static_cast<double>(comp_ui::kPenPreviewSamples));
        const comp_ui::Point screen = shell.view.doc_to_screen(
            point.x, point.y, layout.canvas, shell.width, shell.height);
        const int px = static_cast<int>(std::lround(screen.x));
        const int py = static_cast<int>(std::lround(screen.y));
        if (!started) {
          MoveToEx(dc, px, py, nullptr);
          started = true;
        } else {
          LineTo(dc, px, py);
        }
      }
    }

    /* 锚点与控制柄：锚点是 7×7 白框黑心，控制柄用 1px 虚线连到端点。 */
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, path_pen);
    for (int i = 0; i < anchor_count; ++i) {
      const comp_ui::PenAnchor& anchor = shell.pen_anchors[static_cast<size_t>(i)];
      const comp_ui::Point center = shell.view.doc_to_screen(
          anchor.position.x, anchor.position.y, layout.canvas, shell.width, shell.height);
      const int cx = static_cast<int>(std::lround(center.x));
      const int cy = static_cast<int>(std::lround(center.y));
      Rectangle(dc, cx - 3, cy - 3, cx + 4, cy + 4);
      SelectObject(dc, handle_pen);
      if (anchor.handle_out.x != 0.0 || anchor.handle_out.y != 0.0) {
        const comp_ui::Point handle = shell.view.doc_to_screen(
            anchor.position.x + anchor.handle_out.x, anchor.position.y + anchor.handle_out.y,
            layout.canvas, shell.width, shell.height);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, static_cast<int>(std::lround(handle.x)),
               static_cast<int>(std::lround(handle.y)));
      }
      if (anchor.handle_in.x != 0.0 || anchor.handle_in.y != 0.0) {
        const comp_ui::Point handle = shell.view.doc_to_screen(
            anchor.position.x + anchor.handle_in.x, anchor.position.y + anchor.handle_in.y,
            layout.canvas, shell.width, shell.height);
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, static_cast<int>(std::lround(handle.x)),
               static_cast<int>(std::lround(handle.y)));
      }
      SelectObject(dc, path_pen);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(path_pen);
    DeleteObject(handle_pen);
  }

  /* 进行中的笔迹折线（文档像素 -> 屏幕）：落色用前景色，重绘用高对比度青。 */
  if (stroke_in_progress(shell) && shell.stroke_points.size() >= 4) {
    const COLORREF trail_color =
        shell.stroke_kind == comp_ui::StrokeKind::Paint && !shell.stroke_erase
            ? RGB(shell.foreground.r, shell.foreground.g, shell.foreground.b)
            : RGB(80, 200, 255);
    HPEN trail = CreatePen(PS_SOLID, 3, trail_color);
    HGDIOBJ old_pen = SelectObject(dc, trail);
    bool started = false;
    for (size_t i = 0; i + 1 < shell.stroke_points.size(); i += 2) {
      const comp_ui::Point screen = shell.view.doc_to_screen(
          shell.stroke_points[i], shell.stroke_points[i + 1], layout.canvas, shell.width,
          shell.height);
      const int px = static_cast<int>(std::lround(screen.x));
      const int py = static_cast<int>(std::lround(screen.y));
      if (!started) {
        MoveToEx(dc, px, py, nullptr);
        started = true;
      } else {
        LineTo(dc, px, py);
      }
    }
    SelectObject(dc, old_pen);
    DeleteObject(trail);
  }

  /* 笔尖轮廓：吃笔刷直径的工具，鼠标在画布上时随时可见。 */
  if (shell.cursor_valid && comp_ui::tool_has_brush_tip(shell.tool) &&
      layout.canvas.contains(shell.cursor_x, shell.cursor_y)) {
    const double radius = comp_ui::brush_tip_radius(shell.brush_size, shell.view.zoom);
    const int left = static_cast<int>(std::lround(shell.cursor_x - radius));
    const int top = static_cast<int>(std::lround(shell.cursor_y - radius));
    const int right = static_cast<int>(std::lround(shell.cursor_x + radius));
    const int bottom = static_cast<int>(std::lround(shell.cursor_y + radius));
    HPEN solid = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(dc, solid);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, left, top, right, bottom);
    SelectObject(dc, dotted);
    Ellipse(dc, left, top, right, bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
  }

  RestoreDC(dc, saved);
}

void present(HWND window, ShellState& shell) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  if (shell.needs_render) render_canvas(shell);

  RECT client{};
  GetClientRect(window, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  const Layout layout = comp_ui::compute_layout(client_width, client_height);

  /* 新文档 / 打开文档后按窗口适配一次；之后缩放与平移完全由用户操作决定。 */
  if (shell.needs_fit) {
    if (shell.document != nullptr && !layout.canvas.empty()) {
      shell.view.fit(shell.width, shell.height, layout.canvas);
    }
    shell.needs_fit = false;
  }

  HDC memory_dc = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client_width, client_height);
  HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

  RECT fill{0, 0, client_width, client_height};
  HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
  FillRect(memory_dc, &fill, background);
  DeleteObject(background);

  /*
   * 画布上屏：视图变换给出文档 (0,0)/(w,h) 的屏幕位置，与画布矩形求交后
   * 只把可见部分交给 GDI。不能直接传整幅目标矩形——32x 放大时它会达到
   * 数万像素，既慢又可能触发 GDI 坐标上限。
   */
  if (!shell.framebuffer.empty() && shell.width > 0 && shell.height > 0 &&
      !layout.canvas.empty()) {
    const comp_ui::Point top_left = shell.view.doc_to_screen(
        0.0, 0.0, layout.canvas, shell.width, shell.height);
    const comp_ui::Point bottom_right = shell.view.doc_to_screen(
        shell.width, shell.height, layout.canvas, shell.width, shell.height);
    const double clip_left = std::max<double>(top_left.x, layout.canvas.x);
    const double clip_top = std::max<double>(top_left.y, layout.canvas.y);
    const double clip_right = std::min<double>(bottom_right.x, layout.canvas.right());
    const double clip_bottom = std::min<double>(bottom_right.y, layout.canvas.bottom());

    if (clip_right > clip_left && clip_bottom > clip_top) {
      const int dst_x = static_cast<int>(std::floor(clip_left));
      const int dst_y = static_cast<int>(std::floor(clip_top));
      const int dst_w = std::max(1, static_cast<int>(std::ceil(clip_right)) - dst_x);
      const int dst_h = std::max(1, static_cast<int>(std::ceil(clip_bottom)) - dst_y);

      int src_x = static_cast<int>(std::floor((clip_left - top_left.x) / shell.view.zoom));
      int src_y = static_cast<int>(std::floor((clip_top - top_left.y) / shell.view.zoom));
      int src_w = static_cast<int>(std::ceil(static_cast<double>(dst_w) / shell.view.zoom)) + 1;
      int src_h = static_cast<int>(std::ceil(static_cast<double>(dst_h) / shell.view.zoom)) + 1;
      if (src_x < 0) src_x = 0;
      if (src_y < 0) src_y = 0;
      if (src_x > shell.width) src_x = shell.width;
      if (src_y > shell.height) src_y = shell.height;
      if (src_x + src_w > shell.width) src_w = shell.width - src_x;
      if (src_y + src_h > shell.height) src_h = shell.height - src_y;

      if (src_w > 0 && src_h > 0) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = shell.width;
        info.bmiHeader.biHeight = -shell.height; /* 负值 = 自顶向下，与引擎缓冲一致 */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        const int saved = SaveDC(memory_dc);
        IntersectClipRect(memory_dc, layout.canvas.x, layout.canvas.y, layout.canvas.right(),
                          layout.canvas.bottom());
        SetStretchBltMode(memory_dc, HALFTONE);
        SetBrushOrgEx(memory_dc, 0, 0, nullptr);
        StretchDIBits(memory_dc, dst_x, dst_y, dst_w, dst_h, src_x, src_y, src_w, src_h,
                      shell.framebuffer.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        RestoreDC(memory_dc, saved);
      }
    }
  }

  draw_canvas_overlays(memory_dc, shell, layout);

  draw_toolbar(memory_dc, shell, layout.toolbar);
  if (layout.layer_panel.w > 0) draw_layer_panel(memory_dc, shell, layout.layer_panel);
  if (layout.adjust_panel.w > 0) {
    if (shell.filter_open) {
      draw_filter_panel(memory_dc, shell, layout.adjust_panel);
    } else {
      draw_adjust_panel(memory_dc, shell, layout.adjust_panel);
    }
  }
  draw_status_bar(memory_dc, shell, layout.status);

  BitBlt(dc, 0, 0, client_width, client_height, memory_dc, 0, 0, SRCCOPY);
  SelectObject(memory_dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory_dc);
  EndPaint(window, &paint);
}

/* ------------------------------------------------------------ 工具 / 视图 / 鼠标 */

void set_tool(ShellState& shell, comp_ui::ToolId id) {
  /* 离开钢笔工具 = 放弃未提交的路径（与切走套索/marquee 的取消口径一致）。 */
  if (shell.tool == comp_ui::ToolId::Pen && id != comp_ui::ToolId::Pen) {
    pen_cancel(shell);
  }
  /* 离开渐变工具 = 放弃进行中的拖拽（只丢预览，不产生撤销步）。 */
  if (shell.tool == comp_ui::ToolId::Gradient && id != comp_ui::ToolId::Gradient) {
    gradient_cancel(shell);
  }
  /* 离开形状工具同理：只丢预览。 */
  if (shell.tool == comp_ui::ToolId::Shape && id != comp_ui::ToolId::Shape) {
    shape_cancel(shell);
  }
  /* 离开文字工具 = 放弃未提交的键入（只丢预览，不产生撤销步）。 */
  if (shell.tool == comp_ui::ToolId::Type && id != comp_ui::ToolId::Type) {
    type_cancel(shell);
  }
  shell.tool = id;
  const comp_ui::ToolInfo* info = comp_ui::tool_info(id);
  if (info == nullptr) return;
  wchar_t buffer[192];
  if (id == comp_ui::ToolId::Retouch) {
    wsprintfW(buffer, L"工具：Retouch / %s（再按 R 循环）",
              widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
  } else if (id == comp_ui::ToolId::Marquee) {
    wsprintfW(buffer, L"工具：Marquee / %s（M 切矩形/椭圆，Shift 加选 / Alt 减选）",
              widen_ascii(comp_ui::marquee_shape_name(shell.marquee_shape)).c_str());
  } else if (id == comp_ui::ToolId::Lasso) {
    wsprintfW(buffer, L"工具：Lasso（拖出自由选区；Shift 加选 / Alt 减选；羽化 %d px）",
              shell.selection_feather);
  } else if (id == comp_ui::ToolId::Pen) {
    wsprintfW(buffer,
              L"工具：Pen（点击落锚点，拖动拉控制柄；点首锚点闭合，Enter 完成，Esc 取消；羽化 %d px）",
              shell.selection_feather);
  } else if (id == comp_ui::ToolId::Gradient) {
    wsprintfW(buffer,
              L"工具：Gradient / %s（拖出方向与长度后松开；再按 G 切线性/径向；Shift 反向；色标 = 前景 → 透明）",
              widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
  } else if (id == comp_ui::ToolId::Shape) {
    wsprintfW(buffer,
              L"工具：Shape / %s（拖出%s；Shift = 正方/正圆，Alt = 从中心；再按 U 切种类；"
              L"Alt+[ / ] 调线宽，Shift+Alt+[ / ] 调圆角）",
              widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
              shell.shape_kind == comp_ui::kShapeLine ? L"两端点" : L"对角点");
  } else if (id == comp_ui::ToolId::Type) {
    wsprintfW(buffer,
              L"工具：Type（画布点击落插入点，键入 ASCII；Shift+Enter 换行，回车提交，Esc 取消；"
              L"字号 %d px，行距 %d%%，字距 %d px；[ / ] 调字号，Shift+[ / ] 调字距）",
              static_cast<int>(shell.type_font_size + 0.5),
              static_cast<int>(shell.type_line_height * 100.0 + 0.5),
              static_cast<int>(std::lround(shell.type_letter_spacing)));
  } else if (id == comp_ui::ToolId::Wand) {
    wsprintfW(buffer,
              L"工具：Magic Wand（容差 %d，窗口 %d×%d，%s取样，%s；[ / ] 调容差，Alt+[ / ] 调窗口，Shift+W 切来源）",
              shell.wand_tolerance, shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1,
              shell.wand_merged ? L"合并" : L"当前图层",
              shell.wand_contiguous ? L"连通" : L"全图");
  } else {
    wsprintfW(buffer, L"工具：%s（%c）", widen_ascii(info->name).c_str(),
              info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ');
  }
  set_status(shell, buffer);
}

/* R 键循环 Blur / Smudge / Liquify；三者都走 comp_tool_apply。 */
void set_retouch_mode(ShellState& shell, int mode) {
  shell.retouch_mode = mode;
  shell.tool = comp_ui::ToolId::Retouch;
  wchar_t buffer[192];
  if (mode == comp_ui::kRetouchBlur) {
    wsprintfW(buffer, L"工具：Retouch / Blur（Alt+[ / ] 调半径，当前 %d px）",
              static_cast<int>(shell.blur_radius + 0.5));
  } else {
    wsprintfW(buffer, L"工具：Retouch / %s（Alt+[ / ] 调强度，当前 %d%%）",
              widen_ascii(comp_ui::retouch_mode_name(mode)).c_str(),
              static_cast<int>(shell.retouch_strength * 100.0 + 0.5));
  }
  set_status(shell, buffer);
}

void brush_step(ShellState& shell, int direction, bool hardness) {
  wchar_t buffer[96];
  if (hardness) {
    shell.brush_hardness = comp_ui::step_brush_hardness(shell.brush_hardness, direction);
    wsprintfW(buffer, L"笔刷硬度 %d%%",
              static_cast<int>(shell.brush_hardness * 100.0 + 0.5));
  } else {
    shell.brush_size = comp_ui::step_brush_size(shell.brush_size, direction);
    wsprintfW(buffer, L"笔刷直径 %d px", static_cast<int>(shell.brush_size + 0.5));
  }
  set_status(shell, buffer);
}

bool client_layout(const ShellState& shell, Layout* out) {
  RECT client{};
  if (shell.window == nullptr || !GetClientRect(shell.window, &client)) return false;
  *out = comp_ui::compute_layout(client.right - client.left, client.bottom - client.top);
  return true;
}

void view_fit(ShellState& shell) {
  Layout layout{};
  if (!client_layout(shell, &layout)) return;
  shell.view.fit(shell.width, shell.height, layout.canvas);
  shell.needs_fit = false;
  wchar_t buffer[96];
  wsprintfW(buffer, L"适合窗口：%d%%", static_cast<int>(shell.view.zoom * 100.0 + 0.5));
  set_status(shell, buffer);
}

void view_actual(ShellState& shell) {
  shell.view.actual_pixels();
  shell.needs_fit = false;
  set_status(shell, L"实际像素：100%");
}

void view_zoom_step(ShellState& shell, double factor) {
  Layout layout{};
  if (!client_layout(shell, &layout) || layout.canvas.empty()) return;
  const comp_ui::Point center = comp_ui::CanvasView::canvas_center(layout.canvas);
  shell.view.zoom_by(factor, center.x, center.y, layout.canvas);
  wchar_t buffer[96];
  wsprintfW(buffer, L"缩放 %d%%", static_cast<int>(shell.view.zoom * 100.0 + 0.5));
  set_status(shell, buffer);
}

void begin_pan(HWND window, ShellState& shell, int x, int y) {
  shell.panning = true;
  shell.pan_anchor.x = x;
  shell.pan_anchor.y = y;
  SetCapture(window);
  SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEALL)));
}

void end_pan(HWND window, ShellState& shell) {
  if (!shell.panning) return;
  shell.panning = false;
  ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
}

void canvas_click(ShellState& shell, int x, int y, const Layout& layout) {
  if (shell.document == nullptr) return;
  const comp_ui::Point doc =
      shell.view.screen_to_doc(x, y, layout.canvas, shell.width, shell.height);
  wchar_t buffer[128];
  wsprintfW(buffer, L"画布 (%d, %d)", static_cast<int>(std::floor(doc.x)),
            static_cast<int>(std::floor(doc.y)));
  set_status(shell, buffer);
}

/* ------------------------------------------------------------ 前景色键入（Shift+X） */

void begin_color_input(ShellState& shell) {
  shell.color_input = L"#";
  set_status(shell, L"前景色：键入 #RRGGBB 或 #RGB，回车确认（Esc 取消）");
}

void refresh_color_input_status(ShellState& shell) {
  wchar_t buffer[96];
  wsprintfW(buffer, L"前景色：%s（回车确认，Esc 取消）", shell.color_input.c_str());
  set_status(shell, buffer);
}

void commit_color_input(ShellState& shell) {
  char ascii[8] = {0};
  for (size_t i = 0; i < shell.color_input.size() && i < 7; ++i) {
    const wchar_t ch = shell.color_input[i];
    ascii[i] = ch < 128 ? static_cast<char>(ch) : '?';
  }
  comp_ui::ColorRGB parsed{};
  const bool ok = comp_ui::color_parse_hex(ascii, &parsed);
  shell.color_input.clear();
  if (!ok) {
    set_status(shell, L"前景色：格式无效（需 #RGB 或 #RRGGBB）");
    return;
  }
  shell.foreground = parsed;
  char hex[8] = {0};
  comp_ui::color_to_hex(shell.foreground, hex);
  wchar_t buffer[96];
  wsprintfW(buffer, L"前景色：%s", widen_ascii(hex).c_str());
  set_status(shell, buffer);
}

void handle_key(HWND window, ShellState& shell, WPARAM key, bool ctrl, bool shift) {
  /* 前景色十六进制键入态最先生效：它是模态输入，不能漏到工具快捷键。 */
  if (!shell.color_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.color_input.clear();
      set_status(shell, L"已取消前景色输入");
      return;
    }
    if (key == VK_BACK) {
      shell.color_input.pop_back();
      if (shell.color_input.empty()) {
        set_status(shell, L"已取消前景色输入");
      } else {
        refresh_color_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_color_input(shell);
      return;
    }
    const bool hex_digit = (key >= '0' && key <= '9') || (key >= 'A' && key <= 'F') ||
                           (key >= 'a' && key <= 'f');
    if (hex_digit && shell.color_input.size() < 7) {
      shell.color_input.push_back(static_cast<wchar_t>(key));
      refresh_color_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 羽化半径键入态（Shift+F）：只吃数字/退格/回车/Esc，其余键一律屏蔽。 */
  if (!shell.feather_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.feather_input.clear();
      set_status(shell, L"已取消羽化输入");
      return;
    }
    if (key == VK_BACK) {
      shell.feather_input.pop_back();
      if (shell.feather_input.empty()) {
        set_status(shell, L"已取消羽化输入");
      } else {
        refresh_feather_input_status(shell);
      }
      return;
    }
    if (key == VK_RETURN) {
      commit_feather_input(shell);
      return;
    }
    if (key >= '0' && key <= '9' && shell.feather_input.size() < 3) {
      shell.feather_input.push_back(static_cast<wchar_t>(key));
      refresh_feather_input_status(shell);
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 滤镜面板输入态：与调整面板同一套输入规矩，但目标值是滤镜参数。 */
  if (!shell.filter_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.filter_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      filter_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      /* 键入 + 回车 = 落值并立刻应用，少一次按键。 */
      filter_commit_input(shell);
      apply_filter(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.filter_input.clear();
      filter_focus_row(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      filter_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      filter_begin_input(shell, L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      filter_begin_input(shell, L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /* 滤镜面板打开时，键盘归面板：Esc 只关面板（不关窗），Ctrl 组合仍可穿透。 */
  if (shell.filter_open && !ctrl) {
    switch (key) {
      case VK_ESCAPE:
        close_filter_panel(shell);
        return;
      case VK_TAB:
        filter_focus_row(shell, shift ? -1 : 1);
        return;
      case VK_RETURN:
        apply_filter(shell);
        return;
      case VK_OEM_MINUS:
        filter_step_value(shell, -1);
        return;
      case VK_OEM_PLUS:
        filter_step_value(shell, 1);
        return;
      case VK_BACK:
        filter_reset_values(shell);
        return;
      default:
        break;
    }
    if (key >= '0' && key <= '9') {
      filter_begin_input(shell, static_cast<wchar_t>(key));
      return;
    }
    /* 其余按键在面板打开时吞掉：避免按工具字母键把面板留在「另一个工具」上。 */
    return;
  }

  /* 调整面板输入态优先：Esc 取消输入（不关窗）、退格删字符、回车提交、数字/./- 进缓冲。 */
  if (!shell.adj_input.empty()) {
    if (key == VK_ESCAPE) {
      shell.adj_input.clear();
      set_status(shell, L"已取消输入");
      return;
    }
    if (key == VK_BACK) {
      panel_backspace(shell);
      return;
    }
    if (key == VK_RETURN) {
      panel_commit_input(shell);
      return;
    }
    if (key == VK_TAB) {
      shell.adj_input.clear();
      panel_move_param(shell, shift ? -1 : 1);
      return;
    }
    if (key >= '0' && key <= '9') {
      shell.adj_input.push_back(static_cast<wchar_t>(key));
      return;
    }
    if (key == VK_OEM_PERIOD) {
      shell.adj_input.push_back(L'.');
      return;
    }
    if (key == VK_OEM_MINUS) {
      shell.adj_input.push_back(L'-');
      return;
    }
    return; /* 输入态下屏蔽其它快捷键，避免误触发 */
  }

  /*
   * 文字键入态：最优先吃掉字符键，避免 'B' 被当成画笔快捷键、空格被当成抓手。
   * Esc 取消、Enter 提交、Shift+Enter 换行、Backspace 删字符；[ / ] 调字号与字距。
   * Ctrl 组合放行（撤销/打开等仍可用），其余未识别键一律吞掉（模态输入规矩）。
   */
  if (shell.type_active) {
    if (key == VK_ESCAPE) {
      type_cancel(shell);
      set_status(shell, L"已取消文字输入（未产生撤销步）");
      return;
    }
    if (key == VK_RETURN) {
      if (shift) {
        shell.type_text.push_back('\n');
        type_refresh_status(shell);
      } else {
        type_commit(shell);
      }
      return;
    }
    if (key == VK_BACK) {
      if (!shell.type_text.empty()) shell.type_text.pop_back();
      type_refresh_status(shell);
      return;
    }
    if (key == VK_OEM_4 || key == VK_OEM_6) {
      type_step_param(shell, key == VK_OEM_4 ? -1 : 1, shift);
      return;
    }
    if (!ctrl) {
      const char typed = comp_ui::type_ascii_from_vk(static_cast<int>(key), shift);
      if (typed != 0) {
        shell.type_text.push_back(typed);
        type_refresh_status(shell);
      }
      return; /* 键入态下屏蔽其它快捷键，避免误触发 */
    }
  }

  if (key == VK_ESCAPE) {
    /* 钢笔有未提交路径时 Esc 只取消路径，不关窗（与输入态的 Esc 同规矩）。 */
    if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
      pen_cancel(shell);
      return;
    }
    /* 形状拖拽中 Esc 只取消这一笔预览，不关窗。 */
    if (shell.shape_active) {
      shape_cancel(shell);
      set_status(shell, L"已取消形状拖拽（未产生撤销步）");
      return;
    }
    PostMessageW(window, WM_CLOSE, 0, 0);
    return;
  }
  if (ctrl) {
    switch (key) {
      case 'A':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjAdd);
          return;
        }
        run_shell_command(window, shell, kCmdSelectAll);
        return;
      case 'I':
        /* Ctrl+Shift+I 反选（与上游一致）；单独 Ctrl+I 不做绑定。 */
        if (shift) {
          run_shell_command(window, shell, kCmdSelectInvert);
          return;
        }
        break;
      case 'U':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjApply);
          return;
        }
        break;
      case 'K':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjKindNext);
          return;
        }
        break;
      case 'J':
        if (shift) {
          run_shell_command(window, shell, kCmdAdjKindPrev);
          return;
        }
        break;
      case 'D':
        /* Ctrl+Shift+D 删除选中的调整；Ctrl+D 取消选区。 */
        if (shift) {
          run_shell_command(window, shell, kCmdAdjRemove);
          return;
        }
        run_shell_command(window, shell, kCmdSelectNone);
        return;
      case 'Z':
        /* Ctrl+Shift+Z 与 Ctrl+Y 同为重做，兼容两种习惯。 */
        run_shell_command(window, shell, shift ? kCmdRedo : kCmdUndo);
        return;
      case 'Y':
        run_shell_command(window, shell, kCmdRedo);
        return;
      case 'N':
        /* Ctrl+Shift+N 新建图层（沿用图像编辑器惯例），Ctrl+N 新建文档。 */
        run_shell_command(window, shell, shift ? kCmdLayerAdd : kCmdNew);
        return;
      case 'E':
        /* Ctrl+E 向下合并（对齐上游 ⌘E）。 */
        run_shell_command(window, shell, kCmdLayerMergeDown);
        return;
      case 'O':
        run_shell_command(window, shell, kCmdOpen);
        return;
      case 'S':
        run_shell_command(window, shell, kCmdSave);
        return;
      case 'V':
        /* V 已是 Move 工具的键位，图层显隐改走 Ctrl+Shift+V。 */
        if (shift) {
          run_shell_command(window, shell, kCmdLayerToggleVisible);
          return;
        }
        break;
      case '0':
        run_shell_command(window, shell, kCmdViewFit);
        return;
      case '1':
        run_shell_command(window, shell, kCmdViewActual);
        return;
      case VK_OEM_PLUS:
        run_shell_command(window, shell, kCmdViewZoomIn);
        return;
      case VK_OEM_MINUS:
        run_shell_command(window, shell, kCmdViewZoomOut);
        return;
      case VK_OEM_4: /* Ctrl+[ = 图层不透明度 -10% */
        run_shell_command(window, shell, kCmdLayerOpacityDown);
        return;
      case VK_OEM_6: /* Ctrl+] = 图层不透明度 +10% */
        run_shell_command(window, shell, kCmdLayerOpacityUp);
        return;
      default:
        break;
    }
  }
  switch (key) {
    case VK_UP:
      run_shell_command(window, shell, kCmdLayerSelectUp);
      break;
    case VK_DOWN:
      run_shell_command(window, shell, kCmdLayerSelectDown);
      break;
    case VK_OEM_4: /* [ = 笔刷变小；Shift+[ = 硬度降低；Wand = 容差 -；Type = 字号/字距 - */
      if (shell.tool == comp_ui::ToolId::Type) {
        type_step_param(shell, -1, shift);
      } else if (shell.tool == comp_ui::ToolId::Wand) {
        shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, -1);
        wchar_t buffer[160];
        wsprintfW(buffer, L"魔棒容差 %d（窗口 %d×%d，逐通道，alpha 参与）", shell.wand_tolerance,
                  shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1);
        set_status(shell, buffer);
      } else {
        brush_step(shell, -1, shift);
      }
      break;
    case VK_OEM_6: /* ] = 笔刷变大；Shift+] = 硬度提高；Wand = 容差 +；Type = 字号/字距 + */
      if (shell.tool == comp_ui::ToolId::Type) {
        type_step_param(shell, 1, shift);
      } else if (shell.tool == comp_ui::ToolId::Wand) {
        shell.wand_tolerance = comp_ui::step_wand_tolerance(shell.wand_tolerance, 1);
        wchar_t buffer[160];
        wsprintfW(buffer, L"魔棒容差 %d（窗口 %d×%d，逐通道，alpha 参与）", shell.wand_tolerance,
                  shell.wand_radius * 2 + 1, shell.wand_radius * 2 + 1);
        set_status(shell, buffer);
      } else {
        brush_step(shell, 1, shift);
      }
      break;
    case VK_TAB:
      panel_move_param(shell, shift ? -1 : 1);
      break;
    case VK_RETURN:
      /* 钢笔：回车 = 提交闭合路径；其余工具里回车仍是调整面板的落值键。 */
      if (shell.tool == comp_ui::ToolId::Pen && !shell.pen_anchors.empty()) {
        commit_pen_path(shell);
      } else {
        panel_commit_input(shell);
      }
      break;
    case VK_OEM_COMMA:
      panel_select_adjustment(shell, -1);
      break;
    case VK_OEM_PERIOD:
      panel_select_adjustment(shell, 1);
      break;
    case VK_OEM_MINUS:
      panel_step_value(shell, -1);
      break;
    case VK_OEM_PLUS:
      panel_step_value(shell, 1);
      break;
    default:
      /* 数字键在调整面板里开始键入数值；其余字母按上游工具键位映射。 */
      if (key >= '0' && key <= '9') {
        panel_begin_input(shell, static_cast<wchar_t>(key));
        break;
      }
      /* X = 下一项色板；Shift+X = 键入十六进制。前景色是画笔与吸管的公共落点。 */
      if (key == 'X' && !ctrl) {
        if (shift) {
          begin_color_input(shell);
          break;
        }
        shell.palette_index = comp_ui::step_swatch(shell.palette_index, 1);
        shell.foreground = comp_ui::swatch_at(shell.palette_index);
        char hex[8] = {0};
        comp_ui::color_to_hex(shell.foreground, hex);
        wchar_t buffer[160];
        wsprintfW(buffer, L"前景色：%s（色板 %d/%d，Shift+X 键入十六进制）",
                  widen_ascii(hex).c_str(), shell.palette_index + 1,
                  comp_ui::swatch_count());
        set_status(shell, buffer);
        break;
      }
      /* Shift+F = 键入新选区羽化半径；M = 框选形状循环；Shift+W = 魔棒取样来源。 */
      if (key == 'F' && shift) {
        begin_feather_input(shell);
        break;
      }
      if (key == 'M' && !ctrl) {
        if (shell.tool == comp_ui::ToolId::Marquee) {
          shell.marquee_shape = comp_ui::marquee_shape_next(shell.marquee_shape);
          wchar_t buffer[160];
          wsprintfW(buffer, L"框选形状：%s（再按 M 切换）",
                    widen_ascii(comp_ui::marquee_shape_name(shell.marquee_shape)).c_str());
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Marquee);
        }
        break;
      }
      if (key == 'W' && shift && !ctrl) {
        if (shell.tool == comp_ui::ToolId::Wand) {
          shell.wand_merged = !shell.wand_merged;
          set_status(shell, shell.wand_merged
                                ? L"魔棒取样：合并图层（Shift+W 切回当前图层）"
                                : L"魔棒取样：当前图层（Shift+W 切到合并）");
        } else {
          set_tool(shell, comp_ui::ToolId::Wand);
        }
        break;
      }
      if (key == 'G' && !ctrl) {
        /* 重复按 G 在同一工具内循环 Linear -> Radial（对齐上游的同类工具习惯）。 */
        if (shell.tool == comp_ui::ToolId::Gradient) {
          shell.gradient_type = comp_ui::next_gradient_type(shell.gradient_type);
          wchar_t buffer[192];
          wsprintfW(buffer, L"渐变类型：%s（再按 G 切换；Shift = 反向 前景↔透明）",
                    widen_ascii(comp_ui::gradient_type_name(shell.gradient_type)).c_str());
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Gradient);
        }
        break;
      }
      if (key == 'U' && !ctrl) {
        /* 重复按 U 在同一工具内循环 Rectangle -> Ellipse -> Line（与 M/G/R 同款习惯）。 */
        if (shell.tool == comp_ui::ToolId::Shape) {
          shell.shape_kind = comp_ui::next_shell_shape_kind(shell.shape_kind);
          wchar_t buffer[224];
          wsprintfW(buffer, L"形状种类：%s（再按 U 切换；%s）",
                    widen_ascii(comp_ui::shape_kind_name(shell.shape_kind)).c_str(),
                    shell.shape_kind == comp_ui::kShapeLine
                        ? L"Alt+[ / ] 调线宽"
                        : L"Shift+Alt+[ / ] 调圆角，Alt+[ / ] 调线宽");
          set_status(shell, buffer);
        } else {
          set_tool(shell, comp_ui::ToolId::Shape);
        }
        break;
      }
      if (key < 128) {
        const comp_ui::ToolId id =
            comp_ui::tool_shortcut_target(shell.tool, static_cast<char>(key));
        if (id == comp_ui::ToolId::Retouch) {
          /* 重复按 R 在同一工具内循环 Blur -> Smudge -> Liquify（对齐上游）。 */
          set_retouch_mode(shell, shell.tool == comp_ui::ToolId::Retouch
                                      ? comp_ui::next_retouch_mode(shell.retouch_mode)
                                      : comp_ui::kRetouchBlur);
        } else if (id != comp_ui::ToolId::Count) {
          set_tool(shell, id);
        }
      }
      break;
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  ShellState* shell = g_shell;
  const int mouse_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
  const int mouse_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
  switch (message) {
    case WM_CREATE:
      return 0;
    case WM_PAINT:
      if (shell != nullptr) present(window, *shell);
      break;
    case WM_KEYDOWN:
      if (shell != nullptr) {
        /* 文字键入态下空格是字符，不是临时抓手。 */
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wparam == VK_SPACE && !(shell->type_active && !ctrl)) {
          /* 空格 = 临时抓手：按下不重复触发，松开才结束。 */
          if (!shell->space_down) {
            shell->space_down = true;
            set_status(*shell, L"按住空格拖动画布（或中键拖动）");
            InvalidateRect(window, nullptr, FALSE);
          }
          break;
        }
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        handle_key(window, *shell, wparam, ctrl, shift);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_KEYUP:
      if (shell != nullptr && wparam == VK_SPACE) {
        shell->space_down = false;
        end_pan(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_LBUTTONDOWN:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          const int button =
              comp_ui::tool_button_at(layout.toolbar, comp_ui::tool_count(), mouse_x, mouse_y);
          if (button >= 0) {
            const comp_ui::ToolInfo* info = comp_ui::tool_info_at(button);
            if (info != nullptr) set_tool(*shell, info->id);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          const int row = comp_ui::layer_row_at(
              layout.layer_panel, static_cast<int>(shell->layers.size()), mouse_x, mouse_y);
          if (row >= 0) {
            shell->selected_layer = row;
            refresh_adjustments(*shell);
            const LayerRow* selected = selected_row(*shell);
            if (selected != nullptr) {
              wchar_t buffer[128];
              wsprintfW(buffer, L"已选中图层 #%I64u（第 %d 行）",
                        static_cast<unsigned long long>(selected->id), row + 1);
              set_status(*shell, buffer);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
          if (shell->filter_open && layout.adjust_panel.w > 0) {
            /* 点参数行 = 聚焦到该行；行几何与绘制共用 filter_param_row_rect。 */
            const int row = comp_ui::filter_param_row_at(
                layout.adjust_panel, shell->filter.param_count(), mouse_x, mouse_y);
            if (row >= 0) {
              shell->filter_input.clear();
              filter_focus_row(*shell, row - shell->filter.focused);
              InvalidateRect(window, nullptr, FALSE);
              return 0;
            }
          }
          if (layout.canvas.contains(mouse_x, mouse_y)) {
            if (shell->tool == comp_ui::ToolId::Hand || shell->space_down) {
              begin_pan(window, *shell, mouse_x, mouse_y);
            } else if (pen_available(*shell)) {
              pen_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (marquee_available(*shell)) {
              marquee_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (lasso_available(*shell)) {
              lasso_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (wand_available(*shell)) {
              wand_click(window, *shell, mouse_x, mouse_y, layout);
            } else if (gradient_available(*shell)) {
              gradient_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (shape_available(*shell)) {
              shape_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (type_available(*shell)) {
              type_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (retouch_stroke_available(*shell)) {
              retouch_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (paint_stroke_available(*shell)) {
              paint_stroke_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (eyedropper_available(*shell)) {
              eyedropper_begin(window, *shell, mouse_x, mouse_y, layout);
            } else if (move_available(*shell)) {
              move_begin(window, *shell, mouse_x, mouse_y, layout);
            } else {
              canvas_click(*shell, mouse_x, mouse_y, layout);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          }
        }
      }
      break;
    case WM_MBUTTONDOWN:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout) && layout.canvas.contains(mouse_x, mouse_y)) {
          begin_pan(window, *shell, mouse_x, mouse_y);
          return 0;
        }
      }
      break;
    case WM_LBUTTONUP:
      if (shell != nullptr && shell->pen_dragging) {
        pen_up(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->gradient_active) {
        gradient_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->shape_active) {
        shape_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->marquee_active) {
        marquee_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->lasso_active) {
        lasso_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->move_active) {
        move_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->eyedropper_drag) {
        shell->eyedropper_drag = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->stroke_kind == comp_ui::StrokeKind::Retouch) {
        retouch_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr && shell->stroke_kind == comp_ui::StrokeKind::Paint) {
        paint_stroke_end(window, *shell);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (shell != nullptr) end_pan(window, *shell);
      break;
    case WM_MBUTTONUP:
      if (shell != nullptr) end_pan(window, *shell);
      break;
    case WM_KILLFOCUS:
      /* 失焦后收不到 KEYUP / MOUSEUP：必须主动清状态，否则会「粘住」。 */
      if (shell != nullptr) {
        shell->space_down = false;
        end_pan(window, *shell);
        stroke_cancel(*shell);
        move_cancel(*shell);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
        lasso_cancel(*shell);
        pen_cancel_drag(*shell);
        gradient_cancel(*shell);
        shape_cancel(*shell);
        type_cancel(*shell);
      }
      break;
    case WM_CAPTURECHANGED:
      if (shell != nullptr) {
        end_pan(window, *shell);
        stroke_cancel(*shell);
        move_cancel(*shell);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
        lasso_cancel(*shell);
        pen_cancel_drag(*shell);
        gradient_cancel(*shell);
        shape_cancel(*shell);
      }
      break;
    case WM_MOUSEMOVE:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          if (shell->move_active) {
            move_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->pen_dragging) {
            pen_drag(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->gradient_active) {
            gradient_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->shape_active) {
            shape_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->eyedropper_drag) {
            /* 吸管拖动连续取样：只更新前景色与状态栏，不改画布内容。 */
            eyedropper_sample(*shell, mouse_x, mouse_y, layout);
          } else if (shell->marquee_active) {
            marquee_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->lasso_active) {
            lasso_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (stroke_in_progress(*shell)) {
            stroke_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->panning) {
            shell->view.pan_by(static_cast<double>(mouse_x - shell->pan_anchor.x),
                               static_cast<double>(mouse_y - shell->pan_anchor.y));
            shell->pan_anchor.x = mouse_x;
            shell->pan_anchor.y = mouse_y;
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        /*
         * 笔尖轮廓跟随光标：只在笔刷类工具下刷新，避免每帧重画。
         * WM_MOUSELEAVE 是一次性的，收到后要重新 Track 才能再收到。
         */
        if (comp_ui::tool_has_brush_tip(shell->tool)) {
          shell->cursor_x = mouse_x;
          shell->cursor_y = mouse_y;
          shell->cursor_valid = true;
          if (!shell->tracking_leave) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            shell->tracking_leave = true;
          }
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      break;
    case WM_MOUSELEAVE:
      if (shell != nullptr) {
        shell->cursor_valid = false;
        shell->tracking_leave = false;
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_TIMER:
      /* 蚂蚁线相位：只在有选区时跑，避免空转重绘。 */
      if (shell != nullptr && wparam == kAntsTimerId) {
        if (shell->selection_active) {
          shell->selection_phase = (shell->selection_phase + 1) % kAntsPhasePeriod;
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      break;
    case WM_SYSKEYDOWN:
      /*
       * Alt+[ / Alt+]：模糊模式调半径，涂抹/液化模式调强度。
       * 普通 [ ] 已用于笔刷，Ctrl+[ ] 用于图层不透明度，因此放到 Alt 组合，
       * 且必须在这里吞掉，否则 DefWindowProc 会响铃。
       */
      if (shell != nullptr && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
        const int direction = wparam == VK_OEM_6 ? 1 : -1;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        wchar_t buffer[128];
        if (shell->tool == comp_ui::ToolId::Wand) {
          /* S8：魔棒的取样窗口是 (2r+1)²，0 = 只取种子像素（S7 行为）。 */
          shell->wand_radius = comp_ui::step_wand_radius(shell->wand_radius, direction);
          wsprintfW(buffer, L"魔棒取样窗口 %d×%d（半径 %d px）",
                    shell->wand_radius * 2 + 1, shell->wand_radius * 2 + 1, shell->wand_radius);
        } else if (shell->tool == comp_ui::ToolId::Shape && shift &&
                   shell->shape_kind == comp_ui::kShapeRect) {
          /* S10：Shift+Alt+[ / ] 调矩形圆角半径（0 = 直角；引擎再夹到短边一半）。 */
          shell->shape_corner_radius =
              comp_ui::step_shape_corner_radius(shell->shape_corner_radius, direction);
          wsprintfW(buffer, L"矩形圆角半径 %d px（0 = 直角）",
                    static_cast<int>(shell->shape_corner_radius + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Shape) {
          /* S10：Alt+[ / ] 调形状描边宽度（直线用；矩形/椭圆的外壳默认纯填充）。 */
          shell->shape_stroke_width =
              comp_ui::step_shape_stroke_width(shell->shape_stroke_width, direction);
          wsprintfW(buffer, L"形状描边宽度 %d px（直线描边用）",
                    static_cast<int>(shell->shape_stroke_width + 0.5));
        } else if (shell->tool == comp_ui::ToolId::Retouch &&
                   shell->retouch_mode != comp_ui::kRetouchBlur) {
          shell->retouch_strength =
              comp_ui::step_retouch_strength(shell->retouch_strength, direction);
          wsprintfW(buffer, L"%s强度 %d%%",
                    widen_ascii(comp_ui::retouch_mode_name(shell->retouch_mode)).c_str(),
                    static_cast<int>(shell->retouch_strength * 100.0 + 0.5));
        } else {
          shell->blur_radius = comp_ui::step_blur_radius(shell->blur_radius, direction);
          wsprintfW(buffer, L"模糊半径 %d px",
                    static_cast<int>(shell->blur_radius + 0.5));
        }
        set_status(*shell, buffer);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
      if (shell != nullptr) {
        /* WM_MOUSEWHEEL 的坐标是屏幕坐标，先换算到客户区。 */
        POINT cursor{mouse_x, mouse_y};
        ScreenToClient(window, &cursor);
        Layout layout{};
        if (client_layout(*shell, &layout) && layout.canvas.contains(cursor.x, cursor.y)) {
          const int delta = static_cast<short>(HIWORD(wparam));
          if (delta != 0) {
            const double factor = delta > 0 ? 1.2 : 1.0 / 1.2;
            shell->view.zoom_by(factor, cursor.x, cursor.y, layout.canvas);
            wchar_t buffer[96];
            wsprintfW(buffer, L"缩放 %d%%",
                      static_cast<int>(shell->view.zoom * 100.0 + 0.5));
            set_status(*shell, buffer);
            InvalidateRect(window, nullptr, FALSE);
          }
        }
        return 0;
      }
      break;
    case WM_COMMAND:
      /* lparam != 0 表示控件通知，本骨架无子控件，直接忽略。 */
      if (shell != nullptr && lparam == 0) {
        if (LOWORD(wparam) >= kCmdToolBase &&
            LOWORD(wparam) < kCmdToolBase + comp_ui::tool_count()) {
          const comp_ui::ToolInfo* info =
              comp_ui::tool_info_at(static_cast<int>(LOWORD(wparam) - kCmdToolBase));
          if (info != nullptr) set_tool(*shell, info->id);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        run_shell_command(window, *shell, LOWORD(wparam));
        return 0;
      }
      break;
    case WM_SIZE:
      if (shell != nullptr) InvalidateRect(window, nullptr, FALSE);
      break;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/* 菜单结构：文件 / 编辑 / 图层 / 视图 / 工具 / 调整；快捷键提示文本与 handle_key
 * 的映射保持一致，工具菜单直接取自 ui_core.h 的 tool_table。 */
HMENU build_menu() {
  HMENU menu = CreateMenu();

  HMENU file_menu = CreatePopupMenu();
  AppendMenuW(file_menu, MF_STRING, kCmdNew, L"新建(&N)\tCtrl+N");
  AppendMenuW(file_menu, MF_STRING, kCmdOpen, L"打开(&O)...\tCtrl+O");
  AppendMenuW(file_menu, MF_STRING, kCmdSave, L"保存(&S)...\tCtrl+S");
  AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file_menu, MF_STRING, kCmdExit, L"退出(&X)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"文件(&F)");

  HMENU edit_menu = CreatePopupMenu();
  AppendMenuW(edit_menu, MF_STRING, kCmdUndo, L"撤销(&U)\tCtrl+Z");
  AppendMenuW(edit_menu, MF_STRING, kCmdRedo, L"重做(&R)\tCtrl+Y");
  AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(edit_menu, MF_STRING, kCmdSelectAll, L"全选(&A)\tCtrl+A");
  AppendMenuW(edit_menu, MF_STRING, kCmdSelectNone, L"取消选择(&D)\tCtrl+D");
  AppendMenuW(edit_menu, MF_STRING, kCmdSelectInvert, L"反选(&I)\tCtrl+Shift+I");
  AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(edit_menu, MF_STRING, kCmdListCommands, L"枚举引擎命令(&L)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_menu), L"编辑(&E)");

  HMENU layer_menu = CreatePopupMenu();
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerAdd, L"新建图层(&A)\tCtrl+Shift+N");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMergeDown, L"向下合并(&M)\tCtrl+E");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlatten, L"拼合图像(&F)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectUp, L"选择上一图层(&U)\t↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectDown, L"选择下一图层(&D)\t↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleVisible, L"切换可见性(&V)\tCtrl+Shift+V");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityDown, L"降低不透明度(&-)\tCtrl+[");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityUp, L"提高不透明度(&+)\tCtrl+]");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_menu), L"图层(&L)");

  HMENU view_menu = CreatePopupMenu();
  AppendMenuW(view_menu, MF_STRING, kCmdViewFit, L"适合窗口(&F)\tCtrl+0");
  AppendMenuW(view_menu, MF_STRING, kCmdViewActual, L"实际像素(&1)\tCtrl+1");
  AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view_menu, MF_STRING, kCmdViewZoomIn, L"放大(&I)\tCtrl+=");
  AppendMenuW(view_menu, MF_STRING, kCmdViewZoomOut, L"缩小(&O)\tCtrl+-");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"视图(&V)");

  /* 工具菜单与工具条、快捷键共用同一张表（tool_table），不另抄一份键位。 */
  HMENU tool_menu = CreatePopupMenu();
  for (int i = 0; i < comp_ui::tool_count(); ++i) {
    const comp_ui::ToolInfo* info = comp_ui::tool_info_at(i);
    if (info == nullptr) continue;
    wchar_t label[128];
    wsprintfW(label, L"%s\t%c", widen_ascii(info->name).c_str(),
              info->shortcut != 0 ? static_cast<wchar_t>(info->shortcut - 'a' + 'A') : L' ');
    AppendMenuW(tool_menu, MF_STRING, kCmdToolBase + static_cast<UINT>(i), label);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tool_menu), L"工具(&T)");

  HMENU adjust_menu = CreatePopupMenu();
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjKindPrev, L"上一类型(&P)\tCtrl+Shift+J");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjKindNext, L"下一类型(&N)\tCtrl+Shift+K");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRowPrev, L"上一调整(&E)\t,");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRowNext, L"下一调整(&X)\t.");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjParamPrev, L"上一参数(&R)\tShift+Tab");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjParamNext, L"下一参数(&T)\tTab");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjValueDown, L"参数减(&-)\t-");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjValueUp, L"参数加(&+)\t=");
  AppendMenuW(adjust_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjAdd, L"新增调整(&A)\tCtrl+Shift+A");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjApply, L"应用到选中调整(&U)\tCtrl+Shift+U");
  AppendMenuW(adjust_menu, MF_STRING, kCmdAdjRemove, L"删除选中调整(&D)\tCtrl+Shift+D");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(adjust_menu), L"调整(&J)");

  /* 滤镜菜单与面板参数表共用 ui_core.h 的 filter_table（量程与 comp_filter.h 对齐）。 */
  HMENU filter_menu = CreatePopupMenu();
  for (int i = 0; i < comp_ui::filter_count(); ++i) {
    const comp_ui::FilterSpec* spec = comp_ui::filter_spec_at(i);
    if (spec == nullptr) continue;
    wchar_t label[128];
    wsprintfW(label, L"%s", widen_ascii(spec->name).c_str());
    AppendMenuW(filter_menu, MF_STRING, kCmdFilterBase + static_cast<UINT>(i), label);
  }
  AppendMenuW(filter_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterApply, L"应用(&A)\t回车");
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterReset, L"重置参数(&R)");
  AppendMenuW(filter_menu, MF_STRING, kCmdFilterClose, L"关闭面板(&C)\tEsc");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(filter_menu), L"滤镜(&F)");

  return menu;
}

/* ------------------------------------------------------------ 无窗口自检 */

const char* backend_name(comp_backend backend) {
  switch (backend) {
    case COMP_BACKEND_METAL:  return "METAL";
    case COMP_BACKEND_D3D12:  return "D3D12";
    case COMP_BACKEND_VULKAN: return "VULKAN";
    case COMP_BACKEND_CPU:    return "CPU";
    default:                  return "AUTO";
  }
}

/*
 * 端到端自检：只用 C ABI 串起「建引擎 -> 建文档 -> 加图层写像素 -> 合成 -> 撤销 ->
 * .comp 往返」。不创建窗口，故可无人值守运行；结论写入 out_path 并回退出码（0 = 全通过）。
 */
int run_selftest(const wchar_t* out_path) {
  FILE* report = nullptr;
  if (_wfopen_s(&report, out_path, L"wb") != 0 || report == nullptr) return 2;

  int failures = 0;
  const auto emit = [&](const char* text) { std::fprintf(report, "%s\n", text); };
  const auto verify = [&](bool ok, const char* label) {
    std::fprintf(report, "[%s] %s\n", ok ? "OK" : "FAIL", label);
    if (!ok) ++failures;
  };

  emit("# compositor-shell selftest (Windows shell + engine C ABI)");

  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-selfcheck";
  engine_desc.version = "0.1.0";
  engine_desc.backend = COMP_BACKEND_AUTO;
  comp_engine engine = nullptr;
  const bool engine_ok =
      comp_engine_create(&engine_desc, &engine) == COMP_OK && engine != nullptr;
  verify(engine_ok, "comp_engine_create(AUTO)");
  if (!engine_ok) {
    std::fclose(report);
    return 1;
  }

  comp_backend backend = COMP_BACKEND_AUTO;
  const bool backend_ok = comp_engine_backend(engine, &backend) == COMP_OK;
  verify(backend_ok, "comp_engine_backend()");
  std::fprintf(report, "[info] backend = %s\n", backend_ok ? backend_name(backend) : "(query failed)");

  comp_document_desc doc_desc{};
  doc_desc.width = 64;
  doc_desc.height = 64;
  doc_desc.dpi = 72.0f;
  doc_desc.color_space = "sRGB";
  comp_document document = nullptr;
  const bool doc_ok =
      comp_document_create(engine, &doc_desc, &document) == COMP_OK && document != nullptr;
  verify(doc_ok, "comp_document_create(64x64)");
  if (!doc_ok) {
    comp_engine_destroy(engine);
    std::fclose(report);
    return 1;
  }

  comp_size size{};
  verify(comp_document_size(document, &size) == COMP_OK && size.width == 64 && size.height == 64,
         "comp_document_size() == 64x64");

  /* 半透明红（预乘 RGBA8）：验证「壳只调 ABI、像素由引擎处理」的端到端路径。 */
  const int32_t width = 64;
  const int32_t height = 64;
  const size_t stride = static_cast<size_t>(width) * 4u;
  std::vector<uint8_t> pixels(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = 128;
    pixels[i + 1] = 0;
    pixels[i + 2] = 0;
    pixels[i + 3] = 128;
  }
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = stride;
  image.pixels = pixels.data();

  comp_layer_desc layer_desc{};
  layer_desc.name = "selftest-layer";
  layer_desc.blend = COMP_BLEND_NORMAL;
  layer_desc.opacity = 1.0f;
  layer_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  comp_layer_id layer = 0;
  const bool add_ok = comp_layer_add(document, &layer_desc, &layer) == COMP_OK && layer != 0;
  verify(add_ok, "comp_layer_add()");
  verify(add_ok && comp_layer_set_pixels(document, layer, &image) == COMP_OK,
         "comp_layer_set_pixels()");

  size_t layer_count = 0;
  const bool count_ok = comp_layer_count(document, &layer_count) == COMP_OK && layer_count >= 2;
  verify(count_ok, "comp_layer_count() >= 2 (background + added)");
  std::fprintf(report, "[info] layers = %u\n", static_cast<unsigned>(layer_count));

  std::vector<uint8_t> flattened(stride * static_cast<size_t>(height), 0);
  const bool flatten_ok =
      comp_render_flatten(document, flattened.data(), COMP_PIXEL_RGBA8, stride) == COMP_OK;
  verify(flatten_ok, "comp_render_flatten()");
  size_t non_transparent = 0;
  for (size_t i = 3; i < flattened.size(); i += 4) {
    if (flattened[i] != 0) ++non_transparent;
  }
  verify(flatten_ok && non_transparent > 0, "flatten output has non-transparent pixels");
  std::fprintf(report, "[info] non-transparent pixels = %u / %u\n",
               static_cast<unsigned>(non_transparent), static_cast<unsigned>(width * height));

  /* 撤销/重做由引擎维护，外壳只触发与展示。 */
  int32_t can_undo = 0;
  comp_document_can_undo(document, &can_undo);
  size_t undo_depth = 0;
  size_t redo_depth = 0;
  comp_document_history_stats(document, &undo_depth, &redo_depth);
  std::fprintf(report, "[info] history: can_undo=%d depth=%u redo=%u\n", can_undo,
               static_cast<unsigned>(undo_depth), static_cast<unsigned>(redo_depth));
  verify(can_undo == 0 || comp_document_undo(document) == COMP_OK, "comp_document_undo()");

  /* .comp 往返：写入临时目录再读回，核对尺寸。 */
  wchar_t temp_dir[MAX_PATH] = {0};
  wchar_t temp_file[MAX_PATH] = {0};
  if (GetTempPathW(MAX_PATH, temp_dir) != 0) {
    wsprintfW(temp_file, L"%scompositor_shell_selftest.comp", temp_dir);
  } else {
    wsprintfW(temp_file, L"compositor_shell_selftest.comp");
  }
  char comp_path_utf8[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, temp_file, -1, comp_path_utf8,
                      static_cast<int>(sizeof(comp_path_utf8)), nullptr, nullptr);
  const bool save_ok = comp_document_save(document, comp_path_utf8) == COMP_OK;
  verify(save_ok, "comp_document_save(.comp)");
  if (save_ok) {
    comp_document reloaded = nullptr;
    const bool load_ok = comp_document_load(engine, comp_path_utf8, &reloaded) == COMP_OK &&
                         reloaded != nullptr;
    verify(load_ok, "comp_document_load(.comp round trip)");
    if (load_ok) {
      comp_size reloaded_size{};
      verify(comp_document_size(reloaded, &reloaded_size) == COMP_OK &&
                 reloaded_size.width == size.width && reloaded_size.height == size.height,
             "round trip keeps canvas size");
      comp_document_destroy(reloaded);
    }
  }

  /*
   * 调整面板链路（面板真正走的接口序列）：
   * 键名表元数据 -> 引擎组装 settings JSON -> 新增 -> 读回 -> 按索引更新 -> 非法值被拒。
   */
  const char* kind_name = nullptr;
  verify(comp_layer_adjustment_kind_name(3u, &kind_name) == COMP_OK && kind_name != nullptr &&
             std::strcmp(kind_name, "Exposure") == 0,
         "comp_layer_adjustment_kind_name(3) == Exposure");

  size_t param_count = 0;
  verify(comp_layer_adjustment_param_count(3u, &param_count) == COMP_OK && param_count == 3,
         "comp_layer_adjustment_param_count(3) == 3");

  comp_adjustment_param first_param{};
  verify(comp_layer_adjustment_param_at(3u, 0u, &first_param) == COMP_OK &&
             first_param.json_path != nullptr &&
             std::strcmp(first_param.json_path, "exposureSettings.exposure") == 0,
         "comp_layer_adjustment_param_at(3,0) path");

  const char* panel_values[3] = {"1.5", "0.25", "2.2"};
  size_t settings_size = 0;
  verify(comp_layer_adjustment_build_settings(3u, panel_values, 3, nullptr, 0, &settings_size) ==
                 COMP_OK &&
             settings_size > 0,
         "comp_layer_adjustment_build_settings(size probe)");
  std::vector<char> settings(settings_size + 1, '\0');
  const bool build_ok = comp_layer_adjustment_build_settings(
      3u, panel_values, 3, settings.data(), settings.size(), &settings_size) == COMP_OK;
  verify(build_ok, "comp_layer_adjustment_build_settings");
  std::fprintf(report, "[info] panel settings = %s\n", settings.data());

  const bool add_adj_ok =
      add_ok && build_ok && comp_layer_add_adjustment(document, layer, 3u, settings.data()) == COMP_OK;
  verify(add_adj_ok, "comp_layer_add_adjustment(panel settings)");

  size_t adj_count = 0;
  verify(add_adj_ok && comp_layer_adjustment_count(document, layer, &adj_count) == COMP_OK &&
             adj_count == 1,
         "comp_layer_adjustment_count() == 1");

  double exposure = 0.0;
  verify(comp_layer_adjustment_read_value(settings.data(), "exposureSettings.exposure", &exposure,
                                          nullptr) == COMP_OK &&
             std::fabs(exposure - 1.5) < 1e-9,
         "comp_layer_adjustment_read_value(exposure) == 1.5");

  const char* updated_values[3] = {"-2", "", "1.8"};
  std::vector<char> updated(256, '\0');
  size_t updated_size = 0;
  const bool update_ok =
      comp_layer_adjustment_build_settings(3u, updated_values, 3, updated.data(), updated.size(),
                                           &updated_size) == COMP_OK &&
      comp_layer_update_adjustment_at(document, layer, 0u, 3u, updated.data()) == COMP_OK;
  verify(update_ok, "comp_layer_update_adjustment_at(0)");

  double gamma = 0.0;
  verify(comp_layer_adjustment_read_value(updated.data(), "exposureSettings.gamma", &gamma, nullptr) ==
             COMP_OK &&
             std::fabs(gamma - 1.8) < 1e-9,
         "updated gamma == 1.8");

  /* 越界面板值必须被拒（面板不会把非法值写进文档）。 */
  const char* bad_values[3] = {"999", "", ""};
  verify(comp_layer_adjustment_build_settings(3u, bad_values, 3, nullptr, 0, &settings_size) != COMP_OK,
         "out-of-range panel value rejected");

  /* 删除接口（面板 Ctrl+Shift+D 走这条）：越界被拒，删除后数量回落。 */
  verify(comp_layer_remove_adjustment(document, layer, 5u) == COMP_ERR_INVALID_ARGUMENT,
         "comp_layer_remove_adjustment(out of range) rejected");
  const bool remove_ok = comp_layer_remove_adjustment(document, layer, 0u) == COMP_OK;
  verify(remove_ok, "comp_layer_remove_adjustment(0)");
  size_t after_remove = 99;
  verify(remove_ok && comp_layer_adjustment_count(document, layer, &after_remove) == COMP_OK &&
             after_remove == 0,
         "comp_layer_adjustment_count() == 0 after remove");

  /*
   * 滤镜面板链路：面板按 ui_core.h 的行序把值搬进 comp_filter_desc，这里走同一条
   * ABI。要点：零初始化 = 恒等（不碰像素、不开事务）；越界在事务之前拒绝；
   * 真应用后一步撤销可逐字节还原。
   */
  /*
   * 滤镜观测必须先有像素：本段之前的 `comp_document_undo()` 已经把 selftest 图层的
   * 半透明红退回空像素，而 Vignette 在 `fillsClear = 0` 下逐像素跳过全透明像素
   * （AdjustPixels.c），因此空图层上「应用成功但字节不变」是正确行为、看不到效果。
   * 重新铺一张不透明灰图，与 tests/test_filters.cpp 的观测口径一致。
   */
  std::vector<uint8_t> opaque(stride * static_cast<size_t>(height), 0);
  for (size_t i = 0; i < opaque.size(); i += 4) {
    opaque[i + 0] = 200;
    opaque[i + 1] = 200;
    opaque[i + 2] = 200;
    opaque[i + 3] = 255;
  }
  comp_image opaque_image{};
  opaque_image.width = width;
  opaque_image.height = height;
  opaque_image.format = COMP_PIXEL_RGBA8;
  opaque_image.stride = stride;
  opaque_image.pixels = opaque.data();
  verify(add_ok && comp_layer_set_pixels(document, layer, &opaque_image) == COMP_OK,
         "comp_layer_set_pixels(opaque gray)");

  std::vector<uint8_t> flatten_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t depth_before = 0;
  comp_document_history_stats(document, &depth_before, &redo_depth);

  comp_filter_desc identity{};
  identity.kind = COMP_FILTER_VIGNETTE; /* 其余字段零初始化 -> amount 0 = 恒等 */
  int32_t changed = -1;
  char filter_note[256] = {0};
  size_t filter_note_size = 0;
  const bool identity_ok =
      comp_filter_apply(document, layer, &identity, 1.0, &changed, filter_note,
                        sizeof(filter_note), &filter_note_size) == COMP_OK &&
      changed == 0;
  verify(identity_ok, "comp_filter_apply(identity) is a no-op");
  size_t depth_after_identity = 0;
  comp_document_history_stats(document, &depth_after_identity, &redo_depth);
  verify(depth_after_identity == depth_before, "identity filter leaves history untouched");

  comp_filter_desc bad_filter{};
  bad_filter.kind = COMP_FILTER_VIGNETTE;
  bad_filter.vignette.amount = 101.0; /* 量程 0..100 */
  verify(add_ok && comp_filter_apply(document, layer, &bad_filter, 1.0, &changed, filter_note,
                                     sizeof(filter_note), &filter_note_size) ==
                      COMP_ERR_INVALID_ARGUMENT,
         "comp_filter_apply(out-of-range amount) rejected");

  comp_filter_desc vignette{};
  vignette.kind = COMP_FILTER_VIGNETTE;
  vignette.vignette.amount = 100.0;
  vignette.vignette.midpoint = 50.0;
  vignette.vignette.roundness = 100.0;
  vignette.vignette.feather = 60.0;
  vignette.vignette.highlights = 25.0;
  vignette.vignette.color[0] = 0.0;
  vignette.vignette.color[1] = 0.0;
  vignette.vignette.color[2] = 0.0;
  changed = 0;
  const bool vignette_ok =
      add_ok && comp_filter_apply(document, layer, &vignette, 1.0, &changed, filter_note,
                                  sizeof(filter_note), &filter_note_size) == COMP_OK &&
      changed == 1;
  verify(vignette_ok, "comp_filter_apply(vignette amount=100) changed pixels");

  std::vector<uint8_t> flatten_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(vignette_ok && flatten_after != flatten_before,
         "vignette changes the flattened output");
  verify(vignette_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(filter step)");
  std::vector<uint8_t> flatten_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, flatten_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(vignette_ok && flatten_restored == flatten_before,
         "filter undo restores pixels byte-for-byte");

  /*
   * 模糊工具（COMP_TOOL_BLUR）：窗口路径是「鼠标折线 -> comp_tool_apply」。
   * 先铺一张棋盘让模糊有内容可抹（纯色图模糊前后字节相同，测不出东西）。
   */
  std::vector<uint8_t> pattern(stride * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const bool red = ((x / 8) + (y / 8)) % 2 == 0;
      uint8_t* pixel = pattern.data() + static_cast<size_t>(y) * stride +
                       static_cast<size_t>(x) * 4u;
      pixel[0] = red ? 255 : 0;
      pixel[1] = 0;
      pixel[2] = red ? 0 : 255;
      pixel[3] = 255;
    }
  }
  comp_image patched{};
  patched.width = width;
  patched.height = height;
  patched.format = COMP_PIXEL_RGBA8;
  patched.stride = stride;
  patched.pixels = pattern.data();
  verify(comp_layer_set_pixels(document, layer, &patched) == COMP_OK,
         "comp_layer_set_pixels(checkerboard)");

  std::vector<uint8_t> blur_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_before.data(), COMP_PIXEL_RGBA8, stride);
  const float stroke[] = {8.0f, 32.0f, 20.0f, 32.0f, 32.0f, 32.0f,
                          44.0f, 32.0f, 56.0f, 32.0f};
  comp_tool_op_desc blur_desc{};
  blur_desc.kind = COMP_TOOL_BLUR;
  blur_desc.stroke_points = stroke;
  blur_desc.stroke_point_count = 5;
  blur_desc.brush.diameter = 24.0f;
  blur_desc.brush.hardness = 0.75f;
  blur_desc.brush.pressure = 1.0f;
  blur_desc.brush.flow = 1.0f;
  blur_desc.blur.radius = 6.0f;
  char tool_note[256] = {0};
  size_t tool_note_size = 0;
  const bool blur_ok =
      comp_tool_apply(document, layer, &blur_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(blur_ok, "comp_tool_apply(COMP_TOOL_BLUR)");
  std::vector<uint8_t> blur_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(blur_ok && blur_after != blur_before, "blur tool changes pixels along the stroke");
  verify(blur_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(blur step)");
  std::vector<uint8_t> blur_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, blur_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(blur_ok && blur_restored == blur_before,
         "blur undo restores pixels byte-for-byte");

  comp_tool_op_desc bad_blur = blur_desc;
  bad_blur.blur.radius = 0.0f; /* 量程 0.5..50 */
  verify(comp_tool_apply(document, layer, &bad_blur, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_BLUR(radius=0) rejected");

  /*
   * 涂抹 / 液化（COMP_TOOL_SMUDGE / LIQUIFY）：与模糊共用同一条「鼠标折线 ->
   * comp_tool_apply」通路，参数换成 warp.strength。strength=0 必须逐字节不动，
   * 否则壳层把 Alt+] 调到 0 时会悄悄改图。
   */
  std::vector<uint8_t> warp_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, warp_before.data(), COMP_PIXEL_RGBA8, stride);

  comp_tool_op_desc smudge_desc{};
  smudge_desc.kind = COMP_TOOL_SMUDGE;
  smudge_desc.stroke_points = stroke;
  smudge_desc.stroke_point_count = 5;
  smudge_desc.brush.diameter = 24.0f;
  smudge_desc.brush.hardness = 0.75f;
  smudge_desc.brush.pressure = 1.0f;
  smudge_desc.brush.flow = 1.0f;
  smudge_desc.warp.strength = 0.5f;
  const bool smudge_ok =
      comp_tool_apply(document, layer, &smudge_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(smudge_ok, "comp_tool_apply(COMP_TOOL_SMUDGE)");
  std::vector<uint8_t> smudge_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, smudge_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(smudge_ok && smudge_after != warp_before, "smudge drags colour along the stroke");
  verify(smudge_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(smudge step)");

  comp_tool_op_desc liquify_desc = smudge_desc;
  liquify_desc.kind = COMP_TOOL_LIQUIFY;
  liquify_desc.warp.strength = 0.0f;
  const bool liquify_zero_ok =
      comp_tool_apply(document, layer, &liquify_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  std::vector<uint8_t> liquify_zero(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, liquify_zero.data(), COMP_PIXEL_RGBA8, stride);
  verify(liquify_zero_ok && liquify_zero == warp_before,
         "COMP_TOOL_LIQUIFY(strength=0) is a byte-identical no-op");

  liquify_desc.warp.strength = 0.8f;
  const bool liquify_ok =
      comp_tool_apply(document, layer, &liquify_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(liquify_ok, "comp_tool_apply(COMP_TOOL_LIQUIFY)");
  std::vector<uint8_t> liquify_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, liquify_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(liquify_ok && liquify_after != warp_before, "liquify moves pixels along the stroke");
  verify(liquify_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(liquify step)");

  /*
   * 渐变（COMP_TOOL_GRADIENT，S9）：窗口路径是「拖出两个端点 -> comp_tool_apply」。
   * 冻结三件事：枚举值、铺色后逐字节可撤销、端点重合被参数闸拒绝且不留撤销步。
   */
  verify(static_cast<int>(COMP_TOOL_GRADIENT) == 13, "COMP_TOOL_GRADIENT stays at 13");

  std::vector<uint8_t> gradient_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t gradient_undo_before = 0;
  size_t gradient_redo = 0;
  comp_document_history_stats(document, &gradient_undo_before, &gradient_redo);

  comp_tool_op_desc gradient_desc{};
  gradient_desc.kind = COMP_TOOL_GRADIENT;
  gradient_desc.gradient.type = 0; /* 线性 */
  gradient_desc.gradient.start_x = 4.0;
  gradient_desc.gradient.start_y = 32.0;
  gradient_desc.gradient.end_x = 60.0;
  gradient_desc.gradient.end_y = 32.0;
  gradient_desc.gradient.color0[0] = 255; /* 第一色标：不透明红 */
  gradient_desc.gradient.color0[3] = 255;
  gradient_desc.gradient.color1[2] = 255; /* 第二色标：不透明蓝 */
  gradient_desc.gradient.color1[3] = 255;
  gradient_desc.gradient.opacity = 1.0f;
  const bool gradient_ok =
      comp_tool_apply(document, layer, &gradient_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(gradient_ok, "comp_tool_apply(COMP_TOOL_GRADIENT)");

  std::vector<uint8_t> gradient_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t left_pixel = static_cast<size_t>(32) * stride + static_cast<size_t>(4) * 4u;
  const size_t right_pixel = static_cast<size_t>(32) * stride + static_cast<size_t>(60) * 4u;
  verify(gradient_ok && gradient_after != gradient_before,
         "gradient tool paints the layer");
  verify(gradient_ok && gradient_after[left_pixel + 0] == 255 &&
             gradient_after[left_pixel + 2] == 0,
         "the gradient starts at the first stop (red)");
  verify(gradient_ok && gradient_after[right_pixel + 2] == 255 &&
             gradient_after[right_pixel + 0] == 0,
         "the gradient ends at the second stop (blue)");
  verify(gradient_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(gradient step)");
  std::vector<uint8_t> gradient_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(gradient_ok && gradient_restored == gradient_before,
         "gradient undo restores pixels byte-for-byte");

  /* 端点重合：参数闸必须在事务之前拒绝，撤销栈与像素都不许动。 */
  size_t gradient_undo_rejected = 0;
  comp_document_history_stats(document, &gradient_undo_rejected, &gradient_redo);
  comp_tool_op_desc bad_gradient = gradient_desc;
  bad_gradient.gradient.end_x = bad_gradient.gradient.start_x;
  bad_gradient.gradient.end_y = bad_gradient.gradient.start_y;
  verify(comp_tool_apply(document, layer, &bad_gradient, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_GRADIENT(coincident endpoints) rejected");
  size_t gradient_undo_after_reject = 0;
  comp_document_history_stats(document, &gradient_undo_after_reject, &gradient_redo);
  verify(gradient_undo_after_reject == gradient_undo_rejected,
         "the rejected gradient leaves no undo step");
  std::vector<uint8_t> gradient_after_reject(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, gradient_after_reject.data(), COMP_PIXEL_RGBA8, stride);
  verify(gradient_after_reject == gradient_restored,
         "the rejected gradient leaves the render byte-identical");

  /*
   * 形状（COMP_TOOL_SHAPE，S10）：窗口路径是「拖出对角点 / 两端点 -> comp_tool_apply」。
   * 冻结：枚举值、矩形填充与逐字节撤销、椭圆不碰包围盒角（椭圆不是矩形）、直线是
   * 圆头描边而不是填充、带填充的直线被参数闸拒绝且不留撤销步、图层外形状成功但
   * 不产生空撤销步。
   */
  verify(static_cast<int>(COMP_TOOL_SHAPE) == 14, "COMP_TOOL_SHAPE stays at 14");

  std::vector<uint8_t> shape_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t shape_undo_before = 0;
  size_t shape_redo = 0;
  comp_document_history_stats(document, &shape_undo_before, &shape_redo);

  comp_tool_op_desc shape_rect_desc{};
  shape_rect_desc.kind = COMP_TOOL_SHAPE;
  shape_rect_desc.shape.kind = COMP_SHAPE_RECT;
  shape_rect_desc.shape.x0 = 8.0;
  shape_rect_desc.shape.y0 = 8.0;
  shape_rect_desc.shape.x1 = 40.0;
  shape_rect_desc.shape.y1 = 40.0;
  shape_rect_desc.shape.style.fill_enabled = 1;
  shape_rect_desc.shape.style.fill_color[1] = 255; /* 不透明绿 */
  shape_rect_desc.shape.style.fill_color[3] = 255;
  shape_rect_desc.shape.style.opacity = 1.0f;
  shape_rect_desc.shape.style.antialias = 1;
  const bool shape_rect_ok =
      comp_tool_apply(document, layer, &shape_rect_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_rect_ok, "comp_tool_apply(COMP_TOOL_SHAPE rect)");
  std::vector<uint8_t> shape_rect_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_rect_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t rect_inside = static_cast<size_t>(20) * stride + static_cast<size_t>(20) * 4u;
  const size_t rect_outside = static_cast<size_t>(20) * stride + static_cast<size_t>(44) * 4u;
  verify(shape_rect_ok && shape_rect_after[rect_inside + 1] == 255 && shape_rect_after[rect_inside + 3] == 255,
         "the rectangle is filled with the shape colour");
  verify(shape_rect_ok && std::memcmp(&shape_rect_after[rect_outside], &shape_before[rect_outside], 4) == 0,
         "pixels outside the rectangle are untouched");
  verify(shape_rect_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape rect step)");
  std::vector<uint8_t> shape_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_rect_ok && shape_restored == shape_before,
         "shape undo restores pixels byte-for-byte");

  /* 椭圆：中心要填，包围盒的角不能填——那正是「椭圆不是矩形」的证据。 */
  comp_tool_op_desc shape_ellipse_desc = shape_rect_desc;
  shape_ellipse_desc.shape.kind = COMP_SHAPE_ELLIPSE;
  shape_ellipse_desc.shape.style.fill_color[0] = 255;
  shape_ellipse_desc.shape.style.fill_color[1] = 0;
  const bool shape_ellipse_ok =
      comp_tool_apply(document, layer, &shape_ellipse_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_ellipse_ok, "comp_tool_apply(COMP_TOOL_SHAPE ellipse)");
  std::vector<uint8_t> shape_ellipse_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_ellipse_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t shape_ellipse_center = static_cast<size_t>(20) * stride + static_cast<size_t>(20) * 4u;
  const size_t shape_ellipse_corner = static_cast<size_t>(8) * stride + static_cast<size_t>(8) * 4u;
  verify(shape_ellipse_ok && shape_ellipse_after[shape_ellipse_center] == 255 && shape_ellipse_after[shape_ellipse_center + 3] == 255,
         "the ellipse centre is filled");
  verify(shape_ellipse_ok &&
             std::memcmp(&shape_ellipse_after[shape_ellipse_corner], &shape_before[shape_ellipse_corner], 4) == 0,
         "the ellipse leaves its bounding-box corner untouched");
  verify(shape_ellipse_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape ellipse step)");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_ellipse_ok && shape_restored == shape_before,
         "ellipse undo restores pixels byte-for-byte");

  /* 直线：只有描边（fill 必须为 0）；圆头端点越过端点半个线宽。 */
  comp_tool_op_desc shape_line_desc{};
  shape_line_desc.kind = COMP_TOOL_SHAPE;
  shape_line_desc.shape.kind = COMP_SHAPE_LINE;
  shape_line_desc.shape.x0 = 8.0;
  shape_line_desc.shape.y0 = 32.0;
  shape_line_desc.shape.x1 = 56.0;
  shape_line_desc.shape.y1 = 32.0;
  shape_line_desc.shape.style.fill_enabled = 0;
  shape_line_desc.shape.style.stroke_enabled = 1;
  shape_line_desc.shape.style.stroke_color[0] = 255;
  shape_line_desc.shape.style.stroke_color[1] = 255;
  shape_line_desc.shape.style.stroke_color[2] = 255;
  shape_line_desc.shape.style.stroke_color[3] = 255;
  shape_line_desc.shape.style.stroke_width = 6.0; /* 半宽 3 */
  shape_line_desc.shape.style.opacity = 1.0f;
  shape_line_desc.shape.style.antialias = 1;
  const bool shape_line_ok =
      comp_tool_apply(document, layer, &shape_line_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  verify(shape_line_ok, "comp_tool_apply(COMP_TOOL_SHAPE line)");
  std::vector<uint8_t> shape_line_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, shape_line_after.data(), COMP_PIXEL_RGBA8, stride);
  const auto at = [&](int32_t x, int32_t y) {
    return static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
  };
  verify(shape_line_ok && shape_line_after[at(32, 32) + 3] == 255, "the line core is opaque");
  verify(shape_line_ok && shape_line_after[at(32, 29) + 3] == 255,
         "a pixel inside half the stroke width is covered");
  verify(shape_line_ok && std::memcmp(&shape_line_after[at(32, 28)], &shape_before[at(32, 28)], 4) == 0,
         "a pixel beyond half the stroke width is untouched");
  verify(shape_line_ok && shape_line_after[at(58, 32) + 3] > 0, "the round cap extends past the endpoint");
  verify(shape_line_ok && std::memcmp(&shape_line_after[at(60, 32)], &shape_before[at(60, 32)], 4) == 0,
         "the cap stops within half the stroke width");
  verify(shape_line_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(shape line step)");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_line_ok && shape_restored == shape_before,
         "line undo restores pixels byte-for-byte");

  /* 「填充直线」没有定义：参数闸必须在事务之前拒绝，撤销栈与像素都不许动。 */
  comp_tool_op_desc filled_line = shape_line_desc;
  filled_line.shape.style.fill_enabled = 1;
  verify(comp_tool_apply(document, layer, &filled_line, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_SHAPE(line with fill) rejected");
  size_t shape_undo_after_reject = 0;
  comp_document_history_stats(document, &shape_undo_after_reject, &shape_redo);
  verify(shape_undo_after_reject == shape_undo_before,
         "the rejected line shape leaves no undo step");

  /* 合法但完全落在图层外的形状：成功、无像素改动、不产生空撤销步。 */
  comp_tool_op_desc outside_desc = shape_rect_desc;
  outside_desc.shape.x0 = 100.0;
  outside_desc.shape.y0 = 100.0;
  outside_desc.shape.x1 = 120.0;
  outside_desc.shape.y1 = 120.0;
  verify(comp_tool_apply(document, layer, &outside_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "a shape entirely outside the layer succeeds as a no-op");
  size_t shape_undo_after_outside = 0;
  comp_document_history_stats(document, &shape_undo_after_outside, &shape_redo);
  verify(shape_undo_after_outside == shape_undo_before,
         "an off-canvas shape leaves no empty undo step");
  comp_render_flatten(document, shape_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(shape_restored == shape_before,
         "an off-canvas shape leaves the render byte-identical");

  /*
   * 文字（COMP_TOOL_TYPE，S11）：窗口路径是「点击落插入点 -> 键入 ASCII -> 回车提交」。
   * 冻结：枚举值、硬边落墨与逐字节撤销、多行行距、UTF-8 拒绝且不留撤销步、
   * 图层外文字成功但不产生空撤销步。字体是引擎内 5×7 点阵，ASCII-only。
   */
  verify(static_cast<int>(COMP_TOOL_TYPE) == 15, "COMP_TOOL_TYPE stays at 15");

  std::vector<uint8_t> type_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_before.data(), COMP_PIXEL_RGBA8, stride);
  size_t type_undo_before = 0;
  size_t type_redo = 0;
  comp_document_history_stats(document, &type_undo_before, &type_redo);

  comp_tool_op_desc type_desc{};
  type_desc.kind = COMP_TOOL_TYPE;
  type_desc.type.origin_x = 8.0;
  type_desc.type.origin_y = 8.0;
  type_desc.type.text = "A";
  type_desc.type.text_length = 1;
  type_desc.type.style.font_size = 14.0; /* 缩放 2 => 每个点阵格 2x2 像素 */
  type_desc.type.style.line_height = 1.0;
  type_desc.type.style.letter_spacing = 0.0;
  type_desc.type.style.antialias = 0;
  type_desc.type.color[1] = 255; /* 不透明绿 */
  type_desc.type.color[3] = 255;
  type_desc.type.opacity = 1.0f;
  const bool type_ok =
      comp_tool_apply(document, layer, &type_desc, tool_note, sizeof(tool_note), &tool_note_size) ==
      COMP_OK;
  verify(type_ok, "comp_tool_apply(COMP_TOOL_TYPE \"A\")");
  std::vector<uint8_t> type_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_after.data(), COMP_PIXEL_RGBA8, stride);
  const auto type_at = [&](int32_t x, int32_t y) {
    return static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
  };
  /* 'A' 第 0 列是 0x7E（bit1..6）：原点 (8,8)、缩放 2 时 x=8..9、y=10..21 有墨。 */
  verify(type_ok && type_after[type_at(8, 10) + 3] == 255 &&
             type_after[type_at(8, 10) + 1] == 255,
         "the 'A' stem is painted opaque green");
  verify(type_ok && std::memcmp(&type_after[type_at(8, 8)], &type_before[type_at(8, 8)], 4) == 0,
         "the empty top row of the glyph stays untouched");
  verify(type_ok && comp_document_undo(document) == COMP_OK, "comp_document_undo(type step)");
  std::vector<uint8_t> type_restored(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, type_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(type_ok && type_restored == type_before, "type undo restores pixels byte-for-byte");

  /* 多行：字号 7 + 行距 1.0 => 第二行正好下移 7 px。 */
  comp_tool_op_desc multiline_desc = type_desc;
  multiline_desc.type.text = "A\nB";
  multiline_desc.type.text_length = 3;
  multiline_desc.type.style.font_size = 7.0;
  const bool multiline_ok =
      comp_tool_apply(document, layer, &multiline_desc, tool_note, sizeof(tool_note),
                      &tool_note_size) == COMP_OK;
  std::vector<uint8_t> multiline_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, multiline_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(multiline_ok && multiline_after[type_at(8, 9) + 3] == 255 &&
             multiline_after[type_at(8, 16) + 3] == 255,
         "both text lines are painted one line step apart");
  verify(multiline_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(multiline type step)");

  /* UTF-8/CJK：ASCII 字体明确不支持 => 参数闸拒绝且不留撤销步。 */
  comp_tool_op_desc cjk_desc = type_desc;
  cjk_desc.type.text = "\xE4\xBD\xA0\xE5\xA5\xBD";
  cjk_desc.type.text_length = 6;
  verify(comp_tool_apply(document, layer, &cjk_desc, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_ERR_INVALID_ARGUMENT,
         "COMP_TOOL_TYPE(UTF-8/CJK) rejected");
  size_t type_undo_after_reject = 0;
  comp_document_history_stats(document, &type_undo_after_reject, &type_redo);
  verify(type_undo_after_reject == type_undo_before, "the rejected type leaves no undo step");

  /* 合法但完全落在图层外：成功、无像素改动、不产生空撤销步。 */
  comp_tool_op_desc type_outside = type_desc;
  type_outside.type.origin_x = 100.0;
  type_outside.type.origin_y = 100.0;
  verify(comp_tool_apply(document, layer, &type_outside, tool_note, sizeof(tool_note),
                         &tool_note_size) == COMP_OK,
         "text entirely outside the layer succeeds as a no-op");
  size_t type_undo_after_outside = 0;
  comp_document_history_stats(document, &type_undo_after_outside, &type_redo);
  verify(type_undo_after_outside == type_undo_before,
         "an off-canvas text box leaves no empty undo step");
  comp_render_flatten(document, type_restored.data(), COMP_PIXEL_RGBA8, stride);
  verify(type_restored == type_before,
         "an off-canvas text box leaves the render byte-identical");

  /*
   * 选区：壳层蚂蚁线的数据源就是这条「有效 coverage」查询。这里冻结三件事：
   * 契约里「无选区」与「显式空选区」不同、覆盖率为文档分辨率、选区变更是一步撤销。
   */
  int32_t selection_active = -1;
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_get_active() starts at 0 (no selection)");

  const float rect_points[4] = {8.0f, 8.0f, 40.0f, 40.0f};
  int32_t selection_changed = 0;
  const bool rect_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                 COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
      selection_changed == 1;
  verify(rect_ok, "comp_selection_apply_shape(rect)");

  std::vector<uint8_t> coverage(stride * static_cast<size_t>(height), 0);
  comp_mask coverage_mask{};
  coverage_mask.width = width;
  coverage_mask.height = height;
  coverage_mask.stride = stride;
  coverage_mask.pixels = coverage.data();
  verify(rect_ok && comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1,
         "comp_selection_get_active() == 1 after a rect shape");
  verify(comp_selection_get_coverage(document, &coverage_mask) == COMP_OK,
         "comp_selection_get_coverage()");
  verify(rect_ok && coverage[static_cast<size_t>(20) * stride + 20u] == 255 &&
             coverage[static_cast<size_t>(2) * stride + 2u] == 0,
         "coverage is 255 inside the rect and 0 outside");

  /* 蚂蚁线几何：矩形选区应得到 4 条边界；覆盖率全 0 时一条都不画。 */
  comp_ui::BoundarySegment ants[16];
  verify(comp_ui::selection_boundary_segments(coverage.data(), width, height, stride, 128,
                                              ants, 16) == 4,
         "selection_boundary_segments(rect) == 4 (what the shell draws)");

  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(selection step)");
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "undoing the selection shape clears the ants");

  int32_t select_all_changed = 0;
  const bool select_all_ok =
      comp_selection_select_all(document, &select_all_changed) == COMP_OK &&
      comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(63) * stride + 63u] == 255;
  verify(select_all_ok, "comp_selection_select_all() covers the canvas");

  /*
   * 反选「全选」得到的是**无选区**而不是显式空选区（ABI 契约，见
   * comp_selection.h）：此后 get_coverage 会返回 COMP_ERR_STATE，所以壳层
   * refresh_selection 必须先查 active 再决定要不要取覆盖率。
   */
  int32_t invert_changed = 0;
  verify(comp_selection_invert(document, &invert_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_invert() of a full selection means no selection");

  int32_t deselect_changed = 0;
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "comp_selection_deselect() returns to the no-selection state");

  /*
   * S7：椭圆框选 / 套索多边形 / 魔棒 / 新选区羽化。
   * 按壳层 marquee_end / lasso_end / wand_click / finish_new_selection 的同一参数
   * 序列调用 ABI——验证的是「接线后的真实调用」，不是另写一套理想路径。
   */
  const float ellipse_points[4] = {8.0f, 8.0f, 48.0f, 40.0f};
  verify(comp_selection_apply_shape(document, COMP_SELECTION_ELLIPSE, ellipse_points, 2,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
             selection_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(24) * stride + 28u] == 255 &&
             coverage[static_cast<size_t>(8) * stride + 8u] == 0,
         "comp_selection_apply_shape(ellipse) covers the centre and clips the corner");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(ellipse selection step)");

  const float lasso_polygon[6] = {8.0f, 8.0f, 40.0f, 8.0f, 8.0f, 40.0f};
  verify(comp_selection_apply_shape(document, COMP_SELECTION_POLYGON, lasso_polygon, 3,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
             selection_changed == 1 &&
             comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
             coverage[static_cast<size_t>(12) * stride + 12u] == 255 &&
             coverage[static_cast<size_t>(36) * stride + 36u] == 0,
         "comp_selection_apply_shape(polygon) is what the lasso commits");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(lasso selection step)");

  /* 魔棒测试图：左半 (x<32) 不透明灰 200，右半不透明黑；容差 0 = 只吃同色连通块。 */
  std::vector<uint8_t> wand_pixels(stride * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* pixel =
          wand_pixels.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
      const uint8_t value = x < 32 ? 200 : 0;
      pixel[0] = value;
      pixel[1] = value;
      pixel[2] = value;
      pixel[3] = 255;
    }
  }
  comp_layer_desc wand_desc{};
  wand_desc.name = "selftest-wand";
  wand_desc.blend = COMP_BLEND_NORMAL;
  wand_desc.opacity = 1.0f;
  wand_desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  wand_desc.transform = comp_rect{0, 0, width, height}; /* 1:1，魔棒 LAYER 取样的前提 */
  comp_image wand_image{};
  wand_image.width = width;
  wand_image.height = height;
  wand_image.format = COMP_PIXEL_RGBA8;
  wand_image.stride = stride;
  wand_image.pixels = wand_pixels.data();
  comp_layer_id wand_layer = 0;
  const bool wand_fixture_ok =
      comp_layer_add(document, &wand_desc, &wand_layer) == COMP_OK && wand_layer != 0 &&
      comp_layer_set_pixels(document, wand_layer, &wand_image) == COMP_OK;
  verify(wand_fixture_ok, "comp_layer_add(wand fixture) + comp_layer_set_pixels(1:1)");

  int32_t wand_changed = 0;
  const bool wand_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 10.0, 10.0, 0, 1,
                                0, COMP_SELECTION_REPLACE, &wand_changed) == COMP_OK &&
      wand_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(10) * stride + 10u] == 255 &&
      coverage[static_cast<size_t>(10) * stride + 50u] == 0;
  verify(wand_ok, "comp_selection_apply_wand(contiguous) selects one colour region");

  comp_rect wand_bounds{};
  verify(wand_ok && comp_selection_get_bounds(document, &wand_bounds) == COMP_OK &&
             wand_bounds.w == 32 && wand_bounds.h == height,
         "wand selection bounds cover the matched half only");
  verify(wand_ok && comp_document_undo(document) == COMP_OK,
         "comp_document_undo(wand selection step)");

  int32_t wand_merged_changed = 0;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_MERGED, 2.0, 2.0,
                                       0, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_merged_changed) == COMP_OK &&
             wand_merged_changed == 1,
         "comp_selection_apply_wand(merged source) samples the composite");
  verify(comp_document_undo(document) == COMP_OK, "comp_document_undo(merged wand step)");

  int32_t wand_bad = -1;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, -1.0, 4.0,
                                       0, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_bad) == COMP_ERR_INVALID_ARGUMENT &&
             wand_bad == -1,
         "comp_selection_apply_wand(seed outside canvas) rejected, out_changed untouched");
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 4.0, 4.0,
                                       256, 1, 0, COMP_SELECTION_REPLACE,
                                       &wand_bad) == COMP_ERR_INVALID_ARGUMENT,
         "comp_selection_apply_wand(tolerance=256) rejected");

  /*
   * 0 匹配必须是「无变化」而不是清空选区：壳层靠 changed==0 回滚事务，
   * 所以引擎既不能改选区，也不能留下撤销步。
   * 取两色交界 (32,10) 做种、radius 2：参考色是窗口均值 80，种子像素是 0，
   * 容差 0 下同窗口内没有像素落在 80±0 内 -> 选中 0 个。
   */
  verify(comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                    COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK,
         "comp_selection_apply_shape(rect) before the no-match wand probe");
  size_t depth_before_no_match = 0;
  comp_document_history_stats(document, &depth_before_no_match, &redo_depth);
  int32_t no_match_changed = 1;
  const bool no_match_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0, 0, 1,
                                2, COMP_SELECTION_REPLACE, &no_match_changed) == COMP_OK &&
      no_match_changed == 0 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(20) * stride + 20u] == 255;
  size_t depth_after_no_match = 0;
  comp_document_history_stats(document, &depth_after_no_match, &redo_depth);
  verify(no_match_ok && depth_after_no_match == depth_before_no_match,
         "wand with no matching pixel keeps the old selection and adds no undo step");

  /*
   * 新选区羽化：finish_new_selection 把「形状 + comp_selection_set_feather」包进
   * 一个显式事务，因此两步只能算**一步**撤销，且羽化值是本次的绝对值
   * （apply_shape 已把 feather 归零，set_feather 按覆盖语义写入）。
   */
  comp_txn feather_txn = nullptr;
  size_t depth_before_feather = 0;
  comp_document_history_stats(document, &depth_before_feather, &redo_depth);
  int32_t feather_changed = 0;
  bool feather_ok = comp_txn_begin(document, "Feather Selection", &feather_txn) == COMP_OK;
  feather_ok = feather_ok &&
               comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                          COMP_SELECTION_REPLACE, 1,
                                          &selection_changed) == COMP_OK;
  feather_ok = feather_ok && comp_selection_set_feather(document, 4, &feather_changed) == COMP_OK &&
               feather_changed == 1;
  feather_ok = feather_ok && comp_txn_commit(feather_txn) == COMP_OK;
  float feather_value = -1.0f;
  size_t depth_after_feather = 0;
  comp_selection_get_feather(document, &feather_value);
  comp_document_history_stats(document, &depth_after_feather, &redo_depth);
  verify(feather_ok && std::fabs(feather_value - 4.0f) < 0.01f,
         "shape + comp_selection_set_feather(4) yields feather 4 px");
  verify(feather_ok && depth_after_feather == depth_before_feather + 1,
         "shape + feather commit as ONE undo step (finish_new_selection contract)");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1 && comp_selection_get_feather(document, &feather_value) == COMP_OK &&
             std::fabs(feather_value) < 0.01f,
         "undoing the feathered selection restores the unfeathered previous selection");

  /*
   * S8：绝对羽化 / 魔棒取样窗口 / 钢笔路径。
   * 按 commit_feather_input / wand_click / commit_pen_path 的真实调用序列走 ABI。
   */
  int32_t absolute_changed = 0;
  float absolute_feather = -1.0f;
  verify(comp_selection_set_feather(document, 8, &absolute_changed) == COMP_OK &&
             absolute_changed == 1 &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather - 8.0f) < 0.01f,
         "comp_selection_set_feather(8) sets the absolute feather");
  absolute_changed = 99;
  verify(comp_selection_set_feather(document, 8, &absolute_changed) == COMP_OK &&
             absolute_changed == 0,
         "setting the same feather twice is a no-op");
  verify(comp_selection_set_feather(document, 251, &absolute_changed) ==
             COMP_ERR_INVALID_ARGUMENT,
         "comp_selection_set_feather(251) rejected before any transaction");
  verify(comp_selection_set_feather(document, 0, &absolute_changed) == COMP_OK &&
             absolute_changed == 1 &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather) < 0.01f,
         "comp_selection_set_feather(0) clears the feather");
  verify(comp_document_undo(document) == COMP_OK &&
             comp_selection_get_feather(document, &absolute_feather) == COMP_OK &&
             std::fabs(absolute_feather - 8.0f) < 0.01f,
         "undoing the feather reset restores 8 px");
  verify(comp_document_undo(document) == COMP_OK,
         "undoing the absolute feather step restores the previous selection");

  /* 魔棒取样窗口：边界种子 + 半径 1 的参考色是窗口均值，容差 0 下不再匹配。 */
  int32_t radius_changed = 0;
  comp_rect radius_bounds{};
  const bool radius_zero_ok =
      wand_fixture_ok &&
      comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0, 0, 1,
                                0, COMP_SELECTION_REPLACE, &radius_changed) == COMP_OK &&
      radius_changed == 1 && comp_selection_get_bounds(document, &radius_bounds) == COMP_OK &&
      radius_bounds.w == 32;
  verify(radius_zero_ok, "wand radius 0 selects the black half from the boundary seed");
  radius_changed = 99;
  verify(wand_fixture_ok &&
             comp_selection_apply_wand(document, wand_layer, COMP_SELECTION_WAND_LAYER, 32.0, 10.0,
                                       0, 1, 1, COMP_SELECTION_REPLACE,
                                       &radius_changed) == COMP_OK &&
             radius_changed == 0 &&
             comp_selection_get_bounds(document, &radius_bounds) == COMP_OK &&
             radius_bounds.w == 32,
         "wand radius 1 averages the window, finds no match, and keeps the selection");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(wand radius selection step)");

  /*
   * 钢笔路径：ui_core 的锚点/段装配 -> comp_selection_apply_path，与
   * commit_pen_path 走同一条 ABI。这里用 4 个直角锚点的 24..40 方形。
   */
  comp_ui::PenAnchor pen_anchors[4] = {};
  int pen_anchor_count = 0;
  bool pen_state_ok = comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                 comp_ui::Point{24.0, 24.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{40.0, 24.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{40.0, 40.0});
  pen_state_ok = pen_state_ok && comp_ui::pen_append_anchor(pen_anchors, &pen_anchor_count,
                                                            comp_ui::Point{24.0, 40.0});
  comp_ui::PenSegment pen_segments[4] = {};
  const int pen_segment_count =
      comp_ui::pen_build_segments(pen_anchors, pen_anchor_count, /*closed=*/true, pen_segments, 4);
  comp_selection_cubic pen_cubics[4] = {};
  for (int i = 0; i < pen_segment_count && i < 4; ++i) {
    pen_cubics[i].start[0] = static_cast<float>(pen_segments[i].start.x);
    pen_cubics[i].start[1] = static_cast<float>(pen_segments[i].start.y);
    pen_cubics[i].control1[0] = static_cast<float>(pen_segments[i].control1.x);
    pen_cubics[i].control1[1] = static_cast<float>(pen_segments[i].control1.y);
    pen_cubics[i].control2[0] = static_cast<float>(pen_segments[i].control2.x);
    pen_cubics[i].control2[1] = static_cast<float>(pen_segments[i].control2.y);
    pen_cubics[i].end[0] = static_cast<float>(pen_segments[i].end.x);
    pen_cubics[i].end[1] = static_cast<float>(pen_segments[i].end.y);
    pen_cubics[i].new_subpath = i == 0 ? 1 : 0;
  }
  size_t depth_before_pen = 0;
  comp_document_history_stats(document, &depth_before_pen, &redo_depth);
  int32_t pen_changed = 0;
  const bool pen_ok =
      pen_state_ok && pen_segment_count == 4 &&
      comp_selection_apply_path(document, pen_cubics, pen_segment_count,
                                COMP_SELECTION_REPLACE, 1, &pen_changed) == COMP_OK &&
      pen_changed == 1 && comp_selection_get_coverage(document, &coverage_mask) == COMP_OK &&
      coverage[static_cast<size_t>(32) * stride + 32u] == 255 &&
      coverage[static_cast<size_t>(60) * stride + 60u] == 0;
  size_t depth_after_pen = 0;
  comp_document_history_stats(document, &depth_after_pen, &redo_depth);
  verify(pen_ok, "pen path commits a closed selection through comp_selection_apply_path");
  verify(pen_ok && depth_after_pen == depth_before_pen + 1,
         "pen path selection is exactly one undo step");
  comp_ui::Point pen_preview[1 + 4 * 3] = {};
  const int pen_preview_count =
      comp_ui::pen_preview_polyline(pen_anchors, pen_anchor_count, /*closed=*/true, pen_preview,
                                    1 + 4 * 3, /*samples_per_segment=*/3);
  verify(pen_preview_count == 1 + 4 * 3 && pen_preview[pen_preview_count - 1].x == 24.0 &&
             pen_preview[pen_preview_count - 1].y == 24.0,
         "pen preview polyline closes back onto the first anchor");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(pen path selection step)");


  /* 测试图用完即删：它是全画布不透明层，留着会盖住后面 S6 的 flatten 断言。 */
  verify(comp_layer_remove(document, wand_layer) == COMP_OK,
         "comp_layer_remove(wand fixture) keeps later flatten assertions honest");
  verify(comp_selection_deselect(document, &deselect_changed) == COMP_OK &&
             comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 0,
         "deselect after the S7 probes restores the S6 precondition");

  /*
   * S6：前景色 / 画笔 / 橡皮擦 / 吸管 / 选区搬移。
   * 壳层只做参数装配，因此这里按 paint_stroke_end / eyedropper_sample / move_end
   * 的同一顺序调用 ABI——验证的是「壳层接线后的真实序列」，不是另写一套。
   */

  verify(comp_ui::tool_shortcut_target(comp_ui::ToolId::Brush, 'e') ==
                 comp_ui::ToolId::Eraser &&
             comp_ui::tool_shortcut_target(comp_ui::ToolId::Eraser, 'E') ==
                 comp_ui::ToolId::Brush,
         "ui_core tool_shortcut_target(E) toggles brush/eraser");
  verify(comp_ui::swatch_count() == 12 && comp_ui::step_swatch(11, 1) == 0 &&
             comp_ui::step_swatch(0, -1) == 11,
         "ui_core swatch palette cycles (X)");

  comp_ui::ColorRGB foreground{};
  const bool parse_ok = comp_ui::color_parse_hex("#3A7BD5", &foreground);
  char foreground_hex[8] = {0};
  comp_ui::color_to_hex(foreground, foreground_hex);
  verify(parse_ok && foreground.r == 0x3A && foreground.g == 0x7B && foreground.b == 0xD5 &&
             std::strcmp(foreground_hex, "#3A7BD5") == 0,
         "ui_core color_parse_hex / color_to_hex round trip (Shift+X)");

  /* 与 paint_stroke_end 相同：前景色 + 单点折线（单击 = 一记 dab）。 */
  verify(comp_layer_set_pixels(document, layer, &opaque_image) == COMP_OK,
         "comp_layer_set_pixels(base for paint)");
  std::vector<uint8_t> paint_base(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, paint_base.data(), COMP_PIXEL_RGBA8, stride);

  comp_tool_op_desc paint_desc{};
  paint_desc.kind = COMP_TOOL_STROKE;
  paint_desc.brush.color[0] = foreground.r;
  paint_desc.brush.color[1] = foreground.g;
  paint_desc.brush.color[2] = foreground.b;
  paint_desc.brush.diameter = 10.0f;
  paint_desc.brush.pressure = 1.0f;
  paint_desc.brush.hardness = 1.0f;
  paint_desc.brush.flow = 1.0f;
  paint_desc.brush.seed = 0;
  const float dab[] = {32.0f, 32.0f};
  paint_desc.stroke_points = dab;
  paint_desc.stroke_point_count = 1;
  char stroke_note[256] = {0};
  size_t stroke_note_size = 0;
  const bool paint_ok =
      comp_tool_apply(document, layer, &paint_desc, stroke_note, sizeof(stroke_note),
                      &stroke_note_size) == COMP_OK;
  std::vector<uint8_t> paint_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, paint_after.data(), COMP_PIXEL_RGBA8, stride);
  const size_t center = static_cast<size_t>(32) * stride + static_cast<size_t>(32) * 4u;
  verify(paint_ok && paint_after != paint_base && paint_after[center + 0] == 0x3A &&
             paint_after[center + 1] == 0x7B && paint_after[center + 2] == 0xD5,
         "COMP_TOOL_STROKE paints the shell foreground colour");

  /* 吸管：同一位置取回的直通 sRGB 应还原成同一个前景色。 */
  float sampled[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const bool sample_ok = comp_eyedropper_sample(document, layer, 32.0, 32.0, 1,
                                                COMP_EYEDROPPER_LAYER, sampled) == COMP_OK;
  const comp_ui::ColorRGB sampled_color =
      comp_ui::color_from_unit(sampled[0], sampled[1], sampled[2]);
  verify(sample_ok && sampled_color.r == foreground.r && sampled_color.g == foreground.g &&
             sampled_color.b == foreground.b && sampled[3] == 1.0f,
         "comp_eyedropper_sample() returns the painted colour (feeds foreground)");

  /* 橡皮擦：同一支笔、同样的 diameter/hardness，方向翻转成 destination-out。 */
  paint_desc.erase = 1;
  const bool erase_ok =
      comp_tool_apply(document, layer, &paint_desc, stroke_note, sizeof(stroke_note),
                      &stroke_note_size) == COMP_OK;
  std::vector<uint8_t> erase_after(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, erase_after.data(), COMP_PIXEL_RGBA8, stride);
  verify(erase_ok && erase_after != paint_after,
         "COMP_TOOL_STROKE(erase) removes the painted pixel");
  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(erase step)");
  std::vector<uint8_t> erase_undone(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, erase_undone.data(), COMP_PIXEL_RGBA8, stride);
  verify(erase_undone == paint_after, "undoing erase restores the painted pixel byte-identically");

  /*
   * 选区搬移：像素搬移 + 轮廓平移必须落在同一个显式事务里（一次拖拽 = 一步撤销）。
   * 顺序与 move_end 一致：先按当前选区切像素，再平移轮廓。
   */
  comp_rect bounds_before{};
  const bool move_rect_ok =
      comp_selection_apply_shape(document, COMP_SELECTION_RECT, rect_points, 2,
                                 COMP_SELECTION_REPLACE, 1, &selection_changed) == COMP_OK &&
      comp_selection_get_bounds(document, &bounds_before) == COMP_OK;
  std::vector<uint8_t> move_before(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, move_before.data(), COMP_PIXEL_RGBA8, stride);

  comp_txn move_txn = nullptr;
  const bool move_txn_ok =
      comp_txn_begin(document, "Move Selection Pixels", &move_txn) == COMP_OK;
  int32_t moved_pixels = 0;
  int32_t moved_outline = 0;
  const bool move_ok =
      move_txn_ok &&
      comp_selection_move_pixels(document, layer, 8, 0, 0, &moved_pixels) == COMP_OK &&
      comp_selection_translate(document, 8, 0, &moved_outline) == COMP_OK &&
      comp_txn_commit(move_txn) == COMP_OK;
  comp_rect bounds_after{};
  comp_selection_get_bounds(document, &bounds_after);
  verify(move_rect_ok && move_ok && moved_pixels > 0 && moved_outline > 0 &&
             bounds_after.x == bounds_before.x + 8 && bounds_after.y == bounds_before.y,
         "selection move pixels + translate commit in one explicit transaction");

  verify(comp_document_undo(document) == COMP_OK,
         "comp_document_undo(move selection)");
  comp_rect bounds_undone{};
  std::vector<uint8_t> move_undone(stride * static_cast<size_t>(height), 0);
  comp_render_flatten(document, move_undone.data(), COMP_PIXEL_RGBA8, stride);
  verify(comp_selection_get_active(document, &selection_active) == COMP_OK &&
             selection_active == 1 &&
             comp_selection_get_bounds(document, &bounds_undone) == COMP_OK &&
             bounds_undone.x == bounds_before.x && bounds_undone.y == bounds_before.y &&
             move_undone == move_before,
         "one undo restores both the pixels and the selection outline");

  comp_document_destroy(document);
  comp_engine_destroy(engine);

  std::fprintf(report, "\n[summary] failures = %d\n", failures);
  std::fprintf(report, "[%s] compositor-shell selftest\n", failures == 0 ? "PASS" : "FAIL");
  std::fclose(report);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
  /* --selftest[=报告文件]：无窗口端到端自检（只走 C ABI），用于真机可审计验证。 */
  if (command_line != nullptr && wcsstr(command_line, L"--selftest") != nullptr) {
    const wchar_t* equals = wcschr(command_line, L'=');
    return run_selftest(equals != nullptr ? equals + 1 : L"compositor_shell_selftest.txt");
  }

  ShellState shell;

  comp_engine_desc engine_desc{};
  engine_desc.app_name = "compositor-shell-windows";
  engine_desc.version = "0.1.0";
  engine_desc.backend = COMP_BACKEND_AUTO; /* 本机无 GPU 时由引擎回落 CPU */
  if (comp_engine_create(&engine_desc, &shell.engine) != COMP_OK) {
    MessageBoxW(nullptr, L"引擎创建失败", L"compositor", MB_ICONERROR);
    return 1;
  }

  comp_backend actual = COMP_BACKEND_AUTO;
  if (comp_engine_backend(shell.engine, &actual) == COMP_OK && actual != COMP_BACKEND_CPU) {
    /* 后端非 CPU 时如实提示，不做静默假设。 */
    set_status(shell, L"引擎后端已启用硬件加速");
  }

  const wchar_t* class_name = L"CompositorShellWindow";
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = class_name;
  if (RegisterClassExW(&window_class) == 0) {
    comp_engine_destroy(shell.engine);
    return 1;
  }

  HWND window = CreateWindowExW(0, class_name, L"Compositor (Windows shell)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, kDefaultWidth, kDefaultHeight, nullptr,
                                build_menu(), instance, nullptr);
  if (window == nullptr) {
    comp_engine_destroy(shell.engine);
    return 1;
  }

  g_shell = &shell;
  shell.window = window; /* 视图命令需要客户区矩形，必须早于首次绘制设置 */
  create_document(shell);
  /* 蚂蚁线相位定时器：无选区时处理函数直接返回，不产生重绘。 */
  SetTimer(window, kAntsTimerId, kAntsTimerPeriodMs, nullptr);
  ShowWindow(window, show);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  g_shell = nullptr;
  if (shell.document != nullptr) comp_document_destroy(shell.document);
  comp_engine_destroy(shell.engine);
  return 0;
}
