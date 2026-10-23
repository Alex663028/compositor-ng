# BACKLOG —— 以 Photoshop 正常使用为基准的功能缺口

> 基线：`cbbbec2`（2026-10-22）。范围：**功能面**（用户能做什么），不含性能优化、重构与文档债（文档漂移见 §8）。
> 方法：按「一个 Photoshop 用户从新建文档到交付成片」的实际路径逐段对照，只记录**本仓当前确实做不到**的事，
> 每条给出现状证据、缺口性质与验收口径。允许的例外（ML / 上游未做）单列 §5，不与缺口混计。
> 硬约束：ABI 导出只能**纯尾部追加**（当前 **152**）；回归 6/6、壳自检 263 项 SHA、视觉探针不得因新增功能而破（当前 **128 条 / 24 场景**；落地规则见 §6）。
> 自检基线：**263 项 / SHA256 `2f3b81c4fc599cb7…`**（原 257 项 / `377de660d48ac933…`；2026-10-23 一次**显式审批**的
> 附录式变更：`[summary]` 前插入 1 空行 + 6 项 PSD 分层入口断言，其余逐字节未动，见 README §6.88）。
> **2026-10-22 第一批落地**（Claude，按 §7 阶段 A 往前推）：P0-1 新建对话框、P0-2 图像/画布大小、
> P0-3 另存为 + 标题脏标记、P1-8 历史容量入口。四项**全部零 ABI 新增**（仍 145），
> 视觉探针 80 → **86 条**（只追加 `doc_dialog` 场景，既有阈值未动），其余断言见 §2 各条。
> **同日第二批**：P1-5 Camera Raw 滤镜入口（零 ABI 新增），视觉探针 86 → **91 条**
> （只追加 `camera_raw` 场景）。
> **同日第三批**：P0-4 剪贴板（复制 / 合并拷贝 / 剪切 / 粘贴 / 贴入选区 / 清除），
> **唯一一处 ABI 尾部追加**（`comp_layer_get_pixels`，145 → **146**，仅用于把图层原始像素读出来做
> 逐字节拷贝），视觉探针 91 → **97 条**（只追加 `clipboard` 场景）。
> **同日第四批**：P0-3 关闭 / 退出前的未保存提示（**面板内**三选一，零 ABI 新增），
> 视觉探针 97 → **102 条**（只追加 `close_prompt` 场景）。
> **同日第五批**：P0-10 工具选项栏（工具参数从右栏搬到工具条下，零 ABI 新增），
> 视觉探针 102 → **115 条 / 22 场景**（只追加 `options_bar` 场景）；DPI 与浮动面板仍未做。
> **同日第六批**：P0-5 选区修改补全（边界 / 平滑，**只追加两个枚举值**、导出数仍 146），
> 视觉探针 115 → **122 条 / 23 场景**（只追加 `selection_modify` 场景）；Grow/Similar 与变换选区仍未做。
> **2026-10-23 第七批**：外壳拆分（`main.cpp` 1.9 万行 → 13 个翻译单元 + `ShellState` 文档上下文对象化，
> 零行为改动，见 README §6.83）与 P0-6 ① 可编辑文字图层（**尾部追加 4 个文字导出，146 → 150**，
> 视觉探针 122 → **128 条 / 24 场景**，只追加 `text_layer` 场景）。同日顺带：§8 文档漂移校正一轮、
> 仓库根目录清理、视觉探针 `_park_pointer()` 加固（见 shell-parity §5.52）。字体枚举、段落框仍未做。

标签说明（用于排期与判断"能不能只改外壳"）：

| 标签 | 含义 | 典型成本 |
|------|------|----------|
| 【接线】 | 引擎 / ABI 已有能力，外壳没有入口或 UI 没接 | S（≤0.5 天） |
| 【壳层】 | 用现有 ABI 在 Windows 壳内即可完成 | S–M |
| 【ABI 追加】 | 需要新增导出、枚举值或结构体尾字段（纯追加） | M |
| 【引擎】 | 需要新的领域模型或像素实现 | M–XL |
| 【架构】 | 需要改外壳结构（多文档、面板系统、输入栈） | L–XL |

---

## 1. 现状快照

| 维度 | 本仓 | Photoshop（参照） | 差距性质 |
|------|------|-------------------|----------|
| C ABI | **152** 个导出（`^COMP_API` 行计数，2026-10-23 实测） | — | 契约面稳定 |
| 工具 | **20**（`ui_core.h` `tool_table()`） | 工具栏 60+（含嵌套变体） | 引擎算子多、工具少 |
| 混合模式 | **24**（`comp_core.h:126-150`） | 27 + 组「穿透」 | 缺 Dissolve / Darker Color / Lighter Color；组混合与隔离已可往返（`comp_file.cpp:397-410` 的 `engineIsolated`/`engineBlendMode`） |
| 调整种类 | **12**（`comp_layer.h:300-313`） | 调整菜单 20+ | 只有调整**图层**，没有破坏性 Image > Adjustments（除反相） |
| 图层样式 | **6**（描边/投影/颜色叠加/内阴影/外发光/内发光） | 10 | 缺斜面和浮雕 / 缎光 / 渐变叠加 / 图案叠加 |
| 滤镜 | **9**（`comp_filter.h:38-48`） | 100+ | 无锐化 / 降噪 / 模糊画廊 / 扭曲 / 像素化 / 风格化 / 渲染 |
| 可用格式 | 导入 PNG/JPEG/PSD/HDR/EXR；导出 PNG/JPEG/PSD/EXR + `.comp` | 20+ | PSD 只是常用子集（8/16 位 RGB(A)；PSB/CMYK/32 位/智能对象/文字/效果具名拒绝）；无 TIFF、WebP、HEIF、PDF、SVG、RAW（`docs/format-import-matrix.md:44-51`） |
| 文档 | **单文档**、单个 `ShellState::document` | 多标签 | 新建会直接销毁旧文档（`shell_document.cpp:255/313`） |
| 面板 | 10 个（图层/调整/曲线/裁剪/滤镜/效果/色彩范围/路径/历史/文字） | 15+ | 无通道 / 导航器 / 色板 / 属性 / 动作 |
| 菜单 | 9 个顶层菜单 | 11 | 无「视图 > 标尺/参考线/网格」、无「窗口」 |
| 色彩 | 仅 sRGB；`.comp` 拒绝其它色彩空间（`comp_file.cpp:653-656`） | RGB/CMYK/Lab/灰度/位图/索引 + ICC | 无 `Image > Mode`、无 8/16 位文档模式、无色彩管理 |
| 位深 | 图层可以是 RGBA16/16F；**文档合成模式没有开关** | 8/16/32 位文档 | `comp_document_desc` 只有 width/height/dpi/color_space（`comp_engine.h:56-61`） |
| 视图辅助 | 无标尺、无参考线、无网格、无对齐 | 全套 | 全仓 0 命中 |
| 剪贴板 | 复制/合并拷贝/剪切/粘贴/贴入选区/清除（自定义格式 + CF_DIBV5） | 全套 | 已可用；跨应用仅 32 位 DIB，无色彩管理 |
| 命令表 | `comp_command_*` 仅 **7** 条（`comp_command_abi.cpp:30-39`） | 动作/脚本可录制全部命令 | 自动化没有地基 |

