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

/* 新建文档必须至少有一个可绘制图层；创建后保证其像素缓冲铺满画布且全透明。
 *
 * P0-1：背景层的像素格式取文档的缺省位深（state.pixel_format），不再写死 RGBA8。
 * 判断条件里必须比格式——只比宽高会让「新建 16 位文档」拿到一个 8 位缓冲，
 * 而 state 里记的是 16 位，回读与实际不符。 */
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
  const comp_pixel_format want = compositor::abi::document_default_format(state);
  if (layer->pixels.width != state.width || layer->pixels.height != state.height ||
      layer->pixels.format != want) {
    layer->pixels.resize(state.width, state.height, want);
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

comp_result comp_engine_create2(const comp_engine_desc* desc, size_t desc_size,
                                comp_engine* out_engine) {
  /* OPT-8：desc_size 握手在前——结构体版本错配连解析都不该发生。 */
  if (desc_size != sizeof(comp_engine_desc)) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  return comp_engine_create(desc, out_engine);
}

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

  /*
   * 后端选择：按请求（AUTO 走平台偏好）创建**一个**实例并由引擎长期持有。
   * 渲染入口此后借用这个实例（GPU 设备/管线常驻），不再每次渲染重建；
   * 不可用时 create_backend 已回落 CPU，原因经 last_error 暴露。
   */
  std::string backend_error;
  comp_backend actual = engine->requested_backend();
  std::unique_ptr<compositor::RenderBackend> backend =
      compositor::create_backend(engine, engine->requested_backend(), &backend_error, &actual);
  if (backend == nullptr) {
    /* Engine::create 是 placement new + 引擎分配器：释放必须走配对的
     * Engine::destroy。全局 delete 在定制分配器场景下跨堆释放（报告 6.2
     * 同类错误的漏网处）。 */
    Engine::destroy(engine);
    return COMP_ERR_BACKEND;
  }
  engine->adopt_render_backend(std::move(backend), actual);
  if (!backend_error.empty()) engine->set_last_error(backend_error);

  *out_engine = reinterpret_cast<comp_engine>(engine);
  return COMP_OK;
}

