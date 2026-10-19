# GPU 逐层纹理缓存的修订号设计（S17 设计稿）

状态：**R1 已实施（S18），R2 待做**。本文确定数据结构、修订号分配点、缓存键与验收口径；
不修改 C ABI（导出仍 112 个）、不改 `.comp` 格式、不动事务/历史语义。

- **R1 已落地**：`Layer::content_revision`、`Document::allocate_revision()` /
  `touch_layer()` / `touch_all_layers()`、全部写入口换号（含 3 个领域自管事务入口）、
  §4.1 的 1–5 条验收测试（`tests/test_layer_revision.cpp` 6 例，默认构建 457 例全绿）。
- **R2 未做**：D3D12 持久纹理缓存、`texture_uploads()` 计数、§4.2 的真机测试与基准复测。

关联：§6.23（S13 GPU 设备侧接入）、§6.26（S16 DXGI 直呈）、§6.27（S17 按单元拆细）、
§6.28（S18 R1 落地）。

---

## 1. 为什么必须做（实测依据）

| 场景 | 实测 | 说明 |
|------|------|------|
| 1024²、4 层、GPU 离屏合成 | **3.01 ms**（S13 稳态） | 4×4 MB 逐帧上传 + 4 次 dispatch |
| 同场景 CPU 展平 | **0.65 ms** | 参考 |
| 1024² 窗口直呈（render+present） | **3.04–3.27 ms**（S16，30/60 帧各三次） | 只比 CPU flatten+GDI 快 1.18–1.29× |
| 同场景 CPU flatten + RGBA→BGRA + `StretchDIBits` | **3.85–3.91 ms** | — |

结论：**瓶颈是逐层上传，不是上屏也不是混合**。S17 之后 CPU 侧还多了一次「烘焙」，
所以缓存收益更大：静止的层只需上传一次，之后每帧只做 dispatch + 拷贝。

但是「逐层纹理缓存」需要回答一个问题：**这张 GPU 纹理还对应图层当前的内容吗？**
现在没有答案。

## 2. 现状盘点（代码事实，别凭印象）

1. **只有整篇序号**：`Document::content_serial_`（§6.22）在事务提交/回滚、撤销/重做、
   结构增删挪、画布重采样、`mark_dirty(true)` 时 `+1`；`composite_cache_serial_` 只是
   「上次写合成缓存时的序号」。它回答的是「整篇合成结果是否还有效」，
   **粒度太粗**：画笔改一层 → 整篇失效 → 所有层纹理都要重传，缓存零收益。
2. **`Layer` 没有修订号**。`ImageBuffer` / `MaskBuffer` 是 `shared_ptr` + COW：
   `writable_pixels()` 只在 `use_count()>1` 时复制。COW 保证「写不污染别的持有者」，
   **不提供「内容变过」的版本信息**。
3. 因此**不能**拿 `pixels.pixel_data().data()` 指针当缓存键：图层是唯一持有者时，
   画笔/滤镜/液化全是**原地写**，地址不变 → 缓存会静默读到旧像素（最坏情况：
   用户看到上一笔的画面，且不报错）。
4. 也不能对像素做哈希：1024² RGBA8 每帧 4 MB，哈希成本约等于上传成本的一半，
   收益直接归零；而且哈希在「内容没变」时仍要全量读一遍内存。

所以需要**显式的内容修订号**，由写路径负责推进，而不是靠地址/哈希推测。

## 3. 设计

### 3.1 数据结构（拟）

```cpp
// domain/layer.h
struct Layer {
  // ... 既有字段 ...
  /*
   * 内容修订号：标识「这一层的图形输入」（像素/蒙版/效果/调整/几何/混合参数）
   * 处于哪个版本。0 = 尚未分配（新建图层由 Document 立即分配，正常运行不会看到 0）。
   * 单调递增、文档内唯一；同值 ⟺ 同一份内容（快照恢复可安全复用旧值）。
   */
  uint64_t content_revision = 0;
};

// domain/document.h
class Document {
  // ...
  uint64_t allocate_revision() noexcept { return next_revision_++; }
 private:
  uint64_t next_revision_ = 1;
};
```

不变量（必须由实现守住）：

- **单调**：`allocate_revision()` 只增不减；回滚/撤销不让序号回落。
- **同值 ⟺ 同内容**：一个修订号一旦分配，就只能对应那一份内容；内容再变必须换号。
  推论：历史快照恢复**保留**快照里的修订号是安全的（内容确实回到了那份），
  而「把 A 层的内容复制给 B 层」必须给 B 分配新号（B 的 id 不同，缓存本来也不会混）。
