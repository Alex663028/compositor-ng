# Filter 菜单（滤镜）参数契约

> 本文是 `comp_filter_apply` 的契约说明。ABI 声明见
> [`engine/abi/include/compositor/comp_filter.h`](../engine/abi/include/compositor/comp_filter.h)，
> 领域实现见 [`engine/domain/filters.h`](../engine/domain/filters.h) / `filters.cpp`。

## 1. 通路与分工

上游把 Filter 菜单收在 `FilterSettings` + `PixelFilter.run` 一条通路上
（`Filters.swift:63`、`:312-356`）。本仓的七个像素内核早已就位，缺的是**产品入口**：
参数契约、与图层几何的装配、一步撤销、与选区的混合。`comp_filter_apply` 就是那条入口。
Motion Blur 在上游走 Core Image 的 `CIMotionBlur`、Progressive Blur 走
`CIMaskedVariableBlur`，本仓都没有对应内核，由本层按可观察口径各补一份
（盒式线积分 / 分档三角混合，见 §3.8、§3.9、§5）。

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
| Progressive Blur | 本文件 `progressive_blur_pixels`（8 档 `blur_rgba8_copy` 三角混合，见 §3.9） | `progressive*`（Core Image `CIMaskedVariableBlur`） |
| Sharpen | `blur_rgba8_copy` + 本文件 `sharpen_pixels`（经典 USM，见 §3.10） | Filter > Sharpen > Unsharp Mask |
| Reduce Noise | 本文件 `reduce_noise_pixels`（YCoCg 双边 + 色度平滑，见 §3.11） | Filter > Noise > Reduce Noise |
| High Pass | `blur_rgba8_copy` + 本文件 `high_pass_pixels`（见 §3.12） | Filter > Other > High Pass |
| Minimum | 本文件 `morphology_pixels`（方形窗口腐蚀，单调队列，见 §3.13） | Filter > Other > Minimum |
| Maximum | 本文件 `morphology_pixels`（方形窗口膨胀，单调队列，见 §3.14） | Filter > Other > Maximum |
| Offset | 本文件 `offset_pixels`（整像素平移，三种露出处理，见 §3.15） | Filter > Other > Offset |

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

### 3.9 Progressive Blur（`COMP_FILTER_PROGRESSIVE_BLUR`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `from` | 0..250 | 0 | 渐变轴起点的模糊半径（图层像素；预览再乘 `scale`） |
| `to` | 0..250 | 20 | 渐变轴终点的模糊半径（同上） |
| `radial` | 0 / 1 | 0 | 1 = 以 `start` 为心、`\|end - start\|` 为半径的径向渐变 |
| `inverted` | 0 / 1 | 0 | 1 = 交换 `from` / `to`（等价于把轴掉头） |
| `start_x` / `start_y` | 文档坐标（连续） | 0 | 轴起点；`explicit_axis = 0` 时忽略 |
| `end_x` / `end_y` | 文档坐标（连续） | 0 | 轴终点；同上 |
| `explicit_axis` | 0 / 1 | 0 | 0 = 用缺省轴（图层像素矩形中线、自顶向下）；1 = 用 `start` / `end` |

- **恒等判据**：`max(from, to) <= 0`（上游 `guard strongest > 0`）。零初始化
  （`from = to = 0`、`explicit_axis = 0`）= 恒等 + 缺省轴，不碰像素、不开事务。
  文档层用反相的 `auto_axis`（缺省 `true`），两侧零初始化含义一致。
- 实现：把 `[from, to]` 等分成 8 档，逐档跑共享内核 `blur_rgba8_copy`，
  每个像素按它在轴上的位置 `t ∈ [0, 1]` 在相邻两档之间取三角权重（权重和为 1）。
  `from == to` 时各档半径相同，结果与同 sigma 的 Gaussian Blur **逐字节相同**。
  第 0 档是**起点的半径**而不是原图：`from > 0` 时它同样要模糊，否则均匀档
  会在轴起点留一条不模糊的边；`from = 0` 才用原图（0 半径用 0.1 px 近似会
  平白改掉一像素）。
