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
| 编解码能力查询 | 导入/导出菜单 | 预留 | `comp_io_codec_support` | 已达成 |
| PNG 导入/导出 | 可用 | 可用 | `comp_io_import` / `comp_io_export` | 已达成（libpng+zlib） |
| JPEG 导入/导出 | 可用（有损） | 可用 | 同上 | 已达成（stb） |
| PSD 只读导入 | 可用 | 可用 | `comp_io_import` | 已达成（stb） |
| 窗口表面上屏 | AUTO 回落 CPU；HWND 返回 `UNSUPPORTED` | `presentSurface` 返回 `UNSUPPORTED` | `comp_render_document`(kind=HWND/HARMONY_WINDOW) | **有边界**：设备侧交换链未落地，两壳均如实拒绝 |
| 离屏 present/wait | 无操作成功 | 无操作成功 | `comp_render_present` / `comp_render_wait` | 已达成 |

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

1. **设备侧上屏**：Windows 无 `d3d12.h`（SDK 仅 8.1），D3D12 真实路径不可编译；鸿蒙无在线设备
   （`hdc list targets` 为空），Vulkan 交换链未落地。故两壳窗口表面统一 `UNSUPPORTED`。
2. **鸿蒙 UI 面板**：鸿蒙壳目前只有 `startup/shutdown/backendName/flatten/surfaceInfo/presentSurface`
   六个桥函数，尚未接图层/调整面板 UI；能力矩阵中标注为「未接 UI」，非能力缺失。
3. **鸿蒙 HAP 真机验证**：交叉编译（aarch64 / x86_64）均可产出 `.so`，但无设备可上机。

以上三项均属**环境边界**，不涉及两壳语义分叉；共享回归护栏已把可离线验证的部分全部锁死。
