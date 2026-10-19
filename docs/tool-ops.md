# 工具操作（Tool Ops）参数契约

> 本文是 `comp_tool_apply` 的契约说明。ABI 声明见
> [`engine/abi/include/compositor/comp_layer.h`](../engine/abi/include/compositor/comp_layer.h)，
> 领域实现见 [`engine/domain/tool_ops.h`](../engine/domain/tool_ops.h) / `tool_ops.cpp`。

## 1. 为什么有这组接口

`engine/imaging/` 下有 10 组从上游 `robbietilton/Compositor` 移植的**纯 C 像素内核**。
它们的算法已经完成并有测试覆盖，但历史上**没有任何 `comp_*` 入口能触发它们**——
魔棒、修补、抖动、透镜、画笔对宿主而言是「有实现、无功能」。

缺的不是「把函数调用起来」，而是**参数契约**：笔压、选区、容差、种子、蒙版来源
在上游是工具层的交互概念（宿主跟踪鼠标采样与笔压曲线），内核签名里只留下结果。
本组接口把这些交互概念收敛成稳定结构，由引擎装配成内核签名要的形态。

## 2. 坐标空间（最容易踩的一条）

| 空间 | 谁在用 |
|------|--------|
| **文档像素坐标** | ABI 的 `select.seed_x/y`、`stroke_points` |
| **图层像素坐标** | 全部 10 组内核（魔棒种子、修补 coverage、透镜中心、笔迹） |

引擎显式做文档 → 图层的平移（用 `effective_bounds()` 的原点，对齐合成路径）。
种子落在图层之外 ⇒ `COMP_ERR_INVALID_ARGUMENT`，不做静默夹取。

`heal_coverage` 的尺寸与**图层像素**一致，不是画布。

## 3. 通用契约

- **一步撤销**：加入调用方已开启的显式事务，或自成一个隐式步。
- **参数闸在开事务之前**：非法参数不留半成品、不进撤销栈
  （`comp_document_can_undo` 保持不变）。
- **不改几何**：工具算子不改 `transform` / `blend` / `opacity`，因此
  `merge_down` / `flatten` 的逐字节不变性契约不受影响。
- **note 两段式**：非致命说明走 `note` + `out_note_size`；
  `note_capacity` 为 0 时只回报所需长度（含 `'\0'`）。终止符落在
  `note[min(capacity-1, size)]`，**永不写出调用方缓冲**。
- **错误码由域层直接回报**，不从文案反推。

| 错误码 | 场景 |
|--------|------|
| `COMP_ERR_NOT_FOUND` | `target` 图层不存在 |
| `COMP_ERR_INVALID_ARGUMENT` | kind 非法、种子出界、容差/半径/点数越界、coverage stride 过小 |
| `COMP_ERR_STATE` | 目标是图层组（不持有像素）、图层无像素、16 位降位要求 RGBA16 资产 |
| `COMP_ERR_OUT_OF_MEMORY` | 内核工作内存申请失败 |
| `COMP_ERR_UNSUPPORTED` | `deep` 的 round trip 模式（见 §7）。**域层可达、ABI 边界不可达**：`comp_tool_op_desc` 刻意不承载 `deep_mode`，经 `comp_tool_apply` 只能做 quantize；该码只由直接调域层的调用方触发 |

## 4. 逐算子语义

### 4.1 Select（`COMP_TOOL_SELECT`）

魔棒（`use_color_range = 0`）与 Color Range（`= 1`）。

- `tolerance`：**逐通道**距离阈值 0..255，alpha 也参与比较。
- `radius`：种子邻域半边长。参考色 = 该 `(2r+1)²` 方窗内的平均色，方窗按边界裁剪。
- `contiguous`：`1` = 从种子做 4-连通泛洪（种子自身不匹配则选中 0 个）；
  `0` = 选中全图所有匹配像素。
- `include` / `exclude`：直通 sRGB 三元组，各至多 8 色。`fuzziness` 为逐通道容差。
- **选中数不为 0 时才写蒙版**——点了个空处不留下一张全 0 的空蒙版
  （否则整层被蒙成透明；上游是「无选中即不改变」）。
- 写入是**加选**：与既有蒙版取逐像素最大值。
- **与只读魔棒的分工（S7）**：`COMP_TOOL_SELECT` 是**破坏性**的——它直接改图层蒙版，
  等于「选中即生效」。要「先选、再改」请用 `comp_selection_apply_wand`
  （`engine/abi/include/compositor/comp_selection.h`）：同样吃 `layer/source/seed/
  tolerance/contiguous/radius`，但只写选区 coverage，不改图层数据；
  `COMP_SELECTION_WAND_MERGED` 取整篇合成、`LAYER` 要求图层是 1:1 整数平移。
  选中 0 个时两条通路都遵守「无选中即不改变」（不写空蒙版、不清空既有选区）。
  语义与外壳手势见 README §6.17。
