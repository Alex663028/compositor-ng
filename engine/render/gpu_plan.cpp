// AI生成
#include "render/gpu_plan.h"

#include <algorithm>
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

/* ------------------------------------------------------------------
 * 烘焙缓存键（S20 / v2）
 *
 * FNV-1a 64：只要求「输入不同 → 键几乎必然不同」，不是安全边界；
 * 64 位空间里同一进程内的烘焙单元数最多几百，碰撞概率可忽略。
 * 键里含文档实例号，所以同一个后端先后服务两个文档时不会串用。
 * ------------------------------------------------------------------ */

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
/* 键的用途标签：源图层烘焙与子树烘焙的输入形态不同，先分流避免「同前缀不同语义」。 */
constexpr uint64_t kSourceKeyTag = 1;
constexpr uint64_t kSubtreeKeyTag = 2;

struct Fnv1a {
  uint64_t value = kFnvOffsetBasis;

  void bytes(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      value ^= p[i];
      value *= kFnvPrime;
    }
  }

  template <typename T>
  void add(const T& v) {
    bytes(&v, sizeof(v));
  }
};

/* 0 在 GpuUnit 里表示「无缓存键」，所以实际键永远不为 0。 */
uint64_t finish_key(const Fnv1a& h) { return h.value == 0 ? 1 : h.value; }

void hash_canvas(Fnv1a* h, const Document& doc) {
  h->add(doc.instance_serial());
  const DocumentState& state = doc.state();
  h->add(state.width);
  h->add(state.height);
}

/*
 * 单个像素源图层烘焙键：bake_layer_contribution 只吃该层内容 + 累积不透明度。
 * 蒙版/效果/几何/混合参数/像素的改动都由 content_revision 覆盖；分组结构不在
 * 这里（穿透组只贡献 opacity_scale，已hash）。
 */
uint64_t source_bake_key(const Document& doc, const Layer& layer, double opacity_scale) {
  Fnv1a h;
  hash_canvas(&h, doc);
  h.add(kSourceKeyTag);
  h.add(opacity_scale);
  h.add(layer.id);
  h.add(layer.content_revision);
  return finish_key(h);
}

/*
 * 子树结构签名：DFS 先序，逐个节点混入
 *   (深度, 累积不透明度, 节点类型, 隔离标志, 图层 id, 图层 content_revision, 子节点数)
 * 然后按 inputs 顺序递归。深度 + 有序 DFS 足以区分结构：reorder、换父级、
 * 改剪贴基底都会改变成员/顺序/深度，因此即使这些入口不发修订号，键也会变。
 *
 * 累积不透明度与 Planner::append_units 的算法逐字一致（穿透组乘组不透明度），
 * 因为 bake_filter_unit_surface 会把它折进每个子层的贡献。
 */
void hash_subtree(Fnv1a* h, const Document& doc, const FilterGraph& graph, int32_t index,
                  int32_t depth, double opacity_scale) {
  h->add(depth);
  h->add(opacity_scale);
  if (index < 0 || index >= static_cast<int32_t>(graph.nodes.size())) {
    h->add(static_cast<int32_t>(-1));
    h->add(index);
    return;
  }
  const FilterNode& node = graph.nodes[static_cast<size_t>(index)];
  const Layer* layer = doc.find(node.layer);
  h->add(static_cast<int32_t>(node.kind));
  h->add(static_cast<uint8_t>(node.isolated ? 1 : 0));
  h->add(node.layer);
  h->add(layer != nullptr ? layer->content_revision : 0ull);
  h->add(static_cast<int32_t>(node.inputs.size()));

  double child_scale = opacity_scale;
  if (node.kind == FilterNodeKind::kGroup && !node.isolated && layer != nullptr) {
    child_scale = opacity_scale * static_cast<double>(layer->opacity);
  }
  for (int32_t input : node.inputs) {
    hash_subtree(h, doc, graph, input, depth + 1, child_scale);
  }
}

