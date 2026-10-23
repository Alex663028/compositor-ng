# 调整图层参数 JSON 键名表（`.comp` engineAdjustments 契约）

> 本表是**唯一权威**。键名逐字取自上游 `robbietilton/Compositor` 的
> `Compositor/Document/LayerAdjustment.swift` 及其嵌套 `*Settings` 结构，
> **不做重命名、不做展平**，确保 `.comp` 在两端往返保真。
> 代码侧单一真源：`engine/domain/adjustment_table.{h,cpp}`；外壳面板经
> `comp_layer_adjustment_param_*` 读取同一张表生成控件（不会各自维护一份）。

## 1. 落盘位置与形态

每个图层在 `.comp` 的 `manifest.json` 中带一个数组键 `engineAdjustments`（自有键，不与上游键冲突）：

```json
{
  "engineAdjustments": [
    { "kind": 3, "settings": "{\"exposureSettings\":{\"exposure\":1.5,\"gamma\":2.2}}" }
  ]
}
```

- `kind`：整数，见下表 §2。0..11 等于上游 `AdjustmentKind` 的 `CaseIterable` 序号；
  12..15 是本仓追加值（上游无对应），只在追加位置出现，既有编号不重排。
- `settings`：**UTF-8 JSON 字符串**（不是嵌套对象），内容为 §3 的键名表结构；引擎原样保真存取，不做规范化。

## 2. kind 编码（冻结，禁止重排）

| kind | 名称（上游 rawValue） | 面板可逐参数编辑 |
|------|----------------------|------------------|
| 0 | `Hue/Saturation` | 是 |
| 1 | `Levels` | 是（通道条 + 20 个滑杆参数，§3.1） |
| 2 | `Curves` | 专用控制点编辑器（不走参数表，§3.2 + §5 曲线 ABI） |
| 3 | `Exposure` | 是 |
| 4 | `Gradient Map` | 是 |
| 5 | `Grain` | 是 |
| 6 | `Add Noise` | 是 |
| 7 | `Gaussian Blur` | 是 |
| 8 | `Motion Blur` | 是 |
| 9 | `Invert` | 无参数 |
| 10 | `Black & White` | 是 |
| 11 | `Color Balance` | 是 |
| 12 | `Brightness/Contrast` | 是（**本仓追加**，上游无） |
| 13 | `Desaturate` | 无参数（**本仓追加**） |
| 14 | `Threshold` | 是（**本仓追加**） |
| 15 | `Posterize` | 是（**本仓追加**） |

`COMP_ADJUSTMENT_KIND_COUNT = 16`；越界 kind 在 ABI 层直接返回 `COMP_ERR_INVALID_ARGUMENT`，
**不回落**到任何默认类型。

12..15 是本仓在 S27 追加的**破坏性**调整（`Image > Adjustments`，README §6.94），
上游 `AdjustmentKind` 没有这四种，所以**不参与与上游的 kind 互操作**；但作为调整图层
写入 `.comp` 时同样合法（求值器只做逐像素映射），`tests/test_adjustment.cpp`
的 `adjustment_extended_kinds_round_trip_through_comp` 锁着这条往返。

## 3. 键名表（settings 对象内）

`路径`列即 `comp_adjustment_param.json_path`（相对 settings 对象的点分路径）。
未列出的键在 ABI 层被拒绝（键名白名单），列出的数值/布尔字段会做类型与范围校验。

### kind 0 — Hue/Saturation（可含 `hsvSettings` 对象，本表只编辑顶层三项）

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `hue` | number | -360 … 360 | 0 |
| `saturation` | number | -100 … 100 | 0 |
| `lightness` | number | -100 … 100 | 0 |
| `colorize` | bool | — | false |

### kind 1 — Levels（`levels` 对象）

`levels.channel`（`"RGB"` / `"Red"` / `"Green"` / `"Blue"`）只决定面板高亮哪个通道条，
求值恒为「RGB 用 `ranges[0]`，R/G/B 各用 `ranges[1..3]`」（同上游 `Levels.swift`）。
`levels.ranges` 是**定长 4 项**数组，每项 `{ black, gamma, white, outputBlack, outputWhite }`；
数组下标有语义，所以参数表里的路径带数字段（`levels.ranges.1.black` 等），ABI 按数组下标解析。

