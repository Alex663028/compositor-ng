# compositor-ng

Compositor 跨平台统一架构的**引擎核**实现（单仓）。同一份 C++17 引擎核 + 稳定 C ABI，
供 Windows 壳（D3D12）与 HarmonyOS 壳（Vulkan）共同调用。

- 上游参考工程：`robbietilton/Compositor`（macOS / Swift + Metal + CoreImage），MIT。
- 本仓不是上游的 fork，而是按《Compositor 跨平台统一架构评估报告》第八章路线**重写**的引擎核。
  当前进度：**M1（内核奠基）完成并验证通过**；**M2 完成**——像素内核区间并行调度、合成有向图
  （FilterGraph）与图层组穿透/隔离语义、仿射变换与高质量重采样、`.comp` 像素改 PNG + UUID
  图层 id（打通上游互通）均已落地。**M3 进行中**——渲染后端接口切片（`rhi.h` + CPU/D3D12/Vulkan
  三后端）、SPIR-V 构建链路、Vulkan 计算管线设备侧实现（opt-in）、Windows 壳（Win32 + GDI 上屏、
  菜单/快捷键、图层面板、调整参数面板、撤销接线）与 HarmonyOS 壳（NAPI + XComponent）均已有可编译产物；
  另外落地了 **M5 编解码**（能力登记表 + PNG/JPEG 真实导入导出、PSD 只读；第三方库以源码
  vendoring 进 `engine/third_party/`）。
  全量 **135 用例 0 失败**；Windows 外壳实机自检 **27 项全 OK**（与仓内基线逐字节一致）。
  2026-10-15 追加（按审核结论改善 + 上游对齐）：撤销栈**像素资产化**（shared_ptr + COW，
  对齐上游 DocumentHistory 的「独占字节」账本，快照不再逐字节拷贝像素）、**向下合并 /
  组合并 / 拼合图像**（对齐上游 ⌘E 家族，渲染不变）、一键回归入口
  `scripts/run_regression.py`（含鸿蒙交叉编译，6 步全绿）。
  2026-10-15 再追加（第二轮独立审计）：修复 12 个缺陷并补10 条回归护栏，
  详见 `../report/AUDIT2-compositor-ng-20261015-claude.md`——
  含 `remove_subtree` 悬垂指针（UAF）、`.comp` 非法像素格式导致步长归零、
  事务句柄悬垂、活动事务内可undo/redo 导致历史链错位、跨父级 reorder、
  6 个 adjustment ABI 缺异常护栏（可`std::terminate` 宿主）等。
  2026-10-15 第三轮（架构风险轻量收口）：**句柄有效性**（对象内置 magic，
  析构擦除——销毁后使用/类型混淆/野指针一律得到错误码而非崩溃，重复 destroy
  为无操作）、**反序生命周期兜底**（引擎先销毁时自动回收在册文档）、
  **错误槽线程局部化**（多线程共用一个引擎不再数据竞争）、
  **定制分配器真正接入**（引擎/文档对象经 `desc.alloc` 分配，placement new
  配对手动析构；alloc/free 必须成对，缺 realloc 时 `reallocate` 不再越界读）、
  **`comp_command_list` 改按文档求值 enabled**（与 `comp_command_state`
  共用同一份逻辑，消除两套标准）。

## 1. M1 目标与范围

| 项目 | 内容 |
|------|------|
| 目标 | ABI 定稿、领域模型与 10 个像素内核可编译可测 |
| 交付物 | `libcompositor_engine` 静态库、`engine/abi/include/compositor/*.h`、单元测试 |
| 不在范围 | 栅格图像编解码（PNG/JPEG/HEIF/OpenEXR…）、GPU 后端、FilterGraph、UI |

M1 的验收标准是「**能编过、能跑过**」，不是「功能完整」：所有超出 M1 的能力都必须
显式返回 `COMP_ERR_UNSUPPORTED`，不得静默成功。

上游对齐原则（M1 收口时确立）：**blocks 用法与 ABI 语义一律以上游 `robbietilton/Compositor`
为准**，不做自作主张的重新设计。

## 2. 目录结构

