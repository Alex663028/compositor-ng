# Camera Raw（F4）接口与语义

> 语义唯一真源是上游 `robbietilton/Compositor`：
> `Document/CameraRaw.swift`、`CameraRawColor.swift`、`CameraRawDetailOptics.swift`、
> `CameraRawGeometryCalibration.swift`、`CameraRawTables.swift` 与 `Curves.swift`。
> 本仓只做「C ABI + 设置解析 + 表装配 + 事务/撤销」，全部像素公式由
> `engine/imaging/AdjustPixels.c` 承担（与上游已对齐，见 README §6.1.15）。

代码位置：

| 文件 | 职责 |
|------|------|
| `engine/abi/include/compositor/comp_camera_raw.h` | 三个导出符号（唯一的 C 边界） |
| `engine/abi/src/comp_camera_raw_abi.cpp` | 句柄校验、错误码映射、note 两段式拷贝 |
| `engine/domain/camera_raw.{h,cpp}` | 设置结构、JSON、曲线、管线顺序、文档事务 |
| `engine/domain/camera_raw_tables.{h,cpp}` | CRT4 解码、取表、stage 装配、表合成、亮度测量 |
| `tools/convert_camera_raw_tables.py` | 上游 CRT3（xz）→ 本仓 CRT4（zlib）离线转换 |
| `resources/camera_raw_tables.bin` | 763 张 17³ 实测表，压缩后约 1.78 MB |

## 1. 表：为什么必须加载，什么时候加载

763 张 17³ 色彩表是 Photoshop 实测数据，**不是公式**。任何非恒等的 Camera Raw
设置都依赖它们；缺表时上游整条滤镜不可用，本仓返回 `COMP_ERR_STATE`，不会
悄悄降级成另一套观感。

表是**引擎级只读资源**：一个引擎加载一次，所有文档复用（`shared_ptr<const>`）。
加载失败时引擎原有的表保持不变——一次坏 blob 不会让已打开的文档失去 Camera Raw。

```c
comp_result comp_engine_load_camera_raw_tables(comp_engine engine, const void* data, size_t size);
comp_result comp_engine_camera_raw_tables_loaded(comp_engine engine, int32_t* out_loaded);
```

- `data` 只需在调用期间有效；解码结果由引擎持有。
- `data == NULL` → `COMP_ERR_INVALID_ARGUMENT`。
- 本构建没有 zlib → `COMP_ERR_UNSUPPORTED`；头部/压缩流/解压长度不符 → `COMP_ERR_DECODE`，
  `comp_engine_last_error` 写明原因。
- `comp_engine_camera_raw_tables_loaded` 只读，写 1/0。

推荐宿主在创建引擎后、首次应用 Camera Raw 前调用一次；路径由宿主决定
（仓库内是 `resources/camera_raw_tables.bin`）。

### CRT4 容器

```
偏移 0   4 字节  "CRT4"
偏移 4   u16 LE  size  (= 17)
偏移 6   u16 LE  count (= 763)
偏移 8   u16 LE  step  (差分步长，非 0)
偏移 10  ...     zlib 流
```

解压后必须是 `count × 17³ × 3` 字节（逐表、逐通道、逐格的**差分**，上游 CRT3 原样）。
解码时每个通道维护一个 `uint8_t` running 和：`value = level[axis] + int8(running) × step`，
再夹到 0..255。三个通道的轴分别是 `cell / 17²`、`(cell / 17) % 17`、`cell % 17`；
`level[axis] = round(axis × 255 / 16)`。

## 2. 应用：一次调用 = 一步撤销

```c
comp_result comp_camera_raw_apply(comp_document document, comp_layer_id target,
                                  const char* settings_json, double scale, uint32_t seed,
                                  int32_t* out_changed, char* note, size_t note_capacity,
                                  size_t* out_note_size);
```

- `settings_json`：UTF-8 JSON 对象；空串等价 `"{}"`（恒等设置）。
- `scale`：预览像素 / 图层像素，`<= 0` 视为 1（颗粒与半径按它缩放）。
- `seed`：颗粒定相。
- `out_changed`：成功时写 1；**恒等设置写 0 且不开事务、不动像素**。
- `note`：非致命说明（实际走了哪条通路），两段式：`note_capacity == 0` 时只回报
  `out_note_size = 长度 + 1`；写入时截断到 `capacity - 1` 并补 `'\0'`。
- 16 位图层先落 8 位工作副本（与工具层同一约定）；HDR / 其它格式 → `COMP_ERR_STATE`。

领域层自己 `txn_begin("Camera Raw")` / `txn_commit`：整次应用恰好一步撤销，
ABI 层不再包 `ImplicitTxn`（否则会叠出两条历史）。事务内任何异常都会先回滚再上抛，
ABI 的异常护栏把它转成错误码。

## 3. 设置 JSON

字段名与上游 Codable **逐字一致**（camelCase）。缺省字段取上游缺省值。

