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
WM_CHAR = 0x0102
WM_IME_CHAR = 0x0286
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
CMD_ADJ_KIND_PREV = 1014
CMD_ADJ_ADD = 1022
CMD_ADJ_APPLY = 1023
CMD_ADJ_VALUE_UP = 1021
# 面板动作号落在 1220 段：滤镜序号涨到 15 个（1200..1214）后，1210 已经是
# Sharpen、1211..1214 是 High Pass / Minimum / Maximum / Offset 的菜单项，旧的应用号
# 会和菜单项抢同一个 id（见 shell_state.h）。面板动作号 1220..1222。
CMD_FILTER_APPLY = 1220
# 图像 > 调整：kCmdImageAdjustBase(1590) + kind。kind 12..24 是本仓追加的破坏性调整
# （亮度对比、去色、阈值、色调分离、自然饱和度、可选颜色、照片滤镜、通道混合器、
# 颜色查找、阴影高光、自动三档），0..11 里只有逐像素可做的那几项才发号
# （见 shells/windows/src/shell_state.h::kImageAdjustKinds）。
CMD_IMAGE_ADJUST_BASE = 1590
# 图层 > 效果：kCmdEffectBase(1510) + kind。kind 9 = Pattern Overlay（P1-1 第二段）。
CMD_EFFECT_BASE = 1510
CMD_NEW_DOCUMENT_DIALOG = 1560
CMD_SAVE_AS = 1561
CMD_IMAGE_SIZE = 1562
CMD_CANVAS_SIZE = 1563
CMD_HISTORY_SETTINGS = 1564

TOOL = {
    "select": 0, "move": 1, "marquee": 2, "lasso": 3, "pen": 4, "wand": 5,
    "crop": 6, "eyedropper": 7, "brush": 8, "eraser": 9, "heal": 10,
    "clone": 11, "retouch": 12, "tone": 13, "gradient": 14, "shape": 15,
    "type": 16, "hand": 17, "zoom": 18, "history": 19,
}

# ui_core.h 的布局常量（客户区坐标）
TOOLBAR_H = 46
STATUS_H = 26
LAYER_W = 248
ADJUST_W = 330
MIN_CANVAS_W = 180
# §5.49 工具选项栏：高度 + 收起阈值（与 Layout::kOptionsHeight / kMinBodyHeight 同值）。
OPTIONS_H = 32
MIN_BODY_H = 96
OPTION_LEFT_PAD = 12
OPTION_GROUP_W = 150
OPTION_GROUP_GAP = 8
OPTION_LABEL_W = 56
OPTION_ARROW_W = 18
OPTION_VALUE_W = 54
OPTION_CONTROL_H = 22

# ui_core.h 的配色（断言用真值）
COL_TOOLBAR_BG = (44, 46, 52)
COL_TOOL_BTN = (58, 60, 68)
COL_TOOL_SELECTED = (70, 104, 168)
COL_LAYER_BG = (24, 26, 30)
COL_ADJ_BG = (22, 24, 28)
COL_STATUS_BG = (26, 28, 32)
COL_OPTIONS_BG = (38, 40, 46)
COL_OPTION_CTRL = (58, 60, 68)
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
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
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


def median_luma(img, rect):
    """区域亮度的中位数（「画布直方图右移」用它比均值更抗个别像素）。"""
    region = img.crop(rect).convert("L")
    hist = region.histogram()
    total = sum(hist)
    if total == 0:
        return 0
    running = 0
    for value, count in enumerate(hist):
        running += count
        if running * 2 >= total:
            return value
    return 255


