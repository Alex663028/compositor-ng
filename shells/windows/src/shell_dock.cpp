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
   * 目标区已有另一块面板：两块对调（PS 里把面板拖到另一侧就是这个效果）。
   * 这里只换**区域**，宽度与收起态各归各位——换边不是换面板。
   */
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    PanelState& other = panels[i];
    if (i != index_of(id) && other.area == area) {
      other.area = (area == DockArea::Left) ? DockArea::Right : DockArea::Left;
    }
  }
  target.area = area;
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

  prefs.adjust_docked = !adjust_floating && !adjust_hidden;
  prefs.layer_docked = !layers_floating && !layers_hidden;

  prefs.adjust_panel_width =
      adjust.width > 0 ? adjust.width : static_cast<int>(comp_ui::Layout::kAdjustPanelWidth);
  prefs.layer_panel_width =
      layers.width > 0 ? layers.width : static_cast<int>(comp_ui::Layout::kLayerPanelWidth);
  prefs.adjust_panel_collapsed = adjust.collapsed;
  prefs.layer_panel_collapsed = layers.collapsed;
  /* 停左：调整面板在左、图层栏让到右缘。两块都在同一侧不可能（set_area 会换位），
   * 但从文件读进来的状态未必合法——这时以调整面板为准。 */
  prefs.adjust_dock_left = adjust.area == DockArea::Left;
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
      }
      break;
    }
  }
  std::fclose(file);
  if (!any) return false;
  /* 读进来的状态要过一遍不变式：至多一块浮动、浮动的不带收起态。 */
  int floating = 0;
  for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
    if (next.panels[i].area == DockArea::Floating) {
      ++floating;
      next.panels[i].collapsed = false;
    }
  }
  if (floating > 1) {
    for (int i = 0; i < static_cast<int>(PanelId::Count); ++i) {
      if (next.panels[i].area == DockArea::Floating && --floating > 0) {
        next.panels[i].area = i == index_of(PanelId::Adjust) ? DockArea::Right : DockArea::Left;
      }
    }
  }
  panels[0] = next.panels[0];
  panels[1] = next.panels[1];
  return true;
}

}  // namespace comp_dock
