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
  全量 **373 用例 0 失败**；Windows 外壳实机自检 **41 项全 OK**（与仓内基线逐字节一致）。
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
  2026-10-18 第七轮（F1 选区几何）：新增 `comp_selection_*` 一族 10 个 ABI，把上游
  `Document/Selection.swift` 的产品通路补上——矩形/椭圆/多边形/三次贝塞尔路径栅格化
  （非零缠绕，反向子路径成洞）、replace/add/subtract、全选/取消/反选、feather/expand/contract。
  选区存**文档分辨率 8 位蒙版 + 羽化参数**（不是路径），并并入 `DocumentState`，
  于是事务快照、撤销/重做与回滚自动覆盖；写操作一步撤销，**状态没变就不进历史**；
  参数闸全部先于隐式事务。基线 292 → **304 用例**（12 例护栏，含 2 轮反向注入），
  6/6 回归全绿。详见 §6.7。
  2026-10-18 第八轮（F2 Crop）：新增 `comp_crop_*` 5 个 ABI，把上游 `Crop.swift` 的
  裁剪工具补成产品通路——拖拽生成框（比例约束 / Option 对称）、保留宽度的比例切换、
  四边取整、可提交性判定，以及**提交时平移图层 + 换画布**（不重采样、不裁像素，
  画布外内容保留；框不变则不进历史）。选区随坐标系变更丢弃，整次裁剪一步撤销。
  顺带修正 Canvas Size：尺寸真的变化时丢弃选区（此前只有 Image Size 丢）。
  基线 304 → **316 用例**（12 例护栏），6/6 回归全绿。详见 §6.8。
  2026-10-18 第九轮（F3 修饰类工具）：`comp_tool_apply` 追加 `COMP_TOOL_CLONE`(9) /
  `COMP_TOOL_SMUDGE`(10) / `COMP_TOOL_LIQUIFY`(11)——仿制图章按「一笔开始时的快照」取样、
  `aligned` 跨笔保留首笔偏移、`sample_all_layers` 取整篇合成；涂抹用跨 dab 的 carried
  缓冲拖色；液化（Push）用 dab 前 scratch 做前向位移重采样。参数闸仍在事务之前，
  一步撤销，`out_code` 成功也写 `COMP_OK`。基线 316 → **326 用例**（10 例护栏），
  6/6 回归全绿。详见 §6.9。
  2026-10-18 第十轮（F4 Camera Raw）：新增 `comp_engine_load_camera_raw_tables` /
  `comp_engine_camera_raw_tables_loaded` / `comp_camera_raw_apply` 3 个 ABI，把上游
  `CameraRaw.swift` 的设置、曲线与管线搬成产品通路——763 张 17³ 实测表（上游 CRT3
  离线转 CRT4/zlib，1.78 MB）引擎级共享，上游 Codable 同名字段、严格 JSON
  （未知键/越界拒绝，结构修复照旧），校准 → 曝光 → 白平衡 → 对比度 → 黑白场 →
  饱和/自然饱和 → 参数曲线 → folded 合成表 → 逐像素曲线 → 效果/颗粒 → 光学 → 细节，
  恒等设置零副作用，整次应用一步撤销。基线 326 → **341 用例**（15 例护栏），
  6/6 回归全绿。详见 §6.10。
  2026-10-18 第十一轮（引擎核收尾）：把最后一对未接内核 `deep_take_painted` /
  `deep_take_painted_hdr` 接进落笔族（Stroke / Clone / Smudge / Liquify）——16 位与 HDR
  图层落笔后**保留深像素**，只有写成过的像素回写（对齐上游 `BrushStroke.swift:1097` 的
  `sixteenBit()`），其余工具照旧按 `DeepLayers.swift:5` 永久降 8 位。同轮补上两条从未
  实测的构建路径：`cmake + ninja + ctest`（344/344）与 clang-cl / MSVC ABI（113 个 TU，
  344/344），并修掉四处「CMake 与 build.py 不等价」的分叉与 blocks 运行时在静态库下的
  `__imp_*` 链接问题；订正「本机无 `d3d12.h`」的旧记录。基线 341 → **344 用例**
  （3 例护栏），6/6 回归全绿。口径 A 由 ≈94% 收敛到 **≈97%**。详见 §6.1.21。
  2026-10-18 第十二轮（P1 Filter 菜单 + Blur 工具）：新增 `comp_filter_apply`
  （`COMP_FILTER_VIGNETTE` / `TONAL_CONTRAST` / `BLOOM` / `LENS_CORRECTION`）——把上游
  `Filters.swift` 的四条滤镜补成产品通路：参数契约、与图层几何的装配、一步撤销、
  按**文档坐标**选区混合、16 位工作副本与 HDR 拒绝；Bloom 因 `CIBloom` 不公开而采用
  显式亮部近似并在 note 里写明。同轮补上 `COMP_TOOL_BLUR = 12`：整层模糊快照在整笔开始前
  取好，按笔刷 coverage 盖回，笔尖之外保留（含 16 位）像素。参数契约见
  [`docs/filters.md`](docs/filters.md) 与 [`docs/tool-ops.md`](docs/tool-ops.md) §4.9。
  基线 352 → **368 用例**（16 例护栏：11 滤镜 + 5 模糊），口径 B ≈48% → **≈52%**。
  详见 §6.12。
  2026-10-18 第十三轮（P1 UI 接线：Filter 菜单 / 面板 + Blur 工具）：把上一轮的
  `comp_filter_apply` 接到 Windows 壳——「滤镜(&F)」菜单按 `filter_table` 动态生成
  4 个条目，右侧面板切换为参数面板（Tab 选行、`-`/`=` 调值、数字键入、回车应用、
  Esc 关闭），值与行序由 `build_filter_desc` 一处映射进 `comp_filter_desc`；
  恒等设置如实提示「未改动」，越界值在面板侧就被夹住。Blur 工具（`R` 循环
  Blur/Smudge/Liquify）的 Blur 模式接上画布折线 → `COMP_TOOL_BLUR`，
  `Alt+[` / `Alt+]` 调半径（避开 `[`/`]` 笔刷直径与 `Ctrl+[`/`]` 不透明度）；
  失焦 / 捕获丢失一律取消未完成的笔迹。平台无关部分仍在 `ui_core.h`，
  新增 5 例护栏（滤镜表量程/缺省、面板夹取与焦点、行命中几何、retouch 循环与
  半径量程、笔迹去抖）。基线 368 → **373 用例**，Windows 壳自检 27 → **41 项**，
  6/6 回归全绿。详见 §6.13。

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
| 图像格式 | `.comp` 内图层像素按位深落盘：RGBA8 → `images/<uuid>.png`，RGBA16/RGBA16F → `images/<uuid>.exr`（线性光、直通 alpha；16 位用 FLOAT 通道保低位，HDR 用 HALF 通道逐位精确）；蒙版用 8 位灰度 `images/<uuid>.mask.png`（符合上游 `LayerMask.isValid`） | 更大色深 / 硬件编解码 |
| 交换格式键名 | 完整对齐上游 `ProjectManifest` / `ProjectLayerRecord`：`format`/`version`(13)/`supported`/`colorSpace`/`resolution`/`documentID`/`width`/`height`/`activeLayerID`/`layers`；图层 id 与 `parentID`/`maskSourceID` 用 UUID，`transform` 为六键形态（`origin`/`size`/`rotation`/`flipX`/`flipY`/`sampling`）。引擎自有键一律 **`engine` 前缀**（`engineLayerID`/`enginePixelFormat`/`engineClippingSourceID`/`engineAdjustments`/`engineEffects`/`engineIsolated`），避免与上游对象型键（`effects`/`text`/`shape`/`adjustment`）同名冲突 | 上游未建模字段（`guides`、live-mask 启用态等）按原样保真 |
| 编解码 | **PNG 与 JPEG 已接入真实第三方库并真实可用**：`comp_io_export`（拍平 → 落盘）与 `comp_io_import` / `comp_io_import_layer`（文件 → 文档 / 追加图层）已打通；PNG 走 libpng + zlib、JPEG 走 stb（有损，`{"quality":N}`）、PSD 走 stb（只读）。**HDR 来源（C3，§6.1.17）**：Radiance `.hdr`/`.pic` 走 stb 的**浮点通道**解出线性光，按 1.001 闸判成 HDR(RGBA16F) 或 16 位图层；浮点格式**结构上不许**走 8 位通道。**EXR 已由树内自研 scanline 读取器接入**（C3b，§6.1.18：NONE/RLE/ZIPS/ZIP 可读，PIZ/PXR24/B44/DWA 与 tiled/deep/multipart 如实拒绝，写侧仍 `UNSUPPORTED`）；32 位 float TIFF **仍无解码器**（树内 stb 无 TIFF）。第三方源码 vendoring 到 `engine/third_party/`，`__has_include` 探测缺库时如实回落。`.comp` **内部**像素仍走自研零依赖 `comp_png`（工程包可移植的前提）。**能力边界（P2-4）**：宿主构建走 libpng+zlib，而 `scripts/build_ohos.sh` 的包含路径不含 `engine/third_party/*`，故鸿蒙交叉编译时 `__has_include("png.h")` 探测失败、PNG 会**如实回落**到自研 `comp_png` —— 本行「真实第三方库」的表述**仅对宿主构建成立**，鸿蒙侧是同语义的另一条实现。其余格式集中在 `format/codec_registry` 登记，未链接实现者如实 `UNSUPPORTED` 并给出原因 | 按 `docs/codec-migration.md` 第 6 节继续接入 TIFF / libheif / OpenEXR / lcms2；只改 `codec_lib` 与登记表 |
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
| 编译单元 | 82（38 个 `.c` + 44 个 `.cpp`；其中 vendored 第三方 26 个 `.c`），按 `build/engine_*.o` 实测 |
| 产物 | `build/compositor_tests.exe` |
| 测试 | `summary: 352 cases, 0 failed, 0 check failures`（当前基线；§6.11 起） |
| 全量构建耗时 | ≈ 60–120 秒（含 `.c/.cpp` 编译、链接与测试运行） |
| 源码规模 | engine：44 `.cpp` / 38 `.c`（含 vendored 26）/ 82 `.h`；tests：32 `.cpp` |

blocks 运行时符号的 Windows 细节（§6.1.21 收口）：clang 在 Windows 目标上把
`_NSConcreteStackBlock` / `_Block_object_assign` 等声明成 `__declspec(dllimport)`，引用落在
`__imp_*` 槽位上。散装 `.o` 链接时 lld 会自动回退到本地定义（只给 `LNK4217` 警告）；
但**静态库**参与链接时 lld 拒绝用库成员满足导入引用（`... is available in libcompositor_engine.a
but cannot be used because it is not an import library`）。`comp_blocks_rt.c` 现已按 COFF 导入
槽位约定补齐 `__imp_*` 数据符号（`_WIN32` 守卫），两条链接路径都零警告、零未定义符号。

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
  「存盘 → 打开 → 合成语义不变」往返；EXR 资产与 manifest 声明不一致时显式拒绝（声明 RGBA8 却给
  `.exr`、声明 16 位却没有资产），合法 EXR 往返见 `test_exr.cpp`。
- `test_resample.cpp`：恒等变换逐字节精确（默认 High quality 不漂移）、整数倍放大象限映射、
  顺时针 90 度像素级映射、水平翻转、Smooth 与 Nearest 在非整数倍缩放下的差异、采样模式的
  `.comp` manifest 往返、非法采样值被拒。
- `test_imaging.cpp`：覆盖可确定性断言的像素内核；依赖浮点收敛的内核只做冒烟调用。
- `test_adjustment.cpp`：12 种 kind 的名称与上游枚举逐字锁定；各 kind 缺省 settings 必过校验；
  越界/类型错/未知顶层键被拒；ABI 元数据（`param_count` / `param_at` / `validate`）与
  `.comp` 往返后「kind 与 settings 逐字节回读」护栏。
- `test_codec.cpp`：能力表只陈述事实（PNG = `VENDORED`/`BUILTIN`、JPEG = `VENDORED` 且 note 含
  `stb_image`、EXR = `BUILTIN` 且 note 写明 scanline 与 `export writes … HALF`、未知格式 `NOT_FOUND`、
  截断读取仍以 `'\0'` 收尾）；另有 `test_exr.cpp` 覆盖自研 EXR 读写器（见 §6.1.18）、
  `test_c4_exr_export.cpp` 覆盖合成结果导出（见 §6.1.20）；
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
| 编解码能力表 | `comp_io_codec_support` 两段式查询：PNG = `COMP_CODEC_VENDORED`（libpng 1.6.44 + zlib 1.3.1）、JPEG = `VENDORED`（stb，note 含 `stb_image`）、PSD = `VENDORED`（只读）；EXR = `BUILTIN`（树内 scanline 读写器，note 写明可写 HALF 与支持的压缩）；未知格式 = `COMP_ERR_NOT_FOUND` |
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

> 本节写于 C3 之前，其中「**没有任何浮点解码器**」这条**已由 C3 取代**（Radiance 已接入，
> 见 §6.1.17）；其余结论（三层深度模型、`deep_split` 无待决项、两个 PNG 解码器都截 16 位）仍然成立。

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

### 6.1.14 C 方案 C1：RGBA16F 像素路径打通（2026-10-17，Pi）

C1 是 C 方案的第一块地基：`COMP_PIXEL_RGBA16F` 枚举早就在
`comp_geometry.h:127`（8 字节/像素），但 ABI 入口一直显式拒绝
（`comp_layer_abi.cpp:162`），域层与合成侧也没有半精度的像素路径。C1 把它变成真通路。

**新增两个转换内核**（`engine/imaging/DeepPixels.c`，以 C-linkage ABI 暴露，不复制位操作）：

| 内核 | 方向 | 上游对应 |
|------|------|----------|
| `deep_half_to_u16` | half RGBA → 16 位 RGBA（按**值**换算） | `DeepLayer.image(_:)` / `canonical`（`DeepLayers.swift:24/31`） |
| `deep_u16_to_half` | 16 位 RGBA → half RGBA | 写入侧同款 |

口径已冻结成 6 组护栏（`tests/test_hdr_semantics.cpp` C1-1~C1-6），四条要点：

1. **饱和而非回绕。** half 上限 65504，若直接 `(uint16_t)(v*65535)` 会被截成低 16 位，
   65504×65535 的低 16 位是随机乱值——画面出现彩色噪点。白点以上的光**饱和到 65535**
   （"anything brighter than white shows as white"，`DeepLayers.swift:175`）。
2. **NaN→0，+Inf→65535，两条刻意不同。** `!(value > 0.0f)` 把负数、-Inf、NaN 一起落 0
   （负光不是有效颜色，16 位 context 也存不下）；+Inf 语义上就是「白」故落上限。
   把 NaN 映成白会在画面上打出刺眼亮斑（一处坏像素变成一个白点），映成 0 最多是它透明。