- 轴向：线性档 `t = dot(p - start, d) / |d|²`，径向档 `t = |p - start| / |d|`，
  都夹到 `0..1`（径向超过轴长的部分按终点半径处理）。轴退化（`|d| < 1 px`）时
  终点取 `start + (0, +1)`：本仓像素 y 向下，与上游在 CI 里取 `start + (0, -1)`
  是同一方向。
- 显式轴端点经 `Layer::document_to_pixel_continuous` 投到图层像素空间：
  同一套仿射（归一化 → 逆旋转 → 翻转），**保留亚像素**、允许落在缓冲外。
  取整版 `document_to_pixel` 在缩放 / 旋转下会让同一对端点抖动 ±1 px，
  渐变方向随之跳变，所以这里不用它。
- 缺省轴（`explicit_axis = 0`）取**图层自身像素矩形**的中线：
  `x = (w - 1) / 2`，`y` 从 `0` 到 `h - 1`。上游 `progressiveStart/End` 为 nil
  时取画布中线，但域层没有画布上下文，不替宿主猜；note 写明
  `axis defaulted to the layer's mid-line`。
- `scale` 只乘半径，不改正则方向：降采样预览看起来与全尺寸一致。

### 3.10 Sharpen / Unsharp Mask（`COMP_FILTER_SHARPEN`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `amount` | 0 = 恒等；否则 0.1..500 | 0 | 锐化量（百分比） |
| `radius` | 0.1..250 | 1 | detail 半径（图层像素；预览再乘 `scale`） |
| `threshold` | 0..255 | 0 | 通道差低于它不动（避免放大噪点） |

- 口径 = 经典 USM：`src + amount / 100 * (src - blur(src))`。`blur` 复用共享的
  `blur_rgba8_copy`（与 Gaussian Blur / Tonal Contrast 同一实现），
  `sigma = max(0.1, radius * scale)`，不另起一套高斯。
- 运算落在**预乘**像素上、逐通道夹到 `[0, alpha]`，alpha 逐字节不变：
  图层轮廓不向外扩散，半透明边缘的颜色不会因为锐化越过自身 coverage。
- `threshold` 按通道判据：`|src - blur| <= threshold` 的通道保持原值，
  因此低对比细节（例如 120/128 的棋盘格）在阈值足够大时逐字节不动。
- 恒等判据只看 `amount`：`amount = 0` 时不碰像素、不开事务；
  `(0, 0.1)` 与 `radius` 越界显式拒绝，不静默夹到端点。
- `out_changed` 是**参数**口径而非像素口径：`amount > 0` 就是一步事务，
  即使阈值把每个通道都放过了（像素逐字节不变），这一步仍如实计入撤销栈。

### 3.11 Reduce Noise（`COMP_FILTER_REDUCE_NOISE`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `strength` | 0 = 恒等；否则 0.1..10 | 0 | 整体强度 |
| `preserve_details` | 0..100 | 50 | 越高越保留细节（双边值域 sigma 越小） |
| `reduce_color_noise` | 0..100 | 25 | 只平滑色度：亮度按双边结果、色度按窗口均值混合 |

- 实现：5×5 双边滤波，工作在**直通（unpremultiply 后）**颜色上，先转 YCoCg
  （`Y = (r + 2g + b) / 4`、`Co = r - b`、`Cg = g - (r + b) / 2`），窗口权重取
  Y 轴之差（亮度边沿保留），色度两轴按窗口均值平滑，最后按原 alpha 重新预乘。
- `range_sigma = 2 + strength / 10 * (1 - preserve_details / 100) * 60`：
  `strength` 越大、`preserve_details` 越低，窗口越愿意跨过亮度差取平均。
