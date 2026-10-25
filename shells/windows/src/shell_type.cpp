// AI生成
/*
 * shell_type.cpp — 文字工具与文字面板
 *
 * 从原 main.cpp 拆出（M3 拆分第二步），行为未变：状态在 shell_state.h，
 * 跨文件契约在 shell_internal.h。
 */
#include "shell_internal.h"

namespace comp_shell {


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
  return shell.doc.document != nullptr && shell.tool == comp_ui::ToolId::Type;
}

/* 字体轨道开关的实际取值：句柄没加载成功就只能是 5×7（不静默借系统字体）。 */
bool type_font_track(const ShellState& shell) {
  return shell.type_use_font && shell.type_font != nullptr;
}

/*
 * 预览度量：两条轨道量纲不同（点阵是格子、字体是 em），调用点不应该各写一次
 * 分支——状态栏、面板数字、虚线框三处必须给出同一组值。
 */
comp_ui::TypeBlockMetrics type_metrics(const ShellState& shell) {
  if (type_font_track(shell)) {
    return comp_ui::type_block_metrics_font(
        shell.doc.type_font_size, shell.doc.type_line_height, shell.doc.type_letter_spacing,
        shell.doc.type_text.c_str(), shell.type_font_ascender, shell.type_font_descender);
  }
  return comp_ui::type_block_metrics(shell.doc.type_font_size, shell.doc.type_line_height,
                                     shell.doc.type_letter_spacing, shell.doc.type_text.c_str());
}

/*
 * 预览框尺寸：声明了段落框宽/框高就用声明值（与引擎写入的图层缓冲同口径，
 * 框高只裁切），没声明的维度仍按内容度量。框内文字的折行位置是引擎的事，
 * 预览不预演字形（S11 的既定口径），只给框。
 */
comp_ui::TypeBlockMetrics type_preview_metrics(const ShellState& shell) {
  comp_ui::TypeBlockMetrics metrics = type_metrics(shell);
  if (shell.doc.type_box_width > 0.0) metrics.width = shell.doc.type_box_width;
  if (shell.doc.type_box_height > 0.0) metrics.height = shell.doc.type_box_height;
  return metrics;
}

void type_cancel(ShellState& shell) {
  shell.doc.type_active = false;
  shell.doc.type_text.clear();
  shell.doc.type_pending_high = 0;
  shell.doc.type_skip_char = false;
}

/*
 * 一个 UTF-16 码元进入缓冲（WM_CHAR / 输入法提交的共同出口）。
 *
 * 控制字符（回车、退格、Esc、Tab）由 WM_KEYDOWN 处理，这里一律挡掉——否则
 * 回车会被写成 0x0D，引擎如实报 kBadText，用户看到的是「打字把文本框弄坏了」。
 * 代理对的两半分别到达：先存高位，低位拼成完整码点再写（孤立的一半直接丢弃）。
 * 返回是否真的写入了字符（调用方据此决定要不要刷状态栏）。
 */
bool type_char_input(ShellState& shell, wchar_t unit) {
  if (shell.doc.type_skip_char) {
    shell.doc.type_skip_char = false;
    return false;
  }
  uint32_t codepoint = 0;
  const uint32_t value = static_cast<uint32_t>(unit);
  if (value >= 0xD800u && value <= 0xDBFFu) { /* 高代理：等低位 */
    shell.doc.type_pending_high = unit;
    return false;
  }
  if (value >= 0xDC00u && value <= 0xDFFFu) { /* 低代理：必须配上一个高位 */
    if (shell.doc.type_pending_high == 0) return false;
    const uint32_t high = static_cast<uint32_t>(shell.doc.type_pending_high) - 0xD800u;
    const uint32_t low = value - 0xDC00u;
    shell.doc.type_pending_high = 0;
    codepoint = 0x10000u + (high << 10) + low;
  } else {
    shell.doc.type_pending_high = 0; /* 高代理后面不是低位：丢弃那一半 */
    codepoint = value;
  }
  if (codepoint < 0x20u || codepoint == 0x7Fu || codepoint > 0x10FFFFu) return false;
  comp_ui::type_utf8_append(shell.doc.type_text, codepoint);
  return true;
}

