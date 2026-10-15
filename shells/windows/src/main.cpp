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
#include <compositor/comp_io.h>
#include <compositor/comp_layer.h>
#include <compositor/comp_render.h>
#include <compositor/comp_undo.h>

namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;

/* 图层面板几何（像素）与行高。 */
constexpr int kPanelWidth = 248;
constexpr int kPanelRowHeight = 22;

/* 调整面板几何（右侧）：列宽与行高。 */
constexpr int kAdjustPanelWidth = 330;
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
  shell.visibility.clear();
  refresh_layers(shell);
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
  shell.visibility.clear();
  refresh_layers(shell);
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
    case kCmdLayerAdd:
      panel_add_layer(shell);
      break;
    case kCmdLayerMergeDown:
      panel_merge_down(shell);
      break;
    case kCmdLayerFlatten:
      panel_flatten(shell);
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
    case kCmdExit:
      PostMessageW(window, WM_CLOSE, 0, 0);
      return;
    default:
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

/* 图层面板：GDI 文本绘制（外壳不读像素，只呈现引擎的属性查询结果）。 */
void draw_layer_panel(HDC dc, const ShellState& shell, int height) {
  RECT panel{0, 0, kPanelWidth, height};
  HBRUSH background = CreateSolidBrush(RGB(24, 26, 30));
  FillRect(dc, &panel, background);
  DeleteObject(background);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(240, 240, 244));
  wchar_t title[64];
  wsprintfW(title, L"图层（%u）", static_cast<unsigned>(shell.layers.size()));
  TextOutW(dc, 10, 10, title, static_cast<int>(wsprintfW(title, L"图层（%u）",
                                                        static_cast<unsigned>(shell.layers.size()))));

  const int32_t count = static_cast<int32_t>(shell.layers.size());
  for (int32_t line = 0; line < count; ++line) {
    const LayerRow& row = shell.layers[static_cast<size_t>(count - 1 - line)];
    const int top = 40 + line * kPanelRowHeight;
    if (line == shell.selected_layer) {
      RECT highlight{0, top - 2, kPanelWidth, top + kPanelRowHeight - 2};
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
    TextOutW(dc, 10, top, line_text, lstrlenW(line_text));
  }

  SetTextColor(dc, RGB(140, 144, 156));
  const wchar_t* hint = L"↑↓ 选择 · V 显隐 · [ ] 不透明度";
  TextOutW(dc, 10, height - 46, hint, lstrlenW(hint));
  const wchar_t* hint2 = L"Ctrl+Shift+N 新建图层";
  TextOutW(dc, 10, height - 26, hint2, lstrlenW(hint2));
}

