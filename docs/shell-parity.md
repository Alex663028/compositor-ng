# 两壳一致性（shell parity）

compositor-ng 是「同引擎、两壳」结构：

- `shells/windows/` —— Windows 外壳（Win32 + GDI，`compositor_shell.exe`）
- `shells/ohos/`    —— 鸿蒙外壳（NAPI + ArkTS + XComponent，`libcompositor.so`）

两壳都**只 include `compositor/comp_*.h`**，不做像素处理、不拼 JSON。图像能力全部经同一组
`comp_*` C ABI 下沉到引擎核。因此「两壳是否表现一致」可归结为一件事：

> 同一串 ABI 调用，在两壳下必须得到同一语义结果；两壳共用的数值常量必须冻结。

本文记录该契约的**能力矩阵**与**共享回归护栏**，并把「已达成 / 有边界 / 未达成」三类如实标注。

---

## 1. 能力矩阵

| 能力 | Windows 壳 | 鸿蒙壳 | 共享 ABI 入口 | 状态 |
|------|-----------|--------|---------------|------|
| 引擎启动 / 释放 | `startup(AUTO)` | `startup(AUTO)` | `comp_engine_create` / `comp_engine_destroy` | 已达成 |
| 后端查询 | `backend_name()` | `backendName()` | `comp_engine_backend` | 已达成 |
| 离屏合成展平 | `--selftest` / 面板预览 | `flatten(w,h)` | `comp_render_flatten` | 已达成 |
| 图层增删改查 | 图层面板（增/删/复制/改名/建组/多选/拖行重排/拖进拖出组/显隐/不透明度/混合/变换/蒙版/剪贴/隔离/采样/效果） | （未接 UI） | `comp_layer_*` | **已达成**（P0-1，见 §5.12）：`add`/`remove`/`duplicate`/`get_name`/`set_name`/`move`/`reorder`/`set_blend`/`set_transform`/`set_mask`/`set_clipping_source`/`set_isolated`/`set_sampling`/`set_flip`/`set_effect`/`merge_down` 均有壳侧调用点；鸿蒙壳仍无面板 |
| 调整图层参数编辑 | 调整面板 | （未接 UI） | 调整 ABI 全套 | 已达成（鸿蒙无面板） |
| 调整控件元数据 | `refresh_adjustments` | 预留 | `comp_layer_adjustment_param_at` 等 | 已达成 |
| 调整滑杆 + 曲线/色阶编辑器（P0-3） | 调整面板：拖动滑杆实时预览；曲线加点/拖点/删点；色阶 20 参数分页 | （未接 UI） | `comp_layer_update_adjustment_at` + `comp_layer_curves_*` | **已达成**（P0-3，见 §5.14）：一次拖动 = **一个**撤销步，空拖动不产生撤销步 |
| 选区补齐第一批（P0-4） | 「选择(&S)」菜单：扩展…/收缩…/按图层像素载入选区 | （未接 UI） | `comp_selection_modify`(EXPAND/CONTRACT) / `comp_selection_load_layer`(LOAD_PIXELS) | **已达成**（§5.15）：三条入口接线完成 |
| 色彩范围对话框（P0-4 第二批） | 「选择 → 色彩范围…」+ 右侧面板（色板 / 容差 / 反相 / 加减取样，事务预览） | （未接 UI） | `comp_selection_apply_color_range` / `comp_eyedropper_sample` / `comp_txn_*` | **已达成**（§5.16）：预览不落步，回车 = 一步撤销，Esc 回到打开前 |
| 路径面板最小版（P0-4 可选） | 「选择 → 路径面板…」+ 右侧列表（已提交钢笔路径 / ↑↓ 选行 / 回车或双击转选区 / Del 删除） | （未接 UI） | 无（纯外壳；引擎无路径实体，转选区走 `comp_selection_apply_path`） | **已达成**（§5.17）：转选区 = 一步撤销；入列与删除只动壳侧列表，不落撤销步 |
| 历史记录面板（P1） | 「编辑 → 历史记录…」+ 右侧列表（状态 0..N / 点行、↑↓、Home/End 回跳） | （未接 UI） | `comp_document_history_step_count` / `_step_label` / `_jump`（ABI 尾部追加，135 → 138） | **已达成**（§5.18）：回跳不新增撤销步；高亮 = 引擎 `current_index`，壳侧无第二份真相 |
| 滤镜菜单 / 参数面板 | 「滤镜(&F)」动态菜单 + 右侧参数面板 | （未接 UI） | `comp_filter_apply` | 已达成（鸿蒙无面板） |
| 智能滤镜栈面板（P1-2 第九批） | 「图层 → 智能对象 → 智能滤镜面板…」+ 右侧列表（↑↓ 选、空格开关、回车/双击改参数、Del 删除、滚轮翻页） | （未接 UI） | `comp_layer_smart_filter_count` / `_at` / `_set` / `_remove`（本批无新增导出） | **已达成（Windows）**（§5.74）：开关 / 删除各一步撤销；改参数复用滤镜面板，参数与开关都没变时不落撤销步；面板每帧现问引擎，撤销后自动同步 |
| 智能滤镜调序与改名（P1-2 收尾 ⑩） | 栈面板内 Alt+↑/↓、拖动行重排、F2 / 菜单重命名、行显示自定义名 | （未接 UI） | `comp_layer_smart_filter_move` / `_set_name` / `_name`（尾部追加，208 → 211） | **已达成（Windows）**（§5.75）：调序整栈重求值 = 一步撤销，锁像素的层拒绝调序；改名是纯元数据（不重求值、锁像素也能改、同值不落步），空名回退类型名 |
| 滤镜 Other 组（P1-5，2026-10-25） | 「滤镜」菜单尾部四项 + 同一套参数面板 | （未接 UI） | `comp_filter_apply`（零新增导出，只追加 4 个 kind） | **已达成（Windows）**（§5.3）：菜单与面板由 `filter_table()` 自动派生，`shell_window.cpp` 零改动；命令号 1211..1214，面板动作仍 1220..1222 |
| 滤镜 Custom（P1-5 收尾，2026-10-25） | 「滤镜 → 其他 → 自定…」：5×5 核表 + 除数 + 偏移，面板第一次有网格布局 | （未接 UI） | `comp_filter_apply`（零新增导出，只追加 kind 15） | **已达成（Windows）**（§5.3）：`FilterSpec::columns / grid_count` 只有 Custom 填 5/25，方向键在核内移动；命令号 1215，其余滤镜的绘制与键位未变 |
| 图层效果参数面板（P0-2） | 「图层效果」菜单 + 右侧参数面板（`Ctrl+Shift+E`） | （未接 UI） | `comp_layer_effect_param_count` / `_param_at` / `_read_value` / `_build_settings` | **已达成**（P0-2，见 §5.13）：标签/量程/缺省值全部来自 ABI 元数据表，壳不再自持任何一份数值 |
| Blur 修饰工具 | 画布折线 → 一次 `COMP_TOOL_BLUR` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板） |
| 减淡 / 加深 / 海绵（P1） | `O` 循环 8 档，画布折线 → 一次 `COMP_TOOL_DODGE` / `_BURN` / `_SPONGE` | （未接 UI） | `comp_tool_apply` | **已达成**（§5.19）：`Alt+[ / ]` 调强度，受选区约束 |
| 历史记录画笔（P1） | `Y` 工具 + 历史面板取样（Shift+点行），画布折线 → 一次 `COMP_TOOL_HISTORY_BRUSH` | （未接 UI） | `comp_tool_apply`（kind 尾部追加，导出数仍 138） | **已达成**（§5.20）：取样不回跳；一笔 = 一个撤销步；`Alt+[ / ]` 调不透明度 |
| 渐变工具（S9） | 画布拖拽两端点 → 一次 `COMP_TOOL_GRADIENT` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；线性/径向、Shift 反向） |
| 形状工具（S10） | 画布拖拽对角点/两端点 → 一次 `COMP_TOOL_SHAPE` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；矩形/椭圆/直线，Shift 正方、Alt 从中心） |
| 字体加载（P5-1/P5-3） | 启动加载 `resources/fonts/NotoSansSC-Regular-Subset.ttf`，面板第四行 `Typeface` 切换 | （未接 UI，也未打包字体） | `comp_font_load_file` / `_load_memory` / `_release` / `_get_metrics` / `_name`（尾部追加，140 → 145） | **已达成（Windows）**：字体不可用时如实退回 5×7 点阵并提示；鸿蒙壳还没有文字工具，故无「缺字体」可回落（见 §5.45 末段） |
| 文字工具（S11，P5-3 起含 CJK） | 点击落插入点 + 键入（`WM_CHAR`/输入法，UTF-8）→ 一次 `COMP_TOOL_TYPE` | （未接 UI） | `comp_tool_apply` + `comp_tool_op_desc.type_font`（尾部追加） | **已达成**（§5.45）：随仓字体轨道支持 CJK，面板 `Typeface` 可切回内置 5×7 点阵（ASCII-only，逐字节不变） |
| 编解码能力查询 | 导入/导出菜单 | 预留 | `comp_io_codec_support` | 已达成 |
| PNG 导入/导出 | 可用 | 可用 | `comp_io_import` / `comp_io_export` | 已达成（libpng+zlib） |
| JPEG 导入/导出 | 可用（有损） | 可用 | 同上 | 已达成（stb） |
| PSD 只读导入 | 可用 | 可用 | `comp_io_import` | 已达成（stb） |
| 离屏上屏（GPU 协同） | AUTO → D3D12 离屏计算 + 回读；**HWND 直呈已落地（S16，`caps().window_present`）** | `presentSurface` 返回 `UNSUPPORTED` | `comp_render_document`(kind=HWND/HARMONY_WINDOW) / `comp_render_present` / `comp_render_flatten` | **部分达成**：Windows 的设备侧离屏（S13）与交换链直呈（S16）都已落地；鸿蒙设备侧交换链未落地，窗口表面如实拒绝 |
| 离屏 present/wait | 无操作成功 | 无操作成功 | `comp_render_present` / `comp_render_wait` | 已达成 |
| 工具条 / 画布导航 | 20 工具按钮、滚轮缩放、中键/空格平移 | （未接 UI） | 无（纯外壳，不跨 ABI） | **仅 Windows**：鸿蒙壳尚无对应面板，不影响共享语义 |
| 标尺 / 参考线 / 网格 / 对齐（ROI #3） | 视图菜单五项 + `Ctrl+R` / `Ctrl+;` / `Ctrl+Shift+;` / `Ctrl+'`；从标尺拖出、Move 工具拖动、拖回标尺删除，整次拖动一步撤销 | （未接 UI） | `comp_guide_add` / `_move` / `_remove` / `_count` / `_get` / `_clear`（尾部追加，152 → 158） | **已达成（Windows）**（§5.54）：参考线随 `.comp` 的 `engineGuides` 往返（12.5 原样），吸附只改落点不改像素 |
| 破坏性 `图像 > 调整`（ROI #8 + P1-6） | 子菜单二十项（`kCmdImageAdjustBase` + kind）+ 破坏性面板态（`Ctrl+Shift+U` 落墨、Esc 退出） | （未接 UI） | `comp_layer_apply_adjustment`（尾追加，163 → 164）+ kind 12…24 | **已达成（Windows）**（§5.57）：参数与调整层面板同源；零参数 kind（去色/自动三档）点即落墨；一步撤销、无变化不进历史 |

「有边界」表示：接口已完整接线，但因缺少设备侧上屏实现，两壳统一**如实返回
`COMP_ERR_UNSUPPORTED`**，不伪造成功。Windows 侧已在 S16 补上 D3D12 交换链直呈
（§5.10），鸿蒙侧仍需等设备侧交换链落地——调用点无需改动。

---

## 2. 冻结的 ABI 数值常量

两壳都以数值（而非符号）跨语言传递下列枚举；任一漂移即两壳行为分叉。这些常量由
`tests/test_shell_contract.cpp::shell_contract_abi_constants_are_frozen` 锁死：

| 组 | 常量 | 值 |
|----|------|----|
| 像素格式 | `COMP_PIXEL_RGBA8` | 0 |
| 渲染后端 | `AUTO/METAL/D3D12/VULKAN/CPU` | 0/1/2/3/4 |
| 表面种类 | `HWND/METAL_LAYER/HARMONY_WINDOW/CPU_BUFFER` | 0/1/2/3 |
| 栅格格式 | `UNKNOWN/PNG/JPEG/…/PSD` | 0/1/2/…/7 |
| 结果码 | `COMP_OK` / `COMP_ERR_INVALID_ARGUMENT` / `COMP_ERR_UNSUPPORTED` | 0/1/4 |
| 编解码状态 | `BUILTIN/UNAVAILABLE/VENDORED` | 0/1/2 |
| 调整 kind | `HUE_SATURATION…COLOR_BALANCE` + 本仓追加的 `BRIGHTNESS_CONTRAST/DESATURATE/THRESHOLD/POSTERIZE` | 0…11 / 12…15（`COMP_ADJUSTMENT_KIND_COUNT` = 16） |

调整 kind 的计数另由 `tests/test_adjustment.cpp::adjustment_destructive_kind_set_is_explicit`
锁定（`test_shell_contract.cpp` 只锁 kind=3 的参数元数据）；新增 kind 只能追加在 15 之后。

任何一处需要变更，先改 README §5 冻结表与本文，再同步两壳。

---

## 3. 共享回归护栏

`tests/test_shell_contract.cpp`（零依赖，随引擎回归一起跑）覆盖 6 个用例：

| 用例 | 锁定的契约 |
|------|-----------|
| `shell_contract_abi_constants_are_frozen` | 上表全部数值常量 |
| `shell_contract_engine_lifecycle_is_shared` | `create(AUTO)`→`backend()` 返回合法枚举；`create(CPU)` 原样生效（两壳回退路径依赖） |
| `shell_contract_flatten_semantics_is_shared` | 不透明源 + Normal 叠透明底，flatten 保源色（±1 LSB）、alpha=255 |
| `shell_contract_window_surfaces_report_unsupported` | **CPU 后端**对 HWND / HARMONY_WINDOW 均返回 `UNSUPPORTED`（不支持时不许假装）；离屏 present/wait 无操作成功。D3D12 的 HWND 直呈另见 §5.10 |
| `shell_contract_codec_capability_query_is_shared` | PNG 可用、JPEG/PSD 为 VENDORED、TIFF 为 UNAVAILABLE，且 note 非空 |
| `shell_contract_adjustment_control_metadata_is_shared` | kind=3=Exposure、3 个参数、首个路径 `exposureSettings.exposure`；越界显式失败 |

运行方式：`python scripts/build.py`（回归内含），或 `build/compositor_tests.exe`。
任意一壳所依赖的常量/语义被改动，这里立即失败。

---

## 4. 边界与未达成项（如实记录）

1. **设备侧上屏**：D3D12 **离屏渲染已真实走设备**（S13：上传 → 计算 → 回读；子集内
   文档由 `last_render_offloaded()` 证明，见 `tests/test_rhi.cpp`），**窗口交换链直呈也已
   落地**（S16：`caps().window_present`，`HWND` 表面写 DXGI flip-model 后备缓冲，见 §5.10）；
   但交互壳主画布仍用 CPU 展平 + GDI（覆盖层冲突，§5.10 边界），鸿蒙无在线设备
   （`hdc list targets` 为空）、Vulkan 交换链未落地，故**鸿蒙侧**窗口表面仍 `UNSUPPORTED`。
2. **鸿蒙 UI 面板**：鸿蒙壳目前只有 `startup/shutdown/backendName/flatten/surfaceInfo/presentSurface`
   六个桥函数，尚未接图层/调整面板 UI；能力矩阵中标注为「未接 UI」，非能力缺失。
3. **鸿蒙 HAP 真机验证**：交叉编译（aarch64 / x86_64）均可产出 `.so`，但无设备可上机。

以上三项均属**环境边界**，不涉及两壳语义分叉；共享回归护栏已把可离线验证的部分全部锁死。

---

## 5. Windows 壳 UI 地基（2026-10-18）

上游 `Compositor` 只有 macOS 应用层，**没有 Windows 版可供参照**，故 Windows 壳的
编辑器界面是自建的。为让 UI 数学可回归，平台无关部分被抽到
`shells/windows/src/ui_core.h`（header-only，不 include Windows 头、不 include `engine/` 头、
不分配内存），由 `tests/test_ui_core.cpp` 的 32 例覆盖；`main.cpp` 只做
「消息 → ui_core 调用 → 绘制」。**绘制与命中测试共用同一套几何函数**，这是本层的第一原则。

### 5.1 冻结的布局常量

| 常量 | 值 | 说明 |
|------|----|------|
| `Layout::kToolbarHeight` | 46 | 顶部工具条 |
| `Layout::kStatusHeight` | 26 | 底部状态栏 |
| `Layout::kLayerPanelWidth` | 248 | 左侧图层面板 |
| `Layout::kAdjustPanelWidth` | 330 | 右侧调整面板 |
| `Layout::kMinCanvasWidth` | 180 | 画布保底宽度，不足则先收右面板、再收左面板 |
| `kToolButtonSize` / `kToolButtonGap` / `kToolButtonLeft` | 32 / 6 / 10 | 工具条按钮几何 |
| `kLayerHeaderHeight` / `kLayerRowHeight` | 30 / 22 | 图层面板表头与行高 |
| `kFilterRowsTop` / `kFilterRowHeight` | 54 / 20 | 滤镜参数面板行几何（绘制与命中共用） |
| `kMinBlurRadius` / `kMaxBlurRadius` | 0.5 / 50 | 与 `comp_tool_blur.radius` 同量程 |
| `CanvasView::kMinZoom` / `kMaxZoom` | 0.02 / 32.0 | 缩放夹取范围 |

不变量（测试钉死）：任意宽度 × 高度下所有矩形非负、互不重叠、三块横向拼接**正好**铺满
客户区；`pan = 0` 时文档中心恒在画布中心；以光标为锚缩放前后，光标下的文档像素不漂移。

### 5.2 工具与快捷键（对齐上游 `UI/KeyboardShortcuts.swift`）

`A` Select · `V` Move/Transform · `M` Marquee · `L` Lasso · `W` Magic Wand · `C` Crop ·
`I` Eyedropper · `B` Brush · `E` Eraser · `J` Spot Healing · `S` Clone Stamp ·
`R` Blur/Smudge/Liquify · `O` Dodge/Burn/Sponge · `G` Gradient · `U` Shape · `T` Type · `H` Hand · `Z` Zoom

- 大写字母与小写等价（`WM_KEYDOWN` 不区分大小写）；
- 未占用键显式返回 `ToolId::Count`，**不静默回落到第一个工具**；
- 默认键不重复；工具条、工具菜单、快捷键共用 `tool_table` 一张表。

为对齐上游而挤出的键位：图层可见性 `V` → `Ctrl+Shift+V`；图层不透明度 `[` `]` →
`Ctrl+[` / `Ctrl+]`（`[` `]` 改为笔刷直径、`Shift+[` `]` 为硬度）；新建/打开/保存改
`Ctrl+N` / `Ctrl+O` / `Ctrl+S`。图层面板命令（P0-1）：`Ctrl+G` 新建组、`Ctrl+J` 复制、
`Del` 删除、`F2`（或双击行）重命名、`Shift+O` 精确不透明度键入；Ctrl/Shift 点行多选，
批量删除/复制共用一次撤销（§5.12）。

### 5.3 滤镜面板与 Blur 工具（2026-10-18 追加）

面板字段与量程全部来自 `ui_core.h` 的 `filter_table()`，逐项对齐
[`filters.md`](filters.md) §3；`FilterId` 的数值由 `main.cpp` 的 `static_assert`
与 ABI `comp_filter_kind` 锁死。几点容易踩的口径：

- **ABI 零初始化 = 恒等，面板缺省 ≠ 恒等**：`comp_filter_desc{}` 的 amount 是 0
  （不动像素）；面板在选中滤镜时用上游默认值（如 Vignette midpoint 50），
  让「打开面板」就有可见效果。两个缺省各管一层，不互相冒充。
- **一次调用，不做 note 长度探测**：`comp_filter_apply` 先执行、后写 note，
  探长度会把滤镜应用两遍；壳用固定 512 字节缓冲一次调用。
- **面板夹取，ABI 拒绝**：键入手输先用 `clamp_filter_param` 夹到量程内，
  ABI 侧仍是越界拒绝（不静默 clamp）。
- **十六个滤镜、一张表**：菜单、面板、值映射都从 `filter_table()` 派生
  （Vignette / Tonal Contrast / Bloom / Lens Correction / Gaussian Blur /
  Add Noise / Grain / Motion Blur / Progressive Blur / Sharpen / Reduce Noise /
  High Pass / Minimum / Maximum / Offset / Custom）。滤镜序号涨到 16 个后菜单号占
  1200..1215，面板的应用 / 重置 / 关闭动作仍在 1220..1222（§6.90 搬家时留出的
  1211..1219 余量装下了 Other 组、Custom 用 1215；见 `shell_state.h`、
  `filters.md` §8）。布尔项（`Preserve` /
  `Gaussian` / `Mono` / `Radial` / `Inverted`）在面板上是
  0/1 行，`build_filter_desc` 用 `>= 0.5` 转成 ABI 的 int32；`Seed` 用
  `llround` 转 `uint32`（面板量程 0..4294967295，正好是 uint32 全域）。
  Offset 的 `Wrap 0/1/2` 同样是数值行，`build_filter_desc` 用 `llround` 转 int32
  （越界值由 ABI 拒绝：`0/1/2` 之外没有「静默当 0」的余地）。
- **Custom 是唯一的网格面板**：`FilterSpec::columns / grid_count` 只有它填 `5/25`，
  前 25 行按 5 列铺成核表（格子只画数值，参数名在状态行），第 26/27 行
  （`Scale` / `Offset`）仍整宽。面板开着时方向键先给网格（`filter_focus_grid`）：
  上下按行步进、左右在行内走、下移越过最后一行核落到 `Scale`；单列滤镜不进这段
  逻辑，键位与绘制逐像素未变。核不随预览缩放，`Scale = 0` 按 1 处理（见
  `filters.md` §3.16）。
- **Gaussian Blur 的面板量程含 0**（恒等）；步进从 0 直达 1，键入手输
  `(0, 0.1)` 里的值会被 ABI 拒绝——这是有意的严格口径，不是壳的遗漏。
  Motion Blur 的 `Distance` 同理：0 = 恒等，步进从 0 到 1，`(0, 1)` 被拒。
- **Progressive Blur 的轴在画布上拖**：面板只有 From / To / Radial / Inverted
  四行；渐变轴端点来自画布拖拽（`progressive_axis_begin/extend/end`），
  拖拽中画实线、已提交的轴画虚线，径向档另画半径圆。面板打开期间画布拖拽
  优先给轴而不是笔迹，手型 / 空格平移仍优先；`WM_CAPTURECHANGED` / 失焦经
  `progressive_axis_cancel` 只丢弃本次拖拽（不动已提交的轴）。
  `progressive_axis_set` 才置 `explicit_axis = 1`，否则交给域层的缺省中线；
  新建 / 打开文档会作废旧轴（端点是旧文档坐标）。轴拖拽**不进撤销栈**，
  回车应用才是那一步 `Filter: Progressive Blur`。
- **`Alt+[` / `Alt+]` 调模糊半径**：`[`/`]` 已属笔刷直径、`Ctrl+[`/`]` 已属
  不透明度；Blur 半径 0.5..50。
- **笔迹去抖**：点间距小于 `kStrokeMinDistance`（0.75 文档像素）不追加，
  整笔在 `WM_LBUTTONUP` 一次提交；失焦/捕获丢失取消未完成笔迹。

### 5.4 选区蚂蚁线 / 可视笔迹 / Smudge-Liquify 接线（2026-10-18 追加）

**蚂蚁线几何在 `ui_core.h`，不在 `main.cpp`。** `selection_boundary_segments()` 在
像素网格上取「命中 vs 未命中」的跨界处，并把同一行/列的连续边合并成一条线段；
`main.cpp` 只做文档→屏幕变换、按画布裁剪、生成虚线段并画「白底 + 黑虚」。要点：

- 覆盖率来源是 `comp_selection_get_coverage`（文档分辨率、8 位、含 feather），
  阈值 128。壳层**先查 `comp_selection_get_active`**：反选「全选」按 ABI 契约
  得到的是「无选区」而不是空选区，此时取覆盖率返回 `COMP_ERR_STATE`。
- **无选区时一条线都不画**：`selection_segments` 为空，叠加层直接跳过。
- 全选时边界是**画布外框**（贴边选区同理），与上游 Ctrl+A 后沿画布爬虫一致。
- 相位动画：`WM_TIMER` 每 120 ms 把相位 +1（周期 dash 4 + gap 4 = 8 px），
  没有选区时定时器处理函数直接返回、不触发重绘。
- 线段先用画布矩形做轴对齐裁剪，再生成虚线段：32 倍放大时一条文档边会变成
  数万屏幕像素，不裁剪会为一条边生成上千个虚线段。

**选区交互。** 「编辑」菜单与快捷键：`Ctrl+A` 全选、`Ctrl+D` 取消、`Ctrl+Shift+I`
反选；Marquee（`M`）在画布上拖矩形 → `comp_selection_apply_shape(RECT)`，
Shift 并集 / Alt 差集 / 否则替换。每次写入是一个撤销步；`undo_document` /
`redo_document` 都会重取覆盖率，撤销后蚂蚁线同步消失。

**可视笔迹。** 落笔期间把文档坐标折线画成 3 px 高对比度折线，并在光标处画笔尖
轮廓（`直径 × 缩放 / 2`，最小 1 px 半径）；`WM_MOUSELEAVE`（`TrackMouseEvent`）
后隐藏。笔迹只是**预览**，真正提交仍只在 `WM_LBUTTONUP` 发生一次。

**Smudge / Liquify 接线。** 原来的 `blur_stroke_*` 收敛为 `retouch_stroke_*`：
按 `shell.retouch_mode` 选择 `COMP_TOOL_BLUR`（`blur.radius`）/
`COMP_TOOL_SMUDGE` / `COMP_TOOL_LIQUIFY`（`warp.strength`）。强度由
`clamp_retouch_strength` 夹在 0.01..1，`Alt+[` / `Alt+]` 步进 10%；模糊模式
同一组键仍调半径。状态栏与工具条按当前子模式显示半径或强度，不再写「尚未接内核」。

**事实边界。** 与 §5.3 相同：以上交互路径**没有经过真人鼠标实测**。`ui_core` 的
几何/分段与 ABI 调用有测试和 `--selftest` 覆盖，但「画出来对不对」仍需开窗目视。

### 5.5 渐变工具（`G`）接线（2026-10-18 S9 追加）

工具条里的 `G` 从 S1 起就在 `tool_table` 占位（快捷键与上游一致），S9 才把它接成真工具。
几何状态全在 `ui_core.h`，`main.cpp` 只做「拖拽 → 一次 `comp_tool_apply`」：

- `kGradientLinear = 0` / `kGradientRadial = 1` 与 ABI `comp_tool_gradient.type` 数值一致；
  `gradient_type_name` 对未知值返回 `?`（不猜），`next_gradient_type` 把未知值收敛回线性；
- `kGradientMinLength = 1.0` 文档像素 + `gradient_drag_is_usable`：短于 1 px 的拖拽由
  外壳直接放弃并提示，**不**把它变成引擎的 `INVALID_ARGUMENT`；
- 手势：`G` 选工具 → 拖出两个端点 → 松开提交；已在该工具时再按 `G` 切线性/径向；
  `Shift` 反向（透明 → 前景）；失焦 / 捕获丢失 / 切走工具只丢预览，不产生撤销步；
- 色标固定「前景 → 透明」：外壳只有前景色模型（§5.4 的 S6 前景色），第二色标没有
  取色 UI，不编造一个宿主不存在的背景色；
- 画布预览只是几何：方向线 + 起点空框 / 终点实心块，径向额外画半径虚圆；
  真正的像素渐变在松开时由引擎一次性写出（与 Blur/Smudge 的「整笔一次提交」同思路）。

**事实边界。** 与 §5.4 相同：交互路径**未经真人鼠标实测**；ABI、`ui_core` 数学与
`--selftest` 驱动序列有覆盖，「画出来对不对」仍需开窗目视。

### 5.6 形状工具（`U`）接线（2026-10-18 S10 追加）

工具条里的 `U` 从 S1 起就在 `tool_table` 占位（快捷键与上游一致），S10 才把它接成真工具。
几何状态全在 `ui_core.h`，`main.cpp` 只做「拖拽 → 一次 `comp_tool_apply(COMP_TOOL_SHAPE)`」：

- `kShapeRect = 0` / `kShapeEllipse = 1` / `kShapeLine = 2` / `kShapePath = 3` 与 ABI
  `comp_shape_kind` 数值一致；`shape_kind_name` 对未知值返回 `?`（不猜），
  `next_shell_shape_kind` 在**矩形 → 椭圆 → 直线 → 矩形**间循环，未知值收敛回矩形；
  外壳只能拖出前三种（`kShellShapeKindCount = 3`），路径只经 ABI。
- `shape_drag_box(start, current, square, from_center)` 是唯一的几何换算：
  `Shift` = 正方/正圆（取较长边、保留拖拽方向），`Alt` = 从中心向外（按下点变中心），
  两者可叠加；修饰键在**松开时**读，拖动中按下/松开都算数（与 Marquee 同口径）。
- `kShapeMinExtent = 1.0` + `shape_drag_is_usable`：直线看两端点距离，矩形/椭圆看宽高
  都至少 1 px；不够大的拖拽由外壳直接放弃并提示，**不**变成引擎的 `INVALID_ARGUMENT`。
- `kDefaultShapeStrokeWidth = 4.0`（量程 0.5..50，`×/÷1.2` 步进）、
  `kDefaultShapeCornerRadius = 0.0`（量程 0..256，2 px 步进）：
  `Alt+[` / `Alt+]` 调**线宽**，`Shift+Alt+[` / `Shift+Alt+]` 调**矩形圆角**；
  圆角预览与提交都用 `shape_effective_corner_radius`（先夹用户量程，再夹短边一半），
  因此预览的圆角半径与落笔盒一致。
- 手势：`U` 选工具 → 拖对角点（直线拖两端点）→ 松开提交；已在该工具时再按 `U` 切种类；
  `Esc` / 失焦 / 捕获丢失 / 切走工具只丢预览，不产生撤销步。
- 颜色/样式：矩形与椭圆**只填充前景色**、直线**只用前景色描边**——外壳没有「描边颜色 /
  线宽面板」，而引擎支持 fill+stroke 同时开；不编造宿主不存在的第二个颜色。
  `corner_radius` 只对矩形生效（对椭圆/直线传 0）。
- 画布预览只是几何：矩形/椭圆画白实线 + 黑虚线双描边（圆角用 `RoundRect`，半径按缩放
  换算），直线画一条双线；真正的像素在松开时由引擎一次性写出（与渐变预览同口径）。
- 与上游的差异：上游画完**新建一个像素图层**，本壳画进**当前选中图层**（引擎口径，
  见 `docs/tool-ops.md` §4.11）；上游用 `Tab` 循环种类，本壳 `Tab` 已归调整面板参数导航，
  因此种类循环改到 `U`。

**事实边界。** 与 §5.5 相同：交互路径**未经真人鼠标实测**；ABI、`ui_core` 数学与
`--selftest` 驱动序列有覆盖，「画出来对不对」仍需开窗目视。

### 5.7 文字工具（`T`）接线（2026-10-18 S11 追加）

工具条里的 `T` 从 S1 起就在 `tool_table` 占位（快捷键与上游一致），S11 才把它接成真工具。
它是第一条**点击 + 键盘输入**的工具（此前都是拖拽），状态机与几何都在 `ui_core.h`：

- 位置/度量：`TypeBlockMetrics` + `type_block_metrics(font_size, line_height,
  letter_spacing, text)` 与引擎 `type_raster` 的 `build_layout` **同一公式**
  （`scale = font_size / 7`、步距 `6 × scale + letter_spacing`、行距
  `font_size × line_height`、`\n` 分行），因此状态栏尺寸、画布预览框与落笔结果一致；
- 字距下限由「步距 ≥ 0.5 px」反推：`type_letter_spacing_min(font_size) =
  0.5 - 6 × font_size / 7`（负字距合法，但不能把字形挤到重叠）；
  `clamp_type_letter_spacing` / `step_type_letter_spacing` 都带 `font_size` 参数，
  字号变小后字距会被重新夹取；
- 键入映射：**S11 当时**用 `type_ascii_from_vk(vk, shift)` 覆盖 **US 布局**的字母、
  数字与常用标点；P5-3 起该函数已删除，字符只从 `WM_CHAR`/输入法进来（见 §5.45）；
- 手势：`T` 选工具 → 画布点击落插入点 → 直接键入 ASCII → `Enter` 提交一次
  `comp_tool_apply(COMP_TOOL_TYPE)`；`Shift+Enter` 换行、`Backspace` 删字符、
  `Esc` / 切工具 / 失焦取消（只丢预览、不产生撤销步）；`[ / ]` 调字号、
  `Shift+[ / ]` 调字距；已在键入态时再点画布 = 先提交上一段、再在新位置落点；
- 键入态是**模态**的：普通字符不再触发工具快捷键（`B` 是字母不是画笔）、
  空格也不再是临时抓手，只放行 Ctrl 组合（撤销/保存等）；`WM_KILLFOCUS` 取消；
- 画布预览只画插入点光标 + 文本框（白实线 + 黑虚线双描边），**不预演字形**：
  外壳没有系统字体（字体是引擎内的点阵），用 GDI 画一套只会与落笔结果不一致；
  像素在 `Enter` 时由引擎一次写出（与渐变/形状「拖完一次提交」同口径）；
- 与上游的差异（**S11 当时**）：ASCII-only（无 CJK / 字体选择 / 粗斜体 / 对齐 /
  旋转），文字是**一次性像素**而不是可编辑文本对象，画进当前图层而不是新建图层
  （见 `docs/tool-ops.md` §4.12）。CJK 与字体选择由 P5-3 补上（见 §5.45）。

**事实边界。** 与 §5.6 相同：交互路径**未经真人键盘/鼠标实测**；ABI、`ui_core`
数学层与 `--selftest` 驱动序列有覆盖，「打出来的字好不好看、光标位置对不对」仍需开窗目视。

### 5.8 文字属性面板与行距快捷键（2026-10-19 S14 追加）

S11 的 `T` 工具只能用快捷键改字号/字距，行距没有按键。S14 把它补成右侧属性面板，
**只改外壳，引擎与 ABI 零改动**：

- 三行固定行表：`Font Size`（px）/ `Line Height`（%）/ `Letter Spacing`（px）；
  行几何 `kTypeRowHeight = 20`、`kTypeRowsTop = 54`、`type_param_row_rect` /
  `type_param_row_at` 与绘制共用（同滤镜面板「看得见就点得中」）；
- 量纲：字号乘性 ×/÷1.25（6..2048）；字距线性 ±1 px，下限仍由
  `type_letter_spacing_min(font_size) = 0.5 - 6 × font_size / 7` 决定；
  行距新增 `step_type_line_height`，**5% 一步、夹在 1.0..8.0**；
- 键盘（Type 工具、未在键入文字时）：`Tab / Shift+Tab` 切行、`- / =` 调值、
  数字键开始输入、`Enter` 提交（完全解析 + 有限值 + 夹取）、`Esc` 取消；
  点面板行 = 聚焦；`[ / ]` 仍调字号、`Shift+[ / ]` 仍调字距；
- `Alt+[ / ]` 调行距（`WM_SYSKEYDOWN`），**键入文字时也可用**；这是 S11 缺的按键；
- 面板与调整面板互斥输入态：切到 Type 清调整面板缓冲，离开 Type 清面板缓冲并
  丢弃未提交文字；
- 面板只改「下一段文字」的参数；键入中的文字读同一组值，状态栏与文本框预览实时跟随；
- 边界：数字键入为非负值（空缓冲时 `-` 是步进键，负字距走 `Shift+[`，与滤镜/调整面板
  同规矩）；值随会话保留、不进 `.comp`；仍未经人手 Win32 目视。

### 5.9 拖动提交缺陷修复 + 自动化目视（2026-10-19 S15 追加）

S4–S14 的每一份报告都记着同一条边界：「未经人手 Win32 目视实测」。S15 补了两件事：
修掉一条只有真鼠标才能暴露的提交缺陷，并把「开窗点一遍」做成可重跑的命令。

**1. 真缺陷：`ReleaseCapture()` 早于载荷搬出。** `paint_stroke_end`（画笔/橡皮擦）、
`retouch_stroke_end`（模糊/涂抹/液化）、`lasso_end`（套索）三处旧顺序是
「先 `ReleaseCapture()`，再把 `stroke_points` / `lasso_points` 搬出去提交」。
`ReleaseCapture()` 会**同步**派发 `WM_CAPTURECHANGED`，而该处理器为「丢焦点/丢捕获即取消」
会调 `stroke_cancel` / `lasso_cancel` 清空折点——于是真人拖完松手，画面上什么都没有；
脚本用 `SendMessage` 模拟时因为在同一线程里不会触发这条重入路径，所以自检一直绿。
修法：先把折点 `swap` 到局部变量、再 `ReleaseCapture()`、最后提交。三处同改，并加中文注释
说明「为什么不能调换顺序」。`marquee_end` / `gradient_end` / `shape_end` / `move_end` /
`pen_up` / `eyedropper` 的 `*_cancel` 只清 active 标志，不受影响（已逐一核对）。

**2. 自动化目视（`scripts/visual_check.py`，新增）。** 11 个场景、45 项像素/几何断言、
32 张 PNG（`../report/assets/s15/`），走真 `PostMessage` 键鼠 + `PrintWindow` 抓客户区像素：
`start / brush / selection / wand / gradient / shape / type / filter / adjustment / layers / view`。
几条值得记的口径：

- 键：`AttachThreadInput` + `SetKeyboardState` 摆修饰键，再 `PostMessage(WM_KEYDOWN/UP)`；
  Alt 组合走 `WM_SYSKEYDOWN`。焦点：`AttachThreadInput(fg, target)` + `SetForegroundWindow`
  并校验 `GetForegroundWindow()`——否则 `SetCapture` 会丢，拖动类工具全部失真。
- 鼠标：`PostMessage(WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP)`，**不移动物理光标**；
  但会抢前台窗口（`SetCapture` 需要），跑脚本时别同时用键鼠。
- 断言不猜颜色常量：先由空文档的纯黑外接框测出文档屏幕矩形，再在矩形内取样；
  文字场景用「字形像素外接框高度」比较 32 px 与 48 px 两段文字（实测 16 px vs 23 px）；
  调整场景先铺底 `#336699`、新图层上挂 Exposure、抬值应用，断言像素
  `(51,102,153) → (116,218,255)`，`Ctrl+Z` 后逐字节回到 `(51,102,153)`。
- 已知坑：`Ctrl+=` 是 `VK_OEM_PLUS`（0xBB），不是字符 `'='`；文字键入态会吃掉数字键，
  改字号必须先 `Enter` 提交文字再点面板行；调整是「调整图层、只作用于下方」，
  挂在像素层上本来就看不到变化（见 §6.1 / `docs/manual-test.md` M10）。

**3. 人工清单（`docs/manual-test.md`，新增）。** 把 M1–M16 的手工步骤、预期文案、
脚本盲区（手感、蚂蚁线流动、跨 DPI、多显示器、模态对话框、退出清理）写成可勾选清单，
并给出「脚本场景 ↔ 清单章节」对照表。**它仍不替代 ALEX 的目视结论**。

**事实边界。** 本轮证据是**机器抓的像素**，不是人眼；脚本抢前台、不模拟物理光标；
`Z` 缩放工具按钮可选中但点击暂不改缩放。（当时「DXGI 交换链直呈未接」，S16 已接，见 §5.10。）

### 5.10 S16 窗口 DXGI 交换链直呈（2026-10-19 追加）

**1. 能力。** D3D12 构建下 `caps().window_present` 为 true，`comp_render_document` 收
`COMP_SURFACE_HWND` 时：合成结果拷进 DXGI flip-model 后备缓冲，`comp_render_present`
调 `Present` 上屏，**不回读**（`last_render_offloaded()` 仍为 true，`readback_frames()` 不增）。
非 D3D12 构建或设备不可用 → 照旧 `COMP_ERR_UNSUPPORTED`，AUTO 回落 CPU 缓冲路径。

**2. 关键实现约束（踩过的坑）。** 后备缓冲必须**创建时取一次并常驻持有**，每帧用
`IDXGISwapChain3::GetCurrentBackBufferIndex()` 轮转。第一版每帧 `GetBuffer(0)` 再
`Release`，第二帧 `ExecuteCommandLists` 后设备被移除（`0x887A0005`，原因
`0x887A002B DXGI_ERROR_ACCESS_DENIED`）——因为写到了刚 Present 出去、正在扫描输出的表面。
D3D11 上同样写法不炸（运行时内部代管轮转），所以这条坑只在 D3D12 直呈路径上暴露。
代码里已用中文注释钉住，并有 `tests/test_rhi.cpp` 的**连续 4 帧**用例守门。

**3. 基准（`--present-bench[=N]`）。** 交互壳主画布仍走 CPU 展平 + GDI（直呈窗口与 GDI
覆盖层面板/蚂蚁线/笔迹预览不能安全混用），基准因此走独立窗口：1024²、底色 + 3 层整幅
画布图层、预热 3 帧。AMD RX 6800 实测（30/60 帧各三次）：GPU 直呈平均
**3.04–3.27 ms**，CPU `flatten + RGBA→BGRA + StretchDIBits` 平均 **3.85–3.91 ms**，
约 **1.18–1.29×**。只快两成，瓶颈是逐层上传而非上屏；逐层 GPU 纹理缓存需要图层内容
修订号（当时 `ImageBuffer::pixels` 是 shared_ptr + COW，无修订计数），列为后续工作。

**该后续工作已在 S18/S19/S20 完成**：S18 加了逐层 `content_revision`（§6.28），S19 做了
D3D12 持久逐层纹理（原生单元跨帧常驻，§6.29），S20 把烘焙单元也纳入缓存（子树结构键 +
CPU 烘焙表面缓存 + 烘焙纹理缓存，§6.30）。同一基准连跑三次（`--present-bench=60`）：
S19 GPU 直呈平均 **1.389 / 1.406 / 1.413 ms**、S20 **1.365 / 1.390 / 1.421 ms**，CPU 路径
**2.145–2.266 ms**，比值 **1.56–1.61×**——证实瓶颈确实是逐层上传。剩余成本是每帧
「4 MB 清屏拷贝 + 4 次 dispatch + 8 MB 后备缓冲拷贝 + Present」，不是缓存能消掉的。
（该基准场景全部是原生单元，S20 的数字只说明没有回退；v2 收益见 §6.30 的计数。）

**4. 边界。** GPU 子集自 S17 起**按合成单元**拆分（裁剪/蒙版/效果/变换/隔离组/剪贴栈在
CPU 烘焙后照样上 GPU，§6.27），仍整篇回落的是调整层与 HDR/RGBA16F 表面。烘焙单元自 S20
起跨帧缓存（文档不动时不再烘焙/上传，只改一层时只重做该单元）。交换链只在尺寸变化时重建，跨 DPI/多显示器/窗口拖动后的
行为**未经人手目视**（`docs/manual-test.md` M1–M16 仍未勾选）；鸿蒙壳窗口表面仍
`UNSUPPORTED`。

### 5.11 未达成的 UI 项

| 缺口 | 状态 | 原因 |
|------|------|------|
| Move 工具拖拽平移图层 | 已做（2026-10-21，§5.24 / §5.36） | 无选区 + 选中图层时拖动 = 改 transform；有选区时按落点分流：选区内搬像素、选区外拖整层；一次拖动一步撤销 |
| Spot Healing（`J`） | 已做（2026-10-21，§5.25） | 折线在宿主侧光栅化成图层像素空间的 coverage 蒙版，一次 `comp_tool_apply` |
| 工具按钮图标 | 已做（2026-10-21，§5.40） | 20 个工具图标按 32×32 逻辑网格用 GDI 现画，跟随按钮缩放；不新增 .rc/.ico/字体资源，构建产物仍是单文件 exe |
| 选区扩缩手柄 | 已做（2026-10-22，§5.44） | 选区边上 8 个把手（Marquee/Lasso/Wand/Move 下出现），拖动调 `selection_drag_rect` 出预览、松手一次 `comp_selection_set_bounds`；ABI 尾部追加 `comp_selection_get_base_bounds` / `_set_bounds`（138 → 140）——源恒为紧包围盒（不含羽化）、双线性、恒等逐字节不变且不落撤销步、退化 = 显式空选区 |
| Brush/Eraser 逐笔撤销粒度 | 已做 | 每一笔 = 一次 `comp_tool_apply` = 一步撤销（笔迹预览只在宿主侧，不落文档）；「笔中逐段撤销」上游也没有 |
| 滤镜实时预览 | 已做（2026-10-21，§5.35） | 改参数即出帧：宿主事务里应用 → 取帧 → abort，预览只在 framebuffer，文档不动；回车真应用，结果与预览逐字节一致 |
| 面板缩放 / 停靠 | **已做（2026-10-26，§5.47 停靠子系统）** | 宽度可拖：左右面板内侧 5 px 分界线，图层面板 168–520 px、右侧面板 240–560 px，双击分界线复位；整块收起/展开已做（F7 / F8、视图菜单、标题行按钮，收起后留 24 px 竖条）；**停靠子系统的四项增量也已全部落地**：多块面板可同时浮动、拖拽停靠（拖出成浮窗 / 拖回边缘停靠）、浮动面板滚轮、多面板合并成一组标签（tab strip） |

以上均为**外壳交互**缺口，不涉及引擎核与两壳共享语义。

### 5.12 图层面板补全（P0-1，2026-10-21）

P0-1 之前面板只能新增、显示、±10% 调不透明度：删除/复制/改名/建组/多选/拖进组全部缺失。
本轮把它补到「对齐上游图层面板」的可用面，并对齐两条工程约束：**每个命令恰好一步撤销**、
**跨 ABI 只传合法 UTF-8**。

**1. ABI 纯尾部追加 4 个导出（123 → 127）。** 既有枚举/签名一个不动：

| 导出 | 语义 |
|------|------|
| `comp_layer_get_name` | 读名字（壳侧不再自持名字，撤销/合并/读盘后都重取） |
| `comp_layer_set_name` | 改名；严格 UTF-8，空名/坏编码/超长/未知 id 显式拒绝 |
| `comp_layer_move` | 改父级；`before = 0` = 移到目标组末尾（组顶），非 0 为同父锚点；一步撤销 |
| `comp_layer_duplicate` | 复制到同一父级、原层上方；返回新 id；一步撤销 |

**2. 命令与撤销粒度。** 每个命令**只发一次显式事务**（`comp_txn_begin`/`commit`，失败 `abort`）：

- `Ctrl+G` 新建组：先 `comp_layer_add` 建组，再把选中层逐层 `comp_layer_move` 进组；任一步失败
  整体回滚，不留半个组。
- `Del` 删除、`Ctrl+J` 复制：多选时主选中 + `extra_selection` 整批入**一个**事务，N 层只占一步撤销。
- `Shift+O` 精确不透明度：键入百分比（0..100，允许小数），整批设置进一个事务。
- `F2` / 双击行改名：预填当前名字，回车提交；改名单写一步事务。
- 拖行重排 / 拖进组 / 拖出组：只发一次 `comp_layer_reorder` 或 `comp_layer_move`，天然一步撤销。

**3. 记录名字的权威在引擎。** 面板行不再缓存名字：每次 `refresh_layers` 都经
`comp_layer_get_name` 重取（两段式：先探长度、再取值）。否则撤销重命名、合并图层、读盘之后
面板会显示旧名字，与引擎状态分叉。

**4. 键入态（重命名 / 不透明度）走 `WM_CHAR`。** 本壳沿用状态栏输入态，不另开对话框：
进入即模态（`Esc` 取消、`Enter` 提交、退格删字符），其它快捷键让路；字符经 `WM_CHAR`
（而非 `WM_KEYDOWN` 的 VK 映射），因此中文/IME 与 ASCII 走同一条路径。提交时
UTF-16 → UTF-8 一次性转换（未配对代理由系统替换为 U+FFFD），引擎只收合法 UTF-8。
中文名有回归：验收脚本用 `WM_CHAR` 输入「组甲」，断言变化像素从行首名字区开始。

**5. 多选与拖拽。** Ctrl/Shift 点行 = 追加/扩展选中，`selected_layer` 仍是**主选中行**
（既有单层命令的输入语义不变），额外选中的 id 存在 `extra_selection`（存 id 不存行号，
重排/撤销后行号会失效）。最初**拖拽只作用于主选中层**，多选批量拖拽没做；§5.37 起批量
拖动与方向键微调都作用于整个选中集合（组按子树展开、整批一个事务 = 一步撤销）。

**6. 拖进/拖出组的几何判定。** 面板仍是扁平列表（不是树），跨父级只能靠落点区分：
落到**组行的中间带**（行高 30%..70%）= 拖进该组末尾；落到**根级行** = 从组内移出到根级，
放在目标行下方；其余跨父级落点（目标在别的组里、或把组拖进自己的子树）明确拒绝并说明原因，
不静默改父级。落在行内上/下 30% 仍是同父重排。

**7. 修掉的引擎缺陷（本轮唯一行为修复）。** `Document::move_layer` 在 `before = 0` 时用
`pos == state_.layers.end()` 判断「该组还没有子层」，但 `pos = 最后一个同父兄弟 + 1` 恰好在
「最后一个兄弟就是数组末元素」时也等于 `end()`——于是落进 fallback，被插到组自身下标 +1
（= 组底）。结果是**每一次**拖进组都落到组的最下面，与 `comp_layer.h` 写明的「`before = 0`
表示移到目标父级的末尾」相反；`Ctrl+G` 批量收层会静默反转整组堆叠序（两层直接上下颠倒）。
修法：用独立的 `positioned` 标志区分「找到位置」与「插入点在末尾」，`end()` 从此是合法插入点。
`tests/test_layer_mutation.cpp` 在既有用例里补一条回归（不新增用例数）：在**非空**组上
`comp_layer_move(id, group, 0)` 后展平，断言新层盖住原组顶。

**8. 已知偏差与边界。**

- **组行显示在子层下方**：`move_layer` 把子层插在「组自身下标 +1」，数组序自下而上，
  于是旧版扁平面板里子层在组行之上（与上游 PS 相反）。**§5.42 起改为树形面板**：
  显示序不再等于数组逆序，组行在上、子层按堆叠序缩进列在下方。
- 旧版面板没有树形缩进、没有组折叠（**§5.42 已补**）；Shift 点选连续区间也已在 P0-1
  收尾时补上（本节第 5 段）。
- **验收证据是机器抓的像素，不是人眼**：临时脚本（仓库外）对每一步做「操作前 ≡ 一次撤销后」
  逐像素相等 + 「操作后 ≡ 一次重做后」逐像素相等，30 项全过；`docs/manual-test.md` 的
  手感/跨 DPI/多显示器等项仍待 ALEX 目视。

### 5.13 效果参数面板（P0-2，2026-10-21）

P0-2 之前效果只能从菜单添加/清除，参数一个也改不了；壳里还抄了一份
`effect_kind_defaults()` 的缺省值摘要，与 `effect_eval.cpp` 的 `read_*` 实参是**两份数值**。
两份表漂移不会编译报错，只会让面板显示的量程/缺省与落笔语义分叉。

**1. ABI 纯尾部追加 4 个导出（127 → 131）。** 既有枚举/签名一个不动：

| 导出 | 语义 |
|------|------|
| `comp_layer_effect_param_count` | 某 kind 的参数个数；未知 kind → `INVALID_ARGUMENT` |
| `comp_layer_effect_param_at` | 取第 i 项元数据（`json_path`/`label`/`type`/`min`/`max`/`default`） |
| `comp_layer_effect_read_value` | 按扁平键从 settings 里读标量（数字或布尔），复用调整层的路径规则 |
| `comp_layer_effect_build_settings` | 两段式：把面板的 N 个文本值**合并**进既有 settings，未知键原样保留 |

**2. 单一事实源。** 参数表落在 `engine/domain/effect_table.{h,cpp}`，六种 kind 的参数个数
**6 / 7 / 4 / 7 / 5 / 5**（Stroke / DropShadow / ColorOverlay / InnerShadow / OuterGlow /
InnerGlow），键名就是 `effect_eval.cpp` 的扁平 JSON 键。`effect_read_value` 直接委托给
`adjustment_read_value`：两者语义完全相同（点分路径取标量，缺失 / 中间段非对象 / 叶子非标量
一律 `NOT_FOUND`），共用一份实现避免同一条规则写两遍。壳侧的 `effect_kind_defaults()` 整块删除，
壳只剩 kind **名称**（显示用）。

**3. 合并而不是重建。** 效果 settings 是前向兼容的不透明字符串，整棵重建会丢掉上游新增字段。
因此 `build_settings` 的底串永远是本层当前 settings 的回读，空串 = 该项不动，只有显式给出的
键被覆盖。越界 / 非数 / 坏布尔 / 坏 base 显式失败；多给的值按调整面板同口径忽略。

**4. 面板交互。** `Ctrl+Shift+E`（或效果菜单「编辑参数…」）打开右侧效果面板；右面板同一时刻
只属于滤镜/效果/文字/调整之一，打开效果会先关掉滤镜。`Tab` 选参数、`-`/`=` 步进
（跨度 ≤ 1 时 0.05，否则 1.0）、数字/`.`/`-` 进入键入态、`回车` 应用（未启用时等同添加）、
`,`/`.` 切换效果类型、`Backspace` 重置为本表缺省、`Esc` 关闭；点行即聚焦。几何与步进是
`ui_core.h` 里的纯函数，有单测；应用只发一次 `comp_layer_set_effect`（同 kind 幂等替换），
天然一步撤销。

**5. 漂移护栏。** `tests/test_effect_table.cpp` 对六种 kind 各做两次展平：
`set_effect(kind, "{}")` 与「把表里缺省值全部显式写入」必须**逐字节相同**。另有参数个数、
键名/缺省值抽样、未知键保留、越界拒绝、回读往返共 6 例；壳侧几何/步进 1 例。

**6. 边界。** ① 无滑杆拖动、无实时预览（`回车` 落笔一次，与滤镜面板同口径）；
② 颜色是 `red`/`green`/`blue` 三个 0..1 数字，无取色器/HSL；③ 参数面板只编辑当前主选中
图层（§5.38 已补多选**批量开关**，批量时参数仍只改主选中层）；④ 参数表与
`effect_eval.cpp` 是两份手写常量，靠像素对拍守而不是编译期
联动——新增参数必须两处同改；⑤ 手感/跨 DPI 待 ALEX 目视（`docs/manual-test.md` M17）。

> **2026-10-23 补记（P1-1，README §6.97）：** kind 表由 6 张扩到 **9 张**（尾部追加
> 斜面和浮雕 12 项 / 缎光 8 项 / 渐变叠加 9 项），壳侧 `kEffectKindCount` 与效果菜单按表
> 自动多出三项，面板交互与步进规则**一行未改**——这正是本轮把参数元数据收进 ABI 的回报。

### 5.14 调整滑杆 + 曲线/色阶编辑器（P0-3，2026-10-21）

P0-3 之前调整面板只能「`Tab` 换行 + `=` 步进 + 数字键入」，没有滑杆也没有拖动预览；
Levels 的 20 个量压根没有面板参数（`param_count = 0`），Curves 整条曲线没有编辑入口。
本轮补三件事：**滑杆拖动**、**一步撤销**、**曲线控制点编辑**。

**1. ABI 纯尾部追加 4 个导出（131 → 135）。** 既有枚举/签名一个不动（表见
`docs/adjustment-json-keys.md` §5）：

| 导出 | 语义 |
|------|------|
| `comp_layer_curves_channel_count` | 通道数（恒 4：0 = RGB，1..3 = R/G/B） |
| `comp_layer_curves_point_count` | 读某通道点数；缺失/非法 = 0（语义即恒等） |
| `comp_layer_curves_point_at` | 读第 i 个点；与上面共用同一份「排序 + 同 x 去重」结果，不会漂移 |
| `comp_layer_curves_set_points` | 改写某通道点列，返回新 settings（两段式；**纯函数**：不开事务、不写文档） |

**2. 单一事实源。** Levels 补齐 20 个参数（`levels.ranges.0..3` × black/gamma/white/
outputBlack/outputWhite），量程逐条抄上游 `LevelRange.normalized`（black 0..254、
white 1..255、gamma 0.1..9.99、输出 0..255），落在 `adjustment_table.cpp`；
白与黑的相对顺序不校验，由求值侧 `normalize()` 兜（同上游）。曲线点列的结构约束
（4 通道、2..32 点、x 严格递增、首末点 x 锁 0/255、值域 0..255）静态参数表表达不了，
落在 `engine/domain/curves_settings.cpp`，由 `adjustment_validate_settings` 调用。

**3. 一步撤销。** 拖动途中每次写回都是 `comp_layer_update_adjustment_at`，
不开事务时引擎的 `ImplicitTxn` 会给每次写各记一步——拖一次滑杆能产生几十步撤销，
正是 P0-3 要修的东西。现在的边界是：

- 按下开**显式事务**（`comp_txn_begin`），拖动期间所有写回都折在它里面；
- 松手 `comp_txn_commit` → 整段拖动**只占一步**；
- 拖动没有任何实际写入（值没变）→ `comp_txn_abort`，**不留空撤销步**；
- `Esc` / `WM_CAPTURECHANGED`（失焦、被别处抢捕获）→ abort 回滚到按下前快照；
- 新增态（`selected_adjustment < 0`，还没有可写对象）拖动只改编辑缓冲、不写文档，
  仍由 `Ctrl+Shift+U` 落盘——「先调好再加」不该进历史。

**4. 几何/命中共用一份纯函数。** 滑道矩形、滑道命中、x→值、值吸附、曲线绘图区、
曲线通道条、点命中、插入点选址（`curve_insert_x`）、值↔像素换算全在
`ui_core.h`，绘制与命中**调同一批函数**（而不是各写一套常量），单测直接盯这些函数；
`visual_check.py` 侧把同一套几何再镜像一遍，断言「绘图区是参数区里的正方形」这类性质，
不抄魔法像素。

**5. 曲线编辑器交互。** 通道条四页（RGB/R/G/B，点条即换页）；在绘图区空白处按下 =
插入控制点并立刻进入拖动（一次按下就能定位），按在已有点上 = 拖该点；`Delete` 删焦点点，
两端点锁死不可删（删了定义域就不完整）。x 保持严格递增（内部点夹在左右邻点 ±1 内），
x/y 取整后写回引擎；点列真相在引擎 settings 里，壳只缓存当前通道用于绘制/命中，
每次写完 `refresh_adjustments` 以引擎为准回读。

**6. 这次真崩过一次（教训）。** 第一次用鼠标拖曲线点时外壳 `0xc0000005` 崩在 `USER32.dll`
（`fault offset 0x401c3`）——根因是新写的两处状态栏文字用了
`wsprintfW(L"… (%.0f, %.0f) …")`：**`wsprintfW` 不支持浮点转换**，它按整数宽度取变参，
`double` 被当成指针读飞。修法是把数值先过 `number_text()`（`snprintf`）再以 `%s` 拼进去，
与壳里其它面板同口径。自动化场景 `curve_levels` 现在覆盖这条路径（键盘路径**永远**测不出来：
`scene_adjustment` 全是键盘，`%.0f` 那两行只有拖动/绘制焦点点时才执行）。

**7. 验证。** `tests/test_adjustment.cpp`：通道点数/读点/写点/非法拒绝/两段式容量，
外加 `curves_points_survive_comp_round_trip`（`.comp` 存盘再载入后 settings **逐字节**相同、
四个通道逐点相同）；`scripts/visual_check.py` 新增场景 `curve_levels` 6 项：
绘图区几何、抬中间调后画布变亮、画布直方图中位数右移、一次 `Ctrl+Z` 回到修改前、
色阶黑场拖到 40 压暗、再一次 `Ctrl+Z` 回到色阶前。全套 16 场景 74 项零失败。

**8. 边界。** ① 曲线没有预设库、没有直方图底图、没有贝塞尔手柄（求值固定 Fritsch–Carlson
形状保持三次 Hermite）；② 色阶通道条只切面板分页，**不写回** `levels.channel`，求值恒按
`ranges[0]` 管合成、`ranges[1..3]` 各管一个色彩通道；③ 布尔参数没有滑道（点一下即切换）；
④ 滑杆值吸附到整数是「量程 ≥ 50」时的统一规则，没有按住 `Shift` 的精细模式；
⑤ 手感/跨 DPI/连续拖动是否跟手仍待 ALEX 目视（`docs/manual-test.md` M18）。

### 5.15 选区补齐第一批：扩展 / 收缩 / 按图层像素载入（P0-4，2026-10-21）

P0-4 的前提是「引擎与 ABI 全齐，只差 UI」。实测三个入口确实都是**壳侧零调用**：
`comp_selection_modify(EXPAND | CONTRACT)` 与 `comp_selection_load_layer(LOAD_PIXELS)`
在引擎里有实现、在 `tests/test_selection.cpp` / `test_canvas_ops.cpp` 里有断言，
但 `shells/windows/src/main.cpp` 里搜不到调用点。本批先接这两条（ABI **零改动**）：

**1. 菜单单列「选择(&S)」。** 此前全选/取消/反选夹在「编辑」里，扩展/收缩装不下。
现在单列一个菜单，三条旧命令**只搬位置、ID 不变**（1031/1032/1033），
`run_shell_command` 同时服务菜单与快捷键，两条入口不会分叉；新增
`扩展…`（1047）/`收缩…`（1048）/`按图层像素载入选区`（1049）。

**2. 扩展/收缩共用一个键入态。** 菜单只决定 `modify_op`，半径的键入/退格/回车/Esc
与羽化完全同款（状态机只有一份：`begin_modify_input` → `commit_modify_input`）。
量程 1..500 来自 `ui_core.h` 的 `kMinSelectionGrow`/`kMaxSelectionGrow`，与引擎
`kMaxSelectionGrow` 同口径；**0 在壳侧就拦成提示**，不把必然被判 `INVALID_ARGUMENT`
的调用打到引擎上（羽化的 0 有意义 = 清除羽化，扩展/收缩的 0 没有语义，这是两者唯一的
量程差异，所以没有复用 `clamp_selection_feather`）。

**3. 按图层像素载入**取当前选中图层、`threshold = 1`（任何有像素处都算选中）。
取 0 会把全透明像素也算进去（等价全选），不是「按内容载入」的本意；将来给阈值面板时
再放出来。蒙版载入那条入口早在 S22 就有（图层菜单「载入蒙版为选区」），这里补的是
**像素**侧，二者共用同一个 `comp_selection_load_layer`。

**4. 验证。** `--selftest` 追加 9 项（219 → **228**，`compositor_shell_selftest.txt`
已显式重定基线）：20×20 矩形 → EXPAND 3 得 26×26、一次撤销回 20×20；CONTRACT 3 得
14×14、一次撤销回 20×20；EXPAND 0 被判非法且不改选区；LOAD_PIXELS 选中全不透明层。
`tests/test_ui_core.cpp` 加 `ui_core_selection_grow_clamps_to_engine_range`（钳位与步进，
含「步进不得推到 1 以下」）。单测 **554 例 / 0 失败**。

**5. 边界。** ① 色彩范围对话框当时未做，已在第二批补齐（见 §5.16）；② 扩展/收缩在引擎里是
「按距离变换外扩/内缩」，拐角是圆角、超出画布的部分被裁掉，这是上游口径不是缺陷；
③ 空选区调用扩展会被引擎当无操作（`changed = 0`），壳在入口就先拒绝并提示；
④ 菜单项置灰已在本轮补齐（见 §5.41）；快捷键路径不变，无选区时按快捷键仍给状态栏提示。

### 5.16 色彩范围对话框（P0-4 第二批，2026-10-21）

第一批留下的那半条。引擎侧 `comp_selection_apply_color_range` 早就存在
（`engine/domain/selection.cpp:1030`），壳里同样**零调用点**；本批只做接线与面板，
ABI 零改动。语义对齐上游 `UI/ColorRangeSheet.swift` + `Document/ColorRangeSelection.swift`。

**1. 入口。** 「选择(&S)」菜单新增 `色彩范围(&C)…`（命令 ID 1050），与
`扩展…/收缩…` 之间用分隔线隔开。面板挂在右侧同一列（`layout.adjust_panel`），
与滤镜面板、图层效果面板**互斥**：开一个时把另一个按各自的 cancel 路径关掉，
避免两块矩形互相遮住、也避免两份事务同时挂在同一个文档上。

**2. 预览 = 一条显式事务（与 P0-3 滑杆同一套手法）。**
- 打开面板即 `comp_txn_begin("Color Range")`，快照停在「打开前的选区」；
- 每次取色 / 改容差 / 切反相都在事务里**整篇重算**一次（`COMP_SELECTION_REPLACE`），
  因此预览不产生撤销步，也不会攒出几十步垃圾；
- `回车` = `comp_txn_commit` → **恰好一步撤销**；`Esc`、或从没取过色就回车 =
  `comp_txn_abort` → 选区回到打开前，撤销栈不动。
- 取色退到空表（点掉所有色块 / `Backspace` 清空）时，上游是「回到原选区」而不是
  「全不选」。实现是 abort 当前事务再立刻 begin 一条新的：快照随之变成当前（= 打开前）
  状态，后续再取色仍然只落一步。

**3. 取色与色板。** 画布单击取样：`comp_eyedropper_sample(..., sample_size=3,
COMP_EYEDROPPER_MERGED)`，即**合并合成的 3×3 均值**（上游 `color(in:at:)` 同口径）。
单击 = 替换（清空旧取色与排除，对应上游 `sampleMode = .replace`）；`Shift+单击` = 加色；
`Alt+单击` = 加入排除表。取色行/排除行各画一行 18 px 色块（行距 4 px），排除色块上叠
一道红斜杠以便与取色行区分；**点色块 = 删掉该取样色**并立即重算。

**4. 参数。** 容差 `0..200`（缺省 40，抄 `ColorRangeEdit.fuzziness`），`-` / `=` 步进
（<20 走 1、<100 走 5、其余走 10），数字键进入键入态、`回车` 落值（键入值同样夹到
`0..200`，面板永不把越界参数递给引擎）；`I` 切反相；`Backspace` 清空取样色。
`0` 在这里是合法容差（只选完全同色），与扩展/收缩的下限 1 不同 —— 所以
`clamp_color_range_fuzziness` 没有复用 `clamp_selection_grow`。

**5. 0 匹配的语义缝。** 引擎对「一个像素都没匹配」的定义是**不改选区**（与魔棒
「点空处不留下空蒙版」同口径），而上游面板此时显示空选区。壳侧在
`changed == 0` 时显式调 `comp_selection_deselect`（仍在事务内），于是「反相取一个不存在的
颜色」也能得到上游那样的空选区，且回车后是一步可撤销的操作。

**6. 键盘归属。** 面板打开时键盘归面板（工具字母键被吞掉）。与滤镜/效果面板不同，
这里**连部分 Ctrl 组合也拦**：`Ctrl+Z/Y/N/O/S/A/D` 会先把事务快照与当前文档撕开，
abort 时「复活」一个本不该存在的选区，所以只在状态栏提示「先回车应用或 Esc 取消」；
缩放等与事务无关的 Ctrl 组合照常穿透。

**7. 文档替换时的 UAF 防护。** `新建 / 打开 .comp / 打开位图` 三条路径都会销毁旧文档，
而 `color_range_txn` 是挂在旧文档上的裸句柄。三处都在 `comp_document_destroy` **之前**
调 `forget_color_range`（只丢句柄、关面板、清取样，**不 abort**）——对已释放文档 abort
才是真 UAF，这与「面板没关」是两码事。

**8. 验证。** `--selftest` 追加 17 项（228 → **245**，基线已显式重定，连续两次运行
SHA256 一致）：同色全选不要求连通（左半灰 + 底行取样点）、恰好一步撤销、撤销回到打开前的
无选区状态、反相 = 补集且一步撤回未反相、容差 200 是闭区间（两半全选）、
`include == exclude` 零匹配且不动选区/不落步、容差 201 与 `NULL + count>0` 被 ABI 闸拒绝
且 `out_changed` 不被写、事务内连改两次只落一步且以最后一次为准、
`comp_txn_abort` 还原选区且不增步。`tests/test_ui_core.cpp` 加 2 例
（`ui_core_color_range_fuzziness_is_zero_based`、`ui_core_color_range_swatch_geometry_hits_what_it_paints`：
钳位/步进与色块·参数行几何的绘制↔命中共源）。单测 **556 例 / 0 失败**。

**9. 边界。** ① 色块只画首屏放得下的（330 宽 → 13 个）：超出的颜色仍然生效，只是点不到；
要减掉它们按 `Backspace` 清空后重取；② 没有「取样半径」可选项（固定 3×3）；
③ 未做上游的「存储/载入取样」(Save/Load selection colors)；④ 取色基于合成结果，
16 位/HDR 文档走 8 位视图（与魔棒同一家族），不是逐通道深比较。

### 5.17 路径面板最小版（P0-4 可选，2026-10-21）

路线图里 P0-4 的可选尾巴：「列出已闭合路径、激活/删除、双击转选区」。先确认了
**ABI 里没有路径实体**——`comp_selection_apply_path` 是「折线/三次贝塞尔进、选区出」，
没有 `comp_path_create` 一类句柄，也没有任何 `.comp` 侧的路径持久化。所以这一节做的是
**壳侧存储 + 复用已有入口**，ABI 零改动（仍是 135 个导出）。

**1. 路径是什么。** 本壳里「路径」= 一次钢笔提交时的锚点副本（`StoredPath{name, anchors}`，
`PenAnchor` 是文档坐标 + 两个控制柄）。提交后 `pen_anchors` 会被清空，所以在
`commit_pen_path` 里**先复制一份**再走原流程，最后无条件入列：即使这次 `changed == 0`
（选区没变、事务回滚），锚点本身有效，留着可以再落一次。入列只动壳侧 `std::vector`，
**不产生撤销步**。

**2. 上限与命名。** `kPathMaxEntries = 64`，满了丢最旧一条（FIFO）并在状态栏点明
「最旧一条被挤掉」；名字用单调流水号 `路径 N`（删除后不复用，避免重名）。之所以不给
重命名/复制/导出：没有引擎实体，这些操作在壳侧都只是字符串，做了也带不出文档。

**3. 入口与互斥。** 「选择(&S)」菜单新增 `路径面板(&T)…`（命令 ID **1051**，
`Ctrl+Shift+P`），与色彩范围共用右侧同一列（`layout.adjust_panel`）。开面板时：
色彩范围若开着走 `color_range_cancel`（**先 abort**，不能让显式事务留着）、滤镜面板
与效果面板直接关掉；反过来这三者打开时也会把 `path_open` 置假。右栏一次只挂一个面板，
与 §5.16 同一条约束。

**4. 文字工具占栏。** `T` 要独占右栏显示自己的属性面板。这里不弹错、也不强行抢栏，
而是用一条谓词 `path_panel_visible()`（`path_open && tool != Type`）同时约束
**绘制、键盘、鼠标、调整面板的命中排除**四处；面板挂着但不可见时，状态栏提示
「T 文字工具占着右栏，切走后可见」。这样不存在「画了但点不到」的半截状态。

**5. 操作。** 面板打开时键盘归面板（工具字母键被吞，避免选行时误切工具、也避免
`Del` 落到「删除选中图层」上）：`↑`/`↓` 选行（选到哪滚到哪，`path_ensure_visible`
只滚到刚好够、不居中）、`回车` 把选中路径**重新落成选区**、`Del` 删掉选中路径
（只删列表，选区不动）、`Esc` 关面板。一个例外：**钢笔正在落点**时（面板是上一条
路径留下的）回车/`Esc` 归钢笔——提交或取消当前这条，其余键仍吞掉。鼠标单击行 =
选中，点空白 = 吞掉点击（不落到画布），**双击行** = 转选区（走 `WM_LBUTTONDBLCLK`，
在「双击回退成按下」那条分支之前处理）。

**6. 转选区 = 一步撤销。** `path_apply_selected` 复用钢笔画路径的装配函数
`path_build_cubics`（`pen_segment_count(n, closed) == n`，闭合子路径），再走
`finish_new_selection`——与框选/套索/魔棒/钢笔同一条事务尾巴，因此同样
「恰好一步撤销」，且 `changed == 0` 时事务回滚、不落空步。组合方式固定为
`COMP_SELECTION_REPLACE`（面板里没有加选/减选修饰键，双击不方便按 Shift 的语义）。

**7. 文档替换。** 锚点是旧文档的坐标，新文档里落下去会偏到无关位置。`forget_color_range`
（新建 / 打开 `.comp` / 打开位图三条路径在 `comp_document_destroy` 之前都会调）末尾
一并 `path_store_clear`：关面板、清列表、清选中/滚动/流水号。

**8. 几何单源。** 行高 22、列表顶 64、底部提示 96 写在 `ui_core.h`
（`kPathRowHeight` / `kPathRowsTop` / `kPathPanelFooter`），并给出
`path_visible_rows` / `path_max_scroll` / `path_clamp_scroll` / `path_ensure_visible` /
`path_row_rect` / `path_row_at`。绘制与命中调同一组函数——这是 S15「看得见点不中」类
缺陷的通用防线。

**9. 验证。** `tests/test_ui_core.cpp` 加 `ui_core_path_panel_rows_hit_what_they_paint`：
可见行数随面板高度变化（560 高 → 18 行，120 高 → 0 行）、行矩形与行距、命中上/下边界
与行尾、滚动钳位与 `ensure_visible` 只滚到刚好够。单测 **557 例 / 0 失败**；
`--selftest` **245 项不变**（路径面板纯壳侧、不碰 ABI，基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**。

**10. 边界。** ① 面板不落盘：`Ctrl+S` 存 `.comp` 不带路径（引擎无路径实体，带不上）；
② 不画路径本身（没有「路径作为图形」这条能力），只列名字与锚点数；
③ 不能加选/减选、不能重命名/复制；④ 锚点坐标系不随文档裁剪变化，裁剪后旧路径可能
部分落到画布外（引擎会裁掉，选区变为可见部分）。

### 5.18 历史记录面板（P1，2026-10-21）

路线图里「历史记录面板」的验收口径是「面板列出每一步，点选可回跳，与撤销栈同源」。
之前 `History` 只在状态栏露一个数字（`comp_document_history_stats` 的 undo/redo 深度），
没有任何「看得见的历史」。这一节把撤销栈**原样**列成列表，并加一条「回跳到第 N 个状态」
的入口——**ABI 尾部追加 3 个导出（135 → 138）**，既有枚举/结构体/签名一个不动。

**1. 列的是「状态」不是「步」。** `index 0 = 文档最初状态（新建/打开）`，
`index i = 第 i 步之后的状态`，行数 = `step_count() + 1`。这样列表与 Photoshop
的历史面板同构：最上面一行永远「点回原样」，不用额外记「第 0 步」这种概念。

**2. 高亮没有第二份真相。** 当前行**不单独存**，它就是引擎的 `current_index()`
（= `undo_depth`，`comp_document_history_stats` 已经能给，所以**没有**为「当前位置」
新增 ABI）。后果：撤销 / 重做 / 新操作 / 回跳之后，面板不可能把高亮停在一个不存在的
状态上，也不可能出现「列表说第 3 步、引擎在第 5 步」。壳侧只存一个 `history_scroll`。

**3. 三个新导出（纯尾部追加）。**

| 导出 | 语义 |
| --- | --- |
| `comp_document_history_step_count` | 总步数（不含最初状态）；`out_steps` 可为 `NULL`（此时只做参数校验） |
| `comp_document_history_step_label` | 第 `step` 步的标签；`step == 0` 或越界 → `INVALID_ARGUMENT`。查询口径同 `comp_layer_get_name`：`out_required` **含 NUL**，`buffer == NULL \\|\\| capacity == 0` 为查询模式，容量不足 → `INVALID_ARGUMENT`（绝不静默截断） |
| `comp_document_history_jump` | 回跳到第 `step` 个状态。小于当前 = 连续 `undo`，大于 = 连续 `redo`；相等 = `COMP_OK` 空操作；越界 → `INVALID_ARGUMENT`；**活动事务内 → `COMP_ERR_STATE`** |

**4. 标签的顺序不随撤销漂移。** `History` 的撤销栈是 `deque`、重做栈是 LIFO
（back = 下一步会重做的那条），所以第 `undo_depth + 1` 步对应
`redo_stack_[size - 1 - k]`。`step_label()` 按这个映射取值，于是「A/B/C 三步，
撤销两步」之后第 1..3 步仍报 A/B/C，而不是倒序或串位。

**5. 回跳不新增历史记录。** 引擎内部就是连续调 `Document::undo()/redo()`——它们只
搬栈，不 push 新记录。因此「回跳 → 再回跳」不会把历史越点越长，撤销栈的标签与
字节账本也不变。两条失败路径都被显式挡掉并报错：
① 目标越界（调用方先问 `step_count`）；② 重做过程中 `History::trim()` 把刚推入的
撤销记录挤掉、目标变得不可达（预算过小时可能发生）——这时返回 `COMP_ERR_STATE`
「history was trimmed during jump」，而不是让循环空转或假装成功。

**6. 入口与互斥。** 「编辑(&E)」菜单新增 `历史记录(&H)…`（命令 ID **1052**，
`Ctrl+Alt+H`；`Ctrl+Shift+H` 已被「水平翻转」占用，且它走 `WM_KEYDOWN`，
Alt 组合才落在 `WM_SYSKEYDOWN`）。面板占用右栏 `layout.adjust_panel`，与路径 /
色彩范围 / 效果 / 滤镜 / 文字面板**互斥**：开历史会关掉前四个并 `color_range_cancel`
（先 abort，不能留显式事务），其余四个打开时也会把 `history_open` 置假。

**7. 文字工具占栏。** 与 §5.17 同一条规矩：`history_panel_visible()`
（`history_open && tool != Type`）同时约束绘制、键盘、鼠标、滚轮与调整面板的命中排除；
`T` 占栏时状态栏提示「T 文字工具占着右栏，切走后可见」，不存在「画了但点不到」。

**8. 操作。** 面板打开时键盘归面板（工具字母键被吞）：`↑` / `↓` 回跳一格、
`Home` / `End` 回跳最前 / 最后、`Esc` 关面板。一个例外：**钢笔正在落点**时
回车/Esc 归钢笔（提交或取消当前路径），其余键仍吞掉——否则画布上点了锚点之后
回车会被面板吃掉，路径永远提交不了。鼠标**单击行**即回跳（与路径面板不同：
回跳本身可再回跳、不产生历史记录，误点无破坏性），点空白吞掉不落到画布；滚轮一格 3 行。
面板之后的状态行（重做侧）用暗色画，一眼分出「已发生 / 未发生」。

**9. 刷新口径。** 回跳等于换整篇文档状态（图层、选区、调整都可能变），所以
`history_jump_to` 的刷新与 `undo_document` / `redo_document` 完全一致：
`needs_render` + `refresh_layers` + `refresh_selection`，只是把两次刷新合成一次，
再把目标行滚进可视区。状态栏随后走 `refresh_history_status(prefix)`，保留原有的
「撤销 N / 重做 M / 字节数」尾巴。

**10. 几何单源。** 行高 22、列表顶 64、底部提示 96 与路径面板同款，抽成
`ui_core.h` 的通用 `list_*`（`list_visible_rows` / `list_max_scroll` /
`list_clamp_scroll` / `list_ensure_visible` / `list_row_rect` / `list_row_at`），
`path_*` 与 `history_*` 两组包装都委托给它——路径面板的既有用例原样通过，
历史面板的绘制与命中也调同一组函数。

**11. 验证。** `tests/test_domain.cpp` 加 `history_step_labels_follow_undo_and_redo`
（A/B/C 三步：步号↔标签稳定、撤销两步不倒序、push 丢弃重做链后越界标签消失、
`clear()` 后归零）；`tests/test_abi.cpp` 加 `abi_history_panel_steps_labels_and_jump`
（步数 / 查询模式 required / 容量不足与 `NULL out_required` 被拒 / 前后回跳的像素与
`undo_depth` / 相等回跳为空操作 / 越界被拒 / 事务内回跳 `COMP_ERR_STATE` 且 abort 后可用）；
`tests/test_ui_core.cpp` 加 `ui_core_history_panel_rows_hit_what_they_paint`（可见行数、
行矩形、命中上/下边界与行尾、滚动钳位、`ensure_visible`）。单测 557 → **560 例 / 0 失败**；
`--selftest` **245 项不变**（面板本身不进自检脚本，基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**。

**12. 边界。** ① 只读不写：不能重命名 / 删除单条 / 清空历史（上游面板的右键菜单未做）；
② 标签是写入方给的（壳里是英文操作名），面板不改写成中文；③ 历史被预算裁掉后
列表随之收缩，不提示「更早的记录已丢弃」（状态栏的字节数仍在）；
④ 快照粒度是「一步」，笔迹仍然是每笔一步，没有上游的「合成笔迹」分组。

### 5.19 减淡 / 加深 / 海绵（P1，2026-10-21）

路线图 P1「减淡 / 加深 / 海绵」的验收口径是「三种工具能改像素且受选区约束，撤销一步」。
上游 `RetouchTools.swift` 是工具层逻辑、没有 `.c` 内核，所以这一项在**域层新增算子**
（`tool_ops.cpp` 的色调分支），再在 ABI 尾部追加三个 kind。

**1. 一个工具、八个状态。** `O` 键进 Tone 工具，重复按在
`Dodge/Midtones → Dodge/Shadows → Dodge/Highlights → Burn/Midtones → Burn/Shadows →
Burn/Highlights → Sponge/Desaturate → Sponge/Saturate` 里循环（第 8 档再按回到第 1 档）；
从别的工具切进来一律落在 `Dodge/Midtones`。状态是壳侧**唯一真值**：
ABI 的 `(kind, mode)` 由 `tone_state_tool()` / `tone_state_mode()` 从同一张表换算，
壳内不另存一份，因此不会出现「状态栏说阴影、实际按中间调算」。

**2. 参数增量最小。** ABI 只加 `comp_tool_tone { mode; strength; }` 与三个枚举值
（16/17/18，纯尾部追加，导出数仍 138）。笔迹几何完全复用 `stroke_points` / `brush`，
与 Blur 同一条折线通路；`Alt+[` / `Alt+]` 调强度（0.01..1.0，10% 一步，`clamp` 兜住
0 / 负数 / NaN），`Shift+O` 图层不透明度、`Ctrl+O` 打开、`[` `]` 笔刷都不受影响。

**3. 落笔族。** 三种算子走 `StrokeKind::Tone`，与画笔同一条 `StrokeShape` 覆盖率与
「不叠加」口径；`is_stroke_family` 因此扩到这三种 kind——RGBA16 / RGBA16F 上只有
真正被改动的像素拿深值，笔尖之外保持原深度（与 Photoshop 一致）。

**4. 空撤销防线。** 三种运算都可能算出「逐字节不变」：海绵加色碰到纯灰、
减淡碰到纯白。实现逐通道**量化后与旧值比较**，没有字节变化就不写、全笔都没变化时
`txn_abort` 并返回 `COMP_OK`，状态栏显示引擎 note
`tone stroke changed no pixel (arithmetically a no-op)`——`Ctrl+Z` 撤的是上一笔，
不会撤出一步空操作。`strength == 0` 更早：在参数闸就以 `INVALID_ARGUMENT` 拒绝。

**5. 选区约束。** 与渐变 / 形状 / 文字同口径：在文档分辨率取
`selection_effective_coverage()` 并乘进笔迹覆盖率，画布外与覆盖率为 0 的像素
逐字节不动；外层坐标用 `effective_bounds()` 的原点映射。一条横跨选区边界的笔迹
只有选区内一侧变化。

**6. 入口与工具表。** `ToolId::Tone` 插在 `Retouch` 之后（工具栏第 14 个按钮，
`tool_table` 仍是工具条 / Tools 菜单 / 快捷键的唯一来源，索引全部由表生成）；
工具条与状态栏都显示当前档名与强度。M1 的工具顺序因此变为
`A V M L P W C I B E J S R O G U T H Z`（19 个）。

**7. 验证。** 领域层 4 例、ABI 层 3 例、UI 层 1 例，单测 560 → **568 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M23**。

### 5.20 历史记录画笔（P1，2026-10-21）

路线图 P1「滤镜库扩容 + 历史记录画笔」里可独立交付的一半。历史快照与回跳在 §5.18
已经具备，缺的只是「从第 N 个状态取样，随笔迹刷回当前层」。

**1. 取样源 = 历史面板下标，不是撤销步数。** 引擎直接读 `History::state_at(index)`，
因此取样与撤销栈同源，壳内没有第二份快照。下标越界 / 负数在参数闸就
`INVALID_ARGUMENT`；下标合法但那一份状态里没有当前图层（图层是后加的）→ `STATE`。
两种拒绝都不开事务、不改像素、不留撤销步。

**2. 自动 / 显式两种取样。** 壳侧只存一个 `history_brush_state`：`-1` = 自动 =
当前状态的上一格（「刚画错一笔，拿这支笔抹掉」的直觉路径），`>= 0` = Shift+点历史
面板某行显式指定。显式路径**不回跳**——当前画面必须停在最新，否则这支笔就失去意义；
面板里该行显示 `← 画笔取样`（琥珀色），与「当前状态」的高亮（蓝底）互不覆盖。
普通点行仍是 §5.18 的回跳，两者互不干扰。

**3. 一笔 = 一个撤销步。** 沿用 `StrokeKind` 通路（新增 `HistoryBrush`），松手时
一次 `comp_tool_apply`；`ReleaseCapture` 之前先把折点搬走（S15 的同一条防线）。
取样状态**只在提交时读一次**，笔画途中打开/回跳面板不改变这一笔的来源。

**4. 失败与 no-op 都说清楚。** 引擎的三种「成功但没改像素」（笔尖不相交 / 取样
逐字节相同 / 被选区整片遮住）都带 note，状态栏直接显示，且不落撤销步；取样的
状态里没有该图层时状态栏写「历史记录画笔失败：取样第 N 个状态（该状态下可能没有
此图层）」——把取样下标写出来，用户才知道该换一格。

**5. 入口。** `ToolId::HistoryBrush` **追加在工具表末尾**（`Y`，与上游一致），
旧工具下标全部不动；`tool_table` 仍是工具条 / 菜单 / 快捷键的唯一来源。
切到该工具时自动打开历史面板（右栏单挂位，其余面板按 §5.16/§5.17 的规则让位）。
`Alt+[ / ]` 调不透明度（10% 一步，键盘下限 10%，因为引擎拒绝 0）；状态栏与工具栏
都常显「取样 #N / 不透明度 X%」。

**6. 验证。** ABI 层 2 例（经 `comp_tool_apply` 的取样刷回 + 恰好一步撤销、
参数闸先于事务且 no-op 不留步），领域层 1 例（RGBA16 笔尖外保深 + 两次撤销逐字节
复原），UI 层 1 例（工具下标 19 / `StrokeKind` 值 / 不透明度钳位与步进）。单测
568 → **575 例 / 0 失败**；PE 导出 **138 不变**（本项零新增导出）；壳自检
**245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py
--require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M24**（M1 的工具顺序改为 20 个）。

### 5.21 图像菜单：转画布 / 修边 / 反相（2026-10-21）

**缺口是接线，不是能力。** `comp_document_rotate_canvas`、`comp_document_trim`、
`comp_layer_invert` 在 §6.35 就已在引擎与 ABI 就位并有用例，但 `shells/windows`
里零调用、顶层菜单也没有「图像」项——`grep comp_canvas_` 在壳目录无命中。本项按上游
`Image` 菜单补齐五条产品通路：旋转（CW / CCW / 180°）、修边、反相。**零新增导出**
（PE 仍 138），命令 ID 1053..1057 追加在既有 ID 段之后。

**1. 旋转不重采样，壳只负责换坐标系。** 引擎只换画布尺寸与图层落位，像素字节不动
（`comp_canvas.h` 贯穿口径），因此壳侧不复制像素、不自行换算。统一的
`finish_canvas_command` 重取 `comp_document_size` → `shell.width/height`，
置 `needs_fit`（旧偏移对应旧画布）与 `needs_render`，再从引擎回读图层与选区——
90° 会转置选区蒙版，**壳侧缓存一律作废而不自己推**。撤销深度由
`refresh_history_status` 从引擎取。

**2. 四种结果分开说。** 参数 / 图层位置非法 → 「画布旋转失败…（未改动）」；
`changed == 0`（空文档且旋转后尺寸不变）→ 明确写「未产生撤销步」，不假装成功；
正常 → 「已旋转画布：顺时针 90°/逆时针 90°/180°」+ 撤销计数。旋转会先收掉修边面板：
面板里的四条边是按旧画布算的，留在屏幕上只会误导。

**3. 修边面板是可编辑模型，不是实时预览。** `ui_core.h` 的 `TrimPanel` 六行
（Based On / Top / Bottom / Left / Right / Tolerance），无堆分配，行几何
`trim_row_rect` 同时供绘制与命中——沿用滤镜 / 效果面板「看得见就点得中」的规矩。
Tab / Shift+Tab 切行（夹住不环绕）；`-` / `=` 对基准行循环三档、对四条边是开关、
对容差 ±1 并夹在 0..255；数字键直接把焦点跳到容差行并进入键入态（最多 3 位，
`Esc` 取消、`Backspace` 删字符）。**面板值 → `comp_trim_options` 只在
`build_trim_options` 一处映射**，行序与 ABI 字段一一对应。

**4. 拒绝路径全部有具体文案。** 四边全关在本地就拦（「至少勾选一条边」），
比引擎的 `INVALID_ARGUMENT` 更好读；`STATE`（空文档 / 整篇没有可保留内容）、
`changed == 0`（算出来的边框等于当前画布）都保持面板打开、不产生撤销步，
用户可以直接改参数再来。成功才关面板并走 `finish_canvas_command`，
状态栏写明「基于哪种基准 · 容差多少」。

**5. 反相：破坏性落墨，与调整层并存。** 目标 = 当前选中图层（组 / 无像素 →
`STATE`，HDR → `UNSUPPORTED`，两者都不改像素、不落步）。预乘语义由引擎负责
（`c → alpha - c`，透明像素仍透明）；活动选区按文档坐标混合，显式空选区
`changed == 0` → 「选区为空，未改动」。16 位图层按引擎口径先落 8 位工作副本、
写回后**永久变 RGBA8**（撤销可恢复原 16 位快照），状态栏不另作提示。
快捷键 `Ctrl+I` = 反相，`Ctrl+Shift+I` 仍是反选——两者共存，与上游一致。

**6. 面板互斥。** `trim_open` 进入右栏单挂位：打开时取消色彩范围的预览事务、
关掉滤镜 / 效果 / 路径 / 历史；反过来说，打开那五个面板也会关掉修边
（`trim_begin` 与各面板的开启路径同侧收口）。修边面板拥有键盘时，非 `Ctrl`
按键一律吞掉（与 §5.16/§5.17 一致），只有 `Ctrl+…` 快捷键穿透。

**7. 验证。** UI 层 2 例（`TrimPanel` 循环 / 钳位 / 重置 / 名称表；行几何命中
首末行、行外与空行数）。单测 586 → **588 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py
--require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M25**。

### 5.22 仿制图章（`S`）接线（2026-10-21）

**第三个「引擎有、壳没接」的工具。** 工具栏从工具栏表第一版就有 `Clone Stamp`
（键位 `s`），`comp_tool_op` 里 `COMP_TOOL_CLONE` 与 `comp_tool_clone` 也早已存在，
但 `shells/windows` 中 `ToolId::Clone` **没有任何分支**：点画布会一路落到
`canvas_click`，只往状态栏写一行文档坐标。本项把源点、落笔、对齐与反馈补齐，
**零新增导出**（PE 仍 138）。

**1. ABI 是两段式的：源点与偏移都由宿主保存。** 引擎不记「上一次的源点」：
`has_offset == 0` 时按 `round(source - first_point)` 现算首笔偏移并只用在这一笔；
`comp_tool_apply` 的 desc 是 `const`，算出来的值不会回填。因此壳必须在首笔成功后
自己算出同一组整数（`comp_ui::clone_stroke_offset`，`std::lround` 与引擎 `round`
同口径、.5 远离零），后续笔画带 `has_offset = 1` 复用，`aligned = 1` 才成立。
用 `static_cast<int>` 截断会让首笔差半像素，症状是「第一笔正确、之后每一笔整体偏
一格」——归因成本极高，所以这条规则放进 `ui_core.h` 并有单测钉住。

**2. 交互与状态机。** Alt+点击画布 = 落源点（吸附到整数像素，清零偏移），
不进入笔迹、不落撤销步；Shift+Alt+点击 = 把 `sample_all_layers` 置为整篇合成并保持
（否则只取目标图层），随时切换走 `Shift+K`（§5.31）。普通按下 / 拖动 =
`StrokeKind::Clone` 笔迹，单击也是一记 dab。
未设源点时按下不捕获，状态栏直接写「先 Alt+点击画布设置源点」。
源点在一个工具会话内保留（切走再切回仍有效，与上游一致）；**换文档必须复位**：
新建 / `load_document` / 拖入位图三条路径都调 `clone_reset_source`——旧文档的源点
在新像素网格上没有意义。

**3. 提交参数与画笔同源。** `stroke_points` + `brush`（直径 / 硬度 / 满压 / flow 1 /
seed 0）与画笔、Retouch、历史记录画笔完全一致；`clone.opacity` 显式给当前选项值
（默认 `1.0`；ABI 零初始化 = opacity 0 = 逐字节不动，漏掉就「工具像坏了」）。
折线仍是**先 swap 再 `ReleaseCapture`**（S15：释放捕获会同步派发
`WM_CAPTURECHANGED` → `stroke_cancel` 清空点集），失焦 / 捕获丢失走同一个取消路径。

**4. 源点可见。** 画布按文档坐标画十字（`doc_to_screen` 每帧现算，缩放 / 平移后
跟着画布走），只在选中该工具且已设源点时出现；工具栏与状态栏常显
「源 x,y / 图层|合并」，未设源点写「未设源点」。笔迹拖影复用既有折线预览
（非落色笔迹统一高对比青）。落笔后文案含折点数、源点、取样口径，引擎有 note 时
附在「｜」后，再 `refresh_history_status` 回读撤销计数。

**5. 验证。** UI 层 1 例（偏移四舍五入：89.6→90、29.4→29、±0.5 远离零、
-2.5→-3 与 -2.4→-2、空指针安全；`StrokeKind::Clone` 与其余取值互不相同）。
单测 588 → **589 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。目视项见 `docs/manual-test.md` **M26**。

**6. 仍未做。** ① Spot Healing（`J`）已接线，见 §5.25；
② 不透明度与「对齐」开关已在 §5.26 补齐；③ `sample_all_layers` 的持久勾选态已在
§5.31 补齐（`Shift+K`）。

### 5.23 缩放工具（`Z`）接线（2026-10-21）

**按钮早就有，行为没有。** `ToolId::Zoom` 从工具栏表第一版就在（键位 `z`），
`ui_core::CanvasView::zoom_at / zoom_by` 也早被单测覆盖（锚点不动、2%..3200% 夹取），
但壳里 `ToolId::Zoom` **没有任何分支**：点画布落到 `canvas_click`，缩放只能走
`Ctrl+= / Ctrl+- / Ctrl+0 / Ctrl+1`。`docs/manual-test.md` M12 曾把它记成已知边界。

**1. 语义。** 点击 = 放大 1.2 倍，`Alt+点击` = 缩小 1/1.2；锚点是**点击点**，
所以连点时该点下方的文档像素不动（`zoom_by` 的既有语义，复用它而不是自己算 pan）。
`needs_fit = false`：否则下一次窗口尺寸变化会把视图重置回适合窗口。
纯视图操作——不进撤销栈、不改文档、不碰选区、不写任何图层。

**2. 到顶必须说话。** 尺度夹取在 `ui_core`，到 2% / 3200% 后再点，
`zoom_at` 算出的仍是同一个值。壳侧比较点击前后的 `zoom`，相等时写
「已到上限 3200% / 下限 2%，画面不再变化」——夹取本身已保证画面不位移，
但状态栏不给反馈的话，按钮看起来就是坏的。

**3. 分发顺序。** 分支放在 Hand / 空格平移之后、所有绘制工具之前：
空格临时抓手优先于 `Z`（PS 同语义），`Z` 又先于渐进轴 / 裁剪 / 选框 / 画笔等，
避免「某个面板开着 + 选了 Z」时点击被别的工具吃掉。选中工具时状态栏给出
「点击放大 1.2 倍，Alt+点击缩小；Ctrl+0 适合窗口 / Ctrl+1 实际像素」。

**4. 验证。** 无新增单测：本项零新增纯模型代码，`zoom_at` 锚点不变与
`clamp_zoom` 上下限在原 `test_ui_core` 用例里已覆盖。单测 **589 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M12 步 6–8**。

**5. 仍未做。** ① 拖出矩形缩放（marquee zoom）到该区域；② 双击画布 = 实际像素；
③ 缩放光标（放大镜 +/- 形态，壳目前统用箭头）；④ 拖动时连续缩放（上游是点按式）。
①–③ 已在 §5.27 补齐，只剩 ④。

### 5.24 Move 工具拖拽图层（2026-10-21）

**1. 分流规则。** `move_begin` 按下时看 `shell.selection_active`：有选区 → 走原路径
（搬选区像素，松开时一次性提交，预览靠蚂蚁线跟随）；无选区且有选中图层 →
走新路径 `layer_move_begin`。两者互斥，不会出现「像素和图层一起动」。
（§5.36 起有选区时**再按落点细分**：落点在选区外改走整层拖动，选区蒙版不动。）
~~组图层直接拒绝（「组图层不参与 transform 合成，暂不支持整组拖动」）~~ ——
已在 §5.34 改为平移子树成员；组自己的 transform 依旧不写，因为
`filter_graph` 只烘焙成员、组节点不读自己的 transform，写进去是静默无操作。

**2. 一次拖动 = 一步撤销。** 按下时 `comp_txn_begin(document, "Move Layer")`；
每次移动用 `base + (doc - start)` **重新推导**目标矩形再调
`comp_layer_set_transform(document, id, target, rotation)`——不在上一帧结果上累加，
来回拖动不会因取整漂移；写的是真 transform，旋转过的图层由渲染管线直接画对，
不需要额外的几何预览代码。松开 `comp_txn_commit`；`dx == dy == 0` 则
`comp_txn_abort` 并写「位置未变化」，不留空撤销步。

**3. 先清状态，再 `ReleaseCapture()`。** 捕获变更会同步重入
`WM_CAPTURECHANGED → move_cancel`，顺序反了会把刚提交的事务 abort 掉——
与调整滑杆的 `*_drag_finish` 是同一条规矩。

**4. 换文档必须比对句柄。** `move_cancel` 在打开位图 / 新建 / 关闭三条路径上
都在旧文档 `destroy` **之后**才被调用；此时 `shell.document != shell.layer_move_doc`，
若照旧 `comp_txn_abort` 就是 UAF。守卫条件与 `forget_color_range` 的注释同源。

**5. 取消与调用点。** `Esc` / 失焦 / 捕获丢失 → `move_cancel`（abort 回滚，
「已取消移动，图层位置未改变」）；三个入口都做两条路径的清理，不会留下 active 残标。
`WM_MOUSEMOVE` 与 `WM_LBUTTONUP` 的分发条件都已放宽为
`move_active || layer_move_active`（否则图层拖动收不到移动与提交）。

**6. 验证。** 纯外壳改动，无新增单测。单测 **589 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M27**。

**7. 仍未做。** ~~① `Alt` 拖动 = 边移边复制图层~~ —— 已在 §5.29 补齐；
~~② 方向键微调~~ —— 已在 §5.30 补齐（Move 工具下无修饰方向键）；
~~③ 拖出画布的位置提示~~ —— 已在 §5.32 补齐（状态栏）；
④ ~~旋转 / 缩放手柄（transform 的尺寸与角度目前只能由引擎侧命令改）~~
—— 已在 §5.39 补齐（Move 工具的单选图层显示 8 个手柄 + 旋钮）；
⑤ ~~PS 那个「拖选区外 = 移图层」的细节本项不模拟（有选区就只搬像素）~~
—— 已在 §5.36 补齐（有选区时按落点分流）；
~~⑥ 组图层整体移动~~ —— 已在 §5.34 补齐（沿 `parent` 链平移子树成员）。

### 5.25 Spot Healing（`J`）接线（2026-10-21）

**1. 缺口。** `J` 从工具栏第一版起就存在、`tool_has_brush_tip` 也已经把它算进画笔，
但壳里没有任何调用点：按下拖动落到 `canvas_click`，只写文档坐标。
引擎侧 `COMP_TOOL_HEAL` 早已完整（PatchMatch、三种模式、不透明度、`seed` 决定确定性），
难点全在宿主：它要的是**整层像素空间的 8 位蒙版** `heal.coverage`，不是一条折线。

**2. 宿主规则：折线 → 蒙版。** 笔迹折点仍由既有 `stroke_begin / stroke_extend` 收集
（文档坐标）。松手时对每个折点做与引擎 `Layer::map_document_to_pixel` **同一套**逆变换
（归一化 → 逆旋转 → 翻转 → 像素坐标），再在图层像素空间里沿折线按
`spacing = max(0.75, radius * 0.25)` 细分、逐点盖软圆章（`inner = radius * hardness`，
从 `inner` 到 `radius` 线性衰减，同一点多次覆盖取 `max`）写进蒙版。
因此硬度/直径直接复用画笔的 `[` `]`、`Shift+[` `]`，不新增第二套参数。

**3. 一次一笔 = 一步撤销。** 蒙版光栅化完只调一次 `comp_tool_apply(COMP_TOOL_HEAL)`，
`opacity`、`mode`、`seed = 1` 一并传入；引擎的 `spot_heal` 按
`coverage × opacity` 替换像素。撤销栈只加一步，`Ctrl+Z` 整笔回退。

**4. `heal_coverage_rows`（ABI 尾部追加）。** 域层的 heal 闸只校验
`coverage_stride >= 图层宽`，看不到宿主缓冲**到底有几行**，而执行时按
**图层像素高**逐行 `memcpy`。图层像素尺寸并不总是等于画布：`comp_io_import_layer`
会按解码图的原始尺寸建像素。图层比画布高时，宿主按画布分配的缓冲会被读越尾。
修法是尾部追加 `size_t heal_coverage_rows`（既有字段布局不动、导出数 138 不变），
宿主填实际行数；ABI 在写入前发现 `0 < rows < 图层高` 直接返回
`COMP_ERR_INVALID_ARGUMENT`，壳把它翻成「图层像素尺寸大于画布，当前 coverage 装不下」。
`0` = 旧宿主按图层高信任，行为与追加前完全一致，既有测试不受影响。

**5. 键位。** 重复按 `J` 在三档算法里循环（Content-Aware / Create Texture /
Proximity Match），从别的工具切进来一律回到 Content-Aware；`Alt+[ / ]` 调不透明度
（10% 一步，下限 10%，与历史记录画笔同一口径）。取消走通用 `stroke_cancel`
（`Esc` 关闭、失焦、捕获丢失），它只清 `stroke_kind` 与折点，没有 Heal 专属残留状态。

**6. 验证。** 纯外壳 + 一个尾部字段，无新增单测。单测 **589 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M28**。

**7. 仍未做。** ~~① 修补**不看当前选区**（蒙版只由笔迹决定，PS 会叠选区）~~ ——
已在 §5.28 补齐（只剩「选区不限制采样源」这一点差别）；
② 非等比缩放的图层用 `(scale_x + scale_y) / 2` 近似半径，宽度与高度比例悬殊时有偏差；
③ 拖动中没有蒙版预览，只有松手后一次性落墨；④ 没有「采样全部图层」开关
（固定 `mode` 自带的取样口径）；⑤ 蒙版坐标按**画布**尺度解释：图层像素尺寸与画布
不一致的导入层（ABI 没有像素尺寸读口），笔迹落点会整体缩放到画布范围；
装不下时（层更宽/更高）由 stride / 行数闸明确拒绝，不会越界。

### 5.26 仿制图章：不透明度与对齐开关（2026-10-21）

**1. 缺口。** §5.22 接线时把 `clone.opacity` 写死 1.0、`aligned` 写死 1，
选项栏那两项能力在壳里没有入口，§5.22 的「仍未做」里记着这两条。
引擎侧两个字段本来就在 `comp_tool_clone` 里，无需新导出。

**2. 不透明度（`Alt+[ / ]`）。** 与历史记录画笔、修补同口径：10% 一步、下限 10%
（引擎的闸允许 0，但 0 = 什么都不画，对外壳用户是无反馈的死调节）。
值存在 `ShellState::clone_opacity`，跨文档保留；每笔提交时
`clamp_clone_opacity` 后再写进 `desc.clone.opacity`。

**3. 对齐（`Shift+S`）。** PS 把「对齐」放选项栏，本壳没有选项栏，用同一字母的
Shift 组合最省记忆；不带 Shift 的 `S` 仍是选工具。语义直接落到 ABI：
- 开（默认）：首笔不带 `has_offset`，成功后壳用 `clone_stroke_offset` 存下同一组
  整数，之后每笔带 `has_offset = 1`；
- 关：每笔都不带 `has_offset`，引擎按 `round(source - 首笔落点)` 现算，
  即「每一笔都以源点重新对位」。

**4. 关掉时必须清偏移。** `aligned = 0` 时引擎不读 `has_offset`，所以不清也「能跑」，
但重新打开对齐后会复用关掉之前的旧偏移——症状是「开关了一轮，落笔突然跳到旧位置」。
因此 `Shift+S` 关掉对齐的同一次按键里就 `clone_has_offset = false`，
重新打开后的下一笔由现算重新建立。

**5. 状态栏。** 工具选中时显示「Shift+S 切对齐（开/关），Alt+[ / ] 调不透明度（N%）」；
切换对齐与调不透明度都有独立提示；落笔文案补「对齐 / 不对齐 · 不透明度 N%」，
便于目视核对参数确实生效。

**6. 验证。** 纯外壳改动，无新增单测。单测 **589 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M26** 补步 13–17。

**7. 仍未做。** 无。`sample_all_layers` 的持久开关已在 §5.31 补齐（`Shift+K`），
与上游选项栏的差别只剩「选项不落盘」。

### 5.27 缩放工具补齐：拖框 / 双击 100% / 放大镜光标（2026-10-21）

**1. 范围。** §5.23 的「仍未做」①–③ 一次做完，零新增 ABI 导出：
① 拖出矩形缩放到该区域；② 双击画布 = 实际像素；③ 放大镜 +/- 光标。

**2. 纯模型先行。** `CanvasView::zoom_to_doc_rect(x0, y0, x1, y1, canvas, doc_w, doc_h,
margin = 0.98)` 与 `fit` 并列：区域按 margin 铺满画布、区域中心移到画布中心。
不复用 `zoom_at`——`zoom_at` 的契约是「锚点不动」，这里要的恰恰是「把中心搬过来」，
所以直接 `pan = -(rect_center - doc_center) * zoom`。两点先归一化；宽或高不为正
返回 `false`，调用方回落到单击；缩放仍经 `clamp_zoom`（2%..3200%）。

**3. 拖框状态机。** `zoom_marquee_begin` 只记录**屏幕坐标**起点并 `SetCapture`。
拖框期间视图不变，屏幕矩形就是用户看到的矩形，预览直接画、不必每帧反算文档坐标；
松手时位移 `< 4 px` 视为单击 → 原 `zoom_tool_click`（放大 1.2 / Alt+缩小 1/1.2），
否则两角 `screen_to_doc` 后交给 `zoom_to_doc_rect`。预览是白实线 + 黑虚线双描边
（与选区预览同一套）。`WM_CAPTURECHANGED / WM_KILLFOCUS` 清掉
`zoom_marquee_active`；没有半提交状态可回滚。

**4. 双击 100%。** `WM_LBUTTONDBLCLK` 分支放在最后「还原成普通按下」之前，
只调 `actual_pixels()` 并写状态；否则双击会退化成两次 1.2 倍放大。
第一次点击仍会先放大 1.2 倍再被双击复位，净结果 100%（消息顺序决定，不影响结果）。

**5. 光标。** 系统光标表没有放大镜，用 32×32 单色光标自绘：AND/XOR 位平面
（`AND=1,XOR=0` 透明、`00` 黑、`01` 反色白），逐像素判定圆环（黑边 + 白环、
玻璃透明）、手柄线段、玻璃内横线（缩小）/十字（放大）。32 px 宽正好 4 字节/行，
无需行尾补齐。`WM_SETCURSOR` 只在「缩放工具 + 画布内 + 未平移」时接管，
`Alt` 换缩小形态；平移保持抓手。`CreateCursor` 失败返回 `nullptr` → 默认箭头。

**6. 验证。** 新增 1 个单测（铺满比例、居中、反序等价、上下限夹取、退化输入不改视图），
单测 **590 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。目视项见 `docs/manual-test.md`
**M12 步 9–13**。

**7. 仍未做。** ④ 拖动过程中连续缩放（与上游同为松手结算）；拖框不支持拖出画布
边缘自动平移；单色光标不跟随深色主题（反色设计在任何背景上都可见）。

### 5.28 修补工具按当前选区裁剪（2026-10-21）

**1. 缺口。** §5.25 的遗留 ① ：有选区时修补仍涂满整幅画布，而画笔、橡皮、
仿制图章都受限。原因不在壳的调用点写错，而在**责任划分**：别的工具把折线交给
`comp_tool_apply`，引擎在自己的落笔路径上统一叠选区闸；修补是壳侧自己光栅化
**成品蒙版**再交给 `COMP_TOOL_HEAL`，引擎内 `spot_heal` 直接写图层像素
（签名里根本没有选区参数），没有可叠的东西。所以这个闸**只能由壳侧补**。

**2. 纯模型先行。** `comp_ui::intersect_coverage_with_selection(coverage, mask, count)`
逐像素 `(coverage * mask + 127) / 255`：`255*255` 仍是 `255`（全保留不产生变形），
`+127` 是四舍五入而非截断，软边叠一次不会整体变暗一档。任一指针为空返回 `false`
且一个字节都不写；`count` 由调用方保证两边等长。

**3. 接线位置。** `heal_stroke_end` 里顺序是：光栅化 coverage → **叠选区** →
拼 `comp_tool_op_desc` → `comp_tool_apply`。必须在光栅化之后（蒙版尺寸这时才确定）
且必须在调 ABI 之前（引擎拿到的就是最终蒙版）。选区蒙版本来就按
`shell.width × shell.height` 维护，与 coverage 同坐标系，不需要任何重采样。

**4. 尺寸不一致时不假装。** `selection_mask.size() != coverage.size()` 理论上不该出现；
真出现时**不做缩小/截断近似**，而是让状态栏写「选区未裁剪（蒙版尺寸不一致）」。
静默按最小长度裁会把蒙版和像素错位，越界则更糟——宁可让用户看到没生效。
结果以 `selection_note` 后缀并入两条成功文案，而不是中途 `set_status`
（后者会被紧接着的成功提示覆盖，用户根本看不到）。

**5. 提示。** 工具选中时的状态串补「有选区时只修补选区内」；落笔成功后按情况追加
「· 按选区裁剪」/「· 选区未裁剪（蒙版尺寸不一致）」，便于目视确认分支真的走对了。

**6. 验证。** 新增 1 个单测（`ui_core_coverage_intersects_selection_mask`：
255/128/0 三档、`200×64 → 50`、零 coverage 恒零、全掩版恒等、空指针返回 false
且不写字节），单测 **591 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。目视项见 `docs/manual-test.md`
**M28 步 12–13**。

**7. 仍未做。** 选区只限制**目标**，不限制**采样源**：`spot_heal` 内部按补丁重采样，
选区外的像素仍可能被当作来源（PS 的选区同时限制取样）。要一致得把选区传进
`spot_heal`，属于引擎接口变化，不在壳侧范围内。

### 5.29 Move：Alt 拖动 = 复制图层再移动（2026-10-21）

**1. 范围。** §5.24 遗留 ①。有选区时的**像素搬移**早有 `Alt = 复制`
（`move_duplicate` → `comp_selection_move_pixels`）；缺的是无选区时**整层拖动**那条路。
零新增 ABI 导出（`comp_layer_duplicate` 既有）。

**2. 复制推迟到首次位移。** `layer_move_begin` 只把 `Alt` 取样成 `layer_move_alt`
（途中松开不改变语义），真正的复制在 `layer_move_extend` 里第一次 `dx/dy` 非零时做。
这样「Alt 点一下不拖」不会在画布上凭空多出一层，事务 abort 也能干净收场，
不需要「先建再删」的补偿路径。

**3. 一步撤销。** 复制发生在 `comp_txn_begin` **之后**：`comp_layer_duplicate` 内部的
`ImplicitTxn` 只在没有活动事务时才 own（`owns_ = !document->txn_active()`），
所以它加入已开启的显式事务，松手 `commit` 后 `Ctrl+Z` 一次同时撤掉新图层与位移。
事务名按 Alt 分成 `Duplicate & Move Layer` / `Move Layer`。

**4. 基准与选中项切到副本。** 复制后回读**副本自己的** transform 当拖动基准
（引擎若挪过副本，起点不跳）；`refresh_layers` + `panel_select_layer_id(copy_id)`
把面板选中项挪到副本，后续拖动与快捷键都作用在副本上。复制失败则
`layer_move_alt = false`，本拖退回移动原图层，不假装复制过。

**5. 清理。** `layer_move_end` / `move_cancel` 都清两个新标志；`!dirty` 分支若复制已发生
（复制成功但 `comp_layer_set_transform` 失败），先 `refresh_layers` 再 abort，
避免面板留着已被回滚的 id。工具选中文案补 Alt 语义。

**6. 验证。** 纯外壳改动，无新增单测。单测 **591 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M27 步 12–15**。

**7. 仍未做。** 仍在 §5.24 清单里：~~② 方向键微调~~（已在 §5.30）；
~~③ 拖出画布的位置提示~~（已在 §5.32）；
④ 旋转/缩放手柄；⑤「有选区时拖选区外移图层」；~~⑥ 组图层整体移动~~（已在 §5.34）。

### 5.30 Move：无修饰方向键微调 1 px（2026-10-21）

**1. 缺口。** §5.24 遗留 ②。S21 已有 `Shift+方向 = 1 px` / `Ctrl+方向 = 10 px`，
但上游 V 的无修饰方向键才是主力（1 px）；壳里 `←/→` 无修饰走空分支、`↑/↓` 被
「选图层」占着，等于 Move 工具下没有微调入口。

**2. 只在 Move 工具下接管。** `tool == Move` 且无修饰时，四个方向键都改成
`panel_move_layer(±1, 0 / 0, ±1)`；其它工具一字不改（`←/→` 仍无动作、`↑/↓` 仍选图层）。
理由：`↑/↓` 换图层是壳里唯一不用鼠标的换层方式，全局让位给 1 px 微调不划算；
Move 工具下用户正在摆图层，与上游语义一致。`Shift` 维持 1 px、`Ctrl` 维持 10 px，
不跟随上游把 Shift 改成 10——两个步长是全工具共用的口径。

**3. 组图层如实拒绝（§5.34 起改为平移组成员）。** `panel_move_layer` 补组判断：组节点不读自己的 transform，
此前菜单与 Shift+方向打上去是「状态栏说成功、画面没动」。现在回
「组图层不参与 transform 合成，暂不支持整组移动」，与 Alt 拖动同一措辞。
状态栏文案改为「图层位置：(x, y)（方向键微调）」。

**4. 验证。** 纯外壳改动，无新增单测。单测 **591 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M27 步 16–17**。

**5. 连续微调合并成一步撤销（S42）。** 引擎的事务语义决定了合并方式：`comp_txn_begin`
取快照、`comp_txn_commit` 才压一条撤销记录，所以**一个事务 = 一步撤销**。此前
`panel_move_layer` 每按一次就自开自交，按住方向键（自动重复 = 一串 `WM_KEYDOWN` +
**一个** `WM_KEYUP`）会把历史面板刷成几十步。现在同一段按键重复共用一个显式事务：

- `ShellState` 加 `nudge_txn` / `nudge_anchor`（事务归属的图层 id）/ `nudge_dirty`
  （事务内是否真的写过）。
- `nudge_begin(shell, anchor, label)`：锚点相同就复用未提交的事务；锚点换了先 flush 再开新的。
  三条分支（单层 / 多选 `Nudge Layers` / 组 `Nudge Layer Group`）都改成走它。
- `nudge_flush()`：没有事务返回 false；`nudge_dirty == false` 时 **abort**（空事务不该占
  一步撤销）；否则 commit 并返回 true，调用方据此刷新历史面板。
- 收口点选 `WM_KEYUP`（方向键），不选定时器：上游的边界是「松键」，`WM_KEYUP` 天然给出
  这个边界，而定时器还得处理「按着键切走窗口」之类的情况，反而更容易把事务拖长。
- 反方向的保险：任何**可能是另一件事**的入口在开自己的事务前都先 flush ——
  `run_shell_command` 入口（菜单项每次仍各自一步）、`WM_INITMENU`、鼠标按下/双击/中键/
  滚轮、`WM_KILLFOCUS`、`WM_DROPFILES`、`WM_CLOSE`。否则菜单撤销会撞上「已有事务」被拒。

状态栏文案改为「图层位置：(x, y)（方向键微调，松键合并为一步）」。

**6. 验证（S42）。** 纯外壳时序，没新增纯函数、没新增单测：单测 **599 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项 / 失败 0**（基线 SHA256 仍 `26b3e910…09423`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
冒烟脚本 `%TEMP%\nudge_smoke.ps1` 连发 5 次 `WM_KEYDOWN`(→) + 一次 `WM_KEYUP`，
再 Undo/Redo、菜单微调 ×2 + Undo，进程存活且窗口仍响应（`alive … = True` /
`responding = True`）——只能证明没有死锁/崩溃；合并粒度本身进目视项
`docs/manual-test.md` **M38**。

### 5.31 仿制图章：取样范围做成持久开关（2026-10-21）

**1. 缺口。** §5.22 遗留 ③ / §5.26 遗留 ⑦。`sample_all_layers` 只有
`Shift+Alt+点击` 一条入口，且 `clone_pick_source` 无条件写 `= shift_held`——
换一次源点就把口径清回「当前图层」。上游是选项栏里的持久勾选，壳没有选项栏。

**2. 键位 `Shift+K`。** 复用 `Shift+S` 切对齐的记法：`Shift + 工具字母 = 工具选项`，
只在仿制工具激活时拦截。不用 `Shift+A`（Sample All 首字母）因为 `a` 是 Select 工具的
键位，拦截会把「按 A 回选择工具」抢走；`k` 不在 `tool_table` 里。`Alt+字母` 也走不通：
它由 `WM_SYSKEYDOWN` 派发，`WM_KEYDOWN` 分支收不到（与 §6.56 的 Alt 拖动同一个坑）。

**3. 两处语义修改，零新增状态。** ① `clone_toggle_sample_all(shell)` 翻转
`clone_sample_all` 并回一条带去向的提示；② `clone_pick_source` 改为「按住 Shift 才
置真」——`Shift+Alt+点击` 仍可直接置为整篇合成（并保持），不带 Shift 的换源点则保留选项。
换文档仍由 `clone_reset_source` 连同源点一起复位（源点属旧文档的像素网格）。
读取点不变：`clone_stroke_end` 的 `desc.clone.sample_all_layers`，所以切换对下一笔生效，
已提交的笔画不受影响。工具栏「源 x,y 图层/合并」每帧现读，无需刷新。

**4. 验证。** 纯外壳改动，无新增单测。单测 **591 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M26 步 18–21**。

**5. 仍未做。** 选项不落盘：退出重开回到「当前图层」（上游写进偏好设置）。

### 5.32 Move：拖出画布时给出位置提示（2026-10-21）

**1. 缺口。** §5.24 遗留 ③ / §5.29 遗留 ③。图层被拖到完全离开画布后画面上一片空白，
状态栏仍只写 `(x, y)` 与位移，用户会以为图层丢了（其实还在面板里）。

**2. 判定是纯函数。** `ui_core::layer_rect_touches_canvas(x, y, w, h, rotation_deg,
doc_w, doc_h)`：按旋转角取 AABB（绕中心展开 `|cos|`/`|sin|`）后与画布求交。
`transform.x/y` 是左上角，与 `heal_layer_point` 同一口径。

**3. 保守判定的方向。** AABB 是上界，所以只有**确定**完全离开才提示；旋转后实际已空出
一部分的画面不提示。反过来误报「已拖出画布」会让人以为图层丢了，代价更大。
宽高 ≤ 0、画布 ≤ 0、角度非有限时返回 `true`（判断不了 = 当它还在）。

**4. 三个调用点。** `layer_move_extend` 追加 ` · 已拖出画布`；`layer_move_end` 的松手文案
追加 ` · 已拖出画布（图层仍在面板里，可拖回或 Ctrl+Z）`（前者会被后者覆盖，两处都要）；
方向键微调的 `panel_move_layer` 同样追加。无新增状态，每帧重算，拖回来提示自动消失。

**5. 验证。** UI 层新增 1 例（画布内 / 四方向拖出 / 贴边 / 45° 与 90° 旋转 / 宽高为 0 /
画布为 0 / NaN 角度）：单测 **592 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M27 步 18–20**。

**6. 仍未做。** 提示只在状态栏，画布上不画标记；旋转/缩放手柄仍在 §5.24 清单里
（组图层整体移动已在 §5.34 补齐）。

### 5.33 面板宽度可拖（§5.11「面板缩放 / 停靠」的一半，2026-10-21）

**1. 缺口。** 布局是写死的：图层面板恒 248 px、右侧面板恒 330 px。层名一长就被截断，
调色滑杆挤在固定宽度里；用户没有任何不换窗口的调整手段。

**2. 宽度偏好进 `LayoutPrefs`，`compute_layout` 只多一个入口。**
`compute_layout(w, h, prefs)` 用偏好宽度排版，原来的 `compute_layout(w, h)` 转调
`LayoutPrefs{}`，缺省行为与旧版逐字节相同（`ui_core_layout_never_overlaps_at_any_size`
未改一行仍通过）。宽度范围：图层面板 168–520 px，右侧面板 240–560 px
（`clamp_*_panel_width`，纯函数、单测钉住上下界）。

**3. 收起口径不变，只把「偏好宽度」当输入。** 面板仍在
`w >= clamp(pref) + kMinCanvasWidth` 时才显示，右侧面板看的是**图层面板排完之后**剩下的宽度；
三块仍精确铺满客户区。不做「挤成 120 px 的面板」：宽度不对的面板比没有面板更难用。

**4. 分界线放在面板内侧，不吃画布像素。** `Layout` 新增 `layer_splitter` / `adjust_splitter`
（图层面板最右 5 px、右侧面板最左 5 px），`panel_splitter_at` 返回 0/1/2 做命中。
画布边缘是落笔最频繁的地方之一，把那里抢去做缩放会让「画到边上」变成「面板变宽」，
这类误触比点不准行更难排查。代价只是面板最后 5 px 不再用于内容。

**5. 拖动只算总位移，pan 补偿让画面不动。** `*_panel_width_from_drag(base, dx)`
= `clamp(base ± lround(dx))`：夹取只作用在结果上，拖到边界后立刻反向不会有一段死区；
来回拖回原点必然回到原宽度，不累积取整误差。宽度一变，画布中心会平移，这里把
`中心前 - 中心后` 补进 `pan`，所以画面里的文档**待在原地**——用户拖的是面板，不该顺带
把正在看的图推走。宽度偏好是壳层布局状态，**不进撤销栈**（`Ctrl+Z` 只回滚像素/图层）。

**6. 收尾手感和既有拖动一致。** 松开 = 提交（状态栏 `图层面板宽度：288 px（双击分界线复位）`）；
`Esc` / 捕获被抢 = `panel_resize_cancel` 回到按下前宽度（先清状态再 `ReleaseCapture`，
重入的 `WM_CAPTURECHANGED` 因此是空操作）；双击分界线复位到 248 / 330 px。
悬停与拖动时整条 5 px 带提亮、光标变 `IDC_SIZEWE`，常态只画 1 px 暗线——否则用户
看不出这里能拖。面板被窗口宽度收走时状态栏如实追加
`（当前窗口放不下，已收起；放宽窗口后生效）`。

**7. 验证。** UI 层新增 3 例（偏好排版铺满/收起口径 × 25 组偏好 × 8 种宽度、夹取与拖动
换算的死区/取整、分界线命中含收起态）：单测 **595 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M29**。

**8. 仍未做。** 停靠里更重的两件：拖出成浮窗、面板合并——都不在本次范围内；
「整块收起/展开」已在 §5.43 补齐。偏好只在会话内保存（`ShellState` 字段），不落盘到设置文件。

### 5.34 整组移动：拖动与方向键搬整个组（§5.24 遗留 ⑥，2026-10-21）

**1. 缺口。** 组图层自己的 transform 不参与合成：`filter_graph` 只烘焙成员，组节点不读
自己的 `comp_rect`。所以过去两条入口都是错的——`panel_move_layer` 直接拒绝，
`layer_move_*` 却把位移写进组自己的 transform，状态栏数字在变、画面一动不动。

**2. 整组移动 = 平移子树里的每个成员。** 按下时快照成员的 `id / transform / rotation`，
之后每帧写「基准 + 总位移」，与单层同一写法：不累加、不按帧取整，来回拖回原点精确复原。
组自身的 transform 一律不写，因为它对合成没有意义。

**3. 成员判定沿 `parent` 链，不看数组相邻。** 面板是一张扁平列表，「数组里挨着」没有
父子含义。新增 `ui_core::layer_in_subtree(layers, count, node, root)`：从 node 逐层向上，
父级等于 root 才是成员；表里查不到该 id 就返回 false（宁可少移一层）。链最多走 `count`
步，数据成环（自引用/互引用）时返回 false —— 绝不死循环。嵌套组因此天然覆盖：判断看的
是整条祖先链，不只一层。

**4. 空组如实说「无位移」。** 不能只用「成员表是否为空」区分整组/单层：空组也走整组
路径，一旦落进单层分支就又会写出那个无效果的 transform。`layer_move_is_group` 单独记录
按下的是不是组；空组拖动与方向键都只给 `组内没有成员，无位移`，不碰文档。

**5. 一次操作 = 一步撤销。** 拖动时所有成员写进同一个显式事务（`Move Layer Group` /
`Duplicate & Move Layer Group`），松手 `comp_txn_commit` 算一步；方向键每次按键开一个
`Nudge Layer Group` 事务并立即提交（撤销一步回到按键之前，与 `panel_move_layer` 单层
行为一致）。按成功写入的成员数统计：一条都没成功就 abort，不留半组位移。

**6. Alt 拖动整组：复制后必须按副本重建成员快照。** `comp_layer_duplicate` 深拷贝整组，
所有成员 id 都是新的，按下时的清单全部失效。不重建会继续写**原组**的成员——画面上原组
在动、副本不动，看起来像「复制了但没复制」。复制成功后按副本 id 重新确认 `is_group`，
再快照子树，后续拖动与方向键都落在副本上。

**7. 提示口径。** 拖动中 `移动图层组：位移 (dx, dy)，N 个成员（松开算一步撤销）`；
松手后 `已移动图层组：…（一步撤销）`。`members_off_canvas` 要求**所有**成员都离开画布
才追加「已拖出画布」——与单层同一保守口径，少报只是少一句提示，误报会让人以为图层丢了。
方向键路径用局部成员清单调用同一个函数，不依赖拖动状态。

**8. 验证。** UI 层新增 1 例 `ui_core_layer_in_subtree`（直接子层 / 孙层 / 兄弟 / 反向
查询 / 表外 id / 自环与互环 / null、非正 count、root=0）：单测 **596 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M30**。
仍未做：组的混合模式 / 不透明度仍由合成管线按原样生效，移动不影响它们；
组自身 transform 依旧不烘焙，这是引擎口径，壳只按成员搬。

### 5.35 滤镜实时预览（§5.11「滤镜实时预览」，2026-10-21）

**1. 缺口。** 滤镜面板只有一条出口：回车 `comp_filter_apply` 应用一次、开一个撤销步。
参数好不好看只能靠「应用 → 看 → 撤销 → 再调」，而上游的面板是调一下就变。

**2. 预览帧只活在 framebuffer 里。** 引擎的滤镜会加入宿主已开的事务
（`engine/domain/filters.cpp:675`：`const bool owns_txn = !document->txn_active();`，
只有 `owns_txn` 时才自己 abort），所以每次参数变化走四步：
`comp_txn_begin` → `comp_filter_apply` → `render_canvas` 取帧 → `comp_txn_abort`。
事务把像素回滚，画面留着预览。文档、撤销栈、图层修订号全部停在「预览前」。

**3. 为什么不长期挂着事务（色彩范围那条路）。** 长期事务要精确回滚，就得在**每个**出口
记得 abort：关面板、换面板、新建/打开文档、切文字工具、拖轴中途关面板……漏一处就把别的
命令的写入卷进同一个撤销步，甚至 abort 到已经销毁的文档上（`forget_color_range` 的注释
就是为了这个坑）。framebuffer 方案的最坏后果是「画面停在一张旧预览」，数据永远不脏：任何
`needs_render` 的重画都能回到真状态。

**4. 回车仍是真应用，结果与预览逐字节一致。** 回车走未改动的 `apply_filter`（不开宿主
事务 = 引擎自开 `Filter: <name>`），一步撤销。同一组参数因此算两遍，这是刻意选择：
「预览帧 == 提交结果」比省一次计算重要。用 ctypes 对真实 DLL 验证过（脚本临时放在
`%TEMP%`，不进仓）：宿主事务内应用后 abort，像素逐字节复原且撤销栈不变；真应用的结果与
预览帧逐字节相同，撤销栈只 +1。

**5. 恒等 / 失败都如实回报，并强制作废旧预览。** `changed == 0`（半径或强度为 0）→
`参数为恒等，无预览`；`COMP_ERR_STATE`（组图层 / 无像素 / HDR）→ `预览失败，像素未改动`
（`set_status` 会带出引擎的 last_error）。两种情况都置 `needs_render`：参数已经变了，
屏上那一帧是错的。

**6. `filter_preview_pending` 的语义只有一条：framebuffer 里这一帧是预览。** 唯一置真点
是预览出帧成功；唯一清零点在 `present()` 的重画分支（任何 `needs_render` 的重画都说明
预览被顶掉）。其余出口（`open_filter_panel` / `close_filter_panel` / 换面板 / 新建 /
打开 / 切文字工具）调用 `filter_preview_forget`，只为让状态行能说「预览已丢弃，图层未
改动」。

**7. 面板模态化：画布点击不落到工具上。** 预览不在文档里，这时让画笔落笔等于在「看不见
的那份文档」上画，预览还会被重画冲掉。滤镜面板打开时画布点击被吞掉并提示（与色彩范围
「点击只取样」同规矩），唯一例外是 Progressive Blur 的轴拖拽。顺带修掉老问题：以前开着
滤镜面板还能用画笔落笔。缩放不受影响——滚轮 / Ctrl 缩放只改视图，预览帧照样拉伸显示。

**8. 入口。** `-` / `=` 步进（`filter_step_value`）、键入值提交（`filter_commit_input`，
带 `with_preview` 参数：键入+回车这条「落值并立刻应用」的路不预览，免得白算一遍）、
参数复位（`filter_reset_values`）、Progressive Blur 定轴（`progressive_axis_end`）。

**9. 验证。** 引擎未改，PE 导出 **138 不变**；单测 **596 例 / 0 失败**；壳自检
**245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。目视项见 `docs/manual-test.md` **M31**。
遗留：预览按全分辨率计算（scale 恒 1），大文档上每按一次就是整层算一遍；增量缓存要等
「图层快照 / 局部重算」类 ABI，壳侧不自造。

### 5.36 Move：有选区时按落点分流（§5.24 遗留 ⑤，2026-10-21）

**1. 缺口。** 此前 `move_begin` 只看 `selection_active`：有选区就一律搬选区像素。
上游 V 看的是**落点**——点在选区内搬像素、点在选区外搬整层（选区蒙版原地不动）。
用户在有选区时想把整个图层挪个位置，壳里没有入口（只能先 `Ctrl+D` 取消选区再拖，等于
丢掉选区）。

**2. 判定做成纯函数。** `comp_ui::selection_contains_point(mask, width, height, stride,
x, y, threshold)`：文档坐标 `floor` 后读 8 位 coverage，`>= threshold` 算「里面」。
阈值口径与 `selection_boundary_segments` 一致（调用点传 128，蚂蚁线画在哪就是哪）。
两处容易错的地方都钉住了：① 越界与 NaN 判成「外面」（`!(x >= 0.0)` 的写法才拦得住 NaN），
否则画布边缘外的一拖会去搬像素；② 阈值 0/负值按 1、>255 按 255，不返回「全选中」。

**3. 分流点仍是唯一的一个。** `move_begin`：里面的落点走原路径
（`comp_selection_move_pixels` → 松手一次性提交，蚂蚁线跟着走）；外面的落点改调
`layer_move_begin`（只写 transform，像素一个字节都不搬），事务名与 Alt 语义完全复用无选区
那条路（Alt = 复制整层再移动，一步撤销）。**蒙版读不到时不猜**：`selection_mask` 为空意味着
连「内/外」都没有依据，此时退回旧的搬像素行为，不改走整层拖动。

**4. 整层拖动不动选区蒙版。** 蚂蚁线留在原地，与上游一致（选区描述画面位置，不跟图层走）。
`layer_move_begin` 新增一个可选的 `hint` 参数，只用来在这条路上补一句
` · 落点在选区外：移动整层，选区不动`——四种开始文案（单层/组 × 移动/复制）都从同一个尾部
追加，不用复制四份格式串。

**5. 边界。** `move_begin` 只会在 `layout.canvas.contains(...)` 成立时被调用，所以「选区外」
指**画布内、选区轮廓外**；画布外的空白处按下本来就不是拖动起点，本项不改这个口径。
组图层照旧走整组平移（§5.34），与落点无关；`selection_active` 为假时行为一字未改。

**6. 验证。** 新增 1 个单测（`ui_core_selection_contains_point`：实心区 / 羽化边 128 与
127 / `1.9` 的 floor 取整 / 阈值 0 与 400 的夹取 / 四方向越界 / NaN / 空指针与非正尺寸），
单测 **597 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。目视项见 `docs/manual-test.md` **M32**。

### 5.37 多选批量移动（§5.12 遗留 5，2026-10-21）

**1. 缺口。** 面板支持 Ctrl 点选多行，但拖动与方向键只写**主选中层**：选中三行拖一下，
只有一行动，另外两行原地不动——这是最容易让人以为「选中了」的假象。

**2. 选中集合先展开成成员快照。** 新增 `build_move_targets`：`panel_selection_ids(shell, true)`
给出批量集合（已丢掉被**选中的祖先组**覆盖的子层，同一像素不会被写两遍），再逐项分流——
组用 `collect_group_members` 展开成子树成员（组自身 transform 不参与合成，写了等于没写），
普通层只取自己。两条来源统一成 `GroupMemberMove`（id + 按下基准 + 角度），后面只有一套写入循环。

**3. 拖动 = 一个事务 + 每帧「基准 + 总位移」。** 选中集合超过 1 个顶层图层时
`layer_move_begin` 记 `multi` 与快照并开**一个**事务（`Move Layers` / Alt 时
`Duplicate & Move Layers`），`layer_move_extend` 逐成员重写 `base + (dx, dy)`（不累加，
来回拖不漂移），松手 commit = 一步撤销。`multi` 与 `is_group` 互斥：组单选仍走 §5.34 的
整组分支与文案。全部选中项都是空组时不开始事务，状态栏
`移动多个图层：选中的图层没有可移动的成员，无位移`，撤销栈不留空格。

**4. Alt 拖动是整批复制的原子操作。** 首次位移时逐个 `comp_layer_duplicate`（组深拷贝整棵
子树），随后把**副本集合**设为选中集合、按副本重建快照——副本成员 id 全新，沿用旧快照会去写
原图层，表现成「复制了但没复制」。任一副本失败就整批作废：不写 transform，松手 abort 把已建
的副本一起回滚，状态栏 `复制多个图层失败：已回滚，位置未变化`，不留半个副本。

**5. 方向键同一条路。** `panel_move_layer` 在 `extra_selection` 非空时批量平移
（事务 `Nudge Layers`，一连串按键共用它 = 一步撤销，见 §5.30 项 5），状态栏
`图层：位移 (dx, dy)，N 个成员（方向键微调）`，出画布补 ` · 已拖出画布`（复用
`members_off_canvas`）。Move 工具帮助补了一句 `面板多选 = 整批移动（Alt = 整批复制）`。

**6. 验证。** 本项只改壳层，无新增纯函数：单测 **597 例 / 0 失败**（无新增）；PE 导出
**138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M33**。

### 5.38 多选批量开关图层效果（§5.13 遗留 ③，2026-10-21）

**1. 缺口。** 效果菜单与 `Ctrl+Shift+E` 只作用于**主选中层**：Ctrl 点选三层再点「投影」，
只有一行动，另外两行没变——与 §5.37 修掉的「多选拖不动」是同一类假象。

**2. 判据取主选中层，动作统一。** 菜单勾选本来就是看主选中层的：它有效果 → 整批移除；
它没有 → 整批添加。每层用 `effect_count` + `effect_at` **自己查自己**（新增
`layer_has_effect`），不复用壳里缓存的 `shell.effect_kinds`（那是主选中层的副本）。
已经在目标状态的层不写、也不占撤销步，最后在状态栏里算「已在目标状态」。
整批一个事务（`Add Effect To Layers` / `Remove Effect From Layers`）= 一步撤销；
一个都没改成时 abort，撤销栈不留空格。

**3. 组图层必须跳过，并且说出来。** `filter_graph` 的 `kGroup` 分支只合成子树，
**从不读组自己的 `effects`**，写进去画面一个像素都不会变。批量时逐层跳过并追加
` · 跳过 N 个组图层（引擎不按组求值效果）`；单选组时直接拒绝：
`效果：组图层不参与效果求值（请选中组内成员）`。这条也顺手修掉了单选路径上原来的
静默无操作（与组 transform 同一类假象，见 §5.34）。

**4. 边界。** 批量只做**开关**：参数面板打开后仍然只编辑主选中层（改值写的是
`comp_layer_set_effect(primary)`），批量改参数未做。批量为空 / 主选中层缺失时不猜。
添加效果后不自动打开参数面板（多选时打开会让人以为面板管整批）。

**5. 验证。** 纯壳层改动，无新增纯函数：单测 **597 例 / 0 失败**（无新增）；PE 导出
**138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M34**。

### 5.39 变换手柄：拖动缩放 / 旋转（§5.24 遗留 ④，2026-10-21）

**1. 缺口。** transform 的尺寸与角度此前只有引擎侧命令能改（`Alt+[ ]` 缩放、
`Ctrl+左右` 旋转），画布上没有任何可拖的把手——上游 Move 工具把「拖角缩放、拖旋钮
旋转」当默认操作。本项补齐这条，零新增 ABI 导出。

**2. 只在该画的地方画。** `transform_frame_for()` 四条硬条件：Move 工具且无
裁剪 / 框选 / 套索；面板**单选**一个非组图层；没有像素选区（有选区时落点分流已在
§5.36 定好，手柄不该抢命中）；该层 `w > 0 && h > 0`（有显式变换）。
最后一条是能力边界而非偏好：ABI 没有像素尺寸回读口，按像素尺寸的图层壳算不出真实
边界，**画一个猜的框比不画更糟**——与 `Alt+[ ]` 拒绝「无显式变换」同一理由。
多选不提供整批缩放：多选的既有语义是批量移动（§5.37）。

**3. 几何全在 `ui_core.h`（纯函数，可单测）。**
`transform_handle_point` / `transform_rotate_knob_point` 把局部点绕**矩形中心**旋转
（引擎 `map_document_to_pixel` 先归中心再逆旋转，绕左上角转出来的框会与画面错位）；
`transform_handle_at` / `transform_knob_at` 用同一个几何做命中，画在哪就点在哪。
屏幕尺寸不随缩放变（方块 9×9、命中半径 7 px、旋钮偏移 24 px），因此命中半径与旋钮
偏移都先除以 zoom 换成文档像素。

**4. 缩放保持对角锚点不动。** `transform_scale_rect` 把指针逆旋转回局部系、按手柄改
对应边、宽高压到下限（1 px，指针越过对边时不出现 0/负数）；旋转过的图层中心会移动，
所以还要把原点回移 `anchor_doc - R(θ)·anchor_local_new`——只改 x/y 不够，会把图层
「顺带」推走。每帧都从按下时的 base 重算，不在上一帧结果上叠加，来回拖动不漂移。

**5. 一次拖动 = 一步撤销。** 按下 `comp_txn_begin`（`Scale Layer` / `Rotate Layer`，
历史面板里两类操作不该同名），途中每次整数变化写一次 `comp_layer_set_transform`
（内部 ImplicitTxn 加入该事务），松手 commit；没变化则 abort 并写
`缩放：尺寸未变化` / `旋转：角度未变化`，不留空撤销步。Shift 吸附 15°（绝对角度
对齐，与 `Ctrl+左右` 同步长）。提交/写入失败如实报 `变换：提交失败` /
`变换：写入图层变换失败`。

**6. 与既有交互的先后。** 手柄命中优先于整层拖动（`move_begin` 里最先调用
`layer_handle_begin`）；事务开不起来时也**吃掉**这次点击，绝不退化成整层拖动
（否则「按在手柄上」会变成「把图层拖走」）。Esc / 失焦 / 捕获丢失走 `move_cancel`：
先按「事务只属于当前文档」的守卫 abort，再清状态、重画。`WM_MOUSEMOVE` /
`WM_LBUTTONUP` 的分发条件已加上 `layer_handle_active`。

**7. 边界。** 没有随位置切换光标（十字 / 缩放箭头）：光标仍由工具决定，
手柄只靠图形本身提示，见 M35 的目视项；多选手柄、按 Alt 从中心缩放、
吸附到画布/图层边缘都未做。`Shift` 拖角等比已在 S43 补齐（见第 9 条）。

**8. 验证。** 单测 **598 例 / 0 失败**（新增 1 例 `ui_core_transform_handles_track_rotation_and_anchor`：
8 个手柄位置、旋转后的命中、90° 锚点不漂移、下限钳制、角度归一化）；PE 导出
**138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M35**。

**9. Shift 拖角 = 等比缩放（S43）。** 自由缩放两个方向各走各的，拖角很容易把图层
拉扁；上游按住 `Shift` 锁比例。做法是给 `transform_scale_rect` 加一个 `uniform`
参数（默认 false，既有调用点与用例一个不动）：

- 只对**角**手柄（0/2/4/6）生效。自由拖拽算出的 `(right-left, bottom-top)` 就是指针
  要求的两个新尺寸，取相对变化更大的那个方向定倍率，另一方向按 base 的宽高比跟随。
  取「主方向」而不是取最小/最大：指针斜着走时结果可预测，且与常见编辑器一致。
- 倍率下限同时满足两个方向的最小尺寸（否则会出现「宽够 1px、高只有 0.2px」）。
- 比例锁在**按下时**的 base 上（每帧重算，不在上一帧结果上叠），所以松手再拖、
  来回拖都不会逐帧漂。
- 对角锚点与旋转补偿完全复用既有逻辑：等比只改两个尺寸，不改锚点语义，旋转过的
  图层照样钉住对角。边手柄忽略 `Shift`——PS 里那是「以中心等比」，本壳没有中心
  锚点这条路径，静默按自由缩放处理比偷偷换锚点更好解释。
- 壳侧只在 `layer_handle_extend` 读一次 `GetKeyState(VK_SHIFT)`（与旋转吸附同一个
  读法），拖动中途按下/松开 `Shift` 立即切换；状态栏补 `Shift = 拖角等比` 与 ` 等比`
  标记。

单测新增 `ui_core_transform_uniform_scale_locks_aspect`：左上角取主方向倍率且对角不动、
同一指针下自由缩放破坏比例（对照）、右下角钉住左上角、边手柄忽略 `Shift`、
拖到 0 时两边都不低于下限、旋转 90° 后锚点不漂。单测 **600 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项 / 失败 0**（基线 SHA256 不变）；
`run_regression.py --require-ohos` **6/6**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M39**。

### 5.40 工具图标：GDI 现画，零资源文件（2026-10-21）

**1. 缺口。** 工具栏 20 个按钮画的是快捷键字母（`V` / `B` / `M`…），必须记住键位才知道
按钮是什么工具；上游是图形图标。本项补齐图形，零新增 ABI 导出。

**2. 为什么不用资源。** `.rc` + `.ico` 要多一份资源文件并改构建脚本；字体符号依赖目标
机器装了什么字体。两者都会破坏「单个 exe 拷走就能跑」这个部署前提。所以图标用 GDI
图元现画：构建产物不新增任何文件，ABI 不需要任何「取图标」入口。

**3. 几何随按钮缩放。** `ToolIconMap` 以 32×32 逻辑网格为坐标系，`scale = 内框边长 / 32`，
所有坐标只写逻辑值；`lround` 落像素。笔宽 `max(1, lround(2·scale))`，小缩放下线条不会
消失；框选用 1 px `PS_DOT`，与实线图标区分。

**4. 每个工具的形状。** 箭头光标 / 四向箭头 / 虚线框 / 套索环+尾 / 钢笔尖 / 魔棒+星 /
裁切双 L / 滴管+笔杆 / 笔刷+笔尖 / 橡皮斜块 / 创可贴+十字 / 印章 / 水滴 / 太阳 /
渐变方框+对角半实心 / 方块+圆 / `T` / 手掌 / 放大镜+加号 / 时钟。空心形状先换
`NULL_BRUSH` 再换回，实心/空心在同一支笔和同一颜色下画，不依赖透明色。

**5. 颜色与可发现性。** 图标颜色与按钮文字同色：选中纯白、未选中浅灰
（`RGB(198,200,208)`），选中态仍是蓝底。快捷键字母不再画在按钮上，但状态栏本来就有
`工具名（V）`，键位仍可发现——这也是不需要 tooltip 的另一半理由。

**6. 边界。** 图标是静态的：没有 hover 反馈、没有禁用态、没有高分屏的 2× 位图
（GDI 矢量绘制本身就是分辨率无关的，所以也不缺这块）；图标的「设计」是壳侧自绘，
不宣称与上游逐像素一致。

**7. 验证。** 单测 **598 例 / 0 失败**（本项只有绘制代码，不新增纯函数）；PE 导出
**138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M36**。

### 5.41 菜单项按前置条件置灰（2026-10-21）

**1. 缺口。** 菜单项一直是「永远可点」：没有选区也能点「取消选择」、单层也能点「向下合并」、
顶层也能点「上移一层」。点了之后靠处理函数在状态栏回一句「没有选区」——不给假结果，
但**入口本身不诚实**：用户看不出来哪些项此刻没有意义。§5.15 边界 ④ 记的就是这条。

**2. 判定放在纯函数里，壳只搬运。** `comp_ui::LayerMenuState` 扩出前置条件字段
（`has_selection` / `has_mask` / `can_undo` / `can_redo` / `can_merge_down` / `can_move_up` /
`can_move_down` / `clipping_available` / `multi_selected` / `effect_count` / `layer_count`），
`comp_ui::layer_menu_enabled` 折成逐项可用态 `LayerMenuEnabled`。`EnableMenuItem` 的调用
本身无法单测（要真窗口），所以单测只覆盖规则表，这也是把规则抽净的理由。

**3. 只置灰「点了会拒绝或纯空转」的项。** 判据不是「有没有选中层」一刀切，而是照各处理
函数的实际前提逐条对：
- 撤销/重做问 `comp_document_can_undo/can_redo`，与图层无关；
- 向下合并直接用 `comp_layer_can_merge_down`——ABI 注释里写的就是「供外壳置灰菜单」；
- 剪贴/重排复用执行时同一套纯函数（`plan_clipping_base`、`plan_layer_reorder`），
  所以不会出现「菜单说能做、点了说不行」；
- 组的效果项默认置灰（效果作用于成员层，组本身不挂效果），有多选集合时放开——
  那条路径本来就支持批量跳过组；
- 拼合/上下选层要求 ≥2 层；清除效果要求已有效果；蒙版三项分别按有无选区、有无蒙版。

**4. 递归应用。** `EnableMenuItem` 与 `CheckMenuItem` 一样只作用于单个 `HMENU`，
混合模式/采样/效果都在二级子菜单里，所以新增 `menu_set_enabled_recursive`，按位置下钻
（`MF_BYPOSITION` + `GetMenuItemID`）找到命令 id 再改。整个动作仍挂在
`WM_INITMENU → sync_menu_state` 这一个入口，不重建菜单。

**5. 键盘路径刻意不动。** 置灰只影响菜单。同一命令若还有快捷键，按键仍直接进
`run_shell_command`，各处理函数保留自己原有的拒绝提示——键盘用户不该因为菜单项变灰
就失去「为什么不行」的说明。这是有意的双路径，不是漏改。

**6. 边界。** ① 没有工具栏按钮的置灰（工具栏按钮是自绘的，没有 Win32 禁用态可复用，
留待与按钮 hover/按下态一起做）；② 不按「组/剪贴」细化到每一项（例如剪贴态下部分变换项
仍可点，由处理函数兜底）；③ 已知的真实状态以 `WM_INITMENU` 时刻为准，长按菜单不刷新
（Win32 菜单常态）。

**7. 验证。** `tests/test_ui_core.cpp` 加
`ui_core_layer_menu_enabled_follows_preconditions`（空状态、单层、组+多选三段），单测
**599 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。真实窗口另用 `%TEMP%\menu_state_probe.ps1` 验证：
按 `WM_INITMENU` 后逐项读 `GetMenuState`，新建文档时 Undo/Redo/合并/拼合/选层/蒙版/
选择修改全灰、添加图层后合并/拼合/选层/剪贴转可用（顶层仍在「上移一层」置灰），
全选后「从选区建立蒙版」与选择修改项转可用。目视项见 `docs/manual-test.md` **M37**。

### 5.42 树形图层面板（§5.12 项 8，2026-10-21）

**1. 缺口。** §5.12 项 8 记了三条：组行显示在子层**下方**（上游 PS 相反）、面板没有树形
缩进、没有组折叠。前两条同源——面板显示序一直写成 `行 = 层数-1-数组下标`，即「数组逆序」。
数组是自下而上的堆叠序，而组在数组里位于子层**下方**（`move_layer` 把子层插在组下标 +1），
逆序之后子层就跑到组行上面去了。**改显示方式，不改引擎落位**：数组序仍由引擎说了算。

**2. 显示序是纯函数。** `comp_ui::build_layer_display(layers, count, collapsed, display, depth)`
返回 `display[row] = 数组下标` 与 `depth[row]`：

- 根层按数组逆序遍历（尾 = 顶层，行 0 = 最上层），这是旧面板的唯一正确部分，保留；
- 每遇到组，**先输出组行**，再按同样规则递归输出它的子层——于是组在上、子层缩进在下；
- 折叠的组只输出组行，整棵子树不占行；
- 父级不存在（悬空 parent）或成环的层不丢：收尾时按数组逆序追加到根级，环里每层仍只出现
  一次（`layer_reaches_root` 挡住无限递归）；
- 行数上限 `kMaxLayers = 4096`，超限整段回落到旧的逆序显示——宁可没有树，也不能截断
  一个真实文档。

**3. 几何与对齐。** `kLayerIndentStep = 12`、`kLayerIndentLimit = 72`（6 级封顶，更深的
嵌套不再往右推，否则名字会被挤出 248px 栏宽）；`kLayerTreeMarker = 14` 在**每一行**都留出
三角槽位，同层级的名字才会对齐。三角是 `layer_triangle_rect(row_rect, depth)` 的 10×10
方块：折叠画 ▸、展开画 ▾，只有组行真的画。

**4. 缩进之后，行号不再能反推图层。** 折叠会改变行数，展开的子树会整体平移后续行。以往
「行 = 逆序下标」的换算散落在点击、双击、Ctrl 晋升、Shift 区间、剪贴基底、重排、拖拽命中、
批量枚举里，逐处改成 `layer_row_at` / `layer_index_at_row` / `panel_row_of_id`。**结构变化后
选中态一律按 id 重选**：行号缓存不会报错，只会让下一次快捷键作用到别的层。

**5. 重排与拖拽改「同级序」。** 旧实现在数组里找相邻元素，树形显示下相邻两行未必同级
（组行下面是它的子层）。`plan_layer_reorder` 改为在同父集合里数序号：上移 = 插到下下个兄弟前、
下移 = 插到上一个兄弟前、置顶 = `before = 0`、置底 = 插到第一个兄弟前；`plan_layer_drag`
同样先按行取图层，再验同父、再算兄弟序号。端头判定（`at_top`）也按同级集合而非数组端头，
否则「组里唯一子层」会被告知可以上移。

`before = 0` 在引擎里的语义不是「数组末尾」：`parent != 0` 时插到**最后一个同父兄弟之后**，
`parent == 0` 时追加到数组末尾（`Document::reorder_layer`）。单测里的引擎模拟器最初按
「一律追加到末尾」写，被树形用例抓出差异——这次也一并修正了模拟器，让它真的等于引擎。

**6. 折叠是视图态。** 三角只改壳侧的 `collapsed_groups`：不写文档、不进撤销，与上游一致。
折叠后若主选中层被藏起来，选中落到该组行上；多选集合里被藏起来的 id 一并摘掉——批量命令
按可见行枚举 id，留着它们会让「已选中 N 个图层」的计数与真正执行的层数对不上。
双击三角槽位被显式吞掉，否则「连点两下想展开」会先弹出组名编辑框。

**7. 边界。** ① 缩进 6 级封顶，更深的层级共享同一缩进（可看出是「更深」，但看不出深几级）；
② 没有键盘展开/折叠（`Left`/`Right` 仍是选层，留待与面板焦点导航一起做）；③ 折叠状态不
持久化，关文档即丢；④ 仍不支持跨组拖拽改父级（拖进组/拖出组仍走 §5.12 项 6 的中间带与
根级行两条路径）。

**8. 验证。** 单测 **603 例 / 0 失败**，新增
`ui_core_layer_tree_display_puts_group_above_children`（显示序/缩进/折叠/悬空 parent/成环/
容量回落）、`ui_core_layer_tree_reorder_stays_within_siblings`（组内子层与根层各自的上移/
置顶/下移 + 引擎重排复算）、`ui_core_layer_tree_drag_uses_sibling_ordinals`（树形 display
下的拖拽锚点、折叠态、跨父拒绝）；既有扁平表驱动用例全部保留（它们现在是「没有组的文档」
这一特例）。PE 导出 **138 不变**（本项零新增 ABI）；壳自检 **245 项不变**（基线 SHA256 仍
`26b3e910e3b9ccb7…`，面板绘制不进文档内容摘要）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。目视项见 `docs/manual-test.md` **M40**。

### 5.43 面板整块收起（§5.11「面板缩放 / 停靠」的续，2026-10-22）

**1. 缺口。** §5.33 只做了「拖宽」。停靠有三件：拖出成浮窗、面板合并、整块收起。
收起是最小也最独立的一件——它不需要新的窗口、不需要容器概念，只改布局里「面板要多宽」，
所以先做它。目标是：一块屏幕里想多看画布时，一个键就能把左右栏收成竖条，再一个键还原。

**2. 收起是显示开关，不改宽度偏好。** `LayoutPrefs` 增 `layer_panel_collapsed` /
`adjust_panel_collapsed`；收起时 `compute_layout` 只把「想要的宽度」换成
`kCollapsedPanelWidth = 24`，`layer_panel_width` / `adjust_panel_width` 原样不动——
展开回来必须是用户拖出来的宽度，而不是缺省值。

**3. 收起条仍占布局宽度，不用 0。** 0 宽条虽然能把画布放到最大，但有两个立刻能看见的
毛病：① 竖条只有 24 px，用户还有地方点回来；0 宽时「展开」只能靠快捷键/菜单；
② pan 补偿按**画布中心**算，宽度与中心同时变会让画面「闪一下」。24 px 让收起/展开
仍走同一套「三块横向铺满」的算式（`layer_panel.w + canvas.w + adjust_panel.w == width`），
补偿量是连续的。收起口径也没有破例：仍按 `想要的宽度 + kMinCanvasWidth(180)` 判断放不放得下，
窄窗整块不给，状态栏如实追加「（当前窗口放不下，放宽窗口后生效）」。

**4. 命中：展开态的小方块不碰分界线，收起态的整条都是按钮。** 展开态按钮 18×18 落在
标题行右端（`panel_toggle_rect`：离右边缘 8 px、距顶 6 px），分界线占最右 5 px，两者留
3 px 间隔——重叠时「拖宽」会吃掉「收起」，而这正是最容易在窄面板上误触的地方。
收起态整条 24 px 都是 `panel_toggle`，点条上任意位置都能展开，不必去瞄小方块；同时收起态
**不提供分界线**：24 px 的竖条里塞一条 5 px 拖动带，几乎整条都会变成「拉宽」。
点击顺序固定在工具栏之后、分界线与面板行之前（收起条压在滑杆行/图层行上，顺序反了就永远点不到）。

**5. 通道条左移让位。** 右侧面板展开态的标题行右端原本被色阶/曲线通道格占着（"R/G/B/合成"，
可点）。按钮进来后会把最后一格盖住，于是 `adjust_channel_chip_rect` 整体左移
`kPanelToggleSize + kPanelToggleRightMargin`（26 px）——绘制与命中用的是同一个函数，
不会出现「看得见但点不中」的错位。既有的「通道格不超出面板右边缘 10 px」单测仍通过。

**6. 绘制。** 展开态画 18×18 方块 + 朝外的三角（左栏朝左、右栏朝右），悬停提亮；
收起态画整条深色底 + 朝画布一侧的 1 px 交界线 + 竖排两字（「图/层」「调/整」）+
朝画布的三角（左栏朝右、右栏朝左），悬停整条提亮。视觉上一眼能分出「按下去收」还是
「按下去展开」。悬停时 `WM_SETCURSOR` 给 `IDC_HAND`；`WM_MOUSELEAVE` 与分界线高亮一起熄灭。

**7. 入口三条，pan 补偿与拖宽同一写法。** 标题行按钮、`F7` / `F8`（PS 同键位：
Layers / Adjustments）、「视图」菜单两项（勾选态 = 已收起）都走 `run_shell_command`，
菜单与键不分叉。切换前后各算一次布局，把 `中心前 - 中心后` 补进 `pan`：收起图层面板会让
画布宽 224 px、中心右移 112 px，不补偿的话正在看的图会跟着跳一下。与 §6.60 一样，
这是壳层布局状态，**不进撤销栈**。

**8. 边界。** ① 只做收起，拖出成浮窗/面板合并仍未做；② 收起时面板内容整块不画（不是
画一半再裁掉），竖条上不会漏出半截图层行/滑杆；③ 收起状态与宽度偏好一样只活在会话里，
不落盘；④ 收起时滚轮/点击不再落到面板内容上（面板行命中在收起条上必然先被按钮吃掉，
滚轮分支显式跳过）。

**9. 验证。** 单测 **604 例 / 0 失败**，新增 `ui_core_collapsed_panels_layout`（收起条
24 px 与铺满不变式、收起态整条可点且无分界线、展开态按钮在标题行内且不压分界线/通道格、
0..400 px 窄宽度扫描下绘制矩形不越界）；PE 导出 **138 不变**（本项零新增 ABI）；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M41**。

### 5.44 选区扩缩手柄：8 个把手 + 绝对矩形重采样（§5.11「选区扩缩手柄」，2026-10-22）

**1. 缺口。** 选区早就有了，面板里的**数值**扩缩（`comp_selection_modify`，
1..500 px）也早就接了，但选区的四条边上**没有可拖的把手**。缺的不是 UI 而是
ABI：手柄给出的是「把选区改到这个矩形」，而既有入口只有**相对**扩缩
（再扩几像素），表达不了绝对位移；壳侧又不能自己改蒙版（选区在引擎里，
壳只持有一份 coverage 副本）。所以这一项必须先补 ABI，再谈把手。

**2. ABI 尾部追加两个导出（138 → 140）。**

- `comp_selection_get_base_bounds(document, comp_rect* out)`：基础蒙版的**紧**包围盒
  （非零像素的 bbox，**不含**羽化外扩）。注意它与既有的 `comp_selection_get_bounds`
  是**两种口径**：后者是「可见 coverage 的范围」，羽化会把盒子外扩 `ceil(2*feather)`；
  手柄必须贴在**真正被变换的那张蒙版**上，所以用前者。没有选区/显式空选区时写
  `{0,0,0,0}`；`out` 为 NULL → `COMP_ERR_INVALID_ARGUMENT`（在事务之前拒绝）。
- `comp_selection_set_bounds(document, comp_rect bounds, int32_t* out_changed)`：
  源矩形**不是**调用方给的，恒为当前 `get_base_bounds()`；调用方只给落点。

既有的枚举、结构体、签名一个没动，只是尾部追加。

**3. 变换语义（四条都是刻意定的）。**

1. **恒等 = 逐字节不变、`changed = 0`。** 源 = 当前紧包围盒，所以
   `bounds == get_base_bounds()` 必然不发生重采样。实现里 `masks_equal` 再兜一层：
   万一坐标换算有舍入，也**不会**因为「把把手拖回原处」多出一条撤销步。
2. **双线性重采样，保留抗锯齿边。** 目标像素中心 → 源坐标：
   `(x + 0.5 - target.x) * scale + source.x - 0.5`，`scale = source.w / target.w`。
   减 0.5 是因为源像素中心在整数格点上；`target == source` 时这里恰好落到整数，
   双线性权重全在原像素上（这就是第 1 条成立的算术原因）。最近邻会让椭圆选区
   变成锯齿，所以不用。
3. **画布裁剪与缩放比解耦。** 越出画布的部分直接丢弃（画布外没有选区），
   但 `scale` 仍按**原样**的 target 算——贴着画布边界拖时，内容不能被悄悄拉伸。
   `target.x + target.w` 用 int64 中间量，避免整数溢出。
4. **退化 = 显式空选区，不是「取消选区」。** `w <= 0 || h <= 0` 写入一张全 0 蒙版，
   `active` 仍为 1：用户在 PS 里把把手拖到对边，得到的是「有选区但空」，
   不是「没有选区」（两者在合成限制与后续相加/相减语义上不同）。
   `feather` 半径原样保留——羽化是选区**参数**，不是几何，与
   `comp_selection_translate` 同口径。不做镜像翻转；拖过头的夹取由宿主负责。

一次调用 = 一步撤销（`ImplicitTxn("Transform Selection")`，加入调用方已开的显式
事务，或自成一步）；没有选区/基础蒙版为空时无操作、`changed = 0`、不落撤销步。

**4. 几何全在 ui_core：手柄是屏幕尺寸，不是文档尺寸。** 新增
`kSelectionHandleCount = 8` 与下标表 **0=左上 1=上 2=右上 3=右 4=右下 5=下 6=左下 7=左**
（**偶数都是角、奇数都是边**，绘制/命中/拖拽三处共用这一个顺序）：

- `selection_handle_point(rect, index)`：文档坐标锚点。矩形是左闭右开像素网格，
  右/下边取 `x + w` / `y + h`（与 `doc_to_screen` 同口径，不另立一套）。
- `selection_handle_hit(centers, count, p, tolerance)`：**屏幕空间**命中，返回最近的、
  距离 ≤ tolerance 的手柄，否则 -1。手柄大小不随缩放变，所以不能在文档空间做命中；
  命中点由调用方用 `doc_to_screen` 换算好——**与绘制共用同一份屏幕坐标**，
  这一仓反复踩的「看得见点不中」就是两处各算一遍造成的。NaN 永不命中
  （`!(p.x == p.x)`），否则 NaN 比较全假会误判成「命中第一个」。
- `selection_drag_rect(source, index, point, lock_aspect)`：文档坐标结果矩形。
  只动被拖的那条边/那个角，对边不动；`lock_aspect` **只对角生效**（PS 里边手柄
  也不等比），锚点是对角线另一端，且按**主导轴**取宽：
  `width = max(|dx|, |dy| * aspect)`——只按 dx 算会让「往下拖得比往右多」时
  比例失守。拖过对边夹到 1 px 并停住（先夹再 round，round 单调性保证宽 ≥ 1），
  **不镜像**（引擎也只有「收成空」这一种退化语义）。NaN / index 越界原样返回 source。

**5. 壳只做四件事：取包围盒 → 命中 → 拖拽矩形 → 松手一次 `set_bounds`。**

- **生效工具**：Marquee / Lasso / Wand / Move。画笔类工具的落点含义是画图，
  把手会抢命中也会误导（PS 同规矩）；框选/套索/裁剪/钢笔/缩放框的临时态里一律不出现。
- **点击优先级**：`selection_handle_begin` 排在框选、套索、魔棒、Move 整层拖动**之前**。
  返回 true 表示「这次点击归手柄」，调用链到此为止——否则按在把手上会退化成
  新框选，直接把用户想改的选区删掉。与 `layer_handle_begin` 同一约定。
- **拖动只出预览**：`selection_handle_current` 是文档坐标落点，引擎在松手前不动，
  所以拖动中途**没有**撤销步，也不需要「拖一半失败怎么办」的回滚。
- **松手一次提交**：先落 `target`/`source` 再 `selection_handle_cancel()`，
  **然后**才 `ReleaseCapture()`——`ReleaseCapture` 会同步派发
  `WM_CAPTURECHANGED`，顺序反了会把刚要提交的落点自己清掉。
  落点与源矩形逐字段相等（含抖动一像素又拖回来）时**不调引擎**，状态栏如实说
  「选区未变化」，不产生撤销步。提交后 `refresh_selection` 重新取紧包围盒与蚂蚁线。
- **取消**：`Esc`（拖动中按键整个被手柄吃掉，其它快捷键不生效）、`WM_KILLFOCUS`、
  `WM_CAPTURECHANGED`、换/开文档都走 `selection_handle_cancel`。取消只丢预览，
  幂等，不 `ReleaseCapture`（捕获由调用点负责）。
- **悬停光标**：`WM_SETCURSOR` 里先判拖动（按 drag 序号），再判命中；角给斜双箭头
  （0/4 → `IDC_SIZENWSE`，2/6 → `IDC_SIZENESW`），边给单轴箭头
  （1/5 → `IDC_SIZENS`，3/7 → `IDC_SIZEWE`），且只在画布矩形内生效。

**6. 绘制。** 8 个 7×7 白底 + 1 px 黑框的方块（GDI `Rectangle`，**屏幕尺寸**，
放大画布不会变成巨块），锚点用 `selection_handle_centers`——与命中共用一份。
拖动期间额外画一个双笔画预览矩形（白色实线 + 黑色 `PS_DOT`），从当前落点换算，
让「松手会变成多大」在松手前就看得见。蚂蚁线仍从缓存的 `selection_segments` 画，
口径不变；所有覆盖层都裁在 `layout.canvas` 内。

**7. 边界。** ① 只做「拖动缩放」，透视/变形/旋转手柄属于 §5.39 的变换手柄，不在此列；
② 不做「Alt 从中心缩放」「Shift 边上等比」（PS 也没有）；③ 把手不改羽化值、
不改选区模式（相加/相减），一次拖动就是一个 REPLACE 式的几何变换；
④ 引擎侧不记「上一次的矩形」，每次拖动都以当前紧包围盒为基准，所以两次拖动
是串行的、可撤销的，不会累积误差；⑤ 收起面板/切工具不打断拖动（拖动中被
`WM_CAPTURECHANGED` 收敛）。

**8. 验证。** 单测 604 → **610 例 / 0 失败**：ABI 层 4 例（`selection_base_bounds_ignores_feather`
——羽化不改紧包围盒、可见包围盒确实更大、取消选择后返回 `{0,0,0,0}`、NULL 参数在
事务前拒绝；`selection_transform_bounds_identity_is_byte_identical`——带羽化椭圆
逐字节不变 + `changed = 0` + 撤销深度不动 + 羽化保留；`selection_transform_bounds_rescales_the_mask`
——16×12→32×24 面积四倍、内点 255、最外圈 0<v<255（双线性）、旧轮廓消失、
一步撤销回原矩形、再缩到 8×6；`selection_transform_bounds_collapses_to_an_explicit_empty_selection`
——退化矩形得到 `active=1` 的空蒙版、可撤销、无选区时无操作、越出画布只保留画布内部分）；
UI 层 2 例（`ui_core_selection_handle_points_and_hit`：8 个锚点位置、最近命中、
容差边界、空表/NaN 不命中；`ui_core_selection_drag_rect`：四边/四角单向移动、
对角不动、Shift 角等比按主导轴、边手柄忽略 Shift、拖过头夹 1 px、NaN/越界不炸）。
PE 导出 **138 → 140**（尾部追加 `comp_selection_get_base_bounds` /
`comp_selection_set_bounds`，既有导出与签名不变）；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`——本轮没有动任何既有自检路径）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
目视项见 `docs/manual-test.md` **M42**。

### 5.45 文字接入字体轨道：中文输入与码点编辑（P5-3，2026-10-22）

P0-5 验收「你好，世界」@48 px 的外壳半边。引擎的字体轨道在 P5-2 已通
（`comp_tool_apply` 读 `comp_tool_op_desc.type_font`），这一步把它接到 Windows 壳：
启动加载随仓字体、面板第四行切轨道、键入只走 `WM_CHAR`（输入法才进得来）、
退格按码点删。**ABI 零新增**，仍是 145 个导出。

**1. 字体从哪来。** `resources/fonts/NotoSansSC-Regular-Subset.ttf`（仓内子集字体）。
`load_shipped_font` 依次探 `<exe>/resources/fonts`、`<exe>/../resources/fonts`
（开发构建的 `build/` 就在仓根下一层）、`<cwd>/resources/fonts`——构建脚本不拷贝字体，
所以三处都探；可用性闸门是 `scalable && units_per_em > 0 && has_cjk`，
**位图字体与纯拉丁字体一律不选**：宁可退回 5×7，也不挂一个排不出中文的字体上去。
`comp_font_metrics` 的 `ascender` / `descender`（em 比例）存进 `ShellState`，
退出时 `comp_font_release`。找不到字体不是错误：状态栏如实说「未找到随仓字体」，
文字工具只用点阵。

**2. 两条轨道，一个开关。** 面板第四行 `Typeface` 是**开关行**
（`type_panel_row_is_toggle`）：值文本由外壳填（字体 family 名 / `5×7 点阵`），
`- / =` 切换，数字键不进入输入态（这一行没有数值量纲）。开轨道时
`comp_tool_op_desc.type_font` 填句柄，`font_size` 的含义同时从「格子高」变成
**em 高**——同一组面板数字两种量纲，状态栏明写「em 高」；关轨道时字段是 NULL，
5×7 轨道逐字节不变（P5-2 的 22 条逐字节断言仍全绿）。

**3. 键入只认 `WM_CHAR`，`type_ascii_from_vk` 已删除。** 两条理由：VK→ASCII
映射天生看不见输入法（中文永远打不进去），而且它与 `TranslateMessage` 派发的
`WM_CHAR` 会**各插一遍**，开输入法时打进双字。现在字符的唯一来源是 `WM_CHAR`
（`TranslateMessage` 与 IME 的共同出口）：代理对
（`0x10000 + (high - 0xD800) × 0x400 + (low - 0xDC00)`）在 `type_char_input`
里拼好，孤立半个代理丢弃；`< 0x20`、`0x7F`、越界码点挡掉。`type_skip_char`
吞掉 `[ / ]` 快捷键之后必然跟来的那个 `WM_CHAR`——不吞的话按一次 `[` 会同时
改字号**和**打出一个 `[`。

**4. 编辑按码点。** `Backspace` 走 `type_utf8_erase_last`：先找首字节、再按首字节
声明的长度整段删。按字节删会把缓冲停在半个汉字上，引擎如实拒收成 `kBadText`，
用户看到的是「退格把整段字弄没了」。缓冲不是完整 UTF-8 时函数返回 0 且**不动**：
宁可原样留着让引擎拒收，也不猜该丢哪个字节——缓冲只由本文件的追加函数写入，
出现半截只可能是本文件自己的 bug，那时不能悄悄「修好」。

**5. 预览仍是估计值。** 外壳没有「只度量不落墨」的 ABI 入口，也不为此加导出：
`type_block_metrics_font` 按 em 比例估宽——CJK/全角 1 em、其余 0.55 em
（Noto Sans SC 的拉丁平均量级，±5%）；高 = `(ascender - descender) × em +
(行数 - 1) × em × line_height`；`advance` 归 0 表示「不存在单一逐字步进」。
界面数字一律带 `≈`，状态栏写「宽度是估计值」，不把估计当引擎真值。
画布上仍只画插入点光标 + 虚线文本框，**不预演字形**：GDI 画一套与 FreeType
落出来的不是一回事，假预览比没有预览更糟。

**6. 验证。** 单测 **619 例 / 0 失败**（同数换内容：删掉 22 条 `type_ascii_from_vk`
断言，换成 UTF-8 追加/退格/解码与字体轨道度量共 30 条）；壳自检 **245 → 257 项**，
新增 12 项（字体可加载且 scalable/has_cjk、UTF-8 三个助手、四行面板表与开关、
字体轨道度量、CJK 走 ABI 落墨覆盖 > 100 像素、一步撤销逐字节还原、缺字 U+0378
如实写进 note、撤销深度回到起点）。基线 `compositor_shell_selftest.txt` 重新生成
（SHA256 `377DE660D48AC933…`）；`run_regression.py --require-ohos` **6/6**；
`build_windows_shell.py` 零警告。

**7. 边界。** ① 没有富文本（粗/斜/字重/对齐/旋转），也没有字体选择对话框——
只有「随仓字体 ↔ 5×7 点阵」两档；② 两轨的 `font_size` 量纲不同（em vs 格子高），
切换时数字不换算，状态栏载明；③ 输入法候选窗不跟随插入点（没链 `imm32`：
`ImmSetCompositionWindow` 要实时取插入点坐标），候选框停在默认位置；
④ 键入路径**未经真人键盘/输入法实测**，目视项见 `docs/manual-test.md` **M43**。

### 5.46 剪贴板：拷贝 / 剪切 / 粘贴 / 贴入选区 / 清除（P0-4，2026-10-22）

M46 的对侧记录。全仓此前没有一行剪贴板代码，本轮补齐 `编辑` 菜单六项，
ABI **尾部追加一处**（`comp_layer_get_pixels`，145 → 146）。

**1. 为什么要新导出。** 逐字节往返必须拿图层自己的原始像素缓冲；
`comp_render_flatten` 会把图层按 transform 重采样、叠背景、过混合模式，
回来就不是原来的字节。`comp_layer_get_pixels` 只读缓冲 + 格式描述
（宽/高/format），不做色彩转换；换层格式后读到的是该层真实格式，
RGBA8 才允许把缓冲直接当剪贴板内容。

**2. 两种剪贴板格式。** 自定义 `Compositor Layer Pixels`
（`"CGP4"` 魔数 + 宽/高/原点/预乘标记 + 预乘 RGBA8）保证**程序内逐字节相等**；
同时写 `CF_DIBV5`（直通 alpha、自下而上 BGRA、`BITMAPV5HEADER` +
`BI_BITFIELDS` 标准掩码）供跨应用。读时优先自定义格式，退化到 DIB；
32 位 DIB 的 alpha **全零**时按不透明处理（不少老程序不写 alpha，
照抄会把整张图当全透明）。

**3. 拷贝路径分两档。** 图层轴对齐 RGBA8（`|rotation| ≤ 1e-4`、无翻转、
format 为 RGBA8、transform 尺寸 == 像素尺寸）→ 直接读原始像素，原点取
transform 左上角；否则退化为 `comp_render_layer_thumbnail(doc, id,
max(canvas_w, canvas_h))` 取画布尺寸单层合成（"所见即所拷"），
尺寸对不上画布直接报错，**不悄悄缩放**。有选区时按文档空间 coverage 裁剪：
预乘空间里直接乘覆盖度，`coverage == 255` 时整字节拷贝，满覆盖像素零损耗。

**4. 剪切 / 清除更严。** 同一 1:1 RGBA8 条件不满足就报错，
不把 16 位/HDR 层降级成 8 位、也不抹错区域。剪切先写剪贴板、写成功才清像素；
状态栏写明「未改动」。

**5. 粘贴 = 新图层 + 一步撤销。** `comp_txn_begin` → `comp_layer_add` →
`comp_layer_set_pixels`（贴入选区时再按**新层像素尺寸**建蒙版，索引的是文档空间
选区覆盖度）→ `commit`；任一步失败 `abort`。落点优先取当前选区 bounds 左上角，
无选区时用自定义格式里记下的原点（跨应用 DIB 落 0,0）。100 MP 上限挡住误粘超大图。

**6. 键位不改既有语义。** `Ctrl+C` / `Ctrl+V` 以前会掉进工具快捷键
（C = 裁剪、V = 移动），现在在 `handle_key` 里先截住并 `return`；
`Ctrl+Shift+V` 仍是切图层可见性、`Del` 仍是删图层——所以自检 257 项 SHA 不变。
清除只挂 `Backspace`。

**7. 验证。** 单测 **626 例 / 0 失败**（`comp_layer_get_pixels` 往返与越界护栏）；
壳自检 **257 项 failures=0、SHA256 `377DE660D48AC933…` 不变**；
`run_regression.py --require-ohos` **6/6**；`check_tool_table.py` PASS（20 工具未变）；
`visual_check.py` **97 条 / 20 场景 / 0 失败**——新增 `clipboard` 场景 6 条。

**8. 边界。** ① 跨应用只有 32 位 DIB，16 位/浮点会被压到 8 位；② 不写
`CF_BITMAP` / `CF_DIB`（老程序兼容面），也不做 ICC 转换（与 P0-9 关联）；
③ 拷贝大图层时自定义格式占用 4×W×H 字节（无压缩），100 MP 上限即求解脱。

### 5.47 裁剪拖框：从起点重算 + 松手即停（2026-10-22）

跑 97 条视觉探针时 `crop` 场景间歇失败（提交后的画布比拖出的框大一截），
定位到两个独立缺陷，均在本轮修掉：

**1. 框在上一帧上累积。** 旧 `crop_extend` 用 `min()` 往 `crop_rect.x/y` 里收，
框只会单向变大：拖拽中抖一下、或回车前又移动一下鼠标，框就永久撑大、收不回来。
改为记住**按下点** `crop_drag_start`，每帧由「起点 → 当前点」重新规范化。

**2. 松手后的鼠标移动仍在改框。** `crop_active` 一个标志同时表示「框已定、等回车」
和「正在拖拽」，于是松手后任何 `WM_MOUSEMOVE` 都继续重算框。真人操作时表现为
「松手后挪鼠标，框跟着变」；脚本注入（`PostMessageW`）时更严重——松手后还会来
一条迟到的 `WM_MOUSEMOVE`，回车提交的就是被它改过的区域。
现在用独立标志 `crop_dragging`：`crop_begin` 置位，`WM_LBUTTONUP` 以**松手坐标**
规范化一次后立刻清零，`WM_CAPTURECHANGED` 也清零（捕获被抢走时框保留、
仍可回车提交，但不再跟随鼠标变形）。

行为不变的部分：拖拽中框实时可见、`Enter` 提交 / `Esc` 取消、提交走
`comp_crop_apply` 一步撤销。自检 257 项与 SHA **未变**（`crop` 只有视觉探针覆盖）；
`visual_check.py` 97 条 / 20 场景 / **0 失败**，`crop` 场景 7 条连续复跑稳定。

### 5.48 关闭 / 退出前的未保存提示：面板内三选一（P0-3，2026-10-22）

**旧行为**：`WM_CLOSE` 无条件 `DestroyWindow`，脏文档直接丢。**新行为**：先问
`comp_document_is_dirty`，脏则打开右栏的 `DocDialogKind::ClosePrompt` 面板，不脏才销毁。
零 ABI 新增（导出数仍 **146**）。

#### 为什么不用 `MessageBox`

自动化收尾 `shell.stop()` 是「post `WM_CLOSE` → 等 1.2 s → 超时补一个键 → 等 3 s → kill」。
系统模态弹窗有三处致命：阻塞式，会把每次目视跑拖成超时；不吃 `PostMessageW` 到主窗口的
键（进不了模态消息循环），脚本没法回答；截不到图，探针无从断言。所以提示必须画在我们
自己的面板里——走既有的 `draw_doc_dialog` / `doc_dialog_*` 一套，`present()` 每帧刷。

#### 行模型

`ui_core.h` 只追加一个枚举值 `DocDialogKind::ClosePrompt`，其余全是常量与纯函数：

| 项 | 值 |
|----|----|
| `doc_dialog_row_count(ClosePrompt)` | `1`（唯一一行「动作」） |
| `doc_dialog_row(ClosePrompt, 0)` | `DocRowSpec{"动作", DocRowKind::Choice, 0, 2, 1, 3}` |
| 动作档位 | `kCloseActionSave = 0` / `kCloseActionDiscard = 1` / `kCloseActionCancel = 2` |
| 默认值 | `0` = 保存并关闭（所以「回车」= 安全动作） |
| 文案 | `doc_close_action_name()`：保存并关闭 / 不保存，直接关闭 / 取消（留在文档） |
| 标题 | 「关闭文档（Close）」，副标题「文档有未保存的修改 · 关闭前请选择」 |
| 提示行 | `doc_dialog_hint` = 「不保存 = 这次修改连同撤销栈一起丢弃」 |

提示行**不写键位**：`draw_doc_dialog` 底部本来就有三行固定键位提示，重复一遍在截图里很明显
（第一版就是这么翻车的）。固定键行对 `ClosePrompt` 分支换文案：

```
S 保存并关闭 · D 不保存
回车执行当前动作
Esc 取消（留在文档）
```

其它对话框的这三行一字未动（像素级不变）。

#### 键盘

- `S` → 置 `Save` 后立刻 `doc_dialog_apply`；`D` → 置 `Discard` 后立刻 apply；`Esc` → 取消。
- 这两个字母只在 `ClosePrompt` 下特判，位置在模态块的 `switch (key)` **之前**。其它文档对话
  框继续吞掉字母，工具快捷键（C=裁剪、V=移动、B=画笔…）不会从提示底下穿过去。
- 原有的 Tab/↑↓ 切行、←/→/±/Space 步进、数字直输、回车应用全部照旧。

#### 「不关窗，除非真存下去了」

```
Save 且 document_path 非空 → save_document(path)
                           失败 → 关面板，窗口留着，状态栏「保存失败，窗口未关闭」
Save 且无路径             → save_document_dialog()（另存为）
                           回来后重查 comp_document_is_dirty
                           仍脏 / 路径为空 → 关面板，窗口留着，「未保存（另存为已取消），窗口未关闭」
Discard                   → 直接 DestroyWindow（不落盘）
Cancel                    → 只 doc_dialog_close，文档与撤销栈都不动
```

无路径分支必须**复查 dirty** 而不是只看对话框返回值：`comp_document_save` 成功时内部走
`mark_dirty(false)`（`comp_engine_abi.cpp:240`），所以「存完之后还脏吗」是可靠的落盘判据。
（`文件 > 退出` 是 `PostMessageW(window, WM_CLOSE, 0, 0)`，共用同一条路径。）

提示已经开着时再来一个 `WM_CLOSE`（再点一次关闭键）当作**取消**：不销毁窗口，也不重置
用户已经选好的档位。

#### 测试侧

- `shells/windows/src/ui_core.h` 的常量与行规格由新单测 `ui_core_close_prompt_rows` 覆盖
  （行数/档位数/`min == Save && max == Cancel`/三档名字互不相同/越界动作名与越界行兜底）。
- `visual_check.py` 的 `stop()` 学会在脏文档上补一个 `D`：先 post `WM_CLOSE`，1.2 s 内没退
  就发 `D`，再等 3 s 才 kill。这样所有**脏文档收尾**的场景不再白等超时（此前只是硬等）。
- 新场景 `close_prompt` 5 条：脏文档 post `WM_CLOSE` 后进程仍存活 → 右栏标题区变化 > 300
  像素（实测 8046）→ `Esc` 后仍存活 → 取消后画布 `pixel_diff == 0` → 按 `D` 后 3 秒内退出。
  取文档矩形**必须在涂色之前**：`detect_document_rect` 依赖空文档纯黑，涂成橙色就认不出边框了。

#### 已知边界

- `文件 > 新建` / `文件 > 打开` 仍直接销毁旧文档、不提示（本批未动）。
- 窗口过窄时右栏宽度塌成 0，提示只剩键盘路径可用。
- 崩溃恢复、最近文件仍未做；多文档按 BACKLOG 原文仍是【架构】。

`visual_check.py` **102 条 / 21 场景 / 0 失败**；单测 **627 例 / 0 失败**；
壳自检 257 项 failures=0、SHA `377DE660D48AC933…` 与基线逐字节一致。

### 5.49 工具选项栏：工具参数从右栏搬到工具条下（P0-10，2026-10-22）

**旧行为**：画笔大小、硬度、魔棒容差这些参数只能在右栏面板里改，工具条下空白一片——
和 Photoshop 的位置直觉相反，且右栏窄到塌陷时就没得改。**新行为**：工具条正下方多一条
32 px 的选项栏，按当前工具列出 Stepper（`- 值 +`）/ Choice（循环档位）两类控件。
零 ABI 新增（导出数仍 **146**），全部状态复用既有 `ShellState` 字段。

#### 布局：真的参与排版，不是浮层

浮层会让点击穿透到画布，所以选项栏是**布局的一部分**（`ui_core.h`）：

| 项 | 值 |
|----|----|
| `Layout::kOptionsHeight` | `32` |
| `Layout::kMinBodyHeight` | `96`（画布+面板的最低高度） |
| `options` 矩形 | `{0, toolbar_h, width, options_h}` |
| `body_top` | `toolbar_h + options_h` |
| `body_h` | `remainder_h - options_h` |
| 出现条件 | `remainder_h >= kOptionsHeight + kMinBodyHeight` |

窗口矮到只剩不到 96 px 主体时选项栏高度取 0、整块不画、画布把那 32 px 收回去——
宁可少一个控件，也不让画布被压没。宽窗口（1264 px）实测：`options = (0, 46, 1264, 78)`
即 y 46..78，`canvas.y == kToolbarHeight + kOptionsHeight`。

#### 行模型（纯函数，全部可单测）

`ToolOptionId` 只列有意义的参数：`BrushSize`、`BrushHardness`、`WandTolerance`、
`SelectionFeather`、`GradientType`、`ShapeKind`、`ShapeStrokeWidth`、`CloneSample`、
`CloneAligned`、`HealMode`。`ToolOptionKind` 两档：`Stepper = 0`（`min/max/step`）、
`Choice = 1`（`choice_count`，`max == min + count - 1`）。

| 工具 | 行 |
|------|----|
| 画笔 / 橡皮 / 修饰 / 加深减淡 / 历史画笔 | 大小 1..500、硬度 0..100 step 10 |
| 修复 | 大小、硬度、模式（Choice 3） |
| 仿制图章 | 大小、取样（Choice 2）、对齐（Choice 2） |
| 选框 / 套索 | 羽化 0..250 |
| 魔棒 | 容差 0..255、羽化 0..250 |
| 渐变 | 类型（Choice `kGradientTypeCount`） |
| 形状 | 形状（Choice `kShellShapeKindCount`）、线宽 1..50 |
| 文字 / 抓手 / 移动 / 裁剪 / 钢笔 / 缩放 / 吸管 / 选择 | 0 行（画一行「该工具没有选项栏参数」） |

#### 几何与命中

固定像素组宽，不做流式换行：`kOptionLeftPad = 12`、`kOptionGroupWidth = 150`、
`kOptionGroupGap = 8`、`kOptionLabelWidth = 56`、`kOptionArrowWidth = 18`、
`kOptionValueWidth = 54`、`kOptionControlHeight = 22`。`option_group_rect` 在组宽越界时
返回空矩形（窄窗口只显示能放下的前几组），`option_control_rect` 与
`option_hit` 共用同一份几何——画出来的框就是能点的框。

- Stepper **只有左右两条 18 px 箭头带响应**，中间数值带 `return -1`（避免「点值等于点 +」
  这种手感歧义）。实测 `control.x + 4` → `-1`、`control.right - 4` → `+1`。
- Choice 整个控件都可点，方向恒为 `+1`（`option_cycle_choice` 前向循环，无箭头）。
- `option_hit` 对 0 行工具与越界 y 一律 `-1`；y 判据是 control 矩形，不是整条 bar。

#### 接线

`option_value_text()` 渲染值（`%d px` / `%d%%` / `合并·图层` / `开·关` / `%.1f px`），
`apply_tool_option()` 把 delta 写回**同一批** `ShellState` 字段（`step_brush_size`、
`step_brush_hardness`、`step_wand_tolerance`、`step_selection_feather`、
`step_shape_stroke_width`，Choice 走 `option_cycle_choice`），所以选项栏和右栏永远是同一个
数值，不存在两套真相。命中顺序在 `WM_LBUTTONDOWN` 里是：工具条按钮 → **选项栏** →
§5.43 面板开关 → 画布。点完只在状态栏回一行「标签：值」并 `InvalidateRect`。

#### 测试侧

- 新单测 `ui_core_tool_options_bar`：逐工具行数（画笔 2 / 仿制 4 / 修复 3 / 魔棒 2 /
  选框 1 / 渐变 1 / 形状 2 / 抓手·移动·文字 0）、规格合法性（标签非空、`min <= max`、
  Choice 的 `choice_count == max-min+1` 且 ≥2、无重复 id、越界索引兜底 `None`）、
  几何（首组 x == 12、组宽/间距、负索引与空 bar → 空矩形、300 px 窄 bar 第二组为空）、
  命中带与循环（`(2,3,1)→0`、`(0,3,-1)→2`、`(9,3,1)→1`、`(1,0,1)→0`）。
- `ui_core_layout_never_overlaps_at_any_size` 增加选项栏不重叠断言，并断言
  「要么 h == 0，要么 h == 32 且 y == toolbar.bottom 且满宽」。
- `visual_check.py` 的 `Shell.layout` 镜像了上面所有常量与新矩形（**不同步就是所有下游场景
  默默量错矩形**，这一节最容易踩的坑）；新场景 `options_bar` 13 条：
  条高 32、条底色、画笔两组、控件底色、标签字形、小圆点橙色像素 > 0、Ctrl+Z 清空后
  画同一点橙色像素 122 → 418（证明参数真的落到引擎）、四次点 `+` 只改值不改画布
  （`pixel_diff == 0`）、同点重绘宽 > 2×窄、形状 Choice 点击值像素变化 326 且画布 0。

#### 已知边界（本批未做）

- **没有数字直输**：Stepper 只能点箭头/±，值域靠 clamp，不能键入精确值。
- **DPI 未做**：高 DPI 下 32 px 与 22 px 控件仍是物理像素，会被系统缩放模糊；整套
  UI 缩放是独立一批（要改全局度量），本批不半做。
- 窄窗口只显示放得下的前几组，不换行、不横向滚动。
- 浮动面板（P0-10 的另一半）仍未做。

`visual_check.py` **115 条 / 22 场景 / 0 失败**；单测 **628 例 / 0 失败**；
`check_tool_table.py` PASS（20 工具）；`run_regression.py --require-ohos` 6/6；
壳自检 257 项 failures=0、SHA `377DE660D48AC933…` 与基线逐字节一致。

### 5.50 选区修改补全：边界 / 平滑（P0-5 第一批，2026-10-22）

P0-4 接了扩展/收缩，P0-5 的「修改补全」还剩四条：边界、平滑、扩展至相似、选区变换。
本批落地前两条——引擎侧只**追加两个枚举值**（`COMP_SELECTION_BORDER = 3`、
`COMP_SELECTION_SMOOTH = 4`），导出数仍 **146**；两条命令复用 P0-4 已经打通的
`comp_selection_modify` 通道，ABI 没有新增函数。

#### 边界：环带总宽 amount，奇偶都对称

```
outer = dilate(ceil(amount/2))          # resize_base 的正距离
inner = erode(floor(amount/2))          # resize_base 的负距离；0 时跳过
band  = outer * (255 - inner) / 255     # 8 位乘法相减，软边仍是软边
```

`amount = 1 → 只有外侧 1 px`，`2 → 外 1 + 内 1`，`3 → 外 2 + 内 1`，环带总宽恒为
`amount`，奇数时外侧多 1 px。这与 PS 的 Select > Modify > Border 同口径（Border 21
得到约 21 px 宽的环带）。**本批同时修了一处文档与实现不一致**：`selection.h`、
`comp_selection.h` 与 `border_base()` 的注释原先写「两侧各 amount 像素（总宽
2×amount）」，而代码、单测（`selection_border_keeps_a_ring_on_both_sides` 断言
Border 2 只保留 7/8 与 23/24 各 2 列）和实测（Border 21 → 屏幕上原线上下各一条
新蚂蚁线，间距 10 px ≈ 20 文档像素）都是「总宽 amount」。改的是注释，不是行为。

覆盖率相减**不能**用布尔与非：羽化过的边缘是半选，布尔相减会把它变成硬边；8 位乘法
相减后半选仍是半选，这正是 PS 里「羽化后的选区做边界」的观感。两趟外扩/内缩都复用
`resize_base` 的精确欧氏距离场，所以环带内外沿的位置与扩缩命令一致。

#### 平滑：开 + 闭的形态学，直边逐像素不变

```
opened = dilate(erode(mask, r), r)      # 削掉比 2r 窄的凸起/毛刺
closed = erode(dilate(opened, r), r)    # 补掉比 2r 窄的凹口
```

`erode_binary` / `dilate_binary` 都走 `edt_1d` / `edt_2d` 的**精确欧氏距离场**，
不是方框核；判定用 `sqrt(field) - 0.5 >= r`（腐蚀）与 `<= r`（膨胀），所以整像素
直边是恒等的（腐蚀再膨胀把它逐字节复原），只有拐角与毛刺被改。窗口取
`bounds ± (r + 2)` 并补一圈零边界，画布外的部分按「未选中」处理——这是腐蚀的
正确语义，也是窗口不需要整幅图的原因。输出是二值 0/255：平滑的定义是改形状，
不是改软边；要软边请随后羽化。半径 1..100（`kMaxSelectionSmooth`）。

`smooth_base` 的结果与旧蒙版**逐字节相等**时，`selection_modify` 走 `masks_equal`
短路、`out_changed = 0`，壳不占撤销步——所以「对规整矩形反复平滑」不会堆出一串
空撤销。

#### 壳侧：菜单、键入态与量程

| 项 | 值 |
|----|----|
| `kCmdSelectBorder` | `1058` |
| `kCmdSelectSmooth` | `1059` |
| 菜单项 | 选择 > 边界(&B)... / 平滑(&S)... |
| 量程 | 边界 1..500（与扩展/收缩同为 `kMaxSelectionGrow`）、平滑 1..100 |
| 键入态 | 与扩展/收缩/羽化共用一套：`begin_modify_input` → 数字/退格/回车/Esc → `commit_modify_input` |

四条「修改」命令的半径量程不同，但状态机只有一份：`modify_op` 记住是哪条命令，
`modify_op_min/max/clamp` 按 op 选量程——这样菜单、快捷键、键入解析不会各写一遍
夹取逻辑。上次用过的半径共用 `selection_grow` 一个字段，`begin_modify_input` 用
`modify_op_clamp(op, selection_grow)` 显示「上次 N px」，从边界切到平滑时 500 会
被夹成 100 而不是显示一个必然非法的值。0 在壳侧就拦成提示（`半径必须 ≥ 1 px`），
不把必然 `INVALID_ARGUMENT` 的调用打到引擎上。

顺带修掉一个 P0-4 遗留的输入态缺陷：此前「缓冲非空」被当成「正在键入」，于是在空
缓冲上按退格会被工具快捷键吃掉、重开菜单也不会清状态。现在显式记录
`feather_input_active` / `modify_input_active` 两个布尔，键入态的开关与缓冲内容
解耦（Esc 退出、回车提交、菜单重开都复位）。这个缺陷 `--selftest` 测不到（不走
消息泵），是本批用视觉探针量出来的。

#### 测试侧

- `tests/test_selection.cpp`：`selection_border_keeps_a_ring_on_both_sides`
  断言 Border 2/1/3 的环带位置、中心为空、拐角抗锯齿（`0 < c < 255`）、不动羽化
  半径；`selection_smooth_rounds_corners_and_keeps_straight_edges` 断言直边四边
  逐像素保留（255）、直角被磨圆（`< 255`）、2 px 宽的刺被 2 px 半径削掉、
  蒙版总和下降。
- `tests/test_ui_core.cpp`：`ui_core_selection_smooth_clamps_to_engine_range`
  （0 / 负数 → 1，100 边界保持，101 / 5000 → 100，并与 `kMin/kMaxSelectionSmooth`
  同值）。
- 单测 628 → **631 例 / 0 失败**。

#### 视觉探针 `selection_modify`（7 条）

蚂蚁线是**白/黑交替且逐帧走动**的虚线，所以任何逐像素对比都会把相位差当成几何差
（本批一开始就踩了：拿 1 px 窗口做 `pixel_diff`，读到「上 5 下 237」）。能稳定量的
只有结构：哪几行有横贯画布的长亮行、贴角的小窗口里有没有虚线。新增两个助手
`long_bright_rows()`（长亮行扫描，`min_px` 滤掉角手柄）与 `brightest_column()`
（定位竖边所在列）。

| # | 断言 | 实测 |
|---|------|------|
| 1 | 前置：矩形上边是 1 条横贯长亮行 | `[238]` |
| 2 | 边界 21 px：原线消失、上下各出一条 | `[233, 243]`（原线 238；两侧间距 10 屏幕 px ≈ 20 文档像素，与「总宽 21」一致） |
| 3 | 状态栏报半径 | 状态栏变化 2286 像素 |
| 4 | 平滑 100 px：直边留在原行 | `[238]`（开闭运算对直边恒等） |
| 5 | 前置：贴角两窗（竖 3×18、横 22×1）都有虚线 | 10 / 10 |
| 6 | 平滑 100 px 磨圆直角（两窗清空） | 10→0 / 10→0 |
| 7 | 平滑撤销一步回到矩形 | 长亮行 `[238]`、两窗 8 / 12 复原 |

半径取 21 / 100 而不是 3 / 30 是量出来的：49% 缩放下虚线是「4 亮 4 暗」8 屏幕像素
周期，30 文档像素的圆角只把边线起点推出约 15 屏幕像素，和相位同量级；100 文档像素
≈ 49 屏幕像素，贴角 8~26 px 这一段的圆弧离边线还有 6 px 以上，扫描窗里只剩原来的
直边虚线，平滑后必空。另外每个角上有 5×5 的白色选中手柄，扫描窗必须避开它（这也是
「取第一个亮像素」方案失败的原因：手柄被当成选区边线）。

#### 已知边界

- **扩展至相似（Grow）没做**：按邻居颜色扩散需要新的 ABI 导出（选区 + 参考点 +
  容差），本批坚持零新增，留到下一批。
- **选区变换（Transform Selection）没做**：§5.44 的 8 把手只改矩形选区，任意
  路径/魔法棒选区的自由变换需要新的重采样入口。
- 平滑是二值输出：半径 100 会把羽化过的软边压成硬边（PS 同款行为），需要软边请
  平滑后再羽化。
- 边界在画布边缘会内缩（外扩被画布裁掉），与扩展命令同口径。

`visual_check.py` **122 条 / 23 场景 / 0 失败**；单测 **631 例 / 0 失败**；
`check_tool_table.py` PASS（20 工具）；`run_regression.py --require-ohos` 6/6；
壳自检 257 项 failures=0、SHA `377DE660D48AC933…` 与基线逐字节一致。

### 5.51 可编辑文字图层：文字对象 + 渲染缓存（P0-6 ①，2026-10-23）

P0-6 的设计是「文字图层可反复编辑」，本批落地①（文本图层模型 + 无损重编辑），
②字体枚举与③段落框不在此列。ABI **尾部追加 4 个导出**（146 → 150），既有导出与
结构体布局一个没动。

**1. 数据模型：对象是真源，像素是缓存。** `Layer::text`（`std::optional<TextObject>`）
持有文本、原点、样式、颜色、不透明度与**字体文件路径**；`Layer::pixels` 恰好是文本框
大小的渲染结果，`Layer::transform` 也设成同一个框。合成、缩略图、合并、拼贴、GPU
纹理全部走既有路径，**合成内核一行未改**。这样做的代价是每层文字多占一份像素，
换来的好处是「改字号」只需重渲染这一层——`.comp` 重开、撤销重做、混合模式、蒙版、
效果全部照旧生效，不需要为文字层另开一条合成分支。

对象挂在 `Layer` 上而不是单独的注册表：`DocumentState` 快照本来就覆盖整份图层数组，
所以**撤销/重做天然一致**，没有新的历史记录类型，也不必在每条事务里记得登记/注销。

**2. 四个导出。** 全部只追加在 `comp_layer.h` 尾部：

| 导出 | 语义 |
|------|------|
| `comp_layer_add_text(doc, desc, out_id)` | 新建文字图层，放在活动层上方，一步撤销 |
| `comp_layer_set_text(doc, id, desc)` | 改写已有文字层并重渲像素，一步撤销 |
| `comp_layer_get_text(doc, id, desc, buf, cap, out_required)` | 只读回读；文本两段式（`buf == NULL` 只回长度） |
| `comp_layer_rasterize_text(doc, id)` | 丢掉文字对象、像素留下；非文字层幂等 `COMP_OK` |

`comp_text_desc` 里存的是**字体文件路径**而不是 `comp_font` 句柄：对象要进 `.comp`，
句柄存盘后即失效，路径 + `font_face_index` 才能跨会话还原。`font_family` 只用于回显，
不参与渲染——族名在不同机器上可能指向不同文件，拿它当键会在跨机打开时静默换字体。

**3. 参数闸在副作用之前。** 空文本 / 超 65536 字节 / 旧轨道含 UTF-8 或 Tab /
字号行距字距越界或非有限 / 原点非有限或绝对值 > 1e6 / alpha 为 0 / `opacity` 不在
`(0,1]` / `_reserved` 非 0 一律 `COMP_ERR_INVALID_ARGUMENT`；文本框与画布完全不相交也是
`INVALID_ARGUMENT`；面积超过画布上限量级是 `COMP_ERR_UNSUPPORTED`；字体打不开是
`COMP_ERR_IO` / `COMP_ERR_DECODE`。全部发生在**开事务之前**：不建层、不消耗图层 id、
不动旧像素、不进撤销栈。渲染先写进局部缓冲、最后 `move` 出去，失败没有半成品。

**4. 文字层上的像素写入一律拒绝。** `comp_layer_set_pixels` 与 `comp_tool_apply`
遇到 `layer->has_text()` 直接 `COMP_ERR_STATE`（引擎原文 `layer is an editable text
layer; rasterize it before running pixel tools`）。不静默接受的理由很具体：文字层
像素是「对象当前渲染结果的缓存」，下一次改字号就会整层重渲，用户手绘的笔迹会被
抹掉——这正是上游「文字图层上不能直接落笔」的同一条规矩。要画就先栅格化。

`comp_layer_rasterize_text` 是这道闸的出口：幂等（非文字层返回 `COMP_OK` 且不记
撤销步），成功时**恰好一步撤销**，撤销后文字对象与可编辑性一起回来。
`comp_document_resize_image` 会**隐式丢弃**文字对象：非等比缩放无法用「字号 + 文本框」
表达，宁可退回普通图层也不撒谎。

**5. `.comp` 往返。** 图层记录追加**一个**键 `engineText`（缺键 = 普通像素层，旧工程
逐字节照旧读；与 `engineIsolated` 同一条「默认值不写」的规矩）。对象内部的键：

```
"text": string, "origin": [x, y], "fontSize", "lineHeight", "letterSpacing",
"antialias", "color": [r,g,b,a], "opacity",
"fontPath"（可缺省）, "fontFaceIndex", "fontFamily"（可缺省）
```

读回后同样过一遍 `text_layer_validate`，**不可渲染的对象整块丢弃、图层退回普通
像素层**（像素仍从 `imageFile` 照常读），坏数据不留一个半修复的文字层。
绘图几何仍写在上游的 `transform` 里，所以只认图像的第三方读者看到的仍是渲染结果。

**6. 壳侧：提交 = 新建或改写，靠落点分。** `type_commit` 只做两件事：插入点落在
**选中的文字图层框内** → `comp_layer_set_text`（改这一段）；否则 → `comp_layer_add_text`
（新建一层）。框内判定用 `comp_layer_get_transform` 的矩形做包含测试，**不另设一份
真相**。这条分流是必需而不是讲究：第一次提交后新层就是选中层，若第二次点击别处也走
`set_text`，上一段文字会被悄悄搬走（用户看到的是「上一段消失了」）。本批在写视觉场景
时先踩到这一条，才补的 `type_origin_inside_layer`。

「是不是文字层」也全部问 ABI：查询模式下 `comp_layer_get_text` 返回 `COMP_OK` 即文字层、
`COMP_ERR_STATE` 即普通像素层。同一个判据被三处复用——提交分流、菜单置灰、栅格化入口，
壳里没有第二份「文字与否」的缓存。

字体路径从 `load_shipped_font` 一路带进 `ShellState.type_font_path`（多了一个默认为
`nullptr` 的出参，既有两参数调用不受影响），提交时写进 `comp_text_desc.font_path_utf8`；
走内置 5×7 轨道时传 `NULL`。

**7. 图层 > 栅格化文字。** 新命令 `kCmdLayerRasterizeText = 1572`，处理函数
`panel_rasterize_text`：无层 / 非文字层 / 引擎失败各有独立文案，成功刷新面板与历史。
置灰规则放进了纯函数 `comp_ui::layer_menu_enabled`（`LayerMenuState` 新增 `is_text`、
`LayerMenuEnabled` 新增 `layer_rasterize_text`，条件为 `has_layer && is_text`），
因为 `EnableMenuItem` 本身无法单测；`sync_menu_state` 只负责把 ABI 查询结果填进
`is_text` 再搬给菜单。键盘路径刻意不受置灰影响（与 §5.41 同一条规则）。

**8. 测试。** 引擎侧新增 `tests/test_text_layer.cpp` 8 例：增删改回读往返、
`get_text` 对普通层报 `STATE`、改样式只动该层其它层逐字节不变、样式变更恰好一步撤销、
像素在栅格化前只读（`set_pixels` 与 `comp_tool_apply` 都 `STATE`）、非法参数无副作用
（不建层、id 不消耗、撤销深度不动）、`.comp` 往返后仍可编辑、**无 `text` 键的旧工程
逐字节照读**。UI 层新增 1 例 `ui_core_layer_menu_rasterize_text_needs_text_layer`。
单测 631 → **640 例 / 0 失败**。

壳自检 **257 项 failures=0、SHA256 `377DE660D48AC933…` 与基线逐字节一致**——本批没有
改动任何既有自检路径（新增的文字图层入口不在 `--selftest` 的自检项里），基线未重新生成。

**9. 视觉探针 `text_layer`（6 条，追加在 `SCENES` 末尾）。** 只加场景，不改任何既有
阈值或命令 id：

| # | 断言 | 实测 |
|---|------|------|
| 1 | 提交文字后图层面板多出一行（新建的是图层） | 面板浅色像素 1036 → 1523 |
| 2 | 新建的文字图层在画布上有字形 | 29 像素（32 px @ 49% 视图，与 `type` 场景同口径） |
| 3 | 框内再提交仍是同一图层（行数不变） | 1523 → 1523 |
| 4 | 改文字后字形变宽（同一层重新渲染） | 29 → 57 像素，宽 8 → 20 px |
| 5 | 栅格化后再提交会新建另一层 | 1523 → 2001 |
| 6 | 撤销两步回到单层文字 | 1523（与栅格化前一致） |

第 1/3/5 条数的是**图层面板的浅色像素**而不是行数：面板不对外暴露行几何，多一行
文字就多一截亮像素，这个量在既有场景里一直这么用（§5.49 的 `layers` 断言）。
第 6 条同时钉住栅格化与 `add_text` 各占一步撤销。加上本场景后全量为
**128 条 / 24 场景 / 0 失败**（连跑两次一致；`_park_pointer()` 加固见 §5.52）。

**10. 边界（如实记）。** ① 没有字体枚举/选择：只有「随仓字体 ↔ 5×7 点阵」，加字体要
P1-4；② 没有段落框、对齐、竖排、变形；③ **框内再次编辑是「在哪里点就在哪里重排」**，
不是上游的「插入光标」：壳没有逐字符光标（§5.45 边界 ③），`type_begin` 记的就是点击点，
所以先拖动文字层、再点框内改字，文字会以点击点重新排——要「原地改」得先记住框的左上角
（引擎侧没有「只改原点」的轻量入口，`set_text` 必须带完整 desc）。④ 每次改字号整层
重渲，大文档有可见延迟（没有「按需栅格化」的省内存优化）；⑤ 文字层上不能落笔、
不能抠图，必须先栅格化——这是刻意与上游对齐的闸，不是缺陷。

### 5.52 视觉探针加固：把物理指针停到状态栏（2026-10-23）

**症状。** 全量跑 `visual_check.py --scene all` 时，`options_bar` 的最后一条
「Choice 点击不穿透到画布」偶发失败，报「画布变化像素 17」；同一个场景单跑
`--scene options_bar` 必过。2026-10-23 连续两次全量跑都在同一条、同一个 17 px，
不是随机抖动。

**定位。** 把 50/51 两张截图逐像素做差，17 个点全部落在画布内一个半径约 5 px 的
虚线圆上（`(423..433, 425..436)`），圆心正好等于**真实鼠标的客户区坐标**。这个圆
是 `shell_paint.cpp:2355` 的笔尖轮廓（`PS_DOT` 白圈），画在 `shell.cursor_x/y` 上，
而 `cursor_x/y` 只由 `WM_MOUSEMOVE` 的 lparam 更新。探针用 `PostMessage` 合成鼠标
事件、并不移动真实指针，所以只要**物理指针恰好在画布内**，窗口激活/悬停时系统补的
一条真实 `WM_MOUSEMOVE` 就会把合成拖动的坐标顶掉，之后第一张截图带着这个圈、点完
选项栏后圈消失，被「画布逐字节不变」的断言算成穿透。

**修法（只改探针，壳与引擎零改动）。** `Shell.start()` 在激活窗口、读到客户区尺寸
之后调用新增的 `_park_pointer()`：用 `SetCursorPos` 把物理指针停到**状态栏中央**
（画布外、且状态栏没有悬停高亮，不会引入任何新像素），再等 0.15 s 让真实
`MOUSEMOVE`/`MOUSELEAVE` 送完。场景列表、断言阈值、命令 id 一个没动。

**验证。** 修前：两次全量跑各 128 项 1 失败（都是这一条）；单跑 13 项 0 失败。
修后：**连续两次全量跑 128 项 / 24 场景 / 0 失败**，其余 122 条断言的实测值与修前
逐条一致。顺带记一句环境事实：这条断言此前能过，靠的是「鼠标恰好不在画布上」，
属于运气——不是壳的 bug，也不是探针算法的错。

### 5.53 字体枚举与选择器：平台知识在壳、像素在引擎（P0-6 ②，2026-10-23）

`docs/ps-parity.md` §4 的 ROI 第 1 顺位。文字维在日常路径里原本只有 55%，卡点不是
渲染而是「选不了字体」：外壳只加载一支随包 `NotoSansSC-Regular-Subset.ttf`。
本批补这条产品通路，**ABI 零新增**（`^COMP_API` 仍 152）。

**1. 为什么在壳里枚举，且不需要新 ABI。** 引擎的字体 ABI 早就够用：
`comp_font_load_file`（UTF-8 路径 + 面号，超过 100 MB 拒收）、`comp_font_get_metrics`
（`face_count` / `scalable` / `units_per_em` / `has_cjk`）、`comp_font_name`
（族名 / 样式名）、`comp_font_release`。缺的只是「把系统里的字体列出来让用户挑」。
字体装在哪个目录、认哪些扩展名、per-user 装在哪，是**平台知识**，引擎不该知道；
所以目录扫描与新面板全部在 `shells/windows/src/shell_fonts.cpp`，像素与排版仍然
全部回引擎。壳只保留字符串，不持有 `comp_font`——`comp_font` 没有全局缓存，句柄
常驻就是几十 MB。

**2. 扫描：分片，且每面读完立即归还。** 候选顺序是「随仓字体 → `%WINDIR%\Fonts` →
`%LOCALAPPDATA%\Microsoft\Windows\Fonts`」，只认 `.ttf/.otf/.ttc`，路径小写去重
（Windows 大小写不敏感）。`.ttc` 按面拆条目，每份文件最多 `kFontScanMaxFaces = 8`
个面；`units_per_em == 0` 的位图字体不进列表（排不了版）。每个面都是「打开 → 读名字
与度量 → 立即 release」，`probe_face` 不持有任何句柄。

同步扫一遍 153 个文件实测约 **1.2 s**，足以冻住 UI。所以扫描走 `WM_TIMER`（id 2，
30 ms 一片，每片 `kFontScanPerTick = 6` 个文件）：`font_scan_start` 同步探第一片，
面板打开立刻有内容，其余片在后台补齐；期间可滚、可应用已探到的条目，状态栏与面板
标题都显示 `扫描中 n/m`。面板关闭时 `KillTimer`，但 `cursor` 与已探条目保留，下次
打开接着走。

**3. 排序与选中按身份恢复。** 面板顺序是「内置点阵 → 随仓 → 含 CJK → 名字
（大小写不敏感）」，只在扫描**全部结束**时排一次。排序会打乱 `font_filtered` 里存的
`font_entries` 下标，所以结束前先按 `path + face_index + builtin` 记下当前选中项，
`stable_sort` 后重建 `font_filtered` 再按身份找回——否则面板会在扫描结束时跳到某支
无名字体上（这是写实现时先想到、再用 `stable_sort` 保证同名同面顺序稳定的理由）。

**4. 右栏单面板不变式，双向让位。** 字体面板打开时先关路径 / 历史 / 裁剪 / 效果 /
滤镜面板（色彩范围持有显式事务，先 `color_range_cancel` 再让位），并清掉滤镜预览帧。
反过来 `font_panel_visible` 在那些面板打开时返回 false（`font_open` 与扫描进度保留，
关掉别的面板它就回来）；`type_panel_visible` 同样排除字体面板。这样 paint / 键盘 /
点击 / 滚轮四处的分派顺序只要一致（对话框 → 路径 → 历史 → 字体），就不会出现
「键被一边吃掉、画面画的是另一边」。菜单命令走 `font_panel_toggle`，它先测
`font_panel_visible` 而不是 `font_open`，所以面板被别的面板挡住时，再点一次菜单是
**重新露出来**，不是把它关掉。

**5. 应用：先验证句柄，再改轨道；文字层一步撤销。** `apply_track` 对真实字体必须
先 `comp_font_load_file` + `comp_font_get_metrics` 成功，才换掉 `type_font` 并置
`type_use_font`；失败只报状态栏，不改任何状态——否则用户打完字才在提交时撞
`COMP_ERR_IO`。回到内置点阵不释放已有句柄（切回来无需再读盘）。

两条轨道的字号量纲不同（真实字体按 em 高，内置点阵按格子高），切换时**不换算数值**，
与 `type_panel_toggle_font` 同一口径：用户调好的 48 px 在字体轨道是 48 px 的 em，
回到点阵就是 48 px 的格子；硬换算只会制造一个没人要的 5.2。

选中图层是文字层时，应用字体会重排这一层：`comp_layer_get_text` 两段式取回完整
`comp_text_desc`（含文本、原点、字号、行距、字距、颜色、不透明度），只替换
`font_path_utf8` / `font_face_index`，再 `comp_layer_set_text`——**恰好一步撤销**。
`get_text` 回的 `font_path_utf8` 指向引擎内部字符串，替换前先拷进本地 `std::string`；
`set_text` 返回后不再引用它。非文字层不调用 `set_text`：字体面板改的是「下一次打字」
的轨道，没必要给像素层记一条空撤销步。

切回内置点阵而文本含 CJK 时，引擎如实拒绝（`kBadText → COMP_ERR_INVALID_ARGUMENT`），
状态栏写明「内置 5×7 点阵只排 ASCII；含中文请选真实字体」，不做静默替换。

**6. 键盘 / 鼠标。** 面板可见时是模态的：`↑/↓` 选行、`PgUp/PgDn` 翻页（按面板实际
可见行数 − 1）、`Home/End` 跳两端、`Enter` 应用、`Backspace` 删过滤字符、`Esc`
先清过滤词、再按一次才关面板；其余按键一律吞掉。直接键入字母或中文就是过滤
（`WM_CHAR` 进 `font_filter`，`font_label_matches` 做大小写不敏感的 ASCII 子串与
CJK 子串匹配）。鼠标点击命中的行即应用，滚轮 3 行/格。入口是 `图层 > 字体…(O)`
（`kCmdLayerFonts`）与文字属性面板「字体」行上的回车。

**7. 测试。** `tests/test_ui_core.cpp` 新增 2 例：一例钉行几何与滚动
（`kFontRowHeight = 22`、`kFontRowsTop = 80`、footer 88、可视行数公式、
`font_clamp_scroll` 上下界、`font_ensure_visible`、末行不压 footer、`font_row_at` 的
表头 / footer / 越界 / 空列表都返回 −1）；一例钉过滤匹配（`ans`、`sc` 的大小写不敏感
子串，`思源`、`黑体` 的 CJK 子串，`宋体` 不匹配，`思源 黑体` 因空格不匹配，内置条目
能被 `ascii` 命中）。单测 654 → **656 例 / 0 失败**。

视觉探针追加 `font_picker` 场景 4 条（只加场景，不改既有阈值）：面板出现
（右栏变化 30375 px）、过滤词重绘（14831 px）、第一次 Esc 只清过滤词（相对打开前
34889 px，面板仍在）、第二次 Esc 还原（与打开前差异 **0** px）。全量 128 →
**132 条 / 25 场景 / 0 失败**。壳自检没有走字体面板的既有路径，仍为
**263 项 / failures=0 / SHA256 `2f3b81c4fc599cb7…`**。

**8. 边界（如实记）。** ① 没有回退链：选中面缺字时仍按单面排字，不会自动找同族补字，
仍属 BACKLOG P1-4；② `.comp` 存的是**绝对 Windows 路径**，跨设备打开时同名字体不在
原路径就回落到内置点阵，可移植性需要「字体身份 + 本机解析」两层，属后续；③ 只扫
两个字体目录，不读注册表，「已注册但文件不在目录里」的字体看不到；④ `.ttc` 只看前
8 个面；⑤ 扫描是首次打开面板时才启动（不拖慢启动），但首次打开的那一瞬仍同步扫
6 个文件。

### 5.54 标尺 / 参考线 / 网格 / 对齐（ROI #3，2026-10-23）

`docs/ps-parity.md` §4 的 ROI 第 3 顺位。视图维此前的 60% 是「缩放 + 平移 + 适合
窗口」，缺的是定位工具：量距离、对边、排版的日常动作一个都做不了。本批补上标尺、
参考线、网格与吸附。参考线是文档状态，所以这一项穿过三层：引擎域
（`engine/domain/guide.*`）、ABI（`comp_guide.h` 尾部追加 6 个导出，152 → **158**）
与外壳。**壳层自身零新增导出**，只消费那 6 个入口。

**1. 三条不变式。** ① 吸附只改拖动落点，不参与像素合成，所以对齐开关开与关必须
逐字节同画面；② 一整次拖动 = 一步撤销；③ 值没变只 `abort`，绝不 commit 空步
（`Document::txn_commit` 无条件进历史）。

**2. 标尺叠在画布内部，不切 `layout`。** `Layout::kRulerThickness = 18`，顶部/左侧
两条 18px 带子直接用 `layout.canvas` 的内边缘算矩形（`ruler_top_rect` /
`ruler_left_rect` / `ruler_corner_rect`），`Layout` 一个字段都没改。这不是省事，是
**避免连锁补偿**：一旦从 `layout` 里切掉 18px，缩放锚点、平移夹取、面板分界、
所有命中测试都要跟着减偏移，任何一处漏了就出现「点在线的左边却选中右边的图层」。
代价是标尺会盖住画面最上/最左 18px——所以默认关闭，`Ctrl+R` 才出现。

**3. 刻度是 1-2-5 序列。** `ruler_major_step(zoom)` 从 `{1,2,5} × 10^n` 里挑第一个
满足「屏幕间距 ≥ 56px」的档位；间距 ≥ 40px 时再插 `step/5` 次刻度。数字用
`ExtTextOutW` + `ETO_CLIPPED` 贴着条带内边画，`Segoe UI -11`；角落方块最后画，
盖住两条刻度带的交叉。极端输入有下界保护：`zoom <= 0` 或 NaN 一律回落 5000，
不会让 `log10` 求出负无穷或空序列。

**4. 参考线拖动状态机。** `guide_drag_begin` 分三条路：上标尺 → 新建
**横线**（`COMP_GUIDE_HORIZONTAL`，position 是 y），左标尺 → 新建**竖线**
（`COMP_GUIDE_VERTICAL`，position 是 x），
画布上只在 **Move 工具**下才认已存在的线（其它工具要给画笔让出起笔）。按下即
`comp_txn_begin` 并 `SetCapture`；第一帧 `guide_drag_extend` 才真正
`comp_guide_add` / `comp_guide_move`，因此单击不会留下一条零位移的空撤销步。
`guide_drag_end` **先清 active 状态再做后续**（`ReleaseCapture` 会同步回调
`WM_CAPTURECHANGED`，顺序反了就会重入一个已经结束的拖动）：

- 落在标尺上：本次新建的 `abort`（线随事务回滚，等于「拖出来又拖回去」）；
  已有线 `comp_guide_remove` + commit；
- 位置没变：`abort`；
- 其余：`comp_txn_commit`。

捕获被菜单弹出或切窗抢走时走 `guide_drag_cancel`（abort）。文档替换在
`forget_color_range` 里先 `guide_drag_forget` 再动旧文档——向已释放的 `Document`
abort 就是 UAF，和那里既有的 color_range 注释是同一条理由。

**5. 吸附候选与手感。** 容差 `kGuideSnapPx = 8.0` **设备像素**，经
`snap_tolerance_doc(zoom)` 换成文档像素（8 / zoom），所以 2% 和 800% 下的抓取手感
一致。候选：画布 `0` / 中线 / 另一边，网格可见时「最接近的一条网格线」，其它
**同轴**参考线（被拖的那条排除）。距离相等时保留先出现的（画布 → 网格 → 参考线），
NaN 候选跳过，所有候选就地扫描、不分配数组。命中半径是另一组常量
（`kGuideHitPx = 4.0` 设备像素）——1px 的线太细点不中，太粗会吃掉画笔起笔；
悬停的线画成 2px 高亮（`RGB(140,225,255)`，常规色 `RGB(0,174,239)`）。

**6. 网格与视图开关的归属。** 网格固定 `kGridStep = 50` 文档像素、每 5 格加粗
（`kGridMajorEvery`），不随缩放变密度——变密会让人误以为文档尺寸变了。四个开关
（标尺 / 参考线 / 网格 / 吸附）存在 `DocumentContext` 里而不是 `ShellState`：它们是
「这个文档的视图设置」，换文档应回默认（标尺、网格关；参考线可见、吸附开）。
复位点统一在 `forget_color_range`，三条文档替换路径都经过它。命令号
`kCmdViewRulers..kCmdViewClearGuides = 1580..1584`，勾选态与「清除参考线」的置灰
由 `sync_menu_state` 同步；快捷键 `Ctrl+R` / `Ctrl+;` / `Ctrl+Shift+;` / `Ctrl+'`。

**7. 存盘。** 参考线写进 manifest 的 `engineGuides`：
`[{"axis":0|1,"position":double}]`（`0` = 水平线，`position` 是 y；`1` = 竖直线，
`position` 是 x），空数组不写、缺键即「没有参考线」。位置是 `double` 文档像素，
`12.5` 原样往返。上游同名 `guides` 键**不读不写**：两边语义没有逐字核对过，
静默互读只会让对齐线跳位（`engine/format/comp_file.h` 顶部约定与 README §6.3 已同步）。

**8. 测试。** `tests/test_guides.cpp` 新增 5 例：验参与 `guide_snap_to`（12.5 命中）、
ABI CRUD 各一步撤销（`changed = 0` 不落步）、非法参数拒绝、`.comp` 往返（12.5
两次存盘两次读取都相等，加载后不占撤销栈）、坏条目（非数组/超容量/非对象/字段
非法）显式报错。`tests/test_ui_core.cpp` 另新增 `ui_core_rulers_and_snap_tolerance`：
18px 厚度、三块矩形的几何与 `Rect::contains` 的半开区间边界、1-2-5 序列的
`step * zoom >= 56` 与尾数 ∈ {1,2,5} 及最小性、`ruler_major_step` 在
1.0 / 32.0 / 0.02 / 0 / 负 / NaN 下的取值、`ruler_hit` 的条带归属（隐藏与空布局
返回 0）、`snap_tolerance_doc` 的 `tol * zoom == 8` 对所有缩放成立、非法缩放
回落 400。单测 661 → **667 例 / 0 失败**。

视觉探针追加 `guides` 场景 9 条（只加场景，不改既有阈值）：标尺默认关（边缘着色
0 px）、对齐开关整幅差异 **0**、开标尺 10553 px、拖出参考线 623 px、关标尺后
仍是 623 px、清除参考线 0 px、再显示仍是 623 px、网格变化 21188 px、关网格后与
开网格前差异 **0** px。全量 `--scene all` **145 条 / 27 场景 / 0 失败**（首跑
`blend` 偶发 3 条，单场景与整轮重跑均 0 失败，本轮未改动该场景）。壳自检没有覆盖
标尺/参考线绘制，仍为 **263 项 / failures=0 / SHA256 与基线一致**。

**9. 边界（如实记）。** ① 标尺没有选区范围高亮，也没有「拖动时的虚线预览」；
刻度只用像素，没有单位切换（`ProjectInfo.dpi` 已读，但标尺不换算）。② 参考线
不可旋转/锁定；吸附候选不含图层包围盒和选区边界——那要跨图层树求几何，留下一轮。
③ 网格间距不可配置，没有子网格。④ 吸附只接在参考线拖动上，画笔 / 选框 / Move
的落点还没共用这一套。⑤ 参考线位置是文档像素的绝对值，画布尺寸变小后越界的线
会留在文档外（不自动夹取，避免「改画布就悄悄挪我的线」）。

### 5.55 通道面板：存储 / 载入选区（ROI #5，2026-10-23）

`docs/ps-parity.md` §4 的 ROI 第 5 顺位，选区维 80% → 90%。引擎侧
（`domain/selection_channel.*`、`comp_selection.h` 尾部追加 5 个导出，158 → **163**、
`engineChannels` + `images/<docID>.channel<N>.png`）见 README §6.92；本节只记壳。
壳层仍然**零新增导出**。

**1. 面板同构。** 通道面板直接在 `layout.adjust_panel` 里画，行几何在 `ui_core.h`
（`kChannelRowHeight = 22`、`kChannelRowsTop = 64`、`kChannelPanelFooter = 96`、
`kChannelMaxEntries = 16`），`channel_clamp_scroll` / `channel_visible_rows` /
`channel_row_rect` / `channel_row_at` 全部转调既有的 `list_*`。绘制与命中读同一组
函数，和路径/历史面板一样不存在「画在这一行、点中下一行」。右栏一次只挂一个面板：
打开通道面板时依次关掉滤镜（先 `filter_preview_forget`）、效果、路径、历史；
色彩范围持有显式事务，先 `color_range_cancel` 再让位。反向也对称——其它面板的
打开路径都补了 `channel_panel_close`，文档对话框与换文档路径同理。

**2. 键位是模态的。** `channel_panel_visible() = channel_open && tool != Type`
（文字工具独占右栏时面板挂着不显示，与既有面板同规矩）。`handle_key` 里通道块
排在历史之后、字体之前，闸门是 `!ctrl || key == VK_RETURN`——`Ctrl+回车` 必须放
进来做减选。没有 `alt` 参数（Alt 组合走 `WM_SYSKEYDOWN`），所以加选用 Shift、
减选用 Ctrl，与 PS 的 Shift/Alt 不同，状态栏与面板底部的提示都写明这一点。
编辑键在 `WM_KEYDOWN`；字符走 `WM_CHAR`，中文与输入法从那里进来，排在字体面板
之前、文字缓冲之后。

**3. 命名态的坑。** 面板里按 `S` 会先 `channel_save_begin`，随后
`TranslateMessage` 还会把 `'s'` 以 `WM_CHAR` 再送一次。因此 KEYDOWN 路径在
**成功进入命名态之后**置 `channel_name_skip_char`，`channel_name_char` 第一件事
就是消费它；菜单入口不置位，从菜单进来时第一个字符正常落进缓冲。名字上限按
`narrow_utf8(next).size() > 255` 判断——按 wchar 计数会放过 85 个汉字以上的名字，
到引擎才被拒。空名字回车用 `Alpha N`（`channel_count + 1`，允许重名，引擎不查重）。
提交失败时命名态保持打开，用户改一个字再回车即可，不用从头来。

**4. 载入不走 `finish_new_selection`。** 普通「新选区」命令走那条函数，它会把壳的
「新选区羽化」盖到结果上；通道自带羽化，套上去就不再是存进去的那份选区。通道载入
只做 `refresh_selection` + 历史深度提示：`changed == 0` 时给「与当前选区相同」或
「当前没有选区，减选无操作」并且**不刷新**成「已载入」；`changed != 0` 才走
`finish_selection_command` 并注明「一步撤销」。

**5. 鼠标。** 单击行只选中（载入会顶掉现有选区，误点代价大），双击载入 replace；
单击面板空白吞掉点击，不漏给调整滑杆。命名态下点面板内 = 先取消命名再选行；点到
面板以外 = 先取消命名，点击本身照常继续（工具按钮、画布、其它面板都能用）。滚轮
在面板上翻列表，一格 3 行，夹取在 `ui_core`；调整面板与滚轮分支都显式排除了
`channel_panel_visible` 的矩形。

**6. 命令号与菜单。** `kCmdSelectChannels = 1585`（通道面板…）、
`kCmdSelectSaveChannel = 1586`（存储选区到通道…），挂在「选择」菜单末尾，
`kCmdSelectSaveChannel` 的启用态接 `enabled.selection_save_channel = has_selection`；
面板本身随时可开（空列表也给「还没有通道」的提示）。未注册快捷键。

**7. 测试与边界。** 壳层没有新增单测（几何全部复用 `list_*`，已被
`tests/test_ui_core.cpp` 的列表用例覆盖）；引擎新增 7 例，见 README §6.92。
本轮 `build.py` **674 例 / 0 失败**，`run_regression.py --require-ohos` **6/6 PASS**，
视觉探针 **145 条 / 27 场景 / 0 失败**（未加场景），壳自检 **263 项 / failures=0 /
SHA256 与基线一致**（未改自检场景）。边界：没有通道缩略图与可见性眼睛，不能改名/
复制/直接编辑蒙版；载入只有 replace / 加选 / 减选，没有「反向 / 相交」；面板开关
与选中行不写进 `.comp`。

### 5.56 HiDPI：Per-Monitor V2 感知 + 逻辑像素排版（ROI #7，2026-10-23）

`docs/ps-parity.md` §4 的 ROI 第 7 顺位，视图维 50% → 55%（全功能面）／ 80% → 85%
（日常路径）。引擎零改动、零新增
导出（**163** 不变），全部落在 `shells/windows/src`。完整设计见 README §6.93，
本节只记壳层落点与坑。

**1. 感知在运行时声明，不靠清单。** 构建是 ziglang 直接编译链接，没有 `.rc` /
manifest 管线，所以 `dpi_enable_awareness()` 作为 `wWinMain` 的第一条语句（必须在
任何 `CreateWindowEx` 之前，包括 `--present-bench` 那条路径）调用，按
`SetProcessDpiAwarenessContext(V2)` → `shcore.SetProcessDpiAwareness(2)` →
`SetProcessDPIAware()` 降级；全部失败（`ERROR_ACCESS_DENIED` = 清单已声明）也当
成功。实测本机 `Shcore!GetProcessDpiAwareness` 返回 **2（per-monitor）**。

**2. 排版常量一个都不乘系数。** `present()` 在内存 DC 上开 `MM_ANISOTROPIC`，
window 范围 = 逻辑宽高、viewport 范围 = 设备宽高；绘制与命中测试继续用同一套
逻辑数字。真正的换算点只有三处：`client_layout` 里的 `GetClientRect`、
`window_proc` 顶部的鼠标坐标（`WM_MOUSEMOVE` 的 `lparam`、`WM_MOUSEWHEEL` 与
两处 `WM_SETCURSOR` 的 `GetCursorPos`+`ScreenToClient` 结果）、以及
`main.cpp` 的初始窗口尺寸。`dpi == 96` 时映射模式**完全不开**，所以 100% 下与
改造前逐像素相同。

**3. 字体与映射模式的进出。** 内存 DC 的默认字体是 stock 对象，未必可缩放：
`GetObjectW` 取出 `LOGFONTW` 再 `CreateFontIndirectW` 选入，画完先选回原字体再
`DeleteObject`；随后 `SetMapMode(memory_dc, MM_TEXT)` 才 `BitBlt`——`BitBlt` 的
源矩形也受映射模式影响，留着各向异性会让 GDI 再换算一次。没有用
`SaveDC`/`RestoreDC` 包整帧：`RestoreDC` 会连选中的位图一起回滚。

**4. 顺带修掉 `present` / 命中测试的分叉。** 过去 `present` 自己调
`compute_layout(w, h)`（默认面板宽度），命中测试走 `client_layout()`（用户拖过的
宽度）；拖完分界线只改命中矩形、画面不动。现在两边共用 `client_layout`，这条
历史缺陷是 ROI #7 排查时暴露的，与 DPI 本身无关但不修就会在非 96 下同样发作。

**5. `WM_DPICHANGED`。** 从 `HIWORD(wparam)` 取新 DPI（夹取 96..480），按系统
建议的 `RECT` 走 `SetWindowPos(SWP_NOZORDER | SWP_NOACTIVATE)`，更新
`shell->dpi` 后整窗失效重绘；不 `break` 给 `DefWindowProc`（否则系统会再按老
DPI 调整一次）。跨屏时画布缩放与滚动位置保持不变（`needs_fit` 不重设），这是
用户操作状态，不该被 DPI 事件重置。

**6. `--dpi=N` 诊断开关。** 与 `--selftest` / `--present-bench` 同类：只覆盖壳层
自己的换算基准，不动进程感知、不改窗口像素尺寸。用处是让恒为 100% 的机器（CI、
远程会话）也能复现 150%/200% 的排版与字号；手动测试 M52 用它出图。注意覆盖后
窗口设备像素不变，逻辑面积会变小，面板拥挤是「小窗口」逻辑的既有表现——已用
100% + 843×441 客户区做过对照，两者画面结构一致。

**7. 测试与边界。** 本轮无引擎/单测改动（**674 例 / 0 失败**），
`run_regression.py --require-ohos` **6/6 PASS**，视觉探针 **145 条 / 27 场景 /
0 失败**，壳自检 **263 项 / failures=0 / SHA256 与基线一致**（按既有规矩没加
自检场景，DPI 验证走手动 M52 + 进程感知查询）。边界：画布 `zoom` 仍是逻辑像素
语义（200% 屏上 100% = 2 倍整数放大），要改成 PS 的「1 设备像素」需要把 `View`
变换整体挪到设备坐标系，记入 `BACKLOG.md`；`WM_GETDPISCALEDSIZE` 未实现，跨屏
时窗口尺寸会有一次轻微跳变；没有清单，所以用户的兼容性 DPI 覆盖仍然有效。
---

### 5.57 图像 > 调整：破坏性一次落墨（ROI #8，2026-10-23）

引擎侧见 README §6.94。壳层要解决的是「同一批参数、两种落点」：`图像 > 调整`
下面的每一项都是**破坏性**的一次性落墨（`comp_layer_apply_adjustment`），而
「调整」面板里那套是**非破坏可编辑调整层**。壳内不复制参数表——两边都走
`comp_layer_adjustment_param_*`，只在提交那一步分叉。

**1. 命令号与菜单共表。** `kCmdImageAdjustBase = 1590`，命令 = 基数 + kind；
菜单构建（`shell_window.cpp`）与命令分发（`shell_document.cpp` 的 `default` 分支）
共用 `shell_state.h::kImageAdjustKinds[]`。20 个 kind 都在表里，表外的号一律忽略：
漏加一项只会少一个菜单项，不会出现「菜单能点、命令不认」。前 7 项与后 13 项之间
插一条分隔线（后者是本仓追加、上游没有的类型）。

**2. 破坏性面板态。** 点菜单后 `adj_destructive = true`，`selected_adjustment`
强制为 `-1`。这一个标志同时解决三件事：

- 参数行照旧渲染（标题改成「调整（图像 > 破坏性落墨）」），但 `selected_adjustment`
  为 -1 让 `adjust_write_live` 提前返回、`adjust_begin_drag` 不开事务——**拖滑杆
  只是预览**，不会边拖边落墨；
- `Ctrl+Shift+A`（新增调整层）在破坏性态被拒，避免把一次性参数误存成图层；
- `Ctrl+Shift+U` 走 `panel_apply_destructive` 一次性提交，`Esc` / 切类型
  （`Ctrl+Shift+J/K`）/ 选行（`,` `.`）都退出并丢弃未应用的参数。

**3. 无参数类型点即落墨。** `Desaturate` 与 `Auto Tone / Auto Contrast / Auto Color`
的 `param_count = 0`，进面板态没有意义，所以 `panel_begin_destructive` 对它们直接
提交（`adj_destructive` 保持 false）。**Curves 同样是 0 参数，但不能同样处理**——
它要在面板里拉点，是这条规则唯一的例外。判据取自 ABI
（`comp_layer_adjustment_param_count`），外壳不维护「哪些 kind 无参数」的名单。

**4. 破坏性 Curves 的点列寄存在壳内。** 曲线编辑器原有的写入路径是
`comp_layer_curves_set_points` + `comp_layer_update_adjustment_at`，后者需要
「第几条调整」，破坏性态没有调整对象可写。于是点列先存进
`ShellState::destructive_curve_settings`（纯函数改写 JSON 串，不开事务、不占撤销步），
`Ctrl+Shift+U` 时由 `build_settings_json` 原样交给引擎，`Esc` 时清空。没有这条寄存，
`图像 > 调整 > Curves` 会打开一个拉不动点的死面板——这是本轮实现过程中发现并补上的。

**5. 失败与无变化都如实回显。** 引擎在开事务之前就拒掉非法参数，壳只负责把状态栏
写清楚：HDR（RGBA16F）→「不受支持（未改动）」；组 / 无像素 →「组图层或没有像素的
图层（未改动）」；`changed == 0`（恒等参数或显式空选区）→「结果与像素相同，未进历史」。
最后一条是可验证的：视觉探针 `destructive_adjust` 对同一阈值连续落墨两次，再一次
`Ctrl+Z` 必须回到阈值之前，证明空操作确实没进撤销栈。

**6. 验证。** `scripts/visual_check.py` 追加 `destructive_adjust` 场景，现已 **12 条**
（进态标题带差异、+40 变亮、一步撤销、去色三通道拉平、阈值二值化、空操作不进历史、
色调分离量化、曲线抬中点后落墨变亮、Esc 退出；P1-6 追加 Photo Filter 面板 +
100% 加温、Auto Tone 零参数不面板）。全量 **157 条 / 28 场景 / 0 失败**，
既有 145 条阈值一处未动；96 DPI 的布局与命中路径零改动。

### 5.58 扩大选取 / 选取相似（P0-5 收尾，2026-10-23）

引擎侧见 README §6.96。壳层这轮只做**接线**，没有新增面板——两条命令都是
「点一下就落墨」的选区操作，没有任何可调参数。

**1. 容差复用魔棒的那一份。** `shell.wand_tolerance`（默认 32，`[` / `]` 调）
是壳上唯一的颜色容差，两条命令直接取它，状态栏也把它回显出来。没有为
Grow / Similar 另开一个数值输入：PS 里这两条的容差就是同一个「容差」值，
再多一个旋钮只会让两个数打架。

**2. 命令号 1060 / 1061。** `kCmdSelectGrow` / `kCmdSelectSimilar`，
接在 `kCmdSelectSmooth = 1059` 之后，菜单里排在 `平滑` 下面、与上面四条
（扩展 / 收缩 / 边界 / 平滑）同属「修改」段。置灰规则走既有的
`enabled.selection_modify`（有活动选区才亮），与四条修改项完全一致——
没有选区时这两条本来就无事可做，灰掉比点下去弹状态栏更诚实。

**3. 状态栏三分支。** 成功且有变化 → 「已扩大选取（容差 N，撤销一步）」；
引擎成功但 `changed == 0` → 「扩大选取（容差 N）：无变化」（不进历史）；
引擎报错 → 「扩大选取失败（合成不可用？）」。无文档 / 无选区则根本走不到
命令：前者「选区：还没有文档」，后者「选区：先建立一个选区，再扩大选取/选取相似」。

**4. 视觉探针 `selection_grow`（5 条）。** 形状工具画实心矩形（0066cc），
内部拖一个小矩形选区，然后把上边线当成唯一可量的锚点：

| # | 断言 | 量法 |
|---|---|---|
| 1 | 前置：小矩形上边是一条长亮行 | `long_bright_rows` 在 `my0±16` 恰好 1 行 |
| 2 | 扩大选取后旧线消失、外扩到矩形上边 | `my0±16` 空、`y0±16` 恰好 1 行且 `abs(row-y0) <= 2` |
| 3 | 状态栏报告「已扩大选取」 | 状态栏带 `pixel_diff > 20` |
| 4 | 选取相似在同色区域已选满时边界不动 | 上面两窗结果不变 |
| 5 | Ctrl+Z 一步退回小矩形 | `my0±16` 回 1 行、`y0±16` 空 |

**填充色必须选 R 通道 < 150 的**（0066cc）。`long_bright_rows` 数的是
`count_bright(threshold=150)`，而 `count_bright` 取的是 **R 通道**直方图——
亮橙色 ff8800（R=255）会把整块矩形都算成「长亮行」，蚂蚁线被淹没。
第一次跑就是栽在这里（窗口 32 行全亮）。用形状而不是画笔同理：画笔硬度
75% 的软边在容差 32 下会一格一格漫进黑底，边界就不在矩形边上，第 2 条
断言会变成一个随机数。

**5. 已知边界。** 容差是**逐通道 Chebyshev**（与魔棒同口径，alpha 参与），
不是 Lab 距离；所以「视觉上接近」不等于一定选中。Grow 只在 4 邻域扩散，
斜角方向的连通性比 PS 的 8 邻域保守——这是引擎侧刻意的选择（见 §6.96），
壳不做补偿。

### 5.59 渐变：五类型 / 预设 / 抖动（P1-7，2026-10-23）

引擎与 ABI 见 README §6.98。壳层没有做色标编辑器，只把「选择」做全：
类型五档、三个预设、抖动开关，全部在工具选项栏里（`G` 工具，3 组）。

**1. 预设是只读表，不是状态。** `ui_core.h` 的 `gradient_preset_stops()` 在每次
`begin_tool_op` 时把预设展开成 `comp_gradient_stop[5]`，`ShellState` 只存
`gradient_preset` 的**下标**。这样 `.comp`/撤销里不会出现「一份预设副本」，
预设表改了，所有用它的落笔跟着改。`gradient_preset_stop_count(preset, &is_foreground)`
单独给出计数与「是否吃前景色」，是因为状态栏要写「色标：彩虹（5 标）」而前景色
只有在 `Gradient` 落到引擎时才现取——两处口径必须来自同一张表。

**2. 反相是几何镜像。** `reverse` 不是把数组倒过来，而是
`position' = 1 - position`、`midpoint' = 1 - table[count-2-i].midpoint`。
原因：中点属于「该色标与左邻」那一段，倒序后左邻变了，中点必须跟着换主人；
单纯倒序会让彩虹的偏置整体翻转方向（首版就是这样，视觉上像「反相了但节奏也反了」）。

**3. 选项栏组数是 3，不是 1。** `tool_option_count(ToolId::Gradient)` 从 1 变 3：
类型（5 档循环）/ 色标（3 个预设）/ 抖动（0·1）。窄窗口下第三组会被截掉——
既有规则「放不下的组不画也不可点」原样适用，没有为渐变开特例。
`test_ui_core.cpp` 的组数断言同步改 1 → 3（这是本轮唯一改动既有断言的地方，
因为组数确实变了）。

**4. 预览几何区分类型。** 线性仍是一条带箭头的轴；径向与角度画同心圆
（角度渐变的等值线也是圆，只是角度从轴方向起算）；对称画**虚线镜像轴**；
菱形画旋转 45° 的虚线方框（顶点 `a ± u·L`、`a ± v·L` 经 `doc_to_screen` 变换）。
只画几何，不画色带——色带要按色标表逐像素算，属编辑器那一批。

**5. 状态栏。** 落笔后写「渐变 / Diamond / 彩虹 + 抖动 · 反向：已应用（5 个色标）」；
引擎带回 note 时改为「渐变 / <类型> / <预设>：已应用｜<note>」。类型与预设用
`ui_core` 的显示名，反向与抖动都是**落笔时**的取值——事后改开关不回溯已有像素。

**6. 已知边界。** 不能增删色标、不能改色标颜色/位置/中点——只能选预设。
PS 的「不透明度中点」与「噪点渐变」未做；渐变映射调整层仍两色，与本工具不同源。

### 5.60 段落框与对齐：参数进选项栏，右栏面板仍是 4 行（P0-6 ③，2026-10-23）

引擎与 ABI 见 README §6.99。壳侧的难点不是画控件，而是**往哪放**：文字面板被
自检冻结在 4 行（`kTypePanelRowCount == 4`，`shell_selftest.cpp` 逐项断言），加一行
就破了 263 项 SHA。所以段落参数放**工具选项栏**（`T` 工具，0 组 → 3 组）。

**1. 选项栏三组。** 框宽 / 框高 / 对齐。前两组是 `Stepper`，第三组是 `Choice`
（左 → 中 → 右 循环）。显示值 `0` 写「自动」而不是「0 px」——0 在内核里是「点文字」，
在界面上写成零会让人以为文本框真的零宽。

**2. 步进是确定性整数。** `step_type_box()`：`0 → 128`，此后 `×1.25` **向上**取整到
16 的倍数；向下 `÷1.25` **向下**取整，`≤ 128` 再往下直接回 0（回到自动）。夹取
`16..8192`。为什么不直接 `×1.25`：浮点会让「点三下」得到 203.125 这种值，既难读，
也会让状态栏、`.comp` 与引擎各自四舍五入出不同的数。为什么上限 8192 而引擎允许
`1e6`：壳是给鼠标用的，8192 已远超任何屏幕文档；真正的上限仍在内核，别的宿主可以
用到更大值。

**3. 预览只给框，不预演字形。** `type_preview_metrics()` 在内容度量之上覆盖声明的
框宽/框高——虚线框与实际写入的图层缓冲同尺寸（框高只裁切）。框内文字的折行位置
是引擎的事，壳不预排（沿用 S11「预览不预演字形」的既定口径）；否则壳里要再实现
一遍折行，两套算法必然漂移。

**4. 提交与往返。** `type_commit()` 把三个值写进 `comp_text_desc` 的尾部字段；
`_reserved` 与 `_reserved2` 显式置 0。插入点在选中文字层框内时走 `set_text`（改这段
文字），框外走 `add_text`（新起一段）——这条分流在 P0-6 ① 就有，段落参数不改变它。

**5. 状态栏。** 只在段落参数非默认时追加「｜段落框 W×H px，居中」。默认点文字的
状态栏一个字都不多——这是「不给默认路径加噪音」的既有规矩。

**6. 已知边界。** 没有竖排、文字变形、首行缩进/段前段后距；框高裁切后不提示
「还有 N 行未显示」；没有逐字符光标（点哪就把插入点放哪，不是点在第 3 个字中间
插入）。段落参数只在选项栏，右栏面板保持 4 行是自检契约，不是遗漏。

### 5.61 画笔预设 / 流量 / 压感（P1-4，2026-10-23）

壳层这条缺口很具体：`comp_tool_op_desc.brush` 早就有 `pressure` 与 `flow`，
但外壳每一笔都写死 `1.0`；选项栏只有「大小 / 硬度」两组；没有预设，也没有
任何数位笔输入。**零新增导出、零结构体改动**（仍 166），全部落在壳层。

**1. 预设是只读表，不是状态。** `ui_core.h` 的 `brush_preset_spec()` 返回
`{名字, 大小, 硬度, 流量}`，`ShellState` 只存下标：

| 下标 | 名字 | 大小 | 硬度 | 流量 |
|---|---|---|---|---|
| 0 | 自定义 | — | — | —（不覆盖当前值） |
| 1 | 硬边圆 | 24 | 100% | 100% |
| 2 | 软边圆 | 80 | 0% | 100% |
| 3 | 喷枪 | 160 | 25% | 35% |
| 4 | 细节 | 5 | 100% | 100% |

下标 0 是**无操作**而不是「硬边圆的副本」：启动时若记成 1，选项栏会把当时的
0.75 硬度显示成 100%，界面第一次就撒谎。任何手动改大小/硬度/流量的动作
（选项栏、快捷键、预设之外的入口）都把下标复位到 0，所以「自定义」永远等于
「当前的数不是任何预设给的」。表里的每个值都在引擎夹取范围内，点哪一档都
不会产生被引擎拒收的笔刷。

**2. 流量是唯一的覆盖旋钮。** `COMP_TOOL_STROKE` 忽略顶层的 `desc.opacity`，
一笔的浓淡只能由 `brush.flow` 表达。壳侧存 `0.01..1.0` 的 double，选项栏显示
`1..100%`、步进 5%，`clamp_brush_flow()` 把 0 / 负数 / NaN 抬到 0.01——下限不是
审美，是「流量 0 = 什么都没画」在界面上没有任何反馈，用户会以为工具坏了。
落笔时再夹一次，壳侧永远产生不出引擎范围外的数。

**3. 压感：采样，然后把消息原样交回去。** `WM_POINTERDOWN` / `WM_POINTERUPDATE`
进来时只做一件事：`GetPointerType()` 确认是 `PT_PEN`，`GetPointerPenInfo()` 读
`penMask & PEN_MASK_PRESSURE` 与 `pressure`（0..1024）→ `pen_pressure_to_unit()`。
**随后照常 `DefWindowProcW`**：系统会把笔输入提升成 `WM_MOUSEMOVE` /
`WM_LBUTTONDOWN`，既有的鼠标绘制路径一行不改就继续工作。不调 `EnableMouseInPointer`
也不独占 `WM_POINTER`——那样等于把整条鼠标路径接管过来，收益只是少两条消息。
两个 API 用 `GetProcAddress` 从 `user32` 现取（与 `shell_dpi.cpp` 同一写法），
老系统上取不到就只是「没有笔压」，不影响任何鼠标行为。

**4. 整笔取平均，不是逐点变化。** 一次笔画在松手时合成**一个** op（整条折线），
笔刷只有一个 `pressure` 标量，所以壳在 `stroke_begin` 清零累加器，
`pen_sample_pressure` 在每个笔消息里 `+=` 并计数，`paint_stroke_end` 用
`average_stroke_pressure()` 求均值。鼠标路径没有笔消息 → `count == 0` → 1.0；
关掉「压感」开关同样写 1.0。默认状态与 P1-4 之前**逐字节相同**。
一个精确度上的小边界：`WM_POINTERDOWN` 先于被系统提升出的 `WM_LBUTTONDOWN` 到达，
那时 `stroke_kind` 还是 `None`，所以**落笔那一条采样不计入均值**，均值只由后续
`WM_POINTERUPDATE` 构成（仍然是一笔一个标量，见已知边界③）。

**5. 选项栏顺序冻结。** Brush / Eraser 从 2 组变 5 组：0 大小、1 硬度、
2 预设（Choice，5 档）、3 流量（Stepper）、4 压感（Choice 开/关）。0/1 的位置
是既有契约（`options_bar` 视觉场景按组 0 点 `+`，`test_ui_core.cpp` 断言组数），
所以新组只能追加在 2 以后。Retouch / Tone / HistoryBrush 仍是 2 组——它们的
落笔不读 brush 的 flow/pressure，多给两组等于给假旋钮。窄窗口下放不下的组
照旧不画也不可点。

**6. 状态栏只在非默认时加尾巴。** 流量 100% 且笔压有效值为 100% 时，文案与
以前一字不差；否则追加「 · 流量 50%」「 · 笔压 62%」。这是既有规矩（不给默认
路径加噪音），也是壳自检 263 项 SHA 能保持不变的原因之一。

**7. 验证。** 单测 703 → **704 例 / 0 失败**（`ui_core_brush_preset_flow_and_pen_pressure`：
预设 5 档、0 与越界都落到「自定义」、每档数值都在夹取范围内、流量 0/NaN/负数 →
0.01、越界 → 1.0、步进 ±5% 与方向 0 不变、笔压 1024/512/0/无 mask、均值的 0 样本与
负数兜底）；PE 导出 **166 不变**；壳自检 **263 项 / failures=0 / SHA256
`2f3b81c4fc599cb7…` 逐字节不变**（未加自检项）；`check_tool_table.py` PASS（20 工具）。
视觉探针追加 `brush_options` 9 条（追加在 `SCENES` 末尾，既有阈值未动）：
第 5 组在栏内、预设/流量/压感控件盒可见、满流量落点强橙 122 px、连点流量左箭头
10 次值区变化 145 px、50% 流量下强橙 0 而浅橙 140 px（仍然落墨，只是淡）、
点一次预设后值区变化 522 px 且强橙回到 158 px、压感值文本变化 87 px 且画布
**0 px** 变化。目视项见 `docs/manual-test.md` **M56**。

**8. 顺手修掉的乱码（既有缺陷，不是本轮引入）。** 看 `113_brush_options_full.png`
才发现选项栏所有标签、部分枚举值（`前景→透明`、`左对齐`）与「点选项后的状态反馈」
全是 `å...`：这些窄字符串本来就是 UTF-8，绘制却统一走 `widen_ascii()`（逐字节抬成
`wchar_t`，一个汉字变成三个拉丁字形）。§5.49 引入选项栏起就如此——此前的探针只量
几何和像素数，没有一条读文字。修法放进 `widen_ascii()` 内部而不是改 20 多个调用点：
先 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)`，**不是合法 UTF-8 才**退回
旧路径。纯 ASCII 两条路径逐字符相同（`Brush` / `Normal` / `Exposure` 显示不变），
引擎回传的非 UTF-8 片段也不会被抹成空串。修后选项栏读作「大小 24 px｜硬度 75%｜
预设 自定义｜流量 50%｜压感 开」，状态栏读作「… · 流量：50%」。**修完重跑全量**：
`run_regression.py --require-ohos` **6/6 PASS**（704 例、自检 263 项 SHA 未变、OHOS
ok=118/fail=0），视觉全量 **171 条 / 30 场景 / 0 失败**。

**9. 已知边界。** ① 预设只有 5 个**内置只读档**，不能保存/删除自己的笔头，也不进
`.comp`——预设是壳侧视图态，重开文档回到「自定义」；② 没有间距、圆度、角度、
纹理、双重画笔与任何动态抖动；③ 压感只在 `WM_POINTER` 可用的机器上有，且**整笔
一个平均值**：同一笔里轻重变化画不出来，这是「一次落笔 = 一个 op」的引擎口径，
逐点压感要等笔画分段/逐点 op；④ 笔刷光标仍是系统箭头 + 笔尖轮廓，没有 PS 那种
跟随大小的圆圈（§5.49 的笔尖轮廓已有，但不是「笔刷形状预览」）；⑤ WinTab 未接，
老驱动的数位板不覆盖。

### 5.62 变换选区：数值宽高 + 旋转（P0-5 收尾，2026-10-24）

引擎与 ABI 见 README §6.102。壳层这轮**复用既有的右栏 `doc_dialog` 框架**，
没有新面板类：`DocDialogKind::TransformSelection` 就是三行数字（宽 / 高 / 角度），
渲染走 `draw_doc_dialog`，键盘走既有的 Tab / 数字 / 回车 / Esc 那套。

**1. 命令号 1062，排在「选取相似」下面。** `kCmdSelectTransform`，菜单项
`变换选区(&T)...`，与修改族之间有一条分隔线——它是「整张选区」的操作，不是
「改边界」。置灰规则沿用 `enabled.selection_modify`（有活动选区才亮），
与边界/平滑/Grow/Similar 完全一致。

**2. 打开前先看**真**几何，而不是 `selection_active`。** `selection_active` 对
**显式空选区**也为真（退化的手柄拖拽会产生这种状态），拿它当闸门会开出一个
「宽 0 × 高 0」的空面板。`doc_dialog_begin` 里直接看 `shell.doc.selection_base`
的宽高（这是 `comp_selection_get_base_bounds` 的缓存，正是被变换的那张蒙版），
`<= 0` 就回退并提示「请先建立一个选区」。

**3. 起点是包围盒，回车什么都不填 = 恒等。** 打开时把当前基础包围盒的
`w` / `h` 填进前两行、角度填 0。直接回车走引擎的 `masks_equal` 短路：
`changed = 0`，不落撤销步。壳**不关面板**，只把状态栏改成「选区未变化（未产生
撤销步）」——用户填的数一个不丢，可以继续改。

**4. 中心点现算，不缓存。** 应用时用 `shell.doc.selection_base` 当场求中心
（`x + w/2, y + h/2`）。面板开着的时候选区可能被 `Ctrl+Z` / `Ctrl+Y` 换掉
（撤销会刷新缓存），打开时缓存下来的中心会把它转到一个「已经不存在的选区」
该在的位置上。面板打开期间选区被清空的话，应用分支直接关面板并报
「选区已不存在（未改动）」。

**5. 角度行的量程来自 `DocRowSpec`，输入态不另写。** `kTransformAngleMin = -180`、
`kTransformAngleMax = 180`、`kTransformAngleStep = 15`。`doc_dialog_begin_input`
只对 `spec.min < 0` 的行接受 `-`（且只能在缓冲首位）、缓冲上限 5 字符，
−180..180 正好放得下；`doc_dialog_set_row_value` 按 spec 夹取，不接受越界值。
角度显示成 `90 度` / `-45 度`（`wsprintfW` 没有浮点格式，角度按整数取）。

**6. 状态栏。** 成功且有变化 →「变换选区：W×H · A 度（撤销一步）」；`changed == 0`
→「选区未变化（未产生撤销步）」；引擎拒绝 →「应用失败（文档未改动）」，
面板留着。

**7. 视觉探针 `selection_transform`（10 条）。** 量蚂蚁线用「沿中线扫描」：
横边穿过中线那一列、竖边穿过中线那一行，各取 ±5 px 窗口（盖住蚂蚁线 4 亮 4 暗
的一个周期，与走帧相位无关）：

| # | 断言 | 量法 |
|---|---|---|
| 1 | 前置：矩形选区可量（约 240×120 屏幕像素） | 行/列跨度 `abs(span-2*half) <= 6` |
| 2 | 面板画在右栏 | 右栏标题区 `count_bright > 120` |
| 3 | 恒等回车选区不动 | 行/列跨度与前置逐项相等 |
| 4 | 恒等不落撤销步、面板保持打开 | 回车后右栏浅色像素仍 > 120 |
| 5 | 状态栏说明「选区未变化」 | 状态栏带 `pixel_diff > 20` |
| 6 | Esc 收回面板 | 右栏标题带 `pixel_diff > 20` |
| 7 | 90° 后包围盒交换 | 高 120→240、宽 240→120（±6） |
| 8 | 应用后面板自动关闭 | 右栏标题带 `pixel_diff > 20` |
| 9 | 状态栏报告结果 | 状态栏带 `pixel_diff > 20` |
| 10 | `Ctrl+Z` 一步回到原矩形 | 行/列跨度与前置逐项相等 |

**8. 已知边界。** 只做选区，不动像素（PS 的「变换选区」本来就只改蚂蚁线）；
没有斜切 / 扭曲 / 透视 / Warp；没有实时预览（回车才落）；中心不可选（固定为
包围盒中心，PS 可以拖中心点）；角度只能整数；旋转后的紧包围盒仍是轴对齐的，
所以再次打开面板回显的是**包围盒**，不是上一轮那个旋转矩形。

### 5.63 数值变换 + 再次变换（P1-3，2026-10-24）

零 ABI 新增：引擎的 `comp_layer_get_transform` / `comp_layer_set_transform` 早已
存在，缺的只是壳的命令入口。设计动机与契约见 README §6.103，这里只记壳层实现。

**1. 一个面板两个命令。** `DocDialogKind::TransformLayer`（宽 / 高 / 角度三行）+
`kCmdLayerTransformNumeric = 1063`、`kCmdLayerTransformAgain = 1064`。菜单项挂在
「缩放」下面一行：`数值变换…(&T)\tCtrl+T`、`再次变换(&A)\tCtrl+Shift+T`，置灰复用
`enabled.layer_present`（与旋转/缩放同一档，组/空图层的精确拒绝在打开时做）。

**2. `layer_effective_transform` 把「隐式」和「显式」抹平。** `comp_layer_get_transform`
对没设过 transform 的层报 `w/h <= 0`（含义是「按像素尺寸」）。三处都要用同一个尺：
面板起点、恒等判断、再次变换的基数。宽高 `<= 0` 时用 `comp_layer_get_pixels` 的
查询模式（`pixels == nullptr`）取资产尺寸；`0×0` 回 `false`（组 / 空层）。

**3. 打开闸门在面板之前。** `doc_dialog_begin(TransformLayer)` 先查
`selected_layer_id`，再查 `layer_effective_transform` 成功；任一失败只写状态栏，
不开面板。状态：「请先在图层面板选中一个图层」/「组或空图层没有可变换的像素」。

**4. 角度行：`doc_dialog_angle_base` 保住小数。** 面板是整数行，引擎是 `float`。
应用时 `keep_rotation = (doc_dialog_angle == doc_dialog_angle_base)`——没动过角度行
就写回**原始 float**，只改宽度不会把 22.5° 圆成 23°。

**5. 应用分支。** 走 `panel_apply_layer_transform`（中心锚定、`clamp_doc_size`
夹取、恒等短路返回 0）。返回 `-1` →「应用失败（图层未改动）」且面板留着；返回 `0`
→「尺寸与角度未变（未产生撤销步）」且面板留着；返回 `1` → 关面板 +
`refresh_history_status`「数值变换：W×H · A 度（撤销一步）」。

**6. `record_transform_step` 的四条来源 + 两条克制。** 数值变换、旋转 15°、缩放、
手柄拖动提交成功都记 `(scale_x, scale_y, angle_delta)`；移动 / 方向键微调**不记**
（与矩形不是同一个量，记了会让「再次变换」突然挪图层）；恒等步不入账（保住上一次
的真变换）。记录点一律在事务提交**成功之后**。手柄那条的基数必须在
`layer_handle_clear` **之前**快照——那个函数会把 `layer_handle_base` 清零。

**7. `Ctrl+T` 在面板开着时也有效。** 组合键走 `WM_KEYDOWN` 的 `if (ctrl)` 分支；
右栏 `doc_dialog` 的模态键处理只吞 `!ctrl` 的键，所以两个组合键不被面板吃掉。

**8. 视觉探针 `layer_transform_numeric`（11 条）。** 形状工具画一个以画布中心对称
的矩形，只比**跨度比例**（画布按窗口缩放，绝对像素不可靠）：

| # | 断言 | 量法 |
|---|---|---|
| 1 | 前置：横/竖跨度可量且居中 | 横跨度 > 60、中心 `±6` |
| 2 | 面板画在右栏 | 右栏标题区 `count_bright > 120` |
| 3 | 恒等回车尺寸不动 | 横/竖跨度皆 `<= 4` 变化 |
| 4 | 恒等不落撤销步（面板留着） | 右栏浅色像素仍 > 120 |
| 5 | 宽度 640 → 横跨度约减半 | `0 < 新宽 <= 旧宽 × 0.75` |
| 6 | 高度未填 → 竖跨度不变 | `abs(差) <= 8` |
| 7 | 中心锚定 | 中心 x `±8` |
| 8 | 应用后面板自动关闭 | 右栏标题带 `pixel_diff > 20` |
| 9 | `Ctrl+Shift+T` 再减半 | `0 < 新宽 <= 上次 × 0.75` |
| 10 | 再次变换不缩竖轴 | 竖跨度 `±8` |
| 11 | `Ctrl+Z×2` 回原尺寸 | 横/竖跨度 `±6` |

实测：横跨度 `238 → 116 → 56`，竖跨度三次都是 `118`，中心 `592.0 → 591.0`。

**9. 已知边界。** 只有平移/缩放/旋转（六自由度），没有斜切/扭曲/透视/Warp；
中心固定；改过的角度只能是整数；「再次变换」是单步记忆，撤销后仍按同一增量复读；
移动类操作不进入记忆。

### 5.64 图案叠加：效果菜单的第十项（P1-1 第二段，2026-10-24）

零 ABI 新增、零命令号冲突：kind 9 直接用 `kCmdEffectBase + 9 = 1519`，`1510..1519`
十年空档正好排满，`kCmdEffectClear`（1520）与后续 1521/1522 不动。图案语义与
参数表见 README §6.104，这里只记壳层接线与探针。

**1. 壳层只改了「多一行」。** `comp_ui::kEffectKindCount` 9 → 10，
`effect_kind_name(9) = "Pattern Overlay"`，`effect_kind_label(9) = L"图案叠加"`。
菜单在 `shell_window.cpp` 的 `AppendMenuW` 循环里按 `kEffectKindCount` 生成，
勾选/置灰按 `kCmdEffectBase + kind` 递归查找，全部自动跟随，没有新增分支。

**2. 参数面板自动多 12 行。** `draw_effect_panel` 只认
`comp_layer_effect_param_count/param_at`；壳里没有图案的键名、量程或默认 JSON。
添加时仍写 `"{}"`，面板显示的是**参数表的缺省值**（pattern=0 / scale=100 /
angle=0 / 双色黑+白 / opacity=1），回车应用写回显式值——引擎侧的对拍测试保证
「显式缺省」与 `"{}"` 逐字节同像素。面板高度与斜面和浮雕（同为 12 行）一致，
未改布局常量。

**3. 视觉探针 `effect_pattern`（5 条）。** 形状工具画满文档的 `#FF8800` 矩形，
命令 1519 添加效果（面板自动打开），Esc 关面板后取样：

| # | 断言 | 量法 |
|---|---|---|
| 1 | 前置：矩形是纯橙色 | 中心 12×12 均值 ≈ `(255,136,0)` |
| 2 | 菜单勾选后右栏出现参数面板 | 右栏整块 `pixel_diff > 40` |
| 3 | 棋盘同时有黑格与白格 | 96×96 窗内白/黑像素各 > 400 |
| 4 | 图案完全盖住底色 | 窗内橙色像素 = 0 |
| 5 | 再次勾选移除后回到纯色 | 中心均值 ≈ `(255,136,0)` |

实测：白 4101 px / 黑 4141 px / 橙 0 px，右栏差异 23387 px，移除后
`(255,136,0)`。缺省 100% 缩放 = 32px 周期，96×96 采样窗覆盖 3×3 格，所以两色
必然都出现；若哪天缺省值或 `pattern_covers` 被改坏，这三条会同时变红。

**4. 已知边界。** 只有四种内置图案，没有 `.pat` 解析、图案预设或「定义图案」；
图案对齐图层局部坐标（无「与图层链接」）；缩放是百分比不是像素周期；全局光
角度/高度仍未做。

### 5.65 可编辑渐变色标表：选项栏入口 + 右栏 8 行（P1-7 收尾，2026-10-24）

引擎的多色标入口（`comp_tool_op_desc` 尾字段，2..32 个、每段中点）在 P1-7
第一批就通了，外壳却只有三张只读预设。这一段把「选预设」变成「可编辑」：
零 ABI 新增、零命令号。值的量程与校验口径见 README §6.105，这里只记壳层接线。

**1. 选项栏从 3 组变 4 组。** `ToolOptionId::GradientEdit` 追加在枚举尾部，
`tool_option_count(ToolId::Gradient)` 3 → 4，`tool_option(Gradient, 3)` 是
`{"色标表", Stepper, 0, 0, 1}`：

- 包成 **Stepper 而不是 Choice**：它不是枚举值，`ui_core_tool_options_bar` 对
  Choice 有「`choice_count == max-min+1` 且 ≥ 2」的硬契约，套不上；Stepper 的
  两个箭头落到同一个「打开」动作，`delta` 被忽略。
- 值文本在自定义且表合法时是 `编辑 N…`，否则 `编辑…`（`option_value_text`）。
- 点击只有**右箭头**吃（Stepper 的命中规则：数值区不响应，防误触）。打开的是
  右栏面板，不是画布上的东西。

**2. 面板复用 `DocDialogKind`。** 枚举尾部追加 `GradientStops`，
`kGradientStopsRowCount = 8`，行矩形/焦点/Tab/-`=`/数字输入全部走既有的
`doc_dialog_row_rect` / `doc_dialog_focus_row` / `doc_dialog_begin_input`。
值文本与状态栏同源（`doc_dialog_value_text`），所以「面板显示 25.0%」与状态栏
「位置 = 25.0%」不会各说一套。`wsprintfW` 不支持 `%f`，百分比一律
`%d.%d%%`（整数 + 十分位）。

**3. 增删是动作，不是档位。** 第 8 行 `doc_dialog_step` 有专用分支：算出
`action = (current + direction) mod 3`，执行后立刻把 `gradient_stop_action`
归零。状态栏按**表长度前后变化**报「已插入（共 N 个）/ 已删除 / 不能插入（满 32）
/ 不能删除（至少两个）/ 首末固定」，而不是照抄行值——否则用户会反复按同一颗键
却不知道是哪种失败。

**4. Shift 反向走同一张表。** `gradient_end` 在自定义档用
`reverse_gradient_stops_copy` 做栈上拷贝（位置 `1-p`、颜色反序、段中点镜像），
预设档仍走原来的 `gradient_preset_stops(..., reverse=true)`；两条路径最后都填
同一份 `comp_gradient_stop[32]`。

**5. 视觉探针 `gradient_stops`（8 条）。** 全新进程里预设从 0 开始，连续点
3 次到「自定义」——此时用刚离开的彩虹播种成 5 个色标：

| # | 断言 | 量法 |
|---|---|---|
| 1/2 | 预设控件、色标表控件都在选项栏上 | `option_control_rect(1)` / `(3)` 非 None |
| 3 | 面板已绘制 | 右栏标题区浅色像素 > 120 |
| 4 | `→` 插入改了表 | 右栏（选中/总数/RGB 文本）差异 > 20 px |
| 5 | `←` 删除又改一次 | 与插入帧差异 > 20 px |
| 6 | 改过的颜色进了落笔 | 渐变 50% 处 12×12 均值 ≈ 洋红 |
| 7 | 表是彩虹播种的 | 25% 处 ≈ 黄、75% 处 ≈ 青 |

实测：标题区 3127 px、插入差异 16824 px、删除差异 174 px、50% `(249,8,252)`、
25% `(255,243,0)`、75% `(0,247,255)`。第 6 条是「草稿没写进落笔」的兜底，
第 7 条是「自定义退化成一两张色标」的兜底——两条一起才说明整表确实到了像素。

**6. 已知边界。** 没有画布拖拽手柄、没有渐变条点击加色标；表是工具状态，
不写 `.comp`；插值仍是 sRGB 线性（无 Lab / 噪点 / 透明度中点）。


### 5.66 图层锁 + 颜色标签：菜单单选组、面板标记与色块（P1-2 第一批，2026-10-24）

引擎侧的锁（ABI 167 → 171，四个 `comp_layer_*` 导出）与标签（同批导出）语义见
README §6.106。这一节只记壳层：怎么进菜单、单选组怎么落、面板怎么显示、
以及一处**命令分发顺序的坑**。

**1. 命令号 1660..1672。** 锁 5 个（`kCmdLayerLockNone` 1660 … `kCmdLayerLockAll`
1664，顺序是菜单显示顺序，不是位号），颜色标签 8 个（`kCmdLayerColorBase`
1665 + label 0..7）。

第一版发在 1610..1622，理由是「1600 段空」。**不是空的**：图像调整的号是
`kCmdImageAdjustBase (1590) + COMP_ADJUSTMENT_*`，而枚举值已经涨到 24，
1610..1614 正好是 Color Lookup / 阴影高光 / 自动色调 / 自动对比度 / 自动颜色。
锁的五个号是显式 `case` 标签，先被匹配，于是这五项菜单全变成了「给图层上锁」——
视觉探针 `destructive_adjust` 第 12 条（自动色调不应打开面板）当场报红：
标题带出现 786 px 差异，说明它走了别的分支。全套只有这一条测到，是因为
其余被压住的四项当时没被探针点到。现在整段搬到 1660 起。

**2. 还有一段 64 宽的宽容区间。** `run_shell_command` 的 `default:` 里：

```cpp
if (id >= kCmdImageAdjustBase /*1590*/ && id < kCmdImageAdjustBase + 64) { ...; break; }
```

它把 1590..1653 整段吞掉且**无条件 `break`**（本意是兜住没发号的 kind）。
1665 段在它之外，所以不再受影响；颜色区间判断仍排在它前面，作防御性顺序。
新增 1600 段命令号时，先看 `COMP_ADJUSTMENT_*` 枚举值涨到几。

**3. 菜单是两级。** `图层 > 锁定`（无/透明像素/图像像素/位置/全部）与
`图层 > 颜色标签`（无/红/橙/黄/绿/蓝/紫/灰）都是 `CreatePopupMenu` 造的弹层，
`menu_set_enabled_recursive` 会钻进去置灰，所以「无图层选中」时整组变灰无需额外
代码。单选用 `menu_radio_check_recursive(menu, first, last, selected)`——它要求
首尾号在**同一个子菜单**内连续，两组的号段恰好各自连续。

**4. 单选组语义 = 再点已勾项即解锁。** 锁在引擎里是位集合（`ALL` 还能和别的位
并存），菜单却是单选。壳侧 `panel_lock_menu_flag()` 把位集合归一成一档
（优先级 TRANSPARENT → PIXELS → POSITION → ALL，`ALL` 位单独看时落到「全部」），
于是「当前归一档 == 被点的那档」就写回 `LOCK_NONE`。用户不必先点「无」再点一遍。

**5. 锁不触发重绘。** `panel_set_lock` / `panel_set_color_label` 都不置
`needs_render`：锁与标签不参与合成、不推进 `content_revision`，只刷新图层面板
（`refresh_layers`）与状态栏。它们仍**可撤销**（走快照），所以状态栏用
`refresh_history_status` 而不是 `set_status`，撤销/重做计数会跟着变。

**6. 面板标记。** 行内标记串在原有 `M` / `>|` / `{}` / `H` / `V` / `fx`
之后追加 `T`（透明锁）、`P`（图像锁）、`G`（位置锁）、`L`（全部锁）——
锁是位集合、可以并存，所以逐位报而不是只报最高档；颜色标签是行首
7×7 实心色块（`FillRect` + `CreateSolidBrush`，用完即删），七色是手调的中间调，
在浅灰行底上都够辨认。

**7. 视觉探针 `layer_lock`（9 条）。** 断言与量法见 manual-test M61；
关键三条是**透明锁下画一笔画布差异为 0**、**解锁后同一笔画差异 1066 px**、
**图像锁下 `Ctrl+I` 差异为 0**——它们把「闸在引擎里而不是壳里」这件事测实了。

**8. 已知边界。** 位置锁只挡移动类入口（`set_transform` / 翻转），挡不住所有
写像素的路径；锁不是文件保护（合成/导出/读盘都不看）；标签不导出到 PSD；
没有图层链接、按颜色过滤、图层复合（仍在 BACKLOG P1-2）。

### 5.67 合并可见 + 反向排列：两个「无面板」的结构命令（P1-2 第二批，2026-10-24）

引擎侧的语义（有效可见集合、组烘焙、隐藏后代提升、剪贴到隐藏基底拒绝）见
README §6.107。这一节只记壳层：两个新菜单项、一个加速键冲突、一个父节点推断。

**1. 命令号 1673 / 1674，紧跟拼合之后。** `kCmdLayerMergeVisible = 1673`
（`图层 > 合并可见(&V)\tCtrl+Alt+E`）、`kCmdLayerReverse = 1674`
（`图层 > 反向排列(&R)`，无加速键）。两者都不开面板、没有参数，点击即落一步。

**2. 加速键不能用 `Ctrl+Shift+E`。** 它已经是 `kCmdEffectEdit`（效果参数
面板）。`Ctrl+Alt+E` 不在主加速键表里，走 `WM_SYSKEYDOWN` 里那条
`Ctrl+Alt` 分支 `case 'E'`。改这块之前先扫一遍 `kCmdEffectEdit` 与主表。

**3. 置灰复用 `enabled.layer_flatten`。** 判据是 `layer_count >= 2`——
单层文档里两个命令都无事可做。真实的「参与不足两层」由引擎返回
`changed = 0`，壳侧给不同文案（`…：实际参与合成的图层不足两层（未产生撤销步）`
/ `…：同级图层不足两层（未产生撤销步）`），与失败文案
`…失败（未改动）` 三态分明。

**4. 反向排列的父节点要推断。** 命令语义是「反转某一父节点下的兄弟顺序」，
壳侧只拿到选中行：选中层是组 → 用它自己的 id；否则用它的 `group_parent`
（`comp_layer_get_group_info` 回读）；再否则根 `0`。这条推断不做缓存，
每次命令时现读，因为多选/拖动后选中行可能已经换了。

**5. 合并可见先过 ABI 锁闸。** 参与合并的非组可见层里任一被 `ALL`/`PIXELS`
锁住即整个命令拒绝（`layer '<name>' is locked (merge visible)`）。壳侧不
自己判锁，交给 ABI——和 §5.66 的「闸在引擎里」保持一致。

**6. 成功后一律 `refresh_layers` + `refresh_history_status`。** 两个命令都真的
改像素/结构（合并会改画布），所以置 `needs_render`；反向排列虽然画布不变，
但同样走 `refresh_layers` 重排面板行。`refresh_layers` 会把 `selected_layer`
夹回合法下标，但**不同步 layer id**——与 `merge_down` / `flatten` 的行为一致，
不在这里另立规则。

**7. 视觉探针 `merge_visible`（6 条）。** 三可见层隐藏中间层 → 面板亮像素
1851 → 1104、画布差异 0；`Ctrl+Z` 后 1851 复原；反向排列后面板差异 1674 px、
画布差异仍 0；再反向一次面板差异 0。断言细节见 manual-test M62。

**8. 已知边界。** 合并可见不处理智能对象 / 未求值效果（含
`kUnevaluatedEffectMarker` 的子集直接 `COMP_ERR_UNSUPPORTED`）；反向排列只
反转同级，跨组搬动仍是拖动 / `comp_layer_move` 的活。

### 5.68 图层链接：一个命令做开关 + 多选移动去重（P1-2 第三批，2026-10-24）

引擎侧的链接语义（关系元数据、组号分配、仿射展开、剪切拒绝）见 README §6.108。
这一节只记壳层：菜单三态、面板标记、以及最容易写错的「多选移动别动两次」。

**1. 命令号 1675，一个命令做链/解链。** `kCmdLayerLinkToggle = 1675`，
`图层 > 链接图层(&K)`，无加速键。点击后做的是「切换」而不是两个命令：

- `panel_selection_ids(shell, false)` 拿到多选集合，**剔除组行**（组不能入链，
  ABI 会返回 `COMP_ERR_UNSUPPORTED`，壳侧提前过滤，用户看不到这条错误）；
- 逐个 `comp_layer_get_link` 回读：若全部非零且组号一致 → `comp_layer_unlink`；
  否则 `comp_layer_link`（引擎负责并组合并）；
- 成功文案：链接 `已链接 N 个图层（组 G）`，同组再点 `已在同一链接组：G`
  （`changed == 0`，不落撤销步）；解除 `已解除 N 个图层的链接`。都走
  `refresh_history_status`，所以状态栏能同时看到撤销栈深度变化。

**2. 菜单勾选看主选中行，不看整个多选。** `menu_set_checked_recursive(menu,
kCmdLayerLinkToggle, row->link_group != 0)`——`row` 是 `selected_layer` 指向的
**主选中行**。勾选项（不是 `menu_radio_check_recursive` 那类单选组），因为
「链接」不是互斥状态。多选集合里混有已链接/未链接时，勾选状态只反映主选中行，
这是刻意的：菜单表达的是「点下去会对这一组做什么」的近似提示，最终判定在命令里。

**3. 可用性走 `enabled.layer_present`，门槛放在命令里。** 有选中层就能点；
「不足两个非组层」不置灰，而是给状态栏文案
`链接图层需要至少选中两个图层（不含图层组）`。理由：置灰是无声的，用户不知道
缺什么；这一条要求（≥2 且不含组）无法从单行选中状态推断，必须解释。

**4. 面板标记复用 `marks` 路径。** `refresh_layers` 回读 `link_group` 存进
`LayerRow`，`shell_paint.cpp` 里 `if (row.link_group != 0) marks += L"~";`——
与锁的 `T/P/G/L`、色块同一条渲染路径，不新增列、不改行高，自检 263 项 SHA 不变。

**5. 多选移动必须按链接组去重。** 引擎的联动会在 `comp_layer_set_transform`
里把同组伙伴一起带走，所以壳层若把 a、b 都列进移动目标，b 会**动两次**
（自己一次 + 被 a 带一次），位移翻倍。`shell_tools.cpp` 的
`append_move_target` 做两级去重：

- 先按 id 去重（同层不重复列）；
- 再按 `row_link_group` 去重：同一非零链接组只保留**一个代表**，其余交给引擎。

`collect_group_members`（拖动组时的成员展开）与 `build_move_targets`（多选
位移）共用这一个函数，两条路径不会各自漂移。新增「让某些层一起动」的功能时，
必须走 `append_move_target`，否则就会重新引入双倍位移。

**6. 微调步长仍是 10 文档像素。** `Ctrl+→` 等方向键走
`layer_transform_move_step(true)`，与 §5.60 的数值一致；探针正是在 ~49% 画布
缩放下用这个步长量到 ~5 屏幕像素的位移。

**7. 视觉探针 `layer_link`（8 条，截图 143–147）。** 两层画同一笔橙色描边
（`ff8800`）→ `Ctrl+J` 复制成完全重合的两层 → 普通点底行 + `Ctrl` 点顶行
（`modified_click` 通过 `AttachThreadInput` + `SetKeyboardState` 在目标线程里
置修饰键，`PostMessage` 期间保持，否则 Ctrl 不生效）→ 1675 链接 → 只单选背景层
`Ctrl+→`。断言：旧笔迹带完全腾空、新笔迹带被涂上、红色通道 bbox 宽度不变
（没有变成两份错开的笔迹）、位移与 10 文档像素在 49% 缩放下相符；`Ctrl+Z`
后画布逐像素复原；再按 1675 解链后面板行逐像素复原。细节见 manual-test M63。

**8. 已知边界。** 链接只联几何（位移/旋转/等比缩放），不联不透明度、混合模式、
效果与蒙版；组图层不能入链；`.comp` 的组号只在本文档内有意义，粘贴/导入一律
按未链接处理。

### 5.69 图层样式拷贝/粘贴：剪贴板归外壳，整批粘贴一个撤销步（P1-2 第四批，2026-10-24）

引擎侧的报文格式、两段式缓冲与整份替换语义见 README §6.109。这一节只记壳层：
剪贴板放哪、为什么没有快捷键、多选粘贴怎么保证「一次撤销」。

**1. 两个命令，挂在「图层样式」子菜单下。** `kCmdLayerCopyStyle = 1676`、
`kCmdLayerPasteStyle = 1677`，`图层 > 图层样式(&Y) > 拷贝图层样式(&C) /
粘贴图层样式(&P)`。**不加加速键**是刻意的：PS 的 `Ctrl+Alt+C/V` 在本项目里已经是
「画布大小 / 贴入」，抢过来会破坏既有键位；菜单 + 脚本命令号足够覆盖日常路径。

**2. 剪贴板是 `ShellState` 的字段，不是引擎状态。** 引擎两个导出都是无状态的
（导出只读、导入只写目标层），样式块落在 `std::string layer_style_clipboard`。
这样做的直接好处：**可以跨文档粘贴**——在 A 里拷贝、切到 B 里粘贴，与 PS 一致；
代价是它不跟随系统剪贴板（不与其它应用互通），这是已知边界，不是遗漏。

**3. 拷贝走两段式，先问长度再分配。** `comp_layer_export_style(doc, id, nullptr,
0, &need)` 只回报长度，然后 `std::vector<char> buffer(need + 1)` 再取一次；
`need` 不含结尾 NUL，所以必须 `+1`。组图层会拿到 `COMP_ERR_UNSUPPORTED`，壳层转述
成 `该图层不能拷贝样式（图层组没有求值的样式）`——不减信息，用户能明白原因。
成功文案 `已拷贝图层样式（N 字节）`（N 用报文长度，便于对日志）。

**4. 粘贴的可用性看剪贴板。** `menu_set_enabled_recursive(menu,
kCmdLayerPasteStyle, enabled.layer_present && !shell.layer_style_clipboard.empty())`：
**空的就置灰**，而不是让用户点下去再收到「剪贴板为空」。拷贝项只要求有选中层，
门槛（组层 / 未知 kind）由引擎给原因。

**5. 单层靠隐式事务，多选必须显式包。** `comp_layer_import_style` 自带
`ImplicitTxn("Paste Layer Style")`，单层粘贴天然一步撤销；但多选循环时每个调用
各自成一步，撤销会一层一层往回退。所以 `batch.size() > 1` 时先
`comp_txn_begin(..., "Paste Layer Style", &txn)`，循环里的隐式事务会自动并入；
**若一层都没真正改动则 `comp_txn_abort`**——不能 commit 一个空事务，否则用户
撤销一次什么也没发生。

**6. 状态栏按四类计数拼装。** `changed` / `same`（已是这份样式，引擎返回
`changed=0` 且不落撤销步）/ `groups`（组层 `UNSUPPORTED`）/ `failed`。文案把
「为什么看起来没反应」写出来，例如
`粘贴图层样式：选中的图层没有改动（组图层不参与样式）`；有改动时给
`已粘贴图层样式：N 层更新`，并把跳过的组层数附在后面。随后
`refresh_effects` / `refresh_layers`，仅当 `changed > 0` 才置 `needs_render`，
最后 `refresh_history_status`。

**7. 视觉探针 `layer_style_paste`（5 条，截图 148–152）。** 橙色矩形
（`ff8800`）→ `Ctrl+J` → 上层加 Color Overlay（默认不透明黑）→ 画布纯黑；
点上层 1676 拷贝，画布**逐像素不变**；隐藏供体（命令 1011，画布回到橙色，得到
无样式基线）；点下层 1677 粘贴，画布纯黑；`Ctrl+Z` 后与「隐藏供体」那张逐像素
相同（`pixel_diff == 0`）。供体必须在拷贝后再隐藏：副本与原件像素重合，副本留在
上面会盖住靶层，「粘贴没生效」与「被盖住」就分不出来。细节见 manual-test M64。

**8. 已知边界。** 只搬效果栈（不透明度/混合模式/锁定/蒙版不搬）；剪贴板不与
系统剪贴板互通；组图层不能拷贝也不能粘贴；报文是引擎自有 JSON，不解析 PS 原生
样式文本。


### 5.70 过滤图层：三个维度、一条视图态、零 ABI（P1-2 第五批，2026-10-25）

**1. 为什么只能是视图态。** PS 的过滤是面板顶部的下拉筛选器，它决定「显示哪些行」，
不改变文档。本壳没有面板顶栏，三条筛选收进 `图层 > 过滤图层(&F)` 子菜单
（命令号 `kCmdLayerFilterKindBase = 1680` +0..6、`kCmdLayerFilterColorAll = 1690`、
`ColorNone = 1691`、`ColorBase = 1692` +0..6、`Name = 1699`、`Clear = 1700`）。
**零新增导出**：引擎不需要知道用户在找什么层。

**2. 状态只有三个字段。** `layer_filter_kind`（0 = 全部，1..6 依次是像素 / 文字 /
组 / 调整 / 填充 / 智能对象）、`layer_filter_color`（**-1 = 全部，0 = 无标签，
1..7 = 红..灰**）、`layer_filter_name`（`std::wstring`，空 = 不过滤），全在
`ShellState` 里。它们**不进文档、不进撤销栈、不影响脏标记**。

**3. 三个条件是 AND。** `row_matches_filter` 依次判类型 → 颜色 → 名称，任一不过即
淘汰。`全部类型 / 全部颜色 / 空名称` 都是恒真项，所以「只设一维」等于只按那一维筛。

**4. 命中的祖先组要保留。** 只留命中行会让组内子层飞到根级缩进，视觉层级断裂。
`filtered_layer_indices` 对每个命中行沿 `parent` 链上溯，把沿途的组一并标进 `keep`
（遇到已标记的立即停，所以同一棵子树不会被重复走）。上溯带 `guard <= count` 上限：
脏数据里构造出的环不能把 UI 线程挂死。

**5. 名称是 ASCII 大小写折叠的子串匹配。** `fold_case` 只对 `A..Z` 生效，
非 ASCII（CJK / 全角）原样比较——不引 `towlower` 是因为它与 locale 相关，
同一次输入在不同机器上可能给出不同结果。空串恒真。

**6. 菜单用单选标记，不是勾选。** 类型与颜色各是一个 radio 组
（`menu_radio_check_recursive`），当前值以 `MF_CHECKED` 标出；颜色维度是二级
子菜单（`颜色标签(&C)` → 全部颜色 / 无标签 / 红橙黄绿蓝紫灰）。名称维度开右栏
输入框（与图层重命名同一个输入管线），`清除过滤` 一次清零三维。

**7. 过滤只影响 `build_layer_display` 的输入。** `refresh_layers` 先用
`filtered_layer_indices` 得到索引集合，再交给 `build_layer_display`；面板的行序、
缩进、选中同步、锁/链接标记的绘制路径全部不变，所以过滤态下点选、拖动、快捷键
都作用于**真实**的图层，没有「只能看不能动」的半瘫痪状态。

**8. 已知边界。** 不做模糊匹配 / 正则；不记忆上次的过滤（PS 会）；不影响
「选择上一/下一图层」的遍历集合（它们仍在全量列表里走）。


### 5.71 图层复合：子菜单 + 上一个/下一个，替代 PS 的复合面板（P1-2 第六批，2026-10-25）

**1. 入口。** `图层 > 图层复合(&O)` 七项：`捕获为新复合…`(1701)、`应用当前复合`
(1702)、`用当前状态更新`(1703)、`删除当前复合`(1704)、`上一个复合`(1705)、
`下一个复合`(1706)、`重命名当前复合…`(1707)；另有 `kCmdLayerCompStatus = 1708`
只给状态栏/脚本用，不挂在菜单上。引擎侧 10 个导出与前缀见 README §6.111。

**2. 「当前复合」是视图态。** `ShellState::layer_comp_current` 存的是复合 id
（不是索引，所以删除其它复合后它依然有效）。捕获新复合后自动指向它；应用/更新/
重命名/删除都作用于这个 id；`上一个/下一个` 在候选层里取 id 排序的环。

**3. 菜单可用性看两层条件。** 捕获只要求有文档；其余六项要求
`panel_current_layer_comp(shell) != 0`，`应用` 还额外要求当前文档里真有这条复合
（`layer_comp_exists`，防止把 A 文档的 id 带进 B 文档后点下去）。不可用时置灰，
不是点了才报错。

**4. 名字输入复用图层重命名。** `panel_layer_comp_capture_begin` /
`rename_begin` 把 `layer_input_*` 切到 `LayerInput::CompName` 模式（`input_id == 0`
表示新建、非 0 表示改名），回车提交、Esc 取消；空名提示「图层复合名不能为空」。
默认名 `复合 N`（N = 已有数量 + 1）。引擎不要求名字唯一，壳也不伪造这个约束。

**5. 捕获/应用/更新都走引擎事务。** 三个写操作各是一步撤销；壳层不做「差量判断」，
幂等（捕获出与现有快照完全相同的复合、或应用一个与当前状态相同的复合）由引擎
返回 `changed=0`，此时壳层只刷新状态栏、不开空事务。`更新` 会先用
`layer_comp_matches_current` 预判一次，好把「内容未变，未记入撤销」说出来。

**6. 应用之后刷新图层面板即可。** 复合改的是每个层的 `visible` 与 `opacity`，
`refresh_layers` 一趟就把显隐图标与不透明度读数都带上；`changed > 0` 才置
`needs_render`。`changed == 0` 时既不重绘也不推历史计数——每按一次都像改了什么
是错的。

**7. 删除后指针退到第一个现存复合。** 先 `layer_comp_current = 0`，再调
`panel_current_layer_comp` 重新解析（它就是「id 还活着就用它，否则退到第 0 条」），
所以删掉当前复合后菜单不会指向孤儿 id；一条都不剩时才归零、六项转灰。

**8. 已知边界。** 快照只含「可见性 + 不透明度」（不含混合模式、效果、蒙版、
位置——PS 的复合也只记前四项里的一部分，但本仓刻意收窄）；没有缩略图列表，
所以没有「一次看全部复合」的入口；复合名不要求唯一。


### 5.72 智能对象：转换 / 栅格化 / 清滤镜三个命令，滤镜参数仍走既有效果面板（P1-2 第七批，2026-10-25）

**1. 入口。** `图层 > 智能对象(&S)`：`转换为智能对象`(1710)、`栅格化`(1711)、
`清除全部智能滤镜`(1712)、`状态（滤镜条数）`(1713)。转换/栅格化/清除都要求有
选中层，`状态` 永远可点（它只读）。

**2. 滤镜入口没有新增。** 现有的 `滤镜` 菜单与效果参数面板是唯一的「叠加」入口：
`build_filter_desc` 之后先问 `comp_layer_smart_object_is_smart`，是智能对象就走
`comp_layer_smart_filter_add`，否则仍走 `comp_filter_apply`（一步落墨）。
同一个面板、同一次回车，普通层是破坏性落墨，智能层是**往栈尾叠一条**（连做两次
= 两条，按序求值）。「改已经存在的那一条」在 §5.74 里由滤镜栈面板接上：双击某条
会把这个面板换成**编辑态**（回车写回该条，不再叠新的）。

**3. 预览靠 abort。** 智能层的预览会真的往滤镜栈尾加一条（整条栈从 source 重算，
所以看到的画面就是真应用后的结果），取帧后 `comp_txn_abort` 把像素与栈一起回滚。
这样预览路径与提交路径共用同一段求值代码，不会出现「预览和结果不一样」。

**4. 像素写入的闸在引擎。** 智能对象上的 `comp_layer_set_pixels`、画笔落墨与
`comp_filter_apply` 都返回 `COMP_ERR_STATE`；壳层不预判「这一笔会不会被拒」，
直接把结果写进状态栏。要涂像素得先 `栅格化`。菜单可用性由
`comp_layer_smart_object_is_smart` / `comp_layer_smart_filter_count` 现场查询决定：
转换只对有选中层且尚非智能对象时可用，栅格化要求是智能对象，清除要求滤镜条数 > 0
（所以撤销掉转换后，三项立刻跟着变灰）。

**5. 清除滤镜要显式事务。** 引擎的 `comp_layer_smart_filter_remove` 每次一步撤销，
壳层循环删除时必须先用 `comp_txn_begin("Clear Smart Filters")` 包住，否则按 N 次
`Ctrl+Z` 才退干净；一条都没删则 `abort`，不留空步骤。

**6. 转换与栅格化的画布不变性。** 转换不渲染也不改像素（源像素冻结进
`sourceFile`）；栅格化把当前渲染结果写回图层并丢掉可编辑性，像素**逐字节相同**。
壳层两条命令成功后都不置 `needs_render`，只在状态栏给文案——画布不变就不该重绘。

**7. 状态行只报条数。** `状态（滤镜条数）` 走
`comp_layer_smart_filter_count`，输出「N 条智能滤镜」+ 一句指路提示（0 条时提示
用「滤镜 > 应用」叠加）；**不是**逐条的 kind / enabled 清单。逐条清单在 §5.74 的
滤镜栈面板里（每一行一条，`[关]` 前缀标出已关闭的条目）。

**8. 已知边界。** 单条编辑 / 单条开关 / 单条删除现在都有 UI（§5.74），但只有
「参数面板行能表达的那些字段」可改：`vignette.color` 与 `progressive_blur` 的
显式轴在面板里没有行，编辑时按原值原样带过去（`build_filter_desc_patched`），
既不显示也不清零。不支持嵌套智能对象与「替换内容」；`.comp` 里的智能对象依赖
`sourceFile` 同目录文件，单独拷 `.comp` 会丢掉原图（载入时不报错，但降级为不可再
替换的普通数据）。


### 5.73 新建填充层：菜单建层 + 右栏参数面板 + 拖拽一步撤销（P1-2 第八批，2026-10-25）

**1. 入口。** `图层 > 新建填充图层(&F)`：`纯色…`(1714)、`渐变…`(1715)、
`图案…`(1716)、`编辑填充参数`(1717)、`栅格化填充`(1718)、`状态（种类/参数）`
(1719)。命令号之后是引擎 12 个导出（README §6.113）。

**2. 点菜单就建层。** `panel_add_fill(kind)` 先用 `comp_fill_default_settings`
拿缺省参数 → `comp_layer_add_fill` 建层 → 选中新层 → 打开面板。也就是说
「建层」与「调参数」是两步，用户点完菜单立刻能看到画布变化（缺省纯色是
不透明灰 `(128,128,128)`），不需要先确认什么。

**3. 面板是「跟随选中层」的。** `refresh_fill` 判断选中层是不是填充层：是则绑定
（`fill_layer = id`，参数从 `comp_layer_get_fill` 回读），不是则 `fill_layer = 0`
进入「新建」模式（状态栏与面板底栏的提示按这个字段在「应用 / 新建」之间切换）。
这样面板永远指着用户正在看的东西，不会出现「面板调的是 A、画布选的是 B」。
注意 `open_fill_panel` 自己**总是**开在新建态（`fill_layer = 0`），绑定是随后
`refresh_fill` 的事——两个入口（`panel_add_fill` / `panel_edit_fill`）靠这个分野。

**4. 参数行由引擎的表驱动。** 每种 kind 的行数、名字、单位、范围、步长全来自
`comp_fill_kind_param_count` / `_param_at`，壳层只负责几何与命中（行高 34、底栏
180，与效果面板同规格）。加一种 kind 不需要动壳层。

**5. 拖拽 = 一个显式事务。** `fill_panel_begin_drag` 在按下时
`comp_txn_begin(document, "Fill Parameter", &fill_drag_txn)` 并冻结参数行矩形；
拖动期间只改数值、`set_fill` 并入该事务；松手时 `dirty ? commit : abort`——**空
拖拽不留撤销步**。开不出事务就「未改动」并放弃这次拖拽，不在半改状态上留像素。
拖动中 `SetCapture`，`ReleaseCapture` 在 `drag_finish` 里做，顺序是「先清状态、
再放捕获、最后提交」。

**6. 键盘也要能用。** `focus_row` / `↑↓` 切行、`←→` 按该行步长调值、回车进输入
（右栏输入管线，Tab 切行）、Esc 收起面板并回读文档值。所有步进都过
`comp_fill_read_value` 的范围钳制，面板不会给出引擎会拒绝的值。

**7. 面板互斥。** `open_fill_panel` 会先关掉颜色范围 / 滤镜（并丢弃预览帧）/ 路径 /
历史 / 通道 / 效果 / 修边，反之亦然——右栏同一时刻只有一个参数面板，避免两个
面板抢同一块矩形。打开前还会 `fill_panel_drag_finish`（幂等），上一次没结束的拖动
不能跨面板存活。

**8. 编辑不换 kind。** `panel_edit_fill` 的 kind 取自**图层本身**
（`comp_layer_get_fill` 的回报），不是菜单上最后点的那个。否则「刚才建过渐变，
现在编辑纯色层」会把纯色层静默改写成渐变。

**9. 已知边界。** 渐变只有两站线性；图案只有四种内置程序化图案；没有「从图层
定义图案」、没有图案预设库；没有图层缩略图预览（面板只有参数行）。


### 5.74 智能滤镜栈面板：逐条列出 / 开关 / 改参数 / 删除（P1-2 第九批，2026-10-25）

**1. 入口。** `图层 > 智能对象(&S)` 新增四条：`智能滤镜面板…`(1720)、
`开关当前条`(1721)、`编辑当前参数…`(1722)、`删除当前条`(1723)。面板开关只要
当前层是智能对象就可点（面板开着时也允许关掉）；后三条还要求栈里至少一条——
条数现问 `comp_layer_smart_filter_count`，撤销掉「叠加」后立刻跟着置灰。

**2. 面板 = 选中层滤镜栈的只读投影。** 壳层**不缓存**滤镜栈：标题摘要的条数、
每一行的 kind / enabled / 参数摘要、以及三个动作的取值，全部当场调
`comp_layer_smart_filter_count` / `_at` 读取。所以撤销、重做、整栈清除之后列表
不会与文档漂移；这也意味着绘制一屏 N 行要 N+1 次查询（只读、不开事务，可接受）。

**3. 行内容与空态。** 一行 = 一条：`序号. [关] 滤镜名 · 前两个参数名值对`。
`[关] ` 前缀是唯一标示「该条已关闭」的地方（关闭的条目仍在列表里，按 PS 的语义
可以再打开）。空态按四种原因给不同提示：无文档 / 没选图层 / 不是智能对象 /
还没叠加过，避免用一句「列表为空」糊住四种不同的下一步。

**4. 键盘与鼠标。** 面板打开时 `↑ / ↓`（或 `Home/End`）移动焦点、`空格` 开关当前
条、`回车` 或**双击行**进编辑、`Del` 删除、`Esc` 关闭。鼠标单击行 = 聚焦（不写
文档），滚轮 = 翻列表（一格 3 行，不移动焦点）；行几何与命中全部来自 ui_core 的
`smart_row_rect` / `smart_row_at`（行高 22、列表从 y+64 起、底栏 96 = 5 行提示），
绘制与 `WM_LBUTTONDOWN` / `WM_LBUTTONDBLCLK` / `WM_MOUSEWHEEL` 读同一组常量。

**5. 开关 = 先读再写回。** `空格` 走 `comp_layer_smart_filter_at` 取整条 desc，
翻转 `enabled` 后 `comp_layer_smart_filter_set(..., &desc, 新开关)`——参数原样带过去，
只改开关。引擎的 `_set` 幂等（设置与开关都没变才不落步），这里设置必然相同、开关
必然不同，所以**恰好一步撤销**，状态栏照实写「一步撤销」。

**6. 编辑复用滤镜面板，不新造一套控件。** `open_smart_filter_editor` 读该条 desc →
按 `filter_spec_at(kind)` 打开既有的滤镜参数面板（同一份行几何、同一套步进/输入/
预览）→ `filter_load_values` 把值灌进面板 → 再装上 `smart_edit_layer/index/enabled/base`
绑定。绑定必须在 `open_filter_panel` **之后**装：那个函数自己会清绑定（新叠加态）。

**7. 编辑态的预览与写回。** `filter_preview_render` 在编辑态改为
`comp_layer_smart_filter_set`（仍包在事务里、取帧后 `abort`），所以预览看到的就是
「这一条改完之后」的画面，而文档停在预览前。回车 `apply_filter` 走同一条 `_set`；
参数与开关都没变时引擎不落撤销步，状态栏只说「已写回第 N 条」而**不承诺**一步撤销。
Esc / 切工具 = 丢弃缓冲回到列表，状态栏明说「本次编辑未写回」，不产生撤销步。

**8. 面板行表达不了的字段按原值带回。** `vignette.color` 与 `progressive_blur` 的
显式轴在参数面板里没有行。编辑时 `build_filter_desc_patched` 先从 `smart_edit_base`
（打开时的原 desc，同 kind 才采信）复制这些字段，再用面板值覆盖**面板控制的**字段；
用户这次拖了轴（`progressive_axis_set`）则轴取拖拽值。否则「双击改个半径」会把
暗角颜色清零、把显式轴悄悄丢掉。

**9. 互斥与生命周期。** 打开栈面板会收起颜色范围 / 填充 / 滤镜 / 效果 / 修边 / 路径 /
历史 / 通道，反之每个面板打开时都 `smart_panel_dismiss`；字体面板与文字属性面板的
`*_panel_visible` 也把 `smart_open` 纳入让位条件（右栏同一时刻只有一个面板）。
换文档（新建 / 打开 / 载入分层 PSD）调 `smart_panel_reset` 把开关、焦点、滚动与
编辑绑定一起清零。焦点与滚动在 `refresh_layers`（选中层 / 撤销 / 重做的汇聚点）
里收敛：选中层变了就换绑定回第 1 条，条数缩了就夹回去。

**10. 已知边界。** 引擎没有「调整顺序 / 移动某条」的导出，所以列表只读顺序、不能
拖动排序；也没有重命名。关闭的条目仍可编辑参数，写回后仍保持关闭（状态栏明说
「该条仍为关闭，画面不变」）。`_set` 每次真实写入是一步撤销，但 `add` 与 `remove`
的位置语义（序号）不会在壳层重新解释——删除后焦点落到下一条，删到空栈时列表回到
空态提示。（前两条已在 §5.75 补齐；第三条是 PS 语义。）


### 5.75 智能滤镜调序与改名：`move` 与 `name`（P1-2 收尾 ⑩，2026-10-25）

**1. 新增导出（尾部追加，208 → 211）。** `comp_layer_smart_filter_move(doc, id, from,
to)`、`comp_layer_smart_filter_set_name(doc, id, index, name_utf8)`、
`comp_layer_smart_filter_name(doc, id, index, buffer, capacity, out_required)`；头文件加
`COMP_SMART_FILTER_NAME_MAX_BYTES = 256`，与引擎 `kMaxSmartFilterNameBytes` 由
`static_assert` 对齐。命令号续在壳侧：`1724` 重命名当前条、`1725` 上移一位、`1726` 下移
一位。

**2. `move` 的记忆法：`to` 是移动后的最终下标。** 实现是 std::vector 那套「取出再
插入」：存 `stack[from]` → `erase(from)` → `insert(to)`。`from < to` 时 erase 已让目标
左移一格，所以 insert 到 `to` 恰好是用户期望的位置。`from == to` 返回 OK 且**不重求值、
不落撤销步**；越界 `COMP_ERR_INVALID_ARGUMENT`；锁 `COMP_LAYER_LOCK_PIXELS` 的层返回
`COMP_ERR_STATE`（理由是 "reorder smart filters"）。

**3. 调序必须整栈重求值，且先于事务。** 滤镜不可交换，换顺序就换画面：先在事务外
`evaluate_stack`，失败直接返回（像素与历史都干净），成功才开 `ImplicitTxn("Reorder
Smart Filters")`、写 `filters` 与 `pixels`、`touch_layer`、`commit`——成功即**恰好一步
撤销**。

**4. `set_name` 是纯元数据。** 只写 `SmartFilter::name`：不重求值、**不过像素锁闸**
（锁像素的层也能改名，与色标 / 链接同口径）；同值重设不落步；仍进快照，撤销退回旧名。
空串合法且表示「回到类型名」——引擎只存空，展示层回退 `filter_kind_name`，不把类型名
抄成自定义名。校验：合法 UTF-8、单行（含控制字符即拒）、≤ 256 **字节**（中文 3 字节）。

**5. 读取两段式，不截断。** `_name` 的 `buffer = NULL` 或 `capacity = 0` 只回报所需
长度（含 `'\0'`）；容量不足返回 `COMP_ERR_INVALID_ARGUMENT` 且已写好 `out_required`；
`out_required == NULL` 属参数错误。

**6. 持久化。** `.comp` 的 `engineSmart` 每条多一个 `name`；载入时不合规的名字（超长 /
非法 UTF-8 / 含换行）当没写、回退类型名——坏字段只丢一个展示名，不连累整条滤镜或整层。

**7. 壳层三条入口。** Alt+↑/↓（或菜单 1725/1726）走 `smart_panel_move_entry(±1)`：夹到
±1、到端如实报「未改动」、成功焦点跟随；菜单要求条数 > 1。F2（或菜单 1724）走
`panel_begin_smart_filter_name`：复用状态栏键入态，新增 `LayerInput::SmartFilterName`
（`FilterName` 是「按名称过滤图层」，别混），栈内下标放 `layer_input_slot`；**预填只给
自定义名**（从未改名就空着），键入按候选串真实 UTF-8 字节数夹 256，空串 = 回退类型名。
拖动与图层面板同一套：4 px 阈值才 active、落点画暖绿底、松手才 `_move`、
`WM_KILLFOCUS` / `WM_CAPTURECHANGED` 全清。

**8. 展示名一个口径。** `smart_entry_name`：非空自定义名优先（截 16 字符＋省略号，免得
256 字节撑爆状态行与行缓冲），否则类型名。行、标题状态、开关 / 删除 / 调序回执全用它；
删除与调序必须**在写之前**取名，否则读到的已是邻居。

**9. 几何微调。** `kSmartPanelFooter` 96 → 98，提示 4 行 → 5 行（Alt 调序 / F2 改名 /
拖动重排各占一行）。行几何仍由 `smart_row_rect` / `smart_row_at` 单一口径提供。

**10. 验证。** 772 例 / 0 失败；壳自检 267 项 SHA256 与基线一致；视觉 233 / 0（面板不在
场景里）；OHOS `ok=121 fail=0`；导出 211；工具表 20 PASS。

**11. 已知边界。** 拖动不自动滚动；无多选 / 批量调序、无 Alt+拖动复制；名字不参与图层
面板过滤；关闭的条目改参数后仍关闭（PS 语义）。





### 5.76 停靠子系统收口：多浮动 / 拖拽停靠 / 滚轮 / 面板分组（P0-10，2026-10-26）

dock 子系统（`shell_dock.{h,cpp}`，纯数据、不依赖 windows.h）在 §5.43 之后
攒了四项增量，本轮一次收完。**零 ABI 变更**，引擎核零改动。

**1. 多块面板同时浮动。** 放开了旧的不变式「至多一块处于 Floating」。
旧理由（「两块都浮要两套命中与焦点通路」）站不住：浮动窗口过程从
`GWLP_USERDATA` 取自己的 PanelId，命中层（`shell_panel_hit`）是**无状态函数**，
键盘统一转发主窗口——第二块浮动不需要第二套通路。
句柄从「一个 HWND + 一个 PanelId」改成**按 PanelId 索引**存
（`ShellState::float_windows[2]`），每块面板一个独立浮窗。
`DockState::load` 里那条「多于一块浮动就裁剪」的逻辑一并去掉。

**2. 拖拽停靠。** 按住面板**标题行**往外拖，越过 6 px 阈值就浮成独立窗口
并跟随指针（按增量搬，不按绝对位置——指针越出客户区时绝对定位会抖）。
松手时看**浮窗中心**落在主窗口哪条热区：左边缘热区停到左侧、右边缘停到
右侧、画布中间或窗口外则**保持浮动**（PS 的手感：拖出来就是要它浮着）。
落点判定是纯函数 `comp_dock::drop_zone_at`（热区宽 48 px），可脱离 GUI 单测。

**3. 浮动面板滚轮。** `WM_MOUSEWHEEL` 转发给主窗口的滚动通路。
**不能直接转发消息**：那个消息的 lParam 是屏幕坐标，转发过去主窗口会用
**自己的**客户区解释它——主窗口会以为是主窗口内的点。所以这里自己算好
「相对主窗口客户区的坐标」再合成一条新消息。

**4. 多面板合并成一组标签**（photocraft 的 craft-ui：`group` + `tab strip`）。
`视图 > 面板分组 > 合并成一组 / 拆开分组`。两块面板住进同一列，列顶出现
标签栏，点标签切换显示哪一块。

- **单一真源**：分组关系由 `panels[].area` 推导（两块同列即同组），
  **不另存成员列表**——那份列表表达的是与 area 同一件事，两份真源必然分叉
  （改了 area 忘了改列表，就会出现「标签栏说有 3 个标签，实际只有 2 块面板」）。
- **标签栏不占列宽**：把面板内容矩形整体下移一行。列宽是用户拖出来的偏好，
  因多了一条标签栏就变窄 = 分组瞬间画布宽度跳一下。
  面板内容矩形下移后，组内面板的行几何自动跟着走——行位置都由面板矩形推导。
- **列宽存在列上**，切标签时不变（PS 的组就是这个语义）。若宽度跟着激活成员
  走，切一下标签画布宽度就跳一次，正在看的图会被挤一下。
- **拆开后恢复合并前的归属**（从 `last_docked_` 取），而不是变成「图层右 /
  调整左」——后者会让用户以为面板跑错了地方。

**5. 顺带修掉一个被新语义暴露的既存隐患。** `PanelState::area` 的成员缺省值
是 `Right`，两块都用它就变成「都在右列」。旧语义下这只是「都停右」的错位
状态，被 `set_area` 的换位逻辑掩盖着；分组语义下它**直接变成「两块同列 =
已分组」**，分组状态凭空出现。症状是视觉探针 42 项全线错位（每项面板几何
都偏移），而壳自检与引擎测试全绿——**基线绿不等于界面对**。
现在 `DockState` 的成员初始值与 `defaults()` 都写全，两处一致。

**6. 验证。** 引擎 794 例 / 0 失败；壳自检 277 项 SHA256
`4ffee705da27c7d3…`（前 272 行逐字节未动，末尾追加 5 项：多浮动、拖拽落点
热区、浮窗几何往返、面板分组、标签栏几何）；视觉探针 254 / 0（41 场景，
新增 `panel_group` 8 项）；回归 7 PASS / 1 SKIP / 0 FAIL。
新增的 `panel_group` 场景量的是**激活标签的顶部 2 px 强调线**（颜色唯一，
不会被面板底色混淆）与**图层行高亮**——不用「面板底色」判是因为图层栏
`(24,26,30)` 与调整栏 `(22,24,28)` 只差 2，容差放宽就误判、收紧就漏判。

**7. 组模式的三处列级归属（2026-10-26 交付后自查发现并修掉）。**
组模式的列是**一个整体**，但既有的字段名都按**面板身份**分
（`layer_panel_width` / `adjust_panel_width` 各自夹取，`layer_panel` /
`adjust_panel` 各自是「这块面板画在哪」）。三处因此对不上：

- **列宽按面板身份夹取** → 组宽 200 时，切到图层标签是 200、切到调整标签
  被夹到 240（调整栏区间是 240..560，图层栏是 168..520）。用户看到的是
  「切一下标签，画布宽度跳一下」。修法：组有自己的宽度区间
  （168..560 = 两者并集）与自己的宽度字段 `group_width`。
- **列位置按激活成员算** → 写成 `on_left ? active_is_adjust : !active_is_adjust`
  （那是「激活成员属于左列吗」，把「组在哪」与「组里显示谁」混成一件事）。
  后果同上：组在右列、切到图层标签时列宽跳成左侧图层栏的缺省宽 248。
  修法：位置只由 `group_on_left` 决定，激活成员只决定「画谁的内容」。
- **收起是面板级、按钮按面板身份取矩形** → 切标签时列忽收忽展；且激活成员
  不在那一列时 `out.adjust_panel` 是空矩形，按它算出来的收起按钮会跑到
  **负坐标**。修法：收起是列级（`group_collapsed`），按钮用组列的矩形算。

这三条都不是自检抓到的——**自检当时测的是 prefs 里的宽度字段，而那个字段
由 `group_width` 决定、本来就不受 `group_active` 影响，断言恒真**。
换句话说，那条断言测错了对象，测的是一个恒量。
重写成「量布局输出的实际列宽」之后立刻抓到真 bug。
**教训：断言要量「用户看得见的东西」，不是量「实现里的某个字段」。**

**9. 视觉探针的布局改为问壳要（`kMsgLayoutQuery` = WM_APP+1）。**

探针此前靠 **Python 复算** ui_core 的布局公式，那是**第二份真源**。壳里改了
布局公式（新增面板分组就是一种），复算就跟不上——探针量到的位置与壳画出来的
位置对不上，断言要么误报失败、要么漏掉真缺陷。

本轮分组就栽在这上面：**「切标签时列宽跳变」这个真缺陷**，既没被自检抓到
（当时测的是 prefs 里的一个恒量），也没被探针抓到（量的标签位置与强调线都
不随列宽变）。

**通道怎么做的（三次走错，过程值得记）**：

1. **自定义 WM_APP + 指针**——跨进程不能传裸指针。壳进程解引用探针进程的
   地址必然读到垃圾，查询**静默失败**（探针退回复算却看不出自己退回了）。
2. **WM_COPYDATA**——它是**单向**的：系统把数据复制给接收方，接收方对
   COPYDATASTRUCT（含 lpData 指向的缓冲）的修改**不会回传给发送方**。实测
   壳写了 JSON、也回填了 dwData 与 cbData，发送方读到的仍是原值。
   「问一句、答一句」用它根本不成立。
3. **共享内存 + WM_APP**（最终方案）——探针 `CreateFileMappingW` 建一个命名
   映射，问壳一句，壳 `OpenFileMappingW` 打开**同名**映射写入 JSON，消息
   返回值只带「写了几字节」。共享内存是唯一能做到双向的路子。

顺带加了一条**可见信号**：探针在布局查询失败时打印 `[WARN] 布局来源 =
fallback`。静默退化是最难查的故障模式——探针用第二份真源量错地方，还报
「通过」。

**10. 探针抓到第四个真 bug：合并时组宽没初始化。**

合并时壳侧写的是 `set_group_width(target, group_width(target))`——「把组宽设成
它自己的当前值」= 没设。组宽为 0 → 回落成「激活成员的缺省宽」→ 激活的是
调整时 330、切到图层变 248，**切标签时列宽跳一次**。

修法：组宽初值 = 目标列**合并前**的实际宽度（那一列当时停着谁就用谁的缺省
宽），且必须在移动**之前**取——移动之后两块同列，「激活成员的宽度」就变成
按激活标签变化的东西了。逻辑收进 `DockState::group_merge`，自检直接调它
（不在自检里复制一遍壳侧逻辑）。

这条 bug 是**探针的新断言抓到的**（自检当时测的是恒量）。修完之后探针连跑
两轮都是 255/0。

**11. 验证。** 引擎 794 例 / 0 失败；壳自检 281 项 SHA
（末尾追加 9 项：多浮动、拖放落点热区、浮窗几何往返、面板分组、左列分支、
组列宽与激活成员无关、组收起是列级、组宽合并时初始化、标签栏几何）；
视觉探针 255 项 / 41 场景；回归 7 PASS / 1 SKIP / 0 FAIL。

**12. 已知边界。** 组内最多两块面板（本仓只有两块）；合并走菜单而非拖放
（真正的拖放重排需要拖放命中层与边缘热区）；浮窗内容里没有滚动条，
面板列表的滚动靠滚轮事件驱动。

### 5.77 拖放重排 + 图层列表滚动 + 可见滚动条 + 鸿蒙图层面板（P0-10 收尾 / P1-2，2026-10-26）

把上一轮诚实清单里剩下的三项一次收掉，外加鸿蒙侧补齐图层面板。

**1. 拖放重排（把面板拖到另一列去合并）。**

原先拖到边缘只有一种结果：**两块对调**。现在按 PS 的手感分成四种落点语义：

| 落点 | 结果 |
|---|---|
| 拖回**自己原来那侧** | 停回原位（不是合并） |
| 拖到**另一侧、那列空着** | 直接停过去 |
| 拖到**另一侧、那列有面板** | **合并成一组**（拖放重排） |
| 落在画布中间 / 窗口外 | 保持浮动 |

为什么「拖到有面板的那侧」是合并而不是对调：菜单里的「面板停靠到右/左侧」
是**独占**语义（那块过去、另一块让位），而把面板**拖到另一块面板所在的位置**
在 PS 里是「合成标签组」。两个入口意图不同，结果就该不同——混成一样的话
「停靠左侧」会把用户拖成组。

判定收在 dock 层的**纯函数** `drop_action_for`（壳与自检共用同一条），
不在自检里复制一遍壳侧逻辑——复制就会分叉，分叉就等于没测。

组宽初值取**目标列合并前的实际宽度**（用户拖出来的宽度优先，没有就用缺省）。
用缺省宽的话，用户把右栏拖宽到 420 再合并，组宽会变回 330，合并瞬间画布
宽度跳一下，正在看的图被挤动。

**2. 图层列表滚动。**

此前图层面板**根本没有滚动**：所有行都画出来、超出面板的部分直接裁掉——
层一多就看不到底下的层。那不是「不方便」，是**看不到**。调整栏里的历史 /
通道 / 滑杆区都有滚动，只有图层列表漏了。

- 滚动量存在 `shell.doc.layer_scroll`，行矩形与命中都跟着它走
  （`layer_row_rect_scrolled` / `layer_row_at_scrolled`）。
- 拖行落点、插入线、中间带判定一并跟滚动量对齐（否则行会画在面板外、
  「看得到但点不准」）。
- 「选中行永远可见」放在**渲染入口**统一做（`panel_sync_layer_scroll`）：
  选中行会因点选、键盘上下、多选提升、撤销重做等很多条路径变化，每条都补
  一次 ensure 迟早漏一处；放在一处就是**结构上**保证。

**3. 可见滚动条。**

滚动此前只能靠滚轮——用户**看不到**「下面还有东西」，也就不知道能滚。

- **图层列表**：右侧画一条可见滚动条（轨道 + 滑块），可拖、可点轨道翻页。
- **调整栏的三个可滚子面板**（历史 / 通道 / 字体）：同一套设施接上。绘制与命中
  共用 `adjust_panel_scroll_bar` 这一条几何——两处各算一次必然有一天不一致
  （滑块画在一处、命中算在另一处 = 点不动的滚动条）。
- 拖动期间要知道自己动的是哪一条，记在 `ScrollDragTarget` 枚举里，不靠「当前
  显示的是哪个面板」反推（面板可能在拖动中途切换）。
- **取消路径**：拖动是「按住鼠标」的临时态，只由松手结束。所以**失焦**
  （WM_KILLFOCUS）、**捕获被抢走**（WM_CAPTURECHANGED，菜单弹出/切窗口）、
  **ESC** 三处都必须清标志——收不到松手事件时，标志不清的话**之后所有鼠标移动
  都被当成拖滚动条**，界面像卡住了一样。

**4. 鸿蒙壳补图层面板。**

鸿蒙侧此前只有按钮级演示，**没有图层列表**（与 Windows 壳的差距）。现在
补上同语义的一层：图层名、显隐开关、透明度滑杆、删除、改名。

- 五个 NAPI 入口：`layerListJson` / `setLayerVisible` / `setLayerOpacity` /
  `removeLayer` / `setLayerName`。数据来自引擎（`comp_layer_list` +
  `comp_layer_get_name`），**不是壳自己记账**——壳记账的话，导入图片加的层
  就不见了。
- 图层名走 JSON 转义：名字是用户可改的，里面可能有引号/反斜杠，不转义
  会让整份 JSON 解析失败、整个图层面板出不来。
- 跨 NAPI 边界传数组在 ArkTS 侧类型不稳（实测 ArrayBuffer / TypedArray 都不被
  识别），所以列表走**纯文本 JSON**，ArkTS 侧一次 `JSON.parse`。
- **布局重排**：手机（尤其竖屏）窗口高度有限，内容总高很容易超出——超出时
  Column 末尾的元素会被**挤没**（实测图层面板只剩 40px 高度，列表整块看不见）。
  现在除 XComponent 外的内容全放进 `Scroll`，内容再长也能滚到。
- **自检按钮放在 Scroll 之外**：ArkUI 的 Scroll 会把落在它身上的触摸先当成
  滚动处理，按钮的 `onClick` 常常不触发（实测放在 Scroll 内连点多次都点不动，
  移到 Scroll 外一按就响）。控件放在会被滚动容器吞触摸的位置，等于放了个
  点不着的按钮。
- `layerPanelSelfTest()`：把「列表 / 显隐 / 透明度 / 改名 / 删除」在引擎层
  跑一遍并**读回**校验。模拟器上 XComponent 上不了屏（OPT-21b），画布上看不到
  任何变化，「显隐有没有真的作用到引擎」光看屏幕验不了，只能读回引擎状态。
  实测结果：`list=1 hide=0 opacity=0.50 name=renamed_probe del=1 layers=1->1`。

**5. 真机（Mate 80 Pro Max）上的一个真 bug：libpng 的 ARM NEON 优化。**

本仓的 libpng 是**裁剪过的**（`arm/` 目录不在仓里），而 libpng 的 `pngpriv.h`
在检测到 `__ARM_NEON__` 时会自动开 NEON 优化——aarch64 上该宏**总是**定义，
于是引用了被裁掉的 `arm/*.c` 里的函数，**链接期**报 undefined symbol。
x86_64 模拟器没有 `__ARM_NEON__`，自动为 0，所以此前一直「测过是好的」。
`pngpriv.h` 自己的注释给了正解：`-DPNG_ARM_NEON_OPT=0`（`build_ohos.sh` 与
`build.py` 两处同口径）。

代价是 ARM 上 PNG 解码走纯 C 路径（慢一些，结果一致）。

**6. 真机上裸二进制跑不起来（平台限制，非缺陷）。**

HarmonyOS NEXT 真机的 `sh` 域**不允许执行 `/data/local/tmp` 下的 ELF**
（SELinux `execute_no_trans`；文件权限已是 `rwxr-xr-x`，仍报
Permission denied）。模拟器（OpenHarmony）没这个限制，所以此前没暴露。
后果：真机上跑不了 `compositor_tests`。绕法有两条，都不理想——
裸二进制这条路走不通；把测试二进制塞进 HAP 的 rawfile 又需要先装上
（而装机要签名）。

**7. 签名：真机装机需要 DevEco 自动签名重新签发。**

本机 `~/.ohos/config/` 里有 5 组 DevEco 自动签名的材料，但**没有一组能用**：

- `bundle-name` 与本工程不符（如 `com.ekkostudio.harmony` vs 本工程的
  `com.ekkostudio.compositor`）；
- `device-ids` 里只有两台老设备，**不含**新接入的 Mate 80 Pro Max。

`p7b`（profile）由 AGC 用 Huawei CBG 的密钥签发，没有私钥签不出新的——
所以这条必须由 DevEco Studio 的「自动签名」重新签发（登录华为账号 +
设备在账号下 + 点一次 Apply）。

**8. 写这个场景时当场抓到的第三个真 bug：滚轮滚不动。**

「选中行永远可见」放在**每次渲染**里做（`panel_sync_layer_scroll`），看着是
最稳的做法——不管选中怎么变，它都在可视区。实际效果是：**用户滚轮往下看
底下的层，滚动位置被立刻拽回选中行，滚轮等于滚不动。**

修法：记录「上次做 ensure 时的选中行」（`layer_scroll_anchor`），**只在选中行
真的变了**时才 ensure，其余时候只夹取滚动量。滚轮滚动时选中没变，所以滚动
位置守得住。

这条是**视觉探针的新场景 `layer_scroll` 抓到的**（滑块 y 质心 222 → 252：
修了才动）。断言不软：滚动后「选中高亮消失」——若 ensure 每帧都在做，
选中行会被顶回第一行，高亮仍在画面里，列表等于弹回原处。

**9. 顺带修：图层面板底部的提示行被算成了可视区。**

图层面板底部有两行提示字（`Del 删除 · Ctrl 点选多选 …`，占 52 px）。滚动计算
原先没算它 → 可视行数多算两行 → **滚到底时最后一行被提示字盖住**，
用户看到的是「明明还有层，点不到」。现在 `kLayerPanelFooter` 算进可视高度，
滚动条轨道也不再压到提示上。

**10. 顺带修掉探针自己的三个老毛病：连跑不稳定（这一条值不少）。**

视觉探针从 2026-10-23 起就有「连跑偶发失败」的毛病：靠物理鼠标拖动造图的
场景（铺纯色场、拖滑杆、画形状）会随机失败，**失败项每轮不同、单独跑该场景
必过**。一直用 `git stash` 对照确认「基线同样中招、与功能改动无关」，就当成了
环境偶发记着。

**真根因是探针自己造的**：探针用**合成**鼠标消息驱动，但**真实指针**也在场
——窗口激活/悬停时系统会补一条真实 `WM_MOUSEMOVE`，那条消息的坐标是**物理
指针的位置**，会把合成拖动的坐标顶掉：

> 合成拖动画到 A 处 → 真实 mousemove 把光标挪到 B 处 → 笔迹落在 B →
> 断言量的是 A → 「画布上没出现预期像素」。

原先把物理指针停在**状态栏**（仍在**客户区内**），那条补发的消息照样顶掉
合成坐标。

**修法三条**：

1. 物理指针停到**标题栏**（**非客户区**）——系统只给非客户区发
   `WM_NCMOUSEMOVE`，客户区收不到，合成坐标不会被顶掉；
2. 鼠标消息改 **SendMessageW 同步**发送——返回时壳已处理完这一串输入，
   不是「消息还在队列里」；
3. 截图**等画面稳定**（连续两帧逐字节相同才用）——`PrintWindow` 画的是窗口
   当前状态，壳还没重绘就拍，拍到的是旧帧。

修完**连续两轮全场景 260 项 0 失败**（此前每轮 5~14 项失败）。

**第三条：组合键偶尔退化成单键。**

`press()` 的 KEYDOWN 与 KEYUP 之间只等 0.05s。壳还没处理完 KEYDOWN 就发
KEYUP，壳看到的组合键**没有修饰键**——`Ctrl+Z` 变成普通 `Z`，撤销没发生，
于是「一步撤销把两层一起复位」这类断言连跑时随机失败（单独跑必过）。

0.05s 在机器空闲时够、连跑时不够（上一个场景的进程刚退、新的刚起，正忙）。
改成 0.12s。

> 这三条（物理指针抢通路 / `press()` 抢修饰键 / 截图抢在重绘前）都在**同一层**：
> **探针在跟壳抢同一条输入与渲染通路**。前两条是「谁先到」，第三条是「什么时候
> 拍」。修一条不够，得三条一起看——这才是「连跑偶发」的完整解法。

> 这件事本身是个教训：**「基线也这样」不能当成「不是我的问题」**——它只能说明
> 「不是这一轮的改动造成的」，不能说明「这个毛病是对的」。稳定性的问题是
> **工具**的缺陷，不是环境的偶然。

**11. 两个 GDI / 输入层的坑（都是这一轮踩的）。**

**① `SelectClipRgn` 的返回值不能用来恢复裁剪区。**

为了让图层行的文字不被新画的滚动条盖住，行内容要裁到滑块左侧，画完再恢复。
第一版用 `SelectClipRgn` 的**返回值**恢复——那是个 GDI 句柄，但**它返回的是
哪个区域**（旧的还是新的）在不同实现上不一致：按「旧区域」理解时，恢复成的就是
`row_clip` 自己，滑块正好落在裁剪区外被裁掉。

现象极具迷惑性：**滚动条凭空消失**（不是画歪、不是颜色不对，就是没有），
而自检与其它场景全绿——只有视觉探针量得到「滑块在不在」。

现在改成**显式恢复成面板矩形**（自己再建一个 region），不依赖返回值。顺带保住
了「面板内容不溢出到画布」这个原有约束。

**② `press()` 不能改成同步发送（试过，踩坑）。**

曾怀疑「按键会丢」是因为 `press()` 用 `PostMessageW`（异步）投 `WM_KEYDOWN/UP`，
而修饰键状态是 `AttachThreadInput + SetKeyboardState` 写进目标线程的——壳还没
处理 WM_KEYDOWN 探针就走了，修饰键状态一恢复，按键就丢。改成
`SendMessageW` 同步发送后，**文字输入场景全线崩**（IME 的组合态需要按键的
原始投递顺序，同步投递把那个顺序打乱了）。

所以 `press()` **保持异步**。「按键会丢」的真正解法是另一条：物理指针停到标题栏
（第一节那条）——根因是**真实指针与合成消息抢同一条通路**，不是投递方式。

> 这条值得记：**「看起来像同一个原因」的两个现象，未必是同一个原因**。按键丢和
> 文字崩都发生在输入层，但一个靠改 park 解决，另一个改了反而更糟。

**12. 验证。** 引擎 794 例 / 0 失败；壳自检 285 项（末尾追加 4 项：拖放落点
语义 / 拖放合并到指定列 / 图层列表滚动 / 可见滚动条）；视觉探针 260 项 /
43 场景（新增 `layer_scroll` 3 项：滚动条可见 / 滚轮真滚动 / 选中行不被拽回；
`adjust_scroll` 2 项：调整栏字体面板的滚动条可见 / 滚轮真滚动），
**连续两轮连跑 0 失败**；回归 7 PASS / 1 SKIP / 0 FAIL。鸿蒙侧：模拟器 HAP 装机 + 图层面板
链路自检全绿 + 图片导入像素核对；真机 aarch64 交叉编译与链接通过。

### 5.78 鸿蒙图层分组 + 拖行 + 调整联动（P1-2 收尾，2026-10-27）

上轮诚实清单里剩下的两项（鸿蒙图层分组/拖行、调整面板联动）这一轮做掉了，
外加把拖放重排的视觉探针场景补上。**零 ABI 变更**——引擎侧的
`comp_layer_move` / `comp_layer_add`（`is_group` + `parent`）/
`comp_layer_get_group_info` / `comp_layer_set_active` 早就齐了，缺的只是壳。

**1. 鸿蒙图层面板：树形 + 分组 + 拖行。**

- **树形列表**：NAPI 侧算好「树形前序」（组后面**紧跟**它的子层）与层级，壳按层级
  缩进。不能直接把引擎的堆叠序发给壳——那边父组与子层混在一起、不保证相邻，
  照着画就是「子层散落在列表各处」，用户看不出谁在谁里面、也拖不对。
- **建组（原位）**：组落在原层的**位置**上，原层成为组的第一个子层。顺序必须
  先「把组移到原层之前」再「把原层移进组」，反过来的话组会跑到原层下面。
- **拖行**：拖到**组**上 = 收进组；拖到**普通行**上 = 插到它之前（同父级）——
  拖到根级行之前就自然出了组，所以「拖出组」不需要单独的手势。
- 「建组 / 移出」两个按钮是拖行的兜底入口（手机上长按拖动不一定顺手）。

**2. 这一段最贵的是三个 ArkTS 坑，都记下来。**

**① `JSON.parse(raw) as T[]` 会把 interface 靠后的字段抹成 0。**

实测：同一个字符串直接显示出来是 `{"id":3,"parent":4,"nest":1}`，`as LayerRowItem[]`
之后读到的却是 `parent=0, nest=0`；而靠前的 `id / name / visible / opacity /
isGroup` 都正常。换字段名（`depth` → `nest` → `pp`）都没用——**是位置，不是名字**。

最终改成**分隔符字符串**：NAPI 一次给完，每行
`id\x1fname\x1fvisible\x1fopacity\x1fisGroup\x1fparent\x1flevel`，行间 `\x1e`，
壳侧一次 `split` 到底。字符串本身从来没出过错（同一份 raw 直接显示一直是对的），
所以就把字段在字符串里分好。

中途还试过「build 一次 + 逐字段取 7 个标量」，也不行：多次 NAPI 调用之间那份缓存
会与渲染节奏错位（不同行读到不同批次的值，出现「组行有父级、子层没有」）。

**② `padding({left})` 在这个 Row 上不生效。**

缩进恒为 0。换成一个固定宽度的空元素占位（层级 × 14vp）就正常了。

**③ ForEach 的 key 只用 id → 复用旧组件与旧 item。**

把层拖进组之后 **id 没变**，ForEach 认为还是同一个节点，**复用上一次的组件与
闭包里的旧 item**——缩进、组标记全是上一次的样子。数据明明是对的（`layerTreeText`
里已经是 `p4,l1`），界面上还是 `p0,l0`。

key 必须带上**会变化的展示字段**（层级 / 父级 / 名字）。

> 这三个坑叠在一起，一度让排查绕了很久：先怀疑引擎没记住父子关系（不是），
> 再怀疑 NAPI 读错（不是），最后才发现**数据一直是对的、错在渲染**。

**3. 调整面板联动：作用于「选中层」，不是顶层。**

- 壳自己记 `g_active_layer`：引擎的 `comp_layer_set_active` **只有写没有读**
  （ABI 没有 getter，`activeLayerID` 只在 .comp 序列化时用）。壳侧所有「作用于
  选中层」的操作都要一个 id，所以壳保管一份，同时写回引擎让 .comp 里的
  `activeLayerID` 正确。
- 图层面板点选即写回；新建层自动选中（与 PS 一致）。
- `applyVignette` 改成打**选中层**。此前固定打顶层：用户选中 B 层点暗角，被改的
  是最上面那层 A——「选中的层」与「被改的层」不是同一个，这是最糟的一类交互错误
  （用户会以为操作丢了，其实是打到别处，撤销一次才发现改错了层）。

**4. 顺带修掉一个一直藏着的真 bug：暗角按钮是空操作。**

引擎的 vignette `amount` 是 **0..100 的百分比**（域层 `in_range(amount, 0, 100)`，
再映射成内核的负值压暗），而鸿蒙壳一直把 ArkTS 的 **0..1** 直接传进去——
`applyVignette(0.7)` 实际是 **0.7%**，等于什么都没做。`midpoint` / `feather` 同样
是 0..100 而不是 0..1。

因为这个 bug 的表现是「什么都没发生」，而模拟器上画布上不了屏（OPT-21b），
屏幕上连「没生效」都看不出来——一直没被发现。现在换算成百分比。

**自检 `adjustTargetSelfTest`**（硬证据）：两层纯色（红 / 蓝），选中**红**层
（蓝层后建、在更上面）打暗角，读回像素比：

```
target=red ok=1 red=255->25 blue=40->40 rc=0 changed=1
```

红层角落 255 → 25（明显压暗），蓝层 40 → 40（**一个字节没动**）。量角落不是量
中心：暗角的语义就是「中心不动、边缘压暗」，量中心既看不出打没打上，也分不清
「没生效」还是「打对了」。

**5. 拖放重排的视觉探针场景（`panel_drag_dock`，8 项）。**

自检已经覆盖落点**判定**（纯函数），但没覆盖「拖」这件事本身：标题栏按住 →
拖出成浮窗 → 移到目标列 → 松手 → 合并。这条链上任一环断了，用户看到的就是
「拖了没反应」，而纯函数自检照样全绿。

量的判据与 `panel_group` 同源（真实布局 + 激活标签的顶部强调线）：起始两栏都在 /
拖过阈值后出现浮窗 / 浮窗中心被搬进右边缘热区 / 松手后合并 / 左栏让位 / 标签栏
有激活态 / 拆开后复原 / 复原后不再是应用底色。

**这一条又抓到一个探针的死穴：增量式拖动对「真实指针顶掉合成坐标」零容忍。**

壳侧日志（当时临时加的）看得很清楚：

```
move mouse=(294,94) delta=(182,133) -> SetWindowPos 移到 (1702,300)
move mouse=(112,-39) delta=(-182,-133) -> SetWindowPos 移回 (1520,167)
```

每发一条**合成** mousemove，系统紧接着补一条**真实** mousemove（坐标是物理指针
位置），把浮窗又搬回去——净位移为零。一次性 `drag()` 能靠后续密集消息把位置
拉回来，**增量式的面板拖动不能**：位移丢一次就永久丢。

修法：分步拖动期间用 `SetCursorPos` 让**物理指针跟随**合成坐标，松手后再把指针
收回标题栏（留在画布上的话，笔尖轮廓会进后续截图的量区）。

顺带一个 API 坑：`ScreenToClient` 没声明 `argtypes`，64 位下 ctypes 按 32 位 int
传指针，函数**静默失败**（返回 0、坐标原样不动）——量出来的浮窗位置是屏幕坐标，
判热区全错。

**6. 验证。** 引擎 794 例 / 0 失败；壳自检 285 项（未受影响）；视觉探针
268 项 / 44 场景（新增 `panel_drag_dock` 8 项）；鸿蒙侧模拟器实测：
分层建组后列表缩进正确、拖入组/拖出组都生效、`layerPanelSelfTest` 全绿
（`group=1 ungroup=1 active=1`）、`adjustTargetSelfTest` 全绿。

### 5.79 鸿蒙图层折叠 + 多选 + 可编辑调整层（2026-10-27）

上一轮结尾列的「下一批」三项，这一轮做完。仍然**零 ABI 变更**——
`comp_layer_add_adjustment` / `adjustment_at` / `update_adjustment_at` /
`remove_adjustment` 与参数元数据（`param_at`）、`build_settings`、`read_value`
引擎侧早就齐了。

**1. 图层折叠（组可收起）。**

折叠是**纯 UI 状态**：层级的真相在引擎里，壳只记「哪些组是收起的」，按父级链
过滤显示行——不写回引擎、不进撤销栈（收起一个组不该能被撤销）。

- 行首的 `▾` / `▸` 就是折叠开关，点图标才算，点空白不折叠（避免误触）。
- 组被删掉后，它的 id 还留在折叠集合里会「哪天 id 被复用就凭空收起」，所以每次
  刷新列表时清一遍已消失的 id。

**2. 图层多选 + 批量操作。**

手机上没有 Shift/Ctrl，长按又被拖行占了（拖拽起手就是长按），所以行首单独放一个
勾选圆点：**点圆点 = 加入/移出多选，点名字 = 单选**。同一个手势不能既做拖行又做
多选——用户拖到一半发现自己「选中」了一堆层，是最糟的体验。

- 三个按钮（建组 / 移出 / 删除）**都走批量语义**：有多选就作用于多选，否则作用于
  主选中。这样「选几个层一起建组」不需要额外的模式开关。
- 批量建组：以第一个层**原位**建组，其余逐个移进去。建组读的是活动层，所以先把
  活动层设成第一个——否则建出来是个空组。
- 批量删除**从后往前**删：删一个组会连子树一起删，先删后面的层可以避免「按 id 删
  到一半发现前面的树已经没了」。

**3. 可编辑调整层。**

- 类型清单**来自引擎**（`comp_layer_adjustment_kind_name` 能报出名字的就是合法
  kind），参数控件由 `comp_adjustment_param`（路径 / 标签 / 类型 / 范围 / 缺省）
  生成。壳不维护枚举表也不维护参数表——上游加一种调整，壳不用改。
- settings JSON **由引擎组装**（`build_settings`），回填也走引擎（`read_value`
  按 `json_path` 取值）。壳碰 JSON 就会把键名、转义、范围校验再实现一遍，迟早分叉。
- 交互：选类型 → 调参数 → 新增；点「已有」里的条目进入编辑（按钮变「更新」、
  滑块回填当前值）；点 ✕ 删掉。

**4. 这一轮踩的坑（都记下来）。**

**① `out_size` 不含 NUL，缓冲区要多给一位。**

引擎的两段式写入（`build_settings` / `adjustment_at`）回报的 `out_size` 是**不含
NUL 的字节数**，而写回时按 `capacity - 1` 截断。按 `out_size` 分配缓冲区，JSON 会
少最后一个字符（`}`）——**后续 add 直接报 INVALID_ARGUMENT**，症状是「点新增永远
是 rc1」，而参数、类型、值全都是对的。三处（add / update / 读回）一起修。

**② 调整区放进 Scroll 被挤成 0 高。**

上方有画布 + 图层面板 + 四行调试按钮，Scroll 只剩一百多 px——「调整」两个字都看
不见，等于这个功能没做。改成：调整区**固定**在图层面板下方，参数区内部再套一个
固定高度的 Scroll，调试按钮整片挪进外层 Scroll（它们不常用，可滚即可）。

**③ 「已有」行放在参数区下面 = 看得见点不着。**

参数区的 Scroll 把「已有」行挡在滚动内容里，dump 能取到它的坐标（那是布局坐标，
不是可视坐标），点击落到下面的按钮区上。改成把「已有」放在**参数区上面**：先看到
这层有什么调整，再改参数，也更符合顺序。

**④ `aboutToAppear` 要载入类型清单。**

`loadAdjustKinds` 原来只在 `refresh()` 里调，而 `refresh` 只在用户点操作后跑——
首次进页面「调整」区的下拉一直是空的，看着像没做。

**5. 验证（`scripts/verify_ohos_layers.py`，16 项 0 失败）。**

```
=== 四、折叠（组可收起） ===
  [PASS] 点组行的 ▾ 收起子层（行数变少）  —— 收起前 4 行 -> 收起后 3 行
  [PASS] 点 ▸ 展开（子层回来）  —— 3 行 -> 4 行

=== 五、多选与批量建组 ===
  [PASS] 勾选两个层后提示「多选 2 层」
  [PASS] 批量建组：两个层一起收进新组  —— 建组 2/2 ✓ / 缩进 2 行

=== 六、可编辑调整层 ===
  [PASS] 新增一条调整（引擎拼 JSON，rc=0）  —— 新增 ✓
  [PASS] 该层「已有」列表里出现这条调整
  [PASS] 点已有条目进入编辑（按钮变「更新」+ 回填提示）  —— 更新 / 编辑第 1 条：Hue/Saturation
  [PASS] 点 ✕ 删掉这条调整（引擎里也没了）  —— 删除 ✓
```

加上原有的分组 / 缩进 / 拖进组 / 引擎侧自检 / 调整目标，脚本一共 16 项，一条命令
复跑。壳自检 285 项与视觉探针 268 项未受影响。
### 5.80 窗口可拖拽缩放 + 鸿蒙混合模式 / 剪贴蒙版（2026-10-27）

老板：「软件窗口大小可以像窗口一样自行拖拽大小」「继续下一批」。

#### 一、窗口缩放：先证伪，再补缺口

先实测：四条边、四个角、双击标题栏最大化，`WM_NCHITTEST` 全部返回正确的缩放码，
真实拖动也都生效。**内核没问题**——会让人以为「拖不动」的是两处体感：

1. **看不出哪里能拖。** 深色 UI 把客户区内容画到窗口最外沿，系统那圈 8px 边框与
   内容没有视觉分界，用户是「凭感觉往边上按」，按偏 2px 就落进面板/画布。
   → `WM_NCHITTEST` 在客户区**内侧**再铺 3 逻辑像素（左右下三边 + 两个角）。
   顶部不做：菜单与工具条贴着窗口顶边，让出 3px 会把「文件」菜单第一行咬掉。
2. **没有下限。** 此前能拖到客户区 0 高，布局各档（选项栏 → 标签栏 → 右栏 →
   左栏）连着降级，最后画布只剩一条缝、面板全消失——那不是小窗口，是不可用。
   → `WM_GETMINMAXINFO` 给下限：`kMinWindowWidth = 600`、`kMinWindowHeight = 360`
   （逻辑像素）。

两个数字的来历都踩过坑：

- **`AdjustWindowRectEx` 不能用。** 它是 DPI-unaware 的，在 DPI 感知进程里算出的
  边框比实际窄一半（实测客户区少了 8×19，正好把选项栏挤掉）。改成**量当前窗口**的
  `WindowRect - ClientRect`，再与 `GetSystemMetrics` 的估算取大——兜住「窗口正在
  改尺寸、菜单恰好折成两行」的中间态。
- **最小宽度受菜单栏约束，不是布局。** 按布局常量推出来是 476，而 476 宽时 9 个
  菜单项折成两行：非客户区凭空高 19px、客户区反而更矮，「最小尺寸」变成自我指涉
  的量。取 600（菜单单行 + 一档字体/DPI 余量）。

顺带修了**工具条在窄窗下的叠字**：右侧读数（缩放/笔刷/渲染）原先是「从右边缘往里
520px 再右对齐」，1280 宽就开始压按钮，900 宽糊成一团「缩放 30%笔刷 24 px75%渲染
CPU」。现在宽度是**让出来**的——先扣掉最后一个工具按钮，再分三档（完整 / 只留
「缩放 %」/ 不画）；超出窗口的按钮整块不画（画半个比看不见更糟：用户会以为那里
能点，点下去落到画布上）。

窗口几何（位置 / 大小 / 是否最大化）存 `%APPDATA%\Compositor\window.ini`。不并进
dock 的 `workspace.ini`：那份文件由 `DockState::save` **整体重写**，两个写者共用
一个文件必然互相覆盖。最大化存 `GetWindowPlacement` 的 `rcNormalPosition`（还原
矩形），在 `WM_DESTROY` 存——`WM_CLOSE` 存早了会把「用户取消了关闭」的那次位置
也记下来。

还修了一个回退路径的坑：屏幕外的旧位置要退回缺省，但 `SetWindowPos` **不认**
`CW_USEDEFAULT`（那是 `CreateWindowEx` 专用值 0x80000000），会把窗口送到 -32768。
改成「只恢复大小，位置交给系统」。

#### 二、鸿蒙：混合模式 + 剪贴蒙版 + 回填精度

- **混合模式下拉。** 此前只有一个写死「乘法」的演示按钮。清单与显示名**都来自引擎**
  （`comp_blend_mode_name`，壳只做 `LINEAR_DODGE -> Linear Dodge` 的格式变换），
  壳里没有「24 档」这个数字——引擎加档位，下拉自动多一项。与建组/移出/删除同一套
  批量语义：有多选就全改。图层行加「混」标记（非 Normal 才显示）。
- **剪贴蒙版。** 引擎的 `set_clipping_source` 早就能用，壳一直没接。基底**不让用户
  挑**：按引擎的成栈规则取「下方紧邻的同父图层」（数组序自下而上，所以下方才是
  基底；取上方会得到「面板说有剪贴、画面毫无变化」的假成功）。邻居自己已是成员时
  挂到同一个基底上（并入已有剪贴栈），跨组拒绝。图层行加「剪」标记。
- **参数回填按 `json_path`。** 原先按 param 下标回填，引擎调顺序或在中间插参数就
  会整体错位一格——每个值单看都在合法范围内，界面上完全看不出异常。现在
  napi 回报 `path\x1f值\x1fused_default`，壳按路径匹配；`used_default` 意味着文档里
  没有这个键（旧 settings / 引擎新加参数），提示里显式写出「N 项缺省」。

#### 三、验证脚本自己的一个坑

`verify_ohos_layers.py` 原来靠点调试区的「蓝层」按钮选层。这一轮图层面板加了
「混合/剪贴」一行（+34vp），页面内容把 Column 撑满，**底部调试区的
`layoutWeight(1)` Scroll 拿到 0 高度**——不是一个按钮变矮，是整个区块不渲染，而
`tap()` 找不到文本只返回 False、不报错。症状从第六节起全崩，且图层一路攒到 6 层、
列表里全是组。

两处修：给调试区**固定 112vp**（不再靠剩余空间）；脚本每节 `fresh_document()`
（重启 + 新建）回到确定起点，并按文本点**列表行**选层而不是点工具栏按钮。

#### 四、基线

引擎 794 例 / 0 失败；壳自检 **285 项 / SHA `72a07fbfed54bf7d` 未变**；视觉探针
**268 → 280 项 / 44 → 45 场景**（新增 `window_resize` 12 项：客户区内侧 1~2px 是缩放
命中而 5px 已是客户区、左下/右下返回对角码、顶部仍是客户区、真实拖右下角后客户区
变大且布局跟着长、缩到 200×150 被下限挡在 600×360、退出前大小重启后原样回来）；
鸿蒙验证 **16 → 23 项 0 失败**（混合下拉显示当前模式 / 选 Screen 写回引擎 / 「混」
标记、剪贴挂到下方同父层 / 「剪」标记 / 再点取消、回填不出现「缺省」）。

### 5.81 鸿蒙图层蒙版 + 色阶分页 + 浮窗出屏吸回（2026-10-27）

上一轮结尾列的「下一批」三项。

#### 一、鸿蒙图层蒙版

引擎的 `comp_layer_set_mask` / `comp_layer_get_mask` 早就能用（Windows 侧接的是
「从选区建立蒙版」），鸿蒙一条没接。但鸿蒙**没有选区工具**，那条路缺前置条件。

改成 PS 的另一条：**从图层自身的透明度建立蒙版**（Layer > Layer Mask > From
Transparency）。实现不走「自己读像素再按 transform 合成」——图层可能有缩放/旋转/
翻转，自己合成等于把引擎的坐标映射再实现一遍（Windows 侧同一个坑踩过：映射错了
蒙版与画面错位，屏幕上只表现为「蒙版边缘有点怪」，很难归因）。

走的是「图层 alpha → 选区 → coverage → 图层蒙版」：

```
comp_selection_load_layer(LOAD_PIXELS, REPLACE)   // 引擎按 transform 映射回文档坐标
comp_selection_get_coverage(&mask)                // 文档分辨率的有效 coverage
comp_layer_set_mask(id, &mask)
comp_selection_deselect()                         // 中转物，无论成败都清掉
```

最后一步不能省：鸿蒙壳没有选区 UI，留着这个临时选区会让后续操作（滤镜、裁剪）
莫名其妙只作用于那一块。

两个边界：组/空层没有像素 → 拒绝并说明；图层**完全透明**时 coverage 全 0，
建出来是一张全黑蒙版＝「点一下这层就没了」，所以也拒绝，提示「这层完全透明」。
图层行加「蒙」标记。

#### 二、鸿蒙色阶通道分页

色阶有 4 通道 × 5 参 = 20 条滑杆，此前全平铺在一个 88vp 高的 Scroll 里——一屏
只能看到两条，「调一个参数要滚半天」，等于这个功能不可用。Windows 壳早就是按
通道分页。

分组依据是参数路径里的 `ranges.<数字>`（`levels.ranges.1.black` → 通道 1），
**不是硬编码「4 通道 / 每通道 5 条」**：那组数字在引擎的参数表里，引擎加通道或
加参数时，壳跟着改就会漏。通道名取该组第一条参数的 label 首词（引擎给的是
`RGB Black` / `R Black`…），壳不维护第二份名字表。

#### 三、Windows 浮窗出屏吸回

面板浮出来之后可以拖到任意位置，拖到屏幕外就再也抓不回来；多显示器拔掉一块之后
更糟——存下来的几何整个落在不存在的区域，下次启动就是「面板不见了」，界面没有
任何提示，用户只能去删 `workspace.ini`。

判据用**与显示器工作区的交集面积**，不是「中心点在某块屏上」：

- 窗口 90% 在屏外时，中心点可能仍在屏内，但用户看到的只剩一条边；
- 窗口正好卡在两块屏之间的缝里时，中心点又可能落在无效区域。

两种都会让「按中心点判」得出「一切都好」。阈值取绝对 40×40（还看得出是个窗口）
与自身面积的 1/4（大窗口露出一个角也算抓得住）的较大者。

三个触发点：浮窗拖完（`WM_EXITSIZEMOVE`）、从面板里拖出来松手、启动恢复几何
之后。**不在 `WM_MOVE` 里判**——那是拖动过程中连续发的，每帧都拉会让窗口「粘在
屏幕边上」，用户想挪到副屏边缘时手感会变得很怪。

#### 四、这一轮踩的坑：参数区整个不渲染

色阶分页做完，数据全对（`参数20/页5/通道4`），界面上却一条滑杆都没有。

第一层原因：参数区还在**调试按钮区的 Scroll 里**（上一轮它就在那儿，只是靠滚动
能看到）。搬到调整区之后仍然不显示。

第二层原因才是真的：整页内容超过一屏，`Column` 会把**可压缩的子元素按比例压扁**
——调整区最后一个子元素（参数区）被压成 0 高，而界面上没有任何报错。通道条能
显示是因为它在压缩后的空间里恰好排得进去。

修法不是「给参数区加高度」（那只会让它撑出屏幕），而是**给页面腾出空间**：
标题 20→14、画布 150→120、两行状态合成一行、自检按钮并进常用按钮行。

顺手纠正了一个结构错误：参数区本来就该在调整区里（`调整 / 选择类型 / 通道条 /
参数 / 已有`），不在调试区。

`verify_ohos_layers.py` 也跟着补了「整页滚动」能力（`scroll_page`）——页面现在
比一屏高，`dumpLayout` **只回可视节点**，屏幕外的「已有」行与参数区 `find` 直接
找不到，断言会拿到空列表、点击会静默返回 False。同时 `layer_rows()` 的行范围
判据从「图层标题 → 不透明度行」改成「勾选点 ○ 的 y 区间」：不透明度行在层多时
会滚出屏幕，那个下边界会读成 None，整个列表被判成空。

#### 五、基线

引擎 794 例 / 0 失败；壳自检 **285 项 / SHA `72a07fbfed54bf7d` 未变**；视觉探针
**280 → 284 项 / 45 → 46 场景**（新增 `float_clamp` 4 项：命令浮起有浮窗 / 先确认
真在屏外 / 松手后拉回可见区 / 拉回位置贴着主窗口）；鸿蒙验证 **23 → 30 项 0 失败**
（蒙版建立与清除 + 「蒙」标记、色阶 4 通道页 + 默认页只显示当前通道 + 切 R 后
参数换组）。
