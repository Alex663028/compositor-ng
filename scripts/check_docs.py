# AI生成
#!/usr/bin/env python3
"""check_docs.py — OPT-27 最小版：文档关键数字 vs 实测校验。

背景：同一体系里曾并存 112/167/211 三个导出数、710/263 与 778/267 两套基线
（README 首页与 BACKLOG §1 停在旧值）。本脚本不猜语义、只做「文档声称的数字
必须等于实测」的机械校验：
  1. ^COMP_API 计数（abi/include/compositor/*.h）必须与 BACKLOG 页眉「当前 N」一致；
  2. 壳自检基线 [OK] 计数（compositor_shell_selftest.txt）必须与 BACKLOG 页眉「N 项」一致；
  3. README 首页摘要的「N 用例」须与最近一次全量日志（build/ 下 summary 行）一致（有日志才查）。
用法：python scripts/check_docs.py   （rc=0 全绿；rc=1 有失同步，逐条打印）
"""
from __future__ import annotations

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures: list[str] = []


def check(label: str, claimed, actual) -> None:
    status = "PASS" if claimed == actual else "FAIL"
    if status == "FAIL":
        failures.append(f"{label}: 文档={claimed} 实测={actual}")
    print(f"  [{status}] {label}: 文档={claimed} 实测={actual}")


# 1. ABI 导出数
api_count = 0
for header in glob.glob(os.path.join(ROOT, "engine", "abi", "include", "compositor", "*.h")):
    with open(header, "r", encoding="utf-8", errors="replace") as f:
        api_count += sum(1 for line in f if line.startswith("COMP_API"))
backlog = open(os.path.join(ROOT, "BACKLOG.md"), encoding="utf-8").read()
m = re.search(r"尾部追加\*\*（当前\s*\*\*(\d+)", backlog)
if m:
    check("ABI 导出数（BACKLOG 页眉）", int(m.group(1)), api_count)
else:
    failures.append("BACKLOG 页眉未找到「当前 N」导出数声明")

# 2. 壳自检项数
selftest = open(os.path.join(ROOT, "compositor_shell_selftest.txt"), encoding="utf-8",
                errors="replace").read()
ok_count = selftest.count("[OK]")
m2 = re.search(r"壳?自检[^\n]*?(\d+)\s*项", backlog)
if m2:
    check("壳自检项数（BACKLOG）", int(m2.group(1)), ok_count)

# 3. README 首页用例数 vs 最近构建日志
readme_head = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()[:6000]
m3 = re.search(r"(\d+)\s*用例\s*0?\s*[失例]", readme_head)
logs = sorted(glob.glob(os.path.join(ROOT, "..", "_logs", "*.log")),
              key=os.path.getmtime, reverse=True)
recent_cases = None
for log in logs[:10]:
    with open(log, "r", encoding="utf-8", errors="replace") as f:
        mm = re.search(r"summary: (\d+) cases, 0 failed", f.read())
        if mm:
            recent_cases = int(mm.group(1))
            break
if m3 and recent_cases:
    check("README 首页用例数（最近日志）", int(m3.group(1)), recent_cases)

print()
if failures:
    print("check_docs: FAIL")
    for line in failures:
        print("  -", line)
    sys.exit(1)
print("check_docs: PASS")
