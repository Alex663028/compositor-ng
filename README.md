# compositor-ng

Compositor 跨平台统一架构的**引擎核**实现（单仓）。同一份 C++17 引擎核 + 稳定 C ABI，
供 Windows 壳（D3D12）与 HarmonyOS 壳（Vulkan）共同调用。

- 上游参考工程：`robbietilton/Compositor`（macOS / Swift + Metal + CoreImage），MIT。
- 本仓不是上游的 fork，而是按《Compositor 跨平台统一架构评估报告》第八章路线**重写**的引擎核。
  当前进度：**M1（内核奠基）完成并验证通过**；**M2 完成**——像素内核区间并行调度、合成有向图
  （FilterGraph）与图层组穿透/隔离语义、仿射变换与高质量重采样、`.comp` 像素改 PNG + UUID
  图层 id（打通上游互通）均已落地。**M3 进行中**——渲染后端接口切片（`rhi.h` + CPU/D3D12/Vulkan
  三后端）、SPIR-V 构建链路、Vulkan 计算管线设备侧实现（opt-in）、
  **D3D12 离屏渲染真机接入主路径（S13：设备真的参与合成；S16：DXGI 交换链直呈已接；
  S19：逐层纹理跨帧常驻，1024²/4 层窗口直呈 3.04–3.27 → 1.41 ms；
  S20：烘焙单元也按子树结构键缓存，CPU 烘焙与纹理上传都能跨帧省掉）**、
  Windows 壳（Win32 + GDI 上屏、菜单/快捷键、图层面板、调整参数面板、撤销接线）与 HarmonyOS 壳
  （NAPI + XComponent）均已有可编译产物；另外落地了 **M5 编解码**（能力登记表 + PNG/JPEG 真实导入导出、
  PSD 当时只读（2026-10-23 起改为树内自研的分层读写，见 §6.87）；第三方库以源码 vendoring 进 `engine/third_party/`）。
  全量 **654 用例 0 失败**；Windows 外壳实机自检 **263 项全 OK**（与仓内基线逐字节一致）。
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
  2026-10-18 第十四轮（S4 外壳交互补齐）：`ui_core` 补选区边界折线、虚线相位、
  笔尖半径与 retouch 强度夹取；Windows 壳接上选区蚂蚁线（`Ctrl+A` / `Ctrl+D` /
  `Ctrl+Shift+I`、`M` 框选）、落笔预览线、Smudge / Liquify 落笔。基线 373 →
  **377 用例**，Windows 壳自检 41 → **59 项**，6/6 回归全绿。详见 §6.14。
  2026-10-18 第十五轮（S5 选区运算 / 像素搬移 / 橡皮擦 / 吸管）：新增
  `comp_selection_load_layer` / `comp_selection_translate` /
  `comp_selection_move_pixels` / `comp_eyedropper_sample` 与
  `comp_tool_op_desc.erase`——图层像素/蒙版载入为选区、平移轮廓、按 coverage
  剪切/复制选区像素（含扩层与蒙版同步）、预乘 destination-out 擦除、预乘平均后
  反预乘的取色。基线 377 → **391 用例**（14 例护栏），口径 B **27/48 ≈ 56%**；
  外壳尚未接线取色/前景色/拖动选区，详见 §6.15。
  2026-10-18 第十六轮（S6 外壳接线：前景色 / 吸管 / `E` 橡皮擦 / 拖动选区）：`ui_core`
  补前景色模型（RGB↔HSV、`#RRGGBB` 解析与格式化、12 色板）与 `E` 画笔/橡皮擦切换；
  Windows 壳接上吸管（`I`，Alt = 合并源，读数含 HSV 与不透明度）、画笔/橡皮擦共用
  `COMP_TOOL_STROKE`（`desc.erase`）、`V` 拖拽搬移/复制选区像素（像素搬移 + 轮廓平移
  同一个显式事务 = 一步撤销）、状态栏前景色块与 `Shift+X` 十六进制键入。自检还暴露出
  一处内核缺陷：单点折线（点一下不移动）被参数闸接受却不落笔——已修为圆形 dab。
  基线 391 → **397 用例**，Windows 壳自检 59 → **71 项**，口径 B **27/48 ≈ 56%** →
  **28/48 ≈ 58%**。详见 §6.16。
  2026-10-18 第十七轮（S7 选区工具补口：椭圆框选 / 套索 / 魔棒 / 新选区羽化）：新增
  `comp_selection_apply_wand`（第 111 个导出 ABI；只写选区、不碰图层蒙版，与
  `COMP_TOOL_SELECT` 的破坏性写入明确分工），`ui_core` 补框选形状循环、套索点数闸、
  容差/羽化步进；Windows 壳接上 `M` 循环 矩形/椭圆、套索拖拽（≥3 点自动闭合，
  Shift/Alt = 加/减选）、魔棒点选（`[`/`]` 调容差、`Shift+W` 切图层/合并取样）、
  `Shift+F` 羽化键入，形状 + 羽化包在**一个显式事务**里（一步撤销、无变化不留空撤销步）。
  基线 397 → **408 用例**，Windows 壳自检 71 → **90 项**；口径 B 保持 **28/48 ≈ 58%**
  （S7 补的是已计数的「选区几何」条目内的 Marquee/Lasso/Magic Wand 子项，未凑整抬高条数）。
  详见 §6.17。
  2026-10-18 第十八轮（S8 选区工具收尾：钢笔贝塞尔 / 绝对羽化 / 魔棒取样窗口）：
  新增 `comp_selection_set_feather`（第 112 个导出 ABI，**绝对覆盖**语义；S7 的
  `COMP_SELECTION_FEATHER` 仍是 `sqrt` 叠加，两者不混用），并复用它修掉一个 S7 缺陷——
  魔棒点空（0 匹配）时不再因为默认羽化而改写既有选区；`ui_core` 补锚点/贝塞尔段/
  预览折线数学与魔棒半径步进（0/1/2/4…512）；Windows 壳接上 `P` 钢笔（点击落锚点、
  拖动拉对称控制柄、点首锚点或 `Enter` 闭合提交为 `COMP_SELECTION_PATH`、`Esc` 取消、
  `Shift`/`Alt` 加/减选、切工具自动放弃未提交路径）与 `Alt+[` / `Alt+]` 魔棒
  N×N 取样窗口（0 = 只取种子像素，状态栏显示 `(2r+1)×(2r+1)`）。基线 408 →
  **414 用例**，Windows 壳自检 90 → **103 项**；口径 B 保持 **28/48 ≈ 58%**
  （钢笔是既有选区几何的可视化入口，不新增计分项）。详见 §6.18。
  2026-10-18 第十九轮（S9 渐变工具：`COMP_TOOL_GRADIENT = 13`）：
  在 `comp_tool_apply` 家族尾部追加**整幅渐变算子**（线性 / 径向、两个直通 RGBA 色标、
  不透明度、选区 coverage 混合；参数闸全在 `txn_begin` 之前，端点重合 / 双色标全透明 /
  opacity 0 一律 `COMP_ERR_INVALID_ARGUMENT`）；`comp_tool_op_desc.gradient` 是尾部追加
  字段，导出 ABI 仍是 **112** 个。Windows 壳接上 `G` 渐变（再按 `G` 切线性/径向、
  拖拽两端点松开提交、色标固定「前景 → 透明」、`Shift` 反向、失焦取消预览）。
  基线 414 → **418 用例**，Windows 壳自检 103 → **113 项**；口径 B 的
  Type/Shape/Gradient 一行由「零」改为「部分：Gradient 已交付」，计分仍 **28/48 ≈ 58%**
  （Type/Shape 仍是零，凑不满一整条）。详见 §6.19。
  2026-10-18 第二十轮（S10 形状工具：`COMP_TOOL_SHAPE = 14`）：把路径基元从
  `selection.cpp` 抽成中立的 `domain/path_raster.{h,cpp}`（行为中立，仅改名接线），
  再新增 `domain/shape_raster.{h,cpp}`（矩形 / 椭圆 / 三次贝塞尔路径的填充 + 胶囊距离
  描边 + 圆角矩形 + 窗口化覆盖率），在 `comp_tool_apply` 家族尾部追加**整幅形状算子**
  （填充与描边两个独立开关、先填后描、选区 coverage 混合；参数闸全在 `txn_begin` 之前，
  「填充直线」/ 构造退化 / 不可见色一律 `COMP_ERR_INVALID_ARGUMENT`，形状完全落在图层外
  则成功但**不留空撤销步**）；`comp_tool_op_desc.shape` 是尾部追加字段，导出 ABI 仍是
  **112** 个。Windows 壳接上 `U` 形状（再按 `U` 循环矩形/椭圆/直线、拖拽对角点或两端点、
  `Shift` 正方/正圆、`Alt` 从中心、`Alt+[ / ]` 调线宽、`Shift+Alt+[ / ]` 调圆角、
  失焦取消预览）。基线 418 → **424 用例**，Windows 壳自检 113 → **137 项**；口径 B 的
  Type/Shape/Gradient 一行改为「部分：Shape/Gradient 已交付」，计分仍 **28/48 ≈ 58%**
  （Type 仍是零）。详见 §6.20。

  2026-10-18 第二十一轮（S11 文字工具：`COMP_TOOL_TYPE = 15`）：新增零依赖的
  `domain/type_raster.{h,cpp}`（内置 5×7 列主序 ASCII 点阵 95 字形、字号/行距/字距排版、
  精确面积覆盖率 AA 与像素中心硬边两档；UTF-8/Tab/控制字节如实拒绝，不画方框顶替），
  在 `comp_tool_apply` 家族尾部追加**整幅文字算子**（参数闸全在 `txn_begin` 之前；
  「文本框在图层外」与「一个像素都没落墨」都是成功 no-op、不留空撤销步）；
  `comp_tool_op_desc.type` 是尾部追加字段，导出 ABI 仍是 **112** 个。Windows 壳接上
  `T` 文字（画布点击落插入点、键入 ASCII、`Shift+Enter` 换行、回车提交、`Esc`/失焦/
  切工具取消、`[ / ]` 调字号、`Shift+[ / ]` 调字距，预览只画插入点与文本框）。
  基线 424 → **431 用例**，Windows 壳自检 137 → **150 项**；口径 B 的
  Type/Shape/Gradient 一行三个子项全部交付 → **29/48 ≈ 60%**（Type 为 ASCII 点阵，
  边界见 §6.21）。详见 §6.21。

  2026-10-18 第二十二轮（S12 合成缓存 MERGED：`composite_document_cached`）：把「每次
  渲染都整篇重算」改成**文档级画布缓存**。`Document` 增加派生状态（内容序列号 +
  `ImageBuffer` 缓存槽 + 有效标记/键），任何可能改变合成结果的写入都让序列号 +1
  （事务提交/回滚/隐式提交、撤销/重做、图层增删挪、合并/拼合/改画布、显式
  `mark_dirty(true)`）；`compositing/compositor.h` 新增 `composite_document_cached`，
  命中时零合成、零像素拷贝，直接返回文档持有的只读视图；活跃事务期间**绝不供缓存**
  （中间态照常求值但不入缓存键）。`CpuRenderBackend::render_document` 改走该入口再做
  深度转换，拼合（Flatten）复用缓存并把像素 `shared_ptr` 直接接管（后续写入走 COW，
  不复制整幅画布）。**不新增导出 ABI**（仍 **112** 个）、不改任何 ABI 结构体与工具算子。
  基线 431 → **435 用例**（新增 `tests/test_composite_cache.cpp` 4 例），Windows 壳自检
  150 → **155 项**；口径 B 计分不变 **29/48 ≈ 60%**（内部实现不计分）。详见 §6.22。

  2026-10-19 第二十三轮（S17 GPU 子集按单元拆细 + 逐层纹理缓存修订号设计）：S13/S16 的
  能力闸门是**整篇布尔**——文档里只要有一层带蒙版/效果/组/剪贴/调整/非全画布，就整篇回落
  CPU。S17 把它拆成**按合成单元**：整幅 RGBA8、恒等几何的层直接上传（原生单元）；裁剪/
  偏移、蒙版、效果、旋转翻转缩放、16 位源图层、隔离组、剪贴栈在 CPU 上用**与 CPU 合成器
  相同的函数**烘焙成画布尺寸的预乘 RGBA8（烘焙单元），GPU 再按原混合模式混合。新增内部
  入口 `bake_layer_contribution`（`compositing/composite_ops`）、`bake_filter_unit_surface`
  （`compositing/filter_graph`，隔离组/剪贴栈）与 `render/gpu_plan.{h,cpp}`（`build_gpu_plan`
  分类 + 统计）；`d3d12_backend` 的整篇闸门删除，改为遍历单元上传。**不新增导出 ABI**
  （仍 **112** 个）。精度口径：原生单元与 CPU 基准 ≤1 LSB；烘焙单元把「蒙版 x 不透明度 x
  效果」量化进 8 位预乘色，最多 **2 LSB**（测试按 2 断言并写明原因）。仍整篇回落：调整层
  （作用于已合成画布）、HDR（RGBA16F）表面。同时交付**逐层纹理缓存修订号设计稿**
  `docs/gpu-layer-revision.md`（只设计不实施：`Layer` 无修订号、`ImageBuffer` 是
  `shared_ptr` + COW，原地写会让缓存静默过期，故先定 `content_revision` + 分配点 + 缓存键
  + 验收口径）。基线 447 → **451 用例**（+4）；详见 §6.27。

  2026-10-19 第二十四轮（S18 逐层内容修订号 R1 落地）：S17 的设计稿只差实施。这一轮把
  **R1** 做完——`Layer::content_revision`（文档内唯一、单调、同值⟺同内容、不进 `.comp`）+
  `Document::allocate_revision()` / `touch_layer()` / `touch_all_layers()`，并在**所有**能改
  图层内容的入口换号：14 个 `comp_layer_*` 写入口、`selection_move_pixels`（changed 时）、
  `crop_apply` / `document_set_size`（整篇）、`create_layer` / `merge_down` / `flatten_image` /
  `resize_image` / `load_project`，以及三个**领域自管事务**的入口（`run_tool_op`、
  `filter_apply`、`camera_raw_apply`）。换号一律发生在 `txn_commit()` **之前**——否则重做会
  带着编辑后的内容却拿到编辑前的号，R2 的缓存会静默供应旧纹理；这条由「笔刷 → 撤销 → 重做」
  用例直接钉住。纯结构变更（增删/排序/选中/剪贴源）与 undo/redo 刻意不换号。**不新增导出
  ABI**（仍 **112**），不动 `.comp`，不动 D3D12。基线 451 → **457 用例**（+6，新的
  `tests/test_layer_revision.cpp`）；**不宣称加速**（本轮还没有缓存）。详见 §6.28。

  2026-10-19 第二十五轮（S19 D3D12 持久逐层纹理 R2 落地 + 真机基准复测）：把 S17 设计稿的
  **R2** 做完。`d3d12_backend` 新增**持久逐层纹理缓存**：键 = `(文档实例号, 图层 id,
  content_revision, 画布宽高)`，只缓存原生单元，512 MiB 预算 LRU 淘汰；活跃事务期间整段
  旁路（不读不写）；换文档/换画布尺寸/设备丢失整体丢弃。为此给 `Document` 加了**进程内唯一、
  不序列化**的 `instance_serial()`——`comp_layer_id` 与修订号都只是文档内唯一，同一后端先后
  渲染两个文档时键会撞车（反向注入实验确认过：去掉这层判断会静默贴上一张文档的纹理，
  画面 max_diff 222）。同时补两处 GPU 侧硬约束：**描述符按单元成对分配**（命令录制后统一
  执行，共用一张表会让所有 dispatch 读到同一张源纹理）+ **相邻 dispatch 之间显式 UAV 屏障**
  （缓存命中后源纹理不再逐帧过渡，写→读顺序必须声明）。新增 `texture_uploads()` 计数
  （仅测试/诊断，不进 C ABI）。**真机基准**（1024²、4 层、`--present-bench=60`，连跑两次）：
  窗口直呈 3.04–3.27 → **1.41 ms**，相对 CPU flatten+GDI 从 1.18–1.29× 扩到 **1.58–1.60×**。
  **不新增导出 ABI**（仍 **112**）、`.comp` 未变、壳/鸿蒙代码零改动、壳自检 164 项逐字节不变。
  基线 457 → **462 用例**（+5）。详见 §6.29。

  2026-10-19 第二十六轮（S20 烘焙单元缓存 v2 落地）：S19 只缓存原生单元，裁剪/蒙版/效果/
  旋转翻转/16 位源层/隔离组/剪贴栈这些**烘焙单元**仍每帧在 CPU 上重烘焙、再重传纹理。这一轮
  把设计稿的 **v2** 做完：一个 64 位 FNV-1a 结构键同时服务**两层**缓存——CPU 烘焙表面缓存
  `GpuBakeCache`（存预乘画布单元，512 MiB 预算 LRU，浅拷贝 COW）与 D3D12 烘焙纹理缓存
  `baked_textures`。键 = 文档实例号 + 画布尺寸 + 累积不透明度 + **有序 DFS 子树结构签名**
  （深度/节点类型/isolated/图层 id/每层 content_revision/子节点数）+ 用途标签。深度进键是
  关键：`reorder`/换父级/改剪贴基底**不发修订号**，但会改变成员顺序或深度，因此键必然改变
  （测试用「交换兄弟 → 必须重烘焙」钉住）。活跃事务期间 CPU 与 GPU 两层缓存整段旁路；
  换文档/换画布/设备丢失整体丢弃。新增 `bake_cache_hits()/misses()` 诊断计数（不进 C ABI）。
  **不新增导出 ABI**（仍 **112**）、`.comp` 未变、壳/鸿蒙代码零改动、壳自检 164 项不变。
  基线 462 → **470 用例**（+8）。详见 §6.30。

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
│   ├── visual_check.py            # ★ Windows 壳自动化目视（真消息驱动 + PrintWindow 像素断言，S15）
│   └── once/                      # 一次性补丁 / 诊断脚本（P2-7：已从 scripts/ 根移入，非构建入口）
├── docs/
│   ├── adjustment-json-keys.md    # ★ 调整参数 JSON 键名表（对齐上游 $LayerAdjustment）
│   ├── codec-migration.md         # ★ M5 编解码替换：探测证据、能力矩阵与接入步骤
│   ├── format-import-matrix.md    # ★ 格式导入策略矩阵：HEIC/WebP/SVG/PSB 判定链与失败不变式
│   ├── manual-test.md             # ★ Windows 壳人工目视清单（M1–M16，与 visual_check.py 互补，S15）
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
│       │   ├── comp_io.h          # 导入导出（PNG/JPEG 走第三方库，PSD 走树内分层读写器）与 .comp（内层像素为 PNG）
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
| 编解码 | **PNG 与 JPEG 已接入真实第三方库并真实可用**：`comp_io_export`（拍平 → 落盘）与 `comp_io_import` / `comp_io_import_layer`（文件 → 文档 / 追加图层）已打通；PNG 走 libpng + zlib、JPEG 走 stb（有损，`{"quality":N}`）、PSD 走树内自研的分层读写器 `format/psd_codec`（§6.87：`comp_io_import_document` / `comp_io_export_document` 分层往返；拍平的 `comp_io_export` 仍不支持 PSD）。**HDR 来源（C3，§6.1.17）**：Radiance `.hdr`/`.pic` 走 stb 的**浮点通道**解出线性光，按 1.001 闸判成 HDR(RGBA16F) 或 16 位图层；浮点格式**结构上不许**走 8 位通道。**EXR 已由树内自研 scanline 读取器接入**（C3b，§6.1.18：NONE/RLE/ZIPS/ZIP 可读，PIZ/PXR24/B44/DWA 与 tiled/deep/multipart 如实拒绝，写侧仍 `UNSUPPORTED`）；32 位 float TIFF **仍无解码器**（树内 stb 无 TIFF）。第三方源码 vendoring 到 `engine/third_party/`，`__has_include` 探测缺库时如实回落。`.comp` **内部**像素仍走自研零依赖 `comp_png`（工程包可移植的前提）。**能力边界（P2-4）**：宿主构建走 libpng+zlib，而 `scripts/build_ohos.sh` 的包含路径不含 `engine/third_party/*`，故鸿蒙交叉编译时 `__has_include("png.h")` 探测失败、PNG 会**如实回落**到自研 `comp_png` —— 本行「真实第三方库」的表述**仅对宿主构建成立**，鸿蒙侧是同语义的另一条实现。其余格式集中在 `format/codec_registry` 登记，未链接实现者如实 `UNSUPPORTED` 并给出原因 | 按 `docs/codec-migration.md` 第 6 节继续接入 TIFF / libheif / OpenEXR / lcms2；只改 `codec_lib` 与登记表 |
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
| 编译单元 | 91（38 个 `.c` + 53 个 `.cpp`；其中 vendored 第三方 26 个 `.c`），按 `build/engine_*.o` 实测 |
| 产物 | `build/compositor_tests.exe` |
| 测试 | `summary: 457 cases, 0 failed, 0 check failures`（当前基线；§6.28 起） |
| 全量构建耗时 | ≈ 60–120 秒（含 `.c/.cpp` 编译、链接与测试运行） |
| 源码规模 | engine：53 `.cpp` / 38 `.c`（含 vendored 26）/ 91 `.h`（含 vendored）；tests：37 `.cpp` |

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
- `test_composite_cache.cpp`：画布级合成缓存（MERGED）——事务外连拍命中同一块缓冲且与现算
  逐字节一致、图层结构/像素写入/撤销/重做都会失效重算、活跃事务期间不供缓存（事务内渲染
  看到未提交像素，提交/回滚后各自正确）、拼合复用缓存且零拷贝接管像素。
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
  **PSD 的 note 必须说全两条通道**（含 `comp_io_export_document` 与 `flattened`，且不再出现
  `READ-ONLY`，见 §6.88）；
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
| 编解码能力表 | `comp_io_codec_support` 两段式查询：PNG = `COMP_CODEC_VENDORED`（libpng 1.6.44 + zlib 1.3.1）、JPEG = `VENDORED`（stb，note 含 `stb_image`）、PSD = `VENDORED`（拍平只读；分层读写走 §6.87 的文档入口）；EXR = `BUILTIN`（树内 scanline 读写器，note 写明可写 HALF 与支持的压缩）；未知格式 = `COMP_ERR_NOT_FOUND` |
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
  → **2026-10-18 S13 已接入**：`offscreen_compute` 真实为 true，离屏渲染由设备执行
  （`last_render_offloaded()` 证明）；**2026-10-19 S16 已接窗口交换链直呈**（见 §6.26）。
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
  → **≈60%**（命中约 29 条）。

AUDIT8 记的 ≈90% 是**口径 A**，天然不含上游 macOS 应用层（`Compositor/UI` 9.3k 行 +
`ContentView`），因此「90% vs 60%」不矛盾。

#### 口径 A：引擎核

把「同等正常使用」拆成**四层**，逐层给权重与实测覆盖率。
引擎核（`engine/` + ABI + 两壳）已完成，**UI/编辑器层整体不在本仓范围**。

| 层 | 权重 | 覆盖率 | AUDIT8 | 变动依据 |
|----|------|--------|--------|---------|
| ① 引擎核契约面（ABI / 领域模型 / 事务 / 历史 / 诊断 / 边界） | 30% | **~95%** | ~95% | **112 个**导出 ABI 全有异常护栏；句柄 magic、反序生命周期、线程局部错误槽、定制分配器、字节预算、transform 写入侧闸均已落地并有护栏；`run_tool_op` 的 `out_code` 契约已补；S8 补 `comp_selection_set_feather` 绝对羽化入口 |
| ② 像素内核**有产品通路**的部分 | 35% | **~100%** | ~86% | AUDIT8 列的可达缺口清零：`wand_trace`、`scanlines`/`chunky`、deep round trip、`cube_apply`(16bit)、`extract/restore_alpha` 已接（见 §6.1.10）；**C5 又接上 `hdr_exposure`/`extract_alpha_hdr`/`restore_alpha_hdr`**（half 表面，§6.1.19）；**最后一对 `deep_take_painted`/`deep_take_painted_hdr` 已接进落笔族**（Stroke/Clone/Smudge/Liquify 在 16 位与 HDR 图层上保留深度，§6.1.21）——上游语义本就是「整幅比较 painted/unpainted」，不需要 tile+coverage 记账。`dither_dots`/`dither_glow`/`deep_split`/`deep_merge` 经核实**已接线**，不再计入缺口；`brush_alpha_bounds` 一族由 `extract/restore_alpha` 取代，属**主动不接**而非缺口 |
| ③ 合成与渲染正确性 | 20% | **~98%** | ~95% | **剪贴栈 alpha 语义对齐上游**（`a=0` → `a=128`）、`ImageBuffer::fill` 预乘修正、hidden 层优先级修正、剪贴栈成员 backdrop 补齐；**C5：合成表面按文档深度携带 half，8 位 24 种混合模式逐字节不变**（§6.1.19）；**C4：合成结果按全深度导出 EXR，8 位文档同路径**（§6.1.20）；**S17：GPU 子集按合成单元拆细**（裁蒙/效果/变换/组/剪贴烘焙后上 GPU，调整层与 HDR 仍整篇回落，§6.27）；**S19：D3D12 逐层纹理跨帧常驻（原生单元），1024²/4 层窗口直呈 3.04–3.27 → 1.41 ms（§6.29）**；**S20：烘焙单元（蒙版/裁剪/效果/变换/隔离组/剪贴栈）也按子树结构键缓存，CPU 烘焙与纹理上传均跨帧省掉（§6.30）** |
| ④ 跨端与交付（两壳 / 编解码 / 互通 / 回归） | 15% | **~91%** | ~80% | Windows 壳 **164 项 `--selftest` 基线**（不是人工实测）、OHOS aarch64 交叉编译 + NAPI、PNG/JPEG/PSD 真实编解码、`.comp` 与 PIL 双向 0 LSB 交叉验证；**2026-10-18 补上 `cmake+ctest` 与 clang-cl（MSVC ABI）两条构建路径实测**（各 344/344，§6.1.21）；**2026-10-19 补上 `scripts/visual_check.py` 自动化目视**（11 场景 / 45 断言 / 32 张机器截图，§6.25）；**2026-10-19 S16 补上 Windows 窗口 DXGI 交换链直呈 + 真机基准**（§6.26）。扣分项：无 OHOS 真机、无 macOS 逐像素对比、Win32 交互仍无人工目视 |

**加权完成度 ≈ 97%**（口径 A）。算术：① 30%×95% + ② 35%×100% + ③ 20%×98%
+ ④ 15%×91% = 28.5 + 35.0 + 19.6 + 13.65 = **96.75% ≈ 97%**。

**剩下的 3% 按层拆开，全部有明确归属**（不再是「被上游工具面阻塞」）：

- ① 契约面 1.5 个点：`run_tool_op` 之外的少量诊断/边界口径；
- ③ 合成与渲染 0.4 个点：GPU 子集已从「整篇全有或全无」拆到**按合成单元**（裁剪/蒙版/
  效果/变换/隔离组/剪贴栈在 CPU 烘焙后照样上 GPU，§6.27）；**S18 补上逐层纹理缓存的前提**
  （每层 `content_revision` + 全部写入口换号，§6.28），**S19 把纹理缓存本身（R2）做完**
  （原生单元跨帧常驻，§6.29），窗口直呈 1024²/4 层从 3.04–3.27 ms 降到 **1.41 ms**、
  比 CPU flatten+GDI 快 **1.58–1.60×**；**S20 再把烘焙单元也纳入缓存**（子树结构键 +
  CPU 烘焙表面缓存 + D3D12 烘焙纹理缓存，§6.30），文档不动时烘焙与上传都为 0；
  仍剩调整层、HDR 表面两条整篇回落，故仍扣分；
- ④ 跨端 1.5 个点：无鸿蒙真机（`hdc list targets` 空）、无 macOS 端逐像素对比。

上一轮记的「`deep_take_painted` 被 S4 仿制笔刷阻塞」已证伪：上游 `BrushStroke.swift:1097`
的 `sixteenBit()` 是**整幅**比较 painted/unpainted，本仓照此实现后不再需要 tile+coverage 模型。

#### 口径 B：产品功能面

| 上游类（README 48 条 / 7 大类） | ng 命中 |
|---------|---------|
| Layers（图层/组/不透明度/全混合模式/图层蒙版/剪贴蒙版/调整层 12 kind/图层效果 6 kind/合并/重排嵌套） | ✓ |
| Transform（仿射 move·scale·rotate / distort / 精确值） | ✓ |
| Painting（笔刷 / Spot Heal / Content-Aware Fill / 吸管） | ✓（笔刷为解析式，噪声未采样；**S5 补上 Erase 模式**——同一支笔的 destination-out，§6.15；**S6 接上前景色模型、`E` 切换与吸管**，§6.16） |
| Canvas（Canvas Size / Image Size / Trim / 旋转） | ✓ |
| **Camera Raw**（6 文件 1,708 行） | ✓ 设置解析、曲线、763 张实测表与完整像素管线已交付（`comp_camera_raw_apply` 等 3 个 ABI，§6.10）；几何 / Upright 透视变换与 RAW 解码未做 |
| **选区几何**（Marquee/Lasso/Pen/Select Subject/抠图，1,000+ 行） | ✓ **部分**：Marquee（矩形/椭圆）/Lasso（多边形）/魔棒、形状叠加、全选/取消/反选、羽化/扩缩已交付（`comp_selection_*`，§6.7 / §6.17）；Pen 工具手势与 Select Subject / 抠图仍缺（后两者依赖 ML，不在本仓范围） |
| **选区运算**（加选/减选、移动轮廓、搬移与复制选区内的像素、把图层像素或蒙版载入为选区） | ✓ **S5 交付**：载入像素/蒙版（阈值 + 四则叠加）、平移轮廓、搬移/复制像素均已进 ABI（§6.15）；**S6 补上外壳的拖拽手势**（`V`，Alt = 复制，一步撤销，§6.16） |
| **Type / Shape / Gradient**（TypeTool 472 行 + ShapeTool + Gradient） | ✓ **三个子项均已交付**：Gradient = 线性/径向 + 双色标 + 选区混合（`COMP_TOOL_GRADIENT = 13`，§6.19）；Shape = 矩形/椭圆/直线/贝塞尔路径 + 填充/胶囊描边 + 选区混合（`COMP_TOOL_SHAPE = 14`，§6.20）；Type = 内置 5×7 点阵 ASCII 字体 + 字号/行距/字距 + AA（`COMP_TOOL_TYPE = 15`，§6.21）。**边界**：Type 是 ASCII-only 位图字体，没有系统字体/CJK/富文本/可编辑文本对象，与上游 472 行富文本 TypeTool 不等价 |
| **Clone Stamp / Smudge / Liquify**（400+ 行） | ✓ 仿制图章（源快照 / aligned 偏移 / sampleAllLayers）、涂抹（carried 拖色）、液化 Push（前向位移重采样）已交付（`comp_tool_apply` kind 9/10/11，§6.9）；同一 `BlurToolMode` 里的 Blur 模式也已在 P1 补齐（`COMP_TOOL_BLUR = 12`，§6.12） |
| **Crop 工具**（`Crop.swift` 249 行） | ✓ 拖拽成框（比例 / Option 对称）、比例切换、取整、可提交性判定与提交（平移图层 + 换画布，一步撤销）已交付（`comp_crop_*`，§6.8）；手柄 / 蚂蚁线 / 吸附参考线属外壳 |
| **Vignette / Bloom / Remove Background**（`Filters.swift` 1,007 行） | ✓ **部分**：Vignette / Bloom（含 Tonal Contrast、Lens Correction）已交付（`comp_filter_apply`，§6.12）；Remove Background 仍为零（依赖 ML / 分割，不在本仓范围） |
| **格式导入** HEIC/TIFF/WebP/SVG/RAW/PSB | 枚举有 7 种，但 HEIF/WebP、SVG、PSB 无解码器，TIFF=UNAVAILABLE |

即 **约 29/48 命中 ≈ 60%**（Vignette 与 Bloom 各计一条；Tonal Contrast /
Lens Correction 是同一 `Filters.swift` 通路的附加值，不额外抬高条数；选区几何按
「部分命中」计，未抬高条数——缺的 Select Subject / 抠图正是该条的主要工作量；
Crop、Clone/Smudge/Liquify 与 Camera Raw 按已交付计入）。S5 抬高**两条**：
Selections 的「加选/减选 + 移动轮廓 + 搬移/复制像素」与「载入图层像素或蒙版」；
S6 再抬高**一条**：**吸管（Painting #7）**——前景色模型与取色工具都已接进外壳，
按「用户能做的事」口径从此命中（§6.16）。Painting 的 Erase 模式仍算进已计数的
笔刷条目，不额外抬高条数。**S7 也不抬高条数**：它补的是已计数的「选区几何」条目
里的 Marquee(椭圆)/Lasso/Magic Wand 子项与外壳手势，该条目仍缺 Pen 工具、
Select Subject 与抠图，不足以从「部分命中」升为整条命中（§6.17）。**S9 也不抬高条数**：
渐变只是同一条「Type / Shape / Gradient」三个子项里的一个，剩余 Type（472 行）与 Shape
仍为零，整条按「部分命中」计（§6.19）。**S10 同样不抬高条数**：形状补上后该条的三个
子项只剩 Type 未交付，但 Type 是其中最大的一块（472 行 + 字体栅格化），整条仍按
「部分命中」计——**28/48 ≈ 58% 不变**，不因为「两个子项都做了」凑整抬高（§6.20）。
**S11 抬高一条**：「Type / Shape / Gradient」的三个子项终于全部有产品通路——Type 是
内置 5×7 点阵 ASCII 字体（`COMP_TOOL_TYPE = 15`，§6.21），不再是零。边界如实说明：
它是 ASCII-only 位图字体（无 CJK / 系统字体 / 粗斜体 / 对齐 / 旋转 / 可编辑文本对象），
与上游 472 行富文本 TypeTool **不等价**；这个 +1 只表示「该条不再缺子项」，
不代表 Type 功能面与上游等价。
其余约 4,800 行 Swift
**完全没有对应物**，且这些**不属于「接线」范畴，属新增功能**——不是把孤立内核接出去能解决的。

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
→ 选区运算 + 橡皮擦 + 吸管（把已有像素/蒙版/笔刷内核换成用户可做的东西；
  **2026-10-18 已交付引擎与 ABI，见 §6.15**；外壳手势/取色 UI 待接）
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
- **Shape（S10 追加，kind 14）**：填充与描边是**两个独立开关**（先填后描），直线只准描边；
  抗锯齿用 4 行超采样 + 水平解析覆盖率，描边是胶囊距离并集（圆头端点/折点）。
  `stroke_width` 只在描边开启时校验（纯填充形状不需要宿主为不用的字段编数值）；
  形状与图层完全不相交时**成功但留不下撤销步**。细节见
  [`docs/tool-ops.md`](docs/tool-ops.md) §4.11。
- **Dodge / Burn / Sponge（P1 追加，kind 16/17/18）**：三种色调工具共用一个
  `comp_tool_tone{mode, strength}`，是**落笔族**（RGBA16 / RGBA16F 上笔尖之外保深像素）；
  在直通色空间运算、alpha 不变、全透明像素跳过；选区覆盖率在文档分辨率乘进笔迹，
  选区外逐字节不动。运算结果逐字节不变（海绵加色碰到灰度、减淡碰到纯白）时整步
  `txn_abort`，不留空撤销步。细节见 [`docs/tool-ops.md`](docs/tool-ops.md) §4.13。

### 6.6 内核接线现状（2026-10-18 更新）

| 内核 | 产品通路 | 已接线 | 仍未接线 |
|------|---------|--------|----------|
| Wand | `COMP_TOOL_SELECT`（破坏性写蒙版）；`comp_selection_apply_wand`（只读选区，S7） | `wand_mask`、`wand_trace`（只读闭合轮廓） | — |
| Heal | `COMP_TOOL_HEAL` | `spot_heal`、`heal_membrane`（内部派发） | `heal_coverage_bounds` |
| Patch | `COMP_TOOL_HEAL` | `patch_fill` | — |
| Dither | `COMP_TOOL_DITHER` | `dither_apply`、`scanlines`、`chunky`（后两者分别接 `dither_glow` 与 `dither_dots`） | — |
| Lens | `COMP_TOOL_LENS` / `COMP_FILTER_LENS_CORRECTION` | `lens_distort`（工具收 `|k| <= 1`，滤镜收 `-100..100` 再换算 `k`） | — |
| Blur | `COMP_TOOL_BLUR` / `COMP_FILTER_GAUSSIAN_BLUR` / Tonal Contrast、Bloom 的 detail 模糊 | `blur_rgba8_copy`（三趟滑动窗口盒式近似，与 Gaussian Blur 调整层同一实现） | — |
| Dodge / Burn / Sponge（P1 新增，域层算子） | `COMP_TOOL_DODGE` / `COMP_TOOL_BURN` / `COMP_TOOL_SPONGE` | `engine/domain/tool_ops.cpp` 的色调分支（直通色空间混合 + Range 权重，alpha 不变；选区在文档分辨率裁剪） | 无独立内核函数（上游同为工具层逻辑） |
| Shape（S10 新增，非上游 `.c` 内核） | `COMP_TOOL_SHAPE` | `shape_raster::shape_rasterize`（矩形 / 椭圆 / 三次贝塞尔路径的填充 + 胶囊距离描边 + 圆角矩形 + 窗口化覆盖率，4 行超采样 AA）；路径基元来自 S10 抽出的 `path_raster`（与选区共用，行为中立） | 贝塞尔路径只有 ABI 通路（外壳 `U` 只能拖出矩形/椭圆/直线；钢笔仍用于提交选区） |
| Brush | `COMP_TOOL_STROKE` | 解析式圆头笔 + **`noise_add_at` 颗粒**（噪声钉在文档坐标；`seed` 参与计算） | `brush_alpha_bounds` 一族（剪贴栈已改走 `extract/restore_alpha`） |
| Noise | `COMP_TOOL_STROKE`（经 brush 字段）/ `COMP_FILTER_ADD_NOISE` | `noise_add_at`（origin 锚定） | — |
| Motion Blur（P1 新增，本仓内核） | `COMP_FILTER_MOTION_BLUR` | `engine/domain/filters.cpp` 的 `motion_blur_pixels`（盒式线积分：长度 = `distance * scale`，预乘双线性、边缘夹取、最多 257 采样） | 无独立内核函数（上游是 Core Image `CIMotionBlur`） |
| Progressive Blur（P1 新增，本仓内核） | `COMP_FILTER_PROGRESSIVE_BLUR` | `engine/domain/filters.cpp` 的 `progressive_blur_pixels`（8 档 `blur_rgba8_copy` 三角混合；线性 / 径向轴，端点经 `document_to_pixel_continuous`） | 无独立内核函数（上游是 Core Image `CIMaskedVariableBlur`） |
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

### 6.14 S4 外壳交互补齐：选区蚂蚁线 / 可视笔迹 / Smudge-Liquify 接线（2026-10-18，Claude）

**1. 分工仍是两层。** 选区边界、虚线段、笔尖半径、retouch 强度这些**几何与状态机**
留在 `ui_core.h`（不 include Windows/engine 头、不分配内存、可单测），`main.cpp` 只做
「消息 → ui_core 调用 → GDI」和 ABI 映射。新增的叠加层绘制 `draw_canvas_overlays()`
在画布 `StretchDIBits` 之后、面板之前调用，并先 `IntersectClipRect` 到画布。

**2. 选区蚂蚁线。**

- 几何：`selection_boundary_segments()` 在像素网格上取「命中/未命中」的跨界处，
  同一行/列的连续边合并成一条线段；返回真实总数，容量不足时只写前 N 条，调用方
  据此如实提示截断（壳层上限 20000 条）。
- 数据源：`comp_selection_get_coverage`（文档分辨率 8 位、含 feather）阈值 128。
  壳层**先查 `comp_selection_get_active`**：按 ABI 契约，反选「全选」得到的是
  「无选区」而不是空选区，此时取覆盖率会返回 `COMP_ERR_STATE`。
- 绘制：白色底线 + 黑色虚线段；`dash_spans()` 按阶段生成，`WM_TIMER` 每 120 ms
  推进相位（dash 4 + gap 4 = 8 px 周期）。**没有选区时一条线都不画**，定时器处理
  函数也直接返回，不产生空转重绘。
- 性能：屏幕线段先用画布矩形做轴对齐裁剪再分段。32 倍放大时一条文档边是数万
  屏幕像素，不裁剪会为一条边生成上千段虚线。
- 交互：`Ctrl+A` 全选 / `Ctrl+D` 取消 / `Ctrl+Shift+I` 反选（菜单「编辑」同项）；
  Marquee（`M`）拖矩形走 `comp_selection_apply_shape(RECT)`，Shift 并集、Alt 差集、
  默认替换。每次写入一个撤销步，`undo/redo` 后重取覆盖率，蚂蚁线同步出现/消失。

**3. 可视笔迹。** 落笔期间把文档坐标折线画成 3 px 预览线，并在光标处画笔尖轮廓
（`brush_tip_radius = 直径 × 缩放 / 2`，最小 1 px）；`TrackMouseEvent` +
`WM_MOUSELEAVE` 负责离开画布后隐藏。预览不写像素：提交仍只在 `WM_LBUTTONUP`
发生一次，所以「拖很久」不会产生一串撤销步。

**4. Smudge / Liquify 接线。** 原来的 `blur_stroke_*` 收敛成 `retouch_stroke_*`，
按 `shell.retouch_mode` 选 kind：Blur → `COMP_TOOL_BLUR` + `blur.radius`；
Smudge → `COMP_TOOL_SMUDGE` + `warp.strength`；Liquify → `COMP_TOOL_LIQUIFY` +
`warp.strength`。强度 `clamp_retouch_strength` 夹在 0.01..1（ABI 允许 0，壳层不把
「0 强度」当可用状态），`Alt+[` / `Alt+]` 步进 10%。工具条与状态栏按子模式显示
半径或强度，不再出现「尚未接内核」。

**5. 测试与自检。** 新增 4 例 `test_ui_core`：矩形边界 4 条（落在像素边界而非
像素中心）、空选区/全选/贴边/threshold、虚线相位与截断、笔尖半径 + 强度夹取。
`--selftest` 新增 18 项，仍只走 C ABI：Smudge 改变像素且一步撤销逐字节还原、
Liquify `strength = 0` 是**逐字节 no-op**、`strength = 0.8` 改变像素；选区部分冻结
「矩形覆盖率 255/0、边界 4 条、撤销清除蚂蚁线、全选覆盖画布、反选全选 = 无选区、
取消后回到 active=0」。

**6. 验证。** `scripts/run_regression.py --require-ohos` **6/6 PASS，0 skipped**：
步骤 ① **377 cases / 0 failed**（373 + 4）；步骤 ② Windows 壳自检 **59 项**
`failures=0`、SHA256 `3478e678835dc537…` 与仓内基线逐字节一致（基线已同步更新）；
步骤 ⑤ 共享 DLL 真实加载；步骤 ⑥ OHOS aarch64 交叉编译 + NAPI rc=0。

**7. 如实说明的边界。** 本轮新增的 Win32 交互路径（框选拖拽、蚂蚁线动画、笔尖
轮廓、Smudge/Liquify 落笔）**没有经过人手实测**——无窗口环境下只能验证到 ABI 层与
`ui_core` 数学层。选区目前只接矩形框选与全选/取消/反选，羽化/扩缩手柄、套索与
魔棒仍在后续；滤镜实时预览、面板缩放/停靠未变。

报告：`../report/S4-shell-interaction-20261018-claude.md`。

### 6.15 S5 选区运算 / 像素搬移 / 橡皮擦 / 吸管（2026-10-18，Claude）

S5 的目标是把上游 README 里三条「要有、但还没有」的能力补进引擎与 ABI：
**加选/减选并搬移选区像素**、**把图层像素或蒙版载入为选区**、**Paint/Erase 双模式**
以及**吸管取色**。五个入口全部按既有口径交付：领域层实现 + C ABI + 单元测试 +
文档，不引入新依赖，`.comp` 格式不变。

**1. 载入选区（`comp_selection_load_layer`）。** 两种源：

- `COMP_SELECTION_LOAD_PIXELS`：取图层的 **alpha**，经 `document_to_pixel()`
  逐像素映射回文档坐标——与合成、吸管同一套「文档坐标 ↔ 图层像素」公式，因此
  缩放/旋转/翻转的图层也能正确投影（旋转/翻转不重采样：每个文档像素查一次最近源像素）。
- `COMP_SELECTION_LOAD_MASK`：取图层蒙版的灰度。**没有蒙版的图层取到空选区**
  （没有可载入的数据，不做「全不透明」的臆测）；蒙版与像素矩形同尺寸这一约束由
  `comp_layer_set_mask` 保证。阈值与叠加口径同下。

阈值是**严格大于**：`value > threshold ? value : 0`，`threshold ∈ [0, 255]`。
叠加沿用 `comp_selection_apply_shape` 的三则口径（replace / add / subtract），
并且**任何载入都会把 feather 归零**——羽化是几何形状的属性，不是像素的属性；
保留旧 feather 会让载入结果被一圈无来源的模糊污染。结果与当前选区逐字节相同时
`changed = 0`，不产生撤销步。

**2. 平移轮廓（`comp_selection_translate`）。** 整张覆盖率缓冲按整数偏移平移，
越出画布的部分直接裁掉（不扩画布、不环绕）；feather **保留**（轮廓只是搬家，
软边跟着走）。`|dx| / |dy| > 1e6` 在事务之前拒绝，非法参数不留半成品。

**3. 搬移/复制选区像素（`comp_selection_move_pixels`）。** 这是「剪切并粘贴到偏移处」
的引擎版，一次调用完成：

- 只处理**图层 ∩ 画布 ∩ 选区包围盒**内、coverage > 0 且 alpha > 0 的像素；
  搬移量按 coverage 缩放（半选中像素只搬一半，预乘空间里减/加都是线性的）。
- 默认是**剪切**（源处减掉搬走的那一份）；`duplicate != 0` 时源像素保留。
- 落点在画布外：丢弃并写 `note`，**不把图层撑到画布之外**；落点仍在画布内但
  超出图层矩形时，按需扩层，transform 与像素矩形同步更新。
- 蒙版随像素矩形平移，新增区域填 255（未蒙）。
- 一次调用 = **一个撤销步**；`dx == dy == 0` 直接返回 `changed = 0`
  （原地「复制」会把像素加到自己身上、把 alpha 叠满，那是数据损坏不是复制）。
- 明确拒绝：组图层、无像素、非 RGBA8（16 位/HDR 待后续）、带旋转/翻转、
  非 1:1 缩放、蒙版与像素不同尺寸。都是 `COMP_ERR_STATE`，不静默降级。

**4. 橡皮擦（`comp_tool_op_desc.erase`）。** 尾部追加字段，既有偏移不变；只有
`COMP_TOOL_STROKE` 读它。语义是**预乘空间的 destination-out**：
`px[c] = px[c] * (1 - coverage)`，四通道同乘——直通色上只减 alpha 会留下
「透明却发亮」的脏数据。笔形、hardness、`pressure * flow` 与 Paint 模式完全一致，
所以擦掉的正是同一笔会盖上去的那一份；橡皮擦**忽略 `brush.grain_*`**，
`pressure * flow == 0` 依旧逐字节 no-op。

**5. 吸管（`comp_eyedropper_sample`）。** 输出是**非预乘 sRGB 0..1**（不透明红 =
`{1,0,0,1}`），不是 0..255，也不是预乘值。

- 窗口 **N×N 按预乘值平均后再反预乘**：半透明边缘不会因 alpha 加权而发亮，也不是
  「先平均直通色再乘 alpha」那种会被透明黑拉暗的写法。
- 窗口越出画布/图层时只统计真实存在的像素；整窗落空返回透明 `{0,0,0,0}`。
- 源两种：`COMP_EYEDROPPER_LAYER` 读目标图层（**忽略可见性**）；
  `COMP_EYEDROPPER_MERGED` 读整篇合成（隐藏图层取不到）。
- 16 位 / HDR 图层走既有的 8 位工作副本（`deep_to_rgba8` / `hdr_to_rgba8`），
  取到的是屏幕上那个颜色。
- **纯只读**：不开事务、不产生撤销步、不改像素；参数错误分派
  `INVALID_ARGUMENT`（坐标出界、窗口非奇数/越界、空指针、未知 source）/
  `NOT_FOUND`（图层不存在）/ `STATE`（组图层或无像素）。

**6. 测试。** 新增 14 例：`tests/test_eyedropper.cpp` 5 例（直通色 + 只读、
预乘平均、合并源 vs 图层源、16 位工作副本、错误码分派），`tests/test_selection.cpp`
7 例（载入像素 + 阈值、蒙版忽略像素 alpha、参数拒绝、平移裁剪、剪切并扩层、
复制保留源、不支持图层拒绝），`tests/test_tool_ops.cpp` 2 例（橡皮擦按 coverage
做减法而非覆盖、零压力 no-op）。

**7. 验证与边界。** 交付后按「查残余」补跑全量回归：`scripts/build.py` →
**391 cases, 0 failed**；`scripts/run_regression.py --require-ohos` → **6/6 PASS，
0 skipped**（Windows 壳自检 59 项、SHA256 与基线一致；OHOS `ok=87 fail=0`）。
第一次全量运行曾在 `selection_translate` 处堆损坏（`0xC0000374`）：负向平移把负
指针偏移转成 `size_t`，越界平移又把负数长度交给 `memcpy`；修复为目标列偏移
`left + dx`，且只在重叠区非空时拷贝（回归已覆盖两个方向）。
**外壳尚未接线**：Windows 壳还没有取色工具、前景色模型、拖拽移动
选区/搬移像素的手势，M 框选之外的载入/搬移只能走 ABI。因此口径 B 只抬高 2 条
（Selections 的两条），吸管按「用户能做的事」不计入。（S6 已补齐这组接线，见 §6.16。）

报告：`../report/S5-selection-pixels-eyedropper-20261018-claude.md`。

### 6.16 S6 外壳接线：前景色 / 吸管 / `E` 橡皮擦 / 拖动选区（2026-10-18，Claude）

S6 不新增 ABI，也不改 `.comp`——它把 S5 已经交付的 `comp_eyedropper_sample` /
`comp_selection_move_pixels` / `comp_selection_translate` / `comp_tool_op_desc.erase`
接到 Windows 壳，并补上让它们「可用」的前景色模型。平台无关部分仍全部在
`ui_core.h`（无 Win32、无引擎头），`main.cpp` 只做消息到 `ui_core` / C ABI 的翻译。

**1. 前景色模型（`ui_core`）。** `ColorRGB` 是 8 位直通 sRGB，与
`comp_tool_brush.color` 同口径；`ColorHSV` 为 h 0..360 / s、v 0..1。
`rgb_to_hsv` / `hsv_to_rgb` 往返在 8 位量化下逐通道相等；`hsv_to_rgb` 对越界输入
**收口而不报错**：h 用 `fmod` 环绕，s / v 夹到 0..1，NaN 回落到 0（灰阶）。
`color_to_hex` 写大写 `#RRGGBB`；`color_parse_hex` 只认整串（可选 `#`，
3 位缩写 `#abc` → `#AABBCC`），前后空白或多余字符整体失败——键入路径宁可让用户改，
不静默截断。色板固定 12 色，`step_swatch` 双向取模循环，越界 `swatch_at`
回落黑色而不是野指针。

**2. 吸管（`I`）。** 点按或拖拽画布连续取色；命中画布外 / 文档外直接忽略
（ABI 对越界坐标返回 `INVALID_ARGUMENT`，那不是「吸管坏了」）。`sample_size` 固定 1；
按住 `Alt` 取 `COMP_EYEDROPPER_MERGED`（整篇合成），否则取当前图层；**没有选中图层
时回落到合并源**——画布上看到什么就吸什么，比报错更符合预期。结果经
`color_from_unit` 写进 `shell.foreground`，状态栏同时给出 hex、源、H/S/B 与不透明度。
吸管是只读工具：不开事务、不进撤销栈、不置 `needs_render`。

**3. 画笔 / 橡皮擦（`B` / `E` / `X` / `Shift+X`）。** `E` 在画笔与橡皮擦之间往返
（其余键位仍按 `tool_table`）；两者共用同一条 `COMP_TOOL_STROKE`，形状、`hardness`、
`pressure * flow` 完全一致，只有 `desc.erase` 翻转合成方向（预乘 destination-out）。
`stroke_erase` 在**落笔时冻结**：拖拽中途按 `E` 不改变这一笔的合成方向。
`X` 循环色板、`Shift+X` 进入十六进制键入态（模态：只吃 0-9/A-F，`Backspace` 删字符，
回车确认，`Esc` 只取消输入而不是关窗口）。状态栏右端 16 px 色块 +
`前景 #RRGGBB`，左端文本按省略号截断（状态栏窄于 260 px 时不画色块）。

**4. 拖动选区（`V`）。** 没有活动选区时只提示、不开事务。按下鼠标冻结起点并记录
`Alt`（= 复制，否则剪切），松开时把位移取整到文档像素，在**一个显式事务**里先
`comp_selection_move_pixels` 再 `comp_selection_translate`——顺序不能反：像素搬移按
当前选区取像素，先平移轮廓会搬错地方。任一步失败 `comp_txn_abort`（半搬的像素
比不搬更坏）；两步都 `changed = 0` 也 abort，不产生空的撤销步。拖拽期间蚂蚁线按
同一取整偏移预览，所以预览与提交不会差一像素。

**5. 自检暴露的内核缺陷：单点折线不落笔。** 第一版 S6 自检有三项红：
`COMP_TOOL_STROKE paints…` / `comp_eyedropper_sample()…` / `erase removes…`。
根因不在壳：`stroke_begin` 为一次点按只塞 1 个折点，而 `StrokeShape::coverage_at`
只量到**线段**的距离——参数闸接受 1..65536 个折点，1 个点时循环一次都不执行，
coverage 恒 0，于是「点了没反应」。修复在 `engine/domain/tool_ops.cpp`：单点按
「到该点的距离」算 coverage，等价于一记圆形 dab；既有的两个
`stroke_point_count == 1` 测试都带 `pressure = 0`，没有断言过「单点必须不画」。
同轮补了一条 ABI 护栏（单点落笔、笔心实色、半径外不落墨），并订正域层测试里
「单点没有线段可落」的过时注释；`docs/tool-ops.md` §4.2 同步写明 dab 语义。

**6. 测试。** 新增 6 例：`test_ui_core` 5 例（`E` 往返、RGB↔HSV 往返与越界收口、
hex 往返与垃圾拒绝、12 色板循环、0..1 换算夹取），`test_tool_ops` 1 例（单点 dab）。
基线 391 → **397 用例**。`--selftest` 新增 12 项（`E` 切换、色板循环、hex 往返、
落色后吸管读回同色、橡皮擦擦掉并一步撤销逐字节还原、像素搬移 + 轮廓平移同一事务
一步撤销），59 → **71 项**。

**7. 验证与边界。** `scripts/build.py` → **397 cases, 0 failed**；
`scripts/run_regression.py --require-ohos` → **6/6 PASS，0 skipped**（Windows 壳自检
71 项、`failures = 0`、SHA256 `0adc0e00f2ea2518…` 与仓内基线一致；OHOS aarch64
交叉编译 rc=0）。本轮仍**没有经过人手实测**：无窗口环境下只能验证到 ABI 与
`ui_core` 数学层；吸管取样窗口固定 1（没有 N×N 平均的 UI），HSV 只有模型与读数，
尚无三滑块面板；`V` 搬移沿用引擎约束（RGBA8、1:1、无旋转/翻转），其它图层会得到
明确失败提示而不是降级。

报告：`../report/S6-shell-wiring-20261018-claude.md`。

### 6.17 S7 选区工具补口：椭圆框选 / 套索 / 魔棒 / 新选区羽化（2026-10-18，DSH）

S6 把 S5 的选区 ABI 接进了外壳，但外壳只能画矩形框选，魔棒、套索与新选区羽化在
ABI 与 UI 两侧都没有通路（S6 报告 §9 的 follow-up）。S7 补这一组，并**只补缺口、
不改 S6 已验证的接线**。

**1. 新 ABI：`comp_selection_apply_wand`（第 111 个导出符号）。** 入参
`(document, layer, source, seed_x, seed_y, tolerance, contiguous, radius, combine, out_changed)`；
`source` 为 `COMP_SELECTION_WAND_LAYER`（当前图层像素）或 `COMP_SELECTION_WAND_MERGED`
（整篇合成）。它**只写选区**，不动图层蒙版——这是与 `COMP_TOOL_SELECT` 的分工：
后者是破坏性写入（魔棒直接改蒙版），前者是「先选再改」。关键口径：

- 像素取样一律走 8 位预乘视图（16 位/HDR 图层先落到 8 位副本，探针读的就是画布上
  看到的颜色）；`wand_rgba8_view` 是唯一取样口。
- `LAYER` 源要求**整数平移的 1:1 变换**（`effective_bounds()` 的宽高 == 像素宽高）；
  缩放/旋转/翻转的图层显式返回 `COMP_ERR_STATE`，需宿主先栅格化——不静默降级。
- 种子在 ABI 闸里校验「在画布内」，域层再校验「在取样面内」（图层可能小于画布）；
  分组图层 / 无像素图层 / 不存在的图层分别给 `COMP_ERR_STATE` / `COMP_ERR_STATE` /
  `COMP_ERR_NOT_FOUND`。
- **选中 0 个 = 无变化**：返回 `COMP_OK`、`out_changed = 0`，既有选区原样保留、
  不产生撤销步（上游「无选中即不改变」）。这与 `COMP_TOOL_SELECT`「空选中不写蒙版」
  是同一条原则的只读版。
- 命中掩码经**画布尺寸的 coverage** 走既有的 `apply_coverage_mask`，因此
  add/subtract/replace、羽化清零与「无变化短路」全部复用 S5 已冻结的语义。
- 常量与量程：`tolerance` 0..255、`radius` 0..512、`contiguous` 0/1；参数闸在
  事务之前，失败不留撤销步。

**2. 外壳手势（`ui_core` 判据 + `main.cpp` 翻译）。**

- `M` 循环 `Rect → Ellipse → Rect`（只循环引擎真支持的两种形状，不造「假形状」）；
  `marquee_end` 按形状调 `COMP_SELECTION_RECT` / `COMP_SELECTION_ELLIPSE`，
  预览也按同一形状画。
- 套索：按下起笔、移动累积文档坐标点（复用 `stroke_point_should_append` 的 0.75 px
  去抖，上限 8192 点），松开时 ≥3 点自动闭合提交为 `COMP_SELECTION_POLYGON`；
  点数不足则不提交。Shift = 加选、Alt = 减选，与框选同一套修饰键。
- 魔棒：点按取种子 `floor(doc)`，`LAYER` 源；`Shift+W` 切 `MERGED`（画布看到的颜色）；
  `[` / `]` 在魔棒激活时调容差（0..255，步进 1/4/8），否则保持原笔刷直径行为；
  状态栏如实显示「图层/合并」与当前容差。
- 羽化：`Shift+F` 键入 0..250 px（模态输入，只吃数字、`Backspace`、回车确认、
  `Esc` 取消）。默认对新选区生效；对**已有选区**再按 `Shift+F` 走
  `comp_selection_modify(COMP_SELECTION_FEATHER, r)`，状态栏写明是**叠加**语义
  （ABI 没有「绝对设置羽化」的入口，`modify` 按 `sqrt(cur² + r²)` 叠加），
  不假装它是绝对值。
- **一步撤销**：`finish_new_selection` 把「形状/魔棒 + 可选羽化」包进一个显式事务，
  任一步失败 `comp_txn_abort`，`changed == 0` 也 abort（不产生空的撤销步）。
  `apply_shape` 在 REPLACE 时会把 feather 归零，所以「形状 + 羽化」在同一事务里
  得到的羽化值就是键入的 `r`，不会叠加到上一次的残留上。

**3. 测试。** 新增 11 例：`test_selection` 7 例（连通魔棒、全局魔棒跨不连通色块、
容差匹配半径、MERGED 取样、与既有选区做减选、0 匹配保留旧选区且不加撤销步、
坏参数矩阵：种子越界/NaN/tolerance 256/contiguous 2/radius 513/非法 combine/
非法 source/图层不存在/分组图层/非 1:1 图层）、`test_ui_core` 4 例（形状循环与命名、
套索点数闸、魔棒容差夹取与步进、羽化夹取与步进）。基线 397 → **408 用例**。

**4. 自检。** `--selftest` 新增 19 项：椭圆覆盖率/角落裁剪、多边形套索、魔棒 fixture
（1:1 图层）、连通魔棒选区与边界、MERGED 取样、种子越界与 tolerance 越界的
`out_changed` 不变性、0 匹配保留旧选区且不加深历史、形状 + 羽化同事务 = 一步撤销、
撤销后回到未羽化的上一版选区。71 → **90 项**，`compositor_shell_selftest.txt` 同步
重新生成（SHA256 `4cfd2789028cd6ec…`）。

**5. 验证与边界。** `scripts/build.py` → **408 cases, 0 failed**；
`scripts/run_regression.py --require-ohos` → **6/6 PASS / 0 skipped**（壳自检 90 项、
`failures = 0`、SHA256 与仓内基线一致；OHOS aarch64 交叉编译 + NAPI rc=0）。
仍然**没有经过人手实测**：无窗口环境下只验证到 ABI 与 `ui_core` 数学层。
明确边界：套索只有多边形（Pen 贝塞尔手势未接，ABI 的 `COMP_SELECTION_PATH` 可用但
没有外壳工具）；魔棒不做 N×N 平均窗口（`radius` 恒为 0，取样窗口 1）；
`MERGED` 每次点击整篇合成一次（大文档下有成本，未做缓存）；羽化没有绝对设置入口；
选区工具仍受引擎的 RGBA8 / 1:1 约束，其它图层给明确失败提示而非降级。

报告：`../report/S7-selection-tools-20261018-dsh.md`。

### 6.18 S8 选区工具收尾：钢笔贝塞尔 / 绝对羽化 / 魔棒取样窗口（2026-10-18，DSH）

S7 把 Marquee / Lasso / Wand 接进了外壳，但留了三处「ABI 有、UI 没有」或「口径别着」的
缺口（S7 报告 §5 的 1–3 项）。S8 按该顺序补齐，并**只补缺口**：不改 S7 已冻结的
`modify(FEATHER)` 叠加语义，也不改魔棒命中算法。

**1. 新 ABI：`comp_selection_set_feather`（第 112 个导出符号）。** 入参
`(document, feather, out_changed)`，`feather` 量程 0..250 px。它就是 S7 缺的
「绝对设置羽化」入口：

- **覆盖 vs 叠加**：`comp_selection_modify(COMP_SELECTION_FEATHER, r)` 保持
  `sqrt(cur² + r²)` 叠加（S7 已冻结、有测试），本入口是把羽化**设为**给定值；
  两者互不复用，文档与状态栏都写明是绝对值。
- `feather == 0` = 清除羽化（有效 coverage 逐字节回到硬边蒙版）；
- 无活动选区 / 空选区时是 no-op（`COMP_OK` + `changed = 0`），不产生撤销步；
- 等值设置短路（`changed = 0`）；量程闸在 `ImplicitTxn` 之前，非法值
  `COMP_ERR_INVALID_ARGUMENT` 且不写 `out_changed`；
- 只改羽化半径、不重算基础蒙版：与 `modify(FEATHER)` 同一套半径语义。

**2. 修掉 S7 的一个真实缺陷。** S7 的 `finish_new_selection` 对新选区无条件调
`comp_selection_modify(FEATHER, shell.selection_feather)`。当魔棒点空（0 匹配、
既有选区原样保留）且默认羽化 > 0 时，这一步会**改写既有选区的羽化**——用户什么都没
选中却改了选区。S8 改成：仅当 `changed != 0` 且羽化 > 0 时才调
`comp_selection_set_feather`（绝对值，不再叠加）。

**3. 钢笔工具（`P`）。** `ui_core.h` 新增与平台无关的路径数学：
`PenAnchor{position, handle_in, handle_out}`、`PenSegment{start, control1, control2, end}`、
`pen_append_anchor` / `pen_drag_handle`（对称平滑点：`out = pointer - anchor`，
`in = -out`）/ `pen_segment_count`（闭合 n 段、开放 n−1 段）/ `pen_build_segments`
（返回总段数、容量不足只写前 capacity 段）/ `pen_segment_at` / `pen_cubic_at` /
`pen_preview_polyline` / `pen_hits_first_anchor`，上限 `kPenMaxAnchors = 2048`。
外壳手势：

- 点击落锚点（上限即上表的 2048），按住拖动拉出对称控制柄，松开结束；
- 点首锚点（屏幕约 6 px 命中圈，按缩放换算）/ `Enter` 提交为
  `comp_selection_apply_path`（第一个 cubic `new_subpath = 1`，其余接续），
  钢笔选区**总按闭合子路径**解释——这是选区语义，不是图形路径语义；
- `Esc` 只取消未提交路径（不关窗），切走工具 / 新建 / 打开文档也会放弃路径；
- `Shift` = 加选、`Alt` = 减选，与框选/套索同一套修饰键；提交仍走
  `finish_new_selection`，所以「路径 + 羽化」也是一步撤销；
- 画布预览：每段按 12 点采样画折线、虚线提示闭合边、锚点方块 + 控制柄线（逐段取
  几何，不给 2048 锚点的整条路径开大缓冲）。

**4. 魔棒 N×N 取样窗口（`Alt+[` / `Alt+]`）。** 引擎侧 `apply_wand` 的 `radius` 早已
支持 0..512，缺的只是外壳入口。`ui_core` 补 `clamp_wand_radius` / `step_wand_radius`
（0, 1, 2, 4, 8 … 512 倍增/减半），`wand_click` 不再硬编码 `radius = 0`，
`Alt+[` / `Alt+]` 在魔棒激活时调窗口，状态栏显示 `(2r+1)×(2r+1)`。参考色是
**裁剪到画布/取样面内的 `(2r+1)²` 窗口像素均值**（含 alpha 的取整平均），
`radius = 0` 即 S7 的单像素行为。

**5. 测试。** 新增 6 例：`test_selection` 3 例（`set_feather` 绝对值覆盖 vs `modify`
叠加、等值短路、清除后 coverage 逐字节回硬边、撤销链 0→12→30；无选区/非法量程/
不写 `out_changed`/取消选区后 no-op；魔棒半径 0 与 1 的参考窗口差异、半径 1 容差 0
的 0 匹配保留旧选区）、`test_ui_core` 3 例（工具表 18 项 + `p`/`pen_index`、锚点
上限/对称控制柄/段数/容量截断/首锚点命中、闭合预览折线、曲线中点离开弦、魔棒半径步进）。
基线 408 → **414 用例**。

**6. 自检。** `--selftest` 新增/改写 13 项：形状 + `set_feather` 同事务 = 一步撤销、
绝对设置 8 / 等值 no-op / 251 越界 / 0 清除 / 撤销回 8、魔棒半径 0 与半径 1 的
边界种子行为、钢笔路径闭合选区 + 单步撤销 + 预览折线闭合回首锚点。90 → **103 项**，
`compositor_shell_selftest.txt` 同步重新生成（SHA256 `620f02c6b837b1ca…`）。

**7. 验证与边界。** `scripts/build.py` → **414 cases, 0 failed, 0 check failures**；
`build_windows_shell.py` + `compositor_shell.exe --selftest` → `failures = 0`、
`[PASS]`、103 项 OK、SHA256 与仓内基线一致。仍然**没有经过人手实测**：无窗口环境下
只验证到 ABI、`ui_core` 数学层与自检驱动序列。明确边界：钢笔不做锚点编辑（选中/移动/
删除已落锚点）、控制柄不与相邻锚点联动锁定、路径只提交为选区（不成为可复用矢量对象）；
魔棒窗口均值取整后再比，不是逐通道浮点平均；`MERGED` 每次点击仍整篇合成一次
（大文档缓存留待后续）；`comp_selection_set_feather` 只改半径、不重跑基础蒙版。
S7 报告 §5 剩余项（Shape / Gradient / Type 内核、MERGED 合成缓存、格式导入策略等）
不在本轮范围。

报告：`../report/S8-selection-polish-20261018-dsh.md`。

### 6.19 S9 渐变工具：`COMP_TOOL_GRADIENT`（2026-10-18，DSH）

S8 收口后，S7 报告 §5 的下一个产品条目是 Type / Shape / Gradient 一行。按边际成本
从低到高先做 **Gradient**：它是纯像素算子，不依赖字体栅格化也不依赖矢量填充几何，
而且能直接复用既有选区 coverage 通路。

**1. ABI。** 新增 `COMP_TOOL_GRADIENT = 13`（`comp_layer.h`，枚举尾部追加）；
`comp_tool_op_desc` 尾部追加 `comp_tool_gradient gradient`（`type` / 两个文档像素端点 /
`color0[4]` / `color1[4]` / `opacity`，色标是**直通 sRGB + alpha**）。**不新增导出函数**，
仍是 **112** 个：渐变是既有 `comp_tool_apply` 的第 13 种算子。既有字段偏移全部不变。

**2. 求值口径。** 图层像素 → 文档坐标用 `effective_bounds()` 的原点平移（与
Stroke / Clone 同一套口径；缩放/旋转不做逆变换，如实记为边界）。线性
`t = dot(p − start, end − start) / |end − start|²`，径向 `t = |p − start| / |end − start|`，
都夹到 0..1（端点外是纯色带，不回绕）；两色标按 `t` 直通插值，再按**预乘 source-over**
混合（所以「不透明 → 透明」只叠色、不挖洞）。`opacity` 与活跃选区的
`selection_effective_coverage` 逐像素相乘作为覆盖率，**覆盖率 0 的像素逐字节不动**。
它不读 `stroke_points`，不属于落笔族。

**3. 参数闸（全在 `txn_begin` 之前）。** 未知 `type`、非有限或 `|v| > 1e6` 的端点、
端点重合（`|end − start| < 1e-3`）、`opacity` 越界或 0、两个色标全透明——一律
`COMP_ERR_INVALID_ARGUMENT`，撤销栈与像素都不动。拒绝「端点重合」而不是静默画纯色：
后者会让宿主以为方向生效了；拒绝「双色标全透明」而不是记一步恒等撤销，同上。

**4. 16 位 / HDR。** 整幅算子写 8 位工作副本：RGBA16 先降到 8 位再写回（与
Dither / Lens 同口径），RGBA16F 直接 `COMP_ERR_STATE`。

**5. 外壳（`G`）。** `ui_core.h` 新增 `kGradientLinear / kGradientRadial`、
`gradient_type_name`（未知值如实返回 `?`，不猜）、`next_gradient_type`（未知值收敛回
线性，不产生第三态）、`kGradientMinLength = 1.0` 与 `gradient_drag_is_usable`。
手势：`G` 选渐变工具、已在该工具时再按 `G` 切线性/径向；拖出两个端点后松开一次性
`comp_tool_apply` 提交；色标固定「前景 → 透明」（第二色标没有 UI 可调，不编造背景色），
`Shift` 反向为「透明 → 前景」；短于 1 px 的拖拽外壳直接放弃并提示，不把一次手抖变成
参数错误；失焦 / 捕获丢失 / 切走工具只丢预览，不产生撤销步。画布预览：方向线 +
起点空框/终点实心块，径向额外画半径虚圆（只画几何，不预演颜色）。

**6. 测试。** `test_tool_ops` 新增 3 例：线性红→蓝渐变的通道单调 + 垂直方向恒定 +
一步撤销逐字节回滚；径向渐变的中心/半径外/中间 ramp + 选区左半遮罩（右半逐字节不动）；
四类非法参数（端点重合、双色标全透明、未知 type、opacity 0）不留撤销步且合法参数
照样成功。`test_ui_core` 新增 1 例（类型镜像数值/命名/循环 + 拖拽下限含恰好 1 px 边界）。
基线 414 → **418 用例 0 失败**。

**7. 自检与边界。** `--selftest` 新增 10 项：枚举值冻结、`comp_tool_apply`、左右端点
颜色、单步撤销、逐字节回滚、端点重合拒绝、拒绝后无撤销步 / 渲染不变。103 → **113 项**，
`compositor_shell_selftest.txt` 同步重新生成（SHA256 `ceea1ab3b3855940…`）。
`scripts/build.py` → **418 cases, 0 failed, 0 check failures**；`build_windows_shell.py` +
`--selftest` → `failures = 0`、`[PASS]`、113 项 OK。仍然**没有经过人手 Win32 实测**：
无窗口环境下只验证到 ABI、`ui_core` 数学层与自检驱动序列。明确边界：没有第二色标
取色 UI / 渐变预设 / 抖动；色标 alpha 按直通线性插值，不做「透明色 → 前景色」的特殊
插值；不做缩放/旋转逆变换；`MERGED` 每次点击仍整篇合成。Type / Shape 内核与 S7 报告
§5 其余项（MERGED 合成缓存、格式导入策略矩阵、人工 Win32 清单）不在本轮范围。

报告：`../report/S9-gradient-20261018-dsh.md`。

### 6.20 S10 形状工具：`COMP_TOOL_SHAPE`（2026-10-18，DSH）

S9 之后「Type / Shape / Gradient」一行只剩 Type 与 Shape。按边际成本先做 **Shape**：
它复用已有的路径/扫描线覆盖率基础设施，不需要字体栅格化，也不依赖外部资源。
（Type 的位图字体内核与 `MERGED` 合成缓存仍在后续轮次。）

**1. 先拆路径基元，再加形状内核。** `engine/domain/selection.cpp` 里那套「轮廓 →
扁平化 → 扫描线填充」的代码写死在选区里，形状要用同一套语义。本轮把它抽成
`engine/domain/path_raster.{h,cpp}`（`PathContour` / `PathContours` / `PathBounds`、
矩形/椭圆/圆角矩形轮廓、de Casteljau 扁平化（容差 0.1、最大递归 20）、非零缠绕扫描线
填充），`selection.cpp` 改成只做别名接线（−265 行）。这次抽取是**行为中立**的：抽完先跑
全量回归 418 用例全绿，再动形状。新增 `engine/domain/shape_raster.{h,cpp}`
（`ShapeRasterKind` / `ShapeGeometry` / `ShapeCoverage` / `ShapeRasterStatus`）：
- 填充走 `path_rasterize_contours` 的非零缠绕；
- 描边是**胶囊距离并集** `cov = clamp(0.5 + 半宽 − 距离, 0, 1) × 255`，因此端点与折点
  天然是圆的（对齐上游 `line` 的 `.round` 线帽），共线折线按 ≤32 px 切段避免长边距离场退化；
- 只算几何真正覆盖的窗口（`ShapeCoverage` 带窗口原点与 fill/stroke 两个覆盖率缓冲），
  4 行超采样 + 水平解析覆盖率做 AA，`antialias = 0` 时退化成像素中心一票；
- 点数上限 `1<<18`，超限返回 `kTooComplex`（映射成 `COMP_ERR_INVALID_ARGUMENT`），
  不吃爆内存也不静默截断。

**2. ABI。** 新增 `COMP_TOOL_SHAPE = 14`（`comp_layer.h`，枚举尾部追加）；
`comp_tool_op_desc` 尾部追加 `comp_tool_shape shape`（`kind` / 对角点或两端点 /
`corner_radius` / `segments` + `segment_count` + `closed` / `comp_shape_style`）。
几何种类 `COMP_SHAPE_RECT=0 / ELLIPSE=1 / LINE=2 / PATH=3` 与内核枚举同值。
路径段用 `comp_shape_cubic`（float 的 `start/control1/control2/end` + `new_subpath`），
ABI 层只做**结构性守卫**（指针与 1..65536 段数），坐标合法性交给域层参数闸。
**不新增导出函数**，仍是 **112** 个：形状是既有 `comp_tool_apply` 的第 14 种算子，
既有字段偏移全部不变。

**3. 参数闸（全在 `txn_begin` 之前）。** kind 越界、填充与描边全关、可见色全透明、
`opacity` 越界或 0、**描边开启时** `stroke_width` 非正、坐标为 NaN/Inf 或 `|v| > 1e6`、
矩形/椭圆宽或高为 0、直线两端点重合、路径段数 0/超限/坐标非法——一律
`COMP_ERR_INVALID_ARGUMENT`，撤销栈与像素都不动。上一轮踩到的一个坑写在这里：
**纯填充形状不看 `stroke_width`**（0 合法）——外壳的矩形/椭圆只开填充，若把线宽
无条件当必填，宿主就得为一个用不到的字段编数值，否则整条渲染通路白挂
（自检里 13 项 FAIL 就是这条导致的，已修）。

**4. 语义三处显式差异（不是遗漏）。** ① 上游画完**新建一个像素图层**，本仓工具算子统一
「画进当前图层」——工具算子不该改图层树结构，否则 merge/flatten 的逐字节契约与撤销粒度
都要跟着改；② 上游矩形/椭圆只填充、直线只描边，本仓把填充/描边做成两个独立开关
（`fill`+`stroke` 同时开是超集，先填后描）；③ 贝塞尔路径本轮只有 ABI 通路，外壳的钢笔
仍用于提交选区。另外**形状与图层完全不相交时成功、无像素改动、不产生撤销步**
（note 如实说明），否则宿主会看到一步什么都没做的撤销。

**5. 16 位 / HDR。** 整幅算子写 8 位工作副本，RGBA16 经 `merge_deep_paint` 合回
（与 Dither / Lens / Gradient 同口径），RGBA16F 直接 `COMP_ERR_STATE`。

**6. 外壳（`U`）。** `ui_core.h` 新增 `kShapeRect/kShapeEllipse/kShapeLine/kShapePath`
（数值与 ABI 一致）、`shape_kind_name`（未知值返回 `?`，不猜）、
`next_shell_shape_kind`（矩形 → 椭圆 → 直线 → 矩形，未知值收敛回矩形）、
`shape_drag_box(start, current, square, from_center)`（`Shift` = 取较长边的正方/正圆、
保留拖拽方向；`Alt` = 从中心向外；两者可叠加）、`kShapeMinExtent = 1.0` +
`shape_drag_is_usable`（直线看端点距离，矩形/椭圆看宽高都 ≥ 1 px）、
描边宽度 0.5..50（`×/÷1.2` 步进）与圆角半径 0..256（2 px 步进）的夹取/步进、
`shape_effective_corner_radius`（先夹用户量程再夹短边一半，预览与落笔同一函数）。
手势：`U` 选工具、已在该工具时再按 `U` 循环种类；拖对角点（直线拖两端点）松开提交；
`Alt+[` / `Alt+]` 调线宽、`Shift+Alt+[` / `Shift+Alt+]` 调矩形圆角；`Esc` / 切工具 /
失焦 / 捕获丢失只丢预览，不产生撤销步；短于 1 px 的拖拽外壳先放弃并提示。
画布预览是白实线 + 黑虚线双描边（矩形/椭圆/直线，圆角用 `RoundRect` 且半径按缩放换算），
只预演几何不预演像素。矩形/椭圆**只填充前景色**、直线**只用前景色描边**——外壳没有
「描边颜色/线宽面板」，不编造宿主不存在的第二个颜色（引擎支持 fill+stroke 同时开）。

**7. 测试与基线。** `test_tool_ops` 新增 5 例：矩形填充 + 一步撤销 + 未覆盖像素逐字节
不变（并冻结 `COMP_TOOL_SHAPE == 14`）；椭圆中心填、四角不填、撤销逐字节回滚，再在左半
矩形选区下重画且右半逐字节不动；直线是圆头描边（宽 4 → 轴心满、半宽仍实、超半宽为 0、
端点外 1 px 仍有覆盖而 2 px 归零、起点侧同样圆头）；贝塞尔路径的非零缠绕成洞（外环填、
内环是真空洞、框外不动）；参数闸 + 图层外 no-op（7 种非法参数都不留撤销步、合法参数
照常成功、纯填充形状的 `stroke_width = 0` 合法、开描边时 0 线宽被拒）。
`test_ui_core` 新增 1 例（种类镜像数值/命名/循环、`Shift`/`Alt`/叠加的拖拽换算、
下限判定、线宽与圆角的夹取/步进、有效圆角半径）。基线 418 → **424 用例 0 失败**
（`build.py` → **424 cases, 0 failed, 0 check failures**）。
`--selftest` 新增 24 项：枚举冻结、矩形/椭圆/直线三次 `comp_tool_apply` + 像素断言 +
单步撤销 + 逐字节回滚、填充直线拒绝且不留撤销步、图层外 no-op 不留空撤销步且渲染不变。
113 → **137 项**，`compositor_shell_selftest.txt` 同步重新生成
（SHA256 `bb416b59a614c690…`）；`scripts/run_regression.py --require-ohos` **6/6 PASS
（0 skipped）**，含 OHOS aarch64 交叉编译 rc=0。

**8. 事实边界。** 仍然**没有经过人手 Win32 实测**：无窗口环境下只验证到 ABI、
`ui_core` 数学层与自检驱动序列，「画出来对不对」（尤其是圆角矩形的合成覆盖率与
直线圆头的视觉宽度）仍需开窗目视。明确未做：形状**不进图层树**（不新建图层）、
没有形状属性面板（线宽/圆角只能快捷键改）、路径只有 ABI、Type 与 `MERGED` 合成缓存
不在本轮。口径 B 仍是 **28/48 ≈ 58%**——Type 未交付，整条不抬。

报告：`../report/S10-shape-20261018-dsh.md`。

### 6.21 S11 文字工具：`COMP_TOOL_TYPE`（2026-10-18，DSH）

S10 之后「Type / Shape / Gradient」只剩 Type。与 Shape 不同，Type 没有可复用的几何设施，
而本仓**零依赖、不链接系统字体**（Windows 壳也不引 GDI 字体来冒充落笔结果），所以这一轮
做的是**自带位图字体的栅格化内核**；CJK 明确不做——ASCII 之外的字节由参数闸如实拒绝，
不画方框顶替。

**1. 字体内核。** 新增 `engine/domain/type_raster.{h,cpp}`：
- 内置 **5×7 列主序** ASCII 点阵（`0x20`..`0x7E` 共 95 字形，每字形 5 字节、bit0 = 最上行），
  字距基数 6 列（5 列字形 + 1 列间隙）；95 个字形逐个带 `/* 0xNN 'c' */` 注释便于核对；
- 排版：`scale = font_size / 7`、水平步距 `6 * scale + letter_spacing`、行距
  `font_size * line_height`；`\n` 分行且**行尾 `\n` 产生一个额外空行**；文本框宽 =
  最长行 `字形数 × 步距`、高 = `font_size + (行数-1) × 行距`；
- 两档栅格化：`antialias != 0` 走**精确面积覆盖率**（字形格与像素格的交集，逐像素累加并
  饱和到 `uint8`），`0` 走像素中心一票的硬边——小字号不会因采样而整行消失；
- `TypeRasterStatus` 五态把「文本不合法」（`kBadText` / `kTooLong` / `kBadStyle`）与
  「文本框在图层外」（`kEmpty`）分开：前者是参数错误，后者是成功 no-op。

**2. ABI。** 新增 `COMP_TOOL_TYPE = 15`（枚举尾部追加）；`comp_tool_op_desc` 尾部追加
`comp_tool_type type`（`origin_x/y`、`text` + `text_length`、`comp_type_style`
（`font_size` / `line_height` / `letter_spacing` / `antialias`）、`color[4]`、`opacity`）。
**不新增导出函数**，仍是 **112** 个：Type 是既有 `comp_tool_apply` 的第 15 种算子，
既有字段偏移全部不变。

**3. 参数闸（全在 `txn_begin` 之前）。** 空/负文本长度、`0x20..0x7E` 之外的字节
（UTF-8/CJK、Tab、控制字符）、超过 65536 字符、`font_size` 越出 6..2048、`line_height`
越出 1..8、`letter_spacing` 把水平步距压到 0.5 px 以下、坐标 NaN/Inf 或 `|v| > 1e6`、
`opacity` 越界或 0、颜色全透明——一律 `COMP_ERR_INVALID_ARGUMENT`，撤销栈与像素都不动。
「文本框完全落在图层外」以及「一个像素都没落墨」（纯空格文本、或选区覆盖率全 0）都是
**成功 no-op**：`txn_abort` 整步撤回，不产生空撤销步（note 如实说明）。

**4. 语义与边界。** 合成是预乘 source-over，权重 = 覆盖率 × `opacity` × `color.a`；
活跃选区按 `selection_effective_coverage` 逐像素乘入，覆盖率为 0 的像素逐字节不动
（画布外 = 选区之外）；8 位工作副本，RGBA16 经 `merge_deep_paint` 合回，RGBA16F 直接
`COMP_ERR_STATE`（与 Dither / Lens / Gradient / Shape 同口径）。与上游三处显式差异
（S11 当时）：① 没有系统字体 / 字体选择 / CJK / 富文本（粗体、斜体、对齐、旋转、文字描边
都没有）——**P5-3 起字体轨道与 CJK 已接上，见 `docs/shell-parity.md` §5.45**；
② 文字是**一次性像素**，不是可再编辑的文本对象，提交后不能回改内容（撤销只能整步回滚）；
③ 上游画完新建图层，本仓写进当前图层（与所有工具算子同一条规矩）。

**5. 外壳（`T`）。** `ui_core.h` 新增 `kDefaultTypeFontSize = 32`、
`kDefaultTypeLineHeight = 1.25`、字号 6..2048（`×/÷1.25` 步进）、行距 1..8、字距量程
由「步距 ≥ 0.5 px」反推（`type_letter_spacing_min(font_size) = 0.5 - 6 × font_size / 7`，
负字距合法）、`type_block_metrics`（与内核 `build_layout` 同公式，状态栏与预览共用）、
`type_caret_height`。S11 当时的 `type_ascii_from_vk`（US 布局 VK→ASCII）已在 P5-3 删除：
字符改由 `WM_CHAR`/输入法进入，见 `docs/shell-parity.md` §5.45。
手势：`T` 选工具 → 画布点击落插入点 → 直接键入 ASCII → `Enter` 提交一次
`comp_tool_apply(COMP_TOOL_TYPE)`；`Shift+Enter` 换行、`Backspace` 删字符、
`Esc` / 切工具 / 失焦取消（只丢预览，不产生撤销步）；`[ / ]` 调字号、`Shift+[ / ]`
调字距；已在键入态时再点画布 = 先提交上一段、再在新位置落点。键入态是**模态**的：
普通字符不再触发工具快捷键（`B` 是字母不是画笔）、空格也不再是临时抓手，只放行 Ctrl
组合。画布预览只画插入点光标与文本框（白实线 + 黑虚线），**不预演字形**：外壳没有系统
字体，用 GDI 画一套只会与落笔结果不一致。

**6. 测试与基线。** `test_tool_ops` 新增 4 例（枚举冻结 + 『A』逐像素落墨 + 空首行不动 +
一步撤销逐字节回滚；`A\nB` 两行行距；AA 半像素覆盖率 + 选区遮罩；七类非法参数拒绝 +
图层外 no-op + 纯空格 no-op 都不留撤销步）；`test_tool_ops_domain` 新增 2 例（度量与落墨
面积、拒绝项与图层边界裁剪）；`test_ui_core` 新增 1 例（常量/夹取/步进/字距下限/度量/
键盘映射/工具表）。基线 424 → **431 用例 0 失败**
（`build.py` → **431 cases, 0 failed, 0 check failures**）。
`--selftest` 新增 13 项：枚举冻结、`comp_tool_apply` 落墨与空首行、单步撤销与逐字节
回滚、多行行距、UTF-8 拒绝且不留撤销步、图层外 no-op 不留空撤销步且渲染不变。
137 → **150 项**，`compositor_shell_selftest.txt` 同步重新生成
（SHA256 `b7841a5b72b24250…`）；`scripts/run_regression.py --require-ohos` **6/6 PASS
（0 skipped）**，含 OHOS aarch64 交叉编译 rc=0。

**7. 事实边界。** 仍然**没有经过人手 Win32 实测**：无窗口环境下只验证到 ABI、`ui_core`
数学层与自检驱动序列，「打出来的字好不好看、光标位置对不对」仍需开窗目视。明确未做：
ASCII-only（无 CJK）、无字体选择/粗斜体/对齐/旋转/文字描边、无可编辑文本对象、
无字形预览、没有文字属性面板（字号/行距/字距只能快捷键改）。口径 B：Type / Shape /
Gradient 三个子项全部有产品通路，该条从「部分命中」升为**命中**——**29/48 ≈ 60%**。
边界再强调一次：Type 是 ASCII 点阵，与上游 472 行富文本 TypeTool 不等价，这个 +1 只代表
「该条不再缺子项」。

报告：`../report/S11-type-20261018-dsh.md`。

### 6.22 S12 合成缓存：`composite_document_cached`（2026-10-18，DSH）

S11 收口后，编辑链路（工具算子、撤销/重做、图层结构）已全部落地，但每次 `WM_PAINT`
与每次 `comp_render_flatten` 都从零建 FilterGraph、整篇重算——外壳连续重绘同一画面时
这笔账被重复支付。这一轮只做**派生数据缓存**，不碰任何 ABI，也不改变任何像素结果。

**1. 缓存放哪、键是什么。** `Document` 新增四个私有字段：`content_serial_`（内容序列号）、
`composite_cache_`（画布级 `ImageBuffer`）、`composite_cache_serial_`（缓存对应的序列号）、
`composite_cache_valid_`。它们是**派生状态**：不进 `DocumentState`、不进事务快照、
不进撤销栈、不参与 `.comp` 序列化——丢了只会多算一次合成，对任何可观察结果零影响。

**2. 失效（invalidate）在哪。** 所有能改变合成结果的入口都让序列号 +1：
`txn_begin` / `txn_commit` / `txn_abort` / `commit_implicit` / `undo` / `redo`、
`create_layer` / `remove_subtree` / `reorder_layer` / `merge_down` / `flatten_image` /
`resize_image`，以及宿主兜底的 `mark_dirty(true)`。约束是**宁可多算一次**：漏加会读到旧
画面，多加只多算一次。ABI 侧所有写入都包在 `ImplicitTxn` 里（提交即 `commit_implicit`），
显式事务走 `txn_commit`——两族入口都已覆盖，因此**没有**给逐算子增加 hook。
唯一不经过序列号的是**域内代码/域测试直接改 `layer->pixels`**：那条路没有事件可挂，域测试
要么用 `composite_document` 现算，要么显式 `mark_dirty(true)` 标记；ABI 与外壳没有这条路径。

**3. 取值契约。** `composite_document_cached(Document&, const ImageBuffer**, std::string*)`：
- 命中（有效 且 键 == 当前序列号 且 不在事务内）→ 直接返回文档持有的只读视图；
- 未命中 → 以文档的缓存槽为目标求值 `composite_document`（同一条 FilterGraph 通路），
  成功后才登记缓存键；
- **活跃事务期间不命中、也不登记**：事务是未定型的中间态，照常求值供本次使用，但绝不让
  「事务内的半成品」在事务外被复用；
- 返回指针的生存期到下一次文档变更或下一次本函数调用为止；要留存就拷 `ImageBuffer`
  （`shared_ptr` 只增引用，不复制像素）。

**4. 接进渲染与拼合。** `CpuRenderBackend::render_document` 从「先 `composite_document`
到局部变量」改为「取缓存视图 → 按目标深度转换（`hdr_to_rgba8` / `rgba8_to_hdr`）→ 逐行
`memcpy`」；同深度时仍是零拷贝直通，8 位输出逐字节不变。`flatten_image` 直接复用缓存：
`ImageBuffer` 结构拷贝接走 `shared_ptr`，拼合层之后写像素触发 COW，不复制整幅画布。
D3D12 / Vulkan 的 GPU 路径不经过 `compositing/compositor`，本轮不接（它们本来就是每次
重画，语义不变）。

**5. 测试与基线。** 新增 `tests/test_composite_cache.cpp` 4 例：① 事务外连拍命中同一块
缓冲（指针相同、序列号不变、内容与现算逐字节一致）；② 图层结构变化 / 显式像素写入 /
撤销 / 重做都会失效重算，且撤销到底回到最初画面；③ 活跃事务期间不供缓存（事务内渲染看到
未提交像素、两次事务内渲染一致、提交/回滚后各自正确）；④ 拼合复用缓存且像素零拷贝接管。
基线 431 → **435 用例 0 失败**；`--selftest` 新增 5 项（连拍逐字节相同、编辑后下一帧
变化、撤销后逐字节还原），150 → **155 项**，`compositor_shell_selftest.txt` 同步重新生成
（SHA256 `483e8883dc5bcb9d…`）；`run_regression.py --require-ohos` **6/6 PASS
（0 skipped）**，含 OHOS aarch64 交叉编译 rc=0。

**6. 事实边界与代价。** 缓存是**一条**画布级缓冲（RGBA8 或 RGBA16F，跟
`composite_surface_format` 走），不进 512 MB 历史预算、也不算进历史内存记账——它的大小
恒等于一张画布，不随编辑步数增长。缓存命中/失效都发生在同一次渲染调用栈内，**同一文档的
并发渲染仍不是线程安全的**（与既有 `comp_render_*` 契约一致）。它不改变任何像素结果、
不改变任何 ABI、不改变口径 B——**29/48 ≈ 60%** 不动。性能收益本机只做了「连续两次渲染
第二次不再走 FilterGraph」的定性验证，没有做基准计时，不宣称倍数。

报告：`../report/S12-merged-20261018-dsh.md`。

### 6.23 S13 GPU 设备侧接入主渲染路径 + 格式导入矩阵（2026-10-18，DSH）

S12 之后 M3 只剩最后一块硬骨头：**GPU 协同**。`rhi.h`、D3D12 后端、HLSL 内核、`--gpu`
构建开关都在，外壳自检也一直打印 `backend = D3D12`——但**设备从未真正渲染过一帧**。
本轮先证伪、再修复、再给可复验的证据，不拿「后端名是 D3D12」当结论。

**1. 先证伪：GPU 为什么一直没跑。** 链路是：根签名把 `dst/src` 纹理直接当**根 UAV**
描述符 → D3D12 规范只允许根 UAV 描述 buffer、不允许描述 texture →
`CreateComputePipelineState` 返回 `E_INVALIDARG (0x80070057)` → `ensure()` 失败后
`gpu_.reset()` → 下一帧重新建 D3D12 设备（冷启动 ~100 ms）→ 渲染失败 → ABI 按文档
静默回落 CPU。像素测试全绿、`comp_render_backend` 报告 D3D12，于是「GPU 已接入」
的假象维持了两轮（上一轮记录的性能数据也建立在「每帧重建设备」上，不可信）。
**这类静默回落必须能被观测**，否则同样的事总会再发生一次。

**2. 三处修复。**
- **描述符表替代根 UAV**：根签名改成 2 个参数——`b0` 32 位常量 + 1 个
  `D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE`（两个 UAV range：`u0 = dst`、`u1 = src`），
  在 `ensure()` 里建 2 slot 的 shader-visible CBV_SRV_UAV 堆并写 UAV。根 UAV 描述 texture
  是**规范限制**（不是驱动 bug），描述符表才是可移植路径。
- **回读方向**：`COPY_DEST` 的回读缓冲必须当 copy 的**目的地**。此前复用了「上传」的
  `copy_into`（buffer 当源）→ `Close()` 返回 `E_INVALIDARG`；新增 `copy_out`
  （`PLACED_FOOTPRINT` 作为 destination placement，buffer→texture 的 `copy_into` 保持不变）。
  这处用独立探针按「清屏 / +图层 / +barrier+dispatch / +回读」四变体二分定位，前三变体
  `hr=0`、第四变体失败，才把范围收窄到回读拷贝。
- **持久上下文 + 能力闸门**：`GpuContext`（device/queue/allocator/list/root/PSO/堆/fence/
  textures/readback/upload blocks）随引擎常驻，一帧一次 `Reset`/`Close`/`Execute`；
  `AUTO` 选择加 `caps().offscreen_compute` 闸门（否则本机可创建但不会渲染的 Vulkan
  会被选中），`probe_backend` 结果按后端枚举进程内缓存，避免每次探测都建/毁设备。

**3. 让「真上设备」可观测、可断言。** `RenderBackend` 新增内部虚函数
`last_render_offloaded()`（纯 C++ RHI，**不进 C ABI**）：D3D12 只有在
「上传 → dispatch → fence → 回读」整条链路成功后才置 true，CPU 后端恒为 false。
新增 `tests/test_rhi.cpp::rhi_gpu_backend_really_offloads_an_eligible_document`：直接持有
D3D12 实例渲染 32×32 全画布文档，断言 `can_render_document` + `render_document` +
`last_render_offloaded() == true` + 回读像素确实被写；无设备/未编入 D3D12 时跳过而不是失败。
像素对拍仍是 ±1 LSB（GPU 走 float、CPU 镜像走 double）：本机 RX 6800 上
`max_diff = 1`、超差 0 字节。另补一例 `rhi_gpu_matches_cpu_for_translucent_screen_stack`
锁住「半透明底 + 不透明度 + SCREEN」——外壳自检这个场景原先用了**非预乘**像素
（`200,64,32,128`，颜色 > alpha），GPU 浮点与 CPU 整数在饱和钳制上差 **34 级**；
那已超出 `comp_layer.h` 的预乘契约（非法输入，两后端不做一致性承诺），改成合法预乘值
`100,32,16,128` 后对拍 `max_diff = 0`，这个坑现在由用例和注释一起钉住。

**4. 实测数字（AMD Radeon RX 6800，1024²，3 层，同场景 AUTO vs 显式 CPU）。**

| 指标 | AUTO(D3D12) | 显式 CPU |
|------|------------|----------|
| 冷启动（首帧，含建设备 + PSO） | 151.7 ms | 152.6 ms |
| 稳态（5 次平均） | **3.01 ms** | **0.65 ms** |

稳态拆解：录制含上传 0.06–0.9 ms、dispatch 0.03 ms、fence 等待 0.19–1.28 ms、
回读拷贝 0.02–0.26 ms。尺寸扫描（AUTO 稳态）：256² 0.29 ms / 512² 0.80 ms / 1024² 2.91 ms。
**如实结论：当前「离屏上传 + 整幅回读」的 GPU 路径比 CPU 慢**（每层上传 + PCIe 往返 +
回读），本轮不宣称任何加速，只宣称「设备真的参与、结果正确、为交换链路径打好地基」。
真正可能带来收益的是下一步的 **DXGI 交换链 `present`（不回读、直接上屏）**，
在此之前没有性能结论。→ **2026-10-19 S16 已接直呈并实测**：1024² 三层场景下
GPU 直呈 3.04–3.27 ms vs CPU flatten+GDI 3.85–3.91 ms（约 1.18–1.29×），
瓶颈在逐层上传而非上屏，见 §6.26。

**5. 边界（未变）。** GPU 子集仍是**全有或全无**：组图层、`parent != 0`、剪贴源、蒙版、
调整、效果、旋转/翻转、非全画布或非 RGBA8 图层 → 本次调用回落 CPU，像素与 CPU 基准
**逐字节一致**；绝不渲染半个图层栈。窗口表面在 S13 仍返回 `UNSUPPORTED`（交换链未落地，
**S16 已接直呈**，见 §6.26）。
本机 D3D12 调试层未安装（`D3D12GetDebugInterface` → `0x887A002D`），故新增
`COMP_D3D12_DEBUG` 环境变量开关（有 SDK 组件时启用 InfoQueue 并在 `Close()` 失败时打印
最多 12 条消息），默认零开销。默认构建仍不编入 D3D12：`python scripts/build.py --gpu`
与 `python scripts/build_windows_shell.py --gpu` 才启用设备路径，基线可重复。

**6. 同时交付：格式导入策略矩阵（s7）。** `docs/format-import-matrix.md` 写清
HEIC/WebP/SVG/PSB 的判定链（magic → 注册表 → 扩展名 → 显式拒绝，绝不按扩展名猜）、
四格式的能力矩阵与拒绝原因、失败不变式；`tests/test_import_matrix.cpp` 4 例锁死
「扩展名映射显式」「不可用格式在触 IO 前拒绝」「未知格式不猜」「导入失败文档不被污染」。
这是**文档 + 护栏**，不新增解码器，也不改变任何既有失败语义。

**7. 基线。** 435 → **444 用例 0 失败**（+5 RHI GPU/回落/半透明对拍 + 4 格式导入矩阵），
默认构建与 `--gpu` 构建**都是 444/0**；Windows 壳自检 155 → **160 项 0 失败**，
基线 `compositor_shell_selftest.txt` 重新生成（SHA256 `772201aaad4f7263…`）；
`run_regression.py --require-ohos` **6/6 PASS（0 skipped）**，含 OHOS aarch64
`compile ok=90 fail=0` rc=0；口径 B 维持 **29/48 ≈ 60%**。ABI 导出仍 **112** 个，
结构体布局/枚举值/工具算子零改动。

报告：`../report/S13-gpu-device-backend-20261018-dsh.md`。

### 6.24 S14 文字属性面板 + 行距快捷键（2026-10-19，Claude）

S11 报告 §5 留了一条明确的缺口：「没有文字属性面板：字号/行距/字距只能快捷键改；
行距只有 1..8 的夹取函数，壳里尚未提供改行距的按键（默认 1.25）」。本轮只补这一条，
**引擎核与 C ABI 零改动**（导出仍 112 个，`COMP_TOOL_TYPE = 15`、`comp_tool_op_desc.type`
布局不变）。

**1. 面板模型（`ui_core.h`，纯函数、可单测）。** 三行固定行表
`Font Size / Line Height / Letter Spacing`（`kTypePanelRowCount = 3`），
行高 20、首行距面板顶 54（`kTypeRowHeight` / `kTypeRowsTop`），
`type_param_row_rect` 与 `type_param_row_at` 供绘制与命中共用——与滤镜面板同一套
「看得见就点得中」的口径。夹取/步进按行分派到既有量纲：
字号乘性 ×/÷1.25（6..2048），字距线性 ±1 px 且下限仍是
`0.5 - 6 × font_size / 7`，行距新增 `step_type_line_height`（**5% 一步，夹在 1.0..8.0**）。

**2. 外壳（`main.cpp`）。** `ShellState` 增 `type_panel_focused` / `type_input`；
`present()` 在 `tool == Type` 且滤镜面板未打开时把右侧面板切到 `draw_type_panel`
（标题、三行值、文本框度量、输入回显、快捷键提示）。交互：

- 鼠标点行 = 聚焦（`type_param_row_at`，与绘制同源）；
- `Tab / Shift+Tab` 切行，`- / =` 调当前行，数字键开始键入，`Enter` 提交
  （`strtod` 必须完全消费且 `isfinite`，再夹到量纲内），`Esc` / 退格回退；
- `Alt+[ / ]` 调行距（`WM_SYSKEYDOWN`），**键入文字时也可用**——这是 S11 缺的那条按键；
- 面板数字输入为**非负值**：空缓冲时 `-` 是「步进」快捷键，负字距请用 `Shift+[`（与滤镜/调整面板现有输入规矩一致，已记入边界）；
- 切到 Type 会清掉调整面板的数值缓冲，离开 Type 会清掉面板输入并丢弃未提交文字，
  两个面板不会同时处于输入态；
- 状态栏文案同步补上面板与 `Alt+[ / ]` 提示。

**3. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py` | 444 → **445 cases, 0 failed, 0 check failures**（新增 1 例面板行表/夹取/步进/命中） |
| `python scripts/build_windows_shell.py` | 通过，无 error |
| `compositor_shell.exe --selftest` | 160 → **164 项 0 失败**，基线重新生成（SHA256 `aa2e2754122d5d5d…`） |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**（OHOS aarch64 `compile ok=90 fail=0`） |
| ABI 导出 | **112**（未新增） |

**4. 边界（如实记录）。** 仍然**未经人手 Win32 目视实测**（ALEX 明确推迟）：
面板布局、点击命中、键入回显只验证到 `ui_core` 数学层、壳自检与编译期；
「面板长得好不好看、点得准不准」需要开窗确认。面板值随会话保留，**不写进 `.comp`**；
字体仍是 ASCII 点阵、画布仍不预演字形（S11 边界不变）；行距在面板上以整数百分比显示
（内部是 double，`Alt+[ / ]` 与数值键入都走同一夹取）。

报告：`../report/S14-type-panel-20261019-claude.md`。

### 6.25 S15 拖动提交缺陷修复 + 自动化目视（2026-10-19，DSH）

S4–S14 的每一份报告都写着同一条边界：「未经人手 Win32 目视实测」。ALEX 把人工目视
推迟了，于是本轮换个做法：**把「开窗点一遍」写成可重跑的命令**，再用它去找真 bug。
结论是找到了一个从 S4 潜伏至今的提交缺陷。**引擎核与 C ABI 零改动**（导出仍 112 个）。

**1. 真缺陷：`ReleaseCapture()` 早于载荷搬出。** `paint_stroke_end`（画笔/橡皮擦）、
`retouch_stroke_end`（模糊/涂抹/液化）、`lasso_end`（套索）三处旧顺序是「先
`ReleaseCapture()`，再把 `stroke_points` / `lasso_points` 搬出去提交」。
`ReleaseCapture()` 会**同步**派发 `WM_CAPTURECHANGED`，而该处理器为「丢捕获 = 取消
手势」调用 `stroke_cancel` / `lasso_cancel` 清空折点 —— 于是真人拖完松手，画面上什么
都没有、撤销栈也不加步；而 `--selftest` 不建窗口、不走消息泵，永远测不到。
修法：先 `swap` 出折点到局部变量、再 `ReleaseCapture()`、再提交，三处同改并加中文注释
说明为什么不能调换。其余 `*_end`（`marquee` / `gradient` / `shape` / `move` / `pen_up` /
`eyedropper`）的 `*_cancel` 只清 active 标志，逐一核对**不受影响**。

**2. `scripts/visual_check.py`（新增）：自动化目视。** 启动真实外壳，`AttachThreadInput`
+ `SetKeyboardState` + `PostMessage(WM_KEYDOWN/WM_SYSKEYDOWN)` 发真键、`PostMessage`
发真鼠标消息（不动物理光标）、`PrintWindow(PW_RENDERFULLCONTENT)` 抓客户区像素。
关键约束：拖动类工具依赖 `SetCapture`，窗口必须**真前台**（`GetForegroundWindow` 校验，
失败即抛错）；空文档的纯黑外接框用来定位文档矩形，坐标不写死。
11 场景 45 项断言 32 张 PNG（`../report/assets/s15/`）：`start / brush / selection /
wand / gradient / shape / type / filter / adjustment / layers / view`。几个实测证据：

- 画笔松手后提交 17 个折点、画布出现橙色笔迹（修复前像素差 0）；
- 文字：32 px 字形外接框高 16 px，改成 48 px 后 23 px；`Alt+]` 行距 125% → 135%；
- 调整：铺底 `#336699` → 新图层挂 Exposure 抬值应用 → `(51,102,153)` 变 `(116,218,255)`，
  `Ctrl+Z` 逐字节回到 `(51,102,153)`；
- 视图：适合窗口 49% → 放大后应用底色像素 175457 → 50395。

写脚本时确认了两条**语义**（不是 bug）：调整是「调整图层、只作用于下方内容」
（上游 `EditorCanvas.swift:3092`、引擎 `compositing/filter_graph.cpp:503`），挂在唯一
像素层上本来就看不到变化——脚本改成先建新层再挂调整；`Ctrl+=` 是 `VK_OEM_PLUS`（0xBB）
而不是字符 `'='`，早先用 `ord("=")` 发键会让缩放静默失效。同时把调整面板底部提示补到
7 行（含「调整只作用于下方 · 挂在新图层上」），避免长提示被面板右边界/底边裁掉。

**3. `docs/manual-test.md`（新增）：人工清单。** M1–M16 手把手步骤 + 预期文案 +
脚本盲区（手感、蚂蚁线流动、跨 DPI、多显示器、模态对话框、退出清理、GPU 回退观感）+
「脚本场景 ↔ 清单章节」对照表 + 结论模板。**它不替代 ALEX 的目视结论。**

**4. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py` | **445 cases, 0 failed, 0 check failures**（引擎零改动） |
| `python scripts/build_windows_shell.py` | 通过，无 error |
| `compositor_shell.exe --selftest` | **164 项 0 失败**，`compositor_shell_selftest.txt` 逐字节未变 |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**（OHOS aarch64 `compile ok=90 fail=0`） |
| `python scripts/visual_check.py` | **45 项检查，0 失败**（11 场景 / 32 截图） |
| ABI 导出 | **112**（未新增） |

**5. 边界（如实记录）。** 证据是**机器抓的像素**，不是人眼：手感、闪烁、光标形态、
跨 DPI 仍需 ALEX 按清单走一遍（S4–S14 那条边界从「没人碰过」变成「脚本碰过且留了
可复核截图」，不算消除）。脚本会抢前台窗口（`SetCapture` 需要），跑时别用键鼠。
沿用 S13 边界：GPU 仅覆盖子集；「不比 CPU 快」这一条在 S16 直呈路径下已改成
「只快 1.18–1.29×，瓶颈在逐层上传」（§6.26）；
`Z` 缩放工具按钮可选中但点击暂不改缩放；文字仍 ASCII-only；
ML / RAW / HEIC / WebP / SVG / PSB / TIFF 不在范围内。

报告：`../report/S15-visual-drag-fix-20261019-dsh.md`。

### 6.26 S16 DXGI 交换链直呈（2026-10-19，DSH）

S13 把设备接进了离屏合成路径，但窗口表面仍返回 `UNSUPPORTED`：合成完要把整幅结果回读
到 CPU 再交给 GDI。S16 补上这条缺口——**合成结果直接拷进 DXGI 后备缓冲，由
`IDXGISwapChain1::Present` 上屏，全程不回读**。引擎核与 C ABI 零改动（导出仍 112 个）。

**1. 实现（`engine/render/d3d12_backend.cpp` + `.h`）。** `GpuContext` 新增
`IDXGIFactory2` / `IDXGISwapChain1` / `IDXGISwapChain3`，以及**创建时取一次、长期持有**的
两块后备缓冲 `ID3D12Resource* window_buffers[2]`。`ensure_swapchain(HWND,W,H)` 建
`DXGI_FORMAT_RGBA8_UNORM` / `BufferCount=2` / `DXGI_SWAP_EFFECT_FLIP_DISCARD` /
`DXGI_SCALING_STRETCH` 的 flip-model 链（`CreateSwapChainForHwnd`，`MakeWindowAssociation(NO_ALT_ENTER)`），
再 QI `IDXGISwapChain3`、`GetBuffer(0/1)` 常驻持有。`render_document(HWND)` 用
`GetCurrentBackBufferIndex()` 轮转，`CopyResource` 进当前缓冲（`PRESENT→COPY_DEST→PRESENT`
屏障），`present()` 调 `Present(0,0)`；尺寸/窗口变化或上屏失败时整块上下文丢弃重建。
`caps().window_present` 为 true 时(`D3D12` 构建) `COMP_SURFACE_HWND` 才走该路径，否则照旧
`COMP_ERR_UNSUPPORTED` 并由 AUTO 回落 CPU。

**2. 真缺陷与根因（本节最值得记的部分）。** 第一版按「`GetBuffer(0)` → 用 → `Release()` →
`Present()`」写，实测**只能上屏一帧**：第二帧 `ExecuteCommandLists` 之后设备直接被移除
（`Present` 返回 `0x887A0005`，`GetDeviceRemovedReason()` = `0x887A002B`
`DXGI_ERROR_ACCESS_DENIED`）。用 8 个一次性最小探针把变量拆到只剩「DXGI 与用法」：

| 探针 | 做法 | 结果 |
|------|------|------|
| 纯 DXGI | `GetBuffer(0)`→`Release`→`Present`，无 GPU 工作 | 6/6 帧 `S_OK` |
| 空命令表 | 每帧执行空命令表再 `Present` | 4/4 帧 `S_OK` |
| 屏障 / Clear | 后备缓冲 `PRESENT→COPY_DEST→PRESENT`（±`ClearRenderTargetView`） | **第 1 帧 `S_OK`，第 2 帧必挂** |
| 参数矩阵 | `FLIP_SEQUENTIAL` / `SCALING_NONE` / `BufferCount=3` / `BGRA8` | 四种组合全部第 2 帧挂 |
| 显式适配器 | 设备+交换链都建在带输出的 adapter 0 | 同样挂（排除适配器错配） |
| D3D11 对照 | 同窗口/同格式/同 flip 参数，D3D11 设备 | **5/5 帧 `S_OK`** |
| WARP(D3D12) | 换微软软件适配器跑同一循环 | **同样第 2 帧挂**（排除 AMD 驱动） |
| 标准写法 | `GetCurrentBackBufferIndex()` + 每缓冲常驻 RTV/引用 | **4/4 帧 `S_OK`** |

结论：不是显卡、不是驱动、不是桌面/会话（`OpenInputDesktop` 成功、`GetSystemMetrics(SM_REMOTESESSION)=0`），
而是**flip-model 下 D3D12 的后备缓冲必须按索引轮转并常驻持有**——每帧 `GetBuffer(0)`
拿到的是刚 `Present` 出去的那块，再写它等于写正在扫描输出的表面，DXGI 直接把设备判为
移除。D3D11 运行时内部代管缓冲轮转，所以同样写法在 D3D11 上不炸，这也是这个坑在
「D3D11 经验」下极易踩中的原因。修法就是上表的「标准写法」，并在代码里用中文注释钉住
「为什么不能每帧 `GetBuffer(0)` 再 `Release`」。

**3. 护栏（`tests/test_rhi.cpp`）。** 既有的 `rhi_gpu_presents_eligible_document_to_hwnd_without_readback`
从「渲染 1 帧 + Present 1 帧」扩成**连续 4 帧**：每帧都断言 `last_render_offloaded()`、
后备缓冲回读与 CPU 基准 `max_diff <= 1`，最后断言 `presented_frames() == 4` 且
`readback_frames() == 0`。单帧用例抓不到这个缺陷，4 帧循环把它变成回归项。

**4. 真机基准（`compositor_shell.exe --present-bench[=N]`，新增）。** 交互壳不能直接用直呈：
画布上还叠着 GDI 绘制的面板/选区蚂蚁线/缩放，混用会互相覆盖。所以基准走**独立窗口**：
1024²、底色 + 3 层整幅画布图层（NORMAL/MULTIPLY/SCREEN，不透明度 1.0/0.75/0.5，全部是
GPU 子集允许的形态）、预热 3 帧、默认 20 帧，结果写 `present_bench.txt` 并按需回显到
父控制台。同一进程内对比两条上屏路径（AMD Radeon RX 6800，30/60 帧各跑三次）：

| 指标 | GPU 直呈（render(HWND)+present） | CPU flatten + RGBA→BGRA + `StretchDIBits` |
|------|-------------------------------|------------------------------------------|
| 平均 | **3.04 – 3.27 ms** | **3.85 – 3.91 ms** |
| 最好 / 最差 | 2.33 – 2.46 / 4.51 – 5.77 ms | — |
| 只算 CPU 展平 | — | 2.94 – 3.02 ms |

**如实结论**：直呈比「CPU 展平 + GDI 上屏」快 **1.18–1.29×**，只快两成，不是数量级。
原因是瓶颈不在上屏而在**逐层上传**：每帧 4 块 1024² 纹理经 PCIe 上传 + 4 次 dispatch 已经
吃掉约 3 ms（与 S13 记录的 3.01 ms 稳态一致），省掉的只是回读 + GDI blit 那一段。
要真正拉开差距，下一步得做**逐层 GPU 纹理缓存**（避免每帧重传）；这需要给图层加内容
版本号（`ImageBuffer::pixels` 是 `shared_ptr` + COW，当前没有修订计数，就地写入会让缓存
静默过期），属于独立一轮工作，本轮不宣称。

**5. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py --gpu` | **447 cases, 0 failed, 0 check failures**（+2，S16 两例） |
| `python scripts/build.py`（默认） | **447 cases, 0 failed, 0 check failures** |
| `python scripts/build_windows_shell.py --gpu` + `--present-bench=30` | `result=ok`，`hwnd_target=ok`，三次重复见上表 |
| `compositor_shell.exe --selftest` | **164 项 0 失败**，`compositor_shell_selftest.txt` 逐字节未变（SHA256 `AA2E2754…`） |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**（OHOS aarch64 `compile ok=90 fail=0`） |
| ABI 导出 | **112**（未新增；S16 只动内部 RHI 与 Shell 基准） |

**6. 边界（如实记录）。**

- 直呈只在 **D3D12 构建 + 合格文档**（整幅画布、RGBA8、无组/蒙版/调整/效果/旋转/裁剪）时启用；
  其余文档仍逐字节回落 CPU，不渲染半个图层栈。
- 交互外壳的**主画布仍走 CPU 展平 + GDI**：直呈窗口与 GDI 覆盖层（面板、蚂蚁线、笔迹预览）
  不能安全混用，直呈目前是「能力已通 + 可复现基准」，没有替换交互路径。
- 多显示器 / 跨 DPI / 窗口 resize 后的交换链重建只做了尺寸变化重建，**未经人手目视**
  （`docs/manual-test.md` 里 M1–M16 仍未勾选）。
- 鸿蒙壳的窗口表面仍如实返回 `UNSUPPORTED`；OHOS 真机 `hdc list targets` 仍为空。

报告：`../report/S16-dxgi-present-20261019-dsh.md`。

### 6.27 S17 GPU 子集按单元拆细 + 逐层纹理缓存修订号设计（2026-10-19，DSH）

S13 把设备接进离屏路径、S16 打通窗口直呈，但两者共用同一道**整篇闸门**
（`gpu_document_ready`）：文档里只要有一层带蒙版/效果/组/剪贴/调整/旋转/非全画布，
就把**整篇**文档回落 CPU。用户只要给一张图层加个蒙版，整份文档的 GPU 加速就没了。
S17 把闸门拆成**按合成单元**：能被 GPU 独立混合的部分照常上设备，只有真正做不到的
才回落。引擎核与 C ABI 零改动（导出仍 112 个）。

**1. 单元分类（`engine/render/gpu_plan.{h,cpp}`，新增）。** 按 `FilterGraph` 的绘制序
走一遍文档，产出 `std::vector<GpuUnit>`：每个单元是「画布尺寸的预乘源 + 混合模式 +
不透明度」。分类规则：

| 形态 | 单元 | 做法 |
|------|------|------|
| 整幅画布、RGBA8、无蒙版/效果/调整、旋转=0、无翻转、几何恒等 | **原生** | 图层像素直接上传 |
| 裁剪/偏移（`effective_bounds` ≠ 画布） | 烘焙 | CPU 合成到透明画布 → 上传 |
| 带蒙版 | 烘焙 | 蒙版在烘焙时乘进预乘色 |
| 带效果（含多层效果链） | 烘焙 | 效果在烘焙时求值 |
| 旋转 / 翻转 / 缩放 / 非整幅采样 | 烘焙 | 变换在烘焙时完成 |
| 16 位（RGBA16）源图层落在 8 位画布 | 烘焙 | 深度转换 + 量化一次 |
| 隔离组 | 烘焙（整组一个单元） | 子树先自合成到透明缓冲，再按组的混合模式/不透明度混合 |
| 剪贴栈 | 烘焙（整栈一个单元） | 基底 + 成员按 CPU 剪贴语义烘焙成一个贡献 |
| 穿透组 | 展开（不产生单元） | 组不透明度乘进每个子层，组混合模式不参与（与 CPU 同口径） |
| 调整层 | **整篇回落** | 调整作用于「已合成画布」，不是图层自身贡献 |
| HDR（RGBA16F）文档表面 | **整篇回落** | D3D12 路径只实现 8 位目标 |

关键技巧：`bake_layer_contribution`（`compositing/composite_ops`，新增）把图层复制成
`COMP_BLEND_NORMAL`、混合进一张**全透明**画布。W3C 混合公式在 dst=0 时正好退化为
「源的贡献」（预乘色 × 蒙版 × 效果 × 不透明度 × 剪贴因子），GPU 随后用**原混合模式**
和 alpha=1 混合这个贡献，就等价于 CPU 的一次 `blend_pixels`。隔离组与剪贴栈复用
`filter_graph` 里已有的求值函数（`build_isolated_group_surface` /
`build_clipping_stack_surface` 从 `eval_node` 里提出来共享），新增
`bake_filter_unit_surface` 作为统一入口——CPU 合成与 GPU 烘焙**同一份代码**，不会漂移。

**2. 后端接线（`engine/render/d3d12_backend.cpp`）。** 删除 `gpu_document_ready` 与
「收集整幅图层」的 `paint` 数组：`can_render_document` = `build_gpu_plan(doc, nullptr,
nullptr, error)`（只分类、不分配像素）；`render_document` 先建计划，再
`ensure_upload_blocks(units.size()+1)`，逐单元上传（原生拷图层行、烘焙拷
`unit.baked` 行）并用 `unit.blend` / `unit.opacity` 作 dispatch 常量。窗口直呈与
离屏回读两条路径都不变。`caps().note`、`d3d12_backend.h`、`rhi.h` 的能力说明同步改成
「按单元」口径，不再写「simple stack subset」。

**3. 精度口径（如实记录，不混用 ±1）。**

| 单元类型 | 与 CPU 基准的偏差 | 原因 |
|----------|------------------|------|
| 原生 | **≤1 LSB**（既有口径） | 像素逐位上传，只差 GPU 混合的整数舍入 |
| 烘焙 | **≤2 LSB**（实测 2） | 「蒙版 × 不透明度 × 效果」乘进预乘色时量化一次 8 位，经 MULTIPLY/SCREEN 的颜色乘积项放大到 2 |

这是「少一次逐像素 CPU 混合 vs 一次 8 位量化」的显式取舍，不是画面错误；测试里按 2
断言并把原因写进用例注释，不假装还是 ±1。

**4. 护栏（`tests/test_rhi.cpp`，+4 例）。**

- `rhi_gpu_plan_classifies_units_instead_of_rejecting_documents`（默认构建也跑）：一个
  塞满裁剪/蒙版/穿透组/隔离组/剪贴栈的文档必须分类成功（原生 3 / 烘焙 2 / 隔离组 1 /
  剪贴栈 1 / 穿透组 1），并把**计划自己的单元序列**用同一条 CPU 混合公式重组整篇，
  与 CPU 合成器结果对拍（≤1 LSB）——证明计划没漏、没重、没乱序。
- `rhi_gpu_plan_names_remaining_fallbacks`：调整层与 HDR 表面仍整篇回落，且错误信息
  必须**具名**（`adjustment` / `HDR`），不能笼统说「不支持」。
- `rhi_gpu_renders_cropped_and_masked_layers_like_cpu`（真机）：32²、裁剪 MULTIPLY
  0.75 + 16×16 蒙版 SCREEN 0.8，断言真的上设备（`last_render_offloaded()`）且与 CPU
  基准 ≤2 LSB。
- `rhi_gpu_renders_group_and_clipping_units_like_cpu`（真机）：穿透组 + 隔离组 + 剪贴栈
  同一文档，断言上设备且 ≤1 LSB。

**5. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py`（默认） | **451 cases, 0 failed, 0 check failures**（+4） |
| `python scripts/build.py --gpu` | **451 cases, 0 failed, 0 check failures** |
| `python scripts/build_windows_shell.py` + `compositor_shell.exe --selftest` | **164 项 0 失败**；默认构建自检产物 SHA256 `AA2E2754…` 与 S16 基线逐字节一致（壳代码未改），`--gpu` 构建项数不变、仅能力说明文本随 `caps().note` 更新 |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**（OHOS aarch64 `compile ok=90 fail=0`） |
| ABI 导出 | **112**（未新增；S17 只动引擎内部合成/渲染） |

**6. 逐层纹理缓存修订号设计稿（`docs/gpu-layer-revision.md`，只设计不实施）。**
S13/S16 的实测已经证明瓶颈是**逐层上传**（1024²/4 层≈3 ms），S17 之后 CPU 侧还多一次
烘焙，缓存收益更大。但现在没法安全缓存：`Document` 只有**整篇** `content_serial_`
（任何一层动一下全篇失效），`Layer` 没有修订号，`ImageBuffer` 是 `shared_ptr` + COW
（唯一持有者时画笔原地写，指针不变 → 缓存会静默读到旧画面）。设计稿给出：
`Layer::content_revision` + `Document::allocate_revision()`（单调、文档内唯一、同值⟺同内容）、
必须 bump 的写入清单（像素/蒙版/效果/调整/几何/混合/结构/画布 + 快照恢复保留旧号）、
缓存键 `(layer.id, revision, w, h, format)` + 持久堆复用 + LRU、以及两轮验收计划
（R1 修订号与失效测试走默认构建；R2 纹理缓存 + `texture_uploads()` 计数 + 真机基准复测）。
本轮**不实施**，不宣称任何加速。

**7. 边界（如实记录）。**

- 仍整篇回落：**调整层**（需要分段回读已合成画布，独立一轮）、**HDR/RGBA16F 表面**。
- 烘焙单元有 **≤2 LSB** 量化差（见上表），原生单元仍是 ≤1 LSB。
- **不宣称加速**：S17 只扩展覆盖面；烘焙本身是 CPU 成本，逐帧上传未变，真机基准未重测。
- Vulkan 的文档路径仍未接线（`can_render_document` 恒 false）。
- 交互壳主画布仍走 CPU 展平 + GDI；**Win32 人工目视仍未做**（`docs/manual-test.md`
  的 M1–M16 保持未勾选，ALEX 已推迟这项）。

报告：`../report/S17-gpu-per-unit-20261019-dsh.md`。

### 6.28 S18 逐层内容修订号（R1）落地（2026-10-19，DSH）

S17 的结论是：S13/S16 的实测已经把瓶颈指到**逐层上传**（1024²/4 层≈3 ms），要真正拉开
差距必须做逐层纹理缓存；但当时没有任何东西能告诉缓存「这层内容变没变」，所以 S17 只交付了
设计稿。S18 把设计稿的 **R1** 实施完：号发了、全部写入点接了、不变量被测试钉住了。
**本轮还没有缓存，因此一个加速数字都不宣称。**

**1. 为什么三个现成候选键都不能用。**

| 候选键 | 为什么不行 |
|--------|-----------|
| `ImageBuffer::pixels` 地址 | `shared_ptr` + COW：图层是唯一持有者时画笔/滤镜/液化**原地写**，地址不变 → 缓存命中旧像素（静默出错） |
| 像素哈希 | 1024² RGBA8 全量读一遍 ≈ 4 MB，成本约等于上传的一半，收益归零 |
| `Document::content_serial_` | **整篇**序号：随便动一层全篇失效 → 逐层缓存零收益 |

**2. 数据与发号器。**

```cpp
struct Layer { uint64_t content_revision = 0; /* 0 = 尚未分配 */ };

// Document
uint64_t allocate_revision() noexcept { return next_revision_++; }  // 只增，从 1 起
uint64_t touch_layer(comp_layer_id id) noexcept;                    // 找不到返回 0
void     touch_all_layers() noexcept;                               // 换画布 / 加载工程
```

三条不变量（写进 `layer.h` / `document.h` 注释，测试逐条验）：

- **单调**：`next_revision_` 只增不减，撤销/重做不让计数器回落；
- **同值 ⟺ 同内容**：一个号只对应一份内容，内容再变必须换号。这是缓存唯一的安全前提，
  所以「同值重写也换号」的过失效是刻意允许的；反向（换号 ⟹ 内容不同）**不**保证；
- **不序列化**：不进 `.comp`、不进 `DocumentState` 的语义比较——它是会话内派生数据，
  同一份工程两次加载拿到不同的号完全正常。

`next_revision_` 与合成缓存的 `content_serial_` 并列放在 `Document` 私有区，不进
`DocumentState`，所以历史/事务/`.comp` 的可观察语义一个字节都没变。

**3. 换号点（22 个调用点 + 3 个新建路径）。** 逐层换号的是 14 个 `comp_layer_*` 写入口
（`set_pixels`/`set_mask`/`set_blend`/`set_opacity`/`set_transform`/`set_visible`/
`set_sampling`/`add_adjustment`/`update_adjustment`/`update_adjustment_at`/
`remove_adjustment`/`set_effect`/`remove_effect`/`set_isolated`）、`selection_move_pixels`
（仅 `changed` 时）、`resize_image` 与 `load_project`（整篇）、`create_layer` / `merge_down` /
`flatten_image`（新内容拿新号）；整篇换号的是 `crop_apply`、`document_set_size`（仅
`changed`/`resized` 时）。三个**领域自管事务**的入口——`run_tool_op`、`filter_apply`、
`camera_raw_apply`——必须自己在 commit 前换号：它们不能在外面再包 `ImplicitTxn`，否则双记历史。

`load_project` 特意整篇发号而不是留 0：留 0 会让所有层撞同一个缓存键、跨层串纹理，是 R2
上线后最难查的一类 bug。

**4. 换号必须在 `txn_commit()` 之前（本轮最容易写错的一点）。** 提交时历史栈压入的是
`state_` 的 after 快照（含 `content_revision`）。若先 commit 再换号：撤销正确（快照把内容与
旧号一起恢复），但**重做**会把「编辑后的内容」和「编辑前的号」凑在一起 → 违反「同值⟺同内容」
→ R2 的缓存静默供应旧纹理，表现为「重做没生效」。测试用「笔刷（领域自管事务）→ 撤销 → 重做」
直接钉住：重做后必须拿到落笔后的号。

**5. 刻意不换号。** 纯结构变更（`layer_add`/`remove`/`reorder`/`set_active`/
`set_clipping_source`）不换：单层像素没变，纹理可以留；合成计划是每帧从文档重建
（`build_gpu_plan`），没有需要失效的计划缓存。`undo`/`redo` 不换：快照恢复天然带回正确号。
失败的写入、恒等滤镜、空选区滤镜、完全落在图层外的**形状**（契约里的 noop）不换号。
注意**笔刷落空不在此列**——笔刷走「成功且记一步」的既有语义（只有形状/修复/克隆有 noop
判定），用例里用落空形状验 noop，不是笔刷。

**6. 护栏（`tests/test_layer_revision.cpp`，+6 例，562 行，默认构建即跑）。**

| 用例 | 钉住的不变量 |
|------|-------------|
| `layer_revision_is_unique_and_ignores_structure_edits` | 建层即发号、文档内唯一、与创建序同向；reorder/active/clipping/remove 不动既有号 |
| `layer_revision_bumps_on_every_content_write_entry` | 14 个写入口逐个「目标层换号、隔壁层不动」；只读查询与失败写入（blend 越界、adjustment kind 越界、NOT_FOUND）不换号 |
| `layer_revision_tracks_undo_and_redo` | 撤销/重做恢复「字节 + 号」；笔刷（领域自管事务）同理；整幅 `comp_render_flatten` 逐字节比对 |
| `layer_revision_is_per_layer_not_per_buffer` | 两层共享同一 `ImageBuffer`：改一层只换那层的号、另一层字节不动、COW 后地址分离 → 缓存键必须是 `(图层, 修订号)` 而非缓冲地址 |
| `layer_revision_covers_canvas_tool_filter_and_selection_edits` | 重采样/笔刷/真滤镜/选区搬移换号并可撤销回退；恒等滤镜与落空形状不换号且不加撤销步 |
| `layer_revision_feeds_gpu_plan_recompute` | 与 `build_gpu_plan` 对齐：整幅原生 → 裁剪变烘焙（号变）→ 改回整幅不复用旧号；穿透组↔隔离组的单元序列与组号同步变化 |

**7. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py`（默认） | **457 cases, 0 failed, 0 check failures**（451 → 457，+6） |
| `python scripts/build.py --gpu` | **457 cases, 0 failed, 0 check failures** |
| `python scripts/build_windows_shell.py` + `compositor_shell.exe --selftest=<报告>` | **164 项 0 失败**；报告 SHA256 `AA2E2754…` 与仓内基线 `compositor_shell_selftest.txt` **逐字节一致**（壳代码未改；默认构建 AUTO 走 CPU） |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**（OHOS aarch64 `compile ok=91 fail=0`；§6.27 表里记的 90 漏计了 S17 自己新增的 `gpu_plan.cpp`） |
| ABI 导出 | **112**（未新增；本轮只加引擎内部 C++ 接口） |

**8. 边界（如实记录）。**

- **不宣称加速**：本轮没有缓存，逐帧上传一点没变；S13 离屏 3.01 ms / S16 直呈
  3.04–3.27 ms 的基线未重测（改动不触及热路径）。
- 只覆盖**单层**纹理缓存的前提：烘焙单元（组/剪贴/蒙版/效果合成的结果）的缓存键需要
  「子树修订号集合 + 组结构签名」，是设计稿里的 v2，本轮不做。
- 事务中途渲染的缓存安全（设计稿 §4.2 第 4 条）属于 R2——现在没有缓存，也就无从违反。
- 仍整篇回落：调整层、HDR/RGBA16F 表面；Vulkan 文档路径仍未接线。
- 交互壳主画布仍走 CPU 展平 + GDI；**Win32 人工目视仍未做**（`docs/manual-test.md`
  的 M1–M16 保持未勾选，ALEX 已推迟这项）。

报告：`../report/S18-layer-revision-20261019-dsh.md`。

### 6.29 S19 D3D12 持久逐层纹理（R2）落地（2026-10-19，DSH）

S18 把「这层内容变没变」的问题解决了（逐层 `content_revision`），但一张纹理都没缓存。
S19 把设计稿的 **R2** 做完：原生单元的纹理**跨帧常驻**，内容不变就不上传；同时给出
`texture_uploads()` 计数、§4.2 的 4 条真机用例（外加跨文档隔离一例）。**本轮终于有实测加速，且只报实测。**

**1. 缓存键与失效。**

```cpp
struct LayerTexture {           // D3d12RenderBackend::GpuContext 内部
  ID3D12Resource* texture = nullptr;
  uint64_t revision = 0;        // 0 = 尚无有效内容
  UINT width = 0, height = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  uint64_t last_used_frame = 0;
};
std::unordered_map<comp_layer_id, LayerTexture> layer_textures;
// 键 = (document.instance_serial(), id, revision, w/h)
```

- **只缓存原生单元**（`GpuUnit::is_native()`）：烘焙单元的键要「子树修订号集合 + 组结构
  签名」，是设计稿里的 v2，本轮仍逐帧上传。
- 命中条件全等（`revision != 0 && revision == layer.content_revision && w/h 相等`）；
  `revision == 0` 永远视为不命中——这是 R1 特意给「加载工程留 0」留的护栏。
- **键里必须带文档实例号**：`comp_layer_id` 与修订号都只是文档内唯一，同一个后端先后
  渲染两个文档时 `(id, revision)` 会撞车。为此给 `Document` 加了进程内原子递增、
  永不序列化的 `instance_serial()`；后端发现实例号变了就整体丢弃缓存（只损失一次重传），
  用实例号而不是对象地址是因为地址会被分配器复用。
- 512 MiB 预算 + `last_used_frame` LRU 淘汰；本帧用过的条目不淘汰（避免同帧反复重建）。
- 缓存是**派生数据**：不进事务快照/历史/`.comp`；`drop_targets()`（画布尺寸变化）与
  上下文析构（设备丢失/关闭）整体丢弃。丢了只是多传一次纹理，不影响画面。

**2. 活跃事务整段旁路。** `doc.txn_active()` 时既不读缓存也不写缓存，回到 S17 的共享
`src_tex` 逐帧上传。事务是中间态，没有任何修订号语义；读会供应事务前内容，写会把中间态
钉进缓存键。测试第 4 例钉住这条：事务内每帧重传全部原生单元，提交后只有被改的那层重传。

**3. 描述符必须按单元分开（本轮最容易写错的一点）。** 原来是一张 2 项 UAV 表
（u0=dst、u1=src），每单元只是换常量。缓存上线后源纹理因层而异，如果仍共用一张表，
**命令是录制后统一执行**的 → 所有 `Dispatch` 执行时读到的都是最后一次写入的源纹理
（与 S13「上传缓冲共用一块」同源的错误）。改成：描述符堆按「单元对」扩容到
`2 × max(16, 单元数)`，第 i 个单元写 `[dst UAV, src UAV]` 并把表头偏移 `2*i` 传给
`SetComputeRootDescriptorTable(1, ...)`。每单元 2 次 `CreateUnorderedAccessView` 是微秒级。

**4. 相邻 dispatch 之间补 UAV 屏障。** S17 每个单元都会把 `src_tex` 转回 `COPY_DEST`，
那次过渡屏障在本机驱动上顺带保证了「上一个 dispatch 的写 → 下一个 dispatch 的读」；
缓存命中后源纹理不再逐帧过渡，顺序必须显式声明，否则混合结果可能是未定义顺序。
目标纹理每次 `Dispatch` 后发 `D3D12_RESOURCE_BARRIER_TYPE_UAV`。

**5. 计数口径（`texture_uploads()`，仅供测试/诊断）。** 创建 +1、重传 +1、走共享
`src_tex` 的逐帧上传也 +1。不进 C ABI、不进 `BackendCaps`——它不是能力，是缓存是否
真的生效的证据。

**6. 护栏（`tests/test_rhi.cpp`，+4 例；无 D3D12/无设备时整例跳过）。**

| 用例 | 钉住的行为 |
|------|-----------|
| `rhi_gpu_layer_textures_are_reused_across_identical_frames` | 4 原生单元首帧恰好 4 次上传；同文档第二帧 **+0**、两帧逐字节相同、与 CPU 基准 ≤1 LSB |
| `rhi_gpu_layer_texture_cache_uploads_only_changed_layers` | 改一层 + `touch_layer` 后恰好 **+1**，画面确实变了，与 CPU 一致 |
| `rhi_gpu_layer_texture_cache_rebuilds_after_canvas_resize` | 32²→64² 后缓存整体重建：增量 = 当前原生单元数 4，与 CPU 一致 |
| `rhi_gpu_layer_texture_cache_is_bypassed_inside_a_transaction` | 事务内连渲两帧每帧 **= 4**（全量），提交后只重传被改的 **1** 层，画面与 CPU 一致 |

**7. 真机基准复测（同机同场景：1024²、4 层；`--present-bench=60`，连跑三次）。**

| 项 | S16 基线 | S19 实测 |
|----|----------|----------|
| GPU 窗口直呈（render+present）均值 | 3.04–3.27 ms | **1.389 / 1.406 / 1.413 ms** |
| 同帧最优 | — | 1.040–1.081 ms |
| CPU flatten+RGBA→BGRA+StretchDIBits 均值 | 3.85–3.91 ms | 2.205–2.266 ms |
| GPU/CPU 帧耗时比 | 1.18–1.29× | **1.58–1.60×** |

结论：瓶颈确实是逐层上传——稳态帧不再上传后，窗口直呈从 ~3.1 ms 降到 ~1.41 ms
（同机同场景约 2.2×），相对 CPU 直绘的优势从 ~1.2× 扩到 ~1.6×。**剩余成本**是每帧
「4 MB 全透明清屏拷贝 + 4 次 dispatch + 8 MB 后备缓冲拷贝 + Present」，不是缓存能消掉的，
所以不宣称数量级提升；离屏（CPU_BUFFER）路径仍整幅回读，耗时不能由本表推断。

**8. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py`（默认） | **462 cases, 0 failed, 0 check failures**（457 → 462，+5；GPU 用例跳过） |
| `python scripts/build.py --gpu` | **462 cases, 0 failed, 0 check failures**（5 个 R2 用例在 RX 6800 上真跑） |
| `compositor_shell.exe --selftest` | **164 项 0 失败**；SHA256 `AA2E2754…` 与仓内基线逐字节一致 |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**（OHOS aarch64 compile ok=91） |
| ABI 导出 | **112**（未变）；`.comp` 格式未变 |

**9. 边界（如实记录）。**

- 烘焙单元仍逐帧上传（组/剪贴栈/蒙版/裁剪/效果/变换）；它们的缓存键是 v2。
- 缓存不跨文档、不跨后端共享：换文档（`Document::instance_serial()` 变）即整体丢弃，
  换画布尺寸（`drop_targets()`）同理。同尺寸下的**编辑**只重传被标脏的层。
- 离屏路径仍是「GPU 合成 → 整幅回读」，没做零拷贝。
- 交互壳主画布仍走 CPU 展平 + GDI；**Win32 人工目视仍未做**（`docs/manual-test.md`
  的 M1–M16 保持未勾选，留给 ALEX 真机测试）。

报告：`../report/S19-layer-texture-cache-20261019-dsh.md`。

### 6.30 S20 烘焙单元缓存（v2）落地（2026-10-19，Claude）

S19 只缓存原生单元；裁剪/蒙版/效果/旋转翻转/16 位源层/隔离组/剪贴栈这些**烘焙单元**仍每帧
在 CPU 上重烘焙、再整幅上传。S20 把设计稿的 **v2** 做完：一个 64 位结构键同时服务
**CPU 烘焙表面缓存**与 **D3D12 烘焙纹理缓存**。**本轮不宣称新的毫秒级加速**——收益由
上传/烘焙计数证明，见第 6 条。

**1. 缓存键（FNV-1a 64，仅区分输入，不是安全边界）。**

```
key = FNV1a(文档实例号, 画布 w/h, 用途标签(源层=|子树=), 累积不透明度,
            源层: layer.id, content_revision
            子树: 有序 DFS 逐节点 (深度, 累积不透明度, 节点类型, isolated,
                                     layer.id, content_revision, 子节点数))
```

- **深度 + 有序 DFS 是必需的**：`reorder` / 换父级 / 改剪贴基底**不发修订号**（S18 的
  设计），但它们会改变 DFS 的成员顺序或深度，键因此必然改变。测试用「交换两个兄弟 →
  必须重烘焙」钉住。
- **累积不透明度**：穿透组的不透明度被折进每个子层的烘焙像素（与 `Planner::append_units`
  同一算式），改组不透明度必须重烘焙。
- **文档实例号**：图层 id 与修订号只是文档内唯一，两个文档可以有完全相同的
  `(id, revision)`；不带实例号会静默贴上另一文档的表面。
- 源层与子树用不同用途标签分流；`cache_key == 0` 约定为「无键」，实际键永不为 0。

**2. CPU 烘焙表面缓存 `GpuBakeCache`。** 存预乘画布单元的 `ImageBuffer` **浅拷贝**
（shared_ptr COW，从不 `writable_pixels()`）。512 MiB 预算 + `last_used_frame` LRU，
**只淘汰本帧未使用的条目**；单面超过预算则永不入缓存（每帧重烘焙但结果正确）。
缓存自身不加锁，由后端独立的 `bake_cache_mutex_` 串行化——计划构建在设备互斥量之前，
两把锁不嵌套。

**3. D3D12 烘焙纹理缓存。** `unordered_map<uint64_t, BakedTexture>`，键与 CPU 缓存相同；
命中省上传，新建/重传走 `COMMON → COPY_DEST → UNORDERED_ACCESS`，命中后直接以 UAV 状态复用。
两层缓存命中与否互不影响正确性。

**4. 自查修掉一处悬垂引用。** 淘汰函数会 `erase` 条目：若先把新键的占位项插进表再淘汰，
占位项（`last_used_frame == 0`）可能被选中并 `erase`，随后返回的引用指向已释放节点。
改为**先淘汰 → 创建 → `insert_or_assign`**，失败分支用键重新查迭代器。

**5. 事务 / 换文档 / 尺寸。** 活跃事务期间 CPU 与 GPU 两层缓存**整段旁路**（不读不写，
回到共享 `src_tex` 逐帧上传）；换文档清空 CPU 缓存（键已含实例号，第二道保险）；
画布尺寸变化 `drop_targets()` 整体丢弃两张纹理表。

**6. 护栏（+8 例：5 默认 + 3 真机）。**

| 用例 | 钉住的行为 |
|------|-----------|
| `gpu_bake_cache_reuses_surfaces_across_identical_plans` | 同文档两次计划 2 miss → 2 hit，取回像素逐字节相同；原生单元键为 0 |
| `gpu_bake_cache_invalidates_only_the_changed_layer` | 只改一层：恰好 1 个键变化，未变层缓存取回像素不变 |
| `gpu_bake_cache_invalidates_when_group_structure_changes` | 交换兄弟（不发修订号）→ 键必变、必须重烘焙 |
| `gpu_bake_cache_is_not_shared_between_documents` | id/修订号完全相同的两个文档 → 键全不同、全 miss |
| `gpu_bake_cache_respects_budget` | 1 字节预算 → 永不入缓存、每轮 miss、计划仍成功 |
| `rhi_gpu_baked_textures_are_reused_across_identical_frames` | 第二帧 **+0 上传**、**+2 烘焙命中**、两帧逐字节相同、与 CPU ≤2 LSB |
| `rhi_gpu_baked_texture_cache_uploads_only_changed_units` | 改一个烘焙层：**+1 上传**、命中/未命中各 **+1**、与 CPU ≤2 LSB |
| `rhi_gpu_baked_texture_cache_is_bypassed_inside_a_transaction` | 事务内每帧 **+3 上传**、烘焙计数不变；提交后只重传 **1** 个单元 |

**7. 测试与基线。**

| 项 | 结果 |
|----|------|
| `python scripts/build.py`（默认） | **470 cases, 0 failed, 0 check failures**（462 → 470，+8；GPU 用例跳过） |
| `python scripts/build.py --gpu` | **470 cases, 0 failed, 0 check failures**（3 个 v2 真机用例在 RX 6800 上真跑） |
| `compositor_shell.exe --selftest=<报告>` | **164 项 0 失败**（`--gpu` 构建 `path = gpu`） |
| `python scripts/run_regression.py --require-ohos` | **6 passed, 0 skipped, 0 failed**；壳自检 SHA256 与基线逐字节一致；OHOS `compile ok=91 fail=0` |
| ABI 导出 | **112**（未变）；`.comp` 格式未变 |

验证中曾出现一次无法复现的 `0xC0000005`（`rhi_gpu_renders_group_and_clipping_units_like_cpu`，
该用例本轮未改）；随后连续 13 次重跑均 470/0。

**8. 真机基准（同机同场景，`--present-bench=60`，1024²、4 层，连跑三次）。**

| 项 | S19 | S20 |
|----|-----|-----|
| GPU 窗口直呈均值 | 1.389 / 1.406 / 1.413 ms | **1.365 / 1.390 / 1.421 ms** |
| 同帧最优 | 1.040–1.081 ms | 1.055 / 1.078 / 1.083 ms |
| CPU flatten+RGBA→BGRA+StretchDIBits | 2.205–2.266 ms | 2.145 / 2.233 / 2.216 ms |
| GPU / CPU 帧耗时比 | 1.58–1.60× | 1.56–1.61× |

**这张表不能用来读 v2 的收益**：基准场景（背景 + 3 个整幅画布层）全部是原生单元，
烘焙缓存根本不参与；它只证明没有回退。v2 的收益是计数意义上的：文档不变时烘焙单元
从「每帧 2 次 CPU 烘焙 + 2 次上传」变为第二帧 **0 + 0**；只改一层时从全量重做变为
**1 次烘焙 + 1 次上传**。

**9. 边界（如实记录）。**

- 调整层仍整篇回落 CPU（它重着色下方已合成画布，不是独立每层贡献）；HDR/RGBA16F 同理。
- 两层缓存各 512 MiB 上限，实际显存是两者之和；不跨文档/跨后端共享。
- 离屏路径仍是「GPU 合成 → 整幅回读」；交互壳主画布仍走 CPU 展平 + GDI。
- **Win32 人工目视仍未做**：`docs/manual-test.md` 的 M1–M16 保持未勾选，留给 ALEX 真机测试。

报告：`../report/S20-baked-unit-cache-20261019-claude.md`。

## 7. 许可

上游 `robbietilton/Compositor` 为 MIT 许可；本仓沿用 MIT，并保留上游版权声明。
`engine/imaging/` 下的像素内核移植自上游 C 源码，文件头保留原始注释。

### 6.31 S21 图层属性接线（混合模式 + 变换）+ `comp_layer_get_transform`（2026-10-20，Pi）

上两轮测试的结论是「引擎早就做完、壳一个都没接」：36 个 `comp_layer_*` 导出里有 16 个
调用数为 0，其中最贵的是 24 种混合模式与图层变换——上游 Layers 面板的第一组控件就是它们，
而本壳只有一条不透明度滑条。本轮把其中**用户一打开就该看见**的两组接上。

**1. 混合模式（24 档全量接线）。**

- 壳侧新增 `comp_ui::blend_mode_name` / `blend_group_of` / `step_blend_mode`：档位数值即
  `comp_blend_mode` 枚举（0..23，ABI 冻结表），顺序逐字对齐上游
  `LayerAppearance.swift` 的 `LayerBlendMode.allCases` 声明序——上游 Shift++ / Shift+−
  就按这个序循环、一步一撤销；分组照抄 `LayerBlendMode.groups`（Normal / 加深 / 减淡 /
  对比 / 比较 / 分量六组），菜单在同一组内不画分隔线。
- 三条入口：**Shift+B** 后退一档、**Shift+N** 前进一档（本壳 OEM_PLUS/MINUS 已被调整
  面板与缩放占用，故取语义键且不与 Brush 小写 `b` 冲突）；图层菜单「混合模式」子菜单
  24 项直达（命令 `kCmdBlendBase + 档位`）；图层面板行直接显示模式名而非 `混合#N`。
- 每步一次 ABI 写入 + `refresh_layers` + 标脏，状态栏回读档位、组名与序号。

**2. 图层变换（平移 / 旋转 / 缩放）。**

- 接线的前提是**补一个 ABI**：`comp_layer_get_transform`（导出 112 → **113**，纯尾部追加，
  冻结面不变）。此前只有 `set_transform` 没有回读口，壳若凭缓存做相对步进，撤销/合并/
  `.comp` 读回都会让它与文档漂移。契约：回读值可直接再写入（往返保真）；两个出参都必填；
  `flip_x` / `flip_y` 不在本入口，且 `set_transform` 不会覆盖既有翻转。
- 按键映射（步长全部由修饰现场决定，命令常量只表达动作）：
  方向键不带修饰仍是选图层（既有行为不变；**Move 工具例外**：见 §6.57 的无修饰微调）、
  **Shift+方向 = 平移 1 px 微调**、
  **Ctrl+方向 = 平移 10 px**、**Ctrl+Shift+左右 = 旋转 ∓15°**、
  **Alt+, / Alt+. = 等比例缩放 ±1%（中心锚定）**。
- 旋转在既有角度上累加并归一化到 (-360, 360]；缩放以当前中心为锚（`ui_core::
  layer_transform_scale_rect`），与拖角缩放同行为。`w/h<=0`（未显式给变换）的图层
  在缩放前被如实拒绝——没有可缩放的基准，不是静默不做事。
- 与 **Move 工具的选区搬移刻意分开**：后者走 `comp_selection_move_pixels`，只动当前
  图层的选区像素；本组改的是图层几何，整层位移。

**3. 验证。**

| 项 | 结果 |
|---|---|
| 引擎单元测试 | 470 → **476 例 / 0 失败**（+5：混合模式表与上游 allCases 逐条对齐、步进回绕、变换 clamp/中心锚定、`get_transform` 往返镜像） |
| Win32 UI 核心回归 | +3 例（变换步进/旋转有限性/缩放夹取），全绿 |
| 壳自检 | 164 → **177 项 / 0 失败**（混合模式 round-trip + 非法值不污染已存值；变换在显式事务内 get→步进→set→回读后 **abort**，不往撤销栈留步） |
| 自动化目视 | 45 → **57 项 / 0 失败**（新增 `blend` 5 项、`xform` 7 项） |
| 全量回归 | `run_regression.py --require-ohos` **6/6 PASS** |
| ABI | 112 → **113**（`comp_layer_get_transform`，尾部追加） |

`blend` 场景拿到的是**公式级证据**而非「看起来变了」：底层 `#FF8800`、上层 `#00FFFF`
重叠区在 Normal 下实测 `(0,255,255)`，切到 Multiply 后实测 `(0,136,0)`——正是逐通道
相乘 `(255·0/255, 136·255/255, 0·255/255)` 的手算值，通道差 374，且连按 22 次 Shift+N
（24 档一整圈）回到 Normal。

`xform` 场景的教训值得记：调试期四个断言 FAIL，逐一拆开后**全部是判据自己的错**，不是
产品缺陷——最隐蔽的一处是「矩形中心点」恰好在平移 20px 后落在新矩形的左边缘上，于是
「真的移动了」被判成「没动」。已改用「距左缘 6px 的探针点 + 平移后的对应带」作判据，
并用 stderr 探针取到铁证后才改判据（移动 readback 10→20、旋转 15→60、撤销回退到 45°
并保留位移）。这与 S15 修掉的「`--selftest` 不走消息泵所以一直绿」是同一类教训：验证
工具自己会骗人，改判据之前先证明产品真的错了。

**4. 边界（如实记录）。**

- `comp_layer_set_effect` / `set_mask` / `set_clipping_source` / `set_isolated` /
  `set_sampling` / `reorder` / `set_active` 等仍未接线——其中效果是**纯元数据**
  （ABI 注释明说「内核目前不参与效果像素求值」，`merge_down` 遇到带效果的图层直接返回
  `COMP_ERR_UNSUPPORTED`），接上去只是「能写能读能落盘、不改变画面」，与混合模式/变换
  不是同一性质，留到下一轮单独评估。
- 变换是按键步进式，**没有拖拽控制框**（上游的 Free Transform 形态）；缩放只支持等比例，
  不支持自由宽高比。
- 翻转（`flip_x`/`flip_y`）仍无法从壳修改：ABI 只在 `comp_layer_desc` 里给出，
  `set_transform` 不覆盖它，也没有独立 setter。
- 图层菜单的混合模式项**没有勾选态**：壳当前没有菜单勾选机制（无 `CheckMenuRadioItem`
  调用），当前档位由图层面板行与状态栏回读。

报告：`../report/S21-layer-property-wiring-20261020-pi.md`。

### 6.32 S22 图层属性补全（蒙版 / 剪贴 / 隔离 / 采样 / 重排 / 直方图）（2026-10-20，Pi）

S21 接完混合模式与变换后，ALEX 的评价是“和 PS 功能比起来根本不是一个东西”。本轮按 T2 给出的可量化缺口清单开干：**110 个 ABI 导出里 43 个在壳侧调用数为 0**。

**1. 先定性再动手。** 43 项不能一锅端，按“引擎到底参不参与像素求值”分三类：

- **A 类（引擎已参与求值）**：剪贴蒙版、蒙版、组隔离、采样、重排、直方图——本轮全接。判定不看注释而看代码：`composite_ops.cpp:137` 的 `clipping_factor()`、`filter_graph.cpp:481` 的 `coverage.add(&layer->mask)`、`filter_graph.cpp:119` 的 `node.isolated` 均为真实的像素路径；
- **B 类（纯元数据）**：`comp_layer_set_effect` 等能写能读能落盘但不改变画面（内核不参与求值，`merge_down` 遇效果层直接 `COMP_ERR_UNSUPPORTED`）——单独决策，不是本轮白送的；
- **C 类**：`comp_document_is_dirty` / `comp_document_engine` 等宴主内部信息＠14—壳用不到，不接。

**2. 补两个 ABI 回读口。** 接线的第一道门槛不是写入口（那些早就在），是**回读**：

| 导出 | 编号 | 解决的问题 |
|---|---|---|
| `comp_layer_get_mask` | 113 → **114** | 面板无法知道“这一层带不带蒙版” |
| `comp_layer_get_group_info` | → **115** | 面板无法知道“是不是组 / 挂在哪 / 剪贴谁” |

均为纯尾部追加，冻结面不变。理由与 S21 的 `get_transform` 同源：**没有回读口，面板就只能靠猜**——`comp_layer_desc` 只在创建时给出 `is_group`/`parent`/`clipping_source`，`.comp` 读回一份带组、带剪贴的文档后，面板必然静黙失真。`get_mask` 只回答“有没有、多大”，**不回传像素**（把 8 位盖盖图搬过 ABI 边界是纯浪费，要变成可见选区应走 `comp_selection_load_layer`）。

**3. 产品面入口。** 图层菜单新增排序（上移/下移/置顶/置底）、蒙版三连（建/读/清）、剪贴蒙版、组隔离、采样模式子菜单；视图菜单新增亮度直方图（F6）。键位：**Ctrl+Shift+M** 建蒙版、**Ctrl+Alt+G** 剪贴、**Ctrl+Shift+↑/↓** 排序。图层面板行内新增状态标记：`M` 带蒙版、`>|` 已剪贴、`[]` 组且隔离、`{}` 组且穿透。

直方图的口径值得记：**不自己扫像素**，全部取引擎只读入口（预乘先反预乘再取亮度、按 alpha 加权）。自己扫一遍必然与内核的 levels 口径漂移，面板曲线会系统性偏向暗部——那是“面板与实际调整对不上”的经典成因。

**4. 验证。** 单测 476 → **478 例 / 0 失败**；UI 核心 38 → **41 例**；壳自检 182 → **203 项 / 0 失败**（基线已更新）；`run_regression.py --require-ohos` **6/6 PASS**；ABI 113 → **115**。

**5. 两次“判据的错”。**

- 直方图我先写“均匀灰(200)尖值落桶 200”，实测 **76**。先算再改：本段图层像素是**预乘**红 `(128,0,0,128)`，反预乘得 `(255,0,0)`，亮度 `0.299×255 = 76.245` → 桶 76（口径根源：`tool_ops.cpp:549`）。产品对了、判据错了——这与 S21 `xform` 四个 FAIL 全错在判据上是同一类教训：验证工具自己会骗人。
- 第一次跑全量回归时第 2 步 FAIL（selftest SHA 与基线不一致）。根因是 `--clean` 会连 `build/` 一起删，而自检的内容对拍依赖 `build/import_probe.png` ——**S21 能过只是因为当时那份夹具偶然还在，随机构建目录残留决定基线成败本身就是缺陷**。修法：让自检**自己写夹具**（壳静态链着引擎，写 PNG 与读 PNG 是同一条 `comp_io_*` 路径），不把二进制夹具提交进版本库；自备产物与原先手工生成的**逐字节相同**。走过的弯路也记一笔：最初想在 `run_regression.py` 第 2 步补生成，但那要用 `build/compositor_engine.dll`，而**第 5 步才建它**——依赖放错步骤，比不补更糟。

**6. 边界（如实记录）。** 效果仍未接（纯元数据，需先做滤镜图里程碑）；变换仍无拖拽控制框且缩放只等比例；菜单无勾选态（壳尚无 `CheckMenuRadioItem` 机制，当前档位由面板行与状态栏回读）；直方图用对数制度显示（否则高光滚降只有 1px 高、看不见）且无缩放/平移交互；图层面板不支持拖行重排。

报告：`../report/S22-layer-property-completion-20261020-pi.md`。

### 6.33 S23 S22 复审四项修复 + 引擎对象变体隔离（2026-10-20，DSH）

S22 交付后 Claude 复审 `07397ad`，判出四项：**P1-a / P1-b 必修**、P2 顺手、P3 建议一起。根因一句话：`panel_*` 那族函数在 S22 里有**零覆盖**，而它们正是「面板行为」与「引擎语义」之间的手工换算层。Pi 修到一半（P1-a 已改、未提交）进程挂掉，本轮接手做完。

**1. P1-a 重排方向整对写反。** 面板上「上移 / 下移」两个分支的数组下标换算互换，且选中态按相反假设跟随——两个错误互相抵消，不报错、自检也能过，只有真去拖图层的人才发现层往反方向跑。修法不是改符号，而是把换算抽成 `comp_ui::plan_layer_reorder` 纯函数，测试里**独立按引擎语义重演一遍** `reorder_layer`（先 erase、再插到锚点之前、数组尾 = 顶层），再拿「用户在面板上看到的行」比对。表驱动 2~3 层 × 4 方向（置顶/上移/下移/置底）× 每个选中行。

**2. P1-b 剪贴基底取成了上方一层。** 原实现取 `self_index + 1`（数组上方），守卫却拒绝顶层、提示写「最底层无法剪贴」。后果不是「没效果」而是**两条渲染路径给出不同画面**：`filter_graph` 要求成员紧跟在基底之后才成栈，取反后栈根本不成立（FilterGraph 画面不变），而 `compositor.cpp` 的单层视图只看 `clipping_source != 0` 就按基底求值（画面变了）。修法同样抽纯函数 `comp_ui::plan_clipping_base`，规则四条：

| 情况 | 结果 |
|---|---|
| 下方最近的是同父普通层 | 剪到它 |
| 下方邻居已是剪贴成员 | 并入**它所在栈的基底**，不做链式（链式成员落在栈外，正是上述两路径分叉） |
| 下方没有同父层 | 如实拒绝，提示「下方没有同组图层可剪贴」 |
| 下方邻居的基底越级 / 反向 / 不存在 | 拒绝，不退化成链 |

「下方」必须是**同父**的下方：`filter_graph` 的 siblings 只含同父层，全数组相邻 ≠ 同父相邻——有图层组时直接取 `index-1` 会跨组。为此 `LayerRow` 新增 `parent`（由既有的 `comp_layer_get_group_info` 回读填充）。另外纯函数接口里 id 一律走 **64 位**：`comp_layer_id` 是 `uint64_t`，窄化成 `int32_t` 会让高位 id 撞值、把「基底是哪一层」判错，测试用 2^40 量级的 id 钉住位宽。

**3. P2 直方图标签与标题叠字。** 块顶取 `panel.y`、块内标签落在 `panel.y+10`，而标题在 `panel.y+8`；行起点却在 `+94`，中间空 30px。根因是「块坐标 / 标签坐标 / 行偏移」三处各算一套。现统一为 `histogram_rect` / `histogram_label_rect` / `histogram_plot_rect` / `layer_rows_top` 四个纯函数：块贴 `panel.y + kLayerHeaderHeight`，块底与首行严丝合缝，绘制与命中测试同源。

**4. P3 自检里那个撤销循环是空的。** 历史基线在**全部写入之后**才取，于是 `current <= baseline` 第一次判断就成立，一条都没撤——注释说「全部撤销回去」，实际什么都不做。基线移到写入之前，并补两条断言：`s22_undone > 0`、循环结束深度 == 基线。这样一来「护栏是不是真的在护」变成可判定的，而不是靠注释自称。

**5. T2 遗留：`--gpu --shared` 静默降级。** T2 实测过壳以为在跑 D3D12、引擎里却根本没编进去（AUTO 落到 CPU 且无提示）。两个成因都在「路径只按一种口径分」：`build.py` 的 `obj_path()` 只用 `shared_` 前缀，GPU / 非 GPU 共用同一 `.o` 路径；`build_windows_shell.py` 用 `glob("build/engine_*.o")` 把历史构建残留全当本次输入。现改为**变体前缀**（`gpu_` / `shared_` / `gpu_shared_`）+ `engine_object_paths(driver, shared, gpu)` 算确定路径，壳侧 `--gpu` 缺对象即失败并给出该跑的命令，另加 `--check` 供回归烟测。正面证据：本变体下 `--present-bench` 报 `backend=D3D12`。

**6. 验证。** 单测 481 → **485 例 / 0 失败**（UI 核心 41 → 45）；壳自检 203 → **205 项 / 0 失败**（基线更新，diff 仅新增两行）；`run_regression.py --require-ohos` **6/6 PASS**（OHOS `ok=91 fail=0`）；`--gpu` 变体单测 485/0、自检 205/0、`backend=D3D12`；ABI 导出数不变（**115**，本轮无新导出）。

**7. 两次「判据比产品更可疑」。** 组图层用例里我断言「选中顶层 103 的基底应是根级 100」，实测是组图层 200——`200` 的 `parent` 就是 0（根级），它本身就在 103 下方、是合法基底，产品对、断言错。改成更锋利的一条：基底必须是 1，且**不得**是全局相邻的组内层（下标 3）。另一件：更新自检基线前先 `Compare-Object` 证明 diff 只有新增两行，再写回——基线是回归护栏，不是记分牌。

**8. 边界（如实记录）。** 直方图仍无缩放/平移交互；图层面板仍不支持拖行重排（只有菜单/快捷键）；延续 S22 的非目标：效果层、蒙版即滤镜图里程碑、变换拖拽控制框、菜单勾选态、镜像（需新 ABI）。

报告：`../report/S23-review-fixes-20261020-dsh.md`。

### 6.34 S24 S23 剩余五项非目标收口：效果层 / 拖行重排 / 直方图交互 / 菜单勾选态 / 镜像（2026-10-20，DSH）

S23 报告 §9 列了五项非目标，ALEX 要求「一并做完再交测」。五项全部落地，新增 4 个导出 ABI（115 → **119**）。

**1. 镜像（需新 ABI）——引擎早就在算，只是读不回。** `resample.cpp` 的 `if (layer.flip_x) us = -us;` 一直按局部中心镜像（`x → w-1-x`），`gpu_plan.cpp` 也因翻转禁用 GPU 快路径；但 ABI 只有写口没有回读，壳只能「盲取反」、菜单也显示不了当前方向。新增 `comp_layer_set_flip` / `comp_layer_get_flip`：写入归一化为布尔（`-1/2 → 1/1`），**同值写入不产生撤销步**（否则空步塞满撤销栈，用户按一次撤销画面不动）。用例用 8×8 左半红图：翻转后像素跑到右半，撤销后逐字节回到左侧。

**2. 效果层——顺带纠正 S22 报告的一处旧结论。** S22 写「效果纯元数据、内核不参与像素求值，属滤镜图里程碑」。实测不成立：`effect_eval.cpp` 对 6 种 kind（Stroke / Drop Shadow / Color Overlay / Inner Shadow / Outer Glow / Inner Glow）都按颜色 + 不透明度 + 尺寸/角度/距离/模糊求值，`composite_ops.cpp` 用 `effects_margin()` 扩边后 `apply_effects`，`test_layer_merge` 已证明合并会把效果烘焙进像素。真正缺的是**枚举回读**，不是求值能力：新增 `comp_layer_effect_count` / `comp_layer_effect_at`（两段式，JSON 逐字回读、未知键保留、越界 `INVALID_ARGUMENT`、只读不脏）。壳侧 Layer 菜单新增「效果」子菜单（6 档开关 + 清除全部），勾选态显示该层已启用档位，图层行尾标记 `fx`。

| 档位（即 `.comp` 的 `engineEffects[].effect`） | 默认参数 |
|---|---|
| 0 Stroke 描边 | size 4, opacity 1, inside=false |
| 1 Drop Shadow 投影 | angle 90, distance 20, blur 20, opacity .5 |
| 2 Color Overlay 颜色叠加 | 黑, opacity 1 |
| 3 Inner Shadow 内阴影 | angle 90, distance 10, blur 10, opacity .5 |
| 4 Outer Glow 外发光 | size 20, opacity .75 |
| 5 Inner Glow 内发光 | size 10, opacity .75 |

**3. 拖行重排。** 面板行拖到另一行即重排，规则抽成 `comp_ui::plan_layer_drag`：只允许**同父**拖拽（面板是扁平列表、不画缩进，跨组拖会改 parent 而用户看不出层去了哪个组——「层没了」比「明确拒绝」糟得多）；落点即原行返回 `kSameRow` 不产生写入；跨父返回 `kCrossParent` 并停原位。落点指示线 `layer_insert_line_rect` 与行矩形同源（同一个 `rows_top`），移动超过 4px 才进入拖拽态。用例用 erase+insert 独立复算 5×5 全部方向，比对「落点行 == 用户看到的那一行」与「选中态跟随」。

**4. 直方图交互。** `HistogramView{lo,hi}` + 一组纯函数：绘图区横向拖拽 = 放大到框选区间，滚轮 = 以光标亮度为中心 25% 缩放，双击 = 复位全幅；最小跨度 8 个 bin（0 跨度会让映射除以 0，NaN 坐标交给 GDI 是一整块空白且不报错）。调试中发现一个真 bug：缩到全幅后继续「缩小一档」会得到 `lo = 1.8e-15 / hi = 255`——画面与全幅一样，但 `histogram_view_is_full` 判假，标签会一直显示「直方图 0–255」这种自相矛盾的放大态；已在 `clamp_histogram_view` 收口为「跨度顶满即归位 0..255」。

**5. 菜单勾选态。** 菜单只在启动时建一次（`build_menu` 交给 `CreateWindowW`），勾选态只能在 `WM_INITMENU` 刷。判定规则抽成 `comp_ui::layer_menu_checks`：无选中层不勾任何项（采样/混合显式 `-1`）、非组图层的 `isolated` 置 1 也不算、越界档位先夹再勾、直方图开关是文档级状态与选中层无关。`menu_set_checked_recursive` / `menu_radio_check_recursive` 在子菜单树里按命令 id 定位，**不重建菜单**（重建会丢用户态并让快捷键表失效）。

**6. 验证。** 单测 485 → **492 例 / 0 失败**（`test_ui_core` 45 → 50、`test_abi` 27 → 29）；壳自检 205 → **219 项 / 0 失败**（`Compare-Object` 逐行确认 diff 仅新增 15 行：14 条 OK + 1 条 info；基线 SHA256 `E277A4C4…`）；`run_regression.py --require-ohos` **6/6 PASS**（OHOS `ok=91 fail=0`）；`--gpu` 变体 `--present-bench` 报 `backend=D3D12`（GPU 直呈 avg 1.55ms/帧 vs CPU flatten+blit 2.18ms/帧，20 帧）。ABI 导出 115 → **119**。

**7. 又一次「判据比产品更可疑」。** 自检里我断言颜色叠加后蓝通道 `> 200`，实测 `(0,0,128,128)`——图层像素是 50% 预乘红，叠加按覆盖率求值、落盘仍是预乘值，蓝色就是 128。产品对、断言错：改成「蓝上来、红下去」并把原因写进注释。另一处：`effect_at` 我最初断言「三个出参全给 nullptr 应返回 `INVALID_ARGUMENT`」，而既有 `comp_layer_adjustment_at` 的口径是三个出参都可选（全空 = 只探测该下标是否存在）——以既有口径为准，改断言并在头文件写清。

**8. 边界（如实记录）。** 效果无参数编辑 UI（只有 6 档开关 + 默认参数）、无效果重排；清除 N 个效果 = N 步撤销，不是一步；拖行仅同父（跨组明确拒绝，不支持拖进/拖出组）；镜像只有菜单 + `Ctrl+Shift+H/F`，无可视控制柄；变换仍无拖拽控制框、非均匀缩放仍缺（S21 遗留）。沿用既有口径：M1–M16 人工目视由 ALEX 本人执行，本轮不代为声称。

报告：`../report/S24-nongoals-20261020-dsh.md`。

### 6.35 S25 对标 PS「图像」菜单：画布旋转 / 修边 / 反相 / 色彩范围（引擎 + ABI 层，2026-10-20，hermes）

S24 交付后 ALEX 指令「对标 Photoshop 功能和 `compositor/` 上游，识别并补充缺失功能」。精读上游后定性：
**12 种调整层已对齐，真正的缺口在产品通路**——上游 `Image` 菜单的四条画布级操作在本仓连引擎
入口都没有（只有 crop）。本轮补齐**引擎 + ABI 层**，新增 4 个导出（119 → **123**），既有 ABI 头
与像素公式零改动：

| ABI（导出序号） | PS 对应 | 上游真源 | 关键口径 |
|---|---|---|---|
| `comp_document_rotate_canvas` (#120) | Image > Image Rotation | `CanvasRotation.swift`（quarterTurned） | 不重采样：像素逐字节不动，落位换成旋转后包围盒 + rotation 累加；选区蒙版精确转置 |
| `comp_document_trim` (#121) | Image > Trim | `ImageTrim.swift` | flatten 整篇合成算框 → 复用 `crop_apply` 提交；三基准 × 逐通道容差 × 逐行扫描 |
| `comp_layer_invert` (#122) | Image > Adjustments > Invert | `PixelInvert.swift` | 预乘 `c' = alpha - c`（透明仍是透明）；HDR 拒绝、16 位落工作副本、选区按文档坐标裁剪 |
| `comp_selection_apply_color_range` (#123) | Select > Color Range | `ColorRangeSelection.swift` | 整篇合成上**全局**匹配（不连续也算）；include/exclude 双表 + fuzziness 0..200 + invert |

**两个由新单测当场抓到的实现 bug（都在本轮新代码里）**：

1. **90° 蒙版转置行列写反**。`rotate_mask_90` 把源行/列直接当目标列/行用（`d[(h-1-y)*stride + x]`），
   目标窄于源时写进相邻行——非正方形选区（如 16×8 画布的 6×4 选区）旋转后蒙版全错。正解是
   `d[x_s * stride + (h-1-y_s)]`（CW）/ `d[(w-1-x_s) * stride + y_s]`（CCW），行列必须同时换。
2. **90° 旋转沿用原横矩形 transform → 渲染缺一半**。渲染器 `build_layer_unit` 的 rotation 是
   「绕 bounds 中心转、光栅窗口 = 轴对齐 bounds」；16×8 图层转 90° 后实际占 8×16 竖矩形，window
   还是 16×8 的话旋转出去的那一半落在窗口外被裁空（实测四角全黑）。正解：90° 时 transform
   换成旋转后轴对齐包围盒（w/h 互换）——**像素依然不动**，不重采样原则保持。这条坑写进了头文件
   注释与 `canvas_ops.cpp` 的落位口径说明，避免后人重蹈。

另从 `filters.cpp` 提取共享的 `blend_through_selection` 到 `selection.h`（filter_apply 与 invert_layer
共用同一条选区混合口径，防两处漂移）；`document_trim` 的 8 位视图统一走 `layer.h` 导出的
`deep_to_rgba8` / `hdr_to_rgba8`，不碰私有/匿名命名空间。

**验证。** 单测 492 → **523 例 / 0 失败**（新增 `test_canvas_ops.cpp` 31 例：旋转几何 / 奇数取整 /
四圈闭合 / origin 越界、选区转置 / 空选区、trim 三基准 × 容差 × 逐边开关 × 事务、反相预乘语义 /
16 位 / HDR / 选区裁剪 / 偏移图层、色彩范围全局匹配 / invert / exclude / fuzziness / combine /
错误闸）；壳自检 **219 项零变化**（SHA256 `e277a4c4…` 与 S24 基线**逐字节一致**——既有行为零漂移）；
`--gpu` 变体 523/0、`--present-bench` 报 `backend=D3D12`（avg 2.09 ms/帧）；视觉探针 68/0；
ctypes 探针实测四个新导出的错误码与空文档 noop 契约；`run_regression.py --require-ohos`
**6/6 PASS**（OHOS 91 → 93，新增两个 `.cpp` 参与鸿蒙交叉编译）。

**边界（下一步的活，本轮没做）。** 本轮交付止于引擎 + ABI + 单测：壳的 Image 菜单三条命令、
Select > Color Range 面板（吸管取样 / fuzziness 滑杆 / 反相勾选）、快捷键接线全部未做；
Trim 的颜色基准预览与 Color Range 的取样 UI 亦未做。`scripts/visual_check.py` 的 +84/−46
探针修复（S24 遗留）仍在工作区未提交，等 ALEX 定夺。

### 6.36 P0-0 选区混合按图层几何映射 + S25 复审 8 条清零（2026-10-21，Claude）

**缺陷。** `blend_through_selection`（`domain/selection.cpp`）把图层像素换算成文档坐标时只做
`bounds.x + x`，**完全忽略 `layer.rotation` / `flip_x` / `flip_y`**。S25 复测在 32×32 画布、
12×4 偏移图层、选区 (12,12)-(20,20) 上实测：0° 选区外改动 0 px，**45° 与 90° 各有 8 px 选区外
被改**——不是「只有非 90° 整倍才错」，90° 整倍同样错，因为旋转后的像素网格与文档网格不再同构。
影响面：`comp_layer_invert` 与 `comp_filter_apply` 两条路径；`comp_layer_set_transform` 是公开
ABI，rotation/flip 都是合法状态。

**修法（路线图上选的「正变换」而不是「拒绝」）。** 新增 `Layer::pixel_to_document()`——它是
`document_to_pixel()` 的严格逆（归一化 → 翻转 → 正旋转 → 反归一化，与前者「逆旋转 → 翻转」
逐项互逆）。`blend_through_selection` 的入参从 `const comp_rect& bounds` 改为 `const Layer&`，
逐像素取文档坐标后最近邻取 coverage：

- 轴对齐（`rotation == 0`、无翻转、`bounds.w/h == 像素宽高`）仍走**整数平移快路径**——
  与旧实现逐字相同，既有选区 / 反相 / 滤镜断言与 68 条视觉探针逐字节不变；
- 其余情况走正变换 + `lround` 取最近的文档像素（coverage 是硬边蒙版，插值只会把边缘抹成
  半选，与文档→图层方向同口径）；落点出画布或出源缓冲时 coverage 记 0，回滚原像素。

不选「`rotation % 360 != 0` 返回 `COMP_ERR_UNSUPPORTED`」的理由：那会让公开 ABI 上合法可设的
rotation/flip 直接把 invert 与全部滤镜打成不支持，功能退化且多出一个错误码；而正变换与合成 /
吸管的映射是**同一条**，语义上没有第二份口径。

**验证。** 新增 2 例（`test_canvas_ops.cpp`）：`invert_maps_the_selection_through_layer_rotation_and_flip`
用 90° / 180° / flip_x 三个整数变换把「图层第 (x,y) 像素落在文档哪一格」钉成精确象限断言；
`invert_maps_the_selection_through_an_arbitrary_rotation` 用 45° + 16×16 图层，按
`doc = center + R(45°)·offset` 独立推导期望，并断言「两边都不是空的」防空转。
全量 **525 例 / 0 失败**、`run_regression.py --require-ohos` **6/6 PASS**、PE 导出仍 **123**、
壳自检 **219 项 SHA256 零变化**、`visual_check.py` **68 项 / 0 失败**；**本批改动新增告警 0**
（干净构建全仓仍有 7 条 S25 之前就在的告警：三个后端头文件的未使用私有字段 + 两个测试辅助
函数 + `test_rhi.cpp` 的未使用 lambda 捕获，与本次改动无关，未动）。

**同批清零的 8 条复审非阻断项。** ① `canvas_ops.cpp` 的 `siempre` 笔误改回「总是」；② 删掉两处
`tolerance > 255`（`TrimOptions::tolerance` 是 `uint8_t`，恒假）；③ 删掉旋转里不可达的
`layer.clipped_empty = false`（空几何已被 bounds 闸跳过）并写清为什么走不到；④ 去掉
`test_canvas_ops.cpp` 里未使用的 `[k]` 捕获；⑤ 空选区旋转用例补
`active_of(doc) == 0`，钉住「丢弃」而不是「留着 active 全 0」；⑥ `comp_canvas.h` 的 trim 段补
「活动选区会被清掉」、invert 段补「16 位写回后永久变 RGBA8」；⑦ `comp_selection.h` 补
`point_count` 数的是**点**不是 float（矩形传 2）；⑧ `gpu_plan.cpp` 注明「画布转一下就整篇
掉出原生单元」是既有回退规则的叠加，不是旋转引入的新回退。

**顺手清掉三处死代码 / 告警（不改行为）。** `tool_ops.cpp` 的 `layer_histogram` 里
`stride` 取了却没用（`pixels.row()` 自带 stride）；`filter_graph.cpp` 里一段只喂给
「也没人用」的 `half_surface` 的 `canvas_format` 计算整块删除（`-Wunused-variable`）；
`comp_layer_abi.cpp` 的 coverage stride 闸把 `int32_t` 宽度显式 `static_cast<size_t>` 并先挡
非正宽度——顺带修掉 `-Wsign-compare`，负宽度也不会再被折成巨值绕过检查。

### 6.37 P0-1 图层面板补全 + `move_layer` 末尾落位缺陷（2026-10-21，Claude）

**缺口。** P0-0 结束时图层面板只有新增、显示与 ±10% 不透明度：删除/复制/改名/建组/多选/
拖进组全部没有 UI；ABI 侧还缺 `set_name` / `get_name` / `move` / `duplicate` 四个入口。
验收标准原话：**建组 → 拖两层进组 → 改名 → 复制 → 不透明度改 37% → 删层，每步恰好一次撤销**。

**ABI（纯尾部追加，123 → 127）。** `comp_layer_get_name` / `comp_layer_set_name` /
`comp_layer_move` / `comp_layer_duplicate`；既有枚举、结构体与签名一个不动。名字的权威留在
引擎：壳每次 `refresh_layers` 都重取名（两段式探长），撤销重命名、合并、读盘之后面板不会
显示旧名字。`set_name` 严格 UTF-8，空名/坏编码/超长/未知 id 显式失败（不静默截断）。

**壳侧命令。** `Ctrl+G` 建组（先 `comp_layer_add` 建组再逐层 `move`）、`Ctrl+J` 复制、
`Del` 删除、`F2`/双击行重命名（预填 + `WM_CHAR` 键入，UTF-16 → UTF-8 一次转换，
中文名可输入）、`Shift+O` 精确不透明度（0..100 百分比键入）。Ctrl 点行多选，追加选中
存 **id** 不存行号（重排/撤销后行号失效）。**批量命令整批放进一个显式事务**，N 层删除/复制
只占一步撤销；拖行重排 / 拖进组 / 拖出组各只发一次 `reorder`/`move`。拖拽只作用于主选中层，
多选批量拖拽未做。

**拖进/拖出组。** 面板仍是扁平列表，跨父级靠落点区分：落到组行中间带（30%..70%）= 拖进该组
末尾；落到根级行 = 移出到根级、放在目标行下方；其余跨父级（目标在别的组、拖进自己子树）
明确拒绝并说明原因，不静默改父级。

**修掉的引擎缺陷（真缺陷，不是适配）。** `Document::move_layer` 在 `before = 0` 时用
`pos == state_.layers.end()` 判断「该组还没有子层」；但 `pos = 最后一个同父兄弟 + 1`，当最后
一个兄弟恰好是数组末元素时也等于 `end()`，于是误走 fallback，插到 `group + 1`（= 组底）。
后果：**每一次** `before = 0` 的移动都落在组的最下面，与 `comp_layer.h` 明写的「移到目标父级
的末尾」相反；`Ctrl+G` 批量收层时整组堆叠序被静默反转（两层直接上下颠倒，画面上表现为选中的
上层跑到下层下面）。修法：用独立的 `positioned` 标志区分「找到位置」与「插入点在末尾」，
`end()` 从此是合法插入点。壳侧 `panel_add_group` 相应**从下往上搬**（`moving.rbegin()`），
让最后落下的保持原顶层。回归补在 `test_layer_mutation.cpp` 既有用例
`move_layer_reparents_and_changes_stacking` 内（不增用例数）：非空组上 `move(id, group, 0)`
后展平，断言新层盖住原组顶。

**验证。** 全量 **528 例 / 0 失败**（P0-0 的 525 + P0-1 新增 3 例）；PE 导出 **127**；
`run_regression.py --require-ohos` **6/6 PASS**；壳自检 **219 项 SHA256 零变化**
（`e277a4c4…`，未加自检项）；`visual_check.py` **68 项 / 0 失败**；**本批新增告警 0**。
图层面板 UI 路径由仓库外的临时验收脚本（`p0_1_ui_check.py`，走 `visual_check.py` 的
真 `PostMessage` 键鼠 + 像素抓取）跑完整验收路径：每一步做「操作前 ≡ 一次 Ctrl+Z 后」
与「操作后 ≡ 一次 Ctrl+Y 后」的**逐像素相等**，共 **30 项全过**；其中建组一条额外断言
「长名层仍在第 0 行」，专门守堆叠序不被反转。

**边界。** ① 组行显示在子层**下方**（`move_layer` 把子层插在组下标 +1，数组序自下而上；
面板未做树形缩进/折叠，与上游 PS 相反，属沿用既有落位口径）；② 多选不支持 Shift 连续区间、
不支持批量拖拽；③ 验收证据是机器抓的像素，手感/跨 DPI/多显示器仍待 ALEX 目视
（`docs/manual-test.md`）；④ `scripts/visual_check.py` 的工作区改动按约定仍未提交。

### 6.38 P0-2 效果参数面板：参数元数据收进 ABI（2026-10-21，Claude）

**缺口。** 效果此前只能从菜单添加/清除，参数一个也改不了；更糟的是壳里抄了一份
`effect_kind_defaults()`（每个 kind 的缺省值摘要），与 `effect_eval.cpp` 的 `read_*` 实参
**两处各写一份数值**。这种重复不会编译报错，只会表现为「面板写着 90°，落笔按 0° 算」。
验收标准：**六种效果都能在面板里逐个改参数，改完画面按引擎语义变化，且壳内不再有任何一份
参数数值**。

**ABI（纯尾部追加，127 → 131）。** `comp_layer_effect_param_count` /
`comp_layer_effect_param_at` / `comp_layer_effect_read_value` /
`comp_layer_effect_build_settings`；既有枚举、结构体与签名一个不动。参数元数据
（`json_path` / `label` / `type` / `min` / `max` / `default`）现在只有一份实现：
`engine/domain/effect_table.{h,cpp}`，由 ABI 原样导出。

**引擎表。** 六种 kind 的参数个数 **6 / 7 / 4 / 7 / 5 / 5**（Stroke、DropShadow、
ColorOverlay、InnerShadow、OuterGlow、InnerGlow），键名就是 `effect_eval.cpp` 里
`read_*` 用的扁平 JSON 键。`effect_read_value` 直接复用 `adjustment_read_value`——
两者要的语义完全相同（点分路径取标量，缺失/中间段非对象/叶子非标量一律 `NOT_FOUND`），
共用一份实现避免「同一条路径规则写两遍」。`effect_build_settings` 只做**合并**：空串 = 该项
不动，未知键原样保留（效果 settings 是前向兼容的不透明字符串，整棵重建会丢上游字段），
越界/非数/坏布尔/坏 base 显式拒绝，多给的值按 adjustment 同口径忽略。

**壳侧面板。** `Ctrl+Shift+E`（效果菜单里也有「编辑参数…」）打开右侧效果面板；右面板
同一时刻只属于滤镜/效果/文字/调整之一，打开效果会先关掉滤镜面板。面板行几何与步进
（`kEffectRowsTop = 54`、`kEffectRowHeight = 20`、跨度 ≤ 1 时步进 0.05，否则 1.0）落在
`ui_core.h` 的纯函数里，可单测，不依赖窗口。键盘：`Tab` 选参数、`-`/`=` 调值、
数字/`.`/`-` 进入键入态、`回车` 应用（未启用该效果时等同添加）、`,`/`.` 切换效果类型、
`Backspace` 重置为本表缺省、`Esc` 关闭；点行即聚焦该参数。应用只调一次
`comp_layer_set_effect`（同 kind 幂等替换），因此天然一步撤销；底串始终是本层当前 settings
的回读，未知键跟着走。

**漂移护栏（本轮的核心理由）。** `tests/test_effect_table.cpp` 对六种 kind 各做两次展平：
`set_effect(kind, "{}")`（引擎内部缺省）与「把参数表缺省值全部显式写入」——两份像素必须
**逐字节相同**。另有用例钉住参数个数、键名与缺省值抽样（`stroke.size = 4 [0,500]`、
`drop_shadow.angle = 90`、`inner_glow.size = 10` 等）、未知键保留、越界拒绝、回读往返。
壳侧几何/步进另加一例 `ui_core_effect_param_panel_geometry_and_steps`。

**验证。** 全量 **535 例 / 0 失败**（P0-1 的 528 + 效果表 6 例 + 面板几何 1 例）；
PE 导出 **131**；`run_regression.py --require-ohos` **6/6 PASS**；壳自检 **219 项 SHA256
零变化**（`e277a4c4…`，未加自检项）；`visual_check.py` **68 项 / 0 失败**；**本批新增告警 0**。
另跑了一次**真实 UI 路径对拍**（仓外一次性脚本，16 项 / 0 失败，截图 `report/assets/p0_2/`）：
面板打开→改 `Red=1` 回车→画布出现描边→一次 `Ctrl+Z` 逐像素回到应用前→`Ctrl+Y` 逐像素回到
应用后；`Size` 步进到 9 与键入 25 各自产生新的撤销步；以及「`Backspace` 回表缺省 + `red=1`」
与「引擎 `{}` + `red=1`」两帧**文档矩形内逐像素相同**——`test_effect_table.cpp` 的漂移对拍
在 UI 上复现了一遍。

**审核补丁（壳侧三处）。** ① 从 `.comp` 回读的参数值按表量程夹取后再显示，否则旧文件里
`size=1000` 会让面板显示 1000 而引擎按 500 求值；② `Backspace` 重置只把**参数值**还原，
保留本层 settings 作为合并底串，避免随后回车时把未知键整串抹掉；③ `type_begin` 顺手关掉
效果/滤镜面板，否则文字工具落插入点后数字键会被面板的键盘分支先吃掉（插入点在闪、打字没反应）。

**边界。** ① 没有滑杆拖动与实时预览：改值靠键盘步进/键入，`回车` 才落笔一次（与滤镜面板
同口径），拖动式预览要等滤镜图的增量缓存；② 颜色是 `red`/`green`/`blue` 三个 0..1 的数字，
没有取色器/HSL；③ 面板只编辑「当前选中图层」，多选批量加效果未做；④ 参数表是手写常量，
新增参数必须同时改 `effect_eval.cpp` 的 `read_*`——漂移由像素对拍用例守，不是编译期联动；
⑤ 面板手感/跨 DPI 仍待 ALEX 目视（`docs/manual-test.md` M17）；⑥ `scripts/visual_check.py`
的工作区改动按约定仍未提交。

### 6.39 P0-3 调整滑杆 + 曲线/色阶编辑器（2026-10-21，Claude）

**缺口。** 调整面板只能「`Tab` 换行 + `=` 步进 + 数字键入」，没有滑杆、没有拖动预览；
更要命的是三类调整根本改不了：**Levels 的 20 个量在参数表里是 0 项**（`param_count = 0`，
面板连一行都不显示），**Curves 整条曲线没有编辑入口**（只能读、只能保真存取）。
验收标准：数字参数能拖、拖的时候画面实时变、**一次拖动只占一步撤销**；
曲线能加点/拖点/删点且落盘后点列不变；Levels 黑场拖到 40 暗部被压死。

**ABI（纯尾部追加，131 → 135）。** `comp_layer_curves_channel_count` /
`_point_count` / `_point_at` / `_set_points`；既有枚举、结构体、签名一个不动。
后三个共用同一份「按 x 排序 + 同 x 去重」结果，两个读入口不会漂移；`set_points` 是
**纯函数**（只在 settings JSON 字符串上操作），既不碰文档也不开事务——撤销步由
`comp_layer_update_adjustment_at`（P0-2 前已存在）产生。

**单一事实源。** Levels 补齐 20 个参数（`levels.ranges.0..3` × black/gamma/white/
outputBlack/outputWhite），量程逐条抄上游 `LevelRange.normalized`（black 0..254、
white 1..255、gamma 0.1..9.99、输出 0..255），落在 `adjustment_table.cpp` 的同一张表里；
白/黑相对顺序不校验，由求值侧 `normalize()` 兜（同上游）。曲线点列的结构约束表达不了在
静态参数表里（4 通道、2..32 点、x 严格递增、首末点 x 锁 0/255、值域 0..255），新增
`engine/domain/curves_settings.{h,cpp}`，由 `adjustment_validate_settings` 调用；
`.comp` 里的对象形式 `{"x":N,"y":N}` 与紧凑形式 `[x,y]` 都接受。

**一步撤销（本轮的核心理由）。** 拖动途中每次写回都是 `comp_layer_update_adjustment_at`，
不开事务时引擎的 `ImplicitTxn` 会给**每次写各记一步**——拖一次滑杆能产生几十步撤销，
`Ctrl+Z` 看起来像「撤销一次没反应」。现在的边界：按下开显式事务 → 拖动期间的写全折在
里面 → 松手 `comp_txn_commit`，整段拖动只占**一步**；值没变（没有任何实际写入）→
`comp_txn_abort`，**不留空撤销步**；`Esc` 或 `WM_CAPTURECHANGED`/`WM_KILLFOCUS`
（失焦、菜单抢捕获）→ abort 回滚到按下前快照；新增态（`selected_adjustment < 0`，还没有
可写对象）拖动只改编辑缓冲、不写文档，仍由 `Ctrl+Shift+U` 落盘——「先调好再加」不该进历史。

**壳侧交互。** 参数行的滑道按下即可左右拖（几何在按下瞬间冻结，拖动途中的重排不会让滑道
在指针下跑掉）；曲线编辑器是「绘图区正方形 + RGB/R/G/B 四页通道条」：空白处按下 = 插入
控制点并立刻进入拖动（一次按下就能定位），按在已有点上 = 拖该点，`Delete` 删焦点点，
两端点锁死（删了定义域就不完整），内部点 x 夹在左右邻点 ±1 内保持严格递增。点列真相在
引擎 settings 里，壳只缓存当前通道用于绘制/命中，每次写完都 `refresh_adjustments` 以引擎为准
回读。滑道矩形、滑道命中、x→值、值吸附、曲线绘图区、通道条、点命中、插入点选址全部是
`ui_core.h` 的纯函数，绘制与命中**调同一批函数**，`tests/test_ui_core.cpp` 直接盯这些函数。

**一次真崩（教训）。** 第一次用鼠标拖曲线点，外壳 `0xc0000005` 崩在 `USER32.dll`
（`fault offset 0x401c3`）。根因是新写的两处状态栏/页脚文字用了
`wsprintfW(L"… (%.0f, %.0f) …")`——**`wsprintfW` 不支持浮点转换**，它按整数宽度取变参，
`double` 被当成指针读飞。改为先 `number_text()`（`snprintf`）再 `%s` 拼入，与壳里其它面板
同口径。自动化此前完全测不到这条路径：`scene_adjustment` 全是键盘，那两行只有「拖动中/绘制
焦点点」时才执行——所以本轮同时补了鼠标驱动的 `curve_levels` 场景。

**验证。** 全量 **553 例 / 0 失败**（P0-2 的 535 + 曲线 ABI/校验 17 例 + `.comp` 点列往返 1 例）；
PE 导出 **135**；`run_regression.py --require-ohos` **6/6 PASS**（壳自检 219 项 SHA256
`e277a4c4…` 与基线逐字节一致，鸿蒙 aarch64 交叉编译 95 目标 0 失败）；
`visual_check.py` **16 场景 74 项 / 0 失败**（新增 `curve_levels` 6 项：
绘图区几何、抬中间调画布变亮、画布直方图中位数右移、一次 `Ctrl+Z` 回原、色阶黑场 40 压暗、
再一次 `Ctrl+Z` 回原；截图 `report/assets/s15/60_*.png`–`66_*.png`）。

**边界。** ① 曲线没有预设库、没有直方图底图、没有贝塞尔手柄（求值固定为 Fritsch–Carlson
形状保持三次 Hermite）；② 色阶通道条只切面板分页（`ShellState.adj_channel`），**不写回**
`levels.channel`，求值恒按 `ranges[0]` 管合成、`ranges[1..3]` 各管一个色彩通道；
③ 布尔参数没有滑道（点一下即切换）；④ 滑杆吸附到整数是「量程 ≥ 50」的统一规则，
没有按住 `Shift` 的精细模式；⑤ 手感/跨 DPI/连续拖动是否跟手仍待 ALEX 目视
（`docs/manual-test.md` M18）；⑥ `scripts/visual_check.py` 的工作区改动按约定仍未提交。

### 6.40 P0-4 选区补齐：扩展 / 收缩 / 按图层像素载入 + 色彩范围对话框（2026-10-21，Claude）

**缺口。** 路线图说这一项「ABI 全齐，3–4 天纯外壳」，实测属实：五条入口都在引擎里有实现、
有单测，**壳侧零调用点**——`comp_selection_modify(EXPAND | CONTRACT)`、
`comp_selection_load_layer(LOAD_PIXELS)`、`comp_selection_apply_color_range` 在
`shells/windows/src/main.cpp` 里搜不到。验收标准：用**魔棒之外的第二条路径**（色彩范围）
选中背景并删掉；扩展 3 px 后边缘无锯齿残留；撤销一步。

**ABI 零改动（135 → 135）。** 本项只接线：没有新导出、没有改签名（追加式约定照旧）。

**第一批（§5.15）。** 「选择(&S)」单列菜单（原全选/取消/反选只搬位置、ID 不变）；
`扩展…`(1047)/`收缩…`(1048) 共用一个半径键入态（1..500，壳侧拦 0）；
`按图层像素载入选区`(1049) 取当前层、`threshold = 1`。壳自检 219 → 228，单测 553 → 554。

**第二批：色彩范围（§5.16）。** 菜单 `色彩范围…`(1050) 开右侧面板，与滤镜/效果面板互斥。
核心是**预览走显式事务**：开面板 `comp_txn_begin`，每次取色/容差/反相在事务内整篇重算
（`COMP_SELECTION_REPLACE`），回车 `commit` = **恰好一步撤销**，`Esc`/没取过色 `abort` =
选区回到打开前且不留空撤销步；取色退到空表时 abort + 立刻 begin 一条新事务，让快照跟着
「回到原选区」的上游语义走。取色 = 合并合成的 3×3 均值，单击替换 / `Shift` 加色 /
`Alt` 排除 / 点色块删色；容差 0..200（缺省 40，步进 1/5/10，数字键入），`I` 反相，
`Backspace` 清空。引擎「0 匹配 = 不改选区」与上游「显示空选区」的缝，由壳在
`changed == 0` 时显式 `comp_selection_deselect`（仍在事务内）补齐。

**两处只有真跑才会暴露的坑。**
① `color_range_txn` 是挂在文档上的裸句柄，而「新建 / 打开 .comp / 打开位图」三条路径都会
`comp_document_destroy` 旧文档——三处都在 destroy **之前**调 `forget_color_range`
（丢句柄、关面板、清取样，**不 abort**），否则对已释放文档提交事务就是 UAF；
② 面板打开时键盘归面板，且**连 `Ctrl+Z/Y/N/O/S/A/D` 也拦**：撤销/换文档会把事务快照
和当前文档撕开，abort 时会「复活」一个本不该存在的选区；缩放类 Ctrl 组合照常穿透。

**验证。** 壳自检 228 → **245 项**（基线显式重定，连续两次运行 SHA256 一致）：同色全选
不要求连通、恰好一步撤销、撤销回到打开前、反相 = 补集且一步撤回、容差 200 是闭区间、
`include == exclude` 零匹配不动选区、容差 201 与 `NULL + count>0` 被 ABI 闸拒绝且
`out_changed` 不被写、事务内连改两次只落一步且以最后一次为准、`comp_txn_abort` 还原选区。
`tests/test_ui_core.cpp` 加 3 例（扩展半径槽 + 色彩范围容差 0 底 / 色块与参数行几何），
单测 **556 例 / 0 失败**；`compositor_tests.exe`、`build_windows_shell.py` 均零警告。
文档：`docs/shell-parity.md` §5.15/§5.16 + 能力矩阵两行，`docs/manual-test.md` 新增
**M19/M20**（M20 就是本项验收路径的 10 条拆解）。

**边界。** ① 色块只画首屏放得下的（330 宽 → 13 个），超出的颜色仍生效但点不到，
要减就 `Backspace` 清空重取；② 色彩范围取样固定 3×3，没有「取样半径」选项，
也没有上游的「存储/载入取样色」；③ 取色基于合成结果，16 位/HDR 文档走 8 位视图
（与魔棒同一家族），不是逐通道深比较；④ 扩展/收缩仍只有菜单入口，没有 PS 的「选择 →
修改 → 边界/平滑/羽化」全套；⑤ **路径面板**（列已闭合路径、双击转选区）本项未做，
是 P0-4 的可选项——已在 §6.41 补齐；⑥ 手感/跨 DPI/大图取色跟手度待 ALEX 目视（M19/M20）；
⑦ `scripts/visual_check.py` 的工作区改动按约定仍未提交，也未加本项的新场景。


### 6.41 P0-4 可选尾巴：路径面板（2026-10-21，Claude）

**先确认能力边界。** 立项前把 ABI 翻了一遍：**没有路径实体**。`comp_selection_apply_path`
是「三次贝塞尔段进、选区出」的一次性调用，没有 `comp_path_*` 句柄，`.comp` 里也没有
路径段。所以路线图这条「列出已闭合路径、激活/删除、双击转选区」只能做成
**壳侧存储 + 复用现有入口**，ABI 零改动（135 → 135）。

**存什么。** 一次钢笔提交时的锚点副本（`PenAnchor` = 文档坐标 + 两控制柄）。
`commit_pen_path` 在清空 `pen_anchors` **之前**复制一份，提交后无条件入列——
即使这次 `changed == 0`（选区没变、事务回滚），锚点仍然有效，可以再落一次。
入列只动 `std::vector`，**不产生撤销步**；上限 64 条，满了 FIFO 丢最旧一条并在状态栏
说明；名字用单调流水号 `路径 N`，删除后不复用。

**面板。** 「选择 → 路径面板…」（命令 ID **1051**，`Ctrl+Shift+P`）挂右侧同一列，
与色彩范围/滤镜/效果面板互斥（开一个关其余；色彩范围先 `abort` 事务再让位）。
键盘归面板：`↑`/`↓` 选行（只滚到刚好够）、`回车` 或**双击行**转选区、`Del` 删除、
`Esc` 关闭，工具字母键被吞掉。`T` 文字工具要独占右栏，用一条谓词
`path_panel_visible() = path_open && tool != Type` 同时约束绘制、键盘、鼠标与调整面板的
命中排除，并在面板挂着但不可见时给出状态栏提示——不制造「画了但点不到」的半截状态。

**转选区 = 一步撤销。** `path_apply_selected` 复用钢笔画路径的 `path_build_cubics`
（闭合子路径，`pen_segment_count(n, true) == n`）再走 `finish_new_selection`，
与框选/套索/魔棒/钢笔同一条事务尾巴，`changed == 0` 时回滚、不留空撤销步。
组合方式固定 `COMP_SELECTION_REPLACE`（双击不适合再按修饰键）。

**文档替换。** 锚点是旧文档坐标，落进新文档会偏。三条换文档路径本来就调
`forget_color_range`（在 `comp_document_destroy` 之前），末尾一并 `path_store_clear`。

**几何单源。** 行高 22 / 列表顶 64 / 底部 96 写进 `ui_core.h`，配
`path_visible_rows` / `path_max_scroll` / `path_clamp_scroll` / `path_ensure_visible` /
`path_row_rect` / `path_row_at`，绘制与命中同源（S15「看得见点不中」那条教训）。

**验证。** `tests/test_ui_core.cpp` 加 `ui_core_path_panel_rows_hit_what_they_paint`
（行数随面板高度、行矩形与行距、命中上/下边界与行尾、滚动钳位、`ensure_visible`）；
单测 556 → **557 例 / 0 失败**；壳自检 **245 项不变**（纯壳侧，不碰 ABI，基线 SHA256
仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.17 + 矩阵一行，
`docs/manual-test.md` 新增 **M21**。

**边界。** ① 路径不落盘（`Ctrl+S` 不带），引擎没有实体，带不上；② 只列名字与锚点数，
不画路径本身，也不能加选/减选、重命名、复制；③ 锚点坐标不随裁剪重映射，裁剪后旧路径
可能部分落到画布外（引擎裁掉，选区只剩可见部分）；④ 列表不在文档历史里，`Del` 删除
不可撤销（状态栏已写明「选区不动」）；⑤ 面板手感与双击时序待 ALEX 目视（M21）。


### 6.42 P1 历史记录面板：把撤销栈列出来，并支持回跳（2026-10-21，Claude）

**立项口径。** 路线图 P1「历史记录面板」：面板列出每一步，点选可回跳，与撤销栈同源。
此前 `History` 只在状态栏露一个数字（`undo_depth` / `redo_depth`），没有任何「看得见的
历史」。这一项把撤销栈原样列成列表，并加「回跳到第 N 个状态」入口。

**ABI 纯尾部追加（135 → 138）。** `comp_document_history_step_count` /
`comp_document_history_step_label` / `comp_document_history_jump`。既有枚举、结构体、
签名一个不动；标签走与 `comp_layer_get_name` 同一套 string-out 规矩（`out_required`
含 NUL、查询模式 `COMP_OK`、容量不足 `INVALID_ARGUMENT`、绝不静默截断）。
**没有**为「当前位置」新增导出：`comp_document_history_stats` 的 `undo_depth` 就是当前
状态下标，多一个导出只会多一份可能不同步的真相。

**列的是状态，不是步。** 第 0 行 = 文档最初状态（新建 / 打开），第 i 行 = 第 i 步之后。
面板高亮不单独记账——它就是引擎的 `current_index()`，所以撤销 / 重做 / 新操作 / 回跳
之后不可能出现「高亮停在不存在的状态上」。壳侧只存一个 `history_scroll`。

**标签顺序不随撤销漂移。** 撤销栈是 `deque`、重做栈是 LIFO，`step_label()` 按
`redo_stack_[size - 1 - k]` 反查，于是「A/B/C 三步、撤销两步」之后第 1..3 步仍报
A/B/C（不是倒序、不串位），新记录丢弃重做链后越界标签自动消失。

**回跳不新增历史记录。** 引擎内部就是连续 `undo()` / `redo()`（只搬栈、不 push），
所以「回跳 → 再回跳」不会把历史越点越长，标签与字节账本就都不变。两条失败路径显式
挡掉：目标越界（`INVALID_ARGUMENT`，调用方先问 `step_count`）；预算过小导致
`trim()` 把刚推入的撤销记录挤掉、目标变得不可达（`COMP_ERR_STATE`，不让循环空转）。
**活动事务内拒绝回跳**（`COMP_ERR_STATE`），与 undo/redo 同一条理由：事务快照会被劈开。

**入口与互斥。** 「编辑 → 历史记录…」（命令 ID **1052**，`Ctrl+Alt+H`；
`Ctrl+Shift+H` 已是「水平翻转」且走 `WM_KEYDOWN`，Alt 组合才落 `WM_SYSKEYDOWN`）。
面板占右栏 `layout.adjust_panel`，与路径 / 色彩范围 / 效果 / 滤镜 / 文字面板互斥；
色彩范围若开着先 `color_range_cancel`（先 abort 再让位）。`T` 文字工具独占右栏，
仍用一条 `history_panel_visible() = history_open && tool != Type` 同时约束**绘制、
键盘、鼠标、滚轮、调整面板命中排除**五处，挂而不见时状态栏点明「切走后可见」。

**操作。** `↑`/`↓` 回跳一格、`Home`/`End` 回跳最前/最后、`Esc` 关闭；工具字母键被吞。
单击行即回跳（与路径面板不同：回跳可再回跳、不产生记录，误点无破坏性），
点空白不落画布，滚轮一格 3 行；重做侧用暗色画。钢笔正在落点时回车/Esc 归钢笔，
否则「画布点了锚点、回车却被面板吃掉」会让路径永远提交不了。

**刷新口径。** 回跳 = 换整篇文档状态（图层、选区、调整都可能变），所以刷新与
`undo_document` / `redo_document` 完全一致（`needs_render` + `refresh_layers` +
`refresh_selection`），只是两次刷新合成一次，再把目标行滚进可视区。

**几何单源。** 面板行几何抽成 `ui_core.h` 的通用 `list_*`
（`list_visible_rows` / `list_max_scroll` / `list_clamp_scroll` /
`list_ensure_visible` / `list_row_rect` / `list_row_at`），`path_*` 与 `history_*`
两组包装都委托给它——路径面板的既有用例原样通过，历史面板的绘制与命中同源。

**验证。** 领域层加 `history_step_labels_follow_undo_and_redo`；ABI 层加
`abi_history_panel_steps_labels_and_jump`（查询模式 / 容量不足被拒 / 前后回跳的像素与
`undo_depth` / 相等回跳空操作 / 越界被拒 / 事务内被拒且 abort 后可用）；UI 层加
`ui_core_history_panel_rows_hit_what_they_paint`。单测 557 → **560 例 / 0 失败**；
PE 导出 **138**；壳自检 **245 项不变**（面板不进自检脚本，基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.18 + 矩阵一行，
`docs/manual-test.md` 新增 **M22**。

**边界。** ① 只读面板：不能重命名 / 删除单条 / 清空历史（上游右键菜单未做）；
② 标签是写入方给的（壳里是英文操作名），面板不改写成中文；③ 历史被预算裁掉后
列表跟着收缩，不额外提示「更早的记录已丢弃」（状态栏字节数仍在）；④ 粒度是一步，
笔迹仍是每笔一步，没有上游的「合成笔迹」分组；⑤ 回跳手感与高亮跟手度待 ALEX 目视（M22）。

### 6.43 P1 减淡 / 加深 / 海绵：色调工具进入产品通路（2026-10-21，Claude）

**立项口径。** 路线图 P1「减淡 / 加深 / 海绵」：三种工具能改像素且受选区约束，
撤销一步。此前工具表里没有这一族，引擎也没有任何色调算子；上游的
`RetouchTools.swift` 同样是工具层逻辑（没有对应 `.c` 内核），所以这一项是**域层新增**，
不是把已有内核接出来。

**ABI 纯尾部追加（138 不变）。** `comp_tool_op` 追加 `COMP_TOOL_DODGE = 16` /
`_BURN = 17` / `_SPONGE = 18`，`comp_tool_op_desc` 尾部追加
`comp_tool_tone tone { int32_t mode; float strength; }`。既有枚举值、既有字段布局、
既有导出一个不动；**没有新增导出**（仍 138），宿主照旧只认 `comp_tool_apply`。

**为什么共用一个 `strength`。** 上游 Dodge/Burn 叫 Exposure、Sponge 叫 Flow，
都是「这一笔有多强」的单一标量；拆成两个字段只会让宿主在切工具时搬一个恒为 0 的值。
`mode` 复用同一条：Dodge/Burn 是 Range（0 = 中间调 / 1 = 阴影 / 2 = 高光），
Sponge 是方向（0 = 去色 / 1 = 加色），越界一律 `INVALID_ARGUMENT`，不静默夹取。

**像素口径。** 图层是预乘 RGBA8：先按 `alpha` 反预乘到直通色、运算、再用**原 alpha**
重新预乘量化——`alpha` 一个字节都不改，`alpha == 0` 的像素跳过（没有颜色可提亮/去色）。
Range 权重按 Rec.601 亮度算，再走 smoothstep（线性边界在中间调/高光交界会出台阶）；
直通色在 `[0,1]` 夹取后再乘回 alpha，避免反预乘的舍入把 255 顶出界。

**选区与图层边界。** 与渐变 / 形状 / 文字同一条规矩：在**文档分辨率**取
`selection_effective_coverage()`，按 `effective_bounds()` 的原点把图层像素映射到画布，
覆盖率为 0 或画布外的像素**逐字节不动**；笔尖与图层完全不相交时整步 `txn_abort`。

**空撤销防线（本项自查修掉的缺陷）。** 第一版无条件写像素并记 `changed = true`，
于是「海绵加色碰到纯灰」「减淡碰到纯白」会留下**像素逐字节相同的空撤销步**。
现在每个通道**量化后与旧值比较**，只有真的有字节变化才写、才置 `changed`；
全笔逐字节不变时 `txn_abort` + `COMP_OK` + note
`tone stroke changed no pixel (arithmetically a no-op)`。同理，`strength == 0`
在参数闸就被拒绝（与渐变 opacity 0 同口径），不会靠「算出来没变」兜底。

**16 位 / HDR。** 三种色调算子属于**落笔族**（`is_stroke_family`），与画笔 / 橡皮擦 /
仿制 / 涂抹 / 液化 / 模糊共用 `DeepPaintCopy` + `deep_take_painted(_hdr)`：
只有真正被改动的像素拿到深值，笔尖之外保住 16 位 / half，不是先降 8 位再补。

**外壳（`O`）。** 新增 `ToolId::Tone`（插在 Retouch 之后）与 8 档状态机：
`Dodge/Midtones → Dodge/Shadows → Dodge/Highlights → Burn/Midtones → Burn/Shadows →
Burn/Highlights → Sponge/Desaturate → Sponge/Saturate`，重复按 `O` 循环，从别的工具
切进来回到第一档。算子与 `mode` 由 `tone_state_tool()` / `tone_state_mode()` 从
**同一张状态表**换算，壳内不存第二份，状态栏文案不可能与实际下发的参数不一致。
`Alt+[` / `Alt+]` 调强度（0.01..1.0，10% 一步）。笔迹走与画笔相同的折线采集
（`StrokeKind::Tone`），松手一次 `comp_tool_apply`，`ReleaseCapture` 之前先把折点搬走
（S15 那个 `WM_CAPTURECHANGED` 缺陷的同一条防线）。

**验证。** 领域层新增 4 例（Dodge/Burn 方向 + 撤销逐字节、Sponge 去色 + 纯灰 no-op
不记步、参数闸先于事务、RGBA16 笔尖外保深）；ABI 层新增 3 例（枚举值冻结、经
`comp_tool_apply` 的提亮/压暗与一步撤销、左半选区裁剪 + 非法 mode/strength 不留步）；
UI 层新增 1 例（8 档映射、越界归位、文案、强度钳位与步进）。单测 560 → **568 例 /
0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/tool-ops.md` §4.13、
`docs/shell-parity.md` §5.19 + 矩阵一行，`docs/manual-test.md` 新增 **M23**
（M1 的工具顺序改为 19 个）。

### 6.44 P1 历史记录画笔：从任意历史状态取样，随笔迹刷回（2026-10-21，Claude）

**立项口径。** 路线图 P1「滤镜库扩容 + 历史记录画笔」里可独立交付的一半。历史快照
（`HistoryEntry{before, after}` 整状态快照）与按状态回跳（§6.42）都已具备，缺的只是
「从第 N 个状态取样，按笔迹刷回当前层」。

**零新增导出。** `comp_tool_op` 尾部追加 `COMP_TOOL_HISTORY_BRUSH = 19`，
`comp_tool_op_desc` 尾部追加 `comp_tool_history_brush { int32_t state; float opacity; }`；
既有枚举值、既有字段布局、既有导出一个不动，宿主照旧只认 `comp_tool_apply`。
PE 导出仍 **138**。

**`state` 是面板下标，不是撤销步数。** 引擎直接读 `History::state_at(index)`，取样
与撤销栈天然同源，不存在第二份快照。越界 / 负数在参数闸就 `INVALID_ARGUMENT`；
下标合法但那一份状态里**没有这个图层**（图层是后加的）→ `STATE`。两种拒绝都在
`txn_begin` 之前返回：不改像素、不留撤销步、不动历史栈。

**只读借用 + 深像素。** 快照是定长借用，事务未提交期间历史栈不动，因此可以逐点读、
不持有跨调用指针；16 位 / HDR 取样源经 `deep_to_rgba8` / `hdr_to_rgba8` 转成 8 位
工作副本（**不改写快照**）。写回走落笔族的深像素路径（`is_stroke_family`）：只有
真正被改动的像素拿到 `8 位值 × 257` 的深值，笔尖之外保持原深度——两次撤销后深像素
逐字节复原。

**混合口径。** 逐像素权重 = `笔迹覆盖率 × opacity × 选区覆盖率`，预乘空间线性插值
`sample × w + current × (1 - w)`。预乘下的线性插值就是 source-over，**不反预乘**：
反预乘会在低 alpha 边缘把颜色放大到 1.0 再乘回，产生可见偏差。包围盒可能已经变了
（图层被移动/缩放过），因此按「当前图层像素 → 文档坐标 → 取样图层像素」两级映射，
两级都只认 `effective_bounds()` 原点（与 clone / 渐变同口径，不认旋转）。

**成功 no-op 不落撤销步。** 笔尖与图层不相交、取样值逐字节等于当前像素、被选区
整片遮住，三种情况都 `txn_abort` + `COMP_OK` + note
（`history brush stroke does not intersect the layer…` /
`history brush changed no pixel (the sample equals the current pixels)` /
`history brush is masked by the active selection…`）。

**外壳（`Y`）。** `ToolId::HistoryBrush` **追加在工具表末尾**（旧工具下标全部不动，
Clone 仍是 11、Pen 仍是 4）。壳侧只存 `history_brush_state`：`-1` = 自动 = 当前状态
的上一格（「刚画错一笔，拿这支笔抹掉」），`>= 0` = Shift+点历史面板某行显式指定——
**显式取样不回跳**，当前画面必须停在最新；面板该行标 `← 画笔取样`（琥珀色），与
「当前状态」的蓝底高亮互不覆盖。普通点行仍是回跳，两者互不干扰。切到该工具自动
打开历史面板；`Alt+[` / `Alt+]` 调不透明度（10% 一步，键盘下限 10%，因为引擎拒绝 0）；
状态栏与工具栏常显「取样 #N / 不透明度 X%」。笔迹走 `StrokeKind::HistoryBrush`，
松手一次 `comp_tool_apply`（= 恰好一个撤销步），`ReleaseCapture` 之前先把折点搬走
（S15 缺陷的同一条防线）。取样状态**只在提交时读一次**：笔画途中打开/回跳面板不会
改变这一笔的来源。

**验证。** ABI 层 2 例（经 `comp_tool_apply` 取样刷回 + 恰好一步撤销、参数闸先于
事务且 no-op 不留步）；领域层 4 例（`state_at` 下标映射、笔尖下取样生效、闸先于事务、
RGBA16 笔尖外保深 + 两次撤销逐字节复原）；UI 层 1 例（工具下标 19 / `StrokeKind` 值 /
不透明度钳位与步进）。单测 568 → **575 例 / 0 失败**；PE 导出 **138 不变**；壳自检
**245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。文档：`docs/tool-ops.md` §4.14、
`docs/shell-parity.md` §5.20 + 矩阵一行，`docs/manual-test.md` 新增 **M24**
（M1 的工具顺序改为 20 个）。

### 6.45 P1 滤镜库扩容：Gaussian Blur / Add Noise / Grain（2026-10-21，Claude）

**立项口径。** 路线图 P1「滤镜库扩容 + 历史记录画笔」的另一半（§6.44 是画笔）。
三个算子的像素内核在本仓早已就位（`blur_rgba8_copy`、`noise_add_at`、`adjust_grain`），
缺的只是 Filter 菜单的产品入口。本批只做装配：参数契约 → 参数闸 → 内核 →
选区混合 → 一步撤销，不新增任何像素算法。

**零新增导出。** `comp_filter_kind` 尾部追加 `COMP_FILTER_GAUSSIAN_BLUR = 4` /
`COMP_FILTER_ADD_NOISE = 5` / `COMP_FILTER_GRAIN = 6`；`comp_filter_desc` 尾部追加
`gaussian_blur` / `add_noise` / `grain` 三个子结构。既有枚举值、既有字段偏移、
既有导出一个不动，宿主照旧只认 `comp_filter_apply`。PE 导出仍 **138**。

**零初始化 = 恒等**（与既有四个滤镜同规矩）：blur `radius = 0`、noise `amount = 0`、
grain `amount = 0` 都不碰像素、不开事务、`out_changed = 0`。参数闸仍是「0 或上游量程」：
blur `0.1..250`、noise `0.1..400`、grain `amount 0..100 / size 0.5..20 / roughness 0..100`；
`(0, 0.1)` 这类夹缝显式拒绝，不静默夹到端点。

**三条口径各对齐上游。** ① Gaussian Blur 复用共享的三遍盒式模糊，
`sigma = radius * scale`——与 Gaussian Blur 调整层（kind 7）同一实现，note 写明
`box approximation ... (sigma = radius * scale)`，不声称与 CI 逐位一致；
`preserve_transparency = 1` 时用模糊后的 alpha 反预乘颜色、再乘回**原** alpha，
因此 alpha 逐字节不变、轮廓不扩散。② Add Noise 的噪声场锚在**文档坐标**
（origin = 图层 `effective_bounds()` 原点，对齐上游 `noiseOrigin`），图层移动后图案
不会在像素上「游动」；同 seed + 同文档位置逐字节可复现。③ Grain 与 Grain 调整层
同一内核，采样在图层像素空间（origin 0，`unitsPerPixel = 1 / scale`）。

**外壳。** `FilterId` 追加 4/5/6（`Count` 移到 7），`filter_table()` 追加三行
（面板行数 2 / 4 / 4），菜单与面板仍从同一张表生成；`build_filter_desc` 是唯一映射点，
布尔项按 `>= 0.5` 转 int32、`Seed` 用 `llround` 转 uint32（面板量程正好是 uint32 全域）。
`main.cpp` 的三条 `static_assert` 把壳枚举与 ABI 数值锁死。

**验证。** 领域 / ABI 层 16 例（新增 5 例）：三个新 kind 的零初始化恒等；越界闸
（blur radius 0.05 / 250.1、noise amount 0.05 / 400.1、grain size 0.4 / 20.1、
roughness 101）；Blur 真软化 + preserve 下 alpha 逐字节不变且结果与默认路径不同；
Noise 同 seed 复现 / 换 seed 不同 / **文档坐标锚定**（缩进图层与全幅图层重叠处一致）/
monochromatic 三通道相等；Grain 同 seed 复现 / 换 seed 不同；三者均恰好一步撤销并可
逐字节还原；ABI 字段搬运与越界拒绝。单测 575 → **580 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。文档：`docs/filters.md` §1 / §3.5–3.7 /
§5 / §6 / §7 / §8，`docs/shell-parity.md` §5.3，`docs/manual-test.md` M9 追加 5–8 条。

### 6.46 P1 滤镜库扩容（收尾）：Motion Blur（2026-10-21，Claude）

**立项口径。** 上游 `FilterKind` 里最后一个**完全没有产品通路**的算子。前七个的像素
内核在本仓早已就位，Motion Blur 没有对应内核（上游是 Core Image `CIMotionBlur`），
本批按它的可观察口径补一份盒式线积分，并接上同一条 Filter 入口。

**零新增导出。** `comp_filter_kind` 尾部追加 `COMP_FILTER_MOTION_BLUR = 7`；
`comp_filter_desc` 尾部追加 `motion_blur { angle, distance }`。既有枚举值、偏移与导出
一个不动，PE 导出仍 **138**。

**口径。** `angle` -90..90（水平 0，逆时针为正，屏幕 y 向下故内核取 `dy = -sin`）；
`distance` 0 = 恒等，否则 1..2000（图层像素，预览再乘 `scale`），`(0, 1)` 显式拒绝。
恒等判据**只看 distance**：角度取任何合法值都不开事务。采样在预乘像素上做双线性平均
（alpha 一起平均，条纹向外扩散），越界坐标夹到图层边缘（等效上游
`extending(...past: canvasSides)`），每像素最多 257 个采样点。

**为什么不声称与 CI 逐位一致。** 上游把 `distance / sqrt(12)` 传给 `CIMotionBlur` 的
radius——即让 CI 的分布与长度为 `distance` 的均匀条纹同方差。本仓直接对长度
`distance` 做盒式平均：方向与长度两条可观察口径一致，形状不同，note 写明
`CIMotionBlur's radius is the equivalent standard deviation`。

**外壳。** `FilterId` 追加 7（`Count` 移到 8），`filter_table()` 追加一行
（Angle / Distance 两行），菜单与面板仍从同一张表生成；`build_filter_desc` 追加
唯一映射分支；`main.cpp` 补一条 `static_assert` 锁死枚举数值。

**验证。** 领域 / ABI 层 18 例（新增 2 例）：`angle = 0` 只沿 x 拉出条纹
（远端行逐字节不动、不沿 y 移动），`angle = 90` 转竖直，`scale = 0.5` 把条纹长度
减半，一步撤销逐字节还原；越界闸（angle ±90.5、distance 0.5 / 2000.5）；ABI 字段
搬运与 note caveat。单测 580 → **582 例 / 0 失败**；PE 导出 **138 不变**；壳自检
**245 项不变**（SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。文档：`docs/filters.md` §1 / §3.8 /
§5 / §6 / §7 / §8，`docs/shell-parity.md` §5.3，`docs/manual-test.md` M9 追加第 9 条。

### 6.47 P1 滤镜库扩容（收尾）：Progressive Blur（2026-10-21，Claude）

**立项口径。** 上游 Filter 菜单里最后一个还没有产品通路的算子（`Filters.swift:387-413`：
`CIMaskedVariableBlur` + 线性 / 径向渐变遮罩，`strongest = max(from, to)`，
`guard strongest > 0`）。本仓没有「逐像素半径」的模糊内核，按上游的可观察口径
（两端半径、渐变方向、`radial` / `inverted` 语义）补一份 **8 档三角混合**，
并接上与其余八个算子同一条 `comp_filter_apply` 入口。

**零新增导出。** `comp_filter_kind` 尾部追加 `COMP_FILTER_PROGRESSIVE_BLUR = 8`；
`comp_filter_desc` 尾部追加
`progressive_blur { from, to, radial, inverted, start_x, start_y, end_x, end_y, explicit_axis }`。
既有枚举值、字段偏移与导出一个不动，PE 导出仍 **138**。

**口径。** `from` / `to` 各 0..250（图层像素，预览再乘 `scale`），恒等判据
`max(from, to) <= 0`（对齐上游 `guard strongest > 0`）——零初始化不碰像素、不开事务、
`out_changed = 0`。域层用反相的 `auto_axis`（缺省 `true`），ABI 用 `explicit_axis`
（缺省 0），两侧零初始化都是「恒等 + 缺省轴」。`inverted` 交换两端半径。
实现把 `[from, to]` 等分成 8 档、逐档跑共享内核 `blur_rgba8_copy`，每个像素按它在轴上
的位置 `t ∈ [0,1]` 在相邻两档间取三角权重（权重和为 1）。**第 0 档是起点的半径而不是
原图**：`from > 0` 时它同样要模糊，否则均匀档会在轴起点留一条不模糊的边；`from = 0`
才用原图（0 半径用 0.1 px 近似会平白改掉一像素）。因此 `from == to` 与同 sigma 的
Gaussian Blur **逐字节相同**，由用例钉住。

**轴与亚像素。** 显式轴端点经新增的 `Layer::document_to_pixel_continuous`
（归一化 → 逆旋转 → 翻转，同一套仿射）投到图层像素空间，**保留亚像素**、允许落在
缓冲外；取整版会让同一对端点随缩放 / 旋转抖动 ±1 px，方向随之跳变。线性档
`t = dot(p - start, d) / |d|²`，径向档 `t = |p - start| / |d|`，都夹到 0..1；
轴退化（`|d| < 1 px`）时终点取 `start + (0, +1)`——本仓像素 y 向下，与上游在 CI 里
取 `start + (0, -1)` 是同一方向。缺省轴（`explicit_axis = 0`）取**图层自身像素矩形**
的中线（`x = (w-1)/2`，`y` 从 0 到 `h-1`）：域层没有画布上下文，不替宿主猜，
note 写明 `axis defaulted to the layer's mid-line`。

**为什么不声称与 CI 逐位一致。** 上游 `CIMaskedVariableBlur` 是逐像素连续变化半径；
本仓切成 8 档均匀模糊再按三角权重混合。两端半径、渐变方向、`radial` / `inverted`
语义一致，过渡带内不逐像素等同，note 写明
`8-level blend ... the ramp is not pixel-identical`。

**外壳。** `FilterId` 追加 8（`Count` 移到 9），`filter_table()` 追加一行
（From / To / Radial / Inverted，4 个参数，仍在上限 6 之内），菜单与面板仍从同一张表
生成；`build_filter_desc` 追加唯一映射分支。渐变的两个端点不在面板行里，而是在画布上
**拖一条线**定轴（`progressive_axis_begin/extend/end`）：拖拽中画实线、松手画虚线，
径向档另画半径圆；未拖拽时 `explicit_axis = 0` 用缺省中线。拖轴本身不进撤销栈，
只有回车应用才产生一步 `Filter: Progressive Blur`。`Ctrl+N` / 打开 `.comp` 时作废旧轴
（端点是旧文档坐标）——新建与打开两条路径各补一次 `progressive_axis_set = false`，
与 `filter_open` / 笔迹清理放在同一处。

**验证。** 领域 / ABI 层 22 例（新增 4 例）：缺省轴自顶向下（顶部方差远大于底部、
起点行逐字节不变）；显式水平轴锐端在 `start`、`inverted` 换到另一端；径向档中心锐、
角上糊；`scale = 0.25` 时远端比全尺寸更锐；`from == to` 与同 sigma 的 Gaussian Blur
逐字节相同；一步撤销逐字节还原；越界闸（`from -0.5`、`to 250.5`、NaN 轴端点）；
ABI 字段搬运（含 `explicit_axis` 反相映射）与 note caveat。单测 582 → **586 例 /
0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**（SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/filters.md` §1 / §3.9 / §5 / §6 / §7 / §8，`docs/shell-parity.md` §5.3，
`docs/manual-test.md` M9 追加第 10 条。

### 6.48 图像菜单接线：转画布 / 修边 / 反相（2026-10-21，Claude）

**缺口在壳，不在引擎。** `comp_document_rotate_canvas` / `comp_document_trim` /
`comp_layer_invert` 自 §6.35 起就有实现与用例（上游 `CanvasRotation.swift` /
`ImageTrim.swift` / `PixelInvert.swift`），但 `shells/windows` 里**零调用**，
顶层菜单也没有「图像」项——能力在，产品通路不在。本轮补菜单、命令 ID 与一个
修边参数面板，让三条命令真的能用。

**旋转不重采样。** 引擎按 `comp_canvas.h` 的口径只换画布尺寸与图层落位
（90° 时 width/height 互换，180° 不变），像素字节一个不动；选区的蒙版随之转置。
壳侧不复制像素，统一走 `finish_canvas_command`：重取文档尺寸、置
`needs_fit`（旧视图偏移对应旧画布）与 `needs_render`，再从引擎回读图层与选区——
**缓存一律作废而不自己推**。撤销计数同样回读，状态栏写「已旋转画布：顺时针 90°」
这类具体文案；空文档旋转后尺寸不变时引擎 `out_changed = 0`，壳明确写
「未产生撤销步」，不假装成功。旋转会先收掉修边面板：面板里的四条边是按旧画布算的。

**修边面板（`ui_core.h` 的 `TrimPanel`）。** 六行 Based On / Top / Bottom / Left /
Right / Tolerance，无堆分配；行几何 `trim_row_rect` 同时供绘制与命中（沿用
「看得见就点得中」）。Tab / Shift+Tab 切行并夹在两端；`-` / `=` 对基准行循环三档、
对四条边是开关、对容差 ±1 并夹 0..255；数字键跳到容差行进入键入态（最多 3 位，
`Esc` 取消、`Backspace` 删字符）。面板值 → `comp_trim_options` 只在
`build_trim_options` 一处映射。四边全关在本地拦下（「至少勾选一条边」），
`STATE`（空文档 / 无内容）与 `changed == 0`（裁出来等于当前画布）都保持面板打开、
不产生撤销步；成功才关面板、走共同尾部并报「已修边（基于 … · 容差 N）」。

**反相是破坏性落墨，与调整层并存。** 目标为当前选中图层：组 / 无像素 →
`STATE`，HDR（RGBA16F）→ `UNSUPPORTED`，两者都不改像素、不落步；预乘语义
（`c → alpha - c`，透明仍透明）与选区混合由引擎负责，显式空选区 `changed == 0` →
「选区为空，未改动」。16 位图层按引擎既有口径写回后永久变 RGBA8（撤销可恢复
原 16 位快照），壳不另作提示。快捷键 `Ctrl+I` = 反相，`Ctrl+Shift+I` 仍是反选。

**互斥与穿透。** `trim_open` 进入右栏单挂位：打开时取消色彩范围的预览事务并关掉
滤镜 / 效果 / 路径 / 历史；反向打开那些面板也会关掉修边。面板拥有键盘时非 `Ctrl`
按键一律吞掉，只有 `Ctrl+…` 快捷键穿透——与 §5.16/§5.17 同一套规矩。
菜单为「图像(&I)」：图像旋转子菜单（CW / CCW / 180°）、修边…、反相（Ctrl+I）。

**验证。** UI 层 2 例（`TrimPanel` 三档循环 / 边开关 / 容差钳位 / 重置 / 名称表；
行几何命中首末行、行外与空行数）。单测 586 → **588 例 / 0 失败**；PE 导出
**138 不变**（本项零新增导出）；壳自检 **245 项不变**（基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.21、
`docs/manual-test.md` M25。

### 6.49 仿制图章接进外壳（S）（2026-10-21，Claude）

**第三个「引擎有、壳没接」的工具。** 工具栏从第一版起就有 `Clone Stamp`（`S`）按钮，
引擎自 F3 起就有 `COMP_TOOL_CLONE` 与 `comp_tool_clone`，但壳里**一个调用点都没有**：
点画布只会落到 `canvas_click` 写一行坐标，按钮是死的。本轮把两段式交互补齐。

**ABI 是两段式的，宿主必须自己记偏移。** 引擎不保存「上一次的源点」：
`has_offset == 0` 时按 `round(source - first_point)` 现算首笔偏移，而
`comp_tool_apply` 的 desc 是 `const`，算出来的值不会回填。所以壳在首笔成功后用
`comp_ui::clone_stroke_offset`（`std::lround`，.5 远离零）算出同一组整数存进
`ShellState`，之后每一笔带 `has_offset = 1` 复用——这就是 `aligned = 1` 的对齐语义。
取整口径必须与引擎一致：`static_cast<int>` 是截断，首笔会差半像素，而症状是
「第一笔对、之后每一笔整体偏一点」，极难归因。

**交互。** Alt+点击画布 = 落源点（不落笔），源点吸附到像素（`floor`）并清零偏移；
同时按住 Shift = `sample_all_layers`（整篇合成取样，否则只取目标图层；这是选项的
持久态，`Shift+K` 可随时切换，见 §6.58）。
普通拖动才落笔；单击也是一记 dab（单点折线）。未设源点时按下不捕获、
状态栏直接写「先 Alt+点击画布设置源点」。源点跟随会话保留（切走再切回仍有效），
但**换文档必须复位**：源点是旧文档的像素坐标，新建 / 打开 .comp / 拖入位图三条
路径都调 `clone_reset_source`。提交参数与画笔同源（直径 / 硬度 / 满压 / flow 1 /
seed 0），`clone.opacity` 显式给 1——零初始化等于 0，引擎会当成「什么都不改」。

**反馈。** 笔迹拖影复用既有 `stroke_points` 通路（非落色笔迹用高对比青）；
画布上按文档坐标画源点十字（每帧 `doc_to_screen` 现算，缩放 / 平移后跟着走，
只在选中该工具且已设源点时出现）；工具栏与状态栏常显「源 x,y / 图层|合并」，
未设源点时写「未设源点」。成功文案带折点数、源点、取样口径，引擎有 note 时附在
「｜」后，随后 `refresh_history_status` 回读撤销计数。

**复用纪律。** 与 Retouch / Tone / History Brush / Paint 同一条
「鼠标折线 → `comp_tool_apply`」通路：`StrokeKind::Clone` 只决定松手时装配哪个算子。
折线仍**先 swap 再 ReleaseCapture**（S15 的坑：`ReleaseCapture` 会同步派发
`WM_CAPTURECHANGED` → `stroke_cancel` 清空点集），失焦 / 捕获丢失走同一个 `stroke_cancel`。
零新增 ABI 导出。

**验证。** UI 层 1 例（首笔偏移的四舍五入：89.6→90、29.4→29、±0.5 远离零、
负偏移 -2.5→-3 / -2.4→-2、空指针安全，以及 `StrokeKind::Clone` 与其余取值互不相同）。
单测 588 → **589 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.22、
`docs/manual-test.md` M26。遗留：Spot Healing（`J`）当时仍是死按钮，已在 §6.52 接线；
仿制不透明度也还没给调节入口（固定 1.0）。

### 6.50 缩放工具接进外壳（Z）（2026-10-21，Claude）

**第一个「按钮有、行为没有」的视图工具。** `Z` 从工具栏第一版起就在，
`ui_core::CanvasView::zoom_at / zoom_by` 也早有单测（锚点不动、上下限夹取），
但壳里没有调用点：选中 `Z` 后点画布落到 `canvas_click`，只写文档坐标，
缩放只能走 `Ctrl+= / Ctrl+- / Ctrl+0 / Ctrl+1`。manual-test M12 曾把它记成已知边界。

**实现。** `zoom_tool_click` 只碰 `view.zoom / pan`：点击放大 1.2 倍、
`Alt+点击` 缩小 1/1.2，锚点是**点击点**（`zoom_by` 的既有语义），所以连点时
光标下的像素不动、不会越点越偏；`needs_fit = false` 防止下一次尺寸变化又把视图
重置回适合窗口。纯视图操作：不进撤销栈、不改文档像素、不碰选区。

**到顶要说话。** 尺度夹取在 `ui_core`（2%..3200%），到顶后再点 `zoom_at` 算出的
仍是同一个值。壳侧比较点击前后的 `zoom`，相等时写「已到上限 3200% / 下限 2%，
画面不再变化」——否则用户看到的是一个完全不响应的按钮。

**分发位置。** 放在 Hand / 空格平移之后、所有绘制工具之前：空格临时抓手仍然
优先于 `Z`（PS 同语义），而 `Z` 又先于渐进轴 / 裁剪 / 选框等分支，
避免「面板开着 + 选了 Z」时点击被别的工具吃掉。状态栏选中工具时给出
「点击放大 1.2 倍，Alt+点击缩小；Ctrl+0 适合窗口 / Ctrl+1 实际像素」。

**验证。** 无新增单测：本项零新增纯模型代码，`zoom_at` 的锚点不变与
`clamp_zoom` 上下限在原用例里已覆盖。单测 **589 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.23，`docs/manual-test.md` M12 补步 6–8。
遗留：拖出矩形缩放到该区域（marquee zoom）、双击 = 实际像素、缩放光标形态
（三项已在 §6.54 补齐）。

### 6.51 Move 工具拖拽图层（2026-10-21，Claude）

**同一把工具、两种对象。** Move（`V`）原来只认选区：有选区就搬像素，
没有选区直接报错「移动：请先选择一个图层」——虽然那条文案本来就暗示了用户想要什么。
图层 transform 的写入侧（`comp_layer_set_transform`）、读取侧（`comp_layer_get_transform`）
和旋转字段都已在 ABI 里，缺的一直是壳侧的拖动循环，所以 §5.11 表里只能写「未做」。

**分流规则。** `move_begin` 按下时看 `selection_active`：有选区 → 原路径不动
（选区像素搬运、松开时一次性提交、预览靠蚂蚁线跟随）；无选区但有选中图层 →
新路径 `layer_move_begin`。这条规则把 Photoshop 那个「选区内外行为不同」的坑
缩到最小：本项只做「没有选区时拖图层」，有选区时仍然只动像素，
不会出现「拖了一下，像素和图层一起动」。

**一次拖动 = 一步撤销。** 按下时 `comp_txn_begin("Move Layer")`，
`layer_move_extend` 每次鼠标移动用 `base + (doc - start)` 重算目标矩形并调
`comp_layer_set_transform`——**目标始终由按下时的基准矩形推导**，不在上一帧结果上累加，
所以来回拖动不会因取整而漂移。因为写的是真 transform，渲染管线直接就能画对，
旋转过的图层也不需要额外几何预览代码。松开时 `comp_txn_commit`，
没动过（`dx == dy == 0`）则 `comp_txn_abort` 并写「位置未变化」，不产生空撤销步。

**两个容易踩的地方。** 一是顺序：`layer_move_end` 必须**先清状态再
`ReleaseCapture()`**，因为捕获变更会同步重入 `WM_CAPTURECHANGED → move_cancel`，
顺序反了就会把刚提交的事务 abort 掉。二是换文档：`move_cancel` 在三条
打开/新建/关闭路径上都在旧文档 `destroy` 之后才被调用，此时
`shell.document != shell.layer_move_doc`，所以 abort 前必须比对这个句柄
——这是 `forget_color_range` 那条注释写的同一个坑，abort 一个已释放文档的事务就是 UAF。

**取消。** `Esc` 走 `move_cancel`（abort 回滚到按下时的位置，写「已取消移动，
图层位置未改变」）；失焦、捕获丢失同样走 `move_cancel`。选区路径与图层路径共用
`move_cancel`，它内部对两条路都做清理，所以不存在「切了一半留下的 active 标志」。

**组图层直接拒绝。** 组的 `transform` 不参与合成——`filter_graph` 只烘焙成员，
组节点不读自己的 transform（`engine/compositing/composite_ops.cpp:472` 明确拒绝
烘焙组）。所以拖组即便写成功也是**静默无操作**，不如按组时报
「组图层不参与 transform 合成，暂不支持整组拖动」，让用户知道该拖组里的成员。
（后续：§6.61 起改为平移组成员，组自己的 transform 仍然不写。）

**验证。** 纯外壳改动，无新增单测（`ui_core` 不参与）。单测 **589 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.24（并回填 §5.11 表格），`docs/manual-test.md` M27。
遗留：`Alt` 拖动 = 边移边复制图层、方向键微调、拖出画布时的位置提示、
旋转/缩放手柄（transform 的尺寸与角度目前只能由引擎侧命令改）、
组图层整体移动（要先让合成侧读组 transform）。

### 6.52 Spot Healing 接进外壳（J）（2026-10-21，Claude）

**最后一个「有按钮、没行为」的绘制工具。** `J` 的工具栏按钮与画笔轮廓从第一版就在，
但壳里没有调用点，点画布只会写文档坐标。引擎侧其实早就完整：`COMP_TOOL_HEAL`
带 PatchMatch、三种模式、不透明度和 `seed`（同一 `seed` 结果确定），
缺的只是宿主怎么把一条笔迹变成它要的整层蒙版。

**笔迹 → 蒙版。** 折点仍走既有 `stroke_begin / stroke_extend`（文档坐标）。松手时
`heal_layer_point` 对每个折点做与引擎 `Layer::map_document_to_pixel` **同一套逆变换**
（归一化 → 逆旋转 → 翻转 → 像素），得到图层像素坐标；`heal_rasterize_stroke`
再沿折线按 `spacing = max(0.75, radius * 0.25)` 细分，逐点盖软圆章
（`inner = radius * hardness`，到 `radius` 线性衰减，重叠取 `max`）。
硬度、直径直接复用画笔的 `[` `]` 与 `Shift+[` `]`，没有第二套参数。
蒙版容器按**画布**尺寸分配——这是壳在纯 origin、无缩放时唯一可靠的尺寸来源。

**一笔一步撤销。** 光栅化完只调一次 `comp_tool_apply(COMP_TOOL_HEAL)`，
`opacity / mode / seed = 1` 一起传；引擎按 `coverage × opacity` 替换像素。

**一个必须先堵的洞。** 域层的 heal 闸只校验 `coverage_stride >= 图层宽`，
看不到宿主缓冲有几行，执行时却按**图层像素高**逐行 `memcpy`。
而图层像素尺寸并不总等于画布：`comp_io_import_layer` 按解码图的原始尺寸建像素，
导入一张比画布高的图，宿主画布大小的缓冲就会被读到尾外。修法是
`comp_tool_op_desc` **尾部追加** `heal_coverage_rows`（既有字段布局不动、导出数 138 不变），
宿主填实际行数；ABI 在写入前发现 `0 < rows < 图层高` 就返回
`COMP_ERR_INVALID_ARGUMENT`，壳翻成「图层像素尺寸大于画布，当前 coverage 装不下」。
`0` = 旧宿主按图层高信任，行为与追加前完全一致。

**键位。** 重复按 `J` 在三档算法里循环（Content-Aware / Create Texture /
Proximity Match），从别的工具切进来一律回到 Content-Aware，避免上次的算法静默生效；
`Alt+[ / ]` 调不透明度（10% 一步、下限 10%，与历史记录画笔同口径）；
取消走通用 `stroke_cancel`（`Esc`、失焦、捕获丢失都覆盖），没有 Heal 专属残留状态。

**验证。** 无新增单测（本轮没有新的纯模型代码，`ui_core` 里只是三个与既有
`step_*` 同构的夹取函数）。单测 **589 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.25（并回填 §5.11 表格、§5.22 遗留项）、
`docs/manual-test.md` M28、`docs/tool-ops.md` 的 coverage 行数说明。
遗留：修补不看当前选区、非等比缩放时半径取两轴平均、无拖动中预览、
无「采样全部图层」开关、导入层像素尺寸与画布不一致时蒙版坐标按画布尺度解释
（ABI 没有像素尺寸读口；装不下时由 stride / 行数闸明确拒绝）。

### 6.53 仿制图章补上不透明度与对齐（2026-10-21，Claude）

**两个写死的字段。** §6.49 接线仿制时为了先跑通，`clone.opacity` 写死 1.0、
`aligned` 写死 1——引擎的 `comp_tool_clone` 里两个字段本来都有，缺的只是壳侧入口。
本项把选项栏那两项能力补齐，**零新增导出**。

**不透明度。** `Alt+[ / ]`，10% 一步、下限 10%（引擎允许 0，但 0 = 什么都不画，
对外壳用户是无反馈的死调节），与历史记录画笔、修补同一口径。值跨文档保留，
每笔提交前夹取后写进 `desc.clone.opacity`。

**对齐。** `Shift+S`：PS 把它放在选项栏，本壳没有选项栏，用同一字母的 Shift 组合
最省记忆，不带 Shift 的 `S` 仍是选工具。开 = 首笔成功后壳存下
`clone_stroke_offset` 的整数偏移、后续每笔 `has_offset = 1` 复用；关 = 每笔都不带
`has_offset`，引擎按 `round(source - 首笔落点)` 现算，即每笔都以源点重新对位。

**关掉时必须清偏移。** `aligned = 0` 时引擎不读 `has_offset`，不清也能跑；但重新
打开后会复用关掉之前的旧偏移，症状是「开关了一轮，落笔突然跳到旧位置」。所以
`Shift+S` 关掉对齐的同一次按键里就 `clone_has_offset = false`，重新打开后的下一笔
由现算重新建立。

**反馈。** 工具选中时状态栏多出「Shift+S 切对齐（开/关），Alt+[ / ] 调不透明度（N%）」；
切换对齐、调不透明度各有独立提示；落笔文案补「对齐/不对齐 · 不透明度 N%」，
便于目视核对参数确实进了 ABI。

**验证。** 纯外壳改动，无新增单测。单测 **589 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.26（并回填 §5.22），`docs/manual-test.md` M26 补步 13–17。
遗留：`sample_all_layers` 当时没有持久勾选态（只能 Shift+Alt+点击指定），已在 §6.58 补齐。

### 6.54 缩放工具补齐：拖框 / 双击 100% / 放大镜光标（2026-10-21，Claude）

**把 §6.50 的三条遗留一次做完。** §6.50 接线时只做了「点击放大 / Alt+点击缩小」，
其余三项记在遗留里：① 拖出矩形缩放到该区域、② 双击画布 = 实际像素、
③ 放大镜 +/- 光标。三项都在壳侧，**零新增 ABI 导出**。

**1. 拖框缩放（`zoom_to_doc_rect`）。** 新函数进 `ui_core::CanvasView`，与 `fit`
并列：矩形按 `margin = 0.98` 铺满画布、矩形中心移到画布中心。`fit` 关心整幅文档，
`zoom_to_doc_rect` 只关心这一块，所以直接用 `pan = -(rect_center - doc_center) * zoom`
而不是复用 `zoom_at`（后者保证锚点不动，而这里要的恰恰是「把中心搬过来」）。
两点先归一化，宽或高不为正返回 `false`；缩放仍走 `clamp_zoom`，夹取后照样居中矩形中心。

**2. 按下只记起点，松手才判定。** `zoom_marquee_begin` 只存**屏幕坐标**并抓鼠标
（拖框期间视图不变，屏幕矩形就是用户看到的矩形，预览不必每帧反算文档坐标）。
松手时位移 `< 4 px` 视为单击，回落到原来的 `zoom_tool_click`；否则两角
`screen_to_doc` 后交给 `zoom_to_doc_rect`。单击 / 拖框共用一个入口，
`Alt+点击缩小` 的语义完全不受影响。预览是白实线 + 黑虚线双描边，与选区预览同一套；
`WM_CAPTURECHANGED / WM_KILLFOCUS` 里清掉 `zoom_marquee_active`，不留悬挂矩形。

**3. 双击 = 100%。** `WM_LBUTTONDBLCLK` 已存在（直方图复位、路径转选区、图层改名都挂在
它上面）。缩放工具的分支放在最后「还原成普通按下」之前：只调 `actual_pixels()` 并写
「缩放 100%（实际像素）」，否则双击会被拆成两次 1.2 倍放大。第一次点击仍会先放大
1.2 倍再被双击复位，净结果是 100%；这是消息顺序决定的，不影响结果。

**4. 放大镜光标。** 系统光标表里没有放大镜 +/-，用 32×32 **单色光标**自绘：
AND/XOR 两个位平面（`AND=1,XOR=0` 透明、`AND=0,XOR=0` 黑、`AND=0,XOR=1` 反色白），
按解析几何逐像素判定圆环（黑边 + 白环、玻璃透明）、手柄线段和玻璃内的横/竖线——
缩小只有横线，放大是十字。32 px 宽正好 4 字节/行，不需要行尾补齐。
`WM_SETCURSOR` 只在「缩放工具 + 光标在画布内 + 不在平移」时接管，
`Alt` 按下换成缩小形态；平移中保持抓手，其它区域/工具继续交给 `DefWindowProc`。
`CreateCursor` 失败时 `zoom_cursor_handle` 返回 `nullptr`，调用方回落到默认箭头，
不会因为光标把工具弄坏。光标按「放大 / 缩小」各建一次并缓存到进程结束。

**验证。** 新增 1 个单测（`ui_core_zoom_to_rect_fills_the_view_and_centers_the_rect`：
铺满比例、居中、反序拖框等价、超大/超小框夹取、退化输入不改视图），单测
**590 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.27（并回填 §5.23），`docs/manual-test.md` M12 补步 9–13。
遗留：④ 拖动过程中连续缩放（上游是点按式，本壳同为松手结算）；
拖框不支持拖出画布边缘自动平移；放大镜是单色反色光标，不跟随系统深色主题。

### 6.55 修补工具按当前选区裁剪（2026-10-21，Claude）

**补掉 §6.52 的唯一遗留：有选区时修补仍涂满整幅画布。** 其它工具（画笔、
橡皮、仿制图章）都走 `comp_tool_apply`，引擎在落笔路径上统一叠选区闸；
修补不一样——外壳自己光栅化 8 位 coverage 再交给 `COMP_TOOL_HEAL`，
引擎拿到的已经是成品蒙版（`engine/domain/tool_ops.cpp` 里 `spot_heal` 直接写
图层像素，没有任何选区参数），所以**这个闸只能由壳侧补**。

**实现。** 新增纯函数 `comp_ui::intersect_coverage_with_selection(coverage, mask, count)`，
逐像素 `(coverage * mask + 127) / 255`：`255*255` 仍是 `255`（全保留不变形），
`±127` 做四舍五入而不是截断，半透明边缘不会因为叠一次就整体变暗。
`heal_stroke_end` 在光栅化之后、调 ABI 之前，若 `selection_active` 且蒙版非空就叠一次。
蒙版本来就是文档分辨率（与 coverage 同坐标系），尺寸对不上属于不该出现的状态，
这时**如实报「选区未裁剪（蒙版尺寸不一致）」而不是假装裁过**——宁可提示，也不静默
越界涂。结果以 `selection_note` 后缀并进两条成功状态串，避免中途 `set_status`
被后面的成功文案覆盖。

**验证。** 新增 1 个单测（`ui_core_coverage_intersects_selection_mask`：255/128/0 三档、
`200×64 → 50` 的取整、零 coverage 恒零、全掩版恒等、空指针返回 false 且不写字节），
单测 **591 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.28（并回填 §5.25），`docs/manual-test.md` M28 补步 12–13。
遗留：选区只在落笔结算时生效（`spot_heal` 内部按补丁扩散采样，选区外像素不参与
作为**源**，只被排除在**目标**之外），与 PS「选区同时限制采样」仍有差别。

### 6.56 Move：Alt 拖动 = 复制图层再移动（2026-10-21，Claude）

**补掉 §5.24 遗留 ①：`Alt` 拖动应复制一份图层再移动。** 有选区时的像素搬移早就支持
`Alt = 复制`（`move_duplicate` 走 `comp_selection_move_pixels`）；这次补的是**无选区**
的整层拖动那条路。零新增 ABI 导出：`comp_layer_duplicate` 本来就在。

**1. 复制推迟到首次位移。** 按下时只记 `layer_move_alt`，真正 `comp_layer_duplicate`
放在 `layer_move_extend` 里第一次算出非零位移的那一帧。理由：只点不拖不该在画布上
凭空多出一层（PS 亦然），而且此时 abort 事务就能干净收场，不需要「先建再删」。
Alt 在按下时取样一次，拖动途中松开 `Alt` 不改变这一拖的语义——与画笔的 Shift 同规矩。

**2. 一步撤销。** 复制发生在 `comp_txn_begin` 之后：`comp_layer_duplicate` 内部的
`ImplicitTxn` 会**加入**已开启的显式事务（`owns_ = !document->txn_active()`），
所以松手 `commit` 后 `Ctrl+Z` 一次同时撤掉「多出来的图层」和「这一次位移」。
事务名按 Alt 分成 `Duplicate & Move Layer` / `Move Layer`，历史面板一眼能分。

**3. 基准与选中项都切到副本。** 复制后用 `comp_layer_get_transform` **回读副本自己的**
transform 作为拖动基准（副本若被引擎偏移过，起点不会跳）；`refresh_layers` 重建面板后
`panel_select_layer_id` 把选中项挪到副本，接着拖、接着按键都作用在副本上。
复制失败时 `layer_move_alt = false`，本拖退回「移动原图层」——不会假装复制过。

**4. 清理。** `layer_move_end` / `move_cancel` 都清两个新标志；`!dirty` 分支若已复制过
（复制成功但写 transform 失败），先 `refresh_layers` 再 abort，避免面板留着已回滚的 id。
工具选中文案补「无选区 = 拖整层、Alt+拖动 = 复制一份再移动」。

**验证。** 纯外壳改动，无新增单测。单测 **591 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.29（并回填 §5.24），`docs/manual-test.md` M27 补步 12–15。
遗留：拖出画布的位置提示已在 §6.59 补齐；旋转/缩放手柄仍按 §5.24 的清单排队。

### 6.57 Move：无修饰方向键微调 1 px（2026-10-21，Claude）

**补掉 §5.24 遗留 ②。** S21 早就接了 `Shift+方向 = 1 px` / `Ctrl+方向 = 10 px`，
但上游 V 的语义是**无修饰**方向键 = 1 px、Shift = 10 px。壳里 `↑/↓` 无修饰是「选图层」
（S21 刻意保留的基本操作），所以缺的那一块具体是：**Move 工具下无修饰方向键没有动作**
（`←/→` 本来就走空分支，`↑/↓` 被选图层占着）。

**1. 只在 Move 工具下接管。** 无修饰方向键在 `tool == Move` 时改为
`panel_move_layer(±1, 0 / 0, ±1)`；其它工具完全不变（`←/→` 仍无动作、`↑/↓` 仍选图层）。
不全局接管的原因：「`↑/↓` 换图层」是壳里唯一不用鼠标的换层方式，为 1 px 微调全局抢走
它，代价大于收益；而在 Move 工具下用户本来就在摆图层，与上游一致。
`Shift` 仍是 1 px、`Ctrl` 仍是 10 px——没有跟着上游把 Shift 改成 10，是因为这两个步长
在全部工具下共用一套口径，为单个工具特例化会让「Shift = 1 px」这条肌肉记忆失效。

**2. 组图层如实拒绝。** `panel_move_layer` 新增组判断：组节点不读自己的 transform
（`filter_graph` 只烘焙成员），此前菜单项与 Shift+方向打上去是「状态栏说成功、画面没动」。
现在直接回「组图层不参与 transform 合成，暂不支持整组移动」，与 Alt 拖动同一措辞。
状态栏文案同时从「图层位置：(x, y) 方向键」改为「（方向键微调）」，不再读成按键提示。
（后续：§6.61 起改为平移组成员，与拖动路径共用同一份成员快照逻辑。）

**验证。** 纯外壳改动，无新增单测。单测 **591 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.30（并回填 §5.24），`docs/manual-test.md` M27 补步 16–17。
遗留：连续按方向键每按一次 = 一步撤销（上游把连续微调合成一步），要合并得做按键节流。

### 6.58 仿制图章：取样范围做成持久开关（2026-10-21，Claude）

**补掉 §5.26 遗留 ⑦（与 README §6.53 的遗留同一项）。** `sample_all_layers` 此前只有
一条入口：`Shift+Alt+点击` 设源点的**同时**把取样口径定成「整篇合成」，松手即忘；
`clone_pick_source` 更是无条件 `= shift_held`，等于每换一次源点就把选项清回「当前图层」。
上游把它做成选项栏里的持久勾选（Sample: Current Layer / All Layers），壳里没有选项栏，
所以缺的是两个东西：**能随时切换的键位** + **换源点不会把它冲掉**。

**1. 键位选 `Shift+K`。** 沿用 `Shift+S` 切对齐的「Shift + 工具字母 = 工具选项」记法。
不选 `Shift+A`（Sample All 首字母）是因为 `a` 是 Select 工具的键位，在仿制工具下拦截
`Shift+A` 会把「按 A 回选择工具」这条基本操作抢走；`k` 不在工具表里，任何工具下都不冲突。
`Alt+字母` 另有问题：组合键以 `WM_SYSKEYDOWN` 到达，`WM_KEYDOWN` 分支收不到。

**2. 持久化边界。** 状态本来就存在壳里（`ShellState::clone_sample_all`），所以这一项
**不需要新字段、不需要新 ABI**，只改两处语义：① 新增 `clone_toggle_sample_all`，
`Shift+K` 翻转并回一条带方向提示的状态栏；② `clone_pick_source` 从「赋值」改成
「按住 Shift 才置真」——`Shift+Alt+点击` 仍可直接把选项切成整篇合成并保持，但不带
Shift 的换源点从此保留选项，不再把用户的勾选清掉。换文档时 `clone_reset_source` 仍连同源点
一起复位：源点属于旧文档的像素网格，取样口径跟着源点走最不容易让人误解。

**3. 读取点不变。** 真正读它的是 `clone_stroke_end` 里的
`desc.clone.sample_all_layers = shell.clone_sample_all ? 1 : 0`，因此切换对**下一笔**
立刻生效；已落下的笔画不受影响（desc 在松手时一次性提交）。工具栏「源 x,y 图层/合并」
与状态栏文案都是每帧现读，不用额外刷新。

**验证。** 纯外壳改动，无新增单测。单测 **591 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.31（并回填 §5.22 的遗留 ③、§5.26 的遗留 ⑦），
`docs/manual-test.md` M26 补步 18–21。
遗留：选项只在本次运行内保留，退出重开回到「当前图层」（上游会把选项存进偏好设置）。

### 6.59 Move：拖出画布时给出位置提示（2026-10-21，Claude）

**补掉 §5.24 遗留 ③。** 图层被拖到完全离开画布后，画面上一点痕迹都不剩，
状态栏却照旧只写 `(x, y)` 与新位移——用户很容易以为「图层被拖丢了」（其实还在面板里，
拖回来或 `Ctrl+Z` 都能恢复）。这一项补的就是这句话，不改变任何几何。

**1. 判定放进 `ui_core.h`。** 新增纯函数
`layer_rect_touches_canvas(x, y, w, h, rotation_deg, doc_w, doc_h)`。先把矩形按旋转角取
AABB（绕中心，`|cos|` / `|sin|` 展开），再与 `[0, doc_w) × [0, doc_h)` 求交；
`transform.x/y` 是左上角（与 `heal_layer_point` 同口径），所以中心 = 左上 + 半宽高。

**2. 宁可少报，不误报。** AABB 是保守上界：旋转 45° 的大图 AABB 明明压着画布时，
实际可能已经空了一半——这种时候**不提示**。反向的代价就大了：误报「已拖出画布」
会让人以为图层丢了。宽高不为正、画布不为正、角度是 NaN/inf 时一律返回 `true`
（「判断不了 = 当它还在」），这四种输入都有单测钉住。

**3. 三个调用点，文案统一。** ① `layer_move_extend` 每条移动提示后追加 ` · 已拖出画布`；
② `layer_move_end` 的松手提示追加 ` · 已拖出画布（图层仍在面板里，可拖回或 Ctrl+Z）`
——拖拽中那条会被松手那条覆盖，两处都得有；③ 方向键微调走的 `panel_move_layer`
也追加 ` · 已拖出画布`（微调同样能把图层提出画布）。每帧重算、无新增状态：
拖回来提示自动消失。

**验证。** 新增 UI 层 1 例（画布内 / 四方向拖出 / 贴边 / 45° 与 90° 旋转 /
宽高为 0 / 画布为 0 / NaN 角度）：单测 **592 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.32（并回填 §5.24 的遗留 ③、§5.29 的遗留），
`docs/manual-test.md` M27 补步 18–20。
遗留：提示只写状态栏，画布上不做任何标记；§5.24 清单里剩下的是旋转/缩放手柄
（`transform` 的尺寸与角度仍只能由 Ctrl+方向、Alt+[ / ] 改）与组图层整体移动。

### 6.60 面板宽度可拖：左右分界线（2026-10-21，Claude）

**补掉 §5.11 里「面板缩放 / 停靠」的宽度一半。** 布局原先写死：图层面板恒 248 px、
右侧面板恒 330 px，层名一长就截断，滑杆挤在固定宽度里，用户除了改窗口尺寸没有别的办法。

**1. 偏好走 `LayoutPrefs`，`compute_layout` 加一个重载。** `compute_layout(w, h, prefs)`
按偏好宽度排版；两参数版本转调 `LayoutPrefs{}`，缺省排版与旧版完全一致——既有的
`ui_core_layout_never_overlaps_at_any_size` 一行没改仍然通过。范围由纯函数夹取：
图层面板 168–520 px、右侧面板 240–560 px。

**2. 收起口径不变，只把它作用在偏好宽度上。** 面板仍在
`w >= clamp(pref) + kMinCanvasWidth` 时显示，右侧面板看的是图层面板排完后的剩余宽度；
三块依旧精确铺满客户区。不做「挤成 120 px 的面板」：挤窄的面板比没有面板更难用。

**3. 分界线在面板内侧 5 px，不吃画布像素。** `Layout` 新增 `layer_splitter`（图层面板最右
5 px）与 `adjust_splitter`（右侧面板最左 5 px），`panel_splitter_at` 返回 0/1/2。
画布边缘是落笔最频繁的位置之一，把那里的像素拿去做缩放会让「画到边上」变成「面板变宽」，
这类误触比点不准行难查得多。代价仅是面板最后/最前 5 px 不用于内容。

**4. 拖动只算总位移。** `*_panel_width_from_drag(base, dx) = clamp(base ± lround(dx))`，
夹取只作用在**结果**上：拖到边界后立刻反向不会有死区，来回拖回原点精确回到原宽度。
宽度一变画布中心就移动，这里把 `中心前 - 中心后` 补进 `pan`，画面里的文档保持不动——
用户拖的是面板，不该顺带把正在看的图推走。宽度偏好是布局状态，**不进撤销栈**。

**5. 手感对齐既有拖动。** 松手提交并给出 `图层面板宽度：288 px（双击分界线复位）`；
`Esc` 或捕获被抢 = 回到按下前宽度（先清状态再 `ReleaseCapture`，重入的
`WM_CAPTURECHANGED` 因此是空操作）；双击分界线复位 248 / 330 px；悬停与拖动时 5 px 带
提亮、光标变 `IDC_SIZEWE`，常态只画 1 px 暗线——不给高亮用户看不出这里能拖。
面板因窗口太窄被收起时，状态栏如实追加`（当前窗口放不下，已收起；放宽窗口后生效）`。

**验证。** 新增 UI 层 3 例（偏好排版铺满/收起口径、夹取与拖动换算的边界与取整、
分界线命中含收起态）：单测 **595 例 / 0 失败**；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.33（并改写 §5.11 的「面板缩放 / 停靠」行），
`docs/manual-test.md` M29。
遗留：停靠（拖出成浮窗、面板合并、整块收起/展开）未做；宽度偏好只在会话内保存，
不写进任何设置文件。

### 6.61 整组移动：拖动与方向键搬整个组（2026-10-21，Claude）

**补掉 §5.24 遗留 ⑥「组图层不能整组平移」。** 根因在合成口径：`filter_graph` 只烘焙
组成员，**组节点不读自己的 transform**。因此过去对组做移动有两种错误表现——方向键直接
拒绝，Alt/普通拖动却把位移写进组自己的 transform，状态栏数字在变、画面一动不动。

**1. 整组移动 = 平移子树里的每个成员。** 按下时快照成员的 `id / transform / rotation`，
之后每帧写「基准 + 总位移」——与单层同一写法，不累加、不按帧取整，来回拖回原点精确复原。

**2. 父子关系沿 `parent` 链走，不看数组相邻。** 面板是一张扁平列表，数组里挨着的两层
没有父子含义；`ui_core::layer_in_subtree(layers, count, node, root)` 从 node 一层层向上找，
不匹配 root 就不算成员。链最多走 `count` 步：数据有环（自引用/互引用）时返回 false ——
宁可少移一层，也绝不死循环。嵌套组天然覆盖，因为成员判断看的是整条祖先链。

**3. 空组如实说「无位移」。** 不能只按「成员表是否为空」区分单层/整组：空组也会走整组
路径，掉进单层分支就又会写出那个无效果的 transform。所以另立 `layer_move_is_group`，
空组拖动/方向键都只给提示，不写文档。

**4. 一次撤销。** 拖动时所有成员写进同一个显式事务（`Move Layer Group` /
`Duplicate & Move Layer Group`），松手 `comp_txn_commit` 算一步；方向键每次按键开一个
`Nudge Layer Group` 事务并立即提交，撤销一步回到按键之前。写失败数按成功条数统计，
全失败则 abort，不会留下半组位移。

**5. Alt 拖动整组 = 复制再移动，副本要重建成员快照。** 深拷贝后所有成员 id 都是新的，
按下时的清单全部失效；不重建就会接着写**原组**的成员——画面上原组在动、副本不动，
看起来像「复制了但没复制」。复制成功后按副本 id 重新扫描一次 `is_group`，再快照子树。

**6. 提示口径一致。** 拖动中显示位移与成员数，松手后显示「已移动图层组：位移 (dx, dy)，
N 个成员（一步撤销）」；`members_off_canvas` 要求**所有**成员都离开画布才追加
「已拖出画布」——保守口径，误报会让人以为图层丢了。

**验证。** 新增 UI 层 1 例 `ui_core_layer_in_subtree`（直接子层/孙层、兄弟与反向查询、
表外 id、自环与互环、null/非正 count/root=0）：单测 **596 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.34，`docs/manual-test.md` M30。
遗留：组的混合模式/不透明度仍按合成管线生效，不受移动影响；组自身 transform 依旧不被
烘焙——这是引擎口径，壳只按成员搬。

### 6.62 滤镜实时预览：调参即出帧，文档不动（2026-10-21，Claude）

**补掉 §5.11 遗留「滤镜实时预览」。** 过去滤镜面板只能「改完参数回车应用一次」，看不到
效果就只能靠撤销来回试。现在 `-` / `=` 步进、键入值提交、参数复位、以及 Progressive Blur
定完渐变轴，都会立刻在画面上出一帧预览。

**1. 预览帧只活在 framebuffer 里。** 引擎的 `comp_filter_apply` 会加入宿主已开的事务
（`const bool owns_txn = !document->txn_active()`，engine/domain/filters.cpp:675），所以
预览固定走四步：`comp_txn_begin` → 应用一次 → `render_canvas` 取帧 → `comp_txn_abort`
回滚像素。文档始终停在「改参数之前」，预览只存在于画面里。

**2. 为什么不学色彩范围面板那样长期挂着事务。** 那条路回滚更直接，但代价是每个出口都得
记得 abort：关面板、换面板、新建/打开文档、切文字工具……漏一处就把别的命令的写入卷进
同一个撤销步，甚至 abort 到已经销毁的文档上（`forget_color_range` 的注释记的就是这个坑）。
framebuffer 方案最坏情况是「画面停在一张旧预览」，数据永远不脏，重画一帧就回到真状态。

**3. 回车仍是真应用，且结果与预览逐字节一致。** 回车走原来的 `comp_filter_apply` 且不开
宿主事务（引擎自开 `Filter: <name>`），一步撤销。同一组参数因此算两遍——明知故犯，换的是
「预览帧 == 提交结果」这条硬保证。已在 `%TEMP%` 用 ctypes 对真实 DLL 验证：宿主事务里
应用后 abort，像素逐字节复原、撤销栈不动；随后真应用的结果与预览帧逐字节相同，且只多
一步撤销。

**4. 恒等与失败都如实说，并且一定作废旧预览。** 半径/强度为 0 时引擎回 `changed == 0`
（连事务都不开），状态行写「参数为恒等，无预览」；`COMP_ERR_STATE`（组图层 / 无像素 /
HDR）写「预览失败，像素未改动」。两种情况都置 `needs_render`——参数已经变了，留在屏上的
那一帧是错的，必须换回真画面。

**5. `filter_preview_pending` 只表示一件事：framebuffer 里这一帧是预览。** 置真点唯一
（预览出帧成功）；清零点主口径唯一（`present()` 的重画分支——任何 `needs_render` 的重画
都说明预览被顶掉了）。其余出口（关面板、换面板、新建/打开文档、切文字工具）顺手清一次，
只是让状态行能说出「预览已丢弃，图层未改动」。

**6. 面板是模态的：画布点击不再落到工具上。** 预览不在文档里，这时让画笔落笔等于在「看不
见的那份文档」上画，预览还会被重画冲掉。因此滤镜面板打开时画布点击被吞掉并给出提示（与
色彩范围同规矩），唯一例外是 Progressive Blur 的轴拖拽。顺带修掉一个老问题：以前开着滤镜
面板仍能用画笔在画布上画。缩放不受影响（滚轮/Ctrl 缩放只改视图，预览帧照样拉伸显示）。

**验证。** 引擎未改（PE 导出 **138 不变**），单测 **596 例 / 0 失败**；壳自检
**245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.35，
`docs/manual-test.md` M31。
遗留：预览按全分辨率算（scale 恒 1），大文档上等于每按一次就整层算一遍；真正的增量预览
缓存要等「图层快照 / 局部重算」这类 ABI，壳侧不自造。

### 6.63 Move：有选区时「落点在哪」决定搬像素还是搬整层（2026-10-21，Claude）

**补掉 §5.24 遗留 ⑤。** 以前只要 `selection_active` 为真，Move 拖动一律搬选区像素；
上游 V 的规则是看**落点**：点在选区内 = 搬选区像素，点在选区外 = 搬整层（选区原地不动）。
本项把这条分流补上，零新增 ABI 导出。

**1. 判定是纯函数，放 `ui_core`。** `comp_ui::selection_contains_point(mask, w, h, stride,
x, y, threshold)`：文档坐标取 floor 后读 8 位 coverage，`>= threshold` 算「里面」，与蚂蚁线
同一阈值 128。越界与 NaN 一律算「外面」（写成 `!(x >= 0)`，NaN 才不会漏过去）；阈值 0/负值
按 1、>255 按 255。`stride` 由调用方传，蒙版带填充也不会读错行。

**2. 分流点只有一个：`move_begin`。** 落点在选区内走原路径（`comp_selection_move_pixels`，
松手一次性提交，蚂蚁线跟着走）；落在选区外改走 `layer_move_begin`（只写 transform，
像素一个字节都不搬）。两条路互斥，不会出现「像素和图层一起动」。**蒙版读不到时（
`selection_mask` 为空）不猜**：退回旧的搬像素行为，因为这时「内/外」根本没有依据。

**3. 整层拖动不动选区蒙版。** 蚂蚁线留在原地，与上游一致——选区描述的是画面上的位置，
不跟着图层走。状态行会补一句 ` · 落点在选区外：移动整层，选区不动`，避免用户以为搬错了。
选区外按住 Alt 拖动 = 复制整层再移动（`Duplicate & Move Layer`，一步撤销），与无选区时同一套。

**4. 边界如实说明。** 只有落在**画布矩形内**的按下才会进入 `move_begin`（更早的分支就用
`layout.canvas.contains` 守卫了），所以「选区外」指画布内、选区轮廓外。画布外的空白处按下
本来就不是拖动起点，本项不改这个口径。

**验证。** 新增 1 个单测（`ui_core_selection_contains_point`：实心区 / 羽化边 128 与 127 /
floor 取整 / 阈值夹取 / 四方向越界 / NaN / 空指针与非正尺寸），单测 **597 例 / 0 失败**；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.36，`docs/manual-test.md` M32。

### 6.64 多选批量移动：拖动与方向键一次搬整批（2026-10-21，Claude）

**补掉 §5.12 遗留 5。** 面板早就支持 Ctrl 点选多个图层，但拖动只作用于**主选中层**，
其余选中项原地不动——选中三行、拖一下只动一行，是明确的假象。本项让整批一起走，
零新增 ABI 导出。

**1. 选中集合先展开成「真正要写 transform 的成员」。** 新增 `build_move_targets`：
用已有的 `panel_selection_ids(shell, true)` 取批量集合（会自动丢掉被**选中的祖先组**
覆盖的子层，避免同一像素被写两次），再对每个顶层项分流——组用 `collect_group_members`
展开成子树成员（组自身 transform 不参与合成，写了等于没写），普通层只取自己。
两种来源最后是同一个 `GroupMemberMove` 快照格式，后续只有一套写入循环。

**2. 拖动：按下快照，每帧重写「基准 + 总位移」。** 选中集合多于 1 个顶层图层时，
`layer_move_begin` 把 `multi` 与成员快照一起记进状态，并开**一个**事务
（`Move Layers` / Alt 时 `Duplicate & Move Layers`）；`layer_move_extend` 逐成员写
`base + (dx, dy)`，不累加，来回拖不漂移，松手 commit = **一步撤销**。组单选仍走 §6.61
的整组分支与文案，`multi` 与 `is_group` 互斥，不会互相抢。全部选中项都是空组时不开始
事务，直接 `移动多个图层：选中的图层没有可移动的成员，无位移`，不在撤销栈里留空格。

**3. Alt 拖动 = 整批复制的原子操作。** 首次产生位移时逐个 `comp_layer_duplicate`
（组是深拷贝整棵子树），再把**副本集合**设为选中集合、按副本重建成员快照——副本成员 id
全变了，沿用旧快照会去写原图层，画面看起来像「复制了但没复制」。任一副本失败就整批作废：
不写任何 transform，松手 abort 把已建的副本一起回滚，状态栏
`复制多个图层失败：已回滚，位置未变化`，不留半个副本。

**4. 方向键走同一条路。** `panel_move_layer` 在 `extra_selection` 非空时也批量平移
（事务 `Nudge Layers`，一次按键一步撤销），文案 `图层：位移 (dx, dy)，N 个成员（方向键微调）`，
拖出画布时补 ` · 已拖出画布`（复用既有的 `members_off_canvas`）。Move 工具帮助也加了一句
`面板多选 = 整批移动（Alt = 整批复制）`，免得只能靠试出来。

**验证。** 单测 **597 例 / 0 失败**（本项只改壳层，无新增纯函数）；PE 导出 **138 不变**；
壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.37，`docs/manual-test.md` M33。

### 6.65 多选批量开关图层效果（2026-10-21，Claude）

**补掉 §5.13 遗留 ③。** 效果菜单只作用于主选中层：Ctrl 点选三层再点「投影」，只有一行动。
本项让整批一起切换，零新增 ABI 导出。

**1. 判据取主选中层，动作整批统一。** 菜单勾选本来就是看主选中层的：它有效果就整批移除，
没有就整批添加。每层用 `comp_layer_effect_count` + `effect_at` **自己查自己**（新增
`layer_has_effect`），不复用壳里缓存的 `shell.effect_kinds`——那是主选中层的副本，
拿去判别的层会给出错误答案。已在目标状态的层不写、不占撤销步。

**2. 一个事务 = 一步撤销。** `Add Effect To Layers` / `Remove Effect From Layers` 包住整批
`set_effect` / `remove_effect`；一个都没改成时 abort，撤销栈不留空格。状态栏
`已为 N 个图层添加效果：<名>`，必要时追加 `（K 个已在目标状态）`、` · M 个失败`。

**3. 组图层跳过并说明原因。** `filter_graph` 的 `kGroup` 分支只合成子树，从不读组自己的
`effects`——写进去画面一个像素都不会变。批量时逐层跳过并补
` · 跳过 N 个组图层（引擎不按组求值效果）`；单选组时直接拒绝
`效果：组图层不参与效果求值（请选中组内成员）`，顺手修掉单选路径上原有的静默无操作
（与组 transform 同一类假象，见 §6.61）。

**4. 边界。** 批量只做**开关**：参数面板仍只编辑主选中层，批量改参数未做；添加后不自动
打开参数面板（多选时打开会让人以为面板管整批）。

**验证。** 单测 **597 例 / 0 失败**（本项无新增纯函数）；PE 导出 **138 不变**；壳自检
**245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.38，`docs/manual-test.md` M34。

### 6.66 变换手柄：拖动缩放 / 旋转（2026-10-21，Claude）

**补掉 §5.24 遗留 ④。** transform 的尺寸与角度此前只能靠 `Alt+[ ]` / `Ctrl+左右`
改，画布上没有把手。现在 Move 工具下选中**一个有显式变换的普通图层**，画布上会出现
蓝色框、8 个白底方块和上方旋钮：拖方块改尺寸、拖旋钮改角度，零新增 ABI 导出。

**1. 画之前先问「算得准吗」。** `transform_frame_for()` 要求：Move 工具、无裁剪/框选
/套索、面板单选非组图层、无像素选区、`w > 0 && h > 0`。最后一条是能力边界：ABI 没有
像素尺寸回读口，按像素尺寸的图层算不出真实边界，**画一个猜的框比不画更糟**（与
`Alt+[ ]` 拒绝同一理由）。多选只给批量移动（§5.37），不提供整批缩放。

**2. 几何放进 `ui_core.h`，画与点同一套函数。** 手柄绕**矩形中心**旋转（引擎先归中心
再逆旋转）；命中半径 7 px、旋钮偏移 24 px 都是屏幕像素，除以 zoom 换成文档像素再交给
纯函数——画在哪就点在哪。新增 1 个单测：8 个手柄位置、旋转后命中、90° 锚点不漂移、
下限钳制、角度归一化。

**3. 缩放保持对角锚点不动。** 指针逆旋转回局部系 → 改对应边 → 钳到 ≥1 px → 把原点
回移 `anchor_doc - R(θ)·anchor_local_new`。旋转过的图层中心会移动，只改 x/y 会把图层
「顺带」推走。每帧从按下时的 base 重算，来回拖不漂移。

**4. 一次拖动 = 一步撤销。** `Scale Layer` / `Rotate Layer` 各开一个事务，途中写
`comp_layer_set_transform`，松手 commit；没改到就 abort，不留空撤销步。Shift 吸附
15°。事务开不起来时也吃掉这次点击，绝不退化成整层拖动。

**5. 边界。** 光标不随位置变形（手柄只靠图形提示）；多选手柄、Alt 中心缩放、
Shift 等比、吸附画布边缘均未做。

**验证。** 单测 **598 例 / 0 失败**；PE 导出 **138 不变**；壳自检 **245 项不变**
（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.39，`docs/manual-test.md` M35。

### 6.67 工具图标：GDI 现画，零资源文件（2026-10-21，Claude）

**补掉 §5.11 的「工具按钮图标」。** 工具栏 20 个按钮原来画快捷键字母，得记住键位才
知道拿的是什么；现在每个按钮是自己的图形。

**1. 不加资源文件。** `.rc` + `.ico` 要改构建脚本，字体符号依赖目标机器装了什么字体，
两者都会破坏「单个 exe 拷走就能跑」。图标改用 GDI 图元现画：`ToolIconMap` 以 32×32
逻辑网格为坐标，`scale = 内框边长 / 32`，所有坐标只写逻辑值；笔宽 `max(1, lround(2·scale))`，
框选用 1 px `PS_DOT`。窗口尺寸变化时图标跟着缩放，构建产物不新增文件。

**2. 形状与空心/实心。** 箭头光标、四向箭头、虚线框、套索环、钢笔尖、魔棒+星、裁切
双 L、滴管、笔刷、橡皮、创可贴、印章、水滴、太阳、渐变方框+对角半实心、方块+圆、
`T`、手掌、放大镜、时钟。空心形状先换 `NULL_BRUSH` 再换回，不与实心图元抢状态。

**3. 颜色与键位。** 图标与按钮文字同色：选中纯白、未选中浅灰，选中态仍是蓝底。
按钮上的字母没了，但状态栏一直是 `工具名（V）`，键位仍可发现。

**4. 边界。** 只有静态图标：没有 hover/禁用态（高分屏不受影响，矢量绘制本身就与
分辨率无关）；图标是壳侧自绘，不宣称与上游逐像素一致。

**验证。** 单测 **598 例 / 0 失败**（本项只有绘制代码）；PE 导出 **138 不变**；壳自检
**245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.40，`docs/manual-test.md` M36。

### 6.68 菜单项按前置条件置灰（2026-10-21，Claude）

**补掉 §5.15 边界 ④「菜单项暂未做置灰」。** 以前无选区点「取消选择」、单层点「向下合并」、
顶层点「上移一层」都能点，点完才在状态栏回一句不行。现在入口本身就说得清哪些项此刻没意义。

**1. 规则在纯函数里，壳只搬运。** `comp_ui::LayerMenuState` 扩出前置条件字段，
`comp_ui::layer_menu_enabled` 折成逐项可用态。`EnableMenuItem` 要真窗口才能验证，
所以单测覆盖规则表而不是调用点——这也是把判定抽净的理由。

**2. 前置条件读实况，复用执行路径。** 撤销/重做问 `comp_document_can_undo/can_redo`；
向下合并直接问 `comp_layer_can_merge_down`（ABI 注释里写的就是「供外壳置灰菜单」）；
剪贴与重排复用执行时的同一对纯函数（`plan_clipping_base`、`plan_layer_reorder`），
不会出现「菜单说能做、点了说不行」。拼合/上下选层要 ≥2 层，清除效果要已有效果，
组的效果项默认灰、有多选集合时放开。

**3. 只置灰「会拒绝或纯空转」的项。** 不按「有没有选中层」一刀切，而是照各处理函数的
真实前提逐条对，所以删除/复制/改名/变换/混合模式/采样这些有层就能做的项不会误灰。

**4. 键盘路径刻意不动。** 置灰只管菜单；同一命令的快捷键仍进 `run_shell_command`，
保留各处理函数原有的拒绝提示。键盘用户不该因为菜单变灰就失去「为什么不行」。

**5. 边界。** 工具栏按钮没有禁用态（自绘按钮，留待与 hover/按下态一起做）；
不细化到剪贴态的每一项；状态以 `WM_INITMENU` 时刻为准。

**验证。** 单测 **599 例 / 0 失败**（新增 `ui_core_layer_menu_enabled_follows_preconditions`）；
PE 导出 **138 不变**；壳自检 **245 项不变**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
真实窗口用 `WM_INITMENU` 后逐项读 `GetMenuState` 验证过三种状态的置灰矩阵。
文档：`docs/shell-parity.md` §5.41，`docs/manual-test.md` M37。

### 6.69 连续方向键微调合并成一步撤销（2026-10-21，Claude）

**补掉 §5.30 的遗留：按住方向键会把历史面板刷满。** 引擎里「一个事务 = 一条撤销记录」
（`comp_txn_begin` 取快照、`comp_txn_commit` 才入栈），而键盘自动重复是「一串
`WM_KEYDOWN` + 一个 `WM_KEYUP`」——原来每按一次自开自交一次事务，按两秒就有几十步。

**1. 事务挂在按键重复段上。** `ShellState` 加 `nudge_txn` / `nudge_anchor` / `nudge_dirty`；
`nudge_begin()` 同锚点复用事务、换层则先收口；`nudge_flush()` 在 `nudge_dirty` 为假时
直接 abort，不给撤销栈留空步。三条分支（单层 / 多选 / 组）共用这套。

**2. 收口点选 `WM_KEYUP`。** 上游的边界是「松键」，`WM_KEYUP` 天然就是这个边界，不需要
定时器节流；定时器还要处理「按着键切走窗口」之类的情况，容易把事务拖长。

**3. 到处先收口。** 菜单命令入口、`WM_INITMENU`、鼠标按下/双击/中键/滚轮、失焦、拖放、
关窗都先 flush：既让「菜单一次点击 = 一步撤销」保持不变，也避免下一个操作撞上「已有事务」
被引擎拒绝。状态栏补「松键合并为一步」。

**验证。** 单测 **599 例 / 0 失败**（无新增：这是外壳时序，纯层测不到）；PE 导出
**138 不变**；壳自检 **245 项 / 失败 0**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
冒烟：连发 5 次 `WM_KEYDOWN` + 一次 `WM_KEYUP`，再撤销/重做、菜单微调 ×2 + 撤销，
进程存活、窗口仍响应。
文档：`docs/shell-parity.md` §5.30 项 5–6，`docs/manual-test.md` M38。

### 6.70 Shift 拖角 = 等比缩放（2026-10-21，Claude）

**补掉 §5.39 边界里的「等比缩放未做」。** 自由缩放两个方向各走各的，拖角很容易把
图层拉扁；上游按住 `Shift` 锁比例。

**1. 纯函数加一个开关。** `transform_scale_rect` 多一个 `uniform` 参数（默认 false，
既有调用点与用例不动）：只对角手柄生效，用指针要求的两个新尺寸取**相对变化更大**的
方向定倍率，另一方向按按下时的宽高比跟随；倍率下限同时满足两个方向的最小尺寸。

**2. 比例锁在按下的 base 上。** 每帧从 base 重算，不在上一帧结果上叠，所以来回拖不漂；
对角锚点与旋转补偿原样复用——等比只改两个尺寸，不改锚点语义。边手柄忽略 `Shift`
（那是「以中心等比」，本壳没有中心锚点这条路，静默按自由缩放处理更可解释）。

**3. 壳侧一行读键。** `layer_handle_extend` 读 `GetKeyState(VK_SHIFT)`（与旋转吸附
同一个读法），拖动中途按下/松开立即切换；状态栏补「Shift = 拖角等比」与「等比」标记，
仍是整段拖动一步撤销。

**验证。** 单测 **600 例 / 0 失败**（新增 `ui_core_transform_uniform_scale_locks_aspect`：
主方向倍率 + 对角不动、自由缩放的对照、边手柄忽略 Shift、下限、90° 旋转锚点）；
PE 导出 **138 不变**；壳自检 **245 项 / 失败 0**（基线 SHA256 仍 `26b3e910e3b9ccb7…`）；
`run_regression.py --require-ohos` **6/6 通过**；`build_windows_shell.py` 零警告。
文档：`docs/shell-parity.md` §5.39 项 9，`docs/manual-test.md` M39。

### 6.71 树形图层面板：组在上、缩进、可折叠（2026-10-21，Claude）

**补掉 §5.12 项 8 的三条边界。** 面板显示序一直是「数组逆序」，而组在数组里排在子层
**下方**，逆序后子层跑到了组行上面——与 PS 相反；同时没有缩进、没有折叠。

**1. 显示序抽成纯函数。** `comp_ui::build_layer_display` 输出 `display[row] = 数组下标` +
`depth[row]`：根层按数组逆序遍历，遇到组先输出组行、再递归输出子层（子层按堆叠序缩进在
下方）。折叠的组只留组行；悬空 parent 与成环的层不丢，收尾追加到根级且每层只出现一次；
行数超过 4096 整段回落到旧逆序，宁可没有树也不截断真实文档。**引擎落位不动**——只改
显示方式。

**2. 缩进 6 级封顶 + 全局三角槽位。** 每级 12px、上限 72px（更深的嵌套不再右推，否则名字
被挤出 248px 栏宽）；每行都留 14px 三角槽位，同层级名字才对齐，只有组行画 ▸/▾。

**3. 行号不再能反推图层。** 折叠改变行数、展开的子树平移后续行，于是点击、双击、Ctrl
晋升、Shift 区间、剪贴基底、重排、拖拽命中、批量枚举全部改走
`layer_row_at`/`layer_index_at_row`/`panel_row_of_id`；结构变化后选中态**按 id 重选**。

**4. 重排/拖拽改「同级序」。** 树形显示下相邻两行未必同级，`plan_layer_reorder` 与
`plan_layer_drag` 改为在同父集合里数兄弟序号；端头判定也按同级集合（否则组里唯一子层会
被告知还能上移）。`before = 0` 的引擎语义（非根插到最后一个同父兄弟之后、根追加到数组末尾）
被写进单测模拟器——原来的模拟器按「一律追加到末尾」写，是树形用例抓出来的偏差。

**5. 折叠是视图态。** 只改壳侧 `collapsed_groups`，不写文档、不进撤销。折叠后主选中层被
藏起来则选中落到组行，多选集合里被藏起来的 id 一并摘掉（批量命令按可见行枚举 id，
留着会让计数与执行数对不上）；双击三角槽位被吞掉，不会顺手弹改名框。

**验证。** 单测 **603 例 / 0 失败**（新增 `ui_core_layer_tree_display_puts_group_above_children`、
`ui_core_layer_tree_reorder_stays_within_siblings`、`ui_core_layer_tree_drag_uses_sibling_ordinals`）；
PE 导出 **138 不变**（零新增 ABI）；壳自检 **245 项 / 失败 0**（基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.42（并回填 §5.12 项 8），
`docs/manual-test.md` M40。

### 6.72 面板整块收起：F7/F8、菜单、标题行按钮（2026-10-22，Claude）

**补掉 §5.11「面板缩放 / 停靠」剩下的一半（§5.33 拖宽之外）。** 两个侧栏此前只能拖宽，
**不能收起**——宽屏时想临时腾出画布没有通路，窄屏时它们按三栏平铺挤掉画布。
浮动窗口与面板合并仍不做（前者要改布局模型，后者要改面板体系），本轮只做**收起/展开**。

**1. 收起是显示开关，不改宽度偏好。** `LayoutPrefs` 新增
`layer_panel_collapsed` / `adjust_panel_collapsed`，收起只影响 `compute_layout` 的
`wanted`，拖宽存下的宽度原样留着；展开即回到用户上次的宽度。

**2. 收起后仍占 24px 布局宽度，不是 0。** 三栏不变量
`layer.w + canvas.w + adjust.w == width` 保持；24px 竖条既是可点的「展开」把手，
也让 §5.33 的 pan 补偿连续——宽度从 248 跳到 0 会让画布中心一次跳 124px，
而 24px 的台阶只有 112px 且用户随手就能点回来。窄窗口下仍走既有收起规则
（`width < wanted + kMinCanvasWidth` 即宽度 0），此时连竖条也不画，面板是**不存在**而不是被压扁。

**3. 命中：展开态 18×18 按钮，收起态整条 24px。** `Layout` 新增
`layer_toggle` / `adjust_toggle`，`panel_toggle_at` 统一判定。展开态按钮贴标题行右侧
（`kPanelToggleRightMargin = 8`、`kPanelToggleTop = 6`、18×18），与 §5.33 的 5px 分界线
留 **3px 净空**——两者命中共面时先判按钮再判分界线，不会出现「想拖宽却收起了面板」。
收起态没有分界线（没有可拖的东西），整条竖条就是展开热区。点击次序固定在工具栏之后、
分界线与面板行之前。

**4. 通道条左移 26px。** 18+8 的按钮位在图层面板标题行右侧、但也在调整面板**标题行右侧**，
而调整面板右端原本坐着 RGB/B 通道条。`adjust_channel_chip_rect` 的 x0 统一左移
`kPanelToggleSize + kPanelToggleRightMargin`，按钮与通道条不重叠，且这是唯一的几何来源，
绘制与命中共用一份。

**5. 绘制两种形态。** 展开态是方按钮：常态 `RGB(38,42,52)`、悬停 `RGB(52,62,80)`，
`FrameRect` 描边，箭头朝外（图层面板朝左、调整面板朝右）；收起态是竖条：
常态 `RGB(30,33,40)`、悬停 `RGB(44,50,62)`，朝画布一侧一条 1px `RGB(16,18,22)` 边，
竖排「图/层」「调/整」两行字加一个朝内箭头。悬停走 `panel_toggle_hover`（1/2/0），
光标 `IDC_HAND`，`WM_MOUSELEAVE` 归零。

**6. 三条入口同一命令。** F7 = 图层面板、F8 = 调整面板（PS 同键位；`VK_TAB` 已被
§5.33 的面板轮转占用），视图菜单两项带勾选（`sync_menu_state` 同步），标题行按钮直点。
三者都走 `run_shell_command` → `panel_toggle_collapsed`，`target` 1/2 越界直接返回。

**7. 收起要补画布中心，且不进撤销。** 收起后画布宽 +224px，`CanvasView` 的中心随之平移
112px，画布内容会「跳」一下；函数在切换前后各算一次 `client_layout`，用
`CanvasView::canvas_center` 的差值 `pan_by` 抵消（与 §5.33 分界线拖动同一套账）。
这是视图态，不写文档、不进撤销栈——F7 不该出现在「撤销」里。状态栏给一句反馈；
窄窗口放不下时明说「当前窗口放不下，放宽窗口后生效」，不让用户以为按键没生效。

**8. 边界。** 不做浮动窗口、不做面板合并；收起态面板内容（缩略图/图层行/参数行）不绘制，
不留半截；鼠标滚轮的三条分支（直方图缩放、历史滚动、参数滚动）都加了
`!*_collapsed` 守卫，收起后滚轮不会去滚看不见的东西；偏好仍是会话态，
不落盘（与 §5.33 的宽度一致）。

**验证。** 单测 **604 例 / 0 失败**（新增 `ui_core_collapsed_panels_layout`：24px 竖条与
精确平铺、收起态按钮 == 整条且分界线为空、展开态 18×18 落在标题行内且不与分界线/通道条重叠、
`{0,24,203,204,205,227,228,229,400}` 窄宽扫描，204px 处恰好 `24 + 180 + 0`）；
PE 导出 **138 不变**（零新增 ABI）；壳自检 **245 项 / 失败 0**（基线 SHA256 仍
`26b3e910e3b9ccb7…`）；`run_regression.py --require-ohos` **6/6 通过**；
`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.43（并回填 §5.11、
§5.33 项 8），`docs/manual-test.md` M41。

### 6.73 选区扩缩手柄：8 个把手 + 绝对矩形重采样（2026-10-22，Claude）

**补掉 §5.11 最后一条「选区扩缩手柄」。** 选区有了、面板里的数值扩缩也接了
（`comp_selection_modify`，1..500 px），但选区边上**没有可拖的把手**。卡点不在 UI
而在 ABI：手柄说的是「把选区改到这个矩形」（绝对落点），而既有入口只有**相对**扩缩
（再扩几像素）。蒙版在引擎里，壳不自己改，所以先补 ABI 再谈把手。

**1. ABI 尾部追加两个导出（138 → 140），既有签名一个不动。**

- `comp_selection_get_base_bounds(document, comp_rect* out)`：基础蒙版的**紧**包围盒，
  **不含**羽化外扩。它与既有 `comp_selection_get_bounds` 是两种口径——后者是
  「可见 coverage 范围」，羽化会外扩 `ceil(2*feather)`；手柄必须贴在真正被变换的
  那张蒙版上，所以用前者。无选区/显式空选区写 `{0,0,0,0}`；NULL → 参数错（事务前拒绝）。
- `comp_selection_set_bounds(document, comp_rect bounds, int32_t* out_changed)`：
  源矩形恒为当前紧包围盒，调用方只给落点。

**2. 恒等必须是逐字节不变。** 源 = 当前紧包围盒，所以
`bounds == get_base_bounds()` 不走重采样；实现里 `masks_equal` 再兜一层，
坐标舍入也不会让「把手拖回原处」多出一步撤销。这不是优化而是正确性：
用户把把手拖出去又拖回来，文档必须和没动过一样。

**3. 双线性，保留抗锯齿边。** 目标像素中心 → 源坐标：
`(x + 0.5 - target.x) * scale + source.x - 0.5`，`scale = source.w / target.w`。
减 0.5 是因为源像素中心在整数格点上——`target == source` 时恰好落回整数，
双线性权重全在原像素上（第 2 条的算术依据）。最近邻会把椭圆选区变成锯齿，不用。

**4. 裁剪与缩放比解耦。** 越出画布的部分直接丢弃（画布外没有选区），但 `scale`
仍按**原样** target 算：贴着画布边界拖时内容不能被悄悄拉伸。`target.x + target.w`
用 int64，避免整数溢出。

**5. 退化 = 显式空选区，羽化不动。** `w <= 0 || h <= 0` 写入全 0 蒙版但 `active`
仍为 1——PS 里把手拖到对边得到的是「有选区但空」，不是「没有选区」（两者在合成
限制与相加/相减语义上不同），有测试钉住。`feather` 半径原样保留（羽化是参数不是
几何，与 `comp_selection_translate` 同口径）。不做镜像翻转，夹取由宿主负责。
一次调用 = 一步撤销（`ImplicitTxn("Transform Selection")`）。

**6. 几何全在 ui_core，且手柄是屏幕尺寸。** `kSelectionHandleCount = 8`，下标
**0=左上 1=上 2=右上 3=右 4=右下 5=下 6=左下 7=左**（偶角奇边，绘制/命中/拖拽
共用这一个顺序）。`selection_handle_point` 用 `x + w` / `y + h`（左闭右开像素网格，
与 `doc_to_screen` 同口径）；`selection_handle_hit` 在**屏幕空间**取最近的、
距离 ≤ 7 px 的把手（大小不随缩放变，所以不能在文档空间命中；NaN 永不命中）；
`selection_drag_rect` 只动被拖的边/角，对边不动。

**7. Shift 等比只对角生效，且按主导轴取宽。** 锚点是对角线另一端，
`width = max(|dx|, |dy| * aspect)`：只按 dx 算会在「往下拖得比往右多」时失守。
边手柄忽略 Shift（PS 也不等比）。拖过对边夹到 1 px 并停住（先夹再 round），
不镜像。

**8. 壳只做四件事。** 取包围盒 → 命中 → 拖出矩形 → 松手一次 `set_bounds`。
把手只在 Marquee / Lasso / Wand / Move 下出现（画笔类工具的落点含义是画图，
把手会抢命中），框选/套索/裁剪/钢笔/缩放框的临时态里不出现；点击优先级排在
框选、套索、魔棒、整层拖动**之前**，返回 true 就终止调用链——否则按在把手上
会退化成新框选，把用户想改的选区直接删掉。拖动只改预览（`selection_handle_current`），
引擎在松手前不动，所以中途没有撤销步；松手时先落 target/source 再
`selection_handle_cancel()`，**然后**才 `ReleaseCapture()`（它会同步派发
`WM_CAPTURECHANGED`，顺序反了会把刚要提交的落点清掉）。落点与源矩形逐字段相等
就不调引擎，状态栏说「选区未变化」。`Esc` / `WM_KILLFOCUS` / `WM_CAPTURECHANGED` /
换文档都只丢预览，取消幂等。悬停光标：角给斜双箭头（0/4 NWSE、2/6 NESW），
边给单轴箭头（1/5 NS、3/7 WE），只在画布矩形内生效。

**9. 绘制。** 8 个 7×7 白底 + 1 px 黑框方块（GDI `Rectangle`），锚点来自
`selection_handle_centers`——与命中共用同一份屏幕坐标，杜绝「看得见点不中」；
拖动中另画白实线 + 黑 `PS_DOT` 双笔画预览矩形，让「松手会变成多大」提前可见。
蚂蚁线口径不变，覆盖层都裁在 `layout.canvas` 内。

**10. 边界。** 只做拖动缩放（透视/旋转变换手柄属 §5.39）；不做 Alt 从中心缩放、
Shift 边上等比（PS 也没有）；把手不改羽化、不改选区模式，一次拖动 = 一次 REPLACE
式几何变换；引擎不存「上一次的矩形」，每次以当前紧包围盒为基准，两次拖动串行、
可撤销、不累积误差。

**验证。** 单测 604 → **610 例 / 0 失败**（ABI 层 4 例：羽化不改紧包围盒 +
取消后归零 + NULL 拒绝；带羽化椭圆恒等前后逐字节相同、`changed = 0`、撤销深度
不动；16×12→32×24 面积四倍、内点 255、最外圈 0<v<255、旧轮廓消失、一步撤销回原矩形、
再缩到 8×6；退化矩形得到 `active=1` 的空蒙版、可撤销、无选区无操作、越界只留画布内。
UI 层 2 例：8 个锚点/最近命中/容差边界/空表与 NaN 不命中；四边四角单向移动、
对角不动、Shift 角等比按主导轴、边忽略 Shift、拖过头夹 1 px、NaN 与越界不炸）。
PE 导出 **138 → 140**；壳自检 **245 项 / 失败 0**（基线 SHA256 仍
`26b3e910e3b9ccb7…`，本轮没动既有自检路径）；`run_regression.py --require-ohos`
**6/6 通过**；`build_windows_shell.py` 零警告。文档：`docs/shell-parity.md` §5.44
（并回填 §5.11），`docs/manual-test.md` M42。

### 6.74 字体：引入 FreeType + 字体加载层（P5-0 / P5-1，2026-10-22，Claude）

**P0-5 的第一、二步。** 文字工具一直只有编译进内核的 5×7 点阵 ASCII，排不了中文。
要接真字体先得有个字体引擎，而「零依赖、以源码 vendoring」是这个仓既有规矩
（libpng / zlib / stb 都这么进来）。

**1. P5-0：FreeType 2.13.3 进 `engine/third_party/freetype/`，三路构建对齐。**
只编需要的模块（`base` / `truetype` / `smooth` / `sfnt` / `psnames` / `cff` /
`psaux` / `pshinter`），不引 harfbuzz、不引 png/brotli（`FT_CONFIG_OPTION_*`
一律关）；`scripts/build.py`、`scripts/build_ohos.sh`、鸿蒙 `CMakeLists.txt`
三处源文件清单必须一致——漏一个模块是链接期才炸的错误，不是编译期。

**2. P5-1：`engine/domain/font_face.*` + 5 个导出（140 → 145）。**
`comp_font_load_file` / `comp_font_load_memory` / `comp_font_release` /
`comp_font_get_metrics` / `comp_font_name`。句柄与引擎无关（可在 `comp_engine_create`
之前加载），内置 magic 判活：释放后再用得到 `COMP_ERR_STATE` 而不是读已释放内存；
**重复释放是宿主错误，不是「安全空操作」**——文档曾这么写，本轮改成实话。
`comp_font_metrics` 给 em 比例（ascender / descender / line_gap / height /
max_advance）与 `has_cjk`、`units_per_em`、`scalable`，宿主的可用性判断不需要自己解析
name 表。`FontFace::render_glyph` 用 `FT_Set_Char_Size` 的 26.6 定点保留小数字号、
`FT_LOAD_NO_HINTING | FT_LOAD_RENDER` 取灰度 coverage，`FT_Bitmap.pitch` 为负时
按行号从尾取行（自下而上存行是合法情形）；`FT_Face` 非线程安全，全程持内部互斥锁。

**3. 随仓字体：`resources/fonts/NotoSansSC-Regular-Subset.ttf`。**
Noto Sans SC 可变字体固定到 Regular（wght=400）后按固定字符集裁剪：ASCII + Latin-1、
常用标点、CJK 标点、全角形式、**GB2312 一级汉字 3755**；31036 → 4491 字形，
1.23 MB。产物族名 `Noto Sans SC Subset`（OFL 要求修改版不冒用保留名），
`OFL.txt` 同目录一起分发；`scripts/make_font_subset.py` 可重放，字节确定，
不随宿主机器装了什么字体而变（见 `resources/fonts/README.md`）。

**4. 验证。** 单测 610 → **614 例 / 0 失败**（`tests/test_font.cpp`：加载失败路径、
magic 判活与重复释放、度量字段、小数字号的 26.6 定点、缺字步进、子集里真有中文
字形、OFL 名字表未被裁掉）；PE 导出 **140 → 145**；壳自检 **245 项不变**（引擎侧
新能力，壳还没接）；`run_regression.py --require-ohos` 6/6（鸿蒙交叉编译也编进了
FreeType）。

### 6.75 文字字体轨道：UTF-8 / CJK 排版与栅格化（P5-2，2026-10-22，Claude）

**引擎侧接上 P5-1 的字体。** 不传字体时旧轨道一个字节都不许变，所以是一条**并行**
轨道而不是改语义：`comp_tool_op_desc` **尾部**追加 `comp_font type_font`
（既有字段偏移不变，宿主需用同版本头文件编译），`type_measure_text_with_font` /
`type_rasterize_with_font` 是新的内部入口，旧的两个函数原样保留。

- **UTF-8 严格解码**：拒绝过长编码、代理对、> U+10FFFF、孤立续字节、Tab 等控制
  字符；不合格整段 `kBadText`，不当成「能排多少排多少」。
- **量纲**：字体轨道下 `font_size` 是 **em 高**（旧轨道是格子高，7 px = 1 倍），
  行步进 = `font_size × line_height`，首行基线 = 文本框顶 + `ascender × font_size`；
  逐字形用字体自己的 `advance`，`letter_spacing` 加在字形之间。
- **文本框四周各留一 em 余量再裁到图层**：负左侧距与重音不会被切掉；字形原点取整到
  像素（已知取舍，亚像素定位留给以后）；重叠像素取**最大**覆盖率而不是相加，免得
  相邻字形接缝处叠亮。
- **缺字只占步进、不画 .notdef 方框**，个数如实写进 note；`kFontError` 专指宿主态
  错误（句柄失效 / FreeType 失败），**绝不静默回落到 5×7**。
- 顺手修掉 `build_font_layout` 的行尾哨兵缺失：只有行首没有行尾，最后一行永远落不进
  循环，单行文本量出 `line_count = 0`、宽 0 且一个字都不画——新用例接入当天抓到。

**验证。** 单测 614 → **619 例 / 0 失败**；**无新导出，仍 145**；P5-11 的 22 条
逐字节断言（不传字体时旧轨道）全部保持通过；壳自检尚未接（P5-3 才接）。

### 6.76 Windows 壳字体轨道：中文输入、em 字号、缺字如实回报（P5-3，2026-10-22，Claude）

**P0-5 的壳半边。** 启动加载随仓字体，面板第四行 `Typeface` 切轨道，键入只走
`WM_CHAR`（输入法才进得来），退格按码点删。ABI 零新增，仍 145 导出。

- **字体从哪来**：`load_shipped_font` 依次探 `<exe>/resources/fonts`、
  `<exe>/../resources/fonts`（开发构建的 `build/` 就在仓根下一层）、`<cwd>/resources/fonts`；
  闸门是 `scalable && units_per_em > 0 && has_cjk`，位图或纯拉丁字体一律不选。
  找不到不是错误：状态栏如实说「未找到随仓字体」，工具退回点阵。
- **删掉 `type_ascii_from_vk`**：VK→ASCII 看不见输入法（中文永远打不进去），且与
  `TranslateMessage` 派发的 `WM_CHAR` 各插一遍，开输入法时双字。现在 `WM_CHAR` 是
  唯一来源，UTF-16 代理对在壳里拼成码点，孤立半个丢弃。
- **编辑按码点**：`Backspace` 走 `type_utf8_erase_last`，按字节删会留下半个汉字，
  引擎如实拒收成 `kBadText`（用户看到的是「退格把整段字弄没了」）。缓冲不是完整
  UTF-8 时函数返回 0 且不动——不猜该丢哪个字节。
- **预览是估计值**：外壳没有「只度量不落墨」的 ABI 入口，也不为此加导出；宽度按
  CJK/全角 1 em、其余 0.55 em 估（±5%），界面数字带 `≈`，状态栏写「宽度是估计值」。
  画布仍不预演字形——GDI 画一套与 FreeType 落出来的不是一回事。
- **面板**：第 4 行是**开关行**（`type_panel_row_is_toggle`），数字键不进输入态；
  行号只追加，前三行的几何与命中顺序不变。

**验证。** 单测 **619 例 / 0 失败**（换内容：22 条 VK 断言 → UTF-8 助手与字体轨道度量
30 条）；壳自检 **245 → 257 项**，基线 `compositor_shell_selftest.txt` 重新生成
（SHA256 `377DE660D48AC933…`）；`run_regression.py --require-ohos` 6/6；
`build_windows_shell.py` 零警告。边界与目视项：`docs/shell-parity.md` §5.45、
`docs/manual-test.md` **M43**（留 ALEX 真机）。

**鸿蒙侧**：`shells/ohos` 目前只接引擎/文档/IO/render，**还没有文字工具**，所以没有
「缺字体」可回落；将来接入时须把 `resources/fonts` 打进 assets、用沙箱路径调
`comp_font_load_file`，失败时按同一条规矩退回点阵并如实提示，不得静默画方框。

### 6.77 文档级对话框：新建 / 图像大小 / 画布大小 / 历史容量（P0-1、P0-2、P0-3、P1-8，2026-10-22，Claude）

**按 `BACKLOG.md` 阶段 A 往前推的第一批，四项全部零 ABI 新增，仍 145 导出。**
四个面板都画在右栏调整面板的位置（`layout.adjust_panel`），沿用修边面板那套模态规矩：
面板打开时键盘归面板（Tab/Shift+Tab 切行、数字键直接键入、`-`/`=` 或方向键步进、
回车上值/应用、Esc 撤草稿再关面板），Ctrl 组合仍穿透，其余字母键被吞掉，
避免「对话框开着按 B 把底层工具换掉」。

| 入口 | 快捷键 | 行 |
|------|--------|----|
| 文件 > 新建文档… | Ctrl+Alt+N | 宽 / 高 / 分辨率 / 背景（白·透明·前景色） |
| 图像 > 图像大小… | Ctrl+Alt+I | 宽 / 高 / 约束比例 / 百分比 |
| 图像 > 画布大小… | Ctrl+Alt+C | 宽 / 高 / 锚点（九宫格） |
| 编辑 > 历史记录容量… | Ctrl+Alt+H | 步数上限 / 内存上限（MiB） |

- **新建**：`Ctrl+N` **仍然是立即新建**（1280×720@72 透明），不弹面板——`visual_check.py`
  的 view 场景断言的就是这个行为，改成弹窗会直接破既有断言。带参数的走 Ctrl+Alt+N，
  面板记住上次设置。背景经 `comp_layer_set_pixels` 写进首层（0 白 / 1 透明 / 2 前景色），
  所以填了底色的新文档标题栏立刻出现 ` *`：这是如实反映「文档内容与磁盘不同」，不是误报。
- **图像大小**：百分比行**不另存一份**，始终由「当前宽 / 基准宽」现算，来回切行不会漂移；
  基准取**打开面板时**的尺寸，所以连续微调不累积取整误差。约束比例开着时改任一边，
  另一边按基准比例联动。
- **画布大小**：`comp_document_set_size` 本身不做内容偏移，壳在**一个事务**里先按锚点算
  `dx/dy`、把**引擎里的全部图层**（用 `comp_layer_list` 枚举，含折叠组的子层——只遍历面板
  可见行会让组内子层脱队）逐个 `get_transform`/`set_transform` 平移，再改文档尺寸，
  一次提交 = 一步撤销；任一步失败就 `comp_txn_abort`，不留半成品。
- **尺寸变化后的缓冲同步**：`render_canvas` 直接按 `shell.width*shell.height` 索引
  `shell.rgba` / `shell.framebuffer`，换尺寸必须重分配。统一收在 `sync_document_buffers`：
  只在**长度真的变了**时重分配，旋转、修边这类等尺寸操作不受影响。
- **历史容量**：0 输入回显「引擎默认」（200 步 / 512 MiB），壳里不抄一份默认值当第二真源。
- **标题栏**：`update_window_title` 每帧读 `comp_document_is_dirty`（不维护壳侧副本），
  形如 `Compositor (Windows shell) - CPU - 未命名 *`；先与缓存比较，没变就不 `SetWindowTextW`。
  同时补上 `文件 > 另存为…`（Ctrl+Alt+S）：成功后记下路径，`文件 > 保存`（Ctrl+S）对有路径
  的文档直接写回，无路径时退化为另存为。

**写探针时抓到的三个壳层缺陷**（都已修，且都由新场景首次跑到）：

1. 面板文字是**乱码**：`ui_core` 里的对话框标题/标签/提示是 UTF-8 中文，绘制却走
   `widen_ascii`（逐字节抬高），显示成 `å...ç...`。改用 `widen_utf8`——同仓 `widen_ascii`
   只配给纯 ASCII 的引擎名字段用。
2. **空格键到不了面板**：`WM_KEYDOWN` 里「空格 = 临时抓手」的判定在 `handle_key` **之前**，
   对话框的「空格翻开关」永远收不到。现在对话框打开时空格归面板（`dialog_owns_space`），
   抓手逻辑不受影响。
3. **回车要按两次**：键入态下 `VK_RETURN` 只提交草稿就返回，而 `doc_dialog_apply` 里那句
   「键入态下回车 = 先落值再应用」永远走不到。现在键入态回车直接调 `doc_dialog_apply`。

**验证。** 单测 **624 例 / 0 失败**（`ui_core_doc_dialog_rows` / `_anchor` /
`_percent_and_aspect` 三组新增）；`run_regression.py --require-ohos` **6/6**，壳自检
**257 项 failures=0、SHA256 `377DE660D48AC933…` 与基线一致**（本批没碰 `run_selftest()`）；
`build_windows_shell.py` 零警告；`check_tool_table.py` PASS（20 个工具未变）；
`visual_check.py` **86 条 / 18 场景 / 0 失败**——新增的 `doc_dialog` 场景只追加断言，
量的是**画布屏幕宽高比**（空透明文档渲染成纯黑，`detect_document_rect` 口径可靠）：
新建 640×480 → 4:3、图像大小 320×120 → 8:3（顺带证明空格确实关掉了约束比例）、
画布大小 520×120 → 13:3、Esc 取消不改尺寸、历史容量面板开合有像素变化。

**没做的（仍是缺口）**：关闭/退出前的 dirty 三选一（刻意不用 `MessageBox`——自动化
`stop()` 是「post WM_CLOSE 后等 5 秒」，阻塞弹窗会让每次目视跑成超时，得做成面板内提示）；
最近文件、多文档；颜色模式 / 位深 / 预设（要 `comp_document_desc` 尾部追加）。

### 6.78 Camera Raw 滤镜入口（P1-5，2026-10-22，Claude）

**引擎与 763 张实测表早就交付（§6.10），外壳一处调用都没有——用户完全拿不到。本批只做接线，
零 ABI 新增（仍 145 导出）。** 入口 `滤镜 > Camera Raw 滤镜…`（`Ctrl+Alt+R`），复用
§6.77 的右栏模态面板与整套键盘模型（Tab 切行 / 数字输入 / 回车应用 / Esc 取消），
12 行 Basic 组：曝光、色温、色调、对比、高光、阴影、白色、黑色、清晰度、鲜艳度、饱和度、去雾。

- **表在启动时加载**：`load_shell_camera_raw_tables`（引擎创建后、建窗之前），候选路径与
  字体同序（`exe/resources` → `exe/../resources` → `cwd/resources`）。缺文件**不阻断启动**，
  菜单照常出现，打开面板时状态栏报「色彩表未加载」——比整个壳起不来强。
- **JSON 生成在 `ui_core.h`**（`camera_raw_settings_json`）：只写**非零行**，全默认因此是
  `"{}"`，引擎对恒等设置回 `changed=0`、不开事务、不记撤销步。键名是引擎严格解析的
  camelCase，写错一个字符就是 `COMP_ERR_INVALID_ARGUMENT`，所以键表在 `ui_core` 里并有
  单测钉死（12 个键逐字 + 互不重复）。
- **曝光用整数存**：`0.01 EV` 为单位（±500 = ±5.00 EV），面板只收整数，写 JSON 时才除以
  100。`wsprintfW` 不支持浮点（P0-3 的曲线面板曾在 `%f` 上崩过），显示端也把整数拆成
  「元」「角分」两段拼，显示值与写进 JSON 的值同源。
- **负值输入**：`doc_dialog_begin_input` 现在接受行首 `-`，但只对 `min < 0` 的行生效；
  减号键在这些行上**起输入**而不是步进 -1（左箭头仍可步进），其它对话框行为不变。
  错误位置/正区间行上的 `-` 直接忽略——若当噪声跳过，`-5` 会静默变成 `+5`。
- **打开前先查两件事**：有选中图层、表已加载。两者都满足才开面板，用户不会填完参数才被拒。
  应用失败（组图层 / HDR 图层 / 引擎拒绝）时**面板留在原地**，参数不丢。
- **已知边界**：写选中图层**整层**，不吃选区；ABI 没有「读回图层当前 Camera Raw 参数」的
  getter（设置是破坏性写进像素、不留在图层上），所以每次打开都是零草稿，副标题如实写明。

**验证。** 单测 **625 例 / 0 失败**（新增 `ui_core_camera_raw_rows` 钉行规格与 JSON）；
`run_regression.py --require-ohos` **6/6**；壳自检 **257 项 failures=0、SHA256
`377DE660D48AC933…` 与基线一致**（没碰 `run_selftest()`）；`build_windows_shell.py` 零警告；
`check_tool_table.py` PASS（20 个工具未变）；`visual_check.py` **91 条 / 19 场景 / 0 失败**——
新增 `camera_raw` 场景 5 条量真实像素：`#808080` 灰块上 +1.00 EV → 128→182、Ctrl+Z 精确回到
128、-1.00 EV → 83（同时证明 `-` 起输入没被当成步进）。

### 6.79 剪贴板：拷贝 / 合并拷贝 / 剪切 / 粘贴 / 贴入选区 / 清除（P0-4，2026-10-22，Claude）

**全仓此前没有一行剪贴板代码**，复制粘贴这条最日常的路径是断的。本批补齐 `编辑` 菜单六项，
只有一处 ABI 尾部追加（145 → **146**）。

- **唯一新增导出 `comp_layer_get_pixels`**：逐字节往返必须拿到图层自己的原始像素缓冲；
  合成结果做不到——`comp_render_flatten` 会把图层按 transform 重采样、叠在背景上、再过一遍
  混合模式，回来就不是原来的字节。该导出只读缓冲与格式描述，不做色彩转换。
  尾部追加、宿主重编译，既有 145 个符号的签名与顺序未动。
- **两种剪贴板格式**：自定义 `Compositor Layer Pixels`（`"CGP4"` 魔数 + 宽/高/原点/预乘标记 +
  预乘 RGBA8）用于本程序内往返，**逐字节相等（含 alpha）**；同时写 `CF_DIBV5`（直通 alpha、
  自下而上 BGRA、`BITMAPV5HEADER` + `BI_BITFIELDS` 标准掩码）供外部程序取用。
  读时优先自定义格式、退化到 DIB；32 位 DIB 且 alpha 全零时按**不透明**处理（不少老程序
  不写 alpha 通道，照抄会被当成全透明）。
- **拷贝走哪条路**：图层是轴对齐 RGBA8（`|rotation| ≤ 1e-4`、无翻转、格式 RGBA8、
  transform 尺寸 == 像素尺寸）→ 直接读原始像素，原点取 transform 左上角；否则退化为
  `comp_render_layer_thumbnail(doc, id, max(canvas_w, canvas_h))` 取**画布尺寸的单层合成**
  （"所见即所拷"），尺寸对不上画布则明确报错而不是悄悄缩放。有选区时按文档空间 coverage
  裁剪，预乘空间里直接乘覆盖度（`coverage == 255` 时整字节拷贝，保证满覆盖像素零损耗）。
- **剪切 / 清除要求同一 1:1 RGBA8 条件**：不满足就报错，而不是把 16 位/HDR 层降级成 8 位、
  或抹掉错误的区域。剪切是"先写剪贴板、成功才清像素"，写失败状态栏明确写「未改动」。
- **粘贴**：`comp_layer_add` + `comp_layer_set_pixels`，贴入选区时再按文档空间选区为新层建
  蒙版（蒙版尺寸按新层像素尺寸索引，不是文档尺寸）；全部包在一个显式事务里，
  失败 `comp_txn_abort`，成功 `comp_txn_commit` = **一步撤销**。落点优先取当前选区 bounds
  左上角，无选区时用自定义格式里记下的原点；跨应用 DIB 没有原点，落 (0,0)。
  100 MP 上限防止误粘超大图把内存打爆。
- **键盘冲突**：`Ctrl+C` / `Ctrl+V` 以前会掉进工具快捷键（C = 裁剪，V = 移动），现在在
  `handle_key` 里先截住并 `return`；`Ctrl+Shift+V` 仍是「切换图层可见性」，`Del` 仍是
  「删除图层」——既有语义一个没改，所以自检 257 项的 SHA 不变。清除只挂在 `Backspace` 上。
- **已知边界**：跨应用只有 32 位 DIB，16 位/浮点会被压到 8 位；不写 `CF_BITMAP` / `CF_DIB`
  （老程序兼容面），也不做 ICC 转换（与 P0-9 色彩管理关联）。

**验证。** 单测 **626 例 / 0 失败**（新增 `comp_layer_get_pixels` 的往返与越界护栏）；
`run_regression.py --require-ohos` **6/6**；壳自检 **257 项 failures=0、SHA256
`377DE660D48AC933…` 与基线一致**；`build_windows_shell.py` 零警告；
`check_tool_table.py` PASS（20 个工具未变）；`visual_check.py` **97 条 / 20 场景 / 0 失败**——
新增 `clipboard` 场景 6 条按"铺橙 → 拷 → 清 → 贴入 → 撤销 → 粘贴 → 撤销"走完整条路径：
清除后回到黑、贴入只在选区内出现橙色而选区外仍是黑、两次粘贴各自只占一步撤销。

### 6.80 裁剪拖框修正：迟到鼠标移动不再改框（2026-10-22，Claude）

`visual_check.py` 跑到 97 条时 `crop` 场景间歇失败：提交后的画布比拖出的框大一截。
定位到两个都出在拖拽实现里的缺陷（与剪贴板无关，属同批发现）：

- **框在上一帧上累积**：旧 `crop_extend` 用 `min()` 往 `crop_rect.x/y` 里收，框只增不减——
  拖拽中抖一下、或回车前又动了一下鼠标，框就永久撑大。现在记住按下点 `crop_drag_start`，
  每帧由「起点 → 当前点」**重新规范化**，方向随便拖都能收回。
- **松手后仍在改框**：`crop_active` 一个标志兼任「框已定」与「正在拖」两种状态，于是松手后
  任何 `WM_MOUSEMOVE` 都继续重算。真人操作时是"松手后挪鼠标框跟着变"；脚本注入
  （`PostMessageW`）时更糟——松手后的一条迟到消息会改掉待提交的区域，回车裁出来就对不上。
  现在用独立标志 `crop_dragging`：按下置位，`WM_LBUTTONUP` 以**松手坐标**规范化一次后
  立即清零，`WM_CAPTURECHANGED` 也清零（框保留、仍可回车提交，但不再跟随鼠标）。

交互语义未变：拖拽中框实时可见、`Enter` 提交 / `Esc` 取消、提交走 `comp_crop_apply`
一步撤销。自检 257 项与 SHA `377DE660D48AC933…` **未变**；`visual_check.py` 97 条 /
20 场景 / **0 失败**，`crop` 场景 7 条连续复跑稳定。

### 6.81 关闭 / 退出的未保存提示：右栏面板内三选一，不用 MessageBox（P0-3，2026-10-22，Claude）

`WM_CLOSE` 此前是无条件 `DestroyWindow`：改了半天点右上角叉，修改连同撤销栈一起消失。
本批把提示做成**面板内**的三选一，零 ABI 新增（仍 **146** 导出、既有签名一个没动）。

- **为什么不是 `MessageBox`**：自动化 `stop()` 的收尾是「post WM_CLOSE 后等 1.2 秒，
  超时再补一次按键，最后 kill」。阻塞式系统弹窗会把每次目视跑拖成超时，而且它不吃我们
  注入的键盘脚本（`PostMessageW` 到主窗口的键不会进系统模态循环）。面板内提示则完全走
  现有的 `DocDialogKind` 渲染/键盘栈：`draw_doc_dialog` 会画、`present()` 会刷、
  截图探针量得到。
- **复用行模型，只加一个枚举值**：`DocDialogKind::ClosePrompt` 的 `doc_dialog_row_count()`
  是 1，唯一一行「动作」是 `DocRowKind::Choice`、`choice_count = 3`，取值
  `kCloseActionSave(0) / Discard(1) / Cancel(2)`（`ui_core.h`，纯头文件常量，单测可测）。
  默认停 **0 = 保存并关闭**，所以「回车」永远是安全动作；Tab/↑↓ 切行、←/→ 步进照旧。
- **键位 `S` / `D`**：键盘模型里 `S` 保存并关闭、`D` 不保存直接关闭、`Esc` 取消。
  两个字母键只在 `ClosePrompt` 下打开——其它文档对话框继续吞掉字母，工具快捷键
  （C=裁剪、V=移动、B=画笔…）不会从提示底下穿过去。
- **「不关窗，除非真存下去了」**：有路径 → `save_document()` 的返回值说了算；无路径 →
  先弹 `save_document_dialog()`（另存为），回来后**再查一次** `comp_document_is_dirty`
  （`comp_document_save` 成功时内部 `mark_dirty(false)`，所以这次复查就是"到底存没存"的
  判据）。仍脏或路径为空只关面板、窗口留着，状态栏写「未保存（另存为已取消），窗口未关闭」；
  磁盘写失败同理「保存失败，窗口未关闭」。
- **取消真的不改东西**：`Esc` / 档位选「取消」只 `doc_dialog_close`，不碰文档也不动撤销栈；
  提示开着时再点一次关闭键当作取消（不销毁窗口，也不重置用户已选的那一档）。
- **共享渲染，键行改文案**：`draw_doc_dialog` 的三行固定键位提示对 `ClosePrompt` 换成
  「S 保存并关闭 · D 不保存 / 回车执行当前动作 / Esc 取消（留在文档）」；`doc_dialog_hint`
  写的是不重复的警示语「不保存 = 这次修改连同撤销栈一起丢弃」。其它对话框的像素一字未动。
- **边界（如实记）**：`文件 > 新建` / `文件 > 打开` 仍然直接销毁旧文档、不提示（同一批没动，
  本质是多文档上下文问题）；窗口太窄时右栏宽度塌成 0，提示只剩键盘路径可用。

**验证。** 单测 **627 例 / 0 失败**（新增 `ui_core_close_prompt_rows`：行数/档位数/
`min == Save && max == Cancel`/三档名字非空且互不相同/越界动作名与越界行都安全兜底）；
`run_regression.py --require-ohos` **6/6**；壳自检 **257 项 failures=0、
SHA256 `377DE660D48AC933…` 与基线逐字节一致**；`build_windows_shell.py` 零警告；
`check_tool_table.py` PASS（20 个工具）；`visual_check.py` **102 条 / 21 场景 / 0 失败**——
新增 `close_prompt` 场景 5 条：脏文档 post WM_CLOSE 后进程仍活着、右栏标题区变化 8046 像素、
`Esc` 后仍活着、取消不改画布（`pixel_diff == 0`）、按 `D` 后 3 秒内退出。
同时 `stop()` 学会在脏文档上补一个 `D`（其它场景不再为这个提示白等超时）。

### 6.82 工具选项栏：工具参数从右栏搬到工具条下（P0-10，2026-10-22，Claude）

改画笔大小此前只有两条路：`[` / `]` 快捷键，或切到右栏面板再找滑杆。本批补上
Photoshop 的**工具选项栏**——工具条正下方一条 32px 横栏，只放「当前工具」的参数，
改完立刻生效。零 ABI 新增（仍 **146** 导出），引擎一行未动。

- **布局是新增的一层，不是浮层**：`Layout` 增加 `kOptionsHeight = 32` 与
  `kMinBodyHeight = 96`，`compute_layout` 里 `body_top = toolbar_h + options_h`。
  横栏**整块参与布局**：显示时画布/两侧面板整体下移 32px，收起时高度记 0、画布
  回收这 32px，不存在「一条悬空带子盖在画布上」。矮窗口（主体不足 96px）先收选项栏，
  再挤压面板——挤扁的 32px 横栏连一行字都放不下，只会白白推走画布。
- **行集合是纯函数**：`ui_core.h` 新段 `ToolOptionId / ToolOptionKind / ToolOptionSpec`
  + `tool_option_count(ToolId)` / `tool_option(ToolId, index)`。绘制与命中都从这两个
  函数取数，不存在「画出来的行和点中的行不是同一行」。行集合按工具：
  画笔/橡皮/修补画笔/减淡加深/历史画笔 → 大小、硬度；污点修复 → + 模式；
  仿制图章 → + 取样（图层/合并）、对齐；选框/套索 → 羽化；魔棒 → 容差、羽化；
  渐变 → 类型；形状 → 形状、线宽。文字工具**刻意不给**：字号/行距在右栏已有完整面板，
  横栏再放一份就是两份真源。
- **几何与命中同一份常量**：`option_group_rect` / `option_control_rect` 按「左内边距 12、
  组宽 150、组距 8、标签 56、箭头各 18、值 54」排；放不下就**整组不画**（返回空矩形），
  不做半宽裁切——只剩 20px 的「+」比没有更糟。`option_hit` 只认两侧 18px 箭头，
  数值区点了不改参数（PS 里数值区是文本输入，本仓的数值编辑在右栏）。
  `Choice` 整块响应、点一下循环到下一档（`option_cycle_choice`，首尾绕环）。
- **改的是同一个 `ShellState` 字段**：`apply_tool_option` 直接写 `brush_size` /
  `brush_hardness` / `wand_tolerance` / `selection_feather` / `gradient_type` /
  `shape_kind` / `shape_stroke_width` / `clone_sample_all` / `clone_aligned` / `heal_mode`，
  夹取全部复用 ui_core 已有的 `step_*` 函数（横栏与快捷键不会分叉出两套边界）。
- **命中顺序**：`WM_LBUTTONDOWN` 里排在「工具按钮」之后、「面板开关/分界线/画布」之前。
  横栏命中即 `return 0`，否则同一次按下会继续落到画布上落下第一笔。
- **边界（如实记）**：横栏数值只能步进、不能键入（数值输入仍走右栏/快捷键）；
  `Retouch` / `Tone` 的子模式与强度已经有工具条右侧读数与快捷键，未搬进横栏；
  高 DPI 仍未做（`SetProcessDpiAware*`、manifest 都不在，本批没有半做——那是一整套
  UI 缩放改造，留作独立批次）；横栏宽度不足时只显示能放下的前几组（不滚动）。

**验证。** 单测 **628 例 / 0 失败**（新增 `ui_core_tool_options_bar`：各工具行数、
行 id/标签/区间/Choice 跨度/同工具不重复行、组控件矩形与放不下时整组消失、
Stepper 两侧箭头与数值区不同响应、空矩形不命中、Choice 绕环；布局测试补了
选项栏「要么整条满宽显示、要么高度 0」与高度阈值两侧的边界）；
`run_regression.py --require-ohos` **6/6**；壳自检 **257 项 failures=0、
SHA256 `377DE660D48AC933…` 与基线逐字节一致**（`kToolbarHeight/kOptionsHeight` 只出现在
`ui_core.h`，自检不覆盖布局）；`build_windows_shell.py` 零警告；`check_tool_table.py`
PASS（20 个工具）；`visual_check.py` **115 条 / 22 场景 / 0 失败**——新增 `options_bar`
场景 13 条：横栏 32px 且底色为 (38,40,46)、画笔有两组、控件盒底色正确、标签有字形、
小笔尖落点有橙色像素且 `Ctrl+Z` 清零、连点 4 次「+」后值区变化 **58** 像素而画布
**0** 像素变化（点击确实被横栏吃掉）、同一落点橙色像素 **122 → 418**（参数真的进了
画布）、形状工具的 Choice 组点击后值区变化 **326** 像素且画布不变。
`visual_check.py` 的布局镜像同步加了 `options` 与 `option_group_rect/option_control_rect`，
既有 21 个场景在画布下移 32px 后仍全绿。

### 6.83 外壳拆分：1.9 万行单文件 → 13 个翻译单元 + 文档上下文对象化（【架构】前提，2026-10-23，Claude）

多文档与面板停靠一直卡在两件事上：外壳是**一个 19,046 行的 main.cpp**（改一行
就要全量重编，冲突面永远是整份文件），以及**所有状态都挂在一份全局 `ShellState`
里**（换文档只能逐个字段擦洗，没地方放第二份文档的运行态）。本批只做前提，
不做多文档本身：把文件拆开、把「文档态 / 会话态」分开。**零行为变化**——函数体
逐字搬家，一行逻辑没改。

- **第一步：状态分层（`shell_state.h`，542 行）**。`DocumentContext` 收 189 个
  字段（引擎文档句柄、图层镜像与可见性缓存、选区/套索/魔棒中间态、拖拽与
  变换手柄、面板模态、滤镜/裁剪/曲线/直方图、视图），`ShellState` 收 60 个字段
  （引擎、工具与笔刷参数、面板宽度、窗口句柄）+ 一个内嵌的 `doc`。划分口径只有
  一条：**切文档之后必须重建或复位的进 `DocumentContext`**，其余留在 `ShellState`。
  多文档落地时把 `doc` 换成「标签页 → `DocumentContext`」，其余字段不动。
- **第二步：翻译单元拆分**。`shell_internal.h`（631 行）收拢 430 个跨文件函数
  声明（与定义逐字同签名）+ `ClipboardHeader` / `ClipboardImage` / `ToolIconMap`
  与少量内部常量；每个 .cpp 只 `#include "shell_internal.h"`，就能看到全部兄弟
  函数。分法是按职责而非行数均衡：

  | 文件 | 职责 | 行数 |
  |---|---|---|
  | `main.cpp` | `g_shell`、5 个通用小工具、`wWinMain` | 254 |
  | `shell_window.cpp` | 窗口过程、键鼠交互、菜单、视图 | 3,853 |
  | `shell_layers.cpp` | 图层 / 调整 / 曲线 / 效果 / 直方图面板 | 2,949 |
  | `shell_paint.cpp` | 画布渲染、工具条与各面板绘制、上屏 | 2,663 |
  | `shell_selftest.cpp` | 自检与呈现基准 | 2,560 |
  | `shell_tools.cpp` | 笔刷/修复/仿制/渐变/形状/变换 | 1,946 |
  | `shell_document.cpp` | 文档生命周期、文件对话框、导入导出 | 1,615 |
  | `shell_selection.cpp` | 选区、套索、魔棒、裁剪、颜色范围 | 1,009 |
  | `shell_clipboard.cpp` | 图层像素读写与剪贴板 | 703 |
  | `shell_path_history.cpp` | 钢笔路径与历史面板 | 582 |
  | `shell_filter.cpp` | 滤镜面板与裁切 | 425 |
  | `shell_type.cpp` | 文字工具与文字面板 | 390 |

- **只有两处非逐字的机械改动**，都是 C++ 链接规则逼出来的：① `finish_new_selection`
  的默认实参 `bool* applied = nullptr` 从定义移到声明（默认实参全程序只能出现一次，
  而其它翻译单元也要能省略它）；② 两个 `const wchar_t*` 常量改成
  `constexpr const wchar_t*` 才能进头文件（`constexpr` 隐含 inline，不会每个 TU
  各留一份定义）。其余常量本来就是 `constexpr`。
- **构建脚本一行未改**：`build_windows_shell.py` 本来就按 `*.cpp` glob 收集源文件，
  新文件自动纳入编译与链接。壳层仍然只 `#include <compositor/comp_*.h>`
  （`engine/abi/include`）与 `ui_core.h`，没有因为拆分碰到任何 `engine/` 内部头。
- **边界（如实记）**：本轮**没有**做多文档本身——没有标签页、没有 `DocumentContext`
  实例池、没有切换流程，`文件 > 新建 / 打开` 依然直接销毁旧文档；面板停靠同样
  只是「不再被单文件卡住」。`main.cpp` 里保留的 5 个工具函数
  （`set_status` / `number_text` / `widen_ascii` / `widen_utf8` / `narrow_utf8`）
  是被几乎每个 .cpp 调用的小工具，等它们有明确归属再挪。

**验证。** 单测 **631 例 / 0 失败**；`run_regression.py --require-ohos` **6/6**
（含鸿蒙 113 个文件交叉编译）；壳自检 **257 项 failures=0、SHA256
`377DE660D48AC933…` 与基线逐字节一致**——这是「零行为变化」最硬的证据；
`build_windows_shell.py` **零警告**；`check_tool_table.py` PASS（20 个工具）；
`visual_check.py --scene all` **122 项 / 0 失败**（18 个场景，新 `options_bar`
13 项同批跑绿）。

**一处既有 flaky（与本批无关，如实记）**：`options_bar` 的「选项栏点击不穿透到
画布」断言 `== 0` 偶尔报到 17～32 px。用拆分**之前**的 main.cpp 单独构建做对照，
同一场景 5 次跑有 3 次失败（32 px），拆后 4 次跑 1 次失败（17 px）——差异来自
`Ctrl+Z` 重绘与两次截图之间的时序，不是拆分引入。没有为了跑绿去放宽阈值，
留作独立修复（截图前等重绘落地）。

### 6.84 可编辑文字图层：文字对象 + 渲染缓存（P0-6 ①，2026-10-23，Claude）

P0-6 说文字是「一次性栅格化」，做海报时改一个字就要重画。本批落地 P0-6 的**第一刀**
（① 文本图层模型 + 无损重编辑），字体枚举与段落框不在其内。ABI **尾部追加 4 个导出**
（146 → 150），既有导出、枚举值、结构体布局一个没动。

**模型：对象是真源，像素是缓存。** `Layer::text`（`std::optional<TextObject>`）持有文本、
原点、样式、颜色、不透明度与**字体文件路径**；`Layer::pixels` 是恰好文本框大小的渲染
结果，`Layer::transform` 也设成同一个框。合成、缩略图、合并、拼贴、GPU 纹理全部照旧
走既有路径，**合成内核一行未改**。对象挂在 `Layer` 上而不是单独的注册表，所以撤销/重做
天然一致（`DocumentState` 快照本来就覆盖整份图层数组），不需要新的历史记录类型。

**四个导出（`comp_layer.h` 尾部）。** `comp_layer_add_text`（新建、一步撤销）、
`comp_layer_set_text`（改写并重渲这一层、一步撤销）、`comp_layer_get_text`（只读回读，
文本两段式）、`comp_layer_rasterize_text`（丢对象留像素；非文字层幂等 `COMP_OK`、
不记撤销步）。`comp_text_desc` 存的是**字体文件路径**而不是 `comp_font` 句柄：对象要进
`.comp`，句柄存盘即失效，只有路径 + `font_face_index` 能跨会话还原。

**参数闸先于副作用。** 空文本 / 超 65536 字节 / 旧轨道含 UTF-8 或 Tab / 字号行距字距
越界或非有限 / 原点非有限或绝对值 > 1e6 / alpha 为 0 / `opacity` 不在 `(0,1]` /
`_reserved` 非 0 → `COMP_ERR_INVALID_ARGUMENT`；文本框与画布完全不相交也是
`INVALID_ARGUMENT`；面积超上限量级 `COMP_ERR_UNSUPPORTED`；字体打不开
`COMP_ERR_IO` / `COMP_ERR_DECODE`。全部在**开事务之前**：不建层、不消耗 id、不动旧像素、
不进撤销栈；渲染先写局部缓冲、最后 `move`，失败没有半成品。

**文字层上的像素写入一律拒绝。** `comp_layer_set_pixels` 与 `comp_tool_apply` 遇到
`has_text()` 直接 `COMP_ERR_STATE`，因为像素是「对象当前渲染结果的缓存」——下一次改
字号会整层重渲，用户手绘的笔迹会被抹掉。要画就先 `comp_layer_rasterize_text`，与上游
「文字图层上不能直接落笔」同一条规矩。`comp_document_resize_image` 会**隐式丢弃**文字
对象：非等比缩放无法用「字号 + 文本框」表达，宁可退回普通图层也不撒谎。

**`.comp` 往返。** 图层记录追加**一个**键 `engineText`（缺键 = 普通像素层，旧工程逐字节
照旧；与 `engineIsolated` 同一条「默认值不写」的规矩），内部存文本/原点/样式/颜色/
不透明度/字体路径。读回时同样过一遍 `text_layer_validate`，**不可渲染的对象整块丢弃、
图层退回普通像素层**（像素照旧从 `imageFile` 读），坏数据不留半成品。

**壳侧：提交按落点分流。** 插入点落在**选中的文字图层框内** → `comp_layer_set_text`（改
这一段）；否则 → `comp_layer_add_text`（新建）。框内判定用 `comp_layer_get_transform` 的
矩形做包含测试，不另设真相。这条分流是必需的：第一次提交后新层就是选中层，若第二次
点击别处也走 `set_text`，上一段会被悄悄搬走（写视觉场景时先踩到，才补的
`type_origin_inside_layer`）。「是不是文字层」也全部问 ABI——查询模式下
`comp_layer_get_text` 的 `COMP_OK` / `COMP_ERR_STATE` 就是答案，同一个判据被提交分流、
菜单置灰、栅格化入口三处复用。

新增 `图层 > 栅格化文字`（`kCmdLayerRasterizeText = 1572`）。置灰规则进了纯函数
`comp_ui::layer_menu_enabled`（`LayerMenuState` 新增 `is_text`，`LayerMenuEnabled` 新增
`layer_rasterize_text` = `has_layer && is_text`），因为 `EnableMenuItem` 本身无法单测；
`sync_menu_state` 只把 ABI 查询结果填进去。字体路径从 `load_shipped_font` 一路带进
`ShellState.type_font_path`（多一个默认为 `nullptr` 的出参，既有两参数调用不受影响）。

**验证。** 引擎新增 `tests/test_text_layer.cpp` 8 例（增改回读往返、普通层 `get_text` 报
`STATE`、改样式只动该层、样式变更一步撤销、像素在栅格化前只读、非法参数无副作用、
`.comp` 往返仍可编辑、**无 `engineText` 键的旧工程逐字节照读**）；UI 新增 1 例
`ui_core_layer_menu_rasterize_text_needs_text_layer`。单测 **631 → 640 例 / 0 失败**；
`run_regression.py --require-ohos` **6/6**；`build_windows_shell.py` 零警告；
`check_tool_table.py` PASS（20 工具）；壳自检 **257 项 failures=0、SHA256
`377DE660D48AC933…` 与基线逐字节一致**（本批没有改动任何既有自检路径，基线未重新生成）；
`visual_check.py` **122 → 128 条 / 23 → 24 场景 / 0 失败**——新场景 `text_layer` 6 条，
只追加在 `SCENES` 末尾，既有阈值与命令 id 一个未改（首次全量复跑时 `options_bar`
曾因物理指针停在画布内误报 17 px，属探针与环境的耦合，已在 §6.85 修掉）。

**边界（如实记）。** ① 没有字体枚举/选择（只有「随仓字体 ↔ 5×7 点阵」，加字体要 P1-4）；
② 没有段落框、对齐、竖排、变形；③ 没有逐字符光标，框内**点哪儿就以哪儿**为新的左上角
重排（不是上游的「在光标处插入」）；④ 每次改字号整层重渲，大文档有可见延迟（没有
「按需栅格化」的省内存优化）；⑤ 文字层上不能落笔、不能抠图，必须先栅格化——这是与上游
对齐的闸，不是缺陷。目视项见 `docs/manual-test.md` **M50**，设计记录见
`docs/shell-parity.md` §5.51。

### 6.85 视觉探针加固：物理指针停到状态栏（2026-10-23，Claude）

**问题。** 全量跑 `visual_check.py --scene all` 时 `options_bar` 的「Choice 点击不
穿透到画布」连续失败，报画布变化 17 px；单跑该场景必过。

**根因。** 壳在最后一次 `WM_MOUSEMOVE` 的坐标上画笔尖虚线轮廓，而探针用
`PostMessage` 合成鼠标事件、不移动真实指针。真实指针停在画布内时，窗口激活会让系统
补一条真实 `WM_MOUSEMOVE`，把合成坐标顶掉，截图上多出一个半径 5 px 的笔尖圈；点完
选项栏后圈消失，被「画布逐字节不变」的断言算成穿透。这是探针与物理指针位置的耦合，
不是壳的缺陷。

**修法。** 探针 `Shell.start()` 里新增 `_park_pointer()`：`SetCursorPos` 把物理指针
停到状态栏中央（画布外、无悬停高亮），等 0.15 s 收完真实鼠标事件。**壳与引擎零改动**，
场景列表、阈值、命令 id 未动。

**验证。** 修前两次全量跑均 128 项 1 失败（同一条）；修后**连续两次全量跑 128 条 /
24 场景 / 0 失败**，其余断言实测值不变。回归 6/6、壳自检 257 项 SHA 未变。细节见
`docs/shell-parity.md` §5.52。

**顺带（同一轮清掉的告警）。** `tests/test_tool_ops.cpp` 里 `blue.brush.color[3] = 255;`
是写在 `uint8_t color[3]` 之外的越界写，编译器的 `-Warray-bounds` 一直在报，只是没人看。
删掉该行（笔刷颜色本来就没有 alpha）。删后重跑 `build.py` **640 例 / 0 失败、零告警**，
`run_regression.py --require-ohos` **6/6**（`^COMP_API` 计数仍为 **150**，
`check_tool_table.py` PASS 20 工具）。

### 6.86 对标 Photoshop 的完成度：日常路径 ≈70%，全功能面 ≈50%（2026-10-23，Claude）

§6.4 的口径 B 以**上游 Compositor 的 48 条**为分母，用它回答「能当 PS 用吗」会系统性
高估（BACKLOG §8 已记这条漂移）。这一节改用 Photoshop 自己的功能面做分母，重新计分，
完整表格与逐条证据见 [`docs/ps-parity.md`](docs/ps-parity.md)。

**四个口径，四个数字，不能混用：**

| 口径 | 分母 | 数字 |
|------|------|------|
| A 引擎核 | 契约面 / 像素保真 / 跨端交付 | **≈97%**（沿用 §6.4，未变） |
| B 上游功能条目 | 上游 Compositor README 48 条 | **≈60%**（29/48，沿用 §6.4） |
| C PS 日常使用路径 | 按使用频率加权的九维 | **≈70%** |
| D PS 全功能面 | PS 核心功能条目 190 条 | **≈50%**（加权，原始命中 86/190） |

C 与 D 差 20 个点属预期：PS 的功能条目长尾极重（滤镜 100+、调整 20+、色彩管理、打印、
自动化、3D/视频），条目数大但日常频率低；本仓在高频主干上的覆盖好于条目总数。

**九维加权（口径 D，合计 100%）：** 文档与文件 45%、图层与合成 70%、选区 50%、
绘制与修饰 60%、文字/形状/渐变 38%、调整与滤镜 36%、变换与画布 55%、视图/界面 45%、
色彩/输出/自动化 20%。算术 `0.15×45 + 0.20×70 + 0.10×50 + 0.15×60 + 0.10×38 +
0.15×36 + 0.05×55 + 0.05×45 + 0.05×20 = 49.95 ≈ 50%`。

**能抬高日常得分最快的四件事（按 ROI）：** ① 字体枚举与选择器（文字维 55→75%）；
② 锐化/降噪入口（调整滤镜维 55→70%）；③ 标尺/参考线/网格/对齐（视图维 60→80%）；
④ PSD 分层导出（文档维 70→85%）。做完这四项，日常路径约到 **85%**。

**本轮验证基线（实跑）：** `build.py` **640 例 / 0 失败、零告警**；
`run_regression.py --require-ohos` **6/6**；壳自检 **257 项 failures=0**、SHA256
`377de660d48ac933…` 与基线一致；OHOS `compile ok=114 fail=0`；`^COMP_API` **150**；
`check_tool_table.py` PASS（20 工具）；`visual_check.py --scene all`
**128 条 / 24 场景 / 0 失败**。

### 6.87 PSD 分层读写：自研常用子集编解码 + 组混合模式落地（2026-10-23，Claude）

**为什么换掉 stb 的 PSD 路径。** `codec_lib.cpp` 里 PSD 走 stb，而 stb 的 PSD
通道**只解合成结果**：图层名、偏移、不透明度、蒙版、组、剪贴全部丢掉。对一个
主打「打开 PSD、改完再存回去」的产品，这等于用户每保存一次就销毁一次分层工程。
所以本轮要的是**分层容器**，不是一张图：树内自研 `format/psd_codec`，不引入第三方
PSD 库（vendoring 一个完整的 PSD 库会同时引入它自己的格式解释与近似取舍）。

**交付物。**

| 项 | 内容 |
|---|---|
| `engine/format/psd_codec.{h,cpp}` | 分层读写器。读：8 / 16 位 RGB(A)，raw(0) / RLE(1)，图层记录（名字、可见性、不透明度、偏移、剪贴标志）、图层蒙版（-2）、组（`lsct`）；写：同一子集，RLE 通道 + 16 位 `Lr16` 附加块 |
| `engine/abi/src/comp_io_abi.cpp` | 新 ABI 只做**纯尾追加**：`comp_io_import_document` / `comp_io_export_document`，`^COMP_API` 150 → **152**；`comp_io_codec_support(PSD)` 维持 `COMP_CODEC_VENDORED` + 非空 note。`comp_io_export`（单张合成结果）对 PSD 仍如实 `COMP_ERR_UNSUPPORTED`，分层导出走新入口。**当时登记表的 note 仍写着 `READ-ONLY: ... export unsupported`**（描述的是老的拍平路径）——这是不实陈述，本轮未改，下一轮 §6.88 改正 |
| `engine/format/comp_file.cpp` | 组混合模式与穿透落进 `.comp`：`engineBlendMode` / `engineIsolated`。**默认值不落盘**（旧工程 manifest 逐字节不变），`blendMode` 仍写 Normal 保持上游兼容 |
| `scripts/crosscheck_psd.py` | 独立交叉验证：由 psd-tools 现场生成参考 PSD，再让引擎走与宿主同一条 C ABI 导入/导出 |
| `shells/windows/src/shell_document.cpp` | **壳接线（M1.5）**：打开按扩展名分派（`.psd` → `comp_io_import_document`，其余 → `comp_io_import`）；「文件 > 导出为」过滤器加 PSD，`.psd` → `comp_io_export_document`。分层读失败**不回落**拍平，具名原因进状态栏。壳自检 257 项 / SHA256 与基线逐字节一致——因为**当时这条分派没有任何自检覆盖**（GUI 入口被模态对话框挡着），下一轮 §6.88 补齐 |

**能力边界（超出即拒绝，绝不静默近似）。** 拒绝清单：PSB（版本 2）、32 位、灰度 /
CMYK / 索引色、ZIP / ZIP+predictor 压缩、`diss`/`dkCl`/`lgCl` 混合模式、真实蒙版通道
-3、文字层（`TySh`）、智能对象（`SoLd`/`SoLE`/`PlLd`/`vmsk`/`vsms`）、图层特效
（`lmfx`/`lrFX`/`lfx2`）、调整 / 填充图层（`brit`/`levl`/`curv`/…）。拒绝时 `error`
写明是哪一条，不是「跳过这一层继续」。

**交叉验证（`scripts/crosscheck_psd.py`，psd-tools 1.26.0）：67 项断言全过。**
参考文件 64×48 / 8 位、4 层（背景、剪贴层、MULTIPLY 组、组内 MULTIPLY + 不透明度
180 + 按列渐变蒙版）：

| 断言 | 结果 |
|---|---|
| 导入：图层树（名字 / 包围盒 / 混合 / 不透明度 / 可见性 / 剪贴 / 蒙版尺寸） | 与 psd-tools 全等 |
| 导出后再由 psd-tools 读：图层树 + 文件头（尺寸 / 深度 / 色彩模式） | 与原始文件全等 |
| 蒙版像素（原始 vs 导出） | 逐字节相同（maxdiff=0） |
| 图层像素（容差 ≤1） | maxdiff=0 |
| 引擎 flatten vs psd-tools composite（原始） | max=1，mean=0.0024，>2 的像素 0/12288 |
| 引擎 flatten vs psd-tools composite（导出文件） | max=0 |
| 原始 vs 导出的引擎合成结果 | max=1 |
| 16 位：ABI 建 RGBA16 图层 → 导出 → psd-tools | `depth=16`，像素值与 `>>8` 一致 |

**顺带修掉的两个既有缺陷**（都由上面的交叉验证暴露，且影响范围超出 PSD）：

1. **剪贴栈把成员当成不透明黑，且覆盖度乘两遍**（`compositing/filter_graph.cpp`）。
   栈成员混合时「额外不透明度」写死成 `1.0`，成员缓冲里的透明像素于是以不透明黑
   混进栈表面——基底 alpha 还在、RGB 在成员范围外被涂黑；修掉这一半后，成员又
   因为 `clipping_source` 同时被 `blend_layer_into` 和栈节点各乘一次而掉色
   （半透明基底 + 不透明绿成员得到 `(64,64,0,128)`，应为 `(0,128,0,128)`）。
   修法：成员 alpha 取自像素本身并跳过全透明像素；新增
   `FilterNode::clipping_consumed_by_stack`，求值时用「清掉 `clipping_source` 的
   图层副本」，与 `composite_layer` 的口径一致。**这是通用合成缺陷，不止 PSD
   文档会踩**——`tests/test_filter_graph.cpp` 原有两条剪贴用例都用全画布基底，
   永远覆盖不到「范围之外」，本轮补了 3 条：成员外基底可见、成员 alpha 取自
   像素、以及同样两条语义在 half 表面上的分支。
2. **导出的组记录写 0 个通道**（`format/psd_codec.cpp:write_empty_record`）。
   本仓解码器宽容，第三方不认：psd-tools 合并图层时抛
   `Channels mismatch: expected 3 != given 0`，整棵图层结构被跳过——导出的 PSD
   在 Photoshop / psd-tools 里背景凭空消失。改为与 Photoshop 一致的 4 个空通道
   （`-1/0/1/2`，每个带「压缩=0、长度=0」的 2 字节块）。

**本轮验证基线（2026-10-23 实跑）：**

| 项 | 结果 |
|---|---|
| `scripts/build.py` | **654 例 / 0 失败**（新增 3 条剪贴语义用例） |
| `scripts/run_regression.py --require-ohos` | **6/6 PASS** |
| 壳自检 | **257 项 failures=0**，SHA256 `377de660d48ac933…` 与基线一致（§6.88 之后为 **263 项 / `2f3b81c4…`**） |
| OHOS 交叉编译 | `compile ok=115 fail=0`（较上轮 +1，即新增的 `psd_codec.cpp`） |
| `^COMP_API` 导出 | **152**（150 + 2，纯尾追加） |
| `scripts/check_tool_table.py` | PASS（20 工具） |
| `scripts/visual_check.py --scene all` | **128 条 / 24 场景 / 0 失败** |
| `scripts/crosscheck_psd.py` | **67 项断言 / 0 失败** |

已知边界：写入侧目前只产出 8 / 16 位 PSD（浮点合成结果显式拒绝，不悄悄截断）；
WebP 仍未接入（只读导入留作 M1 的可选项）。

### 6.88 修正 PSD 能力描述不实 + 补齐分层入口自检（2026-10-23，Claude）

上一轮（§6.87）交付了分层读写，但留了两个尾巴：**登记表对 PSD 说了假话**，**新入口没有自检**。
两个都是「引擎能做的事，宿主/回归看不见」——本轮一并收掉，作为**一次**基线变更。

**问题一：note 仍写 `READ-ONLY`。** `codec_registry.cpp` 的 PSD 条目停在 M1 之前：
`...; READ-ONLY: import supported, export unsupported`。而 M1 之后分层读写早已可用。
要命的地方在于**宿主唯一能看到的证据就是这条 note**——`comp_io_codec_support` 只回
`state` + `note`，`can_read` / `can_write` 两个 bool 根本不导出。note 说「导出不支持」，
宿主就会把「分层可写」报成「整个格式不能写」。

修法三步，**不动 ABI、不动 bool**：

| 改动 | 内容 |
|---|---|
| `engine/format/codec_registry.cpp` | PSD note 重写：`vendored real library (...) for flattened read; layered read/write via comp_io_import_document / comp_io_export_document (...); flattened comp_io_export is unsupported - use the document entry points`。两条通道与各自的边界都在里面 |
| `engine/format/codec_registry.h` | 把 `can_read` / `can_write` 的语义钉死为**栅格（拍平）通道**，并写明「分层能力只能写进 note」的原因 |
| `engine/abi/src/comp_io_abi.cpp` | `unsupported_write_reason` 加 `flattened`：`PSD flattened export is unsupported: <note>`。用户不会再被告知「整个 PSD 不能写」 |

**刻意没有把 `can_write` 改成 true。** 拍平 `comp_io_export` 对 PSD 确实写不出来
（`tests/test_codec.cpp` 就在断言这件事）；改成 true 只会让 `unsupported_write_reason`
落到 `unsupported_reason()`，报出 `PSD is not available in this build` —— 比原来更假。
**缺陷在文案，不在能力位。**

**问题二：分层分派没有自检。** `.psd` → `comp_io_import_document` 的分派写在
`open_path_as_document` 里，而这条路径被 GUI 的模态对话框挡着，自动化点不进去；
上一轮只能说「壳自检 257 项不变」，那**不是证据，是没有覆盖**。分派一旦退化成
`comp_io_import`，用户看到的是一张「打开了、但只剩一层」的图——不报错，数据已经丢了。

自检做法（`shell_selftest.cpp` 尾部追加，只加不改）：临时目录里建 14×12 两层不透明
RGBA8 文档（预乘与「像素尺寸 = transform 尺寸」两条 ABI 契约都满足），导出 → 回读 →
再走壳的 `open_path_as_document`。**判据取图层数**：同一个两层 PSD，拍平路径读回来永远
是 1 层。这个差异可判定，且不依赖像素容差。新增 6 项：

```
[OK] PSD layered fixture builds (2 layers)
[OK] comp_io_export_document(PSD) writes a layered file
[OK] comp_io_import_document(PSD) round trip keeps both layers
[OK] flattened comp_io_export(PSD) is still rejected
[OK] shell open_path_as_document(.psd) takes the layered path
[OK] shell open_path_as_document(.png) still opens one flattened layer
```

最后一项是**反向护栏**：分派不能为了 PSD 把普通位图也改道（PNG 仍须是 1 层、240×160）。

**基线变更（附录式，需批准）。** `compositor_shell_selftest.txt` **257 项 → 263 项**，
SHA256 `377de660d48ac933…` → `2f3b81c4fc599cb7…`。diff 仅一处：`[summary]` 之前插入
1 个空行 + 6 个 `[OK]`，其余 270 行逐字节不变。新增项只输出固定标签，不含临时路径，
因此基线仍可逐字节复现。

**本轮验证基线（2026-10-23 实跑）：**

| 项 | 结果 |
|---|---|
| `scripts/build.py --clean` | **654 例 / 0 失败** |
| `scripts/run_regression.py --require-ohos` | **6/6 PASS** |
| 壳自检 | **263 项 / failures=0**，SHA256 `2f3b81c4fc599cb7…` == 新基线 |
| OHOS 交叉编译 | `compile ok=115 fail=0` + NAPI 壳 rc=0 |
| `^COMP_API` 导出 | **152**（未变；本轮零 ABI 变更） |
| `scripts/crosscheck_psd.py` | **67 项断言 / 0 失败** |
| `scripts/visual_check.py --scene all` | **128 条 / 24 场景 / 0 失败** |
| `scripts/check_tool_table.py` | PASS（20 工具） |

顺带把 `docs/codec-migration.md` 里同样过时的 PSD / EXR 口径改掉：该文档 §1 表格与失败
语义段仍写着「PSD 只读」「EXR 不可用」，并在顶部标注第 2–5 节是 M5 当时的快照
（83 cases / 27 项自检），不可当当前基线引用。

### 6.89 字体枚举与选择器：系统字体列表 + 分片扫描 + 应用到文字图层（P0-6 ②，2026-10-23，Claude）

`docs/ps-parity.md` §4 的 ROI 第 1 顺位。日常路径里最刺眼的一格是文字维：能打字、能改
字、能存盘，**但选不了字体**——随包只有一支 `NotoSansSC-Regular-Subset.ttf`。

问题不在引擎能力。字体 ABI 早就齐了：`comp_font_load_file` 能读任意 TTF/OTF/TTC
（含 `.ttc` 面号），`comp_font_get_metrics` 给 `face_count` / `scalable` /
`units_per_em` / `has_cjk`，`comp_font_name` 给族名与样式名，`comp_font_release`
归还。缺的是**「把系统里的字体列出来让用户挑」这条产品通路**。

**本轮 ABI 零新增**（`^COMP_API` 仍 **152**）。枚举放在壳侧，因为「字体装在哪个目录、
认哪些扩展名、per-user 装在哪」是平台知识，引擎不该知道；像素与排版仍然全部回引擎。

| 文件 | 改动 |
|---|---|
| `shells/windows/src/shell_fonts.cpp`（新） | 目录枚举、分片扫描、过滤、选择、应用 |
| `shells/windows/src/ui_core.h` | 字体列表的行几何 / 滚动 / 命中 / 标签匹配纯函数 + 面板常量 |
| `shells/windows/src/shell_state.h` | `ShellFontEntry` / `FontCandidate` 与扫描状态（放在会话态，换文档不丢） |
| `shells/windows/src/shell_paint.cpp` | `draw_font_panel`；右栏绘制分派插在历史面板之后 |
| `shells/windows/src/shell_window.cpp` | 键鼠 / 滚轮 / `WM_CHAR` / `WM_TIMER` 接线，菜单 `图层 > 字体…(O)` |
| `shells/windows/src/shell_document.cpp` | 菜单命令分派 |
| `shells/windows/src/shell_type.cpp` | 文字属性面板与字体面板互斥（同一块右栏） |
| `tests/test_ui_core.cpp` | 新增 2 例：行几何 / 滚动 / 命中、标签过滤匹配 |

**扫描：分片，且句柄立刻归还。** 候选按「随仓字体 → `%WINDIR%\Fonts` →
`%LOCALAPPDATA%\Microsoft\Windows\Fonts`」收集，只认 `.ttf/.otf/.ttc`，路径小写去重
（Windows 大小写不敏感）。`comp_font` 没有全局缓存，每个面都是「打开 → 读名字与度量 →
立即 release」，只留字符串——否则常驻几十 MB。`units_per_em == 0` 的位图字体不进列表
（排不了版）。

同步扫一遍 153 个文件实测约 **1.2 s**，足以把 UI 冻住一截。所以扫描走 `WM_TIMER`
（id 2）：每 30 ms 探 6 个文件，`kFontScanMaxFaces = 8` 限制 `.ttc` 的面数。
`font_scan_start` 同步探第一片，面板打开立刻有内容；其余片在后台补齐，期间可滚、
可应用已探到的条目，状态栏显示 `扫描中 n/m`。

**排序与选中。** 面板顺序是「内置点阵 → 随仓 → 含 CJK → 名字（大小写不敏感）」。
扫描结束时的排序会打乱 `font_filtered` 里存的下标，所以先按身份（`path` +
`face_index` + `builtin`）记下当前选中项，`stable_sort` 后重建下标再定位——否则面板
会在扫描结束时跳到一支无名字体上。

**面板与互斥。** 右栏一次只挂一个面板的既有不变式不改：字体面板打开时先关路径 /
历史 / 裁剪 / 效果 / 滤镜（色彩范围持有显式事务，先 `color_range_cancel`），而
`font_panel_visible` 反过来在那些面板打开时让位（状态保留，关掉别的面板它就回来）。
`type_panel_visible` 同样排除字体面板，避免同一块区域画两层。菜单命令先测
`font_panel_visible` 而不是 `font_open`，所以面板被别的面板挡住时再点菜单是重新露出，
不是把它关掉。

**键盘与鼠标。** `↑/↓` 选行，`PgUp/PgDn` 翻页（按面板实际可见行数 − 1），`Home/End`
跳两端，`Enter` 应用，点击命中的行也应用，滚轮 3 行/格。直接键入字母或中文就是过滤
（`WM_CHAR` 进 `font_filter`），`Backspace` 删字符，`Esc` 先清过滤词、再按一次才关
面板；其他按键在面板可见时一律吞掉——面板是模态的。入口是 `图层 > 字体…(O)` 与文字
属性面板「字体」行上的回车。

**应用语义：不换算字号。** 两条轨道的字号量纲不同（真实字体是 em 高，内置点阵是格子
高），切换时**不改数值**，与 `type_panel_toggle_font` 同一口径——硬换算只会制造一个
没人要的 5.2。真实字体必须**先真的 `comp_font_load_file` 成功**才置 `type_use_font`，
否则用户打完字才在提交时撞上 `COMP_ERR_IO`。

选中图层是文字层时，应用字体会把这一层重排一遍：`comp_layer_get_text` 两段式取出完整
`comp_text_desc`，只替换 `font_path_utf8` / `font_face_index`，再
`comp_layer_set_text` —— **一步撤销、一次重渲**。非文字层不动（字体面板改的是
「下一次打字」的轨道，不该给像素层记一条空撤销步）。切回内置点阵而文本含 CJK 时，
引擎如实拒绝（`kBadText → COMP_ERR_INVALID_ARGUMENT`），状态栏照实写「内置 5×7 点阵
只排 ASCII；含中文请选真实字体」，不做静默替换。

**新增测试（`tests/test_ui_core.cpp`，+2 例）。** 一例钉行几何与滚动：
`kFontRowHeight = 22`、`kFontRowsTop = 80`、footer 88，可视行数公式、
`font_clamp_scroll` 上下界、`font_ensure_visible`、末行不压 footer、`font_row_at` 的
表头 / footer / 越界 / 空列表都返回 −1。另一例钉标签过滤：ASCII 大小写不敏感子串
（`ans`、`sc`）、CJK 子串（`思源`、`黑体`）、不匹配（`宋体`）、空格敏感
（`思源 黑体` 不匹配带空格两侧的标签），以及内置条目能被 `ascii` 命中。

**视觉探针 `font_picker`（4 条，追加在 `SCENES` 末尾）。** 只加场景，不改既有阈值或
命令 id。不同机器装的字体必然不同，所以断言只覆盖面板的「出现 / 重绘 / 仍在 / 消失」，
不做字体名或列表内容的像素断言：

| # | 断言 | 实测 |
|---|------|------|
| 1 | 菜单开启后字体面板替换右栏 | 右栏变化 30375 px |
| 2 | 键入过滤词后列表重绘（空结果显示无匹配提示） | 面板变化 14831 px |
| 3 | 第一次 Esc 只清过滤词，面板仍在 | 相对打开前 34889 px |
| 4 | 第二次 Esc 关闭后回到文字属性面板 | 与打开前差异 **0** px |

第 4 条是逐像素相等：关闭路径把右栏完整还原，没有残留。加上本场景后全量为
**132 条 / 25 场景 / 0 失败**（连跑两次一致）。

**本轮验证基线（2026-10-23 实跑）：**

| 项 | 结果 |
|---|---|
| `scripts/build.py --clean` | **656 例 / 0 失败**（新增 2 条字体面板用例） |
| `scripts/run_regression.py --require-ohos` | **6/6 PASS**（656 例、壳自检、编解码、`.comp` 双向、共享构建、OHOS aarch64） |
| 壳自检 | **263 项 / failures=0**，SHA256 `2f3b81c4fc599cb7…` == 基线（本轮未改自检路径） |
| OHOS 交叉编译 | `compile ok=115 fail=0` + NAPI 壳 rc=0 |
| `^COMP_API` 导出 | **152**（未变；本轮零 ABI 变更） |
| `scripts/crosscheck_psd.py` | **67 项断言 / 0 失败** |
| `scripts/visual_check.py --scene all` | **132 条 / 25 场景 / 0 失败**（只追加 `font_picker`） |
| `scripts/check_tool_table.py` | PASS（20 工具） |

**已知边界（不算本轮缺陷，但要说清）：**

- **没有回退链**：选中字体缺字时仍是引擎现状（单面排字），不会自动找同族补字。
  回退链仍在 BACKLOG P1-4 的未完成侧。
- `.comp` 里存的是**绝对 Windows 路径**（`comp_text_desc.font_path_utf8`），跨设备 /
  换系统打开时同名字体在原路径不存在就回落到内置点阵；可移植性需要「字体身份 +
  本机解析」两层，属后续。
- 只扫两个字体目录，不读注册表：「已注册但没有文件落在目录里」的字体看不到。
- `.ttc` 只列前 8 个面。

详见 `docs/shell-parity.md` §5.53。










