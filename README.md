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
  全量 **233 用例 0 失败**；Windows 外壳实机自检 **27 项全 OK**（与仓内基线逐字节一致）。
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
  2026-10-16 第四轮（工具算子产品通路）：7 组原「孤立内核」接入新 ABI
  `comp_tool_apply`（Wand/Heal/Patch/Dither/Lens/Brush/Deep），逐算子参数契约见
  `docs/tool-ops.md`；基线 184 → **204 用例**，6/6 回归全绿。
  详见 `../report/AUDIT7-compositor-ng-20261016-dsh.md`。
  2026-10-17 第五轮（16 位通路收口）：`comp_layer_set_pixels` 接受 `COMP_PIXEL_RGBA16`
  之后，把「16 位资产的 8 位工作副本」补成真实现——`domain/layer.h` 的 `deep_to_rgba8`
  是唯一生产者（对齐上游 `DeepLayer.dithered`），合成 / 缩略图、裁剪基底取 alpha、
  8 位像素工具三处消费点全部改走它；此前这三处会按 4 字节/像素读 8 字节/像素的缓冲，
  不报错但画面是垃圾。同时把 `COMP_PIXEL_RGBA16` 的 ABI 口径从「非预乘」改正为
  「预乘」（与上游布局一致；该格式此前在任何入口都被拒绝，无宿主可依赖旧措辞）。
  基线 204 → **207 用例**，6/6 回归全绿。
  详见 `../report/AUDIT8-compositor-ng-20261017-grok.md`。
  2026-10-17 第六轮（内核收口 + 完成度对标）：修掉 4 处真实缺陷——**剪贴栈 alpha 语义**
  （旧实现「先合成基底再按基底 alpha 缩放」，软 alpha 基底 + MULTIPLY 子层得到 `a=0`，
  改为上游 `LiveMaskRenderer.drawComposite` 的 extract → 不透明基底上混合 → restore，实测
  `a=128` 与上游一致）、**`ImageBuffer::fill` 未预乘**（直通色写进预乘缓冲，半透明实色得到
  「比 alpha 还亮」的值）、**`run_tool_op` 成功时不回写 `out_code`**（调用方传初值当标志位会
  把成功读成失败）、**`engine/imaging/*.h` 全部缺 `extern "C"`**（这正是「未接线核」的根因：
  C++ 侧 include 后符号被 mangle，只能各自重写一份 C++ 版）；并接线 `wand_trace`、Scanlines、
  Chunky、deep round trip、16 位通道混合调整、`extract/restore_alpha`。
  基线 207 → **212 用例**，6/6 回归全绿。
  同轮产出**双口径完成度对标**：引擎核内 **≈92%**、作为「Photoshop 替代品」的产品口径 **≈45%**
  （详见 §6.4 与 `../report/AUDIT9-compositor-ng-20261017-pi.md`）。

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
└── tests/                         # 零依赖测试框架 + 16 组测试（test_main 为入口）
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
| 编译单元 | 83（38 个 `.c` + 45 个 `.cpp`；其中 vendored 第三方 26 个 `.c`）→ **72**（38 个 `.c` + 34 个 `.cpp`；其中 vendored 第三方 26 个 `.c`） |
| 产物 | `build/compositor_tests.exe` |
| 测试 | `summary: 233 cases, 0 failed, 0 check failures`（当前基线；§6.1.13 起在此基础上继续累加） |
| 全量构建耗时 | ≈ 60–120 秒（含 `.c/.cpp` 编译、链接与测试运行） |
| 源码规模 | engine：34 `.cpp` / 38 `.c`（含 vendored 26）/ 72 `.h`；tests：21 `.cpp` |

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
   与 `COMP_ABI_CATCH` 各 50 处、逐文件配平：command 3 + engine 5 + io 4 + layer 30 + render 5 + undo 3）。
   > 护栏总数是**快照而非契约**，会随新增异常入口变动；可复核的契约是
   > 「TRY 与 CATCH 一一配平、每个外部 ABI 函数都有一对」——用
   > `grep -c COMP_ABI_TRY engine/abi/src/comp_*.cpp` 现场统计即可，不要把数字写进契约。

### 6.1.8 工具算子产品通路基线（2026-10-16，DSH 接手核实 AUDIT7）

Pi 第五轮留下的 Step 5（Tool ops：把 7 组孤立内核接出 ABI 入口）在被 API 403/525
打断时**代码已落地但未上报**。DSH 接手后逐文件核实并跑通全量回归：

- **实测**：`run_regression.py --require-ohos` → **6/6 PASS**，单测 `204 cases, 0 failed,
  0 check failures`（AUDIT6 的 184 + Pi 本组 16 + DSH 补 4），OHOS aarch64 交叉编译
  **72 单元 fail=0** rc=0，Windows 外壳 27 项实机自检 SHA256 与仓内基线一致。
