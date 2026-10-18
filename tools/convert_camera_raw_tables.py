# AI生成
#!/usr/bin/env python3
"""把上游 Camera Raw 实测表从 CRT3（xz/LZMA）转成本仓的 CRT4（zlib）。

为什么需要转换：表是 Photoshop Camera Raw 实测出来的 763 张 17³ 色彩表，
上游把它按「逐通道、逐格、相对前一格的变化量」打包后用 xz 压缩成
`Compositor/Resources/CameraRawTables.bin`（1.25 MB）。本仓的引擎用树内
zlib（`engine/third_party/zlib`）解压，不引入 LZMA 解码器，因此离线转一次，
产物 `resources/camera_raw_tables.bin` 随仓库分发。

格式（与 CRT3 同构，只有魔数与压缩器不同）：

    偏移  长度  内容
    0     4     "CRT4"
    4     2     size   u16 小端，每轴取样数（17）
    6     2     count  u16 小端，表数（763）
    8     2     step   u16 小端，差值步长（3）
    10    ..    zlib 流，解压后 = count * size³ * step 字节

解压后的负载仍是上游的原样（逐通道差值），由 C++ 侧
`engine/domain/camera_raw_tables.cpp` 还原成每张表 17³ 个 sRGB 三元组。

用法：
    python tools/convert_camera_raw_tables.py
    python tools/convert_camera_raw_tables.py --source <CameraRawTables.bin> --output <out.bin>
"""

from __future__ import annotations

import argparse
import lzma
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = os.path.join(ROOT, "..", "compositor", "Compositor", "Resources",
                              "CameraRawTables.bin")
DEFAULT_OUTPUT = os.path.join(ROOT, "resources", "camera_raw_tables.bin")

SOURCE_MAGIC = b"CRT3"
OUTPUT_MAGIC = b"CRT4"
HEADER_SIZE = 10


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="上游 CRT3 文件")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="输出 CRT4 文件")
    args = parser.parse_args()

    with open(args.source, "rb") as handle:
        blob = handle.read()
    if len(blob) <= HEADER_SIZE or blob[:4] != SOURCE_MAGIC:
        print(f"error: {args.source} is not a CRT3 table file", file=sys.stderr)
        return 1
    size, count, step = struct.unpack("<HHH", blob[4:HEADER_SIZE])
    if size != 17 or step == 0:
        print(f"error: unexpected CRT3 header size={size} count={count} step={step}",
              file=sys.stderr)
        return 1

    try:
        planes = lzma.decompress(blob[HEADER_SIZE:], format=lzma.FORMAT_XZ)
    except lzma.LZMAError as error:
        print(f"error: CRT3 payload is not a valid xz stream: {error}", file=sys.stderr)
        return 1

    expected = count * size * size * size * step
    if len(planes) != expected:
        print(f"error: payload is {len(planes)} bytes, expected {expected}", file=sys.stderr)
        return 1

    payload = zlib.compress(planes, 9)
    out = OUTPUT_MAGIC + struct.pack("<HHH", size, count, step) + payload

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    temporary = args.output + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(out)
    os.replace(temporary, args.output)

    print(f"{args.source} -> {args.output}")
    print(f"  header: CRT4 size={size} count={count} step={step}")
    print(f"  planes: {len(planes)} bytes (xz) -> payload {len(payload)} bytes (zlib -9)")
    print(f"  file:   {len(out)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
