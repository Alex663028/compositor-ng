// AI生成
#include "engine.h"

#include "core/comp_parallel.h"

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

}  // namespace compositor