数字不是完成度：README §6.4 的口径 A（引擎核）≈97%、口径 B（产品功能面）≈60% 仍然成立——
**像素保真度接近满配，缺的是把能力做成用户路径**。

**对标 Photoshop 的计分见 [`docs/ps-parity.md`](docs/ps-parity.md)（2026-10-23）**：
日常使用路径 **≈70%**，全功能面 **≈50%**（190 条 PS 核心条目加权，原始命中 86 条）；
与口径 A/B 的差异原因在该文件 §3 说明。按 ROI 补「字体选择 → 锐化/降噪入口 →
标尺参考线 → PSD 分层导出」四项，日常路径约到 85%。

---

## 2. P0 —— 不做这些，"正常使用 PS" 第一天就断

### P0-1 新建文档没有对话框：尺寸 / 分辨率 / 背景 / 预设全固定
- **现状**：`create_document()` 固定 `kDefaultWidth/kDefaultHeight`、`dpi=72`、`sRGB`、无背景选项（`main.cpp:8153-8160`）。
- **PS 行为**：新建对话框给画布尺寸、分辨率、背景（白/透明/背景色）、颜色模式、位深、预设（A4/1080p/照片…）。
- **做法**：引擎侧 `comp_document_desc` 已带 width/height/dpi/color_space → 【接线】先做尺寸/分辨率/背景；位深需在 `comp_document_desc` **尾部追加** `pixel_format`（纯追加，宿主重编译）→ 【ABI 追加】。
- **验收**：新建 3000×2000 @300dpi 后 `comp_document_size` + resolution 回读一致；背景为透明时首层 alpha=0。
- 规模：S（不含位深）/ M（含位深）。
- **已落地（2026-10-22，零 ABI 新增）**：`文件 > 新建文档…`（Ctrl+Alt+N）打开右栏模态面板：
  宽 / 高 / 分辨率 / 背景（白 / 透明 / 前景色），Tab 切行、数字键入、回车新建、Esc 取消；
  面板记住上次设置（与 PS 的 Remember Last Used Settings 同效）。`Ctrl+N` 保持**立即新建**
  （1280×720@72 透明），不弹面板——`visual_check.py` 的 view 场景依赖这个行为。
  背景填充走 `comp_layer_set_pixels` 一步写入首层（0 白 / 1 透明 / 2 前景色），
  因此**填了底色即视为已修改**（标题栏出现 ` *`），这是如实反映而非缺陷。
  仍未做：颜色模式、位深、A4/1080p 预设（需 `comp_document_desc` 尾部追加字段，见本条原文）。


### P0-2 图像大小 / 画布大小：ABI 齐了，UI 一条都没有
- **现状**：`comp_document_resize_image`（整篇重采样，`comp_engine.h:94`）与 `comp_document_set_size`（裁画布，`:84`）已实现并有单测，但**外壳零调用**——全仓只有 `tests/` 出现（`grep resize_image` 在壳内 0 命中）；菜单里也没有「图像大小 / 画布大小」（`main.cpp:14859-14866` 只有旋转/修边/反相）。
- **PS 行为**：图像大小（像素/百分比、重采样算法、分辨率、约束比例）；画布大小（锚点九宫格、相对/绝对、扩画布填背景色）。
- **做法**：【接线】做两个对话框。注意两条已记录边界：`resize_image` 会**丢弃选区**、**不缩放效果 JSON 参数**（`comp_engine.h:88-93`）；`set_size` **不做内容偏移**，画布锚点需壳先 `comp_layer_move` 再 `set_size`，或引擎追加一个带 anchor 的入口 【ABI 追加】。
- **验收**：`test_image_size.cpp` 已覆盖引擎语义；UI 走 `scripts/visual_check.py` 新增场景（只追加，不改既有断言）。
- **已落地（2026-10-22，零 ABI 新增）**：`图像 > 图像大小…`（Ctrl+Alt+I：宽 / 高 / 约束比例 /
  百分比，基准取打开时的尺寸所以连续微调不累积取整误差）与 `图像 > 画布大小…`
  （Ctrl+Alt+C：宽 / 高 / 九宫格锚点）。
  - **画布锚点不需要新 ABI**：壳先 `comp_txn_begin`，按锚点算 `dx/dy`，
    用 `comp_layer_list` 枚举**全部**引擎图层（含折叠组的子层，不止面板可见行）
    `get_transform`/`set_transform` 平移，再 `comp_document_set_size`，一次提交 = 一步撤销；
    任何一步失败 `comp_txn_abort`，不留半成品。
  - 尺寸变化后 `rgba`/`framebuffer` 必须按新尺寸重分配（`render_canvas` 直接用
    `shell.width*shell.height` 索引），统一在 `sync_document_buffers` 里只在长度变化时重分配。
  - 已知边界按原文保留：`resize_image` 丢弃选区、不缩放效果参数；`set_size` 裁掉的内容不可恢复
    （面板副标题如实写）。
  - 探针：`doc_dialog` 场景量的是**画布屏幕宽高比**（空透明文档是纯黑，`detect_document_rect`
    可靠）：新建 640×480 → 4:3、图像大小 320×120 → 8:3（顺带证明空格关掉了约束比例）、
    画布大小 520×120 → 13:3、Esc 取消不改尺寸。


