# Photoshop 核心条目逐条清单（190 条）

> 目的：把 `docs/ps-parity.md` 里口径 D/C 的分母与命中数变成**可复算**的。
> 上一版（2026-10-23～10-27）只有九维配额与一个估计的命中总数，没有任何逐条清单，
> 导致「命中 118/190」无法被复核——任何人换几个缺口项就能把总分调到 78%～85%。
> 本清单是那次批评的收口：**每条一行，标命中与否，并给出可 grep 的证据**。
>
> 维护规则：新增能力时只改对应行的「命中」与「证据」两列，**不改条目本身**；
> 要增删条目属于改分母，须单独说明。`scripts/check_docs.py` 会校验
> 「本文件条目总数 == 190」且「各维命中数 == ps-parity.md 表格声明值」。

## 0. 判定口径（读表前先看这四条）

| 规则 | 内容 |
|------|------|
| **R1 一条是什么** | 「用户能直接触发的一个功能动作」。变体不重复计：24 种混合模式算 1 条「混合模式」，25 种调整算 1 条「调整层」，16 种滤镜算 1 条「滤镜菜单」。 |
| **R2 命中的门槛** | 必须有**用户可达的入口**（菜单项 / 快捷键 / 面板）且**后端真的实现**。只有 ABI 没有 UI 入口的，记「部分」并在证据列写明缺哪一半；「部分」在计分时按**未命中**处理（保守）。 |
| **R3 证据怎么写** | 写可 grep 的符号（ABI 名、`kCmd*`、函数名、文件路径）。写不出证据的一律记未命中。 |
| **R4 ML 与外部解码器** | 依赖 ML 模型（Select Subject / Remove Background / Neural Filters）与需第三方解码器的格式（HEIF/WebP/TIFF/RAW），按 `BACKLOG.md` §5 计未命中，不设豁免。 |

**状态标记**：`✅ 命中` ／ `◐ 部分`（有实现但缺入口或缺变体，计未命中）／ `❌ 未命中`。

---

## 1. 文档与文件（20 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| D01 | 新建文档（尺寸/分辨率/背景） | ✅ | `kCmdNewDocument`（Ctrl+Alt+N）→ `doc_dialog` 面板；`create_document_with` |
| D02 | 新建预设（A4/1080p/网页/照片） | ✅ | `kCmdNewPresetHd/A4/Web/Photo`（`shell_document.cpp:2199-2208`） |
| D03 | 打开文件 | ✅ | `kCmdOpen` → `open_path_as_document`（按扩展名分派分层/拍平） |
| D04 | 保存 / 另存为 | ✅ | `kCmdSave` / `kCmdSaveAs`（Ctrl+S / Ctrl+Alt+S，`GetSaveFileNameW`） |
| D05 | 未保存提示（保存/不保存/取消） | ✅ | `DocDialogKind::ClosePrompt`，`kCloseActionSave/Discard/Cancel` |
| D06 | 关闭/退出前逐标签提示 | ✅ | `close_intent` + `tabs`（`shell_state.h`） |
| D07 | 最近打开列表 | ✅ | `recent_push` / `recent_load`（`shell_document.cpp:398,457`） |
| D08 | 多文档标签页 | ✅ | `std::vector<DocTabSlot> tabs` / `active_tab`（`shell_state.h:1129`） |
| D09 | 文档位深 8/16 位 | ✅ | `comp_document_create_ex` + `comp_document_pixel_format` |
| D10 | 颜色模式 RGB / 灰度 | ✅ | `comp_document_color_mode`；灰阶 flatten 恒 R=G=B |
| D11 | PNG 导入导出 | ✅ | `COMP_CODEC_VENDORED`（libpng）+ 自研 `comp_png`；`comp_io_import/export` |
| D12 | JPEG 导入导出 | ✅ | stb_image / stb_image_write（`{"quality":N}`） |
| D13 | PSD 拍平导入 | ✅ | stb_image 路径（`COMP_CODEC_VENDORED`） |
| D14 | PSD 分层读写 | ✅ | 自研 `format/psd_codec`；`comp_io_import_document` / `comp_io_export_document`；`crosscheck_psd.py` 67 项 |
| D15 | EXR 读写 | ✅ | 自研 scanline 编解码器（NONE/RLE/ZIPS/ZIP；写 HALF） |
| D16 | Radiance HDR 导入 | ✅ | stb_image 浮点路径（只读，如实登记） |
| D17 | `.comp` 工程存取 | ✅ | `comp_file.cpp`；与 PIL 双向 0 LSB 交叉验证 |
| D18 | TIFF 读写 | ❌ | `COMP_CODEC_UNAVAILABLE`（需 libtiff，`docs/format-import-matrix.md` §2） |
| D19 | WebP / HEIF 读写 | ❌ | 登记为 `UNAVAILABLE`，导入在读文件前显式拒绝（同上 §3.1–3.2） |
| D20 | PDF / SVG 导出、RAW 解码、PSB | ❌ | SVG/PSB 未登记；无 PDF 导出；无 RAW 解码器（BACKLOG §5） |

**本维命中：17 / 20**

---