3. **预乘保持**：分量与 alpha 各自独立换算，**不做反预乘**。
4. **子正规数落 0**（`0x0001`），往返容差按 half 精度（约 2⁻¹¹）判定，远小于 1/255。

**8 位视图走 `hdr_to_rgba8`（`domain/layer.cpp`）**，两趟不可交换：
先 `deep_half_to_u16` 得到 16 位布局，再 `dither_quantize16` 加约一阶噪声取整成 8 位——
就是上游 `dithered(image(pixels))` 的原序。若把半精度直接喂给 `dither_quantize16`，
`0x3C00`（1.0）会读成 15360 而非 255，是白点的 23 倍。
`resample.cpp:171` 按 `format == COMP_PIXEL_RGBA16F` 分流，两个中间缓冲都先落本地
（src 与 dst 允许是同一个对象，避免边读边写）。

**两件**没做**的事，写明**：

- `rgba8_to_hdr`（8 位工作副本回写 HDR 图层）已实现但**零产品通路**，且**不回填高光**：
  上游的 8 位工具用完后图层整体降为 8 位（`DeepLayers.swift:297` "they're dithered down"），
  要保留高光必须走 `hdr_normalize`/`hdr_restore` 那对内核——那是 C2（已在 §6.1.16 落地为
  「HDR 图层上的 deep round trip」；本句在 C1 时写下，故仍按当时的待办表述保留）。
- **`.comp` 解码侧的 RGBA16F 拒绝保留**（`comp_file.cpp:617`）：PNG 路径只产 RGBA8，
  HDR 图层无法从文件产生，与 C1 的内存路径互相独立。错误消息已改写为指向 C3 解码器。
  换句话说，**HDR 图层现在能被 API 写进去，但存盘/读盘仍会明确拒绝**。

**护栏反向注入**：注入 1（`deep_half_to_u16` 去掉饱和）→ 3 cases / 14 checks 挂；
注入 2（`resample.cpp` 去掉格式分流，退回 16 位整型路径）→ 2 cases / 5 checks 挂；
注入 2b（`deep_to_rgba8` 放宽接受 RGBA16F）**初次未红** → 见下。`grep -rn INJECT` 零残留。

**护栏不够、已补的一条**：注入 2b 单独做时 240 用例全绿——因为 `resample.cpp` 的分流还在，
没人真的走到那条错路。原来的两个 ABI 用例都**经由合成路径**，钉不住 `deep_to_rgba8`
这个**入口本身**。故补 `tests/test_tool_ops_domain.cpp` 的
`deep_and_hdr_eight_bit_views_keep_their_own_formats`：钉死两个 8 位视图的格式闸互不越界
（16 位视图拒绝 half 源、half 视图拒绝 16 位源），外加空缓冲拒绝且不写 dst。
补上后注入 2b 独立被抓住。**这是本轮第二个「护栏初报不红，先怀疑护栏本身」**（第一个见 §6.1.13）。

基线 **233 → 241**（0 failed），`run_regression.py --require-ohos` **6/6 PASS**，
OHOS 72 fail=0，Windows 壳 27 项 SHA256 与基线一致。

### 6.1.15 上游函数对齐：AdjustPixels 缺失的 4 个 Camera Raw 函数（2026-10-17，Pi）

ALEX 拍板「1. 对齐上游函数」。先把 10 组内核做了一次**函数名集合差**扫描（不是靠 diff 行数
目测），确认缺口**只有 AdjustPixels 一组**：

| 内核 | 上游函数数 | ng 函数数 | 上游有 ng 无 |
|------|-----------|----------|------------|
| Brush / Heal / Lens / Levels / Noise / Wand | 4/6/1/3/4/7 | 同 | **零** |
| PatchPixels / DitherPixels / DeepPixels | 19 / 17 / 23 | 19 / 17 / 27 | **零**（Deep 多 4 个是 C1 主动新增的 half↔u16，ng 是超集） |
| **AdjustPixels** | **62** | **58** | **4** |

补进来的四个（上游 v1.5.2 / v1.5.3 新增，ng 一个都没有）：

| 上游函数 | 作用 | 为什么之前缺它也没崩 |
|----------|------|--------------------|
| `camera_raw_lookup_is_valid` | Metal lookup 的**受支持表域闸**：`grid==33` 且每个分量有限且在 0..1 | ng 无 GPU lookup 通路，域闸无人问 |
| `camera_raw_repair_lookup` | GPU 舍入边界像素回落到原双精度 `composed_lookup` | 同上 |
| `box_mean_plane`（static） | **窗口在边缘收缩**的盒式均值 | 与已有的 `box_blur_plane` 名字相近、职责相近，极易误认为重复 |
| `adjust_camera_raw_shadows` | 按画面**分区自适应**的 Shadows（log2 亮度域 + 引导滤波） | Camera Raw 无产品通路（见口径 B） |

移植后 `AdjustPixels.c` 与上游 `origin/main 365b574` 的 diff **只剩两处**：三处
`dispatch_apply` → 顺序 `for`（M1 去 GCD，与同文件既有的两处写法逐字一致），以及
include 与 `in_bands` 的桥接。头文件除 `// AI生成` 与 `extern "C"` 包裹外与上游一致。

**`box_mean_plane` 与 `box_blur_plane` 不是重复，是两个不同的算子**（README 之前没写清，
本轮踩过一次）：前者边缘**收缩窗口**、分母用实际可用计数；后者边缘**重复取值**、分母恒为 `2r+1`。
Shadow 的区域层正是靠前者算的，换成后者会让图像边缘变暗。

护栏交付 `tests/test_camera_raw_align.cpp` 7 个用例，注入验证：

- 注 1 去 `grid!=33` → 1 case / 3 checks 挂 ✓
- 注 2 去 `uncertain` 闸（无脑重算全部像素）→ 1 case / 1 check 挂 ✓
- 注 3 去 Shadows 的 alpha 守卫生 → 1 case / 1 check 挂 ✓
- 注 6 放松 `kCount<1` 守卫 → 1 case / 1 check 挂 ✓
- **注 5（x 域右端夹取）初报不红** → 见下
- **注 4（`box_mean_plane` 换成 `box_blur_plane` 语义）初报不红** → 见下

**两次「护栏初报不红」，根因各不同，都已定位并补栏**（本轮第三个、第四次同类事件，
前两次见 §6.1.13 / §6.1.14）：

1. **注 5 是真缺陷，不是等价变异。** 上游写 `int x0c = (int)xp < xCount - 1 ? (int)xp : xCount - 2;`
   是因为三个平面要在 x 上**双线性插值**，列号必须夹到 `xCount-2` 才读得到右邻。
   我先写了脚本穷举 `xp ∈ [0, xCount-1]`，确认注入版 `int x0c = (int)xp` 只在
   `xp` **恰好落在最右列**（`xt` 恰为 0）时才越界——共 38 种可达情形，`xt=0` 时读到的
   是**下一个 K 行的第 0 列**（末行时直接读出整个 `coefficients` 缓冲区）。
   原护栏用 `xLow=-6.0f` 让 `xp` 落在域内而非末列，压根碰不到。
   补 `camera_raw_shadows_clamps_the_x_domain_inside_its_row`：用 `xLow=-7.5f` 强制
   `xp` 落到最右列，且把**下一个 K 行的第 0 列**设成 `9.0`（lift=9 → gain=512 → 白场），
   正误两条路给出「不变」与「全白」两种互斥结果。补后注入被抓住。
2. **注 4 是真差异，但我前两版探针都是瞎的。** 换 `box_blur_plane` 语义后
   `adjust_camera_raw_shadows` 输出**逐字节不变**，连查三步：
   ① 线性 ramp——两种窗口的均值都等于 ramp 本身；② 阶跃边缘 + `radius=1`——饱和成同一串；
   ③ 把 `box_mean_plane` 单独拷出来对比——**差异极大**（`step row0: -4.000 -4.000 -2.400`
   vs `fixed row0: -1.440 -1.920 -1.440`）。
   根因是探针**只把 lift 平面设成非零**：`lift` 不乘 `detail`，所以输出与被注入的平滑结果无关。
   改成只给 **gain-above** 平面非零（`detail = level - region` 才生效）后立刻分道扬镳——
   正确实现 `212 208 205 204 203 202 201 200`（过边后逐点衰减），固定窗长版
   `200 200 200 ...`（一步跳到平台）。补
   `camera_raw_shadows_detail_gain_exposes_the_smoothing_window` 钉住「过边后 >200 且严格递减」。
   教训比结论重要：**护栏的输入要选在「被注入的量真正参与输出」的那条支路上**，
   否则注入的是一条死路，测一百遍也不红。

护栏与注入记录见 `report/A1-upstream-align-20261017-pi.md`。基线 **241 → 249**（0 failed），
`run_regression.py --require-ohos` **6/6 PASS**，OHOS 72 fail=0，Windows 壳 27 项 SHA256 一致。

### 6.1.16 C 方案 C2：HDR 图层走 normalize/restore 的深度往返（2026-10-17，Claude 接手）

Pi 连续三次因 provider 侧额度错误（`403 no active subscription`）无法继续，由 Claude 接替 C2。
接手的第一个动作不是补实现，而是**核实调用点**——结论把 C2 的范围缩小了一半：

- `comp_*` 导出面里**没有任何「把一组调整烘到图层上」的入口**；工具侧唯一入口是
  `comp_tool_apply` → `run_tool_op`。
- `apply_adjustments` 全仓**只有一个调用点**（`engine/compositing/filter_graph.cpp` 的 `kSource`），
  作用在 **RGBA8 的 backdrop** 上；生成路径的每一块表面都是 `COMP_PIXEL_RGBA8`。
- 故 Pi 留下的、**只有声明没有实现**的 `apply_adjustments_hdr` 即便照上游
  `LayerAdjustment.applySixteenBit`（`LayerAdjustment.swift:156-160`）的分派表写全，
  也**没有任何调用者**——它会成为第 7 族零通路内核。**上游那个 `sixteenBit` 开关的前提是合成
  表面按文档深度携带 half，那是 C5。** 口径与出处留在 `adjustment_eval.h`（纯注释，无代码）。

C2 因此交付**有真调用点的那一半**：`COMP_TOOL_DEEP` 的往返分支对 HDR 图层走
`deep_hdr_normalize` → 8 位操作 → `deep_hdr_restore`（`tool_ops.cpp:323-347`，对齐上游 `DeepLayer.run`
HDR 分支的 `normalizing`，`DeepLayers.swift:196` / `:99-120`）。同轮：quantize 分支按格式分流到
`hdr_to_rgba8`（`:898`）、deep 参数闸放宽到收 RGBA16F（`:695`）、非 deep 闸对 HDR **如实拒绝**
而不是静默降位。语义要点：normalize 的输出是 **RGBA16 整数布局而非 half**（C0 冻结第 1 条），
restore 按 `scales` 乘回颜色、**alpha 原样**（`DeepPixels.c:289-290`）。

| 内核 | C2 前 | C2 后 |
|------|-------|-------|
| `deep_hdr_normalize` / `deep_hdr_restore` | 零通路 | **已接线**（HDR 图层上的 deep round trip） |
| `deep_hdr_exposure` / `extract_alpha_hdr` / `restore_alpha_hdr` | 零通路 | 零通路，阻塞点 = **C5**（调整表面与剪贴栈基底的深度） |
| `deep_take_painted_hdr` | 零通路 | 零通路，阻塞点 = **S4**（tile+coverage 仿制笔刷） |

护栏 4 例（`tests/test_tool_ops_domain.cpp:335/396/477/522`）。反向注入三次，各自只打红该红的用例：
N1（`is_hdr` 恒 false）→ C2-1 / C2-2 挂；N2（deep 闸退回只收 RGBA16）→ C2-1 / C2-2 / C2-3 挂；
N3（`use_rgba8_working_copy` 顺手把 HDR 降成 8 位）→ C2-4 挂 5 check（运行成功、深度掉到 RGBA8、
进了撤销栈）。还原后 `253 cases, 0 failed`，`grep -rn INJECT` 零残留。

**本轮第 5 次「护栏初报不红，先怀疑护栏本身」**：C2-2 最初用**均匀 4.0 高光场**、判据「亮点 ≥3/4」，
注入 N1 时**不红**。两层根因：

1. **`deep_low` 不做反预乘**（`DeepPixels.c:29-32`）：它把 16 位值整体乘 `255/65535` 再减 1，
   然后**夹到 `alpha8`**。4.0 的位模式 `0x4400` 经此得 66/67，被夹到错读出来的 alpha(60)，
   比值恰好 1.0——**错路给操作看到的也是满白**，只是标尺被一起读错了。反预乘其实发生在更下游的
   8 位内核里（`dither_apply` 自己除 `px[3]`）。
2. **均匀场天生看不见这件事**：`scale = max(brightest, 1)` 使「颜色 == 自身最大值」的均匀场在正确
   路径上必然归一到白；两条路在「亮不亮」这一维不可分，可分的是**还原后的绝对量级**（4.0 vs 1.059）
   ——那正是 C2-1 钉住的。

修法：探针换成**不透明 0.5 中灰**（正确路径 `scale = 1` ⇒ 操作看到 0.5 ⇒ 亮 2/4；错路看到
54/60 ≈ 0.9 ⇒ 亮 4/4，实测 32/64 vs 64/64），判据由单侧下界改为**上下界夹住**，并加一条不依赖
阈值的精确断言：完全不透明的 alpha 走完往返必须逐字节是 `0x3C00`（错路 `0x3C3C`）。补后 N1 稳定打红。
C2-1 最初也因「均匀场乘径向重采样仍是自己」在 `after != before` 上挂过一次，改用「左 3/4 高光块 +
右 1/4 逐列阶梯」的场才成立。教训同 §6.1.15：**探针的输入必须落在被注入量真正参与输出的那条支路上。**

基线 **249 → 253**（0 failed），`run_regression.py --require-ohos` **6/6 PASS**，OHOS 72 fail=0，
Windows 壳 27 项 SHA256 与基线一致。报告：`report/C2-hdr-round-trip-20261017-claude.md`。

### 6.1.17 C 方案 C3：浮点解码器接入（Radiance .hdr/.pic）+ 尺寸闸（2026-10-18，Claude 接手）

C3 把**浮点来源**接进解码分派：Radiance（`.hdr`/`.pic`）经 stb_image 的浮点通道解出**线性光**
浮点像素，再由 `wide_asset_from_linear` 按上游 `DeepLayers.swift:164-178` 判决——
编码过 extended sRGB → 取**反预乘**峰值 → `peak > 1.001` 则 HDR 图层（RGBA16F），否则 16 位图层。

