# M5 编解码替换：能力登记与落地记录

本文件记录 M5「编解码替换」的**实测证据**、接缝设计与真实库落地结果。
一句话结论：**PNG 与 JPEG 已接入真实第三方库并真实可用（导入/导出打通），PSD 只读可用；
TIFF / HEIF / WebP / EXR 本机无第三方库可链接，故如实标记不可用并给出原因**——不写占位实现，
也不假装成功。

## 1. 结论一览

| 格式 | 状态 | 后端 | 依据 |
|------|------|------|------|
| PNG | **可用（vendored 真实库）** | libpng 1.6.44 + zlib 1.3.1 | `engine/third_party/`；导入/导出往返与独立解码器 0 LSB |
| JPEG | **可用（vendored 真实库，有损）** | stb_image + stb_image_write | 读写；`{"quality":N}` 控制质量，缺省 92 |
| PSD | **只读可用（vendored 真实库）** | stb_image | 仅导入，导出显式拒绝 |
| TIFF | 不可用 | 需 libtiff | 本机无头文件/库，未 vendoring |
| HEIF | 不可用 | 需 libheif | 同上 |
| WebP | 不可用 | 需 libwebp | 仅 DevEco 插件目录下非链接态副本 |
| EXR | 不可用 | 需 OpenEXR | 未找到 `ImfHeader.h` |

状态由**编译期实证**决定：`engine/format/codec_lib.{h,cpp}` 用 `__has_include` 探测
`engine/third_party/` 下的真实库；缺库的机器上同一份源码会如实回落——PNG 走自研 `comp_png`，
JPEG / PSD 标 `UNAVAILABLE`。登记表（`codec_registry`）是唯一真源，`comp_io_*` 的失败原因由它生成。

## 2. 本机探测证据与取源渠道（2026-10-14 实测）

- 编译器/构建工具：`cmake` / `make` / `gcc` / `g++` / `clang` / `pkg-config` / `vcpkg` **均为 None**。
  唯一编译路径是 `python -m ziglang`（PyPI wheel 内置 clang/lld），故真实库以**源码 vendoring**接入，
  而不是链接系统包。
- 头文件与库搜索根：`C:\Program Files`、`C:\Program Files (x86)`、`<local-workspace>`、
  `G:\DevEco Studio`、`<user-profile>`、`E:\Git`（深度 5 层）。未发现
  `png.h` / `pngconf.h` / `jpeglib.h` / `zlib.h` / `lcms2.h` / `ImfHeader.h` / `libheif`。
  唯一命中是 `G:\DevEco Studio\plugins` 下的 **webp 目录名**，非可链接库。
- 因此改用联网取**源码**并入库（可复现、可审计），发布渠道实测可用：
  - `fastly.jsdelivr.net/gh/nothings/stb/stb_image.h`（283,010 B）
  - `fastly.jsdelivr.net/gh/nothings/stb/stb_image_write.h`（71,221 B）
  - `download.sourceforge.net/libpng/libpng-1.6.44.tar.gz`（1,545,850 B）
  - `zlib.net/fossils/zlib-1.3.1.tar.gz`（1,512,791 B）
- Python 侧：PIL 12.3.0 具备 `jpg` / `zlib` / `webp` / `libtiff`，numpy 2.5.3 在装。
  这些**只作交叉验证参照**（见 `scripts/crosscheck_codec.py`），不链进 C++ 引擎。

## 3. 接缝设计（不改 ABI 形状）

- `engine/format/codec_registry.{h,cpp}`：能力登记表，**唯一真源**。每条含
  `format` / `name` / `state` / `backend` / `note` / `can_read` / `can_write`；
  状态由编译期探测动态决定，不写死。
- ABI 追加（尾部追加，属冻结面允许的纯函数追加）：
  `comp_io_codec_support(format, out_state, note_utf8, note_capacity, out_note_size)`，
  两段式读取原因文本；未登记格式返回 `COMP_ERR_NOT_FOUND` 且 `state = COMP_CODEC_UNAVAILABLE`。
  状态枚举新增 `COMP_CODEC_VENDORED = 2`（由 vendor 在 `engine/third_party` 的真实库提供）。
- 失败原因文案**由登记表生成**，不再写死会过期的「M2 scope」类文案。

## 4. 已落地的真实功能（非空壳）

| 入口 | 行为 |
|------|------|
| `comp_io_export(doc, path, format, options)` | `comp_render_flatten` → `codec_lib_encode`（PNG 走 libpng / JPEG 走 stb，quality 取自 options）→ 落盘 |
| `comp_io_import(engine, path, out_doc)` | 读文件 → `codec_lib_decode` → 建文档（画布 = 图像尺寸）→ 背景层写像素 |
| `comp_io_import_layer(doc, path, out_id)` | 同上，但**追加**一个图层，不覆盖画布 |
| `comp_io_codec_support(format, ...)` | 两段式回报真实可用性与原因（含库版本号） |

