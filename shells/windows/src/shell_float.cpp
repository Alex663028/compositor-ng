// AI生成
/*
 * shell_float.cpp — 浮动面板窗口的实现。
 *
 * 四件事：
 *   1. 建/拆独立顶层窗口（WS_OVERLAPPEDWINDOW：标题栏 + 可缩放边框）；
 *   2. 绘制：把面板内容画进自己的客户区（与停靠时同一份代码）；
 *   3. 交互：命中、拖动推进、拖动收尾都走 shell_panel_hit（与主窗口同一份）；
 *   4. 拖出 / 拖回：把面板拖出主窗口成浮窗，拖回边缘热区则停回。
 *
 * 坐标口径：客户区坐标先按壳的 DPI 折算成**逻辑像素**，再交给共享命中层；
 * 面板矩形 = 客户区（逻辑像素）。行几何全部由这块矩形推出，因此浮起来的
 * 面板与停靠时的行高、行位完全一致。
 */
#include "shell_float.h"

#include <cstring>
#include <windowsx.h>

#include "shell_internal.h"
#include "shell_panel_hit.h"

namespace comp_shell {
namespace {

constexpr wchar_t kFloatClassName[] = L"CompositorFloatPanel";
constexpr int kFloatDefaultWidth = 330;
constexpr int kFloatDefaultHeight = 560;
/* 拖出阈值（设备像素）：拖动小于这个距离仍算「点击」，不浮出新窗口。 */
constexpr int kTearThreshold = 6;

/* 浮动窗口当前承载哪块面板：存在窗口的 USERDATA 里。 */
comp_dock::PanelId panel_of(HWND window) {
  const LRESULT value = GetWindowLongPtrW(window, GWLP_USERDATA);
  return static_cast<comp_dock::PanelId>(value);
}

void set_panel_of(HWND window, comp_dock::PanelId id) {
  SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(id));
}

/* 该面板的浮窗句柄（可能为 nullptr）。 */
HWND float_window_of(const ShellState& shell, comp_dock::PanelId id) {
  const int index = static_cast<int>(id);
  if (index < 0 || index >= static_cast<int>(comp_dock::PanelId::Count)) return nullptr;
  return shell.float_windows[index];
}

void set_float_window(ShellState& shell, comp_dock::PanelId id, HWND window) {
  const int index = static_cast<int>(id);
  if (index < 0 || index >= static_cast<int>(comp_dock::PanelId::Count)) return;
  shell.float_windows[index] = window;
}

/*
 * 浮动窗口的「面板矩形」= 客户区（逻辑像素）。
 * 其余矩形一律空：浮动窗口里没有画布、没有工具条、没有状态栏。
 */
comp_ui::Layout float_layout(const ShellState& shell, HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  const int dpi = shell_dpi(shell);
  comp_ui::Layout layout{};
  const int width = dpi_logical(client.right - client.left, dpi);
  const int height = dpi_logical(client.bottom - client.top, dpi);
  layout.adjust_panel = comp_ui::Rect{0, 0, width, height};
  layout.layer_panel = comp_ui::Rect{0, 0, width, height};
  return layout;
}

/*
 * 浮动窗口里的滚轮：转给主窗口的滚动通路。
 *
 * 滚动语义完全由「当前开着哪个面板」决定（历史/字体/通道/填充/智能滤镜/调整
 * 参数表各有各的滚动量），这些状态都挂在 ShellState 上，所以把滚轮按
 * **屏幕坐标**转给主窗口最省事：主窗口拿到坐标后自己判断落在哪块面板上。
 *
 * 不能直接转发 WM_MOUSEWHEEL 消息：那个消息的 lParam 是屏幕坐标，
 * 转发的目标窗口会用**自己的**客户区解释它——主窗口会以为是主窗口内的点。
 * 所以这里自己算好「相对主窗口客户区的坐标」，再合成一条新的滚轮消息。
 */
void forward_wheel_to_main(ShellState& shell, HWND window, WPARAM wparam, LPARAM lparam) {
  if (shell.window == nullptr || shell.window == window) return;
  POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ScreenToClient(shell.window, &cursor);
  const LRESULT packed = MAKELPARAM(static_cast<short>(cursor.x), static_cast<short>(cursor.y));
  PostMessageW(shell.window, WM_MOUSEWHEEL, wparam, packed);
}

LRESULT CALLBACK float_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  ShellState* shell = g_shell;
  switch (message) {
    case WM_PAINT: {
      if (shell == nullptr) break;
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      const int device_width = client.right - client.left;
      const int device_height = client.bottom - client.top;
      if (device_width > 0 && device_height > 0) {
        HDC memory_dc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, device_width, device_height);
        HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
        /*
         * 与主窗口同一套映射：逻辑坐标 -> 设备像素（MM_ANISOTROPIC）。
         * 排版常量、命中半径因此不必乘系数，浮动面板与停靠面板逐像素一致。
         */
        const int dpi = shell_dpi(*shell);
        const int logical_width = dpi_logical(device_width, dpi);
        const int logical_height = dpi_logical(device_height, dpi);
        HFONT ui_font = nullptr;
        HGDIOBJ old_font = nullptr;
        if (dpi != 96 && logical_width > 0 && logical_height > 0) {
          SetMapMode(memory_dc, MM_ANISOTROPIC);
          SetWindowExtEx(memory_dc, logical_width, logical_height, nullptr);
          SetViewportExtEx(memory_dc, device_width, device_height, nullptr);
          LOGFONTW font{};
          HGDIOBJ stock_font = GetCurrentObject(memory_dc, OBJ_FONT);
          if (stock_font != nullptr &&
              GetObjectW(stock_font, sizeof(font), &font) == static_cast<int>(sizeof(font))) {
            ui_font = CreateFontIndirectW(&font);
            if (ui_font != nullptr) old_font = SelectObject(memory_dc, ui_font);
          }
        }
        const comp_ui::Rect panel{0, 0, logical_width, logical_height};
        /* 面板内容裁进客户区：提示行比面板宽，不裁会溢出窗口。 */
        const HRGN clip = CreateRectRgn(0, 0, logical_width, logical_height);
        SelectClipRgn(memory_dc, clip);
        if (panel_of(window) == comp_dock::PanelId::Layers) {
          /*
           * 这里**不**调 panel_sync_layer_scroll：它会按**本面板**的高度去夹
           * 滚动量，而滚动量是主窗口与浮窗**共用**的一份。浮窗比主窗口矮 →
           * 夹一次就把主窗口的滚动位置改了，用户在主窗口里看到的图层列表
           * 会莫名跳位。
           *
           * 滚动量只在主窗口绘制时同步（那里才有主窗口的面板尺寸）。
           * 浮窗里的滚轮本来就转发给主窗口的滚动通路，改的是同一份滚动量，
           * 两边一致。
           */
          draw_layer_content(memory_dc, *shell, panel);
        } else {
          draw_adjust_content(memory_dc, *shell, panel);
        }
        SelectClipRgn(memory_dc, NULL);
        DeleteObject(clip);

        SetMapMode(memory_dc, MM_TEXT);
        BitBlt(dc, 0, 0, device_width, device_height, memory_dc, 0, 0, SRCCOPY);
        if (old_font != nullptr) SelectObject(memory_dc, old_font);
        if (ui_font != nullptr) DeleteObject(ui_font);
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
      }
      EndPaint(window, &paint);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      if (shell == nullptr) break;
      const int dpi = shell_dpi(*shell);
      const int x = dpi_logical(GET_X_LPARAM(lparam), dpi);
      const int y = dpi_logical(GET_Y_LPARAM(lparam), dpi);
      const comp_ui::Layout layout = float_layout(*shell, window);
      const bool consumed =
          panel_of(window) == comp_dock::PanelId::Layers
              ? layer_panel_click(*shell, layout, x, y, window)
              : adjust_panel_click(*shell, layout, x, y, window);
      if (consumed) {
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE: {
      if (shell == nullptr) break;
      const int dpi = shell_dpi(*shell);
      const int x = dpi_logical(GET_X_LPARAM(lparam), dpi);
      const int y = dpi_logical(GET_Y_LPARAM(lparam), dpi);
      const comp_ui::Layout layout = float_layout(*shell, window);
      if (panel_drag_move(*shell, layout, x, y, window)) {
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    }
    case WM_LBUTTONUP: {
      if (shell == nullptr) break;
      const int dpi = shell_dpi(*shell);
      const int x = dpi_logical(GET_X_LPARAM(lparam), dpi);
      const int y = dpi_logical(GET_Y_LPARAM(lparam), dpi);
      const comp_ui::Layout layout = float_layout(*shell, window);
      if (panel_drag_end(*shell, layout, x, y, window)) {
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    }
    case WM_MOUSEWHEEL:
      /* 滚动语义在主窗口那条通路上，这里把屏幕坐标换算好再转过去。 */
      if (shell != nullptr) {
        forward_wheel_to_main(*shell, window, wparam, lparam);
        return 0;
      }
      break;
    case WM_KEYDOWN:
    case WM_CHAR:
      /*
       * 键盘转发给主窗口：浮动窗口拿到焦点时，工具快捷键、面板键盘操作
       * （Tab 切行、数字键输入）仍然有效——否则浮起来的面板会变成
       * 「只能用鼠标」的次等面板。
       */
      if (shell != nullptr && shell->window != nullptr && shell->window != window) {
        PostMessageW(shell->window, message, wparam, lparam);
        return 0;
      }
      break;
    case WM_SIZE:
    case WM_MOVE:
      /* 位置/尺寸变了就写回 dock 状态（workspace 存读与「停回去再浮出来」要用）。 */
      if (shell != nullptr && g_shell != nullptr) {
        float_panel_sync(*shell);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_CLOSE:
      /* 关掉浮动窗口 = 面板停回原位（不是「隐藏面板」）。 */
      if (shell != nullptr) {
        float_panel_close(*shell, panel_of(window));
        return 0;
      }
      break;
    case WM_DESTROY:
      if (shell != nullptr) {
        const comp_dock::PanelId id = panel_of(window);
        if (float_window_of(*shell, id) == window) {
          set_float_window(*shell, id, nullptr);
        }
      }
      break;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/*
 * 窗口类只注册一次。注册必须在窗口过程可用之后（RegisterClassExW 要拿
 * lpfnWndProc），所以这个函数放在窗口过程下面。
 */
bool class_registered() {
  static bool registered = false;
  if (registered) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = float_window_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kFloatClassName;
  const ATOM atom = RegisterClassExW(&wc);
  registered = atom != 0;
  return registered;
}

/* 屏幕工作区（用于把浮窗夹回可见范围）。 */
RECT work_area() {
  RECT screen{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
  return screen;
}

}  // namespace

bool float_panel_is_open(const ShellState& shell, comp_dock::PanelId id) {
  return float_window_of(shell, id) != nullptr;
}

bool float_panel_any_open(const ShellState& shell) {
  for (int i = 0; i < static_cast<int>(comp_dock::PanelId::Count); ++i) {
    if (shell.float_windows[i] != nullptr) return true;
  }
  return false;
}

/*
 * 浮 / 停回 的切换入口（菜单与双击标题都走这里）。
 *
 * 多块面板可以同时浮动：每块面板一个独立浮窗，互不影响。
 */
void toggle_float_panel(ShellState& shell, comp_dock::PanelId id) {
  if (shell.dock.is_floating(id)) {
    float_panel_close(shell, id);
    set_status(shell, id == comp_dock::PanelId::Layers ? L"图层面板已停回原位"
                                                        : L"调整面板已停回原位");
    return;
  }
  if (!float_panel_open(shell, shell.window, id)) {
    set_status(shell, L"浮动面板创建失败");
    return;
  }
  set_status(shell, id == comp_dock::PanelId::Layers
                         ? L"图层面板已浮出（双击标题栏可停回）"
                         : L"调整面板已浮出（双击标题栏可停回）");
}

bool float_panel_open(ShellState& shell, HWND owner, comp_dock::PanelId id) {
  if (float_window_of(shell, id) != nullptr) {
    /* 已经有浮动窗口：前置它，不再开第二个。 */
    SetForegroundWindow(float_window_of(shell, id));
    return true;
  }
  if (!class_registered()) return false;

  /* 初始几何：贴着主窗口右侧，宽 = 该面板当前停靠宽度，高 = 主窗口的 70%。 */
  RECT owner_rect{};
  GetWindowRect(owner, &owner_rect);
  const int dpi = shell_dpi(shell);
  const int width = shell.dock.width_of(id, kFloatDefaultWidth);
  const int height = kFloatDefaultHeight;
  int x = owner_rect.right + 12;
  int y = owner_rect.top + 40;
  /* 别把窗口开到屏幕外：右缘放不下就挪到主窗口左边。 */
  const RECT screen = work_area();
  if (x + width > screen.right) x = screen.right - width - 8;
  if (x < screen.left) x = screen.left + 8;
  if (y + height > screen.bottom) y = screen.bottom - height - 8;
  if (y < screen.top) y = screen.top + 8;

  const int scale = dpi > 0 ? dpi / 96 : 1;
  HWND window = CreateWindowExW(
      WS_EX_TOOLWINDOW, kFloatClassName,
      id == comp_dock::PanelId::Layers ? L"图层面板" : L"调整面板",
      WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, x, y, width * scale, height * scale, owner,
      nullptr, GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) return false;
  set_panel_of(window, id);
  set_float_window(shell, id, window);
  shell.dock.float_panel(id, x, y, width, height);
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  return true;
}

void float_panel_close(ShellState& shell, comp_dock::PanelId id) {
  HWND window = float_window_of(shell, id);
  if (window != nullptr) {
    set_float_window(shell, id, nullptr);
    /* DestroyWindow 会走 WM_DESTROY；那里再清一次句柄（幂等）。 */
    DestroyWindow(window);
  }
  shell.dock.dock_panel(id);
  /* 主窗口要重画：那块面板的列宽变了（浮走时让位，停回时归位）。 */
  if (shell.window != nullptr) InvalidateRect(shell.window, nullptr, FALSE);
}

/* ------------------------------------------------ 分组（tab strip） */

bool panel_group_active(const ShellState& shell) {
  return shell.dock.group_count(comp_dock::DockArea::Left) > 1 ||
         shell.dock.group_count(comp_dock::DockArea::Right) > 1;
}

comp_dock::DockArea panel_group_side(const ShellState& shell) {
  return shell.dock.group_count(comp_dock::DockArea::Left) > 1 ? comp_dock::DockArea::Left
                                                               : comp_dock::DockArea::Right;
}

void panel_group_merge(ShellState& shell) {
  if (panel_group_active(shell)) {
    set_status(shell, L"已经是分组状态了");
    return;
  }
  /*
   * 合并 = 把图层栏挪到调整栏那一列（两块住进同一列 = 一组）。
   * 选调整栏那列是因为它是缺省下用户视线停留的地方（PS 的属性面板在右）。
   *
   * 组宽的初值由 dock 的 group_merge 负责（取目标列合并前的实际宽度）——
   * 这里不重复算：宽度初值算错的后果是「切标签时列宽跳一次」
   * （2026-10-26 视觉探针抓到的真 bug）。
   */
  shell.dock.group_merge(comp_dock::PanelId::Layers);
  if (shell.window != nullptr) InvalidateRect(shell.window, nullptr, FALSE);
  set_status(shell, L"面板已合并成一组（点列顶标签切换；视图 > 面板分组可拆开）");
}

void panel_group_split(ShellState& shell) {
  if (!panel_group_active(shell)) {
    set_status(shell, L"当前不是分组状态");
    return;
  }
  const comp_dock::DockArea side = panel_group_side(shell);
  /*
   * 拆开 = 把组里**非激活**的那块挪回另一列。
   *
   * 为什么挪非激活的那块：留在原列的是激活成员（用户正在看的那块），
   * 把它挪走会让「当前显示的是谁」在拆组瞬间换掉——用户看到的是
   * 「面板跳到另一边去了」，而不是「组拆开了」。
   */
  const comp_dock::PanelId active = shell.dock.group_active_panel(side);
  const comp_dock::PanelId other = active == comp_dock::PanelId::Adjust
                                       ? comp_dock::PanelId::Layers
                                       : comp_dock::PanelId::Adjust;
  shell.dock.group_ungroup(other);
  if (shell.window != nullptr) InvalidateRect(shell.window, nullptr, FALSE);
  set_status(shell, L"面板组已拆开（各占一列）");
}

void panel_group_select(ShellState& shell, comp_dock::DockArea side, int index) {
  shell.dock.group_set_active(side, index);
  if (shell.window != nullptr) InvalidateRect(shell.window, nullptr, FALSE);
  const comp_dock::PanelId active = shell.dock.group_active_panel(side);
  set_status(shell, active == comp_dock::PanelId::Layers ? L"当前显示：图层面板"
                                                          : L"当前显示：调整面板");
}

/* ---------------------------------------------------------------- 拖出 / 拖回 */

void float_panel_begin_tear(ShellState& shell, comp_dock::PanelId id, int grab_dx, int grab_dy) {
  shell.tear_panel = id;
  shell.tear_grab_x = grab_dx;
  shell.tear_grab_y = grab_dy;
  shell.tear_active = false;
  shell.tear_moved = false;
  shell.tear_last_x = grab_dx;
  shell.tear_last_y = grab_dy;
}

bool float_panel_is_tearing(const ShellState& shell) { return shell.tear_active; }

bool float_panel_tear_move(ShellState& shell, int mouse_x, int mouse_y) {
  if (shell.tear_panel == comp_dock::PanelId::Count) return false;
  const int dx = mouse_x - shell.tear_grab_x;
  const int dy = mouse_y - shell.tear_grab_y;
  if (!shell.tear_active) {
    /* 阈值没过：还只是一次「按住标题行」，不浮出新窗口。 */
    if (dx > -kTearThreshold && dx < kTearThreshold && dy > -kTearThreshold &&
        dy < kTearThreshold) {
      shell.tear_last_x = mouse_x;
      shell.tear_last_y = mouse_y;
      return false;
    }
    shell.tear_active = true;
    shell.tear_moved = true;
    /*
     * 越过阈值的这一刻就把浮窗建出来：浮窗要出现在指针底下，
     * 之后每次移动只按增量搬它（绝对定位会在指针越出客户区时抖）。
     */
    if (!float_panel_open(shell, shell.window, shell.tear_panel)) {
      shell.tear_active = false;
      shell.tear_moved = false;
      return false;
    }
    shell.tear_last_x = mouse_x;
    shell.tear_last_y = mouse_y;
    return true;
  }
  /* 已进入拖出：按增量搬浮窗。 */
  const int move_x = mouse_x - shell.tear_last_x;
  const int move_y = mouse_y - shell.tear_last_y;
  shell.tear_last_x = mouse_x;
  shell.tear_last_y = mouse_y;
  if (move_x == 0 && move_y == 0) return true;
  HWND window = float_window_of(shell, shell.tear_panel);
  if (window != nullptr) {
    RECT rect{};
    GetWindowRect(window, &rect);
    SetWindowPos(window, nullptr, rect.left + move_x, rect.top + move_y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return true;
}

void float_panel_tear_end(ShellState& shell) {
  const comp_dock::PanelId id = shell.tear_panel;
  const bool moved = shell.tear_moved;
  shell.tear_panel = comp_dock::PanelId::Count;
  shell.tear_active = false;
  shell.tear_moved = false;
  if (!moved) return;
  if (float_window_of(shell, id) == nullptr) return;

  /*
   * 松手落点判定：拖到主窗口左右边缘的热区 → 停回那一侧；拖到画布中间或
   * 窗口外 → 保持浮动（PS 的手感：拖出来就是要它浮着）。
   *
   * 判定用的是**浮窗中心**落在主窗口客户区的哪个热区，不是鼠标位置——
   * 拖出时鼠标按在浮窗标题栏上，指针位置与浮窗位置差半个窗口，用户
   * 看着的是浮窗落在哪。
   */
  HWND window = float_window_of(shell, id);
  RECT float_rect{};
  GetWindowRect(window, &float_rect);
  RECT client{};
  GetClientRect(shell.window, &client);
  POINT center{(float_rect.left + float_rect.right) / 2, (float_rect.top + float_rect.bottom) / 2};
  ScreenToClient(shell.window, &center);

  comp_dock::DropGeometry geo{};
  geo.client_w = client.right - client.left;
  geo.client_h = client.bottom - client.top;
  const comp_dock::DropZone zone = comp_dock::drop_zone_at(geo, center.x, center.y);
  /*
   * 落点判定走 dock 层的**纯函数** drop_action_for（壳与自检同一条判定）。
   *
   * 四种落点语义：
   *   - 拖回**自己原来那侧** → 停回原位（不是合并）。
   *   - 拖到**另一侧、那列空着** → 直接停过去。
   *   - 拖到**另一侧、那列有面板** → **合并成一组**（拖放重排）。
   *   - 落在画布中间或窗口外 → 保持浮动（PS 的手感：拖出来就是要它浮着）。
   *
   * 为什么「拖到有面板的那侧」是合并而不是「两块对调」：菜单里的
   * 「面板停靠到右/左侧」是**独占**语义（那块过去、另一块让位），而把
   * 面板**拖到另一块面板所在的位置**在 PS 里是「合成标签组」。两个入口
   * 意图不同，结果就该不同——混成一样的话「停靠左侧」会把用户拖成组。
   */
  const comp_dock::DockArea origin = shell.dock.last_docked_area(id);
  const comp_dock::DockArea target = comp_dock::drop_zone_docks_left(zone)
                                         ? comp_dock::DockArea::Left
                                         : comp_dock::DockArea::Right;
  const bool target_used = shell.dock.group_count(target) > 0 &&
                           shell.dock.group_member(target, 0) != id;
  const comp_dock::DropAction action =
      comp_dock::drop_action_for(zone, origin, target, target_used);

  if (action == comp_dock::DropAction::StayFloating) {
    set_status(shell, L"面板已浮出（拖回窗口边缘可停靠）");
    return;
  }

  /* float_panel_close 会把面板停回浮起前的那一侧（origin）。 */
  float_panel_close(shell, id);

  const bool is_layers = id == comp_dock::PanelId::Layers;
  const wchar_t* message = nullptr;
  switch (action) {
    case comp_dock::DropAction::DockBack:
      message = is_layers ? L"图层面板已停回原位" : L"调整面板已停回原位";
      break;
    case comp_dock::DropAction::MergeInto:
      shell.dock.group_merge_to(id, target);
      message = is_layers ? L"图层面板已与调整面板合并成一组（点列顶标签切换）"
                          : L"调整面板已与图层面板合并成一组（点列顶标签切换）";
      break;
    case comp_dock::DropAction::MoveTo:
      shell.dock.set_area(id, target);
      message = target == comp_dock::DockArea::Left ? L"图层面板已停到左侧"
                                                   : L"调整面板已停到右侧";
      break;
    default:
      break;
  }
  set_status(shell, message);
  if (shell.window != nullptr) InvalidateRect(shell.window, nullptr, FALSE);
}

namespace {

/* workspace 文件位置：%APPDATA%\Compositor\workspace.ini。 */
std::wstring workspace_path() {
  wchar_t buffer[1024] = {0};
  const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, 1024);
  std::wstring dir;
  if (length > 0 && length < 1024) {
    dir = std::wstring(buffer) + L"\\Compositor";
  } else {
    /* 没有 APPDATA（精简环境）：退回 exe 同级，绿色包也能用。 */
    dir = exe_directory();
  }
  if (dir.empty()) return std::wstring();
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\workspace.ini";
}

}  // namespace

void workspace_save(const ShellState& shell) {
  shell.dock.save(workspace_path());
}

void workspace_load(ShellState& shell) {
  if (!shell.dock.load(workspace_path())) return;
  /*
   * 存的是「浮在外面」的状态：重启后要把窗口重新开出来，否则用户看到的
   * 是「面板不见了」而不是「面板浮在别处」。
   * 多块面板可同时浮动，逐块恢复。
   */
  for (int i = 0; i < static_cast<int>(comp_dock::PanelId::Count); ++i) {
    const comp_dock::PanelId id = static_cast<comp_dock::PanelId>(i);
    if (!shell.dock.is_floating(id)) continue;
    const comp_dock::PanelState& state = shell.dock.panel(id);
    if (float_panel_open(shell, shell.window, id)) {
      /* 用存下来的几何覆盖默认位置（窗口已建好，再摆一次）。 */
      if (state.float_w > 0 && state.float_h > 0) {
        const int dpi = shell_dpi(shell);
        const int scale = dpi > 0 ? dpi / 96 : 1;
        SetWindowPos(float_window_of(shell, id), nullptr, state.float_x, state.float_y,
                     state.float_w * scale, state.float_h * scale,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
  }
}

void float_panel_sync(ShellState& shell) {
  const int dpi = shell_dpi(shell);
  for (int i = 0; i < static_cast<int>(comp_dock::PanelId::Count); ++i) {
    HWND window = shell.float_windows[i];
    if (window == nullptr) continue;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) continue;
    comp_dock::PanelState& state = shell.dock.panel(static_cast<comp_dock::PanelId>(i));
    state.float_x = rect.left;
    state.float_y = rect.top;
    state.float_w = dpi_logical(rect.right - rect.left, dpi);
    state.float_h = dpi_logical(rect.bottom - rect.top, dpi);
  }
}

}  // namespace comp_shell