| 位置 | 内容 |
|------|------|
| `comp_io.h:40` | `COMP_FORMAT_HDR = 8`，**追加在末尾**（0..7 已进宿主与 `.comp` 互操作约定） |
| `codec_lib.cpp:393-417` | `stb_decode_float_impl`；`CodecFloatImage` + `can_decode_float` / `decode_float` |
| `codec_registry.cpp:82-85` | Radiance 登记项，可读性取自**浮点通道的真实探测**（不是「有 stb 就能读」） |
| `layer.cpp:146-178` | `wide_asset_from_linear`（`kWideAssetPeak = 1.001f`） |
| `comp_io_abi.cpp:291-331` | 浮点分支：尺寸闸 → 解码 → 宽资产判决 |
| `comp_io_abi.cpp:147-158` | **结构上堵死**：浮点格式不许走 8 位通道（那会把白点以上的光抹平） |
| `comp_core.h:78-86` | 修掉 `COMP_PIXEL_RGBA16F` 上过期的「本构建无半精度像素路径，入口拒绝」 |

**浮点通道的三道闸**：格式闸（`codec_lib.cpp:396` 的 `stbi_is_hdr_from_memory`——
`stbi_loadf` 对非 Radiance 输入会静默走「8 位解码 + 转浮点」，`stb_image.h:1472-1474`，
改名成 `.hdr` 的 PNG 会得到一份**看起来合法、实则 8 位量化**的"浮点"像素）；
**分配前预检**（`comp_io_abi.cpp:298-303`，只读头部声明的 `-Y/+X`，不分配像素——伪造尺寸
不会先让 stb 按 `w*h*16` 分配；认不出头即放过，**不抢解码器的权威判定权**）；
解码后闸（`:319-323`，按解码器**实际报告**的尺寸）。

护栏 7 例（`tests/test_hdr_import.cpp`，**253 → 260**）：线性光解码、非 Radiance 拒绝、
extended sRGB 编码（与测试内独立实现比对）、NaN/Inf 收口、1.001 闸两侧边界、
分配前尺寸闸（合法 4×40000 payload 由解码器成功、由我们拒绝；只有头的 40000×40000 报**我们的**文案）、
端到端导入（扩展名映射 / 登记表 / 导出如实 UNSUPPORTED / `.comp` 存盘拒绝非 RGBA8 作为
「这层确实不是 RGBA8」的观测量，同尺寸 PNG 对照可存）。反向注入 5 轮，各自只打红该打红的用例：
I1 去掉编码（2 cases/10 checks）、I2a 闸改 1.0（1/1）、I2b 闸改 1.01（1/2）、
I3 关掉 Radiance 格式闸（1/6）、I4 关掉预检（1/2）。还原后 `260 cases, 0 failed`，`INJECT` 零残留。

**第 6 次「护栏初报不红，先怀疑护栏本身」——这次是量化台阶抹掉了判据。** I2 首次把闸改成
`1.0f` 时**全绿**。根因不在代码：白以上的 half 台阶是 `2^-10`，1.0 之后依次
**1.000977 / 1.001953 / 1.002930**，1.001 闸恰好卡在**前两级之间**；原用例里的「擦着白」
线性 `1.0005` 编码后是 `1.00022`，**half 取整又回到 1.0**，对闸的位置完全免疫。
修法是改护栏：补 `1.0023`（编码 1.001010 → half **1.000977**，闸下第一级）与
`1.0046`（编码 1.002019 → half **1.001953**，闸上第一级）分别钉住两侧，并加一条不依赖阈值的
自检断言把「输入落在哪一级台阶」也钉进护栏。**对判决阈值做护栏，必须先把阈值换算到存储精度的
格点上再挑输入**——`1.0005` 这种「看着很贴边界」的值是自我安慰。

**C3 不改任何内核调用点**：六件套的可达性与 C2 相同（normalize/restore 已接线；
exposure / extract+restore_alpha 仍等 **C5**，take_painted 等 **S4**）。C3 是入口侧的。

**本轮查明的两个改变计划的事实**：

1. **zlib 已 vendored**（`engine/third_party/zlib`，回归日志里 zlib 全部参与链接），
   此前记录的「EXR 的 ZIP/ZIPS 被 zlib 挡住」**作废**，C3b 直接用树内 zlib。
2. **`.comp` 存盘仍只收 RGBA8**（`comp_file.cpp:383-385` 保存侧拒绝、`:435-439` 拒绝 `.exr` 资产、
   `:615-620` 拒绝非 RGBA8 声明）⇒ **HDR 图层能导进来、能在内存里走 deep 往返，但一存盘就明确失败**。
   上游 v13 存的是 `<uuid>.exr`；闭这个环需要**资产级 EXR 读写器**，它**不依赖 C5**
   （写的是图层自己的 RGBA16F 资产，不是合成结果），与 C4 的「合成结果 → EXR」是两件事。

**顺序修订：C3 → C5 → C4 → C6**（原记 C4 在 C5 前，**不成立**）。依据是两处硬编码：
`cpu_backend.cpp:27-30` 的渲染目标只收 RGBA8、`compositor.cpp:70-72` 把合成表面写死成 RGBA8。
C4 交付的是「**合成结果** → EXR」，而今天的合成结果是 8 位表面——导出会**成功但内容错**
（高光早在 8 位副本里被抹平），比失败更坏，故 C4 必须等 C5。**唯一例外**是资产级
`<uuid>.exr` 读写器，它不经过合成表面，可随 C3b 先做。

**同轮如实记录（未根因）**：`rhi_probe_is_thread_safe`（`tests/test_rhi.cpp:73-106`）在 11 次
全量运行中挂了 1 次（刚链接完、机器满负荷时），其余 10 次（含连续 6 次压测）全绿；
与 C3 改动的文件无关，最可疑的路径是 AUTO 探测会**真实创建 D3D12 设备**
（`d3d12_backend.cpp:131-141`），高负载下设备创建失败会让 `caps.note` 文本变化。留待后续轮次。

基线 **253 → 260**（0 failed），`run_regression.py --require-ohos` **6/6 PASS**，OHOS `72 fail=0`，
Windows 壳 27 项 SHA256 与基线一致。报告：`report/C3-hdr-float-decoder-20261018-claude.md`。
**未交付的一半**：32 位 float TIFF——本机与本仓都没有 TIFF 解码器（树内 stb 无 TIFF，登记表里
TIFF 是 `kCodecUnavailable`），它不是「接一根线」能补的，见报告 §6/§10。

### 6.1.18 C 方案 C3b：自研 EXR 读取器 + 16 位资产存盘闭环（2026-10-18，Claude 接手）

C3 留下了两件事：`.comp` 存盘仍只收 RGBA8（HDR / 16 位图层一存盘就明确失败），以及 EXR
仍无解码器。C3b 把两件一起收掉，**不依赖 C5**——资产级 `<uuid>.exr` 读写的是图层自己的
像素，不是合成表面。

**读侧（`engine/format/exr_codec.*`）**：手写 scanline 读取器，只依赖树内 zlib。

- 容器：magic/version、flags、属性表、uint64 偏移表、chunk = `int32 y + int32 dataSize + payload`。
  tiled / deep / multipart 在头部按标志位拒绝；UINT、子采样通道、RANDOM_Y 同样带名字拒绝。
- 压缩：NONE / RLE / ZIPS（1 行块）/ ZIP（16 行块）；PIZ / PXR24 / B44 / B44A / DWAA / DWAB
  一律 `"<名字> compression is not supported"`，**不做近似解码**。
- 两个会静默毁像素的前置步骤：RLE/ZIP 写侧先做**偶奇字节重排**、再做**预测器**
  （`d = cur - prev + 384`）；读侧顺序必须完全相反：解压 → 逆预测 → 逆重排。
- `dataSize == rawSize` 是 OpenEXR 的「压缩无收益就存原样」约定：块内就是像素，**不能再解压**。
- 行语义：chunk 的 y 是块内**最低**行，块向上覆盖 `min(linesPerBlock, max_y - y + 1)`；
  DECREASING_Y 只改偏移表顺序，块内行仍自下而上。
- 尺寸闸在**任何面积分配之前**：`is_valid_canvas`（侧长 30000、总像素 1e8）；dataWindow 的
  闭区间跨度先按 int64 算，防止 `max - min + 1` 回绕成小尺寸。
- 写侧 `exr_encode_float` 只写 scanline / NONE / HALF|FLOAT，供 `.comp` 资产使用。

**资产化（`engine/format/comp_file.cpp`）**：

- 存盘：RGBA8 → `.png`；RGBA16 → `<uuid>.exr` + **FLOAT** 通道（half 只有 11 位尾数，会把
  16 位资产降成 11 位）；RGBA16F → HALF 通道（逐位精确）。文件内是线性光、直通 alpha，转换
  只用同一对曲线（`deep_u16_to_linear` / `deep_hdr_to_linear` 与其反向 `*_take_linear`）。
- 新增 C 内核 `deep_u16_to_linear` / `deep_u16_take_linear`：16 位整数的反预乘/预乘与
  sRGB 编解码，**不借道 half**。
- 读盘：`enginePixelFormat` 在**分配缓冲之前**解析；声明与资产必须一致（RGBA8 + `.exr`、
  16/16F + `.png` 都拒绝）。这条判定**先于文件 IO**——否则资产恰好缺失时，IO 错误会盖掉
  真正的错因。无 `enginePixelFormat` 的 `.exr`（上游只给扩展名）按内容峰值判决：`>1.001`
  走 RGBA16F，否则 RGBA16。

**护栏（`tests/test_exr.cpp`，16 个用例；总数 260 → 276）**：EXR 夹具全部手写（本机没有
EXR 工具/样本），期望值来自测试内**独立实现**的半精度编码与 OpenEXR 前置变换，绝不复用
被测实现。覆盖：NONE/HALF/FLOAT 逐位往返、RLE 与 ZIP/ZIPS 的逆重排+逆预测、16 行块与
1 行块、raw 块、DECREASING_Y 按 chunk y 落行、未知压缩/容器特性逐项拒绝、伪造 dataWindow
（30000×30000 与 int32 跨度回绕）在分配前拒绝、Y-only 与 `beauty.R/G/B/A` 通道选择、
写侧 HALF 精确 + FLOAT 保尾数、`.comp` 的 RGBA16 与 RGBA16F 存盘→读回逐值/逐位相等、
无 `enginePixelFormat` 的 EXR 峰值判决、u16 内核夹紧与往返。

**反向注入（6 轮，各自只打红该打红的用例）**：I1 去掉逆重排 + 关掉原样块分支 → 5 cases；
I2 去掉逆预测 + 按块序号放行 + 未知压缩按原样拷贝 → 5 cases；I3 关掉容器特性六道闸 →
1 case / 6 checks；I4 关掉画布尺寸闸（临时把夹具缩到 40000×8，避免注入时真去分配 GB 级
缓冲）→ 1 case / 3 checks；I5a 漏乘 alpha、I5b 漏除 alpha、I5c RGBA16 改走 HALF → 各自
打红 u16 内核或 16 位往返。manifest「声明与资产不匹配」这道守卫**首次就是红的**（错误
优先级 bug，见上），修好才绿。还原后 `grep -rn INJECT` 零残留。

**第 7 次「护栏报绿，先怀疑护栏本身」——RLE 夹具根本没走 RLE。** 最初的夹具只发字面量
游程，payload 必然大于 raw，`build_exr` 按 OpenEXR 的 raw 回退规则把它存成原样块；于是
I1 拿掉逆重排时用例**仍然是绿的**。改成真正的重复游程编码（样本逐行常量 → 预测后整块
几乎全是 `0x80`，payload 远小于 raw）后 I1/I2 才稳定打红。**压缩格式的护栏必须能证明
夹具真的产生了压缩块，否则测到的是 raw 回退。**

**实测**：`276 cases, 0 failed, 0 check failures`；`run_regression.py --require-ohos`
**6/6 PASS**；OHOS aarch64 交叉编译 **73 单元 fail=0**；Windows 壳 27 项 SHA256 与基线
一致；PIL 交叉验证 rc=0。报告：`report/C3b-exr-reader-20261018-claude.md`。

**如实边界**：C3b 交付时 EXR 只读不写（`comp_io_export(EXR)` 为 `UNSUPPORTED`）；
写侧已由 C4 接上（§6.1.20），但只写 scanline NONE + HALF。读取只做 scanline
（tile/deep/multipart 未实现）；PIZ/PXR24/B44/DWA 需要对应编解码器，
明确拒绝；不解析 chromaticities 等色彩属性，按 sRGB 口径；ZIP 的 16 字节前缀变体只在两个
长度字段都精确匹配时才跳过。

### 6.1.19 C 方案 C5：合成表面按文档深度携带 half（2026-10-18，Claude 接手）

C5 的目标是让**文档里有 HDR 图层时，合成表面本身就是 half**，白点以上的光在合成途中不被截断，
只有最终交付 8 位的缩略图/渲染目标才按显示口径截到白。验收底线是**8 位文档的 24 种混合模式
逐字节不变**。

| 位置 | 内容 |
|------|------|
| `compositing/compositor.cpp` | `composite_surface_format` 扫到任一非空 RGBA16F 图层即返回 half；`composite_layer` 按图层（或剪贴基底的 half）决定表面深度，half 结果经 `hdr_to_rgba8` 返回显示副本（缩略图输出契约仍是 RGBA8） |
| `compositing/composite_ops.cpp` | `half_to_straight`（alpha 夹 [0,1]，**颜色不夹**）、`store_half_premultiplied`（不夹到白、不夹到 alpha）、`blend_pixels_half`（与 8 位共用同一套 W3C 公式）、`blend_layer_into` 的 half 分支、`extract/restore_stack_alpha` 的 float 重载（走 `deep_extract_alpha_hdr` / `deep_restore_alpha_hdr`） |
| `compositing/filter_graph.cpp` | 剪贴栈表面/成员 backdrop 取 `canvas_format`；成员结果与栈表面深度不一致时以具名错误拒绝，绝不按错误字节宽混读 |
| `compositing/adjustment_eval.cpp` | half 表面上的调整：Exposure 直落 `deep_hdr_exposure`；跨通道调整 `deep_hdr_normalize` → 16 位往返 → `deep_hdr_restore` |
| `render/cpu_backend.cpp` | 渲染目标接受 RGBA8 与 RGBA16F；同深度零拷贝，跨深度显式转换（half→8 位走 `hdr_to_rgba8`，8 位→half 走 `rgba8_to_hdr`）；其余格式**带名字拒绝** |
| `domain/layer_merge.cpp` | `trim_to_content` 按格式解码 alpha（8 位看第 4 字节；16 位整数与 half 都看末两字节；half 走 `deep_half_value`），`0x00FF` / `0x0001` 不再被当成透明裁掉 |

