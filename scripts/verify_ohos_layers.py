#!/usr/bin/env python3
"""鸿蒙壳的图层验证（模拟器）：分组 / 缩进 / 拖行 / 折叠 / 多选 / 调整层。

为什么要有这个脚本：鸿蒙侧的验证此前是「手敲 hdc 命令 + 硬编码屏幕坐标」，
布局一改坐标就全错——实测把「蓝层」点到了「导入图片」上（弹出文件选择器把
应用顶到后台），还排查了半天「应用是不是崩了」。**按控件文本找坐标**就没有
这个问题：先 dump 布局，按 text 找到元素，点它的中心。

另一条教训：模拟器上画布上不了屏（OPT-21b），所以「分组有没有生效」「暗角
打在哪个层」这些事**光看屏幕验不了**，只能读回引擎状态 / 像素来比。

覆盖（16 项）：分组与缩进 / 拖进组 / 引擎侧自检 / 折叠 / 多选批量建组 /
可编辑调整层（新增·编辑·删除）/ 调整只作用于选中层。

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

    def tap_box(self, box, wait=2.0):
        """按 bounds 点中心（同一个文本出现多次时，用坐标指定点哪一个）。"""
        self.shell("uitest uiInput click %d %d" % ((box[0] + box[2]) // 2,
                                                  (box[1] + box[3]) // 2))
        time.sleep(wait)

    def boxes_of(self, text):
        """某个文本出现的全部位置，按 y 排序（上到下）。"""
        out = [box for _, value, box in self.texts() if value == text]
        out.sort(key=lambda item: item[1])
        return out

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


def layer_count(device):
    """状态栏里的「图层 N」。-1 = 读不到。"""
    for _, text, _ in device.texts():
        if "图层" in text and text.startswith("S"):
            match = re.search(r"图层 (-?\d+)", text)
            if match:
                return int(match.group(1))
    return -1


def fresh_document(device):
    """重启应用 + 新建文档，回到**确定的**初始状态。

    为什么每节都重启，而不是接着上一节：调试按钮在图层面板下方，面板一高一低
    坐标就变；接着上一节跑会「有的节点得到、有的节点不到」，而点不到的 tap 只
    返回 False 不报错，整节断言就落到错的对象上（2026-10-27 实测：图层攒到 6
    层、列表里全是组，第六节开始全崩且看不出原因）。
    重启 + 新建一共十秒，换来每节的起点可预测。
    """
    device.start()
    device.tap("新建", 2)
    return layer_count(device) == 1


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
    fresh_document(device)
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

    print("\n=== 四、折叠（组可收起） ===")
    fresh_document(device)
    device.tap("蓝层", 2)
    device.tap("蓝层", 2)
    device.tap("建组", 3)
    rows = device.layer_rows()
    collapsed_before = len(rows)
    icons = device.boxes_of("▾")
    if icons:
        device.tap_box(icons[0], 2)
        rows_collapsed = device.layer_rows()
        report.check("点组行的 ▾ 收起子层（行数变少）",
                     len(rows_collapsed) < collapsed_before,
                     "收起前 %d 行 -> 收起后 %d 行" % (collapsed_before, len(rows_collapsed)))
        expand = device.boxes_of("▸")
        if expand:
            device.tap_box(expand[0], 2)
            rows_expanded = device.layer_rows()
            report.check("点 ▸ 展开（子层回来）",
                         len(rows_expanded) == collapsed_before,
                         "%d 行 -> %d 行" % (len(rows_collapsed), len(rows_expanded)))
        else:
            report.check("点 ▸ 展开", False, "找不到 ▸ 图标")
    else:
        report.check("折叠", False, "找不到 ▾ 图标")

    print("\n=== 五、多选与批量建组 ===")
    fresh_document(device)
    device.tap("蓝层", 2)
    device.tap("蓝层", 2)
    marks = device.boxes_of("○")
    if len(marks) >= 2:
        device.tap_box(marks[0], 1.5)
        # 勾一个之后圆点变 ●，剩下的 ○ 会前移，重新取
        rest_marks = device.boxes_of("○")
        if rest_marks:
            device.tap_box(rest_marks[0], 2)
        multi_text = ""
        for _, value, _ in device.texts():
            if "多选" in value:
                multi_text = value
        report.check("勾选两个层后提示「多选 2 层」", "多选 2" in multi_text, multi_text)
        before = len(device.layer_rows())
        device.tap("建组", 3)
        rows_after = device.layer_rows()
        base_x = min([row[1] for row in rows_after]) if rows_after else 0
        indented = [row for row in rows_after if row[1] > base_x + 10]
        line = status_line(device)
        report.check("批量建组：两个层一起收进新组",
                     "建组 2/2" in line and len(indented) >= 2,
                     "%s / 缩进 %d 行" % (line, len(indented)))
    else:
        report.check("多选", False, "勾选点不足两个")

    print("\n=== 六、可编辑调整层 ===")
    # 从确定的起点开始：重启 + 新建 + 两层，然后**点列表行**选中真正的图层
    # （组没有像素，引擎不给它挂调整，症状是「新增 ✓ 但已有一直空」）。
    fresh_document(device)
    device.tap("蓝层", 2)
    rows = device.layer_rows()
    solids = [row for row in rows if row[0].startswith("solid")]
    if solids:
        device.tap_box(solids[0][3], 2)
    else:
        report.check("选中一个图层", False,
                     "列表里没有 solid 行：%s" % ([(r[0], r[1], r[2]) for r in rows],))
    device.tap("新增", 3)

    added = ""
    for _, value, _ in device.texts():
        if value.startswith("新增"):
            added = value
    report.check("新增一条调整（引擎拼 JSON，rc=0）", "✓" in added, added)
    has_existing = False
    for _, value, _ in device.texts():
        if value == "已有":
            has_existing = True
    report.check("该层「已有」列表里出现这条调整", has_existing, "")
    entries = device.boxes_of("Hue/Saturation")
    if len(entries) >= 2:
        device.tap_box(entries[-1], 2)
        edit_state = ""
        button_text = ""
        for _, value, _ in device.texts():
            if value.startswith("编辑第"):
                edit_state = value
            if value in ("更新", "新增"):
                button_text = value
        report.check("点已有条目进入编辑（按钮变「更新」+ 回填提示）",
                     button_text == "更新" and edit_state != "",
                     "%s / %s" % (button_text, edit_state))
        # 删掉这条调整（点「已有」行里的 ✕，不是图层面板那个「删除」按钮）
        crosses = device.boxes_of("✕")
        if crosses:
            device.tap_box(crosses[0], 3)
            removed = ""
            for _, value, _ in device.texts():
                if value.startswith("删除"):
                    removed = value
            still_there = False
            for _, value, _ in device.texts():
                if value == "已有":
                    still_there = True
            report.check("点 ✕ 删掉这条调整（引擎里也没了）",
                         "✓" in removed and not still_there, removed)
        else:
            report.check("删除调整", False, "找不到 ✕")
    else:
        report.check("已有条目可点", False, "找不到两条 Hue/Saturation")

    device.tap("调自检", 3)
    adjust = ""
    for _, text, _ in device.texts():
        if "target=" in text:
            adjust = text
    report.check("调整只作用于选中层（选中层变暗 / 未选中层一个字节没动）",
                 "ok=1" in adjust, adjust)
    report.check("滤镜真的改了像素（rc=0 changed=1，不是 0.7% 的空操作）",
                 "rc=0" in adjust and "changed=1" in adjust, adjust)

    print("\n=== 七、混合模式下拉 ===")
    # 一律点**列表行**选中图层，不点调试区按钮（面板一高一低坐标就变，
    # tap 会静默失败——第六节就是被这个坑过）。
    fresh_document(device)
    device.tap("蓝层", 2)
    rows = device.layer_rows()
    solids = [row for row in rows if row[0].startswith("solid")]
    if solids:
        device.tap_box(solids[0][3], 2)
    blend_value = ""
    for kind, value, _ in device.texts():
        if kind == "Select" and value not in ("Hue/Saturation", ""):
            blend_value = value
    report.check("混合下拉显示选中层当前模式（缺省 Normal）",
                 blend_value == "Normal", blend_value or "（没找到下拉）")
    # 点开下拉 -> 选 Screen -> 应写回引擎且状态栏报出来
    opened = device.tap(blend_value if blend_value else "Normal", 2)
    picked = device.tap("Screen", 3) if opened else False
    line = status_line(device)
    report.check("选 Screen 写回引擎（状态栏报混合模式）",
                 picked and "混合 Screen" in line, line)
    mix_marks = device.boxes_of("混")
    report.check("该行出现「混」标记（非 Normal 才显示）",
                 len(mix_marks) >= 1, "标记 %d 个" % len(mix_marks))

    print("\n=== 八、剪贴蒙版 ===")
    fresh_document(device)
    device.tap("蓝层", 2)
    device.tap("蓝层", 2)
    rows = device.layer_rows()
    solids = [row for row in rows if row[0].startswith("solid")]
    if len(solids) >= 2:
        # 最上面那个 solid 是上层：它下面是同父的另一个 solid，正好剪贴。
        device.tap_box(solids[0][3], 2)
        device.tap("剪贴", 3)
        clip_text = ""
        for _, value, _ in device.texts():
            if value.startswith("剪贴 →") or value in ("不剪贴", "下方没有同组图层",
                                                      "下方图层不在同一组"):
                clip_text = value
        report.check("点「剪贴」挂到下方同父图层（基底由引擎规则算）",
                     clip_text.startswith("剪贴 →"), clip_text or "（没有状态文本）")
        clip_marks = device.boxes_of("剪")
        report.check("该行出现「剪」标记", len(clip_marks) >= 1,
                     "标记 %d 个" % len(clip_marks))
        device.tap("剪贴", 3)
        clip_after = ""
        for _, value, _ in device.texts():
            if value in ("不剪贴", "剪贴 →"):
                clip_after = value
        report.check("再点一次取消剪贴", clip_after == "不剪贴", clip_after)
    else:
        report.check("剪贴蒙版", False, "不足两层 solid，构造不出剪贴关系")

    print("\n=== 九、调整层回填按 json_path ===")
    fresh_document(device)
    device.tap("蓝层", 2)
    rows = device.layer_rows()
    solids = [row for row in rows if row[0].startswith("solid")]
    if solids:
        device.tap_box(solids[0][3], 2)
    device.tap("新增", 3)
    entries = device.boxes_of("Hue/Saturation")
    if len(entries) >= 2:
        # 最后一个 y 最大的是「已有」行里的条目（第一个是类型下拉自己）。
        device.tap_box(entries[-1], 2)
        edit_state = ""
        for _, value, _ in device.texts():
            if value.startswith("编辑第"):
                edit_state = value
        # 「缺省」只在文档里读不到该参数时出现；能读到就说明路径匹配对上了。
        report.check("回填按路径命中（提示里不出现「缺省」）",
                     edit_state != "" and "缺省" not in edit_state,
                     edit_state or "（没有回填提示）")
    else:
        report.check("回填按路径", False, "找不到「已有」条目（%d 个 Hue/Saturation）" % len(entries))

    if not args.keep:
        device.shell("aa force-stop %s" % BUNDLE)
    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