- alpha 逐字节不变（噪声不向透明区扩散）。
- 上游算法内部不公开（细节层 + 多尺度一类），本仓只保证三条可观察口径：
  噪声方差下降、边沿对比保留、色噪按键平滑（见 §5 第 10 条）。
- 上游对话框第四行 `sharpen_details` 不在本结构里：锐化已有独立滤镜
  （§3.10），不把两条锐化通路塞进同一个算子。

### 3.12 High Pass（`COMP_FILTER_HIGH_PASS`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `radius` | 0 = 恒等；否则 0.1..250 | 0 | 模糊半径（图层像素；预览再乘 `scale`） |

- 口径 = `128 + (src - blur(src))`：模糊复用共享的 `blur_rgba8_copy`
  （与 Gaussian Blur / Sharpen 同一实现），`sigma = max(0.1, radius * scale)`，
  不另起一套高斯。
- **平坦区域落 128 中灰**（无细节即无偏移），细节以中灰为轴上下偏移；
  这正是「常被拿去叠加 / 柔光混合回原图」的那种底图。
- `src` 与 `blur(src)` 都按**直通**色（`color / alpha`，夹到 255）比较，
  结果再乘回**原** alpha：半透明边缘按自身 coverage 归一，不会被透明邻域拉暗。
- alpha 逐字节不变；`alpha == 0` 的像素逐字节不动。模糊后 alpha 为 0 的邻域
  没有颜色可比，该项按 128 写（差值 0）。
- 逐通道夹到 `0..255` 后预乘回写并夹到 `rgb <= alpha`（预乘不变式）。
- `(0, 0.1)` 与 `radius` 越界显式拒绝，不静默夹到端点；恒等判据只看 `radius`。

### 3.13 Minimum（`COMP_FILTER_MINIMUM`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `radius` | 0 = 恒等；否则 1..100 | 0 | 方形窗口半径（整数像素；预览再乘 `scale` 后取整，至少 1） |

### 3.14 Maximum（`COMP_FILTER_MAXIMUM`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `radius` | 0 = 恒等；否则 1..100 | 0 | 方形窗口半径（整数像素；预览再乘 `scale` 后取整，至少 1） |

Minimum / Maximum 共用一份实现（`morphology_pixels(..., dilate)`），只差取极值方向：

- 窗口 = 边长 `2r + 1` 的正方形，**四个通道一起**在窗口内取最小 / 最大值；
  因此 alpha 也参与：透明底上的形状会随 Minimum 收缩（腐蚀）、Maximum 扩张
  （膨胀），与上游在透明图层上的可观察行为一致。
- 两趟一维滑动窗口（横向写临时缓冲、纵向写回图层），每趟都用单调队列，
  整体 O(w·h)，**与半径无关**——半径 100 与半径 1 同价。
- 作用在**预乘**像素上是安全的：颜色 ≤ alpha ⇒ 窗口内的颜色极值 ≤ alpha 极值，
  结果仍是合法预乘像素，不会出现「颜色越界」。
- 窗口在图层边界处**裁剪**（不复制边缘像素），与共享盒式模糊同一口径：
  贴边的窗口只看落在图层内的样本。
- 半径先 `llround(radius * scale)` 且至少 1，再按轴夹到 `维度 - 1`：
  窗口大于图即等价于覆盖全图，不会越界读。
- note 分别写明 `square-window erosion` / `dilation over all four channels`。

### 3.15 Offset（`COMP_FILTER_OFFSET`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `horizontal` | -10000..10000 | 0 | 水平位移（图层像素，四舍五入到整像素） |
| `vertical` | -10000..10000 | 0 | 垂直位移（同上） |
| `wrap` | 0 / 1 / 2 | 0 | 露出区域：0 透明、1 边缘延展、2 环绕 |

- **整像素平移**：目标像素读源 `(x - dx, y - dy)`，`dx = llround(horizontal * scale)`，
  不做重采样，因此不引入插值模糊。先整份拷出源像素再写回，重叠区域不互相污染。
