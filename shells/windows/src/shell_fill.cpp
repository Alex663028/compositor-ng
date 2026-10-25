// AI生成
/*
 * shell_fill.cpp — 填充图层（纯色 / 渐变 / 图案）的参数面板
 *
 * P1-2 收尾 ⑧。与文字图层、智能对象同族：**参数是真源，图层像素是渲染缓存**
 * （见 engine/domain/fill_layer.h）。所以壳侧只有两件事：
 *   1. 参数行 —— 键名 / 量程 / 缺省值全部来自引擎的参数表（comp_fill_kind_param_at），
 *      壳里不抄任何一份，也就不会和引擎漂移；
 *   2. settings —— 由 comp_fill_build_settings 从扁平值组装，壳不拼 JSON、不解析 JSON。
 *
 * 三种 kind 共用一套参数行，所以只有一个面板：`fill_kind` 决定参数表，`fill_layer`
 * 决定写入目标（0 = 还没建层，回车才 add；非 0 = 改既有填充层，回车 set）。
 * 新建走 comp_layer_add_fill，改参数走 comp_layer_set_fill（幂等：参数相同不落撤销步）。
 *
 * 拖动滑杆与调整面板同款：按下开显式事务，拖动途中每次写回都落在里面，松手才
 * commit——不开事务时引擎的 ImplicitTxn 会把每次写都自提交，拖一次能产生几十步撤销。
 */
#include "shell_internal.h"