- **S8 外壳补充**：`radius` 的 N×N 取样窗口有了外壳入口（魔棒激活时 `Alt+[` /
  `Alt+]`，量程 0..512，状态栏显示 `(2r+1)×(2r+1)`）；绝对羽化用
  `comp_selection_set_feather`（**覆盖**语义），与 `COMP_SELECTION_FEATHER` 的
  `sqrt(cur² + r²)` **叠加**语义明确分工。见 README §6.18。

### 4.2 Stroke（`COMP_TOOL_STROKE`）

圆头笔，`stroke_points` 为折线折点（x, y 成对，共 `stroke_point_count * 2` 个 float）。

- 形状 = 到最近线段的距离 ≤ `diameter / 2`；
  `hardness` 决定 `[hardness * r, r]` 这段的 smoothstep 羽化带
  （`1` = 全实，`0` = 从笔芯就羽化）。
- **单点折线（`stroke_point_count == 1`）是一记圆形 dab**：鼠标只按一下、不移动
  也必须留下笔尖（参数闸允许 1..65536 个折点，不能对 1 个点静默不画）。
- `pressure * flow` 乘进峰值 coverage。两者相乘是上游画笔口径：
  `pressure` 是笔压，`flow` 是「单次落笔不把像素涂死」的总量上限。
- **不叠加**：笔画按 coverage 做 source-over（同一像素被同一笔压两次仍是同一结果）。
  这让「同参数两次调用逐字节相同」成立——若改成累加，可重复性就没了。
- `pressure * flow == 0` ⇒ 逐字节不变。
- `seed` 保留在结构里以固定契约；当前圆头笔形状是解析式的（不采样噪声），故不参与计算。
- `erase != 0`（S5 尾部追加之字段）= **橡皮擦模式**：同一支笔的形状与 coverage，
  但合成方向反过来，对**预乘**像素做 destination-out——
  `px[c] = px[c] * (1 - coverage)`，四个通道一起乘，alpha 归零时颜色也随之归零
  （在直通色上只减 alpha 会留下「透明却发亮」的脏数据）。
  因此它擦掉的正是 Paint 模式同一笔会盖上去的那一份。橡皮擦**忽略 `brush.grain_*`**
  （颗粒是「落墨」的噪声，擦除没有对应语义）；`pressure * flow == 0` 依旧是逐字节 no-op。
  只有 `COMP_TOOL_STROKE` 读这个字段，其余算子不受影响。

### 4.3 Heal（`COMP_TOOL_HEAL`）

`mode`：`0` = Content-Aware（PatchMatch）、`1` = Create Texture、`2` = Proximity Match。
三者都是 `spot_heal` 的 mode（`patch_fill` 与 `heal_membrane` 经它传递调用）。

结果按 `coverage * opacity` 替换原像素。`seed` 固定 ⇒ 同参数同结果
（PatchMatch 内部是随机化的，必须靠它定住）。

### 4.4 Dither（`COMP_TOOL_DITHER`）

`style` 0..9 = Atkinson / Floyd-Steinberg / Bayer 2,4,8 / Dots / Lines / Diamonds / Patterns / Glyphs。

**字形图集不由 ABI 携带**（`glyphs` / `glyphCoverage` / `glyphCount` 是宿主资源）。
`style = 9`（GLYPHS）时这三项为 0，内核走 `spot()` 的 `default` 分支，
即**菱形网点**——note 会如实回报这一回落，不静默按方块画。

`levels` 2..16（内核自身上限 16；面板常用 2 = 纯 1-bit）。

### 4.5 Lens（`COMP_TOOL_LENS`）

径向畸变，作用在**图层像素**上，几何不变。

- `k > 0` 拉直桶形畸变（边角裁切）；`k < 0` 拉直枕形畸变（边角转透明）；
  `k = 0` 恒等（逐字节不变，引擎另有一条短路）。
- 畸变中心取**图层像素的中心**。上游 Lens 是整篇镜头（中心 = 画布中心），
  这里把作用面收窄到当前图层，与上游工具栏一致，也避免图层在画布一角时轴心跑偏。
- `|k| > 1` 被参数闸拒绝。

### 4.6 Deep（`COMP_TOOL_DEEP`）