def region_std(img, rect):
    """区域亮度的标准差（锐化 / 降噪的可观察口径：同一块噪声场的方差）。"""
    hist = img.crop(rect).convert("L").histogram()
    total = sum(hist)
    if total == 0:
        return 0.0
    mean = sum(value * count for value, count in enumerate(hist)) / float(total)
    variance = sum((value - mean) ** 2 * count for value, count in enumerate(hist)) / float(total)
    return variance ** 0.5


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
        self._park_pointer()
        return self

    def _park_pointer(self):
        """把**物理指针**停到状态栏，让它别落在画布上。

        笔尖轮廓（`shell_paint.cpp` 的 dotted ellipse）画在最后一次 WM_MOUSEMOVE 的
        坐标上；本脚本靠 PostMessage 合成鼠标事件，真实指针并不会跟着走。真实指针
        若恰好停在画布内，窗口激活/悬停时系统会补一条真实 MOUSEMOVE，把合成拖动的
        坐标顶掉——截图里就凭空多出一个笔尖圈。2026-10-23 全量跑时，`options_bar`
        的「点击不穿透到画布」就因为这个多出 17 px 而误报（同一场景单跑必过）。
        状态栏既在画布外、又没有悬停高亮，停这里不会引入任何新的像素。
        """
        target = POINT(self.client_w // 2, max(0, self.client_h - 8))
        if user32.ClientToScreen(self.hwnd, ctypes.byref(target)):
            user32.SetCursorPos(target.x, target.y)
            self.wait(0.15)  # 等系统把真实 MOUSEMOVE/MOUSELEAVE 送完

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
                self.proc.wait(timeout=1.2)
            except subprocess.TimeoutExpired:
                # 脏文档会先弹右栏的「关闭提示」（P0-3），按 D = 不保存直接关闭。
                # 干净文档在 1.2 秒内就已经退出，不会走到这里；真的连 D 都不理
                # （面板都没起来）才 kill，不让超时把整轮目视拖垮。
                if self.hwnd:
                    self.press(ord("D"))
                try:
                    self.proc.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
        self.hwnd = None
        self.proc = None

    def focus(self):
        """把窗口切到前台并返回是否成功（失败会导致 SetCapture 立刻丢失）。

        前台锁是会话状态：当前台窗口恰好是「无前台/锁屏/第三方抢前台」时，
        单次 SetForegroundWindow 会被拒。这里做两件事加强：
        1) SystemParametersInfo 清掉前台锁定超时（SPI_SETFOREGROUNDLOCKTIMEOUT=0）；
        2) 最多 5 轮重试，每轮失败后按一次 Alt 键重置前台锁计数（Win32 既定技巧）。
        """
        SPI_SETFOREGROUNDLOCKTIMEOUT = 0x2001
        user32.SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, None, 0)
        current = kernel32.GetCurrentThreadId()
        target = user32.GetWindowThreadProcessId(self.hwnd, None)
        for _ in range(5):
            fg = user32.GetForegroundWindow()
            fg_thread = user32.GetWindowThreadProcessId(fg, None) if fg else 0
            if fg_thread:
                user32.AttachThreadInput(current, fg_thread, True)
            user32.AttachThreadInput(current, target, True)
            user32.BringWindowToTop(self.hwnd)
            user32.SetForegroundWindow(self.hwnd)
            user32.SetActiveWindow(self.hwnd)
            if fg_thread:
                user32.AttachThreadInput(current, fg_thread, False)
            user32.AttachThreadInput(current, target, False)
            if user32.GetForegroundWindow() != self.hwnd:
                # Alt 键按下再抬起会清掉前台锁定状态，下一轮重试
                user32.keybd_event(0x12, 0, 0, 0)   # VK_MENU down
                user32.keybd_event(0x12, 0, 2, 0)   # VK_MENU up
                self.wait(0.25)
            else:
                self.wait(0.2)
                return True
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
        # §5.49 选项栏：与 ui_core compute_layout 同一阈值，收起时高度为 0。
        remainder = h - toolbar_h - status_h
        options_h = OPTIONS_H if remainder >= OPTIONS_H + MIN_BODY_H else 0
        body_top = toolbar_h + options_h
        body_h = remainder - options_h
        layer_w = LAYER_W if w >= LAYER_W + MIN_CANVAS_W else 0
        adjust_w = ADJUST_W if w - layer_w >= ADJUST_W + MIN_CANVAS_W else 0
        return {
            "toolbar": (0, 0, w, toolbar_h),
            "options": (0, toolbar_h, w, toolbar_h + options_h),
            "layer": (0, body_top, layer_w, body_top + body_h),
            "adjust": (w - adjust_w, body_top, w, body_top + body_h),
            "canvas": (layer_w, body_top, w - adjust_w, body_top + body_h),
            "status": (0, body_top + body_h, w, body_top + body_h + status_h),
        }

    def option_group_rect(self, index):
        """选项栏第 index 组的矩形（与 ui_core option_group_rect 同算式）。"""
        left, top, right, bottom = self.layout["options"]
        if bottom - top <= 0:
            return None
        x = left + OPTION_LEFT_PAD + index * (OPTION_GROUP_W + OPTION_GROUP_GAP)
        if x + OPTION_GROUP_W > right:
            return None
        return (x, top, x + OPTION_GROUP_W, bottom)

    def option_control_rect(self, index):
        """选项栏第 index 组的「- 值 +」控件盒。"""
        group = self.option_group_rect(index)
        if group is None:
            return None
        left, top, right, bottom = group
        height = min(OPTION_CONTROL_H, bottom - top - 10)
        if height <= 0:
            return None
        y = top + (bottom - top - height) // 2
        return (left + OPTION_LABEL_W, y, left + OPTION_LABEL_W + OPTION_ARROW_W * 2 +
                OPTION_VALUE_W, y + height)

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

    def document_rect(self, refresh=False):
        """文档（黑色空画布）在客户区里的矩形。

        refresh=True 时丢弃缓存重新扫描。裁剪提交 / 视图适配会让文档在客户区里
        重排，缓存里仍是旧矩形——crop 场景提交后必须 refresh，否则永远量到
        旧画布（文档内容要有黑边留白，detect_document_rect 靠黑色外接框定位）。
        """
        if refresh or getattr(self, "_doc_rect", None) is None:
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

    def post_char(self, code):
        """WM_CHAR 直接送一个 UTF-16 码元：键盘字符与输入法提交字符的同一出口。"""
        user32.PostMessageW(self.hwnd, WM_CHAR, code, 0)
        self.wait(0.12)

    def send_ime_char(self, code):
        """按真实 IME 的方式投 WM_IME_CHAR；壳未拦截时 DefWindowProc 翻成 WM_CHAR。"""
        user32.SendMessageW(self.hwnd, WM_IME_CHAR, code, 0)
        self.wait(0.12)

    def click(self, x, y):
        x, y = int(x), int(y)
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, make_lparam(x, y))
        user32.PostMessageW(self.hwnd, WM_LBUTTONDOWN, 1, make_lparam(x, y))
        user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, make_lparam(x, y))
        self.wait(0.25)

    def modified_click(self, x, y, ctrl=False, shift=False):
        """带修饰键的单击（面板多选）。

        与 press() 同一套机关：修饰状态写进目标线程的输入状态，并且在消息被
        处理完之前不能恢复——否则 GetKeyState 读到的已是干净状态，Ctrl+点
        会被当成普通点，多选永远选不上第二个。
        """
        x, y = int(x), int(y)
        current, target, attached, state, saved = self._set_modifiers(shift, ctrl, False)
        try:
            user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, make_lparam(x, y))
            user32.PostMessageW(self.hwnd, WM_LBUTTONDOWN, 1, make_lparam(x, y))
            self.wait(0.05)
            user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, make_lparam(x, y))
            self.wait(0.2)
        finally:
            self._restore_modifiers(current, target, attached, state, saved)
        self.wait(0.1)

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

    引擎语义（engine/domain/crop.cpp 的 crop_apply）：裁剪框 是**保留区**——
    画布换成 rect 尺寸、图层整体平移 -rect.xy、框外内容丢弃。所以本场景：

      1. 框内画橙色块、框外（右上角）画蓝色块；
      2. Enter 提交后重新「适合窗口」再量几何（文档矩形必须 refresh，
         缓存里是旧画布；提交后视图重排，旧的屏幕坐标也不再对应同一文档像素）：
         - 文档宽高比 == 裁剪框比例（画布真的换了尺寸）；
         - 橙色仍在（保留内容存活，方向没搞反）；
         - 蓝色为 0（框外内容确实被裁掉）；
      3. Esc 取消不改变画布尺寸（两次都在同一「适合窗口」视图下量）。
    """
    scene = "crop"
    shell.press(ord("0"), ctrl=True)      # 适合窗口：先统一视图再量几何
    shell.wait(0.3)
    doc = shell.document_rect()
    left, top, right, bottom = doc
    width, height = right - left, bottom - top

    # 裁剪框：左下角四分之一块（与右上角的蓝色不出边、不重叠）
    box_x0, box_y0 = left + 20, top + height // 2
    box_x1, box_y1 = left + width // 3, bottom - 20
    box_w, box_h = box_x1 - box_x0, box_y1 - box_y0
    box_aspect = float(box_w) / float(box_h)

    # 1) 框内画橙色（内缩 15px，给 detect_document_rect 留黑色外接边）
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    orange = (box_x0 + 15, box_y0 + 15, box_x1 - 15, box_y1 - 15)
    shell.drag(orange[0], orange[1], orange[2], orange[3], steps=8)
    # 框外画蓝色（右上角，与裁剪框完全不相交）
    shell.press(ord("X"), shift=True)
    shell.type_text("0000ff")
    shell.press(VK_RETURN)
    blue = (left + 3 * width // 4, top + 12, right - 12, top + height // 4)
    shell.drag(blue[0], blue[1], blue[2], blue[3], steps=8)
    img = shell.shot("34_crop_before")
    orange_center = ((orange[0] + orange[2]) // 2, (orange[1] + orange[3]) // 2)
    before_orange = pixel(img, *orange_center)
    before_blue = count_matching(img, (blue[0] + 4, blue[1] + 4,
                                       blue[2] - 4, blue[3] - 4), (0, 0, 255), 16)
    checker.check(scene, "裁剪前：框内已画橙色保留块",
                  near(before_orange, (255, 136, 0), 30),
                  "实测 %s" % (before_orange,))
    checker.check(scene, "裁剪前：框外已画蓝色待裁块",
                  before_blue > 200,
                  "蓝色像素 %d" % (before_blue,))

    # 2) 切 Crop，拖出同一个框，Enter 提交
    shell.select_tool("crop")
    shell.drag(box_x0, box_y0, box_x1, box_y1, steps=8)
    img = shell.shot("35_crop_frame")
    checker.check(scene, "裁剪框可见（框内区域未被遮罩糊掉）",
                  brightness(mean_rgb(img, (box_x0 + 4, box_y0 + 4,
                                            box_x1 - 4, box_y0 + 40))) < 200,
                  "框内区域平均 %s" % (mean_rgb(img, (box_x0 + 4, box_y0 + 4,
                                                     box_x1 - 4, box_y0 + 40)),))
    shell.press(VK_RETURN)
    shell.wait(0.4)
    shell.press(ord("0"), ctrl=True)      # 重新适合窗口：统一视图再量新画布
    shell.wait(0.5)
    img = shell.shot("36_crop_after")

    # 3) 提交后：新文档矩形必须重测（缓存里是旧画布）
    doc_after = shell.document_rect(refresh=True)
    al, at, ar, ab = doc_after
    after_w, after_h = ar - al, ab - at
    after_aspect = float(after_w) / float(after_h)
    aspect_tol = max(0.03, box_aspect * 0.03)
    checker.check(scene, "提交后画布换成裁剪框尺寸（宽高比一致）",
                  abs(after_aspect - box_aspect) <= aspect_tol,
                  "裁剪框 %dx%d 比 %.3f，提交后文档 %dx%d 比 %.3f"
                  % (box_w, box_h, box_aspect, after_w, after_h, after_aspect))

    after_orange = count_matching(img, (al + 2, at + 2, ar - 2, ab - 2),
                                  (255, 136, 0), 32)
    after_blue = count_matching(img, (al + 2, at + 2, ar - 2, ab - 2),
                                (0, 0, 255), 32)
    checker.check(scene, "框内橙色内容保留（方向没搞反）",
                  after_orange > 500,
                  "文档内橙色像素 %d" % (after_orange,))
    checker.check(scene, "框外蓝色内容被裁掉",
                  after_blue == 0,
                  "文档内蓝色像素 %d" % (after_blue,))

    # 4) Esc 取消也不改画布：再拖一个框，按 Esc，尺寸应保持不变
    size_before_esc = (after_w, after_h)
    shell.select_tool("crop")
    shell.drag(al + 20, at + 20, al + after_w // 3, at + after_h // 3, steps=8)
    shell.press(VK_ESCAPE)
    shell.wait(0.3)
    shell.press(ord("0"), ctrl=True)
    shell.wait(0.4)
    doc_esc = shell.document_rect(refresh=True)
    size_after_esc = (doc_esc[2] - doc_esc[0], doc_esc[3] - doc_esc[1])
    checker.check(scene, "Esc 取消不改变画布尺寸",
                  size_after_esc == size_before_esc,
                  "取消前 %s，取消后 %s" % (size_before_esc, size_after_esc))


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
                  glyphs32 > 15, "字形像素 %d" % glyphs32)
    # 容差说明：glyph_bbox 只数前景色 ±60 以内的像素；「适合窗口」视图把文档
    # 缩到 ~49%，32 px 字形降采样后大量像素是抗锯齿混合色（如 (127,68,0)）
    # 落在容差外，正确渲染的单字只剩 ~29 个全饱和核心。阈值取 15 仍能区分
    # 「没画上」（0）与「画上了」；48 px 那条字号大、核心多，保持 > 40。
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


def scene_ime(shell, checker):
    """输入法路径的文字工具验收（M43 的自动化预检）。

    壳没有 WM_IME_CHAR 分支：IME 提交的字符经 DefWindowProc 翻成 WM_CHAR，与
    键盘字符共用 type_char_input 一个出口。所以 M43 的「上屏不双字」「退格整字
    删」都能在消息层如实复现——WM_IME_CHAR 送一次，缓冲里只能多一个码点；
    退格一次，UTF-8 缓冲按码点整删（type_utf8_erase_last）。
    """
    scene = "ime"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("type")
    adjust = shell.layout["adjust"]
    rows_top = 54
    row_h = 20
    origin = (left + 40, top + 40)
    box = (origin[0] - 6, origin[1] - 10, origin[0] + 110, origin[1] + 70)

    # 1) WM_CHAR 送「你」：字体轨道渲染中文的底线（没加载字体就是 0 像素）
    shell.click(*origin)
    shell.post_char(0x4F60)
    shell.press(VK_RETURN)
    img = shell.shot("70_ime_char_commit")
    base, _, _, _, _ = glyph_bbox(img, box)
    checker.check(scene, "输入法字符经 WM_CHAR 上屏（中文字形落画布）",
                  base > 10, "字形像素 %d" % base)
    # 一次提交 = 恰好一步撤销，Ctrl+Z 后画布必须完全复原
    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("71_ime_undo_clean")
    cleared, _, _, _, _ = glyph_bbox(img, box)
    checker.check(scene, "一步撤销后画布无中文残留",
                  base > 10 and cleared < 5,
                  "提交 %d 像素，撤销后 %d 像素" % (base, cleared))

    # 2) WM_IME_CHAR 送同一个字：DefWindowProc 只能翻出一次 WM_CHAR（不双字）
    shell.click(*origin)
    shell.send_ime_char(0x4F60)
    shell.press(VK_RETURN)
    img = shell.shot("72_ime_imechar_commit")
    ime, _, _, _, _ = glyph_bbox(img, box)
    ratio = ime / base if base else 0.0
    checker.check(scene, "WM_IME_CHAR 上屏不双字（与 WM_CHAR 像素量一致）",
                  base > 10 and 0.7 <= ratio <= 1.4,
                  "WM_CHAR %d，WM_IME_CHAR %d，比值 %.2f" % (base, ime, ratio))
    shell.press(ord("Z"), ctrl=True)

    # 3) 两个汉字 + 一次退格 = 剩一个字（退格按码点整删，不是半个字节）
    shell.click(*origin)
    shell.post_char(0x4F60)
    shell.post_char(0x597D)
    shell.press(VK_BACK)
    shell.press(VK_RETURN)
    img = shell.shot("73_ime_backspace_one_glyph")
    after_bs, _, _, _, _ = glyph_bbox(img, box)
    bs_ratio = after_bs / base if base else 0.0
    checker.check(scene, "退格一次只删一个整字（提交后像素量与单字一致）",
                  base > 10 and 0.7 <= bs_ratio <= 1.4,
                  "单字 %d，退格后 %d，比值 %.2f" % (base, after_bs, bs_ratio))
    shell.press(ord("Z"), ctrl=True)

    # 4) M43 预检：48px「你好，世界」整段上屏，再一步撤销完全复原
    shell.click(adjust[0] + 40, adjust[1] + rows_top + row_h // 2)
    shell.type_text("48")
    shell.press(VK_RETURN)
    shell.click(*origin)
    for ch in "你好，世界":
        shell.post_char(ord(ch))
    shell.press(VK_RETURN)
    img = shell.shot("74_ime_cjk_48")
    wide = (origin[0] - 6, origin[1] - 12, origin[0] + 260, origin[1] + 90)
    glyphs, gx0, gy0, gx1, gy1 = glyph_bbox(img, wide)
    checker.check(scene, "48px「你好，世界」整段上屏（M43 预检）",
                  glyphs > 60,
                  "字形像素 %d，外接框 %dx%d" % (glyphs, gx1 - gx0 + 1, gy1 - gy0 + 1))
    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("75_ime_cjk_48_undo")
    cleared, _, _, _, _ = glyph_bbox(img, wide)
    checker.check(scene, "48px 段落后撤销完全复原",
                  glyphs > 60 and cleared < 8,
                  "提交 %d 像素，撤销后 %d 像素" % (glyphs, cleared))


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


def scene_sharpen_denoise(shell, checker):
    """Sharpen(1209) / Reduce Noise(1210) 的面板入口与像素方向。

    这一场的核心不是「参数能不能存」，而是**命令号没被抢**：1210 以前是滤镜面板的
    「应用」，现在必须打开 Reduce Noise 面板。噪声场用 Add Noise 现铺，之后两个
    滤镜对同一块区域给出相反方向的方差变化，方向错了就说明走错了算子。
    """
    scene = "sharpen_denoise"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("8899aa")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left, top, right, bottom, steps=8)

    # 1) 先铺噪声：锐化/降噪都需要一块有方差的场才量得出方向。
    shell.command(CMD_FILTER_BASE + 5)  # Add Noise
    shell.type_text("40")  # 幅度小一点：太大就全被 0/255 夹住，方向量不出来
    shell.press(VK_RETURN)
    shell.press(VK_ESCAPE)
    shell.wait(0.8)
    img = shell.shot("90_sharpen_noise_before")
    probe = (left + 20, top + 20, left + 80, top + 80)
    noisy = region_std(img, probe)

    adjust = shell.layout["adjust"]
    row1 = (adjust[0] + 6, adjust[1] + 53, adjust[2] - 8, adjust[1] + 73)

    # 2) Reduce Noise 走 1210：这个 id 以前是滤镜面板的「应用」，被抢就看不出面板。
    shell.command(CMD_FILTER_BASE + 10)
    img = shell.shot("91_reduce_noise_panel")
    checker.check(scene, "Reduce Noise 面板打开且第一参数高亮",
                  count_matching(img, row1, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, row1, COL_HIGHLIGHT, 12))
    # Strength 默认 0 = 恒等，键入 10 后回车一次落值并应用（键入 + Tab 会丢缓冲）。
    # 用一次真应用而不是连按步进键：每一步都出一帧全分辨率预览，消息会堆在队列里，
    # 紧接着的截图就会拍到旧帧（本场景第一次跑就栽在这上面）。
    shell.type_text("10")
    shell.press(VK_RETURN)
    shell.press(VK_ESCAPE)
    shell.wait(0.8)
    img = shell.shot("92_reduce_noise_applied")
    denoised = region_std(img, probe)
    checker.check(scene, "Reduce Noise 压低局部方差（噪声被平滑）",
                  denoised < noisy * 0.8,
                  "σ %.1f -> %.1f" % (noisy, denoised))

    # 3) Sharpen 走 1209：面板能打开 + 第一参数高亮 = 命令号确实落到新滤镜上。
    shell.command(CMD_FILTER_BASE + 9)
    img = shell.shot("93_sharpen_panel")
    checker.check(scene, "Sharpen 面板打开且第一参数高亮",
                  count_matching(img, row1, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, row1, COL_HIGHLIGHT, 12))
    shell.type_text("300")
    shell.press(VK_RETURN)
    shell.press(VK_ESCAPE)
    shell.wait(0.8)
    img = shell.shot("94_sharpen_applied")
    sharpened = region_std(img, probe)
    checker.check(scene, "Sharpen 拉大局部方差（噪声更硬）",
                  sharpened > denoised * 1.5,
                  "σ %.1f -> %.1f" % (denoised, sharpened))


def scene_filter_other(shell, checker):
    """High Pass(1211) / Minimum(1212) / Maximum(1213) / Offset(1214) 的入口与像素方向。

    这一场钉的是**命令号区间**：1200..1210 已被前 11 个滤镜占满，面板动作号顺延到
    1220..1222（见 CMD_FILTER_APPLY 的注释），1211..1214 必须是新滤镜、不能被面板
    动作抢走。四张面板截图各证一个 id 能打开对应面板；像素口径用一块**平坦纯色场**
    量 High Pass——没有细节就没有偏移，整幅必须落 128 中灰，撤销一步原色回来。
    """
    scene = "filter_other"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("8899aa")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left, top, right, bottom, steps=8)
    shell.wait(0.4)
    img = shell.shot("95_other_flat_field")
    probe = (left + 20, top + 20, left + 80, top + 80)
    before = mean_rgb(img, probe)
    checker.check(scene, "铺一块平坦纯色场（下面用它量中灰）",
                  before[2] - before[0] > 20, "实测 %s" % (before,))

    adjust = shell.layout["adjust"]
    row1 = (adjust[0] + 6, adjust[1] + 53, adjust[2] - 8, adjust[1] + 73)

    # High Pass 走 1211，radius 默认 0 = 恒等，键入 4 后回车一次落值并应用。
    shell.command(CMD_FILTER_BASE + 11)
    img = shell.shot("96_high_pass_panel")
    checker.check(scene, "High Pass 面板打开且第一参数高亮",
                  count_matching(img, row1, COL_HIGHLIGHT, 12) > 100,
                  "高亮像素 %d" % count_matching(img, row1, COL_HIGHLIGHT, 12))
    shell.type_text("4")
    shell.press(VK_RETURN)
    shell.press(VK_ESCAPE)
    shell.wait(0.8)
    img = shell.shot("97_high_pass_applied")
    flat = mean_rgb(img, probe)
    checker.check(scene, "High Pass 把平坦区整幅压到 128 中灰",
                  abs(brightness(flat) - 128) <= 10 and max(flat) - min(flat) < 12,
                  "实测 %s 亮度 %d" % (flat, brightness(flat)))
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.8)
    img = shell.shot("98_high_pass_undone")
    restored = mean_rgb(img, probe)
    checker.check(scene, "撤销一步回到原色（且只落了一步）",
                  near(restored, before, 12), "原 %s 现 %s" % (before, restored))

    # 另外三个 id 只验「面板能打开」：1212 / 1213 / 1214 都必须是新滤镜。
    for command_id, name, file_name in ((12, "Minimum", "99_minimum_panel"),
                                        (13, "Maximum", "9a_maximum_panel"),
                                        (14, "Offset", "9b_offset_panel")):
        shell.command(CMD_FILTER_BASE + command_id)
        img = shell.shot(file_name)
        checker.check(scene, "%s 面板打开且第一参数高亮" % name,
                      count_matching(img, row1, COL_HIGHLIGHT, 12) > 100,
                      "高亮像素 %d" % count_matching(img, row1, COL_HIGHLIGHT, 12))
        shell.press(VK_ESCAPE)


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


def adjust_curve_plot(shell):
    """ui_core.h::adjust_curve_plot_rect 的镜像：通道条以下取最大正方形。

    绘制、命中、拖动都用同一个矩形；脚本要按同一份几何算坐标，
    否则「脚本点到的位置」和「外壳认的位置」会各说各话。
    """
    left, top, right, bottom = shell.layout["adjust"]
    width = right - left
    plot_top = top + 50 + 22  # kAdjustParamRowsTop + kCurveChannelBarHeight
    side = min(width - 24, bottom - 180 - plot_top - 8)
    return left + (width - side) // 2, plot_top, side


def scene_curve_levels(shell, checker):
    """
    P0-3 验收：曲线/色阶编辑器。

    1. 加一条恒等 Curves，从绘图区空白处按下加点并往上拖 -> 画布变亮、直方图右移。
    2. 一次 Ctrl+Z 回到修改前（拖一次只算一步撤销）。
    3. 加一条 Levels，把 RGB 黑场拖到 40 -> 暗部被压死（画布整体压暗）。
    4. 再一次 Ctrl+Z 回到色阶前。
    """
    scene = "curve_levels"
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
    img = shell.shot("60_curve_before")
    region = (left + 10, top + 10, right - 10, bottom - 10)
    before = mean_rgb(img, region)
    before_median = median_luma(img, region)

    # 默认 kind = 3（Exposure）；后退一档 = 2（Curves）
    shell.command(CMD_ADJ_KIND_PREV)
    shell.press(ord("A"), ctrl=True, shift=True)  # 新增恒等 Curves
    img = shell.shot("61_curve_added")
    adjust = shell.layout["adjust"]
    px, py, side = adjust_curve_plot(shell)
    checker.check(scene, "曲线绘图区是参数区里的正方形",
                  side >= 48 and py > adjust[1] + 60 and py + side < adjust[3] - 100,
                  "plot=(%d, %d, %d)" % (px, py, side))

    def vx(value):
        return px + (value / 255.0) * (side - 1)

    def vy(value):
        return py + (1.0 - value / 255.0) * (side - 1)

    # 空白处按下 = 加点，按住往上拖 = 抬中间调（一次拖动 = 一步撤销）
    shell.drag(vx(128), vy(128), vx(128), vy(190), steps=8)
    img = shell.shot("62_curve_lifted")
    lifted = mean_rgb(img, region)
    lifted_median = median_luma(img, region)
    checker.check(scene, "曲线抬中间调后画布变亮",
                  brightness(lifted) > brightness(before) + 20,
                  "前 %s 后 %s" % (before, lifted))
    checker.check(scene, "画布直方图中位数右移",
                  lifted_median > before_median + 10,
                  "中位 %d -> %d" % (before_median, lifted_median))

    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("63_curve_undo")
    undo = mean_rgb(img, region)
    checker.check(scene, "一次 Ctrl+Z 回到曲线修改前",
                  abs(brightness(undo) - brightness(before)) < 8,
                  "撤销后 %s 原 %s" % (undo, before))

    # 再后退一档 = 1（Levels），新增一条（缺省恒等：黑 0 / 白 255）
    shell.command(CMD_ADJ_KIND_PREV)
    shell.press(ord("A"), ctrl=True, shift=True)
    img = shell.shot("64_levels_added")

    # 第一行 = levels.ranges.0.black（0..254）；滑道 = 行宽 - 20，行高 34 / 标签带 18
    track_x = adjust[0] + 10
    track_w = (adjust[2] - adjust[0]) - 20
    row_y = adjust[1] + 50 + 18 + 7
    shell.drag(track_x, row_y, track_x + track_w * 40.0 / 254.0, row_y, steps=8)
    img = shell.shot("65_levels_black_40")
    crushed = mean_rgb(img, region)
    checker.check(scene, "色阶黑场拖到 40 后画布压暗",
                  brightness(crushed) < brightness(before) * 0.9,
                  "前 %s 后 %s" % (before, crushed))

    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("66_levels_undo")
    restored = mean_rgb(img, region)
    checker.check(scene, "一次 Ctrl+Z 回到色阶修改前",
                  abs(brightness(restored) - brightness(before)) < 8,
                  "撤销后 %s 原 %s" % (restored, before))


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


def doc_dialog_aspect(shell):
    """适合窗口 -> 量文档在屏幕上的宽高比。空（透明）文档是纯黑，可测。"""
    shell.press(ord("0"), ctrl=True)
    shell.wait(0.45)
    left, top, right, bottom = shell.document_rect(refresh=True)
    return float(right - left) / float(bottom - top), (right - left, bottom - top)


def scene_doc_dialog(shell, checker):
    """
    文档级对话框（S25 P0-3）：新建文档 / 图像大小 / 画布大小 / 历史容量。

    断言全部是「量出来的画布宽高比」，不是截图观感；口径沿用 crop 场景——
    空（透明）文档渲染成纯黑，detect_document_rect 能给出屏幕矩形。默认文档
    是 1280×720（16:9），下面每一步换成别的比例都能区分：
      1. 新建 640×480 -> 4:3；
      2. 图像大小：Tab 到「约束比例」用空格关掉，再设 320×120 -> 8:3；
      3. 画布大小：宽度 520（居中锚点）-> 13:3；
      4. Esc 取消画布大小不改尺寸；
      5. 历史容量面板能打开、Esc 能收回（右栏像素确实变过）。
    """
    scene = "doc_dialog"
    panel = shell.layout["adjust"]
    title_rect = (panel[0] + 8, panel[1] + 8, panel[2] - 8, panel[1] + 96)

    # 1) 新建文档：640×480。背景保持默认「透明」——填了底色文档就不再是纯黑，
    #    下面的几何断言就失去可靠口径（白底文档的探针由标题栏脏标记覆盖）。
    shell.command(CMD_NEW_DOCUMENT_DIALOG)
    img = shell.shot("37_new_doc_dialog")
    bright = count_bright(img, title_rect, 150)
    checker.check(scene, "新建文档面板已绘制（右栏标题区有文字）", bright > 120,
                  "右栏浅色像素 %d" % bright)
    shell.type_text("640")
    shell.press(VK_TAB)          # 提交宽度，焦点到高度行
    shell.type_text("480")
    shell.press(VK_RETURN)       # 提交高度并应用
    shell.wait(0.4)
    aspect, size = doc_dialog_aspect(shell)
    checker.check(scene, "新建 640×480 生效（文档比例 4:3）",
                  abs(aspect - 4.0 / 3.0) <= 0.06,
                  "实测比例 %.3f（屏幕 %s）" % (aspect, size))

    # 2) 图像大小：默认「约束比例 = 开」，先关掉才能设非等比尺寸。
    shell.command(CMD_IMAGE_SIZE)
    shell.press(VK_TAB)
    shell.press(VK_TAB)          # 焦点到「约束比例」
    shell.press(VK_SPACE)        # 关掉
    shell.press(VK_UP)
    shell.press(VK_UP)           # 回到宽度行
    shell.type_text("320")
    shell.press(VK_TAB)          # 提交宽度；比例未锁，高度不动
    shell.type_text("120")
    shell.press(VK_RETURN)
    shell.wait(0.6)
    shell.shot("37b_image_size_applied")
    aspect, size = doc_dialog_aspect(shell)
    checker.check(scene, "图像大小 320×120 生效（文档比例 8:3）",
                  abs(aspect - 8.0 / 3.0) <= 0.12,
                  "实测比例 %.3f（屏幕 %s）" % (aspect, size))

    # 3) 画布大小：只改宽度到 520，锚点保持默认居中。
    shell.command(CMD_CANVAS_SIZE)
    shell.type_text("520")
    shell.press(VK_RETURN)
    shell.wait(0.6)
    aspect, size = doc_dialog_aspect(shell)
    checker.check(scene, "画布大小 520×120 生效（文档比例 13:3）",
                  abs(aspect - 13.0 / 3.0) <= 0.16,
                  "实测比例 %.3f（屏幕 %s）" % (aspect, size))
    size_after_canvas = size

    # 4) Esc 取消：键入 100 后连按两次 Esc（第一次撤草稿、第二次关面板），
    #    画布尺寸必须一点不变。
    shell.command(CMD_CANVAS_SIZE)
    shell.type_text("100")
    shell.press(VK_ESCAPE)
    shell.press(VK_ESCAPE)
    shell.wait(0.3)
    _, size_after_esc = doc_dialog_aspect(shell)
    checker.check(scene, "Esc 取消画布大小不改变尺寸",
                  size_after_esc == size_after_canvas,
                  "取消前 %s，取消后 %s" % (size_after_canvas, size_after_esc))

    # 5) 历史容量：面板打开 -> Esc 收回，右栏像素必须变过（不是「开着当没开」）。
    shell.command(CMD_HISTORY_SETTINGS)
    img_open = shell.shot("38_history_settings")
    shell.press(VK_ESCAPE)
    shell.wait(0.3)
    img_closed = shell.shot("39_history_settings_closed")
    changed = pixel_diff(img_open, img_closed,
                         (panel[0] + 4, panel[1] + 4, panel[2] - 4, panel[1] + 160), 24)
    checker.check(scene, "历史容量面板打开后被 Esc 收回（右栏像素发生变化）",
                  changed > 500, "变化像素 %d" % changed)


def scene_camera_raw(shell, checker):
    """
    P1-5 Camera Raw 滤镜入口：面板 -> 曝光真写进像素 -> 一步撤销 -> 负号键入。

    为什么用中性灰做底：曝光是线性光乘 2^EV，灰色在中段，+1 EV 与 -1 EV 都会
    明显朝两个方向分开（约 128 -> 176 / 92），不会像黑白两端那样撞上限或钳到 0。
    断言量的是灰块中心一块区域的平均色，不是整屏截图观感：
      1. 画一个 #808080 矩形，确认底子确实是中性灰；
      2. Ctrl+Alt+R 打开面板（右栏像素必须变过，证明真开了）；
      3. 曝光行键入 100（= +1.00 EV）回车 -> 灰块变亮；
      4. Ctrl+Z -> 回到曝光前的平均色（一步撤销，不是两次）；
      5. 重开面板键入 -100 -> 灰块变暗（负号能起输入，没有被当成步进 -1）。
    """
    scene = "camera_raw"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("808080")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    x0, y0 = left + 30, top + 30
    x1, y1 = right - 30, bottom - 30
    shell.drag(x0, y0, x1, y1, steps=10)
    img = shell.shot("40_camera_raw_before")
    region = ((x0 + x1) // 2 - 20, (y0 + y1) // 2 - 20,
              (x0 + x1) // 2 + 20, (y0 + y1) // 2 + 20)
    before = mean_rgb(img, region)
    checker.check(scene, "前置：画布上有中性灰图层",
                  90 < brightness(before) < 160, "灰块平均 %s" % (before,))

    panel = shell.layout["adjust"]
    panel_title = (panel[0] + 4, panel[1] + 4, panel[2] - 4, panel[1] + 120)
    before_panel = shell.capture()
    shell.press(ord("R"), ctrl=True, alt=True)
    img_open = shell.shot("41_camera_raw_panel")
    changed = pixel_diff(img_open, before_panel, panel_title, 24)
    checker.check(scene, "Ctrl+Alt+R 打开 Camera Raw 面板（右栏像素变化）",
                  changed > 300, "变化像素 %d" % changed)

    shell.type_text("100")       # 曝光 +1.00 EV
    shell.press(VK_RETURN)
    shell.wait(0.6)
    img = shell.shot("42_camera_raw_exposure_up")
    brighter = mean_rgb(img, region)
    checker.check(scene, "曝光 +1.00 EV 后灰块变亮",
                  brightness(brighter) > brightness(before) + 20,
                  "曝光后 %s 原 %s" % (brighter, before))

    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.5)
    img = shell.shot("43_camera_raw_undo")
    undone = mean_rgb(img, region)
    checker.check(scene, "一步撤销回到曝光前的像素",
                  abs(brightness(undone) - brightness(before)) <= 3,
                  "撤销后 %s 原 %s" % (undone, before))

    shell.press(ord("R"), ctrl=True, alt=True)
    shell.type_text("-100")      # 负号起输入 -> 曝光 -1.00 EV
    shell.press(VK_RETURN)
    shell.wait(0.6)
    img = shell.shot("44_camera_raw_exposure_down")
    darker = mean_rgb(img, region)
    checker.check(scene, "负号键入生效：曝光 -1.00 EV 使灰块变暗",
                  brightness(darker) < brightness(before) - 15,
                  "曝光后 %s 原 %s" % (darker, before))


def scene_clipboard(shell, checker):
    """
    P0-4 剪贴板：拷贝 -> 清除 -> 贴入选区 -> 撤销 -> 普通粘贴。

    为什么先铺满一块橙色再清除：清除后画布回到黑（空图层），之后任何橙色都只能
    来自剪贴板，「往返内容一致」才被截图证实，而不是靠状态栏文字。贴入选区单独
    验一次落点与蒙版：橙色只出现在选区里，右半必须还是黑。
    """
    scene = "clipboard"
    left, top, right, bottom = shell.document_rect()
    mid_x = (left + right) // 2
    mid_y = (top + bottom) // 2

    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left + 20, top + 20, right - 20, bottom - 20, steps=10)
    img = shell.shot("80_clipboard_source")
    checker.check(scene, "前置：画布铺满橙色矩形",
                  near(pixel(img, mid_x, mid_y), (255, 136, 0), 40),
                  "实测 %s" % (pixel(img, mid_x, mid_y),))

    # 无选区时 Ctrl+C 拷整层，Backspace 清空整层
    shell.press(ord("C"), ctrl=True)
    shell.press(VK_BACK)
    shell.wait(0.2)
    img = shell.shot("81_clipboard_cleared")
    checker.check(scene, "Backspace 清除后画布回到黑（图层已清空）",
                  near(pixel(img, mid_x, mid_y), (0, 0, 0), 10),
                  "实测 %s" % (pixel(img, mid_x, mid_y),))

    # 选左半区，Ctrl+Alt+V 贴入：只在选区内出现橙色
    shell.select_tool("marquee")
    shell.drag(left + 20, top + 20, mid_x - 20, bottom - 20, steps=10)
    shell.press(ord("V"), ctrl=True, alt=True)
    shell.wait(0.3)
    img = shell.shot("82_clipboard_paste_into")
    inside = pixel(img, mid_x - 40, mid_y)
    outside = pixel(img, mid_x + 60, mid_y)
    checker.check(scene, "贴入选区：选区内橙色、选区外仍是黑",
                  near(inside, (255, 136, 0), 40) and near(outside, (0, 0, 0), 10),
                  "内 %s 外 %s" % (inside, outside))

    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.2)
    img = shell.shot("83_clipboard_undo_into")
    checker.check(scene, "Ctrl+Z 一步撤销贴入（整幅回到黑）",
                  near(pixel(img, mid_x - 40, mid_y), (0, 0, 0), 10),
                  "实测 %s" % (pixel(img, mid_x - 40, mid_y),))

    # 取消选区后 Ctrl+V：按剪贴板记下的落点铺回去（左右都应是橙色）
    shell.press(ord("D"), ctrl=True)
    shell.press(ord("V"), ctrl=True)
    shell.wait(0.3)
    img = shell.shot("84_clipboard_paste")
    left_px = pixel(img, mid_x - 40, mid_y)
    right_px = pixel(img, mid_x + 60, mid_y)
    checker.check(scene, "Ctrl+V 粘贴回原落点（左右两半都是橙）",
                  near(left_px, (255, 136, 0), 40) and near(right_px, (255, 136, 0), 40),
                  "左 %s 右 %s" % (left_px, right_px))

    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.2)
    img = shell.shot("85_clipboard_undo_paste")
    checker.check(scene, "Ctrl+Z 一步撤销粘贴（画布回到黑）",
                  near(pixel(img, mid_x, mid_y), (0, 0, 0), 10),
                  "实测 %s" % (pixel(img, mid_x, mid_y),))


def scene_close_prompt(shell, checker):
    """
    P0-3 关闭提示：脏文档关窗时必须出现右栏三选一（不是 MessageBox）。

    为什么要截屏而不是读状态栏：这条功能的失败方式就是「弹了个阻塞对话框」或
    「干脆什么也没问就退出了」。所以先量进程活没活，再量右栏像素有没有变
    （面板真的画出来），最后量画布像素一点没动（取消 = 不改文档）。

    断言：
      1. 脏文档 post WM_CLOSE 后进程仍存活（没有静默丢弃，也没有把消息循环卡死）；
      2. 右栏标题区像素变化（关闭提示真的画在面板里，而不是无提示）；
      3. Esc 取消后进程仍存活、画布像素逐像素不变；
      4. 再次 WM_CLOSE 后按 D（不保存），进程在 3 秒内退出。
    """
    scene = "close_prompt"
    # 先量文档矩形：空文档是纯黑，detect_document_rect 可靠；填了颜色之后再量
    # 就失去黑色外接框这个口径，所以顺序不能反。
    left, top, right, bottom = shell.document_rect()
    canvas = shell.layout["canvas"]

    # 脏文档：形状工具铺一块橙色（图层像素变了，引擎的 dirty 才会置位）。
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left + 30, top + 30, right - 30, bottom - 30, steps=8)
    shell.wait(0.3)
    before = shell.shot("45_close_prompt_before")

    panel = shell.layout["adjust"]
    title_rect = (panel[0] + 4, panel[1] + 4, panel[2] - 4, panel[1] + 120)

    user32.PostMessageW(shell.hwnd, WM_CLOSE, 0, 0)
    shell.wait(0.6)
    alive_after_close = shell.proc.poll() is None
    img = shell.shot("46_close_prompt_panel")
    checker.check(scene, "脏文档关窗后进程仍存活（没有静默丢弃）", alive_after_close,
                  "proc.poll=%s" % (shell.proc.poll(),))
    changed = pixel_diff(img, before, title_rect, 24)
    checker.check(scene, "关闭提示已绘制在右栏（标题区像素变化）", changed > 300,
                  "变化像素 %d" % changed)

    # Esc = 取消：窗口活着、画布一个像素都不许变。
    shell.press(VK_ESCAPE)
    shell.wait(0.4)
    img_cancel = shell.shot("47_close_prompt_cancelled")
    checker.check(scene, "Esc 取消后进程仍存活", shell.proc.poll() is None,
                  "proc.poll=%s" % (shell.proc.poll(),))
    canvas_changed = pixel_diff(img_cancel, before, canvas, 24)
    checker.check(scene, "取消关闭不改画布像素", canvas_changed == 0,
                  "画布变化像素 %d" % canvas_changed)

    # 再关一次：D = 不保存，进程必须在 3 秒内退出（面板确认真的执行了）。
    user32.PostMessageW(shell.hwnd, WM_CLOSE, 0, 0)
    shell.wait(0.6)
    prompt_again = shell.proc.poll() is None
    if prompt_again:
        shell.press(ord("D"))
    exited = True
    try:
        shell.proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        exited = False
    checker.check(scene, "按 D 不保存后进程退出（3 秒内）", exited,
                  "proc.poll=%s" % (shell.proc.poll(),))
    if exited:
        shell.hwnd = None  # 窗口已销毁，stop() 不必再 post WM_CLOSE


def scene_options_bar(shell, checker):
    """
    P0-10 工具选项栏：横栏存在、点击归横栏、改的值真的进了画布。

    这条功能的失败方式有三种，所以每条都单独量：
      1. 横栏没画出来 / 画到画布上（布局破了）；
      2. 点击穿透到画布（横栏在命中顺序里排在画布之后）——用「点 + 之后画布
         像素逐字节不变」证明；
      3. 值只改了显示没进状态（横栏与右栏各存一份副本）——用同一个落点画一笔，
         大画笔的橙色像素数必须明显多于小画笔。

    断言：
      1. 选项栏占 32px、底色与工具条不同；
      2. 画笔工具有「大小 / 硬度」两组，控件盒底色正确、标签有字形；
      3. 连点 4 次「+」后值文本变化，且画布一个像素都没变（点击被横栏吃掉）；
      4. 同一落点画点：放大后的橙色像素数 > 2 倍初始值；
      5. 形状工具第一组是 Choice，点击后值文本变化、画布仍不变。
    """
    scene = "options_bar"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    cx, cy = (left + right) // 2, (top + bottom) // 2
    bar = shell.layout["options"]
    checker.check(scene, "选项栏在工具栏下方占 32px", bar[3] - bar[1] == OPTIONS_H,
                  "options=%s 客户区高=%d" % (bar, shell.client_h))

    # 先换前景色（Shift+X 十六进制键入），否则黑笔在空文档上看不见。
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("brush")
    img = shell.shot("48_options_bar_brush")

    checker.check(scene, "选项栏底色", near(pixel(img, bar[0] + 2, bar[1] + 2), COL_OPTIONS_BG, 8),
                  "实测 %s" % (pixel(img, bar[0] + 2, bar[1] + 2),))
    group0 = shell.option_group_rect(0)
    group1 = shell.option_group_rect(1)
    checker.check(scene, "画笔有两组选项（大小 / 硬度）", group1 is not None,
                  "group1=%s" % (group1,))
    control0 = shell.option_control_rect(0)
    checker.check(scene, "第一组控件盒可见",
                  control0 is not None and
                  near(pixel(img, control0[0] + 2, control0[1] + 2), COL_OPTION_CTRL, 10),
                  "control=%s 实测 %s" % (control0,
                                        pixel(img, control0[0] + 2, control0[1] + 2)))
    labels = count_bright(img, (group0[0], group0[1] + 6, group0[0] + OPTION_LABEL_W - 6,
                               group0[3] - 6), 110)
    checker.check(scene, "选项标签有字形", labels > 10, "浅色像素 %d" % labels)

    # 初始画笔：画一个小点，记橙色像素数。
    shell.drag(cx - 2, cy, cx + 2, cy, steps=3)
    shell.wait(0.2)
    img_small = shell.shot("49_options_bar_small_dot")
    box = (cx - 70, cy - 70, cx + 70, cy + 70)
    small = count_matching(img_small, box, (255, 136, 0), 60)
    checker.check(scene, "小画笔在落点留下橙色像素", small > 0, "橙色像素 %d" % small)
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.2)
    after_undo = shell.shot("50_options_bar_undo")
    checker.check(scene, "Ctrl+Z 后落点清空",
                  count_matching(after_undo, box, (255, 136, 0), 60) == 0,
                  "残留 %d" % count_matching(after_undo, box, (255, 136, 0), 60))

    # 连点 4 次「+」：值文本必须变；画布必须一个像素都不变（点击归横栏）。
    plus = (control0[2] - OPTION_ARROW_W // 2, (control0[1] + control0[3]) // 2)
    for _ in range(4):
        shell.click(plus[0], plus[1])
        shell.wait(0.05)
    shell.wait(0.2)
    img_big = shell.shot("51_options_bar_bigger")
    value_diff = pixel_diff(img_big, after_undo, control0, 24)
    checker.check(scene, "点 + 后大小数值变化", value_diff > 20, "值区变化像素 %d" % value_diff)
    canvas = shell.layout["canvas"]
    penetration = pixel_diff(img_big, after_undo, canvas, 24)
    checker.check(scene, "选项栏点击不穿透到画布", penetration == 0,
                  "画布变化像素 %d" % penetration)

    # 同一落点再画一次：更大的笔尖覆盖的橙色像素必须明显更多。
    shell.drag(cx - 2, cy, cx + 2, cy, steps=3)
    shell.wait(0.2)
    img_wide = shell.shot("52_options_bar_wide_dot")
    wide = count_matching(img_wide, box, (255, 136, 0), 60)
    checker.check(scene, "放大后的笔迹覆盖更多像素", wide > small * 2,
                  "小 %d → 大 %d" % (small, wide))

    # Choice 组：形状工具第一组是「形状」，点一下整块都算下一档。
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.2)
    shell.select_tool("shape")
    img_shape = shell.shot("53_options_bar_shape")
    shape_control = shell.option_control_rect(0)
    checker.check(scene, "形状工具有选项组", shape_control is not None,
                  "control=%s" % (shape_control,))
    if shape_control is not None:
        shell.click((shape_control[0] + shape_control[2]) // 2,
                    (shape_control[1] + shape_control[3]) // 2)
        shell.wait(0.3)
        img_choice = shell.shot("54_options_bar_choice")
        choice_diff = pixel_diff(img_choice, img_shape, shape_control, 24)
        checker.check(scene, "点 Choice 值切换下一档", choice_diff > 20,
                      "值区变化像素 %d" % choice_diff)
        checker.check(scene, "Choice 点击不穿透到画布",
                      pixel_diff(img_choice, img_shape, canvas, 24) == 0,
                      "画布变化像素 %d" % pixel_diff(img_choice, img_shape, canvas, 24))


def scene_brush_options(shell, checker):
    """
    P1-4 画笔选项：预设 / 流量 / 压感。

    压感需要真实数位笔；本脚本只能合成鼠标消息（系统把笔提升成鼠标的路径上
    恒为满笔压），所以这里不假装测压感，只测**它不改变鼠标像素**，外加两条
    用户真能点的路：
      1. 画笔选项栏有 5 组，第 2/3/4 组是预设/流量/压感（0/1 仍是大小/硬度）；
      2. 流量 100% -> 50% 后，同一落点的橙色覆盖明显变少；
      3. 点一次「预设」（硬边圆）把流量/硬度写回，覆盖回到满流量水平；
      4. 压感开关能点（值文本 开 -> 关），画布一个像素都不动。
    """
    scene = "brush_options"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    cx, cy = (left + right) // 2, (top + bottom) // 2 - 80

    # 先换前景色（Shift+X 十六进制键入），否则黑笔在空文档上看不见。
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("brush")
    shell.wait(0.2)

    group4 = shell.option_group_rect(4)
    checker.check(scene, "画笔选项栏排到第 5 组（0 大小 / 1 硬度 / 2 预设 / 3 流量 / 4 压感）",
                  group4 is not None and group4[2] <= shell.layout["options"][2],
                  "group4=%s bar=%s" % (group4, shell.layout["options"]))
    preset_control = shell.option_control_rect(2)
    flow_control = shell.option_control_rect(3)
    pressure_control = shell.option_control_rect(4)
    checker.check(scene, "预设/流量/压感三组控件盒都可见",
                  preset_control is not None and flow_control is not None and
                  pressure_control is not None,
                  "preset=%s flow=%s pressure=%s" % (preset_control, flow_control,
                                                     pressure_control))
    if preset_control is None or flow_control is None or pressure_control is None:
        return

    box = (cx - 80, cy - 80, cx + 80, cy + 80)

    def tint_count(shot):
        """落点里「偏橙」的像素数：满流量是强橙，半流量是浅橙，都算画上了。"""
        data = shot.crop(box).convert("RGB").tobytes()
        count = 0
        for index in range(0, len(data), 3):
            if data[index] - data[index + 2] > 40:
                count += 1
        return count

    def dot_shot(tag):
        """同一落点画一点 -> 数橙色像素 -> 撤销（保证下一次从干净画布开始）。"""
        shell.drag(cx - 2, cy, cx + 2, cy, steps=3)
        shell.wait(0.2)
        shot = shell.shot(tag)
        strong = count_matching(shot, box, (255, 136, 0), 60)
        tinted = tint_count(shot)
        shell.press(ord("Z"), ctrl=True)
        shell.wait(0.2)
        return strong, tinted

    full, full_tint = dot_shot("113_brush_options_full")
    checker.check(scene, "满流量落点有橙色像素", full > 0 and full_tint > 0,
                  "强橙 %d / 偏橙 %d" % (full, full_tint))

    # 流量 100% -> 50%：每组单击 -5%，十下。
    flow_before = shell.shot("114_brush_options_flow_before")
    flow_arrow = (flow_control[0] + OPTION_ARROW_W // 2,
                  (flow_control[1] + flow_control[3]) // 2)
    for _ in range(10):
        shell.click(flow_arrow[0], flow_arrow[1])
        shell.wait(0.05)
    shell.wait(0.2)
    flow_after = shell.shot("115_brush_options_flow_50")
    flow_text_diff = pixel_diff(flow_after, flow_before, flow_control, 24)
    checker.check(scene, "流量箭头的值文本变化", flow_text_diff > 20,
                  "值区变化像素 %d" % flow_text_diff)
    low, low_tint = dot_shot("116_brush_options_flow_low")
    checker.check(scene, "流量 50% 仍然落墨（浅橙），但强橙覆盖少于满流量",
                  low_tint > 0 and low < full,
                  "满 %d/%d -> 50%% 强橙 %d / 偏橙 %d" % (full, full_tint, low, low_tint))

    # 点一次「预设」：下标 1 = 硬边圆（24 px / 硬度 100% / 流量 100%）。
    preset_before = shell.shot("117_brush_options_preset_before")
    shell.click((preset_control[0] + preset_control[2]) // 2,
                (preset_control[1] + preset_control[3]) // 2)
    shell.wait(0.3)
    preset_after = shell.shot("118_brush_options_preset_after")
    preset_text_diff = pixel_diff(preset_after, preset_before, preset_control, 24)
    checker.check(scene, "点预设后值文本变化", preset_text_diff > 20,
                  "值区变化像素 %d" % preset_text_diff)
    restored, restored_tint = dot_shot("119_brush_options_preset_dot")
    checker.check(scene, "预设把流量写回后强橙覆盖恢复", restored > low and restored > 0,
                  "50%% %d -> 预设 %d（偏橙 %d）" % (low, restored, restored_tint))

    # 压感开关：鼠标路径没有笔压，点一下只改显示。先把指针移出画布，免得两次
    # 截图之间笔尖轮廓消失被算成「画布变了」。
    canvas = shell.layout["canvas"]
    bar = shell.layout["options"]
    shell.click(bar[2] - 10, (bar[1] + bar[3]) // 2)
    shell.wait(0.2)
    pressure_before = shell.shot("120_brush_options_pressure_before")
    shell.click((pressure_control[0] + pressure_control[2]) // 2,
                (pressure_control[1] + pressure_control[3]) // 2)
    shell.wait(0.3)
    pressure_after = shell.shot("121_brush_options_pressure_off")
    pressure_text_diff = pixel_diff(pressure_after, pressure_before, pressure_control, 24)
    checker.check(scene, "压感开关的值文本变化（开 -> 关）", pressure_text_diff > 20,
                  "值区变化像素 %d" % pressure_text_diff)
    pressure_canvas_diff = pixel_diff(pressure_after, pressure_before, canvas, 24)
    checker.check(scene, "压感开关不碰画布", pressure_canvas_diff == 0,
                  "画布变化像素 %d" % pressure_canvas_diff)


def long_bright_rows(img, xa, xb, y_from, y_to, min_px=60, threshold=150):
    """横跨整幅的蚂蚁线所在行（长亮行）。

    蚂蚁线是白/黑交替虚线且**逐帧走动**，所以任何逐像素对比都会把相位差当成
    变化；能稳定量的是「哪几行有横贯画布的虚线」。角上手柄只有几个像素，
    min_px 一挡就滤掉了。边界命令的结果就是「原本 1 条长亮行 -> 环带两侧 2 条」。
    """
    rows = []
    for row in range(int(y_from), int(y_to)):
        count = count_bright(img, (int(xa), row, int(xb), row + 1), threshold)
        if count >= min_px:
            rows.append(row)
    return rows


def brightest_column(img, col_x, y_from, y_to, span=6, threshold=150):
    """在 col_x 附近找竖直蚂蚁线所在的列（亮像素最多的那一列）。"""
    best_col, best_count = int(col_x), -1
    for col in range(int(col_x) - span, int(col_x) + span + 1):
        count = count_bright(img, (col, int(y_from), col + 1, int(y_to)), threshold)
        if count > best_count:
            best_col, best_count = col, count
    return best_col, best_count


def scene_selection_modify(shell, checker):
    """
    P0-5 修改补全：选择 > 边界 / 平滑 的「菜单 -> 键入半径 -> 回车」全链路。

    这条功能的失败方式：菜单没接上（回车没反应）、键入态被工具快捷键吃掉
    （半径永远进不去，P0-5 修过一次）、或者命令执行了但选区没变。四条分别量：
      1. 前置：矩形选区画出 1 条横贯长亮行（= 上边蚂蚁线）；
      2. 边界 21 px：原线消失，上下各出现 1 条新蚂蚁线（环带两侧），状态栏报半径；
      3. 平滑 100 px：直边仍留在原行（开闭运算不动直边），左上角贴身的一段
         竖边/横边被圆弧换掉（两小块扫描窗从「有虚线」变「全空」）；
      4. 平滑后 Ctrl+Z 一步，回退到矩形（单条长亮行 + 两块扫描窗的虚线复原）。

    半径取 21 / 100 而不是 3 / 6 / 30：画布默认 49% 缩放，3 个文档像素只有
    1.5 个屏幕像素，环带两侧的两条虚线会重叠成一条；平滑用 30 时圆角只把边线
    起点推出约 15 屏幕像素，和蚂蚁线「4 亮 4 暗」的周期同量级，相位一漂就把
    差异吃掉。100 文档像素约 49 屏幕像素，圆弧在贴角 8~26 px 这一段上离边线
    还有 6 px 以上，扫描窗里只剩原来的直边虚线，平滑后必空。
    """
    scene = "selection_modify"
    cmd_border = 1058  # main.cpp: kCmdSelectBorder
    cmd_smooth = 1059  # main.cpp: kCmdSelectSmooth
    left, top, right, bottom = shell.document_rect()
    x0, y0 = left + 60, top + 60
    x1, y1 = right - 60, bottom - 60
    xa, xb = x0 + 40, x1 - 40  # 中段扫描窗，避开四个角手柄与左右竖线

    shell.select_tool("marquee")
    shell.drag(x0, y0, x1, y1, steps=10)
    before = shell.shot("55_modify_rect")
    rows_before = long_bright_rows(before, xa, xb, y0 - 16, y0 + 16)
    checker.check(scene, "前置：矩形选区上边是一条横贯长亮行",
                  len(rows_before) == 1, "长亮行 %s" % rows_before)
    line_row = rows_before[0] if rows_before else y0

    # 边界 21 px：dilate(11) - erode(10)，外侧比原线外 11 px、内侧内 10 px，
    # 于是上边变成两条长亮行，原线所在行落进环带内部（不再有虚线）。
    shell.command(cmd_border)
    shell.type_text("21")
    shell.press(VK_RETURN)
    border = shell.shot("56_modify_border21")
    rows_border = long_bright_rows(border, xa, xb, y0 - 40, y0 + 40)
    above = [row for row in rows_border if row < line_row]
    below = [row for row in rows_border if row > line_row]
    checker.check(scene, "边界 21 px：原线消失、上下各出一条新蚂蚁线",
                  len(above) == 1 and len(below) == 1 and len(rows_border) == 2,
                  "长亮行 %s（原线 %d）" % (rows_border, line_row))
    status_border = pixel_diff(border, before, shell.layout["status"], 24)
    checker.check(scene, "状态栏报告「已将选区边界 21 px」", status_border > 20,
                  "状态栏变化像素 %d" % status_border)

    # 退回矩形再平滑：开运算不动直边，只把直角磨圆 100 px。
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.3)
    shell.command(cmd_smooth)
    shell.type_text("100")
    shell.press(VK_RETURN)
    smooth = shell.shot("57_modify_smooth100")
    rows_smooth = long_bright_rows(smooth, xa, xb, y0 - 40, y0 + 40)
    checker.check(scene, "平滑 100 px：直边留在原行（仍是一条长亮行）",
                  rows_smooth == [line_row], "长亮行 %s" % rows_smooth)

    # 左上角：圆角最直接的证据是「贴角那一段竖线/横线被圆弧替换掉了」。
    # 两块扫描窗都紧贴角点、避开了 5×5 的白色角手柄，且只取离角 8~26 px 的一段：
    # 磨圆 100 文档像素（屏幕上约 49 px）后，圆弧在这一段上离边线还有 6 px 以上，
    # 不会落进窗里；而原来的直边在这里必然有虚线（窗长跨 2 个以上虚线周期）。
    # 数「窗内亮像素」与虚线相位无关。
    col_left, col_count = brightest_column(before, x0, y0 + 40, y0 + 160)
    v_win = (col_left - 1, line_row + 8, col_left + 2, line_row + 26)
    h_win = (col_left + 4, line_row, col_left + 26, line_row + 1)
    v_before = count_bright(before, v_win, 150)
    h_before = count_bright(before, h_win, 150)
    v_smooth = count_bright(smooth, v_win, 150)
    h_smooth = count_bright(smooth, h_win, 150)
    checker.check(scene, "前置：贴角两窗（竖边/横边各一段）都有虚线",
                  abs(col_left - x0) <= 2 and col_count >= 30 and
                  v_before >= 6 and h_before >= 6,
                  "左边线列 %d(%d)、竖窗 %d、横窗 %d" %
                  (col_left, col_count, v_before, h_before))
    checker.check(scene, "平滑 100 px 磨圆直角（贴角两窗清空）",
                  v_smooth == 0 and h_smooth == 0,
                  "竖窗 %d->%d、横窗 %d->%d" % (v_before, v_smooth, h_before, h_smooth))

    # 撤销一步：回到矩形——单条长亮行在原行，两块扫描窗的虚线也都回来。
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.3)
    undone = shell.shot("58_modify_smooth_undo")
    rows_undo = long_bright_rows(undone, xa, xb, y0 - 40, y0 + 40)
    v_undo = count_bright(undone, v_win, 150)
    h_undo = count_bright(undone, h_win, 150)
    checker.check(scene, "平滑撤销一步后回到矩形（角部虚线复原）",
                  rows_undo == [line_row] and v_undo >= 6 and h_undo >= 6,
                  "长亮行 %s、竖窗 %d、横窗 %d" % (rows_undo, v_undo, h_undo))


def scene_selection_grow(shell, checker):
    """
    P0-5 收尾：选择 > 扩大选取 / 选取相似 的端到端链路。

    失败方式各自单独量：
      1. 菜单项没接上（WM_COMMAND 被吞）——选区原地不动，上边线不动；
      2. 命令跑了但边界算错方向——旧上边线没消失，或新线不在矩形上边；
      3. 命令真改了选区但没落撤销步——Ctrl+Z 回不到小矩形；
      4. 「选取相似」在选区已经吃满同色区域后还硬塞一条撤销步——
         一次 Ctrl+Z 退的是相似而不是扩大。

    做法：形状工具画一块**实心**矩形（硬边、单色 0066cc，四周是画布黑），
    在它内部拖一个小矩形选区；扩大选取（容差取壳上魔棒默认 32）应把选区
    吃到整块矩形：上边线从矩形内部的 my0 外扩到矩形上边 y0；选取相似此时
    已没有同色新像素，边界不动；一次 Ctrl+Z 退回小矩形选区。

    用形状而不是画笔：画笔硬度 75% 的软边在容差 32 下会一路漫进黑底，
    边界不在矩形边上，就量不到「停在颜色边界」这个承诺。填充色必须选
    R 通道 < 150 的（这里是 0066cc）：long_bright_rows 数的是 R>=150 的
    像素，亮橙色填充会把整块矩形都算成「长亮行」，蚂蚁线就淹没了。
    """
    scene = "selection_grow"
    cmd_grow = 1060     # shell_state.h: kCmdSelectGrow
    cmd_similar = 1061  # shell_state.h: kCmdSelectSimilar
    left, top, right, bottom = shell.document_rect()
    x0, y0 = left + 90, top + 90
    x1, y1 = right - 90, bottom - 90

    shell.press(ord("X"), shift=True)
    shell.type_text("0066cc")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(x0, y0, x1, y1, steps=10)

    shell.select_tool("marquee")
    mx0, my0 = x0 + 70, y0 + 70
    mx1, my1 = x1 - 70, y1 - 70
    shell.drag(mx0, my0, mx1, my1, steps=10)
    before = shell.shot("59_grow_marquee")
    rows_before = long_bright_rows(before, mx0 + 40, mx1 - 40, my0 - 16, my0 + 16)
    checker.check(scene, "前置：小矩形选区的上边是一条长亮行",
                  len(rows_before) == 1, "长亮行 %s" % rows_before)
    line_row = rows_before[0] if rows_before else my0

    # 扩大选取：上边从 my0 外扩到矩形上边 y0，旧线必须消失。
    shell.command(cmd_grow)
    grown = shell.shot("60_grow_result")
    old_rows = long_bright_rows(grown, mx0 + 40, mx1 - 40, my0 - 16, my0 + 16)
    new_rows = long_bright_rows(grown, mx0 + 40, mx1 - 40, y0 - 16, y0 + 16)
    checker.check(scene, "扩大选取：旧上边线消失、外扩到矩形上边",
                  not old_rows and len(new_rows) == 1 and abs(new_rows[0] - y0) <= 2,
                  "旧行 %s、新行 %s（矩形上边 %d）" % (old_rows, new_rows, y0))
    status_grow = pixel_diff(grown, before, shell.layout["status"], 24)
    checker.check(scene, "状态栏报告「已扩大选取」", status_grow > 20,
                  "状态栏变化像素 %d" % status_grow)

    # 选取相似：同色区域已选满 -> 边界不动（不新增撤销步）。
    shell.command(cmd_similar)
    similar = shell.shot("61_similar_result")
    rows_similar = long_bright_rows(similar, mx0 + 40, mx1 - 40, y0 - 16, y0 + 16)
    old_similar = long_bright_rows(similar, mx0 + 40, mx1 - 40, my0 - 16, my0 + 16)
    checker.check(scene, "选取相似：同色区域已选满，边界不动",
                  not old_similar and len(rows_similar) == 1 and
                  abs(rows_similar[0] - y0) <= 2,
                  "长亮行 %s（旧线窗口 %s）" % (rows_similar, old_similar))

    # 一次 Ctrl+Z 必须退回小矩形：相似没有占撤销步。
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.3)
    undone = shell.shot("62_grow_undo")
    rows_undo = long_bright_rows(undone, mx0 + 40, mx1 - 40, my0 - 16, my0 + 16)
    grown_undo = long_bright_rows(undone, mx0 + 40, mx1 - 40, y0 - 16, y0 + 16)
    checker.check(scene, "Ctrl+Z 一步退回小矩形选区",
                  len(rows_undo) == 1 and abs(rows_undo[0] - line_row) <= 1 and not grown_undo,
                  "长亮行 %s（原线 %d）、矩形上边窗口 %s" % (rows_undo, line_row, grown_undo))


def scene_selection_transform(shell, checker):
    """
    P0-5 收尾：选择 > 变换选区（宽 / 高 / 角度数值输入）的端到端链路。

    轴对齐手柄本来就能缩放，这条命令多出来的是**数值输入 + 旋转**。失败方式
    各自单独量：
      1. 菜单项没接上——面板不出现，选区不动；
      2. 恒等回车（角度 0、宽高等于当前包围盒）硬塞一条空撤销步——引擎没改
         蒙版却落了历史，一次 Ctrl+Z 会退掉看不见的东西；
      3. 角度没生效或转反——包围盒不交换；
      4. 改了选区却没落撤销步（或落了不止一步）——Ctrl+Z 回不到原矩形。

    做法：在空文档上拖一个 **宽高不等**（屏幕 240×120）的矩形选区。量蚂蚁线
    用「沿中线扫描」：横边穿过中线那一列，竖边穿过中线那一行；各取 ±5 px 的
    窗口，任何一行/列只要有白像素就算边线所在——蚂蚁线 4 亮 4 暗逐帧走动，
    窗口宽度盖住一个周期，所以与相位无关。转 90° 后包围盒必然变成 120×240，
    这个交换是方向性断言：镜像实现（角度取反）也测不出来，但「没转」一定能测出。

    宽高取 240×120 而不是更大：默认文档 1280×720 适合窗口后高约 353 屏幕
    像素，转完高 240 仍留得下边距，扫描窗口不会碰到画布外。
    """
    scene = "selection_transform"
    cmd_transform = 1062  # shell_state.h: kCmdSelectTransform
    panel = shell.layout["adjust"]
    title_rect = (panel[0] + 8, panel[1] + 8, panel[2] - 8, panel[1] + 96)

    def span_rows(img, x_center, y_from, y_to, half=5):
        """x_center 附近哪些行有白像素：横边穿过这一列，返回上/下边行号。"""
        rows = []
        for row in range(int(y_from), int(y_to)):
            if count_bright(img, (int(x_center) - half, row,
                                  int(x_center) + half, row + 1), 150) > 0:
                rows.append(row)
        return rows

    def span_cols(img, y_center, x_from, x_to, half=5):
        """y_center 附近哪些列有白像素：竖边穿过这一行，返回左/右边列号。"""
        cols = []
        for col in range(int(x_from), int(x_to)):
            if count_bright(img, (col, int(y_center) - half,
                                  col + 1, int(y_center) + half), 150) > 0:
                cols.append(col)
        return cols

    left, top, right, bottom = shell.document_rect()
    cx, cy = (left + right) // 2, (top + bottom) // 2
    half_w, half_h = 120, 60
    scan_x = half_w + 30  # 扫描窗半宽：盖住转后的高（240）还留 30 px 余量

    shell.select_tool("marquee")
    shell.drag(cx - half_w, cy - half_h, cx + half_w, cy + half_h, steps=10)
    base = shell.shot("63_transform_base")
    base_rows = span_rows(base, cx, cy - scan_x, cy + scan_x)
    base_cols = span_cols(base, cy, cx - scan_x, cx + scan_x)
    base_h = (max(base_rows) - min(base_rows)) if base_rows else -1
    base_w = (max(base_cols) - min(base_cols)) if base_cols else -1
    checker.check(scene, "前置：矩形选区的蚂蚁线可量（约 240×120 屏幕像素）",
                  len(base_rows) >= 2 and len(base_cols) >= 2 and
                  abs(base_w - 2 * half_w) <= 6 and abs(base_h - 2 * half_h) <= 6,
                  "行 %s、列 %s（高 %d / 宽 %d）" %
                  (base_rows, base_cols, base_h, base_w))

    # 1) 面板必须真的画在右栏（菜单 -> doc_dialog_begin 全链路）。
    shell.command(cmd_transform)
    opened = shell.shot("64_transform_dialog")
    bright_open = count_bright(opened, title_rect, 150)
    checker.check(scene, "变换选区面板已绘制（右栏标题区有文字）", bright_open > 120,
                  "右栏浅色像素 %d" % bright_open)

    # 2) 恒等：什么都不改直接回车。宽高等于当前包围盒、角度 0，引擎 masks_equal
    #    短路由 -> changed = 0：选区不动、**不落撤销步**，面板留着不吞输入。
    shell.press(VK_RETURN)
    shell.wait(0.3)
    identity = shell.shot("65_transform_identity")
    ident_rows = span_rows(identity, cx, cy - scan_x, cy + scan_x)
    ident_cols = span_cols(identity, cy, cx - scan_x, cx + scan_x)
    checker.check(scene, "恒等等于当前包围盒：选区纹丝不动",
                  ident_rows == base_rows and ident_cols == base_cols,
                  "行 %s->%s、列 %s->%s" %
                  (base_rows, ident_rows, base_cols, ident_cols))
    checker.check(scene, "恒等不落撤销步：面板保持打开（输入不丢）",
                  count_bright(identity, title_rect, 150) > 120,
                  "回车后右栏浅色像素 %d" % count_bright(identity, title_rect, 150))
    status_identity = pixel_diff(identity, opened, shell.layout["status"], 24)
    checker.check(scene, "状态栏说明「选区未变化」", status_identity > 20,
                  "状态栏变化像素 %d" % status_identity)

    # 收起面板（第一次 Esc 撤键入态、第二次关面板）。
    shell.press(VK_ESCAPE)
    shell.press(VK_ESCAPE)
    shell.wait(0.3)
    closed = shell.shot("66_transform_closed")
    checker.check(scene, "Esc 收回变换选区面板（右栏像素变化）",
                  pixel_diff(opened, closed, title_rect, 24) > 20,
                  "右栏标题带变化像素 %d" % pixel_diff(opened, closed, title_rect, 24))

    # 3) 转 90°：Tab 两次到角度行，键入 90 回车。包围盒必须交换 240×120 ->
    #    120×240（顺时针在屏幕坐标里同样是 90°，宽高互换与方向无关，但「没转」
    #    和「转 45°」都会被这一条挡住）。
    shell.command(cmd_transform)
    shell.press(VK_TAB)
    shell.press(VK_TAB)
    shell.type_text("90")
    shell.press(VK_RETURN)
    shell.wait(0.5)
    rotated = shell.shot("67_transform_rot90")
    rot_rows = span_rows(rotated, cx, cy - scan_x, cy + scan_x)
    rot_cols = span_cols(rotated, cy, cx - scan_x, cx + scan_x)
    rot_h = (max(rot_rows) - min(rot_rows)) if rot_rows else -1
    rot_w = (max(rot_cols) - min(rot_cols)) if rot_cols else -1
    checker.check(scene, "角度 90°：包围盒交换（高 120 -> 240、宽 240 -> 120）",
                  abs(rot_h - 2 * half_w) <= 6 and abs(rot_w - 2 * half_h) <= 6,
                  "行 %s、列 %s（高 %d / 宽 %d）" % (rot_rows, rot_cols, rot_h, rot_w))
    checker.check(scene, "应用成功后面板自动关闭",
                  pixel_diff(opened, rotated, title_rect, 24) > 20,
                  "右栏标题带变化像素 %d" % pixel_diff(opened, rotated, title_rect, 24))
    status_rot = pixel_diff(rotated, identity, shell.layout["status"], 24)
    checker.check(scene, "状态栏报告变换结果（撤销一步）", status_rot > 20,
                  "状态栏变化像素 %d" % status_rot)

    # 4) 一次 Ctrl+Z 必须回到原矩形（变换恰好占一步，不多不少）。
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.3)
    undone = shell.shot("68_transform_undo")
    undo_rows = span_rows(undone, cx, cy - scan_x, cy + scan_x)
    undo_cols = span_cols(undone, cy, cx - scan_x, cx + scan_x)
    checker.check(scene, "Ctrl+Z 一步回到原矩形选区",
                  undo_rows == base_rows and undo_cols == base_cols,
                  "行 %s->%s、列 %s->%s" %
                  (base_rows, undo_rows, base_cols, undo_cols))


def scene_layer_transform_numeric(shell, checker):
    """
    P1-3：图层 > 数值变换（Ctrl+T）+ 再次变换（Ctrl+Shift+T）的端到端链路。

    各自量一种失败方式：
      1. 命令没接线——面板不出现，图层不动；
      2. 面板打开就回车被当成一次变换——隐式 transform 被物化，多落一条空撤销步；
      3. 宽度没写进去 / 写错了轴——跨度不变，或竖跨度跟着变；
      4. 再次变换没复读上一次的比例——第二次执行形状不动；
      5. 两次变换各占一步撤销：Ctrl+Z×2 必须回到原尺寸。

    度量口径：形状工具画一个**以画布中心对称**的矩形，数值变换是中心锚定，
    所以「宽度减半、高度不变」两条互相独立的断言，再配中心位置不变，能同时
    挡住「没变」「缩错了轴」「缩了但跑偏」。绝对像素数不可靠（画布按窗口
    缩放），因此只比**跨度比例**。
    """
    scene = "layer_transform_numeric"
    cmd_numeric = 1063  # shell_state.h: kCmdLayerTransformNumeric
    panel = shell.layout["adjust"]
    title_rect = (panel[0] + 8, panel[1] + 8, panel[2] - 8, panel[1] + 96)
    ref = (255, 136, 0)

    left, top, right, bottom = shell.document_rect()
    cx, cy = (left + right) // 2, (top + bottom) // 2
    half_w, half_h = 120, 60

    def span_x(img, y):
        lo = hi = None
        for x in range(int(left) + 1, int(right) - 1, 2):
            if near(pixel(img, x, int(y)), ref, 45):
                if lo is None:
                    lo = x
                hi = x
        return (lo, hi)

    def span_y(img, x):
        lo = hi = None
        for y in range(int(top) + 1, int(bottom) - 1, 2):
            if near(pixel(img, int(x), y), ref, 45):
                if lo is None:
                    lo = y
                hi = y
        return (lo, hi)

    def width_of(span):
        return (span[1] - span[0]) if span[0] is not None else -1

    def center_of(span):
        return ((span[0] + span[1]) / 2.0) if span[0] is not None else -1.0

    # 前景色 + 居中矩形（宽高比 2:1，缩一半后仍可量）。
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(cx - half_w, cy - half_h, cx + half_w, cy + half_h, steps=8)
    before = shell.shot("69_layer_xform_base")
    base_x = span_x(before, cy)
    base_y = span_y(before, cx)
    base_w, base_h = width_of(base_x), width_of(base_y)
    checker.check(scene, "前置：居中矩形的横/竖跨度都可量",
                  base_w > 60 and base_h > 30 and abs(center_of(base_x) - cx) <= 6,
                  "横 %s、竖 %s（宽 %d / 高 %d / 中心 %.1f）" %
                  (base_x, base_y, base_w, base_h, center_of(base_x)))

    # 1) 面板必须真的画出来。
    shell.command(cmd_numeric)
    opened = shell.shot("70_layer_xform_dialog")
    checker.check(scene, "数值变换面板已绘制（右栏标题区有文字）",
                  count_bright(opened, title_rect, 150) > 120,
                  "右栏浅色像素 %d" % count_bright(opened, title_rect, 150))

    # 2) 恒等回车：宽高/角度都取自当前图层，不该改动像素，也不该落撤销步。
    shell.press(VK_RETURN)
    shell.wait(0.3)
    identity = shell.shot("71_layer_xform_identity")
    id_x, id_y = span_x(identity, cy), span_y(identity, cx)
    checker.check(scene, "恒等回车：图层尺寸纹丝不动",
                  abs(width_of(id_x) - base_w) <= 4 and abs(width_of(id_y) - base_h) <= 4,
                  "横 %d（原 %d）、竖 %d（原 %d）" %
                  (width_of(id_x), base_w, width_of(id_y), base_h))
    checker.check(scene, "恒等不落撤销步：面板保持打开（输入不丢）",
                  count_bright(identity, title_rect, 150) > 120,
                  "回车后右栏浅色像素 %d" % count_bright(identity, title_rect, 150))

    # 3) 宽度改成 640（默认文档 1280 => 横向缩一半）：竖跨度必须不变。
    shell.type_text("640")
    shell.press(VK_RETURN)
    shell.wait(0.5)
    scaled = shell.shot("72_layer_xform_width")
    sc_x, sc_y = span_x(scaled, cy), span_y(scaled, cx)
    sc_w, sc_h = width_of(sc_x), width_of(sc_y)
    checker.check(scene, "宽度 640：横跨度约减半",
                  0 < sc_w <= base_w * 0.75,
                  "横跨度 %d（原 %d，比例 %.2f）" % (sc_w, base_w, sc_w / base_w if base_w else -1))
    checker.check(scene, "高度未填：竖跨度保持不变（没缩错轴）",
                  abs(sc_h - base_h) <= 8,
                  "竖跨度 %d（原 %d）" % (sc_h, base_h))
    checker.check(scene, "中心锚定：缩小后中心没有跑偏",
                  abs(center_of(sc_x) - cx) <= 8,
                  "中心 x %.1f（画布中心 %d）" % (center_of(sc_x), cx))
    checker.check(scene, "应用成功后面板自动关闭",
                  pixel_diff(opened, scaled, title_rect, 24) > 20,
                  "右栏标题带变化像素 %d" % pixel_diff(opened, scaled, title_rect, 24))

    # 4) 再次变换：复读上一步的 0.5 倍横缩，横跨度必须再减半。
    shell.press(ord("T"), ctrl=True, shift=True)
    shell.wait(0.5)
    again = shell.shot("73_layer_xform_again")
    ag_x, ag_y = span_x(again, cy), span_y(again, cx)
    ag_w = width_of(ag_x)
    checker.check(scene, "Ctrl+Shift+T 再次变换：横跨度再减半",
                  0 < ag_w <= sc_w * 0.75,
                  "横跨度 %d（上一次 %d，比例 %.2f）" % (ag_w, sc_w, ag_w / sc_w if sc_w else -1))
    checker.check(scene, "再次变换不再缩竖轴（上一步的竖比例是 1）",
                  abs(width_of(ag_y) - base_h) <= 8,
                  "竖跨度 %d（原 %d）" % (width_of(ag_y), base_h))

    # 5) 两次变换 = 两步撤销：撤两次必须回到原尺寸。
    shell.press(ord("Z"), ctrl=True)
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.3)
    undone = shell.shot("74_layer_xform_undo")
    un_x, un_y = span_x(undone, cy), span_y(undone, cx)
    checker.check(scene, "Ctrl+Z×2 回到原尺寸",
                  abs(width_of(un_x) - base_w) <= 6 and abs(width_of(un_y) - base_h) <= 6,
                  "横 %d（原 %d）、竖 %d（原 %d）" %
                  (width_of(un_x), base_w, width_of(un_y), base_h))


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


def scene_text_layer(shell, checker):
    """P0-6 ①：文字工具产出的是「可编辑文字图层」，不是画死的像素。

    三条用户可见承诺，全部用截图与像素证实（不读状态栏文字）：
      1. 回车提交后图层面板多出一行 —— 新建的是图层，不是涂在选中层上；
      2. 点回文本框内再提交 —— 还是同一层（行数不变），字形变宽（文字真的改了）；
      3. 图层 > 栅格化文字 之后在同一位置再提交 —— 又新建一层（像素层不能再改）。
    """
    scene = "text_layer"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    layer = shell.layout["layer"]
    panel = (layer[0], layer[1], layer[0] + 240, layer[1] + 160)
    origin = (left + 40, top + 40)
    glyph_box = (origin[0] - 6, origin[1] - 10, origin[0] + 220, origin[1] + 90)

    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("type")

    before = shell.shot("81_text_layer_before")
    rows_one = count_bright(before, panel, 150)

    # 1) 第一段文字：提交 = 新建文字图层（面板多一行）
    shell.click(*origin)
    shell.type_text("H")
    shell.press(VK_RETURN)
    img = shell.shot("82_text_layer_created")
    rows_two = count_bright(img, panel, 150)
    checker.check(scene, "提交文字后图层面板多出一行（新建的是图层）",
                  rows_two > rows_one + 20,
                  "面板文字像素 %d -> %d" % (rows_one, rows_two))
    count_h, hx0, _, hx1, _ = glyph_bbox(img, glyph_box)
    checker.check(scene, "新建的文字图层在画布上有字形", count_h > 10, "字形像素 %d" % count_h)

    # 2) 点回文本框内再提交：同一层改文字（行数不变，字形变宽）
    inside = (origin[0] + 3, origin[1] + 3)
    shell.click(*inside)
    shell.type_text("HH")
    shell.press(VK_RETURN)
    img = shell.shot("83_text_layer_edited")
    rows_after_edit = count_bright(img, panel, 150)
    checker.check(scene, "框内再提交仍是同一图层（行数不变）",
                  abs(rows_after_edit - rows_two) <= 20,
                  "面板文字像素 %d -> %d" % (rows_two, rows_after_edit))
    count_hh, gx0, _, gx1, _ = glyph_bbox(img, glyph_box)
    width_h = hx1 - hx0 + 1
    width_hh = gx1 - gx0 + 1
    checker.check(scene, "改文字后字形变宽（同一层重新渲染）",
                  count_hh > count_h and width_hh > width_h + 3,
                  "字形 %d -> %d 像素，宽 %d -> %d px"
                  % (count_h, count_hh, width_h, width_hh))

    # 3) 栅格化文字：之后同位置再提交只能新建一层
    shell.command(1572)  # 图层 > 栅格化文字
    shell.click(*origin)
    shell.type_text("H")
    shell.press(VK_RETURN)
    img = shell.shot("84_text_layer_rasterized")
    rows_raster = count_bright(img, panel, 150)
    checker.check(scene, "栅格化后再提交会新建另一层（像素层不可编辑）",
                  rows_raster > rows_after_edit + 20,
                  "面板文字像素 %d -> %d" % (rows_after_edit, rows_raster))

    # 撤销两步应回到「只有一层文字」的状态：栅格化与新建各一步。
    shell.press(ord("Z"), ctrl=True)
    shell.press(ord("Z"), ctrl=True)
    img = shell.shot("85_text_layer_undo")
    rows_undo = count_bright(img, panel, 150)
    checker.check(scene, "撤销两步回到单层文字（新建与栅格化各一步）",
                  abs(rows_undo - rows_two) <= 20,
                  "面板文字像素 %d（栅格化前 %d）" % (rows_undo, rows_two))


def scene_font_picker(shell, checker):
    """P0-6 ②：字体选择器面板（右栏出现 / 过滤 / 两次 Esc 才关闭）。

    不依赖机器上装了哪些字体：只断言面板「出现 / 重绘 / 仍在 / 消失」，
    列表内容与族名一律不做像素断言（不同机器的字体目录必然不同）。

    为什么两次 Esc：面板里第一次 Esc 只清过滤词（有过滤时），第二次才关面板。
    这条顺序是键盘路径的公开语义，退化成「一次就关」会让用户丢掉过滤词。
    """
    scene = "font_picker"
    adjust = shell.layout["adjust"]
    shell.select_tool("type")

    before = shell.shot("86_font_picker_before")
    shell.command(1573)  # 图层 > 字体…
    opened = shell.shot("87_font_picker_open")
    changed = pixel_diff(before, opened, adjust)
    checker.check(scene, "字体面板在右栏出现（替换文字属性面板）",
                  changed > 2000, "右栏变化像素 %d" % changed)

    # 过滤行在标题（y+8）与计数行（y+30）之下：从 y+48 起比对，避开「扫描中 n/m」
    # 这种随时间变化的进度文本。
    body = (adjust[0], adjust[1] + 48, adjust[2], adjust[3])
    shell.type_text("zzzz")  # 过滤词：几乎不可能命中任何字体
    filtered = shell.shot("88_font_picker_filtered")
    typed = pixel_diff(opened, filtered, body)
    checker.check(scene, "键入过滤词后列表重绘（空结果显示无匹配提示）",
                  typed > 50, "面板变化像素 %d" % typed)

    # 第一次 Esc：只清过滤词，面板仍在（与打开前相比仍是大片不同）
    shell.press(VK_ESCAPE)
    still_open = shell.shot("89_font_picker_esc_cleared")
    survived = pixel_diff(before, still_open, adjust)
    checker.check(scene, "第一次 Esc 只清过滤词，面板仍在",
                  survived > 2000, "相对打开前的差异 %d" % survived)

    # 第二次 Esc：关闭面板，右栏回到文字属性面板（与打开前几乎逐像素一致）
    shell.press(VK_ESCAPE)
    closed = shell.shot("90_font_picker_closed")
    restored = pixel_diff(before, closed, adjust)
    checker.check(scene, "第二次 Esc 关闭后右栏回到文字属性面板",
                  restored < 200, "与打开前的差异 %d" % restored)


def scene_guides(shell, checker):
    """ROI #3：标尺 / 参考线 / 网格 / 对齐。

    断言口径全部可算（颜色计数 / 像素差），不依赖观感：
      1. 标尺默认关闭；打开后画布顶部 18px 变成标尺底色；
      2. 从上尺拖进画布 → 出现一条贯穿画布的青色参考线；
      3. 关标尺**不删**参考线（参考线是文档状态，标尺只是画不画它）；
      4. Ctrl+; 隐藏参考线后青色消失，再按一次回来（视图开关可逆）；
      5. Ctrl+' 网格是覆盖层（打开后画布必然有像素变化）；
      6. Ctrl+Shift+; 对齐开关**一个像素都不改**——它只吸附拖动落点。

    场景顺序有意先做第 6 步：此时画布上还没有参考线，指针悬停不可能产生
    高亮状态，两帧必须在画布矩形内逐像素相同。

    存盘往返（拖到 12.5px 后关闭再打开位置不变）由 tests/test_guides.cpp 的
    .comp round-trip 用例覆盖——要落盘就得点模态文件对话框，脚本无法无人值守。
    """
    scene = "guides"
    canvas = shell.layout["canvas"]
    cl, ct, cr, cb = canvas
    band = (cl, ct, cr, ct + 18)                       # 上尺条带
    body = (cl + 24, ct + 24, cr - 24, cb - 24)        # 避开标尺与画布边缘

    before = shell.shot("91_guides_off")
    ruler_px = count_matching(before, band, (46, 48, 54), 6)
    checker.check(scene, "标尺默认关闭（画布上方没有标尺条）",
                  ruler_px < 50, "标尺底色像素 %d" % ruler_px)

    # 6. 对齐开关：只改交互，不改像素。
    shell.command(1583)  # 视图 > 对齐（关）
    snap_off = shell.shot("92_snap_off")
    snap_delta = pixel_diff(before, snap_off, canvas)
    checker.check(scene, "关闭对齐不改变任何像素（只影响拖动落点）",
                  snap_delta == 0, "画布差异像素 %d" % snap_delta)
    shell.command(1583)  # 恢复默认

    # 1. 打开标尺。
    shell.command(1580)  # 视图 > 标尺
    ruled = shell.shot("93_rulers_on")
    ruler_px = count_matching(ruled, band, (46, 48, 54), 6)
    checker.check(scene, "打开标尺：画布顶部 18px 出现标尺条",
                  ruler_px > (cr - cl) * 8, "标尺底色像素 %d" % ruler_px)

    # 2. 从上尺拖出水平参考线。
    shell.select_tool("move")
    x0 = (cl + cr) // 2
    shell.drag(x0, ct + 6, x0, ct + (cb - ct) // 2)
    guided = shell.shot("94_guide_added")
    guide_px = count_matching(guided, body, (0, 174, 239), 40)
    checker.check(scene, "从标尺拖出参考线：画布上出现贯穿的青色细线",
                  guide_px > (cr - cl) // 3, "参考线像素 %d" % guide_px)

    # 3. 关标尺：参考线仍在。
    shell.command(1580)
    kept = shell.shot("95_rulers_off_guide_kept")
    kept_px = count_matching(kept, body, (0, 174, 239), 40)
    checker.check(scene, "关闭标尺后参考线仍在（文档状态 ≠ 标尺可见性）",
                  kept_px > (cr - cl) // 3, "参考线像素 %d" % kept_px)

    # 4. 隐藏 / 显示参考线。
    shell.command(1581)  # 视图 > 参考线
    hidden = shell.shot("96_guides_hidden")
    hidden_px = count_matching(hidden, body, (0, 174, 239), 40)
    checker.check(scene, "隐藏参考线后画布恢复干净（青色消失）",
                  hidden_px < 20, "参考线像素 %d" % hidden_px)
    shell.command(1581)
    shown = shell.shot("97_guides_shown")
    shown_px = count_matching(shown, body, (0, 174, 239), 40)
    checker.check(scene, "再次显示参考线（视图开关可逆，位置没有丢）",
                  shown_px > (cr - cl) // 3, "参考线像素 %d" % shown_px)

    # 5. 网格覆盖层。
    shell.command(1582)  # 视图 > 网格
    grid = shell.shot("98_grid_on")
    grid_delta = pixel_diff(shown, grid, body)
    checker.check(scene, "打开网格：画布出现 50px 网格覆盖层",
                  grid_delta > 500, "网格变化像素 %d" % grid_delta)
    shell.command(1582)
    grid_off = shell.shot("99_grid_off")
    back_delta = pixel_diff(shown, grid_off, body)
    checker.check(scene, "关闭网格后画布回到只有参考线的样子",
                  back_delta == 0, "与开网格前的差异像素 %d" % back_delta)


def scene_destructive_adjust(shell, checker):
    """ROI #8：图像 > 调整 的破坏性落墨（命令号 1590 + kind）。

    与 `adjustment` / `curve_levels` 的分工：那两场验的是**调整图层**（非破坏、可再
    编辑）；这里验的是同一批求值器的一次性落墨入口。断言覆盖：面板能开（标题换字样，
    说明不是新增调整图层）、落墨真的改像素、一次 Ctrl+Z 能回去、结果没变的那次落墨
    **不进历史**、零参数 kind（自动色调）单击即落墨不进面板。
    """
    scene = "destructive_adjust"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    shell.press(ord("X"), shift=True)
    shell.type_text("336699")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    shell.drag(left, top, right, bottom, steps=8)
    shell.select_tool("select")

    img = shell.shot("100_destructive_before")
    region = (left + 10, top + 10, right - 10, bottom - 10)
    before = mean_rgb(img, region)
    adjust = shell.layout["adjust"]
    # 标题只占面板顶部 22 px；下一行「类型 n/16」在 +30，落在带外，比较才不会被种类名污染。
    title_band = (adjust[0] + 6, adjust[1] + 6, adjust[2] - 6, adjust[1] + 26)
    idle_title = img

    # 1) 1590+12 = Brightness/Contrast：进破坏性面板，参数行与调整面板共用。
    shell.command(CMD_IMAGE_ADJUST_BASE + 12)
    img = shell.shot("101_bc_panel")
    checker.check(scene, "图像 > 调整 > Brightness/Contrast 打开破坏性面板",
                  pixel_diff(idle_title, img, title_band) > 20,
                  "标题带差异像素 %d" % pixel_diff(idle_title, img, title_band))
    # 键入 40 而不是连按步进键：每按一次都出一帧全分辨率预览，消息会堆在队列里。
    shell.type_text("40")
    shell.press(VK_RETURN)
    shell.press(ord("U"), ctrl=True, shift=True)
    shell.wait(0.8)
    img = shell.shot("102_bc_applied")
    lifted = mean_rgb(img, region)
    checker.check(scene, "破坏性 Brightness/Contrast +40 直接改像素",
                  brightness(lifted) > brightness(before) + 15,
                  "前 %s 后 %s" % (before, lifted))
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.4)
    img = shell.shot("103_bc_undo")
    undo = mean_rgb(img, region)
    checker.check(scene, "一次 Ctrl+Z 回到破坏性调整前",
                  abs(brightness(undo) - brightness(before)) < 8,
                  "撤销后 %s 原 %s" % (undo, before))

    # 2) 1590+13 = Desaturate：无参数，单击即落墨（不该停在面板态）。
    shell.command(CMD_IMAGE_ADJUST_BASE + 13)
    shell.wait(0.8)
    img = shell.shot("104_desaturate")
    gray = mean_rgb(img, region)
    checker.check(scene, "Desaturate 单击即落墨（三通道拉平）",
                  max(gray) - min(gray) <= 3 and (max(before) - min(before)) > 40,
                  "前 %s 后 %s" % (before, gray))

    # 3) 1590+14 = Threshold：默认 128，中灰（≈92）落到黑。
    shell.command(CMD_IMAGE_ADJUST_BASE + 14)
    shell.type_text("128")
    shell.press(VK_RETURN)
    shell.press(ord("U"), ctrl=True, shift=True)
    shell.wait(0.8)
    img = shell.shot("105_threshold")
    binary = mean_rgb(img, region)
    checker.check(scene, "Threshold 128 把中灰压成纯黑或纯白",
                  brightness(binary) < 8 or brightness(binary) > 247,
                  "阈值后 %s（阈值前 %s）" % (binary, gray))
    # 同参数再落一次：像素没变 -> 不得多出一步撤销。这一步错了，下面的 Ctrl+Z 会停在黑屏。
    shell.press(ord("U"), ctrl=True, shift=True)
    shell.wait(0.8)
    shell.press(ord("Z"), ctrl=True)
    shell.wait(0.4)
    img = shell.shot("106_threshold_noop_undo")
    restored = mean_rgb(img, region)
    checker.check(scene, "结果未变的第二次落墨不进历史（一次 Ctrl+Z 回到阈值前）",
                  abs(brightness(restored) - brightness(gray)) < 8,
                  "撤销后 %s 阈值前 %s" % (restored, gray))

    # 4) 1590+15 = Posterize：中灰 3 级量化到中间档（≈128），仍是平的。
    shell.command(CMD_IMAGE_ADJUST_BASE + 15)
    shell.type_text("3")
    shell.press(VK_RETURN)
    shell.press(ord("U"), ctrl=True, shift=True)
    shell.wait(0.8)
    img = shell.shot("107_posterize")
    poster = mean_rgb(img, region)
    checker.check(scene, "Posterize 3 级把中灰量化到中间档",
                  region_std(img, region) < 1.5 and abs(brightness(poster) - brightness(gray)) > 15,
                  "灰 %s -> %s σ %.1f" % (gray, poster, region_std(img, region)))

    # 5) 1590+2 = Curves：点列没有可写的调整对象，走壳的寄存串；拖中点抬亮后落墨。
    shell.command(CMD_IMAGE_ADJUST_BASE + 2)
    px, py, side = adjust_curve_plot(shell)
    # 恒等曲线上没有控制点：按在绘图区中间 = 加点，同一次拖动把它抬到 y≈0.8。
    shell.drag(px + side / 2.0, py + side / 2.0, px + side / 2.0, py + side * 0.2, steps=8)
    shell.press(ord("U"), ctrl=True, shift=True)
    shell.wait(0.8)
    img = shell.shot("108_curve_destructive")
    curved = mean_rgb(img, region)
    checker.check(scene, "破坏性 Curves：拖曲线抬中点后落墨变亮",
                  brightness(curved) > brightness(poster) + 20,
                  "Posterize 后 %s 曲线后 %s" % (poster, curved))

    # 6) Esc 只退破坏性态：标题回到「调整（Adjustment）」，且像素不再被落墨。
    #    必须紧跟在一段破坏性面板会话之后验——再开别的面板会换掉标题带基线。
    shell.press(VK_ESCAPE)
    shell.wait(0.2)
    img = shell.shot("109_escaped")
    checker.check(scene, "Esc 退出破坏性模式（标题回到 Adjustment）",
                  pixel_diff(idle_title, img, title_band) == 0,
                  "标题带差异像素 %d" % pixel_diff(idle_title, img, title_band))

    # 7) 1590+18 = Photo Filter：5 行参数，Tab 到 density（第 4 行）后 100% 加温。
    #    这条同时验证「追加 kind 的参数表能经 ABI 送到外壳」——面板行数由
    #    comp_layer_adjustment_param_count 决定，外壳没有第二张表。
    shell.command(CMD_IMAGE_ADJUST_BASE + 18)
    img = shell.shot("110_photo_filter_panel")
    checker.check(scene, "图像 > 调整 > Photo Filter 打开破坏性面板",
                  pixel_diff(idle_title, img, title_band) > 20,
                  "标题带差异像素 %d" % pixel_diff(idle_title, img, title_band))
    for _ in range(3):
        shell.press(VK_TAB)
    shell.type_text("100")
    shell.press(VK_RETURN)
    shell.press(ord("U"), ctrl=True, shift=True)
    shell.wait(0.8)
    img = shell.shot("111_photo_filter_applied")
    warm = mean_rgb(img, region)
    checker.check(scene, "加温滤镜 100% 把灰推成暖色（R 明显高于 B）",
                  warm[0] - warm[2] > 30,
                  "落墨后 %s" % (warm,))

    # 8) 1590+22 = Auto Tone：无参数，必须**不进面板态**（标题带保持 Adjustment）。
    #    若零参数 kind 误走面板路径，这里会看到标题换字样。
    shell.command(CMD_IMAGE_ADJUST_BASE + 22)
    shell.wait(0.6)
    img = shell.shot("112_auto_tone")
    checker.check(scene, "自动色调单击即落墨、不打开参数面板",
                  pixel_diff(idle_title, img, title_band) == 0,
                  "标题带差异像素 %d" % pixel_diff(idle_title, img, title_band))



def scene_effect_pattern(shell, checker):
    """
    图案叠加（P1-1 第二段，kind 9）：纯程序化图案，不依赖任何资源文件。
    缺省参数 = 100% 缩放（32px 周期）的黑白棋盘，因此「加效果」后矩形内部必须
    同时出现接近纯黑与纯白的像素，且原来的橙色完全消失；再点一次同一菜单项
    （勾选态 -> 移除）必须逐像素回到橙色。这条链路同时覆盖：
    命令号 1519 的接线（菜单发号 = kCmdEffectBase + kind）、参数表缺省值真的
    落到引擎（壳内不拼默认 JSON）、以及效果只在源形状内取色（不外扩）。
    """
    scene = "effect_pattern"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    width, height = right - left, bottom - top

    # 一份纯色矩形当底：棋盘的两色都必须能和它区分开（橙 vs 黑/白）。
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    x0, y0 = left + 20, top + 20
    x1, y1 = right - 20, bottom - 20
    shell.drag(x0, y0, x1, y1, steps=10)
    img = shell.shot("113_effect_pattern_base")
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    base = mean_rgb(img, (cx - 6, cy - 6, cx + 6, cy + 6))
    checker.check(scene, "底层纯色矩形", near(base, (255, 136, 0), 40), "实测 %s" % (base,))

    adjust = shell.layout["adjust"]
    panel_before = img  # 打开面板前的一帧（同一窗口、同一布局）
    shell.command(CMD_EFFECT_BASE + 9)
    shell.wait(0.6)
    img = shell.shot("114_effect_pattern_panel")
    checker.check(scene, "菜单勾选后右栏出现效果参数面板",
                  pixel_diff(panel_before, img, (adjust[0], adjust[1], adjust[0] + adjust[2],
                                                 adjust[1] + adjust[3])) > 40,
                  "右栏差异像素 %d" % pixel_diff(panel_before, img,
                                                (adjust[0], adjust[1], adjust[0] + adjust[2],
                                                 adjust[1] + adjust[3])))

    # Esc 只关面板，图层不动；画布上才是判定图案的主场。
    shell.press(VK_ESCAPE)
    shell.wait(0.5)
    img = shell.shot("115_effect_pattern_checker")
    # 96x96 的采样窗覆盖 3x3 个 32px 棋盘格，两色都必然出现。
    probe = (cx - 48, cy - 48, cx + 48, cy + 48)
    whites = count_matching(img, probe, (255, 255, 255), 10)
    blacks = count_matching(img, probe, (0, 0, 0), 10)
    checker.check(scene, "棋盘图案同时有黑格与白格", whites > 400 and blacks > 400,
                  "白 %d 黑 %d" % (whites, blacks))
    checker.check(scene, "图案完全盖住底色（橙色不再出现）",
                  count_matching(img, probe, (255, 136, 0), 40) == 0,
                  "橙色像素 %d" % count_matching(img, probe, (255, 136, 0), 40))

    # 再点一次同一项 = 移除（面板不打开）；撤销栈由引擎事务负责，这里只验像素。
    shell.command(CMD_EFFECT_BASE + 9)
    shell.wait(0.5)
    img = shell.shot("116_effect_pattern_off")
    back = mean_rgb(img, (cx - 6, cy - 6, cx + 6, cy + 6))
    checker.check(scene, "再次勾选移除效果后回到纯色", near(back, (255, 136, 0), 40),
                  "实测 %s" % (back,))


def scene_gradient_stops(shell, checker):
    """
    可编辑色标表（P1-7 收尾）：右栏改表 -> 回车应用 -> 落笔画布跟着变。

    三种失败方式各量一项，全部走用户的真实操作路径，不直接读 ShellState：
      1. 面板没画出来（布局/命中破了）——右栏标题区必须有字形；
      2. 增删行只改显示、表没动——插入/删除后右栏文本必须各变一次；
      3. 改的颜色只进草稿、没进落笔——渐变在 25% / 50% / 75% 三处采样：
         50% 必须是改过的洋红，两侧的黄与青还在。两侧在，说明自定义表确实
         是用「彩虹」播种的 5 个色标，而不是退化成两色或空表。
    """
    scene = "gradient_stops"
    doc = shell.document_rect()
    left, top, right, bottom = doc
    y = (top + bottom) // 2
    panel = shell.layout["adjust"]
    title_rect = (panel[0] + 8, panel[1] + 8, panel[2] - 8, panel[1] + 96)

    shell.select_tool("gradient")

    # 预设是 Choice：整块都算「下一档」。0 -> 1 -> 2 -> 3（自定义），
    # 踏进自定义时用刚离开的「彩虹」现展开一份 5 色标表当起点。
    preset = shell.option_control_rect(1)
    checker.check(scene, "渐变预设控件在选项栏上", preset is not None, "preset=%s" % (preset,))
    if preset is None:
        return
    center = ((preset[0] + preset[2]) // 2, (preset[1] + preset[3]) // 2)
    for _ in range(3):
        shell.click(center[0], center[1])
    shell.wait(0.3)

    # 「色标表」是 Stepper：点在数值上不响应，只有右箭头吃点击（避免误触）。
    edit = shell.option_control_rect(3)
    checker.check(scene, "色标表控件在选项栏上", edit is not None, "edit=%s" % (edit,))
    if edit is None:
        return
    shell.click(edit[2] - OPTION_ARROW_W // 2, (edit[1] + edit[3]) // 2)
    shell.wait(0.3)
    img_open = shell.shot("123_gradient_stops_panel")
    bright = count_bright(img_open, title_rect, 150)
    checker.check(scene, "色标表面板已绘制（右栏标题区有文字）", bright > 120,
                  "右栏浅色像素 %d" % bright)

    # 第 8 行是增删动作：Tab 7 次到它，→ 插入、← 删除。动作执行完立刻归零，
    # 所以两次按键都在改表长度；选中项跟着变，行 1..5 的文本也一起变。
    for _ in range(7):
        shell.press(VK_TAB)
    shell.press(VK_RIGHT)
    img_insert = shell.shot("124_gradient_stops_insert")
    grow = pixel_diff(img_insert, img_open, panel, 24)
    checker.check(scene, "→ 插入后右栏文本变化（选中/总数）", grow > 20,
                  "右栏变化像素 %d" % grow)
    shell.press(VK_LEFT)
    img_delete = shell.shot("125_gradient_stops_delete")
    shrink = pixel_diff(img_delete, img_insert, panel, 24)
    checker.check(scene, "← 删除后右栏文本再次变化", shrink > 20,
                  "右栏变化像素 %d" % shrink)

    # 回到第 1 行（Tab 绕圈）。删除后选中项停在索引 1（黄），→ 一次到
    # 第 3 个色标（彩虹的绿），再 Tab 到 R/G/B 三行各键入一个数，
    # 回车把整表写进「自定义」。
    shell.press(VK_TAB)
    shell.press(VK_RIGHT)
    for _ in range(3):
        shell.press(VK_TAB)
    shell.type_text("255")
    shell.press(VK_TAB)
    shell.type_text("0")
    shell.press(VK_TAB)
    shell.type_text("255")
    shell.press(VK_TAB)
    shell.press(VK_RETURN)
    shell.wait(0.3)

    shell.drag(left + 10, y, right - 10, y, steps=20)
    img = shell.shot("126_gradient_stops_applied")

    def strip_mean(fx):
        x = int(left + (right - left) * fx)
        return mean_rgb(img, (x - 6, y - 6, x + 6, y + 6))

    quarter = strip_mean(0.25)
    middle = strip_mean(0.5)
    three_quarter = strip_mean(0.75)
    checker.check(scene, "50% 处是改过的洋红（颜色真的进了落笔）",
                  near(middle, (255, 0, 255), 60), "实测 %s" % (middle,))
    checker.check(scene, "25% 处仍是彩虹的黄（表是用彩虹播种的）",
                  near(quarter, (255, 255, 0), 60), "实测 %s" % (quarter,))
    checker.check(scene, "75% 处仍是彩虹的青", near(three_quarter, (0, 255, 255), 70),
                  "实测 %s" % (three_quarter,))


def scene_layer_lock(shell, checker):
    """P1-2：图层锁 + 颜色标签（命令号 1660..1672）。

    断言分两类，全部可算：
      1. **颜色标签**是纯元数据：行首出现 7px 色块（红 / 灰），换「无」后消失；
      2. **锁**真的拦写入口：透明锁下画笔落在完全透明的背景层上 → 画布
         **逐像素不变**（restore_transparent_lock 把 alpha 还原，等于没落笔）；
         解锁后同一笔必须改像素；图像像素锁下再次不变；Ctrl+I 反相也被拒绝。

    锁本身不参与合成，所以全程画布只应在「解锁后那一笔」变化——其余每一步
    都是 0 差异，这比「状态栏说了锁上了」强得多：前者证明闸在引擎里生效，
    后者只证明壳发了命令。
    """
    scene = "layer_lock"
    layer = shell.layout["layer"]
    canvas = shell.layout["canvas"]
    doc = shell.document_rect()
    left, top, right, bottom = doc

    # 第一行（背景层）的行矩形：面板顶 + 30px 标题，行高 22（ui_core）。
    row_y = layer[1] + 30
    row_band = (layer[0], row_y, layer[2], row_y + 22)
    row_x0 = layer[0] + 1
    row_slot = (row_x0, row_y, row_x0 + 11, row_y + 22)

    shell.select_tool("brush")
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)

    base = shell.shot("127_lock_base")
    checker.check(scene, "默认无颜色标签（行首色块槽为空）",
                  count_matching(base, row_slot, (0xE0, 0x52, 0x52), 20) == 0,
                  "红色像素 %d" % count_matching(base, row_slot, (0xE0, 0x52, 0x52), 20))

    # 1. 颜色标签：红 → 灰 → 无。1672 是最后一项（灰），1666 是红，1665 是无。
    shell.command(1666)  # 图层 > 颜色标签 > 红
    red_img = shell.shot("128_color_label_red")
    red_px = count_matching(red_img, row_slot, (0xE0, 0x52, 0x52), 20)
    checker.check(scene, "颜色标签=红：行首出现红色色块", red_px >= 20, "红色像素 %d" % red_px)

    shell.command(1672)  # 灰
    gray_img = shell.shot("129_color_label_gray")
    gray_px = count_matching(gray_img, row_slot, (0x8C, 0x8C, 0x8C), 20)
    red_left = count_matching(gray_img, row_slot, (0xE0, 0x52, 0x52), 20)
    checker.check(scene, "换成灰标签：色块变色且没有红色残留",
                  gray_px >= 20 and red_left == 0,
                  "灰 %d / 红残留 %d" % (gray_px, red_left))

    shell.command(1665)  # 无
    none_img = shell.shot("130_color_label_none")
    checker.check(scene, "颜色标签=无：色块消失",
                  count_matching(none_img, row_slot, (0x8C, 0x8C, 0x8C), 20) == 0,
                  "灰色像素 %d" % count_matching(none_img, row_slot, (0x8C, 0x8C, 0x8C), 20))

    # 2. 透明锁：行内出现 T 标记，且画笔落笔对画布零影响。
    before_lock = shell.shot("131_lock_transparent_on")
    shell.command(1661)  # 锁定 > 透明像素
    locked = shell.shot("132_lock_transparent_row")
    marked = pixel_diff(locked, before_lock, row_band, 24)
    checker.check(scene, "锁定后行内多出锁标记（行文本变化）", marked > 10,
                  "行内变化像素 %d" % marked)

    x = (left + right) // 2
    y = (top + bottom) // 2
    shell.drag(x - 40, y, x + 40, y, steps=12)
    after_locked_stroke = shell.shot("133_transparent_lock_stroke")
    blocked = pixel_diff(after_locked_stroke, locked, canvas, 24)
    checker.check(scene, "透明锁下画笔改不了画布（整幅逐像素不变）", blocked == 0,
                  "画布差异像素 %d" % blocked)

    # 解锁后同一位置必须真的画上东西。
    shell.command(1660)  # 锁定 > 无
    shell.drag(x - 40, y, x + 40, y, steps=12)
    unlocked_stroke = shell.shot("134_unlocked_stroke")
    painted = pixel_diff(unlocked_stroke, after_locked_stroke, canvas, 24)
    checker.check(scene, "解锁后同一笔改变画布（闸只拦锁定期）", painted > 50,
                  "画布差异像素 %d" % painted)

    # 3. 图像像素锁：画笔与反相都被拒绝（这里验反相，画笔路径上一步已验）。
    shell.command(1662)  # 锁定 > 图像像素
    locked_pixels = shell.shot("135_pixels_lock_before_invert")
    shell.press(ord("I"), ctrl=True)  # 图层反相
    inverted = shell.shot("136_pixels_lock_invert_blocked")
    invert_delta = pixel_diff(inverted, locked_pixels, canvas, 24)
    checker.check(scene, "图像像素锁下 Ctrl+I 反相被拒绝（画布不变）", invert_delta == 0,
                  "画布差异像素 %d" % invert_delta)

    # 全部锁：同样拒绝，且行内标记变成 L（与 T/P/G 区分）。
    shell.command(1664)  # 锁定 > 全部
    all_locked = shell.shot("137_lock_all_row")
    all_mark = pixel_diff(all_locked, inverted, row_band, 24)
    checker.check(scene, "全部锁的行内标记与图像像素锁不同", all_mark > 5,
                  "行内变化像素 %d" % all_mark)


def scene_merge_visible(shell, checker):
    """P1-2 第二批：合并可见 / 反向排列（命令号 1673 / 1674）。

    合并可见的契约是「渲染不变、结构收敛」：参与合成的层并成一个，隐藏层原样
    保留。所以这里不比对颜色，而是同时断言两件事——
      1. 画布逐像素不变（隐藏层本来就不参与合成，可见层并一层后必须等价）；
      2. 面板行文本变少（层数真的少了），撤销后恢复三行。
    反向排列只动堆叠序：面板行内容变化、画布不变；再反一次逐像素回到原样。
    """
    scene = "merge_visible"
    layer = shell.layout["layer"]
    canvas = shell.layout["canvas"]
    # 面板顶 + 30px 标题；留 4 行高度，三行 / 两行都在框内。
    panel = (layer[0], layer[1] + 30, layer[2], layer[1] + 30 + 22 * 4)

    shell.command(1660)  # 解除上一场景留下的「全部锁」，否则合并可见会被引擎拒绝

    for _ in range(2):
        shell.press(ord("N"), ctrl=True, shift=True)
    # 藏掉中间那层：选中下一行后切换可见性（隐藏层不参与合成、也不会被合并）。
    shell.command(1010)
    shell.command(1011)

    before = shell.shot("138_merge_visible_before")
    texts_before = count_bright(before, panel, 150)

    shell.command(1673)  # 图层 > 合并可见
    after = shell.shot("139_merge_visible_after")
    texts_after = count_bright(after, panel, 150)
    checker.check(scene, "合并可见后面板行数变少（可见层并成一个）",
                  texts_after < texts_before,
                  "行文本亮像素 %d -> %d" % (texts_before, texts_after))
    canvas_delta = pixel_diff(after, before, canvas, 24)
    checker.check(scene, "合并可见对画布零影响（隐藏层不参与、可见层代数等价）",
                  canvas_delta == 0, "画布差异像素 %d" % canvas_delta)

    shell.press(ord("Z"), ctrl=True)
    undone = shell.shot("140_merge_visible_undo")
    texts_undone = count_bright(undone, panel, 150)
    checker.check(scene, "撤销合并可见：面板恢复三行", texts_undone >= texts_before - 5,
                  "行文本亮像素 %d（原 %d）" % (texts_undone, texts_before))

    shell.command(1674)  # 图层 > 反向排列
    flipped = shell.shot("141_reverse_rows")
    rows_delta = pixel_diff(flipped, undone, panel, 24)
    checker.check(scene, "反向排列改变面板行顺序", rows_delta > 20,
                  "面板差异像素 %d" % rows_delta)
    reverse_canvas = pixel_diff(flipped, undone, canvas, 24)
    checker.check(scene, "反向排列不动画布", reverse_canvas == 0,
                  "画布差异像素 %d" % reverse_canvas)

    shell.command(1674)
    back = shell.shot("142_reverse_back")
    panel_delta = pixel_diff(back, undone, panel, 24)
    checker.check(scene, "再反向一次回到原顺序（面板逐像素还原）", panel_delta == 0,
                  "面板差异像素 %d" % panel_delta)


def scene_layer_link(shell, checker):
    """P1-2 第三批：图层链接（命令号 1675）。

    链接是**关系元数据**：命令本身不该改一个像素。真正的判据是联动——
    背景层与它的副本在画布上完全重合，链接后**只选背景层**右移 10px：
      1. 副本跟着走（旧位置必须变成空的；没联动时副本会留在原地，两个副本
         叠在旧位置，那一带仍是橙色）；
      2. 这一步是一步撤销，撤销后画布逐像素回到原位；
      3. 解除链接后两行文本恢复原样（面板行逐像素还原）。
    行内的 `~` 标记只用来证明「壳真的把链接状态画出来了」，不与几何判据混谈。
    """
    scene = "layer_link"
    layer = shell.layout["layer"]
    canvas = shell.layout["canvas"]
    doc = shell.document_rect()
    left, top, right, bottom = doc

    row_y = layer[1] + 30
    row_band = (layer[0], row_y, layer[2], row_y + 22 * 2)
    row_x = layer[0] + 80
    top_row = row_y
    bottom_row = row_y + 22

    # 背景层画一段橙色横线，再复制一层：两层像素完全重合，肉眼分不出「副本
    # 留下了」还是「两层都走了」——所以下面必须量「旧位置是否变空」。
    shell.select_tool("brush")
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    cx = (left + right) // 2
    cy = (top + bottom) // 2
    stroke_half = max(20, min(40, (right - left) // 6))
    shell.drag(cx - stroke_half, cy, cx + stroke_half, cy, steps=10)
    shell.press(ord("J"), ctrl=True)  # 复制图层：副本与原件像素重合

    # 多选两层：先点背景行（清掉多选），再 Ctrl+点副本行。
    shell.click(row_x, bottom_row)
    shell.modified_click(row_x, top_row, ctrl=True)

    base = shell.shot("143_link_base")

    shell.command(1675)  # 图层 > 链接图层
    linked = shell.shot("144_link_linked")
    rows_delta = pixel_diff(linked, base, row_band, 24)
    checker.check(scene, "链接后两行都出现链接标记（行文本变化）", rows_delta > 20,
                  "面板行差异像素 %d" % rows_delta)
    canvas_delta = pixel_diff(linked, base, canvas, 24)
    checker.check(scene, "链接本身不改画布（纯关系元数据）", canvas_delta == 0,
                  "画布差异像素 %d" % canvas_delta)

    # 只选背景层，Ctrl+→ 移 10px：副本必须跟过来。
    shell.click(row_x, bottom_row)
    shell.press(VK_RIGHT, ctrl=True)
    moved = shell.shot("145_link_moved")

    # 取样带按实际笔画外接框算：笔画比拖拽端点两头各多出一截（笔刷半径），
    # 用常量去猜迟早差几像素、量到空白处。这里直接从像素里量「笔画到哪结束」。
    # 只看文档矩形：canvas 矩形下半部还画着快捷键提示文字，混进来会把外接框
    # 撑到整块画布。
    def stroke_box(img):
        mask = img.crop(doc).split()[0].point(lambda value: 255 if value >= 120 else 0)
        found = mask.getbbox()
        if found is None:
            return None
        return (found[0] + doc[0], found[1] + doc[1],
                found[2] + doc[0], found[3] + doc[1])

    box = stroke_box(base)
    checker.check(scene, "背景层上确有笔迹（后续取样以它为基准）", box is not None,
                  "笔迹外接框 %s" % (box,))
    moved_box = stroke_box(moved)
    checker.check(scene, "移动后仍只有一份笔迹（副本没有留在原地叠出第二份）",
                  box is not None and moved_box is not None and
                  abs((moved_box[2] - moved_box[0]) - (box[2] - box[0])) <= 2,
                  "笔迹宽度 %s -> %s" % (
                      None if box is None else box[2] - box[0],
                      None if moved_box is None else moved_box[2] - moved_box[0]))

    if box is not None and moved_box is not None and moved_box[0] > box[0]:
        # 画布是缩放显示的（这里约 49%），10 个文档像素只折合约 5 个屏幕像素：
        # 位移量直接从两帧外接框的差值量，不拿文档像素当屏幕像素用。
        shift = moved_box[0] - box[0]
        vacated = (box[0], box[1] + 2, box[0] + max(1, shift - 1), box[3] - 2)
        arrival = (box[2] + 1, box[1] + 2, box[2] + shift, box[3] - 2)
    else:
        vacated = (cx - 20, cy - 8, cx - 12, cy + 8)
        arrival = (cx + 20, cy - 8, cx + 28, cy + 8)
    vacated_before = count_bright(base, vacated, 120)
    vacated_after = count_bright(moved, vacated, 120)
    arrival_before = count_bright(base, arrival, 120)
    arrival_after = count_bright(moved, arrival, 120)
    checker.check(scene, "链接层右移后旧位置变空（副本没有留在原地）",
                  vacated_before > 0 and vacated_after == 0,
                  "橙色像素 %d -> %d" % (vacated_before, vacated_after))
    checker.check(scene, "新位置出现内容（整组跟着移动）",
                  arrival_before == 0 and arrival_after > 0,
                  "橙色像素 %d -> %d" % (arrival_before, arrival_after))

    shell.press(ord("Z"), ctrl=True)
    undone = shell.shot("146_link_undo")
    undo_delta = pixel_diff(undone, base, canvas, 24)
    checker.check(scene, "一步撤销把两层一起复位（画布逐像素还原）", undo_delta == 0,
                  "画布差异像素 %d" % undo_delta)

    # 解除链接：两行标记消失，面板行逐像素回到链接前。
    shell.click(row_x, bottom_row)
    shell.modified_click(row_x, top_row, ctrl=True)
    shell.command(1675)  # 同一命令：同组再执行一次 = 解除
    unlinked = shell.shot("147_link_unlinked")
    unlink_rows = pixel_diff(unlinked, base, row_band, 24)
    checker.check(scene, "解除链接后面板行逐像素还原", unlink_rows == 0,
                  "面板行差异像素 %d" % unlink_rows)


def scene_layer_style_paste(shell, checker):
    """P1-2 第四批：图层样式拷贝/粘贴（命令号 1676 / 1677）。

    判据不是「命令没报错」，而是画布真的换了样式：
      1. 拷贝只读：拷贝前后画布逐像素不变（剪贴板由外壳持有，不碰文档）；
      2. 供体层加颜色叠加后确实变黑；隐藏供体后基线回到橙色矩形；
      3. 粘贴到不带样式的靶层：靶层被刷成纯黑（样式参与了求值）；
      4. 整次粘贴是一步撤销：Ctrl+Z 后画布逐像素回到粘贴前。
    供体必须在拷贝之后隐藏：副本与原件像素重合，副本留在上面会盖住靶层，
    「粘贴没生效」与「粘贴被盖住」就分不出来了。
    """
    scene = "layer_style_paste"
    layer = shell.layout["layer"]
    doc = shell.document_rect()
    left, top, right, bottom = doc
    row_y = layer[1] + 30
    row_x = layer[0] + 80
    top_row = row_y
    bottom_row = row_y + 22

    # 底：橙色矩形。颜色叠加的缺省值是不透明纯黑，橙 -> 黑是最干净的判据。
    shell.press(ord("X"), shift=True)
    shell.type_text("ff8800")
    shell.press(VK_RETURN)
    shell.select_tool("shape")
    x0, y0 = left + 20, top + 20
    x1, y1 = right - 20, bottom - 20
    shell.drag(x0, y0, x1, y1, steps=10)

    # 复制一层（副本在上、像素与原件重合），给副本加颜色叠加当样式供体。
    shell.press(ord("J"), ctrl=True)
    shell.command(CMD_EFFECT_BASE + 2)  # 颜色叠加（默认不透明纯黑）
    shell.wait(0.6)
    shell.press(VK_ESCAPE)  # 只关参数面板，效果留在图层上
    shell.wait(0.3)

    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    probe = (cx - 30, cy - 30, cx + 30, cy + 30)
    styled = shell.shot("148_style_donor_black")
    donor = mean_rgb(styled, probe)
    checker.check(scene, "供体层加颜色叠加后变纯黑", near(donor, (0, 0, 0), 24),
                  "实测 %s" % (donor,))

    # 选中供体行，拷贝样式。
    shell.click(row_x, top_row)
    shell.command(1676)  # 图层 > 图层样式 > 拷贝图层样式
    copied = shell.shot("149_style_copied")
    copy_delta = pixel_diff(copied, styled, doc, 24)
    checker.check(scene, "拷贝样式不改画布（只读导出）", copy_delta == 0,
                  "画布差异像素 %d" % copy_delta)

    # 藏掉供体，露出下面的橙色原件：这才是粘贴前的基线。
    shell.command(1011)  # 切换选中层可见性（供体行仍选中）
    hidden = shell.shot("150_style_donor_hidden")
    base = mean_rgb(hidden, probe)
    checker.check(scene, "隐藏供体后画布回到橙色矩形", near(base, (255, 136, 0), 30),
                  "实测 %s" % (base,))

    # 选中靶层（下面那行），粘贴样式。
    shell.click(row_x, bottom_row)
    shell.command(1677)  # 图层 > 图层样式 > 粘贴图层样式
    shell.wait(0.3)
    pasted = shell.shot("151_style_pasted")
    target = mean_rgb(pasted, probe)
    checker.check(scene, "粘贴后靶层被刷成纯黑（样式真的参与求值）",
                  near(target, (0, 0, 0), 24), "实测 %s" % (target,))

    # 一步撤销：粘贴整步回退，画布逐像素回到粘贴前。
    shell.press(ord("Z"), ctrl=True)
    undone = shell.shot("152_style_paste_undo")
    undo_delta = pixel_diff(undone, hidden, doc, 24)
    checker.check(scene, "一步撤销把粘贴整步回退（画布逐像素还原）", undo_delta == 0,
                  "画布差异像素 %d" % undo_delta)


SCENES = [    ("start", scene_start, ()),
    ("brush", scene_brush, ()),
    ("selection", scene_selection, ()),
    ("wand", scene_wand, ()),
    ("gradient", scene_gradient, ()),
    ("shape", scene_shape, ()),
    ("type", scene_type, ()),
    ("ime", scene_ime, ()),
    ("filter", scene_filter, ()),
    ("sharpen_denoise", scene_sharpen_denoise, ()),
    ("adjustment", scene_adjustment, ()),
    ("curve_levels", scene_curve_levels, ()),
    ("layers", scene_layers, ()),
    ("blend", scene_blend, ()),
    ("xform", scene_xform, ()),
    ("view", scene_view, ()),
    ("image_io", scene_image_io, ("import_probe.png",)),
    ("crop", scene_crop, ()),
    ("doc_dialog", scene_doc_dialog, ()),
    ("camera_raw", scene_camera_raw, ()),
    ("clipboard", scene_clipboard, ()),
    ("close_prompt", scene_close_prompt, ()),
    ("options_bar", scene_options_bar, ()),
    ("selection_modify", scene_selection_modify, ()),
    ("selection_grow", scene_selection_grow, ()),
    ("text_layer", scene_text_layer, ()),
    ("font_picker", scene_font_picker, ()),
    ("guides", scene_guides, ()),
    ("destructive_adjust", scene_destructive_adjust, ()),
    ("brush_options", scene_brush_options, ()),
    ("selection_transform", scene_selection_transform, ()),
    ("layer_transform_numeric", scene_layer_transform_numeric, ()),
    ("effect_pattern", scene_effect_pattern, ()),
    ("gradient_stops", scene_gradient_stops, ()),
    ("layer_lock", scene_layer_lock, ()),
    ("merge_visible", scene_merge_visible, ()),
    ("layer_link", scene_layer_link, ()),
    ("layer_style_paste", scene_layer_style_paste, ()),
    ("filter_other", scene_filter_other, ()),
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
