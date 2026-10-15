// AI生成
#include "engine.h"

#include <new>

#include "core/comp_parallel.h"
#include "domain/document.h"

namespace compositor {

namespace {

/* M1：AUTO 解析为 CPU 后端。
 * M2 起按平台择优：Apple -> METAL，Windows -> D3D12，HarmonyOS -> VULKAN，
 * 探测失败时一律回落 CPU，保证“始终可用”。 */
comp_backend resolve_backend(comp_backend requested) {
  if (requested == COMP_BACKEND_AUTO) return COMP_BACKEND_CPU;
  return requested;
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
  backend_ = resolve_backend(desc.backend);

  const size_t effective = comp_parallel_set_threads(desc.worker_threads);
  threads_ = static_cast<uint32_t>(effective);
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
