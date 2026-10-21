# Filter 菜单（滤镜）参数契约

> 本文是 `comp_filter_apply` 的契约说明。ABI 声明见
> [`engine/abi/include/compositor/comp_filter.h`](../engine/abi/include/compositor/comp_filter.h)，
> 领域实现见 [`engine/domain/filters.h`](../engine/domain/filters.h) / `filters.cpp`。

## 1. 通路与分工

上游把 Filter 菜单收在 `FilterSettings` + `PixelFilter.run` 一条通路上
（`Filters.swift:63`、`:312-356`）。本仓的七个像素内核早已就位，缺的是**产品入口**：
参数契约、与图层几何的装配、一步撤销、与选区的混合。`comp_filter_apply` 就是那条入口。
Motion Blur 在上游走 Core Image 的 `CIMotionBlur`，本仓没有对应内核，由本层按
其可观察口径补一份盒式线积分（见 §3.8、§5）。

| 滤镜 | 内核 | 上游来源 |
|------|------|---------|
| Vignette | `adjust_colored_vignette`（`imaging/AdjustPixels.h`） | `Filters.swift` 的 `vignette*` |
| Tonal Contrast | `blur_rgba8_copy` + `adjust_tonal_contrast` | `tonal*` |
| Bloom / Glow | 本仓显式近似（见 §5） | `bloom*`（Core Image `CIBloom`） |
| Lens Correction | `lens_distort`（`imaging/LensPixels.h`） | `distortion`（`Filters.swift:238`） |
| Gaussian Blur | `blur_rgba8_copy`（与 blur 工具、Tonal 的 detail 同一实现） | `radius` / `preserveTransparency` |
| Add Noise | `noise_add_at`（`imaging/NoisePixels.h`） | `amount` / `gaussian` / `monochromatic` / `seed` |
| Grain | `adjust_grain`（与 Grain 调整层同一内核） | `grain` / `GrainSettings` |
| Motion Blur | 本文件 `motion_blur_pixels`（盒式线积分，见 §5） | `angle` / `distance`（Core Image `CIMotionBlur`） |

与工具层（[`docs/tool-ops.md`](tool-ops.md)）的分工：

- **工具层**按「笔迹 / coverage」工作，一次调用通常落一笔（Blur 工具属于这一层）；
- **滤镜层**按「整层一次」工作，参数是面板上的滤镜设置。

## 2. 通用契约

- **零初始化 = 恒等**：各子结构的 `amount` / `distortion` / `distance` 缺省为 0，此时
  不动像素、不开事务、`out_changed = 0`，note 写明 `settings are the identity`。
  ABI 不替宿主决定「面板缺省值」——宿主需要上游默认值时自己填（见 §3 各表）。
- **参数闸在开事务之前**：越界 / 非有限参数返回 `COMP_ERR_INVALID_ARGUMENT`，
  不留半成品、不进撤销栈。
- **越界拒绝，不静默 clamp**：上游 `FilterSettings.normalized` 会 clamp；
  本仓刻意更严格（与 Camera Raw 同规矩），避免宿主传错量程却静默生效半个效果。
- **一步撤销**：加入调用方已开启的显式事务，或自成一个隐式步
  （标签 `Filter: <名字>`）。
- **不改几何**：滤镜只写像素，不写 `transform` / `blend` / `opacity`。
- **note 两段式**：`note_capacity = 0` 时只回报所需长度（含 `'\0'`），
  终止符落在 `note[min(capacity-1, size)]`，**永不写出调用方缓冲**。

| 错误码 | 场景 |
|--------|------|
| `COMP_ERR_INVALID_ARGUMENT` | `desc` 为 NULL、参数越界 / 非有限、`document` 为 NULL |
| `COMP_ERR_NOT_FOUND` | `target` 图层不存在 |
| `COMP_ERR_STATE` | 组图层、无像素图层、HDR（`COMP_PIXEL_RGBA16F`）图层 |
| `COMP_ERR_OUT_OF_MEMORY` | 模糊 / 工作副本分配失败 |

## 3. 逐滤镜语义

数值量程与域层 `filter_settings_valid` 一致；**默认值为上游面板默认值**，
但 ABI 结构零初始化后是 0（恒等），宿主若要「打开面板即有可见效果」需显式填表。

### 3.1 Vignette（`COMP_FILTER_VIGNETTE`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `amount` | 0..100 | 0（恒等） | 暗角强度；0 = 不改 |
| `midpoint` | 0..100 | 50 | 羽化起点（50 = 画面中部开始压暗） |
| `roundness` | -100..100 | 100 | 负数 = 方角，100 = 正圆 |
| `feather` | 0..100 | 60 | 过渡带宽度 |
| `highlights` | 0..100 | 25 | 高光保护：越亮越不吃暗角 |
| `color[3]` | 0..1 | 0,0,0 | 直通 sRGB 边缘色 |

