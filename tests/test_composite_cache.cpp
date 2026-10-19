// AI生成
/*
 * test_composite_cache.cpp — 画布级合成缓存（MERGED，S12）。
 *
 * 缓存是**派生数据**：它只能改变「合成算几次」，绝不能改变「合成算出什么」。
 * 因此每条用例的硬断言都是同一个形状：
 *   缓存结果 == 现算结果（composite_document），且在应有的时点失效/刷新。
 *
 * 覆盖：
 *   1. 事务外连续渲染命中同一块画布缓冲（零合成、零像素拷贝），内容与现算逐字节一致；
 *   2. 图层结构变化 / 显式像素写入 / 撤销 / 重做都会让缓存失效并重算；
 *   3. 活跃事务期间绝不供缓存（事务内改像素必须立刻可见），提交后按新状态重算，
 *      回滚后回到事务前的画面；
 *   4. 拼合（Flatten）复用缓存且共享像素（不复制整幅画布）。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "compositing/compositor.h"
#include "domain/document.h"
#include "domain/layer.h"
#include "engine.h"
#include "test_framework.h"

using namespace compositor;

namespace {

comp_engine_desc test_engine_desc() {
  comp_engine_desc desc{};
  desc.app_name = "compositor-composite-cache-tests";
  desc.backend = COMP_BACKEND_CPU;
  desc.worker_threads = 2;
  return desc;
}

/* 追加一个铺满画布的实心像素层（后添加 = 堆叠序在上）。 */
comp_layer_id add_solid_layer(Document& doc, const char* name, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t a, float opacity = 1.0f,
                              comp_blend_mode blend = COMP_BLEND_NORMAL) {
  comp_layer_desc desc{};
  desc.name = name;
  desc.blend = blend;
  desc.opacity = opacity;
  desc.transform = comp_rect{0, 0, doc.state().width, doc.state().height};
  comp_layer_id id = 0;
  doc.create_layer(desc, &id);
  Layer* layer = doc.find(id);
  if (layer != nullptr) {
    layer->pixels =
        ImageBuffer::solid(doc.state().width, doc.state().height, COMP_PIXEL_RGBA8, r, g, b, a);
  }
  return id;
}

/* 现算一份整篇合成（不经缓存），作为逐字节比对的基准。 */
bool fresh_composite(const Document& doc, std::vector<uint8_t>* out, std::string* error) {
  ImageBuffer canvas;
  if (!composite_document(doc, &canvas, error)) return false;
  out->assign(canvas.pixel_data().begin(), canvas.pixel_data().end());
  return true;
}

const ImageBuffer* cached_composite(Document& doc, std::string* error) {
  const ImageBuffer* view = nullptr;
  if (!composite_document_cached(doc, &view, error)) return nullptr;
  return view;
}

/* 缓存结果必须与现算结果逐字节一致。 */
void check_matches_fresh(Document& doc, const ImageBuffer* view, const char* what) {
  COMP_CHECK_MSG(view != nullptr, what);
  if (view == nullptr) return;
  std::vector<uint8_t> fresh;
  std::string error;
  COMP_CHECK_MSG(fresh_composite(doc, &fresh, &error), error.c_str());
  COMP_CHECK_MSG(fresh.size() == view->pixel_data().size(), what);
  if (fresh.size() != view->pixel_data().size()) return;
  COMP_CHECK_MSG(std::memcmp(fresh.data(), view->pixel_data().data(), fresh.size()) == 0, what);
}

}  // namespace

COMP_TEST(composite_cache_hits_reuse_the_canvas_buffer) {
  Engine engine(test_engine_desc());
  Document document(&engine, 8, 6, 72.0f, "sRGB");
  add_solid_layer(document, "Back", 20, 40, 60, 255);
  add_solid_layer(document, "Front", 200, 10, 10, 128);

  std::string error;
  const ImageBuffer* first = cached_composite(document, &error);
  COMP_CHECK_MSG(first != nullptr, error.c_str());
  if (first == nullptr) return;
  check_matches_fresh(document, first, "cached composite must equal a fresh composite");

  const std::vector<uint8_t> snapshot = first->pixel_data();
  const uint8_t* const pixels = first->pixel_data().data();
  const uint64_t serial = document.content_serial();

  /* 没有任何写入：第二次必须是同一块缓冲（命中，不重算、不复制）。 */
  const ImageBuffer* second = cached_composite(document, &error);
  COMP_CHECK_MSG(second != nullptr, error.c_str());
  if (second == nullptr) return;
  COMP_CHECK_MSG(second == first, "an unchanged document must serve the same cached buffer");
  COMP_CHECK_MSG(second->pixel_data().data() == pixels, "a hit must not reallocate the canvas");
  COMP_CHECK_MSG(document.content_serial() == serial, "a hit must not bump the content serial");
  COMP_CHECK(document.composite_cache_valid());
  COMP_CHECK(document.composite_cache_serial() == document.content_serial());
  COMP_CHECK_MSG(second->pixel_data() == snapshot, "a hit must return identical pixels");

  /* 尺寸与格式也跟画布一致。 */
  COMP_CHECK(second->width == 8 && second->height == 6);
  COMP_CHECK(second->format == COMP_PIXEL_RGBA8);
}

