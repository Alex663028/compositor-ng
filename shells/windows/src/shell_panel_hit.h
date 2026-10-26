// AI生成
/*
 * shell_panel_hit.h — 面板命中与交互层（主窗口与浮动面板窗口共用）。
 *
 * 四个函数都返回「这次点击/移动是否被我吃掉」：true = 已处理（调用方不要再往下
 * 派发），false = 与面板无关（主窗口继续走画布分支）。
 */
#ifndef COMPOSITOR_SHELL_WINDOWS_PANEL_HIT_H
#define COMPOSITOR_SHELL_WINDOWS_PANEL_HIT_H

#include <windows.h>

#include "ui_core.h"

namespace comp_shell {

struct ShellState;

/* 图层面板命中（直方图框选起点 + 图层行点击）。 */
bool layer_panel_click(ShellState& shell, const comp_ui::Layout& layout, int mouse_x, int mouse_y,
                        HWND window);
/* 右栏命中（各面板的行与滑杆）。 */
bool adjust_panel_click(ShellState& shell, const comp_ui::Layout& layout, int mouse_x, int mouse_y,
                        HWND window);
/* 面板拖动推进（按住不放时的连续更新）。 */
bool panel_drag_move(ShellState& shell, const comp_ui::Layout& layout, int mouse_x, int mouse_y,
                      HWND window);
/* 面板拖动收尾（松手：提交事务 / 落子 / 释放捕获）。 */
bool panel_drag_end(ShellState& shell, const comp_ui::Layout& layout, int mouse_x, int mouse_y,
                    HWND window);

}  // namespace comp_shell

#endif  // COMPOSITOR_SHELL_WINDOWS_PANEL_HIT_H