- **不参与序列化**：修订号是派生/会话内数据，不进 `.comp`、不进事务快照的语义比较、
  不进历史 diff。

### 3.2 哪些写入必须推进修订号

判据：**任何会改变该层在合成中的贡献或它在 GPU 计划里的分类的写入**。

| 写入 | 例 | 为什么 |
|------|----|--------|
| 像素 | `comp_layer_set_pixels`、笔刷/橡皮/克隆/涂抹/液化、`comp_filter_apply`、Camera Raw、选区填充/删除、裁剪切图、merge down、导入解码 | 纹理内容变了 |
| 蒙版 | `comp_layer_set_mask`、选区存为蒙版 | 烘焙结果变了 |
| 效果 / 调整 | `comp_layer_set_effect` / `remove_effect` / `add/update/remove_adjustment(_at)` | 烘焙结果或单元分类变了（调整层当前整篇回落，但键必须先正确，避免以后接入时读旧） |
| 几何 | `comp_layer_set_transform`、旋转/翻转/缩放（含 `resize_image` 的光栅化路径）、`sampling` | 原生↔烘焙分类、采样结果变了 |
| 混合参数 | `comp_layer_set_blend` / `set_opacity` / `set_visible` | 单元参数变了；`visible` 还决定是否出现在计划里 |
| 结构 | `set_parent`、`set_isolated`、`set_clipping_source`、`reorder`、`remove`、`add` | 单元序列/子树归属变了；单层纹理可留，**计划**必须重算 |
| 画布 | Image Size / Canvas Size / `resize_image` | 所有层像素被重采样 → 全部换号（或把 `doc.width/height` 放进缓存键，双保险） |

实施落点（**S18 / R1 已按此落地**；与最初草案不同的地方标了「实测修正」）：

1. `Layer::content_revision` + `Document::allocate_revision()` / `touch_layer(id)` /
   `touch_all_layers()`；新建图层在 `Document::create_layer` 里分配，`merge_down` /
   `flatten_image` 的结果层也分配新号（新内容不能沿用旧号）。
2. **写入口统一 bump**：14 个 `comp_layer_*` setter、`comp_selection_move_pixels`
   （仅 `changed`）、`comp_crop_apply` / `comp_document_set_size`（整篇，仅真的变了时）。
   ABI 层 bump 的好处是「一个用户操作 = 一次 bump」，domain 内部多段写不会重复推进。
   **实测修正**：`comp_tool_apply` / `comp_filter_apply` / `comp_camera_raw_apply` 走的是
   领域自管事务（外面再包 `ImplicitTxn` 会双记历史），所以这三处由 domain 函数自己在
   `txn_commit()` **之前** bump。
3. **直接写像素的 domain 路径自查结论**：`writable_pixels()` 的 61 处写点全部要么被上面
   的入口覆盖（tool_ops / filters / camera_raw / selection_pixels / comp_file / crop），
   要么写的是临时派生缓冲（`compositing/*` 的 out/临时 surface、`layer_merge` 的临时
   文档）——**没有漏网**，纯内部派生缓冲不 bump。
4. **事务/撤销（实测修正：不按快照差异推进，也不在 undo/redo 时 bump）**：
   - 换号发生在 `txn_commit()` **之前**：提交压入的 after 快照因此带上新号；
   - 撤销/重做只恢复快照，**保留快照里的号**（不额外 bump）：内容确实回到了那一份，
     缓存命中安全；在 undo/redo 时再 bump 只会制造「同内容两个号」的假失效；
   - 若在 commit **之后** bump，重做会把「编辑后的内容 + 编辑前的号」凑在一起，违反
     「同值 ⟺ 同内容」，缓存会**静默供应旧纹理**——所以顺序是硬约束，测试用
     「笔刷 → 撤销 → 重做」钉住；
   - 活跃事务期间（`txn_active()`）GPU 后端不得采信缓存——与 S12 合成缓存同一条规则
     （R2 实现）。
5. **结构变更不发号（实测修正）**：`add` / `remove` / `reorder` / `set_active` /
   `set_clipping_source` 只改堆叠序或编辑器状态，单层像素没变，纹理可留；合成计划每帧
   从文档重建（`build_gpu_plan`），不存在需要失效的计划缓存。`set_isolated` 是例外——
   §3.2 表里列在「结构」，但它改变该组的合成贡献，因此**要**换号。

### 3.3 缓存键与失效策略