COMP_TEST(composite_cache_is_invalidated_by_layer_and_pixel_writes) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id base = add_solid_layer(document, "Base", 0, 0, 0, 255);

  std::string error;
  const ImageBuffer* before = cached_composite(document, &error);
  COMP_CHECK_MSG(before != nullptr, error.c_str());
  if (before == nullptr) return;
  const std::vector<uint8_t> before_pixels = before->pixel_data();
  const uint64_t before_serial = document.content_serial();

  /* 1) 图层结构变化（新建图层，单独一步事务）→ 缓存作废，重算后必须与现算一致。 */
  COMP_CHECK(document.txn_begin("Add Layer", &error) == COMP_OK);
  add_solid_layer(document, "Added", 255, 255, 255, 255);
  COMP_CHECK(document.txn_commit(&error) == COMP_OK);
  COMP_CHECK(document.content_serial() > before_serial);
  COMP_CHECK(!document.composite_cache_valid());
  const ImageBuffer* after_add = cached_composite(document, &error);
  COMP_CHECK_MSG(after_add != nullptr, error.c_str());
  check_matches_fresh(document, after_add, "cache must be recomputed after adding a layer");
  if (after_add == nullptr) return;
  const std::vector<uint8_t> after_add_pixels = after_add->pixel_data();
  COMP_CHECK_MSG(after_add_pixels != before_pixels,
                 "the recomputed canvas must reflect the added layer");

  /* 2) 显式像素写入（显式事务）→ 缓存再次作废。 */
  Layer* layer = document.find(base);
  COMP_CHECK(layer != nullptr);
  if (layer == nullptr) return;
  COMP_CHECK(document.txn_begin("Paint Base", &error) == COMP_OK);
  layer->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 255, 0, 255);
  COMP_CHECK(document.txn_commit(&error) == COMP_OK);
  const ImageBuffer* after_paint = cached_composite(document, &error);
  COMP_CHECK_MSG(after_paint != nullptr, error.c_str());
  check_matches_fresh(document, after_paint, "cache must be recomputed after a pixel write");
  if (after_paint != nullptr) {
    const uint8_t* px = after_paint->row(0);
    /* 上面那层是白色不透明，绿色被盖住——缓存必须看到「写入已生效」这条新状态。 */
    COMP_CHECK(px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255);
  }

  /* 3) 撤销 / 重做都会换掉整份状态 → 缓存必须跟着换。 */
  COMP_CHECK(document.undo()); /* 撤销像素写入 */
  const ImageBuffer* after_undo = cached_composite(document, &error);
  COMP_CHECK_MSG(after_undo != nullptr, error.c_str());
  check_matches_fresh(document, after_undo, "cache must be recomputed after undo");
  if (after_undo != nullptr) {
    COMP_CHECK_MSG(after_undo->pixel_data() == after_add_pixels,
                   "undoing the pixel write must restore the canvas from just before it");
  }

  COMP_CHECK(document.undo()); /* 撤销「新建图层」 */
  const ImageBuffer* after_second_undo = cached_composite(document, &error);
  COMP_CHECK_MSG(after_second_undo != nullptr, error.c_str());
  check_matches_fresh(document, after_second_undo, "cache must be recomputed after the second undo");
  if (after_second_undo != nullptr) {
    COMP_CHECK_MSG(after_second_undo->pixel_data() == before_pixels,
                   "undoing every edit must restore the original cached canvas");
  }

  COMP_CHECK(document.redo());
  const ImageBuffer* after_redo = cached_composite(document, &error);
  COMP_CHECK_MSG(after_redo != nullptr, error.c_str());
  check_matches_fresh(document, after_redo, "cache must be recomputed after redo");
  if (after_redo != nullptr) {
    COMP_CHECK_MSG(after_redo->pixel_data() == after_add_pixels,
                   "redo must bring back the state captured by the redo step");
  }
}

