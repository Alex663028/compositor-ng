// AI生成
/*
 * shell_channels.cpp — 存储的选区通道面板（ROI #5）
 *
 * 对应上游的 Alpha 通道 / Select > Save Selection / Load Selection。通道**不是**
 * 壳侧列表：它是文档状态，存在引擎里（随 .comp 往返、随撤销回退），壳这边只
 * 负责把 comp_selection_channel_* 五个尾部追加的导出接到面板与键盘上。
 *
 * 面板排版沿用路径/历史面板（ui_core.h 的 channel_row_* 与 draw 同源），
 * 键盘在面板打开时是模态的（与其它右栏面板同规矩）：字符走 WM_CHAR（中文
 * 名字与输入法都从那里来），编辑键走 WM_KEYDOWN。
 */
#include "shell_internal.h"

namespace comp_shell {
namespace {

/* 引擎的名字上限是 255 字节（含中文多字节），加 NUL 后 300 有足够余量。 */
constexpr size_t kChannelNameCapacity = 300;
constexpr size_t kChannelNameMaxBytes = 255;

/* 默认名与 PS 一致：Alpha 1、Alpha 2……按当前条数递增（允许重名，引擎不查重）。 */
std::wstring channel_default_name(int count) {
  wchar_t name[32];
  wsprintfW(name, L"Alpha %d", count + 1);
  return name;
}

void refresh_channel_name_status(ShellState& shell) {
  wchar_t buffer[320];
  if (shell.doc.channel_name_input.empty()) {
    const std::wstring fallback =
        channel_default_name(channel_count(shell));
    wsprintfW(buffer, L"存储选区：键入通道名，或直接回车用「%s」（Esc 取消）",
              fallback.c_str());
  } else {
    wsprintfW(buffer, L"存储选区：名字「%s」（回车保存 / Esc 取消）",
              shell.doc.channel_name_input.c_str());
  }
  set_status(shell, buffer);
}

}  // namespace

/* 文字工具（T）独占右栏显示它自己的属性面板：此时通道面板只是「挂着不显示」。 */
bool channel_panel_visible(const ShellState& shell) {
  return shell.doc.channel_open && shell.tool != comp_ui::ToolId::Type;
}

int channel_count(const ShellState& shell) {
  if (shell.doc.document == nullptr) return 0;
  size_t count = 0;
  if (comp_selection_channel_count(shell.doc.document, &count) != COMP_OK) return 0;
  /* 引擎的 kMaxSelectionChannels：超出只可能是版本错配，面板按上限画。 */
  if (count > static_cast<size_t>(comp_ui::kChannelMaxEntries)) {
    return comp_ui::kChannelMaxEntries;
  }
  return static_cast<int>(count);
}

/* 两段式读名（与 comp_layer_get_name 同一把尺子）：失败给一句占位，不静默空白。 */
std::wstring channel_name(const ShellState& shell, int index) {
  if (shell.doc.document == nullptr || index < 0) return L"（无法读取）";
  char utf8[kChannelNameCapacity] = {0};
  size_t required = 0;
  if (comp_selection_channel_get_name(shell.doc.document, static_cast<size_t>(index), nullptr, 0,
                                      &required) != COMP_OK ||
      required == 0 || required > sizeof(utf8) ||
      comp_selection_channel_get_name(shell.doc.document, static_cast<size_t>(index), utf8,
                                      sizeof(utf8), &required) != COMP_OK) {
    return L"（无法读取）";
  }
  return widen_utf8(utf8);
}

void channel_status(ShellState& shell) {
  const int count = channel_count(shell);
  if (count <= 0) {
    set_status(shell, L"通道：还没有存储的选区（有选区时按 S 或「选择 > 存储选区到通道」）");
    return;
  }
  int index = shell.doc.channel_selected;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  wchar_t buffer[384];
  wsprintfW(buffer,
            L"通道 %d/%d：%s · 回车载入 · Shift+回车加选 · Ctrl+回车减选 · S 存储 · Del 删除",
            index + 1, count, channel_name(shell, index).c_str());
  set_status(shell, buffer);
}

void channel_select(ShellState& shell, int delta) {
  const int count = channel_count(shell);
  if (count <= 0) {
    channel_status(shell);
    return;
  }
  int index = shell.doc.channel_selected + delta;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  shell.doc.channel_selected = index;
  /* 键盘选行要把高亮滚进可视区（与路径/历史面板同一口径）。 */
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.doc.channel_scroll = comp_ui::channel_ensure_visible(
        layout.adjust_panel, count, index, shell.doc.channel_scroll);
  }
  channel_status(shell);
}