```
compositor-ng/
├── CMakeLists.txt                 # 标准构建（优先级最高）
├── README.md
├── .gitignore                     # 构建产物 / Python 缓存 / 历史产物目录（P2-6：此前缺失）
├── scripts/
│   ├── build.py                   # 可移植构建（无需 CMake，借 PyPI ziglang 的 clang）
│   ├── build.sh                   # POSIX / Git Bash 入口
│   ├── build.ps1                  # Windows PowerShell 入口
│   ├── build_windows_shell.py     # ★ Windows 壳构建（复用 build/*.o 直接链接）
│   ├── build_ohos.sh              # ★ 鸿蒙交叉编译引擎静态库（aarch64-linux-ohos）
│   ├── build_ohos_shell.sh        # ★ 鸿蒙 NAPI 共享库构建（libcompositor_ohos.so）
│   ├── build_spirv.sh             # ★ GLSL -> SPIR-V 构建链路（DevEco 自带 glslang）
│   ├── gen_vulkan_bindings.py     # ★ 从 GLSL 抽取 Vulkan 绑定/推送常量布局常量头
│   ├── check_vulkan_slice.py      # ★ Vulkan 切片自检（双变体编译 + 回归 + 外壳链接）
│   ├── run_regression.py          # ★ 一键回归（构建+单测+壳自检+两个 crosscheck+shared+鸿蒙交叉编译）
│   └── once/                      # 一次性补丁 / 诊断脚本（P2-7：已从 scripts/ 根移入，非构建入口）
├── docs/
│   ├── adjustment-json-keys.md    # ★ 调整参数 JSON 键名表（对齐上游 $LayerAdjustment）
│   ├── codec-migration.md         # ★ M5 编解码替换：探测证据、能力矩阵与接入步骤
│   └── shell-parity.md            # ★ 两壳一致性：能力矩阵、冻结常量与共享回归护栏（M5）
├── engine/
│   ├── engine.h / engine.cpp      # 引擎实例：分配器、后端、线程池、错误状态
│   ├── core/                      # 平台无关基础层
│   │   ├── comp_geometry.h        # header-only 几何与画布合法性（30000 边 / 1e8 像素）
│   │   ├── comp_alloc.h/.cpp      # 分配器抽象（可注入自定义 allocator）
│   │   ├── comp_parallel.h/.cpp   # C ABI 区间并行原语（替代 GCD dispatch_apply）
│   │   ├── comp_blocks_rt.c       # 非 Apple 平台的 BlocksRuntime 最小实现（见 §4）
│   │   └── comp_meta.cpp          # comp_version / comp_result_name / comp_blend_mode_name
│   ├── imaging/                   # 10 组纯 C 像素内核（移植自上游，已去 GCD）
│   │   ├── comp_imaging.h         # ★ 统一入口：为 C++ 调用方提供 extern "C" 包裹
│   │   ├── comp_bands.h/.c        # ★ band 调度：把内核的 block 区间体交给并行层（见 §4）
│   │   └── *Pixels.c/.h           # Adjust/Brush/Deep/Dither/Heal/Lens/Levels/Noise/Patch/Wand
│   ├── domain/                    # 领域模型
│   │   ├── layer.h/.cpp           # Layer + ImageBuffer + MaskBuffer + Adjustment/Effect
│   │   ├── adjustment_table.h/.cpp # ★ 调整参数键名/范围/默认值唯一真源（校验 + settings JSON 构造）
│   │   ├── domain_state.h         # DocumentState / SelectionState（可快照，含 documentID）
│   │   ├── document.h/.cpp        # Document：图层树、事务、撤销、脏标记
│   │   └── history.h/.cpp         # History：整篇快照式撤销栈（默认 200 步）
│   ├── compositing/
│   │   ├── blend.h/.cpp           # W3C 24 种混合模式 + 上游规范字符串映射
│   │   ├── composite_ops.h/.cpp   # ★ 合成基础算子：预乘/直通换算、蒙版、剪贴、单像素混合
│   │   ├── filter_graph.h/.cpp    # ★ 合成有向图：图层树求值、组穿透/隔离语义（见 §4）
│   │   ├── resample.h/.cpp        # ★ 仿射重采样：逆映射取样 + 蒙版烘焙（见 §4）
│   │   └── compositor.h/.cpp      # 文档/图层合成（走 FilterGraph）、降采样
│   ├── render/
│   │   ├── rhi.h                  # ★ RenderBackend 抽象 + 能力探测 + 工厂（④ 接口切片）
│   │   ├── cpu_backend.h/.cpp     # CPU 后端（一致性基准，始终可用）
│   │   ├── d3d12_backend.h/.cpp   # ★ D3D12 后端（`COMP_HAS_D3D12` opt-in，默认诚实 stub）
│   │   ├── vulkan_backend.h/.cpp  # ★ Vulkan 后端（`COMPOSITOR_ENABLE_VULKAN_COMPUTE` opt-in）
│   │   ├── gpu_kernel.h/.cpp      # ★ GPU 内核唯一来源：HLSL + GLSL + C++ 镜像 + 一致性校验
│   │   └── spirv/                 # ★ SPIR-V 链路产物
│   │       ├── gpu_kernel.comp        # 从 gpu_kernel.cpp 抽取的 GLSL（可人工审阅）
│   │       ├── gpu_kernel.comp.spv    # SPIR-V 字模块
│   │       ├── gpu_kernel_spv.h       # 可嵌入的 SPIR-V 字数组
│   │       └── vulkan_compute_bindings.h # 从 GLSL 抽取的绑定/推送常量布局常量
│   ├── format/
│   │   ├── codec_registry.h/.cpp  # ★ 编解码能力登记表（可用性 + 原因，M5 接缝唯一真源）
│   │   ├── codec_lib.h/.cpp       # ★ 真实编解码后端（libpng+zlib / stb；import/export 用）
│   │   ├── comp_json.h/.cpp       # 自研极简 JSON（零第三方依赖）
│   │   ├── comp_png.h/.cpp        # ★ 自研零依赖 PNG 编解码（.comp 内部像素，见 §4）
│   │   ├── comp_uuid.h/.cpp       # ★ UUID 文本/派生（图层 id 与 documentID 用）
│   │   └── comp_file.h/.cpp       # .comp 目录包读写 + 探测（上游 schema）
│   ├── third_party/               # ★ vendored 真实库源码（可复现，随仓）
│   │   ├── zlib/                  # zlib 1.3.1
│   │   ├── libpng/                # libpng 1.6.44
│   │   └── stb/                   # stb_image + stb_image_write
│   └── abi/
│       ├── include/compositor/    # ★ 唯一的对外契约（7 个头文件）
│       │   ├── comp_core.h        # 类型 / 句柄 / 错误码 / 混合模式 / 采样模式 / 渲染目标
│       │   ├── comp_engine.h      # 引擎与文档生命周期
│       │   ├── comp_layer.h       # 图层、调整图层、图层效果（含可见性/组隔离）
│       │   ├── comp_render.h      # 合成、上屏、缩略图
│       │   ├── comp_io.h          # 导入导出（真实库 PNG/JPEG/PSD）与 .comp（内层像素为 PNG）
│       │   ├── comp_command.h     # 声明式命令模型（可序列化，三端共享）
│       │   └── comp_undo.h        # 事务与撤销/重做
│       └── src/                   # ABI 实现（extern "C" 薄封装，不含业务逻辑）
├── shells/                        # 平台外壳（只 include ABI 头，不含像素处理）
│   ├── windows/src/main.cpp       # ★ Win32 + GDI 上屏、菜单/快捷键、图层面板、撤销接线
│   └── ohos/                      # ★ NAPI + XComponent 薄壳 + ArkTS 页面/typings
└── tests/                         # 零依赖测试框架 + 13 组测试（test_main 为入口）
    ├── test_framework.h / test_main.cpp
    ├── test_core.cpp              # 几何、分配器、并行原语、JSON
    ├── test_domain.cpp            # 缓冲、图层、文档、历史/事务
    ├── test_compositing.cpp       # 混合模式与上游枚举的一致性（冻结护栏）
    ├── test_imaging.cpp           # 像素内核可确定性断言的部分
    ├── test_parallel.cpp          # ★ 区间覆盖正确性 + 内核 threads=1/N 等价性
    ├── test_filter_graph.cpp      # ★ 建图/边链、组穿透与隔离、隐藏组、悬空父级拒绝
    ├── test_format.cpp            # ★ PNG 往返/外部解码、UUID、manifest 上游键名护栏
    ├── test_resample.cpp          # ★ 恒等逐字节精确、放大/旋转/翻转、Smooth vs Nearest
    ├── test_abi.cpp               # C ABI 端到端（即「壳的视角」）
    ├── test_rhi.cpp               # ★ RHI 接口切片、GPU 内核镜像与预乘语义护栏（M3）
    ├── test_adjustment.cpp        # ★ 调整键名表/校验/ABI 元数据/回写与 .comp 往返（M5）
    ├── test_codec.cpp             # ★ 能力表事实性 + PNG/JPEG 往返 + 独立解码器交叉（M5）
    └── test_shell_contract.cpp    # ★ 两壳共享契约：常量冻结/生命周期/flatten/表面/编解码/控件（M5）
```

## 3. 构建

### 3.1 标准路径（有 CMake）