16 位资产的落位，也是 ABI 唯一暴露的 deep 模式（**quantize**）：把图层像素从
`COMP_PIXEL_RGBA16`（预乘、native-endian）换成 8 位预乘 RGBA8 的工作副本。实现是
`domain/layer.h` 的 `deep_to_rgba8`——`dither_quantize16` 加约一阶噪声后取整，
对齐上游 `DeepLayer.dithered`：平滑梯度不出台阶，且噪声按 (x,y,通道) 固定，
**同一输入恒得同一输出**。

- 这份 8 位副本是 16 位图层的**唯一 8 位视图**：合成、缩略图、裁剪基底 alpha、
  8 位像素工具都由它或与它同口径的取值出发（`comp_layer.h` 的格式契约）。
  于是「传 16 位资产」与「传它的 8 位副本」合成结果逐字节一致。
- 8 位工具（Select / Stroke / Heal / Dither / Lens）打在 16 位图层上时，图层先落到
  这份副本、**自此就是 8 位**，与上游「任何不认识 16 位的操作都让图层不再是 16 位」
  （`DeepLayers.swift:5`）一致。
- 打在 RGBA8 图层上 -> `COMP_ERR_STATE`（拿 8 位图层要 16 位精度是错的**状态**请求，
  不是能力边界）。round trip 模式 -> `COMP_ERR_UNSUPPORTED`（见 §7）。
- `.comp` 存盘：RGBA16 / RGBA16F 图层写成 `<uuid>.exr` 资产（C3b）；只有宿主明确
  要求 8 位资产时才需要先 quantize（见 §6）。

### 4.7 Clone Stamp（`COMP_TOOL_CLONE`）

笔刷形状与 Stroke 同源（`brush.diameter / pressure / hardness / flow`），
`clone` 子结构只承载**源**与采样策略：

| 字段 | 语义 |
|------|------|
| `source_x/y` + `has_source` | 宿主吸管落点（文档像素）。没有源且没有可沿用的偏移 ⇒ `COMP_ERR_STATE` |
| `offset_x/y` + `has_offset` | 「源 − 首笔落点」的整数像素偏移，由宿主在两次落笔之间带回 |
| `aligned` | 非 0 且 `has_offset` 非 0 时沿用带回来的偏移；否则按 `round(source − points[0])` 现算 |
| `sample_all_layers` | 非 0 = 源快照取整篇实时合成；0 = 只取目标图层自身像素 |
| `opacity` | 0..1，与 `pressure × flow` 相乘作为 coverage 峰值 |

**快照在整笔开始前取好**：同一笔里既画又采样会吃到自己刚画的像素，这不是仿制
图章的行为。源点落在图层外时**跳过**该像素（不落透明——那会在图层上擦出洞）。
`sample_all_layers` 只按图层包围盒原点把合成面搬进图层空间；scale / rotation
不参与换算（与 Stroke 相同的边界）。

### 4.8 Smudge / Liquify（`COMP_TOOL_SMUDGE` / `COMP_TOOL_LIQUIFY`）

`warp.strength` 为 0..1；笔刷直径与硬度同样来自 `brush`。两者共用上游的间距与
权重口径：

- Smudge 间距 `max(1, diameter × 0.005)`：首点只把像素读进 carried 缓冲
  （`pickUp`），不落笔；之后每个 dab 做
  `painted = under + (carried − under) × weight × strength`，并把 painted 写回
  carried，于是颜色被一路拖走。
- Liquify（Push）间距 `max(1, diameter × 0.025)`：每个 dab 先把覆盖区复制成
  scratch，再按 `move = (to − from) × strength` 做**前向**双线性重采样；
  读 scratch 而非当前像素，避免自反馈。
- 权重 `u <= hardness ? 1 : smoothstep((1−u)/(1−hardness))`，hardness 夹到 0.98
  （上游上限）。
- `strength = 0` ⇒ 逐字节不变（本仓显式允许 0，便于宿主做「强度 0 预览」；
  上游把 opacity 夹在 0.01..1）。
- **warp 直径上限 1024**：carried / scratch 是 O(diameter²) 个 float，
  8192 直径仅 carried 就约 1 GB；越界在参数闸返回 `COMP_ERR_INVALID_ARGUMENT`。
- 折点坐标必须有限且 `|v| <= 1e6`（否则取整 UB）；点数 1..65536。
- **壳层接线（2026-10-18）**：Windows 壳的 `retouch_stroke_*` 按
  `shell.retouch_mode` 选 kind——Blur 用 `comp_tool_blur.radius`，Smudge /
  Liquify 用 `comp_tool_warp.strength`（`clamp_retouch_strength` 夹在 0.01..1，
  `Alt+[` / `Alt+]` 步进 10%）。鼠标折线在 `WM_LBUTTONUP` 一次提交，落笔前
  只在画布上画预览线，不产生中间事务。