```cpp
struct CachedLayerTexture {
  ID3D12Resource* texture = nullptr;
  uint64_t revision = 0;
  int32_t width = 0, height = 0;
  comp_pixel_format format = COMP_PIXEL_RGBA8;
  uint64_t last_used_frame = 0;
};
// D3d12RenderBackend 私有
std::unordered_map<comp_layer_id, CachedLayerTexture> layer_textures_;
```

- 键 = `(layer.id, layer.content_revision, doc.state().width/height, surface format)`。
  命中条件全等；否则重建纹理。
- 纹理走**持久堆 + 按尺寸空闲链**复用，避免每次 `CreateCommittedResource` 抖动；
  上限按显存预算（例如 512 MB）LRU 淘汰，淘汰只影响性能。
- **只缓存原生单元**（`GpuUnit::is_native()`）。烘焙单元先不做：它的键需要子树
  所有层修订号的集合 + 组结构，属于 v2。
- 缓存是派生数据：不进事务快照/历史/`.comp`；文档销毁、设备丢失、尺寸变化即整体丢弃。
- 诊断：加 `texture_uploads()` 计数（每创建/重传一张纹理 +1），**仅供测试**，
  不进 C ABI、不进 `BackendCaps`。

### 3.4 为什么不直接用整篇序号做键

「整篇 `content_serial` + layer id」能保证正确，但粒度不对：画笔一笔就让整篇失效，
等于没有缓存。**折中 v0**（可先做）：缓存项记录上次上传时的整篇序号，只在整篇序号
不变时命中——对「文档不动、只是重绘/缩放/上屏」的 S16 基准场景已经有效（那正是
当前 3.0 ms 的场景），但交互编辑期间无收益。逐层修订号是为交互编辑准备的正解。

## 4. 验收与测试计划

### 4.1 不需要 GPU 的测试（默认构建）——**S18 已全部落地**

落点：`tests/test_layer_revision.cpp`（6 例）；下面 1–5 条都有对应断言，映射见 S18 报告 §3。

1. **单调性**：对每个 ABI 写入口调用一次，断言目标层 `content_revision` 严格变大；
   未触碰的层不变。
2. **COW 陷阱**：两层共享同一 `pixels`（复制/派生），改 A 层后：A 变号、B 不变号、
   B 像素与改前逐字节相同。
3. **原地写**：`writable_pixels()` 原地改像素后经公开 ABI（画笔）落笔 → 必须变号
   （这条能抓住「只改指针比较」的实现）。
4. **撤销**：修改 → 撤销，断言恢复后的修订号等于修改前那一份内容对应的号
   （同值 ⟺ 同内容），且再渲染结果与修改前逐字节相同。
5. **计划重算**：改 `transform`/`blend`/`visible`/`parent` 后 `build_gpu_plan`
   的单元序列随之变化（现有 S17 计划测试可扩展）。

### 4.2 需要 GPU 的测试（`--gpu` + 真机）

1. 连续两次 `render_document`（不改文档）：第二次 `texture_uploads()` 不增，
   像素与 CPU 基准仍 ≤1 LSB。
2. 只改一层再渲染：`texture_uploads()` 恰好 +1，画面反映该层改动。
3. 改画布尺寸：缓存整体重建（计数回到全量），画面正确。
4. 事务中途渲染：不得命中事务前的缓存（活跃事务期间禁用缓存）。

### 4.3 性能口径（只报实测）

基线：S13 离屏 3.01 ms、S16 直呈 3.04–3.27 ms。实施后在同一机器、同一场景
（1024²、4 层）复测并**如实写数字**；若没有明显下降（例如仍被 dispatch 或像素
拷贝主导），就按实测记录，不宣称加速倍数。

## 5. 明确不做 / 后续项

- 不做像素哈希、不做内容比较上传、不做跨文档/跨后端共享。
- 不做烘焙单元缓存（需求：子树修订号集合 + 组结构签名，v2）。
- 不做「调整层分段回读」：调整层作用于「已合成画布」，要上 GPU 就得把画布分段
  回读/分段提交，属于独立一轮，不与本设计混做。
- 不改 C ABI、不改 `.comp` 格式、不改事务/历史可观察语义。

## 6. 实施顺序（两轮）

| 轮次 | 内容 | 验收 | 状态 |
|------|------|------|------|
| R1 | `Layer::content_revision` + `Document::allocate_revision()` + 全部写入口 bump + 4.1 的 1–5 | 默认构建全绿，用例数增加 | **已落地（S18）**：451 → 457 例，默认/`--gpu`/壳自检/OHOS 回归全绿 |
| R2 | D3D12 持久纹理 + `texture_uploads()` + 4.2 + 真机基准复测 | `--gpu` 全绿，报告实测 | 待做 |
