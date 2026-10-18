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
| 图层增删改查 | 图层面板（增/选/显隐/不透明度） | （未接 UI） | `comp_layer_*` | **部分达成**：Windows 面板仅接 `add`/`get_visible`/`set_visible`/`set_opacity`；`remove`/`reorder`/`set_blend`/`set_transform`/`set_mask`/`set_clipping_source` 无调用点（仅 selftest 覆盖 `set_pixels`） |
| 调整图层参数编辑 | 调整面板 | （未接 UI） | 调整 ABI 全套 | 已达成（鸿蒙无面板） |
| 调整控件元数据 | `refresh_adjustments` | 预留 | `comp_layer_adjustment_param_at` 等 | 已达成 |
| 滤镜菜单 / 参数面板 | 「滤镜(&F)」动态菜单 + 右侧参数面板 | （未接 UI） | `comp_filter_apply` | 已达成（鸿蒙无面板） |
| Blur 修饰工具 | 画布折线 → 一次 `COMP_TOOL_BLUR` | （未接 UI） | `comp_tool_apply` | 已达成（鸿蒙无面板） |
| 编解码能力查询 | 导入/导出菜单 | 预留 | `comp_io_codec_support` | 已达成 |
| PNG 导入/导出 | 可用 | 可用 | `comp_io_import` / `comp_io_export` | 已达成（libpng+zlib） |
| JPEG 导入/导出 | 可用（有损） | 可用 | 同上 | 已达成（stb） |
| PSD 只读导入 | 可用 | 可用 | `comp_io_import` | 已达成（stb） |
| 窗口表面上屏 | AUTO 回落 CPU；HWND 返回 `UNSUPPORTED` | `presentSurface` 返回 `UNSUPPORTED` | `comp_render_document`(kind=HWND/HARMONY_WINDOW) | **有边界**：设备侧交换链未落地，两壳均如实拒绝 |
| 离屏 present/wait | 无操作成功 | 无操作成功 | `comp_render_present` / `comp_render_wait` | 已达成 |
| 工具条 / 画布导航 | 17 工具按钮、滚轮缩放、中键/空格平移 | （未接 UI） | 无（纯外壳，不跨 ABI） | **仅 Windows**：鸿蒙壳尚无对应面板，不影响共享语义 |

「有边界」表示：接口已完整接线，但因缺少设备侧上屏实现，两壳统一**如实返回
`COMP_ERR_UNSUPPORTED`**，不伪造成功。一旦窗口交换链落地，两壳无需改动调用点。

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
| `shell_contract_window_surfaces_report_unsupported` | HWND / HARMONY_WINDOW 均返回 `UNSUPPORTED`；离屏 present/wait 无操作成功 |
| `shell_contract_codec_capability_query_is_shared` | PNG 可用、JPEG/PSD 为 VENDORED、TIFF 为 UNAVAILABLE，且 note 非空 |
| `shell_contract_adjustment_control_metadata_is_shared` | kind=3=Exposure、3 个参数、首个路径 `exposureSettings.exposure`；越界显式失败 |

运行方式：`python scripts/build.py`（回归内含），或 `build/compositor_tests.exe`。
任意一壳所依赖的常量/语义被改动，这里立即失败。

---

## 4. 边界与未达成项（如实记录）

1. **设备侧上屏**：D3D12 代码可在本机 SDK（VS 2022 / Windows SDK 10.0.26100）下编译，
   但 `render_document` 仍走 CPU，交换链未接主渲染路径；鸿蒙无在线设备
   （`hdc list targets` 为空），Vulkan 交换链未落地。故两壳窗口表面统一 `UNSUPPORTED`。
2. **鸿蒙 UI 面板**：鸿蒙壳目前只有 `startup/shutdown/backendName/flatten/surfaceInfo/presentSurface`
   六个桥函数，尚未接图层/调整面板 UI；能力矩阵中标注为「未接 UI」，非能力缺失。
3. **鸿蒙 HAP 真机验证**：交叉编译（aarch64 / x86_64）均可产出 `.so`，但无设备可上机。

以上三项均属**环境边界**，不涉及两壳语义分叉；共享回归护栏已把可离线验证的部分全部锁死。

---

## 5. Windows 壳 UI 地基（2026-10-18）

上游 `Compositor` 只有 macOS 应用层，**没有 Windows 版可供参照**，故 Windows 壳的
编辑器界面是自建的。为让 UI 数学可回归，平台无关部分被抽到
`shells/windows/src/ui_core.h`（header-only，不 include Windows 头、不 include `engine/` 头、
不分配内存），由 `tests/test_ui_core.cpp` 的 12 例覆盖；`main.cpp` 只做
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
`Ctrl+N` / `Ctrl+O` / `Ctrl+S`。

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

### 5.4 未达成的 UI 项

| 缺口 | 状态 | 原因 |
|------|------|------|
| Move 工具拖拽平移图层 | 未做 | 需图层 transform 写入侧与像素回读配合；现有 `comp_layer_*` 不足以在不猜语义的前提下实现 |
| 工具按钮图标 | 未做 | 当前为文字快捷键字母，避免引入图标资源/字体 |
| 画布上的选区 / 蚂蚁线 / 裁剪手柄 | 未做 | 选区 ABI 已具备（README §6.7），交互层待补 |
| 笔刷落笔 | 部分做 | Blur 已接「折线 → `COMP_TOOL_BLUR`」；Brush/Eraser 等仍缺可视笔迹预览与逐笔撤销粒度 |
| 滤镜实时预览 | 未做 | 当前是「回车应用一次」，无拖动滑杆的增量预览缓存 |
| Smudge / Liquify 壳层接线 | 未做 | 内核与 `COMP_TOOL_SMUDGE`/`LIQUIFY` 已具备，状态栏如实显示「尚未接内核」 |
| 面板缩放 / 停靠 | 未做 | 固定布局，窄窗口按规则收起 |

以上均为**外壳交互**缺口，不涉及引擎核与两壳共享语义。