预乘口径与 `comp_png` 完全一致：引擎内部是预乘 RGBA8，文件内是直通（非预乘）颜色；
解码后预乘、编码前反预乘。因此 `codec_lib` 的输出可直接进合成，且与上游 macOS 版颜色一致。

失败语义（实测）：

- 不可用格式（TIFF / HEIF / WebP / EXR）：`COMP_ERR_UNSUPPORTED`，原因由登记表生成，
  形如 `host has no OpenEXR headers/libs (ImfHeader.h not found)`。
- 可用格式但文件不存在/损坏：`COMP_ERR_IO`（是 IO 问题，不是「不支持」）。
- PSD 导出：`COMP_ERR_UNSUPPORTED`，原因 `...; READ-ONLY: import supported, export unsupported`。

## 5. 验收证据

- 引擎回归：`summary: 83 cases, 0 failed, 0 check failures`（新增 `tests/test_codec.cpp` 与
  `tests/test_adjustment.cpp` 用例）。
- PNG 往返精度：8x6 全不透明随机像素，`diffs = 0`、`worst = 0 LSB`
  （不透明像素下预乘与直通数值相同，故可要求逐字节相等）。
- JPEG：`codec_jpeg_export_import_round_trip_is_lossy_but_consistent` 断言有损但稳定
  （导出后可回读、尺寸与不透明通道一致）。
- 能力表事实性：PNG = `COMP_CODEC_VENDORED`（或 `BUILTIN`，取决于 third_party 是否存在）；
  JPEG = `VENDORED` 且 note 含 `stb_image`；EXR 原因含 `OpenEXR`；
  `COMP_FORMAT_UNKNOWN` = `COMP_ERR_NOT_FOUND` 且状态不可用；截断读取仍以 `'\0'` 结尾。
- 独立解码器交叉验证：`scripts/crosscheck_codec.py` 用 PIL 12.3.0 回读引擎产物，
  `round_trip.png`(8x6, 157 B) / `layer_source.png`(4x4, 79 B) / `round_trip.jpg`(16x12, 1121 B)
  全部 `[OK]`、`failures = 0`。
- 外壳实机：`compositor_shell.exe --selftest=...` 实机 `rc = 0`，**27 项自检全 OK**。

## 6. 后续接入第三方库的步骤（保持 ABI 不变）

以本轮的 PNG / JPEG / PSD 落地为范例，接入其余格式时：

1. **取源与固定版本**：把源码置于 `engine/third_party/<lib>/`，记录版本号（发布渠道与文件大小见第 2 节）。
2. **放置与接线**：在 `scripts/build.py` 增加包含目录（`.c` 走 `-std=gnu11` 路径）。
3. **只改接缝**：在 `codec_lib.cpp` 增加对应通道（`__has_include` 探测），
   在 `codec_registry.cpp` 更新该条目的 `state` / `backend` / `note` / `can_read` / `can_write`，
   在 `comp_io_abi.cpp` 接上真实分派（多数情况无需改动，已在 `codec_lib` 层分派）。
4. **交叉验证**：与新库做双向比对（复用 PIL 参照法，方法同 `scripts/crosscheck_codec.py`），
   并把结果写成新的 `tests/test_codec.cpp` 用例。
5. **保持诚实**：任何中间态都必须返回 `COMP_ERR_UNSUPPORTED` 并写明原因。

## 7. 未覆盖与边界（诚实声明）

- **已接入**：PNG（libpng + zlib）、JPEG（stb，有损）、PSD（stb，只读）。
- **仍未接入**：TIFF / HEIF / WebP / EXR——不是「忘了做」，是本机确无可链接依赖，
  且本仓不允许引入不可复现的二进制。联网/换机后可按第 6 节接入。
- 第三方源码以源码形式 vendoring 到 `engine/third_party/`，随仓可复现；
  若目录被移除，`codec_lib` 的 `__has_include` 探测会失败，登记表**自动**回落为
  「PNG 走自研 comp_png、其余 UNAVAILABLE」，并如实写明原因。
- `.comp` 工程包**内部**图层像素仍走自研零依赖 `comp_png`（零依赖是工程包可移植性的前提），
  与外部库解耦；其与独立解码器（PIL）的双向 0 LSB 交叉验证见 README §6.1.2。
- PNG 外部编码走 libpng 真实 deflate 压缩；自研 `comp_png` 的编码为**存储块（无压缩）**，
  解码已覆盖 inflate 的存储块 / 固定 Huffman / 动态 Huffman 三类，位深 1/2/4/8/16，
  色彩类型 0/2/3/4/6；Adam7 隔行**显式拒绝**（不静默产出错误图像）。
- 色彩管理（ICC / lcms2）未接：PNG 按 sRGB 假设处理，不做 ICC 变换。