**三处「看起来该夹、实际绝不能夹」**：

1. half 颜色**不夹到 alpha**。预乘 HDR 像素的 2.0 配 alpha 0.5 是合法状态（直通 4.0 的光），
   夹一次就变成 0.5。8 位显示副本的夹紧发生在 `dither_quantize16`（`DitherPixels.c:325-334`）：
   先按 alpha 截断再量化，于是 2.0@0.5 显示为 **128**（50% 白），不是 255（不透明的白）。
2. half 的**显示口径是截断到白，不是回绕**。`hdr_to_rgba8` 逐通道夹到 0..65535 再抖动量化，
   2.0 与 half 65504 都显示为白。
3. **HSV 的 saturation/lightness 是绝对 HSL 参数**（`hsv_to_rgb(hue, sat/100, light/100, …)`），
   不是增量。lightness=50 表示归一化后的 0.5，不是「加 50%」。

**护栏（`tests/test_c5_half_surface.cpp`，10 例；总数 276 → 286）**：

1. 表面深度跟随文档（RGBA8→8 位、RGBA16→8 位既有口径、RGBA16F→half）；
2. 2.0 与 65504 穿过合成仍原值，8 位副本两者都截到白；
3. **验收底线**：24 种混合模式下 FilterGraph 结果与直接顺序 `blend_layer_into` 逐字节相同；
4. half 混合与 8 位公式逐通道差 ≤ 2（24 种模式）；
5. 预乘颜色可高于 alpha（2.0 > 0.5，且不被改写）；
6. half 表面上的曝光与 HSV 调整保留白点以上的光；
7. half 剪贴栈按栈语义求值（成员替换基底颜色、alpha 保持 1）；
8. CPU 后端接受 half 目标（逐字节零拷贝）、RGBA8 目标是 half 结果的显示副本、RGBA16 被具名拒绝；
9. 图层缩略图对 HDR 图层仍返回 RGBA8 显示副本（2.0@0.5 → 128）；
10. 裁边按格式解码 alpha（half `0x00FF`、16 位整数 `0x0001` 都算有内容）。

**反向注入（5 轮，各自只打红该打红的用例）**：

| 注入 | 结果 |
|------|------|
| A. `store_half_premultiplied` 把颜色夹到 alpha | 3 cases / 6 checks（第 5、7 例等） |
| B. 裁边退回旧的 `px[bpp-1]` 判 alpha | 1 case / 6 checks（第 10 例） |
| C. CPU 后端放行 RGBA16 目标 | 1 case / 2 checks（第 8 例） |
| D. 表面深度恒为 RGBA8 | 6 cases / 10 checks（含 `test_abi.cpp` 两条 HDR 用例） |
| E. 剪贴栈成员 backdrop 强制 8 位 | 1 case / 2 checks（第 7 例：具名错误触发、合成失败） |

注入 E 同时证明 `filter_graph.cpp` 的「成员深度与栈表面不一致」这道守卫**是活的**：正常路径下
成员由 `canvas_format` 构造、深度必然一致，因此它是**防御性守卫**，靠注入才会触发；但触发时
拿到的是具名错误而不是按错误字节宽混读。还原后全量 `286 cases, 0 failed, 0 check failures`，
`grep -rn INJECT` 零残留。

**如实边界**：half 目标目前只在 **CPU 后端**落地；D3D12 构建（本机为诚实 stub）仍只接受
RGBA8 并**明确拒绝**其它格式，Vulkan 主渲染路径本就不在这次构建里（`render_document` 返回
UNSUPPORTED）。GPU 侧的 half 目标留待后端接线，不在这里假装支持。

### 6.1.20 C 方案 C4：合成结果导出 EXR（2026-10-18，Claude 接手）

C4 交付的是 `comp_io_export(EXR)`：写**合成表面本身**，不是 8 位显示副本。
先降到 8 位再写 EXR 会把白点以上的光截掉——文件写得出来、内容是错的，比失败更坏。

| 位置 | 内容 |
|------|------|
| `engine/abi/src/comp_io_abi.cpp` | EXR 分支在 8 位 flatten **之前**：申请 RGBA16F 表面 → `comp_render_flatten(..., COMP_PIXEL_RGBA16F, width*8)` → `deep_hdr_to_linear`（除 alpha + extended sRGB 曲线）→ `exr_encode_float(..., kExrSampleHalf)` → 写文件。PNG/JPEG 仍走原 8 位路径 |
| `engine/format/codec_lib.cpp` | `codec_lib_can_encode(EXR)` 取 `exr_codec_available()`：读写同一份自研 scanline 编解码器 |
| `engine/format/codec_registry.cpp` | EXR 条目 `can_write = codec_lib_can_encode(6u)`；可写时 note 追加 `export writes scanline NONE + HALF, linear light with straight alpha from the full-depth composite` |
| `tests/test_c4_exr_export.cpp` | 6 例：登记表可写、HDR 高光保真、8 位文档同路径、失败原因具名；C6 追加两例——写的是**合成结果**不是图层资产（图层只盖右半，左半必须透明黑）、空文档导出为全透明黑而不是未初始化内存 |
| `tests/test_codec.cpp` | 旧断言「EXR 导出不可用」改为「登记表必须说可写」，并保留反侧断言（note 不得再出现 `export unsupported`） |

**口径**：文件与 `.comp` 资产一致——线性光、直通 alpha、HALF；8 位文档由 CPU 后端
`rgba8_to_hdr` 扩成 half，走同一条路径；后端给不出 half 目标时**具名失败**，不悄悄降回 8 位。

**护栏**：新增 6 例，总数 **286 → 292**（`292 cases, 0 failed`）。关键断言用测试内独立
`ref_linear` 给期望值：预乘 2.0 → `ref_linear(2.0) ≈ 4.95` 且 `> 1`；预乘 0.25/0.5/1.0 @ alpha 0.5
→ 直通 0.5/1.0/2.0 的线性值（同时钉住「除 alpha」与「sRGB 曲线」）；8 位 (100,150,200) →
`ref_linear(100/255)` 等，全透明像素为 0；TIFF 仍 `UNSUPPORTED` 且错误里出现 `TIFF`。
C6 追加的两例把「导出的是合成表面」从注释变成可观测：画布 2×1、图层只覆盖右半时，左半必须
读到透明黑；无图层的空文档必须全零（含 alpha），而不是把未初始化缓冲写出去。

**反向注入（6 轮，各自只打红该打红的用例）**：

| 注入 | 模拟的错误 | 结果 |
|------|-----------|------|
| A. 线性值夹到 1.0 | 先降 8 位再写 EXR（高光截到白） | 1 case / 3 checks |
| B. `codec_lib_can_encode(EXR)` 返回 false | 登记表谎称只读 | 4 cases / 9 checks |
| C. 把 extended sRGB 直接写进 EXR | 忘了线性化 | 2 cases / 4 checks |
| D. 转换后乘回 alpha | 忘了除 alpha（写的是预乘值） | 1 case / 2 checks |
| E. 导出前把每个像素 alpha 置 1 | 输出变成整幅不透明（空文档不再全零） | 4 cases / 6 checks |
| F. 合成表面整体右移一像素 | 把资产位置当画布位置（导出不是合成结果） | 3 cases / 8 checks |

E 打红的是所有断言 alpha 的用例（含空文档全零），F 打红的是断言**位置**的三例、空文档保持
全绿——这正是「各自只打红该打红的用例」应有的形态。还原后全量 `292 cases, 0 failed,
0 check failures`，`grep -rn INJECT` 零残留（仅 README 有历史提及）。

**全量回归（C6，2026-10-18）**：

```
python scripts/run_regression.py --require-ohos --python <local-workspace>\tools\python\python.exe
```

**6/6 PASS，0 skipped**：① 构建 + 单测 100.6s（`292 cases, 0 failed`）；② Windows 壳 27 项
`failures=0`、SHA256 `32151220f4748dfa…` 与基线一致；③ 编解码交叉验证 rc=0；④ `.comp` 双向
交叉验证 rc=0；⑤ 共享 DLL ctypes 真实调用（`comp_version=1.0.0`）；⑥ OHOS aarch64 交叉编译
+ NAPI 壳 rc=0。指定带 Pillow 12.3.0 + numpy 2.5.3 的 `<local-workspace>\tools\python\python.exe`
后，步骤 ③/④ 从历来的 `SKIP`（PIL unavailable）变为真实执行——提升的是证据等级，不是代码。

**这里要说明一条边界**：步骤 ③ 的交叉验证覆盖 PNG/JPEG 等 8 位格式，**不覆盖 EXR**——本机没有
独立的 EXR 解码器。C4 的 EXR 验收是「写出 → 同族自研读取器读回 → 与测试内独立公式比对」，
钉住了编码语义，但**不能**声称已与第三方 EXR 实现互操作。详见报告 §14.4。

**如实边界**：EXR 导出要求后端能产出 RGBA16F 表面，当前只有 CPU 后端可写；D3D12 构建下
flatten 被后端具名拒绝，导出失败而不是退回 8 位。存储是 HALF：线性化后绝对值超过 half
上限（约 65520）的像素在文件里读回 `inf`——这是 half 容器的量程边界，不是把高光夹成白。

### 6.1.21 引擎核收尾：`deep_take_painted` 接线 + 三条构建路径实测（2026-10-18，Claude）

**1. 最后一对内核接入产品通路。**

上游语义只有一句话（`BrushStroke.swift:1097` 的 `sixteenBit()`）：先把深像素交给画笔画进宽
缓冲，再渲染两份 **8 位**结果——`painted`（原图 + 笔迹）与 `unpainted`（原图），
`deep_take_painted` 只把「两者不同」的像素写回深缓冲；HDR 走 `deep_take_painted_hdr`。
所以这里**不需要** tile+coverage 记账，整幅比较即可。

`engine/domain/tool_ops.cpp` 的实现（`DeepPaintCopy`）：

- `build_deep_paint_copy` 收好原始深像素（共享 + COW，工具写的是副本），
  并把图层换成 8 位工作副本（RGBA16 → `deep_to_rgba8`，RGBA16F → `hdr_to_rgba8`）；
- 内核跑完后 `merge_deep_paint` 调 `deep_take_painted(_hdr)`，**只**把写成过的像素并回；
- 内核失败或并回失败：原始深像素原样放回，事务照旧回滚（不留 8 位半成品）。

作用域严格限定在**落笔族**（Stroke / Clone / Smudge / Liquify，上游 `SmudgeLiquify.swift`
同样用 `DeepLayer.flattened` + `stroke.deepClone`）。其余工具保持上游
`DeepLayers.swift:5` 的规则——16 位图层被非深度算子碰过就**永久降 8 位**，HDR 继续如实拒绝。

**2. 护栏（+3 例，341 → 344）。**

| 用例 | 钉住什么 |
|------|---------|
| `tool_ops_domain_stroke_keeps_sixteen_bit_depth_outside_the_stroke` | 16 位图层落笔后格式仍是 `RGBA16`；笔迹外像素**未变**；笔迹内每通道都是 `v×257`（真 8 位回写，不是伪造 16 位）；撤销后逐字节还原 |
| `tool_ops_domain_hdr_stroke_keeps_light_above_white_outside_the_stroke` | HDR 图层落笔后格式仍是 `RGBA16F`；白以上的光（4.0）原样保留；笔迹内落到 ≤1.0；撤销还原 |
| `tool_ops_domain_clone_and_warp_keep_sixteen_bit_depth` | Clone / Smudge / Liquify 三种落笔族在 16 位上都是 `COMP_OK` + 保持 `RGBA16` + 恰好一步撤销 |

**3. CMake 路径实测（此前一直是「未验证」）。**

本机现装有 CMake 4.4.4 + Ninja（PyPI），但 CMake 无法从 zig 驱动推导出归档器，
故 `CMAKE_AR` 指向 `python -m ziglang ar` 包装。跑通过程中修掉**四处**「CMake 与
`build.py` 不等价」的分叉，每一处都会让 CMake 路径静默少测或编不过：

| 症状 | 根因 | 修复 |
|------|------|------|
| libpng 在 `pngstruct.h:30` 找不到 `zlib.h` | vendor 包含路径只加进了 `build.py` | `CMakeLists.txt` 同步 `third_party/{zlib,libpng,stb}` |
| 只注册 341 例（少 3 个） | 测试目标看不到 `zlib.h`，`__has_include` 让 EXR ZIP/ZIPS 用例整段不编译 | 测试目标补齐与引擎相同的包含路径 |
| `ctest` 报 12 例失败 | `ctest` 默认在 `build-cmake/` 运行，`resources/camera_raw_tables.bin` 用相对仓库根的路径 | `add_test(... WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})` |
| 静态库链接报 `_Block_object_assign` 未定义 | clang 在 Windows 上把 blocks 符号标成 `dllimport`，lld 拒绝用静态库成员满足导入引用 | `comp_blocks_rt.c` 补 `__imp_*` 导入槽位（`_WIN32` 守卫） |

结果：`cmake --build build-cmake` 全绿，`ctest` **344/344 通过**，与 `build.py` 完全一致。

**4. clang-cl（MSVC ABI）路径实测。**

VS 2022 工具集在位（MSVC 14.44.35207 / Windows SDK 10.0.26100 / clang-cl 19.1.5）。
用 `vcvars64` 环境 + clang-cl 编译全部 **113 个 TU**（38 `.c` + 44 `.cpp` + 31 测试）并链接，
`compositor_tests.exe` **344/344 通过**——这是 MSVC ABI + MSVC STL 下的真实构建，
不是 zig 的 MinGW 目标。顺带确认：`d3d12_backend.cpp` 加 `-DCOMP_HAS_D3D12` 能编译
（§6.2 的「无 SDK」记录已订正）。`cl.exe` 仍不可用（像素内核的 clang block 语法），
Windows 侧推荐编译器仍是 clang-cl。

**5. 全量回归。**

`scripts/run_regression.py --require-ohos`：**6/6 PASS，0 skipped，0 failed**；
步骤 ① `344 cases, 0 failed`，步骤 ② Windows 壳 27 项 `failures=0` + SHA256 与基线一致，
步骤 ⑤ 共享 DLL（含 `comp_blocks_rt` 改动）真实加载调用，步骤 ⑥ OHOS aarch64 交叉编译 + NAPI rc=0。

**6. 仍然不在本轮的**：设备侧后端接主渲染路径（D3D12/Vulkan 就绪但 `render_document`
仍走 CPU）、鸿蒙真机联调（`hdc list targets` 为空）、macOS 端逐像素对比。

### 6.2 尚未验证的部分（证据边界）


