# AI生成
#!/usr/bin/env bash
# AI生成
#
# build_ohos.sh — 把 compositor-ng 引擎核交叉编译到 HarmonyOS (aarch64-linux-ohos)。
#
# 为什么需要这个脚本（而不是直接用 DevEco 的 wrapper）：
#   DevEco 自带的 llvm/bin/aarch64-unknown-linux-ohos-clang(++) 是 shell wrapper，
#   其内部对安装路径未加引号；当 SDK 装在含空格的目录（如 "G:/DevEco Studio/..."）时
#   会报 `G:/DevEco: No such file or directory`。这里改为直接调用 llvm/bin/clang(.exe)，
#   显式给出 --target 与 --sysroot，绕开 wrapper。
#
# 已验证环境：DevEco Studio DS-243.24978.46.36.602650
#   SDK: sdk/default/openharmony/native
#   头: sysroot/usr/include/{vulkan,native_window,ace/xcomponent}
#   库: sysroot/usr/lib/aarch64-linux-ohos/{libvulkan.so,libnative_window.so}
#
# 用法：
#   bash scripts/build_ohos.sh                     # 默认 aarch64（真机）
#   OHOS_ARCH=x86_64 bash scripts/build_ohos.sh     # 模拟器（本地 x86_64 虚拟设备）
#   OHOS_ARCH=armv7 bash scripts/build_ohos.sh      # 32 位
#   OHOS_NDK=/path/to/native bash scripts/build_ohos.sh
#   OHOS_ASAN=1 bash scripts/build_ohos.sh          # ASan 变体：独立输出目录
#                                                  # build/ohos_${ARCH}_asan，对象不与
#                                                  # 普通变体混链（S23 纪律；NDK 自带
#                                                  # clang_rt.asan，与 Windows+zig 不同）
#
# 目标三元组映射：OpenHarmony 的 32 位 ARM 目标三元组是 arm-linux-ohos（而非 armv7-），
# 故 OHOS_ARCH（用户视角的名字）与三元组（编译器视角）分开维护；
# sysroot 实测同时具备 aarch64 / arm / x86_64 三套 usr/lib 目录。
set -u

# ROOT 必须是「原生路径」形式：DevEco 的 clang(.exe) 是 Windows 原生程序，
# MSYS/MinGW 下 $(pwd) 给出的是 /e/... 形式；当 MSYS_NO_PATHCONV=1（或
# MSYS2_ARG_CONV_EXCL=*）时 MSYS 不会把该参数翻译成 E:/...，原生 clang 会
# 直接报 `no such file or directory`（实测 ok=0 fail=91，且 5 秒内全灭）。
# 故优先用 pwd -W（MSYS 专有，输出 E:/… 正斜杠形式）；Linux/macOS 没有 -W，
# 回落到 pwd，那里本来就是原生路径，无需翻译。
ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
NDK="${OHOS_NDK:-G:/DevEco Studio/sdk/default/openharmony/native}"
SYSROOT="$NDK/sysroot"
BIN="$NDK/llvm/bin"
ARCH="${OHOS_ARCH:-aarch64}"
case "$ARCH" in
  aarch64|arm64)   TRIPLE="aarch64" ;;
  armv7|arm|armhf) TRIPLE="arm" ;;
  x86_64|amd64)    TRIPLE="x86_64" ;;
  *)               TRIPLE="$ARCH" ;;
esac
TARGET="${TRIPLE}-linux-ohos"
# ASan 变体（配合 scripts/run_ohos_tests.sh OHOS_ASAN=1）：独立输出目录防对象混链。
ASAN_FLAGS=""
OUT="$ROOT/build/ohos_${ARCH}"
if [ "${OHOS_ASAN:-0}" = "1" ]; then
  ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
  OUT="$ROOT/build/ohos_${ARCH}_asan"
fi

