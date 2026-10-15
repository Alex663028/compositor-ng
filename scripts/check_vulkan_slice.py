# AI生成
"""check_vulkan_slice.py — 本机可做的 Vulkan 计算管线切片自检。

做四件事：
  1) 用 scripts/gen_vulkan_bindings.py 重建绑定常量头（证明可从 GLSL 复现）；
  2) 给 vulkan_backend.cpp 补 <vector>；给 CMakeLists.txt 补 tests/test_rhi.cpp（修分叉）；
  3) 用 ziglang 对 vulkan_backend.cpp 做**两种变体**的编译检查：
       默认（不编入设备侧）与 opt-in（-DCOMPOSITOR_ENABLE_VULKAN_COMPUTE，编入设备侧全链路）；
  4) 跑 build.py（含 70 用例回归）与 build_windows_shell.py（Windows 外壳链接）。

本机无 Vulkan SDK / 无 GPU，故只做「可编译 + 回归不退化」这一级验证；
设备侧运行行为需真机，属明确的能力边界。
"""
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable


def run(cmd, label):
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print("[run] %s -> rc=%d" % (label, proc.returncode))
    for line in (proc.stdout or "").strip().splitlines()[-14:]:
        print("    " + line)
    if proc.returncode != 0:
        for line in (proc.stderr or "").strip().splitlines()[-14:]:
            print("    E " + line)
    return proc.returncode == 0


def patch(rel_path, old, new):
    path = os.path.join(ROOT, rel_path)
    text = io.open(path, encoding="utf-8", newline="").read()
    if new in text:
        return "already"
    if old not in text:
        return "MISS"
    io.open(path, "w", encoding="utf-8", newline="").write(text.replace(old, new, 1))
    return "patched"


def compile_check(name, extra_flags):
    out = os.path.join(ROOT, "build", "_chk_vulkan_%s.o" % name)
    cmd = ([PY, "-m", "ziglang", "c++", "-std=c++17"] + extra_flags +
           ["-I", "engine", "-I", "engine/abi/include", "-I", "engine/abi/src",
            "-c", "-o", out, "engine/render/vulkan_backend.cpp"])
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print("[compile %s] rc=%d" % (name, proc.returncode))
    for line in (proc.stderr or "").strip().splitlines()[-10:]:
        print("    " + line)
    return proc.returncode == 0


def main():
    print("=== 1) 重建绑定常量头（单一真源 = GLSL） ===")
    run([PY, "scripts/gen_vulkan_bindings.py"], "gen_vulkan_bindings.py")
    header = os.path.join(ROOT, "engine", "render", "spirv", "vulkan_compute_bindings.h")
    if os.path.exists(header):
        for line in io.open(header, encoding="utf-8").read().splitlines():
            if "constexpr" in line:
                print("    " + line.strip())

    print("=== 2) 补 <vector> 与 CMakeLists 测试列表 ===")
    print("    cpp <vector>:", patch("engine/render/vulkan_backend.cpp",
                                     "#include <cstring>\n",
                                     "#include <cstring>\n#include <vector>\n"))
    print("    CMakeLists :", patch("CMakeLists.txt",
                                     "    tests/test_abi.cpp",
                                     "    tests/test_abi.cpp\n    tests/test_rhi.cpp"))

    print("=== 3) 双变体编译检查 ===")
    ok_default = compile_check("default", [])
    ok_optin = compile_check("optin", ["-DCOMPOSITOR_ENABLE_VULKAN_COMPUTE"])

    print("=== 4) 回归 + 外壳链接 ===")
    ok_tests = run([PY, "scripts/build.py"], "build.py (tests)")
    ok_shell = run([PY, "scripts/build_windows_shell.py"], "build_windows_shell.py")

    for name, path in (("tests", "build/compositor_tests.exe"),
                       ("shell", "build/compositor_shell.exe")):
        full = os.path.join(ROOT, path.replace("/", os.sep))
        print("    %s: %s bytes" % (name, os.path.getsize(full) if os.path.exists(full) else "MISSING"))

    print("=== 结论 ===")
    print("    default-compile=%s optin-compile=%s tests=%s shell=%s"
          % (ok_default, ok_optin, ok_tests, ok_shell))
    return 0 if (ok_default and ok_optin and ok_tests and ok_shell) else 1


if __name__ == "__main__":
    sys.exit(main())