### 4.9 Blur（`COMP_TOOL_BLUR`，P1 尾部追加）

上游 `BlurToolMode` 的 Blur 模式：先把**整层**模糊成一份样本，再沿折线按笔刷
coverage 把样本盖回当前像素。

- 几何复用 `stroke_points` 与 `brush.diameter / hardness / pressure / flow`；
  `blur.radius` 是高斯半径（**图层像素**），量程 **0.5..50**。`0` 在参数闸被拒绝，
  而不是静默当作「不模糊」——上游把 `blurRadius` 夹到 0.5..50，0 会被抬高，
  宿主若按「radius 0 = 恒等」使用就会得到相反的结果。
- `sigma = min(radius, max(width, height) * 0.5)`（上游 `sidePixels / 2` 的上夹）。
- **快照在整笔开始前取好**：同一笔内的折线不会吃到自己刚模糊过的像素；
  以同一参数重复落笔才会继续变软（每笔都以当笔开始时的像素为准）。
- 盖回是预乘空间的两点插值 `p + (blurred − p) × coverage`，**四个通道一起**：
  alpha 也参与，透明边缘随之变软（上游对整幅 CIImage 做高斯模糊）。
- 折点坐标与其它落笔共用「有限且 `|v| <= 1e6`」的闸；点数 1..65536。
- 16 位 / HDR：与 Stroke/Clone/Smudge/Liquify 同属落笔族——笔尖盖到的像素写成
  8 位、笔尖之外**保留深像素**（`deep_take_painted` 口径，见 §4.6）。

### 4.10 Gradient（`COMP_TOOL_GRADIENT`，S9 追加）

上游渐变 / 填充工具的**最小闭环**：两个文档像素端点 + 两个色标 + 不透明度，一次性
铺满整层。它是**整幅算子**，不读 `stroke_points`、不是落笔族。

- 类型：`gradient.type = 0` 线性（`t` = 端点连线上的投影比例）、`1` 径向（`t` = 到
  起点的距离 / 两端点距离）。`t` 夹到 **0..1**，端点之外是纯色带而不是回绕。
- 色标：`color0` → `color1`（直通 sRGB + alpha，**非预乘**）按 `t` 线性插值；混合是
  预乘 **source-over**，因此「不透明 → 透明」只叠颜色、不挖洞。
- `opacity` 量程 0..1（0 在参数闸被拒绝，而不是静默当恒等）。活跃选区按
  `selection_effective_coverage` 逐像素乘进覆盖率，**覆盖率 0 的像素逐字节不动**
  （与像素滤镜同一口径）。
- 参数闸都在 `txn_begin` **之前**：未知类型、非有限或 `|v| > 1e6` 的端点、端点重合
  （`|end − start| < 1e-3`，没有方向/半径可言）、`opacity` 越界或 0、两个色标全透明。
  全部 `COMP_ERR_INVALID_ARGUMENT`，撤销栈与像素都不动。
- 16 位 / HDR：整幅算子写 8 位工作副本——RGBA16 会被降到 8 位再写回（与
  Dither / Lens 同口径），RGBA16F 直接 `COMP_ERR_STATE`。
- 外壳（S9）：`G` 选工具、已在该工具时再按 `G` 切线性/径向；拖出两端点后松开提交，
  色标固定「前景 → 透明」，`Shift` 反向。拖拽短于 1 px 时外壳直接放弃并提示，
  不把一次手抖变成 `INVALID_ARGUMENT`。

### 4.11 Shape（`COMP_TOOL_SHAPE`，S10 追加）

上游 ShapeTool（`Document/ShapeTool.swift`）的**最小闭环**：矩形 / 椭圆 / 直线三种
拖拽几何 + 通用三次贝塞尔路径，填充与描边是两个独立开关。它也是**整幅算子**：
不读 `stroke_points`、不是落笔族，松开时一次铺完。