/*
 * 载入通道：引擎一侧就是一步撤销（未变化时 out_changed = 0，不留空步）。
 * 与「按图层像素载入选区」走同一条刷新口径；这里**不**套
 * finish_new_selection——那会把壳的「新选区羽化」盖到通道自带的羽化上，
 * 载回来的就不再是存进去的那份选区了。
 */
void channel_load_selected(ShellState& shell, comp_selection_combine combine) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"通道：还没有文档");
    return;
  }
  const int count = channel_count(shell);
  const int index = shell.doc.channel_selected;
  if (count <= 0 || index < 0 || index >= count) {
    set_status(shell, L"通道面板：先选中一条通道");
    return;
  }
  int32_t selection_active = 0;
  comp_selection_get_active(shell.doc.document, &selection_active);

  int32_t changed = 0;
  const comp_result result =
      comp_selection_channel_load(shell.doc.document, static_cast<size_t>(index), combine,
                                  &changed);
  if (result != COMP_OK) {
    set_status(shell,
               result == COMP_ERR_INVALID_ARGUMENT
                   ? L"载入通道失败：通道下标或合并方式无效（未改动）"
                   : L"载入通道失败（未改动）");
    return;
  }
  const std::wstring name = channel_name(shell, index);
  wchar_t buffer[384];
  if (changed == 0) {
    refresh_selection(shell);
    if (combine == COMP_SELECTION_SUBTRACT && selection_active == 0) {
      wsprintfW(buffer, L"通道「%s」：当前没有选区，减选无操作（未改动）", name.c_str());
    } else {
      wsprintfW(buffer, L"通道「%s」：与当前选区相同（未改动）", name.c_str());
    }
    refresh_history_status(shell, buffer);
    return;
  }
  const wchar_t* verb = combine == COMP_SELECTION_ADD      ? L"已把通道「%s」加选进选区"
                        : combine == COMP_SELECTION_SUBTRACT ? L"已从选区减去通道「%s」"
                                                             : L"已载入通道「%s」为选区";
  wsprintfW(buffer, verb, name.c_str());
  lstrcatW(buffer, L"（一步撤销）");
  finish_selection_command(shell, buffer);
}

void channel_delete_selected(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"通道：还没有文档");
    return;
  }
  const int count = channel_count(shell);
  if (count <= 0) {
    set_status(shell, L"通道面板：没有可删除的通道");
    return;
  }
  int index = shell.doc.channel_selected;
  if (index < 0) index = 0;
  if (index >= count) index = count - 1;
  const std::wstring name = channel_name(shell, index);

  int32_t changed = 0;
  const comp_result result =
      comp_selection_channel_remove(shell.doc.document, static_cast<size_t>(index), &changed);
  if (result != COMP_OK || changed == 0) {
    set_status(shell, L"删除通道失败（未改动）");
    return;
  }
  const int remaining = channel_count(shell);
  if (remaining <= 0) {
    shell.doc.channel_selected = 0;
    shell.doc.channel_scroll = 0;
  } else {
    if (shell.doc.channel_selected >= remaining) shell.doc.channel_selected = remaining - 1;
    if (shell.doc.channel_selected < 0) shell.doc.channel_selected = 0;
    Layout layout{};
    if (client_layout(shell, &layout)) {
      shell.doc.channel_scroll = comp_ui::channel_ensure_visible(
          layout.adjust_panel, remaining, shell.doc.channel_selected,
          shell.doc.channel_scroll);
    }
  }
  wchar_t buffer[384];
  if (remaining <= 0) {
    wsprintfW(buffer, L"已删除通道「%s」（列表清空，选区不动，一步撤销）", name.c_str());
  } else {
    wsprintfW(buffer, L"已删除通道「%s」（剩 %d 条，选区不动，一步撤销）", name.c_str(),
              remaining);
  }
  refresh_history_status(shell, buffer);
}