- **环境辨析（重要）**：先前记录的「进程非零退出」**不是测试失败**——是 `python` 解析到
  Studio 自带 venv（未装 `ziglang`），使步骤 1/2/3/5 全部报环境错。改用
  `<local-workspace>\tools\python\python.exe` 后 6/6 全绿、退出码 0。**跑回归必须用该解释器**。
- **接线核实（查调用点，不是读文档）**：`tool_ops.cpp` 实际调用 `wand_mask`、`spot_heal`
  （内部派发 `patch_fill` + `heal_membrane`）、`dither_apply`、`lens_distort`、
  `dither_quantize16`；`run_tool_op` 的参数闸全部位于 `txn_begin` **之前**，与上游
  `EditorSession.swift:516`「先 guard 再 beginEdit」同序。
- **本轮发现并修复的三个问题**：
  ① ABI 头 `comp_tool_dither.levels` 注释写 2..8，而域层闸与 `docs/tool-ops.md` 都是
  2..16（唯一真源是域层闸）——已改正注释；
  ② ABI 头把 `deep round trip -> COMP_ERR_UNSUPPORTED` 列为可达错误码，但
  `comp_tool_op_desc` 刻意不承载 `deep_mode`，该码在 ABI 边界**不可达**——已改成如实
  说明，避免宿主照不存在的码写重试逻辑；
  ③ **测试名与断言不符**：`tests/test_tool_ops.cpp` 的
  `tool_ops_deep_round_trip_is_reported_unsupported` 名字承诺冻结 round trip 的拒绝，
  但 ABI 无法表达该模式，实际只测到「DEEP 打在 RGBA8 上失败」，且断言写成 `!= COMP_OK`
  ——任何内部错误都能骗过它。已改名为
  `tool_ops_deep_quantize_on_rgba8_is_refused_with_state` 并把错误码钉死为
  `COMP_ERR_STATE`；至于 round trip 那条**ABI 不可达**的契约，另开
  `tests/test_tool_ops_domain.cpp`（+4 用例）直接调域层 `run_tool_op` 验证：
  `COMP_ERR_UNSUPPORTED`、不留悬空事务、不进撤销栈、像素逐字节不变；并补
  `pressure * flow == 0` 笔画逐字节不变与「被拒后紧接一次合法调用仍成功」。

### 6.1.9 16 位通路收口（2026-10-17，Grok）

AUDIT7 §6 的第一条缺口是「Deep 降位路径**当前不可达**：`set_pixels` 仍拒绝
`COMP_PIXEL_RGBA16`」。复核工作树后发现真实情况比这条记录更糟一档：`set_pixels` 的
格式闸**已经被放开**（接受 RGBA16），但合成侧、裁剪基底取 alpha、8 位像素工具三处仍按
「4 字节/像素」解释像素缓冲，`tests/test_abi.cpp` 里那条「16 位必须被拒」的用例因此变成
红色（**204 用例 1 失败**，与 AUDIT7 记载的 204/0 不符）——也就是说：门开了，房间还没修。

本轮把它做成真通路，口径以上游 `DeepLayers.swift` 为准（：「A layer whose `ImportedImage`
carries `deep` keeps its pixels at 16 bits a channel; its `image` is a dithered 8-bit copy
that the canvas, thumbnails and every tool keep using unchanged」）：

- **唯一生产者**：`domain/layer.h` 的 `deep_to_rgba8`（`dither_quantize16` 加约一阶噪声
  后取整；噪声按 (x,y,通道) 固定，同一输入恒得同一输出）。原先 `tool_ops.cpp` 内自带一份
  同名实现，已合并——两份实现一旦漂移，同一张 16 位图层「合成出来」与「quantize 出来」
  就会不同。
- **三处消费点改走它**：`compositing/resample.cpp` 的 `build_layer_unit`（合成与缩略图
  的唯一光栅化入口）、`compositing/composite_ops.cpp` 的 `clipping_factor`（按实际字节宽
  取 alpha，取整口径与内核一致）、`domain/tool_ops.cpp`（8 位工具先把 16 位图层落到那份
  副本，图层自此是 8 位——上游 :5 同款语义）。
- **口径纠正**：`COMP_PIXEL_RGBA16` 的 ABI 注释从「非预乘」改为「**预乘**、native-endian」。
  上游布局是预乘，`dither_quantize16` 也按预乘取值（颜色夹到 alpha 以内）。改动安全：
  该格式在 HEAD 的任何入口都被拒绝，没有宿主可能依赖旧措辞。
- **实测**：`run_regression.py --require-ohos` **6/6 PASS**，单测 **207 cases, 0 failed,
  0 check failures**，OHOS aarch64 交叉编译 **72 单元 fail=0** rc=0。
- **负向对照（确认护栏不是空的）**：① 把 `build_layer_unit` 的 8 位副本改回直读 →
  新用例 18 条断言失败；② 只把 `clipping_factor` 改回写死 4 字节 → 恰好 1 条
  （剪贴子层与 8 位副本逐字节相同那条）失败。两次对照后均已还原。
