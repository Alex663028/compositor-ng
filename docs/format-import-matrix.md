# s7 格式导入策略矩阵：HEIC / WebP / SVG / PSB

本文件回答一个具体问题：**遇到这四个格式时，导入通道到底做什么、为什么这么做、失败时
用户看到什么**。结论先行：

- **HEIC/HEIF 与 WebP 已登记但不可用**：本构建没有链接 libheif / libwebp，导入在**读文件
  之前**就以 `COMP_ERR_UNSUPPORTED` 拒绝，原因由编解码登记表生成（不是占位文案）。
- **SVG 与 PSB 未登记**：扩展名不映射到任何 `comp_image_format`，导入以
  `COMP_ERR_UNSUPPORTED` 拒绝，原因是「没有任何 codec 认领该格式」。
- 四者都**不退化成伪成功**：不写文件、不建空文档、不塞半解析图层、不把未知扩展名当
  PNG/PSD 硬试。

矩阵的可执行证据是 `tests/test_import_matrix.cpp`（4 个用例）。

## 1. 判定链路（唯一通路）

导入只走一条链路，不存在「按格式分叉的特殊通道」：

```
路径 → extension_of()：取最后一个扩展名、大小写归一（"photo.HEIC" → "heic"）
     → comp_io_format_from_path()：扩展名 → comp_image_format（未登记 = UNKNOWN）
     → can_decode()/can_encode()：本构建是否真的链了解码/编码实现
          （PNG 另有自研 comp_png 兜底，其余格式只看 vendored 真实库）
     → 不可用：COMP_ERR_UNSUPPORTED + unsupported_reason()（来自 codec_registry 登记表）
     → 可用：读文件（IO 失败 = COMP_ERR_IO）→ 解码（失败 = COMP_ERR_IO）
            → 画布尺寸闸（超限 = COMP_ERR_IO "image exceeds maximum canvas size"）
            → 建文档 / 在显式事务里加图层
```

关键顺序：**格式支持先于文件存在性**。所以「拖进一张 .heic，路径还不存在」得到的是
`UNSUPPORTED`（格式不支持），而不是 `IO`（文件没找到）——后者会把用户引向错误方向。

## 2. 策略矩阵

| 格式 | 扩展名映射 | 登记表状态（本构建） | 当前导入行为 | 错误码 | 原因来源 |
|------|-----------|----------------------|--------------|--------|----------|
| HEIC / HEIF | `.heic` `.heif` → `COMP_FORMAT_HEIF` | `COMP_CODEC_UNAVAILABLE`（backend: libheif；本机无头文件/库） | 显式拒绝，不读文件 | `COMP_ERR_UNSUPPORTED` | `HEIF is not available in this build (backend: libheif): host has no libheif; ...` |
| WebP | `.webp` → `COMP_FORMAT_WEBP` | `COMP_CODEC_UNAVAILABLE`（backend: libwebp；仅 DevEco 插件目录下非链接态副本） | 显式拒绝，不读文件 | `COMP_ERR_UNSUPPORTED` | `WebP is not available in this build (backend: libwebp): ...` |
| SVG | 无（→ `COMP_FORMAT_UNKNOWN`） | 未登记 | 显式拒绝，不读文件 | `COMP_ERR_UNSUPPORTED` | `unknown image format: no codec registered` |
| PSB | 无（`.psd` 才映射 `COMP_FORMAT_PSD`） | 未登记（无 PSB 枚举） | 显式拒绝，不读文件 | `COMP_ERR_UNSUPPORTED` | `unknown image format: no codec registered` |

对照（同链路里已可用的格式，说明矩阵不是「凡是新格式一律拒绝」）：

