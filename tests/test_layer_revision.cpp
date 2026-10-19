// AI生成
/*
 * test_layer_revision.cpp — 逐层内容修订号 `Layer::content_revision`（S18 / R1）。
 *
 * 为什么需要它：GPU 的逐层纹理缓存（R2）不能拿指针 / 字节哈希 / 整篇 content_serial
 * 当键——指针会因 COW 复用、哈希要整幅扫描、整篇序号太粗（改一层等于全丢）。所以
 * 引擎给每个图层发一个文档内唯一、单调递增的修订号：
 *
 *   同号 ==> 同内容（缓存的唯一安全前提）；
 *   内容变了 ==> 换号（宁可多失效，不可少失效）。
 *
 * 反向不成立：换号不保证内容一定不同（同值重写也换号），这是刻意允许的过失效。
 *
 * 本文件钉住的不变量：
 *   1. 建层即发号，文档内唯一且单调；**结构变更**（增删 / 排序 / 选中 / 剪贴源）
 *      不动既有图层的号；
 *   2. 每个 ABI 写入入口只换目标层的号，读到 / 查询 / 失败的写入一律不发号；
 *   3. 撤销把「内容 + 修订号」一起回退，重做一起前进——这条**直接**检验
 *      「换号必须发生在事务提交之前」：若换号在 commit 之后，重做会带着
 *      编辑后的内容却拿到编辑前的号，缓存会静默供应旧纹理；
 *   4. 修订号是**逐层**属性：两个图层共享同一块像素缓冲时，改其中一层只换那层的号；
 *   5. 画布重采样 / 工具落笔 / 滤镜 / 选区搬移像素都算内容变更；
 *   6. 修订号变化与 GPU 合成计划（gpu_plan）的重建对得上：变换、隔离组、
 *      可见性一改，单元序列就得重算。
 *
 * 这些断言全部只依赖内部头（tests 链接领域层），不需要 GPU；R2 的纹理缓存测试
 * 会在此基础上加「上传次数」计数。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "compositor/comp_filter.h"
#include "compositor/comp_layer.h"
#include "compositor/comp_render.h"
#include "compositor/comp_selection.h"
#include "compositor/comp_undo.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "render/gpu_plan.h"
#include "test_framework.h"

using namespace compositor;

namespace {

constexpr int32_t kW = 8;
constexpr int32_t kH = 6;

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-layer-revision-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 1;
  return desc;
}

comp_document as_document_handle(Document* document) {
  return reinterpret_cast<comp_document>(document);
}

std::vector<uint8_t> solid_pixels(int32_t width, int32_t height, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = 255;
  }
  return pixels;
}

comp_image image_view(int32_t width, int32_t height, std::vector<uint8_t>& pixels) {
  comp_image image{};
  image.width = width;
  image.height = height;
  image.format = COMP_PIXEL_RGBA8;
  image.stride = static_cast<size_t>(width) * 4u;
  image.pixels = pixels.data();
  return image;
}

/* 追加一个铺满画布的实心图层（经 ABI，顺带覆盖建层入口）。 */
comp_layer_id add_layer(comp_document doc, const char* name, int32_t width, int32_t height,
                        uint8_t r, uint8_t g, uint8_t b, comp_layer_id parent = 0) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  desc.parent = parent;
  desc.transform = comp_rect{0, 0, width, height};
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &id) == COMP_OK);
  std::vector<uint8_t> pixels = solid_pixels(width, height, r, g, b);
  comp_image image = image_view(width, height, pixels);
  COMP_CHECK(comp_layer_set_pixels(doc, id, &image) == COMP_OK);
  return id;
}

comp_layer_id add_group(comp_document doc, const char* name) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.is_group = 1;
  desc.blend = COMP_BLEND_NORMAL;
  desc.opacity = 1.0f;
  desc.sampling = COMP_SAMPLING_HIGH_QUALITY;
  desc.transform = comp_rect{0, 0, kW, kH};
  comp_layer_id id = 0;
  COMP_CHECK(comp_layer_add(doc, &desc, &id) == COMP_OK);
  return id;
}

uint64_t revision_of(const Document& document, comp_layer_id id) {
  const Layer* layer = document.find(id);
  COMP_CHECK_MSG(layer != nullptr, "the layer under test must exist");
  return layer != nullptr ? layer->content_revision : 0;
}