/* 状态栏：空缓冲时给快捷键提示，有内容时给行数/文本框尺寸/排版值。 */
void type_refresh_status(ShellState& shell) {
  const comp_ui::TypeBlockMetrics metrics = type_preview_metrics(shell);
  const int size_px = static_cast<int>(shell.doc.type_font_size + 0.5);
  const int line_pct = static_cast<int>(shell.doc.type_line_height * 100.0 + 0.5);
  const int spacing_px = static_cast<int>(std::lround(shell.doc.type_letter_spacing));
  const int width_px = static_cast<int>(std::lround(metrics.width));
  const int height_px = static_cast<int>(std::lround(metrics.height));
  const wchar_t* track = type_font_track(shell)
                             ? shell.type_font_label.c_str()
                             : L"5×7 点阵";
  /* P0-6③：只在真的设了段落参数时才追加——默认的点文字状态栏一个字都不多。 */
  wchar_t paragraph[96] = L"";
  if (shell.doc.type_box_width > 0.0 || shell.doc.type_box_height > 0.0 ||
      shell.doc.type_align != comp_ui::kTypeAlignLeft) {
    wchar_t width_text[24] = L"自动";
    wchar_t height_text[24] = L"自动";
    if (shell.doc.type_box_width > 0.0) {
      wsprintfW(width_text, L"%d", static_cast<int>(shell.doc.type_box_width + 0.5));
    }
    if (shell.doc.type_box_height > 0.0) {
      wsprintfW(height_text, L"%d", static_cast<int>(shell.doc.type_box_height + 0.5));
    }
    wsprintfW(paragraph, L"｜段落框 %s×%s px，%s", width_text, height_text,
              widen_ascii(comp_ui::type_align_name(shell.doc.type_align)).c_str());
  }
  wchar_t buffer[384];
  if (shell.doc.type_text.empty()) {
    wsprintfW(buffer,
              L"文字：插入点 (%d, %d)｜字体 %s，字号 %d px（%s），行距 %d%%，字距 %d px%s｜"
              L"直接键入（中文用输入法），Shift+Enter 换行，回车提交，Esc 取消；"
              L"[ / ] 字号，Shift+[ / ] 字距，Alt+[ / ] 行距",
              static_cast<int>(std::floor(shell.doc.type_origin.x)),
              static_cast<int>(std::floor(shell.doc.type_origin.y)), track, size_px,
              type_font_track(shell) ? L"em 高" : L"点阵格高", line_pct, spacing_px, paragraph);
  } else {
    /* 状态栏是单行：把换行显示成 '|'，不把控制字符塞进窗口标题。 */
    std::string display;
    display.reserve(shell.doc.type_text.size());
    for (char ch : shell.doc.type_text) display.push_back(ch == '\n' ? '|' : ch);
    /* 字体轨道的宽度是估计值：数字前面加「≈」，不让估计值冒充引擎结果。 */
    if (type_font_track(shell)) {
      wsprintfW(buffer, L"文字：%s｜%d 行 ≈%d×%d px，字体 %s（em %d），行距 %d%%，字距 %d%s｜"
                        L"回车提交，Esc 取消",
                widen_utf8(display).c_str(), metrics.line_count, width_px, height_px,
                track, size_px, line_pct, spacing_px, paragraph);
    } else {
      wsprintfW(buffer, L"文字：%s｜%d 行 %d×%d px，字号 %d，行距 %d%%，字距 %d%s｜"
                        L"回车提交，Esc 取消",
                widen_utf8(display).c_str(), metrics.line_count, width_px, height_px,
                size_px, line_pct, spacing_px, paragraph);
    }
  }
  set_status(shell, buffer);
}

