// AI生成
/*
 * engine.h — 引擎实例（内部 C++ 类型）。
 *
 * 与 ABI 的关系：comp_engine 不透明句柄在 abi 层被 reinterpret_cast 为 Engine*。
 * Engine 持有跨文档共享的资源：分配器、渲染后端选择、工作线程数、最近错误。
 *
 * 句柄有效性（轻量方案，不引入全局句柄表）：
 *   对象内置一枚构造期写入、析构期擦除的 magic。abi 层的 as_engine/as_document
 *   在 reinterpret_cast 之后核对它——销毁后使用、类型混淆（拿引擎句柄当文档
 *   句柄）、绝大多数野指针都会被挡成 COMP_ERR_INVALID_ARGUMENT，而不是静默
 *   解引用。代价是一次整数比较，ABI 类型与二进制布局均不变。
 */
#ifndef COMPOSITOR_ENGINE_ENGINE_H
#define COMPOSITOR_ENGINE_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "compositor/comp_core.h"
#include "compositor/comp_engine.h"
#include "core/comp_alloc.h"
#include "core/comp_handle_magic.h"

namespace compositor {

class Document;

class Engine {
 public:
  explicit Engine(const comp_engine_desc& desc);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /*
   * 经调用方分配器创建/销毁（placement new + 手动析构）。
   *
   * comp_engine_desc::alloc 此前「收下从不使用」——ABI 承诺了定制分配器，
   * 实现却走全局 operator new，跨 CRT 场景（Windows 上不同模块的堆）直接
   * 违反 comp_alloc.h 的「谁分配谁释放」。引擎与文档对象现在统一经此分配；
   * 分配失败抛 std::bad_alloc（与 new 语义一致，ABI 层已统一接住）。
   */
  static Engine* create(const comp_engine_desc& desc);
  static void destroy(Engine* engine);

  const Allocator& allocator() const noexcept { return allocator_; }
  comp_backend backend() const noexcept { return backend_; }
  uint32_t threads() const noexcept { return threads_; }
  const std::string& app_name() const noexcept { return app_name_; }
  const std::string& app_version() const noexcept { return app_version_; }

  /* 句柄有效性：见文件头说明。析构后恒为 false。 */
  bool is_alive() const noexcept { return handle_magic_ == kEngineMagic; }

  /*
   * 文档登记（顺序误用兜底）。
   *
   * ABI 约定宿主应先销毁文档再销毁引擎；现实中常见反序。Engine 记住自己
   * 创建的文档，析构时统一销毁，避免「引擎先走、文档变野指针」。
   */
  void attach_document(Document* document) { documents_.push_back(document); }
  void detach_document(Document* document);

  /*
   * 最近错误（线程局部）。
   *
   * 多个宿主线程可能共用同一引擎实例，进程级共享的 std::string 会被并发
   * 读写踩坏（数据竞争 = UB）。改为 thread_local：每个调用线程拥有独立的
   * 错误槽，错误码沿调用栈原路返回，同线程读 comp_engine_last_error 即可
   * 拿到本条错误——这与 C ABI 的同步调用模型一致，也无需加锁。
   */
  void set_last_error(std::string message) { error_slot() = std::move(message); }
  const std::string& last_error() const noexcept { return error_slot(); }

  /* 便捷：设置错误并返回错误码。 */
  comp_result fail(comp_result code, std::string message) {
    set_last_error(std::move(message));
    return code;
  }

  void clear_error() { error_slot().clear(); }

 private:
  static std::string& error_slot() {
    static thread_local std::string slot;
    return slot;
  }

  Allocator allocator_{};
  comp_backend backend_ = COMP_BACKEND_CPU;
  uint32_t threads_ = 1;
  std::string app_name_ = "compositor";
  std::string app_version_;
  uint32_t handle_magic_ = kEngineMagic;
  std::vector<Document*> documents_{};
};

}  // namespace compositor

#endif /* COMPOSITOR_ENGINE_ENGINE_H */