std::vector<uint8_t> pixels_of(const Document& document, comp_layer_id id) {
  const Layer* layer = document.find(id);
  if (layer == nullptr) return {};
  return layer->pixels.pixel_data();
}

/* 整篇渲染成 RGBA8，用于「撤销后画面逐字节回到从前」这类断言。 */
std::vector<uint8_t> flatten(const Document& document) {
  std::vector<uint8_t> out(static_cast<size_t>(document.state().width) *
                               static_cast<size_t>(document.state().height) * 4u,
                           0);
  COMP_CHECK(comp_render_flatten(as_document_handle(const_cast<Document*>(&document)), out.data(),
                                COMP_PIXEL_RGBA8,
                                static_cast<size_t>(document.state().width) * 4u) == COMP_OK);
  return out;
}

GpuPlanStats plan_stats(const Document& document) {
  GpuPlanStats stats{};
  std::string error;
  COMP_CHECK_MSG(build_gpu_plan(document, nullptr, &stats, &error), error.c_str());
  return stats;
}

}  // namespace

/* ============================================ 1. 发号：唯一、单调、结构变更不换号 */

COMP_TEST(layer_revision_is_unique_and_ignores_structure_edits) {
  Engine engine(test_engine_desc());
  Document document(&engine, kW, kH, 72.0f, "sRGB");
  const comp_document doc = as_document_handle(&document);

  const comp_layer_id back = add_layer(doc, "back", kW, kH, 10, 20, 30);
  const comp_layer_id front = add_layer(doc, "front", kW, kH, 200, 10, 10);
  const uint64_t back_revision = revision_of(document, back);
  const uint64_t front_revision = revision_of(document, front);

  COMP_CHECK_MSG(back_revision > 0, "a layer created through the ABI gets a revision right away");
  COMP_CHECK_MSG(front_revision > 0, "…and so does the next one");
  COMP_CHECK_MSG(back_revision != front_revision, "revisions are unique inside one document");
  COMP_CHECK_MSG(front_revision > back_revision, "creation order and revision order agree");

  /* 第三个图层只影响自己：已存在图层的号必须原地不动。 */
  const comp_layer_id top = add_layer(doc, "top", kW, kH, 0, 200, 0);
  COMP_CHECK_MSG(revision_of(document, top) > front_revision, "a newer layer has a newer revision");
  COMP_CHECK_MSG(revision_of(document, back) == back_revision,
                 "adding a layer must not renumber the existing ones");
  COMP_CHECK_MSG(revision_of(document, front) == front_revision,
                 "adding a layer must not renumber the existing ones");

  /*
   * 结构变更（顺序 / 选中 / 剪贴源 / 删除）不改内容，因此不发号：
   * 它们要么只动堆叠序，要么只动编辑器状态。GPU 单元序列会因顺序变化而重算，
   * 但每层自己的像素没变，纹理不用重传——这正是缓存要区分「结构」与「内容」。
   */
  COMP_CHECK(comp_layer_reorder(doc, top, back) == COMP_OK);
  COMP_CHECK(comp_layer_set_active(doc, front) == COMP_OK);
  COMP_CHECK(comp_layer_set_clipping_source(doc, front, back) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, back) == back_revision, "reorder/active/clipping keep the id");
  COMP_CHECK_MSG(revision_of(document, front) == front_revision,
                 "reorder/active/clipping keep the id");

  COMP_CHECK(comp_layer_remove(doc, top) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, back) == back_revision, "removing a sibling keeps the id");
  COMP_CHECK_MSG(revision_of(document, front) == front_revision,
                 "removing a sibling keeps the id");
}

/* ============================================ 2. 每个写入入口只换目标层的号 */