### P0-3 文档生命周期：另存为 / 关闭提示 / 最近文件 / 多文档
- **现状**：保存只有一个路径（`kCmdSave`），没有「另存为」；`create_document` 直接 `comp_document_destroy` 旧文档，**未保存修改无提示**（`main.cpp:8166`）；退出同样无 `comp_document_is_dirty` 检查（ABI 已有该查询）；无最近文件列表。
- **PS 行为**：另存为、关闭/退出前「保存/不保存/取消」、最近打开、多标签并行编辑、崩溃恢复。
- **做法**：另存为 / dirty 提示 = 【壳层】S（`comp_document_is_dirty` + `comp_document_save` 新路径）；最近文件 = 【壳层】S（注册表或本地配置）；**多文档 = 【架构】**：`ShellState` 单句柄、面板与选区状态都挂在全局，需要文档上下文对象化。
- **验收**：关闭 dirty 文档必须出现三选一；取消后文档与撤销栈不变。
- **部分落地（2026-10-22）**：
  - **另存为**：`文件 > 另存为…`（Ctrl+Alt+S）走 `GetSaveFileNameW`，成功后把路径记进
    `document_path`；`文件 > 保存`（Ctrl+S）对有路径的文档直接写回，无路径时退化成另存为。
  - **标题栏脏标记**：`update_window_title` 每次 `present()` 读 `comp_document_is_dirty`
    （不是壳自己维护的副本），标题形如 `Compositor (Windows shell) - CPU - 未命名 *`；
    文本先与缓存比较，不变化就不 `SetWindowTextW`（避免每帧刷标题）。
  - **关闭/退出提示（第四批已落地，2026-10-22）**：`WM_CLOSE` 先 `comp_document_is_dirty`，
    脏则打开右栏的 `DocDialogKind::ClosePrompt` 面板（**不用 MessageBox**——自动化 `stop()` 是
    「post WM_CLOSE 后等 5 秒」，阻塞式系统弹窗会把每次目视跑拖成超时，也没法用键盘脚本回答）。
    - 行模型只一行「动作」，三档 = 保存并关闭 / 不保存直接关闭 / 取消（`ui_core.h`
      `kCloseActionSave|Discard|Cancel`，默认停**保存**，回车即安全动作）。
    - 键位：`S` 保存并关闭、`D` 不保存、`Esc` 取消；Tab/↑↓ 切档、←/→ 步进、回车执行仍照旧。
    - 「不关窗除非真存下去」：有路径走 `save_document` 的返回值；无路径先弹「另存为」，
      回来后**再查一次 dirty**（`comp_document_save` 成功内部会 `mark_dirty(false)`），
      仍脏或路径为空就只关面板、窗口留着，状态栏报「未保存（另存为已取消），窗口未关闭」。
    - 取消路径不改文档、不动撤销栈；提示开着时再点一次关闭键等于取消，不重置用户选的那一档。
    - 与既有对话框共享渲染/键盘栈（`draw_doc_dialog`），只给 `ClosePrompt` 换固定键行文案；
      其它文档对话框的关键字仍被吞掉，工具快捷键不会从提示底下穿过去。
  - **仍有边界**：`文件 > 新建` / `文件 > 打开` 依然直接销毁旧文档、不提示（同一批没动，
    属于多文档上下文问题，按原文仍是【架构】）；窗口很窄时右栏宽度塌成 0，提示只剩键盘路径。
  - **多文档的前置已就位（2026-10-23）**：`ShellState` 已拆成「会话态 + `DocumentContext`」
    （189 个文档态字段，换文档 = 整体替换一份 `doc`），main.cpp 也从 19,046 行单文件拆成
    13 个翻译单元（见 README §6.83）。多文档**本身**仍未做：缺的是「标签页 →
    `DocumentContext`」的实例管理与切换流程，`文件 > 新建 / 打开` 依然直接销毁旧文档。
  - 最近文件未做。


