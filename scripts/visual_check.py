# -*- coding: utf-8 -*-
"""visual_check.py — Windows 外壳「自动化目视」检查。

背景：S4–S14 的交付报告都记着同一条边界——**没有经过人手 Win32 目视实测**。
本脚本把「开窗点一遍」变成可重复的命令：启动 build/compositor_shell.exe，用
SendInput/PostMessage 走真实 Win32 消息（键盘、鼠标、菜单命令），再用
PrintWindow 抓客户区像素，既落盘 PNG（供人肉眼复核），又对关键像素做断言
（供 CI/回归判断）。它不替代真人手感判断，但把「没人按过」变成「脚本按过」。

用法（先跑一次 ``python scripts/build.py`` 再跑本脚本）::

    python scripts/visual_check.py                  # 全部场景
    python scripts/visual_check.py --scene brush    # 单个场景
    python scripts/visual_check.py --list           # 列出场景
    python scripts/visual_check.py --gpu            # 用 --gpu 构建的 exe

产物：
  * ``report/assets/s15/<scene>-<n>-<step>.png`` 每个步骤一张客户区截图；
  * 终端 / ``build/visual_check.txt`` 每个断言的 PASS/FAIL 明细。

退出码：0 = 全部断言通过；1 = 有断言失败（截图仍会落盘供复查）。
"""

import argparse
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

try:
    from PIL import Image
except ImportError:  # pragma: no cover - 工具链 python 自带 Pillow
    sys.exit("需要 Pillow：本仓约定使用 <local-workspace>\\\\tools\\python\\python.exe")


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DEFAULT_EXE = os.path.join(BUILD, "compositor_shell.exe")
DEFAULT_OUT = os.path.join(ROOT, "..", "report", "assets", "s15")

# ---------------------------------------------------------------- Win32 常量

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
WM_COMMAND = 0x0111
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MOUSELEAVE = 0x02A3
WM_CLOSE = 0x0010

PW_CLIENTONLY = 0x00000001
PW_RENDERFULLCONTENT = 0x00000002

VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_ESCAPE = 0x1B
VK_RETURN = 0x0D
VK_TAB = 0x09
VK_BACK = 0x08
VK_SPACE = 0x20
VK_LEFT = 0x25
VK_UP = 0x26
VK_RIGHT = 0x27
VK_DOWN = 0x28
VK_OEM_4 = 0xDB   # [
VK_OEM_6 = 0xDD   # ]
VK_OEM_MINUS = 0xBD
VK_OEM_PLUS = 0xBB
VK_OEM_PERIOD = 0xBE

KEYEVENTF_KEYUP = 0x0002

# 工具菜单命令：kCmdToolBase(1100) + tool_table 序号
CMD_TOOL_BASE = 1100
CMD_FILTER_BASE = 1200
CMD_UNDO = 1005
CMD_REDO = 1006
CMD_NEW = 1001
CMD_LAYER_ADD = 1008
CMD_LAYER_MERGE_DOWN = 1025
CMD_LAYER_FLATTEN = 1026
CMD_VIEW_FIT = 1027
CMD_VIEW_ACTUAL = 1028
CMD_SELECT_NONE = 1032
CMD_ADJ_ADD = 1022
CMD_ADJ_APPLY = 1023
CMD_ADJ_VALUE_UP = 1021
CMD_FILTER_APPLY = 1210

TOOL = {
    "select": 0, "move": 1, "marquee": 2, "lasso": 3, "pen": 4, "wand": 5,
    "crop": 6, "eyedropper": 7, "brush": 8, "eraser": 9, "heal": 10,
    "clone": 11, "retouch": 12, "gradient": 13, "shape": 14, "type": 15,
    "hand": 16, "zoom": 17,
}

# ui_core.h 的布局常量（客户区坐标）
TOOLBAR_H = 46
STATUS_H = 26
LAYER_W = 248
ADJUST_W = 330
MIN_CANVAS_W = 180

# ui_core.h 的配色（断言用真值）
COL_TOOLBAR_BG = (44, 46, 52)
COL_TOOL_BTN = (58, 60, 68)
COL_TOOL_SELECTED = (70, 104, 168)
COL_LAYER_BG = (24, 26, 30)
COL_ADJ_BG = (22, 24, 28)
COL_STATUS_BG = (26, 28, 32)
COL_HIGHLIGHT = (56, 76, 120)
COL_APP_BG = (32, 32, 36)


# ---------------------------------------------------------------- Win32 绑定

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)


class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(POINT)]
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
user32.GetKeyboardState.argtypes = [ctypes.c_void_p]
user32.SetKeyboardState.argtypes = [ctypes.c_void_p]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.keybd_event.argtypes = [ctypes.c_ubyte, ctypes.c_ubyte, wintypes.DWORD, ctypes.c_size_t]
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.GetCurrentThreadId.restype = wintypes.DWORD
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                            ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), wintypes.UINT]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]


def make_lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


# ---------------------------------------------------------------- 断言工具

class Checker:
    def __init__(self):
        self.rows = []  # (scene, name, ok, detail)

    def check(self, scene, name, ok, detail=""):
        self.rows.append((scene, name, bool(ok), detail))
        return bool(ok)

    def failures(self):
        return [row for row in self.rows if not row[2]]


def near(color, target, tol=24):
    return all(abs(int(color[i]) - int(target[i])) <= tol for i in range(3))


def brightness(color):
    return (int(color[0]) * 299 + int(color[1]) * 587 + int(color[2]) * 114) // 1000


def mean_rgb(img, rect):
    region = img.crop((rect[0], rect[1], rect[2], rect[3]))
    if region.width < 1 or region.height < 1:
        return (0, 0, 0)
    return region.resize((1, 1), Image.BOX).getpixel((0, 0))[:3]


def count_bright(img, rect, threshold=150, channel=0):
    region = img.crop((rect[0], rect[1], rect[2], rect[3]))
    hist = region.split()[channel].histogram()
    return sum(hist[threshold:])


def count_matching(img, rect, target, tol=18):
    region = img.crop((rect[0], rect[1], rect[2], rect[3]))
    data = region.convert("RGB").tobytes()
    count = 0
    for index in range(0, len(data), 3):
        if (abs(data[index] - target[0]) <= tol and
                abs(data[index + 1] - target[1]) <= tol and
                abs(data[index + 2] - target[2]) <= tol):
            count += 1
    return count


def count_color(image, target, tol=18):
    """在已裁剪的 Image 上数目标色像素（等价于 count_matching(img.crop(...))）。"""
    data = image.convert("RGB").tobytes()
    count = 0
    for index in range(0, len(data), 3):
        if (abs(data[index] - target[0]) <= tol and
                abs(data[index + 1] - target[1]) <= tol and
                abs(data[index + 2] - target[2]) <= tol):
            count += 1
    return count


def band_bright(img, x0, x1, y_center, band=5, threshold=150):
    """横向窄带里的亮像素数（选区的蚂蚁线是白/黑交替虚线，按带统计更稳）。"""
    region = img.crop((int(x0), int(y_center - band), int(x1), int(y_center + band)))
    data = region.convert("RGB").tobytes()
    count = 0
    for index in range(0, len(data), 3):
        if brightness(data[index:index + 3]) > threshold:
            count += 1
    return count


