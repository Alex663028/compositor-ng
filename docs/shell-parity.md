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
| 滤镜菜单 / 参数面板 | 「滤镜(&F)」动态菜单 + 右侧参数面板 | （未接 UI） | `comp_filter_apply` | 已达成（鸿蒙无面板） |
| 图层效果参数面板（P0-2） | 「图层效果」菜单 + 右侧参数面板（`Ctrl+Shift+E`） | （未接 UI） | `comp_layer_effect_param_count` / `_param_at` / `_read_value` / `_build_settings` | **已达成**（P0-2，见 §5.13）：标签/量程/缺省值全部来自 ABI 元数据表，壳不再自持任何一份数值 |
| Blur 修饰工具 | 画布折线 → 一次 `COMP_TOOL_BLUR` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板） |
| 渐变工具（S9） | 画布拖拽两端点 → 一次 `COMP_TOOL_GRADIENT` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；线性/径向、Shift 反向） |
| 形状工具（S10） | 画布拖拽对角点/两端点 → 一次 `COMP_TOOL_SHAPE` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；矩形/椭圆/直线，Shift 正方、Alt 从中心） |
| 文字工具（S11） | 点击落插入点 + 键入 ASCII → 一次 `COMP_TOOL_TYPE` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板；内置 5×7 点阵，ASCII-only） |
| 编解码能力查询 | 导入/导出菜单 | 预留 | `comp_io_codec_support` | 已达成 |
| PNG 导入/导出 | 可用 | 可用 | `comp_io_import` / `comp_io_export` | 已达成（libpng+zlib） |
| JPEG 导入/导出 | 可用（有损） | 可用 | 同上 | 已达成（stb） |
| PSD 只读导入 | 可用 | 可用 | `comp_io_import` | 已达成（stb） |
| 离屏上屏（GPU 协同） | AUTO → D3D12 离屏计算 + 回读；**HWND 直呈已落地（S16，`caps().window_present`）** | `presentSurface` 返回 `UNSUPPORTED` | `comp_render_document`(kind=HWND/HARMONY_WINDOW) / `comp_render_present` / `comp_render_flatten` | **部分达成**：Windows 的设备侧离屏（S13）与交换链直呈（S16）都已落地；鸿蒙设备侧交换链未落地，窗口表面如实拒绝 |
| 离屏 present/wait | 无操作成功 | 无操作成功 | `comp_render_present` / `comp_render_wait` | 已达成 |
| 工具条 / 画布导航 | 17 工具按钮、滚轮缩放、中键/空格平移 | （未接 UI） | 无（纯外壳，不跨 ABI） | **仅 Windows**：鸿蒙壳尚无对应面板，不影响共享语义 |

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
`R` Blur/Smudge/Liquify · `G` Gradient · `U` Shape · `T` Type · `H` Hand · `Z` Zoom

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
| Move 工具拖拽平移图层 | 未做 | 需图层 transform 写入侧与像素回读配合；现有 `comp_layer_*` 不足以在不猜语义的前提下实现 |
| 工具按钮图标 | 未做 | 当前为文字快捷键字母，避免引入图标资源/字体 |
| 选区羽化 / 扩缩手柄 | 未做 | ABI `comp_selection_modify` 已有；壳层只接了矩形框选与全选/取消/反选 |
| Brush/Eraser 逐笔撤销粒度 | 部分做 | 已有可视笔迹预览与笔尖轮廓；Brush/Eraser 的像素提交仍走既有 Stroke 通路 |
| 滤镜实时预览 | 未做 | 当前是「回车应用一次」，无拖动滑杆的增量预览缓存 |
| 面板缩放 / 停靠 | 未做 | 固定布局，窄窗口按规则收起 |

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
重排/撤销后行号会失效）。**拖拽只作用于主选中层**——多选批量拖拽未做，不假装支持。

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
  于是面板里子层在组行之上（与上游 PS 相反）。这是沿用引擎既有落位口径，不是本轮新引入；
  改显示需要树形面板，未做。
- 面板没有树形缩进、没有组折叠、没有 Shift 点选连续区间（只有 Ctrl 追加）。
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
② 颜色是 `red`/`green`/`blue` 三个 0..1 数字，无取色器/HSL；③ 只编辑当前选中图层，
多选批量加效果未做；④ 参数表与 `effect_eval.cpp` 是两份手写常量，靠像素对拍守而不是编译期
联动——新增参数必须两处同改；⑤ 手感/跨 DPI 待 ALEX 目视（`docs/manual-test.md` M17）。