### P0-4 剪贴板：复制 / 剪切 / 粘贴 / 贴入 / 清除
- **现状**：全仓无剪贴板代码，ABI 也没有 copy/paste。引擎已有 `comp_render_flatten`（取合成像素）与 `comp_layer_set_pixels`（写图层像素），所以**不必新增 ABI**。
- **PS 行为**：复制（选区内像素）、合并拷贝、剪切、粘贴为新图层、贴入选区、清除。
- **做法**：【壳层】M——`render_flatten` → 按选区 coverage 裁剪 → 转 `CF_DIBV5` 写剪贴板；粘贴读 DIB → `comp_layer_add` + `comp_layer_set_pixels`（一步撤销）。跨应用色彩假设 sRGB（与 P0-9 关联）。
- **验收**：复制→粘贴往返 8 位逐字节相等（含 alpha）；贴入时落点按选区 bounds；无选区时复制整层。
- **已落地（2026-10-22）**：`编辑` 菜单新增 拷贝 Ctrl+C / 合并拷贝 Ctrl+Shift+C / 剪切 Ctrl+X /
  粘贴 Ctrl+V / 贴入选区 Ctrl+Alt+V / 清除 Backspace。Ctrl+C/V 之前会掉进工具快捷键
  （C=裁剪、V=移动），现在在 `handle_key` 里先截住；Ctrl+Shift+V 仍是「切换图层可见性」，
  Del 仍是「删除图层」——不改既有语义，自检 SHA 因此不变。
  - **ABI**：唯一新增是 `comp_layer_get_pixels`（146）。逐字节往返必须拿到**图层原始像素**，
    合成结果做不到（合成会重采样、叠背景、套混合模式）。该导出读的是图层自己的像素缓冲与格式，
    不做色彩转换。
  - **两种剪贴板格式**：自定义 `Compositor Layer Pixels`（`"CGP4"` 魔数 + 宽高/原点/预乘标记 +
    预乘 RGBA8）用于本程序内往返，**逐字节相等**；同时写 `CF_DIBV5`（直通 alpha、自下而上 BGRA、
    `BITMAPV5HEADER` + `BI_BITFIELDS` 标准掩码）供跨应用，读时优先自定义格式、退化到 DIB。
    全零 alpha 的 DIB 按不透明处理（很多老程序不写 alpha 通道）。
  - **拷贝路径**：图层是轴对齐 RGBA8 且 transform 尺寸等于像素尺寸 → 直接读原始像素（原点取
    transform 左上角）；否则退化为 `comp_render_layer_thumbnail` 取画布尺寸的单层合成
    （"所见即所拷"）。有选区时按选区 coverage 裁剪并乘覆盖度（预乘空间里直接乘）。
  - **剪切 / 清除**要求同一 1:1 RGBA8 条件，否则明确报错而不是把 16 位/HDR 层降级或抹错区域；
    剪切先写剪贴板、写成功才清像素（"写入剪贴板失败（未改动）"）。
  - **粘贴**：`comp_layer_add` + `comp_layer_set_pixels`（+ 贴入时按文档空间选区为新层建蒙版），
    全部包在一个显式事务里 = **一步撤销**；落点优先取当前选区 bounds 左上角，无选区时用自定义
    格式里记下的原点（跨应用 DIB 则是 0,0）。100 MP 上限防止误粘超大图。
  - **已知边界**：跨应用只有 32 位 DIB，16 位/浮点会被压到 8 位；不写 `CF_BITMAP`/`CF_DIB`
    （老程序兼容面），也不做 ICC 转换（与 P0-9 关联）。

### P0-5 选区持久化与高级编辑
- **已落地（修改补全第一批，2026-10-22，零导出新增）**：`comp_selection_modify`
  尾部追加 `COMP_SELECTION_BORDER = 3`（环带总宽 N，外扩 ceil(N/2) 减内缩
  floor(N/2)）与 `COMP_SELECTION_SMOOTH = 4`（开+闭形态学，精确欧氏距离场，
  直边逐像素保留、拐角磨圆，半径 1..100）。壳侧菜单 1058/1059、与扩展/收缩共用
  键入态；顺带修掉「缓冲非空即键入态」的输入态缺陷。详见 `docs/shell-parity.md`
  §5.50、`docs/manual-test.md` M49、`visual_check.py --scene selection_modify`（7 条）。
  **仍未做**：Grow / Similar（需新导出，按邻居颜色扩散）、通道 save/load selection、
  变换选区。
- **现状**：已有全选/取消/反选、色彩范围、扩展/收缩、按图层像素载入、羽化、路径、矩形/椭圆/套索/魔棒（`comp_selection.h` 18 个导出）。**缺**：
  - **通道**：没有任何「alpha 通道 / 存储选区」概念（ABI 无 save/load selection；`.comp` manifest 也无 selection 键，`comp_file.cpp:382-473`）；
  - **修改**已有羽化/扩展/收缩/~~边界 / 平滑~~（上条已补），仍无 Grow / Similar；
  - 无变换选区（有手柄缩放，但没有「变换选区」命令与数值输入）；
  - 无快速选择 / 对象选择 / 快速蒙版 / Select and Mask（细化边缘、输出到蒙版/新图层）。
- **PS 行为**：存储选区到通道、载入选区、修改菜单全集、变换选区、快速选择、快速蒙版、选择并遮住。
- **性质**：【ABI 追加】+【引擎】——通道是新的文档模型（还需 `.comp` 持久化与合成语义），Grow/Similar 是新算法，快速选择是连通域+边缘模型。
- **验收**：存/载选区往返 coverage 逐字节相等；`.comp` 保存再打开后选区与通道仍在；Grow 后边界单调外扩。

### P0-6 文字：仍是"一次性栅格化"，没有可编辑文本与字体选择
- **已落地（① 文本图层模型 + 无损重编辑，2026-10-23）**：`Layer::text`
  （`std::optional<TextObject>`）是文字的真源，图层像素只是它恰好文本框大小的渲染
  缓存，合成/缩略图/合并/拼合/`.comp` 像素管线一行未改。ABI 尾部追加 4 个导出
  （146 → **150**）：`comp_layer_add_text` / `comp_layer_set_text` /
  `comp_layer_get_text` / `comp_layer_rasterize_text`；`.comp` 图层记录追加
  `engineText` 键（缺键 = 普通像素层，旧工程逐字节照读）。文字层上的
  `comp_layer_set_pixels` 与 `comp_tool_apply` 一律 `COMP_ERR_STATE`（先栅格化，
  与上游同规矩），`comp_document_resize_image` 隐式丢弃文字对象。壳侧：文字工具
  按落点分流（框内 = 改这一层、框外 = 新建一层）、新菜单
  `图层 > 栅格化文字`、置灰规则进纯函数 `comp_ui::layer_menu_enabled`。
  验收口径已达成：改字号/文本后重新合成，非文字像素逐字节不变；`.comp` 保存再
  打开文本仍可编辑（`tests/test_text_layer.cpp` 8 例 + `visual_check.py
  --scene text_layer` 6 条）。详见 README §6.84、`docs/shell-parity.md` §5.51、
  `docs/manual-test.md` M50。
