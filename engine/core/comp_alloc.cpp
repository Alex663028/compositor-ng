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
    /* 调用方未提供 realloc：退化为 alloc + copy + free（容量未知，仅能拷贝旧块上限）。 */
    void* next = desc_.alloc(bytes, desc_.user);
    if (next != nullptr && p != nullptr) {
      std::memcpy(next, p, bytes);
      desc_.free(p, desc_.user);
    }
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