uint64_t subtree_bake_key(const Document& doc, const FilterGraph& graph, int32_t index,
                          double opacity_scale) {
  Fnv1a h;
  hash_canvas(&h, doc);
  h.add(kSubtreeKeyTag);
  hash_subtree(&h, doc, graph, index, 1, opacity_scale);
  return finish_key(h);
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
  GpuBakeCache* cache;
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
      unit.cache_key = source_bake_key(doc, layer, opacity_scale);
      const bool hit = cache != nullptr && cache->lookup(unit.cache_key, &unit.baked);
      if (!hit) {
        if (!bake_layer_contribution(doc, layer, opacity_scale, &unit.baked, error)) {
          ok = false;
          return;
        }
        if (cache != nullptr && !unit.baked.empty()) cache->store(unit.cache_key, unit.baked);
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
    unit.cache_key = subtree_bake_key(doc, graph, index, opacity_scale);
    const bool hit = cache != nullptr && cache->lookup(unit.cache_key, &unit.baked);
    if (!hit) {
      if (!bake_filter_unit_surface(doc, graph, index, opacity_scale, &unit.baked, error)) {
        ok = false;
        return;
      }
      if (cache != nullptr && !unit.baked.empty()) cache->store(unit.cache_key, unit.baked);
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
    unit.cache_key = subtree_bake_key(doc, graph, index, opacity_scale);
    const bool hit = cache != nullptr && cache->lookup(unit.cache_key, &unit.baked);
    if (!hit) {
      if (!bake_filter_unit_surface(doc, graph, index, opacity_scale, &unit.baked, error)) {
        ok = false;
        return;
      }
      if (cache != nullptr && !unit.baked.empty()) cache->store(unit.cache_key, unit.baked);
    }
    if (unit.baked.empty()) return; /* 隐藏组 */
    units->push_back(std::move(unit));
  }
};

}  // namespace

GpuBakeCache::GpuBakeCache(size_t budget_bytes) : budget_(budget_bytes) {}

void GpuBakeCache::begin_frame() { ++frame_; }

bool GpuBakeCache::lookup(uint64_t key, ImageBuffer* surface) {
  if (key == 0 || surface == nullptr) {
    ++misses_;
    return false;
  }
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    ++misses_;
    return false;
  }
  it->second.last_used_frame = frame_;
  /* 浅拷贝：shared_ptr 复制，不触发像素深拷贝（缓存只读，绝不 writable_pixels）。 */
  *surface = it->second.surface;
  ++hits_;
  return true;
}

void GpuBakeCache::store(uint64_t key, const ImageBuffer& surface) {
  if (key == 0 || surface.empty()) return;
  const size_t incoming = surface.byte_size();
  if (incoming > budget_) return; /* 单个表面都放不下：永不入缓存 */

  auto it = entries_.find(key);
  if (it != entries_.end()) {
    bytes_ -= it->second.surface.byte_size();
    it->second.surface = surface; /* 浅拷贝 */
    it->second.last_used_frame = frame_;
    bytes_ += incoming;
    trim(0);
    return;
  }

  trim(incoming);
  Entry entry;
  entry.surface = surface;
  entry.last_used_frame = frame_;
  bytes_ += incoming;
  entries_.emplace(key, std::move(entry));
}

void GpuBakeCache::clear() {
  entries_.clear();
  bytes_ = 0;
}

void GpuBakeCache::set_budget_bytes(size_t bytes) {
  budget_ = bytes;
  trim(0);
}

/*
 * LRU 淘汰：只淘汰**本帧未用过**的条目（否则同一帧内反复重建），
 * 且只影响下一次是否重烘焙，不影响可观察结果。
 */
void GpuBakeCache::trim(size_t incoming_bytes) {
  while (bytes_ + incoming_bytes > budget_ && !entries_.empty()) {
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second.last_used_frame == frame_) continue;
      if (victim == entries_.end() || it->second.last_used_frame < victim->second.last_used_frame) {
        victim = it;
      }
    }
    if (victim == entries_.end()) break; /* 全是本帧条目：不再淘汰 */
    bytes_ -= victim->second.surface.byte_size();
    entries_.erase(victim);
    ++evictions_;
  }
}

bool build_gpu_plan(const Document& doc, std::vector<GpuUnit>* units, GpuPlanStats* stats,
                    std::string* error, GpuBakeCache* cache) {
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

  if (cache != nullptr) cache->begin_frame();
  Planner planner{doc, graph, units, GpuPlanStats{}, error, cache, true};
  const FilterNode& root = graph.nodes[static_cast<size_t>(graph.output)];
  for (int32_t input : root.inputs) {
    planner.append_units(input, 1.0, 1);
    if (!planner.ok) return false;
  }
  if (stats != nullptr) *stats = planner.stats;
  return true;
}

}  // namespace compositor