pick() {  # pick <basename> — 兼容 .exe 与无扩展名两种打包形态
  for candidate in "$BIN/$1" "$BIN/$1.exe"; do
    if [ -x "$candidate" ] || [ -f "$candidate" ]; then echo "$candidate"; return 0; fi
  done
  return 1
}

CC="$(pick "clang")" || { echo "[ohos] 未找到 clang，请检查 OHOS_NDK=$NDK" >&2; exit 2; }
CXX="$(pick "clang++")" || { echo "[ohos] 未找到 clang++" >&2; exit 2; }
AR="$(pick "llvm-ar")"

if [ ! -d "$SYSROOT" ]; then
  echo "[ohos] sysroot 不存在: $SYSROOT（请先安装 OpenHarmony Native SDK）" >&2
  exit 2
fi

mkdir -p "$OUT"
# 后端开关（P2-5）：D3D12 是 Windows 专属，d3d12_backend.cpp 以 COMP_HAS_D3D12 正向开关
# 保留；交叉编译到 OHOS 时该宏天然未定义，所以这里**不需要**任何 DEFINES。
# 原 `-DCOMP_NO_D3D12=1` 是全仓零引用的死开关（全仓无一处引用 COMP_NO_D3D12），已删除，
# 以免后来者误以为「显式关掉了 D3D12」。
DEFINES=""
CPPFLAGS="-I$ROOT/engine -I$ROOT/engine/abi/include -I$ROOT/engine/abi/src"

# libpng 的 ARM NEON 优化必须**显式关掉**（PNG_ARM_NEON_OPT=0）。
#
# 背景：本仓的 libpng 是**裁剪过的**（只留编解码必需的 .c，arm/ 整个目录
# 不在仓里——见 engine/third_party 的裁剪约定）。但 libpng 的 pngpriv.h 里
# 有这么一段自动判定：
#
#     #if (defined(__ARM_NEON__) || defined(__ARM_NEON)) && \
#        defined(PNG_ALIGNED_MEMORY_SUPPORTED)
#     #  define PNG_ARM_NEON_OPT 2      // 开 NEON
#     #else
#     #  define PNG_ARM_NEON_OPT 0
#     #endif
#
# aarch64 上 __ARM_NEON__ **总是定义**的 → PNG_ARM_NEON_OPT=2 → pngpriv.h
# 把 png_do_read_transformations 之类改写成调用 png_do_expand_palette_rgba8_neon
# / png_init_filter_functions_neon —— 而这些函数定义在被裁掉的 arm/*.c 里，
# 于是**链接期**报 undefined symbol。
#
# 这个坑只在真机（aarch64）上出现，模拟器（x86_64）没有 __ARM_NEON__，
# PNG_ARM_NEON_OPT 自动为 0，所以之前一直「测过是好的」。
#
# pngpriv.h 第 126-128 行的注释本身就给了正解：把 -DPNG_ARM_NEON_OPT=0
# 放进 CPPFLAGS。代价是 ARM 上 PNG 解码走纯 C 路径（比 NEON 慢一些），
# 换来的是**不依赖被裁掉的源文件**——这正是裁剪 vendor 的前提。
CPPFLAGS="$CPPFLAGS -DPNG_ARM_NEON_OPT=0"

# 编解码后端（P2-4）：与 scripts/build.py 的 INCLUDE_DIRS 同一条口径——目录存在才加入。
# 这些目录下的 .c 本来就被上面的 `find engine -name "*.c"` 编进静态库，此前只是没给
# include 路径，导致 codec_lib.cpp 的 __has_include("png.h") / ("stb_image.h") 探测失败：
# PNG 静默回落自研 comp_png、JPEG/HDR/EXR 直接不可用。补上后鸿蒙与宿主走同一套编解码。
# 缺目录时不报错（保持「不假装成功」：探测失败仍然如实降级），但会打印一行提示。
_third_party_missing=""
for _vendor in zlib libpng stb; do
  if [ -d "$ROOT/engine/third_party/$_vendor" ]; then
    CPPFLAGS="$CPPFLAGS -I$ROOT/engine/third_party/$_vendor"
  else
    _third_party_missing="$_third_party_missing $_vendor"
  fi