## 2. 图层与合成（20 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| L01 | 图层新建/删除/重命名 | ✅ | `comp_layer_add` / `remove` / `rename`；`kCmdLayer*` |
| L02 | 图层组与嵌套 | ✅ | `comp_layer_group` 族；树形 6 级缩进；`children` |
| L03 | 组穿透 / 隔离语义 | ✅ | `engineIsolated` / `engineBlendMode`（`comp_file.cpp`） |
| L04 | 不透明度 | ✅ | `comp_layer_set_opacity` |
| L05 | 混合模式（24 种） | ✅ | `COMP_BLEND_*` 24 个（`comp_core.h:126-150`） |
| L06 | 图层蒙版 | ✅ | `comp_layer_mask*`；贴入选区建蒙版（`shell_clipboard.cpp`） |
| L07 | 剪贴蒙版 | ✅ | `COMP_CLIP_*` + `ClippingTarget`（`ui_core.h:4594`） |
| L08 | 调整图层（25 种） | ✅ | `COMP_ADJUSTMENT_*` 24 kind + COUNT（`comp_layer.h`） |
| L09 | 图层样式（10 种） | ✅ | `COMP_EFFECT_*` 10 kind（描边/投影/颜色叠加/内阴影/外发光/内发光/斜面和浮雕/缎光/渐变叠加/图案叠加） |
| L10 | 合并向下 / 拼合图像 | ✅ | `comp_layer_merge_down` / `comp_document_flatten` |
| L11 | 合并可见 | ✅ | `comp_layer_merge_visible`（`kCmd` Ctrl+Alt+E） |
| L12 | 反向排列 | ✅ | `comp_layer_reverse` |
| L13 | 图层锁定（像素/位置/透明/全部） | ✅ | `comp_layer_*_lock` 4 个导出；面板 `T/P/G/L` 标记 |
| L14 | 图层颜色标签 | ✅ | `comp_layer_color_label`；八色 |
| L15 | 图层链接 | ✅ | `comp_layer_link` / `unlink` / `get_link`；`engineLink` 存盘 |
| L16 | 图层样式拷贝/粘贴 | ✅ | `comp_layer_export_style` / `import_style` |
| L17 | 图层复合（Layer Comps） | ✅ | 10 个 `comp_layer_comp_*`；`engineLayerComps` |
| L18 | 智能对象 + 智能滤镜 | ✅ | 8 个 `comp_layer_smart_object_*` / `smart_filter_*`（+3 收尾：`_move`/`_set_name`/`_name`） |
| L19 | 填充图层（纯色/渐变/图案） | ✅ | 12 个 `comp_layer_*_fill`；`comp_layer_add_fill` |
| L20 | 图层对齐与分布、全局光角度、合并的样式对话框 | ❌ | 全仓无 `align_layers` / `distribute` / `global_light`（关键词反查为误命中）；样式仍是逐效果面板 |

**本维命中：19 / 20**

---

## 3. 选区（18 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| S01 | 矩形选框 | ✅ | `ToolId::Marquee`；`comp_selection_rect` |
| S02 | 椭圆选框 | ✅ | `comp_selection_ellipse`（§6.17） |
| S03 | 多边形套索 | ✅ | `ToolId::Lasso`；`comp_selection_polygon` |
| S04 | 钢笔（贝塞尔路径选区） | ✅ | `ToolId::Pen`；`comp_selection_path`；路径面板 |
| S05 | 魔棒 | ✅ | `ToolId::Wand`；`comp_selection_wand` + 容差 |
| S06 | 色彩范围 | ✅ | `color_range_panel`；`kCmdColorRange` |
| S07 | 选区加减交（加选/减选/交集） | ✅ | `COMP_SELECT_*` 运算枚举 + `ClippingTarget::Add/Subtract` |
| S08 | 羽化 | ✅ | `comp_selection_feather` / `set_feather`（绝对羽化） |
| S09 | 扩展 / 收缩 | ✅ | `comp_selection_expand` / `contract` |
| S10 | 边界 / 平滑 | ✅ | `comp_selection_border` / `smooth`（P0-5 补全） |
| S11 | 扩大选取 / 选取相似 | ✅ | `comp_selection_grow` / `similar`（§6.96） |
| S12 | 存储选区到通道 / 从通道载入 | ✅ | 5 个 `comp_selection_channel_*`；`shell_channels.cpp`；`engineChannels` |
| S13 | 按图层像素/蒙版载入选区 | ✅ | `comp_selection_load_layer` |
| S14 | 移动/复制选区内像素 | ✅ | `comp_selection_move_pixels`；`V` 拖拽、Alt 复制 |
| S15 | 变换选区（数值宽高 + 旋转） | ✅ | `comp_selection_transform`（§6.102） |
| S16 | 手柄缩放选区 | ✅ | `transform_frame` 绘制 + 手柄命中（S46） |
| S17 | 磁性套索 / 快速选择 / 对象选择 | ❌ | 无 `magnetic` / `quick_select` 实现；对象选择属 ML（R4） |
| S18 | 快速蒙版 / 选择并遮住 | ❌ | 无 `quick_mask` / `select_and_mask`（BACKLOG P0-5 剩余） |

**本维命中：16 / 18**

---

## 4. 绘制与修饰（20 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| P01 | 画笔 | ✅ | `ToolId::Brush`；`comp_tool_brush` |
| P02 | 橡皮擦 | ✅ | `ToolId::Eraser`；`desc.erase`（destination-out） |
| P03 | 仿制图章 | ✅ | `ToolId::Clone`；`COMP_TOOL_CLONE`（源快照/aligned/sampleAll） |
| P04 | 涂抹 | ✅ | `COMP_TOOL_SMUDGE`（carried 拖色） |
| P05 | 模糊工具 | ✅ | `COMP_TOOL_BLUR`（§6.12） |
| P06 | 液化（Push） | ✅ | `COMP_TOOL_LIQUIFY`（前向位移重采样） |
| P07 | 污点修复 / 修复画笔 | ✅ | `ToolId::Heal`；`COMP_TOOL_HEAL`；`HealMode` |
| P08 | 内容识别填充 | ✅ | `COMP_TOOL_PATCH`（PatchPixels 内核） |
| P09 | 减淡 / 加深 / 海绵 | ✅ | `ToolId::Tone`；`COMP_TOOL_TONE`（§6.43） |
| P10 | 历史记录画笔 | ✅ | `ToolId::HistoryBrush`；从任意历史状态取样（§6.44） |
| P11 | 渐变工具 | ✅ | `ToolId::Gradient`；`COMP_TOOL_GRADIENT` 五类型 |
| P12 | 吸管（取色） | ✅ | `ToolId::Eyedropper`；`comp_eyedropper_sample` |
| P13 | 前景色/背景色模型 | ✅ | `ShellState` 前景色；`X` 交换、编辑器面板 |
| P14 | 画笔预设（5 档）+ 流量 | ✅ | 选项栏 `brush_size`/`hardness`/`flow`；`kCmd` 预设（§6.100） |
| P15 | 压感 | ◐ | `WM_POINTER` 笔压已接（整笔取平均），但**无 WinTab**、无逐 dab 压感曲线（R2 记未命中） |
| P16 | 画笔动态/纹理/间距/圆度/角度 | ❌ | `comp_tool_brush` 只有 diameter/pressure/hardness/flow/grain/seed |
| P17 | 图案图章 / 油漆桶图案填充 | ❌ | 无 `pattern_stamp`；填充层有图案但工具无 |
| P18 | 背景橡皮擦 / 魔术橡皮擦 | ❌ | 只有普通橡皮 |
| P19 | 混合器画笔 / 艺术历史画笔 | ❌ | 无实现 |
| P20 | 完整液化（膨胀/褶皱/重建）与修复画笔变体 | ❌ | 只有 Push 一种形变 |