void type_begin(HWND window, ShellState& shell, int x, int y, const Layout& layout) {
  if (!type_available(shell)) return;
  (void)window;
  /* 已经在键入时再点一次 = 先提交上一段文字，再在新位置重新落点（不丢已键入内容）。 */
  if (shell.doc.type_active && !shell.doc.type_text.empty()) type_commit(shell);
  shell.doc.type_origin = shell.doc.view.screen_to_doc(x, y, layout.canvas, shell.doc.width, shell.doc.height);
  shell.doc.type_active = true;
  shell.doc.type_text.clear();
  shell.doc.type_pending_high = 0;
  shell.doc.type_skip_char = false;
  /* 落插入点即离开属性面板的数值键入态，否则数字键会被面板缓冲吃掉。 */
  shell.doc.type_input.clear();
  /*
   * 同理关掉效果/滤镜/智能滤镜栈面板：这几块的键盘分支排在文字键入态之前，
   * 留着会把刚点的这一笔字符全吞掉（用户看到插入点在闪、打字没反应）。
   */
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell); /* 文字工具接管键盘：别让滤镜预览帧留在画布上 */
  smart_panel_dismiss(shell);
  type_refresh_status(shell);
}

/*
 * 是否可编辑文字图层。判定直接用 ABI 自己的口径：查询模式下 comp_layer_get_text
 * 对文字层返回 COMP_OK，对普通像素层返回 COMP_ERR_STATE——外壳不另设一份真相。
 */
bool layer_is_text(ShellState& shell, comp_layer_id id) {
  if (shell.doc.document == nullptr || id == 0) return false;
  size_t required = 0;
  return comp_layer_get_text(shell.doc.document, id, nullptr, nullptr, 0, &required) == COMP_OK;
}

/*
 * 插入点是否落在选中文字图层的矩形内（= 用户想改这一段文字）。
 *
 * 判据用 comp_layer_get_transform 的实话：文字层的矩形就是文本框，落点在框内
 * 才是「编辑这一段」；点在别处是「另起一段文字」，否则第二次提交会把上一段
 * 悄悄搬到新位置（用户看到的是上一段文字消失）。
 */
static bool type_origin_inside_layer(ShellState& shell, comp_layer_id id) {
  comp_rect box{};
  float rotation = 0.0f;
  if (comp_layer_get_transform(shell.doc.document, id, &box, &rotation) != COMP_OK) return false;
  const double x = shell.doc.type_origin.x;
  const double y = shell.doc.type_origin.y;
  return x >= static_cast<double>(box.x) && y >= static_cast<double>(box.y) &&
         x < static_cast<double>(box.x) + static_cast<double>(box.w) &&
         y < static_cast<double>(box.y) + static_cast<double>(box.h);
}

/*
 * 回车提交：**产出一个可编辑文字图层**，不再把字形画进当前层的像素。
 *
 * 两条通路，一次提交只记一步撤销：
 *   - 插入点落在选中的文字层框内 -> comp_layer_set_text（改文本/字号/颜色，重渲这层）；
 *   - 否则 -> comp_layer_add_text（新建一层文字图层，像素只是它的渲染缓存）。
 * 参数（含字体文件打不开）在引擎里先于任何副作用校验：失败不建层、不进撤销栈。
 * 空缓冲不提交：没有可排的字形，留插入点等用户输入。
 */
