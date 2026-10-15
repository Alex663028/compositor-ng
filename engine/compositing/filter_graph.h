// AI生成
/*
 * filter_graph.h — 合成求值有向图（Directed Acyclic Graph）。
 *
 * 背景：M1 的合成器按 state.layers 的扁平顺序线性叠加，且把图层组当作完全
 * 透明的一层跳过。这带来两个缺陷：
 *   1) 图层树的嵌套与同级顺序不参与求值（层级被压平，组内像素没有正确的下层）；
 *   2) 组的不透明度被完全忽略——上游 Compositor 的语义是「组的 opacity 乘进
 *      组内每一层」（见上游Document/LayerGroups.swift 的 LayerOpacity）。
 *
 * 本模块把合成升级为有向图：
 *   - 每个图层 / 图层组是一个节点；节点的 inputs 为子节点（自下而上的绘制序）；
 *   - 节点的 backdrop 指向「绘制它时下方结果」所在的节点，形成一条自左向右的
 *     依赖链（deps 方向为 backdrop/inputs -> 本节点），整体是一张 DAG；
 *   - 求值按拓扑序递归：节点输出 = 把自身（或其子树聚合）叠加到 backdrop 上。
 *
 * 组语义（对齐上游，默认穿透）：
 *   - 穿透组（isolated == false，默认）：组不产生独立缓冲，组内子层直接叠加到
 *     组下方的 backdrop 上——即混合模式「穿透」组边界，与组下方内容交互；组的
 *     不透明度乘进每个子层的有效不透明度（0.5 x 0.5 = 0.25）；组自身的混合模式
 *     不参与求值。
 *   - 隔离组（isolated == true）：子树先合成到一张透明缓冲，作为一个整体再与
 *     backdrop 混合——此时组的混合模式与不透明度作用在「组结果」这一个单元上。
 */
#ifndef COMPOSITOR_COMPOSITING_FILTER_GRAPH_H
#define COMPOSITOR_COMPOSITING_FILTER_GRAPH_H

#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "domain/document.h"
#include "domain/layer.h"

namespace compositor {

/* 图节点类别：像素源（叶子图层）与图层组（子树聚合）。 */
enum class FilterNodeKind {
  kSource = 0, /* 普通像素图层 */
  kGroup = 1,  /* 图层组 */
};

struct FilterNode {
  comp_layer_id layer = 0; /* 对应图层 id；文档根节点为 0 */
  FilterNodeKind kind = FilterNodeKind::kSource;
  bool isolated = false;             /* 仅图层组：true=隔离，false=穿透 */
  int32_t parent = -1;               /* 父节点索引；文档根为 -1 */
  std::vector<int32_t> inputs;       /* 子节点索引（自下而上的绘制序） */
  int32_t backdrop = -1;             /* 绘制下层结果所在节点索引；-1=透明画布 */
};

struct FilterGraph {
  std::vector<FilterNode> nodes; /* 索引 0 恒为文档根节点 */
  int32_t output = -1;           /* 求值出口（根节点索引） */
};

/* 图层嵌套深度上限，与上游 LayerHierarchy 的 64 层限制保持一致。 */
constexpr int32_t kFilterGraphMaxDepth = 64;

/*
 * 从文档图层树构建有向图。
 * 失败（图层存在无法解析的父级 / 嵌套超过深度上限 / 存在孤立节点）时返回 false
 * 并写入 error——绝不静默丢弃图层。
 */
bool build_filter_graph(const Document& doc, FilterGraph* graph, std::string* error);

/* 按拓扑序求值；out 会被重置为画布尺寸的 RGBA8 缓冲。 */
bool execute_filter_graph(const Document& doc, const FilterGraph& graph, ImageBuffer* out,
                          std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_FILTER_GRAPH_H */
