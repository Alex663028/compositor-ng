# AI生成
"""独立交叉验证：本引擎（libpng / stb_image_write）写出的文件必须能被 PIL 正确解读。

单向「我们写 -> PIL 读」即可证明编码产物是标准 PNG/JPEG，而不是自洽的私有格式；
反向「PIL 写 -> 我们读」已由引擎内 libpng 归一化路径（调色板/tRNS/灰度/16 位）覆盖，
如需真机端到端可用 tools/comp_crosscheck.cpp 扩展。

用法：python scripts/crosscheck_codec.py
"""
import os
import sys

try:
    from PIL import Image
except Exception as exc:  # pragma: no cover
    print("PIL unavailable:", exc)
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_CODEC = os.path.join(ROOT, "build", "codec")

cases = [
    ("round_trip.png", (8, 6), "PNG"),
    ("layer_source.png", (4, 4), "PNG"),
    ("round_trip.jpg", (16, 12), "JPEG"),
]

failures = 0
for name, expected_size, expected_format in cases:
    path = os.path.join(BUILD_CODEC, name)
    if not os.path.exists(path):
        print("[MISS] %s (run compositor_tests first)" % name)
        failures += 1
        continue
    with Image.open(path) as image:
        image.load()
        ok_size = image.size == expected_size
        ok_format = image.format == expected_format
        print("[%s] %-18s format=%-4s size=%s bytes=%d"
              % ("OK" if (ok_size and ok_format) else "FAIL", name, image.format, image.size,
                 os.path.getsize(path)))
        if not ok_size or not ok_format:
            failures += 1

print("[summary] failures = %d" % failures)
sys.exit(0 if failures == 0 else 1)