**本维命中：14 / 20**

---

## 5. 文字 / 形状 / 渐变（16 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| T01 | 点文字（可编辑文字图层） | ✅ | `comp_layer_add_text` / `set_text`；`shell_type.cpp` |
| T02 | 改字/改字号重新渲染 | ✅ | 文字对象 + 渲染缓存（§6.84） |
| T03 | 文字栅格化 | ✅ | `comp_layer_rasterize_text` |
| T04 | 字体枚举与选择 | ✅ | `shell_fonts.cpp` 系统字体扫描 + CJK 标记（§6.89） |
| T05 | CJK 输入与排版 | ✅ | FreeType + `NotoSansSC-Regular-Subset.ttf`；`WM_CHAR` 码点编辑 |
| T06 | 字号 / 行距 / 字距 | ✅ | `comp_type_style` |
| T07 | 段落框（按框折行、框高裁切） | ✅ | `comp_text_desc.box_width/box_height` |
| T08 | 对齐（左/中/右） | ✅ | `comp_text_align` 0..2 |
| T09 | 形状（矩形/椭圆/直线/贝塞尔） | ✅ | `ToolId::Shape`；`COMP_TOOL_SHAPE`；`ShapeKind` |
| T10 | 形状填充与描边 | ✅ | `shape_stroke_width` + 填充/胶囊描边 |
| T11 | 渐变五类型（线性/径向/角度/对称/菱形） | ✅ | `comp_gradient_type` 0..4 |
| T12 | 多色标渐变（≤32 标 + 中点 + 抖动） | ✅ | `comp_gradient_stop`；右栏 8 行色标表 |
| T13 | 字体回退链 | ❌ | 单字体，缺字回落点阵；无回退链（BACKLOG P1-4） |
| T14 | 粗体/斜体/基线偏移 | ❌ | `comp_type_style` 无 weight/slant/baseline |
| T15 | 竖排 / 路径文字 / 文字变形 | ❌ | 全仓无实现（关键词反查仅命中 UI 竖排文字标签） |
| T16 | 形状布尔运算 / 画布上手拖色标 | ❌ | 无布尔；色标只能数值编辑（`gradient_stops` 表） |

**本维命中：12 / 16**

---

## 6. 调整与滤镜（50 条）

