# AI生成
#!/usr/bin/env python3
"""make_font_subset.py — 生成随仓字体资源（P5-4）。

本仓的文字工具需要一份**可入仓、可再生成**的字体：完整 Noto Sans SC 有 3 万多字形、
17 MB，塞进仓库既臃肿又无法逐字节复核。本脚本把可变字体固定到 Regular（wght=400）
并按固定字符集裁剪，产出 `resources/fonts/NotoSansSC-Regular-Subset.ttf`。

字符集（刻意固定，不用「GB2312 全表」之类的浮动定义）：
  - ASCII 0x20..0x7E（文字工具的旧路径就只认这些）
  - Latin-1 补充 U+00A0..U+00FF（重音拉丁字母）
  - 常用标点 U+2000..U+206F（引号、破折号、省略号）
  - CJK 标点 U+3000..U+303F 与全角形式 U+FF01..U+FF5E
  - **GB2312 一级汉字**：0xB0A1..0xD7F9 共 3755 字（现代汉语常用字）。
    GB2312 二级字（生僻字）不收——收全 6763 字会让资源翻倍，而缺字形时引擎
    会如实报告（见 domain/type_raster.h 的缺字形口径），不会画假字。

用法：
    python scripts/make_font_subset.py --source C:/Windows/Fonts/NotoSansSC-VF.ttf
    python scripts/make_font_subset.py --source <path> --output <path> [--weight 400]

依赖：fontTools（pip install fonttools）。**只用于重新生成资源**，构建与测试都不需要它；
测试直接读入仓的 .ttf，所以字体字节是确定的、跨机器可复现的。

许可：源字体 Noto Sans SC 是 SIL Open Font License 1.1（见 resources/fonts/OFL.txt）。
裁剪后的产物仍是 OFL：版权声明与许可证随资源入仓，字体名标注 Subset 以示改动。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT = os.path.join(ROOT, "resources", "fonts", "NotoSansSC-Regular-Subset.ttf")

# 固定字符集：改动这里就必须重新生成资源，并把新的 SHA256 记进 resources/fonts/README.md。
UNICODE_RANGES = [
    (0x0020, 0x007E),  # ASCII 可打印
    (0x00A0, 0x00FF),  # Latin-1 补充
    (0x2000, 0x206F),  # 常用标点
    (0x3000, 0x303F),  # CJK 标点
    (0xFF01, 0xFF5E),  # 全角 ASCII
]
# GB2312 一级汉字：区位 16..55 区，字节 0xB0A1..0xD7F9（3755 字）。
GB2312_LEVEL1_FIRST = 0xB0A1
GB2312_LEVEL1_LAST = 0xD7F9


def gb2312_level1() -> list[int]:
    """GB2312 一级汉字的 Unicode 码位（用内置 gb2312 编解码器推导，不硬编码字表）。"""
    codepoints: list[int] = []
    for high in range(GB2312_LEVEL1_FIRST >> 8, (GB2312_LEVEL1_LAST >> 8) + 1):
        for low in range(0xA1, 0xFE + 1):
            if high == GB2312_LEVEL1_LAST >> 8 and low > (GB2312_LEVEL1_LAST & 0xFF):
                break
            try:
                char = bytes([high, low]).decode("gb2312")
            except UnicodeDecodeError:
                continue  # 区位表里的空位
            codepoints.append(ord(char))
    return codepoints


def charset() -> set[int]:
    points: set[int] = set()
    for first, last in UNICODE_RANGES:
        points.update(range(first, last + 1))
    points.update(gb2312_level1())
    return points


def main() -> int:
    parser = argparse.ArgumentParser(description="Subset Noto Sans SC into the shippable test font")
    parser.add_argument("--source", required=True, help="源字体路径（Noto Sans SC 可变字体 .ttf）")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="输出路径")
    parser.add_argument("--weight", type=float, default=400.0, help="wght 轴取值（400 = Regular）")
    args = parser.parse_args()

    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
    except ImportError:
        print("[font] 需要 fontTools：pip install fonttools", file=sys.stderr)
        return 2

    if not os.path.isfile(args.source):
        print(f"[font] 源字体不存在：{args.source}", file=sys.stderr)
        return 2

    with open(args.source, "rb") as handle:
        source_sha = hashlib.sha256(handle.read()).hexdigest()

    font = TTFont(args.source, lazy=True, recalcTimestamp=False)

    # 1) 可变字体固定到 Regular。Noto Sans SC 的默认实例是 Thin，不固定会得到细体。
    if "fvar" in font:
        font = instancer.instantiateVariableFont(font, {"wght": args.weight}, inplace=False,
                                                 optimize=True)

    # 2) 按固定字符集裁剪。
    options = subset.Options()
    options.drop_tables += ["DSIG"]
    options.recalc_bounds = True
    options.recalc_timestamp = False
    options.name_IDs = ["*"]        # 保留版权 / 许可证名（OFL 要求可查看）
    options.name_languages = ["*"]
    options.layout_features = ["*"]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=charset())
    subsetter.subset(font)

    # 3) 名字标注 Subset：这是改动版，不该冒充完整字体（OFL 的诚实要求）。
    family = "Noto Sans SC Subset"
    for record in font["name"].names:
        if record.nameID == 1:
            record.string = family
        elif record.nameID == 2:
            record.string = "Regular"
        elif record.nameID == 4:
            record.string = family + " Regular"
        elif record.nameID == 6:
            record.string = "NotoSansSCSubset-Regular"
        elif record.nameID in (16, 17):
            record.string = family if record.nameID == 16 else "Regular"
        elif record.nameID == 3:
            record.string = "NotoSansSCSubset-Regular;compositor-ng;subset"

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    font.save(args.output)

    size = os.path.getsize(args.output)
    with open(args.output, "rb") as handle:
        out_sha = hashlib.sha256(handle.read()).hexdigest()

    glyphs = len(font.getGlyphOrder())
    print(f"[font] source={args.source}")
    print(f"[font] source_sha256={source_sha}")
    print(f"[font] output={args.output}")
    print(f"[font] glyphs={glyphs} size={size} sha256={out_sha}")
    print("[font] 把上面三行记进 resources/fonts/README.md（重新生成时同步更新）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