void type_commit(ShellState& shell) {
  if (!shell.doc.type_active) return;
  if (shell.doc.type_text.empty()) {
    set_status(shell, L"文字：还没有输入内容（键入文字后回车提交，Esc 取消）");
    return;
  }
  if (shell.doc.document == nullptr) {
    set_status(shell, L"文字：无文档");
    return;
  }

  comp_text_desc desc{};
  desc.text = shell.doc.type_text.c_str();
  desc.text_length = static_cast<int32_t>(shell.doc.type_text.size());
  desc.origin_x = shell.doc.type_origin.x;
  desc.origin_y = shell.doc.type_origin.y;
  desc.style.font_size = shell.doc.type_font_size;
  desc.style.line_height = shell.doc.type_line_height;
  desc.style.letter_spacing = shell.doc.type_letter_spacing;
  desc.style.antialias = 1;
  desc.color[0] = shell.foreground.r;
  desc.color[1] = shell.foreground.g;
  desc.color[2] = shell.foreground.b;
  desc.color[3] = 255;
  desc.opacity = 1.0f;
  /*
   * 字体轨道由 font_path 决定（comp_text_desc 没有 comp_font 句柄）：随仓字体
   * 加载成功时把它的文件路径写进 .comp，重开时同一份字体会被原样找回；走内置
   * 5×7 点阵时传 NULL。路径为空但开关还开着不可能发生——type_font_track 已经
   * 要求句柄非空，而句柄只由 load_shipped_font 在同一处给出路径。
   */
  desc.font_path_utf8 =
      (type_font_track(shell) && !shell.type_font_path.empty()) ? shell.type_font_path.c_str()
                                                                : nullptr;
  /* 面号来自字体面板（.ttc 集合）；内置点阵轨道恒 0——ABI 本来也只在路径非空时采信。 */
  desc.font_face_index = type_font_track(shell) ? shell.type_face_index : 0;
  /*
   * P0-6③：段落框与对齐。三项全默认（0 / 0 / 左）时引擎走的是 P0-6② 之前
   * 逐字节相同的单次栅格化路径，老工程的落墨位置不会有任何漂移。
   */
  desc.box_width = shell.doc.type_box_width;
  desc.box_height = shell.doc.type_box_height;
  desc.align = shell.doc.type_align;
  desc._reserved = 0;
  desc._reserved2 = 0;

  const comp_layer_id selected = selected_layer_id(shell);
  const bool editing = layer_is_text(shell, selected) && type_origin_inside_layer(shell, selected);
  comp_layer_id id = selected;
  const comp_result result =
      editing ? comp_layer_set_text(shell.doc.document, selected, &desc)
              : comp_layer_add_text(shell.doc.document, &desc, &id);
  shell.doc.needs_render = true;

  /* 无论成功与否都退出键入态：成功已提交，失败也没有可保留的预览语义。 */
  const int lines = type_metrics(shell).line_count;
  shell.doc.type_active = false;
  shell.doc.type_text.clear();
  shell.doc.type_pending_high = 0;
  shell.doc.type_skip_char = false;

  wchar_t buffer[384];
  if (result != COMP_OK) {
    wsprintfW(buffer, L"文字失败（%d）%s", static_cast<int>(result),
              result == COMP_ERR_IO ? L"：字体文件打不开" : L"");
    set_status(shell, buffer);
    return;
  }
  if (!editing) {
    /* 新层要进可见性缓存与面板列表，并成为当前层——接着打字就是改这一层。 */
    set_visible_cache(shell, id, static_cast<uint8_t>(1));
    refresh_layers(shell);
    panel_select_layer_id(shell, id);
    wsprintfW(buffer, L"文字：已新建文字图层（%d 行，图层 > 栅格化文字 可转为像素层）",
              lines);
  } else {
    refresh_layers(shell);
    wsprintfW(buffer, L"文字：已更新文字图层（%d 行）", lines);
  }
  refresh_history_status(shell, buffer);
}

/*
 * 图层 > 栅格化文字：文字对象丢弃、像素留下，此后就是普通像素层。
 * 引擎对非文字层幂等返回 OK 且不记撤销步；这里给出明确提示，不做假动作。
 */
void panel_rasterize_text(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"无文档");
    return;
  }
  const comp_layer_id id = selected_layer_id(shell);
  if (id == 0) {
    set_status(shell, L"栅格化文字：请先选择一个图层");
    return;
  }
  if (!layer_is_text(shell, id)) {
    set_status(shell, L"栅格化文字：当前图层不是文字图层");
    return;
  }
  if (comp_layer_rasterize_text(shell.doc.document, id) != COMP_OK) {
    set_status(shell, L"栅格化文字失败");
    return;
  }
  shell.doc.needs_render = true;
  refresh_layers(shell);
  refresh_history_status(shell, L"栅格化文字：已完成（该层现在是像素层）");
}