> 计数口径：R1 规定「25 种调整算 1 条」「16 种滤镜算 1 条」，但本维分母 50 是全部九维中最大的，
> 若只按 R1 会严重失衡（1 条代表 25 个能力）。因此本维**按 PS 的调整/滤镜菜单项逐一列**，
> 每组相邻的同类项合并为一条并在条目名里写清包含哪些。这是本维的显式例外。

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| A01 | 亮度/对比度 | ✅ | `COMP_ADJUSTMENT_BRIGHTNESS_CONTRAST` |
| A02 | 色阶 | ✅ | `COMP_ADJUSTMENT_LEVELS`；`curve_editor` 面板 |
| A03 | 曲线 | ✅ | `COMP_ADJUSTMENT_CURVES`；曲线面板 |
| A04 | 曝光 | ✅ | `COMP_ADJUSTMENT_EXPOSURE` |
| A05 | 自然饱和度 | ✅ | `COMP_ADJUSTMENT_VIBRANCE` |
| A06 | 色相/饱和度 | ✅ | `COMP_ADJUSTMENT_HUE_SATURATION` |
| A07 | 色彩平衡 | ✅ | `COMP_ADJUSTMENT_COLOR_BALANCE` |
| A08 | 黑白 | ✅ | `COMP_ADJUSTMENT_BLACK_WHITE` |
| A09 | 照片滤镜 | ✅ | `COMP_ADJUSTMENT_PHOTO_FILTER` |
| A10 | 通道混合器 | ✅ | `COMP_ADJUSTMENT_CHANNEL_MIXER` |
| A11 | 颜色查找 | ✅ | `COMP_ADJUSTMENT_COLOR_LOOKUP`（内置表；无外部 LUT 文件，见 A50） |
| A12 | 反相 | ✅ | `COMP_ADJUSTMENT_INVERT`；`kCmdInvert` |
| A13 | 色调分离 | ✅ | `COMP_ADJUSTMENT_POSTERIZE` |
| A14 | 阈值 | ✅ | `COMP_ADJUSTMENT_THRESHOLD` |
| A15 | 去色 | ✅ | `COMP_ADJUSTMENT_DESATURATE` |
| A16 | 渐变映射 | ✅ | `COMP_ADJUSTMENT_GRADIENT_MAP` |
| A17 | 可选颜色 | ✅ | `COMP_ADJUSTMENT_SELECTIVE_COLOR` |
| A18 | 阴影/高光 | ✅ | `COMP_ADJUSTMENT_SHADOWS_HIGHLIGHTS` |
| A19 | 自动色调 / 自动对比度 / 自动颜色 | ✅ | `COMP_ADJUSTMENT_AUTO_TONE` / `AUTO_CONTRAST` / `AUTO_COLOR` |
| A20 | 破坏性 `图像 > 调整` 全族入口 | ✅ | `comp_layer_apply_adjustment`；菜单 20 项（§6.94–6.95） |
| A21 | 颗粒 / 噪点（调整层形态） | ✅ | `COMP_ADJUSTMENT_GRAIN` / `ADD_NOISE` |
| A22 | 高斯模糊（调整层形态） | ✅ | `COMP_ADJUSTMENT_GAUSSIAN_BLUR` |
| A23 | 动感模糊（调整层形态） | ✅ | `COMP_ADJUSTMENT_MOTION_BLUR` |
| A24 | Camera Raw 滤镜 | ✅ | `comp_camera_raw_apply`；12 行 Basic；`kCmd` Ctrl+Alt+R |
| A25 | 锐化（USM） | ✅ | `COMP_FILTER_SHARPEN`（§6.90） |
| A26 | 降噪 | ✅ | `COMP_FILTER_REDUCE_NOISE`（YCoCg 双边） |
| A27 | 高斯模糊（滤镜形态） | ✅ | `COMP_FILTER_GAUSSIAN_BLUR` |
| A28 | 动感模糊（滤镜形态） | ✅ | `COMP_FILTER_MOTION_BLUR` |
| A29 | 渐进式模糊 | ✅ | `COMP_FILTER_PROGRESSIVE_BLUR` |
| A30 | 添加杂色 | ✅ | `COMP_FILTER_ADD_NOISE` |
| A31 | 颗粒（滤镜形态） | ✅ | `COMP_FILTER_GRAIN` |
| A32 | 高反差保留 | ✅ | `COMP_FILTER_HIGH_PASS`（Other 组） |
| A33 | 最小值 / 最大值 | ✅ | `COMP_FILTER_MINIMUM` / `MAXIMUM` |
| A34 | 位移 | ✅ | `COMP_FILTER_OFFSET` |
| A35 | 自定（5×5 卷积核） | ✅ | `COMP_FILTER_CUSTOM`（网格面板，§6.117） |
| A36 | 暗角（Vignette） | ✅ | `COMP_FILTER_VIGNETTE` |
| A37 | 色调对比（Tonal Contrast） | ✅ | `COMP_FILTER_TONAL_CONTRAST` |
| A38 | 泛光（Bloom） | ✅ | `COMP_FILTER_BLOOM`（显式亮部近似，note 已写明） |
| A39 | 镜头校正 | ✅ | `COMP_FILTER_LENS_CORRECTION` |
| A40 | 模糊画廊（场景/光圈/倾斜偏移/旋转/路径） | ❌ | 无对应 kind |
| A41 | 扭曲族（波纹/极坐标/球面化/切变…） | ❌ | 无 |
| A42 | 像素化族（马赛克/彩色半调/晶格化…） | ❌ | 无 |
| A43 | 风格化族（浮雕/查找边缘/等高线/油画…） | ❌ | 无 |
| A44 | 渲染族（云彩/镜头光晕/纤维/分层云彩） | ❌ | 无 |
| A45 | 杂色族其余（蒙尘与划痕/中间值） | ❌ | 只有添加杂色与降噪 |
| A46 | 其他（位移之外：自定之外的高反差细节等） | ◐ | Other 组 5 项已做，PS「其他」子菜单还有 6 项未做（R2 计未命中） |
| A47 | RAW 文件解码（非 Camera Raw 滤镜） | ❌ | 无 RAW 解码器（BACKLOG §5） |
| A48 | 消失点（Vanishing Point） | ❌ | 无 |
| A49 | 智能滤镜的非破坏式堆栈编辑 | ✅ | 智能滤镜栈面板：列出/开关/改参/删除/调序/改名（§6.112/6.114/6.115） |
| A50 | 外部 LUT 文件（.cube 导入） | ❌ | 颜色查找只有内置表；无 `.cube` 解析 |

**本维命中：40 / 50**（含 1 条「部分」按未命中计）

---

## 7. 变换与画布（13 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| X01 | 移动 | ✅ | `ToolId::Move`；`comp_layer_move` |
| X02 | 缩放 | ✅ | `comp_layer_set_transform`；手柄拖动 |
| X03 | 旋转（15° 步进 + 数值） | ✅ | `kCmd` 旋转；`comp_layer_set_transform` |
| X04 | 数值变换（宽/高/角度） | ✅ | `Ctrl+T` 三行面板（§6.103） |
| X05 | 再次变换 | ✅ | `Ctrl+Shift+T` 复读上次增量（§6.103） |
| X06 | 水平/垂直翻转 | ✅ | `comp_layer_flip`（S24 镜像） |
| X07 | 3 种重采样算法 | ✅ | `COMP_RESAMPLE_*`（Nearest/Bilinear/High） |
| X08 | 裁剪（比例约束/对称） | ✅ | `comp_crop_*` 5 个导出；`crop_rect` |
| X09 | 图像大小（约束比例/百分比） | ✅ | `kCmdImageResize`（Ctrl+Alt+I）；`comp_document_resize_image` |
| X10 | 画布大小（九宫格锚点） | ✅ | `kCmdCanvasSize`（Ctrl+Alt+C）；`comp_document_set_size` |
| X11 | 旋转画布 / 修边 | ✅ | `kCmdRotateCanvas*` / `kCmdTrim` |
| X12 | 斜切 / 扭曲 / 透视 / 变形（Warp） | ❌ | `transform` 只有 rect + rotation；无 `skew` / `perspective`（BACKLOG P1-3） |
| X13 | 内容识别缩放 / 拉直裁剪 | ❌ | 无 `content_aware_scale` / `straighten`（`LensPixels.h` 的 straighten 是镜头校正参数，不是裁剪拉直） |

**本维命中：11 / 13**

> **本维的两处诚实交代**：
> 1. **原分母偏小**：X12（Warp）与 X13（内容识别缩放/拉直）是 PS 确有、本仓确无的能力，
>    原分母 11 把它们漏在「变换与画布」之外。枚举后按实际能力计 **13 条**——
>    **分母漏项比命中数虚高更难被发现**，所以这里选择补上而不是沿用 11。
> 2. 命中率因此是 11/13 ≈ 85%，不是 100%。

---