void channel_panel_close(ShellState& shell) {
  shell.doc.channel_open = false;
  shell.doc.channel_name_active = false;
  shell.doc.channel_name_skip_char = false;
  shell.doc.channel_name_input.clear();
}

/* 文档被替换/新建：通道本身随旧文档销毁；壳侧只把面板状态复位。 */
void channel_store_clear(ShellState& shell) {
  channel_panel_close(shell);
  shell.doc.channel_selected = 0;
  shell.doc.channel_scroll = 0;
}

void channel_panel_toggle(ShellState& shell) {
  if (shell.doc.channel_open) {
    channel_panel_close(shell);
    set_status(shell, L"已关闭通道面板");
    return;
  }
  if (shell.doc.document == nullptr) {
    set_status(shell, L"通道面板：还没有文档");
    return;
  }
  /* 右栏一次只挂一个面板：色彩范围持有显式事务，必须先 abort 再让位。 */
  if (shell.doc.color_range_open) color_range_cancel(shell);
  smart_panel_dismiss(shell);
  shell.doc.filter_open = false;
  shell.doc.filter_input.clear();
  filter_preview_forget(shell);
  shell.doc.effect_open = false;
  shell.doc.effect_input.clear();
  shell.doc.path_open = false;
  shell.doc.history_open = false;
  shell.doc.channel_open = true;
  const int count = channel_count(shell);
  if (shell.doc.channel_selected >= count) shell.doc.channel_selected = count - 1;
  if (shell.doc.channel_selected < 0) shell.doc.channel_selected = 0;
  if (count > 0) {
    Layout layout{};
    if (client_layout(shell, &layout)) {
      shell.doc.channel_scroll = comp_ui::channel_ensure_visible(
          layout.adjust_panel, count, shell.doc.channel_selected,
          shell.doc.channel_scroll);
    }
  }
  if (!channel_panel_visible(shell)) {
    set_status(shell, L"通道面板已打开（T 文字工具占着右栏，切走后可见）");
    return;
  }
  channel_status(shell);
}

/*
 * 开始「存储选区」：先查三条拒绝条件（无文档 / 没有选区 / 通道已满），
 * 再开面板并进入命名态。面板没开时顺带打开它——否则用户看不到自己正在
 * 给哪条通道起名。
 */
void channel_save_begin(ShellState& shell) {
  if (shell.doc.document == nullptr) {
    set_status(shell, L"存储通道：还没有文档");
    return;
  }
  const int count = channel_count(shell);
  if (count >= comp_ui::kChannelMaxEntries) {
    wchar_t buffer[192];
    wsprintfW(buffer, L"存储通道：已达 %d 条上限，先删一条再存",
              comp_ui::kChannelMaxEntries);
    set_status(shell, buffer);
    return;
  }
  int32_t active = 0;
  if (comp_selection_get_active(shell.doc.document, &active) != COMP_OK || active == 0) {
    set_status(shell, L"存储通道：先建立选区（框选 / 套索 / 魔棒 / 全选）");
    return;
  }
  if (!shell.doc.channel_open) channel_panel_toggle(shell);
  /*
   * 文字工具（T）独占右栏时面板只是挂着不显示：命名态的键盘收不到，存了也
   * 看不见自己起了什么名字——直接拒绝，别让用户对着空白画面输入。
   */
  if (!channel_panel_visible(shell)) {
    set_status(shell, L"存储通道：文字工具（T）占着右栏，先切走再存储");
    return;
  }
  shell.doc.channel_name_active = true;
  shell.doc.channel_name_skip_char = false;
  /* 空缓冲 = 用默认名；预填反而让用户每敲一个字都得先退格一轮。 */
  shell.doc.channel_name_input.clear();
  refresh_channel_name_status(shell);
}