- 恒等：两轴取整后都为 0（`horizontal = 0.4` 在 `scale = 1` 下也是恒等）。
- 三种露出处理（数值与 ABI `comp_offset_wrap` 一致，其余值越界拒绝）：

  | `wrap` | 露出区域 | 后果 |
  |---|---|---|
  | 0 `COMP_OFFSET_TRANSPARENT` | 写 `(0, 0, 0, 0)` | 移出图层的像素丢失；反向量位移可逐字节还原 |
  | 1 `COMP_OFFSET_EDGE` | 越界坐标**逐轴**夹到图层边界 | 角上会取到角落像素，因此露出的角带被角落像素填满；移出图层的内容仍然丢失，反向量**不**能还原 |
  | 2 `COMP_OFFSET_WRAP` | 按图层宽高取模（负值补到正区间） | 移出的内容从对侧卷回；反向量位移逐字节还原 |

- 环绕范围是**图层像素矩形**，不是画布（见 §5 第 13 条）。

### 3.16 Custom（`COMP_FILTER_CUSTOM`）

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `kernel[25]` | -999..999 | 中心 `kernel[12] = 1`，其余 0 | 5×5 卷积核，行优先（`kernel[row * 5 + column]`，第 0 行在最上面） |
| `scale` | -999..999 | 1 | 除数；**0 按 1 处理**（面板把除数清空时不除零） |
| `offset` | -999..999 | 0 | 卷积后逐通道相加的常量 |

```
out(p) = clamp(sum(kernel[i] * straight(neighbour_i)) / divisor + offset, 0, 255)
divisor = (scale == 0 ? 1 : scale)
```

- 卷积在**直通色**上做（`rgb / alpha`，夹到 255），结果再乘回**原** alpha：
  alpha 逐字节不变，图层轮廓不会被卷积核自己撑开或收掉。
- `alpha == 0` 的像素原样跳过；邻域里 alpha 为 0 的位置没有直通色，**按 0 参与累加**。
  除数是面板给的 `scale`，不按覆盖率归一，所以紧贴透明区的轮廓像素会因为少了
  那几格权重而偏暗（上面的 8×8 方块测试里，角落像素 = `9 * 255 / 25 ≈ 92`）。
  这是 5×5 直通卷积的直接结果，**不是 bug**；需要「只改颜色不咬边」时用
  High Pass / Sharpen 那类乘回 alpha 的算子，而不是自定义核。
- 邻居坐标**夹到图层内**（边缘延展），窗外不当黑：全 1 核 / divisor 25 作用在
  均匀图层上逐字节不变——这条测试同时钉住了边界口径。
- 恒等判据：核恰为恒等核、`offset == 0`、且 `scale ∈ {0, 1}`（0 与 1 同为除数 1）。
  零初始化的 `FilterSettings{}` 因此仍是恒等；ABI 侧把**完全未触碰**的
  `comp_filter_custom`（核全 0 + scale 0 + offset 0）映射成恒等核，让
  「零初始化 = 恒等」在 C 边界也成立。想显式要全 0 输出（黑）就把 `scale` 置 1
  或 `offset` 置非 0。
- note 写明 `as a 5x5 convolution on straight colour with edge-clamped neighbours
  (alpha untouched; previews do not rescale the kernel)`。
- 预览（`scale < 1`）**不缩小卷积核**：核没有连续的「半径」语义，降采样预览
  会比全尺寸偏强（见 §5 第 14 条）。

### 3.17 去噪族：Median / Dust & Scratches（`COMP_FILTER_MEDIAN` / `COMP_FILTER_DUST_SCRATCHES`）

A45（2026-10-27）尾部追加的两项，共用一份设置（ABI 里叫 `comp_filter_denoise`）。

| 字段 | 量程 | 上游默认 | 含义 |
|------|------|---------|------|
| `radius` | 0 = 恒等；否则 1..50 | 0 | 方形窗口半径（整数像素） |
| `threshold` | `int32`，0..255（仅 Dust & Scratches） | 0 | 0 = 等同 Median；1..255 = 中心与中位数的差超过它才替换 |