- **仍未落地**：引擎自己的解码器还不能**产出** 16 位资产（无 16 位 PNG / EXR 导入，
  `.comp` 存盘也只写 RGBA8），所以「16 位资产从文件进来」这条路仍缺；本轮打通的是
  **ABI 供给 16 位像素 -> 合成 -> quantize 落盘**这一条，并且可实测。

### 6.1.10 内核收口 + 双口径完成度对标（2026-10-17，Pi）

本轮两件事：把 AUDIT8 §6 列的可达缺口接线完毕，并对标上游做一个可复核的完成度评估。

**修复的 4 个真实缺陷**（均为接线过程中暴露的既有缺陷，非本轮新增）：

- **剪贴栈 alpha 语义错误（最严重）**：旧实现是「先合成基底、再按基底 alpha 逐像素缩放」，
  半透明基底 + MULTIPLY 子层两次 source-over 把 alpha 叠没了，得到 `a=0`——非 Normal 混合
  的语义完全不对。改为上游 `LiveMaskRenderer.drawComposite` 的口径：**extract alpha →
  把颜色变不透明 → 在不透明基底上按混合模式混 → 按原 alpha restore**，实测 `a=128`
  且 RGB `0,0,0`，与上游一致。`FilterGraph` 新增 `kClippingStack` 节点（此前图里根本没有
  剪贴栈节点，只靠 `blend_layer_into` 里的 `clipping_factor` 兼）。
- **`ImageBuffer::fill` 未预乘**：直通色直接写进预乘缓冲，半透明实色得到 `rgb=(255,0,0)`
  `alpha=128` 这种「比 alpha 还亮」的值。已按 `(c*a+127)/255` 预乘；`MaskBuffer::fill` 同核。
- **`run_tool_op` 成功时不回写 `out_code`**：调用方传 `COMP_ERR_INTERNAL` 初值当标志位，
  成功路径上该值不被覆盖，把成功读成失败。已在每个出口写明并修复。
- **`engine/imaging/*.h` 10 个头全部缺 `extern "C"`**：**这就是「未接线核」的根因**——
  这些是上游 C 内核，C++ 侧 include 后符号被 mangle，链接不上，只能各自重写一份 C++ 版。
  补完后真正的 C 内核可直接调用。`LevelsPixels.h` 还顺带补了缺失的 include guard。

**接线的内核**：`wand_trace` → `SelectionOutline`（只读闭合轮廓，不写像素/蒙版）；
Scanlines / Chunky 新入口；deep round trip（RGBA16 保持 16 位，low/+1/+2/+4 四档，
ABI 携带内层 8 位算子）；16 位通道混合调整（与 8 位共用 `build_hsv_cube`，对齐 `DeepLayer.lookup`）；
`extract_alpha` / `restore_alpha`（剪贴栈）；`blur_rgba8_copy` 抽出复用（扫描线 glow）。

**实测**：`run_regression.py --require-ohos` **6/6 PASS**，单测 **212 cases, 0 failed,
0 check failures**，OHOS aarch64 交叉编译 **72 单元 fail=0** rc=0，Windows 壳 27 项
SHA256 `32151220f4748dfa…` 与基线一致。

**完成度对标结论（两个口径，见 §6.4）**：引擎核内 **≈92%**；若分母放大到上游 README 的
48 条**产品功能条目**（而非引擎核能力），ng 命中约 20 条，产品口径 **≈45%**。
像素算法层几乎满配（10 组 `.c` 内核中 6 组与上游逐字节等同，另 4 组差异仅注释），
缺的是**功能面**而非接线——Camera Raw / 选区几何 / Type·Shape·Gradient / Clone·Smudge·Liquify /
Crop 工具 / 5 种格式导入，grep 全仓零命中，属未开工。详见 `../report/AUDIT9-compositor-ng-20261017-pi.md`。

### 6.1.11 引擎核收尾：颗粒 / 直方图 / 修补范围（2026-10-17，Pi）

AUDIT9 列的「未接线内核」逐核实后发现：`dither_dots` / `dither_glow` 已由
chunky / scanlines 分支接线，`deep_split` / `deep_merge` 已由 deep round trip 接线、
`patch_fill` / `heal_membrane` 已由 `spot_heal` 内部派发——**剩下真正无产品通路的只有四个**，
本轮全部接上：

- **`noise_add_at`（颗粒）** -> `comp_tool_brush` 新增 `grain_amount` / `grain_gaussian` /
  `grain_monochromatic`。为什么用 `noise_add_at` 而不是 `noise_add`：内核噪声只取决于
  **像素位置 + seed**，而笔画以文档坐标给出、以图层原点平移后落笔——用 `noise_add` 则同一笔
  在图层移动后会得到不同颗粒（噪声跟着图层原点跑）。颗粒叠在**落笔之后、整图层范围**
  （不是只在笔迹上）。
