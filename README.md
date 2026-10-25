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
  全量 **787 用例 0 失败**；Windows 外壳实机自检 **267 项全 OK**（与仓内基线逐字节一致）。
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
| 交换格式键名 | 完整对齐上游 `ProjectManifest` / `ProjectLayerRecord`：`format`/`version`(13)/`supported`/`colorSpace`/`resolution`/`documentID`/`width`/`height`/`activeLayerID`/`layers`；图层 id 与 `parentID`/`maskSourceID` 用 UUID，`transform` 为六键形态（`origin`/`size`/`rotation`/`flipX`/`flipY`/`sampling`）。引擎自有键一律 **`engine` 前缀**（`engineLayerID`/`enginePixelFormat`/`engineClippingSourceID`/`engineAdjustments`/`engineEffects`/`engineIsolated`/`engineGuides`），避免与上游对象型键（`effects`/`text`/`shape`/`adjustment`/`guides`）同名冲突 | 上游未建模字段（live-mask 启用态等）按原样保真；参考线写在本引擎的 `engineGuides`（§6.91），上游同名 `guides` 不读不写 |
| 编解码 | **PNG 与 JPEG 已接入真实第三方库并真实可用**：`comp_io_export`（拍平 → 落盘）与 `comp_io_import` / `comp_io_import_layer`（文件 → 文档 / 追加图层）已打通；PNG 走 libpng + zlib、JPEG 走 stb（有损，`{"quality":N}`）、PSD 走树内自研的分层读写器 `format/psd_codec`（§6.87：`comp_io_import_document` / `comp_io_export_document` 分层往返；拍平的 `comp_io_export` 仍不支持 PSD）。**HDR 来源（C3，§6.1.17）**：Radiance `.hdr`/`.pic` 走 stb 的**浮点通道**解出线性光，按 1.001 闸判成 HDR(RGBA16F) 或 16 位图层；浮点格式**结构上不许**走 8 位通道。**EXR 已由树内自研 scanline 读取器接入**（C3b，§6.1.18：NONE/RLE/ZIPS/ZIP 可读，PIZ/PXR24/B44/DWA 与 tiled/deep/multipart 如实拒绝，写侧仍 `UNSUPPORTED`）；32 位 float TIFF **仍无解码器**（树内 stb 无 TIFF）。第三方源码 vendoring 到 `engine/third_party/`，`__has_include` 探测缺库时如实回落。`.comp` **内部**像素仍走自研零依赖 `comp_png`（工程包可移植的前提）。**能力边界（P2-4）**：宿主构建走 libpng+zlib，而 `scripts/build_ohos.sh` 的包含路径不含 `engine/third_party/*`，故鸿蒙交叉编译时 `__has_include("png.h")` 探测失败、PNG 会**如实回落**到自研 `comp_png` —— 本行「真实第三方库」的表述**仅对宿主构建成立**，鸿蒙侧是同语义的另一条实现。其余格式集中在 `format/codec_registry` 登记，未链接实现者如实 `UNSUPPORTED` 并给出原因 | 按 `docs/codec-migration.md` 第 6 节继续接入 TIFF / libheif / OpenEXR / lcms2；只改 `codec_lib` 与登记表 |
| 调整图层参数 | 面板控件与 JSON **都由引擎元数据驱动**：键名表 `engine/domain/adjustment_table.cpp` 是唯一真源（16 种 kind：前 12 种对齐上游枚举，后 4 种为本仓追加的 Brightness/Contrast、Desaturate、Threshold、Posterize；点分 `json_path` 逐字沿用上游字段名），外壳经 ABI 取 `label` / `min` / `max` / `default`；settings JSON 由 `comp_layer_adjustment_build_settings` 组装（**壳内不拼、不解析 JSON**）；写入口 `add` / `update` / `update_at` 一律校验，`.comp` 读取路径不校验以保真 | Curves / Levels 需专用曲线编辑器（当前 `editable=false`，仅白名单放行）；删除接口 `comp_layer_remove_adjustment` 已补（面板 `Ctrl+Shift+D`） |

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

10. **调整 kind 也只在尾部追加**：`comp_adjustment_kind` 的 0..11 与上游
    `AdjustmentKind` 逐字对齐并冻结，本仓追加的四项（`BRIGHTNESS_CONTRAST = 12`、
    `DESATURATE = 13`、`THRESHOLD = 14`、`POSTERIZE = 15`）排在最后，
    `COMP_ADJUSTMENT_KIND_COUNT` 随之 12 → 16，既有编号一个未动；
    破坏性落墨入口 `comp_layer_apply_adjustment` 同样是纯函数尾部追加（163 → 164），
    不改任何结构体布局。外部调用方按 `KIND_COUNT` 分配数组的代码只需重新编译。

不透明句柄的结构体 tag 名刻意与 typedef 名不同（`comp_engine_handle` / `comp_document_handle` /
`comp_txn_handle`）：在 C++ 下 `typedef struct comp_engine* comp_engine;` 会与注入普通命名空间的
类名冲突（clang 报 `typedef redefinition with different types`）。tag 名不属于 ABI，调用方只使用
typedef，故对契约零影响。

## 6. 验证记录 → 已迁出

全部验证记录（§6.1 ~ §6.117，110+ 个交付小节）自 2026-10-26 起移至
[VERIFICATION.md](VERIFICATION.md)，编号不变。README 只保留当前基线速览：

| 项 | 当前值 |
|---|---|
| ABI 导出 | 212（tag `abi-212`） |
| 单元测试 | 787 例 0 失败（build.py 全量） |
| 壳自检 | 267 项，SHA 基线 `cae180809eed3ba4` |
| 鸿蒙 | 模拟器 783/0 + ASan 0 报告；HAP 端到端（上屏 rc=0 + 触摸画笔） |

## 7. 许可

上游 `robbietilton/Compositor` 为 MIT 许可；本仓沿用 MIT，并保留上游版权声明。
`engine/imaging/` 下的像素内核移植自上游 C 源码，文件头保留原始注释。