| 格式 | 状态 | 读写 |
|------|------|------|
| PNG | `COMP_CODEC_VENDORED`（libpng 1.6.44 + zlib 1.3.1）或 `BUILTIN`（自研 comp_png） | 读 + 写 |
| JPEG | `COMP_CODEC_VENDORED`（stb_image / stb_image_write，`{"quality":N}`，缺省 92） | 读 + 写（有损） |
| PSD | 拍平导入 `COMP_CODEC_VENDORED`（stb_image）；分层导入/导出 `COMP_CODEC_BUILTIN`（自研 `psd_codec`） | 读 + 写（分层常用子集，见 §3.5）；`comp_io_export` 单图导出仍 `UNSUPPORTED`，整篇走 `comp_io_export_document` |
| Radiance HDR | `COMP_CODEC_VENDORED`（stb_image 浮点） | **只读** |
| EXR | `COMP_CODEC_BUILTIN`（本仓 scanline 编解码器） | 读 + 写（NONE/RLE/ZIPS/ZIP） |
| TIFF | `COMP_CODEC_UNAVAILABLE`（需 libtiff） | 拒绝，原因同矩阵 |

## 3. 逐格式决策与理由

### 3.1 HEIC / HEIF — 已登记、不可用、不转码

- **为什么不「先解成 JPEG 再进来」**：本机没有 HEIF 解码器，所谓「转码」只能借 Python/PIL
  之类的宿主工具，产物不可复现、也不进引擎的可审计边界；跨机器结果会漂。宁可拒绝。
- **为什么两个扩展名同一枚举**：HEIC 是 HEIF 容器的一种编码组合，导入语义同为「一张位图」，
  拆成两个枚举只会让登记表与 ABI 重复。
- **落地路线**（未做）：vendoring `libheif`（需要 HEVC 解码器或系统编解码器）并在解码后做
  ICC/色彩空间归一；接上后只需把登记表该项状态改成 `VENDORED`，导入链路无需改动。

### 3.2 WebP — 已登记、不可用

- 本机只在 `G:\DevEco Studio\plugins` 下找到 **webp 目录名**，不是可链接的库；CMake/py
  `pkg-config`/vcpkg 均不存在，故与 HEIC 同样处理：登记为不可用、导入显式拒绝。
- 落地路线：vendoring `libwebp`（解码 + alpha；动图按「首帧」策略显式定义并写进文档），
  再把登记表状态改为 `VENDORED`。

### 3.3 SVG — 不登记，不进位图导入通道

- SVG 不是位图容器，导入它需要**矢量栅格化器**（如 nanosvg/resvg）并额外定义：目标 DPI、
  字体替换、CSS/滤镜支持子集、超出画布的处理。把它塞进 `comp_io_import` 会让「导入一张
  位图」的语义变成「按某个隐式 DPI 栅格化一份文档」——参数无处安放。
- 现状：不映射扩展名 → `UNKNOWN` → 统一拒绝。**不做的理由不是懒，而是语义不该混**。
- 若要做：独立入口（如「放置 SVG」）携带 DPI/尺寸参数，栅格化后走 `comp_io_import_layer`
  的像素通路；矩阵与登记表在那一轮再扩。

### 3.4 PSB — 与 PSD 明确区分

- `.psd` 的**拍平**导入走 `COMP_CODEC_PSD`（stb_image），分层导入/导出走自研
  `psd_codec`（§3.5）；`.psb` 是 Photoshop 的大文档变体（不同文件头/分块/长度字段），
  两条通路都**不支持**。
- 为什么不把 `.psb` 映射到 `COMP_FORMAT_PSD`：那会让解码器读到一个它不认识的容器，错误
  变成 `COMP_ERR_IO`（看起来像「文件坏了」），掩盖「格式不支持」的真实原因；更糟的是某些
  解析器会把前若干字节当 PSD 解析出**半张图**，违反「不输出半成品」。
- 落地路线：引入 PSB 读取器（libpsd / psd_sdk）或按 PSB 规范扩展自研 PSD 解析，然后新增
  `COMP_FORMAT_PSB` 枚举并登记；在那之前保持 `UNKNOWN`。

