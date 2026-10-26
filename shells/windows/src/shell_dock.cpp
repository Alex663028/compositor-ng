// AI生成
/*
 * shell_dock.cpp — 停靠子系统的实现。
 *
 * 纯数据 + 纯函数：不碰 windows.h，不碰引擎，只产出数字与矩形。
 */
#include "shell_dock.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace comp_dock {
namespace {

int index_of(PanelId id) { return static_cast<int>(id); }

const char* panel_key(PanelId id) { return id == PanelId::Adjust ? "adjust" : "layers"; }

/* 解析 "x,y,w,h"。任何一段不是数字就整条作废（宁可回缺省，不留半个几何）。 */
bool parse_rect(const std::string& text, int* out_x, int* out_y, int* out_w, int* out_h) {
  int values[4] = {0, 0, 0, 0};
  int count = 0;
  size_t start = 0;
  while (count < 4) {
    const size_t comma = text.find(',', start);
    const std::string part =
        text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (part.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(part.c_str(), &end, 10);
    if (end == part.c_str() || *end != '\0') return false;
    values[count++] = static_cast<int>(value);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (count != 4) return false;
  *out_x = values[0];
  *out_y = values[1];
  *out_w = values[2];
  *out_h = values[3];
  return true;
}

/* 去掉首尾空白。**必须连 '\n' 一起去**：fgets 读出来的行带着换行符，
 * 不去掉的话 "left\n" 跟 "left" 比不相等，存回去的停靠区会全部读成缺省。 */
std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  const auto blank = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (begin < end && blank(text[begin])) ++begin;
  while (end > begin && blank(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

}  // namespace

DockState DockState::defaults() {
  DockState state;
  state.panels[index_of(PanelId::Adjust)].area = DockArea::Right;
  state.panels[index_of(PanelId::Layers)].area = DockArea::Left;
  return state;
}

DockArea DockState::area_of(PanelId id) const {
  return panels[index_of(id)].area;
}

bool DockState::is_floating(PanelId id) const {
  return panels[index_of(id)].area == DockArea::Floating;
}

bool DockState::is_hidden(PanelId id) const {
  return panels[index_of(id)].area == DockArea::Hidden;
}

int DockState::floating_count() const {
  int count = 0;
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    if (panels[i].area == DockArea::Floating) ++count;
  }
  return count;
}

int DockState::width_of(PanelId id, int fallback) const {
  const int width = panels[index_of(id)].width;
  return width > 0 ? width : fallback;
}

PanelState& DockState::panel(PanelId id) { return panels[index_of(id)]; }

const PanelState& DockState::panel(PanelId id) const { return panels[index_of(id)]; }

void DockState::set_area(PanelId id, DockArea area) {
  PanelState& target = panels[index_of(id)];
  if (target.area == area) return;
  if (area == DockArea::Floating) {
    /* 浮起来之前先记住原停靠区：停回来时要回到原位。 */
    if (target.area != DockArea::Floating) {
      last_docked_[index_of(id)] = target.area == DockArea::Left ? DockArea::Left
                                                                  : DockArea::Right;
    }
    target.area = DockArea::Floating;
    target.collapsed = false; /* 浮动窗口没有「竖条」形态 */
    return;
  }
  if (target.area == DockArea::Floating) {
    last_docked_[index_of(id)] = area;
  }
  /*
   * 目标列已有另一块面板：**两块对调**（这是「这块面板独占目标列」的语义，
   * 与 group_move 的「合并成一组」是两种不同的意图）。
   *
   * 「视图 > 面板停靠左侧/右侧」走的是独占：调整面板去左列，图层栏让到右缘。
   * 「视图 > 面板分组 > 合并成一组」走的是合并：两块住进同一列，列顶出标签栏。
   * 两种意图必须分开——混在一起的话「停靠左侧」会把用户拖成组。
   */
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    PanelState& other = panels[i];
    if (i != index_of(id) && other.area == area) {
      other.area = (area == DockArea::Left) ? DockArea::Right : DockArea::Left;
      last_docked_[i] = other.area;
    }
  }
  target.area = area;
  last_docked_[index_of(id)] = area;
}

void DockState::set_width(PanelId id, int width) {
  panels[index_of(id)].width = width > 0 ? width : 0;
}

void DockState::toggle_collapsed(PanelId id) {
  PanelState& target = panels[index_of(id)];
  if (target.area == DockArea::Floating) return; /* 浮动窗口没有收起态 */
  target.collapsed = !target.collapsed;
}

void DockState::float_panel(PanelId id, int x, int y, int width, int height) {
  PanelState& target = panels[index_of(id)];
  if (target.area != DockArea::Floating) {
    last_docked_[index_of(id)] =
        target.area == DockArea::Left ? DockArea::Left : DockArea::Right;
  }
  target.area = DockArea::Floating;
  target.collapsed = false;
  target.float_x = x;
  target.float_y = y;
  target.float_w = width;
  target.float_h = height;
}

void DockState::dock_panel(PanelId id) {
  PanelState& target = panels[index_of(id)];
  if (target.area != DockArea::Floating) return;
  target.area = last_docked_[index_of(id)];
  target.float_w = 0;
  target.float_h = 0;
}

void DockState::apply_to(comp_ui::LayoutPrefs& prefs) const {
  const PanelState& adjust = panels[index_of(PanelId::Adjust)];
  const PanelState& layers = panels[index_of(PanelId::Layers)];

  const bool adjust_floating = adjust.area == DockArea::Floating;
  const bool layers_floating = layers.area == DockArea::Floating;
  const bool adjust_hidden = adjust.area == DockArea::Hidden;
  const bool layers_hidden = layers.area == DockArea::Hidden;

  /*
   * 分组模型：**同一列里的面板构成一组**（由 area 推导，单一真源）。
   *
   * 一列里若有两块面板（都是 Left 或都是 Right），该列就是一组：
   * 列顶出现标签栏，两块面板共用一个矩形，同一时刻只显示激活的那块。
   *
   * 关键推论：**一块面板被浮走后，它就不再属于任何列**（area = Floating），
   * 那一列就只剩一块面板 → 组自动解散，标签栏消失。这与 PS 一致：
   * 把面板拖出成浮窗，它就离开了那个组。
   * 停回来时（dock_panel）回到 last_docked_ 记录的列，组重新出现。
   */
  const int right_count = group_count(DockArea::Right);
  const int left_count = group_count(DockArea::Left);
  const bool grouped = right_count > 1 || left_count > 1;

  /*
   * 组模式下，激活成员决定这一列画谁；未激活的那块**不占矩形**
   * （rect 为空 = 不画、也不参与命中）。
   */
  const PanelId right_active = group_active_panel(DockArea::Right);
  const PanelId left_active = group_active_panel(DockArea::Left);

  prefs.adjust_docked = !adjust_floating && !adjust_hidden;
  prefs.layer_docked = !layers_floating && !layers_hidden;

  /*
   * 宽度按**列**给，不按面板给。
   *
   * 一列里叠了多块面板时，列宽是这一列共用的（PS 的组就是这个语义）：
   * 切标签不改变列宽，画布宽度因此不动。有组宽就用组宽；
   * 没有（未分组）就用该列激活成员的缺省宽度。
   */
  const int left_group_w = group_width(DockArea::Left);
  const int right_group_w = group_width(DockArea::Right);
  const int left_default = left_active == comp_dock::PanelId::Layers
                               ? static_cast<int>(comp_ui::Layout::kLayerPanelWidth)
                               : static_cast<int>(comp_ui::Layout::kAdjustPanelWidth);
  const int right_default = right_active == comp_dock::PanelId::Layers
                                ? static_cast<int>(comp_ui::Layout::kLayerPanelWidth)
                                : static_cast<int>(comp_ui::Layout::kAdjustPanelWidth);
  prefs.adjust_panel_width = right_group_w > 0 ? right_group_w : right_default;
  prefs.layer_panel_width = left_group_w > 0 ? left_group_w : left_default;
  prefs.adjust_panel_collapsed = adjust.collapsed;
  prefs.layer_panel_collapsed = layers.collapsed;
  /*
   * 停左：调整面板在左、图层栏让到右缘。组模式下以**左列的激活成员**为准
   * （左列现在可能是调整面板）。
   */
  prefs.adjust_dock_left = left_active == comp_dock::PanelId::Adjust;

  /*
   * 分组（tab strip）：整份布局只有一个组（本仓只有两块面板）。
   * 组在哪一侧、组内第几块激活，由 dock 状态给出。
   */
  if (grouped) {
    const bool group_on_left = left_count > 1;
    prefs.group_enabled = true;
    prefs.group_on_left = group_on_left;
    prefs.group_count = group_on_left ? left_count : right_count;
    prefs.group_active = group_on_left ? group_active_index(DockArea::Left)
                                       : group_active_index(DockArea::Right);
  }
}

/* ---------------------------------------------------------------- 分组 */

int DockState::group_count(DockArea side) const {
  int count = 0;
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    const DockArea area = panels[i].area;
    if (area != side) continue;
    ++count;
  }
  return count;
}

PanelId DockState::group_member(DockArea side, int index) const {
  int seen = 0;
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    if (panels[i].area != side) continue;
    if (seen == index) return static_cast<PanelId>(i);
    ++seen;
  }
  return PanelId::Count;
}

