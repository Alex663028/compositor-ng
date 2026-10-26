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
 *   - area == Floating 时 collapsed 视为 false：浮起来的窗口没有「竖条」形态。
 *   - **两块面板可以同时浮动**（2026-10-26 起）。此前是「至多一块」，理由是
 *     「两套浮动窗口就要两套命中与焦点通路」——但壳侧其实一直是**按窗口**
 *     分派的（浮动窗口过程从 GWLP_USERDATA 取自己的 PanelId，命中层是
 *     无状态函数），第二块浮动并不需要第二套通路。放开后每个面板各自
 *     有独立浮窗，互不影响。
 */
struct DockState {
  /*
   * 两块面板的状态。
   *
   * 缺省必须**显式给全**：PanelState::area 的成员缺省值是 Right（对单块面板
   * 而言的自然缺省），两块都用它就变成「都在右列」。
   * 旧语义下这只是「都停右」的错位状态，被 set_area 的换位逻辑掩盖着；
   * 分组语义下它直接变成「两块同列 = 已分组」——分组状态凭空出现。
   *
   * 所以缺省写死：Adjust 在右、Layers 在左（与 defaults() 一致）。
   * defaults() 与这里的成员初始值必须一致，两处都写全是为了让
   * 「默认构造的 DockState」与「defaults()」完全等价。
   */
  PanelState panels[static_cast<int>(PanelId::Count)] = {
      PanelState{DockArea::Right, 0, false, 0, 0, 0, 0},
      PanelState{DockArea::Left, 0, false, 0, 0, 0, 0},
  };

  /* ------------------------------------------------------------- 查询 */
  DockArea area_of(PanelId id) const;
  bool is_floating(PanelId id) const;
  bool is_hidden(PanelId id) const;
  /* 停靠宽度：用户拖出来的值，0 表示用缺省（由调用方给）。 */
  int width_of(PanelId id, int fallback) const;
  PanelState& panel(PanelId id);
  const PanelState& panel(PanelId id) const;
  /* 当前有几块面板浮在外面（0..2）。 */
  int floating_count() const;

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

  /* ------------------------------------------------ 分组（tab strip） */
  /*
   * 面板分组：两块面板合并到**同一列**，列顶出现标签栏切换显示哪一块。
   * 与 photocraft 的 craft-ui 同一结构（group + tab strip）。
   *
   * **单一真源**：分组关系不另存一份「成员列表」，而是直接由
   * `panels[].area` 推导——两块面板的 area 相同（都是 Left 或都是 Right）
   * 即为同一组。
   *
   * 为什么不用「组对象 + 成员列表」：那份列表表达的是与 area **同一件事**
   * （谁和谁住一列），两份真源必然分叉——改了 area 忘了改列表，就会出现
   * 「标签栏说有 3 个标签，实际只有 2 块面板」。
   *
   * 「这一列当前显示组内第几块」是另一件事，必须单独存（它不是布局的推论，
   * 是用户的选择）。组内顺序固定按 PanelId 升序（Adjust=0, Layers=1），
   * 因此这个下标在壳与布局之间含义一致，不需要另传成员表。
   */
  /* 某列里有几块面板（0 = 该列空，1 = 未分组，2 = 已分组）。 */
  int group_count(DockArea side) const;
  /* 某列里第 index 块面板是谁（按 PanelId 升序；越界返回 Count）。 */
  PanelId group_member(DockArea side, int index) const;
  /* 某列当前激活的是哪块面板。 */
  PanelId group_active_panel(DockArea side) const;
  /* 某列当前激活成员在组内的下标（0 基）。 */
  int group_active_index(DockArea side) const;
  /* 切换某列的激活面板（按成员下标）。 */
  void group_set_active(DockArea side, int index);
  /*
   * 把 id 移到 side 列。**若该列已有面板，两块合成一组**（这是分组的入口）。
   * id 此刻若是浮动态，会同时停靠到该列。
   */
  void group_move(PanelId id, DockArea side);
  /*
   * 合并成一组：把 id 挪到另一块面板所在的列，并把该列的宽度定下来。
   *
   * **组宽的初值 = 目标列合并前的实际宽度**（那一列当时停着谁，就用谁的
   * 缺省宽）。必须在移动**之前**取——移动之后两块同列，「激活成员的宽度」
   * 就变成按激活标签变化的东西了（那正是要消除的依赖）。
   *
   * 不设初值的后果：组宽为 0 → 回落成「激活成员的缺省宽」→ 激活的是调整
   * 时 330、切到图层变 248，**切标签时列宽跳一次**（2026-10-26 视觉探针
   * 抓到的真 bug）。
   */
  void group_merge(PanelId id);
  /*
   * 把 id 从当前列摘出来：**两块回到合并前的归属**（各占一列）。
   *
   * 「合并前的归属」从 last_docked_ 取——group_move 合并时**不动**它，
   * 那里留着的正是合并前各面板所在的列。拆组据此复位：
   * 用户合并前是「图层左 / 调整右」，拆开后还是「图层左 / 调整右」，
   * 而不是变成「图层右 / 调整左」——后者会让用户以为面板跑错了地方。
   */
  void group_ungroup(PanelId id);
  /*
   * 某列的宽度（0 = 用缺省）。
   *
   * 组有自己的宽度，不随激活标签变化——PS 里切标签栏时组宽度不变，
   * 变的是组里的内容。如果宽度跟着激活成员走，切一下标签画布宽度就跳一次，
   * 正在看的图会被挤一下。所以宽度存在**列**上。
   */
  int group_width(DockArea side) const;
  void set_group_width(DockArea side, int width);
  /*
   * 某列的收起态（组模式下整列收起）。
   *
   * 为什么单独存：组的收起是**列**的属性，不是某块面板的属性——
   * 组里两块面板都住在这一列，收起的是列。若用「激活成员的收起标志」
   * 来表达，切标签时列会忽收忽展。
   */
  bool group_collapsed(DockArea side) const;
  void toggle_group_collapsed(DockArea side);