| 路径（`ranges.<i>.` 后同列，i = 0…3） | 标签前缀 | 类型 | 范围 | 默认 |
|------|------|------|------|------|
| `black` | `RGB` / `R` / `G` / `B` `Black` | number | 0 … 254 | 0 |
| `gamma` | `… Gamma` | number | 0.1 … 9.99 | 1 |
| `white` | `… White` | number | 1 … 255 | 255 |
| `outputBlack` | `… OutLo` | number | 0 … 255 | 0 |
| `outputWhite` | `… OutHi` | number | 0 … 255 | 255 |

`white` 与 `black` 的相对顺序不靠校验兜底，由求值侧 `normalize()` 保证（同上游）。
面板渲染顺序 = 上表顺序（通道分页：`levels.channel` 决定当前页，20 个参数全量可滚）。

### kind 2 — Curves（`curves` 对象）

```json
{ "curves": { "channels": [ [ {"x": 0, "y": 0}, {"x": 128, "y": 190}, {"x": 255, "y": 255} ],
                           [], [], [] ] } }
```

- `channels` 是 4 项数组，下标 0 = RGB 合成，1..3 = R/G/B；**空数组 = 恒等曲线**（不是错误）。
- 每通道 2..32 个点，`x` 严格递增、首点 `x = 0`、末点 `x = 255`、`x`/`y` 都在 0..255。
- 点同时接受对象形式 `{"x":N,"y":N}` 与紧凑形式 `[x, y]`（上游两种都出现过）。
- 曲线求值是形状保持的三次 Hermite（Fritsch–Carlson），见 `engine/compositing/adjustment_eval.cpp`。

面板不通过参数表编辑（`param_count` = 0），而是调 §5 的 4 个曲线纯函数读写 `channels`。

### kind 3 — Exposure

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `exposureSettings.exposure` | number | -20 … 20 | 0 |
| `exposureSettings.offset` | number | -0.5 … 0.5 | 0 |
| `exposureSettings.gamma` | number | 0.01 … 9.99 | 1 |

### kind 4 — Gradient Map

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `gradientMapSettings.shadows.red` | number | 0 … 1 | 0 |
| `gradientMapSettings.shadows.green` | number | 0 … 1 | 0 |
| `gradientMapSettings.shadows.blue` | number | 0 … 1 | 0 |
| `gradientMapSettings.highlights.red` | number | 0 … 1 | 1 |
| `gradientMapSettings.highlights.green` | number | 0 … 1 | 1 |
| `gradientMapSettings.highlights.blue` | number | 0 … 1 | 1 |
| `gradientMapSettings.reversed` | bool | — | false |

### kind 5 — Grain

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `grainSettings.amount` | number | 0 … 100 | 25 |
| `grainSettings.size` | number | 0.5 … 20 | 1.5 |
| `grainSettings.roughness` | number | 0 … 100 | 50 |
| `grainSettings.seed` | number | 0 … 4294967295 | 0 |

### kind 6 — Add Noise（上游为 `LayerAdjustment` 顶层字段）

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `noiseAmount` | number | 0.1 … 400 | 10 |
| `noiseGaussian` | bool | — | false |
| `noiseMonochromatic` | bool | — | false |
| `noiseSeed` | number | 0 … 4294967295 | 0 |

### kind 7 — Gaussian Blur

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `blurRadius` | number | 0.1 … 250 | 10 |

### kind 8 — Motion Blur

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `motionAngle` | number | -90 … 90 | 0 |
| `motionDistance` | number | 1 … 2000 | 10 |

### kind 9 — Invert

无参数，`settings` 恒为 `{}`。

### kind 10 — Black & White

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `blackWhiteSettings.reds` | number | -200 … 300 | 40 |
| `blackWhiteSettings.yellows` | number | -200 … 300 | 60 |
| `blackWhiteSettings.greens` | number | -200 … 300 | 40 |
| `blackWhiteSettings.cyans` | number | -200 … 300 | 60 |
| `blackWhiteSettings.blues` | number | -200 … 300 | 20 |
| `blackWhiteSettings.magentas` | number | -200 … 300 | 80 |
| `blackWhiteSettings.tint` | bool | — | false |
| `blackWhiteSettings.tintHue` | number | 0 … 360 | 40 |
| `blackWhiteSettings.tintSaturation` | number | 0 … 100 | 20 |

