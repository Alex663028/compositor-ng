// AI生成
#include "compositing/filter_graph.h"

#include <functional>
#include <utility>

#include "compositing/composite_ops.h"
#include "compositing/adjustment_eval.h"
#include "compositing/blend.h"
#include "compositing/compositor.h"
#include "imaging/DeepPixels.h"

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
    const std::vector<const Layer*> siblings = children_of(parent_layer);
    int32_t previous = backdrop;
    /* 按下标走：剪贴子层要与基底合成一个栈节点，因此「若干个」兄弟只建一个节点。 */
    for (size_t i = 0; i < siblings.size(); ++i) {
      const Layer* layer = siblings[i];

      /*
       * 剪贴子层不单独建节点：它必须与基底同节点求值（kClippingStack）。
       * 基底就是它在兄弟列表里的前一个图层（同父，layer_mutation 测试钉过
       * comp_layer_set_clipping_source 不允许跨父）。因此在**基底处**就往后看：
       * 收「紧跟着的一串剪到这个基底的兄弟」建一个栈节点，中间夹了非成员就收尾，
       * 与上游 stacks[id] 按基底分组、遇非成员即止的口径一致。
       *
       * **隐藏的成员不进栈**：eval_node 会把它当 backdrop 透传，那等于给栈
       * 加了一层「什么都不做的成员」——render 不变，但成员白占一个节点；
       * 更重要的是它会中断本该连续的成员串（隐藏层夹在中间就不再收后面的），
       * 于是隐藏一个剪贴子层会改变栈的构成。隐藏成员在求值层跳过即可（上面）。
       */
      size_t clip_run = 0;
      while (i + 1 + clip_run < siblings.size() &&
             siblings[i + 1 + clip_run]->clipping_source == layer->id &&
             siblings[i + 1 + clip_run]->visible) {
        ++clip_run;
      }

      if (clip_run > 0) {
        const int32_t stack_index = static_cast<int32_t>(graph->nodes.size());
        FilterNode stack_node;
        stack_node.layer = layer->id;
        stack_node.kind = FilterNodeKind::kClippingStack;
        stack_node.isolated = false;
        stack_node.parent = parent_index;
        stack_node.backdrop = previous;
        graph->nodes.push_back(stack_node);
        graph->nodes[parent_index].inputs.push_back(stack_index);

        /* inputs[0] = 基底（铺底的那一层），随后依次是被剪的成员（自下而上）。 */
        for (size_t m = 0; m <= clip_run; ++m) {
          const Layer* member = siblings[i + m];
          const int32_t member_index = static_cast<int32_t>(graph->nodes.size());
          FilterNode member_node;
          member_node.layer = member->id;
          member_node.kind = member->is_group ? FilterNodeKind::kGroup : FilterNodeKind::kSource;
          member_node.isolated = member->is_group && member->isolated;
          member_node.parent = stack_index;
          /*
           * 栈内成员**不互为下层**：每个成员自起一张透明画布，求值后由栈节点
           * 把它们逐个叠到「不透明基底」上（见 eval_node 的 kClippingStack 分支）。
           * 若让它们互为下层，就会退化成逐像素 alpha 缩放——那正是本节要修的旧行为。
           */
          member_node.backdrop = -1;
          graph->nodes.push_back(member_node);
          graph->nodes[stack_index].inputs.push_back(member_index);
          ++visited;

          if (member->is_group) {
            const int32_t child_backdrop = member_node.isolated ? -1 : -1;
            if (!add_children(member->id, member_index, child_backdrop, depth + 1)) return false;
          }
        }
        previous = stack_index;
        /* 隐藏的剪贴成员被跳过了，它仍需一个普通节点（求值时按不可见透传）。 */
        i += clip_run; /* 跳过已归栈的可见成员 */
        continue;
      }

      /*
       * 隐藏的剪贴子层：不建栈节点，按普通节点建（求值层会跳过），
       * 否则 visited 计数会与图层数对不上，被误报成「孤立或成环」。
       */
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
/* 栈内成员的混合模式（缺失 / 未找到图层时按 normal，与 blend_layer_into 同宽容）。 */
comp_blend_mode node_blend_mode(const Document& doc, const FilterGraph& graph, int32_t index) {
  if (index < 0 || index >= static_cast<int32_t>(graph.nodes.size())) return COMP_BLEND_NORMAL;
  const Layer* member = doc.find(graph.nodes[index].layer);
  return member != nullptr ? member->blend : COMP_BLEND_NORMAL;
}

/*
 * 一串蒙版的相乘（自身 × 祖先组，上游 folders:true 口径）。
 *
 * 空蒙版按 1.0 透传，否则「某层没蒙版」会把下层的遮罩清掉。每张蒙版按**自己的
 * 尺寸**取样（超出范围的像素按 1.0），因为上游的 live-mask 蒙版可以比画布小。
 */
struct CoverageMaps {
  std::vector<const MaskBuffer*> maps;

  void add(const MaskBuffer* map) {
    if (map != nullptr && !map->empty()) maps.push_back(map);
  }

  double at(int32_t x, int32_t y) const {
    double coverage = 1.0;
    for (const MaskBuffer* map : maps) {
      if (x < 0 || y < 0 || x >= map->width || y >= map->height) continue;
      coverage *= static_cast<double>(map->row(y)[x]) / 255.0;
    }
    return coverage;
  }
};

/*
 * 把调整结果按遮罩与不透明度混回 backdrop。
 *
 * 调整层是「重新着色下层」，所以这里不是 source-over：每个像素先按 coverage
 * 从 backdrop 取色，再换成 adjusted 的颜色（预乘按 coverage 缩放），alpha 保持
 * backdrop 原样——与上游 EditorCanvas.swift:3096-3106 “takes back the original
 * coverage” 一致。
 */
void mix_adjusted_through_coverage(ImageBuffer* dst, const ImageBuffer& adjusted, double opacity,
                                   const CoverageMaps& coverage) {
  if (dst == nullptr || dst->empty()) return;
  for (int32_t y = 0; y < dst->height; ++y) {
    uint8_t* dst_row = dst->writable_row(y);
    const uint8_t* src_row = adjusted.row(static_cast<size_t>(y));
    for (int32_t x = 0; x < dst->width; ++x) {
      const double strength = coverage.at(x, y) * opacity;
      if (strength <= 0.0) continue;
      uint8_t* px = dst_row + static_cast<size_t>(x) * 4;
      const uint8_t* src = src_row + static_cast<size_t>(x) * 4;
      /*
       * 强度为 1 时**直接写 src**，不走插值。
       *
       * 不这么做就不是「精确取反」：对预乘像素做 lerp 会把舍入误差留在结果里，
       * 而 invert 这类逐通道映射在 alpha=255 处必须逐字节等于 255 - 原值
       * （test_adjustment_eval 的 invert 断言逐字节相等）。走一次浮点再取整，
       * 在半透明像素上就会差 1。
       */
      if (strength >= 1.0) {
        px[0] = src[0];
        px[1] = src[1];
        px[2] = src[2];
        continue; /* alpha 不动：调整只改颜色，coverage 属于下层。 */
      }
      for (int channel = 0; channel < 3; ++channel) {
        const int32_t mixed = static_cast<int32_t>(px[channel]) +
                              static_cast<int32_t>((static_cast<double>(src[channel]) -
                                                   static_cast<double>(px[channel])) * strength +
                                                  0.5);
        px[channel] = static_cast<uint8_t>(mixed < 0 ? 0 : (mixed > 255 ? 255 : mixed));
      }
      /* alpha 不动 */
    }
  }
}

/*
 * 非 normal 混合模式下的调整层（上游 EditorCanvas.swift:3096-3106）：
 * 先把 adjusted 与 backdrop 按**全不透明**混一次，再按 below 的 alpha 取回原
 * coverage。逐点 scale alpha 的做法（clipping_factor 那种）在非 normal 下会
 * 把调整层自己也变半透明，得到的不是同一个颜色。
 */
bool blend_adjusted_in_blend_mode(const ImageBuffer& adjusted, const ImageBuffer& below,
                                 comp_blend_mode mode, ImageBuffer* out) {
  if (out == nullptr || adjusted.empty() || below.empty()) return false;
  if (adjusted.width != below.width || adjusted.height != below.height) return false;
  if (!is_blend_supported(mode)) return false;
  out->resize(adjusted.width, adjusted.height, COMP_PIXEL_RGBA8);
  for (int32_t y = 0; y < out->height; ++y) {
    uint8_t* dst_row = out->writable_row(static_cast<size_t>(y));
    const uint8_t* adj_row = adjusted.row(static_cast<size_t>(y));
    const uint8_t* below_row = below.row(static_cast<size_t>(y));
    for (int32_t x = 0; x < out->width; ++x) {
      uint8_t* px = dst_row + static_cast<size_t>(x) * 4;
      const uint8_t* adj = adj_row + static_cast<size_t>(x) * 4;
      /* 全不透明基底：alpha 统一置 255 再混合，否则非 normal 模式的结果随 below
       * 的半透明度变深，与上游「opaque blend then re-mask」不是一回事。 */
      uint8_t opaque_above[4] = {adj[0], adj[1], adj[2], 255};
      uint8_t opaque_below[4] = {below_row[static_cast<size_t>(x) * 4 + 0],
                                 below_row[static_cast<size_t>(x) * 4 + 1],
                                 below_row[static_cast<size_t>(x) * 4 + 2], 255};
      uint8_t mixed[4] = {opaque_below[0], opaque_below[1], opaque_below[2], 255};
      blend_pixels(mixed, opaque_above, mode, 1.0);
      px[0] = mixed[0];
      px[1] = mixed[1];
      px[2] = mixed[2];
      px[3] = below_row[static_cast<size_t>(x) * 4 + 3]; /* 取回 below 的 coverage */
    }
  }
  return true;
}

/*
 * C5：half 表面上的同一对助手。
 *
 * 与 8 位版逐行同构，只把像素读写换成 half。两条路必须给出同一套
 * 「coverage/不透明度如何作用」的语义，否则同一个调整图层在 HDR 文档里
 * 会与 8 位文档分叉；差别只在颜色是浮点、且**不夹到 [0,1]**。
 */
void mix_adjusted_through_coverage_hdr(ImageBuffer* dst, const ImageBuffer& adjusted,
                                       double opacity, const CoverageMaps& coverage) {
  if (dst == nullptr || dst->empty()) return;
  for (int32_t y = 0; y < dst->height; ++y) {
    uint16_t* dst_row = reinterpret_cast<uint16_t*>(dst->writable_row(y));
    const uint16_t* src_row = reinterpret_cast<const uint16_t*>(adjusted.row(y));
    for (int32_t x = 0; x < dst->width; ++x) {
      const double strength = coverage.at(x, y) * opacity;
      if (strength <= 0.0) continue;
      uint16_t* px = dst_row + static_cast<size_t>(x) * 4u;
      const uint16_t* src = src_row + static_cast<size_t>(x) * 4u;
      if (strength >= 1.0) {
        px[0] = src[0];
        px[1] = src[1];
        px[2] = src[2];
        continue; /* alpha 不动：调整只改颜色，coverage 属于下层。 */
      }
      for (int channel = 0; channel < 3; ++channel) {
        const double from = static_cast<double>(deep_half_value(px[channel]));
        const double to = static_cast<double>(deep_half_value(src[channel]));
        px[channel] = deep_half_bits(static_cast<float>(from + (to - from) * strength));
      }
      /* alpha 不动 */
    }
  }
}

/* half 版：全不透明混一次、再取回 below 的 alpha（见 8 位版注释）。 */
bool blend_adjusted_in_blend_mode_hdr(const ImageBuffer& adjusted, const ImageBuffer& below,
                                      comp_blend_mode mode, ImageBuffer* out) {
  if (out == nullptr || adjusted.empty() || below.empty()) return false;
  if (adjusted.width != below.width || adjusted.height != below.height) return false;
  if (!is_blend_supported(mode)) return false;
  out->resize(adjusted.width, adjusted.height, COMP_PIXEL_RGBA16F);
  const uint16_t one = deep_half_bits(1.0f);
  for (int32_t y = 0; y < out->height; ++y) {
    uint16_t* dst_row = reinterpret_cast<uint16_t*>(out->writable_row(y));
    const uint16_t* adj_row = reinterpret_cast<const uint16_t*>(adjusted.row(y));
    const uint16_t* below_row = reinterpret_cast<const uint16_t*>(below.row(y));
    for (int32_t x = 0; x < out->width; ++x) {
      uint16_t* px = dst_row + static_cast<size_t>(x) * 4u;
      const uint16_t* adj = adj_row + static_cast<size_t>(x) * 4u;
      const uint16_t* below_px = below_row + static_cast<size_t>(x) * 4u;
      uint16_t opaque_above[4] = {adj[0], adj[1], adj[2], one};
      uint16_t mixed[4] = {below_px[0], below_px[1], below_px[2], one};
      blend_pixels_half(mixed, opaque_above, mode, 1.0);
      px[0] = mixed[0];
      px[1] = mixed[1];
      px[2] = mixed[2];
      px[3] = below_px[3]; /* 取回 below 的 coverage */
    }
  }
  return true;
}

/*
 * S17：两个「独立合成单元」的表面求值入口，供 eval_node 与 GPU 后端的单元烘焙共用。
 *
 * 它们与 eval_node 的对应分支**同源**：把表面算出来、但不与 backdrop 做最后一步
 * 整单元混合（那一步的混合模式/不透明度属于调用方语义）。抽出来是为了让
 * 「CPU 求值」与「GPU 后端把复杂节点烘焙成单元再交给设备」不会各写一份公式。
 */
bool eval_node(const Document& doc, const FilterGraph& graph, int32_t index,
               const ImageBuffer& backdrop, double opacity_scale, ImageBuffer* out,
               std::string* error);
bool build_clipping_stack_surface(const Document& doc, const FilterGraph& graph, int32_t index,
                                  double opacity_scale, ImageBuffer* out, std::string* error);
bool build_isolated_group_surface(const Document& doc, const FilterGraph& graph, int32_t index,
                                  double opacity_scale, ImageBuffer* out, std::string* error);

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

  /*
   * 隐藏图层连同整棵子树不参与求值（虚拟根 layer==0 恒可见）。
   *
   * 这个检查必须在剪贴栈分支**之前**：隐藏的基底会让整个栈无贡献，
   * 而隐藏的成员也不能把基底的 alpha 取走——否则「隐藏一个剪贴子层」会
   * 改变可见基底的 alpha（上游 toggleLayerVisibility 逐字节回到隐藏前）。
   */
  if (!is_root && layer->visible == false) {
    *out = backdrop;
    return true;
  }

  /*
   * 剪贴栈（上游 LiveMaskRenderer.drawComposite:110-150）。
   * inputs[0] 是基底，inputs[1..] 是被剪的成员（自下而上）。
   *
   * 严格按上游的顺序求值：
   *   1. 基底合成到栈表面；
   *   2. extract——alpha 存进 coverage，颜色拉成不透明；
   *   3. 每个成员在**不透明**基底上按各自混合模式叠（成员各自自起画布，
   *      所以不会退化成「按基底 alpha 逐像素缩放」）；
   *   4. restore——alpha 放回；
   *   5. 整个栈以**基底的**混合模式与不透明度参与叠加（上游 `stackModes[id]`）。
   */
  if (node.kind == FilterNodeKind::kClippingStack) {
    /* 剪贴栈表面：extract → 成员 → restore 由共用实现算好（S17 抽出）。 */
    ImageBuffer surface;
    if (!build_clipping_stack_surface(doc, graph, index, opacity_scale, &surface, error)) {
      return false;
    }
    /* 第五步：整个栈以基底的模式与不透明度参与。 */
    *out = backdrop;
    if (surface.empty()) return true; /* 隐藏 / 空 bounds：整个栈无贡献 */
    const double stack_opacity = static_cast<double>(layer->opacity) * opacity_scale;
    if (!blend_buffer_into(out, surface, layer->blend, stack_opacity)) {
      set_error(error, "blend clipping stack failed");
      return false;
    }
    return true;
  }

  if (node.kind == FilterNodeKind::kSource) {
    if (layer == nullptr) {
      set_error(error, "source node has no layer");
      return false;
    }

    /*
     * 调整图层是滤镜节点，不是像素图层（对齐上游 EditorCanvas.swift:3092
     * “An adjustment re-colors what's under it”）：
     *   它不对自身像素做变换，而是把「下层已合成结果（backdrop）」过一遍调整。
     *   因此带调整的图层不贡献自己的像素；若它同时带像素（上游允许一个图层
     *   既有 asset 又有 adjustment），像素仍然正常叠加，调整只作用在叠加之后。
     */
    if (!layer->adjustments.empty()) {
      ImageBuffer adjusted = backdrop;
      const bool adjusted_half = adjusted.format == COMP_PIXEL_RGBA16F;
      AdjustmentEvalResult eval;
      std::string eval_error;
      uint8_t* adjusted_pixels = adjusted.writable_pixels().data();
      /*
       * C5：half 表面走 apply_adjustments_hdr（曝光直接落 half；跨通道调整
       * normalize → 16 位往返 → restore）。8 位表面仍走原来那条路，
       * 逐字节不变——这是 C5 的验收底线。
       */
      const bool applied =
          adjusted_half
              ? apply_adjustments_hdr(layer->adjustments,
                                      reinterpret_cast<uint16_t*>(adjusted_pixels), adjusted.width,
                                      adjusted.height, adjusted.stride(), &eval, &eval_error)
              : apply_adjustments(layer->adjustments, adjusted_pixels, adjusted.width,
                                  adjusted.height, adjusted.stride(), &eval, &eval_error);
      if (!applied) {
        set_error(error, eval_error.empty() ? "apply adjustment failed" : eval_error);
        return false;
      }
      /* 上游在非 normal 混合模式下会先把调整结果与下层按全不透明混一次、再按
       * below 的 alpha 取回原 coverage（EditorCanvas.swift:3096-3106）。 */
      *out = backdrop;
      if (eval.changed) {
        const double layer_opacity = static_cast<double>(layer->opacity) * opacity_scale;
        if (layer->blend != COMP_BLEND_NORMAL) {
          ImageBuffer styled;
          const bool styled_ok =
              adjusted_half
                  ? blend_adjusted_in_blend_mode_hdr(adjusted, backdrop, layer->blend, &styled)
                  : blend_adjusted_in_blend_mode(adjusted, backdrop, layer->blend, &styled);
          if (styled_ok) {
            adjusted = std::move(styled);
          }
        }
        /* coverage = 自身蒙版 × 组蒙版链（上游 folders:true） × 不透明度。
         * 上游在剪贴栈内对子层用 folders:false（此时父组蒙版已作用在栈上）；
         * 本引擎的 FilterGraph 无剪贴栈节点，等价位置即「图层直接挂在组下」，
         * 一律按 folders:true 取组蒙版链（多乘一次不会错，因为组蒙版对非组
         * 调整层本就是它唯一的裁剪来源）。depth 上限沿用上游的 64。 */
        CoverageMaps coverage;
        coverage.add(&layer->mask);
        comp_layer_id parent_id = layer->parent;
        int depth = 0;
        while (parent_id != 0 && depth < 64) {
          const Layer* folder = doc.find(parent_id);
          if (folder == nullptr) break;
          if (folder->is_group && !folder->mask.empty()) coverage.add(&folder->mask);
          parent_id = folder->parent;
          ++depth;
        }
        if (adjusted_half) {
          mix_adjusted_through_coverage_hdr(out, adjusted, layer_opacity, coverage);
        } else {
          mix_adjusted_through_coverage(out, adjusted, layer_opacity, coverage);
        }
      }
    } else {
      *out = backdrop;
    }

    /* 达因透传：blend_layer_into 现在把 build_layer_unit 的内因（如
     * "layer unit too large"）带上来，保留回退文案仅作内因为空时的兼底。 */
    std::string blend_error;
    if (!blend_layer_into(out, doc, *layer, opacity_scale, &blend_error)) {
      set_error(error, blend_error.empty() ? "blend layer failed" : blend_error);
      return false;
    }
    return true;
  }

  /* 图层组：虚拟根之外的组到这里说明子树不参与求值（可见性已在前面判过）。 */
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
  if (!build_isolated_group_surface(doc, graph, index, opacity_scale, &internal, error)) {
    return false;
  }
  *out = backdrop;
  const comp_blend_mode group_blend = layer != nullptr ? layer->blend : COMP_BLEND_NORMAL;
  if (!blend_buffer_into(out, internal, group_blend, group_opacity * opacity_scale)) {
    set_error(error, "blend group buffer failed");
    return false;
  }
  return true;
}

