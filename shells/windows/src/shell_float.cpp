// AI生成
/*
 * shell_float.cpp — 浮动面板窗口的实现。
 *
 * 三件事：
 *   1. 建/拆一个独立顶层窗口（WS_OVERLAPPEDWINDOW：标题栏 + 可缩放边框）；
 *   2. 绘制：把面板内容画进自己的客户区（与停靠时同一份代码）；
 *   3. 交互：命中、拖动推进、拖动收尾都走 shell_panel_hit（与主窗口同一份）。
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

/* 浮动窗口当前承载哪块面板：存在窗口的 USERDATA 里。 */
comp_dock::PanelId panel_of(HWND window) {
  const LRESULT value = GetWindowLongPtrW(window, GWLP_USERDATA);
  return static_cast<comp_dock::PanelId>(value);
}

void set_panel_of(HWND window, comp_dock::PanelId id) {
  SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(id));
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
      if (shell != nullptr && shell->float_window == window) {
        shell->float_window = nullptr;
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

}  // namespace

bool float_panel_is_open(const ShellState& shell) { return shell.float_window != nullptr; }

/*
 * 浮 / 停回 的切换入口（菜单与双击标题都走这里）。
 *
 * 「一次只浮一块」是 dock 的不变式：两块都浮起来就要两套命中与焦点通路，
 * 本版只做一套。与其让第二块变成「能看不能点」，不如明确拒绝并说明。
 */
void toggle_float_panel(ShellState& shell, comp_dock::PanelId id) {
  if (shell.dock.is_floating(id)) {
    float_panel_close(shell, id);
    set_status(shell, id == comp_dock::PanelId::Layers ? L"图层面板已停回原位"
                                                        : L"调整面板已停回原位");
    return;
  }
  if (shell.float_window != nullptr) {
    set_status(shell, L"已有一块面板浮在外面了（一次只浮一块）");
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
  if (shell.float_window != nullptr) {
    /* 已经有浮动窗口：前置它，不再开第二个（不变式：至多一块浮动）。 */
    SetForegroundWindow(shell.float_window);
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
  RECT screen{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
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
  shell.float_window = window;
  shell.float_panel = id;
  shell.dock.float_panel(id, x, y, width, height);
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  return true;
}

void float_panel_close(ShellState& shell, comp_dock::PanelId id) {
  if (shell.float_window != nullptr) {
    HWND window = shell.float_window;
    shell.float_window = nullptr;
    /* DestroyWindow 会走 WM_DESTROY；那里再清一次句柄（幂等）。 */
    DestroyWindow(window);
  }
  shell.dock.dock_panel(id);
  /* 主窗口要重画：那块面板的列宽变了（浮走时让位，停回时归位）。 */
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
        SetWindowPos(shell.float_window, nullptr, state.float_x, state.float_y,
                     state.float_w * scale, state.float_h * scale,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
    break; /* 至多一块浮动 */
  }
}

void float_panel_sync(ShellState& shell) {
  if (shell.float_window == nullptr) return;
  RECT rect{};
  if (!GetWindowRect(shell.float_window, &rect)) return;
  const int dpi = shell_dpi(shell);
  comp_dock::PanelState& state = shell.dock.panel(shell.float_panel);
  state.float_x = rect.left;
  state.float_y = rect.top;
  state.float_w = dpi_logical(rect.right - rect.left, dpi);
  state.float_h = dpi_logical(rect.bottom - rect.top, dpi);
}

}  // namespace comp_shell
