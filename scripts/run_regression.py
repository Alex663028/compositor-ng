# AI生成
#!/usr/bin/env python3
"""compositor-ng 一键回归入口（把散落的验证步骤串成单命令）。

为什么存在：审核报告（REVIEW-compositor-ng-20261014）四轮迭代里，鸿蒙交叉编译
始终挂在「未验证」；独立复核（2026-10-15）实测本机 DevEco SDK 在位、可跑，
遂把全部验证步骤固化为单入口，杜绝「挑着验」——要么全绿，要么如实报红。

步骤（任一失败即整体 rc != 0；标注 SKIP 的步骤不计失败）：
  1. build.py --clean      全量重编译 + 单元测试（基线：summary 行 0 failed）
  2. Windows 壳            构建 + --selftest=<报告文件路径>；核对 rc=0、
                           [summary] failures = 0、SHA256 与仓内基线
                           compositor_shell_selftest.txt 逐字节一致
                           （基线是快照契约：自检项增减必须连基线一起更新）
  3. crosscheck_codec.py   引擎编码产物 -> PIL 独立解码（证明不是自洽私有格式）
  4. crosscheck_comp.py    .comp 双向 <-> PIL（0 LSB 超容差）
  5. build.py --shared     共享库 + ctypes 真实加载调用
                           （comp_version / comp_result_name，不只是文件存在）
  6. build_ohos.sh         鸿蒙 aarch64 交叉编译（OHOS NDK 缺失记 SKIP；
                           加 --require-ohos 可把缺失升级为失败）

用法：
    python scripts/run_regression.py                  # 全步骤
    python scripts/run_regression.py --skip-ohos      # 无 NDK 环境跳过鸿蒙步
    python scripts/run_regression.py --require-ohos   # NDK 缺失视为失败
    python scripts/run_regression.py --keep-build     # 不做 --clean，增量（快但非全量）
    python scripts/run_regression.py --log <path>     # 日志另存一份
    python scripts/run_regression.py --python <path>  # 指定跑 crosscheck 的解释器
                                                     # （需要 Pillow；缺省取 $CROSSCHECK_PYTHON
                                                     #  或当前解释器。PIL 缺失记 SKIP 不记 FAIL）

退出码：0 = 全部通过（含 SKIP）；1 = 有步骤失败；2 = 环境/参数问题。
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
SHELL_EXE = os.path.join(BUILD, "compositor_shell.exe")
SELFTEST_BASELINE = os.path.join(ROOT, "compositor_shell_selftest.txt")
DLL = os.path.join(BUILD, "compositor_engine.dll")
DEFAULT_OHOS_NDK = "G:/DevEco Studio/sdk/default/openharmony/native"

IS_WINDOWS = os.name == "nt"

# 输出流容错：子进程输出按本地编码解码后仍可能混有无法回写的字符（U+FFFD），
# 在 GBK 控制台 + 重定向的组合下会让 print 抛 UnicodeEncodeError，把「某步骤
# 到底过没过」变成一句 unexpected exception。这里只放宽编码错误处理为替换，
# 不改变任何判定逻辑。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):
        pass


class Regression:
    """极简步骤执行器：统一计时、打印、记账。"""

    def __init__(self) -> None:
        self.rows: list[tuple[str, str, float, str]] = []

    def run_cmd(self, cmd: list[str], *, shell: bool = False,
                capture: bool = False, timeout: float = 1800.0) -> subprocess.CompletedProcess:
        """执行一条子命令。

        timeout：挂死兜底。此前任何一步若死循环（如单测卡死、壳等待输入），
        整个回归会无限期挂起且无任何诊断，只能人工杀进程。超时后
        subprocess.run 会 kill 子进程并抛 TimeoutExpired——step() 把它记成
        该步 FAIL，回归得以收口。默认 1800s 对最慢的 --clean 全量构建
        （实测约 133s）有充分余量。
        """
        printable = cmd if shell else " ".join(cmd)
        print(f"    $ {printable}", flush=True)
        try:
            if capture:
                proc = subprocess.run(cmd, cwd=ROOT, shell=shell, timeout=timeout,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, errors="replace")
                sys.stdout.write(proc.stdout or "")
                return proc
            return subprocess.run(cmd, cwd=ROOT, shell=shell, timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"    [TIMEOUT] 超过 {timeout:.0f}s 已杀掉: {printable}", flush=True)
            raise

    def step(self, name: str, fn) -> bool:
        """执行一个步骤；返回是否通过（SKIP 也视为通过）。"""
        print(f"\n==> {name}", flush=True)
        started = time.time()
        try:
            status, detail = fn()
        except Exception as exc:  # 步骤自身炸了也算失败，不吞异常
            status, detail = "FAIL", f"unexpected exception: {exc}"
        elapsed = time.time() - started
        self.rows.append((name, status, elapsed, detail))
        marker = {"PASS": "[PASS]", "FAIL": "[FAIL]", "SKIP": "[SKIP]"}[status]
        print(f"    {marker} {name} ({elapsed:.1f}s) {detail}".rstrip(), flush=True)
        return status != "FAIL"


def sha256_of(path: str) -> str:
    """文本文件的 SHA256，先把行尾归一化为 LF。

    为什么必须归一化：外壳按Windows 惯例写 CRLF，而仓内基线是 LF。
    直接对字节算哈希，同一份内容会因为行尾不同而给出不同摘要——那这个
    "基线"既不跨平台也不跨检出目录可用（曾导致干净 clone 后回归误报FAIL）。
    归一化后比对的是**内容**是否一致，与行尾、与检出路径都无关。
    """
    with open(path, "rb") as f:
        raw = f.read()
    return hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest()


def step_blocks_gate(reg: Regression) -> bool:
    """OPT-7：blocks 逃逸闸。

    engine/imaging 的 clang block 只允许栈内同步用法（comp_blocks_rt.c 的
    _Block_copy 恒等实现决定了 block 一旦逃逸就是悬垂指针，且该文件自曝的
    abort 闸在当前构建不可达——见 comp_blocks_rt.c N6 注记）。机械可查的约束：
    仓内（除 BlocksRuntime 桩自身）不得出现 Block_copy 调用点。
    """
    import glob as _glob

    def fn() -> tuple[str, str]:
        offenders: list[str] = []
        for path in _glob.glob(os.path.join(ROOT, "engine", "**", "*.[ch]"),
                               recursive=True) +                 _glob.glob(os.path.join(ROOT, "engine", "**", "*.cpp"), recursive=True):
            if os.path.basename(path) == "comp_blocks_rt.c":
                continue  # 桩自身的定义不是调用点
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    if "Block_copy(" in line or "Block_copy (" in line:
                        offenders.append(f"{os.path.relpath(path, ROOT)}:{lineno}")
        if offenders:
            return "FAIL", "block 逃逸调用点: " + ", ".join(offenders[:5])
        return "PASS", "Block_copy 调用点 = 0"

    return reg.step("0. blocks 逃逸闸（Block_copy 调用点 = 0）", fn)


def step_build_and_tests(reg: Regression, clean: bool) -> bool:
    title = "1. 全量构建 + 单元测试（build.py" + (" --clean）" if clean else "，增量）")

    def fn():
        cmd = [sys.executable, os.path.join("scripts", "build.py")]
        if clean:
            cmd.append("--clean")
        proc = reg.run_cmd(cmd, capture=True)
        if proc.returncode != 0:
            return "FAIL", f"build.py rc={proc.returncode}"
        # build.py 对测试二进制 check=True，rc=0 已保证无失败用例；
        # 再解析 summary 行把「用例数 / 失败数」写进日志，作为可审计证据。
        match = re.search(r"summary: (\d+) cases, (\d+) failed", proc.stdout or "")
        if match is None:
            return "FAIL", "no 'summary: N cases, M failed' line in build output"
        cases, failed = match.group(1), match.group(2)
        if failed != "0":
            return "FAIL", f"unit tests: {cases} cases, {failed} failed"
        return "PASS", f"{cases} cases, 0 failed"

    return reg.step(title, fn)


def step_windows_shell(reg: Regression) -> bool:
    def fn():
        if not IS_WINDOWS:
            return "SKIP", "非 Windows 平台，无 compositor_shell.exe"
        build = reg.run_cmd([sys.executable, os.path.join("scripts", "build_windows_shell.py")])
        if build.returncode != 0:
            return "FAIL", "build_windows_shell.py failed"
        if not os.path.isfile(SHELL_EXE):
            return "FAIL", f"missing {SHELL_EXE}"

        # S23：引擎对象的变体记账必须与磁盘一致（T2 的 `--gpu --shared` 曾静默降级：
        # 壳以为在跑 D3D12，链的却是上一次非 GPU 构建留下的 engine_*.o，AUTO 落到 CPU
        # 且无任何提示）。壳构建现在按变体算确定路径、缺一个就失败，这里再加一条烟测，
        # 盯住 --check 的结论确实反映磁盘，而不是恒真/恒假。
        cpu_check = reg.run_cmd(
            [sys.executable, os.path.join("scripts", "build_windows_shell.py"), "--check"],
            capture=True)
        if cpu_check.returncode != 0:
            return "FAIL", "shell --check 报告 CPU 引擎对象缺失（第 1 步应已产出）"
        gpu_on_disk = os.path.isfile(os.path.join("build", "gpu_engine_domain_document.o"))
        gpu_check = reg.run_cmd(
            [sys.executable, os.path.join("scripts", "build_windows_shell.py"),
             "--gpu", "--check"], capture=True)
        if (gpu_check.returncode == 0) != gpu_on_disk:
            return ("FAIL",
                    "shell --check --gpu 的结论与磁盘上的 GPU 对象不一致（变体记账不可信）")

        #
        # 壳自检自己会写 build/import_probe.png 夹具（缺了就现写，见 main.cpp 的
        # probe_png_exists / write_probe_png），所以这一步**不**再替它补。
        # 曾经在这里补过，但那依赖 build/compositor_engine.dll——而第 5 步才建它，
        # 第 2 步必然取不到；把依赖放错了步骤，比不补更糟。

        fd, report_path = tempfile.mkstemp(prefix="compositor_selftest_", suffix=".txt")
        os.close(fd)
        try:
            # 注意：--selftest= 的参数是「报告文件路径」（不是目录），GUI 壳无控制台输出。
            proc = reg.run_cmd([SHELL_EXE, f"--selftest={report_path}"])
            if proc.returncode != 0:
                return "FAIL", f"shell selftest rc={proc.returncode}"
            if not os.path.isfile(report_path):
                return "FAIL", "selftest report file not written"
            with open(report_path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            failures = re.search(r"\[summary\] failures = (\d+)", text)
            if failures is None:
                return "FAIL", "no [summary] failures line in report"
            if failures.group(1) != "0":
                return "FAIL", f"selftest failures = {failures.group(1)}"
            if "[PASS]" not in text:
                return "FAIL", "no [PASS] line in report"

            if not os.path.isfile(SELFTEST_BASELINE):
                return "FAIL", f"baseline missing: {SELFTEST_BASELINE}"
            digest = sha256_of(report_path)
            baseline_digest = sha256_of(SELFTEST_BASELINE)
            if digest != baseline_digest:
                return ("FAIL",
                        "selftest SHA256 != baseline "
                        f"({digest[:16]}… vs {baseline_digest[:16]}…)；"
                        "若自检项有增减，请连 compositor_shell_selftest.txt 一起更新")
            ok_count = text.count("[OK]")
            return "PASS", f"{ok_count} items OK, failures=0, SHA256 {digest[:16]}… == baseline"
        finally:
            try:
                os.remove(report_path)
            except OSError:
                pass

    return reg.step("2. Windows 壳自检（rc / failures / SHA256 基线）", fn)


def step_crosscheck(reg: Regression, script: str, title: str, python: str) -> bool:
    def fn():
        proc = reg.run_cmd([python, os.path.join("scripts", script)], capture=True)
        if proc.returncode != 0:
            # rc=2 是 crosscheck 脚本的 "PIL unavailable" 退出码：这是环境缺失，
            # 不是引擎回归。没有这条诊断时，一个没装 Pillow 的解释器会让第 3/4 步
            # 以晦涩的 rc 失败——看起来像编码器坏了，实际只是用错了解释器。
            if proc.returncode == 2 and "PIL unavailable" in (proc.stdout or ""):
                return ("SKIP",
                        f"{script} 需要 Pillow；当前解释器 {python} 未安装。"
                        "若该解释器用于其它用途，可用 --python <带 Pillow 的解释器> 重跑")
            return "FAIL", f"{script} rc={proc.returncode}"
        return "PASS", "rc=0"

    return reg.step(title, fn)


def step_shared_dll(reg: Regression) -> bool:
    def fn():
        if not IS_WINDOWS:
            return "SKIP", "非 Windows 平台，无 compositor_engine.dll"
        proc = reg.run_cmd([sys.executable, os.path.join("scripts", "build.py"), "--shared"])
        if proc.returncode != 0:
            return "FAIL", "build.py --shared failed"
        if not os.path.isfile(DLL):
            return "FAIL", f"missing {DLL}"
        try:
            lib = ctypes.WinDLL(DLL)
            lib.comp_version.restype = ctypes.c_char_p
            lib.comp_result_name.restype = ctypes.c_char_p
            lib.comp_result_name.argtypes = [ctypes.c_int]
            version = lib.comp_version()
            ok_name = lib.comp_result_name(0)
        except OSError as exc:
            return "FAIL", f"ctypes load/call failed: {exc}"
        if version != b"1.0.0":
            return "FAIL", f"comp_version() = {version!r}, expect b'1.0.0'"
        if ok_name != b"COMP_OK":
            return "FAIL", f"comp_result_name(0) = {ok_name!r}, expect b'COMP_OK'"
        return "PASS", f"DLL 真实加载并调用：comp_version={version.decode()}, comp_result_name(0)={ok_name.decode()}"

    return reg.step("5. 共享构建 + ctypes 真实调用", fn)


def ohos_arch() -> str:
    """build_ohos.sh 的目标架构；默认 aarch64（真机），模拟器用 OHOS_ARCH=x86_64。"""
    return os.environ.get("OHOS_ARCH", "aarch64")


def step_ohos(reg: Regression, require: bool) -> bool:
    def fn():
        ndk = os.environ.get("OHOS_NDK", DEFAULT_OHOS_NDK)
        if not os.path.isdir(ndk):
            if require:
                return "FAIL", f"OHOS NDK not found: {ndk}"
            return "SKIP", f"OHOS NDK not found: {ndk}（--require-ohos 可升级为失败）"
        for script in ("build_ohos.sh", "build_ohos_shell.sh"):
            proc = reg.run_cmd(["bash", os.path.join("scripts", script)], shell=False)
            if proc.returncode != 0:
                return "FAIL", f"{script} rc={proc.returncode}"
        return "PASS", "交叉编译 + NAPI 壳 rc=0"

    return reg.step(f"6. 鸿蒙交叉编译（{ohos_arch()}）", fn)


def main() -> int:
    parser = argparse.ArgumentParser(description="compositor-ng one-click regression")
    parser.add_argument("--skip-ohos", action="store_true",
                        help="skip the HarmonyOS cross-compile step")
    parser.add_argument("--require-ohos", action="store_true",
                        help="treat a missing OHOS NDK as failure instead of SKIP")
    parser.add_argument("--keep-build", action="store_true",
                        help="skip --clean (faster, but not a full rebuild)")
    parser.add_argument("--log", metavar="PATH", help="also write the log to PATH")
    parser.add_argument("--python", metavar="PATH",
                        default=os.environ.get("CROSSCHECK_PYTHON", sys.executable),
                        help="interpreter for the PIL crosscheck steps "
                             "(default: $CROSSCHECK_PYTHON or this interpreter)")
    args = parser.parse_args()

    if args.skip_ohos and args.require_ohos:
        print("--skip-ohos and --require-ohos are mutually exclusive", file=sys.stderr)
        return 2

    if args.log:
        # 第 1 步会 --clean 整个 build/：日志若写进 build/，rmtree 撞上正在
        # 写入的日志文件（WinError 32）会让第一步就炸，后续步骤连锁失败。
        log_path = os.path.abspath(args.log)
        if log_path.startswith(os.path.abspath(BUILD) + os.sep):
            print(f"--log 不能指向 build/ 目录内（会被 --clean 删除）: {args.log}",
                  file=sys.stderr)
            return 2

    log_fh = open(args.log, "w", encoding="utf-8") if args.log else None
    if log_fh is not None:
        sys.stdout = _Tee(sys.stdout, log_fh)  # type: ignore[assignment]

    reg = Regression()
    print("compositor-ng regression "
          f"(clean={not args.keep_build}, ohos={'required' if args.require_ohos else 'auto'})")

    step_blocks_gate(reg)
    step_build_and_tests(reg, clean=not args.keep_build)
    step_windows_shell(reg)
    step_crosscheck(reg, "crosscheck_codec.py", "3. 编解码交叉验证（引擎写 -> PIL 读）", args.python)
    step_crosscheck(reg, "crosscheck_comp.py", "4. .comp 双向交叉验证（<-> PIL）", args.python)
    step_shared_dll(reg)
    if not args.skip_ohos:
        step_ohos(reg, require=args.require_ohos)
    else:
        reg.rows.append((f"6. 鸿蒙交叉编译（{ohos_arch()}）", "SKIP", 0.0, "--skip-ohos"))

    print("\n================ regression summary ================")
    for name, status, elapsed, detail in reg.rows:
        print(f"  [{status}] {elapsed:6.1f}s  {name}"
              + (f"  -- {detail}" if detail else ""))
    failed = [r for r in reg.rows if r[1] == "FAIL"]
    skipped = [r for r in reg.rows if r[1] == "SKIP"]
    print(f"  => {len(reg.rows) - len(failed) - len(skipped)} passed, "
          f"{len(skipped)} skipped, {len(failed)} failed")
    if log_fh is not None:
        sys.stdout = sys.stdout.primary  # type: ignore[attr-defined]
        log_fh.close()
    return 1 if failed else 0


class _Tee:
    """stdout 同时写终端与日志文件。"""

    def __init__(self, primary, secondary) -> None:
        self.primary = primary
        self.secondary = secondary

    def write(self, data: str) -> int:
        self.primary.write(data)
        self.secondary.write(data)
        return len(data)

    def flush(self) -> None:
        self.primary.flush()
        self.secondary.flush()


if __name__ == "__main__":
    sys.exit(main())