- ~~**CMake 路径未实测**~~ → **2026-10-18 已实测**：`cmake + ninja + ctest` 在本机跑通
  （ziglang 驱动，`CMAKE_AR` 指向 `python -m ziglang ar` 包装），`ctest` 344/344 通过。
  过程中修掉三处「CMake 静默少测」的分叉：vendor 包含路径、测试目标的包含路径
  （`__has_include("zlib.h")` 决定是否注册 3 个 EXR 用例）、测试工作目录（`resources/`
  相对仓库根）。证据见 §6.1.21。
- ~~**MSVC / clang-cl 路径未实测**~~ → **2026-10-18 已实测**：VS 2022 工具集在位
  （MSVC 14.44.35207、Windows SDK 10.0.26100、clang-cl 19.1.5）。用 clang-cl 以
  **MSVC ABI + MSVC STL** 编译全部 113 个 TU 并链接 `compositor_tests.exe`，344/344 通过。
  `cl.exe` 仍不可用（像素内核用 clang block 语法），故 Windows 侧的推荐编译器仍是 clang-cl。
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
- **D3D12 真实设备路径：SDK 已在位，仍未上主渲染路径**（2026-10-18 订正）。
  此前记录的「无 `d3d12.h`」是错的：Windows SDK 10.0.22621 / 10.0.26100 装在
  `E:\AMD\include\`，`engine/render/d3d12_backend.cpp` 加 `-DCOMP_HAS_D3D12` 已用 clang-cl
  实测编译通过（53 KB 目标文件）。因此缺的不是 SDK，而是**把设备侧后端接到引擎主渲染路径**
  （`render_document` 仍返回 UNSUPPORTED、`caps().offscreen_compute` 仍为 false）——
  这是功能/性能项，默认构建继续关掉 `COMP_HAS_D3D12`，不把未验证的设备路径带进基线。
- **SPIR-V 仅静态校验**：真机 `vkCreateShaderModule` 已实际接受该 blob（不报错），但未接校验层
  做更严格的运行期校验；静态证据为 glslang 编译 + 真机管线创建成功。
- **第三方编解码库：部分已接入，其余仍未链接**。PNG（libpng 1.6.44 + zlib 1.3.1）、
  JPEG（stb，有损）、PSD（stb，只读）的真实源码已 vendoring 到 `engine/third_party/` 并真实可用；
  TIFF / HEIF / WebP 在本机**无任何可链接依赖**（无 cmake / make / gcc / clang /
  pkg-config / vcpkg，全盘常见根目录下未找到 `jpeglib.h` / `lcms2.h` / `ImfHeader.h`），
  故仍如实 `UNSUPPORTED` 并写明原因。**EXR 例外**：C3b/C4 用树内 zlib 自己写了 scanline
  读写器（不复用 OpenEXR 库）：读只解 NONE/RLE/ZIPS/ZIP，写只出 NONE + HALF；PIZ/PXR24/B44/B44A/DWAA/DWAB、
  tiled、deep、multipart、UINT、子采样通道、RANDOM_Y 一律带名字拒绝。后续接入步骤见
  `docs/codec-migration.md` 第 6 节。
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

- **口径 A：分母 = 引擎核能力**（引擎 + ABI + 两壳）。回答「引擎核做完了吗」→ **≈97%**。
- **口径 B：分母 = 上游产品功能条目**（上游 README 的 48 条）。回答「能当 Photoshop 用吗」
  → **≈45%**（命中约 20 条）。

AUDIT8 记的 ≈90% 是**口径 A**，天然不含上游 macOS 应用层（`Compositor/UI` 9.3k 行 +
`ContentView`），因此「90% vs 45%」不矛盾。

#### 口径 A：引擎核

把「同等正常使用」拆成**四层**，逐层给权重与实测覆盖率。
引擎核（`engine/` + ABI + 两壳）已完成，**UI/编辑器层整体不在本仓范围**。

| 层 | 权重 | 覆盖率 | AUDIT8 | 变动依据 |
|----|------|--------|--------|---------|
| ① 引擎核契约面（ABI / 领域模型 / 事务 / 历史 / 诊断 / 边界） | 30% | **~95%** | ~95% | 102 个导出 ABI 全有异常护栏；句柄 magic、反序生命周期、线程局部错误槽、定制分配器、字节预算、transform 写入侧闸均已落地并有护栏；`run_tool_op` 的 `out_code` 契约已补 |
| ② 像素内核**有产品通路**的部分 | 35% | **~100%** | ~86% | AUDIT8 列的可达缺口清零：`wand_trace`、`scanlines`/`chunky`、deep round trip、`cube_apply`(16bit)、`extract/restore_alpha` 已接（见 §6.1.10）；**C5 又接上 `hdr_exposure`/`extract_alpha_hdr`/`restore_alpha_hdr`**（half 表面，§6.1.19）；**最后一对 `deep_take_painted`/`deep_take_painted_hdr` 已接进落笔族**（Stroke/Clone/Smudge/Liquify 在 16 位与 HDR 图层上保留深度，§6.1.21）——上游语义本就是「整幅比较 painted/unpainted」，不需要 tile+coverage 记账。`dither_dots`/`dither_glow`/`deep_split`/`deep_merge` 经核实**已接线**，不再计入缺口；`brush_alpha_bounds` 一族由 `extract/restore_alpha` 取代，属**主动不接**而非缺口 |
| ③ 合成与渲染正确性 | 20% | **~98%** | ~95% | **剪贴栈 alpha 语义对齐上游**（`a=0` → `a=128`）、`ImageBuffer::fill` 预乘修正、hidden 层优先级修正、剪贴栈成员 backdrop 补齐；**C5：合成表面按文档深度携带 half，8 位 24 种混合模式逐字节不变**（§6.1.19）；**C4：合成结果按全深度导出 EXR，8 位文档同路径**（§6.1.20） |
| ④ 跨端与交付（两壳 / 编解码 / 互通 / 回归） | 15% | **~90%** | ~80% | Windows 壳 27 项实机自检基线、OHOS aarch64 交叉编译 + NAPI、PNG/JPEG/PSD 真实编解码、`.comp` 与 PIL 双向 0 LSB 交叉验证；**2026-10-18 补上 `cmake+ctest` 与 clang-cl（MSVC ABI）两条构建路径实测**（各 344/344，§6.1.21）。扣分项：无 OHOS 真机、无 macOS 逐像素对比 |

**加权完成度 ≈ 97%**（口径 A）。算术：① 30%×95% + ② 35%×100% + ③ 20%×98%
+ ④ 15%×90% = 28.5 + 35.0 + 19.6 + 13.5 = **96.6% ≈ 97%**。

**剩下的 3% 按层拆开，全部有明确归属**（不再是「被上游工具面阻塞」）：

- ① 契约面 1.5 个点：`run_tool_op` 之外的少量诊断/边界口径；
- ③ 合成与渲染 0.4 个点：**设备侧后端未接主渲染路径**（D3D12 代码已能在 SDK 下编译，
  `render_document` 仍走 CPU，见 §6.2）；
- ④ 跨端 1.5 个点：无鸿蒙真机（`hdc list targets` 空）、无 macOS 端逐像素对比。

上一轮记的「`deep_take_painted` 被 S4 仿制笔刷阻塞」已证伪：上游 `BrushStroke.swift:1097`
的 `sixteenBit()` 是**整幅**比较 painted/unpainted，本仓照此实现后不再需要 tile+coverage 模型。

#### 口径 B：产品功能面

| 上游类（README 48 条 / 7 大类） | ng 命中 |
|---------|---------|
| Layers（图层/组/不透明度/全混合模式/图层蒙版/剪贴蒙版/调整层 12 kind/图层效果 6 kind/合并/重排嵌套） | ✓ |
| Transform（仿射 move·scale·rotate / distort / 精确值） | ✓ |
| Painting（笔刷 / Spot Heal / Content-Aware Fill） | ✓（笔刷为解析式，噪声未采样） |
| Canvas（Canvas Size / Image Size / Trim / 旋转） | ✓ |
| **Camera Raw**（6 文件 1,708 行） | ✓ 设置解析、曲线、763 张实测表与完整像素管线已交付（`comp_camera_raw_apply` 等 3 个 ABI，§6.10）；几何 / Upright 透视变换与 RAW 解码未做 |
| **选区几何**（Marquee/Lasso/Pen/Select Subject/抠图，1,000+ 行） | ✓ **部分**：Marquee/Lasso/Pen 的形状、叠加、全选/取消/反选、羽化/扩缩已交付（`comp_selection_*`，§6.7）；Select Subject / 抠图仍为零（依赖 ML，不在本仓范围） |
| **Type / Shape / Gradient**（TypeTool 472 行 + ShapeTool + Gradient） | **零** |
| **Clone Stamp / Smudge / Liquify**（400+ 行） | ✓ 仿制图章（源快照 / aligned 偏移 / sampleAllLayers）、涂抹（carried 拖色）、液化 Push（前向位移重采样）已交付（`comp_tool_apply` kind 9/10/11，§6.9）；同一 `BlurToolMode` 里的 Blur 模式也已在 P1 补齐（`COMP_TOOL_BLUR = 12`，§6.12） |
| **Crop 工具**（`Crop.swift` 249 行） | ✓ 拖拽成框（比例 / Option 对称）、比例切换、取整、可提交性判定与提交（平移图层 + 换画布，一步撤销）已交付（`comp_crop_*`，§6.8）；手柄 / 蚂蚁线 / 吸附参考线属外壳 |
| **Vignette / Bloom / Remove Background**（`Filters.swift` 1,007 行） | ✓ **部分**：Vignette / Bloom（含 Tonal Contrast、Lens Correction）已交付（`comp_filter_apply`，§6.12）；Remove Background 仍为零（依赖 ML / 分割，不在本仓范围） |
| **格式导入** HEIC/TIFF/WebP/SVG/RAW/PSB | 枚举有 7 种，但 HEIF/WebP、SVG、PSB 无解码器，TIFF=UNAVAILABLE |

即 **约 25/48 命中 ≈ 52%**（Vignette 与 Bloom 各计一条；Tonal Contrast /
Lens Correction 是同一 `Filters.swift` 通路的附加值，不额外抬高条数；选区几何按
「部分命中」计，未抬高条数——缺的 Select Subject / 抠图正是该条的主要工作量；
Crop、Clone/Smudge/Liquify 与 Camera Raw 按已交付计入）。其余约 4,800 行 Swift **完全没有对应物**，
且这些**不属于「接线」范畴，属新增功能**——不是把孤立内核接出去能解决的。

#### 口径 A 与 B 不一致的原因与推进顺序

两者差异集中在 ①~④ 盖不到的地方：口径 A 问的是「引擎核的契约面与像素保真度做完了吗」，
答案是基本做完（≈97%，§6.1.21 后）；口径 B 问的是「用户能做的事做完了吗」，答案是还差一半。
**像素算法层几乎满配**：上游 10 组 `.c` 内核逐文件 diff，**6 组 diff 行数为 0**
（Brush/Heal/Lens/Levels/Noise/Wand 逐字节等同）；PatchPixels / DitherPixels / DeepPixels
为去 GCD 的调度改写或主动超集；**AdjustPixels 曾落后 4 个函数**（上游 1.5.2/1.5.3 新增的
`camera_raw_lookup_is_valid` / `camera_raw_repair_lookup` / `box_mean_plane` /
`adjust_camera_raw_shadows`），已于 §6.1.15 全部补齐——补完后 10 组的**函数名差集为零**，
AdjustPixels 与上游的 diff 只剩「去 GCD」与 include。

**功能面推进顺序**（边际成本从低到高）：
选区几何（1,000+ 行，但它是几乎所有下游编辑的入口；**2026-10-18 已交付，见 §6.7**）
→ Crop 工具（249 行，解锁导出打印尺寸；**2026-10-18 已交付，见 §6.8**）
→ Clone Stamp + Smudge/Liquify（已有 `PatchPixels`/`HealPixels` 基础设施可复用；**2026-10-18 已交付，见 §6.9**）
→ Camera Raw（1,708 行 + RAW 解码，投入最大，但是上游招牌功能；**像素管线 2026-10-18 已交付，见 §6.10**）
→ 滤镜 / Blur（`Filters.swift` 四条滤镜 + `BlurToolMode` 的 Blur；复用现有像素内核；
  **2026-10-18 已交付，见 §6.12**）
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
- **Blur（P1 追加，kind 12）**：整层模糊样本在**整笔开始前**取好，同一笔的折线不会
  吃到自己刚模糊过的像素；`radius` 0.5..50，`0` 被参数闸拒绝（上游会把 0 抬高到 0.5，
  若按「0 = 恒等」解读会得到相反结果）。细节见 [`docs/tool-ops.md`](docs/tool-ops.md) §4.9。

### 6.6 内核接线现状（2026-10-18 更新）

| 内核 | 产品通路 | 已接线 | 仍未接线 |
|------|---------|--------|----------|
| Wand | `COMP_TOOL_SELECT` | `wand_mask`、`wand_trace`（只读闭合轮廓） | — |
| Heal | `COMP_TOOL_HEAL` | `spot_heal`、`heal_membrane`（内部派发） | `heal_coverage_bounds` |
| Patch | `COMP_TOOL_HEAL` | `patch_fill` | — |
| Dither | `COMP_TOOL_DITHER` | `dither_apply`、`scanlines`、`chunky`（后两者分别接 `dither_glow` 与 `dither_dots`） | — |
| Lens | `COMP_TOOL_LENS` / `COMP_FILTER_LENS_CORRECTION` | `lens_distort`（工具收 `|k| <= 1`，滤镜收 `-100..100` 再换算 `k`） | — |
| Blur | `COMP_TOOL_BLUR` / Tonal Contrast、Bloom 的 detail 模糊 | `blur_rgba8_copy`（三趟滑动窗口盒式近似，与 Gaussian Blur 调整层同一实现） | — |
| Brush | `COMP_TOOL_STROKE` | 解析式圆头笔 + **`noise_add_at` 颗粒**（噪声钉在文档坐标；`seed` 参与计算） | `brush_alpha_bounds` 一族（剪贴栈已改走 `extract/restore_alpha`） |
| Noise | `COMP_TOOL_STROKE`（经 brush 字段） | `noise_add_at`（origin 锚定） | — |
| Levels | `comp_layer_histogram`（只读查询） | `levels_histogram`（**预乘先反预乘再取亮度**） | — |
| Heal | `COMP_TOOL_HEAL` | `spot_heal`、`heal_membrane`（内部派发）、**`heal_coverage_bounds`**（`comp_tool_coverage_bounds` 只读查询） | — |
| Deep | `COMP_TOOL_DEEP` | `dither_quantize16`、**deep round trip**（内含 `deep_split`/`deep_merge`）、**16 位 `cube_apply`**、`extract_alpha`/`restore_alpha`、**`hdr_normalize`/`hdr_restore`**（C2：HDR 图层上的 deep round trip，§6.1.16）、**`hdr_exposure`/`extract_alpha_hdr`/`restore_alpha_hdr`**（C5：half 调整与剪贴栈 coverage，§6.1.19）、**`deep_take_painted`/`deep_take_painted_hdr`**（落笔族在 RGBA16 / RGBA16F 上保留深像素，§6.1.21） | — |
| HDR | `comp_layer_set_pixels`（RGBA16F 放行）+ `flatten`（`resample.cpp:171` 分流） | **`hdr_to_rgba8`**（C1）+ **HDR 图层上的 deep round trip**（C2：`hdr_normalize` → 8 位操作 → `hdr_restore`，`tool_ops.cpp:323`）+ **half 合成表面与 `hdr_exposure`/`extract_alpha_hdr`/`restore_alpha_hdr`**（C5，§6.1.19；`rgba8_to_hdr` 由 CPU 后端消费）+ **浮点解码器**（C3，Radiance `.hdr`/`.pic`）+ **`.comp` 的 `<uuid>.exr` 资产读写**（C3b，§6.1.18）+ **合成结果导出 EXR**（C4，`comp_io_export` 全深度 flatten → 线性光 HALF，§6.1.20） | float TIFF 无解码器、如实不支持 |
| Adjust | `comp_camera_raw_apply`（Camera Raw 全管线）、`comp_layer_histogram` 等 | 内核 62 个函数已与上游对齐（§6.1.15）；Camera Raw 产品通路见 §6.10 | 其余 `adjust_*` 单独算子无独立 ABI |

### 6.7 F1 选区几何（2026-10-18，Claude）

**交付物**：`engine/domain/selection.{h,cpp}`（域层，984 行）、
`engine/abi/src/comp_selection_abi.cpp`、`engine/abi/include/compositor/comp_selection.h`
（10 个 ABI）、`tests/test_selection.cpp`（12 例）。

#### 为什么存蒙版而不是路径

上游 `DocumentSelection` 的唯一真源是 `CGPath`，coverage 在使用时才由 CoreGraphics
栅格化；本仓不引入路径库，改为在**文档分辨率**上存一张 8 位灰度蒙版
（`MaskBuffer`，0 = 不选中，255 = 全选中）+ 一个羽化参数：

- 每个消费点（画笔/橡皮裁剪、内容感知填充、Crop 之后的重建）本来就要逐像素
  coverage，路径每次现场栅格化只会把同一份工作重复多遍；
- 蒙版与图层蒙版同构，直接复用 `MaskBuffer` 的 shared_ptr + COW 资产模型，
  「整篇快照」的撤销因此自动成立，不需要给选区单开一套历史钩子；
- 选中 0/N 像素这件事与「用过哪条贝塞尔曲线」无关。

**「无选区」与「显式空选区」必须区分**：`active = 0` 是无选区（编辑不受限）；
`active = 1` 且蒙版全 0 是显式空选区（合法但落空的形状，编辑结果为空）。
上游用 `nil` 与空 `CGPath` 区分，这里用 `active` 区分，`comp_selection_get_active`
是唯一判据。全选后反选得到的是**无选区**，Contract 缩到没有得到的是**显式空选区**。

#### ABI

| 函数 | 语义 |
|------|------|
| `comp_selection_apply_shape` | 矩形/椭圆（两个对角点）/多边形（≥3 点）栅格化并叠加 |
| `comp_selection_apply_path` | 三次贝塞尔路径（`new_subpath` 对齐 CGPath 的 current point 语义） |
| `comp_selection_select_all` / `deselect` / `invert` | 全选 / 取消 / 反选 |
| `comp_selection_modify` | `FEATHER`(1..250) / `EXPAND`(1..500) / `CONTRACT`(1..500) |
| `comp_selection_get_active` / `get_feather` / `get_bounds` | 只读查询 |
| `comp_selection_get_coverage` | 有效 coverage（含羽化）拷进调用方缓冲 |

所有写函数都带 `int32_t* out_changed`（可为 NULL）：状态**真的变了**才写 1。
枚举类型名 `comp_selection_modify_op` 带 `_op` 后缀——C 里 typedef 与函数名共用
普通标识符命名空间，`comp_selection_modify` 已被函数占用。

#### 数值口径

- **栅格化**：4 行超采样 + 水平方向解析覆盖率（非 AA 时取每像素中心一票，
  阈值 128）；非零缠绕（`CGPath.fillPath(using: .winding)`），反向子路径是洞、
  同向自交不会互相抵消。曲线按容差 0.1 px / 最大 20 层递归扁平化。
- **叠加**：replace / add / subtract 在**未羽化**的 base 上做——羽化是形状的参数，
  不烘焙进蒙版，否则反复叠选会越叠越糊。任何成功的形状操作都把 feather 重置为 0
  （上游 `applySelection` 新建 `DocumentSelection` 时没有传 feather）。
- **羽化**：高斯 σ = feather / 2；σ ≤ 2.5 走精确可分离高斯（半径 `ceil(3σ)`，
  uint16 中间缓冲），否则 3 遍箱式滤波（`r = lround((sqrt(4σ²+1)-1)/2)`，滑动窗口
  O(1)/px）。边界钳到画布（全选羽化不会在画布边缘变淡）。再次羽化按
  `sqrt(f1² + f2²)` 叠加、上限 250。
- **扩缩**：精确欧氏距离场（Felzenszwalb & Huttenlocher，O(n)），在非零包围盒
  外扩 `|delta| + 2` 的窗口里算，窗口外视为未选中。深度模型让**整像素选区的边界
  落在整像素上**：外扩 N 后第 N 个外层整像素恰好 255、再外一层恰好 0；外扩被
  画布裁掉，内缩连画布边缘一起缩。
- **包围盒**：非零像素 bbox 外扩 `ceil(2 * feather)` 再裁到画布；无选区或显式空
  选区返回 `{0,0,0,0}`。

#### 事务、历史与参数闸

- **校验先行、失败无副作用**：句柄、枚举、空输入、amount 范围与输出指针在 ABI 层
  先校验；点数/段数上限与坐标有限性由域函数统一校验（单点真源，不重复实现）。
  所有失败路径都不 commit、不改状态（`comp_txn_active == 0`、撤销深度不变，有护栏钉住）。
- **没改就不进历史**：域函数用 `masks_equal` 做等值短路，ABI 层 `changed` 为假
  时不 commit——上游 `setSelection` 对相同值不产生撤销步，这里同口径。
- **一步撤销**：ABI 在未处于显式事务时补一个 `ImplicitTxn`；显式事务内各次调用的
  commit 被抑制，由外层 commit 产出恰好一步。
- **选区是 `DocumentState` 的成员**（不是 `Document` 的旁路成员）：事务快照、
  撤销/重做、回滚自动覆盖；历史字节账本把选区蒙版与图层资产记在同一本账上。
- **Image Size 丢弃选区**：换画布 = 换文档坐标系，蒙版随之失效；上游
  `ImageResizer` 直接构造新 `CanvasDocument`，选区自然不存在。这里同样丢弃而不是
  缩放（缩放 0/255 蒙版会把边缘糊成半选），与尺寸变更同属一步撤销。

#### 上限与护栏

| 项 | 上限 |
|----|------|
| 形状点数 | 2^18（262,144） |
| 路径段数 | 2^16（65,536；扁平化后的顶点总数同样受 2^18 约束） |
| Feather | 1..250 |
| Expand / Contract | 1..500 |

`tests/test_selection.cpp` 的 12 例覆盖：矩形精确像素/等值短路的撤销深度、add 与
subtract（含无选区时 subtract 无操作）、椭圆抗锯齿与面积、路径洞的非零缠绕、
真实贝塞尔曲线的扁平化边界、全选/反选/取消的状态迁移与往返、羽化的软边与叠加、
扩缩的整像素边界与 Contract 到空、画布裁剪（不环绕）、非法参数不留事务、
coverage 缓冲校验（尺寸/stride/行内 padding 不被写）、显式事务的分组与 abort 回滚。

**反向注入（2 轮，各自只打红该打红的用例）**：
① `finish()` 无条件 commit → `selection_rect_hits_exact_pixels` 的撤销深度与
undo 后状态变红（证明「没改不进历史」的护栏真的在起作用）；
② 扩缩的整像素深度 `- 0.5` 改成 `- 0.0` → `selection_expand_and_contract_keep_pixel_edges`
5 条断言变红（四条边界像素 + 面积）。两轮均已还原。

#### 明示边界

- **Select Subject / 抠图不在本仓范围**：依赖 ML 推理，无法用「零依赖自研」方式
  复刻；口径 B 的该条只按「部分命中」计。
- **`.comp` 不落选区**：当前保存器只写图层与文档元数据，重新打开文档后无选区。
- **本仓无 UI**：选区 ABI 已可供两壳调用，但 Marquee/Lasso 的交互（拖拽矩形、
  套索采样、蚂蚁线）属外壳职责。

### 6.8 F2 Crop 工具（2026-10-18，Claude）

**交付物**：`engine/domain/crop.{h,cpp}`（104 / 170 行）、
`engine/abi/src/comp_crop_abi.cpp`、`engine/abi/include/compositor/comp_crop.h`
（5 个 ABI）、`tests/test_crop.cpp`（12 例）。

#### 为什么不能复用 `comp_document_set_size`

`set_size` 是 **Canvas Size**：它只改画布并把图层包围盒**裁剪**回画布范围，
没有 content offset。上游 `commitCrop` 走的是 `CanvasResizer.resize(contentOffset:)`——
**平移所有图层原点、不裁像素**，画布外的内容保留（渲染时被画布边界裁掉）。
两者语义正交，复用会把内容永久裁掉。因此 F2 在域层单独实现 `crop_apply`：
换画布 + 平移 transform，不重采样、不重排缓冲。

#### ABI

| 函数 | 语义 | 是否写文档 |
|------|------|-----------|
| `comp_crop_snap_rect` | 标准化负宽高 → 四边各自取整 → 宽高最小 1 | 否（纯几何） |
| `comp_crop_create_frame` | 由拖拽起止点成框；`ratio > 0` 约束比例，`symmetric` 以起点为中心展开 | 否 |
| `comp_crop_change_ratio` | 保宽、高度 = 宽 / 比例、垂直居中；取整后不合法则原样保留 | 否 |
| `comp_crop_is_valid` | 可提交性查询（先取整，与 apply 同一判据） | 否 |
| `comp_crop_apply` | 平移图层 + 换画布 + 丢弃选区，**一步撤销** | 是 |

纯几何函数只借 `comp_engine` 上报 `last_error`，不需要文档句柄。
`comp_crop_rect` 用 `double`（与上游 `CGFloat` 同宽），避免在 1e6 原点附近因
`float` 精度把边界挪掉半像素。

#### 语义逐条对齐

| 上游行为 | ng 实现 | 护栏 |
|----------|---------|------|
| `snapped`：四边各自 `round`（.5 远离零），宽高最小 1 | `std::llround`（同 Swift `.rounded()`），负宽高先标准化 | `crop_snap_rounds_each_edge_and_enforces_minimum_size` |
| `create`：比例修正向量（宽优先或高优先两分支） | 与上游同式，`ratio <= 0` = 无约束 | `crop_create_frame_follows_drag_ratio_and_symmetry` |
| Option 拖拽以起点为中心 | `symmetric` 分支等距展开 | 同上 |
| `changeCropRatio` 保持宽度、垂直居中；不合法不赋值 | `height = w / ratio`，中心不变；不合法回填原框 | `crop_change_ratio_keeps_width_and_centers_vertically` |
| `valid`：宽高 1..maxSide，`abs(x)`、`abs(y)` ≤ 1e6 | 另加本仓 1 亿像素画布预算 | `crop_is_valid_matches_commit_rules` |
| `commitCrop` → `CanvasResizer` 平移图层、换画布、**不裁像素** | `crop_apply` 平移 `transform.x/y`，不碰 `w/h` 与像素 | `crop_translates_layers_and_resizes_canvas`、`crop_is_non_destructive_and_restores_by_expanding_back` |
| 负原点裁剪扩大可视范围（画布外内容重新露出） | 平移后 origin 变负，合成时按画布边界裁剪 | `crop_to_negative_origin_extends_canvas_with_transparent_border` |
| 尺寸与偏移都没变 → 无操作 | 框恰为 `(0,0,w,h)` 时 `out_changed = 0`，不 commit | `crop_noop_does_not_add_history` |
| 换画布 = 换坐标系，选区不随行 | 提交丢弃 `SelectionState`，与裁剪同属一步撤销 | `crop_drops_selection_in_the_same_undo_step` |
| 非法框 / transform 溢出 → 文档不变 | 两遍扫描：先校验所有图层的新 transform，再统一写入 | `crop_rejects_invalid_inputs_without_side_effects` |

#### 事务与失败语义

- **参数闸先于事务**：句柄、输出指针、非有限坐标/比例、取整后合法性全部在
  `ImplicitTxn` 生效前或 `commit()` 前判完；失败路径不 commit、不改状态。
- **apply 内部先取整**：宿主可以直接传拖拽中的浮点框，不需要自己 snap；
  `is_valid` 同样先取整，因此它的判定与 apply 完全一致。
- **两遍写入**：裁剪把每个图层原点减去 rect 原点，极端情况下可能越过
  `is_valid_transform` 的 1e6 上限。第一遍只算新值并校验，第二遍才写入——
  拒绝时零副作用，不会留下「改了一半」的文档。
- **没变就不进历史**：`changed == false` 时不 commit（上游 `CanvasResizer`
  对尺寸与偏移都未变的请求同样短路）。
- **显式事务内**：`ImplicitTxn` 不占事务，`commit()` 被抑制，由外层事务产出
  恰好一步；abort 时整体回滚（护栏钉住撤销深度与像素）。

#### 同轮修正：Canvas Size 也丢选区

§6.7 只让 Image Size 丢弃选区，而 Canvas Size（`comp_document_set_size`）此前
保留选区——换画布同样换坐标系，旧分辨率的蒙版留下就是错位。上游
`applyDocumentSize` 对两者都重建 `CanvasDocument`，选区自然不存在。现在
`set_size` 只在**尺寸真的变化**时丢弃选区，并补了
`canvas_size_change_drops_selection` 护栏。

#### 护栏与基线

`tests/test_crop.cpp` 的 12 例覆盖：四边取整 / .5 远离零 / 最小 1 像素 / NaN 拒绝；
拖拽成框的正反向、比例两分支与 Option 对称；比例切换的保宽与垂直居中；
合法性三条上限；提交后的画布尺寸、图层平移与**子矩形逐字节相等**；负原点扩展
出的透明边；裁出去再扩回来的**非破坏性逐字节还原**；无操作不进历史；选区丢弃
与裁剪同属一步撤销；非法输入与 transform 溢出零副作用；显式事务的分组与 abort。

基线 304 → **316 cases, 0 failed**；`run_regression.py --require-ohos` 6/6 PASS
（构建 + 单元测试、Windows 壳 27 项、编解码交叉、`.comp` 双向、ctypes 真实调用、
OHOS aarch64 交叉编译）。详见
`../report/F2-crop-20261018-claude.md`。

#### 明示边界

- **手柄、蚂蚁线、吸附参考线属外壳**：ABI 只提供几何与提交。
- **不重采样**：裁剪不缩放像素；需要缩放请用 `comp_document_resize_image`。
- **选区丢弃而非缩放**：缩放 0/255 蒙版会把边缘糊成半选，上游同样不保留。
- **极负原点可能被拒**：本仓在写入侧拒绝 |origin| > 1e6 的 transform，
  这类裁剪返回 `COMP_ERR_INVALID_ARGUMENT` 且文档不变（比静默写入坏值安全）。

### 6.9 F3 仿制图章 / 涂抹 / 液化（2026-10-18，Claude）

**交付物**：`engine/domain/retouch.{h,cpp}`（93 / 362 行，纯像素算法）、
`ToolOpParams` 追加 `clone` / `warp` 两组参数、
`comp_tool_op` 追加 3 个 kind、`comp_tool_apply` 的派发与参数闸、
`tests/test_retouch.cpp`（10 例）。**没有新增导出符号**——F3 走既有的
`comp_tool_apply`，事务、撤销、`note`/`out_code` 与 16 位工作副本的既有契约原样继承。

#### 为什么 kind 从 9 起

域层 `ToolOpKind` 里的 7/8 已被 `scanlines` / `chunky` 占用，但这两个算子当时
没有进 ABI（宿主无入口）。F3 若「补齐枚举」把 CLONE 填成 7，同一个数值在域层
与 ABI 就会指向两个不同算子。ABI 与域层都从 **9** 起追加：

| ABI | 域层 | 含义 |
|-----|------|------|
| `COMP_TOOL_CLONE = 9` | `kToolOpClone` | 仿制图章 |
| `COMP_TOOL_SMUDGE = 10` | `kToolOpSmudge` | 涂抹 |
| `COMP_TOOL_LIQUIFY = 11` | `kToolOpLiquify` | 液化（Push） |

`tests/test_retouch.cpp` 用 `static_assert` 把这三个数值连同 `COMP_TOOL_DEEP == 6`
一起钉死；同时确认既有数值不变。

#### 语义逐条对齐

| 上游行为 | ng 实现 | 护栏 |
|----------|---------|------|
| `cloneStrokeOffset`：`offset = round(source - 首笔落点)` | `llround(source - points[0..1])`，整条折线共用一个偏移 | `retouch_clone_aligned_keeps_the_first_offset_and_unaligned_recomputes` |
| `aligned` 时沿用宿主带回的首笔偏移 | `aligned && has_offset` 直接用 `offset_x/y`；否则重算 | 同上 |
| `sampleAllLayers` 取整篇实时合成 | `composite_document` + 按图层包围盒原点搬进图层空间；HDR 合成先 `hdr_to_rgba8` | `retouch_clone_sample_all_layers_uses_the_composite_snapshot` |
| 一笔内既画又采样会吃到自己 | **开工前**取快照，整笔只读快照 | 同上（源像素在落笔后仍保持原值） |
| 没有源 → "Nothing to copy until a source is set" | `COMP_ERR_STATE`，且不进撤销栈 | `retouch_clone_without_source_is_a_state_error_and_leaves_no_history` |
| Smudge 首点只 `pickUp`，不落笔 | 首点读进 carried，dab 从第一个移动采样开始 | `retouch_smudge_single_point_only_picks_up_colour` |
| Smudge `painted = under + (carried - under) × w × strength` | 同一式；carried 在 dab 后同步为 painted | `retouch_smudge_drags_colour_across_a_boundary` |
| Smudge 间距 `max(1, diameter × 0.005)` | 同式 | 同上 |
| Liquify `move = (to - from) × strength`，从 dab 前像素重采样 | 每 dab 先复制 scratch 再双线性采样，避免自反馈 | `retouch_liquify_pushes_pixels_forward` |
| Liquify 间距 `max(1, diameter × 0.025)` | 同式 | 同上 |
| 笔刷权重 `u <= hardness ? 1 : smoothstep((1-u)/(1-hardness))`；hardness 上限 0.98 | 同一式，hardness 显式夹到 0.98 | 涂抹/液化两例 |

坐标口径与既有 Stroke 完全一致：ABI 收**文档像素**折点，引擎按
`effective_bounds()` 原点平移到**图层像素**后再落笔；不缩放、不旋转。

#### 参数闸与内存安全

闸仍在 `txn_begin` **之前**，拒绝路径不产生历史、不改像素：

- 折线 1..65536 点、指针非空；所有坐标必须有限且 `|v| <= 1e6`（否则
  `lround` 会 UB——这是新算子显式加的闸，既有 Stroke 未改）；
- Clone 要求 `has_source` 或 `aligned && has_offset`，否则 `COMP_ERR_STATE`；
  `pressure` / `flow` / `opacity` 必须落在 0..1；
- Smudge/Liquify 的 `strength` 必须有限且 0..1；
- 笔刷直径沿用 Stroke 的 8192 上限；warp 另设 **1024** 上限——carried/scratch
  是 O(diameter²) 个 float，8192 直径仅 carried 就约 1 GB。这是本仓显式收紧的
  一处上界（上游没有这个闸），越界返回 `COMP_ERR_INVALID_ARGUMENT`。

#### 与上游的两处显式分歧

1. **`strength = 0` 允许且逐字节不动**：上游把 opacity 夹到 0.01..1；本仓允许
   0，宿主可以用它做「强度 0 预览」，并在实现里短路成完全无操作。
   `retouch_warp_strength_zero_is_a_byte_identical_no_op` 钉住这一点。
2. **warp 直径上限 1024**（见上），属内存安全取舍，不是像素语义差异。

#### 护栏与基线

`tests/test_retouch.cpp` 的 10 例覆盖：仿制图章取样与源不变、aligned/非 aligned
的偏移语义、`sampleAllLayers` 与仅当前层的差异、缺源的 `COMP_ERR_STATE` 零副作用；
涂抹跨色块拖色、单点只拾色；液化把白色条纹向下游推移（`x=40` 变黑、`x=60` 变白、
`x=75` 不变）；`strength = 0` 逐字节恒等；非法直径 / 越界 strength / NaN 与 1e7 坐标 /
空指针全部在事务前拒绝；同一组参数的重复回放逐字节一致且恰好一步撤销。

基线 316 → **326 cases, 0 failed**；`run_regression.py` 6/6 PASS、0 skipped
（构建 + 单元测试、Windows 壳 27 项、编解码交叉、`.comp` 双向、ctypes 真实调用、
OHOS aarch64 交叉编译）。详见 `../report/F3-clone-smudge-liquify-20261018-claude.md`。

#### 明示边界

- **`sampleAllLayers` 只按包围盒原点映射**：图层的 scale / rotation 不参与采样
  坐标换算，与既有 Stroke 的边界相同；1:1 无变换图层逐像素精确。
- **源越界跳过**：仿制图章的源点落在图层外时不落笔（不取透明），否则会在图层
  上擦出洞，与「没有可复制的像素」语义不同。
- **16 位 / HDR**：16 位图层与其他 8 位工具一样先走 `use_rgba8_working_copy`；
  HDR 图层明确拒绝（8 位工具无法保真，等 S4 的 tile+coverage 模型）。
- **Blur 模式未做**：上游 `BlurToolMode` 的 liquify / smudge 已交付，同一枚举里的
  `blur` 仍是零通路。
- **无 UI**：交互（笔刷光标、源点十字、realignment 快捷键）属外壳职责。

### 6.10 F4 Camera Raw（2026-10-18，Claude）

**交付物**：`engine/domain/camera_raw.{h,cpp}`（278 / 1270 行：设置结构、JSON、
曲线、管线顺序、文档事务）、`engine/domain/camera_raw_tables.{h,cpp}`（110 / 233 行：
CRT4 解码、取表、stage 装配、表合成、亮度测量）、
`engine/abi/src/comp_camera_raw_abi.cpp` + `comp_camera_raw.h`（3 个导出）、
`tools/convert_camera_raw_tables.py`（67 行离线转换）、
`resources/camera_raw_tables.bin`（1,779,985 字节）、
`tests/test_camera_raw.cpp`（15 例）与 `docs/camera-raw.md`。
引擎新增 `set_camera_raw_tables` / `camera_raw_tables()` / `camera_raw_tables_loaded()`，
表是 `shared_ptr<const>` 的引擎级只读资源，所有文档复用。

#### 表：为什么必须加载

763 张 17³ 色彩表是 Photoshop **实测数据**，不是可重算的公式。缺表时上游整条滤镜
不可用，本仓同样返回 `COMP_ERR_STATE`，不降级成另一套观感。上游用 CRT3（10 字节头
+ **xz**），本仓不引入 liblzma，离线转成 CRT4（同布局 + **zlib**）：

```
"CRT4" | u16 size(=17) | u16 count(=763) | u16 step(≠0) | zlib(763×17³×3 差分字节)
```

解码按通道维护 `uint8_t` running 和：`value = level[axis] + int8(running) × step`，
`level[axis] = round(axis × 255 / 16)`，再夹到 0..255。加载失败时**旧表原样保留**——
一次坏 blob 不会让已打开的文档失去 Camera Raw；没有 zlib 的构建单独报
`COMP_ERR_UNSUPPORTED`，不冒充数据损坏。

#### JSON：严格拒绝 + 上游结构修复

字段名与上游 Codable 逐字一致（camelCase），缺省值取上游缺省。解析复用了
`engine/format/comp_json.h`，但把「调用方算错」与「上游自身的修复」分开：

| 输入 | 本仓 | 上游 `normalized()` |
|------|------|---------------------|
| 未知键（根或任意嵌套） | 拒绝 | 忽略 |
| 类型不符 / NaN / Inf | 拒绝 | 部分兜底 |
| 标量越界 | **拒绝**，不静默 clamp | clamp |
| 曲线点乱序 / 端点缺失 / 间距过小 | 照上游修复 | 修复 |
| 点颜色 > 8 个 | 截断 | 截断 |
| 去紫 / 去绿色相端点写反 | 对调 | 对调 |

理由：静默 clamp 会把「宿主算错参数」变成「看起来能用但结果不对」，而结构修复是
上游定义的产品语义，必须保留。`geometry` 能解析、能回存，但非默认时提交返回
`COMP_ERR_UNSUPPORTED`（透视变换属 S4）。

#### 管线顺序（逐字对齐上游 `CameraRaw.apply`）

```
normalize → 恒等即返回 → 几何非默认 → UNSUPPORTED → 缺表 → STATE
  → 亮度测量（仅 contrast/highlights/shadows 需要，在校准之前）
  → 校准 shadowTint
  → 实测表 stages：校准主色 → 曝光 → 白平衡 → 对比度/高光/阴影
                  → 白/黑/饱和/自然饱和 → 参数曲线
  → foldingColor（仅 foldsColor && colorAdjusts 时折进合成表）
  → adjust_camera_raw → applyCurveColor → effects+颗粒 → optics → detail
