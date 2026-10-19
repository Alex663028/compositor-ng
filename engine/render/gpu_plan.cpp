// AI生成
#include "render/gpu_plan.h"

#include <utility>

#include "compositing/blend.h"
#include "compositing/composite_ops.h"
#include "compositing/compositor.h"
#include "compositing/filter_graph.h"
#include "core/comp_geometry.h"

namespace compositor {

namespace {

void set_error(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
}

/*
 * 能否把该像素图层原样上传成一张源纹理（原生单元）：
 * 整幅画布、RGBA8、无蒙版/效果/调整、无旋转翻转、几何为恒等。
 * 其余一律走 CPU 烘焙——烘焙路径与 CPU 合成器同函数，不在 GPU 侧重写语义。
 */
bool is_native_layer(const Document& doc, const Layer& layer) {
  const DocumentState& state = doc.state();
  if (layer.is_group || !layer.has_pixels()) return false;
  if (layer.pixels.format != COMP_PIXEL_RGBA8) return false;
  if (layer.pixels.width != state.width || layer.pixels.height != state.height) return false;
  if (!layer.mask.empty() || !layer.effects.empty() || !layer.adjustments.empty()) return false;
  if (layer.rotation != 0.0f || layer.flip_x || layer.flip_y) return false;
  const comp_rect bounds = layer.effective_bounds();
  return bounds.x == 0 && bounds.y == 0 && bounds.w == state.width && bounds.h == state.height;
}

/*
 * 计划构建器：按 filter_graph 的绘制序递归展开单元。
 *
 * 语义对照 CPU 求值（filter_graph.cpp / composite_ops.cpp）：
 *   - 隐藏图层与整棵子树不参与合成（eval_node 的可见性检查在最前面）；
 *   - 穿透组：子层直接叠加到组下方，组的不透明度乘进每个子层，组自身混合模式
 *     不参与（因此展开成子单元 + 累积系数）；
 *   - 隔离组 / 剪贴栈：作为**一个**单元整体烘焙，单元自身的混合模式与
 *     不透明度在 GPU 混合时生效，与 eval_node 的最后一步同口径。
 */
struct Planner {
  const Document& doc;
  const FilterGraph& graph;
  std::vector<GpuUnit>* units;
  GpuPlanStats stats;
  std::string* error;
  bool ok = true;

  void fail(std::string message) {
    if (!ok) return;
    ok = false;
    set_error(error, std::move(message));
  }

  void append_units(int32_t index, double opacity_scale, int32_t depth) {
    if (!ok) return;
    if (depth > kFilterGraphMaxDepth) {
      fail("gpu plan: layer nesting exceeds depth limit");
      return;
    }
    if (index < 0 || index >= static_cast<int32_t>(graph.nodes.size())) {
      fail("gpu plan: graph node index out of range");
      return;
    }
    const FilterNode& node = graph.nodes[static_cast<size_t>(index)];
    const Layer* layer = doc.find(node.layer);
    if (layer == nullptr) {
      fail("gpu plan: graph node references a missing layer");
      return;
    }
    if (!layer->visible) return;

    switch (node.kind) {
      case FilterNodeKind::kSource:
        append_source_unit(*layer, opacity_scale);
        return;
      case FilterNodeKind::kClippingStack:
        append_clipping_stack_unit(index, *layer, opacity_scale);
        return;
      case FilterNodeKind::kGroup:
        if (node.isolated) {
          append_isolated_group_unit(index, *layer, opacity_scale);
          return;
        }
        /* 穿透组：展开子节点，组不透明度乘进每个子层（组的混合模式不参与）。 */
        ++stats.pass_through_groups;
        for (int32_t input : node.inputs) {
          append_units(input, opacity_scale * static_cast<double>(layer->opacity), depth + 1);
        }
        return;
    }
    fail("gpu plan: unknown filter node kind");
  }