void comp_engine_destroy(comp_engine engine) {
  /*
   * as_engine 先查存活登记表再校验 magic：NULL、已销毁句柄（含重复 destroy）、
   * 完全野的地址均为无操作。
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
  return comp_document_create_ex(engine, desc, nullptr, out_document);
}

/*
 * P0-1 选项解码。两种失败必须分开：
 *   - 结构本身不合法（struct_size 太小，说明宿主与引擎的头文件版本对不上）：
 *     INVALID_ARGUMENT，与其它参数错误同一口径；
 *   - 结构合法但值超出本版能力（CMYK / Lab / RGBA16F 新建）：UNSUPPORTED。
 * 混成一个码会让宿主分不清「我传错了」和「引擎还没做」，前者要改代码，
 * 后者要换功能。
 */
static comp_result decode_document_options(Engine* e, const comp_document_options* options,
                                          comp_pixel_format* out_format,
                                          comp_color_mode* out_mode) {
  *out_format = COMP_PIXEL_RGBA8;
  *out_mode = COMP_COLOR_MODE_RGB;
  if (options == nullptr) return COMP_OK;

  constexpr size_t kNeedFormat =
      offsetof(comp_document_options, pixel_format) + sizeof(int32_t);
  constexpr size_t kNeedMode = offsetof(comp_document_options, color_mode) + sizeof(int32_t);
  if (options->struct_size != 0 && options->struct_size < kNeedFormat) {
    return compositor::abi::fail(e, COMP_ERR_INVALID_ARGUMENT,
                                 "document options struct_size is too small");
  }
  int32_t format = static_cast<int32_t>(COMP_PIXEL_RGBA8);
  int32_t mode = static_cast<int32_t>(COMP_COLOR_MODE_RGB);
  if (options->struct_size == 0 || options->struct_size >= kNeedFormat) {
    format = options->pixel_format;
  }
  if (options->struct_size == 0 || options->struct_size >= kNeedMode) {
    mode = options->color_mode;
  }

  if (format != static_cast<int32_t>(COMP_PIXEL_RGBA8) &&
      format != static_cast<int32_t>(COMP_PIXEL_RGBA16)) {
    return compositor::abi::fail(
        e, COMP_ERR_UNSUPPORTED,
        "document pixel format: only RGBA8 and RGBA16 can be created (HDR documents are a "
        "separate path)");
  }
  if (mode != static_cast<int32_t>(COMP_COLOR_MODE_RGB) &&
      mode != static_cast<int32_t>(COMP_COLOR_MODE_GRAYSCALE)) {
    return compositor::abi::fail(e, COMP_ERR_UNSUPPORTED,
                                 "document color mode: only RGB and GRAYSCALE are supported "
                                 "(color management is out of scope)");
  }
  *out_format = static_cast<comp_pixel_format>(format);
  *out_mode = static_cast<comp_color_mode>(mode);
  return COMP_OK;
}

comp_result comp_document_create_ex(comp_engine engine, const comp_document_desc* desc,
                                   const comp_document_options* options,
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
  comp_pixel_format pixel_format = COMP_PIXEL_RGBA8;
  comp_color_mode color_mode = COMP_COLOR_MODE_RGB;
  const comp_result options_result =
      decode_document_options(e, options, &pixel_format, &color_mode);
  if (options_result != COMP_OK) return options_result;

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
  /* P0-1：位深与颜色模式要在**建背景层之前**落进状态——ensure_background_layer
   * 按 state.pixel_format 决定缓冲格式，晚一步就是「声明 16 位、实际 8 位」。 */
  document->state().pixel_format = static_cast<int32_t>(pixel_format);
  document->state().color_mode = static_cast<int32_t>(color_mode);

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
   * as_document 先查存活登记表再校验 magic：NULL、已销毁句柄（含重复
   * destroy）、完全野的地址均为无操作。
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

/*
 * P0-1 位深回读。
 *
 * 口径是「文档缺省位深」而不是「背景层的当前格式」：8 位工具作用在 16 位图层上
 * 会把那一层降成 8 位（见 tool_ops.cpp 的 use_rgba8_working_copy），如果这里跟着
 * 背景层走，画第一笔之后标题栏的「16 位」就会自己变成「8 位」——而用户选的文档
 * 位深并没有变。新建图层仍按文档位深创建，两者必须分开记。
 */
comp_result comp_document_pixel_format(comp_document document, comp_pixel_format* out_format) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_format == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_format is null");
  }
  *out_format = compositor::abi::document_default_format(doc->state());
  return compositor::abi::ok(engine);
}

comp_result comp_document_color_mode(comp_document document, comp_color_mode* out_mode) {
  Document* doc = nullptr;
  Engine* engine = nullptr;
  if (compositor::abi::guard_document(document, &doc, &engine) != COMP_OK) {
    return COMP_ERR_INVALID_ARGUMENT;
  }
  if (out_mode == nullptr) {
    return compositor::abi::fail(engine, COMP_ERR_INVALID_ARGUMENT, "out_mode is null");
  }
  const int32_t mode = doc->state().color_mode;
  *out_mode = mode == static_cast<int32_t>(COMP_COLOR_MODE_GRAYSCALE) ? COMP_COLOR_MODE_GRAYSCALE
                                                                     : COMP_COLOR_MODE_RGB;
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
  if (resized) {
    state.selection = compositor::SelectionState{};
    /* 通道蒙版同样是文档分辨率，随旧画布一起失效（见 domain_state.h）。 */
    state.channels.clear();
  }
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
  /* S18：尺寸变了 = 换坐标系（超界层的 transform 被裁），整篇换号；尺寸没变时
   * 上面只是原样写回，不换号。 */
  if (resized) doc->touch_all_layers();
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