```

通路判定：`foldsColor = points 为空 && refineSaturation == 0`；
`colorAdjusts = curve.adjusts || mixer.adjusts || grading.adjusts`；
`paintColor = !foldsColor && colorAdjusts`；
`paintEffects = adjustsEffects`；`paintDetailOptics = detail.adjusts || optics.adjusts`。
note 回报实际通路（identity / folded / per-pixel）。颗粒
`kernel = 0.5 + grainSize/100 × 19.5`、`unitsPerPixel = 1/max(scale,1)`，同一 `seed`
逐字节可复现；光学 `distortion_k = 0.35`。

#### 事务与 ABI

领域层自己 `txn_begin("Camera Raw")` / `txn_commit`，整次应用**恰好一步撤销**；
ABI 层不再包 `ImplicitTxn`（否则叠出两条历史）。恒等设置不动像素、不开事务，
`out_changed = 0`。事务内 `bad_alloc` 会先回滚再重抛，不让活动事务逃进 ABI 的
异常护栏。16 位图层先落 8 位工作副本（与工具层同一约定），HDR / 其它格式 →
`COMP_ERR_STATE`。`note` 是两段式拷贝：容量 0 只回报所需长度。

#### 与上游的显式分歧 / 边界

1. **越界标量拒绝**（见上），上游 clamp。
2. **几何 / Upright 未移植**：需要 Core Image 类透视变换与重采样，属 S4。
3. **预览专用参数未暴露**：`clipping` / `sharpenMask` / `visualizePointColor` 会把
   预览效果烘进图层像素；`visualize` 可解析但不生效。
4. **无 RAW 解码**：本接口处理已解码的图层像素；相机原始格式属编解码层。
5. **无 GPU 通路**：CPU 内核；`camera_raw_lookup_is_valid` / `repair_lookup` 已随
   AdjustPixels 移植，但 F4 不走 Metal。
6. **无 UI**：面板、曲线编辑器、颜色轮属外壳职责。

#### 护栏与基线

`tests/test_camera_raw.cpp` 的 15 例覆盖：CRT4 加载/查询/坏 blob 与「失败不破坏旧表」、
恒等设置零副作用、曝光提亮 + 恰好一步撤销、JSON 严格性、非默认几何、缺表、缺图层、
folded / per-pixel 两条通路、note 两段式拷贝、颗粒 seed 确定性、细节与光学改像素、
16 位落 8 位、透明像素保持、曲线求值器边界。像素公式本身由
`tests/test_camera_raw_align.cpp`（8 例）钉住上游 1.5.3。

基线 326 → **341 cases, 0 failed**；`run_regression.py` 6/6 PASS、0 skipped。
完整接口与键表见 [`docs/camera-raw.md`](docs/camera-raw.md)，报告见
`../report/F4-camera-raw-20261018-claude.md`。

### 6.11 F5 Windows 壳 UI 地基（2026-10-18，Claude）

**背景**：上游 `Compositor` 只有 macOS 应用层（SwiftUI），**没有 Windows 版**
（`Compositor.xcodeproj` 仅 macOS 目标，无 CMake / Win 目标 / 平台抽象层），因此
Windows 壳无法「参考」任何现成 UI 代码，只能自建。栈的选择沿用既定结论：
**零外部依赖的 Win32 + GDI**（不加 Qt/Electron，不为 UI 引依赖），代价是 UI 迭代比
声明式框架慢。

**交付物**（两层，均不含像素处理）：

| 文件 | 行数 | 职责 |
|------|------|------|
| `shells/windows/src/ui_core.h` | 358 | 平台无关 UI 核心：视图变换 / 布局 / 命中 / 工具表。**不 include 任何 Windows 头、不 include `engine/` 头、不分配内存、不抛异常** |
| `tests/test_ui_core.cpp` | 335 | 8 例回归护栏，随 `scripts/build.py` 一起跑，无窗口也能判定 |
| `shells/windows/src/main.cpp` | 改造 | 只做「消息 → ui_core 调用 → 绘制」的翻译；绘制与命中共用同一套几何常量 |

`ui_core.h` 是 header-only：`build.py` / `CMakeLists.txt` **一行未改**，
测试直接 `#include "../shells/windows/src/ui_core.h"`。