- **`levels_histogram`（直方图）** -> `comp_layer_histogram` 新 ABI。关键口径：
  **预乘像素先反预乘再取亮度**（否则半透明区域的颜色被 alpha 压低，面板曲线系统性偏向暗部，
  与实际 levels 调整对不上），coverage 权重按 alpha 走。16 位图层显式 `COMP_ERR_UNSUPPORTED`
  ——桶是 8 位的，不偷偷降位。
- **`heal_coverage_bounds`（修补范围）** -> `comp_tool_coverage_bounds` 新 ABI。
  `spot_heal` 内部算的是同一份包围盒，暴露出来免得宿主自己再扫一遍、然后与内核漂移。
  **全 0 coverage 报四 0 而不是 [-1,-1,-1,-1]**（宿主会拿它去算宽度）。该函数是纯计算、
  不触引擎状态，故不取引擎句柄、不写 `last_error`。

**基线 212 -> 219 用例**（6 条新护栏）。`run_regression.py --require-ohos` **6/6 PASS**；
OHOS aarch64 72 单元 fail=0；Windows 壳 27 项 SHA256 与基线一致。

**负向对照（证护栏不是空的）**：三条新护栏逐一做了反向注入，每次恰好击穿并已还原：
① 直方图去掉反预乘 -> `histogram_unpremultiplies_before_taking_luma` 红；
② 颗粒 origin 改成 `0,0`（即 `noise_add` 口径）-> `grain_is_reproducible_and_origin_anchored` 红；
③ 空 coverage 报 [-1,-1,-1,-1] -> `coverage_bounds_matches_nonzero_pixels` 红。

> 注：② 一开始报不红的原因是**测试本身写错了**（断言「两条图层内容相同、原点不同应得同一颗粒」）。
> 正确口径是**同 seed + 同起始像素 + 同文档原点** -> 逐字节相同，而颗粒是**叠加**的
> （再跑一遍会再加一层，这正是画笔该有的行为，不是幂等）。

### 6.1.12 深度语义核实：32 位浮点不成立、HDR 被解码器阻塞（2026-10-17，Pi）

AUDIT9 把 `deep_split` 记作「需先定 32 位浮点深度语义」、`hdr_*` 记作「需线性 HDR 决策」，
并据此认为它们占引擎核剩余缺口的大半。本轮逐条对上游核实，**其中两条的前提不成立**，
已从 §6.4 口径 A 的缺口列表中撤下（口径本身未变，仍为 ≈92%）。

**① `deep_split` 的 32 位浮点深度语义 —— 伪命题，无需决策。**
上游全仓 35 处图层存储上下文分配，32 处 `bitsPerComponent: 8`、1 处 `: 16`（`DeepLayers.swift:14`），
**零处 32 位浮点**。唯一的 `CIFormat.RGBAf` 在 `DeepLayers.swift:251`，且只作 Core Image 的
`workingFormat`（中间态，`PixelAdjust.render` 的落点仍是 `.RGBA16`）。即上游深度模型只有
**三层**：8 位层 / 16 位层 / HDR（半浮点 extended sRGB）层，**没有第四层**。
而 `deep_split` 的口径本就写死在 `DeepPixels.h` 里，且与上游 `.c` 逐字节等同
（`diff` 仅差 GCD 一处、见 §6.1.11 注），也已由 deep round trip 接线（`tool_ops.cpp:270`）。
**结论：没有需要拍板的东西。**

**② `deep_take_painted` —— 语义明确，但被仿制笔刷阻塞，非语义问题。**
上游触发面只有两处：`DeepLayers.swift:71`（`flattened`）与 `BrushStroke.swift:1120`
（`sixteenBit`）。语义是「16 位层被笔刷/仿制笔刷涂抹后，哪里的 8 位像素与 8 位基底不同，
哪里就是被画过，取该值」。ng 现状：`grep -rni "clone|smudge|liquify" engine/` **零命中**，
且非 deep 工具全走 `use_rgba8_working_copy` 直接降 8 位（`tool_ops.cpp:713`）。
**结论：要等 S4 仿制笔刷的 tile + coverage 模型，届时上游口径无歧义。**

