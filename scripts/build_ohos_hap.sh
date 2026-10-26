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

# JAVA_HOME：hvigor 的打包阶段要起 java（签名 / 打包工具链）。
# 机器上常常**没有 java 在 PATH**（DevEco 自带 JBR，但不注册到系统 PATH），
# 于是打包阶段报 `spawn java ENOENT` —— 那个错看着像打包器坏了，其实是
# PATH 问题。这里优先用 DevEco 自带的 JBR，找不到再退回系统 java。
#
# 探测用 POSIX 路径（/g/...）：DEVECO_SDK_HOME 是 Windows 形式（G://...），
# 拿它拼出来的候选路径带双斜杠与盘符，MSYS 下 [ -f ] 判不出来。
if [ -z "${JAVA_HOME:-}" ]; then
  DEVECO_ROOT_POSIX="$(cygpath -u "G:/DevEco Studio" 2>/dev/null || echo "/g/DevEco Studio")"
  for candidate in "$DEVECO_ROOT_POSIX/jbr" "/c/Program Files/Huawei/DevEco Studio/jbr"; do
    if [ -f "$candidate/bin/java.exe" ]; then
      JAVA_HOME="$(cygpath -w "$candidate" 2>/dev/null || echo "$candidate")"
      break
    fi
  done
fi
if [ -n "${JAVA_HOME:-}" ]; then
  export JAVA_HOME
  JAVA_BIN_POSIX="$(cygpath -u "$JAVA_HOME/bin" 2>/dev/null || echo "$JAVA_HOME/bin")"
  PATH="$JAVA_BIN_POSIX:$PATH"
  export PATH
fi
JAVA_BIN="$(command -v java || true)"
[ -n "$JAVA_BIN" ] || echo "[hap] 警告：找不到 java（打包阶段可能报 spawn java ENOENT）" >&2

cd "$APP_DIR" || exit 1
"$NODE" "$HVIGOR" --mode module -p product=default -p module=entry@default \
  -p buildMode=debug --no-daemon assembleHap || exit 1

/*
 * 找 HAP 产物。
 *
 * **优先用 signed 的**：工程配好 DevEco 自动签名后，hvigor 会额外产出
 * entry-default-signed.hap——那个才能装到**真机**上（unsigned 的模拟器能装、
 * 真机装不上：系统校验签名，profile 里还得含本机 UDID）。
 * 没配签名时只有 unsigned 的，模拟器照常能跑。
 */
SIGNED_HAP="$APP_DIR/entry/build/default/outputs/default/entry-default-signed.hap"
UNSIGNED_HAP="$APP_DIR/entry/build/default/outputs/default/entry-default-unsigned.hap"
if [ -f "$SIGNED_HAP" ]; then
  HAP="$SIGNED_HAP"
  echo "[hap] OK -> $HAP（已签名；真机可装）"
elif [ -f "$UNSIGNED_HAP" ]; then
  HAP="$UNSIGNED_HAP"
  echo "[hap] OK -> $HAP（unsigned；模拟器可装，**真机需要 DevEco 自动签名**）"
  if [ "$TARGET_ID" != "127.0.0.1:5555" ]; then
    echo "[hap] 提示：目标是真机（$TARGET_ID），unsigned HAP 装不上——请在 DevEco 里"
    echo "      配置一次自动签名（File > Project Structure > Signing Configs >"
    echo "      Automatically generate signature > 勾选本机设备 > Apply），之后本脚本"
    echo "      会自动改用 signed 产物。" >&2
  fi
else
  echo "[hap] 未产出 HAP（找过 $SIGNED_HAP 与 $UNSIGNED_HAP）" >&2
  exit 1
fi

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
