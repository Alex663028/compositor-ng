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
#include <cmath>
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

/*
 * half 编解码只服务本文件的 HDR 用例：取值限于 [0,1] 的精确二进制小数
 * （0.5、1.0），因此不需要处理次正规与舍入之外的边角。
 */
uint16_t half_bits(float value) {
  if (!(value > 0.0f)) return 0;
  int exponent = 0;
  const float mantissa = std::frexp(value, &exponent); /* value = mantissa * 2^exponent */
  if (exponent > 16) return 0x7C00u;
  if (exponent < -14) return 0;
  const uint32_t head = static_cast<uint32_t>(exponent + 14) << 10;
  const uint32_t tail = static_cast<uint32_t>((mantissa * 2.0f - 1.0f) * 1024.0f + 0.5f);
  return static_cast<uint16_t>(head + tail);
}

float half_value(uint16_t bits) {
  const int exponent = (bits >> 10) & 0x1F;
  const float fraction = static_cast<float>(bits & 0x3FFu) / 1024.0f;
  if (exponent == 0) return std::ldexp(fraction, -14);
  return std::ldexp(1.0f + fraction, exponent - 15);
}

/* 把一层填成 RGBA16F 的预乘常量色（half 编码见上）。 */
bool set_half_solid(Layer* layer, int32_t w, int32_t h, float r, float g, float b, float a) {
  if (layer == nullptr) return false;
  layer->pixels.resize(w, h, COMP_PIXEL_RGBA16F);
  for (int32_t y = 0; y < h; ++y) {
    uint16_t* row = reinterpret_cast<uint16_t*>(layer->pixels.writable_row(y));
    for (int32_t x = 0; x < w; ++x) {
      row[x * 4 + 0] = half_bits(r);
      row[x * 4 + 1] = half_bits(g);
      row[x * 4 + 2] = half_bits(b);
      row[x * 4 + 3] = half_bits(a);
    }
  }
  return true;
}

/* half 缓冲里某像素的预乘分量。 */
void half_pixel(const ImageBuffer& buffer, int32_t x, int32_t y, float out[4]) {
  const uint16_t* p =
      reinterpret_cast<const uint16_t*>(buffer.row(y)) + static_cast<size_t>(x) * 4;
  for (int i = 0; i < 4; ++i) out[i] = half_value(p[i]);
}

int round_u8(float value) {
  const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
  return static_cast<int>(clamped * 255.0f + 0.5f);
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
 * 基底在成员覆盖范围之外必须原样可见。
 *
 * 上一轮修掉的缺陷：栈内成员混合时把「额外不透明度」写成常量 1.0，于是成员缓冲
 * 里的**透明**像素被当成不透明黑混进栈表面，基底 alpha 还在、RGB 却被涂黑。
 * 全画布基底 + 全画布成员看不出这个问题（没有「范围之外」），所以必须用小成员。
 */
COMP_TEST(clipping_stack_keeps_base_visible_outside_partial_member) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255); /* 不透明红 */

  comp_layer_desc child_desc{};
  child_desc.name = "member";
  child_desc.transform = comp_rect{1, 1, 2, 2}; /* 只盖住中间 2x2 */
  child_desc.opacity = 1.0f;
  comp_layer_id member = 0;
  COMP_CHECK(document.create_layer(child_desc, &member));
  document.find(member)->pixels = ImageBuffer::solid(2, 2, COMP_PIXEL_RGBA8, 0, 255, 0, 255);
  document.find(member)->clipping_source = base;

  ImageBuffer out;
  std::string error;
  COMP_CHECK_MSG(composite_document(document, &out, &error), "composite: " + error);
  COMP_CHECK_MSG(pixel_matches(out, 0, 0, 255, 0, 0, 255, 0),
                 "the base keeps its colour outside the member");
  COMP_CHECK_MSG(pixel_matches(out, 3, 3, 255, 0, 0, 255, 0),
                 "the bottom-right corner is still the base");
  COMP_CHECK_MSG(pixel_matches(out, 1, 1, 0, 255, 0, 255, 0),
                 "the member colours the base inside its own rect");
}

/*
 * 成员的有效 alpha 必须取自像素本身，而不是常量。
 *
 * 同一处缺陷的另一半：半透明成员曾被当成完全不透明。这里用「不透明红基底 +
 * 50% 绿成员」钉住数值——source-over 后预乘应为 (127,128,0,255)，而不是
 * (0,255,0,255)（当成不透明）或 (0,128,0,128)（把栈覆盖度乘两遍）。
 */
COMP_TEST(clipping_stack_member_alpha_comes_from_pixel) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  const comp_layer_id base = add_layer(document, "base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  set_solid(document, base, 255, 0, 0, 255);

  const comp_layer_id member = add_layer(document, "member", COMP_BLEND_NORMAL, 1.0f, 0, false);
  /* solid() 的入参是直通色：straight (0,255,0) a=128 -> 预乘 (0,128,0,128)。 */
  set_solid(document, member, 0, 255, 0, 128);
  document.find(member)->clipping_source = base;

  ImageBuffer out;
  std::string error;
  COMP_CHECK_MSG(composite_document(document, &out, &error), "composite: " + error);
  COMP_CHECK_MSG(pixel_matches(out, 2, 2, 127, 128, 0, 255, 2),
                 "a half-transparent member blends at its own alpha");
}

/*
 * half 表面上的同两条语义（栈的混合循环有一份独立的 half 分支，
 * 8 位用例覆盖不到）。基底半透明红、成员只盖住左上角：
 *   - 成员内：绿以成员 alpha=1 覆盖不透明基底，restore 后仍是 a=0.5；
 *   - 成员外：基底原样，不得被涂黑。
 */
COMP_TEST(clipping_stack_keeps_base_visible_in_half_surface) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");

  /* 预乘 50% 红。任何 RGBA16F 图层都会把合成表面切到 half。 */
  const comp_layer_id base = add_layer(document, "base", COMP_BLEND_NORMAL, 1.0f, 0, false);
  COMP_CHECK(set_half_solid(document.find(base), 4, 4, 0.5f, 0.0f, 0.0f, 0.5f));

  comp_layer_desc child_desc{};
  child_desc.name = "member";
  child_desc.transform = comp_rect{0, 0, 2, 2};
  child_desc.opacity = 1.0f;
  comp_layer_id member = 0;
  COMP_CHECK(document.create_layer(child_desc, &member));
  document.find(member)->pixels = ImageBuffer::solid(2, 2, COMP_PIXEL_RGBA8, 0, 255, 0, 255);
  document.find(member)->clipping_source = base;

  ImageBuffer out;
  std::string error;
  COMP_CHECK_MSG(composite_document(document, &out, &error), "composite: " + error);
  COMP_CHECK_MSG(out.format == COMP_PIXEL_RGBA16F, "a half layer keeps the surface in half");

  float inside[4];
  float outside[4];
  half_pixel(out, 0, 0, inside);
  half_pixel(out, 3, 3, outside);
  COMP_CHECK_MSG(round_u8(inside[1]) == 128 && round_u8(inside[3]) == 128,
                 "the half member blends onto the opaque base and keeps a=0.5");
  COMP_CHECK_MSG(round_u8(inside[0]) == 0, "no red leaks through the green member");
  COMP_CHECK_MSG(round_u8(outside[0]) == 128 && round_u8(outside[3]) == 128,
                 "the base outside the member is untouched half red");
  COMP_CHECK_MSG(round_u8(outside[1]) == 0, "the member does not tint pixels it does not cover");
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
