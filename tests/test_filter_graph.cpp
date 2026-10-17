// AI生成
/*
 * test_filter_graph.cpp — FilterGraph 有向图与图层组语义。
 *
 * 关注三件事：
 *   1) 建图正确性：节点数、inputs 边、backdrop 链、悬空父级必须被拒绝；
 *   2) 穿透组：组内子层的混合模式穿透组边界、与组下方内容交互（混合穿透），
 *      且组的不透明度乘进子层（对齐上游 LayerOpacity）；
 *   3) 隔离组：子树作为整体参与混合，组的混合模式作用在组结果上。
 */
#include <string>
#include <vector>

#include "compositing/compositor.h"
#include "compositing/filter_graph.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "test_framework.h"

using namespace compositor;

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-filtergraph-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

comp_layer_id add_layer(Document& doc, const char* name, comp_blend_mode blend, float opacity,
                        comp_layer_id parent, bool group) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = blend;
  desc.opacity = opacity;
  desc.is_group = group ? 1 : 0;
  desc.parent = parent;
  desc.transform = comp_rect{0, 0, 4, 4};
  comp_layer_id id = 0;
  Layer* layer = doc.create_layer(desc, &id);
  COMP_CHECK(layer != nullptr);
  return id;
}

void set_solid(Document& doc, comp_layer_id id, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  Layer* layer = doc.find(id);
  COMP_CHECK(layer != nullptr);
  layer->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, r, g, b, a);
}

const uint8_t* pixel(const ImageBuffer& buffer, int32_t x, int32_t y) {
  return buffer.row(y) + static_cast<size_t>(x) * 4;
}

int abs_diff(int a, int b) { return a > b ? a - b : b - a; }

bool pixel_matches(const ImageBuffer& buffer, int32_t x, int32_t y, int r, int g, int b, int a,
                   int tolerance) {
  /* 合成失败时 out 可能仍为空缓冲；此处直接判失败，避免在空缓冲上取像素造成访问违例。 */
  if (buffer.empty() || x < 0 || y < 0 || x >= buffer.width || y >= buffer.height) return false;
  const uint8_t* p = pixel(buffer, x, y);
  return abs_diff(p[0], r) <= tolerance && abs_diff(p[1], g) <= tolerance &&
         abs_diff(p[2], b) <= tolerance && abs_diff(p[3], a) <= tolerance;
}

size_t top_level_count(const Document& doc) {
  size_t count = 0;
  for (const Layer& layer : doc.state().layers) {
    if (layer.parent == 0) ++count;
  }
  return count;
}

}  // namespace

COMP_TEST(filter_graph_structure_and_backdrop_chain) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id a = add_layer(document, "A", COMP_BLEND_NORMAL, 1.0f, 0, false);
  const comp_layer_id b = add_layer(document, "B", COMP_BLEND_NORMAL, 1.0f, 0, false);
  const comp_layer_id c = add_layer(document, "C", COMP_BLEND_NORMAL, 1.0f, 0, false);

  FilterGraph graph;
  std::string error;
  COMP_CHECK(build_filter_graph(document, &graph, &error));

  COMP_CHECK(graph.output == 0);
  COMP_CHECK(graph.nodes.size() == top_level_count(document) + 1);
  COMP_CHECK(graph.nodes[0].kind == FilterNodeKind::kGroup);
  COMP_CHECK(graph.nodes[0].parent == -1);
  COMP_CHECK(graph.nodes[0].inputs.size() == top_level_count(document));

  /* 根的最后一个子节点应为 C，且三个顶层节点的 layer id 与创建序一致。 */
  const std::vector<int32_t>& root_inputs = graph.nodes[0].inputs;
  COMP_CHECK(graph.nodes[root_inputs[root_inputs.size() - 1]].layer == c);

  /* backdrop 链：首个顶层节点落在透明画布，其后各自指向前一节点。 */
  const int32_t first = root_inputs[0];
  COMP_CHECK(graph.nodes[first].backdrop == -1);
  for (size_t i = 1; i < root_inputs.size(); ++i) {
    COMP_CHECK(graph.nodes[root_inputs[i]].backdrop == root_inputs[i - 1]);
  }

  /* 节点 id 不含 0（0 为虚拟根），且覆盖全部图层。 */
  size_t source_nodes = 0;
  bool saw_a = false, saw_b = false, saw_c = false;
  for (const FilterNode& node : graph.nodes) {
    COMP_CHECK(node.layer != 0 || node.parent == -1);
    if (node.layer == a) saw_a = true;
    if (node.layer == b) saw_b = true;
    if (node.layer == c) saw_c = true;
    if (node.kind == FilterNodeKind::kSource) ++source_nodes;
  }
  COMP_CHECK(saw_a && saw_b && saw_c);
  COMP_CHECK(source_nodes == 3);
}