### kind 11 — Color Balance

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `colorBalanceSettings.shadowCyanRed` | number | -100 … 100 | 0 |
| `colorBalanceSettings.shadowMagentaGreen` | number | -100 … 100 | 0 |
| `colorBalanceSettings.shadowYellowBlue` | number | -100 … 100 | 0 |
| `colorBalanceSettings.midCyanRed` | number | -100 … 100 | 0 |
| `colorBalanceSettings.midMagentaGreen` | number | -100 … 100 | 0 |
| `colorBalanceSettings.midYellowBlue` | number | -100 … 100 | 0 |
| `colorBalanceSettings.highlightCyanRed` | number | -100 … 100 | 0 |
| `colorBalanceSettings.highlightMagentaGreen` | number | -100 … 100 | 0 |
| `colorBalanceSettings.highlightYellowBlue` | number | -100 … 100 | 0 |
| `colorBalanceSettings.preserveLuminosity` | bool | — | true |

### kind 12 — Brightness/Contrast（本仓追加，`brightnessContrastSettings` 对象）

求值用 Photoshop 的经典 **259 公式**（`factor = 259(c+255) / 255(259−c)`，
`brightness`/`contrast` 的 ±100 先 ×2.55，`c` 夹到 ±254 避开 259 奇点），
**缺省值即恒等**：`brightness = contrast = 0` 时 `factor = 1`、`b = 0`，输出与输入
逐字节相同，因此破坏性入口对默认参数会走「无变化不进历史」的短路。

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `brightnessContrastSettings.brightness` | number | -100 … 100 | 0 |
| `brightnessContrastSettings.contrast` | number | -100 … 100 | 0 |

### kind 13 — Desaturate（本仓追加）

无参数，`settings` 恒为 `{}`（等价于红/绿/蓝混合权重相同的灰度转换）。
`param_count = 0` 且 `editable = false`；壳侧菜单点即落墨，不打开参数面板。

### kind 14 — Threshold（本仓追加，`thresholdSettings` 对象）

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `thresholdSettings.level` | number | 0 … 255 | 128 |

Rec.601 亮度（`0.299R + 0.587G + 0.114B`，先反预乘）≥ `level` 的像素写纯白，其余写纯黑
（输出只有 0 / 255 两个亮度；透明像素原样跳过）。

### kind 15 — Posterize（本仓追加，`posterizeSettings` 对象）

| 路径 | 类型 | 范围 | 默认 |
|------|------|------|------|
| `posterizeSettings.levels` | number | 2 … 255 | 4 |

每通道按 `levels` 级量化；`levels = 255` 即恒等。

## 4. 校验规则（ABI 写入时）

`comp_layer_add_adjustment` / `comp_layer_update_adjustment` 在写入前调用
`adjustment_validate_settings`，顺序如下：

1. `kind` 必须 < 16；
2. `settings` 必须是 JSON **对象**（空串按 `{}` 处理）；
3. 顶层键必须在白名单内（= §3 各路径的根段，加上 kind 0 允许的包裹层 `hsvSettings`）——
   多余键直接拒绝，避免打错键名却静默无效；
4. §3 列出的数值/布尔叶子若存在，必须类型正确且在范围内；
5. `levels` 必须是对象、`ranges` 必须是定长 4 项数组；`curves` 必须是对象，并做
   §3.2 的**结构**校验（通道数、点数、x 递增、端点锁死、值域）——变长控制点表达不了
   在静态参数表里，所以这部分校验在 `curves_settings.cpp`。

失败返回 `COMP_ERR_INVALID_ARGUMENT`，错误文本经 `comp_engine_last_error` 可取。
**读取路径不校验**：`.comp` 载入时对 `engineAdjustments` 原样保真，不因参数越界而拒绝工程文件。

## 5. ABI 接口

