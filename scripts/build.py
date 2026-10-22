# AI生成
#!/usr/bin/env python3
"""compositor-ng 的可移植构建脚本（不依赖 CMake）。

用途：在没有 CMake / MSVC / MinGW 的环境下，借助 PyPI `ziglang` 自带的
clang 完成真实编译与测试运行；仅依赖 Python 3 与 pip。

用法：
    python scripts/build.py                 # 编译引擎库与测试，并运行测试
    python scripts/build.py --no-run        # 只编译
    python scripts/build.py --shared        # 真实共享构建：产出 compositor_engine.dll / libcompositor_engine.so
                                            # （定义 COMP_SHARED + COMP_SHARED_BUILD，使 comp_core.h 的
                                            #  COMP_API 切换为 dllexport / visibility("default")；
                                            #  共享构建只产出库，不编译也不运行单测）
    python scripts/build.py --driver clang  # 使用 PATH 中的 clang/clang++（需在 VS 开发者环境中运行）
    python scripts/build.py --driver msvc   # 显式使用 cl.exe（需已安装 VS 环境）
    python scripts/build.py --driver cc     # 使用 PATH 中的 cc/c++（gcc/clang）
    python scripts/build.py --gpu           # 编入 D3D12 后端（-DCOMP_HAS_D3D12），AUTO 才可能走 GPU

重要：`--driver msvc` 只对 C++ 源可用。engine/imaging/*.c 沿用原 macOS 工程的
clang block 语法（^），cl.exe 没有 blocks 扩展，会直接以 C2059 报错；
MSVC 环境请改用 `--driver clang`（VS 自带 LLVM 工具集，同为 MSVC ABI）。

`--gpu` 只影响编译期是否编入 D3D12 后端，不做链接期依赖（D3D12/编译器入口
全部运行期从 d3d12.dll / d3dcompiler_47.dll 解析）；未加该开关时后端如实报
「not compiled into this build」，AUTO 回落 CPU。外壳层复用 build/ 下的目标文件，
因此要用 GPU 路径时先跑 `--gpu` 再跑 build_windows_shell.py（外壳脚本同样接受
--gpu，保持编译口径一致）。
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "engine")
TESTS = os.path.join(ROOT, "tests")
BUILD = os.path.join(ROOT, "build")

# 默认工具链。build_windows_shell.py 复用本模块的 engine_object_paths() 计算
# 「同变体引擎对象在哪」，必须查到同一个 driver，因此把默认值提成常量而不是字面量。
DEFAULT_DRIVER = "zig"

INCLUDE_DIRS = [
    ENGINE,
    os.path.join(ENGINE, "abi", "include"),
    os.path.join(ENGINE, "abi", "src"),
]

# 可选第三方（vendor 在 engine/third_party 下）：目录存在才加入包含路径，
# 使 codec_lib.cpp 的 __has_include 探测生效；缺失时编解码如实回落为不可用。
for _vendor in ("zlib", "libpng", "stb"):
    _vendor_path = os.path.join(ENGINE, "third_party", _vendor)
    if os.path.isdir(_vendor_path):
        INCLUDE_DIRS.append(_vendor_path)

# FreeType（P5-0）。裁剪版目录结构与上游一致，包含根是 <root>/include 而不是 <root>，
# 所以不能走上面那条 vendor 规则。
FREETYPE_ROOT = os.path.join(ENGINE, "third_party", "freetype")
FREETYPE_INCLUDE = os.path.join(FREETYPE_ROOT, "include")

# **只编译聚合翻译单元。** 每个模块目录里除聚合文件外的 .c 都被聚合文件 #include
# （如 src/truetype/truetype.c 包含 ttobjs.c），若 glob 再单独编译它们就会重复定义。
# 因此 collect_sources() 把 third_party/freetype 从 engine/**/*.c 的 glob 里排除，
# 再追加下面这张显式清单。CMake 与 build_ohos.sh 必须保持同一份清单；
# 详见 engine/third_party/freetype/VENDOR-NOTES.md。
FREETYPE_SOURCES = [
    "src/base/ftsystem.c",
    "src/base/ftinit.c",
    "src/base/ftdebug.c",
    "src/base/ftbase.c",
    "src/base/ftbbox.c",
    "src/base/ftglyph.c",
    "src/base/ftbitmap.c",
    # TrueType GX/可变字体默认开启，sfnt 会调用 FT_Set_Named_Instance。
    "src/base/ftmm.c",
    "src/sfnt/sfnt.c",
    "src/truetype/truetype.c",
    "src/cff/cff.c",
    "src/psaux/psaux.c",
    "src/pshinter/pshinter.c",
    "src/psnames/psnames.c",
    "src/smooth/smooth.c",
]

if os.path.isdir(FREETYPE_INCLUDE):
    INCLUDE_DIRS.append(FREETYPE_INCLUDE)

# C 侧必须带 -fblocks：engine/imaging/*.c 沿用原 macOS 工程的 clang block 语法（^）
# 作为区间回调；非 Apple 平台的 BlocksRuntime 由 engine/core/comp_blocks_rt.c 提供。
# -std=gnu11 而非 c11：内核用到 M_PI 等 GNU/BSD 扩展宏（严格 ANSI 模式下不可见）。
C_FLAGS = ["-std=gnu11", "-O2", "-fblocks", "-D_USE_MATH_DEFINES"]
CXX_FLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
             # zig 自带的 libc++ 头文件带 nullability 注解但缺 _Nonnull 标注，
             # -Wall 下会刷出成百条 _Nonnull 建议，与本工程代码无关，关掉。
             "-Wno-nullability-completeness"]


def zig_prefix() -> list[str]:
    return [sys.executable, "-m", "ziglang"]


def resolve_toolchain(driver: str) -> tuple[list[str], list[str], list[str]]:
    """返回 (cc, cxx, linker) 命令前缀。

    注意：Windows 上 `subprocess` 不能直接执行 .bat/.cmd，因此各驱动一律
    要求真实的可执行文件（`cl` / `clang` / `cc` 都在 PATH 上查找）。
    """
    if driver == "msvc":
        return ["cl"], ["cl"], ["cl"]
    if driver == "cc":
        return ["cc"], ["c++"], ["c++"]
    if driver == "clang":
        cc = shutil.which("clang")
        cxx = shutil.which("clang++")
        if cc is None or cxx is None:
            raise SystemExit(
                "clang/clang++ not found on PATH; run inside a Visual Studio developer "
                "prompt, or set PATH to the LLVM toolset bin directory")
        return [cc], [cxx], [cxx]
    # 默认：ziglang（pip 安装后自带 clang 后端）
    return zig_prefix() + ["cc"], zig_prefix() + ["c++"], zig_prefix() + ["c++"]


def _is_freetype_source(path: str) -> bool:
    root = os.path.normcase(os.path.abspath(FREETYPE_ROOT)) + os.sep
    return os.path.normcase(os.path.abspath(path)).startswith(root)


def collect_sources() -> tuple[list[str], list[str], list[str]]:
    c_sources = [path for path in
                 sorted(glob.glob(os.path.join(ENGINE, "**", "*.c"), recursive=True))
                 if not _is_freetype_source(path)]
    if os.path.isdir(FREETYPE_ROOT):
        c_sources.extend(os.path.join(FREETYPE_ROOT, name) for name in FREETYPE_SOURCES)
        c_sources.sort()
    cxx_sources = sorted(glob.glob(os.path.join(ENGINE, "**", "*.cpp"), recursive=True))
    test_sources = sorted(glob.glob(os.path.join(TESTS, "*.cpp")))
    return c_sources, cxx_sources, test_sources


def child_env() -> dict:
    """构造子进程环境。

    两个坑：
      1. zig 需要全局缓存目录；若宿主环境缺 LOCALAPPDATA（部分 CI / 精简沙箱即如此），
         zig 会直接报 "unable to resolve zig cache directory: AppDataDirUnavailable"。
      2. 因此显式把缓存目录钉在 build/ 下，既不污染用户目录，也保证可随 build/ 一起清理。
    """
    env = dict(os.environ)
    cache = os.path.join(BUILD, ".zig-cache")
    fallback_profile = os.path.join(BUILD, ".appdata")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(fallback_profile, exist_ok=True)
    env.setdefault("LOCALAPPDATA", fallback_profile)
    env.setdefault("APPDATA", fallback_profile)
    env.setdefault("USERPROFILE", fallback_profile)
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(cache, "global")
    env["ZIG_LOCAL_CACHE_DIR"] = cache
    return env


def run(cmd: list[str], env: dict | None = None) -> None:
    printable = " ".join(cmd)
    print(f"[build] {printable}")
    subprocess.run(cmd, check=True, cwd=ROOT, env=env or child_env())


def obj_variant_prefix(shared: bool, gpu: bool) -> str:
    """目标文件的变体前缀（S23）：把「是否共享」「是否编入 D3D12」都编进文件名。

    之前只用 `shared_` 区分，GPU 与非 GPU 变体共用同一个 .o 路径，于是「先构建
    --gpu、再构建默认」会**复用上一次的 GPU 对象**，反之则静默降级到 CPU 桩
    （T2 在 `--gpu --shared` 上实测到的就是这个：壳链到了非 GPU 对象，AUTO 落到
    CPU 而没有任何报错）。让变体在路径上就分开，混用不再可能发生。
    """
    parts: list[str] = []
    if gpu:
        parts.append("gpu")
    if shared:
        parts.append("shared")
    return "_".join(parts) + "_" if parts else ""


def obj_path(source: str, driver: str, shared: bool = False, gpu: bool = False) -> str:
    rel = os.path.relpath(source, ROOT).replace("..", "_").replace("/", "_").replace("\\", "_")
    ext = ".obj" if driver == "msvc" else ".o"
    return os.path.join(BUILD, obj_variant_prefix(shared, gpu) + os.path.splitext(rel)[0] + ext)


def engine_object_paths(driver: str, shared: bool = False, gpu: bool = False) -> list[str]:
    """本变体下引擎全部目标文件的**预期**路径（壳构建脚本用它，而不是 glob）。

    glob `build/engine_*.o` 会把任意一次历史构建剩下的对象都当成本次输入——这正是
    静默降级的另一半原因。按变体算出确定路径，缺一个就报错，不给「凑巧能链上」的
    机会。
    """
    c_sources, cxx_sources, _ = collect_sources()
    return [obj_path(source, driver, shared, gpu) for source in c_sources + cxx_sources]


def compile_all(driver: str, shared: bool, gpu: bool = False) -> list[str]:
    cc, cxx, linker = resolve_toolchain(driver)
    os.makedirs(BUILD, exist_ok=True)

    c_sources, cxx_sources, test_sources = collect_sources()
    if not cxx_sources:
        raise SystemExit("no engine sources found")

    includes_c = [f"-I{path}" for path in INCLUDE_DIRS]
    includes_cxx = list(includes_c)

    # P2-3：--shared 是真实构建（不再是 documentation-only）。
    # 定义 COMP_SHARED + COMP_SHARED_BUILD，令 comp_core.h 的 COMP_API 切换成
    # dllexport / visibility("default")，符号才会从动态库里导出。
    defines: list[str] = []
    if shared:
        defines = ["-DCOMP_SHARED", "-DCOMP_SHARED_BUILD"]
        if os.name != "nt":
            defines.append("-fPIC")
    msvc_defines = ["/DCOMP_SHARED", "/DCOMP_SHARED_BUILD"] if shared else []
    # S13：D3D12 后端是 opt-in 构建；不定义 COMP_HAS_D3D12 时后端只有如实失败的桩。
    if gpu:
        defines.append("-DCOMP_HAS_D3D12")
        msvc_defines.append("/DCOMP_HAS_D3D12")

    objects: list[str] = []
    msvc = driver == "msvc"

    for source in c_sources:
        obj = obj_path(source, driver, shared, gpu)
        # FreeType 的 fterrors.h 在未定义 FT2_BUILD_LIBRARY 时会 #undef FT_ERR_PREFIX，
        # 于是库内部 FT_ERR(x) 拼不出 FT_Err_ 前缀（上游 autotools 正是靠这个宏编库）。
        extra = ["-DFT2_BUILD_LIBRARY"] if _is_freetype_source(source) else []
        if msvc:
            cmd = cc + ["/nologo", "/c", "/O2", "/utf-8", "/TC", source, f"/Fo{obj}"] + \
                  [f"/I{path}" for path in INCLUDE_DIRS] + msvc_defines
            if extra:
                cmd.append("/DFT2_BUILD_LIBRARY")
        else:
            cmd = cc + ["-c", source, "-o", obj] + C_FLAGS + includes_c + defines + extra
        run(cmd)
        objects.append(obj)

    # 共享构建不编译单测：单测必须静态链进引擎，与「导出符号的库」是两种产物。
    translation_units = list(cxx_sources) + ([] if shared else list(test_sources))
    for source in translation_units:
        obj = obj_path(source, driver, shared, gpu)
        if msvc:
            cmd = cxx + ["/nologo", "/c", "/O2", "/utf-8", "/EHsc", "/TP", source, f"/Fo{obj}"] + \
                  [f"/I{path}" for path in INCLUDE_DIRS] + msvc_defines
        else:
            cmd = cxx + ["-c", source, "-o", obj] + CXX_FLAGS + includes_cxx + defines
        run(cmd)
        objects.append(obj)

    if shared:
        lib = os.path.join(BUILD,
                           "compositor_engine.dll" if os.name == "nt" else "libcompositor_engine.so")
        if msvc:
            link = linker + ["/nologo", "/DLL", "/Fe:" + lib] + objects
        else:
            link = linker + objects + ["-shared", "-o", lib]
            if os.name != "nt":
                link.append("-pthread")
        run(link)
        print(f"[build] OK (shared) -> {lib}")
        return []

    binary = os.path.join(
        BUILD, "compositor_tests.exe" if os.name == "nt" else "compositor_tests")

    if msvc:
        link = linker + ["/nologo", "/Fe:" + binary] + objects
        # 单测里 S16 的窗口直呈用例要真实 HWND（CreateWindowExW）；非 Windows 不链。
        if os.name == "nt":
            link.append("user32.lib")
    else:
        link = linker + objects + ["-o", binary, "-pthread"]
        # 同上：Windows 上补 user32 供 CreateWindowExW/DestroyWindow。
        if os.name == "nt":
            link.append("-luser32")
    run(link)

    print(f"[build] OK -> {binary}")
    return [binary]


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and test compositor-ng engine")
    parser.add_argument("--no-run", action="store_true", help="only build")
    parser.add_argument("--shared", action="store_true",
                        help="build the C ABI as a shared library "
                             "(compositor_engine.dll / libcompositor_engine.so); "
                             "does not build or run the unit tests")
    parser.add_argument("--driver", default=DEFAULT_DRIVER, choices=["zig", "clang", "msvc", "cc"])
    parser.add_argument("--gpu", action="store_true",
                        help="compile the D3D12 backend (-DCOMP_HAS_D3D12); "
                             "without it AUTO resolves to CPU")
    parser.add_argument("--clean", action="store_true", help="remove build/ before building")
    args = parser.parse_args()

    if args.clean and os.path.isdir(BUILD):
        shutil.rmtree(BUILD)

    binaries = compile_all(args.driver, args.shared, args.gpu)
    if args.no_run:
        return 0

    for binary in binaries:
        run([binary])
    return 0


if __name__ == "__main__":
    sys.exit(main())