- **剩余现状（②③ 未做）**：`comp_type_style` 只有字号/行距/字距/抗锯齿，无对齐、
  粗斜体、基线偏移、段落框；外壳只加载**一个**随包字体
  `NotoSansSC-Regular-Subset.ttf`，无字体枚举与选择器；也没有逐字符光标（框内点击
  处即新的左上角，不是上游的「光标处插入」）。
- **PS 行为**：系统字体枚举与选择、点文字/段落文字、对齐、行距字距基线、竖排、
  路径文字、文字变形、栅格化（栅格化已做）。
- **性质**：【引擎】L / 【壳层】M——②字体枚举（`comp_font_load_file` 已能加载任意
  TTF，缺枚举与回退链，P1-4）；③段落框/对齐/竖排/变形（P1–P2）。
- **建议切分**：① 文本图层模型 + 无损重编辑（**已完成**）；② 字体枚举/选择（P1）；
  ③ 段落框/对齐/竖排/变形（P1–P2）。
- **验收（① 已满足，②③ 沿用）**：改字号/文本后重新合成，非文本像素逐字节不变；
  保存 `.comp` 再打开文本仍可编辑。

### P0-7 文件格式：PSD 分层读写已落地（常用子集）；TIFF/WebP/HEIF/PDF 仍缺
- **现状（2026-10-23）**：PSD **分层读 + 分层写**已落地（自研 `engine/format/psd_codec.*`，
  ABI 走 `comp_io_import_document` / `comp_io_export_document`；拍平导入仍走 stb_image）：
  8/16 位 RGB(A)、RLE/raw、图层记录、蒙版、组（穿透/隔离）、剪贴、不透明度、混合模式；
  PSB / CMYK / 32 位 / ZIP / 文字 / 智能对象 / 矢量蒙版 / 图层效果 / 调整层**具名拒绝**。
  TIFF/HEIF/WebP 仍登记为不可用，HDR 只读、EXR 读写；无 PDF / SVG / PSB / RAW 解码。
- **PS 行为**：PSD/PSB 分层读写、TIFF（LZW/ZIP、图层）、WebP/HEIF、PDF 导出、SVG 放置、RAW 导入。
- **余下**：PSB、TIFF（LZW/ZIP、图层）、WebP/HEIF、PDF 导出、SVG 放置、RAW 导入。
- **性质**：【引擎】L–XL（PSD 这一块已完成，路线选了零依赖自研，不引第三方 PSD 库）。
- **验收（已满足）**：导出的 PSD 在第三方阅读器（psd-tools）+ 本仓重新导入后图层数 /
  位置 / 不透明度 / 蒙版一致；不支持的变体显式拒绝，不静默丢弃（`tests/test_psd.cpp`，11 例）。

### P0-8 视图辅助：标尺 / 参考线 / 网格 / 对齐 + 通道面板
- **现状**：全仓 0 命中（ABI 与外壳均无 `guide/ruler/grid/snap`）。注意 README §6.3 的 `guides` 指的是 **Camera Raw 几何校准参数**（`camera_raw.cpp:394-415`），**不是画布参考线**——画布参考线完全不存在。
- **PS 行为**：标尺、从标尺拖参考线、显示/隐藏/锁定/清除参考线、网格与对齐（图层/参考线/文档边界）、智能参考线。
- **性质**：【引擎】+【壳层】M——参考线是文档模型（要进 `.comp`），吸附是交互层。
- **验收**：拖参考线到 12.5px 时按设备像素对齐；存盘再开参考线位置不变；对齐开关不改变像素结果（只影响交互）。

### P0-9 色彩模式与位深
- **现状**：`color_space` 只是字符串，`.comp` 仅接受 `"sRGB"`（`comp_file.cpp:653-656`）；无 `Image > Mode`，无灰度/CMYK/Lab/索引，无 ICC 配置、无软打样、无 16/32 位文档模式开关（`comp_document_desc` 无 pixel_format）。
- **PS 行为**：模式转换（含合并/不拼合提示）、转换为配置文件、指定配置文件、校样设置/色域警告。
- **性质**：【引擎】XL（色彩管理是独立子系统）。可切两刀：① 8/16 位文档模式 + 灰度/RGB 转换（M–L）；② ICC/CMYK/Lab 与软打样（XL）。
- **验收**：位深切换后 8 位像素逐字节不变；灰度转换有明确色度学口径与往返测试；非 sRGB 文档在 `.comp` 里有可识别的 profile 字段（追加）。

### P0-10 界面可用性基线：选项栏 / 光标预览 / HiDPI
- **已落地（选项栏部分，2026-10-22，零 ABI 新增）**：工具条下方新增 32 px 选项栏，
  参与布局（矮窗口主体不足 96 px 时整条收起，画布收回该高度，不重叠、不穿透点击）。
  画笔/橡皮/修饰/加深减淡/历史画笔 2 组，修复 3 组，仿制图章 4 组，选框/套索 1 组，
  魔棒 2 组，渐变 1 组，形状 2 组；Stepper 只有左右 18 px 箭头带响应，Choice 点击前向循环。
  参数复用既有 `ShellState` 字段，与右栏面板同一份数值。详见 `docs/shell-parity.md` §5.49、
  `docs/manual-test.md` M48、`visual_check.py --scene options_bar`（13 条）。
  **仍未做**：数字直输、DPI awareness/manifest、浮动/停靠面板、十字/隐藏光标模式。