void channel_name_char(ShellState& shell, wchar_t ch) {
  if (!shell.doc.channel_name_active) return;
  /* 开命名的那一次按键还会以 WM_CHAR 形式回来一次（见 shell_state.h）。 */
  if (shell.doc.channel_name_skip_char) {
    shell.doc.channel_name_skip_char = false;
    return;
  }
  /* 控制字符（回车/退格/Esc 在 WM_KEYDOWN 已处理）不进名字。 */
  if (ch < 32 || ch == 127) return;
  std::wstring next = shell.doc.channel_name_input;
  next.push_back(ch);
  /* 上限按**转换后的字节数**算：中文一个字 3 字节，按 wchar 计数会放过超长名，
   * 到保存时才被引擎拒绝。 */
  if (narrow_utf8(next).size() > kChannelNameMaxBytes) {
    set_status(shell, L"通道名太长：最多 255 字节（中文约 85 字）");
    return;
  }
  shell.doc.channel_name_input.swap(next);
  refresh_channel_name_status(shell);
}

void channel_name_backspace(ShellState& shell) {
  if (!shell.doc.channel_name_active) return;
  if (!shell.doc.channel_name_input.empty()) shell.doc.channel_name_input.pop_back();
  refresh_channel_name_status(shell);
}

void channel_name_commit(ShellState& shell) {
  if (!shell.doc.channel_name_active) return;
  if (shell.doc.document == nullptr) {
    channel_name_cancel(shell);
    return;
  }
  const int count = channel_count(shell);
  std::wstring name = shell.doc.channel_name_input;
  if (name.empty()) name = channel_default_name(count);
  const std::string utf8 = narrow_utf8(name);
  if (utf8.empty() || utf8.size() > kChannelNameMaxBytes) {
    set_status(shell, L"通道名无效：需要 1..255 字节");
    return;
  }

  size_t index = 0;
  const comp_result result =
      comp_selection_channel_save(shell.doc.document, utf8.c_str(), &index);
  if (result != COMP_OK) {
    /* 命名态保持打开：用户改一下名字再回车，不必从头来。 */
    if (result == COMP_ERR_STATE) {
      const int now = channel_count(shell);
      int32_t active = 0;
      comp_selection_get_active(shell.doc.document, &active);
      if (now >= comp_ui::kChannelMaxEntries) {
        set_status(shell, L"存储通道失败：已达 16 条上限（先删一条）");
      } else if (active == 0) {
        set_status(shell, L"存储通道失败：当前没有选区");
      } else {
        set_status(shell, L"存储通道失败：引擎拒绝（状态不允许）");
      }
    } else if (result == COMP_ERR_INVALID_ARGUMENT) {
      set_status(shell, L"存储通道失败：名字无效（检查控制字符）");
    } else {
      set_status(shell, L"存储通道失败");
    }
    return;
  }

  shell.doc.channel_name_active = false;
  shell.doc.channel_name_skip_char = false;
  shell.doc.channel_name_input.clear();
  shell.doc.channel_selected =
      static_cast<int>(index) < comp_ui::kChannelMaxEntries
          ? static_cast<int>(index)
          : comp_ui::kChannelMaxEntries - 1;
  Layout layout{};
  if (client_layout(shell, &layout)) {
    shell.doc.channel_scroll = comp_ui::channel_ensure_visible(
        layout.adjust_panel, channel_count(shell), shell.doc.channel_selected,
        shell.doc.channel_scroll);
  }
  wchar_t buffer[384];
  wsprintfW(buffer, L"已把当前选区存为通道「%s」（第 %d / %d 条，一步撤销）",
            name.c_str(), shell.doc.channel_selected + 1, channel_count(shell));
  refresh_history_status(shell, buffer);
}

void channel_name_cancel(ShellState& shell) {
  if (!shell.doc.channel_name_active) return;
  shell.doc.channel_name_active = false;
  shell.doc.channel_name_skip_char = false;
  shell.doc.channel_name_input.clear();
  set_status(shell, L"已取消存储通道（未写入）");
}

}  // namespace comp_shell