  void append_source_unit(const Layer& layer, double opacity_scale) {
    if (!layer.adjustments.empty()) {
      /*
       * 调整层重着色的是「它下方的已合成画布」，不是自己的像素贡献：
       * 没有新图层的像素，也就无法作为独立单元烘焙。这是 S17 明确保留的
       * 整篇回落条件（分段回读方案见 docs/gpu-layer-revision.md 的「后续项」）。
       */
      fail("gpu plan: adjustment layers re-colour the composite below them and are not a "
           "per-layer contribution yet");
      return;
    }
    if (!layer.has_pixels() || layer.is_group) return;
    if (is_empty(layer.effective_bounds())) return; /* 完全在画布外：无贡献 */
    if (!is_blend_supported(layer.blend)) return;   /* CPU 同口径：跳过该层 */

    const bool native = is_native_layer(doc, layer);
    if (native) {
      ++stats.native_units;
    } else {
      ++stats.baked_units;
    }
    if (units == nullptr) return;

    GpuUnit unit;
    unit.blend = layer.blend;
    if (native) {
      unit.native_layer = &layer;
      unit.opacity = static_cast<float>(static_cast<double>(layer.opacity) * opacity_scale);
    } else {
      /*
       * 烘焙的像素源图层：bake_layer_contribution 已把不透明度（含穿透组
       * 累积系数）、蒙版、效果、剪贴系数折进预乘像素，GPU 侧只做原混合模式
       * + alpha=1 的合成，避免二次乘 alpha。
       */
      if (!bake_layer_contribution(doc, layer, opacity_scale, &unit.baked, error)) {
        ok = false;
        return;
      }
      unit.opacity = 1.0f;
    }
    units->push_back(std::move(unit));
  }

  void append_clipping_stack_unit(int32_t index, const Layer& base, double opacity_scale) {
    if (!is_blend_supported(base.blend)) return; /* CPU 同口径：整栈无贡献 */
    ++stats.clipping_stacks;
    if (units == nullptr) return;
    GpuUnit unit;
    unit.blend = base.blend;
    unit.opacity = static_cast<float>(static_cast<double>(base.opacity) * opacity_scale);
    if (!bake_filter_unit_surface(doc, graph, index, opacity_scale, &unit.baked, error)) {
      ok = false;
      return;
    }
    if (unit.baked.empty()) return; /* 隐藏 / 空 bounds：整栈无贡献 */
    units->push_back(std::move(unit));
  }

  void append_isolated_group_unit(int32_t index, const Layer& group, double opacity_scale) {
    if (!is_blend_supported(group.blend)) return; /* CPU 同口径：整组无贡献 */
    ++stats.isolated_groups;
    if (units == nullptr) return;
    GpuUnit unit;
    unit.blend = group.blend;
    unit.opacity = static_cast<float>(static_cast<double>(group.opacity) * opacity_scale);
    if (!bake_filter_unit_surface(doc, graph, index, opacity_scale, &unit.baked, error)) {
      ok = false;
      return;
    }
    if (unit.baked.empty()) return; /* 隐藏组 */
    units->push_back(std::move(unit));
  }
};

}  // namespace

bool build_gpu_plan(const Document& doc, std::vector<GpuUnit>* units, GpuPlanStats* stats,
                    std::string* error) {
  if (units != nullptr) units->clear();
  const DocumentState& state = doc.state();
  if (state.width <= 0 || state.height <= 0) {
    set_error(error, "gpu plan: document canvas is empty");
    return false;
  }
  /*
   * GPU 内核只实现 8 位目标。HDR（RGBA16F 表面）文档先回落 CPU：如果只在
   * 8 位目标上近似，结果会和 CPU 的 half 合成公式不同深度，等于静默降级。
   */
  if (composite_surface_format(doc) != COMP_PIXEL_RGBA8) {
    set_error(error, "gpu plan: HDR (half-float) surfaces are outside the GPU subset");
    return false;
  }

  FilterGraph graph;
  if (!build_filter_graph(doc, &graph, error)) {
    return false;
  }
  if (graph.output < 0 || graph.output >= static_cast<int32_t>(graph.nodes.size())) {
    set_error(error, "gpu plan: filter graph has no output");
    return false;
  }

  Planner planner{doc, graph, units, GpuPlanStats{}, error, true};
  const FilterNode& root = graph.nodes[static_cast<size_t>(graph.output)];
  for (int32_t input : root.inputs) {
    planner.append_units(input, 1.0, 1);
    if (!planner.ok) return false;
  }
  if (stats != nullptr) *stats = planner.stats;
  return true;
}

}  // namespace compositor