- **现状**：~~无工具选项栏~~ 已补（同上）；~~画笔无尺寸/圆形光标预览~~ **已过时**：笔尖轮廓早就有了（`main.cpp:12663-12682`，
  `comp_ui::tool_has_brush_tip` + `brush_tip_radius(brush_size, zoom)`，黑白双描边椭圆，随缩放变化），
  这一项只剩「十字/隐藏光标模式」没做；`shells/windows` **没有 manifest、源码无 `SetProcessDpi*`**，
  高 DPI 屏由系统位图拉伸；面板不能浮动/合并（`docs/shell-parity.md` §5.11）。
- **PS 行为**：选项栏、笔刷大小光标、HiDPI 原生渲染、可停靠/浮动面板。
- **性质**：【壳层】S（选项栏/光标）+ S（DPI awareness）+【架构】L（面板停靠系统）。
- **验收**：200% 缩放下 1px 线条仍为 1 设备像素；触摸笔刷尺寸可见且不遮挡落点；窗口 resize 后布局不重叠（已由
  `ui_core_layout_never_overlaps_at_any_size` 覆盖选项栏）。

---

## 3. P1 —— 专业工作流常用，但第一天不致命

### P1-1 图层样式补全与组混合
- 补 Bevel & Emboss / Satin / Gradient Overlay / Pattern Overlay（6 → 10）；合并为**一个图层样式对话框**（当前只有「效果子菜单 + 参数面板」）；全局光角度/高度。
- 组支持混合模式（现在 `comp_file.cpp:285` 对组强制 Normal）；Pass Through 应成为显式枚举而非仅 `isolated` 开关。
- 性质：【引擎】M +【ABI 追加】（新 effect kind 追加到 `COMP_EFFECT_*` 尾部，值 6..9）。

### P1-2 图层管理
锁定（像素/位置/透明/全部）、图层链接、颜色标签、按类型/名称/颜色过滤、图层复合（Layer Comps）、智能对象与智能滤镜、栅格化、合并可见、反向排列、样式拷贝/粘贴、新建填充层（纯色/渐变/图案）。
- 性质：【引擎】M–L；锁定与标签可先做【ABI 追加】+【壳层】S。

### P1-3 变换
自由变换全套：斜切/扭曲/透视/变形（Warp 网格）、数值变换、再次变换（Ctrl+Shift+T）、变换选区、内容识别缩放。
- 现状：只有移动/缩放/15° 旋转与 3 种采样模式；无 skew/distort/perspective（ABI 0 命中）。
- 性质：【引擎】L（需要任意四边形/网格重采样与几何模型扩展）。

### P1-4 画笔系统
画笔预设库、间距/圆度/角度、纹理/双重画笔/动态、压感（WinTab / WM_POINTER）、混合器画笔、图案图章、背景橡皮擦 / 魔术橡皮擦、颜色替换、历史记录艺术画笔、笔刷光标表现。
- 现状：`comp_tool_brush` 有 diameter/pressure/hardness/flow/grain/seed（`comp_layer.h:556-575`），但外壳恒 `pressure=1.0`、`flow=1.0`（`main.cpp:6522`），无预设、无间距、无数位板。
- 性质：【引擎】M（间距/纹理/动态）+【壳层】M（Wintab/Pointer 输入、预设存储）。

### P1-5 滤镜家族（引擎已就绪的优先接线）
- **立即可做**：Camera Raw 滤镜入口【接线】——`comp_camera_raw_apply` + 763 张表 + 资源文件都在（`docs/camera-raw.md:13-18`），外壳 0 处调用，用户完全拿不到。
- **已落地（2026-10-22，零 ABI 新增）**：`滤镜 > Camera Raw 滤镜…`（`Ctrl+Alt+R`），
  复用文档对话框的右栏面板与键盘模型（12 行 Basic 组：曝光 / 色温 / 色调 / 对比 /
  高光 / 阴影 / 白色 / 黑色 / 清晰度 / 鲜艳度 / 饱和度 / 去雾）。
  - 表在**启动时**从 `resources/camera_raw_tables.bin` 加载（exe 同级 / 上一级 / cwd
    三个候选，与字体同一套查找顺序）；缺失不阻断启动，打开面板时如实报「色彩表未加载」。
  - JSON 在 `ui_core.h` 里生成并有单测：只写**非零行**，因此「全默认」就是 `{}`
    → 引擎回 `changed=0`、不开事务、不记撤销步。
  - 曝光以 0.01 EV 整数为单位（±500），写 JSON 时除以 100；负值靠 `-` 起输入
    （只对 min<0 的行生效，其它对话框行为不变）。
  - 已知边界：写**选中图层整层**，不吃选区；组/HDR 图层由引擎拒绝（`COMP_ERR_STATE`），
    失败时面板留在原地、参数不丢；ABI 没有「读回当前 Camera Raw 参数」的 getter，
    所以每次打开都是零草稿（副标题已写明）。
  - 探针：新增 `camera_raw` 场景 5 条（面板打开 / +1 EV 变亮 / 一步撤销 / -1 EV 变暗）。
- 新增：锐化/智能锐化/USM、降噪、模糊画廊（场景/光圈/倾斜偏移/旋转/路径）、其他（高反差保留/最小值/最大值/位移/自定）、液化独立对话框（现有 Push 仅笔刷）、扭曲/像素化/风格化/渲染各族、消失点。
- 性质：复用 `engine/imaging` 内核的部分为 M；全新滤镜族 L。

