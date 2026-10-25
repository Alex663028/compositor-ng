# AI生成
#!/usr/bin/env bash
#
# build_ohos_hap.sh — OPT-22：鸿蒙壳 HAP 一键构建（2026-10-25 首次跑通模拟器）。
#
# 链路：build_ohos_shell.sh 交叉编 NAPI .so → 拷入 DevEco 工程 entry/libs/<abi>/
# （重命名为 libcompositor.so 并随包 libc++_shared.so）→ hvigor assembleHap
# → 可选 --install/--launch 直上模拟器。
#
# 已实测（2026-10-25，DevEco 模拟器 127.0.0.1:5555，x86_64）：
#   unsigned HAP 可安装启动；NAPI 模块加载成功，startup→引擎就绪（CPU 后端，
#   AUTO 诚实回落）、surface 回调 ready（2030x456）、flatten 8294400 字节、
#   presentSurface 如实返回 rc=4 UNSUPPORTED（交换链未实现，见任务卡 OPT-21/24）。
#   真机需要 AGC 签名 profile（模拟器接受 unsigned，Ekko 工程文档实测结论）。
#
# 用法：
#   bash scripts/build_ohos_hap.sh                 # 构建（默认 x86_64 模拟器）
#   OHOS_ARCH=aarch64 bash scripts/build_ohos_hap.sh   # 真机 arm（libs/arm64-v8a）
#   bash scripts/build_ohos_hap.sh --install --launch  # 构建+装机+启动
set -u

ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
ARCH="${OHOS_ARCH:-x86_64}"
NDK="${OHOS_NDK:-G:/DevEco Studio/sdk/default/openharmony/native}"
HDC="${OHOS_HDC:-G:/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe}"
TARGET_ID="${OHOS_TARGET:-127.0.0.1:5555}"
APP_DIR="$ROOT/shells/ohos"

# HAP 的 libs 目录用 Android 风格 ABI 名：x86_64 同名，aarch64 是 arm64-v8a。
case "$ARCH" in
  aarch64|arm64) ABI="arm64-v8a" ;;
  *)             ABI="$ARCH" ;;
esac

INSTALL=0; LAUNCH=0
for arg in "$@"; do
  case "$arg" in
    --install) INSTALL=1 ;;
    --launch)  LAUNCH=1; INSTALL=1 ;;
  esac
done

echo "[hap] arch=$ARCH abi=$ABI"

# ------------------------------------------------------------- 1. NAPI .so
OHOS_ARCH="$ARCH" bash "$ROOT/scripts/build_ohos_shell.sh" || exit 1
SO_SRC="$ROOT/build/ohos_${ARCH}/libcompositor_ohos.so"
[ -f "$SO_SRC" ] || { echo "[hap] missing $SO_SRC" >&2; exit 1; }

# ------------------------------------------------------------- 2. 工程 libs
# 重命名原因：Index.ets import 'libcompositor.so' 与 napi_module.nm_modname
# （"compositor"）按文件名匹配；libc++_shared.so 是 .so 的动态依赖，必须随包
# （否则应用进程加载报 "Error loading shared library libc++_shared.so"，实测）。
LIBS_DIR="$APP_DIR/entry/libs/$ABI"
mkdir -p "$LIBS_DIR"
cp "$SO_SRC" "$LIBS_DIR/libcompositor.so"
LIBCXX=$(find "$NDK/llvm/lib" -name "libc++_shared.so" -path "*${ARCH}*" | head -1)
[ -n "$LIBCXX" ] || { echo "[hap] NDK 里找不到 ${ARCH} 的 libc++_shared.so" >&2; exit 1; }
cp "$LIBCXX" "$LIBS_DIR/libc++_shared.so"
echo "[hap] libs: $LIBS_DIR (libcompositor.so + libc++_shared.so)"

# ------------------------------------------------------------- 3. hvigor 打包
# 环境变量口径抄自 ../EkkoStudioHarmonyOS/build.sh：DEVECO_SDK_HOME 双斜杠形式，
# hvigor 用 DevEco 自带（工程不装 node_modules，否则 Sync 报 root node 不可用）。
export DEVECO_SDK_HOME="${DEVECO_SDK_HOME:-G://DevEco Studio//sdk}"
export HOS_SDK_HOME="${HOS_SDK_HOME:-G://DevEco Studio//sdk//default}"
export NODE_HOME="${NODE_HOME:-G://DevEco Studio//tools//node}"
NODE="$NODE_HOME/node.exe"
[ -f "$NODE" ] || NODE="$(command -v node)"
HVIGOR="$(dirname "$DEVECO_SDK_HOME")/tools/hvigor/bin/hvigorw.js"
[ -f "$HVIGOR" ] || HVIGOR="G:/DevEco Studio/tools/hvigor/bin/hvigorw.js"

cd "$APP_DIR" || exit 1
"$NODE" "$HVIGOR" --mode module -p product=default -p module=entry@default \
  -p buildMode=debug --no-daemon assembleHap || exit 1

HAP="$APP_DIR/entry/build/default/outputs/default/entry-default-unsigned.hap"
[ -f "$HAP" ] || { echo "[hap] 未产出 $HAP" >&2; exit 1; }
echo "[hap] OK -> $HAP（unsigned；模拟器可装，真机需 AGC 签名）"

# ------------------------------------------------------------- 4. 装机 / 启动
hdc_cmd() { MSYS_NO_PATHCONV=1 "$HDC" -t "$TARGET_ID" "$@"; }
if [ "$INSTALL" = "1" ]; then
  WIN_HAP=$(cygpath -w "$HAP" 2>/dev/null || echo "$HAP")
  hdc_cmd uninstall com.ekkostudio.compositor >/dev/null 2>&1
  hdc_cmd install "$WIN_HAP" || exit 1
  echo "[hap] installed (com.ekkostudio.compositor)"
fi
if [ "$LAUNCH" = "1" ]; then
  hdc_cmd shell "aa start -a EntryAbility -b com.ekkostudio.compositor" || exit 1
  echo "[hap] launched；验证：hdc shell hilog | grep compositor-shell"
fi
