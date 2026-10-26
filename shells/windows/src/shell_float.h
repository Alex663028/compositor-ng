// AI生成
/*
 * shell_float.h — 浮动面板窗口。
 *
 * 停靠子系统的「Floating」形态在这里落地：把一块面板从主窗口里摘出来，
 * 变成一个**独立的顶层窗口**（可拖动、可缩放、可单独关闭）。
 *
 * 之所以能复用主窗口的交互：面板的绘制与命中都按「面板矩形」走
 * （draw_adjust_content / draw_layer_content + shell_panel_hit），浮动窗口把
 * 自己的客户区当作那块面板矩形即可。
 *
 * 2026-10-26 起支持**多块面板同时浮动**（此前是「至多一块」）：每块面板一个
 * 独立浮窗，句柄按 PanelId 索引存在 ShellState::float_windows。
 */
#ifndef COMPOSITOR_SHELL_WINDOWS_FLOAT_H
#define COMPOSITOR_SHELL_WINDOWS_FLOAT_H

#include <windows.h>

#include "shell_dock.h"

namespace comp_shell {

struct ShellState;

/* 浮起来：把 id 这块面板摘成独立窗口。已在浮动中则前置。 */
bool float_panel_open(ShellState& shell, HWND owner, comp_dock::PanelId id);
/* 停回去：关掉浮动窗口，面板回到原来的停靠区。 */
void float_panel_close(ShellState& shell, comp_dock::PanelId id);
/* 位置/尺寸写回 dock 状态（窗口被拖动或缩放后调用）。 */
void float_panel_sync(ShellState& shell);
/* 某块面板当前是否有浮动窗口。 */
bool float_panel_is_open(const ShellState& shell, comp_dock::PanelId id);
/* 任意一块面板是否有浮动窗口。 */
bool float_panel_any_open(const ShellState& shell);
/*
 * 拖出：从主窗口里把一块面板**拖**成浮动窗口（按住标题行拖出画布）。
 * 与菜单 / 双击的差别只在于「怎么触发」，停靠状态机完全一致。
 * 指针在主窗口边缘热区松手 → 停回那一侧；否则浮着。
 */
void float_panel_begin_tear(ShellState& shell, comp_dock::PanelId id, int grab_dx, int grab_dy);
/* 拖出过程中的推进：指针位置（客户区坐标）。返回 true = 仍在拖。 */
bool float_panel_tear_move(ShellState& shell, int mouse_x, int mouse_y);
/* 拖出收尾：松手。落在边缘热区就停回，否则保持浮动。 */
void float_panel_tear_end(ShellState& shell);
/* 当前是否正在拖出。 */
bool float_panel_is_tearing(const ShellState& shell);
/* ------------------------------------------------ 分组（tab strip） */
/*
 * 合并成一组：两块面板住进同一列，列顶出现标签栏切换。
 * 拆开：回到「各占一列」。
 */
void panel_group_merge(ShellState& shell);
void panel_group_split(ShellState& shell);
/* 点标签切换激活成员。side = 标签栏所在列，index = 组内下标。 */
void panel_group_select(ShellState& shell, comp_dock::DockArea side, int index);
/* 当前是否处于分组状态（两列里有一列叠了两块面板）。 */
bool panel_group_active(const ShellState& shell);
/* 组所在的列（未分组时返回 Right，调用方应先判 panel_group_active）。 */
comp_dock::DockArea panel_group_side(const ShellState& shell);
/*
 * workspace 存读（%APPDATA%\Compositor\workspace.ini）：停靠状态、面板宽度、
 * 收起态、浮动窗口几何。读失败 = 用缺省布局，不影响启动。
 */
void workspace_load(ShellState& shell);
void workspace_save(const ShellState& shell);

}  // namespace comp_shell

#endif  // COMPOSITOR_SHELL_WINDOWS_FLOAT_H