### 3.1 根对象

| 键 | 范围 | 缺省 |
|----|------|------|
| `whiteBalance` | `"Custom"` / `"Auto"` | `"Custom"` |
| `temperature`, `tint` | −100..100（相对偏移，不是开尔文） | 0 |
| `exposure` | −5..5（线性光档位） | 0 |
| `contrast`, `highlights`, `shadows`, `whites`, `blacks` | −100..100 | 0 |
| `vibrance`, `saturation` | −100..100 | 0 |
| `texture`, `clarity`, `dehaze` | −100..100 | 0 |
| `glow` | 0..100 | 0 |
| `glowStyle` | `"Diffusion"` / `"Bloom"` / `"Halation"` | `"Diffusion"` |
| `glowRange`, `glowSpread`, `glowWarmth` | −100..100 | 0 |
| `vignetteAmount` | −100..100 | 0 |
| `vignetteStyle` | `"Highlight Priority"` / `"Color Priority"` / `"Paint Overlay"` | `"Highlight Priority"` |
| `vignetteMidpoint` | 0..100 | 50 |
| `vignetteRoundness` | −100..100 | 0 |
| `vignetteFeather` | 0..100 | 50 |
| `vignetteHighlights` | 0..100 | 0 |
| `grainAmount` | 0..100 | 0 |
| `grainSize` | 0..100 | 25 |
| `grainRoughness` | 0..100 | 50 |
| `curve`, `mixer`, `grading`, `detail`, `optics`, `geometry`, `calibration` | 对象 | 见下 |

### 3.2 `curve`

| 键 | 范围 | 缺省 |
|----|------|------|
| `shadows`, `darks`, `lights`, `highlights` | −100..100 | 0 |
| `shadowSplit`, `darkSplit`, `lightSplit` | 0..100 | 25 / 50 / 75 |
| `refineSaturation` | −100..100 | 0 |
| `rgb`, `red`, `green`, `blue` | `[{"x":0..1,"y":0..1}, …]` | `[(0,0),(1,1)]` |

曲线点是 0..1 的**保形三次 Hermite**（上游 `Curves.value`）；少于 2 个点时
`camera_raw_curve_value` 返回 x。结构修复（排序、端点、间距）照上游执行。

### 3.3 `mixer`

| 键 | 说明 |
|----|------|
| `hue` / `saturation` / `luminance` | 各 8 个 −100..100，顺序为上游八个色族 |
| `points` | 最多 8 个点颜色；每个键见下 |

点颜色：`hue`（0..360）、`saturation` / `luminance`（0..1）、`hueShift` /
`saturationShift` / `luminanceShift`（−100..100）、`hueRange`（5..180）、
`saturationRange` / `luminanceRange`（0.05..1）、`visualize`（bool，只是预览标记）。

### 3.4 `grading`

四个轮 `shadows` / `midtones` / `highlights` / `global`，每个 `hue`（0..360）、
`saturation`（0..100）、`luminance`（−100..100）；外加 `blending`（0..100，缺省 50）
与 `balance`（−100..100）。

### 3.5 `detail`

`sharpenAmount`（0..150）、`sharpenRadius` / `sharpenDetail` / `sharpenMasking`（0..100）、
`noiseLuminance` / `noiseLuminanceDetail` / `noiseLuminanceContrast`、
`noiseColor` / `noiseColorDetail` / `noiseColorSmoothness`（0..100）。

### 3.6 `optics`

`removeChromaticAberration` / `enableLensProfile`（bool）、`profileDistortion` /
`profileVignetting`（0..100，缺省 100）、`distortion`（−100..100）、
`purpleAmount` / `greenAmount`（0..100）、`purpleHueLow` / `purpleHueHigh` /
`greenHueLow` / `greenHueHigh`（0..360）、`vignetteAmount`（−100..100）、
`vignetteMidpoint`（0..100）。色相端点写反时按上游对调。

### 3.7 `geometry`

`upright`（`"Off"` / `"Guided"`）、`projection`（`"Perspective"` / `"Rectilinear"`）、
`vertical` / `horizontal` / `aspect` / `scale` / `offsetX` / `offsetY`（−100..100）、
`rotate`（−45..45）、`constrainCrop`（bool）、`guides`
（`[{"startX":…,"startY":…,"endX":…,"endY":…}]`，归一化坐标，长度 ≤ 0.01 的被丢弃）。

**F4 不做 Core Image 透视变换**：`geometry.adjusts()` 为真时管线返回
`COMP_ERR_UNSUPPORTED`，但设置仍能解析与回读——宿主可以存盘、显示面板，
只是不能提交。导引线只有在 `upright == "Guided"` 且存在有效线时才算「非默认」。

### 3.8 `calibration`

`process`（`"Version 1"` … `"Version 6"`，缺省 `"Version 6"`）、`shadowTint`、
`redHue` / `redSaturation` / `greenHue` / `greenSaturation` / `blueHue` / `blueSaturation`。

