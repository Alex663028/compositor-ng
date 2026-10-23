// AI生成
/*
 * shell_dpi.cpp — HiDPI（ROI #7）：进程级 DPI 感知 + 逻辑/设备像素换算。
 *
 * 口径（与 ui_core.h 的排版常量配套）：
 *   壳层所有布局常量、命中半径、行高都是**逻辑像素**（96 DPI 下的设备像素，
 *   即 Windows 的 DIP）。present() 在 memory DC 上开 MM_ANISOTROPIC，把逻辑
 *   坐标整体放大到设备坐标，因此绘制与命中测试都不必改；只有下面三类数字
 *   需要显式换算，它们是两个坐标系真正交界的地方：
 *     - 窗口客户区尺寸（GetClientRect 给设备像素，layout 要逻辑像素）；
 *     - 鼠标消息坐标（Windows 的鼠标坐标恒为设备像素）；
 *     - 窗口初始大小（CreateWindowEx 在 DPI 感知进程里按设备像素算）。
 *
 * 为什么是 Per-Monitor V2 而不是「系统感知」：系统感知在跨屏拖动时不会收到
 * WM_DPICHANGED，窗口会被系统位图拉伸（正是 ROI #7 要消灭的模糊）；V2 还会
 * 让非客户区（标题栏/菜单/滚动条）按新 DPI 重绘。
 *
 * 降级顺序：SetProcessDpiAwarenessContext(V2) → shcore.SetProcessDpiAwareness(2)
 * → SetProcessDPIAware()（Vista+ 的系统感知）。三项全部失败说明清单已经声明
 * 过 DPI 感知（ERROR_ACCESS_DENIED），同样算成功——那时以清单为准。
 *
 * 本文件不依赖引擎，也不含任何排版常量：换算只做 MulDiv + 夹取。
 */
#include <windows.h>

#include "shell_internal.h"

namespace comp_shell {
namespace {

/* DPI_AWARENESS_CONTEXT 在旧 SDK 里没有，用 HANDLE 声明即可（同尺寸）。 */
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)(VOID);

/* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4；不引用宏是为了
 * 兼容未声明该宏的头文件。 */
const HANDLE kPerMonitorAwareV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
const int kPerMonitorDpiAware = 2; /* PROCESS_PER_MONITOR_DPI_AWARE */

/* DPI 的合法区间：96（100%）到 480（500%）。越界一律当 96，绝不拿它做除数。 */
int clamp_dpi(UINT value) {
  if (value < 96 || value > 480) return 96;
  return static_cast<int>(value);
}

}  // namespace

void dpi_enable_awareness() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr && set_context(kPerMonitorAwareV2)) return;
  }
  HMODULE shcore = LoadLibraryW(L"shcore.dll");
  if (shcore != nullptr) {
    const auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
        GetProcAddress(shcore, "SetProcessDpiAwareness"));
    const bool ok = set_awareness != nullptr && SUCCEEDED(set_awareness(kPerMonitorDpiAware));
    FreeLibrary(shcore);
    if (ok) return;
  }
  /* Vista+ 的系统感知：至少保证 100% 以外不再被位图拉伸。 */
  SetProcessDPIAware();
}

int dpi_for_system() {
  static GetDpiForSystemFn get_dpi = []() -> GetDpiForSystemFn {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return nullptr;
    return reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
  }();
  if (get_dpi != nullptr) {
    const int dpi = clamp_dpi(get_dpi());
    if (dpi != 96) return dpi;
    /* GetDpiForSystem 只在 100% 时返回 96；继续往下走一次 GetDeviceCaps，
     * 旧系统与远程会话下它能给出更准确的值（且没有额外代价）。 */
  }
  HDC dc = GetDC(nullptr);
  if (dc == nullptr) return 96;
  const int dpi = clamp_dpi(static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)));
  ReleaseDC(nullptr, dc);
  return dpi;
}

int dpi_for_window(HWND window) {
  if (window == nullptr) return dpi_for_system();
  static GetDpiForWindowFn get_dpi = []() -> GetDpiForWindowFn {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return nullptr;
    return reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
  }();
  if (get_dpi != nullptr) {
    const int dpi = clamp_dpi(get_dpi(window));
    if (dpi != 96) return dpi;
  }
  /* 旧系统：窗口 DC 的 LOGPIXELSY 会跟着所在显示器走。 */
  HDC dc = GetDC(window);
  if (dc == nullptr) return dpi_for_system();
  const int dpi = clamp_dpi(static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)));
  ReleaseDC(window, dc);
  return dpi;
}

int dpi_logical(int device_px, int dpi) {
  if (dpi < 96) dpi = 96;
  if (dpi == 96) return device_px;
  return MulDiv(device_px, 96, dpi);
}

int dpi_device(int logical_px, int dpi) {
  if (dpi < 96) dpi = 96;
  if (dpi == 96) return logical_px;
  return MulDiv(logical_px, dpi, 96);
}

int shell_dpi(const ShellState& shell) {
  return shell.dpi < 96 ? 96 : shell.dpi;
}

}  // namespace comp_shell
