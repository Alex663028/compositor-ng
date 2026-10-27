# AI生成
#!/usr/bin/env python3
"""check_docs.py — 文档关键数字 vs 实测校验。

背景：同一体系里曾并存 112/167/211 三个导出数、710/263 与 778/267 两套基线
（README 首页与 BACKLOG §1 停在旧值）；2026-10-27 又发现视觉探针条数与
`docs/ps-parity.md` 的加权分都在文档里对不上实测。本脚本不猜语义、只做
「文档声称的数字必须等于实测/自身一致」的机械校验：

  [FAIL 级]
  1. ^COMP_API 计数（abi/include/compositor/*.h）== BACKLOG 页眉「当前 **N**」；
  2. 壳自检 [OK] 计数（compositor_shell_selftest.txt）== BACKLOG 页眉「自检基线：**N 项**」；
  3. README 首页「N 用例」== 最近一次全量日志的 summary 行（有日志才查）；
  4. 视觉探针：SCENES 注册数 == README「M 场景」；断言条数（含循环内重复执行）==「N 项」；
  5. ps-parity 口径 D/C：表格九维加权值 == 该节「算术」行给出的百分比；
  6. ps-parity 算术行里的每一对「0.WW×VV」必须与表格同一行的（权重, 得分）逐项相等
     —— 这是 2026-10-27 那类「结论改了、算式没改」的直接拦截点。

  [WARN 级]
  7. ps-parity 头部声明的证据基准 HEAD 是否等于当前 HEAD（落后通常是忘了重算，
     不阻断，但要看得见）。

用法：python scripts/check_docs.py   （rc=0 全绿；rc=1 有失同步，逐条打印）
"""
from __future__ import annotations

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures: list[str] = []
warnings: list[str] = []


def check(label: str, claimed, actual) -> None:
    status = "PASS" if claimed == actual else "FAIL"
    if status == "FAIL":
        failures.append(f"{label}: 文档={claimed} 实测={actual}")
    print(f"  [{status}] {label}: 文档={claimed} 实测={actual}")


def warn(label: str, detail: str) -> None:
    warnings.append(f"{label}: {detail}")
    print(f"  [WARN] {label}: {detail}")


def read(rel: str) -> str:
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


# ---------------------------------------------------------------- 1. ABI 导出数
print("[1] ABI 导出数")
api_count = 0
for header in glob.glob(os.path.join(ROOT, "engine", "abi", "include", "compositor", "*.h")):
    with open(header, "r", encoding="utf-8", errors="replace") as f:
        api_count += sum(1 for line in f if line.startswith("COMP_API"))
backlog = read("BACKLOG.md")
m = re.search(r"当前\s*\*\*(\d+)\*\*", backlog)
if m:
    check("ABI 导出数（BACKLOG 页眉）", int(m.group(1)), api_count)
else:
    failures.append("BACKLOG 页眉未找到「当前 N」导出数声明")

# ------------------------------------------------------------- 2. 壳自检项数
print("[2] 壳自检项数")
selftest = read("compositor_shell_selftest.txt")
ok_count = selftest.count("[OK]")
m2 = re.search(r"自检基线：\s*\*\*(\d+)\s*项", backlog)
if m2:
    check("壳自检项数（BACKLOG 页眉）", int(m2.group(1)), ok_count)
else:
    failures.append("BACKLOG 页眉未找到「自检基线：**N 项**」声明")

# --------------------------------------------------- 3. README 首页用例数
print("[3] README 首页用例数")
readme = read("README.md")
head = readme[:8000]
m3 = re.search(r"(\d+)\s*用例\s*0?\s*[失例]", head)
if not m3:
    m3 = re.search(r"全量\s*\*\*(\d+)\s*用例", head)
recent_cases = None
logs = sorted(glob.glob(os.path.join(ROOT, "..", "_logs", "*.log")),
              key=os.path.getmtime, reverse=True)
logs += sorted(glob.glob(os.path.join(ROOT, "build", "*.log")),
               key=os.path.getmtime, reverse=True)
