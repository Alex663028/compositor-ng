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

- `kind`：整数，见下表 §2，等于上游 `AdjustmentKind` 的 `CaseIterable` 序号。
- `settings`：**UTF-8 JSON 字符串**（不是嵌套对象），内容为 §3 的键名表结构；引擎原样保真存取，不做规范化。

## 2. kind 编码（冻结，禁止重排）

| kind | 名称（上游 rawValue） | 面板可逐参数编辑 |
|------|----------------------|------------------|
| 0 | `Hue/Saturation` | 是 |
| 1 | `Levels` | 否（需专用色阶编辑器） |
| 2 | `Curves` | 否（需专用曲线编辑器） |
| 3 | `Exposure` | 是 |
| 4 | `Gradient Map` | 是 |
| 5 | `Grain` | 是 |
| 6 | `Add Noise` | 是 |
| 7 | `Gaussian Blur` | 是 |
| 8 | `Motion Blur` | 是 |
| 9 | `Invert` | 无参数 |
| 10 | `Black & White` | 是 |
| 11 | `Color Balance` | 是 |

`COMP_ADJUSTMENT_KIND_COUNT = 12`；越界 kind 在 ABI 层直接返回 `COMP_ERR_INVALID_ARGUMENT`，
**不回落**到任何默认类型。

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

### kind 1 — Levels（`levels` 对象，浅层校验）

`levels.channel`（`"RGB"` / `"Red"` / `"Green"` / `"Blue"`）与
`levels.ranges`（4 个 `{ black, gamma, white, outputBlack, outputWhite }`，默认 `0, 1, 255, 0, 255`）。
结构复杂，面板不编辑；ABI 层只要求 `levels` 是对象。

### kind 2 — Curves（`curves` 对象，浅层校验）

面板不编辑；ABI 层只要求 `curves` 是对象。

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

## 4. 校验规则（ABI 写入时）

`comp_layer_add_adjustment` / `comp_layer_update_adjustment` 在写入前调用
`adjustment_validate_settings`，顺序如下：

1. `kind` 必须 < 12；
2. `settings` 必须是 JSON **对象**（空串按 `{}` 处理）；
3. 顶层键必须在白名单内（= §3 各路径的根段 + 允许但不可编辑的 `hsvSettings` / `levels` / `curves`）——
   多余键直接拒绝，避免打错键名却静默无效；
4. §3 列出的数值/布尔叶子若存在，必须类型正确且在范围内；
5. `levels` / `curves` 只做「是对象」的浅层校验。

失败返回 `COMP_ERR_INVALID_ARGUMENT`，错误文本经 `comp_engine_last_error` 可取。
**读取路径不校验**：`.comp` 载入时对 `engineAdjustments` 原样保真，不因参数越界而拒绝工程文件。

## 5. ABI 接口

| 接口 | 用途 |
|------|------|
| `comp_layer_add_adjustment(doc, id, kind, settings_json)` | 追加调整（校验后写入，构成一个撤销步） |
| `comp_layer_update_adjustment(doc, id, settings_json)` | 替换该图层**首个**调整的 settings（kind 沿用） |
| `comp_layer_adjustment_validate(kind, settings_json)` | 只校验不写入 |
| `comp_layer_adjustment_kind_name(kind, &name)` | 取规范名称 |
| `comp_layer_adjustment_param_count(kind, &count)` | 可编辑参数个数（面板据此决定是否渲染控件） |
| `comp_layer_adjustment_param_at(kind, i, &param)` | 取参数元数据（路径/标签/类型/范围/默认） |
| `comp_layer_adjustment_count(doc, id, &count)` | 图层上调整数量（面板列出既有调整） |
| `comp_layer_adjustment_at(doc, id, i, &kind, buf, cap, &size)` | 读取第 i 个调整（两段式字符串） |

## 6. 已知未覆盖

- **Levels / Curves 无面板编辑**：需要专用编辑器（色阶直方图、曲线控制点），当前仅保真存取并做浅层校验。
- **无删除调整接口**：暂未提供 `comp_layer_remove_adjustment`，面板不提供删除按钮。
- **上游对象型键不覆盖**：上游自有的 adjustment 表现形式不在本轮改写范围（见 README §6.3）。