#### 视图变换的锚点口径

统一用「文档中心」为缩放锚点，而不是用户滚动位置：

```
screen = canvas_center + pan + (doc - doc_center) * zoom
```

好处是 `pan = 0` 时无论缩放级别，文档恒在画布正中（`fit` 与 `actual_pixels`
都只需清零 pan）。以光标为锚的缩放走另一条公式：

```
ratio   = new_zoom / zoom          // new_zoom 先夹到 [0.02, 32]
pan     = pan * ratio + (cursor - center) * (1 - ratio)
```

**先夹取再计算**，所以「已在 32× 还继续放大」不会偷偷平移画布——这一条与
「夹取后光标下的文档像素不漂移」都写成了断言（4 组锚点 × 4 档目标缩放）。

#### 键位冲突的处理（对齐上游 `KeyboardShortcuts.swift`）

工具快捷键逐字对齐上游的 17 个默认键，因此挤掉了壳里原来的三处单键：

| 原键 | 现在 | 理由 |
|------|------|------|
| `V` 切换图层可见性 | `V` = Move 工具；可见性改 `Ctrl+Shift+V` | 上游 `V` 是 Move/Transform |
| `[` / `]` 图层不透明度 | `[` / `]` = 笔刷直径、硬度（`Shift`）；不透明度改 `Ctrl+[` / `Ctrl+]` | 上游 `[ ]` 属笔刷 |
| `N` / `O` / `S` 新建/打开/保存 | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | `S` 被 Clone Stamp 占用 |