- 几何（全部是**文档像素**，引擎减掉图层 `effective_bounds()` 原点后落图层局部像素）：
  - `kind = 0` 矩形 / `kind = 1` 椭圆：`(x0,y0)-(x1,y1)` 是对角点，顺序任意；
    矩形的 `corner_radius` **夹到短边一半**，`<= 0` 为直角（0.5522847498307936 的四段
    三次贝塞尔近似圆角）。
  - `kind = 2` 直线：`(x0,y0)-(x1,y1)` 是两端点，**恒为描边**——`fill_enabled` 必须为 0
    （填一条「实心线」没有定义，显式拒绝而不是静默忽略），`stroke_enabled` 必须非 0。
  - `kind = 3` 路径：`segments` 是 1..65536 段三次贝塞尔，`fill` 走**非零缠绕**
    （反向子路径成为洞），`closed` 只影响描边是否回到起点。S10 只有 ABI 通路能产出路径。
- 抗锯齿：`antialias != 0` 时 4 行超采样 + 水平解析覆盖率（直边是精确的半像素规则）；
  0 时按像素中心一票取硬边。填充与描边共用同一套覆盖率窗口。
- 描边 = **胶囊距离并集**：`cov = clamp(0.5 + 半宽 − 距离, 0, 1) × 255`，所以端点/折点是
  圆头（上游 `line` 的 `.round` 线帽同款）。`stroke_width` 是文档像素**全宽**；
  共线折线按 ≤32 px 切段，避免长边距离场退化。
- 合成：`alpha = coverage/255 × color.a/255 × opacity`，**先填充后描边**（描边压在上面），
  预乘 source-over。颜色是直通 sRGB + alpha（非预乘）。
- 选区：活跃选区的 `selection_effective_coverage` 逐像素乘进覆盖率，覆盖率 0 的像素
  **逐字节不动**；图层像素落在画布外视为选区之外（与渐变/滤镜同一口径）。
- 参数闸都在 `txn_begin` **之前**：kind 越界、填充与描边全关、可见色全透明（alpha 0）、
  `opacity` 越界或 0、**描边开启时** `stroke_width` 非正、坐标为 NaN/Inf 或 `|v| > 1e6`、
  矩形/椭圆宽或高为 0、直线两端点重合、路径段数为 0/超限、路径坐标非法。
  全部 `COMP_ERR_INVALID_ARGUMENT`，撤销栈与像素都不动。
  （纯填充形状**不看** `stroke_width`：0 合法，宿主不必为一个用不到的字段编数值。）
- 与目标图层**完全不相交**：成功、无像素改动、**不产生撤销步**，note 如实说明
  （否则宿主会看到一步什么都没做的撤销）。
- 16 位 / HDR：整幅算子写 8 位工作副本，RGBA16 经 `merge_deep_paint` 合回；
  RGBA16F 直接 `COMP_ERR_STATE`（与 Dither / Lens / Gradient 同口径）。
- 与上游的**三处显式差异**（写死在 `engine/domain/tool_ops.h` 的 `ShapeParams` 注释）：
  1. 上游画完会**新建一个像素图层**，本仓工具算子统一「画进当前图层」（新建图层由宿主
     先做）；工具算子不该改图层树结构，否则 merge/flatten 的逐字节契约与撤销粒度都要改。
  2. 上游矩形/椭圆只填充、直线只描边；本仓把填充与描边做成两个独立开关
     （`fill`+`stroke` 同时开 = 超集能力，先填后描）。
  3. 路径在本轮只是 ABI 能力，外壳的钢笔仍用于提交选区，不建形状。
- 外壳（S10，`U`）：矩形 / 椭圆 / 直线三种可拖，已在该工具时再按 `U` 循环；
  拖对角点（直线拖两端点）后松开提交，矩形/椭圆填充前景色、直线用前景色描边；
  `Shift` = 正方/正圆，`Alt` = 从中心拉（松开时读修饰键）；`Alt+[` / `Alt+]` 调线宽，
  `Shift+Alt+[` / `Shift+Alt+]` 调矩形圆角；`Esc` / 切工具 / 失焦只丢预览、不产生撤销步；
  拖拽短于 1 px 时外壳先放弃并提示，不把一次手抖变成 `INVALID_ARGUMENT`。

### 4.12 Type（`COMP_TOOL_TYPE`，S11 追加）

上游 `Document/TypeTool.swift`（472 行）是**系统字体 + 富文本**工具；本仓零依赖、不能链接
系统字体，因此做的是它最有价值的那一半：**内置位图字体的栅格化内核**，输出与其它算子一样
是「一次 `comp_tool_apply` 落进当前图层」。它也是**整幅算子**：不读 `stroke_points`、
不是落笔族，提交时一次铺完。

- 字体是编译进内核的 **5×7 列主序** ASCII 点阵（`0x20`..`0x7E` 共 95 字形，
  每字形 5 字节、bit0 = 最上一行），字距基数为 6 列（5 列字形 + 1 列间隙）。
  因此**只支持 ASCII**：UTF-8 多字节、Tab、控制字符一律 `kBadText` →
  `COMP_ERR_INVALID_ARGUMENT`，**不画方框/豆腐块顶替**。