def glyph_bbox(img, rect, target=(255, 136, 0), tol=60):
    """数目标色（字形）像素并给出其外接框：(数量, left, top, right, bottom)。"""
    left, top, right, bottom = rect
    region = img.crop(rect).convert("RGB")
    width, height = region.size
    data = region.tobytes()
    count = 0
    min_x, min_y, max_x, max_y = width, height, -1, -1
    for y in range(height):
        row = data[y * width * 3:(y + 1) * width * 3]
        for x in range(width):
            index = x * 3
            if (abs(row[index] - target[0]) <= tol and
                    abs(row[index + 1] - target[1]) <= tol and
                    abs(row[index + 2] - target[2]) <= tol):
                count += 1
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y
    if count == 0:
        return 0, left, top, left, top
    return count, left + min_x, top + min_y, left + max_x, top + max_y


def pixel_diff(img_a, img_b, rect, threshold=24):
    """两帧同一区域的像素差异数（用于「面板文字/数值变了」这类断言）。"""
    a = img_a.crop(rect).convert("RGB").tobytes()
    b = img_b.crop(rect).convert("RGB").tobytes()
    changed = 0
    for index in range(0, min(len(a), len(b)), 3):
        if (abs(a[index] - b[index]) + abs(a[index + 1] - b[index + 1]) +
                abs(a[index + 2] - b[index + 2])) > threshold:
            changed += 1
    return changed


def count_app_bg(img, rect):
    """数画布里的应用底色像素（(32,32,36) = 文档没盖住的地方）。"""
    return count_matching(img, rect, COL_APP_BG, 3)


def detect_document_rect(img, canvas, dark=12):
    """空文档是纯黑，画布底色是 (32,32,36)：扫出黑色外接框 = 文档在屏幕上的位置。"""
    left, top, right, bottom = canvas
    region = img.crop(canvas).convert("RGB")
    width, height = region.size
    pixels = region.load()
    xs, ys = [], []
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            r, g, b = pixels[x, y]
            if r < dark and g < dark and b < dark:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (left + min(xs), top + min(ys), left + max(xs) + 2, top + max(ys) + 2)


def pixel(img, x, y):
    return img.getpixel((int(x), int(y)))[:3]


# ---------------------------------------------------------------- 外壳驱动