/*
 * S17：剪贴栈表面（第一步基底 → 第二步 extract → 第三步成员 → 第四步 restore）。
 *
 * 与 eval_node 的 kClippingStack 分支同源；差别只有：本函数**不做第五步**
 * （整栈与 backdrop 的混合），因为调用方要用自己的混合模式/不透明度来做。
 * 无贡献（隐藏基底 / 空 bounds）时返回**空缓冲**而不是 backdrop。
 */
bool build_clipping_stack_surface(const Document& doc, const FilterGraph& graph, int32_t index,
                                  double opacity_scale, ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    set_error(error, "clipping stack surface destination is null");
    return false;
  }
  if (index < 0 || index >= static_cast<int32_t>(graph.nodes.size())) {
    set_error(error, "graph node index out of range");
    return false;
  }
  const FilterNode& node = graph.nodes[index];
  if (node.kind != FilterNodeKind::kClippingStack) {
    set_error(error, "graph node is not a clipping stack");
    return false;
  }
  const Layer* layer = doc.find(node.layer);
  if (layer == nullptr) {
    set_error(error, "clipping stack references a missing base layer");
    return false;
  }
  out->resize(0, 0, COMP_PIXEL_RGBA8);
  if (!layer->visible) return true;
  if (node.inputs.empty()) {
    set_error(error, "clipping stack has no base input");
    return false;
  }
  const comp_rect stack_bounds = layer->effective_bounds();
  if (is_empty(stack_bounds)) return true; /* 基底为空：整个栈无贡献 */

  const comp_pixel_format canvas_format = composite_surface_format(doc);
  const bool half_surface = canvas_format == COMP_PIXEL_RGBA16F;

  /*
   * 第一步：基底。栈内成员节点的 backdrop 是 -1（它们不互为下层，只过
   * extract/restore），所以这里显式给一张画布大小的透明图当基底的下层。
   */
  ImageBuffer base_backdrop;
  base_backdrop.resize(doc.state().width, doc.state().height, canvas_format);
  base_backdrop.clear_transparent();
  ImageBuffer base_surface;
  if (!eval_node(doc, graph, node.inputs[0], base_backdrop, opacity_scale, &base_surface, error)) {
    return false;
  }
  if (base_surface.empty() || base_surface.width != doc.state().width ||
      base_surface.height != doc.state().height) {
    set_error(error, "clipping stack base produced an empty surface");
    return false;
  }
  ImageBuffer surface = std::move(base_surface);

  /* 第二步：取走 alpha、颜色拉成不透明（coverage 容器与表面同深度）。 */
  std::vector<uint16_t> coverage;
  std::vector<float> coverage_hdr;
  const bool extracted = half_surface ? extract_stack_alpha(&surface, &coverage_hdr, error)
                                      : extract_stack_alpha(&surface, &coverage, error);
  if (!extracted) return false;

  /* 第三步：成员在**不透明**基底上按各自模式叠（成员自起透明画布）。 */
  ImageBuffer member_backdrop;
  member_backdrop.resize(doc.state().width, doc.state().height, canvas_format);
  member_backdrop.clear_transparent();
  for (size_t i = 1; i < node.inputs.size(); ++i) {
    ImageBuffer member_surface;
    if (!eval_node(doc, graph, node.inputs[i], member_backdrop, opacity_scale, &member_surface,
                   error)) {
      return false;
    }
    /*
     * 成员深度必须与栈表面一致：不同深度按同一 stride 混合会把 half 的
     * 8 字节行当 RGBA8 的 4 字节行读成垃圾。宁可具名失败，也不静默出错色。
     */
    if (member_surface.format != surface.format) {
      set_error(error, "clipping stack member depth does not match the stack surface");
      return false;
    }
    const comp_blend_mode mode = node_blend_mode(doc, graph, node.inputs[i]);
    const int32_t bpp = half_surface ? 8 : 4;
    for (int32_t y = 0; y < surface.height; ++y) {
      uint8_t* dst_row = surface.writable_row(static_cast<size_t>(y));
      const uint8_t* src_row = member_surface.row(static_cast<size_t>(y));
      if (half_surface) {
        for (int32_t x = 0; x < surface.width; ++x) {
          blend_pixels_half(reinterpret_cast<uint16_t*>(dst_row + static_cast<size_t>(x) * bpp),
                            reinterpret_cast<const uint16_t*>(src_row +
                                                              static_cast<size_t>(x) * bpp),
                            mode, 1.0);
        }
      } else {
        for (int32_t x = 0; x < surface.width; ++x) {
          blend_pixels(dst_row + static_cast<size_t>(x) * bpp,
                       src_row + static_cast<size_t>(x) * bpp, mode, 1.0);
        }
      }
    }
  }

  /* 第四步：还原 alpha。 */
  const bool restored = half_surface ? restore_stack_alpha(&surface, coverage_hdr, error)
                                     : restore_stack_alpha(&surface, coverage, error);
  if (!restored) return false;
  *out = std::move(surface);
  return true;
}