- 排版（全在 `type_raster.cpp` 的 `build_layout`）：`scale = font_size / 7`，
  字形水平步距 `6 * scale + letter_spacing`，行距 `font_size * line_height`；
  `\n` 分行，**行尾 `\n` 产生一个额外空行**（与文本编辑器一致，不偷偷丢掉）；
  文本框宽度 = 最长行 `字形数 * 步距`，高度 = `font_size + (行数-1) * 行距`。
- 抗锯齿：`antialias != 0` 时按**精确面积覆盖率**（字形格与像素格的交集面积，
  逐像素累加并饱和到 `uint8`，不溢出）；`0` 时按像素中心一票取硬边。
  覆盖率再乘 `opacity` 与 `color.a/255`，用预乘 source-over 落图层。
- 量程与参数闸（**全在 `txn_begin` 之前**）：文本非空且 ≤ 65536 字符；`font_size`
  6..2048、`line_height` 1..8、`letter_spacing` 使水平步距 **≥ 0.5 px**（负字距可以，
  但不能把字形挤到重叠）；坐标 NaN/Inf 或 `|v| > 1e6`、`opacity` 越界或 0、
  可见色全透明——一律 `COMP_ERR_INVALID_ARGUMENT`，撤销栈与像素都不动。
- 两类**成功 no-op**：文本框完全落在图层外（`kEmpty`），以及一个像素都没落墨
  （纯空格文本、或活跃选区覆盖率全为 0）。两者都 `txn_abort` 整步撤回、
  **不产生空撤销步**，note 如实说明。
- 16 位 / HDR：整幅算子写 8 位工作副本，RGBA16 经 `merge_deep_paint` 合回，
  RGBA16F 直接 `COMP_ERR_STATE`（与 Dither / Lens / Gradient / Shape 同口径）。
- 与上游的**显式差异**（不是遗漏）：① 没有系统字体/字体选择/CJK/富文本（粗体、斜体、
  对齐、旋转、文字描边都没有）；② 文字是**一次性像素**，不是可再编辑的文本对象，
  提交后无法回改内容（撤销只能整步回滚）；③ 上游画完新建图层，本仓画进当前图层
  （与所有工具算子同一条规矩）。
- 外壳（S11，`T`）：点击画布落插入点 → 键入 ASCII → 回车提交；`Shift+Enter` 换行、
  `Backspace` 删字符、`Esc` / 切工具 / 失焦取消（只丢预览、不产生撤销步）；
  `[ / ]` 调字号，`Shift+[ / ]` 调字距。画布预览只画插入点光标与文本框
  （白实线 + 黑虚线），**不预演字形**——外壳没有系统字体，用 GDI 字画一套只会与
  落笔结果不一致。

## 5. 可重复性

`dither_apply` 的误差扩散与 `patch_fill` 的 PatchMatch 都是随机化/顺序敏感的。
`seed` 字段与「不叠加」的落笔口径共同保证：**同参数两次调用逐字节相同**。
`tests/test_tool_ops.cpp` 对此有专门护栏（撤销后重做 = 逐字节相等）。

## 6. 与 `.comp` 的关系

工具算子**不引入新的 manifest 键**：结果都落进既有的 `layer` 像素 / `mask` / `transform`，
因此 `.comp` 往返与既有护栏完全不受影响。

## 7. 接线状态（2026-10-18 更新：已无「孤立内核」）

本表曾记录「已实现但无产品通路」的内核。除最后一行为**主动不接**外，其余全部已接进
`comp_tool_apply`；与 README §6.6 的表格一致。