namespace comp_shell {

/* 参数表查询：面板行号 -> 引擎元数据。行号越界 / kind 非法都回 false。 */
bool fill_param_query(const ShellState& shell, int32_t row, comp_fill_param* out) {
  if (row < 0 || out == nullptr) return false;
  return comp_fill_kind_param_at(shell.doc.fill_kind, static_cast<size_t>(row), out) == COMP_OK;
}

/* 参数行数（参数表是唯一事实来源，壳不缓存数量）。 */
static int32_t fill_param_total(const ShellState& shell) {
  size_t count = 0;
  if (comp_fill_kind_param_count(shell.doc.fill_kind, &count) != COMP_OK) return 0;
  return static_cast<int32_t>(count);
}

/* kind 显示名（引擎的 kind 名是稳定 ASCII 标识，直接展开即可，与 effect_kind_label 同口径）。 */
static std::wstring fill_kind_label(uint32_t kind) {
  const char* name = nullptr;
  if (comp_fill_kind_name(kind, &name) != COMP_OK || name == nullptr) return L"Fill";
  return widen_ascii(name);
}

/*
 * 编辑缓冲 <- 参数表 + settings。
 *
 * settings 为空 = 全新（走引擎缺省值）；非空 = 已有填充层，逐项按 json_path 回读。
 * 回读走 comp_fill_read_value（引擎解析 JSON），数值按面板量程夹一次：.comp 里可能
 * 带着超出当前量程的旧值，面板显示 1000 而引擎按 255 算就是在骗用户。
 */
static void fill_reset_values(ShellState& shell, const std::string& settings) {
  const int32_t focused = shell.doc.fill_param;
  shell.doc.fill_values.clear();

  const int32_t total = fill_param_total(shell);
  for (int32_t i = 0; i < total; ++i) {
    comp_fill_param param{};
    if (!fill_param_query(shell, i, &param)) {
      shell.doc.fill_values.emplace_back(number_text(0.0));
      continue;
    }
    bool filled = false;
    std::string text;
    if (!settings.empty()) {
      if (param.type == COMP_PARAM_BOOL) {
        int32_t boolean = 0;
        if (comp_fill_read_value(settings.c_str(), param.json_path, nullptr, &boolean) == COMP_OK) {
          text = boolean != 0 ? "1" : "0";
          filled = true;
        }
      } else {
        double number = 0.0;
        if (comp_fill_read_value(settings.c_str(), param.json_path, &number, nullptr) == COMP_OK) {
          text = number_text(
              comp_ui::clamp_effect_param(number, param.min_value, param.max_value));
          filled = true;
        }
      }
    }
    if (!filled) {
      text = param.type == COMP_PARAM_BOOL ? (param.default_value != 0.0 ? "1" : "0")
                                           : number_text(param.default_value);
    }
    shell.doc.fill_values.push_back(text);
  }

  if (total <= 0) {
    shell.doc.fill_param = 0;
  } else if (focused < 0 || focused >= total) {
    shell.doc.fill_param = total - 1;
  }
  if (shell.doc.fill_scroll < 0) shell.doc.fill_scroll = 0;
}

/*
 * 面板回读：选中图层是不是填充层、是哪个 kind、settings 是什么。
 *
 * 绑定策略：面板**跟随选中图层**（与调整/效果面板同一条规矩）。选中层是填充层
 * 就绑定它并回填既有参数；不是就退回「新建态」（fill_layer = 0），此时回车会
 * 用当前 kind 新建一层。视觉上不会出现「面板显示 A 层参数、回车写进 B 层」。
 */
void refresh_fill(ShellState& shell) {
  if (!shell.doc.fill_open) return;

  const comp_layer_id id = shell.doc.document != nullptr ? selected_layer_id(shell) : 0;
  int32_t is_fill = 0;
  uint32_t kind = COMP_FILL_NONE;
  std::string settings;
  bool bound = false;
  if (id != 0 && comp_layer_is_fill(shell.doc.document, id, &is_fill) == COMP_OK &&
      is_fill != 0) {
    size_t need = 0;
    if (comp_layer_get_fill(shell.doc.document, id, &kind, nullptr, 0, &need) == COMP_OK) {
      std::vector<char> text(need + 1, '\0');
      if (comp_layer_get_fill(shell.doc.document, id, &kind, text.data(), text.size(), &need) ==
          COMP_OK) {
        settings.assign(text.data(), need);
        bound = true;
      }
    }
  }

  shell.doc.fill_layer = bound ? id : 0;
  if (bound && kind >= COMP_FILL_SOLID && kind <= COMP_FILL_PATTERN) {
    shell.doc.fill_kind = kind;
  }
  fill_reset_values(shell, settings);
}

/* 把焦点行滚进可视区（面板矮 / 图案 10 行参数时必需）。 */
static void fill_scroll_to_focus(ShellState& shell) {
  Layout layout{};
  if (!client_layout(shell, &layout) || layout.adjust_panel.w <= 0) return;
  shell.doc.fill_scroll = comp_ui::adjust_param_ensure_visible(
      layout.adjust_panel, fill_param_total(shell), shell.doc.fill_param, shell.doc.fill_scroll);
}

void open_fill_panel(ShellState& shell, uint32_t kind) {
  if (kind < COMP_FILL_SOLID || kind > COMP_FILL_PATTERN) {
    set_status(shell, L"无效的填充类型");
    return;
  }
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  /* 右栏一次只挂一个面板：与滤镜 / 效果 / 修边 / 路径 / 历史 / 色彩范围 / 智能滤镜栈互斥。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  smart_panel_dismiss(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell); /* 滤镜预览帧不能留在新面板底下 */
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  channel_panel_close(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.trim_open = false;
  shell.doc.trim_input.clear();

  fill_panel_drag_finish(shell, false); /* 幂等：上一次拖动不该跨面板存活 */
  shell.doc.fill_open = true;
  shell.doc.fill_kind = kind;
  shell.doc.fill_layer = 0; /* 新建态：回车才真正建层 */
  shell.doc.fill_param = 0;
  shell.doc.fill_scroll = 0;
  shell.doc.fill_input.clear();
  fill_reset_values(shell, std::string());
  fill_scroll_to_focus(shell);

  wchar_t buffer[224];
  wsprintfW(buffer, L"填充图层：%s（%d 个参数）Tab 选参数 · 拖动/ - = 调值 · 回车%s · Esc 关闭",
            fill_kind_label(kind).c_str(), fill_param_total(shell),
            shell.doc.fill_layer != 0 ? L"应用" : L"新建");
  set_status(shell, buffer);
}

void close_fill_panel(ShellState& shell) {
  /* 先收口拖动：它持有捕获与事务，留着会在下一次打开时把旧事务提交掉。 */
  fill_panel_drag_finish(shell, false);
  shell.doc.fill_open = false;
  shell.doc.fill_input.clear();
  shell.doc.fill_layer = 0;
  shell.doc.fill_drag_param = -1;
  set_status(shell, L"已关闭填充参数面板");
}

/* 聚焦某一行参数；越界夹取（与面板行几何共用同一个序号空间）。 */
void fill_panel_focus_row(ShellState& shell, int32_t row) {
  const int32_t count = static_cast<int32_t>(shell.doc.fill_values.size());
  if (count == 0) {
    set_status(shell, L"该填充类型没有可编辑参数");
    return;
  }
  if (row < 0) row = 0;
  if (row >= count) row = count - 1;
  shell.doc.fill_param = row;
  shell.doc.fill_input.clear();
  fill_scroll_to_focus(shell);
  comp_fill_param param{};
  if (!fill_param_query(shell, row, &param)) return;
  wchar_t buffer[220];
  wsprintfW(buffer, L"参数 %d/%d：%s = %s", row + 1, count,
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.fill_values[static_cast<size_t>(row)].c_str()).c_str());
  set_status(shell, buffer);
}

void fill_panel_move_param(ShellState& shell, int32_t delta) {
  fill_panel_focus_row(shell, shell.doc.fill_param + delta);
}

/* 步长与调整/效果面板同口径（0..1 的归一量走 0.05，其余走 1），夹取在 ui_core。 */
void fill_panel_step_value(ShellState& shell, int32_t direction) {
  const size_t index = static_cast<size_t>(shell.doc.fill_param);
  if (index >= shell.doc.fill_values.size()) {
    set_status(shell, L"该填充类型没有可编辑参数");
    return;
  }
  comp_fill_param param{};
  if (!fill_param_query(shell, shell.doc.fill_param, &param)) return;
  if (param.type == COMP_PARAM_BOOL) {
    shell.doc.fill_values[index] = shell.doc.fill_values[index] == "1" ? "0" : "1";
  } else {
    const double current = std::atof(shell.doc.fill_values[index].c_str());
    shell.doc.fill_values[index] = number_text(comp_ui::step_effect_param(
        current, direction, param.min_value, param.max_value));
  }
  shell.doc.fill_input.clear();
  wchar_t buffer[240];
  wsprintfW(buffer, L"%s = %s （范围 [%s, %s]）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.fill_values[index].c_str()).c_str(),
            widen_ascii(number_text(param.min_value).c_str()).c_str(),
            widen_ascii(number_text(param.max_value).c_str()).c_str());
  set_status(shell, buffer);
}

/* 数字键开始输入：布尔参数不进入文本态，直接切换（与调整/效果面板同规矩）。 */
void fill_panel_begin_input(ShellState& shell, wchar_t character) {
  const size_t index = static_cast<size_t>(shell.doc.fill_param);
  if (index >= shell.doc.fill_values.size()) {
    set_status(shell, L"该填充类型没有可编辑参数");
    return;
  }
  comp_fill_param param{};
  if (!fill_param_query(shell, shell.doc.fill_param, &param)) return;
  if (param.type == COMP_PARAM_BOOL) {
    fill_panel_step_value(shell, 1);
    return;
  }
  shell.doc.fill_input.assign(1, character);
  set_status(shell, L"输入中：回车提交并应用，Esc 取消");
}

void fill_panel_backspace(ShellState& shell) {
  if (!shell.doc.fill_input.empty()) shell.doc.fill_input.pop_back();
}

void fill_panel_commit_input(ShellState& shell) {
  if (shell.doc.fill_input.empty()) {
    set_status(shell, L"未在输入状态（数字键开始输入）");
    return;
  }
  const size_t index = static_cast<size_t>(shell.doc.fill_param);
  if (index >= shell.doc.fill_values.size()) {
    shell.doc.fill_input.clear();
    return;
  }
  comp_fill_param param{};
  if (!fill_param_query(shell, shell.doc.fill_param, &param)) {
    shell.doc.fill_input.clear();
    return;
  }
  /* 布尔参数不接受文本：只认 0/1（布尔不走键入态，这里兜住误按）。 */
  if (param.type == COMP_PARAM_BOOL) {
    const std::string text(shell.doc.fill_input.begin(), shell.doc.fill_input.end());
    if (text != "0" && text != "1") {
      set_status(shell, L"布尔参数只接受 0 / 1，未提交");
      return;
    }
    shell.doc.fill_values[index] = text;
    shell.doc.fill_input.clear();
    return;
  }
  const std::string text(shell.doc.fill_input.begin(), shell.doc.fill_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  /* 面板夹取（与调整/效果同口径）；不夹就等着 ABI 拒绝，状态栏只能报「参数不合法」。 */
  shell.doc.fill_values[index] =
      number_text(comp_ui::clamp_effect_param(value, param.min_value, param.max_value));
  shell.doc.fill_input.clear();
  wchar_t buffer[192];
  wsprintfW(buffer, L"已设置 %s = %s", widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(shell.doc.fill_values[index].c_str()).c_str());
  set_status(shell, buffer);
}

/* Backspace（非输入态）= 缓冲回引擎缺省；仍然要回车才落盘。 */
void fill_panel_reset_values(ShellState& shell) {
  shell.doc.fill_input.clear();
  fill_reset_values(shell, std::string());
  set_status(shell, L"填充参数已重置为默认值（回车应用，Esc 关闭）");
}

/*
 * settings JSON 由引擎组装（壳内不拼 JSON、不解析 JSON）。
 * 没有「合并底」参数是刻意的：每个 kind 的顶层键白名单是封闭的（fill_validate_settings
 * 拒绝未知键，.comp 载入时校验不过的 engineFill 整块丢弃），所以不存在需要原样带回去的
 * 未知键——这一点与 effect_build_settings 的深合并契约不同。
 */
bool build_fill_settings_json(const ShellState& shell, std::string* out_json) {
  std::vector<const char*> values;
  values.reserve(shell.doc.fill_values.size());
  for (const std::string& text : shell.doc.fill_values) values.push_back(text.c_str());

  const uint32_t kind = shell.doc.fill_kind;
  const char* const* data = values.empty() ? nullptr : values.data();
  size_t need = 0;
  if (comp_fill_build_settings(kind, data, values.size(), nullptr, 0, &need) != COMP_OK) {
    return false;
  }
  std::string json(need + 1, '\0');
  if (comp_fill_build_settings(kind, data, values.size(), &json[0], json.size(), &need) !=
      COMP_OK) {
    return false;
  }
  json.resize(need);
  if (out_json != nullptr) *out_json = json;
  return true;
}

/* 回车 = 落盘：新建态 add，已绑定 set（set 幂等，参数没变不落撤销步）。 */
void panel_apply_fill(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  std::string json;
  if (!build_fill_settings_json(shell, &json)) {
    set_status(shell, L"应用被拒：参数不合法");
    return;
  }
  const uint32_t kind = shell.doc.fill_kind;
  const int32_t focused = shell.doc.fill_param;
  wchar_t buffer[224];

  if (shell.doc.fill_layer != 0) {
    int32_t changed = 0;
    const comp_result result = comp_layer_set_fill(shell.doc.document, shell.doc.fill_layer, kind,
                                                   json.c_str(), &changed);
    shell.doc.needs_render = true;
    refresh_layers(shell); /* 会把编辑缓冲重建（并清焦点），下面恢复焦点 */
    if (focused >= 0 && focused < static_cast<int32_t>(shell.doc.fill_values.size())) {
      shell.doc.fill_param = focused;
    }
    if (result != COMP_OK) {
      wsprintfW(buffer, L"应用填充参数失败（图层 #%I64u）",
                static_cast<unsigned long long>(shell.doc.fill_layer));
    } else if (changed == 0) {
      wsprintfW(buffer, L"填充参数未改动（未产生撤销步）");
    } else {
      wsprintfW(buffer, L"已应用%s参数", fill_kind_label(kind).c_str());
    }
    refresh_history_status(shell, buffer);
    return;
  }

  comp_layer_id id = 0;
  const comp_result result = comp_layer_add_fill(shell.doc.document, kind, json.c_str(), &id);
  if (result != COMP_OK) {
    set_status(shell, L"新建填充图层失败（参数不合法或被锁定）");
    return;
  }
  set_visible_cache(shell, id, static_cast<uint8_t>(1));
  shell.doc.needs_render = true;
  /*
   * 两次 refresh_layers：第一次让新图层进入面板行表，选中它之后再来一次，
   * 让调整/效果/填充三个面板都跟着换数据源（refresh_layers 是唯一汇合点）。
   */
  refresh_layers(shell);
  panel_select_layer_id(shell, id);
  refresh_layers(shell);
  if (focused >= 0 && focused < static_cast<int32_t>(shell.doc.fill_values.size())) {
    shell.doc.fill_param = focused;
  }
  wsprintfW(buffer, L"已新建%s图层 #%I64u（一步撤销）", fill_kind_label(kind).c_str(),
            static_cast<unsigned long long>(id));
  refresh_history_status(shell, buffer);
}

/* 图层 > 新建填充图层 > 纯色 / 渐变 / 图案：开面板 + 立刻按缺省参数建层。 */
void panel_add_fill(ShellState& shell, uint32_t kind) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  open_fill_panel(shell, kind);
  if (!shell.doc.fill_open) return; /* 打开失败已在状态栏说明 */
  panel_apply_fill(shell);
}

/* 编辑当前选中填充层的参数（kind 取自图层本身，不是面板上次用过的 kind）。 */
void panel_edit_fill(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  int32_t is_fill = 0;
  if (id == 0 || comp_layer_is_fill(shell.doc.document, id, &is_fill) != COMP_OK ||
      is_fill == 0) {
    set_status(shell, L"当前图层不是填充图层（图层 > 新建填充图层…）");
    return;
  }
  uint32_t kind = COMP_FILL_SOLID;
  if (comp_layer_get_fill(shell.doc.document, id, &kind, nullptr, 0, nullptr) != COMP_OK) {
    set_status(shell, L"读取填充参数失败");
    return;
  }
  open_fill_panel(shell, kind);
  refresh_fill(shell); /* 新建态 -> 绑定选中层并回填既有参数 */
  wchar_t buffer[224];
  wsprintfW(buffer, L"编辑填充层 #%I64u：%s（拖动/- = 调值 · 回车应用 · Esc 关闭）",
            static_cast<unsigned long long>(id), fill_kind_label(kind).c_str());
  set_status(shell, buffer);
}

/* 栅格化：丢掉参数，像素缓存留下（之后才能直接涂像素 / 上滤镜）。 */
void panel_rasterize_fill(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  int32_t is_fill = 0;
  if (id == 0 || comp_layer_is_fill(shell.doc.document, id, &is_fill) != COMP_OK ||
      is_fill == 0) {
    set_status(shell, L"栅格化填充层：请先选中一个填充图层");
    return;
  }
  const comp_result result = comp_layer_rasterize_fill(shell.doc.document, id);
  shell.doc.needs_render = true;
  refresh_layers(shell); /* fill_kind 归 0：行内 F 标记与过滤随之消失 */
  close_fill_panel(shell); /* 参数已经不存在了，面板留着只会显示缺省值 */
  wchar_t buffer[192];
  wsprintfW(buffer, L"%s（图层 #%I64u）", result == COMP_OK ? L"已栅格化填充图层" : L"栅格化失败",
            static_cast<unsigned long long>(id));
  refresh_history_status(shell, buffer);
}

/* 状态栏报告选中填充层的种类 / 参数项数 / settings 字节数。 */
void panel_fill_status(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  int32_t is_fill = 0;
  if (id == 0 || comp_layer_is_fill(shell.doc.document, id, &is_fill) != COMP_OK ||
      is_fill == 0) {
    set_status(shell, L"当前图层不是填充图层（图层 > 新建填充图层…）");
    return;
  }
  uint32_t kind = COMP_FILL_NONE;
  size_t need = 0;
  if (comp_layer_get_fill(shell.doc.document, id, &kind, nullptr, 0, &need) != COMP_OK) {
    set_status(shell, L"读取填充参数失败");
    return;
  }
  size_t params = 0;
  if (comp_fill_kind_param_count(kind, &params) != COMP_OK) params = 0;
  wchar_t buffer[224];
  wsprintfW(buffer, L"填充层 #%I64u：%s · 参数 %u 项 · settings %u 字节",
            static_cast<unsigned long long>(id), fill_kind_label(kind).c_str(),
            static_cast<unsigned>(params), static_cast<unsigned>(need));
  set_status(shell, buffer);
}

/* ------------------------------------------------------ 填充滑杆（拖动） */

/* 编辑缓冲 -> 绑定的填充层。只在拖动持有事务时写；参数没变不算一次写入。 */
static bool fill_write_live(ShellState& shell) {
  if (shell.doc.document == nullptr || shell.doc.fill_layer == 0) return false;
  if (shell.doc.fill_drag_txn == nullptr) return false;
  std::string json;
  if (!build_fill_settings_json(shell, &json)) return false;
  int32_t changed = 0;
  if (comp_layer_set_fill(shell.doc.document, shell.doc.fill_layer, shell.doc.fill_kind,
                          json.c_str(), &changed) != COMP_OK) {
    return false;
  }
  shell.doc.needs_render = true;
  /* changed == 0 = 参数与当前相同（set_fill 幂等），不能据此把事务标成「有写入」。 */
  return changed != 0 || shell.doc.fill_drag_dirty;
}

void fill_panel_begin_drag(ShellState& shell, const comp_ui::Rect& panel, int32_t row, double x) {
  comp_fill_param param{};
  if (!fill_param_query(shell, row, &param)) return;
  if (param.type == COMP_PARAM_BOOL) return; /* 布尔没有滑道，点击即切换 */

  if (shell.doc.fill_layer != 0) {
    if (shell.doc.document == nullptr) return;
    if (comp_txn_begin(shell.doc.document, "Fill Parameter", &shell.doc.fill_drag_txn) !=
        COMP_OK) {
      shell.doc.fill_drag_txn = nullptr;
      set_status(shell, L"填充滑杆：无法开启事务（未改动）");
      return;
    }
  } else {
    /* 新建态没有可写的对象：拖动只改编辑缓冲，回车才建层（不写历史）。 */
    shell.doc.fill_drag_txn = nullptr;
  }
  shell.doc.fill_param = row;
  shell.doc.fill_input.clear();
  shell.doc.fill_drag_param = row;
  shell.doc.fill_drag_dirty = false;
  /* 几何在按下瞬间冻结：拖动途中面板重排也不会让滑道在指针下跑掉。 */
  shell.doc.fill_drag_track = comp_ui::adjust_slider_track_rect(
      comp_ui::adjust_param_row_rect(panel, row, shell.doc.fill_scroll));
  fill_panel_drag_value(shell, x);
}

/* 把指针 x 换算成值写进编辑缓冲；值没变就不写文档（少一次快照，也少一次重绘）。 */
void fill_panel_drag_value(ShellState& shell, double x) {
  if (shell.doc.fill_drag_param < 0) return;
  comp_fill_param param{};
  if (!fill_param_query(shell, shell.doc.fill_drag_param, &param)) return;
  if (param.type == COMP_PARAM_BOOL) return;
  const size_t index = static_cast<size_t>(shell.doc.fill_drag_param);
  if (index >= shell.doc.fill_values.size()) return;

  const double raw = comp_ui::adjust_slider_value(shell.doc.fill_drag_track, x, param.min_value,
                                                  param.max_value);
  const double value = comp_ui::adjust_snap_value(raw, param.min_value, param.max_value);
  const std::string text = number_text(value);
  if (shell.doc.fill_values[index] == text) return;
  shell.doc.fill_values[index] = text;
  shell.doc.fill_input.clear();
  if (fill_write_live(shell)) shell.doc.fill_drag_dirty = true;
  shell.doc.needs_render = true;

  wchar_t buffer[220];
  wsprintfW(buffer, L"%s = %s（%s）",
            widen_ascii(param.label != nullptr ? param.label : "").c_str(),
            widen_ascii(text.c_str()).c_str(),
            shell.doc.fill_drag_txn != nullptr ? L"实时预览，松手算一步撤销"
                                               : L"预览值，回车新建填充层");
  set_status(shell, buffer);
}

/* commit=false 用于 Esc / 失焦：事务 abort 会回滚到按下前的快照。 */
void fill_panel_drag_finish(ShellState& shell, bool commit) {
  if (shell.doc.fill_drag_param < 0) return;
  const int32_t focused = shell.doc.fill_param;
  const comp_txn txn = shell.doc.fill_drag_txn;
  const bool dirty = shell.doc.fill_drag_dirty;
  /* 先清状态再 ReleaseCapture：捕获变更会重入 WM_CAPTURECHANGED。 */
  shell.doc.fill_drag_param = -1;
  shell.doc.fill_drag_txn = nullptr;
  shell.doc.fill_drag_dirty = false;
  ReleaseCapture();

  bool committed = false;
  if (txn != nullptr) {
    if (commit && dirty) {
      committed = comp_txn_commit(txn) == COMP_OK;
    } else {
      /* 没有实际写入的空事务不该占一步撤销：直接回滚。 */
      comp_txn_abort(txn);
    }
  }
  shell.doc.needs_render = true;
  refresh_fill(shell);
  if (focused >= 0 && focused < static_cast<int32_t>(shell.doc.fill_values.size())) {
    shell.doc.fill_param = focused;
  }

  wchar_t buffer[220];
  if (committed) {
    wsprintfW(buffer, L"已应用填充参数（一步撤销）");
    refresh_history_status(shell, buffer);
  } else if (txn != nullptr && !dirty) {
    wsprintfW(buffer, L"填充参数未改动（未产生撤销步）");
    refresh_history_status(shell, buffer);
  } else if (txn != nullptr) {
    refresh_history_status(shell, L"已回滚填充参数拖动");
  } else {
    set_status(shell, L"填充预览值：回车新建填充层（拖动不写历史）");
  }
}

}  // namespace comp_shell