for log in logs[:10]:
    if not os.path.isfile(log):
        continue
    with open(log, "r", encoding="utf-8", errors="replace") as f:
        mm = re.search(r"summary: (\d+) cases, 0 failed", f.read())
        if mm:
            recent_cases = int(mm.group(1))
            break
if m3 and recent_cases:
    check("README 首页用例数（最近日志）", int(m3.group(1)), recent_cases)
elif m3:
    # 没有构建日志可对照时，退回静态用例计数（这也能拦住「文档停留在旧值」）。
    static_cases = 0
    for src in glob.glob(os.path.join(ROOT, "tests", "*.cpp")):
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            static_cases += len(re.findall(r"\bCOMP_TEST\s*\(", f.read()))
    check("README 首页用例数（静态 COMP_TEST 计数）", int(m3.group(1)), static_cases)

# ------------------------------------------------------- 4. 视觉探针条数/场景
print("[4] 视觉探针条数 / 场景数")
vp = read(os.path.join("scripts", "visual_check.py"))
scenes_block = None
if "SCENES = [" in vp:
    i = vp.index("SCENES = [")
    scenes_block = vp[i:vp.index("]", i)]
registered = len(re.findall(r'\(\s*"([a-z0-9_]+)"', scenes_block)) if scenes_block else 0


def probe_assert_count(src: str) -> int:
    """数断言**执行**次数：`.check(` 落在 for 循环体内时会执行多次。

    用 AST 逐层记录祖先循环，对每个 `.check(` 调用点求「外层循环迭代次数」之积：
      - `for _ in range(N)` → N；
      - `for a, b, c in ((..), (..), (..))` → 元组字面量元素数；
      - 其它（遍历变量/生成式）→ 未知，按 1 计并在返回值里体现（宁可少算不虚增）。

    这是 2026-10-27 那次「代码里 283 处 check、文档写 284 条」的化解方式：
    第 1842 行那个三元组循环让同一条断言跑 3 次，真值是 285。
    """
    import ast

    def loop_iters(node) -> int:
        it = node.iter
        if isinstance(it, ast.Call) and getattr(it.func, "id", "") == "range":
            if it.args and isinstance(it.args[0], ast.Constant) and isinstance(it.args[0].value, int):
                return max(it.args[0].value, 1)
            return 1
        if isinstance(it, (ast.Tuple, ast.List, ast.Set)):
            return max(len(it.elts), 1)
        return 1

    def count(node, mult: int) -> int:
        """返回 node 子树在该倍率下的**执行**断言数。

        if/else 互斥：两条分支各自统计后取**较大者**（运行时只会走一条，
        取大者是对「哪条会走」的保守上界；分支数相同时与实测一致）。
        """
        total = 0
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.For, ast.AsyncFor)):
                total += count(child, mult * loop_iters(child))
            elif isinstance(child, ast.While):
                total += count(child, mult)  # 条件循环：次数不可静态判定，按 1 计
            elif isinstance(child, ast.If):
                body = sum(count(st, mult) for st in child.body)
                orelse = sum(count(st, mult) for st in child.orelse)
                # 分支自身的 .check 直接调用
                body += sum(mult for st in child.body
                            if isinstance(st, ast.Call) and isinstance(st.func, ast.Attribute)
                            and st.func.attr == "check")
                orelse += sum(mult for st in child.orelse
                              if isinstance(st, ast.Call) and isinstance(st.func, ast.Attribute)
                              and st.func.attr == "check")
                total += max(body, orelse)
            else:
                if (isinstance(child, ast.Call)
                        and isinstance(child.func, ast.Attribute)
                        and child.func.attr == "check"):
                    total += mult
                total += count(child, mult)
        return total

    return count(ast.parse(src), 1)


probe_items = probe_assert_count(vp)
m4 = re.search(r"视觉探针\s*\|\s*\*{0,2}(\d+)\*{0,2}\s*项[^|\n]*?(\d+)\s*场景", readme)
if m4:
    check("视觉探针场景数（README）", int(m4.group(2)), registered)
    check("视觉探针断言条数（README）", int(m4.group(1)), probe_items)