| 内核函数 | 现状 | 通路 |
|---------|------|------|
| `deep_split` / `deep_merge` / `deep_take_painted` / `deep_take_painted_hdr` | 已接线（2026-10-18 收口最后一对 `take_painted`） | `COMP_TOOL_DEEP`（round trip，RGBA16 保留 16 位）+ 落笔族（Stroke / Clone / Smudge / Liquify 在 RGBA16 / RGBA16F 上保留深像素，见 README §6.1.21） |
| `cube_apply`（16bit）/ `hdr_normalize` / `hdr_restore` / `hdr_exposure` / `extract_alpha` / `restore_alpha` / `extract_alpha_hdr` / `restore_alpha_hdr` | 已接线 | `COMP_TOOL_DEEP` 与 half 合成表面（README §6.1.16 / §6.1.19） |
| `wand_trace` | 已接线（只读闭合轮廓） | `comp_tool_coverage_bounds` |
| `scanlines_apply` / `dither_dots` / `dither_glow` | 已接线 | `COMP_TOOL_DITHER` 的 `scanlines` / `chunky` 模式 |
| `heal_coverage_bounds` | 已接线 | `comp_tool_coverage_bounds` |
| `brush_alpha_bounds` / `layer_extract_alpha` / `layer_unpremultiply_opaque` / `layer_restore_alpha` | **主动不接** | 剪贴栈的 alpha 拆合。上游用它把剪贴基底的不透明度摊平；本仓合成路径走 `clipping_factor()` 逐点取 alpha，再由 `extract_alpha`/`restore_alpha` 处理深度路径，语义已覆盖，保留这四个函数只会造成两套写法 |

## 8. 交付与验证基线（2026-10-16 DSH 接手复核；2026-10-17 Grok 收口 16 位通路）

- 实现：`engine/domain/tool_ops.{h,cpp}`（领域层闸 + 装配）、ABI 入口
  `comp_tool_apply`（`engine/abi/src/comp_layer_abi.cpp`）、护栏 `tests/test_tool_ops.cpp`
  （ABI 视角）与 `tests/test_tool_ops_domain.cpp`（域层视角，覆盖 ABI 不可达的 deep 模式契约）。
- Grok 收口（16 位通路）：`comp_layer_set_pixels` 接受 `COMP_PIXEL_RGBA16` 之后，
  顺带把「16 位资产的 8 位视图」补成真实现——`domain/layer.h` 的 `deep_to_rgba8` 是唯一生产者，
  合成/缩略图（`compositing/resample.cpp` 的 `build_layer_unit`）、裁剪基底 alpha
  （`compositing/composite_ops.cpp` 的 `clipping_factor`）、8 位像素工具
  （`domain/tool_ops.cpp`）三处消费点全部改走它。此前这三处会按 4 字节/像素读
  8 字节/像素的缓冲：不报错，画面是垃圾。
- 全量基线：**207 用例 0 失败**（AUDIT7 的 204 + 本轮 ABI 16 位等价性 1 + 域层 2；原
  `layer_set_pixels_rejects_16bit_format` 改名并只保半精度那一半），
  `scripts/run_regression.py --require-ohos` **6/6 PASS**，OHOS aarch64 交叉编译 72 单元 rc=0。
  （跑回归必须用带 ziglang 的解释器 `<local-workspace>\tools\python\python.exe`；
  用 Studio 自带 venv 的 `python` 会因缺 `ziglang` 让步骤 1/2/3/5 全红——那是环境问题不是测试失败。）
- 复核结论（2026-10-18 更新）：**7 组「孤立内核」已全部有产品通路**——
  `wand_mask`/`wand_trace`（Select）、`spot_heal` → 内部 `patch_fill` + `heal_membrane`（Heal / Patch）、
  `dither_apply` + `scanlines`/`chunky`（Dither）、`lens_distort`（Lens）、`dither_quantize16`（Deep 降位）、
  以及本轮收口的 `deep_take_painted(_hdr)`（落笔族保留深度）。
  Brush 组本身（`BrushPixels.c` 只有 4 个剪贴栈 alpha 助手）由解析式圆头笔实现替代，
  且剪贴栈已改走 `extract_alpha`/`restore_alpha`，属 §7 的**主动不接**。
- 本轮基线（2026-10-18）：**344 用例 0 失败**，`scripts/run_regression.py --require-ohos`
  **6/6 PASS / 0 skipped**；CMake（`ctest`）与 clang-cl（MSVC ABI）两条路径同为 344/344。
- P1 追加（2026-10-18）：`COMP_TOOL_BLUR = 12`（§4.9）与 Filter 菜单四条通路
  （[`docs/filters.md`](filters.md)）同时落地，基线 352 → **368 用例 0 失败**
  （16 例新增：11 滤镜 + 5 模糊）。`comp_tool_op_desc.blur` 是**尾部追加**字段，
  既有字段偏移不变。
- P1 UI 接线（2026-10-18）：Blur 模式接上 Windows 壳的画布折线——`R` 在
  Blur/Smudge/Liquify 间循环（后两者状态栏如实标注「尚未接内核」），
  `Alt+[` / `Alt+]` 调半径，`WM_LBUTTONUP` 把整条折线一次 `comp_tool_apply`
  提交；失焦 / 捕获丢失取消未完成笔迹。基线 368 → **373 用例 0 失败**
  （5 例 `test_ui_core`：滤镜表、面板状态机、行几何、retouch 循环、笔迹去抖），
  Windows 壳自检 27 → **41 项**。Win32 交互路径未经人手实测，只验证到 ABI 与
  `ui_core` 数学层。
