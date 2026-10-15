# AI生成
"""run_vulkan_real.py — 在真机上运行 Vulkan 计算管线验证。

为什么需要独立脚本：默认构建**不编入**设备侧代码（`COMPOSITOR_ENABLE_VULKAN_COMPUTE`），
所以真机用例需要：
  1) 用 opt-in 开关单独编译 `engine/render/vulkan_backend.cpp`（覆盖默认对象）；
  2) 编译 `tools/vulkan_real_test.cpp` 宿主；
  3) 链接「除默认 vulkan_backend 外的全部引擎对象 + opt-in 对象」；
  4) 运行并检查退出码。

用法：python scripts/run_vulkan_real.py [宽] [高]
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(BUILD, "vk_real")
ENGINE = os.path.join(ROOT, "engine")

ZIG = [sys.executable, "-m", "ziglang"]
INCLUDES = ["-I" + ENGINE, "-I" + os.path.join(ENGINE, "abi", "include")]
CXX_FLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-Wno-nullability-completeness"]
OPT_IN = "-DCOMPOSITOR_ENABLE_VULKAN_COMPUTE"


def child_env():
    env = dict(os.environ)
    # zig 需要可用的缓存目录，否则报 AppDataDirUnavailable。
    fallback = os.path.join(ROOT, "build", ".hostenv")
    env.setdefault("LOCALAPPDATA", os.path.join(fallback, "LocalAppData"))
    env.setdefault("HOME", os.path.join(fallback, "Home"))
    env.setdefault("XDG_CACHE_HOME", os.path.join(fallback, "Cache"))
    for path in (env["LOCALAPPDATA"], env["HOME"], env["XDG_CACHE_HOME"]):
        os.makedirs(path, exist_ok=True)
    return env


def run(cmd, label):
    print("[run] " + label)
    print("      " + " ".join(cmd))
    result = subprocess.run(cmd, cwd=ROOT, env=child_env(), text=True, capture_output=True)
    if result.stdout.strip():
        print(result.stdout.rstrip())
    if result.stderr.strip():
        print(result.stderr.rstrip())
    if result.returncode != 0:
        raise SystemExit("[fail] %s -> rc=%d" % (label, result.returncode))
    return result


def main():
    os.makedirs(OUT, exist_ok=True)

    if not os.path.isdir(BUILD):
        raise SystemExit("[fail] 未找到 build/；请先运行 python scripts/build.py")

    optin_obj = os.path.join(OUT, "vulkan_backend_optin.o")
    host_obj = os.path.join(OUT, "vulkan_real_test.o")
    exe = os.path.join(OUT, "vulkan_real_test.exe")

    run(ZIG + ["c++", "-c", os.path.join(ENGINE, "render", "vulkan_backend.cpp"), "-o", optin_obj]
        + CXX_FLAGS + [OPT_IN] + INCLUDES, "编译 opt-in vulkan_backend.cpp")
    run(ZIG + ["c++", "-c", os.path.join(ROOT, "tools", "vulkan_real_test.cpp"), "-o", host_obj]
        + CXX_FLAGS + INCLUDES, "编译真机用例宿主")

    # 引擎对象：排除默认（未编入设备侧）的 vulkan_backend.o，用 opt-in 版本替代。
    engine_objects = sorted(glob.glob(os.path.join(BUILD, "engine_*.o")))
    default_backend = os.path.join(BUILD, "engine_render_vulkan_backend.o")
    objects = [path for path in engine_objects if os.path.normcase(path) != os.path.normcase(default_backend)]
    if len(objects) != len(engine_objects) - 1:
        raise SystemExit("[fail] 未能从对象列表剔除默认 vulkan_backend.o（请先跑 build.py）")

    run(ZIG + ["c++", host_obj, optin_obj] + objects + ["-o", exe, "-pthread"], "链接真机用例")
    print("[exe] " + exe)

    result = subprocess.run([exe] + sys.argv[1:], cwd=ROOT, env=child_env(), text=True,
                            capture_output=True)
    print(result.stdout.rstrip())
    if result.stderr.strip():
        print(result.stderr.rstrip())
    print("[exit] rc=%d" % result.returncode)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