/* 调整面板：控件完全由 ABI 元数据生成（外壳不复制键名表、不解析 JSON）。 */
void draw_adjust_panel(HDC dc, const ShellState& shell, int left, int height) {
  RECT panel{left, 0, left + kAdjustPanelWidth, height};
  HBRUSH background = CreateSolidBrush(RGB(22, 24, 28));
  FillRect(dc, &panel, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  wchar_t line[256];
  int y = 10;
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
        RECT highlight{left + 4, y - 2, left + kAdjustPanelWidth - 6, y + kAdjustRowHeight - 4};
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

void present(HWND window, ShellState& shell) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(window, &paint);
  if (shell.needs_render) render_canvas(shell);

  RECT client{};
  GetClientRect(window, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  /* 画布区 = 客户区去掉左侧图层面板与右侧调整面板。 */
  const int canvas_left = client_width > kPanelWidth ? kPanelWidth : 0;
  const int right_panel =
      client_width > kPanelWidth + kAdjustPanelWidth ? kAdjustPanelWidth : 0;
  const int canvas_width =
      client_width > kPanelWidth + right_panel ? client_width - kPanelWidth - right_panel : client_width;
  const int canvas_height = client_height;

  HDC memory_dc = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client_width, client_height);
  HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

  RECT fill{0, 0, client_width, client_height};
  HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
  FillRect(memory_dc, &fill, background);
  DeleteObject(background);

  if (!shell.framebuffer.empty() && shell.width > 0 && shell.height > 0 && canvas_width > 0) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = shell.width;
    info.bmiHeader.biHeight = -shell.height; /* 负值 = 自顶向下，与引擎缓冲一致 */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    const double scale_x = static_cast<double>(canvas_width) / shell.width;
    const double scale_y = static_cast<double>(canvas_height) / shell.height;
    const double scale = scale_x < scale_y ? scale_x : scale_y;
    const int draw_width = static_cast<int>(shell.width * scale);
    const int draw_height = static_cast<int>(shell.height * scale);
    const int offset_x = canvas_left + (canvas_width - draw_width) / 2;
    const int offset_y = (canvas_height - draw_height) / 2;

    SetStretchBltMode(memory_dc, HALFTONE);
    StretchDIBits(memory_dc, offset_x, offset_y, draw_width, draw_height, 0, 0, shell.width,
                  shell.height, shell.framebuffer.data(), &info, DIB_RGB_COLORS, SRCCOPY);
  }

  draw_layer_panel(memory_dc, shell, client_height);
  if (right_panel > 0) {
    draw_adjust_panel(memory_dc, shell, client_width - kAdjustPanelWidth, client_height);
  }

  /* 状态栏：撤销深度等壳层反馈（引擎不提供 UI，由外壳呈现）。 */
  if (!shell.status.empty()) {
    SetBkMode(memory_dc, TRANSPARENT);
    SetTextColor(memory_dc, RGB(226, 226, 230));
    TextOutW(memory_dc, canvas_left + 12, 10, shell.status.c_str(),
             static_cast<int>(shell.status.size()));
  }

  BitBlt(dc, 0, 0, client_width, client_height, memory_dc, 0, 0, SRCCOPY);
  SelectObject(memory_dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory_dc);
  EndPaint(window, &paint);
}

void handle_key(HWND window, ShellState& shell, WPARAM key, bool ctrl, bool shift) {
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
        /* Ctrl+Shift+D 删除选中的调整。 */
        if (shift) {
          run_shell_command(window, shell, kCmdAdjRemove);
          return;
        }
        break;
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
    case 'V':
      run_shell_command(window, shell, kCmdLayerToggleVisible);
      break;
    case VK_OEM_4: /* '[' */
      run_shell_command(window, shell, kCmdLayerOpacityDown);
      break;
    case VK_OEM_6: /* ']' */
      run_shell_command(window, shell, kCmdLayerOpacityUp);
      break;
    case 'N':
      run_shell_command(window, shell, kCmdNew);
      break;
    case 'O':
      run_shell_command(window, shell, kCmdOpen);
      break;
    case 'S':
      run_shell_command(window, shell, kCmdSave);
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
      /* 数字键在调整面板里开始键入数值。 */
      if (key >= '0' && key <= '9') {
        panel_begin_input(shell, static_cast<wchar_t>(key));
      }
      break;
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  ShellState* shell = g_shell;
  switch (message) {
    case WM_CREATE:
      return 0;
    case WM_PAINT:
      if (shell != nullptr) present(window, *shell);
      break;
    case WM_KEYDOWN:
      if (shell != nullptr) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        handle_key(window, *shell, wparam, ctrl, shift);
      }
      break;
    case WM_COMMAND:
      /* lparam != 0 表示控件通知，本骨架无子控件，直接忽略。 */
      if (shell != nullptr && lparam == 0) {
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

/* 菜单结构：文件 / 编辑 / 图层；快捷键提示文本与 handle_key 的映射保持一致。 */
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
  AppendMenuW(edit_menu, MF_STRING, kCmdListCommands, L"枚举引擎命令(&L)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_menu), L"编辑(&E)");

  HMENU layer_menu = CreatePopupMenu();
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerAdd, L"新建图层(&A)\tCtrl+Shift+N");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerMergeDown, L"向下合并(&M)\tCtrl+E");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerFlatten, L"拼合图像(&F)");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectUp, L"选择上一图层(&U)\t↑");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerSelectDown, L"选择下一图层(&D)\t↓");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerToggleVisible, L"切换可见性(&V)\tV");
  AppendMenuW(layer_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityDown, L"降低不透明度(&-)\t[");
  AppendMenuW(layer_menu, MF_STRING, kCmdLayerOpacityUp, L"提高不透明度(&+)\t]");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layer_menu), L"图层(&L)");

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
  create_document(shell);
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