COMP_TEST(layer_revision_bumps_on_every_content_write_entry) {
  Engine engine(test_engine_desc());
  Document document(&engine, kW, kH, 72.0f, "sRGB");
  const comp_document doc = as_document_handle(&document);

  const comp_layer_id target = add_layer(doc, "target", kW, kH, 10, 20, 30);
  const comp_layer_id sibling = add_layer(doc, "sibling", kW, kH, 40, 50, 60);
  const uint64_t sibling_revision = revision_of(document, sibling);

  uint64_t previous = revision_of(document, target);
  /*
   * 同一个断言形状套在每个入口上：目标层换号、隔壁层不动。
   * 这里的「换号」不要求内容真的不同（同值重写也换），只要求「内容可能变了就换」。
   */
  const auto expect_bump = [&](const char* what) {
    const uint64_t now = revision_of(document, target);
    COMP_CHECK_MSG(now > previous, what);
    COMP_CHECK_MSG(revision_of(document, sibling) == sibling_revision,
                   "a write to one layer must never renumber another layer");
    previous = now;
  };

  std::vector<uint8_t> pixels = solid_pixels(kW, kH, 90, 90, 90);
  comp_image image = image_view(kW, kH, pixels);
  COMP_CHECK(comp_layer_set_pixels(doc, target, &image) == COMP_OK);
  expect_bump("set_pixels must produce a new revision");

  std::vector<uint8_t> mask_pixels(static_cast<size_t>(kW) * kH, 255);
  comp_mask mask{};
  mask.width = kW;
  mask.height = kH;
  mask.stride = static_cast<size_t>(kW);
  mask.pixels = mask_pixels.data();
  COMP_CHECK(comp_layer_set_mask(doc, target, &mask) == COMP_OK);
  expect_bump("set_mask must produce a new revision");

  COMP_CHECK(comp_layer_set_blend(doc, target, COMP_BLEND_MULTIPLY) == COMP_OK);
  expect_bump("set_blend must produce a new revision");

  COMP_CHECK(comp_layer_set_opacity(doc, target, 0.5f) == COMP_OK);
  expect_bump("set_opacity must produce a new revision");

  COMP_CHECK(comp_layer_set_transform(doc, target, comp_rect{0, 0, kW, kH}, 0.0f) == COMP_OK);
  expect_bump("set_transform must produce a new revision");

  COMP_CHECK(comp_layer_set_visible(doc, target, 1) == COMP_OK);
  expect_bump("set_visible must produce a new revision");

  COMP_CHECK(comp_layer_set_sampling(doc, target, COMP_SAMPLING_NEAREST) == COMP_OK);
  expect_bump("set_sampling must produce a new revision");

  COMP_CHECK(comp_layer_add_adjustment(doc, target, COMP_ADJUSTMENT_INVERT, "{}") == COMP_OK);
  expect_bump("add_adjustment must produce a new revision");

  COMP_CHECK(comp_layer_update_adjustment(doc, target, "{}") == COMP_OK);
  expect_bump("update_adjustment must produce a new revision");

  COMP_CHECK(comp_layer_update_adjustment_at(doc, target, 0, COMP_ADJUSTMENT_INVERT, "{}") ==
             COMP_OK);
  expect_bump("update_adjustment_at must produce a new revision");

  COMP_CHECK(comp_layer_remove_adjustment(doc, target, 0) == COMP_OK);
  expect_bump("remove_adjustment must produce a new revision");

  COMP_CHECK(comp_layer_set_effect(doc, target, 1u, "{}") == COMP_OK);
  expect_bump("set_effect must produce a new revision");

  COMP_CHECK(comp_layer_remove_effect(doc, target, 1u) == COMP_OK);
  expect_bump("remove_effect must produce a new revision");

  COMP_CHECK(comp_layer_set_isolated(doc, target, 1) == COMP_OK);
  expect_bump("set_isolated must produce a new revision");

  /* 只读查询不换号：查询本身不能把缓存打掉。 */
  int32_t visible = 0;
  float opacity = 0.0f;
  comp_blend_mode blend = COMP_BLEND_NORMAL;
  comp_sampling sampling = COMP_SAMPLING_HIGH_QUALITY;
  size_t count = 0;
  comp_size size{};
  COMP_CHECK(comp_layer_get_visible(doc, target, &visible) == COMP_OK);
  COMP_CHECK(comp_layer_get_opacity(doc, target, &opacity) == COMP_OK);
  COMP_CHECK(comp_layer_get_blend(doc, target, &blend) == COMP_OK);
  COMP_CHECK(comp_layer_get_sampling(doc, target, &sampling) == COMP_OK);
  COMP_CHECK(comp_layer_adjustment_count(doc, target, &count) == COMP_OK);
  COMP_CHECK(comp_document_size(doc, &size) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, target) == previous, "read-only queries must not bump");
  COMP_CHECK_MSG(revision_of(document, sibling) == sibling_revision, "reads bump nothing");

  /* 失败的写入不换号：参数闸 / 查找失败都发生在写之前。 */
  COMP_CHECK(comp_layer_set_blend(doc, target, static_cast<comp_blend_mode>(999)) ==
             COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_add_adjustment(doc, target, 999u, "{}") == COMP_ERR_INVALID_ARGUMENT);
  COMP_CHECK(comp_layer_set_pixels(doc, 99999, &image) == COMP_ERR_NOT_FOUND);
  COMP_CHECK_MSG(revision_of(document, target) == previous,
                 "a rejected write must not bump the revision");

  /* 反过来：改隔壁层只换隔壁层的号。 */
  COMP_CHECK(comp_layer_set_opacity(doc, sibling, 0.25f) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, sibling) > sibling_revision,
                 "a write to the sibling bumps the sibling");
  COMP_CHECK_MSG(revision_of(document, target) == previous,
                 "a write to the sibling must not renumber the target");
}