### P1-6 调整补全
补 Brightness/Contrast、Vibrance、Selective Color、Photo Filter、Channel Mixer、Color Lookup、Posterize、Threshold、Desaturate、Shadows/Highlights；**破坏性 Image > Adjustments 全族**（现在只有反相）；自动色调/对比度/颜色。
- 性质：12 → 20+ kind 【ABI 追加】（尾部）+【引擎】像素实现（多数可复用 AdjustPixels）。

### P1-7 渐变升级
多色标（>2）、色标中点、不透明度中点、角度/对称/菱形、抖动、渐变预设库与编辑器；渐变映射调整同步支持多色标。
- 现状：`comp_tool_gradient` 只有 `color0/color1`，类型 0=线性/1=径向（`comp_layer.h:660-669`）。
- 性质：【ABI 追加】（新结构体 `comp_gradient_stops` 尾字段）+【引擎】M。

### P1-8 面板 / 工作区 / 首选项
面板浮动/停靠/合并/分组、保存工作区、工具选项栏持久化、首选项（内存/历史步数/缓存盘/单位/网格）、快捷键自定义、明暗主题。
- 性质：【架构】L +【壳层】M；历史步数/字节预算 ABI 已有（`comp_undo.h:49-61`），壳只显示已用字节（`main.cpp:768-772`），**没有设置入口**。
- **设置入口已补（2026-10-22）**：`编辑 > 历史记录容量…`（Ctrl+Alt+H）打开两个数值行
  （步数上限 / 内存上限 MiB），回车调 `comp_document_set_history_limit` +
  `comp_document_set_history_budget`。**0 输入回显为「引擎默认」**（200 步 / 512 MiB），
  不把引擎默认值抄进壳里当第二份真源。面板、工作区保存、首选项其余项仍未做。


### P1-9 打印与输出
打印对话框、纸张/缩放/位置、色彩配置文件、出血与角线、PDF/X；导出预设与批量导出。
- 性质：新子系统（不建议现在做；无色彩管理时打印是假的）。

### P1-10 自动化
动作录制/播放、批处理、脚本/命令面板、数据驱动图形。
- 现状：`comp_command_*` 只有 7 条命令（`comp_command_abi.cpp:30-39`），与外壳 kCmd* 是两套体系。
- 性质：【引擎】+【架构】——先把命令表统一为单一真源（外壳菜单/快捷键/动作共用），再做录制。

---

## 4. P2 —— 长尾与可选

- 3D 图层、视频图层、时间轴动画、360 全景编辑。
- 画板（Artboards）、切片、注释、计数工具、测量记录。
- 变量与数据组、图像处理器脚本。
- 完整色彩管理：CMYK/Lab/专色/ICC/软打样/色域警告、32 位 HDR 显示与广色域。
- 文件长尾：PSB、RAW（DNG/CR2/NEF…）解码、SVG 放置、AVIF、GIF 动画、HDR 导出、PDF 导入。
- 云文档/字体同步/协作/版本历史（PS 需要账号体系，非本仓取向）。
- 辅助功能：UI Automation、屏幕阅读器、高对比度主题。
- 平台：macOS / Linux 壳、鸿蒙真机验证（README §6.2 已记证据边界）。
- 性能：调整层与 HDR 表面的 GPU 路径（README §6.4 ③ 已记两条整篇回落）。

---

## 5. 明确不做（与 README §6.4 口径一致，不占缺口）

- **ML 类**：Select Subject、Remove Background、Neural Filters、天空替换、内容识别填充的模型部分。
- **需要第三方解码器且尚未定策略**：HEIF/WebP/TIFF/RAW（`docs/format-import-matrix.md` §3 已给出各自落地路线，缺的是"引不引库"的决策）。
- **上游 Compositor 本身没有的**：不作为"vs Photoshop"的必达项，除非用户明确要求。

---

## 6. 硬约束的落地规则（每条需求提交前自查）

1. **ABI 只追加**：新导出加在对应头文件末尾并新建 `.cpp`；枚举**只在末尾追加**（当前末尾：`COMP_TOOL_HISTORY_BRUSH=19`、`COMP_FILTER_PROGRESSIVE_BLUR=8`、`COMP_EFFECT_KIND_COUNT=6`、`COMP_ADJUSTMENT_COLOR_BALANCE=11`、`COMP_FORMAT_*`=8）；结构体字段只能尾部追加（`comp_tool_op_desc` 已有 "S5/S9/S10 尾部追加" 先例）。
2. **冻结值不得重排**：像素格式/后端/表面/编解码/结果码/混合/工具/调整/效果/滤镜/格式的数值都是 ABI。
3. **回归门禁**：任何引擎改动后 `E:\python-3.14.5-embed-amd64\python.exe scripts/build.py` + `scripts/run_regression.py --require-ohos` 必须 6/6。
4. **壳自检 SHA**：既有 **257 项**的输出文本与顺序**不得改动**。新能力验证优先放 `tests/` 与 `scripts/visual_check.py`；若必须新增 selftest 项，**只能追加到末尾**，并把基线更新作为一次显式审批（否则 SHA 必变，等于破了硬约束）。
5. **视觉探针**：只追加场景，不改既有 80 条断言的阈值与命令 ID；前台焦点问题沿用已修的 `SPI_SETFOREGROUNDLOCKTIMEOUT` 方案，不从终端后台启动。
6. **工具表**：新增工具必须同时改 `ui_core.h tool_table()`、`scripts/check_tool_table.py` 基线、快捷键冲突检查，并追加 `COMP_TOOL_*` 值。
7. **每条功能的通用契约**：非法参数在事务前拒绝；失败不留半成品、不写历史；一步撤销；无操作不记空步；可重复（seed 固定）。

---

## 7. 建议推进顺序（最短路径到"能当 PS 用"）