PanelId DockState::group_active_panel(DockArea side) const {
  const int slot = side == DockArea::Left ? 1 : 0;
  const PanelId stored = group_active_[slot];
  /* 存下来的激活成员必须**还在这一列**，否则回落到该列的第一块。 */
  if (stored != PanelId::Count && panels[index_of(stored)].area == side) return stored;
  const PanelId first = group_member(side, 0);
  return first == PanelId::Count ? stored : first;
}

int DockState::group_active_index(DockArea side) const {
  const PanelId active = group_active_panel(side);
  if (active == PanelId::Count) return 0;
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    if (panels[i].area != side) continue;
    if (static_cast<PanelId>(i) == active) return i;
  }
  return 0;
}

int DockState::group_width(DockArea side) const {
  const int slot = side == DockArea::Left ? 1 : 0;
  return group_width_[slot];
}

void DockState::set_group_width(DockArea side, int width) {
  const int slot = side == DockArea::Left ? 1 : 0;
  group_width_[slot] = width > 0 ? width : 0;
}

void DockState::group_move(PanelId id, DockArea side) {
  if (side != DockArea::Left && side != DockArea::Right) return;
  const int index = index_of(id);
  if (panels[index].area == side) return;
  /*
   * 直接改 area：**同一列即同一组**（单一真源）。
   * 若该列已有另一块面板，两块现在同列 = 合成一组。
   *
   * **不动 last_docked_**：那里留着的正是「合并前这块面板在哪一列」，
   * 拆组时据此复位（见 group_ungroup）。改了它拆组就回不到原状。
   */
  PanelState& target = panels[index];
  const bool was_floating = target.area == DockArea::Floating ||
                            target.area == DockArea::Hidden;
  target.area = side;
  if (was_floating) {
    /* 原来是浮动/隐藏的：现在停靠到该列，浮窗要跟着关掉（壳侧负责）。 */
    target.collapsed = false;
  }
  /*
   * 新列若已有面板，激活成员保持不变（用户看到的是「多了一个标签」，
   * 不是「当前显示的东西被换掉了」）。新列原先为空才把 id 设为激活。
   */
  const int slot = side == DockArea::Left ? 1 : 0;
  if (group_member(side, 0) == id && group_count(side) == 1) {
    group_active_[slot] = id;
  }
}