工具表、工具条按钮、工具菜单、快捷键**共用同一张 `tool_table`**，不存在「菜单提示与
实际行为不一致」的第三份键位表；测试同时钉住「默认键不重复」与「未占用键返回
`ToolId::Count` 而不是静默回落到第一个工具」。

#### 壳侧接线

- **工具条**：17 个按钮（`tool_button_rect`/`tool_button_at`），选中态高亮，右侧显示缩放与笔刷读数；
- **鼠标**：左键命中顺序 工具条 → 图层面板 → 画布；图层面板行命中与行矩形共用
  `layer_row_rect`，绘制/命中的边界不会各算各的；
- **滚轮**：在画布内以光标为锚 ×1.2 / ÷1.2，`1e9` 目标被夹到 32×；
- **平移**：中键拖动，或按住空格 + 左键拖动（`Hand` 工具同理）；`WM_KEYUP`/失焦都会结束平移，不会「粘住」；
- **视图命令**：`Ctrl+0` 适合窗口、`Ctrl+1` 实际像素、`Ctrl+=` / `Ctrl+-` 缩放；
- **窗口变窄**：右侧调整面板先收起，再收起左侧图层面板，画布保底 180 px；11 种宽度 × 8 种高度下断言「任意矩形不重叠」且「三块横向正好铺满客户区」；
- **画布上屏**：先按 `layout.canvas` 求交，再由裁剪后的目标矩形反推源子矩形，最后 `StretchDIBits`——32× 下不会产生数十万像素的目标矩形。

**明确没做的**（不是漏掉，是当前 ABI 不够）：Move 工具的**拖拽平移图层**需要图层
transform 写入侧与像素回读的配合，现有 `comp_layer_*` 只暴露尺寸/不透明度/可见性等，
无法在不猜语义的前提下实现；工具条按钮也仍是纯文字（无图标字体/位图资源，避免引入
资源文件）。二者记在 `docs/shell-parity.md` 的缺口表里。

基线 344 → **352 cases, 0 failed**（8 例新增）；Windows 壳实机自检仍
**27 项全 OK**，SHA256 与仓内基线**逐字节相同**（本轮只改 UI 层，未触碰 `run_selftest()`）；
`run_regression.py --require-ohos` **6/6 PASS，0 skipped**。

### 6.12 P1 Filter 菜单 + Blur 工具（2026-10-18，Claude）

**1. 为什么这批不是「新算法」。** Vignette / Tonal Contrast / Bloom / Lens Correction
的像素内核在 `engine/imaging` 早已就位（`adjust_colored_vignette`、
`adjust_tonal_contrast`、`lens_distort`；Bloom 复用 `blur_rgba8_copy`）。缺的是与
`comp_tool_apply` 同级的**产品入口**：参数契约、图层几何装配、一步撤销、选区混合。
本轮新增 `comp_filter_apply`（`engine/abi/src/comp_filter_abi.cpp`）与领域层
`engine/domain/filters.{h,cpp}`，四个算子一次收口。

**2. 口径。** 参数量程对齐上游 `FilterSettings.normalized`（Vignette / Bloom 的
`amount` 0..100、Tonal 三个 gain ±100、Lens `distortion` ±100）；**越界拒绝而不是
clamp**，闸在 `txn_begin` 之前；恒等设置不碰像素、不开事务。Vignette 作用在图层
自身像素矩形（不按画布铺底、不改 alpha）；Tonal Contrast 先用 `radius * scale`
高斯得到 base，再比较局部细节；Lens 的 `k = distortion / 100 * 0.35`
（上游 `lensStrength`，`Filters.swift:238`），与 Lens 工具共用同一个
`lens_distort`，回归护栏直接比对两条通路的逐字节结果。

**3. Bloom 的显式近似。** `CIBloom` 内部不公开。本仓定义为：rec.709 亮度在
`[0.5, 1]` 线性起权提取亮部 → 预乘高斯 → `rgb += glow * (amount / 50)` → 裁到自身
alpha。**只加光、不改 alpha**，透明像素不会凭空发光；note 如实写明
`bright-pass approximation (threshold 0.5; CIBloom's curve is not public)`，
不假装与 Core Image 逐像素一致。

**4. 选区。** 有选区时按 coverage 与原像素混合（`PixelAdjust.blend` 口径），
coverage 是**文档分辨率**；图层像素的文档坐标 = `effective_bounds()` 原点 + 像素
坐标。显式空选区是 no-op 且 `out_changed = 0`。

**5. 16 位 / HDR。** RGBA16 先落 `deep_to_rgba8` 工作副本（之后图层就是 8 位，
note 写明），撤销恢复 16 位资产；RGBA16F 与组 / 无像素图层一律 `COMP_ERR_STATE`。
上游 Bloom 有 `runsAtSixteenBits`，本仓 8 位内核做不到逐位对齐，如实记入文档。

**6. Blur 工具。** `comp_tool_op_desc` **尾部追加** `blur.radius`（既有偏移不变），
`COMP_TOOL_BLUR = 12`。整层模糊样本在整笔开始前取好（同一笔不吃自己刚模糊的像素，
重复落笔才继续变软），按笔刷 coverage 在预乘空间盖回，**四个通道一起**（alpha 也变软）。
`radius` 0.5..50，`sigma` 再夹到较长边一半；`0` 被参数闸拒绝而不是当作恒等
（上游会把 0 抬到 0.5）。落笔族口径：16 位 / HDR 图层笔尖之外保留深像素。

**7. 护栏（+16 例，352 → 368）。**

| 文件 | 例数 | 钉住什么 |
|------|------|---------|
| `tests/test_filters.cpp` | 11 | 恒等 no-op；6 组越界参数在事务前拒绝；Vignette 压边角/中心不动/alpha 不变；Tonal 保留黑白格结构；Bloom 扩散、远角不动、alpha 全不变、`rgb <= alpha`；Lens 与工具逐字节一致；选区按文档坐标裁切；16 位工作副本与撤销恢复；组/空/HDR/未知 id 错误码；空选区 no-op；ABI `out_changed` 与 note 两段式 |
| `tests/test_tool_ops_domain.cpp` | 3 | Blur 只软化笔尖下、笔尖外逐字节不动；第二笔继续变软（快照按笔取）；radius 0 / 50.5 / NaN / 缺 points 在事务前拒绝；16 位笔尖外保留深像素 |
| `tests/test_tool_ops.cpp` | 2 | `COMP_TOOL_BLUR == 12` 可达；硬边两侧变软、alpha 不掉洞、一笔一步撤销；radius 0 拒绝且不留历史，同一 desc 改合法值即成功 |

**8. 验证。** `scripts/run_regression.py --require-ohos` **6/6 PASS，0 skipped**：
步骤 ① 368 cases / 0 failed；步骤 ② Windows 壳 27 项 `failures=0`、SHA256
`32151220f4748dfa…` 与基线一致；步骤 ⑤ 共享 DLL 真实加载；步骤 ⑥ OHOS aarch64
交叉编译 + NAPI rc=0。参数契约见 [`docs/filters.md`](docs/filters.md) 与
[`docs/tool-ops.md`](docs/tool-ops.md) §4.9。

**9. 仍不在本轮**：Remove Background（依赖 ML / 分割）、滤镜的实时预览缓存、
上游 Bloom 的 16 位逐位对齐。

### 6.13 P1 UI 接线：Filter 菜单 / 参数面板 + Blur 工具（2026-10-18，Claude）

**1. 分工没有变化。** 引擎侧 API 在 §6.12 已收口；本轮只做「壳把消息翻译成
`ui_core` 调用，再把结果画出来」。`ui_core.h` 仍然不 include Windows 头、
不 include `engine/` 头、不分配内存——滤镜表、面板状态机、行几何、retouch
循环都留在这一层，`main.cpp` 只负责 ABI 映射与 GDI 绘制。

**2. Filter 菜单与面板。**

- 菜单「滤镜(&F)」的 4 个条目由 `comp_ui::filter_table()` **动态生成**，菜单 id
  用 `kCmdFilterBase + 索引` 区间承接；「应用 / 重置参数 / 关闭面板」是固定项。
  新增滤镜只改 `ui_core.h` 一张表，菜单与面板同时跟上，不存在第三份清单。
- 选中滤镜时 `FilterPanel::select()` 把值重置为 **上游面板缺省**
  （Vignette midpoint 50 / roundness 100 / feather 60 / highlights 25；
  Tonal radius 16 / shadows 40 / midtones 60 / highlights 30；Bloom radius 24）。
  注意 ABI 的零初始化是**恒等**（amount 0），两者刻意不同：面板负责给
  「打开即有可见效果」的默认值，ABI 不替宿主决定缺省。
- `FilterPanel::values[]` → `comp_filter_desc` 的映射只写在 `build_filter_desc()`
  一处，顺序与面板行序一致；`FilterId` 与 `comp_filter_kind` 的数值对齐由
  `main.cpp` 的 `static_assert` 钉死（头文件之间不能互相 include）。
- 交互：`Tab` / `Shift+Tab` 换行（夹在 `[0, param_count-1]`，不环绕）、
  `-` / `=` 按 `spec.step` 调值、数字/`.`/`-` 键入后回车提交并应用、`Esc` 关闭。
  键入解析用 `strtod`，拒绝空串、尾部残留和非有限值；面板侧
  `clamp_filter_param` 保证永远不会向 ABI 送出越界参数（ABI 的规矩仍是
  **越界拒绝**，不是静默 clamp）。
- 一次 `comp_filter_apply` 调用完成：`note` 用固定 512 字节缓冲，**不做
  size-probe 两次调用**——该函数先执行后写 note，探长度会把滤镜应用两遍。
  返回 `changed == 0` 时如实显示「参数为恒等，未改动」，不伪造成功。

**3. Blur 工具。**

- `R` 在 Blur / Smudge / Liquify 之间循环（对齐上游 retouch 组）；只有 Blur
  已接内核，另外两个模式状态栏明确写「尚未接内核」，不假装能用。
- 半径 `Alt+[` / `Alt+]`（`WM_SYSKEYDOWN`）：`[`/`]` 已属笔刷直径、
  `Ctrl+[`/`]` 已属图层不透明度，Alt 组合是唯一不冲突且与上游手感接近的键位。
- 笔迹：`WM_LBUTTONDOWN` 转文档坐标后 `SetCapture`，`WM_MOUSEMOVE` 按
  `stroke_point_should_append`（0.75 px 欧氏距离去抖）追加，`WM_LBUTTONUP`
  把整条折线一次 `COMP_TOOL_BLUR` 提交。`WM_KILLFOCUS` / `WM_CAPTURECHANGED`
  取消未完成笔迹，不会留下「粘住」的画笔。
- 半径量程 0.5..50 与 `comp_tool_blur.radius` 同口径；工具条与状态栏显示
  `Retouch / <模式> / 半径 N px`。

**4. 自检（`--selftest`）。** 新增 14 项，全部只走 C ABI：恒等滤镜是**真 no-op**
（字节、事务、历史深度都不动）、越界 amount 在事务前拒绝、Vignette 改变展平输出、
一步撤销逐字节还原；棋盘图上 `COMP_TOOL_BLUR` 沿笔迹改变像素、撤销逐字节还原、
`radius = 0` 拒绝。仍有一个易踩的观测前提：Vignette 在 `fillsClear = 0` 下逐像素
跳过全透明像素，因此自检必须先铺一张**不透明**图（与 `tests/test_filters.cpp`
同口径），空图层上「成功但字节不变」是正确行为。

**5. 验证。** `scripts/run_regression.py --require-ohos` **6/6 PASS，0 skipped**：
步骤 ① **373 cases / 0 failed**（新增 5 例 `test_ui_core`）；步骤 ② Windows 壳
**41 项** `failures=0`、SHA256 `739f4e7c20257af9…` 与仓内基线逐字节一致
（基线已随自检项同步更新）；步骤 ⑤ 共享 DLL 真实加载；步骤 ⑥ OHOS aarch64
交叉编译 + NAPI rc=0。

**6. 如实说明的边界。** 本轮所有 Win32 交互路径（菜单点击、参数面板键盘、
真实鼠标落笔）**没有经过人手实测**——无窗口环境下只能验证到 ABI 层与
`ui_core` 数学层；`tests/test_ui_core.cpp` 钉住的是几何与状态机，不是
「画得对不对」。Vignette 的边缘色选择器、Smudge / Liquify 的壳层接线、
滤镜预览缓存仍不在本轮。

报告：`../report/P1b-ui-filter-blur-20261018-claude.md`。

## 7. 许可

上游 `robbietilton/Compositor` 为 MIT 许可；本仓沿用 MIT，并保留上游版权声明。
`engine/imaging/` 下的像素内核移植自上游 C 源码，文件头保留原始注释。
