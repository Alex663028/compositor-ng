# AI生成
#!/usr/bin/env bash
#
# run_ohos_tests.sh — OPT-20：把全量单测真正跑到 OHOS 设备/模拟器上（2026-10-25）。
#
# 背景：鸿蒙侧此前只有「交叉编译 rc=0」这一步在回归里（run_regression.py 第 6 步），
# 2026-10-24 的模拟器 705/705 是手工推二进制跑出来的，无脚本、日志未存档。
# 本脚本把那条手工链路固化：交叉编静态库（复用 build_ohos.sh）→ 编译 tests →
# 链接设备侧测试二进制 → hdc 推送 → 运行 → 解析 summary 行。
#
# 用法：
#   bash scripts/run_ohos_tests.sh                    # 默认 x86_64（DevEco 模拟器）
#   OHOS_ARCH=aarch64 bash scripts/run_ohos_tests.sh  # 真机
#   RUN_ARGS="txn_wild" bash scripts/run_ohos_tests.sh  # 透传给二进制的子串过滤（OPT-11）
#   OHOS_SKIP_LIB=1 bash scripts/run_ohos_tests.sh   # 静态库已是最新时跳过第 1 步
#
# 退出码：0 = summary 行显示 0 failed；1 = 失败/设备不在线。
set -u

ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
ARCH="${OHOS_ARCH:-x86_64}"
OUT="$ROOT/build/ohos_${ARCH}"
NDK="${OHOS_NDK:-G:/DevEco Studio/sdk/default/openharmony/native}"
HDC="${OHOS_HDC:-G:/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe}"
TARGET_ID="${OHOS_TARGET:-127.0.0.1:5555}"   # DevEco 模拟器默认；真机换 sn 或用 OHOS_TARGET 覆盖
REMOTE_DIR="/data/local/tmp/compositor"

# 与 build_ohos.sh 同一条口径（用户视角名 → 编译器三元组），分叉即编译不过。
case "$ARCH" in
  aarch64|arm64)   TRIPLE="aarch64" ;;
  armv7|arm|armhf) TRIPLE="arm" ;;
  x86_64|amd64)    TRIPLE="x86_64" ;;
  *)               TRIPLE="$ARCH" ;;
esac
TARGET="${TRIPLE}-linux-ohos"
SYSROOT="$NDK/sysroot"
BIN="$NDK/llvm/bin"

pick() {  # pick <basename> — 兼容 .exe 与无扩展名两种打包形态（与 build_ohos.sh 同款）
  for candidate in "$BIN/$1" "$BIN/$1.exe"; do
    if [ -x "$candidate" ] || [ -f "$candidate" ]; then echo "$candidate"; return 0; fi
  done
  return 1
}

CXX="$(pick "clang++")" || { echo "[ohos-test] 未找到 clang++，请检查 OHOS_NDK=$NDK" >&2; exit 2; }
if [ ! -d "$SYSROOT" ]; then
  echo "[ohos-test] sysroot 不存在: $SYSROOT" >&2; exit 2
fi
if [ ! -f "$HDC" ]; then
  echo "[ohos-test] 未找到 hdc: $HDC（可用 OHOS_HDC 覆盖路径）" >&2; exit 2
fi

echo "[ohos-test] arch=$ARCH target=$TARGET device=$TARGET_ID"

# ------------------------------------------------------------- 1. 引擎静态库
if [ "${OHOS_SKIP_LIB:-0}" != "1" ]; then
  OHOS_ARCH="$ARCH" bash "$ROOT/scripts/build_ohos.sh" || exit 1
fi
LIB="$OUT/libcompositor_engine_ohos.a"
[ -f "$LIB" ] || { echo "[ohos-test] 缺 $LIB（先跑 build_ohos.sh）" >&2; exit 1; }

# ------------------------------------------------------------- 2. 编译 tests
# 注意：测试对象放 $OUT/tests/ 子目录，**不能**平铺在 $OUT —— build_ohos.sh 归档时
# `ar rcs ./*.o` 会把平铺的测试对象（带 main）卷进引擎静态库，污染 .so 壳链接。
# include 口径与 build_ohos.sh 完全一致（third_party 存在才加，保证编解码探测同源）。
CPPFLAGS="-I$ROOT/engine -I$ROOT/engine/abi/include -I$ROOT/engine/abi/src"
for _vendor in zlib libpng stb; do
  [ -d "$ROOT/engine/third_party/$_vendor" ] && CPPFLAGS="$CPPFLAGS -I$ROOT/engine/third_party/$_vendor"
done
[ -d "$ROOT/engine/third_party/freetype/include" ] && \
  CPPFLAGS="$CPPFLAGS -I$ROOT/engine/third_party/freetype/include"