/* [ / ] 调字号，Shift+[ / ] 调字距；只在文字工具内生效（其余工具沿用笔刷/魔棒）。 */
void type_step_param(ShellState& shell, int direction, bool spacing) {
  if (spacing) {
    shell.doc.type_letter_spacing =
        comp_ui::step_type_letter_spacing(shell.doc.type_letter_spacing, shell.doc.type_font_size,
                                          direction);
  } else {
    shell.doc.type_font_size = comp_ui::step_type_font_size(shell.doc.type_font_size, direction);
    /* 字号变小会让原字距超出「步进 >= 0.5 px」的底限：跟着重新夹取。 */
    shell.doc.type_letter_spacing =
        comp_ui::clamp_type_letter_spacing(shell.doc.type_letter_spacing, shell.doc.type_font_size);
  }
  if (shell.doc.type_active) {
    type_refresh_status(shell);
  } else {
    wchar_t buffer[192];
    wsprintfW(buffer,
              L"文字：字号 %d px，行距 %d%%，字距 %d px（[ / ] 字号，Shift+[ / ] 字距，"
              L"Alt+[ / ] 行距）",
              static_cast<int>(shell.doc.type_font_size + 0.5),
              static_cast<int>(shell.doc.type_line_height * 100.0 + 0.5),
              static_cast<int>(std::lround(shell.doc.type_letter_spacing)));
    set_status(shell, buffer);
  }
}

/* ------------------------------------------------------------ 文字属性面板（S14） */

/*
 * 四行属性面板（字号 / 行距 / 字距 / 字体）。前三行的值写回 ShellState 的
 * type_* 字段，第四行切换轨道；面板只是第二条操作通路：引擎参数、状态栏、
 * 文本框预览都读同一组值。行几何、夹取、步进全部来自 ui_core.h，绘制与命中
 * 不会漂移。
 */
bool type_panel_visible(const ShellState& shell) {
  /* 字体 / 智能滤镜栈面板开着时右栏归它们（同一块区域，画两层只会互相咬字）。 */
  return shell.tool == comp_ui::ToolId::Type && !shell.doc.filter_open &&
         !shell.doc.smart_open && !font_panel_visible(shell);
}

double type_panel_value(const ShellState& shell, int row) {
  switch (row) {
    case comp_ui::kTypePanelFontSizeRow: return shell.doc.type_font_size;
    case comp_ui::kTypePanelLineHeightRow: return shell.doc.type_line_height;
    case comp_ui::kTypePanelLetterSpacingRow: return shell.doc.type_letter_spacing;
    default: return 0.0;
  }
}

std::wstring type_panel_value_text(const ShellState& shell, int row) {
  wchar_t buffer[96];
  if (row == comp_ui::kTypePanelLineHeightRow) {
    wsprintfW(buffer, L"%d%%", static_cast<int>(shell.doc.type_line_height * 100.0 + 0.5));
  } else if (row == comp_ui::kTypePanelFontSizeRow) {
    wsprintfW(buffer, L"%d px", static_cast<int>(shell.doc.type_font_size + 0.5));
  } else if (row == comp_ui::kTypePanelFontRow) {
    /* 字体行显示的是「当前会用到哪个字体」，不是字号——字号量纲对它没有意义。 */
    wsprintfW(buffer, L"%s", type_font_track(shell) ? shell.type_font_label.c_str()
                                                    : L"5×7 点阵");
  } else {
    wsprintfW(buffer, L"%d px", static_cast<int>(std::lround(shell.doc.type_letter_spacing)));
  }
  return std::wstring(buffer);
}

/*
 * 写值的唯一入口：改字号后必须重新夹字距（字号变小会让原字距压碎步距），
 * 与 type_step_param 的规则保持一致。
 */
void type_panel_set(ShellState& shell, int row, double value) {
  switch (row) {
    case comp_ui::kTypePanelFontSizeRow:
      shell.doc.type_font_size = comp_ui::clamp_type_font_size(value);
      shell.doc.type_letter_spacing =
          comp_ui::clamp_type_letter_spacing(shell.doc.type_letter_spacing, shell.doc.type_font_size);
      break;
    case comp_ui::kTypePanelLineHeightRow:
      shell.doc.type_line_height = comp_ui::clamp_type_line_height(value);
      break;
    case comp_ui::kTypePanelLetterSpacingRow:
      shell.doc.type_letter_spacing =
          comp_ui::clamp_type_letter_spacing(value, shell.doc.type_font_size);
      break;
    default:
      break;
  }
}