- Median（`COMP_FILTER_MEDIAN = 16`）：窗口取**中位色**（直通色逐通道中位数），
  alpha 逐字节不变；孤立噪点（亮点 / 暗点）被窗口多数色替换掉。
- Dust & Scratches（`COMP_FILTER_DUST_SCRATCHES = 17`）：中心像素与窗口中位数的
  逐通道差**超过 `threshold`** 才替换为中位数，否则原样保留——只压孤立划痕，
  不动阈值内的温和细节（一条测试同时验两侧：阈值内保留、超阈值替换）。
- 两者共用同一窗口实现；`threshold == 0` 时 Dust 退化为 Median。
- 恒等判据：`radius == 0`。零初始化 `comp_filter_denoise` 因此是恒等（不开事务）。

### 3.18 扭曲 / 像素化 / 风格化：Spherize / Pinch / Twirl / Mosaic / Emboss（A46）

A46（2026-10-28）尾部追加的五项（`COMP_FILTER_SPHERIZE = 18` …
`COMP_FILTER_EMBOSS = 22`）。三项扭曲（Spherize / Pinch / Twirl）共用一套
**以图层中心为原点、半短边归一**的径向重映射 + 双线性采样；采样落在图像外按
**透明**参与（与 §3.4 Lens Correction 的窗外口径一致），输出为预乘 RGBA8
（`out_color <= out_alpha`）。

| 滤镜 | kind | 参数 | 量程 / 恒等 | 映射口径 |
|------|------|------|-------------|----------|
| Spherize | 18 | `amount` | -100..100，0 = 恒等 | `r_src = r * (1 - s * (1 - r²))`，`s = amount / 100`；正 = 球面（中心放大），负 = 凹面 |
| Pinch | 19 | `amount` | -100..100，0 = 恒等 | `r_src = r * (1 + s * (1 - r²))`；正 = 向内挤压，负 = 向外扩张 |
| Twirl | 20 | `angle` | -999..999（度），0 = 恒等 | `theta_src = theta_dst - angle * max(0, 1 - r)`；`r >= 1`（边缘）不转 |
| Mosaic | 21 | `cell_size` | 0 = 恒等；否则 2..200 | 方格边长；格内取**预乘分量**算术均值（alpha 也一起平均） |
| Emboss | 22 | `amount` / `angle` / `height` | 0 = 恒等；否则 1..500 / -360..360（度）/ 0..10 | `128 + amount / 100 * (A - B)` |

- 扭曲三者的 `r` 是**归一半径**（图层中心为 0，半短边为 1）；采样用双线性，
  重复映射是连续的，不做最近邻的硬边。贴边像素会因为采样落到窗外而变半透明——
  这是「窗外按透明参与」的直接结果，**不是 bug**。
- Mosaic 的 `cell_size` 在预览时乘 `scale` 后取整、下限 2；(0, 2) 之间没有可用
  方格，显式拒绝而不是夹到 2。恒等时 `cell_size == 0`，不开事务。
- Emboss 的 `A` / `B` 是沿 `angle` 方向 ±`height`（预览乘 `scale` 后至少 1 像素）
  处的**直通色**；`angle` 逆时针为正（图层 y 向下，方向向量 `(cos, -sin)`）。结果
  逐通道夹 0..255 后乘回**原** alpha——平坦区域落 128 底灰，alpha 不变。
- 越界参数（如 `amount = 101`、`cell_size = 1`、`angle = 1000`）一律
  `COMP_ERR_INVALID_ARGUMENT`，参数闸先于事务；非零参数恰好一步撤销。