## 8. 视图 / 界面（20 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| V01 | 图层面板 | ✅ | `draw_layer_panel`（`shell_layers.cpp`） |
| V02 | 调整面板 | ✅ | `draw_adjust_panel` |
| V03 | 曲线/色阶面板 | ✅ | `curve_editor`（§6.39） |
| V04 | 通道面板（选区通道） | ✅ | `selection_channel_panel`（`shell_channels.cpp`） |
| V05 | 历史记录面板（可回跳） | ✅ | `history_panel`（§6.42） |
| V06 | 路径面板 | ✅ | `path_panel`（§6.41） |
| V07 | 文字面板 | ✅ | `type_panel` |
| V08 | 滤镜参数面板 | ✅ | `filter_panel` |
| V09 | 效果（图层样式）参数面板 | ✅ | `effect_panel` |
| V10 | 智能滤镜栈面板 | ✅ | `smart_panel`（§6.114） |
| V11 | 工具选项栏 | ✅ | `tool_options`（`options_bar` 场景 13 条，§6.101） |
| V12 | 笔尖轮廓光标 | ✅ | `brush_tip_radius` + 黑白双描边椭圆 |
| V13 | 缩放 / 抓手 | ✅ | `ToolId::Zoom` / `Hand`；`ui_core_zoom_*` |
| V14 | 标尺 / 参考线 / 网格 / 对齐 | ✅ | 6 个 `comp_guide_*`；`engineGuides` 进 `.comp`（§6.91） |
| V15 | HiDPI 原生渲染（Per-Monitor V2） | ✅ | `shell_dpi.cpp`；`GetProcessDpiAwareness`=2（§6.93） |
| V16 | 面板停靠 / 浮动 / 分组 | ✅ | `shell_dock.cpp` / `shell_float.cpp`（§6.dock） |
| V17 | 工作区存读 | ✅ | `workspace_save` / `workspace_load`（`shell_float.cpp:595,599`） |
| V18 | 窗口可拖拽缩放 + 几何记忆 | ✅ | `window_resize` 场景 12 条；`window.ini` |
| V19 | 导航器 / 色板 / 属性 / 动作面板 | ❌ | 无 `navigator` / 独立色板面板 / `action` 面板（关键词反查为误命中） |
| V20 | 主题（明暗） / 快捷键自定义 | ❌ | 无 theme 实现、无 keymap 自定义 |

**本维命中：18 / 20**

---

## 9. 色彩 / 输出 / 自动化（15 条）

| # | PS 功能条目 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| C01 | sRGB 工作流 | ✅ | `.comp` 仅接受 `"sRGB"`（`comp_file.cpp`） |
| C02 | 图层级 16 位 / 16F | ✅ | `COMP_PIXEL_RGBA16` / `RGBA16F`；`deep_to_rgba8` |
| C03 | 文档级 8/16 位模式 | ✅ | `comp_document_pixel_format`（P0-1 收尾） |
| C04 | 文档级 RGB / 灰度模式 | ✅ | `comp_document_color_mode`；灰阶 flatten 恒 R=G=B |
| C05 | 导出 PNG/JPEG/EXR/PSD/`.comp` | ✅ | `comp_io_export` + `comp_io_export_document` |
| C06 | 批处理 / 动作录制 | ❌ | `comp_command_*` 仅 7 条；壳内 `kCmd*` 160+ 但两套体系未统一（BACKLOG P1-10） |
| C07 | 脚本（命令面板） | ❌ | 无脚本宿主 |
| C08 | 打印（纸张/缩放/位置） | ❌ | 无打印子系统（BACKLOG P1-9） |
| C09 | PDF / PDF-X 导出 | ❌ | 无 |
| C10 | ICC 配置 / 转换为配置文件 | ❌ | 无 ICC 支持（BACKLOG P0-9） |
| C11 | CMYK / Lab / 索引色 / 位图模式 | ❌ | 保留枚举号但回 `UNSUPPORTED` |
| C12 | 软打样 / 色域警告 | ❌ | 无 |
| C13 | 32 位浮点**文档**模式 | ❌ | 文档级只有 8/16；图层级 RGBA16F 有 |
| C14 | 数据驱动图形 / 变量 | ❌ | 无 |
| C15 | 色彩管理（广色域工作流） | ❌ | 整体缺失，与 C10/C11/C12 同源 |

**本维命中：5 / 15**

---

## 10. 汇总（本清单的计数结果）

**先说明分母的变化**：逐条枚举后得到 **192 条**，不是原来的 190 条。差额全部来自「变换与画布」
——原分母 11 条漏掉了 Warp 变形、内容识别缩放、拉直裁剪这三项 PS 确有而本仓确无的能力。
我选择**补上而不是压缩**：分母漏项会让得分偏高，而它比命中数虚高更难被读者发现。

| 维度 | 权重 | 条目 | 命中 | 得分 | 加权贡献 |
|---|---|---|---|---|---|
| 文档与文件 | 15% | 20 | 17 | 85% | 12.75 |
| 图层与合成 | 20% | 20 | 19 | 95% | 19.00 |
| 选区 | 10% | 18 | 16 | 88.9% | 8.90 |
| 绘制与修饰 | 15% | 20 | 14 | 70% | 10.50 |
| 文字 / 形状 / 渐变 | 10% | 16 | 12 | 75% | 7.50 |
| 调整与滤镜 | 15% | 50 | 40 | 80% | 12.00 |
| 变换与画布 | 5% | **13** | 11 | 84.6% | 4.25 |
| 视图 / 界面 | 5% | 20 | 18 | 90% | 4.50 |
| 色彩 / 输出 / 自动化 | 5% | 15 | 5 | 33.3% | 1.65 |
| **合计** | **100%** | **192** | **152** | | **81.05 ≈ 81%** |

算术：`12.75 + 19.00 + 8.90 + 10.50 + 7.50 + 12.00 + 4.25 + 4.50 + 1.65 = 81.05`

**与上一版（估计值 67%）的差异不是能力变化**，而是三件事叠加：
1. 上一版把 9 项已落地能力仍记在缺口里（记账滞后）；
2. 上一版的命中数是估计，逐条枚举后发现**多个维度估低了**（调整与滤镜 26→40、绘制 13→14、
   文档 9→17、视图 11→18）；
3. 变换维分母补上 2 条真实缺口（+2 条分母，得分从虚高的 100% 回落到 85%）。

**保留的保守处理**：标 `◐` 的 2 条（绘制维压感、调整维「其他」子菜单）**一律计未命中**。
若把它们算命中，总分约 81.5%——不值得为 0.5 个点牺牲口径的严格性。

