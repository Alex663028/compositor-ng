// AI生成
#include "comp_alloc.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace compositor {

namespace {

void* default_alloc(std::size_t bytes, void*) { return std::malloc(bytes); }

void default_free(void* p, void*) { std::free(p); }

}  // namespace

void* Allocator::allocate(std::size_t bytes) const {
  if (bytes == 0) return nullptr;
  if (custom_) return desc_.alloc(bytes, desc_.user);
  return default_alloc(bytes, nullptr);
}

void* Allocator::reallocate(void* p, std::size_t bytes) const {
  if (custom_) {
    if (desc_.realloc != nullptr) return desc_.realloc(p, bytes, desc_.user);
    /*
     * 调用方未提供 realloc。此前退化为 alloc + memcpy(bytes) + free，
     * 但旧块的容量对引擎不可见——拷「新长度」会读越界（旧块通常更小）。
     * 缺 realloc 时无法安全搬运旧内容：只能分配新块并清零，旧内容丢弃。
     * 契约已写明这一点（comp_core.h：realloc 为空 ⇒ reallocate 不保留旧内容），
     * 且当前引擎没有任何 reallocate 调用点，这条路径只是防御。
     */
    void* next = desc_.alloc(bytes, desc_.user);
    if (next == nullptr) return nullptr;
    std::memset(next, 0, bytes);
    desc_.free(p, desc_.user);
    return next;
  }
  if (p == nullptr) return default_alloc(bytes, nullptr);
  return std::realloc(p, bytes);
}

void Allocator::release(void* p) const {
  if (p == nullptr) return;
  if (custom_) {
    desc_.free(p, desc_.user);
  } else {
    default_free(p, nullptr);
  }
}

void* Allocator::allocate_zeroed(std::size_t bytes) const {
  void* p = allocate(bytes);
  if (p != nullptr) std::memset(p, 0, bytes);
  return p;
}

}  // namespace compositor