COMP_TEST(composite_cache_is_never_served_inside_a_transaction) {
  Engine engine(test_engine_desc());
  Document document(&engine, 4, 4, 72.0f, "sRGB");
  const comp_layer_id id = add_solid_layer(document, "Base", 10, 20, 30, 255);

  std::string error;
  const ImageBuffer* before = cached_composite(document, &error);
  COMP_CHECK_MSG(before != nullptr, error.c_str());
  if (before == nullptr) return;
  const std::vector<uint8_t> before_pixels = before->pixel_data();

  /* 事务内改像素：缓存里的旧画面不得被供出去——事务内渲染必须看到新像素。 */
  COMP_CHECK(document.txn_begin("Paint", &error) == COMP_OK);
  Layer* layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  if (layer == nullptr) return;
  layer->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 250, 0, 0, 255);
  COMP_CHECK(!document.composite_cache_valid());

  const ImageBuffer* in_txn = cached_composite(document, &error);
  COMP_CHECK_MSG(in_txn != nullptr, error.c_str());
  if (in_txn == nullptr) return;
  COMP_CHECK_MSG(in_txn->pixel_data() != before_pixels,
                 "a render inside a transaction must not serve the pre-transaction cache");
  const uint8_t* px = in_txn->row(0);
  COMP_CHECK_MSG(px[0] == 250 && px[1] == 0 && px[2] == 0 && px[3] == 255,
                 "a render inside a transaction must reflect the uncommitted pixels");
  COMP_CHECK_MSG(!document.composite_cache_valid(),
                 "an in-transaction evaluation must not become a cache entry");

  /* 事务内再渲染一次仍然照常求值（不能因为已经算过一次就当成缓存命中）。 */
  const ImageBuffer* in_txn_again = cached_composite(document, &error);
  COMP_CHECK_MSG(in_txn_again != nullptr, error.c_str());
  if (in_txn_again != nullptr) {
    COMP_CHECK_MSG(in_txn_again->pixel_data() == in_txn->pixel_data(),
                   "two in-transaction renders must agree");
  }

  /* 回滚：回到事务前的画面，并把缓存重新建立为可用状态。 */
  COMP_CHECK(document.txn_abort(&error) == COMP_OK);
  const ImageBuffer* after_abort = cached_composite(document, &error);
  COMP_CHECK_MSG(after_abort != nullptr, error.c_str());
  check_matches_fresh(document, after_abort, "cache after abort must equal a fresh composite");
  if (after_abort != nullptr) {
    COMP_CHECK_MSG(after_abort->pixel_data() == before_pixels,
                   "aborting the transaction must bring back the original canvas");
  }

  /* 提交：缓存按提交后的新状态重算。 */
  COMP_CHECK(document.txn_begin("Paint", &error) == COMP_OK);
  layer = document.find(id);
  COMP_CHECK(layer != nullptr);
  if (layer == nullptr) return;
  layer->pixels = ImageBuffer::solid(4, 4, COMP_PIXEL_RGBA8, 0, 0, 250, 255);
  COMP_CHECK(document.txn_commit(&error) == COMP_OK);
  const ImageBuffer* after_commit = cached_composite(document, &error);
  COMP_CHECK_MSG(after_commit != nullptr, error.c_str());
  check_matches_fresh(document, after_commit, "cache after commit must equal a fresh composite");
  if (after_commit != nullptr) {
    const uint8_t* committed = after_commit->row(0);
    COMP_CHECK_MSG(committed[0] == 0 && committed[1] == 0 && committed[2] == 250 &&
                       committed[3] == 255,
                   "a render after commit must show the committed pixels");
    COMP_CHECK(document.composite_cache_valid());
  }
}

COMP_TEST(flatten_image_reuses_the_cached_canvas_without_copying_pixels) {
  Engine engine(test_engine_desc());
  Document document(&engine, 6, 5, 72.0f, "sRGB");
  add_solid_layer(document, "Back", 10, 10, 10, 255);
  add_solid_layer(document, "Front", 0, 120, 240, 200);

  std::string error;
  const ImageBuffer* view = cached_composite(document, &error);
  COMP_CHECK_MSG(view != nullptr, error.c_str());
  if (view == nullptr) return;
  const std::vector<uint8_t> before_pixels = view->pixel_data();
  const uint8_t* const cached_pixels = view->pixel_data().data();

  COMP_CHECK_MSG(document.flatten_image(&error), error.c_str());

  /* 拼合不改变画面：拼合后的合成结果与拼合前逐字节一致。 */
  const ImageBuffer* after = cached_composite(document, &error);
  COMP_CHECK_MSG(after != nullptr, error.c_str());
  check_matches_fresh(document, after, "flatten must not change the rendered canvas");
  if (after != nullptr) {
    COMP_CHECK_MSG(after->pixel_data() == before_pixels,
                   "flatten must keep the canvas pixels byte-identical");
  }

  /* 拼合层直接接过缓存像素（shared_ptr 共享），不是整幅拷贝。 */
  COMP_CHECK(document.state().layers.size() == 1);
  if (!document.state().layers.empty()) {
    const ImageBuffer& flat = document.state().layers.front().pixels;
    COMP_CHECK_MSG(flat.pixel_data().data() == cached_pixels,
                   "flatten should adopt the cached pixels instead of copying them");
  }
  COMP_CHECK(document.state().width == 6 && document.state().height == 5);
}