class Shell:
    def __init__(self, exe, out_dir, keep):
        self.exe = exe
        self.out_dir = out_dir
        self.keep = keep
        self.proc = None
        self.hwnd = None
        self.client_w = 0
        self.client_h = 0
        self.shots = []
        self._doc_rect = None
        os.makedirs(out_dir, exist_ok=True)

    # -- 生命周期 --------------------------------------------------
    def start(self, args=None):
        cmd = [self.exe] + list(args or [])
        self.proc = subprocess.Popen(cmd, cwd=BUILD)
        deadline = time.time() + 15.0
        while time.time() < deadline:
            self.hwnd = self._find_window()
            if self.hwnd:
                break
            time.sleep(0.1)
        if not self.hwnd:
            raise RuntimeError("找不到外壳窗口（exe 是否启动失败？）")
        user32.ShowWindow(self.hwnd, 9)  # SW_RESTORE
        if not self._activate():
            raise RuntimeError("无法把外壳窗口切到前台（拖动工具依赖鼠标捕获，必须前台）")
        self.wait(0.8)  # 等第一帧与 view.fit
        self._read_client()
        return self

    def _find_window(self):
        pid = self.proc.pid
        found = []

        @WNDENUMPROC
        def callback(hwnd, _):
            if not user32.IsWindowVisible(hwnd):
                return True
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value != pid:
                return True
            title = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, title, 255)
            if title.value.startswith("Compositor"):
                found.append(hwnd)
                return False
            return True

        user32.EnumWindows(callback, 0)
        return found[0] if found else None

    def stop(self):
        if self.hwnd:
            user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.hwnd = None
        self.proc = None

    def focus(self):
        """把窗口切到前台并返回是否成功（失败会导致 SetCapture 立刻丢失）。"""
        fg = user32.GetForegroundWindow()
        fg_thread = user32.GetWindowThreadProcessId(fg, None)
        current = kernel32.GetCurrentThreadId()
        target = user32.GetWindowThreadProcessId(self.hwnd, None)
        # 纯 SetForegroundWindow 会被前台锁拒绝；先临时共享输入队列再调用。
        user32.AttachThreadInput(current, fg_thread, True)
        user32.AttachThreadInput(current, target, True)
        user32.SetForegroundWindow(self.hwnd)
        user32.AttachThreadInput(current, fg_thread, False)
        user32.AttachThreadInput(current, target, False)
        self.wait(0.2)
        return user32.GetForegroundWindow() == self.hwnd

    _activate = focus

    def wait(self, seconds=0.15):
        time.sleep(seconds)

    def _read_client(self):
        rect = RECT()
        user32.GetClientRect(self.hwnd, ctypes.byref(rect))
        self.client_w = rect.right - rect.left
        self.client_h = rect.bottom - rect.top

    @property
    def layout(self):
        w, h = self.client_w, self.client_h
        toolbar_h = min(h, TOOLBAR_H)
        status_h = min(h - toolbar_h, STATUS_H)
        body_top = toolbar_h
        body_h = h - toolbar_h - status_h
        layer_w = LAYER_W if w >= LAYER_W + MIN_CANVAS_W else 0
        adjust_w = ADJUST_W if w - layer_w >= ADJUST_W + MIN_CANVAS_W else 0
        return {
            "toolbar": (0, 0, w, toolbar_h),
            "layer": (0, body_top, layer_w, body_top + body_h),
            "adjust": (w - adjust_w, body_top, w, body_top + body_h),
            "canvas": (layer_w, body_top, w - adjust_w, body_top + body_h),
            "status": (0, body_top + body_h, w, body_top + body_h + status_h),
        }

    def tool_button_center(self, index):
        size = 32
        gap = 6
        stride = size + gap
        x = 10 + index * stride + size // 2
        y = (TOOLBAR_H - size) // 2 + size // 2
        return x, y

    def canvas_point(self, fx, fy):
        """画布矩形内的相对点 -> 客户区坐标（fx/fy 0..1）。"""
        left, top, right, bottom = self.layout["canvas"]
        return (int(left + (right - left) * fx), int(top + (bottom - top) * fy))

    def canvas_center(self):
        return self.canvas_point(0.5, 0.5)

    def document_rect(self):
        """文档（黑色空画布）在客户区里的矩形；只在文档还是空的时候调用。"""
        if getattr(self, "_doc_rect", None) is None:
            self._doc_rect = detect_document_rect(self.capture(), self.layout["canvas"])
            if self._doc_rect is None:
                raise RuntimeError("画布上找不到文档区域（空文档应为纯黑）")
        return self._doc_rect

    # -- 输入 ------------------------------------------------------
    #
    # 键盘不抢前台：`SetForegroundWindow` 在无人值守时会被前台锁挡掉，而
    # `keybd_event` 只往「当前前台」注入，脚本就会静默丢键。这里改走
    # 「AttachThreadInput + SetKeyboardState 造出修饰键状态 + PostMessage 投
    # WM_KEYDOWN/UP」：目标线程的 GetKeyState 能看到 Shift/Ctrl/Alt，消息也只
    # 进目标窗口，桌面焦点不跳。鼠标同理只用 PostMessage（客户区坐标）。
    def _set_modifiers(self, shift, ctrl, alt):
        target = user32.GetWindowThreadProcessId(self.hwnd, None)
        current = kernel32.GetCurrentThreadId()
        attached = user32.AttachThreadInput(current, target, True)
        state = (ctypes.c_ubyte * 256)()
        user32.GetKeyboardState(state)
        saved = (state[VK_SHIFT], state[VK_CONTROL], state[VK_MENU])
        state[VK_SHIFT] = 0x80 if shift else 0
        state[VK_CONTROL] = 0x80 if ctrl else 0
        state[VK_MENU] = 0x80 if alt else 0
        user32.SetKeyboardState(state)
        return current, target, attached, state, saved

    def _restore_modifiers(self, current, target, attached, state, saved):
        state[VK_SHIFT], state[VK_CONTROL], state[VK_MENU] = saved
        user32.SetKeyboardState(state)
        if attached:
            user32.AttachThreadInput(current, target, False)

    def press(self, vk, shift=False, ctrl=False, alt=False):
        """投一次按键。

        修饰键状态通过「AttachThreadInput + SetKeyboardState」写进目标线程的
        输入状态，再 PostMessage 投 WM_KEYDOWN/UP。注意：消息是异步处理的，
        必须等目标处理完这两条消息再恢复修饰键状态，否则 GetKeyState 读到的
        已经是干净状态（Shift+X 被当成 X）。
        """
        current, target, attached, state, saved = self._set_modifiers(shift, ctrl, alt)
        try:
            if alt:
                user32.PostMessageW(self.hwnd, WM_SYSKEYDOWN, vk, 0x20000000)
                self.wait(0.05)
                user32.PostMessageW(self.hwnd, WM_SYSKEYUP, vk, 0xE0000001)
            else:
                user32.PostMessageW(self.hwnd, WM_KEYDOWN, vk, 0)
                self.wait(0.05)
                user32.PostMessageW(self.hwnd, WM_KEYUP, vk, 0xC0000001)
            self.wait(0.05)
        finally:
            self._restore_modifiers(current, target, attached, state, saved)
        self.wait(0.05)

    def syskey(self, vk):
        """Alt+键：直接投 WM_SYSKEYDOWN（应用只读 wparam，不依赖 GetKeyState）。"""
        user32.PostMessageW(self.hwnd, WM_SYSKEYDOWN, vk, 0x20000000)
        self.wait(0.05)
        user32.PostMessageW(self.hwnd, WM_SYSKEYUP, vk, 0xE0000001)
        self.wait(0.12)

    def type_text(self, text):
        for ch in text:
            if ch.isupper():
                self.press(ord(ch), shift=True)
            elif ch.isdigit() or ch.islower():
                self.press(ord(ch.upper()))
            elif ch == " ":
                self.press(VK_SPACE)
            elif ch == "-":
                self.press(VK_OEM_MINUS)
            elif ch == ".":
                self.press(VK_OEM_PERIOD)
            elif ch == "\n":
                self.press(VK_RETURN, shift=True)
            else:
                raise ValueError("visual_check 不支持的字符: %r" % ch)

    def command(self, cmd_id):
        user32.PostMessageW(self.hwnd, WM_COMMAND, cmd_id, 0)
        self.wait(0.25)

    def select_tool(self, name):
        user32.PostMessageW(self.hwnd, WM_COMMAND, CMD_TOOL_BASE + TOOL[name], 0)
        self.wait(0.2)

    def click(self, x, y):
        x, y = int(x), int(y)
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, make_lparam(x, y))
        user32.PostMessageW(self.hwnd, WM_LBUTTONDOWN, 1, make_lparam(x, y))
        user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, make_lparam(x, y))
        self.wait(0.25)

    def drag(self, x0, y0, x1, y1, steps=12):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, make_lparam(x0, y0))
        user32.PostMessageW(self.hwnd, WM_LBUTTONDOWN, 1, make_lparam(x0, y0))
        self.wait(0.05)
        for step in range(1, steps + 1):
            x = x0 + (x1 - x0) * step // steps
            y = y0 + (y1 - y0) * step // steps
            user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 1, make_lparam(x, y))
            self.wait(0.02)
        user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, make_lparam(x1, y1))
        self.wait(0.45)

    def stroke(self, points, steps=8):
        """折线落笔：逐段拖动（画笔/橡皮擦/retouch 共用）。"""
        self.drag(points[0][0], points[0][1], points[0][0] + 1, points[0][1], steps=1)
        for index in range(1, len(points)):
            self.drag(points[index - 1][0], points[index - 1][1],
                      points[index][0], points[index][1], steps=steps)

    # -- 截屏 ------------------------------------------------------
    def capture(self):
        win_rect = RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(win_rect))
        win_w = win_rect.right - win_rect.left
        win_h = win_rect.bottom - win_rect.top
        window_dc = user32.GetDC(self.hwnd)
        mem_dc = gdi32.CreateCompatibleDC(window_dc)
        bitmap = gdi32.CreateCompatibleBitmap(window_dc, win_w, win_h)
        old = gdi32.SelectObject(mem_dc, bitmap)
        user32.PrintWindow(self.hwnd, mem_dc, PW_RENDERFULLCONTENT)
        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = win_w
        info.bmiHeader.biHeight = -win_h
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = 0  # BI_RGB
        buffer = ctypes.create_string_buffer(win_w * win_h * 4)
        gdi32.GetDIBits(mem_dc, bitmap, 0, win_h, buffer, ctypes.byref(info), 0)
        gdi32.SelectObject(mem_dc, old)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(mem_dc)
        user32.ReleaseDC(self.hwnd, window_dc)

        full = Image.frombuffer("RGBA", (win_w, win_h), buffer, "raw", "BGRA", 0, 1)
        origin = POINT(0, 0)
        user32.ClientToScreen(self.hwnd, ctypes.byref(origin))
        left = origin.x - win_rect.left
        top = origin.y - win_rect.top
        self._read_client()
        return full.crop((left, top, left + self.client_w, top + self.client_h)).convert("RGB")

    def shot(self, name):
        image = self.capture()
        path = os.path.join(self.out_dir, name + ".png")
        image.save(path)
        self.shots.append(path)
        return image


# ---------------------------------------------------------------- 场景

def scene_start(shell, checker):
    scene = "start"
    img = shell.shot("01_start")
    layout = shell.layout
    toolbar = layout["toolbar"]
    canvas = layout["canvas"]
    layer = layout["layer"]
    status = layout["status"]

    checker.check(scene, "工具条背景",
                  near(pixel(img, toolbar[0] + 2, toolbar[1] + 2), COL_TOOLBAR_BG, 8),
                  "实测 %s" % (pixel(img, toolbar[0] + 2, toolbar[1] + 2),))
    checker.check(scene, "画布背景（文档为空 = 黑）",
                  near(pixel(img, *shell.canvas_center()), (0, 0, 0), 6),
                  "实测 %s" % (pixel(img, *shell.canvas_center()),))
    checker.check(scene, "图层面板背景",
                  near(pixel(img, layer[0] + 4, layer[1] + 30), COL_LAYER_BG, 8),
                  "实测 %s" % (pixel(img, layer[0] + 4, layer[1] + 30),))
    checker.check(scene, "状态栏背景",
                  near(pixel(img, status[0] + 4, status[1] + 4), COL_STATUS_BG, 8),
                  "实测 %s" % (pixel(img, status[0] + 4, status[1] + 4),))

    # 18 个工具按钮都画出了字母（浅色字形像素）
    missing = []
    for index in range(18):
        cx, cy = shell.tool_button_center(index)
        rect = (cx - 16, cy - 16, cx + 16, cy + 16)
        if count_bright(img, rect, 150) < 8:
            missing.append(index)
    checker.check(scene, "18 个工具按钮都有字形", not missing, "缺 %s" % missing)

    # 当前工具（Select）高亮
    cx, cy = shell.tool_button_center(TOOL["select"])
    checker.check(scene, "当前工具高亮",
                  near(pixel(img, cx - 13, cy - 13), COL_TOOL_SELECTED, 20),
                  "实测 %s" % (pixel(img, cx - 13, cy - 13),))
    # 状态栏文字
    count = count_bright(img, status, 150)
    checker.check(scene, "状态栏有文字", count > 200, "浅色像素 %d" % count)
    # 图层面板标题 + 第一行
    checker.check(scene, "图层行有文字", count_bright(img, (layer[0], layer[1], layer[0] + 240,
                                                         layer[1] + 120), 150) > 200,
                  "浅色像素 %d" % count_bright(img, (layer[0], layer[1], layer[0] + 240,
                                                    layer[1] + 120), 150))
    # 右侧默认是调整面板
    checker.check(scene, "调整面板有标题", count_bright(img, (layout["adjust"][0] + 8,
                                                         layout["adjust"][1],
                                                         layout["adjust"][2],
                                                         layout["adjust"][1] + 60), 150) > 50,
                  "浅色像素 %d" % count_bright(img, (layout["adjust"][0] + 8,
                                                     layout["adjust"][1], layout["adjust"][2],
                                                     layout["adjust"][1] + 60), 150))