作用面 = **图层自身的像素矩形**（上游非空图层 `job.canvas == nil` 时 frame = extent），
不按画布铺底：图层在画布一角时暗角跟着图层走。`fillsClear = 0`，因此
**不改 alpha**——透明像素不会因为滤镜得到 coverage。

### 3.2 Tonal Contrast（`COMP_FILTER_TONAL_CONTRAST`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `amount` | 0..100 | 0（恒等） | 整体强度（内核 strength = amount / 50） |
| `radius` | 1..100 | 16 | 高斯 detail 半径（图层像素） |
| `shadows` | -100..100 | 40 | 暗部增益 |
| `midtones` | -100..100 | 60 | 中间调增益 |
| `highlights` | -100..100 | 30 | 高光增益 |

实现：先按 `radius * scale` 做高斯模糊得到 base，再逐像素比较局部细节
（`adjust_tonal_contrast`）。高斯复用共享的 `blur_rgba8_copy`——与 Gaussian Blur
调整层、扫描线辉光同一实现，不另起一套近似。

恒等判据（对齐上游 `Filters.swift:858`）：`amount == 0`，或三个 gain 全为 0。

### 3.3 Bloom / Glow（`COMP_FILTER_BLOOM`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `amount` | 0..100 | 0（恒等） | 强度；内核 intensity = amount / 50 |
| `radius` | 1..150 | 24 | 高斯半径（图层像素） |

**只加光、不改 alpha**：图层轮廓与合成几何不变；加光后的颜色不超过该像素自己的
alpha（预乘不变式），透明像素不会凭空发光。近似口径见 §5。

### 3.4 Lens Correction（`COMP_FILTER_LENS_CORRECTION`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `distortion` | -100..100 | 0（恒等） | Remove Distortion |

内核参数 `k = distortion / 100 * 0.35`（上游 `lensStrength`，`Filters.swift:238`）：
`k > 0` 采样向内（边角裁切），`k < 0` 采样图外（边角转透明），`k = 0` 恒等短路。

与 Blur 工具里的 Lens 是**同一实现**（`lens_distort`），只是参数空间不同：
工具收 `|k| <= 1`，滤镜菜单收面板量程 `-100..100` 再换算成 `k`。
回归护栏直接把「滤镜 distortion = -100」与「工具 k = -0.35」的逐字节结果对齐，
防止第二份畸变公式漂移。

### 3.5 Gaussian Blur（`COMP_FILTER_GAUSSIAN_BLUR`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `radius` | 0 = 恒等；否则 0.1..250 | 0 | 高斯半径（图层像素；预览再乘 `scale`） |
| `preserve_transparency` | 0 / 1 | 0 | 1 = 只模糊颜色，轮廓与原 alpha 不动 |

`(0, 0.1)` 是越界（拒绝），不静默夹到 0.1。实现复用共享的
`blur_rgba8_copy`（三遍盒式近似），`sigma = radius * scale`——与
Gaussian Blur 调整层（kind 7）同一口径；note 写明
`box approximation ... (sigma = radius * scale)`。

`preserve_transparency = 1`：用模糊后的 alpha 反预乘取得颜色，再乘回**原**
alpha。因此 alpha 通道逐字节不变，轮廓不向外扩散，透明邻域也不会把颜色拉暗。
关掉时整份预乘像素一起模糊，alpha 会向外扩散（上游默认行为）。

### 3.6 Add Noise（`COMP_FILTER_ADD_NOISE`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `amount` | 0 = 恒等；否则 0.1..400 | 0 | Photoshop 百分比 |
| `gaussian` | 0 / 1 | 0 | 1 = 高斯分布（更密），0 = 均匀分布 |
| `monochromatic` | 0 / 1 | 0 | 1 = 三通道加同一个值（只改亮度） |
| `seed` | `uint32` | 0 | 噪声场种子 |

噪声场按**文档坐标**锚定：origin = 图层 `effective_bounds()` 原点，与上游
`noiseOrigin` 一致。图层移动后图案不会在像素上「游动」；同一 seed + 同一文档
位置逐字节可复现。

### 3.7 Grain（`COMP_FILTER_GRAIN`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `amount` | 0..100 | 0（恒等） | 颗粒强度；0 = 不改 |
| `size` | 0.5..20 | 1.5 | 颗粒尺度（文档像素） |
| `roughness` | 0..100 | 50 | 小尺度不规则细节 |
| `seed` | `uint32` | 0 | 图案种子 |

