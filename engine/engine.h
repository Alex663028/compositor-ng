// AI生成
/*
 * engine.h — 引擎实例（内部 C++ 类型）。
 *
 * 与 ABI 的关系：comp_engine 不透明句柄在 abi 层被 reinterpret_cast 为 Engine*。
 * Engine 持有跨文档共享的资源：分配器、渲染后端选择、工作线程数、最近错误。
 */
#ifndef COMPOSITOR_ENGINE_ENGINE_H
#define COMPOSITOR_ENGINE_ENGINE_H

#include <cstdint>
#include <string>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "core/comp_alloc.h"

namespace compositor {

class Engine {
 public:
  explicit Engine(const comp_engine_desc& desc);

  const Allocator& allocator() const noexcept { return allocator_; }
  comp_backend backend() const noexcept { return backend_; }
  uint32_t threads() const noexcept { return threads_; }
  const std::string& app_name() const noexcept { return app_name_; }
  const std::string& app_version() const noexcept { return app_version_; }

  void set_last_error(std::string message) { last_error_ = std::move(message); }
  const std::string& last_error() const noexcept { return last_error_; }

  /* 便捷：设置错误并返回错误码。 */
  comp_result fail(comp_result code, std::string message) {
    set_last_error(std::move(message));
    return code;
  }

  void clear_error() { last_error_.clear(); }

 private:
  Allocator allocator_{};
  comp_backend backend_ = COMP_BACKEND_CPU;
  uint32_t threads_ = 1;
  std::string app_name_ = "compositor";
  std::string app_version_;
  std::string last_error_;
};

}  // namespace compositor

#endif /* COMPOSITOR_ENGINE_ENGINE_H */