 private:
  /* 浮动前记住的停靠区：停回来时回到原位，而不是硬塞到右边。 */
  DockArea last_docked_[static_cast<int>(PanelId::Count)] = {DockArea::Right, DockArea::Left};
  /* 每列当前激活的成员（PanelId）。索引 0 = Right 列，1 = Left 列。 */
  PanelId group_active_[2] = {PanelId::Adjust, PanelId::Layers};
  /* 每列的宽度（0 = 用缺省）。宽度属于**列**，不随标签切换而变。 */
  int group_width_[2] = {0, 0};
  /* 每列的收起态（组模式下整列收起）。 */
  bool group_collapsed_[2] = {false, false};
};

/* ------------------------------------------------------------------ 拖拽停靠 */

/*
 * 拖放落点判定（纯函数，可脱离 GUI 单测）。
 *
 * PS 的手感是「把面板标题拖出画布 → 浮起来成独立窗口；把它拖回主窗口边缘 →
 * 停回那一侧」。所以判定要回答两个问题：
 *   1. 指针当前落在**哪条热区**上（左边缘 / 右边缘 / 画布中段 / 窗口外）；
 *   2. 这次松手应该**停到哪一侧**（左 / 右 / 保持浮动）。
 *
 * 热区宽度取 48 px（设备像素）：太窄很难瞄准，太宽会让「拖到画布中间松手」
 * 意外被解释成停靠。用户拖到画布正中间松手 = 想让它浮着（PS 同款）。
 */
enum class DropZone {
  None = 0,   /* 不在任何热区（窗口外） */
  Left = 1,   /* 主窗口左边缘 */
  Right = 2,  /* 主窗口右边缘 */
  Body = 3,   /* 主窗口内部但不靠边：松手 = 保持浮动 */
};

struct DropGeometry {
  /* 主窗口客户区（设备像素，屏幕坐标无关的窗口内坐标）。 */
  int client_x = 0;
  int client_y = 0;
  int client_w = 0;
  int client_h = 0;
  /* 边缘热区宽度（设备像素）。 */
  int edge_width = 48;
};

/* 指针在主窗口内的落点（设备像素，客户区坐标）→ 热区。 */
inline DropZone drop_zone_at(const DropGeometry& geo, int x, int y) {
  if (geo.client_w <= 0 || geo.client_h <= 0) return DropZone::None;
  if (x < 0 || y < 0 || x >= geo.client_w || y >= geo.client_h) return DropZone::None;
  const int edge = geo.edge_width > 0 ? geo.edge_width : 48;
  if (x < edge) return DropZone::Left;
  if (x >= geo.client_w - edge) return DropZone::Right;
  return DropZone::Body;
}

/* 热区 → 停靠侧。Body / None 一律「保持浮动」。 */
inline bool drop_zone_docks_left(DropZone zone) { return zone == DropZone::Left; }

}  // namespace comp_dock

#endif  // COMPOSITOR_SHELL_WINDOWS_DOCK_H