### 3.9 严格性

| 输入 | 结果 |
|------|------|
| 未知键（根或任意嵌套对象） | 拒绝，`COMP_ERR_INVALID_ARGUMENT` |
| 类型不符 / 非有限数值 | 拒绝 |
| 标量越界 | **拒绝**（不静默 clamp） |
| 曲线点乱序 / 端点缺失 / 间距过小 | 结构修复（上游语义），不报错 |
| 点颜色超过 8 个 | 截断 |
| 去紫 / 去绿色相端点写反 | 对调 |
| 空串 | 等价 `"{}"` |
| 根不是对象 | 拒绝 |

上游 `normalized()` 对越界值一律 clamp；本仓把「调用方算错」与「上游自身的结构
修复」分开：前者拒绝，后者照旧。理由见 `engine/domain/camera_raw.h` 文件头。

## 4. 管线顺序（逐字对齐上游 `CameraRaw.apply`）

```
normalize
  → 恒等设置？ → 直接成功（out_changed = 0，不开事务）
  → 几何非默认？ → COMP_ERR_UNSUPPORTED
  → 表缺失？ → COMP_ERR_STATE
  → 亮度测量（仅 contrast / highlights / shadows 非零时需要，**在校准之前**）
  → 校准 shadowTint
  → 实测表 stages（校准主色 → 曝光 → 白平衡 → 对比度/高光/阴影 →
     白/黑/饱和/自然饱和 → 参数曲线）
  → foldingColor（当且仅当 foldsColor && colorAdjusts 时折进合成表）
  → adjust_camera_raw（表非空时）
  → applyCurveColor（逐像素曲线 + 点颜色，当 paintColor 时）
  → effects + 颗粒（当 paintEffects 时）
  → optics，然后 detail（当 paintDetailOptics 时）
```

通路判定：

- `foldsColor = mixer.points.isEmpty && curve.refineSaturation == 0`
- `colorAdjusts = curve.adjusts || mixer.adjusts || grading.adjusts`
- `paintColor = !foldsColor && colorAdjusts`
- `paintEffects = adjustsEffects`
- `paintDetailOptics = detail.adjusts || optics.adjusts`

颗粒：`grainKernelSize = 0.5 + grainSize/100 × 19.5`，
`unitsPerPixel = 1 / max(scale, 1)`；同一 `seed` 逐字节可复现。

光学：`distortion_k(0.35)`（上游 `PixelFilter.lensStrength`）；镜头暗角按位置
从 21 张曝光表取值。

note 会写明实际通路：恒等 / 单次查找（folded）/ 逐像素曲线（per-pixel）。

## 5. 错误码

| 条件 | 错误码 |
|------|--------|
| `document` / `settings_json` 为 NULL，JSON 非法/未知键/类型不符/越界 | `COMP_ERR_INVALID_ARGUMENT` |
| 表未加载 | `COMP_ERR_STATE` |
| 非默认几何（Upright / 投影 / 变换） | `COMP_ERR_UNSUPPORTED` |
| 图层不存在 | `COMP_ERR_NOT_FOUND` |
| 组图层 / HDR / 其它像素格式 | `COMP_ERR_STATE` |
| 16 位工作副本分配失败 | `COMP_ERR_OUT_OF_MEMORY` |
| 表 blob 损坏 | `COMP_ERR_DECODE` |
| 本构建没有 zlib | `COMP_ERR_UNSUPPORTED` |

所有失败路径**不修改像素、不开事务**；事务内失败会先回滚。

## 6. 明示边界

- **几何 / Upright 未移植**：需要 Core Image 类透视变换与重采样，属 S4 范围。
- **预览专用参数未暴露**：`clipping` / `sharpenMask` / `visualizePointColor` 会把
  预览效果烘进图层像素，因此不在 ABI 里；`visualize` 字段可解析但不生效。
- **无 RAW 解码**：本接口处理的是已解码的图层像素；相机原始格式的读取属编解码层。
- **无 GPU 通路**：CPU 内核；`camera_raw_lookup_is_valid` / `repair_lookup` 已随
  AdjustPixels 移植，但 F4 不走 Metal。
- **无 UI**：面板、曲线编辑器、颜色轮属外壳职责。

## 7. 测试

`tests/test_camera_raw.cpp`（15 例）覆盖：CRT4 加载/查询/坏 blob 与「失败不破坏
旧表」、恒等设置零副作用、曝光提亮 + 恰好一步撤销、JSON 严格性、非默认几何、
缺表、缺图层、folded / per-pixel 两条通路的 note、note 两段式拷贝、颗粒 seed
确定性、细节与光学改像素、16 位落 8 位、透明像素保持、曲线求值器边界。
`tests/test_camera_raw_align.cpp`（8 例）另钉住上游 1.5.3 的四个 AdjustPixels 函数。
