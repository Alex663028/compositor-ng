# AI生成
"""
build_windows_shell.py — 构建 Windows 外壳层（M3）。

策略：复用 scripts/build.py 已产出的**同变体**引擎对象文件（避免重复编译 68 个引擎
编译单元），只单独编译 shells/windows/src 下的壳源码，然后链接成 GUI 可执行文件。
因此本脚本的前置条件是先跑一次**同变体**的 scripts/build.py。

用法：
    python scripts/build.py                # 先产出引擎对象文件与测试
    python scripts/build.py --gpu          # 编入 D3D12 后端后再构建外壳
    python scripts/build_windows_shell.py  # 再产出 build/compositor_shell.exe

`--gpu` 决定**两件事**：壳源码是否带 -DCOMP_HAS_D3D12，以及链接哪一套引擎对象
（build/gpu_engine_*.o 还是 build/engine_*.o）。

S23 之前这里只影响前者：对象文件靠 glob `build/engine_*.o` 收集，于是
`build_windows_shell.py --gpu` 会去链上一次**非 GPU** 构建留下的对象——壳以为自己在
跑 D3D12，引擎里 D3D12 后端根本没编进去，AUTO 静默落到 CPU 且无任何提示（T2 实测）。
现在对象路径按变体确定，缺一个就直接失败并说清该跑哪条命令；不存在「凑巧链上」的
路径，因此也不会再有静默降级。
"""
import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def compile_resource_script():
    """把 packaging/compositor_shell.rc 编成 .res（程序图标 + VERSIONINFO）。

    资源只影响「发出去之后长什么样」，不影响能否运行，因此任何一步失败都只打印
    提示并返回 None，绝不阻塞构建。

    rc 编译器优先级：DevEco NDK 的 llvm-rc（本机确定存在）→ PATH 上的 llvm-rc →
    Windows SDK 的 rc.exe。zig 自带的 `zig rc` 在本机会因缓存目录限制直接失败，
    所以不在候选里。
    """
    rc_script = os.path.join(ROOT, "packaging", "compositor_shell.rc")
    if not os.path.isfile(rc_script):
        print("[shell] 跳过资源：未找到 packaging/compositor_shell.rc")
        return None
    out = os.path.join(BUILD, "compositor_shell.res")
    if os.path.isfile(out) and os.path.getmtime(out) > os.path.getmtime(rc_script):
        return out

    deveco_sdk = os.environ.get("DEVECO_SDK_HOME", "G:/DevEco Studio/sdk")
    candidates = [
        os.path.join(deveco_sdk, "default", "openharmony", "native", "llvm", "bin", "llvm-rc.exe"),
        "llvm-rc",
        "rc",
    ]
    compiler = next((c for c in candidates if c == "llvm-rc" or c == "rc" or os.path.isfile(c)), None)
    if compiler is None:
        print("[shell] 跳过资源：找不到 rc 编译器（llvm-rc / rc.exe）")
        return None

    print("[shell] resource: " + compiler)
    # llvm-rc 只认 `-fo <out> <in>`；用 `/fo` 会被当成第二个输入文件。
    result = subprocess.run([compiler, "-fo", out, rc_script],
                            cwd=os.path.dirname(rc_script),
                            capture_output=True, text=True)
    if result.returncode != 0 or not os.path.isfile(out):
        print("[shell] 资源编译失败（不阻塞构建）：" + (result.stderr or "").strip()[:300])
        return None
    return out
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


def engine_objects(gpu: bool) -> list[str]:
    """本变体要求的引擎对象文件（**预期**路径，不看磁盘上有什么）。"""
    return _BUILD.engine_object_paths(_BUILD.DEFAULT_DRIVER, shared=False, gpu=gpu)


def missing_objects(gpu: bool) -> list[str]:
    return [path for path in engine_objects(gpu) if not os.path.isfile(path)]


def variant_hint(gpu: bool) -> str:
    return "python scripts/build.py --gpu" if gpu else "python scripts/build.py"


def main():
    parser = argparse.ArgumentParser(description="Build the Windows shell executable")
    parser.add_argument("--gpu", action="store_true",
                        help="link the GPU engine objects and compile the shell with -DCOMP_HAS_D3D12")
    parser.add_argument("--check", action="store_true",
                        help="only verify that the matching engine objects exist (no compile/link)")
    args = parser.parse_args()

    missing = missing_objects(args.gpu)
    if args.check:
        if missing:
            print("[shell] 缺少 %d 个 %s 引擎对象，例如：%s"
                  % (len(missing), "GPU" if args.gpu else "CPU", missing[0]))
            print("[shell] 请先运行：" + variant_hint(args.gpu))
            return 2
        print("[shell] 引擎对象齐全（%s 变体）" % ("GPU" if args.gpu else "CPU"))
        return 0

    if missing:
        # 绝不退回另一变体的对象：静默降级比构建失败危险得多（T2 的教训）。
        print("[shell] 缺少 %d 个引擎对象（%s 变体），例如：%s"
              % (len(missing), "GPU" if args.gpu else "CPU", missing[0]))
        print("[shell] 引擎对象必须与外壳同变体。请先运行：" + variant_hint(args.gpu))
        if args.gpu:
            print("[shell] 提示：非 GPU 对象不能替代——链上它们会让 AUTO 静默落到 CPU。")
        return 2

    os.makedirs(BUILD, exist_ok=True)
    shell_objects = []
    for source in sorted(glob.glob(os.path.join(SHELL_SRC, "*.cpp"))):
        name = os.path.splitext(os.path.basename(source))[0]
        variant = "gpu_" if args.gpu else ""
        obj = os.path.join(BUILD, "shell_windows_" + variant + name + ".o")
        cmd = [PYTHON, "-m", "ziglang", "c++", "-c", source, "-o", obj]
        cmd += CXX_FLAGS
        cmd += ["-I" + path for path in INCLUDES]
        if args.gpu:
            cmd.append("-DCOMP_HAS_D3D12")
        run(cmd)
        shell_objects.append(obj)

    exe = os.path.join(BUILD, "compositor_shell.exe")
    resource = compile_resource_script()
    link = [PYTHON, "-m", "ziglang", "c++"] + shell_objects + engine_objects(args.gpu)
    if resource is not None:
        link.append(resource)
    link += ["-o", exe, "-mwindows", "-municode", "-luser32", "-lgdi32", "-lcomdlg32", "-pthread"]
    run(link)

    print("[shell] executable: " + exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
