// AI生成
/*
 * domain_state.h — 可快照的文档状态。
 *
 * 撤销/重做在 M1 采用「整篇状态快照」实现：结构简单、语义绝对正确，
 * 代价是撤销栈内存占用与文档规模成正比。M2 将改为增量/分块历史（对齐原
 * DocumentHistory 的增量记录思路）。此取舍在 README 与评审材料中显式标注。
 */
#ifndef COMPOSITOR_DOMAIN_DOMAIN_STATE_H
#define COMPOSITOR_DOMAIN_DOMAIN_STATE_H

#include <cstdint>
#include <string>
#include <vector>

#include "domain/layer.h"

namespace compositor {

/* ---------------------------------------------------------------------------
 * 选区状态（F1，对齐上游 Document/Selection.swift 的 DocumentSelection）。
 *
 * 上游把选区存成 CGPath + antialiased + feather：路径是唯一真源，coverage 在
 * 使用时才由 CoreGraphics 栅格化。本仓不引入路径库，改为在**文档分辨率**上
 * 存一张 8 位灰度蒙版（MaskBuffer：0 = 不选中，255 = 全选中），原因有三：
 *
 *   1. 每个消费点（画笔/橡皮裁剪、内容感知填充、Crop 之后的重建）本来就要
 *      逐像素 coverage，路径每次现场栅格化只会把同一份工作重复多遍；
 *   2. 蒙版与图层蒙版同构，直接复用 MaskBuffer 的共享 + COW 资产模型，
 *      于是「整篇快照」的撤销自动成立（不再需要单独的历史钩子）；
 *   3. .comp 往返需要的是「选中了哪些像素」，不是「用过哪条贝塞尔曲线」。
 *
 * 字段口径（与上游逐一对应）：
 *   - active：是否有选区。false = 无选区（编辑不受限）；true 且 base 全 0
 *     = 上游的「显式空选区」（一个合法但落空的路径，编辑结果为空），两者
 *     在语义上必须区分——上游用 nil 与空 CGPath 区分，这里用 active 区分；
 *   - base：**未羽化**的硬边 coverage（上游 path 栅格化的结果）。add/subtract
 *     在这张图上叠加，保证羽化不会越叠越糊（上游每次都用 path 重建）；
 *   - feather：羽化半径（像素，0 = 硬边）。与上游一样是路径上的**一个参数**
 *     而非烘焙进路径：再次羽化按 sqrt(f1² + f2²) 叠加、上限 250
 *     （Selection.swift `featherSelection(by:)`）；任何新的形状（含 add/subtract）
 *     把它重置为 0（上游 applySelection 构造 DocumentSelection 时未传 feather）。
 *
 * 消费方读「有效 coverage」必须走 selection_effective_coverage()（把 base 按
 * feather 做高斯模糊），不要直接读 base——直接读会得到一个硬边选区。
 */
struct SelectionState {
  bool active = false;
  float feather = 0.0f;
  MaskBuffer base;
};

/* 画布参考线（标尺拖出的对齐线）。
 * 放进 DocumentState 而不是外壳的设置表：参考线要进 `.comp`、要随撤销回退，
 * 而撤销/事务快照只认 DocumentState（同选区的理由）。
 * axis 取 0 = 水平（position 是 y）、1 = 垂直（position 是 x）；位置是连续的
 * 文档像素，取整发生在交互层。详细契约见 domain/guide.h。 */
struct Guide {
  uint32_t axis = 0;
  double position = 0.0;
};

/* 存储的选区通道（PS 的 Alpha 通道；Select > Save Selection / Load Selection）。
 *
 * 与 SelectionState 同一套理由放进 DocumentState：通道要随撤销回退、要进
 * `.comp`，而快照只认 DocumentState。
 *
 * 字段口径：
 *   - name：UTF-8、单行、非空，≤ kMaxSelectionChannelNameBytes（见
 *     domain/selection_channel.h）。名字只用于面板与 .comp，不参与像素求值；
 *   - mask：**未羽化**的硬边 coverage，**文档分辨率**（0/255 与抗锯齿灰阶），
 *     与 `SelectionState.base` 同构。存硬边而不是「烘焙过羽化」的图，是为了
 *     存/载往返逐字节相等：羽化是选区参数，载回时一起恢复；
 *   - feather：保存那一刻的羽化半径，与 mask 成对恢复。
 *
 * 注意 mask 尺寸是文档尺寸：换画布（Image Size / Canvas Size / Crop）时必须
 * 与选区一起丢弃（见 document.cpp / crop.cpp），否则载入会得到错位的选区。
 * 旋转画布（90°/180°）是例外：坐标系只旋转不缩放，通道蒙版随选区一起转置。
 */
struct SelectionChannel {
  std::string name;
  float feather = 0.0f;
  MaskBuffer mask;
};

/* ---------------------------------------------------------------------------
 * 图层复合（对齐 Photoshop 的 Layer Comp）。
 *
 * 一张复合记录的是「某一刻所有图层的可见性与不透明度」——即上游 Layer Comp
 * 面板里勾了 Visibility 与 Appearance 时的两组属性；位置（Position）不在其中：
 * 本引擎的几何是 transform 矩阵，把它塞进复合会让「移动图层后应用旧复合」
 * 把用户的位移悄悄改回去，代价与收益不成比例，故明确不做。
 *
 * 存储是**快照数组**而不是「按 id 查表」：数组序就是捕获时的堆叠序，面板按
 * 顺序列出时不需要再排序；查找在这一规模上线性扫描足够。
 *
 * entries 只记录捕获时**存在**的图层：之后新建的层不在表里，应用时保持原样
 * （上游同款语义——复合并不知道未来会出现什么层）；被删掉的层在应用时跳过。
 */
struct LayerCompEntry {
  comp_layer_id layer = 0;
  uint8_t visible = 0;
  float opacity = 1.0f;
};

struct LayerComp {
  uint32_t id = 0;
  /* UTF-8、单行、非空，≤ kMaxLayerCompNameBytes（见 comp_layer.h 的 ABI 契约）。 */
  std::string name;
  std::vector<LayerCompEntry> entries;
};

/* 单篇文档的复合数量上界：面板是人工管理的清单，128 已远超实用规模，同时又
 * 给 manifest 的 4 MiB 上界留足余量。 */
constexpr size_t kMaxLayerComps = 128;
constexpr size_t kMaxLayerCompNameBytes = 1024;

struct DocumentState {
  int32_t width = 0;
  int32_t height = 0;
  float dpi = 72.0f;
  std::string color_space = "sRGB";
  /*
   * P0-1：文档级位深与颜色模式。创建时确定，之后不可转换（PS 的「模式转换」
   * 是独立功能，本版不做）。
   *
   * pixel_format 是**新建空图层的缺省格式**，不是「所有图层都必须如此」：
   * 导入的 16 位资产、单层 set_pixels 写入的 HDR 图层都可以与文档缺省不同。
   * 放在 DocumentState 里是因为 .comp 要落盘、撤销快照要带上——模式是文档的
   * 属性，丢一次就会让「保存后重开变 RGB」这种账算不清。
   */
  int32_t pixel_format = 0; /* comp_pixel_format：0=RGBA8 / 1=RGBA16 */
  int32_t color_mode = 0;   /* comp_color_mode：0=RGB / 1=GRAYSCALE */