else:
    failures.append("README 未找到「视觉探针 ... N 项 ... M 场景」声明")

# ------------------------------------- 5+6. ps-parity 表格加权 vs 算术行
print("[5] ps-parity 表格加权 vs 自称分数")
if not os.path.exists(os.path.join(ROOT, "docs", "ps-parity.md")):
    failures.append("找不到 docs/ps-parity.md")
    parity = None
else:
    parity = read(os.path.join("docs", "ps-parity.md"))


def section(text: str, name: str) -> str:
    start = text.index(name)
    rest = text[start + len(name):]
    nxt = rest.find("\n## ")
    return text[start:] if nxt < 0 else text[start:start + len(name) + nxt]


def dim_rows(seg: str, score_col: int):
    """取「维度 | 权重 | ... | 得分 | ...」行 -> [(名称, 权重, 得分)]（不含合计行）。

    score_col 为「得分」所在列**之前的列数偏移**的不同算法由调用方给出：
    口径 D 是第 5 列（index 4），口径 C 是第 5 列（index 4，含「日常子集/命中/得分」）。
    这里统一按「找表头里叫『得分』的那一列」来定位，避免两处硬编码。
    """
    out = []
    lines = [l for l in seg.split("\n") if l.strip().startswith("|")]
    if not lines:
        return out
    header = [c.strip() for c in lines[0].strip().strip("|").split("|")]
    si = next((i for i, h in enumerate(header) if "得分" in h), score_col)
    for line in lines:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) <= si:
            continue
        if cells[0].startswith("**合计") or cells[0] in ("维度", "------"):
            continue
        mw = re.fullmatch(r"(\d+)%", cells[1]) if len(cells) > 1 else None
        ms = re.fullmatch(r"(\d+)%", cells[si].replace("*", ""))
        if mw and ms:
            out.append((cells[0], int(mw.group(1)), int(ms.group(1))))
    return out


def claimed_total(seg: str):
    mm = re.search(r"=\s*\*\*([\d.]+)\s*%?\s*[≈~]?\s*(\d+)%\*\*", seg)
    if mm:
        return float(mm.group(1)), int(mm.group(2))
    return None, None


if parity is not None:
    for title, col in (("## 1. 口径 D", 4), ("## 2. 口径 C", 4)):
        try:
            seg = section(parity, title)
        except ValueError:
            failures.append(f"{title}: 章节不存在")
            continue
        rows = dim_rows(seg, col)
        if not rows:
            failures.append(f"{title}: 未解析到九维表格行")
            continue
        weight_sum = sum(w for _, w, _ in rows)
        weighted = sum(w * s for _, w, s in rows) / 100.0
        exact, rounded = claimed_total(seg)
        check(f"{title} 权重合计", 100, weight_sum)
        if rounded is None:
            failures.append(f"{title}: 未找到「= ... ≈ NN%」算术行")
        else:
            check(f"{title} 加权分（四舍五入）", rounded, round(weighted))

        # [6] 算术行里的每一对 0.WW×VV 必须与表格逐项相等
        body = seg[seg.find("算术："):]
        pairs = re.findall(r"0\.(\d{2})\s*[×x*]\s*(\d+)", body)
        if not pairs:
            failures.append(f"{title}: 算术行里没有可解析的「0.WW×VV」对")
        elif len(pairs) != len(rows):
            failures.append(f"{title}: 算术对 {len(pairs)} 项 != 表格 {len(rows)} 维")
        else:
            mismatch = []
            for (wtxt, stxt), (name, w, s) in zip(pairs, rows):
                if int(wtxt) != w or int(stxt) != s:
                    mismatch.append(f"{name}(表 {w}%×{s}% / 式 {wtxt}%×{stxt}%)")
            if mismatch:
                failures.append(f"{title}: 算式与表格不一致 -> " + "；".join(mismatch))
                print(f"  [FAIL] {title} 算式逐项一致: 不一致 {len(mismatch)} 项")
            else:
                print(f"  [PASS] {title} 算式逐项一致: {len(pairs)} 维全部对齐")