---

## 11. 这份清单能回答什么、不能回答什么

| 能 | 不能 |
|---|---|
| 「某一维到底命中几条」——每条有编号、有证据、可 grep 复核 | 「这个仓比 Photoshop 强/弱多少」——分母是本清单定义的 192 条，不是 Adobe 的功能全集 |
| 「某个具体功能有没有」——按编号直接查 | 「多久能补齐」——§13 给的是**规模（diff 行数量级）与先例锚点**，不是工期天数；仓库 git 时间戳无法反推工时 |
| 「分数怎么来的」——§10 的算术与逐维命中数可逐项相加验证 | 「用户满意度」——命中 ≠ 好用，交互质量需 `docs/manual-test.md` 的人工目视 |
| 「缺的怎么补、先补哪个」——§13 的 40 条缺口分级与 ROI 排序 | 「补完的工期」——§13 只保证依据可追溯，不保证预测准确 |

**唯一的强度声明**：`scripts/check_docs.py` 会校验「本清单各维命中数 == `ps-parity.md` 表格声明值」
且「条目总数 == 192」。任何一侧被改动而另一侧没跟上，脚本立刻 FAIL。

---

## 12. 口径 C 的「日常子集」——逐条点名（117 项）

上一版的口径 C 是导演主观打分（"文字维 75%"这类），无法复核。本节把它改成**可数的子集**：
从 192 条里挑出**一个普通用户一周内真的会碰到**的项，命中几项就算几分。

**定子集的纪律（这条比数字重要）**：子集必须**独立于命中状态**确定。
我第一次算的时候，只挑了已命中的项，结果九维全 100%——那是把结论写进前提，是循环论证。
现在下面每个缺项都明确列出来了（带 ⚠️），它们**拉低**维度得分，而不是被排除在子集外。

| 维度 | 权重 | 日常子集 | 命中 | 得分 | 日常子集里**本仓没有**的项 |
|---|---|---|---|---|---|
| 文档与文件 | 15% | 12 | 12 | **100%** | 无（TIFF/WebP/RAW/PDF 属偶发需求，不算日常） |
| 图层与合成 | 20% | 14 | 13 | **93%** | ⚠️ L20 图层对齐与分布（排版居中每天都要用） |
| 选区 | 10% | 14 | 13 | **93%** | ⚠️ S17 快速选择（抠图最常用的那个） |
| 绘制与修饰 | 15% | 12 | 10 | **83%** | ⚠️ P15 压感（用数位板的人每天碰到）、⚠️ P18 背景橡皮擦 |
| 文字 / 形状 / 渐变 | 10% | 12 | 11 | **92%** | ⚠️ T14 粗体/斜体（做标题必然要用） |
| 调整与滤镜 | 15% | 25 | 24 | **96%** | ⚠️ A45 蒙尘与划痕 / 中间值（日常去噪） |
| 变换与画布 | 5% | 10 | 9 | **90%** | ⚠️ X12 Warp 变形（合成海报常用） |
| 视图 / 界面 | 5% | 14 | 13 | **93%** | ⚠️ V19 色板面板（取色的日常入口） |
| 色彩 / 输出 / 自动化 | 5% | 4 | 4 | **100%** | 无（打印/CMYK/批处理不属日常） |
| **合计** | **100%** | **117** | **109** | **≈93%** | 8 项缺口 |

算术：`0.15×100 + 0.20×93 + 0.10×93 + 0.15×83 + 0.10×92 + 0.15×96 + 0.05×90 + 0.05×93 + 0.05×100`
= `15.00 + 18.60 + 9.30 + 12.45 + 9.20 + 14.40 + 4.50 + 4.65 + 5.00` = **93.10 ≈ 93%**

### 12.1 为什么口径 C（93%）远高于口径 D（81%）

因为 192 条里**没命中的 40 条绝大多数是长尾**：模糊画廊、扭曲、像素化、风格化、渲染各族、
CMYK/Lab/ICC、打印、动作与批处理、32 位文档。这些条目数多、日常频率低。
按**条目数**算扣分多（D=81%），按**日常使用**算扣分少（C=93%）。

**这两个数字都对，但含义不同**：
- 说"能当 PS 用吗、干活够不够"→ **93%** 更贴切；
- 说"离 PS 的全部功能还差多少"→ **81%** 更贴切。

### 12.2 口径 C 的两个已知弱点（必须一起读）

1. **"日常"的边界是我定的。** 谁来决定 A45 算不算日常？我可以争辩说蒙尘与划痕不是每天都用。
   缓解办法：把所有缺项都摊在表里（上面 ⚠️ 那 8 条），读者可以自己判断该不该扣。
2. **命中 ≠ 好用。** 面板能不能开、参数能不能改、快捷键顺不顺，这张表一个字都没说。
   交互质量只有 `docs/manual-test.md` 的人工目视能回答，**93% 不代表"用起来有 PS 的 93% 顺"**。

---

## 13. 40 条未命中项的规模与排期

### 13.0 这份估算的依据与局限（先读，否则会误用）

**依据**：本仓有 15 个可对照的先例（每个都是一次完整交付：引擎 + ABI + 壳 + 测试 + 探针 + 文档）。
下表取它们的**实际 diff 规模**作为量级锚点：