| 阶段 | 内容 | 预估 | 理由 |
|------|------|------|------|
| A | P0-2 图像/画布大小、P1-5 Camera Raw 入口、P0-1 新建对话框、P0-3 另存为/脏提示 | 3–5 天 | 全是【接线】/【壳层】，零 ABI 风险，立刻补上"改尺寸/存副本"的日常动作 |
| B | P0-4 剪贴板、P0-10 选项栏/光标/DPI | 1–2 周 | 壳层为主，跨应用工作流打通 |
| C | P0-5 选区通道 + 修改补全 + 变换选区 | 2–3 周 | 引擎 + ABI 追加，所有下游编辑的入口 |
| D | P0-6① 文本图层可编辑 → P1-4 字体选择/画笔预设 | 3–5 周 | 最痛的用户体验缺口 |
| E | P0-7 ~~PSD 分层导出~~ → **PSD 分层读写已完成**（2026-10-23，自研常用子集）；余 PSB / TIFF / WebP / PDF | — | 交付刚需已解锁 |
| F | P1-1/2/3 样式/图层/变换、P1-6/7 调整/渐变、P1-5 滤镜族 | 6–10 周 | 专业工作流 |
| G | P0-9 色彩管理、P1-9 打印、P1-10 自动化、P0-3 多文档 | 视排期 | 各自独立子系统，建议单独立项 |

---

## 8. 顺带记录的文档漂移（不属于功能缺口，但会误导排期）

> 2026-10-23 校正一轮：下面每条都重新对着当前代码数过一遍，数字与指针已更新。

- README §6.4 ① 写"**112 个**导出 ABI 全有异常护栏"；当前头文件实际导出 **152** 个
  （尾部追加 `comp_layer_get_pixels`、4 个文字图层导出、以及 PSD 分层读写的
  `comp_io_import_document` / `comp_io_export_document`）。
- README §6.4 ④ 写"Windows 壳 **164 项** `--selftest` 基线"、"`visual_check.py`（11 场景 / 45 断言）"；当前基线为 **263 项**（2026-10-23 §6.88 追加 6 项 PSD 分层入口断言）、视觉探针 **128 条 / 24 场景**（2026-10-22/23 依次追加 `doc_dialog`、`camera_raw`、`clipboard`、`close_prompt`、`options_bar`、`selection_modify`、`text_layer` 后；原 80 条含 IME 6/6）。
- README §6.3 的 `guides` 易被读成"画布参考线已有"；实际是 Camera Raw 几何校准参数。**画布参考线不存在**（本清单 P0-8）。
- §6.4 口径 B 的 48 条分母来自上游 Compositor，不是 Photoshop；用它回答"能当 PS 用吗"会系统性高估（本清单改用 PS 使用路径对照）。
- `comp_layer.h` 的 `comp_tool_type` 注释原先只写"画进当前图层、不可再编辑"，没提可编辑
  文字图层；已补一行指路（`comp_layer_add_text` / `comp_layer_set_text`），行为未改。

---

## 附录：证据索引

| 结论 | 证据 |
|------|------|
| 152 个导出 | `engine/abi/include/compositor/*.h`（`^COMP_API` 行计数，2026-10-23 实测 152） |
| 新建固定参数 + 销毁旧文档 | `shells/windows/src/shell_state.h:52`（`kDefaultWidth/Height`）、`shell_document.cpp:302`（`create_document`）→ `shell_document.cpp:255/313`（`comp_document_destroy`） |
| 无图像/画布大小菜单 | ~~`shells/windows/src/main.cpp:14859-14866`~~ → 2026-10-22 已接线，见 P0-2（`kCmdImageResize` 等） |
| 单文档 | `ShellState` 内嵌**一个** `DocumentContext`（`shell_state.h`）；多文档仍是【架构】项，`main.cpp` 拆分不为它背书 |
| 剪贴板 | 2026-10-22 已落地（P0-4，`编辑` 菜单六项，`shell_clipboard.cpp`）；跨应用仅 `CF_DIBV5` |
| 选区修改仅五种 | `engine/abi/include/compositor/comp_selection.h:52-58`（羽化/扩/缩/边界/平滑；Grow/Similar 仍无） |
| 文字样式字段 | `engine/abi/include/compositor/comp_layer.h:762-767`（可编辑文字对象见同文件尾部 P0-6 ①） |
| 单一随包字体 | `shells/windows/src/shell_internal.h:89`（`kShellFontFileName`）；枚举/选择未做（P1-4） |
| 渐变仅两色标 | `engine/abi/include/compositor/comp_layer.h:660-669` |
| 6 种效果 / 12 种调整 | `comp_layer.h:300-313`；`README.md:1462-1500` |
| 9 种滤镜 | `engine/abi/include/compositor/comp_filter.h:38-48` |
| 格式可用性 | `docs/format-import-matrix.md:44-51` |
| 仅 sRGB | `engine/format/comp_file.cpp:653-656` |
| 组强制 Normal | `engine/format/comp_file.cpp:285` |
| `.comp` 不存选区/路径/参考线 | `engine/format/comp_file.cpp:382-473` |
| 命令表仅 7 条 | `engine/abi/src/comp_command_abi.cpp:30-39` |
| Camera Raw 无壳入口 | ~~`shells/windows/src/*` 内 `camera` 0 命中~~ → 2026-10-22 已接「滤镜 > Camera Raw」入口（`docs/camera-raw.md`）；**RAW 文件解码**仍无 |
| 无 DPI 感知 | `shells/windows` 无 `*.manifest`；源码无 `SetProcessDpi*` |
| 壳自检/视觉探针基线 | 壳自检 **263 项** failures=0、SHA256 `2f3b81c4…`（2026-10-23 §6.88 追加 6 项 PSD 分层入口断言，附录式、已审批）；视觉探针 **128/128 / 24 场景**（依次追加 `doc_dialog`、`camera_raw`、`clipboard`、`close_prompt`、`options_bar`、`selection_modify`、`text_layer`） |