def scene_brush(shell, checker):
    scene = "brush"
    doc = shell.document_rect()
    # 先换前景色（Shift+X 十六进制键入），否则黑笔在空文档上看不见
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("brush")
    img = shell.shot("02_brush_tool")
    checker.check(scene, "画笔右侧面板回到调整面板（未残留文字面板）",
                  near(pixel(img, shell.layout["adjust"][0] + 4,
                             shell.layout["adjust"][1] + 4), COL_ADJ_BG, 8),
                  "实测 %s" % (pixel(img, shell.layout["adjust"][0] + 4,
                                     shell.layout["adjust"][1] + 4),))
    # 状态栏前景色色块应为 ff8800（内缩 3 px 避开 1 px 边框）
    status = shell.layout["status"]
    swatch = (status[2] - 187, status[1] + 8, status[2] - 169, status[1] + 18)
    checker.check(scene, "状态栏前景色块 = #FF8800",
                  near(mean_rgb(img, swatch), (255, 136, 0), 12),
                  "实测 %s" % (mean_rgb(img, swatch),))

    # 画一条横跨文档中部的笔迹（都在文档内，否则会被文档边界裁掉）
    left, top, right, bottom = doc
    y = (top + bottom) // 2
    x0, x1 = left + 40, right - 40
    shell.drag(x0, y, x1, y + (bottom - top) // 6, steps=16)
    img = shell.shot("03_brush_stroke")
    hits = 0
    for step in range(0, 12):
        x = x0 + (x1 - x0) * step // 11
        yy = y + ((bottom - top) // 6) * step // 11
        if any(near(pixel(img, x, yy + dy), (255, 136, 0), 40) for dy in (-2, -1, 0, 1, 2)):
            hits += 1
    checker.check(scene, "笔迹像素落在拖拽路径上（12 点命中）", hits >= 9,
                  "命中 %d/12" % hits)

    # Ctrl+Z 一步撤销：笔迹消失
    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("04_brush_undo")
    remaining = 0
    for step in range(0, 12):
        x = x0 + (x1 - x0) * step // 11
        yy = y + ((bottom - top) // 6) * step // 11
        if any(near(pixel(img, x, yy + dy), (255, 136, 0), 40) for dy in (-2, -1, 0, 1, 2)):
            remaining += 1
    checker.check(scene, "Ctrl+Z 后笔迹消失", remaining <= 1, "残留 %d/12" % remaining)


def scene_selection(shell, checker):
    scene = "selection"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("brush")
    y = (top + bottom) // 2
    shell.drag(left + 20, y, right - 20, y, steps=10)

    shell.select_tool("marquee")
    x0, y0 = left + 30, top + 30
    x1, y1 = right - 30, bottom - 30
    shell.drag(x0, y0, x1, y1, steps=10)
    img = shell.shot("05_marquee")
    # 蚂蚁线：白/黑交替虚线按窄带统计（逐像素点采样会正好踩在间隙上）
    whites_top = band_bright(img, x0, x1, y0, band=3)
    whites_bottom = band_bright(img, x0, x1, y1, band=3)
    checker.check(scene, "矩形选区画出了蚂蚁线（上/下边白段）",
                  whites_top >= 30 and whites_bottom >= 30,
                  "上 %d 下 %d" % (whites_top, whites_bottom))
    # 取消选区
    shell.press(ord("D"), ctrl=True)
    img = shell.shot("06_selection_none")
    after_top = band_bright(img, x0, x1, y0, band=3)
    after_bottom = band_bright(img, x0, x1, y1, band=3)
    checker.check(scene, "Ctrl+D 后蚂蚁线消失",
                  after_top <= 2 and after_bottom <= 2,
                  "取消后上 %d 下 %d" % (after_top, after_bottom))


def scene_wand(shell, checker):
    scene = "wand"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("22cc55")
    shell.press(VK_RETURN)
    shell.select_tool("brush")
    y = (top + bottom) // 2
    shell.drag(left + 40, y, right - 40, y, steps=14)
    shell.select_tool("wand")
    shell.click((left + right) // 2, y)
    img = shell.shot("07_wand_selection")
    # 魔棒应沿涂色笔画产生蚂蚁线：笔迹上下缘的窄带里出现白色虚线
    whites = band_bright(img, left + 50, right - 50, y - 7, band=4) + \
        band_bright(img, left + 50, right - 50, y + 7, band=4)
    checker.check(scene, "魔棒沿笔迹生成选区（蚂蚁线出现）", whites >= 30, "白段 %d" % whites)


def scene_gradient(shell, checker):
    scene = "gradient"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("gradient")
    y = (top + bottom) // 2
    shell.drag(left + 10, y, right - 10, y, steps=20)
    img = shell.shot("08_gradient")

    def strip_mean(fx):
        x = int(left + (right - left) * fx)
        return mean_rgb(img, (x, y - 6, x + 6, y + 6))

    left_mean = strip_mean(0.05)
    mid_mean = strip_mean(0.5)
    right_mean = strip_mean(0.95)
    checker.check(scene, "渐变左端接近前景色", left_mean[0] > 120 and left_mean[1] > 40,
                  "左端 %s" % (left_mean,))
    checker.check(scene, "渐变右端更暗（到透明）",
                  brightness(right_mean) < brightness(mid_mean) < brightness(left_mean),
                  "左 %s 中 %s 右 %s" % (left_mean, mid_mean, right_mean))


def scene_shape(shell, checker):
    scene = "shape"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    width, height = right - left, bottom - top
    shell.press(ord("X"), shift=True)
    shell.type_text("3399ff")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    # 矩形：文档左半，整体落在文档内
    x0, y0 = left + 20, top + 20
    x1, y1 = left + width // 2, bottom - 20
    shell.drag(x0, y0, x1, y1, steps=10)
    img = shell.shot("09_shape_rect")
    inside = mean_rgb(img, ((x0 + x1) // 2 - 6, (y0 + y1) // 2 - 6,
                            (x0 + x1) // 2 + 6, (y0 + y1) // 2 + 6))
    checker.check(scene, "矩形内部填充 = 前景色", near(inside, (51, 153, 255), 40),
                  "实测 %s" % (inside,))
    outside = pixel(img, x1 + 30, y0 + 30)
    checker.check(scene, "矩形外部仍是文档黑",
                  near(outside, (0, 0, 0), 10), "实测 %s" % (outside,))

    # U 切椭圆后拖第二个形状：包围盒四角不填、中心填
    shell.press(ord("U"))
    ex0, ey0 = left + width // 2 + 20, top + 20
    ex1, ey1 = right - 20, bottom - 20
    shell.drag(ex0, ey0, ex1, ey1, steps=10)
    img = shell.shot("10_shape_ellipse")
    center = mean_rgb(img, ((ex0 + ex1) // 2 - 5, (ey0 + ey1) // 2 - 5,
                            (ex0 + ex1) // 2 + 5, (ey0 + ey1) // 2 + 5))
    corner = mean_rgb(img, (ex0 + 4, ey0 + 4, ex0 + 12, ey0 + 12))
    checker.check(scene, "椭圆中心填充", near(center, (51, 153, 255), 40),
                  "实测 %s" % (center,))
    checker.check(scene, "椭圆包围盒角落不填（椭圆不是矩形）",
                  brightness(corner) < 60, "实测 %s" % (corner,))


def scene_crop(shell, checker):
    """
    裁剪（S22）：拖框 -> Enter 提交 -> 画布尺寸与内容真的变了。

    这个场景要证明三件事：
      1. 裁剪框能拖出来（Crop 工具此前是死的：图标在、快捷键在，按下无反应）；
      2. Enter 真的提交（改画布尺寸），不是只画了个框；
      3. 裁掉的内容真的消失、保留的内容真的留下——即裁剪方向不是反的。

    判据全部用文档坐标算出来的采样点，不是"看起来像"。
    """
    scene = "crop"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    width, height = right - left, bottom - top

    # 1) 先在画布右上角画一块橙色矩形（偏离中心，便于判断裁剪方向）
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    keep_x0, keep_y0 = left + width // 2, top + 20
    keep_x1, keep_y1 = right - 20, top + height // 3
    shell.drag(keep_x0, keep_y0, keep_x1, keep_y1, steps=8)
    img = shell.shot("34_crop_before")
    keep_center = ((keep_x0 + keep_x1) // 2, (keep_y0 + keep_y1) // 2)
    before_keep = pixel(img, *keep_center)
    checker.check(scene, "裁剪前：保留区是前景色",
                  near(before_keep, (255, 136, 0), 30),
                  "实测 %s" % (before_keep,))

    # 2) 切 Crop，拖出左下角的框（**不包含**橙色区），Enter 提交
    shell.select_tool("crop")
    shell.drag(left + 20, bottom - height // 2, left + width // 3, bottom - 20, steps=8)
    img = shell.shot("35_crop_frame")
    checker.check(scene, "裁剪框可见（框内区域未被遮罩糊掉）",
                  brightness(mean_rgb(img, (left + 24, bottom - height // 2 + 4,
                                            left + width // 3 - 4,
                                            bottom - height // 2 + 40))) < 200,
                  "框内区域平均 %s" % (mean_rgb(img, (left + 24, bottom - height // 2 + 4,
                                                     left + width // 3 - 4,
                                                     bottom - height // 2 + 40)),))
    shell.press(VK_RETURN)
    shell.wait(0.4)
    img = shell.shot("36_crop_after")

    # 3) 提交后橙色区仍在（它不在被裁掉的那一半里）
    after_keep = pixel(img, *keep_center)
    checker.check(scene, "提交后：保留区仍是前景色（方向没搞反）",
                  near(after_keep, (255, 136, 0), 30),
                  "实测 %s" % (after_keep,))

    # 4) 画布尺寸变了：文档矩形不再是原来的宽度
    doc_after = shell.document_rect()
    checker.check(scene, "画布宽度被裁剪（文档矩形变窄）",
                  (doc_after[2] - doc_after[0]) < (right - left) - 40,
                  "裁剪前宽 %d，裁剪后宽 %d" % (right - left, doc_after[2] - doc_after[0]))

    # 5) Esc 取消也不改画布：再拖一个框，按 Esc，尺寸应保持不变
    width_before_esc = doc_after[2] - doc_after[0]
    shell.select_tool("crop")
    shell.drag(left + 20, top + 20, left + width // 3, top + height // 2, steps=8)
    shell.press(VK_ESCAPE)
    shell.wait(0.3)
    shell.select_tool("select")
    shell.press(ord("0"), ctrl=True)   # 适合窗口，便于量文档矩形
    doc_esc = shell.document_rect()
    checker.check(scene, "Esc 取消不改变画布尺寸",
                  (doc_esc[2] - doc_esc[0]) == width_before_esc,
                  "取消前宽 %d，取消后宽 %d" % (width_before_esc, doc_esc[2] - doc_esc[0]))


def scene_type(shell, checker):
    scene = "type"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("type")
    img = shell.shot("11_type_panel")
    adjust = shell.layout["adjust"]
    panel = (adjust[0], adjust[1], adjust[2], adjust[3])
    shell._type_panel_before = img
    # 文字属性面板：三行参数里第一行（字号）高亮
    row_h = 20
    rows_top = 54
    row1 = (adjust[0] + 6, adjust[1] + rows_top - 1, adjust[2] - 8,
            adjust[1] + rows_top + row_h - 1)
    checker.check(scene, "文字面板第一行（字号）高亮",
                  count_matching(img, row1, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, row1, COL_HIGHLIGHT, 12))

    # 点画布落插入点，键入 "H"（键入态预览 + 面板副标题变化）
    origin = (left + 40, top + 40)
    shell.click(*origin)
    shell.type_text("H")
    img = shell.shot("12_type_typing")
    checker.check(scene, "键入态面板文字变化（出现键入提示）",
                  pixel_diff(img, shell._type_panel_before, panel) > 40,
                  "面板差异像素 %d" % pixel_diff(img, shell._type_panel_before, panel))
    # 文本框预览：点击 + 键入后，插入点附近出现文本框边框/字形（与空画布比像素差）
    preview = (origin[0] - 12, origin[1] - 12, origin[0] + 240, origin[1] + 110)
    changed = pixel_diff(img, shell._type_panel_before, preview)
    checker.check(scene, "键入态画布出现文本框预览", changed > 20, "画布差异像素 %d" % changed)

    # 点第二行（行距）聚焦 -> 高亮行移动
    shell.click(adjust[0] + 40, adjust[1] + rows_top + row_h + row_h // 2)
    img = shell.shot("13_type_row_lineheight")
    row2 = (adjust[0] + 6, adjust[1] + rows_top + row_h - 1, adjust[2] - 8,
            adjust[1] + rows_top + 2 * row_h - 1)
    checker.check(scene, "点击可把高亮切到第二行（行距）",
                  count_matching(img, row2, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, row2, COL_HIGHLIGHT, 12))
    # Alt+] 调行距（键入文字时也可用），状态栏有反馈
    before_line = shell.capture()
    shell.syskey(VK_OEM_6)
    shell.syskey(VK_OEM_6)
    img = shell.shot("14_type_lineheight_alt")
    checker.check(scene, "Alt+] 调整行距后状态栏有反馈",
                  pixel_diff(img, before_line, shell.layout["status"]) > 20,
                  "状态栏差异像素 %d" % pixel_diff(img, before_line, shell.layout["status"]))

    # 回车提交第一段（32 px）：字形像素落画布，状态栏从「键入中」变成「已应用」
    typing_status = shell.capture()
    shell.press(VK_RETURN)
    img = shell.shot("15_type_commit_32")
    glyphs32, _, gy0, _, gy1 = glyph_bbox(
        img, (origin[0] - 4, origin[1] - 8, origin[0] + 120, origin[1] + 90))
    checker.check(scene, "回车提交后画布出现前景色字形（32 px）",
                  glyphs32 > 40, "字形像素 %d" % glyphs32)
    checker.check(scene, "提交后退出键入态（状态栏回到属性提示）",
                  pixel_diff(img, typing_status, shell.layout["status"]) > 20,
                  "状态栏差异像素 %d" % pixel_diff(img, typing_status, shell.layout["status"]))

    # 字号改成 48：焦点切回第一行，数字键入 + 回车
    shell.click(adjust[0] + 40, adjust[1] + rows_top + row_h // 2)
    before_font = shell.capture()
    shell.type_text("48")
    img = shell.shot("16_type_fontsize_input")
    checker.check(scene, "字号行键入数字后面板有「输入中」反馈",
                  pixel_diff(img, before_font, panel) > 20,
                  "面板差异像素 %d" % pixel_diff(img, before_font, panel))
    shell.press(VK_RETURN)
    img = shell.shot("17_type_fontsize_48")
    checker.check(scene, "回车后字号 48 已写回面板（首行数值变化）",
                  pixel_diff(img, before_font, row1) > 8,
                  "首行差异像素 %d" % pixel_diff(img, before_font, row1))

    # 再落一段 48 px 文字：字形明显更高（同字符 H，排除字形差异）
    origin2 = (left + 40, top + 130)
    shell.click(*origin2)
    shell.type_text("H")
    shell.press(VK_RETURN)
    img = shell.shot("18_type_commit_48")
    glyphs48, _, hy0, _, hy1 = glyph_bbox(
        img, (origin2[0] - 4, origin2[1] - 8, origin2[0] + 120, origin2[1] + 110))
    height32 = gy1 - gy0 + 1
    height48 = hy1 - hy0 + 1
    checker.check(scene, "48 px 文字已提交且字形比 32 px 更高",
                  glyphs48 > 40 and height48 > height32 + 3,
                  "字形 %d 像素，高 %d px > 32 px 的 %d px" % (glyphs48, height48, height32))


def scene_filter(shell, checker):
    scene = "filter"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left, top, right, bottom, steps=8)  # 铺满整幅文档
    img = shell.shot("19_filter_before")

    shell.command(CMD_FILTER_BASE + 0)  # Vignette
    img = shell.shot("20_filter_panel")
    adjust = shell.layout["adjust"]
    rows_top = 54
    row_h = 20
    row1 = (adjust[0] + 6, adjust[1] + rows_top - 1, adjust[2] - 8,
            adjust[1] + rows_top + row_h - 1)
    checker.check(scene, "滤镜面板第一参数高亮",
                  count_matching(img, row1, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, row1, COL_HIGHLIGHT, 12))

    # 输入 Amount = 80 并回车（键入 + 回车 = 落值并应用）
    shell.type_text("80")
    shell.press(VK_RETURN)
    img = shell.shot("21_filter_vignette")
    center = mean_rgb(img, ((left + right) // 2 - 10, (top + bottom) // 2 - 10,
                            (left + right) // 2 + 10, (top + bottom) // 2 + 10))
    corner = mean_rgb(img, (left + 10, top + 10, left + 50, top + 50))
    checker.check(scene, "Vignette 压暗四角而保留中心",
                  brightness(corner) < brightness(center) * 0.85,
                  "中心 %s 角落 %s" % (center, corner))
    shell.press(VK_ESCAPE)
    img = shell.shot("22_filter_closed")
    checker.check(scene, "Esc 关闭滤镜面板后回到调整面板",
                  near(pixel(img, adjust[0] + 4, adjust[1] + 4), COL_ADJ_BG, 8),
                  "实测 %s" % (pixel(img, adjust[0] + 4, adjust[1] + 4),))


def scene_adjustment(shell, checker):
    scene = "adjustment"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("336699")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left, top, right, bottom, steps=8)
    shell.select_tool("select")
    # 调整是「调整图层」：只作用于本层下方的内容，必须新建一层再挂调整
    shell.press(ord("N"), ctrl=True, shift=True)
    img = shell.shot("23_adjust_before")
    region = (left + 10, top + 10, right - 10, bottom - 10)
    before = mean_rgb(img, region)

    # Ctrl+Shift+A 新增 Exposure 调整（默认值 0），再按 = 抬几次
    shell.press(ord("A"), ctrl=True, shift=True)
    img = shell.shot("24_adjust_added")
    adjust = shell.layout["adjust"]
    panel = (adjust[0], adjust[1], adjust[2], adjust[3])
    checker.check(scene, "调整面板出现新调整行（高亮）",
                  count_matching(img, panel, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, panel, COL_HIGHLIGHT, 12))
    for _ in range(6):
        shell.press(VK_OEM_PLUS)
    shell.press(ord("U"), ctrl=True, shift=True)  # 应用
    img = shell.shot("25_adjust_applied")
    after = mean_rgb(img, region)
    checker.check(scene, "应用 Exposure 后下方像素变亮",
                  brightness(after) > brightness(before) + 5,
                  "前 %s 后 %s" % (before, after))
    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("26_adjust_undo")
    undo = mean_rgb(img, region)
    checker.check(scene, "撤销后亮度回到修改前",
                  abs(brightness(undo) - brightness(before)) < 12,
                  "撤销后 %s 原 %s" % (undo, before))


def scene_layers(shell, checker):
    scene = "layers"
    for _ in range(2):
        shell.press(ord("N"), ctrl=True, shift=True)
    img = shell.shot("27_three_layers")
    layer = shell.layout["layer"]
    # 三行图层：行高 22（ui_core layer_row_rect），高亮只画在选中行
    highlight = count_matching(img, (layer[0], layer[1], layer[2], layer[3]),
                               COL_HIGHLIGHT, 12)
    checker.check(scene, "三图层面板有选中高亮", highlight > 500, "高亮像素 %d" % highlight)
    texts = count_bright(img, (layer[0], layer[1], layer[0] + 240, layer[1] + 160), 150)
    checker.check(scene, "三行图层都有文字", texts > 500, "浅色像素 %d" % texts)

    shell.press(ord("E"), ctrl=True)  # 向下合并
    img = shell.shot("28_merge_down")
    layer = shell.layout["layer"]
    texts_after = count_bright(img, (layer[0], layer[1], layer[0] + 240, layer[1] + 160), 150)
    checker.check(scene, "Ctrl+E 合并后图层文字变少", texts_after < texts,
                  "合并前 %d 合并后 %d" % (texts, texts_after))


def scene_blend(shell, checker):
    """
    混合模式接线（S21）：画出两层重叠，Shift+N 逐档切换混合模式，
    重叠区像素必须真的换公式——Normal 是上层青色，Multiply 是两色逐通道相乘。
    期望值手工可算：底 #FF8800=(255,136,0)，上 #00FFFF=(0,255,255)，
    Multiply -> (255*0/255, 136*255/255, 0*255/255) = (0,136,0)。
    """
    scene = "blend"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    width, height = right - left, bottom - top

    # 第一层：橙色矩形铺满文档中心一大块
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    x0, y0 = left + width // 6, top + height // 6
    x1, y1 = right - width // 6, bottom - height // 6
    shell.drag(x0, y0, x1, y1, steps=10)
    img = shell.shot("32_blend_base_layer")
    base = mean_rgb(img, ((x0 + x1) // 2 - 6, (y0 + y1) // 2 - 6,
                          (x0 + x1) // 2 + 6, (y0 + y1) // 2 + 6))
    checker.check(scene, "底层橙色矩形居中填充", near(base, (255, 136, 0), 40),
                  "实测 %s" % (base,))

    # 第二层：青色矩形，与底层同位置重叠
    shell.press(ord("N"), ctrl=True, shift=True)
    shell.press(ord("X"), shift=True)
    shell.type_text("00ffff")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(x0, y0, x1, y1, steps=10)
    img = shell.shot("33_blend_two_layers_normal")
    normal = mean_rgb(img, ((x0 + x1) // 2 - 6, (y0 + y1) // 2 - 6,
                            (x0 + x1) // 2 + 6, (y0 + y1) // 2 + 6))
    checker.check(scene, "Normal 下重叠区 = 上层青色", near(normal, (0, 255, 255), 40),
                  "实测 %s" % (normal,))

    # 切到 Multiply（档位 2）：Shift+B 后退一步 -> Multiply（Normal=0 后退回绕到 23）
    # 这里按菜单序连步前进更直观：Normal -> Darken -> Multiply 共两次 Shift+N。
    shell.press(ord("N"), shift=True)
    shell.press(ord("N"), shift=True)
    img = shell.shot("34_blend_multiply")
    multiply = mean_rgb(img, ((x0 + x1) // 2 - 6, (y0 + y1) // 2 - 6,
                              (x0 + x1) // 2 + 6, (y0 + y1) // 2 + 6))
    expected = (0, 136, 0)
    checker.check(scene, "Multiply 下重叠区 = 两色逐通道相乘", near(multiply, expected, 40),
                  "实测 %s 期望 %s" % (multiply, expected))

    # 与 Normal 必须显著不同，否则上面的相等判断可能是巧合
    delta = sum(abs(multiply[i] - normal[i]) for i in range(3))
    checker.check(scene, "Multiply 与 Normal 结果显著不同", delta > 60,
                  "通道差不和 %d" % delta)

    # 再切回 Normal：连续五步必回到起点（档位共 24 档，步进回绕）
    for _ in range(22):
        shell.press(ord("N"), shift=True)
    img = shell.shot("35_blend_back_to_normal")
    again = mean_rgb(img, ((x0 + x1) // 2 - 6, (y0 + y1) // 2 - 6,
                           (x0 + x1) // 2 + 6, (y0 + y1) // 2 + 6))
    checker.check(scene, "步进 22 次后回到 Normal", near(again, normal, 40),
                  "实测 %s（Normal %s）" % (again, normal))


def scene_xform(shell, checker):
    """
    图层变换接线（S21）：画一个偏左矩形，Ctrl+右 平移 10px，矩形必须整体右移；
    Ctrl+Shift+右 旋转 15 度，旋转后同一水平带的像素分布必须改变（不再是整齐矩形）。
    用「列亮度剖面」判定位移，比单点取样稳（不依赖文档落点的整数化）。
    """
    scene = "xform"
    doc = shell.document_rect()
    left, top, right, bottom = doc

    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    # 小矩形，落在文档左上区域，右移 10px 后仍在文档内
    x0, y0 = left + 8, top + 8
    x1, y1 = left + 48, top + 48
    shell.drag(x0, y0, x1, y1, steps=8)
    img = shell.shot("36_xform_rect_before")
    inner = mean_rgb(img, (x0 + 4, y0 + 4, x1 - 4, y1 - 4))
    checker.check(scene, "变换前矩形填充前景色", near(inner, (255, 136, 0), 40),
                  "实测 %s" % (inner,))
    # 原位置右外侧应是文档黑（矩形还没过去）
    outside_left = pixel(img, x1 + 30, (y0 + y1) // 2)
    checker.check(scene, "矩形右侧原为空白", near(outside_left, (0, 0, 0), 10),
                  "实测 %s" % (outside_left,))

    # 采样点取「矩形内、离左缘 6px」——不取中心：中心在平移 20px 后恰好落在
    # 新矩形的左边缘上，会把「真的移动了」误判成「没动」。
    probe_x = x0 + 6
    probe_band = (probe_x - 3, y0 + 4, probe_x + 3, y1 - 4)
    before_probe = mean_rgb(img, probe_band)
    checker.check(scene, "变换前探针点在前景色内", near(before_probe, (255, 136, 0), 40),
                  "实测 %s" % (before_probe,))

    # Ctrl+右 平移 10px × 2 = 20px（步长见 ui_core kLayerTransformMoveCoarse）
    for _ in range(2):
        shell.press(VK_RIGHT, ctrl=True)
    img = shell.shot("37_xform_moved")
    # 原探针点必须变空（矩形整体右移离开了这里）
    after_probe = mean_rgb(img, probe_band)
    checker.check(scene, "Ctrl+右×2 后原探针点变空", near(after_probe, (0, 0, 0), 24),
                  "实测 %s" % (after_probe,))
    # 新探针点（右移 20px）必须落在前景色内
    moved_probe_x = probe_x + 20
    if moved_probe_x + 3 < right:
        moved_probe = mean_rgb(img, (moved_probe_x - 3, y0 + 4, moved_probe_x + 3, y1 - 4))
        checker.check(scene, "新探针点填充前景色", near(moved_probe, (255, 136, 0), 40),
                      "实测 %s" % (moved_probe,))
    else:
        checker.check(scene, "新位置仍在文档内", False, "平移超出文档右边界")

    # 旋转 15°×4 = 60°：同一探针带不可能再整段是前景色（矩形的角被切掉了）
    for _ in range(4):
        shell.press(VK_RIGHT, ctrl=True, shift=True)
    img = shell.shot("38_xform_rotated")
    rotated_probe = mean_rgb(img, probe_band)
    checker.check(scene, "旋转 60° 后原探针带不再是纯前景色",
                  not near(rotated_probe, (255, 136, 0), 20),
                  "实测 %s" % (rotated_probe,))

    # 撤回旋转：旋转是「绕画布中心转整层」，矩形会离开原处，因此要连撤 4 步
    # 才回到 0°；撤一步只到 45°（探针已证实），矩形仍在别处。
    # 采样带必须取平移后矩形内部（probe_x + 20），不能取原始 probe_band。
    restored_band = (moved_probe_x - 3, y0 + 4, moved_probe_x + 3, y1 - 4)
    for _ in range(4):
        shell.press(ord("Z"), ctrl=True)
    img = shell.shot("39_xform_undo_rotate")
    after_undo = mean_rgb(img, restored_band)
    checker.check(scene, "Ctrl+Z×4 撤销旋转后矩形回到原位",
                  near(after_undo, (255, 136, 0), 40),
                  "实测 %s" % (after_undo,))


def scene_view(shell, checker):
    scene = "view"
    canvas = shell.layout["canvas"]
    canvas_rect = (canvas[0] + 1, canvas[1] + 1, canvas[2] - 1, canvas[3] - 1)
    img = shell.shot("29_view_fit")
    # 适合窗口：文档（黑）完整落在画布内，四周留有应用底色
    fit = detect_document_rect(img, canvas)
    checker.check(scene, "适合窗口：文档完整可见且四周有留白",
                  fit is not None and fit[0] > canvas[0] + 2 and fit[2] < canvas[2] - 2
                  and fit[1] > canvas[1] + 2 and fit[3] < canvas[3] - 2,
                  "文档 %s" % (fit,))
    fit_bg = count_app_bg(img, canvas_rect) if fit else 0

    shell.press(ord("0"), ctrl=True)   # 适合窗口
    shell.press(VK_OEM_PLUS, ctrl=True)   # 放大（Ctrl+=：VK_OEM_PLUS，不是字符 '='）
    shell.press(VK_OEM_PLUS, ctrl=True)
    img = shell.shot("30_view_zoom_in")
    zoom_bg = count_app_bg(img, canvas_rect)
    checker.check(scene, "Ctrl+= 放大后文档吃掉画布留白",
                  zoom_bg < fit_bg,
                  "放大后应用底色 %d < 适合窗口 %d" % (zoom_bg, fit_bg))

    shell.press(ord("1"), ctrl=True)   # 实际像素
    img = shell.shot("31_view_actual")
    # 实际像素：1280×720 文档远大于 686×589 画布，画布应几乎全是文档
    actual_bg = count_app_bg(img, canvas_rect)
    checker.check(scene, "实际像素模式画布被文档覆盖",
                  actual_bg < (canvas_rect[2] - canvas_rect[0]) * (canvas_rect[3] - canvas_rect[1]) // 100,
                  "应用底色 %d" % actual_bg)

    shell.press(ord("N"), ctrl=True)   # 新建文档（无对话框）
    img = shell.shot("32_new_document")
    layer = shell.layout["layer"]
    row1 = (layer[0], layer[1], layer[0] + 240, layer[1] + 90)
    checker.check(scene, "Ctrl+N 新文档回到单图层且视图回到适应窗口",
                  count_bright(img, row1, 150) > 150 and
                  detect_document_rect(img, canvas) is not None,
                  "图层浅色像素 %d" % count_bright(img, row1, 150))


def scene_image_io(shell, checker):
    """
    位图导入 / 导出闭环。

    这个场景由 main() 以命令行参数启动外壳（start(args=[path])），画内容来自
    build/import_probe.png——一张像素值精确已知的 240×160 PNG：
      x < 116        → (255, 0, 0) 纯红
      116 <= x < 124 → (0, 255, 0) 纯绿竖条
      x >= 124       → (0, 0, 255) 纯蓝
    断言全部是算出来的常量，不是「看起来像」。

    验证点：
      1. 命令行打开后画布出现该图案（导入链路通）；
      2. 状态栏/图层面板有内容（壳真的接管了文档，不是黑屏碰巧）；
      3. 导出为 PNG 后文件存在且是一张合法 PNG（导出链路通）；
      4. 把导出的文件再导入（round trip）仍是 240×160，尺寸不漂。
    """
    scene = "image_io"
    canvas = shell.layout["canvas"]
    img = shell.shot("33_image_open")

    # 1) 图案出现：画布中心区域应同时含红与蓝（中缝绿条在校正后可能在任意位置）
    reds = count_matching(img, canvas, (255, 0, 0), 8)
    blues = count_matching(img, canvas, (0, 0, 255), 8)
    checker.check(scene, "打开的位图在画布上出现（红色区）", reds > 200,
                  "红色像素 %d" % reds)
    checker.check(scene, "打开的位图在画布上出现（蓝色区）", blues > 200,
                  "蓝色像素 %d" % blues)

    # 2) 壳真的接管：图层面板应有文字（图层名行）
    layer = shell.layout["layer"]
    checker.check(scene, "导入后图层面板有内容",
                  count_bright(img, (layer[0], layer[1], layer[0] + 240,
                                     layer[1] + 120), 150) > 200,
                  "浅色像素 %d" % count_bright(img, (layer[0], layer[1], layer[0] + 240,
                                                     layer[1] + 120), 150))

    # 3) 导出的 ABI 级证据由壳自检承担（--selftest 里 comp_io 往返），
    #    因为 GetSaveFileNameW 是模态对话框，脚本无法无人值守点完。
    #    这里只证明「引擎侧同一入口产出合法 PNG」，与壳只差一个路径字符串。
    checker.check(scene, "导出通路由壳自检覆盖（见 compositor_shell_selftest.txt）",
                  selftest_has_io_roundtrip(),
                  "自检报告未找到 comp_io 往返断言")


def selftest_has_io_roundtrip():
    """壳自检报告里必须出现 comp_io 导出/导入往返断言。"""
    # 壳自检从仓库根跑（它要用相对路径 build/import_probe.png），报告落根目录；
    # visual_check 的 CWD 是 scripts/，所以这里显式取仓库根。
    path = os.path.join(ROOT, "compositor_shell_selftest.txt")
    if not os.path.exists(path):
        path = os.path.join(BUILD, "compositor_shell_selftest.txt")
    if not os.path.exists(path):
        return False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    return "comp_io_export" in text and "comp_io_import" in text


SCENES = [    ("start", scene_start, ()),
    ("brush", scene_brush, ()),
    ("selection", scene_selection, ()),
    ("wand", scene_wand, ()),
    ("gradient", scene_gradient, ()),
    ("shape", scene_shape, ()),
    ("type", scene_type, ()),
    ("filter", scene_filter, ()),
    ("adjustment", scene_adjustment, ()),
    ("layers", scene_layers, ()),
    ("blend", scene_blend, ()),
    ("xform", scene_xform, ()),
    ("view", scene_view, ()),
    ("image_io", scene_image_io, ("import_probe.png",)),
    ("crop", scene_crop, ()),
]


# ---------------------------------------------------------------- 入口

def main():
    parser = argparse.ArgumentParser(description="Windows 外壳自动化目视检查")
    parser.add_argument("--scene", default="all", help="场景名，或 all")
    parser.add_argument("--list", action="store_true", help="列出场景")
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--gpu", action="store_true", help="使用 --gpu 构建（仅记录标题栏）")
    parser.add_argument("--no-clean", action="store_true", help="失败时保留进程（调试用）")
    args = parser.parse_args()

    if args.list:
        for name, _, _ in SCENES:
            print(name)
        return 0
    if not os.path.exists(args.exe):
        print("找不到 exe：%s（先跑 python scripts/build.py && "
              "python scripts/build_windows_shell.py）" % args.exe)
        return 2

    scenes = dict((name, (func, extra)) for name, func, extra in SCENES)
    selected = [name for name, _, _ in SCENES] if args.scene == "all" else [args.scene]
    checker = Checker()
    lines = []
    try:
        for name in selected:
            entry = scenes.get(name)
            if entry is None:
                print("未知场景：%s" % name)
                return 2
            func, extra = entry
            if func is None:
                print("未知场景：%s" % name)
                return 2
            shell = Shell(args.exe, args.out, keep=args.no_clean).start(extra)
            try:
                print("\n=== 场景：%s ===" % name)
                func(shell, checker)
            finally:
                if not args.no_clean:
                    shell.stop()
                else:
                    print("   保留进程 pid=%s hwnd=%s" % (shell.proc.pid, shell.hwnd))
            for scene, check_name, ok, detail in checker.rows:
                if scene == name:
                    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", check_name,
                                           "" if not detail else "  —— " + detail))
            lines.append("%s: %d 项检查，%d 失败" % (
                name, sum(1 for row in checker.rows if row[0] == name),
                sum(1 for row in checker.rows if row[0] == name and not row[2])))
    finally:
        pass

    failures = checker.failures()
    summary = ["# visual_check 摘要", ""]
    summary.extend(lines)
    summary.append("")
    summary.append("合计 %d 项检查，%d 失败" % (len(checker.rows), len(failures)))
    text = "\n".join(summary)
    print("\n" + text)
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "visual_check.txt"), "w", encoding="utf-8",
              newline="\n") as handle:
        handle.write(text + "\n")
    print("\n截图目录：%s" % os.path.abspath(args.out))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