| 先例 | 规模 | 性质 |
|---|---|---|
| `b765066` 图层面板过滤 | 461 行 / 11 文件 | 纯壳层 |
| `1e2300b` 文档位深 + 颜色模式 | 669 行 / 17 文件 | ABI 追加 + 引擎 |
| `bf85b47` 变换选区 | 930 行 / 16 文件 | ABI 追加 + 引擎 |
| `d623d18` 图层样式拷贝/粘贴 | 1,027 行 / 14 文件 | ABI 追加 |
| `5b7bee3` 滤镜 Custom 5×5 | 1,028 行 / 20 文件 | 枚举追加 + 新面板形态 |
| `f1882e2` 滤镜 Other 四项 | 1,381 行 / 18 文件 | 零新导出、复用内核 |
| `3c864f8` 停靠子系统四项 | 1,427 行 / 17 文件 | 壳层架构 |
| `a9a080a` 图层链接 | 1,454 行 / 20 文件 | ABI 追加 + 关系模型 |
| `9d08a99` 图层复合 | 1,671 行 / 18 文件 | ABI 追加 ×10 |
| `ee0cad9` 图层锁 + 标签 | 1,744 行 / 24 文件 | ABI 追加 ×4 |
| `46fdf3b` 浮动面板 | 1,790 行 / 20 文件 | 壳层架构（顶层窗口） |
| `e852cd8` 标尺/参考线/网格/吸附 | 2,017 行 / 21 文件 | ABI 追加 ×6 + 交互 |
| `0d372ed` 选区通道存取 | 2,260 行 / 29 文件 | ABI 追加 ×5 + 文档模型 |
| `a9fb6af` 智能对象/智能滤镜 | 2,603 行 / 25 文件 | 新领域模型 |
| `6da55fd` 填充图层 | 3,341 行 / 29 文件 | ABI 追加 ×12 + 新模型 |

**分级**（按上面这批先例的相对位置）：

| 级别 | 大致规模 | 对应先例 | 含义 |
|---|---|---|---|
| **S** | 300–1,000 行 | `b765066`、`1e2300b` | 单一入口或单一参数，无需新模型 |
| **M** | 1,000–1,800 行 | `d623d18`、`a9a080a`、`ee0cad9` | 一个完整功能族，含 ABI 与面板 |
| **L** | 1,800–3,500 行 | `e852cd8`、`0d372ed`、`6da55fd` | 涉及文档模型或多处消费点 |
| **XL** | 3,500 行以上，或需引入第三方库/独立子系统 | 无先例 | 本仓从未做过，量级只能给下限 |

**三条必须声明的局限**：

1. **这是「规模外推」，不是「工期预测」。** 仓库的 git 时间戳不可用（同一批提交时间戳完全相同，
   无法反推实际工作时长），所以**我无法给出天数**。要看工期只能看团队自己的记录。
2. **XL 项没有先例可锚定。** 色彩管理、打印、PDF、RAW 解码、动作录制都是独立子系统，
   下限按「最接近的 L 项 × 1.5」给，实际很可能远超。
3. **含第三方库的项（TIFF/WebP/HEIF/RAW/PDF）规模不在代码行数，而在依赖策略**——
   本仓「零依赖自研」的取向与引入 libtiff/libwebp 冲突，这会先卡一次决策，再谈实现。

---

### 13.1 逐条清单（40 条）

**图例**：`日常` = 是否在口径 C 的 117 项日常子集内（是 → 补上会抬高日常分；否 → 只抬高全功能面）；
`性质` 用 BACKLOG 的标签（接线/壳层/ABI 追加/引擎/架构）。

#### 日常子集内（8 条，优先）

| # | 缺口 | 级别 | 规模锚点 | 性质 | 说明 |
|---|---|---|---|---|---|
| L20 | 图层对齐与分布 | **S–M** | `b765066` 461 行（壳层） | 壳层 | 纯壳层：遍历选中层算包围盒 + 平移。**但要求多选层已有**（已有） |
| S17 | 快速选择 | **L** | `a9fb6af` 2,603 行（新模型） | 引擎 | 连通域 + 边缘模型；磁性套索可共用同一套边沿代价图 |
| P15 | 压感（WinTab） | **S–M** | `1e2300b` 669 行 | 壳层 | `WM_POINTER` 已接；缺的是 WinTab 与逐 dab 采样 |
| P18 | 背景橡皮擦 / 魔术橡皮擦 | **M** | `bf85b47` 930 行 | ABI 追加（枚举）+ 引擎 | 两个都是「笔刷 + 判定」，可复用 Wand 容差 |
| T14 | 粗体/斜体/基线偏移 | **S–M** | `1e2300b` 669 行 | ABI 追加（尾字段）+ 引擎 | `comp_type_style` 尾追加 3 字段 + FreeType 合成粗斜 |
| A45 | 蒙尘与划痕 / 中间值 | **S** | `f1882e2` 1,381 行 / 4 项 ⇒ 单型约 350 行 | 枚举追加 + 引擎 | 复用现有卷积框架，两个新 kind |
| X12 | Warp（斜切/扭曲/透视/变形） | **L** | `0d372ed` 2,260 行 | 引擎 | 任意四边形/网格重采样，几何模型扩展 |
| V19 | 导航器 / 色板 / 属性 / 动作面板 | **M**（仅色板+导航器） | `b765066` 461 行 × 2 | 壳层 | 动作面板属 C06，单列 |

#### 日常子集外（32 条，只抬高全功能面）