COMP_TEST(filter_graph_pass_through_group_blends_with_backdrop) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "Base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255); /* 红 */

  const comp_layer_id group = add_layer(document, "Group", COMP_BLEND_NORMAL, 1.0f, 0, true);
  const comp_layer_id child = add_layer(document, "Child", COMP_BLEND_MULTIPLY, 1.0f, group, false);
  set_solid(document, child, 0, 255, 0, 255); /* 绿，Multiply */

  /* 默认穿透：绿 Multiply 与组下方的红交互 -> 黑。若组被隔离则结果为绿。 */
  ImageBuffer out;
  std::string error;
  COMP_CHECK(composite_document(document, &out, &error));
  COMP_CHECK_MSG(pixel_matches(out, 1, 1, 0, 0, 0, 255, 0), "pass-through multiply hits backdrop");

  COMP_CHECK(document.find(group)->isolated == false);
}

COMP_TEST(filter_graph_pass_through_group_opacity_multiplies_into_children) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "Base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255); /* 红 */

  /* 组 50% x 子层 50% = 有效 25%，对齐上游 LayerOpacity。 */
  const comp_layer_id group = add_layer(document, "Group", COMP_BLEND_NORMAL, 0.5f, 0, true);
  const comp_layer_id child = add_layer(document, "Child", COMP_BLEND_NORMAL, 0.5f, group, false);
  set_solid(document, child, 0, 255, 0, 255); /* 绿 */

  ImageBuffer out;
  std::string error;
  COMP_CHECK(composite_document(document, &out, &error));
  /* 25% 绿压在红上：R=0.75*255=191，G=0.25*255=64，B=0。 */
  COMP_CHECK_MSG(pixel_matches(out, 2, 2, 191, 64, 0, 255, 2), "group opacity multiplies into child");
}

COMP_TEST(filter_graph_isolated_group_composites_as_unit) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "Base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255); /* 红 */

  const comp_layer_id group = add_layer(document, "Group", COMP_BLEND_NORMAL, 1.0f, 0, true);
  const comp_layer_id child = add_layer(document, "Child", COMP_BLEND_MULTIPLY, 1.0f, group, false);
  set_solid(document, child, 0, 255, 0, 255); /* 绿，Multiply */

  document.find(group)->isolated = true;

  ImageBuffer out;
  std::string error;
  /* 隔离后子树在透明底上自洽：Multiply 对透明 => 绿原样；再以 Normal 叠红 => 绿。 */
  COMP_CHECK(composite_document(document, &out, &error));
  COMP_CHECK_MSG(pixel_matches(out, 1, 1, 0, 255, 0, 255, 0), "isolated group is Normal over red");

  /* 换成 Multiply：组的混合模式此时作用在「组结果」这一个单元上 => 黑。 */
  document.find(group)->blend = COMP_BLEND_MULTIPLY;
  COMP_CHECK(composite_document(document, &out, &error));
  COMP_CHECK_MSG(pixel_matches(out, 1, 1, 0, 0, 0, 255, 0), "isolated group blend applies to unit");
}

