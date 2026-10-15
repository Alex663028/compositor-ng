# AI生成
"""
build_windows_shell.py — 构建 Windows 外壳层（M3）。

策略：复用 scripts/build.py 已产出的 engine_*.o（避免重复编译 68 个引擎编译单元），
只单独编译 shells/windows/src 下的壳源码，然后链接成 GUI 可执行文件。
因此本脚本的前置条件是先跑一次 scripts/build.py。

用法：
    python scripts/build.py                # 先产出引擎对象文件与测试
    python scripts/build_windows_shell.py  # 再产出 build/compositor_shell.exe
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
PYTHON = sys.executable
SHELL_SRC = os.path.join(ROOT, "shells", "windows", "src")

INCLUDES = [
    os.path.join(ROOT, "engine"),
    os.path.join(ROOT, "engine", "abi", "include"),
    os.path.join(ROOT, "engine", "abi", "src"),
]

CXX_FLAGS = [
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-nullability-completeness",
]


def _load_build_module():
    """复用 scripts/build.py 的子进程环境：zig 需要可用的 AppData/TEMP 目录，
    否则报 "unable to resolve zig cache directory: AppDataDirUnavailable"。"""
    import importlib.util

    path = os.path.join(ROOT, "scripts", "build.py")
    spec = importlib.util.spec_from_file_location("compositor_build", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_BUILD = _load_build_module()


def child_env():
    return _BUILD.child_env()


def run(cmd):
    print("[shell] " + " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=ROOT, env=child_env())


def main():
    objects = sorted(glob.glob(os.path.join(BUILD, "engine_*.o")))
    if not objects:
        print("[shell] 未找到 build/engine_*.o，请先运行 python scripts/build.py")
        return 2

    os.makedirs(BUILD, exist_ok=True)
    shell_objects = []
    for source in sorted(glob.glob(os.path.join(SHELL_SRC, "*.cpp"))):
        name = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(BUILD, "shell_windows_" + name + ".o")
        cmd = [PYTHON, "-m", "ziglang", "c++", "-c", source, "-o", obj]
        cmd += CXX_FLAGS
        cmd += ["-I" + path for path in INCLUDES]
        run(cmd)
        shell_objects.append(obj)

    exe = os.path.join(BUILD, "compositor_shell.exe")
    link = [PYTHON, "-m", "ziglang", "c++"] + shell_objects + objects
    link += ["-o", exe, "-mwindows", "-municode", "-luser32", "-lgdi32", "-lcomdlg32", "-pthread"]
    run(link)

    print("[shell] executable: " + exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
