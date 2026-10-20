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
OUT="$ROOT/build/ohos_${ARCH}"

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
# 注意（P2-4，能力边界）：这里刻意**不**加入 engine/third_party/{zlib,libpng,stb}。
# 因此 codec_lib.cpp 的 __has_include("png.h") 探测失败 -> 鸿蒙侧 PNG 静默回落到自研
# comp_png（回落本身是如实的，符合「不假装成功」），但与宿主构建（libpng+vendored）
# 不是同一条编解码路径。该事实已写入 README「能力边界」一节；若要统一，
# 应把 third_party 加入下面的包含路径并确认 zlib/libpng 能过 OHOS 交叉编译后一并开启。
CPPFLAGS="-I$ROOT/engine -I$ROOT/engine/abi/include -I$ROOT/engine/abi/src"

echo "[ohos] target=$TARGET"
echo "[ohos] sysroot=$SYSROOT"
echo "[ohos] out=$OUT"

ok=0
fail=0
failed_list=""

for src in $(cd "$ROOT" && find engine -name "*.c" | sort); do
  obj="$OUT/$(echo "$src" | tr '/' '_').o"
  if "$CC" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -fvisibility=hidden -std=gnu11 -O2 -fblocks $DEFINES \
        -c "$ROOT/$src" $CPPFLAGS -o "$obj" 2>"$OUT/last_error.log"; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); failed_list="$failed_list $src"
  fi
done

for src in $(cd "$ROOT" && find engine -name "*.cpp" | sort); do
  obj="$OUT/$(echo "$src" | tr '/' '_').o"
  if "$CXX" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -fvisibility=hidden -std=c++17 -O2 -fblocks $DEFINES \
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