```bash
cmake -S compositor-ng -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

MSVC 必须启用 `/utf-8`（源码含 UTF-8 中文注释），`CMakeLists.txt` 已内置。
**Windows 侧请用 clang-cl 而非 cl.exe**：像素内核使用 clang block 语法，cl.exe 不支持（见 §4）。

### 3.2 无 CMake 环境

```bash
python scripts/build.py            # 编译 + 运行测试
python scripts/build.py --no-run   # 只编译
python scripts/build.py --clean    # 先清空 build/ 再全量构建
python scripts/build.py --shared   # 真实共享构建：只产出动态库（不编译/运行单测）
```

`scripts/build.py` 默认使用 PyPI `ziglang` 自带的 clang（无需系统编译器）：

```bash
pip install ziglang
# 国内网络建议走镜像（实测 98.7 MB wheel 约 20 秒）：
pip install ziglang -i https://pypi.tuna.tsinghua.edu.cn/simple
```

也支持显式切换工具链：

```bash
python scripts/build.py --driver cc      # 系统 cc/c++（gcc/clang）
python scripts/build.py --driver msvc    # cl.exe（需 VS 开发者环境；见 §4 限制）
```

`build.py` 的行为：`.c` 用 `-std=gnu11 -fblocks` 编译（10 组像素内核 + band 调度 + BlocksRuntime 桩），
`.cpp` 用 C++17 编译（引擎与测试），最后统一链接并运行测试二进制。**全部 `.c` 与 `.cpp` 都参与
链接**，因此任何一个仍残留平台依赖（如 `dispatch_apply`）都会在链接期直接暴露。

脚本会把 zig 的缓存目录钉在 `build/.zig-cache`，并在宿主缺少 `LOCALAPPDATA` 时提供回退值——
否则 zig 会报 `unable to resolve zig cache directory: AppDataDirUnavailable`。

### 3.3 SPIR-V 构建链路（Vulkan 计算着色器）

Vulkan 只接受 SPIR-V。着色器的**唯一来源**是 `engine/render/gpu_kernel.cpp` 里的 GLSL 字面量，
由构建期抽取并编译，产物入库，避免运行期读文件：

```bash
bash scripts/build_spirv.sh          # GLSL -> .comp -> .spv -> gpu_kernel_spv.h
python scripts/gen_vulkan_bindings.py # 抽取绑定/推送常量 -> vulkan_compute_bindings.h
```

工具链为 **DevEco Studio 自带**（`sdk/default/openharmony/toolchains/glslang_validator.exe`），
**不需要 Vulkan SDK**。实测：`gpu_kernel.comp` 5,020 B → `gpu_kernel.comp.spv` 19,052 B。

### 3.4 鸿蒙交叉编译

```bash
bash scripts/build_ohos.sh            # 引擎静态库（aarch64-linux-ohos）
bash scripts/build_ohos_shell.sh      # NAPI 共享库 libcompositor_ohos.so
```

用 DevEco NDK（`G:/DevEco Studio/sdk/default/openharmony/native`）的 clang 直调
`--target=aarch64-linux-ohos --sysroot=...`；编译行注入 `-fPIC -fvisibility=hidden`
（否则 `ld.lld` 报 `R_AARCH64_ADR_PREL_PG_HI21` 重定位错误）。

## 4. 关键设计决策

| 议题 | 决策 | 后续 |
|------|------|------|
| blocks | 保留上游 clang block 语法（`^`）作为区间回调；非 Apple 平台由 `comp_blocks_rt.c` 提供最小 BlocksRuntime。**调度采用「同步汇合」**：返回前 join 全部工作线程，block 始终不逃逸栈帧，因此无需 `_Block_copy`；Apple 构建排除该桩 | 若将来出现「异步持有 block」需求，再升级为真正的引用计数 BlocksRuntime |
| 并行 | `comp_parallel` 提供区间调度（`comp_parallel_ranges[_n]`）。图像内核的 band 调度经 `comp_bands` 接入，**线程数默认 1（顺序、与 M1 逐字节一致）**，`comp_parallel_set_band_threads(n)` 显式开启 | 逐站点评估 band 安全性后，考虑把默认值调为自动 |
| Windows 工具链 | 支持 zig / cc / clang-cl；**cl.exe 不可用**（不支持 block 语法） | 评估把内核回调改为函数指针形态，彻底摆脱 blocks |
| C++ 链接 | `imaging/comp_imaging.h` 统一为 C++ 调用方加 `extern "C"` 包裹 | 保持不变 |
| 撤销 | 整篇 `DocumentState` 快照，默认 200 步 | 视内存占用评估差分/命令式撤销 |
| 合成 | W3C 合成公式 + 蒙版/剪贴近似；**合成路径已升级为 FilterGraph 有向图**：图层树按节点递归求值，组默认**穿透**（组内子层混合模式穿透组边界、组不透明度乘进子层，对齐上游 `LayerGroups.swift`），`isolated` 组先把子树自合成到透明缓冲、再作为单元参与混合；悬空父级 / 成环 / 超深 64 一律显式报错 | 组级裁剪与组级效果、GPU 侧求值 |
| 变换 | **平移/缩放/旋转/翻转统一走逆映射重采样**（`compositing/resample`）：目标像素反解源坐标后取样，采样模式 `Nearest` / `Smooth`（双线性）/ `High quality`（Catmull-Rom，缺省），对齐上游 `LayerTransform.sampling`；恒等变换在整数对齐下**逐字节精确**；旋转按目标矩形裁剪（不做包围盒扩张） | 与 macOS 参考输出逐像素基准比对；透视/网格变形 |
| 图像格式 | `.comp` 内图层像素以 **PNG** 落盘，布局 `images/<uuid>.png` / `images/<uuid>.mask.png`；灰度蒙版用 8 位灰度 PNG（符合上游 `LayerMask.isValid`） | 更大色深 / 硬件编解码 |
| 交换格式键名 | 完整对齐上游 `ProjectManifest` / `ProjectLayerRecord`：`format`/`version`(13)/`supported`/`colorSpace`/`resolution`/`documentID`/`width`/`height`/`activeLayerID`/`layers`；图层 id 与 `parentID`/`maskSourceID` 用 UUID，`transform` 为六键形态（`origin`/`size`/`rotation`/`flipX`/`flipY`/`sampling`）。引擎自有键一律 **`engine` 前缀**（`engineLayerID`/`enginePixelFormat`/`engineClippingSourceID`/`engineAdjustments`/`engineEffects`/`engineIsolated`），避免与上游对象型键（`effects`/`text`/`shape`/`adjustment`）同名冲突 | 上游未建模字段（`guides`、live-mask 启用态等）按原样保真 |
| 编解码 | **PNG 与 JPEG 已接入真实第三方库并真实可用**：`comp_io_export`（拍平 → 落盘）与 `comp_io_import` / `comp_io_import_layer`（文件 → 文档 / 追加图层）已打通；PNG 走 libpng + zlib、JPEG 走 stb（有损，`{"quality":N}`）、PSD 走 stb（只读）。第三方源码 vendoring 到 `engine/third_party/`，`__has_include` 探测缺库时如实回落。`.comp` **内部**像素仍走自研零依赖 `comp_png`（工程包可移植的前提）。**能力边界（P2-4）**：宿主构建走 libpng+zlib，而 `scripts/build_ohos.sh` 的包含路径不含 `engine/third_party/*`，故鸿蒙交叉编译时 `__has_include("png.h")` 探测失败、PNG 会**如实回落**到自研 `comp_png` —— 本行「真实第三方库」的表述**仅对宿主构建成立**，鸿蒙侧是同语义的另一条实现。其余格式集中在 `format/codec_registry` 登记，未链接实现者如实 `UNSUPPORTED` 并给出原因 | 按 `docs/codec-migration.md` 第 6 节继续接入 TIFF / libheif / OpenEXR / lcms2；只改 `codec_lib` 与登记表 |
| 调整图层参数 | 面板控件与 JSON **都由引擎元数据驱动**：键名表 `engine/domain/adjustment_table.cpp` 是唯一真源（12 种 kind，点分 `json_path` 逐字沿用上游字段名），外壳经 ABI 取 `label` / `min` / `max` / `default`；settings JSON 由 `comp_layer_adjustment_build_settings` 组装（**壳内不拼、不解析 JSON**）；写入口 `add` / `update` / `update_at` 一律校验，`.comp` 读取路径不校验以保真 | Curves / Levels 需专用曲线编辑器（当前 `editable=false`，仅白名单放行）；删除接口 `comp_layer_remove_adjustment` 已补（面板 `Ctrl+Shift+D`） |

| 后端 | **④ 接口切片已落地**：`rhi.h` 定义 `RenderBackend` + `BackendCaps` + 工厂；`AUTO` 按平台择优，GPU 后端不可用则**显式回落 CPU** 并在 error 写明原因 | 接入真实设备路径（`COMP_HAS_D3D12` / `COMPOSITOR_ENABLE_VULKAN_COMPUTE` 已在代码中预留开关） |
| GPU 计算内核 | 着色器三份（HLSL / GLSL / C++ 镜像）**同源**于 `render/gpu_kernel.*`，`gpu_kernel_validate` + 单测锁定 24 分支与上游规范名；无 GPU 的机器上由 C++ 镜像把 GPU 语义钉在 CPU 基准上 |真机逐像素比对 |
| Vulkan 计算管线 | **设备侧全链路已实现但默认不编入**（opt-in `COMPOSITOR_ENABLE_VULKAN_COMPUTE`）：`vkCreateShaderModule`（19,052 B SPIR-V）+ 两个 `rgba8` storage image 描述符 + 推送常量（20 B）+ `vkCmdDispatch`（local 8x8x1）+ 回读。绑定号不手写，由 GLSL 抽取成常量头 | 真机验证 GPU<->CPU <= 1 LSB |
| 预乘语义 | **修正「二次乘 alpha」缺陷**：W3C 公式产出的 `out_rgb` 已是预乘值，落盘改走 `store_premultiplied`（不再乘 alpha），GLSL 镜像同步 6 处；新增公共入口 `compositor_blend_pixel_scalar` 并加逐字节回归断言 | 保持断言常绿，防止缺陷复活 |
| ABI | 只暴露 POD 与 C 符号，`comp_` 前缀，返回 `comp_result` | 结构体只在尾部追加字段，破坏性变更需升 major |

## 5. ABI 稳定性承诺

`engine/abi/include/compositor/*.h` 是 Windows 壳与 HarmonyOS 壳的**唯一**集成边界：

1. 头文件只含 C 符号与 POD；不得出现 STL 类型、类、模板、异常。
2. 所有字符串为 UTF-8 且以 `'\0'` 结尾。
3. 所有函数返回 `comp_result`（0 = `COMP_OK`）；失败时不保证输出参数被写入。
4. 结构体新增字段只能追加在尾部。
5. 枚举数值（错误码、`comp_blend_mode`、`comp_sampling`、`comp_backend`、`comp_pixel_format`）
   是 ABI 的一部分，不得重排。
6. `comp_blend_mode` 的 24 个取值与顺序**冻结为上游 `LayerBlendMode` 的声明顺序**，由
   `tests/test_compositing.cpp` 逐字比对护栏保证不漂移。
7. `comp_sampling` 的字符串取值与上游 `LayerSampling` 逐字对齐（`"Nearest"` / `"Smooth"` /
   `"High quality"`）；但**缺省值 `High quality` 刻意取整数 0**，使 `comp_layer_desc desc{}`
   零初始化即得到上游默认值，避免调用方遗漏设置时静默退化为 `Nearest`。

8. **函数也可在尾部追加**：`comp_layer_get_visible`、`comp_layer_set_isolated` /
   `comp_layer_get_isolated` 即以纯函数追加方式补入（不改任何结构体布局），
   因此不破坏冻结面。只读 getter 不开隐式事务，避免污染撤销栈。

9. 调整图层参数的**元数据、校验、JSON 构造、只读查询与删除**（`comp_layer_adjustment_kind_name` /
   `comp_layer_adjustment_param_count` / `comp_layer_adjustment_param_at` /
   `comp_layer_adjustment_validate` / `comp_layer_adjustment_count` / `comp_layer_adjustment_at` /
   `comp_layer_adjustment_read_value` / `comp_layer_adjustment_build_settings` /
   `comp_layer_update_adjustment_at` / `comp_layer_remove_adjustment`）与 IO 侧的
   `comp_io_codec_support`、状态枚举 `COMP_CODEC_VENDORED`，同属**纯函数/枚举尾部追加**：
   不改既有结构体布局，只读查询不开事务，故对 v1 冻结面零破坏。

不透明句柄的结构体 tag 名刻意与 typedef 名不同（`comp_engine_handle` / `comp_document_handle` /
`comp_txn_handle`）：在 C++ 下 `typedef struct comp_engine* comp_engine;` 会与注入普通命名空间的
类名冲突（clang 报 `typedef redefinition with different types`）。tag 名不属于 ABI，调用方只使用
typedef，故对契约零影响。

## 6. 验证记录

### 6.1 已完成的真实验证

构建环境：Windows 11 · Python 3.13.4 · ziglang 0.16.0（PyPI wheel 内置 clang/lld）。
本机原本**没有任何 C/C++ 编译器与 CMake**，故采用 `scripts/build.py` + ziglang 路径完成真实编译。

```bash
python scripts/build.py --clean
```

实测结果（M2 基线：并行 + FilterGraph + 仿射重采样 + PNG/UUID 互通落地后）：

| 项 | 实测值 |
|----|--------|
| 编译单元 | 83（38 个 `.c` + 45 个 `.cpp`；其中 vendored 第三方 26 个 `.c`） |
| 产物 | `build/compositor_tests.exe` |
| 测试 | `summary: 106 cases, 0 failed, 0 check failures`（当前基线；= 原 99 + P1-7 的 6 个 + 审核报告 §8 的 N5 的 1 个 = 106） |
| 全量构建耗时 | ≈ 60 秒（含 `.c/.cpp` 编译、链接与测试运行） |
| 源码规模 | engine：30 `.cpp` / 38 `.c`（含 vendored 26）/ 47 `.h`；tests：15 `.cpp` |

已知非致命告警：链接期 `lld-link` 报若干 `LNK4217`（`_NSConcreteStackBlock` 等 blocks 运行时符号
在镜像内本地定义、同时被按导入符号引用）。这是 clang 在 Windows 目标上对 blocks 符号的默认
`dllimport` 标注所致，符号最终解析到 `comp_blocks_rt.c` 的定义，运行结果正确。

验证覆盖：

- `test_abi.cpp`：端到端验收（新建文档 → 写像素 → 合成 → 混合模式 → 撤销/重做 → 命令调用 →
  `.comp` 存取往返 → 缩略图），全程只用 C ABI；另含组隔离 `set/get_isolated` 的 ABI 往返
  与「穿透=黑 / 隔离=绿」的合成语义钉死。
- `test_compositing.cpp`：把 24 种混合模式的清单字符串与上游枚举逐字锁定。
- `test_parallel.cpp`：区间调度「恰好覆盖一次」（6 种规模 × 5 种线程数）、退化输入不改内存、
  band 线程数默认 1，以及两个走 band 调度的内核在 threads=1 与 =4 下输出逐字节一致。
- `test_filter_graph.cpp`：建图的节点/边与 backdrop 链；穿透组的「混合穿透 + 组不透明度乘进
  子层」（0.5 × 0.5 = 0.25）；隔离组把子树作为单元参与混合（可切换组混合模式）；两级嵌套穿透；
  隐藏组跳过整棵子树；悬空父级被显式拒绝。
- `test_format.cpp`：PNG RGBA/灰度往返、外部真实 deflate PNG 解码、损坏输入被拒、UUID 文本/派生、
  manifest 上游键名护栏、`documentID` 跨保存稳定、组隔离（`engineIsolated`，缺键即穿透）的
  「存盘 → 打开 → 合成语义不变」往返、`.exr` 图层显式拒绝。
- `test_resample.cpp`：恒等变换逐字节精确（默认 High quality 不漂移）、整数倍放大象限映射、
  顺时针 90 度像素级映射、水平翻转、Smooth 与 Nearest 在非整数倍缩放下的差异、采样模式的
  `.comp` manifest 往返、非法采样值被拒。
- `test_imaging.cpp`：覆盖可确定性断言的像素内核；依赖浮点收敛的内核只做冒烟调用。
- `test_adjustment.cpp`：12 种 kind 的名称与上游枚举逐字锁定；各 kind 缺省 settings 必过校验；
  越界/类型错/未知顶层键被拒；ABI 元数据（`param_count` / `param_at` / `validate`）与
  `.comp` 往返后「kind 与 settings 逐字节回读」护栏。
- `test_codec.cpp`：能力表只陈述事实（PNG = `VENDORED`/`BUILTIN`、JPEG = `VENDORED` 且 note 含
  `stb_image`、EXR 原因含 `OpenEXR`、未知格式 `NOT_FOUND`、截断读取仍以 `'\0'` 收尾）；
  PNG 导出→导入**往返逐字节精确**；JPEG 往返**有损但一致**；`import_layer` 恰好追加一层；
  文件缺失是 IO 错误而非「不支持」；不可用格式（如 TIFF）显式 `UNSUPPORTED`；
  **P1-2 负向入库**：伪造 IHDR（`40000x40000` 与 `1000000x4`，CRC 重算）在 C ABI（libpng 路径）
  与自研 `comp_png` **直调**路径上都被拒，且 `comp_engine_last_error` 带上真实原因（原为临时探针，
  见报告 §6.4-1/§6.4-2）。
- `test_layer_mutation.cpp`（P1-7 新增）：补齐此前零单测的 5 个已交付 ABI —— `comp_layer_reorder`
  的堆叠序变化（`before` 语义 + `before=0` 置顶）与可撤销；`comp_layer_remove` 的子树级联删除
  （含 `clipping_source` 归零的落盘证据）；`comp_layer_set_mask` 的合成生效（全 0 掩码隐藏图层）
  + `maskFile`/`maskEnabled` 与 `.mask.png` 资产往返；`comp_layer_set_clipping_source` 的
  自引用/悬空来源拒绝 + `engineClippingSourceID` 往返；`comp_layer_set_effect` /
  `remove_effect` 的 `engineEffects` 往返与「重复设置即更新」。
- `test_shell_contract.cpp`：**两壳共享契约**——ABI 数值常量冻结（像素格式/后端/表面/结果码/
  编解码状态）；`create(AUTO)`→`backend()` 返回合法枚举、`create(CPU)` 原样生效；不透明源 + Normal
  叠透明底的 flatten 像素语义（±1 LSB）；HWND 与 HARMONY_WINDOW 表面**如实返回 `UNSUPPORTED`**
  且离屏 present/wait 无操作成功；编解码能力查询（PNG 可用、JPEG/PSD = `VENDORED`、TIFF =
  `UNAVAILABLE` + note 非空）；调整控件元数据（kind=3=Exposure、3 参数、首路径
  `exposureSettings.exposure`、越界显式失败）。详见 `docs/shell-parity.md`。

### 6.1.1 M3 追加验证记录

| 项 | 实测值 |
|----|--------|
| ④ 渲染后端接口切片 | `rhi.h` + `cpu_backend` / `d3d12_backend` / `vulkan_backend` / `gpu_kernel` / `test_rhi` 全部编入并回归通过 |
| SPIR-V 链路 | `gpu_kernel.comp` 5,020 B -> `gpu_kernel.comp.spv` **19,052 B**；glslang 11:14.0.0（DevEco 自带） |
| Vulkan 绑定抽取 | `set=0`：`binding 0 = g_dst`、`binding 1 = g_src`（均 `rgba8` image2D）；`local_size 8x8x1`；push constant 20 B |
| Vulkan 计算管线编译 | 默认变体 rc=0（设备侧不编入）；`-DCOMPOSITOR_ENABLE_VULKAN_COMPUTE` rc=0（设备侧全链路编入） |
| 预乘修正断言 | `compositor_blend_pixel_scalar` 逐字节断言：透明底 + 预乘源 `(64,128,0,192)` -> `(64,128,0,192)`（缺陷版为 `(48,96,0,192)`） |
| GPU 镜像一致性 | `gpu_kernel_blend_pixel` 与 CPU 标量路径在 5 模式 x 3 底色 x 3 源色 x 3 不透明度下逐字节一致 |
| 鸿蒙交叉编译 | 脚本编译的引擎编译单元共 68（自有 42：12 `.c` + 30 `.cpp`；vendored 26 `.c`）；`libcompositor_ohos.so` / `libcompositor_engine_ohos.a` 在位（**非契约项**：`.so` 体积随源码必变，不做基线断言——2026-10-15 用本机 DevEco SDK 重跑交叉编译实测通过，68 单元 0 失败，`.so` 仅导出 `RegisterCompositorOHOSModule`，符合「引擎走静态库」设计） |
| Windows 外壳 | `build/compositor_shell.exe` 链接通过（Win32 + GDI 上屏、菜单/快捷键、图层面板、撤销） |

### 6.1.2 真机验证记录（把「未验证」门槛项做成实证）

本机实测探针推翻了此前「无 GPU / 无 Vulkan SDK」的前提：`vulkaninfo --summary` 报告
`GPU0 = AMD Radeon RX 6800`（`apiVersion 1.4.315`、`driver 26.2.2`、`conformanceVersion 1.4.0.0`），
`vulkan-1.dll` / `d3d12.dll` / `dxgi.dll` 均在位。

| 项 | 实测证据 |
|----|----------|
| Vulkan 设备侧 dispatch | `tools/vulkan_real_test.cpp` 真机跑 10 种混合模式 x 2 档 alpha：**20/20 PASS**，比对 163,840 像素，**最大偏差 1 LSB、超容差通道 0**（`rc=0`） |
| 真机性能对照 | 1024x1024 Normal：GPU 21.89 ms/次（20 次均值）· CPU 15.08 ms/次 · 加速比 0.69x（GPU 每次调用重建 image/staging/描述符并全链路同步，属调用开销而非内核慢） |
| Windows 外壳实机运行 | `build/compositor_shell.exe --selftest=<报告文件路径>` **实机运行 rc=0**（注意参数是**文件路径**，报告写入该文件；GUI 壳无控制台输出，勿按目录传参），17 项自检全部 OK（引擎创建 / 文档 / 图层 / 合成 / 撤销 / `.comp` 往返），`[PASS] failures = 0`；实机 `AUTO` 回落 `backend = CPU`（Vulkan RHI 文档路径未接线且不静默成功，符合诚实策略） |
| `.comp` 双向交叉验证 | `scripts/crosscheck_comp.py` + `tools/comp_crosscheck.cpp`：与**独立**编解码器 PIL 12.3.0 双向比对 —— 引擎像素→PNG→PIL 与 PIL→PNG→引擎解码**均 0 LSB 超容差**；据此判定 `.comp` 的 PNG 为**直通（non-premultiplied）RGBA** 口径 |
| `.comp` manifest 键名 | 实测键集合 `activeLayerID colorSpace documentID engineVersion format height layers resolution version width`；`format="com.compositor.project"`、`version=13` 与上游规范一致 |

本轮真机验证还**修出一处真实缺陷**：`vulkan_backend.cpp` 的 `kImageUsageStorage` 原误写为
`0x00000020`（`DEPTH_STENCIL_ATTACHMENT_BIT`），正确值应为 `0x00000008`（`STORAGE_IMAGE`）。
错误值使 rgba8 图像缺少 STORAGE 用法，`imageStore` 被驱动静默丢弃（`vkCmd*` 无返回码、无校验层），
表现为全零输出；已修正并复验为 20/20（诊断链见 `scripts/once/_diag_vulkan_real.py`）。

### 6.1.3 调整面板与真实编解码（M5 本轮）

| 项 | 实测证据 |
|----|----------|
| 调整参数键名表 | `docs/adjustment-json-keys.md` 收录 12 种 kind 的 `json_path` / 类型 / 范围 / 默认值，逐字对齐上游 `LayerAdjustment.swift` 及其嵌套 Settings 结构 |
| 写入口校验 | `comp_layer_add_adjustment` / `comp_layer_update_adjustment` / `comp_layer_update_adjustment_at` 一律校验；越界、类型错、未知顶层键在断言中被拒（实机自检出现 `out-of-range panel value rejected`） |
| 调整删除接口 | `comp_layer_remove_adjustment` 已补：越界索引显式 `INVALID_ARGUMENT`，成功后 `adjustment_count` 归零（`tests/test_adjustment.cpp` + 实机自检 3 项） |
| 面板控件由引擎驱动 | 外壳经 `comp_layer_adjustment_param_count` / `param_at` 取控件元数据，经 `comp_layer_adjustment_build_settings` 组装 settings JSON（壳内不拼、不解析 JSON）；实机自检回读 `{"exposureSettings":{"exposure":1.5,"offset":0.25,"gamma":2.2}}` |
| 调整图层回读 | `.comp` 往返后 `comp_layer_adjustment_count == 1`，kind 与 settings 逐字节一致（`tests/test_adjustment.cpp`） |
| 外壳实机自检 | `build/compositor_shell.exe --selftest=<报告文件路径>` 实机 **rc = 0**，**27 项全 OK**（本轮新增调整面板 7 项 + 删除 3 项） |
| 编解码能力表 | `comp_io_codec_support` 两段式查询：PNG = `COMP_CODEC_VENDORED`（libpng 1.6.44 + zlib 1.3.1）、JPEG = `VENDORED`（stb，note 含 `stb_image`）、PSD = `VENDORED`（只读）；EXR 原因含 `OpenEXR`；未知格式 = `COMP_ERR_NOT_FOUND` |
| 真实导入 / 导出 | `comp_io_export` / `comp_io_import` / `comp_io_import_layer` 已打通：PNG 8x6 全不透明像素往返 **diffs = 0、worst = 0 LSB**；JPEG 往返有损但一致 |
| 独立解码器交叉 | `scripts/crosscheck_codec.py` 用 PIL 12.3.0 回读引擎产物：`round_trip.png` / `layer_source.png` / `round_trip.jpg` 全部 `[OK]`、`failures = 0` |
| 回归（M5 当时） | `summary: 83 cases, 0 failed, 0 check failures`；累计到最新为 **106**（见 §6.1.6） |

### 6.1.4 两壳一致性收敛（M5 本轮）

| 项 | 实测证据 |
|----|----------|
| 共享契约测试 | 新增 `tests/test_shell_contract.cpp`（6 用例），随引擎回归一起跑；当轮回归由 83 → 89 cases，0 failed（当前基线 **106**，见 §6.1.6） |
| 常量冻结 | 像素格式 / 后端 / 表面种类 / 结果码 / 编解码状态的数值常量在本轮一次锁死，两壳任一漂移即失败 |
| 表面诚实性 | HWND 与 HARMONY_WINDOW 两种窗口表面在断言中均返回 `COMP_ERR_UNSUPPORTED`；离屏 `present` / `wait` 无操作成功 |
| 能力矩阵文档 | `docs/shell-parity.md` 记录两壳逐项能力、冻结常量表与共享护栏，并如实标注「设备侧上屏 / 鸿蒙 UI 面板 / HAP 真机」三处环境边界 |
| 回归（两壳收敛当时） | `summary: 89 cases, 0 failed, 0 check failures`；累计到最新为 **106**（见 §6.1.6） |

### 6.1.5 审核报告 §5 修复后基线（2026-10-15，hermes 落地）

审核报告 `../report/REVIEW-compositor-ng-20261014.md` §5 的修复项按序落地后重跑（环境
`<local-workspace>\tools\python`，ziglang 0.16.0；全部为本机实跑，非看修改说明）：

| 项 | 命令 | 实测 |
|----|------|------|
| 单测 | `build/compositor_tests.exe` | `summary: 105 cases, 0 failed, 0 check failures`，rc=0（该轮基线；已被 §6.1.6 的 **106** 取代） |
| 壳自检 | `build/compositor_shell.exe --selftest=<报告文件路径>` | rc=0，27 项 OK；产物 SHA256 与仓内 `compositor_shell_selftest.txt` **完全相同**（`32151220…FAD16A`） |
| 编解码交叉 | `python scripts/crosscheck_codec.py` | `failures = 0`，rc=0 |
| `.comp` 交叉 | `python scripts/crosscheck_comp.py` | 方向A / 方向B / manifest **全 PASS**，0 超容差、0 LSB，rc=0 |
| 共享构建（P2-3） | `python scripts/build.py --shared` | 产出 `build/compositor_engine.dll`；`ctypes.WinDLL` 加载后 `comp_version()` 返回 `1.0.0`、`comp_result_name(0)` 返回 `COMP_OK`（**真实加载并调用**，不只是「文件存在」） |

本轮代码改动：P0-2 失败分支拆分 + 错误码透传；P1-1 ABI 统一异常护栏（`bad_alloc` 不再越过
`extern "C"`）；P1-2 PNG 两条解码路径的上界预检（负向用例入库）；P1-3 撤销预算信号透出
（新尾部追加 ABI `comp_document_history_truncated` + Windows 壳状态栏显示深度/内存/是否截断）；
P1-4 `comp_io_import_layer` 改显式事务 + `abort`（撤销栈不再留 `add`+`remove` 两步）；
P1-7 补 5 个零覆盖 ABI 单测；P2 文档/工程纠偏（P2-1/P2-2/P2-3/P2-4/P2-5/P2-6/P2-7/P2-9/P2-10
与报告 §6.4 的两个新缺口）。**P1-5 经复核判定为「已文档化且有测试背书的设计契约」，撤销、不改代码。**

### 6.1.6 审核报告 §8（N1–N6）修复后基线（2026-10-15，hermes 落地）

审核报告 §8 的第二轮独立复核发现按 DSH 指定顺序 **N1 → N3 → N5 → N4 → N2/N6** 落地后重跑
（同一环境 `<local-workspace>\tools\python`，ziglang 0.16.0；**从当前源码全量重编译**，不复用上一轮产物）：

| 项 | 命令 | 实测 |
|----|------|------|
| 单测 | `build/compositor_tests.exe` | `summary: 106 cases, 0 failed, 0 check failures`，rc=0（+1 = N5 的 `abi_history_truncated_flag`） |
| 壳自检 | `build/compositor_shell.exe --selftest=<报告文件路径>` | 用本轮重链（含补丁 `.o`）的壳 rc=0，27 项 OK；产物 SHA256 与仓内 `compositor_shell_selftest.txt` **完全相同**（`32151220…FAD16A`） |
| 编解码交叉 | `python scripts/crosscheck_codec.py` | `failures = 0`，rc=0 |
| `.comp` 交叉 | `python scripts/crosscheck_comp.py` | 方向A / 方向B / manifest **全 PASS**，0 超容差、0 LSB，rc=0 |
| 共享构建 | `python scripts/build.py --shared` | 产出 `build/compositor_engine.dll`；`ctypes.WinDLL` 加载后 `comp_version() = "1.0.0"`、`comp_result_name(0) = "COMP_OK"` |
| N3 功能实测 | `comp_io_probe_project`（`ctypes` 直调 DLL） | 199 B manifest → `COMP_OK` 且 `16×12 / version 13`；**4 MiB+64 → `COMP_ERR_IO`(3)**；8 MiB → `COMP_ERR_IO`(3)；无崩溃、无 `bad_alloc` 外泄 |

本轮代码改动：N1 `comp_layer_remove_adjustment` 补 `COMP_ABI_TRY/CATCH`（全仓 19 处 `ImplicitTxn`
里唯一漏护栏的画布级入口）；N3 `probe_project` 补 4 MiB manifest 上界（与 load/save 一致）
+ `comp_io_probe_project` 加护栏；N5 补 `comp_document_history_truncated` 回归用例；
N4 在 `comp_io.h` **仅文档化** `comp_io_import_layer` 的前置条件（不得有活动事务），**不改代码**；
N2/N6 为报告与注释订正。**P1-5 维持撤销，零代码改动。**

> N6 如实记账：`comp_blocks_rt.c` 的 `_Block_object_assign` 硬闸在当前构建中**不可达**
> （仓内零 `_Block_copy` 调用点，`_Block_copy` 为恒等实现），「全绿」**不能**证明该闸有效；
> 该改动只作「把前提写成显式断言」的文档价值，**不记为风险已收口**（呼应 §6.2）。

> **【修订记录 7 订正（R1/R2）】** ① 上方 `:269` 行基线算式原写「原 89 + 6 + 1」（89+6+1=96，与 106 不符），
> 已订正为「原 **99** + 6 + 1 = 106」——审核报告实测的审核起点即 **99** 用例。② `engine/core/comp_blocks_rt.c`
> 的 P2-8 注释已按 N6 结论改写（`_Block_copy` 为恒等实现 ⇒ 该硬闸在当前构建中**不可达**，**不构成对逃逸的
> 防护**、**不得记账为「风险已收口」**）；该措辞的收紧实际在本次订正落地，不在第五轮提交内。

### 6.1.7 资产化撤销 + 图层合并基线（2026-10-15，独立复核后续轮）

按 `report/AUDIT-compositor-ng-20261015-agan.md` 的建议推进：先修漂移与低危遗留，再对齐上游
`robbietilton/Compositor`。**全部步骤从当前源码全量重编译**（`<local-workspace>\tools\python`，ziglang 0.16.0）：

| 项 | 命令 | 实测 |
|----|------|------|
| 单测 | `build/compositor_tests.exe` | `summary: 135 cases, 0 failed, 0 check failures`，rc=0（原 107 + 合并/拼合/COW/字节预算 12 + 第二轮审计护栏 10 + 架构风险收口 6） |
| 壳自检 | `build/compositor_shell.exe --selftest=<报告文件路径>` | rc=0，27 项 OK；SHA256 与仓内基线**逐字节相同**（壳层新增菜单项不改自检行为） |
| 编解码交叉 | `python scripts/crosscheck_codec.py` | `failures = 0`，rc=0 |
| `.comp` 交叉 | `python scripts/crosscheck_comp.py` | 全 PASS，rc=0 |
| 共享构建 | `python scripts/build.py --shared` | `ctypes` 真实调用 `comp_version() = "1.0.0"` |

> ⚠️ **构建解释器**：`scripts/` 依赖 PyPI 的 `ziglang`（自带 clang），
> 编解码交叉验证还需要 `numpy` + `PIL`。请用**已装齐这三者的解释器**运行，例如：
>
> ```bat
> <local-workspace>\tools\python\python.exe scripts\run_regression.py
> ```
>
> 用一个缺依赖的解释器（如裸系统 Python）跑，回归会 5 步全 FAIL 且报
> `No module named ziglang` —— 那是环境问题，不是代码问题。
| 鸿蒙交叉编译 | `bash scripts/build_ohos.sh` + `build_ohos_shell.sh` | 68 单元 0 失败；`.so` 仅导出 `RegisterCompositorOHOSModule` |
| 一键回归 | `python scripts/run_regression.py` | **6/6 PASS，rc=0**（上述步骤固化为单入口，杜绝「挑着验」） |

本轮代码改动：

1. **撤销栈像素资产化（P1-3 根治）**：`ImageBuffer/MaskBuffer` 的像素改 `shared_ptr<vector>`
   持有——文档快照、历史记录、Layer 复制只增引用不复制像素；写入走 `writable_pixels()/writable_row()`,
   引用计数 > 1 时先复制（COW）。`History` 改上游同款「独占字节」账本（按指针去重，活文档资产不计），
   内存上限语义不变（先最旧撤销、后最旧重做、始终保留最近一条）。1280×720 每步约 7 MB 的
   整篇拷贝消除，改为差异写入时才复制。
2. **图层合并 / 拼合（对齐上游 LayerMerge.swift 的 ⌘E 家族）**：
   `comp_layer_merge_down`（向下合并 / 组合并，混合、不透明度、蒙版、剪贴、变换按画布呈现烘入，
   结果裁剪到内容、落在锚点槽位、一步撤销）与 `comp_document_flatten`（拼合图像）；
   配套 `comp_layer_can_merge_down`（外壳置灰）、`comp_layer_set_active`（选择态，不入历史）、
   命令表新增 `layer.merge_down`（Ctrl+E）与 `layer.flatten`，Windows 壳菜单/快捷键已接线。
   **渲染不变性**由 12 个新用例钉住（合并前后整篇合成逐字节一致）。
3. **漂移与遗留修复**：README 去掉 `.so` 精确字节数（非契约项，随源码必变）、写明
   `--selftest=` 参数是**报告文件路径**；`comp_command_abi.cpp` 补齐异常护栏（全仓 `COMP_ABI_TRY`
   计 41 处：command 3 + engine 4 + io 4 + layer 22 + render 5 + undo 3）。

### 6.2 尚未验证的部分（证据边界）

- **CMake 路径未实测**：本机无 CMake，`CMakeLists.txt` 与 `build.py` 已按同一源集合/标准/开关对齐，
  但未跑通 `cmake + ctest`。
- **MSVC / clang-cl 路径未实测**：本机无 VS 工具集。
- **像素内核的逐像素等价性未验证**：并行一致性只证明了「并行不改变自身结果」，
  与 macOS 参考输出的逐像素比对属 M2 的图像回归基准。
- **band 并行默认关闭**：其它内核站点是否可安全切分尚未逐一评估，故默认线程数保持 1。
- **仿射重采样仅以引擎内单测为准**：恒等/整数倍/90 度等情形已用固定像素断言锁定，
  但尚未与 macOS 参考输出逐像素比对；非 90 度旋转的插值质量与边界行为属后续图像回归基准。
- **`.comp` 已做独立编解码器交叉验证，但仍无 macOS 端实测**：键名、取值、PNG 像素与 UUID 图层
  id 已按上游规范落盘并加护栏（`test_format.cpp`），本轮又用 **PIL 独立编解码器**完成双向
  0 LSB 交叉验证（见 6.1.2）；但本机无 macOS，**尚无真实 macOS 端打开验证**。
- **组语义仅以引擎内单测为准**：穿透/隔离两种组语义已用固定像素断言锁定，但尚未与 macOS 版的
  渲染结果做逐像素比对；组级裁剪与组级效果不在本轮范围。

- **Vulkan 设备侧已真机验证**（见 6.1.2）：`vulkan_compute_blend` 在 RX 6800 上 20/20 且
  GPU<->CPU <= 1 LSB，`vkCreateShaderModule` / `vkCreateComputePipelines` 真机返回码有效。
  但**引擎主渲染路径尚未接线到设备侧**，故 `caps().offscreen_compute` 仍**如实为 false**，
  `render_document` 明确返回 UNSUPPORTED，`AUTO` 回落 CPU（实机自检亦确认 `backend = CPU`）。
- **鸿蒙壳仍无真机可验**：`libcompositor_ohos.so` 已构建，DevEco 工具链（`hvigor` / `node` /
  `hdc`）在位，但 `hdc list targets` 实测返回 `[Empty]` —— **无鸿蒙真机/模拟器在线**，HAP 打包
  与 ArkTS 页面未经设备联调；且当前 `.so` 为 `aarch64`，模拟器平台多为 `x86_64`，需另加交叉目标。
- **D3D12 真实设备路径不可编译**：本机 `d3d12.dll` / `d3d12core.dll` / `dxgi.dll` 与 GPU 在位，
  但 **Windows SDK 仅有 8.1 与 NETFXSDK，无 `d3d12.h`**，故 `COMP_HAS_D3D12` 保持关闭，
  默认构建为诚实 stub。
- **SPIR-V 仅静态校验**：真机 `vkCreateShaderModule` 已实际接受该 blob（不报错），但未接校验层
  做更严格的运行期校验；静态证据为 glslang 编译 + 真机管线创建成功。
- **第三方编解码库：部分已接入，其余仍未链接**。PNG（libpng 1.6.44 + zlib 1.3.1）、
  JPEG（stb，有损）、PSD（stb，只读）的真实源码已 vendoring 到 `engine/third_party/` 并真实可用；
  TIFF / HEIF / WebP / EXR 在本机**无任何可链接依赖**（无 cmake / make / gcc / clang /
  pkg-config / vcpkg，全盘常见根目录下未找到 `jpeglib.h` / `lcms2.h` / `ImfHeader.h`），
  故仍如实 `UNSUPPORTED` 并写明原因；后续接入步骤见 `docs/codec-migration.md` 第 6 节。
  Python 侧 PIL 只作交叉验证参照，不可链接进 C++ 引擎。
- **调整面板的 GUI 交互未经人手实测**：面板所走的 ABI 链路已由实机自检逐项覆盖（取控件元数据 /
  组装 JSON / 新增 / 回读 / 按索引更新 / 越界拒绝），但「用鼠标键盘点按面板」这一交互路径
  仍需在有桌面会话的机器上人工确认。
- **调整图层删除接口已补**：`comp_layer_remove_adjustment` 已以纯函数尾部追加方式加入
  （面板 `Ctrl+Shift+D`），越界索引显式拒绝，回归与实机自检均通过。

### 6.3 显式能力边界（保真但暂不参与合成）

以下上游字段在 `.comp` 读写时**原样保真**，但内核暂不参与合成计算，避免跨端往返丢信息：

- `guides`（画布参考线）、live-mask 的启用状态；
- 文本 / 形状 / 图层效果 / 调整图层的上游语义参数（引擎侧以 `engineAdjustments` / `engineEffects`
  承载自有参数，上游对象型键不覆盖）；
- `maskSourceID` 仅原样保真，不参与合成。

**图层合并的显式边界（2026-10-15，随合并能力落地）**：

- 被合并图层**携带调整/效果**时，合并返回 `COMP_ERR_UNSUPPORTED`——调整/效果的像素求值属
  滤镜图里程碑（见 `compositing/compositor.h` 声明），在求值落地前明确拒绝，不悄悄丢参数、
  不假装烘入；
- 被合并图层**上方的穿透组不透明度非 100%** 时，合并返回 `COMP_ERR_STATE`——穿透组的不透明度
  按「每个子层」缩放（`filter_graph.cpp` 的 `child_scale`），两层并一层后无等价单层表示；
  隔离组不受此限（其不透明度在子树合成后整体应用）。二者叠加时「渲染不变」不成立，
  因此拒绝而非近似。

## 7. 许可

上游 `robbietilton/Compositor` 为 MIT 许可；本仓沿用 MIT，并保留上游版权声明。
`engine/imaging/` 下的像素内核移植自上游 C 源码，文件头保留原始注释。