/* ============================================ 3. 撤销 / 重做与修订号同步回退前进 */

COMP_TEST(layer_revision_tracks_undo_and_redo) {
  Engine engine(test_engine_desc());
  Document document(&engine, kW, kH, 72.0f, "sRGB");
  const comp_document doc = as_document_handle(&document);

  const comp_layer_id layer = add_layer(doc, "paint", kW, kH, 200, 0, 0);
  const std::vector<uint8_t> red = pixels_of(document, layer);
  const std::vector<uint8_t> red_render = flatten(document);
  const uint64_t red_revision = revision_of(document, layer);
  COMP_CHECK(red_revision > 0);

  std::vector<uint8_t> green = solid_pixels(kW, kH, 0, 200, 0);
  comp_image green_image = image_view(kW, kH, green);
  COMP_CHECK(comp_layer_set_pixels(doc, layer, &green_image) == COMP_OK);
  const uint64_t green_revision = revision_of(document, layer);
  COMP_CHECK_MSG(green_revision != red_revision, "a pixel write must produce a new revision");
  COMP_CHECK_MSG(pixels_of(document, layer) == green, "the pixel write must land");
  const std::vector<uint8_t> green_render = flatten(document);
  COMP_CHECK_MSG(green_render != red_render, "the pixel write must change the rendered canvas");

  COMP_CHECK(comp_document_undo(doc) == COMP_OK);
  COMP_CHECK_MSG(pixels_of(document, layer) == red, "undo restores the exact bytes");
  COMP_CHECK_MSG(revision_of(document, layer) == red_revision,
                 "undo restores the pre-edit revision together with the content");
  COMP_CHECK_MSG(flatten(document) == red_render, "undo restores the rendered canvas");

  COMP_CHECK(comp_document_redo(doc) == COMP_OK);
  COMP_CHECK_MSG(pixels_of(document, layer) == green, "redo restores the edited bytes");
  COMP_CHECK_MSG(revision_of(document, layer) == green_revision,
                 "redo restores the post-edit revision together with the content");
  COMP_CHECK_MSG(flatten(document) == green_render, "redo restores the rendered canvas");

  /*
   * 领域层自管事务的入口（笔刷）最关键：换号必须发生在 txn_commit() 之前。
   * 若在提交之后换号，撤销/重做会把「编辑后的内容」和「编辑前的号」凑在一起——
   * 这里通过重做后必须拿到 stroke_revision 来钉死这一点。
   */
  comp_tool_op_desc stroke{};
  stroke.kind = COMP_TOOL_STROKE;
  float points[6] = {4.0f, 3.0f, 4.5f, 2.5f, 5.0f, 3.5f};
  stroke.stroke_points = points;
  stroke.stroke_point_count = 3;
  stroke.brush.color[0] = 255;
  stroke.brush.color[1] = 255;
  stroke.brush.color[2] = 255;
  stroke.brush.diameter = 6.0f;
  stroke.brush.pressure = 1.0f;
  stroke.brush.hardness = 1.0f;
  stroke.brush.flow = 1.0f;
  COMP_CHECK(comp_tool_apply(doc, layer, &stroke, nullptr, 0, nullptr) == COMP_OK);
  const uint64_t stroke_revision = revision_of(document, layer);
  COMP_CHECK_MSG(stroke_revision != green_revision,
                 "a stroke through the domain transaction must produce a new revision");
  const std::vector<uint8_t> stroked = pixels_of(document, layer);
  COMP_CHECK_MSG(stroked != green, "the stroke must actually change pixels");

  COMP_CHECK(comp_document_undo(doc) == COMP_OK);
  COMP_CHECK_MSG(pixels_of(document, layer) == green, "undoing a stroke restores the bytes");
  COMP_CHECK_MSG(revision_of(document, layer) == green_revision,
                 "undoing a stroke restores the revision (bump happened before commit)");
  COMP_CHECK_MSG(flatten(document) == green_render,
                 "undoing a stroke restores the rendered canvas byte for byte");

  COMP_CHECK(comp_document_redo(doc) == COMP_OK);
  COMP_CHECK_MSG(pixels_of(document, layer) == stroked, "redoing a stroke restores the bytes");
  COMP_CHECK_MSG(revision_of(document, layer) == stroke_revision,
                 "redoing a stroke restores the post-stroke revision");
  COMP_CHECK_MSG(flatten(document) != green_render,
                 "redoing a stroke brings the painted canvas back");
}

