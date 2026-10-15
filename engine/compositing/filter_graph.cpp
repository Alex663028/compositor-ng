// AI生成
#include "compositing/filter_graph.h"

#include <functional>
#include <utility>

#include "compositing/composite_ops.h"

namespace compositor {

namespace {

void set_error(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
}

/*
 * 建图器：按 parent 关系把扁平图层列表还原为树，并赋予每条 backdrop 边。
 * 兄弟顺序沿用 state.layers 的向量序（即 .comp 的堆叠序：索引 0 在最下）。
 */
struct GraphBuilder {
  const Document& doc;
  FilterGraph* graph;
  std::string* error;
  size_t visited = 0;

  std::vector<const Layer*> children_of(comp_layer_id parent) const {
    std::vector<const Layer*> result;
    for (const Layer& layer : doc.state().layers) {
      if (layer.parent == parent) result.push_back(&layer);
    }
    return result;
  }

  bool add_children(comp_layer_id parent_layer, int32_t parent_index, int32_t backdrop,
                    int32_t depth) {
    if (depth > kFilterGraphMaxDepth) {
      set_error(error, "layer nesting exceeds depth limit");
      return false;
    }
    int32_t previous = backdrop;
    for (const Layer* layer : children_of(parent_layer)) {
      const int32_t index = static_cast<int32_t>(graph->nodes.size());
      FilterNode node;
      node.layer = layer->id;
      node.kind = layer->is_group ? FilterNodeKind::kGroup : FilterNodeKind::kSource;
      node.isolated = layer->is_group && layer->isolated;
      node.parent = parent_index;
      node.backdrop = previous;
      graph->nodes.push_back(node);
      graph->nodes[parent_index].inputs.push_back(index);
      ++visited;

      if (layer->is_group) {
        /* 穿透组：子树沿用组的下层，实现混合穿透；隔离组：子树自起一张透明画布。 */
        const int32_t child_backdrop = node.isolated ? -1 : previous;
        if (!add_children(layer->id, index, child_backdrop, depth + 1)) return false;
      }
      previous = index;
    }
    return true;
  }

  bool run() {
    graph->nodes.clear();
    graph->output = -1;

    FilterNode root;
    root.layer = 0;
    root.kind = FilterNodeKind::kGroup;
    root.isolated = false;
    root.parent = -1;
    root.backdrop = -1;
    graph->nodes.push_back(root);
    graph->output = 0;

    if (!add_children(0, 0, -1, 1)) return false;

    if (visited != doc.state().layers.size()) {
      /* 有图层挂在不存在/非组的父级下，或层级存在环 —— 一律拒绝，不静默丢层。 */
      set_error(error, "layer hierarchy contains orphaned or cyclic nodes");
      return false;
    }
    return true;
  }
};

/*
 * 递归求值：把节点 index 叠加到 backdrop 之上，结果写入 out。
 * opacity_scale 为自顶层穿透组累积下来的不透明度系数。
 */
bool eval_node(const Document& doc, const FilterGraph& graph, int32_t index,
               const ImageBuffer& backdrop, double opacity_scale, ImageBuffer* out,
               std::string* error) {
  if (index < 0 || index >= static_cast<int32_t>(graph.nodes.size())) {
    set_error(error, "graph node index out of range");
    return false;
  }
  const FilterNode& node = graph.nodes[index];
  /*
   * 索引 0 是虚拟根（layer == 0），代表「文档画布」这一容器本身，没有对应图层。
   * 注意 Document::find(0) 按约定返回 nullptr，因此根节点必须与「图层缺失」区分开，
   * 否则整篇合成会在第一步就误判为缺层而失败。
   */
  const bool is_root = (node.layer == 0);
  const Layer* layer = is_root ? nullptr : doc.find(node.layer);
  if (!is_root && layer == nullptr) {
    set_error(error, "graph node references a missing layer");
    return false;
  }

  if (node.kind == FilterNodeKind::kSource) {
    if (layer == nullptr) {
      set_error(error, "source node has no layer");
      return false;
    }
    *out = backdrop;
    if (!blend_layer_into(out, doc, *layer, opacity_scale)) {
      set_error(error, "blend layer failed");
      return false;
    }
    return true;
  }

  /* 图层组：隐藏组连同其整棵子树都不参与求值（虚拟根恒可见）。 */
  if (layer != nullptr && layer->visible == false) {
    *out = backdrop;
    return true;
  }

  const double group_opacity = layer != nullptr ? static_cast<double>(layer->opacity) : 1.0;

  if (!node.isolated) {
    /* 穿透：子层直接叠到组的下层之上（混合穿透），组不透明度乘进子层。 */
    ImageBuffer acc = backdrop;
    const double child_scale = opacity_scale * group_opacity;
    for (int32_t input : node.inputs) {
      ImageBuffer next;
      if (!eval_node(doc, graph, input, acc, child_scale, &next, error)) return false;
      acc = std::move(next);
    }
    *out = std::move(acc);
    return true;
  }

  /* 隔离：子树先合成到透明缓冲，再作为整体按组的混合模式与不透明度叠加。 */
  ImageBuffer internal;
  internal.resize(doc.state().width, doc.state().height, COMP_PIXEL_RGBA8);
  internal.clear_transparent();
  for (int32_t input : node.inputs) {
    ImageBuffer next;
    if (!eval_node(doc, graph, input, internal, opacity_scale, &next, error)) return false;
    internal = std::move(next);
  }
  *out = backdrop;
  const comp_blend_mode group_blend = layer != nullptr ? layer->blend : COMP_BLEND_NORMAL;
  if (!blend_buffer_into(out, internal, group_blend, group_opacity * opacity_scale)) {
    set_error(error, "blend group buffer failed");
    return false;
  }
  return true;
}

}  // namespace

bool build_filter_graph(const Document& doc, FilterGraph* graph, std::string* error) {
  if (graph == nullptr) {
    set_error(error, "graph is null");
    return false;
  }
  GraphBuilder builder{doc, graph, error};
  return builder.run();
}

bool execute_filter_graph(const Document& doc, const FilterGraph& graph, ImageBuffer* out,
                          std::string* error) {
  if (out == nullptr) {
    set_error(error, "out buffer is null");
    return false;
  }
  const DocumentState& state = doc.state();
  if (state.width <= 0 || state.height <= 0) {
    set_error(error, "document canvas is empty");
    return false;
  }

  ImageBuffer backdrop;
  backdrop.resize(state.width, state.height, COMP_PIXEL_RGBA8);
  backdrop.clear_transparent();

  if (graph.output < 0 || graph.nodes.empty()) {
    *out = std::move(backdrop);
    return true;
  }

  ImageBuffer result;
  if (!eval_node(doc, graph, graph.output, backdrop, 1.0, &result, error)) return false;
  *out = std::move(result);
  return true;
}

}  // namespace compositor