/* 键入中的文字也用这组值：此时状态栏走 type_refresh_status，不另写一套数字。 */
void type_panel_status(ShellState& shell, const wchar_t* prefix) {
  if (shell.doc.type_active) {
    type_refresh_status(shell);
    return;
  }
  const int row = shell.type_panel_focused;
  wchar_t buffer[256];
  wsprintfW(buffer, L"%s%s = %s", prefix,
            widen_ascii(comp_ui::type_panel_row_label(row)).c_str(),
            type_panel_value_text(shell, row).c_str());
  set_status(shell, buffer);
}

void type_panel_focus(ShellState& shell, int delta) {
  int next = shell.type_panel_focused + delta;
  if (next < 0) next = 0;
  if (next >= comp_ui::kTypePanelRowCount) next = comp_ui::kTypePanelRowCount - 1;
  shell.type_panel_focused = next;
  type_panel_status(shell, L"文字属性 ");
}

/*
 * 字体行切换：字体轨道 <-> 5×7 点阵。两条轨道的 font_size 量纲不同（em / 格子），
 * 切换时**不改数值**——用户调好的 48 px 在字体轨道是 48 px 的 em，回到点阵就是
 * 48 px 的格子；硬换算只会制造一个没人要的 5.2。状态栏说清楚当前量纲即可。
 */
void type_panel_toggle_font(ShellState& shell) {
  if (shell.type_font == nullptr) {
    set_status(shell, L"字体：没有加载随仓字体，只能用 5×7 点阵（ASCII）");
    return;
  }
  shell.type_use_font = !shell.type_use_font;
  wchar_t buffer[192];
  wsprintfW(buffer, L"字体 = %s（%s）",
            shell.type_use_font ? shell.type_font_label.c_str() : L"5×7 点阵",
            shell.type_use_font ? L"UTF-8/CJK，字号按 em 高" : L"仅 ASCII，字号按格子高");
  if (shell.doc.type_active) {
    type_refresh_status(shell);
  } else {
    set_status(shell, buffer);
  }
}

void type_panel_step(ShellState& shell, int direction) {
  const int row = shell.type_panel_focused;
  if (comp_ui::type_panel_row_is_toggle(row)) {
    type_panel_toggle_font(shell);
    return;
  }
  const double before = type_panel_value(shell, row);
  const double next = comp_ui::type_panel_step_value(row, before, shell.doc.type_font_size, direction);
  type_panel_set(shell, row, next);
  type_panel_status(shell, type_panel_value(shell, row) == before ? L"已到边界：" : L"");
}

void type_panel_begin_input(ShellState& shell, wchar_t character) {
  if (shell.doc.type_active) return;
  shell.doc.type_input.push_back(character);
  wchar_t buffer[96];
  wsprintfW(buffer, L"输入中：%s", shell.doc.type_input.c_str());
  set_status(shell, buffer);
}

void type_panel_backspace(ShellState& shell) {
  if (shell.doc.type_input.empty()) return;
  shell.doc.type_input.pop_back();
  if (shell.doc.type_input.empty()) set_status(shell, L"已取消输入");
}

/* 键入值先夹到面板量程再落值（与滤镜面板同一套输入规矩）。 */
void type_panel_commit_input(ShellState& shell) {
  if (shell.doc.type_input.empty()) return;
  const std::string text(shell.doc.type_input.begin(), shell.doc.type_input.end());
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
    set_status(shell, L"输入不是合法数值，未提交");
    return;
  }
  type_panel_set(shell, shell.type_panel_focused, value);
  shell.doc.type_input.clear();
  type_panel_status(shell, L"已设置 ");
}

}  // namespace comp_shell
