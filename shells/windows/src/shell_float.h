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
/* 当前是否有浮动窗口。 */
bool float_panel_is_open(const ShellState& shell);
/*
 * workspace 存读（%APPDATA%\Compositor\workspace.ini）：停靠状态、面板宽度、
 * 收起态、浮动窗口几何。读失败 = 用缺省布局，不影响启动。
 */
void workspace_load(ShellState& shell);
void workspace_save(const ShellState& shell);

}  // namespace comp_shell

#endif  // COMPOSITOR_SHELL_WINDOWS_FLOAT_H