**③ `hdr_*` 线性 HDR —— 语义已可定，但被解码器硬阻塞。**
上游 HDR 存在的唯一理由是 EXR / Radiance HDR 往返：`ImageImporter.swift:97` 只对
`.openEXR` / `.radianceHDR` 开浮点路径，`DeepLayers.swift:171-174` 用 `deep_half_peak` 取峰值、
**`peak > 1.001` 才算 HDR 层**（"a file brushing white through rounding isn't HDR"），
存储在 **extended sRGB 半浮点、预乘**（`hdrSpace`，`DeepLayers.swift:85`）。口径确定，但 ng 的 `codec_lib_can_decode`
只有 png / jpeg / psd（`codec_lib.cpp:470`），**没有任何浮点解码器**，HDR 层无法从文件产生。
另需注意：两个 PNG 解码器都**截掉 16 位**——`codec_lib.cpp:260` 调 `png_set_strip_16`，
`comp_png.cpp:342` 的 `sample8` 对 depth 16 直接取高字节，故 **16 位 PNG 也会掉到 8 位**。
**结论：这是「有没有解码器」的前置问题，不是「线性还是非线性」的语义问题**；
HDR 六个内核的接法在上游有唯一解，但入口不可达时接上去即死代码。

### 6.1.13 C 方案 C0 口径冻结：HDR 六内核护栏（2026-10-17，Pi）

ALEX 选 **C**（完整 HDR：EXR/Radiance/float-TIFF 解码器 + 6 内核 + 全部调整走浮点 + EXR 导出）。
推进前先做 C0（口径冻结）。**触发 C0 的不是「需要写文档」，而是一个核对结果**：

```
grep -rn "deep_hdr" tests/   →  零命中
```

6 个 `hdr_*` 内核在 `DeepPixels.c:257-363` 全部有实现、且与上游逐字节等同，
但**一行测试都没有执行过**——口径没对着测试冻结，等于没冻结。故 C0 交付
`tests/test_hdr_semantics.cpp` **14 条行为护栏**，未改动任何引擎源码。

完整口径文档：`report/C0-hdr-contract-20261017-pi.md`。四条最易踩的：

1. **normalize 的输出是 RGBA16，不是 half float**（`DeepLayers.swift:101` 的目标 `context(...)` 未传 `hdr:`；
   `:115` 的 restore 输入取自 `canonical(operation(...))`，同样 16 位）。只有两端是半精度。
   写反会让 16 位布局被当浮点解释，读出 0 或 65535 倍偏差。
2. **`inverseGamma` 是 `1/gamma`，而 `gamma` 默认 1.0**（`ImageAdjustments.swift:45`），
   故上游正常路径传进来就是 **1.0** 而非 `1/2.2`；内核不校验，传错照样出结果。
3. **Exposure 的缩放发生在线性域**，编码回来的数值不等于「原值除以 2 的幂」：
   8 的两档下压在线性域是 /4，编码回来是 **4.46 而不是 2**。
4. **`scale = brightest` 使 `color[c]/scale ≤ 1` 由构造保证**，源码里那个 `fminf(1, ...)`
   本就不可达——别去「修」它。

判定闸冻结为 `peak > 1.001`（`DeepLayers.swift:174` "a file brushing white through rounding
isn't HDR"），峰值取**反预乘后**值，`alpha==0` 跳过、`< INFINITY` 才参与。

**14 条护栏全部做了反向注入**（改坏内核 → 确认对应护栏变红 → 还原，`diff` 无差异）：
峰值去反预乘（1 挂）、峰值不排除 inf（1 挂）、normalize 的 `scale` 恒为 1（**4 cases / 11 checks 挂**）、
exposure 在白点裁切（2 cases / 6 checks 挂）、extract 去掉 `alpha>0` 守卫（1 case / 6 checks 挂）、
take_painted 无条件覆盖（1 挂）。

**护栏写错的记录**（同 S1 颗粒护栏）：先试「去掉 normalize 里的 `fminf`」做注入，结果 0 挂——
因为该分支不可达（见上第 4 条）。换成「`scale` 恒为 1」才有响应。**护栏初报不红，先怀疑护栏本身。**

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
- 文本 / 形状的上游语义参数（引擎侧承载自有参数，上游对象型键不覆盖）；
- `maskSourceID` 仅原样保真，不参与合成。

**图层效果已参与合成（2026-10-16 起，见 `compositing/effect_eval.cpp`）**：
`comp_layer_set_effect` 写入的 6 个 kind（描边 / 投影 / 颜色叠加 / 内阴影 / 外发光 / 内发光）
全部有像素实现，在图层的单元缓冲上就地求值，绘制次序对齐上游
`LayerEffects.swift:388 render(_:mask:effects:)`：

- **绘制次序不可交换**：投影 / 外发光 / outside 描边垫在图层像素**之下**；
  图层像素；颜色叠加 / 内发光 / 内阴影 / inside 描边压在像素**之上**。
  叠色类（Color Overlay / 内发光 / 内阴影）走 **source-atop**——只落在已有 alpha 之内
  且**不改变 alpha**，半透明像素因此完全吃到颜色（这正是上游注释强调的语义）；
- 效果以图层「呈现出来的形状」（变换与蒙版已烘进单元缓冲）为源，因此效果跟随实际形状；
- 包围盒按 `effects_margin()` 外扩（outside 描边吃 size、投影吃 distance + blur×3、
  外发光吃 size×3），否则 outside 类效果会被图层 bounds 裁掉；
