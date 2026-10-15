# AI生成
#!/usr/bin/env bash
# compositor-ng 构建入口（POSIX / Git Bash）
#   ./scripts/build.sh            编译并运行测试
#   ./scripts/build.sh --no-run   只编译
#   COMP_DRIVER=cc ./scripts/build.sh  改用系统 cc/c++
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python}"
DRIVER="${COMP_DRIVER:-zig}"

cd "$ROOT"
exec "$PYTHON_BIN" scripts/build.py --driver "$DRIVER" "$@"