/* ============================================ 4. 逐层属性：共享像素缓冲时各算各的 */

COMP_TEST(layer_revision_is_per_layer_not_per_buffer) {
  Engine engine(test_engine_desc());
  Document document(&engine, kW, kH, 72.0f, "sRGB");
  const comp_document doc = as_document_handle(&document);

  const comp_layer_id shared = add_layer(doc, "shared", kW, kH, 30, 40, 50);
  const comp_layer_id holder = add_layer(doc, "holder", kW, kH, 60, 70, 80);

  /*
   * 让两层共享同一块像素缓冲。ABI 没有「复制图层」入口，但引擎内部（合并子集、
   * 快照）确实能让两个 Layer 指向同一个 ImageBuffer；COW 保证写入时自动分离。
   * 这里直接构造这种局面，检验缓存的键必须是 (图层, 修订号) 而不是缓冲地址。
   */
  document.find(holder)->pixels = document.find(shared)->pixels;
  COMP_CHECK_MSG(document.find(holder)->pixels.pixel_data().data() ==
                     document.find(shared)->pixels.pixel_data().data(),
                 "the two layers must actually share one buffer for this test to mean anything");

  const uint64_t shared_before = revision_of(document, shared);
  const uint64_t holder_before = revision_of(document, holder);
  const std::vector<uint8_t> holder_bytes = pixels_of(document, holder);
  COMP_CHECK_MSG(shared_before != holder_before, "sharing a buffer must not merge the revisions");

  std::vector<uint8_t> replacement = solid_pixels(kW, kH, 5, 250, 5);
  comp_image image = image_view(kW, kH, replacement);
  COMP_CHECK(comp_layer_set_pixels(doc, shared, &image) == COMP_OK);

  const uint64_t shared_after = revision_of(document, shared);
  COMP_CHECK_MSG(shared_after != shared_before, "the written layer gets a new revision");
  COMP_CHECK_MSG(revision_of(document, holder) == holder_before,
                 "the layer that merely shared the buffer keeps its revision");
  COMP_CHECK_MSG(pixels_of(document, holder) == holder_bytes,
                 "COW must leave the sharing layer's bytes untouched");
  COMP_CHECK_MSG(pixels_of(document, shared) == replacement, "the written layer's bytes land");
  COMP_CHECK_MSG(document.find(holder)->pixels.pixel_data().data() !=
                     document.find(shared)->pixels.pixel_data().data(),
                 "the write must have split the once-shared buffer");
}

/* ============================================ 5. 画布 / 工具 / 滤镜 / 选区都算内容变更 */

