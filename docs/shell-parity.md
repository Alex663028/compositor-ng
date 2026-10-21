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
| 图层效果参数面板（P0-2） | 「图层效果」菜单 + 右侧参数面板（`Ctrl+Shift+E`） | （未接 UI） | `comp_layer_effect_param_count` / `_param_at` / `_read_value` / `_build_settings` | **已达成**（P0-2，见 §5.13）：标签/量程/缺省值全部来自 ABI 元数据表，壳不再自持任何一份数值 |
| Blur 修饰工具 | 画布折线 → 一次 `COMP_TOOL_BLUR` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板） |
| 减淡 / 加深 / 海绵（P1） | `O` 循环 8 档，画布折线 → 一次 `COMP_TOOL_DODGE` / `_BURN` / `_SPONGE` | （未接 UI） | `comp_tool_apply` | **已达成**（§5.19）：`Alt+[ / ]` 调强度，受选区约束 |
| 历史记录画笔（P1） | `Y` 工具 + 历史面板取样（Shift+点行），画布折线 → 一次 `COMP_TOOL_HISTORY_BRUSH` | （未接 UI） | `comp_tool_apply`（kind 尾部追加，导出数仍 138） | **已达成**（§5.20）：取样不回跳；一笔 = 一个撤销步；`Alt+[ / ]` 调不透明度 |
| 渐变工具（S9） | 画布拖拽两端点 → 一次 `COMP_TOOL_GRADIENT` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；线性/径向、Shift 反向） |
| 形状工具（S10） | 画布拖拽对角点/两端点 → 一次 `COMP_TOOL_SHAPE` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；矩形/椭圆/直线，Shift 正方、Alt 从中心） |
| 文字工具（S11） | 点击落插入点 + 键入 ASCII → 一次 `COMP_TOOL_TYPE` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；内置 5×7 点阵，ASCII-only） |
| 编解码能力查询 | 导入/导出菜单 | 预留 | `comp_io_codec_support` | 已达成 |
| PNG 导入/导出 | 可用 | 可用 | `comp_io_import` / `comp_io_export` | 已达成（libpng+zlib） |
| JPEG 导入/导出 | 可用（有损） | 可用 | 同上 | 已达成（stb） |
| PSD 只读导入 | 可用 | 可用 | `comp_io_import` | 已达成（stb） |
| 离屏上屏（GPU 协同） | AUTO → D3D12 离屏计算 + 回读；**HWND 直呈已落地（S16，`caps().window_present`）** | `presentSurface` 返回 `UNSUPPORTED` | `comp_render_document`(kind=HWND/HARMONY_WINDOW) / `comp_render_present` / `comp_render_flatten` | **部分达成**：Windows 的设备侧离屏（S13）与交换链直呈（S16）都已落地；鸿蒙设备侧交换链未落地，窗口表面如实拒绝 |
| 离屏 present/wait | 无操作成功 | 无操作成功 | `comp_render_present` / `comp_render_wait` | 已达成 |
| 工具条 / 画布导航 | 20 工具按钮、滚轮缩放、中键/空格平移 | （未接 UI） | 无（纯外壳，不跨 ABI） | **仅 Windows**：鸿蒙壳尚无对应面板，不影响共享语义 |

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
- **九个滤镜、一张表**：菜单、面板、值映射都从 `filter_table()` 派生
  （Vignette / Tonal Contrast / Bloom / Lens Correction / Gaussian Blur /
  Add Noise / Grain / Motion Blur / Progressive Blur）。布尔项（`Preserve` /
  `Gaussian` / `Mono` / `Radial` / `Inverted`）在面板上是
  0/1 行，`build_filter_desc` 用 `>= 0.5` 转成 ABI 的 int32；`Seed` 用
  `llround` 转 `uint32`（面板量程 0..4294967295，正好是 uint32 全域）。
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
- 键入映射：`type_ascii_from_vk(vk, shift)` 只覆盖 **US 布局**的字母、数字与常用标点
  （字母区 + `0x30..0x39` + `OEM_*`）；未识别键返回 0，不猜其它布局；
- 手势：`T` 选工具 → 画布点击落插入点 → 直接键入 ASCII → `Enter` 提交一次
  `comp_tool_apply(COMP_TOOL_TYPE)`；`Shift+Enter` 换行、`Backspace` 删字符、
  `Esc` / 切工具 / 失焦取消（只丢预览、不产生撤销步）；`[ / ]` 调字号、
  `Shift+[ / ]` 调字距；已在键入态时再点画布 = 先提交上一段、再在新位置落点；
- 键入态是**模态**的：普通字符不再触发工具快捷键（`B` 是字母不是画笔）、
  空格也不再是临时抓手，只放行 Ctrl 组合（撤销/保存等）；`WM_KILLFOCUS` 取消；
- 画布预览只画插入点光标 + 文本框（白实线 + 黑虚线双描边），**不预演字形**：
  外壳没有系统字体（字体是引擎内的点阵），用 GDI 画一套只会与落笔结果不一致；
  像素在 `Enter` 时由引擎一次写出（与渐变/形状「拖完一次提交」同口径）；
- 与上游的差异：ASCII-only（无 CJK / 字体选择 / 粗斜体 / 对齐 / 旋转）、
  文字是**一次性像素**而不是可编辑文本对象、画进当前图层而不是新建图层
  （见 `docs/tool-ops.md` §4.12）。

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
| 选区扩缩手柄 | 未做 | 面板里的**数值**扩缩已接（`comp_selection_modify` 1..500）；缺的是画在选区边上的拖拽手柄——手柄要重采样蒙版，而 ABI 没有「按任意覆盖率写回选区」的入口（`comp_selection_modify` 是相对扩缩，无法表达手柄的绝对位移），壳侧不自造 |
| Brush/Eraser 逐笔撤销粒度 | 已做 | 每一笔 = 一次 `comp_tool_apply` = 一步撤销（笔迹预览只在宿主侧，不落文档）；「笔中逐段撤销」上游也没有 |
| 滤镜实时预览 | 已做（2026-10-21，§5.35） | 改参数即出帧：宿主事务里应用 → 取帧 → abort，预览只在 framebuffer，文档不动；回车真应用，结果与预览逐字节一致 |
| 面板缩放 / 停靠 | 部分做（2026-10-21，§5.33） | 宽度可拖：左右面板内侧 5 px 分界线，图层面板 168–520 px、右侧面板 240–560 px，双击分界线复位；停靠（拖出成浮窗 / 面板合并 / 收起按钮）未做 |

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

**8. 仍未做。** 停靠：拖出成浮窗、面板合并、把面板整块收起/展开都不在本次范围内；
偏好只在会话内保存（`ShellState` 字段），不落盘到设置文件。

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