### 3.5 PSD 分层读写（M1，自研 `psd_codec`）

- **为什么自研而不引第三方**：第三方 PSD 库要么只读、要么把不支持的图层悄悄丢掉；
  本仓要的是「读得进来、写得回去、表示不了就具名拒绝」的闭环。`engine/format/psd_codec.cpp`
  零依赖，格式的每个字节都在本仓可审计。
- **读**：8/16 位 RGB(A)、RLE/raw、图层记录（名称/可见性/不透明度/偏移/剪贴标志）、
  图层蒙版（含与图层不同的矩形）、组（`lsct` 穿透/隔离）。
- **写**：同一子集原样写回；组走两条记录（边界分隔 + 文件夹），隔离组保留自己的混合
  模式，穿透组只写 `pass`。
- **明确拒绝**（`COMP_ERR_UNSUPPORTED` / `COMP_ERR_IO` + 具名原因，绝不静默降级）：
  PSB、CMYK/灰度/索引色彩、32 位通道、ZIP 压缩、文字层、智能对象、矢量蒙版、图层效果、
  调整层、缩放/旋转/翻转、非相邻剪贴来源。
- **入口分工**：`comp_io_import_document` / `comp_io_export_document` 是整篇语义（PSD
  走这里）；`comp_io_import` / `comp_io_export` 仍是单图语义，PSD 单图导出照旧
  `COMP_ERR_UNSUPPORTED`——同一格式的两个语义不混用同一个入口。
- **证据**：`tests/test_psd.cpp`（11 例，手写 fixture，正反例逐个钉字节）+ 与 psd-tools
  的独立交叉核对（见 README §6.87）。

## 4. 失败语义与不变量（回归点）
| 不变量 | 含义 | 证据 |
|--------|------|------|
| 错误码分层 | 格式不支持 = `UNSUPPORTED`；文件不存在/损坏/超限 = `IO`；参数空 = `INVALID_ARGUMENT` | `test_import_matrix.cpp`、`test_codec.cpp` |
| 失败不产出文档 | `comp_io_import` 失败时 `*out_document` 保持调用方初值（`nullptr`） | `import_matrix_unavailable_formats_reject_before_io`、`import_matrix_unknown_formats_are_not_guessed` |
| 失败不改目标文档 | `comp_io_import_layer` 失败：不新增图层、不写历史、不给出图层 id | `import_matrix_layer_import_failure_leaves_document_untouched` |
| 不猜格式 | 未登记扩展名一律 `UNKNOWN`，不按内容嗅探、不当 PNG/PSD 硬试 | `import_matrix_extension_mapping_is_explicit`、`import_matrix_unknown_formats_are_not_guessed` |
| 不写文件 | 导入路径全程只读；导出才落地 | `comp_io_abi.cpp` 导入段无写调用 |
| 像素口径 | 内部预乘 RGBA8 / RGBA16F；文件内直通；导入解码后预乘 | `docs/codec-migration.md` §4 |

## 5. 宿主侧表现

- 外壳/宿主可用 `comp_io_codec_support(format, out_state, note_utf8, capacity, out_size)`
  两段式查询：先拿状态，再按容量取原因文本；文案由登记表生成，不随代码注释过期。
- `comp_engine_last_error` 在导入失败后给出与上表一致的完整原因，宿主可直接展示。
- 未登记格式查询返回 `COMP_ERR_NOT_FOUND`，同时 `state = COMP_CODEC_UNAVAILABLE`——「查不到
  登记」本身就是「不可用」，不是一个成功的中性结果。

## 6. 本轮验证

- `tests/test_import_matrix.cpp`：4 个用例，覆盖扩展名映射、登记表拒绝、未知格式拒绝、
  失败不留痕。
- 全量回归：`python scripts/build.py` → 用例总数见根 `README.md` 的基线表；本轮新增用例后
  基线同步更新。