# ------------------------------------- 8. 文档内 §N 引用必须真实存在
print("[8] 文档内 §N 章节引用（FAIL 级：全仓都不存在的编号）")
# 2026-10-27 教训：ps-parity 里写了「详见 §7 变更记录」，而该文件只有 §0~§6。
# 这类悬空引用比数字错误更隐蔽——读者按图索骥找不到，整段论证就断了。

def section_numbers(text: str) -> set:
    """收集文档里真实存在的章节号：`## 3.` / `## 3.1 ` / `### 5.45 ` 等。"""
    nums = set()
    for m in re.finditer(r"^#{2,4}\s+(\d+(?:\.\d+)*)", text, re.M):
        nums.add(m.group(1))
        # `## 3.` 也应让 `§3` 通过；`## 3.1` 同理
    return nums


def check_refs(rel_path: str, text: str, all_nums: set) -> None:
    """只校验一类高置信度的悬空引用，不试图重建整张引用图。

    理由：本仓有既成约定——无前缀的 `§6.x` 指 VERIFICATION.md（该文件开篇写明
    「编号沿用 README §6.x」），`§5.x` 指 shell-parity.md。若强行按「默认指向本文件」
    判定，会产出 100+ 条误报，把一个有用的检查变成噪声。

    因此只查**裸单级编号**（`§7`、`§9` 这种，不带小数点）在**本文件内**是否存在：
    这正是 2026-10-27 那类错误的形态——ps-parity 写「详见 §7 变更记录」，而该文件
    只有 §0～§6，读者按图索骥会落空。带小数点的跨文件引用一律跳过（交给人工惯例）。

    这条检查覆盖不全，但**报出来的每一条都是真的**——宁可少报，不要误报。
    """
    have = section_numbers(text)
    if not have:
        return
    top_have = {n.split(".")[0] for n in have}
    doc_names = ("README", "BACKLOG", "VERIFICATION", "shell-parity", "manual-test",
                 "ps-parity", "tool-ops", "filters", "codec-migration",
                 "gpu-layer-revision", "format-import-matrix", "camera-raw",
                 "adjustment-json-keys", "THIRD-PARTY", "REVIEW", "AUDIT",
                 "该文档", "本文件", "另一份")
    dangling = []
    for m in re.finditer(r"§\s*(\d+)(?![\d.])", text):
        num = m.group(1)
        line = text[:m.start()].count("\n") + 1
        if num in have or num in top_have:
            continue
        # 前缀取前 96 字符即可：白名单词（清单/报告/文件名）一般紧邻引用；
        # 不做「编号组继承」的复杂处理——那需要解析 §1–§9 的区间语义，
        # 为一个检查项引入解析器不划算。若同组后续成员误报，NFKC 归一后人工复核。
        # 前缀取**引用之前最多两行**（去换行后拼接）：
        #  - 只看同行会漏掉「文档：`docs/filters.md` §1 / … /\n§5 / §6 …」这种
        #    文件名在上一行的情况（VERIFICATION 里很常见）；
        #  - 取 96 字符跨行又可能捞到无关的上一个文件名（「… §7 不做。\n\n 详见 §9」）。
        # 折中：最多两行、且若中间出现空行就截断（空行意味着换了段落，前缀不再有效）。
        start = max(0, m.start() - 160)
        raw = text[start:m.start()]
        para = raw.split("\n\n")[-1]                       # 空行后重新起算
        prefix = para.replace("\n", " ").rstrip()[-120:]
        # 判定「这个引用是否指向别的文档」的规则，按严格程度排序：
        # ① 前缀里点名了本仓的某份文档（README/BACKLOG/...）→ 跨文件
        if re.search(r"(README|BACKLOG|VERIFICATION|shell-parity|manual-test|ps-parity|"
                     r"tool-ops|filters|codec-migration|gpu-layer-revision|"
                     r"format-import-matrix|camera-raw|adjustment-json-keys|THIRD-PARTY|"
                     r"REVIEW|AUDIT)", prefix):
            continue
        # ② 「报告/AUDITn/Sn 报告」→ report/ 下的审计报告
        if re.search(r"(报告|AUDIT\w*|S\d+)\s*(§\s*\d+(\.\d+)?\s*/\s*)*$", prefix):
            continue
        # ③ `xxx.md` 紧跟 §N → 指向那份文件
        if re.search(r"\.md`?\s*(§\s*\d+(\.\d+)?\s*/\s*)*$", prefix):
            continue
        # ④ 「文档 §3.16 / §5」：紧邻「文档 §数字」形式（泛指 docs/ 下某份）
        if re.search(r"文档\s*§\s*\d", prefix):
            continue
        # ⑤ 「清单 §1–§9」「同上 §12」：同目录另一份清单。**只认紧邻**——
        #    前缀必须以这些词(+可选的连写编号)收尾，否则不算（否则「详见 §9」会被放过，
        #    负向验证就是在这里失败的）。
        if re.search(r"(清单|items\.md|同上|上表|下表)\s*(§\s*\d+(\.\d+)?\s*[–\-—~至]?\s*)*$", prefix):
            continue
        dangling.append((num, line))
    if dangling:
        detail = "、".join(f"§{n}(L{l})" for n, l in dangling[:6])
        failures.append(f"{rel_path}: 悬空章节引用 {detail}")
        print(f"  [FAIL] {rel_path} 章节引用: 本文件无此章 {len(dangling)} 处 -> {detail}")
    else:
        print(f"  [PASS] {rel_path} 章节引用: 无裸编号悬空")