COMP_TEST(filter_graph_nested_pass_through_groups) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "Base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255); /* 红 */

  const comp_layer_id outer = add_layer(document, "Outer", COMP_BLEND_NORMAL, 1.0f, 0, true);
  const comp_layer_id inner = add_layer(document, "Inner", COMP_BLEND_NORMAL, 1.0f, outer, true);
  const comp_layer_id leaf = add_layer(document, "Leaf", COMP_BLEND_MULTIPLY, 1.0f, inner, false);
  set_solid(document, leaf, 0, 255, 0, 255);

  FilterGraph graph;
  std::string error;
  COMP_CHECK(build_filter_graph(document, &graph, &error));

  /* 内层组的下层边应穿透到外层组的下层边（即 Base）。 */
  int32_t inner_index = -1;
  int32_t leaf_index = -1;
  for (int32_t i = 0; i < static_cast<int32_t>(graph.nodes.size()); ++i) {
    if (graph.nodes[i].layer == inner) inner_index = i;
    if (graph.nodes[i].layer == leaf) leaf_index = i;
  }
  COMP_CHECK(inner_index >= 0 && leaf_index >= 0);
  COMP_CHECK(graph.nodes[leaf_index].backdrop == graph.nodes[inner_index].backdrop);

  ImageBuffer out;
  /* 两级穿透：叶子的 Multiply 一路穿透到 Base -> 黑。 */
  COMP_CHECK(composite_document(document, &out, &error));
  COMP_CHECK_MSG(pixel_matches(out, 3, 3, 0, 0, 0, 255, 0), "leaf multiplies through two groups");
}

COMP_TEST(filter_graph_hidden_group_skips_subtree) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "Base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255); /* 红 */

  const comp_layer_id group = add_layer(document, "Group", COMP_BLEND_NORMAL, 1.0f, 0, true);
  const comp_layer_id child = add_layer(document, "Child", COMP_BLEND_NORMAL, 1.0f, group, false);
  set_solid(document, child, 0, 255, 0, 255);
  document.find(group)->visible = false;

  ImageBuffer out;
  std::string error;
  COMP_CHECK(composite_document(document, &out, &error));
  COMP_CHECK_MSG(pixel_matches(out, 1, 1, 255, 0, 0, 255, 0), "hidden group hides its children");
}

COMP_TEST(filter_graph_rejects_orphan_layer) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "Base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255);
  const comp_layer_id orphan = add_layer(document, "Orphan", COMP_BLEND_NORMAL, 1.0f, 0, false);

  /* 人为制造悬空父级：父 id 不存在。 */
  document.find(orphan)->parent = 9999;

  FilterGraph graph;
  std::string error;
  COMP_CHECK(!build_filter_graph(document, &graph, &error));
  COMP_CHECK_MSG(!error.empty(), "orphan hierarchy reports an error");

  ImageBuffer out;
  COMP_CHECK(!composite_document(document, &out, &error));
}

/*
 * 剪贴栈的 alpha 语义（对齐上游 LiveMaskRenderer.drawComposite:110-150）。
 *
 * 本轮实测到的缺陷：软 alpha 基底（a=128）+ MULTIPLY 子层，旧的「按基底 alpha
 * 逐像素缩放」实现得到 a=0——两次半透明 source-over 把 alpha 叠没了，而且
 * 非 Normal 的混合语义完全不对。上游口径（extract → 不透明基底上混合 → restore）
 * 应得 a=128，RGB 为 multiply(红, 绿) = 黑。
 *
 * 这条断言盯的是**语义**不是数值巧合：alpha 必须原样取回，而不是被叠乘。
 */