TESTS_OUT="$OUT/tests"
mkdir -p "$TESTS_OUT"
ok=0; fail=0; failed_list=""
for src in $(cd "$ROOT" && ls tests/*.cpp | sort); do
  obj="$TESTS_OUT/$(basename "$src" .cpp).o"
  if "$CXX" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -fvisibility=hidden \
        -std=c++17 -O2 -c "$ROOT/$src" $CPPFLAGS -o "$obj" 2>"$TESTS_OUT/last_error.log"; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1)); failed_list="$failed_list $src"
  fi
done
echo "[ohos-test] tests compile ok=$ok fail=$fail"
if [ "$fail" != "0" ]; then
  echo "[ohos-test] failed:$failed_list" >&2
  head -14 "$TESTS_OUT/last_error.log" >&2
  exit 1
fi

# ------------------------------------------------------------- 3. 链接
# 无 user32（S16 窗口直呈用例在非 _WIN32 下编译期排除）；musl 的 pthread/dlopen
# 都在 libc 里，不额外链。
TESTS_BIN="$OUT/compositor_tests"
if ! "$CXX" --target="$TARGET" --sysroot="$SYSROOT" -fPIC -O2 \
     "$TESTS_OUT"/*.o "$LIB" -o "$TESTS_BIN" 2>"$TESTS_OUT/link_error.log"; then
  echo "[ohos-test] link failed:" >&2
  head -20 "$TESTS_OUT/link_error.log" >&2
  exit 1
fi
echo "[ohos-test] binary: $TESTS_BIN"

# ------------------------------------------------------------- 4. 设备在线检查
# hdc 的三个坑（2026-10-25 实测，曾让整轮设备测试跑了旧二进制还全然不知）：
#   1. Git Bash 把 /data/... 远端路径按 MSYS 规则翻译成 Windows 路径，hdc 在设备端
#      报 "read-only file system, path:./E:..."——所有 hdc 调用必须带
#      MSYS_NO_PATHCONV=1（Linux 宿主没有该机制，设了也无害）。
#   2. hdc 不认 "E:/..." 正斜杠盘符绝对路径（会当相对路径拼到 cwd 前面）；
#      本地路径经 cygpath -w 转成 "E:\..." 反斜杠形式。
#   3. hdc 失败时退出码仍可能是 0 ——「|| exit 1」防不住，推送必须检查输出里
#      真的出现了 FileTransfer finish。
hdc_cmd() { MSYS_NO_PATHCONV=1 "$HDC" -t "$TARGET_ID" "$@"; }
to_win() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}
send_file() {  # send_file <local> <remote>；失败返回非 0
  local out
  out="$(hdc_cmd file send "$(to_win "$1")" "$2")"
  case "$out" in *FileTransfer\ finish*) return 0 ;; esac
  echo "[ohos-test] 推送失败: $1 -> $2" >&2
  echo "$out" >&2
  return 1
}

if ! hdc_cmd shell "echo ok" >/dev/null 2>&1; then
  echo "[ohos-test] 设备不在线: $TARGET_ID（hdc list targets:）" >&2
  "$HDC" list targets >&2
  exit 1
fi

# ------------------------------------------------------------- 5. 推送
# 测试以 cwd 相对路径读 resources/（camera_raw 表 + Noto 字体子集；缺表用例会
# 如实报断言失败，所以两样都必须推）。hdc file send 不会建目录，先 mkdir。
hdc_cmd shell "mkdir -p $REMOTE_DIR/resources/fonts" || exit 1
send_file "$TESTS_BIN" "$REMOTE_DIR/compositor_tests" || exit 1
send_file "$ROOT/resources/camera_raw_tables.bin" "$REMOTE_DIR/resources/camera_raw_tables.bin" || exit 1
send_file "$ROOT/resources/fonts/NotoSansSC-Regular-Subset.ttf" \
    "$REMOTE_DIR/resources/fonts/NotoSansSC-Regular-Subset.ttf" || exit 1
# 推送核验：二进制大小必须与本地一致（防「跑了设备上的旧文件」——2026-10-25 的
# 事故：send 静默失败 + rc=0，结果跑的是前一天的旧 compositor_tests）。
local_size=$(wc -c < "$TESTS_BIN" | tr -d ' ')
remote_size="$(hdc_cmd shell "stat -c %s $REMOTE_DIR/compositor_tests" | tr -d '\r\n')"
if [ "$remote_size" != "$local_size" ]; then
  echo "[ohos-test] 推送核验失败：本地 $local_size 字节 vs 设备 ${remote_size:-?} 字节" >&2
  exit 1
fi
echo "[ohos-test] 推送核验通过：$remote_size 字节"

# ------------------------------------------------------------- 6. 运行 + 解析
RUN_ARGS="${RUN_ARGS:-}"
echo "[ohos-test] run: ./compositor_tests $RUN_ARGS  (cwd=$REMOTE_DIR)"
hdc_cmd shell \
  "cd $REMOTE_DIR && chmod +x compositor_tests && ./compositor_tests $RUN_ARGS" | \
  tee "$OUT/device_run.log" | grep -vE "^\[ (RUN|OK)"
# 上行把逐用例行收进 device_run.log 存档，终端只留 summary 与失败详情。

summary="$(grep -E "^summary: [0-9]+ cases, [0-9]+ failed" "$OUT/device_run.log" | tail -1)"
if [ -z "$summary" ]; then
  echo "[ohos-test] FAIL：未见 summary 行（崩溃或输出被截断），完整日志：$OUT/device_run.log" >&2
  exit 1
fi
echo "[ohos-test] $summary"
if echo "$summary" | grep -q ", 0 failed,"; then
  echo "[ohos-test] PASS"
  exit 0
fi
echo "[ohos-test] FAIL（失败详情见上方 failure 行 / $OUT/device_run.log）" >&2
exit 1