DOC_SET = ("docs/ps-parity.md", "README.md", "BACKLOG.md", "VERIFICATION.md")
# 必须覆盖**全部**带编号章节的文档：BACKLOG 里的 `§5.55` 指向 shell-parity.md，
# VERIFICATION 里的 `§14.4` 指向别处。只查 4 个文件会造成大面积误报。
DOC_SET = DOC_SET + tuple(
    os.path.join("docs", os.path.basename(p)).replace("\\", "/")
    for p in glob.glob(os.path.join(ROOT, "docs", "*.md"))
    if "docs/" + os.path.basename(p).replace("\\", "/") not in DOC_SET
)
_docs = {}
for _rel in DOC_SET:
    try:
        _docs[_rel] = read(os.path.join(*_rel.split("/")))
    except FileNotFoundError:
        failures.append(f"{_rel}: 文件不存在")
_all_nums = set()
for _t in _docs.values():
    _all_nums |= section_numbers(_t)
for _rel, _text in _docs.items():
    check_refs(_rel, _text, _all_nums)

# ------------------------------- 9. 逐条清单命中数 == ps-parity 表格声明
print("[9] ps-parity 逐条清单 vs 表格（命中数一致性）")
# 2026-10-27 教训：口径 C/D 长期是估计值，无法复核。现在清单是唯一真源，
# 任何一侧改动而另一侧没跟上，这里必须 FAIL。
items_path = os.path.join(ROOT, "docs", "ps-parity-items.md")
if not os.path.exists(items_path):
    failures.append("找不到 docs/ps-parity-items.md（逐条清单）")