- 效果求值在**混合之前**，因此效果不吃图层的混合模式与不透明度——与上游「先做效果、
  再当普通图层混合」一致；
- 参数缺键回落**上游同名字段的默认值**（如 `StrokeEffect.size=4/opacity=1/color=black`）；
  不可见参数（opacity=0 / size=0 / 距离与模糊同时为 0）逐字节无变化；坏 JSON 与键类型不对
  回落默认值而非跳过——注意上游默认值全都是**可见**的，早期把「回落默认值」当成
  「当作无效果跳过」会让实现与上游语义分道扬镳；
- **引擎不认识的 effect kind**（`kind >= 6`，如未来版本或第三方扩展）既不求值也不绘制：
  逐字节跳过，但**仍然阻止 flatten / merge_down**——烘入会静默丢弃它，正是 AUDIT5 §7.4
  记的那类「报成功但丢参数」；
- 已知差异：上游用 Core Image 的高斯模糊与 GPU 路径，本引擎复用调整求值里同一套三趟
  滑动窗口盒式近似（同一参数在两条路径上得到同一结果），语义等价但**未与上游逐像素比对**
  （证据边界见 §6.2）；上游在图层**像素网格**上算效果再整体贴到画布，本引擎在文档坐标上算，
  免去 inset 与 transform 的往返换算。

**调整图层已参与合成（2026-10-16 起，见 `compositing/adjustment_eval.cpp`）**：
`comp_layer_add_adjustment` 写入的 12 个 kind 全部有像素实现，并在合成路径的滤镜节点
位置求值——对齐上游 `EditorCanvas.swift:3092`「调整作用于其下方已合成结果」的语义：

- 调整作用于 **backdrop（下层合成结果）**，不影响其上方图层；
- coverage = **自身蒙版 × 祖先组蒙版链（最多 64 层）× 图层不透明度**，逐像素相乘，
  对齐上游 `EditorCanvas.swift:3114-3120` 的 `folders: true` 分支；部分覆盖处按 W3C 源-over
  混合回原 backdrop（满覆盖保持逐字节写回）；
- **非 normal 混合模式**已按上游 `EditorCanvas.swift:3096-3106` 处理：满覆盖时先按该模式把
  调整结果与下层混一次，再按 below 的**原始 alpha** 取回原 coverage（因此软边缘不会被加厚）；
- opacity < 1 是「半强度调整」，不是「只对部分像素调整」；
- 隐藏调整图层 ⇒ 渲染结果逐字节回到无调整状态；
- 参数缺键回落默认（恒等），坏 JSON / 未知 kind 跳过并计入 skipped，不让一条坏参数毁掉整篇合成。

**图层合并的显式边界（2026-10-15，随合并能力落地；2026-10-16 更新）**：

- **携带图层效果（effects）的图层现在允许合并**（2026-10-16）：效果求值落地后整篇合成已包含
  效果，烘入合成结果即为正确行为，且合并是**渲染不变**的（合并前后逐字节一致，
  由 `merge_down_bakes_adjustments_and_effects` 钉住）；
- **携带调整（adjustments）的图层允许合并**（同上）；
- 仍拒绝的两种情形，都**不改文档状态**：
  - 被合并图层携带**引擎不认识的 effect kind** ⇒ `COMP_ERR_UNSUPPORTED`（烘入会静默丢弃）；
  - 被合并图层**上方的穿透组不透明度非 100%** ⇒ `COMP_ERR_STATE`——穿透组的不透明度按
    「每个子层」缩放（`filter_graph.cpp` 的 `child_scale`），两层并一层后无等价单层表示；
    隔离组不受此限（其不透明度在子树合成后整体应用）。二者叠加时「渲染不变」不成立，
    因此拒绝而非近似。

### 6.4 整体完成度评估（2026-10-17，以「与上游 Compositor 同等正常使用」为 100%）

**先说清楚一件事：两个口径，两个数字，不能混用。**

- **口径 A：分母 = 引擎核能力**（引擎 + ABI + 两壳）。回答「引擎核做完了吗」→ **≈92%**。
- **口径 B：分母 = 上游产品功能条目**（上游 README 的 48 条）。回答「能当 Photoshop 用吗」
  → **≈45%**（命中约 20 条）。

AUDIT8 记的 ≈90% 是**口径 A**，天然不含上游 macOS 应用层（`Compositor/UI` 9.3k 行 +
`ContentView`），因此「90% vs 45%」不矛盾。

#### 口径 A：引擎核

把「同等正常使用」拆成**四层**，逐层给权重与实测覆盖率。
引擎核（`engine/` + ABI + 两壳）已完成，**UI/编辑器层整体不在本仓范围**。

