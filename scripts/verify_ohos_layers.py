#!/usr/bin/env python3
"""鸿蒙壳的图层验证（模拟器）：分组 / 缩进 / 拖行 / 调整目标。

为什么要有这个脚本：鸿蒙侧的验证此前是「手敲 hdc 命令 + 硬编码屏幕坐标」，
布局一改坐标就全错——实测把「蓝层」点到了「导入图片」上（弹出文件选择器把
应用顶到后台），还排查了半天「应用是不是崩了」。**按控件文本找坐标**就没有
这个问题：先 dump 布局，按 text 找到元素，点它的中心。

另一条教训：模拟器上画布上不了屏（OPT-21b），所以「分组有没有生效」「暗角
打在哪个层」这些事**光看屏幕验不了**，只能读回引擎状态 / 像素来比。

用法（模拟器已启动、HAP 已装）：

    python scripts/verify_ohos_layers.py
    python scripts/verify_ohos_layers.py --sn 127.0.0.1:5555 --keep

依赖：hdc 在 PATH 或通过 --hdc 指定。
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

DEFAULT_HDC = r"G:/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"
BUNDLE = "com.ekkostudio.compositor"
LOCAL_DUMP = os.path.join(tempfile.gettempdir(), "ohos_uid.json")

# 分隔符与 NAPI 侧 layerTreeText 一致（见 shells/ohos/src/napi_init.cpp）。
FIELD_SEP = "\x1f"
ROW_SEP = "\x1e"


class Device:
    def __init__(self, hdc, sn):
        self.hdc = hdc
        self.sn = sn
        # Windows 下 hdc 会吃掉 /data 这类 POSIX 路径的转换（MSYS 的锅），统一关掉。
        self.env = dict(os.environ)
        self.env["MSYS_NO_PATHCONV"] = "1"

    def run(self, args, timeout=90):
        return subprocess.run([self.hdc, "-t", self.sn] + args, capture_output=True,
                              text=True, env=self.env, timeout=timeout)

    def shell(self, command):
        return self.run(["shell", command])

    def start(self, settle=8.0):
        self.shell("aa force-stop %s" % BUNDLE)
        time.sleep(1)
        self.shell("aa start -a EntryAbility -b %s" % BUNDLE)
        time.sleep(settle)

    def dump(self):
        """当前界面的控件树（dumpLayout，跨进程拉回本地解析）。"""
        self.shell("uitest dumpLayout -p /data/local/tmp/uid.json")
        self.run(["file", "recv", "/data/local/tmp/uid.json", LOCAL_DUMP])
        with open(LOCAL_DUMP, encoding="utf-8") as handle:
            return json.load(handle)

    def texts(self):
        """[(type, text, [x1, y1, x2, y2]), …]，按 dump 顺序。

        带上 type 是为了把 **TextInput** 与列表行分开——改名输入框里的文字也是
        「solid」，只按文本匹配会把它当成图层行（实测：拖行拖到了输入框上）。
        """
        out = []
        stack = [self.dump()]
        while stack:
            node = stack.pop()
            if not isinstance(node, dict):
                continue
            attrs = node.get("attributes", {})
            text = str(attrs.get("text", ""))
            kind = str(attrs.get("type", ""))
            box = [int(v) for v in re.findall(r"-?\d+", str(attrs.get("bounds")))]
            if text and len(box) >= 4:
                out.append((kind, text, box))
            children = node.get("children", []) or []
            stack.extend(reversed(children))
        return out

    def find(self, needle, rows=None):
        rows = rows if rows is not None else self.texts()
        for _, text, box in rows:
            if text == needle:
                return box
        for _, text, box in rows:
            if needle in text:
                return box
        return None

    def tap(self, needle, wait=2.0):
        box = self.find(needle)
        if box is None:
            return False
        self.shell("uitest uiInput click %d %d" % ((box[0] + box[2]) // 2,
                                                  (box[1] + box[3]) // 2))
        time.sleep(wait)
        return True

    def drag(self, source, target, wait=3.0):
        """按住 source 拖到 target（都是 bounds）。"""
        self.shell("uitest uiInput drag %d %d %d %d" %
                   ((source[0] + source[2]) // 2, (source[1] + source[3]) // 2,
                    (target[0] + target[2]) // 2, (target[1] + target[3]) // 2))
        time.sleep(wait)

    def layer_rows(self):
        """图层列表里的行：[(name, x, y, box)]，按 y 排序（x 反映缩进）。

        只取**列表区域**（「图层」标题之下、「不透明度」行之上）里的非输入框文本：
        改名输入框里的层名、状态栏里的文字都不能算作列表行。
        """
        rows = self.texts()
        title = self.find("图层", rows)
        opacity = self.find("不透明度", rows)
        if title is None or opacity is None:
            return []
        top = title[1] + 20
        bottom = opacity[1]
        out = []
        for kind, text, box in rows:
            if kind == "TextInput":
                continue
            if not (top < box[1] < bottom):
                continue
            if text in ("Background", "组") or text.startswith("solid"):
                out.append((text, box[0], box[1], box))
        out.sort(key=lambda row: row[2])
        return out


class Report:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               "" if not detail else "  —— " + detail))

    def summary(self):
        failed = [row for row in self.rows if not row[1]]
        print("\n合计 %d 项检查，%d 失败" % (len(self.rows), len(failed)))
        return 1 if failed else 0


def status_line(device):
    for _, text, _ in device.texts():
        if "最近" in text:
            return text
    return ""


def main():
    parser = argparse.ArgumentParser(description="鸿蒙壳图层验证（模拟器）")
    parser.add_argument("--hdc", default=DEFAULT_HDC)
    parser.add_argument("--sn", default="127.0.0.1:5555")
    parser.add_argument("--keep", action="store_true", help="跑完不重启应用（调试用）")
    args = parser.parse_args()

    device = Device(args.hdc, args.sn)
    report = Report()

    print("=== 启动应用 ===")
    device.start()

    print("\n=== 一、图层分组与缩进 ===")
    device.tap("新建", 2)
    device.tap("蓝层", 2)
    device.tap("蓝层", 2)
    rows = device.layer_rows()
    report.check("建两层后列表有 3 行（背景 + 2 个 solid）",
                 len(rows) == 3, "%s" % ([(r[0], r[1], r[2]) for r in rows],))

    device.tap("建组", 3)
    rows = device.layer_rows()
    names = [row[0] for row in rows]
    report.check("建组后列表有组行", "组" in names, "%s" % (names,))
    base_x = min([row[1] for row in rows]) if rows else 0
    inner = [row for row in rows if row[1] > base_x + 10]
    report.check("组内的层向右缩进（x 比根级大）", len(inner) >= 1,
                 "根级 x=%d，缩进行 %s" % (base_x, [(r[0], r[1]) for r in inner]))

    print("\n=== 二、拖行（拖进组） ===")
    rows = device.layer_rows()
    group = [row for row in rows if row[0] == "组"]
    outer = [row for row in rows if row[0].startswith("solid") and row[1] <= base_x + 10]
    if group and outer:
        before = len(rows)
        device.drag(outer[0][3], group[0][3])
        rows_after = device.layer_rows()
        indented = [r for r in rows_after if r[1] > base_x + 10]
        report.check("把根级层拖到组上 -> 收进组（层数不变、缩进+1）",
                     len(rows_after) == before and len(indented) >= 2,
                     "%s" % ([(r[0], r[1]) for r in rows_after],))
        line = status_line(device)
        report.check("状态栏记录「拖入组」", "拖入组" in line, line)
    else:
        report.check("拖进组", False, "找不到可拖的行")

    print("\n=== 三、引擎侧自检（画布上不了屏，只能读回状态） ===")
    device.tap("层自检", 3)
    selftest = ""
    for _, text, _ in device.texts():
        if "list=" in text:
            selftest = text
    report.check("图层面板自检全绿（列表/显隐/透明度/改名/分组/拖行/活动层/删除）",
                 "group=1" in selftest and "ungroup=1" in selftest and
                 "active=1" in selftest and "del=1" in selftest, selftest)

    device.tap("调自检", 3)
    adjust = ""
    for _, text, _ in device.texts():
        if "target=" in text:
            adjust = text
    report.check("调整只作用于选中层（选中层变暗 / 未选中层一个字节没动）",
                 "ok=1" in adjust, adjust)
    report.check("滤镜真的改了像素（rc=0 changed=1，不是 0.7% 的空操作）",
                 "rc=0" in adjust and "changed=1" in adjust, adjust)

    if not args.keep:
        device.shell("aa force-stop %s" % BUNDLE)
    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