- 与 A45 一样，A46 只追加 kind 与子结构，ABI 导出数不变（见 §5 尾部追加清单）。

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
7. **其余算子同样没有 16 位通路**：Gaussian Blur / Add Noise / Grain / Motion Blur /
   Progressive Blur / Sharpen / Reduce Noise / High Pass / Minimum / Maximum / Offset /
   Custom 都跑在 8 位工作副本上（见 §6），HDR 拒绝。
8. **Progressive Blur 是 8 档混合，不是逐像素半径**。上游 `CIMaskedVariableBlur`
   逐像素连续变化半径；本仓把 `[from, to]` 切成 8 档、逐档均匀模糊后按三角权重
   混合（见 §3.9）。可观察口径一致：两端半径、渐变方向、`radial` / `inverted`
   语义；过渡带内不声称逐像素等同，note 写明
   `8-level blend ... the ramp is not pixel-identical`。
9. **Sharpen 的 detail 模糊是盒式近似**。上游 USM 在 Core Image 的高斯上做，
   本仓复用共享的三遍盒式模糊（`blur_rgba8_copy`），`sigma = max(0.1, radius * scale)`。
   保证「amount 越大越锐、radius 越大 halo 越宽、threshold 以下不动、alpha 不变」
   四条可观察口径，不声称逐像素等同；note 写明
   `as an unsharp mask over the shared box-blur approximation`。
10. **Reduce Noise 是多尺度算法的单尺度近似**。上游内部不公开；本仓用 5×5 双边
    + YCoCg 色度平滑，只保证「噪声方差下降、边沿对比保留、色噪按键平滑」
   三条可观察口径，note 写明 `a 5x5 bilateral on luma with chroma smoothing`。
11. **High Pass 的模糊是盒式近似**。上游在 Core Image 的高斯上做差值；本仓复用
   共享的三遍盒式模糊，`sigma = max(0.1, radius * scale)`。保证「平坦区落 128、
   边沿两侧上下分离、alpha 不变」三条可观察口径，halo 形状不声称逐像素等同；
   note 写明 `128 + (src - blurred) over the shared box-blur approximation`。
12. **Minimum / Maximum 的窗口在边界裁剪，不做边缘延展**。上游的边界处理不公开；
   本仓与共享盒式模糊取同一口径——贴边窗口只看落在图层内的样本。另外上游对话框
   有「方形 / 圆角」一类形状选项，本层只做方形窗口（半径 1..100），不承载形状参数。
13. **Offset 在图层像素矩形内卷绕，不是画布**。上游以画布为参照、只处理与画布重叠的
   部分；域层没有画布上下文，因此 `wrap = 2` 按图层宽高取模、`wrap = 1` 逐轴夹到
   图层边界（角带由角落像素填满）。note 分开写明三种露出处理，宿主可据 note 判断
   与上游的差异是否影响自己的合成。
14. **Custom 的两条边界**：
    a. **预览不缩放卷积核**。其余滤镜的半径乘 `scale`，但核没有连续的「半径」语义，
       降采样预览里同一个核相对像素更「大」，观感比全尺寸偏强；全尺寸结果不受影响。
    b. **透明邻域会让轮廓变暗**。直通卷积不按覆盖率归一除数，紧贴透明区的像素少了
       那几格权重就偏暗（§3.16）。上游 `CIConvolution` 系列的边界处理不公开，本仓
       取「窗外夹到边界、窗外透明按 0」这一份显式口径，不声称与上游逐像素等同。

## 6. 16 位与 HDR

- **RGBA16 图层**：先经 `deep_to_rgba8`（`domain/layer.h`，唯一生产者）落 8 位工作副本，
  滤镜跑在副本上，**此后图层就是 8 位**（对齐 `DeepLayers.swift:5`：任何不认识 16 位的
  操作都让图层不再是 16 位）。note 写明
  `on the 16-bit layer's 8-bit working copy (the layer is RGBA8 afterward)`。
  撤销会把 16 位资产原样恢复（历史快照覆盖格式与字节）。
- **RGBA16F（HDR）图层**：`COMP_ERR_STATE`，不做静默降位。
- **组 / 无像素图层**：`COMP_ERR_STATE`。

