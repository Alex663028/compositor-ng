#!/usr/bin/env python3
"""工具表漂移守卫：scripts/visual_check.py 的 TOOL 表必须与
shells/windows/src/ui_core.h::tool_table() 的顺序逐项一致。

缘由：P1 的 cfd708d 在工具表 index 13 插入 Tone（减淡/加深/海绵）后，
visual_check.py 的 TOOL 表没同步，WM_COMMAND 命令号整体错位 1——脚本以为
在选 Gradient，实际选中了 Dodge，6 个场景确定性假失败，产品无辜背锅。
本守卫把「脚本的工具表」钉在单一真相源 ui_core.h 上：工具表顺序一变而
脚本忘了改，这里先炸，不用等视觉探针出谜语。

用法：python scripts/check_tool_table.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_PATH = os.path.join(ROOT, "shells", "windows", "src", "ui_core.h")
VC_PATH = os.path.join(ROOT, "scripts", "visual_check.py")

# ui_core.h 的 ToolId 与 visual_check.py 的 TOOL 键唯一不一致的一处
ALIAS = {"history": "historybrush"}


def ui_tools():
    """ui_core.h kTools[] 的 ToolId，按下标顺序。"""
    with open(UI_PATH, encoding="utf-8") as fh:
        text = fh.read()
    block = re.search(r"static const ToolInfo kTools\[\] = \{(.*?)\n  \};",
                      text, re.S)
    if block is None:
        sys.exit("找不到 kTools[]：ui_core.h 结构变了，先更新本脚本")
    return [tid.lower() for tid in re.findall(r"\{ToolId::(\w+),", block.group(1))]


def vc_tools():
    """visual_check.py TOOL 表的 (键, 下标)，按下标顺序。"""
    with open(VC_PATH, encoding="utf-8") as fh:
        text = fh.read()
    block = re.search(r"^TOOL = \{(.*?)^\}", text, re.M | re.S)
    if block is None:
        sys.exit("找不到 TOOL 表：visual_check.py 结构变了，先更新本脚本")
    pairs = re.findall(r'"(\w+)"\s*:\s*(\d+)', block.group(1))
    return [(name, int(idx)) for name, idx in pairs]


def main():
    ui = ui_tools()
    vc = vc_tools()
    problems = []
    if len(ui) != len(vc):
        problems.append("工具数不一致：ui_core.h %d 个，TOOL 表 %d 个" %
                        (len(ui), len(vc)))
    for position, (name, idx) in enumerate(vc):
        canon = ALIAS.get(name, name)
        if idx != position:
            problems.append("TOOL[%r] = %d（应为下标 %d）" % (name, idx, position))
        if position < len(ui) and canon != ui[position]:
            problems.append("位置 %d：TOOL 键 %r 对应 ui_core.h 的 %r" %
                            (position, name, ui[position]))
    if problems:
        print("FAIL: visual_check.py 的 TOOL 表与 ui_core.h 不一致")
        for line in problems:
            print("  - " + line)
        return 1
    print("PASS: TOOL 表与 ui_core.h 一致（%d 个工具，下标 0..%d）" %
          (len(vc), len(vc) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