void DockState::group_ungroup(PanelId id) {
  const int index = index_of(id);
  const DockArea current = panels[index].area;
  if (current != DockArea::Left && current != DockArea::Right) return;
  /*
   * 复位到**合并前的归属**：每块面板回到 last_docked_ 记录的列。
   * last_docked_ 在 group_move 合并时没有被改动，所以它记的正是合并前
   * 的位置——用户拆组后期望看到的正是那个布局。
   */
  const DockArea restore = last_docked_[index];
  const DockArea restore_other =
      last_docked_[1 - index] == DockArea::Left ? DockArea::Left : DockArea::Right;
  panels[index].area = restore;
  panels[1 - index].area = restore_other;
  last_docked_[index] = restore;
  last_docked_[1 - index] = restore_other;
  /* 激活成员跟着搬家，别让某一列的激活成员停在已经离开的面板上。 */
  for (int slot = 0; slot < 2; ++slot) {
    const DockArea side = slot == 0 ? DockArea::Right : DockArea::Left;
    const PanelId active = group_active_[slot];
    const bool still_here = panels[index_of(active)].area == side;
    if (still_here) continue;
    const PanelId first = group_member(side, 0);
    group_active_[slot] = first == PanelId::Count ? active : first;
  }
}

void DockState::group_set_active(DockArea side, int index) {
  const PanelId member = group_member(side, index);
  if (member == PanelId::Count) return;
  const int slot = side == DockArea::Left ? 1 : 0;
  group_active_[slot] = member;
}