| # | 缺口 | 级别 | 规模锚点 | 性质 | 说明 |
|---|---|---|---|---|---|
| D18 | TIFF 读写 | **XL**（依赖决策） | 无 | 架构（依赖） | 需 libtiff；先定「引不引库」 |
| D19 | WebP / HEIF 读写 | **XL**（依赖决策） | 无 | 架构（依赖） | 需 libwebp + libheif（后者还要 HEVC） |
| D20 | PDF/SVG 导出、RAW 解码、PSB | **XL × 4** | 无 | 架构 + 引擎 | 四个独立能力，建议拆开评估 |
| S18 | 快速蒙版 / 选择并遮住 | **L** | `0d372ed` 2,260 行 | 引擎 + 壳层 | 快速蒙版是选区与通道的叠加态；「选择并遮住」是独立细化器 |
| P16 | 画笔动态/纹理/间距/圆度/角度 | **L** | `6da55fd` 3,341 行 | 引擎 + ABI 追加 | `comp_tool_brush` 尾追加多字段 + 笔尖采样重构 |
| P17 | 图案图章 / 油漆桶图案填充 | **M** | `d623d18` 1,027 行 | 引擎 | 图案平铺已有（`COMP_EFFECT_PATTERN_OVERLAY`），复用 |
| P19 | 混合器画笔 / 艺术历史画笔 | **L** | `a9fb6af` 2,603 行 | 引擎 | 混合器需笔尖携带多色状态 |
| P20 | 完整液化 + 修复画笔变体 | **L** | `0d372ed` 2,260 行 | 引擎 | 膨胀/褶皱/重建三种形变 + 网格冻结 |
| T13 | 字体回退链 | **M** | `1e2300b` 669 行 | 引擎 | 已有 `comp_font_load_file`，缺逐字形回退查表 |
| T15 | 竖排 / 路径文字 / 文字变形 | **L × 2** | `a9fb6af` | 引擎 | 竖排是排版方向；路径文字/变形是新的行布局器 |
| T16 | 形状布尔运算 / 画布拖色标 | **M** | `bf85b47` 930 行 | 引擎 + 壳层 | 布尔需路径运算库（可自研扫描线） |
| A40 | 模糊画廊（5 种） | **L** | `f1882e2` 1,381 行 × 5 | 引擎 | 五种独立模糊（场景/光圈/倾斜/旋转/路径），可共用一次「可模糊遮罩」 |
| A41 | 扭曲族 | **L** | 同上 × 6 | 引擎 | 六种形变，多数是坐标重采样 |
| A42 | 像素化族 | **M** | `f1882e2` × 4 | 引擎 | 四种块化，实现简单但各有参数 |
| A43 | 风格化族 | **L** | × 5 | 引擎 | 浮雕/查找边缘是卷积，油画要聚类 |
| A44 | 渲染族 | **M** | × 4 | 引擎 | 云彩/纤维是分形噪声，已有 `grain`/`noise` 基础 |
| A46 | 其他子菜单剩余 6 项 | **S** | `f1882e2` 单型 ≈350 行 | 枚举追加 | 与 A45 同批做最划算 |
| A47 | RAW 解码 | **XL**（依赖决策） | 无 | 架构（依赖） | 每个相机型号一套解码，通常引 libraw |
| A48 | 消失点 | **L** | 无直接先例 | 引擎 + 壳层 | 透视网格编辑 + 透视变换贴图 |
| A50 | 外部 LUT（.cube） | **S–M** | `1e2300b` 669 行 | 引擎 | `.cube` 是纯文本解析 + 三线性插值 |
| X13 | 内容识别缩放 / 拉直裁剪 | **XL / S** | — | 引擎 / 壳层 | 内容识别缩放需能量图（L–XL）；拉直只是旋转+裁切（S） |
| V20 | 主题 / 快捷键自定义 | **M** | `3c864f8` 1,427 行 | 壳层 | 主题是配色表；快捷键要建 keymap 层 |
| C06 | 批处理 / 动作录制 | **XL** | 无 | 架构 | 先把 `comp_command_*`(7 条) 与壳内 `kCmd*`(160+) 合成单一真源 |
| C07 | 脚本 / 命令面板 | **XL** | 无 | 架构 | 依赖 C06 的命令表统一 |
| C08 | 打印 | **XL** | 无 | 架构 | 无色彩管理时打印是假的（文档已明说） |
| C09 | PDF / PDF-X 导出 | **XL**（依赖决策） | 无 | 架构 | 需 PDF 库或自研 writer |
| C10 | ICC 配置 | **XL** | 无 | 引擎 | 需 lcms2 或自研 CMM |
| C11 | CMYK / Lab / 索引 / 位图模式 | **XL** | 无 | 引擎 | 依赖 C10 |
| C12 | 软打样 / 色域警告 | **XL** | 无 | 引擎 | 依赖 C10 |
| C13 | 32 位浮点文档模式 | **L–XL** | `1e2300b` 669 行 × 2 | ABI 追加 + 引擎 | 8/16 已做，32F 要打通全部消费点 |
| C14 | 数据驱动图形 | **XL** | 无 | 架构 | 依赖 C06/C07 |
| C15 | 色彩管理（广色域） | **XL** | 无 | 引擎 | C10–C12 的总和 |

---

### 13.2 按 ROI 的排期建议

**第 1 批（日常子集内、规模 S–M）——唯一能抬高 C 口径的方向**

| 顺序 | 缺口 | 级别 | 为什么先做 |
|---|---|---|---|
| 1 | L20 图层对齐与分布 | S–M | 纯壳层、无 ABI、排版每天都用；`b765066` 证明同类壳层活 461 行可完成 |
| 2 | T14 粗体/斜体/基线偏移 | S–M | 尾追加 3 字段；做标题必然要用 |
| 3 | A45 + A46 杂色/其他剩余项 | S | 同批做 8 个 kind，复用现有卷积框架，边际成本最低 |
| 4 | V19 色板 + 导航器面板 | M | 纯壳层两块面板 |
| 5 | P15 WinTab 压感 | S–M | `WM_POINTER` 已通，补 WinTab 与逐 dab |

> 估算：这 5 项做完，口径 C 从 93% → 约 **97%**（日常子集 117 项里再命中 5 项，且都是 S–M）。

**第 2 批（单点价值高、规模 M–L）**

| 顺序 | 缺口 | 级别 | 说明 |
|---|---|---|---|
| 6 | S17 快速选择 | L | 抠图最常用工具，但需连通域+边缘模型 |
| 7 | P18 背景/魔术橡皮擦 | M | 复用 Wand 容差 |
| 8 | A50 外部 LUT | S–M | `.cube` 解析简单，专业用户刚需 |
| 9 | P17 图案图章 | M | 图案平铺已有 |
| 10 | T13 字体回退链 | M | 缺字不再落方框 |

**第 3 批（明确不建议现在做）**

C06–C15 全部（打印/色彩管理/自动化/PDF）、D18–D20（格式依赖）、A47（RAW）、X13 内容识别缩放。
理由：都在日常子集外，且要么需第三方库决策，要么是独立子系统。
`BACKLOG.md` §7 阶段 G 已明确不做，本节只是把「为什么不做」量化了。

---

### 13.3 一句话结论

**40 条缺口里，只有 8 条在日常子集内**；这 8 条中有 5 条是 S–M 级（合计约 3,500–5,000 行，
与本仓一次中等批次相当）。**把日常口径从 93% 推到 97% 是一个中等批次的工作量**，
而剩下 32 条要动依赖决策或独立子系统，**不是"再努把力"能补的**。