- S4 追加（2026-10-18）：Smudge / Liquify 接上 Windows 壳的落笔折线（强度
  `Alt+[` / `Alt+]` 步进），基线 373 → **377 用例**，壳自检 41 → **59 项**。
- S5 追加（2026-10-18）：Stroke 增加 `erase` 尾部字段（§4.2，destination-out），
  新增 2 例：擦除按 coverage 做减法而非覆盖、零压力 no-op。全量回归
  `build.py` → **391 cases, 0 failed**，`run_regression.py --require-ohos` 6/6 PASS。
  选区侧的载入/平移/像素搬移与吸管不走 `comp_tool_apply`，见 README §6.15。
- S6 追加（2026-10-18）：外壳接线（前景色/吸管/`E` 橡皮擦/拖动选区），不新增 ABI；
  基线 391 → **397 用例**，壳自检 59 → **71 项**，6/6 PASS。见 README §6.16。
- S7 追加（2026-10-18）：新增 `comp_selection_apply_wand`（第 111 个导出 ABI，
  §4.1 的只读魔棒）——它**不是** `comp_tool_apply` 的算子，而是 `comp_selection_*`
  家族的选区构造入口；`COMP_TOOL_SELECT` 的破坏性合同不变。基线 397 → **408 用例**
  （`test_selection` 7 + `test_ui_core` 4），壳自检 71 → **90 项**，6/6 PASS。
  见 README §6.17。
- S8 追加（2026-10-18）：新增 `comp_selection_set_feather`（第 112 个导出 ABI，
  **绝对**羽化；§4.1 末的 S8 补充），并把钢笔路径（`comp_selection_apply_path`）与
  魔棒 N×N 窗口接进外壳；不新增 `comp_tool_apply` 算子。基线 408 → **414 用例**
  （`test_selection` 3 + `test_ui_core` 3），壳自检 90 → **103 项**。见 README §6.18。
- S9 追加（2026-10-18）：新增 `COMP_TOOL_GRADIENT = 13`（§4.10），`comp_tool_op_desc.gradient`
  是**尾部追加**字段，既有字段偏移不变；不新增导出 ABI（仍是 **112** 个）。基线
  414 → **418 用例**（`test_tool_ops` 3 + `test_ui_core` 1），壳自检 103 → **113 项**。
  见 README §6.19。
- S10 追加（2026-10-18）：新增 `COMP_TOOL_SHAPE = 14`（§4.11），`comp_tool_op_desc.shape`
  是**尾部追加**字段，既有字段偏移不变；不新增导出 ABI（仍是 **112** 个）。
  同时新增两个内核文件：`engine/domain/path_raster.{h,cpp}`（把路径基元与
  扫描线填充从 `selection.cpp` 抽出，行为中立，`selection` 只是改名接线）与
  `engine/domain/shape_raster.{h,cpp}`（fill/capsule-stroke/圆角矩形/路径扁平化 +
  窗口化覆盖率）。基线 418 → **424 用例**（`test_tool_ops` 5 + `test_ui_core` 1），
  壳自检 113 → **137 项**，`run_regression.py --require-ohos` 6/6 PASS。
  见 README §6.20。

- S11 追加（2026-10-18）：新增 `COMP_TOOL_TYPE = 15`（§4.12），`comp_tool_op_desc.type`
  是**尾部追加**字段，既有字段偏移不变；不新增导出 ABI（仍是 **112** 个）。
  同时新增字体内核文件 `engine/domain/type_raster.{h,cpp}`（内置 5×7 列主序 ASCII
  点阵 95 字形 + 字号/行距/字距排版 + 精确面积覆盖率 AA / 像素中心硬边）。基线
  424 → **431 用例**（`test_tool_ops` 4 + `test_tool_ops_domain` 2 + `test_ui_core` 1），
  壳自检 137 → **150 项**，`run_regression.py --require-ohos` 6/6 PASS。见 README §6.21。
- S12 追加（2026-10-18）：内部 C++ 画布级合成缓存（`composite_document_cached`，
  派生数据，不进 `DocumentState`/历史/`.comp`），**不新增算子、不改 ABI**（仍 112 个
  导出）。基线 431 → **435 用例**（`test_composite_cache` 4），壳自检 150 → **155 项**，
  `run_regression.py --require-ohos` 6/6 PASS。见 README §6.22。