COMP_TEST(clipping_stack_keeps_base_alpha_and_blends_in_normal_mode) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc base_desc{};
  base_desc.name = "base";
  base_desc.transform = comp_rect{0, 0, 4, 4};
  base_desc.opacity = 1.0f;
  comp_layer_id base = 0;
  COMP_CHECK(document.create_layer(base_desc, &base));
  /* 预乘红，alpha=128（straight 255 预乘后为 128）。 */
  document.find(base)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 128, 0, 0, 128);

  comp_layer_desc child_desc{};
  child_desc.name = "multiply";
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.opacity = 1.0f;
  child_desc.blend = COMP_BLEND_MULTIPLY;
  comp_layer_id child = 0;
  COMP_CHECK(document.create_layer(child_desc, &child));
  document.find(child)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 255, 0, 255);
  document.find(child)->clipping_source = base;
  COMP_CHECK(document.find(child)->clipping_source == base);

  ImageBuffer out;
  std::string error;
  COMP_CHECK_MSG(composite_document(document, &out, &error), "composite: " + error);
  const uint8_t* px = out.pixel_data().data();
  COMP_CHECK_MSG(px[3] == 128, "the stack restores the base alpha verbatim, not zero");
  COMP_CHECK_MSG(px[0] == 0 && px[1] == 0 && px[2] == 0,
                 "multiply happens inside the base, so red x green is black");
}

/*
 * Normal 模式下两种口径应当一致（不透明基底上 source-over 再乘回 alpha ≈ 缩放），
 * 但**走的是同一条代码路径**：这里钉住「隐藏一个剪贴成员不改变基底 alpha」，
 * 因为那正是成员被错误并入/排除出栈时最先暴露的破绽。
 */
COMP_TEST(clipping_stack_hidden_member_leaves_base_untouched) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  comp_layer_desc base_desc{};
  base_desc.name = "base";
  base_desc.transform = comp_rect{0, 0, 4, 4};
  base_desc.opacity = 1.0f;
  comp_layer_id base = 0;
  COMP_CHECK(document.create_layer(base_desc, &base));
  document.find(base)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 128, 0, 0, 128);

  comp_layer_desc child_desc{};
  child_desc.name = "normal";
  child_desc.transform = comp_rect{0, 0, 4, 4};
  child_desc.opacity = 1.0f;
  comp_layer_id child = 0;
  COMP_CHECK(document.create_layer(child_desc, &child));
  document.find(child)->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 255, 255);
  document.find(child)->clipping_source = base;

  ImageBuffer with_child;
  std::string error;
  COMP_CHECK(composite_document(document, &with_child, &error));

  document.find(child)->visible = false;
  ImageBuffer hidden_child;
  COMP_CHECK_MSG(composite_document(document, &hidden_child, &error), "composite: " + error);

  const uint8_t* shown = with_child.pixel_data().data();
  const uint8_t* hidden = hidden_child.pixel_data().data();
  COMP_CHECK_MSG(hidden[3] == 128,
                 "hiding the clipped member leaves the base alpha alone");
  COMP_CHECK_MSG(shown[3] == hidden[3], "a hidden member does not change the stack coverage");
  /* 蓝成员在可见时会把基底染蓝，隐藏后应回到纯底色。 */
  COMP_CHECK_MSG(shown[2] != hidden[2], "the visible member really contributes colour");
}

/*
 * 半透明实色必须存成**预乘**像素：直通色直接写进预乘缓冲会得到
 * rgb=(255,0,0) alpha=128 这种「比 alpha 还亮」的值，一过剪贴栈的
 * extract/restore 就会整体偏亮。fill/solid 因此要预乘。
 */
COMP_TEST(solid_fill_premultiplies_straight_colour) {
  ImageBuffer buffer = ImageBuffer::solid(2, 2, COMP_PIXEL_RGBA8, 255, 0, 0, 255);
  buffer.fill(255, 0, 0, 128);
  const uint8_t* px = buffer.pixel_data().data();
  COMP_CHECK_MSG(px[0] == 128 && px[3] == 128,
                 "a half-transparent red must be stored premultiplied, not straight");
  COMP_CHECK_MSG(px[1] == 0 && px[2] == 0, "green and blue stay zero");
}
