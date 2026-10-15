# AI生成
"""gen_vulkan_bindings.py — 从 GLSL 抽出 Vulkan 计算管线的绑定/推送常量布局。

为什么需要：宿主侧 vulkan_backend.cpp 的 descriptor set 与 push constant 必须与
gpu_kernel.cpp 里的 GLSL 严格一致。手写数字会漂移，因此把布局做成「从 GLSL 抽出来的
常量头」（render/spirv/vulkan_compute_bindings.h），让着色器成为唯一真源。

用法（先跑 scripts/build_spirv.sh 生成 .comp 与 .spv）：
    python scripts/gen_vulkan_bindings.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLSL = os.path.join(ROOT, "engine", "render", "spirv", "gpu_kernel.comp")
SPV = os.path.join(ROOT, "engine", "render", "spirv", "gpu_kernel.comp.spv")
OUT = os.path.join(ROOT, "engine", "render", "spirv", "vulkan_compute_bindings.h")

SIZE_OF = {"uint": 4, "int": 4, "float": 4}


def camel(name):
    name = re.sub(r"^g_", "", name)
    return "".join(part.capitalize() for part in name.split("_"))


def main():
    glsl = io.open(GLSL, encoding="utf-8").read()
    spv_bytes = os.path.getsize(SPV)

    local = re.search(
        r"layout\(\s*local_size_x\s*=\s*(\d+)\s*,\s*local_size_y\s*=\s*(\d+)"
        r"\s*,\s*local_size_z\s*=\s*(\d+)\s*\)",
        glsl,
    )
    if not local:
        raise SystemExit("[bindings] local_size 未找到")
    lx, ly, lz = (int(x) for x in local.groups())

    bindings = []
    pattern = r"layout\(([^)]*)\)\s*uniform\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*;"
    for match in re.finditer(pattern, glsl):
        attrs, typ, name = match.group(1), match.group(2), match.group(3)
        if "binding" not in attrs:
            continue
        set_m = re.search(r"set\s*=\s*(\d+)", attrs)
        bind_m = re.search(r"binding\s*=\s*(\d+)", attrs)
        if not (set_m and bind_m):
            raise SystemExit("[bindings] layout 缺少 set/binding: " + attrs)
        bindings.append((int(set_m.group(1)), int(bind_m.group(1)), typ, name))
    bindings.sort(key=lambda item: item[1])
    if len(bindings) != 2 or any(item[0] != 0 for item in bindings):
        raise SystemExit("[bindings] 绑定数与预期不符（期望 set0 下 2 个）: %r" % (bindings,))

    pc = re.search(r"layout\(push_constant\)\s*uniform\s+\w+\s*\{(.*?)\}\s*pc\s*;", glsl, re.S)
    if not pc:
        raise SystemExit("[bindings] push_constant 未找到")
    pc_size = 0
    for line in pc.group(1).split(";"):
        line = re.sub(r"//.*", "", line).strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2 or parts[0] not in SIZE_OF:
            raise SystemExit("[bindings] push_constant 字段无法识别: " + line)
        pc_size += SIZE_OF[parts[0]]

    lines = []
    lines.append("// AI生成（由 scripts/gen_vulkan_bindings.py 从 GLSL 抽取，请勿手工修改）")
    lines.append("/*")
    lines.append(" * vulkan_compute_bindings.h — Vulkan 计算管线的描述符 / 推送常量布局常量。")
    lines.append(" * 单一真源：engine/render/gpu_kernel.cpp 的 GLSL 字面量。")
    lines.append(" * 重建：bash scripts/build_spirv.sh && python scripts/gen_vulkan_bindings.py")
    lines.append(" */")
    lines.append("#ifndef COMPOSITOR_RENDER_SPIRV_VULKAN_COMPUTE_BINDINGS_H")
    lines.append("#define COMPOSITOR_RENDER_SPIRV_VULKAN_COMPUTE_BINDINGS_H")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace compositor {")
    lines.append("namespace vulkan_compute {")
    lines.append("")
    lines.append("inline constexpr uint32_t kDescriptorSet = %d;" % bindings[0][0])
    for _, binding, typ, name in bindings:
        lines.append(
            "inline constexpr uint32_t kBinding%s = %d; /* %s : %s */" % (camel(name), binding, name, typ)
        )
    lines.append("")
    lines.append("inline constexpr uint32_t kLocalSizeX = %d;" % lx)
    lines.append("inline constexpr uint32_t kLocalSizeY = %d;" % ly)
    lines.append("inline constexpr uint32_t kLocalSizeZ = %d;" % lz)
    lines.append("")
    lines.append("inline constexpr uint32_t kPushConstantSize = %d;" % pc_size)
    lines.append("inline constexpr uint32_t kPushConstantOffset = 0;")
    lines.append("")
    lines.append("inline constexpr uint32_t kSpirvByteCount = %d;" % spv_bytes)
    lines.append("")
    lines.append("}  // namespace vulkan_compute")
    lines.append("}  // namespace compositor")
    lines.append("")
    lines.append("#endif /* COMPOSITOR_RENDER_SPIRV_VULKAN_COMPUTE_BINDINGS_H */")
    lines.append("")

    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print(
        "[bindings] %s -> set=%d %s local=%dx%dx%d push=%dB spirv=%dB"
        % (
            os.path.basename(OUT),
            bindings[0][0],
            ", ".join("b%d=%s" % (b, n) for _, b, _, n in bindings),
            lx,
            ly,
            lz,
            pc_size,
            spv_bytes,
        )
    )


if __name__ == "__main__":
    sys.exit(main())
