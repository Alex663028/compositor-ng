// AI生成
/*
 * comp_alloc.h — 引擎内部统一内存分配入口。
 *
 * 所有面向调用方可见的缓冲（图层像素、蒙版、导出缓冲）都必须经由此处分配，
 * 以保证「谁分配谁释放」在跨模块、跨平台场景下成立（Windows 上不同 CRT 不可互 free）。
 */
#ifndef COMPOSITOR_CORE_COMP_ALLOC_H
#define COMPOSITOR_CORE_COMP_ALLOC_H

#include <cstddef>

#include "compositor/comp_core.h"

namespace compositor {

class Allocator {
 public:
  Allocator() = default;
  explicit Allocator(const comp_allocator& desc) noexcept { set(desc); }

  void set(const comp_allocator& desc) noexcept {
    desc_ = desc;
    custom_ = desc_.alloc != nullptr;
  }

  bool is_custom() const noexcept { return custom_; }
  const comp_allocator& desc() const noexcept { return desc_; }

  void* allocate(std::size_t bytes) const;
  void* reallocate(void* p, std::size_t bytes) const;
  void release(void* p) const;

  /* 归零分配，失败返回 nullptr。 */
  void* allocate_zeroed(std::size_t bytes) const;

 private:
  comp_allocator desc_{};
  bool custom_ = false;
};

}  // namespace compositor

#endif /* COMPOSITOR_CORE_COMP_ALLOC_H */