/*
 * S17：隔离组的内部表面（子树合成到透明画布）。组自身的混合模式与不透明度
 * 不在这里作用——那一步归调用方（eval_node 或 GPU 后端）。
 * 组不可见时返回空缓冲（整棵子树不参与求值）。
 */
bool build_isolated_group_surface(const Document& doc, const FilterGraph& graph, int32_t index,
                                  double opacity_scale, ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    set_error(error, "group surface destination is null");
    return false;
  }
  if (index < 0 || index >= static_cast<int32_t>(graph.nodes.size())) {
    set_error(error, "graph node index out of range");
    return false;
  }
  const FilterNode& node = graph.nodes[index];
  if (node.kind != FilterNodeKind::kGroup || !node.isolated) {
    set_error(error, "graph node is not an isolated layer group");
    return false;
  }
  const Layer* layer = doc.find(node.layer);
  if (layer == nullptr) {
    set_error(error, "isolated group references a missing layer");
    return false;
  }
  out->resize(0, 0, COMP_PIXEL_RGBA8);
  if (!layer->visible) return true;

  const comp_pixel_format canvas_format = composite_surface_format(doc);
  ImageBuffer internal;
  internal.resize(doc.state().width, doc.state().height, canvas_format);
  internal.clear_transparent();
  for (int32_t input : node.inputs) {
    ImageBuffer next;
    if (!eval_node(doc, graph, input, internal, opacity_scale, &next, error)) return false;
    internal = std::move(next);
  }
  *out = std::move(internal);
  return true;
}

}  // namespace

bool bake_filter_unit_surface(const Document& doc, const FilterGraph& graph, int32_t node_index,
                              double opacity_scale, ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "bake destination is null";
    return false;
  }
  if (node_index < 0 || node_index >= static_cast<int32_t>(graph.nodes.size())) {
    if (error != nullptr) *error = "graph node index out of range";
    return false;
  }
  out->resize(0, 0, COMP_PIXEL_RGBA8);
  const FilterNode& node = graph.nodes[static_cast<size_t>(node_index)];
  if (node.kind == FilterNodeKind::kClippingStack) {
    return build_clipping_stack_surface(doc, graph, node_index, opacity_scale, out, error);
  }
  if (node.kind == FilterNodeKind::kGroup && node.isolated) {
    return build_isolated_group_surface(doc, graph, node_index, opacity_scale, out, error);
  }
  if (error != nullptr) {
    *error =
        "node is not an independent compositing unit (only isolated groups and clipping stacks "
        "can be baked as a whole)";
  }
  return false;
}

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
  backdrop.resize(state.width, state.height, composite_surface_format(doc));
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