COMP_TEST(layer_revision_covers_canvas_tool_filter_and_selection_edits) {
  Engine engine(test_engine_desc());
  Document document(&engine, kW, kH, 72.0f, "sRGB");
  const comp_document doc = as_document_handle(&document);

  const comp_layer_id layer = add_layer(doc, "art", kW, kH, 120, 60, 30);
  const uint64_t base_revision = revision_of(document, layer);

  /* 画布重采样：每层像素都被重采样，全部换号。 */
  COMP_CHECK(comp_document_resize_image(doc, comp_size{16, 12}) == COMP_OK);
  const uint64_t resized_revision = revision_of(document, layer);
  COMP_CHECK_MSG(resized_revision > base_revision, "resize_image must renumber every layer");
  comp_size size{};
  COMP_CHECK(comp_document_size(doc, &size) == COMP_OK);
  COMP_CHECK_MSG(size.width == 16 && size.height == 12, "resize_image must land");

  /* 撤销：尺寸与修订号一起回到从前。 */
  COMP_CHECK(comp_document_undo(doc) == COMP_OK);
  COMP_CHECK(comp_document_size(doc, &size) == COMP_OK);
  COMP_CHECK_MSG(size.width == kW && size.height == kH, "undo restores the canvas size");
  COMP_CHECK_MSG(revision_of(document, layer) == base_revision,
                 "undo restores the pre-resize revision");

  /* 笔刷落笔：内容变更。 */
  comp_tool_op_desc stroke{};
  stroke.kind = COMP_TOOL_STROKE;
  float points[2] = {4.0f, 3.0f};
  stroke.stroke_points = points;
  stroke.stroke_point_count = 1;
  stroke.brush.color[0] = 255;
  stroke.brush.color[1] = 255;
  stroke.brush.color[2] = 0;
  stroke.brush.diameter = 5.0f;
  stroke.brush.pressure = 1.0f;
  stroke.brush.hardness = 1.0f;
  stroke.brush.flow = 1.0f;
  COMP_CHECK(comp_tool_apply(doc, layer, &stroke, nullptr, 0, nullptr) == COMP_OK);
  const uint64_t stroked_revision = revision_of(document, layer);
  COMP_CHECK_MSG(stroked_revision > base_revision, "a stroke must produce a new revision");

  /*
   * 落空的形状（完全在图层外）是**契约里的 noop**：不发号、不留撤销步。
   * 笔刷不走这条 noop 判定（笔迹落空仍记一步），所以这里用形状算子。
   */
  size_t undo_depth_before = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_depth_before, nullptr) == COMP_OK);
  comp_tool_op_desc missing{};
  missing.kind = COMP_TOOL_SHAPE;
  missing.shape.kind = COMP_SHAPE_RECT;
  missing.shape.x0 = 100.0;
  missing.shape.y0 = 100.0;
  missing.shape.x1 = 110.0;
  missing.shape.y1 = 110.0;
  missing.shape.style.fill_enabled = 1;
  missing.shape.style.fill_color[0] = 255;
  missing.shape.style.fill_color[1] = 255;
  missing.shape.style.fill_color[2] = 255;
  missing.shape.style.fill_color[3] = 255;
  missing.shape.style.opacity = 1.0f;
  COMP_CHECK(comp_tool_apply(doc, layer, &missing, nullptr, 0, nullptr) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, layer) == stroked_revision,
                 "a shape that misses the layer must not bump the revision");
  size_t undo_depth_after = 0;
  COMP_CHECK(comp_document_history_stats(doc, &undo_depth_after, nullptr) == COMP_OK);
  COMP_CHECK_MSG(undo_depth_after == undo_depth_before,
                 "a no-op shape must not add an undo step");

  /* 滤镜：恒等参数（零初始化 = amount 0）不改像素，不换号。 */
  comp_filter_desc identity{};
  identity.kind = COMP_FILTER_VIGNETTE;
  int32_t changed = -1;
  COMP_CHECK(comp_filter_apply(doc, layer, &identity, 1.0, &changed, nullptr, 0, nullptr) ==
             COMP_OK);
  COMP_CHECK_MSG(changed == 0, "an amount-0 filter is identity");
  COMP_CHECK_MSG(revision_of(document, layer) == stroked_revision,
                 "an identity filter must not bump the revision");

  /* 滤镜：真的改了像素，换号。 */
  comp_filter_desc vignette{};
  vignette.kind = COMP_FILTER_VIGNETTE;
  vignette.vignette.amount = 60.0;
  vignette.vignette.midpoint = 50.0;
  vignette.vignette.feather = 60.0;
  vignette.vignette.color[0] = 0.0;
  vignette.vignette.color[1] = 0.0;
  vignette.vignette.color[2] = 0.0;
  changed = -1;
  COMP_CHECK(comp_filter_apply(doc, layer, &vignette, 1.0, &changed, nullptr, 0, nullptr) ==
             COMP_OK);
  COMP_CHECK_MSG(changed == 1, "a non-identity filter must report a change");
  const uint64_t filtered_revision = revision_of(document, layer);
  COMP_CHECK_MSG(filtered_revision > stroked_revision, "a filter must produce a new revision");

  /* 选区搬移像素：搬了内容，换号。 */
  int32_t selection_changed = 0;
  COMP_CHECK(comp_selection_select_all(doc, &selection_changed) == COMP_OK);
  COMP_CHECK(selection_changed == 1);
  int32_t moved = -1;
  COMP_CHECK(comp_selection_move_pixels(doc, layer, 1, 0, 0, &moved) == COMP_OK);
  COMP_CHECK_MSG(moved == 1, "moving a full-canvas selection must move pixels");
  COMP_CHECK_MSG(revision_of(document, layer) > filtered_revision,
                 "moving pixels must produce a new revision");
}

