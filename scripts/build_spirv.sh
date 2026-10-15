# AI生成
#!/usr/bin/env bash
# AI生成
#
# build_spirv.sh — 把 render/gpu_kernel.cpp 里的 GLSL 计算着色器编成 SPIR-V。
#
# 为什么需要这一步：
#   Vulkan 不接受 GLSL 源码，只吃 SPIR-V 字模块。本脚本把「着色器唯一来源」
#   （engine/render/gpu_kernel.cpp 的 GLSL 字面量）抽取出来，用 DevEco 自带的
#   glslang_validator 编成 SPIR-V，并生成可直接嵌入的 C++ 头，避免运行时读文件。
#
# 工具链（DevEco Studio DS-243.24978.46.36.602650 自带，无需 Vulkan SDK）：
#   sdk/default/openharmony/toolchains/glslang_validator.exe
#   sdk/default/openharmony/toolchains/spirv-remap.exe
#
# 产物：
#   engine/render/spirv/gpu_kernel.comp        （抽取出的 GLSL，便于人工审阅/调试）
#   engine/render/spirv/gpu_kernel.comp.spv    （SPIR-V 字模块，可被工具链校验）
#   engine/render/spirv/gpu_kernel_spv.h       （SPIR-V 字节数组头，供后端嵌入）
#
# 用法：bash scripts/build_spirv.sh
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN="${OHOS_TOOLCHAIN:-G:/DevEco Studio/sdk/default/openharmony/toolchains}"
GLSLANG="$TOOLCHAIN/glslang_validator.exe"
OUT_DIR="$ROOT/engine/render/spirv"
GLSL_SRC="$OUT_DIR/gpu_kernel.comp"
SPV_OUT="$OUT_DIR/gpu_kernel.comp.spv"
HEADER_OUT="$OUT_DIR/gpu_kernel_spv.h"
PYTHON="${PYTHON:-<local-workspace>/tools/python/python.exe}"

if [ ! -f "$GLSLANG" ]; then
  echo "[spirv] 未找到 glslang_validator: $GLSLANG" >&2
  echo "[spirv] 可用 OHOS_TOOLCHAIN=<DevEco .../openharmony/toolchains> 指定" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"

# 1) 抽取 GLSL 字面量（着色器的唯一来源是 gpu_kernel.cpp）
"$PYTHON" - "$ROOT" <<'PY'
import io, os, sys
root = sys.argv[1]
source = os.path.join(root, "engine", "render", "gpu_kernel.cpp")
text = io.open(source, encoding="utf-8", newline="").read()
start = text.index('R"GLSL(') + len('R"GLSL(')
end = text.index(')GLSL"')
glsl = text[start:end]
out = os.path.join(root, "engine", "render", "spirv", "gpu_kernel.comp")
io.open(out, "w", encoding="utf-8", newline="\n").write(glsl)
print("[spirv] extracted %d lines -> engine/render/spirv/gpu_kernel.comp" % glsl.count("\n"))
PY

# 2) GLSL -> SPIR-V（Vulkan 1.0 目标环境）
"$GLSLANG" -V --target-env vulkan1.0 -S comp -o "$SPV_OUT" "$GLSL_SRC" || {
  echo "[spirv] glslang 编译失败" >&2
  exit 1
}

# 3) SPIR-V -> 可嵌入 C++ 头
"$PYTHON" - "$ROOT" <<'PY'
import io, os, struct, sys

root = sys.argv[1]
spv = os.path.join(root, "engine", "render", "spirv", "gpu_kernel.comp.spv")
data = io.open(spv, "rb").read()
words = struct.unpack("<%dI" % (len(data) // 4), data)

lines = []
lines.append("// AI生成（由 scripts/build_spirv.sh 生成，请勿手工修改）")
lines.append("/*")
lines.append(" * gpu_kernel_spv.h — SPIR-V 字模块（Vulkan 计算着色器），由 GLSL 经 glslang 编译而来。")
lines.append(" * 源：engine/render/gpu_kernel.cpp 的 GLSL 字面量；重新生成：bash scripts/build_spirv.sh")
lines.append(" */")
lines.append("#ifndef COMPOSITOR_RENDER_SPIRV_GPU_KERNEL_SPV_H")
lines.append("#define COMPOSITOR_RENDER_SPIRV_GPU_KERNEL_SPV_H")
lines.append("")
lines.append("#include <cstddef>")
lines.append("#include <cstdint>")
lines.append("")
lines.append("namespace compositor {")
lines.append("")
lines.append("inline constexpr uint32_t kGpuKernelSpirv[] = {")
for i in range(0, len(words), 6):
    chunk = ", ".join("0x%08xu" % w for w in words[i:i + 6])
    lines.append("    " + chunk + ",")
lines.append("};")
lines.append("")
lines.append("inline constexpr size_t kGpuKernelSpirvWordCount = sizeof(kGpuKernelSpirv) / sizeof(uint32_t);")
lines.append("inline constexpr size_t kGpuKernelSpirvByteCount = sizeof(kGpuKernelSpirv);")
lines.append("")
lines.append("}  // namespace compositor")
lines.append("")
lines.append("#endif /* COMPOSITOR_RENDER_SPIRV_GPU_KERNEL_SPV_H */")
lines.append("")

out = os.path.join(root, "engine", "render", "spirv", "gpu_kernel_spv.h")
io.open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
print("[spirv] header written: %d bytes, %d words" % (len(data), len(words)))
PY

echo "[spirv] done"