done
if [ -n "$_third_party_missing" ]; then
  echo "[ohos] 提示：third_party 缺少$_third_party_missing，编解码将降级（PNG 走自研回落，JPEG/HDR/EXR 不可用）" >&2
fi

# FreeType（P5-0）：与 build.py / CMakeLists.txt 同一份「聚合翻译单元」清单。
# 目录中的其余 .c 被聚合文件 #include，不能再单独编译；下面的 find 必须排除整棵树。
# 裁剪说明见 engine/third_party/freetype/VENDOR-NOTES.md。
FT_ROOT="engine/third_party/freetype"
FT_SOURCES="src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c \
src/base/ftbase.c src/base/ftbbox.c src/base/ftglyph.c src/base/ftbitmap.c src/base/ftmm.c \
src/sfnt/sfnt.c src/truetype/truetype.c src/cff/cff.c src/psaux/psaux.c \
src/pshinter/pshinter.c src/psnames/psnames.c src/smooth/smooth.c"
if [ -d "$ROOT/$FT_ROOT/include" ]; then
  CPPFLAGS="$CPPFLAGS -I$ROOT/$FT_ROOT/include"
fi

echo "[ohos] target=$TARGET"
echo "[ohos] sysroot=$SYSROOT"
echo "[ohos] out=$OUT"

ok=0
fail=0
failed_list=""

for src in $(cd "$ROOT" && find engine -name "*.c" ! -path "$FT_ROOT/*" | sort); do
  obj="$OUT/$(echo "$src" | tr '/' '_').o"
  if "$CC" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -fvisibility=hidden -std=gnu11 -O2 -fblocks $DEFINES $ASAN_FLAGS \
        -c "$ROOT/$src" $CPPFLAGS -o "$obj" 2>"$OUT/last_error.log"; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); failed_list="$failed_list $src"
  fi
done

if [ -d "$ROOT/$FT_ROOT" ]; then
  for ft in $FT_SOURCES; do
    src="$FT_ROOT/$ft"
    obj="$OUT/$(echo "$src" | tr '/' '_').o"
    # -DFT2_BUILD_LIBRARY：fterrors.h 在没有它时会 #undef FT_ERR_PREFIX（上游编库口径）。
    if "$CC" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -fvisibility=hidden -std=gnu11 -O2 -fblocks \
          -DFT2_BUILD_LIBRARY $DEFINES $ASAN_FLAGS -c "$ROOT/$src" $CPPFLAGS -o "$obj" 2>"$OUT/last_error.log"; then
      ok=$((ok + 1))
    else
      fail=$((fail + 1)); failed_list="$failed_list $src"
    fi
  done
fi

for src in $(cd "$ROOT" && find engine -name "*.cpp" | sort); do
  obj="$OUT/$(echo "$src" | tr '/' '_').o"
  if "$CXX" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -fvisibility=hidden -std=c++17 -O2 -fblocks $DEFINES $ASAN_FLAGS \
        -c "$ROOT/$src" $CPPFLAGS -o "$obj" 2>"$OUT/last_error.log"; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); failed_list="$failed_list $src"
  fi
done

echo "[ohos] compile ok=$ok fail=$fail"
if [ "$fail" != "0" ]; then
  echo "[ohos] failed:$failed_list" >&2
  echo "[ohos] 最后一次错误：" >&2
  head -14 "$OUT/last_error.log" >&2
  exit 1
fi

if [ -n "$AR" ]; then
  rm -f "$OUT/libcompositor_engine_ohos.a"
  (cd "$OUT" && "$AR" rcs "libcompositor_engine_ohos.a" ./*.o >/dev/null 2>&1) \
    && echo "[ohos] archive: $OUT/libcompositor_engine_ohos.a"
fi

echo "[ohos] done"