/* ============================================ 6. 修订号与 GPU 计划重建对齐 */

COMP_TEST(layer_revision_feeds_gpu_plan_recompute) {
  Engine engine(test_engine_desc());
  Document document(&engine, kW, kH, 72.0f, "sRGB");
  const comp_document doc = as_document_handle(&document);

  const comp_layer_id content = add_layer(doc, "content", kW, kH, 200, 100, 50);
  const uint64_t native_revision = revision_of(document, content);

  /* 恒等几何 + 整幅 RGBA8 = 原生单元：可直接上传成一张纹理。 */
  GpuPlanStats stats = plan_stats(document);
  COMP_CHECK_MSG(stats.native_units == 1, "an identity full-canvas layer is a native unit");
  COMP_CHECK_MSG(stats.baked_units == 0, "nothing needs CPU baking yet");

  /* 裁到半幅：只能烘焙，单元类别变了 => 纹理也必须重做。 */
  COMP_CHECK(comp_layer_set_transform(doc, content, comp_rect{0, 0, kW / 2, kH}, 0.0f) == COMP_OK);
  const uint64_t cropped_revision = revision_of(document, content);
  COMP_CHECK_MSG(cropped_revision != native_revision, "a transform change must produce a new revision");
  stats = plan_stats(document);
  COMP_CHECK_MSG(stats.native_units == 0, "a cropped layer is no longer native");
  COMP_CHECK_MSG(stats.baked_units == 1, "a cropped layer must be baked");

  /* 改回整幅：类别回到原生，但修订号必须比第一次的原生号更大（缓存不能复用过期的原生纹理）。 */
  COMP_CHECK(comp_layer_set_transform(doc, content, comp_rect{0, 0, kW, kH}, 0.0f) == COMP_OK);
  const uint64_t restored_revision = revision_of(document, content);
  COMP_CHECK_MSG(restored_revision != cropped_revision,
                 "restoring the transform must produce a new revision");
  COMP_CHECK_MSG(restored_revision != native_revision,
                 "…and must not reuse the revision from before the detour");
  stats = plan_stats(document);
  COMP_CHECK_MSG(stats.native_units == 1, "the layer is native again");

  /* 组：穿透组只计数、不产生单元；改成隔离组就烘焙成一个单元。 */
  const comp_layer_id group = add_group(doc, "group");
  const comp_layer_id child = add_layer(doc, "child", kW, kH, 0, 200, 200, group);

  stats = plan_stats(document);
  COMP_CHECK_MSG(stats.pass_through_groups == 1, "a plain group is pass-through");
  COMP_CHECK_MSG(stats.isolated_groups == 0, "a plain group produces no baked unit");
  COMP_CHECK_MSG(stats.native_units == 2, "both pixel layers are native units");

  const uint64_t group_revision = revision_of(document, group);
  COMP_CHECK(comp_layer_set_isolated(doc, group, 1) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, group) > group_revision,
                 "toggling isolation must produce a new revision on the group");
  stats = plan_stats(document);
  COMP_CHECK_MSG(stats.isolated_groups == 1, "an isolated group is baked as one unit");
  COMP_CHECK_MSG(stats.pass_through_groups == 0, "…and is no longer pass-through");

  /* 隐藏子层：单元消失，且子层换号（可见性也算内容版本）。 */
  const uint64_t child_revision = revision_of(document, child);
  COMP_CHECK(comp_layer_set_visible(doc, child, 0) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, child) > child_revision,
                 "hiding a layer must produce a new revision");
  stats = plan_stats(document);
  COMP_CHECK_MSG(stats.native_units == 1, "a hidden layer contributes no unit");

  /* 隐藏整组：隔离组单元也消失。 */
  const uint64_t hidden_group_revision = revision_of(document, group);
  COMP_CHECK(comp_layer_set_visible(doc, group, 0) == COMP_OK);
  COMP_CHECK_MSG(revision_of(document, group) > hidden_group_revision,
                 "hiding the group must produce a new revision");
  stats = plan_stats(document);
  COMP_CHECK_MSG(stats.isolated_groups == 0, "a hidden group contributes no baked unit");
  COMP_CHECK_MSG(stats.native_units == 1, "only the unrelated top-level layer is left");
}