与调整层的 Grain 走**同一个内核**（`adjust_grain`），差别只在入口：调整层作用于
合成后的 backdrop，滤镜作用于目标图层自身的像素。采样在图层像素空间
（origin 0，`unitsPerPixel = 1 / scale`），与上游
`grain.apply(job.image, unitsPerPixel: 1 / job.scale, seed: job.seed)` 一致。

### 3.8 Motion Blur（`COMP_FILTER_MOTION_BLUR`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `angle` | -90..90 | 0 | 方向角（度；水平为 0，逆时针为正，Photoshop 口径） |
| `distance` | 0 = 恒等；否则 1..2000 | 0 | 条纹长度（图层像素；预览再乘 `scale`） |

- **恒等判据只看 `distance`**：`distance = 0` 时不碰像素、不开事务，
  `angle` 取任何合法值都一样。
- `(0, 1)` 显式拒绝，不静默夹到 1（与 Gaussian Blur 的 `(0, 0.1)` 同规矩）。
- 采样在**预乘**像素上沿条纹做双线性平均，alpha 一起平均（条纹会向外扩散，
  与上游不保留透明的 motionBlur 一致）；越界坐标夹到图层边缘（等效上游
  `extending(...past: canvasSides)`）。
- 每像素最多 257 个采样点；更长的条纹按步长重采样（仍覆盖整条长度）。
- 角度只决定方向，条纹以像素为中心左右对称（上游 `CIMotionBlur` 亦无偏移参数）。

## 4. 选区与坐标

有选区（`SelectionState.active` 且非空）时，结果按 **文档分辨率**的 coverage
与原像素混合（上游 `PixelFilter.run` 末尾的 `PixelAdjust.blend`）：

```
out = filtered * cov + original * (255 - cov)   // 逐通道，含 alpha
```

图层像素的文档坐标 = `effective_bounds()` 原点 + 像素坐标（与合成 / 工具同款平移）。
落在画布外的图层像素 coverage 视为 0，保持原像素。

显式空选区（active 但蒙版全 0）⇒ 什么都不改，`out_changed = 0`，
note 写明 `document selection is empty`。

## 5. 与上游的已知差异（明示，不静默）

1. **Bloom 的亮部曲线是近似**。`CIBloom` 内部不公开。本仓显式定义：
   按 rec.709 亮度在 `[0.5, 1]` 线性起权提取亮部 → 预乘高斯模糊 →
   `rgb += glow * (amount / 50)` → 裁到自身 alpha。note 会写明
   `bright-pass approximation (threshold 0.5; CIBloom's curve is not public)`。
   只保证三条可观察口径：亮部向外扩散、强度按 `amount / 50`、半径按 `radius`。
2. **越界参数拒绝而不是 clamp**（见 §2）。
3. **Vignette 不铺底**：上游 `fillsCanvas` 会给空图层铺一张画布大小的透明底；
   本层要求图层已有像素（与工具层同一条拒绝）。
4. **Bloom 无 16 位通路**：上游对 Bloom 有 `runsAtSixteenBits`；本仓的 8 位内核
   做不到逐位对齐，因此 16 位图层会先落 8 位工作副本（note 写明），
   HDR 图层直接 `COMP_ERR_STATE`。
5. **Gaussian Blur 是盒式近似**。上游走 Core Image 的
   `applyingGaussianBlur(sigma:)`；本仓用共享的三遍盒式模糊
   （`blur_rgba8_copy`），`sigma = radius * scale`。保证「半径越大越糊、
   alpha 处理按 `preserveTransparency`」这两条可观察口径，不声称逐位一致。
6. **Motion Blur 是盒式条纹，不是 CI 的高斯近似**。上游把
   `distance / sqrt(12)` 作为 `CIMotionBlur` 的 radius（让 CI 的分布与长度为
   `distance` 的均匀条纹同方差）；本仓直接对长度 `distance` 做盒式平均。
   方向与长度两条可观察口径一致，形状不声称逐像素等同，note 写明
   `CIMotionBlur's radius is the equivalent standard deviation`。
7. **新四例同样没有 16 位通路**：Gaussian Blur / Add Noise / Grain / Motion Blur
   都跑在 8 位工作副本上（见 §6），HDR 拒绝。

## 6. 16 位与 HDR