## 7. 护栏

`tests/test_filters.cpp`（34 例，ABI + 域层混合视角）钉住：

- 恒等设置是**真 no-op**（字节、事务、撤销栈都不动；含 Gaussian Blur … Custom 这 12 个
  kind 的零初始化）；
- 越界参数在事务之前拒绝（43 组，含 NaN scale / 轴端点；覆盖 blur radius、
  noise amount、grain size / roughness、motion blur angle / distance、
  progressive blur from / to、sharpen amount / radius / threshold、
  reduce noise strength / preserve details / color noise、high pass radius、
  minimum / maximum radius、offset horizontal / vertical / wrap、
  custom kernel 项 / scale / offset 的边界内外）；
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
- Progressive Blur 的缺省轴自顶向下（顶部方差远大于底部、起点行逐字节不变）；
  显式水平轴的锐端在 `start`、`inverted` 换到另一端；径向档中心锐、角上糊；
  `scale = 0.25` 时远端比全尺寸更锐；`from == to` 与同 sigma 的 Gaussian Blur
  **逐字节相同**；一步撤销；
- Sharpen 让硬边更陡（跨边对比明显上升）、平坦区逐字节不动、alpha 不变、一步撤销；
  `threshold = 30` 时 120/128 的低对比棋盘格**逐字节不动**（阈值之外才改）；
- Reduce Noise 把噪声方差压到原来的一半以下、硬边对比保留（> 0.8×）、
  alpha 不变、note 含 `bilateral`、一步撤销；
  `reduce_color_noise = 0` 时平均 `|r - b|` 基本不动，= 100 时明显收窄（只动色度）；
- ABI 正确搬运五个新 kind 的字段（含布尔与 `explicit_axis` 的反相映射），
  以及 Sharpen / Reduce Noise 的六个字段与 Other 组四个 kind 的六个字段
  （`radius` ×3、`horizontal` / `vertical` / `wrap`），越界值在 ABI 边界同样被拒；
- High Pass 把平坦场均压到 128（逐字节）、硬边两侧一个向暗一个向亮地分离、
  alpha 逐字节不变、note 含 `mid-grey`、一步撤销；
- Minimum / Maximum 在透明底的 8×8 方块上把不透明像素数 64 → 16 / 64 → 144，
  窗口外的 alpha 归 0、方块内颜色逐字节保留、note 含 `erosion` / `dilation`、
  一步撤销逐字节还原；
- Offset 三种 `wrap` 各验一遍：透明留空（露出区逐字节 0）、边缘延展（角带被角落
  像素填满、移出内容仍丢失）、环绕（从对侧卷回、反向量逐字节还原）、
  每种都是一步撤销；
- Custom 的全 1 核 / divisor 25 作用在均匀图层上**逐字节不变**（钉住「窗外夹到边界、
  不当黑」）、恒等核 + `offset = 10` 精确每通道 +10、负结果夹到 0、
  `scale = 0` 与 `scale = 1` 逐字节相同、alpha 逐字节不变、透明像素被跳过、
  透明底方块角落落到 92（覆盖率不归一，钉住 §3.16 的边界）、一步撤销；
  ABI 侧 kernel / scale / offset 三个字段都搬运、零初始化 desc 回报 `changed == 0`
  且不改字节、越界核项与 `scale` 在 ABI 边界被拒。

另有 5 例在 `tests/test_tool_ops.cpp` / `tests/test_tool_ops_domain.cpp` 覆盖
Blur 工具（见 [`docs/tool-ops.md`](tool-ops.md) §4.9）。

## 8. Windows 外壳接线

