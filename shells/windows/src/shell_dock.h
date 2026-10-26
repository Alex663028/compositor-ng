// AI生成
/*
 * shell_dock.h — 停靠子系统：面板的**状态**、**命令**、**布局求解**与 **workspace 存读**。
 *
 * 为什么要独立成子系统（而不是继续在壳里加字段）：
 *   1. 面板现在有三种形态——停靠（左右两侧）、收起、**浮动（独立顶层窗口）**。
 *      三种形态互相影响（浮动的那块不在主窗口占位、收起态是 24px 竖条、
 *      停靠态有分界线可拖宽），这些规则散在壳里必然互相打架。
 *   2. 「dock 状态 / 修改状态的命令 / 由状态生成默认布局 / workspace 存读」是
 *      四件独立的事，把它们收在一个纯数据模块里，才能既被主窗口用、也能被
 *      浮动窗口用、还能被单元测试直接测。
 *   3. 这个模块**不依赖 windows.h**：它只产出数字与矩形，窗口怎么建是壳的事。
 *      因此「dock 状态机」可以脱离 GUI 单测。
 *
 * 与 ui_core 的分工：ui_core::compute_layout 负责「给定偏好怎么摆」（含窄窗时
 * 放不下就整块收起的规则），本模块负责「偏好是什么」与「怎么改」。
 */
#ifndef COMPOSITOR_SHELL_WINDOWS_DOCK_H
#define COMPOSITOR_SHELL_WINDOWS_DOCK_H

#include <cstdint>
#include <string>

#include "ui_core.h"

namespace comp_dock {

/* 面板种类。目前两块：右栏 = 调整，左栏 = 图层。 */
enum class PanelId { Adjust = 0, Layers = 1, Count = 2 };

/*
 * 停靠区。
 *   Right / Left：停靠在主窗口的一侧（Left = 停左，列序与 Right 相反）。
 *   Floating：独立顶层窗口（不在主窗口占位）。
 *   Hidden：整块隐藏（比「收起」更彻底：连 24px 竖条都不留）。
 */
enum class DockArea { Right = 0, Left = 1, Floating = 2, Hidden = 3 };

struct PanelState {
  DockArea area = DockArea::Right;
  /* 停靠时的列宽（0 = 用缺省宽度；夹取在 ui_core 那一侧做）。 */
  int width = 0;
  /* 收起态：整块变成 24px 竖条，点一下展开（沿用既有 §5.43 语义）。 */
  bool collapsed = false;
  /* 浮动窗口几何（**屏幕**坐标；width/height 为 0 = 尚未决定，用缺省）。 */
  int float_x = 0;
  int float_y = 0;
  int float_w = 0;
  int float_h = 0;
};

/*
 * 停靠状态。两块面板各一份状态；命令改状态，布局由状态推导。
 *
 * 不变式：
 *   - 同一时刻**至多一块**处于 Floating —— 两个浮动窗口就意味着两套命中与
 *     焦点通路，本版只做一套（见 shell_float.cpp）。
 *   - area == Floating 时 collapsed 视为 false：浮起来的窗口没有「竖条」形态。
 */
struct DockState {
  PanelState panels[static_cast<int>(PanelId::Count)];

  /* ------------------------------------------------------------- 查询 */
  DockArea area_of(PanelId id) const;
  bool is_floating(PanelId id) const;
  bool is_hidden(PanelId id) const;
  /* 停靠宽度：用户拖出来的值，0 表示用缺省（由调用方给）。 */
  int width_of(PanelId id, int fallback) const;
  PanelState& panel(PanelId id);
  const PanelState& panel(PanelId id) const;

  /* ------------------------------------------------------------- 命令 */
  /* 换停靠区。若目标区已有另一块面板，两块对调位置（与 PS 的「面板拖到另一侧」
   * 同一效果：那里本来就是另一块的位置）。 */
  void set_area(PanelId id, DockArea area);
  /* 设停靠宽度（0 = 回到缺省）。 */
  void set_width(PanelId id, int width);
  /* 收起 / 展开。 */
  void toggle_collapsed(PanelId id);
  /* 浮起来：记下屏幕几何，并从原停靠区让位。 */
  void float_panel(PanelId id, int x, int y, int width, int height);
  /* 停回原位（float → 之前的停靠区由 last_docked_area 记着）。 */
  void dock_panel(PanelId id);

  /* ------------------------------------------------- 布局求解（唯一出口） */
  /*
   * 把状态翻译成 ui_core::LayoutPrefs。
   *
   * 浮动的那块：docked = false（主窗口完全不给它留位），宽度也不参与。
   * 隐藏的那块：同样 docked = false，但区别在于浮动有自己的窗口、隐藏没有——
   * 两者在主窗口里表现一致（都不占位），差别在壳要不要给它建窗口。
   */
  void apply_to(comp_ui::LayoutPrefs& prefs) const;

  /* ------------------------------------------------- workspace 存读 */
  /*
   * 纯文本 ini（`键=值` 一行一个）。不引 JSON 依赖：这一层的数据只有十来个字段，
   * 引个解析器进来反而更容易写坏。存读失败一律返回 false，调用方按「用缺省」
   * 处理——工作区读不出来不该让程序起不来。
   */
  bool save(const std::wstring& path) const;
  bool load(const std::wstring& path);

  /* 缺省布局：调整在右、图层在左，都展开。 */
  static DockState defaults();

 private:
  /* 浮动前记住的停靠区：停回来时回到原位，而不是硬塞到右边。 */
  DockArea last_docked_[static_cast<int>(PanelId::Count)] = {DockArea::Right, DockArea::Left};
};

}  // namespace comp_dock

#endif  // COMPOSITOR_SHELL_WINDOWS_DOCK_H