  /* .comp 包的 documentID（大写 UUID 文本）。文档创建时生成、随文档持久保存，
   * 图层文件名由 (documentID, 图层 id) 派生，因此同一文档多次保存不会漂移。 */
  std::string document_id;

  /* 堆叠序：索引 0 为最底层，与 .comp 规范一致。 */
  std::vector<Layer> layers;
  comp_layer_id next_layer_id = 1;
  comp_layer_id active_layer = 0;

  /* 选区（见 SelectionState）。放在状态里而不是 Document 的旁路成员：
   * 撤销/重做、事务快照与事务回滚都只认 DocumentState，旁路成员会漏掉。
   * 注意 base 的尺寸是**文档尺寸**，换画布（Image Size）时必须同步处理。 */
  SelectionState selection;

  /* 参考线（见 Guide）。空向量 = 无参考线；落盘为 manifest 的 engineGuides。 */
  std::vector<Guide> guides;

  /* 存储的选区通道（见 SelectionChannel）。空向量 = 没有通道；落盘为
   * manifest 的 engineChannels，蒙版在 images/<documentID>.channel<N>.png。 */
  std::vector<SelectionChannel> channels;

  /* 图层复合（见 LayerComp）。空向量 = 没有复合；落盘为 manifest 的
   * engineLayerComps（空表不写键，旧 manifest 逐字节不变）。 */
  std::vector<LayerComp> layer_comps;
  /* 复合 id 分配器：与图层 id 一样单调递增，删除后不复用空洞（否则撤销/重做
   * 与宿主持有的旧 id 会指到另一份复合上）。 */
  uint32_t next_layer_comp_id = 1;
};

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_DOMAIN_STATE_H */
