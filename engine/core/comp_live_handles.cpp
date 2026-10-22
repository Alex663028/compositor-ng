// AI生成
#include "comp_live_handles.h"

#include <mutex>
#include <unordered_set>

namespace compositor {
namespace {

/* 函数内 static：避开全局构造顺序问题，首次调用时初始化。 */
std::mutex& live_handles_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_set<const void*>& live_handles() {
  static std::unordered_set<const void*> handles;
  return handles;
}

}  // namespace

void register_live_handle(const void* object) noexcept {
  if (object == nullptr) return;
  std::lock_guard<std::mutex> lock(live_handles_mutex());
  live_handles().insert(object);
}

void unregister_live_handle(const void* object) noexcept {
  if (object == nullptr) return;
  std::lock_guard<std::mutex> lock(live_handles_mutex());
  live_handles().erase(object);
}

bool is_live_handle(const void* object) noexcept {
  if (object == nullptr) return false;
  std::lock_guard<std::mutex> lock(live_handles_mutex());
  return live_handles().count(object) > 0;
}

}  // namespace compositor