| 接口 | 用途 |
|------|------|
| `comp_layer_add_adjustment(doc, id, kind, settings_json)` | 追加调整（校验后写入，构成一个撤销步） |
| `comp_layer_update_adjustment(doc, id, settings_json)` | 替换该图层**首个**调整的 settings（kind 沿用） |
| `comp_layer_update_adjustment_at(doc, id, index, kind, settings_json)` | 替换第 `index` 个调整；面板改曲线时用它（**写入 = 一个撤销步**） |
| `comp_layer_adjustment_validate(kind, settings_json)` | 只校验不写入 |
| `comp_layer_adjustment_kind_name(kind, &name)` | 取规范名称 |
| `comp_layer_adjustment_param_count(kind, &count)` | 可编辑参数个数（面板据此决定是否渲染控件） |
| `comp_layer_adjustment_param_at(kind, i, &param)` | 取参数元数据（路径/标签/类型/范围/默认） |
| `comp_layer_adjustment_count(doc, id, &count)` | 图层上调整数量（面板列出既有调整） |
| `comp_layer_adjustment_at(doc, id, i, &kind, buf, cap, &size)` | 读取第 i 个调整（两段式字符串） |
| `comp_layer_curves_channel_count(&count)` | 通道数（恒 4：0 = RGB，1..3 = R/G/B） |
| `comp_layer_curves_point_count(settings, channel, &count)` | 该通道控制点数（缺省/非法 = 0，即恒等） |
| `comp_layer_curves_point_at(settings, channel, i, &x, &y)` | 读第 i 个控制点（越界 `COMP_ERR_NOT_FOUND`） |
| `comp_layer_curves_set_points(settings, channel, pts, n, buf, cap, &size)` | 改写该通道点列，返回新 settings（两段式；纯函数，**不开事务、不写文档**） |
| `comp_layer_remove_adjustment(doc, id, index, &changed)` | 删除第 `index` 个调整（**一个撤销步**；越界 `COMP_ERR_INVALID_ARGUMENT`） |
| `comp_layer_apply_adjustment(doc, id, kind, settings_json, &changed)` | **破坏性**一次落墨：把一条调整直接写进图层像素，不产生 `engineAdjustments` 条目（README §6.94） |

`comp_layer_apply_adjustment` 的 `kind` 白名单比调整层窄：只接受逐像素可破坏的
0/1/2/3/4/10/11/12/13/14/15；5..8（颗粒 / 噪点 / 高斯模糊 / 动感模糊）走
`comp_filter_apply`，9（反相）走 `comp_layer_invert`。settings 校验与
`comp_layer_add_adjustment` 完全相同（同一张表、同一套范围），所以面板构造出的
JSON 可以直接喂进来；结果与像素相同时不开事务、`changed = 0`。

**曲线的撤销从哪来**：上面 4 个曲线接口都是纯函数（只在 settings JSON 字符串上操作）。
面板「按下 → 拖动 → 松手」的把戏是：`comp_txn_begin` 一次，拖动过程中反复
`comp_layer_curves_set_points` + `comp_layer_update_adjustment_at` 做实时预览，松手时
`comp_txn_commit` —— 于是整段拖动只占**一个**撤销步；拖动没改动像素则 `comp_txn_abort`，
不留空步（`tests/test_adjustment.cpp` 的 `curves_*` 用例守着这两条）。
点列落盘后逐点不变（`curves_points_survive_comp_round_trip` 用 `.comp` 往返证明）。

## 6. 已知未覆盖

- **曲线只有 RGB/R/G/B 四通道**：上游的「单独编辑某通道同时看合成结果」已覆盖，
  但预设（如强对比 S 曲线库）、直方图背景、曲线求值以外的插值策略（单调三次 vs
  Catmull-Rom 可选）都没有——求值固定为 Fritsch–Carlson 形状保持 Hermite。
- **色阶通道条只是面板分页**：点通道条只改外壳当前页（`ShellState.adj_channel`），
  **不写回** settings 里的 `levels.channel`；求值始终把 `ranges[0]` 用在合成通道、
  `ranges[1..3]` 各管一个色彩通道（与上游 `Levels.swift` 一致）。所以「只调红通道」
  的正确做法是改 `ranges.1.*`，而不是先点红通道再改第一页。
- **删除调整已有接口**：`comp_layer_remove_adjustment`（面板 `Ctrl+Shift+D`）会从列表
  里移除一条并记一步撤销；但它不提供「停用调整层」（可见性开关）——要临时关掉只能删。
- **本仓追加的 4 种 kind 不与上游互操作**：12..15 的键名按上游 `*Settings` 风格自定，
  上游读不了、本仓读上游也不会遇到（上游没有这四个值）。
- **上游对象型键不覆盖**：上游自有的 adjustment 表现形式不在本轮改写范围（见 README §6.3）。