bool DockState::save(const std::wstring& path) const {
  if (path.empty()) return false;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;
  std::fprintf(file, "# compositor shell workspace (dock state)\n");
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    const PanelState& state = panels[i];
    const PanelId id = static_cast<PanelId>(i);
    const char* area = "right";
    switch (state.area) {
      case DockArea::Right: area = "right"; break;
      case DockArea::Left: area = "left"; break;
      case DockArea::Floating: area = "floating"; break;
      case DockArea::Hidden: area = "hidden"; break;
    }
    std::fprintf(file, "%s.area=%s\n", panel_key(id), area);
    std::fprintf(file, "%s.width=%d\n", panel_key(id), state.width);
    std::fprintf(file, "%s.collapsed=%d\n", panel_key(id), state.collapsed ? 1 : 0);
    std::fprintf(file, "%s.float=%d,%d,%d,%d\n", panel_key(id), state.float_x, state.float_y,
                 state.float_w, state.float_h);
  }
  /*
   * 每列的激活成员与列宽。
   * 分组关系不另存——它由 panels[].area 推导（同一列即同一组），存一份
   * 成员列表反而会与 area 分叉。
   */
  for (int slot = 0; slot < 2; ++slot) {
    const DockArea side = slot == 0 ? DockArea::Right : DockArea::Left;
    std::fprintf(file, "%s.active=%d\n", side == DockArea::Left ? "left" : "right",
                 static_cast<int>(index_of(group_active_[slot])));
    std::fprintf(file, "%s.colwidth=%d\n", side == DockArea::Left ? "left" : "right",
                 group_width_[slot]);
  }
  std::fclose(file);
  return true;
}

bool DockState::load(const std::wstring& path) {
  if (path.empty()) return false;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
  DockState next = defaults();
  bool any = false;
  char line[512];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    const std::string text = trim(line);
    if (text.empty() || text[0] == '#') continue;
    const size_t equals = text.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = trim(text.substr(0, equals));
    const std::string value = trim(text.substr(equals + 1));
    for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
      const PanelId id = static_cast<PanelId>(i);
      const std::string prefix = std::string(panel_key(id)) + ".";
      if (key.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string field = key.substr(prefix.size());
      PanelState& state = next.panels[i];
      any = true;
      if (field == "area") {
        if (value == "right") state.area = DockArea::Right;
        else if (value == "left") state.area = DockArea::Left;
        else if (value == "floating") state.area = DockArea::Floating;
        else if (value == "hidden") state.area = DockArea::Hidden;
        else state.area = DockArea::Right; /* 认不出的值回缺省，不猜 */
      } else if (field == "width") {
        state.width = std::atoi(value.c_str());
        if (state.width < 0) state.width = 0;
      } else if (field == "collapsed") {
        state.collapsed = value != "0";
      } else if (field == "float") {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        if (parse_rect(value, &x, &y, &w, &h)) {
          state.float_x = x;
          state.float_y = y;
          state.float_w = w;
          state.float_h = h;
        }
      } else if (field == "group") {
        /* 旧版本写过的分组键：现在分组由 area 推导，这个键不再需要，忽略即可。 */
      }
      break;
    }
    /* 每列的激活成员与列宽：`left.active=N` / `right.active=N`。 */
    if (key == "left.active" || key == "right.active") {
      const int slot = key[0] == 'l' ? 1 : 0;
      const int value_int = std::atoi(value.c_str());
      if (value_int >= 0 && value_int < static_cast<int>(PanelId::Count)) {
        next.group_active_[slot] = static_cast<PanelId>(value_int);
      }
      any = true;
      continue;
    }
    if (key == "left.colwidth" || key == "right.colwidth") {
      const int slot = key[0] == 'l' ? 1 : 0;
      const int width = std::atoi(value.c_str());
      next.group_width_[slot] = width > 0 ? width : 0;
      any = true;
      continue;
    }
  }
  std::fclose(file);
  if (!any) return false;
  /*
   * 读进来的状态要过一遍不变式：浮动的面板不带收起态。
   * （此处此前还有一条「至多一块浮动」的裁剪——2026-10-26 起放开为「可同时浮动
   * 多块」，壳侧按窗口分派命中，不需要第二套通路。）
   */
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    if (next.panels[i].area == DockArea::Floating) {
      next.panels[i].collapsed = false;
    }
  }
  /*
   * 分组不变式：激活的成员必须**还在这一列**。文件被手改过时可能出现
   * 「右列的激活成员是左列的面板」——那时标签栏会显示一个不属于本列的标签。
   * 认不出来就回落到该列的第一个成员。
   */
  for (int slot = 0; slot < 2; ++slot) {
    const DockArea side = slot == 0 ? DockArea::Right : DockArea::Left;
    const PanelId active = next.group_active_[slot];
    bool belongs = false;
    for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
      if (next.panels[i].area == side && static_cast<PanelId>(i) == active) belongs = true;
    }
    if (!belongs) {
      const PanelId first = next.group_member(side, 0);
      next.group_active_[slot] = first == PanelId::Count ? active : first;
    }
  }
  panels[0] = next.panels[0];
  panels[1] = next.panels[1];
  group_active_[0] = next.group_active_[0];
  group_active_[1] = next.group_active_[1];
  group_width_[0] = next.group_width_[0];
  group_width_[1] = next.group_width_[1];
  return true;
}

}  // namespace comp_dock
