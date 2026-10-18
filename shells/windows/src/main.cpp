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

  /* 框选矩形的两个角点（顺序任意，引擎按包围盒解释）。 */
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

  int32_t changed = 0;
  const comp_result result = comp_selection_apply_shape(
      shell.document, COMP_SELECTION_RECT, points, 2, combine, /*antialias=*/1, &changed);
  if (result != COMP_OK) {
    set_status(shell, L"框选失败");
    return;
  }
  const wchar_t* prefix = changed ? L"框选：已应用" : L"框选：选区未变化";
  if (combine == COMP_SELECTION_ADD) prefix = changed ? L"框选：已加选" : L"框选：加选未变化";
  if (combine == COMP_SELECTION_SUBTRACT) {
    prefix = changed ? L"框选：已减选" : L"框选：减选未变化";
  }
  finish_selection_command(shell, prefix);
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

  /* 框选矩形：文档坐标两点 -> 屏幕矩形；白实线 + 黑虚线。 */
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
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, dotted);
    Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(solid);
    DeleteObject(dotted);
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
  shell.tool = id;
  const comp_ui::ToolInfo* info = comp_ui::tool_info(id);
  if (info == nullptr) return;
  wchar_t buffer[128];
  if (id == comp_ui::ToolId::Retouch) {
    wsprintfW(buffer, L"工具：Retouch / %s（再按 R 循环）",
              widen_ascii(comp_ui::retouch_mode_name(shell.retouch_mode)).c_str());
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
  if (key == VK_ESCAPE) {
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
    case VK_OEM_4: /* [ = 笔刷变小；Shift+[ = 硬度降低 */
      brush_step(shell, -1, shift);
      break;
    case VK_OEM_6: /* ] = 笔刷变大；Shift+] = 硬度提高 */
      brush_step(shell, 1, shift);
      break;
    case VK_TAB:
      panel_move_param(shell, shift ? -1 : 1);
      break;
    case VK_RETURN:
      panel_commit_input(shell);
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
        if (wparam == VK_SPACE) {
          /* 空格 = 临时抓手：按下不重复触发，松开才结束。 */
          if (!shell->space_down) {
            shell->space_down = true;
            set_status(*shell, L"按住空格拖动画布（或中键拖动）");
            InvalidateRect(window, nullptr, FALSE);
          }
          break;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
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
            } else if (marquee_available(*shell)) {
              marquee_begin(window, *shell, mouse_x, mouse_y, layout);
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
      if (shell != nullptr && shell->marquee_active) {
        marquee_end(window, *shell);
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
      }
      break;
    case WM_CAPTURECHANGED:
      if (shell != nullptr) {
        end_pan(window, *shell);
        stroke_cancel(*shell);
        move_cancel(*shell);
        eyedropper_cancel(*shell);
        marquee_cancel(*shell);
      }
      break;
    case WM_MOUSEMOVE:
      if (shell != nullptr) {
        Layout layout{};
        if (client_layout(*shell, &layout)) {
          if (shell->move_active) {
            move_extend(*shell, mouse_x, mouse_y, layout);
            InvalidateRect(window, nullptr, FALSE);
          } else if (shell->eyedropper_drag) {
            /* 吸管拖动连续取样：只更新前景色与状态栏，不改画布内容。 */
            eyedropper_sample(*shell, mouse_x, mouse_y, layout);
          } else if (shell->marquee_active) {
            marquee_extend(*shell, mouse_x, mouse_y, layout);
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
        wchar_t buffer[96];
        if (shell->tool == comp_ui::ToolId::Retouch &&
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
