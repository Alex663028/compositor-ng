# hilog: OH_LOG_* needs libhilog_ndk.z.so at link -- without it dlopen relocation
# fails and the WHOLE NAPI module fails to load (2026-10-26 emulator; the ArkTS error
# 'module does not provide an export' is the misleading symptom).
# AI生成
#!/usr/bin/env bash
# AI生成
#
# build_ohos_shell.sh — 把鸿蒙外壳（NAPI 桥）交叉编译成 {arch}-linux-ohos 共享库。
#
# 前置：
#   1. scripts/build_ohos.sh 已产出 build/ohos_{arch}/libcompositor_engine_ohos.a
#      （aarch64 / x86_64 / armv7 均可，缺失时本脚本会自动先跑一遍）。
#   2. DevEco Studio 的 openharmony native sysroot（默认 G 盘，可用 OHOS_SDK 覆盖）。
#
# 产物：build/ohos_{arch}/libcompositor_ohos.so（ArkTS 侧 XComponent/libraryname 加载）
#
# 用法：
#   bash scripts/build_ohos_shell.sh                 # 默认 aarch64（真机）
#   OHOS_ARCH=x86_64 bash scripts/build_ohos_shell.sh # 模拟器（本地 x86_64 虚拟设备）
#
# 注：不使用 {arch}-unknown-linux-ohos-clang(++) wrapper —— 该 wrapper 内部路径未加引号，
#     SDK 路径含空格时必然失败；这里直接调用 llvm/bin/clang++ 并显式传 --target / --sysroot。
set -u

# 同 build_ohos.sh：ROOT 用原生路径（pwd -W），否则 MSYS_NO_PATHCONV=1 时
# 传给原生 clang++ 的 /e/... 不会被翻译，链接必失败。
ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
SDK="${OHOS_SDK:-G:/DevEco Studio/sdk/default/openharmony}"
NATIVE="$SDK/native"
SYSROOT="$NATIVE/sysroot"
CLANGXX="$NATIVE/llvm/bin/clang++.exe"
ARCH="${OHOS_ARCH:-aarch64}"
case "$ARCH" in
  aarch64|arm64)   TRIPLE="aarch64" ;;
  armv7|arm|armhf) TRIPLE="arm" ;;
  x86_64|amd64)    TRIPLE="x86_64" ;;
  *)               TRIPLE="$ARCH" ;;
esac
TARGET="${TRIPLE}-linux-ohos"
ARCH_LIB_DIR="$SYSROOT/usr/lib/${TRIPLE}-linux-ohos"
OUT_DIR="$ROOT/build/ohos_${ARCH}"
ENGINE_LIB="$OUT_DIR/libcompositor_engine_ohos.a"
SHELL_SO="$OUT_DIR/libcompositor_ohos.so"

if [ ! -x "$CLANGXX" ]; then
  echo "[ohos-shell] 未找到 clang++: $CLANGXX" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"

if [ ! -f "$ENGINE_LIB" ]; then
  echo "[ohos-shell] 引擎静态库缺失，先执行 scripts/build_ohos.sh (OHOS_ARCH=$ARCH)"
  OHOS_ARCH="$ARCH" bash "$ROOT/scripts/build_ohos.sh" || exit 1
fi

# 链接依赖（均在 sysroot 中实测存在）：
#   ace_napi.z     → napi_* 运行时
#   ace_ndk.z      → OH_NativeXComponent_* 回调注册
#   native_window  → OHNativeWindow*（surface 承载）
"$CLANGXX" \
  --target="$TARGET" \
  --sysroot="$SYSROOT" \
  -std=c++17 -O2 -fPIC -shared \
  -Wall -Wextra -Wno-unused-parameter \
  -I "$ROOT/engine/abi/include" \
  "$ROOT/shells/ohos/src/napi_init.cpp" \
  "$ENGINE_LIB" \
  -L"$ARCH_LIB_DIR" \
  -lace_napi.z -lace_ndk.z -lnative_window -l:libhilog_ndk.z.so \
  -o "$SHELL_SO" || {
    echo "[ohos-shell] 链接失败" >&2
    exit 1
  }

echo "[ohos-shell] OK ($TARGET) -> $(ls -l "$SHELL_SO" | awk '{print $5" bytes  "$9}')"
