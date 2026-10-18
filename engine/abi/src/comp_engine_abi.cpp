// AI生成
/*
 * comp_engine_abi.cpp — comp_engine.h 的 C ABI 实现（引擎与文档生命周期）。
 */
#include "compositor/comp_engine.h"
#include "compositor/comp_io.h"

#include <memory>
#include <new>

#include "comp_abi_internal.h"
#include "core/comp_geometry.h"
#include "core/comp_parallel.h"
#include "format/comp_file.h"
#include "format/comp_uuid.h"
#include "render/rhi.h"

using compositor::Document;
using compositor::DocumentState;
using compositor::Engine;
using compositor::Layer;

namespace {

/* 新建文档必须至少有一个可绘制图层；创建后保证其像素缓冲铺满画布且全透明。 */
Layer* ensure_background_layer(Document* document) {
  DocumentState& state = document->state();
  if (state.layers.empty()) {
    comp_layer_desc desc{};
    desc.name = "Background";
    desc.blend = COMP_BLEND_NORMAL;
    desc.opacity = 1.0f;
    desc.transform = comp_rect{0, 0, state.width, state.height};
    desc.sampling = COMP_SAMPLING_HIGH_QUALITY; /* 引擎默认采样，对齐上游缺省 */
    comp_layer_id id = 0;
    document->create_layer(desc, &id);
    state.active_layer = id;
  }
  if (state.active_layer == 0 && !state.layers.empty()) {
    state.active_layer = state.layers.front().id;
  }
  Layer* layer = document->find(state.active_layer);
  if (layer == nullptr) return nullptr;
  if (layer->pixels.width != state.width || layer->pixels.height != state.height ||
      layer->pixels.format != COMP_PIXEL_RGBA8) {
    layer->pixels.resize(state.width, state.height, COMP_PIXEL_RGBA8);
    layer->pixels.clear_transparent();
  }
  return layer;
}

/*
 * 分配器完整性：alloc 与 free 必须成对出现。
 * 只填 alloc 不填 free = 分配出的块永远无法归还（必然泄漏，且多半来自另一座
 * 堆）；只填 free 不填 alloc = 释放来源不明的指针。两者都是调用方错误，
 * 在引擎创建时一次性拒绝，好过把隐患带进运行期。realloc 允许为空——
 * 契约规定此时 reallocate 不保留旧内容（见 comp_core.h）。
 */
bool allocator_is_paired(const comp_allocator& alloc) {
  return (alloc.alloc == nullptr) == (alloc.free == nullptr);
}

}  // namespace