elif parity is not None:
    items = read(os.path.join("docs", "ps-parity-items.md"))
    # 清单各节以 `## N. 名称（M 条）` 分隔，节内表格行 `| D01 | 名称 | ✅ | 证据 |`
    dims = [("## 1.", "D", "文档与文件"), ("## 2.", "L", "图层与合成"),
            ("## 3.", "S", "选区"), ("## 4.", "P", "绘制与修饰"),
            ("## 5.", "T", "文字 / 形状 / 渐变"), ("## 6.", "A", "调整与滤镜"),
            ("## 7.", "X", "变换与画布"), ("## 8.", "V", "视图 / 界面"),
            ("## 9.", "C", "色彩 / 输出 / 自动化")]
    secs = {}
    idx = [(m.start(), m.group(0)) for m in re.finditer(r"^## \d+\. .*$", items, re.M)]
    for k, (pos, head) in enumerate(idx):
        end = idx[k + 1][0] if k + 1 < len(idx) else len(items)
        secs[head] = items[pos:end]

    total_items = total_hits = 0
    for prefix, key, name in dims:
        head = next((h for h in secs if h.startswith(prefix)), None)
        if head is None:
            failures.append(f"清单缺少「{name}」一节")
            continue
        rows = re.findall(r"^\|\s*" + key + r"(\d{2})\s*\|[^|]*\|\s*(✅|◐|❌)\s*\|",
                          secs[head], re.M)
        n = len(rows)
        h = sum(1 for _, st in rows if st == "✅")
        total_items += n
        total_hits += h
        claimed_n = claimed_h = None
        # 取口径 D 表格里该维的行：| 名称 | 权重 | 条目 | 命中 |
        for line in section(parity, "## 1. 口径 D").split("\n"):
            if line.strip().startswith(f"| {name} "):
                c = [x.strip() for x in line.strip().strip("|").split("|")]
                if len(c) >= 4:
                    claimed_n = int(re.sub(r"\D", "", c[2]) or 0)
                    claimed_h = int(re.sub(r"\D", "", c[3]) or 0)
                break
        if claimed_n is None:
            failures.append(f"parity 口径 D 无「{name}」行")
            continue
        check(f"{name} 清单条目数 == 表格", claimed_n, n)
        check(f"{name} 清单命中数 == 表格", claimed_h, h)
    print(f"  [INFO] 清单合计：{total_items} 条，命中 {total_hits} 条")

    # ---------- §13 缺口表必须覆盖全部未命中项（含 ◐） ----------
    miss_ids = set()
    for prefix, key, name in dims:
        head = next((h for h in secs if h.startswith(prefix)), None)
        if head is None:
            continue
        for num, st in re.findall(r"^\|\s*" + key + r"(\d{2})\s*\|[^|]*\|\s*(✅|◐|❌)\s*\|",
                                  secs[head], re.M):
            if st != "✅":
                miss_ids.add(key + num)
    s13 = secs.get(next((h for h in secs if h.startswith("## 13.")), ""), "")
    listed = set(re.findall(r"^\|\s*([A-Z]\d{2})\s*\|", s13, re.M))
    check("§13 覆盖未命中项数", len(miss_ids), len(miss_ids & listed))
    missing = sorted(miss_ids - listed)
    if missing:
        failures.append(f"§13 未列出这些未命中项：{'、'.join(missing)}")
        print(f"  [FAIL] §13 覆盖: 漏 {len(missing)} 项 -> {'、'.join(missing[:8])}")
    else:
        print(f"  [PASS] §13 覆盖: {len(miss_ids)} 条未命中项全部有条目")


print("[7] ps-parity 证据基准 HEAD（WARN 级）")
if parity is not None:
    def _last_source_commit() -> str:
        """最近一次改动源码（engine/ shells/ tests/）的提交。

        文档记的是**代码基线**，不是「当前 HEAD」：只改文档的提交不应让它过期，
        否则每提交一次文档就要重算一次分数，反而逼人改数字。
        """
        try:
            out = subprocess.run(
                ["git", "-C", ROOT, "log", "-1", "--format=%h", "--",
                 "engine", "shells", "tests"],
                capture_output=True, text=True, timeout=10).stdout.strip()
            return out
        except Exception:
            return ""

    head_now = _last_source_commit()
    mh = re.search(r"(?:代码基线|HEAD)\s*\**\s*`([0-9a-f]{7,40})`", parity[:1800])
    if mh and head_now:
        if not head_now.startswith(mh.group(1)[:7]):
            warn("ps-parity 代码基线",
                 f"文档={mh.group(1)[:7]}，最近源码提交={head_now}（代码变了，宜重算）")
        else:
            print(f"  [PASS] ps-parity 代码基线: {mh.group(1)[:7]} == 最近源码提交")

# ------------------------------------------------------------------ 汇总
print()
if warnings:
    print("check_docs: WARN")
    for line in warnings:
        print("  ! ", line)
if failures:
    print("check_docs: FAIL")
    for line in failures:
        print("  -", line)
    sys.exit(1)
print("check_docs: PASS")
