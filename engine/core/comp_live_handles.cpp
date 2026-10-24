// AI生成
#include "comp_live_handles.h"

#include <mutex>
#include <unordered_map>

namespace compositor {
namespace {

/* 函数内 static：避开全局构造顺序问题，首次调用时初始化。 */
std::mutex& live_handles_mutex() {
  static std::mutex mutex;
  return mutex;
}

/* 指针 → 类型标签。键是对象地址，值是它是哪一类句柄。 */
std::unordered_map<const void*, LiveHandleKind>& live_handles() {
  static std::unordered_map<const void*, LiveHandleKind> handles;
  return handles;
}

}  // namespace

void register_live_handle(const void* object, LiveHandleKind kind) noexcept {
  if (object == nullptr) return;
  std::lock_guard<std::mutex> lock(live_handles_mutex());
  live_handles()[object] = kind;
}

void unregister_live_handle(const void* object, LiveHandleKind kind) noexcept {
  (void)kind; /* 注销按地址无条件删：留下悬空表项比删错更危险。 */
  if (object == nullptr) return;
  std::lock_guard<std::mutex> lock(live_handles_mutex());
  live_handles().erase(object);
}

bool is_live_handle(const void* object, LiveHandleKind kind) noexcept {
  if (object == nullptr) return false;
  std::lock_guard<std::mutex> lock(live_handles_mutex());
  const auto found = live_handles().find(object);
  return found != live_handles().end() && found->second == kind;
}

}  // namespace compositor