- **RGBA16 图层**：先经 `deep_to_rgba8`（`domain/layer.h`，唯一生产者）落 8 位工作副本，
  滤镜跑在副本上，**此后图层就是 8 位**（对齐 `DeepLayers.swift:5`：任何不认识 16 位的
  操作都让图层不再是 16 位）。note 写明
  `on the 16-bit layer's 8-bit working copy (the layer is RGBA8 afterward)`。
  撤销会把 16 位资产原样恢复（历史快照覆盖格式与字节）。
- **RGBA16F（HDR）图层**：`COMP_ERR_STATE`，不做静默降位。
- **组 / 无像素图层**：`COMP_ERR_STATE`。

## 7. 护栏

`tests/test_filters.cpp`（18 例，ABI + 域层混合视角）钉住：

- 恒等设置是**真 no-op**（字节、事务、撤销栈都不动；含新增四个 kind 的零初始化）；
- 越界参数在事务之前拒绝（17 组，含 NaN scale；覆盖 blur radius、noise amount、
  grain size / roughness、motion blur angle / distance 的边界内外）；
- Vignette 压暗边角、中心不动、alpha 不变；一步撤销并可逐字节还原；
- Tonal Contrast 改变细节但保留黑白格结构；
- Bloom 向外扩散、远角不动、**alpha 全不变**、`rgb <= alpha`、note 含近似说明；
- Lens Correction 与 Lens 工具内核逐字节一致；
- 选区按**文档坐标**裁切（图层平移后仍只改选区内的部分）；
- 16 位工作副本与撤销恢复 16 位；
- 组 / 无像素 / HDR / 未知 id 的错误码；
- 显式空选区是 no-op；
- ABI 的 `out_changed` 与 note 两段式（capacity 0 只回报长度）。
- Gaussian Blur 真的软化边缘；`preserve_transparency` 下 alpha 逐字节不变、
  结果与默认路径不同；一步撤销；
- Add Noise 同 seed 逐字节复现、换 seed 不同、**按文档坐标锚定**（缩进图层与
  全幅图层在重叠处一致）、monochromatic 三通道相等；
- Grain 同 seed 复现、换 seed 不同、一步撤销；
- Motion Blur 在 `angle = 0` 时只沿 x 拉出条纹（远端行逐字节不动、不沿 y 移动），
  `angle = 90` 时转为竖直；`scale = 0.5` 把条纹长度减半；一步撤销；
- ABI 正确搬运四个新 kind 的字段（含布尔），越界值在 ABI 边界同样被拒。

另有 5 例在 `tests/test_tool_ops.cpp` / `tests/test_tool_ops_domain.cpp` 覆盖
Blur 工具（见 [`docs/tool-ops.md`](tool-ops.md) §4.9）。

## 8. Windows 外壳接线

「滤镜(&F)」菜单的条目由 `shells/windows/src/ui_core.h` 的 `filter_table()` 动态生成，
右侧面板的每一行也来自同一张表——**菜单、面板、值映射不各写一份清单**。

| 面板行 | ABI 字段 | 面板缺省 |
|--------|---------|---------|
| Vignette 0..4 | `vignette.amount/midpoint/roundness/feather/highlights` | 0 / 50 / 100 / 60 / 25 |
| Tonal 0..4 | `tonal_contrast.amount/radius/shadows/midtones/highlights` | 0 / 16 / 40 / 60 / 30 |
| Bloom 0..1 | `bloom.amount/radius` | 0 / 24 |
| Lens 0 | `lens_correction.distortion` | 0 |
| Gaussian Blur 0..1 | `gaussian_blur.radius/preserve_transparency` | 0 / 0 |
| Add Noise 0..3 | `add_noise.amount/gaussian/monochromatic/seed` | 0 / 0 / 0 / 0 |
| Grain 0..3 | `grain.amount/size/roughness/seed` | 0 / 1.5 / 50 / 0 |
| Motion Blur 0..1 | `motion_blur.angle/distance` | 0 / 0 |

- **面板夹取、ABI 拒绝**：`clamp_filter_param` 只作用于键入/步进，保证壳不会把越界
  值送进 ABI；ABI 的规矩仍是 §2 的「越界拒绝」。
- **一次调用**：壳用固定 512 字节 note 缓冲调用一次 `comp_filter_apply`，不做
  `note_capacity = 0` 的长度探测（该函数先执行后写 note，探测会应用两遍）。
- **恒等如实回报**：`out_changed == 0` 时状态栏写「参数为恒等，未改动」。
- **边缘色**：`vignette.color[3]` 固定 0,0,0（面板行模型只承载标量，颜色选择器
  属后续 UI 轮次，见 §3.1）。

面板的交互键位、几何常量与「哪些 Win32 路径未经人手实测」记在
[`docs/shell-parity.md`](shell-parity.md) §5.3。
