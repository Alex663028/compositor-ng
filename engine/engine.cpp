// AI生成
#include "engine.h"

#include <new>

#include "core/comp_parallel.h"
#include "domain/camera_raw_tables.h"
#include "domain/document.h"
#include "render/rhi.h"

namespace compositor {

bool Engine::camera_raw_tables_loaded() const noexcept {
  return camera_raw_tables_ != nullptr && camera_raw_tables_->valid();
}

namespace {

/*
 * AUTO 的后端偏好：Windows -> D3D12，Apple -> Metal，其余 -> Vulkan（OHOS/Linux）。
 * 这里只表达「偏好」，真正能否创建由 create_backend 探测；不可用时它自己回落
 * CPU 并把原因写进 last_error——所以 AUTO 永远能拿到一个能渲染的后端。
 */
comp_backend resolve_backend(comp_backend requested) {
  if (requested != COMP_BACKEND_AUTO) return requested;
#if defined(_WIN32)
  return COMP_BACKEND_D3D12;
#elif defined(__APPLE__)
  return COMP_BACKEND_METAL;
#else
  return COMP_BACKEND_VULKAN;
#endif
}

}  // namespace

Engine::Engine(const comp_engine_desc& desc) {
  allocator_.set(desc.alloc);
  if (desc.app_name != nullptr && desc.app_name[0] != '\0') {
    app_name_ = desc.app_name;
  }
  if (desc.version != nullptr) {
    app_version_ = desc.version;
  }
  requested_backend_ = desc.backend;
  backend_ = resolve_backend(desc.backend);

  const size_t effective = comp_parallel_set_threads(desc.worker_threads);
  threads_ = static_cast<uint32_t>(effective);
}

void Engine::adopt_render_backend(std::unique_ptr<RenderBackend> backend, comp_backend actual) {
  render_backend_ = std::move(backend);
  backend_ = actual;
}

Engine::~Engine() {
  /*
   * 兜底清理：宿主若先销毁引擎（反序），在册文档一并销毁。
   * 先 swap 摘掉列表再逐个 delete——Document 析构会回调 detach_document，
   * 迭代中修改容器是未定义行为。poison 放在最后：清理期间引擎对自身
   * 析构路径仍然「活着」。
   */
  std::vector<Document*> pending;
  pending.swap(documents_);
  for (Document* document : pending) {
    /* 必须走 Document::destroy（placement new 的手动析构 + 分配器归还），
     * 默认 delete 会把定制分配器出来的块交给全局 operator delete。 */
    Document::destroy(document);
  }
  handle_magic_ = kHandlePoisoned;
}

void Engine::detach_document(Document* document) {
  for (auto it = documents_.begin(); it != documents_.end(); ++it) {
    if (*it == document) {
      documents_.erase(it);
      return;
    }
  }
}

Engine* Engine::create(const comp_engine_desc& desc) {
  Allocator alloc;
  alloc.set(desc.alloc);
  void* memory = alloc.allocate(sizeof(Engine));
  if (memory == nullptr) throw std::bad_alloc();
  try {
    return new (memory) Engine(desc);
  } catch (...) {
    alloc.release(memory);
    throw;
  }
}

void Engine::destroy(Engine* engine) {
  if (engine == nullptr) return;
  /* 先取出分配器再析构：~Engine 之后 allocator_ 成员本身已失效。 */
  const Allocator alloc = engine->allocator();
  engine->~Engine();
  alloc.release(engine);
}

}  // namespace compositor