| 层 | 权重 | 覆盖率 | AUDIT8 | 变动依据 |
|----|------|--------|--------|---------|
| ① 引擎核契约面（ABI / 领域模型 / 事务 / 历史 / 诊断 / 边界） | 30% | **~95%** | ~95% | 89 个导出 ABI 全有异常护栏；句柄 magic、反序生命周期、线程局部错误槽、定制分配器、字节预算、transform 写入侧闸均已落地并有护栏；`run_tool_op` 的 `out_code` 契约已补 |
| ② 像素内核**有产品通路**的部分 | 35% | **~91%** | ~86% | AUDIT8 列的可达缺口清零：`wand_trace`、`scanlines`/`chunky`、deep round trip、`cube_apply`(16bit)、`extract/restore_alpha` 已接（见 §6.1.10）。余 `deep_take_painted`（等 S4 仿制笔刷 tile 模型）与 `hdr_*`（无浮点解码器，见 §6.1.12）；`dither_dots`/`dither_glow`/`deep_split`/`deep_merge` 经核实**已接线**，不再计入缺口 |
| ③ 合成与渲染正确性 | 20% | **~96%** | ~95% | **剪贴栈 alpha 语义对齐上游**（`a=0` → `a=128`）、`ImageBuffer::fill` 预乘修正、hidden 层优先级修正、剪贴栈成员 backdrop 补齐 |
| ④ 跨端与交付（两壳 / 编解码 / 互通 / 回归） | 15% | **~80%** | ~80% | Windows 壳 27 项实机自检基线、OHOS aarch64 交叉编译 + NAPI、PNG/JPEG/PSD 真实编解码、`.comp` 与 PIL 双向 0 LSB 交叉验证；扣分项：无真机验证、无 CMake/MSVC 实测 |

**加权完成度 ≈ 92%**（口径 A）。算术：① 30%×95% + ② 35%×91% + ③ 20%×96%
+ ④ 15%×80% = 28.5 + 31.85 + 19.2 + 12 = **91.55% ≈ 92%**。

**剩下的 8% 里大半不是技术债，而是「需要语义决定」或「被上游工具面阻塞」**——
这两项在 §6.1.12 重新核实过：`deep_split` 的 32 位浮点语义**不成立**（上游深度模型只有
8/16/HDR 三层，无 32 位浮点图层），`deep_take_painted` 被仿制笔刷阻塞。真正待定的只剩 `hdr_*`。

#### 口径 B：产品功能面

| 上游类（README 48 条 / 7 大类） | ng 命中 |
|---------|---------|
| Layers（图层/组/不透明度/全混合模式/图层蒙版/剪贴蒙版/调整层 12 kind/图层效果 6 kind/合并/重排嵌套） | ✓ |
| Transform（仿射 move·scale·rotate / distort / 精确值） | ✓ |
| Painting（笔刷 / Spot Heal / Content-Aware Fill） | ✓（笔刷为解析式，噪声未采样） |
| Canvas（Canvas Size / Image Size / Trim / 旋转） | ✓ |
| **Camera Raw**（6 文件 1,708 行） | **零** |
| **选区几何**（Marquee/Lasso/Pen/Select Subject/抠图，1,000+ 行） | **零**（ng 只有 `wand_mask` 选颜色 + `wand_trace` 轮廓查询） |
| **Type / Shape / Gradient**（TypeTool 472 行 + ShapeTool + Gradient） | **零** |
| **Clone Stamp / Smudge / Liquify**（400+ 行） | **零** |
| **Crop 工具**（`Crop.swift` 249 行） | **零**（只有 canvas resize，非裁剪工具） |
| **Vignette / Bloom / Remove Background**（`Filters.swift` 1,007 行） | **零** |
| **格式导入** HEIC/TIFF/WebP/SVG/RAW/PSB | 枚举有 7 种，但 HEIF/WebP、SVG、PSB 无解码器，TIFF=UNAVAILABLE |

即 **20/48 命中 ≈ 45%**。缺的约 5,000+ 行 Swift **完全没有对应物**，且这些**不属于「接线」
范畴，属新增功能**——不是把孤立内核接出去能解决的。

#### 口径 A 与 B 不一致的原因与推进顺序

两者差异集中在 ①~④ 盖不到的地方：口径 A 问的是「引擎核的契约面与像素保真度做完了吗」，
答案是基本做完（≈92%）；口径 B 问的是「用户能做的事做完了吗」，答案是还差一半。
**像素算法层几乎满配**：上游 10 组 `.c` 内核逐文件 diff，**6 组 diff 行数为 0**
（Brush/Heal/Lens/Levels/Noise/Wand 逐字节等同），另 4 组差异仅注释。

**功能面推进顺序建议**（边际成本从低到高）：
选区几何（1,000+ 行，但它是几乎所有下游编辑的入口）→ Crop 工具（249 行，解锁导出打印尺寸）
→ Clone Stamp + Smudge/Liquify（已有 `PatchPixels`/`HealPixels` 基础设施可复用）
→ Camera Raw（1,708 行 + RAW 解码，投入最大，但是上游招牌功能）
→ 格式导入（需第三方库，与「零依赖自研」取向冲突，**需先定策略**）。