extern "C" {

/* ---------------------------------------------------------------- 引擎 */

comp_result comp_engine_create(const comp_engine_desc* desc, comp_engine* out_engine) {
  if (out_engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;

  comp_engine_desc resolved{};
  if (desc != nullptr) resolved = *desc;
  if (!allocator_is_paired(resolved.alloc)) {
    return COMP_ERR_INVALID_ARGUMENT;
  }

  Engine* engine = nullptr;
  try {
    engine = Engine::create(resolved);
  } catch (const std::bad_alloc&) {
    return COMP_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return COMP_ERR_INTERNAL;
  }
  if (engine == nullptr) return COMP_ERR_OUT_OF_MEMORY;

  /* 让 C 侧并行原语与引擎线程配置保持一致（M1 内核为顺序等价实现，此处为 M2 预置）。 */
  comp_parallel_set_threads(engine->threads());

  /* 后端选择：M1 除 CPU 外均回落 CPU，回落事实通过 last_error 暴露。 */
  std::string backend_error;
  std::unique_ptr<compositor::RenderBackend> backend =
      compositor::create_backend(engine, engine->backend(), &backend_error);
  if (backend == nullptr) {
    /* Engine::create 是 placement new + 引擎分配器：释放必须走配对的
     * Engine::destroy。全局 delete 在定制分配器场景下跨堆释放（报告 6.2
     * 同类错误的漏网处）。 */
    Engine::destroy(engine);
    return COMP_ERR_BACKEND;
  }
  if (!backend_error.empty()) engine->set_last_error(backend_error);

  *out_engine = reinterpret_cast<comp_engine>(engine);
  return COMP_OK;
}

void comp_engine_destroy(comp_engine engine) {
  /*
   * as_engine 已做 magic 校验：NULL、已销毁句柄（含重复 destroy）均为无操作。
   * ~Engine 会兜底销毁所有在册文档——宿主先销毁引擎的反序用法从此安全。
   */
  Engine::destroy(compositor::abi::as_engine(engine));
}

const char* comp_engine_last_error(comp_engine engine) {
  Engine* e = compositor::abi::as_engine(engine);
  if (e == nullptr) return "";
  return e->last_error().c_str();
}

comp_result comp_engine_backend(comp_engine engine, comp_backend* out_backend) {
  Engine* e = compositor::abi::as_engine(engine);
  if (e == nullptr || out_backend == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_backend = e->backend();
  return COMP_OK;
}

comp_result comp_engine_threads(comp_engine engine, uint32_t* out_threads) {
  Engine* e = compositor::abi::as_engine(engine);
  if (e == nullptr || out_threads == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  /*
   * 读进程级真值而不是 Engine 构造时的快照：并行度是全局单例
   * （comp_parallel_set_threads 写 g_threads），后建的引擎会改变前一个引擎
   * 的实际并行度。返回快照会让ABI 报告值与真实行为不一致。
   */
  *out_threads = static_cast<uint32_t>(::comp_parallel_threads());
  return COMP_OK;
}

/* ---------------------------------------------------------------- 文档 */

comp_result comp_document_create(comp_engine engine, const comp_document_desc* desc,
                                comp_document* out_document) {
  if (out_document == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  Engine* e = compositor::abi::as_engine(engine);
  COMP_ABI_TRY(e)
  if (e == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  if (desc == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_INVALID_ARGUMENT, "document desc is null");
  }
  const comp_size size{desc->width, desc->height};
  if (!compositor::is_valid_canvas(size)) {
    return compositor::abi::fail(e, COMP_ERR_INVALID_ARGUMENT, "invalid canvas size");
  }

  const float dpi = desc->dpi > 0.0f ? desc->dpi : 72.0f;
  const std::string color_space = desc->color_space != nullptr ? desc->color_space : "sRGB";

  Document* raw_document = nullptr;
  try {
    raw_document = Document::create(e, desc->width, desc->height, dpi, color_space);
  } catch (const std::bad_alloc&) {
    return compositor::abi::fail(e, COMP_ERR_OUT_OF_MEMORY, "cannot allocate document");
  }
  if (raw_document == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_OUT_OF_MEMORY, "cannot allocate document");
  }
  /*
   * attach 之前的所有权用 unique_ptr 看守：Document::create 出自引擎分配器的
   * placement new，unique_ptr 的删除器必须是配对的 Document::destroy。此前
   * uuid_v4_new()（字符串分配）与 ensure_background_layer()（背景层 resize）
   * 都在 try 之外，一旦抛 bad_alloc，异常直达 COMP_ABI_CATCH，文档既未 attach
   * 也未 destroy——定制分配器场景即泄漏。
   */
  std::unique_ptr<Document, void (*)(Document*)> document(raw_document,
                                                          Document::destroy);

  /* .comp 包的 documentID 必须非空（上游为必填 UUID）：文档一创建就分配，
   * 随文档持久保存，保证多次保存产出稳定的图层文件名。 */
  document->state().document_id = compositor::uuid_v4_new();

  if (ensure_background_layer(document.get()) == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_INTERNAL, "cannot create background layer");
  }

  document->mark_dirty(false);
  e->attach_document(document.get());
  *out_document = reinterpret_cast<comp_document>(document.release());
  return compositor::abi::ok(e);
  COMP_ABI_CATCH(e)
}

comp_result comp_document_load(comp_engine engine, const char* comp_path_utf8,
                              comp_document* out_document) {
  if (out_document == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  Engine* e = compositor::abi::as_engine(engine);
  COMP_ABI_TRY(e)
  if (e == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  if (comp_path_utf8 == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_INVALID_ARGUMENT, "comp path is null");
  }

  Document* loaded = nullptr;
  std::string error;
  if (!compositor::load_project(comp_path_utf8, e, &loaded, &error)) {
    return compositor::abi::fail(e, COMP_ERR_IO, error);
  }
  if (loaded == nullptr) {
    return compositor::abi::fail(e, COMP_ERR_IO, "load produced no document");
  }
  e->attach_document(loaded);
  *out_document = reinterpret_cast<comp_document>(loaded);
  return compositor::abi::ok(e);
  COMP_ABI_CATCH(e)
}

comp_result comp_document_save(comp_document document, const char* comp_path_utf8) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (comp_path_utf8 == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "comp path is null");
  }

  std::string error;
  if (!compositor::save_project(*doc, comp_path_utf8, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_IO, error);
  }
  doc->mark_dirty(false);
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

void comp_document_destroy(comp_document document) {
  /*
   * 经引擎分配器析构（与 Document::create 的 placement new 配对）。
   * as_document 已校验 magic：NULL 与已销毁句柄均为无操作（重复 destroy 安全）。
   * ~Document 会自行从引擎登记表摘掉自己。
   */
  Document::destroy(compositor::abi::as_document(document));
}

comp_result comp_document_size(comp_document document, comp_size* out_size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_size == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_size is null");
  }
  *out_size = comp_size{doc->state().width, doc->state().height};
  return compositor::abi::ok(engine);
}

comp_result comp_document_set_size(comp_document document, comp_size size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  if (!compositor::is_valid_canvas(size)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "invalid canvas size");
  }

  compositor::abi::ImplicitTxn txn(doc, "Canvas Size");
  DocumentState& state = doc->state();
  const bool resized = state.width != size.width || state.height != size.height;
  state.width = size.width;
  state.height = size.height;
  /* 换画布 = 换坐标系：选区蒙版按旧画布分辨率存的，留下就是错位的。
   * 上游 applyDocumentSize 重建 CanvasDocument，选区不随行（Crop 同口径，
   * 见 domain/crop.cpp）。尺寸没变时保留选区。 */
  if (resized) state.selection = compositor::SelectionState{};
  /*
   * M1 不做重采样：仅把超出画布的图层包围盒裁剪回画布范围。
   *
   * 相交为空时 intersection 返回 {x0, y0, 0, 0}，而 effective_bounds 对
   * w/h<=0 会回落到像素尺寸——直接写回会让完全落在新画布外的图层尺寸丝毫
   * 不变，与头文件「缩小时超出部分被裁剪」的契约相反。这里改为显式置空，
   * effective_bounds 因此不会把它放大回来。
   */
  for (Layer& layer : state.layers) {
    const comp_rect bounds = layer.effective_bounds();
    const comp_rect clipped = compositor::clamp_to_canvas(bounds, size);
    if (compositor::is_empty(clipped)) {
      layer.clipped_empty = true;
      layer.transform = comp_rect{clipped.x, clipped.y, 0, 0};
    } else {
      layer.clipped_empty = false;
      layer.transform = clipped;
    }
  }
  txn.commit();
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_document_resize_image(comp_document document, comp_size size) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  COMP_ABI_TRY(engine)
  std::string error;
  if (!doc->resize_image(size, &error)) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, error.c_str());
  }
  return compositor::abi::ok(engine);
  COMP_ABI_CATCH(engine)
}

comp_result comp_document_is_dirty(comp_document document, int32_t* out_dirty) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_dirty == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_dirty is null");
  }
  *out_dirty = doc->is_dirty() ? 1 : 0;
  return compositor::abi::ok(engine);
}

comp_result comp_document_engine(comp_document document, comp_engine* out_engine) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_engine == nullptr) return COMP_ERR_INVALID_ARGUMENT;
  *out_engine = reinterpret_cast<comp_engine>(engine);
  return COMP_OK;
}

} /* extern "C" */