「滤镜(&F)」菜单的条目由 `shells/windows/src/ui_core.h` 的 `filter_table()` 动态生成，
右侧面板的每一行也来自同一张表——**菜单、面板、值映射不各写一份清单**。
Other 组四个滤镜印证了这条：它们只在表的尾部追加四条，菜单项、面板行、
`build_filter_desc` / `filter_load_values` 的分支随之生效，`shell_window.cpp` 一行没改。
Custom 则多要了一样东西：**网格布局**。`FilterSpec` 尾部追加 `columns` / `grid_count`
（缺省 1 / 0），只有 Custom 填 5 / 25 —— 前 25 行按 5 列铺成数字表，第 26/27 行
（Scale / Offset）仍整宽。行矩形与命中测试共用 `filter_param_row_rect` /
`filter_param_row_at`（新增两个**带默认值**的参数），方向键在核内移动
（`filter_focus_grid`），单列面板的键位与绘制**逐像素未变**。

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
| Progressive Blur 0..3 | `progressive_blur.from/to/radial/inverted` | 0 / 20 / 0 / 0 |
| Sharpen 0..2 | `sharpen.amount/radius/threshold` | 0 / 1 / 0 |
| Reduce Noise 0..2 | `reduce_noise.strength/preserve_details/reduce_color_noise` | 0 / 50 / 25 |
| High Pass 0 | `high_pass.radius` | 0 |
| Minimum 0 | `minimum.radius` | 0 |
| Maximum 0 | `maximum.radius` | 0 |
| Offset 0..2 | `offset.horizontal/vertical/wrap` | 0 / 0 / 0 |
| Custom 0..24（5×5 核） | `custom.kernel[0..24]` | 中心 `kernel[12] = 1`，其余 0 |
| Custom 25..26 | `custom.scale/offset` | 1 / 0 |
| （渐变轴） | `progressive_blur.start_x/start_y/end_x/end_y` + `explicit_axis = 1` | 画布拖拽定轴；未拖拽时 `explicit_axis = 0` 用缺省中线 |

- **命令号**：菜单项 = `kCmdFilterBase(1200) + 滤镜序号`，当前 **1200..1215**
  （0..15 共 16 个滤镜，Other 组的 High Pass / Minimum / Maximum / Offset 占
  1211..1214，Custom 占 1215）。面板动作（应用 / 重置 / 关闭）**不在**这段里：滤镜涨到 11 个之后
  1210 是 Sharpen，原先放在 1210..1212 的应用/重置/关闭会与它抢同一个 id（Sharpen
  菜单变成「应用」、面板打不开）。动作号现落在 **1220..1222**，当初留出的
  1211..1219 余量正好装下 Other 组，动作号不用再搬，见 `shell_state.h`。
- **面板夹取、ABI 拒绝**：`clamp_filter_param` 只作用于键入/步进，保证壳不会把越界
  值送进 ABI；ABI 的规矩仍是 §2 的「越界拒绝」。
- **一次调用**：壳用固定 512 字节 note 缓冲调用一次 `comp_filter_apply`，不做
  `note_capacity = 0` 的长度探测（该函数先执行后写 note，探测会应用两遍）。
- **恒等如实回报**：`out_changed == 0` 时状态栏写「参数为恒等，未改动」。
- **Progressive Blur 的轴**：面板只承载两端半径与两个开关；渐变轴在画布上
  **拖一条线**来定（`main.cpp` 的 `progressive_axis_begin/extend/end`）。
  面板打开期间画布拖拽优先给轴，不落笔迹；手型 / 空格平移仍然优先。
  已提交的轴以虚线画在画布上（拖拽中为实线，径向档另画半径圆），
  `Ctrl+N` / 打开新文档会作废旧轴（端点是旧文档坐标）。轴本身不进撤销栈，
  只有回车应用才产生一步 `Filter: Progressive Blur`。
- **边缘色**：`vignette.color[3]` 固定 0,0,0（面板行模型只承载标量，颜色选择器
  属后续 UI 轮次，见 §3.1）。

面板的交互键位、几何常量与「哪些 Win32 路径未经人手实测」记在
[`docs/shell-parity.md`](shell-parity.md) §5.3。