### 6.5 工具算子（Tool ops）已接入产品通路（2026-10-16 起）

`comp_tool_apply` 是 7 组原「孤立内核」的统一产品入口，参数契约见
[`docs/tool-ops.md`](docs/tool-ops.md)。要点：

- **坐标空间显式转换**：ABI 收**文档像素坐标**（种子、笔迹折点），内核要**图层像素坐标**，
  引擎用 `effective_bounds()` 的原点做平移，与合成路径同一套口径；种子落在图层外一律
  `COMP_ERR_INVALID_ARGUMENT`，不做静默夹取。
- **一步撤销 + 闸在事务之前**：非法参数不留半成品、不进撤销栈（`comp_document_can_undo`
  不变），全部参数校验先于 `txn_begin`。
- **不改几何**：工具算子不写 `transform`/`blend`/`opacity`，因此 `merge_down`/`flatten`
  的逐字节不变性契约不受影响。
- **可重复性**：`seed` 固定 + 笔画「不叠加」（同像素被同一笔压两次仍是同一结果），
  使「同参数两次调用逐字节相同」成立，并有「撤销后重做 = 逐字节相等」的护栏。
- **空选中不写蒙版**：魔棒选中数为 0 时不留下全 0 蒙版（否则整层被蒙成透明），
  对齐上游「无选中即不改变」；写入既有蒙版是逐像素**取最大值**（加选）。
- **note 两段式**：`note_capacity = 0` 时只回报所需长度（含 `\0`）；终止符落在
  `note[min(capacity-1, size)]`，**永不写出调用方缓冲**（护栏含首字节与 capacity 后一字节断言）。
- **明示边界而非静默回落**：字形图集不由 ABI 灌入，`GLYPHS` 回落菱形网点**并在 note 里
  如实回报**；deep round trip **§6.1.10 起已实现**（RGBA16 保持 16 位，low/+1/+2/+4 四档），
  在 8 位图层上则显式拒绝（`COMP_ERR_STATE`）而不是伪造一组像素。

### 6.6 内核接线现状（2026-10-17 更新）

| 内核 | 产品通路 | 已接线 | 仍未接线 |
|------|---------|--------|----------|
| Wand | `COMP_TOOL_SELECT` | `wand_mask`、`wand_trace`（只读闭合轮廓） | — |
| Heal | `COMP_TOOL_HEAL` | `spot_heal`、`heal_membrane`（内部派发） | `heal_coverage_bounds` |
| Patch | `COMP_TOOL_HEAL` | `patch_fill` | — |
| Dither | `COMP_TOOL_DITHER` | `dither_apply`、`scanlines`、`chunky`（后两者分别接 `dither_glow` 与 `dither_dots`） | — |
| Lens | `COMP_TOOL_LENS` | `lens_distort` | — |
| Brush | `COMP_TOOL_STROKE` | 解析式圆头笔 + **`noise_add_at` 颗粒**（噪声钉在文档坐标；`seed` 参与计算） | `brush_alpha_bounds` 一族（剪贴栈已改走 `extract/restore_alpha`） |
| Noise | `COMP_TOOL_STROKE`（经 brush 字段） | `noise_add_at`（origin 锚定） | — |
| Levels | `comp_layer_histogram`（只读查询） | `levels_histogram`（**预乘先反预乘再取亮度**） | — |
| Heal | `COMP_TOOL_HEAL` | `spot_heal`、`heal_membrane`（内部派发）、**`heal_coverage_bounds`**（`comp_tool_coverage_bounds` 只读查询） | — |
| Deep | `COMP_TOOL_DEEP` | `dither_quantize16`、**deep round trip**（内含 `deep_split`/`deep_merge`）、**16 位 `cube_apply`**、`extract_alpha`/`restore_alpha` | `deep_take_painted`（语义已定，等 S4 仿制笔刷的 tile+coverage 模型才能接线）、`hdr_*` 六个（**C 方案已拍板，口径已于 §6.1.13 冻结并配 14 条护栏**；接线属 C2，前置是 C1 的 RGBA16F 像素路径） |
| HDR | （无 —— 入口不可达） | — | `hdr_normalize`/`hdr_restore`/`hdr_exposure`/`extract_alpha_hdr`/`restore_alpha_hdr`/`take_painted_hdr` 六件套**实现齐备且有 14 条护栏**，但零产品通路：`COMP_PIXEL_RGBA16F` 枚举存在而入口拒绝（`comp_layer_abi.cpp:162`），且无浮点解码器（见 §6.1.12、C0） |

## 7. 许可

上游 `robbietilton/Compositor` 为 MIT 许可；本仓沿用 MIT，并保留上游版权声明。
`engine/imaging/` 下的像素内核移植自上游 C 源码，文件头保留原始注释。
